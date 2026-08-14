# app_perf — reference results

Hardware-measured results for the workloads implemented by
`app_perf/src/main.c` (build/flash instructions in `RUN_ON_DK.md`). Keep
this file honest — update it when the shape of the benchmark changes or
when regenerating on new hardware.

Every wall-clock figure here comes from the board. On `native_sim` they
are all `0 ms`, because the flash simulator models no latency; only the
`io …` counters are meaningful there.

Two commits were measured back-to-back on the same board, so the effect
of the PR 2 lookup change can be read straight off the first table:

- **`8428e35`** — *app_perf: add large-object phases and flash I/O
  accounting*. The branch tip (`599766a`) only adds documentation, so
  these numbers describe the tip as well.
- **`255ce7a`** — *blob_db: fix data loss when a compaction scratch write
  is torn*, the commit immediately before PR 2. Small-blob phases only.

## Setup

- **Target**: nRF5340-DK (S/N 960115021, PCA10095), cpuapp core
- **Storage**: `storage_partition` on the on-board MX25R6435F QSPI NOR
  (8 MB, 64 KB sectors, 8 MHz Quad-SPI) — see
  `boards/nrf5340dk_nrf5340_cpuapp.overlay`. Nothing non-default about
  the QSPI setup; internal flash is untouched.
- **Config**: `N_OPS = 100`, `VAL_LEN = 24 B` (node = 32 B);
  large objects `OBJ_LEN = 65536`, `N_LARGE = 4`, `N_PART = 32`,
  `PART_LEN = 64` — all defaults
- **Zephyr**: build `4a405846193f`, SDK `zephyr-sdk-1.0.1`
- **Console**: VCOM 2 at 115200 8N1

### Geometry

`prj.conf` pins blob_db to warnings so per-op messages cannot perturb
the timed loops, which also hides the two lines that confirm the
geometry. From a throwaway `CONFIG_BLOB_DB_LOG_LEVEL_INF=y` build:

```
partition 8388608 B, 128 sectors of 65536 B, 125 buckets
segments: chunk 2004 B, up to 128 per object (max object 256512 B)
```

The chunk is **2004 B**, not `sector/4`: the auto rule starts at
`sector/4` but clamps to what one slot can hold, and the index record is
itself a single-slot payload. Every number below depends on this.

## Small-blob point operations — the PR 2 result

| workload  |                `255ce7a` |                 `8428e35` |     Δ |
| --------- | -----------------------: | ------------------------: | ----: |
| `prepend` |  11.944 ops/s (83720 µs) |   47.169 ops/s (21200 µs) |  ×3.9 |
| `append`  |  11.960 ops/s (83610 µs) |   64.641 ops/s (15470 µs) |  ×5.4 |
| `read`    |  43.140 ops/s (23180 µs) | 2173.913 ops/s **(460 µs)** | **×50** |
| `update`  |  20.995 ops/s (47630 µs) |  425.531 ops/s (2350 µs)  | **×20** |
| `prepare` | 0.908 ops/s (1100940 µs) |  0.981 ops/s (1018620 µs) |     — |

Both runs are the **warm** configuration (the harness calls
`blob_db_prepare()` after each `erase_all()`), and both produced
identical checksums — `0xee3fa466` for prepend, `0x50f65666` for append
— so this is the same logical work in both columns.

**PR 2 is not a regression on real hardware; it is a 50× improvement on
reads.**

### Where the ×50 goes

PR 2 replaced one whole-sector read per lookup with many small reads. On
`native_sim` that measured as 6× more transactions for 47× fewer bytes,
and which term dominates is a property of the real part. This board
answers it: **bytes dominate, decisively.** A `read` at `8428e35` is 605
flash reads totalling 8650 B — 2.70× amplification over the 3200 B of
node data — and costs 460 µs. The baseline pulled a full 64 KB sector
per lookup and cost 23180 µs. Transaction overhead on this part is
nowhere near expensive enough to pay for 47× the bytes.

### Flash accounting (`8428e35`)

