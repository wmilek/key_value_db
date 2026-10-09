/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * kvhash spilling (doc/proposals/2026-10-09-kvhash-spill.md).
 *
 * The requirement these tests exist for: the inline thresholds are a write
 * policy, not a format. Changing them must never make a stored entry
 * unreadable, never change what a walk returns or in what order, and must
 * move an entry to the new placement only when its key is next set.
 *
 * Placement is not observable through map_ops, so it is read off
 * blob_db_count(): a spilled key or value is one blob of its own.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/shape_map.h>
#include <app/lib/containers/kvhash.h>

#define NEVER    32767u /* threshold that spills nothing */
#define N_KEYS   16
#define KMAX     160
#define VMAX     64

static const struct map_ops *ops = &kvhash_map_ops;

static void spill_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
	kvhash_set_inline_max(NEVER, NEVER);
	kvhash_test_fp_mask = 0xffffffffu;
}

static void spill_after(void *fixture)
{
	ARG_UNUSED(fixture);
	kvhash_set_inline_max(NEVER, NEVER);
	kvhash_test_fp_mask = 0xffffffffu;
	blob_db_unmount();
}

ZTEST_SUITE(kvhash_spill, NULL, NULL, spill_before, spill_after, NULL);

static uint64_t new_map(size_t expected)
{
	struct map_config cfg = { .expected_entries = expected };
	uint64_t root = blob_db_alloc_id();

	zassert_not_equal(root, 0);
	zassert_ok(ops->create(root, &cfg));
	return root;
}

/*
 * Key i and its value at generation g. Keys and values come in four sizes
 * each, so every threshold below splits the population differently.
 */
static const size_t klens[] = { 4, 12, 24, 40 };
static const size_t vlens[] = { 0, 8, 24, 48 };

static size_t make_key(uint8_t *k, int i)
{
	size_t kl = klens[i % 4];

	memset(k, 'a' + (i % 26), kl);
	snprintf((char *)k, kl, "%02d", i); /* distinct prefix; NUL overwritten */
	k[2 < kl ? 2 : kl - 1] = '-';
	return kl;
}

static size_t make_val(uint8_t *v, int i, int g)
{
	size_t vl = vlens[(i / 4 + g) % 4];

	for (size_t j = 0; j < vl; j++) {
		v[j] = (uint8_t)(i * 13 + g * 7 + j);
	}
	return vl;
}

static void put_all(uint64_t root, int g)
{
	uint8_t k[KMAX], v[VMAX];

	for (int i = 0; i < N_KEYS; i++) {
		size_t kl = make_key(k, i);
		size_t vl = make_val(v, i, g);

		zassert_ok(ops->set(root, k, kl, v, vl), "set %d (gen %d)", i, g);
	}
}

/* Every key reads back its value at gen[i]. */
static void check_all(uint64_t root, const int *gen)
{
	uint8_t k[KMAX], want[VMAX], got[VMAX];
	size_t len;

	for (int i = 0; i < N_KEYS; i++) {
		size_t kl = make_key(k, i);
		size_t vl = make_val(want, i, gen[i]);

		zassert_ok(ops->get(root, k, kl, got, sizeof(got), &len), "get %d", i);
		zassert_equal(len, vl, "key %d: length %zu, want %zu", i, len, vl);
		zassert_mem_equal(got, want, vl, "key %d: value", i);
	}
}

/* The order of a full walk, as key indices; also checks each value. */
struct order {
	size_t n;
	int idx[N_KEYS];
};

