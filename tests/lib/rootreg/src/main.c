/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite for rootreg — cases mirror doc/layers/l1_root_registry.md §10.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>

#define KEY_A  ROOTREG_KEY(0x4b564442 /* 'KVDB' */, 0)
#define KEY_B  ROOTREG_KEY(0x424c4653 /* 'BLFS' */, 0)
#define KEY_A1 ROOTREG_KEY(0x4b564442 /* 'KVDB' */, 1)

static void rootreg_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
	zassert_ok(rootreg_init());
}

static void rootreg_after(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
}

ZTEST_SUITE(rootreg, NULL, NULL, rootreg_before, rootreg_after, NULL);

/* §10.1 — virgin bootstrap: id 1 bound, magic/version correct (proved by
 * the API accepting it), and init is idempotent. */
ZTEST(rootreg, test_virgin_bootstrap_and_idempotent_init)
{
	uint64_t id;

	zassert_equal(rootreg_get(KEY_A, &id), -ENOENT,
		      "fresh registry must be empty");
	zassert_ok(rootreg_init());   /* second init is a no-op */
	zassert_ok(rootreg_init());
}

/* §10.2 — foreign id 1: refuse with -ENOTSUP and leave the payload alone. */
ZTEST(rootreg, test_foreign_root_refused)
{
	static const char foreign[] = "not a registry";

	zassert_ok(blob_db_format());
	zassert_ok(blob_db_update(BLOB_DB_ROOT_ID, foreign, sizeof(foreign)));

	zassert_equal(rootreg_init(), -ENOTSUP);

	uint64_t id;

	zassert_equal(rootreg_get(KEY_A, &id), -ENOTSUP);
	zassert_equal(rootreg_set(KEY_A, 42), -ENOTSUP);

	/* The foreign payload must be untouched. */
	char buf[32];
	size_t got;

	zassert_ok(blob_db_get(BLOB_DB_ROOT_ID, buf, sizeof(buf), &got));
	zassert_equal(got, sizeof(foreign));
	zassert_mem_equal(buf, foreign, sizeof(foreign));
}

/* §10.3 — get/set/unregister round-trips, -EEXIST, -ENOSPC at cap. */
ZTEST(rootreg, test_set_get_unregister_roundtrip)
{
	uint64_t id;

	zassert_ok(rootreg_set(KEY_A, 1234));
	zassert_ok(rootreg_get(KEY_A, &id));
	zassert_equal(id, 1234);

	zassert_equal(rootreg_set(KEY_A, 999), -EEXIST);
	zassert_ok(rootreg_get(KEY_A, &id));
	zassert_equal(id, 1234, "-EEXIST must not modify the entry");

	zassert_ok(rootreg_unregister(KEY_A));
	zassert_equal(rootreg_get(KEY_A, &id), -ENOENT);
	zassert_equal(rootreg_unregister(KEY_A), -ENOENT);
}

ZTEST(rootreg, test_capacity_enforced)
{
	/* Fill to CONFIG_ROOTREG_MAX_ROOTS with distinct instance numbers. */
	for (uint32_t i = 0; i < CONFIG_ROOTREG_MAX_ROOTS; i++) {
		zassert_ok(rootreg_set(ROOTREG_KEY(0x54455354, i), 100 + i),
			   "set %u failed", i);
	}
	zassert_equal(rootreg_set(KEY_B, 42), -ENOSPC);

	uint64_t id;

	zassert_equal(rootreg_get_or_create(KEY_B, &id), -ENOSPC);

	/* Everything registered before the cap is still readable. */
	for (uint32_t i = 0; i < CONFIG_ROOTREG_MAX_ROOTS; i++) {
		zassert_ok(rootreg_get(ROOTREG_KEY(0x54455354, i), &id));
		zassert_equal(id, 100 + i);
	}

	/* Unregistering one frees a slot. */
	zassert_ok(rootreg_unregister(ROOTREG_KEY(0x54455354, 0)));
	zassert_ok(rootreg_set(KEY_B, 42));
}

