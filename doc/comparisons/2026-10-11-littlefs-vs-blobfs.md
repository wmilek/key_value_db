# Comparison — littlefs vs blobfs

Status: **measured** · 2026-10-11 · Zephyr `main` @ `bbc6385f0`, littlefs
`e9a8638` (the revision Zephyr pins, LittleFS 2.11 / disk 2.1)
· Harness: `doc/comparisons/fs_compare/` (how to re-run every number here)
· Timing model: `app_perf_l0/models/mx25r64_nrf5340dk_direct.json`
· Related: `doc/layers/l3_interfaces.md` §4, `app_cbor_persondb/FINDINGS.md`

**The question.** A device needs files. Should they live in **blobfs**, on the
blob_db stack, or in **littlefs**, Zephyr's standard flash file system?

**The assumption this comparison is built on.** The device already runs the
blob_db stack — UBI volume, blob_db, rootreg, kvhash, kvdb — as its
**multi-purpose** store, because it wants key/value storage anyway. So blobfs
is an extra interface on a stack that is already paid for. littlefs is the
**larger, standalone** alternative: its own code, its own partition, its own
wear leveling.

**Short answer.**

- **Small files (up to `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN`, 256 B by default),
  blob stack already present: blobfs.** Files then cost +10.1 KiB ROM and
  +4.3 KiB RAM, against +23.7 KiB / +5.5 KiB and a second partition for littlefs.
  On the DK's flash, blobfs's steady-state rewrite of a small file is predicted
  at about 2–3× less flash time than littlefs tuned for small files, and about
  45× less than littlefs on Zephyr's defaults. Reads and listings are 3–25×
  cheaper. Both stacks then share one wear-leveled, bad-block-managed pool.
- **littlefs where blobfs cannot do the job.** That covers files larger than
  one blob payload, appends to a growing log, `statvfs`, rename across
  directories or onto an existing name, sorted listings, and host tooling.
  blobfs refuses a 4 KB file outright (`-ENOSPC`). These are v1 limits,
  documented in `l3_interfaces.md` §4.1.
- **If files are the only reason to have the blob stack, use littlefs.** The
  whole blob stack adds 64.2 KiB ROM and 148.8 KiB RAM over a bare image.
  littlefs plus Zephyr `settings` covers both files and key/value for
  29.5 KiB ROM and 5.6 KiB RAM. The RAM gap is blob_db's two sector buffers
  (FINDINGS B6): 64 KiB each on this flash.
- **littlefs on Zephyr defaults is the wrong baseline on this flash.** With
  64 KB erase blocks and a 64 B cache, every file above 64 B occupies its own
  64 KB block, and every rewrite of it erases one. Any littlefs deployment here
  needs the cache raised (`lfs_tuned` below) or 4 KB layout pages.

---

## 1. Setup

The harness is one application. It runs the same `fs_*` calls against
whichever file system its variant fragment enables:

| Variant | Contents |
|---|---|
| `base` | kernel, QSPI flash driver, harness — no storage |
| `lfs` | littlefs, Zephyr defaults (read/prog 16 B, cache 64 B, lookahead 32 B) |
| `lfs_tuned` | `lfs` with `FS_LITTLEFS_CACHE_SIZE=512`, so files ≤ 512 B are stored inline in directory metadata |
| `lfs_settings` | `lfs` + Zephyr `settings` (file backend on the same littlefs) |
| `kvdb_ubi` | blob_db on UBI + rootreg + kvhash + kvdb — the multi-purpose stack |
| `blobfs_ubi` | `kvdb_ubi` + blobfs + VFS driver |
| `blobfs_ubi_ar` | `blobfs_ubi` + `BLOB_DB_UBI_ATOMIC_REPLACE` (compaction = 1 erase, not 5) |
| `blobfs_fa` | `blobfs_ubi` on the raw partition (no UBI, no PSA Crypto) |
| `lfs_kvdb_ubi` | `kvdb_ubi` for key/value + littlefs on a second partition for files |

**Geometry.** The DK's 8 MB MX25R64 is split into two 4 MB partitions, one per
stack. Erase blocks are 64 KB (`NORDIC_QSPI_NOR_FLASH_LAYOUT_PAGE_SIZE`
default) and write alignment is 4 B. `native_sim` is given the same shape, as
`app_perf_l0/geometry/mx25r64.overlay` does. blob_db keeps its default
`MAX_PAYLOAD_LEN` = 256 B.

