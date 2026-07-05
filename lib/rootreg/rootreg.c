/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * rootreg — root registry (L1.5): key -> structure-root map, owner of id = 1.
 *
 * The productionized model container in its simplest form: one pair list in
 * a single i-node, every mutation a single atomic blob_db_update(1, ...) —
 * basic flow only, no intent, no recovery machinery, no residue possible.
 * Deliberately stateless: every operation re-reads the registry image from
 * flash (one read) and validates it; nothing is cached.
 *
 * Contract: doc/layers/l1_root_registry.md.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/toolchain.h>

#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>

LOG_MODULE_REGISTER(rootreg, CONFIG_ROOTREG_LOG_LEVEL);

/* On-flash format v1 (l1_root_registry.md §4) — frozen. */
static const uint8_t RR_MAGIC[4] = { 'R', 'R', 'E', 'G' };
#define RR_VERSION  1

struct __packed rr_hdr {
	uint8_t  magic[4];
	uint16_t version;
	uint16_t count;
};
BUILD_ASSERT(sizeof(struct rr_hdr) == 8, "rr_hdr layout drift");

struct __packed rr_entry {
	uint64_t key;
	uint64_t root_id;
};
BUILD_ASSERT(sizeof(struct rr_entry) == 16, "rr_entry layout drift");

#define RR_IMAGE_MAX (sizeof(struct rr_hdr) + \
		      CONFIG_ROOTREG_MAX_ROOTS * sizeof(struct rr_entry))
BUILD_ASSERT(RR_IMAGE_MAX <= CONFIG_BLOB_DB_MAX_PAYLOAD_LEN,
	     "ROOTREG_MAX_ROOTS image exceeds BLOB_DB_MAX_PAYLOAD_LEN");

/* RAM image of the registry. */
struct rr_image {
	struct rr_hdr   hdr;
	struct rr_entry e[CONFIG_ROOTREG_MAX_ROOTS];
};

/* Read + validate the registry image from id = 1.
 *
 * Returns 0 with the parsed image, -ENOTSUP if id = 1 does not hold a valid
 * v1 registry (empty root = never bootstrapped; foreign bytes = some other
 * owner of id 1 — either way rootreg must not touch it), or a blob_db error.
 */
static int rr_load(struct rr_image *img)
{
	uint8_t buf[RR_IMAGE_MAX];
	size_t got;
	int rc = blob_db_get(BLOB_DB_ROOT_ID, buf, sizeof(buf), &got);

	if (rc < 0) {
		return rc;
	}
	if (got < sizeof(struct rr_hdr)) {
		return -ENOTSUP;   /* empty (not bootstrapped) or truncated */
	}

	memcpy(&img->hdr, buf, sizeof(img->hdr));
	if (memcmp(img->hdr.magic, RR_MAGIC, 4) != 0 ||
	    img->hdr.version != RR_VERSION) {
		return -ENOTSUP;   /* foreign format — refuse to touch */
	}
	if (img->hdr.count > CONFIG_ROOTREG_MAX_ROOTS ||
	    got != sizeof(struct rr_hdr) +
		   img->hdr.count * sizeof(struct rr_entry)) {
		LOG_ERR("registry image inconsistent (count=%u got=%zu)",
			img->hdr.count, got);
		return -EIO;
	}
	memcpy(img->e, buf + sizeof(struct rr_hdr),
	       img->hdr.count * sizeof(struct rr_entry));
	return 0;
}

/* Serialize + commit the image with ONE atomic update of id = 1. */
static int rr_store(const struct rr_image *img)
{
	uint8_t buf[RR_IMAGE_MAX];
	const size_t len = sizeof(struct rr_hdr) +
			   img->hdr.count * sizeof(struct rr_entry);

	memcpy(buf, &img->hdr, sizeof(img->hdr));
	memcpy(buf + sizeof(struct rr_hdr), img->e,
	       img->hdr.count * sizeof(struct rr_entry));

	return blob_db_update(BLOB_DB_ROOT_ID, buf, len);
}

static int rr_find(const struct rr_image *img, uint64_t key)
{
	for (uint16_t i = 0; i < img->hdr.count; i++) {
		if (img->e[i].key == key) {
			return i;
		}
	}
	return -1;
}

