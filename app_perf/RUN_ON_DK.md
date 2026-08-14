# Running `app_perf` on the nRF5340-DK — instructions

**Why:** every wall-clock figure this benchmark prints is `0 ms` on
`native_sim`, because the flash simulator models no latency. The I/O counts are
already measured and trustworthy; **only the board can supply real timings.**

- **PR:** [wmilek/key_value_db#10](https://github.com/wmilek/key_value_db/pull/10)
- **Branch:** `claude/blob-db-max-payload-increase-6qobv5`
- **Commit to run:** `8428e35db03e0393f6cc9591dac68b213db2d44e` (short `8428e35`,
  *"app_perf: add large-object phases and flash I/O accounting"*)
- **Board:** `nrf5340dk/nrf5340/cpuapp`
- **Storage:** on-board **MX25R64 QSPI NOR**, 8 MB, 64 KB sectors — the board
  overlay moves `storage_partition` there, so this does *not* touch internal
  flash.

---

## Run A — the one that matters

```sh
west init -l key_value_db && west update --narrow -o=--depth=1   # if needed
cd <workspace>
git -C key_value_db checkout 8428e35

west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/dk_perf key_value_db/app_perf
west flash -d build/dk_perf
```

Then capture the **whole** console — 115200 8N1 on the DK's first VCOM — with
whatever you normally use, e.g. any one of:

```sh
python -m serial.tools.miniterm /dev/ttyACM0 115200 | tee dk_perf_8428e35.log
# or:  picocom -b 115200 /dev/ttyACM0 | tee dk_perf_8428e35.log
# or:  JLinkRTTClient  (if the build is routed to RTT rather than UART)
```

**Expect 5–15 minutes, with long silent stretches.** A 64 KB sector erase on
this part takes roughly a second, and the run performs several hundred of them:
each `bench prepare` line alone is ~100 erases, so ~100 s of apparent silence
before it prints. That is the measurement, not a hang.

The app never exits — Zephyr's idle loop runs forever once `main()` returns — so
stop capturing after the final `lg objects intact (…)` line.

### Sanity check — needs one extra build

The app pins `blob_db` logging to warnings so per-operation messages cannot
perturb the timed loops, which also hides the two lines that confirm the
geometry. Worth one short throwaway build to see them:

```sh
west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/dk_geom key_value_db/app_perf -- \
    -DCONFIG_BLOB_DB_LOG_LEVEL_INF=y -DCONFIG_APP_PERF_N_OPS=10 \
    -DCONFIG_APP_PERF_N_LARGE=1 -DCONFIG_APP_PERF_N_PART=4
west flash -d build/dk_geom
```

Expect, near mount:

```
partition 8388608 B, 128 sectors of 65536 B, 125 buckets
segments: chunk 2004 B, up to 128 per object (max object 256512 B)
```

A chunk of **2004** is correct here, not 16384: the auto rule starts at
sector/4 but clamps to what one slot can hold, and the index record is itself a
single-slot payload. If either line differs, say so — every other number depends
on it.

**Take the timings from the Run A build, not this one.** Compaction also logs at
INF, so this build's numbers are perturbed; it is only for reading the geometry.

---

## What to send back

The full log is ideal. If you have to trim, these lines are the payload:

| Line | Question it answers |
|---|---|
| `bench read` / `bench update` | small-blob point-operation cost — the PR 2 result |
| `bench lg write` vs `bench lg rewrite` | what a sector erase actually costs here |
| `bench lg read` | sequential read throughput in small windows |
| `bench lg pread q0..q3` | **must be four similar numbers** — contract R2 says read cost is independent of offset; a rising series falsifies it |
| `bench lg pwrite` + `partial vs whole-object write: … (Nx)` | the reason `blob_db_write` exists |
| every `io …` line | flash operations and bytes; deterministic, so it cross-checks the `native_sim` run |

Please also note the **board revision** and anything non-default about the QSPI
setup, since the erase and program timings depend on it.

---

## Run B — optional, but it settles an open question

PR 2 replaced one whole-sector read per lookup with many small reads. On
`native_sim` this measured as **6× more transactions for 47× fewer bytes**;
which of those dominates is a property of the real part, and nothing but this
board can say.

```sh
git -C key_value_db checkout 255ce7ad57d14ee1e23f95be92ef4d082c9ff3a5   # 255ce7a, immediately before PR 2
west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/dk_base key_value_db/app_perf
west flash -d build/dk_base
```

Capture as `dk_perf_255ce7a.log`. Only the **`bench read` and `bench update`**
lines are comparable between the two runs — that commit has no large-object
phases and no I/O counters. Everything else differs by design.

If `bench read` is *slower* at `8428e35` than at `255ce7a`, PR 2 is a
regression on real hardware and should be revisited. That is a real possible
outcome, not a formality.

---

## If something goes wrong

- **Mount fails with `-ENOTSUP`** and a message naming a payload cap versus a
  sector size: the build's `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` does not suit the
  geometry. Send the message; it names both numbers.
- **Mount fails with `-ENOTSUP` about a foreign format:** the QSPI already holds
  a store written by different firmware. That refusal is deliberate — nothing
  was destroyed. Erase the partition (or run any build that calls
  `blob_db_format()`) and retry.
- **`lg write` returns `-EFBIG` or `-ENOSPC`:** send the log; it means the
  segment geometry resolved differently than expected on 64 KB sectors, and the
  `segments: chunk … per object …` line near mount will say how.
- **Nothing on the console:** check `CONFIG_LOG` output is going to the DK's
  VCOM0 and that you flashed the app-core image (`cpuapp`, not `cpunet`).

## Tuning, if you want a shorter run

`west build … -- -DCONFIG_APP_PERF_N_LARGE=2 -DCONFIG_APP_PERF_OBJ_LEN=32768`
roughly quarters the large-object phases. Please report the knob values you
used, since every number scales with them — the defaults are `OBJ_LEN=65536`,
`N_LARGE=4`, `N_PART=32`, `PART_LEN=64`, and they are echoed in the
`-- large objects (…) --` header line.
