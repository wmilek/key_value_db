# `app_cbor_persondb` — CBOR person/credential database

Status: **v0.6 — implemented, measured on `native_sim` at full scale and on
the nRF5340-DK at 1 000 persons.** A4 remains open: the 10 000-person board run
has not been done, and the DK figures in `RESULTS.md` §5 are a tenth-scale
store, not the benchmark's (`FINDINGS.md` N1).

The design document for this test application, kept beside the code it
describes. Governed by `doc/principles.md` · consumes the stack in
`doc/architecture.md`.

| | |
|---|---|
| Application | `app_cbor_persondb/` |
| This document | requirements (§1–§6) → design proposal (§7–§14) → as-built notes |
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

> **No premature optimization, and no fighting the implementation.** The app
> implements the natural, idiomatic design and stays inside the limits it is
> given. Where that design performs badly, the number is measured and recorded
> as a finding — it is *not* engineered around. Working around a weakness hides
> it, and hiding it defeats job 2. Running deliberately at a limit to make it
> fail is the same mistake from the other side: it produces a failure the app
> provoked rather than one a product would hit.

The line: a *good practice* is using the API correctly (choosing the right
layer, sizing inside the documented capacity, ordering writes so a crash fails
safe, keeping keys regenerable, opening handles once). A *workaround* is a
contortion that exists only because a layer underperforms (caching,
denormalizing, shadow indexes, overflow chains). The app does the first, and
records the second in `FINDINGS.md` as an unimplemented mitigation.

The domain is physical access control:

> A population of *persons*. Each person holds permissions and is assigned one
> or more *credentials* (card IDs). A reader presents a card ID and must decide
> whether that credential grants a given permission.

## 2. Scope

In scope:

- one test application in `app_cbor_persondb/`, built for `native_sim` and
  `nrf5340dk/nrf5340/cpuapp`;
- a **person management API** internal to the application (§8) — the domain
  contract, not a new library;
- a **scenario layer** above it (§9) holding everything a caller does *with* a
  population — fill, verify, mutate, measure — so that the frontends stay thin;
- **two frontends** over that layer, selected by Kconfig and built as separate
  `sample.yaml` scenarios (§10): an interactive shell and an automatic
  benchmark;
- the good-practices guide (`README.md`), findings register (`FINDINGS.md`) and
  reference numbers (`RESULTS.md`).

Out of scope:

- **No changes to `lib/` or `include/app/lib/`.** The app is a *client*. A
  limitation it hits is written down, not patched around by editing the
  library. Fixes are follow-up work driven by `FINDINGS.md`.
- No new container or L3 interface. Whether any of §8 deserves promotion into
  `lib/` is a question `FINDINGS.md` raises, not one this app answers.
- No reader hardware; credentials are strings from a synthetic dataset.
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
| **R-E** | Demonstrate performance at a **representative** database size. "~50 % of the DK's 8 MiB external flash" is the *heuristic used once to choose that size*, not a level to hold — see §6.3. |
| **R-F** | Delivered as a **set of good practices** for using the system. |
| **R-G** | Any drawback visible in a lower layer is **recorded as a finding**. |
| **R-H** | The person-management functionality is an **API internal to the application**. It may build on **L2** rather than being confined to L3. |
| **R-I** | A **separate layer** sits between that API and the test using it, so more than one frontend can share it. |
| **R-J** | Stay **inside** the implementation's limits: size the dataset to what fits rather than working around a capacity ceiling. |

## 4. Constraints the existing stack imposes

Read out of the tree and the existing hardware captures, not assumed.

