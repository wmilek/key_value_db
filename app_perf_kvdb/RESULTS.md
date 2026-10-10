# app_perf_kvdb — reference results

Hardware-measured numbers for the kvdb (L3) demo/benchmark
(`src/main.c`). Keep this file honest — update it when the shape of the
benchmark changes or when regenerating on new hardware.

Regenerated on `e80f404` (main, after PR 10 merged). The previous numbers
in this file predated PR 2's lookup change and are superseded — the
reference to app_perf's "16.9 ms per blob_db read" no longer describes
this library at all; a small-blob read is now 460 µs.

**The two-level `kvhash` is measured on the DK in "On the two-level
`kvhash`".** `populate` is 2.08× faster and a rerun's `mount+open` 2.89×
faster, but steady-state reads cost ~1.65× more, so this app's rerun goes
12.8 s → 19.7 s. It lands the opposite way from `app_cbor_persondb`, and the
reason is read/write mix.

**v1.1.0 adds two enumeration phases, `count` and `walk`, for kvhash's
`map_ops.count` and `map_ops.next`.** They have not run on the DK yet; "On
`count` and `next`" gives their flash I/O from a `native_sim` run and what
that predicts for the board, and says which figure is measured and which is
not. Every other table in this file predates them and is unaffected: the
new phases read only, and sit between phases that were already timed
separately.

**v1.2.0 adds a value-blob variant (`CONFIG_APP_PERF_KVDB_VALUE_BLOBS`):
the map holds blob ids and every value is its own blob.** On `native_sim` it
writes 2.9× fewer bytes per rewrite and does ~12 % more flash reads per get;
scaled to the DK by blob transactions, a get is ≈1.33× slower. See "On values
as blob ids"; nothing in it has run on the DK yet.

**`blob_db` now defaults to the UBI backend, and the tables below are
`flash_area`.** Both are measured — see "On the UBI backend", which is also
where this app's most interesting result lives: **UBI halves the cost of
creating the store**, because `flash_area` erases the partition and then
erases every bucket again, while UBI reuses the blocks its own format just
erased.

## Setup

- **Target**: nRF5340-DK (S/N 960115021, PCA10095), cpuapp core
- **Storage**: `storage_partition` on the on-board MX25R6435F QSPI NOR
  (8 MB, 64 KB sectors, 8 MHz Quad-SPI) — see
  `boards/nrf5340dk_nrf5340_cpuapp.overlay`
- **Config**: `N_KEYS = 768`, `VAL_LEN = 16` (value = 24 B), kvhash
  backend, `BLOB_DB_MAX_PAYLOAD_LEN = 1024`
- **Code**: `e80f404`; **Zephyr** build `4a405846193f`, SDK
  `zephyr-sdk-1.0.1`

## Numbers

First run (store creation, one-time):

| phase    | ops | time     | per op  | notes                                                               |
| -------- | --: | -------: | ------: | ------------------------------------------------------------------- |
| format   |   — | 140.0 s  |       — | full-partition erase (FRESH_START path; reported inside mount+open) |
| prepare  | 122 | 134.5 s  | 1.102 s | one 64 KB sector erase per blob_db bucket                           |
| populate | 770 |  20.9 s  | 27.1 ms | warm write path + lazy kvhash-bucket creation                       |

Every rerun (the steady-state test):

| phase      | ops | time    | per op  |  ops/s |
| ---------- | --: | ------: | ------: | -----: |
| mount+open |   — |  1.26 s |       — |      — |
| verify     | 769 |  2.08 s | 2.71 ms |  369.0 |
| modify     | 196 |  2.17 s | 11.1 ms |   90.4 |
| reverify   | 769 |  2.17 s | 2.82 ms |  354.1 |

**Rerun total ≈ 6.4 s, down from ≈65 s.** The one-minute steady-state
test the `N_KEYS` help text describes is now a six-second test; that
default was sized against the old per-op costs and is worth revisiting if
a minute of wall clock was the point.

### Where the gains come from

Everything here is inherited from L1, not from kvdb changes:

