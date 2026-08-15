/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite for blob_db. Cases mirror doc/impl/l1_bucketlog.md §11 and the
 * contract in doc/layers/l1_blob_db.md.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>

#define BLOB_DB_TEST_PARTITION_ID  PARTITION_ID(storage_partition)

/* native_sim overlay geometry: 8 MB of 4 KB sectors. */
#define TEST_SECTOR_SZ  4096

/* alloc + bind in one step — the contract's replacement for the old `put`. */
static uint64_t put_blob(const void *payload, size_t len)
{
	uint64_t id = blob_db_alloc_id();

	zassert_not_equal(id, 0, "alloc_id returned 0 (not mounted?)");
	zassert_ok(blob_db_update(id, payload, len), "bind failed");
	return id;
}

/* Suite fixture: mount and format before each test so the cases start
 * with a clean partition; unmount after so the next setup can mount
 * fresh. */
static void blob_db_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount(), "mount failed in setup");
	zassert_ok(blob_db_format(), "format failed in setup");
}

static void blob_db_after(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
}

ZTEST_SUITE(blob_db, NULL, NULL, blob_db_before, blob_db_after, NULL);

/* 1. Fresh format: only the reserved root blob is live (empty payload). */
ZTEST(blob_db, test_mount_empty_formats_partition)
{
	zassert_equal(blob_db_count(), 1,
		      "want count=1 (root only), got %zu", blob_db_count());
	zassert_true(blob_db_exists(BLOB_DB_ROOT_ID),
		     "root must be live after format");

	uint8_t buf[16];
	size_t got = 42;

	zassert_ok(blob_db_get(BLOB_DB_ROOT_ID, buf, sizeof(buf), &got));
	zassert_equal(got, 0, "root payload must be empty after format, got %zu", got);
}

/* 2. Single blob round-trip with NUL and 0xff bytes inside. */
ZTEST(blob_db, test_bind_get_roundtrip)
{
	const uint8_t pl[] = { 'h', 'e', 0x00, 'l', 0xff, 'o' };
	uint64_t id = put_blob(pl, sizeof(pl));

	uint8_t buf[16];
	size_t got;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_equal(got, sizeof(pl));
	zassert_mem_equal(buf, pl, sizeof(pl));
}

/* 3. First user alloc_id returns 2 (root consumed id=1), then monotonic. */
ZTEST(blob_db, test_alloc_id_skips_root_then_monotonic)
{
	uint64_t a = blob_db_alloc_id();

	zassert_equal(a, 2, "first user alloc_id returned %llu, want 2",
		      (unsigned long long)a);

	uint64_t b = blob_db_alloc_id();
	uint64_t c = blob_db_alloc_id();

	zassert_equal(b, 3);
	zassert_equal(c, 4);
}

/* 4. Get on a never-issued id returns -ENOENT. */
ZTEST(blob_db, test_get_missing_returns_enoent)
{
	uint8_t buf[16];
	size_t got;

	zassert_equal(blob_db_get(9999, buf, sizeof(buf), &got), -ENOENT);
}

/* 5. update keeps the id; payload changes. */
ZTEST(blob_db, test_update_keeps_id)
{
	uint64_t id = put_blob("v1", 2);

	zassert_ok(blob_db_update(id, "v2", 2));

	uint8_t buf[16];
	size_t got;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_equal(got, 2);
	zassert_mem_equal(buf, "v2", 2);
	zassert_true(blob_db_exists(id));
}

/* 6. delete works; double-delete and delete-missing both return -ENOENT. */
ZTEST(blob_db, test_delete_lifecycle)
{
	uint64_t id = put_blob("v", 1);

	zassert_true(blob_db_exists(id));

	zassert_ok(blob_db_delete(id));
	zassert_false(blob_db_exists(id));

	uint8_t buf[16];
	size_t got;

	zassert_equal(blob_db_get(id, buf, sizeof(buf), &got), -ENOENT);
	zassert_equal(blob_db_delete(id), -ENOENT);
	zassert_equal(blob_db_delete(9999), -ENOENT);
}

/* 7. update on a never-allocated id returns -EINVAL (the defined boundary;
 *    update on a *dead* id is UB per decision D3 and is not asserted here).
 *    Root (id=1) is not a "never-allocated" id — it is consumed at format
 *    time — so update(1, ...) succeeds; test_root_rebind covers that path. */
ZTEST(blob_db, test_update_never_allocated_returns_einval)
{
	/* id=0 is never valid. */
	zassert_equal(blob_db_update(0, "x", 1), -EINVAL);

	uint64_t id = blob_db_alloc_id();   /* == 2 */

	/* id+1 was never allocated. */
	zassert_equal(blob_db_update(id + 1, "x", 1), -EINVAL);
	/* the allocated id binds fine. */
	zassert_ok(blob_db_update(id, "x", 1));
}

/* 8. Unmount/mount preserves all live blobs. */
ZTEST(blob_db, test_persistence_across_remount)
{
	uint64_t a = put_blob("aa", 2);
	uint64_t b = put_blob("bb", 2);
	uint64_t c = put_blob("cc", 2);

	zassert_ok(blob_db_delete(b));

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	uint8_t buf[16];
	size_t got;

	zassert_ok(blob_db_get(a, buf, sizeof(buf), &got));
	zassert_mem_equal(buf, "aa", 2);
	zassert_equal(blob_db_get(b, buf, sizeof(buf), &got), -ENOENT);
	zassert_ok(blob_db_get(c, buf, sizeof(buf), &got));
	zassert_mem_equal(buf, "cc", 2);

	zassert_equal(blob_db_count(), 3, "want 3 (a, c, root), got %zu",
		      blob_db_count());
}

/* 9. Payload length boundaries: 0 (allowed), MAX (allowed), MAX+1 (rejected). */
ZTEST(blob_db, test_boundary_payload_len)
{
	uint64_t empty_id = put_blob(NULL, 0);

	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t got;

	zassert_ok(blob_db_get(empty_id, buf, sizeof(buf), &got));
	zassert_equal(got, 0);

	uint8_t big[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];

	for (size_t i = 0; i < sizeof(big); i++) {
		big[i] = (uint8_t)(i ^ 0x5a);
	}
	uint64_t big_id = put_blob(big, sizeof(big));

	uint8_t back[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];

	zassert_ok(blob_db_get(big_id, back, sizeof(back), &got));
	zassert_equal(got, sizeof(big));
	zassert_mem_equal(back, big, sizeof(big));

	/* One byte past the configured cap is rejected by update. */
#ifdef CONFIG_BLOB_DB_LARGE_PAYLOADS
	/* One past the single-slot cap is a segmented write here, not an error;
	 * the cap that still rejects is the logical one. */
	static uint8_t over[CONFIG_BLOB_DB_MAX_OBJECT_LEN + 1];
#else
	uint8_t over[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN + 1];
#endif

	memset(over, 0x5a, sizeof(over));
	zassert_equal(blob_db_update(big_id, over, sizeof(over)), -EINVAL,
		      "a payload past the configured cap must be rejected");
}

/* 10. Corrupting a byte in a slot causes that slot (and everything after
 *     it in the same bucket) to drop out on next mount. A blob in a
 *     different bucket is unaffected. */
ZTEST(blob_db, test_corrupted_slot_is_invisible)
{
	uint64_t a = put_blob("AAA", 3);
	uint64_t b = put_blob("BBB", 3);

	/* a and b land in different buckets (a%N vs b%N). */
	zassert_ok(blob_db_unmount());

	const struct flash_area *fa;

	zassert_ok(flash_area_open(BLOB_DB_TEST_PARTITION_ID, &fa));

	/* a is in bucket (a%N). With N≥2 (which the 8 MB overlay gives us),
	 * a and b land in different sectors. Bucket sector for id=1 is at
	 * (BLOB_DB_FIRST_BUCKET + 1) * 4096 = 4*4096 = 16384. Slot starts at
	 * +16 (after bucket header). Slot layout: hdr(4) | id(8) | payload(3) | crc(2).
	 * Flip a bit in the payload so the CRC fails. flash NOR can only go
	 * 1→0, so we read the byte and clear a bit. */
	const off_t bucket_a_off = (3 + (off_t)(a % 2045)) * 4096;
	const off_t payload_off  = bucket_a_off + 16 + 4 + 8;

	uint8_t b0;

	zassert_ok(flash_area_read(fa, payload_off, &b0, 1));
	uint8_t b1 = b0 & 0xfe;

	zassert_ok(flash_area_write(fa, payload_off, &b1, 1));
	flash_area_close(fa);

	zassert_ok(blob_db_mount());

	uint8_t buf[16];
	size_t got;

	zassert_equal(blob_db_get(a, buf, sizeof(buf), &got), -ENOENT,
		      "corrupted slot should be invisible");
	zassert_ok(blob_db_get(b, buf, sizeof(buf), &got),
		   "unrelated bucket should be intact");
	zassert_mem_equal(buf, "BBB", 3);
}

/* A corrupt slot must not hide the slots after it in the same bucket.
 *
 * The resident-buffer walk treated a failed CRC as end-of-log, so one rotten
 * slot made every later slot in that bucket unreachable until the next
 * compaction. The header-driven walk knows each slot's size from its own
 * header, so it steps over the rotten one and keeps going.
 */
