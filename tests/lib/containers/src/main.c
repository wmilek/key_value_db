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
 * Build a fresh map and return its root. @p expected is the population the
 * caller declares -- a sizing hint, not a limit; the geometry it buys is the
 * container's business (see the kvhash_layout suite).
 *
 * Note what this helper has to do: `create` binds a root, it does not mint
 * one. The caller allocates the id first. That precondition is not stated in
 * shape_map.h today (see UNSPECIFIED-2) — this helper is where it lives.
 */
static uint64_t fresh_map(const struct provider *p, size_t expected)
{
	struct map_config cfg = { .expected_entries = expected };
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
		zassert_not_null(p->ops->next, "%s: next missing", p->name);
		zassert_not_null(p->ops->count, "%s: count missing", p->name);
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

/*
 * The key domain: a key is non-empty, so a NULL or empty key is -EINVAL.
 * Formerly UNSPECIFIED-4; the shape states it now, because `next` gives the
 * empty key a meaning of its own (the start of a walk).
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

/* A NULL value pointer is only legal when the length is zero. */
ZTEST(map_contract, test_rejects_null_value_with_nonzero_len)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);

		zassert_equal(p->ops->set(root, "k", 1, NULL, 5), -EINVAL,
			      "%s: set(NULL val, len 5)", p->name);
	}
}

/*
 * An empty *value* is legal even though an empty *key* is not, and it is a
 * stored key, not an absent one. This is the asymmetry most likely to be got
 * wrong by a second provider.
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

/* ------------------------------------------------------------------ */
/* next — stateless enumeration                                        */
/* ------------------------------------------------------------------ */

#define WALK_MAX 160
#define WKEY     16

/* The keys of one walk, in the order `next` returned them. */
struct walk {
	size_t n;
	char key[WALK_MAX][WKEY];
	size_t klen[WALK_MAX];
};

/* Large enough to be worth keeping off the ztest stack. */
static struct walk walk_a, walk_b;

/* Store n keys "k000".. with values "v000".. */
static void fill(const struct provider *p, uint64_t root, int n)
{
	char key[8], val[8];

	for (int i = 0; i < n; i++) {
		snprintf(key, sizeof(key), "k%03d", i);
		snprintf(val, sizeof(val), "v%03d", i);
		zassert_ok(p->ops->set(root, key, 4, val, 4), "%s: set %s",
			   p->name, key);
	}
}

/* Append to @w the rest of the walk that follows @cur (klen 0: from start). */
static void walk_from(const struct provider *p, uint64_t root,
		      const void *cur, size_t clen, struct walk *w)
{
	char k[WKEY], v[16];
	size_t kl = 0, vl = 0;
	int rc;

	while ((rc = p->ops->next(root, cur, clen, k, sizeof(k), &kl,
				  v, sizeof(v), &vl)) == 0) {
		zassert_true(w->n < WALK_MAX, "%s: walk does not end", p->name);
		memcpy(w->key[w->n], k, kl);
		w->klen[w->n] = kl;
		cur = w->key[w->n];
		clen = kl;
		w->n++;
	}
	zassert_equal(rc, -ENODATA, "%s: walk ended with %d", p->name, rc);
}

static void walk_all(const struct provider *p, uint64_t root, struct walk *w)
{
	w->n = 0;
	walk_from(p, root, NULL, 0, w);
}

static bool walks_equal(const struct walk *a, const struct walk *b)
{
	if (a->n != b->n) {
		return false;
	}
	for (size_t i = 0; i < a->n; i++) {
		if (a->klen[i] != b->klen[i] ||
		    memcmp(a->key[i], b->key[i], a->klen[i]) != 0) {
			return false;
		}
	}
	return true;
}

/* Every key of @w is "k%03d" below @n, and each appears exactly once. */
static void assert_each_once(const char *name, const struct walk *w, int n)
{
	static bool seen[WALK_MAX];
	int i;

	memset(seen, 0, sizeof(seen));
	zassert_equal(w->n, (size_t)n, "%s: walk returned %zu of %d keys",
		      name, w->n, n);
	for (size_t j = 0; j < w->n; j++) {
		zassert_equal(w->klen[j], 4, "%s: key length", name);
		zassert_equal(sscanf(w->key[j], "k%3d", &i), 1, "%s: key", name);
		zassert_true(i >= 0 && i < n, "%s: foreign key", name);
		zassert_false(seen[i], "%s: k%03d returned twice", name, i);
		seen[i] = true;
	}
}

