/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * kvhash detach-on-update (draft,
 * doc/proposals/2026-11-07-kvhash-detach-on-update.md).
 *
 * What the policy promises, and what these tests pin:
 *
 *   - an insert is placed exactly as without it (inline under the default
 *     thresholds), so a key written once costs what it did before;
 *   - the first set that replaces a value moves it into a blob of its own;
 *   - every later set of that key is ONE flash write, whatever the length;
 *   - reads, short buffers and the walk report the true length;
 *   - it is a write policy: off again, the next set re-inlines and releases
 *     the blob; a map created with it off (v3) is never detached;
 *   - del, destroy and remount neither leak nor lose a detached value.
 *
 * Placement is read off blob_db_count(), as in spill.c. Flash writes are
 * counted through the power-cut hook: arm it far away, run the operation,
 * and see how far it moved.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/shape_map.h>
#include <app/lib/containers/kvhash.h>

#define NEVER  32767u
#define FAR    100000

static const struct map_ops *ops = &kvhash_map_ops;

/* What every other suite runs under. */
#define DETACH_DEFAULT IS_ENABLED(CONFIG_BLOB_CONTAINER_KVHASH_DETACH_ON_UPDATE)
#if defined(CONFIG_BLOB_CONTAINER_KVHASH_DETACH_MIN_LEN)
#define DETACH_MIN_DEFAULT CONFIG_BLOB_CONTAINER_KVHASH_DETACH_MIN_LEN
#else
#define DETACH_MIN_DEFAULT 0
#endif

static void detach_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
	kvhash_set_inline_max(NEVER, NEVER);
	kvhash_set_detach_on_update(true);
	kvhash_set_detach_min_len(0);
	kvhash_test_cut_after = -1;
	kvhash_test_cut_fired = false;
}

static void detach_after(void *fixture)
{
	ARG_UNUSED(fixture);
	kvhash_set_inline_max(NEVER, NEVER);
	kvhash_set_detach_on_update(DETACH_DEFAULT);
	kvhash_set_detach_min_len(DETACH_MIN_DEFAULT);
	kvhash_test_cut_after = -1;
	blob_db_unmount();
}

ZTEST_SUITE(kvhash_detach, NULL, NULL, detach_before, detach_after, NULL);

static uint64_t new_map(size_t expected)
{
	struct map_config cfg = { .expected_entries = expected };
	uint64_t root = blob_db_alloc_id();

	zassert_not_equal(root, 0);
	zassert_ok(ops->create(root, &cfg));
	return root;
}