```
io read      : rd    605 ops/    8650 B   wr     0 ops/       0 B   er    0   ampl rd 2.70x wr 0.00x
io update    : rd    800 ops/   11400 B   wr   100 ops/    4800 B   er    0   ampl rd 3.56x wr 1.50x
```

Neither phase erases. These counters are deterministic and match the
`native_sim` run exactly, which is what makes them usable as a
regression gate.

## Large objects

These phases exist only at `8428e35`.

| phase                    |    per-op | throughput | flash ops                                                    |
| ------------------------ | --------: | ---------: | ------------------------------------------------------------ |
| `lg write` (cold)        | 38495250 µs |     1 KB/s | rd 168/2576 B, wr 269/269584 B, **er 133**, wr ampl 1.02×    |
| `lg rewrite` (warm)      |  4562250 µs |    14 KB/s | rd 1040/14616 B, wr 277/269712 B, **er 9**, wr ampl 1.02×    |
| `lg read` (64 B windows) |     3254 µs |    19 KB/s | rd 92135/11623044 B, **rd ampl 44.33×**                      |
| `lg pwrite` (64 B)       |  2336281 µs |          — | rd 1651/99708 B, wr 160/77912 B, er 64, ampl rd 48.68× wr 38.04× |

### Contract R2 holds — read cost is independent of offset

| quartile | per-op   | reads          |
| -------- | -------: | -------------- |
| `q0`     | 3187 µs  | 710/89224 B    |
| `q1`     | 3250 µs  | 714/91270 B    |
| `q2`     | 3625 µs  | 727/91426 B    |
| `q3`     | 3281 µs  | 727/91040 B    |

Four similar numbers, and crucially **not a rising series** — `q3` is
below `q2`. The ~14% spread at `q2` is run-to-run noise on a phase that
lasts ~100 ms in total. R2's claim survives on hardware.

### The erase dominates everything

A 64 KB sector erase costs **~1.06–1.09 s**, measured three independent
ways that agree:

- `prepare`: 100 bucket formats (one sector each) in 101.9–110.2 s
  → 1.02–1.10 s per sector
- `lg write` cold vs warm: 135732 ms of difference over 124 extra erases
  → 1.09 s per erase
- a full 8 MB partition erase (128 sectors) took 136 s → 1.06 s

Write amplification is **1.02×** — the segment layout is near-optimal in
bytes. Essentially all of the wall clock is erase, which is why cold and
warm large writes differ by 8.4× while writing the same bytes.

### Partial write vs whole-object rewrite: ×1.95, not ×1

The app prints:

```
partial vs whole-object write: 2336281 us vs 4562250 us/op  (1x)
```

The parenthesised ratio is integer-truncated and under-reports the
result — the real figure is **1.95×**. (The same line prints `11x` for an
`N_LARGE=1` build, where the whole-object cost is 39.7 s.) Worth fixing
in `src/main.c`, since this line is the headline for why `blob_db_write`
exists.

The win is real but modest at these parameters: a 64-byte `pwrite` still
pays **2 sector erases** (64 erases over 32 ops ≈ 2 s of the 2.34 s), so
it saves the data rewrite, not the erase. `blob_db_write` earns its keep
more in bytes written — 77912 B against 269712 B — than in time. A
partial write that lands inside a single already-erased chunk would show
the ratio the design intends; at `PART_LEN=64` spread across a 64 KB
object, it does not.

## Raw UART capture — `8428e35`

Warnings from the leftover foreign store on the partition are elided;
see "Downgrading" below.