/* "an empty map answers this to the first call" -- including one emptied. */
ZTEST(map_contract, test_next_on_empty_map_is_enodata)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		char k[WKEY], v[16];
		size_t kl = 0, vl = 0;

		zassert_equal(p->ops->next(root, NULL, 0, k, sizeof(k), &kl,
					   v, sizeof(v), &vl),
			      -ENODATA, "%s: fresh map", p->name);

		zassert_ok(p->ops->set(root, "k", 1, "v", 1));
		zassert_ok(p->ops->del(root, "k", 1));
		zassert_equal(p->ops->next(root, NULL, 0, k, sizeof(k), &kl,
					   v, sizeof(v), &vl),
			      -ENODATA, "%s: emptied map", p->name);
	}
}

/* A walk returns every key exactly once, each with its own value. */
ZTEST(map_contract, test_next_visits_every_key_once)
{
	const int n = 40;

	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, n);
		char k[WKEY], v[16], val[8];
		size_t kl = 0, vl = 0;

		fill(p, root, n);
		walk_all(p, root, &walk_a);
		assert_each_once(p->name, &walk_a, n);

		/* And each carries its own value, not a neighbour's. */
		for (size_t j = 0; j < walk_a.n; j++) {
			const void *cur = j ? walk_a.key[j - 1] : NULL;
			size_t clen = j ? walk_a.klen[j - 1] : 0;

			zassert_ok(p->ops->next(root, cur, clen, k, sizeof(k), &kl,
						v, sizeof(v), &vl));
			snprintf(val, sizeof(val), "v%.3s", &k[1]);
			zassert_equal(vl, 4, "%s: value length", p->name);
			zassert_mem_equal(v, val, 4, "%s: %.4s's value", p->name, k);
		}
	}
}

/*
 * The cursor need not be present: a deleted key still has the successor it
 * had while it was there. Every other key is deleted, so each cursor here is
 * gone and its successor is not.
 */
ZTEST(map_contract, test_next_from_a_deleted_key)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 40);
		char k[WKEY], v[16];
		size_t kl = 0, vl = 0;

		fill(p, root, 40);
		walk_all(p, root, &walk_a);
		zassert_equal(walk_a.n, 40, "%s", p->name);

		for (size_t i = 0; i < walk_a.n; i += 2) {
			zassert_ok(p->ops->del(root, walk_a.key[i], walk_a.klen[i]));
		}

		for (size_t i = 0; i < walk_a.n; i += 2) {
			int rc = p->ops->next(root, walk_a.key[i], walk_a.klen[i],
					      k, sizeof(k), &kl, v, sizeof(v), &vl);

			if (i + 1 == walk_a.n) {
				zassert_equal(rc, -ENODATA, "%s: past the last",
					      p->name);
				continue;
			}
			zassert_ok(rc, "%s: next(deleted %.4s)", p->name,
				   walk_a.key[i]);
			zassert_equal(kl, walk_a.klen[i + 1], "%s", p->name);
			zassert_mem_equal(k, walk_a.key[i + 1], kl,
					  "%s: successor of deleted %.4s moved",
					  p->name, walk_a.key[i]);
		}

		/* A key that never existed is a valid cursor too. */
		int rc = p->ops->next(root, "never-stored", 12, k, sizeof(k), &kl,
				      v, sizeof(v), &vl);

		zassert_true(rc == 0 || rc == -ENODATA,
			     "%s: next(absent key) = %d", p->name, rc);
	}
}

/*
 * The order is a function of the keys alone: replacing every value, churning
 * other keys in and out, and remounting all leave it as it was.
 */
