# `app_cbor_persondb` — CBOR person/credential database

Status: **v0.3 — requirements settled; design proposal open for review.**
Implementation has not started.

The design document for this test application, kept beside the code it
describes. Governed by `doc/principles.md` · consumes the stack in
`doc/architecture.md`.

| | |
|---|---|
| Application | `app_cbor_persondb/` |
| This document | requirements (§1–§6) → design proposal (§7–§13) → as-built notes |
| `FINDINGS.md` | the flaw/limitation register — the *probe* output |
| `README.md` | the good-practices guide (R-F) — the *showcase* output |
| `RESULTS.md` | measured numbers, `native_sim` and nRF5340-DK |

---

## 1. Purpose

A test application that does two jobs at once, designed so neither compromises
the other:

1. **Showcase.** Build a real, non-trivial domain database the way the stack
   intends it to be built, and write down the practices that emerge (R-F).
2. **Probe.** Push the stack hard enough that its flaws and limits become
   visible, and record every one (R-G). A limitation this app trips over is a
   limitation a product would trip over.

One rule keeps them compatible:

> **No premature optimization.** The app implements the natural, idiomatic
> design. Where that design performs badly, the number is measured and recorded
> as a finding — it is *not* engineered around. Working around a weakness hides
> it, and hiding it defeats job 2.

The line: a *good practice* is using the API correctly (choosing the right
layer, ordering writes so a crash fails safe, keeping keys regenerable, opening
handles once). A *workaround* is a contortion that exists only because a layer
underperforms (caching, denormalizing, shadow indexes). The app does the first
and records the second in `FINDINGS.md` as an unimplemented mitigation.

The domain is physical access control:

> A population of *persons*. Each person holds permissions and is assigned one
> or more *credentials* (card IDs). A reader presents a card ID and must decide
> whether that credential grants a given permission.

## 2. Scope

In scope:

- one test application in `app_cbor_persondb/`, built for `native_sim` and
  `nrf5340dk/nrf5340/cpuapp`, picked up by `west twister -T app_cbor_persondb`;
- an **internal Person Access API** (§8) that the rest of the application is
  written against — the app's own domain contract, not a new library;
- the good-practices guide (`README.md`), findings register (`FINDINGS.md`) and
  reference numbers (`RESULTS.md`).

Out of scope:

- **No changes to `lib/` or `include/app/lib/`.** The app is a *client*. A
  limitation it hits is written down, not patched around by editing the
  library. Fixes are follow-up work driven by `FINDINGS.md`.
- No new container or L3 interface. The internal API of §8 lives in the
  application; whether any of it deserves promotion into `lib/` is a question
  `FINDINGS.md` raises, not one this app answers.
- No reader hardware; credentials arrive as strings from a synthetic dataset.
- Not a security product. Access decisions are demonstrated, not hardened.

## 3. Stakeholder requirements

| Id | Requirement |
|---|---|
| **R-A** | Use the API already published by this repository. |
| **R-B** | Use **CBOR** as the serialized object format. |
| **R-C1** | The stored object is a **person**. |
| **R-C2** | A person carries **permissions, as an array of strings**. |
| **R-C3** | A person has **credential card IDs (strings)** assigned to them. |
| **R-D** | Given a credential, it must be possible to get the person and check a permission **quickly**. |
| **R-E** | Demonstrate performance with the database at **~50 % of the external flash on the DK** — 4 MiB of an 8 MiB MX25R6435F. |
| **R-F** | Delivered as a **set of good practices** for using the system. |
| **R-G** | Any drawback visible in a lower layer is **recorded as a finding**. |
| **R-H** | The person-access functionality is an **API internal to the application**. It may build on **L2** rather than being confined to L3. |

## 4. Constraints the existing stack imposes

Read out of the tree and the existing hardware captures, not assumed.

