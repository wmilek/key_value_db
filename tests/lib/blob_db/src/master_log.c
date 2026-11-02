/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * The major-5 master: a header written once per erase and a log of records
 * (doc/proposals/2026-10-28-master-log.md §3, §4, §11). The cases forge
 * flash at blob_db's own offsets, so they run on the flash_area backend and
 * skip on UBI.
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
#include <app/lib/blob_db_iostats.h>

#include "master_img.h"

#define ML_SKIP_UNLESS_RAW_PARTITION() \
	Z_TEST_SKIP_IFNDEF(CONFIG_BLOB_DB_BACKEND_FLASH_AREA)

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
#define ML_BUILD_INCOMPAT  0x1u
#else
#define ML_BUILD_INCOMPAT  0x0u
#endif

static uint8_t ml_save[2][MI_SECTOR_SZ];

static void ml_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount(), "mount failed in setup");
	zassert_ok(blob_db_format(), "format failed in setup");
}

static void ml_after(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
}

ZTEST_SUITE(blob_db_master_log, NULL, NULL, ml_before, ml_after, NULL);

static uint64_t ml_put(const char *val)
{
	const uint64_t id = blob_db_alloc_id();

	zassert_not_equal(id, 0);
	zassert_ok(blob_db_update(id, val, strlen(val)));
	return id;
}

static void ml_assert_blob(uint64_t id, const char *val)
{
	uint8_t buf[16];
	size_t got;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_equal(got, strlen(val));
	zassert_mem_equal(buf, val, got);
}

static void ml_info(struct blob_db_inspect_info *in)
{
	zassert_ok(blob_db_inspect_info_get(in));
}

/* Allocate until the active master's log has rotated @p n times. */
static uint64_t ml_rotate(int n)
{
	struct blob_db_inspect_info in;
	uint64_t id = 0;

	ml_info(&in);

	const uint32_t want = in.rot_gen + (uint32_t)n;

	while (in.rot_gen < want) {
		id = blob_db_alloc_id();
		zassert_not_equal(id, 0);
		ml_info(&in);
	}
	return id;
}

/* Remount and report what the mount itself wrote. */
static void ml_remount(struct blob_db_iostats *io)
{
	zassert_ok(blob_db_unmount());
	blob_db_iostats_reset();
	zassert_ok(blob_db_mount());
	blob_db_iostats_get(io);
}

static void ml_save_masters(void)
{
	for (uint8_t i = 0; i < 2; i++) {
		mi_sector_read(i);
		memcpy(ml_save[i], mi_sec, MI_SECTOR_SZ);
	}
}

static void ml_assert_masters_unchanged(const char *why)
{
	for (uint8_t i = 0; i < 2; i++) {
		mi_sector_read(i);
		zassert_mem_equal(mi_sec, ml_save[i], MI_SECTOR_SZ,
				  "%s: master %u was written", why, i);
	}
}

static void ml_restore_masters(void)
{
	for (uint8_t i = 0; i < 2; i++) {
		memcpy(mi_sec, ml_save[i], MI_SECTOR_SZ);
		mi_sector_write(i);
	}
}

/* 3c, 5b. A format writes both master sectors (D18), each a header and a
 * record[0] for the same state, byte for byte as §3 lays them out; and the
 * next mount has nothing to doubt, so it writes nothing. */
