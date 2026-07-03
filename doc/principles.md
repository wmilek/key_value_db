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

Power can fail at any instruction. In order of strength:

- **Never treat partial writes as data (must).** An operation interrupted by
  power-off is detected (CRC) and discarded at L1; a reference that does not
  resolve is treated as absent at L2/L3 — never returned as data.
- **Atomic visible state (must).** Every externally visible operation either
  fully happened or never happened; multi-node mutations use copy-on-write
  with a single commit write.
- **No permanent leak (must).** Crash residue is confined to unreachable
  i-nodes of one interrupted mutation and is reclaimed by bounded, idempotent
  recovery. Long-term accumulation of leaked space is not accepted.
- **Self-healing (advisable).** On detecting residue that should be
  impossible (e.g. a dangling reference), repair it — drop the entry — rather
  than carry it.
- **Avoidance first (advisable).** Order operations so the windows in which
  residue can arise are as few and as small as possible.

## P8 — Test-proven

Each module ships a ztest suite; crash-recovery and persistence claims are
exercised (torn writes, corruption, remount), not just argued.
