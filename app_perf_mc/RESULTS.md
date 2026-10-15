# app_perf_mc — reference results

Ops/sec for the two L1 sufficiency-proof containers from
`tests/lib/blob_db_contract/`: the model container map (`model_container_map`,
key->value) and, since v1.1.0, the model container list
(`model_container_list`, a doubly linked list addressed by node handle). Each
has its root resolved through the root registry at id 1. Update this file when
the benchmark shape changes or when regenerating on new hardware.

**v1.1.0 is measured on the DK, map and list, on both backends, with and
without the slot index; see the next section.** Those runs used the MX25R in
high-performance mode, which main does not set yet (see "Setup"). The
v1.0.0 sections further down are the map's earlier captures. They predate
both that mode and word-aligned buffers (PR 55), so their absolute figures
are stale. They are kept for the record and for the UBI `prepare`
measurement.

## v1.1.0: map and list side by side

v1.1.0 runs the map, then the list on the same store (`MC_L` in the
registry, `N_NODES = 10`, the same 24 B values). Each phase is named
`<container>.<operation>`. An operation that does the same thing to both
containers has the same name in both, so `map.get` and `list.get` compare
directly:

| operation   | `map.*` (`N_KEYS` keys)                        | `list.*` (`N_NODES` nodes)                                  |
| ----------- | ---------------------------------------------- | ----------------------------------------------------------- |
| `insert`    | `mc_set()` of a fresh key                      | append at the back, `mcl_insert_before(0)`                  |
| `get`       | lookup by key, `N_GET` times                   | value read by handle, `N_GET` times                         |
| `overwrite` | `mc_set()` of an existing key, `N_OVW` times   | `mcl_set()` by handle, `N_OVW` times: one `update`, no intent |
| `remove`    | `mc_del()` of every key                        | `mcl_remove()` of every node, from the front                |
| `cleanup`   | `mc_destroy()` + `rootreg_unregister()`        | `mcl_destroy()` + `rootreg_unregister()`                    |
| `next`      | —                                              | one forward step, wrapping through the root, `N_GET` times  |
| `prev`      | —                                              | one backward step, `N_GET` times                            |

`insert` and `remove` run the full crash-safe discipline in both containers:
5 steps in the map, 6 in the list (its extra step is REPAIR). Both walks
check the order against the handles as they go. `list.get` keeps a checksum,
as `map.get` does, and `mcl_check` runs after `list.insert` and
`list.remove`.

Every phase prints an `io` line after its `bench` line when
`CONFIG_BLOB_DB_IOSTATS=y`, which `boards/native_sim.conf` sets. The
native_sim board now also takes the DK's geometry (64 KB erase blocks, 4 B
writes, the same 8 MB partition), as `app_perf_blob_db` does, so the counters
are the ones the board issues.

### Setup

- **Target**: nRF5340-DK, cpuapp core, `storage_partition` on the on-board
  MX25R6435F QSPI NOR (8 MB, 64 KB sectors).
- **Code**: `aef48ea` on main (`e78f04c`), **plus PR 49's driver patch**,
  which puts the MX25R in high-performance mode. Every blob_db buffer is
  word-aligned (PR 55, checked with `nm`). `aef48ea` printed the same phases
  under their earlier names (`set`, `delete`, `l.set`, …); the tables use
  the current ones.
- **Builds**: six. The UBI and `flash_area` backends, each plain, with
  `CONFIG_BLOB_DB_IOSTATS=y`, and with `CONFIG_BLOB_DB_SLOT_INDEX=y` (default
  2048 entries). The tables use the plain and slot-index builds. IOSTATS
  moves timings by 1–2% (2–13% on `list.remove` and `list.cleanup`, which are
  10 ops and 1 op).
- One `erase_all` + `blob_db_prepare(150)` up front. `prepare` costs 520 ms
  per block in every build, against 1.09 s in the v1.0.0 captures.

**PR 49 is not on main.** A build from main leaves the part in its power-up
ultra-low-power mode. These runs cannot say how much of the change since
v1.0.0 that mode accounts for and how much is alignment.

### Correctness on hardware

All six builds pass:

- no `<err>` lines;
- `map.get checksum` and `list.get checksum` both `0xac6a5f02`;
- `mcl_check` passes after `list.insert` and `list.remove`;
- 1 live blob after the map and at the end.