ZTEST(blob_db_master_log, test_format_writes_both_masters)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct mrec_img rec[2];
	struct blob_db_iostats io;

	zassert_ok(blob_db_unmount());
	for (uint8_t slot = 0; slot < 2; slot++) {
		struct mhdr_img want = {
			.magic = { 'B', 'D', 'M', 'S' },
			.format_major = MI_MAJOR,
			.format_minor = 0,
			.hdr_len = MI_HDR_LEN,
			.rot_gen = slot == 0 ? 2 : 1,
			.peb_size = MI_SECTOR_SZ,
			.n_buckets = MI_N_BUCKETS,
			.rec_size = (uint16_t)mi_rec_size(),
			.incompat = ML_BUILD_INCOMPAT,
			.max_payload_len = CONFIG_BLOB_DB_MAX_PAYLOAD_LEN,
			.write_align = (uint16_t)mi_align(),
		};

		mhdr_seal(&want);
		mi_sector_read(slot);
		zassert_mem_equal(mi_sec, &want, sizeof(want),
				  "master %u header is not the §3.1 image", slot);
		for (size_t i = sizeof(want); i < MI_LOG_OFF; i++) {
			zassert_equal(mi_sec[i], 0xff, "master %u filler byte %zu",
				      slot, i);
		}
		zassert_equal(mi_log_end(), 1, "master %u holds more than record[0]",
			      slot);
		zassert_equal(mi_log_newest(&rec[slot]), 0);
		zassert_equal(rec[slot].next_id_hint, 2 + 1 + MI_ID_STEP);
		zassert_equal(rec[slot].seg_owner, 0);
		zassert_equal(rec[slot].fuses, 0);
		zassert_equal(rec[slot].seq_hint, 0);
		zassert_equal(rec[slot].reserved0 | rec[slot].reserved1 |
			      rec[slot].reserved2, 0, "reserved bytes are not 0");
		for (size_t i = sizeof(rec[slot]); i < mi_rec_size(); i++) {
			zassert_equal(mi_slot(0)[i], 0xff, "record padding");
		}
	}

	blob_db_iostats_reset();
	zassert_ok(blob_db_mount());
	blob_db_iostats_get(&io);
	zassert_equal(io.writes + io.erases, 0,
		      "a mount after a format wrote %u / erased %u", io.writes,
		      io.erases);
}

/* 1, M1. A master update programs one record and erases nothing. */
ZTEST(blob_db_master_log, test_records_erase_nothing)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct blob_db_iostats io;
	struct blob_db_inspect_info in;

	ml_info(&in);

	const uint16_t room = in.master_log_cap - in.master_log_used;

	blob_db_iostats_reset();
	/* Two ceiling records, if the log has room for both: one each time
	 * allocation reaches the ceiling, which every record puts STEP + 1
	 * past the next id. */
	for (int i = 0; i < 2 * (MI_ID_STEP + 1) + 1; i++) {
		zassert_not_equal(blob_db_alloc_id(), 0);
	}
	blob_db_iostats_get(&io);
	if (room >= 2) {
		zassert_equal(io.master_records, 2, "%u records", io.master_records);
		zassert_equal(io.master_rotations, 0);
		zassert_equal(io.erases, 0, "a record append erased %u blocks",
			      io.erases);
	}
}

/* 4. A full log rotates into the other sector; the state survives, and the
 * new header carries the old one's feature sets verbatim, an unknown compat
 * bit included. */
ZTEST(blob_db_master_log, test_rotation_keeps_state_and_features)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct mhdr_img h;
	struct blob_db_inspect_info in;

	/* An unknown compat bit, which any build may ignore, in both
	 * headers. */
	zassert_ok(blob_db_unmount());
	for (uint8_t slot = 0; slot < 2; slot++) {
		master_hdr_read_raw(slot, &h);
		h.compat |= 0x20;
		mhdr_seal(&h);
		master_hdr_rewrite_raw(slot, &h);
	}
	zassert_ok(blob_db_mount());

	const uint64_t id = ml_put("ROTATE");
	const uint64_t last = ml_rotate(2);

	ml_info(&in);
	for (uint8_t slot = 0; slot < 2; slot++) {
		master_hdr_read_raw(slot, &h);
		zassert_equal(h.compat, 0x20, "master %u lost the compat bit", slot);
		zassert_equal(h.incompat, ML_BUILD_INCOMPAT);
	}

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	ml_assert_blob(id, "ROTATE");
	zassert_true(blob_db_alloc_id() > last, "an id was issued twice");
}