| phase    | before  |    now  |     Δ |
| -------- | ------: | ------: | ----: |
| verify   | 34.9 ms | 2.71 ms | ×12.9 |
| modify   | 55.9 ms | 11.1 ms |  ×5.0 |
| reverify | 35.1 ms | 2.82 ms | ×12.4 |
| populate | 78.0 ms | 27.1 ms |  ×2.9 |

A `kvdb_get` is two blob_db reads (bucket directory + bucket blob). At
460 µs per small-blob read that predicts ~0.9 ms, against 2.71 ms
measured — the remainder is kvhash's in-bucket unpack and key compare
over a 1024 B packed bucket, which is now the dominant term rather than
flash time. **The read path is no longer where a `get` spends its time**,
so further work on this app belongs in the packing, not in L1.

`modify` (a `kvdb_set` over an existing key) is a get plus one blob_db
update. At 2.5 ms per update that predicts ~5.2 ms against 11.1 ms
measured; the gap is the bucket repack plus the second directory read.

`format` and `prepare` are unchanged, as they must be — both are pure
64 KB sector erase at ~1.09 s each, a property of the MX25R64 and not of
any code in this tree.

## On the UBI backend (the default)

Same commit, same board, same defaults; only `CONFIG_BLOB_DB_BACKEND_UBI`
differs, taken from the board conf rather than a command-line override.

### Store creation is halved — the one place UBI wins outright

| phase | `flash_area` | UBI | |
|---|--:|--:|--:|
| `mount+open` (incl. format) | 140.0 s | 145.4 s | +4% |
| `prepare` (116–122 buckets) | **134.5 s** | **0.148 s** | **×909** |
| **format + prepare total** | **274.5 s** | **145.5 s** | **×1.89 faster** |
| `populate` | 27.1 ms/op | 29.8 ms/op | ×1.10 slower |

`prepare` costs 1 275 µs/op against 1 102 467. The reason is not that UBI
made erasing cheap — it is that **`flash_area` erases the same blocks
twice.** `FRESH_START` erases the whole partition, and then `prepare()`
erases each of the 122 buckets again, because on the raw partition a bucket
format means erasing that sector whether or not it was just erased. On UBI
a bucket format is an LEB operation, and UBI still has the PEBs its own
format erased moments earlier, so it hands one over without touching flash.

**This is a genuine saving of ~129 s on first population, not an accounting
artifact** — but it is available only while UBI has pre-erased blocks in
hand. `app_perf_mc/RESULTS.md` now shows both sides of that from a single
binary run twice: 1 279 µs/op on a volume UBI has just formatted, and
1 102 000 µs/op on the very next run, when the pool is spent. UBI moves erase
cost in time; it does not remove it.

### Steady state is slower, in proportion to read count

| phase | `flash_area` | UBI | Δ |
|---|--:|--:|--:|
| `mount+open` (rerun) | 1.26 s | 1.52 s | ×1.21 |
| `verify` | 2.71 ms | 6.08 ms | ×2.24 |
| `modify` | 11.1 ms | 16.1 ms | ×1.46 |
| `reverify` | 2.82 ms | 6.40 ms | ×2.27 |
| **rerun total** | **≈6.4 s** | **≈12.8 s** | **×2.0** |

Both `VERIFY PASS` results hold on both backends, at the same generations.

The pattern matches `app_perf/RESULTS.md`: UBI's LEB→PEB indirection costs
~112 µs per flash transaction and nothing per byte, so read-dominated
phases take the full ~2.25× while `modify`, which is half write, takes
~1.46×. A `kvdb_get` is two blob_db reads, and both now pay the penalty.

So on the default backend the "one-minute steady-state test" is a
~13-second test rather than a ~6-second one — still far from the minute the
`N_KEYS` help text assumes.

## On the two-level `kvhash`

`kvhash` gained a second bucket level (`doc/proposals/2026-08-20-kvhash-second-level.md`).
This app goes through `kvdb` to reach it, so it is measured here as well.
Same board, same defaults, same UBI backend; only the container differs. The
proposal's §13.2 notes nothing had run on the DK — this is that run.

`VERIFY PASS` at every generation, so the change is behaviour-preserving through
`kvdb`.

