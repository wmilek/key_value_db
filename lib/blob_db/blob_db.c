/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db — stable-id blob storage on flash_area.
 *
 * Stage 3: mount, unmount, master sector + bucket scan.
 * Put/get/update/delete arrive in stage 4. Real compaction recovery in
 * stage 5.
 *
 * See doc/design.md for the on-flash format and algorithms.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

#include <app/lib/blob_db.h>

#include "blob_db_internal.h"

LOG_MODULE_REGISTER(blob_db, CONFIG_BLOB_DB_LOG_LEVEL);

#define BLOB_DB_PARTITION_ID  PARTITION_ID(storage_partition)

/* On-flash magic strings, 4 bytes each (no NUL terminator). */
static const uint8_t MASTER_MAGIC[4] = { 'B', 'D', 'M', 'S' };
static const uint8_t BUCKET_MAGIC[4] = { 'B', 'D', 'B', 'H' };

/* Single global instance. v1 is single-threaded; caller serializes. */
static struct {
	bool      mounted;
	const struct flash_area *fa;
	size_t    fa_size;
	size_t    peb_size;
	uint16_t  n_pebs;
	uint16_t  n_buckets;
	uint8_t   active_master;   /* 0 or 1 */
	uint32_t  master_gen;
	uint64_t  next_id;
} st;

static inline off_t peb_offset(uint16_t peb)
{
	return (off_t)peb * (off_t)st.peb_size;
}

/* CRC32-IEEE over the leading (sizeof - 4) bytes of a header struct;
 * the trailing 4 bytes hold the CRC. */
static uint32_t hdr_crc32(const void *buf, size_t total)
{
	return crc32_ieee(buf, total - sizeof(uint32_t));
}

/* Master sector helpers -------------------------------------------------- */

static int read_master(uint8_t slot, struct blob_db_master_hdr *out, bool *valid)
{
	*valid = false;

	int rc = flash_area_read(st.fa, peb_offset(slot), out, sizeof(*out));
	if (rc < 0) {
		LOG_ERR("master %u read failed: %d", slot, rc);
		return rc;
	}

	if (memcmp(out->magic, MASTER_MAGIC, 4) != 0) {
		return 0;
	}
	uint32_t want = hdr_crc32(out, sizeof(*out));
	if (out->hdr_crc32 != want) {
		LOG_WRN("master %u CRC mismatch (got 0x%08x want 0x%08x)",
			slot, out->hdr_crc32, want);
		return 0;
	}
	*valid = true;
	return 0;
}

static int write_master(uint8_t slot, uint32_t gen, uint8_t state,
			uint16_t compacting_bid, uint64_t next_id_hint)
{
	int rc = flash_area_erase(st.fa, peb_offset(slot), st.peb_size);
	if (rc < 0) {
		LOG_ERR("master %u erase failed: %d", slot, rc);
		return rc;
	}

	struct blob_db_master_hdr hdr = {
		.generation     = gen,
		.state          = state,
		.compacting_bid = compacting_bid,
		.reserved       = 0,
		.next_id_hint   = next_id_hint,
	};
	memcpy(hdr.magic, MASTER_MAGIC, 4);
	hdr.hdr_crc32 = hdr_crc32(&hdr, sizeof(hdr));

	rc = flash_area_write(st.fa, peb_offset(slot), &hdr, sizeof(hdr));
	if (rc < 0) {
		LOG_ERR("master %u write failed: %d", slot, rc);
		return rc;
	}
	return 0;
}

/* Bucket scan ----------------------------------------------------------- */

