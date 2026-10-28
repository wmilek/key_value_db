/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * A power cut inside a plain slot append, followed by more appends to the same
 * bucket. The torn slot ends the log where it starts; the next append must not
 * be programmed over the torn bytes, or it is lost although update() reported
 * success. Every backend, no deltas involved.
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

#define TA_PARTITION_ID   PARTITION_ID(storage_partition)
#define TA_PARTITION_SIZE DT_REG_SIZE(DT_NODELABEL(storage_partition))
#define TA_LEN            40

static struct {
	bool cut, armed;
	uint32_t budget, bytes;
} ta;

static const struct device *ta_sim;
static uint8_t *ta_part;
static uint8_t ta_snap[TA_PARTITION_SIZE];

static int ta_write_byte(const struct device *dev, off_t off, uint8_t data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(off);
	if (ta.cut) {
		return -EIO;
	}
	if (ta.armed) {
		if (ta.budget == 0) {
			ta.cut = true;
			return -EIO;
		}
		ta.budget--;
	}
	ta.bytes++;
	return data;
}

static int ta_erase_unit(const struct device *dev, off_t unit_off)
{
	size_t size;
	uint8_t *mem = flash_simulator_get_memory(dev, &size);
	struct flash_pages_info page;

	if (ta.cut) {
		return -EIO;
	}
	(void)flash_get_page_info_by_offs(dev, unit_off, &page);
	memset(&mem[unit_off], flash_get_parameters(dev)->erase_value, page.size);
	return 0;
}

static const struct flash_simulator_cb ta_cb = {
	.write_byte = ta_write_byte,
	.erase_unit = ta_erase_unit,
};

static void *ta_setup(void)
{
	const struct flash_area *fa;
	size_t size;

	zassert_ok(flash_area_open(TA_PARTITION_ID, &fa));
	ta_sim = fa->fa_dev;
	ta_part = (uint8_t *)flash_simulator_get_memory(ta_sim, &size) + fa->fa_off;
	flash_area_close(fa);
	flash_simulator_set_callbacks(ta_sim, &ta_cb);
	return NULL;
}

static void ta_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	flash_simulator_set_callbacks(ta_sim, NULL);
}

static void ta_before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(&ta, 0, sizeof(ta));
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
}

ZTEST_SUITE(blob_db_torn_append, NULL, ta_setup, ta_before, NULL, ta_teardown);

/* b's values are shorter than a's: a slot programmed over a torn one then
 * disagrees with it in the length bytes, not only in the payload. */
static size_t ta_len(uint8_t fillb)
{
	return (fillb == 0x22 || fillb == 0x44) ? TA_LEN - 15 : TA_LEN;
}

static bool ta_is(uint64_t id, uint8_t fillb)
{
	uint8_t got[TA_LEN + 4];
	size_t len = 0;

	if (blob_db_get(id, got, sizeof(got), &len) != 0 || len != ta_len(fillb)) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (got[i] != fillb) {
			return false;
		}
	}
	return true;
}

static void ta_put(uint64_t id, uint8_t fillb)
{
	uint8_t v[TA_LEN];

	memset(v, fillb, sizeof(v));
	zassert_ok(blob_db_update(id, v, ta_len(fillb)));
}