| Id | Constraint | Source |
|---|---|---|
| **C1** | `kvhash` is the only implemented Map provider and `kvdb` the only L3 interface. No ordered iteration, no `foreach`, at either layer. | `lib/kvdb/kvdb.c`, `doc/layers/l3_interfaces.md` §3 |
| **C2** | A `kvhash` map holds at most `(MAX_PAYLOAD−8)/8` buckets of `MAX_PAYLOAD` bytes, and a *single bucket* is the real limit: it overflows at `MAX_PAYLOAD` of packed entries regardless of how empty the store is. ~~`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` is capped at 4096~~ — **the cap moved**: the range is now 1..65535, bounded at mount by the geometry to `(sector − 16) / 2 − 14` = **32 746 B on 64 KB sectors**, so a map reaches 4 092 buckets. The app still configures 4 096 → **511 buckets × 4 KB ≈ 2.09 MB per map**, and §12.1 is why that is now a choice rather than a ceiling. | `kvhash.c:45-50,291`, `lib/blob_db/Kconfig`, `blob_db.c:490-503` |
| **C3** | One `kvhash` entry costs `4 + klen + vlen` and must fit a single payload. | `kvhash.c:291` |
| **C4** | ~~Every `blob_db` operation reads a whole sector.~~ **No longer true** — `main`'s slot-header walk reads only the slots it needs (this app's finding B1). A read is now many small transactions rather than one large one: measured on the DK, a map get ≈ **7 ms**, a map set ≈ **42 ms**, and a transaction costs ~65 µs (`RESULTS.md` §5b, `FINDINGS.md` N1). | `blob_db.c` slot walk; `RESULTS.md` §4, §5b |
| **C5** | A `blob_db` bucket is one sector; a blob lands in `id % n_buckets`. 8 MiB / 64 KB = 128 − 3 reserved = **125 buckets**. | `blob_db_internal.h` |
| **C6** | Compacting one bucket costs **five 64 KB erases**; one erase measured at ~1.1 s. | `compact_commit()`; `app_perf_kvdb/RESULTS.md` |
| **C7** | Single-threaded: the caller serializes every call across every open map — including across *different* roots, since `kvhash` scratch buffers are file-scope. | `blob_db.h`; `kvhash.c:54-55` |
| **C8** | Each `blob_db_update`/`delete` is individually atomic; **no multi-key or multi-blob transaction**. | `blob_db.h` §atomicity |
| **C9** | No occupancy or geometry introspection at any layer: no partition size, sector size, free space, fill level, or per-map entry count. | `blob_db.h`, `shape_map.h` |
| **C10** | `CONFIG_ROOTREG_MAX_ROOTS` defaults to **8**. | `lib/rootreg/Kconfig` |

Consequences that drive the design:

- **C2 ⇒** 10 000 person records (~3.6 MB) do not fit one map *at this payload
  size*, and per-R-J the answer is to spread them over enough maps that every
  bucket sits well inside 4 KB — not to add overflow handling. §6 does that
  arithmetic. Since the cap moved they *would* fit one map at a larger payload;
  §12.1 measures what that costs and why the app does not do it.
- **C8 ⇒** a person record and its credential-index entries cannot be updated
  together atomically. The app must define an ordering that fails safe.
- **C9 ⇒** the app cannot ask the stack how full it is, or how close a bucket is
  to C2's ceiling. R-E reporting is reconstructed from devicetree plus the app's
  own accounting, and margin has to be chosen blind — see §6.

## 5. Derived functional requirements

| Id | Requirement | Rationale |
|---|---|---|
| **F1** | Person records are CBOR, keyed by person id. | R-A, R-B, R-C1 |
| **F2** | The schema carries id, display fields, a validity window, a PIN hash, permissions as an **array of text strings**, and card IDs as an **array of text strings**. | R-C2, R-C3 |
| **F3** | A **credential index** maps card ID → person id, because without it a card cannot be resolved at all (C1). It stores the person id and nothing else. | R-D, no-premature-optimization |
| **F4** | The person record is the single authoritative copy of permissions. | correctness |
| **F5** | Card assignment writes the person record **first**, the index **second**; revocation deletes the index **first**, the person **second**. Either crash point leaves a *deny*. | C8, P7 |
| **F6** | Every record is a pure function of its index, so any record is re-derivable and verifiable without shadow state. | C1, P3 |
| **F7** | Scale is **fixed at 10 000 persons**, spread over enough maps that no bucket approaches C2's ceiling. The count is a constant of the benchmark, never re-derived from geometry. | R-E, R-J |
| **F7a** | Fill percentage is an **output**, never an input. Nothing in the app is computed from the partition size. | R-E (§6.3) |
| **F8** | Population is batched and **resumable across reboots** with committed progress; replay of an interrupted batch is idempotent. | R-E fill time, P7 |
| **F9** | A run verifies a deterministic sample against the generator, mutates a bounded subset, and re-verifies — proving what the *previous boot* wrote. | P8 |
| **F10** | Per-phase timings, achieved size in bytes and per cent, and **read/write amplification** are reported. | R-E, R-G |
| **F11** | `-ENOSPC` from a full bucket is **counted and reported**, never fatal. It is not an expected path (F7 sizes it away); if it fires, the sizing rule in §6 was wrong and that is the finding. | R-G |
| **F12** | Application code above the person management API speaks only domain vocabulary — no keys, shards, blob ids or CBOR. | R-H |
| **F13** | The whole database is reachable from **one** registry key, via an app-owned superblock. | P5, R-H |
| **F14** | Everything done *with* a population — fill, verify, mutate, benchmark, report — lives in the scenario layer (§9), not in a frontend. | R-I |
| **F15** | The frontends contain no storage or dataset logic; each is argument parsing and printing over §9. | R-I |

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
                            ~349.5 B     (worst case ~660 B)
