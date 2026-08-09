# `app_cbor_persondb` — CBOR person/credential database

Status: **v0.1 — requirements only, for discussion.** The design proposal is
deliberately *not* in this document yet; §8 lists the questions that must be
settled before it is written.

The design document for this test application, kept beside the code it
describes. Governed by `doc/principles.md` · consumes the stack in
`doc/architecture.md`.

| | |
|---|---|
| Application | `app_cbor_persondb/` |
| This document | requirements (now) → design proposal → as-built notes |
| Also here | `README.md` (the good-practices guide, R-F) and `RESULTS.md` (measured numbers), following the `app_perf*/` convention |

---

## 1. Purpose

A sample application that shows how to build a **real, non-trivial domain
database** on this storage stack, and that doubles as a **worked set of good
practices** for anyone putting the stack into a product.

The domain is deliberately concrete — physical access control:

> A population of *persons*. Each person holds a set of *permissions* and is
> assigned one or more *credentials* (card IDs). A reader presents a card ID
> and must decide, quickly, whether that credential grants a given permission.

This is a good vehicle because it exercises the three things a real embedded
database has to get right at once: a **serialization format** with optional and
repeated fields, a **secondary index** for the query that actually matters, and
**scale** — enough data that the storage layer's real costs show up.

## 2. Scope

In scope:

- one new test application in `app_cbor_persondb/`, built for `native_sim` and
  `nrf5340dk/nrf5340/cpuapp` and picked up by `west twister -T app_cbor_persondb`
  like the existing `app_perf*` apps;
- a written good-practices guide derived from the app's own code;
- reference performance numbers at the specified fill level.

Out of scope (explicit non-goals):

- **No changes to `lib/` or `include/app/lib/`.** The app is a *client* of the
  published API. If a limitation is hit, it is documented as a finding, not
  patched around by editing the library. (This is the point of a sample: it
  proves the shipped API is sufficient — see `l1_model_container.md` for the
  same discipline one layer down.)
- No new container or L3 interface.
- No BLE/NFC/reader hardware integration; credentials arrive as strings from a
  synthetic dataset.
- Not a security product. Access decisions are demonstrated, not hardened
  (no key material, no anti-replay, no secure element).

## 3. Stakeholder requirements

Verbatim from the request, split into testable items.

| Id | Requirement |
|---|---|
| **R-A** | Use the API already published by this repository. |
| **R-B** | Use **CBOR** as the serialized object format. |
| **R-C1** | The stored object is a **person**. |
| **R-C2** | A person carries **permissions, as an array of strings**. |
| **R-C3** | A person has **credential card IDs (strings)** assigned to them. |
| **R-D** | Given a credential, it must be possible to get the person and check a permission **very quickly**. |
| **R-E** | Demonstrate performance with the database sized at **~50 % of the external flash available on the DK board**. |
| **R-F** | The application is delivered as a **set of good practices** for using the system. |

## 4. Constraints the existing stack imposes

These are measured or read out of the current tree, not assumed. They are what
makes R-D and R-E non-trivial, so they belong in the requirements.

| Id | Constraint | Source |
|---|---|---|
| **C1** | `kvdb` is the only implemented L3 interface, and `kvhash` its only implemented backend. No ordered iteration, no `foreach`. | `lib/kvdb/kvdb.c`, `doc/layers/l3_interfaces.md` §3 |
| **C2** | A `kvhash` instance holds at most `(MAX_PAYLOAD−8)/8` buckets of `MAX_PAYLOAD` bytes each. `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` is capped at 4096 → **511 buckets × 4 KB ≈ 2.09 MB per named instance**. | `lib/containers/kvhash/kvhash.c:47`, `lib/blob_db/Kconfig` |
| **C3** | One `kvhash` entry costs `4 + klen + vlen` and must fit a single payload, so a value is bounded by ~4 KB. | `kvhash.c:291` |
| **C4** | Every `blob_db` operation reads a **whole sector**. On the DK's MX25R6435F that is 64 KB per read, so **per-op cost is essentially independent of payload size**. Measured: 16.9 ms per `blob_db` read; `kvdb_get` ≈ **34.9 ms**, `kvdb_set` ≈ **55.9 ms**. | `blob_db.c` `read_bucket()`; `app_perf/RESULTS.md`, `app_perf_kvdb/RESULTS.md` |
| **C5** | A `blob_db` bucket is one sector; a blob lands in `id % n_buckets`. 8 MB / 64 KB = 128 sectors − 3 reserved = **125 buckets**. | `blob_db.c`, `blob_db_internal.h` |
| **C6** | Compacting one bucket costs **five 64 KB erases** (master, scratch, bucket, scratch, master). One erase measured at ~1.1 s. | `compact_commit()`; `app_perf_kvdb/RESULTS.md` |
| **C7** | Single-threaded: the caller serializes every call across every open instance. | `blob_db.h` §concurrency |
| **C8** | Each `kvdb_set`/`kvdb_delete` is individually atomic; there is **no multi-key transaction**. | `blob_db.h` §atomicity |

Two consequences deserve to be stated as requirements rather than left implicit,
because they drive the whole design:

- **C2 ⇒ one `kvdb` instance cannot hold a 4 MB dataset.** Reaching R-E
  requires spreading the data over several named instances.
- **C8 ⇒ a person record and its index entries cannot be updated atomically
  together.** The application must define an ordering that fails safe.

## 5. Derived functional requirements

