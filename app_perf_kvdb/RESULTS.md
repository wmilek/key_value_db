# app_perf_kvdb — reference results

Hardware-measured numbers for the kvdb (L3) demo/benchmark
(`src/main.c`). Keep this file honest — update it when the shape of the
benchmark changes or when regenerating on new hardware.

Regenerated on `e80f404` (main, after PR 10 merged). The previous numbers
in this file predated PR 2's lookup change and are superseded — the
reference to app_perf's "16.9 ms per blob_db read" no longer describes
this library at all; a small-blob read is now 460 µs.

## Setup

- **Target**: nRF5340-DK (S/N 960115021, PCA10095), cpuapp core
- **Storage**: `storage_partition` on the on-board MX25R6435F QSPI NOR
  (8 MB, 64 KB sectors, 8 MHz Quad-SPI) — see
  `boards/nrf5340dk_nrf5340_cpuapp.overlay`
- **Config**: `N_KEYS = 768`, `VAL_LEN = 16` (value = 24 B), kvhash
  backend, `BLOB_DB_MAX_PAYLOAD_LEN = 1024`
- **Code**: `e80f404`; **Zephyr** build `4a405846193f`, SDK
  `zephyr-sdk-1.0.1`

## Numbers

First run (store creation, one-time):

| phase    | ops | time     | per op  | notes                                                               |
| -------- | --: | -------: | ------: | ------------------------------------------------------------------- |
| format   |   — | 140.0 s  |       — | full-partition erase (FRESH_START path; reported inside mount+open) |
| prepare  | 122 | 134.5 s  | 1.102 s | one 64 KB sector erase per blob_db bucket                           |
| populate | 770 |  20.9 s  | 27.1 ms | warm write path + lazy kvhash-bucket creation                       |

Every rerun (the steady-state test):

| phase      | ops | time    | per op  |  ops/s |
| ---------- | --: | ------: | ------: | -----: |
| mount+open |   — |  1.26 s |       — |      — |
| verify     | 769 |  2.08 s | 2.71 ms |  369.0 |
| modify     | 196 |  2.17 s | 11.1 ms |   90.4 |
| reverify   | 769 |  2.17 s | 2.82 ms |  354.1 |

**Rerun total ≈ 6.4 s, down from ≈65 s.** The one-minute steady-state
test the `N_KEYS` help text describes is now a six-second test; that
default was sized against the old per-op costs and is worth revisiting if
a minute of wall clock was the point.

### Where the gains come from

Everything here is inherited from L1, not from kvdb changes:

| phase    | before  |    now  |     Δ |
| -------- | ------: | ------: | ----: |
| verify   | 34.9 ms | 2.71 ms | ×12.9 |
| modify   | 55.9 ms | 11.1 ms |  ×5.0 |
| reverify | 35.1 ms | 2.82 ms | ×12.4 |
| populate | 78.0 ms | 27.1 ms |  ×2.9 |

A `kvdb_get` is two blob_db reads (bucket directory + bucket blob). At
460 µs per small-blob read that predicts ~0.9 ms, against 2.71 ms
measured — the remainder is kvhash's in-bucket unpack and key compare
over a 1024 B packed bucket, which is now the dominant term rather than
flash time. **The read path is no longer where a `get` spends its time**,
so further work on this app belongs in the packing, not in L1.

`modify` (a `kvdb_set` over an existing key) is a get plus one blob_db
update. At 2.5 ms per update that predicts ~5.2 ms against 11.1 ms
measured; the gap is the bucket repack plus the second directory read.

`format` and `prepare` are unchanged, as they must be — both are pure
64 KB sector erase at ~1.09 s each, a property of the MX25R64 and not of
any code in this tree.

## Raw UART capture (first run after FRESH_START, gen 1 -> 2)

```
*** Booting Zephyr OS build 4a405846193f ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
FRESH_START: formatting store
[00:02:16.969,268] <inf> rootreg: virgin store — registry bootstrapped at id 1
mount+open   :         140003 ms
state: empty store -> initial population
bench prepare  :  122 ops in 134501 ms  ->     0.907 ops/s  (1102467 us/op)
bench populate :  770 ops in  20885 ms  ->    36.868 ops/s  (  27123 us/op)
bench verify   :  769 ops in   1867 ms  ->   411.890 ops/s  (   2427 us/op)
VERIFY PASS (gen 1)
bench modify   :  196 ops in   1983 ms  ->    98.840 ops/s  (  10117 us/op)
bench reverify :  769 ops in   2048 ms  ->   375.488 ops/s  (   2663 us/op)
VERIFY PASS (gen 2)
done — store at gen 2; rerun to verify persistence
```

## Raw UART capture (rerun, gen 2 -> 3)

```
*** Booting Zephyr OS build 4a405846193f ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
mount+open   :           1256 ms
state: rerun, store at gen 2
bench verify   :  769 ops in   2084 ms  ->   369.001 ops/s  (   2710 us/op)
VERIFY PASS (gen 2)
bench modify   :  196 ops in   2168 ms  ->    90.405 ops/s  (  11061 us/op)
bench reverify :  769 ops in   2172 ms  ->   354.051 ops/s  (   2824 us/op)
VERIFY PASS (gen 3)
done — store at gen 3; rerun to verify persistence
```

The first run's own verify/modify/reverify (2.43 / 10.1 / 2.66 ms) run
slightly faster than the rerun's (2.71 / 11.1 / 2.82 ms) because that
store was populated moments earlier in bucket order; the rerun reads it
back cold from a fresh mount.

## Reproducing

```bash
west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/kvdb app_perf_kvdb
# add -- -DCONFIG_APP_PERF_KVDB_FRESH_START=y for the store-creation run
```

The store must be one this build can mount. `app_perf` enables
`CONFIG_BLOB_DB_LARGE_PAYLOADS=y`, which bumps the on-flash format major
to 2, and this app does not — so after running `app_perf` on the same
board, mount fails `-ENOTSUP` (a foreign store) *before* `FRESH_START`
gets a chance to format. Erase the partition first; see the
"Downgrading" section of `app_perf/RESULTS.md`.

Attach exactly one reader to the console tty. Two concurrent `cat`s split
the byte stream and silently shred the capture.

## Power-loss field note

A pre-intent-protocol build was power-cut mid-modify during a gen 5 -> 6
bump. The next boot's strict verify reported exactly the torn prefix —
108 keys (`k002..k430`, every 4th) each holding a *complete, valid*
gen-6 value, the rest untouched at their expected values — i.e. per-op
atomicity held; only the classification was missing. Builds with the
intent protocol classify the same state as `POWER LOSS detected`,
run the recovery verify (old-XOR-new per key), report the torn split,
and roll the bump forward.

To reproduce, cut power during the modify window right after the first
`VERIFY PASS` of a rerun — but note that window is now **~2 s**, not the
~11 s this note was written against.
