# `app_cbor_persondb` — reference results

Keep this file honest: update it when the shape of the app changes or when
regenerating on new hardware.

**Status: `native_sim` measured at 10 000 persons; nRF5340-DK re-measured on
post-merge code at 1 000 persons (§5).** The DK timings are no longer
projections, and they no longer predate the large-payload merge — §5 was
re-taken at `338ec24` (which contains all of `e80f404`). They were taken at a
tenth of the target scale, so §5's per-operation numbers are directly
comparable to the rest of this file, while its whole-run numbers (fill
duration, occupancy) are *not* the 10 000-person figures and are marked where
they appear. A full-scale DK run remains outstanding for `A4`; §5 gives a
revised floor of ≈2.2 h for it.

---

## 0. What this benchmark measures

**The result is time per operation, and operations per second.** Everything
else here is context for those two numbers.

The dataset is **ballast**. Its only job is to put the store into a state where
the per-operation numbers mean something: deep enough buckets, a realistic hash
spread, and enough write history that compaction is part of the picture. Its
size, its byte count and the fill percentage are *properties of the ballast* —
reported so a reader knows what state the measurement was taken in, never
results in themselves (`DESIGN.md` §6.3).

§3b shows the ballast earning its place: the same operation costs **38 % more**
at half-full than on a near-empty store, so measuring on a small store would
report a number no product would ever see.

| Headline, `native_sim`, 10 000 persons | |
|---|--:|
| `check` — resolve a credential and decide (**R-D**) | **44 µs/op · 21 510 ops/s** |
| `byid` — fetch a person record | **23 µs/op · 39 666 ops/s** |
| Same on the nRF5340-DK, pre-merge (§5) | **114.2 ms/op · 8.8 ops/s** |

## 1. What `native_sim` can and cannot tell you

| | |
|---|---|
| **Meaningful** | operation counts, blob-op counts, amplification, byte accounting, correctness, crash-safety behaviour |
| **Not meaningful** | wall-clock times — the flash simulator is RAM, so nothing here reflects QSPI NOR |

Two things worth knowing before reading any duration below:

- The simulator's own clock does not advance during computation, so
  `k_uptime_get()` reports zero for every phase. The app uses host real time on
  POSIX instead (`scenario_now_us()`); that measures the work, but against a
  RAM-backed store.
- The overlay gives `native_sim` the DK's **exact** geometry — 8 MiB partition,
  64 KB erase blocks — so the *structural* results (bucket occupancy, shard
  count, fill percentage, amplification) do transfer. Only time does not.

## 2. Setup

- **Platform**: `native_sim`, host `gcc`, Zephyr `da0718ca0d52`
- **Storage**: 8 MiB `storage_partition` in 64 KB sectors
  (`boards/native_sim.overlay`), mirroring the DK's MX25R6435F
- **Config**: 10 000 persons, 16 person maps + 1 credential map, 511 buckets
  each, `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN=4096`, zcbor canonical

## 3. The ballast — what the store holds during a measurement

| | |
|---|---|
| Persons | 10 000 |
| Credentials | 24 932 (1–4 per person, mean 2.49) |
| Mean bytes per person, all-in | 433 B |
| **Live content** | **4 336 158 B** |
| **Fraction of the 8 MiB partition** | **51.6 %** |
| Bucket overflows during the fill | **0** |
| Fullest `kvhash` bucket (from `tools/sizing.py`) | 2 711 B = 66 % of the 4 KB ceiling |

**Read the last row as an output, not a score.** 51.6 % is where a *fixed*
10 000-person dataset happens to land against this stack at this commit. The
50 % figure in R-E was the heuristic that chose 10 000 once — big enough to be
representative, small enough to leave the board usable and to avoid a reformat
before every run (`DESIGN.md` §6.3). Nothing in the app is scaled from it.

So if this number **falls**, the stack got better at storing the same data, and
that is the result — not something to correct by enlarging the dataset. §3a
tracks it.

*Physical* occupancy — live bytes plus not-yet-compacted garbage — is **not
reported**, because no layer exposes it (`FINDINGS.md` B3). Everything above is
logical content, computed by the application from its own generator.

## 3a. Fill history — a *ballast* indicator, not a performance one

Not a performance metric — it describes the ballast. Same 10 000 persons and
same record shape every time, so movement here is the stack's *storage density*
changing, which is a different axis from the throughput in §0. **Down is
better,** and a drop does not by itself imply a faster store.

