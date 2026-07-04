# L1 — `blob_db`: Stable-ID Blob Store — Contract & Requirements

Status: v3 · **Contract specification** — implementation-agnostic; everything
upper layers may rely on (P6) is in this document and nothing more.
· Part of the stack in `doc/architecture.md` · Governed by `doc/principles.md`
· Lower boundary: `doc/layers/l0_flash.md` · Consumed by: `doc/layers/l2_containers.md`
· Implementation design (feasibility, non-normative): `doc/impl/l1_bucketlog.md`

---

## 1. Scope

One Zephyr library module: **`lib/blob_db`** — an embedded **blob store with
stable identifiers** on top of the `flash_area` interface.

### What it is

`blob_db` stores opaque, variable-length **payloads** bound to **u64 ids**
obtained from the library (`alloc_id`). It does *not* know about strings,
keys, schemas, indexes, or queries. It promises one thing: **once content is
bound to an id, you can fetch it by that id** — across reboots, crashes, and
internal reorganization.

### What it isn't

Not a key-value store. Indexing (mapping user-meaningful keys to ids) is the
client's concern — see the container layer (`l2_containers.md`) and the model
container (`l1_model_container.md`).

```
┌──────────────────────────────────────────────────────────┐
│  Clients: containers, root registry, application         │
└──────────────────────────────────────────────────────────┘
              │   alloc_id / update / get / delete   (u64 id)
              ▼
┌──────────────────────────────────────────────────────────┐
│  blob_db  — stable u64 ids, never reused                 │
│             crash-atomic single operations               │
└──────────────────────────────────────────────────────────┘
              │   flash_area_*
              ▼
┌──────────────────────────────────────────────────────────┐
│  L0 provider (fixed partition today, FTL later)          │
└──────────────────────────────────────────────────────────┘
```

---

## 2. Stability contract

The load-bearing promise of the library — clients build on top of it, and may
rely on nothing beyond it.

| Property | Guarantee |
|---|---|
| **Id allocation** | `alloc_id` returns a u64 id never returned before in the lifetime of this DB — across all crashes — and strictly greater than every previously returned id. Allocation itself is a RAM operation. |
| **Id lifecycle** | *allocated* (fresh from `alloc_id`, nothing on flash) → *bound* (first `update` writes content) → rebound (further `update`s) → *dead* (`delete`). After `delete` the id **ceases to exist**: `get`/`exists`/`delete` on it are defined (`-ENOENT`/false), but `update` on it is **undefined behavior** — a debug build may assert. Likewise UB: `update` on an id never allocated. |
| **Id stability** | A bound id refers to the same logical blob until `delete`d. `update` keeps the id; only the payload changes. |
| **Reorganization transparency** | Internal reorganization (garbage collection, compaction, moves) never changes an id. A blob you can `get` today is reachable by the same id afterwards. |
| **No reuse** | A dead id is never returned by `alloc_id` again. The next allocated id is strictly greater than every id ever seen. |
| **Atomicity of single operations** | Each `update`/`delete` is atomic with respect to crash: either it takes effect fully or it doesn't (on next mount). Partial writes are detected and discarded — never surfaced as data. |
| **No partial reads** | `get` either returns the complete payload that was committed at some point, or returns `-ENOENT`/`-EIO`. It never returns partial bytes from an in-flight write. |

These together make ids usable as **persistent references** — foreign keys
for client-owned structures. What the contract means for a client — the call
ordering that makes multi-blob structures crash-safe, and which steps can
leave unreferenced blobs — is demonstrated operation by operation in
`doc/layers/l1_model_container.md` (the *model container*).

### Root convention

The very first `alloc_id` after a fresh format returns **id = 1**:

```
First boot:  id = blob_db_alloc_id()                     →  id = 1
             blob_db_update(1, root_payload, ...)        ← binds the root
On mount:    blob_db_get(1, ...)                         ← always finds the root
```

One remembered integer bootstraps everything (P5). In the full stack, id = 1
is owned exclusively by the **root registry**
(`doc/layers/l1_root_registry.md`); other clients obtain their roots through
registry keys rather than binding id = 1 themselves.

---

## 3. Requirements

Any implementation of this contract must satisfy:

- **R1 — Steady-state RAM is O(1).** No per-blob RAM between calls, no
  caches, no mount-time index rebuild (P3).
- **R2 — `get` cost is independent of database size.** O(1) flash reads per
  lookup; no linear scan of the database.
- **R3 — Write operations are O(1) flash writes** amortized; occasional
  bounded maintenance (e.g. local garbage collection) is permitted, favoring
  reads over writes (P2).
- **R4 — Mount may read the whole partition** but must complete in bounded
  time and O(1) RAM; recovery after a crash is bounded and idempotent (P7).
- **R5 — Crash-safety per P7 must-tiers**: partial writes are never data;
  visible state flips atomically; internal residue is bounded and reclaimed
  by the implementation's own maintenance.
- **R6 — Scale target**: 100 000 blobs in an 8 MB partition; payload size cap
  is a Kconfig option; no structural limit other than partition size.
- **R7 — Transient buffers are stack-allocated and bounded** (≤ one erase
  sector; 4 KB target on `native_sim`).

---

## 4. Public API (`include/app/lib/blob_db.h`)

