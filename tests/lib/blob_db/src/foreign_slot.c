/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Slots this build cannot read, and what compaction may drop
 * (doc/proposals/2026-10-28-master-log.md D13, implementation plan PR 3).
 *
 * A foreign slot is whole (sealed, sized, CRC intact) but carries a slot flag
 * this build does not know: a newer build wrote it. It is data, not a torn
 * tail, so its bucket answers no read (-EIO) and takes no write (-ENOTSUP),
 * and compaction never drops it. Separately, compaction drops what its walk
 * cannot reach only when that is the remains of one torn append, at most one
 * slot span past where the log ends.
 *
 * The cases forge flash at blob_db's own offsets, so they run on the
 * flash_area backend and skip on UBI (see SKIP_UNLESS_RAW_PARTITION in
 * main.c).
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>

#define FS_PARTITION_ID  PARTITION_ID(storage_partition)
#define FS_SECTOR_SZ     4096   /* native_sim overlay: 4 KB sectors */
#define FS_N_BUCKETS     2045   /* 2048 sectors - 2 masters - scratch */
#define FS_SEALED        0x01
/* A slot flag no build assigns yet: bit 6 is free in the registry of the
 * master-log proposal (§4.2), so every build here reads it as unknown. */
#define FS_FLAG_UNKNOWN  0x40
/* Flags whose combination the format leaves undefined (DELTA with any of
 * TOMBSTONE, INDEXED, SEGMENT): foreign to every build, delta or not. */
#define FS_TOMBSTONE     0x02
#define FS_INDEXED       0x04
#define FS_SEGMENT       0x08
#define FS_DELTA         0x20

#define FS_SKIP_UNLESS_RAW_PARTITION() \
	Z_TEST_SKIP_IFNDEF(CONFIG_BLOB_DB_BACKEND_FLASH_AREA)

struct __packed fs_slot_head {
	uint8_t  flags;
	uint8_t  reserved;
	uint16_t val_len;
	uint64_t id;
};

static uint8_t fs_before[FS_SECTOR_SZ];
static uint8_t fs_after[FS_SECTOR_SZ];

static off_t fs_bucket_off(uint16_t bid)
{
	return (off_t)(3 + bid) * FS_SECTOR_SZ;
}

static size_t fs_align(void)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(FS_PARTITION_ID, &fa));

	const size_t a = flash_area_align(fa);

	flash_area_close(fa);
	return a ? a : 1;
}

static size_t fs_slot_size(uint16_t val_len)
{
	const size_t a = fs_align();

	return (14 + val_len + a - 1) & ~(a - 1);
}

static void fs_read_bucket(uint16_t bid, uint8_t *buf)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(FS_PARTITION_ID, &fa));
	zassert_ok(flash_area_read(fa, fs_bucket_off(bid), buf, FS_SECTOR_SZ));
	flash_area_close(fa);
}

static void fs_write(off_t off, const void *buf, size_t len)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(FS_PARTITION_ID, &fa));
	zassert_ok(flash_area_write(fa, off, buf, len));
	flash_area_close(fa);
}

/* Where the next slot of bucket @p bid would land: the first erased header. */
static off_t fs_cursor(uint16_t bid)
{
	off_t cursor = 16;

	fs_read_bucket(bid, fs_before);
	for (;;) {
		const struct fs_slot_head *h =
			(const struct fs_slot_head *)(fs_before + cursor);

		if (cursor + 14 > FS_SECTOR_SZ || h->flags == 0xff) {
			return cursor;
		}
		cursor += (off_t)fs_slot_size(h->val_len);
	}
}

/* Append, by hand, the slot a newer build would write: whole and intact, with
 * a flag this build does not know. */
static void forge_slot(uint16_t bid, uint64_t id, uint8_t flags, const char *val)
{
	static uint8_t slot[64];
	const uint16_t len = (uint16_t)strlen(val);
	const size_t ssz = fs_slot_size(len);
	struct fs_slot_head h = {
		.flags = flags,
		.val_len = len,
		.id = id,
	};

	zassert_true(ssz <= sizeof(slot));
	memset(slot, 0xff, sizeof(slot));
	memcpy(slot, &h, sizeof(h));
	memcpy(slot + sizeof(h), val, len);

	uint16_t crc = crc16_ccitt(0xffff, (const uint8_t *)&h, 4);

	crc = crc16_ccitt(crc, (const uint8_t *)&h.id, sizeof(h.id));
	crc = crc16_ccitt(crc, (const uint8_t *)val, len);
	memcpy(slot + sizeof(h) + len, &crc, sizeof(crc));

	fs_write(fs_bucket_off(bid) + fs_cursor(bid), slot, ssz);
}

