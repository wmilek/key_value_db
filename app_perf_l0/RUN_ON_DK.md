# Running `app_perf_l0` on the nRF5340-DK

The operator's guide for the L0 sweep: what to run, what to expect while it
runs, and what to do with the capture afterwards. `RESULTS.md` holds the
numbers and the analysis.

> **This run destroys `storage_partition`.** The app erases and rewrites the
> first `CONFIG_APP_PERF_L0_REGION_BLOCKS` (default 32) of the partition's 128
> blocks, repeatedly, and erases them once more on the way out. Anything a
> previous app stored there is gone. That is the app working correctly, not a
> mistake — but do not run it against a device holding a store you care about.

- **Board**: nRF5340-DK (PCA10095), cpuapp core
- **Storage**: on-board MX25R6435F QSPI NOR, 8 MB, 64 KB erase blocks, 8 MHz
  Quad-SPI. `boards/nrf5340dk_nrf5340_cpuapp.overlay` moves
  `storage_partition` there, so internal flash is untouched.

## Build and flash

```bash
# From the west workspace top-dir
west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/dk_l0 key_value_db/app_perf_l0
nrfutil device program --firmware build/dk_l0/zephyr/zephyr.hex \
    --options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE,reset=RESET_SYSTEM \
    --serial-number <your-jlink-sn>
```

No backend flags: this app links no storage stack. There is no UBI variant of
this run and there should not be — the sweep exists to measure the substrate
UBI itself sits on.

Start the console capture **before** programming and let `reset=RESET_SYSTEM`
start the run. Do not send a second reset: it risks interrupting a QSPI block
erase.

```bash
nrfutil device list        # find the port labelled "vcom: 2"
stty -F /dev/ttyACM2 115200 raw -echo -icrnl -inlcr
cat /dev/ttyACM2 | tee dk_l0.log
```

Attach exactly **one** reader to the console. Two concurrent `cat`s on the same
tty split the byte stream and silently shred the capture — the run is fine, but
every number in the log is garbage. The `/dev/ttyACM*` number is not stable;
a silent console is usually the wrong node, not a dead board.

## What to expect while it runs

**About seven minutes, most of it silence.** A 64 KB block erase on this part
is ~1.07 s and the sweep performs roughly 220 of them; the erase phase alone is
around four minutes of apparent quiet between lines. That silence *is* the
measurement.

The phases arrive in this order, and each prints an `l0raw` record followed by
a human-readable line:

1. `-- erase: cost vs erase size --` — the long one. Spans of 1, 2, 4, 8, 16
   and 32 blocks, three repetitions each, then 32 single-block calls.
2. `-- read: cost vs transfer size --` — seconds. Programs two blocks first
   (untimed), then sweeps 1 B … 64 KB in 32 rows.
3. `-- write: cost vs transfer size --` — a minute or two; each sweep point
   erases the space it is about to consume.
4. `-- write: page-program staircase --` — page-aligned transfers around the
   256 B boundary; twenty seconds or so.
5. `-- write: page-straddle penalty --` — seconds to tens of seconds.
6. A final erase of the working region, then `l0end status=0`.

Each of 2–5 prints a matrix: size, ops, µs/op, KiB/s, ns/B and the marginal
cost. The last column is the one to read — see `README.md`. Sweeps 2 and 3
also print a one-line verdict on whether their curve is affine.

The app never exits — Zephyr's idle loop runs forever once `main()` returns —
so stop capturing after `l0end status=0`. A non-zero status means a phase
failed; the `<err>` line above it names which call and which errno.

### Shortening the run

The erase phase dominates, and it scales with both knobs:

```bash
west build … -- -DCONFIG_APP_PERF_L0_REGION_BLOCKS=8 -DCONFIG_APP_PERF_L0_ERASE_REPS=1
```

takes it to well under a minute. The cost is real: fewer erase-span points
means a weaker slope estimate, and one repetition per span means the spread —
which on NOR is not small — goes unmeasured. Report the knob values with the
numbers; they are echoed in the `l0geom` line.

## Confirming the geometry before trusting anything

The first two lines of output are the whole basis of the run:

```
l0geom part_bytes=8388608 block_bytes=65536 blocks=128 write_align=4 erased_val=0xff region_blocks=32 max_xfer=65536 program_page=256 cycles_per_s=32768 source=hardware timing=real board=nrf5340dk/nrf5340/cpuapp
partition 8388608 B, 128 blocks of 65536 B, write align 4, erased 0xff
```

`block_bytes=65536` is correct for this board and comes from
`CONFIG_NORDIC_QSPI_NOR_FLASH_LAYOUT_PAGE_SIZE` (default 65536), not from the
part's 4 KB sector. `write_align=4` is what `nrf_qspi_nor` reports. If either
differs, stop and say so — every number below depends on them, and the model
records them so a later `predict` can refuse a mismatched run.

`source=hardware` is what makes the fitted model usable for prediction;
`predict` rejects a model that does not carry it.

## Afterwards

```bash
python3 key_value_db/app_perf_l0/tools/l0_timing.py fit dk_l0.log \
    -o key_value_db/app_perf_l0/models/mx25r64_nrf5340dk.json -v
```

Read the residual table `-v` prints before using the model. Then:

- Check it against `RESULTS.md` §4, which states what the derived model
  predicts each coefficient will be and what a disagreement would mean.
- Check the write tables against `RESULTS.md` §4b, which predicts from the
  existing captures that **write on this part is affine in bytes** and that
  `write_pg` will therefore show *no* staircase. A clean staircase there
  falsifies the model in §3 and everything predicted from it, so it is the
  first thing to look at, not the last.
- Check it against the datasheet, which also tells you which mode the part
  powered up in:

  ```bash
  python3 …/l0_timing.py spec -m …/mx25r64_nrf5340dk.json \
      --spec key_value_db/app_perf_l0/models/mx25r6435f_datasheet.json
  ```

  `RESULTS.md` §5 has the expected outcome: everything inside the Ultra Low
  Power envelope, and the page program over the High Performance maximum. An
  `OVER MAX` in *both* modes means the measurement is picking up time that is
  not the part's — a driver retry or a busy-poll interval — and the model
  should not be published until that is understood.
- Re-check the model against reality on a workload it was not fitted to:

  ```bash
  python3 …/l0_timing.py verify -m …/mx25r64_nrf5340dk.json dk_perf.log
  ```

  with any `app_perf` hardware capture. A median ratio near 1.0 means the model
  transfers; consistently below 1.0 is CPU time above L0 that the model does
  not carry.
- Update `RESULTS.md`: replace §3's derived model with the measured one, keep
  the derived numbers alongside it for comparison, and record the board, the
  Zephyr build and the knob values.

## If something goes wrong

- **`l0end status=-2` (`-ENOENT`) or a `flash_area_open` error:** the board
  overlay did not apply, so there is no `storage_partition` on the QSPI.
- **`non-uniform sectors not supported`:** the partition spans a region whose
  page layout is not uniform. The app refuses rather than averaging over it.
- **A phase fails with `-EINVAL` on a write:** the transfer size is not a
  multiple of `write_align`. The sweep starts at `write_align` precisely to
  avoid this, so this means the reported alignment is not the enforced one.
- **Nothing on the console:** wrong `/dev/ttyACM*` node, or the net-core image
  was flashed instead of `cpuapp`.
- **Erase times that climb steadily across the run:** the part is not
  misbehaving; NOR erase time rises with wear, and a heavily re-run region will
  show it. Use a fresh region (`REGION_BLOCKS`) or note it with the results.
