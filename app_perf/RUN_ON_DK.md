# Running `app_perf` on the nRF5340-DK

`RESULTS.md` holds the numbers from the first board run, the raw captures, and
the exact `nrfutil` / console commands. This file is the operator's guide:
what to expect while it runs, and what tends to go wrong.

> ## Run 2 is wanted — one flash, no A/B
>
> **Commit:** the branch tip of `claude/blob-db-max-payload-increase-6qobv5`
> · **Board:** `nrf5340dk/nrf5340/cpuapp`
>
> The code under test is `8f5b16b`; commits after it on this branch change
> only documentation and CI, so take the tip and record its hash in the
> capture.
>
> PR 5 added a one-entry index cache. Its effect is measured on `native_sim`
> but **not on hardware**, and the `native_sim` ratio is known not to
> transfer — the DK's chunk is 2004 B against 1024, so its index record is
> half the size while its 64 KB buckets make the scan in front of that record
> far more expensive.
>
> **No second flash is needed.** The pre-cache numbers are already recorded at
> `8428e35`, and nothing between the two commits touches the read path (a
> format fix, a `printk`, and a test-only Kconfig that is off by default). So
> run `8f5b16b` once and compare against the table in `RESULTS.md`.
>
> ### What is predicted, and what would falsify it
>
> | line | recorded at `8428e35` | predicted at `8f5b16b` |
> |---|--:|--:|
> | `io lg read` ops | 92 135 | **~30 700** (~3×) |
> | `io lg read` bytes | 11 623 044 | **~3 285 000** (~3.5×) |
> | `io lg read` ampl | 44.33× | **~12.5×** |
> | `bench lg read` | 3254 µs/op | **~920 µs/op** |
> | `io lg pread q0..q3` ops | ~710–727 each | **~240 each**, still flat |
>
> The reasoning is that a windowed read does three bucket lookups and the
> cache removes two of them; that model reproduces the `native_sim` result
> exactly. **If the ops drop by ~3× but the bytes barely move**, the model is
> wrong about where the bytes go and the write-up in
> `doc/proposals/2026-08-09-large-payloads-cost.md` §3 needs revisiting — say
> so, that is a useful result, not a failed run.
>
> Also worth a glance: `partial vs whole-object write` now prints two
> decimals and should read **~1.95x** where it previously printed `1x`.
>
> Everything else in the run is unchanged and mainly serves as a
> regression check against the recorded table.

Re-run it when the shape of the benchmark changes, when `blob_db`'s I/O paths
change, or on different hardware. `RESULTS.md` is the file to update
afterwards.

- **Board measured**: nRF5340-DK (S/N 960115021, PCA10095), cpuapp core
- **Storage**: on-board MX25R6435F QSPI NOR, 8 MB, 64 KB sectors, 8 MHz
  Quad-SPI. The board overlay moves `storage_partition` there, so internal
  flash is untouched.

## What to expect while it runs

**5–15 minutes, with long silent stretches.** A 64 KB sector erase on this part
takes **~1.07 s** (measured), and the run performs several hundred: each
`bench prepare` line alone is ~100 erases, so ~100 s of apparent silence before
it prints. That silence *is* the measurement.

The app never exits — Zephyr's idle loop runs forever once `main()` returns — so
stop capturing after the final `lg objects intact (…)` line.

`/dev/ttyACM*` numbering is **not** stable; any other USB CDC device that
enumerates first shifts it. A silent console is usually the wrong node, not a
dead board. Start the capture *before* programming and let
`reset=RESET_SYSTEM` start the run; a second reset risks interrupting a QSPI
sector erase.

## Confirming the geometry

`prj.conf` pins `blob_db` to warnings so per-op messages cannot perturb the
timed loops, which also hides the two lines that confirm the geometry. To see
them, one throwaway build:

```sh
west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/dk_geom key_value_db/app_perf -- \
    -DCONFIG_BLOB_DB_LOG_LEVEL_INF=y -DCONFIG_APP_PERF_N_OPS=10 \
    -DCONFIG_APP_PERF_N_LARGE=1 -DCONFIG_APP_PERF_N_PART=4
```

Expect, near mount:

```
partition 8388608 B, 128 sectors of 65536 B, 125 buckets
segments: chunk 2004 B, up to 128 per object (max object 256512 B)
```

A chunk of **2004** is correct, not 16384: the auto rule starts at `sector / 4`
but clamps to what one slot can hold, and the index record is itself a
single-slot payload. Every other number depends on this, so if either line
differs, say so before trusting the run. **Take timings from the normal build,
not this one** — compaction also logs at INF, so this build's numbers are
perturbed.

## Reading the output

Each timed phase prints a `bench …` line and, with
`CONFIG_BLOB_DB_IOSTATS=y`, an `io …` line beneath it. The I/O counters are
deterministic, so they should match a `native_sim` run at the same geometry
exactly — that is what makes them usable as a regression gate, and it is worth
checking, because a mismatch means the two are not running the same code path.

Two lines carry most of the weight:

- **`bench lg pread q0..q3`** must be four *similar* numbers. Contract R2 says
  read cost is independent of offset; a rising series falsifies it. (Measured:
  3187 / 3250 / 3625 / 3281 µs — flat, with `q3` below `q2`.)
- **`partial vs whole-object write`** is the headline for why `blob_db_write`
  exists. It prints two decimals; at the default `PART_LEN=64` expect ~`1.95x`,
  because a 64 B partial write still pays two sector erases.

## Shorter runs

```sh
west build … -- -DCONFIG_APP_PERF_N_LARGE=2 -DCONFIG_APP_PERF_OBJ_LEN=32768
```

roughly quarters the large-object phases. Report the knob values used — every
number scales with them. Defaults are `OBJ_LEN=65536`, `N_LARGE=4`,
`N_PART=32`, `PART_LEN=64`, echoed in the `-- large objects (…) --` header.

## If something goes wrong

- **Mount fails with `-ENOTSUP` naming a payload cap versus a sector size:**
  the build's `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` does not suit the geometry. The
  message names both numbers.
- **Mount fails with `-ENOTSUP` about a foreign format:** the QSPI holds a
  store written by firmware using a different on-flash format major — what a
  downgrade across `CONFIG_BLOB_DB_LARGE_PAYLOADS` hits. The refusal is
  deliberate and nothing was destroyed; call `blob_db_format()` to discard it
  (no mount required). Downgrading to a build that predates that fix needs a
  raw `flash_area_erase()` instead — see `RESULTS.md`, "Downgrading".
- **`lg write` returns `-EFBIG` or `-ENOSPC`:** the segment geometry resolved
  differently than expected. The `segments: chunk … per object …` line above
  says how.
- **Nothing on the console:** wrong `/dev/ttyACM*` node (see above), or the
  net-core image was flashed instead of `cpuapp`.
