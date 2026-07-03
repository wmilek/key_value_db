# Design Principles

Status: v1 · Applies to every layer of the storage stack (see `doc/architecture.md`)

These principles are binding for all modules in this repository — L0 through L3.
A design document that violates one of them must call the violation out explicitly
and justify it. When two principles conflict, the earlier-numbered one wins.

---

## P1 — Zephyr-native

The project is a Zephyr application; every module uses Zephyr's mechanisms
instead of inventing parallel ones.

- **Storage** goes through `<zephyr/storage/flash_map.h>` (`flash_area_*`) — never
  raw flash drivers.
- **Build** integration is CMake + `add_subdirectory_ifdef`, mirroring the
  existing `lib/custom/` and `lib/blob_db/` patterns.
- **Configuration** is Kconfig (see P4), sourced into the existing `lib/Kconfig`
  menu.
- **Utilities**: `<zephyr/sys/crc.h>` for CRC, `<zephyr/logging/log.h>` with a
  per-module `module-str`, `BUILD_ASSERT` for on-flash struct invariants.
- **Testing** is ztest, run via `west twister`, with `native_sim` as the primary
  development and CI target.
- Public headers live under `include/app/lib/`, following the repository's
  namespace convention.

## P2 — Embedded-first

Code must be suitable for a resource-constrained MCU, not just `native_sim`.

- **No dynamic allocation.** No `k_malloc`/heap use on any path; transient
  buffers are stack-allocated and bounded (≤ one flash sector, 4 KB).
- **Bounded stack.** Worst-case stack per call is documented (see the cost tables
  in each layer's design doc).
- **Deterministic cost.** Every operation has a stated worst-case in flash
  reads/writes; no operation's cost grows with total database size unless the
  design doc says so explicitly (e.g. `mount`, `iterate`).
- **Flash-friendly.** Prefer read-heavy algorithms over write-heavy ones; respect
  write-block alignment; never rewrite a sector when an append will do.
- **Portable.** No assumptions beyond the `flash_area` contract (P6): erase-block
  size, write alignment, and partition geometry are discovered at runtime.

## P3 — Minimum steady-state RAM

RAM held *between* calls is the scarcest resource.

- **O(1) steady-state RAM per module.** No per-blob, per-key, per-node, or
  per-file RAM. `blob_db` owns ~32 bytes between calls; containers and interfaces
  hold only a root id plus a small fixed handle.
- **No caches.** State is re-read from flash on demand. If a future layer adds a
  cache, it must be optional (Kconfig, default off) and bounded.
- **No RAM index rebuild at mount.** A structure is reachable from its root
  i-node (P5); opening it must not require scanning it into RAM.

## P4 — Configurable via Kconfig

Everything above the always-present core is à la carte.

- **Each module has its own Kconfig symbol** and is added to the build only via
  `add_subdirectory_ifdef`. A disabled module contributes zero flash and zero RAM.
- **Selection flows downward**: an L3 interface `select`s the L2 container that
  backs it; L2 depends on L1; L1 selects its L0 requirements (`FLASH`,
  `FLASH_MAP`, `CRC`). Invalid combinations are unrepresentable.
- **Tunables are Kconfig options** with sane defaults and ranges (payload limits,
  tree fan-out, hash bucket count) — not compile-time magic numbers.
- **Backends are `choice`s**: where an interface can be backed by several
  containers, the binding is a Kconfig `choice`, so exactly one is linked.

## P5 — Single-integer reachability (the root convention)

Every persistent structure is recoverable from **one `uint64_t`**.

- An i-node id is a persistent pointer. A container is "a root id plus an
  interpretation of the i-nodes reachable from it".
- By convention id = 1 (the first id ever assigned) is the client's root; a
  client that remembers only the integer 1 can re-open everything after reboot.
- No layer may require side-band persistent state (files, NVS entries, RAM
  journals) to find its own data.

## P6 — Strict downward layering

- Dependencies point strictly downward: L3 → L2 → L1 → L0. No layer references,
  includes, or links against a layer above it.
- Each boundary is a **narrow, documented contract** (the `flash_area` API at
  L0/L1; the `blob_db` API at L1/L2; `map_ops`/`seq_ops` at L2/L3). A provider
  behind a boundary is swappable without source changes above it — this is what
  lets a UBI-like FTL replace the plain partition at L0, and lets `kvdb` switch
  containers at build time.
- Upper layers may rely only on guarantees the lower layer's design document
  states — never on implementation details observed in its code.

## P7 — Crash-safety by construction

Power can fail at any instruction; the stack must never need a repair tool.

- **Every externally visible operation is atomic**: after a crash and remount, it
  either fully happened or never happened. No partial state is observable.
- Multi-i-node mutations use **copy-on-write with a single commit point** — one
  `blob_db_update` (typically of the root or a parent node) linearizes the whole
  mutation and inherits L1's single-operation atomicity.
- Torn writes are **detected** (CRC) and **discarded**, never silently accepted.
- Recovery is **bounded and idempotent**: mount-time recovery cost is O(1) sectors
  beyond the normal mount scan, and re-crashing during recovery is safe.

## P8 — Test-proven, on `native_sim` first

- Every module ships with a ztest suite under `tests/lib/…`, runnable via
  `west twister -p native_sim`.
- Crash-recovery claims are tested, not just argued: suites simulate torn
  writes, corrupt sectors, and mid-compaction states.
- Persistence claims are tested across remount (in-process) and, where relevant,
  across process restart (`--flash=` backing file).
- A change that alters an on-flash format must extend the tests before it lands.

---

## Quick compliance checklist for new design docs

- [ ] Uses only Zephyr APIs at its lower boundary (P1, P6)
- [ ] No heap; transient stack usage stated (P2)
- [ ] Steady-state RAM stated and O(1) (P3)
- [ ] Kconfig symbol(s), defaults, and `select`/`depends on` edges listed (P4)
- [ ] Structure reachable from a single root id (P5)
- [ ] Crash table or COW commit-point argument included (P7)
- [ ] Test plan with crash + persistence cases (P8)
