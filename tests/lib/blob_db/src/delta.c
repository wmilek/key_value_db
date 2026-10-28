/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Delta slots (CONFIG_BLOB_DB_DELTA): an update that changes one contiguous
 * part of a value may be stored as the change against an earlier slot of the
 * same id, and every read rebuilds the value.
 *
 * blob_db_delta checks values, not mechanics: a random sequence of edits on a
 * handful of ids sharing buckets, compared after every write with a model and
 * across remounts, long enough for every bucket involved to compact. A few
 * cases then pin the mechanics the random run relies on.
 *
 * blob_db_delta_power cuts the power at every byte (and every erase) of an
 * update that appends a delta on top of a chain, and of one that compacts a
 * bucket holding chains, then checks the value the id reads back is wholly the
 * old one or wholly the new one, and that its neighbours are untouched.
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
#if defined(CONFIG_BLOB_DB_IOSTATS)
#include <app/lib/blob_db_iostats.h>
#endif

#define D_PARTITION_ID   PARTITION_ID(storage_partition)
#define D_PARTITION_SIZE DT_REG_SIZE(DT_NODELABEL(storage_partition))
#define MAXV             400   /* largest value the model builds */

/* Deterministic PRNG, so a failure replays. */
static uint32_t rng = 0x2545f491;

static uint32_t rnd(uint32_t n)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return n ? rng % n : 0;
}

static void fill(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		p[i] = (uint8_t)rnd(256);
	}
}

static void expect_value(uint64_t id, const uint8_t *want, size_t len, const char *when)
{
	static uint8_t got[MAXV + 8];
	size_t got_len = 0;

	zassert_ok(blob_db_get(id, got, sizeof(got), &got_len), "%s: get id %llu", when,
		   (unsigned long long)id);
	zassert_equal(got_len, len, "%s: id %llu length %zu, want %zu", when,
		      (unsigned long long)id, got_len, len);
	zassert_mem_equal(got, want, len, "%s: id %llu content", when, (unsigned long long)id);

	size_t sz = 0;

	zassert_ok(blob_db_size(id, &sz));
	zassert_equal(sz, len, "%s: size of id %llu", when, (unsigned long long)id);
}

/* One edit of the kind the recorded traffic makes, or a harder one. */
static void edit(uint8_t *v, size_t *len)
{
	size_t n = *len;
	uint32_t at = rnd((uint32_t)n + 1);
	uint32_t k;

	switch (rnd(8)) {
	case 0: /* overwrite a few bytes (counters, timestamps) */
		k = 1 + rnd(4);
		for (uint32_t i = 0; i < k && at + i < n; i++) {
			v[at + i] ^= (uint8_t)(1 + rnd(255));
		}
		break;
	case 1: /* append an entry (kvhash insert) */
		k = 1 + rnd(24);
		if (n + k <= MAXV) {
			fill(v + n, k);
			n += k;
		}
		break;
	case 2: /* insert in the middle */
		k = 1 + rnd(24);
		if (n + k <= MAXV) {
			memmove(v + at + k, v + at, n - at);
			fill(v + at, k);
			n += k;
		}
		break;
	case 3: /* remove a range */
		k = 1 + rnd(24);
		if (at + k <= n) {
			memmove(v + at, v + at + k, n - at - k);
			n -= k;
		}
		break;
	case 4: /* move a range to the end (a kvhash replace) */
		k = 1 + rnd(24);
		if (at + k <= n) {
			uint8_t tmp[24];

			memcpy(tmp, v + at, k);
			memmove(v + at, v + at + k, n - at - k);
			memcpy(v + n - k, tmp, k);
			v[n - 1] ^= 0x5a;
		}
		break;
	case 5: /* replace everything */
		n = 1 + rnd(MAXV);
		fill(v, n);
		break;
	case 6: /* truncate */
		if (n > 1) {
			n = 1 + rnd((uint32_t)n - 1);
		}
		break;
	default: /* unchanged */
		break;
	}
	*len = n;
}

/* blob_db_delta ----------------------------------------------------------- */

static void delta_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
}

static void delta_after(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
}

ZTEST_SUITE(blob_db_delta, NULL, NULL, delta_before, delta_after, NULL);

#define N_IDS   12
#define N_STEPS 6000