ZTEST(blob_db_torn_append, test_append_after_a_torn_append_is_kept)
{
	struct blob_db_inspect_info in;

	zassert_ok(blob_db_inspect_info_get(&in));

	const uint64_t a = blob_db_alloc_id();
	uint64_t b;

	do {
		b = blob_db_alloc_id();
	} while (b % in.n_buckets != a % in.n_buckets);
	ta_put(a, 0x11);
	ta_put(b, 0x22);

	/* Reference: how many bytes the append of a's new value writes. */
	blob_db_unmount();
	memcpy(ta_snap, ta_part, sizeof(ta_snap));
	zassert_ok(blob_db_mount());
	ta.bytes = 0;
	ta_put(a, 0x33);

	const uint32_t bytes = ta.bytes;

	zassert_true(bytes > 0);
	for (uint32_t cut = 1; cut < bytes; cut++) {
		blob_db_unmount();
		memcpy(ta_part, ta_snap, sizeof(ta_snap));
		zassert_ok(blob_db_mount());
		ta.cut = false;
		ta.budget = cut;
		ta.armed = true;

		uint8_t v[TA_LEN];

		memset(v, 0x33, sizeof(v));
		(void)blob_db_update(a, v, sizeof(v));
		ta.armed = false;
		ta.cut = false;
		blob_db_unmount();
		zassert_ok(blob_db_mount(), "cut %u: mount", cut);
		zassert_true(ta_is(a, 0x11), "cut %u: a is not its old value", cut);
		zassert_true(ta_is(b, 0x22), "cut %u: b damaged", cut);

		/* The next append to the same bucket: acknowledged, so it must
		 * read back, now and after a remount. */
		ta_put(b, 0x44);
		zassert_true(ta_is(b, 0x44), "cut %u: append after the torn one unreadable", cut);
		blob_db_unmount();
		zassert_ok(blob_db_mount());
		zassert_true(ta_is(b, 0x44), "cut %u: append after the torn one lost on remount",
			     cut);
		zassert_true(ta_is(a, 0x11), "cut %u: a changed", cut);
	}
}

/* Rewrite @p id, alternating two values, until bucket @p bid compacts; the
 * last value written is 0x44. */
static void ta_rewrite_until_compacted(uint64_t id, uint16_t bid)
{
	struct blob_db_inspect_bucket bs;
	uint8_t v[TA_LEN];

	zassert_ok(blob_db_inspect_bucket_get(bid, &bs, NULL, NULL));

	const uint32_t gen = bs.gen;

	for (int n = 0; n < 400; n++) {
		memset(v, 0x44, sizeof(v));
		zassert_ok(blob_db_update(id, v, ta_len(0x44)));
		zassert_ok(blob_db_inspect_bucket_get(bid, &bs, NULL, NULL));
		if (bs.gen > gen) {
			return;
		}
		memset(v, 0x22, sizeof(v));
		zassert_ok(blob_db_update(id, v, ta_len(0x22)));
	}
	zassert_unreachable("the bucket never compacted");
}

/* A slot that rots after it was written (a CRC failure on a slot whose header
 * is intact) is skipped by readers, which then still find the slots after it.
 * Compaction must do the same: drop the rotten slot, keep everything after.
 *
 * `third` is never rewritten: the update that triggers the compaction appends
 * into the compacted bucket, so only a slot written before it and after the
 * rotten one shows whether compaction kept the slots behind the rot. */
