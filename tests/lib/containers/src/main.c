/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite for the L2 Map shape (map_ops).
 *
 * Written against the *shape*, not a container. Every test in the
 * `map_contract` suite loops over `providers[]`, so a new provider is covered
 * by adding one row and one Kconfig line — that portability is the whole
 * reason the shape is a vtable rather than a compile-time bind.
 *
 * The suite is deliberately two-tier:
 *
 *   Tier 1 — behaviour `shape_map.h` documents. A failure here is a provider
 *            bug.
 *   Tier 2 — behaviour the shape does *not* yet document, marked UNSPECIFIED
 *            below. These pin what kvhash does today so a second provider
 *            cannot silently disagree; each one names the open question it
 *            encodes. When the shape is amended, the marker goes away.
 *
 * The `kvhash_layout` suite is separate on purpose: it asserts things that
 * follow from kvhash's directory-of-buckets layout and must NOT be promoted
 * into the contract.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/shape_map.h>
#ifdef CONFIG_BLOB_CONTAINER_KVHASH
#include <app/lib/containers/kvhash.h>
#endif

/* ------------------------------------------------------------------ */
/* Provider table                                                      */
/* ------------------------------------------------------------------ */

struct provider {
	const char *name;
	const struct map_ops *ops;
};

static const struct provider providers[] = {
#ifdef CONFIG_BLOB_CONTAINER_KVHASH
	{ "kvhash", &kvhash_map_ops },
#endif
	/*
	 * kvlist and kvtree slot in here when they land. If a contract test
	 * below fails for one of them, that is the shape being under-specified
	 * (see the UNSPECIFIED markers), not the test being wrong.
	 */
};

BUILD_ASSERT(ARRAY_SIZE(providers) > 0, "no Map provider built in — enable one in prj.conf");

#define FOR_EACH_PROVIDER(p)                                                   \
	for (const struct provider *p = providers;                             \
	     p < providers + ARRAY_SIZE(providers); p++)

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

static void map_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
}

static void map_after(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
}

ZTEST_SUITE(map_contract, NULL, NULL, map_before, map_after, NULL);
ZTEST_SUITE(kvhash_layout, NULL, NULL, map_before, map_after, NULL);

/*
 * Build a fresh map and return its root.
 *
 * Note what this helper has to do: `create` binds a root, it does not mint
 * one. The caller allocates the id first. That precondition is not stated in
 * shape_map.h today (see UNSPECIFIED-2) — this helper is where it lives.
 */
static uint64_t fresh_map(const struct provider *p, size_t capacity)
{
	struct map_config cfg = { .initial_capacity = capacity };
	uint64_t root = blob_db_alloc_id();

	zassert_not_equal(root, 0, "%s: alloc_id failed", p->name);
	zassert_ok(p->ops->create(root, &cfg), "%s: create", p->name);
	return root;
}

/* ================================================================== */
/* Tier 1 — the documented contract                                    */
/* ================================================================== */

/*
 * Every op is mandatory. Nothing in the shape says so, and designated
 * initialisers make an omitted op a NULL jump on first call rather than a
 * build error — so assert it once, loudly, for every provider.
 */
ZTEST(map_contract, test_op_vector_is_complete)
{
	FOR_EACH_PROVIDER(p) {
		zassert_not_null(p->ops, "%s: no op vector", p->name);
		zassert_not_null(p->ops->create, "%s: create missing", p->name);
		zassert_not_null(p->ops->get, "%s: get missing", p->name);
		zassert_not_null(p->ops->set, "%s: set missing", p->name);
		zassert_not_null(p->ops->del, "%s: del missing", p->name);
		zassert_not_null(p->ops->destroy, "%s: destroy missing", p->name);
	}
}

/* "On return @p root holds a valid, empty structure." */
ZTEST(map_contract, test_create_yields_an_empty_map)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		char out[16];
		size_t len = 0;

		zassert_equal(p->ops->get(root, "k", 1, out, sizeof(out), &len),
			      -ENOENT, "%s: fresh map returned a value", p->name);
		zassert_equal(p->ops->del(root, "k", 1), -ENOENT,
			      "%s: del on fresh map", p->name);
	}
}

