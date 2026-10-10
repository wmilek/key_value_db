/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BLOB_DB_INSPECT_H_
#define APP_LIB_BLOB_DB_INSPECT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_blob_db_inspect blob_db inspection (bucket-log diagnostics)
 * @ingroup lib_blob_db
 * @{
 *
 * @brief Look inside the bucket-log implementation of blob_db: its geometry,
 *        and how each bucket's sector is spent.
 *
 * **Not part of the blob_db contract.** `<app/lib/blob_db.h>` holds what every
 * implementation guarantees and upper layers may use; this header describes
 * one implementation (doc/impl/l1_bucketlog.md) and exists for diagnostics —
 * the `blob_db` shell commands, tests, sizing studies. Containers and
 * interfaces must not include it (P6): another allocator need not have
 * buckets at all.
 *
 * Built with `CONFIG_BLOB_DB_INSPECT`. Works on a mounted store.
 */

/** @brief Geometry and allocator state of the mounted store. */
struct blob_db_inspect_info {
	size_t   partition_size;  /**< bytes blob_db addresses                 */
	size_t   sector_size;     /**< erase block (PEB/LEB) size              */
	size_t   write_align;     /**< write-block size of the substrate       */
	uint16_t n_sectors;       /**< masters + scratch + buckets             */
	uint16_t n_buckets;       /**< id `i` lives in bucket `i % n_buckets`  */
	size_t   bucket_capacity; /**< slot-stream bytes per bucket            */
	size_t   max_payload_len; /**< largest single-slot payload             */
	size_t   slot_overhead;   /**< per-slot bytes besides the payload      */
	uint8_t  format_major;    /**< on-flash format this build writes       */
	uint8_t  format_minor;
	uint8_t  active_master;   /**< 0 or 1                                  */
	uint32_t master_gen;      /**< generation of the active master         */
	uint64_t next_id;         /**< what `blob_db_alloc_id()` returns next  */
	uint64_t next_id_hint;    /**< durable id ceiling on the master        */
	uint64_t seg_owner;       /**< segmented write in flight; 0 = none     */
	bool     wedged;          /**< a torn compaction refuses mutations     */
};

/**
 * @brief Snapshot geometry and allocator state. RAM only, no flash access.
 *
 * @retval 0        success
 * @retval -EINVAL  @p out is NULL
 * @retval -ENODEV  not mounted
 */
int blob_db_inspect_info_get(struct blob_db_inspect_info *out);

/** @brief What a slot means for its id, as compaction would judge it. */
enum blob_db_inspect_slot_state {
	BLOB_DB_INSPECT_LIVE,       /**< newest slot for its id: kept         */
	BLOB_DB_INSPECT_SUPERSEDED, /**< a later slot shadows it: garbage     */
	BLOB_DB_INSPECT_TOMBSTONE,  /**< deletion marker: garbage             */
};

/** @brief One slot of a bucket's log, as `blob_db_inspect_bucket_get()` sees
 *         it. */
struct blob_db_inspect_slot {
	uint32_t offset;          /**< byte offset within the bucket sector    */
	uint32_t size;            /**< on-flash bytes, overhead and padding    */
	uint64_t id;
	uint16_t val_len;         /**< payload bytes stored in this slot       */
	uint8_t  flags;           /**< raw on-flash flag bits                  */
	enum blob_db_inspect_slot_state state;
	bool     segment;         /**< chunk of a large object (internal id)   */
	bool     index;           /**< segment table of a large object         */
};

/**
 * @brief Occupancy of one bucket.
 *
 * Byte figures cover the slot stream only (the bucket header is excluded),
 * and add up: `live_bytes + garbage_bytes + tail_bytes + free_bytes ==
 * capacity`.
 */
struct blob_db_inspect_bucket {
	bool     formatted;       /**< carries a valid bucket header           */
	uint32_t gen;             /**< header generation: 1 at format, +1 per
	                               compaction                             */
	uint32_t capacity;        /**< slot-stream bytes the sector can hold   */
	uint32_t live_bytes;      /**< bytes compaction would keep             */
	uint32_t payload_bytes;   /**< payload part of `live_bytes`            */
	uint32_t garbage_bytes;   /**< superseded slots and tombstones         */
	uint32_t tail_bytes;      /**< programmed but unreadable (torn/rotten) */
	uint32_t free_bytes;      /**< still erased, available to append       */
	uint16_t objects;         /**< live user-visible ids                   */
	uint16_t segments;        /**< live segment slots (large objects)      */
	uint16_t superseded;      /**< superseded slots                        */
	uint16_t tombstones;      /**< tombstone slots                         */
};

/**
 * @brief Called by `blob_db_inspect_bucket_get()` for every readable slot,
 *        in log order. Return non-zero to stop the walk (stats are then
 *        partial).
 */
typedef int (*blob_db_inspect_slot_cb_t)(
	const struct blob_db_inspect_slot *slot, void *user);

/**
 * @brief Measure one bucket, optionally visiting each slot.
 *
 * Reads the whole sector; O(n²) in the bucket's slot count, like
 * `blob_db_count()`. Not for a hot path. Holds the blob_db lock for the
 * whole walk, so other threads' blob_db calls wait for it. @p cb runs with
 * the sector staged in a buffer blob_db shares, so it must not call blob_db.
 *
 * @param bid   bucket index, `0 .. n_buckets - 1`
 * @param out   filled with the bucket's occupancy
 * @param cb    optional per-slot visitor, may be NULL
 * @param user  passed through to @p cb
 *
 * @retval 0        success
 * @retval other    the non-zero value @p cb returned
 * @retval -EINVAL  @p out is NULL or @p bid out of range
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error
 */
int blob_db_inspect_bucket_get(uint16_t bid,
			       struct blob_db_inspect_bucket *out,
			       blob_db_inspect_slot_cb_t cb, void *user);

/** @} */ /* end of lib_blob_db_inspect */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_BLOB_DB_INSPECT_H_ */