| Id | Constraint | Source |
|---|---|---|
| **C1** | `kvhash` is the only implemented Map provider and `kvdb` the only L3 interface. No ordered iteration, no `foreach`, at either layer. | `lib/kvdb/kvdb.c`, `doc/layers/l3_interfaces.md` §3 |
| **C2** | A `kvhash` instance holds at most `(MAX_PAYLOAD−8)/8` buckets of `MAX_PAYLOAD` bytes. `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` is capped at 4096 → **511 buckets × 4 KB ≈ 2.09 MB per map**. | `kvhash.c:45-50`, `lib/blob_db/Kconfig` |
| **C3** | One `kvhash` entry costs `4 + klen + vlen` and must fit a single payload. | `kvhash.c:291` |
| **C4** | Every `blob_db` operation reads a **whole sector** — 64 KB on the DK — so per-op cost is essentially independent of payload size. Measured: 16.9 ms/read; `kvdb_get` ≈ **34.9 ms**, `kvdb_set` ≈ **55.9 ms**. | `blob_db.c:870,928,990,1052`; `app_perf*/RESULTS.md` |
| **C5** | A `blob_db` bucket is one sector; a blob lands in `id % n_buckets`. 8 MiB / 64 KB = 128 − 3 reserved = **125 buckets**. | `blob_db_internal.h` |
| **C6** | Compacting one bucket costs **five 64 KB erases**; one erase measured at ~1.1 s. | `compact_commit()`; `app_perf_kvdb/RESULTS.md` |
| **C7** | Single-threaded: the caller serializes every call across every open instance — including across *different* handles, since `kvhash` scratch buffers are file-scope. | `blob_db.h`; `kvhash.c:54-55` |
| **C8** | Each `blob_db_update`/`delete` is individually atomic; **no multi-key or multi-blob transaction**. | `blob_db.h` §atomicity |
| **C9** | No occupancy or geometry introspection at any layer: no partition size, sector size, free space, fill level, or per-map entry count. | `blob_db.h`, `shape_map.h` |
| **C10** | `CONFIG_ROOTREG_MAX_ROOTS` defaults to **8**. | `lib/rootreg/Kconfig` |

Consequences that drive the design:

- **C2 ⇒** 10 000 person records (~3.6 MB) do not fit one map. The app must
  shard, and nothing in L2 or L3 helps it do so.
- **C8 ⇒** a person record and its credential-index entries cannot be updated
  together atomically. The app must define an ordering that fails safe.
- **C9 ⇒** the app cannot ask the stack how full it is; R-E reporting has to be
  reconstructed from devicetree plus the app's own accounting.

## 5. Derived functional requirements

| Id | Requirement | Rationale |
|---|---|---|
| **F1** | Person records are CBOR, keyed by person id. | R-A, R-B, R-C1 |
| **F2** | The schema carries id, display fields, a validity window, a PIN hash, permissions as an **array of text strings**, and card IDs as an **array of text strings**. | R-C2, R-C3 |
| **F3** | A **credential index** maps card ID → person id, because without it a card cannot be resolved at all (C1: no iteration). It stores the person id and nothing else. | R-D, no-premature-optimization |
| **F4** | The person record is the single authoritative copy of permissions. | correctness |
| **F5** | Card assignment writes the person record **first**, the index **second**; revocation deletes the index **first**, the person **second**. Either crash point leaves a *deny*. | C8, P7 |
| **F6** | Every record is a pure function of its index, so any record is re-derivable and verifiable without shadow state. | C1, P3 |
| **F7** | Scale is 10 000 persons (Kconfig), sized so live content lands near 4 MiB. | R-E |
| **F8** | Population is batched and **resumable across reboots** with committed progress; replay of an interrupted batch is idempotent. | R-E fill time, P7 |
| **F9** | Every run after the fill verifies a deterministic sample against the generator, mutates a bounded subset, and re-verifies — proving what the *previous boot* wrote. | P8 |
| **F10** | The run reports per-phase timings, achieved size in bytes and per cent, and **read/write amplification**. | R-E, R-G |
| **F11** | Failures that reveal a limit (`-ENOSPC` from a full `kvhash` bucket) are **counted and reported**, not fatal. | R-G |
| **F12** | All application code above the storage layer speaks only the internal Person Access API (§8) — domain vocabulary, no keys, no shards, no CBOR, no ids. | R-H |
| **F13** | The whole database is reachable from **one** registry key, via an app-owned superblock. | P5, R-H |

