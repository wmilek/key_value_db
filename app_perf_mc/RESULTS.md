# app_perf_mc — reference results

Ops/sec for the two L1 sufficiency-proof containers from
`tests/lib/blob_db_contract/`: the model container map (`model_container_map`,
key->value) and, since v1.1.0, the model container list
(`model_container_list`, a doubly linked list addressed by node handle). Each
has its root resolved through the root registry at id 1. Update this file when
the benchmark shape changes or when regenerating on new hardware.

**The map's figures below are hardware-measured (v1.0.0). The list's are not
yet:** v1.1.0 adds the list phases and per-phase I/O counters, and the list
section that follows gives those counters and what the DK timing model
predicts from them. When a v1.1.0 DK capture exists, it replaces the
prediction columns.

## v1.1.0: the list, and I/O counters per phase

v1.1.0 runs the map exactly as before, then the list on the same store
(`MC_L` in the registry, `N_NODES = 10`, the same 24 B values):

| phase       | ops        | what it does                                                         |
| ----------- | ---------- | -------------------------------------------------------------------- |
| `l.insert`  | `N_NODES`  | append at the back, `mcl_insert_before(0)`: the 6-step discipline     |
| `l.get`     | `N_GET`    | value read by handle, cycling over the nodes                          |
| `l.set`     | `N_OVW`    | in-place value overwrite by handle: one `update`, no intent           |
| `l.next`    | `N_GET`    | one forward step, wrapping through the root: never recovers or writes |
| `l.prev`    | `N_GET`    | one backward step: checks for a staged intent first                   |
| `l.remove`  | `N_NODES`  | remove from the front: the 6-step discipline                          |
| `l.cleanup` | 1          | `mcl_destroy()` + `rootreg_unregister()`                              |

Both walks check the order against the handles as they go, `l.get` keeps a
checksum, and `mcl_check` runs after `l.insert` and `l.remove`. Every phase
prints an `io` line after its `bench` line when `CONFIG_BLOB_DB_IOSTATS=y`,
which `boards/native_sim.conf` sets. The native_sim board now also takes the
DK's geometry (64 KB erase blocks, 4 B writes, the same 8 MB partition), as
`app_perf_blob_db` does, so the counters are the ones the board issues: 124
buckets on `flash_area`, as on the DK.

### No board yet: I/O counters, and what they predict

`native_sim`, DK geometry, `flash_area` backend, on `e78f04c` plus this
change. "Flash reads" and "writes" are flash transactions per op, not blob
operations: one blob read walks the 12-byte slot headers of its bucket, so
it costs several. The prediction is `app_perf_l0/tools/l0_timing.py predict`
over `models/mx25r64_nrf5340dk_full.json`. It covers flash cost only, with no
CPU time and no UBI per-transaction cost.

The map rows give the model a check against the v1.0.0 DK capture further
down:

| phase       | flash reads / op | writes / op | predicted, DK | measured, DK (v1.0.0) | ratio |
| ----------- | ---------------: | ----------: | ------------: | --------------------: | ----: |
| `set`       |             82.5 |           5 |       9.60 ms |              13.60 ms |  0.71 |
| `get`       |             47.5 |           0 |       3.46 ms |               3.53 ms |  0.98 |
| `overwrite` |            330.5 |           5 |      28.38 ms |              29.10 ms |  0.98 |
| `delete`    |            561.0 |           5 |      43.65 ms |              37.00 ms |  1.18 |
| `cleanup`   |              452 |           3 |      33.83 ms |              23.00 ms |  1.47 |

`get` and `overwrite`, the two phases with the most ops, land within 3%.
`delete` and `cleanup` are 10 ops and 1 op, run late on a store whose
bucket histories changed since `e80f404` (PR 55 word-aligns every slot), and
the model overshoots them. So this is a check, not a calibration. The list:

