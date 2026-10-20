/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authenticated and encrypted storage (CONFIG_BLOB_DB_PROTECT): the test plan
 * of doc/proposals/2026-10-15-blob-db-protection.md §9, items 2-7.
 *
 * The round trips (item 1) are the rest of this suite, and of the contract,
 * container, kvdb and blobfs suites, run in the protected scenarios of their
 * testcase.yaml. This file forges what an attacker with write access to the
 * flash could: a changed payload with its CRC recomputed, a slot moved to
 * another id or taken from another store, forged flags, a bucket sector
 * remapped onto another, a forged header, forged masters. Each must read
 * -EIO -- never the forged bytes, never an older version, never "absent" --
 * and erase nothing. Damage (a CRC that fails) must stay benign.
 *
 * The forgeries write the raw partition at blob_db's own offsets, so they run
 * on the flash_area backend only; the layout below is a private copy of the
 * protected format, so that drift in it fails these tests loudly.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_inspect.h>
#include <app/lib/blob_db_security.h>
#include <app/lib/blob_db_test.h>

#define PR_PARTITION_ID  PARTITION_ID(storage_partition)
#define PR_SECTOR        4096
#define PR_FIRST_BUCKET  3

/* The protected layout, as the proposal specifies it (§3.3, §3.5). */
#define PR_BUCKET_HDR    48
#define PR_HEAD          12
#define PR_TAG           CONFIG_BLOB_DB_PROTECT_TAG_LEN
#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
#define PR_NONCE         12
#else
#define PR_NONCE         0
#endif
#define PR_F_SEALED      0x01
#define PR_F_TOMBSTONE   0x02
#define PR_F_COMPACTED   0x10

struct __packed pr_master {
	uint8_t  magic[4];
	uint8_t  format_major;
	uint8_t  format_minor;
	uint16_t hdr_len;
	uint16_t reserved;
	uint16_t prefix_crc16;
	uint32_t generation;
	uint8_t  state;
	uint16_t cbid;
	uint8_t  suite;
	uint64_t next_id_hint;
	uint64_t seg_owner;
	uint8_t  store_uuid[16];
	uint64_t seq_hint;
	uint32_t hdr_crc32;
	uint8_t  mac[16];
	uint32_t tail_crc32;
};
BUILD_ASSERT(sizeof(struct pr_master) == 84, "protected master layout drift");

/* Events ------------------------------------------------------------------ */

static uint32_t ev_count[2][4];

void blob_db_event(enum blob_db_event_kind kind,
		   enum blob_db_event_layer layer, uint32_t where)
{
	ARG_UNUSED(where);
	ev_count[kind][layer]++;
}

#define TAMPERED(layer)  ev_count[BLOB_DB_EVENT_TAMPERED][BLOB_DB_EVENT_##layer]
#define CORRUPT(layer)   ev_count[BLOB_DB_EVENT_CORRUPT][BLOB_DB_EVENT_##layer]

/* Nonces (AEAD) ----------------------------------------------------------- */

#if defined(CONFIG_BLOB_DB_TEST_PROTECT_HOOKS)
#define NONCE_MAX 4096
static uint8_t nonces[NONCE_MAX][12];
static size_t n_nonces;
static bool nonce_overflow;

void blob_db_test_nonce_used(const uint8_t *nonce)
{
	if (n_nonces < NONCE_MAX) {
		memcpy(nonces[n_nonces++], nonce, 12);
	} else {
		nonce_overflow = true;
	}
}
#endif

/* Raw access -------------------------------------------------------------- */

static uint8_t sector[PR_SECTOR];

static off_t bucket_off(uint16_t bid)
{
	return (off_t)(PR_FIRST_BUCKET + bid) * PR_SECTOR;
}

static uint16_t n_buckets(void)
{
	struct blob_db_inspect_info in;

	zassert_ok(blob_db_inspect_info_get(&in));
	zassert_equal(in.sector_size - in.bucket_capacity, PR_BUCKET_HDR,
		      "bucket header is not the protected one");
	zassert_equal(in.slot_overhead, 14 + PR_TAG + PR_NONCE,
		      "slot overhead drift");
	return in.n_buckets;
}

static void raw_read(off_t off, void *buf, size_t len)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(PR_PARTITION_ID, &fa));
	zassert_ok(flash_area_read(fa, off, buf, len));
	flash_area_close(fa);
}