| Commit | Live content | Fill | What changed |
|---|--:|--:|---|
| first working fill | 3 847 589 B | 45.8 % | permission vocabulary was ~11-char names |
| `2c58710` | 4 336 158 B | **51.6 %** | qualified permission names (~14 chars) — a *dataset* change, so not a stack result |
| post-merge (`f5b062e`) | 4 336 158 B | **51.6 %** | main's slot-header walk: 19× less flash *moved*, but the same bytes *stored* |

The third row is the interesting one. The large-payload merge cut the flash
traffic of a decision by 19× and changed occupancy by **nothing** — because it
changed how the store is read, not how densely it is packed. Two different
things, and this table separates them. A change to `kvhash`'s per-entry framing
or to `blob_db`'s slot overhead would move this column; a faster read path never
will.

## 3b. Does the ballast earn its place?

Yes, and this is the measurement that justifies carrying 4 MiB around. Same
build, same 16 maps, 200 samples, only the dataset size varied — so the store
is the same product at five points in its life:

| persons | fill | `check` µs/op | `check` ops/s | flash ops/op | `byid` µs/op | `byid` ops/s |
|--:|--:|--:|--:|--:|--:|--:|
| 500 | 2.5 % | 32 | 30 907 | 70 | 20 | 50 543 |
| 1 000 | 5.1 % | 33 | 28 930 | 112 | 19 | 48 461 |
| 2 500 | 12.9 % | 40 | 24 603 | 264 | 23 | 46 061 |
| 5 000 | 25.8 % | 42 | 22 891 | 351 | 23 | 41 963 |
| **10 000** | **51.6 %** | **44** | **21 510** | 261 | **23** | **39 666** |

An access decision costs **38 % more** at half-full than on a near-empty store,
and throughput falls **30 %**. Benchmark a nearly-empty store and you publish a
number the product never sees.

Note the flash-operation column is **not monotonic** — it peaks at 5 000 and
falls again at 10 000. Per-operation cost tracks how much superseded data sits
in the sectors being walked, and that is a function of *write history and when
compaction last ran*, not of live fill. "Half full" is a convenient label for
the ballast, not the variable that actually drives the cost.

### Repeatability — which numbers to trust

Same store, same fill, three consecutive runs:

| | run 1 | run 2 | run 3 | spread |
|---|--:|--:|--:|--:|
| `check` µs/op | 44 | 43 | 45 | **±2 %** |
| `put` µs/op | 1 112 | 786 | 1 125 | **±20 %** |

The read path is repeatable and can be compared run to run. The write path is
dominated by whether a compaction lands inside the sample window, so a single
200-sample `put` figure is an estimate, not a measurement — treat a change
under ~30 % as noise, or raise `BENCH_SAMPLES` until it settles.

## 4. Measured — `native_sim`

Steady-state run (store already filled), 200 samples per benchmark. Flash
figures come from `blob_db_iostats_get()` — **measured at the storage seam, not
modelled** (`CONFIG_BLOB_DB_IOSTATS=y`):

| Phase | µs/op | map ops/op | flash ops/op | flash bytes/op | payload | **amplification** |
|---|--:|--:|--:|--:|--:|--:|
| `check` — card → person → permission (**R-D**) | **42** | 2 | 261 | 13.3 KB | 402 B | **33×** |
| `byid` — person id → record | 23 | 1 | 128 | 6.5 KB | 388 B | 17× |
| `miss` — unknown card | 21 | 1 | 132 | 6.8 KB | 23 B | 294× |
| `put` — rewrite a record + its index entries | 1 020 | 3.3 | 675 | 87 KB | 431 B | 202× |
| `cbor` — encode + decode, no flash | **6** | 0 | 0 | 0 | 388 B | — |

### Before and after the large-payload merge

The same benchmark, same host, across `main`'s slot-header walk
(`7f10295`). The "before" byte column was modelled as
`map operations × sector size` — accurate for code that read whole sectors;
the "after" column is measured:

| bench | before | after | change |
|---|--:|--:|--:|
| `check` µs/op | 580 | **42** | **14× faster** |
| `check` flash/op | 256 KB (656×) | **13.3 KB (33×)** | **19× less** |
| `check` flash *operations*/op | 4 | **261** | **65× more** — see `FINDINGS.md` N1 |
| `put` µs/op | 2 406 | 1 020 | 2.4× faster |
| RAM on the DK | 157 584 B | 157 688 B | unchanged — B6 still open |

