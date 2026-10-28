# app_footprint — results

FLASH (ROM) cost of the storage stack on `nrf5340dk/nrf5340/cpuapp`, measured
2026-10-26 with Zephyr SDK 1.0.1 (`arm-zephyr-eabi`, default `-Os`) at
key_value_db `501bd3f9f6a6` (plus this change), Zephyr `c4aa9324508f`, zephyr-ubi `d66b599af9ef`. Method
and how to re-run: [`README.md`](README.md). Raw data:
[`results/footprint.json`](results/footprint.json). The same data as an
interactive page is `kvdb_book/tools/stack-footprint/`.

All figures are bytes of FLASH: code, read-only data and RAM initialisers, the
linker's "FLASH used"; KB below is 1024 bytes. Each row's components sum to that total exactly. RAM is
in the JSON but is **not quoted here**: buffer sizing is not final.

## 1. The ladder

Each row adds one fragment from `stages/` to the row above (`p0` is
`app_cbor_persondb` as shipped). Columns after Δ are the same image split by
component; "Zephyr base" is kernel, arch/SoC/HAL, board drivers, libc, Zephyr
libs, CRC/random and linker data. It grows along the ladder because each layer
pulls in kernel and library code of its own (heap, mutexes, cbprintf, ...).

| # | Build | Kconfig added | FLASH | Δ | Zephyr base | Logging | Flash drv | PSA Crypto | UBI | blob_db | rootreg | containers | kvdb+blobfs | app |
|---|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| s00 | Zephyr base | `—` | 22 568 |  | 22 521 |  |  |  |  |  |  |  |  | 47 |
| s01 | + logging | `CONFIG_LOG=y` | 31 244 | +8 676 | 27 218 | 3 956 |  |  |  |  |  |  |  | 70 |
| s02 | + flash driver | `CONFIG_FLASH=y CONFIG_FLASH_MAP=y` | 37 328 | +6 084 | 29 420 | 3 981 | 3 748 |  |  |  |  |  |  | 179 |
| s03 | + UBI | `CONFIG_UBI=y CONFIG_PSA_CRYPTO=y` | 78 596 | +41 268 | 32 970 | 3 983 | 3 858 | 14 456 | 22 770 |  |  |  |  | 559 |
| s04 | + blob_db | `CONFIG_BLOB_DB=y CONFIG_BLOB_DB_BACKEND_UBI=y` | 90 020 | +11 424 | 33 004 | 3 983 | 3 872 | 14 456 | 22 562 | 11 773 |  |  |  | 370 |
| s05 | + rootreg | `CONFIG_BLOB_ROOTREG=y` | 91 204 | +1 184 | 33 011 | 3 983 | 3 873 | 14 456 | 22 562 | 11 773 | 1 046 |  |  | 500 |
| s06 | + kvhash | `CONFIG_BLOB_CONTAINER_KVHASH=y` | 96 600 | +5 396 | 33 159 | 3 983 | 3 873 | 14 456 | 22 562 | 11 773 | 1 046 | 5 019 |  | 729 |
| s07 | + logring | `CONFIG_BLOB_CONTAINER_LOGRING=y` | 98 668 | +2 068 | 33 158 | 3 983 | 3 873 | 14 456 | 22 562 | 11 773 | 1 046 | 6 941 |  | 876 |
| s08 | + kvdb | `CONFIG_BLOBDB_KVDB=y` | 99 560 | +892 | 33 160 | 3 983 | 3 873 | 14 456 | 22 562 | 11 773 | 1 046 | 6 941 | 801 | 965 |
| s09 | + blobfs | `CONFIG_BLOBDB_BLOBFS=y` | 103 052 | +3 492 | 33 161 | 3 983 | 3 873 | 14 456 | 22 564 | 11 773 | 1 046 | 6 941 | 4 141 | 1 114 |
| p0 | app_cbor_persondb | `as shipped` | 108 992 |  | 33 294 | 3 983 | 3 872 | 14 456 | 22 561 | 9 905 | 780 | 5 039 |  | 15 102 |

What the rows say:

- **Zephyr base is 22.0 KB**; logging adds 8.5 KB and the QSPI NOR driver with
  flash_map 5.9 KB. That 36.5 KB is what any storage on this board starts from.
