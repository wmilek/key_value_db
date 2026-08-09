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

#include <stdint.h>

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

/* Master state machine values. */
#define BLOB_DB_STATE_CLEAN       0x00
#define BLOB_DB_STATE_COMPACTING  0x01

/* On-flash format version.
 *
 * MAJOR marks an incompatible change: software that does not know a major
 * refuses the store with -ENOTSUP rather than touching it. MINOR marks an
 * additive one: fields are appended to the master body and hdr_len grows, so
 * older software validates and reads the prefix of the header it knows and
 * ignores the tail. A minor bump must keep hdr_len <= BLOB_DB_MASTER_HDR_MAX;
 * exceeding that is a major change.
 */
#define BLOB_DB_FORMAT_MAJOR      1
#define BLOB_DB_FORMAT_MINOR      0

/* Largest master header any version may declare. Bounds the read buffer that
 * older software must be able to stage a foreign header in. */
#define BLOB_DB_MASTER_HDR_MAX    128

/* Durable id allocation (contract §2 "no reuse", impl design §13.1).
 *
 * next_id_hint on the master is a LEADING ceiling: an exclusive upper bound
 * on every id ever returned by alloc_id — i.e. no id >= next_id_hint has been
 * (or ever will be, until the hint is raised) handed out. Mount takes
 * next_id = next_id_hint, so an allocated-but-unbound id is never re-issued
 * after a crash. alloc_id persists a fresh ceiling this many ids ahead
 * whenever the current one is reached, amortizing the master write. */
#define BLOB_DB_ID_HINT_STEP      256

/* Slot flags. After erase the flags byte is 0xff, which trivially fails
 * the "is sealed" check. A slot is considered valid when it has the
 * SEALED bit set AND its CRC verifies.
 */
#define BLOB_DB_SLOT_F_SEALED     (1u << 0)
#define BLOB_DB_SLOT_F_TOMBSTONE  (1u << 1)

/* Every flag bit this version understands. A slot carrying a bit outside this
 * mask was written by software we do not understand, and its payload must not
 * be handed out as data — see slot_view_at().
 *
 * This is defense in depth, not the primary guard: introducing a slot flag
 * changes what a payload *means*, so it is a MAJOR format change and mount
 * refuses the store before any slot is read (§ format version above). The mask
 * covers the case where that discipline is broken.
 */
#define BLOB_DB_SLOT_F_KNOWN                                                   \
	(BLOB_DB_SLOT_F_SEALED | BLOB_DB_SLOT_F_TOMBSTONE)

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

/* Master sector header (64 B). Lives at offset 0 of sectors 0 and 1; the
 * sector with the higher valid generation is authoritative.
 *
 * Fixed size with reserved padding, deliberately: hdr_crc32 stays at a
 * constant offset over a constant span, so a later MINOR revision that
 * consumes reserved space does not move anything an older reader depends on.
 * A change that cannot fit here — or that older software would misread rather
 * than merely miss — is a MAJOR change instead. The sector is >= 4 KB, so the
 * padding is free.
 */
struct __packed blob_db_master_hdr {
	struct blob_db_compat_hdr compat;  /*  0..11                            */
	uint32_t generation;      /* 12..15  monotonic LE                       */
	uint8_t  state;           /* 16      BLOB_DB_STATE_*                    */
	uint16_t compacting_bid;  /* 17..18  meaningful only when COMPACTING    */
	uint8_t  reserved0;       /* 19                                         */
	uint64_t next_id_hint;    /* 20..27  leading id ceiling                 */
	uint8_t  reserved1[32];   /* 28..59  future MINOR revisions             */
	uint32_t hdr_crc32;       /* 60..63  CRC32-IEEE over bytes [0, 60)      */
};
BUILD_ASSERT(sizeof(struct blob_db_master_hdr) == 64,
	     "blob_db_master_hdr layout drift");
BUILD_ASSERT(sizeof(struct blob_db_master_hdr) <= BLOB_DB_MASTER_HDR_MAX,
	     "master header exceeds the bound older readers can stage");

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

#endif /* LIB_BLOB_DB_INTERNAL_H_ */
