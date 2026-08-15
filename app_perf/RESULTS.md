# app_perf — reference results

Hardware-measured results for the workloads implemented by
`app_perf/src/main.c` (build/flash instructions in `RUN_ON_DK.md`). Keep
this file honest — update it when the shape of the benchmark changes or
when regenerating on new hardware.

Every wall-clock figure here comes from the board. On `native_sim` they
are all `0 ms`, because the flash simulator models no latency; only the
`io …` counters are meaningful there.

Four commits were measured on the same board, so each change under review
can be read straight off the tables:

- **`e80f404`** — main, after PR 10 merged. The current reference. Only
  `update` moved; see "Run 3 on main" below.
- **`12df53b`** — PR branch tip, run 2. Contains PR 5's one-entry index
  cache (`8f5b16b`); nothing after that commit touches the read path, so
  these numbers describe `8f5b16b` as well. See
  "The index cache" below.
- **`8428e35`** — *app_perf: add large-object phases and flash I/O
  accounting*, run 1. The pre-cache baseline.
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

| workload  |                `255ce7a` |                 `8428e35` |     Δ | main `e80f404` |
| --------- | -----------------------: | ------------------------: | ----: | -------------: |
| `prepend` |  11.944 ops/s (83720 µs) |   47.169 ops/s (21200 µs) |  ×3.9 |      21230 µs |
| `append`  |  11.960 ops/s (83610 µs) |   64.641 ops/s (15470 µs) |  ×5.4 |      15520 µs |
| `read`    |  43.140 ops/s (23180 µs) | 2173.913 ops/s **(460 µs)** | **×50** | **460 µs** |
| `update`  |  20.995 ops/s (47630 µs) |  425.531 ops/s (2350 µs)  | **×20** | **2510 µs** ⚠ |
| `prepare` | 0.908 ops/s (1100940 µs) |  0.981 ops/s (1018620 µs) |     — |    1096500 µs |

The last column is the current reference. ⚠ `update` is 6.4% slower on
main than at `8428e35` — a deliberate correctness cost, quantified in
"Run 3 on main" below.

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

These phases do not exist at `255ce7a`. The figures below are the
pre-cache baseline from `8428e35`; only `lg read` changes at the tip —
see "The index cache" for its run-2 values.

| phase                    |    per-op | throughput | flash ops                                                    |
| ------------------------ | --------: | ---------: | ------------------------------------------------------------ |
| `lg write` (cold)        | 38495250 µs |     1 KB/s | rd 168/2576 B, wr 269/269584 B, **er 133**, wr ampl 1.02×    |
| `lg rewrite` (warm)      |  4562250 µs |    14 KB/s | rd 1040/14616 B, wr 277/269712 B, **er 9**, wr ampl 1.02×    |
| `lg read` (64 B windows) |     3254 µs |    19 KB/s | rd 92135/11623044 B, **rd ampl 44.33×** → 1831 µs, 33.34× with the cache |
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
`N_LARGE=1` build, where the whole-object cost is 39.7 s.)

> **Fixed and confirmed on the board.** `src/main.c` now computes the
> ratio in hundredths and prints two decimals. Run 2 printed
> `partial vs whole-object write: 2314062 us vs 4536250 us/op  (1.96x)`.
> The capture below predates the fix and is left as measured. The
> truncation mattered because at these parameters it swallowed the entire
> result — this line is the headline for why `blob_db_write` exists, and
> it was reporting `1x`.

The win is real but modest at these parameters: a 64-byte `pwrite` still
pays **2 sector erases** (64 erases over 32 ops ≈ 2 s of the 2.34 s), so
it saves the data rewrite, not the erase. `blob_db_write` earns its keep
more in bytes written — 77912 B against 269712 B — than in time. A
partial write that lands inside a single already-erased chunk would show
the ratio the design intends; at `PART_LEN=64` spread across a 64 KB
object, it does not.

## The index cache (PR 5) — run 2 at `12df53b`

`8f5b16b` caches the last-read index record, one entry, keyed on id and
dropped by any append. Run 2 tested the predictions in `RUN_ON_DK.md`.
**The transaction prediction was exact; the byte prediction was not.**

| line                     | `8428e35` |  `12df53b` |  predicted | verdict            |
| ------------------------ | --------: | ---------: | ---------: | ------------------ |
| `io lg read` ops         |    92 135 | **30 755** |   ~30 700  | **hit**, to 0.2%   |
| `io lg read` bytes       | 11 623 044 | **8 742 276** | ~3 285 000 | **missed**, 2.7× high |
| `io lg read` ampl        |   44.33×  | **33.34×** |    ~12.5×  | **missed**         |
| `bench lg read`          |   3254 µs |  **1831 µs** |    ~920 µs | **missed**, ×1.78 not ×3.5 |
| `io lg pread q0..q3` ops | 710–727   | **710–727** |     ~240   | **no change at all** |

