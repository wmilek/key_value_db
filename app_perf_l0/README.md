# `app_perf_l0` — what `flash_area` costs, and what that predicts

Every other benchmark in this repository measures a stack. `app_perf` measures
`blob_db`, `app_perf_mc` measures the model container, `app_cbor_persondb`
measures a product-shaped workload. All of them bottom out in three L0 calls —
`flash_area_read()`, `flash_area_write()`, `flash_area_erase()` — and none of
them can say what those calls cost. So every question of the form *"is this
change faster?"* has had exactly one answer available: flash a board and find
out.

This app measures the bottom directly, and the model fitted from it answers
that question from a `native_sim` run instead.

It is a **standalone application** and does not link the storage stack:
`CONFIG_BLOB_DB` is off, nothing above L0 is in the image, and nothing above L0
can perturb a timing. What it opens is `storage_partition`, and what it sweeps
is the one parameter each L0 call has.

| phase | swept parameter | question |
|---|---|---|
| `read` | transfer size, **1 B → one erase block**, 8 samples per doubling | what does a small transfer cost against a large one, and how much of it is per-call overhead a caller pays again on every extra transaction? |
| `read_odd` | **consecutive integer sizes** either side of 16, 64, 256, 512, 1024, 4096, 16384 B | does the driver split a transfer whose length is not a whole number of controller words? At one-byte steps the marginal column stops being a slope estimate and becomes a direct readout: the cost of the 257th byte |
| `read_offset` | fixed size, start offset shifted by 1, 2, 3 B | the same question asked of the address rather than the length |
| `write` | transfer size, `write_align` → one erase block, transfers packed back to back as `blob_db` writes them | the same question on the program path — and whether the cost is a line at all |
| `write_pg` | transfer size, each transfer **pinned to a program-page boundary** | the page-program staircase, isolated: is one byte past a page boundary a whole extra program? |
| `write_unaligned` | fixed size, offset by one alignment unit | the page-straddle penalty: one transfer, two program pages |
| `erase` | **erase size** — blocks covered by one call | is a 16-block erase cheaper than sixteen 1-block erases? `blob_db_prepare()` does the latter, `blob_db_erase_all()` the former |
| `erase1` | one block, repeated over every block in the region | the per-block anchor, and the **distribution** — each sample is emitted individually, because a model carries one number for erase and this is the spread that number stands in for |
| `erase_erased` | one block that is already erased | does erasing blank flash cost anything less? `blob_db_prepare()` re-erases often enough that it is worth measuring rather than assuming |

Every read and write point is measured **three times** (`CONFIG_APP_PERF_L0_PASSES`).
The reported cost is the mean; the extremes appear beside it as a `spread`
column and as `min_ns`/`max_ns` on the machine-readable row. A single pass
gives a number with nothing to say about its own reliability, and the fit
weights every point equally — so a point whose passes disagree needs to be
visible as such.

### The matrix, and the column that answers the question

Each size sweep prints a table, and every row is also emitted as a machine-readable `l0raw` record:

```
     size      ops         us/op        KiB/s       ns/B   marginal ns/B
        1     4096         2.000          488    2000.00         -
        2     4096         2.000          976    1000.00        0.00
        3     4096         2.000         1464     666.66        0.00
      ...
     4096     4096         2.000      2000000       0.48        0.00
```

`us/op` and `KiB/s` say how fast it is. The column that says whether one
slope can describe the cost is the last one — the marginal cost, the extra
nanoseconds each extra byte cost between this row and the one above:

```
d(ns)/d(B) = (t(n) - t(n_prev)) / (n - n_prev)
```

For an affine cost `t(n) = a + b·n` this is `b` at *every* row, regardless of
the fixed cost `a`. So a column of near-identical numbers **is** the proof that
the cost is affine, and a column that steps is a cost a single slope cannot
describe. The app prints the range and its verdict at the end of each sweep;
the tool repeats both next to the fitted slope, so nobody reads a slope without
seeing what it averaged over.

**Affine is not the same as proportional, and the tables are careful to keep
them apart.** A constant marginal cost does *not* mean constant throughput —
`a` is charged in full on every call, however few bytes it carries, so `KiB/s`
climbs with transfer size and only flattens well past `a/b`. On the DK both are
true at once: write's marginal cost is flat to 1.02 % across 8 B…64 KB while
its throughput still varies 4× (19 → 80 KiB/s), and read's varies **64×**
(59 → 3 766 KiB/s) because its fixed term dominates until 254 B. Read the two
columns together: the marginal column tells you the *shape*, the KiB/s column
tells you what a caller at that transfer size actually gets. `RESULTS.md` §5b
works through both, including why `blob_db`'s 14 B mean read lands at 5 % of
the flash's achievable bandwidth.