**Workload** (run 1 on erased flash, run 2 a cold boot onto the result):

| Phase | Operations |
|---|---|
| `create` | 32 files × 128 B |
| `rewrite` | 256 overwrites of 128 B (each: open, write at 0, close) |
| `append` | 15 appends of 16 B to one log file |
| `read` | read back all 33 files |
| `dir` | mkdir, 8 files in it, one rename, 4 unlinks |
| `list` | readdir of the root (34 entries) |
| `churn` | 32 768 more overwrites — steady state, compaction included |
| `big_file` | one 4 KB file |
| run 2: `fs_mount`, `first_write`, `boot_read` | cold boot, first write after it, read everything back |

**Measurement.** Flash operations are counted by the flash simulator. That sits
below both stacks, so neither is measured by its own instrumentation. Flash
time on the DK is predicted from those counts with the repo's L0 model, fitted
on this board. Per operation, the model charges a QSPI read 66 µs + 0.26 µs/B
and a 64 KB erase about 1.1 s. CPU time is not modelled (§6).

## 2. Footprint (nRF5340, Cortex-M33, `-Os`)

Whole image, with each stack's share taken from `rom_report` / `ram_report`
(KiB):

| Variant | ROM | RAM | Storage share of ROM | Storage share of RAM |
|---|--:|--:|---|---|
| `base` | 29.1 KiB | 19.5 KiB | — | — |
| `lfs` | 55.7 KiB | 25.1 KiB | littlefs 16.6 + VFS 2.3 | caches + file heap |
| `lfs_tuned` | 55.7 KiB | 27.7 KiB | same | +2.6 KiB of cache |
| `lfs_settings` | 58.7 KiB | 25.2 KiB | + settings 2.7 | — |
| `kvdb_ubi` | 83.2 KiB | 164.1 KiB | blob_db 5.7 · UBI 13.2 · PSA/Mbed TLS 14.6 · kvhash 4.6 · kvdb 0.5 · rootreg 0.5 | **sector buffers 128.0** · PSA 9.8 · UBI heap 6.1 · kvhash 0.5 |
| `blobfs_ubi` | 93.4 KiB | 168.4 KiB | + blobfs 4.5 + VFS 2.3 | + handle pools |
| `blobfs_fa` | 54.6 KiB | 152.4 KiB | no UBI, no PSA | sector buffers 128.0 |
| `lfs_kvdb_ubi` | 106.9 KiB | 169.6 KiB | both stacks | both |

What it costs to add **files** to a device:

| Starting point | Add | ROM | RAM | Flash |
|---|---|--:|--:|---|
| `kvdb_ubi` (multi-purpose stack present) | blobfs | **+10.1 KiB** | **+4.3 KiB** | shares the UBI volume |
| `kvdb_ubi` | littlefs | +23.7 KiB | +5.5 KiB | needs its own partition |
| `base` (nothing) | littlefs | +26.6 KiB | +5.6 KiB | one partition |
| `base` | littlefs + settings (files + key/value) | +29.5 KiB | +5.6 KiB | one partition |
| `base` | the whole blob stack + blobfs | +64.2 KiB | +148.8 KiB | one partition |

Under the multi-purpose assumption, blobfs is the cheaper way to add files. Of
the 10.1 KiB it adds, 4.5 KiB is blobfs itself and 2.3 KiB is Zephyr's VFS
core, which littlefs needs too. The remaining 3.3 KiB is the harness's own file
workload, which is compiled only when a file system is present and so is in
the littlefs figures too; the kernel and libc code it and the VFS pull in;
and 0.4 KiB of blob_db pread/pwrite paths that kvdb alone does not link.

The blob stack's RAM is fixed by the flash, not by the workload.
`g_bbuf` + `g_bbuf_new` are two erase blocks of `.bss`: 128 KiB on 64 KB blocks,
8 KiB on 4 KB blocks (FINDINGS B6, still open). littlefs's RAM is
`3 × cache + lookahead + per-file caches` and does not grow with the erase
block. On an MCU with less RAM than the nRF5340's 512 KB, that difference
alone decides the question.

## 3. Flash cost (predicted DK flash time)

### 3.1 Steady state — what a device spends its life doing

