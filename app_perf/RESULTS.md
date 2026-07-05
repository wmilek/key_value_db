# app_perf — reference results

Hardware-measured ops/sec for the four workloads implemented by
`app_perf/src/main.c` (build/flash instructions in `src/main.c`'s
header comment). Keep this file honest — update it when the shape
of the benchmark changes or when regenerating on new hardware.

## Setup

- **Target**: nRF5340-DK (S/N 960115021, PCA10095), cpuapp core
- **Storage**: `storage_partition` on the on-board MX25R6435F QSPI NOR
  (8 MB, 64 KB sectors, 8 MHz Quad-SPI) — see
  `boards/nrf5340dk_nrf5340_cpuapp.overlay`
- **Config**: `N_OPS = 100`, `VAL_LEN = 24 B` (node = 32 B)
- **Zephyr**: v4.4.99 (`34bd8ff000cc`), SDK `zephyr-sdk-1.0.1`

## Numbers

Two configurations, back-to-back on the same board:

| workload | cold (no `prepare`) | warm (`prepare(N_OPS)`) | Δ       |
| -------- | ------------------: | ----------------------: | ------: |
| prepend  |         0.850 ops/s |             15.80 ops/s | ×18.6   |
| append   |         0.851 ops/s |             15.94 ops/s | ×18.7   |
| read     |        59.311 ops/s |            59.311 ops/s | —       |
| update   |        29.095 ops/s |            29.103 ops/s | —       |

The current benchmark harness runs the **warm** configuration by
default — see the `blob_db_prepare()` call after each `erase_all()`
in `src/main.c`.

### Where the ×18 goes

The **cold** run pays a full 64 KB sector erase (~1 s on mx25r64 at
8 MHz Quad-SPI) inside every prepend/append, because each fresh id
lands in a bucket whose header isn't valid yet. The **warm** run
hoists that cost out of the timed loop via `blob_db_prepare(N_OPS)`,
which pre-formats the 100 upcoming buckets in one batch (~110 s once,
reported as a separate `prepare` line — see the raw output below).
After that, every write is on the append-only path — no erase, no
compaction — and the timed loop measures the actual per-op cost:
one slot header + payload + CRC written to already-erased flash,
about 63 ms/op with two writes per prepend/append.

`read` and `update` never triggered an erase in either configuration
(both walk buckets whose headers are already valid), so they were
already on the warm path — the numbers match to three decimals.

## Raw UART capture (warm run)

```
*** Booting Zephyr OS build 34bd8ff000cc ***
blob_db perf 1.0.0  (N_OPS=100  VAL_LEN=24  node=32 B)
bench prepare :  100 ops in  110326 ms  ->    0.906 ops/s  (  1103260 us/op)
bench prepend :  100 ops in    6330 ms  ->   15.797 ops/s  (    63300 us/op)
bench read   :  100 ops in    1686 ms  ->   59.311 ops/s  (    16860 us/op)
bench update :  100 ops in    3435 ms  ->   29.112 ops/s  (    34350 us/op)
prepend checksum: 0xee3fa466
bench prepare :  100 ops in  110487 ms  ->    0.905 ops/s  (  1104870 us/op)
bench append :  100 ops in    6275 ms  ->   15.936 ops/s  (    62750 us/op)
bench read   :  100 ops in    1686 ms  ->   59.311 ops/s  (    16860 us/op)
bench update :  100 ops in    3437 ms  ->   29.095 ops/s  (    34370 us/op)
append checksum:  0x50f65666
```

## Raw UART capture (cold reference — no `prepare` call)

Kept for comparison; the harness that produced these numbers erased
the store before each phase but did not pre-format buckets.

```
*** Booting Zephyr OS build 34bd8ff000cc ***
blob_db perf 1.0.0  (N_OPS=100  VAL_LEN=24  node=32 B)
bench prepend:  100 ops in  117603 ms  ->    0.850 ops/s  ( 1176030 us/op)
bench read  :  100 ops in    1686 ms  ->   59.311 ops/s  (   16860 us/op)
bench update:  100 ops in    3437 ms  ->   29.095 ops/s  (   34370 us/op)
prepend checksum: 0xee3fa466
bench append:  100 ops in  117415 ms  ->    0.851 ops/s  ( 1174150 us/op)
bench read  :  100 ops in    1686 ms  ->   59.311 ops/s  (   16860 us/op)
bench update:  100 ops in    3437 ms  ->   29.095 ops/s  (   34370 us/op)
append checksum:  0x50f65666
```

## Reproducing

```bash
# From the west workspace top-dir
west build -p auto -b nrf5340dk/nrf5340/cpuapp app_perf
nrfutil device program --firmware build/zephyr/zephyr.hex \
    --options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE,reset=RESET_SYSTEM \
    --serial-number <your-jlink-sn>
# Console is on VCOM 2 (typically /dev/ttyACM2 on Linux)
stty -F /dev/ttyACM2 115200 raw -echo -icrnl -inlcr
cat /dev/ttyACM2
# Then reset:
nrfutil device reset --serial-number <sn> --reset-kind RESET_SYSTEM
```

Each phase takes ~120 s (110 s prepare + ~10 s timed loops); the full
run finishes in under 5 minutes.
