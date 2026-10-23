/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * L1 contract acceptance suite — drives the model container
 * (doc/layers/l1_model_container.md) end-to-end on native_sim to PROVE the
 * whole concept:
 *
 *   - single-integer reachability (P5): the container is re-opened after every
 *     remount from nothing but id = 1 — the root registry lives there and
 *     maps MC_ROOTREG_KEY to the list root;
 *   - crash-safety (P7): at every step of every reference-graph mutation, a
 *     crash + remount + recovery yields the complete old OR complete new
 *     mapping — never torn, never dangling;
 *   - no permanent leak (P7 must): after recovery the live blob count is
 *     EXACTLY the structural minimum (root + intent + 2 per entry).
 *
 * Crashes are injected at blob_db-operation boundaries: a mutation stops after
 * a chosen step (MC_STOPPED), then we unmount/remount and mc_open() runs
 * recovery from that exact point — matching the crash tables in §8.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>

#include "model_container_map.h"

/* This suite is the container's CLIENT — it owns root-id persistence
 * (l1_root_registry.md §2) and registers the container under its key. */
#define MC_ROOTREG_KEY  ROOTREG_KEY(0x4d435f4d /* 'MC_M': model container, map */, 0)

/* Structural blob floor: the root registry (id = 1) + the list root + the
 * intent blob. Each live entry adds exactly two blobs (key + value). "No
 * leak" == live count matches this formula exactly. */
#define STRUCT_BLOBS  3

static void assert_no_leak(void)
{
	size_t entries;

	zassert_ok(mc_entry_count(&entries));
	zassert_equal(blob_db_count(), STRUCT_BLOBS + 2 * entries,
		      "leak: live blobs=%zu, expected=%zu (entries=%zu)",
		      blob_db_count(), STRUCT_BLOBS + 2 * entries, entries);
}

/* Client-side open: bootstrap the registry, resolve (or register) the
 * container's root id, hand it to the container. This is the l1½ pattern
 * every real client follows. */
static void open_container(void)
{
	uint64_t root;

	zassert_ok(rootreg_init());
	zassert_ok(rootreg_get_or_create(MC_ROOTREG_KEY, &root));
	zassert_ok(mc_open(root), "open/recovery failed");
}

/* Power-cut simulation: drop the mount and bring it back. The client
 * re-derives the entire container from id = 1 (registry -> list root) and
 * mc_open() runs intent recovery. */
static void crash_and_recover(void)
{
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	open_container();
}

/* Wipe to a virgin store and create an empty container. */
static void fresh_container(void)
{
	zassert_ok(blob_db_format());
	open_container();   /* virgin -> bootstrap registry + create */
}

static void before(void *f)
{
	ARG_UNUSED(f);
	mc_fail_after(MC_STEP_NONE, 0);
	mc_fail_key_read(0);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	fresh_container();
}

static void after(void *f)
{
	ARG_UNUSED(f);
	blob_db_unmount();
}

ZTEST_SUITE(blob_db_contract, NULL, NULL, before, after, NULL);

static int get_str(const char *key, char *out, size_t out_sz)
{
	size_t got;
	int rc = mc_get(key, out, out_sz, &got);

	if (rc == 0) {
		out[got] = '\0';
	}
	return rc;
}

/* --- basic flow (§3), completed mutations ------------------------------ */

ZTEST(blob_db_contract, test_empty_container)
{
	char buf[16];

	zassert_equal(get_str("foo", buf, sizeof(buf)), -ENOENT);
	assert_no_leak();
}

ZTEST(blob_db_contract, test_insert_lookup)
{
	zassert_ok(mc_set("foo", "bar", 3, MC_STEP_NONE));

	char buf[16];

	zassert_ok(get_str("foo", buf, sizeof(buf)));
	zassert_str_equal(buf, "bar");
	assert_no_leak();
}

ZTEST(blob_db_contract, test_overwrite)
{
	zassert_ok(mc_set("foo", "bar", 3, MC_STEP_NONE));
	zassert_ok(mc_set("foo", "bazzz", 5, MC_STEP_NONE));

	char buf[16];

	zassert_ok(get_str("foo", buf, sizeof(buf)));
	zassert_str_equal(buf, "bazzz");
	assert_no_leak();   /* old value blob reclaimed — count unchanged */
}

