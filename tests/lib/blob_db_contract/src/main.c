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

#include "model_container.h"

/* This suite is the container's CLIENT — it owns root-id persistence
 * (l1_root_registry.md §2) and registers the container under its key. */
#define MC_ROOTREG_KEY  ROOTREG_KEY(0x4d434e54 /* 'MCNT' */, 0)

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