ZTEST(map_contract, test_next_order_is_stable)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 40);
		char key[8];

		fill(p, root, 30);
		walk_all(p, root, &walk_a);

		for (size_t i = 0; i < walk_a.n; i++) {
			zassert_ok(p->ops->set(root, walk_a.key[i], walk_a.klen[i],
					       "replaced", 8));
		}
		for (int i = 0; i < 10; i++) {
			snprintf(key, sizeof(key), "x%03d", i);
			zassert_ok(p->ops->set(root, key, 4, "t", 1));
		}
		for (int i = 0; i < 10; i++) {
			snprintf(key, sizeof(key), "x%03d", i);
			zassert_ok(p->ops->del(root, key, 4));
		}
		zassert_ok(blob_db_unmount());
		zassert_ok(blob_db_mount());

		walk_all(p, root, &walk_b);
		zassert_true(walks_equal(&walk_a, &walk_b),
			     "%s: order changed under value/neighbour churn",
			     p->name);
	}
}

/*
 * Mutation between calls is allowed. Here every returned key is deleted at
 * once and a new key is inserted per step; every original key, present until
 * the walk reaches it, must still be returned exactly once.
 */
ZTEST(map_contract, test_next_survives_mutation_between_calls)
{
	const int n = 30;

	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 2 * n);
		char cur[WKEY], k[WKEY], v[16], key[16];
		size_t clen = 0, kl = 0, vl = 0;
		int seen[30] = { 0 };
		int added = 0, steps = 0;
		int rc;

		fill(p, root, n);

		while ((rc = p->ops->next(root, cur, clen, k, sizeof(k), &kl,
					  v, sizeof(v), &vl)) == 0) {
			int i;

			zassert_true(steps++ < 2 * n, "%s: walk does not end",
				     p->name);
			if (k[0] == 'k' && sscanf(k, "k%3d", &i) == 1) {
				seen[i]++;
			}

			zassert_ok(p->ops->del(root, k, kl), "%s: del mid-walk",
				   p->name);
			if (added < n) {
				snprintf(key, sizeof(key), "n%03d", added++);
				zassert_ok(p->ops->set(root, key, 4, "new", 3));
			}

			memcpy(cur, k, kl);
			clen = kl;
		}
		zassert_equal(rc, -ENODATA, "%s: walk ended with %d", p->name, rc);

		for (int i = 0; i < n; i++) {
			zassert_equal(seen[i], 1, "%s: k%03d returned %d times",
				      p->name, i, seen[i]);
		}
	}
}

/* The key is the whole cursor: a walk resumes from it after a remount. */
ZTEST(map_contract, test_next_resumes_after_remount)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 40);
		char saved[WKEY];
		size_t saved_len;

		fill(p, root, 40);
		walk_all(p, root, &walk_a);
		zassert_equal(walk_a.n, 40, "%s", p->name);

		/* Take the first 15 steps, then lose all RAM state. */
		walk_b.n = 15;
		memcpy(walk_b.key, walk_a.key, sizeof(walk_a.key[0]) * 15);
		memcpy(walk_b.klen, walk_a.klen, sizeof(walk_a.klen[0]) * 15);
		saved_len = walk_a.klen[14];
		memcpy(saved, walk_a.key[14], saved_len);

		zassert_ok(blob_db_unmount());
		zassert_ok(blob_db_mount());

		walk_from(p, root, saved, saved_len, &walk_b);
		zassert_true(walks_equal(&walk_a, &walk_b),
			     "%s: resumed walk diverged", p->name);
	}
}

/*
 * A buffer too small is -ENOMEM with both true lengths reported, and since
 * nothing moved, the same cursor with larger buffers then succeeds.
 */
