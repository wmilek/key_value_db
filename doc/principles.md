# Design Principles

Status: v1 · Applies to every layer of the stack (see `doc/architecture.md`).
When two principles conflict, the earlier-numbered one wins.

## P1 — Zephyr-based

Use Zephyr mechanisms, not parallel inventions: Kconfig + CMake
(`add_subdirectory_ifdef`) for build, `<zephyr/sys/crc.h>` / logging /
`BUILD_ASSERT` for utilities, ztest + twister (`native_sim` first) for tests.
**Storage is never raw flash**: every layer talks to a high-level interface
(`flash_area` today; a UBI-like provider is just another layer behind the same
kind of interface).

## P2 — Embedded

Deterministic, bounded cost: every operation documents its worst case in flash
I/O and stack. Prefer stack buffers (≤ one sector, 4 KB) over dynamic
allocation — heap use is allowed where justified, not banned. Favor read-heavy
algorithms; discover flash geometry at runtime, assume nothing beyond the L0
contract.

## P3 — Minimum RAM usage

O(1) steady-state RAM per module: no per-blob/key/node RAM, no caches, no
mount-time index rebuild. State is re-read from flash on demand.

## P4 — Configurable via Kconfig

Every module above the core has its own symbol; disabled = zero flash/RAM.
Backends are `choice`s; selection flows downward (L3 `select`s its L2
container, L2 depends on L1). Tunables are Kconfig options, not magic numbers.

## P5 — Single-integer reachability

An i-node id is a persistent pointer; a structure is its root id plus an
interpretation. Id = 1 is the root convention — one remembered integer
re-opens everything after reboot. No side-band persistent state.

## P6 — Strict downward layering

Dependencies point only downward across narrow, documented contracts
(`flash_area` → `blob_db` API → `map_ops`/`seq_ops`). Providers behind a
contract are swappable without source changes above.

## P7 — Crash-safe

Every visible operation is atomic across power loss. Multi-node mutations use
copy-on-write with a single commit write; torn writes are detected (CRC) and
discarded; recovery is bounded and idempotent.

## P8 — Test-proven

Each module ships a ztest suite; crash-recovery and persistence claims are
exercised (torn writes, corruption, remount), not just argued.
