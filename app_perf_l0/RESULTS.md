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

Why the numbers are what they are — the driver code and datasheet clauses
behind them — is in [`FINDINGS.md`](FINDINGS.md).

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
L0 accounts for all of it. Where it is transfer-bound, L0 accounts for 57 %**,
and the remainder was originally read here as CPU above L0 — slot walking, header parsing, CRC and
memcpy over the payload, none of which is flash.

> **§5e qualifies this.** The read sweep is aligned and `blob_db` is not, and a misaligned start costs 42–65 % more. Much of this 43 % may be misalignment *at* L0 rather than CPU above it.

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

### 5c. Does the straight line actually fit? Every point, measured vs fitted

A fitted slope is a claim about data. This is the data it was made from — the
table `l0_timing.py show` now prints unconditionally, because a model without
it is an assertion.

**Write — `t(n) = 157.129 µs + 12 156.49 ns/B · n`**

|     n | measured | fitted | error |
|------:|---------:|-------:|------:|
|     4 B |    208.146 µs |    205.755 µs |  −1.1 % |
|     8 B |    256.717 µs |    254.381 µs |  −0.9 % |
|    16 B |    353.196 µs |    351.632 µs |  −0.4 % |
|    32 B |    526.746 µs |    546.136 µs |  **+3.7 %** |
|    64 B |    913.832 µs |    935.144 µs |  +2.3 % |
|   128 B |   1692.147 µs |   1713.159 µs |  +1.2 % |
|   256 B |   3251.139 µs |   3269.190 µs |  +0.6 % |
|   512 B |   6391.253 µs |   6381.251 µs |  −0.2 % |
|  1024 B |  12674.967 µs |  12605.373 µs |  −0.5 % |
|  4096 B |  50354.003 µs |  49950.105 µs |  −0.8 % |
| 16384 B | 200988.769 µs | 199329.034 µs |  −0.8 % |
| 65536 B | 803588.867 µs | 796844.749 µs |  −0.8 % |

**Erase — `t(m) = 51.643 ms + 1061.434 ms/block · m`**

| blocks | measured | fitted | error |
|------:|---------:|-------:|------:|
|  1 | 1107.133 ms | 1113.077 ms | +0.5 % |
|  1 | 1111.237 ms | 1113.077 ms | +0.2 % |
|  2 | 2202.179 ms | 2174.510 ms | −1.3 % |
|  4 | 4316.722 ms | 4297.378 ms | −0.4 % |
|  8 | 8552.541 ms | 8543.112 ms | −0.1 % |
| 16 | 16913.066 ms | 17034.581 ms | +0.7 % |
| 32 | 33893.382 ms | 34017.519 ms | +0.4 % |

Write and erase are straight lines to within **3.7 %** and **1.3 %** worst
case, and write is within **0.8 % everywhere above 512 B**. For these two the
interpolation is not an approximation worth worrying about.

**Read — `t(n) = 65.682 µs + 258.27 ns/B · n` — is not, in the middle:**

|     n | measured | fitted | error |
|------:|---------:|-------:|------:|
|     4 B |     61.594 µs |     66.715 µs |  **+8.3 %** |
|     8 B |     62.973 µs |     67.749 µs |  **+7.6 %** |
|    16 B |     71.152 µs |     69.815 µs |  −1.9 % |
|    32 B |     75.903 µs |     73.947 µs |  −2.6 % |
|    64 B |     83.951 µs |     82.212 µs |  −2.1 % |
|   128 B |    111.624 µs |     98.741 µs | **−11.5 %** |
|   256 B |    144.082 µs |    131.800 µs |  **−8.5 %** |
|   512 B |    202.929 µs |    197.917 µs |  −2.5 % |
|  1024 B |    330.077 µs |    330.152 µs |  +0.0 % |
|  4096 B |   1108.127 µs |   1123.560 µs |  +1.4 % |
| 65536 B |  16677.856 µs |  16991.725 µs |  +1.9 % |

Above 512 B the read fit is within 2 %. Between 64 B and 256 B it is not: the
line under-predicts by up to 11.5 %, because the measured cost **steps** there.
The marginal cost across 64→128 B is 432 ns/B against ~253 ns/B everywhere
else, and 8→16 B reads 1 022 ns/B. Something in the read path costs extra at
those sizes that a single slope has to average away, and averaging it away is
what produces the +8 % at 4–8 B on the other side of the same fit.

