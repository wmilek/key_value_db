# `app_perf_l0` — reference results

Results for the sweep implemented by `app_perf_l0/src/main.c` and the model
fitted from it by `tools/l0_timing.py`. Keep this file honest — update it when
the shape of the sweep changes or when regenerating on new hardware.

**Status of the hardware measurement: not yet taken.** §4 states what the
board run is predicted to produce and what would falsify it, so the run has
something to disagree with. Everything in §1–§3 is measured or computed and
reproducible from this repository today.

## Setup

- **Host runs**: `native_sim`, Zephyr build `5058917ea61b`, host toolchain
- **Board the derived model describes**: nRF5340-DK (PCA10095) cpuapp, with
  `storage_partition` on the on-board MX25R6435F QSPI NOR — 8 MB, 64 KB erase
  blocks, `flash_area_align()` = 4, 8 MHz Quad-SPI. Same board and same
  captures as `app_perf/RESULTS.md`.

---

## 1. The size matrix, and what a flat substrate proves

The flash simulator charges one flat cost per call — `k_busy_wait()` of
`CONFIG_FLASH_SIMULATOR_MIN_{READ,WRITE,ERASE}_TIME_US`, with no per-byte and
no per-block term. That makes a `native_sim` run a test with a known answer.

```
west build -b native_sim key_value_db/app_perf_l0 && ./build/zephyr/zephyr.exe > l0_sim.log
```

```
-- read: cost vs transfer size --
     size      ops         us/op        KiB/s       ns/B   marginal ns/B
        1     4096         2.000          488    2000.00         -
        2     4096         2.000          976    1000.00         0.00
        3     4096         2.000         1464     666.66         0.00
        4     4096         2.000         1953     500.00         0.00
        6     4096         2.000         2929     333.33         0.00
        8     4096         2.000         3906     250.00         0.00
       12     4096         2.000         5859     166.66         0.00
       16     4096         2.000         7812     125.00         0.00
       24     4096         2.000        11718      83.33         0.00
       32     4096         2.000        15625      62.50         0.00
       48     4096         2.000        23437      41.66         0.00
       64     4096         2.000        31250      31.25         0.00
       96     4096         2.000        46875      20.83         0.00
      128     4096         2.000        62500      15.62         0.00
      192     4096         2.000        93750      10.41         0.00
      256     4096         2.000       125000       7.81         0.00
      384     4096         2.000       187500       5.20         0.00
      512     4096         2.000       250000       3.90         0.00
      768     4096         2.000       375000       2.60         0.00
     1024     4096         2.000       500000       1.95         0.00
     1536     4096         2.000       750000       1.30         0.00
     2048     4096         2.000      1000000       0.97         0.00
     3072     4096         2.000      1500000       0.65         0.00
     4096     4096         2.000      2000000       0.48         0.00
  marginal cost 0.00 .. 0.00 ns/B
  -> cost does not grow with size at all: this substrate charges per call.
```

Read the columns as a set. `us/op` is flat, `KiB/s` rises linearly with size
and `ns/B` falls as `1/n` — three different-looking curves that are all the
same statement, *cost does not depend on size*, which the marginal column says
outright. The write table is identical in shape at 100 µs/op, and the fit
recovers exactly what the simulator was configured with:

```
  class   fixed per call        per unit            points  max rel err
  read         2.000 us         0.0 ns/B       24       0.0 %
  write      100.000 us         0.0 ns/B       24       0.0 %
  erase     2000.000 us       0.000 ms/blk      7       0.0 %
  note (write): per-unit cost is below what this sweep can resolve; clamped to a flat per-call cost
```

The erase sweep says something else worth reading:

```
  erase                 1 blk x3              2.000 ms/call         2.000 ms/block
  erase                32 blk x3              2.000 ms/call         0.062 ms/block
  erase1                1 blk x32             2.000 ms/call  (spread 2.000..2.000 ms)
```

Erasing 32 blocks in one call costs the same as erasing one — the simulator
charges per *call*. That is a true statement about the simulator and a
dangerous one to carry to hardware, which is exactly why the sweep measures
erase span rather than assuming it.

