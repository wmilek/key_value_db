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
#include <zephyr/sys/crc.h>

#include <app/lib/blob_db.h>

#include "blob_db_internal.h"
#include "blob_db_store.h"

LOG_MODULE_REGISTER(blob_db, CONFIG_BLOB_DB_LOG_LEVEL);

/* On-flash magic strings, 4 bytes each (no NUL terminator). */
static const uint8_t MASTER_MAGIC[4] = { 'B', 'D', 'M', 'S' };
static const uint8_t BUCKET_MAGIC[4] = { 'B', 'D', 'B', 'H' };
static const uint8_t SCRATCH_MAGIC[4] = { 'B', 'D', 'S', 'L' };

/* Forward declarations (compaction helpers live below blob_db_mount). */
static int recover_compaction(uint16_t bid);
static int compact_bucket(uint16_t bid);
static int persist_next_id_hint(uint64_t new_hint);
static int bind_root_empty(void);
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
 * partitions. Two full-sector buffers live in .bss (below); the library is
 * single-threaded per contract §5 so sharing them across calls is safe. */
#define BLOB_DB_SECTOR_BUF_MAX CONFIG_BLOB_DB_SECTOR_BUF_SIZE

/* Sector-sized scratch buffers. g_bbuf is used by every read-a-bucket op
 * (mount, get, update, delete, exists, count, iterate) and by compaction
 * as the "old" image; g_bbuf_new is used by compaction as the "new" image. */
static uint8_t g_bbuf[BLOB_DB_SECTOR_BUF_MAX];
static uint8_t g_bbuf_new[BLOB_DB_SECTOR_BUF_MAX];

/* Set while compaction owns g_bbuf_new, so append_slot() — which also stages
 * there — can assert the two never overlap. Debug builds only; release
 * compiles the assert away and this stays a dead store. */
static bool g_bbuf_new_busy;

/* Single global instance. v1 is single-threaded; caller serializes. */
static struct {
	bool      mounted;
	size_t    fa_size;         /* total addressable bytes (n_pebs * peb_size) */
	size_t    peb_size;
	size_t    write_align;     /* backing flash write-block-size */
	uint16_t  n_pebs;
	uint16_t  n_buckets;
	uint8_t   active_master;   /* 0 or 1 */
	uint32_t  master_gen;
	uint64_t  next_id;         /* RAM: next id alloc_id will hand out */
	uint64_t  next_id_hint;    /* durable leading ceiling (see internal.h) */
	uint64_t  seg_owner;       /* segmented write in flight; 0 = none */
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
		*out = g_io;
	}
}

void blob_db_iostats_reset(void)
{
	memset(&g_io, 0, sizeof(g_io));
}
#endif /* CONFIG_BLOB_DB_IOSTATS */

/* Master sector helpers -------------------------------------------------- */

/* CRC16-CCITT over the frozen part of the compatibility prefix (everything
 * before prefix_crc16 itself). */
static uint16_t prefix_crc(const void *buf)
{
	return crc16_ccitt(0xffff, (const uint8_t *)buf,
			   offsetof(struct blob_db_compat_hdr, prefix_crc16));
}

/* How a master sector presents itself. The distinction between FOREIGN and
 * CORRUPT is the whole point of the separate prefix CRC (see
 * blob_db_internal.h): a newer writer produces a prefix that verifies, so we
 * can trust its version and refuse on purpose; bit rot produces one that does
 * not, and the other master may still be good. */
enum master_class {
	MASTER_ERASED,   /* header region is all 0xff — never written */
	MASTER_CORRUPT,  /* written, but unparseable — treat as absent */
	MASTER_FOREIGN,  /* parses, and says it is a format we do not know */
	MASTER_OK,
};

static const char *const master_class_str[] = {
	"erased", "corrupt", "foreign", "ok"
};

static int read_master(uint8_t slot, struct blob_db_master_hdr *out,
		       enum master_class *cls)
{
	uint8_t buf[BLOB_DB_MASTER_HDR_MAX];

	*cls = MASTER_CORRUPT;

	int rc = blob_db_store_read(peb_offset(slot), buf, sizeof(buf));
	if (rc < 0) {
		LOG_ERR("master %u read failed: %d", slot, rc);
		return rc;
	}

	/* Erased means virgin, and only virgin may be formatted. Check the
	 * whole staging window, not just the prefix, so a sector holding
	 * anything at all is never mistaken for untouched flash. */
	bool erased = true;