| phase | one level (UBI) | **two level** | |
|---|--:|--:|--:|
| `mount+open` (first, incl. format) | 145 366 ms | 143 781 ms | ×1.01 faster |
| `prepare` | 1 275 µs/op (116 buckets) | 1 310 µs/op (100 buckets) | ×1.03 slower |
| **`populate`** | **29 770 µs/op** | **14 340 µs/op** | **×2.08 faster** |
| `verify` (first) | 5 531 µs/op | 9 088 µs/op | ×1.64 slower |
| `modify` (first) | 15 352 µs/op | 15 331 µs/op | ×1.00 |
| `reverify` (first) | 6 045 µs/op | 9 846 µs/op | ×1.63 slower |
| **`mount+open`** (rerun) | **1 518 ms** | **526 ms** | **×2.89 faster** |
| `verify` (rerun) | 6 083 µs/op | 9 923 µs/op | ×1.63 slower |
| `modify` (rerun) | 16 122 µs/op | 16 423 µs/op | ×1.02 slower |
| `reverify` (rerun) | 6 401 µs/op | 10 793 µs/op | ×1.69 slower |
| **rerun total** | **≈12.8 s** | **≈19.7 s** | **×1.54 slower** |

The shape matches `app_cbor_persondb/RESULTS.md` §5e exactly, which is the
useful part: two applications, different access patterns, same verdict.

**Writes and boot get faster; steady-state reads get slower.** `populate` halves
and `mount+open` on a rerun is nearly three times quicker, because both are
dominated by writing or reading map structure whose largest blob just got much
smaller. `verify` and `reverify` are pure `kvdb_get` and cost ~1.65× more,
because a two-level lookup is an extra flash transaction and UBI charges 178 µs
for one against 0.616 µs per byte (`app_perf/RESULTS.md`).

`modify` is unchanged to within 2 % in both runs, which is the tell: it is a get
plus an update, so the read regression and the write improvement land on top of
each other and cancel.

**So the trade is not free here, and unlike persondb this app does not come out
ahead on the whole run** — its steady-state loop is read-dominated, and 12.8 s
becomes 19.7 s. persondb's whole run improves 2.31× because it is fill-dominated.
Which way the change lands depends entirely on the read/write mix, and these two
apps bracket it.

Note `prepare` formats **100** buckets against 116: the two-level container's
structure occupies more of the volume up front.

## On `count` and `next`

kvhash gained `count` (the number of keys, computed by a walk and never
stored) and `next` (stateless enumeration, the key itself is the cursor) —
`shape_map.h` has the contracts, `doc/impl/l2_kvhash.md` §4–5 the
implementation. kvdb does not wire either yet, so the app reaches them
through the Map op vector its handle bound at open. Four phases were added,
all read-only:

| phase     | what                                                    | checked against                               |
| --------- | ------------------------------------------------------- | --------------------------------------------- |
| `count`   | one `count` call on the inherited store                 | the gen-G prediction; zero write ops          |
| `walk`    | a full `next` walk, empty key to `-ENODATA`             | every predicted key once, with its gen-G value, nothing else; then `count` again, which must equal the walk |
| `recount` | `count` after the modify phase                          | same, at gen G+1                              |
| `rewalk`  | the walk after the modify phase                         | same, at gen G+1                              |

So the walk is a second proof of the store's content, independent of the
`get` loop: it has no prediction to drive it and must still find exactly the
predicted set. `rewalk` runs after the subset rewrite, the ghost toggle and
an intent insert+delete, so the order's stability under mutation is
exercised on every run, and `walk` on a rerun exercises it across a reboot.

### No board yet: I/O counters, and what they predict

`native_sim`, default (UBI) backend, the DK's flash geometry from
`boards/native_sim.overlay`, `CONFIG_BLOB_DB_IOSTATS=y` (on for this board
only — see `boards/native_sim.conf`). The map built from the declaration is
**depth 2, fanout 16, 256 buckets**. A rerun at gen 2 -> 3:

| phase      |  ops | flash reads | bytes read | reads / op | flash-only prediction, DK |
| ---------- | ---: | ----------: | ---------: | ---------: | ------------------------: |
| `verify`   |  769 |      26 102 |    558 063 |       33.9 |                   1.949 s |
| `count`    |    1 |       2 406 |     52 631 |          — |                   0.180 s |
| `walk`     |  770 |      28 508 |    610 709 |       37.0 |                   2.129 s |
| `modify`   |  196 |       7 894 |    157 883 |       40.3 |                   0.892 s |
| `reverify` |  769 |      26 715 |    565 551 |       34.7 |                   1.993 s |
| `recount`  |    1 |       2 602 |     55 016 |          — |                   0.194 s |
| `rewalk`   |  771 |      29 356 |    621 364 |       38.1 |                   2.190 s |