static void walk_order(uint64_t root, const int *gen, struct order *o)
{
	uint8_t cur[KMAX], k[KMAX], v[VMAX], want[VMAX], probe[KMAX];
	size_t clen = 0, kl, vl;
	int rc;

	o->n = 0;
	while ((rc = ops->next(root, cur, clen, k, sizeof(k), &kl,
			       v, sizeof(v), &vl)) == 0) {
		int hit = -1;

		for (int i = 0; i < N_KEYS; i++) {
			size_t pl = make_key(probe, i);

			if (pl == kl && memcmp(probe, k, kl) == 0) {
				hit = i;
			}
		}
		zassert_true(hit >= 0, "walk returned a foreign key");
		zassert_true(o->n < N_KEYS, "walk does not end");
		zassert_equal(vl, make_val(want, hit, gen[hit]), "walk value length");
		zassert_mem_equal(v, want, vl, "walk value of key %d", hit);
		o->idx[o->n++] = hit;
		memcpy(cur, k, kl);
		clen = kl;
	}
	zassert_equal(rc, -ENODATA);
	zassert_equal(o->n, N_KEYS, "walk returned %zu keys", o->n);
}

/*
 * The requirement itself. Write under one policy, then move through policies
 * that lower, raise, zero and disable each threshold. After every change,
 * before anything is rewritten, the map must read and walk exactly as before
 * and own exactly the same blobs (nothing migrated). Then half the keys are
 * rewritten under the new policy, and everything must still verify.
 */
ZTEST(kvhash_spill, test_thresholds_change_without_touching_stored_entries)
{
	static const size_t pol[][2] = {
		{ 16, 16 }, { 0, 0 }, { NEVER, NEVER }, { 30, 0 }, { 0, NEVER },
		{ 12, 24 }, { NEVER, 8 }, { 4, NEVER },
	};
	int gen[N_KEYS] = { 0 };
	struct order first, now;
	size_t baseline = blob_db_count();
	uint64_t root = new_map(200);

	kvhash_set_inline_max(pol[0][0], pol[0][1]);
	put_all(root, 0);
	check_all(root, gen);
	walk_order(root, gen, &first);

	for (size_t p = 1; p < ARRAY_SIZE(pol); p++) {
		size_t blobs = blob_db_count();
		size_t n = 0;

		kvhash_set_inline_max(pol[p][0], pol[p][1]);

		check_all(root, gen);
		walk_order(root, gen, &now);
		zassert_mem_equal(now.idx, first.idx, sizeof(first.idx),
				  "policy %zu: walk order changed", p);
		zassert_ok(ops->count(root, &n));
		zassert_equal(n, N_KEYS);
		zassert_equal(blob_db_count(), blobs,
			      "policy %zu: a threshold change moved data", p);

		/* Rewrite every other key under the new policy. */
		uint8_t k[KMAX], v[VMAX];

		for (int i = (int)(p % 2); i < N_KEYS; i += 2) {
			gen[i] = (int)p;
			size_t kl = make_key(k, i);
			size_t vl = make_val(v, i, gen[i]);

			zassert_ok(ops->set(root, k, kl, v, vl));
		}
		check_all(root, gen);
		walk_order(root, gen, &now);
		zassert_mem_equal(now.idx, first.idx, sizeof(first.idx),
				  "policy %zu: walk order changed after rewrite", p);
	}

	/* Back to all-inline and rewrite everything: still correct. Then
	 * destroy -- it releases only what the map references, so any blob a
	 * placement change orphaned would keep the count above its baseline. */
	kvhash_set_inline_max(NEVER, NEVER);
	for (int i = 0; i < N_KEYS; i++) {
		gen[i] = 99;
	}
	put_all(root, 99);
	check_all(root, gen);
	walk_order(root, gen, &now);
	zassert_mem_equal(now.idx, first.idx, sizeof(first.idx));

	zassert_ok(ops->destroy(root));
	zassert_equal(blob_db_count(), baseline,
		      "%zu blob(s) leaked across placement changes",
		      blob_db_count() - baseline);
}

/*
 * Lowering a threshold moves nothing; the next set of that key does. Raising
 * it back moves nothing either, until the key is set again -- and then the
 * blob the spilled value occupied is released.
 */