static uint8_t dir_version(uint64_t root)
{
	uint8_t dir[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t dlen = 0;

	zassert_ok(blob_db_get(root, dir, sizeof(dir), &dlen));
	return dir[6];
}

static void fill(uint8_t *v, size_t len, uint8_t tag)
{
	for (size_t i = 0; i < len; i++) {
		v[i] = (uint8_t)(tag + 3 * i);
	}
}

/* Set, and report how many flash writes kvhash issued for it. */
static int set_counting(uint64_t root, const char *k, const void *v, size_t vl,
			int *writes)
{
	kvhash_test_cut_after = FAR;
	int rc = ops->set(root, k, strlen(k), v, vl);

	*writes = FAR - kvhash_test_cut_after;
	kvhash_test_cut_after = -1;
	return rc;
}

static void expect(uint64_t root, const char *k, const void *v, size_t vl)
{
	uint8_t got[128];
	size_t len = 0;

	zassert_ok(ops->get(root, k, strlen(k), got, sizeof(got), &len), "get %s", k);
	zassert_equal(len, vl, "%s: length %zu, want %zu", k, len, vl);
	zassert_mem_equal(got, v, vl, "%s: value", k);
}

ZTEST(kvhash_detach, test_map_version_follows_the_policy_at_create)
{
	zassert_equal(dir_version(new_map(8)), 4, "detaching on: v4");
	kvhash_set_detach_on_update(false);
	zassert_equal(dir_version(new_map(8)), 3, "detaching off: v3, as before");
}

/*
 * The life of one key: inline at insert, detached at the first update, and
 * one write per update after that, through length changes in both directions
 * and down to an empty value.
 */
ZTEST(kvhash_detach, test_insert_inline_update_detached_then_one_write_each)
{
	uint64_t root = new_map(8);
	uint8_t v[100];
	int w;

	/* A neighbour in the same map, so a bucket rewrite would be visible. */
	fill(v, 40, 0x10);
	zassert_ok(ops->set(root, "neighbour", 9, v, 40));

	size_t base = blob_db_count();

	fill(v, 60, 0x20);
	zassert_ok(set_counting(root, "key", v, 60, &w));
	zassert_true(blob_db_count() <= base + 1, "insert spilled (only a fresh bucket allowed)");
	base = blob_db_count();
	expect(root, "key", v, 60);

	fill(v, 64, 0x30);
	zassert_ok(set_counting(root, "key", v, 64, &w));
	zassert_equal(blob_db_count(), base + 1, "first update did not detach");
	zassert_equal(w, 2, "first update: value blob + bucket, got %d writes", w);
	expect(root, "key", v, 64);

	static const size_t lens[] = { 64, 80, 7, 99, 0, 33 };

	for (size_t i = 0; i < ARRAY_SIZE(lens); i++) {
		fill(v, lens[i], (uint8_t)(0x40 + i));
		zassert_ok(set_counting(root, "key", v, lens[i], &w));
		zassert_equal(w, 1, "update to %zu B: %d writes, want 1", lens[i], w);
		zassert_equal(blob_db_count(), base + 1, "update to %zu B moved data", lens[i]);
		expect(root, "key", v, lens[i]);
	}

	fill(v, 40, 0x10);
	expect(root, "neighbour", v, 40);
}

ZTEST(kvhash_detach, test_short_buffer_reports_true_length)
{
	uint64_t root = new_map(8);
	uint8_t v[48], got[16];
	size_t len = 0;

	fill(v, sizeof(v), 1);
	zassert_ok(ops->set(root, "k", 1, v, 10));
	zassert_ok(ops->set(root, "k", 1, v, sizeof(v))); /* detached */

	zassert_equal(ops->get(root, "k", 1, got, sizeof(got), &len), -ENOMEM);
	zassert_equal(len, sizeof(v), "length %zu", len);

	uint8_t k[8];
	size_t kl = 0, vl = 0;

	zassert_equal(ops->next(root, NULL, 0, k, sizeof(k), &kl, got, sizeof(got), &vl),
		      -ENOMEM);
	zassert_equal(vl, sizeof(v), "walk length %zu", vl);

	uint8_t big[64];

	zassert_ok(ops->next(root, NULL, 0, k, sizeof(k), &kl, big, sizeof(big), &vl));
	zassert_equal(kl, 1);
	zassert_equal(vl, sizeof(v));
	zassert_mem_equal(big, v, sizeof(v));
}

/*
 * Mixed population: half the keys updated (detached), half not (inline).
 * get, walk and count agree; turning the policy off moves nothing until a key
 * is set again, and then releases its blob; destroy leaves nothing behind.
 */
/* Value lengths small enough for the suite's 256 B payload cap. */
#define VL0(i) (8 + (i) % 6)
#define VL1(i) (12 + (i) % 5)

ZTEST(kvhash_detach, test_policy_off_reinlines_on_rewrite_and_nothing_leaks)
{
	enum { N = 24 };
	size_t baseline = blob_db_count();
	struct map_config cfg = { .expected_entries = N, .typical_entry_bytes = 32 };
	uint64_t root = blob_db_alloc_id();
	uint8_t v[64];
	char k[16];
	int gen[N] = { 0 };

	zassert_ok(ops->create(root, &cfg));

	for (int i = 0; i < N; i++) {
		snprintf(k, sizeof(k), "dev-%02d", i);
		fill(v, VL0(i), (uint8_t)i);
		zassert_ok(ops->set(root, k, strlen(k), v, VL0(i)));
	}
	size_t inline_blobs = blob_db_count();

	for (int i = 0; i < N; i += 2) {
		snprintf(k, sizeof(k), "dev-%02d", i);
		gen[i] = 1;
		fill(v, VL1(i), (uint8_t)(i + 100));
		zassert_ok(ops->set(root, k, strlen(k), v, VL1(i)));
	}
	zassert_equal(blob_db_count(), inline_blobs + N / 2, "one blob per updated key");

	size_t n = 0;

	zassert_ok(ops->count(root, &n));
	zassert_equal(n, N);

	/* The walk sees every key with its current value. */
	uint8_t cur[16], kk[16], vv[64];
	size_t clen = 0, kl, vl, walked = 0;

	while (ops->next(root, cur, clen, kk, sizeof(kk), &kl, vv, sizeof(vv), &vl) == 0) {
		int i = (kk[4] - '0') * 10 + (kk[5] - '0');

		fill(v, gen[i] ? VL1(i) : VL0(i), (uint8_t)(gen[i] ? i + 100 : i));
		zassert_equal(vl, gen[i] ? VL1(i) : VL0(i), "walk length of %d", i);
		zassert_mem_equal(vv, v, vl, "walk value of %d", i);
		memcpy(cur, kk, kl);
		clen = kl;
		walked++;
	}
	zassert_equal(walked, N);

	/* Off: nothing moves until a key is set; then its blob goes. */
	kvhash_set_detach_on_update(false);
	zassert_equal(blob_db_count(), inline_blobs + N / 2);
	for (int i = 0; i < N; i += 2) {
		snprintf(k, sizeof(k), "dev-%02d", i);
		fill(v, VL1(i), (uint8_t)(i + 100));
		expect(root, k, v, VL1(i));
		fill(v, 9, (uint8_t)(i + 50));
		zassert_ok(ops->set(root, k, strlen(k), v, 9));
		expect(root, k, v, 9);
	}
	zassert_equal(blob_db_count(), inline_blobs, "re-inlined values not released");

	/* On again, detach some, delete some, then destroy: no leak. */
	kvhash_set_detach_on_update(true);
	for (int i = 0; i < N; i += 3) {
		snprintf(k, sizeof(k), "dev-%02d", i);
		zassert_ok(ops->set(root, k, strlen(k), v, 14));
	}
	for (int i = 0; i < N; i += 6) {
		snprintf(k, sizeof(k), "dev-%02d", i);
		zassert_ok(ops->del(root, k, strlen(k)));
	}
	zassert_ok(ops->destroy(root));
	zassert_equal(blob_db_count(), baseline, "%zu blob(s) leaked",
		      blob_db_count() - baseline);
}

ZTEST(kvhash_detach, test_v3_map_is_never_detached)
{
	kvhash_set_detach_on_update(false);
	uint64_t root = new_map(8);

	kvhash_set_detach_on_update(true);

	uint8_t v[40];
	size_t base;

	fill(v, sizeof(v), 7);
	zassert_ok(ops->set(root, "k", 1, v, sizeof(v)));
	base = blob_db_count();
	for (int i = 0; i < 4; i++) {
		v[0] = (uint8_t)i;
		zassert_ok(ops->set(root, "k", 1, v, sizeof(v)));
		zassert_equal(blob_db_count(), base, "a v3 map detached a value");
		expect(root, "k", v, sizeof(v));
	}
	zassert_equal(dir_version(root), 3);
}

ZTEST(kvhash_detach, test_detached_values_survive_remount)
{
	uint64_t root = new_map(8);
	uint8_t v[50];

	fill(v, 10, 1);
	zassert_ok(ops->set(root, "a", 1, v, 10));
	fill(v, 50, 2);
	zassert_ok(ops->set(root, "a", 1, v, 50));
	fill(v, 11, 3);
	zassert_ok(ops->set(root, "a", 1, v, 11));

	zassert_ok(blob_db_unmount());
	kvhash_set_detach_on_update(false); /* a reader needs no policy */
	zassert_ok(blob_db_mount());
	expect(root, "a", v, 11);
}

/* A value spilled by the thresholds (it records its length) becomes detached
 * at its next length-changing set, and is one write per set from then on. */
ZTEST(kvhash_detach, test_threshold_spilled_value_converts)
{
	uint64_t root = new_map(8);
	uint8_t v[64];
	size_t base;
	int w;

	kvhash_set_inline_max(NEVER, 16);
	fill(v, 32, 1);
	zassert_ok(ops->set(root, "s", 1, v, 32)); /* insert: spilled, sized */
	base = blob_db_count();

	fill(v, 32, 2);
	zassert_ok(set_counting(root, "s", v, 32, &w));
	zassert_equal(w, 1, "same-length spilled update is one write");

	fill(v, 40, 3);
	zassert_ok(set_counting(root, "s", v, 40, &w));
	zassert_equal(blob_db_count(), base, "the old value blob was not released");
	expect(root, "s", v, 40);

	fill(v, 20, 4);
	zassert_ok(set_counting(root, "s", v, 20, &w));
	zassert_equal(w, 1, "detached now: one write at a new length, got %d", w);
	expect(root, "s", v, 20);
}

/* A short update stays inline; once a value is detached, a short one does
 * not bring it back. */
ZTEST(kvhash_detach, test_short_updates_stay_inline)
{
	uint64_t root = new_map(8);
	uint8_t v[40];
	size_t base;
	int w;

	kvhash_set_detach_min_len(16);
	fill(v, 8, 1);
	zassert_ok(ops->set(root, "k", 1, v, 8));
	base = blob_db_count();

	fill(v, 15, 2);
	zassert_ok(ops->set(root, "k", 1, v, 15));
	zassert_equal(blob_db_count(), base, "a 15 B update detached at min 16");
	expect(root, "k", v, 15);

	fill(v, 16, 3);
	zassert_ok(ops->set(root, "k", 1, v, 16));
	zassert_equal(blob_db_count(), base + 1, "a 16 B update stayed inline");

	fill(v, 2, 4);
	zassert_ok(set_counting(root, "k", v, 2, &w));
	zassert_equal(w, 1, "a detached value stays detached when it shrinks");
	zassert_equal(blob_db_count(), base + 1);
	expect(root, "k", v, 2);
}
