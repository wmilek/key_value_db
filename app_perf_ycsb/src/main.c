/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * YCSB core workloads over kvdb (L3).
 *
 * What this is
 * ------------
 * The YCSB core workloads (Cooper et al., "Benchmarking Cloud Serving Systems
 * with YCSB", SoCC 2010) driven against this stack, so a number from here can
 * be put next to a number from anywhere else that runs YCSB. The Java client
 * cannot reach an MCU, so what runs here is the *workload specification* — the
 * operation mixes, the record shape, the Zipfian key distribution and its
 * constant — reimplemented in C. That is the same thing SILT, FlashStore and
 * WiscKey did, and it is the only form of YCSB an embedded target can run.
 *
 *   workload  mix                                    key distribution
 *   A         50 % read  / 50 % update               scrambled Zipfian
 *   B         95 % read  /  5 % update               scrambled Zipfian
 *   C        100 % read                              scrambled Zipfian
 *   D         95 % read  /  5 % insert               latest
 *   F         50 % read  / 50 % read-modify-write    scrambled Zipfian
 *
 * Workload E (95 % short scans) is absent and cannot be added: it needs an
 * ordered range query, kvhash is an unordered hash, and kvtree is a skeleton.
 * That is a statement about the stack, not about the harness — see README.md.
 *
 * What it reports, and which number to believe
 * --------------------------------------------
 * Three things per phase: wall clock (ops/s and us/op), a latency
 * distribution (min / avg / p95 / p99 / max), and the flash traffic the phase
 * actually caused (CONFIG_BLOB_DB_IOSTATS).
 *
 * On native_sim only the flash traffic means anything — the simulator models
 * no latency, so every wall-clock figure rounds to zero. It is also the number
 * that carries furthest: it is deterministic, it reproduces the hardware's
 * counters exactly when the build carries the target's geometry (see
 * app_perf_l0/RESULTS.md §2), and read amplification is what this stack's
 * shape actually decides. Wall clock needs the DK.
 *
 * Every run starts from a formatted store, because YCSB is a load phase
 * followed by a run phase and because the expected-value table that verifies
 * reads lives in RAM. Cross-reboot persistence is app_perf_kvdb's job, not
 * this one's.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>
#include <app/lib/kvdb.h>

#include <zephyr/app_version.h>

#ifdef CONFIG_ARCH_POSIX
#include "posix_board_if.h"   /* posix_exit() — end the sim run cleanly */
#endif

#include "generator.h"

LOG_MODULE_REGISTER(app_perf_ycsb, CONFIG_APP_PERF_YCSB_LOG_LEVEL);

#define RECORD_COUNT  CONFIG_APP_PERF_YCSB_RECORD_COUNT
#define OP_COUNT      CONFIG_APP_PERF_YCSB_OPERATION_COUNT
#define FIELD_COUNT   CONFIG_APP_PERF_YCSB_FIELD_COUNT
#define FIELD_LEN     CONFIG_APP_PERF_YCSB_FIELD_LEN
#define VAL_LEN       (FIELD_COUNT * FIELD_LEN)
#define THETA         (CONFIG_APP_PERF_YCSB_ZIPFIAN_PERMILLE / 1000.0)

/* "user" + 10 digits, as YCSB names keys. kvdb keys are NUL-terminated C
 * strings; the container stores strlen() bytes, which is what KEY_LEN is. */
#define KEY_LEN       14

/* Worst case: every operation of every workload an insert. Two bytes per
 * record of bookkeeping is cheaper than reasoning about the real bound. */
#define MAX_RECORDS   (RECORD_COUNT + OP_COUNT)

/* What one record costs inside a kvhash bucket: [u16 klen][u16 vlen][key][val]. */
#define ENTRY_LEN     (4 + KEY_LEN + VAL_LEN)
#define MAX_PAYLOAD   CONFIG_BLOB_DB_MAX_PAYLOAD_LEN
#define MAX_BUCKETS   ((MAX_PAYLOAD - 8) / 8)

BUILD_ASSERT(FIELD_LEN >= 8, "a field must hold its 8-byte verification stamp");
BUILD_ASSERT(ENTRY_LEN <= MAX_PAYLOAD,
	     "one record cannot fit a kvhash bucket — raise "
	     "CONFIG_BLOB_DB_MAX_PAYLOAD_LEN or shrink the record");