/* cfg may be NULL for provider defaults. */
ZTEST(map_contract, test_create_accepts_null_config)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = blob_db_alloc_id();
		char out[8];
		size_t len = 0;

		zassert_not_equal(root, 0);
		zassert_ok(p->ops->create(root, NULL), "%s: create(NULL)", p->name);

		/* A default-shaped map is a working map. */
		zassert_ok(p->ops->set(root, "a", 1, "v", 1), "%s: set", p->name);
		zassert_ok(p->ops->get(root, "a", 1, out, sizeof(out), &len),
			   "%s: get", p->name);
		zassert_equal(len, 1, "%s: len", p->name);
	}
}

ZTEST(map_contract, test_set_get_roundtrip)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		char out[32];
		size_t len = 0;

		zassert_ok(p->ops->set(root, "color", 5, "green", 5),
			   "%s: set", p->name);
		zassert_ok(p->ops->get(root, "color", 5, out, sizeof(out), &len),
			   "%s: get", p->name);
		zassert_equal(len, 5, "%s: out_len", p->name);
		zassert_mem_equal(out, "green", 5, "%s: payload", p->name);

		zassert_equal(p->ops->get(root, "colour", 6, out, sizeof(out), &len),
			      -ENOENT, "%s: near-miss key must not match", p->name);
	}
}

/* "Insert @p key or replace its value." Both directions of length change. */
ZTEST(map_contract, test_set_replaces_value)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		char out[32];
		size_t len = 0;

		zassert_ok(p->ops->set(root, "k", 1, "aaaaaaaa", 8));

		/* Shrink. */
		zassert_ok(p->ops->set(root, "k", 1, "bb", 2));
		zassert_ok(p->ops->get(root, "k", 1, out, sizeof(out), &len));
		zassert_equal(len, 2, "%s: replace must not leave stale length", p->name);
		zassert_mem_equal(out, "bb", 2, "%s", p->name);

		/* Grow again. */
		zassert_ok(p->ops->set(root, "k", 1, "cccccccccc", 10));
		zassert_ok(p->ops->get(root, "k", 1, out, sizeof(out), &len));
		zassert_equal(len, 10, "%s: regrow", p->name);
		zassert_mem_equal(out, "cccccccccc", 10, "%s", p->name);
	}
}

ZTEST(map_contract, test_del_removes_only_its_key)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		char out[16];
		size_t len = 0;

		zassert_ok(p->ops->set(root, "keep", 4, "1", 1));
		zassert_ok(p->ops->set(root, "drop", 4, "2", 1));

		zassert_ok(p->ops->del(root, "drop", 4), "%s: del", p->name);
		zassert_equal(p->ops->get(root, "drop", 4, out, sizeof(out), &len),
			      -ENOENT, "%s: deleted key still readable", p->name);
		zassert_equal(p->ops->del(root, "drop", 4), -ENOENT,
			      "%s: second del", p->name);

		zassert_ok(p->ops->get(root, "keep", 4, out, sizeof(out), &len),
			   "%s: neighbour lost to del", p->name);
		zassert_equal(len, 1, "%s", p->name);
	}
}

/*
 * "If the key exists but @p out_sz is smaller than the value, returns -ENOMEM
 *  with @p out_len (when non-NULL) set to the true length."
 *
 * This is the documented way to size a buffer, so the length must be right
 * even though the call failed.
 */
