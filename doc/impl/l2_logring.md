# Implementation design — `logring` per-entry log container (L2)

Status: v1 · **Implemented** · records the on-flash format and algorithms of
`lib/containers/logring/logring.c`. Normative contract:
`doc/layers/l2_containers.md` §4.5. Accepted design & rationale (amplification
analysis, cost tables, crash analysis):
`doc/proposals/2026-10-08-logring.md`. Upper layers depend only on the public
API in `include/app/lib/containers/logring.h`, never on the layout below (P6).

Target board for bring-up: `native_sim`.

---

## 1. Shape

A log is a **forward-linked chain of per-entry i-nodes** rooted at one id.
Every record is its own `blob_db` blob; the links use `blob_db`'s
reserve-then-bind so an append writes one new blob and rewrites nothing. A
small RAM **handle** caches the tail, so warm appends are O(1) and the root
i-node is written only at **checkpoints**.

## 2. On-flash format v1 (frozen)

```c
struct clog_root {               /* 56 B — the root i-node's payload */
    uint8_t  magic[4];           /* 'C','L','O','G'                         */
    uint8_t  version;            /* 1                                       */
    uint8_t  flags; uint16_t rsvd;
    uint32_t epoch;              /* per-log random tag (non-zero)           */
    uint32_t capacity_bytes;     /* soft retention bound                    */
    uint32_t live_bytes;         /* live record bytes as of this write      */
    uint32_t count;              /* live entry count as of this write       */
    uint64_t waypoint_id;        /* a live entry near the end; open walks it */
    uint64_t head_id;            /* oldest live entry (0 = empty)           */
    uint64_t evict_from, evict_to; /* inline eviction intent (0 = none)     */
};

struct clog_entry_hdr {          /* 10 B, then `len` opaque record bytes */
    uint64_t next_id;            /* reserved id of the next entry (unbound = end) */
    uint16_t len;
};
```

`BUILD_ASSERT`s pin both sizes and keep the root within
`BLOB_DB_MAX_PAYLOAD_LEN`. A record is capped at `BLOB_DB_MAX_PAYLOAD_LEN − 10`
(`-EMSGSIZE` above; no spanning).

**Pre-reservation invariant.** `tail.next_id` is always an id `alloc_id()`
returned but not yet bound; the chain ends at the first `next_id` that
`blob_db_get` reports `-ENOENT` because it is unbound. ids strictly increase
along the chain (allocations are monotonic), which bounds every walk and
detects a cycle (`-EIO`).

## 3. RAM handle

`{ root, epoch, capacity_bytes, tail_id, next_free, head_id, live_bytes,
count, since_ckpt }` — pointers and counters only, no record data. Filled by
`open` (§6); discardable at any time since every append is already durable.

## 4. Append (warm O(1))

`R = next_free` (the predecessor already points at it); `R2 = alloc_id()`;
`blob_db_update(R, {next_id=R2, bytes})` — **binding `R` is the commit**.
Update the handle (`tail_id=R`, `next_free=R2`, `live_bytes+=len`, `count++`,
`since_ckpt++`). The first append (`was_empty`) sets `head_id=R` and
checkpoints; otherwise a checkpoint runs when `since_ckpt ≥ K`
(`CONFIG_BLOB_CONTAINER_LOGRING_CHECKPOINT`). A checkpoint failure is logged,
not fatal — the entry is committed and `open` rebuilds from the last good
checkpoint.

If the bind returns `-ENOSPC`, the hard backstop evicts a run and retries,
looping until the record fits or the log cannot shrink.

## 5. Checkpoint + batched eviction (the only root write)

`checkpoint(force)`: if over the soft budget (or `force`), walk from `head_id`
collecting a run of ≤ K oldest entries whose bytes cover the overshoot (never
the tail); set `head_id` to the first kept entry. Then **one** `blob_db_update`
of the root commits the new `head`, the refreshed `waypoint = tail`, the
updated totals, and the inline eviction intent `{evict_from, evict_to}`. After
the commit, the run is deleted **suffix-first** (`evict_from` last) so a crash
leaves the undeleted part a prefix walkable from `evict_from`. The intent is
cleared by the next checkpoint write; deletes are best-effort (a failed delete
leaves reclaimable garbage, no correctness impact). Evicted ids are found by
following the chain, never by id range (the id space is shared across logs).

## 6. Open / recovery

`open` loads and type-checks the root, finishes any interrupted eviction
(idempotent suffix-first re-delete of `[evict_from, evict_to)`), then walks
from `waypoint_id` to the first unbound id to recover the exact `tail_id` /
`next_free` — and, from the same walk, the exact `count` / `live_bytes` (the
entries after the waypoint are precisely those appended since the last
checkpoint). `head_id` comes from the root; a pre-first-checkpoint crash
(`head_id == 0` with a bound entry) takes the waypoint as the oldest.

## 7. Reading

An opaque cursor holds `{ pos, epoch }` (`pos == 0` means "oldest").
`logring_next` rejects a cursor whose `epoch` differs from the log's
(`-ESTALE`, G2/§8), resolves `pos` (or `head_id`), reads the entry, and
advances `pos` to its `next_id`. A read that misses:

- `-ENOENT` on an id `< next_free` → the position was evicted → `-ESTALE`
  (cursor reset to oldest);
- `-ENOENT` on the unbound frontier → `-ENOENT` (caught up).

`seek_newest` sets `pos = tail_id` (O(1) from the handle). The portable
`logring_moniker { epoch, id }` is `pos`+`epoch`; the caller owns its framing,
versioning, and integrity protection.

## 8. Epoch

A per-log random `epoch` (`sys_rand32_get()`, forced non-zero) set at `create`
and stored in the root tags the incarnation. `sys_rand32_get()` is the
**non-crypto** random subsystem, not PSA, so the module `select`s
`ENTROPY_GENERATOR` to pull its own randomness regardless of the blob_db
backend (it does not depend on the UBI backend's PSA stack). A moniker carries
the epoch; `next` rejects a mismatch, so a moniker from a destroyed+recreated
log (ids reused) or a random value is `-ESTALE` rather than a misread. This is
defense-in-depth, not a security control — a crafted value with the live epoch
is stopped only by
the caller's own integrity check.

## 9. Invariant checklist (`l2_containers.md` §5)

1. **Single-integer reachability** — the root id recovers everything; the
   handle is a rebuildable cache. ✓
2. **Typed root** — `'CLOG'` magic/version; wrong-type `open` → `-ENOTSUP`. ✓
3. **One commit point per mutation** — append = bind entry; eviction = the
   root write. ✓
4. **O(1) steady-state RAM** — handle is a few integers; no per-entry RAM. ✓
5. **Bounded stack** — one payload buffer + a ≤ K-id eviction run buffer;
   walks are iterative and ≤ K between checkpoints. ✓

## 10. Open items

- **Per-step crash injection.** `blob_db_test_cut` cuts within one segmented
  `blob_db` op, not between logring's op sequence. The unit suite covers
  recovery via unmount/mount (handle rebuild, interrupted-eviction re-run);
  a logring-level cut hook to assert each §4/§5 step's residue is a follow-up.
- **Waypoint far behind.** If the live set shrinks below one checkpoint
  interval the waypoint could be evicted; `open` then falls back to a walk from
  `head_id`. Rare; handled, not optimized.
