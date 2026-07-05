# Implementation design — `logring` bounded circular log (L2)

Status: v1 · **Implemented.** This document records the on-flash format and
algorithms of `lib/containers/logring/logring.c`. The normative contract is
`doc/layers/l2_containers.md` §4.5; the container invariants there (§5) and the
mutation protocol (§2.2) govern this design. Upper layers depend only on the
public API in `include/app/lib/containers/logring.h`, never on the layout
below (P6).

Target board for bring-up: `native_sim`.

---

## 1. Approach

`logring` is the simplest possible member of the container family: the whole
structure is a **single i-node**, so it needs none of the reference-graph
machinery (intent blob, prepare/cleanup, recovery) that the multi-node
containers require. It is the `rootreg` pattern (`doc/impl` sibling
`l1_root_registry.md`) applied to an append-only, self-evicting log instead of
a key→root map.

The root i-node's payload holds a fixed header followed by a packed run of
variable-length records, oldest first. Every operation reads the whole payload
into a stack image, mutates the image, and — for mutations — writes it back
with one `blob_db_update`. Nothing is cached between calls (P3): the flash blob
is the single source of truth.

## 2. On-flash format v1 (frozen)

All integers little-endian; both structs `__packed`.

```
struct clog_hdr {              /* 20 bytes — LOGRING_HDR_SIZE */
    uint8_t  magic[4];         /* 'C','L','O','G'                      */
    uint8_t  version;          /* = 1                                  */
    uint8_t  rsvd;
    uint16_t count;            /* number of live records               */
    uint16_t used;             /* bytes of the data region in use      */
    uint16_t rsvd2;
    uint64_t next_seq;         /* sequence number the next append gets  */
};

struct clog_rec {              /* 10 bytes — LOGRING_REC_OVERHEAD      */
    uint64_t seq;              /* strictly increasing, never reused     */
    uint16_t len;              /* payload length; `len` bytes follow    */
};
```

`BUILD_ASSERT`s pin both sizes and require
`BLOB_DB_MAX_PAYLOAD_LEN ≥ 20 + 10 + 1` (room for at least one 1-byte record).

Derived limits (public macros):

```
LOGRING_CAPACITY   = BLOB_DB_MAX_PAYLOAD_LEN − 20   (usable data bytes)
LOGRING_MAX_RECORD = LOGRING_CAPACITY − 10          (largest single record)
```

Records tile the data region `[0, used)` exactly: `count` well-formed
`clog_rec` headers each followed by `len` payload bytes, back to back. `clog_load`
re-derives this partition and returns `-EIO` if it does not hold — turning
silent corruption into an honest error rather than a bad read.

## 3. Operations

| Op | Reads | Writes | Notes |
|---|---|---|---|
| `create`  | 0 | 1 | `alloc_id` + bind empty ring (`next_seq = 1`) |
| `open`    | 1 | 0 | validate magic/version only |
| `append`  | 1 | 1 | evict-then-append, single commit |
| `count` / `oldest_seq` / `newest_seq` | 1 | 0 | read header (+ front record) |
| `reset`   | 1 | 1 | `count = used = 0`, keep `next_seq` |
| `iterate` | 1 | 0 | walk the inline run oldest→newest |

**append** (the only non-trivial path):

1. Reject `len > LOGRING_MAX_RECORD` with `-ENOSPC` up front — no eviction
   could ever make room.
2. Load + validate the image.
3. While `used + (10 + len) > LOGRING_CAPACITY` and `count > 0`: drop the
   head record (`memmove` the tail down, decrement `used`/`count`). Bounded:
   `10 + len ≤ CAPACITY`, so the loop ends at or before `count == 0`.
4. Write the new `clog_rec{ seq = next_seq, len }` + payload at `used`;
   bump `used`, `count`, `next_seq`.
5. `blob_db_update(root, …)` — the single commit point.

Because eviction only ever removes from the head, the tail is always the
most-recently appended record, so `newest_seq == next_seq − 1` whenever
`count > 0` (used by `logring_newest_seq` without a walk).

## 4. Crash model (satisfies §2.2 / invariant 3)

Each mutation is exactly one `blob_db_update`, which L1 guarantees is atomic:
after any crash the root holds the complete old ring or the complete new ring,
never a splice. There is no unreferenced-i-node window (nothing is allocated or
deleted mid-mutation), so no intent blob and no recovery pass exist —
`open` never repairs. `next_seq` advances inside the same atomic image as the
records it stamps, so the "no reuse" property of sequence numbers holds across
the crash boundary exactly as it holds for L1 ids.

## 5. Invariant checklist (§5)

1. **Single-integer reachability** — the root id alone recovers everything;
   `next_seq` persists with it. ✓
2. **Typed root** — `CLOG` magic + version; wrong-type `open` → `-ENOTSUP`. ✓
3. **One commit point per mutation** — a lone `blob_db_update`; no residue. ✓
4. **O(1) steady-state RAM** — stateless; no per-record RAM, no cache. ✓
5. **Bounded stack** — one `BLOB_DB_MAX_PAYLOAD_LEN` image + one serialize
   buffer, both ≤ payload (default 256 B; ≤ 4 KB at the max). ✓

## 6. Open items

- **Capacity is one payload.** By construction (single i-node). Deep logs use
  multiple rings, or a future multi-node `logring` v2 that chains overflow
  segments under the §2.2 stage/commit/cleanup protocol (would then need the
  shared intent helper). Not required by any current L3 interface.
- **No binary search by seq.** `iterate` is linear; adequate for a diagnostic
  log walked start-to-finish. A `seek(seq)` extension op could bisect the
  inline run if a consumer ever needs random access by sequence number.
