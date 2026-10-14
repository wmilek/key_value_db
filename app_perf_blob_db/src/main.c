/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db performance benchmark.
 *
 * Builds a singly-linked list on top of the blob_db API:
 *   - each node is one blob: { uint64_t next_id; uint8_t val[VAL_LEN]; }
 *   - the head id is stored at BLOB_DB_ROOT_ID (empty root == empty list)
 *
 * Phases, in run order:
 *   mount empty — remount a store holding only the root: mount's floor
 *
 * Then twice, once building the list by prepending and once by appending.
 * Each pass runs erase_all() followed by blob_db_prepare(N_OPS), so the timed
 * insert loop measures the append-only write path — the one-time sector-erase
 * cost is paid up-front by prepare() and reported separately.
 *   prepare — pre-format N_OPS buckets ahead of the alloc cursor (one 64 KB
 *             sector erase per bucket on mx25r64; the append/prepend loops
 *             below never trigger a bucket format because of this)
 *   prepend — alloc_id + bind(new, next=head) + update(root=new_id)
 *   append  — alloc_id + bind(new, next=0) + update(prev_tail, next=new_id)
 *             (O(1) via a caller-side tail cache)
 *   read    — traverse from head, get() every node
 *   update  — rewrite the payload of every existing node in place
 *
 *   mount list — remount with the list in place; it must read back the same
 *
 * With CONFIG_BLOB_DB_LARGE_PAYLOADS a second group of phases exercises
 * objects too large for one slot — stored as segments plus an index record —
 * and the partial-access API that exists for them:
 *   lg write   — blob_db_update() of N_LARGE objects of OBJ_LEN each, onto
 *                unformatted buckets, so the sector erases are included
 *   lg rewrite — the same objects written again over now-formatted buckets;
 *                the gap against "lg write" IS the erase cost, measured
 *                rather than assumed
 *   lg read    — blob_db_read() over each object in PART_LEN windows
 *   lg pread   — N_PART reads at pseudo-random offsets, reported per quartile
 *                of the object. Contract R2 says cost is independent of
 *                offset; four similar numbers support that, a rising series
 *                falsifies it
 *   lg pwrite  — N_PART partial writes at pseudo-random offsets. Compare
 *                us/op against "lg rewrite": that ratio is the whole reason
 *                blob_db_write exists, and it should grow with OBJ_LEN
 *                (rewrite) while staying flat (pwrite)
 *   mount large — remount with the large objects' segments in every bucket
 *
 * Last, on the store as it stands:
 *   churn       — CHURN_OPS updates of a small hot set, until its buckets
 *                 fill and compact: the steady-state update cost, with plain
 *                 and compacting updates reported apart (see bench_churn())
 *   mount churn — remount after compaction; churn and large contents are
 *                 then checked against what was written
 *
 * The run ends with "blob_db perf: all phases passed", or with an error and,
 * on native_sim, a non-zero exit. Every remount is followed by a check that
 * the data written before it reads back unchanged.
 *
 * With CONFIG_BLOB_DB_IOSTATS each phase also prints an `io` line (flash
 * operations and bytes) and, when the phase wrote a master, formatted a
 * bucket or compacted one, a `why` line counting those — the usual reasons
 * behind its erases.
 *
 * Wall-clock is via k_uptime_delta(); ops/sec is derived at millisecond
 * resolution — for flash operations that costs less than 1 % vs a raw
 * cycle counter, and stays sane over runs measured in seconds. The churn
 * phase is the exception: it times each update alone, with the cycle
 * counter, to separate the ones that compacted.
 *
 * CAVEAT for native_sim: the flash simulator does not model erase or program
 * latency unless CONFIG_FLASH_SIMULATOR_SIMULATE_TIMING is set with the
 * MIN_*_TIME_US knobs calibrated to the target part. Without that, these
 * phases measure CPU and memcpy, not storage — which will flatter paths that
 * move fewer bytes and penalise paths that issue more transactions. Run on
 * hardware, or calibrate the simulator, before drawing conclusions.
 * The I/O counters, by contrast, are exact on native_sim: boards/native_sim.
 * overlay gives the simulated flash the DK's geometry, so they match the
 * board's operation for operation.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_iostats.h>

#include <zephyr/app_version.h>

#ifdef CONFIG_ARCH_POSIX
#include "posix_board_if.h"   /* posix_exit() — end the sim run cleanly */
#endif

LOG_MODULE_REGISTER(app_perf_blob_db, CONFIG_APP_PERF_BLOB_DB_LOG_LEVEL);