ZTEST(blob_db, test_corrupt_slot_does_not_hide_later_slots)
{
	const uint16_t n_buckets = 2045;

	/* Two ids exactly one full round of the bucket hash apart, so they
	 * share a bucket with `first` written before `second`. */
	uint64_t first = put_blob("FIRST", 5);

	for (int i = 0; i < n_buckets - 1; i++) {
		zassert_not_equal(blob_db_alloc_id(), 0, "alloc_id failed");
	}
	uint64_t second = put_blob("SECOND", 6);

	zassert_equal(first % n_buckets, second % n_buckets,
		      "test needs both ids in one bucket (%llu vs %llu)",
		      (unsigned long long)first, (unsigned long long)second);
	zassert_true(second > first, "second must be appended after first");

	zassert_ok(blob_db_unmount());

	/* Rot a bit in the FIRST slot's payload. NOR only goes 1->0. */
	const off_t bucket_off = (off_t)(3 + (first % n_buckets)) * TEST_SECTOR_SZ;
	const off_t payload_off = bucket_off + 16 + 4 + 8;
	const struct flash_area *fa;
	uint8_t byte;

	zassert_ok(flash_area_open(BLOB_DB_TEST_PARTITION_ID, &fa));
	zassert_ok(flash_area_read(fa, payload_off, &byte, 1));
	zassert_not_equal(byte, 0, "need a set bit to clear");
	byte &= (uint8_t)(byte - 1);   /* clears the lowest set bit */
	zassert_ok(flash_area_write(fa, payload_off, &byte, 1));
	flash_area_close(fa);

	zassert_ok(blob_db_mount());

	uint8_t buf[16];
	size_t got;

	zassert_equal(blob_db_get(first, buf, sizeof(buf), &got), -ENOENT,
		      "the rotten slot itself must stay invisible");
	zassert_ok(blob_db_get(second, buf, sizeof(buf), &got),
		   "a later slot in the same bucket must remain reachable");
	zassert_equal(got, 6);
	zassert_mem_equal(buf, "SECOND", 6);
}

/* 11. Hitting bucket-full triggers per-bucket compaction; the latest
 *     value survives. */
ZTEST(blob_db, test_bucket_full_triggers_compaction)
{
	uint64_t id = put_blob("v0000", 5);

	char val[8];

	for (int i = 0; i < 500; i++) {
		(void)snprintk(val, sizeof(val), "v%04d", i);
		zassert_ok(blob_db_update(id, val, 5),
			   "update #%d failed", i);
	}

	uint8_t buf[16];
	size_t got;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_equal(got, 5);
	zassert_mem_equal(buf, "v0499", 5);
}

/* 12. Tombstones don't survive compaction. */
ZTEST(blob_db, test_compaction_drops_tombstones_and_overrides)
{
	uint64_t id = put_blob("v1", 2);

	zassert_ok(blob_db_update(id, "v2", 2));
	zassert_ok(blob_db_delete(id));

	/* count() does latest-wins dedup, so the tombstoned id is already
	 * invisible — even pre-compaction. Only the reserved root remains. */
	zassert_equal(blob_db_count(), 1);

	/* Force a compaction in this id's bucket via many updates to an id
	 * that lives in the same bucket. With sequential ids and round-robin
	 * hashing, id and id+N share a bucket. So we bind id+N and update it. */
	uint64_t other = 0;

	for (int i = 0; i < 2045; i++) {
		uint64_t tmp = put_blob("x", 1);

		if (tmp % 2045 == id % 2045) {
			other = tmp;
			break;
		}
	}
	zassert_not_equal(other, 0,
			  "couldn't find a same-bucket sibling for id=%llu",
			  (unsigned long long)id);

	char val[8];

	for (int i = 0; i < 500; i++) {
		(void)snprintk(val, sizeof(val), "v%04d", i);
		zassert_ok(blob_db_update(other, val, 5));
	}

	/* After compaction, neither the tombstone nor the overridden slots
	 * for `id` remain on flash. Functionally, get(id) is still -ENOENT. */
	uint8_t buf[16];
	size_t got;

	zassert_equal(blob_db_get(id, buf, sizeof(buf), &got), -ENOENT);
}

/* 13. Compaction preserves ids of live blobs in other buckets. */
ZTEST(blob_db, test_compaction_preserves_ids)
{
	uint64_t ids[20];
	char p[16];

	for (int i = 0; i < (int)ARRAY_SIZE(ids); i++) {
		int n = snprintk(p, sizeof(p), "blob%d", i);

		ids[i] = put_blob(p, n);
	}

	/* Force compaction on ids[0]'s bucket by spamming updates. */
	char val[8];

	for (int i = 0; i < 500; i++) {
		(void)snprintk(val, sizeof(val), "v%04d", i);
		zassert_ok(blob_db_update(ids[0], val, 5));
	}

	for (int i = 1; i < (int)ARRAY_SIZE(ids); i++) {
		char buf[16];
		size_t got;
		int n = snprintk(p, sizeof(p), "blob%d", i);

		zassert_ok(blob_db_get(ids[i], buf, sizeof(buf), &got),
			   "id=%llu lost across compaction",
			   (unsigned long long)ids[i]);
		zassert_equal((int)got, n);
		zassert_mem_equal(buf, p, n);
	}
}

/* 14. Injected COMPACTING(bid) at a higher master gen is recovered cleanly. */
/* --- raw master-sector access, for forging on-flash states ---------------
 *
 * Deliberately a private copy of the layout rather than the library's own
 * header: if blob_db's master ever drifts, these tests must fail loudly
 * rather than silently follow it. sizeof and the two CRC spans are asserted
 * below for the same reason.
 */
struct __packed master_img {
	/* frozen compatibility prefix */
	uint8_t  magic[4];
	uint8_t  format_major;
	uint8_t  format_minor;
	uint16_t hdr_len;
	uint16_t reserved;
	uint16_t prefix_crc16;
	/* body */
	uint32_t generation;
	uint8_t  state;
	uint16_t cbid;
	uint8_t  reserved0;
	uint64_t next_id_hint;
	uint64_t seg_owner;
	uint8_t  reserved1[24];
	uint32_t hdr_crc32;
};
BUILD_ASSERT(sizeof(struct master_img) == 64, "master layout drift");
BUILD_ASSERT(offsetof(struct master_img, prefix_crc16) == 10, "prefix drift");
BUILD_ASSERT(offsetof(struct master_img, hdr_crc32) == 60, "body drift");

/* The format major this build of blob_db writes. A build that can store
 * segmented objects declares 2, because a slot flag changes what a payload
 * means and older software must refuse such a store rather than misread it.
 * Asserted rather than assumed, so a further bump breaks these tests instead
 * of quietly reinterpreting them. */
#ifdef CONFIG_BLOB_DB_LARGE_PAYLOADS
#define TEST_FORMAT_MAJOR  2
#else
#define TEST_FORMAT_MAJOR  1
#endif

static void master_seal(struct master_img *m)
{
	m->prefix_crc16 = crc16_ccitt(0xffff, (const uint8_t *)m,
				      offsetof(struct master_img, prefix_crc16));
	m->hdr_crc32 = crc32_ieee((const uint8_t *)m,
				  offsetof(struct master_img, hdr_crc32));
}

static void master_read_raw(uint8_t slot, struct master_img *m)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(BLOB_DB_TEST_PARTITION_ID, &fa));
	zassert_ok(flash_area_read(fa, (off_t)slot * TEST_SECTOR_SZ, m, sizeof(*m)));
	flash_area_close(fa);
}

static void master_write_raw(uint8_t slot, const struct master_img *m)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(BLOB_DB_TEST_PARTITION_ID, &fa));
	zassert_ok(flash_area_erase(fa, (off_t)slot * TEST_SECTOR_SZ, TEST_SECTOR_SZ));
	zassert_ok(flash_area_write(fa, (off_t)slot * TEST_SECTOR_SZ, m, sizeof(*m)));
	flash_area_close(fa);
}

/* Leave the metadata sectors erased so the next mount sees a virgin store.
 * Every test that deliberately leaves an unmountable partition behind must
 * call this, or the following test's fixture cannot mount. */
static void masters_erase(void)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(BLOB_DB_TEST_PARTITION_ID, &fa));
	zassert_ok(flash_area_erase(fa, 0, 3 * TEST_SECTOR_SZ));
	flash_area_close(fa);
}

ZTEST(blob_db, test_mid_compaction_crash_recovery)
{
	uint64_t a = put_blob("AA", 2);
	uint64_t b = put_blob("BB", 2);

	zassert_ok(blob_db_unmount());

	/* Read current master A to learn its gen. */
	struct master_img mhdr;

	master_read_raw(0, &mhdr);
	zassert_equal(mhdr.format_major, TEST_FORMAT_MAJOR,
		      "format major changed; revisit these tests");

	/* Build a COMPACTING master with much higher gen, written to slot B. */
	struct master_img injected = mhdr;

	injected.generation = mhdr.generation + 100;
	injected.state = 1;          /* COMPACTING */
	injected.cbid = 42;          /* arbitrary bucket id */
	master_seal(&injected);
	master_write_raw(1, &injected);

	zassert_ok(blob_db_mount(), "mount should run recover_compaction");

	/* Recovery must actually have run: it commits a CLEAN master one
	 * generation past the COMPACTING one, on the other slot. Without this
	 * the case passes even when mount quietly ignores the injected master. */
	struct master_img after_a;

	master_read_raw(0, &after_a);
	zassert_equal(after_a.generation, injected.generation + 1,
		      "recovery did not commit a newer master");
	zassert_equal(after_a.state, 0, "recovery left the store COMPACTING");

	uint8_t buf[16];
	size_t got;

	zassert_ok(blob_db_get(a, buf, sizeof(buf), &got));
	zassert_mem_equal(buf, "AA", 2);
	zassert_ok(blob_db_get(b, buf, sizeof(buf), &got));
	zassert_mem_equal(buf, "BB", 2);
}

/* 15. Iterate visits each live blob exactly once. */
struct iter_ctx {
	int hits;
	uint64_t saw[8];
	size_t saw_len[8];
};

static int iter_cb(uint64_t id, const void *p, size_t len, void *user)
{
	struct iter_ctx *cc = user;

	ARG_UNUSED(p);
	if (cc->hits < (int)ARRAY_SIZE(cc->saw)) {
		cc->saw[cc->hits] = id;
		cc->saw_len[cc->hits] = len;
	}
	cc->hits++;
	return 0;
}

