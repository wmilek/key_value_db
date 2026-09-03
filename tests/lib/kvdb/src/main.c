/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite for kvdb (L3) over the kvhash backend.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>
#include <app/lib/kvdb.h>

static void kvdb_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
	zassert_ok(rootreg_init());
}

static void kvdb_after(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
}

ZTEST_SUITE(kvdb, NULL, NULL, kvdb_before, kvdb_after, NULL);

/* Helper: open a hash instance by name with default sizing. */
static kvdb_t open_hash(const char *name)
{
	struct kvdb_config cfg = { .backend = KVDB_BACKEND_HASH };
	kvdb_t db;

	zassert_ok(kvdb_open(&db, name, &cfg), "open '%s'", name);
	return db;
}

/* set / get round-trip, and a miss returns -ENOENT. */
ZTEST(kvdb, test_set_get_roundtrip)
{
	kvdb_t db = open_hash("cfg");
	char out[32];
	size_t len = 0;

	zassert_equal(kvdb_get(&db, "missing", out, sizeof(out), &len), -ENOENT);

	zassert_ok(kvdb_set(&db, "color", "green", 5));
	zassert_ok(kvdb_get(&db, "color", out, sizeof(out), &len));
	zassert_equal(len, 5);
	zassert_mem_equal(out, "green", 5);
}

/* A second set on the same key replaces the value, not appends. */
ZTEST(kvdb, test_overwrite_replaces)
{
	kvdb_t db = open_hash("cfg");
	char out[32];
	size_t len = 0;

	zassert_ok(kvdb_set(&db, "k", "aaaa", 4));
	zassert_ok(kvdb_set(&db, "k", "bb", 2));

	zassert_ok(kvdb_get(&db, "k", out, sizeof(out), &len));
	zassert_equal(len, 2, "overwrite must not leave stale length");
	zassert_mem_equal(out, "bb", 2);
}

/* delete removes the key; deleting a missing key is -ENOENT. */
ZTEST(kvdb, test_delete)
{
	kvdb_t db = open_hash("cfg");
	char out[8];

	zassert_ok(kvdb_set(&db, "gone", "x", 1));
	zassert_true(kvdb_has(&db, "gone"));
	zassert_ok(kvdb_delete(&db, "gone"));
	zassert_false(kvdb_has(&db, "gone"));
	zassert_equal(kvdb_get(&db, "gone", out, sizeof(out), NULL), -ENOENT);
	zassert_equal(kvdb_delete(&db, "gone"), -ENOENT);
}

/* A too-small out buffer reports -ENOMEM and the true length. */
ZTEST(kvdb, test_get_buffer_too_small)
{
	kvdb_t db = open_hash("cfg");
	char out[3];
	size_t len = 0;

	zassert_ok(kvdb_set(&db, "big", "abcdef", 6));
	zassert_equal(kvdb_get(&db, "big", out, sizeof(out), &len), -ENOMEM);
	zassert_equal(len, 6, "true length reported even on -ENOMEM");
}

/* Zero-length values are legal and distinct from "absent". */
ZTEST(kvdb, test_empty_value)
{
	kvdb_t db = open_hash("cfg");
	char out[4];
	size_t len = 7;

	zassert_ok(kvdb_set(&db, "flag", NULL, 0));
	zassert_true(kvdb_has(&db, "flag"));
	zassert_ok(kvdb_get(&db, "flag", out, sizeof(out), &len));
	zassert_equal(len, 0);
}

/* Distinct names are independent stores. */
ZTEST(kvdb, test_multiple_instances_independent)
{
	kvdb_t a = open_hash("alpha");
	kvdb_t b = open_hash("beta");
	char out[16];
	size_t len = 0;

	zassert_ok(kvdb_set(&a, "who", "A", 1));
	zassert_ok(kvdb_set(&b, "who", "B", 1));

	zassert_ok(kvdb_get(&a, "who", out, sizeof(out), &len));
	zassert_mem_equal(out, "A", 1);
	zassert_ok(kvdb_get(&b, "who", out, sizeof(out), &len));
	zassert_mem_equal(out, "B", 1);
}