/* Erase the sector and program @p buf: what an attacker with the flash can
 * do, unlike a bit flip, which only clears bits. */
static void raw_rewrite_sector(off_t off, const void *buf)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(PR_PARTITION_ID, &fa));
	zassert_ok(flash_area_erase(fa, off, PR_SECTOR));
	zassert_ok(flash_area_write(fa, off, buf, PR_SECTOR));
	flash_area_close(fa);
}

static bool raw_programmed(off_t off, size_t len)
{
	uint8_t b[16];

	while (len > 0) {
		const size_t n = MIN(len, sizeof(b));

		raw_read(off, b, n);
		for (size_t i = 0; i < n; i++) {
			if (b[i] != 0xff) {
				return true;
			}
		}
		off += n;
		len -= n;
	}
	return false;
}

/* Where the newest slot of @p id sits in its bucket. */
struct slot_loc {
	uint64_t id;
	uint32_t off;
	uint32_t size;
	uint16_t val_len;
	uint8_t  flags;
	bool     found;
};

static int find_cb(const struct blob_db_inspect_slot *s, void *user)
{
	struct slot_loc *loc = user;

	if (s->id == loc->id) {
		loc->off = s->offset;
		loc->size = s->size;
		loc->val_len = s->val_len;
		loc->flags = s->flags;
		loc->found = true;
	}
	return 0;
}

static struct slot_loc locate(uint64_t id)
{
	struct slot_loc loc = { .id = id };
	struct blob_db_inspect_bucket bs;

	zassert_ok(blob_db_inspect_bucket_get((uint16_t)(id % n_buckets()), &bs,
					      find_cb, &loc));
	zassert_true(loc.found, "no slot for id %llu", (unsigned long long)id);
	return loc;
}

static size_t crc_off(uint8_t flags, uint16_t val_len)
{
	const size_t nonce = (flags & PR_F_COMPACTED) ? 0 : PR_NONCE;

	return PR_HEAD + nonce + val_len + PR_TAG;
}

/* Recompute the CRC of the slot at @p off of the staged sector, as a forger
 * would, so the slot reads as intact and only its tag can catch it. */
static void reseal_crc(uint32_t off)
{
	uint8_t *s = &sector[off];
	const uint16_t len = sys_get_le16(&s[2]);
	const size_t at = crc_off(s[0], len);

	sys_put_le16(crc16_ccitt(0xffff, s, at), &s[at]);
}

static void stage_bucket(uint16_t bid)
{
	raw_read(bucket_off(bid), sector, sizeof(sector));
}

static uint64_t put(const void *p, size_t len)
{
	const uint64_t id = blob_db_alloc_id();

	zassert_not_equal(id, 0);
	zassert_ok(blob_db_update(id, p, len));
	return id;
}

/* An id in the same bucket as @p id, allocated after it. */
static uint64_t alloc_same_bucket(uint64_t id)
{
	const uint16_t n = n_buckets();
	uint64_t other;

	do {
		other = blob_db_alloc_id();
		zassert_not_equal(other, 0);
	} while (other % n != id % n);
	return other;
}

static void expect(uint64_t id, const char *want)
{
	uint8_t buf[32];
	size_t got = 0;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got),
		   "id %llu unreadable", (unsigned long long)id);
	zassert_equal(got, strlen(want));
	zassert_mem_equal(buf, want, got);
}

static void expect_eio(uint64_t id)
{
	uint8_t buf[32];
	size_t got = 0;

	zassert_equal(blob_db_get(id, buf, sizeof(buf), &got), -EIO,
		      "id %llu must read -EIO", (unsigned long long)id);
	zassert_equal(blob_db_size(id, &got), -EIO);
	zassert_equal(blob_db_read(id, 0, buf, sizeof(buf), &got), -EIO);
}

/* Fixture ----------------------------------------------------------------- */

static void erase_masters(void)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(PR_PARTITION_ID, &fa));
	zassert_ok(flash_area_erase(fa, 0, 3 * PR_SECTOR));
	flash_area_close(fa);
}

static void pr_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	if (blob_db_mount() != 0) {
		erase_masters();
		zassert_ok(blob_db_mount());
	}
	zassert_ok(blob_db_format());
	memset(ev_count, 0, sizeof(ev_count));
}

static void pr_after(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
}

ZTEST_SUITE(blob_db_protect, NULL, NULL, pr_before, pr_after, NULL);

