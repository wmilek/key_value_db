/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db — stable-id blob storage on flash_area.
 *
 * Stage 3: mount, unmount, master sector + bucket scan.
 * Put/get/update/delete arrive in stage 4. Real compaction recovery in
 * stage 5.
 *
 * See doc/layers/l1_blob_db.md for the on-flash format and algorithms.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_iostats.h>
#include <app/lib/blob_db_test.h>

#include "blob_db_internal.h"
#include "blob_db_slot_index.h"
#include "blob_db_store.h"

LOG_MODULE_REGISTER(blob_db, CONFIG_BLOB_DB_LOG_LEVEL);

/* On-flash magic strings, 4 bytes each (no NUL terminator). */
static const uint8_t MASTER_MAGIC[4] = { 'B', 'D', 'M', 'S' };
static const uint8_t BUCKET_MAGIC[4] = { 'B', 'D', 'B', 'H' };
static const uint8_t SCRATCH_MAGIC[4] = { 'B', 'D', 'S', 'L' };

/* Forward declarations (compaction helpers live below blob_db_mount). */
static int recover_compaction(void);
static int compact_bucket(uint16_t bid);
static int persist_next_id_hint(void);
static int bind_root_empty(void);
static int invalidate_buckets(int skip_bid);
static int format_whole_store(void);
static bool bucket_hdr_valid(const uint8_t *buf, uint16_t bid);

/* The on-flash prefix of a slot: header then id, contiguous. These 12 bytes
 * are enough to size a slot and know whose it is, which is what lets the walk
 * step over a slot without reading its payload. */
struct __packed slot_head {
	struct blob_db_slot_hdr hdr;
	uint64_t id;
};
BUILD_ASSERT(sizeof(struct slot_head) == 12, "slot_head layout drift");

/* Result of one streaming pass over a bucket. */
struct bucket_scan {
	off_t            write_cursor;  /* end of log — where an append goes   */
	off_t            target_off;    /* < 0 when the id has no slot here    */
	struct slot_head target;        /* the target's on-flash bytes         */
	bool             from_index;    /* cursor is the slot index's, not a
					 * walk's: see confirm_cursor()       */
	bool             tail_torn;     /* the walk stopped at a programmed
					 * header it could not trust: a torn
					 * slot sits at write_cursor          */
	bool             foreign;       /* the walk stopped at an intact slot
					 * written by a build that knows a flag
					 * this one does not: see
					 * slot_foreign_head()                */
};

static int read_bucket_hdr(uint16_t bid, bool *valid);
static int scan_bucket_for(uint16_t bid, uint64_t target_id, off_t limit,
			   struct bucket_scan *out);
static int slot_verify(uint16_t bid, const struct bucket_scan *sc, void *dst);
static int find_live_slot(uint16_t bid, uint64_t id, struct bucket_scan *sc,
			  void *dst);

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
/* Large-object entry points needed before their definitions. */
static int seg_geometry_init(void);
static int seg_sweep(uint64_t owner);
static int pwrite_segmented(uint64_t id, size_t offset, const void *buf,
			    size_t len);
static inline void idx_invalidate(void);

/* Drop the cached index. Spelled as a macro so the call sites — which sit in
 * code compiled with and without segmentation — need no #ifdef of their own. */
#define IDX_INVALIDATE()  idx_invalidate()
#else
#define IDX_INVALIDATE()  do { } while (0)
#endif

/* Upper bound on the sector size we can hold in RAM. mount refuses larger
 * partitions. Two full-sector buffers live in .bss (below); every public call
 * holds g_lock for its whole duration, so sharing them across calls is safe. */
#define BLOB_DB_SECTOR_BUF_MAX CONFIG_BLOB_DB_SECTOR_BUF_SIZE

/* Largest write unit blob_db stages for: slots and master records are padded
 * to it. Open refuses a device with a larger one. */
#define BLOB_DB_MAX_WRITE_ALIGN 32

/* One master record, padded to the largest write unit. */
#define BLOB_DB_MASTER_REC_MAX                                                 \
	ROUND_UP(sizeof(struct blob_db_master_rec), BLOB_DB_MAX_WRITE_ALIGN)

/* Sector-sized scratch buffers. g_bbuf is used by every read-a-bucket op
 * (mount, get, update, delete, exists, count, iterate) and by compaction
 * as the "old" image; g_bbuf_new is used by compaction as the "new" image.
 *
 * Every buffer handed to the store is word-aligned: nrf_qspi_nor DMAs only
 * from aligned RAM and otherwise copies through a 4-byte stack buffer
 * (CONFIG_NORDIC_QSPI_NOR_STACK_WRITE_BUFFER_SIZE), one program per word.
 * On the nRF5340-DK an unaligned g_bbuf_new made writes 4.3x slower. */
static uint8_t __aligned(4) g_bbuf[BLOB_DB_SECTOR_BUF_MAX];
static uint8_t __aligned(4) g_bbuf_new[BLOB_DB_SECTOR_BUF_MAX];

/* Set while compaction owns g_bbuf_new, so append_slot() — which also stages
 * there — can assert the two never overlap. Debug builds only; release
 * compiles the assert away and this stays a dead store. */
static bool g_bbuf_new_busy;

/* Serializes every public call (contract §5). Recursive, so a public call
 * made from inside another — an iterate callback, or the library calling its
 * own API — does not deadlock. Everything below, the sector buffers included,
 * is touched only with it held. */
static K_MUTEX_DEFINE(g_lock);

void blob_db_lock(void)
{
	(void)k_mutex_lock(&g_lock, K_FOREVER);
}

void blob_db_unlock(void)
{
	(void)k_mutex_unlock(&g_lock);
}

/* Single global instance, guarded by g_lock. */
static struct {
	bool      mounted;
	size_t    fa_size;         /* total addressable bytes (n_pebs * peb_size) */
	size_t    peb_size;
	size_t    write_align;     /* backing flash write-block-size */
	uint16_t  n_pebs;
	uint16_t  n_buckets;
	uint8_t   active_master;   /* 0 or 1 */
	uint32_t  rot_gen;         /* the active master sector's header */
	uint16_t  rec_size;        /* R: one master record, write_align-rounded */
	uint16_t  log_cap;         /* record slots per master sector */
	uint16_t  log_next;        /* the next record slot of the active sector */
	bool      log_failed;      /* a record write failed: rotate next (§3.3) */
	bool      ceiling_reset;   /* erase_all's record lowers the ceiling (§3.3) */
	uint32_t  incompat;        /* the store's feature sets: the union of */
	uint32_t  ro_compat;       /* every ok header, copied at rotation */
	uint32_t  compat;
	uint32_t  fuses;           /* opaque; carried into every record (§4.5) */
	uint64_t  next_id;         /* RAM: next id alloc_id will hand out */
	uint64_t  next_id_hint;    /* durable leading ceiling (see internal.h) */
	uint64_t  seg_owner;       /* segmented write in flight; 0 = none */
	bool      wedged;          /* a torn compaction makes further writes unsafe */
	bool      preformat;       /* backend wants fresh buckets formatted ahead */
	uint16_t  maint_start;     /* first bucket blob_db_maintain() looks at */
	uint16_t  maint_scanned;   /* buckets it has looked at since */
} st;

static inline off_t peb_offset(uint16_t peb)
{
	return (off_t)peb * (off_t)st.peb_size;
}

/* CRC32-IEEE over the leading (sizeof - 4) bytes of a header struct;
 * the trailing 4 bytes hold the CRC. */
static uint32_t hdr_crc32(const void *buf, size_t total)
{
	return crc32_ieee(buf, total - sizeof(uint32_t));
}

#if defined(CONFIG_BLOB_DB_IOSTATS)
static struct blob_db_iostats g_io;

void blob_db_io_note(enum blob_db_io_op op, size_t bytes)
{
	switch (op) {
	case BLOB_DB_IO_READ:
		g_io.reads++;
		g_io.bytes_read += bytes;
		break;
	case BLOB_DB_IO_WRITE:
		g_io.writes++;
		g_io.bytes_written += bytes;
		break;
	case BLOB_DB_IO_ERASE:
		g_io.erases++;
		g_io.bytes_erased += bytes;
		break;
	}
}

void blob_db_iostats_get(struct blob_db_iostats *out)
{
	if (out) {
		blob_db_lock();
		*out = g_io;
		blob_db_unlock();
	}
}

void blob_db_iostats_reset(void)
{
	blob_db_lock();
	memset(&g_io, 0, sizeof(g_io));
	blob_db_unlock();
}

/* Cause counters: why the flash counters moved, not by how much. */
#define IO_CAUSE(field)            (g_io.field++)
#define IO_CAUSE_BYTES(field, n)   (g_io.field += (n))
#else
#define IO_CAUSE(field)            ((void)0)
#define IO_CAUSE_BYTES(field, n)   ((void)0)
#endif /* CONFIG_BLOB_DB_IOSTATS */

/* Master sector helpers --------------------------------------------------
 *
 * Major 5 (doc/proposals/2026-10-28-master-log.md §3): each master sector is
 * a header written once per erase and an append-only log of fixed-size
 * records, each a full copy of the mutable state. An update programs one
 * record and erases nothing; a full sector rotates into the other one.
 */

/* CRC16-CCITT over the frozen part of the compatibility prefix (everything
 * before prefix_crc16 itself). */
static uint16_t prefix_crc(const void *buf)
{
	return crc16_ccitt(0xffff, (const uint8_t *)buf,
			   offsetof(struct blob_db_compat_hdr, prefix_crc16));
}

/* How a master sector's header presents itself. The distinction between
 * FOREIGN and CORRUPT is the whole point of the separate prefix CRC (see
 * blob_db_internal.h): a newer writer produces a prefix that verifies, so we
 * can trust its version and refuse on purpose; bit rot produces one that does
 * not, and the other master may still be good. */
enum master_class {
	MASTER_ERASED,   /* header window is all 0xff — never written */
	MASTER_CORRUPT,  /* written, but unparseable — treat as absent */
	MASTER_FOREIGN,  /* parses, and says it is a format we do not know */
	MASTER_OK,
};

static const char *const master_class_str[] = {
	"erased", "corrupt", "foreign", "ok"
};

static bool all_erased(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (buf[i] != 0xff) {
			return false;
		}
	}
	return true;
}

static uint32_t master_hdr_crc(const struct blob_db_master_hdr *h)
{
	return crc32_ieee((const uint8_t *)h,
			  offsetof(struct blob_db_master_hdr, hdr_crc32));
}

/* A record's CRC covers LE32(rot_gen) of its header too (D16). */
static uint32_t master_rec_crc(uint32_t rot_gen,
			       const struct blob_db_master_rec *r)
{
	uint8_t gen[4];

	sys_put_le32(rot_gen, gen);
	return crc32_ieee_update(crc32_ieee(gen, sizeof(gen)),
				 (const uint8_t *)r,
				 offsetof(struct blob_db_master_rec, crc32));
}

/* Read and classify the header of master sector @p slot. */
static int master_classify(uint8_t slot, struct blob_db_master_hdr *out,
			   enum master_class *cls)
{
	uint8_t __aligned(4) buf[BLOB_DB_MASTER_HDR_MAX];

	*cls = MASTER_CORRUPT;

	int rc = blob_db_store_read(peb_offset(slot), buf, sizeof(buf));

	if (rc < 0) {
		LOG_ERR("master %u read failed: %d", slot, rc);
		return rc;
	}

	/* Erased means virgin, and only virgin may be formatted. Check the
	 * whole staging window, not just the prefix, so a sector holding
	 * anything at all is never mistaken for untouched flash. */
	if (all_erased(buf, sizeof(buf))) {
		*cls = MASTER_ERASED;
		return 0;
	}

	const struct blob_db_compat_hdr *c =
		(const struct blob_db_compat_hdr *)buf;

	if (c->prefix_crc16 != prefix_crc(buf)) {
		LOG_WRN("master %u: prefix CRC bad — treating as corrupt", slot);
		return 0;   /* MASTER_CORRUPT */
	}

	/* From here the prefix is trustworthy, so anything we reject is a
	 * deliberate refusal rather than a guess. */
	if (memcmp(c->magic, MASTER_MAGIC, 4) != 0) {
		LOG_ERR("master %u: foreign magic %02x%02x%02x%02x",
			slot, c->magic[0], c->magic[1], c->magic[2], c->magic[3]);
		*cls = MASTER_FOREIGN;
		return 0;
	}
	/* Exactly our major: 1-4 are refused, never migrated (D10). */
	if (c->format_major != BLOB_DB_FORMAT_MAJOR) {
		LOG_ERR("master %u: format major %u, this build reads %u",
			slot, c->format_major, BLOB_DB_FORMAT_MAJOR);
		*cls = MASTER_FOREIGN;
		return 0;
	}
	if (c->hdr_len < sizeof(struct blob_db_master_hdr) ||
	    c->hdr_len > BLOB_DB_MASTER_HDR_MAX) {
		LOG_ERR("master %u: hdr_len %u outside [%zu, %u]", slot,
			c->hdr_len, sizeof(struct blob_db_master_hdr),
			BLOB_DB_MASTER_HDR_MAX);
		*cls = MASTER_FOREIGN;
		return 0;
	}
	/* format_minor is ignored (D15): what a store needs is in its
	 * feature sets, which master_accept() checks. */

	memcpy(out, buf, sizeof(*out));
	if (out->hdr_crc32 != master_hdr_crc(out)) {
		LOG_WRN("master %u: header CRC mismatch — treating as corrupt",
			slot);
		return 0;   /* MASTER_CORRUPT */
	}
	*cls = MASTER_OK;
	return 0;
}

/* Whether this build and this device can use a store whose master sector
 * @p slot carries header @p h (§3.1, §4.4). Checked on every ok header, usable
 * or not, before anything is written. */
static int master_accept(uint8_t slot, const struct blob_db_master_hdr *h)
{
	const char *what = NULL;

	if (h->incompat & ~BLOB_DB_BUILD_INCOMPAT) {
		what = "an incompat feature this build does not know";
	} else if (h->ro_compat & ~BLOB_DB_BUILD_RO_COMPAT) {
		/* No read-only mount yet: refused like incompat (§4.1). */
		what = "a ro_compat feature this build does not know";
	} else if ((BLOB_DB_BUILD_REQUIRED & ~h->incompat) != 0) {
		what = "no feature this build requires";
	} else if (h->suite != BLOB_DB_BUILD_SUITE ||
		   h->prefix.hdr_len != BLOB_DB_BUILD_HDR_LEN) {
		what = "another protection suite";
	} else if (h->peb_size != st.peb_size || h->n_buckets != st.n_buckets) {
		what = "another geometry";
	} else if (h->rec_size != st.rec_size) {
		what = "another record size";
	} else if (h->max_payload_len != CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
		what = "another CONFIG_BLOB_DB_MAX_PAYLOAD_LEN";
	} else if (h->write_align != st.write_align) {
		what = "another write alignment";
	} else {
		return 0;
	}
	LOG_ERR("master %u: the store has %s (incompat 0x%x ro_compat 0x%x "
		"suite %u, %u B x %u buckets, R %u, cap %u, align %u); refusing",
		slot, what, h->incompat, h->ro_compat, h->suite, h->peb_size,
		h->n_buckets, h->rec_size, h->max_payload_len, h->write_align);
	return -ENOTSUP;
}

/* What a sector's record log holds (§3.5): end is one past the last slot that
 * is not fully erased, newest the newest record below it that verifies
 * (-1: none), and tail_bad whether the slot just below end fails. Reads the
 * whole sector into g_bbuf: every slot is read (D3). */
struct master_log {
	uint16_t end;
	int      newest;
	bool     tail_bad;
	struct blob_db_master_rec rec;
};

static bool master_rec_ok(uint32_t rot_gen, const uint8_t *slot, uint16_t i,
			  struct blob_db_master_rec *out)
{
	memcpy(out, slot, sizeof(*out));
	return out->rec_no == i && out->crc32 == master_rec_crc(rot_gen, out);
}

static int master_log_load(uint8_t slot, uint32_t rot_gen,
			   struct master_log *log)
{
	int rc = blob_db_store_read(peb_offset(slot), g_bbuf, st.peb_size);

	if (rc < 0) {
		return rc;
	}

	const uint8_t *base = g_bbuf + BLOB_DB_MASTER_LOG_OFF;

	log->end = 0;
	for (uint16_t i = 0; i < st.log_cap; i++) {
		if (!all_erased(base + (size_t)i * st.rec_size, st.rec_size)) {
			log->end = i + 1;
		}
	}
	log->newest = -1;
	log->tail_bad = false;
	for (int i = (int)log->end - 1; i >= 0; i--) {
		if (master_rec_ok(rot_gen, base + (size_t)i * st.rec_size,
				  (uint16_t)i, &log->rec)) {
			log->newest = i;
			break;
		}
		if (i == (int)log->end - 1) {
			log->tail_bad = true;
		}
	}
	return 0;
}

/* The record the next master update writes: the session's state, with the
 * ceiling carried forward. Every record raises the id ceiling to STEP past the
 * next id (D14), so allocation rarely needs a record of its own; the max keeps
 * it monotonic. erase_all's record is the one that lowers it (§3.3). */
static void master_rec_fill(struct blob_db_master_rec *r)
{
	const uint64_t want = st.next_id + 1 + BLOB_DB_ID_HINT_STEP;

	memset(r, 0, sizeof(*r));
	r->next_id_hint = st.ceiling_reset ? want : MAX(st.next_id_hint, want);
	r->seq_hint     = 0;   /* unprotected */
	r->seg_owner    = st.seg_owner;
	r->fuses        = st.fuses;
}

static void master_rec_stage(uint8_t *buf, struct blob_db_master_rec *r,
			     uint32_t rec_no, uint32_t rot_gen)
{
	r->rec_no = rec_no;
	r->crc32  = master_rec_crc(rot_gen, r);
	memset(buf, 0xff, st.rec_size);
	memcpy(buf, r, sizeof(*r));
}

/* Replace master sector @p slot with a header at @p rot_gen and record[0]
 * holding @p r. The header carries the session's feature sets, which hold
 * every ok header's bits, unknown ones included (§4.2). */
static int master_write_sector(uint8_t slot, uint32_t rot_gen,
			       struct blob_db_master_rec *r)
{
	uint8_t __aligned(4) buf[BLOB_DB_MASTER_LOG_OFF + BLOB_DB_MASTER_REC_MAX];
	struct blob_db_master_hdr *h = (struct blob_db_master_hdr *)buf;

	memset(buf, 0xff, BLOB_DB_MASTER_LOG_OFF);
	memset(h, 0, sizeof(*h));
	memcpy(h->prefix.magic, MASTER_MAGIC, 4);
	h->prefix.format_major = BLOB_DB_FORMAT_MAJOR;
	h->prefix.format_minor = BLOB_DB_FORMAT_MINOR;
	h->prefix.hdr_len      = BLOB_DB_BUILD_HDR_LEN;
	h->prefix.prefix_crc16 = prefix_crc(h);
	h->rot_gen         = rot_gen;
	h->peb_size        = (uint32_t)st.peb_size;
	h->n_buckets       = st.n_buckets;
	h->rec_size        = st.rec_size;
	h->incompat        = st.incompat;
	h->ro_compat       = st.ro_compat;
	h->compat          = st.compat;
	h->suite           = BLOB_DB_BUILD_SUITE;
	h->max_payload_len = CONFIG_BLOB_DB_MAX_PAYLOAD_LEN;
	h->write_align     = (uint16_t)st.write_align;
	h->hdr_crc32       = master_hdr_crc(h);