The last column is `app_perf_l0/tools/l0_timing.py predict` over
`models/mx25r64_nrf5340dk_full.json`: flash cost only, no CPU, and no UBI
per-transaction overhead, so it is a floor rather than a forecast. The ratios
between rows are the usable part, because every row pays those missing terms
in the same proportion:

- **A `next` walk costs 1.09× a `get` loop over the same keys** — 28 508
  reads against 26 102, 610 709 B against 558 063 B. That is the
  "n × (depth + 1) blob reads" of `l2_kvhash.md` §4.2 made concrete: each
  call resolves its cursor exactly as `get` does and usually finds the
  successor in the bucket it already has; the 9 % is the bucket-to-bucket and
  sub-map crossings. Scaled against the DK's measured two-level `verify`
  (9 923 µs per `get`, "On the two-level `kvhash`"), **a full walk of 770
  entries is ≈ 8.3 s on the board, ≈ 10.8 ms per entry** — a scaling, not a
  measurement.
- **`count` costs 9.2 % of a `get` loop**: 2 406 reads for at most 288 blob
  reads (16 of the top directory, 16 sub-directories, up to 256 buckets),
  against 2 307 blob reads for 769 `get`s (three each at depth 2). Per record, as the contract says, not per
  entry — the same 2 406 reads would count 8 keys or 2 000. The same scaling
  puts it at **≈ 0.7 s on the board**, against the model's 0.18 s floor; the
  gap is UBI's ~178 µs per transaction, which `count` pays once per
  record-read like everything else here.
- **`count` wrote nothing** (`wr 0 ops`) — the app fails the run otherwise.
  Neither did the walks.
- **Both drift by exactly +196 reads per generation** (`count` 2 210 →
  2 406 → 2 602 across gens 1, 2, 3), and 196 is the number of blob updates
  `modify` makes. Each update appends a slot to its blob_db bucket log, and
  every later lookup in that bucket scans one more 12-byte slot header
  (`l1_bucketlog.md` §1.5) until compaction. It is an L1 property, not
  kvhash's: `verify` drifts the same way (+613 per generation, spread over
  769 lookups). Deterministic, so it is a regression guard, and it is what
  "cost is per record" turns into once the records have a history.

Putting the scalings together, the four phases add ≈ 18 s to a DK rerun that
"On the two-level `kvhash`" measured at ≈ 19.7 s. The `N_KEYS` help text's
one-minute budget is back to about half spent.

The native_sim capture is below; the DK capture, when it exists, belongs in
its place and this section's scalings replaced by it.

## On values as blob ids (`CONFIG_APP_PERF_KVDB_VALUE_BLOBS`)

v1.2.0 adds a variant that stores **every value in its own blob**: the kvhash
map holds key → u64 blob id, and the value is that blob's payload. This is the
full indirection `l1_model_container.md` §2 and `l2_containers.md` §4.2 take as
the ground case (for values; keys stay in the map), which kvhash v1 replaced
with inline values without, as far as the docs record, measuring it. The
workload, the verification and the intent protocol are unchanged; only
`kv_get` / `kv_set` / `kv_delete` / `kv_next` in `src/main.c` differ:

| op | inline (default) | value blobs |
|---|---|---|
| get | map get (3 blob reads at depth 2) | map get + value blob read (**4**) |
| set, key exists | map get + bucket rewrite | map get + **value blob rewrite** (the map is not written) |
| set, new key | map set | map get (miss) + value blob bind + map set |
| delete | map delete | map get + map delete + value blob delete |

An insert or delete interrupted between its two writes leaves an unreferenced
value blob, never a wrong value. Nothing reclaims it; that cost is not modelled
here.

### No board yet: I/O counters, and what they predict

`native_sim`, same setup as above. Both builds declare the same population, so
both build the same **depth 2, fanout 16, 256 buckets** map, and the only
difference is where the value lives. `VERIFY`, `COUNT` and `WALK PASS` at every
generation (checked through gen 4). Rerun, gen 2 → 3:

| phase | inline: reads / bytes read | value blobs: reads / bytes read | writes (inline → value blobs) | flash-only prediction, DK (inline → value blobs) | |
|---|--:|--:|--:|--:|--:|
| `verify`   | 26 102 / 558 063 | 29 530 / 565 231 | — | 1.949 → 2.188 s | ×1.12 |
| `count`    |  2 406 /  52 631 |  2 213 /  38 027 | — | 0.180 → 0.163 s | ×0.91 |
| `walk`     | 28 508 / 610 709 | 31 748 / 603 323 | — | 2.129 → 2.351 s | ×1.10 |
| `modify`   |  7 894 / 157 883 |  7 219 / 140 805 | 196 / 22 420 B → 200 / 7 673 B, +1 erase | 0.892 → 0.737 s | ×0.83 |
| `reverify` | 26 715 / 565 551 | 29 737 / 567 789 | — | 1.993 → 2.203 s | ×1.11 |
| `recount`  |  2 602 /  55 016 |  2 216 /  38 080 | — | 0.194 → 0.163 s | ×0.84 |
| `rewalk`   | 29 356 / 621 364 | 31 994 / 606 632 | — | 2.190 → 2.369 s | ×1.08 |
| **sum**    | | | | **9.526 → 10.174 s** | **×1.07** |

First run (store creation):

| phase | inline | value blobs | flash-only prediction, DK |
|---|--:|--:|--:|
| `populate` | rd 25 248 / wr 1 027 (100 300 B) / er 1 | rd 46 982 / wr 1 800 (104 216 B) / er 4 | 3.342 → 5.355 s, ×1.60 |

What the counters say:

- **Writes shrink 2.9× in bytes, at the same count.** A rewrite of an existing
  key writes a 24 B value blob instead of repacking its whole kvhash bucket
  (22 420 B → 7 673 B for 196 rewrites). That is the one thing value blobs
  buy outright, and the saving grows with the bucket size: the fuller the
  buckets, the more an inline rewrite costs.
- **Buckets stop drifting.** `count` grows +196 reads per generation inline
  (each bucket rewrite appends a slot to its blob_db bucket log) and +3 with
  value blobs, because `modify` no longer touches the buckets; the rewritten
  value blobs drift instead, and only for the keys that were rewritten.
- **The map is a third smaller.** 16 B per entry against 32 B here
  (`klen`/`vlen` + `k000` + 8 B id, against + 24 B value); `count` reads
  38 027 B against 52 631 B.
- **Every get costs one more blob read**, so `verify` and `walk` do ~12 %
  more flash reads. Bytes read barely move, because the extra read is small
  and the buckets it scans are smaller.
- **Creating the store costs 1.6×.** Every new key pays a map lookup that
  misses before its value blob is bound and its id published: 1 800 writes
  against 1 027, nearly twice the reads, and 4 erases against 1, as the extra
  768 blobs fill more of blob_db's buckets.

### What it means on the DK: probably slower, not ×1.07

The flash-only column cannot be the forecast here. It leaves out the cost per
blob transaction, and on this board that cost is most of the time. The one-level
to two-level change measured on the DK ("On the two-level `kvhash`") added
**one blob read per get** and took `verify` from 6 083 to 9 923 µs/op: about
3.0–3.3 ms per blob read at both depths, against a flash-only prediction of
≈2.5 ms for the whole three-read get. Value blobs add exactly one more blob read
per get, so scaling by blob reads (3 → 4) puts **`verify` at ≈13 ms/op on the
DK, ≈1.33× slower** — a scaling, not a measurement. `modify` makes the same
number of blob transactions in both layouts (one map get and one write) and
writes less, so it should hold or improve. Applied to this app's read-heavy
rerun, the result is roughly **19.7 s → ≈25 s**.

So, for this workload: **value blobs trade read latency for write volume.** They
are worth it where values are large, rewritten often, or would crowd a kvhash
bucket (the `-ENOSPC` limit: a value blob takes 8 B of bucket space whatever its
size). They are not worth it for small values on a read-heavy store, which is
why `l2_containers.md` §4.2 planned an inline threshold (`KVLIST_INLINE_MAX`,
64 B) rather than either extreme. The DK capture, when it exists, should
replace this scaling.