ZTEST(blob_db_torn_append, test_compaction_keeps_slots_after_a_rotten_one)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_BLOB_DB_BACKEND_FLASH_AREA);

	struct blob_db_inspect_info in;
	struct blob_db_inspect_bucket bs;

	zassert_ok(blob_db_inspect_info_get(&in));

	const uint64_t first = blob_db_alloc_id();
	uint64_t second, third;

	do {
		second = blob_db_alloc_id();
	} while (second % in.n_buckets != first % in.n_buckets);
	do {
		third = blob_db_alloc_id();
	} while (third % in.n_buckets != first % in.n_buckets);
	ta_put(first, 0x11);
	ta_put(second, 0x22);
	ta_put(third, 0x55);

	const size_t count = blob_db_count();
	const uint16_t bid = (uint16_t)(first % in.n_buckets);

	blob_db_unmount();
	/* first is the bucket's first slot: header 16 B, slot head 12 B. */
	const struct flash_area *fa;
	const off_t payload_off = (off_t)(3 + bid) * in.sector_size + 16 + 12;
	uint8_t byte;

	zassert_ok(flash_area_open(TA_PARTITION_ID, &fa));
	zassert_ok(flash_area_read(fa, payload_off, &byte, 1));
	byte &= (uint8_t)(byte - 1);
	zassert_ok(flash_area_write(fa, payload_off, &byte, 1));
	flash_area_close(fa);
	zassert_ok(blob_db_mount());
	zassert_true(ta_is(second, 0x22), "second unreachable after first rotted");
	zassert_true(ta_is(third, 0x55), "third unreachable after first rotted");
	zassert_equal(blob_db_count(), count - 1, "count() stopped at the rotten slot");
	zassert_ok(blob_db_inspect_bucket_get(bid, &bs, NULL, NULL));
	zassert_equal(bs.rotten, 1);
	zassert_equal(bs.tail_bytes, 0, "inspect stopped at the rotten slot");

	ta_rewrite_until_compacted(second, bid);
	zassert_true(ta_is(second, 0x44));
	zassert_true(ta_is(third, 0x55), "compaction lost the slots after the rotten one");
	zassert_equal(blob_db_count(), count - 1);
	zassert_ok(blob_db_inspect_bucket_get(bid, &bs, NULL, NULL));
	zassert_equal(bs.rotten, 0, "compaction kept the rotten slot");
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_true(ta_is(second, 0x44));
	zassert_true(ta_is(third, 0x55), "compaction lost the slots after the rotten one");
	zassert_equal(blob_db_count(), count - 1);
}

/* The same through a power cut, on every backend: an append torn after its
 * header leaves a slot the walk can size but whose CRC fails. Writes after it
 * are acknowledged, so the bucket's next compaction must keep them. */
ZTEST(blob_db_torn_append, test_compaction_keeps_slots_after_a_torn_one)
{
	struct blob_db_inspect_info in;
	struct blob_db_inspect_bucket bs;

	zassert_ok(blob_db_inspect_info_get(&in));

	const uint64_t a = blob_db_alloc_id();
	uint64_t b, c;

	do {
		b = blob_db_alloc_id();
	} while (b % in.n_buckets != a % in.n_buckets);
	do {
		c = blob_db_alloc_id();
	} while (c % in.n_buckets != a % in.n_buckets);
	ta_put(a, 0x11);
	ta_put(b, 0x22);

	const size_t count = blob_db_count();
	const uint16_t bid = (uint16_t)(a % in.n_buckets);

	/* Cut halfway through a's new payload: the 12-byte slot head is on
	 * flash, the rest of the payload and the CRC are not. */
	ta.budget = sizeof(uint32_t) + sizeof(uint64_t) + TA_LEN / 2;
	ta.armed = true;

	uint8_t v[TA_LEN];

	memset(v, 0x33, sizeof(v));
	(void)blob_db_update(a, v, sizeof(v));
	zassert_true(ta.cut, "the append finished before the cut");
	ta.armed = false;
	ta.cut = false;
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_inspect_bucket_get(bid, &bs, NULL, NULL));
	zassert_equal(bs.rotten, 1, "the cut left no sized, rotten slot");
	zassert_true(ta_is(a, 0x11), "a is not its old value");

	/* Appended after the torn slot, and acknowledged. */
	ta_put(c, 0x55);
	zassert_true(ta_is(c, 0x55));
	zassert_equal(blob_db_count(), count + 1, "count() stopped at the torn slot");

	ta_rewrite_until_compacted(b, bid);
	zassert_true(ta_is(c, 0x55), "compaction lost the slots after the torn one");
	zassert_true(ta_is(a, 0x11), "compaction lost the slots before the torn one");
	zassert_true(ta_is(b, 0x44));
	zassert_equal(blob_db_count(), count + 1);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_true(ta_is(c, 0x55), "compaction lost the slots after the torn one");
	zassert_true(ta_is(a, 0x11));
	zassert_equal(blob_db_count(), count + 1);
}