**Run time**: 2.9 s, exits on its own.

### 1b. What a flat substrate cannot prove

A flat curve only shows that the fitter does not *invent* structure. It cannot
show that the fitter finds structure that is there — and in fact, while this
was being written, it did not: with relative weights spanning ten decades the
raw normal equations lost enough precision that a slope fitted to data lying
exactly on a line came out **negative**, tripped the non-negativity clamp, and
produced a flat model with 99.8 % error. The native_sim run above passed
throughout, because on a flat curve the wrong answer and the right one are the
same. The fit is now solved in centered form and the failure is a regression
test.

`tools/selftest.py` is that test. It generates captures from two synthetic
devices with identical read and erase costs and different write shapes, then
checks both the recovered coefficients and the reported verdict:

| device | write cost | must recover | must report |
|---|---|---|---|
| (a) affine | 90 µs + 29.53 µs/B | all four coefficients to ≤ 2 % | read **and** write affine; no staircase claimed; write residual < 5 % |
| (b) page-quantised | 90 µs + 7.56 ms per 256 B page touched, simulated over the app's actual access pattern | read and erase still exact | write **NOT affine**; staircase detected; write residual > 10 % |

Both pass. On device (b) the tool prints:

```
  marginal cost across the sweep (d ns / d B) — constant iff the cost is affine:
    read      631.00 ..    631.00 ns/B   -> affine
    write  -59062.50 ..  59062.50 ns/B   -> NOT affine: flat over part of the sweep, stepped elsewhere

  page-program staircase (page-aligned transfers, page = 256 B):
      size        us/op   programs  expected  verdict
        64 B     7650.000       1.00         1  step
       128 B     7650.000       1.00         1  step
       252 B     7650.000       1.00         1  step
       256 B     7650.000       1.00         1  step
       260 B    15210.000       1.99         2  step
       384 B    15210.000       1.99         2  step
       512 B    15210.000       1.99         2  step
       516 B    22770.000       2.98         3  step
       768 B    22770.000       2.98         3  step
      1024 B    30330.000       3.96         4  step
    -> write cost tracks ceil(size / page): it is a staircase, and the fitted
       per-byte slope is an average over it.
```

Note what the affine fit does to device (b) if read alone: **25 570 ns/B with a
66 % worst residual**. The slope is not meaningless — it is the correct average
over the staircase — but it misprices a 4 B write by a factor of 60. That is
the failure the marginal column exists to make visible, and it is why the fit's
residual is printed next to every model rather than filed away.

This is synthetic data and says nothing about any part. What it establishes is
that the analysis reports the shape it is given.

## 2. The geometry correspondence, verified

A prediction is only as good as the operation counts fed into it, and those are
a function of geometry: `blob_db` derives its bucket count, slot alignment and
segment size from what it finds at mount. The claim behind
`geometry/mx25r64.overlay` is that a `native_sim` build carrying the DK's shape
issues *the same operations* the DK issues.

That claim is checkable against `app_perf/RESULTS.md`, and it holds — all
twelve phases, byte for byte:

| phase | `native_sim` + `geometry/mx25r64.*` | DK `e80f404` |
|---|---|---|
| `read` | 605 rd / 8650 B | **identical** |
| `update` (prepend) | 1000 rd / 14800 B, 100 wr / 4800 B | **identical** |
| `update` (append) | 1198 rd / 17176 B, 100 wr / 4800 B | **identical** |
| `lg write` | 168 rd / 2576 B, 269 wr / 269584 B, 133 er | **identical** |
| `lg rewrite` | 1048 rd / 15744 B, 277 wr / 269712 B, 9 er | **identical** |
| `lg read` | 30755 rd / 8742276 B | **identical** |
| `lg pread q0..q3` | 710 / 714 / 727 / 727 rd | **identical** |
| `lg pwrite` | 1651 rd / 99708 B, 160 wr / 77912 B, 64 er | **identical** |

Without the overlay the same build mounts on 4 KB blocks, gets 2048 buckets
instead of 125 and a different segment chunk, and its counts describe a device
nobody owns. The check is not left to discipline: `blob_db`'s geometry line is
parsed out of the run log and compared against the model's, and
`--strict-geometry` makes a mismatch fatal.

