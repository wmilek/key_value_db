# app_perf_kvdb — reference results

Hardware-measured numbers for the kvdb (L3) demo/benchmark
(`src/main.c`). Keep this file honest — update it when the shape of the
benchmark changes or when regenerating on new hardware.

Regenerated on `e80f404` (main, after PR 10 merged). The previous numbers
in this file predated PR 2's lookup change and are superseded — the
reference to app_perf's "16.9 ms per blob_db read" no longer describes
this library at all; a small-blob read is now 460 µs.

**The two-level `kvhash` is measured on the DK in "On the two-level
`kvhash`".** `populate` is 2.08× faster and a rerun's `mount+open` 2.89×
faster, but steady-state reads cost ~1.65× more, so this app's rerun goes
12.8 s → 19.7 s. It lands the opposite way from `app_cbor_persondb`, and the
reason is read/write mix.

**`blob_db` now defaults to the UBI backend, and the tables below are
`flash_area`.** Both are measured — see "On the UBI backend", which is also
where this app's most interesting result lives: **UBI halves the cost of
creating the store**, because `flash_area` erases the partition and then
erases every bucket again, while UBI reuses the blocks its own format just
erased.

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

## On the UBI backend (the default)

Same commit, same board, same defaults; only `CONFIG_BLOB_DB_BACKEND_UBI`
differs, taken from the board conf rather than a command-line override.

### Store creation is halved — the one place UBI wins outright

| phase | `flash_area` | UBI | |
|---|--:|--:|--:|
| `mount+open` (incl. format) | 140.0 s | 145.4 s | +4% |
| `prepare` (116–122 buckets) | **134.5 s** | **0.148 s** | **×909** |
| **format + prepare total** | **274.5 s** | **145.5 s** | **×1.89 faster** |
| `populate` | 27.1 ms/op | 29.8 ms/op | ×1.10 slower |

`prepare` costs 1 275 µs/op against 1 102 467. The reason is not that UBI
made erasing cheap — it is that **`flash_area` erases the same blocks
twice.** `FRESH_START` erases the whole partition, and then `prepare()`
erases each of the 122 buckets again, because on the raw partition a bucket
format means erasing that sector whether or not it was just erased. On UBI
a bucket format is an LEB operation, and UBI still has the PEBs its own
format erased moments earlier, so it hands one over without touching flash.

**This is a genuine saving of ~129 s on first population, not an accounting
artifact** — but it is available only while UBI has pre-erased blocks in
hand. `app_perf_mc/RESULTS.md` now shows both sides of that from a single
binary run twice: 1 279 µs/op on a volume UBI has just formatted, and
1 102 000 µs/op on the very next run, when the pool is spent. UBI moves erase
cost in time; it does not remove it.

### Steady state is slower, in proportion to read count

| phase | `flash_area` | UBI | Δ |
|---|--:|--:|--:|
| `mount+open` (rerun) | 1.26 s | 1.52 s | ×1.21 |
| `verify` | 2.71 ms | 6.08 ms | ×2.24 |
| `modify` | 11.1 ms | 16.1 ms | ×1.46 |
| `reverify` | 2.82 ms | 6.40 ms | ×2.27 |
| **rerun total** | **≈6.4 s** | **≈12.8 s** | **×2.0** |

Both `VERIFY PASS` results hold on both backends, at the same generations.

The pattern matches `app_perf/RESULTS.md`: UBI's LEB→PEB indirection costs
~112 µs per flash transaction and nothing per byte, so read-dominated
phases take the full ~2.25× while `modify`, which is half write, takes
~1.46×. A `kvdb_get` is two blob_db reads, and both now pay the penalty.

So on the default backend the "one-minute steady-state test" is a
~13-second test rather than a ~6-second one — still far from the minute the
`N_KEYS` help text assumes.

## On the two-level `kvhash`

`kvhash` gained a second bucket level (`doc/proposals/2026-08-20-kvhash-second-level.md`).
This app goes through `kvdb` to reach it, so it is measured here as well.
Same board, same defaults, same UBI backend; only the container differs. The
proposal's §13.2 notes nothing had run on the DK — this is that run.

`VERIFY PASS` at every generation, so the change is behaviour-preserving through
`kvdb`.

| phase | one level (UBI) | **two level** | |
|---|--:|--:|--:|
| `mount+open` (first, incl. format) | 145 366 ms | 143 781 ms | ×1.01 faster |
| `prepare` | 1 275 µs/op (116 buckets) | 1 310 µs/op (100 buckets) | ×1.03 slower |
| **`populate`** | **29 770 µs/op** | **14 340 µs/op** | **×2.08 faster** |
| `verify` (first) | 5 531 µs/op | 9 088 µs/op | ×1.64 slower |
| `modify` (first) | 15 352 µs/op | 15 331 µs/op | ×1.00 |
| `reverify` (first) | 6 045 µs/op | 9 846 µs/op | ×1.63 slower |
| **`mount+open`** (rerun) | **1 518 ms** | **526 ms** | **×2.89 faster** |
| `verify` (rerun) | 6 083 µs/op | 9 923 µs/op | ×1.63 slower |
| `modify` (rerun) | 16 122 µs/op | 16 423 µs/op | ×1.02 slower |
| `reverify` (rerun) | 6 401 µs/op | 10 793 µs/op | ×1.69 slower |
| **rerun total** | **≈12.8 s** | **≈19.7 s** | **×1.54 slower** |