ZTEST(blob_db_contract, test_delete)
{
	zassert_ok(mc_set("foo", "bar", 3, MC_STEP_NONE));
	zassert_ok(mc_del("foo", MC_STEP_NONE));

	char buf[16];

	zassert_equal(get_str("foo", buf, sizeof(buf)), -ENOENT);
	assert_no_leak();   /* back to root + intent only */
}

/* --- single-integer reachability across remount ------------------------ */

ZTEST(blob_db_contract, test_multikey_persist_and_reopen)
{
	zassert_ok(mc_set("alpha", "1", 1, MC_STEP_NONE));
	zassert_ok(mc_set("beta", "22", 2, MC_STEP_NONE));
	zassert_ok(mc_set("gamma", "333", 3, MC_STEP_NONE));

	/* Reboot: the ONLY thing that persists is the integer 1. */
	crash_and_recover();

	char buf[16];

	zassert_ok(get_str("alpha", buf, sizeof(buf)));
	zassert_str_equal(buf, "1");
	zassert_ok(get_str("beta", buf, sizeof(buf)));
	zassert_str_equal(buf, "22");
	zassert_ok(get_str("gamma", buf, sizeof(buf)));
	zassert_str_equal(buf, "333");

	size_t entries;

	zassert_ok(mc_entry_count(&entries));
	zassert_equal(entries, 3);
	assert_no_leak();
}

/* --- crash tables (§8): recovery is correct AND leak-free -------------- */

struct crash_case {
	enum mc_crash_point cp;
	bool committed;
};

/* §8.1 Insert: committed once the list image is swapped (COMMIT). */
ZTEST(blob_db_contract, test_insert_crash_table)
{
	const struct crash_case cases[] = {
		{ MC_STEP_STAGE,   false },
		{ MC_STEP_PREPARE, false },
		{ MC_STEP_COMMIT,  true  },
		{ MC_STEP_CLEANUP, true  },
		{ MC_STEP_NONE,    true  },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		fresh_container();

		int rc = mc_set("foo", "bar", 3, cases[i].cp);

		if (cases[i].cp == MC_STEP_NONE) {
			zassert_ok(rc);
		} else {
			zassert_equal(rc, MC_STOPPED, "case %zu", i);
		}

		crash_and_recover();

		char buf[16];

		if (cases[i].committed) {
			zassert_ok(get_str("foo", buf, sizeof(buf)),
				   "case %zu should be committed", i);
			zassert_str_equal(buf, "bar");
		} else {
			zassert_equal(get_str("foo", buf, sizeof(buf)), -ENOENT,
				      "case %zu should be rolled back", i);
		}
		assert_no_leak();   /* prepared-but-abandoned blobs reclaimed */
	}
}

/* §8.2 Overwrite (genuine value replacement): committed at COMMIT. */
ZTEST(blob_db_contract, test_overwrite_crash_table)
{
	const struct crash_case cases[] = {
		{ MC_STEP_STAGE,   false },
		{ MC_STEP_PREPARE, false },
		{ MC_STEP_COMMIT,  true  },
		{ MC_STEP_CLEANUP, true  },
		{ MC_STEP_NONE,    true  },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		fresh_container();
		zassert_ok(mc_set("foo", "bar", 3, MC_STEP_NONE));   /* seed */

		int rc = mc_set("foo", "bazzz", 5, cases[i].cp);

		if (cases[i].cp == MC_STEP_NONE) {
			zassert_ok(rc);
		} else {
			zassert_equal(rc, MC_STOPPED, "case %zu", i);
		}

		crash_and_recover();

		char buf[16];

		zassert_ok(get_str("foo", buf, sizeof(buf)));
		zassert_str_equal(buf, cases[i].committed ? "bazzz" : "bar");

		size_t entries;

		zassert_ok(mc_entry_count(&entries));
		zassert_equal(entries, 1, "case %zu entries", i);
		assert_no_leak();   /* exactly one value blob survives */
	}
}

/* --- full-lifecycle cleanup: destroy + unregister ----------------------- */