## 6. Dataset sizing

A realistic access-control record, then checked against R-E — not padded to hit
a number. Averages over the 10 000 generated persons:

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
| person entry (`4 + klen 9 + 349.5`) | 362.5 B |
| credential entry (`4 + klen 14 + CBOR uint 5`) | 23 B |
| credentials per person (mean) | 2.5 |
| **per person, all-in** | **420 B** |
| × 10 000 persons | **4 200 000 B = 4.01 MiB** |
| of the 8 MiB MX25R6435F | **50.1 %** |

R-E is met by the realistic record — the outcome we wanted.

**Shard count** follows from C2/C3 plus the hash distribution. With 511 buckets
per map, `S` people shards give a mean of `10000/(511·S)` entries per bucket; a
bucket overflows past ~11 person entries. At `S = 8` the mean is 2.45 and the
expected number of buckets reaching 12 is 0.03 — safe, with the tail handled by
F11 rather than by extra margin. Credentials are small enough that all 25 000
fit **one** map (mean 49 entries ≈ 1.1 KB per bucket).

That asymmetry — the same 511-bucket map holds 25 000 small entries comfortably
but only ~5 600 large ones — is finding **K2**.

**Cost of the fill.** 35 000 map writes at ~55.9 ms ≈ 33 min, plus ~37 MB of
appended bytes driving ~1 250 compactions at ~5.7 s ≈ 2 h. **One-time
population on the DK is therefore ≈ 2.5 h.** Not designed around; it is
findings **B1/B2/K4/K5**, and it is why F8 exists.

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
    ├── access.h        THE INTERNAL API (§8) — domain vocabulary only
    ├── access.c        its implementation on L2 + rootreg + blob_db (§9)
    ├── person_cbor.h/.c  CBOR codec (zcbor). Knows nothing about storage.
    ├── dataset.h/.c    pure generator: index → person. Knows nothing about
    │                   storage or CBOR.
    └── main.c          phases, benchmarks, reporting. Calls only access.h.
```

Four modules with one dependency direction, mirroring the stack's own P6:

```
main.c ──► access.h ──► access.c ──► kvhash map_ops ──► rootreg ──► blob_db
              ▲                └────► person_cbor ◄──── dataset
              └── dataset (to build inputs and to verify outputs)
```

`main.c` never sees a key, a shard, an id or a CBOR byte. That separation is
itself a demonstrated practice, and it is what makes the storage layout in §9
replaceable without touching the benchmark or the verification logic.

## 8. The internal Person Access API (R-H)

`src/access.h`. Domain operations only — no storage vocabulary escapes it.

```c
struct access_person {                   /* fixed capacity, no heap (P2/P3) */
        uint32_t id;
        char     name [ACCESS_NAME_MAX  + 1];
        char     dept [ACCESS_DEPT_MAX  + 1];
        char     title[ACCESS_TITLE_MAX + 1];
        uint32_t valid_from, valid_until;      /* epoch seconds */
        uint8_t  pin_hash[16];
        uint8_t  n_perms;
        char     perm[ACCESS_PERMS_MAX][ACCESS_PERM_MAX + 1];
        uint8_t  n_cards;
        char     card[ACCESS_CARDS_MAX][ACCESS_CARD_LEN + 1];
};

enum access_decision {
        ACCESS_GRANTED, ACCESS_DENIED, ACCESS_UNKNOWN_CARD, ACCESS_EXPIRED,
};

/* lifecycle */
int access_open (struct access_db *db);   /* attach, or build a virgin store */
int access_close(struct access_db *db);

/* enrollment */
int access_person_put   (struct access_db *db, const struct access_person *p);
int access_person_get   (struct access_db *db, uint32_t id,
                         struct access_person *out);
int access_person_delete(struct access_db *db, uint32_t id);