/* Re-opening the same name attaches to the existing store; cfg is ignored. */
ZTEST(kvdb, test_reopen_attaches)
{
	{
		kvdb_t db = open_hash("persist");

		zassert_ok(kvdb_set(&db, "keep", "yes", 3));
	}

	/* NULL cfg -> default backend; must still bind the stored hash. */
	kvdb_t again;

	zassert_ok(kvdb_open(&again, "persist", NULL));

	char out[8];
	size_t len = 0;

	zassert_ok(kvdb_get(&again, "keep", out, sizeof(out), &len));
	zassert_equal(len, 3);
	zassert_mem_equal(out, "yes", 3);
}

/* Data and the backend choice survive an unmount/remount cycle. */
ZTEST(kvdb, test_persistence_across_remount)
{
	{
		kvdb_t db = open_hash("durable");

		zassert_ok(kvdb_set(&db, "a", "1", 1));
		zassert_ok(kvdb_set(&db, "b", "22", 2));
	}

	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(rootreg_init());

	kvdb_t db;

	zassert_ok(kvdb_open(&db, "durable", NULL));

	char out[8];
	size_t len = 0;

	zassert_ok(kvdb_get(&db, "a", out, sizeof(out), &len));
	zassert_equal(len, 1);
	zassert_mem_equal(out, "1", 1);
	zassert_ok(kvdb_get(&db, "b", out, sizeof(out), &len));
	zassert_equal(len, 2);
	zassert_mem_equal(out, "22", 2);
}

/* Many keys spanning multiple buckets all round-trip. */
ZTEST(kvdb, test_many_keys_span_buckets)
{
	struct kvdb_config cfg = {
		.backend = KVDB_BACKEND_HASH,
		.initial_capacity = 16,
	};
	kvdb_t db;

	zassert_ok(kvdb_open(&db, "many", &cfg));

	for (int i = 0; i < 40; i++) {
		char key[8];
		int val = i * 7;

		snprintk(key, sizeof(key), "k%d", i);
		zassert_ok(kvdb_set(&db, key, &val, sizeof(val)), "set %s", key);
	}

	for (int i = 0; i < 40; i++) {
		char key[8];
		int val = 0;
		size_t len = 0;

		snprintk(key, sizeof(key), "k%d", i);
		zassert_ok(kvdb_get(&db, key, &val, sizeof(val), &len), "get %s", key);
		zassert_equal(len, sizeof(val));
		zassert_equal(val, i * 7, "wrong value for %s", key);
	}
}

/* Requesting a backend that is not compiled in fails cleanly at create. */
ZTEST(kvdb, test_unavailable_backend_rejected)
{
	struct kvdb_config cfg = { .backend = KVDB_BACKEND_TREE };
	kvdb_t db;

	zassert_equal(kvdb_open(&db, "tree", &cfg), -ENOTSUP);
}

/* ---- id-valued entries (kvdb_set_id / kvdb_get_id) ---- */

/* A reference round-trips, and a missing key is still -ENOENT. */
ZTEST(kvdb, test_id_roundtrip)
{
	kvdb_t db = open_hash("refs");
	uint64_t target = blob_db_alloc_id();
	uint64_t got = 0;

	zassert_not_equal(target, 0, "alloc_id must not yield 0");
	zassert_equal(kvdb_get_id(&db, "missing", &got), -ENOENT);

	zassert_ok(kvdb_set_id(&db, "child", target));
	zassert_ok(kvdb_get_id(&db, "child", &got));
	zassert_equal(got, target);
}

/* Nothing new on flash: a reference is exactly the 8 bytes of the id, so the
 * untyped and typed accessors read each other's writes. */
ZTEST(kvdb, test_id_is_plain_eight_bytes)
{
	kvdb_t db = open_hash("refs");
	uint64_t target = blob_db_alloc_id();
	uint64_t raw = 0;
	size_t len = 0;

	zassert_ok(kvdb_set_id(&db, "child", target));
	zassert_ok(kvdb_get(&db, "child", &raw, sizeof(raw), &len));
	zassert_equal(len, MAP_IDREF_VALUE_LEN, "a reference must be 8 bytes");
	zassert_equal(raw, target);

	/* ...and the same value written untyped reads back as a reference. */
	uint64_t other = blob_db_alloc_id();
	uint64_t got = 0;

	zassert_ok(kvdb_set(&db, "hand", &other, sizeof(other)));
	zassert_ok(kvdb_get_id(&db, "hand", &got));
	zassert_equal(got, other);
}

