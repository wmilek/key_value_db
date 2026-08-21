# `app_perf_l0` — reference results

Results for the sweep implemented by `app_perf_l0/src/main.c` and the model
fitted from it by `tools/l0_timing.py`. Keep this file honest — update it when
the shape of the sweep changes or when regenerating on new hardware.

**Status of the hardware measurement: not yet taken.** §4 states what the
board run is predicted to produce and what would falsify it, so the run has
something to disagree with. Everything in §1–§3 is measured or computed and
reproducible from this repository today.

## Setup

- **Host runs**: `native_sim`, Zephyr build `5058917ea61b`, host toolchain
- **Board the derived model describes**: nRF5340-DK (PCA10095) cpuapp, with
  `storage_partition` on the on-board MX25R6435F QSPI NOR — 8 MB, 64 KB erase
  blocks, `flash_area_align()` = 4, 8 MHz Quad-SPI. Same board and same
  captures as `app_perf/RESULTS.md`.

---

## 1. The fitter, checked against a substrate whose costs are known exactly

The flash simulator charges one flat cost per call — `k_busy_wait()` of
`CONFIG_FLASH_SIMULATOR_MIN_{READ,WRITE,ERASE}_TIME_US`, with no per-byte and
no per-block term. That makes a `native_sim` run a test with a known answer:
whatever the sweep does, the fit must come back with exactly those three
constants and three zero slopes.

```
west build -b native_sim key_value_db/app_perf_l0 && ./build/zephyr/zephyr.exe > l0_sim.log
python3 app_perf_l0/tools/l0_timing.py fit l0_sim.log --name native_sim_default -v
```

```
model: native_sim_default   source=flash_simulator timing=simulated board=native_sim/native
geometry: partition 8388608 B, 2048 blocks of 4096 B, write align 1 B

  class   fixed per call        per unit            points  max rel err
  read         2.000 us         0.0 ns/B       13       0.0 %
  write      100.000 us         0.0 ns/B       13       0.0 %
  erase     2000.000 us       0.000 ms/blk      7       0.0 %
  note (read): per-unit cost fitted negative; clamped to a flat per-call cost
  note (write): per-unit cost fitted negative; clamped to a flat per-call cost
```

Exact, at 0.0 % residual on all 33 points, against the configured
`READ=2 / WRITE=100 / ERASE=2000` µs. The two clamp notes are the mechanism
working as intended: on a perfectly flat curve the slope fits to a tiny
negative number, and a negative cost per byte would silently subtract time from
every prediction, so it is clamped and the model refitted as flat.

The erase sweep says something else worth reading:

```
l0raw op=erase  blocks=1  ops=3  ns_per_op=2000000  ns_per_block=2000000
l0raw op=erase  blocks=32 ops=3  ns_per_op=2000000  ns_per_block=62500
l0raw op=erase1 blocks=1  ops=32 ns_per_op=2000000
```

Erasing 32 blocks in one call costs the same as erasing one — the simulator
charges per *call*. That is a true statement about the simulator and a
dangerous one to carry to hardware, which is exactly why the sweep measures
erase span rather than assuming it.

**Run time**: 2.2 s, exits on its own.

## 2. The geometry correspondence, verified

A prediction is only as good as the operation counts fed into it, and those are
a function of geometry: `blob_db` derives its bucket count, slot alignment and
segment size from what it finds at mount. The claim behind
`geometry/mx25r64.overlay` is that a `native_sim` build carrying the DK's shape
issues *the same operations* the DK issues.

That claim is checkable against `app_perf/RESULTS.md`, and it holds — all
twelve phases, byte for byte:

| phase | `native_sim` + `geometry/mx25r64.*` | DK `e80f404` |
|---|---|---|
| `read` | 605 rd / 8650 B | **identical** |
| `update` (prepend) | 1000 rd / 14800 B, 100 wr / 4800 B | **identical** |
| `update` (append) | 1198 rd / 17176 B, 100 wr / 4800 B | **identical** |
| `lg write` | 168 rd / 2576 B, 269 wr / 269584 B, 133 er | **identical** |
| `lg rewrite` | 1048 rd / 15744 B, 277 wr / 269712 B, 9 er | **identical** |
| `lg read` | 30755 rd / 8742276 B | **identical** |
| `lg pread q0..q3` | 710 / 714 / 727 / 727 rd | **identical** |
| `lg pwrite` | 1651 rd / 99708 B, 160 wr / 77912 B, 64 er | **identical** |

