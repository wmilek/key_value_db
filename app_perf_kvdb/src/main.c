/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * kvdb (L3) demo + performance benchmark, with cross-reboot verification.
 *
 * The app maintains a *generation* counter inside the store itself (key
 * "gen"). Every value in the store is fully predicted by (key index,
 * generation), so a rerun can prove the previous run's content survived:
 *
 *   run 1 (empty store) : populate N_KEYS keys, stamp gen = 1
 *   run r (r >= 2)      : 1. VERIFY every key against the expectation for
 *                            the stored generation G (timed get loop)
 *                         2. MODIFY: rewrite the deterministic subset
 *                            { i : i % STRIDE == (G+1) % STRIDE }, toggle
 *                            the "ghost" key (present iff gen is odd),
 *                            then commit gen = G+1 (timed set loop)
 *                         3. RE-VERIFY at G+1 (timed get loop)
 *
 * Expected value of key i at generation G is derived from the *last
 * generation that wrote i* — computable from the modification rule alone,
 * so no shadow state is kept outside the store.
 *
 * On verification failure the run stops BEFORE modifying, leaving the store
 * intact for inspection. The gen commit is written last, so an interrupted
 * modify phase is detected as a mismatch on the next run (this is a demo,
 * not a crash-safe protocol — kvdb's per-op atomicity comes from L1, but
 * the multi-key modify phase is not one transaction).
 *
 * Reruns:
 *   native_sim : ./zephyr.exe --flash=kvdb.bin        (file-backed flash;
 *                add --flash_erase to start over)
 *   hardware   : just reset — flash persists. Set
 *                CONFIG_APP_PERF_KVDB_FRESH_START=y to format once.
 *
 * Timing uses k_uptime_delta() (ms resolution), like app_perf: flash ops
 * are slow enough that this costs <1 % accuracy.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>
#include <app/lib/kvdb.h>

#include <zephyr/app_version.h>

#ifdef CONFIG_ARCH_POSIX
#include "posix_board_if.h"   /* posix_exit() — end the sim run cleanly */
#endif

LOG_MODULE_REGISTER(app_perf_kvdb, CONFIG_APP_PERF_KVDB_LOG_LEVEL);

#define N_KEYS   CONFIG_APP_PERF_KVDB_N_KEYS
#define VAL_LEN  CONFIG_APP_PERF_KVDB_VAL_LEN
#define STRIDE   4          /* each gen rewrites every STRIDE-th key */
#define GEN_KEY  "gen"
#define GHOST_KEY "ghost"   /* present iff gen is odd — exercises delete */
#define GHOST_IDX 0xffffu

/* Every stored value carries its provenance and a derived fill pattern —
 * verification checks all of it byte for byte. */
struct val {
	uint32_t gen;   /* generation that wrote this value */
	uint32_t idx;   /* key index (GHOST_IDX for the ghost key) */
	uint8_t  fill[VAL_LEN];
};

static void key_name(char *buf, size_t sz, unsigned int i)
{
	snprintk(buf, sz, "k%03u", i);
}

static void make_val(struct val *v, uint32_t gen, uint32_t idx)
{
	v->gen = gen;
	v->idx = idx;
	for (size_t j = 0; j < VAL_LEN; j++) {
		v->fill[j] = (uint8_t)(gen * 31u + idx * 7u + j);
	}
}

/* The modification rule, inverted: which generation last wrote key i, given
 * the store is at generation G? Gen 1 wrote everything; gen g >= 2 wrote
 * { i : i % STRIDE == g % STRIDE }. */
static uint32_t last_writer(uint32_t i, uint32_t G)
{
	for (uint32_t g = G; g >= 2; g--) {
		if (g % STRIDE == i % STRIDE) {
			return g;
		}
		if (G - g >= STRIDE) {
			break; /* no match in a full stride window -> gen 1 */
		}
	}
	return 1;
}

