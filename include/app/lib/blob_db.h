/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BLOB_DB_H_
#define APP_LIB_BLOB_DB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_blob_db blob_db — stable-id blob store
 * @ingroup lib
 * @{
 *
 * @brief A persistent store of opaque, variable-length payloads addressed
 *        by u64 ids that the library assigns.
 *
 * @section blob_db_contract Stability contract
 *
 * `blob_db` does not know about strings, keys, schemas, or queries. It
 * promises only one thing: once you have an id, you can fetch the payload
 * that was stored under it. The full contract is:
 *
 * - **Id assignment.** `blob_db_put()` returns a u64 id that is currently
 *   unused, never previously assigned in the lifetime of this DB, and
 *   greater than or equal to every id returned by any prior `put`.
 * - **Id stability.** Once an id is returned, it refers to the same logical
 *   blob until that id is `delete`d. `update` keeps the id; only the
 *   payload changes.
 * - **Compaction transparency.** Internal compaction may move slots within
 *   the on-flash store and may erase tombstones, but ids never change. A
 *   blob you can `get` today is reachable by the same id after any number
 *   of compactions.
 * - **No reuse.** After `delete(id)`, that id is never assigned to another
 *   blob. The next assigned id is strictly greater than every id ever
 *   seen.
 * - **Atomicity.** Each `put` / `update` / `delete` is atomic with respect
 *   to crash: either it takes effect fully or it doesn't (on next mount).
 *   Partial writes are detected and discarded.
 *
 * @section blob_db_root Root convention
 *
 * The first successful `put` after a fresh format returns **id = 1**.
 * Clients may use id = 1 as their persistent root pointer: because
 * `update` is id-stable, id = 1 always names "the latest version of the
 * client's root blob". No library API is needed for this — it's a
 * convention.
 *
 * @section blob_db_concurrency Concurrency
 *
 * v1 is **single-threaded**: the caller must serialize all calls. The
 * library does no locking internally.
 *
 * See `doc/design.md` for the on-flash format, algorithms, and crash
 * recovery details.
 */

/**
 * @brief Open the DB and recover state from flash.
 *
 * Discovers the partition geometry, validates the master sector,
 * recovers from any mid-compaction crash, scans each bucket to recover
 * its write cursor, and determines the next id to assign. If the
 * partition is unformatted, formats it.
 *
 * @retval 0       success
 * @retval -EALREADY already mounted
 * @retval -EIO    flash I/O error or partition not found
 * @retval -ENODEV partition_label not present in the device tree
 */
int blob_db_mount(void);

/**
 * @brief Release the DB. Idempotent.
 *
 * @retval 0 always
 */
int blob_db_unmount(void);

/**
 * @brief Store a new blob and return its assigned id.
 *
 * @param payload   bytes to store (may be NULL only if @p len == 0)
 * @param len       payload length, in 0..CONFIG_BLOB_DB_MAX_PAYLOAD_LEN
 * @param out_id    (out) assigned id, valid for use with the other ops
 *
 * @retval 0        success
 * @retval -EINVAL  bad arguments (NULL out_id, len out of range, etc.)
 * @retval -ENOSPC  partition is full (compaction could not free space)
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error
 */
int blob_db_put(const void *payload, size_t len, uint64_t *out_id);

/**
 * @brief Fetch the payload for an id.
 *
 * @param id        id previously returned by `put`
 * @param out       (out) buffer to copy payload into
 * @param out_sz    capacity of @p out, in bytes
 * @param out_len   (out, optional) actual payload length written
 *
 * @retval 0        success; @p out_len bytes copied to @p out
 * @retval -ENOENT  id was never assigned, or has been deleted
 * @retval -ENOMEM  @p out_sz is smaller than the stored payload
 * @retval -EINVAL  bad arguments
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error
 */
int blob_db_get(uint64_t id, void *out, size_t out_sz, size_t *out_len);

/**
 * @brief Replace the payload for an existing id. The id is preserved.
 *
 * The previous payload becomes garbage (reclaimed on next compaction).
 *
 * @param id        id previously returned by `put`
 * @param payload   new payload
 * @param len       new payload length
 *
 * @retval 0        success
 * @retval -ENOENT  id was never assigned, or has been deleted
 * @retval -EINVAL  bad arguments
 * @retval -ENOSPC  partition is full
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error
 */
int blob_db_update(uint64_t id, const void *payload, size_t len);

/**
 * @brief Delete the blob with the given id.
 *
 * After this call, `get(id)` returns -ENOENT and the id is never reused.
 *
 * @param id        id previously returned by `put`
 *
 * @retval 0        success
 * @retval -ENOENT  id was never assigned, or has already been deleted
 * @retval -ENOSPC  partition is full (could not write the tombstone)
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error
 */
int blob_db_delete(uint64_t id);

/**
 * @brief Predicate: does this id currently identify a live blob?
 *
 * @param id        id to test
 * @return          true iff `get(id)` would succeed
 */
bool blob_db_exists(uint64_t id);

/**
 * @brief Count the number of live blobs in the store.
 *
 * O(n²) in entry count — intended for diagnostics / fsck, not a hot
 * path. See `doc/design.md` §11 for cost details.
 *
 * @return number of live (non-tombstoned) blobs
 */
size_t blob_db_count(void);

/**
 * @brief Callback for `blob_db_iterate`. The payload pointer is only valid
 *        for the duration of the callback (it points into a stack buffer).
 *
 * @param id        the blob's stable id
 * @param payload   pointer to payload bytes (transient — copy if you need
 *                  it past the callback)
 * @param len       payload length
 * @param user      opaque user pointer passed through from `iterate`
 *
 * @return 0 to continue iteration; non-zero to stop early. The non-zero
 *         value is returned by `blob_db_iterate`.
 */
typedef int (*blob_db_iter_cb_t)(uint64_t id,
				 const void *payload, size_t len,
				 void *user);

/**
 * @brief Walk every live blob in the store, calling @p cb for each.
 *
 * Order is bucket-by-bucket (not sorted by id). Callbacks must not
 * mutate the DB — undefined behavior in v1.
 *
 * O(n²) in entry count (see `blob_db_count`).
 *
 * @retval 0        full walk completed
 * @retval other    value the callback returned to abort early
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error
 */
int blob_db_iterate(blob_db_iter_cb_t cb, void *user);

/**
 * @brief Erase the entire DB and reset to a fresh state.
 *
 * All blobs are destroyed, all sectors are erased, masters are
 * re-initialized, and the next id will be 1 again.
 *
 * @retval 0        success
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error
 */
int blob_db_format(void);

/** @} */ /* end of lib_blob_db */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_BLOB_DB_H_ */