/* §3.2a. A record write that fails having programmed nothing leaves an
 * erased slot. The next update rotates rather than write past it, so the
 * written slots stay a prefix and no later reader can stop at the hole. */
static struct {
	bool armed;
	bool fired;
	off_t lo, hi;
} ml_fail;

static int ml_write_byte(const struct device *dev, off_t off, uint8_t data)
{
	ARG_UNUSED(dev);
	if (ml_fail.armed && off >= ml_fail.lo && off < ml_fail.hi) {
		ml_fail.armed = false;
		ml_fail.fired = true;
		return -EIO;
	}
	return data;
}

static const struct flash_simulator_cb ml_cb = {
	.write_byte = ml_write_byte,
};

ZTEST(blob_db_master_log, test_failed_record_write_rotates)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	const struct flash_area *fa;
	struct blob_db_inspect_info in;
	struct blob_db_iostats io;
	uint64_t last = 0;

	ml_info(&in);
	zassert_true(in.master_log_used < in.master_log_cap,
		     "precondition: room for a record");

	zassert_ok(flash_area_open(MI_PARTITION_ID, &fa));

	const struct device *dev = fa->fa_dev;
	const off_t sector = fa->fa_off + (off_t)in.active_master * MI_SECTOR_SZ;

	flash_area_close(fa);

	/* Fail the first byte of the next record slot: nothing programmed. */
	memset(&ml_fail, 0, sizeof(ml_fail));
	ml_fail.lo = sector + MI_LOG_OFF;
	ml_fail.hi = sector + MI_SECTOR_SZ;
	ml_fail.armed = true;
	flash_simulator_set_callbacks(dev, &ml_cb);

	uint64_t id;

	do {
		id = blob_db_alloc_id();
		if (id != 0) {
			last = id;
		}
	} while (id != 0 && !ml_fail.fired);
	flash_simulator_set_callbacks(dev, NULL);
	zassert_true(ml_fail.fired, "no record write was attempted");
	zassert_equal(id, 0, "alloc_id handed out an id its ceiling did not cover");

	blob_db_iostats_reset();
	id = blob_db_alloc_id();
	blob_db_iostats_get(&io);
	zassert_not_equal(id, 0);
	zassert_true(id > last);
	zassert_equal(io.master_rotations, 1,
		      "the update after a failed record did not rotate");

	/* The old sector's slots are still a written prefix: the failed slot
	 * reads erased, and nothing was written past it. */
	mi_sector_read(in.active_master);
	zassert_equal(mi_log_end(), in.master_log_used,
		      "a record was written past the failed slot");

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	zassert_true(blob_db_alloc_id() > id, "an id was issued twice");
}

/* Patch master @p slot's saved header with @p patch, mount, and expect a
 * refusal that writes nothing; then put both masters back. */
static void ml_expect_refused(uint8_t slot, void (*patch)(struct mhdr_img *),
			      int want, const char *what)
{
	struct mhdr_img h;

	memcpy(&h, ml_save[slot], sizeof(h));
	patch(&h);
	mhdr_seal(&h);
	master_hdr_rewrite_raw(slot, &h);

	zassert_equal(blob_db_mount(), want, "%s in master %u: mount returned "
		      "something else", what, slot);
	mi_sector_read(slot);
	zassert_mem_equal(mi_sec, &h, sizeof(h), "%s: refused mount wrote master "
			  "%u", what, slot);
	ml_restore_masters();
}

static void p_peb(struct mhdr_img *h) { h->peb_size *= 2; }
static void p_buckets(struct mhdr_img *h) { h->n_buckets -= 1; }
static void p_rec(struct mhdr_img *h) { h->rec_size += 16; }
static void p_cap(struct mhdr_img *h) { h->max_payload_len += 1; }
static void p_align(struct mhdr_img *h) { h->write_align *= 2; }
static void p_suite(struct mhdr_img *h) { h->suite = 1; }
static void p_hdr_len(struct mhdr_img *h) { h->hdr_len = 84; }
static void p_incompat(struct mhdr_img *h) { h->incompat |= 0x80; }
static void p_ro_compat(struct mhdr_img *h) { h->ro_compat |= 0x1; }

