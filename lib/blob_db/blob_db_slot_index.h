/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Optional RAM index for blob_db (CONFIG_BLOB_DB_SLOT_INDEX): blob id -> byte
 * offset of that id's newest slot, plus each bucket's append cursor.
 *
 * Private to lib/blob_db. Without it, a point lookup walks every slot header
 * in the id's bucket, one 12-byte flash read per slot, because the newest slot
 * wins and the log has no back-pointers. On the DK that walk is 73-95 % of an
 * access check in app_cbor_persondb (doc/impl/l1_bucketlog.md §5.4a).
 *
 * The index is a hint, never the truth. A hit is checked against the slot it
 * names (sealed, same id, CRC) before it is used, and anything the index cannot
 * vouch for -- a miss, a full table, a bucket it has lost track of -- takes the
 * walk exactly as before. What it must never do is name an *older* slot of an
 * id, which no check can detect; that is what the rules below guarantee.
 *
 * Invariants, per bucket:
 *  - The cursor is either unknown (0) or equal to the bucket's true end of log.
 *  - While the cursor is unknown, the table holds no entry for the bucket.
 *  - While it is known, every entry for the bucket names the newest slot of
 *    its id: every append goes through blob_db_sidx_note(), which advances the
 *    cursor and updates the entry, and an append anywhere but at the cursor
 *    forgets the bucket instead.
 *  - Anything that rewrites a bucket other than by appending (format,
 *    compaction, recovery, erase_all) forgets it, or rebuilds it from the image
 *    it wrote.
 *
 * Single-threaded like the rest of blob_db (contract §5).
 */

#ifndef BLOB_DB_SLOT_INDEX_H_
#define BLOB_DB_SLOT_INDEX_H_

#include <stdbool.h>
#include <stdint.h>

#if defined(CONFIG_BLOB_DB_SLOT_INDEX)

/** Forget everything; index buckets [0, n_buckets) from now on, or none if
 *  n_buckets exceeds CONFIG_BLOB_DB_SLOT_INDEX_MAX_BUCKETS. */
void blob_db_sidx_reset(uint16_t n_buckets);

/** Forget everything and index nothing until the next reset. */
void blob_db_sidx_disable(void);

/** Forget bucket @p bid: its entries and its cursor. */
void blob_db_sidx_forget(uint16_t bid);

/** Bucket @p bid now holds a header and no slots: cursor = @p data_off. */
void blob_db_sidx_begin(uint16_t bid, uint32_t data_off);

/** A slot for @p id of @p size bytes now sits at @p off in bucket @p bid,
 *  appended at the end of the log. A tombstone removes the id's entry. */
void blob_db_sidx_note(uint16_t bid, uint64_t id, uint32_t off,
		       uint32_t size, bool tombstone);

/** A full walk of bucket @p bid found the log ending at @p cursor and, if
 *  @p off is non-negative, @p id's newest slot at @p off. */
void blob_db_sidx_learn(uint16_t bid, uint64_t id, int64_t off,
			uint32_t cursor);

/** @p id's newest slot offset and its bucket's cursor, if both are known. */
bool blob_db_sidx_lookup(uint16_t bid, uint64_t id, uint32_t *off,
			 uint32_t *cursor);

/** Bucket @p bid's cursor, if known. Known implies a valid bucket header. */
bool blob_db_sidx_cursor(uint16_t bid, uint32_t *cursor);

/** Entries in use and table capacity. */
void blob_db_sidx_usage(uint32_t *used, uint32_t *capacity);

#else

static inline void blob_db_sidx_reset(uint16_t n_buckets) { (void)n_buckets; }
static inline void blob_db_sidx_disable(void) { }
static inline void blob_db_sidx_forget(uint16_t bid) { (void)bid; }
static inline void blob_db_sidx_begin(uint16_t bid, uint32_t data_off)
{
	(void)bid;
	(void)data_off;
}
static inline void blob_db_sidx_note(uint16_t bid, uint64_t id, uint32_t off,
				     uint32_t size, bool tombstone)
{
	(void)bid;
	(void)id;
	(void)off;
	(void)size;
	(void)tombstone;
}
static inline void blob_db_sidx_learn(uint16_t bid, uint64_t id, int64_t off,
				      uint32_t cursor)
{
	(void)bid;
	(void)id;
	(void)off;
	(void)cursor;
}
static inline bool blob_db_sidx_lookup(uint16_t bid, uint64_t id,
				       uint32_t *off, uint32_t *cursor)
{
	(void)bid;
	(void)id;
	(void)off;
	(void)cursor;
	return false;
}
static inline bool blob_db_sidx_cursor(uint16_t bid, uint32_t *cursor)
{
	(void)bid;
	(void)cursor;
	return false;
}
static inline void blob_db_sidx_usage(uint32_t *used, uint32_t *capacity)
{
	*used = 0;
	*capacity = 0;
}

#endif /* CONFIG_BLOB_DB_SLOT_INDEX */

#endif /* BLOB_DB_SLOT_INDEX_H_ */
