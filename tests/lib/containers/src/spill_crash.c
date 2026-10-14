/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Power loss inside a spilled set or delete
 * (doc/proposals/2026-10-09-kvhash-spill.md §5, §6 item 6).
 *
 * kvhash_test_cut_after cuts power after N of kvhash's flash writes: that
 * write and every later one do nothing. blob_db makes each write atomic, so
 * sweeping N from 0 until the operation completes untouched visits every state
 * a real power cut can leave. After each cut the store is remounted and must
 * show:
 *
 *   - the key wholly old or wholly new (or absent, where that is the old or
 *     new state) -- never a mixed entry, a dangling id, or -EIO;
 *   - monotonic progress: once a cut point shows the new state, every later
 *     one does too, and the uncut run ends in it;
 *   - every neighbouring key intact, and count agreeing with a full walk;
 *   - a retry of the same operation succeeding;
 *   - after destroy, no more blobs left behind than the scenario's bound --
 *     the unreferenced blobs §5 allows -- and none at all without a cut.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/shape_map.h>
#include <app/lib/containers/kvhash.h>

#define NEVER      32767u
#define N_NEIGH    6
#define MAX_CUTS   24
#define VBUF       64

static const struct map_ops *ops = &kvhash_map_ops;

static const char key[] = "crash-key-long-enough-to-spill-0001";
#define KL (sizeof(key) - 1)

enum op { OP_SET, OP_DEL };

struct scenario {
	const char *name;
	bool neighbours;          /* false: the key's bucket starts empty */
	bool pre_present;
	size_t pre_k, pre_v;      /* thresholds the old state is written under */
	size_t pre_vlen;
	enum op op;
	size_t op_k, op_v;        /* thresholds in force for the operation */
	size_t new_vlen;
	size_t max_leak;          /* unreferenced blobs a cut may leave (§5) */
};

static const struct scenario scenarios[] = {
	{ "insert spilled, fresh bucket", false, false, 0, 0, 0,
	  OP_SET, 0, 0, 32, 3 /* key, value, bucket */ },
	{ "insert spilled, existing bucket", true, false, 0, 0, 0,
	  OP_SET, 0, 0, 32, 2 /* key, value */ },
	{ "rewrite spilled value, same length", true, true, 0, 0, 32,
	  OP_SET, 0, 0, 32, 0 /* one in-place update */ },
	{ "rewrite spilled value, new length", true, true, 0, 0, 32,
	  OP_SET, 0, 0, 48, 1 /* new value, or the old one */ },
	{ "inline entry becomes spilled", true, true, NEVER, NEVER, 32,
	  OP_SET, 0, 0, 32, 2 /* new key, new value */ },
	{ "spilled entry becomes inline", true, true, 0, 0, 32,
	  OP_SET, NEVER, NEVER, 32, 2 /* old key, old value */ },
	{ "delete spilled entry", true, true, 0, 0, 32,
	  OP_DEL, 0, 0, 0, 2 /* old key, old value */ },
	{ "delete spilled entry, alone", false, true, 0, 0, 32,
	  OP_DEL, 0, 0, 0, 2 },
};

static void crash_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	kvhash_set_inline_max(NEVER, NEVER);
	kvhash_test_cut_after = -1;
	kvhash_test_cut_fired = false;
}

static void crash_after(void *fixture)
{
	ARG_UNUSED(fixture);
	kvhash_set_inline_max(NEVER, NEVER);
	kvhash_test_cut_after = -1;
	kvhash_test_cut_fired = false;
	blob_db_unmount();
}

ZTEST_SUITE(kvhash_spill_crash, NULL, NULL, crash_before, crash_after, NULL);

static void value(uint8_t *v, size_t len, uint8_t tag)
{
	for (size_t i = 0; i < len; i++) {
		v[i] = (uint8_t)(tag + i);
	}
}

static void neighbour(char *k, size_t sz, int i)
{
	snprintf(k, sz, "neighbour-%d", i);
}

/* Neighbours alternate between inline and spilled, so a damaged bucket
 * rewrite would show on both kinds. */
static void put_neighbours(uint64_t root)
{
	uint8_t v[24];
	char k[24];

	for (int i = 0; i < N_NEIGH; i++) {
		kvhash_set_inline_max(i % 2 ? 0 : NEVER, i % 2 ? 0 : NEVER);
		neighbour(k, sizeof(k), i);
		value(v, sizeof(v), (uint8_t)(0x10 * i));
		zassert_ok(ops->set(root, k, strlen(k), v, sizeof(v)));
	}
}

static void check_neighbours(const char *name, uint64_t root)
{
	uint8_t v[24], got[24];
	char k[24];
	size_t len;

	for (int i = 0; i < N_NEIGH; i++) {
		neighbour(k, sizeof(k), i);
		value(v, sizeof(v), (uint8_t)(0x10 * i));
		zassert_ok(ops->get(root, k, strlen(k), got, sizeof(got), &len),
			   "%s: neighbour %d lost", name, i);
		zassert_equal(len, sizeof(v), "%s: neighbour %d length", name, i);
		zassert_mem_equal(got, v, sizeof(v), "%s: neighbour %d value", name, i);
	}
}

/* count must equal the number of entries a full walk returns, and the walk
 * must read every value it passes without error. */