Two details exist because of writes specifically:

- **The sizes are not only powers of two.** The ladder subdivides each octave
  into `CONFIG_APP_PERF_L0_STEPS_PER_OCTAVE` samples (8 by default: 1, 2, 3, 4,
  5, 6, 7, 8, 9, 10, 12, 14, 16, 20, 24, 28 …). Powers of two are exactly the
  wrong sample points for a cost that steps at a power-of-two boundary: every
  sample lands on a step, the staircase looks like a line through its corners,
  and the sweep concludes "affine" about a function that is not. Resolution is
  the point — a step between two samples is only located to within the gap, so
  bare powers of two place a feature within a factor of two, and eight steps
  place it within 9 %.

  This is affordable because the write sweep runs a **cursor** through a
  pre-erased region rather than erasing per point. Erased space is the
  expensive thing (a 1.1 s block erase against a 200 ms measurement), so
  erasing per point made cost scale with the number of samples; with a cursor
  it scales with bytes consumed, and extra sample sizes are nearly free.
- **`write_pg` pins each transfer to a page boundary.** The packed `write`
  sweep answers *what `blob_db` pays*, because `blob_db` writes back to back —
  but a packed transfer touches `ceil(n/page)` or one more depending on where
  the previous one ended, and averaging those two smooths the steps. Pinning to
  the boundary makes the cost exactly `ceil(n/page)` programs, so the table is a
  staircase if the part programs by page and a line if it does not. The tool
  prints the implied program count against `ceil(n/page)` and says which.

> **This benchmark destroys `storage_partition`.** It erases and rewrites the
> first `CONFIG_APP_PERF_L0_REGION_BLOCKS` blocks of it, repeatedly. Do not run
> it on a device whose store you want to keep.

## Why the numbers look the way they do

[`FINDINGS.md`](FINDINGS.md) is the companion to `RESULTS.md`: where that one
measures, this one explains, from the `nordic,qspi-nor` driver source and the
part's datasheet. It answers three questions the sweep raised but cannot settle
on its own — why an unaligned read costs 42–65 % more (the driver splits it
into three QSPI transactions plus a `memmove`), which power mode the part is
running in (Ultra Low Power, and nothing on this board can change it), and what
the part offers that the stack does not use (erase suspend/resume, and 4 KB
sector erase the driver already supports).

## The model, and why it is a line

Each class is fitted to an affine cost:

```
read  t(n) = R0 + R1·n        n = bytes in the transfer
write t(n) = W0 + W1·n
erase t(m) = E0 + E1·m        m = erase blocks the call covers
```

Affine is not a simplification of convenience. It is the property that makes
the prediction possible at all: **summing an affine cost over a set of
operations depends only on how many there were and how many bytes they moved**
— never on how those bytes were split between calls. And those two numbers per
class are exactly what `CONFIG_BLOB_DB_IOSTATS` already counts. So

```
T = R0·reads  + R1·bytes_read
  + W0·writes + W1·bytes_written
  + E0·erases + E1·(bytes_erased / block)
```

is *exact for the model*, from totals alone, with no per-operation trace and no
instrumentation beyond the counters the stack already keeps.

That is the whole trick. A `native_sim` run measures no time — the flash
simulator has no latency to model — but it counts every operation exactly. Feed
those counts through the model and a run that took no time on a host becomes a
number of seconds on a part it never touched.

### When it is not a line

Affine is an assumption, so the sweep is built to break it rather than to
flatter it. A NOR page program is a step function of transfer size, and if the
part behaves that way the marginal column goes flat across each tread and jumps
between them, `write_pg` shows the staircase directly, and the fit reports a
large residual instead of a tidy slope. `fit -v` prints the residual at every
swept size.

What to do when that happens depends on which sweep steps:

- **The packed `write` sweep is affine and only `write_pg` is a staircase.**
  This is the good case: `blob_db` writes back to back, so its transfers
  straddle pages at every offset and the staircase averages out. The affine
  model then describes what `blob_db` actually pays, and `write_pg` explains
  where the slope comes from — `per-byte ≈ page-program / page`.
- **The packed sweep steps too.** Then a single slope misprices small writes,
  and the prediction is only as good as the size mix it was fitted over. Refit
  with `--size-range` around the sizes the workload really writes, and say so
  with the numbers.

`write_unaligned` is reported and deliberately **excluded** from the fit —
folding it in would make every prediction slightly pessimistic and the
residuals slightly prettier.

## The workflow

### 1. Measure the board

```shell
west build -b nrf5340dk/nrf5340/cpuapp key_value_db/app_perf_l0
west flash
# capture the console; see RUN_ON_DK.md
```