### The same layout, done by kvhash itself

kvhash can now spill values on its own (`doc/proposals/2026-10-09-kvhash-spill.md`).
Building the inline app with `-DCONFIG_BLOB_CONTAINER_KVHASH_VAL_INLINE_MAX=0`
puts every value in a blob of its own, the layout of this section, without the
app's wrappers. Every read and write count matches `VALUE_BLOBS` (the gen 2 → 3
rerun above, phase for phase, with `modify` ~1.5 % fewer reads), except store
creation:

| `populate` | inline | `VALUE_BLOBS` (app) | `VAL_INLINE_MAX=0` (kvhash) |
|---|--:|--:|--:|
| flash reads | 25 248 | 46 982 | **26 788** |
| writes | 1 027 | 1 800 | 1 800 |
| flash-only prediction, DK | 3.342 s | 5.355 s | **3.849 s** |

The app had to look a key up before inserting it to learn whether a value blob
already existed; kvhash finds that out in the bucket it is already rewriting.
So the indirection's cost at creation is the extra writes, not extra lookups.
At the default thresholds (never spill) every counter in this file is unchanged.

## Raw native_sim capture — rerun (gen 2 -> 3), I/O counters

```
*** Booting Zephyr OS build bbc6385f0a2c ***
kvdb perf 1.1.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
mount+open   :              0 ms
map geometry : depth 2, fanout 16, 256 buckets, entry limit 1020 B
state: rerun, store at gen 2
bench verify   :  769 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io verify   : rd  26102 ops/  558063 B   wr     0 ops/       0 B   er    0 ops/       0 B
VERIFY PASS (gen 2)
bench count    :    1 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io count    : rd   2406 ops/   52631 B   wr     0 ops/       0 B   er    0 ops/       0 B
COUNT PASS (gen 2): 769 keys
bench walk     :  770 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io walk     : rd  28508 ops/  610709 B   wr     0 ops/       0 B   er    0 ops/       0 B
WALK PASS (gen 2): 769 entries, each exactly once
bench modify   :  196 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io modify   : rd   7894 ops/  157883 B   wr   196 ops/   22420 B   er    0 ops/       0 B
bench reverify :  769 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io reverify : rd  26715 ops/  565551 B   wr     0 ops/       0 B   er    0 ops/       0 B
VERIFY PASS (gen 3)
bench recount  :    1 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io recount  : rd   2602 ops/   55016 B   wr     0 ops/       0 B   er    0 ops/       0 B
COUNT PASS (gen 3): 770 keys
bench rewalk   :  771 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io rewalk   : rd  29356 ops/  621364 B   wr     0 ops/       0 B   er    0 ops/       0 B
WALK PASS (gen 3): 770 entries, each exactly once
done — store at gen 3; rerun to verify persistence
```

## Raw native_sim capture — value blobs, rerun (gen 2 -> 3), I/O counters

```
*** Booting Zephyr OS build bbc6385f0a2c ***
kvdb perf 1.2.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B  layout=value blobs)
mount+open   :              0 ms
map geometry : depth 2, fanout 16, 256 buckets, entry limit 1020 B
state: rerun, store at gen 2
bench verify   :  769 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io verify   : rd  29530 ops/  565231 B   wr     0 ops/       0 B   er    0 ops/       0 B
VERIFY PASS (gen 2)
bench count    :    1 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io count    : rd   2213 ops/   38027 B   wr     0 ops/       0 B   er    0 ops/       0 B
COUNT PASS (gen 2): 769 keys
bench walk     :  770 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io walk     : rd  31748 ops/  603323 B   wr     0 ops/       0 B   er    0 ops/       0 B
WALK PASS (gen 2): 769 entries, each exactly once
bench modify   :  196 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io modify   : rd   7219 ops/  140805 B   wr   200 ops/    7673 B   er    1 ops/    3968 B
bench reverify :  769 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io reverify : rd  29737 ops/  567789 B   wr     0 ops/       0 B   er    0 ops/       0 B
VERIFY PASS (gen 3)
bench recount  :    1 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io recount  : rd   2216 ops/   38080 B   wr     0 ops/       0 B   er    0 ops/       0 B
COUNT PASS (gen 3): 770 keys
bench rewalk   :  771 ops in      0 ms  ->     0.000 ops/s  (      0 us/op)
   io rewalk   : rd  31994 ops/  606632 B   wr     0 ops/       0 B   er    0 ops/       0 B
WALK PASS (gen 3): 770 entries, each exactly once
done — store at gen 3; rerun to verify persistence
```