This is the outcome `RUN_ON_DK.md` named as falsifying: the ops dropped
~3× while the bytes barely moved. §3 of
`doc/proposals/2026-08-09-large-payloads-cost.md` is wrong about where
the bytes go, and the correction is below.

### The bytes were never in the index lookups

Per windowed read (4096 of them):

| | reads/op | bytes/op |
| --- | --: | --: |
| `8428e35` | 22.49 | 2838 B |
| `12df53b` |  7.51 | 2134 B |

The cache removed exactly two of the three bucket lookups, as designed —
22.5 → 7.5 reads. But that saved only **704 B/op, i.e. ~352 B per removed
lookup**, where §3's model assumed 2 × (280 B index + ~738 B scan) ≈
2036 B. So the slot-header scan in front of the index record is ~72 B,
not ~738 B: **the scan is cheap, and it was the whole basis of the
prediction.**

The 2134 B/op that remain are the chunk itself. Serving a 64 B window
reads one whole **2004 B** chunk plus ~130 B of headers. That sets a hard
amplification floor of 2004 / 64 = **31.3×**, so the measured 33.34× is
within 7% of the best achievable — and the predicted **12.5× was
unreachable at any cache hit rate**, since it sits below the floor. No
index cache can fix this; only sub-chunk reads or a chunk size matched to
the window would.

### Why `pread` did not move at all

`io lg pread` is byte-for-byte identical across the two runs. The phase
calls `blob_db_read(g_ids[n % N_LARGE], …)` (`src/main.c:462`), cycling
all four objects on consecutive calls, so a one-entry cache keyed on id
is evicted every single call — a 0% hit rate by construction.

