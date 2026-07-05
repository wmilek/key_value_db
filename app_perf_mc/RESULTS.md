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
| set       | 4.251 |   235 | ~2 reads + 4 updates + 0–1 del |
| get       | 6.089 |   164 | ~7 reads (list + key scan + value) |
| overwrite | 3.804 |   263 | ~2 reads + 4 updates + 1 delete |
| delete    | 4.748 |   211 | ~2 reads + 2 updates + 2 deletes |

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

The registry itself adds nothing measurable to the phases: it is read once
in `mc_open()` and never touched again (`mc_get`/`mc_set` operate on the
resolved root id).

Leak check on hardware: after set + overwrite + delete of everything, the
store holds exactly 3 blobs — registry + list + intent, the structural
floor. The crash-discipline bookkeeping reclaims everything else.

## Raw UART capture

```
*** Booting Zephyr OS build 34bd8ff000cc ***
model-container perf 1.0.0  (N_KEYS=10  N_GET=100  N_OVW=50  VAL_LEN=24)
bench prepare  :  124 ops in  137958 ms  ->    0.898 ops/s  (  1112564 us/op)
bench set      :   10 ops in    2352 ms  ->    4.251 ops/s  (   235200 us/op)
bench get      :  100 ops in   16423 ms  ->    6.089 ops/s  (   164230 us/op)
bench overwrite:   50 ops in   13141 ms  ->    3.804 ops/s  (   262820 us/op)
bench delete   :   10 ops in    2106 ms  ->    4.748 ops/s  (   210600 us/op)
get checksum: 0xac6a5f02
live blobs at end: 3 (structural floor: registry + list + intent)
```

native_sim run (timing-free, correctness reference): same checksum
`0xac6a5f02`, same end state of 3 live blobs.

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
