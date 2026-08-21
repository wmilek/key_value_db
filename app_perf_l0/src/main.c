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
 *                     for writes) up to one erase block, sampled
 *                     CONFIG_APP_PERF_L0_STEPS_PER_OCTAVE times per doubling
 *                     and each point timed CONFIG_APP_PERF_L0_PASSES times.
 *                     Each sweep prints a
 *                     matrix — size, ops, us/op, KiB/s, ns/B, and the
 *                     MARGINAL ns/B against the row above — because that last
 *                     column is what answers "can one slope describe this":
 *                     it is constant iff the cost is affine, whatever the
 *                     fixed cost happens to be. Note that affine is not
 *                     proportional: a non-zero fixed cost is charged on every
 *                     call whatever it carries, so throughput still depends
 *                     on transfer size, which is what the KiB/s column is
 *                     there to show.
 *   read_odd        — consecutive integer sizes either side of several
 *                     anchors, plus the same read from shifted start offsets.
 *                     A geometric ladder only ever samples multiples of the
 *                     alignment above a few hundred bytes, which would hide a
 *                     driver that splits a transfer whose length or address
 *                     is not a whole number of controller words.
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
#define PASSES        CONFIG_APP_PERF_L0_PASSES

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
 * near-identical numbers IS the check that one slope suffices, and a column
 * that steps is a cost that a single slope cannot describe.
 *
 * It is NOT a claim that speed is size-independent. a is paid on every call
 * however few bytes it carries, so throughput rises with transfer size and
 * only flattens well past a/b — measured on the MX25R64, 12.9 B for write and
 * 254 B for read. The KiB/s column is what says that; the marginal column
 * says only that each extra byte costs the same as the last.
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
	uint64_t prev_noise;   /* pass-to-pass spread of the previous point */
	bool     have_prev;
	/* Marginal cost range across the sweep, in centi-ns per byte, so the
	 * spread can be reported without floating point. */
	int64_t  marg_min;
	int64_t  marg_max;
	uint32_t n_used;       /* steps that cleared the significance gate */
	uint32_t n_skipped;    /* steps too noisy to say anything */
	bool     have_marg;
	/* Endpoints, for the slope across the whole sweep. Its denominator is
	 * the full size range rather than one step, so it survives an outlier
	 * that would wreck any single first difference — which is what makes
	 * it the headline rather than the min/max of the step column. */
	size_t   first_size;
	uint64_t first_ns;
	uint64_t first_noise;
	size_t   last_size;
	uint64_t last_ns;
	uint64_t last_noise;
	bool     have_first;
};

static void sweep_begin(struct sweep *s, const char *op, const char *title)
{
	memset(s, 0, sizeof(*s));
	s->op = op;
	printk("\n-- %s --\n", title);
	printk("     size      ops         us/op        KiB/s       ns/B   "
	       "marginal ns/B   spread\n");
}

/* Start a new group inside the same table: the next row prints no marginal.
 * Used where consecutive rows are not consecutive sizes, so a difference
 * across the jump would be arithmetic without meaning. */
static void sweep_gap(struct sweep *s)
{
	s->have_prev = false;
}