```

| | |
|---|---|
| | estimated | **measured** |
|---|--:|--:|
| person entry (`4 + klen 9 + record`) | 362.5 B | 376 B |
| credential entry (`4 + klen 14 + CBOR uint 5`) | 23 B | 23 B |
| credentials per person (mean) | 2.5 | 2.49 |
| **per person, all-in** | 420 B | **433 B** |
| × 10 000 persons | 4.01 MiB | **4 336 158 B = 4.13 MiB** |
| of the 8 MiB MX25R6435F | 50.1 % | **51.6 %** |

R-E is met by the realistic record — the outcome we wanted. (The permission
vocabulary was lengthened once, from ~11-character names to qualified ones
averaging ~14, after the first implementation landed at 45.8 %. Qualified
permission identifiers are what a real facility uses; this was a correction to
the model, not padding to hit a number.)

### 6.1 Sizing to fit (R-J)

C2's real limit is not the 2.09 MB per map, it is the **4 KB per bucket**: a
bucket overflows while the store is nearly empty, and nothing warns first
(K2, K10). The app therefore picks a map count that keeps every bucket far from
that edge, and does not attempt to run near it.

**The first attempt at this was wrong, and how it was wrong is the finding.**
An analytic compound-Poisson model — a Poisson number of entries per bucket,
each of variable size — put eight person maps at 5.5 σ of headroom and ~0.03
expected overflows. The implementation hit `-ENOSPC` at person 9 232.

The model was not badly built; it was fed a mean entry size that later grew by
14 %, and a right-skewed compound-Poisson tail is heavier than the Gaussian
intuition behind "5.5 σ". Both are ordinary mistakes, and neither is detectable
at run time, because there is no per-bucket occupancy query (K10) and the
bucket count cannot change after create (K3).

So the number is now obtained by **enumerating the actual population through
the actual hash** — `tools/sizing.py`, which replicates `fnv1a`, the key
format and the CBOR sizing rules:

| person maps | mean bucket | **fullest bucket** | over 4 KB |
|---|--:|--:|--:|
| 8 | 1 009 B | **4 158 B** | **1** |
| 12 | 764 B | 3 286 B | 0 |
| **16** | **660 B** | **2 711 B** | **0** |
| 24 | 553 B | 2 409 B | 0 |
| 32 | 499 B | 2 369 B | 0 |

**Sixteen person maps**, fullest bucket at 66 % of the ceiling. Beyond about
sixteen the maximum stops falling — it is set by the Poisson tail over a growing
number of buckets rather than by the mean — while every extra map adds
directory-rewrite traffic (K5), so more is not freely better. All 24 932
credential entries fit **one** map: 23 B each, fullest bucket 1 587 B.

Two things worth stating plainly:

- The asymmetry is the finding: the same 511-bucket map holds 25 000 small
  entries comfortably but only a few thousand large ones (**K2**).
- **Sixteen maps where four would hold the bytes is what the API's lack of
  introspection costs.** The margin must be chosen up front and blind, and the
  penalty for getting it wrong is an `-ENOSPC` partway through a multi-hour
  fill with no repair short of a reformat. That over-provisioning is the
  measurement, not a workaround — and this application can size by enumeration
  only because its dataset is a pure function of an index (F6). One whose data
  arrives from outside could not.

### 6.3 The dataset is ballast; the result is time per operation

**What this application measures is time per operation and operations per
second.** The data volume is ballast: it exists to put the store into a state
where those numbers are representative — deep buckets, a realistic hash spread,
enough write history that compaction is in the picture — and is not itself a
result.

That is measurable, and measured (`RESULTS.md` §3b). Holding the build fixed
and varying only the dataset, an access decision costs 32 µs on a 2.5 %-full
store and **44 µs at 51.6 % — 38 % more**, with throughput down 30 %. A
benchmark run against a nearly-empty store would publish a number no deployed
product ever sees. That is what the ballast buys, and why it is worth the
minutes it takes to lay down.

One caveat the same measurement produced: the flash-operation count is *not*
monotonic in fill. It peaks at 5 000 persons and falls again at 10 000, because
per-operation cost tracks how much superseded data sits in the sectors being
walked — a function of write history and of when compaction last ran, not of
live fill. **"Half full" is a convenient, reproducible label for the ballast,
not the variable that drives the cost.**

### 6.4 The fill percentage is a result, not a target

Worth stating plainly, because the earlier drafts of this document had it
backwards.

**"50 % of the external flash" was a sizing heuristic, used once.** It answered
"how big should the dataset be?" with three constraints in mind: large enough
that the storage layer's real costs show up; small enough to leave the board
usable for other things; and small enough that a benchmark run does not demand
the whole partition and a reformat before every run. Half the medium satisfies
all three. It was never a level the app must maintain.

**The person count is therefore frozen.** `tools/sizing.py` chose 10 000 once,
against the record shape and the hash; from that point it is a constant of the
benchmark. Reruns use it unchanged, which is the only way two runs are
comparable at all. It is *not* recomputed from the geometry at build or boot —
nothing in the app reads the partition size except the reporting path (F7a).

**A falling fill percentage is a good result.** If a component shrinks its
overhead and the same 10 000 persons come to occupy 40 % instead of 51.6 %,
that is the implementation improving, and the benchmark has just measured it.
Holding the percentage constant would mean silently growing the dataset to
absorb the gain — hiding exactly the improvement the app exists to detect.

So the percentage is tracked over time as a **regression indicator**
(`RESULTS.md` §3a), in the direction where down is better. The two numbers move
independently and mean different things:

| | what it is | changes when |
|---|---|---|
| person count | **fixed input** — 10 000 | someone deliberately re-sizes the benchmark |
| fill percentage | **output** | the stack's per-record overhead changes |

### 6.2 Cost of the fill

35 000 map writes at ~55.9 ms ≈ 33 min, plus ~37 MB of appended bytes driving
~1 250 compactions at ~5.7 s ≈ 2 h. **One-time population on the DK is ≈ 2.2 h** — the post-merge floor measured
in `RESULTS.md` §5 (13.5 min at a tenth of the scale); this section's estimate
of 2.5 h predates that measurement.
Not designed around; it is findings **B1/B2/K4/K5**, and it is why F8 exists.

---

# Design proposal

## 7. Structure

```
app_cbor_persondb/
├── DESIGN.md  FINDINGS.md  README.md  RESULTS.md
├── CMakeLists.txt  Kconfig  prj.conf  sample.yaml  VERSION
├── boards/   native_sim.{conf,overlay}
│             nrf5340dk_nrf5340_cpuapp.{conf,overlay}
├── tools/    sizing.py — offline capacity check (§6.1). Not built.
│
├── ui/           main.c, ui_bench.c, ui_shell.c
│                 Frontends. Parse and print; include only scenario.h.
├── scenario/     scenario.{c,h}
│                 Operations on a population. Returns structs, never prints.
├── persondb/     persondb.{c,h}     the person management API (§8)
│                 person_cbor.{c,h}  the wire format — private to this layer
│                 The only directory that names blob_db, rootreg or map_ops.
└── dataset/      dataset.{c,h}
                  The synthetic population, a pure function of an index.