## 3. A provisional model for the DK, and what it is worth

No `app_perf_l0` run has been taken on the board yet, but three `app_perf`
captures on the same board have — and each of their phases is one equation in
the six coefficients. `models/observations_nrf5340dk_flash_area.txt` holds
those captures verbatim; `derive` solves them.

```
python3 app_perf_l0/tools/l0_timing.py derive \
    app_perf_l0/models/observations_nrf5340dk_flash_area.txt \
    --block-bytes 65536 --part-bytes 8388608 --write-align 4 \
    --board nrf5340dk/nrf5340/cpuapp --name mx25r64_nrf5340dk_derived --loo \
    -o app_perf_l0/models/mx25r64_nrf5340dk_derived.json
```

| class | fixed per call | per unit | implied |
|---|---:|---:|---|
| read | 67.7 µs | 631 ns/B | 1548 KB/s ceiling; a read pays mostly overhead below **107 B** |
| write | 314 µs | 29.6 µs/B | 33 KB/s ceiling; overhead dominates only below **11 B** |
| erase | — | **1106 ms/block** | matches the ~1.07 s sector erase `app_perf/RUN_ON_DK.md` reports |

Fitted against **36 observations spanning 46 ms to 155 s** — three and a half
orders of magnitude — with a **median residual of 1.4 %** and a worst of 8.6 %.
Leave-one-out cross-validation (refit without a phase, then predict it) barely
degrades that: **median 1.4 %, worst 9.1 %**. The model is not memorising its
inputs.

The single worst phase in both directions is `lg pread q2`, under-predicted by
8 %, and it is under-predicted identically in all three captures — a systematic
term the affine model does not carry, not noise.

Two properties of this fit are structural rather than incidental:

- **The erase cost is attributed per block, not per call.** Every observation
  erased exactly one block per call, so the two terms are indistinguishable in
  this data. Attributing to the block is the safe half of that choice: a model
  that put the cost on the call would predict `blob_db_erase_all()` — one call
  over 128 blocks — as costing one block erase. The tool says so in the model's
  `notes`.
- **The fixed terms are upper bounds.** `derive` cannot separate flash time
  from the CPU time above it, so `blob_db`'s slot scan and CRC are folded into
  `R0` and `W0`. This is the main thing the hardware run in §4 will correct.

## 4. What the model predicts — and what the board run must show

Feeding the §2 `native_sim` run (which reports `0 ms` for every phase) through
the §3 model reproduces the DK's wall-clock:

```
phase             reads/bytes        writes/bytes      erases/blocks       read      write      erase        TOTAL
read                605/8650            0/0              0/0             46.4m       0.0m       0.0m     46.433 ms
update             1000/14800         100/4800           0/0             77.1m     173.3m       0.0m    250.375 ms
lg write            168/2576          269/269584       133/133           13.0m    8052.1m  147140.0m    155.205 s
lg rewrite         1048/15744         277/269712         9/9             80.9m    8058.4m    9956.8m     18.096 s
lg read           30755/8742276         0/0              0/0           7598.9m       0.0m       0.0m      7.599 s
lg pwrite          1651/99708         160/77912         64/64           174.7m    2353.0m   70804.2m     73.332 s
```

against measured `46 ms`, `251 ms`, `154.4 s`, `17.9 s`, `7.5 s`, `72.7 s`.
`verify` scores the whole set at a **median ratio of 1.01×**, worst 0.91×.

That agreement is *partly circular* — the model was solved from these same
phases — and the leave-one-out figure in §3 is the non-circular version of it.
What is not circular at all is the decomposition, which no stopwatch on the
board could have produced:

- `lg write` is **95 % erase**. Every microsecond of write-path tuning is
  chasing 5 % of it. The phase is saying *erase less*, not *write faster*.
- `lg rewrite` is 55 % erase / 45 % write, which is why it is 8.5× cheaper than
  `lg write` on identical write traffic: it erases 9 blocks instead of 133.