Fewer bytes, far more transactions. On `native_sim` a transaction is a
`memcpy` and the trade is pure win; on a serial part each carries a fixed
command-and-address cost, so **whether many small reads beat 4 large ones is a
hardware question this platform cannot answer.**

**The board has since answered it (§5): they do, by 7.8×** — but a transaction
costs ~65 µs, which is 54% of the remaining decision cost, so the trade is
narrower than the byte column suggests. Note also that the DK measures **112**
transactions per `check` rather than the 261 here: this `native_sim` column was
taken at `f5b062e`, before the one-entry index cache landed, and the counters
are otherwise platform-independent because the overlay mirrors the DK's
geometry.

Whole-run phases:

| Phase | Time | Work |
|---|--:|---|
| open | 0 ms | 1 registry read + 1 superblock read |
| prepare | 2 ms | 106 buckets pre-formatted |
| fill (first run) | 17.0 s | 10 000 persons + 24 932 credentials = 34 932 map writes, 40 progress commits |
| verify | 271 ms | 256 persons + 664 credential resolutions |
| mutate | 249 ms | 64 revoked + 64 assigned, one revision |
| re-verify | 277 ms | same |


## 4a. Footprint — nRF5340-DK (measured)

Cross-built with Zephyr SDK 1.0.1 (`arm-zephyr-eabi`) for
`nrf5340dk/nrf5340/cpuapp`. These are real target numbers, as are the timings
in §5.

| | FLASH | of 1 MB | RAM | of 448 KB |
|---|--:|--:|--:|--:|
| benchmark frontend | 59 072 B | 5.6 % | **157 584 B** | **34.4 %** |
| shell frontend | 93 476 B | 8.9 % | 162 328 B | 35.4 % |

The Zephyr shell subsystem is the entire difference: +34 KB FLASH, +4.7 KB RAM.

### ROM — where the 59 KB goes

| Component | Bytes | % |
|---|--:|--:|
| Zephyr kernel, drivers, libc | 27 128 | 46.0 % |
| linker//build artefacts not attributed to a path | 10 840 | 18.4 % |
| **`app_cbor_persondb` (all six sources)** | **7 830** | **13.3 %** |
| `hal_nordic` | 6 034 | 10.2 % |
| **storage stack** — `blob_db` 3 772 + `kvhash` 1 124 + `rootreg` 484 | **5 380** | **9.1 %** |
| **zcbor** | **1 754** | **3.0 %** |

The app's own split: `scenario.c` 2 266, `persondb.c` 2 098, `person_cbor.c`
1 680, `dataset.c` 998, `ui_bench.c` 784, `main.c` 4.

**The whole storage stack is 5.4 KB of ROM and CBOR adds 1.75 KB.** Nothing
about this application is ROM-constrained.

### RAM — where the 158 KB goes

| Component | Bytes | % of image RAM |
|---|--:|--:|
| **`blob_db` `g_bbuf` + `g_bbuf_new`** | **131 072** | **83.2 %** |
| main stack (`CONFIG_MAIN_STACK_SIZE`) | 12 288 | 7.8 % |
| **`kvhash` `dir_buf` + `bkt_buf`** | **8 192** | **5.2 %** |
| interrupt + idle + logging stacks | 3 136 | 2.0 % |
| kernel objects, drivers, libc | ~2 600 | 1.6 % |
| **the application itself (`g_db`)** | **248** | **0.16 %** |

**This is finding B6, measured.** The application owns 248 bytes of RAM. Two
`blob_db` sector buffers own 131 072 — five hundred times as much, and 83 % of
everything the image uses — for no reason other than that the DK's QSPI part
has 64 KB erase blocks. The streaming slot walk proposed on
`claude/blob-db-max-payload-increase-6qobv5` would take the image from ~158 KB
to roughly 28 KB.

### Stack: sized by measurement, not by habit

`prj.conf` originally carried `CONFIG_MAIN_STACK_SIZE=32768`, copied from
`app_perf_kvdb`. Compiling with `-fstack-usage` gives the real frames, and the
deepest chain is the write path:

```
scenario_bench 2464 + persondb_person_put 824 + kvhash_set 88
              + blob_db_update 64 + append_slot 4200   =  7640 B
```