/* Tear-down leaves the store at the absolute floor — ONLY the (empty-again)
 * registry survives — and a later open under the same key starts a brand-new
 * container on a fresh id (dead ids are never reused). */
ZTEST(blob_db_contract, test_destroy_and_unregister_cleanup)
{
	zassert_ok(mc_set("alpha", "1", 1, MC_STEP_NONE));
	zassert_ok(mc_set("beta", "22", 2, MC_STEP_NONE));
	zassert_ok(mc_set("gamma", "333", 3, MC_STEP_NONE));

	uint64_t old_root;

	zassert_ok(rootreg_get(MC_ROOTREG_KEY, &old_root));

	/* Cleanup: unset all keys + structure, then drop the registry entry
	 * (teardown BEFORE unregistration — registry §8). */
	zassert_ok(mc_destroy());
	zassert_ok(rootreg_unregister(MC_ROOTREG_KEY));

	/* Only the registry blob remains, and the container is truly gone. */
	zassert_equal(blob_db_count(), 1,
		      "want registry only, got %zu blobs", blob_db_count());
	zassert_false(blob_db_exists(old_root));

	char buf[8];

	zassert_equal(mc_get("alpha", buf, sizeof(buf), NULL), -ENODEV,
		      "container must be closed after destroy");

	/* Same key, fresh life: a new root id, an empty container. */
	open_container();

	uint64_t new_root;

	zassert_ok(rootreg_get(MC_ROOTREG_KEY, &new_root));
	zassert_not_equal(new_root, old_root, "dead ids must not be reused");
	zassert_equal(get_str("alpha", buf, sizeof(buf)), -ENOENT);
	assert_no_leak();
}

/* Destroy is re-runnable: a "crash" between destroy and unregister just
 * means the next open finds the registered id dead… which must NOT happen
 * silently — so the client contract is destroy-then-unregister with no
 * mutation in between. This test pins the safe rerun path instead: destroy
 * on an already-half-torn container (simulated by a double destroy cycle). */
ZTEST(blob_db_contract, test_destroy_empty_container)
{
	zassert_ok(mc_destroy());   /* no entries — deletes intent + root */
	zassert_ok(rootreg_unregister(MC_ROOTREG_KEY));
	zassert_equal(blob_db_count(), 1);

	open_container();           /* fresh container, fresh id */
	assert_no_leak();
}

/* --- an unreadable key is not an absent key ---------------------------- */

/* Lookups skip only a key that answers -ENOENT. Any other read error
 * reaches the caller before anything is written — above all it never
 * turns an overwrite into the insert of a second entry for the key. */
ZTEST(blob_db_contract, test_key_read_error_propagates)
{
	char buf[16];
	size_t n;

	zassert_ok(mc_set("foo", "bar", 3, MC_STEP_NONE));

	mc_fail_key_read(-EIO);
	zassert_equal(mc_set("foo", "baz", 3, MC_STEP_NONE), -EIO);
	zassert_ok(mc_entry_count(&n));
	zassert_equal(n, 1, "an unreadable key was inserted again");
	zassert_ok(get_str("foo", buf, sizeof(buf)));
	zassert_str_equal(buf, "bar");
	assert_no_leak();

	mc_fail_key_read(-EIO);
	zassert_equal(get_str("foo", buf, sizeof(buf)), -EIO);

	mc_fail_key_read(-EIO);
	zassert_equal(mc_del("foo", MC_STEP_NONE), -EIO);
	zassert_ok(get_str("foo", buf, sizeof(buf)));
	zassert_str_equal(buf, "bar");
	assert_no_leak();
}

/* --- direct id-1 binding ------------------------------------------------ */

/* A client without a registry may build the list at BLOB_DB_ROOT_ID itself.
 * That id is never unbound — a fresh store binds it empty — so an empty
 * payload must read as "create now", and destroy must reset it to empty
 * instead of deleting it. */