/* Reading a data-valued key as a reference fails instead of inventing an id. */
ZTEST(kvdb, test_id_rejects_non_reference_value)
{
	kvdb_t db = open_hash("refs");
	uint64_t got = 0xdeadbeef;
	uint64_t zero = 0;

	/* Too short. */
	zassert_ok(kvdb_set(&db, "short", "green", 5));
	zassert_equal(kvdb_get_id(&db, "short", &got), -EINVAL);

	/* Too long — must not leak through as -ENOMEM. */
	zassert_ok(kvdb_set(&db, "long", "0123456789ab", 12));
	zassert_equal(kvdb_get_id(&db, "long", &got), -EINVAL);

	/* Empty. */
	zassert_ok(kvdb_set(&db, "empty", NULL, 0));
	zassert_equal(kvdb_get_id(&db, "empty", &got), -EINVAL);

	/* Right length, but zero is not a valid id. */
	zassert_ok(kvdb_set(&db, "zero", &zero, sizeof(zero)));
	zassert_equal(kvdb_get_id(&db, "zero", &got), -EINVAL);

	zassert_equal(got, 0xdeadbeef, "out_id must be untouched on failure");
}

/* Bad arguments are rejected, including a zero id on the write side. */
ZTEST(kvdb, test_id_argument_validation)
{
	kvdb_t db = open_hash("refs");
	uint64_t got = 0;

	zassert_equal(kvdb_set_id(&db, "k", 0), -EINVAL, "0 is never a valid id");
	zassert_equal(kvdb_set_id(NULL, "k", 42), -EINVAL);
	zassert_equal(kvdb_set_id(&db, NULL, 42), -EINVAL);
	zassert_equal(kvdb_get_id(NULL, "k", &got), -EINVAL);
	zassert_equal(kvdb_get_id(&db, NULL, &got), -EINVAL);
	zassert_equal(kvdb_get_id(&db, "k", NULL), -EINVAL);
}

/* Deleting a reference entry removes the entry only — the blob it named is
 * untouched. This is the deliberate no-ownership rule, not an oversight. */
ZTEST(kvdb, test_id_delete_does_not_touch_target)
{
	kvdb_t db = open_hash("refs");
	uint64_t target = blob_db_alloc_id();

	zassert_ok(blob_db_update(target, "payload", 7));
	zassert_ok(kvdb_set_id(&db, "child", target));

	zassert_ok(kvdb_delete(&db, "child"));

	uint64_t got = 0;

	zassert_equal(kvdb_get_id(&db, "child", &got), -ENOENT);
	zassert_true(blob_db_exists(target), "target blob must survive its entry");

	char out[8];
	size_t len = 0;

	zassert_ok(blob_db_get(target, out, sizeof(out), &len));
	zassert_equal(len, 7);
	zassert_mem_equal(out, "payload", 7);
}

/* References survive a remount, and still resolve to a live blob. */
ZTEST(kvdb, test_id_persistence_across_remount)
{
	uint64_t target;

	{
		kvdb_t db = open_hash("refdur");

		target = blob_db_alloc_id();
		zassert_ok(blob_db_update(target, "v", 1));
		zassert_ok(kvdb_set_id(&db, "child", target));
	}

	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(rootreg_init());

	kvdb_t db;

	zassert_ok(kvdb_open(&db, "refdur", NULL));

	uint64_t got = 0;

	zassert_ok(kvdb_get_id(&db, "child", &got));
	zassert_equal(got, target);
	zassert_true(blob_db_exists(got));
}

/* Name bounds are enforced. */
ZTEST(kvdb, test_name_validation)
{
	kvdb_t db;
	char toolong[KVDB_NAME_MAX + 8];

	memset(toolong, 'x', sizeof(toolong));
	toolong[sizeof(toolong) - 1] = '\0';

	zassert_equal(kvdb_open(&db, "", NULL), -EINVAL);
	zassert_equal(kvdb_open(&db, toolong, NULL), -EINVAL);
	zassert_equal(kvdb_open(NULL, "ok", NULL), -EINVAL);
}
