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
 *   read  / write   — TRANSFER SIZE, swept in powers of two from the write
 *                     alignment up to one erase block. This is the "block
 *                     size" question: what does a small transfer cost versus
 *                     a large one, and how much of that is per-call overhead
 *                     that a caller pays again on every extra transaction?
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
static void raw_rw(const char *op, size_t size, uint32_t ops, uint64_t total_ns)
{
	printk("l0raw op=%s size=%zu ops=%u total_ns=%llu ns_per_op=%llu\n",
	       op, size, ops, (unsigned long long)total_ns,
	       (unsigned long long)(ops ? total_ns / ops : 0));
}

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

/* Human line: µs per operation and the throughput it implies. Printed next to
 * every raw line so the console is readable without the tool. */
static void human_rw(const char *op, size_t size, uint32_t ops,
		     uint64_t total_ns)
{
	const uint64_t ns_op = ops ? total_ns / ops : 0;
	/* KB/s from ns/op, in integer arithmetic: size * 1e9 / ns / 1024. */
	const uint64_t kbs = ns_op ? (uint64_t)size * 1000000ULL / ns_op * 1000ULL / 1024ULL
				   : 0;

	printk("  %-16s %6zu B  x%-6u  %8llu.%03llu us/op  %8llu KB/s\n",
	       op, size, ops, (unsigned long long)(ns_op / 1000),
	       (unsigned long long)(ns_op % 1000), (unsigned long long)kbs);
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
	printk("\n-- read: cost vs transfer size --\n");

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

	for (size_t size = g.align; size <= g.max_xfer; size *= 2) {
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

		raw_rw("read", size, reps, total);
		human_rw("read", size, reps, total);
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
static int phase_write(void)
{
	printk("\n-- write: cost vs transfer size --\n");

	for (size_t size = g.align; size <= g.max_xfer; size *= 2) {
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

		raw_rw("write", size, reps, total);
		human_rw("write", size, reps, total);
	}

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
	const size_t sizes[] = { 256, 512, 4096 };

	printk("\n-- write: page-straddle penalty --\n");

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

		raw_rw("write_unaligned", size, reps, total);
		human_rw("write_unalign", size, reps, total);
	}

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
	       "cycles_per_s=%u source=%s timing=%s board=%s\n",
	       g.part_bytes, g.block, g.blocks, g.align, g.erased_val,
	       g.region, g.max_xfer, (unsigned)sys_clock_hw_cycles_per_sec(),
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
