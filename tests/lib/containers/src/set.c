/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite for the L2 Set shape (set_ops).
 *
 * Like map_contract, written against the shape: every test in `set_contract`
 * loops over `set_providers[]`, so the native kvhash set (proposal step 3)
 * is covered by adding one row. `set_over_map` holds what is specific to the
 * adapter: how it treats a root that is a map with values in it.
 *
 * See doc/proposals/2026-10-31-set-shape.md §6.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_iostats.h>
#include <app/lib/containers/shape_set.h>
#include <app/lib/containers/set_over_map.h>
#include <app/lib/containers/kvhash.h>

struct set_provider {
	const char *name;
	const struct set_ops *ops;
};

static const struct set_provider set_providers[] = {
	{ "set_over_kvhash", &set_over_kvhash_ops },
};

#define FOR_EACH_SET(p)                                                        \
	for (const struct set_provider *p = set_providers;                     \
	     p < set_providers + ARRAY_SIZE(set_providers); p++)

static void set_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
}

static void set_after(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
}

ZTEST_SUITE(set_contract, NULL, NULL, set_before, set_after, NULL);
ZTEST_SUITE(set_over_map, NULL, NULL, set_before, set_after, NULL);

static uint64_t fresh_set(const struct set_provider *p, size_t expected)
{
	struct set_config cfg = { .expected_members = expected };
	uint64_t root = blob_db_alloc_id();

	zassert_not_equal(root, 0, "%s: alloc_id failed", p->name);
	zassert_ok(p->ops->create(root, &cfg), "%s: create", p->name);
	return root;
}

static size_t members(const struct set_provider *p, uint64_t root)
{
	size_t n = SIZE_MAX;

	zassert_ok(p->ops->count(root, &n), "%s: count", p->name);
	return n;
}

/* Members m000..m999: four bytes. @p i stays below 1000. */
static void member_name(char *buf, size_t sz, int i)
{
	snprintf(buf, sz, "m%03u", (unsigned int)i % 1000u);
}

/* ---- the contract ------------------------------------------------- */

ZTEST(set_contract, test_op_vector_is_complete)
{
	FOR_EACH_SET(p) {
		zassert_not_null(p->ops->create, "%s", p->name);
		zassert_not_null(p->ops->stat, "%s", p->name);
		zassert_not_null(p->ops->has, "%s", p->name);
		zassert_not_null(p->ops->add, "%s", p->name);
		zassert_not_null(p->ops->del, "%s", p->name);
		zassert_not_null(p->ops->next, "%s", p->name);
		zassert_not_null(p->ops->count, "%s", p->name);
		zassert_not_null(p->ops->destroy, "%s", p->name);
	}
}

ZTEST(set_contract, test_create_yields_an_empty_set)
{
	FOR_EACH_SET(p) {
		uint64_t root = fresh_set(p, 8);
		char k[8];
		size_t kl = 0;

		zassert_equal(p->ops->has(root, "k", 1), -ENOENT, "%s", p->name);
		zassert_equal(p->ops->del(root, "k", 1), -ENOENT, "%s", p->name);
		zassert_equal(p->ops->next(root, NULL, 0, k, sizeof(k), &kl),
			      -ENODATA, "%s", p->name);
		zassert_equal(members(p, root), 0, "%s", p->name);
	}
}

ZTEST(set_contract, test_create_accepts_null_config_and_stat_reports)
{
	FOR_EACH_SET(p) {
		uint64_t root = blob_db_alloc_id();
		struct set_info info;

		zassert_ok(p->ops->create(root, NULL), "%s", p->name);
		zassert_ok(p->ops->stat(root, &info), "%s", p->name);
		zassert_true(info.depth >= 1, "%s", p->name);
		zassert_true(info.buckets >= 1, "%s", p->name);
		zassert_true(info.key_bytes_limit > 0, "%s", p->name);
		zassert_equal(p->ops->stat(root, NULL), -EINVAL, "%s", p->name);
		zassert_ok(p->ops->add(root, "x", 1), "%s", p->name);
	}
}