	master_rec_stage(buf + BLOB_DB_MASTER_LOG_OFF, r, 0, rot_gen);

	IO_CAUSE(master_rotations);
	IO_CAUSE(master_records);

	const int rc = blob_db_store_replace(peb_offset(slot), buf,
					     BLOB_DB_MASTER_LOG_OFF + st.rec_size);

	if (rc < 0) {
		LOG_ERR("master %u write failed: %d", slot, rc);
	}
	return rc;
}

/* Rotate into the other sector, carrying @p r as its record[0] (§3.4). */
static int master_rotate(struct blob_db_master_rec *r)
{
	const uint8_t other = !st.active_master;

	if (st.rot_gen == UINT32_MAX) {
		LOG_ERR("master rot_gen would wrap");
		return -ENOSPC;
	}

	const int rc = master_write_sector(other, st.rot_gen + 1, r);

	if (rc < 0) {
		return rc;
	}
	st.active_master = other;
	st.rot_gen++;
	st.log_next = 1;
	st.log_failed = false;
	return 0;
}

/* Commit the session's master state: one record appended to the active
 * sector, or a rotation when the sector is full or a record write has failed
 * (§3.3). A failed append leaves its slot possibly erased, so the log must
 * not continue past it: the next update rotates, and the written slots stay a
 * prefix (§3.2a).
 *
 * With @p force_rotate, it rotates whatever room is left.
 *
 * The only path that writes a master outside a format. A caller sets the st
 * fields it changes, commits, and puts them back if the commit fails, so RAM
 * never holds a state that is not on flash; the ceiling RAM takes is the
 * record's, once it has landed. */
static int master_commit_how(bool force_rotate)
{
	struct blob_db_master_rec r;
	int rc;

	master_rec_fill(&r);
	if (force_rotate || st.log_failed || st.log_next >= st.log_cap) {
		rc = master_rotate(&r);
	} else {
		uint8_t __aligned(4) buf[BLOB_DB_MASTER_REC_MAX];
		const off_t at = peb_offset(st.active_master) +
				 BLOB_DB_MASTER_LOG_OFF +
				 (off_t)st.log_next * st.rec_size;

		master_rec_stage(buf, &r, st.log_next, st.rot_gen);
		IO_CAUSE(master_records);
		rc = blob_db_store_write(at, buf, st.rec_size);
		st.log_next++;   /* also on failure: that slot is not erased */
		if (rc < 0) {
			LOG_ERR("master record write failed: %d; the next update "
				"rotates", rc);
			st.log_failed = true;
		}
	}
	st.ceiling_reset = false;
	if (rc == 0) {
		st.next_id_hint = r.next_id_hint;
	}
	return rc;
}

static int master_commit(void)
{
	return master_commit_how(false);
}

/* A rotation now, whatever room is left: what ends a mount's doubt when the
 * other sector is bad, and what finishes an upgrade (§3.5). */
static int master_commit_rotate(void)
{
	return master_commit_how(true);
}

/* Start the master state over, as a format does (§3.4a, D18): next id 2,
 * since the root (id 1) is consumed at format time (see @blob_db_root in
 * blob_db.h), no segmented write in flight, no fuses (D12), and this build's
 * features (D7). Both sectors are written, B at rot_gen 1 and then A at 2, so
 * that after a format no master sector reads erased except inside a rotation,
 * and mount can treat an erased sector beside an ok one as doubt. The caller
 * has emptied the buckets first. */
static int master_format(void)
{
	struct blob_db_master_rec r;
	int rc;

	st.next_id       = 2;
	st.seg_owner     = 0;
	st.fuses         = 0;
	st.incompat      = BLOB_DB_BUILD_INCOMPAT;
	st.ro_compat     = BLOB_DB_BUILD_RO_COMPAT;
	st.compat        = 0;
	st.ceiling_reset = true;
	master_rec_fill(&r);
	st.ceiling_reset = false;

	rc = master_write_sector(BLOB_DB_MASTER_B_SECTOR, 1, &r);
	if (rc == 0) {
		rc = master_write_sector(BLOB_DB_MASTER_A_SECTOR, 2, &r);
	}
	if (rc < 0) {
		return rc;
	}
	st.active_master = BLOB_DB_MASTER_A_SECTOR;
	st.rot_gen       = 2;
	st.log_next      = 1;
	st.log_failed    = false;
	st.next_id_hint  = r.next_id_hint;
	return 0;
}

/* Persist the id ceiling: one record, which raises it to STEP past the next
 * id. A torn write leaves the old ceiling, which is still a safe — if lower —
 * bound, and doubt for the next mount, which re-raises it from the scan. */
static int persist_next_id_hint(void)
{
	int rc = master_commit();

	if (rc < 0) {
		LOG_ERR("persist next_id_hint failed: %d", rc);
		return rc;
	}
	LOG_DBG("id ceiling raised to %llu (rot_gen %u, record %u)",
		(unsigned long long)st.next_id_hint, st.rot_gen,
		st.log_next - 1);
	return 0;
}

/* Bucket scan ----------------------------------------------------------- */

static int scan_bucket(uint16_t bid, uint64_t *max_id_inout)
{
	const off_t base = peb_offset(BLOB_DB_FIRST_BUCKET + bid);

	struct blob_db_bucket_hdr bhdr;
	int rc = blob_db_store_read(base, &bhdr, sizeof(bhdr));
	if (rc < 0) {
		return rc;
	}

	if (memcmp(bhdr.magic, BUCKET_MAGIC, 4) != 0) {
		return 0; /* unformatted — no entries */
	}
	if (bhdr.hdr_crc32 != hdr_crc32(&bhdr, sizeof(bhdr))) {
		LOG_WRN("bucket %u header CRC bad; ignoring", bid);
		return 0;
	}
	if (bhdr.bucket_id != bid) {
		LOG_WRN("bucket %u header claims bid %u; ignoring", bid, bhdr.bucket_id);
		return 0;
	}

	off_t cursor = BLOB_DB_BUCKET_DATA_OFF;
	const size_t min_slot = sizeof(struct blob_db_slot_hdr) +
				 sizeof(uint64_t) + sizeof(uint16_t);

	blob_db_sidx_begin(bid, (uint32_t)cursor);

	while (cursor + min_slot <= st.peb_size) {
		struct blob_db_slot_hdr shdr;
		rc = blob_db_store_read(base + cursor, &shdr, sizeof(shdr));
		if (rc < 0) {
			return rc;
		}

		/* End-of-log: erased, unsealed, or implausible lengths. */
		if (shdr.flags == 0xff) {
			break;
		}
		if (!(shdr.flags & BLOB_DB_SLOT_F_SEALED)) {
			break;
		}
		if (shdr.val_len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
			LOG_WRN("bucket %u: bad val_len %u at off 0x%lx; truncating",
				bid, shdr.val_len, (unsigned long)cursor);
			break;
		}
		/* scan_bucket_for() ends the log at a flag it does not know;
		 * this scan carries on for the id ceiling's sake. The two then
		 * disagree on where the log ends, so leave the bucket to the
		 * walk rather than index a cursor the walk would not find. */
		if (shdr.flags & ~(uint8_t)BLOB_DB_SLOT_F_KNOWN) {
			blob_db_sidx_forget(bid);
		}
		const size_t base_sz = sizeof(shdr) + sizeof(uint64_t) +
				       shdr.val_len + sizeof(uint16_t);
		const size_t a = st.write_align ? st.write_align : 1;
		const size_t slot_sz = (base_sz + a - 1) & ~(a - 1);
		if (cursor + slot_sz > st.peb_size) {
			break;
		}

		uint64_t id;
		rc = blob_db_store_read(base + cursor + sizeof(shdr),
				     &id, sizeof(id));
		if (rc < 0) {
			return rc;
		}

		/* All ids (live or tombstoned) count toward the high-water mark:
		 * a tombstoned id was once assigned and must not be re-issued. */
		if (id > *max_id_inout) {
			*max_id_inout = id;
		}

		blob_db_sidx_note(bid, id, (uint32_t)cursor, (uint32_t)slot_sz,
				  (shdr.flags & BLOB_DB_SLOT_F_TOMBSTONE) != 0);
		cursor += slot_sz;
	}

	return 0;
}

/* Mount / unmount ------------------------------------------------------- */

/* Format a partition whose masters both read erased.
 *
 * Erased masters do not mean erased buckets: blob_db_format() erases from
 * offset 0 up, so a power cut part way leaves blank masters in front of
 * buckets that still hold blobs — under ids the fresh counter is about to
 * hand out again. Invalidate them first, root bucket included, and only then
 * write the master: a cut before that point leaves the masters blank and the
 * next mount repeats this; after it, nothing is left to collide with. This is
 * what lets mount trust a valid master's ceiling without scanning. */
static int format_masters_fresh(void)
{
	int rc = invalidate_buckets(-1);

	if (rc < 0) {
		return rc;
	}

	rc = master_format();
	if (rc < 0) {
		return rc;
	}
	return bind_root_empty();
}

/* Open the store and derive/validate the geometry-dependent state, leaving
 * the store closed if anything is rejected. Shared by mount and by the
 * format-an-unmountable-store path, which needs exactly the same checks:
 * every buffer and bucket-arithmetic assumption below rests on them, and a
 * format writes through the same primitives a mount does. discard is passed
 * to the backend: only the format path may have it replace a substrate it
 * cannot open (blob_db_store.h). */
static int store_open_and_validate(bool discard)
{
	struct blob_db_store_geom geom;
	int rc = blob_db_store_open(&geom, discard);

	if (rc < 0) {
		LOG_ERR("store open: %d", rc);
		return rc;
	}

	st.peb_size = geom.peb_size;
	st.write_align = geom.write_align ? geom.write_align : 1;
	st.n_pebs = geom.n_pebs;
	st.fa_size = (size_t)geom.n_pebs * geom.peb_size;
	st.preformat = geom.preformat;

	if (st.peb_size > BLOB_DB_SECTOR_BUF_MAX) {
		LOG_ERR("sector size %zu exceeds CONFIG_BLOB_DB_SECTOR_BUF_SIZE=%u",
			st.peb_size, BLOB_DB_SECTOR_BUF_MAX);
		rc = -ENOTSUP;
		goto err_close;
	}

	/* Slots and master records are padded to the write unit, and the
	 * record log starts at offset 128 (§3.1). */
	if (st.write_align > BLOB_DB_MAX_WRITE_ALIGN ||
	    (st.write_align & (st.write_align - 1)) != 0) {
		LOG_ERR("write unit %zu B: blob_db needs a power of two up to %u",
			st.write_align, BLOB_DB_MAX_WRITE_ALIGN);
		rc = -ENOTSUP;
		goto err_close;
	}
	st.rec_size = (uint16_t)ROUND_UP(sizeof(struct blob_db_master_rec),
					 st.write_align);

	/* Floor as well as ceiling: master_classify() stages a fixed
	 * BLOB_DB_MASTER_HDR_MAX window, a read must never cross a PEB
	 * boundary (blob_db_store.h), and the sector must hold the header
	 * window and at least two records. It also keeps the
	 * sustainable-payload arithmetic below from underflowing on an absurd
	 * geometry. */
	if (st.peb_size < BLOB_DB_MASTER_LOG_OFF + 2u * st.rec_size) {
		LOG_ERR("sector size %zu below the %zu B minimum blob_db supports",
			st.peb_size, BLOB_DB_MASTER_LOG_OFF + 2u * st.rec_size);
		rc = -ENOTSUP;
		goto err_close;
	}
	st.log_cap = (uint16_t)MIN((st.peb_size - BLOB_DB_MASTER_LOG_OFF) /
				   st.rec_size,
				   (size_t)UINT16_MAX);
#if defined(CONFIG_BLOB_DB_TEST_MASTER_LOG_CAP) && CONFIG_BLOB_DB_TEST_MASTER_LOG_CAP > 0
	/* Tests: rotate every few commits, so the whole suite exercises it. */
	st.log_cap = MIN(st.log_cap, (uint16_t)CONFIG_BLOB_DB_TEST_MASTER_LOG_CAP);
#endif

	if (st.n_pebs < BLOB_DB_FIRST_BUCKET + 1) {
		LOG_ERR("partition too small: %u sectors (need ≥ %u)",
			st.n_pebs, BLOB_DB_FIRST_BUCKET + 1);
		rc = -EINVAL;
		goto err_close;
	}
	st.n_buckets = st.n_pebs - BLOB_DB_FIRST_BUCKET;
	LOG_INF("partition %zu B, %u sectors of %zu B, %u buckets",
		st.fa_size, st.n_pebs, st.peb_size, st.n_buckets);

	/* Refuse a payload cap the geometry cannot sustain. A bucket is an
	 * append-only log, so a rebind needs two slots to coexist before
	 * compaction can reclaim the first — hence half the data area, not all
	 * of it. Checked here because peb_size is a runtime property (P2); a
	 * compile-time range cannot express it. */
	{
		const size_t sustainable =
			(st.peb_size - BLOB_DB_BUCKET_DATA_OFF) / 2 -
			BLOB_DB_SLOT_OVERHEAD;

		if (CONFIG_BLOB_DB_MAX_PAYLOAD_LEN > sustainable) {
			LOG_ERR("CONFIG_BLOB_DB_MAX_PAYLOAD_LEN=%u exceeds the "
				"%zu B this geometry can rebind (sector %zu B)",
				CONFIG_BLOB_DB_MAX_PAYLOAD_LEN, sustainable,
				st.peb_size);
			rc = -ENOTSUP;
			goto err_close;
		}
	}

	return 0;

err_close:
	blob_db_store_close();
	return rc;
}

static int mount_impl(void)
{
	if (st.mounted) {
		return -EALREADY;
	}

	IDX_INVALIDATE();
	st.wedged = false;   /* recovery below re-establishes a safe state */

	int rc = store_open_and_validate(false);

	if (rc < 0) {
		return rc;
	}

	/* Starts empty and fills as buckets are walked. When mount does scan
	 * the buckets (below), the scan fills it at no flash cost of its own. */
	blob_db_sidx_reset(st.n_buckets);

	/* Classify both master headers (§3.5). */
	struct blob_db_master_hdr h[2];
	enum master_class c[2];

	for (uint8_t i = 0; i < 2; i++) {
		rc = master_classify(i, &h[i], &c[i]);
		if (rc < 0) {
			goto err_close;
		}
	}
	LOG_DBG("master A=%s B=%s", master_class_str[c[0]],
		master_class_str[c[1]]);

	/* A parseable master declaring a format we do not know is never
	 * touched — not mounted, and above all not formatted. This is the
	 * -ENOTSUP the contract has always promised (l1_blob_db.md §4, §5.1).
	 * Checked before anything else, and on BOTH sectors: if an interrupted
	 * upgrade left A on our format and B ahead of it, falling back to A
	 * would silently mount a stale view of an upgraded store. The feature,
	 * suite and geometry checks run on every ok header for the same
	 * reason: a rotation torn by a newer build still makes this build
	 * refuse the store. */
	if (c[0] == MASTER_FOREIGN || c[1] == MASTER_FOREIGN) {
		LOG_ERR("refusing to mount a foreign store (A=%s B=%s); "
			"use blob_db_format() to discard it deliberately",
			master_class_str[c[0]], master_class_str[c[1]]);
		rc = -ENOTSUP;
		goto err_close;
	}
	st.incompat = 0;
	st.ro_compat = 0;
	st.compat = 0;
	for (uint8_t i = 0; i < 2; i++) {
		if (c[i] != MASTER_OK) {
			continue;
		}
		rc = master_accept(i, &h[i]);
		if (rc < 0) {
			goto err_close;
		}
		st.incompat |= h[i].incompat;
		st.ro_compat |= h[i].ro_compat;
		st.compat |= h[i].compat;
	}

	const bool both_ok = c[0] == MASTER_OK && c[1] == MASTER_OK;

	/* No writer gives two ok sectors the same rot_gen: either could hold
	 * the newer state (§3.1). */
	if (both_ok && h[0].rot_gen == h[1].rot_gen) {
		LOG_ERR("both masters at rot_gen %u; refusing — blob_db_format() "
			"discards the store", h[0].rot_gen);
		rc = -EIO;
		goto err_close;
	}

	/* Pick the sector to take the state from: the higher ok one, unless
	 * it holds no record that verifies (a rotation torn after its header,
	 * or every record rotten), and then the other (§3.5). */
	struct master_log log = { .newest = -1 };
	bool fell_back = false;
	int active = -1;

	if (c[0] == MASTER_OK || c[1] == MASTER_OK) {
		const uint8_t hi = (!both_ok) ? (c[0] == MASTER_OK ? 0 : 1)
					      : (h[0].rot_gen > h[1].rot_gen ? 0 : 1);
		const uint8_t lo = !hi;

		rc = master_log_load(hi, h[hi].rot_gen, &log);
		if (rc < 0) {
			goto err_close;
		}
		if (log.newest >= 0) {
			active = hi;
		} else if (c[lo] == MASTER_OK) {
			LOG_WRN("master %c holds no record that verifies; "
				"falling back to %c", 'A' + hi, 'A' + lo);
			rc = master_log_load(lo, h[lo].rot_gen, &log);
			if (rc < 0) {
				goto err_close;
			}
			if (log.newest >= 0) {
				active = lo;
				fell_back = true;
			}
		}
	}

	/* Whether the adopted ceiling has to be checked against the buckets,
	 * and how the mount then ends that doubt. Every record carries the
	 * highest ceiling saved so far, and alloc_id saves a ceiling before
	 * handing out any id below it, so the newest record that verifies
	 * covers every id on flash (§13.1) unless a newer one may have been
	 * lost:
	 *   - the newest written record fails its CRC (torn, or rotten);
	 *   - mount fell back to the other sector;
	 *   - the other sector is not ok: corrupt, or erased, which after a
	 *     format (both sectors written, D18) only a rotation torn after its
	 *     erase leaves;
	 *   - the store found damage on a master sector (UBI can stand an older
	 *     copy of a block back up, valid but stale).
	 * The last three are ended by a rotation, which rewrites the other
	 * sector; a bad last record by an append after it. */
	bool scan_ids = false;
	bool end_by_rotation = false;

	if (active < 0) {
		/* A partition whose masters are both blank is formatted.
		 * Anything else — bit rot, a power cut while the first master
		 * was written, another backend's substrate (doc/impl/
		 * l0_backends.md §4) — may be data, so discarding it is opt-in:
		 * CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT does what
		 * blob_db_format() would, erasing every bucket with the masters
		 * so no surviving blob can meet a reset id counter. */
		if (c[0] == MASTER_ERASED && c[1] == MASTER_ERASED) {
			LOG_INF("virgin partition; formatting");
			rc = format_masters_fresh();
		} else if (IS_ENABLED(CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT)) {
			LOG_WRN("no usable master (A=%s B=%s); erasing the "
				"store per CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT",
				master_class_str[c[0]], master_class_str[c[1]]);
			rc = format_whole_store();
		} else {
			LOG_ERR("no usable master (A=%s B=%s); refusing — "
				"blob_db_format() discards the store",
				master_class_str[c[0]], master_class_str[c[1]]);
			rc = -EIO;
		}
		if (rc < 0) {
			goto err_close;
		}
	} else {
		const uint8_t other = !active;
		const bool damage =
			blob_db_store_open_found_damage(
				peb_offset(BLOB_DB_MASTER_A_SECTOR)) ||
			blob_db_store_open_found_damage(
				peb_offset(BLOB_DB_MASTER_B_SECTOR));

		end_by_rotation = fell_back || c[other] != MASTER_OK || damage;
		scan_ids = end_by_rotation || log.tail_bad;

		st.active_master = (uint8_t)active;
		st.rot_gen = h[active].rot_gen;
		st.log_next = log.end;
		st.log_failed = false;
		st.ceiling_reset = false;
		st.next_id_hint = log.rec.next_id_hint;
		st.next_id = log.rec.next_id_hint;   /* ceiling is authoritative */
		st.seg_owner = log.rec.seg_owner;
		st.fuses = log.rec.fuses;
		LOG_INF("master %c rot_gen=%u record %d of %u next_id_hint=%llu%s",
			'A' + active, st.rot_gen, log.newest, st.log_cap,
			(unsigned long long)st.next_id, scan_ids ? " (doubt)" : "");

		/* Every mount, whatever the master says: the scratch seal is
		 * what tells an interrupted compaction (recover_compaction()).
		 * Before the doubt scan below, so it sees the restored bucket. */
		rc = recover_compaction();
		if (rc < 0) {
			goto err_close;
		}
	}

	/* The adopted ceiling is in doubt: find the highest id on flash and
	 * raise next_id past it, in RAM. The write below is what tells the
	 * next mount the ceiling is safe, so it comes after the scan: written
	 * first, a cut during the scan would leave a clean-looking master
	 * whose ceiling was never checked. */
	if (scan_ids) {
		uint64_t max_id = (st.next_id == 0) ? 0 : (st.next_id - 1);

		for (uint16_t bid = 0; bid < st.n_buckets; bid++) {
			rc = scan_bucket(bid, &max_id);
			if (rc < 0) {
				LOG_ERR("scan_bucket(%u): %d", bid, rc);
				goto err_close;
			}
		}
		if (max_id + 1 > st.next_id) {
			LOG_WRN("bucket scan out-ran ceiling (%llu >= %llu); "
				"raising", (unsigned long long)max_id,
				(unsigned long long)st.next_id);
			st.next_id = max_id + 1;
		}
	}

	/* A half-done upgrade: one ok header has a feature bit the other
	 * lacks. Rotations write the union (st's sets), so at most two make
	 * both headers carry every bit (§4.2). Any rotation also ends a doubt:
	 * it rewrites the other sector and gives the active one a fresh
	 * record that verifies. */
	bool rotated = false;

	if (active >= 0 && both_ok &&
	    (h[0].incompat != h[1].incompat || h[0].ro_compat != h[1].ro_compat ||
	     h[0].compat != h[1].compat)) {
		const struct blob_db_master_hdr *old_active = &h[active];

		LOG_WRN("master headers disagree on features; finishing the "
			"upgrade");
		rc = master_commit_rotate();
		if (rc == 0 && (old_active->incompat != st.incompat ||
				old_active->ro_compat != st.ro_compat ||
				old_active->compat != st.compat)) {
			rc = master_commit_rotate();
		}
		if (rc < 0) {
			goto err_close;
		}
		rotated = true;
	}

	/* End the doubt before returning, so the next mount does not scan
	 * again (§3.5): otherwise it would last until the next rotation. */
	if (scan_ids && !rotated) {
		rc = end_by_rotation ? master_commit_rotate() : master_commit();
		if (rc < 0) {
			goto err_close;
		}
	}

	/* Root-always-exists invariant (see @blob_db_root in blob_db.h): if the
	 * store predates this invariant — an older format where nobody bound
	 * id=1, or a virgin store whose master survived a partial format — bind
	 * an empty root now so callers can unconditionally get(root) / exists(root)
	 * / update(root). */
	{
		const uint16_t root_bid =
			(uint16_t)(BLOB_DB_ROOT_ID % st.n_buckets);
		bool formatted = false;

		rc = read_bucket_hdr(root_bid, &formatted);
		if (rc < 0) {
			goto err_close;
		}
		bool root_live = false;

		if (formatted) {
			struct bucket_scan sc;

			rc = find_live_slot(root_bid, BLOB_DB_ROOT_ID, &sc,
					    g_bbuf);
			if (rc == -ENOTSUP) {
				/* Binding an empty root would reformat the
				 * bucket and erase the slot this build cannot
				 * read; and without the root the store has no
				 * entry point. */
				LOG_ERR("root bucket %u holds a slot with a flag "
					"this build does not know; refusing",
					root_bid);
			}
			if (rc < 0 && rc != -ENOENT) {
				goto err_close;
			}
			root_live = (rc == 0);
		}
		if (!root_live) {
			LOG_WRN("root not bound; binding empty root");
			/* Consume id=1 for the root and raise the ceiling if
			 * we came in with next_id < 2. */
			if (st.next_id < 2) {
				st.next_id = 2;
			}
			if (st.next_id_hint < 2) {
				rc = persist_next_id_hint();
				if (rc < 0) {
					goto err_close;
				}
			}
			rc = bind_root_empty();
			if (rc < 0) {
				goto err_close;
			}
		}
	}

	st.mounted = true;

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	rc = seg_geometry_init();
	if (rc < 0) {
		st.mounted = false;
		goto err_close;
	}
	if (st.seg_owner != 0) {
		LOG_WRN("segmented write to id %llu was interrupted; sweeping",
			(unsigned long long)st.seg_owner);
		rc = seg_sweep(st.seg_owner);
		if (rc < 0) {
			st.mounted = false;
			goto err_close;
		}
	}
#endif

	LOG_INF("mount ok; next_id=%llu", st.next_id);
	return 0;

err_close:
	blob_db_sidx_disable();
	blob_db_store_close();
	return rc;
}

