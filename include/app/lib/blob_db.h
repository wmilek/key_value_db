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
 * @section blob_db_root Root id
 *
 * **`BLOB_DB_ROOT_ID` (= 1) is guaranteed to exist after every successful
 * `blob_db_mount()`.** Callers may unconditionally `get(1)` / `exists(1)` /
 * `update(1, ...)` without a pre-check: on a virgin (or freshly formatted)
 * store, mount allocates id 1 and binds it with an empty payload; on a
 * previously mounted store, mount inherits whatever the last `update(1, ...)`
 * wrote.
 *
 * Consequences:
 * - `blob_db_exists(1)` is always `true` between mount and unmount.
 * - `blob_db_get(1, ...)` never returns `-ENOENT`.
 * - `blob_db_alloc_id()` never returns 1. (The library consumes id 1
 *   internally at format time; which id an allocation actually yields is
 *   not part of the contract — treat ids as opaque.)
 * - `blob_db_delete(1)` is undefined behavior — the root id must not be
 *   deleted.
 *
 * Exactly one component owns id = 1 in a build (the root registry when
 * enabled; otherwise a direct binder). The library reserves the id but does
 * not police what the caller writes there.
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
 * @brief The reserved root id — see @ref blob_db_root.
 *
 * Always live between mount and unmount. Callers should reference this
 * macro rather than the literal 1.
 */
#define BLOB_DB_ROOT_ID  ((uint64_t)1)

/**
 * @brief Open the DB and recover state from flash.
 *
 * Discovers the partition geometry, validates the master sector,
 * recovers from any mid-compaction crash, scans each bucket to recover
 * its write cursor, and determines the next id to assign. If the
 * partition is unformatted, formats it. In either case, on return
 * `BLOB_DB_ROOT_ID` is guaranteed to identify a live blob
 * (see @ref blob_db_root); on a virgin store it is bound to an empty
 * payload.
 *
 * An **unrecognized** store is never destroyed. If a master sector parses but
 * declares a format this build does not understand — a newer version, or
 * another allocator's magic — mount returns `-ENOTSUP` and writes nothing;
 * discarding it is the caller's decision, via `blob_db_format()`. Only a
 * partition whose master sectors read as erased is treated as virgin and
 * formatted automatically. (If both masters are unreadable but *not* erased —
 * bit rot, or a power loss during the first format —
 * `CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT` chooses between reformatting and
 * `-EIO`.)
 *
 * @retval 0       success
 * @retval -EALREADY already mounted
 * @retval -EIO    flash I/O error, partition not found, or both masters
 *                 unreadable with `BLOB_DB_AUTOFORMAT_ON_CORRUPT` disabled
 * @retval -ENOTSUP foreign on-flash format, a sector larger than
 *                 `CONFIG_BLOB_DB_SECTOR_BUF_SIZE`, or a payload cap the
 *                 partition geometry cannot sustain
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
 * Never returns `BLOB_DB_ROOT_ID` (= 1); that id is reserved and consumed at
 * format time (see @ref blob_db_root). The concrete value of the first
 * user-visible allocation is not part of the contract.
 *
 * @return a fresh id (never 0 or `BLOB_DB_ROOT_ID`), or 0 if not mounted
 *         (0 is never a valid id) or the id ceiling could not be persisted.
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
 * With `CONFIG_BLOB_DB_LARGE_PAYLOADS` enabled, a payload longer than
 * `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` is stored transparently as several segments
 * plus an index record, up to `CONFIG_BLOB_DB_MAX_OBJECT_LEN`. The id, its
 * stability, and the atomicity of the update are unchanged — only the number of
 * flash writes grows. Read such a payload back with `blob_db_read()`;
 * `blob_db_get()` still works but needs a buffer for the whole object.
 *
 * @param id        id previously returned by `alloc_id`
 * @param payload   payload bytes (may be NULL only if @p len == 0)
 * @param len       payload length, in 0..`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN`, or
 *                  0..`CONFIG_BLOB_DB_MAX_OBJECT_LEN` when
 *                  `CONFIG_BLOB_DB_LARGE_PAYLOADS` is enabled
 *
 * @retval 0        success
 * @retval -EINVAL  bad arguments, or id was never allocated
 * @retval -ENOSPC  partition is full
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error
 */
int blob_db_update(uint64_t id, const void *payload, size_t len);