```

**One directory per layer, one `CMakeLists.txt` each.** Each contributes its
own sources and publishes its own header directory, so the build file has the
same shape as the design and a new edge between layers is a visible edit rather
than an `#include` someone slipped in. The resulting include graph is exactly
the diagram below — verified by inspection, not asserted:

| directory | includes |
|---|---|
| `ui/` | `scenario.h` |
| `scenario/` | `persondb.h`, `dataset.h` |
| `persondb/` | `person_cbor.h` |
| `dataset/` | `persondb.h` (the record type only) |


Four layers inside the application, one dependency direction, mirroring P6:

```
  ui/         ui_bench.c   ui_shell.c     parse and print only        (F15)
                    └──────┬──────┘
  scenario/       scenario.h               fill, verify, mutate, measure
                       │      └──────────────► dataset/  (pure generator, F6)
  persondb/       persondb.h                persons, cards, permissions,
                       │                     decisions          (F12 / R-H)
                       ├──► person_cbor.h    the wire format (private)
                       │
              kvhash map_ops ─► rootreg ─► blob_db        the stack
```

Each boundary is narrow enough to be replaceable: swapping CBOR for another
encoding touches `person_cbor` only; swapping the storage layout touches
`persondb.c` only; adding a third frontend touches neither.

## 8. The person management API (R-H)

`persondb/persondb.h`. Domain operations only — no storage vocabulary escapes it.

