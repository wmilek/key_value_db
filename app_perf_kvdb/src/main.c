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
 *                         2. COUNT the keys (one timed map_ops.count call)
 *                            and WALK them (timed map_ops.next loop),
 *                            checking both against the gen-G prediction
 *                         3. MODIFY: rewrite the deterministic subset
 *                            { i : i % STRIDE == (G+1) % STRIDE }, toggle
 *                            the "ghost" key (present iff gen is odd),
 *                            then commit gen = G+1 (timed set loop)
 *                         4. RE-VERIFY at G+1 (timed get loop)
 *                         5. RECOUNT and REWALK at G+1
 *
 * Enumeration (count and walk) measures kvhash's `count` and `next`
 * (shape_map.h). kvdb does not wire either yet (l3_interfaces.md §3), so
 * this app reaches them through the Map op vector the kvdb handle bound at
 * open — the same vector kvdb_get() itself forwards to. Besides the timing,
 * the walk is a second, independent proof of the store's content: a
 * key-as-cursor walk must return every predicted key exactly once, with
 * its gen-G value, and nothing else; and count must agree with it. Both
 * run on the inherited store (after a reboot) and again after the modify
 * phase, so the order's stability across reboots, inserts, deletes and
 * updates is exercised, not just asserted.
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
#define KEY_BUF   16        /* longest key here is "intent" (6) + NUL */

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

#if defined(CONFIG_BLOB_DB_IOSTATS)
/* Flash actually touched by the phase just measured — the one figure a
 * native_sim run produces that means anything, since the simulator models
 * no latency and every wall-clock line rounds to zero there. The enumeration
 * phases are what this is for: their cost is "reads per record", and that
 * count is deterministic, so it is a regression guard where ms is not.
 * Enabled per board (boards/native_sim.conf); the DK build leaves it off so
 * its timings stay comparable with earlier RESULTS.md captures. */
static struct blob_db_iostats io_mark;

static void io_reset(void)
{
	blob_db_iostats_get(&io_mark);
}

/* Prints the delta and returns the number of write operations in it. */
static uint32_t io_line(const char *what)
{
	struct blob_db_iostats now;

	blob_db_iostats_get(&now);

	const uint32_t rd = now.reads - io_mark.reads;
	const uint32_t wr = now.writes - io_mark.writes;
	const uint32_t er = now.erases - io_mark.erases;

	/* Same shape as app_perf's line, so app_perf_l0/tools/l0_timing.py
	 * can turn a native_sim capture into predicted hardware time. */
	printk("   io %-8s : rd %6u ops/%8llu B   wr %5u ops/%8llu B   "
	       "er %4u ops/%8llu B\n",
	       what, rd, (unsigned long long)(now.bytes_read - io_mark.bytes_read),
	       wr, (unsigned long long)(now.bytes_written - io_mark.bytes_written),
	       er, (unsigned long long)(now.bytes_erased - io_mark.bytes_erased));
	return wr;
}
#else
#define io_reset()      ((void)0)
#define io_line(what)   ((uint32_t)0)
#endif

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

	io_reset();
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
	(void)io_line(label);

	if (bad) {
		printk("VERIFY FAIL (gen %u): %d bad entries\n", G, bad);
		return -EILSEQ;
	}
	printk("VERIFY PASS (gen %u)\n", G);
	return 0;
}

/* Keys present at generation G with no bump in flight: the N_KEYS data
 * keys, the gen key, and the ghost on odd generations. */
static size_t expected_count(uint32_t G)
{
	return (size_t)N_KEYS + 1u + ((G % 2u == 1u) ? 1u : 0u);
}

/* Parse "kNNN" back into its index. Returns false for any other key. */
static bool parse_key_idx(const char *k, size_t klen, uint32_t *idx)
{
	uint32_t v = 0;

	if (klen < 2 || k[0] != 'k') {
		return false;
	}
	for (size_t j = 1; j < klen; j++) {
		if (k[j] < '0' || k[j] > '9') {
			return false;
		}
		v = v * 10u + (uint32_t)(k[j] - '0');
	}
	*idx = v;
	return true;
}