#define SKIP_UNLESS_RAW() Z_TEST_SKIP_IFNDEF(CONFIG_BLOB_DB_BACKEND_FLASH_AREA)

static int count_cb(uint64_t id, const void *p, size_t len, void *user)
{
	ARG_UNUSED(id);
	ARG_UNUSED(p);
	ARG_UNUSED(len);
	(*(int *)user)++;
	return 0;
}

/* 2. Slot tampering ------------------------------------------------------- */

/* A changed payload byte with the CRC recomputed: -EIO, TAMPERED, and no
 * fallback to the older version underneath. */
ZTEST(blob_db_protect, test_tampered_payload_reads_eio_not_older)
{
	SKIP_UNLESS_RAW();

	const uint64_t id = put("old value", 9);

	zassert_ok(blob_db_update(id, "new value", 9));

	const struct slot_loc loc = locate(id);
	const uint16_t bid = (uint16_t)(id % n_buckets());

	zassert_ok(blob_db_unmount());
	stage_bucket(bid);
	sector[loc.off + PR_HEAD + PR_NONCE] ^= 0x01;
	reseal_crc(loc.off);
	raw_rewrite_sector(bucket_off(bid), sector);
	zassert_ok(blob_db_mount());

	expect_eio(id);
	zassert_false(blob_db_exists(id));
	zassert_true(TAMPERED(SLOT) > 0, "no TAMPERED event");
}

/* The same slot bytes under another id of the same bucket. */
ZTEST(blob_db_protect, test_slot_moved_to_another_id_reads_eio)
{
	SKIP_UNLESS_RAW();

	const uint64_t a = put("secret-a", 8);
	const uint64_t z = alloc_same_bucket(a);
	const struct slot_loc loc = locate(a);
	const uint16_t bid = (uint16_t)(a % n_buckets());

	zassert_ok(blob_db_unmount());
	stage_bucket(bid);
	sys_put_le64(z, &sector[loc.off + 4]);
	reseal_crc(loc.off);
	raw_rewrite_sector(bucket_off(bid), sector);
	zassert_ok(blob_db_mount());

	expect_eio(z);
	zassert_true(TAMPERED(SLOT) > 0);
}

/* A slot written by another store (another uuid, so other keys), appended
 * where this store's next slot for the id would go. */
ZTEST(blob_db_protect, test_slot_from_another_store_reads_eio)
{
	SKIP_UNLESS_RAW();

	const uint64_t id = put("other store", 11);
	const struct slot_loc theirs = locate(id);
	const uint16_t bid = (uint16_t)(id % n_buckets());
	uint8_t slot[64];

	zassert_true(theirs.size <= sizeof(slot));
	stage_bucket(bid);
	memcpy(slot, &sector[theirs.off], theirs.size);

	/* A new store incarnation, holding the same id. */
	zassert_ok(blob_db_format());
	zassert_equal(put("this store", 10), id, "need the same id again");

	const struct slot_loc ours = locate(id);

	zassert_ok(blob_db_unmount());
	stage_bucket(bid);
	memcpy(&sector[ours.off + ours.size], slot, theirs.size);
	raw_rewrite_sector(bucket_off(bid), sector);
	zassert_ok(blob_db_mount());

	expect_eio(id);
}

/* A live slot turned into a tombstone, CRC recomputed: not "deleted". */
ZTEST(blob_db_protect, test_forged_tombstone_reads_eio)
{
	SKIP_UNLESS_RAW();

	const uint64_t id = put("alive", 5);
	const struct slot_loc loc = locate(id);
	const uint16_t bid = (uint16_t)(id % n_buckets());
	int visited = 0;

	zassert_ok(blob_db_unmount());
	stage_bucket(bid);
	sector[loc.off] |= PR_F_TOMBSTONE;
	reseal_crc(loc.off);
	raw_rewrite_sector(bucket_off(bid), sector);
	zassert_ok(blob_db_mount());

	expect_eio(id);
	zassert_equal(blob_db_delete(id), -EIO);

	/* iterate visits the rest (the root), and says it could not read
	 * everything. */
	zassert_equal(blob_db_iterate(count_cb, &visited), -EIO);
	zassert_equal(visited, 1);
}

#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
/* COMPACTED set on an appended slot: its nonce becomes payload and its size
 * shrinks. With the CRC recomputed over the new parse, the tag catches it. */