| phase       | flash reads / op | writes / op | predicted, DK |  ops/s | against the map               |
| ----------- | ---------------: | ----------: | ------------: | -----: | ----------------------------- |
| `l.insert`  |            101.2 |           6 |      11.71 ms |   85.4 | `set` 9.60 ms                 |
| `l.get`     |             10.9 |           0 |       0.79 ms |   1264 | `get` 3.46 ms: **×4.4 less**  |
| `l.set`     |             58.9 |           1 |       4.92 ms |    203 | `overwrite` 28.38 ms: **×5.8 less** |
| `l.next`    |              6.9 |           0 |       0.50 ms |   1996 | —                             |
| `l.prev`    |             47.9 |           0 |       3.47 ms |    288 | `l.next`: **×6.9 more**       |
| `l.remove`  |            236.9 |           6 |      20.94 ms |   47.8 | `delete` 43.65 ms: ×2.1 less  |
| `l.cleanup` |              187 |           3 |      14.70 ms |      — | `cleanup` 33.83 ms            |

The UBI build issues the same counters for every timed phase, map and list,
transaction for transaction; only `prepare` differs (117 buckets against
124). On the DK, UBI cost the map ×1.5–2.4 (see "On the UBI backend"); expect
the list to fall in the same band, read-heavy phases at the top of it.

What the counters show:

- **The list's handle accesses are O(1); the map's key lookups are not.**
  `l.get` is two blob reads (node, then value) against the map's ~7. That
  gap grows with population, because the map scans its pair list. The list's
  own price shows up elsewhere: there is no lookup by key, and the client
  holds the handles.
- **`l.set` has no intent.** It changes no references, so it is one atomic
  `update` of the value blob after a membership check (`node_linked`).
  The map's `overwrite` replaces the value blob through the full discipline
  (4 updates + 1 delete). At 1 write against 5, the list overwrites ×5.8
  cheaper.
- **An insert costs one more write than a map `set`.** That is the REPAIR
  step: 6 writes (stage, value, node, commit, repair, clear) against the
  map's 5. Most of the reads are read-modify-writes: every link write
  reloads the blob it changes. With `mcl_begin()`'s recovery check on top,
  an append at the back reads the root four times.
- **`l.prev` costs ×6.9 an `l.next`**, while both read one node. `mcl_prev`
  first runs recovery to rule out a staged mutation, which reads the root and
  the intent: two extra blob reads (the header comment in
  `model_container_list.h` says one). Those two blobs are the most rewritten
  in the store, since every mutation updates the intent twice (stage and
  clear). So each read walks a long slot history in its bucket. The cost
  comes from the store's history, not from the list.
- **Every phase ends at the floor.** The map, then the list, each leave 1
  live blob (the empty registry), and `l.get checksum` equals the map's
  `0xac6a5f02` because both read the same value sequence.

### With `CONFIG_BLOB_DB_SLOT_INDEX=y`

The slot index (PR 54) is opt-in. It removes exactly the slot-header walk that
dominates the rows above. Same run, `flash_area`, default index size (2048
entries):

| phase       | flash reads / op | predicted, DK | without index |     Δ |
| ----------- | ---------------: | ------------: | ------------: | ----: |
| `set`       |             24.5 |       5.40 ms |       9.60 ms | ×1.8  |
| `get`       |             22.5 |       1.64 ms |       3.46 ms | ×2.1  |
| `overwrite` |             30.5 |       6.68 ms |      28.38 ms | ×4.2  |
| `delete`    |             26.0 |       4.97 ms |      43.65 ms | ×8.8  |
| `cleanup`   |               18 |       2.45 ms |      33.83 ms | ×13.8 |
| `l.insert`  |             24.0 |       6.12 ms |      11.71 ms | ×1.9  |
| `l.get`     |              6.0 |       0.43 ms |       0.79 ms | ×1.8  |
| `l.set`     |             13.0 |       1.59 ms |       4.92 ms | ×3.1  |
| `l.next`    |              3.0 |       0.22 ms |       0.50 ms | ×2.3  |
| `l.prev`    |              9.0 |       0.66 ms |       3.47 ms | ×5.3  |
| `l.remove`  |             33.0 |       6.19 ms |      20.94 ms | ×3.4  |
| `l.cleanup` |               21 |       2.69 ms |      14.70 ms | ×5.5  |

Writes per op are unchanged. With the index, a blob read costs 3 flash reads
wherever it lives, so `l.prev` falls to exactly three blob reads (root,
intent, node) against `l.next`'s one. The gains grow down the table as the
blobs touched get older, which is where the walk was longest.
Checksums and end state are identical.