ZTEST(map_contract, test_get_too_small_reports_true_length)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		char small[4];
		size_t len = 0;

		zassert_ok(p->ops->set(root, "k", 1, "0123456789", 10));

		zassert_equal(p->ops->get(root, "k", 1, small, sizeof(small), &len),
			      -ENOMEM, "%s: short buffer must fail", p->name);
		zassert_equal(len, 10, "%s: -ENOMEM must still report the length", p->name);

		/* Exact fit is a success, not an off-by-one failure. */
		char exact[10];

		zassert_ok(p->ops->get(root, "k", 1, exact, sizeof(exact), &len),
			   "%s: exact-size buffer rejected", p->name);
		zassert_equal(len, 10, "%s", p->name);
		zassert_mem_equal(exact, "0123456789", 10, "%s", p->name);
	}
}

/*
 * "@p out may be NULL when @p out_sz is 0 (existence probe)."
 *
 * The probe's result is subtle enough to be worth pinning: a present key with
 * a non-empty value answers -ENOMEM, not 0. So "present" is (0 || -ENOMEM)
 * and only -ENOENT means absent.
 */
ZTEST(map_contract, test_existence_probe)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		size_t len = 0;

		zassert_ok(p->ops->set(root, "here", 4, "value", 5));

		zassert_equal(p->ops->get(root, "here", 4, NULL, 0, &len), -ENOMEM,
			      "%s: probe of a present key", p->name);
		zassert_equal(len, 5, "%s: probe must report the length", p->name);

		zassert_equal(p->ops->get(root, "gone", 4, NULL, 0, &len), -ENOENT,
			      "%s: probe of an absent key", p->name);
	}
}

/* out_len is optional ("when non-NULL"). */
ZTEST(map_contract, test_get_accepts_null_out_len)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		char out[8];

		zassert_ok(p->ops->set(root, "k", 1, "vv", 2));
		zassert_ok(p->ops->get(root, "k", 1, out, sizeof(out), NULL),
			   "%s: NULL out_len rejected", p->name);
		zassert_mem_equal(out, "vv", 2, "%s", p->name);
	}
}

/*
 * Many keys stay independent. For a hash this walks collision chains; for a
 * list or tree it is just bulk. Either way the shape promises key isolation.
 */
ZTEST(map_contract, test_many_keys_stay_independent)
{
	const int n = 32;

	FOR_EACH_PROVIDER(p) {
		/* Deliberately fewer buckets than keys, to force collisions. */
		uint64_t root = fresh_map(p, 4);
		char key[8], val[8], out[8];
		size_t len = 0;

		for (int i = 0; i < n; i++) {
			snprintf(key, sizeof(key), "k%02d", i);
			snprintf(val, sizeof(val), "v%02d", i);
			zassert_ok(p->ops->set(root, key, strlen(key), val, 3),
				   "%s: set %s", p->name, key);
		}

		for (int i = 0; i < n; i++) {
			snprintf(key, sizeof(key), "k%02d", i);
			snprintf(val, sizeof(val), "v%02d", i);
			zassert_ok(p->ops->get(root, key, strlen(key), out, sizeof(out), &len),
				   "%s: get %s", p->name, key);
			zassert_equal(len, 3, "%s: %s len", p->name, key);
			zassert_mem_equal(out, val, 3, "%s: %s crosstalk", p->name, key);
		}

		/* Remove the even keys; the odd ones must be untouched. */
		for (int i = 0; i < n; i += 2) {
			snprintf(key, sizeof(key), "k%02d", i);
			zassert_ok(p->ops->del(root, key, strlen(key)),
				   "%s: del %s", p->name, key);
		}
		for (int i = 0; i < n; i++) {
			snprintf(key, sizeof(key), "k%02d", i);
			snprintf(val, sizeof(val), "v%02d", i);
			int rc = p->ops->get(root, key, strlen(key), out, sizeof(out), &len);

			if (i % 2 == 0) {
				zassert_equal(rc, -ENOENT, "%s: %s survived del",
					      p->name, key);
			} else {
				zassert_ok(rc, "%s: %s lost to a neighbour's del",
					   p->name, key);
				zassert_mem_equal(out, val, 3, "%s: %s", p->name, key);
			}
		}
	}
}