static int scan_bucket(uint16_t bid, uint64_t *max_id_inout)
{
	const off_t base = peb_offset(BLOB_DB_FIRST_BUCKET + bid);

	struct blob_db_bucket_hdr bhdr;
	int rc = flash_area_read(st.fa, base, &bhdr, sizeof(bhdr));
	if (rc < 0) {
		return rc;
	}

	if (memcmp(bhdr.magic, BUCKET_MAGIC, 4) != 0) {
		return 0; /* unformatted — no entries */
	}
	if (bhdr.hdr_crc32 != hdr_crc32(&bhdr, sizeof(bhdr))) {
		LOG_WRN("bucket %u header CRC bad; ignoring", bid);
		return 0;
	}
	if (bhdr.bucket_id != bid) {
		LOG_WRN("bucket %u header claims bid %u; ignoring", bid, bhdr.bucket_id);
		return 0;
	}

	off_t cursor = BLOB_DB_BUCKET_DATA_OFF;
	const size_t min_slot = sizeof(struct blob_db_slot_hdr) +
				 sizeof(uint64_t) + sizeof(uint16_t);

	while (cursor + min_slot <= st.peb_size) {
		struct blob_db_slot_hdr shdr;
		rc = flash_area_read(st.fa, base + cursor, &shdr, sizeof(shdr));
		if (rc < 0) {
			return rc;
		}

		/* End-of-log: erased, unsealed, or implausible lengths. */
		if (shdr.flags == 0xff) {
			break;
		}
		if (!(shdr.flags & BLOB_DB_SLOT_F_SEALED)) {
			break;
		}
		if (shdr.val_len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
			LOG_WRN("bucket %u: bad val_len %u at off 0x%lx; truncating",
				bid, shdr.val_len, (unsigned long)cursor);
			break;
		}
		const size_t slot_sz = sizeof(shdr) + sizeof(uint64_t) +
				       shdr.val_len + sizeof(uint16_t);
		if (cursor + slot_sz > st.peb_size) {
			break;
		}

		uint64_t id;
		rc = flash_area_read(st.fa, base + cursor + sizeof(shdr),
				     &id, sizeof(id));
		if (rc < 0) {
			return rc;
		}

		/* All ids (live or tombstoned) count toward the high-water mark:
		 * a tombstoned id was once assigned and must not be re-issued. */
		if (id > *max_id_inout) {
			*max_id_inout = id;
		}

		cursor += slot_sz;
	}

	return 0;
}

/* Mount / unmount ------------------------------------------------------- */

static int format_masters_fresh(void)
{
	int rc = write_master(BLOB_DB_MASTER_A_SECTOR, 1,
			      BLOB_DB_STATE_CLEAN, 0, 1);
	if (rc < 0) {
		return rc;
	}
	rc = flash_area_erase(st.fa, peb_offset(BLOB_DB_MASTER_B_SECTOR),
			      st.peb_size);
	if (rc < 0) {
		return rc;
	}
	st.active_master = BLOB_DB_MASTER_A_SECTOR;
	st.master_gen = 1;
	st.next_id = 1;
	return 0;
}