ZTEST(blob_db, test_iterate_visits_each_live_blob_once)
{
	uint64_t ids[5];

	ids[0] = put_blob("a", 1);
	ids[1] = put_blob("b", 1);
	ids[2] = put_blob("c", 1);
	ids[3] = put_blob("d", 1);
	ids[4] = put_blob("e", 1);

	zassert_ok(blob_db_delete(ids[1]));
	zassert_ok(blob_db_update(ids[3], "DD", 2));

	struct iter_ctx c = { 0 };

	zassert_ok(blob_db_iterate(iter_cb, &c));
	/* 4 user blobs (ids[1] deleted) + the reserved root = 5. */
	zassert_equal(c.hits, 5, "expected 5 live blobs (4 + root), got %d",
		      c.hits);

	for (int i = 0; i < c.hits; i++) {
		zassert_not_equal(c.saw[i], ids[1],
				  "iterate emitted a deleted id");
	}
}

/* 16. Durable id allocation (contract §2 / impl §13.1): an id that was
 *     allocated but never bound must NEVER be handed out again after a
 *     remount — otherwise a client that durably recorded that id (a root
 *     registry entry, a mutation watermark) would collide with a new owner. */
ZTEST(blob_db, test_allocated_unbound_id_survives_remount)
{
	/* Bind one real blob so the store isn't trivially empty. */
	uint64_t bound = put_blob("keep", 4);

	/* Allocate several ids WITHOUT binding them — nothing on flash. */
	uint64_t unbound1 = blob_db_alloc_id();
	uint64_t unbound2 = blob_db_alloc_id();

	zassert_true(unbound2 > unbound1);
	zassert_true(unbound1 > bound);

	/* Crash/remount: the scan sees no slots for the unbound ids. */
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	/* The next allocation must be strictly greater than every id ever
	 * handed out — including the unbound ones the scan cannot see. */
	uint64_t next = blob_db_alloc_id();

	zassert_true(next > unbound2,
		     "reissued a burned id: next=%llu unbound2=%llu",
		     (unsigned long long)next, (unsigned long long)unbound2);

	/* The bound blob is of course still there. */
	uint8_t buf[8];
	size_t got;

	zassert_ok(blob_db_get(bound, buf, sizeof(buf), &got));
	zassert_mem_equal(buf, "keep", 4);
}

/* Root invariant — id=1 is live between mount and unmount (see @blob_db_root
 * in blob_db.h). */
ZTEST(blob_db, test_root_present_after_mount)
{
	zassert_true(blob_db_exists(BLOB_DB_ROOT_ID));

	uint8_t buf[16];
	size_t got = 42;

	zassert_ok(blob_db_get(BLOB_DB_ROOT_ID, buf, sizeof(buf), &got));
	zassert_equal(got, 0);
}

/* Callers may `update(root, ...)` directly. The root convention says exactly
 * one component owns id=1; the library does not enforce that, but it must
 * accept the write and persist it across a remount. */
ZTEST(blob_db, test_root_rebind_persists_across_remount)
{
	zassert_ok(blob_db_update(BLOB_DB_ROOT_ID, "hi", 2));

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	uint8_t buf[8];
	size_t got;

	zassert_ok(blob_db_get(BLOB_DB_ROOT_ID, buf, sizeof(buf), &got));
	zassert_equal(got, 2);
	zassert_mem_equal(buf, "hi", 2);
}

/* erase_all — postconditions match a fresh format(). */
ZTEST(blob_db, test_erase_all_leaves_only_root)
{
	/* Populate the store across several buckets. */
	uint64_t a = put_blob("alpha", 5);
	uint64_t b = put_blob("beta",  4);
	uint64_t c = put_blob("gamma", 5);

	zassert_ok(blob_db_update(BLOB_DB_ROOT_ID, "OLD", 3));
	zassert_equal(blob_db_count(), 4);   /* a, b, c, root */

	zassert_ok(blob_db_erase_all());

	/* Only root survives, and its payload is empty. */
	zassert_equal(blob_db_count(), 1);
	zassert_true(blob_db_exists(BLOB_DB_ROOT_ID));
	zassert_false(blob_db_exists(a));
	zassert_false(blob_db_exists(b));
	zassert_false(blob_db_exists(c));

	uint8_t buf[16];
	size_t got = 42;

	zassert_ok(blob_db_get(BLOB_DB_ROOT_ID, buf, sizeof(buf), &got));
	zassert_equal(got, 0, "root payload must be empty after erase_all");
	zassert_equal(blob_db_get(a, buf, sizeof(buf), &got), -ENOENT);
	zassert_equal(blob_db_get(b, buf, sizeof(buf), &got), -ENOENT);
	zassert_equal(blob_db_get(c, buf, sizeof(buf), &got), -ENOENT);
}

/* erase_all resets the id space: the next allocation is 2 (root=1 consumed). */
ZTEST(blob_db, test_erase_all_resets_id_space)
{
	/* Burn a few ids first. */
	(void)blob_db_alloc_id();   /* 2 */
	(void)blob_db_alloc_id();   /* 3 */
	(void)blob_db_alloc_id();   /* 4 */

	zassert_ok(blob_db_erase_all());

	zassert_equal(blob_db_alloc_id(), 2,
		      "erase_all should reset next alloc to 2");
	zassert_equal(blob_db_alloc_id(), 3);
}

/* erase_all survives a remount: the new master beats the old, and the wiped
 * buckets stay wiped. */
ZTEST(blob_db, test_erase_all_survives_remount)
{
	(void)put_blob("x", 1);
	(void)put_blob("y", 1);
	uint64_t stale = put_blob("z", 1);

	zassert_ok(blob_db_erase_all());
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	zassert_equal(blob_db_count(), 1);
	zassert_true(blob_db_exists(BLOB_DB_ROOT_ID));
	zassert_false(blob_db_exists(stale));

	/* And the id space is still reset. */
	zassert_equal(blob_db_alloc_id(), 2);
}

/* erase_all is idempotent. */
ZTEST(blob_db, test_erase_all_idempotent)
{
	(void)put_blob("x", 1);

	zassert_ok(blob_db_erase_all());
	zassert_ok(blob_db_erase_all());   /* second call must not fail */
	zassert_ok(blob_db_erase_all());

	zassert_equal(blob_db_count(), 1);
	zassert_true(blob_db_exists(BLOB_DB_ROOT_ID));
	zassert_equal(blob_db_alloc_id(), 2);
}

/* prepare — erase-ahead: pre-format the buckets the allocator will hit next
 * so subsequent updates never pay a sector-erase on their first write. */
ZTEST(blob_db, test_prepare_zero_is_noop)
{
	zassert_equal(blob_db_prepare(0), 0);
}

/* After a fresh format the cursor is at id=2 and the four upcoming buckets
 * (2..5) are all unformatted, so prepare(4) formats exactly four. A repeat
 * call over the same window finds them already prepared and returns 0. */
ZTEST(blob_db, test_prepare_is_idempotent_on_same_cursor)
{
	zassert_equal(blob_db_prepare(4), 4,
		      "expected 4 formats on virgin buckets");

	zassert_equal(blob_db_prepare(4), 0,
		      "second call over same window must be a no-op");
}

/* The ready window slides with the alloc cursor: after some ids are burned,
 * a fresh prepare(N) formats only the new tail. */
ZTEST(blob_db, test_prepare_extends_window_after_alloc)
{
	zassert_equal(blob_db_prepare(4), 4);

	/* Move the cursor forward by 3. */
	for (int i = 0; i < 3; i++) {
		(void)put_blob("x", 1);
	}

	/* Window is now buckets [cursor .. cursor+3]. One was already
	 * inside the previous prepare(4) window; the other three are fresh. */
	zassert_equal(blob_db_prepare(4), 3);
}

/* A prepared bucket must accept its intended write on the append-only path
 * (proof-by-round-trip; timing is measured separately in app_perf). */
ZTEST(blob_db, test_prepare_then_update_roundtrips)
{
	int rc = blob_db_prepare(3);

	zassert_true(rc >= 0, "prepare returned %d", rc);

	uint64_t a = put_blob("A", 1);
	uint64_t b = put_blob("BB", 2);
	uint64_t c = put_blob("CCC", 3);

	uint8_t buf[4];
	size_t got;

	zassert_ok(blob_db_get(a, buf, sizeof(buf), &got));
	zassert_equal(got, 1);
	zassert_equal(buf[0], 'A');
	zassert_ok(blob_db_get(b, buf, sizeof(buf), &got));
	zassert_equal(got, 2);
	zassert_ok(blob_db_get(c, buf, sizeof(buf), &got));
	zassert_equal(got, 3);
}

/* An n larger than the partition's bucket count is capped; the root bucket
 * must never be re-formatted (that would drop the reserved root slot). */
ZTEST(blob_db, test_prepare_caps_and_preserves_root)
{
	zassert_ok(blob_db_update(BLOB_DB_ROOT_ID, "R", 1));

	/* Ask for far more than the partition can offer — a well-behaved
	 * prepare caps internally and returns without error. */
	int rc = blob_db_prepare(SIZE_MAX / 2);

	zassert_true(rc >= 0, "prepare failed: %d", rc);

	/* Root's payload must have survived. */
	uint8_t buf[4];
	size_t got;

	zassert_ok(blob_db_get(BLOB_DB_ROOT_ID, buf, sizeof(buf), &got));
	zassert_equal(got, 1);
	zassert_equal(buf[0], 'R');
}

/* Prepared bucket headers live on flash, so they survive a remount. */
ZTEST(blob_db, test_prepare_survives_remount)
{
	zassert_equal(blob_db_prepare(4), 4);

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	/* Same cursor, same window — everything should already be prepared. */
	zassert_equal(blob_db_prepare(4), 0);
}


/* --- on-flash format compatibility --------------------------------------
 *
 * The property under test: software must be able to tell that a store was
 * written in a format it does not understand, and must refuse it rather than
 * destroy it. Before the compatibility prefix existed, every case below ended
 * in mount() reformatting the partition.
 */