/* 5b, D19. Each geometry value in a header must match the build and the
 * device, in every ok header, the older one included; otherwise mount
 * refuses and writes nothing. */
ZTEST(blob_db_master_log, test_geometry_mismatch_is_refused)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	static const struct {
		void (*patch)(struct mhdr_img *);
		const char *what;
	} cases[] = {
		{ p_peb, "peb_size" },        { p_buckets, "n_buckets" },
		{ p_rec, "rec_size" },        { p_cap, "max_payload_len" },
		{ p_align, "write_align" },   { p_suite, "suite" },
		{ p_hdr_len, "hdr_len" },
	};

	zassert_ok(blob_db_unmount());
	ml_save_masters();
	for (size_t k = 0; k < ARRAY_SIZE(cases); k++) {
		for (uint8_t slot = 0; slot < 2; slot++) {
			ml_expect_refused(slot, cases[k].patch, -ENOTSUP,
					  cases[k].what);
		}
	}
	zassert_ok(blob_db_mount());
}

/* 5. An unknown incompat or ro_compat bit refuses the store, whichever ok
 * sector carries it; nothing is written. */
ZTEST(blob_db_master_log, test_unknown_feature_bits_are_refused)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	zassert_ok(blob_db_unmount());
	ml_save_masters();
	for (uint8_t slot = 0; slot < 2; slot++) {
		ml_expect_refused(slot, p_incompat, -ENOTSUP, "unknown incompat");
		ml_expect_refused(slot, p_ro_compat, -ENOTSUP, "unknown ro_compat");
	}
	zassert_ok(blob_db_mount());
}

/* §3.1. Two ok sectors at the same rot_gen cannot be ordered: -EIO, nothing
 * written. */
ZTEST(blob_db_master_log, test_equal_rot_gen_is_refused)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct mhdr_img a, b;

	zassert_ok(blob_db_unmount());
	master_hdr_read_raw(0, &a);
	master_hdr_read_raw(1, &b);
	b.rot_gen = a.rot_gen;
	mhdr_seal(&b);
	master_hdr_rewrite_raw(1, &b);
	ml_save_masters();

	zassert_equal(blob_db_mount(), -EIO);
	ml_assert_masters_unchanged("equal rot_gen");
	masters_erase();
}

/* §4.2. A half-done upgrade: one ok header carries a feature bit the other
 * lacks. Mount finishes it, so both headers carry the bit. */
ZTEST(blob_db_master_log, test_half_done_upgrade_is_finished)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct mhdr_img h;
	const uint64_t id = ml_put("UPGRADE");

	zassert_ok(blob_db_unmount());
	master_hdr_read_raw(0, &h);
	h.compat |= 0x40;
	mhdr_seal(&h);
	master_hdr_rewrite_raw(0, &h);

	zassert_ok(blob_db_mount());
	for (uint8_t slot = 0; slot < 2; slot++) {
		master_hdr_read_raw(slot, &h);
		zassert_equal(h.compat & 0x40, 0x40,
			      "master %u lacks the bit after mount", slot);
	}
	ml_assert_blob(id, "UPGRADE");
}

/* 10. The fuse word passes through every record — appends, rotations, the
 * write that ends a doubt — unchanged by a build that trips nothing, and
 * only a format clears it. */