int blob_db_mount(void)
{
	if (st.mounted) {
		return -EALREADY;
	}

	int rc = flash_area_open(BLOB_DB_PARTITION_ID, &st.fa);
	if (rc < 0) {
		LOG_ERR("flash_area_open: %d", rc);
		return rc;
	}

	st.fa_size = st.fa->fa_size;

	/* Derive sector size: get the first sector, assume uniform. */
	struct flash_sector sectors[4];
	uint32_t scount = ARRAY_SIZE(sectors);

	rc = flash_area_sectors(st.fa, &scount, sectors);
	if (rc < 0 && rc != -ENOMEM) {
		LOG_ERR("flash_area_get_sectors: %d", rc);
		goto err_close;
	}
	if (scount == 0) {
		LOG_ERR("partition reports zero sectors");
		rc = -EIO;
		goto err_close;
	}
	st.peb_size = sectors[0].fs_size;
	for (uint32_t i = 1; i < scount; i++) {
		if (sectors[i].fs_size != st.peb_size) {
			LOG_ERR("non-uniform sectors not supported");
			rc = -ENOTSUP;
			goto err_close;
		}
	}

	if (st.fa_size % st.peb_size != 0) {
		LOG_ERR("partition size %zu not a multiple of sector %zu",
			st.fa_size, st.peb_size);
		rc = -EINVAL;
		goto err_close;
	}
	st.n_pebs = st.fa_size / st.peb_size;
	if (st.n_pebs < BLOB_DB_FIRST_BUCKET + 1) {
		LOG_ERR("partition too small: %u sectors (need ≥ %u)",
			st.n_pebs, BLOB_DB_FIRST_BUCKET + 1);
		rc = -EINVAL;
		goto err_close;
	}
	st.n_buckets = st.n_pebs - BLOB_DB_FIRST_BUCKET;
	LOG_INF("partition %zu B, %u sectors of %zu B, %u buckets",
		st.fa_size, st.n_pebs, st.peb_size, st.n_buckets);

	/* Pick the authoritative master. */
	struct blob_db_master_hdr m_a, m_b;
	bool va, vb;

	rc = read_master(BLOB_DB_MASTER_A_SECTOR, &m_a, &va);
	if (rc < 0) {
		goto err_close;
	}
	rc = read_master(BLOB_DB_MASTER_B_SECTOR, &m_b, &vb);
	if (rc < 0) {
		goto err_close;
	}

	if (!va && !vb) {
		LOG_INF("no valid master; formatting");
		rc = format_masters_fresh();
		if (rc < 0) {
			goto err_close;
		}
	} else {
		bool pick_a;

		if (va && !vb) {
			pick_a = true;
		} else if (!va && vb) {
			pick_a = false;
		} else {
			pick_a = m_a.generation >= m_b.generation;
		}
		const struct blob_db_master_hdr *m = pick_a ? &m_a : &m_b;

		st.active_master = pick_a ? BLOB_DB_MASTER_A_SECTOR
					  : BLOB_DB_MASTER_B_SECTOR;
		st.master_gen = m->generation;
		st.next_id = m->next_id_hint;
		LOG_INF("master %c gen=%u state=%u next_id_hint=%llu",
			pick_a ? 'A' : 'B',
			st.master_gen, m->state, st.next_id);

		if (m->state == BLOB_DB_STATE_COMPACTING) {
			LOG_ERR("mid-compaction recovery not yet implemented "
				"(stage 5); refusing mount");
			rc = -EIO;
			goto err_close;
		}
	}

	/* Scan every bucket to refine next_id. */
	uint64_t max_id = (st.next_id == 0) ? 0 : (st.next_id - 1);
	for (uint16_t bid = 0; bid < st.n_buckets; bid++) {
		rc = scan_bucket(bid, &max_id);
		if (rc < 0) {
			LOG_ERR("scan_bucket(%u): %d", bid, rc);
			goto err_close;
		}
	}
	if (max_id + 1 > st.next_id) {
		LOG_INF("bucket scan raised next_id %llu -> %llu",
			st.next_id, max_id + 1);
		st.next_id = max_id + 1;
	}

	st.mounted = true;
	LOG_INF("mount ok; next_id=%llu", st.next_id);
	return 0;

err_close:
	flash_area_close(st.fa);
	st.fa = NULL;
	return rc;
}

int blob_db_unmount(void)
{
	if (!st.mounted) {
		return 0;
	}
	flash_area_close(st.fa);
	st.fa = NULL;
	st.mounted = false;
	LOG_INF("unmounted");
	return 0;
}

/* Stage 4 — put / get / update / delete / exists ------------------------ */

/* Maximum sector size we support. Bucket buffers are sized to this on the
 * stack inside each op. */
#define BLOB_DB_SECTOR_BUF_MAX 4096

static inline size_t slot_size_for(uint16_t val_len)
{
	/* v1: no write-block padding (native_sim has W=1). */
	return BLOB_DB_SLOT_OVERHEAD + val_len;
}

static inline off_t bucket_offset(uint16_t bid)
{
	return peb_offset(BLOB_DB_FIRST_BUCKET + bid);
}

static inline uint16_t id_to_bucket(uint64_t id)
{
	return (uint16_t)(id % st.n_buckets);
}

