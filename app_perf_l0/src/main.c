/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * L0 timing benchmark — the raw cost of the `flash_area` API.
 *
 * Every other benchmark in this repository measures a stack: app_perf measures
 * blob_db, app_perf_mc measures the model container, app_cbor_persondb
 * measures a product-shaped workload. All of them bottom out in three L0
 * calls, and none of them can say what those three calls cost, so a change in
 * flash traffic can only be judged after another hardware run.
 *
 * This app measures the bottom directly. It does not mount blob_db; it opens
 * `storage_partition` and times flash_area_read(), flash_area_write() and
 * flash_area_erase() as functions of the one parameter each of them has:
 *
 *   read  / write   — TRANSFER SIZE, from one byte (one write alignment unit
 *                     for writes) up to one erase block, in powers of two
 *                     with the 1.5x midpoints in between. Each sweep prints a
 *                     matrix — size, ops, us/op, KiB/s, ns/B, and the
 *                     MARGINAL ns/B against the row above — because that last
 *                     column is what answers "is this relationship linear":
 *                     it is constant iff the cost is affine, whatever the
 *                     fixed cost happens to be.
 *   write_pg        — the same write, each transfer pinned to a program-page
 *                     boundary, so a part that programs by page shows a clean
 *                     ceil(n/page) staircase instead of the average of two
 *                     straddle cases. This is the phase that says whether the
 *                     write cost has page structure at all.
 *   erase           — ERASE SIZE, swept as the number of erase blocks covered
 *                     by one call. This is the question blob_db_prepare() and
 *                     blob_db_erase_all() ask: is a 16-block erase cheaper
 *                     than sixteen 1-block erases, and by how much?
 *
 * The output is a cost model, not a leaderboard. Fitted to
 *
 *     read  t(n) = R0 + R1*n        n = bytes
 *     write t(n) = W0 + W1*n
 *     erase t(m) = E0 + E1*m        m = erase blocks
 *
 * it turns the operation counters that CONFIG_BLOB_DB_IOSTATS already keeps —
 * ops and bytes for each of the three classes — into predicted wall-clock:
 *
 *     T = R0*reads + R1*bytes_read
 *       + W0*writes + W1*bytes_written
 *       + E0*erases + E1*(bytes_erased / block)
 *
 * That identity is why the model is affine and why the sweep is worth running.
 * For an affine per-operation cost the TOTALS are sufficient — the prediction
 * does not need the size of each individual operation, only how many there
 * were and how many bytes they moved. So a native_sim run, which measures no
 * time at all but counts every operation exactly, becomes a prediction of the
 * hardware it was never run on. `tools/l0_timing.py` does the fitting and the
 * predicting; see README.md for where the identity stops holding.
 *
 * WARNING: this benchmark DESTROYS the contents of `storage_partition`. It
 * erases and rewrites the first CONFIG_APP_PERF_L0_REGION_BLOCKS blocks of it.
 * Do not run it on a device whose store you want to keep.
 *
 * On native_sim the flash simulator models no latency unless
 * CONFIG_FLASH_SIMULATOR_SIMULATE_TIMING is set, so what is measured there is
 * memcpy and call overhead. The run is still useful — it exercises the sweep
 * and produces a well-formed model file — but the model describes the
 * simulator, and every line of output says so.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zephyr/app_version.h>

#if defined(CONFIG_ARCH_POSIX)
#include <posix_board_if.h>
#endif

LOG_MODULE_REGISTER(app_perf_l0, CONFIG_APP_PERF_L0_LOG_LEVEL);

#define MAX_XFER      CONFIG_APP_PERF_L0_MAX_XFER
#define REGION_BLOCKS CONFIG_APP_PERF_L0_REGION_BLOCKS
#define TARGET_MS     CONFIG_APP_PERF_L0_TARGET_MS
#define MAX_REPS      CONFIG_APP_PERF_L0_MAX_REPS
#define ERASE_REPS    CONFIG_APP_PERF_L0_ERASE_REPS

/* One buffer serves every phase; MAX_XFER is clamped to the erase block at
 * runtime, so on a part with small blocks most of it goes unused. */
static uint8_t g_buf[MAX_XFER];

