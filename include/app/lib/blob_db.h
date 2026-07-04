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
 * promises only one thing: once content is bound to an id, you can fetch it
 * by that id. The full contract is:
 *
 * - **Id allocation.** `blob_db_alloc_id()` returns a u64 id never returned
 *   before in the lifetime of this DB — across all crashes — and strictly
 *   greater than every previously returned id. Allocation is a RAM
 *   operation; the id is durable (a later mount never re-issues it) even if
 *   it is never bound.
 * - **Id lifecycle.** *allocated* (fresh from `alloc_id`, nothing on flash)
 *   → *bound* (first `update` writes content) → rebound (further `update`s)
 *   → *dead* (`delete`). After `delete` the id ceases to exist; `update` on
 *   a dead id — or on one never allocated — is undefined behavior.
 * - **Id stability.** A bound id refers to the same logical blob until
 *   `delete`d. `update` keeps the id; only the payload changes.
 * - **Compaction transparency.** Internal compaction may move slots within
 *   the on-flash store and may erase tombstones, but ids never change. A
 *   blob you can `get` today is reachable by the same id after any number
 *   of compactions.
 * - **No reuse.** A dead — or merely allocated — id is never returned by
 *   `alloc_id` again. The next allocated id is strictly greater than every
 *   id ever seen.
 * - **Atomicity.** Each `update` / `delete` is atomic with respect to
 *   crash: either it takes effect fully or it doesn't (on next mount).
 *   Partial writes are detected and discarded.
 *
 * @section blob_db_root Root convention
 *
 * The first `alloc_id()` after a fresh format returns **id = 1**; binding it
 * with `update(1, ...)` gives one remembered integer that re-opens
 * everything after reboot. Exactly one component owns id = 1 in a build
 * (the root registry when enabled; otherwise a direct binder). No library
 * API enforces this — it's a convention.
 *
 * @section blob_db_concurrency Concurrency
 *
 * v1 is **single-threaded**: the caller must serialize all calls. The
 * library does no locking internally.
 *
 * See `doc/layers/l1_blob_db.md` for the on-flash format, algorithms, and crash
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
 * @brief Allocate a fresh, durable id (RAM operation; nothing on flash yet).
 *
 * The returned id is never returned before in this DB's lifetime — across
 * crashes — and strictly greater than every id previously returned. Bind it
 * with `blob_db_update()`. An allocated-but-unbound id is durable: a later
 * mount never re-issues it, which is the watermark client crash-recovery is
 * built on.
 *
 * @return a fresh id (>= 1), or 0 if not mounted (0 is never a valid id) or
 *         the id ceiling could not be persisted.
 */
uint64_t blob_db_alloc_id(void);

/**
 * @brief Fetch the payload for an id.
 *
 * @param id        id previously returned by `alloc_id` and bound by `update`
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
 * @brief Bind (first write) or rebind (rewrite) the payload of an id.
 *
 * First `update` after `alloc_id` binds the blob; later `update`s replace
 * the payload, keeping the id. The previous payload becomes garbage
 * (reclaimed on next compaction).
 *
 * `update` on a dead (deleted) id, or on an id never returned by `alloc_id`,
 * is undefined behavior (decision D3). A best-effort bound check rejects an
 * id of 0 or one not yet allocated with -EINVAL, but release builds owe no
 * such check.
 *
 * @param id        id previously returned by `alloc_id`
 * @param payload   payload bytes (may be NULL only if @p len == 0)
 * @param len       payload length, in 0..CONFIG_BLOB_DB_MAX_PAYLOAD_LEN
 *
 * @retval 0        success
 * @retval -EINVAL  bad arguments, or id was never allocated
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
 * @param id        id previously returned by `alloc_id`
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
 * path. See `doc/layers/l1_blob_db.md` §11 for cost details.
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
