# Implementation design — L0 storage backends

Status: v1 · **Non-normative implementation design.** This document describes
how the two `blob_db_store` providers satisfy the L0 contract
(`doc/layers/l0_flash.md`), and what they cost. Everything here — file layout,
operation mapping, measured numbers, per-board configuration — may change as
long as the contract holds. Upper layers must not depend on anything in this
document (P6).

---

## 1. The seam

`blob_db` addresses storage as a flat byte offset into an array of
equally-sized blocks (`peb_index * peb_size + within`). The seam
(`lib/blob_db/blob_db_store.h`) is deliberately small — open/close/read/write/
erase plus a geometry struct — and carries one structural guarantee that lets
providers stay simple: **a byte range passed to read/write/erase never crosses
a block boundary**, because `blob_db` operates one bucket, master or scratch
block at a time. A provider may therefore translate an offset as
`peb = off / peb_size`, `within = off % peb_size` without handling spans.

I/O accounting (`CONFIG_BLOB_DB_IOSTATS`) is counted at this seam, so it covers
both providers and every caller uniformly. Note the consequence for UBI: the
counters sit *above* UBI's own header traffic and therefore undercount actual
flash operations there (`app_perf/RESULTS.md`).

## 2. Provider a) — `flash_area`

`CONFIG_BLOB_DB_BACKEND_FLASH_AREA`, `lib/blob_db/blob_db_store_flash.c`
(~100 lines, all of it translation).

- Opens the `fixed-partitions` device-tree node labelled
  `CONFIG_BLOB_DB_PARTITION_LABEL` (default `storage`) and reports its geometry
  from `flash_area_get_sectors`. Nothing is hard-coded; on `native_sim` that is
  an 8 MB partition of 4 KB blocks (2048 blocks), and on the nRF5340-DK the
  MX25R64's 64 KB blocks.
- One block = one physical erase block, whole and unshared. Offsets are
  physical, which is what lets `tests/lib/blob_db` inject faults at known
  partition offsets — three of its four scenarios pin this backend for exactly
  that reason.
- Erase and write map straight onto `flash_area_erase` / `flash_area_write`.

**Wear.** There is no management: a block that wears out or arrives bad stays
in the addressable set, and an interrupted erase can leave the part needing
external recovery. Wear is spread only *incidentally* by L1 — writes append
within a block, sequential ids round-robin across all buckets, and only
compaction erases — with the hot spot in the two master blocks, which alternate
(double-buffering halves the wear) and are written only occasionally. That is
mitigation, not management, which is why this is no longer the default.

## 3. Provider b) — UBI (default)

`CONFIG_BLOB_DB_BACKEND_UBI`, `lib/blob_db/blob_db_store_ubi.c`, over the `ubi`
module from `west.yml`.

Each `blob_db` block maps 1:1 onto a **UBI LEB**; UBI maps LEBs onto physical
blocks through per-block headers and moves them for wear leveling and
bad-block avoidance. The three primitives map directly:

| seam | UBI |
|---|---|
| `blob_db_store_erase(peb)` | `ubi_leb_unmap(lnum)` + dirty reclaim |
| `blob_db_store_write(peb, off, …)` | `ubi_leb_write_at(lnum, off, …)`, in place |
| `blob_db_store_read(peb, off, …)` | `ubi_leb_read(lnum, off, …)` |

The in-place append is the reason UBI is attached at its own API rather than
behind a synthesized `flash_area` (`doc/layers/l0_flash.md` §1.1): blob_db's
write path appends a slot record at a growing offset inside an otherwise-erased
block, which is precisely `ubi_leb_write_at()` — no read-modify-write, no
whole-block rewrite.

**Volume.** A dynamic volume named `blobdb`, created on first mount and
re-attached afterwards by probing volume ids for that name. Its LEB count is
derived from device geometry rather than stored, so it is identical on every
boot — the contract's geometry-stability requirement. Four blocks are held back
as spares (`BLOB_DB_UBI_SPARE_PEBS`) so erase/rewrite churn always has a free
block to map into after an unmapped one goes to the dirty pool.

**Geometry overhead.** UBI spends 48 B of each block on its own headers and
reserves two blocks for device headers, on top of blob_db's four spares.
Measured on `native_sim`'s 8 MB partition of 4 KB blocks:

| | raw partition | through UBI |
|---|---|---|
| blocks | 2048 | 2042 |
| usable bytes per block | 4096 | 4048 |
| blob_db buckets | 2045 | 2039 |

The shrunken block is not cosmetic: a payload cap tuned for a 4096 B block can
become unreachable under UBI, and mount then refuses with `-ENOTSUP`. The
`LARGE_PAYLOADS` + UBI pair on 4 KB geometry is one such case, worked through
in `tests/lib/blob_db/testcase.yaml`.