That is the cache behaving as documented ("one entry is enough, because
the workload that hurts is repeated access to the *same* object"), not a
defect: `lg read`, which walks one object at a time, got the full 3×.
The prediction of ~240 ops simply overlooked that this phase interleaves
objects. Worth knowing before the cache is sized up: a second entry buys
nothing here either, since the phase touches four ids in rotation.

### Timing constants, fitted from the two runs

The `lg read` pair gives two equations in reads/op and bytes/op, which
solve to roughly:

- **~65 µs per flash read transaction**
- **~0.63 µs per byte** for these small reads (≈1.6 MB/s)

That is a useful calibration rather than a curiosity: it independently
predicts the small-blob `read` phase (6.05 reads and 86.5 B per op) at
~451 µs against 460–470 µs measured, within 4%. It also explains why the
×3 drop in transactions bought ×1.78 in time and not ×1 — transactions
are not free, they are simply not where the bytes are. Note the per-byte
figure applies to small reads; the 64 KB bucket read at `255ce7a` managed
0.35 µs/B, so bulk transfers do better.

### Regression check

Everything outside the read path is unchanged, as it should be: `io read`
(605/8650 B), `io update` (800/11400 B and 998/13776 B), `io lg write`
(er 133), `io lg rewrite` (er 9) and `io lg pwrite` (er 64) are identical
between runs, and all three checksums match (`0xee3fa466`,
`0x50f65666`, `lg read 0xca1e0000`). Wall-clock differences on the
untouched phases are ≤1%. Contract R2 still holds: 3218 / 3281 / 3656 /
3281 µs, flat and not rising.

## Run 3 on main (`e80f404`) — the cost of the `update` correctness fix

Three blob_db commits landed after run 2 — `8d54e7b` (data corruption on
extend, permanent segment leak), `b4ee453` (decide INDEXED from a
CRC-verified slot in `update()`), `b695bde` (five review findings) — 282
lines of `blob_db.c`. Exactly one benchmark line moved.

| line          |   `12df53b` |   `e80f404` | change            |
| ------------- | ----------: | ----------: | ----------------- |
| `io update` reads | 800 ops / 11400 B | **1000 ops / 14800 B** | **+2 reads, +34 B per op** |
| `io update` ampl  | rd 3.56×    | **rd 4.62×** | |
| `bench update`    | 2360 µs/op  | **2510 µs/op** | **+150 µs (+6.4%)** |

Everything else is unchanged: `io read` byte-identical at 605 ops / 8650 B
and 460 µs/op, `io lg read` byte-identical at 30755 ops / 8742276 B,
`io lg write` (er 133), `io lg rewrite` (er 9) and `io lg pwrite` (er 64)
all identical, all three checksums match, and R2 stays flat at 3187 /
3250 / 3625 / 3281 µs. Untouched wall-clock figures differ by ≤1.3%.

**The cost is exactly what the fix implies, and the fitted constants
predict it.** `b4ee453` makes `update()` re-read a slot and verify its CRC
before deciding the INDEXED flag, which is 2 extra read transactions and
34 extra bytes per op. At the ~65 µs/transaction and ~0.63 µs/B measured
in run 2, that predicts 2 × 65.5 + 34 × 0.63 = **152 µs** against **150 µs
measured** — a 1% error on an independent prediction. This is a
correctness fix being paid for in reads, at a price the cost model
anticipates rather than a surprise regression.

The second `update` phase shows the same shift (998 → 1198 reads,
2490 → 2650 µs), so it is systematic and not a one-phase artifact.

## Raw UART capture — `e80f404` (run 3, main)

```
*** Booting Zephyr OS build 4a405846193f ***
blob_db perf 1.0.0  (N_OPS=100  VAL_LEN=24  node=32 B)
bench prepare :  100 ops in  109650 ms  ->    0.911 ops/s  (  1096500 us/op)
bench prepend :  100 ops in    2123 ms  ->   47.103 ops/s  (    21230 us/op)
bench read   :  100 ops in      46 ms  -> 2173.913 ops/s  (      460 us/op)
   io read      : rd    605 ops/    8650 B   wr     0 ops/       0 B   er    0   ampl rd 2.70x wr 0.00x
bench update :  100 ops in     251 ms  ->  398.406 ops/s  (     2510 us/op)
   io update    : rd   1000 ops/   14800 B   wr   100 ops/    4800 B   er    0   ampl rd 4.62x wr 1.50x
prepend checksum: 0xee3fa466
bench prepare :  100 ops in  110064 ms  ->    0.908 ops/s  (  1100640 us/op)
bench append :  100 ops in    1552 ms  ->   64.432 ops/s  (    15520 us/op)
bench read   :  100 ops in      46 ms  -> 2173.913 ops/s  (      460 us/op)
   io read      : rd    605 ops/    8650 B   wr     0 ops/       0 B   er    0   ampl rd 2.70x wr 0.00x
bench update :  100 ops in     265 ms  ->  377.358 ops/s  (     2650 us/op)
   io update    : rd   1198 ops/   17176 B   wr   100 ops/    4800 B   er    0   ampl rd 5.36x wr 1.50x
append checksum:  0x50f65666

-- large objects (OBJ_LEN=65536  N_LARGE=4  N_PART=32  PART_LEN=64) --
bench lg write  :    4 ops in  154408 ms  ->      1 KB/s  ( 38602000 us/op)
   io lg write  : rd    168 ops/    2576 B   wr   269 ops/  269584 B   er  133   ampl rd 0.00x wr 1.02x
bench lg rewrite:    4 ops in   17918 ms  ->     14 KB/s  (  4479500 us/op)
   io lg rewrite: rd   1048 ops/   15744 B   wr   277 ops/  269712 B   er    9   ampl rd 0.06x wr 1.02x
bench lg read   : 4096 ops in    7458 ms  ->     34 KB/s  (     1820 us/op)
   io lg read   : rd  30755 ops/ 8742276 B   wr     0 ops/       0 B   er    0   ampl rd 33.34x wr 0.00x
lg read checksum: 0xca1e0000
bench lg pread q0:   32 ops in     102 ms  ->     19 KB/s  (     3187 us/op)
   io lg pread q0: rd    710 ops/   89224 B   wr     0 ops/       0 B   er    0   ampl rd 43.56x wr 0.00x
bench lg pread q1:   32 ops in     104 ms  ->     19 KB/s  (     3250 us/op)
   io lg pread q1: rd    714 ops/   91270 B   wr     0 ops/       0 B   er    0   ampl rd 44.56x wr 0.00x
bench lg pread q2:   32 ops in     116 ms  ->     17 KB/s  (     3625 us/op)
   io lg pread q2: rd    727 ops/   91426 B   wr     0 ops/       0 B   er    0   ampl rd 44.64x wr 0.00x
bench lg pread q3:   32 ops in     105 ms  ->     19 KB/s  (     3281 us/op)
   io lg pread q3: rd    727 ops/   91040 B   wr     0 ops/       0 B   er    0   ampl rd 44.45x wr 0.00x
bench lg pwrite :   32 ops in   72708 ms  ->      0 KB/s  (  2272125 us/op)
   io lg pwrite : rd   1651 ops/   99708 B   wr   160 ops/   77912 B   er   64   ampl rd 48.68x wr 38.04x
partial vs whole-object write: 2272125 us vs 4479500 us/op  (1.97x)
lg objects intact (65536 B each)
```

## Raw UART capture — `12df53b` (run 2, with the index cache)

```
*** Booting Zephyr OS build 4a405846193f ***
blob_db perf 1.0.0  (N_OPS=100  VAL_LEN=24  node=32 B)
bench prepare :  100 ops in  107166 ms  ->    0.933 ops/s  (  1071660 us/op)
bench prepend :  100 ops in    2107 ms  ->   47.460 ops/s  (    21070 us/op)
bench read   :  100 ops in      47 ms  -> 2127.659 ops/s  (      470 us/op)
   io read      : rd    605 ops/    8650 B   wr     0 ops/       0 B   er    0   ampl rd 2.70x wr 0.00x
bench update :  100 ops in     236 ms  ->  423.728 ops/s  (     2360 us/op)
   io update    : rd    800 ops/   11400 B   wr   100 ops/    4800 B   er    0   ampl rd 3.56x wr 1.50x
prepend checksum: 0xee3fa466
bench prepare :  100 ops in  109722 ms  ->    0.911 ops/s  (  1097220 us/op)
bench append :  100 ops in    1537 ms  ->   65.061 ops/s  (    15370 us/op)
bench read   :  100 ops in      47 ms  -> 2127.659 ops/s  (      470 us/op)
   io read      : rd    605 ops/    8650 B   wr     0 ops/       0 B   er    0   ampl rd 2.70x wr 0.00x
bench update :  100 ops in     249 ms  ->  401.606 ops/s  (     2490 us/op)
   io update    : rd    998 ops/   13776 B   wr   100 ops/    4800 B   er    0   ampl rd 4.30x wr 1.50x
append checksum:  0x50f65666

-- large objects (OBJ_LEN=65536  N_LARGE=4  N_PART=32  PART_LEN=64) --
bench lg write  :    4 ops in  154060 ms  ->      1 KB/s  ( 38515000 us/op)
   io lg write  : rd    168 ops/    2576 B   wr   269 ops/  269584 B   er  133   ampl rd 0.00x wr 1.02x
bench lg rewrite:    4 ops in   18145 ms  ->     14 KB/s  (  4536250 us/op)
   io lg rewrite: rd   1040 ops/   14616 B   wr   277 ops/  269712 B   er    9   ampl rd 0.05x wr 1.02x
bench lg read   : 4096 ops in    7501 ms  ->     34 KB/s  (     1831 us/op)
   io lg read   : rd  30755 ops/ 8742276 B   wr     0 ops/       0 B   er    0   ampl rd 33.34x wr 0.00x
lg read checksum: 0xca1e0000
bench lg pread q0:   32 ops in     103 ms  ->     19 KB/s  (     3218 us/op)
   io lg pread q0: rd    710 ops/   89224 B   wr     0 ops/       0 B   er    0   ampl rd 43.56x wr 0.00x
bench lg pread q1:   32 ops in     105 ms  ->     19 KB/s  (     3281 us/op)
   io lg pread q1: rd    714 ops/   91270 B   wr     0 ops/       0 B   er    0   ampl rd 44.56x wr 0.00x
bench lg pread q2:   32 ops in     117 ms  ->     17 KB/s  (     3656 us/op)
   io lg pread q2: rd    727 ops/   91426 B   wr     0 ops/       0 B   er    0   ampl rd 44.64x wr 0.00x
bench lg pread q3:   32 ops in     105 ms  ->     19 KB/s  (     3281 us/op)
   io lg pread q3: rd    727 ops/   91040 B   wr     0 ops/       0 B   er    0   ampl rd 44.45x wr 0.00x
bench lg pwrite :   32 ops in   74050 ms  ->      0 KB/s  (  2314062 us/op)
   io lg pwrite : rd   1651 ops/   99708 B   wr   160 ops/   77912 B   er   64   ampl rd 48.68x wr 38.04x
partial vs whole-object write: 2314062 us vs 4536250 us/op  (1.96x)
lg objects intact (65536 B each)
```

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

Nothing is damaged — the refusal is deliberate.

> **Fixed.** At the time of this run the advice in that message could not
> be followed: `blob_db_format()` opened with
> `if (!st.mounted) return -ENODEV;`, so the one store that needed
> discarding — the one that cannot be mounted — was the one store it
> refused. It now opens the partition itself when called unmounted,
> applies the same geometry checks a mount does, and leaves the DB
> mounted and usable. Two regression tests cover it
> (`test_format_discards_an_unmountable_store`,
> `test_format_on_a_mounted_store_stays_mounted`).

So on a current build, discarding a foreign store is just:

```c
blob_db_format();      /* no mount needed; ~136 s for 8 MB */
```

On the build used for this run, the workaround was to reach under the
library and erase the raw partition from a throwaway app:

```c
const struct flash_area *fa;
flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
flash_area_erase(fa, 0, fa->fa_size);
```

A blank partition then mounts as `virgin partition; formatting`. The
same applies in reverse after running an older build — and note the
older build cannot use the fixed `blob_db_format()`, since the fix is
not in it, so downgrades still need the raw erase.
