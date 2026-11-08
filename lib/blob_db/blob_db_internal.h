/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db — internal on-flash layout and constants.
 *
 * See doc/layers/l1_blob_db.md §5 "On-flash format".
 */

#ifndef LIB_BLOB_DB_INTERNAL_H_
#define LIB_BLOB_DB_INTERNAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

/* Sector roles within the partition. */
#define BLOB_DB_MASTER_A_SECTOR   0
#define BLOB_DB_MASTER_B_SECTOR   1
#define BLOB_DB_SCRATCH_SECTOR    2
#define BLOB_DB_FIRST_BUCKET      3

/* On-flash magic markers. Stored as 4 raw bytes in flash; not byte-swapped. */
#define BLOB_DB_MASTER_MAGIC      "BDMS"
#define BLOB_DB_BUCKET_MAGIC      "BDBH"

/* On-flash format version (doc/proposals/2026-10-28-master-log.md).
 *
 * MAJOR marks an incompatible layout: software that does not know a major
 * refuses the store with -ENOTSUP rather than touching it. Major 5 is the
 * master as a log; majors 1-4 are refused, never migrated (D10).
 *
 * MINOR is 0 for all of major 5 (D15). Everything major 5 gains later is a
 * feature bit in the header (BLOB_DB_FEAT_*), which every writer copies at
 * rotation; a minor number would be overwritten by an older writer's.
 */
#define BLOB_DB_FORMAT_MAJOR      5
#define BLOB_DB_FORMAT_MINOR      0

/* The window every reader stages at the start of a master sector. The major-5
 * header sits at its start; the record log begins at its end. */
#define BLOB_DB_MASTER_HDR_MAX    128
#define BLOB_DB_MASTER_LOG_OFF    BLOB_DB_MASTER_HDR_MAX

/* Durable id allocation (contract §2 "no reuse", impl design §13.1).
 *
 * next_id_hint in a master record is a LEADING ceiling: an exclusive upper
 * bound on every id ever returned by alloc_id — i.e. no id >= next_id_hint has
 * been (or ever will be, until the hint is raised) handed out. Mount takes
 * next_id = next_id_hint, so an allocated-but-unbound id is never re-issued
 * after a crash. Every record raises the ceiling to this many ids past the
 * next one (D14), so alloc_id needs a record of its own only when no other
 * record has carried the ceiling forward within the step. */
#define BLOB_DB_ID_HINT_STEP      CONFIG_BLOB_DB_ID_HINT_STEP

/* Slot flags. After erase the flags byte is 0xff, which trivially fails
 * the "is sealed" check. A slot is considered valid when it has the
 * SEALED bit set AND its CRC verifies.
 */
#define BLOB_DB_SLOT_F_SEALED     (1u << 0)
#define BLOB_DB_SLOT_F_TOMBSTONE  (1u << 1)
/* Large objects (CONFIG_BLOB_DB_LARGE_PAYLOADS). INDEXED marks the slot at the
 * object's own id whose payload is a segment table; SEGMENT marks a slot
 * holding one chunk of another object. Both change what a payload means, so
 * they belong to the incompat feature BLOB_DB_FEAT_LARGE_PAYLOADS below. */
#define BLOB_DB_SLOT_F_INDEXED    (1u << 2)
#define BLOB_DB_SLOT_F_SEGMENT    (1u << 3)
/* Bit 4 is reserved for PROTECTED's COMPACTED (PR #63); see the registry
 * below. */
/* DELTA marks a slot whose payload is the change from an older slot of the
 * same id in the same bucket (incompat feature BLOB_DB_FEAT_DELTA), laid out
 * as struct blob_db_delta_hdr followed by the changed bytes. */
#define BLOB_DB_SLOT_F_DELTA      (1u << 5)

/* Every flag bit this version understands. A slot carrying a bit outside this
 * mask was written by software we do not understand, and its payload must not
 * be handed out as data. Nor may it be dropped: a whole one closes its bucket
 * to reads and writes, and compaction leaves it alone — see
 * slot_foreign_head().
 *
 * This is defense in depth, not the primary guard: a slot flag belongs to an
 * incompat feature, set in the master header at format, and mount refuses a
 * store whose features this build does not know before any slot is read. The
 * mask covers the case where that discipline is broken.
 */