enum op_kind { OP_READ, OP_UPDATE, OP_INSERT, OP_RMW, OP_KINDS };

/* Cumulative permille per operation kind, in enum order — an operation is
 * chosen by one draw below 1000 and a walk to the first bound it clears. */
struct workload {
	const char *name;
	const char *mix;
	uint16_t cum[OP_KINDS];
	bool latest;   /* key distribution: "latest" (D) rather than Zipfian */
};

static const struct workload workloads[] = {
#ifdef CONFIG_APP_PERF_YCSB_WORKLOAD_A
	{ "A", "50r/50u",  { 500, 1000, 1000, 1000 }, false },
#endif
#ifdef CONFIG_APP_PERF_YCSB_WORKLOAD_B
	{ "B", "95r/5u",   { 950, 1000, 1000, 1000 }, false },
#endif
#ifdef CONFIG_APP_PERF_YCSB_WORKLOAD_C
	{ "C", "100r",     { 1000, 1000, 1000, 1000 }, false },
#endif
#ifdef CONFIG_APP_PERF_YCSB_WORKLOAD_D
	{ "D", "95r/5i",   { 950, 950, 1000, 1000 }, true },
#endif
#ifdef CONFIG_APP_PERF_YCSB_WORKLOAD_F
	{ "F", "50r/50rmw", { 500, 500, 500, 1000 }, false },
#endif
};

/* Version of each record, so a read can be checked byte for byte without
 * reading the store to find out what it should hold. One byte per record;
 * wrapping is harmless because the stamp only has to distinguish a value from
 * its immediate predecessor. */
static uint8_t rec_version[MAX_RECORDS];
static uint32_t n_records;      /* keys 0 .. n_records-1 exist */

static uint8_t val_buf[VAL_LEN];
static uint8_t got_buf[VAL_LEN];

static struct ycsb_rng rng;
static struct ycsb_zipfian zipf_read;    /* scrambled, over RECORD_COUNT */
static struct ycsb_zipfian zipf_latest;  /* unscrambled, for workload D */

static uint16_t n_buckets;
static uint32_t verify_failures;

/* ------------------------------------------------------------------ records */

static void key_name(char *buf, size_t sz, uint32_t keynum)
{
	snprintk(buf, sz, "user%010u", keynum);
}

/*
 * A record is FIELD_COUNT fields of FIELD_LEN bytes. Each field opens with
 * {keynum, field index, version} and continues with a fill derived from all
 * three, so a verify catches a wrong key, a shuffled field and a stale value
 * alike — not just a corrupted byte.
 *
 * The whole record is rewritten on update. That is a binding decision, and it
 * is the honest one: a key/value store has no field-level write, so YCSB's
 * writeallfields=false has nothing to map onto. It also makes the record a
 * pure function of (keynum, version), which is what lets verification run
 * with a one-byte-per-record table instead of a shadow copy of the store.
 */
static void build_value(uint8_t *out, uint32_t keynum, uint8_t version)
{
	for (uint32_t f = 0; f < FIELD_COUNT; f++) {
		uint8_t *p = out + (size_t)f * FIELD_LEN;

		sys_put_le32(keynum, p);
		sys_put_le16((uint16_t)f, p + 4);
		sys_put_le16(version, p + 6);
		for (uint32_t j = 8; j < FIELD_LEN; j++) {
			p[j] = (uint8_t)(keynum * 31u + f * 17u + version * 7u + j);
		}
	}
}

/* ------------------------------------------------------------- measurement */

struct phase {
	const char *name;
	uint32_t ops;
	uint32_t kinds[OP_KINDS];
	int64_t ms;
	/* latency, in histogram buckets of CONFIG_APP_PERF_YCSB_HIST_BUCKET_US */
	uint32_t hist[CONFIG_APP_PERF_YCSB_HIST_BUCKETS];
	uint32_t over;          /* samples past the top bucket */
	uint32_t min_us, max_us;
	uint64_t sum_us;
};

static struct phase ph;

static void phase_begin(const char *name)
{
	memset(&ph, 0, sizeof(ph));
	ph.name = name;
	ph.min_us = UINT32_MAX;
}