static void bench_line(const char *what, int ops, int64_t ms)
{
	uint64_t milli_ops_per_s =
		(ms > 0) ? (uint64_t)ops * 1000000ULL / (uint64_t)ms : 0;
	uint64_t us_per_op =
		(ops > 0) ? (uint64_t)ms * 1000ULL / (uint64_t)ops : 0;

	printk("bench %-8s : %4d ops in %6" PRId64 " ms  -> "
	       "%5u.%03u ops/s  (%7llu us/op)\n",
	       what, ops, ms,
	       (unsigned)(milli_ops_per_s / 1000),
	       (unsigned)(milli_ops_per_s % 1000),
	       (unsigned long long)us_per_op);
}

/* Check every key (and the ghost) against the expectation for generation G.
 * Returns 0 on full match, -EILSEQ on any mismatch (logged). Timed. */
static int verify_generation(kvdb_t *db, uint32_t G, const char *label)
{
	char key[8];
	struct val got, want;
	size_t len;
	int bad = 0;
	int ops = 0;

	int64_t t = k_uptime_get();

	for (uint32_t i = 0; i < N_KEYS; i++) {
		key_name(key, sizeof(key), i);
		int rc = kvdb_get(db, key, &got, sizeof(got), &len);

		ops++;
		if (rc != 0 || len != sizeof(got)) {
			LOG_ERR("%s: get(%s) rc=%d len=%zu", label, key, rc, len);
			bad++;
			continue;
		}
		make_val(&want, last_writer(i, G), i);
		if (memcmp(&got, &want, sizeof(want)) != 0) {
			LOG_ERR("%s: %s stamped gen=%u idx=%u, expected gen=%u",
				label, key, got.gen, got.idx, want.gen);
			bad++;
		}
	}

	/* Ghost: present iff G is odd, stamped by the last odd gen <= G. */
	int rc = kvdb_get(db, GHOST_KEY, &got, sizeof(got), &len);

	ops++;
	if (G % 2 == 1) {
		make_val(&want, G, GHOST_IDX);
		if (rc != 0 || len != sizeof(got) ||
		    memcmp(&got, &want, sizeof(want)) != 0) {
			LOG_ERR("%s: ghost wrong (rc=%d)", label, rc);
			bad++;
		}
	} else if (rc != -ENOENT) {
		LOG_ERR("%s: ghost should be absent, rc=%d", label, rc);
		bad++;
	}

	bench_line(label, ops, k_uptime_delta(&t));

	if (bad) {
		printk("VERIFY FAIL (gen %u): %d bad entries\n", G, bad);
		return -EILSEQ;
	}
	printk("VERIFY PASS (gen %u)\n", G);
	return 0;
}

/* First run: fill the empty store and stamp gen = 1. Timed. */
static int populate(kvdb_t *db)
{
	char key[8];
	struct val v;
	int64_t t = k_uptime_get();
	int ops = 0;

	for (uint32_t i = 0; i < N_KEYS; i++) {
		key_name(key, sizeof(key), i);
		make_val(&v, 1, i);
		int rc = kvdb_set(db, key, &v, sizeof(v));

		if (rc != 0) {
			LOG_ERR("populate set(%s): %d", key, rc);
			return rc;
		}
		ops++;
	}

	make_val(&v, 1, GHOST_IDX);           /* gen 1 is odd -> ghost present */
	int rc = kvdb_set(db, GHOST_KEY, &v, sizeof(v));

	if (rc != 0) {
		return rc;
	}
	ops++;

	uint32_t gen = 1;

	rc = kvdb_set(db, GEN_KEY, &gen, sizeof(gen));  /* commit point */
	if (rc != 0) {
		return rc;
	}
	ops++;

	bench_line("populate", ops, k_uptime_delta(&t));
	return 0;
}