static void check_walk(const char *name, uint64_t root, size_t want)
{
	uint8_t cur[48], k[48], v[VBUF];
	size_t clen = 0, kl, vl, walked = 0, n = 0;
	int rc;

	while ((rc = ops->next(root, cur, clen, k, sizeof(k), &kl,
			       v, sizeof(v), &vl)) == 0) {
		zassert_true(walked < 64, "%s: walk does not end", name);
		walked++;
		memcpy(cur, k, kl);
		clen = kl;
	}
	zassert_equal(rc, -ENODATA, "%s: walk ended with %d", name, rc);
	zassert_ok(ops->count(root, &n));
	zassert_equal(n, walked, "%s: count %zu, walk %zu", name, n, walked);
	zassert_equal(n, want, "%s: %zu entries, want %zu", name, n, want);
}

enum state { ST_ABSENT, ST_OLD, ST_NEW };

static enum state read_state(const char *name, const struct scenario *s,
			     uint64_t root)
{
	uint8_t got[VBUF], v[VBUF];
	size_t len = 0;
	int rc = ops->get(root, key, KL, got, sizeof(got), &len);

	if (rc == -ENOENT) {
		return ST_ABSENT;
	}
	zassert_ok(rc, "%s: get returned %d", name, rc);

	if (s->pre_present && len == s->pre_vlen) {
		value(v, len, 0xA0);
		if (memcmp(got, v, len) == 0) {
			return ST_OLD;
		}
	}
	if (s->op == OP_SET && len == s->new_vlen) {
		value(v, len, 0xB0);
		if (memcmp(got, v, len) == 0) {
			return ST_NEW;
		}
	}
	zassert_unreachable("%s: value is neither old nor new (len %zu)", name, len);
	return ST_ABSENT;
}

static int run_op(const struct scenario *s, uint64_t root)
{
	uint8_t v[VBUF];

	kvhash_set_inline_max(s->op_k, s->op_v);
	if (s->op == OP_DEL) {
		return ops->del(root, key, KL);
	}
	value(v, s->new_vlen, 0xB0);
	return ops->set(root, key, KL, v, s->new_vlen);
}

static void sweep(const struct scenario *s)
{
	/* The state the operation leaves: absent after a delete, new after a
	 * set. The other allowed state is the old one, absent for an insert. */
	const enum state old = s->pre_present ? ST_OLD : ST_ABSENT;
	const enum state fin = (s->op == OP_DEL) ? ST_ABSENT : ST_NEW;
	bool reached = false;
	int cut;

	for (cut = 0; cut < MAX_CUTS; cut++) {
		char name[96];

		snprintf(name, sizeof(name), "%s, cut after %d", s->name, cut);

		/* A clean store per cut point, so blob counts are exact. */
		kvhash_test_cut_after = -1;
		kvhash_test_cut_fired = false;
		zassert_ok(blob_db_format());

		size_t baseline = blob_db_count();
		struct map_config cfg = { .expected_entries = 2 };
		uint64_t root = blob_db_alloc_id();

		zassert_not_equal(root, 0);
		zassert_ok(ops->create(root, &cfg));
		if (s->neighbours) {
			put_neighbours(root);
		}
		if (s->pre_present) {
			uint8_t v[VBUF];

			kvhash_set_inline_max(s->pre_k, s->pre_v);
			value(v, s->pre_vlen, 0xA0);
			zassert_ok(ops->set(root, key, KL, v, s->pre_vlen));
		}

		/* Cut, then "reboot". */
		kvhash_test_cut_after = cut;
		int rc = run_op(s, root);
		bool fired = kvhash_test_cut_fired;

		kvhash_test_cut_after = -1;
		kvhash_test_cut_fired = false;
		kvhash_set_inline_max(NEVER, NEVER); /* the next boot's policy */
		zassert_ok(blob_db_unmount());
		zassert_ok(blob_db_mount());

		if (!fired) {
			zassert_ok(rc, "%s: uncut operation failed: %d", name, rc);
		}

		enum state st = read_state(name, s, root);

		zassert_true(st == old || st == fin,
			     "%s: state %d is neither old (%d) nor final (%d)",
			     name, st, old, fin);
		if (reached) {
			zassert_equal(st, fin, "%s: went back to the old state", name);
		}
		reached = reached || (st == fin && st != old);

		if (s->neighbours) {
			check_neighbours(name, root);
		}
		check_walk(name, root,
			   (s->neighbours ? N_NEIGH : 0) + (st == ST_ABSENT ? 0 : 1));

		/* A retry after the reboot completes the operation. Deleting a
		 * key the cut already removed is -ENOENT, as for any absent key. */
		rc = run_op(s, root);
		if (s->op == OP_DEL && st == ST_ABSENT) {
			zassert_equal(rc, -ENOENT, "%s: retry of a done delete: %d",
				      name, rc);
		} else {
			zassert_ok(rc, "%s: retry failed: %d", name, rc);
		}
		kvhash_set_inline_max(NEVER, NEVER);
		zassert_equal(read_state(name, s, root), fin, "%s: retry", name);

		/* destroy releases what the map references; the rest leaked. */
		zassert_ok(ops->destroy(root));

		size_t leaked = blob_db_count() - baseline;

		zassert_true(leaked <= s->max_leak,
			     "%s: %zu blob(s) left unreferenced, at most %zu allowed",
			     name, leaked, s->max_leak);
		if (!fired) {
			zassert_equal(leaked, 0, "%s: an uncut operation leaked", name);
			zassert_equal(st, fin, "%s: uncut operation", name);
			break;
		}
	}
	zassert_true(cut < MAX_CUTS, "%s: never completed within %d writes",
		     s->name, MAX_CUTS);
	TC_PRINT("%-36s: %d cut points\n", s->name, cut);
}

ZTEST(kvhash_spill_crash, test_power_loss_at_every_write_of_spilled_set_and_del)
{
	for (size_t i = 0; i < ARRAY_SIZE(scenarios); i++) {
		sweep(&scenarios[i]);
	}
}