static void sweep_row(struct sweep *s, size_t size, uint32_t ops,
		      uint64_t total_ns, uint64_t pmin, uint64_t pmax)
{
	const uint64_t ns_op = ops ? total_ns / ops : 0;

	/* ns_per_op is the mean over every pass, which is what a cost model
	 * wants. min/max are the per-pass extremes: a point whose passes
	 * disagree is a point the model should not be trusted at, and without
	 * them a single number gives no way to tell a stable measurement from
	 * a lucky one. */
	printk("l0raw op=%s size=%zu ops=%u total_ns=%llu ns_per_op=%llu "
	       "min_ns=%llu max_ns=%llu\n",
	       s->op, size, ops, (unsigned long long)total_ns,
	       (unsigned long long)ns_op, (unsigned long long)pmin,
	       (unsigned long long)pmax);

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

		/*
		 * A first difference between two adjacent points has no
		 * defence against one bad batch: at small transfer sizes the
		 * whole signal is smaller than a single outlier, so one
		 * disturbed measurement produces a marginal of thousands of
		 * ns/B, or a negative one, and a verdict built on the extremes
		 * of this column then calls an affine part non-affine. That is
		 * a false negative this check actually produced on a DK
		 * capture, on a part whose fit is clean to +-1.4 %.
		 *
		 * The gate: a step only informs the verdict when the change it
		 * measures is bigger than the noise of the two points that
		 * formed it. `noise` is the pass-to-pass spread each point
		 * reports for itself, so this is a significance test against
		 * measured uncertainty rather than a magic threshold. Steps
		 * that fail it are still printed -- they are data -- but
		 * marked `?` and left out of the range.
		 */
		const uint64_t noise = s->prev_noise + (pmax - pmin);
		const bool significant = d_ns > (int64_t)noise;

		printk("  %8lld.%02llu%s", (long long)(cmarg / 100),
		       (unsigned long long)(llabs(cmarg) % 100),
		       significant ? " " : "?");

		if (!significant) {
			s->n_skipped++;
		} else {
			s->n_used++;
			if (!s->have_marg) {
				s->marg_min = s->marg_max = cmarg;
				s->have_marg = true;
			} else {
				s->marg_min = MIN(s->marg_min, cmarg);
				s->marg_max = MAX(s->marg_max, cmarg);
			}
		}
	} else {
		printk("          -");
	}

	/* Pass-to-pass spread as a percentage of the mean. */
	if (pmax > pmin && ns_op) {
		printk("  %llu.%01llu%%\n",
		       (unsigned long long)((pmax - pmin) * 100 / ns_op),
		       (unsigned long long)((pmax - pmin) * 1000 / ns_op % 10));
	} else {
		printk("     -\n");
	}

	if (!s->have_first) {
		s->first_size = size;
		s->first_ns = ns_op;
		s->first_noise = pmax - pmin;
		s->have_first = true;
	}
	s->last_size = size;
	s->last_ns = ns_op;
	s->last_noise = pmax - pmin;

	s->prev_size = size;
	s->prev_ns = ns_op;
	s->prev_noise = pmax - pmin;
	s->have_prev = true;
}

/*
 * The verdict the matrix supports, stated rather than left to the reader.
 *
 * Two statistics, deliberately not one:
 *
 *   overall — the slope across the whole sweep, (t_last - t_first) /
 *             (n_last - n_first). Its denominator is the entire size range, so
 *             a single disturbed batch moves it by a rounding error. This is
 *             the robust answer to "does cost grow with size at all".
 *   step range — the min and max of the adjacent-step column, over the steps
 *             that rose above their own noise. This is the fine structure, and
 *             the only thing that can catch a staircase, but it is fragile by
 *             construction.
 *
 * Reporting the fragile one alone is what produced a false "NOT affine" on a
 * DK capture whose fit was clean to +-1.4 %.
 */
static void sweep_end(struct sweep *s)
{
	if (!s->have_first || s->last_size <= s->first_size) {
		return;
	}

	const int64_t d_ns = (int64_t)s->last_ns - (int64_t)s->first_ns;
	const int64_t d_b = (int64_t)s->last_size - (int64_t)s->first_size;
	const int64_t overall = d_ns * 100 / d_b;
	const uint64_t end_noise = s->first_noise + s->last_noise;

	printk("l0lin op=%s overall_cmarg=%lld cmarg_min=%lld cmarg_max=%lld "
	       "used=%u skipped=%u\n", s->op, (long long)overall,
	       (long long)(s->have_marg ? s->marg_min : 0),
	       (long long)(s->have_marg ? s->marg_max : 0),
	       s->n_used, s->n_skipped);

	printk("  cost per byte across the whole sweep: %lld.%02llu ns/B\n",
	       (long long)(overall / 100),
	       (unsigned long long)(llabs(overall) % 100));

	if (d_ns <= (int64_t)end_noise) {
		printk("  -> cost does not grow with size: over a %zu -> %zu B "
		       "range the total change\n     is within the "
		       "measurement's own noise. This substrate charges per "
		       "call.\n", s->first_size, s->last_size);
		return;
	}

	if (s->n_used < 3) {
		printk("  -> cost does grow with size, but only %u step(s) "
		       "rose above their own noise,\n     so the sweep cannot "
		       "say whether the growth is even. Read the fit's "
		       "residuals.\n", s->n_used);
		return;
	}

	printk("  step-to-step marginal %lld.%02llu .. %lld.%02llu ns/B",
	       (long long)(s->marg_min / 100),
	       (unsigned long long)(llabs(s->marg_min) % 100),
	       (long long)(s->marg_max / 100),
	       (unsigned long long)(llabs(s->marg_max) % 100));

	if (s->marg_min <= 0) {
		printk("\n  -> flat over part of the sweep and clearly rising "
		       "elsewhere, on steps that\n     cleared the noise "
		       "gate. A cost that is constant across a range and then "
		       "steps\n     looks exactly like this; the page-program "
		       "table says whether that is it.\n");
		return;
	}

	const uint64_t spread =
		(uint64_t)s->marg_max * 100 / (uint64_t)s->marg_min;

	printk("  (%llu.%02llux spread over %u of %u steps; %u too noisy to "
	       "count)\n", (unsigned long long)(spread / 100),
	       (unsigned long long)(spread % 100), s->n_used,
	       s->n_used + s->n_skipped, s->n_skipped);
	if (spread <= 125) {
		printk("  -> affine: one fixed cost plus one cost per byte "
		       "describes this sweep.\n");
	} else {
		printk("  -> NOT affine: the per-byte cost varies %llu.%02llux "
		       "across the steps that\n     cleared the gate, so a "
		       "single slope misprices some sizes. The fit's "
		       "residuals\n     say by how much.\n",
		       (unsigned long long)(spread / 100),
		       (unsigned long long)(spread % 100));
	}
}