ZTEST(blob_db_delta, test_random_edits_match_a_model)
{
	static uint8_t val[N_IDS][MAXV];
	static size_t len[N_IDS];
	uint64_t id[N_IDS];
	struct blob_db_inspect_info in;
	uint32_t gen0[N_IDS];

	zassert_ok(blob_db_inspect_info_get(&in));
	for (int i = 0; i < N_IDS; i++) {
		len[i] = 16 + rnd(200);
		fill(val[i], len[i]);
		id[i] = blob_db_alloc_id();
		zassert_ok(blob_db_update(id[i], val[i], len[i]));

		struct blob_db_inspect_bucket bs;

		zassert_ok(blob_db_inspect_bucket_get(id[i] % in.n_buckets, &bs, NULL, NULL));
		gen0[i] = bs.gen;
	}
#if defined(CONFIG_BLOB_DB_IOSTATS)
	blob_db_iostats_reset();
#endif
	for (int step = 0; step < N_STEPS; step++) {
		const int i = (int)rnd(N_IDS);

		if (rnd(64) == 0) {
			/* Deleted and re-created: a chain must not outlive its id. */
			zassert_ok(blob_db_delete(id[i]));
			zassert_equal(blob_db_get(id[i], NULL, 0, NULL), -ENOENT);
			id[i] = blob_db_alloc_id();
			len[i] = 1 + rnd(MAXV);
			fill(val[i], len[i]);
		} else {
			edit(val[i], &len[i]);
		}
		zassert_ok(blob_db_update(id[i], val[i], len[i]), "step %d", step);
		expect_value(id[i], val[i], len[i], "after update");

		if (step % 1000 == 999) {
			blob_db_unmount();
			zassert_ok(blob_db_mount());
			for (int j = 0; j < N_IDS; j++) {
				expect_value(id[j], val[j], len[j], "after remount");
			}
		}
	}

	/* The run is only a test of deltas and of their compaction if both
	 * happened. */
	int compacted = 0;

	for (int i = 0; i < N_IDS; i++) {
		struct blob_db_inspect_bucket bs;

		zassert_ok(blob_db_inspect_bucket_get(id[i] % in.n_buckets, &bs, NULL, NULL));
		compacted += bs.gen > gen0[i] + 1;
	}
	zassert_true(compacted > 0, "no bucket compacted during the run");
#if defined(CONFIG_BLOB_DB_IOSTATS)
	struct blob_db_iostats io;

	blob_db_iostats_get(&io);
	zassert_true(io.delta_slots > N_STEPS / 4, "only %u delta slots in %d updates",
		     io.delta_slots, N_STEPS);
	zassert_true(io.compactions > 0);
#endif
}

ZTEST(blob_db_delta, test_small_change_is_written_as_a_delta)
{
#if !defined(CONFIG_BLOB_DB_IOSTATS)
	ztest_test_skip();
#else
	uint8_t v[160];
	struct blob_db_iostats a, b;

	fill(v, sizeof(v));
	const uint64_t id = blob_db_alloc_id();

	zassert_ok(blob_db_update(id, v, sizeof(v)));
	blob_db_iostats_get(&a);
	v[80] ^= 1;
	zassert_ok(blob_db_update(id, v, sizeof(v)));
	blob_db_iostats_get(&b);
	zassert_equal(b.delta_slots - a.delta_slots, 1, "a one-byte change was not a delta");
	zassert_true(b.bytes_written - a.bytes_written < 40,
		     "a one-byte delta cost %llu bytes",
		     (unsigned long long)(b.bytes_written - a.bytes_written));
	expect_value(id, v, sizeof(v), "one-byte delta");
#endif
}

ZTEST(blob_db_delta, test_compaction_writes_chains_out_in_full)
{
	struct blob_db_inspect_info in;
	struct blob_db_inspect_bucket bs;
	uint8_t v[120];
	uint8_t w[120];

	zassert_ok(blob_db_inspect_info_get(&in));
	fill(v, sizeof(v));
	fill(w, sizeof(w));

	/* Two ids in one bucket, both chained, then fill the bucket until it
	 * compacts. */
	const uint64_t a = blob_db_alloc_id();
	uint64_t b;

	do {
		b = blob_db_alloc_id();
	} while (b % in.n_buckets != a % in.n_buckets);

	zassert_ok(blob_db_update(a, v, sizeof(v)));
	zassert_ok(blob_db_update(b, w, sizeof(w)));
	zassert_ok(blob_db_inspect_bucket_get(a % in.n_buckets, &bs, NULL, NULL));

	const uint32_t gen = bs.gen;

	for (int n = 0; n < 2000; n++) {
		v[n % sizeof(v)]++;
		zassert_ok(blob_db_update(a, v, sizeof(v)));
		if (n % 3 == 0) {
			w[(n * 7) % sizeof(w)]++;
			zassert_ok(blob_db_update(b, w, sizeof(w)));
		}
		zassert_ok(blob_db_inspect_bucket_get(a % in.n_buckets, &bs, NULL, NULL));
		if (bs.gen > gen) {
			break;
		}
	}
	zassert_true(bs.gen > gen, "the bucket never compacted");
	zassert_equal(bs.objects, 2);
	expect_value(a, v, sizeof(v), "after compaction");
	expect_value(b, w, sizeof(w), "after compaction");
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	expect_value(a, v, sizeof(v), "after compaction and remount");
	expect_value(b, w, sizeof(w), "after compaction and remount");
}