### Raw native_sim capture (`flash_area`, DK geometry)

Wall clock is 0 ms throughout because the flash simulator models no latency.
The `io` lines are the result.

```
model-container perf 1.1.0  (N_KEYS=10  N_NODES=10  N_GET=100  N_OVW=50  VAL_LEN=24)
bench prepare  :  124 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io prepare  : rd    124 ops/    1984 B   wr   124 ops/    1984 B   er  124 ops/ 8126464 B
bench set      :   10 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io set      : rd    825 ops/   10035 B   wr    50 ops/    2320 B   er    0 ops/       0 B
bench get      :  100 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io get      : rd   4750 ops/   65850 B   wr     0 ops/       0 B   er    0 ops/       0 B
bench overwrite:   50 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io overwrite: rd  16525 ops/  202825 B   wr   250 ops/   15000 B   er    0 ops/       0 B
bench delete   :   10 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io delete   : rd   5610 ops/   67660 B   wr    50 ops/    1880 B   er    0 ops/       0 B
bench cleanup  :    1 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io cleanup  : rd    452 ops/    5394 B   wr     3 ops/      56 B   er    0 ops/       0 B
get checksum: 0xac6a5f02
live blobs after map: 1 (expect 1 — the empty registry alone)
bench l.insert :   10 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io l.insert : rd   1012 ops/   12864 B   wr    60 ops/    2800 B   er    0 ops/       0 B
bench l.get    :  100 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io l.get    : rd   1090 ops/   14280 B   wr     0 ops/       0 B   er    0 ops/       0 B
bench l.set    :   50 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io l.set    : rd   2945 ops/   37340 B   wr    50 ops/    2000 B   er    0 ops/       0 B
bench l.next   :  100 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io l.next   : rd    691 ops/    8892 B   wr     0 ops/       0 B   er    0 ops/       0 B
bench l.prev   :  100 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io l.prev   : rd   4791 ops/   60492 B   wr     0 ops/       0 B   er    0 ops/       0 B
bench l.remove :   10 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io l.remove : rd   2369 ops/   29248 B   wr    60 ops/    2320 B   er    0 ops/       0 B
bench l.cleanup:    1 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io l.cleanup: rd    187 ops/    2308 B   wr     3 ops/      56 B   er    0 ops/       0 B
l.get checksum: 0xac6a5f02
live blobs at end: 1 (expect 1 — the empty registry alone)
```

## The map on hardware (v1.0.0)

Regenerated on `e80f404` (main, after PR 10 merged). The previous numbers
predated PR 2's lookup change; the "~59 reads/s (17 ms)" this file used to
scale against is now 2174 reads/s (460 µs).

**`blob_db` now defaults to the UBI backend, and the headline table below is
`flash_area`.** Both are measured — see "On the UBI backend". The default
costs roughly 2× on reads, so the numbers a caller should expect from a
default build are the UBI column, not the first table.

The UBI numbers were **revalidated on the DK at `1d3fcb6`**, after the default
flip, from a build that takes the backend from Kconfig with no override. Two
fresh runs reproduce the timed phases within a tick, and settle what `prepare`
costs on UBI: nothing on a freshly formatted volume, a full sector erase per
bucket on one already in use. See "`prepare` on UBI is not one number".

## Setup

- **Target**: nRF5340-DK (S/N 960115021, PCA10095), cpuapp core
- **Storage**: `storage_partition` on the on-board MX25R6435F QSPI NOR
  (8 MB, 64 KB sectors, 8 MHz Quad-SPI)
- **Config**: `N_KEYS = 10`, `N_GET = 100`, `N_OVW = 50`, `VAL_LEN = 24 B`
- **Code**: `e80f404` for the `flash_area` table, `1d3fcb6` for the UBI
  revalidation; **Zephyr** build `4a405846193f`, SDK `zephyr-sdk-1.0.1`
- One `erase_all` + `blob_db_prepare(150)` up front (capped to the 124
  non-root buckets), so every timed op runs on the warm append-only path.

## Numbers

