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
 * Four workloads are measured. Each phase runs erase_all() followed by
 * blob_db_prepare(N_OPS), so the timed loop measures the append-only write
 * path — the one-time sector-erase cost is paid up-front by prepare() and
 * reported separately.
 *   prepare — pre-format N_OPS buckets ahead of the alloc cursor (one 64 KB
 *             sector erase per bucket on mx25r64; the append/prepend loops
 *             below never trigger a bucket format because of this)
 *   prepend — alloc_id + bind(new, next=head) + update(root=new_id)
 *   append  — alloc_id + bind(new, next=0) + update(prev_tail, next=new_id)
 *             (O(1) via a caller-side tail cache)
 *   read    — traverse from head, get() every node
 *   update  — rewrite the payload of every existing node in place
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
 *
 * Wall-clock is via k_uptime_delta(); ops/sec is derived at millisecond
 * resolution — for flash operations that costs less than 1 % vs a raw
 * cycle counter, and stays sane over runs measured in seconds.
 *
 * CAVEAT for native_sim: the flash simulator does not model erase or program
 * latency unless CONFIG_FLASH_SIMULATOR_SIMULATE_TIMING is set with the
 * MIN_*_TIME_US knobs calibrated to the target part. Without that, these
 * phases measure CPU and memcpy, not storage — which will flatter paths that
 * move fewer bytes and penalise paths that issue more transactions. Run on
 * hardware, or calibrate the simulator, before drawing conclusions.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include <app/lib/blob_db.h>

#include <zephyr/app_version.h>

LOG_MODULE_REGISTER(app_perf, CONFIG_APP_PERF_LOG_LEVEL);

#define N_OPS   CONFIG_APP_PERF_N_OPS
#define VAL_LEN CONFIG_APP_PERF_VAL_LEN

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

	printk("   io %-10s: rd %6u ops/%8llu B   wr %5u ops/%8llu B   "
	       "er %4u",
	       what, rd, (unsigned long long)brd, wr,
	       (unsigned long long)bwr, er);

	/* Amplification against the bytes the caller actually asked for. */
	if (useful_bytes > 0) {
		printk("   ampl rd %llu.%02llux wr %llu.%02llux",
		       (unsigned long long)(brd / useful_bytes),
		       (unsigned long long)((brd * 100 / useful_bytes) % 100),
		       (unsigned long long)(bwr / useful_bytes),
		       (unsigned long long)((bwr * 100 / useful_bytes) % 100));
	}
	printk("\n");
}
#else
#define io_reset()            ((void)0)
#define io_line(what, bytes)  ((void)0)
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

/* --- large-object phases ----------------------------------------------- */
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)

#define OBJ_LEN   CONFIG_APP_PERF_OBJ_LEN
#define N_LARGE   CONFIG_APP_PERF_N_LARGE
#define N_PART    CONFIG_APP_PERF_N_PART
#define PART_LEN  CONFIG_APP_PERF_PART_LEN

BUILD_ASSERT(OBJ_LEN <= CONFIG_BLOB_DB_MAX_OBJECT_LEN,
	     "APP_PERF_OBJ_LEN exceeds CONFIG_BLOB_DB_MAX_OBJECT_LEN");
BUILD_ASSERT(PART_LEN <= OBJ_LEN, "APP_PERF_PART_LEN exceeds OBJ_LEN");

/* Static, not stack: OBJ_LEN is tens of kilobytes. */
static uint8_t g_obj[OBJ_LEN];
static uint8_t g_win[PART_LEN];
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

/* Deterministic offsets, so runs are comparable and a regression is not
 * masked by a different random walk. */
static uint32_t lcg(uint32_t *state)
{
	*state = (*state * 1103515245u) + 12345u;
	return *state >> 8;
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
	 * parameters (app_perf/RESULTS.md). */
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
#endif /* CONFIG_BLOB_DB_LARGE_PAYLOADS */

int main(void)
{
	printk("blob_db perf %s  (N_OPS=%u  VAL_LEN=%u  node=%u B)\n",
	       APP_VERSION_STRING, (unsigned)N_OPS, (unsigned)VAL_LEN,
	       (unsigned)sizeof(struct node));

	int rc = blob_db_mount();

	if (rc < 0) {
		LOG_ERR("mount: %d", rc);
		return 0;
	}

	uint8_t val[VAL_LEN];

	for (size_t i = 0; i < VAL_LEN; i++) {
		val[i] = (uint8_t)i;
	}

	/* ============ prepend phase ==================================== */
	rc = blob_db_erase_all();
	if (rc < 0) {
		LOG_ERR("erase_all: %d", rc);
		goto out;
	}

	int64_t tp = k_uptime_get();
	int prepared = blob_db_prepare(N_OPS);

	if (prepared < 0) {
		LOG_ERR("prepare: %d", prepared);
		goto out;
	}
	bench_line("prepare", prepared, k_uptime_delta(&tp));

	int64_t t0 = k_uptime_get();

	for (int i = 0; i < N_OPS; i++) {
		uint64_t id = blob_db_alloc_id();

		if (id == 0) {
			LOG_ERR("alloc_id returned 0 at i=%d", i);
			goto out;
		}
		val[0] = (uint8_t)i;
		rc = list_prepend(id, val);
		if (rc < 0) {
			LOG_ERR("prepend %d: %d", i, rc);
			goto out;
		}
	}
	bench_line("prepend", N_OPS, k_uptime_delta(&t0));

	uint32_t sum = 0;

	rc = bench_phase_read_and_update(val, &sum);
	if (rc < 0) {
		goto out;
	}
	printk("prepend checksum: 0x%08x\n", sum);

	/* ============ append phase ===================================== */
	rc = blob_db_erase_all();
	if (rc < 0) {
		LOG_ERR("erase_all: %d", rc);
		goto out;
	}

	tp = k_uptime_get();
	prepared = blob_db_prepare(N_OPS);
	if (prepared < 0) {
		LOG_ERR("prepare: %d", prepared);
		goto out;
	}
	bench_line("prepare", prepared, k_uptime_delta(&tp));

	int64_t t1 = k_uptime_get();
	uint64_t tail = 0;   /* caller-side tail cache; O(1) append */

	for (int i = 0; i < N_OPS; i++) {
		uint64_t id = blob_db_alloc_id();

		if (id == 0) {
			LOG_ERR("alloc_id returned 0 at i=%d", i);
			goto out;
		}
		val[0] = (uint8_t)i;
		rc = list_append(id, tail, val);
		if (rc < 0) {
			LOG_ERR("append %d: %d", i, rc);
			goto out;
		}
		tail = id;
	}
	bench_line("append", N_OPS, k_uptime_delta(&t1));

	sum = 0;
	rc = bench_phase_read_and_update(val, &sum);
	if (rc < 0) {
		goto out;
	}
	printk("append checksum:  0x%08x\n", sum);

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	rc = bench_large();
	if (rc < 0) {
		LOG_ERR("large-object phases: %d", rc);
	}
#endif

out:
	blob_db_unmount();
	return 0;
}
