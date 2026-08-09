# app_perf_kvdb — reference results

Hardware-measured numbers for the kvdb (L3) demo/benchmark
(`src/main.c`). Keep this file honest — update it when the shape of the
benchmark changes or when regenerating on new hardware.

## Setup

- **Target**: nRF5340-DK (S/N 960115021, PCA10095), cpuapp core
- **Storage**: `storage_partition` on the on-board MX25R6435F QSPI NOR
  (8 MB, 64 KB sectors, 8 MHz Quad-SPI) — see
  `boards/nrf5340dk_nrf5340_cpuapp.overlay`
- **Config**: `N_KEYS = 768`, `VAL_LEN = 16` (value = 24 B), kvhash
  backend, 127 buckets, `BLOB_DB_MAX_PAYLOAD_LEN = 1024`
- **Zephyr**: main (`c2805ec5324d`), SDK `zephyr-sdk-1.0.1`

## Numbers

First run (store creation, one-time):

| phase    | ops | time      | per op     | notes |
| -------- | --: | --------: | ---------: | ----- |
| format   |   — | 140.3 s   | —          | full-partition erase (FRESH_START path; reported inside mount+open) |
| prepare  | 122 | 133.8 s   | 1.097 s    | one 64 KB sector erase per blob_db bucket |
| populate | 770 | 60.0 s    | 78.0 ms    | warm write path + lazy kvhash-bucket creation |

Every rerun (the steady-state ~1-minute test):

| phase    | ops | time      | per op     | ops/s  |
| -------- | --: | --------: | ---------: | -----: |
| verify   | 769 | 26.8 s    | 34.9 ms    | 28.7   |
| modify   | 196 | 11.0 s    | 55.9 ms    | 17.9   |
| reverify | 769 | 27.0 s    | 35.1 ms    | 28.5   |

Rerun total ≈ 65 s. A `kvdb_get` is two blob_db reads (bucket directory +
bucket blob) — 34.9 ms tracks app_perf's 16.9 ms per blob_db read almost
exactly. A `kvdb_set` adds one blob_db update on top (~17.5 ms warm).

## Raw UART capture (first run after FRESH_START, gen 1 -> 2)

```
*** Booting Zephyr OS build c2805ec5324d ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
FRESH_START: formatting store
[00:02:17.139,770] <inf> rootreg: virgin store — registry bootstrapped at id 1
mount+open   :         140311 ms
state: empty store -> initial population
bench prepare  :  122 ops in 133796 ms  ->     0.911 ops/s  (1096688 us/op)
bench populate :  770 ops in  60047 ms  ->    12.823 ops/s  (  77983 us/op)
bench verify   :  769 ops in  26812 ms  ->    28.681 ops/s  (  34866 us/op)
VERIFY PASS (gen 1)
bench modify   :  196 ops in  10952 ms  ->    17.896 ops/s  (  55877 us/op)
bench reverify :  769 ops in  26994 ms  ->    28.487 ops/s  (  35102 us/op)
VERIFY PASS (gen 2)
done — store at gen 2; rerun to verify persistence
```

## Power-loss field note

A pre-intent-protocol build was power-cut mid-modify during a gen 5 -> 6
bump. The next boot's strict verify reported exactly the torn prefix —
108 keys (`k002..k430`, every 4th) each holding a *complete, valid*
gen-6 value, the rest untouched at their expected values — i.e. per-op
atomicity held; only the classification was missing. Builds with the
intent protocol classify the same state as `POWER LOSS detected`,
run the recovery verify (old-XOR-new per key), report the torn split,
and roll the bump forward. To reproduce: cut power during the ~11 s
modify window (right after the first `VERIFY PASS` of a rerun).
