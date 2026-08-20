# `app_cbor_persondb` — CBOR person/credential database

Status: **v0.7 — the sixteen person shards are removed (§12.2); the app is two
`kvhash` instances.** Re-measured on `native_sim`. Two consequences to read
before the numbers:

- **The benchmark's 10 000-person scale no longer completes.** The fill hits
  `-ENOSPC` at person 9 670 on the 8 MiB part, with live content at half the
  partition. **The store's maximum is 9 670 persons** — measured, and the
  reason is `kvhash`'s bucket directory, not the medium (`FINDINGS.md`
  **K13**). `RESULTS.md` §4b reports the 9 000-person run. This is a finding,
  not a defect to repair by sharding again.
- **The nRF5340-DK figures in `RESULTS.md` §5 predate this change** and
  describe the sixteen-shard build. They are kept, and marked, because the
  before/after is the measurement. A4 was already open for a full-scale board
  run; it now also wants a two-instance one.

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
| **C10** | `CONFIG_ROOTREG_MAX_ROOTS` defaults to **8** — ample, and not a constraint on this app. A structure is reachable from one root (P5) and takes **one** entry; `rootreg` is sized for "a few registered roots … a registry, not a database". A layout that wants more entries than it has structures is misusing L1.5 (§12.1). | `lib/rootreg/Kconfig`, `doc/layers/l1_root_registry.md` §1 |

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

R-E is met by the realistic record — the outcome we wanted. (**The store's
measured maximum is 9 670 persons**, 49.9 % of the part, so the 10 000-person
benchmark scale no longer fits: §6.1 and **K13**.) (The permission
vocabulary was lengthened once, from ~11-character names to qualified ones
averaging ~14, after the first implementation landed at 45.8 %. Qualified
permission identifiers are what a real facility uses; this was a correction to
the model, not padding to hit a number.)

### 6.1 Sizing to fit (R-J)

C2's real limit is not the 2.09 MB per map, it is the **per-bucket ceiling**: a
bucket overflows while the store is nearly empty, and nothing warns first
(K2, K10). The app therefore sizes so that every bucket stays far from that
edge, and does not attempt to run near it.

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
format and the CBOR sizing rules.

**The number it picks is no longer a map count.** The application is two
containers (§12), so there is one people map, and the question is what payload
it needs. `MAX_PAYLOAD` sets the bucket ceiling *and*, through
`(MAX_PAYLOAD - 8) / 8`, the bucket count — the app asks for the largest map
available and gets whatever that arithmetic yields:

| payload | buckets | directory | mean bucket | **fullest bucket** | % of ceiling |
|---|--:|--:|--:|--:|--:|
| 4 096 | 511 | 4 096 B | 7 362 B | **14 735 B** | **360 % — bursts** |
| 8 192 | 1 023 | 8 192 B | 3 677 B | 7 734 B | 94 % |
| **16 384** | **2 047** | **16 384 B** | **1 844 B** | **4 907 B** | **30 %** |
| 32 722 | 4 089 | 32 720 B | 1 006 B | 3 533 B | 11 % — **K12** |

**16 384**, fullest bucket at 30 % of the ceiling. Both neighbours are
excluded by measurement rather than preference: 8 192 leaves a bucket at 94 %
of a cliff that gives no warning (K2, K10), and 32 722 — the most the geometry
sustains — makes the directory a 32 720 B blob that fills an erase block on its
own and kills the fill at person 36 (**K12**). All 24 932 credential entries fit
their own map comfortably at any of these: 23 B each.

**This symbol also decides how many persons the store holds, in the opposite
direction.** The directory is a blob like any other, and `blob_db` has to place
each rewritten copy in one erase block; the bigger it is, the earlier the store
runs out of places to put it. So the app sits between two walls that move
apart, and the maximum is whichever is nearer — both measured on `native_sim`:

| payload | buckets | directory | **max persons** | the wall it hits | fullest bucket there |
|---|--:|--:|--:|---|--:|
| 8 192 | 1 023 | 8 192 B | **11 787** | K2 — a bucket bursts | 8 141 B = **99 %** |
| **16 384** | **2 047** | **16 384 B** | **9 670** | **K13** — the directory cannot be placed | 4 621 B = 28 % |

