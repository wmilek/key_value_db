# Storage harness for the persondb case-performance investigation

**This is not project code and is not built by CMake.** It exists so the
numbers in
[`doc/proposals/2026-08-16-persondb-case-performance.md`](../2026-08-16-persondb-case-performance.md)
can be re-derived instead of taken on trust — the same reason
[`doc/proposals/sizing/`](../sizing/README.md) exists.

```
sh doc/proposals/persondb-perf/measure.sh validate   # the two fidelity checks
sh doc/proposals/persondb-perf/measure.sh            # the full sweep
```

| File | What it is |
|---|---|
| `measure.sh` | builds the harness at three payload ceilings and runs the sweep |
| `harness.c` | replays persondb's storage traffic: fill, mutate, then the `check` / `byid` / `miss` / `put` benchmark cases |
| `store_host.c` | a RAM-backed NOR model with the DK's geometry — program is AND, erase is 0xff |
| `crc.c` | CRC16/CRC32 stand-ins (the same functions write and verify, so any polynomial is self-consistent) |
| `stub/zephyr/` | minimal `kernel.h`, `log.h`, `crc.h`, `util.h`, `toolchain.h` so `blob_db.c` and `kvhash.c` compile outside a Zephyr tree |

**The libraries are the real ones.** `measure.sh` compiles
`lib/blob_db/blob_db.c` and `lib/containers/kvhash/kvhash.c` from the tree, so
the slot walk, the directory read, the bucket rewrite and compaction are the
shipping algorithms, not a model of them.

## What it does and does not reproduce

- **Reproduced:** flash operation counts, flash byte counts, erase counts,
  amplification, bucket occupancy, `-ENOSPC` on an undersized map. These are
  the `native_sim`-class results that `RESULTS.md` §1 says transfer to
  hardware. §2 of the document checks them against both published runs.
- **Not reproduced:** wall-clock time. Durations are *modelled* from the
  constants `app_perf/RESULTS.md` fitted on the DK — ~65.5 µs per flash read
  transaction, ~0.63 µs/B, ~1072 ms per 64 KB erase. The read-only cases use
  only those. The write constant (`HB_WR_US_PER_B` in `harness.c`) is **not**
  fitted against hardware, so `put` and `fill` durations are indicative and
  their op/byte/erase columns are the results to read.
- **Not modelled at all:** CBOR encode and decode. The harness writes blobs of
  the right length without producing them, so add the measured 0.95 ms
  (`RESULTS.md` §5a) when comparing a `check` figure against the board.

## Keeping it honest

The dataset generator, the key formats, the shard hash and the CBOR length
rules are **copies** of `dataset.c`, `persondb.c` and `person_cbor.c`. Nothing
enforces that they stay in step — the same weakness `tools/sizing.py` carries,
and for the same reason. If a record's shape changes, `measure.sh validate`
is what tells you: it stops reproducing the published counters.
