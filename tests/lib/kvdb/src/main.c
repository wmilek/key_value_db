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
		.expected_entries = 16,
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