static const struct flash_area *g_fa;

static struct {
	size_t   part_bytes;
	size_t   block;        /* erase block (flash sector) size */
	uint32_t blocks;       /* erase blocks in the partition */
	size_t   align;        /* write block size */
	uint8_t  erased_val;
	size_t   max_xfer;     /* min(MAX_XFER, block) */
	uint32_t region;       /* erase blocks this app is allowed to touch */
} g;

/* --- timing ------------------------------------------------------------ */

/*
 * k_cycle_get_32() rather than k_uptime_*(): a 64 B read on QSPI NOR is tens
 * of microseconds and the millisecond clock cannot see it. Each timed span is
 * one batch of operations, and a 32-bit cycle delta is unambiguous as long as
 * one batch stays under the counter's wrap period — 67 s at 64 MHz, and the
 * batch target is milliseconds. The one operation that can approach that is a
 * multi-block erase, which is why erases are timed one call at a time.
 */
struct span {
	uint64_t ns;
	uint32_t cyc0;
};

static inline void span_start(struct span *s)
{
	s->cyc0 = k_cycle_get_32();
}

static inline uint64_t span_end(struct span *s)
{
	uint32_t d = k_cycle_get_32() - s->cyc0;

	s->ns = k_cyc_to_ns_floor64(d);
	return s->ns;
}

/* --- machine-readable output ------------------------------------------- */

/*
 * Two output streams share the console. The `l0…` lines are the contract with
 * tools/l0_timing.py: one record per line, `key=value` fields, stable field
 * names. Everything else is for the human watching the UART and may change
 * freely. Keeping them apart is what lets a capture be re-parsed years later.
 */
static void raw_erase(const char *op, uint32_t blocks, uint32_t ops,
		      uint64_t total_ns, uint64_t min_ns, uint64_t max_ns)
{
	const uint64_t per_call = ops ? total_ns / ops : 0;

	printk("l0raw op=%s blocks=%u ops=%u total_ns=%llu ns_per_op=%llu "
	       "ns_per_block=%llu min_ns=%llu max_ns=%llu\n",
	       op, blocks, ops, (unsigned long long)total_ns,
	       (unsigned long long)per_call,
	       (unsigned long long)(blocks ? per_call / blocks : 0),
	       (unsigned long long)min_ns, (unsigned long long)max_ns);
}

/* --- the size matrix ---------------------------------------------------- */

/*
 * The table each size sweep prints, and the reason the sweep exists.
 *
 * Cost per operation and throughput answer "how fast is it". The column that
 * answers "is it a line" is the last one: the MARGINAL cost, the extra
 * nanoseconds each extra byte cost between this row and the one above it,
 *
 *     d(ns)/d(B) = (t(n) - t(n_prev)) / (n - n_prev)
 *
 * For an affine cost t(n) = a + b·n this is b at every row — constant, and
 * equal to the fitted slope, whatever the fixed cost a is. So a column of
 * near-identical numbers IS the linearity check, and a column that steps is a
 * cost that a single slope cannot describe.
 *
 * That is not a hypothetical for writes. A NOR part programs whole pages, so
 * the true write cost is ceil(n / page) page programs — a staircase, not a
 * line. Between two doublings a staircase can look perfectly linear, which is
 * why the sweep also measures the 1.5x midpoints, and why the `write_pg`
 * phase pins each transfer to a page boundary to show the steps directly.
 */
struct sweep {
	const char *op;
	size_t   prev_size;
	uint64_t prev_ns;
	bool     have_prev;
	/* Marginal cost range across the sweep, in centi-ns per byte, so the
	 * spread can be reported without floating point. */
	int64_t  marg_min;
	int64_t  marg_max;
	bool     have_marg;
};

static void sweep_begin(struct sweep *s, const char *op, const char *title)
{
	memset(s, 0, sizeof(*s));
	s->op = op;
	printk("\n-- %s --\n", title);
	printk("     size      ops         us/op        KiB/s       ns/B   "
	       "marginal ns/B\n");
}