#define N_OPS   CONFIG_APP_PERF_BLOB_DB_N_OPS
#define VAL_LEN CONFIG_APP_PERF_BLOB_DB_VAL_LEN

struct __packed node {
	uint64_t next_id;
	uint8_t  val[VAL_LEN];
};

BUILD_ASSERT(sizeof(struct node) <= CONFIG_BLOB_DB_MAX_PAYLOAD_LEN,
	     "node exceeds CONFIG_BLOB_DB_MAX_PAYLOAD_LEN");

/* --- list ops on top of blob_db --------------------------------------- */

static int list_head(uint64_t *head_out)
{
	uint64_t head = 0;
	size_t got;
	int rc = blob_db_get(BLOB_DB_ROOT_ID, &head, sizeof(head), &got);

	if (rc < 0) {
		return rc;
	}
	*head_out = (got == sizeof(head)) ? head : 0;
	return 0;
}

static int list_prepend(uint64_t new_id, const uint8_t *val)
{
	uint64_t head;
	int rc = list_head(&head);

	if (rc < 0) {
		return rc;
	}

	struct node n = { .next_id = head };

	memcpy(n.val, val, VAL_LEN);

	rc = blob_db_update(new_id, &n, sizeof(n));
	if (rc < 0) {
		return rc;
	}
	return blob_db_update(BLOB_DB_ROOT_ID, &new_id, sizeof(new_id));
}

/* Append `new_id` after `prev_tail`, or at the head if the list is empty.
 * Two flash writes per call (same as prepend); the caller keeps track of
 * the current tail to avoid an O(n) walk on every append. */
static int list_append(uint64_t new_id, uint64_t prev_tail, const uint8_t *val)
{
	struct node n = { .next_id = 0 };

	memcpy(n.val, val, VAL_LEN);

	int rc = blob_db_update(new_id, &n, sizeof(n));

	if (rc < 0) {
		return rc;
	}

	if (prev_tail == 0) {
		/* Empty list — new node becomes the head. */
		return blob_db_update(BLOB_DB_ROOT_ID, &new_id, sizeof(new_id));
	}

	/* Splice new_id in after prev_tail. */
	struct node prev;
	size_t got;

	rc = blob_db_get(prev_tail, &prev, sizeof(prev), &got);
	if (rc < 0) {
		return rc;
	}
	prev.next_id = new_id;
	return blob_db_update(prev_tail, &prev, sizeof(prev));
}

/* Returns number of nodes visited (0 on empty list, negative on error). */
static int list_traverse(uint32_t *checksum_out)
{
	uint64_t cur;
	int rc = list_head(&cur);

	if (rc < 0) {
		return rc;
	}

	uint32_t sum = 0;
	int n = 0;

	while (cur != 0) {
		struct node node;
		size_t got;

		rc = blob_db_get(cur, &node, sizeof(node), &got);
		if (rc < 0) {
			return rc;
		}
		if (got != sizeof(node)) {
			LOG_ERR("bad node at id=%llu got=%zu", cur, got);
			return -EIO;
		}
		/* Fold the payload into a checksum so the compiler cannot
		 * elide the reads. */
		for (size_t i = 0; i < VAL_LEN; i++) {
			sum = sum * 33u + node.val[i];
		}
		n++;
		cur = node.next_id;
	}
	if (checksum_out) {
		*checksum_out = sum;
	}
	return n;
}

/* --- benchmark harness ------------------------------------------------- */

/* Deterministic offsets and choices, so runs are comparable and a regression
 * is not masked by a different random walk. */
static uint32_t lcg(uint32_t *state)
{
	*state = (*state * 1103515245u) + 12345u;
	return *state >> 8;
}

static void bench_line(const char *what, int ops, int64_t ms)
{
	/* Print ops/s with three fractional digits so sub-1-op/s workloads
	 * (e.g. a cold add pass on QSPI) still show a meaningful number. */
	uint64_t milli_ops_per_s =
		(ms > 0) ? (uint64_t)ops * 1000000ULL / (uint64_t)ms : 0;
	uint64_t us_per_op =
		(ops > 0) ? (uint64_t)ms * 1000ULL / (uint64_t)ops : 0;

	printk("bench %-6s : %4d ops in %7" PRId64 " ms  -> "
	       "%4u.%03u ops/s  (%9llu us/op)\n",
	       what, ops, ms,
	       (unsigned)(milli_ops_per_s / 1000),
	       (unsigned)(milli_ops_per_s % 1000),
	       (unsigned long long)us_per_op);
}

