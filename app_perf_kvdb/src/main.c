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
 *   run 1 (empty store) : blob_db_prepare() the write path, then populate
 *                         N_KEYS keys, stamp gen = 1 (geometry recorded;
 *                         a build with different N_KEYS/VAL_LEN wipes the
 *                         store and repopulates instead of failing verify)
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
 * Power-loss detection and the atomicity proof
 * --------------------------------------------
 * The multi-key modify phase is not one transaction, but each kvdb op is
 * atomic (L1 contract). To make an interrupted run *detectable* — and to
 * prove that per-op atomicity — every generation bump follows a four-step
 * protocol, each step a single atomic op:
 *
 *   1. set "intent" = { to_gen = G+1 }     declare the bump
 *   2. rewrite the subset keys at G+1      <- power cut here is a torn state
 *   3. set "gen"    = G+1                  commit point
 *   4. delete "intent"
 *
 * On boot, intent classifies the store:
 *   - no intent            : clean; strict verify at G.
 *   - intent, gen == G     : POWER LOSS mid-modify. Recovery verify: every
 *                            key must equal EITHER its gen-G value OR its
 *                            gen-G+1 value, in full — a mixed/garbled value
 *                            would disprove per-op atomicity and FAILs the
 *                            run. Then the bump is rolled forward (the
 *                            subset rewrite is idempotent) and re-verified.
 *   - intent, gen == G+1   : power loss between commit and cleanup; content
 *                            is fully at G+1 — drop the intent, verify.
 *
 * A power cut *inside* a single flash write is covered by the same check:
 * blob_db discards the torn slot (CRC) and the key reads back as its
 * previous value — still one of the two allowed states.
 *
 * To exercise it: cut power (or press reset) while a run is in its modify
 * phase; the next boot reports the torn state, proves atomicity, heals.
 *
 * On verification failure the run stops BEFORE modifying, leaving the store
 * intact for inspection.
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
#define INTENT_KEY "intent" /* present only while a gen bump is in flight */
#define GHOST_KEY "ghost"   /* present iff gen is odd — exercises delete */
#define GHOST_IDX 0xffffu

/* Every stored value carries its provenance and a derived fill pattern —
 * verification checks all of it byte for byte. */
struct val {
	uint32_t gen;   /* generation that wrote this value */
	uint32_t idx;   /* key index (GHOST_IDX for the ghost key) */
	uint8_t  fill[VAL_LEN];
};

/* The gen key stores the generation plus the geometry that populated the
 * store. A build with different N_KEYS/VAL_LEN cannot verify the inherited
 * content, so a geometry mismatch triggers a wipe + repopulation instead of
 * a spurious VERIFY FAIL. */
struct gen_rec {
	uint32_t gen;
	uint16_t n_keys;
	uint16_t val_len;
};

/* Present in the store only between step 1 and step 4 of a generation bump
 * (see the header comment) — its existence at boot means power was lost. */