ZTEST(kvhash_spill, test_placement_follows_the_policy_only_on_rewrite)
{
	uint64_t root = new_map(8);
	uint8_t v[40], got[40];
	size_t len, base;

	memset(v, 0x5a, sizeof(v));
	zassert_ok(ops->set(root, "key", 3, v, sizeof(v)));
	base = blob_db_count();

	kvhash_set_inline_max(NEVER, 16);
	zassert_equal(blob_db_count(), base, "lowering moved data");
	zassert_ok(ops->get(root, "key", 3, got, sizeof(got), &len));
	zassert_equal(len, sizeof(v));

	v[0] = 1;
	zassert_ok(ops->set(root, "key", 3, v, sizeof(v)));
	zassert_equal(blob_db_count(), base + 1, "rewrite did not spill the value");
	zassert_ok(ops->get(root, "key", 3, got, sizeof(got), &len));
	zassert_mem_equal(got, v, sizeof(v));

	kvhash_set_inline_max(NEVER, NEVER);
	zassert_equal(blob_db_count(), base + 1, "raising moved data");
	zassert_ok(ops->get(root, "key", 3, got, sizeof(got), &len));
	zassert_mem_equal(got, v, sizeof(v));

	v[0] = 2;
	zassert_ok(ops->set(root, "key", 3, v, sizeof(v)));
	zassert_equal(blob_db_count(), base, "the spilled value was not released");
	zassert_ok(ops->get(root, "key", 3, got, sizeof(got), &len));
	zassert_mem_equal(got, v, sizeof(v));
}

/*
 * A spilled value rewritten at the same length is one blob update: the
 * bucket is not touched, so nothing is allocated or released. A spilled key
 * keeps its blob through any value change.
 */
ZTEST(kvhash_spill, test_spilled_parts_are_reused_where_they_can_be)
{
	uint64_t root = new_map(8);
	uint8_t v[32], got[32];
	const char *key = "a-key-long-enough-to-spill";
	size_t kl = strlen(key), len, base;

	kvhash_set_inline_max(8, 8);
	memset(v, 1, sizeof(v));
	zassert_ok(ops->set(root, key, kl, v, sizeof(v)));
	base = blob_db_count();

	for (int g = 2; g < 6; g++) {
		memset(v, g, sizeof(v));
		zassert_ok(ops->set(root, key, kl, v, sizeof(v)));
		zassert_equal(blob_db_count(), base, "same-length rewrite allocated");
	}
	zassert_ok(ops->get(root, key, kl, got, sizeof(got), &len));
	zassert_mem_equal(got, v, sizeof(v));

	/* A different length: fresh value blob, old one released, key kept. */
	zassert_ok(ops->set(root, key, kl, v, 20));
	zassert_equal(blob_db_count(), base);
	/* Short enough to go inline: the value blob goes, the key blob stays. */
	zassert_ok(ops->set(root, key, kl, v, 4));
	zassert_equal(blob_db_count(), base - 1);
	zassert_ok(ops->get(root, key, kl, got, sizeof(got), &len));
	zassert_equal(len, 4);
	zassert_mem_equal(got, v, 4);
}

/* get with a short buffer reports the true length of a spilled value. */
ZTEST(kvhash_spill, test_spilled_value_too_small_reports_true_length)
{
	uint64_t root = new_map(8);
	uint8_t v[48], got[8];
	size_t len = 0;

	kvhash_set_inline_max(NEVER, 0);
	memset(v, 7, sizeof(v));
	zassert_ok(ops->set(root, "k", 1, v, sizeof(v)));
	zassert_equal(ops->get(root, "k", 1, got, sizeof(got), &len), -ENOMEM);
	zassert_equal(len, sizeof(v));
	zassert_equal(ops->get(root, "k", 1, NULL, 0, &len), -ENOMEM);
	zassert_equal(len, sizeof(v));

	/* An empty value never spills, whatever the threshold. */
	size_t base = blob_db_count();

	zassert_ok(ops->set(root, "e", 1, NULL, 0));
	zassert_ok(ops->get(root, "e", 1, NULL, 0, &len));
	zassert_equal(len, 0);
	zassert_true(blob_db_count() <= base + 1, "an empty value spilled");
}