`append_slot` alone is **55 %** of the requirement — blob_db builds every slot
in a `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN + 46` byte **stack** frame (B5, job 3).
12 KB gives ~1.5× margin over the measured worst case and returns 20 KB of RAM.
Staging that slot in the existing `.bss` scratch — Stage 1 of the large-payload
proposal — would take this application's stack requirement to about 3.5 KB.

Neither target can measure peak stack at run time: `native_sim`'s main thread
runs on a host pthread stack Zephyr never paints, so the report reads 24 B. The
app carries the instrumentation for hardware
(`-DCONFIG_INIT_STACKS=y -DCONFIG_THREAD_STACK_INFO=y`), where it is meaningful.

### Reproducing

The convenient route is the Zephyr CI image, which is what
`.github/workflows/build.yml` uses:

```
docker run --rm -v "$PWD/..:/ws" -w /ws ghcr.io/zephyrproject-rtos/ci:latest bash -c \
  'west init -l key_value_db && west update --narrow -o=--depth=1 && \
   west build -p always -b nrf5340dk/nrf5340/cpuapp key_value_db/app_cbor_persondb \
              -t ram_report'
```

Swap `ram_report` for `rom_report`, or drop `-t` for a plain build. The numbers
above were taken with a local SDK 1.0.1 install, which gives identical results.

## 5. Measured timings — nRF5340-DK

Board 960115021 (PCA10095), Zephyr build `4a405846193f`, SDK 1.0.1, console on
VCOM 2 at 115200. **`CONFIG_APP_CBOR_PERSONDB_N_PERSONS=1000`** with
`FRESH_START=y`, 200 samples per benchmark phase. Raw capture in §8a.

Re-taken at `338ec24`, which contains the whole large-payload merge. The
pre-merge board column is kept so the effect of the merge — and the error in
the projection that preceded it — stay visible. The pre-merge run's own
"projected" column came from `app_perf_kvdb/RESULTS.md` when a `blob_db` read
cost 17.45 ms; it is folded into the prose below rather than carried as a third
column.

### Per-operation

| Phase | Pre-merge board | §5a predicted | **Post-merge** | Δ | amp before → now |
|---|--:|--:|--:|--:|--:|
| `check` (**R-D**) | 114.2 ms | 7–11 ms | **14.605 ms** | ×7.8 | 701× → **24×** |
| `byid` | 57.6 ms | — | **7.770 ms** | ×7.4 | 355× → **13×** |
| `miss` | 56.6 ms | — | **7.165 ms** | ×7.9 | 26 214× → **210×** |
| `put` | 313.8 ms | — | **83.920 ms** | ×3.7 | 1 770× → **47×** |
| `cbor` | 955 µs | 955 µs | **950 µs** | — | — |

`cbor` is unchanged to within 0.5%, exactly as it must be: it touches no flash.
That it did not move is the control that makes the other four rows credible.

**§5a's prediction was too optimistic, and the reason is the constant it
guessed.** It estimated 7–11 ms from "261 transactions at a plausible 5–20 µs"
of fixed per-transaction cost. Measured: **14.605 ms**. The transaction count
was in fact 112 per `check` (22 415 flash ops over 200), not 261, so the
estimate was wrong in both terms and they partly cancelled — but the dominant
error is the per-transaction cost, which is far above 20 µs.

### This settles `FINDINGS.md` N1: a flash transaction costs ~65 µs

`app_perf/RESULTS.md` fits two constants from the DK's `lg read` phases:
**~65 µs per flash read transaction and ~0.63 µs/B.** Applying them to this
app's own counters, with no refitting:

| | per `check` |
|---|--:|
| flash ops | 112.1 |
| bytes | 10 030 B |
| predicted 112.1 × 65.5 µs + 10 030 × 0.63 µs | **13.66 ms** |
| measured, less the 0.95 ms of CBOR | **13.66 ms** |

The constants fitted on `app_perf` predict a different application's decision
cost to within a fraction of a percent. That is a single test point and two
free parameters, so it is a cross-check rather than a proof — but it is a
cross-check across two apps, two payload configurations and two access
patterns, which is the strongest evidence in this file that the cost model is
real. **Transactions are expensive: at 112 per decision they are 54% of
`check`, against the 12–36% the 5–20 µs guess implied.**

### Whole-run phases (**1 000 persons — not the 10 000-person figures**)