static void phase_sample(uint32_t us)
{
	uint32_t b = us / CONFIG_APP_PERF_YCSB_HIST_BUCKET_US;

	if (b < CONFIG_APP_PERF_YCSB_HIST_BUCKETS) {
		ph.hist[b]++;
	} else {
		ph.over++;
	}
	if (us < ph.min_us) {
		ph.min_us = us;
	}
	if (us > ph.max_us) {
		ph.max_us = us;
	}
	ph.sum_us += us;
	ph.ops++;
}

/*
 * Upper edge of the bucket the p-th percentile falls in, clamped to the
 * largest sample actually seen — a histogram reports the bucket, and without
 * the clamp a run whose every sample lands in bucket 0 prints a p99 above its
 * own maximum. Samples past the top bucket have no value at all, so a
 * percentile that lands in the overflow says so rather than guessing.
 */
static int64_t percentile_us(unsigned int permille)
{
	const uint64_t target = ((uint64_t)ph.ops * permille + 999) / 1000;
	uint64_t seen = 0;

	for (uint32_t b = 0; b < CONFIG_APP_PERF_YCSB_HIST_BUCKETS; b++) {
		seen += ph.hist[b];
		if (seen >= target) {
			int64_t edge = (int64_t)(b + 1) *
				       CONFIG_APP_PERF_YCSB_HIST_BUCKET_US;

			return MIN(edge, (int64_t)ph.max_us);
		}
	}
	return -1;   /* in the overflow */
}

static void print_us(const char *label, int64_t us)
{
	if (us < 0) {
		printk(" %s >%u", label,
		       (unsigned)(CONFIG_APP_PERF_YCSB_HIST_BUCKETS *
				  CONFIG_APP_PERF_YCSB_HIST_BUCKET_US));
	} else {
		printk(" %s %" PRId64, label, us);
	}
}

static void phase_report(void)
{
	const uint64_t ms = (uint64_t)(ph.ms > 0 ? ph.ms : 0);
	const uint64_t milli_ops_per_s =
		ms ? (uint64_t)ph.ops * 1000000ULL / ms : 0;
	const uint64_t us_per_op = ph.ops ? ms * 1000ULL / ph.ops : 0;

	/* "bench <phase> : N ops in M ms" and "io <phase> : rd ..." are the two
	 * shapes app_perf_l0/tools/l0_timing.py parses. Printing them verbatim
	 * is what lets a native_sim capture — where wall clock is zero — be
	 * turned into predicted hardware milliseconds by the L0 model, instead
	 * of waiting for a board. */
	printk("bench %-6s: %5u ops in %6" PRId64 " ms  -> %6u.%03u ops/s  (%7llu us/op)\n",
	       ph.name, ph.ops, ph.ms,
	       (unsigned)(milli_ops_per_s / 1000),
	       (unsigned)(milli_ops_per_s % 1000),
	       (unsigned long long)us_per_op);

	printk("   mix       : read %u  update %u  insert %u  rmw %u\n",
	       ph.kinds[OP_READ], ph.kinds[OP_UPDATE],
	       ph.kinds[OP_INSERT], ph.kinds[OP_RMW]);

	printk("   lat us    : min %u  avg %llu",
	       ph.ops ? ph.min_us : 0,
	       (unsigned long long)(ph.ops ? ph.sum_us / ph.ops : 0));
	print_us("p95", percentile_us(950));
	print_us("p99", percentile_us(990));
	printk("  max %u", ph.max_us);
	if (ph.over) {
		printk("   (%u past the top bucket)", ph.over);
	}
	printk("\n");
}

#if defined(CONFIG_BLOB_DB_IOSTATS)
static struct blob_db_iostats io_mark;

static void io_reset(void)
{
	blob_db_iostats_get(&io_mark);
}

/*
 * Flash the phase actually touched, and what it cost per useful byte.
 *
 * Amplification is the number this benchmark exists to produce. Operations and
 * bytes are both printed because a serial NOR part charges a fixed
 * command-and-address cost per transaction, so a change that moves fewer bytes
 * in more transactions is not obviously a win (the same reasoning as
 * app_perf/src/main.c).
 */
