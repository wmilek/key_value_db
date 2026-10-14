/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * L1 contract acceptance suite, second model container: the doubly linked
 * list (doc/layers/l1_model_container_list.md). Same proof as main.c, for a
 * structure whose every structural mutation changes two existing i-nodes:
 *
 *   - single-integer reachability (P5): reopened after every remount from
 *     id = 1, where the root registry maps MCL_ROOTREG_KEY to the list root;
 *   - crash-safety (P7): at every step of every insert and remove, a crash +
 *     remount + recovery yields the complete old OR complete new list, read
 *     forwards AND backwards — the back chain always mirrors the forward one;
 *   - no permanent leak (P7 must): after recovery the live blob count is
 *     EXACTLY root registry + root + intent + 2 per node.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>

#include "model_container_list.h"

#define MCL_ROOTREG_KEY  ROOTREG_KEY(0x4d435f4c /* 'MC_L': model container, list */, 0)
#define OTHER_ROOTREG_KEY  ROOTREG_KEY(0x4f544852 /* 'OTHR' */, 0)

/* Root registry (id = 1) + list root + intent. Each node adds two blobs:
 * the node and its value. */
#define STRUCT_BLOBS  3

static void open_list(void)
{
	uint64_t root;

	zassert_ok(rootreg_init());
	zassert_ok(rootreg_get_or_create(MCL_ROOTREG_KEY, &root));
	zassert_ok(mcl_open(root), "open/recovery failed");
}

static void crash_and_recover(void)
{
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	open_list();
}

static void fresh_list(void)
{
	mcl_fail_after(MCL_STEP_NONE, 0);
	zassert_ok(blob_db_format());
	open_list();
}

static void before(void *f)
{
	ARG_UNUSED(f);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	fresh_list();
}

static void after(void *f)
{
	ARG_UNUSED(f);
	blob_db_unmount();
}

ZTEST_SUITE(blob_db_contract_list, NULL, NULL, before, after, NULL);

/* Values in these tests are single characters, so a whole list reads as a
 * string: "abc" is a -> b -> c. */
static char val_of(uint64_t id)
{
	char v[4];
	size_t got;

	zassert_ok(mcl_get(id, v, sizeof(v), &got));
	zassert_equal(got, 1);
	return v[0];
}

/* The list reads `want` forwards and backwards, the back chain mirrors the
 * forward one node by node, and nothing leaked. */
static void expect_list(const char *want, const char *ctx)
{
	size_t n = strlen(want);
	size_t count;
	uint64_t id = 0;

	for (size_t i = 0; i < n; i++) {
		zassert_ok(mcl_next(id, &id));
		zassert_not_equal(id, 0, "%s: list ends at %zu, want \"%s\"",
				  ctx, i, want);
		zassert_equal(val_of(id), want[i], "%s: forward [%zu], want \"%s\"",
			      ctx, i, want);
	}
	zassert_ok(mcl_next(id, &id));
	zassert_equal(id, 0, "%s: list longer than \"%s\"", ctx, want);

	for (size_t i = n; i-- > 0;) {
		zassert_ok(mcl_prev(id, &id));
		zassert_not_equal(id, 0, "%s: back chain ends early", ctx);
		zassert_equal(val_of(id), want[i], "%s: backward [%zu], want \"%s\"",
			      ctx, i, want);
	}
	zassert_ok(mcl_prev(id, &id));
	zassert_equal(id, 0, "%s: back chain longer than \"%s\"", ctx, want);

	zassert_ok(mcl_check(&count), "%s: links disagree", ctx);
	zassert_equal(count, n);
	zassert_equal(blob_db_count(), STRUCT_BLOBS + 2 * n,
		      "%s: leak: live blobs=%zu, expected=%zu", ctx,
		      blob_db_count(), STRUCT_BLOBS + 2 * n);
}

/* Append each character of `vals` as a node; ids[i] is the i-th node. */
static void seed(const char *vals, uint64_t *ids)
{
	for (size_t i = 0; vals[i] != '\0'; i++) {
		zassert_ok(mcl_insert_before(0, &vals[i], 1, &ids[i],
					     MCL_STEP_NONE));
	}
}

/* --- basic operations ------------------------------------------------- */

ZTEST(blob_db_contract_list, test_empty_list)
{
	uint64_t id;

	zassert_ok(mcl_next(0, &id));
	zassert_equal(id, 0);
	zassert_ok(mcl_prev(0, &id));
	zassert_equal(id, 0);
	expect_list("", "empty");
}

