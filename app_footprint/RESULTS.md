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
- **F4. `CONFIG_UBI_MAX_NR_OF_VOLUMES=1` does not compile.** zephyr-ubi
  builds with `-Werror`, and GCC 14 reports `array subscript 1 is above array
  bounds of 'struct ubi_volume[1]'` at `library/ubi_volume.c:139` and `:166`.
  Both loops start at `index + 1 < count`, so they never run with one volume;
  the warning is a false positive, but it stops the build. blob_db uses one
  volume. `2` builds and saves nothing, so the default of 4 costs no RAM here.

## 4. The smallest persondb that still runs

How far `app_cbor_persondb` goes down with Kconfig and devicetree alone, without
changing its code. Measured 2026-10-30 with Zephyr SDK 1.0.1 at key_value_db
`43024978632f`, Zephyr `ec98918869ca`, zephyr-ubi `d66b599af9ef`. That Zephyr
is newer than the one in §1–2, so `p0` here is 109 056 B, not 108 992; every
row below comes from this one tree.

Fragments: [`variants/persondb_minimal.conf`](variants/persondb_minimal.conf)
with [`variants/persondb_minimal.overlay`](variants/persondb_minimal.overlay),
and [`variants/persondb_single_thread.conf`](variants/persondb_single_thread.conf)
on top of them. They bundle several toggles, so `footprint.py` does not build
them. Build them by hand:

```
west build -b nrf5340dk/nrf5340/cpuapp key_value_db/app_cbor_persondb -- \
  -DEXTRA_CONF_FILE="$PWD/key_value_db/app_footprint/variants/persondb_minimal.conf" \
  -DEXTRA_DTC_OVERLAY_FILE="$PWD/key_value_db/app_footprint/variants/persondb_minimal.overlay"
```

Each row adds one toggle to the row above it. RAM is listed for reference only,
as in §1.

| Step | Kconfig added | FLASH | Δ | RAM |
|---|---|--:|--:|--:|
| p0 as shipped | — | 109 056 | | 215 188 |
| flash_area backend | `CONFIG_BLOB_DB_BACKEND_FLASH_AREA=y` | 67 528 | -41 528 | 198 864 |
| logging off | `CONFIG_LOG=n` | 51 904 | -15 624 | 196 704 |
| LTO | `CONFIG_LTO=y CONFIG_ISR_TABLES_LOCAL_DECLARATION=y` | 47 304 | -4 600 | 196 680 |
| no GPIO driver | `CONFIG_GPIO=n` | 44 188 | -3 116 | 196 496 |
| no fault dump | `CONFIG_EXCEPTION_DEBUG=n CONFIG_FAULT_DUMP=0` | 42 948 | -1 240 | 196 496 |
| no MPU stack guard | `CONFIG_ARM_MPU=n CONFIG_HW_STACK_PROTECTION=n` | 42 212 | -736 | 196 432 |
| nano cbprintf | `CONFIG_CBPRINTF_NANO=y` | 41 532 | -680 | 196 432 |
| no time slicing | `CONFIG_TIMESLICING=n` | 41 084 | -448 | 196 400 |
| no I/O stats | `CONFIG_BLOB_DB_IOSTATS=n` | 40 876 | -208 | 196 336 |
| no boot banner | `CONFIG_BOOT_BANNER=n` | 40 828 | -48 | 196 336 |
| no libc malloc | `CONFIG_COMMON_LIBC_MALLOC=n` | 40 284 | -544 | 196 320 |
| no internal-flash driver | `CONFIG_SOC_FLASH_NRF=n` + overlay | **39 720** | -564 | 196 296 |
| single-threaded | `CONFIG_MULTITHREADING=n` | **36 948** | -2 772 | 195 560 |

**39 720 B (38.8 KB) is the minimum, 36 % of the shipped 106.5 KB.** With
`CONFIG_MULTITHREADING=n` as well it is 36 948 B (36.1 KB), 34 %.

What is left, attributed without LTO (44 136 B; LTO merges every object, so the
attribution cannot see inside it):

| app | platform | kvhash | blob_db | kernel | flash drv | libc | zcbor | Zephyr libs | linker | rootreg | CRC | blob_db backend |
|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| 10 995 | 10 133 | 4 514 | 4 351 | 3 715 | 2 975 | 1 978 | 1 785 | 1 622 | 860 | 628 | 312 | 268 |

