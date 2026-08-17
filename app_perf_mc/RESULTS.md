# app_perf_mc — reference results

Hardware-measured ops/sec for the model container (the L1 sufficiency-proof
key->value map from `tests/lib/blob_db_contract/`), with its list root
resolved through the root registry at id 1. Update this file when the
benchmark shape changes or when regenerating on new hardware.

Regenerated on `e80f404` (main, after PR 10 merged). The previous numbers
predated PR 2's lookup change; the "~59 reads/s (17 ms)" this file used to
scale against is now 2174 reads/s (460 µs).

**`blob_db` now defaults to the UBI backend, and the headline table below is
`flash_area`.** Both are measured — see "On the UBI backend". The default
costs roughly 2× on reads, so the numbers a caller should expect from a
default build are the UBI column, not the first table.

## Setup

- **Target**: nRF5340-DK (S/N 960115021, PCA10095), cpuapp core
- **Storage**: `storage_partition` on the on-board MX25R6435F QSPI NOR
  (8 MB, 64 KB sectors, 8 MHz Quad-SPI)
- **Config**: `N_KEYS = 10`, `N_GET = 100`, `N_OVW = 50`, `VAL_LEN = 24 B`
- **Code**: `e80f404`; **Zephyr** build `4a405846193f`, SDK
  `zephyr-sdk-1.0.1`
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
differs, taken from the board conf rather than a command-line override.

| workload  | `flash_area` |     UBI |     Δ |
| --------- | -----------: | ------: | ----: |
| `get`     |      3.53 ms | 8.59 ms | ×2.43 |
| `set`     |     13.60 ms | 20.9 ms | ×1.54 |
| `overwrite` |   29.10 ms | 56.5 ms | ×1.94 |
| `delete`  |     37.00 ms | 83.8 ms | ×2.26 |
| `cleanup` |     23.00 ms | 56.0 ms | ×2.43 |
| `prepare` |  1 088 620 µs | 1 088 542 µs | — |

`get checksum: 0xac6a5f02` and the 1-live-blob end state hold on both
backends, so the container behaves identically; only the substrate's cost
changes.

The spread across rows is explained by what each operation is made of.
`get` is ~7 reads and pays UBI's per-transaction penalty on every one, so
it takes the full ×2.4. `set` is dominated by 4 updates whose cost is
program time rather than transactions, so it only takes ×1.5. That matches
`app_perf/RESULTS.md`, which fits the penalty at ~112 µs per flash
transaction and nothing per byte.

`prepare` is **identical** to the byte, and worth dwelling on because
`app_perf_kvdb/RESULTS.md` shows the same phase 865× *faster* on UBI. There
is no contradiction: this app calls `erase_all()` and then `prepare()` on a
volume that has been in use, so UBI has no pre-erased PEBs left and each
bucket format forces a real sector erase. kvdb's first run formats the store
immediately beforehand, so its `prepare` finds PEBs UBI has already erased.
**UBI moves erase cost in time; it does not remove it.**

Note also that `prepare` formats **118** buckets here against 124 on
`flash_area` — UBI keeps 2 PEBs for its headers and the volume is smaller by
that much, so the usable bucket count drops.

### Where the multiples come from

For scale, raw blob_db on the same setup now does ~2174 reads/s (460 µs)
and ~398 updates/s (2.51 ms) — see `app_perf/RESULTS.md`.

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
  appreciably more than an update; `app_perf` does not benchmark delete,
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

## Raw UART capture — UBI backend (the default)

UBI's volume-probe and PEB-recovery lines are elided; it logs recoverable
conditions at `<err>` level.

```
*** Booting Zephyr OS build 4a405846193f ***
model-container perf 1.0.0  (N_KEYS=10  N_GET=100  N_OVW=50  VAL_LEN=24)
bench prepare  :  118 ops in  128448 ms  ->    0.918 ops/s  (  1088542 us/op)
bench set      :   10 ops in     209 ms  ->   47.846 ops/s  (    20900 us/op)
bench get      :  100 ops in     859 ms  ->  116.414 ops/s  (     8590 us/op)
bench overwrite:   50 ops in    2826 ms  ->   17.692 ops/s  (    56520 us/op)
bench delete   :   10 ops in     838 ms  ->   11.933 ops/s  (    83800 us/op)
bench cleanup  :    1 ops in      56 ms  ->   17.857 ops/s  (    56000 us/op)
get checksum: 0xac6a5f02
live blobs at end: 1 (expect 1 — the empty registry alone)
```

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

Start the console capture *before* programming and let the reset above
start the run. Resolve the port with `nrfutil device list` and take the one
labelled `vcom: 2` — the `/dev/ttyACM*` number is not stable. Attach
exactly one reader: two concurrent `cat`s split the byte stream and
silently shred the capture.

This app does not enable `CONFIG_BLOB_DB_LARGE_PAYLOADS`, so it cannot
mount a store left behind by `app_perf` (format major 2 against major 1)
and will fail `-ENOTSUP`. Erase the partition first; see the
"Downgrading" section of `app_perf/RESULTS.md`.

Full run ≈ 2.3 min prepare + ~2.3 s of timed phases. The prepare is now
98% of the wall clock.