static void io_line(const char *phase, uint64_t useful_bytes, uint32_t ops)
{
	struct blob_db_iostats now;

	blob_db_iostats_get(&now);

	const uint32_t rd = now.reads - io_mark.reads;
	const uint32_t wr = now.writes - io_mark.writes;
	const uint32_t er = now.erases - io_mark.erases;
	const uint64_t brd = now.bytes_read - io_mark.bytes_read;
	const uint64_t bwr = now.bytes_written - io_mark.bytes_written;
	const uint64_t ber = now.bytes_erased - io_mark.bytes_erased;

	printk("   io %-6s : rd %6u ops/%9llu B   wr %5u ops/%9llu B   er %4u ops/%9llu B\n",
	       phase, rd, (unsigned long long)brd, wr, (unsigned long long)bwr,
	       er, (unsigned long long)ber);

	if (ops > 0) {
		printk("   per op    : rd %u ops/%llu B   wr %u.%02u ops/%llu B\n",
		       rd / ops, (unsigned long long)(brd / ops),
		       wr / ops, (wr * 100u / ops) % 100u,
		       (unsigned long long)(bwr / ops));
	}
	if (useful_bytes > 0) {
		printk("   ampl      : rd %llu.%02llux   wr %llu.%02llux   (per %llu B of record data)\n",
		       (unsigned long long)(brd / useful_bytes),
		       (unsigned long long)((brd * 100 / useful_bytes) % 100),
		       (unsigned long long)(bwr / useful_bytes),
		       (unsigned long long)((bwr * 100 / useful_bytes) % 100),
		       (unsigned long long)useful_bytes);
	}
}
#else
#define io_reset()          ((void)0)
#define io_line(phase, useful, ops)  ((void)0)
#endif

/* --------------------------------------------------------------- self-test */

#if defined(CONFIG_APP_PERF_YCSB_SELFTEST)
static uint32_t st_counts[RECORD_COUNT];

/*
 * A wrong key distribution does not look wrong in the output — it looks like a
 * store with a suspiciously good cache hit rate. So before any flash is
 * touched, draw from the generator that will drive the run and check that the
 * hottest 1 % of keys take the share zeta() says they should.
 *
 * The prediction is computed, not tabulated, so changing recordcount or the
 * Zipfian constant re-derives the expectation instead of silently invalidating
 * a hard-coded one.
 */
static int generator_selftest(void)
{
	const uint32_t k = MAX(1u, RECORD_COUNT / 100u);
	const uint32_t draws = CONFIG_APP_PERF_YCSB_SELFTEST_DRAWS;
	struct ycsb_rng r;
	uint64_t head = 0;

	ycsb_rng_seed(&r, CONFIG_APP_PERF_YCSB_SEED);
	memset(st_counts, 0, sizeof(st_counts));

	for (uint32_t i = 0; i < draws; i++) {
		st_counts[ycsb_zipfian_next(&zipf_read, &r)]++;
	}

	/* Sum the k largest counts. k is recordcount/100, so a scan per pick
	 * is cheaper than any structure worth writing. */
	for (uint32_t i = 0; i < k; i++) {
		uint32_t best = 0, best_at = 0;

		for (uint32_t j = 0; j < RECORD_COUNT; j++) {
			if (st_counts[j] > best) {
				best = st_counts[j];
				best_at = j;
			}
		}
		head += best;
		st_counts[best_at] = 0;
	}

	const unsigned int got = (unsigned int)(head * 1000ULL / draws);
	const double predicted = ycsb_zeta(k, THETA) / ycsb_zeta(RECORD_COUNT, THETA);
	const unsigned int want = (unsigned int)(predicted * 1000.0);
	const int delta = (int)got - (int)want;

	printk("generator  : theta %u.%03u, top %u of %u keys took %u.%u %% of %u draws "
	       "(zeta predicts %u.%u %%)\n",
	       CONFIG_APP_PERF_YCSB_ZIPFIAN_PERMILLE / 1000,
	       CONFIG_APP_PERF_YCSB_ZIPFIAN_PERMILLE % 1000,
	       k, (unsigned)RECORD_COUNT, got / 10, got % 10, draws, want / 10, want % 10);

	/* 5 points of slack: the scramble is a hash, not a permutation, so
	 * colliding ranks pile onto one key and shift the head a little. */
	if (delta < -50 || delta > 50) {
		printk("GENERATOR FAIL: head share %u.%u %% is not the predicted %u.%u %%\n",
		       got / 10, got % 10, want / 10, want % 10);
		return -EILSEQ;
	}
	printk("generator  : PASS\n");
	return 0;
}
#else
static int generator_selftest(void) { return 0; }
#endif