Roughly **thirteen minutes** on the nRF5340-DK — 112 read sizes and 96 write
sizes, three passes each, plus the probes — and over half of it is the erase
sweep. The knobs that trade run time for resolution are in the table at the
bottom. The capture carries
machine-readable `l0geom` / `l0raw` / `l0end` records alongside the
human-readable tables.

### 2. Fit the model

```shell
python3 app_perf_l0/tools/l0_timing.py fit dk-capture.log \
    -o app_perf_l0/models/mx25r64_nrf5340dk.json -v
```

### 3. Run the workload on `native_sim` — with the target's geometry

This step is the one that is easy to get wrong. `blob_db` derives its bucket
count, its slot alignment and its segment size from the geometry it finds at
mount, so a `native_sim` run on the simulator's native 4 KB blocks issues a
*different number of operations* than the same code on a 64 KB part.
Multiplying those counts by the target's per-operation cost gives a confident
answer to the wrong question.

`geometry/` fixes that. It holds the devicetree overlay and the Kconfig
fragment that give `native_sim` the target's **shape** (not its timing — nothing
in the flash simulator models that):

```shell
west build -b native_sim key_value_db/app_perf \
  -DCONFIG_BLOB_DB_BACKEND_FLASH_AREA=y \
  -DCONFIG_BLOB_DB_LOG_LEVEL_INF=y \
  -DEXTRA_DTC_OVERLAY_FILE=$PWD/key_value_db/app_perf_l0/geometry/mx25r64.overlay \
  -DEXTRA_CONF_FILE=$PWD/key_value_db/app_perf_l0/geometry/mx25r64.conf
./build/zephyr/zephyr.exe > run.log
```

`CONFIG_BLOB_DB_LOG_LEVEL_INF=y` is what makes `blob_db` print its geometry
line, which is how the tool checks that the run's geometry is the model's
rather than trusting you. `--strict-geometry` turns that check into a failure.

This correspondence is not assumed — it is verified. See
[`RESULTS.md`](RESULTS.md) §2: with the overlay in place, a `native_sim`
`app_perf` run reproduces the DK's I/O counters **byte for byte**, across all
twelve phases.

### 4. Predict

```shell
python3 app_perf_l0/tools/l0_timing.py predict -m models/mx25r64_nrf5340dk.json run.log
```

```
phase             reads/bytes        writes/bytes      erases/blocks       read      write      erase        TOTAL
read                605/8650            0/0              0/0             46.4m       0.0m       0.0m     46.433 ms
lg write            168/2576          269/269584       133/133           13.0m    8052.1m  147140.0m    155.205 s
```

The three class columns are the point: `lg write` is 95 % erase, so no amount
of write-path tuning will move it, and the phase is telling you to erase less.
That decomposition is not available from a stopwatch on the board.

## The commands

| command | does |
|---|---|
| `fit CAPTURE -o M.json` | app_perf_l0 capture → model. `--weight`, `--size-range`, `-v` for residuals |
| `predict -m M.json RUN` | a run's I/O counters → predicted milliseconds, split by class |
| `verify -m M.json RUN` | for a capture with *both* counters and wall-clock: predicted vs measured, per phase |
| `derive CAPTURES --block-bytes N` | a **provisional** model solved from stack-level captures, for a board with no L0 run yet |
| `spec -m M.json --spec S.json` | measured against the part's datasheet: in spec, and in **which mode**? |
| `simconf -m M.json [--run R]` | `CONFIG_FLASH_SIMULATOR_*` knobs that make `native_sim` tick at roughly the target's rate |
| `show M.json` | print a saved model and its residuals |
| `selftest.py` | fit two synthetic devices with known costs — one affine, one page-quantised — and check the tool recovers each and reports the right shape |

`predict` also takes counters directly (`--reads`, `--bytes-read`, …), so
anything that can call `blob_db_iostats_get()` can be predicted without this
tool having to learn its output format.

### `spec` — does reality match the datasheet?

A measured cost is more useful next to what the part is specified to do, so
`models/` also carries a **part spec** — the erase and program envelope
transcribed from the datasheet, with its citation
(`mx25r6435f_datasheet.json`, Macronix Rev. 1.6 §15). `spec` puts the two side
by side and reports where each measurement falls: under typ, between typ and
max, or over max.

The verdict that matters is usually not pass/fail but **which mode**. On the
MX25R6435F the Ultra Low Power and High Performance columns differ by 2–4×,
the mode is a *volatile* Configuration-Register-2 bit whose power-on value
comes from the part's ordering code, and Zephyr's `nordic,qspi-nor` driver
never writes it — so nothing in the schematic or the devicetree says which
mode a board is in. The timing does. See `RESULTS.md` §5, where the DK's
numbers come out over the High Performance maximum and inside Ultra Low
Power.