| Phase | Pre-merge | **Post-merge** | Δ | Work |
|---|--:|--:|--:|---|
| open | 24 478 ms | **25 837 ms** | — | **includes the `FRESH_START` 8 MiB erase** — not the steady-state open cost |
| prepare | 113 685 ms | **116 564 ms** | — | 106 buckets formatted = **1 100 ms per bucket** |
| fill | 983 833 ms (16.4 min) | **807 681 ms (13.5 min)** | ×1.22 | 1 000 persons + 2 479 credentials, 4 progress commits |
| verify | 48 023 ms | **6 064 ms** | ×7.9 | 256 persons, 602 credential resolutions |
| mutate | 14 712 ms | **2 850 ms** | ×5.2 | rev 0 → 1, 64 cards assigned (nothing to revoke on a first run) |
| re-verify | 49 758 ms | **6 291 ms** | ×7.9 | 256 persons, 618 credential resolutions |

Both verification passes reported `VERIFY PASS`, and the fill hit **0 bucket
overflows** — the `tools/sizing.py` geometry holds on hardware.

`open` and `prepare` did not move, and could not have: both are pure 64 KB
sector erase, a property of the MX25R6435F. The 1 100 ms per bucket here
matches `app_perf`'s independently measured 1.06–1.09 s per sector.

**Fill gained only ×1.22 while the read-bound phases gained ×7.9.** That is
the sharpest confirmation of §5's finding that fill is dominated by erase and
compaction rather than by the operations it performs: making reads eight times
faster barely moved it. The erase share of fill is therefore now *higher* than
the ~80% estimated pre-merge, and it is the only remaining lever on
fill time.

Extrapolating fill linearly to 10 000 persons gives **≈2.2 h** (down from
≈2.7 h), still a floor for the same reason as before: occupancy and compaction
both worsen with scale.

### A counter was renamed — do not diff it against the old table

The pre-merge edition printed a single `blob ops` figure (4.00 per `check`).
This edition prints **`map ops` plus `flash ops` and bytes** — 2.00 map ops and
112 flash ops per `check`. Those are different counters, not a halving of work;
the physically meaningful quantity is `flash ops`, which the old edition did
not report at all. Structural results are unchanged where they are comparable:
2 479 credentials, mean entry 432 B, 0 overflows, live content 432 759 B
(5.1% of the partition at a tenth scale, consistent with §3's 51.6% at full
scale).

### The projection's real defect: there is no erase term

`prepare` formats 106 buckets in 113.7 s — **1 072 ms each**, ~51× the 21 ms
write the model assumed. That is almost exactly one MX25R6435F 64 KB sector
erase per bucket, and the projection had no erase term at all. The same
omission drives the fill: 3 479 map writes in 983.8 s is **283 ms per map
write** against 55.9 ms projected.

Scaled to this run, the "operations only" line predicted 3.24 min; the fill
took 16.4 min. **About 80 % of fill time is erase and compaction**, which is
the component `DESIGN.md` §6.2 flagged as least certain — and it is the
dominant term, not a correction to one.

Extrapolating the fill linearly to 10 000 persons gives **≈ 2.7 h** against the
≈ 2.5 h estimate. Treat that as a floor: bucket occupancy and compaction both
worsen with scale, so the full-scale run should be expected to exceed it.

### What did transfer

Amplification and blob-op counts match `native_sim` closely — 701 / 355 /
26 214 / 1 770× measured against 711 / 360 / 26 214 / 1 799× simulated, and
9.96 blob ops per `put` against 9.98. Mean entry size is 432 B against 433 B.
This is the §1 claim holding up: structure transfers, wall-clock does not.

Store contents for this run: 1 000 persons, 2 479 credentials, 432 759 B live
= **5.1 %** of the partition. The 51.6 % occupancy in §3 is a 10 000-person
result and is not reproduced at this scale.

## 5a. What the board says about the merge — and about CBOR

The DK run gives two constants the `native_sim` numbers cannot: a `blob_db`
read costs **28.8 ms** for a 64 KB sector (0.45 ms/KB), and a 64 KB erase costs
**1 072 ms**.

