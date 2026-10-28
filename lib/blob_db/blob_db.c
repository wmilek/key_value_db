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
#include <app/lib/blob_db_security.h>
#include <app/lib/blob_db_test.h>

#include "blob_db_internal.h"
#include "blob_db_protect.h"
#include "blob_db_slot_index.h"
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
static int invalidate_buckets(int skip_bid);
static int format_whole_store(void);
static bool bucket_hdr_valid(const uint8_t *buf, uint16_t bid);
static int bucket_hdr_authentic(const uint8_t *buf, uint16_t bid);

/* The on-flash prefix of a slot: header then id, contiguous. These 12 bytes
 * are enough to size a slot and know whose it is, which is what lets the walk
 * step over a slot without reading its payload. */
struct __packed slot_head {
	struct blob_db_slot_hdr hdr;
	uint64_t id;
};
BUILD_ASSERT(sizeof(struct slot_head) == 12, "slot_head layout drift");

/* Slot layout -------------------------------------------------------------
 *
 *     slot_head (12) | [nonce] | payload[val_len] | [tag] | crc16 | pad
 *
 * The nonce is stored only by an appended AEAD slot, the tag only in a
 * protected store; both are zero-length otherwise, so an unprotected slot is
 * byte for byte what it always was. The size comes from flags and val_len
 * alone, so the walk still strides without reading a payload. */

static inline size_t slot_nonce_len(uint8_t flags)
{
	return (flags & BLOB_DB_SLOT_F_COMPACTED) ? 0 : BLOB_DB_NONCE_LEN;
}

/* Bytes before the CRC, which covers all of them. */
static inline size_t slot_crc_off(uint8_t flags, uint16_t val_len)
{
	return sizeof(struct slot_head) + slot_nonce_len(flags) + val_len +
	       BLOB_DB_TAG_LEN;
}

/* AEAD: the leading bytes of the payload kept in the clear and authenticated
 * as associated data: a segment's owner and sequence, so the mount-time sweep
 * finds owners without decrypting. */
static inline size_t slot_clear_len(uint8_t flags, uint16_t val_len)
{
#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
	if ((flags & BLOB_DB_SLOT_F_SEGMENT) &&
	    val_len >= sizeof(struct blob_db_seg_hdr)) {
		return sizeof(struct blob_db_seg_hdr);
	}
#endif
	ARG_UNUSED(flags);
	ARG_UNUSED(val_len);
	return 0;
}

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
};

static int read_bucket_hdr(uint16_t bid, bool mac, bool *valid);
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
	bool      wedged;          /* a torn compaction makes further writes unsafe */
#if defined(CONFIG_BLOB_DB_PROTECTED)
	uint8_t   store_uuid[BLOB_DB_UUID_LEN]; /* drawn at format; in every key */
#endif
#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
	uint64_t  seq;             /* RAM: next nonce counter value */
	uint64_t  seq_hint;        /* durable leading ceiling on seq */
	uint32_t  salt;            /* random per mount: nonce prefix */
#endif
} st;

/* Damage and tampering, for the application (<app/lib/blob_db_security.h>).
 * blob_db has acted on what it found by the time this runs. */
__weak void blob_db_event(enum blob_db_event_kind kind,
			  enum blob_db_event_layer layer, uint32_t where)
{
	static const char *const kinds[] = { "corrupt", "tampered" };
	static const char *const layers[] = { "ubi", "master", "bucket", "slot" };

	LOG_WRN("event: %s %s 0x%08x", layers[layer], kinds[kind], where);
}

/* Close the store and drop the keys opened with it. */
static void store_close(void)
{
	blob_db_store_close();
#if defined(CONFIG_BLOB_DB_KEYS)
	blob_db_keys_close();
#endif
}

static inline off_t peb_offset(uint16_t peb)
{
	return (off_t)peb * (off_t)st.peb_size;
}

/* On-flash size of a slot with these flags and payload length. Rounded up to
 * the backing device's write-block-size so writes always land on aligned
 * boundaries. Any padding bytes read back as the erased value (0xff) and
 * don't affect the CRC (which sits at slot_crc_off(), before the padding is
 * added — see append_slot / slot_view_at). */
static inline size_t slot_size_of(uint8_t flags, uint16_t val_len)
{
	const size_t base = slot_crc_off(flags, val_len) + sizeof(uint16_t);
	const size_t a = st.write_align ? st.write_align : 1;

	return (base + a - 1) & ~(a - 1);
}

/* Size of a slot this build appends. */
static inline size_t slot_size_for(uint16_t val_len)
{
	return slot_size_of(0, val_len);
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

/* Cause counters: why the flash counters moved, not by how much. */
#define IO_CAUSE(field)            (g_io.field++)
#define IO_CAUSE_BYTES(field, n)   (g_io.field += (n))
#else
#define IO_CAUSE(field)            ((void)0)
#define IO_CAUSE_BYTES(field, n)   ((void)0)
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
	MASTER_TAMPERED, /* CRCs verify, MAC does not — treat as absent */
};

static const char *const master_class_str[] = {
	"erased", "corrupt", "foreign", "ok", "tampered"
};

#if defined(CONFIG_BLOB_DB_PROTECTED)
/* The MAC over a master body, under K_meta of the uuid it names. */
static int master_mac(const uint8_t *buf, uint8_t *mac)
{
	const struct blob_db_iov iov = { buf, sizeof(struct blob_db_master_hdr) };

	return blob_db_meta_mac(&iov, 1, mac);
}

/* Check the tail and the MAC of a master whose prefix and body verified,
 * setting *@p cls. K_meta depends on the uuid inside the master, so derive for
 * that uuid: a forged uuid only yields a key under which nothing verifies.
 * Returns a negative errno only when the crypto backend fails, which says
 * nothing about the sector. */
static int master_authenticate(uint8_t slot, const uint8_t *buf,
			       enum master_class *cls)
{
	const struct blob_db_master_hdr *m = (const void *)buf;
	const struct blob_db_master_tail *t =
		(const void *)(buf + sizeof(struct blob_db_master_hdr));
	const size_t tail_span = BLOB_DB_MASTER_LEN - sizeof(uint32_t);

	if (t->tail_crc32 != crc32_ieee(buf, tail_span)) {
		LOG_WRN("master %u: tail CRC mismatch — treating as corrupt",
			slot);
		blob_db_event(BLOB_DB_EVENT_CORRUPT, BLOB_DB_EVENT_MASTER, slot);
		*cls = MASTER_CORRUPT;
		return 0;
	}

	int rc = blob_db_keys_derive(m->store_uuid);

	if (rc == 0) {
		const struct blob_db_iov iov = {
			buf, sizeof(struct blob_db_master_hdr)
		};

		rc = blob_db_meta_verify(&iov, 1, t->mac);
	}
	if (rc == -EBADMSG) {
		LOG_ERR("master %u: MAC does not verify — ignoring it", slot);
		blob_db_event(BLOB_DB_EVENT_TAMPERED, BLOB_DB_EVENT_MASTER, slot);
		*cls = MASTER_TAMPERED;
		return 0;
	}
	if (rc < 0) {
		LOG_ERR("master %u: cannot authenticate (%d)", slot, rc);
		return rc;
	}
	*cls = MASTER_OK;
	return 0;
}
#endif