The DK's `io` lines equal the native_sim capture below for every map and list
phase, transaction for transaction. UBI issues the same counters as
`flash_area`; only `prepare` differs (117 blocks against 124). So a native_sim
run on this board's overlay reproduces the board's I/O exactly.

### Numbers

ms per op, measured on the DK. The last column is how many times cheaper the
list is than the map, on `flash_area`:

| operation   | map, `flash_area` | map, UBI | list, `flash_area` | list, UBI | map / list |
| ----------- | ----------------: | -------: | -----------------: | --------: | ---------: |
| `insert`    |              7.40 |     8.40 |               8.90 |     10.20 |      ×0.83 |
| `get`       |              3.41 |     3.90 |               0.77 |      0.89 |   **×4.4** |
| `overwrite` |             23.38 |    27.08 |               4.22 |      4.90 |   **×5.5** |
| `remove`    |             37.20 |    43.20 |              17.30 |     20.40 |       ×2.2 |
| `cleanup`   |              29.0 |     34.0 |               12.0 |      15.0 |       ×2.4 |
| `next`      |                 — |        — |               0.48 |      0.55 |          — |
| `prev`      |                 — |        — |               3.14 |      3.66 |          — |

Flash transactions per op, from the `io` lines. These are not blob
operations: one blob read walks the 12-byte slot headers of its bucket, so it
costs several transactions.

| operation   | map reads | map writes | list reads | list writes |
| ----------- | --------: | ---------: | ---------: | ----------: |
| `insert`    |      82.5 |          5 |      101.2 |           6 |
| `get`       |      47.5 |          0 |       10.9 |           0 |
| `overwrite` |     330.5 |          5 |       58.9 |           1 |
| `remove`    |     561.0 |          5 |      236.9 |           6 |
| `cleanup`   |       452 |          3 |        187 |           3 |
| `next`      |         — |          — |        6.9 |           0 |
| `prev`      |         — |          — |       47.9 |           0 |

UBI costs ×1.14–×1.18 over `flash_area` in every phase of both containers
except the single-op `list.cleanup` (×1.25), where a 1 ms tick is 8%. The
v1.0.0 captures put that band at ×1.5–2.4. Those builds were unaligned and in
low-power mode, so that band no longer describes a current build.

What the numbers show:

- **`get`: the list's handle access is O(1); the map's key lookup is not.**
  `list.get` is two blob reads (node, then value) against the map's ~7, and
  measures ×4.4 cheaper. That gap grows with population, because the map
  scans its pair list. The list's own price shows up elsewhere: there is no
  lookup by key, and the client holds the handles.
- **`overwrite`: the list needs no intent.** `list.overwrite` changes no
  references, so it is one atomic `update` of the value blob after a
  membership check (`node_linked`). `map.overwrite` replaces the value blob
  through the full discipline (4 updates + 1 delete). At 1 write against 5,
  the list is ×5.5 cheaper.
- **`insert`: the list pays one more write.** That is the REPAIR step: 6
  writes (stage, value, node, commit, repair, clear) against the map's 5.
  Most of the reads are read-modify-writes: every link write reloads the blob
  it changes. With `mcl_begin()`'s recovery check on top, an append at the
  back reads the root four times. `list.insert` costs ×1.2 a `map.insert`,
  the only operation where the list is dearer.
- **`prev` costs ×6.5 a `next`**, while both read one node. `mcl_prev` first
  runs recovery to rule out a staged mutation, which reads the root and the
  intent. Those two blobs are the most rewritten in the store, since every
  mutation updates the intent twice (stage and clear). So each read walks a
  long slot history in its bucket. The cost comes from the store's history,
  not from the list.
- **Both containers end at the floor.** Each leaves 1 live blob (the empty
  registry), and the two `get` checksums are equal because both read the
  same value sequence.

### With `CONFIG_BLOB_DB_SLOT_INDEX=y`

The slot index (PR 54) is opt-in. It removes the slot-header walk that
dominates the reads above. Writes per op are unchanged. ms per op on the DK,
and the speed-up over the plain build on `flash_area`:

