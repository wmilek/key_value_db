/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * The slot index as a cache (CONFIG_BLOB_DB_SLOT_INDEX_CLOCK).
 *
 * Correctness is the rest of the suite's job: in the slot_index.clock
 * scenarios every indexed answer is cross-checked against a walk
 * (CONFIG_BLOB_DB_SLOT_INDEX_VERIFY) while a 16-entry table evicts all the
 * time. This file checks the policy: ids looked up often stay indexed through
 * any amount of cold traffic -- ids written once, ids looked up once, deletes,
 * and compactions of the hot ids' own buckets -- so a hot lookup keeps
 * costing what a hit costs, not a walk.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_iostats.h>
#include <app/lib/blob_db_inspect.h>

#define N_HOT   4
#define N_COLD  400
#define DEPTH   40   /* slots per hot bucket: a walk costs about this */

static void clock_before(void *fixture)
{
	ARG_UNUSED(fixture);
	if (!IS_ENABLED(CONFIG_BLOB_DB_SLOT_INDEX_CLOCK)) {
		ztest_test_skip();
	}
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
}

static void clock_after(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
}

ZTEST_SUITE(blob_db_sidx_clock, NULL, NULL, clock_before, clock_after, NULL);

static uint32_t reads_of_get(uint64_t id, uint8_t *buf, size_t sz)
{
	struct blob_db_iostats a, b;
	size_t len;

	blob_db_iostats_get(&a);
	zassert_ok(blob_db_get(id, buf, sz, &len), "get %llu", (unsigned long long)id);
	blob_db_iostats_get(&b);
	return b.reads - a.reads;
}

ZTEST(blob_db_sidx_clock, test_hot_ids_survive_cold_traffic)
{
	uint64_t hot[N_HOT];
	uint8_t v[32], buf[32];

	/* Hot ids, each rewritten until its bucket holds DEPTH slots, so a
	 * lookup that walks is clearly dearer than a hit. */
	for (int h = 0; h < N_HOT; h++) {
		hot[h] = blob_db_alloc_id();
		zassert_not_equal(hot[h], 0);
	}
	for (int r = 0; r < DEPTH; r++) {
		for (int h = 0; h < N_HOT; h++) {
			memset(v, r + h, sizeof(v));
			zassert_ok(blob_db_update(hot[h], v, sizeof(v)));
		}
	}

	/* What a walk of a hot bucket would cost: one header read per slot.
	 * A hit costs a header, the payload and its CRC; look each hot id up
	 * twice so the second is a hit whatever the first was. */
	struct blob_db_inspect_info ii;
	uint32_t hit = 0, walk = UINT32_MAX;

	zassert_ok(blob_db_inspect_info_get(&ii));
	for (int h = 0; h < N_HOT; h++) {
		struct blob_db_inspect_bucket bi;

		zassert_ok(blob_db_inspect_bucket_get((uint16_t)(hot[h] % ii.n_buckets),
						      &bi, NULL, NULL));
		walk = MIN(walk, (uint32_t)bi.objects + bi.superseded + bi.tombstones);
		(void)reads_of_get(hot[h], buf, sizeof(buf));
		hit = MAX(hit, reads_of_get(hot[h], buf, sizeof(buf)));
	}
	if (!IS_ENABLED(CONFIG_BLOB_DB_SLOT_INDEX_VERIFY)) {
		zassert_true(walk > 4 * hit, "walk %u reads, hit %u: test not sensitive",
			     walk, hit);
	}

	/* Cold traffic, many times the table: each cold id is written, looked
	 * up once (which learns it, and so must evict), and half are deleted.
	 * The hot ids are looked up between every cold step, and rewritten now
	 * and then, so their buckets also compact. */
	uint32_t worst = 0;

	for (int c = 0; c < N_COLD; c++) {
		uint64_t id = blob_db_alloc_id();

		zassert_not_equal(id, 0);
		memset(v, 0xc0 ^ c, sizeof(v));
		zassert_ok(blob_db_update(id, v, sizeof(v)));
		zassert_ok(blob_db_get(id, buf, sizeof(buf), NULL));
		if (c % 2) {
			zassert_ok(blob_db_delete(id));
		}
		if (c % 7 == 0) {
			memset(v, c, sizeof(v));
			zassert_ok(blob_db_update(hot[c % N_HOT], v, sizeof(v)));
			/* A rewrite refreshes the entry; the next get is a hit. */
		}
		for (int h = 0; h < N_HOT; h++) {
			uint32_t r = reads_of_get(hot[h], buf, sizeof(buf));

			if (c >= N_HOT) {   /* past the warm-up */
				worst = MAX(worst, r);
			}
		}
	}

	if (!IS_ENABLED(CONFIG_BLOB_DB_SLOT_INDEX_VERIFY)) {
		zassert_true(worst <= hit, "a hot lookup cost %u reads (hit %u, walk %u): "
			     "a hot id was evicted", worst, hit, walk);
	}

	/* And the values are right. */
	for (int h = 0; h < N_HOT; h++) {
		size_t len;

		zassert_ok(blob_db_get(hot[h], buf, sizeof(buf), &len));
		zassert_equal(len, sizeof(v));
	}
}