ZTEST(blob_db_contract, test_direct_root_binding)
{
	char buf[16];

	zassert_ok(blob_db_format());   /* id 1 bound, empty; no registry */
	zassert_ok(mc_open(BLOB_DB_ROOT_ID));
	zassert_ok(mc_set("foo", "bar", 3, MC_STEP_NONE));

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	zassert_ok(mc_open(BLOB_DB_ROOT_ID));
	zassert_ok(get_str("foo", buf, sizeof(buf)));
	zassert_str_equal(buf, "bar");
	/* id 1 (list root) + intent + key + value */
	zassert_equal(blob_db_count(), 4, "got %zu blobs", blob_db_count());

	zassert_ok(mc_destroy());
	zassert_true(blob_db_exists(BLOB_DB_ROOT_ID));
	zassert_equal(blob_db_count(), 1, "got %zu blobs", blob_db_count());

	zassert_ok(mc_open(BLOB_DB_ROOT_ID));   /* fresh container */
	zassert_equal(get_str("foo", buf, sizeof(buf)), -ENOENT);
	zassert_equal(blob_db_count(), 2, "got %zu blobs", blob_db_count());
}

/* A root whose payload is not shaped like a list is refused, and leaves
 * the container closed. */
ZTEST(blob_db_contract, test_malformed_root_refused)
{
	uint64_t id = blob_db_alloc_id();
	char buf[16];

	zassert_not_equal(id, BLOB_DB_INVALID_ID);
	zassert_ok(blob_db_update(id, "xyz", 3));
	zassert_equal(mc_open(id), -EIO);
	zassert_equal(get_str("foo", buf, sizeof(buf)), -ENODEV);
}

/* --- recovery touches only the ids the intent names -------------------- */

/* A second structure in the same store, registered under its own key. */
#define OTHER_ROOTREG_KEY  ROOTREG_KEY(0x4f544852 /* 'OTHR' */, 0)

/* Firmware opens stores lazily, so after a reboot another client may
 * allocate and bind blobs BEFORE this container's mc_open() runs recovery.
 * Those blobs have ids above anything the interrupted mutation allocated,
 * and the container's list does not reference them. Rolling back must still
 * leave them alone: recovery deletes only what its intent named. */
ZTEST(blob_db_contract, test_rollback_spares_other_store)
{
	zassert_equal(mc_set("foo", "bar", 3, MC_STEP_PREPARE), MC_STOPPED);

	/* Reboot, but let another store come up and write first. */
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	zassert_ok(rootreg_init());

	uint64_t other;

	zassert_ok(rootreg_get_or_create(OTHER_ROOTREG_KEY, &other));
	zassert_ok(blob_db_update(other, "cfg", 3));

	/* Only now is the container opened, and its insert rolled back. */
	open_container();

	char buf[16];
	size_t got;

	zassert_equal(get_str("foo", buf, sizeof(buf)), -ENOENT);
	zassert_ok(blob_db_get(other, buf, sizeof(buf), &got),
		   "recovery deleted another store's blob");
	zassert_equal(got, 3);
	zassert_mem_equal(buf, "cfg", 3);

	/* No leak either: the container's floor plus the other store's root. */
	zassert_equal(blob_db_count(), STRUCT_BLOBS + 1,
		      "live blobs=%zu, expected=%d", blob_db_count(),
		      STRUCT_BLOBS + 1);
}

/* A full list is refused before anything is written: nothing to undo, and
 * nothing left behind. */
ZTEST(blob_db_contract, test_failed_set_leaks_nothing)
{
	char key[8];
	int rc;
	size_t n = 0;

	do {
		snprintk(key, sizeof(key), "k%zu", n++);
		rc = mc_set(key, "v", 1, MC_STEP_NONE);
	} while (rc == 0);

	zassert_equal(rc, -ENOSPC);
	assert_no_leak();

	/* The container still works, and a later mutation leaks nothing. */
	zassert_ok(mc_del("k0", MC_STEP_NONE));
	zassert_ok(mc_set("late", "v", 1, MC_STEP_NONE));
	assert_no_leak();
}

/* --- clean failures (§5 abort path), no remount ----------------------- */

/* A step that fails without a power cut aborts through the same recovery a
 * remount runs: rolled back (error returned) before COMMIT, finished (0
 * returned) after it. Either way nothing leaks, without remounting. */
