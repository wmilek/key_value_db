# app_perf_ycsb — reference results

YCSB core workloads A, B, C, D and F over `kvdb`, at the spec's own defaults.

**These wall-clock numbers are PREDICTED, not measured.** The I/O counters are
measured — on `native_sim`, where they are exact and reproduce hardware
(`app_perf_l0/RESULTS.md` §2) — and the milliseconds come from running them
through the L0 timing model fitted on the DK. Nothing here has been on a board
yet. Replace this file with a hardware capture when it has; the counters should
not move, and the section "What a hardware run should confirm" says what to
check.

## Setup

- **Workload**: YCSB `recordcount` 1000, `operationcount` 1000 per workload,
  records of 10 × 100 B, `zipfianconstant` 0.99, seed 1
- **Store**: `kvdb` on `kvhash`, 512 buckets (derived),
  `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN=16384`
- **Counters from**: `native_sim` carrying the DK's geometry
  (8 MB partition, 64 KB erase blocks — `boards/native_sim.overlay`)
- **Predicted onto**: `app_perf_l0/models/mx25r64_nrf5340dk_full.json`
  (MX25R6435F, 8 MB QSPI NOR, nRF5340-DK cpuapp)
- **Both backends**: UBI (the default) and `flash_area`
- **Code**: this commit; **Zephyr** build `5a56224939aa`

Reproduce:

```shell
west build -p always -b native_sim key_value_db/app_perf_ycsb
./build/zephyr/zephyr.exe --flash=ycsb.bin --flash_erase | tee ycsb.txt
python3 key_value_db/app_perf_l0/tools/l0_timing.py predict \
    -m key_value_db/app_perf_l0/models/mx25r64_nrf5340dk_full.json ycsb.txt
```

## Headline — UBI backend (the default)

Predicted flash time, and the throughput it implies, per YCSB workload:

| Workload | Mix | Predicted | per op | ops/s |
| -------- | --- | --------: | -----: | ----: |
| C | 100 % read | 4.41 s | 4.41 ms | **227** |
| D | 95 % read / 5 % insert | 11.62 s | 11.6 ms | **86** |
| B | 95 % read / 5 % update | 17.80 s | 17.8 ms | **56** |
| A | 50 % read / 50 % update | 67.93 s | 67.9 ms | **14.7** |
| F | 50 % read / 50 % RMW | 97.70 s | 97.7 ms | **10.2** |

The load phase (1000 inserts) is 238.0 s, or 238 ms per insert, on top of a
127.1 s bucket pre-format. Both are one-time, and both are erase.

Workload E is not in the table because it cannot run: it is 95 % range scans
and `kvhash` has no ordered iteration. See `README.md`.

## The measurement: flash traffic

Measured, deterministic, identical on `native_sim` and hardware.

| Phase | read ops | read B | write ops | write B | erase blocks | rd ampl | wr ampl |
| ----- | -------: | -----: | --------: | ------: | -----------: | ------: | ------: |
| prepare |    118 |     1 888 |  116 |     1 856 | 116 |    — |    — |
| load    | 30 707 | 7 509 232 | 1 628 | 4 239 720 | 166 | 7.50× | 4.23× |
| A       | 43 629 | 7 900 548 |  535 | 1 554 558 |  40 | 7.90× | 1.55× |
| B       | 37 237 | 7 451 616 |   63 |   193 720 |  10 | 7.45× | 0.19× |
| C       | 36 934 | 7 285 258 |    0 |         0 |   0 | 7.28× | 0.00× |
| D       | 29 998 | 7 236 296 |   66 |   181 584 |   5 | 7.23× | 0.18× |
| F       | 55 695 | 11 704 690 | 548 | 1 601 640 |  65 | 7.89× | 1.08× |

Amplification is against the record bytes the caller asked for (1 000 000 B per
1000-operation workload; 1 483 000 B for F, whose RMW touches a record twice).

### Read amplification is 7.3×, and 6.1× of it was predictable