## Raw UART capture — UBI, first run (`FRESH_START`, gen 1 -> 2)

UBI's volume-probe lines are elided; it logs them at `<err>` level.

```
*** Booting Zephyr OS build 4a405846193f ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
FRESH_START: formatting store
[00:02:25.567,413] <inf> rootreg: virgin store — registry bootstrapped at id 1
mount+open   :         145366 ms
state: empty store -> initial population
bench prepare  :  116 ops in    148 ms  ->   783.783 ops/s  (   1275 us/op)
bench populate :  770 ops in  22923 ms  ->    33.590 ops/s  (  29770 us/op)
bench verify   :  769 ops in   4254 ms  ->   180.771 ops/s  (   5531 us/op)
VERIFY PASS (gen 1)
bench modify   :  196 ops in   3009 ms  ->    65.137 ops/s  (  15352 us/op)
bench reverify :  769 ops in   4649 ms  ->   165.411 ops/s  (   6045 us/op)
VERIFY PASS (gen 2)
done — store at gen 2; rerun to verify persistence
```

Note `prepare` formats **116** buckets against 122 on `flash_area`: UBI
reserves 2 PEBs for its headers, so the volume is smaller and fewer buckets
fit.

## Raw UART capture — UBI, rerun (gen 2 -> 3)

```
*** Booting Zephyr OS build 4a405846193f ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
mount+open   :           1518 ms
state: rerun, store at gen 2
bench verify   :  769 ops in   4678 ms  ->   164.386 ops/s  (   6083 us/op)
VERIFY PASS (gen 2)
bench modify   :  196 ops in   3160 ms  ->    62.025 ops/s  (  16122 us/op)
bench reverify :  769 ops in   4923 ms  ->   156.205 ops/s  (   6401 us/op)
VERIFY PASS (gen 3)
done — store at gen 3; rerun to verify persistence
```

## Raw UART capture — `flash_area`, first run (gen 1 -> 2)

```
*** Booting Zephyr OS build 4a405846193f ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
FRESH_START: formatting store
[00:02:16.969,268] <inf> rootreg: virgin store — registry bootstrapped at id 1
mount+open   :         140003 ms
state: empty store -> initial population
bench prepare  :  122 ops in 134501 ms  ->     0.907 ops/s  (1102467 us/op)
bench populate :  770 ops in  20885 ms  ->    36.868 ops/s  (  27123 us/op)
bench verify   :  769 ops in   1867 ms  ->   411.890 ops/s  (   2427 us/op)
VERIFY PASS (gen 1)
bench modify   :  196 ops in   1983 ms  ->    98.840 ops/s  (  10117 us/op)
bench reverify :  769 ops in   2048 ms  ->   375.488 ops/s  (   2663 us/op)
VERIFY PASS (gen 2)
done — store at gen 2; rerun to verify persistence
```

## Raw UART capture — `flash_area`, rerun (gen 2 -> 3)

```
*** Booting Zephyr OS build 4a405846193f ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
mount+open   :           1256 ms
state: rerun, store at gen 2
bench verify   :  769 ops in   2084 ms  ->   369.001 ops/s  (   2710 us/op)
VERIFY PASS (gen 2)
bench modify   :  196 ops in   2168 ms  ->    90.405 ops/s  (  11061 us/op)
bench reverify :  769 ops in   2172 ms  ->   354.051 ops/s  (   2824 us/op)
VERIFY PASS (gen 3)
done — store at gen 3; rerun to verify persistence
```

The first run's own verify/modify/reverify (2.43 / 10.1 / 2.66 ms) run
slightly faster than the rerun's (2.71 / 11.1 / 2.82 ms) because that
store was populated moments earlier in bucket order; the rerun reads it
back cold from a fresh mount.