/*
 * The size ladder: a geometric sweep with STEPS_PER_OCTAVE linear
 * subdivisions inside each doubling.
 *
 * Powers of two alone are exactly the wrong sample points for a cost that
 * steps at a power-of-two boundary — every sample lands on a step, the
 * staircase reads as a line through its corners, and the sweep concludes
 * "affine" about a function that is not. Subdividing each octave breaks that,
 * and how finely is the knob: 1 gives bare powers of two, 2 adds the 1.5x
 * midpoints, 4 gives 1x 1.25x 1.5x 1.75x, 8 halves the gaps again.
 *
 * Resolution is the whole reason to spend time here. A step located between
 * two samples is only known to lie somewhere in that interval, so at 1 step
 * per octave a feature is placed within a factor of 2 and at 8 within 9 %.
 * The DK's read path turned out to have exactly such a feature — the measured
 * cost across 64->128 B rose at 432 ns/B against ~253 ns/B either side — and
 * two samples an octave apart cannot say whether that is a step, a knee, or a
 * slope change.
 *
 * The ladder is built once into a static table rather than generated by a
 * next() function, because the increments differ per octave: below
 * `align * STEPS` the linear subdivision would ask for a sub-alignment stride,
 * so those octaves simply carry fewer points.
 */
#define STEPS_PER_OCTAVE CONFIG_APP_PERF_L0_STEPS_PER_OCTAVE

/* 17 octaves (1 B .. 64 KiB) at 8 subdivisions, plus slack for the exact
 * endpoint. */
static uint32_t g_sizes[17 * 8 + 4];
static uint32_t g_nsizes;