ZTEST(blob_db_contract, test_failed_mutation_aborts_cleanly)
{
	const struct crash_case cases[] = {
		{ MC_STEP_STAGE,   false },
		{ MC_STEP_PREPARE, false },
		{ MC_STEP_COMMIT,  true  },
		{ MC_STEP_CLEANUP, true  },
	};
	char buf[16];

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		const bool done = cases[i].committed;

		/* Insert. */
		fresh_container();
		mc_fail_after(cases[i].cp, -EIO);
		zassert_equal(mc_set("foo", "bar", 3, MC_STEP_NONE),
			      done ? 0 : -EIO, "insert case %zu", i);
		if (done) {
			zassert_ok(get_str("foo", buf, sizeof(buf)));
			zassert_str_equal(buf, "bar");
		} else {
			zassert_equal(get_str("foo", buf, sizeof(buf)), -ENOENT);
		}
		assert_no_leak();

		/* Overwrite. */
		fresh_container();
		zassert_ok(mc_set("foo", "bar", 3, MC_STEP_NONE));
		mc_fail_after(cases[i].cp, -EIO);
		zassert_equal(mc_set("foo", "bazzz", 5, MC_STEP_NONE),
			      done ? 0 : -EIO, "overwrite case %zu", i);
		zassert_ok(get_str("foo", buf, sizeof(buf)));
		zassert_str_equal(buf, done ? "bazzz" : "bar");
		assert_no_leak();

		/* Delete. */
		fresh_container();
		zassert_ok(mc_set("foo", "bar", 3, MC_STEP_NONE));
		mc_fail_after(cases[i].cp, -EIO);
		zassert_equal(mc_del("foo", MC_STEP_NONE),
			      done ? 0 : -EIO, "delete case %zu", i);
		zassert_equal(get_str("foo", buf, sizeof(buf)),
			      done ? -ENOENT : 0);
		assert_no_leak();

		/* The container keeps working after the abort. */
		zassert_ok(mc_set("next", "v", 1, MC_STEP_NONE));
		assert_no_leak();
	}
}

/* An interrupted mutation that nobody recovered yet (no remount) is
 * finished by the next mutation before it stages, and by destroy before it
 * deletes, so its prepared blobs are never stranded. */
ZTEST(blob_db_contract, test_staged_intent_finished_before_next_mutation)
{
	zassert_equal(mc_set("foo", "bar", 3, MC_STEP_PREPARE), MC_STOPPED);
	zassert_ok(mc_set("other", "v", 1, MC_STEP_NONE));

	char buf[16];

	zassert_equal(get_str("foo", buf, sizeof(buf)), -ENOENT);
	assert_no_leak();

	zassert_equal(mc_set("foo", "bar", 3, MC_STEP_PREPARE), MC_STOPPED);
	zassert_ok(mc_destroy());
	zassert_ok(rootreg_unregister(MC_ROOTREG_KEY));
	zassert_equal(blob_db_count(), 1,
		      "want registry only, got %zu blobs", blob_db_count());
}

/* §8.3 Delete: committed once the entry is unlinked (COMMIT). */
ZTEST(blob_db_contract, test_delete_crash_table)
{
	const struct crash_case cases[] = {
		{ MC_STEP_STAGE,   false },
		{ MC_STEP_PREPARE, false },
		{ MC_STEP_COMMIT,  true  },
		{ MC_STEP_CLEANUP, true  },
		{ MC_STEP_NONE,    true  },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		fresh_container();
		zassert_ok(mc_set("foo", "bar", 3, MC_STEP_NONE));   /* seed */

		int rc = mc_del("foo", cases[i].cp);

		if (cases[i].cp == MC_STEP_NONE) {
			zassert_ok(rc);
		} else {
			zassert_equal(rc, MC_STOPPED, "case %zu", i);
		}

		crash_and_recover();

		char buf[16];

		if (cases[i].committed) {
			zassert_equal(get_str("foo", buf, sizeof(buf)), -ENOENT,
				      "case %zu should be deleted", i);
		} else {
			zassert_ok(get_str("foo", buf, sizeof(buf)),
				   "case %zu should be intact", i);
			zassert_str_equal(buf, "bar");
		}
		assert_no_leak();   /* orphaned key/value blobs reclaimed */
	}
}
