# `app_cbor_persondb` — reference results

Keep this file honest: update it when the shape of the app changes or when
regenerating on new hardware.

**Status: `native_sim` measured at 10 000 persons; nRF5340-DK measured on the
board at 1 000 persons (§5).** The DK timings are no longer projections. They
were taken at a tenth of the target scale, so §5's per-operation numbers are
directly comparable to the rest of this file, while its whole-run numbers
(fill duration, occupancy) are *not* the 10 000-person figures and are marked
where they appear. A full-scale DK run remains outstanding for `A4`.

---

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

## 3. What the store holds

| | |
|---|---|
| Persons | 10 000 |
| Credentials | 24 932 (1–4 per person, mean 2.49) |
| Mean bytes per person, all-in | 433 B |
| **Live content** | **4 336 158 B** |
| **Fraction of the 8 MiB partition** | **51.6 %** |
| Bucket overflows during the fill | **0** |
| Fullest `kvhash` bucket (from `tools/sizing.py`) | 2 711 B = 66 % of the 4 KB ceiling |

R-E asked for a database at ~50 % of the DK's external flash. The realistic
record shape lands at 51.6 % without padding.

*Physical* occupancy — live bytes plus not-yet-compacted garbage — is **not
reported**, because no layer exposes it (`FINDINGS.md` B3). Everything above is
logical content, computed by the application from its own generator.

## 4. Measured — `native_sim`

Steady-state run (store already filled), 200 samples per benchmark:

| Phase | µs/op | blob ops/op | flash moved/op | payload | **amplification** |
|---|--:|--:|--:|--:|--:|
| `check` — card → person → permission (**R-D**) | 599 | 4 | 256 KB | 365 B | **711×** |
| `byid` — person id → record | 297 | 2 | 128 KB | 364 B | 360× |
| `miss` — unknown card | 327 | 2 | 128 KB | — | 26 214× |
| `put` — rewrite a record + its index entries | 2 485 | 9.98 | 638 KB | 364 B | 1 799× |
| `cbor` — encode + decode, no flash | **6** | 0 | 0 | 364 B | — |

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

The previous edition of this section was a projection built from
`app_perf_kvdb/RESULTS.md` — a `blob_db` read at 17.45 ms and a write at
~21 ms — and it is kept in the "projected" column so the error is visible.

### Per-operation

| Phase | Projected | **Measured** | Ratio | blob ops | amplification |
|---|--:|--:|--:|--:|--:|
| `check` (**R-D**) | ≈ 70 ms | **114.2 ms** | 1.63× | 4.00 | 701× |
| `byid` | ≈ 35 ms | **57.6 ms** | 1.65× | 2.00 | 355× |
| `miss` | ≈ 35 ms | **56.6 ms** | 1.62× | 2.00 | 26 214× |
| `put` | ≈ 184 ms | **313.8 ms** | 1.71× | 9.96 | 1 770× |
| `cbor` | 6 µs | **955 µs** | **159×** | 0 | — |

The four flash-bound phases are consistently **~1.65× slower** than projected:
`byid`'s two reads take 57.6 ms, so a `blob_db` read on this board is 28.8 ms,
not the 17.45 ms the projection assumed.

`cbor` is the outlier. The projection carried the `native_sim` figure across
unchanged on the grounds that it touches no flash — but 6 µs was a host x86
number, and the same encode/decode pair costs 955 µs on a 128 MHz Cortex-M33.
Assuming compute transfers between platforms was wrong by two orders of
magnitude; only the flash model transferred.

### Whole-run phases (**1 000 persons — not the 10 000-person figures**)

| Phase | Measured | Work |
|---|--:|---|
| open | 24 478 ms | **includes the `FRESH_START` 8 MiB erase** — not the steady-state open cost |
| prepare | 113 685 ms | 106 buckets formatted = **1 072 ms per bucket** |
| fill | 983 833 ms (16.4 min) | 1 000 persons + 2 479 credentials, 4 progress commits |
| verify | 48 023 ms | 256 persons, 602 credential resolutions |
| mutate | 14 712 ms | rev 0 → 1, 64 cards assigned (nothing to revoke on a first run) |
| re-verify | 49 758 ms | 256 persons, 618 credential resolutions |

Both verification passes reported `VERIFY PASS`, and the fill hit **0 bucket
overflows** — the `tools/sizing.py` geometry holds on hardware.

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

No `expired` results here: at 1 000 persons the generator's validity windows
land differently than at 10 000, so the sample happens to miss that case.

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
