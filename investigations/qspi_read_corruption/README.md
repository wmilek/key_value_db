# QSPI read corruption on nRF5340 DK + MX25R6435F — investigation

> **DO NOT MERGE.** This directory holds evidence and reproducers for a
> hardware-level read corruption, not production code.

## Summary

On the nRF5340 DK (PCA10095), reads from the on-board MX25R6435F 8 MB QSPI
NOR occasionally return wrong data. It happens after a specific, dense sequence
of earlier reads.

- **What a bad read returns:** the bitwise **OR** of the requested location and
  the same offset in another 64 KB block. That partner block differs from the
  target only in address bits **A16–A18**.
- **The bug is deterministic.** The same read sequence corrupts the same read
  every time.
- **It's one-shot:** the next read of the same address is correct.
- **It isn't one bad unit.** It reproduces on two DKs and on Zephyr main and
  v4.4.0.

It was found because PR #40's `app_perf_kvdb` walk phase failed on the UBI
backend. UBI turned out to be irrelevant. What matters is that UBI puts data
128 bytes into each 64 KB sector, which produces the triggering access pattern.
The plain flash backend reproduces it once given the same layout.

## Findings

### Corruption mechanism
- The bad bytes are `target | partner`, and bits only ever go 0→1. For example:
  - `0x470080` came back ORed with `0x400080`;
  - in the shifted layout, `0x180080` came back ORed with `0x1f0080`.
- The partner is the target with A16–A18 of a block read earlier in the same
  1 MB region (8 × 64 KB). It looks like a stale block or row select inside
  the flash.
- It isn't limited to headers: data at `+0x90` and `+0x100` is corrupted too.
- It's one-shot. Any extra read clears it, but a 2 ms idle wait before the
  target read does not.

### Trigger
- Replaying the recorded 43,300 reads (`common/trace.inc`) corrupts entry
  43297 on every pass.
- The shortest failing tail is the last **2,227 reads**: one read of block
  0x17, then about 22 cycles of reads alternating between blocks 0x07 and 0x0a,
  interleaved with reads in 0x09, 0x21, 0x32 and 0x38.
  - The reads of blocks 0x17, 0x07 and 0x0a are all required.
  - Order matters; read sizes don't.
  - Removing chunks one at a time got it to 1,642 reads, but the end result
    then stopped failing (`logs/run_replay_min.log`). So the trigger also
    depends on timing or state and doesn't minimize cleanly.
- Simplified synthetic patterns never trigger it: "read partner, then N far
  reads, then read target", and plain X/Y cycles.
- **Density matters.** A busy-wait before *every* read ("gap") changes the
  outcome:

  | gap per read | board 1 | board 2 |
  |---|---|---|
  | 0 µs | fails | fails |
  | 20 µs | fails | borderline (main: passes; v4.4.0: fails 2/3) |
  | 100 µs | passes | passes |

### Location: chip-wide, varies per unit
- `sweep_blocks`: with the trigger, all 14 blocks in 0x10–0x16 and
  0x18–0x1e are corrupted. The partner is 0x17 for the lower group and 0x1f for
  the upper group.
- `sweep_relocate`: the same trigger is shifted by k blocks, then the shifted
  0x10–0x1e region is read.

  | shift | target region | board 1 | board 2 |
  |---|---|---|---|
  | +0x00 | 0x10–0x1e | 14/14 | 14/14 |
  | +0x10 | 0x20–0x2e | 0/14 | 12/14 |
  | +0x20 | 0x30–0x3e | 14/14 | 14/14 |
  | +0x30 | 0x40–0x4e | 11/14 | 0/14 |
  | +0x40 | 0x50–0x5e | 14/14 | 14/14 |
  | +0x50 | 0x60–0x6e | 0/14 | 0/14 |
  | +0x60 | 0x70–0x7e | 0/9 | 0/9 |
  | +0x70 | 0x00–0x0e | 12/12 | 0/12 |
  | +0x08 | 0x18–0x26 | 0/14 | 0/14 |
  | +0x01 | 0x11–0x1f | 14/14 | 7/14 |

  Every unit is affected, but in different regions. That points to a
  timing-margin problem, not a defective sector, so bad blocks cannot be mapped
  out.

### Ruled out

