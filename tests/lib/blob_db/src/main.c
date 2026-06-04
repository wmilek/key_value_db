/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite for blob_db. Cases mirror design §14.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>

#define BLOB_DB_TEST_PARTITION_ID  PARTITION_ID(storage_partition)

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

/* 1. Fresh erase, mount OK, count == 0. */
ZTEST(blob_db, test_mount_empty_formats_partition)
{
	zassert_equal(blob_db_count(), 0);
}

/* 2. Single blob round-trip with NUL and 0xff bytes inside. */
ZTEST(blob_db, test_put_get_roundtrip)
{
	const uint8_t pl[] = { 'h', 'e', 0x00, 'l', 0xff, 'o' };
	uint64_t id;

	zassert_ok(blob_db_put(pl, sizeof(pl), &id));

	uint8_t buf[16];
	size_t got;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_equal(got, sizeof(pl));
	zassert_mem_equal(buf, pl, sizeof(pl));
}

/* 3. First put returns id=1; ids are monotonic. */
ZTEST(blob_db, test_put_returns_id_1_first_then_monotonic)
{
	uint64_t a, b, c;

	zassert_ok(blob_db_put("x", 1, &a));
	zassert_equal(a, 1, "first put returned id=%llu, want 1",
		      (unsigned long long)a);
	zassert_ok(blob_db_put("y", 1, &b));
	zassert_equal(b, 2);
	zassert_ok(blob_db_put("z", 1, &c));
	zassert_equal(c, 3);
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
	uint64_t id;

	zassert_ok(blob_db_put("v1", 2, &id));
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
	uint64_t id;

	zassert_ok(blob_db_put("v", 1, &id));
	zassert_true(blob_db_exists(id));

	zassert_ok(blob_db_delete(id));
	zassert_false(blob_db_exists(id));

	uint8_t buf[16];
	size_t got;

	zassert_equal(blob_db_get(id, buf, sizeof(buf), &got), -ENOENT);
	zassert_equal(blob_db_delete(id), -ENOENT);
	zassert_equal(blob_db_delete(9999), -ENOENT);
}

/* 7. update on a deleted id returns -ENOENT (strict semantics). */
ZTEST(blob_db, test_update_after_delete_returns_enoent)
{
	uint64_t id;

	zassert_ok(blob_db_put("v", 1, &id));
	zassert_ok(blob_db_delete(id));
	zassert_equal(blob_db_update(id, "v2", 2), -ENOENT);
}

/* 8. Unmount/mount preserves all live blobs. */
ZTEST(blob_db, test_persistence_across_remount)
{
	uint64_t a, b, c;

	zassert_ok(blob_db_put("aa", 2, &a));
	zassert_ok(blob_db_put("bb", 2, &b));
	zassert_ok(blob_db_put("cc", 2, &c));
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

	zassert_equal(blob_db_count(), 2);
}

/* 9. Payload length boundaries: 0 (allowed), MAX (allowed), MAX+1 (rejected). */
ZTEST(blob_db, test_boundary_payload_len)
{
	uint64_t empty_id;

	zassert_ok(blob_db_put(NULL, 0, &empty_id));

	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t got;

	zassert_ok(blob_db_get(empty_id, buf, sizeof(buf), &got));
	zassert_equal(got, 0);

	uint8_t big[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];

	for (size_t i = 0; i < sizeof(big); i++) {
		big[i] = (uint8_t)(i ^ 0x5a);
	}
	uint64_t big_id;

	zassert_ok(blob_db_put(big, sizeof(big), &big_id));

	uint8_t back[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];

	zassert_ok(blob_db_get(big_id, back, sizeof(back), &got));
	zassert_equal(got, sizeof(big));
	zassert_mem_equal(back, big, sizeof(big));

	uint8_t over[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN + 1];
	uint64_t rejected;

	zassert_equal(blob_db_put(over, sizeof(over), &rejected), -EINVAL);
}

