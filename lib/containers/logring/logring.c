/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * logring — bounded circular log container (L2).
 *
 * A log-specialized structure: one i-node holds an inline, oldest->newest run
 * of variable-length records, each stamped with a monotonic sequence number.
 * Appending evicts records from the head until the newcomer fits, so a write
 * fails for space only when a single record exceeds the whole ring. Every
 * mutation is a single atomic blob_db_update(root, ...) — the rootreg pattern
 * (basic flow, no intent, no residue). Deliberately stateless: each op
 * re-reads the ring image from flash and validates it; nothing is cached.
 *
 * Contract: doc/layers/l2_containers.md §4.5; design: doc/impl/l2_logring.md.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/toolchain.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/logring.h>

LOG_MODULE_REGISTER(logring, CONFIG_CONTAINER_LOGRING_LOG_LEVEL);

/* On-flash format v1 — frozen. */
static const uint8_t CLOG_MAGIC[4] = { 'C', 'L', 'O', 'G' };
#define CLOG_VERSION 1

struct __packed clog_hdr {
	uint8_t  magic[4];
	uint8_t  version;
	uint8_t  rsvd;
	uint16_t count;     /* live records */
	uint16_t used;      /* bytes of data[] occupied by records */
	uint16_t rsvd2;
	uint64_t next_seq;  /* sequence number the next append will assign */
};
BUILD_ASSERT(sizeof(struct clog_hdr) == LOGRING_HDR_SIZE, "clog_hdr layout drift");

struct __packed clog_rec {
	uint64_t seq;
	uint16_t len;
	/* len bytes of payload follow */
};
BUILD_ASSERT(sizeof(struct clog_rec) == LOGRING_REC_OVERHEAD, "clog_rec layout drift");

/* A ring must be able to hold at least one 1-byte record. */
BUILD_ASSERT(CONFIG_BLOB_DB_MAX_PAYLOAD_LEN >=
	     LOGRING_HDR_SIZE + LOGRING_REC_OVERHEAD + 1,
	     "BLOB_DB_MAX_PAYLOAD_LEN too small for a logring record");

/* RAM image of the whole ring (one i-node payload). */
struct clog_image {
	struct clog_hdr hdr;
	uint8_t         data[LOGRING_CAPACITY];
};

/* Read + validate the ring image from @p root.
 *
 * Returns 0 with the parsed image; -ENOTSUP if the blob is not a v1 logring;
 * -ENOENT if the id holds no blob; or a blob_db error. -EIO signals a blob
 * that claims to be a logring but whose record run is internally inconsistent.
 */
static int clog_load(uint64_t root, struct clog_image *img)
{
	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t got;
	int rc = blob_db_get(root, buf, sizeof(buf), &got);

	if (rc < 0) {
		return rc;
	}
	if (got < sizeof(struct clog_hdr)) {
		return -ENOTSUP;   /* empty root or foreign payload */
	}

	memcpy(&img->hdr, buf, sizeof(img->hdr));
	if (memcmp(img->hdr.magic, CLOG_MAGIC, 4) != 0 ||
	    img->hdr.version != CLOG_VERSION) {
		return -ENOTSUP;
	}
	if (img->hdr.used > LOGRING_CAPACITY ||
	    got != sizeof(struct clog_hdr) + img->hdr.used) {
		LOG_ERR("ring image inconsistent (used=%u got=%zu)",
			img->hdr.used, got);
		return -EIO;
	}
	memcpy(img->data, buf + sizeof(struct clog_hdr), img->hdr.used);

	/* Walk the record run: it must partition [0, used) exactly into
	 * `count` well-formed records. Cheap O(count) integrity check that
	 * turns silent corruption into an honest -EIO. */
	size_t off = 0;
	uint16_t seen = 0;

	while (off < img->hdr.used) {
		struct clog_rec r;

		if (img->hdr.used - off < sizeof(struct clog_rec)) {
			goto corrupt;
		}
		memcpy(&r, img->data + off, sizeof(r));
		off += sizeof(struct clog_rec);
		if (r.len > img->hdr.used - off) {
			goto corrupt;
		}
		off += r.len;
		seen++;
	}
	if (off != img->hdr.used || seen != img->hdr.count) {
		goto corrupt;
	}
	return 0;

corrupt:
	LOG_ERR("ring record run corrupt (count=%u used=%u)",
		img->hdr.count, img->hdr.used);
	return -EIO;
}

/* Serialize + commit the image with ONE atomic update of the root. */
static int clog_store(uint64_t root, const struct clog_image *img)
{
	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	const size_t len = sizeof(struct clog_hdr) + img->hdr.used;

	memcpy(buf, &img->hdr, sizeof(img->hdr));
	memcpy(buf + sizeof(struct clog_hdr), img->data, img->hdr.used);

	return blob_db_update(root, buf, len);
}