ZTEST(blob_db_delta, test_delete_over_a_chain)
{
	uint8_t v[100];

	fill(v, sizeof(v));
	const uint64_t id = blob_db_alloc_id();

	zassert_ok(blob_db_update(id, v, sizeof(v)));
	for (int i = 0; i < 5; i++) {
		v[i * 3] ^= 0xff;
		zassert_ok(blob_db_update(id, v, sizeof(v)));
	}
	zassert_ok(blob_db_delete(id));
	zassert_equal(blob_db_get(id, NULL, 0, NULL), -ENOENT);
	zassert_false(blob_db_exists(id));
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_equal(blob_db_get(id, NULL, 0, NULL), -ENOENT);
}

ZTEST(blob_db_delta, test_short_buffer_reports_the_rebuilt_length)
{
	uint8_t v[200];
	uint8_t small[16];
	size_t got = 0;

	fill(v, sizeof(v));
	const uint64_t id = blob_db_alloc_id();

	zassert_ok(blob_db_update(id, v, 150));
	zassert_ok(blob_db_update(id, v, 200));   /* grown by a delta */
	zassert_equal(blob_db_get(id, small, sizeof(small), &got), -ENOMEM);
	zassert_ok(blob_db_size(id, &got));
	zassert_equal(got, 200);

	uint8_t part[50];

	zassert_ok(blob_db_read(id, 120, part, sizeof(part), &got));
	zassert_equal(got, sizeof(part));
	zassert_mem_equal(part, v + 120, sizeof(part));
}

static struct {
	uint64_t id;
	const uint8_t *want;
	size_t len;
	int seen, matched;
} it;

static int it_cb(uint64_t id, const void *p, size_t len, void *user)
{
	ARG_UNUSED(user);
	if (id == it.id) {
		it.seen++;
		it.matched += (len == it.len && memcmp(p, it.want, len) == 0);
	}
	return 0;
}

ZTEST(blob_db_delta, test_iterate_and_count_see_rebuilt_values)
{
	/* count() and iterate() walk resident sectors; a delta there must be
	 * reported as the value it describes, not as its own payload. */
	uint8_t v[90];

	fill(v, sizeof(v));
	const uint64_t id = blob_db_alloc_id();
	const size_t before = blob_db_count();

	zassert_ok(blob_db_update(id, v, sizeof(v)));
	for (int i = 0; i < 3; i++) {
		v[10 + i] ^= 1;
		zassert_ok(blob_db_update(id, v, sizeof(v)));
	}
	zassert_equal(blob_db_count(), before + 1);
	it.id = id;
	it.want = v;
	it.len = sizeof(v);
	it.seen = it.matched = 0;
	zassert_ok(blob_db_iterate(it_cb, NULL));
	zassert_equal(it.seen, 1);
	zassert_equal(it.matched, 1, "iterate handed out the delta, not the value");
}

/* blob_db_delta_power ----------------------------------------------------- */

static struct {
	bool cut;
	bool armed;
	uint32_t budget;
	bool erase_armed;
	uint32_t erase_budget;
	bool tear_head;
	uint32_t bytes;
	uint32_t erases;
	off_t first_off[4];
} pw;

static const struct device *sim;
static uint8_t *sim_part;
static size_t erase_unit;
static uint8_t erase_value;
static uint8_t snap[D_PARTITION_SIZE];

static int pw_write_byte(const struct device *dev, off_t off, uint8_t data)
{
	ARG_UNUSED(dev);
	if (pw.cut) {
		return -EIO;
	}
	if (pw.armed) {
		if (pw.budget == 0) {
			pw.cut = true;
			return -EIO;
		}
		pw.budget--;
	}
	if (pw.bytes < 4) {
		pw.first_off[pw.bytes] = off;
	}
	pw.bytes++;
	return data;
}

static int pw_erase_unit(const struct device *dev, off_t unit_off)
{
	size_t size;
	uint8_t *mem = flash_simulator_get_memory(dev, &size);

	if (pw.cut) {
		return -EIO;
	}
	if (pw.erase_armed) {
		if (pw.erase_budget == 0) {
			memset(&mem[unit_off + (pw.tear_head ? 0 : erase_unit / 2)], erase_value,
			       erase_unit / 2);
			pw.cut = true;
			return -EIO;
		}
		pw.erase_budget--;
	}
	memset(&mem[unit_off], erase_value, erase_unit);
	pw.erases++;
	return 0;
}