Without the overlay the same build mounts on 4 KB blocks, gets 2048 buckets
instead of 125 and a different segment chunk, and its counts describe a device
nobody owns. The check is not left to discipline: `blob_db`'s geometry line is
parsed out of the run log and compared against the model's, and
`--strict-geometry` makes a mismatch fatal.

## 3. A provisional model for the DK, and what it is worth

No `app_perf_l0` run has been taken on the board yet, but three `app_perf`
captures on the same board have — and each of their phases is one equation in
the six coefficients. `models/observations_nrf5340dk_flash_area.txt` holds
those captures verbatim; `derive` solves them.

```
python3 app_perf_l0/tools/l0_timing.py derive \
    app_perf_l0/models/observations_nrf5340dk_flash_area.txt \
    --block-bytes 65536 --part-bytes 8388608 --write-align 4 \
    --board nrf5340dk/nrf5340/cpuapp --name mx25r64_nrf5340dk_derived --loo \
    -o app_perf_l0/models/mx25r64_nrf5340dk_derived.json
```

| class | fixed per call | per unit | implied |
|---|---:|---:|---|
| read | 67.7 µs | 631 ns/B | 1548 KB/s ceiling; a read pays mostly overhead below **107 B** |
| write | 314 µs | 29.6 µs/B | 33 KB/s ceiling; overhead dominates only below **11 B** |
| erase | — | **1106 ms/block** | matches the ~1.07 s sector erase `app_perf/RUN_ON_DK.md` reports |

Fitted against **36 observations spanning 46 ms to 155 s** — three and a half
orders of magnitude — with a **median residual of 1.4 %** and a worst of 8.6 %.
Leave-one-out cross-validation (refit without a phase, then predict it) barely
degrades that: **median 1.4 %, worst 9.1 %**. The model is not memorising its
inputs.

The single worst phase in both directions is `lg pread q2`, under-predicted by
8 %, and it is under-predicted identically in all three captures — a systematic
term the affine model does not carry, not noise.

Two properties of this fit are structural rather than incidental:

- **The erase cost is attributed per block, not per call.** Every observation
  erased exactly one block per call, so the two terms are indistinguishable in
  this data. Attributing to the block is the safe half of that choice: a model
  that put the cost on the call would predict `blob_db_erase_all()` — one call
  over 128 blocks — as costing one block erase. The tool says so in the model's
  `notes`.
- **The fixed terms are upper bounds.** `derive` cannot separate flash time
  from the CPU time above it, so `blob_db`'s slot scan and CRC are folded into
  `R0` and `W0`. This is the main thing the hardware run in §4 will correct.

## 4. What the model predicts — and what the board run must show

Feeding the §2 `native_sim` run (which reports `0 ms` for every phase) through
the §3 model reproduces the DK's wall-clock:

```
phase             reads/bytes        writes/bytes      erases/blocks       read      write      erase        TOTAL
read                605/8650            0/0              0/0             46.4m       0.0m       0.0m     46.433 ms
update             1000/14800         100/4800           0/0             77.1m     173.3m       0.0m    250.375 ms
lg write            168/2576          269/269584       133/133           13.0m    8052.1m  147140.0m    155.205 s
lg rewrite         1048/15744         277/269712         9/9             80.9m    8058.4m    9956.8m     18.096 s
lg read           30755/8742276         0/0              0/0           7598.9m       0.0m       0.0m      7.599 s
lg pwrite          1651/99708         160/77912         64/64           174.7m    2353.0m   70804.2m     73.332 s
```

against measured `46 ms`, `251 ms`, `154.4 s`, `17.9 s`, `7.5 s`, `72.7 s`.
`verify` scores the whole set at a **median ratio of 1.01×**, worst 0.91×.

That agreement is *partly circular* — the model was solved from these same
phases — and the leave-one-out figure in §3 is the non-circular version of it.
What is not circular at all is the decomposition, which no stopwatch on the
board could have produced:

- `lg write` is **95 % erase**. Every microsecond of write-path tuning is
  chasing 5 % of it. The phase is saying *erase less*, not *write faster*.
- `lg rewrite` is 55 % erase / 45 % write, which is why it is 8.5× cheaper than
  `lg write` on identical write traffic: it erases 9 blocks instead of 133.
- `update` is 69–74 % write on 4800 B — 100 slot writes, each paying a fixed
  cost plus a page program.

