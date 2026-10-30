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
they appear. A full-scale DK run remains
outstanding for `A4`, but §5d now measures **5 000 persons on hardware** — half
scale — and puts the `A4` range at 2.0–2.4 h.

**The two-level `kvhash` is measured on the DK in §5e (1 000 persons) and
§5f (5 000 and 10 000).** At full scale **10 000 persons completes** — the
scale the one-level container could not reach — at 51.6 % live with 0 overflows,
and an access decision costs **114.9 flash operations / 26.79 ms**, less than
the same container spends at 1 000 persons. Writes move the other way: a `put`
costs **2.23 sector erases and 2.56 s**. Operation count predicts reads to
within 0.3 % and mispredicts writes by 93 %.

**The 1 000-person view alone is misleading.** Whole run 2.31×
faster than the sixteen-shard build it replaces and 19.2× faster than the
one-level container, with `fill` 4.52× faster than shards; reads and `put` cost
1.3–1.7× more, all of it transaction count. The proposal's §13 `native_sim`
timings do not transfer in either direction — see §5e.

**§4c measures kvhash spilling person records (`VAL_INLINE_MAX=64`) on
`native_sim`:** writes and erases drop by about a third, but reads get
**5–6× slower**, because ~8 000 extra blobs lengthen blob_db's per-lookup slot
walk for every map in the store. A net loss for this read-heavy app.

**`blob_db` now defaults to the UBI backend; §4a, §5 and §5b are
`flash_area`.** §5c measures the default's *timings* on the same board: reads
cost ~1.9× more, `fill` is 1.10× *cheaper*, and the `A4` floor improves
slightly to ≈2.0 h. §4b measures its *footprint*: **+23 032 B FLASH and
+3 408 B RAM**, which is 2.1% of this image's RAM and leaves §4a's B6 finding
— that two sector buffers own 83% of it — completely unchanged.

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
| Same on the **nRF5340-DK** (§5, post-merge) | **14.605 ms/op · 68 ops/s** |
| …on the DK before the large-payload merge | 114.2 ms/op · 8.8 ops/s |

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

**The flash-op column transfers to hardware.** This table predicts 351 flash ops
per `check` at 5 000 persons; the DK measured **344** on the UBI backend (§5d).
Two percent apart, across a different platform, backend and build — which is the
§1 claim holding at a scale where it could plausibly have broken.

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

| Phase | µs/op | store ops/op | flash ops/op | flash bytes/op | payload | **amplification** |
|---|--:|--:|--:|--:|--:|--:|
| `check` — card → person → permission (**R-D**) | **44** | 2 | 274 | 13.4 KB | 402 B | **33×** |
| `byid` — person id → record | 23 | 1 | 131 | 6.5 KB | 388 B | 17× |
| `miss` — unknown card | 22 | 1 | 144 | 6.9 KB | 23 B | 300× |
| `put` — rewrite a record + its index entries | 1 127 | 4.3 | 815 | 98 KB | 431 B | 228× |
| `cbor` — encode + decode, no flash | **5** | 0 | 0 | 0 | 388 B | — |

Two rows moved when the consistency defects were fixed, and the movement is the
cost of the fix rather than noise:

- **`put` went from 3.3 to 4.3 store operations**, 1 020 → 1 127 µs. That is the
  extra map get `persondb_person_put()` now performs to find which cards the
  stored version listed, so that a replace can unindex the ones it drops. Before
  the fix a replace left them resolving to a person who no longer listed them —
  a grant on a withdrawn credential. **10 % on the write path is what that
  correctness costs**, and the read path is untouched.
- **Flash operations per `check` read 274 here against 261 in earlier runs.**
  Not a regression and not the fix: per §3b, per-operation flash cost tracks how
  much superseded data sits in the sectors being walked — write history and
  compaction timing — and this store has one more mutation round in its past.
  The byte column moved 0.8 %, and the amplification factor not at all.

### Two `kvhash` instances — the sixteen shards removed