/* CRC16-CCITT(0xFFFF) over slot_hdr + id + payload. */
static uint16_t slot_crc16(const struct blob_db_slot_hdr *hdr, uint64_t id,
			    const void *payload, uint16_t val_len)
{
	uint16_t c = crc16_ccitt(0xffff, (const uint8_t *)hdr, sizeof(*hdr));

	c = crc16_ccitt(c, (const uint8_t *)&id, sizeof(id));
	if (val_len > 0) {
		c = crc16_ccitt(c, (const uint8_t *)payload, val_len);
	}
	return c;
}

static bool bucket_hdr_valid(const uint8_t *buf, uint16_t bid)
{
	const struct blob_db_bucket_hdr *bhdr =
		(const struct blob_db_bucket_hdr *)buf;

	if (memcmp(bhdr->magic, BUCKET_MAGIC, 4) != 0) {
		return false;
	}
	if (bhdr->hdr_crc32 != hdr_crc32(bhdr, sizeof(*bhdr))) {
		return false;
	}
	if (bhdr->bucket_id != bid) {
		return false;
	}
	return true;
}

struct bucket_walk {
	off_t    write_cursor;     /* end of valid slots in the bucket */
	off_t    target_slot_off;  /* -1 if target_id not present */
	uint8_t  target_flags;
	uint16_t target_val_len;
};

/* Walk a bucket buffer, identifying the write cursor and (latest) slot for
 * target_id. Slot CRC validation is performed inline; on CRC failure the
 * walk truncates the bucket at that offset. */
static void walk_bucket(const uint8_t *buf, uint64_t target_id,
			struct bucket_walk *r)
{
	r->target_slot_off = -1;
	r->target_flags = 0;
	r->target_val_len = 0;

	off_t cursor = BLOB_DB_BUCKET_DATA_OFF;
	const size_t min_slot = BLOB_DB_SLOT_OVERHEAD;

	while (cursor + min_slot <= st.peb_size) {
		const struct blob_db_slot_hdr *shdr =
			(const struct blob_db_slot_hdr *)(buf + cursor);

		if (shdr->flags == 0xff) {
			break;
		}
		if (!(shdr->flags & BLOB_DB_SLOT_F_SEALED)) {
			break;
		}
		if (shdr->val_len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
			break;
		}

		const size_t ssz = slot_size_for(shdr->val_len);

		if (cursor + ssz > st.peb_size) {
			break;
		}

		uint64_t slot_id;
		memcpy(&slot_id, buf + cursor + sizeof(*shdr), sizeof(slot_id));
		const uint8_t *payload = buf + cursor + sizeof(*shdr) +
					 sizeof(slot_id);
		uint16_t stored_crc;
		memcpy(&stored_crc, buf + cursor + ssz - sizeof(stored_crc),
		       sizeof(stored_crc));

		if (slot_crc16(shdr, slot_id, payload, shdr->val_len) !=
		    stored_crc) {
			break;
		}

		if (slot_id == target_id) {
			r->target_slot_off = cursor;
			r->target_flags = shdr->flags;
			r->target_val_len = shdr->val_len;
		}

		cursor += ssz;
	}

	r->write_cursor = cursor;
}

static int read_bucket(uint16_t bid, uint8_t *buf)
{
	return flash_area_read(st.fa, bucket_offset(bid), buf, st.peb_size);
}

static int format_bucket(uint16_t bid)
{
	int rc = flash_area_erase(st.fa, bucket_offset(bid), st.peb_size);

	if (rc < 0) {
		LOG_ERR("bucket %u erase: %d", bid, rc);
		return rc;
	}

	struct blob_db_bucket_hdr bhdr = {
		.bucket_id = bid,
		.reserved  = 0,
		.gen       = 1,
	};
	memcpy(bhdr.magic, BUCKET_MAGIC, 4);
	bhdr.hdr_crc32 = hdr_crc32(&bhdr, sizeof(bhdr));

	rc = flash_area_write(st.fa, bucket_offset(bid), &bhdr, sizeof(bhdr));
	if (rc < 0) {
		LOG_ERR("bucket %u header write: %d", bid, rc);
	}
	return rc;
}