struct intent_rec {
	uint32_t to_gen;
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

/* First run: fill the empty store and stamp gen = 1. Timed.
 *
 * blob_db_prepare() first pre-formats the blob_db buckets the id allocator
 * will land in, so the timed loop measures the warm write path — without it
 * every first touch of a 64 KB QSPI sector pays a ~1 s erase inside the
 * loop (see app_perf/RESULTS.md). Reported as its own bench line. */
static int populate(kvdb_t *db)
{
	char key[8];
	struct val v;
	int ops = 0;

	int64_t t = k_uptime_get();
	int prepared = blob_db_prepare(N_KEYS * 2);

	if (prepared < 0) {
		LOG_ERR("prepare: %d", prepared);
		return prepared;
	}
	bench_line("prepare", prepared, k_uptime_delta(&t));

	t = k_uptime_get();

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

	struct gen_rec rec = { .gen = 1, .n_keys = N_KEYS, .val_len = VAL_LEN };

	rc = kvdb_set(db, GEN_KEY, &rec, sizeof(rec));  /* commit point */
	if (rc != 0) {
		return rc;
	}
	ops++;

	bench_line("populate", ops, k_uptime_delta(&t));
	return 0;
}

/* Advance G -> G+1 under the intent protocol: declare the bump, rewrite the
 * subset, toggle the ghost, commit gen, clear the intent. Idempotent — safe
 * to rerun as roll-forward after a power cut. Timed. */
static int modify(kvdb_t *db, uint32_t G)
{
	uint32_t next = G + 1;
	char key[8];
	struct val v;
	int64_t t = k_uptime_get();
	int ops = 0;

	/* Step 1: durable declaration that gen G+1 is in flight. */
	struct intent_rec intent = { .to_gen = next };
	int irc = kvdb_set(db, INTENT_KEY, &intent, sizeof(intent));

	if (irc != 0) {
		LOG_ERR("modify intent: %d", irc);
		return irc;
	}
	ops++;

	/* Step 2: the subset rewrite — power loss in here is the torn state
	 * the next boot detects and proves atomic. */
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

	/* Step 3: commit point. */
	struct gen_rec rec = { .gen = next, .n_keys = N_KEYS, .val_len = VAL_LEN };

	rc = kvdb_set(db, GEN_KEY, &rec, sizeof(rec));
	if (rc != 0) {
		return rc;
	}
	ops++;

	/* Step 4: bump complete — drop the intent. */
	rc = kvdb_delete(db, INTENT_KEY);
	if (rc != 0) {
		LOG_ERR("modify intent clear: %d", rc);
		return rc;
	}
	ops++;

	bench_line("modify", ops, k_uptime_delta(&t));
	return 0;
}

/* Power was lost during the G -> G+1 subset rewrite. Prove per-op atomicity:
 * every key must be ENTIRELY at its gen-G value or ENTIRELY at its gen-G+1
 * value — a mixed or garbled value fails. Timed; reports old/new counts. */
static int recovery_verify(kvdb_t *db, uint32_t G)
{
	uint32_t next = G + 1;
	char key[8];
	struct val got, want_old, want_new;
	size_t len;
	int n_old = 0, n_new = 0, bad = 0;
	int ops = 0;

	int64_t t = k_uptime_get();

	for (uint32_t i = 0; i < N_KEYS; i++) {
		key_name(key, sizeof(key), i);
		int rc = kvdb_get(db, key, &got, sizeof(got), &len);

		ops++;
		if (rc != 0 || len != sizeof(got)) {
			LOG_ERR("recover: get(%s) rc=%d len=%zu", key, rc, len);
			bad++;
			continue;
		}
		make_val(&want_old, last_writer(i, G), i);
		make_val(&want_new, last_writer(i, next), i);
		if (memcmp(&got, &want_old, sizeof(got)) == 0) {
			n_old++;
		} else if (memcmp(&got, &want_new, sizeof(got)) == 0) {
			n_new++;
		} else {
			LOG_ERR("recover: %s stamped gen=%u — neither gen %u nor %u value",
				key, got.gen, G, next);
			bad++;
		}
	}

	/* Ghost: exactly one of gen G / G+1 has it present (odd gens). Absent
	 * always matches the even one; present must match the odd one's stamp. */
	int rc = kvdb_get(db, GHOST_KEY, &got, sizeof(got), &len);

	ops++;
	if (rc == 0) {
		uint32_t odd = (G % 2 == 1) ? G : next;

		make_val(&want_new, odd, GHOST_IDX);
		if (len != sizeof(got) ||
		    memcmp(&got, &want_new, sizeof(got)) != 0) {
			LOG_ERR("recover: ghost invalid");
			bad++;
		}
	} else if (rc != -ENOENT) {
		LOG_ERR("recover: ghost rc=%d", rc);
		bad++;
	}

	bench_line("recover", ops, k_uptime_delta(&t));
	printk("torn state: %d keys still at gen <= %u, %d already at gen %u\n",
	       n_old, G, n_new, next);

	if (bad) {
		printk("ATOMICITY FAIL: %d keys in a mixed/garbled state\n", bad);
		return -EILSEQ;
	}
	printk("ATOMICITY PASS: every key wholly old or wholly new\n");
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
	struct gen_rec rec;
	size_t len;

	rc = kvdb_get(&db, GEN_KEY, &rec, sizeof(rec), &len);
	if (rc == 0 && len == sizeof(rec) &&
	    (rec.n_keys != N_KEYS || rec.val_len != VAL_LEN)) {
		/* Store was populated by a build with different geometry —
		 * its content cannot verify against this build's predictions.
		 * Wipe (logical, O(1)) and repopulate. */
		printk("state: geometry changed (stored %u/%u, built %u/%u) -> wipe\n",
		       rec.n_keys, rec.val_len, (unsigned)N_KEYS, (unsigned)VAL_LEN);
		rc = -ENOENT;
	} else if (rc == -ENOMEM || (rc == 0 && len != sizeof(rec))) {
		/* Unknown gen record layout (older/newer build) — same treatment. */
		printk("state: unknown gen record (len=%zu) -> wipe\n", len);
		rc = -ENOENT;
	}

	if (rc == -ENOENT && kvdb_has(&db, GEN_KEY)) {
		/* Wipe path: drop everything, rebootstrap, reopen. */
		rc = blob_db_erase_all();
		if (rc != 0) {
			LOG_ERR("erase_all: %d", rc);
			goto out;
		}
		rc = rootreg_init();
		if (rc == 0) {
			rc = kvdb_open(&db, "perf", &cfg);
		}
		if (rc != 0) {
			LOG_ERR("reopen after wipe: %d", rc);
			goto out;
		}
		rc = -ENOENT;   /* fall through to population */
	}

	if (rc == -ENOENT) {
		printk("state: empty store -> initial population\n");
		rc = populate(&db);
		if (rc != 0) {
			goto out;
		}
		G = 1;
	} else if (rc != 0) {
		LOG_ERR("gen key unreadable: rc=%d len=%zu", rc, len);
		goto out;
	} else {
		G = rec.gen;
		printk("state: rerun, store at gen %u\n", G);
	}

	/* Classify the store by the intent record (see header comment). */
	bool torn = false;
	struct intent_rec intent;

	rc = kvdb_get(&db, INTENT_KEY, &intent, sizeof(intent), &len);
	if (rc == 0 && len == sizeof(intent)) {
		if (intent.to_gen == G + 1) {
			printk("POWER LOSS detected: gen %u -> %u bump was in flight\n",
			       G, G + 1);
			torn = true;
		} else if (intent.to_gen == G) {
			printk("POWER LOSS detected after commit of gen %u — clearing intent\n",
			       G);
			rc = kvdb_delete(&db, INTENT_KEY);
			if (rc != 0) {
				LOG_ERR("intent clear: %d", rc);
				goto out;
			}
		} else {
			LOG_ERR("stale intent to_gen=%u at gen %u", intent.to_gen, G);
			rc = -EILSEQ;
			goto out;
		}
	} else if (rc != -ENOENT) {
		LOG_ERR("intent unreadable: rc=%d len=%zu", rc, len);
		if (rc == 0) {
			rc = -EILSEQ;
		}
		goto out;
	}

	if (torn) {
		/* Prove every key is wholly old or wholly new — the per-op
		 * atomicity check. The modify() below then rolls the same
		 * G -> G+1 bump forward (it is idempotent). */
		rc = recovery_verify(&db, G);
	} else {
		/* Prove the content (whether just written or inherited) is
		 * exactly what generation G predicts. */
		rc = verify_generation(&db, G, "verify");
	}
	if (rc != 0) {
		goto out;   /* leave the store untouched for inspection */
	}

	/* Every run modifies: advance (or complete) one generation bump and
	 * prove it took. */
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
