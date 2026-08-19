# L0 — Flash Translation Layer

Status: v2 (the L0 boundary is the `blob_db_store` seam; two providers ship,
UBI is the default)
· Part of the stack in `doc/architecture.md` · Governed by `doc/principles.md`

---

## 1. Role

L0 owns raw flash: erase blocks, write alignment, and — where the substrate
needs it — wear leveling, bad-block handling, and logical-to-physical block
mapping. It presents the layer above (L1, `blob_db`) with a **uniform array of
equally-sized, individually-erasable blocks**.

The **interface is the design decision; the provider behind it is
interchangeable.** That interface is `lib/blob_db/blob_db_store.h`: a
PEB-addressed read/write/erase seam, selected at build time by the
`BLOB_DB_BACKEND` Kconfig choice.

```
┌──────────────────────────────────────────────┐
│ L1  blob_db                                  │
└──────────────────────────────────────────────┘
                 │  lib/blob_db/blob_db_store.h
                 │  blob_db_store_{open,close,read,write,erase}
                 │  struct blob_db_store_geom {peb_size, write_align, n_pebs}
                 ▼
┌──────────────────────────────────────────────┐
│ L0 provider (exactly one, Kconfig choice):   │
│   a) flash_area over a fixed DT partition    │   BLOB_DB_BACKEND_FLASH_AREA
│   b) a dynamic UBI volume                    │   BLOB_DB_BACKEND_UBI ← default
└──────────────────────────────────────────────┘
```

### 1.1 Why the seam is not a virtual `flash_area` (supersedes the v1 plan)

v1 of this document specified UBI integration as a *virtual `flash_area`
provider*: L1 would keep calling `flash_area_*`, UBI would synthesize a
partition, and the integration would be "a pointer swap via
`CONFIG_BLOB_DB_PARTITION_LABEL`, zero code in this repository". That plan was
abandoned during implementation, for a concrete reason worth recording:

`blob_db`'s hot path is an **in-place append at a growing offset inside one
block** — write a slot record, leave the rest of the block erased, append the
next record later. UBI's natural expression of that is `ubi_leb_write_at()`,
which does exactly it. A synthesized `flash_area` would have had to hide LEB
identity behind a flat offset and then reconstruct it, buying nothing and
costing the one operation the allocator depends on. So UBI is attached at its
own API, and the abstraction moved one level up: L1 addresses PEBs, and both
providers implement that.

The result is a *narrower* contract than `flash_area` (P6): six functions and
three geometry fields, none of which assume a Zephyr flash map. `flash_area` is
now one provider behind the seam rather than the boundary itself.

## 2. The interface contract

L1 consumes **only** the following, and every L0 provider must honor exactly
this — nothing more is ever assumed (principle P6):

| Primitive | Contract |
|---|---|
| `blob_db_store_open` | Opens the substrate and reports geometry: `peb_size` (usable bytes per block), `write_align`, `n_pebs`. Geometry is stable for the mount's lifetime **and across boots** — L1 hashes ids modulo the block count, so a changed count is a changed store. |
| `blob_db_store_read` | Returns the last committed bytes at that offset. |
| `blob_db_store_write` | Offset and length aligned to `write_align`; a completed write is durable across power loss. Writes never cross a PEB boundary. |
| `blob_db_store_erase` | Returns whole blocks to the erased state; afterwards the region reads back as `0xff`. |
| `blob_db_store_close` | Releases the substrate. Idempotent. |
| **Torn-write blast radius** | A power loss mid-write/mid-erase may leave arbitrary bytes **in the affected block only**. Other blocks are untouched. |

The last row is the load-bearing one: L1's crash model (CRC-guarded slots,
double-buffered master, scratch-block compaction — see `doc/impl/l1_bucketlog.md`
§6) absorbs any single-block corruption, but assumes corruption never leaks
across block boundaries. **A provider that cannot promise this cannot carry
`blob_db`.** The UBI provider preserves it despite moving data internally: a
wear-leveling copy is committed by UBI's own atomic LEB→PEB remap, so a torn
copy loses the *new* PEB, not the LEB's contents.