Only the model's **per-unit** terms are compared. The fixed terms are bus time
and driver overhead; the datasheet's figures are the part's internal erase and
program times and do not include them, so comparing those would be a category
error rather than a finding.

### `derive`, and what a provisional model is worth

An `app_perf` capture already contains, per phase, the I/O counters *and* the
wall-clock they produced. That is a linear system in the same six coefficients
the sweep measures directly, so it can be solved for them — from captures taken
long ago, on a board that is no longer on the desk. `derive` does that with
non-negative least squares on relative error.

Non-negativity is load-bearing, not decoration: unconstrained, the system
happily returns a *negative* cost per read that cancels against an inflated
erase cost — a perfect fit to those observations and nonsense on any other
workload.

What `derive` cannot do is separate flash time from the CPU time above it. Any
per-operation cost in `blob_db` — the slot scan, the CRC — lands in the fixed
terms and inflates them. So a derived model is a stand-in that says so in every
line of output, and an `app_perf_l0` run on the board supersedes it.
[`models/mx25r64_nrf5340dk_derived.json`](models) is one, together with the
captures it came from and a leave-one-out cross-validation of it — and
[`models/mx25r64_nrf5340dk_direct.json`](models) is the board run that
superseded it. Comparing the two is instructive: the derived model's per-byte
terms are inflated ~2.4× because the stack phases it was solved from have reads
and writes co-occurring, so the two are collinear there and `blob_db`'s CPU was
split across both in proportion. Only a sweep that varies one call at a time
separates them. `RESULTS.md` §5 has the scorecard.

## What the model does not include

- **CPU time between flash calls.** The prediction is the flash cost only, and
  on this stack that turned out to be a large omission for transfer-bound work:
  measured on the DK, erase-bound phases predict at 0.98–1.00× while
  transfer-bound ones predict at 0.57×, so **43 % of a transfer-bound phase is
  CPU above L0** — slot walking, header parsing, CRC, memcpy. `verify` is how
  you find out which case a workload is in, and that split is the number this
  app exists to produce.
- **Anything under a wear-levelling layer.** UBI issues flash operations of its
  own *beneath* the seam where `CONFIG_BLOB_DB_IOSTATS` counts, so a UBI run's
  counters do not describe all the traffic its wall-clock paid for. Predictions
  for UBI builds are lower bounds. The L0 sweep itself is unaffected — it
  measures `flash_area`, which is what UBI sits on.
- **Wear.** Erase time on NOR drifts upward over a part's life. The `erase1`
  spread is the only hint of it here; a model fitted on a fresh part will
  under-predict an old one.
- **Anything that is not a single erase-block-uniform partition.** The app
  refuses a non-uniform sector layout rather than averaging over it.
- **Concurrency.** Every phase is single-threaded with nothing else running.

## Configuration

| Option | Meaning |
|---|---|
| `CONFIG_APP_PERF_L0_MAX_XFER` | largest transfer swept, clamped to one erase block; also the app's only RAM cost |
| `CONFIG_APP_PERF_L0_REGION_BLOCKS` | how many erase blocks the app may destroy; also bounds the erase-span sweep |
| `CONFIG_APP_PERF_L0_TARGET_MS` | how long one sweep point should last; precision against run time |
| `CONFIG_APP_PERF_L0_MAX_REPS` | repetition ceiling, so a cheap operation cannot ask for millions |
| `CONFIG_APP_PERF_L0_ERASE_REPS` | repeats per erase span — the dominant term in total run time |
| `CONFIG_APP_PERF_L0_STEPS_PER_OCTAVE` | size samples per doubling (8): the resolution knob, and what places a cost step to within 9 % instead of a factor of 2 |
| `CONFIG_APP_PERF_L0_PASSES` | timed passes per point (3): turns each row from a number into a number with a spread |
| `CONFIG_APP_PERF_L0_PROGRAM_PAGE` | the part's program page (256 B on most SPI NOR); chooses the sizes for the page-program sweep. `flash_area` does not report it, so it has to be told; `0` skips that phase |

On `native_sim` the app builds and runs in seconds, and
`boards/native_sim.conf` turns on the simulator's own timing model so the sweep
has something to measure. What a `native_sim` fit recovers is
`fixed = CONFIG_FLASH_SIMULATOR_MIN_*_TIME_US, per-byte = 0` — the right answer
about the simulator and a useless one about any part, which is why the model
file records `source=flash_simulator` and `predict` refuses it without
`--allow-simulated`. It makes a good end-to-end self-test of the fitter: see
[`RESULTS.md`](RESULTS.md) §1.