/* A store declaring a newer major is refused — and, above all, left alone. */
ZTEST(blob_db, test_foreign_major_is_refused_and_partition_untouched)
{
	uint64_t id = put_blob("KEEP", 4);

	ARG_UNUSED(id);
	zassert_ok(blob_db_unmount());

	struct master_img m;

	master_read_raw(0, &m);
	zassert_equal(m.format_major, TEST_FORMAT_MAJOR);

	struct master_img future = m;

	future.format_major = TEST_FORMAT_MAJOR + 1;
	master_seal(&future);
	master_write_raw(0, &future);

	zassert_equal(blob_db_mount(), -ENOTSUP,
		      "a newer major must be refused, not mounted");

	/* The assertion that matters: nothing was written. */
	struct master_img after;

	master_read_raw(0, &after);
	zassert_mem_equal(&after, &future, sizeof(future),
			  "mount modified a store it does not understand");

	masters_erase();
}

/* A valid prefix carrying someone else's magic is another allocator's store
 * (contract D1) — same rule, refuse and leave it. */
ZTEST(blob_db, test_foreign_magic_is_refused)
{
	zassert_ok(blob_db_unmount());

	struct master_img m;

	master_read_raw(0, &m);
	memcpy(m.magic, "XXXX", 4);
	master_seal(&m);
	master_write_raw(0, &m);

	zassert_equal(blob_db_mount(), -ENOTSUP);

	struct master_img after;

	master_read_raw(0, &after);
	zassert_mem_equal(&after, &m, sizeof(m), "foreign store was modified");

	masters_erase();
}

/* Refusing to mount is only half a contract: the escape hatch that refusal
 * names has to be reachable. It was not — blob_db_format() opened with
 * `if (!st.mounted) return -ENODEV;`, so the single store that needs
 * discarding, the one that cannot be mounted, was the one store it rejected.
 * Found by downgrading a real DK across the format-major bump
 * (app_perf/RESULTS.md, "Downgrading"), where the only way out was to reach
 * under the library and erase the partition through flash_area directly. */
ZTEST(blob_db, test_format_discards_an_unmountable_store)
{
	put_blob("DOOMED", 6);
	zassert_ok(blob_db_unmount());

	struct master_img m;

	master_read_raw(0, &m);
	zassert_equal(m.format_major, TEST_FORMAT_MAJOR);

	struct master_img future = m;

	future.format_major = TEST_FORMAT_MAJOR + 1;
	master_seal(&future);
	/* Both slots, so there is no understood master to fall back to. */
	master_write_raw(0, &future);
	master_write_raw(1, &future);

	zassert_equal(blob_db_mount(), -ENOTSUP,
		      "precondition: the store must be unmountable");

	/* The property: discard works without a mount. Clean up before
	 * asserting, because a zassert here aborts the test — and leaving an
	 * unmountable partition behind fails every following fixture, turning
	 * one regression into 70-odd failures that bury their own cause. */
	int rc = blob_db_format();

	if (rc != 0) {
		masters_erase();
	}
	zassert_ok(rc, "format must discard a store that cannot be mounted");

	/* And it must leave a *usable* store, not merely an erased partition —
	 * a caller that formats expects to carry on without mounting again. */
	zassert_true(blob_db_exists(BLOB_DB_ROOT_ID),
		     "root must be live after formatting an unmountable store");
	zassert_equal(blob_db_count(), 1, "want root only, got %zu",
		      blob_db_count());

	uint64_t fresh = put_blob("FRESH", 5);
	uint8_t buf[8];
	size_t got = 0;

	zassert_ok(blob_db_get(fresh, buf, sizeof(buf), &got));
	zassert_equal(got, 5);
	zassert_mem_equal(buf, "FRESH", 5);

	/* The foreign major is gone: an older build could mount this again. */
	struct master_img after;

	master_read_raw(0, &after);
	zassert_equal(after.format_major, TEST_FORMAT_MAJOR,
		      "format must rewrite the major it understands");
}

/* The mounted case must be untouched by the above: format on a mounted store
 * still formats it and leaves it mounted. */
ZTEST(blob_db, test_format_on_a_mounted_store_stays_mounted)
{
	put_blob("GONE", 4);
	zassert_ok(blob_db_format());

	/* Still mounted, so this works without a mount call. */
	zassert_equal(blob_db_count(), 1, "want root only, got %zu",
		      blob_db_count());
	zassert_not_equal(blob_db_alloc_id(), 0, "store must still be mounted");
}

/* An interrupted upgrade: our major on one slot, a newer one with a higher
 * generation on the other. Falling back to the older slot would silently
 * mount a stale view, so the whole store must be refused. */
ZTEST(blob_db, test_split_major_refuses_without_falling_back)
{
	put_blob("AA", 2);
	zassert_ok(blob_db_unmount());

	struct master_img ours;

	master_read_raw(0, &ours);
	zassert_equal(ours.format_major, TEST_FORMAT_MAJOR);

	struct master_img newer = ours;

	newer.format_major = TEST_FORMAT_MAJOR + 1;
	newer.generation = ours.generation + 50;
	master_seal(&newer);
	master_write_raw(1, &newer);

	zassert_equal(blob_db_mount(), -ENOTSUP,
		      "must not fall back to the older, understood master");

	masters_erase();
}

/* Bit rot is NOT foreignness. A prefix whose CRC fails could be anything, so
 * the other master stays authoritative and the store still mounts — this is
 * what the separate prefix CRC buys over a single header CRC. */
ZTEST(blob_db, test_corrupt_prefix_falls_back_to_the_good_master)
{
	uint64_t id = put_blob("SURVIVE", 7);

	zassert_ok(blob_db_unmount());

	/* Make A unambiguously authoritative, then wreck B's prefix. */
	struct master_img good;

	master_read_raw(0, &good);
	good.generation += 10;
	master_seal(&good);
	master_write_raw(0, &good);

	struct master_img rotten = good;

	rotten.generation = good.generation + 1;   /* would win if it parsed */
	master_seal(&rotten);
	rotten.prefix_crc16 ^= 0xffff;             /* ...but it does not */
	master_write_raw(1, &rotten);

	zassert_ok(blob_db_mount(), "one rotten master must not brick the store");

	uint8_t buf[8];
	size_t got;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_equal(got, 7);
	zassert_mem_equal(buf, "SURVIVE", 7);
}

/* An additive (minor) revision is readable: the fields we know sit at fixed
 * offsets and the CRC span is ours to compute, so a longer header written by
 * newer software still mounts. */
ZTEST(blob_db, test_future_minor_is_tolerated)
{
	uint64_t id = put_blob("MINOR", 5);

	zassert_ok(blob_db_unmount());

	struct master_img m;

	master_read_raw(0, &m);
	m.generation += 10;
	m.format_minor = 9;    /* far ahead of anything this build knows */
	m.hdr_len = sizeof(m) + 8;  /* and claiming trailing fields we lack */
	master_seal(&m);
	master_write_raw(0, &m);

	zassert_ok(blob_db_mount(), "an additive revision must still mount");

	uint8_t buf[8];
	size_t got;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_equal(got, 5);
	zassert_mem_equal(buf, "MINOR", 5);
}

/* Neither master parses and neither is erased. With the development default
 * this is recoverable — the store is reformatted rather than left dead. */
ZTEST(blob_db, test_both_masters_corrupt_autoformats)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT);

	uint64_t survivor = put_blob("STALE", 5);

	zassert_ok(blob_db_unmount());

	for (uint8_t slot = 0; slot < 2; slot++) {
		struct master_img m;

		master_read_raw(slot, &m);
		/* Ensure the sector is non-erased even if it never held a
		 * master, so this is "corrupt", not "virgin". */
		memcpy(m.magic, "BDMS", 4);
		m.hdr_len = sizeof(m);
		master_seal(&m);
		m.prefix_crc16 ^= 0xffff;
		master_write_raw(slot, &m);
	}

	zassert_ok(blob_db_mount(), "development default should reformat");

	/* Recovery rewrites the masters; it does NOT erase the buckets, so
	 * whatever they still hold stays readable. That is deliberate — the
	 * caller asked to recover, not to destroy — but it puts already-issued
	 * ids back in front of a freshly reset id counter, so the invariant
	 * that actually matters is that mount's defensive scan re-raised the
	 * ceiling past them. */
	zassert_true(blob_db_exists(BLOB_DB_ROOT_ID), "root must be live");
	zassert_true(blob_db_alloc_id() > survivor,
		     "reset id space must not re-issue a surviving id");
}

/* Same state, production policy: refuse and leave the partition for the
 * caller to deal with. Covered by the lib.blob_db.no_autoformat config. */
ZTEST(blob_db, test_both_masters_corrupt_refused_when_configured)
{
	Z_TEST_SKIP_IFDEF(CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT);

	put_blob("STALE", 5);
	zassert_ok(blob_db_unmount());

	struct master_img before[2];

	for (uint8_t slot = 0; slot < 2; slot++) {
		struct master_img m;

		master_read_raw(slot, &m);
		memcpy(m.magic, "BDMS", 4);
		m.hdr_len = sizeof(m);
		master_seal(&m);
		m.prefix_crc16 ^= 0xffff;
		master_write_raw(slot, &m);
		before[slot] = m;
	}

	zassert_equal(blob_db_mount(), -EIO,
		      "production policy must refuse, not reformat");

	for (uint8_t slot = 0; slot < 2; slot++) {
		struct master_img after;

		master_read_raw(slot, &after);
		zassert_mem_equal(&after, &before[slot], sizeof(after),
				  "refused mount modified master %u", slot);
	}

	masters_erase();
}

/* --- compaction crash recovery: the scratch seal --------------------------
 *
 * compact_commit() writes the compacted image to scratch, then seals it with a
 * trailer. recover_compaction() must trust the trailer, not the bucket header:
 * the header is at offset 0 of the image and therefore survives a tear that
 * lost everything after it.
 */

/* A torn scratch write must NOT be restored over an intact bucket. Before the
 * seal existed this lost every blob in the bucket past the tear. */