| workload  |  ops/s | ms/op | blob_db ops per mc op (approx)                                      |
| --------- | -----: | ----: | ------------------------------------------------------------------- |
| set       |  73.53 | 13.60 | ~2 reads + 4 updates + 0–1 del                                      |
| get       | 283.29 |  3.53 | ~7 reads (list + key scan + value)                                  |
| overwrite |  34.36 | 29.10 | ~2 reads + 4 updates + 1 delete                                     |
| delete    |  27.03 | 37.00 | ~2 reads + 2 updates + 2 deletes                                    |
| cleanup   |      — | 23.00 | mc_destroy (2 deletes on an emptied container) + rootreg_unregister |

Against the previous run, with the container's own code unchanged:

| workload  |  before |    now |     Δ |
| --------- | ------: | -----: | ----: |
| set       |  235 ms | 13.6 ms | ×17.3 |
| get       |  164 ms | 3.53 ms | ×46.5 |
| overwrite |  263 ms | 29.1 ms |  ×9.0 |
| delete    |  211 ms | 37.0 ms |  ×5.7 |
| cleanup   |  119 ms | 23.0 ms |  ×5.2 |

Behaviour is bit-for-bit identical across the change: same `get checksum:
0xac6a5f02`, same end state of 1 live blob.

## On the UBI backend (the default)

Same commit, same board, same defaults; only `CONFIG_BLOB_DB_BACKEND_UBI`
differs. **Revalidated on `1d3fcb6`** after the default flip, where the backend
now comes from the Kconfig default with no override on the command line at all.
Three UBI runs agree to within 0.7% on every timed phase except `cleanup`,
which is a single op timed to 1 ms and moves by one tick; all three produce
`get checksum: 0xac6a5f02` and the same 1-live-blob end state.

| workload    | `flash_area` |     UBI |     Δ | spread over 3 UBI runs |
| ----------- | -----------: | ------: | ----: | ---------------------: |
| `get`       |      3.53 ms | 8.59 ms | ×2.43 |         8.55 – 8.60 ms |
| `set`       |     13.60 ms | 20.9 ms | ×1.54 |         20.9 – 21.0 ms |
| `overwrite` |     29.10 ms | 56.5 ms | ×1.94 |         56.3 – 56.6 ms |
| `delete`    |     37.00 ms | 83.8 ms | ×2.26 |         83.3 – 83.9 ms |
| `cleanup`   |     23.00 ms | 56.0 ms | ×2.43 |         55.0 – 56.0 ms |

The container behaves identically on both backends; only the substrate's cost
changes.

The spread across rows is explained by what each operation is made of.
`get` is ~7 reads and pays UBI's per-transaction penalty on every one, so
it takes the full ×2.4. `set` is dominated by 4 updates whose cost is
program time rather than transactions, so it only takes ×1.5. That matches
`app_perf_blob_db/RESULTS.md`, which fits the penalty at ~112 µs per flash
transaction and nothing per byte.

### `prepare` on UBI is not one number, it is two

`prepare` is left out of the table above because on UBI it does not have a
single value. The same firmware, run twice back to back on the same board,
measures:

| `blob_db_prepare()` of 118 buckets                              |     total | per bucket |
| --------------------------------------------------------------- | --------: | ---------: |
| on a volume UBI has just formatted (partition raw-erased first)  | **0.151 s** |   **1 279 µs** |
| on a volume already in use (the very next run)                   | **130.0 s** | **1 102 000 µs** |

**×861, from nothing but the state the previous run left behind.** On
`flash_area` the same phase costs 1 088 620 µs/op either way, because a bucket
format there means erasing that sector whether or not it was erased a moment
ago.

This is the whole of UBI's erase behaviour in one measurement. A bucket format
on UBI is an LEB operation: if UBI is holding a pre-erased PEB it hands one
over without touching flash, and if it is not, it erases one first. Formatting
the volume leaves it holding a pool of them; the benchmark spends that pool.
**UBI moves erase cost in time; it does not remove it** — the erase this app
skips on a fresh volume is one the next run pays.

Which figure applies to a caller depends on what the device did before, and
neither is the "real" one. A product that formats at manufacture and prepares
its buckets in the same session gets the 0.151 s; one that prepares more
buckets later, in the field, pays the full second per bucket.

`app_perf_kvdb/RESULTS.md` reports the same effect on its `FRESH_START` path
(0.148 s against 134.5 s), where it had to be inferred by comparing two
different apps. Here it is one binary twice, so the volume state is the only
variable.