	for (size_t i = 0; i < sizeof(buf); i++) {
		if (buf[i] != 0xff) {
			erased = false;
			break;
		}
	}
	if (erased) {
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
	/* Read anything up to our own major; refuse above it. An older store
	 * cannot contain records this build does not understand, so adopting it
	 * is safe — the next master write upgrades it. */
	if (c->format_major > BLOB_DB_FORMAT_MAJOR || c->format_major == 0) {
		LOG_ERR("master %u: format major %u, this build reads up to %u",
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
	if (c->format_minor > BLOB_DB_FORMAT_MINOR) {
		/* Additive revision: fields we do not know sit in reserved
		 * space at offsets we do not read. Mount it. */
		LOG_INF("master %u: format 1.%u (this build is 1.%u) — "
			"reading the fields we know",
			slot, c->format_minor, BLOB_DB_FORMAT_MINOR);
	}

	/* hdr_crc32 is at a fixed offset for this major, so the span is ours
	 * to compute regardless of what a newer minor appended. */
	uint32_t want = hdr_crc32(buf, sizeof(struct blob_db_master_hdr));

	if (((const struct blob_db_master_hdr *)buf)->hdr_crc32 != want) {
		LOG_WRN("master %u: body CRC mismatch — treating as corrupt",
			slot);
		return 0;   /* MASTER_CORRUPT */
	}

	memcpy(out, buf, sizeof(*out));
	*cls = MASTER_OK;
	return 0;
}

static int write_master(uint8_t slot, uint32_t gen, uint8_t state,
			uint16_t compacting_bid, uint64_t next_id_hint)
{
	const uint64_t seg_owner = st.seg_owner;

	int rc = blob_db_store_erase(peb_offset(slot), st.peb_size);
	if (rc < 0) {
		LOG_ERR("master %u erase failed: %d", slot, rc);
		return rc;
	}

	/* Staged 0xff-filled so any write-alignment padding matches erased
	 * flash rather than introducing zero bits. */
	uint8_t buf[BLOB_DB_MASTER_HDR_MAX];

	memset(buf, 0xff, sizeof(buf));

	struct blob_db_master_hdr *hdr = (struct blob_db_master_hdr *)buf;

	memset(hdr, 0, sizeof(*hdr));
	memcpy(hdr->compat.magic, MASTER_MAGIC, 4);
	hdr->compat.format_major = BLOB_DB_FORMAT_MAJOR;
	hdr->compat.format_minor = BLOB_DB_FORMAT_MINOR;
	hdr->compat.hdr_len      = sizeof(*hdr);
	hdr->compat.reserved     = 0;
	hdr->compat.prefix_crc16 = prefix_crc(hdr);

	hdr->generation     = gen;
	hdr->state          = state;
	hdr->compacting_bid = compacting_bid;
	hdr->next_id_hint   = next_id_hint;
	hdr->seg_owner      = seg_owner;
	hdr->hdr_crc32      = hdr_crc32(hdr, sizeof(*hdr));

	const size_t a = st.write_align ? st.write_align : 1;
	const size_t len = ROUND_UP(sizeof(*hdr), a);

	if (len > sizeof(buf)) {
		LOG_ERR("write_align %zu too large to stage a master", a);
		return -ENOTSUP;
	}

	rc = blob_db_store_write(peb_offset(slot), buf, len);
	if (rc < 0) {
		LOG_ERR("master %u write failed: %d", slot, rc);
		return rc;
	}
	return 0;
}

/* Persist a raised leading id ceiling by committing a fresh CLEAN master on
 * the inactive slot (double-buffered: a torn write leaves the old ceiling,
 * which is still a safe — if lower — bound; mount re-raises it from the
 * scan). On success the new master is active. */
static int persist_next_id_hint(uint64_t new_hint)
{
	const uint8_t inactive = !st.active_master;

	int rc = write_master(inactive, st.master_gen + 1, BLOB_DB_STATE_CLEAN,
			      0, new_hint);
	if (rc < 0) {
		LOG_ERR("persist next_id_hint=%llu failed: %d",
			(unsigned long long)new_hint, rc);
		return rc;
	}
	st.active_master = inactive;
	st.master_gen++;
	st.next_id_hint = new_hint;
	LOG_DBG("id ceiling raised to %llu (master gen=%u)",
		(unsigned long long)new_hint, st.master_gen);
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

		cursor += slot_sz;
	}

	return 0;
}

/* Mount / unmount ------------------------------------------------------- */

static int format_masters_fresh(void)
{
	/* Root id (=1) is consumed at format time (see @blob_db_root in
	 * blob_db.h), so the leading ceiling starts at 2. */
	int rc = write_master(BLOB_DB_MASTER_A_SECTOR, 1,
			      BLOB_DB_STATE_CLEAN, 0, 2);
	if (rc < 0) {
		return rc;
	}
	rc = blob_db_store_erase(peb_offset(BLOB_DB_MASTER_B_SECTOR),
			      st.peb_size);
	if (rc < 0) {
		return rc;
	}
	st.active_master = BLOB_DB_MASTER_A_SECTOR;
	st.master_gen = 1;
	st.next_id = 2;
	st.next_id_hint = 2;
	st.seg_owner = 0;
	return bind_root_empty();
}

/* Open the store and derive/validate the geometry-dependent state, leaving
 * the store closed if anything is rejected. Shared by mount and by the
 * format-an-unmountable-store path, which needs exactly the same checks:
 * every buffer and bucket-arithmetic assumption below rests on them, and a
 * format writes through the same primitives a mount does. */
static int store_open_and_validate(void)
{
	struct blob_db_store_geom geom;
	int rc = blob_db_store_open(&geom);

	if (rc < 0) {
		LOG_ERR("store open: %d", rc);
		return rc;
	}

	st.peb_size = geom.peb_size;
	st.write_align = geom.write_align ? geom.write_align : 1;
	st.n_pebs = geom.n_pebs;
	st.fa_size = (size_t)geom.n_pebs * geom.peb_size;

	if (st.peb_size > BLOB_DB_SECTOR_BUF_MAX) {
		LOG_ERR("sector size %zu exceeds CONFIG_BLOB_DB_SECTOR_BUF_SIZE=%u",
			st.peb_size, BLOB_DB_SECTOR_BUF_MAX);
		rc = -ENOTSUP;
		goto err_close;
	}

	/* Floor as well as ceiling: read_master() stages a fixed
	 * BLOB_DB_MASTER_HDR_MAX window, and a read must never cross a PEB
	 * boundary (blob_db_store.h). It also keeps the sustainable-payload
	 * arithmetic below from underflowing on an absurd geometry. */
	if (st.peb_size < BLOB_DB_MASTER_HDR_MAX) {
		LOG_ERR("sector size %zu below the %u B minimum blob_db supports",
			st.peb_size, BLOB_DB_MASTER_HDR_MAX);
		rc = -ENOTSUP;
		goto err_close;
	}

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

int blob_db_mount(void)
{
	if (st.mounted) {
		return -EALREADY;
	}

	IDX_INVALIDATE();

	int rc = store_open_and_validate();

	if (rc < 0) {
		return rc;
	}

	/* Pick the authoritative master. */
	struct blob_db_master_hdr m_a, m_b;
	enum master_class ca, cb;

	rc = read_master(BLOB_DB_MASTER_A_SECTOR, &m_a, &ca);
	if (rc < 0) {
		goto err_close;
	}
	rc = read_master(BLOB_DB_MASTER_B_SECTOR, &m_b, &cb);
	if (rc < 0) {
		goto err_close;
	}
	LOG_DBG("master A=%s B=%s", master_class_str[ca], master_class_str[cb]);

	/* A parseable master declaring a format we do not know is never
	 * touched — not mounted, and above all not formatted. This is the
	 * -ENOTSUP the contract has always promised (l1_blob_db.md §4, §5.1).
	 * Checked before anything else, and on BOTH sectors: if an interrupted
	 * upgrade left A on our format and B ahead of it, falling back to A
	 * would silently mount a stale view of an upgraded store. */
	if (ca == MASTER_FOREIGN || cb == MASTER_FOREIGN) {
		LOG_ERR("refusing to mount a foreign store (A=%s B=%s); "
			"use blob_db_format() to discard it deliberately",
			master_class_str[ca], master_class_str[cb]);
		rc = -ENOTSUP;
		goto err_close;
	}

	if (ca != MASTER_OK && cb != MASTER_OK) {
		if (ca == MASTER_ERASED && cb == MASTER_ERASED) {
			LOG_INF("virgin partition; formatting");
		} else if (IS_ENABLED(CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT)) {
			LOG_WRN("both masters unreadable (A=%s B=%s); "
				"reformatting per CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT",
				master_class_str[ca], master_class_str[cb]);
		} else {
			LOG_ERR("both masters unreadable (A=%s B=%s); refusing",
				master_class_str[ca], master_class_str[cb]);
			rc = -EIO;
			goto err_close;
		}
		rc = format_masters_fresh();
		if (rc < 0) {
			goto err_close;
		}
	} else {
		bool pick_a;

		if (ca == MASTER_OK && cb != MASTER_OK) {
			pick_a = true;
		} else if (ca != MASTER_OK && cb == MASTER_OK) {
			pick_a = false;
		} else {
			pick_a = m_a.generation >= m_b.generation;
		}
		const struct blob_db_master_hdr *m = pick_a ? &m_a : &m_b;

		st.active_master = pick_a ? BLOB_DB_MASTER_A_SECTOR
					  : BLOB_DB_MASTER_B_SECTOR;
		st.master_gen = m->generation;
		st.next_id_hint = m->next_id_hint;
		st.next_id = m->next_id_hint;   /* ceiling is authoritative (§13.1) */
		st.seg_owner = m->seg_owner;
		LOG_INF("master %c gen=%u state=%u next_id_hint=%llu",
			pick_a ? 'A' : 'B',
			st.master_gen, m->state, st.next_id);

		if (m->state == BLOB_DB_STATE_COMPACTING) {
			rc = recover_compaction(m->compacting_bid);
			if (rc < 0) {
				goto err_close;
			}
		}
	}

	/* Defensive: the leading ceiling should already exceed every bound id,
	 * but scan anyway so a store written by an older (lagging-hint) format,
	 * or one with a corrupt/rolled-back master, cannot re-issue a live id.
	 * If the scan out-runs the ceiling, raise it AND persist the new ceiling
	 * before any allocation can hand the id back out. */
	uint64_t max_id = (st.next_id == 0) ? 0 : (st.next_id - 1);
	for (uint16_t bid = 0; bid < st.n_buckets; bid++) {
		rc = scan_bucket(bid, &max_id);
		if (rc < 0) {
			LOG_ERR("scan_bucket(%u): %d", bid, rc);
			goto err_close;
		}
	}
	if (max_id + 1 > st.next_id_hint) {
		LOG_WRN("bucket scan out-ran ceiling (%llu >= %llu); raising",
			(unsigned long long)max_id, st.next_id_hint);
		st.next_id = max_id + 1;
		rc = persist_next_id_hint(max_id + 1 + BLOB_DB_ID_HINT_STEP);
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
				rc = persist_next_id_hint(2 + BLOB_DB_ID_HINT_STEP);
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
	blob_db_store_close();
	return rc;
}

int blob_db_unmount(void)
{
	if (!st.mounted) {
		return 0;
	}
	blob_db_store_close();
	st.mounted = false;
	IDX_INVALIDATE();
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

/* Interpret a slot at buf[off] in a bucket buffer.
 * Returns true iff structurally valid AND CRC verifies — i.e., a real
 * committed slot. Returns false otherwise (end-of-log, torn write, etc.). */
struct slot_view {
	uint64_t id;
	uint8_t  flags;
	uint16_t val_len;
	size_t   total_size;
};

static bool slot_view_at(const uint8_t *buf, off_t off, struct slot_view *out)
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
	 * what we would take it to mean, so it is not data. Mount should have
	 * refused the store already (a new slot flag is a MAJOR change); this
	 * is the backstop if that discipline ever slips. */
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
	if (slot_crc16(shdr, id, payload, shdr->val_len) != stored) {
		return false;
	}

	out->id         = id;
	out->flags      = shdr->flags;
	out->val_len    = shdr->val_len;
	out->total_size = ssz;
	return true;
}

static int read_bucket(uint16_t bid, uint8_t *buf)
{
	return blob_db_store_read(bucket_offset(bid), buf, st.peb_size);
}

static int format_bucket(uint16_t bid)
{
	int rc = blob_db_store_erase(bucket_offset(bid), st.peb_size);

	if (rc < 0) {
		LOG_ERR("bucket %u erase: %d", bid, rc);
		return rc;
	}

	struct blob_db_bucket_hdr bhdr = {
		.bucket_id = bid,
		.reserved  = 0,
		.gen       = 1,
	};
	memcpy(bhdr.magic, BUCKET_MAGIC, 4);
	bhdr.hdr_crc32 = hdr_crc32(&bhdr, sizeof(bhdr));

	rc = blob_db_store_write(bucket_offset(bid), &bhdr, sizeof(bhdr));
	if (rc < 0) {
		LOG_ERR("bucket %u header write: %d", bid, rc);
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
	struct blob_db_bucket_hdr bhdr;
	int rc = blob_db_store_read(bucket_offset(bid), &bhdr, sizeof(bhdr));

	if (rc < 0) {
		return rc;
	}
	*valid = bucket_hdr_valid((const uint8_t *)&bhdr, bid);
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
 * val_len — because there is no safe stride past it.
 */
static int scan_bucket_for(uint16_t bid, uint64_t target_id, off_t limit,
			   struct bucket_scan *out)
{
	const off_t base = bucket_offset(bid);
	off_t cursor = BLOB_DB_BUCKET_DATA_OFF;

	out->target_off = -1;

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
			break;
		}

		const size_t ssz = slot_size_for(sh.hdr.val_len);

		if (cursor + (off_t)ssz > (off_t)st.peb_size) {
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
	off_t limit = (off_t)st.peb_size;

	for (;;) {
		int rc = scan_bucket_for(bid, id, limit, sc);

		if (rc < 0) {
			return rc;
		}
		if (sc->target_off < 0) {
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
		return 0;
	}
}

/* Build and write a slot at bucket+write_cursor. Slot size on flash is
 * padded to write_block_size; padding bytes are 0xff so any truncated
 * write still reads as erased in the padding region. */
#define BLOB_DB_MAX_WRITE_ALIGN 32

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

	return blob_db_store_write(bucket_offset(bid) + write_cursor,
				buf, total);
}

/* Per-bucket compaction & crash recovery (stage 5) ---------------------- */

/* Build a compacted image of bucket `bid` in new_buf using old_buf as the
 * source. Returns the number of bytes written into new_buf (≥ 16 for the
 * bucket header). Skipped entries: tombstones, and any slot whose id has
 * a later occurrence in the same bucket. */
static size_t build_compacted_image(const uint8_t *old_buf, uint8_t *new_buf,
				    uint16_t bid)
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

		if (!slot_view_at(old_buf, cursor, &sv)) {
			break;
		}

		/* Is there a later slot with the same id? */
		bool superseded = false;
		off_t scan = cursor + sv.total_size;

		for (;;) {
			struct slot_view future;

			if (!slot_view_at(old_buf, scan, &future)) {
				break;
			}
			if (future.id == sv.id) {
				superseded = true;
				break;
			}
			scan += future.total_size;
		}

		if (!superseded && !(sv.flags & BLOB_DB_SLOT_F_TOMBSTONE)) {
			memcpy(new_buf + new_cursor,
			       old_buf + cursor, sv.total_size);
			new_cursor += sv.total_size;
		}

		cursor += sv.total_size;
	}

	return (size_t)new_cursor;
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
	uint8_t buf[BLOB_DB_MAX_WRITE_ALIGN + sizeof(struct blob_db_scratch_seal)];
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

/* Phase-2: persist the compacted image via master + scratch + bucket sequence.
 *
 *   1. write master inactive = COMPACTING(bid)                 ── enters atomic window
 *   2. erase scratch; write scratch = new image
 *   3. erase bucket; write bucket = new image
 *   4. erase scratch
 *   5. write master inactive = CLEAN                           ── leaves atomic window
 *
 * If we crash anywhere in 1..5, mount-time recover_compaction() restores a
 * consistent state per the crash table in design §8.
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

	/* Step 1: enter atomic window. */
	uint8_t inactive = !st.active_master;
	int rc = write_master(inactive, st.master_gen + 1,
			      BLOB_DB_STATE_COMPACTING, bid, st.next_id);
	if (rc < 0) {
		return rc;
	}
	st.active_master = inactive;
	st.master_gen++;

	/* Step 2: scratch, then seal it. The seal is what makes step 3 safe to
	 * start: until it lands, recovery must leave the bucket alone. */
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

	/* Step 3: bucket. */
	rc = blob_db_store_erase(bucket_off, st.peb_size);
	if (rc < 0) {
		return rc;
	}
	rc = blob_db_store_write(bucket_off, new_buf, new_len);
	if (rc < 0) {
		return rc;
	}

	/* Step 4: erase scratch. */
	rc = blob_db_store_erase(scratch_off, st.peb_size);
	if (rc < 0) {
		return rc;
	}

	/* Step 5: leave atomic window. */
	inactive = !st.active_master;
	rc = write_master(inactive, st.master_gen + 1,
			  BLOB_DB_STATE_CLEAN, 0, st.next_id);
	if (rc < 0) {
		return rc;
	}
	st.active_master = inactive;
	st.master_gen++;

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

	const size_t new_len = build_compacted_image(g_bbuf, g_bbuf_new, bid);

	LOG_INF("compact bid=%u: new bucket has %zu B (was up to %zu B)",
		bid, new_len, st.peb_size);

	rc = compact_commit(bid, g_bbuf_new, new_len);
	g_bbuf_new_busy = false;
	return rc;
}

/* Mount-time recovery for a partition we found in COMPACTING(bid) state.
 *
 * Decision rule: if the scratch sector contains a valid bucket header
 * with the matching bid, treat scratch as authoritative — copy it back
 * over the bucket. If scratch is invalid, the bucket is still the
 * original (we crashed before scratch was sealed). Either way, finish
 * by erasing scratch and writing a CLEAN master.
 */
static int recover_compaction(uint16_t bid)
{
	if (bid >= st.n_buckets) {
		LOG_ERR("recover: compacting_bid %u out of range (n=%u)",
			bid, st.n_buckets);
		return -EIO;
	}

	const off_t scratch_off = peb_offset(BLOB_DB_SCRATCH_SECTOR);

	int rc = blob_db_store_read(scratch_off, g_bbuf, st.peb_size);
	if (rc < 0) {
		return rc;
	}

	/* The tail seal — not the bucket header — decides whether the whole
	 * image landed. The header is written first, so it survives a tear that
	 * lost everything after it; restoring on the strength of the header
	 * therefore overwrote intact buckets with truncated images. */
	size_t image_len = 0;

	if (scratch_seal_valid(g_bbuf, &image_len) &&
	    bucket_hdr_valid(g_bbuf, bid)) {
		LOG_WRN("recover: scratch sealed for bid %u (%zu B); "
			"restoring bucket", bid, image_len);
		rc = blob_db_store_erase(bucket_offset(bid), st.peb_size);
		if (rc < 0) {
			return rc;
		}
		/* Only the image: the rest of the bucket must stay erased so
		 * appends can continue after it. */
		rc = blob_db_store_write(bucket_offset(bid), g_bbuf, image_len);
		if (rc < 0) {
			return rc;
		}
	} else {
		/* Either the image never completed (crash before the seal, so
		 * the bucket was never touched) or it is not for this bucket.
		 * Leaving the bucket alone is the safe half of the window. */
		LOG_WRN("recover: scratch unsealed or foreign; bucket %u left as-is",
			bid);
	}

	rc = blob_db_store_erase(scratch_off, st.peb_size);
	if (rc < 0) {
		return rc;
	}

	const uint8_t inactive = !st.active_master;

	rc = write_master(inactive, st.master_gen + 1,
			  BLOB_DB_STATE_CLEAN, 0, st.next_id);
	if (rc < 0) {
		return rc;
	}
	st.active_master = inactive;
	st.master_gen++;

	LOG_INF("recover: complete; master gen=%u", st.master_gen);
	return 0;
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
static uint8_t g_seg_a[8u * CONFIG_BLOB_DB_MAX_SEGMENTS];
static uint8_t g_seg_b[8u * CONFIG_BLOB_DB_MAX_SEGMENTS];

/* Derived at mount from the geometry; validated there too. */
static uint16_t g_seg_len;      /* chunk bytes per segment */
static uint16_t g_seg_max;      /* segments an index record can address */

static inline size_t inline_max(void)
{
	return CONFIG_BLOB_DB_MAX_PAYLOAD_LEN;
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
 * master write; a segmented update already costs K slot writes. */
static int persist_seg_owner(uint64_t owner)
{
	if (st.seg_owner == owner) {
		return 0;
	}

	const uint8_t inactive = !st.active_master;
	const uint64_t prev = st.seg_owner;

	st.seg_owner = owner;

	int rc = write_master(inactive, st.master_gen + 1, BLOB_DB_STATE_CLEAN,
			      0, st.next_id_hint);
	if (rc < 0) {
		st.seg_owner = prev;
		return rc;
	}
	st.active_master = inactive;
	st.master_gen++;
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
	if (memcmp(h->magic, BLOB_DB_INDEX_MAGIC, 2) != 0 ||
	    h->version != BLOB_DB_INDEX_VERSION) {
		return -EIO;
	}
	if (h->seg_count == 0 || h->seg_count > g_seg_max || h->seg_len == 0 ||
	    sc.target.hdr.val_len <
		    sizeof(*h) + (size_t)h->seg_count * sizeof(uint64_t)) {
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
			struct bucket_scan sc;

			rc = scan_bucket_for(bid, sid, (off_t)st.peb_size, &sc);
			if (rc < 0) {
				return rc;
			}
			cursor = sc.write_cursor;
		}

		if (cursor + (off_t)ssz > (off_t)st.peb_size) {
			rc = compact_bucket(bid);
			if (rc == 0) {
				struct bucket_scan sc;

				rc = scan_bucket_for(bid, sid,
						     (off_t)st.peb_size, &sc);
				if (rc < 0) {
					return rc;
				}
				cursor = sc.write_cursor;
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

/* Tombstone a generation of segments. Best effort: what fails here is left for
 * the sweep, which is why the caller keeps seg_owner set until it returns. */
static void seg_release(const uint8_t *ids, uint16_t k)
{
	for (uint16_t j = 0; j < k; j++) {
		const uint64_t sid = seg_id_at(ids, j);
		const uint16_t bid = id_to_bucket(sid);
		bool formatted;

		if (read_bucket_hdr(bid, &formatted) < 0 || !formatted) {
			continue;
		}

		struct bucket_scan sc;

		if (scan_bucket_for(bid, sid, (off_t)st.peb_size, &sc) < 0) {
			continue;
		}
		if (sc.write_cursor + (off_t)slot_size_for(0) >
		    (off_t)st.peb_size) {
			continue;   /* sweep will get it */
		}
		(void)append_slot2(bid, sc.write_cursor, sid,
				   BLOB_DB_SLOT_F_TOMBSTONE |
					   BLOB_DB_SLOT_F_SEGMENT,
				   NULL, 0, NULL, 0);
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

			uint8_t one[sizeof(uint64_t)];

			memcpy(one, &sh.id, sizeof(one));
			seg_release(one, 1);
			dropped++;
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
static uint8_t g_chunk[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];

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
		struct bucket_scan sc;

		rc = scan_bucket_for(bid, id, (off_t)st.peb_size, &sc);
		if (rc < 0) {
			return rc;
		}
		cursor = sc.write_cursor;
	}

	if (cursor + (off_t)slot_size_for(idx_len) > (off_t)st.peb_size) {
		rc = compact_bucket(bid);
		if (rc < 0) {
			return rc;
		}

		struct bucket_scan sc;

		rc = scan_bucket_for(bid, id, (off_t)st.peb_size, &sc);
		if (rc < 0) {
			return rc;
		}
		cursor = sc.write_cursor;
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

uint64_t blob_db_alloc_id(void)
{
	if (!st.mounted) {
		return 0;   /* 0 is never a valid id */
	}

	/* Guarantee durability before returning: the id we hand out must be
	 * strictly below a persisted ceiling, so a crash before it is ever
	 * bound cannot let a later mount re-issue it (contract §2, §13.1). */
	if (st.next_id >= st.next_id_hint) {
		int rc = persist_next_id_hint(st.next_id + 1 +
					      BLOB_DB_ID_HINT_STEP);
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

int blob_db_get(uint64_t id, void *out, size_t out_sz, size_t *out_len)
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

int blob_db_update(uint64_t id, const void *payload, size_t len)
{
	if (!st.mounted) {
		return -ENODEV;
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
		/* Only the append cursor is needed: latest-wins (§4) makes the
		 * new slot live without consulting the old one. */
		struct bucket_scan sc;

		rc = scan_bucket_for(bid, id, (off_t)st.peb_size, &sc);
		if (rc < 0) {
			return rc;
		}
		write_cursor = sc.write_cursor;
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
		if (sc.target_off >= 0 &&
		    (sc.target.hdr.flags & BLOB_DB_SLOT_F_INDEXED)) {
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

		rc = scan_bucket_for(bid, id, (off_t)st.peb_size, &sc);
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

int blob_db_delete(uint64_t id)
{
	if (!st.mounted) {
		return -ENODEV;
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

	const size_t ssz = slot_size_for(0);
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

bool blob_db_exists(uint64_t id)
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

	return find_live_slot(bid, id, &sc, g_bbuf) == 0;
}

/* Stage 6 — format / count / iterate ------------------------------------ */

/* Walk a bucket buffer, invoking `cb` for each *live* slot — the latest
 * non-tombstoned slot for its id within this bucket. O(n²) over slots in
 * the bucket; the per-bucket count is small in normal use. Returns 0 if
 * walk completed, or the non-zero value the callback returned to abort. */
typedef int (*live_slot_visit_t)(uint64_t id, const uint8_t *payload,
				  uint16_t val_len, void *user);

static int for_each_live_slot(const uint8_t *buf,
			      live_slot_visit_t cb, void *user)
{
	off_t cursor = BLOB_DB_BUCKET_DATA_OFF;

	for (;;) {
		struct slot_view sv;

		if (!slot_view_at(buf, cursor, &sv)) {
			break;
		}

		bool superseded = false;
		off_t scan = cursor + sv.total_size;

		for (;;) {
			struct slot_view future;

			if (!slot_view_at(buf, scan, &future)) {
				break;
			}
			if (future.id == sv.id) {
				superseded = true;
				break;
			}
			scan += future.total_size;
		}

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

size_t blob_db_count(void)
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

int blob_db_iterate(blob_db_iter_cb_t cb, void *user)
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

		int rc = for_each_live_slot(g_bbuf, iterate_visitor, &t);

		if (rc) {
			return rc;  /* user-requested early stop */
		}
	}
	return 0;
}

int blob_db_size(uint64_t id, size_t *out_size)
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
	if (sc.target.hdr.flags & BLOB_DB_SLOT_F_INDEXED) {
		struct blob_db_index_hdr h;

		memcpy(&h, g_bbuf, sizeof(h));
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

int blob_db_read(uint64_t id, size_t offset, void *out, size_t len,
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

int blob_db_write(uint64_t id, size_t offset, const void *buf, size_t len)
{
	if (!st.mounted) {
		return -ENODEV;
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
	if (len == 0) {
		return 0;
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

	if (sc.write_cursor + (off_t)ssz > (off_t)st.peb_size) {
		rc = compact_bucket(bid);
		if (rc < 0) {
			return rc;
		}
		rc = find_live_slot(bid, id, &sc, g_bbuf);
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

int blob_db_format(void)
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
		rc = store_open_and_validate();
		if (rc < 0) {
			return rc;
		}
		opened_here = true;
	}

	IDX_INVALIDATE();

	rc = blob_db_store_erase(0, st.fa_size);
	if (rc < 0) {
		LOG_ERR("format: erase: %d", rc);
		goto err;
	}

	/* Root id (=1) is consumed at format time — see bind_root_empty(). */
	rc = write_master(BLOB_DB_MASTER_A_SECTOR, 1, BLOB_DB_STATE_CLEAN, 0, 2);
	if (rc < 0) {
		goto err;
	}

	st.active_master = BLOB_DB_MASTER_A_SECTOR;
	st.master_gen    = 1;
	st.next_id       = 2;
	st.next_id_hint  = 2;
	st.seg_owner     = 0;

	rc = bind_root_empty();
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

int blob_db_erase_all(void)
{
	if (!st.mounted) {
		return -ENODEV;
	}

	/* Not redundant, despite the bind_root_empty() append at the end: the
	 * bucket magics below are zeroed *in place*, and every error path
	 * between here and that append returns without one — leaving the cache
	 * naming an object whose bucket has just been invalidated. */
	IDX_INVALIDATE();

	const uint16_t root_bid = (uint16_t)(BLOB_DB_ROOT_ID % st.n_buckets);
	uint8_t zeros[BLOB_DB_MAX_WRITE_ALIGN];
	const size_t zlen = MAX(sizeof(uint32_t),
				MIN(st.write_align, sizeof(zeros)));

	memset(zeros, 0, sizeof(zeros));

	/* Invalidate every non-root bucket's magic in place (BUCKET_MAGIC is
	 * 'B'/'D'/'B'/'H' = 0x42/0x44/0x42/0x48; overwriting with 0x00 only
	 * flips 1→0 bits — no erase needed, and every scan afterwards treats
	 * the sector as unformatted). The write covers at least the 4 magic
	 * bytes, rounded up to the device's write-block-size; the extra bytes
	 * land inside the bucket header and programming them to 0x00 is
	 * equally legal. Skip buckets that already look unformatted so we do
	 * not touch fresh sectors. */
	for (uint16_t bid = 0; bid < st.n_buckets; bid++) {
		if (bid == root_bid) {
			continue;
		}
		uint8_t peek[4];
		int rc = blob_db_store_read(
					 peb_offset(BLOB_DB_FIRST_BUCKET + bid),
					 peek, sizeof(peek));
		if (rc < 0) {
			LOG_ERR("erase_all: peek bid %u: %d", bid, rc);
			return rc;
		}
		if (memcmp(peek, BUCKET_MAGIC, 4) != 0) {
			continue;
		}
		rc = blob_db_store_write(
				      peb_offset(BLOB_DB_FIRST_BUCKET + bid),
				      zeros, zlen);
		if (rc < 0) {
			LOG_ERR("erase_all: invalidate bid %u: %d", bid, rc);
			return rc;
		}
	}

	/* Reformat the root bucket (single sector erase) and bind an empty
	 * root slot so BLOB_DB_ROOT_ID is live before this call returns. */
	int rc = bind_root_empty();
	if (rc < 0) {
		return rc;
	}

	/* Reset the id space via a new master on the inactive slot. Note that
	 * erase_all as a whole is NOT crash-atomic: a crash after some magics
	 * were zeroed leaves those buckets gone while the rest survive under
	 * the old master. That partial state is still consistent — the old
	 * (higher) next_id_hint stays authoritative so no id is ever reused,
	 * and a torn root rebind is repaired by mount's root-invariant
	 * check. The caller asked for destruction; how much of it lands
	 * before a crash is not part of the contract. */
	const uint8_t inactive = !st.active_master;

	rc = write_master(inactive, st.master_gen + 1,
			  BLOB_DB_STATE_CLEAN, 0, 2);
	if (rc < 0) {
		return rc;
	}
	st.active_master = inactive;
	st.master_gen++;
	st.next_id = 2;
	st.next_id_hint = 2;

	LOG_INF("erase_all: complete; next_id=%llu (master gen=%u)",
		(unsigned long long)st.next_id, st.master_gen);
	return 0;
}

int blob_db_prepare(size_t n)
{
	if (!st.mounted) {
		return -ENODEV;
	}
	if (n == 0) {
		return 0;
	}
	if (n > st.n_buckets) {
		n = st.n_buckets;
	}

	const uint16_t root_bid = (uint16_t)(BLOB_DB_ROOT_ID % st.n_buckets);
	size_t prepared = 0;

	for (size_t i = 0; i < n; i++) {
		const uint16_t bid =
			(uint16_t)((st.next_id + i) % st.n_buckets);

		if (bid == root_bid) {
			continue;
		}

		/* Peek only the header — no need to pull a full sector into
		 * g_bbuf just to decide whether formatting is needed. */
		struct blob_db_bucket_hdr peek;
		int rc = blob_db_store_read(bucket_offset(bid),
					 &peek, sizeof(peek));
		if (rc < 0) {
			LOG_ERR("prepare: read bid %u: %d", bid, rc);
			return rc;
		}
		if (bucket_hdr_valid((const uint8_t *)&peek, bid)) {
			continue;
		}

		rc = format_bucket(bid);
		if (rc < 0) {
			return rc;
		}
		prepared++;
	}

	LOG_DBG("prepare(%zu): formatted %zu (cursor=%llu)",
		n, prepared, (unsigned long long)st.next_id);
	return (int)prepared;
}