ZTEST(blob_db_contract_list, test_insert_everywhere)
{
	uint64_t ids[3];
	uint64_t x;

	seed("abc", ids);                       /* push_back x3 */
	expect_list("abc", "seed");
	zassert_ok(mcl_insert_after(0, "z", 1, NULL, MCL_STEP_NONE));
	expect_list("zabc", "push_front");
	zassert_ok(mcl_insert_after(ids[0], "x", 1, &x, MCL_STEP_NONE));
	expect_list("zaxbc", "insert_after a");
	zassert_ok(mcl_insert_before(ids[2], "y", 1, NULL, MCL_STEP_NONE));
	expect_list("zaxbyc", "insert_before c");
	zassert_ok(mcl_insert_after(ids[2], "w", 1, NULL, MCL_STEP_NONE));
	expect_list("zaxbycw", "insert_after tail");
	zassert_ok(mcl_insert_before(x, "v", 1, NULL, MCL_STEP_NONE));
	expect_list("zavxbycw", "insert_before x");
}

ZTEST(blob_db_contract_list, test_remove_everywhere)
{
	uint64_t ids[5];

	seed("abcde", ids);
	zassert_ok(mcl_remove(ids[2], MCL_STEP_NONE));
	expect_list("abde", "remove middle");
	zassert_ok(mcl_remove(ids[0], MCL_STEP_NONE));
	expect_list("bde", "remove head");
	zassert_ok(mcl_remove(ids[4], MCL_STEP_NONE));
	expect_list("bd", "remove tail");
	zassert_ok(mcl_remove(ids[1], MCL_STEP_NONE));
	zassert_ok(mcl_remove(ids[3], MCL_STEP_NONE));
	expect_list("", "remove only");

	/* A removed node is no handle any more. */
	zassert_equal(mcl_remove(ids[3], MCL_STEP_NONE), -ENOENT);
	zassert_equal(mcl_insert_after(ids[3], "q", 1, NULL, MCL_STEP_NONE),
		      -ENOENT);
	expect_list("", "after stale handles");
}

/* Overwriting a value does not change the reference graph: one in-place
 * update, no new blob, nothing to recover. */
ZTEST(blob_db_contract_list, test_set_in_place)
{
	uint64_t ids[3];

	seed("abc", ids);
	zassert_ok(mcl_set(ids[1], "B", 1));
	expect_list("aBc", "set");
}

ZTEST(blob_db_contract_list, test_persist_and_reopen)
{
	uint64_t ids[4];

	seed("abcd", ids);
	zassert_ok(mcl_remove(ids[1], MCL_STEP_NONE));

	crash_and_recover();   /* only the integer 1 persists */

	expect_list("acd", "reopen");
	zassert_ok(mcl_insert_after(ids[0], "b", 1, NULL, MCL_STEP_NONE));
	expect_list("abcd", "after reopen");
}

/* A node id from another structure, or the list's own root, is not a node
 * of this list: writes refuse it and change nothing. */
ZTEST(blob_db_contract_list, test_foreign_ids_refused)
{
	uint64_t ids[2];
	uint64_t root;
	uint64_t other;
	/* Shaped like a node, and claiming to follow the head. */
	uint64_t fake[3];

	seed("ab", ids);
	zassert_ok(rootreg_get(MCL_ROOTREG_KEY, &root));
	zassert_ok(rootreg_get_or_create(OTHER_ROOTREG_KEY, &other));
	fake[0] = ids[0];
	fake[1] = ids[1];
	fake[2] = ids[1];
	zassert_ok(blob_db_update(other, fake, sizeof(fake)));

	zassert_equal(mcl_remove(other, MCL_STEP_NONE), -ENOENT);
	zassert_equal(mcl_remove(root, MCL_STEP_NONE), -ENOENT);
	zassert_equal(mcl_insert_after(other, "x", 1, NULL, MCL_STEP_NONE),
		      -ENOENT);
	zassert_equal(mcl_set(other, "x", 1), -ENOENT);

	zassert_ok(rootreg_unregister(OTHER_ROOTREG_KEY));
	zassert_ok(blob_db_delete(other));
	expect_list("ab", "foreign");
}

/* --- crash tables (§6): recovery is correct AND leak-free -------------- */

enum op_kind { INS_AFTER, INS_BEFORE, REMOVE };

/* One structural mutation: on a list seeded with `seed`, insert "n" next to
 * (or remove) seed node `at` (-1: the root's end). */
struct op_case {
	const char *name;
	const char *seed;
	enum op_kind kind;
	int at;
	const char *after;
};