ZTEST(blob_db_master_log, test_fuse_word_passes_through)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct mrec_img r;
	struct blob_db_inspect_info in;

	zassert_ok(blob_db_unmount());

	const uint8_t slot = master_active_raw();

	(void)master_newest_raw(slot, &r);
	r.fuses = 0xffffffffu;
	master_rec_append_raw(slot, &r);

	zassert_ok(blob_db_mount());
	ml_info(&in);
	zassert_equal(in.fuses, 0xffffffffu);

	(void)ml_put("FUSES");
	(void)ml_rotate(2);

	for (uint8_t s = 0; s < 2; s++) {
		zassert_true(master_newest_raw(s, &r) >= 0);
		zassert_equal(r.fuses, 0xffffffffu,
			      "master %u's newest record dropped fuse bits", s);
	}

	/* A doubt (the newest record rots) ends with a write that carries it. */
	zassert_ok(blob_db_unmount());

	const uint8_t act = master_active_raw();
	const int newest = master_newest_raw(act, &r);

	master_flip_raw(act, mi_rec_off((uint16_t)newest) +
			offsetof(struct mrec_img, seq_hint), 0x01);
	zassert_ok(blob_db_mount());
	(void)master_newest_raw(master_active_raw(), &r);
	zassert_equal(r.fuses, 0xffffffffu, "the doubt-ending record dropped fuses");

	zassert_ok(blob_db_format());
	for (uint8_t s = 0; s < 2; s++) {
		(void)master_newest_raw(s, &r);
		zassert_equal(r.fuses, 0, "format left fuses in master %u", s);
	}
}

/* B5. erase_all's record is the one that lowers the id ceiling: to one step
 * past id 2. */
ZTEST(blob_db_master_log, test_erase_all_lowers_the_ceiling)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct mrec_img r;

	for (int i = 0; i < 4 * MI_ID_STEP; i++) {
		zassert_not_equal(blob_db_alloc_id(), 0);
	}
	zassert_ok(blob_db_erase_all());
	zassert_ok(blob_db_unmount());

	(void)master_newest_raw(master_active_raw(), &r);
	zassert_equal(r.next_id_hint, 2 + 1 + MI_ID_STEP,
		      "erase_all left ceiling %llu",
		      (unsigned long long)r.next_id_hint);
	zassert_ok(blob_db_mount());
}

/* D16. A record copied from an earlier rotation into the live log fails its
 * CRC, which covers its own header's rot_gen: mount takes the newest record
 * of this rotation and doubts. */
ZTEST(blob_db_master_log, test_record_from_another_rotation_fails)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct mrec_img old, cur;
	struct blob_db_iostats io;
	const uint64_t id = ml_put("BIND");

	zassert_ok(blob_db_unmount());

	const uint8_t act = master_active_raw();

	(void)master_newest_raw(!act, &old);
	(void)master_newest_raw(act, &cur);

	/* Plant the old sector's record, sealed for its own rot_gen, at the end
	 * of the active log. */
	mi_sector_read(act);

	const uint16_t end = mi_log_end();
	struct mhdr_img oh;

	master_hdr_read_raw(!act, &oh);
	old.rec_no = end;
	old.next_id_hint += 1000;
	old.crc32 = mrec_crc(oh.rot_gen, &old);
	mi_sector_read(act);
	memcpy(mi_slot(end), &old, sizeof(old));
	mi_sector_write(act);

	blob_db_iostats_reset();
	zassert_ok(blob_db_mount());
	blob_db_iostats_get(&io);
	zassert_true(io.master_records >= 1, "mount did not end its doubt");
	ml_assert_blob(id, "BIND");
}

/* §3.2a, §3.5. Mount finds the log end by a binary search on "fully erased"
 * and reads only the slots it needs. At every fill level from one record to
 * a full log, mount adopts the reference reader's end (every slot read) and
 * reads less than the sector it searched. */
