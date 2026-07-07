# Layered Storage Stack — Overview

*A 4-slide summary of the proposed architecture and current implementation status.*
*Source of truth: `doc/architecture.md`, `doc/principles.md`, `doc/layers/`, `doc/impl/`.*

---

## Slide 1 — What it is

**A crash-safe, layered key–value / blob storage stack for Zephyr firmware on raw flash.**

The whole design rests on one idea:

> **Every persistent structure is reachable from a single integer.**

An i-node id (`uint64_t`) is a persistent pointer. L1 guarantees it stays valid
for the blob's lifetime and is never reused. A container is just a *root id* plus
a rule for interpreting the i-nodes reachable from it. The first id ever assigned
is **1**, so a client that remembers only the integer `1` re-opens everything
after reboot — **no journal replay, no index rebuild, no side-band state.**

- Built on Zephyr mechanisms (Kconfig + CMake, `flash_area`, ztest/twister).
- Storage is never raw flash — every layer talks to a high-level contract.
- Target: embedded, O(1) steady-state RAM, deterministic bounded cost.

---

## Slide 2 — The proposed architecture (5 layers)

Dependencies point strictly **downward** across narrow contracts; each layer is
independently enable-able via Kconfig (disabled = zero flash/RAM cost).

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

- **L0** — raw flash behind the fixed `flash_area` interface; an FTL (wear
  leveling, bad-block remap) can slot in later behind the *same* interface.
- **L1 `blob_db`** — turns a partition into a pool of stably-identified,
  crash-atomically updatable blobs. The allocator behind it (v1: hash-bucketed
  append-log) is exchangeable.
- **L1½ Root registry** — a tiny map of compile-time keys → root ids; owns id = 1.
- **L2 Containers** — Map (`kvlist` O(n) · `kvhash` O(1) · `kvtree` O(log n)) and
  Sequence (`seq`); copy-on-write with a single root `update` as the commit point.
- **L3 Interfaces** — `kvdb` (string key → value), `blobfs` (paths/files),
  optional Zephyr `settings` backend.

---

## Slide 3 — Design principles (P1–P8)

The binding rules every layer must satisfy (earlier number wins on conflict):

| # | Principle | In short |
|---|---|---|
| P1 | **Zephyr-based** | Use Zephyr mechanisms, never raw flash or parallel inventions. |
| P2 | **Embedded** | Deterministic, bounded cost; each op documents its worst-case flash I/O & stack. |
| P3 | **Minimum RAM** | O(1) steady-state RAM per module; state re-read from flash on demand. |
| P4 | **Kconfig-configurable** | Every module has its own symbol; disabled = zero cost; backends are `choice`s. |
| P5 | **Single-integer reachability** | One remembered integer (`1`) re-opens everything; no side-band state. |
| P6 | **Strict downward layering** | Dependencies only point down, across swappable contracts. |
| P7 | **Crash-safe** | Never treat partial writes as data; atomic visible state; no permanent leak; self-healing. |
| P8 | **Test-proven** | Each module ships a ztest suite; crash/persistence claims are *exercised*, not argued. |

---

## Slide 4 — Implementation status

**Contracts:** L0–L3 specified. Bucket-log implementation design exists with tracked open items.

| Layer / module | Status | Evidence |
|---|---|---|
| **L1 `blob_db`** | ✅ **Implemented** (~1400 LoC) | `alloc_id` + bind/rebind `update`, durable leading-id ceiling; QSPI-safe reads/writes; `erase_all`, `prepare(n)`. |
| **L1½ `rootreg`** | ✅ **Implemented** (~300 LoC) | Root registry per `l1_root_registry.md`; owns id = 1; ztest suite. |
| **Test suites** | ✅ On `native_sim` | Unit suite (`tests/lib/blob_db`) + model-container acceptance suite with crash injection at every mutation step (`tests/lib/blob_db_contract`). |
| **Benchmarks** | ✅ On hardware (nRF5340-DK, QSPI NOR) | `app_perf` (raw blob_db) + `app_perf_mc` (model container via rootreg) with reference ops/s. |
| **L2 containers** (seq, kvlist, kvhash, kvtree) | 🚧 **Scaffolded** | Build-wired, Kconfig-gated `default n`, ~9-line stubs — not yet implemented. |
| **L3 interfaces** (kvdb, blobfs) | 🚧 **Scaffolded** | Stub modules only. |

**Reference numbers (warm, append-only path, nRF5340-DK @ 8 MHz Quad-SPI):**

- Raw `blob_db`: ~59 reads/s, ~29 updates/s, ~16 appends/s.
- Model container (key→value map): set 4.2 · get 6.1 · overwrite 3.8 · delete 4.7 ops/s
  — the measured price of a 5-step, crash-safe, zero-leak mutation discipline.

**Bottom line:** the always-present crash-safe core (L1 + L1½) is built, tested,
and benchmarked on real hardware; the à-la-carte layers above it (L2/L3) are
scaffolded and awaiting implementation.