The application and the platform (QSPI and UART HAL, RTC timer, clocks,
pinctrl) are half of it. The storage stack is 9.5 KB.

If UBI stays, the same toggles apart from the backend and malloc (Mbed TLS
allocates) give **65 004 B**. Single-threaded does not link with UBI, because
zephyr-ubi takes a `k_mutex`.

What it was checked with:

- **nRF5340-DK: built only, not run.** All builds above link. No DK was
  attached when these numbers were taken.
- **native_sim: run.** The part of `persondb_minimal.conf` that native_sim
  has (flash_area, no logging, no I/O stats, nano cbprintf, no GPIO, no
  malloc, no time slicing, no banner) ran the full 8 000 persons: fill,
  `VERIFY PASS` twice, `bucket overflows: 0`, 41.3 % live (the shipped
  figure), then a second boot on the same flash file (`rev 1 -> 2`,
  `VERIFY PASS`). Nano cbprintf prints the report the same as the full
  formatter, apart from the timings. LTO, the MPU, the fault dump, the
  internal-flash driver and `MULTITHREADING=n` are Cortex-M settings that
  native_sim cannot exercise. They need a DK run.

What each cut costs:

- **flash_area instead of UBI** changes the on-flash format (raw partition,
  not a UBI volume): expect to wipe a store the shipped image wrote. It also
  drops UBI's wear leveling and header MACs.
- **No I/O stats**: the report's "flash ops" and "B" columns read 0. A
  benchmark run loses its measurement, which is what the app is for.
- **No MPU guard**: a main-stack overflow corrupts memory silently. The
  28 KB stack is sized at 1.4× a measured worst case (`prj.conf`).
- **Not to be cut**: the console (the report is the output),
  `CONFIG_ZCBOR_CANONICAL` (verification compares encodings), and
  `CBPRINTF_FULL_INTEGRAL` (the report prints 64-bit values).
  `CONFIG_SIZE_OPTIMIZATIONS_AGGRESSIVE` (`-Oz`) gave the same image byte for
  byte, and `CONFIG_MINIMAL_LIBC` was 992 B larger than picolibc.
- **RAM does not shrink with any of this.** 188 KB of the 196 KB is two
  64 KB blob_db sector buffers, two 16 KB kvhash buffers and the 28 KB main
  stack. All of them follow from the 64 KB erase block and
  `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN=16384`, which the 8 000-person dataset
  needs.

## 5. persondb on UBI, storage stack trimmed

§4 trims the whole image. This section keeps UBI and leaves Zephyr as
shipped, and changes only the storage stack: logging off, diagnostics off,
and PSA Crypto cut down to what zephyr-ubi calls. Same tree as §4.

Fragments: [`variants/persondb_ubi_minimal.conf`](variants/persondb_ubi_minimal.conf)
and the TF-PSA-Crypto settings in
[`variants/persondb_ubi_psa.h`](variants/persondb_ubi_psa.h). A `.conf` file
cannot name the header by a relative path, so it goes on the command line:

```
west build -b nrf5340dk/nrf5340/cpuapp key_value_db/app_cbor_persondb -- \
  -DEXTRA_CONF_FILE="$PWD/key_value_db/app_footprint/variants/persondb_ubi_minimal.conf" \
  -DCONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE=\"$PWD/key_value_db/app_footprint/variants/persondb_ubi_psa.h\"
```

Each row adds one setting to the row above it:

| Step | Setting | FLASH | Δ | RAM | ΔRAM |
|---|---|--:|--:|--:|--:|
| p0 as shipped | — | 109 056 | | 215 188 | |
| logging off | `CONFIG_LOG=n` | 80 864 | -28 192 | 213 028 | -2 160 |
| no I/O stats | `CONFIG_BLOB_DB_IOSTATS=n` | 80 560 | -304 | 212 964 | -64 |
| PSA RNG from Zephyr | `CONFIG_MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG=y` (+ test RNG, see below) | 78 128 | -2 432 | 212 796 | -168 |
| AES: encrypt only, 128-bit | `MBEDTLS_BLOCK_CIPHER_NO_DECRYPT`, `MBEDTLS_AES_ONLY_128_BIT_KEY_LENGTH` | **76 436** | -1 692 | **208 444** | -4 352 |