/*
 * P5: the root id is the entire handle. Nothing is cached in RAM that a
 * remount needs to rebuild — hold the uint64_t, get the map back.
 */
ZTEST(map_contract, test_map_survives_remount)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		char out[16];
		size_t len = 0;

		zassert_ok(p->ops->set(root, "persist", 7, "yes", 3));

		zassert_ok(blob_db_unmount());
		zassert_ok(blob_db_mount());

		zassert_ok(p->ops->get(root, "persist", 7, out, sizeof(out), &len),
			   "%s: lost across remount", p->name);
		zassert_equal(len, 3, "%s", p->name);
		zassert_mem_equal(out, "yes", 3, "%s", p->name);

		/* And it is still writable, not just readable. */
		zassert_ok(p->ops->set(root, "after", 5, "ok", 2), "%s", p->name);
	}
}

/*
 * destroy is the mirror of create: the root stops identifying a map, and a
 * second call has nothing left to do.
 */
ZTEST(map_contract, test_destroy_removes_the_map)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 4);
		char out[8];
		size_t len = 0;

		zassert_ok(p->ops->set(root, "k", 1, "v", 1));

		zassert_ok(p->ops->destroy(root), "%s: destroy", p->name);

		zassert_equal(p->ops->get(root, "k", 1, out, sizeof(out), &len),
			      -ENOENT, "%s: readable after destroy", p->name);
		zassert_equal(p->ops->set(root, "k", 1, "v", 1), -ENOENT,
			      "%s: writable after destroy", p->name);
		zassert_equal(p->ops->destroy(root), -ENOENT,
			      "%s: destroy is not idempotent", p->name);
	}
}

/*
 * The point of the op. Count live blobs before the map exists, build and fill
 * it, destroy it, and the count must come all the way back -- not just the
 * root, every node the container owned.
 *
 * This is the inverse of kvhash_layout's create-on-populated-root test, which
 * asserts the same count *failing* to drop. Both are written against
 * blob_db_count() so they read as one pair.
 */
ZTEST(map_contract, test_destroy_releases_every_blob_it_owned)
{
	FOR_EACH_PROVIDER(p) {
		size_t baseline = blob_db_count();
		uint64_t root = fresh_map(p, 4);
		char key[8];

		for (int i = 0; i < 16; i++) {
			snprintf(key, sizeof(key), "k%02d", i);
			zassert_ok(p->ops->set(root, key, strlen(key), "vvvv", 4));
		}

		zassert_true(blob_db_count() > baseline,
			     "%s: a populated map should own blobs", p->name);

		zassert_ok(p->ops->destroy(root), "%s: destroy", p->name);

		zassert_equal(blob_db_count(), baseline,
			      "%s: destroy left %zu blob(s) behind", p->name,
			      blob_db_count() - baseline);
	}
}

/* Destroying one map must not disturb another. */
ZTEST(map_contract, test_destroy_leaves_other_maps_alone)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t doomed = fresh_map(p, 4);
		uint64_t keeper = fresh_map(p, 4);
		char out[8];
		size_t len = 0;

		zassert_ok(p->ops->set(doomed, "k", 1, "d", 1));
		zassert_ok(p->ops->set(keeper, "k", 1, "k", 1));

		zassert_ok(p->ops->destroy(doomed), "%s: destroy", p->name);

		zassert_ok(p->ops->get(keeper, "k", 1, out, sizeof(out), &len),
			   "%s: neighbour lost to destroy", p->name);
		zassert_equal(len, 1, "%s", p->name);
		zassert_mem_equal(out, "k", 1, "%s", p->name);

		/* And the surviving map is still writable. */
		zassert_ok(p->ops->set(keeper, "more", 4, "x", 1), "%s", p->name);
	}
}

/* A root that never held a map has nothing to destroy. */
ZTEST(map_contract, test_destroy_of_an_unbound_root)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = blob_db_alloc_id();

		zassert_not_equal(root, 0);
		zassert_equal(p->ops->destroy(root), -ENOENT,
			      "%s: destroy of an unbound root", p->name);
	}
}