static int unmount_impl(void)
{
	if (!st.mounted) {
		return 0;
	}
	blob_db_store_close();
	st.mounted = false;
	IDX_INVALIDATE();
	blob_db_sidx_disable();
	LOG_INF("unmounted");
	return 0;
}

/* Stage 4 — put / get / update / delete / exists ------------------------ */

static inline size_t slot_size_for(uint16_t val_len)
{
	/* Round the on-flash slot size up to the backing device's
	 * write-block-size so writes always land on aligned boundaries. Any
	 * padding bytes read back as the erased value (0xff) and don't
	 * affect the CRC (which is stored at slot_size_for()-2, before the
	 * padding is added — see append_slot / slot_view_at). */
	const size_t base = BLOB_DB_SLOT_OVERHEAD + val_len;
	const size_t a = st.write_align ? st.write_align : 1;

	return (base + a - 1) & ~(a - 1);
}

static inline off_t bucket_offset(uint16_t bid)
{
	return peb_offset(BLOB_DB_FIRST_BUCKET + bid);
}

static inline uint16_t id_to_bucket(uint64_t id)
{
	return (uint16_t)(id % st.n_buckets);
}

/* CRC16-CCITT(0xFFFF) over slot_hdr + id + payload. */
static uint16_t slot_crc16(const struct blob_db_slot_hdr *hdr, uint64_t id,
			    const void *payload, uint16_t val_len)
{
	uint16_t c = crc16_ccitt(0xffff, (const uint8_t *)hdr, sizeof(*hdr));

	c = crc16_ccitt(c, (const uint8_t *)&id, sizeof(id));
	if (val_len > 0) {
		c = crc16_ccitt(c, (const uint8_t *)payload, val_len);
	}
	return c;
}

static bool bucket_hdr_valid(const uint8_t *buf, uint16_t bid)
{
	const struct blob_db_bucket_hdr *bhdr =
		(const struct blob_db_bucket_hdr *)buf;

	if (memcmp(bhdr->magic, BUCKET_MAGIC, 4) != 0) {
		return false;
	}
	if (bhdr->hdr_crc32 != hdr_crc32(bhdr, sizeof(*bhdr))) {
		return false;
	}
	if (bhdr->bucket_id != bid) {
		return false;
	}
	return true;
}

/* A slot of a resident bucket image, as sized by its header. */
struct slot_view {
	uint64_t id;
	uint8_t  flags;
	uint16_t val_len;
	size_t   total_size;
};

/* Size the slot at buf[off] from its header, as scan_bucket_for() does. False
 * where the header cannot be trusted to size it: the end of the log (§3.4 of
 * l1_bucketlog.md). Otherwise *intact says whether its CRC verifies. A slot
 * that is not intact is stepped over, never served, kept or counted, so one
 * rotten slot does not hide the slots after it. */
static bool slot_walk_at(const uint8_t *buf, off_t off, struct slot_view *out,
			 bool *intact)
{
	if (off + BLOB_DB_SLOT_OVERHEAD > (off_t)st.peb_size) {
		return false;
	}
	const struct blob_db_slot_hdr *shdr =
		(const struct blob_db_slot_hdr *)(buf + off);

	if (shdr->flags == 0xff) {
		return false;
	}
	if (!(shdr->flags & BLOB_DB_SLOT_F_SEALED)) {
		return false;
	}
	/* Written by a version we do not understand: its payload does not mean
	 * what we would take it to mean, so the walk cannot go on. Where it is
	 * whole, it is data all the same, and the bucket is closed rather than
	 * truncated: see slot_foreign_head(). Mount should have refused such a
	 * store already (a new slot flag is a MAJOR change); this is the
	 * backstop if that discipline ever slips. */
	if (shdr->flags & ~(uint8_t)BLOB_DB_SLOT_F_KNOWN) {
		return false;
	}
	if (shdr->val_len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
		return false;
	}
	const size_t ssz = slot_size_for(shdr->val_len);

	if (off + ssz > (off_t)st.peb_size) {
		return false;
	}

	uint64_t id;
	memcpy(&id, buf + off + sizeof(*shdr), sizeof(id));
	const uint8_t *payload = buf + off + sizeof(*shdr) + sizeof(id);
	uint16_t stored;
	const size_t crc_off = sizeof(*shdr) + sizeof(id) + shdr->val_len;

	memcpy(&stored, buf + off + crc_off, sizeof(stored));
	*intact = slot_crc16(shdr, id, payload, shdr->val_len) == stored;

	out->id         = id;
	out->flags      = shdr->flags;
	out->val_len    = shdr->val_len;
	out->total_size = ssz;
	return true;
}

/* Whether a slot header at @p off is one a build with more slot flags than
 * this one wrote: sealed, a flag outside BLOB_DB_SLOT_F_KNOWN, and a length
 * this build could size and that fits the sector. Its CRC still has to
 * verify (slot_foreign_at(), slot_crc_ok_on_flash()) before the slot counts
 * as foreign rather than torn.
 *
 * A foreign slot is not the end of the log. It is data this build cannot
 * read, and slots after it may supersede ones before it, so its bucket can
 * answer no lookup and take no write: reads return -EIO, writes -ENOTSUP,
 * and compaction, which would drop it and everything after it, refuses
 * (doc/proposals/2026-10-28-master-log.md D13). The bucket is kept as it is
 * until a format. */
static bool slot_foreign_head(const struct blob_db_slot_hdr *hdr, off_t off)
{
	return hdr->flags != 0xff && (hdr->flags & BLOB_DB_SLOT_F_SEALED) &&
	       (hdr->flags & ~(uint8_t)BLOB_DB_SLOT_F_KNOWN) &&
	       hdr->val_len <= CONFIG_BLOB_DB_MAX_PAYLOAD_LEN &&
	       off + (off_t)slot_size_for(hdr->val_len) <= (off_t)st.peb_size;
}

/* Whether the slot at buf[off] of a resident bucket image, where a walk
 * stopped, is foreign: see slot_foreign_head(). */
static bool slot_foreign_at(const uint8_t *buf, off_t off)
{
	if (off + BLOB_DB_SLOT_OVERHEAD > (off_t)st.peb_size) {
		return false;
	}

	const struct blob_db_slot_hdr *shdr =
		(const struct blob_db_slot_hdr *)(buf + off);

	if (!slot_foreign_head(shdr, off)) {
		return false;
	}

	uint64_t id;
	uint16_t stored;
	const uint8_t *payload = buf + off + sizeof(*shdr) + sizeof(id);

	memcpy(&id, buf + off + sizeof(*shdr), sizeof(id));
	memcpy(&stored, payload + shdr->val_len, sizeof(stored));
	return slot_crc16(shdr, id, payload, shdr->val_len) == stored;
}

/* Whether the log of a resident bucket image ends at a foreign slot. */
static bool bucket_image_ends_foreign(const uint8_t *buf)
{
	off_t off = BLOB_DB_BUCKET_DATA_OFF;
	struct slot_view sv;
	bool intact;

	while (slot_walk_at(buf, off, &sv, &intact)) {
		off += sv.total_size;
	}
	return slot_foreign_at(buf, off);
}

/* An intact slot at buf[off]; false at the end of the log or a rotten slot.
 * For images blob_db built itself, where every slot is intact. */
static bool slot_view_at(const uint8_t *buf, off_t off, struct slot_view *out)
{
	bool intact;

	return slot_walk_at(buf, off, out, &intact) && intact;
}

/* Whether an intact slot later in the log carries the same id as @p sv at
 * @p off. A rotten slot supersedes nothing: readers fall back past it to the
 * newest intact one, and that is the slot to keep. */
static bool slot_superseded(const uint8_t *buf, off_t off,
			    const struct slot_view *sv)
{
	off_t scan = off + (off_t)sv->total_size;
	struct slot_view future;
	bool intact;

	while (slot_walk_at(buf, scan, &future, &intact)) {
		if (intact && future.id == sv->id) {
			return true;
		}
		scan += future.total_size;
	}
	return false;
}

static int read_bucket(uint16_t bid, uint8_t *buf)
{
	return blob_db_store_read(bucket_offset(bid), buf, st.peb_size);
}

static int format_bucket(uint16_t bid)
{
	struct blob_db_bucket_hdr bhdr = {
		.bucket_id = bid,
		.reserved  = 0,
		.gen       = 1,
	};
	memcpy(bhdr.magic, BUCKET_MAGIC, 4);
	bhdr.hdr_crc32 = hdr_crc32(&bhdr, sizeof(bhdr));

	IO_CAUSE(bucket_formats);
	const int rc = blob_db_store_replace(bucket_offset(bid), &bhdr,
					     sizeof(bhdr));
	if (rc < 0) {
		LOG_ERR("bucket %u format: %d", bid, rc);
		blob_db_sidx_forget(bid);
	} else {
		blob_db_sidx_begin(bid, BLOB_DB_BUCKET_DATA_OFF);
	}
	return rc;
}

/* Streaming bucket access ------------------------------------------------
 *
 * Point operations (get/update/delete/exists) locate a slot by reading slot
 * headers, not by pulling the whole erase block into RAM. On the 64 KB-sector
 * QSPI NOR that is the difference between reading 64 KB and reading ~12 bytes
 * per slot to answer one lookup.
 *
 * Bulk operations (count/iterate) and compaction still work on a resident
 * sector image: their liveness test is "does a later slot share this id?",
 * which is O(n^2) in slots, and turning each of those comparisons into a flash
 * read would cost far more than the sector read it replaced. They are
 * diagnostics and maintenance, not the hot path (contract D5).
 */

static int read_bucket_hdr(uint16_t bid, bool *valid)
{
	uint32_t known;

	/* The index holds a cursor only for a bucket whose header it has seen
	 * valid, and forgets it whenever the bucket is rewritten. */
	if (blob_db_sidx_cursor(bid, &known)) {
		*valid = true;
		return 0;
	}

	struct blob_db_bucket_hdr bhdr;
	int rc = blob_db_store_read(bucket_offset(bid), &bhdr, sizeof(bhdr));

	if (rc < 0) {
		return rc;
	}
	*valid = bucket_hdr_valid((const uint8_t *)&bhdr, bid);
	return 0;
}

/* The CRC check of slot_verify(), streamed in small reads: for a slot the
 * walk stopped at, whose payload is not staged anywhere. Runs only where a
 * walk stops at a sealed slot with an unknown flag, so its cost is off every
 * ordinary path. */
static int slot_crc_ok_on_flash(uint16_t bid, off_t off,
				const struct slot_head *sh, bool *ok)
{
	uint8_t chunk[64];
	off_t at = bucket_offset(bid) + off + (off_t)sizeof(*sh);
	size_t left = sh->hdr.val_len;
	uint16_t c = crc16_ccitt(0xffff, (const uint8_t *)&sh->hdr,
				 sizeof(sh->hdr));
	uint16_t stored;
	int rc;

	c = crc16_ccitt(c, (const uint8_t *)&sh->id, sizeof(sh->id));
	while (left > 0) {
		const size_t n = MIN(left, sizeof(chunk));

		rc = blob_db_store_read(at, chunk, n);
		if (rc < 0) {
			return rc;
		}
		c = crc16_ccitt(c, chunk, n);
		at += (off_t)n;
		left -= n;
	}
	rc = blob_db_store_read(at, &stored, sizeof(stored));
	if (rc < 0) {
		return rc;
	}
	*ok = c == stored;
	return 0;
}

/* Walk bucket `bid` by slot header, recording the append cursor and the LAST
 * slot whose id is `target_id` and which starts below `limit`.
 *
 * Because a slot's size comes from its own header, the walk can step over a
 * slot it has not verified. A slot whose CRC turns out to be bad is therefore
 * *skipped* rather than treated as the end of the log: the caller verifies only
 * the slot it actually wants and re-scans below it on failure
 * (find_live_slot). One corrupt slot consequently no longer hides every slot
 * after it in the bucket, which the resident-buffer walk it replaces did.
 *
 * The log still ends at the first header that cannot be trusted to size its
 * own slot — erased, unsealed, carrying unknown flags, or an implausible
 * val_len — because there is no safe stride past it. Where the slot with
 * unknown flags is whole, out->foreign says so, and the bucket answers and
 * takes nothing (slot_foreign_head()).
 */
static int scan_bucket_for(uint16_t bid, uint64_t target_id, off_t limit,
			   struct bucket_scan *out)
{
	const off_t base = bucket_offset(bid);
	off_t cursor = BLOB_DB_BUCKET_DATA_OFF;

	out->target_off = -1;
	out->from_index = false;
	out->tail_torn = false;
	out->foreign = false;

	for (;;) {
		if (cursor + (off_t)BLOB_DB_SLOT_OVERHEAD > (off_t)st.peb_size) {
			break;
		}

		struct slot_head sh;
		int rc = blob_db_store_read(base + cursor, &sh, sizeof(sh));

		if (rc < 0) {
			return rc;
		}
		if (sh.hdr.flags == 0xff ||
		    !(sh.hdr.flags & BLOB_DB_SLOT_F_SEALED) ||
		    (sh.hdr.flags & ~(uint8_t)BLOB_DB_SLOT_F_KNOWN) ||
		    sh.hdr.val_len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
			if (slot_foreign_head(&sh.hdr, cursor)) {
				bool ok;

				rc = slot_crc_ok_on_flash(bid, cursor, &sh, &ok);
				if (rc < 0) {
					return rc;
				}
				out->foreign = ok;
			}
			/* A slot is programmed from its first byte on, so a
			 * programmed flags byte here means a torn append, unless
			 * the slot is whole and only its flags are unknown. */
			out->tail_torn = !out->foreign && sh.hdr.flags != 0xff;
			break;
		}

		const size_t ssz = slot_size_for(sh.hdr.val_len);

		if (cursor + (off_t)ssz > (off_t)st.peb_size) {
			out->tail_torn = true;
			break;
		}
		if (sh.id == target_id && cursor < limit) {
			out->target_off = cursor;
			out->target     = sh;
		}
		cursor += ssz;
	}

	out->write_cursor = cursor;
	return 0;
}