static void sweep_row(struct sweep *s, size_t size, uint32_t ops,
		      uint64_t total_ns)
{
	const uint64_t ns_op = ops ? total_ns / ops : 0;

	printk("l0raw op=%s size=%zu ops=%u total_ns=%llu ns_per_op=%llu\n",
	       s->op, size, ops, (unsigned long long)total_ns,
	       (unsigned long long)ns_op);

	/* KiB/s = size / (ns_op / 1e9) / 1024, staged to keep the intermediate
	 * inside 64 bits for a 64 KiB transfer. */
	const uint64_t kibs =
		ns_op ? (uint64_t)size * 1000000ULL / ns_op * 1000ULL / 1024ULL
		      : 0;
	/* Average cost per byte, in centi-ns. */
	const uint64_t cns_b = size ? ns_op * 100ULL / size : 0;

	printk("  %7zu  %7u  %8llu.%03llu  %11llu  %6llu.%02llu",
	       size, ops, (unsigned long long)(ns_op / 1000),
	       (unsigned long long)(ns_op % 1000), (unsigned long long)kibs,
	       (unsigned long long)(cns_b / 100),
	       (unsigned long long)(cns_b % 100));

	if (s->have_prev && size > s->prev_size) {
		/* Signed: noise on a flat curve puts this below zero, and
		 * hiding that would misrepresent a curve with no slope at all
		 * as one with a small positive one. */
		const int64_t d_ns = (int64_t)ns_op - (int64_t)s->prev_ns;
		const int64_t d_b = (int64_t)size - (int64_t)s->prev_size;
		const int64_t cmarg = d_ns * 100 / d_b;

		printk("  %8lld.%02llu\n", (long long)(cmarg / 100),
		       (unsigned long long)(llabs(cmarg) % 100));

		if (!s->have_marg) {
			s->marg_min = s->marg_max = cmarg;
			s->have_marg = true;
		} else {
			s->marg_min = MIN(s->marg_min, cmarg);
			s->marg_max = MAX(s->marg_max, cmarg);
		}
	} else {
		printk("         -\n");
	}

	s->prev_size = size;
	s->prev_ns = ns_op;
	s->have_prev = true;
}

/* The verdict the matrix supports, stated rather than left to the reader. */
static void sweep_end(struct sweep *s)
{
	if (!s->have_marg) {
		return;
	}

	printk("l0lin op=%s cmarg_min=%lld cmarg_max=%lld\n", s->op,
	       (long long)s->marg_min, (long long)s->marg_max);

	printk("  marginal cost %lld.%02llu .. %lld.%02llu ns/B",
	       (long long)(s->marg_min / 100),
	       (unsigned long long)(llabs(s->marg_min) % 100),
	       (long long)(s->marg_max / 100),
	       (unsigned long long)(llabs(s->marg_max) % 100));

	if (s->marg_max <= 0) {
		printk("\n  -> cost does not grow with size at all: this "
		       "substrate charges per call.\n");
		return;
	}
	if (s->marg_min <= 0) {
		/* Zero here is a signal, not a gap in the measurement: a cost
		 * that is flat across a range of sizes and then jumps — one
		 * page program covering every transfer up to a page — has
		 * exactly this shape. Calling it noise would report the most
		 * interesting thing the sweep found as an absence. */
		printk("\n  -> NOT affine: the marginal cost is zero or "
		       "negative over part of the sweep\n     and clearly "
		       "positive elsewhere. A cost that is flat across a range "
		       "and\n     then steps looks exactly like this; the "
		       "page-program table below says\n     whether that is "
		       "what this is.\n");
		return;
	}

	/* Spread as a ratio, in hundredths, so "1.08x" prints without a FPU. */
	const uint64_t spread = (uint64_t)s->marg_max * 100 / (uint64_t)s->marg_min;

	printk("  (%llu.%02llux spread)\n", (unsigned long long)(spread / 100),
	       (unsigned long long)(spread % 100));
	if (spread <= 125) {
		printk("  -> affine: one fixed cost plus one cost per byte "
		       "describes this sweep.\n");
	} else {
		printk("  -> NOT affine: the per-byte cost varies %llu.%02llux "
		       "across the sweep, so a\n     single slope misprices "
		       "some sizes. The fit's residuals say by how much.\n",
		       (unsigned long long)(spread / 100),
		       (unsigned long long)(spread % 100));
	}
}