/*
 * Replacing a map means destroying it and building the new one at a *fresh*
 * id. Re-creating at the old root is not the supported path and is not tested
 * here: blob_db_update() on a deleted id is undefined behaviour by decision
 * D3 (see blob_db.h), so create() on a destroyed root inherits that. What is
 * well-defined is everything that reads the root first -- get, set, del and
 * destroy all load the structure before touching it, so they report -ENOENT
 * on a dead root rather than wandering into UB.
 */
ZTEST(map_contract, test_replace_a_map_at_a_fresh_id)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t old_root = fresh_map(p, 4);
		char out[8];
		size_t len = 0;

		zassert_ok(p->ops->set(old_root, "k", 1, "old", 3));
		zassert_ok(p->ops->destroy(old_root));

		uint64_t new_root = fresh_map(p, 4);

		zassert_not_equal(new_root, old_root,
				  "%s: alloc_id reissued a destroyed root", p->name);

		zassert_ok(p->ops->set(new_root, "k", 1, "new", 3));
		zassert_ok(p->ops->get(new_root, "k", 1, out, sizeof(out), &len),
			   "%s: replacement map", p->name);
		zassert_mem_equal(out, "new", 3, "%s", p->name);

		/* The old root stays dead on every read path. */
		zassert_equal(p->ops->get(old_root, "k", 1, out, sizeof(out), &len),
			      -ENOENT, "%s: old root came back", p->name);
	}
}

/* ================================================================== */
/* Tier 2 — UNSPECIFIED: pins today's behaviour, pending a shape edit  */
/* ================================================================== */

/*
 * UNSPECIFIED-4: the key/value domain.
 *
 * kvhash rejects a NULL or empty key with -EINVAL. shape_map.h says nothing
 * about it, so a second provider is free to accept an empty key and nobody
 * would notice until two backends behaved differently on the same data.
 */
ZTEST(map_contract, test_rejects_null_and_empty_keys)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		char out[8];
		size_t len = 0;

		zassert_equal(p->ops->get(root, NULL, 4, out, sizeof(out), &len),
			      -EINVAL, "%s: get(NULL key)", p->name);
		zassert_equal(p->ops->get(root, "k", 0, out, sizeof(out), &len),
			      -EINVAL, "%s: get(klen 0)", p->name);

		zassert_equal(p->ops->set(root, NULL, 4, "v", 1), -EINVAL,
			      "%s: set(NULL key)", p->name);
		zassert_equal(p->ops->set(root, "k", 0, "v", 1), -EINVAL,
			      "%s: set(klen 0)", p->name);

		zassert_equal(p->ops->del(root, NULL, 4), -EINVAL,
			      "%s: del(NULL key)", p->name);
		zassert_equal(p->ops->del(root, "k", 0), -EINVAL,
			      "%s: del(klen 0)", p->name);
	}
}

/*
 * UNSPECIFIED-4: a NULL value pointer is only legal when the length is zero.
 */
ZTEST(map_contract, test_rejects_null_value_with_nonzero_len)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);

		zassert_equal(p->ops->set(root, "k", 1, NULL, 5), -EINVAL,
			      "%s: set(NULL val, len 5)", p->name);
	}
}

/*
 * UNSPECIFIED-4: an empty *value* is legal even though an empty *key* is not,
 * and it is a stored key, not an absent one. This is the asymmetry most
 * likely to be got wrong by a second provider.
 */
ZTEST(map_contract, test_empty_value_is_a_stored_value)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		char out[8];
		size_t len = 42;

		zassert_ok(p->ops->set(root, "empty", 5, NULL, 0),
			   "%s: set(vlen 0)", p->name);

		zassert_ok(p->ops->get(root, "empty", 5, out, sizeof(out), &len),
			   "%s: empty value must read back as present", p->name);
		zassert_equal(len, 0, "%s: empty value length", p->name);

		/* The probe corner: zero-length value, zero-length buffer, so
		 * this is 0 rather than the -ENOMEM a non-empty value gives. */
		zassert_ok(p->ops->get(root, "empty", 5, NULL, 0, &len),
			   "%s: probe of an empty value", p->name);

		zassert_ok(p->ops->del(root, "empty", 5), "%s: del", p->name);
	}
}