| Ruled out | Evidence |
|---|---|
| UBI | The plain flash backend with the same layout fails. |
| PSA crypto | About 860k reads with it gave 0 mismatches. |
| Heap / RAM destination | A static bounce buffer still fails. |
| DMA still landing after return | The buffer is stable for 200 µs afterwards. |
| QSPI deactivation, 192 MHz clock divider | Not the cause. |
| Read command | read4io (0xEB) and fastread (0x0B) fail identically. |
| SPI clock speed | 6 and 8 MHz both fail. |
| Alignment, read size | Neither matters. |
| Zephyr regression | Main `4a405846193f` and v4.4.0 (with hal_nordic `44fd3d4`) both fail. |
| One faulty board | Two DKs fail: J-Link SN 960115021 and 1050091053. |

### Open questions
- Is it a Macronix MX25R64 erratum, or the nRF5340 QSPI peripheral's timing,
  for example chip-select high time between transfers?
  - To test: scope CS and SCK, raise the flash supply voltage, try a different
    flash part.
- Temperature and voltage sensitivity.

## Impact on key_value_db

`blob_db` write paths call `format_bucket()` whenever `read_bucket_hdr()`
reports an invalid header (`blob_db_update()` and two other call sites). One
corrupted read therefore **erases a whole bucket sector**: silent data loss.

Recommended fixes, in separate PRs:
1. Format a bucket only if its header reads as all-0xFF. Otherwise re-read,
   then return `-EIO`.
2. Re-read on any CRC or magic mismatch before treating data as corrupt. The
   corruption has never survived a second read.

## Contents

| Path | What |
|---|---|
| `apps/fmt_once` | Erases the whole 8 MB `storage_partition` (~2 min). |
| `trigger/blob_db_store_flash_emu.patch` | The flash backend changed to use UBI's geometry (data at sector×64K + 128). It also re-reads every read to detect mismatches and has optional tracing: `EMU_MODE` 0–11, `EMU_SHIFT`, `EMU_N`. Mode 11 dumps a RAM ring of every flash op, which is how `trace.inc` was captured. |
| `trigger/fastread.overlay` | Switches the read command to fastread (0x0B). |
| `common/trace.inc` | The 43,300 reads `{addr, len}` that precede the corrupted read. |
| `apps/replay` | Replays the trace with a 0, 20 and 100 µs gap, 3 passes each, and checks every bucket header's CRC. `-DEXTRA_CFLAGS=-DREPLAY_MINIMIZE` builds the delta-debugging minimizer instead. |
| `apps/sweep_blocks` | Runs the trigger, then reads each of the 128 blocks at +0x80, +0x90 and +0x100 and identifies the partner block. The variants are: A = the real tail; B = a synthetic burst; C = the tail with a 100 µs gap. **Variant B is flawed:** its target pre-read resets the state, so its "0 bad" proves nothing. |
| `apps/sweep_relocate` | Shifts the trigger by k blocks to map affected regions. |
| `scripts/dkrun.sh` | Captures the console, flashes the board and waits for a regex. Select the board with `SN=… PORT=…`. |
| `logs/` | Key logs. `b1_`/`b2_` = board 1 or 2; `run_` = board 1. |

## Reproducing

Everything was measured at key_value_db `2502a20` (PR #40 head). The patch
also applies to current main, but `app_perf_kvdb` has changed since. The read
indices will therefore differ, though `apps/replay` only depends on what is
stored in flash.

```sh
# build (zephyr CI container, workspace at ~/src mounted on /workdir)
west build -b nrf5340dk/nrf5340/cpuapp investigations/qspi_read_corruption/apps/<app>

# 1. blank the partition
dkrun.sh build/fmt_once  fmt.log 'DONE' 300

# 2. write the test store: app_perf_kvdb at 2502a20 with the EMU patch applied,
#    using the 1-sector-shifted layout trace.inc was captured with
git apply investigations/qspi_read_corruption/trigger/blob_db_store_flash_emu.patch
west build -b nrf5340dk/nrf5340/cpuapp app_perf_kvdb -- "-DEXTRA_CFLAGS=-DEMU_MODE=0 -DEMU_SHIFT=1"
dkrun.sh build/app_perf_kvdb fresh.log 'WALK (PASS|FAIL)' 400 'walk:|No volumes'
#    -> "FADBL mismatch ... off=0x180080" (board 1) / 0x480080 (board 2), WALK FAIL

# 3. replay (does not modify flash)
dkrun.sh build/replay replay.log 'REPLAY DONE' 200
#    -> "REPLAY bad header #1 at entry 43297/43300 a=0x180080: 42 44 42 48 1f ..."
```

Environment:
- Zephyr SDK 1.0.1 in `ghcr.io/zephyrproject-rtos/ci:latest`.
- Zephyr main `4a405846193f` (4.4.99) and v4.4.0.
- Board DT: `readoc = "read4io"`, SCK 8 MHz.