- `update` is 69–74 % write on 4800 B — 100 slot writes, each paying a fixed
  cost plus a page program.

### 4b. Is write linear? What the existing data already says

The matrix exists to answer this on the board, but the three captures already
constrain the answer, because they contain writes at two very different sizes.
Two hypotheses, both consistent with "NOR programs by page":

- **H1** — program time scales with the bytes actually written, so the cost is
  affine and the page structure does not show.
- **H2** — a page program costs the same whatever fraction of the page it
  writes, so the cost is `ceil(n/page)` programs: a staircase.

Subtract the modelled read and erase time from two measured phases and what is
left is the write time:

| phase | writes | mean size | write time left over |
|---|---:|---:|---:|
| `update` (prepend) | 100 | 48 B | 174.0 ms → **1.740 ms** each |
| `lg rewrite` | 277 | 974 B | 7883 ms → **28.46 ms** each |

H1 predicts both from one line:

| size | H1 predicts | measured | |
|---:|---:|---:|---|
| 48 B | 1.733 ms | 1.740 ms | **1.00×** |
| 974 B | 29.101 ms | 28.459 ms | **1.02×** |

H2 cannot. Calibrated on the 48 B write — one page, so one program costs
1.740 ms — a 974 B write touches 4 or 5 pages and should cost 6.96–8.70 ms. It
costs 28.46 ms: **4.1× more than the staircase allows.**

So the prediction for the board run is that **write on this part is affine in
bytes, not a staircase**, and specifically:

| table | predicted | what a different result would mean |
|---|---|---|
| `write` marginal column | roughly constant near 29.5 ns/B ×10³, verdict `-> affine` | if it is flat below 256 B and then steps, H2 is right and the model in §3 is fitting an artefact of the sizes those captures happened to use |
| `write_pg` staircase | **no staircase**: implied programs ≈ `n/page`, not `ceil(n/page)`, so the tool prints *"cost does NOT track ceil(size / page)"* | a clean staircase here falsifies H1 outright |
| `write` fixed term | ≲ 314 µs (§3's value includes `blob_db`'s CPU) | — |

Worth being clear about the status of this: it is an inference from two mean
transfer sizes, not a measurement of the curve. The whole point of the matrix
is that it samples 28 sizes with the midpoints in between, so it can see
structure that two points average over. **If the board run disagrees with the
table above, the board run is right** — and the disagreement is more
interesting than the agreement, because §3's model and every prediction built
on it assume H1.

### The falsifiable part

When `app_perf_l0` is run on the DK, its direct fit must agree with §3 on the
terms that are pure flash, and should come in **lower** on the terms that
absorbed CPU time:

| coefficient | derived (§3) | direct L0 fit should be | if it is not |
|---|---:|---|---|
| erase per block | 1106 ms | within a few % — `blob_db` adds no CPU to an erase | the captures' erases were not one block each after all |
| read per byte | 631 ns/B | within a few % | the read path's per-byte CPU is not negligible |
| read fixed | 67.7 µs | **lower** — the difference is `blob_db`'s per-read CPU | the QSPI per-call overhead really is that large (DPD exit is ~35 µs, so ~40–70 µs is plausible on its own) |
| write fixed | 314 µs | **lower**, same reason | as above |
| write per byte | 29.6 µs/B | within a few %, i.e. ~7.6 ms per 256 B page | slower than the MX25R64 typical page program by ~4×; either the part is running near its worst case or some of this is not page-program time |

That last row is the one to watch. 29.6 µs/B is well above the datasheet's
typical page-program figure, and `derive` cannot tell whether that is the part,
the driver, or `blob_db` — only a direct sweep can. It is the clearest reason
to run this app on hardware rather than to keep deriving.

## 5. Measured against the datasheet

`tools/l0_timing.py spec` compares a model against a part's specified
envelope. The spec lives in a file, not in the app — the app measures, and
what the numbers are supposed to be is a separate, citable claim:
[`models/mx25r6435f_datasheet.json`](models/mx25r6435f_datasheet.json),
transcribed from **Macronix MX25R6435F Rev. 1.6, August 08 2022, §15
(Erase and Programming Performance), p.75**.