/* One map_ops.count call, timed, checked against the prediction for G. The
 * contract says count writes nothing, so under IOSTATS a write op in it is
 * a FAIL. */
static int count_check(kvdb_t *db, uint32_t G, const char *label)
{
	size_t n = 0;

	int64_t t = k_uptime_get();

	io_reset();
	int rc = db->ops->count(db->root, &n);
	int64_t ms = k_uptime_delta(&t);

	if (rc != 0) {
		LOG_ERR("%s: count rc=%d", label, rc);
		return rc;
	}
	bench_line(label, 1, ms);
	uint32_t wr = io_line(label);

	if (n != expected_count(G)) {
		printk("COUNT FAIL (gen %u): %zu keys, expected %zu\n",
		       G, n, expected_count(G));
		return -EILSEQ;
	}
	if (wr != 0) {
		printk("COUNT FAIL (gen %u): %u write ops — count must not write\n",
		       G, wr);
		return -EILSEQ;
	}
	printk("COUNT PASS (gen %u): %zu keys\n", G, n);
	return 0;
}

/* A full key-as-cursor walk with map_ops.next: start from the empty key,
 * feed each returned key back as the cursor, stop at -ENODATA. Every
 * predicted key must come back exactly once with its gen-G value, and no
 * other key may appear. Timed; ops counts the next() calls, so the final
 * -ENODATA is one op too. *n_out receives the number of entries returned. */
static int walk_verify(kvdb_t *db, uint32_t G, const char *label,
		       size_t *n_out)
{
	static uint8_t seen[(N_KEYS + 7) / 8];
	char cur[KEY_BUF], kout[KEY_BUF];
	size_t clen = 0, klen = 0, vlen = 0;
	union {
		struct val v;
		struct gen_rec g;
		struct intent_rec i;
	} got;
	struct val want;
	size_t entries = 0;
	bool ghost_seen = false;
	int bad = 0;
	int ops = 0;
	int rc;

	memset(seen, 0, sizeof(seen));

	int64_t t = k_uptime_get();

	io_reset();
	for (;;) {
		rc = db->ops->next(db->root, cur, clen,
				   kout, sizeof(kout) - 1, &klen,
				   &got, sizeof(got), &vlen);
		ops++;
		if (rc == -ENODATA) {
			break;
		}
		if (rc != 0) {
			LOG_ERR("%s: next(%.*s) rc=%d", label, (int)clen, cur, rc);
			bad++;
			break;
		}
		entries++;
		kout[klen] = '\0';

		uint32_t idx;

		if (parse_key_idx(kout, klen, &idx)) {
			if (idx >= N_KEYS) {
				LOG_ERR("%s: key %s out of range", label, kout);
				bad++;
			} else if (seen[idx / 8] & (1u << (idx % 8))) {
				LOG_ERR("%s: key %s returned twice", label, kout);
				bad++;
			} else {
				seen[idx / 8] |= (uint8_t)(1u << (idx % 8));
				make_val(&want, last_writer(idx, G), idx);
				if (vlen != sizeof(want) ||
				    memcmp(&got.v, &want, sizeof(want)) != 0) {
					LOG_ERR("%s: %s value wrong (len %zu)",
						label, kout, vlen);
					bad++;
				}
			}
		} else if (strcmp(kout, GEN_KEY) == 0) {
			if (vlen != sizeof(got.g) || got.g.gen != G ||
			    got.g.n_keys != N_KEYS || got.g.val_len != VAL_LEN) {
				LOG_ERR("%s: gen record wrong", label);
				bad++;
			}
		} else if (strcmp(kout, GHOST_KEY) == 0) {
			make_val(&want, G, GHOST_IDX);
			if (ghost_seen || G % 2 != 1 || vlen != sizeof(want) ||
			    memcmp(&got.v, &want, sizeof(want)) != 0) {
				LOG_ERR("%s: ghost wrong (gen %u)", label, G);
				bad++;
			}
			ghost_seen = true;
		} else {
			LOG_ERR("%s: unexpected key '%s'", label, kout);
			bad++;
		}

		/* The returned key is the next cursor. */
		memcpy(cur, kout, klen);
		clen = klen;
	}

	bench_line(label, ops, k_uptime_delta(&t));
	(void)io_line(label);

	size_t missing = 0;

	for (uint32_t i = 0; i < N_KEYS; i++) {
		if (!(seen[i / 8] & (1u << (i % 8)))) {
			missing++;
		}
	}
	if (missing) {
		LOG_ERR("%s: %zu keys never returned", label, missing);
		bad++;
	}
	if ((G % 2 == 1) && !ghost_seen) {
		LOG_ERR("%s: ghost never returned", label);
		bad++;
	}

	*n_out = entries;
	if (bad || entries != expected_count(G)) {
		printk("WALK FAIL (gen %u): %zu entries, %d bad\n", G, entries, bad);
		return -EILSEQ;
	}
	printk("WALK PASS (gen %u): %zu entries, each exactly once\n", G, entries);
	return 0;
}