/*
 * Sizes to sweep: powers of two, each followed by its 1.5x midpoint.
 *
 * Powers of two alone are exactly the wrong sample points for a cost that
 * steps at a power-of-two boundary — every sample lands on a step, the
 * staircase looks like a line through its corners, and the sweep concludes
 * "linear" about a function that is not. The midpoints break that.
 *
 * Returns 0 when the sweep is done.
 */
static size_t sweep_first(size_t base)
{
	return base;
}

static size_t sweep_next(size_t size, size_t base, size_t align, size_t max)
{
	/* A midpoint is only reachable when it is still a whole number of
	 * write-alignment units. */
	if (size >= base && (size & (size - 1)) == 0) {
		const size_t mid = size + size / 2;

		if (mid <= max && size / 2 >= align && (size / 2) % align == 0) {
			return mid;
		}
	}
	/* Back to the next power of two: from a midpoint 1.5s, that is 2s. */
	const size_t pow2 = ((size & (size - 1)) == 0) ? size : (size / 3) * 2;
	const size_t next = pow2 * 2;

	return (next <= max) ? next : 0;
}

/* --- sweep helpers ------------------------------------------------------ */

/* Repetition count for one sweep point: enough operations that the batch
 * lasts TARGET_MS, so the cycle counter's resolution and any one-off jitter
 * are both negligible, but never more than the caller can afford. */
static uint32_t reps_for(uint64_t est_ns_per_op, uint32_t cap)
{
	uint64_t want;

	if (est_ns_per_op == 0) {
		want = MAX_REPS;
	} else {
		want = (uint64_t)TARGET_MS * 1000000ULL / est_ns_per_op;
	}
	if (want < 1) {
		want = 1;
	}
	if (want > MAX_REPS) {
		want = MAX_REPS;
	}
	if (cap && want > cap) {
		want = cap;
	}
	return (uint32_t)want;
}

static void fill_pattern(size_t len, uint8_t seed)
{
	for (size_t i = 0; i < len; i++) {
		g_buf[i] = (uint8_t)((i * 31u + seed) ^ (i >> 8));
	}
}

/* --- phase: erase ------------------------------------------------------- */

/*
 * Erase is the only L0 operation whose cost the layers above can choose to
 * batch: blob_db_prepare() erases block by block, blob_db_erase_all() erases
 * the whole partition in one call. Whether that choice matters is a property
 * of the driver and the part, and this phase measures it two ways over the
 * same blocks:
 *
 *   erase   — one call covering `span` blocks
 *   erase1  — `span` separate one-block calls
 *
 * If the per-block cost is identical the part charges per block and the API
 * shape is free. If the multi-block call is cheaper the driver is issuing a
 * larger erase command (32 KB/64 KB block erase instead of 4 KB sector erase),
 * and any layer that erases one block at a time is leaving that on the table.
 */
