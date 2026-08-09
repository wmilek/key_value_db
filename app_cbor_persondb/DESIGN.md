# `app_cbor_persondb` — CBOR person/credential database

Status: **v0.2 — requirements settled, design proposal open for review.**
Implementation has not started.

The design document for this test application, kept beside the code it
describes. Governed by `doc/principles.md` · consumes the stack in
`doc/architecture.md`.

| | |
|---|---|
| Application | `app_cbor_persondb/` |
| This document | requirements (§1–§6) → design proposal (§7–§10) → as-built notes |
| `FINDINGS.md` | the flaw/limitation register — the *probe* output |
| `README.md` | the good-practices guide (R-F) — the *showcase* output |
| `RESULTS.md` | measured numbers, `native_sim` and nRF5340-DK |

---

## 1. Purpose

A test application that does two jobs at once, and is designed so that neither
job compromises the other:

1. **Showcase.** Build a real, non-trivial domain database the way the stack
   intends it to be built, and write down the practices that emerge. This is
   requirement R-F.
2. **Probe.** Push the stack hard enough that its flaws and limits become
   visible, and record every one of them. A limitation that this app trips over
   is a limitation a product would trip over.

These are compatible as long as one rule is respected:

> **No premature optimization.** The app implements the natural, idiomatic
> design. Where that design performs badly, the number is measured and recorded
> as a finding — it is *not* engineered around. Working around a weakness hides
> it, and hiding it defeats job 2.

The line between the two: a *good practice* is a way of using the API correctly
(ordering writes so a crash fails safe, keeping keys regenerable, opening
handles once). A *workaround* is a contortion that exists only because a layer
underperforms (caching, denormalizing, second indexes). The app does the first
and documents the second as an unimplemented mitigation in `FINDINGS.md`.

The domain is physical access control:

> A population of *persons*. Each person holds permissions and is assigned one
> or more *credentials* (card IDs). A reader presents a card ID and must decide
> whether that credential grants a given permission.

Good vehicle: it needs a serialization format with optional and repeated fields,
a secondary index for the query that actually matters, and enough scale that the
storage layer's real costs surface.

## 2. Scope

In scope:

- one test application in `app_cbor_persondb/`, built for `native_sim` and
  `nrf5340dk/nrf5340/cpuapp`, picked up by
  `west twister -T app_cbor_persondb` like the existing `app_perf*` apps;
- the good-practices guide (`README.md`);
- the findings register (`FINDINGS.md`);
- reference performance numbers at the specified scale (`RESULTS.md`).

Out of scope:

- **No changes to `lib/` or `include/app/lib/`.** The app is a *client* of the
  published API. A limitation it hits is written down, not patched around by
  editing the library. Fixes belong in follow-up work driven by `FINDINGS.md`.
- No new container or L3 interface.
- No reader hardware; credentials arrive as strings from a synthetic dataset.
- Not a security product. Access decisions are demonstrated, not hardened (no
  key material, no anti-replay, no secure element).

## 3. Stakeholder requirements

| Id | Requirement |
|---|---|
| **R-A** | Use the API already published by this repository. |
| **R-B** | Use **CBOR** as the serialized object format. |
| **R-C1** | The stored object is a **person**. |
| **R-C2** | A person carries **permissions, as an array of strings**. |
| **R-C3** | A person has **credential card IDs (strings)** assigned to them. |
| **R-D** | Given a credential, it must be possible to get the person and check a permission **quickly**. |
| **R-E** | Demonstrate performance with the database sized at **~50 % of the external flash available on the DK board** — 4 MiB of an 8 MiB MX25R6435F. |
| **R-F** | The application is delivered as a **set of good practices** for using the system. |
| **R-G** | Any drawback visible in a lower layer is **recorded as a finding**. |

## 4. Constraints the existing stack imposes

Read out of the tree and the existing hardware captures, not assumed.