The shards are gone: one person map, one credential map. Same host, same
benchmark, 200 samples, steady state, **8 000 persons** (`DESIGN.md` §6.5 —
the constant was 10 000 and the layout's ceiling is 9 670).

| Phase | µs/op | store ops/op | flash ops/op | flash bytes/op | **amplification** | vs 16 shards |
|---|--:|--:|--:|--:|--:|--:|
| `check` — card → person → permission (**R-D**) | **179** | 2 | 230 | 37.4 KB | **93×** | **4.1× slower** |
| `byid` — person id → record | 94 | 1 | 111 | 19.4 KB | 51× | 4.1× |
| `miss` — unknown card | 89 | 1 | 120 | 18.0 KB | 782× | 4.0× |
| `put` — rewrite a record + its index entries | 918 | 4.3 | 707 | 122 KB | 283× | 0.81× |
| `cbor` — encode + decode, no flash | **6** | 0 | 0 | 0 | — | 1.2× |

Whole-run phases:

| Phase | | |
|---|--:|---|
| `open` | 5 ms | one registry read + one superblock read |
| `fill` | 11 709 ms | 8 000 written, 32 commits (first run) |
| `verify` | 77 ms | 256 persons, 618 cards — `VERIFY PASS` |
| `mutate` | 96 ms | 64 revoked, 64 assigned |
| `re-verify` | 76 ms | 256 persons, 618 cards — `VERIFY PASS` |
| store | | 3 466 801 B live = **41.3 %** of the partition, **0 bucket overflows** |

**The read path costs 4.1× what the sharded build cost, and every byte of it is
the directory.** `check` moves 37.4 KB where the sixteen-shard build moved
13.4 KB, on the same two store operations — the map got wider, not busier. A
`kvhash` get reads the whole bucket directory before it reads a bucket (K11);
one map means 2 047 buckets in that directory, so the read is 16 384 B whatever
the app is looking for. `miss` shows it undisguised: 18.0 KB of flash to
discover that a 23-byte key is not there, an amplification of 782×.

`put` is the one row that improved (0.81×), and not by any virtue of the
layout: writes were already dominated by rewriting a whole bucket and its
directory (K4, K5), and there are now two directories to keep instead of
seventeen.

**Stability, not a single sample.** Three consecutive runs — each a full
verify / mutate / re-verify / bench round on the store the previous one left —
hold at 3 466 801 B live, 41.3 %, zero overflows, with no drift. That is what
the 17 % margin under K13's 9 670-person ceiling buys, and the reason the
constant is 8 000 rather than 9 600.

**Comparability caveat.** §4 was taken at 10 000 persons and this table at
8 000, because 10 000 no longer completes and the benchmark was re-sized. The
20 % smaller store makes the buckets smaller and does not touch the directory,
so the comparison flatters this layout. It does not flatter it by 4×: the
directory read is 16 384 B at either scale, and it is the whole story.

**What these numbers are for.** They are worse across the board and that is the
result being reported, not a problem with the run. §4's figures were taken with
sixteen person maps hiding a 4 KB payload cap; these were taken with the app
built the way the stack intends. The difference between the two tables is what
the workaround was worth — and, per `DESIGN.md` §1, what it was concealing:
**K12** and **K13** are both reachable only from this side of it.

Reproduce: `west build -p -b native_sim app_cbor_persondb` (defaults are the
two-instance layout at 8 000), then run the binary twice in the same directory.

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
narrower than the byte column suggests.

Note that the DK measures **112** transactions per `check` against the 261 here.
That is **the dataset size, not the platform**: the DK run is 1 000 persons and
this table is 10 000, and §3b measures exactly 112 at 1 000 on `native_sim`. The
counters agreeing across the two platforms at equal scale is evidence *for* the
overlay mirroring the DK's geometry, not a discrepancy needing an explanation.

An earlier version of this paragraph blamed the gap on `blob_db`'s one-entry
index cache. That cache is compiled only under
`CONFIG_BLOB_DB_LARGE_PAYLOADS`, which this app does not enable — §9 says so
four hundred lines further down. Two numbers that differed for a boring reason
got an interesting explanation, and the check that would have caught it was
already in this file.

Whole-run phases:

| Phase | Time | Work |
|---|--:|---|
| open | 0 ms | 1 registry read + 1 superblock read |
| prepare | 2 ms | 106 buckets pre-formatted |
| fill (first run) | 17.0 s | 10 000 persons + 24 932 credentials = 34 932 map writes, 40 progress commits |
| verify | 271 ms | 256 persons + 664 credential resolutions |
| mutate | 249 ms | 64 revoked + 64 assigned, one revision |
| re-verify | 277 ms | same |


## 4c. Spilled person records (`CONFIG_BLOB_CONTAINER_KVHASH_VAL_INLINE_MAX=64`)

kvhash can store a value in a blob of its own instead of inside its bucket
(`doc/proposals/2026-10-09-kvhash-spill.md`). With the value limit at 64 B,
every person record (~380 B CBOR) is spilled and every credential (23 B) stays
inline. Same app, same 8 000-person default, `native_sim`; only that one option
differs. Both builds print `VERIFY PASS` twice and `bucket overflows: 0`, on the
first run and on the rerun, and the map geometry is identical (784 + 256
buckets). CI runs the 200-person form as `app.cbor_persondb.smoke.spill64`.

Rerun (rev 1 → 2), flash I/O counters and `app_perf_l0/tools/l0_timing.py
predict` over `models/mx25r64_nrf5340dk_full.json` (flash cost only):

| phase | inline: reads / bytes | spill64: reads / bytes | flash-only prediction, DK | |
|---|--:|--:|--:|--:|
| `check` (200) | 24 267 / 1 644 441 | 148 937 / 2 400 770 | 2.097 → 10.920 s | **×5.2** |
| `byid` (200)  | 11 693 / 1 079 101 |  84 107 / 1 208 358 | 1.083 → 6.130 s | **×5.7** |
| `miss` (200)  | 12 524 /   550 664 |  65 948 / 1 191 752 | 1.007 → 4.868 s | **×4.8** |
| `put` (200)   | 68 377 / 6 593 936 | 364 094 / 7 809 235 | 307.2 → 245.1 s | ×0.80 |

`put` writes 4 458 243 → 3 049 455 B and erases 225 → 165 sectors: a record
rewrite no longer repacks its whole bucket, so writes and erases drop by about
a third. That is the win spilling was built for, and it is real.

**Reads get five to six times slower, and not because of the extra read.** A
spilled `get` costs one more blob read, which would make it ~1.33× slower
(`app_perf_kvdb/RESULTS.md`). The `miss` row is the tell: it looks up cards that
do not exist, never reaches a person record, and still costs 4.8× more. The
cost is in blob_db. A `get` walks every slot header in its bucket, 12 B each
(`doc/impl/l1_bucketlog.md` §1.5), and an id's bucket is `id % n_buckets`.
Inline, this store is ~1 000 blobs (784 + 256 kvhash buckets, the directories,
the registry); spilled it is ~9 000, so every blob_db bucket holds roughly 8×
the slots, and **every lookup in the store pays for every spilled record**,
whichever map it belongs to. Read operations rise 5.3–7.2×; bytes only
1.1–2.2×, because the extra reads are 12-byte headers.

So for this app, at this limit, spilling is a net loss: a `check` goes from
≈10.5 ms to ≈55 ms of flash time on the DK, against a `put` 20 % cheaper, and
an access-control store is read far more than it is written. Two conclusions
carry beyond this app:

- **A spilled blob is not free to the rest of the store.** Its cost is paid by
  every lookup that shares its blob_db bucket, not only by its own `get`, so the
  spill limit should sit well above a store's typical value size, not at it.
  `app_perf_kvdb`'s value-blob runs did not show this, because they added only
  768 blobs.
- **The real limit is blob_db's linear slot walk.** An index from id to slot
  inside blob_db would make blob count nearly irrelevant to lookups, and is
  what would make spilling broadly worthwhile.

Nothing here has run on the DK; the column is the flash-only model, a floor
that leaves out UBI's per-transaction cost, which would widen the read gap.

## 4a. Footprint — nRF5340-DK (measured)

Cross-built with Zephyr SDK 1.0.1 (`arm-zephyr-eabi`) for
`nrf5340dk/nrf5340/cpuapp`. These are real target numbers, as are the timings
in §5.

**This section is the `flash_area` backend.** `blob_db` now defaults to UBI,
which adds 23 KB of ROM and 3.4 KB of RAM — see §4b for the default build's
numbers and for which conclusions below survive the change.

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

## 4b. Footprint on the UBI backend (the default)

> Measured on the old `wmilek/ubi` fork; zephyr-ubi v0.1.0 figures are in §5g.

§4a is `flash_area`. `blob_db` now defaults to `CONFIG_BLOB_DB_BACKEND_UBI`, so
these are the numbers a default build of this app actually produces. Both
columns were rebuilt at the same commit with the same method — `text + data`
from `arm-zephyr-eabi-size`, which is what Zephyr reports as the FLASH region —
so the delta is attributable to the backend and nothing else.

| | FLASH | of 1 MB | RAM | of 448 KB |
|---|--:|--:|--:|--:|
| `flash_area` | 61 316 B | 5.8 % | 157 695 B | 34.4 % |
| **UBI (default)** | **84 348 B** | **8.0 %** | **161 103 B** | **35.1 %** |
| **Δ** | **+23 032 B** | +2.2 pp | **+3 408 B** | +0.7 pp |

`app_perf_blob_db/RESULTS.md` measures the same delta independently at **+23 036 /
+3 408 B** on a different application. Four bytes apart on ROM and exact on
RAM, which is what it should be: the cost is the library, not the caller.

The `flash_area` column above is 2.2 KB larger than §4a's 59 072 B because the
tree has moved since that section was taken; §4a's *breakdown* still holds, and
this table is the one to diff against.

### ROM — where the 23 KB goes

| Component | Δ bytes | share |
|---|--:|--:|
| `ubi/lib` — the UBI library itself | **+13 982** | 60.7 % |
| linker/build artefacts not attributed to a path | +6 216 | 27.0 % |
| `zephyr/lib/utils` — CRC and helpers UBI pulls in | +1 474 | 6.4 % |
| `kernel/mutex.c` + `mem_slab.c` + `sched.c` | +944 | 4.1 % |
| `blob_db_store_ubi.c` against `blob_db_store_flash_area.c` | +412 | 1.8 % |
| `zephyr/subsys/storage` — flash_map partly dropped | −120 | −0.5 % |
| libc `memmove`/`memcpy` variant shifts | +124 | 0.5 % |

The store seam itself is nearly free: swapping the backend implementation costs
**412 bytes**. Everything else is UBI and what UBI depends on. Note the app,
`kvhash`, `rootreg` and `zcbor` are **byte-identical** across the two builds —
the abstraction holds, and nothing above L0 knows which backend it has.

### RAM — where the 3.4 KB goes

All of it is UBI's own state, in four `k_mem_slab` pools:

| Symbol | Bytes | |
|---|--:|---|
| `_k_mem_slab_buf_leaf_slab` | **2 176** | **the PEB pool — 16 B per PEB** |
| `_k_mem_slab_buf_scratch_slab` | 512 | |
| `_k_mem_slab_buf_volume_slab` | 440 | |
| `_k_mem_slab_buf_device_slab` | 136 | |
| four slab descriptors + `guard_mutex` + `active_partitions` | 136 | |
| **total** | **3 400** | `blob_db` itself grows by 8 B |

**The dominant term scales with `CONFIG_UBI_MAX_NR_OF_DATA_PEBS`**, which this
board's conf sets to 126 — the DK's 8 MiB partition in 64 KB blocks, less the 2
UBI reserves. That is the whole reason PR #20 sized the pool per geometry
instead of picking one generous default: `native_sim`'s 4 KB blocks need 2 046
PEBs, and the same symbol would then cost ~32 KB of RAM on a board that has
128 blocks.

### The shell frontend, and the stack that was never sized for it

§4a's shell row predates the payload bump. Rebuilt at the branch head with
`CONFIG_APP_CBOR_PERSONDB_FRONTEND_SHELL=y`: **FLASH 121 576 B, RAM 233 432 B
(50.9 % of the part)** against 85 724 / 202 064 for the benchmark frontend.

Most of the RAM difference is a fix rather than the shell. `blob_db` builds
every slot in a `MAX_PAYLOAD + 46` byte **stack** frame (B5 job 3), and
`prj.conf` sizes `CONFIG_MAIN_STACK_SIZE` for the resulting 19 870 B chain — but
the shell frontend runs that chain on the **shell thread**, which Zephyr defaults
to 2 048 B. Every `persondb` command overflowed it and halted the system on the
first keystroke:

```
uart:~$ persondb stat
<err> os: ***** USAGE FAULT *****
<err> os:   Stack overflow (context area not valid)
<err> os: Current thread: 0x200008f0 (shell_uart)
```

`CONFIG_SHELL_STACK_SIZE` now defaults to 28 672 when that frontend is selected,
costing ~26 KB of RAM and only in that build. The frontend was previously
buildable but not runnable, which is how a footprint row existed for something
that had never executed a command on hardware.

Verified against the 10 000-person store: `stat`, `show`, `cardof`, `card` and
`check` all work.

The shell also exposed that `scenario_now_us()` was reading `k_uptime_get()` —
whole milliseconds, multiplied by 1 000 to look like microseconds. Invisible in
the benchmark, where every phase averages over 200 samples, and ±3.7 % on the
shell's single-shot `check`. It now reads `k_uptime_ticks()`, which on this SoC
is 32 768 Hz, so ~30.5 µs. The difference:

```
before:  GRANTED in 27000 us   DENIED in 28000 us   (1 ms quantisation)
after:   GRANTED in 27314 us   DENIED in 27314 us
```

Five consecutive decisions measured 27 313–27 314 µs, so **an access decision is
deterministic to ~1 µs** and the apparent 27/28 ms jitter was entirely the clock.
Against §5f's 26.785 ms (run 1) and 27.350 ms (run 2) for the same operation
under the benchmark frontend, the interactive path lands inside the run-to-run
spread — the two frontends agree, as `DESIGN.md` §9 says they should.

It also sharpens §6's point about denials: **GRANTED and DENIED cost the same to
the microsecond** when the card is real, because both resolve card → person →
record and only the permission match differs. Only an *unknown card* is cheap
(§5f's `miss`, 12.5 ms), because it stops after one lookup.

### It does not move the needle on this app, and here is why

Against §4a's finding B6 — that `blob_db`'s two 64 KB sector buffers own 83 % of
this image's RAM — UBI's 3.4 KB is **2.1 %** of the total and 2.6 % of what
`g_bbuf` alone costs. The RAM problem in this application is unchanged by the
backend and remains the sector buffers.

ROM is a different story only in proportion: +23 KB takes the storage stack from
5.4 KB to roughly 19.4 KB, so what was 9.1 % of the image is now about 23 %.
The image is still 8 % of a 1 MB part, so §4a's conclusion stands — **nothing
about this application is ROM-constrained** — but the statement "the whole
storage stack is 5.4 KB of ROM" is now specific to `flash_area` and should not
be quoted for a default build.

### Reproducing

```
west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/pdb_ubi app_cbor_persondb
west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/pdb_fa  app_cbor_persondb \
      -- -DCONFIG_BLOB_DB_BACKEND_FLASH_AREA=y
$ZEPHYR_SDK_INSTALL_DIR/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-size \
      build/pdb_{fa,ubi}/zephyr/zephyr.elf
```

For the attribution tables, `-t rom_report` / `-t ram_report` on each build and
diff the generated `rom.json` / `ram.json` by path.

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
| `check` — `persondb_check` (**R-D**) | 114.2 ms | 7–11 ms | **14.605 ms** | ×7.8 | 701× → **24×** |
| `byid` — `persondb_person_get` | 57.6 ms | — | **7.770 ms** | ×7.4 | 355× → **13×** |
| `miss` — `persondb_card_owner`, unknown card | 56.6 ms | — | **7.165 ms** | ×7.9 | 26 214× → **210×** |
| `put` — `persondb_person_put`, settled store | 313.8 ms | — | **83.920 ms** | ×3.7 | 1 770× → **47×** |
| `cbor` — `persondb_person_roundtrip` | 955 µs | 955 µs | **950 µs** | — | — |

The row names are benchmark labels; the second half of each is the
`persondb.h` operation it calls (`scenario.c:337`, `:359`, `:373`, `:392`,
`:404`). §5b reads the same numbers as a cost model of that header, which is
the level application code is written against.

`cbor` is unchanged to within 0.5%, exactly as it must be: it touches no flash.
That it did not move is the control that makes the other four rows credible.

**§5a's prediction was too optimistic, and the reason is the constant it
guessed.** It estimated 7–11 ms from "261 transactions at a plausible 5–20 µs"
of fixed per-transaction cost. Measured: **14.605 ms**. The transaction count
was in fact 112 per `check` (22 415 flash ops over 200), not 261, so the
estimate was wrong in both terms and they partly cancelled — but the dominant
error is the per-transaction cost, which is far above 20 µs.

### This settles `FINDINGS.md` N1: a flash transaction costs ~65 µs

`app_perf_blob_db/RESULTS.md` fits two constants from the DK's `lg read` phases:
**~65 µs per flash read transaction and ~0.63 µs/B.** Applying them to this
app's own counters, with no refitting:

| | per `check` |
|---|--:|
| flash ops | 112.1 |
| bytes | 10 030 B |
| predicted 112.1 × 65.5 µs + 10 030 × 0.63 µs | **13.66 ms** |
| measured, less the 0.95 ms of CBOR | **13.66 ms** |

The constants fitted on `app_perf_blob_db` predict a different application's decision
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
matches `app_perf_blob_db`'s independently measured 1.06–1.09 s per sector.

**Fill gained only ×1.22 while the read-bound phases gained ×7.9.** That is
the sharpest confirmation of §5's finding that fill is dominated by erase and
compaction rather than by the operations it performs: making reads eight times
faster barely moved it. The erase share of fill is therefore now *higher* than
the ~80% estimated pre-merge, and it is the only remaining lever on
fill time.

Extrapolating fill linearly to 10 000 persons gives **≈2.2 h** (down from
≈2.7 h), described here as a floor on the reasoning that occupancy and
compaction both worsen with scale. **§5d measured that and it is wrong for
`fill`:** at 5 000 persons `fill` costs *less* per person than at 1 000, because
early writes into a growing store are cheap. The reasoning does hold for the
steady-state write path, which §5d finds compaction-bound.

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

**Predicted `check` after the merge — and the prediction was wrong.** This
section estimated ~6 ms of data plus 1.3–5 ms of fixed cost from "261
transactions at a plausible 5–20 µs", giving **≈ 7–11 ms**. The board measured
**14.605 ms** (§5). Both inputs were wrong and partly cancelled: 112
transactions, not 261, but at ~65 µs each rather than 5–20 — the
per-transaction cost was low by 3–13×.

The prediction is left standing above rather than quietly corrected, because
the size of the error is the point: **a plausible-looking estimate of fixed
per-transaction cost was off by up to an order of magnitude**, and no amount of
`native_sim` work would have revealed it. See `FINDINGS.md` N1 for what the
measurement changed.

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
> hand, `app_perf_blob_db`'s independently fitted model reproduces this app's `check`
> cost to a fraction of a percent (§5).

**The CBOR conclusion has to be restated.** This document previously said the
codec was "a projected 0.009 % of the decision on hardware". That was the
`native_sim` 6 µs carried across unchanged. Measured, it is **955 µs on a
128 MHz Cortex-M33 — 159× the host figure**:

| | encode+decode | `check` | codec share |
|---|--:|--:|--:|
| `native_sim`, pre-merge | 6 µs | 580 µs | 1.0 % |
| **DK, pre-merge** | **955 µs** | **114.2 ms** | **0.84 %** |
| **DK, post-merge (measured)** | **950 µs** | **14.605 ms** | **6.5 %** |

The headline conclusion survives — **the serialization format is not the
bottleneck**, and refusing to denormalize (D2) remains right. But the margin is
1–2 orders of magnitude thinner than first claimed: the codec is **6.5 %** of a
decision on today's code, against the "0.009 %" this document once projected.
The 9–14 % predicted alongside the 7–11 ms guess was the closer of the two
estimates, and still high, for the same reason — the storage term was
underestimated.

At 6.5 % the codec is no longer beneath notice, and it is now the *second*
largest term in a decision after flash. Had the storage layer been fast from
the start, "CBOR costs nothing" would have needed the board to say honestly.

Two lessons, both about method rather than about the stack:

- **Compute does not transfer between platforms.** The flash model projected
  within 1.65×; the compute figure was wrong by 159×. Only the part backed by a
  hardware constant travelled.
- **A ratio between two things measured on different platforms is not a
  ratio.** "1 % of the decision" was `native_sim` compute over `native_sim`
  flash, and neither term survived contact with the board.

## 5b. Cost of the `persondb.h` operations

The tables above are organised by benchmark phase. Application code is written
against `persondb.h` (`DESIGN.md` F12), so this section restates the same
measurements per operation of that header, and — more usefully — says which
operations are **not** measured at all.

### The read side: one map get ≈ 7 ms

| operation | cost | how known |
|---|--:|---|
| `persondb_person_get` | **7.770 ms** | measured (`byid`) |
| `persondb_card_owner`, card not found | **7.165 ms** | measured (`miss`) |
| `persondb_card_owner`, card found | **≈6.8–7.0 ms** | derived, below |
| `persondb_check` | **14.605 ms** | measured (`check`) |
| `persondb_person_roundtrip` | **0.950 ms** | measured (`cbor`), no flash |

`verify` and `re-verify` are pure compositions of two calls, which solves for
the hit cost the benchmark never isolates:

| phase | composition | measured | implies a hit at |
|---|---|--:|--:|
| verify | 256 `person_get` + 602 `card_owner` | 6 064 ms | **6.77 ms** |
| re-verify | 256 `person_get` + 618 `card_owner` | 6 291 ms | **6.96 ms** |

Two independent phases agree, and a hit being slightly cheaper than the 7.165 ms
miss is the expected direction: a miss must scan the whole bucket.

So the read side has a simple rule — **cost ≈ (number of map gets) × ~7 ms.**
It predicts the composite operation from its parts:

```
persondb_check = card_owner + person_get = 6.77 + 7.770 = 14.54 ms
                                measured:                 14.605 ms   (0.4%)
```

Permission evaluation is free: it runs inside the already-fetched record, which
is why `check` is two gets and not three. Nothing above this header can beat
~7 ms per lookup without changing how many map gets an operation needs — see
`FINDINGS.md` K11, where a map get is two `blob_db` calls rather than one.

### The write side: the same call can cost 84 ms or 808 ms

| operation | cost | how known |
|---|--:|---|
| `persondb_person_put`, rewriting into a settled bucket | **83.920 ms** | measured (`put`) — see note below |
| `persondb_person_put`, when the write grows a bucket into compaction | **807.7 ms** | measured (fill, per person) |
| `persondb_card_assign` | **44.5 ms** | measured (mutate, 64 assigns) |
| `persondb_prepare` | **1 100 ms per bucket** | measured (prepare, 106 buckets) |
| `persondb_open` including the `FRESH_START` erase | **25.8 s** | measured (open) |

**The 9.6× spread on `persondb_person_put` is the most important thing on this
page for a caller**, and the header gives no way to predict which case you get.
Both figures are the same function on the same board in the same run: the
benchmark rewrites an existing record, while the fill grows buckets until their
64 KB sector log must be compacted, and a compaction erases at ~1.09 s. Op
counts confirm the two are otherwise the same shape of work — 3.32 map ops for
the benchmark's `put` against 3.48 map writes per person during fill — so the
gap is erase, not extra work.

A caller that needs a bounded `put` therefore cannot get one from this API; the
cost depends on the target bucket's fill state, which is not observable
(`FINDINGS.md` B3, K10).

**These write-side figures predate the replace fix.** `persondb_person_put()`
now performs one additional map get, to find which cards the stored version
listed so a replace can unindex the ones it drops (`README.md` practice 5). On
`native_sim` that measured +10 % (§4); on the DK a map get is ~7 ms, so expect
`person_put` nearer **91 ms** and `card_assign` unchanged. The read side is
untouched. A board re-run would settle it; the numbers above are marked rather
than adjusted, because an estimate in a measured table is how §5a went wrong.

### Not measured

No phase exercises these, so this file says nothing about them:

- `persondb_card_revoke` — the first run had nothing to revoke (`0 revoked`); a
  rerun on a populated store would measure it
- `persondb_open` **attaching to an existing store** — every DK run so far used
  `FRESH_START`, so the only figure is 25.8 s of erase and the steady-state open
  cost is unknown
- `persondb_person_delete`, `persondb_permission_grant`,
  `persondb_permission_revoke`
- `persondb_close`, `persondb_erase`, `persondb_stat`,
  `persondb_progress_get`/`_set`, `persondb_person_equal`,
  `persondb_person_record_bytes`, `persondb_person_credential_bytes`

That is 5 of the header's operations measured directly, one derived, and the
rest unknown — worth remembering before quoting this file as the cost of the
API as a whole.

## 5c. On the UBI backend (the default)

> Measured on the old `wmilek/ubi` fork; zephyr-ubi v0.1.0 figures are in §5g.

`blob_db` now defaults to `CONFIG_BLOB_DB_BACKEND_UBI`; everything above is
`flash_area`. Same board, same 1 000-person config, with the backend and its
PEB pool coming from `boards/nrf5340dk_nrf5340_cpuapp.conf` rather than a
command-line override.

Both columns were taken on `f1100f1`. The `flash_area` column is therefore
the re-run recorded in PR #18, not the `338ec24` figures in §5 above — the
two differ by ~2% (e.g. `check` 14.260 against 14.605 ms), which is run-to-run
noise and does not move any ratio below. Where §5's numbers and these
disagree by that much, §5 is the older code.

| phase | `flash_area` | **UBI** | |
|---|--:|--:|--:|
| `check` | 14.260 ms | **27.660 ms** | ×1.94 slower |
| `byid` | 7.600 ms | **14.060 ms** | ×1.85 slower |
| `miss` | 7.005 ms | **13.720 ms** | ×1.96 slower |
| `put` | 90.470 ms | **107.405 ms** | ×1.19 slower |
| `cbor` | 0.970 ms | **0.945 ms** | unchanged |
| `verify` | 5 915 ms | 11 379 ms | ×1.92 slower |
| `mutate` | 3 233 ms | 5 325 ms | ×1.65 slower |
| `re-verify` | 6 138 ms | 12 010 ms | ×1.96 slower |
| `open` | 22 468 ms | 26 014 ms | ×1.16 slower |
| `prepare` | 112 766 ms (106 buckets) | 109 969 ms (100 buckets) | 1 064 → 1 099 ms/bucket |
| **`fill`** | **808 745 ms** | **732 573 ms** | **×1.10 faster** |

`cbor` is unchanged, which is the control: it touches no flash. The store is
structurally identical — 2 479 credentials, mean entry 432 B, 0 bucket
overflows, `VERIFY PASS` both times.

### The UBI cost model transfers between applications

`app_perf_blob_db/RESULTS.md` fits UBI at **178 µs per flash transaction and
0.616 µs/B**, from a different app with different payloads. Applied to this
app's own counters for `check`, with no refitting:

| | per `check` |
|---|--:|
| flash transactions | 115.6 |
| bytes | 10 073 B |
| predicted 115.6 × 178 µs + 10 073 × 0.616 µs + 945 µs CBOR | **27.73 ms** |
| **measured** | **27.66 ms** |

**0.3% apart.** Both backends' constants now predict this application's
decision cost from `app_perf_blob_db`'s numbers, which is the strongest evidence in
either file that the model is real rather than fitted noise.

It also moves `FINDINGS.md` N1 further: fixed transaction cost is now
**~77% of a decision** on the default backend, against ~54% on flash_area.
Whatever reduces transaction count is worth roughly half again as much as it
was.

### Fill is *faster*, which nothing else here is

Every read-bound phase pays ~1.9×, and yet `fill` — the phase that dominates
the whole run — is **76 s cheaper** on UBI. That is consistent with the rest
of this file rather than in tension with it: `fill` is ~97% sector erase, the
one cost UBI does not inflate, and UBI's block management appears to place
erases slightly more efficiently than repeated in-place formatting does.

The practical consequence is that the full-scale `A4` estimate does **not**
get worse on the new default. Extrapolating this fill linearly to 10 000
persons gives **≈2.0 h**, against ≈2.2 h on flash_area.

§5d has since measured 5 000 persons on the board and reaches the same ≈2.0 h by
a different route — but also shows the linear extrapolation is not the *floor*
this file kept calling it. Read the `A4` number as 2.0–2.4 h, and see §5d for
why `fill` is sublinear while the write path is not.

### `prepare` does not get kvdb's discount

`app_perf_kvdb/RESULTS.md` reports its `prepare` phase 909× faster on UBI,
because its `FRESH_START` erases the whole partition first and leaves UBI a
pool of pre-erased PEBs. This app's format only touches what it needs — its
`open` is 26 s against kvdb's 145 s — so there is no such pool and each
bucket format pays a real erase, at 1 099 ms against flash_area's 1 064 ms.
Two apps, opposite results, same mechanism: **UBI moves erase cost in time,
it does not remove it.**

Note also `prepare` formats **100** buckets here against 106 on flash_area:
UBI reserves 2 PEBs for its own headers, so the volume is smaller.

## 5d. The 5 000-person run — what scale actually does

The first DK run above 1 000 persons. It exists because every hardware number
in this file was taken at a tenth of the `A4` target, and §5c's `A4` projection
was a linear extrapolation from that single point. Same board, same default UBI
backend, `FRESH_START=y`, partition raw-erased first. Raw capture in §8b.

**Whole run: 15.5 min at 1 000 persons → 63.5 min at 5 000.** That is ×4.10 for
×5 the data — sublinear overall, but the aggregate hides two opposite movements.

| phase | @1 000 | @5 000 | ratio | per-unit |
|---|--:|--:|--:|---|
| `open` | 26.0 s | 148.2 s | ×5.70 | — |
| `prepare` | 110.0 s | **0.13 s** | ×0.001 | 100 buckets both |
| **`open` + `prepare`** | **136.0 s** | **148.3 s** | **×1.09** | — |
| `fill` | 732.6 s | 3 376.8 s | ×4.61 | **732.6 → 675.4 ms/person** |
| `verify` (256) | 11.4 s | 30.9 s | ×2.71 | 44.5 → 120.5 ms/person |
| `mutate` (64) | 5.3 s | 30.6 s | ×5.75 | **83.2 → 478.7 ms/card** |
| `re-verify` (256) | 12.0 s | 31.0 s | ×2.58 | 46.9 → 121.1 ms/person |
| `check` | 27.66 ms | 67.69 ms | ×2.45 | |
| `byid` | 14.06 ms | 34.14 ms | ×2.43 | |
| `miss` | 13.72 ms | 34.40 ms | ×2.51 | |
| **`put`** | **107.41 ms** | **811.39 ms** | **×7.55** | |
| `cbor` | 0.945 ms | 0.940 ms | ×0.99 | the control — touches no flash |

`VERIFY PASS` both passes, **0 bucket overflows**, 12 550 credentials, mean
entry 433 B, live content 2 169 730 B = 25.8 % of the partition (33.1 % of the
UBI volume's 100 × 65 488 B). The `tools/sizing.py` geometry holds at five times
the scale it was checked at.

### Reads scale with transactions; writes scale with erase

The `app_perf_blob_db` UBI constants — 178 µs per flash transaction, 0.616 µs/B, fitted
on a different app at a different scale — applied to this run's own counters
with no refitting:

| | flash ops | bytes | predicted | measured | error |
|---|--:|--:|--:|--:|--:|
| `check` | 344.0 | 13 494 | 70.48 ms | **67.69 ms** | +4.1 % |
| `byid` | 170.8 | 6 758 | 34.57 ms | **34.14 ms** | +1.2 % |
| `miss` | 175.7 | 6 734 | 35.42 ms | **34.40 ms** | +3.0 % |
| `put` | 1 086.6 | 76 619 | 240.61 ms | **811.39 ms** | **−70.3 %** |

**Three reads within 4 %, and the write off by a factor of 3.4.** The model has
no erase term, and that is now the whole story of the write path. The residual
is 570.8 ms per `put`, which at 1.069 s per 64 KB sector is **0.53 erases per
`put`** — against 0.035 at 1 000 persons, a ×15 rise for ×5 the data. Writes at
this scale are compaction-bound: roughly every second `put` pays a sector erase.
`mutate` says the same thing from the other direction, at 478.7 ms per card
assigned.

So the two-constant model is not wrong, it is incomplete, and the boundary is
sharp: **it predicts reads at any scale and cannot predict writes at any scale.**
`FINDINGS.md` B2 (five erases per compaction) is where the missing term lives.

### Transactions triple, bytes barely move

| per op | flash ops ×  | bytes × |
|---|--:|--:|
| `check` | ×3.07 | ×1.35 |
| `byid` | ×3.18 | ×1.32 |
| `miss` | ×3.09 | ×1.39 |
| `put` | ×3.41 | ×3.67 |

A deeper bucket costs more *transactions* because the slot walk visits more
headers; it does not cost proportionally more *bytes*, because the payload it
finally reads is the same 433 B record. Anything that reduces transaction count
therefore pays off at scale roughly three times better than at 1 000 persons —
which is `FINDINGS.md` N1, sharpened again.

The write row is the exception and it is diagnostic: `put` is the one operation
whose bytes grow with its transactions, because compaction copies live data.

### §3b's scaling law transferred to hardware

§3b predicted 351 flash ops per `check` at 5 000 persons, measured on
`native_sim`. **The DK measured 344.** Two percent apart, on a counter taken
from a different platform, a different backend and a different build. §1's claim
that structure transfers and wall-clock does not now has a second confirmation
at a scale where the two could plausibly have diverged.

### `fill` is sublinear — the standing "floor" claim was wrong

§5 and §5c both say extrapolating `fill` linearly is a **floor**, on the
reasoning that occupancy and compaction worsen with scale. Measured, it is
closer to a ceiling: **732.6 ms per person at 1 000, 675.4 ms at 5 000.** Taking
the whole store-creation path together — the honest unit, see below — 868.6 ms
per person becomes 705.0 ms, **19 % cheaper at five times the scale.**

This does not contradict the `put` blow-up; the two measure different things.
`fill` is the *average* cost of writing person *n* as the store grows from empty
to 25.8 % occupancy, and the early writes are cheap. Bench `put` is the
*steady-state* cost of a replace at the final occupancy, which is the expensive
end of that same curve. A store filled to 51.6 % will have a more expensive
second half than this one did — so `fill`'s per-person cost should eventually
turn back up, and two points cannot say where.

### `open` and `prepare` are one erase budget, not two

They traded places completely: `prepare` went from 110.0 s to **0.13 s** while
`open` went from 26.0 s to 148.2 s, for a combined change of +9 %. At 5 000
persons the store-creation path erases essentially the whole device up front,
which hands `prepare` a full pool of pre-erased PEBs and makes it free; at
1 000 it did not, and `prepare` paid a sector erase per bucket.

This is the third app to show it. `app_perf_kvdb/RESULTS.md` sees `prepare` 909×
faster on its `FRESH_START` path; `app_perf_mc/RESULTS.md` measures both halves
from one binary run twice (×861). **Quote `open` + `prepare` as a single figure
for this app, never either alone** — the split is an artefact of which phase
happened to find erased blocks.

### Revised `A4` projection

Linear from this run's `fill` gives ≈1.88 h, and the whole run ≈**2.0 h** — the
same number §5c reached from the 1 000-person point, by a different route. But
`fill`'s per-person cost should rise over the second half as occupancy goes
25.8 % → 51.6 %, so **treat 2.0 h as the optimistic end and 2.0–2.4 h as the
range.**

The bench phases are the part that will not extrapolate. §3b's flash-op count
per `check` is **non-monotonic** — 351 at 5 000, falling to 261 at 10 000 —
because per-operation cost tracks uncompacted garbage rather than live fill. If
that holds on hardware, decisions at 10 000 persons will be *faster* than the
67.69 ms measured here, and this 5 000-person run is the worst point in the
range rather than a midpoint. `put`, being erase-bound, will not get that
reprieve.

## 5e. The two-level `kvhash` on hardware — at 1 000 persons

> **Read §5f before quoting this section's read results.** Everything here is
> measured at 1 000 persons, which is *below the crossover*: at 5 000 and 10 000
> the read regression reported here reverses into a 2× improvement, and the
> `fill` win reported here erodes. The counters and the mechanism stand; the
> ranking is scale-bound.

The proposal's §13.2 says plainly that nothing had run on the DK. This is that
run, at **1 000 persons** — the scale §5/§5c hold sixteen-shard DK numbers at,
so all three builds compare like for like. Default UBI backend, `FRESH_START`,
partition raw-erased first. Raw capture in §8c.

It builds for ARM, boots, and passes both verifies with `VERIFY PASS`, 2 479
credentials, mean entry 432 B, **0 bucket overflows and 0 buckets past
near-full**. `stat()` reports **people map depth 1, 250 buckets; credential map
depth 2, 256 buckets.**

The middle column is the one-level build this proposal replaces
(`0904b8f`, `DESIGN.md` §12.2), measured on the same board at the same scale.

| phase | sixteen shards | one level | **two level** | vs shards | vs one level |
|---|--:|--:|--:|--:|--:|
| `open` + `prepare` | 136.0 s | 147.4 s | 146.0 s | ×1.07 | ×1.01 faster |
| **`fill`** | 732.6 s | 7 430.4 s | **162.1 s** | **×4.52 faster** | **×45.8 faster** |
| `verify` (256) | 11.4 s | 19.3 s | 16.4 s | ×1.44 slower | ×1.18 faster |
| `mutate` (64) | 5.3 s | 54.5 s | 9.2 s | ×1.72 slower | ×5.95 faster |
| `re-verify` (256) | 12.0 s | 20.1 s | 17.4 s | ×1.44 slower | ×1.16 faster |
| `check` | 27.66 ms | 46.50 ms | 36.04 ms | ×1.30 slower | ×1.29 faster |
| `byid` | 14.06 ms | 23.99 ms | **14.11 ms** | **×1.00** | ×1.70 faster |
| `miss` | 13.72 ms | 21.47 ms | 22.35 ms | ×1.63 slower | ×1.04 slower |
| `put` | 107.41 ms | 145.14 ms | 179.69 ms | ×1.67 slower | ×1.24 slower |
| `cbor` | 0.945 ms | 0.950 ms | 0.985 ms | control — no flash |
| **whole run** | **15.5 min** | **2.14 h** | **6.69 min** | **×2.31 faster** | **×19.2 faster** |

**The container beats the workaround it replaces, on the whole run and by a
wide margin on `fill`.** That is a stronger claim than `native_sim` could make,
where `fill` was still slower than sharded. It is not uniformly better: reads
and `put` cost more than the sixteen-shard build did, and the reason is
specific.

### The trade is bytes for transactions, and UBI prices those apart

| per op | flash ops | | | bytes | | |
|---|--:|--:|--:|--:|--:|--:|
| | shards | one level | **two level** | shards | one level | **two level** |
| `check` | 115.6 | 120.9 | **176.2** | 10 073 | 34 736 | **6 330** |
| `byid` | 53.8 | 60.6 | **57.5** | 5 135 | 17 614 | **4 432** |
| `miss` | 56.8 | 51.5 | **118.5** | 4 846 | 16 998 | **1 860** |
| `put` | 318.4 | 402.2 | **555.8** | 20 870 | 77 326 | **17 636** |

Every byte column improves — `miss` moves **2.6× fewer bytes than the sharded
build and 9.1× fewer than one level**, and amplification falls from 210× and
739× to **80×**. Every transaction column rises. On UBI a transaction costs
178 µs and a byte 0.616 µs, so the axis that got better is the cheap one.

§5c's constants, with no refitting, price the result exactly:

| | predicted | measured | error |
|---|--:|--:|--:|
| `check` | 36.25 ms | 36.04 ms | **+0.6 %** |
| `miss` | 22.24 ms | 22.35 ms | **−0.5 %** |
| `byid` | 12.97 ms | 14.11 ms | −8.1 % |
| `put` | 109.80 ms | 179.69 ms | −38.9 % |

Reads land within a percent. `put`'s residual is 69.9 ms, or **0.065 sector
erases per operation** — against 0.035 for the sharded build and 0.024 for one
level, so writes still carry a little more erase than either.

### `byid` is the control that identifies the cost as *depth*

`byid` is a dead heat with the sixteen-shard build — 14.11 ms against 14.06 —
and it is the one lookup that goes to the people map, which `stat()` reports at
**depth 1**. `check` is two lookups, one into the **depth-2** credential map,
and it costs ×1.30. `miss` is a single depth-2 lookup and costs ×1.63.

So the read regression is not the second level *existing*; it is the second
level being *traversed*. A depth-1 map performs exactly as the sharded build
did. That also predicts the regression shrinks as maps get wider relative to
their contents, and grows wherever depth 2 is reached.

### What `native_sim` got wrong, in both directions again

| claim (§13, `native_sim`) | on the DK |
|---|---|
| `fill` 3.3× faster than one level | **45.8× faster** — understated ~14× |
| `check` 2.6× faster than one level | **1.29× faster** — overstated ~2× |
| 4.5× fewer bytes per `check` | **5.49× fewer** — beaten |

The byte counters transfer and are if anything conservative. The *timings*
transfer in neither direction, and both errors have one root, as in §5d and in
the one-level measurement: `native_sim` counts bytes and models no erase
latency. It therefore cannot see the compaction this change removes — which is
where nearly all of the `fill` win lives — and it cannot see that the
transactions this change adds are the expensive axis on real flash.

**This is §1 earning its place for the third time in this file.** Structure
transferred; the ranking did not.

### What this means for the proposal

On hardware the change is a clear net win at this scale: the whole run is 2.31×
faster than the build it replaces, capacity is restored, amplification falls
everywhere, and nothing regressed by more than ×1.7. The costs are real and
worth stating without softening — `put` is the slowest of the three builds, and
`miss` is marginally slower than one level despite moving a ninth of the bytes.

Both are transaction-count costs, which `FINDINGS.md` N1 identifies as the
dominant term on this stack. **The lever that would pay best here is reducing
transactions per traversal, not bytes** — the bytes are already good.

## 5f. Two-level `kvhash` at 5 000 and 10 000 persons

§5e measured this container at 1 000 persons and concluded reads had regressed.
**That conclusion is scale-bound and does not survive here.** Four runs, two
scales, two runs each — the first fills, the second is steady state. Default UBI
backend, partition raw-erased before each scale. Raw captures in §8d and §8e.

### 10 000 persons completes on hardware

The scale that returned `-ENOSPC` at person 9 670 on the one-level container
finishes on the board: **10 000 of 10 000 written, 40 commits, `VERIFY PASS`
twice**, live content **4 336 158 B = 51.6 %**, 24 932 credentials, mean entry
433 B, **0 bucket overflows and 0 buckets past near-full.**

Those are byte-for-byte the `native_sim` figures and the sixteen-shard build's
figures at this scale, so the structural claim transfers exactly. Even the two
`expired` results in `check` reproduce §8's note about validity windows at
10 000. `fill` took **2.80 h**; whole run 3.02 h.

The `kvhash: bucket … full` warning stayed silent throughout, on hardware as in
simulation.

### Reads get *faster* as the dataset grows

| `check` | flash ops | bytes | wall clock |
|---|--:|--:|--:|
| two level @1 000 | 176.2 | 6 330 B | 36.04 ms |
| two level @5 000 | 144.5 | 7 763 B | 31.15 ms |
| **two level @10 000** | **114.9** | 8 591 B | **26.79 ms** |
| sixteen shards @1 000 | 115.6 | 10 073 B | 27.66 ms |
| sixteen shards @5 000 | 344.0 | 13 494 B | 67.69 ms |

**114.9 operations per access decision at full scale**, against the 135 the
proposal predicted — beaten, and in the right direction. Declared capacity buys
buckets (250 → 484 → 961), buckets are therefore shallower, and the slot walk
per lookup shortens faster than the extra level costs.

The consequence is worth stating plainly: **an access decision at 10 000 persons
costs less than the same container answers at 1 000** (26.79 against 36.04 ms),
and it matches the sixteen-shard build's best DK figure ever recorded — which
that build only reached at a tenth of this scale. `verify` shows the same shape
end to end: 16.4 s → 13.2 s → **11.6 s** across the three scales, where the
sixteen-shard build went 11.4 s → 30.9 s over the first two.

`byid` 14.77 ms / 58.2 ops and `miss` 12.55 ms / 56.9 ops at 10 000 follow.

**This is the result the branch is for, and the DK states it more strongly than
`native_sim` could.** §5e's read regression was real at 1 000 persons and is an
artefact of measuring below the crossover.

### Writes: operations fall, wall clock explodes

| `put` | flash ops | bytes | wall clock | **erases/op** |
|---|--:|--:|--:|--:|
| @1 000 | 555.8 | 17 636 B | 179.7 ms | 0.065 |
| @5 000 | 400.0 | 89 164 B | 1 091.5 ms | 0.90 |
| **@10 000** | **326.8** | **194 721 B** | **2 564.4 ms** | **2.23** |

**Operation count falls 41 % while wall clock rises 14×.** A single `put` at
full scale is 2.56 seconds and pays over two whole sector erases. Amplification
goes 40× → 207× → **453×**.

The erase column is derived, not measured: it is what remains after charging
§5c's UBI constants (178 µs per transaction, 0.616 µs/B) against the measured
counters. Nothing in the stack reports erases directly (`FINDINGS.md` B3), which
is exactly why this cost stayed invisible until wall clock exposed it.

`mutate` says the same: 143 ms per card at 1 000, 821 ms at 5 000, **1 533 ms**
at 10 000, and 1 813 ms per card in the steady-state run where cards are revoked
as well as granted.

### The operation-count model predicts reads and cannot predict writes

Applying §5c's constants to each measured counter set, with no refitting:

| | predicted | measured | error |
|---|--:|--:|--:|
| `check` @10 000 | 26.71 ms | 26.79 ms | **+0.3 %** |
| `check` @5 000 | 31.49 ms | 31.15 ms | **+1.1 %** |
| `byid` @5 000 | 16.33 ms | 17.28 ms | −5.5 % |
| `put` @10 000 | 178.12 ms | 2 564.40 ms | **−93 %** |

Reads land within about a percent at both scales. `put` is out by more than an
order of magnitude, and the gap is erase.

**So the brief's thesis — that operation count is what hardware charges for and
therefore the result to check — is confirmed for reads and refuted for writes.**
A contradiction, recorded as a finding: the two-level container concentrates
data into fewer, larger buckets, which shortens every read walk and lengthens
every write's erase debt. `MIN_BUCKET_BYTES = 4096` sets the floor on how large,
so it is also the knob that sets this cost.

### `fill` crosses over in the opposite direction

| `fill`, ms per person | @1 000 | @5 000 | @10 000 |
|---|--:|--:|--:|
| sixteen shards | 732.6 | 675.4 | not run |
| **two level** | **162.1** | **546.8** | **1 007.3** |
| ratio | ×4.52 faster | ×1.24 faster | — |

The fill advantage erodes with scale as the read advantage grows. At 10 000 the
sixteen-shard build was never run on this board; §5d's linear extrapolation from
its 5 000 point gives ~6 754 s against 10 073 s measured here, which suggests
two-level is now the slower filler — but that is a measurement against an
extrapolation, and §5d itself found linear extrapolation unreliable in both
directions. **Recorded as an open comparison, not a conclusion.** Settling it
needs the sixteen-shard build run at 10 000, which §5d put beyond `A4`'s reach
at ~2.0–2.4 h.

### Declared capacity eats the volume

| | `prepare` formats |
|---|--:|
| sixteen shards @1 000 | 100 buckets |
| two level @1 000 | 99 |
| two level @5 000 | 77 |
| **two level @10 000** | **68** |

Structure consumes 32 of the volume's ~100 PEBs at full scale — roughly 2 MB of
the 8 MB part gone before a person is written. The 51.6 % live figure is against
the whole partition and does not show this; measured against what remains for
data it is far tighter. That is the term B13's medium wall actually moves on,
and it is the one to watch if the declared capacity is raised again.

### Steady state, and run-to-run agreement

Second runs agree closely with first runs on every read — at 10 000, `check`
26.79 → 27.35 ms (+2.1 %), `byid` 14.77 → 14.34, `miss` 12.55 → 13.65, `put`
2 564.4 → 2 558.4 ms (−0.2 %). A rerun's `open` is 789 ms at 10 000 and 935 ms
at 5 000, against ~144 s when the run also creates the store.

`mutate` is the one phase that is materially more expensive in steady state,
because a first run has nothing to revoke: 232.1 s against 98.1 s at 10 000, for
twice the card operations.

### What to take from this

At the scale the app is designed for, the two-level container is a clear win on
every read and a clear loss on every write, and both are larger than
`native_sim` suggested. The capacity claim holds exactly. Whether the trade is
worth taking depends on the read/write mix — `app_perf_kvdb/RESULTS.md` records
the same change making that app's read-dominated loop *slower*, which is the
same trade seen from the other end.

The write cost is the open problem, and it is not a `kvhash` bug: it is
`blob_db` compaction being driven by bucket size, on a medium whose erase
granularity the map cannot see. **A `put` costing 2.23 sector erases is the
finding this run contributes.**

## 5g. On zephyr-ubi v0.1.0 (PR #35)

§4b, §5c and §5e–§5f measured the UBI backend on the old `wmilek/ubi` fork.
PR #35 moved it to the [zephyr-ubi](https://github.com/kamil-kielbasa/zephyr-ubi)
`v0.1.0` release. Same board and build as §5e (Zephyr `4a405846193f`, SDK
1.0.1): two-level `kvhash`, 1 000 persons, `FRESH_START`, partition
raw-erased first, built-in UBI key. The last column adds
`CONFIG_BLOB_DB_UBI_ATOMIC_REPLACE=y` (`36e9169`).

| phase | old fork (§8c, `ade6a3e`) | **v0.1.0 (`99a2ca6`)** | + `ATOMIC_REPLACE` |
|---|--:|--:|--:|
| `open` | 145.8 s | 37.0 s | 36.9 s |
| `prepare` (buckets) | 0.13 s (99) | 109.3 s (98) | 109.9 s (98) |
| `open` + `prepare` | 146.0 s | 146.3 s | 146.8 s |
| **`fill`** | 162.1 s | **130.6 s** | **105.4 s** |
| `verify` / `mutate` / `re-verify` | 16.4 / 9.2 / 17.4 s | 7.4 / 6.4 / 7.7 s | 7.2 / 6.3 / 7.5 s |
| `check` | 36.04 ms | **17.13 ms** | 16.69 ms |
| `byid` | 14.11 ms | **7.98 ms** | 7.84 ms |
| `miss` | 22.35 ms | **9.44 ms** | 9.16 ms |
| `put` | 179.69 ms | **121.98 ms** | 119.64 ms |
| `cbor` (control, no flash) | 0.985 ms | 0.950 ms | 0.949 ms |
| **whole run** | **6.69 min** | **5.50 min** | **5.07 min** |
| FLASH / RAM (B) | 86 032 / 202 064 | 104 328 / 214 980 | 104 832 / 214 980 |

Every run printed `VERIFY PASS` twice, with 0 bucket overflows and 2 479
credentials; flash-operation counts are within 2 % of the old-fork run.

- **Reads roughly halve** (`check`, `byid`, `miss`), because UBI's cost per
  flash access fell from ~112 µs to 7–13 µs (`app_perf_blob_db/RESULTS.md`).
- **The format's erases moved, not vanished.** v0.1.0's format does not
  pre-erase blank blocks, so `open` drops from 146 s to 37 s and the erases
  happen in `prepare`, inside UBI's allocator on each bucket's first write.
  `open` + `prepare` is unchanged.
- **One bucket fewer:** `prepare` formats 98 instead of 99, because v0.1.0
  keeps one more block for its volume table.
- **`ATOMIC_REPLACE` cuts `fill` by 19 %.** Compaction becomes one
  `ubi_leb_change()` instead of five block erases (master, scratch, bucket,
  scratch, master), about six compactions' worth of erases over this run.
  The I/O counters are byte-identical with and without it.

## 6. What the numbers say

*Rewritten after the large-payload merge. The previous version of this section
is preserved in the pre-merge captures of §8 — it described whole-sector reads
in the present tense long after they had stopped happening, and every figure in
it (711×, 256 KB, 26 214×) was modelled rather than measured.*

**The serialization format is not the cost.** `cbor` is 5 µs against `check`'s
44 µs on `native_sim` — 11 % there, where there is no real flash to wait for. On
hardware it is 950 µs against 14.605 ms: **6.5 %** (§5a, measured). It was the
second-largest term after flash even before the merge, and the merge made flash
cheaper, so its share grew. It is still not the thing to optimize.

**The cost is the lookups, and the transactions inside them.** An access
decision is two map gets, ~7 ms each on the DK (§5b), and a map get is two
`blob_db` calls (`FINDINGS.md` K11). Underneath, ~65 µs of fixed per-transaction
cost accounts for over half the remaining time (N1). Flash *bytes* are no longer
the interesting axis: 13.4 KB moved to answer a question about 402 B is 33×
amplification, down from the 656× this section used to quote.

**Negative lookups are still the worst case in relative terms** — 6.9 KB to
learn that a card does not exist, 300× — but in absolute terms a miss is now
*cheaper* than a hit (7.165 ms against 14.605 ms), because it stops after one
lookup instead of two. The pre-merge framing had it the other way round.

**Writes cost about four store operations, not three.** A person plus its ~2.49
credentials is one read-old, one record write and one index write per card. The
read-old is what makes a replace unindex the cards it drops; without it, a
dropped card kept granting access (`README.md` practice 5). That is the price of
the secondary index that makes R-D possible at all, plus the price of keeping it
truthful, and both are paid on every enrollment.

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

## 8b. Raw capture — nRF5340-DK, 5 000 persons, UBI (the default)

Board 960115021, `N_PERSONS=5000`, `FRESH_START=y`, backend from Kconfig with no
override, partition raw-erased beforehand. UBI's volume-probe lines are elided;
it logs them at `<err>` level.

```
*** Booting Zephyr OS build 4a405846193f ***

persondb 1.0.0 — CBOR person/credential database
[00:02:28.446,258] <inf> persondb: created store: 16 people maps + 1 credential map, max buckets each, 5000 persons planned
open         : 148197 ms
prepare      :    132 ms (100 buckets formatted)
fill         : 3376771 ms  5000 written, 5000/5000 total, 20 commits
verify       :  30851 ms  256 persons, 654 cards
VERIFY PASS
mutate       :  30637 ms  rev 0 -> 1  (0 revoked, 64 assigned)
re-verify    :  31008 ms  256 persons, 662 cards
VERIFY PASS

bench check :   200 ops in 13537000 us ->      14 ops/s    67685 us/op   400 store ops  68803 flash ops   2698789 B  amp 33x
             : 200 granted, 0 denied, 0 unknown, 0 expired
bench byid  :   200 ops in  6828000 us ->      29 ops/s    34140 us/op   200 store ops  34164 flash ops   1351636 B  amp 18x
bench miss  :   200 ops in  6880000 us ->      29 ops/s    34400 us/op   200 store ops  35143 flash ops   1346830 B  amp 292x
bench put   :   200 ops in 162277000 us ->       1 ops/s   811385 us/op   874 store ops  217315 flash ops  15323722 B  amp 178x
bench cbor  :   200 ops in   188000 us ->    1063 ops/s      940 us/op     0 store ops      0 flash ops         0 B  amp 0x

store
  partition   : 8388608 B (8192 KiB)
  maps        : 16 person + 1 credential (bucket count not observable — FINDINGS.md K10)
  persons     : 5000 of 5000   credentials: 12550
  mean entry  : 433 B
  live content: 2169730 B = 25.8 % of the partition
  bucket overflows: 0
  NOTE: physical occupancy (live + uncompacted garbage) is not
        observable through the API — see FINDINGS.md B3.

persondb: done — store at rev 1; rerun to prove persistence
```

Wall clock 63.5 min. Reproduce with:

```
west build -p always -b nrf5340dk/nrf5340/cpuapp app_cbor_persondb \
      -- -DCONFIG_APP_CBOR_PERSONDB_N_PERSONS=5000 \
         -DCONFIG_APP_CBOR_PERSONDB_FRESH_START=y
```

### A note on running this after `app_perf_blob_db`

This app does not enable `CONFIG_BLOB_DB_LARGE_PAYLOADS`, so it reads format
major 1 only. `app_perf_blob_db` does enable it and leaves a major-2 store, which this
app refuses to mount with `-ENOTSUP` — and `FRESH_START` cannot save it, because
mount runs first. Erase the partition before the run; see the "Downgrading"
section of `app_perf_blob_db/RESULTS.md`.

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

## 8c. Raw capture — nRF5340-DK, two-level `kvhash`, 1 000 persons, UBI

Board 960115021, `ade6a3e`, `N_PERSONS=1000`, `FRESH_START=y`, default UBI
backend, partition raw-erased beforehand. UBI's volume-probe lines are elided.

The `ops/s` column below is the **pre-fix, truncated** form — `ui_bench.c`
computed it by integer division, so `put`'s 5.57 printed as `5` and, at 5 000
persons in §8b, 1.23 printed as `1` (a 19 % under-report). Every figure in §5e
is derived from `us/op`, which was always exact. A rerun now prints two
rounded decimals.

```
*** Booting Zephyr OS build 4a405846193f ***

persondb 1.0.0 — CBOR person/credential database
[00:02:26.082,214] <inf> persondb: created store for 1000 persons: people map depth 1, 250 buckets; credential map depth 2, 256 buckets
open         : 145837 ms
prepare      :    130 ms (99 buckets formatted)
fill         : 162071 ms  1000 written, 1000/1000 total, 4 commits
verify       :  16385 ms  256 persons, 602 cards
VERIFY PASS
mutate       :   9157 ms  rev 0 -> 1  (0 revoked, 64 assigned)
re-verify    :  17354 ms  256 persons, 618 cards
VERIFY PASS

bench check :   200 ops in  7208000 us ->      27 ops/s    36040 us/op   400 store ops  35245 flash ops   1266097 B  amp 15x
             : 200 granted, 0 denied, 0 unknown, 0 expired
bench byid  :   200 ops in  2821000 us ->      70 ops/s    14105 us/op   200 store ops  11503 flash ops    886418 B  amp 11x
bench miss  :   200 ops in  4469000 us ->      44 ops/s    22345 us/op   200 store ops  23705 flash ops    372059 B  amp 80x
bench put   :   200 ops in 35937000 us ->       5 ops/s   179685 us/op   864 store ops 111164 flash ops   3527182 B  amp 40x
bench cbor  :   200 ops in   197000 us ->    1015 ops/s      985 us/op     0 store ops      0 flash ops         0 B  amp 0x

store
  partition   : 8388608 B (8192 KiB)
  maps        : 1 person (depth 1, 250 buckets) + 1 credential (depth 2, 256 buckets)
  persons     : 1000 of 1000   credentials: 2479
  mean entry  : 432 B
  live content: 432759 B = 5.1 % of the partition
  bucket overflows: 0
  buckets past near-full: 0

persondb: done — store at rev 1; rerun to prove persistence
```

Wall clock 6.69 min, of which `open` is 36 % and `fill` 40 %. Footprint on this
build is FLASH 86 032 B and RAM 202 064 B — within 1.6 % and 16 B of the
one-level build, so the second level costs essentially nothing in either.

## 8d. Raw capture — nRF5340-DK, two-level `kvhash`, 5 000 persons, UBI

Board 960115021, branch head, `N_PERSONS=5000`, default UBI backend, partition
raw-erased before run 1. UBI's volume-probe lines elided. Run 1 creates and
fills; run 2 is the steady-state run.

### Run 1 — create and fill

```
*** Booting Zephyr OS build 4a405846193f ***

persondb 1.0.0 — CBOR person/credential database
[00:02:24.144,470] <inf> persondb: created store for 5000 persons: people map depth 2, 484 buckets; credential map depth 2, 256 buckets
open         : 143899 ms
prepare      :    107 ms (77 buckets formatted)
fill         : 2733867 ms  5000 written, 5000/5000 total, 20 commits
verify       :  13166 ms  256 persons, 654 cards
VERIFY PASS
mutate       :  52543 ms  rev 0 -> 1  (0 revoked, 64 assigned)
re-verify    :  13529 ms  256 persons, 662 cards
VERIFY PASS

bench check :   200 ops in  6230000 us ->    32.10 ops/s    31150 us/op   400 store ops  28890 flash ops   1552566 B  amp 19x
             : 200 granted, 0 denied, 0 unknown, 0 expired
bench byid  :   200 ops in  3455000 us ->    57.89 ops/s    17275 us/op   200 store ops  14529 flash ops   1106933 B  amp 14x
bench miss  :   200 ops in  2820000 us ->    70.92 ops/s    14100 us/op   200 store ops  14066 flash ops    436251 B  amp 94x
bench put   :   200 ops in 218302000 us ->     0.92 ops/s  1091510 us/op   874 store ops  80009 flash ops  17832777 B  amp 207x
bench cbor  :   200 ops in   194000 us ->  1030.93 ops/s      970 us/op     0 store ops      0 flash ops         0 B  amp 0x

store
  partition   : 8388608 B (8192 KiB)
  maps        : 1 person (depth 2, 484 buckets) + 1 credential (depth 2, 256 buckets)
  persons     : 5000 of 5000   credentials: 12550
  mean entry  : 433 B
  live content: 2169730 B = 25.8 % of the partition
  bucket overflows: 0
  buckets past near-full: 0
  NOTE: physical occupancy (live + uncompacted garbage) is not
        observable through the API — see FINDINGS.md B3.

persondb: done — store at rev 1; rerun to prove persistence
```

### Run 2 — steady state (rev 1 -> 2)

```
*** Booting Zephyr OS build 4a405846193f ***

persondb 1.0.0 — CBOR person/credential database
open         :    935 ms
prepare      :     23 ms (0 buckets formatted)
fill         : already complete (5000 persons)
verify       :  13729 ms  256 persons, 662 cards
VERIFY PASS
mutate       : 103869 ms  rev 1 -> 2  (64 revoked, 64 assigned)
re-verify    :  13955 ms  256 persons, 662 cards
VERIFY PASS

bench check :   200 ops in  6588000 us ->    30.36 ops/s    32940 us/op   400 store ops  30846 flash ops   1575269 B  amp 19x
             : 200 granted, 0 denied, 0 unknown, 0 expired
bench byid  :   200 ops in  3813000 us ->    52.45 ops/s    19065 us/op   200 store ops  16526 flash ops   1130657 B  amp 15x
bench miss  :   200 ops in  2869000 us ->    69.71 ops/s    14345 us/op   200 store ops  14353 flash ops    439833 B  amp 95x
bench put   :   200 ops in 258847000 us ->     0.77 ops/s  1294235 us/op   873 store ops  67933 flash ops  21157991 B  amp 246x
bench cbor  :   200 ops in   194000 us ->  1030.93 ops/s      970 us/op     0 store ops      0 flash ops         0 B  amp 0x

store
  partition   : 8388608 B (8192 KiB)
  maps        : 1 person (depth 2, 484 buckets) + 1 credential (depth 2, 256 buckets)
  persons     : 5000 of 5000   credentials: 12550
  mean entry  : 433 B
  live content: 2169730 B = 25.8 % of the partition
  bucket overflows: 0
  buckets past near-full: 0
  NOTE: physical occupancy (live + uncompacted garbage) is not
        observable through the API — see FINDINGS.md B3.

persondb: done — store at rev 2; rerun to prove persistence
```

## 8e. Raw capture — nRF5340-DK, two-level `kvhash`, 10 000 persons, UBI

Same board and build settings at `N_PERSONS=10000`. **This is the scale the
one-level container could not reach** — it stopped at person 9 670 with
`-ENOSPC` (`FINDINGS.md` B13).

### Run 1 — create and fill

```
*** Booting Zephyr OS build 4a405846193f ***

persondb 1.0.0 — CBOR person/credential database
[00:02:24.691,131] <inf> persondb: created store for 10000 persons: people map depth 2, 961 buckets; credential map depth 2, 256 buckets
open         : 144445 ms
prepare      :     97 ms (68 buckets formatted)
fill         : 10073208 ms  10000 written, 10000/10000 total, 40 commits
verify       :  11588 ms  256 persons, 660 cards
VERIFY PASS
mutate       :  98134 ms  rev 0 -> 1  (0 revoked, 64 assigned)
re-verify    :  11818 ms  256 persons, 664 cards
VERIFY PASS

bench check :   200 ops in  5357000 us ->    37.33 ops/s    26785 us/op   400 store ops  22973 flash ops   1718124 B  amp 21x
             : 198 granted, 0 denied, 0 unknown, 2 expired
bench byid  :   200 ops in  2954000 us ->    67.70 ops/s    14770 us/op   200 store ops  11644 flash ops   1082992 B  amp 14x
bench miss  :   200 ops in  2509000 us ->    79.71 ops/s    12545 us/op   200 store ops  11379 flash ops    626210 B  amp 136x
bench put   :   200 ops in 512880000 us ->     0.39 ops/s  2564400 us/op   862 store ops  65353 flash ops  38944186 B  amp 453x
bench cbor  :   200 ops in   194000 us ->  1030.93 ops/s      970 us/op     0 store ops      0 flash ops         0 B  amp 0x

store
  partition   : 8388608 B (8192 KiB)
  maps        : 1 person (depth 2, 961 buckets) + 1 credential (depth 2, 256 buckets)
  persons     : 10000 of 10000   credentials: 24932
  mean entry  : 433 B
  live content: 4336158 B = 51.6 % of the partition
  bucket overflows: 0
  buckets past near-full: 0
  NOTE: physical occupancy (live + uncompacted garbage) is not
        observable through the API — see FINDINGS.md B3.

persondb: done — store at rev 1; rerun to prove persistence
```

### Run 2 — steady state (rev 1 -> 2)

```
*** Booting Zephyr OS build 4a405846193f ***

persondb 1.0.0 — CBOR person/credential database
open         :    789 ms
prepare      :     23 ms (0 buckets formatted)
fill         : already complete (10000 persons)
verify       :  12482 ms  256 persons, 664 cards
VERIFY PASS
mutate       : 232101 ms  rev 1 -> 2  (64 revoked, 64 assigned)
re-verify    :  12485 ms  256 persons, 664 cards
VERIFY PASS

bench check :   200 ops in  5470000 us ->    36.56 ops/s    27350 us/op   400 store ops  23580 flash ops   1725200 B  amp 21x
             : 198 granted, 0 denied, 0 unknown, 2 expired
bench byid  :   200 ops in  2867000 us ->    69.76 ops/s    14335 us/op   200 store ops  11137 flash ops   1076953 B  amp 14x
bench miss  :   200 ops in  2729000 us ->    73.29 ops/s    13645 us/op   200 store ops  12654 flash ops    641602 B  amp 139x
bench put   :   200 ops in 511685000 us ->     0.39 ops/s  2558425 us/op   865 store ops  67037 flash ops  38920838 B  amp 452x
bench cbor  :   200 ops in   195000 us ->  1025.64 ops/s      975 us/op     0 store ops      0 flash ops         0 B  amp 0x

store
  partition   : 8388608 B (8192 KiB)
  maps        : 1 person (depth 2, 961 buckets) + 1 credential (depth 2, 256 buckets)
  persons     : 10000 of 10000   credentials: 24932
  mean entry  : 433 B
  live content: 4336158 B = 51.6 % of the partition
  bucket overflows: 0
  buckets past near-full: 0
  NOTE: physical occupancy (live + uncompacted garbage) is not
        observable through the API — see FINDINGS.md B3.

persondb: done — store at rev 2; rerun to prove persistence
```

Build for either scale with:

```
west build -p always -b nrf5340dk/nrf5340/cpuapp app_cbor_persondb \
      -- -DCONFIG_APP_CBOR_PERSONDB_N_PERSONS=10000
```

Erase the partition raw before run 1; run 2 is the same image reflashed with the
store left in place.

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

## 10. Scaling sweep — how each operation moves with the number of credentials

Every hardware number above is taken at one scale per run, and §5f shows the
answer moves with scale — not monotonically. `tools/perf_sweep.py` makes the
curve a single command: it rebuilds this app at a list of
`CONFIG_APP_CBOR_PERSONDB_N_PERSONS` values and nothing else, runs each one
on `native_sim` three times on the same flash image (the fill run, then two
steady-state reruns), and parses the console into JSON. It also parses DK
UART captures and the raw captures in §8, so board runs land in the same
format.

```
python3 tools/perf_sweep.py run -o sweep.json            # ~15 min on a laptop
python3 tools/perf_sweep.py results -o dk.json           # §8, parsed
python3 tools/perf_sweep.py report sweep.json dk.json    # markdown
```

Taken at `501bd3f` (default UBI backend, 200 samples per bench). Each figure
is the median over the three runs at that scale. *DK ms* applies the read-path
model to the counters — 75 µs per flash transaction (`flash_area`'s 65 µs
plus zephyr-ubi v0.1.0's 7–13 µs, `app_perf_blob_db/RESULTS.md`) + 0.63 µs/B
+ 0.95 ms of CBOR where a record is decoded. **`put` is not projected**; see
below.

| persons | credentials | live | people / cred depth | check txns | check B | **check DK ms** | byid DK ms | miss DK ms | put txns | put B | put erases | host fill ms/person | status |
|--:|--:|--:|:-:|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|
| 250 | 648 | 1.3 % | 1 / 1 | 115.3 | 4 930 | 12.70 | 9.69 | 4.19 | 365.6 | 16 196 | 0.010 | 0.10 | ok |
| 500 | 1 238 | 2.5 % | 1 / 2 | 154.8 | 5 125 | 15.79 | 6.71 | 8.85 | 497.2 | 17 009 | 0.015 | 0.07 | ok |
| 1 000 | 2 479 | 5.1 % | 1 / 2 | 215.7 | 6 805 | 21.41 | 9.46 | 11.61 | 669.3 | 18 996 | 0.000 | 0.09 | ok |
| 2 000 | 4 979 | 10.3 % | 2 / 2 | 354.9 | 8 206 | 32.74 | 14.21 | 18.20 | 1 071.0 | 35 915 | 0.070 | 0.11 | ok |
| 4 000 | 10 017 | 20.6 % | 2 / 2 | 155.3 | 7 448 | 17.29 | 10.97 | 6.25 | 438.7 | 42 981 | 0.135 | 0.21 | ok |
| 8 000 | 19 969 | 41.3 % | 2 / 2 | 121.3 | 8 222 | 15.23 | 8.73 | 6.05 | 327.2 | 67 622 | 0.255 | 0.36 | ok |
| 10 000 | 24 932 | 51.6 % | 2 / 2 | 117.9 | 8 626 | 15.23 | 8.90 | 6.27 | 332.3 | 90 131 | 0.365 | 0.45 | ok |
| 12 000 | 29 942 | 62.0 % | 2 / 2 | 117.8 | 9 141 | 15.54 | 8.84 | 6.70 | 337.6 | 135 153 | 0.590 | 0.59 | ok |
| 14 000 | — | — | — | — | — | — | — | — | — | — | — | — | fill stopped at 12 718 |
| 16 000 | — | — | — | — | — | — | — | — | — | — | — | — | fill stopped at 14 107 |


The same data, with the DK points beside it, is the `persondb-performance`
page of the kvdb_book repository.

### Counters transfer for reads, and at 1 000 persons for writes too

Each §8 two-level capture against the sweep run of the same kind at the same
scale (sweep firmware is newer than the board's):

| run | `check` txns, DK / sim | `byid` | `miss` | `put` txns | `put` bytes, DK / sim |
|---|--:|--:|--:|--:|--:|
| 1 000, fill | 176.2 / 172.1 (0.98×) | 0.97× | 0.96× | 0.98× | 17 636 / 17 496 B (0.99×) |
| 10 000, fill | 114.9 / 123.8 (1.08×) | 1.08× | 1.07× | 1.03× | 194 721 / 90 131 B (**0.46×**) |
| 10 000, steady | 117.9 / 117.9 (1.00×) | 1.08× | 0.91× | 0.99× | 194 604 / 89 266 B (**0.46×**) |

The old-fork model (§5c) applied to each capture's own counters lands within
4 % on every read at every scale; the v0.1.0 model applied to §8c's counters
predicts §5g's measured 17.13 ms `check` at 18.16 ms (+6 %), `byid` +1 %,
`miss` +7 %.

### Reads peak at a few thousand persons; writes keep growing

- **`check` is worst at 2 000 persons** — 355 transactions, a projected
  32.7 ms — and falls to 117–121 from 8 000 up. kvhash sizes both maps from
  the declared population, so a larger store gets more, shallower buckets
  (§5f's "reads get faster as the dataset grows", seen here at ten scales).
  It is also the noisiest point: 262–428 transactions across its three runs.
- **`put` bytes grow 8×** (16 KB → 132 KB) from 250 to 12 000 persons, and its
  counted sector erases from 0.01 to 0.59 per operation.

### Writes cannot be projected from these counters

At 10 000 persons `put` moves 2.2× more bytes on the old-fork DK than on
`native_sim` at `501bd3f`, and §5f's wall clock implies **2.23** sector
erases per `put` where the sweep counts **0.37**. The read model plus the
counted erases predicts 583 ms for a `put` measured at 2 564 ms (−77 %). Whether
the gap is the platform, the old UBI fork, or `blob_db` changes since `ade6a3e`
is open: settling it needs a DK run of the current firmware at 10 000 persons.
Until then the write path is reported as counters and board measurements only.
*§10a has that run: on current firmware the board's `put` counters match
`native_sim`'s, and `put` time follows read cost + bytes programmed + erases.*

### Where the store stops

| declared persons | fill stops at | credentials stored |
|--:|--:|--:|
| 12 000 | completes — 62.0 % live | 29 942 |
| 14 000 | person 12 718, `-ENOSPC` | 31 648 |
| 16 000 | person 14 107, `-ENOSPC` | 35 110 |

A bucket fills (`persondb: bucket overflow`, `FINDINGS.md` K2) and the insert
returns `-ENOSPC`. The ceiling moves with the declared size because the
declared population sets the bucket count — so "how many credentials fit" has
no single answer, and the benchmark constant of 8 000 sits well below every
ceiling measured here.

## 10a. The sweep on the nRF5340-DK — six scales, two boards

§10 projects every scale from `native_sim` counters, and until now the board
had confirmed one point: §5g, 1 000 persons at `99a2ca6`. This section measures
the same sweep on the board — 250, 1 000, 2 000, 4 000, 8 000 and 12 000
persons, each a fill run and two steady-state reruns as `perf_sweep.py run`
does — and checks §10's projection against it, op by op.

### Setup

- **Boards**: two nRF5340-DK PCA10095, J-Link SN 960115021 ("board 1", the
  board of §5g and §8c) and 1050091053 ("board 2"); store on the on-board
  MX25R6435F, whole chip as `storage_partition`, UBI on zephyr-ubi `v0.1.0`,
  two-level `kvhash`. Board 1 ran 250, 1 000 and 12 000 persons; board 2 ran
  1 000, 2 000, 4 000 and 8 000. 1 000 ran on both as the control.
- **Firmware**: `501bd3f` (main) plus one DK-local line, not on any branch:
  a `printk` at UBI attach in `blob_db_store_ubi.c` printing
  `ubi_device_get_info()` — `image_seq`, PEB health, min/max/total erase count
  (the `ubi-ec:` line in every log). Built as `0c441f0`; nothing else differs,
  and `perf_sweep.py` (host-side) is the only other commit on top of `501bd3f`.
  Zephyr `4a405846193f`, SDK 1.0.1. QSPI in its default low-power mode (no #49
  patch) — the mode the read model was fitted in.
- **Scale**: `-DCONFIG_APP_CBOR_PERSONDB_N_PERSONS=<n>
  -DCONFIG_APP_CBOR_PERSONDB_BENCH_SAMPLES=200`, the switches `perf_sweep.py`
  uses, so board and sweep build the same benchmark. No code change was needed.
- **Runs per scale**: (1) program a `FRESH_START=y` image — the fill run;
  (2) program the same `N` without `FRESH_START`; (3) reset. The console is
  captured from before each program/reset, one log per boot.
- **Clean start keeps UBI.** `FRESH_START` is `blob_db_erase_all()`: bucket
  magics invalidated in place and a new master, no partition erase and no UBI
  format. Each board's UBI device — and its erase counters — carried through
  every run: `image_seq` never changed (board 1 `0x27534cfe`, board 2
  `0x6d0b4b7a`), every PEB stayed healthy, 0 bad, 0 corrupt. This is a
  deliberate difference from §5g, which raw-erased the partition first: wear
  history is part of the result, and it changes `open` (below).
- **native_sim**: `perf_sweep.py run --persons 250,1000,2000,4000,8000,12000`
  on the same tree, in the Zephyr CI container. Every point reproduces §10's
  table to the last digit.

Files: `results/dk-scale/` — per scale and board,
`b<board>-n<persons>-<commit>-<UTC start>-run{1,2,3}.log` (raw UART, ANSI and
CR as captured) and `…-<UTC start>.json` (`perf_sweep.py parse` of the three
logs, plus a `dk` block: board, commit, local changes, per-boot status, wall
time and `ubi-ec` line); and `native_sim-0c441f0-<UTC>.json`, the sweep. The
`log` paths inside the JSON are absolute paths on the capture host.

### Reads: within the 15 % warn mark everywhere, within 5 % only for `miss` from 4 000 up

Projected = the read model on the **`native_sim`** counters (75 µs per
transaction + 0.63 µs/B + 0.95 ms CBOR for `check`/`byid`), exactly as §10
computes it. Board = measured. Median over the three runs, min–max beside it;
the spread is the steady-state drift of §10, not noise — it is the same on both
boards and on `native_sim`. Pass ≤ 5 %, warn ≤ 15 %.

**`check`**

| persons | credentials | board | board ms, median (min–max) | projected ms, median (min–max) | error |
|--:|--:|:-:|--:|--:|--:|
| 250 | 648 | 1 | 12.01 (10.77–14.55) | 12.70 (11.22–15.45) | +5.8 % warn |
| 1 000 | 2 479 | 1 | 19.75 (16.38–22.90) | 21.41 (17.81–25.02) | +8.4 % warn |
| 1 000 | 2 479 | 2 | 19.43 (16.12–22.55) | 21.41 (17.81–25.02) | +10.2 % warn |
| 2 000 | 4 979 | 2 | 29.28 (22.64–34.37) | 32.74 (25.09–38.80) | +11.8 % warn |
| 4 000 | 10 017 | 2 | 15.92 (14.13–16.10) | 17.29 (15.34–17.78) | +8.6 % warn |
| 8 000 | 19 969 | 2 | 14.23 (13.58–14.27) | 15.23 (14.47–15.36) | +7.0 % warn |
| 12 000 | 29 942 | 1 | 14.79 (14.71–14.80) | 15.54 (15.37–15.78) | +5.1 % warn |

**`byid`**

| persons | credentials | board | board ms, median (min–max) | projected ms, median (min–max) | error |
|--:|--:|:-:|--:|--:|--:|
| 250 | 648 | 1 | 9.04 (5.57–10.38) | 9.69 (5.73–11.12) | +7.3 % warn |
| 1 000 | 2 479 | 1 | 8.89 (7.48–10.20) | 9.46 (7.93–10.98) | +6.4 % warn |
| 1 000 | 2 479 | 2 | 8.74 (7.36–10.04) | 9.46 (7.93–10.98) | +8.1 % warn |
| 2 000 | 4 979 | 2 | 12.91 (12.32–18.12) | 14.21 (13.53–20.31) | +10.1 % warn |
| 4 000 | 10 017 | 2 | 10.03 (8.38–10.32) | 10.97 (9.03–11.23) | +9.4 % warn |
| 8 000 | 19 969 | 2 | 8.19 (8.07–8.60) | 8.73 (8.61–9.26) | +6.6 % warn |
| 12 000 | 29 942 | 1 | 8.37 (8.27–8.52) | 8.84 (8.70–9.04) | +5.6 % warn |

**`miss`**

| persons | credentials | board | board ms, median (min–max) | projected ms, median (min–max) | error |
|--:|--:|:-:|--:|--:|--:|
| 250 | 648 | 1 | 4.48 (3.33–5.46) | 4.19 (2.92–5.28) | -6.5 % warn |
| 1 000 | 2 479 | 1 | 10.96 (9.21–12.65) | 11.61 (9.70–13.51) | +5.9 % warn |
| 1 000 | 2 479 | 2 | 10.79 (9.07–12.44) | 11.61 (9.70–13.51) | +7.6 % warn |
| 2 000 | 4 979 | 2 | 16.55 (10.86–16.61) | 18.20 (11.67–18.36) | +9.9 % warn |
| 4 000 | 10 017 | 2 | 6.14 (5.97–6.29) | 6.25 (5.99–6.56) | +1.7 % pass |
| 8 000 | 19 969 | 2 | 6.06 (5.90–6.46) | 6.05 (5.80–6.43) | -0.2 % pass |
| 12 000 | 29 942 | 1 | 6.86 (6.84–6.87) | 6.70 (6.63–6.73) | -2.3 % pass |

**0 of 21 fail; 3 pass, 18 warn.** The projection is high at every scale for
`check` and `byid` (+5.1 to +11.8 %) and peaks where §10 said it would, at
2 000 persons (4 979 credentials). The model is the cause, not the counters:
applied to the **board's own** counters it is off by the same amount (+4.7 to
+11.8 % on `check`), because the counters are the same:

| persons | board | worst read-op Δ txns | worst read-op Δ bytes | `put` txns, DK / sim | `put` erases/op, DK / sim |
|--:|:-:|--:|--:|--:|--:|
| 250 | 1 | 2.5 % | 0.7 % | 368.2 / 365.6 | 0.010 / 0.010 |
| 1 000 | 1 | 0.0 % | 0.0 % | 669.3 / 669.3 | 0.000 / 0.000 |
| 1 000 | 2 | 0.0 % | 0.0 % | 669.3 / 669.3 | 0.000 / 0.000 |
| 2 000 | 2 | 0.1 % | 0.0 % | 1067.7 / 1071.0 | 0.070 / 0.070 |
| 4 000 | 2 | 0.8 % | 0.3 % | 435.2 / 438.7 | 0.135 / 0.135 |
| 8 000 | 2 | 0.6 % | 0.2 % | 327.9 / 327.2 | 0.250 / 0.255 |
| 12 000 | 1 | 0.6 % | 0.2 % | 337.1 / 337.6 | 0.580 / 0.590 |

Every read op on the board does the same flash traffic as on `native_sim`
within 2.5 %, at every scale; `put` within 1.3 % in bytes and 2 % in erases. The
control repeats exactly: boards 1 and 2 report byte-identical counters at
1 000 persons and times within 1.5–4.4 % of each other (board 1 slower).

**The 1 000-person control does not repeat §5g, and should not.** §5g is a fill
run at `99a2ca6` (2026-10-01), before the flash-buffer alignment fix #55
(2026-10-14). Board 1's fill run now reads `check` 16.38 ms (§5g 17.13),
`byid` 7.48 (7.98), `miss` 9.21 (9.44) — 2–6 % faster on the same board. The
model's constants were set against that older firmware, which is why it now
reads high. Refit on all 63 board read points with CBOR held at the measured
0.95 ms: **66.2 µs per transaction and 0.645 µs/B**, which brings `check` and
`byid` within ±1.8 % at every scale. 66 µs is `flash_area`'s own 65 µs:
zephyr-ubi's 7–13 µs per access does not show on this firmware. `miss` stays
under-predicted by 4–12 % after the refit, i.e. it carries a fixed cost the
model has no term for (it decodes nothing, so the CBOR term is not it). These
refit constants are in-sample; they are a proposal for §10's model, not a
validation of it.

### Writes: read cost + bytes programmed + erases, within 8.4 %

**`put` and `cbor`** (no projection; per-op counters from the board's `io` lines, median of the three runs)

| persons | board | `put` ms, median (min–max) | reads | B read | B written | erases | `cbor` ms |
|--:|:-:|--:|--:|--:|--:|--:|--:|
| 250 | 1 | 73.01 (59.69–83.27) | 365 | 13 466 | 2 113 | 0.010 | 0.943 |
| 1 000 | 1 | 89.63 (80.23–112.58) | 666 | 16 587 | 2 414 | 0.000 | 0.972 |
| 1 000 | 2 | 86.92 (77.70–107.91) | 666 | 16 587 | 2 414 | 0.000 | 0.956 |
| 2 000 | 2 | 215.97 (185.45–227.45) | 1064 | 26 930 | 4 955 | 0.070 | 0.958 |
| 4 000 | 2 | 268.41 (253.79–292.17) | 432 | 25 679 | 8 503 | 0.135 | 0.944 |
| 8 000 | 2 | 470.66 (441.49–471.63) | 324 | 34 334 | 16 147 | 0.250 | 0.950 |
| 12 000 | 1 | 1085.57 (1083.75–1086.37) | 333 | 58 416 | 37 028 | 0.580 | 0.954 |

The read model explains a median 46 % of `put` (56 % at 250 persons, 10 % at
12 000). Fits over all 21 runs, with the read side priced by the model
(75 µs × reads + 0.63 µs/B × bytes read; the bench line's byte count includes
erased bytes, so the write side is split out):

| model for `put` | constants | R² | median error | max error |
|---|---|--:|--:|--:|
| read cost only | — | −0.64 | 57.5 % | 94.3 % |
| + k · erases | k = 1.75 s | 0.997 | 9.8 % | 46.2 % |
| + b · bytes written + k · erases, erase pinned at the measured 1.11 s | b = 10.0 µs/B | — | 4.5 % | 11.3 % |
| **+ b · bytes written + k · erases** | **b = 11.3 µs/B, k = 1.02 s** | **0.999** | **2.9 %** | **8.4 %** |

Erases alone are not enough: at 250–1 000 persons `put` does almost none
(0–0.02 per op) and still costs 1.5–2× its read cost. The missing term is the
bytes programmed — 2.1 KB per `put` at 250 persons, 37 KB at 12 000 — at about
11 µs per byte in this QSPI mode. A per-program (per-write-call) term does not
help: fitted alongside, it comes out negative. The fitted erase cost, 1.02 s,
sits next to the 1.11 s per 64 KB block `app_perf_blob_db` measured directly.

**Out of sample.** Fitted on the 15 runs at ≤ 4 000 persons only
(b = 11.7 µs/B, k = 0.87 s), the model predicts 8 000 persons at −3.8 to −4.1 %
and 12 000 at −7.6 to −7.9 % — low, and more so as erases grow, so the erase
term is worth more than the small scales can show. §10's open question —
platform, old fork or `blob_db` changes — is answered for current firmware:
the board's `put` counters match `native_sim`'s (0.580 vs 0.590 erases per
`put` at 12 000), so the 2.2× byte gap §10 saw is not the platform — it was
the old fork or `blob_db` changes since `ade6a3e` — and the write path can be
projected from `native_sim` counters with this model.

### Fill, `open` and wear

**Fill and UBI wear** (fill run; `-ENOSPC`: none at any scale)

| persons | board | open s | prepare s (buckets) | fill s | ms / person | live | UBI EC total, first → last attach | EC max, first → last |
|--:|:-:|--:|--:|--:|--:|--:|--:|--:|
| 250 | 1 | 107.5 | 122.9 (114) | 17.5 | 70 | 1.3 % | 8 851 → 9 093 | 74 → 75 |
| 1 000 | 1 | 141.5 | 106.3 (98) | 70.2 | 70 | 5.1 % | 9 098 → 9 345 | 75 → 77 |
| 12 000 | 1 | 168.5 | 69.2 (64) | 4564.7 | 380 | 62.0 % | 9 348 → 11 966 | 77 → 99 |
| 1 000 | 2 | 140.7 | 103.7 (98) | 70.0 | 70 | 5.1 % | 1 659 → 1 906 | 14 → 16 |
| 2 000 | 2 | 143.9 | 86.7 (82) | 173.2 | 87 | 10.3 % | 1 909 → 2 195 | 16 → 19 |
| 4 000 | 2 | 144.0 | 82.3 (78) | 609.0 | 152 | 20.6 % | 2 218 → 2 713 | 19 → 23 |
| 8 000 | 2 | 153.8 | 73.9 (70) | 1842.9 | 230 | 41.3 % | 2 752 → 3 878 | 23 → 33 |

No scale hit `-ENOSPC`; every run printed `VERIFY PASS` twice with 0 bucket
overflows. Fill cost per person is flat at 70 ms to 1 000 persons and
grows to 380 ms at 12 000 (76 min for the fill), tracking `put`'s erases.
`open` is 108–169 s against §5g's 37 s: the store it opens is the previous
scale's, kept, and `FRESH_START`'s `erase_all` has to invalidate it — the cost of
keeping UBI's wear history rather than raw-erasing the chip. The whole sweep
added 3 115 erases to board 1 (max per PEB 74 → 99) and 2 219 to board 2
(14 → 33).

### What is validated, and what is still projected

- **Validated on hardware, 250 to 12 000 persons**: the I/O counters (within
  2.5 % of `native_sim` on every op, both boards); the read model to within
  15 % (`check`/`byid` +5 to +12 %, always high; `miss` −6.5 to +9.9 %); the
  ranking of scales, including the 2 000-person peak; `put` time as read cost +
  ~11 µs per byte programmed + ~1 s per erase, out of sample to 8 000 persons
  within 4 %.
- **Still projected**: anything above 12 000 persons (§10's `-ENOSPC` ceilings
  at 14 000 and 16 000 are `native_sim` only); the read constants'
  refit (66.2 µs, 0.645 µs/B) until a second data set confirms it; `put` where
  erases exceed ~0.6 per op, where the small-scale fit under-predicts; the
  QSPI high-performance mode (#49), in which an erase costs about half
  (`app_perf_blob_db/RESULTS.md`) and none of these constants apply; a chip
  blank at the start rather than carrying wear.

### Reproducing

```
west build -p always -b nrf5340dk/nrf5340/cpuapp app_cbor_persondb -- \
    -DCONFIG_APP_CBOR_PERSONDB_N_PERSONS=<n> \
    -DCONFIG_APP_CBOR_PERSONDB_BENCH_SAMPLES=200 \
    -DCONFIG_APP_CBOR_PERSONDB_FRESH_START=y          # run 1 only
python3 tools/perf_sweep.py parse run1.log run2.log run3.log -o point.json
```

Capture the console before programming and let `nrfutil device program
--options reset=RESET_SYSTEM` start each run (§8b). Run 3 is `nrfutil device
reset` after run 2 prints `persondb: done`.

## 10b. nRF52840-DK — 8 000 persons, against the nRF5340-DK on the same build

The nRF52840-DK (PCA10056) carries the same MX25R6435F on QSPI as the
nRF5340-DK, so the store, its geometry and every flash counter in this file
carry over unchanged. What differs is the CPU: a Cortex-M4F at 64 MHz instead
of the nRF5340's Cortex-M33 at 64 MHz, and half the RAM (256 KB). This section
runs the 8 000-person benchmark on both boards from one commit, so the only
difference is the SoC.

### Setup

- **Boards**: nRF52840-DK PCA10056, J-Link SN 1050295749 ("board 3", new);
  nRF5340-DK PCA10095, SN 1050091053 (board 2 of §10a) as the control.
  Store on the on-board MX25R6435F, whole chip, UBI on zephyr-ubi `v0.1.0`,
  two-level `kvhash`, default `app_cbor_persondb` configuration
  (8 000 persons, 200 bench samples).
- **Firmware**: `f2a2b94` (main) plus the DK-local `ubi-ec:` `printk` of §10a.
  The board files are `boards/nrf52840dk_nrf52840.{overlay,conf}`, the
  nRF5340 ones applied to the nRF52840. Zephyr `4a405846193f` plus the
  `nrf_qspi_nor` power-mode patch from #49, SDK 1.0.1.
- **QSPI in high-performance mode on both boards** (#49), because the
  low-power mode corrupts reads on this flash part (#44). These times are
  therefore not comparable with §10a, which ran in low-power mode; an erase
  costs about half here.
- **Starting state differs, on purpose**: board 3's chip was blank (its first
  UBI attach formatted it); board 2 kept its UBI device and wear from §10a
  (`image_seq` `0x6d0b4b7a`, max erase count 87) and started from
  `FRESH_START`. That only changes `open` on run 1, which is left out of the
  comparison.
- **Runs**: run 1 creates and fills the store, then mutates and benches;
  run 2 is the same image without `FRESH_START`, after a reboot.
- **Footprint on the nRF52840**: 106 120 B flash (10.1 %), 215 108 B RAM
  (**82.1 %**). The 64 KB sector buffer is most of it; RAM, not flash, is
  this board's limit.

Files: `results/dk-nrf52840/` — `<soc>-b<board>-n8000-f2a2b94-run{1,2}.log`,
raw UART as captured.

### Results

Every flash counter (operations and bytes, every bench line) is
**byte-identical on the two boards**, in both runs. The differences below are
time only.

| run 1 | nRF5340 | nRF52840 | Δ |
|---|--:|--:|--:|
| `prepare` (70 bucket formats) | 36.4 s | 39.4 s | +8.4 % |
| `fill`, 8 000 persons | 1 210.5 s | 1 218.8 s | +0.7 % |
| `mutate` (64 assigned) | 15.52 s | 15.71 s | +1.2 % |
| `check` ms/op | 14.60 | 14.76 | +1.1 % |
| `byid` ms/op | 8.80 | 9.01 | +2.5 % |
| `miss` ms/op | 6.23 | 6.34 | +1.7 % |
| `put` ms/op | 272.4 | 276.2 | +1.4 % |
| `cbor` ms/op (no flash) | 0.948 | 1.148 | **+21.1 %** |

| run 2 (after reboot) | nRF5340 | nRF52840 | Δ |
|---|--:|--:|--:|
| `open` | 4.27 s | 4.62 s | +8.4 % |
| `mutate` (64 revoked, 64 assigned) | 28.48 s | 28.75 s | +1.0 % |
| `check` ms/op | 14.36 | 14.75 | +2.7 % |
| `byid` ms/op | 8.24 | 8.57 | +3.9 % |
| `miss` ms/op | 6.51 | 6.73 | +3.3 % |
| `put` ms/op | 253.5 | 256.4 | +1.2 % |
| `cbor` ms/op (no flash) | 0.947 | 1.148 | **+21.2 %** |

Both runs pass on both boards: 8 000 of 8 000 persons in 32 commits, no
`<err>`/`<wrn>`, and run 2 finds the store complete and moves it from rev 1 to
rev 2. Board 3's UBI device after run 2: `image_seq` `0xae640eb4`, 128 PEBs
healthy, erase count max 10, total 1 067.

### What to take from this

- **Pure CPU work is 21 % slower on the nRF52840** at the same clock: `cbor`
  is the only line with no flash in it.
- **Everything that touches the store moves 1–4 %**, because flash time
  dominates and the flash is the same part. Reads move most (`byid`, `miss`
  +2–4 %), `fill` and `put` least (+0.7–1.4 %), in proportion to their CPU
  share.
- **The flash side of the models carries over unchanged**: same counters,
  same per-byte and per-erase costs. Only the CPU share of the per-transaction
  term differs; refitting it on this board has not been done.
- **Projection, not measured here**: work that is mostly CPU — software crypto
  in particular — should cost about 1.2× on the nRF52840, so a protection mode
  that adds ×1.7–×2.5 to reads on the nRF5340 would add proportionally more
  here.

### Reproducing

```
west build -p always -b nrf52840dk/nrf52840 app_cbor_persondb \
    -- -DCONFIG_APP_CBOR_PERSONDB_FRESH_START=y      # run 1 only
```

High-performance mode needs #49's driver patch and
`mxicy,mx25r-power-mode = "high-performance";` on `&mx25r64`, as for the
nRF5340-DK. An nRF52840-DK that arrives with readback protection on must be
unlocked first (`nrfutil device recover`, which erases its internal flash but
not the MX25R).