| operation   | map, fa | map, UBI | map speed-up | list, fa | list, UBI | list speed-up |
| ----------- | ------: | -------: | -----------: | -------: | --------: | ------------: |
| `insert`    |    3.70 |     4.10 |         ×2.0 |     4.00 |      4.40 |          ×2.2 |
| `get`       |    1.74 |     1.98 |         ×2.0 |     0.43 |      0.50 |          ×1.8 |
| `overwrite` |    4.52 |     4.96 |         ×5.2 |     1.30 |      1.46 |          ×3.2 |
| `remove`    |    3.50 |     4.50 |        ×10.6 |     4.40 |      4.90 |          ×3.9 |
| `cleanup`   |     1.0 |      3.0 |          ×29 |      2.0 |       2.0 |          ×6.0 |
| `next`      |       — |        — |            — |     0.22 |      0.25 |          ×2.2 |
| `prev`      |       — |        — |            — |     0.66 |      0.75 |          ×4.8 |

Flash reads per op with the index:

| operation   |  map | list |
| ----------- | ---: | ---: |
| `insert`    | 24.5 | 24.0 |
| `get`       | 22.5 |  6.0 |
| `overwrite` | 30.5 | 13.0 |
| `remove`    | 26.0 | 33.0 |
| `cleanup`   |   18 |   21 |
| `next`      |    — |  3.0 |
| `prev`      |    — |  9.0 |

With the index a blob read costs 3 flash reads wherever it lives, so
`list.prev` falls to exactly three blob reads (root, intent, node) against
`list.next`'s one. The gains are largest where the blobs touched are oldest,
which is where the walk was longest. The `cleanup` rows are single ops timed
to 1 ms, so their multiples are rough. The index helps the map more than
the list, because the map's blobs carry longer slot histories. With it, the
list stays cheaper only on `get` (×4.0) and `overwrite` (×3.5). The map is
now the cheaper one on `insert`, `remove` and `cleanup`.

### The timing model, checked

Before the board run, this section predicted the DK from the native_sim
counters with `app_perf_l0/tools/l0_timing.py predict` over
`models/mx25r64_nrf5340dk_full.json`. That covers flash time only, with no
CPU time and no UBI per-transaction cost. Against `flash_area`, ms per op,
"ratio" being measured / predicted:

| operation   | map predicted | map measured | ratio | list predicted | list measured | ratio |
| ----------- | ------------: | -----------: | ----: | -------------: | ------------: | ----: |
| `insert`    |          9.60 |         7.40 |  0.77 |          11.71 |          8.90 |  0.76 |
| `get`       |          3.46 |         3.41 |  0.99 |           0.79 |          0.77 |  0.97 |
| `overwrite` |         28.38 |        23.38 |  0.82 |           4.92 |          4.22 |  0.86 |
| `remove`    |         43.65 |        37.20 |  0.85 |          20.94 |         17.30 |  0.83 |
| `cleanup`   |         33.83 |         29.0 |  0.86 |          14.70 |          12.0 |  0.82 |
| `next`      |             — |            — |     — |           0.50 |          0.48 |  0.96 |
| `prev`      |             — |            — |     — |           3.47 |          3.14 |  0.91 |

With the slot index:

| operation   | map predicted | map measured | ratio | list predicted | list measured | ratio |
| ----------- | ------------: | -----------: | ----: | -------------: | ------------: | ----: |
| `insert`    |          5.40 |         3.70 |  0.69 |           6.12 |          4.00 |  0.65 |
| `get`       |          1.64 |         1.74 |  1.06 |           0.43 |          0.43 |  0.99 |
| `overwrite` |          6.68 |         4.52 |  0.68 |           1.59 |          1.30 |  0.82 |
| `remove`    |          4.97 |         3.50 |  0.70 |           6.19 |          4.40 |  0.71 |
| `cleanup`   |          2.45 |          1.0 |  0.41 |           2.69 |           2.0 |  0.74 |
| `next`      |             — |            — |     — |           0.22 |          0.22 |  1.01 |
| `prev`      |             — |            — |     — |           0.66 |          0.66 |  1.01 |

The model gets reads right and writes too high:

- **Read-only phases** (`get`, `next`, `prev`) land within 1–10%. With the
  index, the list's three land within 1% and `map.get` within 6%.