/* ----------------------------------------------------------------- sizing */

static uint32_t isqrt(uint64_t v)
{
	uint64_t x = v, y = (v + 1) / 2;

	if (v == 0) {
		return 0;
	}
	while (y < x) {
		x = y;
		y = (x + v / x) / 2;
	}
	return (uint32_t)x;
}

/*
 * Choose the bucket count, and prove the choice fits before writing anything.
 *
 * kvhash reads the whole directory AND one whole packed bucket on every
 * operation, so bytes read per operation is 8*B + R*E/B: the directory grows
 * with B, the bucket shrinks with it. The sum is smallest at B = sqrt(R*E/8),
 * and the minimum itself is 2*sqrt(8*R*E) — which is to say read amplification
 * on this container grows as sqrt(recordcount), whatever B is set to. That is
 * a property of the flat directory, and it is the headline this benchmark
 * exists to measure rather than assume.
 *
 * B is rounded up to a power of two from that optimum, which buys headroom
 * against bucket skew for a few percent off the ideal.
 */
static int size_map(void)
{
	uint32_t b = CONFIG_APP_PERF_YCSB_BUCKETS;

	if (b == 0) {
		const uint32_t ideal = isqrt((uint64_t)RECORD_COUNT * ENTRY_LEN / 8u);

		b = 2;
		while (b < ideal && b < MAX_BUCKETS) {
			b *= 2;
		}
	}
	if (b < 2) {
		b = 2;
	}
	if (b > MAX_BUCKETS) {
		b = MAX_BUCKETS;
	}
	n_buckets = (uint16_t)b;

	/* Poisson: with mean load lambda the fullest of B buckets sits near
	 * lambda + 4*sqrt(lambda). Two spare entries cover the small-lambda
	 * case, where that bound is too tight to mean anything. */
	const uint32_t lambda = (RECORD_COUNT + b - 1) / b;
	const uint32_t worst = lambda + 4 * isqrt(lambda) + 2;
	const uint32_t worst_bytes = worst * ENTRY_LEN;
	const uint32_t dir_bytes = 8 + 8 * b;
	/* Exact, not lambda * ENTRY_LEN: the ceiling above is the right bound
	 * for "does the fullest bucket fit", and the wrong one for "what does
	 * an average get read" — it rounds a 1.95-entry mean up to 2. */
	const uint32_t mean_bytes = (uint32_t)((uint64_t)RECORD_COUNT * ENTRY_LEN / b);

	printk("sizing     : %u records x %u B record (%u fields x %u B) = %u B entry\n",
	       (unsigned)RECORD_COUNT, (unsigned)VAL_LEN,
	       (unsigned)FIELD_COUNT, (unsigned)FIELD_LEN, (unsigned)ENTRY_LEN);
	printk("sizing     : %u buckets (max %u at payload %u B); directory %u B, "
	       "mean bucket %u B, predicted fullest %u B (%u %% of payload)\n",
	       (unsigned)b, (unsigned)MAX_BUCKETS, (unsigned)MAX_PAYLOAD,
	       (unsigned)dir_bytes, (unsigned)mean_bytes, (unsigned)worst_bytes,
	       (unsigned)(worst_bytes * 100u / MAX_PAYLOAD));
	printk("sizing     : predicted read per get = %u B dir + %u B bucket = %u B "
	       "-> %u.%02ux amplification\n",
	       (unsigned)dir_bytes, (unsigned)mean_bytes,
	       (unsigned)(dir_bytes + mean_bytes),
	       (unsigned)((dir_bytes + mean_bytes) / VAL_LEN),
	       (unsigned)((dir_bytes + mean_bytes) * 100u / VAL_LEN % 100u));

	if (worst_bytes > MAX_PAYLOAD) {
		printk("SIZING FAIL: the fullest bucket needs %u B but a payload holds %u B.\n"
		       "  Raise CONFIG_BLOB_DB_MAX_PAYLOAD_LEN, lower "
		       "CONFIG_APP_PERF_YCSB_RECORD_COUNT,\n"
		       "  or shrink the record (FIELD_COUNT x FIELD_LEN).\n",
		       (unsigned)worst_bytes, (unsigned)MAX_PAYLOAD);
		return -ENOSPC;
	}
	return 0;
}