/* credentials — the orderings of F5 live here, not in the caller */
int access_card_assign(struct access_db *db, uint32_t person_id, const char *card);
int access_card_revoke(struct access_db *db, const char *card);
int access_card_owner (struct access_db *db, const char *card, uint32_t *person_id);

/* permissions */
int access_permission_grant (struct access_db *db, uint32_t id, const char *perm);
int access_permission_revoke(struct access_db *db, uint32_t id, const char *perm);

/* the decision — R-D */
int access_check(struct access_db *db, const char *card, const char *perm,
                 uint32_t now, enum access_decision *out,
                 struct access_person *who /* optional */);

/* app-owned persistent state, so main.c needs no store of its own */
int access_progress_get(struct access_db *db, uint32_t *populated, uint32_t *rev);
int access_progress_set(struct access_db *db, uint32_t  populated, uint32_t  rev);

/* introspection for the probe (F10/F11) */
int access_stat(struct access_db *db, struct access_stat *st);
```

`access_check` is deliberately implemented the obvious way — resolve the card,
load the person through `access_person_get`, check the validity window, compare
the permission string. It does **not** shortcut the decode, and the credential
index stores nothing but a person id. What that costs is measured in §11; the
shortcut is recorded as an unimplemented mitigation.

## 9. Storage layout — and why L2 rather than L3

**The layout.** One registry key, one app-owned superblock, ten maps:

```
rootreg[ ROOTREG_KEY('PADB', 1) ] ──► superblock blob   (CBOR, app-owned)
                                        1 version
                                        2 [ root_id ]×8   people shards
                                        3 root_id         credential index
                                        4 n_persons, populated, rev
                                            │
        kvhash_map_ops on each root ────────┘