/* Build and write a slot at bucket+write_cursor. */
static int append_slot(uint16_t bid, off_t write_cursor, uint64_t id,
		       uint8_t extra_flags, const void *payload,
		       uint16_t val_len)
{
	uint8_t buf[BLOB_DB_SLOT_OVERHEAD + CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];

	struct blob_db_slot_hdr *shdr = (struct blob_db_slot_hdr *)buf;

	shdr->flags    = BLOB_DB_SLOT_F_SEALED | extra_flags;
	shdr->reserved = 0;
	shdr->val_len  = val_len;

	memcpy(buf + sizeof(*shdr), &id, sizeof(id));
	if (val_len > 0) {
		memcpy(buf + sizeof(*shdr) + sizeof(id), payload, val_len);
	}

	uint16_t crc = slot_crc16(shdr, id, payload, val_len);
	const size_t crc_off = sizeof(*shdr) + sizeof(id) + val_len;

	memcpy(buf + crc_off, &crc, sizeof(crc));

	const size_t total = slot_size_for(val_len);

	return flash_area_write(st.fa, bucket_offset(bid) + write_cursor,
				buf, total);
}

int blob_db_put(const void *payload, size_t len, uint64_t *out_id)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (!out_id) {
		return -EINVAL;
	}
	if (len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
		return -EINVAL;
	}
	if (len > 0 && !payload) {
		return -EINVAL;
	}

	const uint64_t id = st.next_id;
	const uint16_t bid = id_to_bucket(id);
	uint8_t bbuf[BLOB_DB_SECTOR_BUF_MAX];

	int rc = read_bucket(bid, bbuf);
	if (rc < 0) {
		return rc;
	}

	off_t write_cursor;

	if (!bucket_hdr_valid(bbuf, bid)) {
		rc = format_bucket(bid);
		if (rc < 0) {
			return rc;
		}
		write_cursor = BLOB_DB_BUCKET_DATA_OFF;
	} else {
		struct bucket_walk w;
		walk_bucket(bbuf, 0 /* no target */, &w);
		write_cursor = w.write_cursor;
	}

	const size_t ssz = slot_size_for((uint16_t)len);

	if (write_cursor + ssz > st.peb_size) {
		LOG_WRN("bucket %u full (cursor 0x%lx + %zu > %zu); "
			"compaction not yet implemented (stage 5)",
			bid, (unsigned long)write_cursor, ssz, st.peb_size);
		return -ENOSPC;
	}

	rc = append_slot(bid, write_cursor, id, 0, payload, (uint16_t)len);
	if (rc < 0) {
		return rc;
	}

	st.next_id = id + 1;
	*out_id = id;
	LOG_DBG("put id=%llu bid=%u off=0x%lx len=%zu",
		(unsigned long long)id, bid,
		(unsigned long)write_cursor, len);
	return 0;
}

int blob_db_get(uint64_t id, void *out, size_t out_sz, size_t *out_len)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (out_sz > 0 && !out) {
		return -EINVAL;
	}
	if (id == 0) {
		return -ENOENT;
	}

	const uint16_t bid = id_to_bucket(id);
	uint8_t bbuf[BLOB_DB_SECTOR_BUF_MAX];

	int rc = read_bucket(bid, bbuf);
	if (rc < 0) {
		return rc;
	}

	if (!bucket_hdr_valid(bbuf, bid)) {
		return -ENOENT;
	}

	struct bucket_walk w;
	walk_bucket(bbuf, id, &w);

	if (w.target_slot_off < 0) {
		return -ENOENT;
	}
	if (w.target_flags & BLOB_DB_SLOT_F_TOMBSTONE) {
		return -ENOENT;
	}
	if (out_sz < w.target_val_len) {
		return -ENOMEM;
	}

	if (w.target_val_len > 0) {
		const off_t payload_off = w.target_slot_off +
					  sizeof(struct blob_db_slot_hdr) +
					  sizeof(uint64_t);
		memcpy(out, bbuf + payload_off, w.target_val_len);
	}
	if (out_len) {
		*out_len = w.target_val_len;
	}
	return 0;
}