| Id | Requirement | Rationale |
|---|---|---|
| **F1** | Person records are stored as CBOR under `kvdb`, keyed by person id. | R-A, R-B, R-C1 |
| **F2** | The CBOR schema carries: person id, display fields, permissions as an array of text strings, and card IDs as an array of text strings. | R-C2, R-C3 |
| **F3** | A **credential index** maps card ID → the data needed to answer an access question, so that a permission check on a presented card costs **one** `kvdb_get`, not two. | R-D, C4 |
| **F4** | The full person record remains the authoritative copy; anything denormalized into the index is derived from it and rebuildable. | R-D vs. correctness |
| **F5** | Index and record updates are ordered so that a power cut leaves a **fail-safe (deny)** state, never a fail-open one. | C8, P7 |
| **F6** | The dataset is generated by a pure function of the record index, so any record can be re-derived and verified without shadow state. | C1 (no iteration), P3 |
| **F7** | The store is sized at boot from the **real partition geometry** and a configurable fill target, defaulting to 50 %. | R-E, P4 |
| **F8** | Population is **batched and resumable across reboots**, with committed progress. | R-E (see §6), P7 |
| **F9** | Every run after the fill completes verifies a deterministic sample of the store against the generator, mutates a bounded subset, and re-verifies. | P8 |
| **F10** | The run reports measured timings per phase, plus the achieved fill in bytes and per cent. | R-E |

## 6. The R-E cost problem — needs a decision

R-E asks for ~50 % of 8 MB = **~4 MB of live data**. Working that through C2–C6
with a plausible record:

```
person record   ≈ 227 B CBOR   (id, name, dept, 12 permission strings, 2 cards)
person entry    = 4 + 9 + 227  = 240 B
credential entry= 4 + 14 + 11  =  29 B   × 2 cards
per person      ≈ 298 B
```

| Quantity | Value |
|---|---|
| Persons to reach 4 MB | ≈ 14 000 |
| Credentials | ≈ 28 000 |
| `kvdb_set` calls to populate | ≈ 42 000 |
| Population, ops only (42 000 × 55.9 ms) | **≈ 39 min** |
| Bytes appended to flash during the fill (bucket rewrite amplification) | ≈ 34 MB |
| Resulting bucket compactions (≈ 1 100 × 5.7 s) | **≈ 1.8 h** |
| **Total one-time fill on the DK** | **≈ 2.4 h** |

Steady state afterwards is fine — a verify/mutate/re-verify run is ~1 min, and
the R-D hot path is one 34.9 ms lookup. The problem is purely the *one-time
fill*, and it is a genuine property of the v1 bucket-log at high occupancy, not
a bug in the app.

Levers, in decreasing order of effect:

1. **Fewer, larger records.** Cost is per *operation*, not per byte (C4). A
   person record of ~800 B instead of ~227 B reaches the same 4 MB with ~5 000
   persons and ~15 000 sets — roughly a **3× reduction** in both op count and
   appended bytes. It needs a bigger record to be *plausible* (an access-control
   record with a biometric template is realistic; padding for its own sake is
   not).
2. **One credential per person instead of two.** Removes ~14 000 sets and most
   of the index's write amplification, at the cost of not demonstrating the
   many-cards-per-person case.
3. **Default the fill target below 50 %,** and treat 50 % as a documented
   opt-in long run.
4. **Make the fill unattended-friendly** (F8) and simply accept the hours.

My recommendation is **1 + 4**: keep 50 % as the default because that is what
was asked for, make the record realistically large so the fill is ~45 min rather
than ~2.4 h, and make it resumable so an interrupted run costs nothing.

## 7. Acceptance criteria

- **A1** Builds for `native_sim` and `nrf5340dk/nrf5340/cpuapp`; passes
  `west twister -T <app> -p native_sim`.
- **A2** On `native_sim` configured with the DK's geometry (8 MB partition,
  64 KB sectors), a full run reaches the configured fill target and reports
  `VERIFY PASS`.
- **A3** A second run of the same binary verifies the content written by the
  first — persistence across "reboot" is proven, not asserted.
- **A4** The reported hot-path measurement (R-D) is shown next to the naive
  two-lookup path, so the index's value is quantified rather than claimed.
- **A5** `RESULTS.md` records measured `native_sim` numbers and, for the DK,
  either measured hardware numbers or clearly-labelled projections derived from
  the constants in C4/C6.
- **A6** The good-practices guide (R-F) states each practice, points at the
  lines of the app that implement it, and gives the failure it prevents.

## 8. Open questions

1. **Record size / fill time (§6).** Accept the ~2.4 h fill, or take lever 1
   (larger, biometric-template-carrying person records) to bring it to ~45 min?
   My recommendation is lever 1 + resumable fill.
2. **CBOR codec.** Use **zcbor** (Zephyr's own CBOR library — add `zcbor` to the
   `name-allowlist` in `west.yml`), or hand-roll a minimal CBOR subset to keep
   the manifest untouched? I recommend zcbor: hand-rolling a serializer in a
   document about good practices would undercut the message.
3. **"External flash available" = 8 MB?** Both existing DK overlays give the
   whole MX25R6435F to `storage_partition`, so 50 % is 4 MB. Confirm.
4. **Cards per person:** 2 (demonstrates the multi-credential case, doubles the
   index cost) or 1?
5. **Hardware numbers.** I cannot run the DK from here. Will you run the app on
   the board (as with `app_perf_kvdb`'s S/N 960115021 capture) so `RESULTS.md`
   carries measurements, or should it ship with projections labelled as such?

**Settled.** Naming and placement: the app is `app_cbor_persondb/`, and all of
its documentation lives inside it — this design document, the `README.md`
good-practices guide (R-F), and `RESULTS.md`. Nothing is added under `doc/`.
