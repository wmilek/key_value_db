# app_perf_ycsb — the YCSB core workloads over `kvdb`

A **standard** benchmark, so a number from this stack can be put next to a
number from somewhere else.

The other benchmark applications in this tree measure what *this* code does:
`app_perf` times `blob_db`'s own operations, `app_perf_mc` prices the crash-safe
mutation discipline, `app_perf_kvdb` proves persistence across a reboot. All of
them answer "did this change help?" and none of them answers "how does this
compare to anything?" — because their workloads are their own.

This one runs [YCSB][ycsb], the Yahoo! Cloud Serving Benchmark (Cooper et al.,
*Benchmarking Cloud Serving Systems with YCSB*, SoCC 2010): the operation mixes
that LevelDB, RocksDB, LMDB, WiscKey and SILT all report against. TPC-C, the
benchmark people reach for first, is an SQL OLTP transaction mix and has
nothing to say about a key-value store; the TPC's *own* IoT benchmark, TPCx-IoT,
is YCSB-derived.

[ycsb]: https://github.com/brianfrankcooper/YCSB

## The workloads

| Workload | Mix | Key distribution | Here |
|---|---|---|---|
| A | 50 % read / 50 % update | scrambled Zipfian | yes |
| B | 95 % read / 5 % update | scrambled Zipfian | yes |
| C | 100 % read | scrambled Zipfian | yes |
| D | 95 % read / 5 % insert | latest | yes |
| E | 95 % **scan** / 5 % insert | scrambled Zipfian | **no** |
| F | 50 % read / 50 % read-modify-write | scrambled Zipfian | yes |

**Workload E is absent and cannot be added yet.** It needs an ordered range
query; `kvhash` is an unordered hash and `kvtree` is a skeleton (see the status
table in the top-level [`README.md`](../README.md)). That is a statement about
the stack, not about this harness — and it is the clearest argument in the tree
for prioritizing `kvtree`.

## What it is not

The Java YCSB client speaks to a network service and cannot reach an MCU. What
runs here is the **workload specification** — the mixes, the record shape, the
Zipfian constant, the generators — reimplemented in C, which is what every
embedded and local key-value store that reports YCSB has done. Two deliberate
departures, both because a key-value API has nowhere to put the alternative:

- **`writeallfields` is effectively true.** YCSB's default updates one field of
  a record; `kvdb_set` replaces a whole value and there is no field-level
  write, so an update rewrites the record. This is the same binding choice the
  RocksDB and LevelDB bindings make.
- **The scramble is applied over `recordcount`**, rather than over YCSB's fixed
  10-billion item space folded down. It preserves the distribution's shape over
  the key set, which is what the workload is about.

Both are noted so a comparison is made with its eyes open, not so they can be
forgotten.

## Running it

```shell
west build -p always -b native_sim key_value_db/app_perf_ycsb
./build/zephyr/zephyr.exe --flash=ycsb.bin --flash_erase
```

On `native_sim` the flash simulator models no latency, so **every wall-clock
figure is zero and only the I/O counters mean anything.** That is not a
limitation to work around — see "Reading the output" below.

```shell
west build -p always -b nrf5340dk/nrf5340/cpuapp key_value_db/app_perf_ycsb
west flash
```

Every run formats the store first: YCSB is a load phase followed by a run
phase, and the table that verifies reads lives in RAM. Cross-reboot persistence
is [`app_perf_kvdb`](../app_perf_kvdb)'s job.

## Reading the output

Four lines per phase:

```
bench C     :  1000 ops in      0 ms  ->      0.000 ops/s  (      0 us/op)
   mix       : read 1000  update 0  insert 0  rmw 0
   lat us    : min 0  avg 0 p95 0 p99 0  max 0
   io C      : rd  36934 ops/  7285258 B   wr     0 ops/        0 B   er    0 ops/        0 B
   per op    : rd 36 ops/7285 B   wr 0.00 ops/0 B
   ampl      : rd 7.28x   wr 0.00x   (per 1000000 B of record data)
```

- **`bench` / `lat`** — throughput and the latency distribution. Meaningful on
  hardware, zero on `native_sim`.
- **`io` / `per op` / `ampl`** — the flash the phase actually touched
  (`CONFIG_BLOB_DB_IOSTATS`), per operation, and as a ratio against the record
  bytes the caller asked for. Deterministic, and identical on `native_sim` and
  hardware when the build carries the target's geometry
  ([`app_perf_l0/RESULTS.md`](../app_perf_l0/RESULTS.md) §2) — which
  `boards/native_sim.overlay` here does.