ZTEST(map_contract, test_next_too_small_reports_lengths)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		char k[WKEY], v[16];
		size_t kl = 0, vl = 0;

		zassert_ok(p->ops->set(root, "abcdef", 6, "12345", 5));

		zassert_equal(p->ops->next(root, NULL, 0, k, 2, &kl, v, sizeof(v), &vl),
			      -ENOMEM, "%s: small key buffer", p->name);
		zassert_equal(kl, 6, "%s", p->name);
		zassert_equal(vl, 5, "%s", p->name);

		kl = vl = 0;
		zassert_equal(p->ops->next(root, NULL, 0, k, sizeof(k), &kl, v, 2, &vl),
			      -ENOMEM, "%s: small value buffer", p->name);
		zassert_equal(kl, 6, "%s", p->name);
		zassert_equal(vl, 5, "%s", p->name);

		kl = vl = 0;
		zassert_equal(p->ops->next(root, NULL, 0, NULL, 0, &kl, NULL, 0, &vl),
			      -ENOMEM, "%s: size probe", p->name);
		zassert_equal(kl, 6, "%s", p->name);
		zassert_equal(vl, 5, "%s", p->name);

		zassert_ok(p->ops->next(root, NULL, 0, k, kl, &kl, v, vl, &vl),
			   "%s: retry at the reported sizes", p->name);
		zassert_mem_equal(k, "abcdef", 6, "%s", p->name);
		zassert_mem_equal(v, "12345", 5, "%s", p->name);
	}
}

/* -ENOENT means "not a map" and is never the end of a walk. */
ZTEST(map_contract, test_next_on_a_non_map_is_enoent)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t unbound = blob_db_alloc_id();
		uint64_t root = fresh_map(p, 8);
		char k[WKEY], v[16];
		size_t kl = 0, vl = 0;

		zassert_not_equal(unbound, 0);
		zassert_equal(p->ops->next(unbound, NULL, 0, k, sizeof(k), &kl,
					   v, sizeof(v), &vl),
			      -ENOENT, "%s: unbound root", p->name);

		zassert_ok(p->ops->set(root, "k", 1, "v", 1));
		zassert_ok(p->ops->destroy(root));
		zassert_equal(p->ops->next(root, NULL, 0, k, sizeof(k), &kl,
					   v, sizeof(v), &vl),
			      -ENOENT, "%s: destroyed root", p->name);
	}
}

ZTEST(map_contract, test_next_rejects_bad_arguments)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		char k[WKEY], v[16];
		size_t kl = 0, vl = 0;

		zassert_ok(p->ops->set(root, "k", 1, "v", 1));

		zassert_equal(p->ops->next(root, NULL, 3, k, sizeof(k), &kl,
					   v, sizeof(v), &vl),
			      -EINVAL, "%s: NULL cursor with a length", p->name);
		zassert_equal(p->ops->next(root, NULL, 0, NULL, 4, &kl,
					   v, sizeof(v), &vl),
			      -EINVAL, "%s: NULL key buffer with a size", p->name);
		zassert_equal(p->ops->next(root, NULL, 0, k, sizeof(k), &kl,
					   NULL, 4, &vl),
			      -EINVAL, "%s: NULL value buffer with a size", p->name);
	}
}

/* ------------------------------------------------------------------ */
/* count                                                               */
/* ------------------------------------------------------------------ */

/* The number of keys a full `next` walk returns -- count's reference. */
static size_t walk_len(const struct provider *p, uint64_t root)
{
	walk_all(p, root, &walk_b);
	return walk_b.n;
}

static size_t count_of(const struct provider *p, uint64_t root)
{
	size_t c = SIZE_MAX;

	zassert_ok(p->ops->count(root, &c), "%s: count", p->name);
	return c;
}

/* A fresh map and an emptied one both count 0. */
ZTEST(map_contract, test_count_of_empty_map_is_zero)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);

		zassert_equal(count_of(p, root), 0, "%s: fresh map", p->name);

		zassert_ok(p->ops->set(root, "k", 1, "v", 1));
		zassert_ok(p->ops->del(root, "k", 1));
		zassert_equal(count_of(p, root), 0, "%s: emptied map", p->name);
	}
}

/*
 * Inserts add one each; a replace, and a del of a missing key, change
 * nothing; a successful del removes one. At every step the figure agrees with
 * a full walk.
 */