#if defined(CONFIG_BLOB_DB_DELTA)
#define BLOB_DB_SLOT_F_KNOWN                                                   \
	(BLOB_DB_SLOT_F_SEALED | BLOB_DB_SLOT_F_TOMBSTONE |                    \
	 BLOB_DB_SLOT_F_INDEXED | BLOB_DB_SLOT_F_SEGMENT | BLOB_DB_SLOT_F_DELTA)
#elif defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
#define BLOB_DB_SLOT_F_KNOWN                                                   \
	(BLOB_DB_SLOT_F_SEALED | BLOB_DB_SLOT_F_TOMBSTONE |                    \
	 BLOB_DB_SLOT_F_INDEXED | BLOB_DB_SLOT_F_SEGMENT)
#else
#define BLOB_DB_SLOT_F_KNOWN                                                   \
	(BLOB_DB_SLOT_F_SEALED | BLOB_DB_SLOT_F_TOMBSTONE)
#endif

/* Whether a slot's flags byte says something this build cannot read: a bit
 * outside BLOB_DB_SLOT_F_KNOWN, or a combination the format leaves undefined.
 * DELTA combines with none of TOMBSTONE, INDEXED and SEGMENT; those
 * combinations stay free for a later feature, so a slot carrying one is
 * foreign like an unknown bit, never data and never rot. */
#define BLOB_DB_SLOT_FLAGS_UNKNOWN(f)                                          \
	((((f) & ~(uint8_t)BLOB_DB_SLOT_F_KNOWN) != 0) ||                      \
	 (((f) & BLOB_DB_SLOT_F_DELTA) &&                                      \
	  ((f) & (BLOB_DB_SLOT_F_TOMBSTONE | BLOB_DB_SLOT_F_INDEXED |          \
		  BLOB_DB_SLOT_F_SEGMENT))))

/* Format features (§4 of the master-log proposal). One registry owns every
 * bit, so two features cannot claim the same slot flag. A slot-flag feature
 * is set at format (D7), and a build writes its flags only into a store whose
 * headers carry it (D20). */
#define BLOB_DB_FEAT_LARGE_PAYLOADS  (1u << 0)   /* incompat; INDEXED, SEGMENT */
/* bit 1: PROTECTED (phase 2, PR #63), owns slot flag bit 4 COMPACTED */
#define BLOB_DB_FEAT_DELTA           (1u << 2)   /* incompat; DELTA */

#define BLOB_DB_SLOT_F_BASE                                                    \
	(BLOB_DB_SLOT_F_SEALED | BLOB_DB_SLOT_F_TOMBSTONE)
#define BLOB_DB_SLOT_F_PROTECTED_RESERVED  (1u << 4)
#define BLOB_DB_FEAT_FLAGS_LARGE_PAYLOADS                                      \
	(BLOB_DB_SLOT_F_INDEXED | BLOB_DB_SLOT_F_SEGMENT)
#define BLOB_DB_FEAT_FLAGS_DELTA  BLOB_DB_SLOT_F_DELTA
BUILD_ASSERT((BLOB_DB_FEAT_FLAGS_LARGE_PAYLOADS & BLOB_DB_SLOT_F_BASE) == 0,
	     "a feature's slot flags must not overlap the base flags");
BUILD_ASSERT((BLOB_DB_FEAT_FLAGS_DELTA &
	      (BLOB_DB_SLOT_F_BASE | BLOB_DB_FEAT_FLAGS_LARGE_PAYLOADS |
	       BLOB_DB_SLOT_F_PROTECTED_RESERVED)) == 0,
	     "two features own the same slot flag");

/* The incompat features this library implements, whether or not a build is
 * configured with them: a store carrying one of them needs a differently
 * configured build, not a newer one. */
#define BLOB_DB_LIB_INCOMPAT                                                   \
	(BLOB_DB_FEAT_LARGE_PAYLOADS | BLOB_DB_FEAT_DELTA)