This is a **single run and one point per size**, so the two anomalies are worth
confirming before theorising about the driver. They are not timer resolution:
the 128 B point averages 409 operations over 45.7 ms against a 30.5 µs tick,
so the quantisation is 0.07 %. The 1.5× midpoints added since this capture
would localise the step to within 1.5× instead of 2×, which is the first thing
a re-run buys.

**What it means for predictions.** The model's error on a workload is the error
at the sizes that workload uses, not the headline residual. `blob_db`'s mean
read is 14.3 B, where the fit is good (−1.9 % at 16 B). A caller doing 128 B
reads gets predictions ~11 % low. §5's phase table is consistent with that:
read-dominated phases land at 0.57× because of CPU above L0, not because of
this — but a 10 % modelling error sits underneath that number and should not be
mistaken for signal.

### 5d. The matrix re-run — both gaps closed

Re-run on the current app at `90f75d4`, same board and geometry. The sweep is
now a matrix: **32 read points, 28 write, 10 `write_pg`, 7 erase**, with the
1.5× midpoints and the page-aligned staircase probe that the `6ca2f7d` capture
could not carry.

Coefficients reproduce, which is the first thing to check — a finer grid over
the same part should not move them:

| class | `6ca2f7d` (15 pts) | **matrix (28–32 pts)** | Δ |
|---|--:|--:|--:|
| read per byte | 258.3 ns/B | **256.0 ns/B** | −0.9 % |
| write per byte | 12 156.5 ns/B | **12 182.9 ns/B** | +0.2 % |
| erase per block | 1 061.4 ms | **1 087.6 ms** | +2.5 % |
| read fixed | 65.7 µs | 68.1 µs | +3.7 % |
| write fixed | 157.1 µs | 158.2 µs | +0.7 % |

**The erase intercept is not identified by either run** — 51.6 ms against
8.2 ms — because every erase point is dominated by its per-block term. What both
agree on is the sum: a 1-block erase is 1 113 ms one way and 1 096 ms the other,
against 1 107–1 111 ms measured directly. Quote the 1-block figure, not the
split.

#### `write_pg`: no staircase, measured

The test §5b had no data for. A page-quantised cost would make 252 B and 256 B
identical and jump at 260 B:

| size | µs/op | implied programs | a staircase needs |
|--:|--:|--:|--:|
| 252 B | 3 200.5 | 3.47 | 1 |
| 256 B | 3 251.1 | 3.53 | 1 |
| 260 B | 3 346.4 | 3.63 | **2** |
| 512 B | 6 392.5 | 6.94 | 2 |
| 516 B | 6 479.3 | 7.03 | **3** |
| 1 024 B | 12 671.5 | 13.75 | 4 |

252 → 256 costs +1.6 % for +1.6 % more bytes; 512 → 516 costs +1.4 % for
+0.8 %. The tool prints *"cost does NOT track ceil(size / page); the part or
driver is not programming a page at a time."*

**H1 is now measured, not inferred.** §5b reached the same conclusion from the
flatness between doublings and said plainly that "implausible is not measured".
It is measured.

Stated in the terms `a170069` insists on: each extra byte costs the same
whatever offset it lands at, which is what the staircase question asked. It does
*not* mean a write of 260 B costs 260/256 of a 256 B write — it costs
158.2 µs + 12.183 µs/B either way, and the fixed term is 4.9 % of a 256 B write.

#### The marginal-cost verdict is a false negative

The tool reports `write` **"NOT affine, 1.50x spread"** and `read` **"NOT
affine: flat over part of the sweep, stepped elsewhere"**. Both are artefacts of
the smallest points, and the curves are clean wherever the measurement is:

| | flat above | marginal | spread |
|---|--:|--:|--:|
| write | 96 B | 12 004 – 12 350 ns/B | **±1.4 %** |
| read | 512 B | 241 – 258 ns/B | ±3 % |

Below those sizes the `write` marginals *alternate* high and low — 14 312,
9 835, 12 153, 9 568, 13 500, 10 693 — which is the signature of rounding rather
than structure, and `read` produces **negative** marginals (−18 752 ns/B at 8 B)
that no physical cost can explain.

It is not timer resolution: each point averages 546–819 repetitions, so per-op
resolution is ~7 ns against a 30.5 µs tick. They are outliers — a 6 B read
measured 112.8 µs where its neighbours at 4 B and 8 B measured 78.7 and 75.3,
and 12 B measured 63.2. A first-difference test over adjacent points has no
defence against one bad batch, and at these sizes the whole signal is smaller
than the outlier.