**The shipped configuration is not the one that holds the most records**, and
that is a deliberate choice rather than an oversight. 8 192 holds 22 % more
persons, by running the fullest bucket at 99 % of a ceiling that gives no
warning before it is crossed (K2), cannot be queried (K10), and cannot be
recovered from short of a reformat — for a dataset whose tail this design has
already mis-estimated once (§6.1's opening). 16 384 buys a 28 % bucket and pays
for it in capacity. Both numbers belong in the record; **K13** is the finding
that the payment exists at all.

**What this costs, unhidden.** One map means the directory carries every
bucket, and `kvhash` re-reads all of it on every operation (K11): a person
lookup moves **18 228 B** where the sixteen-shard build moved 4 756 B — 3.8×.
Enumerating offline says ~683 buckets would be the minimum of that curve, at
10 980 B. The application does not pass 683. Hand-tuning a bucket count against
another layer's read amplification is precisely the workaround §1 forbids: it
would bury K11 under a magic number and make the app look faster than the stack
is. The 3.8× is measured and recorded instead (`RESULTS.md` §4b).

**And it is not enough.** At the benchmark's fixed 10 000 persons this layout
stops at **person 9 670** — 4.19 MiB live, 49.9 % of an 8 MiB partition. Not
because the flash is full and not because a bucket burst (the fullest is at
28 % of its ceiling), but because the 16 384 B *directory* can no longer be
placed: once the store is half live, no 65 488 B erase block has 16 398 B
contiguous free, and the map cannot accept a key in a bucket it has not used
yet. That is `kvhash`'s directory, not the storage layer — **K13**. The
sixteen-shard build finished the same dataset at 51.6 % because its largest
blob was 4 096 B. §12.2 is why this is not repaired by sharding again.

Two things worth stating plainly:

- The asymmetry is the finding: the same 511-bucket map holds 25 000 small
  entries comfortably but only a few thousand large ones (**K2**).
- **Choosing this number offline, up front and blind, is what the API's lack
  of introspection costs.** The penalty for getting it wrong is an `-ENOSPC`
  partway through a multi-hour fill with no repair short of a reformat — and
  as K13 shows, the same `-ENOSPC` also means "the directory can no longer be
  placed", which the application cannot distinguish — and misreports. This app can size by enumeration only
  because its dataset is a pure function of an index (F6). One whose data
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

**The layout.** One registry key, one app-owned superblock, **two `kvhash`
instances** — the domain has two collections, so the store has two containers:

```
rootreg[ ROOTREG_KEY('PADB', 0) ] ──► superblock blob   (CBOR, app-owned)
                                        1 version
                                        2 [ root_id ]×1   people map
                                        3 root_id         credential index
                                        4 n_persons, populated, rev
                                            │
        kvhash_map_ops on each root ────────┘
```

Everything is reachable from the integer 1 (P5): `rootreg_get` → superblock id
→ `blob_db_get` → two map roots. Boot costs **two** blob reads.

This is the shape the stack is built for, and a model application is supposed
to be built in it: L2 gives containers, L1½ gives one place to keep a root, and
an application with two collections needs two containers and one entry. The
people map carried sixteen shards until §12.2, for reasons that were real and
are gone.

**One entry, and it is instance 0.** The registry holds a single `'PADB'` key
because this application is a single database, and a database is a single
structure with a single root (P5). The instance field is there to distinguish a
*second*, independent store of the same type (`l1_root_registry.md` §3, whose
own example reserves `('KVDB', 1)` for exactly that) — so the only instance is
instance **0**, matching every other client in the tree (`'KVDB',0`, `'BLFS',0`,
`'BOOT',0`, `'MCNT',0`). This app registered its sole superblock as `('PADB', 1)`
until `main` at `64caa99`, which implied a `('PADB', 0)` that never existed: the
same mistake as §12.1's, one field further down — treating the registry as
something that counts rather than something that names. Changing it moves the
on-flash key, so a store written by an earlier build is not found and `open`
sees a virgin device; that is acceptable here and nowhere else, because this
app's store is rebuilt from `dataset/` on every run and no image of it is
deployed. Person
key `"p%08X"`, credential key the 14-hex-char UID verbatim, shard
`fnv1a32(id) % 16` — a hash of the id rather than the id itself, so persondb
stays independent of how the caller numbers people (§6.1).

**Why not L3.** `kvdb` was the obvious starting point and does not fit. The
table is the *corrected* argument — §12.1 re-runs each row against `main` and
says which of the original ones survived:

| | L3 — seventeen `kvdb` instances | L2 — superblock + seventeen roots |
|---|---|---|
| Boot | 17 × (registry read + meta-blob read) = **34 blob reads, ≲62 ms once** — was "34 sector reads" before B1 (§12.1) | registry + superblock = **2, ≲3.6 ms** |
| Persistent overhead | 17 × 32 B meta blobs | 1 superblock |
| Registry footprint | 17 entries for **one** structure — L1.5 used as an id allocator (§12.1) | 1 entry, which is what a structure costs |
| Progress commit | `kvdb_set` = 2 reads + 1 write | `blob_db_update` = 1 read + 1 write |
| Per-operation cost | identical — `kvdb_get` is `strlen` + `ops->get` | identical |
| Fan-out, naming, capacity accounting | the app's problem | the app's problem |
| What it buys | named instances; backend recorded per instance | direct control of the root graph |

### 12.1 The L3 re-check — one reason expired, one was never a reason, one was a workaround defending itself

Re-run against `main` at `1821328`. A decision should not rest on a number that
has since moved, nor on an argument that was never sound — and this one had one
of each. The table above is the corrected argument; what it used to say, and
why, is below.

| row | verdict | evidence today |
|---|---|---|
| Boot — 34 **sector** reads | **expired.** 34 *blob* reads, but B1's slot walk means a read is no longer a 64 KB sector. At §5's fitted cost — 28 flash transactions per `blob_db_get`, 65 µs each, derived from `check`'s 112 over four calls and therefore an upper bound for a 280 B registry image — the L3 route costs **≲62 ms once at boot** against L2's ≲3.6 ms. A 58 ms one-time difference, next to an `open` phase measured in tens of seconds. | `RESULTS.md` §5, B1 |
| Persistent overhead | **stands, and never mattered.** 17 meta blobs of 32 B is 544 B on a 4 MiB dataset. (The 17 *registry entries* are not overhead — see the withdrawal below.) | — |
| Progress commit | **stands.** `kvdb_set` is still `dir_load` + bucket read + bucket write (K11); `blob_db_update` on an app-owned superblock is still one read and one write. Progress commits are per batch, so this is small. | `kvhash.c:256-330`, `kvdb.c:189-195` |
| Per-operation cost identical | **stands.** `kvdb_get` is `strlen` + `ops->get`, no extra flash op. | `kvdb.c:197-203` |

**The registry-capacity row is withdrawn — it was never a reason.** Earlier
editions of this table argued that seventeen instances "overrun
`ROOTREG_MAX_ROOTS`, default 8, twice over". That is a real symptom pointed at
the wrong module. `rootreg` maps compile-time keys to **structure roots**, and
its own contract sets the scale: *"a few registered roots … it is a registry,
not a database"* (`l1_root_registry.md` §1). **A database is one structure and
takes one entry** — which is what this app's layout does, and what the L3
layout would have to do too if it were shaped correctly.

So the seventeen-entry figure never measured a shortage of registry capacity.
It measured a layout using L1.5 as an id allocator, one entry per shard, for a
structure that has exactly one root. Two consequences, and neither is a cost to
put in this table:

- **Raising `ROOTREG_MAX_ROOTS` would be the wrong fix.** The bound is not the
  problem; a design that needs seventeen entries is. Sizing the registry to
  absorb a misuse is how a registry becomes a database.
- **The number to judge a layout on is entries-per-structure, and it is one.**
  This app registers one. The L3 route registers seventeen for the same single
  structure, and cannot do otherwise, because a `kvdb` instance *is* a registry
  key — which is V4 seen from L1.5.

The row is therefore gone from the table above, replaced by the thing it was
really observing: seventeen entries for one structure. Finding **R1** is
withdrawn on the same grounds — it was raised from this argument, and the
argument does not survive it.

**What actually moved was C2.** `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` was `range 1
4096` when D10 was taken and is now `range 1 65535`, bounded at mount by the
real geometry to `(sector − 16) / 2 − 14` = **32 722 B** on the 64 KB blocks of
both targets, UBI's 48 B per-PEB header included — the raw-flash arithmetic
says 32 746 and `blob_db_mount()` rejects it (B9/B10 working). A map is no longer limited to 511 buckets of 4 KB, so the premise
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
| 32 722 | 1 | 4 089 | 32 720 B | 1 006 B | 3 533 B (11 %) | 33 726 B — ×7.1 · **unbuildable, K12** |

Every one-instance layout costs between **2.4× and 7.1× the bytes an access
decision moves**, and by §5's cost model — 65 µs per transaction plus 0.63 µs/B,
of which the byte term is already 6.3 ms of `check`'s 13.66 ms — that is R-D
getting 1.5× to 2× slower. The reason is structural, not a tuning miss:
`kvhash`'s directory must fit one payload (K1), so bucket **count** and bucket
**size** are the same knob. Collapsing sixteen maps into one has to fatten one
of them, and *both* are on the read path. Sharding is what keeps both small.

**The conclusion changes shape, not direction.** D10 stands:

- Boot I/O is no longer an argument — B1 retired it. Registry capacity never
  was one: a database is one registry entry, and seventeen said something about
  the layout, not about `rootreg`.
- **The surviving argument was R-D — and it did not survive §12.2.** The
  reading above was that sixteen small maps are what keep a lookup at 4 756 B
  instead of 18 228 B, so the app holds its roots itself. Every number in it is
  correct and the conclusion drawn from it was wrong: "our workaround is faster
  than the stack" is an argument for *keeping a workaround*, which §1 forbids
  in the sentence it forbids it in. The 3.8× is a cost of K11 and belongs in
  `FINDINGS.md`, not in a justification. §12.2 removes the shards and records
  what they were covering.
- **What holds the app at L2 now is shape, not bytes.** The application is two
  collections, so it is two containers and one registry entry. L3 would wrap a
  `kvdb` around each container to give it a name the app never uses and a
  backend record it does not vary, and put a registry entry behind each
  instance. Per-op cost is provably identical (`kvdb_get` is `strlen` +
  `ops->get`), so the whole of L3's contribution here is naming — R-H's "may
  use L2", exercised.
- With the shards gone, **V4 no longer describes this application.** It remains
  a finding about `kvdb`; it is simply no longer this app's reason for anything.

This is still R-H's "may use L2", and the practice worth teaching is unchanged
but now better founded: **use the highest layer whose shape matches; drop down
when it doesn't** — and check periodically whether the shape has changed.

### 12.2 The sixteen shards are gone

Re-checked at the same time as §12.1, and this one changed the application
rather than the argument for it.

**Why they existed.** A `kvhash` bucket overflows at
`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN`, which was capped at 4 096 B (C2). Ten
thousand person records at ~376 B do not fit one map's 511 buckets without
bursting one — enumeration puts the fullest at 14 735 B, 3.6× the ceiling — so
the records were spread over sixteen maps. Nothing in the domain has sixteen of
anything. It was a way around a limit of the layer below.

**Why they had to go.** §1's rule is not a style preference:

> Where that design performs badly, the number is measured and recorded as a
> finding — it is *not* engineered around. Working around a weakness hides it,
> and hiding it defeats job 2.

Sharding is the largest workaround in this application, and it was hiding
things. The cap moved — `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` is `range 1 65535`,
bounded at mount by the geometry — so one map now holds the dataset, and the
only reason to keep sixteen was that the app ran better with them. That is the
reason the rule exists to reject.

**What removing it exposed, within an hour of the change:**

| | |
|---|---|
| **K12** | Asking for the largest map the geometry sustains (32 722 B payload) builds a 32 720 B bucket directory — two copies plus headers are 65 484 B of a 65 488 B erase block. The directory owns a block, `blob_db` compacts on nearly every write, and the fill dies at **person 36**. Legal at every layer; checked by none. |
| **K13** | At the shipped 16 384 B payload the store tops out at **9 670 persons**, 49.9 % of the partition. The wall is `kvhash`'s 16 384 B bucket directory: `blob_db` cannot find 16 398 B contiguous in any erase block once the store is half live, so a first insert into a fresh bucket fails while 1 KB blobs still fit. K1, K5, K11 and this are one decision — storing bucket ids that could have been computed. |
| **K11, measured** | A person lookup moves **18 228 B** against the sixteen-shard build's 4 756 B — 3.8×, all of it the whole-directory read. |
| **B5 job 3** | `append_slot` builds every slot in a `MAX_PAYLOAD + 46` byte *stack* frame. Four times the payload is four times the frame: 4 200 B → 16 430 B, and the app's stack went from 12 KB to 28 KB. |

None of these were visible while the app sharded. K12 and K13 are new findings;
K11 and B5 were `read` findings that are now `measured` and `hit`. The
application got slower and now fails its own headline scale — and that is the
correct outcome, because the stack was always going to do this to a product
built the intended way, and a probe that only reports what a workaround lets
through is not probing.

**What did not change.** The registry key, the superblock, the commit
discipline, the API, the CBOR encoding, the verification. Only the number of
person maps, the payload, and the stack.

It is finding **V4** from the other side too: the shard fan-out that made
`kvdb` stop paying for itself is gone, so V4 no longer describes this
application — but the reason to stay on L2 does not depend on it (§12.1).

**Creation and the crash window.** `rootreg_get_or_create` may return an
allocated-but-unbound id (its contract). `persondb_open` detects the virgin case
with `blob_db_get(sb_id) == -ENOENT`, allocates two ids, calls
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
| D10 | It is implemented on **L2 `map_ops` + `rootreg` + `blob_db`**, not L3 `kvdb` (§12). `kvdb` is not linked into the image. **Re-checked against `main` at `1821328` (§12.1): the decision stands, none of its original reasons do.** Boot I/O expired with B1. Registry capacity is withdrawn entirely — a database takes one registry entry, so seventeen was a fact about the proposed layout, not a limit of `rootreg`. R-D (bytes per lookup) turned out to be a workaround defending itself and was retired with the shards in §12.2. What holds the app at L2 is **shape**: two collections are two containers behind one root, and L3's contribution to that would be a name the app never uses. |
| D14 | **The dataset size is frozen; the fill percentage is an observation.** 50 % was the heuristic that picked 10 000 persons once (§6.3), leaving the board room for other uses and avoiding a reformat before each run. Nothing is scaled from geometry at run time, and a fill percentage that *falls* is a better implementation, not a regression. |
| D11 | **Size to fit, do not fight C2** (R-J): a 16 384 B payload puts the fullest bucket at 30 % of the ceiling, a number obtained by enumerating the population rather than by modelling its tail — the model was tried, and was wrong (§6.1). Sizing now picks a payload rather than a map count, because the app is two containers (§12.2). `-ENOSPC` is not an expected path and the app does not provoke it — but it now arrives anyway, from the medium rather than a bucket, and the app cannot tell which (**K13**). |
| D12 | **The sixteen person shards are removed** (§12.2). They were a workaround for a 4 KB payload cap that has since lifted, and §1 forbids keeping one because the app runs better with it. The app is two `kvhash` instances. This cost real performance (K11, 3.8× per lookup) and cost the app its headline scale (**K13**: the store tops out at 9 670 persons), and exposed **K12** and **K13**, neither of which was reachable while the shards were there. A probe that only reports what its workarounds let through is not probing. |
| D12 | A **scenario layer** (§9) holds every operation on a population, and never prints. |
| D13 | **Two frontends** — automatic benchmark and interactive shell — selected by Kconfig, shipped as separate `sample.yaml` scenarios, plus a small `smoke` scenario that actually runs in CI. |