```

Everything is reachable from the integer 1 (P5): `rootreg_get` → superblock id
→ `blob_db_get` → nine map roots. Boot costs **two** sector reads.

Person key `"p%08X"`, credential key the 14-hex-char UID verbatim, shard
`person_index % 8`.

**Why not L3.** `kvdb` was the obvious starting point and does not fit this
shape. The comparison, for nine maps:

| | L3 — nine `kvdb` instances | L2 — superblock + nine roots |
|---|---|---|
| Boot | 9 × (registry read + meta-blob read) = **18 sector reads** | registry + superblock = **2** |
| Persistent overhead | 9 registry entries + 9 × 32 B meta blobs | 1 entry + 1 superblock |
| Registry capacity | 9 of `ROOTREG_MAX_ROOTS`, which defaults to **8** (C10) — does not fit out of the box | 1 |
| Progress commit | `kvdb_set` = 2 reads + 1 write | `blob_db_update` = 1 read + 1 write |
| Per-operation cost | identical — `kvdb_get` is `strlen` + `ops->get` | identical |
| Shard fan-out, naming, capacity accounting | the app's problem | the app's problem |
| What it buys | named instances; backend recorded per instance | direct control of the root graph |

L3 costs nine times the boot I/O and blows the default registry capacity, in
exchange for a naming feature this app does not need — it has one superblock and
knows its own shards. This is R-H's "may use L2", and it is the practice worth
teaching: **use the highest layer whose shape matches; drop down when it
doesn't.** It is also finding **V4** stated from the other side — once you shard,
`kvdb` stops being useful, which bounds L3's current audience to small stores of
a few named instances.

**Creation and the crash window.** `rootreg_get_or_create` may return an
allocated-but-unbound id (its contract, `rootreg.h`). `access_open` detects the
virgin case with `blob_db_get(sb_id) == -ENOENT`, allocates nine ids, calls
`kvhash_map_ops.create` on each, and binds the superblock **last** — the single
atomic write that publishes the whole structure. A crash before that write
leaves the maps unreferenced. `blob_db` has no reachability GC and compaction
reclaims only tombstones and superseded slots, so those blobs are lost until a
format: finding **B8**, and the same window `kvdb` itself has (`kvdb.c:118`).

## 10. Behaviour

**Boot.** `blob_db_mount()` → `rootreg_init()` → `access_open()`.

**Fill (F8).** `access_progress_get()`; populate persons `[done, done+BATCH)`
via `access_person_put` + `access_card_assign`; `access_progress_set()`; repeat.
On reboot the fill resumes from the committed count, and replaying a partial
batch rewrites the same deterministic values, so replay is free.
`blob_db_prepare()` runs first so the one-off ~1.1 s sector erases are reported
as their own phase instead of smearing through the measurement.

**Verify (F9).** A deterministic sample is re-derived by `dataset` and compared
field by field against `access_person_get`, and each person's cards are resolved
through `access_card_owner` back to that person.

**Mutate (F9).** A bounded subset gains a temporary card at odd revisions and
loses it at even ones, so expected state is a pure function of `(index, rev)`
and a rerun proves the previous boot's writes survived. Both orderings of F5 are
exercised — assignment person-first, revocation index-first.

**Report (F10).** Live bytes from the app's accounting; blob-level live bytes
via `blob_db_iterate()`; partition size from `FIXED_PARTITION_SIZE()`, read from
devicetree behind the library's back because C9 leaves no other route — finding
**B3**.

## 11. Measurements

| Bench | Cost | Why |
|---|---|---|
| `check` | card → person id → record → permission compare (**2 map gets**) | R-D, the headline |
| `byid` | person id → record (1 map get) | the index's share of `check` |
| `miss` | unknown card (1 map get, `-ENOENT`) | negative-lookup cost |
| `grant` | read-modify-write one record (1 get + 1 set) | mutation cost |
| `cbor` | encode + decode in RAM, no flash | isolates the codec |
| `fill` | the whole population, compaction visible | R-E |

Every phase also reports **amplification**: bytes moved on flash ÷ bytes the
application asked for. For `check` that is 4 × 64 KB of sector reads to answer a
question about ~365 B — the number that makes finding **B1** concrete.

`cbor` answers "is CBOR the bottleneck?" in advance. Expected answer: no, by
three orders of magnitude — which retires the question and justifies R-B costing
nothing.

## 12. Acceptance criteria

- **A1** Builds for `native_sim` and `nrf5340dk/nrf5340/cpuapp`; passes
  `west twister -T app_cbor_persondb -p native_sim`.
- **A2** On `native_sim` with the DK's geometry (8 MiB partition, 64 KB sectors)
  a full run reaches 10 000 persons and reports `VERIFY PASS`.
- **A3** A second run verifies content written by the first.
- **A4** `RESULTS.md` carries measured nRF5340-DK numbers.
- **A5** `FINDINGS.md` records every limitation hit, with the evidence that
  demonstrates it and the measurement that quantifies it.
- **A6** `README.md` states each practice, points at the code that implements
  it, and names the failure it prevents.
- **A7** `main.c` contains no key string, shard index, blob id or CBOR call —
  the check that F12 actually held.

## 13. Decisions

Settled during review; recorded so they are not relitigated.

| # | Decision |
|---|---|
| D1 | **Scale is 10 000 persons**, with a realistic record rather than one sized to hit a target. It lands at 4.01 MiB ≈ 50 % of the DK's external flash. |
| D2 | **No premature optimization.** The natural design is implemented and measured; mitigations are documented, not built. |
| D3 | **zcbor** is the codec — Zephyr's own CBOR library (P1). Requires adding `zcbor` to the `name-allowlist` in `west.yml`. |
| D4 | **4 MiB of the 8 MiB MX25R6435F** is the R-E target. |
| D5 | **1–4 credentials per person** (mean 2.5), varying per person. |
| D6 | **Hardware numbers measured on the DK**, so `RESULTS.md` ships measurements. |
| D7 | The app is `app_cbor_persondb/` and **all its documentation lives inside it**. |
| D8 | The app is **both** probe and showcase; §1 states the rule that keeps the two compatible. |
| D9 | The person-access functionality is an **internal application API** (§8). It is not proposed for `lib/`; whether part of it should be promoted is a question for `FINDINGS.md`. |
| D10 | That API is implemented on **L2 `map_ops` + `rootreg` + `blob_db`**, not on L3 `kvdb`, for the reasons tabulated in §9. `kvdb` is not linked into the image. |