/* del releases what the entry spilled, and only that. */
ZTEST(kvhash_spill, test_del_releases_spilled_parts)
{
	uint64_t root = new_map(8);
	uint8_t v[32];
	const char *key = "spilled-key-0123456789";
	size_t kl = strlen(key), base;

	memset(v, 3, sizeof(v));
	zassert_ok(ops->set(root, "keep", 4, v, 4));
	base = blob_db_count();

	kvhash_set_inline_max(4, 4);
	zassert_ok(ops->set(root, key, kl, v, sizeof(v)));

	/* Two spilled parts, plus a bucket if the key landed in a fresh one. */
	size_t with = blob_db_count();

	zassert_true(with == base + 2 || with == base + 3,
		     "key and value should be blobs (%zu new)", with - base);

	zassert_ok(ops->del(root, key, kl));
	zassert_equal(blob_db_count(), with - 2, "del left spilled blobs behind");
	zassert_equal(ops->get(root, key, kl, v, sizeof(v), NULL), -ENOENT);
	zassert_ok(ops->get(root, "keep", 4, v, sizeof(v), NULL));
}

/*
 * kvhash's directory: [u32 magic][u16 n][u8 version][u8 depth][u64 id]*n.
 * A map created by v2 firmware must stay parseable by v2 firmware, so v3
 * code serves it but never writes a flagged entry into it.
 */
