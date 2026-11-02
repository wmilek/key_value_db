/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Raw access to blob_db's major-5 master sectors, for forging on-flash states
 * (doc/proposals/2026-10-28-master-log.md §3).
 *
 * Deliberately a private copy of the layout rather than the library's own
 * header: if blob_db's master ever drifts, these tests must fail loudly
 * rather than silently follow it. Offsets are asserted for the same reason.
 *
 * Meaningful on the flash_area backend only, where blob_db's sectors sit at
 * these partition offsets (see SKIP_UNLESS_RAW_PARTITION in main.c).
 */

#ifndef BLOB_DB_TEST_MASTER_IMG_H_
#define BLOB_DB_TEST_MASTER_IMG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#define MI_PARTITION_ID  PARTITION_ID(storage_partition)
#define MI_SECTOR_SZ     4096   /* native_sim overlay: 4 KB sectors */
#define MI_N_BUCKETS     2045   /* 2048 sectors - 2 masters - scratch */
#define MI_LOG_OFF       128
#define MI_MAJOR         5
#define MI_HDR_LEN       64
#define MI_ID_STEP       256    /* BLOB_DB_ID_HINT_STEP */

struct __packed mhdr_img {
	uint8_t  magic[4];
	uint8_t  format_major;
	uint8_t  format_minor;
	uint16_t hdr_len;
	uint16_t reserved;
	uint16_t prefix_crc16;
	uint32_t rot_gen;
	uint32_t peb_size;
	uint16_t n_buckets;
	uint16_t rec_size;
	uint32_t incompat;
	uint32_t ro_compat;
	uint32_t compat;
	uint8_t  suite;
	uint8_t  reserved0[3];
	uint8_t  store_uuid[16];
	uint16_t max_payload_len;
	uint16_t write_align;
	uint32_t hdr_crc32;
};
BUILD_ASSERT(sizeof(struct mhdr_img) == 64, "master header layout drift");
BUILD_ASSERT(offsetof(struct mhdr_img, prefix_crc16) == 10, "prefix drift");
BUILD_ASSERT(offsetof(struct mhdr_img, rot_gen) == 12, "header drift");
BUILD_ASSERT(offsetof(struct mhdr_img, incompat) == 24, "header drift");
BUILD_ASSERT(offsetof(struct mhdr_img, suite) == 36, "header drift");
BUILD_ASSERT(offsetof(struct mhdr_img, max_payload_len) == 56, "header drift");
BUILD_ASSERT(offsetof(struct mhdr_img, hdr_crc32) == 60, "header drift");

struct __packed mrec_img {
	uint32_t rec_no;
	uint32_t reserved0;
	uint64_t next_id_hint;
	uint64_t seq_hint;
	uint64_t seg_owner;
	uint32_t reserved1;
	uint32_t fuses;
	uint32_t reserved2;
	uint32_t crc32;
};
BUILD_ASSERT(sizeof(struct mrec_img) == 48, "master record layout drift");
BUILD_ASSERT(offsetof(struct mrec_img, next_id_hint) == 8, "record drift");
BUILD_ASSERT(offsetof(struct mrec_img, seg_owner) == 24, "record drift");
BUILD_ASSERT(offsetof(struct mrec_img, fuses) == 36, "record drift");
BUILD_ASSERT(offsetof(struct mrec_img, crc32) == 44, "record drift");

/* One whole master sector, staged for reading and patching. */
static uint8_t mi_sec[MI_SECTOR_SZ];

static inline size_t mi_align(void)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(MI_PARTITION_ID, &fa));

	const size_t a = flash_area_align(fa);

	flash_area_close(fa);
	return a ? a : 1;
}

/* R: one record, rounded up to the write unit. */
static inline size_t mi_rec_size(void)
{
	const size_t a = mi_align();

	return (sizeof(struct mrec_img) + a - 1) & ~(a - 1);
}

/* Record slots per sector, as blob_db sizes them (and as the test-only cap
 * limits them). */
static inline uint16_t mi_log_cap(void)
{
	size_t cap = (MI_SECTOR_SZ - MI_LOG_OFF) / mi_rec_size();

#if defined(CONFIG_BLOB_DB_TEST_MASTER_LOG_CAP) && CONFIG_BLOB_DB_TEST_MASTER_LOG_CAP > 0
	cap = MIN(cap, (size_t)CONFIG_BLOB_DB_TEST_MASTER_LOG_CAP);
#endif
	return (uint16_t)cap;
}

static inline void mhdr_seal(struct mhdr_img *h)
{
	h->prefix_crc16 = crc16_ccitt(0xffff, (const uint8_t *)h,
				      offsetof(struct mhdr_img, prefix_crc16));
	h->hdr_crc32 = crc32_ieee((const uint8_t *)h,
				  offsetof(struct mhdr_img, hdr_crc32));
}

/* CRC32-IEEE over LE32(rot_gen) followed by the record's first 44 bytes, as
 * one buffer: the definition, not the library's chained computation. */
static inline uint32_t mrec_crc(uint32_t rot_gen, const struct mrec_img *r)
{
	uint8_t b[4 + offsetof(struct mrec_img, crc32)];

	sys_put_le32(rot_gen, b);
	memcpy(b + 4, r, sizeof(b) - 4);
	return crc32_ieee(b, sizeof(b));
}