static int read_master(uint8_t slot, struct blob_db_master_hdr *out,
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
		blob_db_event(BLOB_DB_EVENT_CORRUPT, BLOB_DB_EVENT_MASTER, slot);
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
	/* Not below either, in a protected build: a store nobody authenticated
	 * would be adopted with slots that carry no tag (a downgrade). */
	if (c->format_major < BLOB_DB_FORMAT_MAJOR_MIN) {
		LOG_ERR("master %u: format major %u is an unprotected store; "
			"this build reads %u..%u", slot, c->format_major,
			BLOB_DB_FORMAT_MAJOR_MIN, BLOB_DB_FORMAT_MAJOR);
		*cls = MASTER_FOREIGN;
		return 0;
	}
	if (c->hdr_len < BLOB_DB_MASTER_LEN ||
	    c->hdr_len > BLOB_DB_MASTER_HDR_MAX) {
		LOG_ERR("master %u: hdr_len %u outside [%zu, %u]", slot,
			c->hdr_len, (size_t)BLOB_DB_MASTER_LEN,
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
		blob_db_event(BLOB_DB_EVENT_CORRUPT, BLOB_DB_EVENT_MASTER, slot);
		return 0;   /* MASTER_CORRUPT */
	}

#if defined(CONFIG_BLOB_DB_PROTECTED)
	/* The mode is part of the format: AUTH and AEAD slots are not each
	 * other's, and a tag length decides where every slot ends. */
	if (((const struct blob_db_master_hdr *)buf)->suite != BLOB_DB_SUITE) {
		LOG_ERR("master %u: protection suite 0x%02x, this build is "
			"0x%02x", slot,
			((const struct blob_db_master_hdr *)buf)->suite,
			BLOB_DB_SUITE);
		*cls = MASTER_FOREIGN;
		return 0;
	}
	rc = master_authenticate(slot, buf, cls);
	if (rc < 0 || *cls != MASTER_OK) {
		*cls = rc < 0 ? MASTER_CORRUPT : *cls;
		return rc;
	}
#endif

	memcpy(out, buf, sizeof(*out));
	*cls = MASTER_OK;
	return 0;
}

static int write_master(uint8_t slot, uint32_t gen, uint8_t state,
			uint16_t compacting_bid, uint64_t next_id_hint)
{
	const uint64_t seg_owner = st.seg_owner;
	int rc;

	/* Staged 0xff-filled so any write-alignment padding matches erased
	 * flash rather than introducing zero bits. */
	uint8_t __aligned(4) buf[BLOB_DB_MASTER_HDR_MAX];

	memset(buf, 0xff, sizeof(buf));

	struct blob_db_master_hdr *hdr = (struct blob_db_master_hdr *)buf;

	memset(hdr, 0, sizeof(*hdr));
	memcpy(hdr->compat.magic, MASTER_MAGIC, 4);
	hdr->compat.format_major = BLOB_DB_FORMAT_MAJOR;
	hdr->compat.format_minor = BLOB_DB_FORMAT_MINOR;
	hdr->compat.hdr_len      = BLOB_DB_MASTER_LEN;
	hdr->compat.reserved     = 0;
	hdr->compat.prefix_crc16 = prefix_crc(hdr);

	hdr->generation     = gen;
	hdr->state          = state;
	hdr->compacting_bid = compacting_bid;
	hdr->suite          = BLOB_DB_SUITE;
	hdr->next_id_hint   = next_id_hint;
	hdr->seg_owner      = seg_owner;
#if defined(CONFIG_BLOB_DB_PROTECTED)
	memcpy(hdr->store_uuid, st.store_uuid, sizeof(hdr->store_uuid));
#endif
#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
	/* The ceiling, never the counter: see seq_reserve(). */
	hdr->seq_hint       = st.seq_hint;
#endif
	hdr->hdr_crc32      = hdr_crc32(hdr, sizeof(*hdr));

#if defined(CONFIG_BLOB_DB_PROTECTED)
	struct blob_db_master_tail *tail =
		(struct blob_db_master_tail *)(buf + sizeof(*hdr));

	rc = master_mac(buf, tail->mac);
	if (rc < 0) {
		return rc;
	}
	tail->tail_crc32 = crc32_ieee(buf, BLOB_DB_MASTER_LEN - sizeof(uint32_t));
#endif

	const size_t a = st.write_align ? st.write_align : 1;
	const size_t len = ROUND_UP(BLOB_DB_MASTER_LEN, a);

	if (len > sizeof(buf)) {
		LOG_ERR("write_align %zu too large to stage a master", a);
		return -ENOTSUP;
	}

	IO_CAUSE(master_writes);
	rc = blob_db_store_replace(peb_offset(slot), buf, len);
	if (rc < 0) {
		LOG_ERR("master %u write failed: %d", slot, rc);
		return rc;
	}
	return 0;
}

/* Persist a raised leading id ceiling by committing a fresh CLEAN master on
 * the inactive slot (double-buffered: a torn write leaves the old ceiling,
 * which is still a safe — if lower — bound, and leaves only one valid master,
 * so mount re-raises it from the scan). On success the new master is
 * active. */
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

#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
/* Take @p n consecutive nonce counter values, persisting a new leading
 * ceiling first when they would reach the durable one.
 *
 * seq_hint follows next_id_hint's rules (§13.1): mount resumes at the
 * ceiling, a ceiling BLOB_DB_SEQ_STEP ahead is on flash before any value past
 * the old one is used, and every master write carries the ceiling, never the
 * counter. So no value is used twice across a power cut. If an attacker rolls
 * the master back, the device resumes at an old ceiling; the per-mount salt in
 * front of the counter still keeps the nonces apart, barring a 2^-32 match. */
static int seq_reserve(uint64_t n, uint64_t *base)
{
	if (st.seq + n > st.seq_hint) {
		const uint64_t prev = st.seq_hint;
		const uint8_t inactive = !st.active_master;

		st.seq_hint = st.seq + n + CONFIG_BLOB_DB_SEQ_STEP;

		int rc = write_master(inactive, st.master_gen + 1,
				      BLOB_DB_STATE_CLEAN, 0, st.next_id_hint);

		if (rc < 0) {
			LOG_ERR("persist seq_hint=%llu failed: %d",
				(unsigned long long)st.seq_hint, rc);
			st.seq_hint = prev;
			return rc;
		}
		st.active_master = inactive;
		st.master_gen++;
	}
	*base = st.seq;
	st.seq += n;
	return 0;
}

/* salt (4) || counter (8, LE). */
static void nonce_make(uint8_t *nonce, uint32_t salt, uint64_t seq)
{
	sys_put_le32(salt, nonce);
	sys_put_le64(seq, nonce + sizeof(uint32_t));
}
#endif

/* Start a new store incarnation: a fresh uuid, so fresh keys, and with them a
 * nonce counter that may start again. Every format path calls this before its
 * first master write. */
static int store_new_identity(void)
{
#if defined(CONFIG_BLOB_DB_PROTECTED)
	int rc = blob_db_random(st.store_uuid, sizeof(st.store_uuid));

	if (rc == 0) {
		rc = blob_db_keys_derive(st.store_uuid);
	}
	if (rc < 0) {
		return rc;
	}
#endif
#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
	/* 0 is no value: a bucket header's cbase_seq of 0 means "none". The
	 * first master carries a ceiling ahead of the first value. */
	st.seq = 1;
	st.seq_hint = 1 + CONFIG_BLOB_DB_SEQ_STEP;
#endif
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

	/* Unformatted, or a header that cannot be trusted: no ids to count.
	 * The latter is never formatted over (bucket_check()), so its ids
	 * cannot meet the counter again either. A protected store checks the
	 * MAC here, in the scan it already makes: a bucket that fails stays
	 * out of the slot index and answers -EIO when it is used. */
	if (!bucket_hdr_valid((const uint8_t *)&bhdr, bid)) {
		return 0;
	}
	rc = bucket_hdr_authentic((const uint8_t *)&bhdr, bid);
	if (rc == -EBADMSG) {
		blob_db_event(BLOB_DB_EVENT_TAMPERED, BLOB_DB_EVENT_BUCKET, bid);
		return 0;
	}
	if (rc < 0) {
		return rc;
	}

	off_t cursor = BLOB_DB_BUCKET_DATA_OFF;
	const size_t min_slot = BLOB_DB_SLOT_MIN_OVERHEAD;

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
		const size_t slot_sz = slot_size_of(shdr.flags, shdr.val_len);
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
	rc = store_new_identity();
	if (rc < 0) {
		return rc;
	}

	/* Root id (=1) is consumed at format time (see @blob_db_root in
	 * blob_db.h), so the leading ceiling starts at 2. */
	rc = write_master(BLOB_DB_MASTER_A_SECTOR, 1,
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
 * format writes through the same primitives a mount does. discard is passed
 * to the backend: only the format path may have it replace a substrate it
 * cannot open (blob_db_store.h). */
static int store_open_and_validate(bool discard)
{
	struct blob_db_store_geom geom;
	int rc;

#if defined(CONFIG_BLOB_DB_KEYS)
	/* Keying material first: the UBI backend hands the same key to UBI
	 * at attach. */
	rc = blob_db_keys_open();
	if (rc < 0) {
		return rc;
	}
#endif
	rc = blob_db_store_open(&geom, discard);
	if (rc < 0) {
		LOG_ERR("store open: %d", rc);
#if defined(CONFIG_BLOB_DB_KEYS)
		blob_db_keys_close();
#endif
		return rc;
	}

#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
	/* Fresh at every open: if the master is rolled back and the counter
	 * resumes at an old ceiling, the salt still keeps the nonces apart. */
	rc = blob_db_random(&st.salt, sizeof(st.salt));
	if (rc < 0) {
		goto err_close;
	}
#endif

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
	store_close();
	return rc;
}

int blob_db_mount(void)
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

	/* Whether the adopted master's ceiling has to be checked against the
	 * buckets. Every master write carries the highest ceiling saved so far,
	 * and alloc_id saves a ceiling before handing out any id below it, so
	 * when both masters are valid the newer one's ceiling exceeds every id
	 * on flash (§13.1) and mount trusts it. With only one valid master we
	 * may be falling back past a newer one — lost to bit rot, or to a torn
	 * write of the ceiling this very scan raised last time — so scan. That
	 * also covers a store formatted but not yet written to since, whose
	 * second master is still blank; its buckets are nearly all empty.
	 *
	 * A backend that keeps old copies (UBI) can hide the loss: it answers a
	 * newest master that fails its seal by standing the previous copy back
	 * up, valid but with an older ceiling, beside a valid other master. So
	 * damage the store found on either master sector also means scan. */
	bool scan_ids = false;

	if (ca != MASTER_OK && cb != MASTER_OK) {
		/* A partition whose masters are both blank is formatted.
		 * Anything else — bit rot, a power cut while the first master
		 * was written, another backend's substrate (doc/impl/
		 * l0_backends.md §4) — may be data, so discarding it is opt-in:
		 * CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT does what
		 * blob_db_format() would, erasing every bucket with the masters
		 * so no surviving blob can meet a reset id counter. */
		if (ca == MASTER_ERASED && cb == MASTER_ERASED) {
			LOG_INF("virgin partition; formatting");
			rc = format_masters_fresh();
		} else if (IS_ENABLED(CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT)) {
			LOG_WRN("no usable master (A=%s B=%s); erasing the "
				"store per CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT",
				master_class_str[ca], master_class_str[cb]);
			rc = format_whole_store();
		} else {
			LOG_ERR("no usable master (A=%s B=%s); refusing — "
				"blob_db_format() discards the store",
				master_class_str[ca], master_class_str[cb]);
			rc = -EIO;
		}
		if (rc < 0) {
			goto err_close;
		}
	} else {
		scan_ids = ca != MASTER_OK || cb != MASTER_OK ||
			   blob_db_store_open_found_damage(
				   peb_offset(BLOB_DB_MASTER_A_SECTOR)) ||
			   blob_db_store_open_found_damage(
				   peb_offset(BLOB_DB_MASTER_B_SECTOR));

		bool pick_a;

		if (ca == MASTER_OK && cb != MASTER_OK) {
			pick_a = true;
		} else if (ca != MASTER_OK && cb == MASTER_OK) {
			pick_a = false;
		} else {
			pick_a = m_a.generation >= m_b.generation;
		}
		const struct blob_db_master_hdr *m = pick_a ? &m_a : &m_b;

#if defined(CONFIG_BLOB_DB_PROTECTED)
		/* Every master a store writes carries the uuid it was
		 * formatted with, and a format erases the other master. Two
		 * authentic masters of different stores means one was put
		 * there: which one is the store cannot be told, so neither is
		 * trusted. */
		if (ca == MASTER_OK && cb == MASTER_OK &&
		    memcmp(m_a.store_uuid, m_b.store_uuid,
			   sizeof(m_a.store_uuid)) != 0) {
			LOG_ERR("masters name different stores; refusing");
			blob_db_event(BLOB_DB_EVENT_TAMPERED,
				      BLOB_DB_EVENT_MASTER, pick_a ? 1 : 0);
			rc = -EIO;
			goto err_close;
		}
		memcpy(st.store_uuid, m->store_uuid, sizeof(st.store_uuid));
		rc = blob_db_keys_derive(st.store_uuid);
		if (rc < 0) {
			goto err_close;
		}
#endif
#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
		/* Resume at the ceiling: nothing at or above it was used. */
		st.seq_hint = m->seq_hint;
		st.seq = m->seq_hint;
		if (st.seq == 0) {
			LOG_ERR("master carries no nonce ceiling");
			rc = -EIO;
			goto err_close;
		}
#endif

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

	/* The adopted ceiling is in doubt: find the highest id on flash. If it
	 * out-runs the ceiling, raise the ceiling AND persist it before any
	 * allocation can hand the id back out. */
	if (scan_ids) {
		uint64_t max_id = (st.next_id == 0) ? 0 : (st.next_id - 1);

		for (uint16_t bid = 0; bid < st.n_buckets; bid++) {
			rc = scan_bucket(bid, &max_id);
			if (rc < 0) {
				LOG_ERR("scan_bucket(%u): %d", bid, rc);
				goto err_close;
			}
		}
		if (max_id + 1 > st.next_id_hint) {
			LOG_WRN("bucket scan out-ran ceiling (%llu >= %llu); "
				"raising", (unsigned long long)max_id,
				st.next_id_hint);
			st.next_id = max_id + 1;
			rc = persist_next_id_hint(max_id + 1 +
						  BLOB_DB_ID_HINT_STEP);
			if (rc < 0) {
				goto err_close;
			}
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
		bool root_live = false;

		rc = read_bucket_hdr(root_bid, true, &formatted);
		if (rc == 0 && formatted) {
			struct bucket_scan sc;

			rc = find_live_slot(root_bid, BLOB_DB_ROOT_ID, &sc,
					    g_bbuf);
			root_live = (rc == 0);
			if (rc == -ENOENT) {
				rc = 0;
			}
		}
		if (rc == -EIO) {
			/* A root we cannot read is not a root we may rebind:
			 * that formats its bucket over whatever is in it. Mount,
			 * and let the root answer -EIO like the rest of that
			 * bucket. */
			LOG_ERR("root bucket %u unreadable; root left as it is",
				root_bid);
			root_live = true;
		} else if (rc < 0) {
			goto err_close;
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
	blob_db_sidx_disable();
	store_close();
	return rc;
}

int blob_db_unmount(void)
{
	if (!st.mounted) {
		return 0;
	}
	store_close();
	st.mounted = false;
	IDX_INVALIDATE();
	blob_db_sidx_disable();
	LOG_INF("unmounted");
	return 0;
}

/* Stage 4 — put / get / update / delete / exists ------------------------ */

static inline off_t bucket_offset(uint16_t bid)
{
	return peb_offset(BLOB_DB_FIRST_BUCKET + bid);
}

static inline uint16_t id_to_bucket(uint64_t id)
{
	return (uint16_t)(id % st.n_buckets);
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

/* The MAC over a bucket header: CMAC(K_meta, [0, 28)). */
#if defined(CONFIG_BLOB_DB_PROTECTED)
static const struct blob_db_iov *bucket_hdr_iov(const uint8_t *buf,
						struct blob_db_iov *iov)
{
	iov->p = buf;
	iov->n = offsetof(struct blob_db_bucket_hdr, mac);
	return iov;
}
#endif

/* Whether a header that bucket_hdr_valid() accepted was written by this
 * store: 0, -EBADMSG, or -EIO from the crypto backend. A header copied from
 * another bucket names the wrong bucket; one copied from another store, or
 * forged, fails here. Nothing to check unprotected. The caller raises the
 * event. */
static int bucket_hdr_authentic(const uint8_t *buf, uint16_t bid)
{
#if defined(CONFIG_BLOB_DB_PROTECTED)
	const struct blob_db_bucket_hdr *bhdr =
		(const struct blob_db_bucket_hdr *)buf;
	struct blob_db_iov iov;
	const int rc = blob_db_meta_verify(bucket_hdr_iov(buf, &iov), 1,
					   bhdr->mac);

	if (rc == -EBADMSG) {
		LOG_ERR("bucket %u: header MAC does not verify", bid);
	}
	return rc;
#else
	ARG_UNUSED(buf);
	ARG_UNUSED(bid);
	return 0;
#endif
}

static bool all_erased(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i] != 0xff) {
			return false;
		}
	}
	return true;
}

/* How a bucket sector presents itself. Only UNFORMATTED may be formatted. */
enum bucket_class {
	BUCKET_UNFORMATTED, /* nothing a format could destroy                */
	BUCKET_OK,          /* a header for this bucket that verifies         */
	BUCKET_CORRUPT,     /* a header that fails its CRC, over slots        */
	BUCKET_DISPLACED,   /* a header for another bucket or store, or one
			     * whose MAC fails, over slots                    */
	BUCKET_CRYPTO_ERR,  /* the MAC could not be computed                  */
};

/* What the slot stream needs to judge a header: the header, and the head of
 * the first slot behind it. */
#define BUCKET_PEEK_LEN  (BLOB_DB_BUCKET_DATA_OFF + sizeof(struct slot_head))

/* Classify the BUCKET_PEEK_LEN bytes at the start of bucket @p bid.
 *
 * A header that cannot be trusted used to mean "unformatted", so one bit flip
 * in it hid the bucket's blobs as absent and the next write to the bucket
 * formatted over them. It no longer does: a bucket is unformatted only when
 * nothing a format would erase is in it --
 *  - its header is erased (never written, or erased by format);
 *  - its magic is zeroed: invalidate_buckets() retired it on purpose;
 *  - or the first slot behind a bad header is erased, which is what a format
 *    torn inside its header write leaves: the sector was erased just before.
 * Anything else is a bucket of slots under a header we cannot read, and is
 * reported rather than discarded.
 *
 * A protected store checks the header's MAC when @p mac is set; a valid CRC
 * over a MAC that fails is tampering, classed with a displaced header. It
 * does not honour a zeroed magic: anyone can clear bits, so its erase_all
 * erases buckets instead (invalidate_buckets()). */
static enum bucket_class bucket_classify(const uint8_t *buf, uint16_t bid,
					 bool mac)
{
	static const uint8_t zero_magic[4];
	const struct blob_db_bucket_hdr *bhdr =
		(const struct blob_db_bucket_hdr *)buf;
	enum bucket_class cls;

	if (all_erased(buf, sizeof(*bhdr))) {
		return BUCKET_UNFORMATTED;
	}
	if (memcmp(bhdr->magic, BUCKET_MAGIC, 4) != 0 ||
	    bhdr->hdr_crc32 != hdr_crc32(bhdr, sizeof(*bhdr))) {
		cls = BUCKET_CORRUPT;
	} else if (bhdr->bucket_id != bid) {
		cls = BUCKET_DISPLACED;
	} else if (!mac) {
		return BUCKET_OK;
	} else {
		const int rc = bucket_hdr_authentic(buf, bid);

		if (rc == 0) {
			return BUCKET_OK;
		}
		if (rc != -EBADMSG) {
			return BUCKET_CRYPTO_ERR;
		}
		cls = BUCKET_DISPLACED;
	}

	if ((!IS_ENABLED(CONFIG_BLOB_DB_PROTECTED) &&
	     memcmp(bhdr->magic, zero_magic, sizeof(zero_magic)) == 0) ||
	    all_erased(buf + BLOB_DB_BUCKET_DATA_OFF, sizeof(struct slot_head))) {
		return BUCKET_UNFORMATTED;
	}
	return cls;
}

/* Turn a classification into what the operations act on: formatted or not,
 * or -EIO for a bucket whose slots are there but cannot be trusted. That
 * bucket answers -EIO on every path and is never formatted, unless
 * CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT asks for the old behaviour, where it
 * reads as empty and the next write to it formats over it. */
static int bucket_check(const uint8_t *buf, uint16_t bid, bool mac,
			bool *formatted)
{
	const enum bucket_class cls = bucket_classify(buf, bid, mac);

	*formatted = (cls == BUCKET_OK);
	if (cls == BUCKET_OK || cls == BUCKET_UNFORMATTED) {
		return 0;
	}
	if (cls == BUCKET_CRYPTO_ERR) {
		return -EIO;
	}
	blob_db_event(cls == BUCKET_CORRUPT ? BLOB_DB_EVENT_CORRUPT
					    : BLOB_DB_EVENT_TAMPERED,
		      BLOB_DB_EVENT_BUCKET, bid);
	if (IS_ENABLED(CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT)) {
		LOG_WRN("bucket %u header %s; treating it as unformatted per "
			"CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT", bid,
			cls == BUCKET_CORRUPT ? "corrupt" : "displaced");
		return 0;
	}
	LOG_ERR("bucket %u header %s over programmed slots; refusing the "
		"bucket (-EIO)", bid,
		cls == BUCKET_CORRUPT ? "corrupt" : "displaced");
	return -EIO;
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
	if (off + BLOB_DB_SLOT_MIN_OVERHEAD > (off_t)st.peb_size) {
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
	const size_t ssz = slot_size_of(shdr->flags, shdr->val_len);

	if (off + ssz > (off_t)st.peb_size) {
		return false;
	}

	uint64_t id;
	memcpy(&id, buf + off + sizeof(*shdr), sizeof(id));
	uint16_t stored;
	const size_t crc_off = slot_crc_off(shdr->flags, shdr->val_len);

	memcpy(&stored, buf + off + crc_off, sizeof(stored));
	if (crc16_ccitt(0xffff, buf + off, crc_off) != stored) {
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

/* Slot protection ---------------------------------------------------------
 *
 *   AUTH  tag = CMAC_T(K_slot, slot_head || payload)
 *   AEAD  CCM_T(K_slot, nonce, AAD = every clear byte before the ciphertext
 *         -- slot_head, a stored nonce, a segment's clear prefix -- payload)
 *
 * The CRC is computed after the tag, over everything before it. -EACCES from
 * these means "does not authenticate"; the caller reports tampering.
 */

#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
/* The nonce of a COMPACTED slot at @p off, from its bucket header's base. */
static void compacted_nonce(const struct blob_db_bucket_hdr *bhdr, off_t off,
			    uint8_t *nonce)
{
	nonce_make(nonce, bhdr->cbase_salt, bhdr->cbase_seq + (uint64_t)off);
}
#endif

#if defined(CONFIG_BLOB_DB_PROTECTED)
/* Verify, and in AEAD decrypt, the slot whose head is @p head and whose
 * remaining bytes -- [nonce] || payload || tag -- are at @p body.
 *
 * @p cnonce is a COMPACTED slot's derived nonce. The payload, in the clear,
 * goes to @p out, which may be @p body itself or lie before it, or NULL to
 * verify only. Returns 0, -EACCES, or -EIO. */
static int slot_open(const struct slot_head *head, const uint8_t *body,
		     const uint8_t *cnonce, uint8_t *out)
{
	const uint16_t len = head->hdr.val_len;
	int rc;

#if defined(CONFIG_BLOB_DB_PROTECT_AUTH)
	ARG_UNUSED(cnonce);

	const struct blob_db_iov iov[] = {
		{ head, sizeof(*head) }, { body, len },
	};

	rc = blob_db_slot_mac_verify(iov, ARRAY_SIZE(iov), body + len);
	if (rc == 0 && out != NULL && out != body && len > 0) {
		memmove(out, body, len);
	}
#else
	const uint8_t flags = head->hdr.flags;
	const size_t nlen = slot_nonce_len(flags);
	const size_t clear = slot_clear_len(flags, len);
	const uint8_t *nonce = nlen ? body : cnonce;
	uint8_t clear_copy[16];

	BUILD_ASSERT(sizeof(clear_copy) >= sizeof(struct blob_db_seg_hdr),
		     "clear prefix staging too small");

	if (nonce == NULL) {
		return -EACCES;
	}
	memcpy(clear_copy, body + nlen, clear);

	const struct blob_db_iov aad[] = {
		{ head, sizeof(*head) }, { body, nlen + clear },
	};

	rc = blob_db_slot_open(nonce, aad, ARRAY_SIZE(aad), body + nlen + clear,
			       out != NULL ? out + clear : NULL, len - clear,
			       body + nlen + len);
	if (rc == 0 && out != NULL) {
		memcpy(out, clear_copy, clear);
	}
#endif
	return rc == -EBADMSG ? -EACCES : rc;
}
#endif /* CONFIG_BLOB_DB_PROTECTED */

/* Finish the slot assembled at @p s -- head, any stored nonce, and the
 * payload in the clear at its place -- by sealing it in place and writing its
 * tag and CRC. @p cnonce is a COMPACTED slot's derived nonce. */
static int slot_seal(uint8_t *s, const uint8_t *cnonce)
{
	const struct slot_head *head = (const struct slot_head *)s;
	const uint8_t flags = head->hdr.flags;
	const uint16_t len = head->hdr.val_len;
	const size_t crc_off = slot_crc_off(flags, len);

#if defined(CONFIG_BLOB_DB_PROTECT_AUTH)
	ARG_UNUSED(cnonce);

	const struct blob_db_iov iov = { s, sizeof(*head) + len };
	int rc = blob_db_slot_mac(&iov, 1, s + sizeof(*head) + len);

	if (rc < 0) {
		return rc;
	}
#elif defined(CONFIG_BLOB_DB_PROTECT_AEAD)
	const size_t nlen = slot_nonce_len(flags);
	const size_t clear = slot_clear_len(flags, len);
	const size_t po = sizeof(*head) + nlen;
	const struct blob_db_iov aad = { s, po + clear };
	int rc = blob_db_slot_seal(nlen ? s + sizeof(*head) : cnonce, &aad, 1,
				   s + po + clear, len - clear, s + po + len);

	if (rc < 0) {
		return rc;
	}
#else
	ARG_UNUSED(cnonce);
#endif
	const uint16_t crc = crc16_ccitt(0xffff, s, crc_off);

	memcpy(s + crc_off, &crc, sizeof(crc));
	return 0;
}

/* A slot whose CRC verifies and whose tag does not was written by someone
 * else: report it. Unlike a CRC failure it gets no fallback to an older slot,
 * which would let whoever wrote it choose which version the reader gets. */
static void slot_tampered(uint16_t bid, off_t off)
{
	LOG_ERR("bucket %u: slot at %ld does not authenticate", bid, (long)off);
	blob_db_event(BLOB_DB_EVENT_TAMPERED, BLOB_DB_EVENT_SLOT,
		      ((uint32_t)bid << 16) | ((uint32_t)off & 0xffff));
}

static void slot_corrupt(uint16_t bid, off_t off)
{
	blob_db_event(BLOB_DB_EVENT_CORRUPT, BLOB_DB_EVENT_SLOT,
		      ((uint32_t)bid << 16) | ((uint32_t)off & 0xffff));
}

static int format_bucket(uint16_t bid)
{
	struct blob_db_bucket_hdr bhdr = {
		.bucket_id = bid,
		.reserved  = 0,
		.gen       = 1,
	};
	int rc;

	memcpy(bhdr.magic, BUCKET_MAGIC, 4);
#if defined(CONFIG_BLOB_DB_PROTECTED)
	/* No compacted slots yet: cbase_seq 0. */
	struct blob_db_iov iov;

	rc = blob_db_meta_mac(bucket_hdr_iov((const uint8_t *)&bhdr, &iov), 1,
			      bhdr.mac);
	if (rc < 0) {
		blob_db_sidx_forget(bid);
		return rc;
	}
#endif
	bhdr.hdr_crc32 = hdr_crc32(&bhdr, sizeof(bhdr));

	IO_CAUSE(bucket_formats);
	rc = blob_db_store_replace(bucket_offset(bid), &bhdr, sizeof(bhdr));
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

/* Whether bucket @p bid is formatted, or -EIO when its header cannot be
 * trusted (bucket_check()).
 *
 * @p mac asks a protected store to check the header's MAC as well. Writers
 * and the mount ask, before they append or format. A read that finds its slot
 * does not need it: the slot's own tag covers its id and store, and a forged
 * cbase only makes a compacted slot's tag fail. A read about to answer
 * -ENOENT does need it, and asks through absent_or_eio(). */
static int read_bucket_hdr(uint16_t bid, bool mac, bool *valid)
{
	uint32_t known;

	/* The index holds a cursor only for a bucket whose header it has seen
	 * valid, and forgets it whenever the bucket is rewritten. It does not
	 * remember having checked a MAC. */
	if ((!mac || !IS_ENABLED(CONFIG_BLOB_DB_PROTECTED)) &&
	    blob_db_sidx_cursor(bid, &known)) {
		*valid = true;
		return 0;
	}

	uint8_t __aligned(4) peek[BUCKET_PEEK_LEN];
	int rc = blob_db_store_read(bucket_offset(bid), peek, sizeof(peek));

	if (rc < 0) {
		return rc;
	}
	return bucket_check(peek, bid, mac, valid);
}

/* The -ENOENT a read is about to return, once the bucket has shown it is
 * this store's: a displaced or forged bucket answers -EIO, never "absent". */
static int absent_or_eio(uint16_t bid)
{
#if defined(CONFIG_BLOB_DB_PROTECTED)
	bool formatted;
	const int rc = read_bucket_hdr(bid, true, &formatted);

	if (rc < 0) {
		return rc;
	}
#else
	ARG_UNUSED(bid);
#endif
	return -ENOENT;
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
	out->from_index = false;
	out->tail_torn = false;

	for (;;) {
		if (cursor + (off_t)BLOB_DB_SLOT_MIN_OVERHEAD >
		    (off_t)st.peb_size) {
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
			/* A slot is programmed from its first byte on, so a
			 * programmed flags byte here means a torn append. */
			out->tail_torn = sh.hdr.flags != 0xff;
			break;
		}

		const size_t ssz = slot_size_of(sh.hdr.flags, sh.hdr.val_len);

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

/* Read the target slot's payload into `dst` and check its CRC, then, in a
 * protected store, its tag.
 *
 * Everything after the 12-byte head -- [nonce] || payload || [tag] || crc --
 * is read in one transaction. The CRC is computed over the header and id
 * bytes exactly as they were read from flash — not over a reconstruction — so
 * a slot written by a version that used a currently-reserved field still
 * verifies against its own bytes. A payload that is decrypted is decrypted
 * into `dst`, never into the caller's buffer, so a slot that fails leaves
 * nothing behind that a caller could take for data. `dst` is a sector buffer.
 *
 * -EBADMSG means the slot is torn or rotten; -EACCES that its CRC verifies
 * and its tag does not; every other negative value is an I/O error.
 */
static int slot_verify(uint16_t bid, const struct bucket_scan *sc, void *dst)
{
	const off_t poff = bucket_offset(bid) + sc->target_off +
			   (off_t)sizeof(struct slot_head);
	const uint8_t flags = sc->target.hdr.flags;
	const uint16_t val_len = sc->target.hdr.val_len;
	const size_t body = slot_crc_off(flags, val_len) -
			    sizeof(struct slot_head);
	uint8_t *const d = dst;
	uint16_t stored;
	int rc;

	rc = blob_db_store_read(poff, d, body + sizeof(stored));
	if (rc < 0) {
		return rc;
	}
	memcpy(&stored, d + body, sizeof(stored));

	uint16_t crc = crc16_ccitt(0xffff, (const uint8_t *)&sc->target,
				   sizeof(sc->target));

	if (crc16_ccitt(crc, d, body) != stored) {
		return -EBADMSG;
	}

#if defined(CONFIG_BLOB_DB_PROTECTED)
	const uint8_t *cnonce = NULL;
#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
	uint8_t nonce[BLOB_DB_NONCE_LEN];

	/* The index caches nothing per bucket, so a compacted slot costs one
	 * small read of its header's nonce base. A forged base only makes the
	 * tag fail. */
	if (flags & BLOB_DB_SLOT_F_COMPACTED) {
		struct blob_db_bucket_hdr bhdr;

		rc = blob_db_store_read(bucket_offset(bid), &bhdr,
					offsetof(struct blob_db_bucket_hdr,
						 reserved1));
		if (rc < 0) {
			return rc;
		}
		if (bhdr.cbase_seq != 0) {
			compacted_nonce(&bhdr, sc->target_off, nonce);
			cnonce = nonce;
		}
	}
#endif
	rc = slot_open(&sc->target, d, cnonce, d);
	if (rc < 0) {
		return rc;
	}
#endif
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
		if (sc->target_off < 0) {
			if (learn && limit == (off_t)st.peb_size) {
				blob_db_sidx_learn(bid, id, -1,
						   (uint32_t)sc->write_cursor);
			}
			return -ENOENT;
		}

		rc = slot_verify(bid, sc, dst);
		if (rc == -EBADMSG) {
			slot_corrupt(bid, sc->target_off);
			limit = sc->target_off;
			continue;
		}
		if (rc == -EACCES) {
			slot_tampered(bid, sc->target_off);
			return -EIO;
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
		return 0;
	}

	int rc = scan_bucket_for(bid, id, (off_t)st.peb_size, sc);

	if (rc == 0) {
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

	if (off + BLOB_DB_SLOT_MIN_OVERHEAD > cursor) {
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
	    off + slot_size_of(sh.hdr.flags, sh.hdr.val_len) > cursor) {
		goto stale;
	}

	sc->write_cursor = cursor;
	sc->target_off = off;
	sc->target = sh;
	sc->from_index = true;
	sc->tail_torn = false;

	rc = slot_verify(bid, sc, dst);
	if (rc == -EBADMSG) {
		return -EAGAIN;   /* rotten: the walk falls back past it */
	}
	if (rc == -EACCES) {
		slot_tampered(bid, off);
		return -EIO;      /* the walk would find the same slot */
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
	int rc;

	if (st.write_align > BLOB_DB_MAX_WRITE_ALIGN) {
		LOG_ERR("write_align %zu exceeds BLOB_DB_MAX_WRITE_ALIGN=%u",
			st.write_align, BLOB_DB_MAX_WRITE_ALIGN);
		return -ENOTSUP;
	}

	struct blob_db_slot_hdr *shdr = (struct blob_db_slot_hdr *)buf;

	shdr->flags    = BLOB_DB_SLOT_F_SEALED | extra_flags;
	shdr->reserved = 0;
	shdr->val_len  = val_len;

	memcpy(buf + sizeof(*shdr), &id, sizeof(id));

	uint8_t *pl = buf + sizeof(struct slot_head) + slot_nonce_len(shdr->flags);

#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
	/* The nonce is taken -- and its ceiling persisted if need be -- before
	 * the slot can reach flash. */
	uint64_t seq;

	rc = seq_reserve(1, &seq);
	if (rc < 0) {
		return rc;
	}
	nonce_make(buf + sizeof(struct slot_head), st.salt, seq);
#endif

	if (hdr_len > 0) {
		memcpy(pl, hdr, hdr_len);
	}
	if (val_len > hdr_len) {
		memcpy(pl + hdr_len, payload, (size_t)val_len - hdr_len);
	}

	/* Tag and CRC over the assembled bytes, so the scatter and contiguous
	 * forms produce identical slots. */
	rc = slot_seal(buf, NULL);
	if (rc < 0) {
		return rc;
	}

	const size_t total = slot_size_for(val_len);
	const size_t base = slot_crc_off(shdr->flags, val_len) + sizeof(uint16_t);

	if (total > base) {
		memset(buf + base, 0xff, total - base);
	}

	rc = blob_db_store_write(bucket_offset(bid) + write_cursor, buf, total);

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

#if defined(CONFIG_BLOB_DB_PROTECTED)
/* Open the slot at @p off of the resident bucket image @p buf, into @p out
 * (another buffer) or nowhere. 0, -EACCES or -EIO. */
static int resident_open(const uint8_t *buf, off_t off, uint8_t *out)
{
	const struct slot_head *head = (const struct slot_head *)(buf + off);
	const uint8_t *cnonce = NULL;

#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
	const struct blob_db_bucket_hdr *bhdr =
		(const struct blob_db_bucket_hdr *)buf;
	uint8_t nonce[BLOB_DB_NONCE_LEN];

	if ((head->hdr.flags & BLOB_DB_SLOT_F_COMPACTED) &&
	    bhdr->cbase_seq != 0) {
		compacted_nonce(bhdr, off, nonce);
		cnonce = nonce;
	}
#endif
	return slot_open(head, buf + off + sizeof(*head), cnonce, out);
}
#endif

/* Build a compacted image of bucket `bid` in new_buf using old_buf as the
 * source. Returns the number of bytes written into new_buf (at least the
 * bucket header), or a negative errno. Skipped entries: tombstones, and any
 * slot whose id has a later occurrence in the same bucket.
 *
 * In a protected store a tombstone is dropped only if it authenticates, and
 * in AEAD every slot kept is opened and resealed at its new offset, COMPACTED,
 * under the nonce base (@p cbase_seq, @p cbase_salt) the new header carries.
 * A slot that does not open is copied as it is, so reads of it keep answering
 * -EIO: compaction never decides what tampered data means. */
static int build_compacted_image(const uint8_t *old_buf, uint8_t *new_buf,
				 uint16_t bid, uint64_t cbase_seq,
				 uint32_t cbase_salt)
{
	const struct blob_db_bucket_hdr *old_bhdr =
		(const struct blob_db_bucket_hdr *)old_buf;

	struct blob_db_bucket_hdr *new_bhdr =
		(struct blob_db_bucket_hdr *)new_buf;

	memset(new_bhdr, 0, sizeof(*new_bhdr));
	memcpy(new_bhdr->magic, BUCKET_MAGIC, 4);
	new_bhdr->bucket_id = bid;
	new_bhdr->reserved  = 0;
	new_bhdr->gen       = old_bhdr->gen + 1;
#if defined(CONFIG_BLOB_DB_PROTECTED)
	new_bhdr->cbase_seq  = cbase_seq;
	new_bhdr->cbase_salt = cbase_salt;
	{
		struct blob_db_iov iov;
		int rc = blob_db_meta_mac(bucket_hdr_iov(new_buf, &iov), 1,
					  new_bhdr->mac);

		if (rc < 0) {
			return rc;
		}
	}
#else
	ARG_UNUSED(cbase_seq);
	ARG_UNUSED(cbase_salt);
#endif
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

		bool keep = !superseded;

#if defined(CONFIG_BLOB_DB_PROTECTED)
		if (keep && (sv.flags & BLOB_DB_SLOT_F_TOMBSTONE)) {
			const int rc = resident_open(old_buf, cursor, NULL);

			if (rc == 0) {
				keep = false;
			} else if (rc != -EACCES) {
				return rc;
			}
		}
#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
		else if (keep) {
			/* Open straight into the new slot's payload position,
			 * then seal it there under its new nonce. */
			uint8_t *d = new_buf + new_cursor;
			const uint8_t flags = sv.flags | BLOB_DB_SLOT_F_COMPACTED;
			int rc;

			memcpy(d, old_buf + cursor, sizeof(struct slot_head));
			d[0] = flags;
			rc = resident_open(old_buf, cursor,
					   d + sizeof(struct slot_head));
			if (rc == 0) {
				uint8_t nonce[BLOB_DB_NONCE_LEN];
				const size_t size = slot_size_of(flags,
								 sv.val_len);
				const size_t base =
					slot_crc_off(flags, sv.val_len) +
					sizeof(uint16_t);

				compacted_nonce(new_bhdr, new_cursor, nonce);
				rc = slot_seal(d, nonce);
				if (rc < 0) {
					return rc;
				}
				memset(d + base, 0xff, size - base);
				new_cursor += size;
				cursor += sv.total_size;
				continue;
			}
			if (rc != -EACCES) {
				return rc;
			}
		}
#endif
#else
		if (sv.flags & BLOB_DB_SLOT_F_TOMBSTONE) {
			keep = false;
		}
#endif
		if (keep) {
			memcpy(new_buf + new_cursor,
			       old_buf + cursor, sv.total_size);
			new_cursor += sv.total_size;
		}

		cursor += sv.total_size;
	}

	return (int)new_cursor;
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
#if defined(CONFIG_BLOB_DB_PROTECTED)
	const struct blob_db_bucket_hdr *bhdr =
		(const struct blob_db_bucket_hdr *)image;

	seal->bucket_id = bhdr->bucket_id;
	seal->reserved  = 0;
	seal->image_gen = bhdr->gen;

	const struct blob_db_iov iov[] = {
		{ seal, offsetof(struct blob_db_scratch_seal, mac) },
		{ image, image_len },
	};
	int rc = blob_db_meta_mac(iov, ARRAY_SIZE(iov), seal->mac);

	if (rc < 0) {
		return rc;
	}
#endif
	seal->seal_crc32  = hdr_crc32(seal, sizeof(*seal));

	return blob_db_store_write(scratch_off + scratch_seal_off(), buf,
				   scratch_seal_len());
}

/* Is the scratch sector a complete, self-consistent compacted image of
 * bucket `bid`? `image` must already hold the sector's first peb_size bytes.
 *
 * In a protected store the seal's MAC covers the seal and the whole image:
 * the one place a MAC spans a bucket, checked once per recovery and never on
 * a read. A seal whose CRC fails is unsealed, as ever; one whose CRC verifies
 * over a MAC that does not was not written by this store, and the bucket is
 * left alone. */
static bool scratch_seal_valid(const uint8_t *image, uint16_t bid,
			       size_t *image_len)
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
#if defined(CONFIG_BLOB_DB_PROTECTED)
	const struct blob_db_iov iov[] = {
		{ &seal, offsetof(struct blob_db_scratch_seal, mac) },
		{ image, seal.image_len },
	};

	if (seal.bucket_id != bid ||
	    blob_db_meta_verify(iov, ARRAY_SIZE(iov), seal.mac) != 0) {
		LOG_ERR("recover: scratch seal for bucket %u does not "
			"authenticate", bid);
		blob_db_event(BLOB_DB_EVENT_TAMPERED, BLOB_DB_EVENT_BUCKET, bid);
		return false;
	}
#else
	ARG_UNUSED(bid);
#endif
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

	/* A backend that replaces a block atomically needs none of the window
	 * below: the bucket holds the old image or the new one whatever the
	 * power does, so there is nothing for recover_compaction() to settle and
	 * no reason to stamp COMPACTING. A failed replace leaves the old image,
	 * so it is reported rather than wedging the store. */
	if (blob_db_store_replace_is_atomic()) {
		return blob_db_store_replace(bucket_off, new_buf, new_len);
	}

	/* Step 1: enter atomic window.
	 *
	 * Every master this function stamps carries st.next_id_hint, not
	 * st.next_id. The field is a leading ceiling (§13.1), and alloc_id
	 * batches its master writes: it persists a ceiling BLOB_DB_ID_HINT_STEP
	 * ids ahead, then hands ids out from RAM until it reaches that ceiling.
	 * Writing the allocation pointer here would publish a master below the
	 * ceiling alloc_id is still allocating against — lowering the durable
	 * bound without lowering the RAM copy alloc_id compares to, so nothing
	 * would re-persist it before the next mount. Mount's bucket scan would
	 * repair the bound ids, but the ceiling exists to cover the ids a scan
	 * cannot see: allocated-but-unbound. Carry the ceiling forward instead.
	 */
	uint8_t inactive = !st.active_master;
	int rc = write_master(inactive, st.master_gen + 1,
			      BLOB_DB_STATE_COMPACTING, bid, st.next_id_hint);
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

	/* From here to step 4 the sealed scratch is authoritative: a mount now
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

	/* Step 3: bucket. */
	COMPACT_OR_WEDGE(blob_db_store_erase(bucket_off, st.peb_size));
	COMPACT_OR_WEDGE(blob_db_store_write(bucket_off, new_buf, new_len));

	/* Step 4: erase scratch. */
	COMPACT_OR_WEDGE(blob_db_store_erase(scratch_off, st.peb_size));

	/* Step 5: leave atomic window. */
	inactive = !st.active_master;
	COMPACT_OR_WEDGE(write_master(inactive, st.master_gen + 1,
				      BLOB_DB_STATE_CLEAN, 0, st.next_id_hint));
#undef COMPACT_OR_WEDGE
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
		/* Callers compact only a bucket they found formatted, so the
		 * header changed under them: never rebuild a bucket from slots
		 * whose header cannot be trusted. */
		LOG_ERR("compact: bucket %u header no longer valid", bid);
		return -EIO;
	}
	rc = bucket_hdr_authentic(g_bbuf, bid);
	if (rc == -EBADMSG) {
		blob_db_event(BLOB_DB_EVENT_TAMPERED, BLOB_DB_EVENT_BUCKET, bid);
		return -EIO;
	}
	if (rc < 0) {
		return rc;
	}

	uint64_t cbase_seq = 0;
	uint32_t cbase_salt = 0;

#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
	/* One counter value per byte of the sector, so every offset a slot can
	 * start at has a nonce of its own, and no appended slot ever draws one
	 * of them. Reserved -- and its ceiling persisted -- before the image
	 * that uses it is built, let alone written. */
	rc = seq_reserve(st.peb_size, &cbase_seq);
	if (rc < 0) {
		return rc;
	}
	cbase_salt = st.salt;
#endif

	g_bbuf_new_busy = true;

	const int built = build_compacted_image(g_bbuf, g_bbuf_new, bid,
						cbase_seq, cbase_salt);

	if (built < 0) {
		g_bbuf_new_busy = false;
		return built;
	}

	const size_t new_len = (size_t)built;

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

	if (scratch_seal_valid(g_bbuf, bid, &image_len) &&
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

	/* The ceiling, for the reason given in compact_commit(). Only mount
	 * calls this, and mount has just set next_id = next_id_hint, so the two
	 * are equal here today; naming the ceiling keeps that an observation
	 * rather than something this write depends on. */
	rc = write_master(inactive, st.master_gen + 1,
			  BLOB_DB_STATE_CLEAN, 0, st.next_id_hint);
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
static uint8_t __aligned(4) g_seg_a[8u * CONFIG_BLOB_DB_MAX_SEGMENTS];
static uint8_t __aligned(4) g_seg_b[8u * CONFIG_BLOB_DB_MAX_SEGMENTS];

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

void blob_db_test_wedge(void)
{
	st.wedged = true;
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

	int rc = read_bucket_hdr(bid, false, &formatted);

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

	int rc = read_bucket_hdr(bid, false, &formatted);

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
		int rc = read_bucket_hdr(bid, true, &formatted);

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

		if (read_bucket_hdr(bid, true, &formatted) < 0 || !formatted) {
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

		if (read_bucket_hdr(bid, false, &formatted) < 0 || !formatted) {
			continue;
		}

		off_t cursor = BLOB_DB_BUCKET_DATA_OFF;

		for (;;) {
			struct bucket_scan sc;

			/* Walk this bucket one slot at a time by asking for a
			 * target that cannot match, then stepping the cursor
			 * ourselves via a scan limited to what we have seen. */
			struct slot_head sh;

			if (cursor + (off_t)BLOB_DB_SLOT_MIN_OVERHEAD >
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

			const size_t ssz = slot_size_of(sh.hdr.flags,
							sh.hdr.val_len);

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

	int rc = read_bucket_hdr(bid, true, &formatted);

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

	int rc = read_bucket_hdr(bid, false, &formatted);
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
	if (rc == -ENOENT) {
		return absent_or_eio(bid);
	}
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

int blob_db_update(uint64_t id, const void *payload, size_t len)
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

	int rc = read_bucket_hdr(bid, true, &formatted);
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

int blob_db_delete(uint64_t id)
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

	int rc = read_bucket_hdr(bid, true, &formatted);
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

bool blob_db_exists(uint64_t id)
{
	if (!st.mounted || id == 0) {
		return false;
	}

	const uint16_t bid = id_to_bucket(id);
	bool formatted;

	if (read_bucket_hdr(bid, false, &formatted) < 0 || !formatted) {
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
 * non-tombstoned slot for its id within this bucket. O(n²) over slots in
 * the bucket; the per-bucket count is small in normal use. Returns 0 if
 * walk completed, or the non-zero value the callback returned to abort.
 *
 * With @p open, a protected store hands the callback the payload opened into
 * g_bbuf_new, and a slot that does not authenticate -- a forged tombstone
 * included -- is not visited but sets *@p unreadable to -EIO. Without it
 * (count), only the clear headers are read. */
typedef int (*live_slot_visit_t)(uint64_t id, const uint8_t *payload,
				  uint16_t val_len, void *user);

static int for_each_live_slot(const uint8_t *buf, uint16_t bid, bool open,
			      live_slot_visit_t cb, void *user,
			      int *unreadable)
{
	off_t cursor = BLOB_DB_BUCKET_DATA_OFF;

	ARG_UNUSED(bid);
	ARG_UNUSED(open);
	ARG_UNUSED(unreadable);

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

		if (superseded || internal) {
			cursor += sv.total_size;
			continue;
		}

		const uint8_t *payload = buf + cursor +
					 sizeof(struct slot_head) +
					 slot_nonce_len(sv.flags);

#if defined(CONFIG_BLOB_DB_PROTECTED)
		if (open) {
			const int orc = resident_open(buf, cursor, g_bbuf_new);

			if (orc == -EACCES) {
				slot_tampered(bid, cursor);
				*unreadable = -EIO;
				cursor += sv.total_size;
				continue;
			}
			if (orc < 0) {
				return orc;
			}
			payload = g_bbuf_new;
		}
#endif
		if (!(sv.flags & BLOB_DB_SLOT_F_TOMBSTONE)) {
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
		bool formatted;

		if (read_bucket(bid, g_bbuf) < 0 ||
		    bucket_check(g_bbuf, bid, true, &formatted) < 0 || !formatted) {
			continue;
		}
		(void)for_each_live_slot(g_bbuf, bid, false, count_visitor,
					 &total, NULL);
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
	int unreadable = 0;

	for (uint16_t bid = 0; bid < st.n_buckets; bid++) {
		bool formatted;

		if (read_bucket(bid, g_bbuf) < 0) {
			continue;
		}
		/* A bucket that cannot be trusted is not empty: visit the rest,
		 * then say the walk was incomplete. */
		if (bucket_check(g_bbuf, bid, true, &formatted) < 0) {
			unreadable = -EIO;
			continue;
		}
		if (!formatted) {
			continue;
		}

		int rc = for_each_live_slot(g_bbuf, bid, true, iterate_visitor,
					    &t, &unreadable);

		if (rc) {
			return rc;  /* user-requested early stop */
		}
	}
	return unreadable;
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
		.master_gen    = st.master_gen,
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

	if (!slot_view_at(buf, off, &sv)) {
		return false;
	}
	*out = (struct blob_db_core_slot){
		.id         = sv.id,
		.flags      = sv.flags,
		.val_len    = sv.val_len,
		.total_size = sv.total_size,
	};
	return true;
}

bool blob_db_core_bucket_hdr_valid(const uint8_t *buf, uint16_t bid)
{
	return bucket_hdr_valid(buf, bid);
}
#endif /* CONFIG_BLOB_DB_INSPECT */

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
	int rc = read_bucket_hdr(bid, false, &formatted);

	if (rc < 0) {
		return rc;
	}
	if (!formatted) {
		return -ENOENT;
	}

	struct bucket_scan sc;

	rc = find_live_slot(bid, id, &sc, g_bbuf);
	if (rc == -ENOENT) {
		return absent_or_eio(bid);
	}
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
	int rc = read_bucket_hdr(bid, false, &formatted);

	if (rc < 0) {
		return rc;
	}
	if (!formatted) {
		return -ENOENT;
	}

	struct bucket_scan sc;

	rc = find_live_slot(bid, id, &sc, g_bbuf);
	if (rc == -ENOENT) {
		return absent_or_eio(bid);
	}
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

int blob_db_write(uint64_t id, size_t offset, const void *buf, size_t len)
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
	int rc = read_bucket_hdr(bid, true, &formatted);

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
	rc = store_new_identity();
	if (rc < 0) {
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
	st.seg_owner     = 0;

	return bind_root_empty();
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
		store_close();
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
 * programming them to 0x00 is equally legal. Buckets that are already
 * unformatted are skipped, so fresh sectors cost one header read each.
 *
 * A protected store erases each bucket instead: an attacker can zero a magic
 * as easily as this can, so there a zeroed magic does not mean unformatted.
 * erase_all is rare; it costs one erase per bucket in use.
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
		uint8_t __aligned(4) peek[BUCKET_PEEK_LEN];
		int rc = blob_db_store_read(off, peek, sizeof(peek));

		if (rc < 0) {
			LOG_ERR("invalidate: peek bid %u: %d", bid, rc);
			return rc;
		}
		/* Already unformatted. Any other header is retired too, valid
		 * or not: a damaged one would otherwise outlive the erase as a
		 * bucket that answers -EIO (bucket_classify()). */
		if (bucket_classify(peek, bid, false) == BUCKET_UNFORMATTED) {
			continue;
		}
		/* A protected store does not take a zeroed magic for
		 * "unformatted" (anyone can clear bits), so it erases. On UBI
		 * that erases every copy awaiting reclaim too. */
		rc = IS_ENABLED(CONFIG_BLOB_DB_PROTECTED)
			     ? blob_db_store_erase(off, st.peb_size)
			: blob_db_store_replace_is_atomic()
			     ? blob_db_store_replace(off, zeros, zlen)
			     : blob_db_store_write(off, zeros, zlen);
		if (rc < 0) {
			LOG_ERR("invalidate: bid %u: %d", bid, rc);
			return rc;
		}
	}
	return 0;
}

int blob_db_erase_all(void)
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
	if (st.wedged) {
		return -EIO;   /* torn compaction; see compact_commit() */
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
		 * g_bbuf just to decide whether formatting is needed. A bucket
		 * that cannot be trusted is left as it is: preparing is an
		 * optimisation, and formatting would discard its slots. */
		bool formatted;
		int rc = read_bucket_hdr(bid, true, &formatted);

		if (rc == -EIO || (rc == 0 && formatted)) {
			continue;
		}
		if (rc < 0) {
			LOG_ERR("prepare: read bid %u: %d", bid, rc);
			return rc;
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

int blob_db_maintain(uint32_t budget, struct blob_db_maint_result *result)
{
	struct blob_db_maint_result res = { 0 };
	int rc;

	if (!st.mounted) {
		rc = -ENODEV;
	} else if (st.wedged) {
		rc = -EIO;   /* torn compaction; see compact_commit() */
	} else {
		/* Below blob_db's view: every block reads the same afterwards,
		 * so no cached bucket or index state goes stale. */
		rc = blob_db_store_maintain(budget, &res.performed, &res.more);
	}

	if (result != NULL) {
		*result = res;
	}
	return rc;
}
