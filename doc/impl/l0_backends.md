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

`CONFIG_BLOB_DB_BACKEND_UBI`, `lib/blob_db/blob_db_store_ubi.c`, over the
[`zephyr-ubi`](https://github.com/kamil-kielbasa/zephyr-ubi) module, pinned
to release `v0.1.0` in `west.yml`.

Each `blob_db` block maps 1:1 onto a **UBI LEB**; UBI maps LEBs onto physical
blocks through per-block headers and moves them for wear leveling and
bad-block avoidance. The three primitives map directly:

| seam | UBI |
|---|---|
| `blob_db_store_erase(peb)` | `ubi_leb_erase(lnum)`, then up to `CONFIG_BLOB_DB_UBI_RELOCATE_BUDGET` wear-leveling moves |
| `blob_db_store_write(peb, off, …)` | `ubi_leb_write_at(lnum, off, …)`, in place |
| `blob_db_store_read(peb, off, …)` | `ubi_leb_read(lnum, off, …)`; an unmapped LEB reads as erased |

The in-place append is the reason UBI is attached at its own API rather than
behind a synthesized `flash_area` (`doc/layers/l0_flash.md` §1.1): blob_db's
write path appends a slot record at a growing offset inside an otherwise-erased
block, which is precisely `ubi_leb_write_at()` — no read-modify-write, no
whole-block rewrite. UBI requires those appends to rise within a block and to
be whole write blocks; blob_db's forward-only writes (`blob_db_internal.h`,
the scratch seal) and `write_align` already guarantee both.

**Erase is `ubi_leb_erase()`, not `ubi_leb_unmap()`.** An unmap writes
nothing: until a reclaim erases the block, the next attach maps it back with
the contents it had. blob_db's recovery reasons about erases that have
happened — compaction erases the scratch block to retire a sealed image — so
the backend asks for the durable erase, at one flash erase per call, which is
what the raw partition charges too.

**Volume.** A volume named `blobdb`, created on first mount from the device's
free LEBs less four spares (`BLOB_DB_UBI_SPARE_LEBS`), and found again by
name (`ubi_volume_find()`) afterwards. Its LEB count is read back from UBI
rather than re-derived, so it is identical on every boot — the contract's
geometry-stability requirement. The spares give relocation a block to move
onto and absorb blocks retired after a failed write, so neither turns into
`-ENOSPC` once every LEB is mapped.

**Geometry overhead.** UBI spends 128 B of each block on its two 64 B headers
and holds three blocks back for its volume table (two copies and a spare for
their updates), on top of blob_db's four spares. Measured on `native_sim`'s
8 MB partition of 4 KB blocks:

| | raw partition | through UBI |
|---|---|---|
| blocks | 2048 | 2041 |
| usable bytes per block | 4096 | 3968 |
| blob_db buckets | 2045 | 2038 |

The shrunken block is not cosmetic: a payload cap tuned for a 4096 B block can
become unreachable under UBI, and mount then refuses with `-ENOTSUP`. The
`LARGE_PAYLOADS` + UBI pair on 4 KB geometry is one such case, worked through
in `tests/lib/blob_db/testcase.yaml`.

**Authenticated metadata.** UBI seals its headers and volume table with
AES-CMAC under keys derived (HKDF-SHA256) from a PSA key handle, so the
backend selects `PSA_CRYPTO` (Mbed TLS, or TF-M where the build has it).
`CONFIG_BLOB_DB_UBI_KEY` picks where the key comes from:

- `BLOB_DB_UBI_KEY_BUILTIN` (default) — a fixed key compiled into blob_db.
  The MAC then catches damage, not tampering: anyone can re-seal a forged
  header. It matches what the previous, unauthenticated UBI backend offered.
- `BLOB_DB_UBI_KEY_APP` — the application defines `blob_db_ubi_ikm_key()`
  (`<app/lib/blob_db_ubi.h>`) and returns a per-device key it provisioned.

UBI also consults a state callback at attach and every
`CONFIG_UBI_STATE_CHECK_INTERVAL` writes, the place for rollback detection.
blob_db forwards it to `blob_db_ubi_state_check()`, whose weak default trusts
every device; an application with a trusted store (PSA ITS, a monotonic
counter) overrides it. A refusal fails the mount, or every later write, with
`-EROFS`. Application data is neither encrypted nor authenticated either way.

**Maintenance.** zephyr-ubi has no background thread. A write that finds no
free block erases one itself, and `ubi_leb_erase()` hands its block straight
back to the free pool, so blob_db needs no reclaim. Two operations are run on
blob_db's behalf:

- `UBI_MAINTENANCE_REPAIR` at every mount, which restores a degraded volume
  table and gives retired blocks another chance;
- `UBI_MAINTENANCE_RELOCATE` after each bucket erase, up to
  `CONFIG_BLOB_DB_UBI_RELOCATE_BUDGET` (default 1) moves. It moves rarely
  rewritten data off little-worn blocks — the masters and cold buckets — which
  the previous UBI backend never did. Most erases find nothing past
  `CONFIG_UBI_WEAR_LEVELING_THRESHOLD`; one that does pays a block copy and an
  extra erase. 0 turns it off.