- **Phases with writes** measure 14–24% under the prediction, and 18–59%
  under it with the index, where writes are a larger share of the time.

The model was fitted from `app_perf_l0` captures taken before PR 55, in the
part's low-power mode. Unaligned buffers made `nrf_qspi_nor` program 4 bytes
at a time, and low-power mode programs and erases more slowly. Either could
explain why writes are now cheaper than the model says, and these runs cannot
separate the two. Refitting the model from an aligned `app_perf_l0` run in
the same mode as the board should close the gap.

### Raw native_sim capture (`flash_area`, DK geometry)

Wall clock is 0 ms throughout because the flash simulator models no latency.
The `io` lines are the result, and the DK's IOSTATS build prints the same
ones.

```
model-container perf 1.1.0  (N_KEYS=10  N_NODES=10  N_GET=100  N_OVW=50  VAL_LEN=24)
bench prepare       :  124 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io prepare       : rd    124 ops/    1984 B   wr   124 ops/    1984 B   er  124 ops/ 8126464 B
bench map.insert    :   10 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io map.insert    : rd    825 ops/   10035 B   wr    50 ops/    2320 B   er    0 ops/       0 B
bench map.get       :  100 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io map.get       : rd   4750 ops/   65850 B   wr     0 ops/       0 B   er    0 ops/       0 B
bench map.overwrite :   50 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io map.overwrite : rd  16525 ops/  202825 B   wr   250 ops/   15000 B   er    0 ops/       0 B
bench map.remove    :   10 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io map.remove    : rd   5610 ops/   67660 B   wr    50 ops/    1880 B   er    0 ops/       0 B
bench map.cleanup   :    1 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io map.cleanup   : rd    452 ops/    5394 B   wr     3 ops/      56 B   er    0 ops/       0 B
map.get checksum: 0xac6a5f02
live blobs after map: 1 (expect 1 — the empty registry alone)
bench list.insert   :   10 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io list.insert   : rd   1012 ops/   12864 B   wr    60 ops/    2800 B   er    0 ops/       0 B
bench list.get      :  100 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io list.get      : rd   1090 ops/   14280 B   wr     0 ops/       0 B   er    0 ops/       0 B
bench list.overwrite:   50 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io list.overwrite: rd   2945 ops/   37340 B   wr    50 ops/    2000 B   er    0 ops/       0 B
bench list.next     :  100 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io list.next     : rd    691 ops/    8892 B   wr     0 ops/       0 B   er    0 ops/       0 B
bench list.prev     :  100 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io list.prev     : rd   4791 ops/   60492 B   wr     0 ops/       0 B   er    0 ops/       0 B
bench list.remove   :   10 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io list.remove   : rd   2369 ops/   29248 B   wr    60 ops/    2320 B   er    0 ops/       0 B
bench list.cleanup  :    1 ops in       0 ms  ->    0.000 ops/s  (        0 us/op)
   io list.cleanup  : rd    187 ops/    2308 B   wr     3 ops/      56 B   er    0 ops/       0 B
list.get checksum: 0xac6a5f02
live blobs at end: 1 (expect 1 — the empty registry alone)
```

## The map on hardware (v1.0.0)

**Stale: superseded by the v1.1.0 numbers above.** The captures below
predate word-aligned buffers (PR 55) and ran the MX25R in low-power mode. For
example, `set` (now `map.insert`) measured 13.60 ms here and 7.40 ms in v1.1.0, and UBI cost
×1.5–2.4 here against ×1.14–×1.18 now. What still holds is the shape: what
each operation is made of, and the two values of UBI `prepare`.

v1.0.0 named the map's phases without a prefix: `set` is `map.insert`, `get` is `map.get`, `overwrite` is
`map.overwrite`, `delete` is `map.remove`, `cleanup` is `map.cleanup`.

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
sized for this geometry in `boards/nrf5340dk_nrf5340_cpuapp.conf`. The v1.1.0
figures also need PR 49's driver patch (`west patch apply`) for the MX25R's
high-performance mode. Without it, expect slower writes and erases. The DK build
leaves `CONFIG_BLOB_DB_IOSTATS` off, so its timings stay comparable with the
v1.0.0 captures above. A v1.1.0 capture prints the `list.*` phases after the `map.*` ones. For the
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