static const struct op_case ops[] = {
	{ "push_front empty", "",    INS_AFTER,  -1, "n"    },
	{ "push_front",       "abc", INS_AFTER,  -1, "nabc" },
	{ "insert_after mid", "abc", INS_AFTER,   0, "anbc" },
	{ "insert_before",    "abc", INS_BEFORE,  2, "abnc" },
	{ "push_back",        "abc", INS_BEFORE, -1, "abcn" },
	{ "remove head",      "abc", REMOVE,      0, "bc"   },
	{ "remove mid",       "abc", REMOVE,      1, "ac"   },
	{ "remove tail",      "abc", REMOVE,      2, "ab"   },
	{ "remove only",      "a",   REMOVE,      0, ""     },
};

/* An insert that returns 0 hands back the new node's id, also when it
 * finished through the abort path: check it names the "n" node. */
static int run_op(const struct op_case *op, const uint64_t *ids,
		  enum mcl_crash_point cp)
{
	uint64_t at = op->at < 0 ? 0 : ids[op->at];
	uint64_t nid = 0;
	int rc;

	switch (op->kind) {
	case INS_AFTER:
		rc = mcl_insert_after(at, "n", 1, &nid, cp);
		break;
	case INS_BEFORE:
		rc = mcl_insert_before(at, "n", 1, &nid, cp);
		break;
	default:
		return mcl_remove(at, cp);
	}
	if (rc == 0) {
		zassert_equal(val_of(nid), 'n', "%s: wrong id handed back",
			      op->name);
	}
	return rc;
}

struct crash_case {
	enum mcl_crash_point cp;
	bool committed;
};

/* Committed once the forward link is swapped (COMMIT). */
static const struct crash_case points[] = {
	{ MCL_STEP_STAGE,   false },
	{ MCL_STEP_PREPARE, false },
	{ MCL_STEP_COMMIT,  true  },
	{ MCL_STEP_REPAIR,  true  },
	{ MCL_STEP_CLEANUP, true  },
	{ MCL_STEP_NONE,    true  },
};

ZTEST(blob_db_contract_list, test_crash_table)
{
	for (size_t i = 0; i < ARRAY_SIZE(ops); i++) {
		for (size_t j = 0; j < ARRAY_SIZE(points); j++) {
			uint64_t ids[3];
			char ctx[48];

			snprintk(ctx, sizeof(ctx), "%s, crash point %d",
				 ops[i].name, points[j].cp);
			fresh_list();
			seed(ops[i].seed, ids);

			int rc = run_op(&ops[i], ids, points[j].cp);

			zassert_equal(rc, points[j].cp == MCL_STEP_NONE ?
					  0 : MCL_STOPPED, "%s: rc %d", ctx, rc);

			crash_and_recover();
			expect_list(points[j].committed ? ops[i].after :
				    ops[i].seed, ctx);
		}
	}
}

/* --- clean failures (abort path), no remount -------------------------- */

ZTEST(blob_db_contract_list, test_failed_mutation_aborts_cleanly)
{
	for (size_t i = 0; i < ARRAY_SIZE(ops); i++) {
		for (size_t j = 0; j < ARRAY_SIZE(points); j++) {
			uint64_t ids[3];
			char ctx[48];

			if (points[j].cp == MCL_STEP_NONE) {
				continue;
			}
			snprintk(ctx, sizeof(ctx), "%s, fail after %d",
				 ops[i].name, points[j].cp);
			fresh_list();
			seed(ops[i].seed, ids);

			mcl_fail_after(points[j].cp, -EIO);
			zassert_equal(run_op(&ops[i], ids, MCL_STEP_NONE),
				      points[j].committed ? 0 : -EIO, "%s", ctx);
			expect_list(points[j].committed ? ops[i].after :
				    ops[i].seed, ctx);

			/* The list keeps working after the abort. */
			size_t len = strlen(points[j].committed ?
					    ops[i].after : ops[i].seed);
			size_t count;

			zassert_ok(mcl_insert_before(0, "t", 1, NULL,
						     MCL_STEP_NONE));
			zassert_ok(mcl_check(&count));
			zassert_equal(count, len + 1, "%s", ctx);
		}
	}
}

/* --- the two chains between COMMIT and REPAIR -------------------------- */

/* The forward chain is right at every instant: before the commit it does
 * not show the new node, after it it does — with no recovery run. The back
 * chain is stale in between, so mcl_prev finishes the mutation first. */