**Runtime cost**, nRF5340-DK, `app_perf/RESULTS.md` "The UBI backend": reads
2.5× slower, updates 1.5×, +23 KB `.text`. The LEB→PEB indirection costs
~112 µs per flash transaction and nothing per byte, so transaction-heavy paths
pay most; the sector erase that dominates writes is unchanged.

**Per-board configuration.** UBI's static memory backend sizes its block pool
at compile time from `CONFIG_UBI_MAX_NR_OF_DATA_PEBS`, whose default of 14
*builds cleanly and then fails to attach at runtime*. Every board using this
backend must set it to `(partition_size / erase_block_size) -
CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS`. Every in-tree board file does; a new one
must too. The pool costs 16 bytes per block, so it is sized to the geometry
rather than to the maximum.

## 4. Cross-backend mounting

The two layouts are incompatible (`doc/layers/l0_flash.md` §4). What happens
when a build meets the other one is **asymmetric**, and only one direction is
safe. Both rows below were observed on `native_sim` by formatting a store with
one backend and booting `app` on the other:

| Build | Meets | Result |
|---|---|---|
| UBI | a `flash_area` store | **Clean refusal.** `ubi_device_init()` finds no valid device header, fails with `-EIO`, and `blob_db_mount()` never proceeds. The partition is left byte-identical. |
| `flash_area` | a UBI store | **Destructive.** Both master blocks classify as *corrupt*, and `CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT` (default `y`) reformats the partition. The UBI volume is gone. |

The second row happens because detection order works against us. A master is
classified by checking the frozen compatibility prefix's CRC *before* comparing
its magic (`l1_bucketlog.md` §3.1). UBI's block headers are not blob_db masters
and fail that CRC, so they never reach the magic comparison that would have
classified them `FOREIGN` — they land in `CORRUPT`, the branch that exists to
recover from bit rot and cannot tell bit rot from a different substrate.

So D1's rule — distinct on-flash magic, mismatched mount fails cleanly with
`-ENOTSUP` — holds on the *allocator* axis, where a foreign store still carries
a parseable prefix, but does not reach the *backend* axis the seam introduced.

Until it is closed:

- switching an existing device between backends requires erasing the partition
  deliberately — do not rely on mount to refuse;
- production builds should set `CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT=n`, which
  turns the destructive row into `-EIO` with nothing written.

The candidate fix — a backend-id byte inside the frozen prefix, so a substrate
mismatch classifies `FOREIGN` and is never formatted — is an L1 format change
and is tracked as `doc/impl/l1_bucketlog.md` §13.7.

## 5. Kconfig

```
choice BLOB_DB_BACKEND                        # in lib/blob_db/Kconfig
    BLOB_DB_BACKEND_FLASH_AREA                # raw partition
    BLOB_DB_BACKEND_UBI      (default)        # selects UBI_ENABLE
```

`BLOB_DB` selects `FLASH` and `FLASH_MAP` for either provider;
`BLOB_DB_BACKEND_UBI` additionally selects `UBI_ENABLE` and requires
`CONFIG_UBI_MAX_NR_OF_DATA_PEBS` per board (§3).

## 6. Coverage

- `tests/lib/blob_db` runs its suite on both backends: three scenarios pin
  `flash_area` (raw-offset fault injection), and `lib.blob_db.ubi` runs the
  shipped default with those cases skipping themselves.
- Every other suite (`blob_db_contract`, `kvdb`, `rootreg`) runs on the
  default, i.e. UBI.
- CI builds `app` on both backends on both targets, and `app_perf` and
  `app_perf_l0` on the DK so the benchmark binaries cannot rot.
- `app_perf_l0` measures the `flash_area` provider directly — no blob_db in
  the image — sweeping transfer size and erase span. The cost model fitted
  from it (`app_perf_l0/tools/l0_timing.py`) converts the seam's I/O counters
  into predicted wall-clock, which is what makes a `native_sim` run at the
  target's geometry a statement about the target.

## 7. Open items

1. **Foreign substrate classifies as `CORRUPT`** — §4; tracked in
   `l1_bucketlog.md` §13.7.
2. **`blob_db_iostats` undercounts on UBI** — the counters instrument the
   blob_db→store seam, above UBI's own header reads (§1). Either document the
   figure as blob_db-level only (it is, today) or push accounting into the
   provider. This now costs more than an inaccurate ratio: the L0 timing model
   consumes those counters, so a predicted time for a UBI build is a lower
   bound by exactly the traffic they miss.
3. **The `ubi` module tracks a fork branch** — `feature/leb-partial-update`,
   for `ubi_leb_write_at()`, which is still pending upstream. `west.yml`
   records the condition for moving back to a release tag.