/* Read the target slot's payload into `dst` and check its CRC.
 *
 * The CRC is computed over the header and id bytes exactly as they were read
 * from flash — not over a reconstruction — so a slot written by a version that
 * used a currently-reserved field still verifies against its own bytes.
 *
 * -EBADMSG means the slot is torn or rotten; every other negative value is an
 * I/O error.
 */
static int slot_verify(uint16_t bid, const struct bucket_scan *sc, void *dst)
{
	const off_t poff = bucket_offset(bid) + sc->target_off +
			   (off_t)sizeof(struct slot_head);
	const uint16_t val_len = sc->target.hdr.val_len;
	uint16_t stored;
	int rc;

	if (val_len > 0) {
		rc = blob_db_store_read(poff, dst, val_len);
		if (rc < 0) {
			return rc;
		}
	}
	rc = blob_db_store_read(poff + val_len, &stored, sizeof(stored));
	if (rc < 0) {
		return rc;
	}
	if (slot_crc16(&sc->target.hdr, sc->target.id, dst, val_len) != stored) {
		return -EBADMSG;
	}
	return 0;
}

/* The walk behind find_live_slot(). With @p learn, an answer taken from the
 * full log is taught to the slot index; one found below a rotten slot is not,
 * since it is not the id's newest slot. */
static int walk_live_slot(uint16_t bid, uint64_t id, struct bucket_scan *sc,
			  void *dst, bool learn)
{
	off_t limit = (off_t)st.peb_size;

	for (;;) {
		int rc = scan_bucket_for(bid, id, limit, sc);

		if (rc < 0) {
			return rc;
		}
		if (sc->foreign) {
			/* A newer slot for this id may lie past the one the walk
			 * cannot read: no answer from here is safe to give. */
			return -ENOTSUP;
		}
		if (sc->target_off < 0) {
			if (learn && limit == (off_t)st.peb_size) {
				blob_db_sidx_learn(bid, id, -1,
						   (uint32_t)sc->write_cursor);
			}
			return -ENOENT;
		}

		rc = slot_verify(bid, sc, dst);
		if (rc == -EBADMSG) {
			limit = sc->target_off;
			continue;
		}
		if (rc < 0) {
			return rc;
		}
		if (sc->target.hdr.flags & BLOB_DB_SLOT_F_TOMBSTONE) {
			return -ENOENT;
		}
		if (learn && limit == (off_t)st.peb_size) {
			blob_db_sidx_learn(bid, id, sc->target_off,
					   (uint32_t)sc->write_cursor);
		}
		return 0;
	}
}

/* Where an append to bucket @p bid goes: the slot index's cursor when it has
 * one, else a walk (which teaches the index the cursor). Only write_cursor is
 * meaningful in @p sc afterwards, and a writer passes it through
 * confirm_cursor() before appending there. */
static int bucket_cursor(uint16_t bid, uint64_t id, struct bucket_scan *sc)
{
	uint32_t cursor;

	if (blob_db_sidx_cursor(bid, &cursor)) {
		sc->write_cursor = cursor;
		sc->target_off = -1;
		sc->from_index = true;
		sc->tail_torn = false;
		sc->foreign = false;
		return 0;
	}

	int rc = scan_bucket_for(bid, id, (off_t)st.peb_size, sc);

	if (rc == 0 && !sc->foreign) {
		blob_db_sidx_learn(bid, id, -1, (uint32_t)sc->write_cursor);
	}
	return rc;
}

/* Report the bucket full when the log ends at a torn slot. Its bytes are
 * programmed, so an append there would be programmed over them and fail its
 * own CRC -- lost although the writer reported success. Full makes the writer
 * compact first, and compaction keeps only intact slots. */
static void cursor_past_torn_tail(uint16_t bid, struct bucket_scan *sc)
{
	LOG_WRN("bucket %u: torn slot at %ld; compacting before the next append",
		bid, (long)sc->write_cursor);
	blob_db_sidx_forget(bid);
	sc->write_cursor = (off_t)st.peb_size;
}

/* Called by every writer before it appends at sc->write_cursor.
 *
 * A cursor the walk found is where the log ends, but not necessarily where
 * erased flash begins: a power cut inside an append leaves a torn slot there,
 * and the walk says so (tail_torn).
 *
 * One the slot index supplied is where the log ended after the last append
 * blob_db made, and flash could only differ if something else wrote it. That
 * cannot happen while mounted, but programming over non-erased bytes is the one
 * failure an index must never cause, so the 12 bytes a slot head would occupy
 * are checked to be erased first. One read per write; reads do not pay it. */
static int confirm_cursor(uint16_t bid, uint64_t id, struct bucket_scan *sc)
{
	if (!sc->from_index) {
		if (sc->foreign) {
			LOG_WRN("bucket %u: a slot at %ld carries a flag this "
				"build does not know; refusing to write the bucket",
				bid, (long)sc->write_cursor);
			return -ENOTSUP;
		}
		if (sc->tail_torn) {
			sc->tail_torn = false;
			cursor_past_torn_tail(bid, sc);
		}
		return 0;
	}
#if defined(CONFIG_BLOB_DB_SLOT_INDEX)
	sc->from_index = false;

	const off_t at = sc->write_cursor;

	if (at + (off_t)sizeof(struct slot_head) <= (off_t)st.peb_size) {
		uint8_t head[sizeof(struct slot_head)];
		int rc = blob_db_store_read(bucket_offset(bid) + at, head,
					    sizeof(head));

		if (rc < 0) {
			return rc;
		}
		for (size_t i = 0; i < sizeof(head); i++) {
			if (head[i] != 0xff) {
				LOG_WRN("slot index: bucket %u not erased at "
					"cursor %ld; walking", bid, (long)at);
				blob_db_sidx_forget(bid);

				struct bucket_scan walk;

				rc = scan_bucket_for(bid, id,
						     (off_t)st.peb_size, &walk);
				if (rc < 0) {
					return rc;
				}
				if (walk.foreign) {
					return -ENOTSUP;
				}
				sc->write_cursor = walk.write_cursor;
				if (walk.tail_torn) {
					cursor_past_torn_tail(bid, sc);
				}
				return 0;
			}
		}
	}

#if defined(CONFIG_BLOB_DB_SLOT_INDEX_VERIFY)
	struct bucket_scan walk;
	int vrc = scan_bucket_for(bid, id, (off_t)st.peb_size, &walk);

	if (vrc < 0) {
		return vrc;
	}
	if (walk.write_cursor != sc->write_cursor) {
		LOG_ERR("slot index: bucket %u cursor %ld, walk says %ld",
			bid, (long)sc->write_cursor, (long)walk.write_cursor);
		__ASSERT(false, "slot index cursor disagrees with the walk");
		return -EIO;
	}
#endif
#else
	ARG_UNUSED(bid);
	ARG_UNUSED(id);
	ARG_UNUSED(sc);
#endif
	return 0;
}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
/* The append cursor of formatted bucket @p bid, found by a walk and passed
 * through confirm_cursor(), so a log that ends at a torn slot reads as full.
 * For the writers that take the cursor from a walk rather than from
 * bucket_cursor() -- the segment, index and segment-tombstone appends. */
static int walk_cursor(uint16_t bid, uint64_t id, off_t *cursor)
{
	struct bucket_scan sc;
	int rc = scan_bucket_for(bid, id, (off_t)st.peb_size, &sc);

	if (rc < 0) {
		return rc;
	}
	rc = confirm_cursor(bid, id, &sc);
	if (rc < 0) {
		return rc;
	}
	*cursor = sc.write_cursor;
	return 0;
}
#endif

#if defined(CONFIG_BLOB_DB_SLOT_INDEX)
/* The slot index's answer for @p id, checked against the slot it names: one
 * 12-byte read in place of the walk. -EAGAIN when the index has no answer, or
 * one that does not hold up, and the caller walks instead. */
static int find_indexed_slot(uint16_t bid, uint64_t id,
			     struct bucket_scan *sc, void *dst)
{
	uint32_t off, cursor;

	if (!blob_db_sidx_lookup(bid, id, &off, &cursor)) {
		return -EAGAIN;
	}

	struct slot_head sh;
	int rc;

	if (off + BLOB_DB_SLOT_OVERHEAD > cursor) {
		goto stale;
	}
	rc = blob_db_store_read(bucket_offset(bid) + off, &sh, sizeof(sh));
	if (rc < 0) {
		return rc;
	}
	if (sh.id != id || sh.hdr.flags == 0xff ||
	    !(sh.hdr.flags & BLOB_DB_SLOT_F_SEALED) ||
	    (sh.hdr.flags & ~(uint8_t)BLOB_DB_SLOT_F_KNOWN) ||
	    sh.hdr.val_len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN ||
	    off + slot_size_for(sh.hdr.val_len) > cursor) {
		goto stale;
	}

	sc->write_cursor = cursor;
	sc->target_off = off;
	sc->target = sh;
	sc->from_index = true;
	sc->tail_torn = false;
	sc->foreign = false;

	rc = slot_verify(bid, sc, dst);
	if (rc == -EBADMSG) {
		return -EAGAIN;   /* rotten: the walk falls back past it */
	}
	if (rc < 0) {
		return rc;
	}

	const int hit = (sh.hdr.flags & BLOB_DB_SLOT_F_TOMBSTONE) ? -ENOENT : 0;

#if defined(CONFIG_BLOB_DB_SLOT_INDEX_VERIFY)
	/* Compare answers, not raw scans: the walk's own answer skips a slot
	 * that fails its CRC, exactly as a lookup would. It stages the same
	 * slot's payload in dst if it agrees. */
	struct bucket_scan walk;
	int wrc = walk_live_slot(bid, id, &walk, dst, false);

	if (wrc != hit || (wrc == 0 && walk.target_off != sc->target_off)) {
		LOG_ERR("slot index: id %llu bid %u: index answers %d at off "
			"%ld, walk answers %d at off %ld",
			(unsigned long long)id, bid, hit, (long)sc->target_off,
			wrc, (long)walk.target_off);
		__ASSERT(false, "slot index disagrees with the walk");
		return -EIO;
	}
#endif
	return hit;

stale:
	/* Should not happen while every append is noted; if it does, stop
	 * trusting this bucket until a walk re-learns it. */
	LOG_WRN("slot index: stale entry for id %llu in bucket %u",
		(unsigned long long)id, bid);
	blob_db_sidx_forget(bid);
	return -EAGAIN;
}
#endif

/* Find the newest *intact* slot for `id`, staging its payload in `dst`.
 *
 * Retries below a slot that fails its CRC, so the visible value is the newest
 * committed one — the same answer the resident walk gave, since there a torn
 * slot ended the log and left the previous one live.
 *
 * On success `sc` describes the slot (`sc->write_cursor` is the append cursor
 * either way). Returns -ENOENT when the id has no live slot here.
 */
static int find_live_slot(uint16_t bid, uint64_t id, struct bucket_scan *sc,
			  void *dst)
{
#if defined(CONFIG_BLOB_DB_SLOT_INDEX)
	int irc = find_indexed_slot(bid, id, sc, dst);

	if (irc != -EAGAIN) {
		return irc;
	}
#endif
	return walk_live_slot(bid, id, sc, dst, true);
}

/* Build and write a slot at bucket+write_cursor. Slot size on flash is
 * padded to write_block_size; padding bytes are 0xff so any truncated
 * write still reads as erased in the padding region. */

/* Scatter form: the slot's payload is `hdr_len` bytes from `hdr` followed by
 * `val_len - hdr_len` bytes from `payload`. Lets a segment or index record be
 * written without first concatenating its header and body into a buffer of
 * their own. `hdr` may be NULL when hdr_len is 0. */
static int append_slot2(uint16_t bid, off_t write_cursor, uint64_t id,
			uint8_t extra_flags, const void *hdr, uint16_t hdr_len,
			const void *payload, uint16_t val_len);

static int append_slot(uint16_t bid, off_t write_cursor, uint64_t id,
		       uint8_t extra_flags, const void *payload,
		       uint16_t val_len)
{
	return append_slot2(bid, write_cursor, id, extra_flags, NULL, 0,
			    payload, val_len);
}

static int append_slot2(uint16_t bid, off_t write_cursor, uint64_t id,
			uint8_t extra_flags, const void *hdr, uint16_t hdr_len,
			const void *payload, uint16_t val_len)
{
	/* Staged in the compaction scratch buffer rather than on the stack:
	 * at CONFIG_BLOB_DB_MAX_PAYLOAD_LEN=4096 a stack frame here was ~4.1 KB,
	 * over the 4 KB bound contract R7 sets. g_bbuf_new is sector-sized and
	 * a slot is smaller than a sector by construction, so it always fits.
	 *
	 * Safe because compaction always *completes* before its caller appends
	 * — blob_db_update()/blob_db_delete() call compact_bucket() and only
	 * then append_slot(), never the other way round. The assert pins that. */
	__ASSERT(!g_bbuf_new_busy, "append_slot re-entered compaction scratch");

	/* Every mutation in the library funnels through here, so this one line
	 * is the whole invalidation story for the index cache: an append can
	 * change what an id's index record says, and no read appends. */
	IDX_INVALIDATE();

	uint8_t *buf = g_bbuf_new;

	struct blob_db_slot_hdr *shdr = (struct blob_db_slot_hdr *)buf;

	shdr->flags    = BLOB_DB_SLOT_F_SEALED | extra_flags;
	shdr->reserved = 0;
	shdr->val_len  = val_len;

	memcpy(buf + sizeof(*shdr), &id, sizeof(id));

	uint8_t *pl = buf + sizeof(*shdr) + sizeof(id);

	if (hdr_len > 0) {
		memcpy(pl, hdr, hdr_len);
	}
	if (val_len > hdr_len) {
		memcpy(pl + hdr_len, payload, (size_t)val_len - hdr_len);
	}

	/* CRC over the assembled bytes, so the scatter and contiguous forms
	 * produce identical slots. */
	uint16_t crc = slot_crc16(shdr, id, pl, val_len);
	const size_t crc_off = sizeof(*shdr) + sizeof(id) + val_len;

	memcpy(buf + crc_off, &crc, sizeof(crc));

	const size_t total = slot_size_for(val_len);
	const size_t base = BLOB_DB_SLOT_OVERHEAD + val_len;

	if (total > base) {
		memset(buf + base, 0xff, total - base);
	}
	if (st.write_align > BLOB_DB_MAX_WRITE_ALIGN) {
		LOG_ERR("write_align %zu exceeds BLOB_DB_MAX_WRITE_ALIGN=%u",
			st.write_align, BLOB_DB_MAX_WRITE_ALIGN);
		return -ENOTSUP;
	}

	int rc = blob_db_store_write(bucket_offset(bid) + write_cursor,
				     buf, total);

	/* The one place a slot is added, so the one place the slot index has
	 * to follow. A failed write may have left a torn slot the walk will
	 * step over, at a cursor the index cannot know. */
	if (rc < 0) {
		blob_db_sidx_forget(bid);
	} else {
		const bool tomb = (extra_flags & BLOB_DB_SLOT_F_TOMBSTONE) != 0;

		blob_db_sidx_note(bid, id, (uint32_t)write_cursor,
				  (uint32_t)total, tomb);
	}
	return rc;
}

/* Per-bucket compaction & crash recovery (stage 5) ---------------------- */

/* Build a compacted image of bucket `bid` in new_buf using old_buf as the
 * source. Returns the number of bytes written into new_buf (≥ 16 for the
 * bucket header), and in *stop the offset in old_buf where the walk ended.
 * Skipped entries: tombstones, rotten slots, and any slot whose id has a
 * later intact occurrence in the same bucket. */
static size_t build_compacted_image(const uint8_t *old_buf, uint8_t *new_buf,
				    uint16_t bid, off_t *stop)
{
	const struct blob_db_bucket_hdr *old_bhdr =
		(const struct blob_db_bucket_hdr *)old_buf;

	struct blob_db_bucket_hdr *new_bhdr =
		(struct blob_db_bucket_hdr *)new_buf;

	memcpy(new_bhdr->magic, BUCKET_MAGIC, 4);
	new_bhdr->bucket_id = bid;
	new_bhdr->reserved  = 0;
	new_bhdr->gen       = old_bhdr->gen + 1;
	new_bhdr->hdr_crc32 = hdr_crc32(new_bhdr, sizeof(*new_bhdr));

	off_t new_cursor = BLOB_DB_BUCKET_DATA_OFF;
	off_t cursor     = BLOB_DB_BUCKET_DATA_OFF;

	for (;;) {
		struct slot_view sv;
		bool intact;

		if (!slot_walk_at(old_buf, cursor, &sv, &intact)) {
			break;
		}

		if (intact && !(sv.flags & BLOB_DB_SLOT_F_TOMBSTONE) &&
		    !slot_superseded(old_buf, cursor, &sv)) {
			memcpy(new_buf + new_cursor,
			       old_buf + cursor, sv.total_size);
			new_cursor += sv.total_size;
		}

		cursor += sv.total_size;
	}

	*stop = cursor;
	return (size_t)new_cursor;
}

/* Whether compaction may drop everything in old_buf from @p stop, where its
 * walk ended, to the end of the sector. It drops what it cannot walk, so it
 * must know that is nothing but erased flash and the remains of one torn
 * append.
 *
 * A foreign slot (slot_foreign_at()) is data: -ENOTSUP, and the bucket stays
 * as it is. Otherwise one append programs one slot, so a power cut leaves
 * programmed bytes at most one slot span past the stop, and in any order
 * within it: a NOR page program cut midway can leave bits anywhere in the
 * page, beyond a header torn too badly to size. A programmed byte further
 * out is data the walk could not reach, from a build with another payload cap
 * or a stride the walk lost: -EIO, and nothing is dropped. */
static int compaction_may_drop_tail(uint16_t bid, const uint8_t *old_buf,
				    off_t stop)
{
	if (slot_foreign_at(old_buf, stop)) {
		LOG_ERR("compact bid=%u: a slot at %ld carries a flag this build "
			"does not know; not compacting", bid, (long)stop);
		return -ENOTSUP;
	}

	const off_t span = (off_t)slot_size_for(CONFIG_BLOB_DB_MAX_PAYLOAD_LEN);

	for (off_t i = stop + span; i < (off_t)st.peb_size; i++) {
		if (old_buf[i] != 0xff) {
			LOG_ERR("compact bid=%u: programmed bytes at %ld, past "
				"where the log ends (%ld); not compacting",
				bid, (long)i, (long)stop);
			return -EIO;
		}
	}
	return 0;
}

/* Byte offset of the scratch seal: the tail of the scratch sector, rounded
 * down so the write stays aligned. The compacted image must end at or before
 * it. */
static inline size_t scratch_seal_len(void)
{
	const size_t a = st.write_align ? st.write_align : 1;

	return ROUND_UP(sizeof(struct blob_db_scratch_seal), a);
}

static inline off_t scratch_seal_off(void)
{
	return (off_t)(st.peb_size - scratch_seal_len());
}

static int scratch_seal_write(off_t scratch_off, const uint8_t *image,
			      size_t image_len)
{
	uint8_t __aligned(4) buf[BLOB_DB_MAX_WRITE_ALIGN + sizeof(struct blob_db_scratch_seal)];
	struct blob_db_scratch_seal *seal = (struct blob_db_scratch_seal *)buf;

	memset(buf, 0xff, sizeof(buf));
	memcpy(seal->magic, SCRATCH_MAGIC, 4);
	seal->image_len   = (uint32_t)image_len;
	seal->image_crc32 = crc32_ieee(image, image_len);
	seal->seal_crc32  = hdr_crc32(seal, sizeof(*seal));

	return blob_db_store_write(scratch_off + scratch_seal_off(), buf,
				   scratch_seal_len());
}

