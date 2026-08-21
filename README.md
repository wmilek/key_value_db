# Zephyr Key-Value DB

<a href="https://github.com/wmilek/key_value_db/actions/workflows/build.yml?query=branch%3Amain">
  <img alt="Build" src="https://github.com/wmilek/key_value_db/actions/workflows/build.yml/badge.svg?branch=main">
</a>
<a href="https://github.com/wmilek/key_value_db/actions/workflows/docs.yml?query=branch%3Amain">
  <img alt="Documentation build" src="https://github.com/wmilek/key_value_db/actions/workflows/docs.yml/badge.svg?branch=main">
</a>
<a href="https://wmilek.github.io/key_value_db">
  <img alt="Documentation" src="https://img.shields.io/badge/documentation-3D578C?logo=sphinx&logoColor=white">
</a>
<a href="https://wmilek.github.io/key_value_db/doxygen">
  <img alt="API Documentation" src="https://img.shields.io/badge/API-documentation-3D578C?logo=c&logoColor=white">
</a>

A **crash-safe, layered key-value storage stack for [Zephyr][zephyr]**, running
on raw flash partitions or on a wear-leveled UBI volume.

At the bottom sits `blob_db`: a store of blobs addressed by a stable `uint64_t`
id, where every visible operation is atomic against power loss. On top of it,
containers turn those ids into data structures and `kvdb` exposes the familiar
string-key / byte-value API:

```c
#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>
#include <app/lib/kvdb.h>

blob_db_mount();                       /* once, at boot */
rootreg_init();

kvdb_t db;
kvdb_open(&db, "config", NULL);        /* named store; created on first open */

kvdb_set(&db, "wifi.ssid", "home", 4);

char ssid[32];
size_t len;
kvdb_get(&db, "wifi.ssid", ssid, sizeof(ssid), &len);
```

Nothing is cached across a reboot and nothing is rebuilt at mount: every
structure is reachable from a single well-known integer (id `1`), so re-opening
a store after power loss is one flash read, not an index rebuild. The design
rationale for that — and for everything else — is in [`doc/`](doc/); this file
is the map.

The repository is packaged as a **Zephyr module plus a set of workspace
applications** ([West T2 topology][west_t2]), so it can either bootstrap its own
workspace or be added to an existing one.

[zephyr]: https://github.com/zephyrproject-rtos/zephyr
[west_t2]: https://docs.zephyrproject.org/latest/develop/west/workspaces.html#west-t2

## The stack

```
┌──────────────────────────────────────────────────────────────────────┐
│  L3  Access interfaces      kvdb · blobfs · settings-registry        │  enable interface(s)
│      what the firmware calls: keys, paths, records                   │
├──────────────────────────────────────────────────────────────────────┤
│  L2  Containers             seq · kvlist · kvhash · kvtree           │  choose backing container(s)
│      data structures wired out of i-nodes                            │
├──────────────────────────────────────────────────────────────────────┤
│  L1½ Root registry          owner of id = 1; key → structure root    │  tiny, always simple
│      where clients persist "where is my structure"                   │
├──────────────────────────────────────────────────────────────────────┤
│  L1  i-node allocation      blob_db: stable u64 id → blob            │  the always-present core
│      crash-atomic alloc_id/update/get/delete by id                   │
├──────────────────────────────────────────────────────────────────────┤
│  L0  Flash translation      UBI volume (default) · flash_area        │  swappable provider
│      erase blocks, alignment, (wear/bad blocks in FTL form)          │
└──────────────────────────────────────────────────────────────────────┘
```

Dependencies point strictly downward across narrow contracts:

```
L3 ──map_ops / seq_ops──► L2 ──blob_db API──► L1 ──blob_db_store──► L0
```

## Status

Contracts for L0–L3 are specified in `doc/layers/`; the code has landed
bottom-up. Modules marked *skeleton* are build-wired and Kconfig-gated
(`default n`) but not yet implemented.

| Layer | Module | Kconfig | State | Tests |
|---|---|---|---|---|
| L0 | UBI volume (wear-leveled) | `BLOB_DB_BACKEND_UBI` | implemented (**default**) | `tests/lib/blob_db` (`.ubi` scenario) + every other L1–L3 suite |
| L0 | raw partition (`flash_area`) | `BLOB_DB_BACKEND_FLASH_AREA` | implemented | `tests/lib/blob_db` (3 pinned scenarios) |
| L1 | `blob_db` | `BLOB_DB` | implemented | `tests/lib/blob_db`, `tests/lib/blob_db_contract` |
| L1 | large payloads (segmented objects) | `BLOB_DB_LARGE_PAYLOADS` | implemented (opt-in, `default n`) | `tests/lib/blob_db` (`.large_payloads`, with crash injection) |
| L1½ | `rootreg` | `BLOB_ROOTREG` | implemented | `tests/lib/rootreg` |
| L2 | `kvhash` (Map, O(1)) | `BLOB_CONTAINER_KVHASH` | implemented | via `tests/lib/kvdb` |
| L2 | `kvlist` · `kvtree` · `seq` | `BLOB_CONTAINER_*` | skeleton | — |
| L2 | shared intent helper | `BLOB_CONTAINERS_INTENT` | skeleton | — |
| L3 | `kvdb` | `BLOBDB_KVDB` | implemented (kvhash backend) | `tests/lib/kvdb` |
| L3 | `blobfs` | `BLOBDB_BLOBFS` | skeleton | — |