static void sizes_build(size_t base, size_t align, size_t max)
{
	g_nsizes = 0;

	for (size_t oct = base; oct <= max; oct *= 2) {
		size_t inc = oct / STEPS_PER_OCTAVE;

		if (inc < align) {
			inc = align;
		}
		for (size_t v = oct; v < oct * 2 && v <= max; v += inc) {
			if (v % align != 0) {
				continue;
			}
			if (g_nsizes > 0 && g_sizes[g_nsizes - 1] >= v) {
				continue;
			}
			if (g_nsizes >= ARRAY_SIZE(g_sizes)) {
				return;
			}
			g_sizes[g_nsizes++] = (uint32_t)v;
		}
	}

	/* The largest transfer is the one the model extrapolates from, so it
	 * is worth having exactly rather than whatever the ladder last
	 * landed on. */
	if (max % align == 0 && g_nsizes > 0 && g_sizes[g_nsizes - 1] != max &&
	    g_nsizes < ARRAY_SIZE(g_sizes)) {
		g_sizes[g_nsizes++] = (uint32_t)max;
	}
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

	/* The same blocks again, one call each: the batching comparison, and
	 * the only place the run sees a POPULATION of identical operations.
	 * Erase time on NOR is not a constant — it varies with the block, with
	 * its contents and with wear — so every sample is emitted individually
	 * as well as summarised. A model carries one number for erase; this is
	 * the spread that number is standing in for. */
	if (span_max > 1) {
		struct span s;
		uint64_t total = 0, min_ns = UINT64_MAX, max_ns = 0;
		static uint32_t samples[64];
		uint32_t n = MIN(span_max, (uint32_t)ARRAY_SIZE(samples));

		for (uint32_t b = 0; b < n; b++) {
			span_start(&s);
			int rc = flash_area_erase(g_fa, (off_t)b * g.block,
						  g.block);

			if (rc < 0) {
				LOG_ERR("erase1 block=%u: %d", b, rc);
				return rc;
			}
			uint64_t ns = span_end(&s);

			samples[b] = (uint32_t)MIN(ns, (uint64_t)UINT32_MAX);
			printk("l0raw op=erase1_sample block=%u ns=%llu\n", b,
			       (unsigned long long)ns);
			total += ns;
			min_ns = MIN(min_ns, ns);
			max_ns = MAX(max_ns, ns);
		}

		/* Median by insertion sort of a copy: n is at most 64 and this
		 * runs once, after several minutes of erasing. */
		for (uint32_t i = 1; i < n; i++) {
			uint32_t v = samples[i];
			uint32_t j = i;

			while (j > 0 && samples[j - 1] > v) {
				samples[j] = samples[j - 1];
				j--;
			}
			samples[j] = v;
		}
		const uint32_t med = n ? samples[n / 2] : 0;

		printk("l0raw op=erase1_dist n=%u min_ns=%llu med_ns=%u "
		       "max_ns=%llu\n", n, (unsigned long long)min_ns, med,
		       (unsigned long long)max_ns);
		printk("  %-16s %6u blk x%-6u  median %llu.%03llu ms  "
		       "spread %llu.%03llu..%llu.%03llu ms (%llu.%02llux)\n",
		       "erase1 dist", 1, n,
		       (unsigned long long)(med / 1000000),
		       (unsigned long long)(med / 1000 % 1000),
		       (unsigned long long)(min_ns / 1000000),
		       (unsigned long long)(min_ns / 1000 % 1000),
		       (unsigned long long)(max_ns / 1000000),
		       (unsigned long long)(max_ns / 1000 % 1000),
		       (unsigned long long)(min_ns ? max_ns * 100 / min_ns / 100 : 0),
		       (unsigned long long)(min_ns ? max_ns * 100 / min_ns % 100 : 0));

		span_max = n;
		raw_erase("erase1", 1, span_max, total, min_ns, max_ns);

		/* Erasing a block that is already erased. A part that checks
		 * before erasing would short-circuit; NOR generally does not,
		 * and blob_db_prepare() re-erases blocks often enough that the
		 * answer is worth having rather than assuming. */
		span_start(&s);
		int rc = flash_area_erase(g_fa, 0, g.block);

		if (rc < 0) {
			LOG_ERR("erase-already-erased: %d", rc);
			return rc;
		}
		uint64_t again = span_end(&s);

		printk("l0raw op=erase_erased blocks=1 ops=1 total_ns=%llu "
		       "ns_per_op=%llu\n", (unsigned long long)again,
		       (unsigned long long)again);
		printk("  %-16s %6u blk x%-6u  %8llu.%03llu ms  "
		       "(%llu%% of a median first erase)\n",
		       "erase (erased)", 1, 1,
		       (unsigned long long)(again / 1000000),
		       (unsigned long long)(again / 1000 % 1000),
		       (unsigned long long)(med ? again * 100 / med : 0));
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
	sizes_build(1, 1, g.max_xfer);

	for (uint32_t si = 0; si < g_nsizes; si++) {
		const size_t size = g_sizes[si];
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
		uint64_t total = 0, pmin = UINT64_MAX, pmax = 0;
		uint32_t ops = 0;

		for (uint32_t pass = 0; pass < PASSES; pass++) {
			span_start(&s);
			for (uint32_t i = 0; i < reps; i++) {
				/* Walk the region so no single address can be
				 * served from a driver-side cache for the
				 * whole batch. */
				off_t off = (off_t)(((uint64_t)i * size) %
						    (region_bytes - size + 1));

				off -= off % (off_t)g.align;
				rc = flash_area_read(g_fa, off, g_buf, size);
				if (rc < 0) {
					LOG_ERR("read size=%zu: %d", size, rc);
					return rc;
				}
			}
			uint64_t t = span_end(&s);

			total += t;
			ops += reps;
			pmin = MIN(pmin, t / reps);
			pmax = MAX(pmax, t / reps);
		}

		sweep_row(&sw, size, ops, total, pmin, pmax);
	}
	sweep_end(&sw);

	return 0;
}

/*
 * Read cost either side of a word boundary — the driver-splitting probe.
 *
 * The size ladder is geometric, so above a few hundred bytes every sample it
 * takes is a multiple of the write alignment. That hides a cost this stack can
 * actually pay: a transfer whose LENGTH is not a whole number of the
 * controller's words, or whose OFFSET is not word-aligned, may be split into
 * two commands, or routed through a bounce buffer, or issued as a DMA plus a
 * tail — none of which the geometric sweep would ever sample.
 *
 * So this phase samples consecutive integers around a few anchors. Because the
 * sizes differ by one byte, the marginal column stops being a slope estimate
 * and becomes a direct readout: the cost of the 257th byte. If that byte costs
 * microseconds, the driver did something structural at 256, and the model's
 * single slope is averaging over a discontinuity rather than measuring one.
 *
 * Reads only. A write must be a multiple of flash_area_align(), so on a part
 * with align > 1 the odd lengths are not merely slow but rejected; the
 * page-program phase covers the equivalent question for writes at the
 * granularity writes are allowed to use.
 */
static int phase_read_odd(void)
{
	const size_t anchors[] = { 16, 64, 256, 512, 1024, 4096, 16384 };
	const uint32_t rd_blocks = MIN(g.region, 2u);
	const size_t region_bytes = (size_t)rd_blocks * g.block;
	struct sweep sw;

	sweep_begin(&sw, "read_odd",
		    "read: cost either side of a word boundary (does the "
		    "driver split?)");

	for (size_t ai = 0; ai < ARRAY_SIZE(anchors); ai++) {
		const size_t a = anchors[ai];

		if (a + 4 > g.max_xfer || a + 8 > region_bytes) {
			continue;
		}
		sweep_gap(&sw);

		for (int d = -2; d <= 4; d++) {
			const size_t size = a + (size_t)d;
			struct span s;

			span_start(&s);
			int rc = flash_area_read(g_fa, 0, g_buf, size);

			if (rc < 0) {
				LOG_ERR("read_odd probe %zu: %d", size, rc);
				return rc;
			}
			const uint32_t reps = reps_for(span_end(&s), 0);

			span_start(&s);
			for (uint32_t i = 0; i < reps; i++) {
				off_t off = (off_t)(((uint64_t)i * size) %
						    (region_bytes - size + 1));

				off -= off % (off_t)g.align;
				rc = flash_area_read(g_fa, off, g_buf, size);
				if (rc < 0) {
					LOG_ERR("read_odd %zu: %d", size, rc);
					return rc;
				}
			}
			uint64_t t = span_end(&s);

			sweep_row(&sw, size, reps, t, t / reps, t / reps);
		}
	}

	/*
	 * The same question asked of the OFFSET rather than the length: one
	 * fixed size, read from offset 0 and then from offsets 1..3. A
	 * controller that requires word-aligned source addresses pays for
	 * these, and nothing else in the sweep would notice.
	 */
	printk("\n-- read: cost vs offset alignment (same size, shifted "
	       "start) --\n");
	printk("   offset      ops         us/op\n");

	const size_t size = MIN((size_t)256, g.max_xfer);

	for (off_t base_off = 0; base_off <= 3; base_off++) {
		struct span s;

		span_start(&s);
		int rc = flash_area_read(g_fa, base_off, g_buf, size);

		if (rc < 0) {
			/* An unaligned offset may simply be refused; that is
			 * an answer, not a failure of the run. */
			printk("   %6lld  %18s (rc=%d)\n",
			       (long long)base_off, "refused", rc);
			continue;
		}
		const uint32_t reps = reps_for(span_end(&s), 0);

		span_start(&s);
		for (uint32_t i = 0; i < reps; i++) {
			rc = flash_area_read(g_fa, base_off, g_buf, size);
			if (rc < 0) {
				break;
			}
		}
		uint64_t total = span_end(&s);
		uint64_t ns_op = reps ? total / reps : 0;

		printk("l0raw op=read_offset size=%zu offset=%lld ops=%u "
		       "total_ns=%llu ns_per_op=%llu\n",
		       size, (long long)base_off, reps,
		       (unsigned long long)total, (unsigned long long)ns_op);
		printk("   %6lld  %7u  %8llu.%03llu us\n",
		       (long long)base_off, reps,
		       (unsigned long long)(ns_op / 1000),
		       (unsigned long long)(ns_op % 1000));
	}

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
/*
 * A write cursor over a pre-erased region, rather than an erase per sweep
 * point.
 *
 * Erased space is the expensive thing here — a 64 KB block erase is ~1.1 s
 * against a ~200 ms measurement — so erasing per point made the sweep's cost
 * scale with the NUMBER OF POINTS, and every extra sample size cost 2.2 s of
 * erase to buy 0.2 s of measurement. That is backwards for a benchmark whose
 * job is resolution.
 *
 * With a cursor the cost scales with BYTES CONSUMED instead: points are laid
 * end to end through the region and the whole region is erased only when it
 * fills. Sampling eight sizes per octave instead of one now costs almost
 * nothing, which is what makes the density affordable. The erases stay outside
 * every timed span either way.
 */
static off_t g_wr_cursor;

/* Reserve `need` erased bytes, erasing the whole region if they do not fit.
 * Returns the offset to write at, or -1 if the region cannot hold `need` at
 * all. */
static off_t wr_reserve(size_t need)
{
	const size_t region_bytes = (size_t)g.region * g.block;

	if (need > region_bytes) {
		return -1;
	}
	if ((size_t)g_wr_cursor + need > region_bytes) {
		int rc = flash_area_erase(g_fa, 0, region_bytes);

		if (rc < 0) {
			LOG_ERR("write region erase: %d", rc);
			return -1;
		}
		g_wr_cursor = 0;
	}

	const off_t off = g_wr_cursor;

	g_wr_cursor += (off_t)need;
	/* Keep the cursor on a write-alignment boundary; a transfer that is
	 * not a multiple of the alignment would otherwise leave it stranded
	 * and the next write would be rejected. */
	if (g_wr_cursor % (off_t)g.align) {
		g_wr_cursor += (off_t)g.align - g_wr_cursor % (off_t)g.align;
	}
	return off;
}

static int phase_write(void)
{
	struct sweep sw;

	sweep_begin(&sw, "write", "write: cost vs transfer size");

	sizes_build(g.align, g.align, g.max_xfer);
	g_wr_cursor = 0;
	if (wr_reserve(0) < 0) {
		return -EINVAL;
	}
	/* Start from a known-erased region. */
	int rc = flash_area_erase(g_fa, 0, (size_t)g.region * g.block);

	if (rc < 0) {
		LOG_ERR("write region erase: %d", rc);
		return rc;
	}
	g_wr_cursor = 0;

	for (uint32_t si = 0; si < g_nsizes; si++) {
		const size_t size = g_sizes[si];
		struct span s;
		off_t off;

		/* Probe one operation to size the batch. */
		off = wr_reserve(size);
		if (off < 0) {
			continue;
		}
		fill_pattern(size, (uint8_t)size);

		span_start(&s);
		rc = flash_area_write(g_fa, off, g_buf, size);
		if (rc < 0) {
			LOG_ERR("write probe size=%zu: %d", size, rc);
			return rc;
		}
		uint64_t est = span_end(&s);

		const uint32_t cap =
			(uint32_t)(((uint64_t)g.region * g.block) / size);
		const uint32_t reps = reps_for(est, cap);
		uint64_t total = 0, pmin = UINT64_MAX, pmax = 0;
		uint32_t ops = 0;

		for (uint32_t pass = 0; pass < PASSES; pass++) {
			off = wr_reserve((size_t)reps * size);
			if (off < 0) {
				break;
			}

			span_start(&s);
			for (uint32_t i = 0; i < reps; i++) {
				rc = flash_area_write(g_fa,
						      off + (off_t)i * size,
						      g_buf, size);
				if (rc < 0) {
					LOG_ERR("write size=%zu i=%u: %d",
						size, i, rc);
					return rc;
				}
			}
			uint64_t t = span_end(&s);

			total += t;
			ops += reps;
			pmin = MIN(pmin, t / reps);
			pmax = MAX(pmax, t / reps);
		}

		if (ops) {
			sweep_row(&sw, size, ops, total, pmin, pmax);
		}
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

		sweep_row(&sw, size, reps, total, total / reps, total / reps);
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

		sweep_row(&sw, size, reps, total, total / reps, total / reps);
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
		rc = phase_read_odd();
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