/*
 * UNSPECIFIED-6: -ENOENT is overloaded.
 *
 * A root that was allocated but never built into a map answers -ENOENT — the
 * same code a missing key gives. A caller cannot tell "you handed me a root
 * that is not a map" from "that key isn't here".
 */
ZTEST(map_contract, test_unbound_root_is_indistinguishable_from_a_miss)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = blob_db_alloc_id();
		char out[8];
		size_t len = 0;

		zassert_not_equal(root, 0);
		/* No create() — the id is allocated but unbound. */
		zassert_equal(p->ops->get(root, "k", 1, out, sizeof(out), &len),
			      -ENOENT, "%s: unbound root", p->name);
	}
}

/* ================================================================== */
/* kvhash layout — provider-specific, NOT part of the contract         */
/* ================================================================== */

#ifdef CONFIG_BLOB_CONTAINER_KVHASH

static const struct provider kvhash = { "kvhash", &kvhash_map_ops };

/*
 * shape_map.h calls initial_capacity an "expected entry count", but kvhash
 * uses the number as its bucket count. The two readings only differ in
 * observable behaviour at the edges — here, that asking for 2 does not cap
 * the map at 2 entries.
 */
ZTEST(kvhash_layout, test_capacity_is_not_an_entry_limit)
{
	uint64_t root = fresh_map(&kvhash, 2);
	char key[8], out[8];
	size_t len = 0;

	for (int i = 0; i < 16; i++) {
		snprintf(key, sizeof(key), "k%02d", i);
		zassert_ok(kvhash.ops->set(root, key, strlen(key), "vvvv", 4),
			   "set %s past the requested capacity", key);
	}
	for (int i = 0; i < 16; i++) {
		snprintf(key, sizeof(key), "k%02d", i);
		zassert_ok(kvhash.ops->get(root, key, strlen(key), out, sizeof(out), &len),
			   "get %s", key);
		zassert_equal(len, 4);
	}
}

/*
 * An oversized capacity is silently clamped to what one directory payload
 * holds, not rejected. Worth pinning: -ENOSPC is a documented create() return,
 * so "too big" plausibly *could* have been an error, and a caller who assumes
 * it gets what it asked for is wrong today with no diagnostic.
 */
ZTEST(kvhash_layout, test_oversized_capacity_is_clamped_not_rejected)
{
	struct map_config cfg = { .initial_capacity = 100000 };
	uint64_t root = blob_db_alloc_id();
	char out[8];
	size_t len = 0;

	zassert_not_equal(root, 0);
	zassert_ok(kvhash_map_ops.create(root, &cfg),
		   "an unreachable capacity should clamp, not fail");

	zassert_ok(kvhash_map_ops.set(root, "k", 1, "v", 1));
	zassert_ok(kvhash_map_ops.get(root, "k", 1, out, sizeof(out), &len));
	zassert_equal(len, 1);
}

/*
 * A bucket is one blob payload. Overflowing it is -ENOSPC, and — the part
 * that matters — the failed insert must leave the bucket exactly as it was.
 *
 * Two buckets and ten 100-byte values guarantee the overflow by pigeonhole
 * without hard-coding fnv1a: some bucket takes at least five entries, and
 * three already exceed the 256-byte default payload.
 */