/* What this build knows, writes at format, and requires of a store. */
#ifdef CONFIG_BLOB_DB_LARGE_PAYLOADS
#define BLOB_DB_BUILD_INCOMPAT_LP BLOB_DB_FEAT_LARGE_PAYLOADS
#else
#define BLOB_DB_BUILD_INCOMPAT_LP 0u
#endif
#ifdef CONFIG_BLOB_DB_DELTA
#define BLOB_DB_BUILD_INCOMPAT_DELTA BLOB_DB_FEAT_DELTA
#else
#define BLOB_DB_BUILD_INCOMPAT_DELTA 0u
#endif
#define BLOB_DB_BUILD_INCOMPAT                                                 \
	(BLOB_DB_BUILD_INCOMPAT_LP | BLOB_DB_BUILD_INCOMPAT_DELTA)

/* The payload of a DELTA slot (feature DELTA): this header, then the changed
 * bytes. Every field is little-endian.
 *
 * kind      0 = one changed range; any other value is refused. A new
 *           encoding is a new kind under a new incompat feature, so it costs
 *           no slot flag.
 * prev_off  offset in the bucket of the older slot of the same id this delta
 *           applies to: a full slot or another DELTA, never a tombstone.
 *           Bucket offsets fit 16 bits (see BLOB_DB_DELTA_MAX_BUCKET).
 * pre, suf  bytes kept from the start and from the end of the older value.
 *
 * The value is pre bytes of the older value, the changed bytes, then its last
 * suf bytes: pre + suf <= the older value's length, and the new length is
 * pre + suf + (val_len - sizeof(struct blob_db_delta_hdr)). */
struct __packed blob_db_delta_hdr {
	uint8_t  kind;
	uint16_t prev_off;
	uint16_t pre;
	uint16_t suf;
};
BUILD_ASSERT(sizeof(struct blob_db_delta_hdr) == 7, "delta header drift");
#define BLOB_DB_DELTA_KIND_RANGE  0u

/* A reader follows at most this many DELTA slots to reach a full one. Part
 * of the format, not a build option: CONFIG_BLOB_DB_DELTA_MAX_CHAIN only
 * bounds what a build writes, so a build with a lower value still reads a
 * chain another build wrote. */
#define BLOB_DB_DELTA_CHAIN_MAX   32u

/* prev_off is 16 bits, so a store with deltas has buckets of at most 64 KiB. */
#define BLOB_DB_DELTA_MAX_BUCKET  65536u
#define BLOB_DB_BUILD_RO_COMPAT   0u
#define BLOB_DB_BUILD_REQUIRED    0u   /* phase 2: PROTECTED */
#define BLOB_DB_BUILD_SUITE       0u
#define BLOB_DB_BUILD_HDR_LEN     64u  /* phase 2: 84 when protected */

/* Frozen compatibility prefix (12 B).
 *
 * THIS LAYOUT MUST NEVER CHANGE. It is what lets blob_db of any vintage read
 * format_major out of a store written by any other vintage and decide whether
 * it understands the rest — including a store written by software that did not
 * exist when it was compiled.
 *
 * It carries its own CRC, separate from the body's, and that separation is the
 * point: a *newer* writer still produces a prefix whose CRC verifies, so its
 * format_major can be trusted and the store deliberately refused; bit rot
 * produces a prefix whose CRC fails, which is a different (recoverable)
 * situation. Sharing one CRC with the body would conflate them, and — worse —
 * would move with the body, so older software could not even locate it.
 */
struct __packed blob_db_compat_hdr {
	uint8_t  magic[4];        /* 'B','D','M','S' — allocator identity      */
	uint8_t  format_major;    /* incompatible; refuse what you don't know  */
	uint8_t  format_minor;    /* additive within a major; safe to ignore   */
	uint16_t hdr_len;         /* total master header length, incl. prefix  */
	uint16_t reserved;
	uint16_t prefix_crc16;    /* CRC16-CCITT(0xffff) over the leading 10 B */
};
BUILD_ASSERT(sizeof(struct blob_db_compat_hdr) == 12,
	     "blob_db_compat_hdr is frozen; changing it breaks every reader");