```c
struct persondb_person {                 /* fixed capacity, no heap (P2/P3) */
        uint32_t id;
        char     name [PERSONDB_NAME_MAX  + 1];
        char     dept [PERSONDB_DEPT_MAX  + 1];
        char     title[PERSONDB_TITLE_MAX + 1];
        uint32_t valid_from, valid_until;          /* epoch seconds */
        uint8_t  pin_hash[16];
        uint8_t  n_perms;
        char     perm[PERSONDB_PERMS_MAX][PERSONDB_PERM_MAX + 1];
        uint8_t  n_cards;
        char     card[PERSONDB_CARDS_MAX][PERSONDB_CARD_LEN + 1];
};

enum persondb_decision {
        PERSONDB_GRANTED, PERSONDB_DENIED,
        PERSONDB_UNKNOWN_CARD, PERSONDB_EXPIRED,
};

/* lifecycle */
int persondb_open (struct persondb *db);  /* attach, or build a virgin store */
int persondb_close(struct persondb *db);

/* enrollment */
int persondb_person_put   (struct persondb *db, const struct persondb_person *p);
int persondb_person_get   (struct persondb *db, uint32_t id,
                           struct persondb_person *out);
int persondb_person_delete(struct persondb *db, uint32_t id);

/* credentials — F5's orderings live here, not in any caller */
int persondb_card_assign(struct persondb *db, uint32_t person_id, const char *card);
int persondb_card_revoke(struct persondb *db, const char *card);
int persondb_card_owner (struct persondb *db, const char *card, uint32_t *person_id);

/* permissions */
int persondb_permission_grant (struct persondb *db, uint32_t id, const char *perm);
int persondb_permission_revoke(struct persondb *db, uint32_t id, const char *perm);

/* the decision — R-D */
int persondb_check(struct persondb *db, const char *card, const char *perm,
                   uint32_t now, enum persondb_decision *out,
                   struct persondb_person *who /* optional */);

/* app-owned persistent state, so no caller needs a store of its own */
int persondb_progress_get(struct persondb *db, uint32_t *populated, uint32_t *rev);
int persondb_progress_set(struct persondb *db, uint32_t  populated, uint32_t  rev);

/* introspection for the probe (F10/F11) */
int persondb_stat(struct persondb *db, struct persondb_stat *st);
```

`persondb_check` is implemented the obvious way — resolve the card, load the
person through `persondb_person_get`, check the validity window, compare the
permission string. It does **not** shortcut the decode, and the credential index
stores nothing but a person id. What that costs is measured in §11; the shortcut
is recorded as an unimplemented mitigation.

## 9. The scenario layer (R-I)

`scenario/scenario.h`. Everything a caller does *with a population*, so no frontend
reimplements it and both measure the same thing.

```c
struct scenario;                              /* owns a struct persondb */

int scenario_open (struct scenario *s);
int scenario_close(struct scenario *s);

/* population, resumable (F8). Fills at most `budget` persons, returns how
 * many it actually wrote and whether the dataset is now complete. */
int scenario_fill(struct scenario *s, uint32_t budget,
                  struct scenario_fill_report *out);

/* verification (F9): re-derive `samples` persons from the generator and
 * compare, resolving each card back through the index. */
int scenario_verify(struct scenario *s, uint32_t samples,
                    struct scenario_verify_report *out);

/* one mutation round (F9): revoke the previous revision's temporary cards,
 * grant the next revision's, commit the revision. Idempotent on replay. */
int scenario_mutate(struct scenario *s, struct scenario_mutate_report *out);

/* measurement (§11). Each fills a struct bench_result: ops, ms, us/op,
 * bytes moved on flash, amplification. */
int scenario_bench(struct scenario *s, enum scenario_bench which,
                   uint32_t samples, struct bench_result *out);

/* one-shot store report (F10) */
int scenario_report(struct scenario *s, struct scenario_report *out);

/* single-shot domain actions the shell needs, by dataset index rather than
 * by person id, so a human can drive the generator */
int scenario_person_show(struct scenario *s, uint32_t index,
                         struct persondb_person *out);
int scenario_card_of    (struct scenario *s, uint32_t index, uint8_t slot,
                         char *card_out, size_t sz);
```

Reports are plain structs; the layer never prints. That is what lets the shell
render a table and the benchmark render a `bench` line from the same call, and
what would let a future frontend emit JSON without touching any logic.

## 10. Frontends

One application, one Kconfig `choice`, two `sample.yaml` scenarios.

```
choice APP_CBOR_PERSONDB_FRONTEND
        default APP_CBOR_PERSONDB_FRONTEND_BENCH
config APP_CBOR_PERSONDB_FRONTEND_BENCH  bool "Automatic benchmark run"
config APP_CBOR_PERSONDB_FRONTEND_SHELL  bool "Interactive shell"  select SHELL
endchoice
```