That is a defect in the check, not in the model: the fitted coefficients are
unaffected, because the fit is weighted least squares over all points rather
than a first difference between neighbours.

#### Fixed: the verdict is now gated

`gated_marginals()` replaces the adjacent-pair range with two structural gates,
neither of them a tuned constant:

- **only above the fixed-cost crossover, n ≥ a/b** — the same crossover the
  throughput lines already print (266 B read, 13 B write). Below it the byte
  term is a minority of the measurement, so a difference there is mostly noise.
- **pair across an octave, not with the neighbour** — each surviving point pairs
  with the first later point at ≥ 2× its size, so Δn ≥ n and noise passes
  through at ~2ε instead of being amplified by n/Δn.

The device's ungated range is still printed beside it, because it is what the
board measured and a reader should see what the gate removed.

| | device, adjacent, ungated | **gated, octave pairs** | verdict |
|---|--:|--:|---|
| read | −18 752 … 17 075 | **244.1 … 272.0 ns/B** | affine |
| write | 9 568 … 14 312 | **10 860.9 … 12 278.6 ns/B** | affine |

It reproduces on the `6ca2f7d` capture, which has no `l0lin` line at all —
read 229.9–253.9, write 10 846.9–12 272.9, both affine. Two captures taken a day
apart, one coarse and one fine, agreeing on the shape is worth more than either.

**A staircase still trips it**, which is the property that matters: secants
across an octave of a page-quantised cost disagree with each other (29.5 against
19.7 µs/B for a 256 B page), and `tools/selftest.py` covers exactly that — its
page-wise synthetic device is still reported non-affine, and its affine device
still reports affine.

**What the device now contributes.** Each sweep point is measured
`CONFIG_APP_PERF_L0_PASSES` times (default 3), the mean is reported and the
extremes are printed as a `spread` column and emitted as `min_ns`/`max_ns`. The
board can therefore gate its own adjacent-pair range on each point's measured
pass-to-pass spread, rather than on a rule of thumb — a step counts only when
the change it measures exceeds the noise of the two points that formed it. The
tool reads `used`/`skipped` off the `l0lin` line and labels the device range
accordingly, so "ungated" is said only of captures that genuinely could not
gate.

That does not replace `gated_marginals()`, and the two are not alternatives.
The device's estimator is still a first difference between neighbours, which is
fragile however well gated; the octave pairing changes the estimator itself.
The device gate makes the printed range honest, and the octave pairing makes
the verdict sound.

**One capability worth being explicit about, because it was nearly lost.** An
earlier attempt at this fix replaced the range with a drift test — median step
against the whole-sweep slope. It removes the false negative just as well, and
it *cannot detect a staircase*: a page-quantised cost sampled on a geometric
ladder zigzags symmetrically, because powers of two divide the page onto one
program while the midpoints straddle onto two, so the median still matches the
overall slope while the fit residual runs to 66 %. Pairing across an octave
keeps the sensitivity that drift throws away, which is why it is the one that
shipped.


### What the `6ca2f7d` capture could not show

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

**Both are closed by §5d**, which is the re-run. Nothing in the fitted model
changed with it — the coefficients come from the same part either way, and
reproduced to within 0.9 % on read and 0.2 % on write — so this was a
completeness item, not a correction. It is kept because the reasoning it
records ("implausible is not measured") is the reason the re-run happened.

### 5e. The full-resolution run — 112 read sizes, 96 write

Run at `04ae252` with `STEPS_PER_OCTAVE=8` and `PASSES=3`: **112 read points,
96 write, 49 word-boundary, 4 offset-alignment**, against the 15 per class the
first capture carried. Capture and model under `models/`.

#### The coefficients survive a 7× denser ladder and a changed procedure

This is the check that matters, because `0e0b48b` did not only add points — it
stopped erasing per write point, laying the sweep end to end through a
pre-erased region instead. A procedure change and a density change at once
could easily have moved the fit.

| | 15 pts | 32 pts | **112 pts** | total drift |
|---|--:|--:|--:|--:|
| read per byte | 258.3 | 256.0 | **253.8 ns/B** | −1.7 % |
| read fixed | 65.7 | 68.1 | **69.2 µs** | +5.3 % |
| write per byte | 12 156.5 | 12 182.9 | **12 222.4 ns/B** | +0.5 % |
| write fixed | 157.1 | 158.2 | **159.4 µs** | +1.5 % |
| erase per block | 1 061.4 | 1 087.6 | **1 086.3 ms** | +2.3 % |