```
python3 tools/l0_timing.py spec -m models/mx25r64_nrf5340dk_derived.json \
    --spec models/mx25r6435f_datasheet.json
```

```
  measured: erase 1106.3 ms per 65536 B block; program 7.566 ms per 256 B page;
            29.55 us per byte

  ultra_low_power  (Configuration Register-2 bit 1 = 0)
    parameter            measured        typ        max   verdict
    block erase 65536 B   1106.32ms   800.00ms  3500.00ms   typ..max (1.38x typ)
    page program 256 B       7.57ms     3.20ms    10.00ms   typ..max (2.36x typ)
    per byte programmed     29.55us    40.00us   100.00us   under typ (0.74x typ)

  high_performance  (Configuration Register-2 bit 1 = 1)
    block erase 65536 B   1106.32ms   480.00ms  3000.00ms   typ..max (2.30x typ)
    page program 256 B       7.57ms     0.85ms     4.00ms   OVER MAX (1.89x max)
    per byte programmed     29.55us    32.00us   100.00us   under typ (0.92x typ)

  -> consistent with ultra_low_power only
```

Three things come out of this.

**Reality matches the specification, but only in one of the two modes.**
Nothing exceeds an Ultra Low Power maximum. Against High Performance the page
program is **1.89× over max**, which is not a "slower than typical" — max is
quoted at 85 °C and minimum VCC, so a room-temperature measurement above it
cannot be explained by conditions.

**So the part is running in Ultra Low Power mode, and that was measured, not
assumed.** The mode is Configuration Register-2 bit 1, the L/H switch. It is a
*volatile* bit whose power-on value comes from the part's ordering code, Rev
1.6 removed the generic default from the datasheet, and Zephyr's
`nordic,qspi-nor` driver never writes CR2 — so a board runs whatever the part
powers up as, and nothing in the schematic or the devicetree says which that
is. The timing does.

This is worth knowing beyond bookkeeping: High Performance mode would cut the
64 KB erase from a typical 0.8 s to 0.48 s and the page program from 3.2 ms to
0.85 ms. `blob_db_prepare()` of 100 buckets is ~110 s of erase on this board;
the same work in High Performance mode is specified at ~48 s. Whether that
trade against idle current is worth making is a product decision, but it is
not currently *being* made — it is being defaulted into.

**Erase is unremarkable and the write path is not.** 1.106 s against a typical
0.8 s is the ordinary gap between a datasheet's 25 °C all-zero-pattern figure
and a real one. The write path is the interesting column, and it is where §4b's
question gets a second, independent answer:

- Per byte, the measurement is **0.74× the Byte Program typ** (29.55 µs vs
  40 µs). That is the right direction — one page-program command covering N
  bytes should beat N individual byte programs — but only by a quarter.
- Per page, the measurement is **2.36× the Page Program typ**. A full 256 B
  page program is specified at 3.2 ms typical and measures ~7.6 ms.

Those two facts together say the part is behaving much more like "40 µs per
byte" than like "3.2 ms per page, whatever the size" — which is exactly what
§4b concluded from the two mean write sizes, arrived at independently. If the
part charged a flat page program the 48 B write in `update` would cost 3.2 ms
typical; it costs 1.740 ms, *less than a typical full-page program*. A part
cannot program a partial page faster than a full one if the cost is per page.

The `write` and `write_pg` matrices settle it directly on the board, and §4b
says what each must show.

### What this does not check

The datasheet specifies the part's *internal* erase and program times. It says
nothing about the SPI bus time to shift data in, nor about the driver's
per-call overhead — and those are exactly the model's **fixed** terms (read
67.7 µs, write 314.5 µs), which is why `spec` refuses to compare them against
anything. The read slope (631 ns/B ≈ 1.55 MB/s, against 4 MB/s theoretical at
8 MHz Quad-SPI) is a bus-and-driver figure too, and §15 has no row for it.