static int phase_erase(void)
{
	printk("\n-- erase: cost vs erase size --\n");

	uint32_t span_max = 1;

	while (span_max * 2 <= g.region && span_max * 2 <= 64) {
		span_max *= 2;
	}

	for (uint32_t span = 1; span <= span_max; span *= 2) {
		struct span s;
		uint64_t total = 0, min_ns = UINT64_MAX, max_ns = 0;
		uint32_t calls = 0;

		/* One call per repetition; each is timed on its own so a
		 * multi-second whole-region erase cannot outrun the 32-bit
		 * cycle counter, and so the spread is visible. NOR erase time
		 * varies with wear and with the block's contents, and a model
		 * built from a single sample would inherit that noise. */
		for (uint32_t r = 0; r < ERASE_REPS; r++) {
			span_start(&s);
			int rc = flash_area_erase(g_fa, 0, (size_t)span * g.block);

			if (rc < 0) {
				LOG_ERR("erase span=%u: %d", span, rc);
				return rc;
			}
			uint64_t ns = span_end(&s);

			total += ns;
			min_ns = MIN(min_ns, ns);
			max_ns = MAX(max_ns, ns);
			calls++;
		}
		raw_erase("erase", span, calls, total, min_ns, max_ns);
		printk("  %-16s %6u blk x%-6u  %8llu.%03llu ms/call  "
		       "%8llu.%03llu ms/block\n",
		       "erase", span, calls,
		       (unsigned long long)(total / calls / 1000000),
		       (unsigned long long)(total / calls / 1000 % 1000),
		       (unsigned long long)(total / calls / span / 1000000),
		       (unsigned long long)(total / calls / span / 1000 % 1000));
	}

	/* The same blocks again, one call each: the batching comparison. */
	if (span_max > 1) {
		struct span s;
		uint64_t total = 0, min_ns = UINT64_MAX, max_ns = 0;

		for (uint32_t b = 0; b < span_max; b++) {
			span_start(&s);
			int rc = flash_area_erase(g_fa, (off_t)b * g.block,
						  g.block);

			if (rc < 0) {
				LOG_ERR("erase1 block=%u: %d", b, rc);
				return rc;
			}
			uint64_t ns = span_end(&s);

			total += ns;
			min_ns = MIN(min_ns, ns);
			max_ns = MAX(max_ns, ns);
		}
		raw_erase("erase1", 1, span_max, total, min_ns, max_ns);
		printk("  %-16s %6u blk x%-6u  %8llu.%03llu ms/call  "
		       "(spread %llu.%03llu..%llu.%03llu ms)\n",
		       "erase1", 1, span_max,
		       (unsigned long long)(total / span_max / 1000000),
		       (unsigned long long)(total / span_max / 1000 % 1000),
		       (unsigned long long)(min_ns / 1000000),
		       (unsigned long long)(min_ns / 1000 % 1000),
		       (unsigned long long)(max_ns / 1000000),
		       (unsigned long long)(max_ns / 1000 % 1000));
	}

	return 0;
}

/* --- phase: read -------------------------------------------------------- */

/*
 * Read is swept over programmed data, not over an erased region: a NOR part
 * does not care, but a driver with a cache might, and the workloads being
 * predicted read records, not blank space.
 */
static int phase_read(void)
{
	struct sweep sw;

	/* Program the read region once, untimed. */
	const uint32_t rd_blocks = MIN(g.region, 2u);
	const size_t chunk = MIN(g.max_xfer, g.block);

	fill_pattern(chunk, 0x5a);
	for (uint32_t b = 0; b < rd_blocks; b++) {
		int rc = flash_area_erase(g_fa, (off_t)b * g.block, g.block);

		if (rc < 0) {
			LOG_ERR("read setup erase: %d", rc);
			return rc;
		}
		for (size_t off = 0; off < g.block; off += chunk) {
			/* Truncate the tail rather than assuming the chunk
			 * divides the block: MAX_XFER is a plain integer knob
			 * and nothing forces it to a power of two. */
			const size_t n = MIN(chunk, g.block - off);

			rc = flash_area_write(g_fa, (off_t)b * g.block + off,
					      g_buf, n);
			if (rc < 0) {
				LOG_ERR("read setup write: %d", rc);
				return rc;
			}
		}
	}

	const size_t region_bytes = (size_t)rd_blocks * g.block;

	sweep_begin(&sw, "read", "read: cost vs transfer size");

	/* Reads start at one byte. Unlike a write, a read has no alignment
	 * floor to respect, and the single-byte row is the one that shows the
	 * per-call overhead almost undiluted. */
	for (size_t size = sweep_first(1); size;
	     size = sweep_next(size, 1, 1, g.max_xfer)) {
		struct span s;

		/* Probe one operation to size the batch. */
		span_start(&s);
		int rc = flash_area_read(g_fa, 0, g_buf, size);

		if (rc < 0) {
			LOG_ERR("read probe size=%zu: %d", size, rc);
			return rc;
		}
		uint64_t est = span_end(&s);

		const uint32_t reps = reps_for(est, 0);

		span_start(&s);
		for (uint32_t i = 0; i < reps; i++) {
			/* Walk the region so no single address can be served
			 * from a driver-side cache for the whole batch. */
			off_t off = (off_t)(((uint64_t)i * size) %
					    (region_bytes - size + 1));

			off -= off % (off_t)g.align;
			rc = flash_area_read(g_fa, off, g_buf, size);
			if (rc < 0) {
				LOG_ERR("read size=%zu: %d", size, rc);
				return rc;
			}
		}
		uint64_t total = span_end(&s);

		sweep_row(&sw, size, reps, total);
	}
	sweep_end(&sw);

	return 0;
}