ZTEST(blob_db_master_log, test_mount_searches_the_log_end)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct blob_db_iostats io;
	struct blob_db_inspect_info in;
	struct mrec_img r;
	const uint64_t id = ml_put("SRCH");

	zassert_ok(blob_db_unmount());

	const uint8_t act = master_active_raw();
	const uint16_t cap = mi_log_cap();

	(void)master_newest_raw(act, &r);

	const uint32_t rot_gen = ((const struct mhdr_img *)mi_sec)->rot_gen;

	for (uint16_t end = 1; end <= cap; end++) {
		mi_sector_read(act);
		memset(mi_sec + MI_LOG_OFF, 0xff, MI_SECTOR_SZ - MI_LOG_OFF);
		for (uint16_t i = 0; i < end; i++) {
			r.rec_no = i;
			r.crc32 = mrec_crc(rot_gen, &r);
			memcpy(mi_slot(i), &r, sizeof(r));
		}
		mi_sector_write(act);
		zassert_equal(mi_log_end(), end);

		blob_db_iostats_reset();
		zassert_ok(blob_db_mount(), "mount failed at end %u", end);
		blob_db_iostats_get(&io);
		ml_info(&in);
		zassert_equal(in.active_master, act);
		zassert_equal(in.master_log_used, end,
			      "mount found end %u, the log holds %u",
			      in.master_log_used, end);
		zassert_equal(io.writes + io.erases, 0,
			      "a clean mount wrote at end %u", end);
		zassert_true(io.bytes_read < MI_SECTOR_SZ,
			     "mount read %llu B at end %u",
			     (unsigned long long)io.bytes_read, end);
		ml_assert_blob(id, "SRCH");
		zassert_ok(blob_db_unmount());
	}
	zassert_ok(blob_db_mount());
}

/* 3. A rotten record before the newest one is harmless: the newest stands,
 * and mount has nothing to doubt. */
ZTEST(blob_db_master_log, test_rotten_record_mid_log_needs_no_scan)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct blob_db_iostats io;
	struct blob_db_inspect_info in;

	for (int i = 0; i < 2 * (MI_ID_STEP + 1); i++) {
		zassert_not_equal(blob_db_alloc_id(), 0);
	}
	ml_info(&in);
	if (in.master_log_used < 3) {
		ztest_test_skip();   /* the log rotated: no middle record */
	}
	zassert_ok(blob_db_unmount());
	master_flip_raw(in.active_master, mi_rec_off(1) +
			offsetof(struct mrec_img, seg_owner), 0x04);

	ml_remount(&io);
	zassert_equal(io.writes + io.erases, 0, "mount wrote for a rotten "
		      "record that a later one replaces");
}

/* 3a. A rotten last record is doubt: mount scans and appends one record, no
 * erase, and the next mount has nothing to doubt. */
ZTEST(blob_db_master_log, test_rotten_last_record_ends_with_one_append)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct blob_db_iostats io;
	struct mrec_img r;
	const uint64_t id = ml_put("TAIL");

	/* A record past record[0], so the one before the rotten one verifies:
	 * a rotten record[0] alone is no usable sector (3b), not a bad tail. */
	for (int i = 0; i < MI_ID_STEP + 2; i++) {
		zassert_not_equal(blob_db_alloc_id(), 0);
	}
	zassert_ok(blob_db_unmount());

	const uint8_t act = master_active_raw();
	const int newest = master_newest_raw(act, &r);

	master_flip_raw(act, mi_rec_off((uint16_t)newest) +
			offsetof(struct mrec_img, next_id_hint), 0x01);

	ml_remount(&io);
	if (mi_log_cap() > (uint16_t)newest + 1) {
		zassert_equal(io.master_records, 1, "%u records", io.master_records);
		zassert_equal(io.master_rotations, 0, "an erase for a rotten record");
	}
	ml_assert_blob(id, "TAIL");

	ml_remount(&io);
	zassert_equal(io.writes + io.erases, 0, "the doubt was not ended");
}

/* 3a. A corrupt other sector is doubt, ended by a rotation into it; the next
 * ten mounts write nothing. */