ZTEST(blob_db, test_torn_scratch_write_does_not_clobber_bucket)
{
	uint64_t id = put_blob("VICTIM", 6);
	const uint16_t n_buckets = 2045;          /* 2048 sectors - 3 metadata */
	const uint16_t bid = (uint16_t)(id % n_buckets);
	const off_t bucket_off = (off_t)(3 + bid) * TEST_SECTOR_SZ;
	const off_t scratch_off = 2 * TEST_SECTOR_SZ;

	zassert_ok(blob_db_unmount());

	/* Scratch: a valid bucket header and nothing else — exactly what a
	 * write torn just after the header leaves behind. No seal. */
	uint8_t bhdr[16];
	const struct flash_area *fa;

	zassert_ok(flash_area_open(BLOB_DB_TEST_PARTITION_ID, &fa));
	zassert_ok(flash_area_read(fa, bucket_off, bhdr, sizeof(bhdr)));
	zassert_ok(flash_area_erase(fa, scratch_off, TEST_SECTOR_SZ));
	zassert_ok(flash_area_write(fa, scratch_off, bhdr, sizeof(bhdr)));
	flash_area_close(fa);

	struct master_img m;

	master_read_raw(0, &m);
	m.generation += 100;
	m.state = 1;               /* COMPACTING */
	m.cbid = bid;
	master_seal(&m);
	master_write_raw(1, &m);

	zassert_ok(blob_db_mount());

	uint8_t buf[8];
	size_t got;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got),
		   "recovery restored a truncated scratch image over the bucket");
	zassert_equal(got, 6);
	zassert_mem_equal(buf, "VICTIM", 6);
}

/* The other half of the window: the image DID land and was sealed, and the
 * bucket was already erased when power went. Recovery must restore it. */
ZTEST(blob_db, test_sealed_scratch_is_restored_over_erased_bucket)
{
	uint64_t id = put_blob("RESTORE", 7);
	const uint16_t n_buckets = 2045;
	const uint16_t bid = (uint16_t)(id % n_buckets);
	const off_t bucket_off = (off_t)(3 + bid) * TEST_SECTOR_SZ;
	const off_t scratch_off = 2 * TEST_SECTOR_SZ;

	zassert_ok(blob_db_unmount());

	/* Copy the live bucket image into scratch, seal it, then erase the
	 * bucket — the state left by a crash between those two steps. */
	static uint8_t image[TEST_SECTOR_SZ];
	const struct flash_area *fa;

	zassert_ok(flash_area_open(BLOB_DB_TEST_PARTITION_ID, &fa));
	zassert_ok(flash_area_read(fa, bucket_off, image, sizeof(image)));

	/* Trim the trailing erased bytes: the image is everything up to the
	 * last non-0xff byte, which is what compaction would have written. */
	size_t image_len = sizeof(image);

	while (image_len > 16 && image[image_len - 1] == 0xff) {
		image_len--;
	}

	struct __packed {
		uint8_t  magic[4];
		uint32_t image_len;
		uint32_t image_crc32;
		uint32_t seal_crc32;
	} seal = {
		.image_len = (uint32_t)image_len,
		.image_crc32 = crc32_ieee(image, image_len),
	};
	BUILD_ASSERT(sizeof(seal) == 16, "seal layout drift");

	memcpy(seal.magic, "BDSL", 4);
	seal.seal_crc32 = crc32_ieee((const uint8_t *)&seal,
				     sizeof(seal) - sizeof(uint32_t));

	zassert_ok(flash_area_erase(fa, scratch_off, TEST_SECTOR_SZ));
	zassert_ok(flash_area_write(fa, scratch_off, image, image_len));
	zassert_ok(flash_area_write(fa, scratch_off + TEST_SECTOR_SZ - sizeof(seal),
				    &seal, sizeof(seal)));
	zassert_ok(flash_area_erase(fa, bucket_off, TEST_SECTOR_SZ));
	flash_area_close(fa);

	struct master_img m;

	master_read_raw(0, &m);
	m.generation += 100;
	m.state = 1;               /* COMPACTING */
	m.cbid = bid;
	master_seal(&m);
	master_write_raw(1, &m);

	zassert_ok(blob_db_mount());

	uint8_t buf[8];
	size_t got;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got),
		   "a sealed image was not restored over the erased bucket");
	zassert_equal(got, 7);
	zassert_mem_equal(buf, "RESTORE", 7);
}

#ifdef CONFIG_BLOB_DB_LARGE_PAYLOADS
/* --- large (segmented) objects ------------------------------------------
 *
 * An object too large for one slot becomes K segment slots plus an index slot
 * at the object's id, committed by writing the index last.
 */

#define TEST_N_BUCKETS  2045
#define TEST_SEG_FLAG   (1u << 3)
#define TEST_TOMB_FLAG  (1u << 1)
#define TEST_SEALED     (1u << 0)

/* Live segment slots across the whole partition.
 *
 * Each segment is written once and tombstoned at most once, so
 * (non-tombstone SEGMENT slots) - (tombstone SEGMENT slots) is the live count
 * without needing per-id state. This is what makes "no leaked segments"
 * testable at all — count() and iterate() deliberately cannot see them.
 */
static void count_segment_slots(int *out_live, int *out_tombs)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(BLOB_DB_TEST_PARTITION_ID, &fa));

	const size_t align = flash_area_align(fa);
	int live = 0, tombs = 0;

	for (uint16_t bid = 0; bid < TEST_N_BUCKETS; bid++) {
		const off_t base = (off_t)(3 + bid) * TEST_SECTOR_SZ;
		uint8_t bh[4];

		zassert_ok(flash_area_read(fa, base, bh, sizeof(bh)));
		if (memcmp(bh, "BDBH", 4) != 0) {
			continue;
		}

		off_t cursor = 16;

		for (;;) {
			struct __packed {
				uint8_t  flags;
				uint8_t  reserved;
				uint16_t val_len;
				uint64_t id;
			} head;

			if (cursor + 14 > TEST_SECTOR_SZ) {
				break;
			}
			zassert_ok(flash_area_read(fa, base + cursor, &head,
						   sizeof(head)));
			if (head.flags == 0xff || !(head.flags & TEST_SEALED) ||
			    head.val_len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
				break;
			}

			size_t ssz = 14 + head.val_len;

			ssz = (ssz + align - 1) & ~(align - 1);
			if (cursor + (off_t)ssz > TEST_SECTOR_SZ) {
				break;
			}
			if (head.flags & TEST_SEG_FLAG) {
				if (head.flags & TEST_TOMB_FLAG) {
					tombs++;
				} else {
					live++;
				}
			}
			cursor += ssz;
		}
	}

	flash_area_close(fa);
	if (out_live) {
		*out_live = live - tombs;
	}
	if (out_tombs) {
		*out_tombs = tombs;
	}
}

static int live_segment_slots(void)
{
	int live;

	count_segment_slots(&live, NULL);
	return live;
}

/* Segments released so far. A rewrite tombstones one id per segment it
 * replaces, so the delta across an operation is exactly how many segments that
 * operation rewrote — which "live count" cannot show, since every replacement
 * both adds and retires one. */
static int released_segment_slots(void)
{
	int tombs;

	count_segment_slots(NULL, &tombs);
	return tombs;
}

static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
	for (size_t i = 0; i < len; i++) {
		buf[i] = (uint8_t)((i * 31u + seed) ^ (i >> 8));
	}
}

#define BIG_LEN  (100u * 1024u)
static uint8_t big_src[BIG_LEN];
static uint8_t big_dst[BIG_LEN];

/* Round-trip through both access paths, and confirm it really was segmented. */
ZTEST(blob_db, test_large_object_roundtrip)
{
	fill_pattern(big_src, BIG_LEN, 0x11);

	uint64_t id = put_blob(big_src, BIG_LEN);

	zassert_true(live_segment_slots() > 1,
		     "a 100 KB object should have been split into segments");

	size_t n = 0;

	zassert_ok(blob_db_size(id, &n));
	zassert_equal(n, BIG_LEN, "size() must report the logical length");

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &n));
	zassert_equal(n, BIG_LEN);
	zassert_mem_equal(big_dst, big_src, BIG_LEN, "read() round-trip");

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_get(id, big_dst, BIG_LEN, &n));
	zassert_equal(n, BIG_LEN);
	zassert_mem_equal(big_dst, big_src, BIG_LEN, "get() round-trip");

	/* Survives a remount — the index and its segments are on flash. */
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &n));
	zassert_mem_equal(big_dst, big_src, BIG_LEN, "survives remount");
}

/* Partial reads: segment boundaries and windows that straddle them. */
ZTEST(blob_db, test_large_object_partial_reads)
{
	fill_pattern(big_src, BIG_LEN, 0x22);

	uint64_t id = put_blob(big_src, BIG_LEN);
	const size_t offsets[] = { 0, 1, 1023, 1024, 1025, 2047, 4096,
				   BIG_LEN / 2, BIG_LEN - 3 };

	for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
		const size_t off = offsets[i];
		const size_t want = MIN((size_t)3000, BIG_LEN - off);
		size_t got = 0;

		memset(big_dst, 0, want);
		zassert_ok(blob_db_read(id, off, big_dst, want, &got),
			   "read at offset %zu", off);
		zassert_equal(got, want, "short read at offset %zu", off);
		zassert_mem_equal(big_dst, big_src + off, want,
				  "wrong bytes at offset %zu", off);
	}

	/* At and past EOF: success with nothing copied, not an error. */
	size_t got = 1;

	zassert_ok(blob_db_read(id, BIG_LEN, big_dst, 16, &got));
	zassert_equal(got, 0);
	zassert_ok(blob_db_read(id, BIG_LEN + 999, big_dst, 16, &got));
	zassert_equal(got, 0);

	/* A window overlapping the end is shortened, not refused. */
	zassert_ok(blob_db_read(id, BIG_LEN - 10, big_dst, 100, &got));
	zassert_equal(got, 10);
}