/* --- phase: write ------------------------------------------------------- */

/*
 * Write needs erased space, and erased space is expensive, so each sweep point
 * erases exactly what it is about to consume and nothing more. The erases are
 * outside the timed span.
 *
 * Sequential offsets, not one address rewritten: a NOR page program can only
 * clear bits, so rewriting the same offset would measure a different (and
 * illegal) operation. That does mean a transfer size which is not a divisor of
 * the part's program page can straddle two pages, which is exactly what the
 * `write_unaligned` points below isolate.
 */
static int phase_write(void)
{
	struct sweep sw;

	sweep_begin(&sw, "write", "write: cost vs transfer size");

	for (size_t size = sweep_first(g.align); size;
	     size = sweep_next(size, g.align, g.align, g.max_xfer)) {
		struct span s;
		int rc;

		rc = flash_area_erase(g_fa, 0, g.block);
		if (rc < 0) {
			LOG_ERR("write probe erase: %d", rc);
			return rc;
		}

		fill_pattern(size, (uint8_t)size);

		span_start(&s);
		rc = flash_area_write(g_fa, 0, g_buf, size);
		if (rc < 0) {
			LOG_ERR("write probe size=%zu: %d", size, rc);
			return rc;
		}
		uint64_t est = span_end(&s);

		/* Cap by what the region can hold: one operation must never
		 * land on already-programmed bytes. */
		const uint32_t cap =
			(uint32_t)(((uint64_t)g.region * g.block) / size);
		const uint32_t reps = reps_for(est, cap);
		const uint32_t need_blocks =
			(uint32_t)DIV_ROUND_UP((uint64_t)reps * size, g.block);

		rc = flash_area_erase(g_fa, 0, (size_t)need_blocks * g.block);
		if (rc < 0) {
			LOG_ERR("write erase %u blocks: %d", need_blocks, rc);
			return rc;
		}

		span_start(&s);
		for (uint32_t i = 0; i < reps; i++) {
			rc = flash_area_write(g_fa, (off_t)i * size, g_buf,
					      size);
			if (rc < 0) {
				LOG_ERR("write size=%zu i=%u: %d", size, i, rc);
				return rc;
			}
		}
		uint64_t total = span_end(&s);

		sweep_row(&sw, size, reps, total);
	}
	sweep_end(&sw);

	return 0;
}

/*
 * The same write, with every transfer starting on a program-page boundary.
 *
 * The packed sweep above answers "what does blob_db pay", because blob_db
 * writes back to back. It cannot answer "why", because a packed transfer of n
 * bytes touches ceil(n/page) or ceil(n/page)+1 pages depending on where the
 * previous one ended, and the average of those two is a smooth-looking curve.
 *
 * Pinning each transfer to a page boundary removes that ambiguity: the cost
 * becomes exactly ceil(n / page) page programs, and the table below is a
 * staircase if the part programs by page and a line if it does not. The sizes
 * are chosen to sit either side of the boundary — one byte over a page should
 * cost a whole extra program, and if it does, that is the non-linearity the
 * affine model cannot carry, measured rather than argued.
 */