ZTEST(blob_db_protect, test_flipped_compacted_flag_reads_eio)
{
	SKIP_UNLESS_RAW();

	const uint64_t id = put("appended slot", 13);
	const struct slot_loc loc = locate(id);
	const uint16_t bid = (uint16_t)(id % n_buckets());

	zassert_ok(blob_db_unmount());
	stage_bucket(bid);
	sector[loc.off] |= PR_F_COMPACTED;
	reseal_crc(loc.off);
	raw_rewrite_sector(bucket_off(bid), sector);
	zassert_ok(blob_db_mount());

	expect_eio(id);
}
#endif

/* 3. Damage stays benign -------------------------------------------------- */

/* A rotten newest slot (CRC fails) falls back to the older version, as a torn
 * write must, and is reported as damage, not tampering. */
ZTEST(blob_db_protect, test_rotten_slot_falls_back_with_corrupt_event)
{
	SKIP_UNLESS_RAW();

	const uint64_t id = put("v1", 2);

	zassert_ok(blob_db_update(id, "v2", 2));

	const struct slot_loc loc = locate(id);
	const uint16_t bid = (uint16_t)(id % n_buckets());

	zassert_ok(blob_db_unmount());
	stage_bucket(bid);
	sector[loc.off + PR_HEAD + PR_NONCE] ^= 0x01;   /* CRC left as it was */
	raw_rewrite_sector(bucket_off(bid), sector);
	zassert_ok(blob_db_mount());

	expect(id, "v1");
	zassert_true(CORRUPT(SLOT) > 0, "no CORRUPT event");
	zassert_equal(TAMPERED(SLOT), 0, "damage reported as tampering");
}

/* 4. Remapping ------------------------------------------------------------ */

/* A whole bucket sector copied over another: -EIO for the victim on get and
 * update, and nothing erased. */
ZTEST(blob_db_protect, test_remapped_bucket_reads_eio_and_survives)
{
	SKIP_UNLESS_RAW();

	const uint64_t a = put("bucket a", 8);
	const uint64_t c = put("bucket c", 8);
	const uint16_t n = n_buckets();

	zassert_ok(blob_db_unmount());
	stage_bucket((uint16_t)(a % n));
	raw_rewrite_sector(bucket_off((uint16_t)(c % n)), sector);
	zassert_ok(blob_db_mount());

	expect(a, "bucket a");
	expect_eio(c);
	zassert_equal(blob_db_update(c, "x", 1), -EIO);
	zassert_ok(blob_db_unmount());
	zassert_true(raw_programmed(bucket_off((uint16_t)(c % n)) + PR_BUCKET_HDR,
				    PR_HEAD),
		     "the remapped bucket must not be erased");
	zassert_ok(blob_db_mount());
}

/* The same, with the header's bucket id and CRC forged to match: only the MAC
 * can tell, and an absent id must not read "absent". */
ZTEST(blob_db_protect, test_forged_bucket_header_reads_eio)
{
	SKIP_UNLESS_RAW();

	const uint64_t a = put("bucket a", 8);
	const uint64_t c = put("bucket c", 8);
	const uint64_t absent = alloc_same_bucket(c);
	const uint16_t n = n_buckets();
	const uint16_t cb = (uint16_t)(c % n);

	zassert_ok(blob_db_unmount());
	stage_bucket((uint16_t)(a % n));
	sys_put_le16(cb, &sector[4]);
	sys_put_le32(crc32_ieee(sector, PR_BUCKET_HDR - 4),
		     &sector[PR_BUCKET_HDR - 4]);
	raw_rewrite_sector(bucket_off(cb), sector);
	zassert_ok(blob_db_mount());

	expect_eio(c);
	expect_eio(absent);
	zassert_equal(blob_db_update(c, "x", 1), -EIO);
	zassert_true(TAMPERED(BUCKET) > 0);
}

/* A protected store does not take a zeroed magic for "unformatted". */
ZTEST(blob_db_protect, test_zeroed_bucket_magic_is_not_unformatted)
{
	SKIP_UNLESS_RAW();

	const uint64_t id = put("keep me", 7);
	const uint16_t bid = (uint16_t)(id % n_buckets());
	const struct flash_area *fa;
	const uint8_t zero[4] = { 0 };

	zassert_ok(blob_db_unmount());
	zassert_ok(flash_area_open(PR_PARTITION_ID, &fa));
	zassert_ok(flash_area_write(fa, bucket_off(bid), zero, sizeof(zero)));
	flash_area_close(fa);
	zassert_ok(blob_db_mount());

	expect_eio(id);
	zassert_equal(blob_db_update(id, "x", 1), -EIO);
}