int blob_db_update(uint64_t id, const void *payload, size_t len)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (id == 0) {
		return -ENOENT;
	}
	if (len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
		return -EINVAL;
	}
	if (len > 0 && !payload) {
		return -EINVAL;
	}

	const uint16_t bid = id_to_bucket(id);
	uint8_t bbuf[BLOB_DB_SECTOR_BUF_MAX];

	int rc = read_bucket(bid, bbuf);
	if (rc < 0) {
		return rc;
	}
	if (!bucket_hdr_valid(bbuf, bid)) {
		return -ENOENT;
	}

	struct bucket_walk w;
	walk_bucket(bbuf, id, &w);

	if (w.target_slot_off < 0) {
		return -ENOENT;
	}
	if (w.target_flags & BLOB_DB_SLOT_F_TOMBSTONE) {
		return -ENOENT;
	}

	const size_t ssz = slot_size_for((uint16_t)len);

	if (w.write_cursor + ssz > st.peb_size) {
		LOG_WRN("bucket %u full; compaction not yet implemented", bid);
		return -ENOSPC;
	}

	rc = append_slot(bid, w.write_cursor, id, 0, payload, (uint16_t)len);
	if (rc < 0) {
		return rc;
	}

	LOG_DBG("update id=%llu bid=%u off=0x%lx len=%zu",
		(unsigned long long)id, bid,
		(unsigned long)w.write_cursor, len);
	return 0;
}

int blob_db_delete(uint64_t id)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (id == 0) {
		return -ENOENT;
	}

	const uint16_t bid = id_to_bucket(id);
	uint8_t bbuf[BLOB_DB_SECTOR_BUF_MAX];

	int rc = read_bucket(bid, bbuf);
	if (rc < 0) {
		return rc;
	}
	if (!bucket_hdr_valid(bbuf, bid)) {
		return -ENOENT;
	}

	struct bucket_walk w;
	walk_bucket(bbuf, id, &w);

	if (w.target_slot_off < 0) {
		return -ENOENT;
	}
	if (w.target_flags & BLOB_DB_SLOT_F_TOMBSTONE) {
		return -ENOENT;
	}

	const size_t ssz = slot_size_for(0);

	if (w.write_cursor + ssz > st.peb_size) {
		LOG_WRN("bucket %u full; compaction not yet implemented", bid);
		return -ENOSPC;
	}

	rc = append_slot(bid, w.write_cursor, id,
			 BLOB_DB_SLOT_F_TOMBSTONE, NULL, 0);
	if (rc < 0) {
		return rc;
	}

	LOG_DBG("delete id=%llu bid=%u off=0x%lx",
		(unsigned long long)id, bid,
		(unsigned long)w.write_cursor);
	return 0;
}

bool blob_db_exists(uint64_t id)
{
	if (!st.mounted || id == 0) {
		return false;
	}

	const uint16_t bid = id_to_bucket(id);
	uint8_t bbuf[BLOB_DB_SECTOR_BUF_MAX];

	if (read_bucket(bid, bbuf) < 0) {
		return false;
	}
	if (!bucket_hdr_valid(bbuf, bid)) {
		return false;
	}

	struct bucket_walk w;
	walk_bucket(bbuf, id, &w);

	return w.target_slot_off >= 0 &&
	       !(w.target_flags & BLOB_DB_SLOT_F_TOMBSTONE);
}

/* Stage 6 stubs --------------------------------------------------------- */

size_t blob_db_count(void)
{
	return 0;
}

int blob_db_iterate(blob_db_iter_cb_t cb, void *user)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(user);
	return -ENOSYS;
}

int blob_db_format(void)
{
	return -ENOSYS;
}