### The falsifiable part

When `app_perf_l0` is run on the DK, its direct fit must agree with §3 on the
terms that are pure flash, and should come in **lower** on the terms that
absorbed CPU time:

| coefficient | derived (§3) | direct L0 fit should be | if it is not |
|---|---:|---|---|
| erase per block | 1106 ms | within a few % — `blob_db` adds no CPU to an erase | the captures' erases were not one block each after all |
| read per byte | 631 ns/B | within a few % | the read path's per-byte CPU is not negligible |
| read fixed | 67.7 µs | **lower** — the difference is `blob_db`'s per-read CPU | the QSPI per-call overhead really is that large (DPD exit is ~35 µs, so ~40–70 µs is plausible on its own) |
| write fixed | 314 µs | **lower**, same reason | as above |
| write per byte | 29.6 µs/B | within a few %, i.e. ~7.6 ms per 256 B page | slower than the MX25R64 typical page program by ~4×; either the part is running near its worst case or some of this is not page-program time |

That last row is the one to watch. 29.6 µs/B is well above the datasheet's
typical page-program figure, and `derive` cannot tell whether that is the part,
the driver, or `blob_db` — only a direct sweep can. It is the clearest reason
to run this app on hardware rather than to keep deriving.

## 5. Feeding the simulator back

`simconf` turns a model into flash-simulator knobs, picking the operating point
from a run's actual operation mix:

```
# operating point: read at 232 B/op, write at 692 B/op, erase at 1.00 block(s)/call
CONFIG_FLASH_SIMULATOR_SIMULATE_TIMING=y
CONFIG_FLASH_SIMULATOR_MIN_READ_TIME_US=214
CONFIG_FLASH_SIMULATOR_MIN_WRITE_TIME_US=20762
CONFIG_FLASH_SIMULATOR_MIN_ERASE_TIME_US=1000000
# NOTE: the true erase cost is 1106 ms, above the Kconfig ceiling of 1 s; it is clamped.
```

This is a blunt instrument and the tool says so: the simulator has no per-byte
term, so every operation is charged the cost of the *average* one, and the
erase cost does not even fit in the Kconfig range. It is useful for pacing and
watchdog behaviour on a host build; it is not a substitute for `predict`,
which uses the whole model.

## Raw capture — `native_sim`, simulator defaults

```
l0geom part_bytes=8388608 block_bytes=4096 blocks=2048 write_align=1 erased_val=0xff region_blocks=32 max_xfer=4096 cycles_per_s=1000000 source=flash_simulator timing=simulated board=native_sim/native
l0raw op=erase blocks=1 ops=3 total_ns=6000000 ns_per_op=2000000 ns_per_block=2000000 min_ns=2000000 max_ns=2000000
l0raw op=erase blocks=2 ops=3 total_ns=6000000 ns_per_op=2000000 ns_per_block=1000000 min_ns=2000000 max_ns=2000000
l0raw op=erase blocks=4 ops=3 total_ns=6000000 ns_per_op=2000000 ns_per_block=500000 min_ns=2000000 max_ns=2000000
l0raw op=erase blocks=8 ops=3 total_ns=6000000 ns_per_op=2000000 ns_per_block=250000 min_ns=2000000 max_ns=2000000
l0raw op=erase blocks=16 ops=3 total_ns=6000000 ns_per_op=2000000 ns_per_block=125000 min_ns=2000000 max_ns=2000000
l0raw op=erase blocks=32 ops=3 total_ns=6000000 ns_per_op=2000000 ns_per_block=62500 min_ns=2000000 max_ns=2000000
l0raw op=erase1 blocks=1 ops=32 total_ns=64000000 ns_per_op=2000000 ns_per_block=2000000 min_ns=2000000 max_ns=2000000
l0raw op=read size=1 ops=4096 total_ns=8192000 ns_per_op=2000
... (sizes 2 .. 4096, all 2000 ns/op)
l0raw op=write size=1 ops=500 total_ns=50000000 ns_per_op=100000
... (sizes 2 .. 4096, all 100000 ns/op)
l0raw op=write_unaligned size=256 ops=64 total_ns=6400000 ns_per_op=100000
l0raw op=write_unaligned size=512 ops=64 total_ns=6400000 ns_per_op=100000
l0raw op=write_unaligned size=4096 ops=31 total_ns=3100000 ns_per_op=100000
l0end status=0
```