/* Every rebind shape must leave exactly one generation of segments behind. */
ZTEST(blob_db, test_large_object_rebind_releases_old_segments)
{
	fill_pattern(big_src, BIG_LEN, 0x33);

	uint64_t id = put_blob(big_src, BIG_LEN);
	const int after_first = live_segment_slots();

	zassert_true(after_first > 1, "expected segments");

	/* large -> large */
	fill_pattern(big_src, BIG_LEN, 0x44);
	zassert_ok(blob_db_update(id, big_src, BIG_LEN));
	zassert_equal(live_segment_slots(), after_first,
		      "rebind leaked a generation of segments");

	size_t n;

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &n));
	zassert_mem_equal(big_dst, big_src, BIG_LEN, "rebound content");

	/* large -> small: every segment must go */
	zassert_ok(blob_db_update(id, "small", 5));
	zassert_equal(live_segment_slots(), 0,
		      "shrinking to inline left segments behind");

	uint8_t small[8];

	zassert_ok(blob_db_get(id, small, sizeof(small), &n));
	zassert_equal(n, 5);
	zassert_mem_equal(small, "small", 5);

	/* small -> large again */
	fill_pattern(big_src, BIG_LEN, 0x55);
	zassert_ok(blob_db_update(id, big_src, BIG_LEN));
	zassert_true(live_segment_slots() > 1, "regrew into segments");
	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &n));
	zassert_mem_equal(big_dst, big_src, BIG_LEN, "regrown content");
}

/* Deleting a large object drops its segments too. */
ZTEST(blob_db, test_large_object_delete_releases_segments)
{
	fill_pattern(big_src, BIG_LEN, 0x66);

	uint64_t id = put_blob(big_src, BIG_LEN);

	zassert_true(live_segment_slots() > 1, "expected segments");
	zassert_equal(blob_db_count(), 2, "root + one object");

	zassert_ok(blob_db_delete(id));
	zassert_equal(live_segment_slots(), 0, "delete left segments behind");
	zassert_equal(blob_db_count(), 1, "only the root should remain");
	zassert_false(blob_db_exists(id));
	zassert_equal(blob_db_read(id, 0, big_dst, 16, NULL), -ENOENT);
	zassert_equal(blob_db_size(id, &(size_t){ 0 }), -ENOENT);
}

/* Segments are storage, not content: count/iterate must not report them. */
ZTEST(blob_db, test_segments_are_invisible_to_count_and_iterate)
{
	fill_pattern(big_src, BIG_LEN, 0x77);

	uint64_t id = put_blob(big_src, BIG_LEN);
	const int segs = live_segment_slots();

	zassert_true(segs > 1, "expected segments");
	zassert_equal(blob_db_count(), 2,
		      "count() reported segments as objects");

	struct iter_ctx ctx = { 0 };

	zassert_ok(blob_db_iterate(iter_cb, &ctx));
	zassert_equal(ctx.hits, 2, "iterate() visited segments");

	bool saw_id = false;

	for (int i = 0; i < ctx.hits; i++) {
		saw_id |= (ctx.saw[i] == id);
	}
	zassert_true(saw_id, "the object itself must be visited");
}

/* An interrupted segmented write leaves unreferenced segments; mount sweeps
 * them. Forged directly: segments written for an owner that never got an index.
 */
ZTEST(blob_db, test_sweep_reclaims_orphan_segments)
{
	fill_pattern(big_src, BIG_LEN, 0x88);

	uint64_t id = put_blob(big_src, BIG_LEN);
	const int referenced = live_segment_slots();

	/* A second object whose index we then destroy, so its segments become
	 * orphans of a known owner. */
	uint64_t victim = put_blob(big_src, BIG_LEN);
	const int with_victim = live_segment_slots();

	zassert_true(with_victim > referenced, "victim added segments");

	zassert_ok(blob_db_unmount());

	/* Zero the victim's bucket magic so its index is unreachable, and point
	 * the master's sweep intent at it — the state a crash between writing
	 * segments and committing the index leaves. */
	const off_t vb = (off_t)(3 + (victim % TEST_N_BUCKETS)) * TEST_SECTOR_SZ;
	const struct flash_area *fa;
	uint8_t zeros[4] = { 0 };

	zassert_ok(flash_area_open(BLOB_DB_TEST_PARTITION_ID, &fa));
	zassert_ok(flash_area_write(fa, vb, zeros, sizeof(zeros)));
	flash_area_close(fa);

	struct master_img m;

	master_read_raw(0, &m);
	m.generation += 100;
	m.seg_owner = victim;
	master_seal(&m);
	master_write_raw(1, &m);

	zassert_ok(blob_db_mount(), "mount should sweep, not fail");

	zassert_equal(live_segment_slots(), referenced,
		      "sweep did not reclaim exactly the orphaned segments");

	/* The untouched object must be intact throughout. */
	size_t n;

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &n));
	zassert_equal(n, BIG_LEN);
	zassert_mem_equal(big_dst, big_src, BIG_LEN,
			  "sweep damaged an unrelated object");

	/* Idempotent: a second sweep of the same owner changes nothing. */
	zassert_ok(blob_db_unmount());
	master_read_raw(0, &m);
	m.generation += 100;
	m.seg_owner = victim;
	master_seal(&m);
	master_write_raw(1, &m);
	zassert_ok(blob_db_mount());
	zassert_equal(live_segment_slots(), referenced, "sweep not idempotent");
}
#endif /* CONFIG_BLOB_DB_LARGE_PAYLOADS */

#ifdef CONFIG_BLOB_DB_LARGE_PAYLOADS
/* --- partial writes (blob_db_write) ------------------------------------- */

/* Overwriting a few bytes must change the bytes and leave the rest alone. */
ZTEST(blob_db, test_pwrite_segmented_updates_range_only)
{
	fill_pattern(big_src, BIG_LEN, 0x91);

	uint64_t id = put_blob(big_src, BIG_LEN);
	const size_t offs[] = { 0, 7, 1023, 1024, 4096, BIG_LEN / 3,
				BIG_LEN - 5 };

	for (size_t i = 0; i < ARRAY_SIZE(offs); i++) {
		const size_t off = offs[i];
		const size_t n = MIN((size_t)1500, BIG_LEN - off);
		uint8_t patch[1500];

		for (size_t b = 0; b < n; b++) {
			patch[b] = (uint8_t)(0xA0 + ((off + b) & 0x0f));
		}

		zassert_ok(blob_db_write(id, off, patch, n),
			   "pwrite at %zu", off);
		memcpy(big_src + off, patch, n);   /* mirror the expectation */

		size_t got = 0;

		memset(big_dst, 0, BIG_LEN);
		zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &got));
		zassert_equal(got, BIG_LEN, "length changed by pwrite at %zu",
			      off);
		zassert_mem_equal(big_dst, big_src, BIG_LEN,
				  "pwrite at %zu disturbed other bytes", off);
	}
}

/* The point of the feature: a small write must not rewrite the object.
 * Counted in segments, since that is what a rewrite would replace. */
ZTEST(blob_db, test_pwrite_rewrites_only_touched_segments)
{
	fill_pattern(big_src, BIG_LEN, 0x92);

	uint64_t id = put_blob(big_src, BIG_LEN);
	const int segs = live_segment_slots();

	zassert_true(segs > 8, "expected a decent segment count, got %d", segs);

	/* Four bytes inside one segment: exactly one segment may be replaced. */
	const int released_before = released_segment_slots();
	const uint8_t four[4] = { 1, 2, 3, 4 };

	zassert_ok(blob_db_write(id, 5000, four, sizeof(four)));

	zassert_equal(released_segment_slots() - released_before, 1,
		      "pwrite of 4 bytes rewrote more than one segment");
	zassert_equal(live_segment_slots(), segs,
		      "pwrite changed the live segment count");

	memcpy(big_src + 5000, four, sizeof(four));

	size_t got;

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &got));
	zassert_equal(got, BIG_LEN);
	zassert_mem_equal(big_dst, big_src, BIG_LEN, "content after pwrite");

	/* The contrast that gives the number meaning: a whole-object update
	 * replaces every segment, so the same measurement returns K, not 1. */
	const int before_full = released_segment_slots();

	fill_pattern(big_src, BIG_LEN, 0x99);
	zassert_ok(blob_db_update(id, big_src, BIG_LEN));
	zassert_equal(released_segment_slots() - before_full, segs,
		      "a full update should have replaced all %d segments", segs);
}

/* Writing past the end extends, and the gap reads as zeros. */
ZTEST(blob_db, test_pwrite_extends_with_zero_fill)
{
	const size_t start = 4096;

	fill_pattern(big_src, start, 0x93);

	uint64_t id = put_blob(big_src, start);
	const uint8_t tail[4] = { 0xde, 0xad, 0xbe, 0xef };
	const size_t at = 20000;

	zassert_ok(blob_db_write(id, at, tail, sizeof(tail)));

	size_t n = 0;

	zassert_ok(blob_db_size(id, &n));
	zassert_equal(n, at + sizeof(tail), "extend did not grow the object");

	memset(big_dst, 0xaa, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, at + sizeof(tail), &n));
	zassert_equal(n, at + sizeof(tail));
	zassert_mem_equal(big_dst, big_src, start, "original bytes changed");
	for (size_t i = start; i < at; i++) {
		zassert_equal(big_dst[i], 0, "gap byte %zu not zero", i);
	}
	zassert_mem_equal(big_dst + at, tail, sizeof(tail), "tail bytes");
}