**`ui_bench.c` — automatic.** The unattended run, and the source of
`RESULTS.md`: open → prepare → fill (resuming, in batches) → verify → mutate →
re-verify → bench suite → report → exit. On `native_sim` it exits with a status
reflecting the verdict, so it works as a twister scenario.

**`ui_shell.c` — interactive.** The same operations under `persondb <cmd>`, for
poking a live store on the DK:

| Command | Calls |
|---|---|
| `stat` | `scenario_report` |
| `fill <n>` | `scenario_fill` |
| `verify [n]` | `scenario_verify` |
| `mutate` | `scenario_mutate` |
| `bench <which> [n]` | `scenario_bench` |
| `show <index>` | `scenario_person_show` |
| `check <card> <perm>` | `persondb_check`, timed |
| `card <card>` | `persondb_card_owner` |
| `grant`/`revoke <id> <perm>` | `persondb_permission_*` |
| `assign`/`unassign` | `persondb_card_*` |

Being able to `fill 500`, pull the power, reboot and `stat` is the most direct
demonstration of F8 and F5 that this app can offer — and it is only possible
because §9 exists.

**`sample.yaml`.**

| Scenario | Config | Runs? |
|---|---|---|
| `app.cbor_persondb.bench` | `FRONTEND_BENCH`, 10 000 persons | build only — a full fill is ~2.2 h on hardware |
| `app.cbor_persondb.shell` | `FRONTEND_SHELL` | build only |
| `app.cbor_persondb.smoke` | `FRONTEND_BENCH`, `N_PERSONS=200` | **runs on `native_sim`**, console harness on `VERIFY PASS` |

The smoke scenario is the point of making the scale a Kconfig knob: CI executes
the whole pipeline — fill, verify, mutate, re-verify — in seconds, so the code
paths are regression-tested even though the headline configuration is not.

## 11. Measurements

| Bench | Cost | Why |
|---|---|---|
| `check` | card → person id → record → permission compare (**2 map gets**) | R-D, the headline |
| `byid` | person id → record (1 map get) | the index's share of `check` |
| `miss` | unknown card (1 map get, `-ENOENT`) | negative-lookup cost |
| `put` | rewrite one record and its index entries (read-old + 1 set + index) | mutation cost |
| `cbor` | encode + decode in RAM, no flash | isolates the codec |

The fill is timed as a whole-run phase rather than a benchmark kind (R-E), and
`scenario_bench_kind` is exactly `check`, `byid`, `miss`, `put`, `cbor`.

Every phase also reports **amplification**: bytes moved on flash ÷ bytes the
application asked for. For `check` that is 13.4 KB moved to answer a
question about ~365 B — the number that makes finding **B1** concrete.

`cbor` answers "is CBOR the bottleneck?" in advance. Expected answer: no, by
three orders of magnitude — which retires the question and shows R-B costs
nothing.

## 12. Storage layout — and why L2 rather than L3

**The layout.** One registry key, one app-owned superblock, seventeen maps:

```
rootreg[ ROOTREG_KEY('PADB', 1) ] ──► superblock blob   (CBOR, app-owned)
                                        1 version
                                        2 [ root_id ]×16  people maps
                                        3 root_id         credential index
                                        4 n_persons, populated, rev
                                            │
        kvhash_map_ops on each root ────────┘
```

Everything is reachable from the integer 1 (P5): `rootreg_get` → superblock id
→ `blob_db_get` → seventeen map roots. Boot costs **two** sector reads. Person
key `"p%08X"`, credential key the 14-hex-char UID verbatim, shard
`fnv1a32(id) % 16` — a hash of the id rather than the id itself, so persondb
stays independent of how the caller numbers people (§6.1).

**Why not L3, as decided.** `kvdb` was the obvious starting point and did not
fit:

| | L3 — seventeen `kvdb` instances | L2 — superblock + seventeen roots |
|---|---|---|
| Boot | 17 × (registry read + meta-blob read) = **34 sector reads** | registry + superblock = **2** |
| Persistent overhead | 17 registry entries + 17 × 32 B meta blobs | 1 entry + 1 superblock |
| Registry capacity | 17 of `ROOTREG_MAX_ROOTS`, default **8** (C10) — does not fit, twice over | 1 |
| Progress commit | `kvdb_set` = 2 reads + 1 write | `blob_db_update` = 1 read + 1 write |
| Per-operation cost | identical — `kvdb_get` is `strlen` + `ops->get` | identical |
| Fan-out, naming, capacity accounting | the app's problem | the app's problem |
| What it buys | named instances; backend recorded per instance | direct control of the root graph |