static int phase_write_pages(void)
{
	const size_t pg = CONFIG_APP_PERF_L0_PROGRAM_PAGE;
	const size_t sizes[] = {
		pg / 4, pg / 2, pg - g.align, pg,
		pg + g.align, pg + pg / 2, 2 * pg,
		2 * pg + g.align, 3 * pg, 4 * pg,
	};
	struct sweep sw;

	if (pg == 0 || pg % g.align != 0 || pg > g.max_xfer) {
		printk("\n(page-program sweep skipped: page %zu does not fit "
		       "align %zu / max transfer %zu)\n",
		       pg, g.align, g.max_xfer);
		return 0;
	}

	sweep_begin(&sw, "write_pg",
		    "write: page-program staircase (each transfer page-aligned)");

	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		const size_t size = sizes[i];
		/* Stride to the next page boundary at or after the transfer,
		 * so every operation starts on one. */
		const size_t stride = ROUND_UP(size, pg);
		struct span s;
		int rc;

		if (size == 0 || size > g.max_xfer || size % g.align != 0) {
			continue;
		}

		const uint32_t cap =
			(uint32_t)(((uint64_t)g.region * g.block) / stride);
		uint32_t reps = MIN(cap, 32u);

		if (reps == 0) {
			continue;
		}

		const uint32_t need_blocks = (uint32_t)DIV_ROUND_UP(
			(uint64_t)reps * stride, g.block);

		rc = flash_area_erase(g_fa, 0, (size_t)need_blocks * g.block);
		if (rc < 0) {
			LOG_ERR("write_pg erase: %d", rc);
			return rc;
		}

		fill_pattern(size, (uint8_t)i);

		span_start(&s);
		for (uint32_t r = 0; r < reps; r++) {
			rc = flash_area_write(g_fa, (off_t)r * stride, g_buf,
					      size);
			if (rc < 0) {
				LOG_ERR("write_pg size=%zu: %d", size, rc);
				return rc;
			}
		}
		uint64_t total = span_end(&s);

		sweep_row(&sw, size, reps, total);
	}
	/* Deliberately no linearity verdict: a staircase is not supposed to
	 * have a constant marginal cost, and calling it "NOT affine" would
	 * report the intent of the phase as a finding. */

	return 0;
}

/*
 * The same transfer size, offset by one write-alignment unit from the start of
 * the region. On a part that programs in pages (256 B on the MX25R64) a
 * page-sized transfer costs one program when aligned and two when it straddles
 * — the single largest source of error in an affine model, and the reason the
 * fit's residuals are worth reading rather than trusting.
 *
 * Skipped when write_align == the transfer size or the part has no page
 * structure to straddle; the tool treats a missing point as "not measured",
 * never as "no penalty".
 */
static int phase_write_unaligned(void)
{
	const size_t pg = CONFIG_APP_PERF_L0_PROGRAM_PAGE;
	const size_t sizes[] = { pg, 2 * pg, 16 * pg };
	struct sweep sw;

	sweep_begin(&sw, "write_unaligned",
		    "write: page-straddle penalty (same size, offset by one "
		    "align unit)");

	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		const size_t size = sizes[i];

		if (size < g.align || size > g.max_xfer) {
			continue;
		}

		struct span s;
		int rc;
		/* One extra alignment unit of head room so the last operation
		 * still fits inside the erased region. */
		const uint32_t cap =
			(uint32_t)((((uint64_t)g.region * g.block) - g.align) /
				   size);
		const uint32_t reps = MIN(cap, 64u);
		const uint32_t need_blocks = (uint32_t)DIV_ROUND_UP(
			(uint64_t)reps * size + g.align, g.block);

		if (reps == 0) {
			continue;
		}

		rc = flash_area_erase(g_fa, 0, (size_t)need_blocks * g.block);
		if (rc < 0) {
			LOG_ERR("unaligned erase: %d", rc);
			return rc;
		}

		fill_pattern(size, 0x3c);

		span_start(&s);
		for (uint32_t r = 0; r < reps; r++) {
			rc = flash_area_write(g_fa,
					      (off_t)g.align + (off_t)r * size,
					      g_buf, size);
			if (rc < 0) {
				LOG_ERR("unaligned write: %d", rc);
				return rc;
			}
		}
		uint64_t total = span_end(&s);

		sweep_row(&sw, size, reps, total);
	}
	/* No verdict here either: three points offset from the grid are a
	 * comparison against the aligned rows, not a curve. */

	return 0;
}

/* --- geometry ----------------------------------------------------------- */