#if defined(CONFIG_BLOB_DB_IOSTATS)
/* Flash actually touched by the phase just measured.
 *
 * On native_sim this is the only meaningful number the benchmark produces: the
 * flash simulator models no latency, so every wall-clock figure rounds to zero.
 * It is also the number that settles arguments wall-clock cannot — amplification
 * is a ratio of bytes, and transaction count is what a serial part charges a
 * fixed cost for. Both are printed because a change can improve one and worsen
 * the other.
 */
static struct blob_db_iostats io_mark;

static void io_reset(void)
{
	blob_db_iostats_get(&io_mark);
}

static void io_line(const char *what, size_t useful_bytes)
{
	struct blob_db_iostats now;

	blob_db_iostats_get(&now);

	const uint32_t rd = now.reads - io_mark.reads;
	const uint32_t wr = now.writes - io_mark.writes;
	const uint32_t er = now.erases - io_mark.erases;
	const uint64_t brd = now.bytes_read - io_mark.bytes_read;
	const uint64_t bwr = now.bytes_written - io_mark.bytes_written;
	/* Erase BYTES, not just calls: one call may cover one erase block or
	 * the whole partition (blob_db_erase_all() does exactly that), and a
	 * cost model fed only the call count cannot tell those apart. This is
	 * the field app_perf_l0/tools/l0_timing.py needs to charge an erase
	 * per block; captures older than it carry the count alone. */
	const uint64_t ber = now.bytes_erased - io_mark.bytes_erased;

	printk("   io %-10s: rd %6u ops/%8llu B   wr %5u ops/%8llu B   "
	       "er %4u ops/%8llu B",
	       what, rd, (unsigned long long)brd, wr,
	       (unsigned long long)bwr, er, (unsigned long long)ber);

	/* Amplification against the bytes the caller actually asked for. */
	if (useful_bytes > 0) {
		printk("   ampl rd %llu.%02llux wr %llu.%02llux",
		       (unsigned long long)(brd / useful_bytes),
		       (unsigned long long)((brd * 100 / useful_bytes) % 100),
		       (unsigned long long)(bwr / useful_bytes),
		       (unsigned long long)((bwr * 100 / useful_bytes) % 100));
	}
	printk("\n");

	/* What the erases above were for. Only printed when something happened:
	 * a read phase has nothing to explain. Deliberately not an `io` line —
	 * l0_timing.py parses those, and these are causes, not more cost. */
	const uint32_t mw = now.master_writes - io_mark.master_writes;
	const uint32_t bf = now.bucket_formats - io_mark.bucket_formats;
	const uint32_t cp = now.compactions - io_mark.compactions;
	const uint64_t bcp = now.bytes_compacted - io_mark.bytes_compacted;

	if (mw || bf || cp) {
		printk("  why %-10s: master writes %4u   bucket formats %4u   "
		       "compactions %4u (%llu B live rewritten)\n",
		       what, mw, bf, cp, (unsigned long long)bcp);
	}
}

static uint32_t io_compactions(void)
{
	struct blob_db_iostats now;

	blob_db_iostats_get(&now);
	return now.compactions;
}
#else
#define io_reset()            ((void)0)
#define io_line(what, bytes)  ((void)0)
#define io_compactions()      ((uint32_t)0)
#endif

/* Run every workload from an empty list (erase_all) so timings are
 * apples-to-apples across phases. Returns 0 on success, negative errno on
 * blob_db failures. */
static int bench_phase_read_and_update(uint8_t *val, uint32_t *checksum)
{
	io_reset();
	int64_t t = k_uptime_get();
	int visited = list_traverse(checksum);
	int64_t t_read = k_uptime_delta(&t);

	if (visited < 0) {
		return visited;
	}
	if (visited != N_OPS) {
		LOG_ERR("traverse saw %d, expected %u", visited,
			(unsigned)N_OPS);
	}
	bench_line("read", visited, t_read);
	io_line("read", (size_t)visited * sizeof(struct node));

	/* Warm update: walk again and rewrite every node in place. The
	 * bucket is already formatted, so no sector erase runs. */
	uint64_t cur;
	int rc = list_head(&cur);

	if (rc < 0 || cur == 0) {
		return rc;
	}

	io_reset();
	t = k_uptime_get();
	int updated = 0;

	while (cur != 0) {
		struct node node;
		size_t got;

		rc = blob_db_get(cur, &node, sizeof(node), &got);
		if (rc < 0) {
			return rc;
		}
		node.val[0]++;
		rc = blob_db_update(cur, &node, sizeof(node));
		if (rc < 0) {
			return rc;
		}
		updated++;
		cur = node.next_id;
	}
	int64_t t_upd = k_uptime_delta(&t);

	bench_line("update", updated, t_upd);
	io_line("update", (size_t)updated * sizeof(struct node));

	(void)val;
	return 0;
}