/* Major-5 master sector (doc/proposals/2026-10-28-master-log.md §3):
 *
 *     offset 0     blob_db_master_hdr   written once per erase
 *     offset 128   record[0]            written with the header
 *     128 + i*R    record[i]            appended, one per update
 *
 * R is sizeof(struct blob_db_master_rec) rounded up to write_align. Filler
 * (header end to 128, record end to R) is written 0xff; reserved fields 0.
 * Integers are little-endian. */

/* Header (64 B). Nothing in it changes until the sector is erased again. */
struct __packed blob_db_master_hdr {
	struct blob_db_compat_hdr prefix;  /*  0..11 frozen                     */
	uint32_t rot_gen;         /* 12..15  +1 per rotation; higher is newer   */
	uint32_t peb_size;        /* 16..19  must equal the device's            */
	uint16_t n_buckets;       /* 20..21  must equal the device's            */
	uint16_t rec_size;        /* 22..23  R; must equal the build's          */
	uint32_t incompat;        /* 24..27  a reader must know every bit       */
	uint32_t ro_compat;       /* 28..31  a writer must know every bit       */
	uint32_t compat;          /* 32..35  anyone may ignore a bit            */
	uint8_t  suite;           /* 36      0 = unprotected                    */
	uint8_t  reserved0[3];    /* 37..39                                     */
	uint8_t  store_uuid[16];  /* 40..55  0 unless protected                 */
	uint16_t max_payload_len; /* 56..57  at most the build's cap (D19, D21) */
	uint16_t write_align;     /* 58..59  must equal the device's (D19)      */
	uint32_t hdr_crc32;       /* 60..63  CRC32-IEEE over [0, 60)            */
};
BUILD_ASSERT(sizeof(struct blob_db_master_hdr) == 64,
	     "blob_db_master_hdr layout drift");
BUILD_ASSERT(offsetof(struct blob_db_master_hdr, rot_gen) == 12, "hdr drift");
BUILD_ASSERT(offsetof(struct blob_db_master_hdr, incompat) == 24, "hdr drift");
BUILD_ASSERT(offsetof(struct blob_db_master_hdr, suite) == 36, "hdr drift");
BUILD_ASSERT(offsetof(struct blob_db_master_hdr, store_uuid) == 40, "hdr drift");
BUILD_ASSERT(offsetof(struct blob_db_master_hdr, max_payload_len) == 56,
	     "hdr drift");
BUILD_ASSERT(offsetof(struct blob_db_master_hdr, hdr_crc32) == 60, "hdr drift");
BUILD_ASSERT(sizeof(struct blob_db_master_hdr) <= BLOB_DB_MASTER_HDR_MAX,
	     "master header exceeds the bound older readers can stage");

/* Record (48 B unprotected): a full copy of the mutable state. Its CRC covers
 * LE32(rot_gen) of its own header too (D16), and rec_no must equal its slot,
 * so a record verifies only where it was written. */
struct __packed blob_db_master_rec {
	uint32_t rec_no;          /*  0..3   == slot index in this sector       */
	uint32_t reserved0;       /*  4..7   compaction keeps no state (D17)    */
	uint64_t next_id_hint;    /*  8..15  leading id ceiling                 */
	uint64_t seq_hint;        /* 16..23  AEAD nonce ceiling; 0 unprotected  */
	uint64_t seg_owner;       /* 24..31  segmented write in flight; 0 none  */
	uint32_t reserved1;       /* 32..35  features live in the header only   */
	uint32_t fuses;           /* 36..39  opaque, OR-only (§4.5). FROZEN for
				   *         major 5: moving it is a new major,
				   *         or an older build would clear it.  */
	uint32_t reserved2;       /* 40..43                                     */
	uint32_t crc32;           /* 44..47  CRC32-IEEE over LE32(rot_gen) ‖ [0, 44) */
};
BUILD_ASSERT(sizeof(struct blob_db_master_rec) == 48,
	     "blob_db_master_rec layout drift");
BUILD_ASSERT(offsetof(struct blob_db_master_rec, next_id_hint) == 8, "rec drift");
BUILD_ASSERT(offsetof(struct blob_db_master_rec, seg_owner) == 24, "rec drift");
BUILD_ASSERT(offsetof(struct blob_db_master_rec, fuses) == 36,
	     "the fuse word is frozen at record bytes 36..39 for major 5");