static int geometry(void)
{
	int rc = flash_area_open(PARTITION_ID(storage_partition), &g_fa);

	if (rc < 0) {
		LOG_ERR("flash_area_open: %d", rc);
		return rc;
	}

	struct flash_sector sectors[4];
	uint32_t scount = ARRAY_SIZE(sectors);

	rc = flash_area_sectors(g_fa, &scount, sectors);
	if (rc < 0 && rc != -ENOMEM) {
		LOG_ERR("flash_area_sectors: %d", rc);
		return rc;
	}
	if (scount == 0) {
		LOG_ERR("partition reports zero sectors");
		return -EIO;
	}

	g.block = sectors[0].fs_size;
	for (uint32_t i = 1; i < scount; i++) {
		if (sectors[i].fs_size != g.block) {
			LOG_ERR("non-uniform sectors not supported");
			return -ENOTSUP;
		}
	}

	g.part_bytes = g_fa->fa_size;
	g.blocks = (uint32_t)(g.part_bytes / g.block);
	g.align = flash_area_align(g_fa);
	if (g.align == 0) {
		g.align = 1;
	}
	g.erased_val = flash_area_erased_val(g_fa);
	g.max_xfer = MIN((size_t)MAX_XFER, g.block);
	g.region = MIN((uint32_t)REGION_BLOCKS, g.blocks);

	if (g.region == 0) {
		LOG_ERR("no blocks to work in");
		return -EINVAL;
	}

	/* `source` is the field that keeps a simulator model from being
	 * mistaken for a hardware one; the tool refuses to predict hardware
	 * from a model whose source is not `hardware`. */
	printk("l0geom part_bytes=%zu block_bytes=%zu blocks=%u write_align=%zu "
	       "erased_val=0x%02x region_blocks=%u max_xfer=%zu "
	       "program_page=%u cycles_per_s=%u source=%s timing=%s board=%s\n",
	       g.part_bytes, g.block, g.blocks, g.align, g.erased_val,
	       g.region, g.max_xfer,
	       (unsigned)CONFIG_APP_PERF_L0_PROGRAM_PAGE,
	       (unsigned)sys_clock_hw_cycles_per_sec(),
	       IS_ENABLED(CONFIG_FLASH_SIMULATOR) ? "flash_simulator"
						 : "hardware",
	       IS_ENABLED(CONFIG_FLASH_SIMULATOR)
		       ? (IS_ENABLED(CONFIG_FLASH_SIMULATOR_SIMULATE_TIMING)
				  ? "simulated"
				  : "none")
		       : "real",
	       CONFIG_BOARD_TARGET);

	printk("partition %zu B, %u blocks of %zu B, write align %zu, "
	       "erased 0x%02x\n",
	       g.part_bytes, g.blocks, g.block, g.align, g.erased_val);
	printk("working region: first %u block(s) = %zu B  "
	       "(THIS ERASES THEM)\n",
	       g.region, (size_t)g.region * g.block);

	if (IS_ENABLED(CONFIG_FLASH_SIMULATOR) &&
	    !IS_ENABLED(CONFIG_FLASH_SIMULATOR_SIMULATE_TIMING)) {
		printk("NOTE: flash simulator without SIMULATE_TIMING — these "
		       "numbers are memcpy,\n"
		       "      not storage. The model they produce describes "
		       "the host, not a part.\n");
	}

	return 0;
}

int main(void)
{
	printk("l0 perf %s — raw flash_area timing\n", APP_VERSION_STRING);

	int rc = geometry();

	if (rc < 0) {
		printk("l0end status=%d\n", rc);
		return 0;
	}

	rc = phase_erase();
	if (rc == 0) {
		rc = phase_read();
	}
	if (rc == 0) {
		rc = phase_write();
	}
	if (rc == 0) {
		rc = phase_write_pages();
	}
	if (rc == 0) {
		rc = phase_write_unaligned();
	}

	/* Leave the region erased rather than half-programmed: whatever runs
	 * next on this board meets a clean partition, not a corpse. */
	(void)flash_area_erase(g_fa, 0, (size_t)g.region * g.block);

	printk("l0end status=%d\n", rc);
	printk("\ndone. Feed this capture to tools/l0_timing.py fit.\n");

	flash_area_close(g_fa);

#if defined(CONFIG_ARCH_POSIX)
	/* On a board, main() returning leaves the idle loop spinning and the
	 * operator stops the capture by hand. On native_sim the capture is a
	 * shell pipeline, so exit and let it end on its own. */
	posix_exit(rc < 0 ? 1 : 0);
#endif
	return 0;
}