### 12.1 The L3 re-check — two of those five reasons have expired

Re-run against `main` at `1821328`, because two of the numbers above were
consequences of a Kconfig cap that has since been lifted, and a decision that
rests on an expired number should not stand on it.

| row | verdict | evidence today |
|---|---|---|
| Boot — 34 **sector** reads | **expired.** 34 *blob* reads, but B1's slot walk means a read is no longer a 64 KB sector. At §5's fitted cost — 28 flash transactions per `blob_db_get`, 65 µs each, derived from `check`'s 112 over four calls and therefore an upper bound for a 280 B registry image — the L3 route costs **≲62 ms once at boot** against L2's ≲3.6 ms. A 58 ms one-time difference, next to an `open` phase measured in tens of seconds. | `RESULTS.md` §5, B1 |
| Persistent overhead | **stands, and never mattered.** 17 entries + 17 × 32 B is 816 B on a 4 MiB dataset. | — |
| Registry capacity | **expired.** `ROOTREG_MAX_ROOTS` is `range 1 1000`, and rootreg's `BUILD_ASSERT` allows 255 at this payload size (`8 + 16 × 17 = 280 B`). Seventeen instances is one `prj.conf` line, not a wall. It is a **default**, which is finding **R1** — a sharp edge, not a limit. | `lib/rootreg/rootreg.c:45-48`, `lib/rootreg/Kconfig` |
| Progress commit | **stands.** `kvdb_set` is still `dir_load` + bucket read + bucket write (K11); `blob_db_update` on an app-owned superblock is still one read and one write. Progress commits are per batch, so this is small. | `kvhash.c:256-330`, `kvdb.c:189-195` |
| Per-operation cost identical | **stands.** `kvdb_get` is `strlen` + `ops->get`, no extra flash op. | `kvdb.c:197-203` |

**What actually moved was C2.** `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` was `range 1
4096` when D10 was taken and is now `range 1 65535`, bounded at mount by the
real geometry to `(sector − 16) / 2 − 14` = **32 746 B on the DK's 64 KB
sectors**. A map is no longer limited to 511 buckets of 4 KB, so the premise
under the whole table — that this dataset needs *seventeen* instances — is the
thing to re-test. If 10 000 persons fit **one** map, they fit one `kvdb`, and
L3's one-instance-per-name model is exactly the right shape.