`churn`: 32 768 rewrites of a 128 B file, round-robin over 32 files. User
payload written: 4.19 MB.

| | flash time / rewrite | reads / rewrite | erases (64 KB) | bytes erased / byte written by the app | write amplification |
|---|--:|--:|--:|--:|--:|
| `lfs` (defaults) | **1 203 ms** | 1 156 | 32 784 | 512 | 1.25 |
| `lfs_tuned` | 62.7 ms | 695 | 86 | 1.3 | 1.35 |
| `blobfs_ubi` | 27.0 ms | 237 | 257 | 4.0 | 1.14 |
| `blobfs_ubi_ar` | **20.5 ms** | 237 | 64 | 1.0 | 1.13 |
| `blobfs_fa` | 29.1 ms | 236 | 320 | 5.0 | 1.13 |

- **littlefs on defaults erases a 64 KB block for every rewrite.** littlefs
  inlines a file only up to `min(cache_size, block_size / 8)` = 64 B. A 128 B
  file therefore gets its own block, and copy-on-write means a fresh block per
  rewrite. Of the 64 blocks, 38 hold 6 KB of data (`statvfs`: 26 free).
  `lfs_tuned` stores the same files in 5 blocks.
- **With erases under control, reads dominate both stacks.** A QSPI read costs
  66 µs before the first byte arrives. Tuned littlefs re-reads its directory's
  metadata log on every open: 46 KB in 695 calls per rewrite once the 64 KB
  metadata block has filled. blob_db walks slot headers inside one bucket:
  3.2 KB in 237 calls. Both costs grow with how full the block is. In
  `rewrite`, run on fresh blocks, they were 253 and 31 reads per op.
  Zephyr does not expose littlefs's `metadata_max`, which is the knob that
  bounds this.
- **Compaction protocol matters more than UBI.** The scratch-sector protocol
  erases 4–5 blocks per compaction. Atomic replace erases 1, bringing blobfs's
  erase traffic level with tuned littlefs. Running UBI instead of the raw
  partition costs nothing per op here (27.0 vs 29.1 ms).

### 3.2 The rest of the workload

| Phase | `lfs` | `lfs_tuned` | `blobfs_ubi` | Note |
|---|--:|--:|--:|---|
| format + mount, erased flash | 2.2 s | 2.2 s | 12.2 s + 2.2 s | UBI formats the volume (9 erases); one-time |
| `create` 32 files, fresh store | 35.8 s | 0.15 s | 44.9 s | blob_db erases a bucket on first touch (40 here; FINDINGS B7) |
| `rewrite` 256, fresh blocks | 290 s | 6.9 s | 1.0 s | |
| `append` 15 × 16 B | 12.5 s | 0.32 s | 1.2 s | blobfs rewrites the whole body per append |
| `read` 33 files, fresh | 0.65 s | 1.07 s | 0.08 s | |
| `read` 33 files after churn (run 2) | 1.17 s | 1.21 s | 0.35 s | |
| `dir` (mkdir, 8 files, rename, 4 unlinks) | 1.4 s | 1.8 s | 9.1 s | 8 first-touch bucket erases |
| `list` 34 entries | 1.6 s | 2.3 s | 0.09 s | |
| `big_file` 4 KB | 1.2 s | 1.2 s | **`-ENOSPC`** | over the 256 B body cap |

blob_db's first touch of a bucket costs a 64 KB erase, about 1.1 s. That
happens once per bucket over the life of a store, and `blob_db_prepare()` can
pay it ahead of time. On a fresh store it makes blobfs's first `create` and
`mkdir` the slowest operations in the table. On an aged store they cost what a
rewrite costs.

### 3.3 Cold boot

| | mount | first write after mount | boot to first durable write |
|---|--:|--:|--:|
| `lfs` | 235 ms | 1 567 ms | 1.80 s |
| `lfs_tuned` | 191 ms | 41 ms | **0.23 s** |
| `blobfs_ubi` | 590 ms (UBI attach + blob_db, which brings kvdb up too) + 2 ms blobfs | 13 ms | 0.61 s |
| `blobfs_fa` | 574 ms + 2 ms | 13 ms | 0.59 s |