`bench` and `io` are printed in the shape
[`app_perf_l0/tools/l0_timing.py`](../app_perf_l0/tools/l0_timing.py) parses, so
a `native_sim` capture becomes predicted hardware milliseconds without a board:

```shell
python3 app_perf_l0/tools/l0_timing.py predict \
    -m app_perf_l0/models/mx25r64_nrf5340dk_full.json capture.txt
```

That is where the numbers in [`RESULTS.md`](RESULTS.md) come from, and they are
labelled *predicted* until someone reruns them on the DK.

## Sizing

`kvhash` reads the **whole bucket directory and one whole packed bucket on
every operation**, so bytes read per operation is

```
    8*B  +  R*E/B          B = buckets, R = records, E = bytes per entry
```

The directory term grows with `B` and the bucket term shrinks with it, so the
sum is smallest at `B = sqrt(R*E/8)` — and the minimum itself is
`2*sqrt(8*R*E)`. **Read amplification on this container grows as the square
root of the record count, whatever `B` is set to.** That is a property of the
flat directory, and measuring it rather than assuming it is a large part of why
this app exists.

`CONFIG_APP_PERF_YCSB_BUCKETS=0` (the default) computes that optimum, rounds it
up to a power of two for headroom against bucket skew, and prints the choice:

```
sizing     : 1000 records x 1000 B record (10 fields x 100 B) = 1018 B entry
sizing     : 512 buckets (max 2047 at payload 16384 B); directory 4104 B, mean bucket 1988 B, predicted fullest 8144 B (49 % of payload)
sizing     : predicted read per get = 4104 B dir + 1988 B bucket = 6092 B -> 6.09x amplification
```

A record set that cannot fit is a **boot-time failure with the arithmetic
printed**, not a mid-run `-ENOSPC`. This matters at YCSB's default record size:
1 KB records do not fit the shipped `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` default of
256, and the payload this app configures (16384) is the value
[`app_cbor_persondb`](../app_cbor_persondb) already proved usable on the DK's
64 KB geometry. Shrink `CONFIG_APP_PERF_YCSB_FIELD_LEN` for a smaller build and
say so when reporting — a YCSB number is only comparable alongside its record
size.

## Data integrity

Every read is checked against the bytes its key and version predict
(`CONFIG_APP_PERF_YCSB_VERIFY`, YCSB's `dataintegrity` option, on by default).
Each field carries a `{keynum, field index, version}` stamp and a fill derived
from all three, so a wrong key, a shuffled field and a stale value are all
caught — not just a corrupted byte. The whole run fails on any mismatch.

This is on by default because the property under test is a *crash-safe* store:
throughput reported without checking that the bytes came back intact is
measuring the wrong thing. It costs one `memcmp` against a flash access.

## The generator, and why it is checked

A wrong key distribution does not look wrong in the output — it looks like a
store with a suspiciously good cache hit rate. So before any flash is touched,
the app draws from the generator that will drive the run and compares the
hottest 1 % of keys against the share `zeta()` predicts:

```
generator  : theta 0.990, top 10 of 1000 keys took 40.0 % of 100000 draws (zeta predicts 38.2 %)
generator  : PASS
```

The prediction is computed from the configured `recordcount` and Zipfian
constant, not tabulated, so changing either re-derives the expectation instead
of quietly invalidating it. A failure stops the run.

## Configuration

The parameters are YCSB's, under `CONFIG_APP_PERF_YCSB_*` — see
[`Kconfig`](Kconfig). The shipped defaults are the spec's: `recordcount` 1000,
`operationcount` 1000, records of 10 × 100 B, `zipfianconstant` 0.99. The seed
is fixed, so two runs of the same build issue the same key sequence and the I/O
counters are a regression guard rather than a sample.

## Comparing against something

A YCSB figure from an MCU has no published peer — nobody else reports YCSB on a
Cortex-M. On its own, "workload A: 14.7 ops/s" tells a reader nothing.

It becomes a claim when the same workload runs against a **peer store on the
same board and the same partition** — Zephyr's NVS or ZMS, or LittleFS. That
harness does not exist yet. Until it does, treat the numbers here as this
stack's own baseline and as the amplification measurement they genuinely are,
not as a competitive result.