One more datasheet row worth carrying into the erase-span sweep: **chip erase
is typ 120 s** in Ultra Low Power, while erasing the same 8 MB as 128 separate
64 KB blocks is 128 × 0.8 = 102 s typical. On this part a whole-device erase is
*not* a shortcut, so the answer to "is a 32-block erase cheaper than 32
one-block erases" is predicted to be **no** — and if the sweep finds a large
win, the driver is doing something the datasheet's command times do not
predict, which is worth knowing.

## 6. Feeding the simulator back

`simconf` turns a model into flash-simulator knobs, picking the operating point
from a run's actual operation mix:

```
# operating point: read at 232 B/op, write at 692 B/op, erase at 1.00 block(s)/call
CONFIG_FLASH_SIMULATOR_SIMULATE_TIMING=y
CONFIG_FLASH_SIMULATOR_MIN_READ_TIME_US=214
CONFIG_FLASH_SIMULATOR_MIN_WRITE_TIME_US=20762
CONFIG_FLASH_SIMULATOR_MIN_ERASE_TIME_US=1000000
# NOTE: the true erase cost is 1106 ms, above the Kconfig ceiling of 1 s; it is clamped.
```

This is a blunt instrument and the tool says so: the simulator has no per-byte
term, so every operation is charged the cost of the *average* one, and the
erase cost does not even fit in the Kconfig range. It is useful for pacing and
watchdog behaviour on a host build; it is not a substitute for `predict`,
which uses the whole model.

## Raw capture — `native_sim`, simulator defaults

Machine-readable records only; the human tables are in §1.

```
l0geom part_bytes=8388608 block_bytes=4096 blocks=2048 write_align=1 erased_val=0xff region_blocks=32 max_xfer=4096 program_page=256 cycles_per_s=1000000 source=flash_simulator timing=simulated board=native_sim/native
l0raw op=erase blocks=1 ops=3 total_ns=6000000 ns_per_op=2000000 ns_per_block=2000000 min_ns=2000000 max_ns=2000000
l0raw op=erase blocks=2 ops=3 total_ns=6000000 ns_per_op=2000000 ns_per_block=1000000 min_ns=2000000 max_ns=2000000
l0raw op=erase blocks=4 ops=3 total_ns=6000000 ns_per_op=2000000 ns_per_block=500000 min_ns=2000000 max_ns=2000000
l0raw op=erase blocks=8 ops=3 total_ns=6000000 ns_per_op=2000000 ns_per_block=250000 min_ns=2000000 max_ns=2000000
l0raw op=erase blocks=16 ops=3 total_ns=6000000 ns_per_op=2000000 ns_per_block=125000 min_ns=2000000 max_ns=2000000
l0raw op=erase blocks=32 ops=3 total_ns=6000000 ns_per_op=2000000 ns_per_block=62500 min_ns=2000000 max_ns=2000000
l0raw op=erase1 blocks=1 ops=32 total_ns=64000000 ns_per_op=2000000 ns_per_block=2000000 min_ns=2000000 max_ns=2000000
l0raw op=read size=1 ops=4096 total_ns=8192000 ns_per_op=2000
... 24 read rows: 1 2 3 4 6 8 12 16 24 32 48 64 96 128 192 256 384 512 768 1024 1536 2048 3072 4096, all 2000 ns/op
l0lin op=read cmarg_min=0 cmarg_max=0
l0raw op=write size=1 ops=500 total_ns=50000000 ns_per_op=100000
... 24 write rows over the same sizes, all 100000 ns/op
l0lin op=write cmarg_min=0 cmarg_max=0
l0raw op=write_pg size=64 ops=32 total_ns=3200000 ns_per_op=100000
... 10 page-aligned rows: 64 128 255 256 257 384 512 513 768 1024, all 100000 ns/op
l0raw op=write_unaligned size=256 ops=64 total_ns=6400000 ns_per_op=100000
l0raw op=write_unaligned size=512 ops=64 total_ns=6400000 ns_per_op=100000
l0raw op=write_unaligned size=4096 ops=31 total_ns=3100000 ns_per_op=100000
l0end status=0
```

Every row is flat because the simulator is flat. §1b is where the analysis is
exercised against curves that are not.