/* erase_all erases the buckets it retires, rather than zeroing magics. */
ZTEST(blob_db_protect, test_erase_all_erases_buckets)
{
	SKIP_UNLESS_RAW();

	const uint64_t id = put("gone", 4);
	const uint16_t bid = (uint16_t)(id % n_buckets());

	zassert_ok(blob_db_erase_all());
	zassert_ok(blob_db_unmount());
	zassert_false(raw_programmed(bucket_off(bid), PR_BUCKET_HDR + PR_HEAD),
		      "a retired bucket must be erased");
	zassert_ok(blob_db_mount());
	zassert_false(blob_db_exists(id));
}

/* 5. Masters -------------------------------------------------------------- */

static void master_read(uint8_t slot, struct pr_master *m)
{
	raw_read((off_t)slot * PR_SECTOR, m, sizeof(*m));
}

static void master_write(uint8_t slot, const struct pr_master *m)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(PR_PARTITION_ID, &fa));
	zassert_ok(flash_area_erase(fa, (off_t)slot * PR_SECTOR, PR_SECTOR));
	zassert_ok(flash_area_write(fa, (off_t)slot * PR_SECTOR, m, sizeof(*m)));
	flash_area_close(fa);
}

/* Every CRC a forger can compute; the MAC stays as it was. */
static void master_crcs(struct pr_master *m)
{
	m->prefix_crc16 = crc16_ccitt(0xffff, (const uint8_t *)m,
				      offsetof(struct pr_master, prefix_crc16));
	m->hdr_crc32 = crc32_ieee((const uint8_t *)m,
				  offsetof(struct pr_master, hdr_crc32));
	m->tail_crc32 = crc32_ieee((const uint8_t *)m,
				   offsetof(struct pr_master, tail_crc32));
}

/* Push the store through enough ceiling writes that both masters are valid,
 * and return which one is newer. */
static uint8_t two_masters(void)
{
	struct pr_master a, b;

	for (int i = 0; i < 600; i++) {
		zassert_not_equal(blob_db_alloc_id(), 0);
	}
	master_read(0, &a);
	master_read(1, &b);
	zassert_equal(a.format_major, b.format_major, "need two masters");
	return a.generation > b.generation ? 0 : 1;
}

/* A forged newer master that lowers the id ceiling: ignored, TAMPERED, and
 * the other master wins. With one valid master mount scans the buckets, so no
 * bound id is handed out twice (an allocated, unbound one above the older
 * ceiling can be, as after any lost master: §13.1). */
ZTEST(blob_db_protect, test_forged_master_loses_to_the_other)
{
	SKIP_UNLESS_RAW();

	const uint8_t newer = two_masters();
	const uint64_t last = put("last", 4);
	struct pr_master m;

	zassert_ok(blob_db_unmount());
	master_read(newer, &m);
	m.generation += 10;
	m.next_id_hint = 2;
	master_crcs(&m);
	master_write(newer, &m);

	memset(ev_count, 0, sizeof(ev_count));
	zassert_ok(blob_db_mount());
	zassert_true(TAMPERED(MASTER) > 0, "no TAMPERED event");
	zassert_true(blob_db_alloc_id() > last, "an id was reissued");
}

/* Both masters forged: -EIO, and nothing formatted. */
ZTEST(blob_db_protect, test_both_masters_forged_are_refused)
{
	SKIP_UNLESS_RAW();

	(void)two_masters();
	zassert_ok(blob_db_unmount());

	for (uint8_t slot = 0; slot < 2; slot++) {
		struct pr_master m;

		master_read(slot, &m);
		m.mac[0] ^= 0xff;
		master_crcs(&m);
		master_write(slot, &m);
	}

	zassert_equal(blob_db_mount(), -EIO);
	zassert_true(raw_programmed(0, 4) && raw_programmed(PR_SECTOR, 4),
		     "the masters must be left alone");
	erase_masters();
}

/* An unprotected store (major 1) is refused, never adopted: its slots carry
 * no tag, so adopting it would be a downgrade. */
ZTEST(blob_db_protect, test_unprotected_store_is_refused)
{
	SKIP_UNLESS_RAW();

	struct pr_master m;

	zassert_ok(blob_db_unmount());
	master_read(0, &m);
	m.format_major = 1;
	m.hdr_len = 64;
	m.suite = 0;
	master_crcs(&m);
	master_write(0, &m);
	master_write(1, &m);

	zassert_equal(blob_db_mount(), -ENOTSUP);
	erase_masters();
}

