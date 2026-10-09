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
 * @defgroup lib_container_logring logring — bounded per-entry log (L2)
 * @ingroup lib
 * @{
 *
 * @brief A large, append-only device log: a bounded ring of records, each its
 *        own blob_db i-node, drained forward through a portable cursor.
 *
 * @section logring_model Model
 *
 * A log is a forward-linked chain of per-entry i-nodes rooted at one id. Each
 * entry is `{ next_id, bytes }`; `next_id` points at an id blob_db has already
 * reserved (`blob_db_alloc_id`) but not yet bound, so an append just binds that
 * pre-reserved id — one new blob written, nothing rewritten. The whole log is
 * reachable from the single root id; persist that id however you persist any
 * structure root (typically a `rootreg` entry per log). Many logs coexist,
 * each its own root and handle, sharing only the global id space. Capacity is
 * not fixed — a log may grow toward the whole partition — and per-operation
 * cost is independent of the log's size.
 *
 * @section logring_handle Handle
 *
 * `logring_open()` fills a caller-allocated ::logring_t — a small RAM handle
 * caching the tail so appends are O(1) and the root i-node is written only
 * occasionally (at checkpoints), not on every append. The handle holds no
 * record data, only pointers; it is rebuilt cheaply at `open` and may be
 * discarded at any time (every append is already durable). Treat ::logring_t
 * and ::logring_cursor as opaque — their fields are internal.
 *
 * @section logring_read Reading
 *
 * Reads are forward only (oldest to newest) through an opaque cursor, in the
 * style of the systemd-journal / lseek position APIs: `logring_seek_oldest()`
 * / `logring_seek_newest()` choose an end, `logring_next()` delivers the entry
 * at the cursor and advances. `logring_seek_newest()` reaches the real end in
 * O(1) (read the latest, or follow entries appended from now). There is no
 * backward "last N" scan.
 *
 * @section logring_moniker Monikers
 *
 * To persist or transmit a read position, export it with
 * `logring_cursor_to_moniker()` into a ::logring_moniker — a struct with public
 * fields — and reconstruct it later with `logring_cursor_from_moniker()`.
 * logring defines no wire format and makes no durability or security guarantee
 * for a moniker: the framing, versioning, and any integrity/secrecy protection
 * are the caller's responsibility before it crosses a trust boundary. The
 * moniker's `epoch` is a per-log random tag that lets `logring_next()` reject a
 * moniker left over from a previous incarnation of the log (e.g. after a
 * reformat, which reuses ids) or a random/forged value — reported as `-ESTALE`.
 * This is defense-in-depth; a crafted value carrying the live epoch is stopped
 * only by the caller's own integrity check.
 *
 * @section logring_retention Retention
 *
 * The ring is bounded by a soft `capacity_bytes` budget: when exceeded, the
 * oldest entries are evicted. The budget is a hint, not a hard limit — the ring
 * sits approximately, not exactly, at it — with a hard out-of-space backstop so
 * a full partition never wedges the log. A read position that has since been
 * evicted is reported `-ESTALE`.
 *
 * @section logring_concurrency Concurrency
 *
 * Single-threaded, inheriting blob_db's v1 contract: the caller serializes all
 * calls on a given log.
 *
 * Design of record: `doc/proposals/2026-10-08-logring.md`; contract:
 * `doc/layers/l2_containers.md` §4.5.
 */

/** Create-time configuration for a new log. */
struct logring_cfg {
	/** Soft retention bound in bytes — a hint, not a hard limit (see the
	 *  Retention section). Must be non-zero. */
	uint32_t capacity_bytes;
};

/** Snapshot of a log's state. `oldest_id`/`newest_id` are exact; `count` and
 *  `bytes` are approximate hints. */
struct logring_stats {
	uint64_t oldest_id;   /**< id of the oldest surviving entry, 0 if empty */
	uint64_t newest_id;   /**< id of the newest entry, 0 if empty */
	uint32_t count;       /**< approximate live entry count */
	uint32_t bytes;       /**< approximate live record-byte total */
};

/**
 * @brief Portable read position. Public fields — the caller serializes it.
 *
 * Exported from a cursor by `logring_cursor_to_moniker()` and restored by
 * `logring_cursor_from_moniker()`. logring imposes no wire format; see the
 * Monikers section for the division of responsibility.
 */
struct logring_moniker {
	uint32_t epoch;   /**< log-incarnation tag; rejects a stale/foreign moniker */
	uint64_t id;      /**< entry position within the log */
};

/**
 * @brief Opaque RAM handle for an open log. Caller-allocated; treat as opaque.
 *
 * Holds the root id plus a cached tail and soft accounting. Filled by
 * `logring_open()`; carries no record data and needs no flush.
 */
typedef struct logring {
	/* --- internal; do not touch --- */
	uint64_t root;           /* root i-node id */
	uint64_t tail_id;        /* cached newest entry id (0 if empty) */
	uint64_t next_free;      /* reserved id the next append will bind */
	uint64_t head_id;        /* cached oldest entry id, hint (0 if empty) */
	uint32_t epoch;          /* per-log incarnation tag */
	uint32_t capacity_bytes; /* retention bound from cfg */
	uint32_t live_bytes;     /* soft byte accounting, hint */
	uint32_t count;          /* soft live-entry count, hint */
	uint32_t since_ckpt;     /* appends since the last root checkpoint */
} logring_t;

/**
 * @brief Opaque, caller-allocated read cursor. Treat as opaque.
 *
 * Drives `logring_seek_oldest()` / `logring_seek_newest()` / `logring_next()`.
 * Its in-memory layout is internal; the portable form is ::logring_moniker.
 */
typedef struct logring_cursor {
	uint64_t pos;    /* internal: id of the next entry to deliver, 0 = oldest */
	uint32_t epoch;  /* internal: incarnation this position belongs to */
} logring_cursor;

/**
 * @brief Create a new, empty log and return its root id.
 *
 * Allocates the root i-node, draws a random epoch, and binds an empty log.
 * Persist the returned id (e.g. in a `rootreg` entry); call `logring_open()`
 * to operate on the log.
 *
 * @param cfg       configuration (soft `capacity_bytes`, non-zero)
 * @param out_root  (out) receives the new log's root id
 *
 * @retval 0        created
 * @retval -EINVAL  @p cfg or @p out_root is NULL, or `capacity_bytes` is 0
 * @retval -ENOSPC  partition full / id ceiling could not be persisted
 * @retval -ENODEV  blob_db not mounted
 * @retval -EIO     flash I/O error
 */
int logring_create(const struct logring_cfg *cfg, uint64_t *out_root);

/**
 * @brief Open an existing log into a caller-allocated handle.
 *
 * Validates the root's type, finishes any interrupted eviction, and rebuilds
 * the cached tail by a short bounded walk from the last checkpoint.
 *
 * @param root  root id previously returned by `logring_create()`
 * @param h     (out) handle to fill
 *
 * @retval 0        opened
 * @retval -EINVAL  @p root is 0 or @p h is NULL
 * @retval -ENOENT  no blob with that id
 * @retval -ENOTSUP the blob is not a log (wrong magic/version)
 * @retval -ENODEV  blob_db not mounted
 * @retval -EIO     flash I/O error
 */
int logring_open(uint64_t root, logring_t *h);

/**
 * @brief Delete every entry and the root. The handle is invalid afterwards.
 *
 * @param h  open log handle
 * @retval 0        destroyed
 * @retval -EINVAL  @p h is NULL
 * @retval -ENODEV  blob_db not mounted
 * @retval -EIO     flash I/O error
 */
int logring_destroy(logring_t *h);

/**
 * @brief Append one record; durable on return.
 *
 * Copies @p rec into a new entry, evicting the oldest entries if the soft
 * budget (or the partition) requires. O(1) on the warm path.
 *
 * @param h       open log handle
 * @param rec     record bytes (may be NULL only if @p len == 0)
 * @param len     record length; must fit one blob_db payload
 * @param out_id  (out, optional) id assigned to the new entry
 *
 * @retval 0         appended; *out_id filled if non-NULL
 * @retval -EINVAL   @p h is NULL, or @p rec is NULL with @p len > 0
 * @retval -EMSGSIZE @p len exceeds one entry payload (no record spanning)
 * @retval -ENOSPC   partition full and nothing left to evict
 * @retval -ENODEV   blob_db not mounted
 * @retval -EIO      flash I/O error
 */
int logring_append(logring_t *h, const void *rec, size_t len, uint64_t *out_id);

/**
 * @brief Position a cursor at the oldest surviving entry.
 *
 * @param h  open log handle
 * @param c  (out) cursor to initialize
 * @retval 0 positioned · -EINVAL @p h or @p c is NULL
 */
int logring_seek_oldest(logring_t *h, logring_cursor *c);

/**
 * @brief Position a cursor at the newest entry (the real end). O(1).
 *
 * The next `logring_next()` delivers the newest entry; after that it reports
 * `-ENOENT` until a new entry is appended (follow-from-end). On an empty log
 * the next `logring_next()` reports `-ENOENT`.
 *
 * @param h  open log handle
 * @param c  (out) cursor to position
 * @retval 0 positioned · -EINVAL @p h or @p c is NULL
 */
int logring_seek_newest(logring_t *h, logring_cursor *c);

/**
 * @brief Deliver the entry at the cursor and advance it (forward only).
 *
 * @param h        open log handle
 * @param c        read cursor
 * @param out      (out) buffer for the record bytes
 * @param out_sz   capacity of @p out
 * @param out_len  (out, optional) actual record length delivered
 * @param out_id   (out, optional) the delivered entry's id
 *
 * @retval 0         a record was delivered
 * @retval -ENOENT   caught up — no entry at/after the cursor yet
 * @retval -ESTALE   the position was evicted, or the cursor belongs to another
 *                   incarnation of the log; the cursor is reset to the oldest
 *                   surviving entry (retry to resume from there)
 * @retval -EMSGSIZE @p out_sz is smaller than the record; cursor not advanced
 * @retval -EINVAL   @p h or @p c is NULL, or @p out is NULL with @p out_sz > 0
 * @retval -ENODEV   blob_db not mounted
 * @retval -EIO      flash I/O error
 */
int logring_next(logring_t *h, logring_cursor *c, void *out, size_t out_sz,
		 size_t *out_len, uint64_t *out_id);

/**
 * @brief Export a cursor's position as a portable moniker.
 *
 * The caller owns the moniker's serialization, versioning, and integrity
 * protection (see the Monikers section).
 *
 * @param c    cursor to export
 * @param out  (out) moniker
 */
void logring_cursor_to_moniker(const logring_cursor *c,
			       struct logring_moniker *out);

/**
 * @brief Rebuild a cursor from a moniker previously exported.
 *
 * Does not validate against any log — a stale or foreign moniker is detected
 * by the following `logring_next()` (`-ESTALE`).
 *
 * @param c  (out) cursor to fill
 * @param m  moniker
 * @retval 0 ok · -EINVAL @p c or @p m is NULL
 */
int logring_cursor_from_moniker(logring_cursor *c,
				const struct logring_moniker *m);

/**
 * @brief Read a log's current statistics.
 *
 * @param h    open log handle
 * @param out  (out) stats
 * @retval 0 filled · -EINVAL @p h or @p out is NULL · -ENODEV not mounted · -EIO flash error
 */
int logring_stats(logring_t *h, struct logring_stats *out);

/**
 * @brief Drop every entry; the log becomes empty.
 *
 * @param h  open log handle
 * @retval 0 cleared · -EINVAL @p h is NULL · -ENODEV not mounted · -EIO flash error
 */
int logring_reset(logring_t *h);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_CONTAINERS_LOGRING_H_ */
