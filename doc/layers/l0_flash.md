# L0 — Flash Translation Layer

Status: v1 (interface fixed; UBI-like provider is future work)
· Part of the stack in `doc/architecture.md` · Governed by `doc/principles.md`

---

## 1. Role

L0 owns raw flash: erase blocks, write alignment, and — in its future form — wear
leveling, bad-block handling, and logical-to-physical block mapping. It presents
the layer above (L1, `blob_db`) with a **linear, sector-erasable partition**
through one fixed interface: Zephyr's `flash_area` API.

The interface is the design decision; the provider behind it is interchangeable.

```
┌──────────────────────────────────────────────┐
│ L1  blob_db                                  │
└──────────────────────────────────────────────┘
                 │  <zephyr/storage/flash_map.h>
                 │  flash_area_{open,close,read,write,erase}
                 │  flash_area_{get_size,get_sectors,align}
                 ▼
┌──────────────────────────────────────────────┐
│ L0 provider (one of):                        │
│   a) fixed DT partition on NOR / native_sim  │   ← today
│   b) UBI-like FTL exposing a virtual         │   ← future
│      flash_area over NAND                    │
└──────────────────────────────────────────────┘
```

## 2. The interface contract

L1 consumes **only** the following, and L0 providers must honor exactly this —
nothing more is ever assumed (principle P6):

| Primitive | Contract |
|---|---|
| `flash_area_open/close` | Partition resolved by label (`CONFIG_BLOB_DB_PARTITION_LABEL`); geometry stable for the mount's lifetime. |
| `flash_area_read` | Returns the last committed bytes at that offset. |
| `flash_area_write` | Write-block aligned (`flash_area_align`); a completed write is durable across power loss. |
| `flash_area_erase` | Returns whole sectors to the all-`0xff` state. |
| `flash_area_get_sectors` | Reports erase-block geometry; L1 sizes its buckets from it at runtime. |
| **Torn-write blast radius** | A power loss mid-write/mid-erase may leave arbitrary bytes **in the affected sector only**. Other sectors are untouched. |

The last row is the load-bearing one: L1's crash model (CRC-guarded slots,
double-buffered master, scratch-sector compaction — see
`doc/layers/l1_blob_db.md` §8) absorbs any single-sector corruption, but assumes
corruption never leaks across sector boundaries.

## 3. Provider a) — fixed partition (today, v1)

- A `fixed-partitions` child node in the device tree (label `storage`), mapped
  1:1 onto physical flash. On `native_sim`: 16 MB simulated flash, 8 MB
  partition at 0x800000 (`app/boards/native_sim.overlay`).
- 1 sector = 1 physical erase block. No remapping, no wear leveling, no bad
  blocks — appropriate for NOR and for simulation.
- Zero code in this repository: the provider is Zephyr's flash map over the
  platform flash driver. Enabled by `CONFIG_FLASH` + `CONFIG_FLASH_MAP`
  (selected by `CONFIG_BLOB_DB`).

Wear consideration without an FTL: L1 already spreads erases naturally — writes
are append-only per bucket, sequential ids round-robin across all 2045 buckets,
and only compaction erases. Hot-spot risk concentrates in the two master sectors,
which alternate (double-buffering halves the wear) and are written only
occasionally (id-hint persistence, compaction brackets).

## 4. Provider b) — UBI-like FTL (future)

A translation layer inspired by Linux UBI, for flash that needs management
(NAND, or NOR at high write volumes):

- **Logical erase blocks (LEBs)** presented upward as the sectors of a virtual
  `flash_area`; mapped to physical erase blocks (PEBs) via per-PEB headers.
- **Wear leveling** by moving cold LEBs onto worn PEBs.
- **Bad-block handling** by remapping LEBs away from failed PEBs (NAND).
- **Same contract as §2** at its upper edge — including the single-sector torn
  blast radius, which the FTL must preserve even though a logical write may
  internally involve a PEB copy.

Integration is a pointer swap, not a rewrite: `blob_db` gets aimed at the FTL's
virtual partition via `CONFIG_BLOB_DB_PARTITION_LABEL`. No source change above
L0 (see `doc/layers/l1_blob_db.md` §17).

The FTL's own design (PEB header format, atomic LEB move, erase-counter
persistence) will be specified in a dedicated document when scheduled:
`doc/layers/l0_ubi.md`.

## 5. Kconfig

Today L0 has no symbols of its own — L1 `select`s `FLASH` and `FLASH_MAP`, and
the partition is chosen by `CONFIG_BLOB_DB_PARTITION_LABEL`. The future FTL adds:

```
CONFIG_FLASH_UBI            bool "UBI-like flash translation layer"
CONFIG_FLASH_UBI_...        (spares %, wear-leveling threshold, …)
```

with the stack above unchanged: selecting the FTL only changes which partition
label L1 is pointed at.

## 6. What L0 must never do

- Cache writes in RAM and acknowledge before they are durable (breaks P7).
- Reorder a write after a later write's acknowledgment (breaks L1's master
  generation ordering).
- Let a torn operation corrupt a sector other than the one being written/erased.
- Change geometry (sector size/count) between mounts without an explicit
  reformat story.