/* Mount is the one operation the contract lets read the whole partition (R4):
 * it walks every bucket's slot headers to recover its write cursor and the id
 * ceiling. Its cost is therefore set by what is on flash rather than by
 * anything the caller does, so it is timed at several points of the run, each
 * with a different store behind it. Unmount is not timed — it touches no
 * flash. */
static int bench_mount(const char *what)
{
	int rc = blob_db_unmount();

	if (rc < 0) {
		LOG_ERR("unmount before %s: %d", what, rc);
		return rc;
	}

	io_reset();
	int64_t t = k_uptime_get();

	rc = blob_db_mount();

	const int64_t ms = k_uptime_delta(&t);

	if (rc < 0) {
		LOG_ERR("%s: %d", what, rc);
		return rc;
	}
	bench_line(what, 1, ms);
	io_line(what, 0);
	return 0;
}

/* --- large-object phases ----------------------------------------------- */
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)

#define OBJ_LEN   CONFIG_APP_PERF_BLOB_DB_OBJ_LEN
#define N_LARGE   CONFIG_APP_PERF_BLOB_DB_N_LARGE
#define N_PART    CONFIG_APP_PERF_BLOB_DB_N_PART
#define PART_LEN  CONFIG_APP_PERF_BLOB_DB_PART_LEN

BUILD_ASSERT(OBJ_LEN <= CONFIG_BLOB_DB_MAX_OBJECT_LEN,
	     "APP_PERF_BLOB_DB_OBJ_LEN exceeds CONFIG_BLOB_DB_MAX_OBJECT_LEN");
BUILD_ASSERT(PART_LEN <= OBJ_LEN, "APP_PERF_BLOB_DB_PART_LEN exceeds OBJ_LEN");

/* Static, not stack: OBJ_LEN is tens of kilobytes. */
static uint8_t __aligned(4) g_obj[OBJ_LEN];
static uint8_t __aligned(4) g_win[PART_LEN];
static uint64_t g_ids[N_LARGE];

/* Throughput line for the phases where bytes matter more than operations. */
static void bench_bytes(const char *what, int ops, size_t bytes, int64_t ms)
{
	uint64_t kb_per_s =
		(ms > 0) ? (uint64_t)bytes * 1000ULL / 1024ULL / (uint64_t)ms : 0;
	uint64_t us_per_op =
		(ops > 0) ? (uint64_t)ms * 1000ULL / (uint64_t)ops : 0;

	printk("bench %-10s: %4d ops in %7" PRId64 " ms  -> "
	       "%6llu KB/s  (%9llu us/op)\n",
	       what, ops, ms, (unsigned long long)kb_per_s,
	       (unsigned long long)us_per_op);
}

static void fill_obj(uint8_t seed)
{
	for (size_t i = 0; i < OBJ_LEN; i++) {
		g_obj[i] = (uint8_t)((i * 31u + seed) ^ (i >> 8));
	}
}

