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

/* Forward declarations (compaction helpers live below blob_db_mount). */
static int recover_compaction(uint16_t bid);
static int compact_bucket(uint16_t bid);
static int persist_next_id_hint(uint64_t new_hint);
static int bind_root_empty(void);
struct bucket_walk {
	off_t    write_cursor;
	off_t    target_slot_off;
	uint8_t  target_flags;
	uint16_t target_val_len;
};
static bool bucket_hdr_valid(const uint8_t *buf, uint16_t bid);
static void walk_bucket(const uint8_t *buf, uint64_t target_id,
			struct bucket_walk *r);

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
	if (c->format_major != BLOB_DB_FORMAT_MAJOR) {
		LOG_ERR("master %u: format major %u, this build knows %u",
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
	return bind_root_empty();
}

int blob_db_mount(void)
{
	if (st.mounted) {
		return -EALREADY;
	}

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
		rc = blob_db_store_read(peb_offset(BLOB_DB_FIRST_BUCKET + root_bid),
				     g_bbuf, st.peb_size);
		if (rc < 0) {
			goto err_close;
		}
		bool root_live = false;
		if (bucket_hdr_valid(g_bbuf, root_bid)) {
			struct bucket_walk w;

			walk_bucket(g_bbuf, BLOB_DB_ROOT_ID, &w);
			root_live = (w.target_slot_off >= 0) &&
				    !(w.target_flags & BLOB_DB_SLOT_F_TOMBSTONE);
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

static void walk_bucket(const uint8_t *buf, uint64_t target_id,
			struct bucket_walk *r)
{
	r->target_slot_off = -1;
	r->target_flags    = 0;
	r->target_val_len  = 0;

	off_t cursor = BLOB_DB_BUCKET_DATA_OFF;

	for (;;) {
		struct slot_view sv;

		if (!slot_view_at(buf, cursor, &sv)) {
			break;
		}
		if (sv.id == target_id) {
			r->target_slot_off = cursor;
			r->target_flags    = sv.flags;
			r->target_val_len  = sv.val_len;
		}
		cursor += sv.total_size;
	}

	r->write_cursor = cursor;
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

/* Build and write a slot at bucket+write_cursor. Slot size on flash is
 * padded to write_block_size; padding bytes are 0xff so any truncated
 * write still reads as erased in the padding region. */
#define BLOB_DB_MAX_WRITE_ALIGN 32

static int append_slot(uint16_t bid, off_t write_cursor, uint64_t id,
		       uint8_t extra_flags, const void *payload,
		       uint16_t val_len)
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

	uint8_t *buf = g_bbuf_new;

	struct blob_db_slot_hdr *shdr = (struct blob_db_slot_hdr *)buf;

	shdr->flags    = BLOB_DB_SLOT_F_SEALED | extra_flags;
	shdr->reserved = 0;
	shdr->val_len  = val_len;

	memcpy(buf + sizeof(*shdr), &id, sizeof(id));
	if (val_len > 0) {
		memcpy(buf + sizeof(*shdr) + sizeof(id), payload, val_len);
	}

	uint16_t crc = slot_crc16(shdr, id, payload, val_len);
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

	/* Step 1: enter atomic window. */
	uint8_t inactive = !st.active_master;
	int rc = write_master(inactive, st.master_gen + 1,
			      BLOB_DB_STATE_COMPACTING, bid, st.next_id);
	if (rc < 0) {
		return rc;
	}
	st.active_master = inactive;
	st.master_gen++;

	/* Step 2: scratch. */
	rc = blob_db_store_erase(scratch_off, st.peb_size);
	if (rc < 0) {
		return rc;
	}
	rc = blob_db_store_write(scratch_off, new_buf, new_len);
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

	if (bucket_hdr_valid(g_bbuf, bid)) {
		LOG_WRN("recover: scratch is sealed for bid %u; "
			"restoring bucket from scratch", bid);
		rc = blob_db_store_erase(bucket_offset(bid), st.peb_size);
		if (rc < 0) {
			return rc;
		}
		rc = blob_db_store_write(bucket_offset(bid),
				      g_bbuf, st.peb_size);
		if (rc < 0) {
			return rc;
		}
	} else {
		LOG_WRN("recover: scratch invalid; bucket %u left as-is", bid);
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

	int rc = read_bucket(bid, g_bbuf);
	if (rc < 0) {
		return rc;
	}

	if (!bucket_hdr_valid(g_bbuf, bid)) {
		return -ENOENT;
	}

	struct bucket_walk w;
	walk_bucket(g_bbuf, id, &w);

	if (w.target_slot_off < 0) {
		return -ENOENT;
	}
	if (w.target_flags & BLOB_DB_SLOT_F_TOMBSTONE) {
		return -ENOENT;
	}
	if (out_sz < w.target_val_len) {
		return -ENOMEM;
	}

	if (w.target_val_len > 0) {
		const off_t payload_off = w.target_slot_off +
					  sizeof(struct blob_db_slot_hdr) +
					  sizeof(uint64_t);
		memcpy(out, g_bbuf + payload_off, w.target_val_len);
	}
	if (out_len) {
		*out_len = w.target_val_len;
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
	if (len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
		return -EINVAL;
	}
	if (len > 0 && !payload) {
		return -EINVAL;
	}

	/* Bind (first write) and rebind (rewrite) are the same append path:
	 * write the new slot; latest-wins (§4) makes it the live one. We do
	 * NOT verify prior state — a first bind has no prior slot, and writing
	 * to a dead id is UB (decision D3), not our job to catch here. */
	const uint16_t bid = id_to_bucket(id);

	int rc = read_bucket(bid, g_bbuf);
	if (rc < 0) {
		return rc;
	}

	off_t write_cursor;

	if (!bucket_hdr_valid(g_bbuf, bid)) {
		rc = format_bucket(bid);
		if (rc < 0) {
			return rc;
		}
		write_cursor = BLOB_DB_BUCKET_DATA_OFF;
	} else {
		struct bucket_walk w;

		walk_bucket(g_bbuf, id, &w);
		write_cursor = w.write_cursor;
	}

	const size_t ssz = slot_size_for((uint16_t)len);

	if (write_cursor + ssz > st.peb_size) {
		rc = compact_bucket(bid);
		if (rc < 0) {
			return rc;
		}
		rc = read_bucket(bid, g_bbuf);
		if (rc < 0) {
			return rc;
		}
		struct bucket_walk w;

		walk_bucket(g_bbuf, id, &w);
		write_cursor = w.write_cursor;
		if (write_cursor + ssz > st.peb_size) {
			return -ENOSPC;
		}
	}

	rc = append_slot(bid, write_cursor, id, 0, payload, (uint16_t)len);
	if (rc < 0) {
		return rc;
	}

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

	const uint16_t bid = id_to_bucket(id);

	int rc = read_bucket(bid, g_bbuf);
	if (rc < 0) {
		return rc;
	}
	if (!bucket_hdr_valid(g_bbuf, bid)) {
		return -ENOENT;
	}

	struct bucket_walk w;
	walk_bucket(g_bbuf, id, &w);

	if (w.target_slot_off < 0) {
		return -ENOENT;
	}
	if (w.target_flags & BLOB_DB_SLOT_F_TOMBSTONE) {
		return -ENOENT;
	}

	const size_t ssz = slot_size_for(0);
	off_t write_cursor = w.write_cursor;

	if (write_cursor + ssz > st.peb_size) {
		rc = compact_bucket(bid);
		if (rc < 0) {
			return rc;
		}
		rc = read_bucket(bid, g_bbuf);
		if (rc < 0) {
			return rc;
		}
		walk_bucket(g_bbuf, id, &w);
		if (w.target_slot_off < 0 ||
		    (w.target_flags & BLOB_DB_SLOT_F_TOMBSTONE)) {
			/* Compaction dropped this id — already deleted. */
			return -ENOENT;
		}
		write_cursor = w.write_cursor;
		if (write_cursor + ssz > st.peb_size) {
			return -ENOSPC;
		}
	}

	rc = append_slot(bid, write_cursor, id,
			 BLOB_DB_SLOT_F_TOMBSTONE, NULL, 0);
	if (rc < 0) {
		return rc;
	}

	LOG_DBG("delete id=%llu bid=%u off=0x%lx",
		(unsigned long long)id, bid,
		(unsigned long)w.write_cursor);
	return 0;
}

bool blob_db_exists(uint64_t id)
{
	if (!st.mounted || id == 0) {
		return false;
	}

	const uint16_t bid = id_to_bucket(id);

	if (read_bucket(bid, g_bbuf) < 0) {
		return false;
	}
	if (!bucket_hdr_valid(g_bbuf, bid)) {
		return false;
	}

	struct bucket_walk w;
	walk_bucket(g_bbuf, id, &w);

	return w.target_slot_off >= 0 &&
	       !(w.target_flags & BLOB_DB_SLOT_F_TOMBSTONE);
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

		if (!superseded && !(sv.flags & BLOB_DB_SLOT_F_TOMBSTONE)) {
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

int blob_db_format(void)
{
	if (!st.mounted) {
		return -ENODEV;
	}

	int rc = blob_db_store_erase(0, st.fa_size);
	if (rc < 0) {
		LOG_ERR("format: erase: %d", rc);
		return rc;
	}

	/* Root id (=1) is consumed at format time — see bind_root_empty(). */
	rc = write_master(BLOB_DB_MASTER_A_SECTOR, 1, BLOB_DB_STATE_CLEAN, 0, 2);
	if (rc < 0) {
		return rc;
	}

	st.active_master = BLOB_DB_MASTER_A_SECTOR;
	st.master_gen    = 1;
	st.next_id       = 2;
	st.next_id_hint  = 2;

	rc = bind_root_empty();
	if (rc < 0) {
		return rc;
	}

	LOG_INF("format: complete; next_id=%llu",
		(unsigned long long)st.next_id);
	return 0;
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