**Predicted `check` after the merge.** The decision now moves 13.3 KB in 261
transactions instead of 256 KB in 4. At 0.45 ms/KB the data is ~6 ms; 261 SPI
transactions at a plausible 5–20 µs of fixed command-address-and-driver cost add
1.3–5 ms. So **≈ 7–11 ms, against 114.2 ms measured** — roughly a 10–15×
improvement, and the spread between those two estimates is exactly the
transaction-cost question in `FINDINGS.md` N1. **Measuring it is the one thing
that settles N1**, and only the board can.

> **Measured: 14.605 ms, and N1 is settled — a transaction costs ~65 µs.** The
> estimate above was wrong in both of its terms. The real transaction count is
> **112** per `check`, not 261, which should have made it *faster* than
> predicted; but the fixed cost is **~65 µs**, not 5–20 µs, which more than
> cancels that out. Fixed transaction cost is 54% of the decision, so the
> "plausible" range was low by 3–13×.
>
> The honest reading is that the *shape* of the estimate was right — data bytes
> and transaction count are the two terms that matter — and the arithmetic was
> defensible from what was known. What it lacked was the one constant only the
> board could give, which is precisely why N1 was raised. With that constant in
> hand, `app_perf`'s independently fitted model reproduces this app's `check`
> cost to a fraction of a percent (§5).

**The CBOR conclusion has to be restated.** This document previously said the
codec was "a projected 0.009 % of the decision on hardware". That was the
`native_sim` 6 µs carried across unchanged. Measured, it is **955 µs on a
128 MHz Cortex-M33 — 159× the host figure**:

| | encode+decode | `check` | codec share |
|---|--:|--:|--:|
| `native_sim`, pre-merge | 6 µs | 580 µs | 1.0 % |
| **DK, pre-merge** | **955 µs** | **114.2 ms** | **0.84 %** |
| DK, post-merge (predicted) | 955 µs | ≈ 7–11 ms | 9–14 % |
| **DK, post-merge (measured)** | **950 µs** | **14.605 ms** | **6.5 %** |

The headline conclusion survives — **the serialization format is not the
bottleneck**, and refusing to denormalize (D2) remains right. But the margin is
1–2 orders of magnitude thinner than claimed, and once B1 is fixed the codec
stops being free: at ~10 % of a decision it is no longer beneath notice. Had the
storage layer been fast from the start, the "CBOR costs nothing" claim would
have needed the board to make honestly.

Two lessons, both about method rather than about the stack:

- **Compute does not transfer between platforms.** The flash model projected
  within 1.65×; the compute figure was wrong by 159×. Only the part backed by a
  hardware constant travelled.
- **A ratio between two things measured on different platforms is not a
  ratio.** "1 % of the decision" was `native_sim` compute over `native_sim`
  flash, and neither term survived contact with the board.

## 6. What the numbers say

**The serialization format is not the cost.** `cbor` is 6 µs against `check`'s
599 µs on `native_sim` — **1.0 %**. On hardware it is 955 µs against 114.2 ms —
**0.84 %** (§5, measured). Note that this section previously projected 0.009 %
by carrying the host's 6 µs onto the target; the real figure is ~93× that, and
the conclusion survives only because 955 µs is still noise against four sector
reads. Choosing CBOR over anything else costs nothing measurable, and the
denormalized permission bitmask this app deliberately did not build
(`DESIGN.md` D2) would have optimized away half of a cost that is 99 %
elsewhere.

**The cost is that every blob operation reads a whole sector.** An access
decision moves **256 KB of flash to answer a question about 365 bytes** — 711×
amplification — and half of that is `kvhash` re-reading its bucket directory
before it can locate anything (`FINDINGS.md` B1, K11). The streaming slot walk
proposed on `claude/blob-db-max-payload-increase-6qobv5` addresses the first;
derived bucket ids would remove the second.

**Negative lookups are the worst case.** 128 KB read to learn that a card does
not exist: 26 214× amplification. A reader presented with an unknown badge pays
the same flash traffic as one presented with a valid one.

**Writes cost ten blob operations, not three.** A person plus its 2.49
credentials is 9.98 blob operations, because the credential index is maintained
per card. That is the price of the secondary index that makes R-D possible at
all, and it is paid on every enrollment.

## 7. Cross-reboot persistence

Three consecutive runs against the same flash file. Each verifies content the
*previous* boot wrote, then advances one mutation revision:

```
run 1  fill 10 000 -> VERIFY PASS (rev 0) -> mutate -> VERIFY PASS (rev 1)
run 2  VERIFY PASS (rev 1) -> mutate -> VERIFY PASS (rev 2)
run 3  VERIFY PASS (rev 2) -> mutate -> VERIFY PASS (rev 3)
```