static int bench_large(void)
{
	int rc;

	printk("\n-- large objects (OBJ_LEN=%u  N_LARGE=%u  N_PART=%u  "
	       "PART_LEN=%u) --\n",
	       (unsigned)OBJ_LEN, (unsigned)N_LARGE, (unsigned)N_PART,
	       (unsigned)PART_LEN);

	rc = blob_db_erase_all();
	if (rc < 0) {
		LOG_ERR("erase_all: %d", rc);
		return rc;
	}

	fill_obj(0x5a);

	/* Cold: buckets are unformatted, so every segment pays a sector erase.
	 * Deliberately NOT prepare()d — the contrast with the warm pass below
	 * is what quantifies the erase cost. */
	io_reset();
	int64_t t = k_uptime_get();

	for (int i = 0; i < N_LARGE; i++) {
		g_ids[i] = blob_db_alloc_id();
		if (g_ids[i] == 0) {
			LOG_ERR("alloc_id returned 0 at i=%d", i);
			return -EIO;
		}
		rc = blob_db_update(g_ids[i], g_obj, OBJ_LEN);
		if (rc < 0) {
			LOG_ERR("large write %d: %d", i, rc);
			return rc;
		}
	}
	bench_bytes("lg write", N_LARGE, (size_t)N_LARGE * OBJ_LEN,
		    k_uptime_delta(&t));
	io_line("lg write", (size_t)N_LARGE * OBJ_LEN);

	/* Confirm the store agrees about what was written before timing reads. */
	size_t reported = 0;

	rc = blob_db_size(g_ids[0], &reported);
	if (rc < 0 || reported != OBJ_LEN) {
		LOG_ERR("size mismatch: rc=%d got=%zu want=%u", rc, reported,
			(unsigned)OBJ_LEN);
		return (rc < 0) ? rc : -EIO;
	}

	/* Warm: same objects, buckets now formatted. */
	fill_obj(0xa5);
	io_reset();
	t = k_uptime_get();
	for (int i = 0; i < N_LARGE; i++) {
		rc = blob_db_update(g_ids[i], g_obj, OBJ_LEN);
		if (rc < 0) {
			LOG_ERR("large rewrite %d: %d", i, rc);
			return rc;
		}
	}
	const int64_t t_rewrite = k_uptime_delta(&t);

	bench_bytes("lg rewrite", N_LARGE, (size_t)N_LARGE * OBJ_LEN, t_rewrite);
	io_line("lg rewrite", (size_t)N_LARGE * OBJ_LEN);

	/* Sequential read through a small window, checksummed so the reads
	 * cannot be elided and a wrong byte shows up as a wrong sum. */
	uint32_t sum = 0;

	io_reset();
	t = k_uptime_get();
	for (int i = 0; i < N_LARGE; i++) {
		for (size_t off = 0; off < OBJ_LEN; off += PART_LEN) {
			size_t got = 0;

			rc = blob_db_read(g_ids[i], off, g_win, PART_LEN, &got);
			if (rc < 0) {
				LOG_ERR("lg read %d@%zu: %d", i, off, rc);
				return rc;
			}
			for (size_t b = 0; b < got; b++) {
				sum = sum * 33u + g_win[b];
			}
		}
	}
	bench_bytes("lg read", N_LARGE * (int)(OBJ_LEN / PART_LEN),
		    (size_t)N_LARGE * OBJ_LEN, k_uptime_delta(&t));
	io_line("lg read", (size_t)N_LARGE * OBJ_LEN);
	printk("lg read checksum: 0x%08x\n", sum);

	/* Offset independence (contract R2): same operation, four bands of the
	 * object. Similar numbers support the claim; a rising series refutes it. */
	for (int q = 0; q < 4; q++) {
		const size_t base = (size_t)q * (OBJ_LEN / 4);
		const size_t span = (OBJ_LEN / 4) - PART_LEN;
		uint32_t rnd = 0x1234u + (uint32_t)q;
		char label[16];

		io_reset();
		t = k_uptime_get();
		for (int n = 0; n < N_PART; n++) {
			const size_t off = base + (span ? (lcg(&rnd) % span) : 0);
			size_t got = 0;

			rc = blob_db_read(g_ids[n % N_LARGE], off, g_win,
					  PART_LEN, &got);
			if (rc < 0) {
				LOG_ERR("pread q%d: %d", q, rc);
				return rc;
			}
		}
		(void)snprintk(label, sizeof(label), "lg pread q%d", q);
		bench_bytes(label, N_PART, (size_t)N_PART * PART_LEN,
			    k_uptime_delta(&t));
		io_line(label, (size_t)N_PART * PART_LEN);
	}

	/* Partial writes. The number to compare against "lg rewrite": this is
	 * the difference between touching one segment and rewriting the object. */
	uint32_t rnd = 0xbeefu;

	io_reset();
	t = k_uptime_get();
	for (int n = 0; n < N_PART; n++) {
		const size_t off = lcg(&rnd) % (OBJ_LEN - PART_LEN);

		memset(g_win, (uint8_t)n, PART_LEN);
		rc = blob_db_write(g_ids[n % N_LARGE], off, g_win, PART_LEN);
		if (rc < 0) {
			LOG_ERR("pwrite %d: %d", n, rc);
			return rc;
		}
	}
	const int64_t t_pwrite = k_uptime_delta(&t);

	bench_bytes("lg pwrite", N_PART, (size_t)N_PART * PART_LEN, t_pwrite);
	io_line("lg pwrite", (size_t)N_PART * PART_LEN);

	/* Spell the comparison out, since it is the point of the phase. Two
	 * decimals, because integer division reported the DK's real 1.95x as
	 * "1x" — the truncation swallowed the entire result at these
	 * parameters (app_perf_blob_db/RESULTS.md). */
	if (t_pwrite > 0 && N_PART > 0 && N_LARGE > 0) {
		const uint64_t us_pw =
			(uint64_t)t_pwrite * 1000ULL / (uint64_t)N_PART;
		const uint64_t us_rw =
			(uint64_t)t_rewrite * 1000ULL / (uint64_t)N_LARGE;
		const uint64_t r100 = us_pw ? (us_rw * 100ULL + us_pw / 2) / us_pw
					    : 0;

		printk("partial vs whole-object write: %llu us vs %llu us/op"
		       "  (%llu.%02llux)\n",
		       (unsigned long long)us_pw, (unsigned long long)us_rw,
		       (unsigned long long)(r100 / 100),
		       (unsigned long long)(r100 % 100));
	}

	/* Objects must still read back correctly after all that. */
	rc = blob_db_size(g_ids[0], &reported);
	if (rc < 0 || reported != OBJ_LEN) {
		LOG_ERR("size after pwrite: rc=%d got=%zu", rc, reported);
		return (rc < 0) ? rc : -EIO;
	}
	printk("lg objects intact (%u B each)\n", (unsigned)reported);
	return 0;
}