ZTEST(kvhash_spill, test_v2_map_is_served_but_never_spilled)
{
	uint64_t root = new_map(8);
	uint8_t dir[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t dlen = 0;

	zassert_ok(blob_db_get(root, dir, sizeof(dir), &dlen));
	zassert_equal(dir[6], 3, "new maps are v3");
	zassert_equal(dir[7], 1, "this test needs a one-level map");
	dir[6] = 2;
	zassert_ok(blob_db_update(root, dir, dlen));

	kvhash_set_inline_max(0, 0);

	uint8_t v[16], got[16];
	size_t len, base = blob_db_count();
	int buckets = 0;

	memset(v, 9, sizeof(v));
	for (int i = 0; i < 6; i++) {
		char k[8];

		snprintf(k, sizeof(k), "v2-%d", i);
		size_t before = blob_db_count();

		zassert_ok(ops->set(root, k, strlen(k), v, sizeof(v)));
		/* At most one new blob: a fresh bucket, never a spilled part. */
		zassert_true(blob_db_count() <= before + 1, "a v2 map spilled");
		buckets += (int)(blob_db_count() - before);
		zassert_ok(ops->get(root, k, strlen(k), got, sizeof(got), &len));
		zassert_mem_equal(got, v, sizeof(v));
	}
	zassert_equal(blob_db_count(), base + buckets);

	/* Still a v2 map afterwards. */
	zassert_ok(blob_db_get(root, dir, sizeof(dir), &dlen));
	zassert_equal(dir[6], 2);
}

/*
 * With every fingerprint forced equal, lookups, deletes and the walk order
 * have to tell spilled keys apart by their bytes -- including keys longer
 * than one compare chunk that differ only in their last byte, and spilled
 * keys sharing a bucket with inline keys of the same length.
 */
ZTEST(kvhash_spill, test_fingerprint_collisions_fall_back_to_bytes)
{
	enum { N = 8, LONG = 150 };
	uint64_t root = new_map(2);
	uint8_t k[LONG], got[8];
	size_t len;

	kvhash_test_fp_mask = 0;

	/* Long keys, identical but for the last byte: spilled. */
	kvhash_set_inline_max(0, NEVER);
	memset(k, 'L', sizeof(k));
	for (int i = 0; i < N; i++) {
		k[LONG - 1] = (uint8_t)('0' + i);
		zassert_ok(ops->set(root, k, LONG, &i, sizeof(i)), "long %d", i);
	}

	/* Short keys of one length, half spilled and half inline. */
	for (int i = 0; i < N; i++) {
		char s[8];

		kvhash_set_inline_max(i % 2 ? 0 : NEVER, NEVER);
		snprintf(s, sizeof(s), "s%05d", i);
		zassert_ok(ops->set(root, s, 6, &i, sizeof(i)), "short %d", i);
	}
	kvhash_set_inline_max(NEVER, NEVER);

	for (int i = 0; i < N; i++) {
		int out = -1;
		char s[8];

		k[LONG - 1] = (uint8_t)('0' + i);
		zassert_ok(ops->get(root, k, LONG, &out, sizeof(out), &len));
		zassert_equal(out, i, "long key %d read another's value", i);

		snprintf(s, sizeof(s), "s%05d", i);
		zassert_ok(ops->get(root, s, 6, &out, sizeof(out), &len));
		zassert_equal(out, i, "short key %d read another's value", i);
	}
	k[LONG - 1] = 'x';
	zassert_equal(ops->get(root, k, LONG, got, sizeof(got), &len), -ENOENT);

	/* Two walks: every key exactly once, in the same order both times. */
	static uint8_t order[2][2 * N][LONG];
	static size_t olen[2][2 * N];

	for (int w = 0; w < 2; w++) {
		uint8_t cur[LONG], ko[LONG];
		size_t clen = 0, kl, vl;
		int n = 0, v, rc;

		while ((rc = ops->next(root, cur, clen, ko, sizeof(ko), &kl,
				       &v, sizeof(v), &vl)) == 0) {
			zassert_true(n < 2 * N, "walk does not end");
			for (int j = 0; j < n; j++) {
				zassert_false(olen[w][j] == kl &&
					      memcmp(order[w][j], ko, kl) == 0,
					      "walk returned a key twice");
			}
			memcpy(order[w][n], ko, kl);
			olen[w][n++] = kl;
			memcpy(cur, ko, kl);
			clen = kl;
		}
		zassert_equal(rc, -ENODATA);
		zassert_equal(n, 2 * N, "walk returned %d of %d keys", n, 2 * N);
	}
	zassert_mem_equal(olen[0], olen[1], sizeof(olen[0]), "walk order unstable");
	zassert_mem_equal(order[0], order[1], sizeof(order[0]), "walk order unstable");

	/* Deleting by bytes removes exactly the named key. */
	k[LONG - 1] = '3';
	zassert_ok(ops->del(root, k, LONG));
	zassert_equal(ops->get(root, k, LONG, got, sizeof(got), &len), -ENOENT);
	k[LONG - 1] = '4';
	zassert_ok(ops->get(root, k, LONG, got, sizeof(got), &len));

	size_t n = 0;

	zassert_ok(ops->count(root, &n));
	zassert_equal(n, 2 * N - 1);
}

/* Spilled parts survive a remount, and destroy of an all-spilled map
 * releases every blob. */
ZTEST(kvhash_spill, test_spilled_map_survives_remount_and_destroys_clean)
{
	int gen[N_KEYS] = { 0 };
	struct order a, b;
	size_t baseline = blob_db_count();
	uint64_t root = new_map(200);

	kvhash_set_inline_max(0, 0);
	put_all(root, 0);
	walk_order(root, gen, &a);

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	kvhash_set_inline_max(NEVER, NEVER); /* a new build, a new policy */

	check_all(root, gen);
	walk_order(root, gen, &b);
	zassert_mem_equal(a.idx, b.idx, sizeof(a.idx));

	zassert_ok(ops->destroy(root));
	zassert_equal(blob_db_count(), baseline, "destroy left %zu blob(s)",
		      blob_db_count() - baseline);
}