- **UBI is the largest single step, +40.3 KB**: zephyr-ubi 22.2 KB, PSA Crypto
  (Mbed TLS: AES-CMAC, HKDF, SHA-256 for UBI's header MACs) 14.1 KB, and 3.5 KB
  of kernel code they pull in (heap, mutexes).
- **key_value_db's own code is 23.3 KB with every layer**: blob_db 11.5 KB
  (core 8.9 + UBI store backend 2.6), kvhash 4.9, blobfs 3.3, logring 1.9,
  rootreg 1.0, kvdb 0.8.
- **app_cbor_persondb is 106.4 KB, 10.4 % of the 1 MB application flash.** The
  storage stack (PSA Crypto + UBI + key_value_db) is 51.5 KB of it, 48 %; the
  application with zcbor is 14.7 KB. It links only the part of each layer it
  calls: 15.4 KB of key_value_db (blob_db 9.7, kvhash 4.9, rootreg 0.8) against
  the 17.4 KB the footprint app links for the same three.

`kvlist`, `kvtree`, `seq` and the intent helper are skeletons and not measured.

## 2. Kconfig toggles

Each variant is the full stack (`s08`) plus one fragment from `variants/`, or
`app_cbor_persondb` (`p0`) plus one; `v14` is against `v13`.

| Build | Kconfig | vs | ΔFLASH | ΔRAM |
|---|---|---|--:|--:|
| v01 blob_db on flash_area (no UBI) | `CONFIG_BLOB_DB_BACKEND_FLASH_AREA=y CONFIG_TEST_RANDOM_GENERATOR=y` | s08 | -41 628 | -16 325 |
| v02 UBI: no atomic replace | `CONFIG_BLOB_DB_UBI_ATOMIC_REPLACE=n` | s08 | -560 | +0 |
| v03 UBI: verify on read | `CONFIG_UBI_VERIFY_ON_READ=y` | s08 | +324 | +0 |
| v04 UBI: self checks | `CONFIG_UBI_SELF_CHECKS=y` | s08 | +1 460 | +0 |
| v05 blob_db: large payloads | `CONFIG_BLOB_DB_LARGE_PAYLOADS=y` | s08 | +4 428 | +7 614 |
| v06 blob_db: slot index | `CONFIG_BLOB_DB_SLOT_INDEX=y` | s08 | +1 964 | +26 118 |
| v07 blob_db: I/O stats | `CONFIG_BLOB_DB_IOSTATS=y` | s08 | +256 | +64 |
| v08 blob_db: inspect API | `CONFIG_BLOB_DB_INSPECT=y` | s08 | +624 | +0 |
| v09 blob_db: autoformat | `CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT=y` | s08 | +12 | +0 |
| v10 Logging off | `CONFIG_LOG=n` | s08 | -27 524 | -2 158 |
| v11 Logging: errors only | `CONFIG_LOG_DEFAULT_LEVEL=1` | s08 | -5 544 | +0 |
| v12 Stack logs at WRN | `CONFIG_BLOB_DB_LOG_LEVEL_WRN=y CONFIG_ROOTREG_LOG_LEVEL_WRN=y CONFIG_BLOB_CONTAINER_KVHASH_LOG_LEVEL_WRN=y CONFIG_UBI_LOG_LEVEL_WRN=y` | s08 | -1 532 | +0 |
| v13 Zephyr shell | `CONFIG_SHELL=y` | s08 | +31 808 | +4 742 |
| v14 + blob_db shell | `CONFIG_SHELL=y CONFIG_BLOB_DB_SHELL=y` | v13 | +6 664 | +0 |
| v15 -O2 instead of -Os | `CONFIG_SPEED_OPTIMIZATIONS=y` | s08 | +18 644 | +7 |
| p1 on flash_area | `CONFIG_BLOB_DB_BACKEND_FLASH_AREA=y` | p0 | -41 528 | -16 333 |
| p2 without I/O stats | `CONFIG_BLOB_DB_IOSTATS=n` | p0 | -304 | -64 |
| p3 logging off | `CONFIG_LOG=n` | p0 | -28 192 | -2 158 |
| p4 shell frontend | `CONFIG_APP_CBOR_PERSONDB_FRONTEND_SHELL=y` | p0 | +60 256 | +34 966 |
| p5 shell frontend  no TUI | `CONFIG_APP_CBOR_PERSONDB_FRONTEND_SHELL=y CONFIG_APP_CBOR_PERSONDB_TUI=n` | p0 | +46 024 | +31 371 |

What the toggles say:

- **The backend decides most of the bill.** `flash_area` instead of UBI saves
  40.7 KB on the full stack and 40.6 KB on persondb (106.4 → 65.9 KB).
- **Logging is the next biggest knob, 26.9 KB.** zephyr-ubi halves without it
  (22.0 → 11.1 KB) and blob_db nearly does (11.5 → 6.3 KB): about half of each
  is log calls and their strings. Errors-only logging saves 5.4 KB; the stack
  at WRN (persondb's own setting for blob_db, rootreg and kvhash) 1.5 KB.
- **blob_db's options are cheap**: large payloads +4.3 KB, slot index +1.9 KB,
  inspect +0.6 KB, I/O stats +0.25 KB, autoformat +12 B. UBI's self checks
  +1.4 KB, verify-on-read +0.3 KB; dropping atomic replace saves 0.5 KB.
- **Debug features cost more than layers**: the Zephyr shell +31.1 KB,
  blob_db's shell commands +6.5 KB on top; persondb's shell frontend +58.8 KB
  with the full-screen TUI, +44.9 KB without it. `-O2` instead of `-Os`
  +18.2 KB.

## 3. Findings

- **F1. logring does not link without an entropy driver.** It selects
  `ENTROPY_GENERATOR` and calls `sys_rand_get()`; the nRF5340 application core
  has no entropy driver, and only PSA Crypto (UBI backend) brings in a fallback.
  With the `flash_area` backend the link fails (`undefined reference to
  z_impl_sys_rand_get`). `variants/backend_flash_area.conf` adds
  `CONFIG_TEST_RANDOM_GENERATOR=y` so the comparison is like for like.
- **F2. Every DK build uses the insecure test RNG.** PSA Crypto falls back to
  `CONFIG_TEST_RANDOM_GENERATOR` (Kconfig warns "Not secure symbol"), in
  `app_cbor_persondb` too. A product build needs a real entropy source, which
  will add its own driver to these numbers.
- **F3. `CONFIG_APP_CBOR_PERSONDB_TUI=n` did not compile**: the
  `SHELL_COND_CMD_ARG` entry still names `cmd_tui`, which the flag compiles
  out. Fixed in `ui/ui_shell.c` (`#define cmd_tui NULL` when off).
