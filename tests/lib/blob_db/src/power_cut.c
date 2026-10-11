/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Power cuts during a compaction, injected beneath the store seam.
 *
 * The crash cases in main.c forge a torn state by writing the raw partition at
 * offsets that are blob_db's own, so they only mean something on flash_area
 * (SKIP_UNLESS_RAW_PARTITION). These cut the power instead: the flash
 * simulator's write and erase callbacks let a chosen number of bytes or erases
 * through and then refuse everything, leaving flash exactly as it stood at
 * that instant, whichever backend wrote it. Nothing here knows an offset, so
 * the same cases run on flash_area, on UBI with the scratch-sector protocol
 * and on UBI with CONFIG_BLOB_DB_UBI_ATOMIC_REPLACE, where a cut can land
 * inside ubi_leb_change().
 *
 * Each case builds one bucket whose next update has to compact it, and records
 * the flash writes and erases that update makes. It then replays the update
 * from the same flash image once per cut point, remounts, and checks:
 *
 *   - the mount recovers;
 *   - the updated id holds wholly its old value or wholly its new one, and
 *     the new one whenever the update reported success;
 *   - every other id, in the bucket and outside it, is intact, and a deleted
 *     one stays deleted;
 *   - over the sweep the outcome flips from old to new once and never back;
 *   - the recovered store takes a write that survives a further remount, so
 *     nothing older is stood back up by a second attach.
 *
 * Writes are cut before every byte, or past MAX_WRITE_CUTS bytes at a stride
 * and at both ends of every contiguous run. An erase is cut leaving either
 * half of the block erased: its header gone and its data left, or the reverse.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/flash/flash_simulator.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_inspect.h>

#define PC_PARTITION_ID    PARTITION_ID(storage_partition)
#define PC_PARTITION_SIZE  DT_REG_SIZE(DT_NODELABEL(storage_partition))

#define VAL_LEN       24
#define N_SIBLINGS    4   /* live ids sharing the compacted bucket */
#define N_ELSEWHERE   2   /* live ids in other buckets */
#define MAX_RUNS      512  /* contiguous runs one compaction may write */
#define MAX_WRITE_CUTS 1024 /* write cut points before the sweep strides */

/* Power ------------------------------------------------------------------ */

enum erase_tear {
	TEAR_HEAD_ERASED, /* header gone, data left */
	TEAR_TAIL_ERASED, /* header left, data gone */
};

struct write_run {
	uint32_t start; /* bytes written before it, since tracing began */
	uint32_t len;
};

static struct {
	bool cut;              /* the power is gone */
	bool write_armed;
	uint32_t write_budget; /* bytes let through before the cut */
	bool erase_armed;
	uint32_t erase_budget; /* erases let through before the cut */
	enum erase_tear tear;

	/* Counted since the last pwr_reset(). */
	uint32_t bytes;
	uint32_t erases;
	off_t last_off;
	uint32_t n_runs;
	struct write_run runs[MAX_RUNS];
} pwr;

static const struct device *sim;
static uint8_t *sim_part;  /* the partition inside the simulator's memory */
static size_t erase_unit;
static uint8_t erase_value;
static uint8_t snapshot[PC_PARTITION_SIZE];

static int pwr_write_byte(const struct device *dev, off_t off, uint8_t data)
{
	ARG_UNUSED(dev);

	if (pwr.cut) {
		return -EIO;
	}
	if (pwr.write_armed) {
		if (pwr.write_budget == 0) {
			pwr.cut = true;
			return -EIO;
		}
		pwr.write_budget--;
	}

	/* A flash write reaches here byte by byte at rising offsets. A jump
	 * starts a new run; a write that carries on where the last one ended
	 * does not. */
	if (pwr.bytes == 0 || off != pwr.last_off + 1) {
		zassert_true(pwr.n_runs < MAX_RUNS, "raise MAX_RUNS");
		pwr.runs[pwr.n_runs++] = (struct write_run){ pwr.bytes, 0 };
	}
	pwr.runs[pwr.n_runs - 1].len++;
	pwr.last_off = off;
	pwr.bytes++;

	return data;
}