static const struct flash_simulator_cb pw_cb = {
	.write_byte = pw_write_byte,
	.erase_unit = pw_erase_unit,
};

static void *power_setup(void)
{
	const struct flash_area *fa;
	struct flash_pages_info page;
	size_t size;

	zassert_ok(flash_area_open(D_PARTITION_ID, &fa));
	sim = fa->fa_dev;
	zassert_ok(flash_get_page_info_by_offs(sim, fa->fa_off, &page));
	erase_unit = page.size;
	erase_value = flash_get_parameters(sim)->erase_value;
	sim_part = (uint8_t *)flash_simulator_get_memory(sim, &size) + fa->fa_off;
	flash_area_close(fa);
	memset(&pw, 0, sizeof(pw));
	flash_simulator_set_callbacks(sim, &pw_cb);
	return NULL;
}

static void power_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	flash_simulator_set_callbacks(sim, NULL);
}

static void power_before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(&pw, 0, sizeof(pw));
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
}

ZTEST_SUITE(blob_db_delta_power, NULL, power_setup, power_before, NULL, power_teardown);

#define N_SIB 4
#define VLEN  96

static struct {
	uint64_t target;
	uint64_t sib[N_SIB];
	uint8_t sib_val[N_SIB][VLEN];
	uint8_t old_val[VLEN];
	uint8_t new_val[VLEN];
} ps;

static bool value_is(uint64_t id, const uint8_t *want)
{
	uint8_t got[VLEN + 8];
	size_t len = 0;

	return blob_db_get(id, got, sizeof(got), &len) == 0 && len == VLEN &&
	       memcmp(got, want, VLEN) == 0;
}

/* Build a target and siblings in one bucket, each with a delta chain. With
 * @p near_full, keep updating the siblings until the target's next update
 * cannot fit without a compaction. */
static void scenario(bool near_full)
{
	struct blob_db_inspect_info in;
	struct blob_db_inspect_bucket bs;

	zassert_ok(blob_db_inspect_info_get(&in));
	fill(ps.old_val, VLEN);
	ps.target = blob_db_alloc_id();
	zassert_ok(blob_db_update(ps.target, ps.old_val, VLEN));
	for (int i = 0; i < N_SIB; i++) {
		do {
			ps.sib[i] = blob_db_alloc_id();
		} while (ps.sib[i] % in.n_buckets != ps.target % in.n_buckets);
		fill(ps.sib_val[i], VLEN);
		zassert_ok(blob_db_update(ps.sib[i], ps.sib_val[i], VLEN));
	}
	/* Two small, adjacent edits each: a chain of two deltas on every id,
	 * short enough that one more is still a delta under either policy. */
	for (int k = 0; k < 2; k++) {
		ps.old_val[40 + k] ^= 0x33;
		zassert_ok(blob_db_update(ps.target, ps.old_val, VLEN));
		for (int i = 0; i < N_SIB; i++) {
			ps.sib_val[i][30 + k] ^= 0x44;
			zassert_ok(blob_db_update(ps.sib[i], ps.sib_val[i], VLEN));
		}
	}
	if (near_full) {
		/* Small updates until the free tail could not take one more
		 * full slot of the target. */
		for (int n = 0; n < 4000; n++) {
			zassert_ok(blob_db_inspect_bucket_get(ps.target % in.n_buckets, &bs, NULL,
							      NULL));
			if (bs.free_bytes < in.slot_overhead + VLEN) {
				break;   /* a full slot of the target no longer fits */
			}
			const int i = n % N_SIB;

			ps.sib_val[i][n % VLEN]++;
			zassert_ok(blob_db_update(ps.sib[i], ps.sib_val[i], VLEN));
		}
	}
	memcpy(ps.new_val, ps.old_val, VLEN);
	if (near_full) {
		fill(ps.new_val, VLEN);         /* a full slot: forces the compaction */
	} else {
		ps.new_val[42] ^= 0x01;   /* a delta on top of the chain */
	}
}