ZTEST(set_contract, test_add_has_del_roundtrip)
{
	FOR_EACH_SET(p) {
		uint64_t root = fresh_set(p, 8);

		zassert_ok(p->ops->add(root, "door", 4), "%s", p->name);
		zassert_ok(p->ops->has(root, "door", 4), "%s", p->name);
		zassert_equal(p->ops->has(root, "doo", 3), -ENOENT,
			      "%s: a prefix is not a member", p->name);
		zassert_equal(p->ops->has(root, "doors", 5), -ENOENT,
			      "%s: an extension is not a member", p->name);
		zassert_ok(p->ops->del(root, "door", 4), "%s", p->name);
		zassert_equal(p->ops->has(root, "door", 4), -ENOENT, "%s", p->name);
		zassert_equal(p->ops->del(root, "door", 4), -ENOENT, "%s", p->name);
	}
}

/* No duplicates: the second add is -EEXIST and the set holds one member. */
ZTEST(set_contract, test_repeated_add_is_eexist)
{
	FOR_EACH_SET(p) {
		uint64_t root = fresh_set(p, 8);

		zassert_ok(p->ops->add(root, "lift", 4), "%s", p->name);
		zassert_equal(p->ops->add(root, "lift", 4), -EEXIST, "%s", p->name);
		zassert_equal(p->ops->add(root, "lift", 4), -EEXIST, "%s", p->name);
		zassert_equal(members(p, root), 1, "%s", p->name);
		zassert_ok(p->ops->del(root, "lift", 4), "%s", p->name);
		zassert_ok(p->ops->add(root, "lift", 4),
			   "%s: a removed member can be added again", p->name);
	}
}

/* The advantage the shape promises: a re-add issues no write at all. */
ZTEST(set_contract, test_repeated_add_writes_nothing)
{
	FOR_EACH_SET(p) {
		uint64_t root = fresh_set(p, 8);
		struct blob_db_iostats io;
		size_t blobs;

		zassert_ok(p->ops->add(root, "park", 4), "%s", p->name);
		blobs = blob_db_count();
		blob_db_iostats_reset();

		zassert_equal(p->ops->add(root, "park", 4), -EEXIST, "%s", p->name);

		blob_db_iostats_get(&io);
		zassert_equal(io.writes, 0, "%s: re-add issued %u write(s)",
			      p->name, io.writes);
		zassert_equal(io.erases, 0, "%s", p->name);
		zassert_equal(blob_db_count(), blobs, "%s", p->name);
	}
}

ZTEST(set_contract, test_rejects_null_and_empty_keys)
{
	FOR_EACH_SET(p) {
		uint64_t root = fresh_set(p, 8);

		zassert_equal(p->ops->has(root, NULL, 1), -EINVAL, "%s", p->name);
		zassert_equal(p->ops->has(root, "k", 0), -EINVAL, "%s", p->name);
		zassert_equal(p->ops->add(root, NULL, 1), -EINVAL, "%s", p->name);
		zassert_equal(p->ops->add(root, "k", 0), -EINVAL, "%s", p->name);
		zassert_equal(p->ops->del(root, NULL, 1), -EINVAL, "%s", p->name);
		zassert_equal(p->ops->del(root, "k", 0), -EINVAL, "%s", p->name);
		zassert_equal(members(p, root), 0, "%s", p->name);
	}
}

/* Members are byte strings: NULs, high bytes, and lengths either side of
 * 127/128 (the native provider's header-form boundary, §4.2). */
ZTEST(set_contract, test_binary_and_boundary_keys)
{
	static uint8_t k127[127], k128[128];
	const uint8_t nul[3] = { 'a', 0x00, 'b' };
	const uint8_t nul2[3] = { 'a', 0x00, 'c' };
	const uint8_t one = 0xff;

	memset(k127, 0xa5, sizeof(k127));
	memset(k128, 0xa5, sizeof(k128));

	FOR_EACH_SET(p) {
		uint64_t root = fresh_set(p, 8);

		zassert_ok(p->ops->add(root, nul, sizeof(nul)), "%s", p->name);
		zassert_equal(p->ops->has(root, nul2, sizeof(nul2)), -ENOENT,
			      "%s: compared past the NUL", p->name);
		zassert_ok(p->ops->add(root, &one, 1), "%s", p->name);
		zassert_ok(p->ops->add(root, k127, sizeof(k127)), "%s", p->name);
		zassert_ok(p->ops->add(root, k128, sizeof(k128)), "%s", p->name);

		zassert_ok(p->ops->has(root, nul, sizeof(nul)), "%s", p->name);
		zassert_ok(p->ops->has(root, &one, 1), "%s", p->name);
		zassert_ok(p->ops->has(root, k127, sizeof(k127)), "%s", p->name);
		zassert_ok(p->ops->has(root, k128, sizeof(k128)), "%s", p->name);
		zassert_equal(members(p, root), 4, "%s", p->name);

		zassert_ok(p->ops->del(root, k127, sizeof(k127)), "%s", p->name);
		zassert_ok(p->ops->has(root, k128, sizeof(k128)),
			   "%s: the 128-byte key went with its 127-byte prefix",
			   p->name);
	}
}