**They do fit one map, and the fit is the cost.** `tools/sizing.py` enumerates
it (§6.1's method, not a model). A map get reads the directory *and* a bucket
(K11), so the bytes it moves are the sum of the two:

| payload | maps | buckets | directory | mean bucket | fullest | **bytes per map get** |
|--:|--:|--:|--:|--:|--:|--:|
| 4 096 | **16** (as built) | 511 | 4 096 B | 660 B | 2 711 B (66 %) | **4 756 B — ×1.0** |
| 16 384 | 1 | 511 | 4 096 B | 7 362 B | 14 735 B (90 %) | 11 458 B — ×2.4 |
| 16 384 | 1 | 2 047 | 16 384 B | 1 844 B | 4 907 B (30 %) | 18 228 B — ×3.8 |
| 32 746 | 1 | 4 092 | 32 744 B | 1 004 B | 3 255 B (10 %) | 33 748 B — ×7.1 |
| 32 746 | 2 | 4 092 | 32 744 B | 635 B | 2 328 B (7 %) | 33 379 B — ×7.0 |

Every one-instance layout costs between **2.4× and 7.1× the bytes an access
decision moves**, and by §5's cost model — 65 µs per transaction plus 0.63 µs/B,
of which the byte term is already 6.3 ms of `check`'s 13.66 ms — that is R-D
getting 1.5× to 2× slower. The reason is structural, not a tuning miss:
`kvhash`'s directory must fit one payload (K1), so bucket **count** and bucket
**size** are the same knob. Collapsing sixteen maps into one has to fatten one
of them, and *both* are on the read path. Sharding is what keeps both small.

**The conclusion changes shape, not direction.** D10 stands, but for the third
reason rather than the first two:

- Boot I/O and registry capacity are no longer arguments. They were artifacts
  of a payload cap and a default, and both are gone.
- **The argument that survives is R-D.** Sixteen small person maps are not
  over-provisioning against C2 that a bigger payload would retire (§6.1) — it
  is what makes a lookup read 4 756 B instead of 18 228 B. `kvdb` cannot
  express seventeen-maps-behind-one-name, so an application that needs it holds
  the roots itself.
- The cheap L3 route — seventeen *named* instances, one per shard — is now
  affordable (58 ms of boot, one `prj.conf` line) and still buys nothing:
  `persondb.c` would keep the same shard table, the same fan-out and the same
  capacity accounting, with `kvdb_t` handles in place of root ids. Per-op cost
  is provably identical. It would, however, turn **V4** from an argument into a
  measurement — which is the one reason to reconsider, and it is a probe
  reason (§1 job 2), not a design one.

This is still R-H's "may use L2", and the practice worth teaching is unchanged
but now better founded: **use the highest layer whose shape matches; drop down
when it doesn't** — and check periodically whether the shape has changed. It is
finding **V4** from the other side: once you shard, `kvdb` stops paying for
itself, and the thing that forces sharding here is R-D, not capacity.

**Creation and the crash window.** `rootreg_get_or_create` may return an
allocated-but-unbound id (its contract). `persondb_open` detects the virgin case
with `blob_db_get(sb_id) == -ENOENT`, allocates seventeen ids, calls
`kvhash_map_ops.create` on each, and binds the superblock **last** — the single
atomic write that publishes the structure. A crash before it leaves the maps
unreferenced, and `blob_db` has no reachability GC: finding **B8**, the same
window `kvdb` itself has (`kvdb.c:118`).

## 13. Acceptance criteria

- **A1** Builds for `native_sim` and `nrf5340dk/nrf5340/cpuapp`; all three
  `sample.yaml` scenarios pass `west twister -T app_cbor_persondb -p native_sim`.
- **A2** The `smoke` scenario runs to `VERIFY PASS` in CI.
- **A3** On `native_sim` with the DK's geometry a full 10 000-person run reaches
  the target and reports `VERIFY PASS`; a second run verifies the first's
  content.
- **A4** `RESULTS.md` carries measured nRF5340-DK numbers.
- **A5** `FINDINGS.md` records every limitation hit, with the evidence and the
  measurement that quantifies it.
- **A6** `README.md` states each practice, points at the code, and names the
  failure it prevents.
- **A7** Nothing outside `persondb/` includes a storage or codec header, calls a
  `blob_db_*` / `rootreg_*` / `map_ops` entry point, encodes or decodes CBOR, or
  contains a key string, shard index or blob id — the check that F12/F15 held.

  The criterion is about *dependencies*, not vocabulary. A comment elsewhere may
  name a lower layer when the point being made is about that layer — the stack
  reserved for `blob_db`'s slot builder (B5/B6) is recorded where it is
  observed. What it may not do is depend on it. The distinction matters because
  this application is also a probe: scrubbing every mention would delete the
  findings, and a criterion satisfied by deleting evidence is the wrong one.
- **A8** No `-ENOSPC` occurs during a full fill. If one does, §6.1's sizing rule
  is wrong and that becomes the finding.

## 14. Decisions

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
| D9 | The person management functionality is an **internal application API** (§8), not proposed for `lib/`. |
| D10 | It is implemented on **L2 `map_ops` + `rootreg` + `blob_db`**, not L3 `kvdb` (§12). `kvdb` is not linked into the image. **Re-checked against `main` at `1821328` (§12.1): the decision stands, two of its three reasons do not.** Boot I/O and registry capacity were artifacts of a payload cap and a Kconfig default and are gone; what holds the app at L2 is R-D — sixteen small maps move 4 756 B per lookup where any single-instance layout moves 11 458–33 748 B, and `kvdb` cannot name sixteen maps. |
| D14 | **The dataset size is frozen; the fill percentage is an observation.** 50 % was the heuristic that picked 10 000 persons once (§6.3), leaving the board room for other uses and avoiding a reformat before each run. Nothing is scaled from geometry at run time, and a fill percentage that *falls* is a better implementation, not a regression. |
| D11 | **Size to fit, do not fight C2** (R-J): sixteen person maps put the fullest bucket at 66 % of the ceiling, a number obtained by enumerating the population rather than by modelling its tail — the model was tried, and was wrong (§6.1). `-ENOSPC` is not an expected path, and the app does not provoke it; the over-provisioning it forces *is* the visible cost. |
| D12 | A **scenario layer** (§9) holds every operation on a population, and never prints. |
| D13 | **Two frontends** — automatic benchmark and interactive shell — selected by Kconfig, shipped as separate `sample.yaml` scenarios, plus a small `smoke` scenario that actually runs in CI. |
