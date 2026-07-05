# app_perf_mc — reference results

Hardware-measured ops/sec for the model container (the L1 sufficiency-proof
key->value map from `tests/lib/blob_db_contract/`), with its list root
resolved through the root registry at id 1. Update this file when the
benchmark shape changes or when regenerating on new hardware.

## Setup

- **Target**: nRF5340-DK (S/N 960115021, PCA10095), cpuapp core
- **Storage**: `storage_partition` on the on-board MX25R6435F QSPI NOR
  (8 MB, 64 KB sectors, 8 MHz Quad-SPI)
- **Config**: `N_KEYS = 10`, `N_GET = 100`, `N_OVW = 50`, `VAL_LEN = 24 B`
- **Zephyr**: v4.4.99 (`34bd8ff000cc`), SDK `zephyr-sdk-1.0.1`
- One `erase_all` + `blob_db_prepare(150)` up front (capped to the 124
  non-root buckets), so every timed op runs on the warm append-only path.

## Numbers

| workload  | ops/s | ms/op | blob_db ops per mc op (approx) |
| --------- | ----: | ----: | ------------------------------ |
| set       | 4.248 |   235 | ~2 reads + 4 updates + 0–1 del |
| get       | 6.084 |   164 | ~7 reads (list + key scan + value) |
| overwrite | 3.801 |   263 | ~2 reads + 4 updates + 1 delete |
| delete    | 4.743 |   211 | ~2 reads + 2 updates + 2 deletes |
| cleanup   |     — |   119 | mc_destroy (2 deletes on an emptied container) + rootreg_unregister |

For scale: raw blob_db on the same setup does ~59 reads/s (17 ms) and
~29 updates/s (34 ms) — see `app_perf/RESULTS.md`. The model container's
multiples follow directly from its design:

- **get** walks the pair list key-by-key (O(n) — each probe is a full
  17 ms bucket read), averaging ~5.5 key reads for 10 keys plus the list
  and value reads: ~164 ms observed, right on the predicted cost.
- **set/overwrite/delete** pay the full 5-step crash-safe mutation
  discipline (stage intent, prepare blobs, commit list swap, cleanup,
  clear intent) — 4–6 flash writes per logical operation, i.e. roughly
  1/6th of raw update throughput. That factor is the measured price of
  "crash-safe at every step + zero permanent leak", not overhead to be
  optimized here: the model container is deliberately naive
  (`l1_model_container.md` §1), and the real L2 containers reduce both
  the O(n) key scan and the per-mutation write count.

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
*** Booting Zephyr OS build 34bd8ff000cc ***
model-container perf 1.0.0  (N_KEYS=10  N_GET=100  N_OVW=50  VAL_LEN=24)
bench prepare  :  124 ops in  137312 ms  ->    0.903 ops/s  (  1107354 us/op)
bench set      :   10 ops in    2354 ms  ->    4.248 ops/s  (   235400 us/op)
bench get      :  100 ops in   16436 ms  ->    6.084 ops/s  (   164360 us/op)
bench overwrite:   50 ops in   13153 ms  ->    3.801 ops/s  (   263060 us/op)
bench delete   :   10 ops in    2108 ms  ->    4.743 ops/s  (   210800 us/op)
bench cleanup  :    1 ops in     119 ms  ->    8.403 ops/s  (   119000 us/op)
get checksum: 0xac6a5f02
live blobs at end: 1 (expect 1 — the empty registry alone)
```

native_sim run (timing-free, correctness reference): same checksum
`0xac6a5f02`, same end state of 1 live blob.

## Reproducing

```bash
west build -p always -b nrf5340dk/nrf5340/cpuapp app_perf_mc
nrfutil device program --firmware build/zephyr/zephyr.hex \
    --options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE,reset=RESET_SYSTEM \
    --serial-number <your-jlink-sn>
# Console on VCOM 2 (/dev/ttyACM2), 115200; then reset:
nrfutil device reset --serial-number <sn> --reset-kind RESET_SYSTEM
```

Full run ≈ 3 min prepare + ~35 s of timed phases.