/* The return codes are exact enough to keep a count by: +1 per 0 from add,
 * -1 per 0 from del. The kept figure must agree with count at every step. */
ZTEST(set_contract, test_count_kept_from_return_codes)
{
	const int n = 60;

	FOR_EACH_SET(p) {
		uint64_t root = fresh_set(p, n);
		size_t kept = 0;
		char k[8];

		for (int round = 0; round < 3; round++) {
			for (int i = 0; i < n; i += 1 + round) {
				member_name(k, sizeof(k), i);
				int rc = p->ops->add(root, k, strlen(k));

				zassert_true(rc == 0 || rc == -EEXIST, "%s: %d",
					     p->name, rc);
				kept += (rc == 0);
			}
			for (int i = round; i < n; i += 5) {
				member_name(k, sizeof(k), i);
				int rc = p->ops->del(root, k, strlen(k));

				zassert_true(rc == 0 || rc == -ENOENT, "%s: %d",
					     p->name, rc);
				kept -= (rc == 0);
			}
			zassert_equal(members(p, root), kept,
				      "%s: round %d", p->name, round);
		}
	}
}

/* next: every member exactly once, then -ENODATA. */
ZTEST(set_contract, test_next_visits_every_member_once)
{
	const int n = 40;

	FOR_EACH_SET(p) {
		uint64_t root = fresh_set(p, n);
		bool seen[40] = { false };
		char cur[8], k[8];
		size_t clen = 0, kl = 0;
		int visited = 0, rc;

		for (int i = 0; i < n; i++) {
			member_name(k, sizeof(k), i);
			zassert_ok(p->ops->add(root, k, strlen(k)), "%s", p->name);
		}

		while ((rc = p->ops->next(root, cur, clen, k, sizeof(k), &kl)) == 0) {
			int i;

			zassert_equal(kl, 4, "%s", p->name);
			zassert_equal(sscanf(k, "m%03d", &i), 1, "%s", p->name);
			zassert_true(i >= 0 && i < n, "%s", p->name);
			zassert_false(seen[i], "%s: %.4s twice", p->name, k);
			seen[i] = true;
			visited++;
			memcpy(cur, k, kl);
			clen = kl;
		}
		zassert_equal(rc, -ENODATA, "%s", p->name);
		zassert_equal(visited, n, "%s", p->name);
	}
}

ZTEST(set_contract, test_next_too_small_reports_length)
{
	FOR_EACH_SET(p) {
		uint64_t root = fresh_set(p, 8);
		char k[4];
		size_t kl = 0;

		zassert_ok(p->ops->add(root, "abcdefgh", 8), "%s", p->name);
		zassert_equal(p->ops->next(root, NULL, 0, k, sizeof(k), &kl),
			      -ENOMEM, "%s", p->name);
		zassert_equal(kl, 8, "%s", p->name);
		zassert_equal(p->ops->next(root, NULL, 0, NULL, 0, &kl), -ENOMEM,
			      "%s", p->name);
		zassert_equal(p->ops->next(root, NULL, 1, k, sizeof(k), &kl),
			      -EINVAL, "%s", p->name);
	}
}