| Id | Constraint | Source |
|---|---|---|
| **C1** | `kvdb` is the only implemented L3 interface and `kvhash` its only backend. No ordered iteration, no `foreach`. | `lib/kvdb/kvdb.c`, `doc/layers/l3_interfaces.md` §3 |
| **C2** | A `kvhash` instance holds at most `(MAX_PAYLOAD−8)/8` buckets of `MAX_PAYLOAD` bytes. `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` is capped at 4096 → **511 buckets × 4 KB ≈ 2.09 MB per named instance**. | `kvhash.c:47`, `lib/blob_db/Kconfig` |
| **C3** | One `kvhash` entry costs `4 + klen + vlen` and must fit a single payload. | `kvhash.c:291` |
| **C4** | Every `blob_db` operation reads a **whole sector** — 64 KB on the DK — so per-op cost is essentially independent of payload size. Measured: 16.9 ms per read; `kvdb_get` ≈ **34.9 ms**, `kvdb_set` ≈ **55.9 ms**. | `blob_db.c:870,928,990,1052`; `app_perf/RESULTS.md`, `app_perf_kvdb/RESULTS.md` |
| **C5** | A `blob_db` bucket is one sector; a blob lands in `id % n_buckets`. 8 MiB / 64 KB = 128 − 3 reserved = **125 buckets**. | `blob_db.c`, `blob_db_internal.h` |
| **C6** | Compacting one bucket costs **five 64 KB erases**; one erase measured at ~1.1 s. | `compact_commit()`; `app_perf_kvdb/RESULTS.md` |
| **C7** | Single-threaded: the caller serializes every call across every open instance. | `blob_db.h` §concurrency |
| **C8** | Each `kvdb_set`/`kvdb_delete` is individually atomic; **no multi-key transaction**. | `blob_db.h` §atomicity |
| **C9** | No occupancy or geometry introspection: the API exposes no partition size, sector size, free space or fill level. | `include/app/lib/blob_db.h` |

Three consequences drive the whole design and are lifted into requirements:

- **C2 ⇒** 10 000 person records (~3.6 MB) do not fit one `kvdb` instance. The
  app must shard across named instances by hand.
- **C8 ⇒** a person record and its credential-index entries cannot be updated
  together atomically. The app must define an ordering that fails safe.
- **C9 ⇒** the app cannot ask the stack how full it is. R-E reporting has to be
  reconstructed from devicetree plus the app's own accounting.

## 5. Derived functional requirements

| Id | Requirement | Rationale |
|---|---|---|
| **F1** | Person records are CBOR, stored under `kvdb`, keyed by person id. | R-A, R-B, R-C1 |
| **F2** | The schema carries id, display fields, a validity window, a PIN hash, permissions as an **array of text strings**, and card IDs as an **array of text strings**. | R-C2, R-C3 |
| **F3** | A **credential index** maps card ID → person id, because without it a card cannot be resolved at all (C1: no iteration). It stores the person id and nothing else — no denormalized permission state. | R-D, no-premature-optimization |
| **F4** | The person record is the single authoritative copy of permissions. | correctness |
| **F5** | Card assignment writes the person record **first**, the index **second**; revocation deletes the index **first**, the person **second**. Either crash point leaves a *deny*. | C8, P7 |
| **F6** | Every record is a pure function of its index, so any record is re-derivable and verifiable without shadow state. | C1, P3 |
| **F7** | Dataset scale is 10 000 persons (Kconfig), sized so live content lands near 4 MiB. | R-E |
| **F8** | Population is batched and **resumable across reboots** with committed progress; replay of an interrupted batch is idempotent. | R-E fill time, P7 |
| **F9** | Every run after the fill verifies a deterministic sample against the generator, mutates a bounded subset, and re-verifies — proving what the *previous boot* wrote. | P8 |
| **F10** | The run reports per-phase timings, the achieved size in bytes and per cent, and **read/write amplification**. | R-E, R-G |
| **F11** | Failures that reveal a limit (`-ENOSPC` from a full `kvhash` bucket) are **counted and reported**, not treated as fatal. | R-G |

## 6. Dataset sizing

Chosen to be a realistic access-control record, then checked against R-E — not
padded to hit a number. Averages over the 10 000 generated persons:

```
person record, CBOR map with 9 integer-keyed pairs
  1 id           uint          6 B      100000 + index
  2 name         tstr         18 B      "Ada Aalto-0042"
  3 dept         tstr         14 B
  4 title        tstr         16 B
  5 valid_from   uint          6 B      epoch seconds
  6 valid_until  uint          6 B
  7 pin          bstr(16)     18 B
  8 permissions  [tstr]      226 B      10..22 strings, mean 16, ~13 B each
  9 cards        [tstr]     39.5 B      1..4 strings, mean 2.5, 14 hex chars
                            -------
                            ~349.5 B
```

| | |
|---|---|
| person `kvhash` entry (`4 + klen 9 + 349.5`) | 362.5 B |
| credential entry (`4 + klen 14 + CBOR uint 5`) | 23 B |
| credentials per person (mean) | 2.5 |
| **per person, all-in** | **420 B** |
| × 10 000 persons | **4 200 000 B = 4.01 MiB** |
| as a fraction of the 8 MiB MX25R6435F | **50.1 %** |

R-E is met by the realistic record, which is the outcome we wanted: no padding,
no contrivance.

**Shard count** follows from C2/C3 plus the hash distribution. With 511 buckets
per instance, `S` people shards give a mean of `10000/(511·S)` entries per
bucket; a bucket overflows past ~11 person entries. At `S = 8` the mean is 2.45
and the expected number of buckets reaching 12 entries is 0.03 — safe, with the
tail handled by F11 rather than by extra margin. Credentials are small enough
that all 25 000 fit **one** shard (mean 49 entries ≈ 1.1 KB per bucket).

That asymmetry — the same 511-bucket instance holds 25 000 small entries
comfortably but only ~5 600 large ones — is finding **K2**.

**Cost of the fill.** 35 000 `kvdb_set` calls at 55.9 ms ≈ 33 min of operations,
plus ~37 MB of appended bytes driving ~1 250 bucket compactions at ~5.7 s ≈ 2 h.
**One-time population on the DK is therefore ≈ 2.5 h.** This is not designed
around; it is finding **B1/B2/K4/K5** and is why F8 exists.

---

# Design proposal

## 7. Structure

```
app_cbor_persondb/
├── DESIGN.md  FINDINGS.md  README.md  RESULTS.md
├── CMakeLists.txt  Kconfig  prj.conf  sample.yaml  VERSION
├── boards/  native_sim.{conf,overlay}
│            nrf5340dk_nrf5340_cpuapp.{conf,overlay}
└── src/
    ├── person.h/.c     domain type + CBOR codec (zcbor). Knows nothing of storage.
    ├── dataset.h/.c    pure generator: index → person. Knows nothing of storage or CBOR.
    ├── persondb.h/.c   the store: shard fan-out, record + credential index, auth.
    └── main.c          phases, benchmarks, reporting.
```

The split is itself a demonstrated practice: the codec, the domain data and the
storage policy are three independent things, and only `persondb.c` calls `kvdb`.

## 8. Data model

**Person record** — CBOR map, integer keys (compact and stable), canonical
encoding (`CONFIG_ZCBOR_CANONICAL=y`). Schema as in §6. Permissions and card
IDs are text strings, per R-C2/R-C3.

**Credential index** — a separate `kvdb` instance, `card ID → CBOR uint person
id`. Nothing else is stored in it. The temptation to also cache a permission
bitmask here is exactly the premature optimization the app refuses; §10 measures
what that refusal costs and `FINDINGS.md` records it as an unimplemented
mitigation.

**Instances** (10 total, all `KVDB_BACKEND_HASH`, `initial_capacity = 511`):

| Name | Contents |
|---|---|
| `ppl0`..`ppl7` | person id → CBOR person record |
| `crd0` | card ID → CBOR person id |
| `meta` | header, population progress, revision counter |

Shard selection is `person_index % 8` for people and a single instance for
cards. Keys: `"p%08X"` for persons, the 14-hex-char card UID verbatim for
credentials.

## 9. Behaviour

