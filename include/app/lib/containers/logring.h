/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_CONTAINERS_LOGRING_H_
#define APP_LIB_CONTAINERS_LOGRING_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_container_logring logring — bounded circular log (L2)
 * @ingroup lib
 * @{
 *
 * @brief A log-specialized container: an append-only ring of records that
 *        auto-evicts its oldest entries so writes never fail for space.
 *
 * @section logring_role Why a dedicated log container
 *
 * The generic `seq` container (doc/layers/l2_containers.md §4.1) grows without
 * bound and is index-addressed. Logging wants the opposite trade: a *bounded*
 * store that keeps the most recent N events and silently drops the oldest —
 * the classic embedded "flight recorder" for boot reasons, fault codes, or the
 * last few lines before a crash. `logring` is that structure.
 *
 * Records are stored inline in a single i-node, oldest → newest. Appending a
 * record that would not fit evicts whole records from the front until it does,
 * so `logring_append()` returns `-ENOSPC` only when a *single* record is larger
 * than the ring's entire capacity — never merely because the ring is full.
 *
 * @section logring_seq Sequence numbers & gap detection
 *
 * Every appended record is stamped with a strictly increasing `uint64_t`
 * sequence number that is **never reused**, not even across eviction, reset, or
 * remount. A consumer that remembers the last sequence number it processed can
 * therefore tell exactly how many records were dropped while it was away: if it
 * last saw seq S and the oldest surviving record is seq S+K (K>1), K-1 records
 * were evicted unseen. The sequence space is the log's one piece of durable
 * "where was I" state — the same single-integer-reachability idea L1 applies to
 * ids (P5).
 *
 * @section logring_crash Crash model
 *
 * Like `rootreg`, `logring` keeps its whole state in one i-node and commits
 * every mutation with exactly **one** `blob_db_update()` — the single
 * linearization point (l2_containers.md §2.2). There is no prepare/cleanup
 * phase and therefore no residue: a crash leaves either the complete old ring
 * or the complete new one, and `open` needs no recovery pass. Sequence numbers
 * stay monotonic across the boundary because `next_seq` is part of the same
 * atomically-written image.
 *
 * @section logring_concurrency Concurrency
 *
 * Single-threaded, inheriting L1's v1 contract: the caller serializes all
 * calls. The library holds no state between calls — every operation re-reads
 * the ring image from flash, so there is nothing to cache or lock (P3).
 *
 * Full design: doc/layers/l2_containers.md §4.5, doc/impl/l2_logring.md.
 */

/**
 * @brief Create a fresh, empty log ring.
 *
 * Allocates an i-node (`blob_db_alloc_id`) and binds it with an empty ring
 * image in a single atomic `update`. The returned id is the ring's root — the
 * one integer from which the whole structure is reachable after reboot. The
 * caller is responsible for persisting it (typically via `rootreg`); an id that
 * is allocated but whose bind was interrupted is simply re-created on the next
 * call and leaks nothing.
 *
 * The first record appended to a brand-new ring is stamped sequence number 1.
 *
 * @param out_root  (out) receives the root id on success
 *
 * @retval 0        created; *out_root filled
 * @retval -EINVAL  out_root is NULL
 * @retval -ENODEV  blob_db not mounted
 * @retval -ENOSPC  partition full / id ceiling could not be persisted
 * @retval -EIO     flash I/O error
 */
int logring_create(uint64_t *out_root);

/**
 * @brief Validate that @p root identifies a log ring.
 *
 * Reads the root i-node and checks the `CLOG` magic and version. No scan is
 * performed and no handle is allocated — the root id *is* the handle.
 *
 * @param root      candidate root id
 *
 * @retval 0        it is a valid log ring
 * @retval -EINVAL  root is 0
 * @retval -ENOENT  no blob with that id
 * @retval -ENOTSUP the blob is not a `logring` (wrong magic/version)
 * @retval -ENODEV  blob_db not mounted
 * @retval -EIO     flash I/O error
 */
int logring_open(uint64_t root);

/**
 * @brief Append a record, evicting the oldest records if necessary.
 *
 * The record is copied to the tail of the ring and stamped with the next
 * sequence number. If it does not fit, whole records are dropped from the head
 * (oldest first) until it does. The updated ring is committed with one atomic
 * `blob_db_update`.
 *
 * A zero-length record is allowed (it still consumes a record slot and a
 * sequence number — useful as a heartbeat / marker).
 *
 * @param root      ring root id
 * @param data      record bytes (may be NULL only if @p len == 0)
 * @param len       record length in bytes
 * @param out_seq   (out, optional) sequence number assigned to this record
 *
 * @retval 0        appended; *out_seq filled if non-NULL
 * @retval -EINVAL  root is 0, or data is NULL with len > 0
 * @retval -ENOTSUP root is not a log ring
 * @retval -ENOSPC  the record alone exceeds the ring's total capacity
 *                  (see @ref logring_capacity); nothing was written
 * @retval -ENODEV  blob_db not mounted
 * @retval -EIO     flash I/O error
 */
int logring_append(uint64_t root, const void *data, size_t len,
		   uint64_t *out_seq);

/**
 * @brief Number of records currently held.
 *
 * @param root      ring root id
 * @param out_count (out) receives the live record count
 *
 * @retval 0        success
 * @retval -EINVAL  root is 0 or out_count is NULL
 * @retval -ENOTSUP root is not a log ring
 * @retval -ENODEV  blob_db not mounted
 * @retval -EIO     flash I/O error
 */
int logring_count(uint64_t root, size_t *out_count);

/**
 * @brief Sequence number of the oldest / newest surviving record.
 *
 * Together with @ref logring_count these let a consumer reason about what it
 * has and has not seen without reading every record.
 *
 * @param root      ring root id
 * @param out_seq   (out) receives the sequence number
 *
 * @retval 0        success; *out_seq filled
 * @retval -ENOENT  the ring is empty
 * @retval -EINVAL  root is 0 or out_seq is NULL
 * @retval -ENOTSUP root is not a log ring
 * @retval -ENODEV  blob_db not mounted
 * @retval -EIO     flash I/O error
 */
int logring_oldest_seq(uint64_t root, uint64_t *out_seq);
int logring_newest_seq(uint64_t root, uint64_t *out_seq);

/**
 * @brief Drop every record but keep the sequence space monotonic.
 *
 * After this call the ring is empty, but the next appended record still gets a
 * sequence number strictly greater than any ever issued — so a reset is
 * indistinguishable, to a sequence-tracking consumer, from having evicted every
 * record. Committed with one atomic `update`.
 *
 * @param root      ring root id
 *
 * @retval 0        cleared
 * @retval -EINVAL  root is 0
 * @retval -ENOTSUP root is not a log ring
 * @retval -ENODEV  blob_db not mounted
 * @retval -EIO     flash I/O error
 */
int logring_reset(uint64_t root);

/**
 * @brief Per-record callback for @ref logring_iterate.
 *
 * @param seq   the record's sequence number
 * @param data  record bytes — transient, valid only for the callback's
 *              duration (points into a stack buffer); copy to keep it
 * @param len   record length
 * @param user  opaque pointer passed through from `iterate`
 *
 * @return 0 to continue; non-zero to stop early (the value is returned by
 *         `logring_iterate`).
 */
typedef int (*logring_iter_cb_t)(uint64_t seq, const void *data, size_t len,
				 void *user);

/**
 * @brief Walk every record oldest → newest.
 *
 * Mutating the ring from inside the callback is undefined behavior
 * (l2_containers.md §2.2) — collect first, mutate after.
 *
 * @param root      ring root id
 * @param cb        per-record callback
 * @param user      opaque pointer forwarded to @p cb
 *
 * @retval 0        full walk completed
 * @retval other    the non-zero value a callback returned to stop early
 * @retval -EINVAL  root is 0 or cb is NULL
 * @retval -ENOTSUP root is not a log ring
 * @retval -ENODEV  blob_db not mounted
 * @retval -EIO     flash I/O error
 */
int logring_iterate(uint64_t root, logring_iter_cb_t cb, void *user);

/**
 * @def LOGRING_CAPACITY
 * @anchor logring_capacity
 * @brief Usable record-data bytes in one ring (payload minus the fixed header).
 *
 * The largest single record is this value minus the per-record header
 * (@ref LOGRING_REC_OVERHEAD). Both are derived from
 * `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN`.
 */
#define LOGRING_HDR_SIZE     20u
#define LOGRING_REC_OVERHEAD 10u
#define LOGRING_CAPACITY     (CONFIG_BLOB_DB_MAX_PAYLOAD_LEN - LOGRING_HDR_SIZE)
#define LOGRING_MAX_RECORD   (LOGRING_CAPACITY - LOGRING_REC_OVERHEAD)

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_CONTAINERS_LOGRING_H_ */
