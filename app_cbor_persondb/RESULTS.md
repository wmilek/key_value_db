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
benchmark, 200 samples, steady state. **9 000 persons, not 10 000** — the
headline scale no longer completes (below).

| Phase | µs/op | store ops/op | flash ops/op | flash bytes/op | **amplification** | vs 16 shards |
|---|--:|--:|--:|--:|--:|--:|
| `check` — card → person → permission (**R-D**) | **181** | 2 | 220 | 37.5 KB | **94×** | **4.1× slower** |
| `byid` — person id → record | 94 | 1 | 104 | 19.5 KB | 51× | 4.1× |
| `miss` — unknown card | 88 | 1 | 116 | 18.0 KB | 782× | 4.0× |
| `put` — rewrite a record + its index entries | 939 | 4.5 | 737 | 126 KB | 291× | 0.83× |
| `cbor` — encode + decode, no flash | **5** | 0 | 0 | 0 | — | 1.0× |

Whole-run phases, same build:

| Phase | | |
|---|--:|---|
| `open` | 0 ms | one registry read + one superblock read |
| `prepare` | 0 ms | 115 blocks formatted |
| `fill` | 13 403 ms | 9 000 written, 36 commits |
| `verify` | 77 ms | 256 persons, 605 cards — `VERIFY PASS` |
| `mutate` | 55 ms | rev 0 → 1, 64 assigned |
| `re-verify` | 77 ms | 256 persons, 609 cards — `VERIFY PASS` |
| store | | 3 900 851 B live = **46.5 %** of the partition, **0 bucket overflows** |

**The read path costs 4.1× what the sharded build cost, and every byte of it is
the directory.** `check` moves 37.5 KB where the sixteen-shard build moved
13.4 KB, on the same two store operations — the map got wider, not busier. A
`kvhash` get reads the whole bucket directory before it reads a bucket (K11);
one map means 2 047 buckets in that directory, so the read is 16 384 B whatever
the app is looking for. `miss` shows it undisguised: 18.0 KB of flash to
discover that a 23-byte key is not there, an amplification of 782×.

`put` is the one row that improved (0.83×), and not by any virtue of the
layout: writes were already dominated by rewriting a whole bucket and its
directory (K4, K5), and there are now two directories to keep instead of
seventeen.

**The headline scale does not complete, and the store's maximum is 9 670
persons.** Measured: 9 670 completes fill, verify, mutate, re-verify and the
benchmark at 4 192 710 B live = **49.9 %** of the partition with zero bucket
overflows; person 9 671 fails with `-ENOSPC`, at the same index whatever the
configured scale (a 20 000-person build stops there too).

The app reports it as a K2 bucket overflow and that is wrong — the fullest
bucket is 4 621 B of a 16 384 B ceiling, 28 %. What is exhausted is the set of
places a 16 384 B blob can go: `kvhash`'s bucket directory is rewritten whole on
every first insert into a fresh bucket (K5), and once the store is half live no
65 488 B erase block has 16 398 B contiguous free. A 1 KB person bucket still
fits; the directory does not. `blob_db` is behaving to contract — the oversized
blob is L2's (**K13**). The sixteen-shard build finished the same 10 000
persons at 51.6 % because its largest blob was 4 096 B.

**What these numbers are for.** They are worse across the board and that is the
result being reported, not a problem with the run. §4's figures were taken with
sixteen person maps hiding a 4 KB payload cap; these were taken with the app
built the way the stack intends. The difference between the two tables is what
the workaround was worth — and, per `DESIGN.md` §1, what it was concealing:
**K12** and **K13** are both reachable only from this side of it.

**Comparability caveat.** §4 was taken at 10 000 persons and this table at
9 000, because 10 000 no longer completes. The 10 % smaller store makes the
buckets slightly smaller and does not touch the directory, so the comparison
flatters this layout a little. It does not flatter it by 4×.

Reproduce: `west build -p -b native_sim app_cbor_persondb` (defaults are the
two-instance layout), then run the binary twice in the same directory.

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

`app_perf/RESULTS.md` measures the same delta independently at **+23 036 /
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

`app_perf/RESULTS.md` fits UBI at **178 µs per flash transaction and
0.616 µs/B**, from a different app with different payloads. Applied to this
app's own counters for `check`, with no refitting:

| | per `check` |
|---|--:|
| flash transactions | 115.6 |
| bytes | 10 073 B |
| predicted 115.6 × 178 µs + 10 073 × 0.616 µs + 945 µs CBOR | **27.73 ms** |
| **measured** | **27.66 ms** |

**0.3% apart.** Both backends' constants now predict this application's
decision cost from `app_perf`'s numbers, which is the strongest evidence in
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

The `app_perf` UBI constants — 178 µs per flash transaction, 0.616 µs/B, fitted
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