Note also that `prepare` formats **118** buckets here against 124 on
`flash_area` — UBI keeps 2 PEBs for its headers and the volume is smaller by
that much, so the usable bucket count drops.

### Where the multiples come from

For scale, raw blob_db on the same setup now does ~2174 reads/s (460 µs)
and ~398 updates/s (2.51 ms) — see `app_perf_blob_db/RESULTS.md`.

- **get** is now almost entirely explained by its read count. Seven reads
  at 460 µs predicts 3.22 ms against **3.53 ms** measured — 91% of the
  time is flash reads and the model needs no fudge factor. Note what did
  *not* change: the pair-list walk is still O(n), averaging ~5.5 key
  probes for 10 keys. The algorithmic shape is untouched; only the
  constant collapsed, from a 17 ms whole-sector read per probe to a
  460 µs streamed one. At larger `N_KEYS` the O(n) term will still
  dominate.
- **set/overwrite/delete** still pay the full 5-step crash-safe mutation
  discipline (stage intent, prepare blobs, commit list swap, cleanup,
  clear intent) — 4–6 flash writes per logical operation. They improved
  by ×5.7–17.3 rather than the ×46 that reads saw, because a mutation's
  cost is dominated by updates and deletes rather than reads, and only the
  read half of the library got faster. `delete` at 37 ms against ~6 ms of
  accountable reads and updates implies a blob_db delete costs
  appreciably more than an update; `app_perf_blob_db` does not benchmark delete,
  so that is inferred here rather than measured.

That mutation factor remains the measured price of "crash-safe at every
step + zero permanent leak", not overhead to be optimized here: the model
container is deliberately naive (`l1_model_container.md` §1), and the real
L2 containers reduce both the O(n) key scan and the per-mutation write
count.

The registry itself adds nothing measurable to the phases: the CLIENT (this
app) reads it once at open to resolve the root id it hands to `mc_open()`;
the container never touches it (`mc_get`/`mc_set` operate on the resolved
root id).

Cleanup check on hardware: after the delete phase the store holds the
structural floor (registry + list + intent); the final cleanup phase —
`mc_destroy()` then `rootreg_unregister()` (teardown before unregistration,
registry §8) — takes it all the way down to **1 blob: the empty registry**.
Nothing leaks anywhere in the lifecycle.

## Raw UART captures — UBI backend (the default), `1d3fcb6`

Two runs of the identical `build/mc` image, back to back. UBI's volume-probe
and PEB-recovery lines are elided; it logs recoverable conditions at `<err>`
level.

Run A — partition raw-erased beforehand, so UBI formats the volume on this
boot and `prepare` finds pre-erased PEBs waiting:

```
*** Booting Zephyr OS build 4a405846193f ***
model-container perf 1.0.0  (N_KEYS=10  N_GET=100  N_OVW=50  VAL_LEN=24)
bench prepare  :  118 ops in     151 ms  ->  781.456 ops/s  (     1279 us/op)
bench set      :   10 ops in     209 ms  ->   47.846 ops/s  (    20900 us/op)
bench get      :  100 ops in     855 ms  ->  116.959 ops/s  (     8550 us/op)
bench overwrite:   50 ops in    2817 ms  ->   17.749 ops/s  (    56340 us/op)
bench delete   :   10 ops in     833 ms  ->   12.004 ops/s  (    83300 us/op)
bench cleanup  :    1 ops in      55 ms  ->   18.181 ops/s  (    55000 us/op)
get checksum: 0xac6a5f02
live blobs at end: 1 (expect 1 — the empty registry alone)
```

Run B — same image reflashed immediately after, on the volume run A left
behind. Only `prepare` moves, by ×861:

```
*** Booting Zephyr OS build 4a405846193f ***
model-container perf 1.0.0  (N_KEYS=10  N_GET=100  N_OVW=50  VAL_LEN=24)
bench prepare  :  118 ops in  130036 ms  ->    0.907 ops/s  (  1102000 us/op)
bench set      :   10 ops in     210 ms  ->   47.619 ops/s  (    21000 us/op)
bench get      :  100 ops in     860 ms  ->  116.279 ops/s  (     8600 us/op)
bench overwrite:   50 ops in    2831 ms  ->   17.661 ops/s  (    56620 us/op)
bench delete   :   10 ops in     839 ms  ->   11.918 ops/s  (    83900 us/op)
bench cleanup  :    1 ops in      56 ms  ->   17.857 ops/s  (    56000 us/op)
get checksum: 0xac6a5f02
live blobs at end: 1 (expect 1 — the empty registry alone)
```