static inline void mi_sector_read(uint8_t slot)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(MI_PARTITION_ID, &fa));
	zassert_ok(flash_area_read(fa, (off_t)slot * MI_SECTOR_SZ, mi_sec,
				   sizeof(mi_sec)));
	flash_area_close(fa);
}

/* Erase sector @p slot and write mi_sec back into it. */
static inline void mi_sector_write(uint8_t slot)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(MI_PARTITION_ID, &fa));
	zassert_ok(flash_area_erase(fa, (off_t)slot * MI_SECTOR_SZ, MI_SECTOR_SZ));
	zassert_ok(flash_area_write(fa, (off_t)slot * MI_SECTOR_SZ, mi_sec,
				    sizeof(mi_sec)));
	flash_area_close(fa);
}

static inline void master_erase_raw(uint8_t slot)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(MI_PARTITION_ID, &fa));
	zassert_ok(flash_area_erase(fa, (off_t)slot * MI_SECTOR_SZ, MI_SECTOR_SZ));
	flash_area_close(fa);
}

/* Leave the metadata sectors erased so the next mount sees a virgin store.
 * Every test that deliberately leaves an unmountable partition behind must
 * call this, or the following test's fixture cannot mount. */
static inline void masters_erase(void)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(MI_PARTITION_ID, &fa));
	zassert_ok(flash_area_erase(fa, 0, 3 * MI_SECTOR_SZ));
	flash_area_close(fa);
}

static inline void master_hdr_read_raw(uint8_t slot, struct mhdr_img *h)
{
	mi_sector_read(slot);
	memcpy(h, mi_sec, sizeof(*h));
}

/* Replace sector @p slot's header with @p h, as given (seal it first, or
 * not), keeping its record log. */
static inline void master_hdr_rewrite_raw(uint8_t slot, const struct mhdr_img *h)
{
	mi_sector_read(slot);
	memcpy(mi_sec, h, sizeof(*h));
	mi_sector_write(slot);
}

static inline uint8_t *mi_slot(uint16_t i)
{
	return mi_sec + MI_LOG_OFF + (size_t)i * mi_rec_size();
}

static inline bool mi_slot_erased(uint16_t i)
{
	const uint8_t *p = mi_slot(i);

	for (size_t k = 0; k < mi_rec_size(); k++) {
		if (p[k] != 0xff) {
			return false;
		}
	}
	return true;
}

/* In mi_sec: one past the last slot that is not fully erased. */
static inline uint16_t mi_log_end(void)
{
	uint16_t end = 0;

	for (uint16_t i = 0; i < (MI_SECTOR_SZ - MI_LOG_OFF) / mi_rec_size(); i++) {
		if (!mi_slot_erased(i)) {
			end = i + 1;
		}
	}
	return end;
}

/* In mi_sec: the newest record that verifies, or -1. */
static inline int mi_log_newest(struct mrec_img *out)
{
	const struct mhdr_img *h = (const struct mhdr_img *)mi_sec;

	for (int i = (int)mi_log_end() - 1; i >= 0; i--) {
		memcpy(out, mi_slot((uint16_t)i), sizeof(*out));
		if (out->rec_no == (uint32_t)i &&
		    out->crc32 == mrec_crc(h->rot_gen, out)) {
			return i;
		}
	}
	return -1;
}

/* The ok master with the higher rot_gen. */
static inline uint8_t master_active_raw(void)
{
	struct mhdr_img a, b;

	master_hdr_read_raw(0, &a);
	master_hdr_read_raw(1, &b);
	return (b.rot_gen > a.rot_gen && memcmp(b.magic, "BDMS", 4) == 0) ? 1 : 0;
}

/* The newest record of sector @p slot; asserts there is one. */
static inline int master_newest_raw(uint8_t slot, struct mrec_img *r)
{
	mi_sector_read(slot);

	const int i = mi_log_newest(r);

	zassert_true(i >= 0, "master %u holds no record that verifies", slot);
	return i;
}

/* Append @p r to sector @p slot's log at its end, sealed for that slot and
 * that sector's header; no erase. */
static inline void master_rec_append_raw(uint8_t slot, struct mrec_img *r)
{
	const struct flash_area *fa;
	uint8_t buf[64];

	mi_sector_read(slot);

	const struct mhdr_img *h = (const struct mhdr_img *)mi_sec;
	const uint16_t end = mi_log_end();

	zassert_true(end < mi_log_cap(), "master %u: log full", slot);
	r->rec_no = end;
	r->crc32 = mrec_crc(h->rot_gen, r);
	memset(buf, 0xff, sizeof(buf));
	memcpy(buf, r, sizeof(*r));

	zassert_ok(flash_area_open(MI_PARTITION_ID, &fa));
	zassert_ok(flash_area_write(fa, (off_t)slot * MI_SECTOR_SZ + MI_LOG_OFF +
					    (off_t)end * mi_rec_size(),
				    buf, mi_rec_size()));
	flash_area_close(fa);
}

/* Flip @p mask into byte @p off of sector @p slot. */
static inline void master_flip_raw(uint8_t slot, size_t off, uint8_t mask)
{
	mi_sector_read(slot);
	mi_sec[off] ^= mask;
	mi_sector_write(slot);
}

/* Byte offset of record slot @p i in a sector. */
static inline size_t mi_rec_off(uint16_t i)
{
	return MI_LOG_OFF + (size_t)i * mi_rec_size();
}

#endif /* BLOB_DB_TEST_MASTER_IMG_H_ */
