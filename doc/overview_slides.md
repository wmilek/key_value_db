# Layered Storage Stack — Architecture Overview

*A 6-slide architecture summary. Source of truth: `doc/architecture.md`,*
*`doc/principles.md`, `doc/layers/`, `doc/impl/`.*

---

## Slide 1 — What it is, design & principles

**A crash-safe, layered key–value / blob storage stack for Zephyr firmware on raw flash.**

One idea carries the whole design:

> **Every persistent structure is reachable from a single integer.**

An i-node id (`uint64_t`) is a persistent pointer — stable for the blob's lifetime,
never reused. A structure is just a *root id* + a rule for interpreting the i-nodes
reachable from it. The first id is **1**, so remembering the integer `1` re-opens
everything after reboot: no journal replay, no index rebuild, no side-band state.

**Design principles (P1–P8):**

| Zephyr-based | Embedded | Minimum RAM | Kconfig-configurable |
|---|---|---|---|
| use `flash_area`, Kconfig, ztest — never raw flash | bounded, documented worst-case cost | O(1) steady-state RAM, no caches | every module optional; disabled = zero cost |

| Single-integer reachability | Strict downward layering | Crash-safe | Test-proven |
|---|---|---|---|
| one integer re-opens everything | swappable contracts, deps point down | atomic, no leak, self-healing | claims *exercised*, not argued |

**Why UBI is the right choice at L0.** The lowest layer (flash translation) is
deliberately behind Zephyr's fixed `flash_area` interface — *the interface is the
design decision, the provider behind it is interchangeable.* Today it's a plain
NOR/`native_sim` partition. For flash that needs real management (NAND, or NOR at
high write volume), a **UBI-like FTL** is the natural fit: it brings wear
leveling and bad-block remapping (logical→physical erase-block mapping via per-PEB
headers) while preserving the *same upper contract* — including the single-sector
torn-write blast radius the layers above rely on. Integration is a **pointer swap**
(`CONFIG_BLOB_DB_PARTITION_LABEL`), not a rewrite: zero source change above L0.

---

## Slide 2 — The layers

Dependencies point strictly **downward** across narrow contracts; every layer above
the core is independently enable-able via Kconfig (disabled = zero flash/RAM).

```
┌────────────────────────────────────────────────────────────────┐
│ L3  Access interfaces   kvdb · blobfs · settings                │  keys, paths, records
├────────────────────────────────────────────────────────────────┤
│ L2  Containers          seq · kvlist · kvhash · kvtree          │  data structures of i-nodes
├────────────────────────────────────────────────────────────────┤
│ L1½ Root registry       owner of id = 1; key → structure root   │  "where is my structure"
├────────────────────────────────────────────────────────────────┤
│ L1  i-node allocation   blob_db: stable u64 id → blob           │  the always-present core
├────────────────────────────────────────────────────────────────┤
│ L0  Flash translation   flash_area today · UBI-like FTL later   │  swappable provider
└────────────────────────────────────────────────────────────────┘

        L3 ──map_ops/seq_ops──► L2 ──blob_db API──► L1 ──flash_area──► L0
```

- **L0** — raw flash behind `flash_area`; UBI-like FTL slots in behind the same interface.
- **L1 `blob_db`** — the always-present core: a pool of stably-identified, crash-atomic blobs.
- **L1½ Root registry** — tiny map of compile-time keys → root ids; owns id = 1.
- **L2 Containers** — data structures wired out of i-nodes (Map + Sequence shapes).
- **L3 Interfaces** — what the firmware actually calls: keys, paths, records.

---

## Slide 3 — L1 `blob_db`: the allocation concept

**`blob_db` turns a flash partition into a pool of blobs, each named by a stable `uint64_t` id.**
It knows nothing about keys, order, schemas, or indexes. It promises exactly one thing:

> *once content is bound to an id, you can fetch it by that id* — across reboots,
> crashes, and internal reorganization; the id is never reused.

**The core operation — allocation:**

```
alloc_id()  ──►  a fresh id (never seen before, strictly increasing)
update(id, payload)  ──►  the allocator finds a place for this chunk on flash,
                          writes it crash-atomically, binds it to the id
get(id)      ──►  the payload, wherever it now lives
delete(id)   ──►  the id ceases to exist; its space is reclaimable
```

The essence is **placement + naming**: for a requested chunk the allocator *searches
for a place to store it* and hands back a stable id; reads later resolve that id to
wherever the chunk currently lives. Internal moves (garbage collection, compaction)
never change the id — *reorganization is transparent* to everyone above.

**Deliberately universal.** *Many* placement algorithms can satisfy this contract —
the v1 design is a hash-bucketed append-log, but a FAT-like or extent-based
allocator could replace it at the cost of a reformat. Because upper layers depend
only on the *contract* (stable id + crash-atomic single ops), the allocator behind
it is swappable. The contract's sufficiency — that these primitives can carry every
layer above — is *proved by construction* by the model container (Slide 4).

---

## Slide 4 — L2 Containers: the building blocks

**A container is a root i-node id + a rule for interpreting the i-nodes reachable from it.**
L2 turns the flat pool of blobs into real data structures: every pointer between
pieces of data is *an i-node id stored inside another i-node's payload*, and L2 is
the layer that writes and follows those pointers. Two abstract **shapes**:

| Shape | Ops | Concrete containers |
|---|---|---|
| **Map** (key → value) | get/set/del/has/iterate | `kvlist` O(n) · `kvhash` O(1) · `kvtree` O(log n), ordered |
| **Sequence** (index-addressed) | append/get/set/remove/len/iterate | `seq` (chunk-chained list) |