/* An inline payload is modified in place, and grown within its slot. */
ZTEST(blob_db, test_pwrite_inline_payload)
{
	uint64_t id = put_blob("hello world", 11);
	uint8_t buf[64];
	size_t n;

	zassert_ok(blob_db_write(id, 6, "WORLD", 5));
	zassert_ok(blob_db_get(id, buf, sizeof(buf), &n));
	zassert_equal(n, 11);
	zassert_mem_equal(buf, "hello WORLD", 11);

	/* Extend within the slot: the gap zero-fills. */
	zassert_ok(blob_db_write(id, 14, "X", 1));
	zassert_ok(blob_db_get(id, buf, sizeof(buf), &n));
	zassert_equal(n, 15);
	zassert_mem_equal(buf, "hello WORLD", 11);
	zassert_equal(buf[11], 0);
	zassert_equal(buf[12], 0);
	zassert_equal(buf[13], 0);
	zassert_equal(buf[14], 'X');

	/* Past a single slot: refused, with the object left untouched. */
	zassert_equal(blob_db_write(id, CONFIG_BLOB_DB_MAX_PAYLOAD_LEN, "y", 1),
		      -EFBIG, "inline growth past a slot must be refused");
	zassert_ok(blob_db_get(id, buf, sizeof(buf), &n));
	zassert_equal(n, 15, "refused pwrite altered the payload");
}

/* pwrite survives a remount, and the sweep does not mistake the segments it
 * left in place for orphans. */
ZTEST(blob_db, test_pwrite_survives_remount_and_sweep)
{
	fill_pattern(big_src, BIG_LEN, 0x94);

	uint64_t id = put_blob(big_src, BIG_LEN);
	const uint8_t patch[64] = { 0 };

	zassert_ok(blob_db_write(id, 3000, patch, sizeof(patch)));
	memset(big_src + 3000, 0, sizeof(patch));

	const int segs = live_segment_slots();

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	size_t got;

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &got));
	zassert_equal(got, BIG_LEN);
	zassert_mem_equal(big_dst, big_src, BIG_LEN, "content across remount");

	/* Force a sweep for this owner: every segment the new index names must
	 * survive it. */
	zassert_ok(blob_db_unmount());

	struct master_img m;

	master_read_raw(0, &m);
	m.generation += 100;
	m.seg_owner = id;
	master_seal(&m);
	master_write_raw(1, &m);

	zassert_ok(blob_db_mount());
	zassert_equal(live_segment_slots(), segs,
		      "sweep reclaimed segments the live index still names");

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &got));
	zassert_mem_equal(big_dst, big_src, BIG_LEN, "content after sweep");
}

/* --- index cache invalidation -------------------------------------------
 *
 * The cache holds one object's index record and, implicitly, g_seg_a. Every
 * test here warms it with a read and then does something that must drop it.
 * A stale hit is silent — it returns plausible, wrong bytes — so these assert
 * content, not just success.
 */

/* Warm the cache: any read of a segmented object leaves it holding that id. */
static void warm_cache(uint64_t id)
{
	size_t got = 0;

	zassert_ok(blob_db_read(id, 0, big_dst, 64, &got));
	zassert_equal(got, 64);
}

ZTEST(blob_db, test_index_cache_sees_an_update)
{
	fill_pattern(big_src, BIG_LEN, 0x11);

	uint64_t id = put_blob(big_src, BIG_LEN);

	warm_cache(id);

	fill_pattern(big_src, BIG_LEN, 0x77);
	zassert_ok(blob_db_update(id, big_src, BIG_LEN));

	size_t got = 0;

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &got));
	zassert_equal(got, BIG_LEN);
	zassert_mem_equal(big_dst, big_src, BIG_LEN,
			  "read served a stale cached index after update");
}

ZTEST(blob_db, test_index_cache_sees_a_partial_write)
{
	fill_pattern(big_src, BIG_LEN, 0x11);

	uint64_t id = put_blob(big_src, BIG_LEN);

	warm_cache(id);

	/* Land the write deliberately far from offset 0, in a later segment. */
	const size_t off = BIG_LEN - 1000;
	uint8_t patch[64];

	memset(patch, 0x5c, sizeof(patch));
	zassert_ok(blob_db_write(id, off, patch, sizeof(patch)));
	memcpy(big_src + off, patch, sizeof(patch));

	size_t got = 0;

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &got));
	zassert_mem_equal(big_dst, big_src, BIG_LEN,
			  "read served a stale cached index after pwrite");
}

ZTEST(blob_db, test_index_cache_sees_a_delete)
{
	fill_pattern(big_src, BIG_LEN, 0x11);

	uint64_t id = put_blob(big_src, BIG_LEN);

	warm_cache(id);
	zassert_ok(blob_db_delete(id));

	size_t got = 0;

	zassert_equal(blob_db_read(id, 0, big_dst, 64, &got), -ENOENT,
		      "a deleted object was still readable from cache");
	zassert_false(blob_db_exists(id));
}

/* Shrinking a segmented object to an inline one is the case a cache keyed on
 * "this id is segmented" gets wrong: the index record is gone, but a stale
 * entry would still route the read through segments that no longer exist. */
ZTEST(blob_db, test_index_cache_survives_segmented_to_inline)
{
	fill_pattern(big_src, BIG_LEN, 0x11);

	uint64_t id = put_blob(big_src, BIG_LEN);

	warm_cache(id);

	zassert_ok(blob_db_update(id, "small", 5));

	uint8_t buf[16];
	size_t got = 0;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_equal(got, 5, "want the inline payload, got %zu B", got);
	zassert_mem_equal(buf, "small", 5);

	/* blob_db_read() is the path with the cache fast path in front of it,
	 * so this is where a stale "id is segmented" entry does damage: it
	 * would route the read through segments that no longer exist and hand
	 * back plausible garbage. get() above cannot catch that. */
	memset(buf, 0, sizeof(buf));
	got = 0;
	zassert_ok(blob_db_read(id, 0, buf, sizeof(buf), &got));
	zassert_equal(got, 5, "read returned %zu B for a 5 B inline blob", got);
	zassert_mem_equal(buf, "small", 5, "read served a stale segmented index");

	zassert_equal(live_segment_slots(), 0,
		      "segments outlived the object that named them");
}

/* And the reverse: an inline blob the cache has never seen, grown past the
 * inline limit, must be read as segmented. */
ZTEST(blob_db, test_index_cache_survives_inline_to_segmented)
{
	uint64_t id = put_blob("small", 5);
	uint8_t buf[16];
	size_t got = 0;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));

	fill_pattern(big_src, BIG_LEN, 0x33);
	zassert_ok(blob_db_update(id, big_src, BIG_LEN));

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &got));
	zassert_equal(got, BIG_LEN);
	zassert_mem_equal(big_dst, big_src, BIG_LEN, "grown object misread");
}

/* Two objects alternating: a one-entry cache must evict rather than answer
 * for the wrong id. */
ZTEST(blob_db, test_index_cache_alternating_objects)
{
	const size_t half = BIG_LEN / 2;

	fill_pattern(big_src, half, 0x11);

	uint64_t a = put_blob(big_src, half);

	fill_pattern(big_src, half, 0x99);

	uint64_t b = put_blob(big_src, half);

	for (int i = 0; i < 3; i++) {
		size_t got = 0;

		memset(big_dst, 0, half);
		zassert_ok(blob_db_read(a, 0, big_dst, half, &got));
		fill_pattern(big_src, half, 0x11);
		zassert_mem_equal(big_dst, big_src, half, "A misread on pass %d", i);

		memset(big_dst, 0, half);
		zassert_ok(blob_db_read(b, 0, big_dst, half, &got));
		fill_pattern(big_src, half, 0x99);
		zassert_mem_equal(big_dst, big_src, half, "B misread on pass %d", i);
	}
}

/* A remount must not let a cache entry survive into a store that may have
 * been rewritten underneath it. */
ZTEST(blob_db, test_index_cache_dropped_across_remount)
{
	fill_pattern(big_src, BIG_LEN, 0x11);

	uint64_t id = put_blob(big_src, BIG_LEN);

	warm_cache(id);
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	size_t got = 0;

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &got));
	zassert_mem_equal(big_dst, big_src, BIG_LEN, "content after remount");
}

/* erase_all() invalidates bucket headers in place rather than by appending,
 * so it is the one mutation append_slot2() does not cover. */
ZTEST(blob_db, test_index_cache_dropped_by_erase_all)
{
	fill_pattern(big_src, BIG_LEN, 0x11);

	uint64_t id = put_blob(big_src, BIG_LEN);

	warm_cache(id);
	zassert_ok(blob_db_erase_all());

	size_t got = 0;

	zassert_equal(blob_db_read(id, 0, big_dst, 64, &got), -ENOENT,
		      "erase_all left a readable object in the cache");
}

#if defined(CONFIG_BLOB_DB_TEST_CRASH_HOOKS)
/* --- the §6.4 crash table -----------------------------------------------
 *
 * Segmentation's whole crash-safety claim rests on one assertion: step 3,
 * writing the index record, is the *single* commit point. Before it the new
 * object is invisible; at it the switch is atomic because it is one
 * slot-append (§4 latest-wins); after it everything remaining is garbage
 * collection. So a power cut at any step must leave the payload **wholly old
 * or wholly new**, never torn — and must leave **no unreferenced segment**
 * once the sweep has run.
 *
 * These tests cut the write at each step and then remount, which is a faithful
 * power cut: everything blob_db knows lives on flash, so dropping the mount
 * discards exactly what RAM would lose. The armed cut fires once, so recovery
 * itself runs unhooked.
 */

#define OLD_SEED  0x11
#define NEW_SEED  0xa5

/* Remount and report what survived: content wholly old or wholly new, and the
 * live segment count after the sweep. */
static void assert_wholly_old_or_new(uint64_t id, bool expect_new)
{
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());   /* runs the sweep if seg_owner is set */

	size_t got = 0;

	zassert_ok(blob_db_read(id, 0, big_dst, BIG_LEN, &got),
		   "object unreadable after recovery");
	zassert_equal(got, BIG_LEN, "length changed: got %zu", got);

	fill_pattern(big_src, BIG_LEN, expect_new ? NEW_SEED : OLD_SEED);
	zassert_mem_equal(big_dst, big_src, BIG_LEN,
			  "expected the %s payload, wholly",
			  expect_new ? "new" : "old");
}