/* ------------------------------------------------------------------ the run */

static int do_read(kvdb_t *db, uint32_t keynum)
{
	char key[KEY_LEN + 1];
	size_t len;

	key_name(key, sizeof(key), keynum);

	int rc = kvdb_get(db, key, got_buf, sizeof(got_buf), &len);

	if (rc != 0) {
		LOG_ERR("get(%s): %d", key, rc);
		return rc;
	}
	if (!IS_ENABLED(CONFIG_APP_PERF_YCSB_VERIFY)) {
		return 0;
	}
	if (len != VAL_LEN) {
		LOG_ERR("get(%s): %zu bytes, expected %u", key, len, (unsigned)VAL_LEN);
		verify_failures++;
		return 0;
	}
	build_value(val_buf, keynum, rec_version[keynum]);
	if (memcmp(got_buf, val_buf, VAL_LEN) != 0) {
		LOG_ERR("get(%s): content mismatch at version %u",
			key, rec_version[keynum]);
		verify_failures++;
	}
	return 0;
}

static int do_write(kvdb_t *db, uint32_t keynum, uint8_t version)
{
	char key[KEY_LEN + 1];

	key_name(key, sizeof(key), keynum);
	build_value(val_buf, keynum, version);

	int rc = kvdb_set(db, key, val_buf, VAL_LEN);

	if (rc != 0) {
		LOG_ERR("set(%s): %d", key, rc);
		return rc;
	}
	rec_version[keynum] = version;
	return 0;
}

/* YCSB's load phase: insert recordcount records, keys user0000000000 up. */
static int load(kvdb_t *db)
{
	phase_begin("load");
	io_reset();

	int64_t t = k_uptime_get();

	for (uint32_t i = 0; i < RECORD_COUNT; i++) {
		uint32_t c0 = k_cycle_get_32();
		int rc = do_write(db, i, 0);

		if (rc != 0) {
			return rc;
		}
		phase_sample(k_cyc_to_us_floor32(k_cycle_get_32() - c0));
		ph.kinds[OP_INSERT]++;
	}
	ph.ms = k_uptime_delta(&t);
	n_records = RECORD_COUNT;

	phase_report();
	io_line("load", (uint64_t)RECORD_COUNT * VAL_LEN, ph.ops);
	return 0;
}

static enum op_kind pick_op(const struct workload *w)
{
	const uint32_t p = (uint32_t)ycsb_rng_below(&rng, 1000);

	for (int k = 0; k < OP_KINDS; k++) {
		if (p < w->cum[k]) {
			return (enum op_kind)k;
		}
	}
	return OP_READ;
}

static uint32_t pick_key(const struct workload *w)
{
	if (w->latest) {
		return (uint32_t)ycsb_latest_next(&zipf_latest, &rng, n_records - 1);
	}
	return (uint32_t)ycsb_zipfian_next(&zipf_read, &rng);
}

static int run_workload(kvdb_t *db, const struct workload *w)
{
	phase_begin(w->name);
	io_reset();

	uint64_t useful = 0;
	int64_t t = k_uptime_get();

	for (uint32_t i = 0; i < OP_COUNT; i++) {
		const enum op_kind kind = pick_op(w);
		uint32_t keynum;
		int rc = 0;

		const uint32_t c0 = k_cycle_get_32();

		switch (kind) {
		case OP_READ:
			keynum = pick_key(w);
			rc = do_read(db, keynum);
			useful += VAL_LEN;
			break;
		case OP_UPDATE:
			keynum = pick_key(w);
			rc = do_write(db, keynum, rec_version[keynum] + 1);
			useful += VAL_LEN;
			break;
		case OP_INSERT:
			if (n_records >= MAX_RECORDS) {
				continue;   /* cannot happen: MAX_RECORDS is the worst case */
			}
			keynum = n_records++;
			rc = do_write(db, keynum, 0);
			useful += VAL_LEN;
			break;
		case OP_RMW:
			/* One YCSB operation, both accesses inside it — the
			 * latency of a read-modify-write is the pair. */
			keynum = pick_key(w);
			rc = do_read(db, keynum);
			if (rc == 0) {
				rc = do_write(db, keynum, rec_version[keynum] + 1);
			}
			useful += 2ULL * VAL_LEN;
			break;
		default:
			rc = -EINVAL;
			break;
		}

		if (rc != 0) {
			return rc;
		}
		phase_sample(k_cyc_to_us_floor32(k_cycle_get_32() - c0));
		ph.kinds[kind]++;
	}
	ph.ms = k_uptime_delta(&t);

	phase_report();
	io_line(w->name, useful, ph.ops);
	return 0;
}