## Raw UART capture — two-level `kvhash`, first run (gen 1 -> 2)

```
*** Booting Zephyr OS build 4a405846193f ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
[00:02:23.903,320] <inf> rootreg: virgin store — registry bootstrapped at id 1
mount+open   :         143781 ms
state: empty store -> initial population
bench prepare  :  100 ops in    131 ms  ->   763.358 ops/s  (   1310 us/op)
bench populate :  770 ops in  11042 ms  ->    69.733 ops/s  (  14340 us/op)
bench verify   :  769 ops in   6989 ms  ->   110.030 ops/s  (   9088 us/op)
VERIFY PASS (gen 1)
bench modify   :  196 ops in   3005 ms  ->    65.224 ops/s  (  15331 us/op)
bench reverify :  769 ops in   7572 ms  ->   101.558 ops/s  (   9846 us/op)
VERIFY PASS (gen 2)
done — store at gen 2; rerun to verify persistence
```

## Raw UART capture — two-level `kvhash`, rerun (gen 2 -> 3)

```
*** Booting Zephyr OS build 4a405846193f ***
kvdb perf 1.0.0  (N_KEYS=768  VAL_LEN=16  STRIDE=4  val=24 B)
mount+open   :            526 ms
state: rerun, store at gen 2
bench verify   :  769 ops in   7631 ms  ->   100.773 ops/s  (   9923 us/op)
VERIFY PASS (gen 2)
bench modify   :  196 ops in   3219 ms  ->    60.888 ops/s  (  16423 us/op)
bench reverify :  769 ops in   8300 ms  ->    92.650 ops/s  (  10793 us/op)
VERIFY PASS (gen 3)
done — store at gen 3; rerun to verify persistence
```

## Reproducing

```bash
west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/kvdb app_perf_kvdb
# add -- -DCONFIG_APP_PERF_KVDB_FRESH_START=y for the store-creation run
```

That build uses the default UBI backend, with the PEB pool sized in
`boards/nrf5340dk_nrf5340_cpuapp.conf`. For the `flash_area` column add
`-DCONFIG_BLOB_DB_BACKEND_FLASH_AREA=y`, and **erase the partition raw when
switching between backends** — the two layouts are not interchangeable, and
UBI only formats a partition it finds erased.

The store must also be one this build can mount. `app_perf` enables
`CONFIG_BLOB_DB_LARGE_PAYLOADS=y`, which bumps the on-flash format major
to 2, and this app does not — so after running `app_perf` on the same
board, mount fails `-ENOTSUP` (a foreign store) *before* `FRESH_START`
gets a chance to format. Erase the partition first; see the
"Downgrading" section of `app_perf/RESULTS.md`.

Attach exactly one reader to the console tty. Two concurrent `cat`s split
the byte stream and silently shred the capture.

For the I/O counters without a board, on the DK's flash geometry:

```bash
west build -p always -b native_sim -d build/kvdb_sim app_perf_kvdb
./build/kvdb_sim/zephyr/zephyr.exe --flash=kvdb.bin --flash_erase   # first run
./build/kvdb_sim/zephyr/zephyr.exe --flash=kvdb.bin | tee rerun.log  # rerun
python3 app_perf_l0/tools/l0_timing.py predict \
    -m app_perf_l0/models/mx25r64_nrf5340dk_full.json rerun.log
```

For the value-blob variant add `-- -DCONFIG_APP_PERF_KVDB_VALUE_BLOBS=y` to
either build (`twister` runs it as `perf.kvdb.value_blobs`). It opens its own
store, so it can share a flash image with the inline build.

## Power-loss field note

A pre-intent-protocol build was power-cut mid-modify during a gen 5 -> 6
bump. The next boot's strict verify reported exactly the torn prefix —
108 keys (`k002..k430`, every 4th) each holding a *complete, valid*
gen-6 value, the rest untouched at their expected values — i.e. per-op
atomicity held; only the classification was missing. Builds with the
intent protocol classify the same state as `POWER LOSS detected`,
run the recovery verify (old-XOR-new per key), report the torn split,
and roll the bump forward.

To reproduce, cut power during the modify window right after the first
`VERIFY PASS` of a rerun — but note that window is now **~2 s**, not the
~11 s this note was written against.