/* A deleted cursor still has a successor; mutating mid-walk is allowed. */
ZTEST(set_contract, test_next_from_a_deleted_cursor)
{
	FOR_EACH_SET(p) {
		uint64_t root = fresh_set(p, 16);
		char first[8], k[8];
		size_t fl = 0, kl = 0;
		int rest = 0;

		for (int i = 0; i < 10; i++) {
			member_name(k, sizeof(k), i);
			zassert_ok(p->ops->add(root, k, strlen(k)), "%s", p->name);
		}
		zassert_ok(p->ops->next(root, NULL, 0, first, sizeof(first), &fl));
		zassert_ok(p->ops->del(root, first, fl), "%s", p->name);

		const char *cur = first;
		size_t clen = fl;

		while (p->ops->next(root, cur, clen, k, sizeof(k), &kl) == 0) {
			zassert_false(kl == fl && memcmp(k, first, fl) == 0,
				      "%s: the deleted cursor came back", p->name);
			rest++;
			memcpy(first, k, kl);
			fl = kl;
			cur = first;
			clen = kl;
		}
		zassert_equal(rest, 9, "%s", p->name);
	}
}

ZTEST(set_contract, test_set_survives_remount)
{
	FOR_EACH_SET(p) {
		uint64_t root = fresh_set(p, 8);

		zassert_ok(p->ops->add(root, "persist", 7), "%s", p->name);
		zassert_ok(blob_db_unmount());
		zassert_ok(blob_db_mount());

		zassert_ok(p->ops->has(root, "persist", 7), "%s", p->name);
		zassert_equal(p->ops->add(root, "persist", 7), -EEXIST, "%s", p->name);
		zassert_ok(p->ops->add(root, "after", 5), "%s", p->name);
		zassert_equal(members(p, root), 2, "%s", p->name);
	}
}

ZTEST(set_contract, test_destroy_releases_every_blob)
{
	FOR_EACH_SET(p) {
		size_t baseline = blob_db_count();
		uint64_t root = fresh_set(p, 4);
		char k[8];

		for (int i = 0; i < 16; i++) {
			member_name(k, sizeof(k), i);
			zassert_ok(p->ops->add(root, k, strlen(k)), "%s", p->name);
		}
		zassert_ok(p->ops->destroy(root), "%s", p->name);
		zassert_equal(blob_db_count(), baseline,
			      "%s: destroy left blobs behind", p->name);

		zassert_equal(p->ops->has(root, "m000", 4), -ENOENT, "%s", p->name);
		zassert_equal(p->ops->add(root, "m000", 4), -ENOENT, "%s", p->name);
		zassert_equal(p->ops->destroy(root), -ENOENT, "%s", p->name);
	}
}

/* ---- the adapter -------------------------------------------------- */

/* A root that holds a map with a value is not an adapter set: membership
 * questions about that key are -EIO, never a silent yes. */
ZTEST(set_over_map, test_a_value_is_not_a_member)
{
	uint64_t root = blob_db_alloc_id();
	char k[16];
	size_t kl = 0;

	zassert_ok(kvhash_map_ops.create(root, NULL));
	zassert_ok(kvhash_map_ops.set(root, "valued", 6, "v", 1));

	zassert_equal(set_over_kvhash_ops.has(root, "valued", 6), -EIO);
	zassert_equal(set_over_kvhash_ops.add(root, "valued", 6), -EIO);
	zassert_equal(set_over_kvhash_ops.next(root, NULL, 0, k, sizeof(k), &kl),
		      -EIO);
}

/* An adapter set is a map with empty values, readable as one. */
ZTEST(set_over_map, test_members_are_empty_map_values)
{
	uint64_t root = blob_db_alloc_id();
	size_t vl = 99;

	zassert_ok(set_over_kvhash_ops.create(root, NULL));
	zassert_ok(set_over_kvhash_ops.add(root, "m", 1));
	zassert_ok(kvhash_map_ops.get(root, "m", 1, NULL, 0, &vl));
	zassert_equal(vl, 0);
}

/* set_config reaches the map as a population: a declared size that no map
 * could honour is refused by the set too, and nothing is written. */
ZTEST(set_over_map, test_config_is_forwarded)
{
	struct set_config bad = { .max_key_bytes = 1u << 20 };
	struct set_config ok = { .expected_members = 2000, .typical_key_bytes = 16 };
	struct set_info info;
	size_t blobs = blob_db_count();
	uint64_t root = blob_db_alloc_id();

	zassert_equal(set_over_kvhash_ops.create(root, &bad), -EINVAL);
	zassert_equal(blob_db_count(), blobs);

	zassert_ok(set_over_kvhash_ops.create(root, &ok));
	zassert_ok(set_over_kvhash_ops.stat(root, &info));
	zassert_true(info.buckets > 8, "the population was not forwarded: %u buckets",
		     info.buckets);
}
