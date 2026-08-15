# app_perf_mc — reference results

Hardware-measured ops/sec for the model container (the L1 sufficiency-proof
key->value map from `tests/lib/blob_db_contract/`), with its list root
resolved through the root registry at id 1. Update this file when the
benchmark shape changes or when regenerating on new hardware.

Regenerated on `e80f404` (main, after PR 10 merged). The previous numbers
predated PR 2's lookup change; the "~59 reads/s (17 ms)" this file used to
scale against is now 2174 reads/s (460 µs).

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

## Raw UART capture

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