/* Is the scratch sector a complete, self-consistent compacted image?
 * `image` must already hold the sector's first peb_size bytes. */
static bool scratch_seal_valid(const uint8_t *image, size_t *image_len)
{
	struct blob_db_scratch_seal seal;

	memcpy(&seal, image + scratch_seal_off(), sizeof(seal));

	if (memcmp(seal.magic, SCRATCH_MAGIC, 4) != 0) {
		return false;
	}
	if (seal.seal_crc32 != hdr_crc32(&seal, sizeof(seal))) {
		return false;
	}
	if (seal.image_len < BLOB_DB_BUCKET_DATA_OFF ||
	    seal.image_len > (uint32_t)scratch_seal_off()) {
		return false;
	}
	if (seal.image_crc32 != crc32_ieee(image, seal.image_len)) {
		return false;
	}
	*image_len = seal.image_len;
	return true;
}

/* Phase-2: persist the compacted image through the scratch sector.
 *
 *   1. erase scratch; write scratch = new image; seal it   ── enters the window
 *   2. erase bucket; write bucket = new image
 *   3. erase scratch                                       ── leaves the window
 *
 * No master is written (D17): a sealed scratch is what marks the window, and
 * mount-time recover_compaction() restores the bucket from it whatever the
 * master says (doc/impl/l1_bucketlog.md §6.1).
 */
static int compact_commit(uint16_t bid, const uint8_t *new_buf, size_t new_len)
{
	const off_t scratch_off = peb_offset(BLOB_DB_SCRATCH_SECTOR);
	const off_t bucket_off  = bucket_offset(bid);

	/* The seal has to fit after the image in the same sector. An image this
	 * large means compaction reclaimed nothing worth having, which is the
	 * -ENOSPC the caller would reach anyway. */
	if (new_len > (size_t)scratch_seal_off()) {
		LOG_WRN("compact bid=%u: image %zu B leaves no room to seal "
			"scratch (limit %zu B)", bid, new_len,
			(size_t)scratch_seal_off());
		return -ENOSPC;
	}

	/* A backend that replaces a block atomically needs none of the window
	 * below: the bucket holds the old image or the new one whatever the
	 * power does, so there is nothing for recover_compaction() to settle. A
	 * failed replace leaves the old image, so it is reported rather than
	 * wedging the store. */
	if (blob_db_store_replace_is_atomic()) {
		return blob_db_store_replace(bucket_off, new_buf, new_len);
	}

	int rc;

	/* Step 1: scratch, then seal it. The seal is what makes step 2 safe to
	 * start: until it lands, recovery leaves the bucket alone. */
	rc = blob_db_store_erase(scratch_off, st.peb_size);
	if (rc < 0) {
		return rc;
	}
	rc = blob_db_store_write(scratch_off, new_buf, new_len);
	if (rc < 0) {
		return rc;
	}
	rc = scratch_seal_write(scratch_off, new_buf, new_len);
	if (rc < 0) {
		return rc;
	}

	/* From here to step 3 the sealed scratch is authoritative: a mount now
	 * would restore it over this bucket. So an I/O error in this window
	 * cannot simply be reported and shrugged off — the caller would carry
	 * on, its next append into this bucket would be acknowledged, and a
	 * crash before the window closes would discard that acknowledged write
	 * when recovery restores the image. Losing a write we said we had done
	 * is worse than refusing to do more, so the store wedges: mutations
	 * fail with -EIO until a remount runs recovery, or a format discards
	 * the store outright. */
#define COMPACT_OR_WEDGE(expr)					\
	do {							\
		rc = (expr);					\
		if (rc < 0) {					\
			LOG_ERR("compact bid=%u: failed inside the atomic "\
				"window (%d); refusing further writes "	\
				"until remount", bid, rc);		\
			st.wedged = true;			\
			return rc;				\
		}						\
	} while (0)

	/* Step 2: bucket. */
	COMPACT_OR_WEDGE(blob_db_store_erase(bucket_off, st.peb_size));
	COMPACT_OR_WEDGE(blob_db_store_write(bucket_off, new_buf, new_len));

	/* Step 3: erase scratch, which closes the window. */
	COMPACT_OR_WEDGE(blob_db_store_erase(scratch_off, st.peb_size));
#undef COMPACT_OR_WEDGE

	return 0;
}

static int compact_bucket(uint16_t bid)
{
	int rc = read_bucket(bid, g_bbuf);
	if (rc < 0) {
		return rc;
	}
	if (!bucket_hdr_valid(g_bbuf, bid)) {
		LOG_WRN("compact: bucket %u not formatted; nothing to do", bid);
		return -EINVAL;
	}

	g_bbuf_new_busy = true;

	off_t stop;
	const size_t new_len = build_compacted_image(g_bbuf, g_bbuf_new, bid,
						     &stop);

	rc = compaction_may_drop_tail(bid, g_bbuf, stop);
	if (rc < 0) {
		g_bbuf_new_busy = false;
		return rc;
	}

	LOG_INF("compact bid=%u: new bucket has %zu B (was up to %zu B)",
		bid, new_len, st.peb_size);
	IO_CAUSE(compactions);
	IO_CAUSE_BYTES(bytes_compacted, new_len);

	rc = compact_commit(bid, g_bbuf_new, new_len);

	/* Every slot moved. The image is still in RAM, so re-index it from
	 * there rather than leave the bucket's lookups to walk. */
	if (rc < 0) {
		blob_db_sidx_forget(bid);
	} else {
		off_t off = BLOB_DB_BUCKET_DATA_OFF;
		struct slot_view sv;

		blob_db_sidx_begin(bid, (uint32_t)off);
		while (off < (off_t)new_len &&
		       slot_view_at(g_bbuf_new, off, &sv)) {
			const bool tomb =
				(sv.flags & BLOB_DB_SLOT_F_TOMBSTONE) != 0;

			blob_db_sidx_note(bid, sv.id, (uint32_t)off,
					  (uint32_t)sv.total_size, tomb);
			off += sv.total_size;
		}
		if (off != (off_t)new_len) {
			blob_db_sidx_forget(bid);
		}
	}

	g_bbuf_new_busy = false;
	return rc;
}

/* The bucket a sealed scratch image belongs to, or -1 when scratch holds no
 * complete image. Reads the 16 B seal first, so a mount with nothing to
 * recover costs one small read; only a seal that verifies costs the whole
 * sector, into g_bbuf, which the caller restores from.
 *
 * The bucket comes from the image's own header, which the seal's CRC covers,
 * not from the master. */
static int scratch_sealed_bucket(size_t *image_len)
{
	const off_t scratch_off = peb_offset(BLOB_DB_SCRATCH_SECTOR);
	struct blob_db_scratch_seal seal;
	int rc = blob_db_store_read(scratch_off + scratch_seal_off(), &seal,
				    sizeof(seal));

	if (rc < 0) {
		return rc;
	}
	if (memcmp(seal.magic, SCRATCH_MAGIC, 4) != 0 ||
	    seal.seal_crc32 != hdr_crc32(&seal, sizeof(seal))) {
		return -1;
	}

	rc = blob_db_store_read(scratch_off, g_bbuf, st.peb_size);
	if (rc < 0) {
		return rc;
	}
	if (!scratch_seal_valid(g_bbuf, image_len)) {
		return -1;   /* the image tore after the seal's own bytes landed */
	}

	const uint16_t bid =
		((const struct blob_db_bucket_hdr *)g_bbuf)->bucket_id;

	if (bid >= st.n_buckets || !bucket_hdr_valid(g_bbuf, bid)) {
		LOG_WRN("recover: sealed scratch names bucket %u, not one of "
			"%u; left as-is", bid, st.n_buckets);
		return -1;
	}
	return bid;
}

/* Mount-time recovery of an interrupted compaction, run at every mount.
 *
 * The scratch seal decides, not the master. compact_commit() seals scratch
 * only after the whole image is in it, and erases scratch before it leaves
 * the window, so a sealed scratch exists exactly while the bucket may be
 * erased or half rewritten, and the image is that bucket's complete new
 * content. Restoring it is right at every point of that window: before the
 * bucket's erase it writes what the bucket was about to receive, and after
 * the bucket was rewritten it writes the same bytes again. A torn image or
 * seal fails the seal's CRCs, and an unsealed scratch means the bucket was
 * never touched.
 *
 * The master holds no compaction state (D17), so nothing a master says, or
 * a rotten master hides, can make mount skip this. The check also runs when
 * the backend replaces atomically, because a store compacted through scratch
 * by a build without atomic replace can carry a sealed scratch into one with
 * it. An unsealed scratch with content is left as it is: the next compaction
 * erases it first. */
static int recover_compaction(void)
{
	const off_t scratch_off = peb_offset(BLOB_DB_SCRATCH_SECTOR);
	size_t image_len = 0;
	int rc = scratch_sealed_bucket(&image_len);

	if (rc < 0) {
		return rc == -1 ? 0 : rc;
	}

	const uint16_t bid = (uint16_t)rc;

	LOG_WRN("recover: scratch sealed for bid %u (%zu B); restoring bucket",
		bid, image_len);
	rc = blob_db_store_erase(bucket_offset(bid), st.peb_size);
	if (rc < 0) {
		return rc;
	}
	/* Only the image: the rest of the bucket must stay erased so appends
	 * can continue after it. */
	rc = blob_db_store_write(bucket_offset(bid), g_bbuf, image_len);
	if (rc < 0) {
		return rc;
	}
	rc = blob_db_store_erase(scratch_off, st.peb_size);
	if (rc == 0) {
		LOG_INF("recover: complete");
	}
	return rc;
}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
/* Large objects — segmented payloads -------------------------------------
 *
 * An object too large for one slot is stored as K segment slots plus an index
 * slot at the object's own id. The index write is the commit: until it lands
 * the new segments are unreferenced, and after it the old ones are. Nothing
 * here needs a new atomicity primitive — a single slot append already is one.
 *
 * Crash recovery is one rule: a SEGMENT slot whose owner's live index does not
 * list it is garbage. It covers all three crash points (mid-segments, torn
 * index, mid-release) because in every one the losing generation is exactly the
 * set no live index references. seg_owner in the master says when to look.
 */

/* Segment-id tables. Sized by segment COUNT, not object size — 16 B per
 * segment all in. Two, because a rebind holds the outgoing and incoming lists
 * at once: the old one must survive until the new index commits so its
 * segments can be released afterwards. */
static uint8_t __aligned(4) g_seg_a[8u * CONFIG_BLOB_DB_MAX_SEGMENTS];
static uint8_t __aligned(4) g_seg_b[8u * CONFIG_BLOB_DB_MAX_SEGMENTS];

/* Derived at mount from the geometry; validated there too. */
static uint16_t g_seg_len;      /* chunk bytes per segment */
static uint16_t g_seg_max;      /* segments an index record can address */

static inline size_t inline_max(void)
{
	return CONFIG_BLOB_DB_MAX_PAYLOAD_LEN;
}

/* Whether this store may hold segmented objects: the build reads them, and
 * the store's headers carry the feature (D20). */
static inline bool seg_enabled(void)
{
	return (st.incompat & BLOB_DB_FEAT_LARGE_PAYLOADS) != 0;
}

/* Resolve segment geometry and refuse a build that cannot reach the configured
 * object size. Runs at mount because peb_size is a runtime property (P2). */
static int seg_geometry_init(void)
{
	size_t sl = CONFIG_BLOB_DB_SEGMENT_LEN;
	const size_t chunk_cap = inline_max() - sizeof(struct blob_db_seg_hdr);

	if (sl == 0) {
		/* A quarter sector leaves several segments plus rebind headroom
		 * in each bucket; chunk sizes that divide a bucket evenly are
		 * denser but hit -ENOSPC abruptly at high fill. */
		sl = st.peb_size / 4;
	}
	if (sl > chunk_cap) {
		sl = chunk_cap;
	}
	if (sl == 0) {
		LOG_ERR("segments: CONFIG_BLOB_DB_MAX_PAYLOAD_LEN=%zu leaves no "
			"room for a chunk", inline_max());
		return -ENOTSUP;
	}

	const size_t idx_cap =
		(inline_max() - sizeof(struct blob_db_index_hdr)) / sizeof(uint64_t);
	const size_t usable = MIN(idx_cap, (size_t)CONFIG_BLOB_DB_MAX_SEGMENTS);
	const size_t reach = usable * sl;

	if (reach < CONFIG_BLOB_DB_MAX_OBJECT_LEN) {
		LOG_ERR("segments: can reach %zu B (%zu segments x %zu B) but "
			"CONFIG_BLOB_DB_MAX_OBJECT_LEN=%u; raise "
			"MAX_PAYLOAD_LEN (index capacity %zu) or MAX_SEGMENTS (%u)",
			reach, usable, sl, CONFIG_BLOB_DB_MAX_OBJECT_LEN,
			idx_cap, CONFIG_BLOB_DB_MAX_SEGMENTS);
		return -ENOTSUP;
	}

	g_seg_len = (uint16_t)sl;
	g_seg_max = (uint16_t)usable;
	LOG_INF("segments: chunk %u B, up to %u per object (max object %zu B)",
		g_seg_len, g_seg_max, reach);
	return 0;
}

#if defined(CONFIG_BLOB_DB_TEST_CRASH_HOOKS)
enum blob_db_test_cut blob_db_test_cut;

void blob_db_test_wedge(void)
{
	blob_db_lock();
	st.wedged = true;
	blob_db_unlock();
}

/* Stop dead at a chosen step of §6.4, leaving flash exactly as a power cut
 * there would. Disarms itself, so one armed cut fires once and the recovery
 * path that follows runs unhooked. */