```c
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

int      blob_db_mount(void);
int      blob_db_unmount(void);

/* Allocate a fresh id: never returned before, strictly greater than every
 * previously returned id, across all remounts and crashes (contract §2).
 * RAM operation; returns 0 when not mounted (0 is never a valid id). Also
 * the watermark client crash-recovery is built on (l1_model_container.md §4). */
uint64_t blob_db_alloc_id(void);

/* Bind (first write) or rebind (rewrite) the payload of an allocated id.
 * Returns -ENOSPC when out of space even after maintenance; -EINVAL if id
 * is 0 or was never allocated. UNDEFINED BEHAVIOR if id is dead (deleted):
 * the id no longer exists — a debug build may assert; release performs no
 * check. */
int      blob_db_update(uint64_t id, const void *payload, size_t len);

/* Defined on any id, dead or alive: -ENOENT / false when not bound. */
int      blob_db_get   (uint64_t id, void *out, size_t out_sz, size_t *out_len);
int      blob_db_delete(uint64_t id);
bool     blob_db_exists(uint64_t id);

size_t   blob_db_count(void);

typedef int (*blob_db_iter_cb_t)(uint64_t id,
                                  const void *payload, size_t len,
                                  void *user);
int      blob_db_iterate(blob_db_iter_cb_t cb, void *user);

/* Format the partition (erase all blobs, reset the id counter to 1). For
 * factory reset / tests. */
int      blob_db_format(void);
```

Errors: `-ENOENT`, `-ENOSPC`, `-ENOMEM`, `-EINVAL`, `-EIO`, `-ENODEV` (not
mounted), `-EALREADY` (double mount), `-ENOTSUP` (foreign on-flash format).

**Concurrency contract (v1):** single-threaded — caller serializes. v2 may
add a `k_mutex`. Corollary used by client recovery: at most one mutation is
in flight per database.

**Planned extension** (specified, not yet part of the header): pread-style
partial access, implementable by any allocator — see decision D4 (§5.4).

---

## 5. Design decisions (contract-level, with rejected alternatives)

### 5.1 D1 — One interface, exchangeable allocators

The layer boundary is **only** the API (§4) plus the stability contract (§2).
The storage strategy behind it — v1's *bucket-log* (`doc/impl/l1_bucketlog.md`),
a future FAT-like or extent-based allocator — is an implementation selected at
build time:

```
choice BLOB_DB_ALLOCATOR
  BLOB_DB_ALLOC_BUCKETLOG   # v1
  BLOB_DB_ALLOC_FAT         # future
  BLOB_DB_ALLOC_EXTENT      # future
endchoice
```

**Swap policy:** on-flash formats are mutually incompatible; changing the
allocator means a reformat — all stored data is lost, code above L1 is
unaffected. Each allocator uses a distinct on-flash magic so a mismatched
mount fails cleanly with `-ENOTSUP`. Every allocator must uphold §2 and §3 in
full — in particular id monotonicity across crashes, which client recovery
(model container, root registry) is built on.

### 5.2 D2 — Payloads are mutable

An id is the stable name of a **logical** blob; `update` replaces content
under the same id. Consequence: a mutation that does not change the reference
graph collapses to a single atomic `update` (`l1_model_container.md` §3.4).

Rejected alternative: immutable blobs + a mutable root register (git/ZFS
model). It would buy one-live-version-per-id simplicity, create-only
atomicity, torn-free partial reads, and lock-free readers — but write
amplification lands on the most common operation (an in-place value change
becomes path-copy-to-root), and ids would name *versions*, not logical
objects, destroying the foreign-key property clients build on. Revisit if a
multi-chunk allocator with partial reads lands, or a concurrent v2 wants
lock-free readers.

### 5.3 D3 — No resurrection: writes to dead ids are UB

`delete` is final: the id ceases to exist. There are deliberately **no**
defined semantics for writing to a dead id (no "unbind/rebind" model). A
stale write is a client bug; the narrow contract lets debug builds assert on
it (fail-fast) without obligating release builds to pay an existence check.
Reads of dead ids stay defined (`-ENOENT`) because recovery and self-healing
legitimately probe stale ids.

### 5.4 D4 — Payload chunking lives at L2 (v1); pread extension reserved

v1 keeps L1 payloads single-chunk (bounded by `BLOB_DB_MAX_PAYLOAD_LEN`);
large data is chained at L2 (`seq` container). If a future allocator spreads
payloads transparently, whole-blob `get` becomes inadequate; the agreed
extension is allocator-agnostic partial access:

```c
int blob_db_size(uint64_t id, size_t *out_size);
int blob_db_read(uint64_t id, size_t offset, void *out, size_t len,
                 size_t *out_read);
```

A multi-chunk write must commit by writing the id's index record **last**, so
"no partial reads" (§2) holds unchanged. Writes stay whole-blob until a
concrete consumer needs streaming.

---

## 6. Companion documents

| Document | Role |
|---|---|
| `doc/layers/l1_model_container.md` | The client contract in executable form: mutation discipline, crash tables, recovery — normative for every L1 client. |
| `doc/layers/l1_root_registry.md` | Owner of id = 1; where clients persist their structure roots. |
| `doc/impl/l1_bucketlog.md` | Feasibility/implementation design of the v1 allocator: on-flash format, algorithms, crash recovery mechanics, costs, open implementation items. **Non-normative** — upper layers must not depend on anything in it. |