ZTEST(map_contract, test_count_follows_inserts_and_deletes)
{
	const int n = 40;

	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, n);
		char key[8];

		for (int i = 0; i < n; i++) {
			snprintf(key, sizeof(key), "k%03d", i);
			zassert_ok(p->ops->set(root, key, 4, "v", 1));
			zassert_equal(count_of(p, root), (size_t)i + 1,
				      "%s: after insert %d", p->name, i);
		}

		for (int i = 0; i < n; i += 3) {
			snprintf(key, sizeof(key), "k%03d", i);
			zassert_ok(p->ops->set(root, key, 4, "replaced", 8));
		}
		zassert_equal(count_of(p, root), (size_t)n,
			      "%s: a replace changed the count", p->name);

		zassert_equal(p->ops->del(root, "absent", 6), -ENOENT);
		zassert_equal(count_of(p, root), (size_t)n,
			      "%s: a missed del changed the count", p->name);

		for (int i = 0; i < n; i += 2) {
			snprintf(key, sizeof(key), "k%03d", i);
			zassert_ok(p->ops->del(root, key, 4));
		}
		zassert_equal(count_of(p, root), (size_t)n / 2,
			      "%s: after deleting half", p->name);
		zassert_equal(count_of(p, root), walk_len(p, root),
			      "%s: count disagrees with a full walk", p->name);
	}
}

/* Computed from flash: the same figure after a remount, nothing cached. */
ZTEST(map_contract, test_count_survives_remount)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 40);

		fill(p, root, 25);
		zassert_ok(blob_db_unmount());
		zassert_ok(blob_db_mount());
		zassert_equal(count_of(p, root), 25, "%s", p->name);
		zassert_equal(walk_len(p, root), 25, "%s", p->name);
	}
}

/* count never writes. */
ZTEST(map_contract, test_count_writes_nothing)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);
		size_t blobs;

		fill(p, root, 5);
		blobs = blob_db_count();
		zassert_equal(count_of(p, root), 5, "%s", p->name);
		zassert_equal(blob_db_count(), blobs,
			      "%s: count changed the store", p->name);
	}
}

ZTEST(map_contract, test_count_on_a_non_map_is_enoent)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t unbound = blob_db_alloc_id();
		uint64_t root = fresh_map(p, 8);
		size_t c = 42;

		zassert_not_equal(unbound, 0);
		zassert_equal(p->ops->count(unbound, &c), -ENOENT,
			      "%s: unbound root", p->name);

		zassert_ok(p->ops->set(root, "k", 1, "v", 1));
		zassert_ok(p->ops->destroy(root));
		zassert_equal(p->ops->count(root, &c), -ENOENT,
			      "%s: destroyed root", p->name);
		zassert_equal(c, 42, "%s: *out written on failure", p->name);
	}
}

ZTEST(map_contract, test_count_rejects_null_out)
{
	FOR_EACH_PROVIDER(p) {
		uint64_t root = fresh_map(p, 8);

		zassert_equal(p->ops->count(root, NULL), -EINVAL, "%s", p->name);
	}
}

/* ================================================================== */
/* Tier 2 — UNSPECIFIED: pins today's behaviour, pending a shape edit  */
/* ================================================================== */

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
 * expected_entries sizes the map; it does not bound it. Understating it costs
 * depth and packing, never correctness -- a map declared for 2 still holds 16.
 *
 * Worth pinning because the field's ancestor (initial_capacity) was read as a
 * bucket count by this provider and as an entry count by the shape header,
 * and the two only diverge observably at exactly this edge (FINDINGS.md K9).
 */
ZTEST(kvhash_layout, test_declared_population_is_not_an_entry_limit)
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
 * A population the geometry cannot honour is refused at create, not quietly
 * built smaller. 100 000 entries want ~317 buckets per level; one directory
 * payload holds far fewer, and there is no third level.
 *
 * This inverts what this provider used to do -- clamp to what fits and say
 * nothing -- which is the mechanism behind FINDINGS.md K9: a caller that
 * declared a large population got a small map and no diagnostic, and only
 * met the consequence later as -ENOSPC on some unlucky key. Failing the
 * declaration outright is the cure, so pin it.
 */
ZTEST(kvhash_layout, test_unhonourable_population_is_refused)
{
	struct map_config cfg = { .expected_entries = 100000 };
	uint64_t root = blob_db_alloc_id();

	zassert_not_equal(root, 0);
	zassert_equal(kvhash_map_ops.create(root, &cfg), -EINVAL,
		      "a population past the payload's reach must be refused");
	zassert_false(blob_db_exists(root),
		      "a refused create must not leave a map behind");
}