#define SEG_CUT(step)							\
	do {								\
		if (blob_db_test_cut == (step)) {			\
			blob_db_test_cut = BLOB_DB_CUT_NONE;		\
			LOG_WRN("test: cut after " #step);		\
			return -EINTR;					\
		}							\
	} while (0)
#else
#define SEG_CUT(step)  do { } while (0)
#endif

/* Record a segmented mutation in flight, so mount knows to sweep. Costs one
 * master record, no erase; a segmented update already costs K slot writes. */
static int persist_seg_owner(uint64_t owner)
{
	if (st.seg_owner == owner) {
		return 0;
	}

	const uint64_t prev = st.seg_owner;

	st.seg_owner = owner;

	int rc = master_commit();

	if (rc < 0) {
		st.seg_owner = prev;
		return rc;
	}
	return 0;
}

/* Finish the cleanup a previous segmented op left behind, before this one
 * takes the intent marker over.
 *
 * A segmented write that fails mid-flight — -ENOSPC after the placement probes
 * exhaust, or any I/O error — returns with its half-written segments on flash
 * and seg_owner still naming it, on the reasoning that the sweep tidies up.
 * But the sweep runs only at mount, and if the session simply carries on, the
 * next segmented op's persist_seg_owner() overwrites that name. The master
 * then names nobody who needs sweeping, so no future mount ever reclaims those
 * segments: they are the live latest slot for their id, so compaction
 * preserves them, count()/iterate() cannot see them, and delete() releases
 * only the generation the live index lists. That is a permanent leak, which
 * P7 lists as a must-not.
 *
 * Sweeping here closes it for the non-crash case; mount still covers the crash
 * case. Note this must run before the caller loads any index, because
 * seg_sweep() stages its owner's table in g_seg_a and would otherwise scribble
 * over an old-segment list the caller is about to release.
 */
static int seg_finish_pending(void)
{
	if (st.seg_owner == 0) {
		return 0;
	}

	LOG_WRN("a segmented op for id %llu left its intent behind; sweeping "
		"before starting another",
		(unsigned long long)st.seg_owner);

	return seg_sweep(st.seg_owner);   /* clears seg_owner on success */
}

/* Is this index record self-consistent? The slot CRC only proves the bytes are
 * the ones that were written; it says nothing about whether they make sense —
 * a record rotted in RAM before the write, or produced by a buggy build, still
 * carries a good CRC.
 *
 * The total_len check is the one with teeth. seg_read_range() derives a
 * segment index as `(uint16_t)(pos / seg_len)`, so a total_len that overruns
 * what seg_count segments can hold lets that division truncate and silently
 * return some other segment's bytes as if they were the object's. Bounding
 * total_len to (seg_count-1, seg_count] strides makes that unrepresentable,
 * and turns a corrupt record into -EIO where it is read rather than into
 * plausible wrong data handed to the caller.
 */
static bool index_hdr_valid(const struct blob_db_index_hdr *h, uint16_t val_len)
{
	if (memcmp(h->magic, BLOB_DB_INDEX_MAGIC, 2) != 0 ||
	    h->version != BLOB_DB_INDEX_VERSION) {
		return false;
	}
	if (h->seg_count == 0 || h->seg_count > g_seg_max || h->seg_len == 0) {
		return false;
	}
	if ((size_t)val_len <
	    sizeof(*h) + (size_t)h->seg_count * sizeof(uint64_t)) {
		return false;
	}

	/* seg_count must be exactly ceil(total_len / seg_len). */
	const uint64_t span = (uint64_t)h->seg_count * h->seg_len;

	if (h->total_len == 0 || h->total_len > span ||
	    h->total_len <= span - h->seg_len) {
		return false;
	}
	return true;
}

/* Load the index record for `id`. Returns K (> 0) with the segment ids copied
 * into `ids` and the header into `h`; 0 when the id holds an inline payload;
 * -ENOENT when it holds nothing live; -EIO for a malformed record. */
static int index_load(uint64_t id, uint8_t *ids, struct blob_db_index_hdr *h)
{
	const uint16_t bid = id_to_bucket(id);
	bool formatted;

	int rc = read_bucket_hdr(bid, &formatted);

	if (rc < 0) {
		return rc;
	}
	if (!formatted) {
		return -ENOENT;
	}

	struct bucket_scan sc;

	rc = find_live_slot(bid, id, &sc, g_bbuf);
	if (rc < 0) {
		return rc;
	}
	if (!(sc.target.hdr.flags & BLOB_DB_SLOT_F_INDEXED)) {
		return 0;
	}
	if (sc.target.hdr.val_len < sizeof(*h)) {
		return -EIO;
	}

	memcpy(h, g_bbuf, sizeof(*h));
	if (!index_hdr_valid(h, sc.target.hdr.val_len)) {
		return -EIO;
	}
	memcpy(ids, g_bbuf + sizeof(*h),
	       (size_t)h->seg_count * sizeof(uint64_t));
	return h->seg_count;
}

/* --- index cache ---------------------------------------------------------
 *
 * A windowed sequential read pays for the index record on every call, twice
 * over: blob_db_read() scans the bucket to test the INDEXED flag, and
 * index_load() scans it again to fetch the record. For a 64 B window that is
 * most of the work, and it is repeated for every window of the same object.
 *
 * One entry is enough, because the workload that hurts is repeated access to
 * the *same* object; a second entry would cost RAM to serve a pattern the
 * layers above do not generate.
 *
 * The cache owns g_seg_a: whenever g_idx_id is non-zero, g_seg_a holds that
 * id's segment table. That is sound because index_load() is the only writer
 * of g_seg_a anywhere in the file, so nothing can desynchronise the two — and
 * it is what keeps the feature at ~24 B of .bss instead of a second table.
 *
 * Correctness rests on invalidation, not on cleverness. Any slot append can
 * change what an index says, so append_slot2() — the single write primitive
 * every mutation funnels through — drops the cache. Reads never append, so
 * the cache survives exactly the run of reads it exists to serve.
 *
 * Three more sites invalidate for reasons append_slot2() does not cover:
 * format (erases under us), erase_all (zeroes bucket magics in place, and can
 * return before its closing append), and mount/unmount. The last is defensive
 * and the test suite cannot falsify it — a store's bytes do not change while
 * this process has it unmounted, so a surviving entry would still be accurate.
 * It is kept because that is a property of today's single-instance usage, not
 * a guarantee the cache should depend on.
 */
static uint64_t g_idx_id;                     /* cached owner; 0 = empty */
static struct blob_db_index_hdr g_idx_hdr;
static uint16_t g_idx_k;

static inline void idx_invalidate(void)
{
	g_idx_id = 0;
}

/* index_load() with the cache in front. Same contract: >0 segment count for a
 * segmented object, 0 for an inline blob, negative errno on failure. */
static int index_load_cached(uint64_t id, struct blob_db_index_hdr *h)
{
	if (g_idx_id == id) {
		*h = g_idx_hdr;
		return g_idx_k;
	}

	int k = index_load(id, g_seg_a, h);

	if (k > 0) {
		g_idx_id  = id;
		g_idx_hdr = *h;
		g_idx_k   = (uint16_t)k;
	}
	return k;
}

static inline uint64_t seg_id_at(const uint8_t *ids, uint16_t seq)
{
	uint64_t v;

	memcpy(&v, ids + (size_t)seq * sizeof(uint64_t), sizeof(v));
	return v;
}

/* Copy [skip, skip+len) of segment `seq` into `out`. */
static int seg_chunk_read(uint64_t owner, uint64_t seg_id, uint16_t seq,
			  size_t skip, void *out, size_t len)
{
	const uint16_t bid = id_to_bucket(seg_id);
	bool formatted;

	int rc = read_bucket_hdr(bid, &formatted);

	if (rc < 0) {
		return rc;
	}
	if (!formatted) {
		return -EIO;
	}

	struct bucket_scan sc;

	rc = find_live_slot(bid, seg_id, &sc, g_bbuf);
	if (rc == -ENOENT) {
		LOG_ERR("segment %llu of %llu missing",
			(unsigned long long)seg_id, (unsigned long long)owner);
		return -EIO;
	}
	if (rc < 0) {
		return rc;
	}
	if (!(sc.target.hdr.flags & BLOB_DB_SLOT_F_SEGMENT) ||
	    sc.target.hdr.val_len < sizeof(struct blob_db_seg_hdr)) {
		return -EIO;
	}

	struct blob_db_seg_hdr sh;

	memcpy(&sh, g_bbuf, sizeof(sh));
	if (sh.owner_id != owner || sh.seq != seq) {
		LOG_ERR("segment %llu claims owner %llu seq %u, expected %llu/%u",
			(unsigned long long)seg_id,
			(unsigned long long)sh.owner_id, sh.seq,
			(unsigned long long)owner, seq);
		return -EIO;
	}

	const size_t have = sc.target.hdr.val_len - sizeof(sh);

	if (skip + len > have) {
		return -EIO;
	}
	memcpy(out, g_bbuf + sizeof(sh) + skip, len);
	return 0;
}

/* Append one chunk as a fresh SEGMENT slot, retrying with a new id when the
 * bucket it hashes to is full. A segment id is internal and arbitrary, so a
 * fresh id is simply a different bucket — hash placement becomes a probe
 * sequence, which a user-visible id (whose bucket is fixed) cannot do. */
#define SEG_PLACEMENT_TRIES  8

static int seg_write_chunk(uint64_t owner, uint16_t seq, const void *chunk,
			   uint16_t chunk_len, uint64_t *out_id)
{
	struct blob_db_seg_hdr sh = {
		.owner_id = owner, .seq = seq, .reserved = 0,
	};
	const uint16_t val_len = (uint16_t)(sizeof(sh) + chunk_len);
	const size_t ssz = slot_size_for(val_len);

	for (int try = 0; try < SEG_PLACEMENT_TRIES; try++) {
		const uint64_t sid = blob_db_alloc_id();

		if (sid == 0) {
			return -EIO;
		}

		const uint16_t bid = id_to_bucket(sid);
		bool formatted;
		int rc = read_bucket_hdr(bid, &formatted);

		if (rc < 0) {
			return rc;
		}

		off_t cursor;

		if (!formatted) {
			rc = format_bucket(bid);
			if (rc < 0) {
				return rc;
			}
			cursor = BLOB_DB_BUCKET_DATA_OFF;
		} else {
			rc = walk_cursor(bid, sid, &cursor);
			if (rc < 0) {
				return rc;
			}
		}

		if (cursor + (off_t)ssz > (off_t)st.peb_size) {
			rc = compact_bucket(bid);
			if (rc == 0) {
				rc = walk_cursor(bid, sid, &cursor);
				if (rc < 0) {
					return rc;
				}
			}
			if (cursor + (off_t)ssz > (off_t)st.peb_size) {
				LOG_DBG("segment %u: bucket %u full, reprobing",
					seq, bid);
				continue;   /* fresh id -> different bucket */
			}
		}

		rc = append_slot2(bid, cursor, sid, BLOB_DB_SLOT_F_SEGMENT,
				  &sh, (uint16_t)sizeof(sh), chunk, val_len);
		if (rc < 0) {
			return rc;
		}
		*out_id = sid;
		return 0;
	}

	LOG_ERR("segment %u: no bucket with room after %d probes", seq,
		SEG_PLACEMENT_TRIES);
	return -ENOSPC;
}

/* Tombstone one segment. Returns 1 when the bucket had to be compacted first
 * (so its slots moved), 0 when the tombstone went in without that, and a
 * negative errno when it was not written at all: the segment then stays live,
 * and only a sweep for its owner can still reclaim it.
 *
 * A bucket without room is compacted rather than skipped. The callers clear
 * seg_owner once they return, so a skipped segment is not left to a sweep, it
 * is leaked; and a log ending at a torn slot reads as full (walk_cursor()),
 * which is the common case here, not a rare one. */
static int seg_release_one(uint64_t sid)
{
	const uint16_t bid = id_to_bucket(sid);
	const off_t ssz = (off_t)slot_size_for(0);
	bool formatted;
	bool compacted = false;
	off_t cursor;
	int rc = read_bucket_hdr(bid, &formatted);

	if (rc < 0) {
		return rc;
	}
	if (!formatted) {
		return -ENOENT;
	}
	rc = walk_cursor(bid, sid, &cursor);
	if (rc < 0) {
		return rc;
	}
	if (cursor + ssz > (off_t)st.peb_size) {
		rc = compact_bucket(bid);
		if (rc < 0) {
			return rc;
		}
		compacted = true;
		rc = walk_cursor(bid, sid, &cursor);
		if (rc < 0) {
			return rc;
		}
		if (cursor + ssz > (off_t)st.peb_size) {
			return -ENOSPC;
		}
	}
	rc = append_slot2(bid, cursor, sid,
			  BLOB_DB_SLOT_F_TOMBSTONE | BLOB_DB_SLOT_F_SEGMENT,
			  NULL, 0, NULL, 0);
	if (rc < 0) {
		return rc;
	}
	return compacted ? 1 : 0;
}

/* Tombstone a generation of segments. Best effort: a failure leaves that
 * segment live (see seg_release_one()). */
static void seg_release(const uint8_t *ids, uint16_t k)
{
	for (uint16_t j = 0; j < k; j++) {
		(void)seg_release_one(seg_id_at(ids, j));
	}
}

/* Reclaim every SEGMENT slot owned by `owner` that its live index does not
 * list. Idempotent, O(1) RAM beyond the tables, and the single rule that makes
 * a segmented write crash-safe. Runs only when the master carries an owner. */
static int seg_sweep(uint64_t owner)
{
	struct blob_db_index_hdr h;
	uint16_t k = 0;
	int rc = index_load_cached(owner, &h);

	if (rc > 0) {
		k = (uint16_t)rc;
	} else if (rc < 0 && rc != -ENOENT) {
		LOG_WRN("sweep: owner %llu index unreadable (%d); treating all "
			"its segments as garbage", (unsigned long long)owner, rc);
	}

	size_t dropped = 0;

	for (uint16_t bid = 0; bid < st.n_buckets; bid++) {
		bool formatted;

		if (read_bucket_hdr(bid, &formatted) < 0 || !formatted) {
			continue;
		}

		off_t cursor = BLOB_DB_BUCKET_DATA_OFF;

		for (;;) {
			struct bucket_scan sc;

			/* Walk this bucket one slot at a time by asking for a
			 * target that cannot match, then stepping the cursor
			 * ourselves via a scan limited to what we have seen. */
			struct slot_head sh;

			if (cursor + (off_t)BLOB_DB_SLOT_OVERHEAD >
			    (off_t)st.peb_size) {
				break;
			}
			if (blob_db_store_read(bucket_offset(bid) + cursor, &sh,
					       sizeof(sh)) < 0) {
				break;
			}
			if (sh.hdr.flags == 0xff ||
			    !(sh.hdr.flags & BLOB_DB_SLOT_F_SEALED) ||
			    (sh.hdr.flags & ~(uint8_t)BLOB_DB_SLOT_F_KNOWN) ||
			    sh.hdr.val_len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
				break;
			}

			const size_t ssz = slot_size_for(sh.hdr.val_len);

			if (cursor + (off_t)ssz > (off_t)st.peb_size) {
				break;
			}

			const bool is_seg =
				(sh.hdr.flags & BLOB_DB_SLOT_F_SEGMENT) &&
				!(sh.hdr.flags & BLOB_DB_SLOT_F_TOMBSTONE) &&
				sh.hdr.val_len >= sizeof(struct blob_db_seg_hdr);

			cursor += ssz;

			if (!is_seg) {
				continue;
			}

			/* Only the live slot for this id matters; an older
			 * duplicate is already superseded. */
			rc = find_live_slot(bid, sh.id, &sc, g_bbuf);
			if (rc < 0) {
				continue;
			}
			if (!(sc.target.hdr.flags & BLOB_DB_SLOT_F_SEGMENT) ||
			    sc.target.hdr.val_len <
				    sizeof(struct blob_db_seg_hdr)) {
				continue;
			}

			struct blob_db_seg_hdr seg;

			memcpy(&seg, g_bbuf, sizeof(seg));
			if (seg.owner_id != owner) {
				continue;
			}

			bool listed = false;

			for (uint16_t j = 0; j < k && !listed; j++) {
				listed = (seg_id_at(g_seg_a, j) == sh.id);
			}
			if (listed) {
				continue;
			}

			rc = seg_release_one(sh.id);
			if (rc >= 0) {
				dropped++;
			}
			if (rc == 1) {
				/* Compaction rewrote this bucket under the
				 * walk: start it again. It ends, since every
				 * restart follows a tombstone written, and
				 * the walk skips a tombstoned segment. */
				cursor = BLOB_DB_BUCKET_DATA_OFF;
			} else if (rc < 0 && rc != -ENOENT) {
				/* Not written. Every segment tombstone is the
				 * same size, so none fits here; and if this
				 * compacted, the cursor no longer names a
				 * slot boundary. */
				break;
			}
		}
	}

	LOG_INF("sweep owner=%llu: dropped %zu orphan segment(s)",
		(unsigned long long)owner, dropped);
	return persist_seg_owner(0);
}

/* Chunk staging for partial writes. A chunk that the caller's range only
 * partly covers has to be materialised from two sources before it can be
 * appended, and it cannot share g_bbuf: writing a segment may compact its
 * bucket, and compaction owns g_bbuf. At most two chunks per call are partial
 * (the first and the last), but they still need somewhere to be built. */
static uint8_t __aligned(4) g_chunk[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];

/* Build and commit the index record for `id`. This is THE COMMIT for any
 * segmented write: before it the new segments are unreferenced, after it the
 * old ones are. */
static int index_commit(uint64_t id, const struct blob_db_index_hdr *h,
			const uint8_t *ids, uint16_t k)
{
	const uint16_t idx_len =
		(uint16_t)(sizeof(*h) + (size_t)k * sizeof(uint64_t));
	const uint16_t bid = id_to_bucket(id);
	bool formatted;

	int rc = read_bucket_hdr(bid, &formatted);

	if (rc < 0) {
		return rc;
	}

	off_t cursor;

	if (!formatted) {
		rc = format_bucket(bid);
		if (rc < 0) {
			return rc;
		}
		cursor = BLOB_DB_BUCKET_DATA_OFF;
	} else {
		rc = walk_cursor(bid, id, &cursor);
		if (rc < 0) {
			return rc;
		}
	}

	if (cursor + (off_t)slot_size_for(idx_len) > (off_t)st.peb_size) {
		rc = compact_bucket(bid);
		if (rc < 0) {
			return rc;
		}
		rc = walk_cursor(bid, id, &cursor);
		if (rc < 0) {
			return rc;
		}
		if (cursor + (off_t)slot_size_for(idx_len) > (off_t)st.peb_size) {
			return -ENOSPC;
		}
	}

	return append_slot2(bid, cursor, id, BLOB_DB_SLOT_F_INDEXED, h,
			    (uint16_t)sizeof(*h), ids, idx_len);
}

/* Read [off, off+len) of the object as it stands, from its segments. Anything
 * at or past the current end reads as zeros, which is what makes a write past
 * the end extend the object sparsely rather than expose old bytes. */
static int old_bytes(uint64_t owner, const struct blob_db_index_hdr *h,
		     const uint8_t *ids, uint16_t k, size_t off,
		     uint8_t *out, size_t len)
{
	size_t done = 0;

	while (done < len) {
		const size_t pos = off + done;
		const uint16_t seq = (uint16_t)(pos / h->seg_len);

		if (pos >= h->total_len || seq >= k) {
			memset(out + done, 0, len - done);
			return 0;
		}

		const size_t skip = pos % h->seg_len;
		size_t n = h->seg_len - skip;

		if (n > len - done) {
			n = len - done;
		}
		if (pos + n > h->total_len) {
			n = h->total_len - pos;
		}

		int rc = seg_chunk_read(owner, seg_id_at(ids, seq), seq, skip,
					out + done, n);
		if (rc < 0) {
			return rc;
		}
		done += n;
	}
	return 0;
}

/* Partial write into a segmented object: rewrite only the segments the range
 * touches, keep the rest, and commit a new index naming the mixture.
 *
 * seg_len is inherited from the existing index, never recomputed — the
 * surviving segments are already cut at that stride, so changing it would
 * misplace every byte they hold.
 */
static int pwrite_segmented(uint64_t id, size_t offset, const void *buf,
			    size_t len)
{
	struct blob_db_index_hdr h;
	int k = index_load_cached(id, &h);

	if (k < 0) {
		return k;
	}
	if (k == 0) {
		return -EIO;   /* caller already established it is indexed */
	}

	const uint16_t seg_len = h.seg_len;
	const size_t new_total = MAX((size_t)h.total_len, offset + len);
	const size_t new_k = (new_total + seg_len - 1) / seg_len;

	if (new_total > CONFIG_BLOB_DB_MAX_OBJECT_LEN) {
		return -EFBIG;
	}
	if (new_k > g_seg_max) {
		return -EFBIG;
	}
	if (seg_len > sizeof(g_chunk)) {
		return -EIO;   /* index written by a wider build */
	}

	int rc = persist_seg_owner(id);

	if (rc < 0) {
		return rc;
	}

	/* The new table starts as the old one and diverges only where a segment
	 * is rewritten, so untouched segments keep their ids and their slots. */
	memcpy(g_seg_b, g_seg_a, (size_t)k * sizeof(uint64_t));

	const uint8_t *src = buf;

	for (size_t j = 0; j < new_k; j++) {
		const size_t cstart = j * seg_len;
		const size_t cend = MIN(cstart + seg_len, new_total);
		const size_t clen = cend - cstart;
		const bool touched = (offset < cend) && (offset + len > cstart);
		const bool fresh = (j >= (size_t)k);

		/* A chunk whose *length* changes must be rewritten even if the
		 * caller's range never reaches it. Growing an object whose last
		 * chunk was short — total_len not a multiple of seg_len — makes
		 * that chunk a full-stride interior chunk, and leaving it at its
		 * old length while the committed index says otherwise corrupts
		 * the object: every later read of that region fails the segment
		 * bound check with -EIO, and so does any repair attempt.
		 *
		 * Only the old final chunk can be affected (new_total never
		 * shrinks), but the condition is written generally so it stays
		 * true if that ever changes. */
		const size_t old_clen =
			fresh ? 0
			      : MIN(cstart + seg_len, (size_t)h.total_len) -
					cstart;
		const bool resized = !fresh && (old_clen != clen);

		if (!touched && !fresh && !resized) {
			continue;
		}

		if (touched && offset <= cstart && offset + len >= cend) {
			/* Fully overwritten: the caller's bytes are the chunk. */
			memcpy(g_chunk, src + (cstart - offset), clen);
		} else {
			rc = old_bytes(id, &h, g_seg_a, (uint16_t)k, cstart,
				       g_chunk, clen);
			if (rc < 0) {
				return rc;
			}
			if (touched) {
				const size_t s = MAX(offset, cstart);
				const size_t e = MIN(offset + len, cend);

				memcpy(g_chunk + (s - cstart),
				       src + (s - offset), e - s);
			}
		}

		uint64_t sid;

		rc = seg_write_chunk(id, (uint16_t)j, g_chunk, (uint16_t)clen,
				     &sid);
		if (rc < 0) {
			return rc;   /* seg_owner stays set; the sweep tidies up */
		}
		memcpy(g_seg_b + j * sizeof(uint64_t), &sid, sizeof(sid));
	}

	struct blob_db_index_hdr nh = {
		.version       = BLOB_DB_INDEX_VERSION,
		.flags         = 0,   /* payload_crc32 no longer covers the object */
		.total_len     = (uint32_t)new_total,
		.payload_crc32 = 0,
		.seg_count     = (uint16_t)new_k,
		.seg_len       = seg_len,
	};

	memcpy(nh.magic, BLOB_DB_INDEX_MAGIC, 2);

	rc = index_commit(id, &nh, g_seg_b, (uint16_t)new_k);
	if (rc < 0) {
		return rc;
	}

	/* Release exactly the segments that were replaced. */
	for (uint16_t j = 0; j < (uint16_t)k; j++) {
		const uint64_t was = seg_id_at(g_seg_a, j);

		if (was != seg_id_at(g_seg_b, j)) {
			uint8_t one[sizeof(uint64_t)];

			memcpy(one, &was, sizeof(one));
			seg_release(one, 1);
		}
	}
	return persist_seg_owner(0);
}

/* Write `len` bytes at `id` as K segments plus an index record. `old_ids`
 * holds the outgoing generation (old_k entries) to release after the commit. */
static int seg_update(uint64_t id, const void *payload, size_t len,
		      const uint8_t *old_ids, uint16_t old_k)
{
	const uint8_t *src = payload;
	const size_t k = (len + g_seg_len - 1) / g_seg_len;

	if (len > CONFIG_BLOB_DB_MAX_OBJECT_LEN) {
		return -EFBIG;
	}
	if (k > g_seg_max) {
		return -EFBIG;
	}

	int rc = persist_seg_owner(id);

	if (rc < 0) {
		return rc;
	}
	SEG_CUT(BLOB_DB_CUT_AFTER_OWNER_SET);   /* step 1 */

	for (size_t j = 0; j < k; j++) {
		const size_t n = MIN((size_t)g_seg_len, len - j * g_seg_len);
		uint64_t sid;

		rc = seg_write_chunk(id, (uint16_t)j, src + j * g_seg_len,
				     (uint16_t)n, &sid);
		if (rc < 0) {
			return rc;   /* seg_owner stays set; sweep cleans up */
		}
		memcpy(g_seg_b + j * sizeof(uint64_t), &sid, sizeof(sid));

		if (j == 0 && k > 1) {
			SEG_CUT(BLOB_DB_CUT_MID_SEGMENTS);   /* inside step 2 */
		}
	}
	SEG_CUT(BLOB_DB_CUT_AFTER_SEGMENTS);    /* step 2 */

	struct blob_db_index_hdr h = {
		.version       = BLOB_DB_INDEX_VERSION,
		.flags         = BLOB_DB_INDEX_F_CRC32,
		.total_len     = (uint32_t)len,
		.payload_crc32 = crc32_ieee(src, len),
		.seg_count     = (uint16_t)k,
		.seg_len       = g_seg_len,
	};

	memcpy(h.magic, BLOB_DB_INDEX_MAGIC, 2);

	rc = index_commit(id, &h, g_seg_b, (uint16_t)k);
	if (rc < 0) {
		return rc;
	}
	SEG_CUT(BLOB_DB_CUT_AFTER_COMMIT);      /* step 3 — the commit point */

	if (old_k > 0) {
		seg_release(old_ids, old_k);
	}
	SEG_CUT(BLOB_DB_CUT_AFTER_RELEASE);     /* step 4 */

	return persist_seg_owner(0);            /* step 5 */
}
#endif /* CONFIG_BLOB_DB_LARGE_PAYLOADS */

static uint64_t alloc_id_impl(void)
{
	if (!st.mounted) {
		return 0;   /* 0 is never a valid id */
	}

	/* Guarantee durability before returning: the id we hand out must be
	 * strictly below a persisted ceiling, so a crash before it is ever
	 * bound cannot let a later mount re-issue it (contract §2, §13.1). */
	if (st.next_id >= st.next_id_hint) {
		int rc = persist_next_id_hint();
		if (rc < 0) {
			LOG_ERR("alloc_id: cannot persist ceiling: %d", rc);
			return 0;
		}
	}

	const uint64_t id = st.next_id;

	st.next_id = id + 1;
	LOG_DBG("alloc_id -> %llu (ceiling %llu)",
		(unsigned long long)id, (unsigned long long)st.next_id_hint);
	return id;
}

static int get_impl(uint64_t id, void *out, size_t out_sz, size_t *out_len)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (out_sz > 0 && !out) {
		return -EINVAL;
	}
	if (id == 0) {
		return -ENOENT;
	}

	const uint16_t bid = id_to_bucket(id);
	bool formatted;

	int rc = read_bucket_hdr(bid, &formatted);
	if (rc < 0) {
		return rc;
	}
	if (!formatted) {
		return -ENOENT;
	}

	/* Staged in g_bbuf so the CRC covers the bytes before anything is
	 * handed to the caller, and so a short out_sz leaves `out` untouched. */
	struct bucket_scan sc;

	rc = find_live_slot(bid, id, &sc, g_bbuf);
	if (rc < 0) {
		return rc;
	}
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	if (sc.target.hdr.flags & BLOB_DB_SLOT_F_SEGMENT) {
		return -ENOENT;   /* an internal id — see §6.6 */
	}
	if (sc.target.hdr.flags & BLOB_DB_SLOT_F_INDEXED) {
		struct blob_db_index_hdr h;
		int k = index_load_cached(id, &h);

		if (k < 0) {
			return k;
		}
		if (out_sz < h.total_len) {
			if (out_len) {
				*out_len = h.total_len;
			}
			return -ENOMEM;
		}
		rc = blob_db_read(id, 0, out, h.total_len, out_len);
		return rc;
	}
#endif
	if (out_sz < sc.target.hdr.val_len) {
		return -ENOMEM;
	}
	if (sc.target.hdr.val_len > 0) {
		memcpy(out, g_bbuf, sc.target.hdr.val_len);
	}
	if (out_len) {
		*out_len = sc.target.hdr.val_len;
	}
	return 0;
}