The earlier UBI run this file's table was built from (`e80f404`, in-use
volume) read 1088542 / 20900 / 8590 / 56520 / 83800 / 56000 µs — the same
numbers to within a tick.

## Raw UART capture — `flash_area`

```
*** Booting Zephyr OS build 4a405846193f ***
model-container perf 1.0.0  (N_KEYS=10  N_GET=100  N_OVW=50  VAL_LEN=24)
bench prepare  :  124 ops in  134989 ms  ->    0.918 ops/s  (  1088620 us/op)
bench set      :   10 ops in     136 ms  ->   73.529 ops/s  (    13600 us/op)
bench get      :  100 ops in     353 ms  ->  283.286 ops/s  (     3530 us/op)
bench overwrite:   50 ops in    1455 ms  ->   34.364 ops/s  (    29100 us/op)
bench delete   :   10 ops in     370 ms  ->   27.027 ops/s  (    37000 us/op)
bench cleanup  :    1 ops in      23 ms  ->   43.478 ops/s  (    23000 us/op)
get checksum: 0xac6a5f02
live blobs at end: 1 (expect 1 — the empty registry alone)
```

`prepare` is unchanged at ~1.09 s per bucket, as it must be — that is a
64 KB sector erase on the MX25R64, not a property of any code here.

native_sim run (timing-free, correctness reference): same checksum
`0xac6a5f02`, same end state of 1 live blob.

## Reproducing

```bash
west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/mc app_perf_mc
nrfutil device program --firmware build/mc/zephyr/zephyr.hex \
    --options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE,reset=RESET_SYSTEM \
    --serial-number <your-jlink-sn>
```

That build takes the UBI backend from the Kconfig default, with the PEB pool
sized for this geometry in `boards/nrf5340dk_nrf5340_cpuapp.conf`. The DK build
leaves `CONFIG_BLOB_DB_IOSTATS` off, so its timings stay comparable with the
v1.0.0 captures above. A v1.1.0 capture prints the list phases after the map's. For the
`flash_area` column add `-DCONFIG_BLOB_DB_BACKEND_FLASH_AREA=y`, and **erase
the partition raw when switching between backends** — the two layouts are not
interchangeable, and UBI only formats a partition it finds erased.

Start the console capture *before* programming and let the reset above
start the run. Resolve the port with `nrfutil device list` and take the one
labelled `vcom: 2` — the `/dev/ttyACM*` number is not stable. Attach
exactly one reader: two concurrent `cat`s split the byte stream and
silently shred the capture.

This app does not enable `CONFIG_BLOB_DB_LARGE_PAYLOADS`, so it cannot
mount a store left behind by `app_perf_blob_db` (format major 2 against major 1)
and will fail `-ENOTSUP`. Erase the partition first; see the
"Downgrading" section of `app_perf_blob_db/RESULTS.md`.

Wall clock depends entirely on what state the volume is in. On a raw-erased
partition the whole run is **~4 s** (0.15 s prepare + ~3.8 s of timed phases);
on a volume already in use it is **~2.2 min**, of which prepare is 98%. Both
are in the captures above. Erase the partition first if you want the numbers
to be comparable between sessions.

For the I/O counters without a board, on the DK's flash geometry:

```bash
west build -p always -b native_sim -d build/mc_sim app_perf_mc -- \
    -DCONFIG_BLOB_DB_BACKEND_FLASH_AREA=y
./build/mc_sim/zephyr/zephyr.exe --flash=mc.bin --flash_erase | tee mc.log
python3 app_perf_l0/tools/l0_timing.py predict \
    -m app_perf_l0/models/mx25r64_nrf5340dk_full.json mc.log
```

Drop the `-DCONFIG_BLOB_DB_BACKEND_FLASH_AREA=y` for the UBI build; add
`-DCONFIG_BLOB_DB_SLOT_INDEX=y` for the slot-index rows.
