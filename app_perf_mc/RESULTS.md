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
`app_perf/RESULTS.md`, which fits the penalty at ~112 µs per flash
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
sized for this geometry in `boards/nrf5340dk_nrf5340_cpuapp.conf`. For the
`flash_area` column add `-DCONFIG_BLOB_DB_BACKEND_FLASH_AREA=y`, and **erase
the partition raw when switching between backends** — the two layouts are not
interchangeable, and UBI only formats a partition it finds erased.

Start the console capture *before* programming and let the reset above
start the run. Resolve the port with `nrfutil device list` and take the one
labelled `vcom: 2` — the `/dev/ttyACM*` number is not stable. Attach
exactly one reader: two concurrent `cat`s split the byte stream and
silently shred the capture.

This app does not enable `CONFIG_BLOB_DB_LARGE_PAYLOADS`, so it cannot
mount a store left behind by `app_perf` (format major 2 against major 1)
and will fail `-ENOTSUP`. Erase the partition first; see the
"Downgrading" section of `app_perf/RESULTS.md`.

Wall clock depends entirely on what state the volume is in. On a raw-erased
partition the whole run is **~4 s** (0.15 s prepare + ~3.8 s of timed phases);
on a volume already in use it is **~2.2 min**, of which prepare is 98%. Both
are in the captures above. Erase the partition first if you want the numbers
to be comparable between sessions.