/* The invariant that outlives every individual step: after recovery exactly
 * one object's worth of segments is live. Old and new payloads are the same
 * length, so whichever one survived, the count must match what a single
 * uncut write leaves — anything more is a leak, anything less is data loss. */
static void assert_no_orphan_segments(int expect_segs)
{
	zassert_equal(live_segment_slots(), expect_segs,
		      "want %d live segments after recovery, got %d",
		      expect_segs, live_segment_slots());
}

/* Establish an object, then rewrite it with a cut at `where`. */
static uint64_t seeded_object(int *out_segs)
{
	fill_pattern(big_src, BIG_LEN, OLD_SEED);

	uint64_t id = put_blob(big_src, BIG_LEN);

	*out_segs = live_segment_slots();
	zassert_true(*out_segs > 1, "precondition: object must be segmented");
	return id;
}

static void rewrite_cut_at(uint64_t id, enum blob_db_test_cut where)
{
	fill_pattern(big_src, BIG_LEN, NEW_SEED);
	blob_db_test_cut = where;
	zassert_equal(blob_db_update(id, big_src, BIG_LEN), -EINTR,
		      "the cut did not fire — hook misplaced?");
	zassert_equal(blob_db_test_cut, BLOB_DB_CUT_NONE,
		      "cut must disarm itself so recovery runs unhooked");
}

/* Step 1 — the sweep window is open but nothing else happened. Wholly old. */
ZTEST(blob_db, test_crash_after_owner_set_keeps_old)
{
	int segs;
	uint64_t id = seeded_object(&segs);

	rewrite_cut_at(id, BLOB_DB_CUT_AFTER_OWNER_SET);
	assert_wholly_old_or_new(id, false);
	assert_no_orphan_segments(segs);
}

/* Inside step 2 — some new segments exist, no index names them. Wholly old,
 * and the half-written segments must not survive the sweep. */
ZTEST(blob_db, test_crash_mid_segments_keeps_old_and_sweeps)
{
	int segs;
	uint64_t id = seeded_object(&segs);

	rewrite_cut_at(id, BLOB_DB_CUT_MID_SEGMENTS);
	assert_wholly_old_or_new(id, false);
	assert_no_orphan_segments(segs);
}

/* Step 2 complete — every new segment is on flash, but the index still points
 * at the old ones. This is the case that would tear if the commit were not a
 * single write. Wholly old. */
ZTEST(blob_db, test_crash_after_segments_before_commit_keeps_old)
{
	int segs;
	uint64_t id = seeded_object(&segs);

	rewrite_cut_at(id, BLOB_DB_CUT_AFTER_SEGMENTS);
	assert_wholly_old_or_new(id, false);
	assert_no_orphan_segments(segs);
}

/* Step 3 — the commit landed. Wholly new, and the *old* segments are now
 * unreferenced and must be swept. */
ZTEST(blob_db, test_crash_after_commit_takes_new_and_sweeps_old)
{
	int segs;
	uint64_t id = seeded_object(&segs);

	rewrite_cut_at(id, BLOB_DB_CUT_AFTER_COMMIT);
	assert_wholly_old_or_new(id, true);
	assert_no_orphan_segments(segs);
}

/* Step 4 — old segments released, seg_owner still set. Wholly new; the sweep
 * must be idempotent over already-released segments. */
ZTEST(blob_db, test_crash_after_release_takes_new)
{
	int segs;
	uint64_t id = seeded_object(&segs);

	rewrite_cut_at(id, BLOB_DB_CUT_AFTER_RELEASE);
	assert_wholly_old_or_new(id, true);
	assert_no_orphan_segments(segs);
}

/* The delete sequence has the same shape, and the same commit point: a cut
 * before the tombstone leaves the object intact, one after leaves it gone with
 * no segment left behind. */
ZTEST(blob_db, test_crash_during_delete_is_all_or_nothing)
{
	int segs;
	uint64_t id = seeded_object(&segs);

	/* Cut before the tombstone: the object survives whole. */
	blob_db_test_cut = BLOB_DB_CUT_AFTER_OWNER_SET;
	zassert_equal(blob_db_delete(id), -EINTR, "the cut did not fire");
	assert_wholly_old_or_new(id, false);
	assert_no_orphan_segments(segs);

	/* Cut immediately after it: the object is gone, and the sweep must
	 * reclaim every segment it used to name. */
	blob_db_test_cut = BLOB_DB_CUT_AFTER_COMMIT;
	zassert_equal(blob_db_delete(id), -EINTR, "the cut did not fire");

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	zassert_false(blob_db_exists(id), "delete committed; id must be gone");
	zassert_equal(live_segment_slots(), 0,
		      "sweep left %d segments of a deleted object",
		      live_segment_slots());
}
#endif /* CONFIG_BLOB_DB_TEST_CRASH_HOOKS */

/* Every large-object length in this suite — 4096 and BIG_LEN=102400 — is an
 * exact multiple of the 1024 B chunk stride, so until the review found it, no
 * test had ever produced a short final chunk. That is what let the extend bug
 * ship. This exercises the ragged-tail case across the ordinary operations,
 * not just the extend that exposed it. */
ZTEST(blob_db, test_large_object_with_a_ragged_tail)
{
	const size_t len = BIG_LEN - 7;   /* deliberately not a chunk multiple */

	fill_pattern(big_src, len, 0x5a);

	uint64_t id = put_blob(big_src, len);
	size_t got = 0;

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, len, &got));
	zassert_equal(got, len, "read %zu of %zu", got, len);
	zassert_mem_equal(big_dst, big_src, len, "ragged round-trip");

	zassert_ok(blob_db_size(id, &got));
	zassert_equal(got, len, "size %zu, want %zu", got, len);

	/* A partial write landing inside the short final chunk. */
	uint8_t patch[16];

	memset(patch, 0xc3, sizeof(patch));

	const size_t tail_off = len - sizeof(patch);

	zassert_ok(blob_db_write(id, tail_off, patch, sizeof(patch)));
	memcpy(big_src + tail_off, patch, sizeof(patch));

	/* And one spanning the boundary into it. */
	const size_t span_off = (len / 1024) * 1024 - 8;

	memset(patch, 0x7e, sizeof(patch));
	zassert_ok(blob_db_write(id, span_off, patch, sizeof(patch)));
	memcpy(big_src + span_off, patch, sizeof(patch));

	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, len, &got));
	zassert_equal(got, len);
	zassert_mem_equal(big_dst, big_src, len, "ragged tail after pwrite");

	/* Survives a remount, and leaves nothing behind when deleted. */
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	memset(big_dst, 0, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, len, &got));
	zassert_mem_equal(big_dst, big_src, len, "ragged tail after remount");

	zassert_ok(blob_db_delete(id));
	zassert_equal(live_segment_slots(), 0,
		      "delete left %d segments of a ragged object",
		      live_segment_slots());
}

/* === REVIEW REPRO 1: extend-past-end when the old last chunk is short ===
 * Same shape as test_pwrite_extends_with_zero_fill, but the object's length
 * is NOT a multiple of seg_len (1024 in this config), so the old last chunk
 * is short (4 bytes). The extending write does not touch that chunk, so
 * pwrite_segmented leaves it short — while the new index claims the object
 * now extends far past it. Every read crossing that chunk then fails. */
ZTEST(blob_db, test_review_pwrite_extend_short_last_chunk)
{
	const size_t start = 4100;   /* 4 full 1 KB chunks + 4 bytes */

	fill_pattern(big_src, start, 0x95);

	uint64_t id = put_blob(big_src, start);
	const uint8_t tail[4] = { 0xde, 0xad, 0xbe, 0xef };
	const size_t at = 20000;

	zassert_ok(blob_db_write(id, at, tail, sizeof(tail)));

	size_t n = 0;

	zassert_ok(blob_db_size(id, &n));
	zassert_equal(n, at + sizeof(tail), "extend did not grow the object");

	memset(big_dst, 0xaa, BIG_LEN);
	zassert_ok(blob_db_read(id, 0, big_dst, at + sizeof(tail), &n),
		   "extended object must be readable in full");
	zassert_equal(n, at + sizeof(tail));
	zassert_mem_equal(big_dst, big_src, start, "original bytes changed");
	for (size_t i = start; i < at; i++) {
		zassert_equal(big_dst[i], 0, "gap byte %zu not zero", i);
	}
	zassert_mem_equal(big_dst + at, tail, sizeof(tail), "tail bytes");
}

#if defined(CONFIG_BLOB_DB_TEST_CRASH_HOOKS)
/* === REVIEW REPRO 2: a failed segmented write leaks its segments for good ===
 * A segmented write that fails mid-flight (injected cut here; a real
 * -ENOSPC/-EIO mid-write takes the identical early-return path) leaves
 * orphan segments and seg_owner set. If the session then continues and any
 * later segmented op succeeds, seg_owner is overwritten and finally cleared,
 * so no mount ever sweeps the earlier orphans. Deleting both objects and
 * remounting twice must leave zero live segments; the orphan survives. */
ZTEST(blob_db, test_review_failed_write_orphans_never_swept)
{
	int segs;
	uint64_t id = seeded_object(&segs);

	/* Mid-write failure: one new segment written, then the op aborts. */
	rewrite_cut_at(id, BLOB_DB_CUT_MID_SEGMENTS);
	zassert_equal(live_segment_slots(), segs + 1,
		      "expected exactly one orphan segment");

	/* Session continues; an unrelated segmented write succeeds and clears
	 * the sweep intent. */
	fill_pattern(big_src, BIG_LEN, 0x77);

	uint64_t other = put_blob(big_src, BIG_LEN);

	/* Drop both objects, then remount (twice) so any sweep that is ever
	 * going to run has run. */
	zassert_ok(blob_db_delete(id));
	zassert_ok(blob_db_delete(other));
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	zassert_equal(live_segment_slots(), 0,
		      "orphan segment from the failed write was never swept");
}
#endif /* CONFIG_BLOB_DB_TEST_CRASH_HOOKS */
#endif /* CONFIG_BLOB_DB_LARGE_PAYLOADS */