static int pwr_erase_unit(const struct device *dev, off_t unit_off)
{
	size_t size;
	uint8_t *mem = flash_simulator_get_memory(dev, &size);
	const size_t half = erase_unit / 2;

	if (pwr.cut) {
		return -EIO;
	}
	if (pwr.erase_armed) {
		if (pwr.erase_budget == 0) {
			memset(&mem[unit_off + (pwr.tear == TEAR_HEAD_ERASED
						 ? 0 : half)],
			       erase_value, half);
			pwr.cut = true;
			return -EIO;
		}
		pwr.erase_budget--;
	}

	/* With this callback set the simulator no longer erases by itself. */
	memset(&mem[unit_off], erase_value, erase_unit);
	pwr.erases++;
	return 0;
}

static const struct flash_simulator_cb pwr_cb = {
	.write_byte = pwr_write_byte,
	.erase_unit = pwr_erase_unit,
};

/* Power on, nothing armed, counters zeroed. */
static void pwr_reset(void)
{
	memset(&pwr, 0, sizeof(pwr));
}

static void flash_save(void)
{
	memcpy(snapshot, sim_part, sizeof(snapshot));
}

static void flash_restore(void)
{
	memcpy(sim_part, snapshot, sizeof(snapshot));
}

/* The scenario ----------------------------------------------------------- */

static struct {
	uint16_t bid;
	uint32_t gen;  /* the bucket's generation before the update */
	uint64_t target;
	uint64_t siblings[N_SIBLINGS];
	uint64_t elsewhere[N_ELSEWHERE];
	uint64_t deleted;
	uint8_t old_val[VAL_LEN];
	uint8_t new_val[VAL_LEN];
} sc;

static void val_make(uint8_t *v, const char *tag, int n)
{
	memset(v, 0, VAL_LEN);
	(void)snprintk((char *)v, VAL_LEN, "%s-%04d", tag, n);
}

static void val_bind(uint64_t id, const char *tag, int n)
{
	uint8_t v[VAL_LEN];

	val_make(v, tag, n);
	zassert_ok(blob_db_update(id, v, VAL_LEN), "bind %s-%d", tag, n);
}

static bool val_is(uint64_t id, const uint8_t *want)
{
	uint8_t got[VAL_LEN + 1];
	size_t len = 0;

	return blob_db_get(id, got, sizeof(got), &len) == 0 &&
	       len == VAL_LEN && memcmp(got, want, VAL_LEN) == 0;
}

static void val_expect(uint64_t id, const char *tag, int n)
{
	uint8_t want[VAL_LEN];

	val_make(want, tag, n);
	zassert_true(val_is(id, want), "id %llu lost %s-%04d",
		     (unsigned long long)id, tag, n);
}

static uint32_t bucket_gen(void)
{
	struct blob_db_inspect_bucket bs;

	zassert_ok(blob_db_inspect_bucket_get(sc.bid, &bs, NULL, NULL));
	return bs.gen;
}

/* One bucket holding the target, siblings with superseded slots, a deleted id
 * and nothing left to append into; two ids in other buckets. Saved unmounted,
 * so that every replay starts from the same flash. */
static void scenario_build(void)
{
	struct blob_db_inspect_info in;
	struct blob_db_inspect_bucket bs;

	zassert_ok(blob_db_inspect_info_get(&in));

	sc.target = blob_db_alloc_id();
	sc.bid = (uint16_t)(sc.target % in.n_buckets);
	val_bind(sc.target, "old", 0);

	int n_sib = 0;
	int n_else = 0;

	while (n_sib < N_SIBLINGS + 1) {
		const uint64_t id = blob_db_alloc_id();

		zassert_not_equal(id, 0);
		if (id % in.n_buckets == sc.bid) {
			if (n_sib < N_SIBLINGS) {
				sc.siblings[n_sib] = id;
			} else {
				sc.deleted = id;
			}
			n_sib++;
		} else if (n_else < N_ELSEWHERE) {
			sc.elsewhere[n_else] = id;
			val_bind(id, "else", n_else++);
		}
	}

	/* Each sibling twice, so the compaction has garbage of theirs to drop
	 * and must keep the later slot. */
	for (int i = 0; i < N_SIBLINGS; i++) {
		val_bind(sc.siblings[i], "stale", i);
		val_bind(sc.siblings[i], "sib", i);
	}
	val_bind(sc.deleted, "doomed", 0);
	zassert_ok(blob_db_delete(sc.deleted));

	/* Fill the bucket until the next VAL_LEN update cannot append. */
	const size_t a = in.write_align ? in.write_align : 1;
	const uint32_t ssz = ROUND_UP(in.slot_overhead + VAL_LEN, a);
	int n = 0;

	for (;;) {
		zassert_ok(blob_db_inspect_bucket_get(sc.bid, &bs, NULL, NULL));
		if (bs.free_bytes < ssz) {
			break;
		}
		val_bind(sc.target, "old", ++n);
		zassert_true(n < 1000, "bucket %u never filled", sc.bid);
	}
	sc.gen = bs.gen;
	val_make(sc.old_val, "old", n);
	val_make(sc.new_val, "new", 0);

	zassert_ok(blob_db_unmount());
	flash_save();
}