Two properties are deliberately *not* in the contract, because neither provider
guarantees them and L1 must never grow a dependency on them: that a block's
usable size equals the physical erase-block size (UBI's does not — see §4), and
that flat offsets correspond to physical addresses (under UBI they do not).

## 3. Provider a) — `flash_area` over a fixed partition

`CONFIG_BLOB_DB_BACKEND_FLASH_AREA` (`lib/blob_db/blob_db_store_flash.c`, ~100
lines of translation).

- A `fixed-partitions` child node in the device tree, label
  `CONFIG_BLOB_DB_PARTITION_LABEL` (default `storage`), mapped 1:1 onto
  physical flash. On `native_sim`: 16 MB simulated flash, 8 MB partition at
  0x800000 (`app/boards/native_sim.overlay`).
- 1 PEB = 1 physical erase block, whole and unshared. No remapping, no wear
  leveling, no bad blocks — appropriate for NOR and for simulation.
- Geometry comes from `flash_area_get_sectors` at runtime; nothing is
  hard-coded.

**What it does not do.** There is no wear leveling and no bad-block handling: a
block that wears out or arrives bad stays in the addressable set, and an
interrupted erase can leave the part needing external recovery. Wear is spread
*incidentally* by L1 — writes are append-only within a block, sequential ids
round-robin across all buckets, and only compaction erases — with the hot spot
in the two master blocks, which alternate (double-buffering halves the wear)
and are written only occasionally. That is mitigation, not management, which is
why it is no longer the default.

Keep this backend when the substrate is NOR with a modest write budget and the
23 KB of flash and the per-transaction cost of UBI are not worth paying, or
when a test needs to reach blob_db's own on-flash structures at known partition
offsets (`tests/lib/blob_db` pins it for exactly that reason).

## 4. Provider b) — a dynamic UBI volume (default)

`CONFIG_BLOB_DB_BACKEND_UBI` (`lib/blob_db/blob_db_store_ubi.c`), backed by the
`ubi` module from `west.yml`. **This is the default**, on the grounds that a
store which survives a torn write is worth more than a faster one.

- Each blob_db PEB maps 1:1 onto a **UBI LEB**; UBI maps LEBs to physical
  blocks through per-block headers and moves them for wear leveling and
  bad-block avoidance, transparently to L1.
- The volume is named `blobdb` and is created on first mount, sized from the
  device geometry (deterministically, so the block count is stable across
  boots as §2 requires). Four PEBs are held back as spares so erase/rewrite
  churn always has a free block to map into.
- `blob_db`'s three primitives map straight onto UBI's:
  `erase → ubi_leb_unmap` + dirty reclaim, `write → ubi_leb_write_at` (in
  place, no read-modify-write), `read → ubi_leb_read`.

**Geometry cost.** UBI spends 48 B of each block on its own headers and
reserves two blocks for device headers, on top of blob_db's four spares. On
`native_sim`'s 8 MB partition of 4 KB blocks that is 2048 × 4096 raw against
**2042 LEBs of 4048 B** presented upward — which is why a payload cap tuned for
a 4096 B block can be unreachable under UBI (`tests/lib/blob_db/testcase.yaml`
documents one such pair).

**Runtime cost**, measured on the nRF5340-DK (`app_perf/RESULTS.md`, "The UBI
backend"): reads 2.5× slower, updates 1.5×, +23 KB `.text`. The LEB→PEB
indirection costs ~112 µs per flash transaction and nothing per byte, so
transaction-heavy paths pay most; the sector erase that dominates writes is
unchanged.

**Configuration trap.** UBI's static memory backend sizes its block pool at
compile time from `CONFIG_UBI_MAX_NR_OF_DATA_PEBS`, whose default of 14 *builds
cleanly and then fails to attach at runtime*. Every board using this backend
must set it to `(partition_size / erase_block_size) -
CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS`. Every in-tree board file does; a new one
must too.

## 5. Cross-backend mounting

The two providers produce **incompatible on-flash layouts**, and the Kconfig
help says so. What happens when a build meets the other layout is asymmetric,
and only one direction is safe. Both behaviors below were observed on
`native_sim` by formatting a store with one backend and booting `app` on the
other:

| Build | Meets | Result |
|---|---|---|
| UBI | a `flash_area` store | **Clean refusal.** `ubi_device_init()` finds no valid device header and fails with `-EIO` before `blob_db` mounts at all. The partition is left byte-identical. |
| `flash_area` | a UBI store | **Destructive, by default.** UBI's block headers are not blob_db masters, so both master blocks classify as *corrupt*, and `CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT` (default `y`) reformats the partition. The UBI volume is gone. |

The second row is a gap against the L1 contract's D1 rule — "distinct on-flash
magic so a mismatched mount fails cleanly with `-ENOTSUP`". That rule works on
the *allocator* axis, where a foreign store still carries a parseable
compatibility prefix and is classified `FOREIGN`. It does not reach the
*backend* axis: a foreign substrate fails the prefix CRC first, so it lands in
the `CORRUPT` branch, which exists to recover from bit rot and cannot tell bit
rot from a different substrate.

Consequences, until this is closed:

- **Switching an existing device between backends requires erasing the
  partition deliberately** — do not rely on mount to refuse.
- **Production builds should set `CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT=n`**,
  which turns the destructive row into `-EIO` with nothing written. The
  option's help text already recommends this for production; the backend axis
  is a second, sharper reason.

Closing it properly means making a non-blob_db substrate distinguishable from a
corrupt blob_db master before the autoformat decision — for example a
backend-id byte in the compatibility prefix, so a mismatch classifies as
`FOREIGN` (never formatted) rather than `CORRUPT`. That is an L1 format change
and belongs to the L1 contract, not here; recorded as an open item in
`doc/impl/l1_bucketlog.md` §13.

## 6. Kconfig

L0 is selected through one choice, owned by L1's Kconfig
(`lib/blob_db/Kconfig`) because the seam is L1-internal:

```
choice BLOB_DB_BACKEND
    BLOB_DB_BACKEND_FLASH_AREA    raw partition via flash_area
    BLOB_DB_BACKEND_UBI           dynamic UBI volume        (default)
```

`BLOB_DB` selects `FLASH` and `FLASH_MAP`; `BLOB_DB_BACKEND_UBI` additionally
selects `UBI_ENABLE` and requires `CONFIG_UBI_MAX_NR_OF_DATA_PEBS` to be sized
per board (§4). The partition itself is named by
`CONFIG_BLOB_DB_PARTITION_LABEL` under either backend.

## 7. What an L0 provider must never do

- Cache writes in RAM and acknowledge before they are durable (breaks P7).
- Reorder a write after a later write's acknowledgment (breaks L1's master
  generation ordering).
- Let a torn operation corrupt a block other than the one being
  written/erased (§2).
- Report a different `peb_size` or `n_pebs` for the same store on a later boot
  without an explicit reformat story — L1 places ids by block count, so a
  geometry change silently relocates every blob.
