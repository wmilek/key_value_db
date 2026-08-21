# `app_perf_l0` — reference results

Results for the sweep implemented by `app_perf_l0/src/main.c` and the model
fitted from it by `tools/l0_timing.py`. Keep this file honest — update it when
the shape of the sweep changes or when regenerating on new hardware.

**The hardware measurement has been taken** — §5, on the nRF5340-DK. §4 is
kept as it was written *before* that run, so the predictions can be scored
rather than quietly revised; §5 scores them, and two of the five failed. §5b
answers the linearity question the matrix was built for, §6 checks the result
against the part's datasheet, and §1–§3 remain reproducible on a host with no
board attached.

## Setup

- **Host runs**: `native_sim`, Zephyr build `5058917ea61b`, host toolchain
- **Board run** (§5): nRF5340-DK S/N 960115021, app at `6ca2f7d`, `flash_area`,
  capture and fitted model in
  [`models/`](models) as `observations_nrf5340dk_l0_direct.txt` and
  `mx25r64_nrf5340dk_direct.json`
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

### 4b. Does write cost scale with bytes or with pages? What the existing data already says

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

> **Scored in §5b: confirmed.** The board's write marginal cost is flat at
> 12 060–12 273 ns/B from 8 B to 64 KB, and a 4 B write costs 208 µs against a
> 256 B write's 3 251 µs. H1 holds; H2 is dead. The per-byte *value* predicted
> here was wrong by 2.4× for the reason §5 gives, but the shape was right.

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

## 5. Measured on the DK — the falsification of §4

Run on board 960115021 at `6ca2f7d`, `flash_area`, geometry as reported by the
app itself: 8 388 608 B partition, 128 blocks of 65 536 B, write align 4,
`source=hardware timing=real`. Capture and fitted model alongside this file.

The direct fit, from a sweep of L0 calls with no storage stack in the image:

| class | fixed per call | per unit | points | max rel err |
|---|--:|--:|--:|--:|
| read | **65.682 µs** | **258.3 ns/B** | 15 | 11.5 % |
| write | **157.129 µs** | **12 156.5 ns/B** | 15 | 3.7 % |
| erase | **51 643 µs** | **1 061.434 ms/blk** | 7 | 1.3 % |

Ceilings that follow: **read 3 781 kB/s, write 80 kB/s**, and a transfer pays
only its fixed cost below 254 B (read) or 13 B (write).

### Scorecard against §4

| coefficient | derived (§3) | **direct** | §4 predicted | verdict |
|---|--:|--:|---|---|
| erase per block | 1 106 ms | **1 061.4 ms** | within a few % | **held** (−4.0 %) |
| read fixed | 67.7 µs | **65.7 µs** | lower | held, but only −3 % |
| write fixed | 314 µs | **157.1 µs** | lower | **held** (−50 %) |
| read per byte | 631 ns/B | **258.3 ns/B** | within a few % | **failed** (−59 %) |
| write per byte | 29.6 µs/B | **12.16 µs/B** | the one to watch | **failed** (−59 %) |

**§4 predicted the CPU above L0 would show up in the fixed terms. It is mostly
in the per-byte terms.** `read fixed` barely moved, which rules out the reading
§4 offered — that the difference is `blob_db`'s per-read CPU — as the main
effect. The per-call overhead really is ~66 µs of QSPI.

The two failures are the same failure. Both per-byte coefficients are
overstated by almost exactly the same factor:

    read   258.3 / 631    = 0.409
    write 12 156 / 29 600 = 0.411

Two independent classes landing within 0.3 % of each other is not the part
behaving oddly; it is the *derived* fit unable to separate them. The 36
observations it was solved from come from stack phases where reads and writes
co-occur, so read-per-byte and write-per-byte are collinear there and the
excess CPU was distributed across both in proportion. Only a sweep that varies
one call at a time can break that, which is the case for this app stated as a
measurement rather than an argument.

The direct fit also finds a term the derived model does not have: **an erase
call costs 51.6 ms before it erases anything.** A 1-block erase is therefore
1.113 s against 1.061 s of block time, and the derived model folded that
overhead into its per-block figure — which is most of the 4 % it sits high by.

### What the model predicts about phases it never saw

Feeding `app_perf`'s `flash_area` I/O counters through the **direct** model.
This is not circular: the model comes from raw L0 calls, the counters from a
`blob_db` stack.