`tests/lib/blob_db_contract` is the acceptance suite for the *model container*
of `doc/layers/l1_model_container.md`: a reference key/value structure built
purely on the L1 contract, with crash injection at every mutation step. It is
the proof that the `blob_db` contract is sufficient to carry the layers above.

## Getting started

You need a working Zephyr development environment — follow the official
[Zephyr Getting Started Guide][getting_started] first.

[getting_started]: https://docs.zephyrproject.org/latest/getting_started/index.html

### Initialize a workspace

```shell
# initialize my-workspace for key_value_db (main branch)
west init -m https://github.com/wmilek/key_value_db --mr main my-workspace
cd my-workspace
west update
```

`west update` clones Zephyr (only the modules this repo needs — see the
`name-allowlist` in [`west.yml`](west.yml)) and the [UBI][ubi] flash
virtualization layer.

[ubi]: https://github.com/kamil-kielbasa/ubi

### Build and run

From the workspace top directory:

```shell
# the blob_db demo, on the host simulator
west build -b native_sim key_value_db/app

# ...or on hardware
west build -b nrf5340dk/nrf5340/cpuapp key_value_db/app
west flash
```

`native_sim` binaries take a backing file for the simulated flash, so state
survives a restart the same way it does on a device:

```shell
./build/zephyr/zephyr.exe --flash=/tmp/blob.bin   # run again: the counter advances
```

The storage stack is exercised on two targets: `native_sim` (simulated flash,
where the test suites run) and `nrf5340dk/nrf5340/cpuapp`, whose
`storage_partition` sits on the on-board MX25R64 QSPI NOR — both are built by
CI. The `custom_plank` board and the `nucleo_f302r8` overlay come from the
example-application scaffolding. A debug configuration is available with
`-DEXTRA_CONF_FILE=debug.conf`.

#### Storage backend

`blob_db` stores blobs on a **wear-leveled UBI volume by default**. The builds
above get it with no extra flags; every in-tree board file already sizes UBI's
static block pool for its partition. To opt out and store directly on the raw
partition — faster, but no wear leveling and no bad-block handling:

```shell
west build -b native_sim key_value_db/app -- -DCONFIG_BLOB_DB_BACKEND_FLASH_AREA=y
```

Two things to know before switching a real device:

- A **new board** using the UBI backend must set
  `CONFIG_UBI_MAX_NR_OF_DATA_PEBS` to its partition's block count. The default
  of 14 builds cleanly and then fails to attach at runtime.
- The two layouts are **not interchangeable**, and mount does not reliably
  refuse the wrong one — booting a `flash_area` build on a UBI store currently
  reformats it. Erase the partition deliberately when switching, and set
  `CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT=n` in production. The measured
  behavior in both directions is in
  [`doc/impl/l0_backends.md`](doc/impl/l0_backends.md) §4.

### Test

The ztest suites all run on `native_sim`:

```shell
west twister -T key_value_db -p native_sim -v --inline-logs   # tests + app builds
west twister -T key_value_db/tests -p native_sim              # tests only
```

This is what CI runs ([`build.yml`](.github/workflows/build.yml)), plus an
app build on the non-default `flash_area` backend. At the merge gate it also
cross-builds for ARM: the demo on both storage backends and with `kvdb`
enabled, `app_perf`, and `app_cbor_persondb` in both of its frontends — so the
binaries that produce the hardware numbers cannot rot between runs on real
hardware.

## Applications

Each application is a standalone Zephyr app. The measuring ones print their
timings over the console and keep hardware-measured reference numbers in a
`RESULTS.md` next to the source.