/**
 * @brief Report the length of the payload bound to an id.
 *
 * The cheap way to size a blob before reading it: one bucket lookup, no
 * payload copy, and no buffer needed. Works for any live id, whether its
 * payload fits a single slot or (with `CONFIG_BLOB_DB_LARGE_PAYLOADS`) is
 * segmented across several.
 *
 * @param id        id previously returned by `alloc_id` and bound by `update`
 * @param out_size  (out) payload length in bytes
 *
 * @retval 0        success
 * @retval -ENOENT  id was never assigned, or has been deleted
 * @retval -EINVAL  bad arguments
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error
 */
int blob_db_size(uint64_t id, size_t *out_size);

/**
 * @brief Read part of a payload — pread-style, with a caller-chosen window.
 *
 * The access path for payloads too large to hold in RAM: the caller's buffer
 * bounds the transfer rather than the payload's size, so reading a 256 KB
 * object through a 1 KB window costs 1 KB of RAM. A read that starts at or
 * past the end of the payload succeeds with @p out_read set to 0; one that
 * overlaps the end is shortened to what exists (a short read, not an error).
 *
 * Cost is independent of where @p offset falls: a segmented payload is
 * addressed through an index record, not walked, so any offset is two bucket
 * lookups (contract R2). Prefer this over `blob_db_get()` whenever the payload
 * may be large; `get` still works but demands a buffer for the whole thing.
 *
 * @param id        id previously returned by `alloc_id` and bound by `update`
 * @param offset    byte offset to start at
 * @param out       (out) buffer to copy into (may be NULL only if @p len == 0)
 * @param len       maximum bytes to copy
 * @param out_read  (out, optional) bytes actually copied
 *
 * @retval 0        success; @p out_read bytes copied
 * @retval -ENOENT  id was never assigned, or has been deleted
 * @retval -EINVAL  bad arguments
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error, or a segment is missing or mislabelled
 */
int blob_db_read(uint64_t id, size_t offset, void *out, size_t len,
		 size_t *out_read);

/**
 * @brief Overwrite part of a payload in place — pwrite-style.
 *
 * The counterpart to `blob_db_read()`, and the reason a filesystem can sit on
 * this layer: `blob_db_update()` replaces the whole payload, so changing four
 * bytes of a large object would rewrite all of it. This touches only the
 * segments the range covers — on a 256 KB object in 8 KB segments that is 8 KB
 * of flash instead of 264 KB.
 *
 * Writing past the current end **extends** the payload, and any gap between
 * the old end and @p offset reads back as zeros. The update is atomic in the
 * same sense as `update`: on crash the payload is either wholly the old one or
 * wholly the new one.
 *
 * Growing an *inline* payload beyond `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` returns
 * `-EFBIG` rather than converting it to a segmented object. Such a payload
 * still fits comfortably in RAM, so the caller can promote it with
 * `blob_db_get()` + `blob_db_update()`; keeping that out of the library avoids
 * a second whole-object buffer inside it. Segmented objects grow normally, up
 * to `CONFIG_BLOB_DB_MAX_OBJECT_LEN`.
 *
 * Note the whole-object checksum recorded by a full `update` is **not**
 * maintained across a partial write — recomputing it would require reading the
 * entire object, defeating the purpose. Every byte remains covered by its
 * slot's own CRC.
 *
 * @param id        id previously returned by `alloc_id` and bound by `update`
 * @param offset    byte offset to start writing at
 * @param buf       bytes to write (may be NULL only if @p len == 0)
 * @param len       number of bytes to write
 *
 * @retval 0        success
 * @retval -ENOENT  id was never assigned, or has been deleted
 * @retval -EFBIG   would exceed `CONFIG_BLOB_DB_MAX_OBJECT_LEN`, or would grow
 *                  an inline payload past a single slot (see above)
 * @retval -ENOSPC  no room to commit the change
 * @retval -EINVAL  bad arguments
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error
 */
int blob_db_write(uint64_t id, size_t offset, const void *buf, size_t len);

/**
 * @brief Delete the blob with the given id.
 *
 * After this call, `get(id)` returns -ENOENT and the id is never reused.
 *
 * Calling `delete(BLOB_DB_ROOT_ID)` is undefined behavior — the root id
 * must remain live for the lifetime of the mount (see @ref blob_db_root).
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
 * re-initialized, and `BLOB_DB_ROOT_ID` is bound to an empty payload
 * (see @ref blob_db_root). The id space is reset.
 *
 * @retval 0        success
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error
 */
int blob_db_format(void);