```
*** Booting Zephyr OS build 4a405846193f ***
blob_db perf 1.0.0  (N_OPS=100  VAL_LEN=24  node=32 B)
bench prepare :  100 ops in  101862 ms  ->    0.981 ops/s  (  1018620 us/op)
bench prepend :  100 ops in    2120 ms  ->   47.169 ops/s  (    21200 us/op)
bench read   :  100 ops in      46 ms  -> 2173.913 ops/s  (      460 us/op)
   io read      : rd    605 ops/    8650 B   wr     0 ops/       0 B   er    0   ampl rd 2.70x wr 0.00x
bench update :  100 ops in     235 ms  ->  425.531 ops/s  (     2350 us/op)
   io update    : rd    800 ops/   11400 B   wr   100 ops/    4800 B   er    0   ampl rd 3.56x wr 1.50x
prepend checksum: 0xee3fa466
bench prepare :  100 ops in  110247 ms  ->    0.907 ops/s  (  1102470 us/op)
bench append :  100 ops in    1547 ms  ->   64.641 ops/s  (    15470 us/op)
bench read   :  100 ops in      46 ms  -> 2173.913 ops/s  (      460 us/op)
   io read      : rd    605 ops/    8650 B   wr     0 ops/       0 B   er    0   ampl rd 2.70x wr 0.00x
bench update :  100 ops in     248 ms  ->  403.225 ops/s  (     2480 us/op)
   io update    : rd    998 ops/   13776 B   wr   100 ops/    4800 B   er    0   ampl rd 4.30x wr 1.50x
append checksum:  0x50f65666

-- large objects (OBJ_LEN=65536  N_LARGE=4  N_PART=32  PART_LEN=64) --
bench lg write  :    4 ops in  153981 ms  ->      1 KB/s  ( 38495250 us/op)
   io lg write  : rd    168 ops/    2576 B   wr   269 ops/  269584 B   er  133   ampl rd 0.00x wr 1.02x
bench lg rewrite:    4 ops in   18249 ms  ->     14 KB/s  (  4562250 us/op)
   io lg rewrite: rd   1040 ops/   14616 B   wr   277 ops/  269712 B   er    9   ampl rd 0.05x wr 1.02x
bench lg read   : 4096 ops in   13331 ms  ->     19 KB/s  (     3254 us/op)
   io lg read   : rd  92135 ops/11623044 B   wr     0 ops/       0 B   er    0   ampl rd 44.33x wr 0.00x
lg read checksum: 0xca1e0000
bench lg pread q0:   32 ops in     102 ms  ->     19 KB/s  (     3187 us/op)
   io lg pread q0: rd    710 ops/   89224 B   wr     0 ops/       0 B   er    0   ampl rd 43.56x wr 0.00x
bench lg pread q1:   32 ops in     104 ms  ->     19 KB/s  (     3250 us/op)
   io lg pread q1: rd    714 ops/   91270 B   wr     0 ops/       0 B   er    0   ampl rd 44.56x wr 0.00x
bench lg pread q2:   32 ops in     116 ms  ->     17 KB/s  (     3625 us/op)
   io lg pread q2: rd    727 ops/   91426 B   wr     0 ops/       0 B   er    0   ampl rd 44.64x wr 0.00x
bench lg pread q3:   32 ops in     105 ms  ->     19 KB/s  (     3281 us/op)
   io lg pread q3: rd    727 ops/   91040 B   wr     0 ops/       0 B   er    0   ampl rd 44.45x wr 0.00x
bench lg pwrite :   32 ops in   74761 ms  ->      0 KB/s  (  2336281 us/op)
   io lg pwrite : rd   1651 ops/   99708 B   wr   160 ops/   77912 B   er   64   ampl rd 48.68x wr 38.04x
partial vs whole-object write: 2336281 us vs 4562250 us/op  (1x)
lg objects intact (65536 B each)
```

## Raw UART capture — `255ce7a` (pre-PR-2 baseline)

Only the `read` and `update` lines are comparable with the run above;
that commit has no large-object phases and no I/O counters.

```
*** Booting Zephyr OS build 4a405846193f ***
blob_db perf 1.0.0  (N_OPS=100  VAL_LEN=24  node=32 B)
bench prepare :  100 ops in  110094 ms  ->    0.908 ops/s  (  1100940 us/op)
bench prepend :  100 ops in    8372 ms  ->   11.944 ops/s  (    83720 us/op)
bench read   :  100 ops in    2318 ms  ->   43.140 ops/s  (    23180 us/op)
bench update :  100 ops in    4763 ms  ->   20.995 ops/s  (    47630 us/op)
prepend checksum: 0xee3fa466
bench prepare :  100 ops in  109913 ms  ->    0.909 ops/s  (  1099130 us/op)
bench append :  100 ops in    8361 ms  ->   11.960 ops/s  (    83610 us/op)
bench read   :  100 ops in    2319 ms  ->   43.122 ops/s  (    23190 us/op)
bench update :  100 ops in    4765 ms  ->   20.986 ops/s  (    47650 us/op)
append checksum:  0x50f65666
```