**76 436 B (74.6 KB), 70 % of the shipped image, with UBI kept.** The storage
stack goes from 51.6 to 31.5 KB:

| Build | PSA Crypto | UBI | blob_db | blob_db backend | rootreg | kvhash | Stack total | Logging core |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| p0 | 14 456 | 22 561 | 7 453 | 2 516 | 780 | 5 039 | 52 805 | 3 983 |
| trimmed | 10 335 | 11 362 | 4 351 | 1 098 | 628 | 4 514 | 32 288 | 0 |

Of the 31.9 KB saved, 20.0 KB comes out of the stack. The rest is the logging
core (3.9 KB), the kernel code logging pulled in (Zephyr base 32.1 → 27.4 KB)
and persondb's own log calls (app 13.0 → 10.7 KB).

Why each step is safe:

- **External RNG.** zephyr-ubi draws one random word from PSA: the image
  sequence number, when it formats a device. Mbed TLS's entropy pool and
  HMAC_DRBG only fed that. The nRF5340 application core has no CSPRNG driver,
  so the fragment keeps the test generator the shipped build already falls
  back to (F2), via `..._EXTERNAL_RNG_ALLOW_NON_CSPRNG` and
  `CONFIG_TEST_RANDOM_GENERATOR`. With a real entropy driver, drop those two
  lines; the external RNG is then the smaller and the secure choice.
- **AES encrypt-only, 128-bit.** zephyr-ubi uses AES only inside AES-CMAC,
  which only encrypts, with 128-bit keys (`UBI_KEY_BITS`). No ECB/CBC
  algorithm is enabled, which `NO_DECRYPT` requires. It also drops the
  decryption tables from RAM, which is most of the RAM saved.
- **No on-flash change.** The keys, MACs and layout are unchanged. On native_sim,
  a store written by the shipped image opens in the trimmed one and the other
  way round: shipped fill at 8 000, then trimmed `rev 1 -> 2`, then shipped
  `rev 2 -> 3`, all `VERIFY PASS`.

Checked: the nRF5340 build links (not run on a DK). On native_sim the same two
files ran the full 8 000 persons: `VERIFY PASS` twice, `bucket overflows: 0`,
41.3 % live, and a second boot from the same flash file (`rev 1 -> 2`), plus
the store exchange above.

Measured and left out (each on top of the trimmed build):

| Setting | ΔFLASH | Why not |
|---|--:|---|
| `CONFIG_BLOB_DB_UBI_ATOMIC_REPLACE=n` | -240 | Not a diagnostic: compaction goes back to five erases through the scratch sector, and an existing store must be formatted first (Kconfig help) |
| `CONFIG_UBI_ERASE_INVALIDATES_HEADERS=n` | -144 | An erase cut short can leave a block that attach takes back as valid |
| `CONFIG_ROOTREG_MAX_ROOTS=1` | -64 | persondb needs one root, but the next root anyone adds fails |
| `CONFIG_MBEDTLS_INIT=n`, `CONFIG_MBEDTLS_HAVE_ASM=n` | -16 each | Too small to matter |
| `CONFIG_MBEDTLS_MEMORY_BUFFER_ALLOC_C=n` | 0 | Not linked anyway |
| `CONFIG_MBEDTLS_PSA_KEY_SLOT_COUNT=4` | 0 | RAM only: -816 B. zephyr-ubi creates two keys (the HKDF secret and the derived CMAC key); take it if RAM matters |
| `CONFIG_UBI_MAX_NR_OF_VOLUMES=1` | — | Does not compile (F4); `2` saves nothing |

Already off in the shipped build, so nothing to gain: `UBI_VERIFY_ON_READ`,
`UBI_SELF_CHECKS`, `BLOB_DB_INSPECT`, `BLOB_DB_SHELL`, `BLOB_DB_SLOT_INDEX`,
`BLOB_DB_LARGE_PAYLOADS`, `BLOB_DB_AUTOFORMAT_ON_CORRUPT`,
`BLOB_CONTAINER_KVHASH_TEST_HOOKS`.

What is left in PSA Crypto (10.3 KB) is mostly its core, `psa_crypto.c` at 4.5 KB,
then AES 1.4, the MAC layer 1.0, SHA-256 0.9 and CMAC 0.8. No Kconfig setting
reaches the core.