/* Advance G -> G+1: rewrite the subset, toggle the ghost, commit gen. Timed. */
static int modify(kvdb_t *db, uint32_t G)
{
	uint32_t next = G + 1;
	char key[8];
	struct val v;
	int64_t t = k_uptime_get();
	int ops = 0;

	for (uint32_t i = 0; i < N_KEYS; i++) {
		if (i % STRIDE != next % STRIDE) {
			continue;
		}
		key_name(key, sizeof(key), i);
		make_val(&v, next, i);
		int rc = kvdb_set(db, key, &v, sizeof(v));

		if (rc != 0) {
			LOG_ERR("modify set(%s): %d", key, rc);
			return rc;
		}
		ops++;
	}

	int rc;

	if (next % 2 == 1) {
		make_val(&v, next, GHOST_IDX);
		rc = kvdb_set(db, GHOST_KEY, &v, sizeof(v));
	} else {
		rc = kvdb_delete(db, GHOST_KEY);
	}
	if (rc != 0) {
		LOG_ERR("modify ghost: %d", rc);
		return rc;
	}
	ops++;

	rc = kvdb_set(db, GEN_KEY, &next, sizeof(next));  /* commit point */
	if (rc != 0) {
		return rc;
	}
	ops++;

	bench_line("modify", ops, k_uptime_delta(&t));
	return 0;
}

int main(void)
{
	printk("kvdb perf %s  (N_KEYS=%u  VAL_LEN=%u  STRIDE=%u  val=%u B)\n",
	       APP_VERSION_STRING, (unsigned)N_KEYS, (unsigned)VAL_LEN,
	       (unsigned)STRIDE, (unsigned)sizeof(struct val));

	int64_t t = k_uptime_get();
	int rc = blob_db_mount();

	if (rc != 0) {
		LOG_ERR("mount: %d", rc);
		return 0;
	}

	if (IS_ENABLED(CONFIG_APP_PERF_KVDB_FRESH_START)) {
		printk("FRESH_START: formatting store\n");
		rc = blob_db_format();
		if (rc != 0) {
			LOG_ERR("format: %d", rc);
			goto out;
		}
	}

	rc = rootreg_init();
	if (rc != 0) {
		LOG_ERR("rootreg_init: %d", rc);
		goto out;
	}

	struct kvdb_config cfg = {
		.backend = KVDB_BACKEND_HASH,
		.initial_capacity = N_KEYS / 2,
	};
	kvdb_t db;

	rc = kvdb_open(&db, "perf", &cfg);
	if (rc != 0) {
		LOG_ERR("kvdb_open: %d", rc);
		goto out;
	}
	printk("mount+open   :         %6" PRId64 " ms\n", k_uptime_delta(&t));

	uint32_t G = 0;
	size_t len;

	rc = kvdb_get(&db, GEN_KEY, &G, sizeof(G), &len);
	if (rc == -ENOENT) {
		printk("state: empty store -> initial population\n");
		rc = populate(&db);
		if (rc != 0) {
			goto out;
		}
		G = 1;
	} else if (rc != 0 || len != sizeof(G)) {
		LOG_ERR("gen key unreadable: rc=%d len=%zu", rc, len);
		goto out;
	} else {
		printk("state: rerun, store at gen %u\n", G);
	}

	/* Prove the content (whether just written or inherited) is exactly
	 * what generation G predicts. */
	rc = verify_generation(&db, G, "verify");
	if (rc != 0) {
		goto out;   /* leave the store untouched for inspection */
	}

	/* Every run modifies: advance one generation and prove it took. */
	rc = modify(&db, G);
	if (rc != 0) {
		goto out;
	}
	rc = verify_generation(&db, G + 1, "reverify");
	if (rc != 0) {
		goto out;
	}

	printk("done — store at gen %u; rerun to verify persistence\n", G + 1);

out:
	blob_db_unmount();
#ifdef CONFIG_ARCH_POSIX
	/* On native_sim, terminate the process so shell reruns are one-liners;
	 * exit code reflects the verify outcome. */
	posix_exit(rc == 0 ? 0 : 1);
#endif
	return 0;
}