/* Another mode or tag length is another format. */
ZTEST(blob_db_protect, test_other_suite_is_refused)
{
	SKIP_UNLESS_RAW();

	struct pr_master m;

	zassert_ok(blob_db_unmount());
	master_read(0, &m);
	m.suite ^= 0x03;   /* AUTH <-> AEAD */
	master_crcs(&m);
	master_write(0, &m);

	zassert_equal(blob_db_mount(), -ENOTSUP);
	erase_masters();
}

/* 7. Compaction with a tampered survivor --------------------------------- */

ZTEST(blob_db_protect, test_compaction_keeps_a_tampered_slot_unreadable)
{
	SKIP_UNLESS_RAW();

	const uint64_t x = put("victim", 6);
	const uint64_t y = alloc_same_bucket(x);
	const uint16_t bid = (uint16_t)(x % n_buckets());
	const struct slot_loc loc = locate(x);
	struct blob_db_inspect_bucket bs;
	char val[16];

	zassert_ok(blob_db_update(y, "y0", 2));
	zassert_ok(blob_db_unmount());
	stage_bucket(bid);
	sector[loc.off + PR_HEAD + PR_NONCE] ^= 0x01;
	reseal_crc(loc.off);
	raw_rewrite_sector(bucket_off(bid), sector);
	zassert_ok(blob_db_mount());

	zassert_ok(blob_db_inspect_bucket_get(bid, &bs, NULL, NULL));

	const uint32_t gen = bs.gen;

	for (int i = 0; i < 400 && bs.gen == gen; i++) {
		(void)snprintk(val, sizeof(val), "y%04d", i);
		zassert_ok(blob_db_update(y, val, 5));
		zassert_ok(blob_db_inspect_bucket_get(bid, &bs, NULL, NULL));
	}
	zassert_true(bs.gen > gen, "the bucket never compacted");

	expect_eio(x);
	expect(y, val);
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	expect_eio(x);
	expect(y, val);
}

/* 6. Nonces --------------------------------------------------------------- */

#if defined(CONFIG_BLOB_DB_TEST_PROTECT_HOOKS)
static int nonce_cmp(const void *a, const void *b)
{
	return memcmp(a, b, 12);
}

/* No nonce, and no counter value whatever the salt, is used twice: across
 * remounts (each one as abrupt as a power cut, since blob_db writes nothing
 * at unmount), ceiling writes, and repeated compactions of one bucket. */
ZTEST(blob_db_protect, test_no_nonce_is_used_twice)
{
	const uint64_t x = put("x", 1);
	const uint64_t y = alloc_same_bucket(x);
	char val[64];

	n_nonces = 0;
	nonce_overflow = false;
	memset(val, 'v', sizeof(val));

	for (int round = 0; round < 8; round++) {
		for (int i = 0; i < 150; i++) {
			zassert_ok(blob_db_update(i & 1 ? x : y, val,
						  sizeof(val)));
		}
		zassert_ok(blob_db_unmount());
		zassert_ok(blob_db_mount());
	}
	zassert_false(nonce_overflow, "raise NONCE_MAX");
	zassert_true(n_nonces > 1000, "too few nonces recorded: %zu", n_nonces);

	/* Whole nonces. */
	qsort(nonces, n_nonces, 12, nonce_cmp);
	for (size_t i = 1; i < n_nonces; i++) {
		zassert_true(memcmp(nonces[i - 1], nonces[i], 12) != 0,
			     "nonce used twice");
	}
	/* Counter values alone: the ceiling, not the salt, keeps them apart. */
	for (size_t i = 0; i < n_nonces; i++) {
		memset(nonces[i], 0, 4);
	}
	qsort(nonces, n_nonces, 12, nonce_cmp);
	for (size_t i = 1; i < n_nonces; i++) {
		zassert_true(memcmp(nonces[i - 1], nonces[i], 12) != 0,
			     "counter value %llu used twice",
			     (unsigned long long)sys_get_le64(&nonces[i][4]));
	}

	uint8_t buf[sizeof(val)];
	size_t got = 0;

	zassert_ok(blob_db_get(x, buf, sizeof(buf), &got));
	zassert_equal(got, sizeof(val));
	zassert_mem_equal(buf, val, sizeof(val));
}
#endif
