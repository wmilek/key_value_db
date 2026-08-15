# Running `app_perf` on the nRF5340-DK

`RESULTS.md` holds the numbers from both board runs, the raw captures, and the
exact `nrfutil` / console commands. This file is the operator's guide: what to
expect while it runs, and what tends to go wrong.

> ## Run 2 is done — `12df53b`, 2026-08-15
>
> PR 5's one-entry index cache has been measured on hardware. The
> transaction prediction below was exact; the byte prediction was not, which
> is the falsifying outcome this section asked to be told about.
>
> | line | recorded at `8428e35` | predicted | **measured at `12df53b`** |
> |---|--:|--:|--:|
> | `io lg read` ops | 92 135 | ~30 700 (~3×) | **30 755** ✓ |
> | `io lg read` bytes | 11 623 044 | ~3 285 000 (~3.5×) | **8 742 276** ✗ |
> | `io lg read` ampl | 44.33× | ~12.5× | **33.34×** ✗ |
> | `bench lg read` | 3254 µs/op | ~920 µs/op | **1831 µs/op** ✗ |
> | `io lg pread q0..q3` ops | ~710–727 each | ~240 each | **unchanged** ✗ |
>
> The scan in front of the index record turned out to be ~72 B, not ~738 B, so
> removing two lookups saved 704 B/op rather than ~2036. What remains is one
> 2004 B chunk read per 64 B window — an amplification floor of 31.3×, which
> makes the predicted 12.5× unreachable at any hit rate. `pread` did not move
> because that phase cycles all four objects on consecutive calls, so a
> one-entry cache never hits.
>
> `doc/proposals/2026-08-09-large-payloads-cost.md` §3 has been corrected
> accordingly; the detail and the raw capture are in `RESULTS.md`. The
> `partial vs whole-object write` line now prints `(1.96x)` as intended.
>
> **If you re-run:** attach exactly one reader to the console. Two concurrent
> `cat`s on the same tty split the byte stream and silently shred the capture —
> the run itself is fine, but every number in the log is garbage.

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