ZTEST(kvhash_layout, test_bucket_overflow_is_enospc_without_damage)
{
	uint64_t root = fresh_map(&kvhash, 2);
	char big[100];
	bool stored[10] = { false };
	int enospc = 0;

	memset(big, 'x', sizeof(big));

	for (int i = 0; i < 10; i++) {
		char key[8];

		snprintf(key, sizeof(key), "k%d", i);
		big[0] = (char)('0' + i);

		int rc = kvhash_map_ops.set(root, key, strlen(key), big, sizeof(big));

		if (rc == -ENOSPC) {
			enospc++;
		} else {
			zassert_ok(rc, "set %s: unexpected %d", key, rc);
			stored[i] = true;
		}
	}

	zassert_true(enospc > 0, "a full bucket must report -ENOSPC");

	/* Everything that was accepted is still intact and correct. */
	for (int i = 0; i < 10; i++) {
		char key[8], out[128];
		size_t len = 0;

		snprintf(key, sizeof(key), "k%d", i);
		int rc = kvhash_map_ops.get(root, key, strlen(key), out, sizeof(out), &len);

		if (!stored[i]) {
			zassert_equal(rc, -ENOENT, "%s was rejected but is readable", key);
			continue;
		}
		zassert_ok(rc, "%s lost after a neighbour's -ENOSPC", key);
		zassert_equal(len, sizeof(big), "%s truncated", key);
		zassert_equal(out[0], (char)('0' + i), "%s corrupted", key);
	}
}

/*
 * kvhash's on-flash directory, as far as these tests need it:
 *   [u32 magic][u16 n_buckets][u16 version][u64 bucket_id]*n
 * 'KVHD' in place of 'KVHA' is the dying stamp destroy writes as its commit.
 */
#define KVHA_DYING_MAGIC 0x4b564844u
#define KVHA_DIR_HDR     8u

/*
 * The middle row of l2_containers.md 2.4: a destroy that committed and did not
 * finish. Stamp the root by hand -- the state a crash between the commit and
 * the last release leaves -- then check a reader is refused at the *container*
 * rather than per key, and that repeating destroy still reclaims everything.
 */