/* Everything except the target is as scenario_build() left it. */
static void others_intact(void)
{
	for (int i = 0; i < N_SIBLINGS; i++) {
		val_expect(sc.siblings[i], "sib", i);
	}
	for (int i = 0; i < N_ELSEWHERE; i++) {
		val_expect(sc.elsewhere[i], "else", i);
	}

	uint8_t buf[VAL_LEN];
	size_t len;

	zassert_equal(blob_db_get(sc.deleted, buf, sizeof(buf), &len), -ENOENT,
		      "deleted id %llu came back",
		      (unsigned long long)sc.deleted);
	zassert_equal(blob_db_count(), 2 + N_SIBLINGS + N_ELSEWHERE);
}

/* The uncut update: it must compact, and its I/O is what the sweeps cut. */
static void reference_run(void)
{
	flash_restore();
	zassert_ok(blob_db_mount());
	pwr_reset();
	zassert_ok(blob_db_update(sc.target, sc.new_val, VAL_LEN));
	zassert_equal(bucket_gen(), sc.gen + 1,
		      "the update did not compact bucket %u", sc.bid);
	zassert_true(val_is(sc.target, sc.new_val));
	others_intact();
	zassert_ok(blob_db_unmount());
	zassert_true(pwr.bytes > 0 && pwr.n_runs > 0);
}

enum outcome { OUT_OLD, OUT_NEW };

/* Replay the update with the power armed as `arm` says, then recover. */
static enum outcome cut_and_recover(void (*arm)(uint32_t, enum erase_tear),
				    uint32_t at, enum erase_tear tear,
				    const char *what)
{
	flash_restore();
	zassert_ok(blob_db_mount());
	pwr_reset();
	arm(at, tear);

	const int rc = blob_db_update(sc.target, sc.new_val, VAL_LEN);

	zassert_true(pwr.cut, "%s %u: the cut never fired (%d)", what, at, rc);

	zassert_ok(blob_db_unmount());
	pwr_reset();
	zassert_ok(blob_db_mount(), "%s %u: mount after the cut failed",
		   what, at);

	enum outcome out;

	if (val_is(sc.target, sc.new_val)) {
		out = OUT_NEW;
	} else {
		zassert_true(val_is(sc.target, sc.old_val),
			     "%s %u: target is neither old nor new", what, at);
		zassert_true(rc != 0,
			     "%s %u: update reported success, value lost",
			     what, at);
		out = OUT_OLD;
	}
	others_intact();

	/* The recovered store is a working one, and stays what it now is. */
	val_bind(sc.target, "after", 0);
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount(), "%s %u: second remount failed", what, at);
	val_expect(sc.target, "after", 0);
	others_intact();
	zassert_ok(blob_db_unmount());

	return out;
}

static void arm_write(uint32_t at, enum erase_tear tear)
{
	ARG_UNUSED(tear);
	pwr.write_armed = true;
	pwr.write_budget = at;
}

static void arm_erase(uint32_t at, enum erase_tear tear)
{
	pwr.erase_armed = true;
	pwr.erase_budget = at;
	pwr.tear = tear;
}

/* Suite ------------------------------------------------------------------- */

static void *power_cut_setup(void)
{
	const struct flash_area *fa;
	struct flash_pages_info page;
	size_t size;

	zassert_ok(flash_area_open(PC_PARTITION_ID, &fa));
	sim = fa->fa_dev;
	zassert_equal(fa->fa_size, sizeof(snapshot));
	zassert_ok(flash_get_page_info_by_offs(sim, fa->fa_off, &page));
	erase_unit = page.size;
	erase_value = flash_get_parameters(sim)->erase_value;

	uint8_t *mem = flash_simulator_get_memory(sim, &size);

	zassert_not_null(mem);
	zassert_true(fa->fa_off + fa->fa_size <= size);
	sim_part = mem + fa->fa_off;
	flash_area_close(fa);

	pwr_reset();
	flash_simulator_set_callbacks(sim, &pwr_cb);
	return NULL;
}