BUILD_ASSERT(offsetof(struct blob_db_master_rec, crc32) == 44, "rec drift");

/* Bucket sector header (16 B). Lives at offset 0 of every bucket sector,
 * written once after that sector is erased. */
struct __packed blob_db_bucket_hdr {
	uint8_t  magic[4];        /* 'B','D','B','H' */
	uint16_t bucket_id;       /* redundant sanity check */
	uint16_t reserved;
	uint32_t gen;             /* incremented on each compaction of this bucket */
	uint32_t hdr_crc32;       /* CRC32-IEEE over the preceding 12 bytes */
};
BUILD_ASSERT(sizeof(struct blob_db_bucket_hdr) == 16,
	     "blob_db_bucket_hdr layout drift");

/* Entry slot header (4 B). Full slot on flash:
 *
 *     slot_hdr (4) | id (8 LE) | payload[val_len] | crc16 (2 LE)
 *
 * The CRC16 is CCITT(0xFFFF) over slot_hdr + id + payload.
 */
struct __packed blob_db_slot_hdr {
	uint8_t  flags;           /* BLOB_DB_SLOT_F_* bits */
	uint8_t  reserved;
	uint16_t val_len;         /* LE, 0..BLOB_DB_MAX_PAYLOAD_LEN */
};
BUILD_ASSERT(sizeof(struct blob_db_slot_hdr) == 4,
	     "blob_db_slot_hdr layout drift");

/* Per-slot overhead (header + id + crc). */
#define BLOB_DB_SLOT_OVERHEAD                                                  \
	(sizeof(struct blob_db_slot_hdr) + sizeof(uint64_t) + sizeof(uint16_t))
BUILD_ASSERT(BLOB_DB_SLOT_OVERHEAD == 14, "slot overhead drift");

/* Data area within a bucket starts immediately after its header. */
#define BLOB_DB_BUCKET_DATA_OFF   (sizeof(struct blob_db_bucket_hdr))

/* Compaction scratch seal (16 B), written at the tail of the scratch sector
 * AFTER the compacted image, and therefore the only trustworthy answer to
 * "did the whole image land?".
 *
 * The bucket header cannot answer that question: it sits at offset 0 of the
 * image, so it is the FIRST thing written, and a write torn anywhere after it
 * leaves a perfectly valid header over a truncated slot stream. Recovery that
 * trusted the header would copy that truncation over an intact bucket and lose
 * every blob past the tear.
 *
 * Sealing at the tail keeps every write forward-only (offsets only increase),
 * which the UBI backend's ubi_leb_write_at() mapping relies on — the reason
 * the header is not simply written last instead.
 */
struct __packed blob_db_scratch_seal {
	uint8_t  magic[4];        /* 'B','D','S','L'                          */
	uint32_t image_len;       /* bytes of compacted image at offset 0      */
	uint32_t image_crc32;     /* CRC32-IEEE over those image_len bytes     */
	uint32_t seal_crc32;      /* CRC32-IEEE over the preceding 12 bytes    */
};
BUILD_ASSERT(sizeof(struct blob_db_scratch_seal) == 16,
	     "blob_db_scratch_seal layout drift");

#define BLOB_DB_SCRATCH_MAGIC     "BDSL"

/* Large-object records (CONFIG_BLOB_DB_LARGE_PAYLOADS) -------------------- */

/* Payload of an INDEXED slot: this header, then seg_count u64 segment ids.
 * The whole thing is an ordinary single-slot payload, which is what bounds how
 * large an object can be — see BLOB_DB_LARGE_PAYLOADS in Kconfig. */
struct __packed blob_db_index_hdr {
	uint8_t  magic[2];        /* 'B','X'                                    */
	uint8_t  version;         /* 1                                          */
	uint8_t  flags;           /* reserved, 0                                */
	uint32_t total_len;       /* logical object length                      */
	uint32_t payload_crc32;   /* CRC32-IEEE over the reassembled object     */
	uint16_t seg_count;       /* K                                          */
	uint16_t seg_len;         /* chunk size; the last segment may be short  */
};
BUILD_ASSERT(sizeof(struct blob_db_index_hdr) == 16,
	     "blob_db_index_hdr layout drift");