ZTEST(blob_db_master_log, test_corrupt_other_sector_ends_with_a_rotation)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct blob_db_iostats io;
	const uint64_t id = ml_put("OTHER");

	zassert_ok(blob_db_unmount());

	const uint8_t other = !master_active_raw();

	master_flip_raw(other, offsetof(struct mhdr_img, hdr_crc32), 0x01);

	ml_remount(&io);
	zassert_equal(io.master_rotations, 1, "%u rotations", io.master_rotations);
	ml_assert_blob(id, "OTHER");

	for (int i = 0; i < 10; i++) {
		ml_remount(&io);
		zassert_equal(io.writes + io.erases, 0,
			      "mount %d after the rotation wrote", i);
	}
}

/* 3b. A header with no record that verifies (a rotation torn after its
 * header landed) above a usable sector: mount takes the lower one, doubts,
 * and rotates into the header-only one. */
ZTEST(blob_db_master_log, test_header_only_sector_falls_back)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	struct mhdr_img h;
	struct blob_db_inspect_info in;
	const uint64_t id = ml_put("LOWER");

	zassert_ok(blob_db_unmount());

	const uint8_t act = master_active_raw();
	const uint8_t other = !act;
	struct mhdr_img top;

	master_hdr_read_raw(act, &top);
	master_hdr_read_raw(other, &h);
	h.rot_gen = top.rot_gen + 1;
	mhdr_seal(&h);
	memset(mi_sec, 0xff, sizeof(mi_sec));
	memcpy(mi_sec, &h, sizeof(h));
	mi_sector_write(other);

	zassert_ok(blob_db_mount());
	ml_assert_blob(id, "LOWER");
	ml_info(&in);
	zassert_equal(in.active_master, other,
		      "the doubt was not ended by a rotation into it");
	/* The rotation replaces the header-only sector, so it takes the
	 * lower sector's rot_gen + 1: the value it overwrites. */
	zassert_equal(in.rot_gen, top.rot_gen + 1);
}

/* 3b. Two header-only sectors, or one beside an erased sector (a first
 * format torn after its header): no usable master, -EIO, nothing written. */
ZTEST(blob_db_master_log, test_no_usable_master_is_refused)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	Z_TEST_SKIP_IFDEF(CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT);

	struct mhdr_img h;

	zassert_ok(blob_db_unmount());
	for (int pass = 0; pass < 2; pass++) {
		for (uint8_t slot = 0; slot < 2; slot++) {
			master_hdr_read_raw(slot, &h);
			memset(mi_sec, 0xff, sizeof(mi_sec));
			if (pass == 0 || slot == 0) {
				memcpy(mi_sec, &h, sizeof(h));
			}
			mi_sector_write(slot);
		}
		ml_save_masters();
		zassert_equal(blob_db_mount(), -EIO, "pass %d mounted", pass);
		ml_assert_masters_unchanged("no usable master");
	}
	masters_erase();
}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
/* D20. A build with large payloads, on a store formatted without them, writes
 * that store as a build without them would: no segment, -EINVAL beyond the
 * cap. */
ZTEST(blob_db_master_log, test_large_write_needs_the_store_feature)
{
	ML_SKIP_UNLESS_RAW_PARTITION();

	static uint8_t big[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN + 1];
	struct mhdr_img h;

	zassert_ok(blob_db_unmount());
	for (uint8_t slot = 0; slot < 2; slot++) {
		master_hdr_read_raw(slot, &h);
		h.incompat = 0;
		mhdr_seal(&h);
		master_hdr_rewrite_raw(slot, &h);
	}
	zassert_ok(blob_db_mount());

	const uint64_t id = blob_db_alloc_id();

	memset(big, 0x5a, sizeof(big));
	zassert_equal(blob_db_update(id, big, sizeof(big)), -EINVAL,
		      "a segmented write into a store without the feature");
	zassert_ok(blob_db_update(id, big, sizeof(big) - 1),
		   "a single-slot write was refused");
}
#endif