/*
 * A contradictory declaration is arithmetic, so it is caught before anything
 * is written: an entry larger than a record can ever hold, and a typical
 * entry larger than the maximum.
 */
ZTEST(kvhash_layout, test_contradictory_config_is_refused)
{
	struct map_info info = { 0 };
	uint64_t root = blob_db_alloc_id();
	struct map_config too_big = {
		.expected_entries = 8,
		.max_entry_bytes = CONFIG_BLOB_DB_MAX_PAYLOAD_LEN + 1u,
	};
	struct map_config inverted = {
		.expected_entries = 8,
		.typical_entry_bytes = 64,
		.max_entry_bytes = 32,
	};

	zassert_not_equal(root, 0);
	zassert_equal(kvhash_map_ops.create(root, &too_big), -EINVAL,
		      "max_entry_bytes past the payload must be refused");
	zassert_equal(kvhash_map_ops.create(root, &inverted), -EINVAL,
		      "typical > max must be refused");
	zassert_false(blob_db_exists(root), "a refused create wrote anyway");

	/* And the limit the caller was measured against is discoverable. */
	zassert_ok(kvhash_map_ops.create(root, NULL));
	zassert_ok(kvhash_map_ops.stat(root, &info));
	zassert_true(info.entry_bytes_limit < CONFIG_BLOB_DB_MAX_PAYLOAD_LEN,
		     "entry_bytes_limit must leave room for the entry header");
}

/*
 * A population that one directory cannot address is built two levels deep
 * instead -- a directory of sub-directories -- and stat() is how a caller
 * learns that. The map must behave exactly like a flat one while it does,
 * and destroy must reclaim the whole tree, sub-directories included.
 */
ZTEST(kvhash_layout, test_large_population_builds_a_second_level)
{
	size_t baseline = blob_db_count();
	struct map_config cfg = { .expected_entries = 128 };
	struct map_info info = { 0 };
	uint64_t root = blob_db_alloc_id();
	char key[8], out[8];
	size_t len = 0;

	zassert_not_equal(root, 0);
	zassert_ok(kvhash_map_ops.create(root, &cfg));
	zassert_ok(kvhash_map_ops.stat(root, &info));

	zassert_equal(info.depth, 2,
		      "128 entries need more buckets than one directory addresses");
	zassert_true(info.fanout >= 2, "a second level with no fan-out");
	zassert_equal(info.buckets, (uint32_t)info.fanout * info.fanout,
		      "levels should be uniform: %u != %u^2",
		      info.buckets, info.fanout);

	for (int i = 0; i < 64; i++) {
		snprintf(key, sizeof(key), "k%03d", i);
		zassert_ok(kvhash_map_ops.set(root, key, strlen(key), "vvvv", 4),
			   "set %s", key);
	}
	for (int i = 0; i < 64; i++) {
		snprintf(key, sizeof(key), "k%03d", i);
		zassert_ok(kvhash_map_ops.get(root, key, strlen(key), out,
					      sizeof(out), &len), "get %s", key);
		zassert_equal(len, 4, "%s truncated", key);
	}

	zassert_ok(kvhash_map_ops.del(root, "k007", 4));
	zassert_equal(kvhash_map_ops.get(root, "k007", 4, out, sizeof(out), &len),
		      -ENOENT, "deleted key still readable two levels down");
	zassert_ok(kvhash_map_ops.get(root, "k008", 4, out, sizeof(out), &len),
		   "a neighbour went with it");

	zassert_ok(kvhash_map_ops.destroy(root), "destroy of a two-level map");
	zassert_equal(blob_db_count(), baseline,
		      "destroy left %zu blob(s) of the second level behind",
		      blob_db_count() - baseline);
}

/*
 * A bucket is one blob payload. Overflowing it is -ENOSPC, and — the part
 * that matters — the failed insert must leave the bucket exactly as it was.
 *
 * Ten 100-byte values into a map declared for 2 guarantee the overflow by
 * pigeonhole without hard-coding the hash: three such entries already exceed
 * the 256-byte default payload, so any bucket count up to 4 forces one. The
 * count is read back rather than assumed -- geometry is the container's to
 * choose, and the pigeonhole is only sound while it stays small.
 */