int main(void)
{
	int rc;

	printk("\n=== YCSB core workloads on kvdb (L3) — %s ===\n", APP_VERSION_STRING);
	printk("config     : recordcount %u, operationcount %u per workload, "
	       "record %u B, seed %u, verify %s\n",
	       (unsigned)RECORD_COUNT, (unsigned)OP_COUNT, (unsigned)VAL_LEN,
	       (unsigned)CONFIG_APP_PERF_YCSB_SEED,
	       IS_ENABLED(CONFIG_APP_PERF_YCSB_VERIFY) ? "on" : "off");

	ycsb_rng_seed(&rng, CONFIG_APP_PERF_YCSB_SEED);
	ycsb_zipfian_init(&zipf_read, RECORD_COUNT, THETA, true);
	ycsb_zipfian_init(&zipf_latest, MAX_RECORDS, THETA, false);

	rc = generator_selftest();
	if (rc != 0) {
		goto out_nomount;
	}

	rc = size_map();
	if (rc != 0) {
		goto out_nomount;
	}

	int64_t t = k_uptime_get();

	rc = blob_db_mount();
	if (rc != 0) {
		LOG_ERR("mount: %d", rc);
		goto out_nomount;
	}

	/* YCSB is load-then-run against a known-empty store, and the
	 * expected-value table lives in RAM, so every run starts formatted. */
	rc = blob_db_format();
	if (rc != 0) {
		LOG_ERR("format: %d", rc);
		goto out;
	}
	rc = rootreg_init();
	if (rc != 0) {
		LOG_ERR("rootreg_init: %d", rc);
		goto out;
	}

	struct kvdb_config cfg = {
		.backend = KVDB_BACKEND_HASH,
		.initial_capacity = n_buckets,
	};
	kvdb_t db;

	rc = kvdb_open(&db, "ycsb", &cfg);
	if (rc != 0) {
		LOG_ERR("kvdb_open: %d", rc);
		goto out;
	}
	printk("format+open: %6" PRId64 " ms\n", k_uptime_delta(&t));

	/* Pre-format the buckets the id allocator is about to land in, so the
	 * load loop measures the warm write path instead of one cold 64 KB
	 * erase per first touch (app_perf/RESULTS.md). Its cost is real and
	 * is reported on its own line rather than hidden in the load. */
	io_reset();
	t = k_uptime_get();
	int prepared = blob_db_prepare(RECORD_COUNT * 2);

	if (prepared < 0) {
		LOG_ERR("prepare: %d", prepared);
		rc = prepared;
		goto out;
	}
	printk("bench prepare: %5d ops in %6" PRId64 " ms  (bucket pre-format)\n",
	       prepared, k_uptime_delta(&t));
	io_line("prepare", 0, 0);

	rc = load(&db);
	if (rc != 0) {
		goto out;
	}

	ARRAY_FOR_EACH_PTR(workloads, w) {
		rc = run_workload(&db, w);
		if (rc != 0) {
			goto out;
		}
	}

	if (verify_failures) {
		printk("VERIFY FAIL: %u reads did not match their expected bytes\n",
		       verify_failures);
		rc = -EILSEQ;
		goto out;
	}
	printk("VERIFY PASS (%u records, %u workloads)\n",
	       n_records, (unsigned)ARRAY_SIZE(workloads));
	printk("ycsb: done\n");

out:
	blob_db_unmount();
out_nomount:
#ifdef CONFIG_ARCH_POSIX
	posix_exit(rc == 0 ? 0 : 1);
#endif
	return 0;
}