| phase | measured | L0 predicts | ratio | dominant term |
|---|--:|--:|--:|---|
| `lg pwrite` | 72.700 s | 72.343 s | **1.00×** | erase 98 % |
| `lg write` | 154.400 s | 151.370 s | **0.98×** | erase 98 % |
| `read` | 0.046 s | 0.042 s | 0.91× | read 100 % |
| `lg rewrite` | 17.900 s | 13.413 s | 0.75× | erase 75 % |
| `update` | 0.251 s | 0.144 s | **0.57×** | write 52 % |
| `lg read` | 7.500 s | 4.278 s | **0.57×** | read 100 % |

The split is exactly along the dominant term. **Where a phase is erase-bound,
L0 accounts for all of it. Where it is transfer-bound, L0 accounts for 57 %,
and the missing 43 % is CPU above L0** — slot walking, header parsing, CRC and
memcpy over the payload, none of which is flash.

That is the number this app was built to produce, and no stopwatch on the board
could have separated it.

### Two conclusions elsewhere in this repo are wrong because of it

**`app_perf/RESULTS.md` attributes ~7.6 ms to a 256 B page program** and calls
it ~4× the part's typical, "the limit is the part". The direct sweep says a page
program is **12.16 µs/B × 256 = 3.11 ms**, which is unremarkable for an
MX25R6435F. The part is behaving normally; the excess was software above L0.

**Write throughput to pre-erased blocks is ~80 kB/s at L0, not ~32 kiB/s.** The
32 kiB/s figure is what the *stack* achieves through `blob_db`; the flash will
take bytes 2.5× faster than that. Likewise reads: 3 781 kB/s at L0 against the
~1.55 MB/s the stack-level per-byte constant implies.

Both errors point the same way — cost measured at the top of a stack was
attributed to the bottom of it — and both were only visible once the bottom was
measured on its own.
### 5b. Does the cost scale with bytes, or with pages? The board's answer

The scorecard above is about coefficients. This is about *shape* — the question
the matrix exists to answer. Both tables below are computed from the same
capture; the `marginal ns/B` column is the extra cost of each extra byte
against the row above, which is constant iff the cost is affine.

**Write — flat to within 1.13× across four orders of magnitude:**

|  size |    µs/op |  KiB/s |     ns/B | marginal ns/B |
|------:|---------:|-------:|---------:|--------------:|
|     4 |  208.145 |     19 | 52036.25 |             — |
|     8 |  256.716 |     30 | 32089.50 |     12 142.75 |
|    16 |  353.196 |     44 | 22074.75 |     12 060.00 |
|    32 |  526.746 |     59 | 16460.81 |     10 846.88 |
|    64 |  913.831 |     68 | 14278.61 |     12 096.41 |
|   128 | 1692.147 |     74 | 13219.90 |     12 161.19 |
|   256 | 3251.139 |     77 | 12699.76 |     12 179.62 |
|   512 | 6391.252 |     78 | 12482.91 |     12 266.07 |
|  1024 | 12674.97 |     79 | 12377.90 |     12 272.88 |
|  4096 | 50354.00 |     79 | 12293.46 |     12 263.66 |
| 16384 | 200988.8 |     80 | 12267.38 |     12 256.21 |
| 65536 | 803588.9 |     80 | 12261.79 |     12 259.00 |

**Each extra byte costs the same, so there is no page staircase.** The
marginal cost sits between 12 060 and 12 273 ns/B for every step from 8 B to
64 KB — a 1.02× spread once the single noisy 32 B point is set aside, 1.13×
including it. A 4 B write costs 208 µs and a 256 B write costs 3 251 µs:
**15.6× more for 64× the bytes**. If the part charged one page program
regardless of size those two numbers would be equal.

#### Affine, not proportional — and the difference is not pedantry

That result is easy to state too strongly, so: **the cost is affine, `t(n) =
a + b·n`, with a fixed term that is not zero.**

```
write  t(n) = 157.1 µs + 12.156 µs/B · n
read   t(n) =  65.7 µs +  0.258 µs/B · n
```

What is constant is `b`, the *marginal* cost — the price of one more byte. That
is the property that rules out a staircase, and the property the prediction
identity in the README needs.

What is **not** constant is throughput, and the `KiB/s` column says so plainly:

| transfer | write KiB/s | fixed share | read KiB/s | fixed share |
|---:|---:|---:|---:|---:|
| 4 B | 19.0 | 76 % | 58.6 | 98 % |
| 16 B | 44.4 | 45 % | 197 | 95 % |
| 64 B | 66.8 | 17 % | 760 | 80 % |
| 256 B | 76.5 | 4.8 % | 1 897 | 50 % |
| 4 KB | 80.0 | 0.3 % | 3 560 | 5.8 % |
| 64 KB | 80.3 | 0.0 % | 3 766 | 0.4 % |

So "linear" in the everyday sense — *time proportional to size, speed the same
at every size* — is **false here, for both classes**. Write speed varies 4×
across the sweep and read speed varies 64×. A transfer pays its fixed cost in
full whether it moves 1 byte or 1 000, and that cost only stops mattering above
`a/b`: **12.9 B for write, 254.3 B for read**.

The read figure is the one with teeth. `blob_db`'s reads on this board average
8 650 B / 605 = **14.3 B**, which is deep inside the fixed-cost-dominated
regime: 201 KiB/s, or **5.3 % of the 3 781 KiB/s the same flash delivers to
large transfers**. Its writes average 48 B and fare much better at 79 % of
their asymptote, because write's fixed term is small next to its per-byte cost.
Reads are where batching would pay, and this is the measurement that says so —
not the slope, but the slope *together with* the fixed term the KiB/s column
exposes.

That is §4b's prediction (**H1**) confirmed by direct measurement rather than
inferred from two mean sizes, and it settles the modelling question the whole
affine approach rests on for this part.

Read is the same story with a noisier small end:

|  size |    µs/op |  KiB/s |     ns/B | marginal ns/B |
|------:|---------:|-------:|---------:|--------------:|
|     4 |   61.594 |     63 | 15398.50 |             — |
|    16 |   71.151 |    220 |  4446.94 |      1 022.38 |
|    64 |   83.951 |    744 |  1311.73 |        251.53 |
|   256 |  144.081 |   1735 |   562.82 |        253.57 |
|  1024 |  330.076 |   3030 |   322.34 |        248.34 |
|  4096 | 1108.127 |   3610 |   270.54 |        253.28 |
| 16384 | 4225.297 |   3787 |   257.89 |        253.88 |
| 65536 | 16677.86 |   3837 |   254.48 |        253.60 |

From 64 B upward the marginal cost is **252.9–253.9 ns/B, a 1.004× spread** —
about as constant as a measurement gets, so read has no size structure either.
Its throughput still climbs 64× across the sweep, for the reason above: the
65.7 µs fixed term dwarfs the per-byte term until 254 B. Below 64 B the marginal
column scatters (the 16 B step
reads 1 022 ns/B), which is per-call variance showing through a difference
between two nearly equal numbers, not structure: the underlying `µs/op` values
there differ by a few microseconds on a ~65 µs fixed cost. This is why the fit
reports 11.5 % worst residual for read against 3.7 % for write, and why it is
worth reading the residual table rather than the headline slope.

**Read saturates at 3 837 KiB/s = 3.93 MB/s**, against 4 MB/s theoretical for
8 MHz Quad-SPI at 4 bits/clock — **98 % of the bus**. There is nothing left to
win in the L0 read path on this board; a faster `sck-frequency` is the only
lever.

### What this capture cannot show

It was taken at `6ca2f7d`, before the sweep became a matrix, so it has powers
of two only — no 1.5× midpoints and no `write_pg` phase. The tables above are
therefore computed *from* it rather than printed *by* it, and two gaps remain:

- **A staircase between two doublings would be invisible here**, which is the
  precise failure mode the midpoints were added for. The 12 060–12 273 ns/B
  flatness across 8→65536 makes a hidden staircase implausible — a page-quantised
  cost cannot be flat across a 256 B boundary at all — but "implausible" is not
  "measured".
- **`write_pg` never ran**, so the page-aligned staircase test has no data.
  Under this result it should show implied programs tracking `n/page`, not
  `ceil(n/page)`, and the tool should print *"cost does NOT track
  ceil(size / page)"*.

A re-run on the current app closes both. Nothing in the fitted model changes
with it — the coefficients come from the same 15 points either way — so this is
a completeness item, not a correction.

## 6. Measured against the datasheet

`tools/l0_timing.py spec` compares a model against a part's specified
envelope. The spec lives in a file, not in the app — the app measures, and what
the numbers are *supposed* to be is a separate, citable claim:
[`models/mx25r6435f_datasheet.json`](models/mx25r6435f_datasheet.json),
transcribed from **Macronix MX25R6435F Rev. 1.6, August 08 2022, §15 (Erase and
Programming Performance), p.75**.