ZTEST(kvhash_layout, test_destroy_resumes_after_an_interrupted_release)
{
	size_t baseline = blob_db_count();
	uint64_t root = fresh_map(&kvhash, 4);
	uint8_t dir[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	uint32_t dying = KVHA_DYING_MAGIC;
	size_t got = 0;
	char key[8], out[8];
	size_t len = 0;

	for (int i = 0; i < 16; i++) {
		snprintf(key, sizeof(key), "k%02d", i);
		zassert_ok(kvhash_map_ops.set(root, key, strlen(key), "vvvv", 4));
	}

	zassert_ok(blob_db_get(root, dir, sizeof(dir), &got));
	memcpy(&dir[0], &dying, sizeof(dying));
	zassert_ok(blob_db_update(root, dir, got));

	/* Refused at the root: every key, not just the released ones. */
	for (int i = 0; i < 16; i++) {
		snprintf(key, sizeof(key), "k%02d", i);
		zassert_equal(kvhash_map_ops.get(root, key, strlen(key),
						 out, sizeof(out), &len),
			      -ENOENT, "%s readable mid-destroy", key);
	}
	zassert_equal(kvhash_map_ops.set(root, "k00", 3, "x", 1), -ENOENT,
		      "a dying map accepted a write");
	zassert_equal(kvhash_map_ops.del(root, "k00", 3), -ENOENT);

	/* The repeat completes it. */
	zassert_ok(kvhash_map_ops.destroy(root), "destroy did not resume");
	zassert_equal(blob_db_count(), baseline,
		      "resumed destroy left %zu blob(s) behind",
		      blob_db_count() - baseline);
}

/*
 * The other half of resumability: buckets already released. Delete some behind
 * kvhash's back, then destroy -- the -ENOENT they answer is the resumed case,
 * not a failure, and the rest must still be reclaimed.
 */
ZTEST(kvhash_layout, test_destroy_tolerates_already_released_buckets)
{
	size_t baseline = blob_db_count();
	uint64_t root = fresh_map(&kvhash, 4);
	uint8_t dir[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t got = 0;
	char key[8];
	int released = 0;

	for (int i = 0; i < 16; i++) {
		snprintf(key, sizeof(key), "k%02d", i);
		zassert_ok(kvhash_map_ops.set(root, key, strlen(key), "vvvv", 4));
	}

	zassert_ok(blob_db_get(root, dir, sizeof(dir), &got));

	uint16_t n;

	memcpy(&n, &dir[4], sizeof(n));

	for (uint16_t i = 0; i < n && released < 2; i++) {
		uint64_t bid;

		memcpy(&bid, &dir[KVHA_DIR_HDR + (size_t)i * 8u], sizeof(bid));
		if (bid == 0) {
			continue;
		}
		zassert_ok(blob_db_delete(bid), "manual release of bucket %u", i);
		released++;
	}
	zassert_equal(released, 2, "test needs at least two live buckets");

	zassert_ok(kvhash_map_ops.destroy(root),
		   "destroy failed over already-released buckets");
	zassert_equal(blob_db_count(), baseline,
		      "destroy left %zu blob(s) behind",
		      blob_db_count() - baseline);
}

/*
 * A root holding something that is not a kvhash directory is refused.
 *
 * NOTE: this pins -EIO, which is what dir_load() returns on a magic mismatch.
 * doc/layers/l2_containers.md §2.3 specifies -EINVAL for a root of the wrong
 * type. One of the two has to move; this test is where that shows up.
 */
ZTEST(kvhash_layout, test_wrong_type_root_is_refused)
{
	uint64_t root = blob_db_alloc_id();
	char out[8];
	size_t len = 0;

	zassert_not_equal(root, 0);
	zassert_ok(blob_db_update(root, "definitely not a directory", 26));

	zassert_equal(kvhash_map_ops.get(root, "k", 1, out, sizeof(out), &len), -EIO,
		      "a foreign root must not be read as a map");
	zassert_equal(kvhash_map_ops.set(root, "k", 1, "v", 1), -EIO);
	zassert_equal(kvhash_map_ops.del(root, "k", 1), -EIO);
	zassert_equal(kvhash_map_ops.destroy(root), -EIO,
		      "a foreign root must not be deleted as a map");
	zassert_true(blob_db_exists(root), "a refused destroy must not delete");
}

/*
 * KNOWN DEFECT (shape review item 3): create() on a populated root silently
 * re-initialises the directory and orphans every bucket blob it pointed at.
 * blob_db reclaims only at format, so those blobs leak permanently, which is
 * what P7 forbids.
 *
 * This is a characterisation test: it asserts the leak so the defect is
 * visible and tracked. When create() learns -EEXIST — or the shape states
 * that the caller guarantees create-once — this test flips to assert that
 * instead.
 *
 * There is now a correct way to do this: destroy() then create(). That is
 * what map_contract's test_destroy_releases_every_blob_it_owned asserts, and
 * the two tests are deliberately the same blob_db_count() measurement with
 * opposite expectations.
 */
ZTEST(kvhash_layout, test_create_on_populated_root_orphans_its_buckets)
{
	uint64_t root = fresh_map(&kvhash, 4);
	char out[8];
	size_t len = 0;

	for (int i = 0; i < 8; i++) {
		char key[8];

		snprintf(key, sizeof(key), "k%d", i);
		zassert_ok(kvhash_map_ops.set(root, key, strlen(key), "v", 1));
	}

	size_t live_before = blob_db_count();

	zassert_ok(kvhash_map_ops.create(root, NULL), "re-create was refused");

	/* The data is gone from the map... */
	zassert_equal(kvhash_map_ops.get(root, "k0", 2, out, sizeof(out), &len),
		      -ENOENT, "re-create should have emptied the map");

	/* ...but the bucket blobs are still live, and now unreachable. */
	zassert_equal(blob_db_count(), live_before,
		      "expected the orphaned buckets to still be live (leak); "
		      "if this now fails, create() learned to clean up and the "
		      "test should assert that instead");
}

#endif /* CONFIG_BLOB_CONTAINER_KVHASH */