static void forge_foreign_slot(uint16_t bid, uint64_t id, const char *val)
{
	forge_slot(bid, id, FS_SEALED | FS_FLAG_UNKNOWN, val);
}

/* What a power cut leaves when it tears a slot inside its header: a sealed
 * flags byte and a length no slot can have, so the walk cannot size it. */
static off_t forge_torn_header(uint16_t bid)
{
	const off_t at = fs_cursor(bid);
	const uint8_t head[4] = { FS_SEALED, 0x00, 0xff, 0xff };

	fs_write(fs_bucket_off(bid) + at, head, sizeof(head));
	return at;
}

static uint64_t fs_put(const char *val)
{
	const uint64_t id = blob_db_alloc_id();

	zassert_not_equal(id, 0);
	zassert_ok(blob_db_update(id, val, strlen(val)));
	return id;
}

static void fs_assert_blob(uint64_t id, const char *val)
{
	uint8_t buf[16];
	size_t got;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_equal(got, strlen(val));
	zassert_mem_equal(buf, val, got);
}

static int fs_count_cb(uint64_t id, const void *p, size_t len, void *user)
{
	ARG_UNUSED(id);
	ARG_UNUSED(p);
	ARG_UNUSED(len);
	(*(int *)user)++;
	return 0;
}

static void fs_before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount(), "mount failed in setup");
	zassert_ok(blob_db_format(), "format failed in setup");
}

static void fs_after_each(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
}

ZTEST_SUITE(blob_db_foreign, NULL, NULL, fs_before_each, fs_after_each, NULL);

/* A newer build appended a slot for an id this build holds an older version
 * of. Answering with the older version would be stale data; dropping the
 * newer one at the next compaction would lose it. Before, the walk took the
 * slot for a torn tail and did both. */
ZTEST(blob_db_foreign, test_foreign_slot_closes_its_bucket)
{
	FS_SKIP_UNLESS_RAW_PARTITION();

	const uint64_t id = fs_put("OLD");
	const uint64_t elsewhere = fs_put("ELSE");
	const uint16_t bid = (uint16_t)(id % FS_N_BUCKETS);
	uint8_t buf[16];
	size_t got;

	zassert_not_equal((uint16_t)(elsewhere % FS_N_BUCKETS), bid);
	zassert_ok(blob_db_unmount());
	forge_foreign_slot(bid, id, "NEWER");
	fs_read_bucket(bid, fs_before);

	for (int pass = 0; pass < 2; pass++) {
		zassert_ok(blob_db_mount());

		zassert_equal(blob_db_get(id, buf, sizeof(buf), &got), -EIO,
			      "pass %d: served a version a newer slot replaces",
			      pass);
		zassert_equal(blob_db_size(id, &got), -EIO);
		zassert_equal(blob_db_read(id, 0, buf, sizeof(buf), &got), -EIO);
		zassert_false(blob_db_exists(id));

		zassert_equal(blob_db_update(id, "X", 1), -ENOTSUP,
			      "pass %d: wrote into a bucket it cannot read", pass);
		zassert_equal(blob_db_write(id, 0, "X", 1), -ENOTSUP);
		zassert_equal(blob_db_delete(id), -ENOTSUP);

		int seen = 0;

		zassert_equal(blob_db_iterate(fs_count_cb, &seen), -EIO);

		/* Every other bucket works as before. */
		fs_assert_blob(elsewhere, pass == 0 ? "ELSE" : "MORE");
		zassert_ok(blob_db_update(elsewhere, "MORE", 4));
		fs_assert_blob(elsewhere, "MORE");

		zassert_ok(blob_db_unmount());
		fs_read_bucket(bid, fs_after);
		zassert_mem_equal(fs_after, fs_before, FS_SECTOR_SZ,
				  "pass %d: the bucket was written", pass);
	}
}

/* DELTA combined with TOMBSTONE, INDEXED or SEGMENT means nothing yet. A
 * whole slot carrying such a combination is foreign, as an unknown bit is:
 * not served, not written over, not dropped. */