256 persons and ~664 credential resolutions checked per pass, against a
generator that holds no state between boots.

## 8. Raw capture — steady-state run

```
*** Booting Zephyr OS build da0718ca0d52 ***

persondb 1.0.0 — CBOR person/credential database
open         :      0 ms
prepare      :      0 ms (0 buckets formatted)
fill         : already complete (10000 persons)
verify       :    271 ms  256 persons, 664 cards
VERIFY PASS
mutate       :    249 ms  rev 1 -> 2  (64 revoked, 64 assigned)
re-verify    :    277 ms  256 persons, 664 cards
VERIFY PASS

bench check :   200 ops in   119835 us ->    1668 ops/s      599 us/op     800 blob ops  amp 711x
             : 198 granted, 0 denied, 0 unknown, 2 expired
bench byid  :   200 ops in    59462 us ->    3363 ops/s      297 us/op     400 blob ops  amp 360x
bench miss  :   200 ops in    65575 us ->    3049 ops/s      327 us/op     400 blob ops  amp 26214x
bench put   :   200 ops in   497064 us ->     402 ops/s     2485 us/op    1995 blob ops  amp 1799x
bench cbor  :   200 ops in     1267 us ->  157853 ops/s        6 us/op       0 blob ops  amp 0x

store
  partition   : 8388608 B (8192 KiB), 65536 B sectors
  maps        : 16 person + 1 credential, 511 buckets each
  persons     : 10000 of 10000   credentials: 24932
  mean entry  : 433 B
  live content: 4336158 B = 51.6 % of the partition
  bucket overflows: 0

persondb: done — store at rev 2; rerun to prove persistence
```

The two `expired` results in `check` are persons whose generated badge validity
window starts after the benchmark's reference time — a real case the decision
path has to handle, left in rather than tuned away.

## 8a. Raw capture — nRF5340-DK, first run

Board 960115021, `N_PERSONS=1000`, `FRESH_START=y`. This is a *first* run, so
`open` carries the partition erase and `mutate` has nothing to revoke — neither
is a steady-state number.

Post-merge, at `338ec24`:

```
*** Booting Zephyr OS build 4a405846193f ***

persondb 1.0.0 — CBOR person/credential database
[00:00:24.986,358] <inf> persondb: created store: 16 people maps + 1 credential map, max buckets each, 1000 persons planned
open         :  25837 ms
prepare      : 116564 ms (106 buckets formatted)
fill         : 807681 ms  1000 written, 1000/1000 total, 4 commits
verify       :   6064 ms  256 persons, 602 cards
VERIFY PASS
mutate       :   2850 ms  rev 0 -> 1  (0 revoked, 64 assigned)
re-verify    :   6291 ms  256 persons, 618 cards
VERIFY PASS

bench check :   200 ops in  2921000 us ->      68 ops/s    14605 us/op   400 map ops  22415 flash ops   2006077 B  amp 24x
             : 200 granted, 0 denied, 0 unknown, 0 expired
bench byid  :   200 ops in  1554000 us ->     128 ops/s     7770 us/op   200 map ops  10753 flash ops   1026970 B  amp 13x
bench miss  :   200 ops in  1433000 us ->     139 ops/s     7165 us/op   200 map ops  11368 flash ops    969175 B  amp 210x
bench put   :   200 ops in 16784000 us ->      11 ops/s    83920 us/op   664 map ops  63679 flash ops   4174041 B  amp 47x
bench cbor  :   200 ops in   190000 us ->    1052 ops/s      950 us/op     0 map ops      0 flash ops         0 B  amp 0x

store
  partition   : 8388608 B (8192 KiB)
  maps        : 16 person + 1 credential (bucket count not observable — FINDINGS.md K10)
  persons     : 1000 of 1000   credentials: 2479
  mean entry  : 432 B
  live content: 432759 B = 5.1 % of the partition
  bucket overflows: 0
  NOTE: physical occupancy (live + uncompacted garbage) is not
        observable through the API — see FINDINGS.md B3.

persondb: done — store at rev 1; rerun to prove persistence
```

Pre-merge, kept for comparison:

```
[00:00:23.614,959] <inf> persondb: created store: 16 people maps + 1 credential map, 511 buckets each, 1000 persons planned
open         :  24478 ms
prepare      : 113685 ms (106 buckets formatted)
fill         : 983833 ms  1000 written, 1000/1000 total, 4 commits
verify       :  48023 ms  256 persons, 602 cards
VERIFY PASS
mutate       :  14712 ms  rev 0 -> 1  (0 revoked, 64 assigned)
re-verify    :  49758 ms  256 persons, 618 cards
VERIFY PASS

bench check :   200 ops in 22833000 us ->       8 ops/s   114165 us/op     800 blob ops  amp 701x
             : 200 granted, 0 denied, 0 unknown, 0 expired
bench byid  :   200 ops in 11528000 us ->      17 ops/s    57640 us/op     400 blob ops  amp 355x
bench miss  :   200 ops in 11327000 us ->      17 ops/s    56635 us/op     400 blob ops  amp 26214x
bench put   :   200 ops in 62767000 us ->       3 ops/s   313835 us/op    1992 blob ops  amp 1770x
bench cbor  :   200 ops in   191000 us ->    1047 ops/s      955 us/op       0 blob ops  amp 0x

store
  partition   : 8388608 B (8192 KiB), 65536 B sectors
  maps        : 16 person + 1 credential, 511 buckets each
  persons     : 1000 of 1000   credentials: 2479
  mean entry  : 432 B
  live content: 432759 B = 5.1 % of the partition
  bucket overflows: 0
  NOTE: physical occupancy (live + uncompacted garbage) is not
        observable through the API — see FINDINGS.md B3.

persondb: done — store at rev 1; rerun to prove persistence
```

No `expired` results in either run: at 1 000 persons the generator's validity
windows land differently than at 10 000, so the sample happens to miss that
case.

Note the store report also changed between editions — it no longer claims "511
buckets each" or restates the sector size, because neither is observable through
the API (`FINDINGS.md` K10). That is a reporting-honesty change, not a
geometry change.

### A note on running this after `app_perf`

This app does not enable `CONFIG_BLOB_DB_LARGE_PAYLOADS`, so it reads format
major 1 only. `app_perf` does enable it and leaves a major-2 store, which this
app refuses to mount with `-ENOTSUP` — and `FRESH_START` cannot save it, because
mount runs first. Erase the partition before the run; see the "Downgrading"
section of `app_perf/RESULTS.md`.

### Reproducing

```
west build -p always -b nrf5340dk/nrf5340/cpuapp app_cbor_persondb \
      -- -DCONFIG_APP_CBOR_PERSONDB_N_PERSONS=1000 \
         -DCONFIG_APP_CBOR_PERSONDB_FRESH_START=y
```

Start the console capture *before* flashing and let `nrfutil device program
--options reset=RESET_SYSTEM` start the run itself — a second reset issued
while the app is mid-QSPI-erase can leave the MX25R6435F answering JEDEC id
`00 00 00` on the next boot, needing `nrfutil device recover`. The console is
VCOM 2; resolve its `/dev/ttyACM*` node with `nrfutil device list` rather than
assuming, since it shifts with USB enumeration order.

## 9. Sizing history

The first full fill **failed**, and the failure is the most useful result here.

An analytic compound-Poisson estimate put eight person maps at 5.5 σ of
headroom, ~0.03 expected overflows. The fill hit `-ENOSPC` at person 9 232.
Enumerating the real population through the real hash (`tools/sizing.py`):

| person maps | mean bucket | **fullest bucket** | over 4 KB |
|---|--:|--:|--:|
| 8 | 1 009 B | **4 158 B** | 1 |
| 12 | 764 B | 3 286 B | 0 |
| **16** | **660 B** | **2 711 B** | **0** |
| 32 | 499 B | 2 369 B | 0 |

Sixteen was chosen: the fullest bucket sits at 66 % of the ceiling, and beyond
about sixteen maps the maximum stops falling — it is set by the Poisson tail
over a growing number of buckets, not by the mean — while the directory-rewrite
traffic of `FINDINGS.md` K5 keeps rising with every additional map.

The lesson is in `FINDINGS.md` K2: with no per-bucket occupancy query (K10) and
no growth path (K3), a `kvhash` map has to be sized by enumeration, up front.
This application can enumerate only because its dataset is a pure function of
an index. A deployment whose data arrives from outside cannot, and has no
recourse but to over-provision blindly.