static void check_after_cut(int rc, uint32_t cut, const char *what)
{
	pw.armed = false;
	pw.erase_armed = false;
	pw.cut = false;
	blob_db_unmount();
	zassert_ok(blob_db_mount(), "%s cut %u: mount", what, cut);

	const bool is_old = value_is(ps.target, ps.old_val);
	const bool is_new = value_is(ps.target, ps.new_val);

	zassert_true(is_old || is_new, "%s cut %u: target is neither old nor new", what, cut);
	if (rc == 0) {
		zassert_true(is_new, "%s cut %u: update reported success but value is old", what,
			     cut);
	}
	for (int i = 0; i < N_SIB; i++) {
		zassert_true(value_is(ps.sib[i], ps.sib_val[i]), "%s cut %u: sibling %d damaged",
			     what, cut, i);
	}

	/* The recovered store takes a write that survives a further remount. */
	uint8_t v[VLEN];

	memcpy(v, ps.sib_val[0], VLEN);
	v[0] ^= 0x80;
	zassert_ok(blob_db_update(ps.sib[0], v, VLEN), "%s cut %u: write after recovery", what,
		   cut);
	{
		uint8_t g[VLEN + 8];
		size_t gl = 0;
		int grc = blob_db_get(ps.sib[0], g, sizeof(g), &gl);

		if (grc != 0 || gl != VLEN || memcmp(g, v, VLEN) != 0) {
			TC_PRINT("%s cut %u: before remount get rc=%d len=%zu\n", what, cut, grc, gl);
		}
	}
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	if (!value_is(ps.sib[0], v)) {
		uint8_t g[VLEN + 8];
		size_t gl = 0;
		int grc = blob_db_get(ps.sib[0], g, sizeof(g), &gl);
		int first = -1;

		for (int i = 0; i < VLEN && grc == 0; i++) {
			if (g[i] != v[i]) {
				first = i;
				break;
			}
		}
		TC_PRINT("%s cut %u: after remount get rc=%d len=%zu first diff %d (old? %d)\n",
			 what, cut, grc, gl, first,
			 grc == 0 && memcmp(g, ps.sib_val[0], VLEN) == 0);
	}
	zassert_true(value_is(ps.sib[0], v), "%s cut %u: write after recovery lost", what, cut);
}

static void sweep(const char *what)
{
	/* Reference run: how many bytes and erases the update makes. */
	blob_db_unmount();
	memcpy(snap, sim_part, sizeof(snap));
	zassert_ok(blob_db_mount());
	pw.bytes = 0;
	pw.erases = 0;
	zassert_ok(blob_db_update(ps.target, ps.new_val, VLEN));
	zassert_true(value_is(ps.target, ps.new_val));

	const uint32_t bytes = pw.bytes;
	const uint32_t erases = pw.erases;
	struct blob_db_inspect_info in;

	zassert_ok(blob_db_inspect_info_get(&in));
	if (strcmp(what, "delta append") == 0) {
		zassert_true(bytes < in.slot_overhead + VLEN,
			     "the reference update wrote %u bytes: not a delta", bytes);
	}

	TC_PRINT("%s: first writes at 0x%lx 0x%lx 0x%lx 0x%lx (partition at 0x%lx)\n", what,
		 (long)pw.first_off[0], (long)pw.first_off[1], (long)pw.first_off[2],
		 (long)pw.first_off[3], (long)(sim_part - (uint8_t *)flash_simulator_get_memory(sim, &(size_t){0})));

	zassert_true(bytes > 0, "%s: the update wrote nothing", what);

	for (uint32_t cut = 0; cut < bytes;
	     cut += (cut < 96 || bytes - cut < 96) ? 1 : 13) {
		blob_db_unmount();
		memcpy(sim_part, snap, sizeof(snap));
		zassert_ok(blob_db_mount());
		pw.cut = false;
		pw.budget = cut;
		pw.armed = true;

		const int rc = blob_db_update(ps.target, ps.new_val, VLEN);

		check_after_cut(rc, cut, what);
	}
	for (uint32_t e = 0; e < erases; e++) {
		for (int half = 0; half < 2; half++) {
			blob_db_unmount();
			memcpy(sim_part, snap, sizeof(snap));
			zassert_ok(blob_db_mount());
			pw.cut = false;
			pw.erase_budget = e;
			pw.erase_armed = true;
			pw.tear_head = half == 0;

			const int rc = blob_db_update(ps.target, ps.new_val, VLEN);

			check_after_cut(rc, 100000 + e * 2 + (uint32_t)half, what);
		}
	}
	TC_PRINT("%s: %u write bytes, %u erases swept\n", what, bytes, erases);
}

ZTEST(blob_db_delta_power, test_cut_during_a_delta_append)
{
	scenario(false);
	sweep("delta append");
}

ZTEST(blob_db_delta_power, test_cut_during_a_compaction_of_chains)
{
	scenario(true);
	sweep("compaction of chains");
}