The per-byte terms move by under 2 % across all three. The fixed terms drift
slightly upward as the ladder fills in below the crossover, which is what an
intercept does when it is asked to carry more points that mostly measure it.

#### The gated verdict tightens by two orders of magnitude

| capture | octave pairs | read range | spread |
|---|--:|--:|--:|
| `6ca2f7d` (15 pts) | 8 | 229.9 – 253.9 ns/B | 1.104× |
| matrix (32 pts) | 14 | 244.1 – 272.0 ns/B | 1.114× |
| **full (112 pts)** | **56** | **252.76 – 253.50 ns/B** | **1.003×** |

Write is 11 004.5 – 12 315.3 ns/B over 85 pairs, also affine. Meanwhile the
device's own noise-gated adjacent-pair range still spans **210 – 17 216 ns/B**
on reads, so the octave pairing is what makes a verdict possible at all; the
extra density does not rescue a first difference, it only makes the gated
estimator sharper. `write_pg` is unchanged: still no staircase.

#### What the resolution was for: alignment is not free

The read sweep is aligned, and the fitted 253.8 ns/B is therefore the *aligned*
cost. The offset probe holds the size at 256 B and shifts only the start:

| start offset | µs/op | vs aligned |
|--:|--:|--:|
| 0 | 132.3 | — |
| 1 | 187.4 | **+42 %** |
| 2 | 218.5 | **+65 %** |
| 3 | 187.4 | +42 % |

Length matters independently, and multiples of four are cheapest:

| n (B) | 14 | 15 | **16** | 17 | 18 | 19 | **20** |
|---|--:|--:|--:|--:|--:|--:|--:|
| µs/op | 97.9 | 78.1 | **70.3** | 94.7 | 82.7 | 82.9 | **76.3** |

The same shape repeats at 64 (86.0 against 95–107 either side) and at 256
(134.8 against 147–161). The driver works in words, and a transfer pays for
both a misaligned start and a non-word length.

#### This softens §5's claim about where the missing time goes

§5 reports that transfer-bound phases predict at 0.57× and concludes **"the
missing 43 % is CPU above L0"**. That is now too confident.

`blob_db` reads at arbitrary offsets inside slots; the model it is being scored
against was fitted on aligned transfers. If those reads pay the ~1.5× the offset
probe measures, `lg read` predicts 4.278 × 1.5 = **6.4 s against 7.5 s measured,
a ratio of 0.86× rather than 0.57×** — and misalignment at L0 accounts for
roughly three quarters of what was attributed to CPU above it.

That is arithmetic on a hypothesis, not a result: this app does not know
`blob_db`'s offset distribution, and nothing here has measured it. But the
claim as written is no longer supported, and the way to settle it is an
alignment histogram at the store seam rather than another sweep down here.

#### Two smaller results

**Erasing an already-erased block costs the same as a dirty one** — 1 091.1 ms
against a 1 111.3 ms median. There is no shortcut to skip, so a layer that
tracks which blocks are already clean saves the whole erase or nothing.

**Block-to-block erase time spans 1 050.7 – 1 151.7 ms across 32 blocks**, a
1.10× spread. Worth knowing before reading a 4 % difference in an erase-bound
phase as a change in behaviour.

### 5e. Are the numbers stable? Three DK runs, and the suite twice

Reproducibility, checked rather than assumed. The three board captures on the
branch are independent runs of the same board at different sweep densities —
and, between the first and the last, a **changed write procedure** (per-point
erase became a cursor through a pre-erased region). A coefficient that survives
both is measuring the part rather than the harness.

| coefficient | `direct` 15 pts | `matrix` 32/28 | `full` 112/96 | spread |
|---|---:|---:|---:|---:|
| read per byte | 258.27 ns/B | 256.02 | 253.78 | **1.018×** |
| write per byte | 12 156.5 ns/B | 12 182.9 | 12 222.4 | **1.005×** |
| erase per block | 1 061.43 ms | 1 087.56 | 1 086.31 | **1.025×** |
| write fixed | 157.13 µs | 158.16 | 159.44 | **1.015×** |
| read fixed | 65.68 µs | 68.12 | 69.23 | 1.054× |

Four of five inside 2.5 % across a 7× density change and a procedure change.