ZTEST(blob_db_foreign, test_undefined_delta_combinations_are_foreign)
{
	FS_SKIP_UNLESS_RAW_PARTITION();

	static const uint8_t combos[] = {
		FS_SEALED | FS_DELTA | FS_TOMBSTONE,
		FS_SEALED | FS_DELTA | FS_INDEXED,
		FS_SEALED | FS_DELTA | FS_SEGMENT,
	};

	for (size_t k = 0; k < ARRAY_SIZE(combos); k++) {
		const uint64_t id = fs_put("OLD");
		const uint16_t bid = (uint16_t)(id % FS_N_BUCKETS);
		uint8_t buf[16];
		size_t got;

		zassert_ok(blob_db_unmount());
		forge_slot(bid, id, combos[k], "0123456789");
		fs_read_bucket(bid, fs_before);
		zassert_ok(blob_db_mount());

		zassert_equal(blob_db_get(id, buf, sizeof(buf), &got), -EIO,
			      "flags 0x%02x: served the older value", combos[k]);
		zassert_equal(blob_db_update(id, "X", 1), -ENOTSUP,
			      "flags 0x%02x: wrote into the bucket", combos[k]);

		zassert_ok(blob_db_unmount());
		fs_read_bucket(bid, fs_after);
		zassert_mem_equal(fs_after, fs_before, FS_SECTOR_SZ,
				  "flags 0x%02x: the bucket was written", combos[k]);
		zassert_ok(blob_db_mount());
	}
}

/* The root's bucket holds a foreign slot. Binding an empty root, as mount does
 * when it finds none, would reformat the bucket and erase the slot, so mount
 * refuses instead and writes nothing there. */
ZTEST(blob_db_foreign, test_foreign_slot_in_the_root_bucket_refuses_mount)
{
	FS_SKIP_UNLESS_RAW_PARTITION();

	const uint16_t root_bid = (uint16_t)(BLOB_DB_ROOT_ID % FS_N_BUCKETS);

	zassert_ok(blob_db_unmount());
	forge_foreign_slot(root_bid, BLOB_DB_ROOT_ID, "ROOT2");
	fs_read_bucket(root_bid, fs_before);

	zassert_equal(blob_db_mount(), -ENOTSUP);
	fs_read_bucket(root_bid, fs_after);
	zassert_mem_equal(fs_after, fs_before, FS_SECTOR_SZ,
			  "the root bucket was written");

	/* Leave a store the next setup can mount: blank masters format. */
	const struct flash_area *fa;

	zassert_ok(flash_area_open(FS_PARTITION_ID, &fa));
	zassert_ok(flash_area_erase(fa, 0, 3 * FS_SECTOR_SZ));
	flash_area_close(fa);
}

/* A torn header with stray bits programmed further into the slot it began,
 * out of order, as a page program cut midway leaves them. That is one torn
 * append, so compaction still drops it, and the write that needed the
 * compaction lands. */
ZTEST(blob_db_foreign, test_torn_append_with_stray_bits_still_compacts)
{
	FS_SKIP_UNLESS_RAW_PARTITION();

	const uint64_t id = fs_put("KEEP");
	const uint16_t bid = (uint16_t)(id % FS_N_BUCKETS);
	const uint8_t stray = 0x00;

	zassert_ok(blob_db_unmount());

	const off_t at = forge_torn_header(bid);

	fs_write(fs_bucket_off(bid) + at + 100, &stray, 1);

	zassert_ok(blob_db_mount());
	fs_assert_blob(id, "KEEP");
	zassert_ok(blob_db_update(id, "NEW", 3),
		   "a torn append's remains blocked the compaction");
	fs_assert_blob(id, "NEW");
}

/* Programmed bytes further past the end of the log than one append can
 * reach: data the walk cannot read, from a build with a larger payload cap or
 * a stride the walk lost. Compaction would drop it, so it refuses, and the
 * bucket stays as it is. Before, compaction dropped it. */
ZTEST(blob_db_foreign, test_bytes_past_one_slot_span_block_compaction)
{
	FS_SKIP_UNLESS_RAW_PARTITION();

	const uint64_t id = fs_put("KEEP");
	const uint16_t bid = (uint16_t)(id % FS_N_BUCKETS);
	const uint8_t data = 0x00;

	zassert_ok(blob_db_unmount());

	const off_t at = forge_torn_header(bid);
	const off_t beyond = at + (off_t)fs_slot_size(CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) + 8;

	if (beyond >= FS_SECTOR_SZ) {
		ztest_test_skip();   /* the cap leaves no room past one span */
	}
	fs_write(fs_bucket_off(bid) + beyond, &data, 1);
	fs_read_bucket(bid, fs_before);

	zassert_ok(blob_db_mount());
	zassert_equal(blob_db_update(id, "NEW", 3), -EIO,
		      "compaction dropped bytes past one slot span");
	fs_assert_blob(id, "KEEP");
	zassert_ok(blob_db_unmount());

	fs_read_bucket(bid, fs_after);
	zassert_mem_equal(fs_after, fs_before, FS_SECTOR_SZ,
			  "the bucket was written");
}