/* The enumeration phase: count, then a full walk, then count again as a
 * cross-check of the two against each other. The second count is not a
 * bench line — the first one already is, and this one must agree with it. */
static int enumerate(kvdb_t *db, uint32_t G, const char *count_label,
		     const char *walk_label)
{
	size_t walked = 0;
	int rc = count_check(db, G, count_label);

	if (rc != 0) {
		return rc;
	}
	rc = walk_verify(db, G, walk_label, &walked);
	if (rc != 0) {
		return rc;
	}

	size_t n = 0;

	rc = db->ops->count(db->root, &n);
	if (rc != 0) {
		LOG_ERR("%s: count rc=%d", count_label, rc);
		return rc;
	}
	if (n != walked) {
		printk("COUNT FAIL (gen %u): count %zu but walk returned %zu\n",
		       G, n, walked);
		return -EILSEQ;
	}
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
	io_reset();

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
	(void)io_line("populate");
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

	io_reset();

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
	(void)io_line("modify");
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

	io_reset();
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
	(void)io_line("recover");
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
		goto out;
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
		/* Declare the population, not a bucket count. This used to say
		 * N_KEYS / 2, meaning "about two entries per bucket" — which the
		 * container read as a bucket count and then silently clamped to
		 * 127 (FINDINGS.md K9). */
		.expected_entries = N_KEYS,
		.typical_entry_bytes = VAL_LEN + sizeof("k0000"),
	};
	kvdb_t db;

	rc = kvdb_open(&db, "perf", &cfg);
	if (rc != 0) {
		LOG_ERR("kvdb_open: %d", rc);
		goto out;
	}
	printk("mount+open   :         %6" PRId64 " ms\n", k_uptime_delta(&t));

	/* What the backend built from the declaration above. count's cost is
	 * per record (every directory and every allocated bucket), so the
	 * geometry is what its reads are read against. */
	struct map_info info;

	rc = db.ops->stat(db.root, &info);
	if (rc != 0) {
		LOG_ERR("stat: %d", rc);
		goto out;
	}
	printk("map geometry : depth %u, fanout %u, %u buckets, entry limit %zu B\n",
	       info.depth, info.fanout, info.buckets, info.entry_bytes_limit);

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
		 * exactly what generation G predicts — first by key, then by
		 * enumeration, which has no prediction to work from and must
		 * find the same set. */
		rc = verify_generation(&db, G, "verify");
		if (rc == 0) {
			rc = enumerate(&db, G, "count", "walk");
		}
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
	/* The order must have survived the subset rewrite, the ghost toggle
	 * and the intent insert+delete the bump just made. */
	rc = enumerate(&db, G + 1, "recount", "rewalk");
	if (rc != 0) {
		goto out;
	}

	printk("done — store at gen %u; rerun to verify persistence\n", G + 1);

out:
	blob_db_unmount();
#ifdef CONFIG_ARCH_POSIX
	posix_exit(rc == 0 ? 0 : 1);
#endif
	return 0;
}