**Boot.** `blob_db_mount()` → `rootreg_init()` → open all 10 instances once and
keep the handles (an instance handle is a `{root, ops}` pair with no resources —
reopening per query would cost two extra sector reads).

**Fill (F8).** Read `meta/prog`. Populate persons `[done, done+BATCH)`, commit
`prog`, repeat. On reboot the fill resumes from the committed count; replaying a
partial batch rewrites the same deterministic values, so replay is free.
`blob_db_prepare()` runs first so the one-off ~1.1 s sector erases are reported
as their own phase rather than smeared through the measurement.

**Verify (F9).** A deterministic sample of persons is re-derived from the
generator and compared field by field against what CBOR decoding returns, and
each person's cards are resolved through the index back to that person.

**Mutate (F9).** A bounded subset gains a temporary card at odd revisions and
loses it at even ones, so the expected state is a pure function of
`(index, rev)` and a rerun proves the previous boot's writes survived. The two
orderings of F5 are both exercised — assignment person-first, revocation
index-first.

**Report (F10).** Live bytes by the app's own accounting, blob-level live bytes
via `blob_db_iterate()`, partition size from `FIXED_PARTITION_SIZE()` — read
from devicetree behind the library's back, because C9 leaves no other route.
That detour is finding **B3**.

## 10. Measurements

| Bench | What it costs | Why |
|---|---|---|
| `auth` | card → person id → record → permission string compare (**2 `kvdb_get`**) | R-D, the headline |
| `byid` | person id → record (1 `kvdb_get`) | the index's share of `auth` |
| `miss` | unknown card (1 `kvdb_get`, `-ENOENT`) | negative-lookup cost |
| `grant` | read-modify-write one record (1 get + 1 set) | mutation cost |
| `cbor` | encode + decode in RAM, no flash | isolates the codec |
| `fill` | the whole population, with compaction visible | R-E |

Every phase also reports **amplification**: bytes moved on flash ÷ bytes the
application asked for. For `auth` that is 4 × 64 KB of sector reads to answer a
question about ~365 B of data — the number that makes finding **B1** concrete
rather than theoretical.

`cbor` exists to answer "is CBOR the bottleneck?" in advance. The expected answer
is no, by three orders of magnitude, which retires the question.

## 11. Acceptance criteria

- **A1** Builds for `native_sim` and `nrf5340dk/nrf5340/cpuapp`; passes
  `west twister -T app_cbor_persondb -p native_sim`.
- **A2** On `native_sim` configured with the DK's geometry (8 MiB partition,
  64 KB sectors) a full run reaches 10 000 persons and reports `VERIFY PASS`.
- **A3** A second run verifies content written by the first.
- **A4** `RESULTS.md` carries measured nRF5340-DK numbers.
- **A5** `FINDINGS.md` records every limitation hit, each with the evidence that
  demonstrates it and the measurement that quantifies it.
- **A6** `README.md` states each practice, points at the code that implements
  it, and names the failure it prevents.

## 12. Decisions

Settled during review; recorded so they are not relitigated.

| # | Decision |
|---|---|
| D1 | **Scale is 10 000 persons**, and the record is realistic rather than sized to hit a target. It lands at 4.01 MiB ≈ 50 % of the DK's external flash. |
| D2 | **No premature optimization.** The natural design is implemented and measured; mitigations are documented, not built. |
| D3 | **zcbor** is the codec — Zephyr's own CBOR library (P1). Requires adding `zcbor` to the `name-allowlist` in `west.yml`. |
| D4 | **4 MiB of the 8 MiB MX25R6435F** is the R-E target; both existing DK overlays already give the whole part to `storage_partition`. |
| D5 | **1–4 credentials per person** (mean 2.5), varying per person, as in a real population. |
| D6 | **Hardware numbers will be measured on the DK**, so `RESULTS.md` ships measurements rather than projections. |
| D7 | The app is `app_cbor_persondb/` and **all of its documentation lives inside it**. Nothing is added under `doc/`. |
| D8 | The app is **both** probe and showcase; §1 states the rule that keeps the two compatible. |