## Historical: cold vs warm, on an older harness

Measured against an earlier commit on Zephyr `34bd8ff000cc`, before the
large-object phases and I/O counters existed. Superseded by the tables
above, but kept because it isolates the `prepare` mechanism, which still
works the same way.

| workload | cold (no `prepare`) | warm (`prepare(N_OPS)`) | Δ     |
| -------- | ------------------: | ----------------------: | ----: |
| prepend  |         0.850 ops/s |             15.80 ops/s | ×18.6 |
| append   |         0.851 ops/s |             15.94 ops/s | ×18.7 |
| read     |        59.311 ops/s |            59.311 ops/s | —     |
| update   |        29.095 ops/s |            29.103 ops/s | —     |

The **cold** run pays a full 64 KB sector erase inside every
prepend/append, because each fresh id lands in a bucket whose header
isn't valid yet. The **warm** run hoists that cost out of the timed loop
via `blob_db_prepare(N_OPS)`, which pre-formats the 100 upcoming buckets
in one batch (~110 s once, reported as a separate `prepare` line). After
that every write is on the append-only path — no erase, no compaction.

`read` and `update` never triggered an erase in either configuration, so
they were already on the warm path — the numbers match to three
decimals.

One caveat on those prepend/append figures: the first `alloc_id` after
an `erase_all` re-persists the id ceiling, which rotates the master —
one 64 KB sector erase inside the timed loop. Amortized over 100 ops
that inflated the per-op cost by ~11 ms.

## Reproducing

```bash
# From the west workspace top-dir
west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/dk_perf app_perf
nrfutil device program --firmware build/dk_perf/zephyr/zephyr.hex \
    --options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE,reset=RESET_SYSTEM \
    --serial-number <your-jlink-sn>
```

Start the console capture **before** programming and let the
`reset=RESET_SYSTEM` above start the run; do not send a second reset,
which risks interrupting a QSPI sector erase.

```bash
nrfutil device list        # find the port labelled "vcom: 2"
stty -F /dev/ttyACM2 115200 raw -echo -icrnl -inlcr
cat /dev/ttyACM2 | tee dk_perf.log
```

The `/dev/ttyACM*` number is **not** stable — any other USB CDC device
that enumerates first shifts it (this run landed on `ttyACM3`). A silent
console is usually the wrong node, not a dead board.

Expect **5–15 minutes with long silent stretches.** The run performs
several hundred ~1 s sector erases; each `bench prepare` line alone is
~100 s of apparent silence. The app never exits — stop capturing after
the final `lg objects intact` line.

### Downgrading: erase the partition first

`CONFIG_BLOB_DB_LARGE_PAYLOADS=y` bumps the on-flash format major to 2.
A build that predates it knows only major 1 and correctly refuses to
mount the store, which is what a downgrade to `255ce7a` hits:

```
<err> blob_db: master 0: format major 2, this build knows 1
<err> blob_db: refusing to mount a foreign store (A=foreign B=foreign); use blob_db_format() to discard it deliberately
<err> app_perf: mount: -134
```

Nothing is damaged — the refusal is deliberate. But the advice in that
message cannot be followed: `blob_db_format()` opens with
`if (!st.mounted) return -ENODEV;`, so it is unreachable for exactly the
store that needs discarding. Erase the raw partition instead, from a
throwaway app that goes under the library:

```c
const struct flash_area *fa;
flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
flash_area_erase(fa, 0, fa->fa_size);   /* ~136 s for 8 MB */
```

A blank partition then mounts as `virgin partition; formatting`. The
same applies in reverse after running an older build.