#define BLOB_DB_INDEX_MAGIC       "BX"
#define BLOB_DB_INDEX_VERSION     1

/* Set when payload_crc32 covers the object as it stands. A partial write
 * (blob_db_write) clears it rather than pay a full-object read to recompute a
 * whole-object checksum — which would defeat the point of writing one segment.
 * Every byte is still covered by its slot's CRC16; what lapses is only the
 * end-to-end check across the gather.
 *
 * Nothing verifies payload_crc32 today: it is stored for a future fsck. The
 * flag exists so that verifier can tell "mismatch" from "not maintained"
 * instead of rejecting every object that was ever partially written. */
#define BLOB_DB_INDEX_F_CRC32     (1u << 0)

/* Prefix of a SEGMENT slot's payload, ahead of the chunk bytes.
 *
 * owner_id is what makes orphan reclaim possible with no RAM index: any
 * segment whose owner's live index does not list it is garbage, and that rule
 * alone covers every crash point in a segmented write (see the sweep). It
 * lives in the payload rather than the slot header so the header stays 4 bytes
 * for the millions of small slots that will never be segments. */
struct __packed blob_db_seg_hdr {
	uint64_t owner_id;        /* the user-visible id this chunk belongs to  */
	uint16_t seq;             /* chunk index, 0..K-1                        */
	uint16_t reserved;
};
BUILD_ASSERT(sizeof(struct blob_db_seg_hdr) == 12,
	     "blob_db_seg_hdr layout drift");

/* The lock every public call holds (recursive k_mutex). For the parts of the
 * library outside blob_db.c that expose API of their own: the inspection
 * calls, the UBI backend's maintenance calls and the background helper. */
void blob_db_lock(void);
void blob_db_unlock(void);

/* Hooks for the inspection module (CONFIG_BLOB_DB_INSPECT) ---------------
 *
 * blob_db_inspect.c lives outside blob_db.c so that <app/lib/blob_db.h>
 * carries only the contract. These are the few pieces of core state and
 * parsing it needs; they are internal to the library, not public API. */
#if defined(CONFIG_BLOB_DB_INSPECT)
struct blob_db_core_state {
	bool     mounted;
	bool     wedged;
	size_t   fa_size;
	size_t   peb_size;
	size_t   write_align;
	uint16_t n_pebs;
	uint16_t n_buckets;
	uint8_t  active_master;
	uint32_t rot_gen;
	uint16_t rec_size;
	uint16_t log_used;
	uint16_t log_cap;
	uint32_t incompat;
	uint32_t ro_compat;
	uint32_t compat;
	uint8_t  suite;
	uint32_t fuses;
	uint64_t next_id;
	uint64_t next_id_hint;
	uint64_t seg_owner;
};

/* Snapshot of the mount state. */
void blob_db_core_state_get(struct blob_db_core_state *out);

/* The core's sector-sized scratch buffer. Shared, so only usable with the
 * blob_db lock held. */
uint8_t *blob_db_core_sector_buf(void);

/* A slot, as the core's own walk sees it. */
struct blob_db_core_slot {
	uint64_t id;
	uint8_t  flags;
	uint16_t val_len;
	size_t   total_size;
	bool     intact;      /* CRC verifies; a rotten slot is stepped over */
};

/* Parse the slot at buf[off] of a resident bucket image. False at end of log
 * (erased, unsealed, unknown flag, implausible length); a CRC mismatch is a
 * slot like any other, with `intact` false. */
bool blob_db_core_slot_at(const uint8_t *buf, off_t off,
			  struct blob_db_core_slot *out);

/* Whether a resident bucket image carries a valid header for bucket bid. */
bool blob_db_core_bucket_hdr_valid(const uint8_t *buf, uint16_t bid);
#endif /* CONFIG_BLOB_DB_INSPECT */

#endif /* LIB_BLOB_DB_INTERNAL_H_ */
