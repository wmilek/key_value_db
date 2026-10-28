# Design Principles

Status: v1 · Applies to every layer of the stack (see `doc/architecture.md`).
When two principles conflict, neither wins by rank or number. The conflict is
decided deliberately, where it arises, and the decision and its reason are
written down there. Numbers are permanent labels: a new principle takes the
next free number, and none is reused or renumbered.

## P1 — Zephyr-based

Use Zephyr mechanisms, not parallel inventions: Kconfig + CMake
(`add_subdirectory_ifdef`) for build, `<zephyr/sys/crc.h>` / logging /
`BUILD_ASSERT` for utilities, ztest + twister (`native_sim` first) for tests.
**Storage is never raw flash**: every layer talks to a high-level interface.
`blob_db` addresses blocks through the `blob_db_store` seam, and the provider
behind it — a UBI volume by default, or `flash_area` over a raw partition — is
a build-time choice that nothing above L1 can observe.

## P2 — Embedded

Deterministic, bounded cost: every operation documents its worst case in flash
I/O and stack. Prefer stack buffers (≤ one sector, 4 KB) over dynamic
allocation — heap use is allowed where justified, not banned. Favor read-heavy
algorithms; discover flash geometry at runtime, assume nothing beyond the L0
contract.

## P3 — Minimum RAM usage

O(1) steady-state RAM per module: no per-blob/key/node RAM, no caches, no
mount-time index rebuild. State is re-read from flash on demand.

**One opt-in exception:** `CONFIG_BLOB_DB_SLOT_INDEX` keeps an id → slot
offset table in RAM (`doc/impl/l1_bucketlog.md` §5.4a). It is off by default,
fixed in size by Kconfig rather than growing with the store, filled by the scan
mount already does, and a hint rather than state: a lost or full index costs
speed, never correctness. It exists because the measured alternative, walking
every slot on every lookup, dominates read cost on large erase blocks.

## P4 — Configurable via Kconfig

Every module above the core has its own symbol; disabled = zero flash/RAM.
Backends are `choice`s; selection flows downward (L3 `select`s its L2
container, L2 depends on L1). Tunables are Kconfig options, not magic numbers.

## P5 — Single-integer reachability

An i-node id is a persistent pointer; a structure is its root id plus an
interpretation. Id = 1 is the root convention — always present after mount,
one remembered integer re-opens everything after reboot. No side-band
persistent state.

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
  impossible (e.g. a dangling reference), repair it — drop the entry and
  report what was dropped — rather than carry it.
- **Avoidance first (advisable).** Order operations so the windows in which
  residue can arise are as few and as small as possible.

## P8 — Test-proven

Each module ships a ztest suite; crash-recovery and persistence claims are
exercised (torn writes, corruption, remount), not just argued.

## P9 — No destruction without intervention

blob_db erases only what it can show is garbage: data that a later committed
write replaced or deleted, and the residue of its own interrupted writes (P7).
Everything else is kept until the application asks for it to go —
`blob_db_format()` at run time, or an explicit Kconfig opt-in such as
`CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT`. That covers a store it did not write or
does not recognize, and committed data that has become unreadable.

Mount refuses such a store with nothing written. Damage inside a mounted store
is reported and left in place: it may hold the only copy of something, and it
is the evidence of what happened. Mount formats on its own only a partition
with nothing to lose — both masters blank, together with whatever buckets an
interrupted format left behind them.