/* 10. Corrupting a byte in a slot causes that slot (and everything after
 *     it in the same bucket) to drop out on next mount. A blob in a
 *     different bucket is unaffected. */
ZTEST(blob_db, test_corrupted_slot_truncates_bucket)
{
	uint64_t a, b;

	zassert_ok(blob_db_put("AAA", 3, &a));
	zassert_ok(blob_db_put("BBB", 3, &b));

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
	uint64_t id;

	zassert_ok(blob_db_put("v0000", 5, &id));

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
	uint64_t id;

	zassert_ok(blob_db_put("v1", 2, &id));
	zassert_ok(blob_db_update(id, "v2", 2));
	zassert_ok(blob_db_delete(id));

	/* count() does latest-wins dedup, so the tombstoned id is already
	 * invisible — even pre-compaction. */
	zassert_equal(blob_db_count(), 0);

	/* Force a compaction in this id's bucket via many updates to an id
	 * that lives in the same bucket. With sequential ids and round-robin
	 * hashing, id and id+N share a bucket. So we put id+N and update it. */
	uint64_t other = 0;

	for (int i = 0; i < 2045; i++) {
		uint64_t tmp;

		zassert_ok(blob_db_put("x", 1, &tmp));
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

		zassert_ok(blob_db_put(p, n, &ids[i]));
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
ZTEST(blob_db, test_mid_compaction_crash_recovery)
{
	uint64_t a, b;

	zassert_ok(blob_db_put("AA", 2, &a));
	zassert_ok(blob_db_put("BB", 2, &b));

	zassert_ok(blob_db_unmount());

	const struct flash_area *fa;

	zassert_ok(flash_area_open(BLOB_DB_TEST_PARTITION_ID, &fa));

	/* Read current master A to learn its gen. */
	struct master_hdr_pkt {
		uint8_t  magic[4];
		uint32_t generation;
		uint8_t  state;
		uint16_t cbid;
		uint8_t  reserved;
		uint64_t next_id_hint;
		uint32_t crc;
	} __packed mhdr;

	zassert_ok(flash_area_read(fa, 0, &mhdr, sizeof(mhdr)));

	/* Build a COMPACTING master with much higher gen, written to slot B. */
	struct master_hdr_pkt injected = mhdr;

	injected.magic[0] = 'B';
	injected.magic[1] = 'D';
	injected.magic[2] = 'M';
	injected.magic[3] = 'S';
	injected.generation = mhdr.generation + 100;
	injected.state = 1;          /* COMPACTING */
	injected.cbid = 42;          /* arbitrary bucket id */
	injected.reserved = 0;
	/* keep next_id_hint */
	injected.crc = crc32_ieee((uint8_t *)&injected, 20);

	zassert_ok(flash_area_erase(fa, 4096, 4096));   /* master B sector */
	zassert_ok(flash_area_write(fa, 4096, &injected, sizeof(injected)));

	flash_area_close(fa);

	zassert_ok(blob_db_mount(), "mount should run recover_compaction");

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

	zassert_ok(blob_db_put("a", 1, &ids[0]));
	zassert_ok(blob_db_put("b", 1, &ids[1]));
	zassert_ok(blob_db_put("c", 1, &ids[2]));
	zassert_ok(blob_db_put("d", 1, &ids[3]));
	zassert_ok(blob_db_put("e", 1, &ids[4]));

	zassert_ok(blob_db_delete(ids[1]));
	zassert_ok(blob_db_update(ids[3], "DD", 2));

	struct iter_ctx c = { 0 };

	zassert_ok(blob_db_iterate(iter_cb, &c));
	zassert_equal(c.hits, 4, "expected 4 live blobs, got %d", c.hits);

	for (int i = 0; i < c.hits; i++) {
		zassert_not_equal(c.saw[i], ids[1],
				  "iterate emitted a deleted id");
	}
}