Blocks UBI keeps as `CORRUPT` (damage behind a valid erase-counter header) are
reported at mount but never discarded by blob_db: they may hold the only copy
of something, and UBI refuses to attach once they reach a twentieth of the
device.

**Runtime cost**, nRF5340-DK, `app_perf/RESULTS.md` "The UBI backend": reads
2.5× slower, updates 1.5×, +23 KB `.text` — measured on the previous UBI
release and not yet re-measured on this one. The LEB→PEB indirection costs
per flash transaction and nothing per byte, so transaction-heavy paths pay
most; the sector erase that dominates writes is unchanged. Linking PSA Crypto
raises the image further: `app` for the nRF5340-DK grows by 41 KB of flash
over the `flash_area` build (88 KB against 47 KB, Mbed TLS included; Arm GNU
toolchain 13.2, so indicative rather than a release figure).

**Per-board configuration.** zephyr-ubi takes its handle, a scratch buffer
and 8 B per erase block from the system heap at attach, and a second handle
and scratch buffer during a format. `CONFIG_HEAP_MEM_POOL_ADD_SIZE_BLOB_DB_UBI`
reserves that, defaulting to the storage partition's size in 4 KB blocks
(`size / 4096 × 8`) plus 4 KB — 20 KB for an 8 MB partition. That covers any
NOR geometry, so a new board needs no setting to work; one with larger blocks
may lower it, as the DK board files do (6 KB for 128 blocks of 64 KB). Too
little shows up as `-ENOMEM` from `blob_db_mount()`, not at build time.

## 4. Cross-backend mounting

The two layouts are incompatible (`doc/layers/l0_flash.md` §4). What happens
when a build meets the other one is **asymmetric**, and only one direction is
safe. Both rows below were observed on `native_sim` by formatting a store with
one backend and booting `app` on the other (the UBI row also with a store
written by the previous UBI backend, `wmilek/ubi`):

| Build | Meets | Result |
|---|---|---|
| UBI | a `flash_area` store, or one the previous UBI backend wrote | **Clean refusal.** `ubi_device_init()` finds no UBI device it can read and returns `-ENODEV`. The partition is not blank, so the backend refuses with `-ENOTSUP` and `blob_db_mount()` never proceeds. The partition is left byte-identical; `blob_db_format()` discards it deliberately. |
| `flash_area` | a UBI store | **Destructive.** Both master blocks classify as *corrupt*, and `CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT` (default `y`) reformats the partition. The UBI volume is gone. |

zephyr-ubi answers `-ENODEV` for a blank partition and for one holding
someone else's bytes alike, and documents `-ENODEV` as the one error that may
be met with a format. The backend tells the two apart itself — mount formats
only a partition that reads erased end to end, a scan that runs only on that
error path — and passes the rest to `blob_db_format()`, which tells the backend
it is discarding the store (`blob_db_store_open(…, discard)`). Errors that say
nothing about what the partition holds (a flash or crypto failure, no heap, a
refused state check) are returned on either path. `tests/lib/blob_db/src/ubi.c`
covers both rows of that decision.

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
    BLOB_DB_BACKEND_UBI      (default)        # selects UBI, PSA_CRYPTO
        choice BLOB_DB_UBI_KEY
            BLOB_DB_UBI_KEY_BUILTIN  (default)   # fixed development key
            BLOB_DB_UBI_KEY_APP                  # blob_db_ubi_ikm_key()
        BLOB_DB_UBI_RELOCATE_BUDGET  (1)          # wear-leveling moves per erase
        HEAP_MEM_POOL_ADD_SIZE_BLOB_DB_UBI        # heap for UBI, from the partition size
```

`BLOB_DB` selects `FLASH` and `FLASH_MAP` for either provider;
`BLOB_DB_BACKEND_UBI` additionally selects `UBI` and `PSA_CRYPTO`. UBI's own
options (`CONFIG_UBI_*` — wear-leveling threshold, verify-on-read, header
invalidation before erase, …) keep zephyr-ubi's defaults (§3).

## 6. Coverage

- `tests/lib/blob_db` runs its suite on both backends: three scenarios pin
  `flash_area` (raw-offset fault injection), and `lib.blob_db.ubi` runs the
  shipped default with those cases skipping themselves. Its `blob_db_ubi`
  suite covers the backend's own decisions (§4), and
  `lib.blob_db.ubi.app_key` adds the application hooks: a wrong key refused
  with `-EBADMSG` and the store intact, a refused state check at mount and at
  run time.
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
3. **UBI throughput not re-measured.** `app_perf/RESULTS.md` predates the
   move to zephyr-ubi v0.1.0, whose per-transaction cost, header size and
   wear-leveling moves differ from the release it measured.
4. **`-Werror` in zephyr-ubi.** The module compiles its sources with its own
   warning set as errors (`cmake/warnings.cmake`), so a new warning from a
   Zephyr header on `main` fails the build there first.