ZTEST(kvhash_layout, test_bucket_overflow_is_enospc_without_damage)
{
	uint64_t root = fresh_map(&kvhash, 2);
	struct map_info info = { 0 };
	char big[100];
	bool stored[10] = { false };
	int enospc = 0;

	memset(big, 'x', sizeof(big));

	zassert_ok(kvhash_map_ops.stat(root, &info));
	zassert_true(info.buckets <= 4,
		     "%u buckets breaks the pigeonhole this test rests on",
		     info.buckets);

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
 *   [u32 magic][u16 n_buckets][u8 version][u8 depth][u64 child_id]*n
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

/*
 * A walk of a two-level map crosses every sub-map, and skips buckets that
 * were allocated and then emptied rather than reporting them.
 */
ZTEST(kvhash_layout, test_next_walks_a_two_level_map)
{
	struct map_config cfg = { .expected_entries = 128 };
	struct map_info info = { 0 };
	uint64_t root = blob_db_alloc_id();
	char k[WKEY], v[16];
	size_t kl = 0, vl = 0;

	zassert_not_equal(root, 0);
	zassert_ok(kvhash_map_ops.create(root, &cfg));
	zassert_ok(kvhash_map_ops.stat(root, &info));
	zassert_equal(info.depth, 2);

	fill(&kvhash, root, 100);
	walk_all(&kvhash, root, &walk_a);
	assert_each_once("kvhash", &walk_a, 100);

	for (size_t i = 0; i < walk_a.n; i++) {
		zassert_ok(kvhash_map_ops.del(root, walk_a.key[i], walk_a.klen[i]));
	}
	zassert_equal(kvhash_map_ops.next(root, NULL, 0, k, sizeof(k), &kl,
					  v, sizeof(v), &vl),
		      -ENODATA, "emptied buckets must be skipped, not returned");
}

/*
 * count at both depths, with buckets that del emptied but left allocated:
 * those are still named by their directory and must count as zero, not be
 * skipped wrongly or miscounted.
 */
static void count_with_emptied_buckets(size_t declared, uint8_t want_depth)
{
	struct map_config cfg = { .expected_entries = declared };
	struct map_info info = { 0 };
	uint64_t root = blob_db_alloc_id();
	char key[8];
	size_t c = SIZE_MAX;

	zassert_not_equal(root, 0);
	zassert_ok(kvhash_map_ops.create(root, &cfg));
	zassert_ok(kvhash_map_ops.stat(root, &info));
	zassert_equal(info.depth, want_depth);

	fill(&kvhash, root, 100);
	zassert_ok(kvhash_map_ops.count(root, &c));
	zassert_equal(c, 100, "depth %u: full map", want_depth);

	/* Empty most buckets entirely: delete all but every tenth key. */
	for (int i = 0; i < 100; i++) {
		if (i % 10 != 0) {
			snprintf(key, sizeof(key), "k%03d", i);
			zassert_ok(kvhash_map_ops.del(root, key, 4));
		}
	}
	zassert_ok(kvhash_map_ops.count(root, &c));
	zassert_equal(c, 10, "depth %u: after emptying buckets", want_depth);
	walk_all(&kvhash, root, &walk_a);
	zassert_equal(c, walk_a.n, "depth %u: count %zu, walk %zu",
		      want_depth, c, walk_a.n);

	/* All gone: every bucket allocated and empty. */
	for (int i = 0; i < 100; i += 10) {
		snprintf(key, sizeof(key), "k%03d", i);
		zassert_ok(kvhash_map_ops.del(root, key, 4));
	}
	zassert_ok(kvhash_map_ops.count(root, &c));
	zassert_equal(c, 0, "depth %u: all buckets emptied", want_depth);
}

ZTEST(kvhash_layout, test_count_at_depth_one)
{
	count_with_emptied_buckets(100, 1);
}

ZTEST(kvhash_layout, test_count_at_depth_two)
{
	count_with_emptied_buckets(128, 2);
}

#endif /* CONFIG_BLOB_CONTAINER_KVHASH */