static int update_impl(uint64_t id, const void *payload, size_t len)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (st.wedged) {
		return -EIO;   /* torn compaction; see compact_commit() */
	}
	/* An id is valid iff alloc_id has handed it out (0 < id < next_id).
	 * Anything else was never allocated — contract §2 makes this UB, and
	 * this cheap bound is the fail-fast the narrow contract permits. */
	if (id == 0 || id >= st.next_id) {
		return -EINVAL;
	}
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	if (len > CONFIG_BLOB_DB_MAX_OBJECT_LEN) {
		return -EINVAL;
	}
	/* A store formatted without large payloads is written as a build
	 * without them would write it (D20): its headers do not carry the
	 * feature, so a build that cannot read segments may mount it. */
	if (len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN && !seg_enabled()) {
		return -EINVAL;
	}
#else
	if (len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
		return -EINVAL;
	}
#endif
	if (len > 0 && !payload) {
		return -EINVAL;
	}

	/* Bind (first write) and rebind (rewrite) are the same append path:
	 * write the new slot; latest-wins (§4) makes it the live one. We do
	 * NOT verify prior state — a first bind has no prior slot, and writing
	 * to a dead id is UB (decision D3), not our job to catch here. */
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	{
		/* Must precede any index load: seg_sweep() stages its owner's
		 * segment table in g_seg_a. */
		int prc = seg_finish_pending();

		if (prc < 0) {
			return prc;
		}
	}
#endif

	const uint16_t bid = id_to_bucket(id);
	bool formatted;

	int rc = read_bucket_hdr(bid, &formatted);
	if (rc < 0) {
		return rc;
	}

	off_t write_cursor;
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	/* An outgoing segmented generation has to be captured before the commit
	 * replaces the index that names it, and released after. */
	uint16_t old_k = 0;
#endif

	if (!formatted) {
		rc = format_bucket(bid);
		if (rc < 0) {
			return rc;
		}
		write_cursor = BLOB_DB_BUCKET_DATA_OFF;
	} else {
		struct bucket_scan sc;

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
		/* The CRC-verified lookup, not the bare cursor scan. Deciding
		 * "was the outgoing slot INDEXED?" from scan_bucket_for() is
		 * unsound: it accepts a target on header sanity alone, so a
		 * torn append — header and id programmed, payload and CRC still
		 * erased — presents itself as the target carrying its own
		 * flags. Reading those as "not indexed" over a live index
		 * record loses the whole outgoing segment generation: no index
		 * names it afterwards, so no delete and no sweep ever reclaims
		 * it. delete() and write() already take this path; update() was
		 * the outlier.
		 *
		 * -ENOENT is ordinary — a first bind, or a rebind after delete.
		 * There is no outgoing generation then, and write_cursor is
		 * still the append point either way.
		 */
		rc = find_live_slot(bid, id, &sc, g_bbuf);
		if (rc < 0 && rc != -ENOENT) {
			return rc;
		}

		if (rc == 0 &&
		    (sc.target.hdr.flags & BLOB_DB_SLOT_F_SEGMENT)) {
			return -EINVAL;   /* an internal id — see §6.6 */
		}

		const bool indexed =
			(rc == 0) &&
			(sc.target.hdr.flags & BLOB_DB_SLOT_F_INDEXED) != 0;
#else
		/* Only the append cursor is needed: latest-wins (§4) makes the
		 * new slot live without consulting the old one. */
		rc = bucket_cursor(bid, id, &sc);
		if (rc < 0) {
			return rc;
		}
#endif
		{
			int crc = confirm_cursor(bid, id, &sc);

			if (crc < 0) {
				return crc;
			}
		}
		write_cursor = sc.write_cursor;
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
		if (indexed) {
			struct blob_db_index_hdr oh;
			int k = index_load_cached(id, &oh);

			if (k > 0) {
				old_k = (uint16_t)k;
			}
		}
#endif
	}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	if (len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
		return seg_update(id, payload, len, g_seg_a, old_k);
	}
#endif

	const size_t ssz = slot_size_for((uint16_t)len);

	if (write_cursor + ssz > st.peb_size) {
		rc = compact_bucket(bid);
		if (rc < 0) {
			return rc;
		}

		struct bucket_scan sc;

		rc = bucket_cursor(bid, id, &sc);
		if (rc < 0) {
			return rc;
		}
		rc = confirm_cursor(bid, id, &sc);
		if (rc < 0) {
			return rc;
		}
		write_cursor = sc.write_cursor;
		if (write_cursor + ssz > st.peb_size) {
			return -ENOSPC;
		}
	}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	/* Shrinking a segmented object to an inline one: same commit-then-release
	 * order, so a crash still leaves exactly one referenced generation. */
	if (old_k > 0) {
		rc = persist_seg_owner(id);
		if (rc < 0) {
			return rc;
		}
	}
#endif

	rc = append_slot(bid, write_cursor, id, 0, payload, (uint16_t)len);
	if (rc < 0) {
		return rc;
	}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	if (old_k > 0) {
		seg_release(g_seg_a, old_k);
		rc = persist_seg_owner(0);
		if (rc < 0) {
			return rc;
		}
	}
#endif

	LOG_DBG("update id=%llu bid=%u off=0x%lx len=%zu",
		(unsigned long long)id, bid,
		(unsigned long)write_cursor, len);
	return 0;
}

static int delete_impl(uint64_t id)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (st.wedged) {
		return -EIO;   /* torn compaction; see compact_commit() */
	}
	if (id == 0) {
		return -ENOENT;
	}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	{
		/* Must precede any index load: seg_sweep() stages its owner's
		 * segment table in g_seg_a. */
		int prc = seg_finish_pending();

		if (prc < 0) {
			return prc;
		}
	}
#endif

	const uint16_t bid = id_to_bucket(id);
	bool formatted;

	int rc = read_bucket_hdr(bid, &formatted);
	if (rc < 0) {
		return rc;
	}
	if (!formatted) {
		return -ENOENT;
	}

	struct bucket_scan sc;

	rc = find_live_slot(bid, id, &sc, g_bbuf);
	if (rc < 0) {
		return rc;
	}
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	if (sc.target.hdr.flags & BLOB_DB_SLOT_F_SEGMENT) {
		return -EINVAL;   /* an internal id — see §6.6 */
	}
#endif

	const size_t ssz = slot_size_for(0);

	rc = confirm_cursor(bid, id, &sc);
	if (rc < 0) {
		return rc;
	}

	off_t write_cursor = sc.write_cursor;

	if (write_cursor + ssz > st.peb_size) {
		rc = compact_bucket(bid);
		if (rc < 0) {
			return rc;
		}
		rc = find_live_slot(bid, id, &sc, g_bbuf);
		if (rc == -ENOENT) {
			/* Compaction dropped this id — already deleted. */
			return -ENOENT;
		}
		if (rc < 0) {
			return rc;
		}
		rc = confirm_cursor(bid, id, &sc);
		if (rc < 0) {
			return rc;
		}
		write_cursor = sc.write_cursor;
		if (write_cursor + ssz > st.peb_size) {
			return -ENOSPC;
		}
	}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	uint16_t old_k = 0;

	if (sc.target.hdr.flags & BLOB_DB_SLOT_F_INDEXED) {
		struct blob_db_index_hdr oh;
		int k = index_load_cached(id, &oh);

		if (k > 0) {
			old_k = (uint16_t)k;
			rc = persist_seg_owner(id);
			if (rc < 0) {
				return rc;
			}
			SEG_CUT(BLOB_DB_CUT_AFTER_OWNER_SET);
		}
	}
#endif

	rc = append_slot(bid, write_cursor, id,
			 BLOB_DB_SLOT_F_TOMBSTONE, NULL, 0);
	if (rc < 0) {
		return rc;
	}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	/* Tombstone first, then the segments: after the commit no live index
	 * names them, so a crash here leaves work the sweep already knows how
	 * to finish. */
	if (old_k > 0) {
		SEG_CUT(BLOB_DB_CUT_AFTER_COMMIT);

		seg_release(g_seg_a, old_k);
		SEG_CUT(BLOB_DB_CUT_AFTER_RELEASE);

		rc = persist_seg_owner(0);
		if (rc < 0) {
			return rc;
		}
	}
#endif

	LOG_DBG("delete id=%llu bid=%u off=0x%lx",
		(unsigned long long)id, bid,
		(unsigned long)write_cursor);
	return 0;
}

static bool exists_impl(uint64_t id)
{
	if (!st.mounted || id == 0) {
		return false;
	}

	const uint16_t bid = id_to_bucket(id);
	bool formatted;

	if (read_bucket_hdr(bid, &formatted) < 0 || !formatted) {
		return false;
	}

	struct bucket_scan sc;

	if (find_live_slot(bid, id, &sc, g_bbuf) != 0) {
		return false;
	}
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	if (sc.target.hdr.flags & BLOB_DB_SLOT_F_SEGMENT) {
		return false;   /* an internal id — see §6.6 */
	}
#endif
	return true;
}

/* Stage 6 — format / count / iterate ------------------------------------ */

/* Walk a bucket buffer, invoking `cb` for each *live* slot — the latest
 * intact, non-tombstoned slot for its id within this bucket. O(n²) over
 * slots in the bucket; the per-bucket count is small in normal use. Returns
 * 0 if walk completed, or the non-zero value the callback returned to abort. */
typedef int (*live_slot_visit_t)(uint64_t id, const uint8_t *payload,
				  uint16_t val_len, void *user);

static int for_each_live_slot(const uint8_t *buf,
			      live_slot_visit_t cb, void *user)
{
	off_t cursor = BLOB_DB_BUCKET_DATA_OFF;

	for (;;) {
		struct slot_view sv;
		bool intact;

		if (!slot_walk_at(buf, cursor, &sv, &intact)) {
			break;
		}
		if (!intact) {
			cursor += sv.total_size;
			continue;
		}

		const bool superseded = slot_superseded(buf, cursor, &sv);

		/* A SEGMENT slot is a chunk of another object, not an object.
		 * Reporting it would make count() and iterate() describe storage
		 * rather than content, and hand fsck bytes it cannot interpret. */
		const bool internal =
			IS_ENABLED(CONFIG_BLOB_DB_LARGE_PAYLOADS) &&
			(sv.flags & BLOB_DB_SLOT_F_SEGMENT);

		if (!superseded && !internal &&
		    !(sv.flags & BLOB_DB_SLOT_F_TOMBSTONE)) {
			const uint8_t *payload = buf + cursor +
						 sizeof(struct blob_db_slot_hdr) +
						 sizeof(uint64_t);
			int r = cb(sv.id, payload, sv.val_len, user);

			if (r) {
				return r;
			}
		}

		cursor += sv.total_size;
	}
	return 0;
}

static int count_visitor(uint64_t id, const uint8_t *p, uint16_t l, void *user)
{
	ARG_UNUSED(id);
	ARG_UNUSED(p);
	ARG_UNUSED(l);
	(*(size_t *)user)++;
	return 0;
}

static size_t count_impl(void)
{
	if (!st.mounted) {
		return 0;
	}

	size_t total = 0;

	for (uint16_t bid = 0; bid < st.n_buckets; bid++) {
		if (read_bucket(bid, g_bbuf) < 0) {
			continue;
		}
		if (!bucket_hdr_valid(g_bbuf, bid)) {
			continue;
		}
		/* count() has no error to return. A bucket holding a foreign
		 * slot counts the objects before it, as a bucket it cannot read
		 * counts none (see blob_db_count()). */
		(void)for_each_live_slot(g_bbuf, count_visitor, &total);
	}
	return total;
}

struct iterate_trampoline {
	blob_db_iter_cb_t user_cb;
	void *user;
};

static int iterate_visitor(uint64_t id, const uint8_t *p, uint16_t l, void *u)
{
	struct iterate_trampoline *t = u;

	return t->user_cb(id, p, l, t->user);
}

static int iterate_impl(blob_db_iter_cb_t cb, void *user)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (!cb) {
		return -EINVAL;
	}

	struct iterate_trampoline t = { .user_cb = cb, .user = user };

	for (uint16_t bid = 0; bid < st.n_buckets; bid++) {
		if (read_bucket(bid, g_bbuf) < 0) {
			continue;
		}
		if (!bucket_hdr_valid(g_bbuf, bid)) {
			continue;
		}

		/* Checked before any callback: the slots before a foreign
		 * one may be superseded by slots past it. */
		if (bucket_image_ends_foreign(g_bbuf)) {
			LOG_ERR("iterate: bucket %u holds a slot with a flag this "
				"build does not know", bid);
			return -EIO;
		}

		int rc = for_each_live_slot(g_bbuf, iterate_visitor, &t);

		if (rc) {
			return rc;  /* user-requested early stop */
		}
	}
	return 0;
}

#if defined(CONFIG_BLOB_DB_INSPECT)
/* Hooks for blob_db_inspect.c — see blob_db_internal.h. */

void blob_db_core_state_get(struct blob_db_core_state *out)
{
	*out = (struct blob_db_core_state){
		.mounted       = st.mounted,
		.wedged        = st.wedged,
		.fa_size       = st.fa_size,
		.peb_size      = st.peb_size,
		.write_align   = st.write_align,
		.n_pebs        = st.n_pebs,
		.n_buckets     = st.n_buckets,
		.active_master = st.active_master,
		.rot_gen       = st.rot_gen,
		.rec_size      = st.rec_size,
		.log_used      = st.log_next,
		.log_cap       = st.log_cap,
		.incompat      = st.incompat,
		.ro_compat     = st.ro_compat,
		.compat        = st.compat,
		.suite         = BLOB_DB_BUILD_SUITE,
		.fuses         = st.fuses,
		.next_id       = st.next_id,
		.next_id_hint  = st.next_id_hint,
		.seg_owner     = st.seg_owner,
	};
}

uint8_t *blob_db_core_sector_buf(void)
{
	return g_bbuf;
}

bool blob_db_core_slot_at(const uint8_t *buf, off_t off,
			  struct blob_db_core_slot *out)
{
	struct slot_view sv;
	bool intact;

	if (!slot_walk_at(buf, off, &sv, &intact)) {
		return false;
	}
	*out = (struct blob_db_core_slot){
		.id         = sv.id,
		.flags      = sv.flags,
		.val_len    = sv.val_len,
		.total_size = sv.total_size,
		.intact     = intact,
	};
	return true;
}

bool blob_db_core_bucket_hdr_valid(const uint8_t *buf, uint16_t bid)
{
	return bucket_hdr_valid(buf, bid);
}
#endif /* CONFIG_BLOB_DB_INSPECT */

static int size_impl(uint64_t id, size_t *out_size)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (!out_size) {
		return -EINVAL;
	}
	if (id == 0) {
		return -ENOENT;
	}

	const uint16_t bid = id_to_bucket(id);
	bool formatted;
	int rc = read_bucket_hdr(bid, &formatted);

	if (rc < 0) {
		return rc;
	}
	if (!formatted) {
		return -ENOENT;
	}

	struct bucket_scan sc;

	rc = find_live_slot(bid, id, &sc, g_bbuf);
	if (rc < 0) {
		return rc;
	}
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	if (sc.target.hdr.flags & BLOB_DB_SLOT_F_SEGMENT) {
		return -ENOENT;   /* an internal id — see §6.6 */
	}
	if (sc.target.hdr.flags & BLOB_DB_SLOT_F_INDEXED) {
		struct blob_db_index_hdr h;

		/* Same validation index_load() applies: reporting a size taken
		 * from an unchecked record would have the caller allocate
		 * against a number a subsequent read then refuses to honour. */
		if (sc.target.hdr.val_len < sizeof(h)) {
			return -EIO;
		}
		memcpy(&h, g_bbuf, sizeof(h));
		if (!index_hdr_valid(&h, sc.target.hdr.val_len)) {
			return -EIO;
		}
		*out_size = h.total_len;
		return 0;
	}