| Application | What it does | Reference results |
|---|---|---|
| [`app/`](app) | `blob_db` demo: a boot counter persisted at the root id, wiped with `blob_db_erase_all()` every 5th boot | — |
| [`app_perf/`](app_perf) | raw `blob_db` benchmark: prepend / append / read / update over a linked list of blobs | [`RESULTS.md`](app_perf/RESULTS.md) |
| [`app_perf_l0/`](app_perf_l0) | **L0 cost model**: raw `flash_area` timing, swept over transfer size and erase size as a matrix of µs/op, KiB/s and marginal cost — so whether the relationship is linear is read off, not assumed. Its output feeds a timing model that turns any `native_sim` run's I/O counters into predicted hardware wall-clock, and can be checked against the part's datasheet | [`RESULTS.md`](app_perf_l0/RESULTS.md) |
| [`app_perf_mc/`](app_perf_mc) | model-container benchmark — the price of the full crash-safe mutation discipline | [`RESULTS.md`](app_perf_mc/RESULTS.md) |
| [`app_perf_kvdb/`](app_perf_kvdb) | `kvdb` demo + benchmark with **cross-reboot verification**: every value is predicted from a stored generation counter, so a rerun proves the previous run survived — and an interrupted run is detected and proven atomic | [`RESULTS.md`](app_perf_kvdb/RESULTS.md) |
| [`app_cbor_persondb/`](app_cbor_persondb) | a CBOR person/credential database — 10 000 people over the L2 Map shape, with the access decision, crash safety and capacity planning a real product needs. Both a **worked example** of building on this stack and a **probe** of it | [`RESULTS.md`](app_cbor_persondb/RESULTS.md) |

`app_perf_l0` is the one to reach for when a change moves flash traffic and
there is no board on the desk. It is the only app here that links none of the
stack — it measures `flash_area` itself — and the model fitted from one board
run turns the operation counters every other benchmark already prints into
predicted seconds on that board. A `native_sim` run carrying the target's
geometry reproduces the hardware's counters exactly (`app_perf_l0/RESULTS.md`
§2), which is what makes the prediction meaningful rather than arithmetic.

`app_perf_kvdb` is the one to reach for when validating power-loss behavior on
real hardware: cut power during its modify phase and the next boot classifies
the torn state, verifies that every key holds *one* of the two allowed values,
and heals.

`app_cbor_persondb` is the one to read before building something real on this
stack. Its [`README.md`](app_cbor_persondb/README.md) is fourteen practices,
each naming the failure it prevents and pointing at the code that applies it;
[`DESIGN.md`](app_cbor_persondb/DESIGN.md) carries the decisions behind them.
It ships a CI-sized configuration alongside the headline one, so the
fill → verify → mutate → re-verify cycle stays regression-tested even though a
10 000-person fill takes hours on real hardware.

## Configuration

Everything above L1 is à la carte: every module has its own Kconfig symbol,
disabled means zero flash and RAM, and selection flows downward — an L3
interface `select`s the container it needs, containers depend on `BLOB_DB`, and
`BLOB_DB` selects `FLASH`, `FLASH_MAP` and `CRC`. Invalid combinations are
unrepresentable.

| Use case | Enable | Image contains |
|---|---|---|
| String key/value store | `CONFIG_BLOBDB_KVDB=y` | blob_db + rootreg + kvhash + kvdb |
| Ids and blobs only, no containers | `CONFIG_BLOB_DB=y` | blob_db |
| Raw partition instead of UBI | `+ CONFIG_BLOB_DB_BACKEND_FLASH_AREA=y` | drops the UBI volume backend |
| Objects larger than one flash sector | `+ CONFIG_BLOB_DB_LARGE_PAYLOADS=y` | + segmented objects and partial access |

Frequently adjusted options (see the module `Kconfig` files for the rest):

| Option | Meaning |
|---|---|
| `CONFIG_BLOB_DB_PARTITION_LABEL` | fixed-partition label to store blobs in (default `storage`) |
| `CONFIG_UBI_MAX_NR_OF_DATA_PEBS` | UBI's static block pool; must match the partition's block count (UBI backend) |
| `CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT` | reformat when both master blocks are unreadable (default `y`; set `n` in production) |
| `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` | largest blob payload; also caps the kvhash bucket directory |
| `CONFIG_BLOB_DB_SECTOR_BUF_SIZE` | upper bound on supported flash sector size (64 KB for mx25r64) |
| `CONFIG_ROOTREG_MAX_ROOTS` | how many structure roots the registry can hold |
| `CONFIG_BLOB_DB_IOSTATS` | count flash operations and bytes at the storage seam; for benchmarks and regression guards, off in production |

### Large payloads

Partial access — `blob_db_size()`, `blob_db_read()` and `blob_db_write()`, so an
object larger than available RAM is still usable — is always available. What is
bounded by default is the object itself: a payload must fit one slot, so it is
capped by `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN`.

`CONFIG_BLOB_DB_LARGE_PAYLOADS=y` lifts that cap. An object too big for one slot
is stored as segment slots plus an index slot at the object's own id, written
last so the object appears atomically. Enabling it bumps the on-flash format
major, so a build without the option refuses such a store rather than misreading
an index record as data.