The sizing model says a `get` should read the whole 4104 B bucket directory
plus a ~1988 B bucket — 6092 B for 1000 B of record, **6.09×**. Workload C
measures **7.28×**. The 1193 B difference is `blob_db`'s own id resolution: 36.9
flash reads per `get`, averaging 197 B each, against the two payload reads
`kvhash` asked for. That is the bucket-log slot-header walk
(`doc/impl/l1_bucketlog.md`) showing up as transactions rather than bytes.

**Transactions cost more than bytes here.** Workload C's whole predicted time
is its read term, and the model splits that term **2557 ms of fixed
per-transaction cost against 1849 ms of per-byte cost** — 58 % of a read-only
YCSB workload is paid for issuing 36 934 QSPI transactions, not for the 7.3 MB
they move. Halving the bytes read would save less than halving the number of
reads.

### The directory is the thing to fix

Read amplification here is not a constant; it is
`(8*B + R*E/B) / E`, minimised at `B = sqrt(R*E/8)`, so it **grows as the square
root of the record count** for any bucket count. At 1000 records the floor is
5.71× (at 357 buckets) and the 512 buckets this app picks cost 6.09×; at 10 000
records of the same shape the floor alone would be **18.0×**. The flat
bucket directory read in full on every operation is the term that does it, and
it is the same finding `app_cbor_persondb` reached from the other direction
(its K11, at 16 384 B of directory per operation).

This is the number to move. Nothing about the workload mixes changes it.

### Writes: the erase term is everything

Workload A writes 1.55× and takes 67.9 s; workload C writes nothing and takes
4.4 s. The gap is not the 1.55 MB written — at the model's write cost that is
19.1 s — it is the **40 block erases, 43.8 s of it**. One 64 KB erase on this
part is ~1.1 s, so at these operation counts a single compaction landing inside
a phase moves its total by more than the whole workload mix does.

That also means the per-workload write comparison below is lumpy at
`operationcount` 1000: whether a compaction falls inside a phase window is close
to quantised. Raise `operationcount` before reading much into a 10 % difference.

## The two backends

Same commit, same geometry, same seed; only `CONFIG_BLOB_DB_BACKEND_*` differs.

| Phase | UBI (default) | `flash_area` | |
| ----- | ------------: | -----------: | --- |
| prepare | 127.1 s | 133.8 s | −5 % |
| load    | 238.0 s | 238.1 s | — |
| A       |  67.9 s |  74.0 s | UBI −8 % |
| B       |  17.8 s |   6.2 s | UBI ×2.9 slower |
| C       |   4.41 s |  4.34 s | — |
| D       |  11.62 s | 11.61 s | — |
| F       |  97.7 s | 138.5 s | UBI −29 % |

Read the two read-only rows (C, D) first: they agree to within 2 %, which is
the check that the two runs are comparable at all. Everything else is the erase
term — UBI took 10 block erases in B where `flash_area` took none, and 65 in F
where `flash_area` took 100. At single-digit erase counts that is which side of
a phase boundary a compaction landed on, not a steady-state property of either
backend. `app_perf_kvdb/RESULTS.md` has the backend comparison that was
measured with enough operations to mean something.

## What a hardware run should confirm

1. **The counters do not move.** Every figure in the flash-traffic table is
   deterministic and geometry-derived; a hardware capture that disagrees means
   the `native_sim` geometry no longer mirrors the DK, and that invalidates
   more than this file.
2. **Measured time exceeds predicted, and by how much.** The model is flash
   cost only — CPU between flash calls is not in it. `app_perf_kvdb` found
   `kvhash`'s in-bucket unpack and key compare to be the dominant term in a
   `get` once L1 got faster, and this app packs 8× more per bucket than that
   one did. Run
   `l0_timing.py verify -m <model> <hardware capture>` to get the residual per
   phase; a large positive residual on C is that CPU term, not a bad model.
3. **The latency distribution.** `native_sim` reports every percentile as zero.
   p99 against p95 on hardware is where a bucket that needs a compaction shows
   up, and it is the one number here that no simulator run can produce.
