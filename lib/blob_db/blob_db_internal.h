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

/* Slot flags. After erase the flags byte is 0xff, which trivially fails
 * the "is sealed" check. A slot is considered valid when it has the
 * SEALED bit set AND its CRC verifies.
 */
#define BLOB_DB_SLOT_F_SEALED     (1u << 0)
#define BLOB_DB_SLOT_F_TOMBSTONE  (1u << 1)

/* Master sector header (24 B). Lives at offset 0 of sectors 0 and 1; the
 * sector with the higher valid generation is authoritative. */
struct __packed blob_db_master_hdr {
	uint8_t  magic[4];        /* 'B','D','M','S' */
	uint32_t generation;      /* monotonic LE */
	uint8_t  state;           /* BLOB_DB_STATE_* */
	uint16_t compacting_bid;  /* meaningful only when state == COMPACTING */
	uint8_t  reserved;
	uint64_t next_id_hint;    /* high-water mark for next id; refined at mount */
	uint32_t hdr_crc32;       /* CRC32-IEEE over the preceding 20 bytes */
};
BUILD_ASSERT(sizeof(struct blob_db_master_hdr) == 24,
	     "blob_db_master_hdr layout drift");

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