littlefs's first write after a mount pays for the allocator's lookahead scan
of the file system: 6 326 reads on defaults, 324 tuned. blob_db's mount is not
a rebuild (README), but it does read slot headers: 8.5 k reads, about the same
on UBI and on the raw partition. In this multi-purpose setup that 0.59 s is
spent at boot anyway, for kvdb. The marginal cost of mounting blobfs is 2 ms.

## 4. Semantics

| | littlefs | blobfs (v1) |
|---|---|---|
| Largest file | 2 GiB | one blob payload: `MAX_PAYLOAD_LEN`, default 256 B, at most ~32 KB on 64 KB blocks / ~2 KB on 4 KB blocks. Raising it also raises kvhash's RAM (2 × payload, FINDINGS K7). Segmented objects are not wired into blobfs yet |
| Write durability | at `fs_sync` / `fs_close` (copy-on-write; an open file is a draft) | **every `fs_write` is crash-atomic and durable when it returns**; `sync` is a no-op |
| Write cost | proportional to bytes written (CTZ skip-list, inline when small) | proportional to **file size**: each write rewrites the whole body |
| Name length | 255 | `BLOBFS_MAX_NAME_LEN`, default 12, up to 255 |
| Rename | anywhere, atomic, replaces an existing target | within one directory, atomic via a rename intent; `-EXDEV` across directories; refuses an existing target |
| Listing order | alphabetical | hash order (kvhash), stable for one listing; sorted waits for kvtree |
| `statvfs` | yes | `-ENOTSUP` (blob_db has no free-space accounting; FINDINGS B3) |
| Open files | `FS_LITTLEFS_NUM_FILES`, each with its own cache | `BLOBFS_FS_MAX_OPEN_FILES`, no per-file RAM beyond the handle; handles follow renames and fail cleanly after unlink |
| Conformance | Zephyr `tests/subsys/fs` (with littlefs's per-FS bypasses) | same `test_fs_basic` / `test_fs_open_flags` bodies, no bypasses; `test_fs_dirops` not yet (needs `statvfs` and cross-directory rename) |
| Host tooling | mklittlefs, littlefs-python, FUSE; images can be built offline | none; `blob_db` shell commands on target |

## 5. Robustness and flash management

| | littlefs | blobfs on the blob stack |
|---|---|---|
| Power-loss safety | yes: COW + metadata-pair commits with CRC | yes: every L1 op crash-atomic, rename intent rolled forward or back at mount, crash-injection tests at every mutation step |
| Wear leveling | dynamic, within its own partition (`BLOCK_CYCLES`); static data is never moved | UBI: across the **whole volume, shared with kvdb**, including relocation of cold blocks |
| Bad blocks | detected when a program fails verification and worked around by relocating; no bad-block table, no spares | UBI retires failed blocks onto spares |
| Metadata integrity | CRC | CRC + UBI AES-CMAC (detects damage with the built-in key; tampering only with a provisioned key) |
| Space sharing with key/value | fixed partition split, chosen at design time | one pool: files and keys draw from the same volume |
| Maturity | widely deployed, stable on-disk format | this repository's v1; on-flash format versioned, open findings tracked in `app_cbor_persondb/FINDINGS.md` |

## 6. Caveats

- **Flash time only.** CPU time between flash calls is not modelled. littlefs
  parses its metadata in CPU, and blob_db CRCs and memcpys slots. On this QSPI
  part the 66 µs per-read floor dominates. On fast internal flash the ranking
  of reads could change, and only a board run settles it
  (`l0_timing.py verify`).
- **One geometry.** Everything above is on 64 KB erase blocks, the DK's
  default. With 4 KB layout pages, littlefs's default configuration is far
  less pathological, and blob_db's RAM drops to 8 KiB of sector buffers. The
  timing model is fitted for 64 KB block erases only, so a 4 KB comparison
  needs its own board run.
- **littlefs read size was left at 16 B.** Raising `FS_LITTLEFS_READ_SIZE`
  cuts read calls, at the cost of RAM and of reading bytes it does not need.
  Like `metadata_max`, it is a lever that was not swept here.
- **Toolchain.** ARM sizes come from Debian `arm-none-eabi-gcc` 13.2 with
  minimal libc, not the Zephyr SDK. Absolute numbers will shift slightly, but
  the differences between variants are what the conclusions rest on.
- **`lfs_settings`** is a footprint data point only. The cost of the settings
  file backend (append + periodic compaction) was not measured.