```
python3 tools/l0_timing.py spec -m models/mx25r64_nrf5340dk_direct.json \
    --spec models/mx25r6435f_datasheet.json
```

```
  measured: erase 1061.4 ms per 65536 B block; program 3.112 ms per 256 B page;
            12.16 us per byte

  ultra_low_power  (Configuration Register-2 bit 1 = 0)
    parameter            measured        typ        max   verdict
    block erase 65536 B   1061.43ms   800.00ms  3500.00ms   typ..max (1.33x typ)
    page program 256 B       3.11ms     3.20ms    10.00ms   under typ (0.97x typ)
    per byte programmed     12.16us    40.00us   100.00us   under typ (0.30x typ)

  high_performance  (Configuration Register-2 bit 1 = 1)
    block erase 65536 B   1061.43ms   480.00ms  3000.00ms   typ..max (2.21x typ)
    page program 256 B       3.11ms     0.85ms     4.00ms   typ..max (3.66x typ)
    per byte programmed     12.16us    32.00us   100.00us   under typ (0.38x typ)

  -> consistent with ultra_low_power and high_performance; this measurement
     does not separate them.
```

**Reality matches the specification, everywhere, with room to spare.** Nothing
approaches a maximum in either mode. The headline is the page program: a full
256 B page measures **3.11 ms against a 3.2 ms Ultra Low Power typical — 0.97×,
within 3 % of the datasheet's typical figure**, taken at room temperature where
"typ" is defined. The erase sits at 1.33× the ULP typical, which is the ordinary
gap between a 25 °C all-zero-pattern number and a real one.

The per-byte figure is 0.30× the Byte Program typical (12.16 µs against 40 µs),
and that is the expected direction: one page-program command covering N bytes
should beat N individual byte programs, and here it does so by about 3×.

**Which mode? Ultra Low Power, on the weight of evidence — but this does not
prove it.** Both modes stay inside their maxima, so neither is excluded. What
separates them is distance from typical: on page program ULP is 0.97× and HP is
3.66×; on erase ULP is 1.33× and HP is 2.21×. Both parameters point the same
way, and ULP is where the part would sit if nothing set the bit — which is the
case, since the L/H switch is a *volatile* Configuration-Register-2 bit whose
power-on value comes from the ordering code, and Zephyr's `nordic,qspi-nor`
driver never writes CR2.

> **Correction.** An earlier revision of this section reached the same
> conclusion far too strongly, from the §3 *derived* model: it put the page
> program at 7.57 ms, which is 1.89× over the High Performance maximum, and
> concluded that HP was excluded outright. §5 shows why that was wrong — the
> derived per-byte terms are inflated ~2.4× by `blob_db` CPU that the stack-level
> fit could not separate from flash time. With the direct measurement the
> exclusion disappears and only a preponderance argument remains. The lesson is
> the one §5 draws twice: a stack-level fit must not be pointed at a datasheet,
> because the datasheet specifies the part and the fit describes the part plus
> everything above it.

Worth acting on regardless: **High Performance mode is being defaulted into
rather than chosen.** It is specified to cut the 64 KB erase from 0.8 s typical
to 0.48 s and the page program from 3.2 ms to 0.85 ms. `blob_db_prepare()` of
100 buckets is ~110 s of erase on this board, and the same work in HP mode is
specified at ~66 s. Whether that is worth the idle-current trade is a product
decision; right now it is not being made.

### What this does not check

The datasheet specifies the part's *internal* erase and program times. It says
nothing about the SPI bus time to shift data in, nor about the driver's
per-call overhead — and those are exactly the model's **fixed** terms
(read 65.7 µs, write 157.1 µs), which is why `spec` refuses to compare them
against anything.

The read path has no §15 row at all, and its own ceiling is the better
yardstick: **3 837 KiB/s measured against 4 MB/s theoretical at 8 MHz
Quad-SPI — 98 % of the bus.**

One more row worth carrying into the erase-span sweep: **chip erase is typ
120 s** in Ultra Low Power, while erasing the same 8 MB as 128 separate 64 KB
blocks is 128 × 0.8 = 102 s typical. On this part a whole-device erase is not a
shortcut — and the board run agrees: per-block cost is 1 111 ms for a 1-block
call and 1 059 ms within a 32-block call, a 4.7 % saving that comes from
amortising the 51.6 ms per-call overhead, not from a cheaper erase command.

## 7. Feeding the simulator back

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