/* §10.4 — get_or_create idempotence: same key -> same id, across remounts. */
ZTEST(rootreg, test_get_or_create_idempotent_across_remount)
{
	uint64_t id1, id2;

	zassert_ok(rootreg_get_or_create(KEY_A, &id1));
	zassert_true(id1 >= 2, "allocated root must be a user id");

	zassert_ok(rootreg_get_or_create(KEY_A, &id2));
	zassert_equal(id2, id1);

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	zassert_ok(rootreg_init());

	zassert_ok(rootreg_get_or_create(KEY_A, &id2));
	zassert_equal(id2, id1, "same key must yield same id after remount");

	/* A different instance of the same magic is a different root. */
	zassert_ok(rootreg_get_or_create(KEY_A1, &id2));
	zassert_not_equal(id2, id1);
}

/* §10.5 — registered-but-unbound is a defined, recoverable state: the caller
 * sees -ENOENT from get(), binds the root, and the binding survives. */
ZTEST(rootreg, test_registered_unbound_root_is_bindable)
{
	uint64_t id;

	zassert_ok(rootreg_get_or_create(KEY_A, &id));

	/* Creation "crashed" before the caller's bind: entry exists, blob
	 * doesn't. */
	uint8_t buf[8];
	size_t got;

	zassert_equal(blob_db_get(id, buf, sizeof(buf), &got), -ENOENT);

	/* Next boot: same id comes back; caller lazily binds it. */
	uint64_t again;

	zassert_ok(rootreg_get_or_create(KEY_A, &again));
	zassert_equal(again, id);
	zassert_ok(blob_db_update(id, "root", 4));

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_equal(got, 4);
	zassert_mem_equal(buf, "root", 4);
}

/* §10.6 — persistence: all entries survive a remount intact. */
ZTEST(rootreg, test_entries_persist_across_remount)
{
	zassert_ok(rootreg_set(KEY_A, 10));
	zassert_ok(rootreg_set(KEY_B, 20));
	zassert_ok(rootreg_set(KEY_A1, 30));

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	zassert_ok(rootreg_init());

	uint64_t id;

	zassert_ok(rootreg_get(KEY_A, &id));
	zassert_equal(id, 10);
	zassert_ok(rootreg_get(KEY_B, &id));
	zassert_equal(id, 20);
	zassert_ok(rootreg_get(KEY_A1, &id));
	zassert_equal(id, 30);
}

struct iter_acc {
	int n;
	uint64_t sum_ids;
};

static int iter_cb(uint64_t key, uint64_t root_id, void *user)
{
	struct iter_acc *a = user;

	ARG_UNUSED(key);
	a->n++;
	a->sum_ids += root_id;
	return 0;
}

static int iter_stop_cb(uint64_t key, uint64_t root_id, void *user)
{
	ARG_UNUSED(key);
	ARG_UNUSED(root_id);
	ARG_UNUSED(user);
	return 77;
}

ZTEST(rootreg, test_iterate_visits_all_and_stops_early)
{
	zassert_ok(rootreg_set(KEY_A, 10));
	zassert_ok(rootreg_set(KEY_B, 20));

	struct iter_acc a = { 0 };

	zassert_ok(rootreg_iterate(iter_cb, &a));
	zassert_equal(a.n, 2);
	zassert_equal(a.sum_ids, 30);

	zassert_equal(rootreg_iterate(iter_stop_cb, NULL), 77);
}

/* Key 0 is reserved. */
ZTEST(rootreg, test_key_zero_invalid)
{
	uint64_t id;

	zassert_equal(rootreg_get(0, &id), -EINVAL);
	zassert_equal(rootreg_set(0, 42), -EINVAL);
	zassert_equal(rootreg_get_or_create(0, &id), -EINVAL);
	zassert_equal(rootreg_unregister(0), -EINVAL);
}