static void power_cut_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	flash_simulator_set_callbacks(sim, NULL);
}

static void power_cut_before(void *fixture)
{
	ARG_UNUSED(fixture);
	pwr_reset();
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
	memset(&sc, 0, sizeof(sc));
	scenario_build();
	reference_run();
}

static void power_cut_after(void *fixture)
{
	ARG_UNUSED(fixture);
	pwr_reset();
	blob_db_unmount();
}

ZTEST_SUITE(blob_db_power_cut, NULL, power_cut_setup, power_cut_before,
	    power_cut_after, power_cut_teardown);

/* Is byte k the first or last of a contiguous run of the reference trace? */
static bool run_edge(const struct write_run *runs, uint32_t n_runs, uint32_t k)
{
	for (uint32_t r = 0; r < n_runs; r++) {
		if (k == runs[r].start || k == runs[r].start + runs[r].len - 1) {
			return true;
		}
	}
	return false;
}

/* A cut before every byte the compacting update writes. Byte by byte, because
 * the trace cannot see where one flash write ends and the next begins when the
 * second carries on at the next address, as an append after a freshly
 * replaced image does. Past MAX_WRITE_CUTS bytes it strides, but still cuts at
 * both ends of every contiguous run. */
ZTEST(blob_db_power_cut, test_cut_before_each_byte)
{
	/* The sweep replaces pwr, so keep the reference trace. */
	static struct write_run runs[MAX_RUNS];
	const uint32_t n_runs = pwr.n_runs;
	const uint32_t total = pwr.bytes;
	const uint32_t step = DIV_ROUND_UP(total, MAX_WRITE_CUTS);

	memcpy(runs, pwr.runs, n_runs * sizeof(runs[0]));

	enum outcome prev = OUT_OLD;
	uint32_t n_cuts = 0;
	uint32_t n_new = 0;

	for (uint32_t k = 0; k < total; k++) {
		if (k % step != 0 && !run_edge(runs, n_runs, k)) {
			continue;
		}

		const enum outcome out = cut_and_recover(arm_write, k, 0,
							 "write byte");

		zassert_false(prev == OUT_NEW && out == OUT_OLD,
			      "write byte %u: the update was undone after a "
			      "cut at an earlier byte had kept it", k);
		zassert_false(k == 0 && out == OUT_NEW,
			      "nothing written, yet the update landed");
		prev = out;
		n_cuts++;
		n_new += (out == OUT_NEW);
	}

	TC_PRINT("%u B in %u runs: %u cuts, %u kept the update\n", total,
		 n_runs, n_cuts, n_new);
}

static void erase_sweep(enum erase_tear tear, const char *what)
{
	const uint32_t n_erases = pwr.erases;
	enum outcome prev = OUT_OLD;
	uint32_t n_new = 0;

	/* UBI with atomic replace writes the new copy into a block erased
	 * beforehand and only queues the old one for reclaim. */
	if (n_erases == 0) {
		TC_PRINT("the compacting update erased nothing\n");
		ztest_test_skip();
	}

	for (uint32_t e = 0; e < n_erases; e++) {
		const enum outcome out = cut_and_recover(arm_erase, e, tear,
							 what);

		zassert_false(prev == OUT_NEW && out == OUT_OLD,
			      "%s %u: the update was undone after a cut at an "
			      "earlier erase had kept it", what, e);
		prev = out;
		n_new += (out == OUT_NEW);
	}

	TC_PRINT("%u erases cut (%s): %u kept the update\n", n_erases, what,
		 n_new);
}

ZTEST(blob_db_power_cut, test_cut_during_each_erase_head_erased)
{
	erase_sweep(TEAR_HEAD_ERASED, "erase, head erased,");
}

ZTEST(blob_db_power_cut, test_cut_during_each_erase_tail_erased)
{
	erase_sweep(TEAR_TAIL_ERASED, "erase, tail erased,");
}