int logring_create(uint64_t *out_root)
{
	if (!out_root) {
		return -EINVAL;
	}

	uint64_t id = blob_db_alloc_id();

	if (id == 0) {
		return -EIO;   /* not mounted, or id ceiling not persistable */
	}

	struct clog_image img = {
		.hdr = { .version = CLOG_VERSION, .count = 0, .used = 0,
			 .next_seq = 1 },
	};

	memcpy(img.hdr.magic, CLOG_MAGIC, 4);

	int rc = clog_store(id, &img);   /* one atomic bind */

	if (rc < 0) {
		return rc;
	}
	*out_root = id;
	LOG_DBG("created ring root=%llu", (unsigned long long)id);
	return 0;
}

int logring_open(uint64_t root)
{
	if (root == 0) {
		return -EINVAL;
	}

	struct clog_image img;

	return clog_load(root, &img);
}

int logring_append(uint64_t root, const void *data, size_t len,
		   uint64_t *out_seq)
{
	if (root == 0 || (!data && len > 0)) {
		return -EINVAL;
	}
	if (len > LOGRING_MAX_RECORD) {
		return -ENOSPC;   /* no eviction can ever make room */
	}

	struct clog_image img;
	int rc = clog_load(root, &img);

	if (rc < 0) {
		return rc;
	}

	const size_t need = sizeof(struct clog_rec) + len;

	/* Evict whole records from the head until the newcomer fits. Bounded:
	 * `need` <= LOGRING_CAPACITY (checked above), so the loop terminates at
	 * or before count == 0. */
	while ((size_t)img.hdr.used + need > LOGRING_CAPACITY &&
	       img.hdr.count > 0) {
		struct clog_rec front;

		memcpy(&front, img.data, sizeof(front));

		const size_t fsz = sizeof(struct clog_rec) + front.len;

		memmove(img.data, img.data + fsz, img.hdr.used - fsz);
		img.hdr.used -= (uint16_t)fsz;
		img.hdr.count--;
	}

	const uint64_t seq = img.hdr.next_seq;
	struct clog_rec nr = { .seq = seq, .len = (uint16_t)len };

	memcpy(img.data + img.hdr.used, &nr, sizeof(nr));
	if (len > 0) {
		memcpy(img.data + img.hdr.used + sizeof(nr), data, len);
	}
	img.hdr.used += (uint16_t)need;
	img.hdr.count++;
	img.hdr.next_seq++;

	rc = clog_store(root, &img);   /* COMMIT */
	if (rc < 0) {
		return rc;
	}
	if (out_seq) {
		*out_seq = seq;
	}
	return 0;
}

int logring_count(uint64_t root, size_t *out_count)
{
	if (root == 0 || !out_count) {
		return -EINVAL;
	}

	struct clog_image img;
	int rc = clog_load(root, &img);

	if (rc < 0) {
		return rc;
	}
	*out_count = img.hdr.count;
	return 0;
}

int logring_oldest_seq(uint64_t root, uint64_t *out_seq)
{
	if (root == 0 || !out_seq) {
		return -EINVAL;
	}

	struct clog_image img;
	int rc = clog_load(root, &img);

	if (rc < 0) {
		return rc;
	}
	if (img.hdr.count == 0) {
		return -ENOENT;
	}

	struct clog_rec front;

	memcpy(&front, img.data, sizeof(front));
	*out_seq = front.seq;
	return 0;
}

int logring_newest_seq(uint64_t root, uint64_t *out_seq)
{
	if (root == 0 || !out_seq) {
		return -EINVAL;
	}

	struct clog_image img;
	int rc = clog_load(root, &img);

	if (rc < 0) {
		return rc;
	}
	if (img.hdr.count == 0) {
		return -ENOENT;
	}
	/* The tail is always the most-recently appended record (eviction only
	 * touches the head), so its seq is next_seq - 1. */
	*out_seq = img.hdr.next_seq - 1;
	return 0;
}

int logring_reset(uint64_t root)
{
	if (root == 0) {
		return -EINVAL;
	}

	struct clog_image img;
	int rc = clog_load(root, &img);

	if (rc < 0) {
		return rc;
	}
	/* Keep next_seq: a reset is, to a seq-tracking consumer, the same as
	 * evicting everything. */
	img.hdr.count = 0;
	img.hdr.used = 0;
	return clog_store(root, &img);
}

int logring_iterate(uint64_t root, logring_iter_cb_t cb, void *user)
{
	if (root == 0 || !cb) {
		return -EINVAL;
	}

	struct clog_image img;
	int rc = clog_load(root, &img);

	if (rc < 0) {
		return rc;
	}

	size_t off = 0;

	for (uint16_t i = 0; i < img.hdr.count; i++) {
		struct clog_rec r;

		memcpy(&r, img.data + off, sizeof(r));
		off += sizeof(struct clog_rec);

		rc = cb(r.seq, img.data + off, r.len, user);
		if (rc) {
			return rc;
		}
		off += r.len;
	}
	return 0;
}