/* Checksum of every large object's full contents. The churn phase compacts
 * buckets these objects' segments live in, and remounts follow it, so this is
 * taken before and compared after: compaction must carry them over untouched.
 * One read per object, through the static object buffer — not timed. */
static int large_checksum(uint32_t *out)
{
	uint32_t sum = 0;

	for (int i = 0; i < N_LARGE; i++) {
		size_t got = 0;
		int rc = blob_db_read(g_ids[i], 0, g_obj, OBJ_LEN, &got);

		if (rc < 0) {
			LOG_ERR("lg checksum read %d: %d", i, rc);
			return rc;
		}
		if (got != OBJ_LEN) {
			LOG_ERR("lg checksum read %d: got %zu B", i, got);
			return -EIO;
		}
		for (size_t b = 0; b < got; b++) {
			sum = sum * 33u + g_obj[b];
		}
	}
	*out = sum;
	return 0;
}
#endif /* CONFIG_BLOB_DB_LARGE_PAYLOADS */

/* --- churn: steady-state updates, compaction included ------------------- */

#define CHURN_OPS  CONFIG_APP_PERF_BLOB_DB_CHURN_OPS
#define CHURN_SET  CONFIG_APP_PERF_BLOB_DB_CHURN_SET
#define CHURN_LEN  CONFIG_APP_PERF_BLOB_DB_CHURN_LEN

BUILD_ASSERT(CHURN_LEN <= CONFIG_BLOB_DB_MAX_PAYLOAD_LEN,
	     "APP_PERF_BLOB_DB_CHURN_LEN exceeds CONFIG_BLOB_DB_MAX_PAYLOAD_LEN");

static uint64_t g_churn_ids[CHURN_SET];
static uint16_t g_churn_ver[CHURN_SET];
static uint8_t __aligned(4) g_churn_buf[CHURN_LEN];
static uint8_t __aligned(4) g_churn_rd[CHURN_LEN];

/* Contents are a function of (member, version), so the final state can be
 * checked without keeping a copy of it. */
static void churn_fill(int k, uint16_t ver)
{
	for (size_t j = 0; j < CHURN_LEN; j++) {
		g_churn_buf[j] = (uint8_t)(k * 7u + ver * 13u + j * 3u);
	}
}

struct churn_lat {
	uint32_t n;
	uint64_t sum_us;
	uint64_t max_us;
};

static void churn_lat_line(const char *what, const struct churn_lat *l)
{
	printk("churn %-11s: %5u updates  avg %9llu us  max %9llu us\n", what,
	       l->n, (unsigned long long)(l->n ? l->sum_us / l->n : 0),
	       (unsigned long long)l->max_us);
}

static int churn_verify(const char *when)
{
	for (int k = 0; k < CHURN_SET; k++) {
		size_t got = 0;
		int rc = blob_db_get(g_churn_ids[k], g_churn_rd,
				     sizeof(g_churn_rd), &got);

		if (rc < 0) {
			LOG_ERR("churn verify (%s) %d: %d", when, k, rc);
			return rc;
		}
		churn_fill(k, g_churn_ver[k]);
		if (got != CHURN_LEN ||
		    memcmp(g_churn_rd, g_churn_buf, CHURN_LEN) != 0) {
			LOG_ERR("churn verify (%s) %d: wrong contents "
				"(got %zu B)", when, k, got);
			return -EIO;
		}
	}
	printk("churn contents verified %s (%u blobs)\n", when,
	       (unsigned)CHURN_SET);
	return 0;
}

