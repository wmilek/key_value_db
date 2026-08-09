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

	/* MAX+1 payload is rejected by update. */
	uint8_t over[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN + 1];
	uint64_t id = blob_db_alloc_id();

	zassert_equal(blob_db_update(id, over, sizeof(over)), -EINVAL);
}

/* 10. Corrupting a byte in a slot causes that slot (and everything after
 *     it in the same bucket) to drop out on next mount. A blob in a
 *     different bucket is unaffected. */
ZTEST(blob_db, test_corrupted_slot_truncates_bucket)
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
#define TEST_SECTOR_SZ  4096

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
	uint8_t  reserved1[32];
	uint32_t hdr_crc32;
};
BUILD_ASSERT(sizeof(struct master_img) == 64, "master layout drift");
BUILD_ASSERT(offsetof(struct master_img, prefix_crc16) == 10, "prefix drift");
BUILD_ASSERT(offsetof(struct master_img, hdr_crc32) == 60, "body drift");

/* The format this build of blob_db writes. Asserted rather than assumed, so
 * a version bump breaks these tests instead of quietly reinterpreting them. */
#define TEST_FORMAT_MAJOR  1

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