The shape matches `app_cbor_persondb/RESULTS.md` §5e exactly, which is the
useful part: two applications, different access patterns, same verdict.

**Writes and boot get faster; steady-state reads get slower.** `populate` halves
and `mount+open` on a rerun is nearly three times quicker, because both are
dominated by writing or reading map structure whose largest blob just got much
smaller. `verify` and `reverify` are pure `kvdb_get` and cost ~1.65× more,
because a two-level lookup is an extra flash transaction and UBI charges 178 µs
for one against 0.616 µs per byte (`app_perf/RESULTS.md`).

`modify` is unchanged to within 2 % in both runs, which is the tell: it is a get
plus an update, so the read regression and the write improvement land on top of
each other and cancel.

**So the trade is not free here, and unlike persondb this app does not come out
ahead on the whole run** — its steady-state loop is read-dominated, and 12.8 s
becomes 19.7 s. persondb's whole run improves 2.31× because it is fill-dominated.
Which way the change lands depends entirely on the read/write mix, and these two
apps bracket it.

Note `prepare` formats **100** buckets against 116: the two-level container's
structure occupies more of the volume up front.

## Raw UART capture — UBI, first run (`FRESH_START`, gen 1 -> 2)

UBI's volume-probe lines are elided; it logs them at `<err>` level.

```
*** Booting Zephyr OS build 4a405846193f ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
FRESH_START: formatting store
[00:02:25.567,413] <inf> rootreg: virgin store — registry bootstrapped at id 1
mount+open   :         145366 ms
state: empty store -> initial population
bench prepare  :  116 ops in    148 ms  ->   783.783 ops/s  (   1275 us/op)
bench populate :  770 ops in  22923 ms  ->    33.590 ops/s  (  29770 us/op)
bench verify   :  769 ops in   4254 ms  ->   180.771 ops/s  (   5531 us/op)
VERIFY PASS (gen 1)
bench modify   :  196 ops in   3009 ms  ->    65.137 ops/s  (  15352 us/op)
bench reverify :  769 ops in   4649 ms  ->   165.411 ops/s  (   6045 us/op)
VERIFY PASS (gen 2)
done — store at gen 2; rerun to verify persistence
```

Note `prepare` formats **116** buckets against 122 on `flash_area`: UBI
reserves 2 PEBs for its headers, so the volume is smaller and fewer buckets
fit.

## Raw UART capture — UBI, rerun (gen 2 -> 3)

```
*** Booting Zephyr OS build 4a405846193f ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
mount+open   :           1518 ms
state: rerun, store at gen 2
bench verify   :  769 ops in   4678 ms  ->   164.386 ops/s  (   6083 us/op)
VERIFY PASS (gen 2)
bench modify   :  196 ops in   3160 ms  ->    62.025 ops/s  (  16122 us/op)
bench reverify :  769 ops in   4923 ms  ->   156.205 ops/s  (   6401 us/op)
VERIFY PASS (gen 3)
done — store at gen 3; rerun to verify persistence
```

## Raw UART capture — `flash_area`, first run (gen 1 -> 2)

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

## Raw UART capture — `flash_area`, rerun (gen 2 -> 3)

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

## Raw UART capture — two-level `kvhash`, first run (gen 1 -> 2)

```
*** Booting Zephyr OS build 4a405846193f ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
[00:02:23.903,320] <inf> rootreg: virgin store — registry bootstrapped at id 1
mount+open   :         143781 ms
state: empty store -> initial population
bench prepare  :  100 ops in    131 ms  ->   763.358 ops/s  (   1310 us/op)
bench populate :  770 ops in  11042 ms  ->    69.733 ops/s  (  14340 us/op)
bench verify   :  769 ops in   6989 ms  ->   110.030 ops/s  (   9088 us/op)
VERIFY PASS (gen 1)
bench modify   :  196 ops in   3005 ms  ->    65.224 ops/s  (  15331 us/op)
bench reverify :  769 ops in   7572 ms  ->   101.558 ops/s  (   9846 us/op)
VERIFY PASS (gen 2)
done — store at gen 2; rerun to verify persistence
```

## Raw UART capture — two-level `kvhash`, rerun (gen 2 -> 3)

```
*** Booting Zephyr OS build 4a405846193f ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
mount+open   :            526 ms
state: rerun, store at gen 2
bench verify   :  769 ops in   7631 ms  ->   100.773 ops/s  (   9923 us/op)
VERIFY PASS (gen 2)
bench modify   :  196 ops in   3219 ms  ->    60.888 ops/s  (  16423 us/op)
bench reverify :  769 ops in   8300 ms  ->    92.650 ops/s  (  10793 us/op)
VERIFY PASS (gen 3)
done — store at gen 3; rerun to verify persistence
```

## Reproducing

```bash
west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/kvdb app_perf_kvdb
# add -- -DCONFIG_APP_PERF_KVDB_FRESH_START=y for the store-creation run
```

That build uses the default UBI backend, with the PEB pool sized in
`boards/nrf5340dk_nrf5340_cpuapp.conf`. For the `flash_area` column add
`-DCONFIG_BLOB_DB_BACKEND_FLASH_AREA=y`, and **erase the partition raw when
switching between backends** — the two layouts are not interchangeable, and
UBI only formats a partition it finds erased.

The store must also be one this build can mount. `app_perf` enables
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