ZTEST(blob_db_contract_list, test_forward_chain_always_right)
{
	uint64_t ids[2];
	uint64_t id;

	seed("ab", ids);
	zassert_equal(mcl_insert_before(0, "n", 1, NULL, MCL_STEP_PREPARE),
		      MCL_STOPPED);
	zassert_ok(mcl_next(ids[1], &id));
	zassert_equal(id, 0, "uncommitted node visible");

	fresh_list();
	seed("ab", ids);
	zassert_equal(mcl_insert_before(0, "n", 1, NULL, MCL_STEP_COMMIT),
		      MCL_STOPPED);
	zassert_ok(mcl_next(ids[1], &id));
	zassert_not_equal(id, 0, "committed node not visible forwards");
	zassert_equal(val_of(id), 'n');

	/* Backwards from the end: the tail is still "b" on flash until the
	 * repair; mcl_prev runs it and answers "n". */
	zassert_ok(mcl_prev(0, &id));
	zassert_equal(val_of(id), 'n', "stale back link surfaced");
	expect_list("abn", "after prev");
}

/* An interrupted mutation nobody recovered yet (no remount) is finished by
 * the next mutation before it stages, and by destroy before it deletes. */
ZTEST(blob_db_contract_list, test_staged_intent_finished_before_next_mutation)
{
	uint64_t ids[2];

	seed("ab", ids);
	zassert_equal(mcl_insert_after(ids[0], "n", 1, NULL, MCL_STEP_COMMIT),
		      MCL_STOPPED);
	zassert_ok(mcl_insert_before(ids[1], "m", 1, NULL, MCL_STEP_NONE));
	expect_list("anmb", "finished, then inserted");

	zassert_equal(mcl_insert_before(0, "x", 1, NULL, MCL_STEP_PREPARE),
		      MCL_STOPPED);
	zassert_ok(mcl_destroy());
	zassert_ok(rootreg_unregister(MCL_ROOTREG_KEY));
	zassert_equal(blob_db_count(), 1,
		      "want registry only, got %zu blobs", blob_db_count());
}

/* Firmware opens stores lazily: another client may bind blobs after the
 * reboot and before this list's recovery runs. Rolling back must leave
 * them alone. */
ZTEST(blob_db_contract_list, test_rollback_spares_other_store)
{
	uint64_t other;
	char buf[8];
	size_t got;

	zassert_equal(mcl_insert_after(0, "n", 1, NULL, MCL_STEP_PREPARE),
		      MCL_STOPPED);

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	zassert_ok(rootreg_init());
	zassert_ok(rootreg_get_or_create(OTHER_ROOTREG_KEY, &other));
	zassert_ok(blob_db_update(other, "cfg", 3));

	open_list();

	zassert_ok(blob_db_get(other, buf, sizeof(buf), &got),
		   "recovery deleted another store's blob");
	zassert_equal(got, 3);
	zassert_mem_equal(buf, "cfg", 3);
	zassert_equal(blob_db_count(), STRUCT_BLOBS + 1);
}

/* --- teardown ----------------------------------------------------------- */

ZTEST(blob_db_contract_list, test_destroy_and_unregister_cleanup)
{
	uint64_t ids[3];
	uint64_t old_root;
	uint64_t new_root;
	uint64_t id;

	seed("abc", ids);
	zassert_ok(rootreg_get(MCL_ROOTREG_KEY, &old_root));
	zassert_ok(mcl_destroy());
	zassert_ok(rootreg_unregister(MCL_ROOTREG_KEY));

	zassert_equal(blob_db_count(), 1,
		      "want registry only, got %zu blobs", blob_db_count());
	zassert_false(blob_db_exists(old_root));
	zassert_equal(mcl_next(0, &id), -ENODEV);

	open_list();
	zassert_ok(rootreg_get(MCL_ROOTREG_KEY, &new_root));
	zassert_not_equal(new_root, old_root, "dead ids must not be reused");
	expect_list("", "new life");
}

/* Destroy deletes from the tail backwards. A crash part-way leaves a
 * forward prefix with a dangling tail; a rerun after reopen finishes it. */
ZTEST(blob_db_contract_list, test_destroy_rerun_after_crash)
{
	uint64_t ids[3];
	uint64_t node[3];   /* prev, next, vid */
	size_t got;

	seed("abc", ids);

	/* What destroy does first, then the power goes: the tail's value and
	 * the tail node. */
	zassert_ok(blob_db_get(ids[2], node, sizeof(node), &got));
	zassert_equal(got, sizeof(node));
	zassert_ok(blob_db_delete(node[2]));
	zassert_ok(blob_db_delete(ids[2]));

	crash_and_recover();
	zassert_ok(mcl_destroy());
	zassert_ok(rootreg_unregister(MCL_ROOTREG_KEY));
	zassert_equal(blob_db_count(), 1,
		      "want registry only, got %zu blobs", blob_db_count());
}