/*
 * Every other phase writes each blob once or twice into a fresh store, so no
 * bucket ever fills and compaction never runs: their figures are the cost of
 * an update with no garbage behind it. This phase is the other case. A small
 * hot set is rewritten over and over, so the buckets it lives in fill with
 * superseded slots and compact; the result is what an update costs in steady
 * state, R3's "amortized" made concrete.
 *
 * It runs on the store as the earlier phases left it, not on an erased one.
 * That is on purpose: with large payloads every bucket then already holds a
 * segment or two of live data, which each compaction has to carry across —
 * a compaction with nothing live to move would flatter it. It also means no
 * bucket needs formatting first, which on 64 KB NOR would be a second per
 * bucket of cost unrelated to what is being measured.
 *
 * Each update is timed alone, with the cycle counter, and classified by
 * whether the compaction counter moved during it. Plain and compacting
 * updates are reported apart, because an average over both describes neither:
 * the first is what most calls cost, the second is the stall a caller has to
 * budget for.
 */
static int bench_churn(void)
{
	printk("\n-- churn (CHURN_OPS=%u  CHURN_SET=%u  CHURN_LEN=%u) --\n",
	       (unsigned)CHURN_OPS, (unsigned)CHURN_SET, (unsigned)CHURN_LEN);

	int prepared = blob_db_prepare(CHURN_SET);

	if (prepared < 0) {
		LOG_ERR("churn prepare: %d", prepared);
		return prepared;
	}

	for (int k = 0; k < CHURN_SET; k++) {
		g_churn_ids[k] = blob_db_alloc_id();
		if (g_churn_ids[k] == 0) {
			LOG_ERR("churn alloc_id returned 0 at k=%d", k);
			return -EIO;
		}
		g_churn_ver[k] = 0;
		churn_fill(k, 0);

		int rc = blob_db_update(g_churn_ids[k], g_churn_buf, CHURN_LEN);

		if (rc < 0) {
			LOG_ERR("churn bind %d: %d", k, rc);
			return rc;
		}
	}

	struct churn_lat plain = { 0 };
	struct churn_lat comp = { 0 };
	uint32_t rnd = 0xc0ffeeu;

	io_reset();
	int64_t t = k_uptime_get();

	for (int op = 0; op < CHURN_OPS; op++) {
		const int k = (int)(lcg(&rnd) % CHURN_SET);

		g_churn_ver[k]++;
		churn_fill(k, g_churn_ver[k]);

		const uint32_t comp_before = io_compactions();
		const uint32_t cyc0 = k_cycle_get_32();
		int rc = blob_db_update(g_churn_ids[k], g_churn_buf, CHURN_LEN);
		const uint64_t us = k_cyc_to_us_floor64(k_cycle_get_32() - cyc0);

		if (rc < 0) {
			LOG_ERR("churn update %d (member %d): %d", op, k, rc);
			return rc;
		}

		struct churn_lat *l =
			(io_compactions() != comp_before) ? &comp : &plain;

		l->n++;
		l->sum_us += us;
		if (us > l->max_us) {
			l->max_us = us;
		}
	}
	bench_line("churn", CHURN_OPS, k_uptime_delta(&t));
	io_line("churn", (size_t)CHURN_OPS * CHURN_LEN);

	if (IS_ENABLED(CONFIG_BLOB_DB_IOSTATS)) {
		churn_lat_line("plain", &plain);
		churn_lat_line("compacting", &comp);

		/* What compaction adds to the average update: the compacting
		 * updates' excess over a plain one, spread over every update. */
		const uint64_t plain_avg = plain.n ? plain.sum_us / plain.n : 0;
		const uint64_t excess = comp.sum_us > comp.n * plain_avg
						? comp.sum_us - comp.n * plain_avg
						: 0;

		printk("churn amortized compaction: %llu us per update\n",
		       (unsigned long long)(excess / MAX(CHURN_OPS, 1)));

		/* Not an error — the store is fine — but the phase has then
		 * measured nothing it exists for. */
		if (comp.n == 0) {
			LOG_WRN("churn: no update compacted; raise CHURN_OPS "
				"or CHURN_LEN, or lower CHURN_SET");
		}
	} else {
		/* Without the counter the two kinds cannot be told apart. */
		churn_lat_line("all", &plain);
	}

	return churn_verify("after churn");
}

/* One list-building phase: erase, prepare, insert N_OPS nodes, then read and
 * update them. `append` picks the insert shape. */