Mutations are **copy-on-write with a single root `update` as the atomic commit
point** — nodes are rewritten (keeping their ids, so parents stay valid), and the
one write that makes new nodes reachable is the linearization point. These are the
building blocks from which more complex structures are composed.

**The model container — the reference.** The `kvlist` ground form is the *model
container*: the smallest complete crash-safe structure, built using **nothing but
the L1 contract**. Its role is threefold:

- **Sufficiency proof** — if it works, the `blob_db` contract is *enough* to build
  every layer above; a capability it needs but the contract lacks is a contract gap
  (this is how `alloc_id` earned its place).
- **Reference pattern** — its 5-step discipline (*stage → prepare → commit → cleanup
  → clear*, with an intent blob + id watermark) is the crash-safe protocol *every*
  real container inherits; a container that deviates needs its own P7 analysis.
- **Acceptance-test blueprint** — implemented as a ztest suite with crash injection
  between every step, it validates *any* allocator claiming to implement L1.

---

## Slide 5 — L3 High-level interfaces (built on containers)

**What the rest of the firmware actually calls.** L3 defines *no on-flash format of
its own* — it translates a domain vocabulary into `map_ops`/`seq_ops` on a container,
inheriting all persistence, atomicity and recovery from L2/L1. Each interface binds
to a *shape*, and Kconfig binds the shape to a concrete container.

- **`kvdb` — object database with indexes.** String key → value over any Map.
  The choice of backing container *is* the index strategy: `kvhash` for O(1) point
  lookup, or `kvtree` for a **sorted / range-queryable index** (ordered `foreach`,
  prefix/range scans) — indexes that are themselves just containers. Same API across
  backends; only cost and iteration order change.

- **`blobfs` — filesystem on top of containers.** Hierarchical `path → file`:
  a **directory is a Map container** (`name → {type, i-node id}`, nesting gives
  paths); a **file body is inline** (small) or a **`seq` chunk chain** (large /
  streamed). Handle-free pread/pwrite style — no open/close, no cursor state.
  Within-directory `rename` is a single atomic Map mutation.

- **Event log / queue — the Sequence shape.** `seq` is O(1) append with a chunk
  chain, exactly the shape for logs, queues and streamed records — an append-only
  event log falls straight out of the Sequence container.

- **`settings` registry.** Flat typed config, optionally as a **Zephyr `settings`
  backend**, so existing subsystems (BT bonding, net config) persist through this
  stack instead of NVS.

Everything above L1 is à la carte: enabling an interface auto-resolves its whole
downward slice via Kconfig `select`, and invalid combinations are unrepresentable.

---

## Slide 6 — Implementation status

**Contracts:** L0–L3 specified. The v1 bucket-log implementation design exists with tracked open items.

| Layer / module | Status | Evidence |
|---|---|---|
| **L1 `blob_db`** | ✅ **Implemented** (~1400 LoC) | `alloc_id` + bind/rebind `update`, durable leading-id ceiling; QSPI-safe I/O; `erase_all`, `prepare(n)`. |
| **L1½ `rootreg`** | ✅ **Implemented** (~300 LoC) | Root registry per contract; owns id = 1; ztest suite. |
| **Model container** | ✅ **Proven** | Acceptance suite (`tests/lib/blob_db_contract`) with crash injection at every mutation step. |
| **Test suites** | ✅ On `native_sim` | blob_db unit suite + model-container contract suite. |
| **Benchmarks** | ✅ On hardware (nRF5340-DK, QSPI NOR) | `app_perf` (raw) + `app_perf_mc` (model container via rootreg). |
| **L2 containers** (seq, kvlist, kvhash, kvtree) | 🚧 **Scaffolded** | Build-wired, Kconfig-gated `default n`, stub modules. |
| **L3 interfaces** (kvdb, blobfs, settings) | 🚧 **Scaffolded** | Stub modules; contracts drafted. |

**Benchmark results** (nRF5340-DK @ 8 MHz Quad-SPI NOR, warm append-only path):

| Raw `blob_db` | ops/s | | Model container (key→value map) | ops/s |
|---|---:|---|---|---:|
| read | 59.3 | | get | 6.1 |
| update | 29.1 | | set | 4.2 |
| append (warm) | 15.9 | | overwrite | 3.8 |
| append (cold) | 0.85 | | delete | 4.7 |

- Cold vs. warm append is ×18.6 — a full 64 KB sector erase (~1 s) hides inside each
  cold write; `blob_db_prepare(N)` hoists it out of the timed loop.
- The model container's ~1/6th throughput is the *measured price* of the full 5-step
  crash-safe, zero-leak mutation discipline — by design, not overhead to optimize.

**Footprint** (architectural guarantees from P2/P3/P4):

| Resource | Cost | Why |
|---|---|---|
| **Steady-state RAM** | **O(1) per module** — no per-blob/key/node RAM, no caches, no mount-time index | P3: state is re-read from flash on demand |
| **Transient RAM** | a single **≤ 4 KB stack buffer** (one sector) per operation; handles are O(1) (root id + cursor), `kvtree` adds O(depth) ids | P2: stack over heap, bounded worst case |
| **Code (source)** | L1 `blob_db` **~1400 LoC**, L1½ `rootreg` **~300 LoC**; every module above the core is Kconfig-gated | P4: disabled module = **zero flash & RAM** |
| **Flash data overhead** | per blob: one append-log slot = header + payload + CRC; partition = N × sector-sized buckets | append-only, compaction reclaims garbage |

*Binary ROM/RAM sizes are not yet measured on target — the numbers above are the
design footprint; a per-config `west build` size report is the next step.*

**Bottom line:** the always-present crash-safe core (**L1 + L1½**) is built, tested,
and benchmarked on real hardware; the à-la-carte layers above (**L2/L3**) are
specified and scaffolded, awaiting implementation.