**The read intercept's 5.4 % is not drift in the part, it is the fit doing what
it should.** It moves monotonically with point count, because the ladder fills
in below the read crossover (254 B) and those added points measure almost
nothing *but* the intercept — so the weighted fit lets them speak. The right
question is whether it changes an answer, and the models are compared where
that is decided, at the predictions:

| read of | `direct` | `matrix` | `full` | spread |
|---:|---:|---:|---:|---:|
| 4 B | 66.72 µs | 69.14 | 70.25 | 1.053× |
| 256 B | 131.80 µs | 133.65 | 134.20 | 1.018× |
| 1 KB | 330.15 µs | 330.23 | 329.10 | **1.003×** |
| 64 KB | 16 991.7 µs | 16 843.3 | 16 700.9 | 1.017× |

| write of | `direct` | `matrix` | `full` | spread |
|---:|---:|---:|---:|---:|
| 4 B | 205.76 µs | 206.90 | 208.33 | 1.013× |
| 256 B | 3 269.2 µs | 3 277.0 | 3 288.4 | 1.006× |
| 64 KB | 796 845 µs | 798 579 | 801 165 | **1.005×** |

The three models disagree by **at most 5 % anywhere, and under 2 % over most of
the range** — including on `blob_db`'s own mean operation sizes (14.3 B read:
1.050×; 48 B write: 1.007×). For a cost model whose stated purpose is turning
operation counts into seconds, that is well inside what the prediction is used
for; §5's phase ratios move in the third digit whichever of the three is used.

Where the disagreement concentrates is exactly where §5b said the fixed term
dominates and §5c said the fit is worst — small reads. Three independent
statements about the same region, which is the sort of agreement that makes a
model believable.

**Host side**, for completeness: `native_sim` produces **byte-identical**
captures across three consecutive runs (253 `l0raw` rows each, `diff`-clean),
so the harness contributes no variance of its own; the flash simulator has none
to contribute. `west twister -T key_value_db -p native_sim` run twice gives
18/18 configurations and 209/209 test cases both times, with per-suite statuses
identical. And `tools/selftest.py` passes both of its synthetic devices.

### 5f. Stability: the same image, run twice

`ddaf35f` compares three captures taken at different sweep densities, where the
harness changed underneath. This is the complementary check — the **bit-identical
image**, same board, run again — so any difference is the part and the run, not
the code.

| class | points | median \|Δ\| | p90 | max |
|---|--:|--:|--:|--:|
| read | 112 | **0.04 %** | 0.63 % | 3.48 % (12 B) |
| write | 96 | **0.03 %** | 0.05 % | 1.04 % (32 B) |
| erase | 6 spans | — | — | 0.65 % |

Coefficients reproduce to within a fifth of a percent: read 253.8 → 253.8 ns/B
with the intercept 69.232 → 69.081 µs, write 12 222.4 → 12 219.2 ns/B with
159.438 → 159.165 µs. The gated read range is 252.76–253.50 against
252.71–253.41. `erase1_dist` returns the **same median to the nanosecond**
(1 111 267 089 ns both runs) with min/max moving ~1 %.

The worst disagreements are at 12 B and 32 B — the small-transfer region §5b,
§5c and `ddaf35f` all independently identify as the noisy one. Nothing new is
noisy.

#### The offset-2 anomaly is real

`FINDINGS.md` flags offset 2 costing ~31 µs more than offsets 1 and 3, where
`read_non_aligned()` decomposes all three identically, and marks it as within one
bad batch because `read_offset` is single-pass. A second run settles that without
needing the multi-pass change:

| offset | run 1 | run 2 | agreement |
|--:|--:|--:|--:|
| 0 | 132 261 ns | 132 205 ns | 0.04 % |
| 1 | 187 409 | 187 311 | 0.05 % |
| **2** | **218 504** | **218 392** | 0.05 % |
| 3 | 187 409 | 187 344 | 0.03 % |

The excess of offset 2 over offsets 1 and 3 is **+31 095 ns** and **+31 081 ns**
— two independent draws agreeing to 14 ns. It is not a bad batch, and the open
question is no longer whether it is real but why: the decomposition into prefix,
252 B aligned body and suffix is the same at all three offsets, so the extra cost
is somewhere the transaction count does not distinguish. The `memmove` shift
distance is the obvious suspect and the obvious next probe.

So the item needs a theory rather than more runs. Making `read_offset` honour
`PASSES` is still worth doing — it would have answered this from one capture
instead of two — but it is no longer what blocks the finding.

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