static int bench_list(bool append, uint8_t *val)
{
	const char *what = append ? "append" : "prepend";
	int rc = blob_db_erase_all();

	if (rc < 0) {
		LOG_ERR("erase_all: %d", rc);
		return rc;
	}

	io_reset();
	int64_t tp = k_uptime_get();
	int prepared = blob_db_prepare(N_OPS);

	if (prepared < 0) {
		LOG_ERR("prepare: %d", prepared);
		return prepared;
	}
	bench_line("prepare", prepared, k_uptime_delta(&tp));
	io_line("prepare", 0);

	io_reset();
	int64_t t0 = k_uptime_get();
	uint64_t tail = 0;   /* caller-side tail cache; O(1) append */

	for (int i = 0; i < N_OPS; i++) {
		uint64_t id = blob_db_alloc_id();

		if (id == 0) {
			LOG_ERR("alloc_id returned 0 at i=%d", i);
			return -EIO;
		}
		val[0] = (uint8_t)i;
		rc = append ? list_append(id, tail, val) : list_prepend(id, val);
		if (rc < 0) {
			LOG_ERR("%s %d: %d", what, i, rc);
			return rc;
		}
		tail = id;
	}
	bench_line(what, N_OPS, k_uptime_delta(&t0));
	io_line(what, (size_t)N_OPS * sizeof(struct node));

	uint32_t sum = 0;

	rc = bench_phase_read_and_update(val, &sum);
	if (rc < 0) {
		return rc;
	}
	printk("%s checksum: 0x%08x\n", what, sum);
	return 0;
}

int main(void)
{
	printk("blob_db perf %s  (N_OPS=%u  VAL_LEN=%u  node=%u B)\n",
	       APP_VERSION_STRING, (unsigned)N_OPS, (unsigned)VAL_LEN,
	       (unsigned)sizeof(struct node));

	int rc = blob_db_mount();

	if (rc < 0) {
		LOG_ERR("mount: %d", rc);
		goto out;
	}

	uint8_t val[VAL_LEN];

	for (size_t i = 0; i < VAL_LEN; i++) {
		val[i] = (uint8_t)i;
	}

	/* ============ mount, empty ===================================== */
	/* The floor: a store holding nothing but the root. What the boot mount
	 * above cost depends on whatever the previous run left behind, so it is
	 * not reported. */
	rc = blob_db_erase_all();
	if (rc < 0) {
		LOG_ERR("erase_all: %d", rc);
		goto out;
	}
	rc = bench_mount("mount empty");
	if (rc < 0) {
		goto out;
	}

	/* ============ prepend, then append ============================= */
	rc = bench_list(false, val);
	if (rc < 0) {
		goto out;
	}
	rc = bench_list(true, val);
	if (rc < 0) {
		goto out;
	}

	/* ============ mount, list ====================================== */
	/* The list must read back the same across the remount. */
	uint32_t before = 0;
	uint32_t after = 0;

	rc = list_traverse(&before);
	if (rc < 0) {
		goto out;
	}
	rc = bench_mount("mount list");
	if (rc < 0) {
		goto out;
	}
	rc = list_traverse(&after);
	if (rc < 0) {
		goto out;
	}
	if (rc != N_OPS || after != before) {
		LOG_ERR("list after remount: %d nodes, checksum 0x%08x "
			"(was 0x%08x)", rc, after, before);
		rc = -EIO;
		goto out;
	}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	/* ============ large objects ==================================== */
	rc = bench_large();
	if (rc < 0) {
		LOG_ERR("large-object phases: %d", rc);
		goto out;
	}

	uint32_t lg_sum = 0;
	uint32_t lg_now = 0;

	rc = large_checksum(&lg_sum);
	if (rc < 0) {
		goto out;
	}
	rc = bench_mount("mount large");
	if (rc < 0) {
		goto out;
	}
#endif

	/* ============ churn ============================================ */
	if (CHURN_OPS > 0) {
		rc = bench_churn();
		if (rc < 0) {
			goto out;
		}
		rc = bench_mount("mount churn");
		if (rc < 0) {
			goto out;
		}
		rc = churn_verify("after remount");
		if (rc < 0) {
			goto out;
		}
	}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	/* Churn compacted buckets holding these objects' segments, and the
	 * store has been remounted since they were written. */
	rc = large_checksum(&lg_now);
	if (rc < 0) {
		goto out;
	}
	if (lg_now != lg_sum) {
		LOG_ERR("large objects changed: checksum 0x%08x, was 0x%08x",
			lg_now, lg_sum);
		rc = -EIO;
		goto out;
	}
	printk("lg objects unchanged by churn and remounts (0x%08x)\n", lg_sum);
#endif

	rc = 0;
	printk("\nblob_db perf: all phases passed\n");

out:
	blob_db_unmount();
#ifdef CONFIG_ARCH_POSIX
	posix_exit(rc == 0 ? 0 : 1);
#endif
	return 0;
}