/**
 * @brief Logically drop every blob and reset the id space, without erasing
 *        flash.
 *
 * Fast counterpart to `blob_db_format()`: the DB is left in the same visible
 * state as after a fresh format (no live blobs except an empty
 * `BLOB_DB_ROOT_ID`, id space reset), but the underlying
 * sectors are not erased and no compaction runs. Prior on-flash records
 * become unreachable garbage that a later compaction — or the next
 * `blob_db_format()` — will reclaim.
 *
 * Typical use: cheap "wipe" between test scenarios, or a factory-reset path
 * where the caller does not want to pay the multi-second cost of erasing a
 * large partition (an 8 MB QSPI NOR can take tens of seconds to erase; this
 * call is O(one master write)).
 *
 * Postconditions, identical to `blob_db_format()`:
 * - `blob_db_exists(BLOB_DB_ROOT_ID)` is `true`, payload is empty.
 * - `blob_db_count()` is 1.
 * - The id space is reset, exactly as after `blob_db_format()`.
 *
 * Unlike `update`/`delete`, this call is NOT crash-atomic: a crash mid-call
 * can leave some blobs destroyed and others still live. The store stays
 * consistent (ids are never reused, and the root invariant is repaired on
 * the next mount); rerun `erase_all` after the remount to finish the wipe.
 *
 * @retval 0        success
 * @retval -ENOSPC  no room to write a new master record; caller must
 *                  fall back to `blob_db_format()`
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash I/O error
 */
int blob_db_erase_all(void);

#if defined(CONFIG_BLOB_DB_IOSTATS)
/**
 * @brief Flash I/O actually performed, counted at the storage seam.
 *
 * Operation counts and byte counts are both kept, because they answer
 * different questions and can point opposite ways: a change that reads fewer
 * bytes may issue more transactions, and on a serial part each transaction
 * carries a fixed command-and-address cost. Judging such a change on bytes
 * alone flatters it; judging on operations alone condemns it.
 *
 * Counted below both backends, so `flash_area` and UBI are directly
 * comparable for the same workload. Deterministic — the same workload yields
 * the same numbers — which makes these usable as regression guards where
 * wall-clock is not.
 */
struct blob_db_iostats {
	uint32_t reads;          /**< read operations issued          */
	uint32_t writes;         /**< write operations issued         */
	uint32_t erases;         /**< erase operations issued         */
	uint64_t bytes_read;     /**< bytes requested from the store  */
	uint64_t bytes_written;  /**< bytes handed to the store       */
	uint64_t bytes_erased;   /**< bytes covered by erase requests */
};

/** @brief Snapshot the counters. */
void blob_db_iostats_get(struct blob_db_iostats *out);

/** @brief Zero the counters, to measure one operation or phase in isolation. */
void blob_db_iostats_reset(void);
#endif /* CONFIG_BLOB_DB_IOSTATS */

/**
 * @brief Pre-format the next @p n buckets the allocator will use.
 *
 * `blob_db_update()` on an id whose bucket has never been written to must
 * first erase-and-header that sector — on 64 KB QSPI NOR this dominates the
 * cost of a cold first write (roughly one second per bucket at 8 MHz Quad-SPI
 * on mx25r64). This call amortizes that cost off the hot path: it walks the
 * @p n buckets ahead of the current allocation cursor (`bid = id % n_buckets`
 * for ids `next_id … next_id + n − 1`) and formats any that don't already
 * have a valid header.
 *
 * Idempotent: buckets already prepared (or already holding live data) are
 * skipped. Safe to call repeatedly from an idle context to keep a rolling
 * "N-ahead" ready window; the return value tells the caller how many
 * sectors were actually erased this call, so progress is observable.
 *
 * @p n is capped at the total bucket count — beyond that the loop would
 * revisit buckets already covered on this call. The root bucket
 * (`BLOB_DB_ROOT_ID % n_buckets`) is always skipped: it stays formatted for
 * the lifetime of the mount, and `alloc_id` never returns the root id, so
 * it never appears in the "next N" window anyway.
 *
 * Prepared sectors persist across a remount — the bucket header is on
 * flash, not in RAM. Prior on-flash bytes in a prepared sector are
 * discarded (this is a real erase, not a logical drop).
 *
 * @param n  desired number of ready-to-write buckets ahead of the cursor
 *
 * @retval >=0     number of buckets actually formatted this call
 * @retval -ENODEV not mounted
 * @retval -EIO    flash I/O error
 */
int blob_db_prepare(size_t n);

/** @} */ /* end of lib_blob_db */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_BLOB_DB_H_ */