int rootreg_init(void)
{
	struct rr_image img;
	int rc = rr_load(&img);

	if (rc == 0) {
		LOG_DBG("registry ready (%u roots)", img.hdr.count);
		return 0;
	}
	if (rc != -ENOTSUP) {
		return rc;
	}

	/* -ENOTSUP from rr_load covers two cases that must diverge here:
	 * an EMPTY root payload is the virgin signal (root always exists
	 * after format, bound empty — bootstrap now); any non-empty payload
	 * that fails validation belongs to a foreign owner of id 1 and must
	 * not be overwritten. */
	uint8_t probe[sizeof(struct rr_hdr)];
	size_t got;

	rc = blob_db_get(BLOB_DB_ROOT_ID, probe, sizeof(probe), &got);
	if (rc == -ENOMEM || (rc == 0 && got > 0)) {
		LOG_ERR("id 1 holds a foreign payload; refusing to bootstrap");
		return -ENOTSUP;
	}
	if (rc < 0) {
		return rc;
	}

	struct rr_image fresh = {
		.hdr = { .version = RR_VERSION, .count = 0 },
	};

	memcpy(fresh.hdr.magic, RR_MAGIC, 4);
	rc = rr_store(&fresh);   /* one atomic bind — virgin bootstrap done */
	if (rc < 0) {
		return rc;
	}
	LOG_INF("virgin store — registry bootstrapped at id %u",
		(unsigned)BLOB_DB_ROOT_ID);
	return 0;
}

int rootreg_get(uint64_t key, uint64_t *root_id)
{
	if (key == 0 || !root_id) {
		return -EINVAL;
	}

	struct rr_image img;
	int rc = rr_load(&img);

	if (rc < 0) {
		return rc;
	}

	int idx = rr_find(&img, key);

	if (idx < 0) {
		return -ENOENT;
	}
	*root_id = img.e[idx].root_id;
	return 0;
}

int rootreg_get_or_create(uint64_t key, uint64_t *root_id)
{
	if (key == 0 || !root_id) {
		return -EINVAL;
	}

	struct rr_image img;
	int rc = rr_load(&img);

	if (rc < 0) {
		return rc;
	}

	int idx = rr_find(&img, key);

	if (idx >= 0) {
		/* The registered root may be allocated-but-unbound (creation
		 * was interrupted before the caller's bind) — still the same
		 * id, returned again; the caller lazily binds it (§7). */
		*root_id = img.e[idx].root_id;
		return 0;
	}
	if (img.hdr.count >= CONFIG_ROOTREG_MAX_ROOTS) {
		return -ENOSPC;
	}

	uint64_t id = blob_db_alloc_id();

	if (id == 0) {
		return -EIO;
	}

	img.e[img.hdr.count].key = key;
	img.e[img.hdr.count].root_id = id;
	img.hdr.count++;

	rc = rr_store(&img);   /* COMMIT — the entry is the creation intent */
	if (rc < 0) {
		return rc;
	}
	*root_id = id;
	LOG_DBG("created key=0x%016llx -> root=%llu",
		(unsigned long long)key, (unsigned long long)id);
	return 0;
}

int rootreg_set(uint64_t key, uint64_t root_id)
{
	if (key == 0 || root_id == 0) {
		return -EINVAL;
	}

	struct rr_image img;
	int rc = rr_load(&img);

	if (rc < 0) {
		return rc;
	}
	if (rr_find(&img, key) >= 0) {
		return -EEXIST;
	}
	if (img.hdr.count >= CONFIG_ROOTREG_MAX_ROOTS) {
		return -ENOSPC;
	}

	img.e[img.hdr.count].key = key;
	img.e[img.hdr.count].root_id = root_id;
	img.hdr.count++;

	return rr_store(&img);
}

int rootreg_unregister(uint64_t key)
{
	if (key == 0) {
		return -EINVAL;
	}

	struct rr_image img;
	int rc = rr_load(&img);

	if (rc < 0) {
		return rc;
	}

	int idx = rr_find(&img, key);

	if (idx < 0) {
		return -ENOENT;
	}

	/* Drop the 16-byte entry and nothing else — the structure behind it
	 * is untouched (§8). */
	img.e[idx] = img.e[img.hdr.count - 1];
	img.hdr.count--;

	return rr_store(&img);
}

int rootreg_iterate(rootreg_iter_cb_t cb, void *user)
{
	if (!cb) {
		return -EINVAL;
	}

	struct rr_image img;
	int rc = rr_load(&img);

	if (rc < 0) {
		return rc;
	}
	for (uint16_t i = 0; i < img.hdr.count; i++) {
		rc = cb(img.e[i].key, img.e[i].root_id, user);
		if (rc) {
			return rc;
		}
	}
	return 0;
}