| Option | Meaning |
|---|---|
| `CONFIG_BLOB_DB_MAX_OBJECT_LEN` | largest object `update()` accepts (default 128 KB); mount refuses a build whose geometry cannot reach it |
| `CONFIG_BLOB_DB_MAX_SEGMENTS` | segments per object (default 128) — this is the RAM knob: two id tables at 8 B per segment, so 16 B each |
| `CONFIG_BLOB_DB_SEGMENT_LEN` | segment size; `0` derives it from the geometry |

## Documentation

Design documents live in [`doc/`](doc) and are split by intent: `doc/layers/`
holds **contracts** — everything an upper layer may rely on — while `doc/impl/`
holds **implementation designs**, which are feasibility proofs of those
contracts and must never be depended on from above.

| Document | Contents |
|---|---|
| [`doc/architecture.md`](doc/architecture.md) | the stack: layers, boundaries, composition model |
| [`doc/principles.md`](doc/principles.md) | binding design principles (P1–P8) for every layer |
| [`doc/layers/l0_flash.md`](doc/layers/l0_flash.md) | L0 — flash translation and the `blob_db_store` contract |
| [`doc/layers/l1_blob_db.md`](doc/layers/l1_blob_db.md) | L1 — `blob_db` contract & requirements |
| [`doc/layers/l1_model_container.md`](doc/layers/l1_model_container.md) | L1 — sufficiency proof + acceptance-test blueprint |
| [`doc/layers/l1_root_registry.md`](doc/layers/l1_root_registry.md) | L1½ — root registry: key → structure root |
| [`doc/layers/l2_containers.md`](doc/layers/l2_containers.md) | L2 — containers: seq, kvlist, kvhash, kvtree |
| [`doc/layers/l3_interfaces.md`](doc/layers/l3_interfaces.md) | L3 — access interfaces: kvdb, blobfs, settings |
| [`doc/impl/l1_bucketlog.md`](doc/impl/l1_bucketlog.md) | implementation design of the v1 bucket-log allocator |
| [`doc/impl/l0_backends.md`](doc/impl/l0_backends.md) | implementation design of the two L0 providers (`flash_area`, UBI) |
| [`doc/proposals/`](doc/proposals) | change proposals: analysis + design for a change spanning a contract and its implementation |
| [`doc/reviews/`](doc/reviews) | dated design-document reviews and their findings |

API reference lives in the public headers under
[`include/app/lib/`](include/app/lib) and is extracted by Doxygen.

**What the stack looks like from above** is documented separately, by the
application that hit it:
[`app_cbor_persondb/FINDINGS.md`](app_cbor_persondb/FINDINGS.md) is a register
of the stack's limitations as encountered while building a real dataset on it —
each entry with the measurement behind it, and marked `closed` rather than
deleted once `main` fixes it. It is the most direct answer to "what is
this stack still bad at".

### Building the documentation

```shell
cd doc
pip install -r requirements.txt
doxygen        # API docs   -> _build_doxygen/
make html      # design docs -> _build_sphinx/
```

Use the same Doxygen version as [CI](.github/workflows/docs.yml). Both outputs
are published together by the Documentation workflow.

## Repository layout

```
lib/
  blob_db/            L1  stable-id blob store (+ flash_area / UBI backends)
  rootreg/            L1½ root registry (owner of id = 1)
  containers/         L2  kvhash (+ seq / kvlist / kvtree / intent skeletons)
  kvdb/  blobfs/      L3  access interfaces
include/app/lib/      public headers — blob_db.h · rootreg.h · kvdb.h · blobfs.h
                      · containers/{shape_map,shape_seq,kvhash}.h
app/                  blob_db demo application
app_perf*/            benchmarks (+ hardware reference RESULTS.md)
                      app_perf_l0/ also carries the L0 timing model:
                      tools/l0_timing.py, models/, geometry/
app_cbor_persondb/    worked example & probe (README · DESIGN · FINDINGS · RESULTS)
tests/lib/            ztest suites: blob_db · blob_db_contract · rootreg · kvdb
tests/support/        shared test shims (crash injection)
doc/                  design documents; Sphinx + Doxygen setup
boards/               out-of-tree boards
drivers/  dts/        out-of-tree drivers and devicetree bindings
scripts/              west extension and runner examples
```

The repository started from the [Zephyr example application][example_app] and
still carries its scaffolding — the `blink` and `example_sensor` drivers, the
`custom` library, the `custom_plank` board, and the custom west extension and
runner. They are unrelated to the storage stack and serve as working references
for out-of-tree Zephyr structure.

[example_app]: https://github.com/zephyrproject-rtos/example-application

## Zephyr version

The manifest tracks Zephyr `main`. The `ubi` module currently points at the
`feature/leb-partial-update` branch of a fork, because the UBI backend needs the
in-place partial-update API (`ubi_leb_write_at`) that is still pending upstream;
[`west.yml`](west.yml) records the condition for flipping it back to a release
tag.

## License

Apache-2.0 — see [LICENSE](LICENSE).
