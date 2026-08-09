# `app_cbor_persondb` — reference results

Keep this file honest: update it when the shape of the app changes or when
regenerating on new hardware.

**Status: `native_sim` measured; nRF5340-DK not yet run.** Per `DESIGN.md` D6
the DK numbers will be measured on the board, so the hardware column below is
a *projection* from this repository's existing captures and is labelled as such
everywhere it appears. It is not a substitute for `A4`.

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

## 5. Projected — nRF5340-DK (**not yet measured**)

Derived from `app_perf_kvdb/RESULTS.md`: a `blob_db` read is 17.45 ms and a
write ~21 ms on the MX25R6435F at 8 MHz Quad-SPI. Multiplying by this app's
measured blob-op counts:

| Phase | Projected | Basis |
|---|--:|---|
| `check` (**R-D**) | **≈ 70 ms** | 4 sector reads |
| `byid` | ≈ 35 ms | 2 sector reads |
| `miss` | ≈ 35 ms | 2 sector reads |
| `put` | ≈ 184 ms | 6.96 reads + 2.97 writes |
| `cbor` | 6 µs | unchanged — no flash |
| fill, operations only | ≈ 33 min | 34 932 map writes × 55.9 ms |
| fill, including compaction | **≈ 2.5 h** | estimate only; see `DESIGN.md` §6.2 |

The compaction component is the least certain number here and the one most
worth measuring on the board.

## 6. What the numbers say

**The serialization format is not the cost.** `cbor` is 6 µs against `check`'s
599 µs on `native_sim` — **1.0 %**. On hardware, where a blob op is 17.45 ms
rather than ~150 µs, the same decode is a projected **0.009 %** of the decision.
Choosing CBOR over anything else costs nothing measurable, and the denormalized
permission bitmask this app deliberately did not build (`DESIGN.md` D2) would
have optimized away half of a cost that is 99 % elsewhere.

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