#endif
	*out_size = sc.target.hdr.val_len;
	return 0;
}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
/* Copy [offset, offset+len) of a segmented object into `out`, clamped to its
 * length. Split out of blob_db_read() so the cache fast path below can reach
 * it without repeating the bucket scan that proved the object segmented.
 * Requires g_seg_a to hold `h`'s segment table — the index-cache invariant. */
static int seg_read_range(uint64_t id, const struct blob_db_index_hdr *h,
			  size_t offset, void *out, size_t len,
			  size_t *out_read)
{
	if (offset >= h->total_len) {
		if (out_read) {
			*out_read = 0;
		}
		return 0;
	}
	if (len > h->total_len - offset) {
		len = h->total_len - offset;
	}

	uint8_t *dst = out;
	size_t done = 0;

	while (done < len) {
		const size_t pos   = offset + done;
		const uint16_t seq = (uint16_t)(pos / h->seg_len);
		const size_t skip  = pos % h->seg_len;
		size_t n = h->seg_len - skip;

		if (n > len - done) {
			n = len - done;
		}

		int rc = seg_chunk_read(id, seg_id_at(g_seg_a, seq), seq,
					skip, dst + done, n);

		if (rc < 0) {
			return rc;
		}
		done += n;
	}
	if (out_read) {
		*out_read = done;
	}
	return 0;
}
#endif

static int read_impl(uint64_t id, size_t offset, void *out, size_t len,
		     size_t *out_read)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (len > 0 && !out) {
		return -EINVAL;
	}
	if (id == 0) {
		return -ENOENT;
	}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	/* A cache hit already proves this id is segmented and already holds its
	 * index, so both bucket scans below — the flag test and the index fetch
	 * — would only re-derive what is in RAM. This is the path a windowed
	 * sequential read takes on every call after the first. */
	if (g_idx_id == id) {
		return seg_read_range(id, &g_idx_hdr, offset, out, len,
				      out_read);
	}
#endif

	const uint16_t bid = id_to_bucket(id);
	bool formatted;
	int rc = read_bucket_hdr(bid, &formatted);

	if (rc < 0) {
		return rc;
	}
	if (!formatted) {
		return -ENOENT;
	}

	struct bucket_scan sc;

	rc = find_live_slot(bid, id, &sc, g_bbuf);
	if (rc < 0) {
		return rc;
	}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	if (sc.target.hdr.flags & BLOB_DB_SLOT_F_SEGMENT) {
		return -ENOENT;   /* an internal id — see §6.6 */
	}
	if (sc.target.hdr.flags & BLOB_DB_SLOT_F_INDEXED) {
		struct blob_db_index_hdr h;
		int k = index_load_cached(id, &h);

		if (k < 0) {
			return k;
		}
		return seg_read_range(id, &h, offset, out, len, out_read);
	}
#endif

	/* Inline payload: it is already staged, so this is a bounded copy. */
	const uint16_t have = sc.target.hdr.val_len;

	if (offset >= have) {
		if (out_read) {
			*out_read = 0;
		}
		return 0;
	}
	if (len > (size_t)have - offset) {
		len = (size_t)have - offset;
	}
	if (len > 0) {
		memcpy(out, g_bbuf + offset, len);
	}
	if (out_read) {
		*out_read = len;
	}
	return 0;
}

static int write_impl(uint64_t id, size_t offset, const void *buf,
		      size_t len)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (st.wedged) {
		return -EIO;   /* torn compaction; see compact_commit() */
	}
	if (len > 0 && !buf) {
		return -EINVAL;
	}
	if (id == 0) {
		return -ENOENT;
	}
	if (offset + len < offset) {
		return -EINVAL;   /* range wraps */
	}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	{
		/* Must precede any index load: seg_sweep() stages its owner's
		 * segment table in g_seg_a. */
		int prc = seg_finish_pending();

		if (prc < 0) {
			return prc;
		}
	}
#endif

	const uint16_t bid = id_to_bucket(id);
	bool formatted;
	int rc = read_bucket_hdr(bid, &formatted);

	if (rc < 0) {
		return rc;
	}
	if (!formatted) {
		return -ENOENT;
	}

	struct bucket_scan sc;

	rc = find_live_slot(bid, id, &sc, g_bbuf);
	if (rc < 0) {
		return rc;
	}
	if (sc.target.hdr.flags & BLOB_DB_SLOT_F_SEGMENT) {
		return -EINVAL;   /* an internal id — see §6.6 */
	}

	/* Only now: a zero-length write is a no-op on an object that exists,
	 * but it must not report success for one that does not. Returning
	 * early on len == 0 made write() the only accessor that answered 0
	 * where read() and size() answer -ENOENT. */
	if (len == 0) {
		return 0;
	}

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	if (sc.target.hdr.flags & BLOB_DB_SLOT_F_INDEXED) {
		return pwrite_segmented(id, offset, buf, len);
	}
#endif

	/* Inline payload: read-modify-write the slot. */
	const size_t have = sc.target.hdr.val_len;
	const size_t need = MAX(offset + len, have);

	if (need > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
		/* Growing an inline payload past a single slot would mean
		 * converting it to a segmented object mid-write. Deliberately
		 * not done here: the object is still small enough for the caller
		 * to hold, so promotion is a get() + update() away, and doing it
		 * in the caller keeps a second full-object staging buffer out of
		 * the library. */
		return -EFBIG;
	}

	/* Make room BEFORE staging: compaction owns g_bbuf, which is where the
	 * payload we are about to modify lives. */
	const size_t ssz = slot_size_for((uint16_t)need);

	rc = confirm_cursor(bid, id, &sc);
	if (rc < 0) {
		return rc;
	}
	if (sc.write_cursor + (off_t)ssz > (off_t)st.peb_size) {
		rc = compact_bucket(bid);
		if (rc < 0) {
			return rc;
		}
		rc = find_live_slot(bid, id, &sc, g_bbuf);
		if (rc < 0) {
			return rc;
		}
		rc = confirm_cursor(bid, id, &sc);
		if (rc < 0) {
			return rc;
		}
		if (sc.write_cursor + (off_t)ssz > (off_t)st.peb_size) {
			return -ENOSPC;
		}
	}

	/* Zero anything between the old end and the new one, so a write past
	 * the end extends with zeros rather than exposing stale bytes. */
	if (need > have) {
		memset(g_bbuf + have, 0, need - have);
	}
	memcpy(g_bbuf + offset, buf, len);

	return append_slot(bid, sc.write_cursor, id, 0, g_bbuf,
			   (uint16_t)need);
}

/* Erase the whole open store and rebuild it empty: masters, root, nothing
 * else. Shared by blob_db_format() and by mount's opt-in recovery from
 * unusable masters (CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT). */
static int format_whole_store(void)
{
	IDX_INVALIDATE();
	blob_db_sidx_reset(st.n_buckets);
	st.wedged = false;   /* the whole store, scratch included, goes away */

	int rc = blob_db_store_erase(0, st.fa_size);

	if (rc < 0) {
		LOG_ERR("format: erase: %d", rc);
		return rc;
	}

	rc = master_format();
	if (rc < 0) {
		return rc;
	}
	return bind_root_empty();
}

static int format_impl(void)
{
	bool opened_here = false;
	int rc;

	/* Formatting is the deliberate-discard path, so it must also work on a
	 * store this build cannot mount — that is the escape hatch mount's
	 * -ENOTSUP names, and it was unreachable while this required a mount:
	 * the one store needing discard is the one that cannot be mounted.
	 * Nothing above blob_db can open the substrate on our behalf, so open
	 * it here, with the same geometry checks a mount applies. */
	if (!st.mounted) {
		rc = store_open_and_validate(true);
		if (rc < 0) {
			return rc;
		}
		opened_here = true;
	}

	rc = format_whole_store();
	if (rc < 0) {
		goto err;
	}

	if (opened_here) {
		/* The store is now in exactly the state a virgin mount would
		 * have produced, so leave it mounted rather than making the
		 * caller mount a store we just built. */
		st.mounted = true;
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
		rc = seg_geometry_init();
		if (rc < 0) {
			st.mounted = false;
			goto err;
		}
#endif
	}

	LOG_INF("format: complete; next_id=%llu",
		(unsigned long long)st.next_id);
	return 0;

err:
	/* Only unwind what this call set up: a format that fails on an already
	 * mounted store leaves it mounted, as it always has. */
	if (opened_here) {
		blob_db_store_close();
	}
	return rc;
}

/* Format the root bucket (erase + write header) and append an empty slot
 * for BLOB_DB_ROOT_ID. See @blob_db_root in blob_db.h. Callers must have
 * already set up the store / st.peb_size / st.n_buckets. */
static int bind_root_empty(void)
{
	const uint16_t root_bid = (uint16_t)(BLOB_DB_ROOT_ID % st.n_buckets);

	int rc = format_bucket(root_bid);
	if (rc < 0) {
		LOG_ERR("bind_root: format bucket %u: %d", root_bid, rc);
		return rc;
	}
	rc = append_slot(root_bid, BLOB_DB_BUCKET_DATA_OFF,
			 BLOB_DB_ROOT_ID, 0, NULL, 0);
	if (rc < 0) {
		LOG_ERR("bind_root: append: %d", rc);
	}
	return rc;
}

/* Invalidate the header of every bucket that has one, except @p skip_bid
 * (-1: none).
 *
 * BUCKET_MAGIC is 'B'/'D'/'B'/'H' = 0x42/0x44/0x42/0x48; overwriting it with
 * 0x00 only flips 1→0 bits — no erase needed, and every scan afterwards treats
 * the sector as unformatted. format_bucket() replaces the whole sector before
 * the bucket is used again, so nothing behind the header can resurface. The
 * write covers at least the 4 magic bytes, rounded up to the device's
 * write-block-size; the extra bytes land inside the bucket header and
 * programming them to 0x00 is equally legal. Buckets that already look
 * unformatted are skipped, so fresh sectors cost one 4-byte read each.
 *
 * Not in place when the backend replaces atomically: there a bucket last
 * written by blob_db_store_replace() carries a checksum over its header
 * (ubi_leb_change), and the block it replaced is still on flash awaiting
 * reclaim. Zeroing the magic under that checksum makes the next attach
 * reject the newest copy and stand the older one back up — bringing back
 * the blobs this call deleted. Replacing the bucket with the zeroed header
 * instead leaves a newest copy that verifies, at no erase. */
static int invalidate_buckets(int skip_bid)
{
	uint8_t __aligned(4) zeros[BLOB_DB_MAX_WRITE_ALIGN];
	const size_t zlen = MAX(sizeof(uint32_t),
				MIN(st.write_align, sizeof(zeros)));

	memset(zeros, 0, sizeof(zeros));

	for (uint16_t bid = 0; bid < st.n_buckets; bid++) {
		if ((int)bid == skip_bid) {
			continue;
		}
		const off_t off = peb_offset(BLOB_DB_FIRST_BUCKET + bid);
		uint8_t peek[4];
		int rc = blob_db_store_read(off, peek, sizeof(peek));

		if (rc < 0) {
			LOG_ERR("invalidate: peek bid %u: %d", bid, rc);
			return rc;
		}
		if (memcmp(peek, BUCKET_MAGIC, 4) != 0) {
			continue;
		}
		rc = blob_db_store_replace_is_atomic()
			     ? blob_db_store_replace(off, zeros, zlen)
			     : blob_db_store_write(off, zeros, zlen);
		if (rc < 0) {
			LOG_ERR("invalidate: bid %u: %d", bid, rc);
			return rc;
		}
	}
	return 0;
}

static int erase_all_impl(void)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (st.wedged) {
		return -EIO;   /* torn compaction; see compact_commit() */
	}

	/* Not redundant, despite the bind_root_empty() append at the end: the
	 * bucket magics below are zeroed *in place*, and every error path
	 * between here and that append returns without one — leaving the cache
	 * naming an object whose bucket has just been invalidated. The slot
	 * index goes for the same reason. */
	IDX_INVALIDATE();
	blob_db_sidx_reset(st.n_buckets);

	/* Every bucket but the root, which is reformatted just below. */
	int rc = invalidate_buckets((int)(BLOB_DB_ROOT_ID % st.n_buckets));

	if (rc < 0) {
		return rc;
	}

	/* Reformat the root bucket (single sector erase) and bind an empty
	 * root slot so BLOB_DB_ROOT_ID is live before this call returns. */
	rc = bind_root_empty();
	if (rc < 0) {
		return rc;
	}

	/* Reset the id space with a record that lowers the ceiling: the one
	 * record that does (§3.3). Safe because every bucket is invalidated and
	 * the root bound again first, so no id at or above 2 is left on flash
	 * but the root's. erase_all as a whole is NOT crash-atomic: a crash
	 * after some magics were zeroed leaves those buckets gone while the
	 * rest survive under the old record. That partial state is still
	 * consistent — the old (higher) ceiling stays authoritative so no id is
	 * ever reused, and a torn root rebind is repaired by mount's
	 * root-invariant check. The caller asked for destruction; how much of
	 * it lands before a crash is not part of the contract. */
	const uint64_t prev_next = st.next_id;

	st.next_id = 2;
	st.ceiling_reset = true;
	rc = master_commit();
	if (rc < 0) {
		st.next_id = prev_next;
		return rc;
	}

	LOG_INF("erase_all: complete; next_id=%llu (ceiling %llu)",
		(unsigned long long)st.next_id,
		(unsigned long long)st.next_id_hint);
	return 0;
}

/* Maintenance ------------------------------------------------------------ */

/* Start looking for fresh buckets again, at the one the next allocation
 * lands in, and have the background helper look too. Called whenever the
 * set of fresh buckets may have grown: mount, format, erase_all. */
static void maint_reset(void)
{
	st.maint_start = (uint16_t)(st.next_id % st.n_buckets);
	st.maint_scanned = 0;
	blob_db_maint_kick();
}

/* Find the next bucket that has never been written and, if format, format
 * it, so its first update does not pay the erase. Buckets ahead of the
 * allocation cursor come first: those are the ones the next ids land in. The
 * scan only reads headers and does not count as a step; *formatted says
 * whether a step was taken. Without format the scan stops at the fresh bucket
 * it finds, so maint_scanned < n_buckets says exactly whether one is left. */
static int preformat_step(bool format, bool *formatted)
{
	const uint16_t root_bid = (uint16_t)(BLOB_DB_ROOT_ID % st.n_buckets);

	*formatted = false;
	while (st.maint_scanned < st.n_buckets) {
		const uint16_t bid = (uint16_t)((st.maint_start + st.maint_scanned) %
						st.n_buckets);
		struct blob_db_bucket_hdr peek;

		if (bid == root_bid) {
			st.maint_scanned++;
			continue;
		}

		int rc = blob_db_store_read(bucket_offset(bid), &peek, sizeof(peek));

		if (rc < 0) {
			LOG_ERR("maintain: read bid %u: %d", bid, rc);
			return rc;
		}
		if (bucket_hdr_valid((const uint8_t *)&peek, bid)) {
			st.maint_scanned++;
			continue;
		}
		if (!format) {
			return 0;
		}

		rc = format_bucket(bid);
		if (rc < 0) {
			return rc;   /* not counted as scanned: the next call retries */
		}
		st.maint_scanned++;
		*formatted = true;
		return 0;
	}
	return 0;
}

int blob_db_maintain(uint32_t budget, struct blob_db_maint_result *result)
{
	struct blob_db_maint_result res = { 0 };
	int rc = 0;

	blob_db_lock();
	if (!st.mounted) {
		rc = -ENODEV;
		goto out;
	}
	if (st.wedged) {
		rc = -EIO;   /* torn compaction; see compact_commit() */
		goto out;
	}

	if (st.preformat) {
		/* Once the budget is spent, one more pass without formatting
		 * looks for the next fresh bucket, so `more` is exact. */
		for (;;) {
			bool formatted;

			rc = preformat_step(res.performed < budget, &formatted);
			if (rc < 0) {
				goto out;
			}
			if (!formatted) {
				break;
			}
			res.performed++;
		}
		res.more = st.maint_scanned < st.n_buckets;
	}

	uint32_t done = 0;
	bool more = false;

	/* Below blob_db's view: every block reads the same afterwards, so no
	 * cached bucket or index state goes stale. */
	rc = blob_db_store_maintain(budget - res.performed, &done, &more);
	res.performed += done;
	res.more = res.more || more;

out:
	blob_db_unlock();
	if (result != NULL) {
		*result = res;
	}
	return rc;
}

/* Public API: each call holds g_lock for its whole duration ---------------- */

int blob_db_mount(void)
{
	blob_db_lock();
	const int rc = mount_impl();

	if (rc == 0) {
		maint_reset();
	}
	blob_db_unlock();
	return rc;
}

int blob_db_unmount(void)
{
	blob_db_lock();
	const int rc = unmount_impl();

	blob_db_unlock();
	return rc;
}

int blob_db_format(void)
{
	blob_db_lock();
	const int rc = format_impl();

	if (rc == 0) {
		maint_reset();
	}
	blob_db_unlock();
	return rc;
}

int blob_db_erase_all(void)
{
	blob_db_lock();
	const int rc = erase_all_impl();

	if (rc == 0) {
		maint_reset();
	}
	blob_db_unlock();
	return rc;
}

uint64_t blob_db_alloc_id(void)
{
	blob_db_lock();
	const uint64_t id = alloc_id_impl();

	blob_db_unlock();
	return id;
}

/* Inside the library, -ENOTSUP from a lookup means the bucket holds a
 * foreign slot (slot_foreign_head()). A writer returns it as it is: the
 * bucket refuses writes. A reader cannot give an answer either, and to it
 * that is an I/O error, as the contract has always said for data it cannot
 * read. No read path returns -ENOTSUP for anything else. */
static inline int read_rc(int rc)
{
	return rc == -ENOTSUP ? -EIO : rc;
}

int blob_db_get(uint64_t id, void *out, size_t out_sz, size_t *out_len)
{
	blob_db_lock();
	const int rc = read_rc(get_impl(id, out, out_sz, out_len));

	blob_db_unlock();
	return rc;
}

int blob_db_update(uint64_t id, const void *payload, size_t len)
{
	blob_db_lock();
	const int rc = update_impl(id, payload, len);

	blob_db_unlock();
	return rc;
}

int blob_db_size(uint64_t id, size_t *out_size)
{
	blob_db_lock();
	const int rc = read_rc(size_impl(id, out_size));

	blob_db_unlock();
	return rc;
}

int blob_db_read(uint64_t id, size_t offset, void *out, size_t len,
		 size_t *out_read)
{
	blob_db_lock();
	const int rc = read_rc(read_impl(id, offset, out, len, out_read));

	blob_db_unlock();
	return rc;
}

int blob_db_write(uint64_t id, size_t offset, const void *buf, size_t len)
{
	blob_db_lock();
	const int rc = write_impl(id, offset, buf, len);

	blob_db_unlock();
	return rc;
}

int blob_db_delete(uint64_t id)
{
	blob_db_lock();
	const int rc = delete_impl(id);

	blob_db_unlock();
	return rc;
}

bool blob_db_exists(uint64_t id)
{
	blob_db_lock();
	const bool live = exists_impl(id);

	blob_db_unlock();
	return live;
}

size_t blob_db_count(void)
{
	blob_db_lock();
	const size_t n = count_impl();

	blob_db_unlock();
	return n;
}

int blob_db_iterate(blob_db_iter_cb_t cb, void *user)
{
	blob_db_lock();
	const int rc = iterate_impl(cb, user);

	blob_db_unlock();
	return rc;
}
