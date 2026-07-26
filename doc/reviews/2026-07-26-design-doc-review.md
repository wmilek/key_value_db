# Design-document review — 2026-07-26

Scope: all nine documents under `doc/` (`architecture.md`, `principles.md`,
`layers/l0_flash.md`, `layers/l1_blob_db.md`, `layers/l1_model_container.md`,
`layers/l1_root_registry.md`, `layers/l2_containers.md`,
`layers/l3_interfaces.md`, `impl/l1_bucketlog.md`), cross-checked against the
code on `main` (post UBI-integration merge, PR #5).

---

## 1. Overall verdict

The documentation set is unusually strong for a project at this stage. The
things that usually go missing are present and load-bearing:

- **Contracts separated from implementations**, with the dependency rule
  stated (`doc/layers/` normative, `doc/impl/` non-normative) and enforced in
  prose ("upper layers must not depend on anything in this document").
- **The model container** is a genuinely good idea executed well: a
  sufficiency proof, a reference pattern, and an acceptance-test blueprint in
  one document, with per-crash-point residue tables for every operation.
- **Principles are numbered, prioritized ("earlier-numbered wins"), and cited
  inline** throughout the layer docs, so trade-offs are traceable.
- **Rejected alternatives are recorded** (D2 immutable blobs, D3 resurrection,
  D5 mutation-tolerant iteration, D6 transactions, Appendix B allocators) with
  the reasons and the conditions to revisit.

The main problem is not quality but **staleness**: the implementation has
moved past the documents in two significant ways (root-id convention, UBI
integration shape), and the contract document now mis-states behavior that
upper layers are supposed to build on. Since `l1_blob_db.md` is explicitly
"everything upper layers may rely on — and nothing more", a divergence there
is a correctness issue for the docs, not a cosmetic one.

Findings below are ordered by severity. §2 is doc↔code divergence, §3 is
substance of the designs themselves, §4 is hygiene, §5 is a prioritized
action list.

---

## 2. Doc ↔ code divergences

### F1 — The root-id convention changed in the code; the contract still describes the old one (major)

The contract (`l1_blob_db.md` §2, "Root convention") specifies:

> The very first `alloc_id` after a fresh format returns **id = 1** … the
> client binds it.

The implemented behavior (`include/app/lib/blob_db.h`, @ref blob_db_root) is
materially different:

- **Mount/format itself allocates and binds id 1** to an empty payload.
  `BLOB_DB_ROOT_ID` is guaranteed live after every successful mount;
  `get(1)` never returns `-ENOENT`.
- `alloc_id()` **never returns 1**; the first user-visible id is **2**.
- `delete(1)` is undefined behavior.

This ripples through nearly every document:

| Document | Stale statement |
|---|---|
| `l1_blob_db.md` §2 | "First boot: id = blob_db_alloc_id() → id = 1" |
| `architecture.md` §3 | "The first id ever assigned is **1** (L1's root convention)" |
| `l1_model_container.md` §2, §3.1 | client "persists `list_id` (as id = 1 …)" via its own alloc/bind |
| `l1_root_registry.md` §6 | the whole bootstrap procedure: the `rc == -ENOENT` branch and the "`alloc_id()` must return 1, else `-EIO`" virgin check **can never execute** against the current L1 |
| `impl/l1_bucketlog.md` §11 test 1 | "first `alloc_id()==1`" |
| `impl/l1_bucketlog.md` §13.1 | the closing note about "a crash that returns id = 1 but never binds it" — that crash window no longer exists |

Notably, `rootreg.h` already documents the *new* convention correctly
("virgin store = root exists with an **empty payload**"), so the code tree is
internally consistent — only the design docs lag. The registry doc's §6
bootstrap and §10 test 1/5 need rewriting around the new virgin-detection
rule (empty payload, not `-ENOENT`), and the contract's root-convention
section needs to describe the reserved-id model, including the new invariants
(`exists(1)` always true, `alloc ≥ 2`, `delete(1)` UB).

The new convention is a *better* design than the documented one — it removes
the awkward "store is not virgin — refuse" failure mode and gives every
client an unconditional anchor — but it is a real contract change and should
be recorded as such (a decision entry D7 with the rejected alternative being
the old client-bound convention).

### F2 — UBI landed as an L1-internal storage backend, not as the documented L0 flash_area provider (major)

`l0_flash.md` §4/§5 and `architecture.md` §4 describe the future UBI
integration as: a virtual `flash_area` provider, integrated by "a pointer
swap via `CONFIG_BLOB_DB_PARTITION_LABEL`", "zero code in this repository",
"today L0 has no symbols of its own".

What was actually merged (PR #5):

- A **storage-backend seam inside `blob_db`**: `lib/blob_db/blob_db_store.h`,
  a PEB-addressed read/write/erase interface with two providers
  (`blob_db_store_flash.c`, `blob_db_store_ubi.c`).
- A **Kconfig `choice BLOB_DB_BACKEND`** (`_FLASH_AREA` default, `_UBI`
  selecting `UBI_ENABLE`), not a partition-label swap.
- A `ubi` module pulled in via `west.yml` (currently a fork branch pending an
  upstream PR).

So the *actual* L0 boundary is no longer `flash_area_*` — it is
`blob_db_store_*`, and `flash_area` is one provider behind it. That is a
perfectly sound (arguably cleaner) shape, but three documentation
consequences follow:

1. `l0_flash.md` describes an integration plan that was superseded. It should
   either be rewritten around the store seam as the L0 contract, or record
   why the virtual-flash_area plan was abandoned (the practical reason is
   visible in `west.yml`: UBI's own API — `ubi_leb_write_at()` — was the
   natural attachment point, not a synthesized flash_area).
2. The **load-bearing torn-write clause** of `l0_flash.md` §2 ("blast radius
   confined to the affected sector") is currently restated nowhere on the new
   seam. `blob_db_store.h` documents geometry and the no-cross-PEB rule but
   not the crash model that L1's whole recovery design assumes. The contract
   row should move to (or be duplicated on) the seam, and the UBI backend
   should state that it preserves it.
3. `architecture.md` §2's boundary line (`L1 ──flash_area API──► L0`), §4's
   L0 summary, and `principles.md` P1's "flash_area today" wording are stale.
   The status paragraph (§1) doesn't mention the backend seam or UBI at all,
   even though it is merged and CI builds both backends.

One technical question the docs should answer explicitly: **cross-backend
mount detection**. Both backends use the same master magic (`BDMS`), and the
`BLOB_DB_BACKEND` Kconfig help says the layouts are "NOT interchangeable".
D1's rule ("distinct on-flash magic so a mismatched mount fails cleanly with
`-ENOTSUP`") was written for allocator swaps; the backend axis is new and has
no equivalent stated rule. If mounting a flash_area-formatted partition
through the UBI backend (or vice versa) can succeed by accident, that is a
gap; if geometry differences make it fail, say where and how.

### F3 — Public API drift: the contract's §4 header no longer matches `blob_db.h` (moderate)

Additions in the code that the contract doesn't know about:

- **`BLOB_DB_ROOT_ID`** macro (part of F1).
- **`blob_db_erase_all()`** — logical wipe without erase. Importantly, it is
  documented in the header as **not crash-atomic** ("a crash mid-call can
  leave some blobs destroyed and others still live"). The contract's §2
  atomicity row currently admits no non-atomic mutating operation, so this
  needs an explicit carve-out in the contract (with its recovery story: store
  stays consistent, rerun after remount) — otherwise a client could
  legitimately assume erase_all is atomic.
- **`blob_db_prepare(n)`** — bucket pre-formatting for large-sector NOR. Pure
  optimization, but it is a public API with observable semantics (erases
  data-free sectors, persists across remount) and belongs in §4.

Specified in the contract but absent from the code:

- **Batch operations (D6)**: `l1_blob_db.md` §4 presents the four `multi_*`
  entry points and their structs as present ("The four entry points are
  defined in §4 … behind `CONFIG_BLOB_DB_MULTI`"), but neither the symbols
  nor `CONFIG_BLOB_DB_MULTI` exist anywhere in the tree. Fine to keep as a
  reserved extension (like D4's pread), but mark it "specified, not
  implemented" the way D4 does, and drop impl §11 test 15 into a
  "when-implemented" note. `architecture.md` §1's "L1 is implemented against
  the current contract" overstates otherwise.

Small retval mismatches worth sweeping while there: header `mount` doesn't
list `-ENOTSUP`, but the implementation returns it (sector size >
`BLOB_DB_SECTOR_BUF_SIZE`, excessive write alignment); the contract lists
`-ENOTSUP` only for "foreign on-flash format".

### F4 — The buffer/RAM story changed: R7 says stack, the code uses .bss (moderate)

`principles.md` P2 and contract R7 promise "transient buffers are
**stack-allocated** and bounded (≤ one erase sector; 4 KB target)"; the impl
doc's cost table says "4 KB stack" per operation. The implementation instead
keeps **two static full-sector buffers in `.bss`** (`g_bbuf`, `g_bbuf_new`),
sized by the new `CONFIG_BLOB_DB_SECTOR_BUF_SIZE` (default 4096, up to 65536
for 64 KB-sector QSPI NOR like mx25r64 on the nRF5340-DK).

The static-buffer choice is defensible (the v1 single-threaded contract makes
them effectively transient, and 64 KB does not fit on any Zephyr stack), but:

- R7 as written is simply violated; it should be re-worded to constrain
  *bounded, single-instance* working memory rather than its storage class.
- The RAM consequence deserves a sentence under P3: on a 64 KB-sector part
  this is **128 KB of permanent .bss**, which dominates the module's RAM
  budget and is exactly the kind of cost P2 says must be documented.
- Impl doc §13.3 ("sector-size portability" open item) is now resolved by
  `BLOB_DB_SECTOR_BUF_SIZE` + the mount-time `-ENOTSUP` check — the open item
  should be closed with the outcome, like §13.1/§13.2 were.

### F5 — Cost claims and cross-references have drifted (minor)

- `blob_db_count()` / `iterate` are documented in the header as **O(n²) over
  slots** (per-slot latest-wins re-verification); the impl doc's §1 table
  still claims a plain full scan ("2045 reads", O(1) RAM). One of them is
  wrong — reconcile.
- Impl §5.1 mount pseudocode still populates a `write_cursor[bid]` array that
  §13.2 explicitly resolved away (re-scan per write). The pseudocode should
  match the resolved design.
- Broken section references: `l0_flash.md` §2 points at "`l1_blob_db.md` §8"
  (the contract has no §8 — the crash model moved to the impl doc);
  `blob_db.h` (`blob_db_count`) points at "`l1_blob_db.md` §11" (§11 is in
  `impl/l1_bucketlog.md`); `blob_db.h`'s trailer and `lib/blob_db/Kconfig`
  help both say the *contract* doc holds "the on-flash format, algorithms,
  and crash recovery details" — those live in the impl doc, by design.
- `l0_flash.md` §3 hard-codes "2045 buckets" and the two master sectors —
  bucket-log implementation details in an L0 contract doc, now doubly wrong
  under the UBI backend where geometry comes from the volume.

### F6 — Kconfig symbol names in the docs don't match the scaffolded code (minor, cheap to fix now)

| Docs (`l2_containers.md` §6, `l3_interfaces.md` §6, `architecture.md` §5) | Code (`lib/*/Kconfig`) |
|---|---|
| `BLOB_CONTAINERS` (menuconfig) | plain `menu` "Containers (L2)", no symbol |
| `CONTAINER_SEQ` / `KVLIST` / `KVHASH` / `KVTREE` | `BLOB_CONTAINER_SEQ` / `_KVLIST` / `_KVHASH` / `_KVTREE` |
| `KVDB`, `BLOBFS` | `BLOBDB_KVDB`, `BLOBDB_BLOBFS` |
| `KVDB_BACKEND_*` / `BLOBFS_DIR_*` choices, `*_INLINE_MAX`, `*_BUCKETS`, `*_FANOUT` | not yet present (skeletons) |
| `CONFIG_BLOB_DB_MULTI` | absent (F3) |

Also: `BLOB_DB_ALLOCATOR` (D1's choice) was never created — the only choice
that exists is `BLOB_DB_BACKEND`, which is a *different axis* (substrate, not
allocator). D1 should acknowledge both axes exist. Since the skeleton symbols
are already merged, either the docs adopt the real names or the skeletons get
renamed before implementations land on them — deciding now is much cheaper
than after L2/L3 code exists.

---

## 3. Substance review of the designs themselves

These stand independent of code drift.

### S1 — Container teardown is unspecified, and it can't be built from the documented pattern (worth a design note)

`l1_root_registry.md` §8 says `unregister` drops the entry and "tearing down
the structure behind a root is the caller's job *before* unregistering (or a
future global sweep's)". But:

- Neither `map_ops` nor `seq_ops` (`l2_containers.md` §3) has a
  `destroy`/`drop` operation.
- The §2.2 mutation discipline can't express teardown of an arbitrary-size
  container: the intent blob's `del[]` must fit one bounded payload, while a
  container holds unboundedly many i-nodes. Destroying one needs either an
  iterative multi-commit protocol (explicitly "outside the contract,
  requires its own crash analysis" per §2.2) or the global sweep — which the
  model-container doc itself notes cannot run safely per-container.
- A crash between the caller's teardown and `unregister` (or mid-teardown)
  strands i-nodes with no intent record covering them — a permanent leak,
  violating P7's must-tier, in the one flow the docs actively recommend.

Recommendation: add a `destroy(root_id)` design to `l2_containers.md` with
its own crash analysis (e.g. unregister-first + "unreachable root" sweep
list, or reverse-order chunked deletion where the root's own deletion is the
commit), or explicitly scope v1 as "containers are never destroyed" so the
gap is at least a recorded decision.

### S2 — The model container's recovery predicate deserves one more edge-case row (small)

`l1_model_container.md` §6 defines `committed?` as "list references any id ≥
W, or (pure delete) list no longer references del[]". For a **mixed**
mutation (creates *and* deletes, §8.2) interrupted between COMMIT and
CLEANUP, the first clause decides correctly — good. But consider a mutation
whose PREPARE succeeds and whose commit image happens to reference *none* of
the new ids (e.g. an upsert that resolved to "value unchanged, drop the
prepared blob"): the first clause reads "not committed" even after the commit
ran. The discipline implicitly forbids commits that don't reference their
prepared blobs, but that invariant is never stated. One sentence — "a
mutation that prepares blobs must reference at least one id ≥ W in its commit
image, else it must abort instead" — would close it.

### S3 — `kvhash` root sizing contradicts its own defaults (small, catch before implementation)

`l2_containers.md` §4.3: root = `{magic, nbuckets, bucket_id[nbuckets]}` with
`CONFIG_CONTAINER_KVHASH_BUCKETS` default **64** → 8 B × 64 + header > 512 B,
while `BLOB_DB_MAX_PAYLOAD_LEN` defaults to **256**. The doc's own §6 note
("root must fit one i-node payload", BUILD_ASSERT rule) means the shipped
defaults are an invalid combination. Either the bucket default drops to ≤ 28,
the payload default rises, or the root grows an overflow scheme. The same
check is worth running for `kvtree` (`FANOUT` 8 × max-entry vs 256 B — tight
for 64 B keys with inline values).

### S4 — `architecture.md` stack diagram places the root registry as a layer L2 sits on (presentation)

The §2 diagram draws L2 above "L1½", implying containers depend on the
registry, while `l1_root_registry.md` §2 is explicit that containers do *not*
use it (its clients are L3/app). The §2 dependency chain
(`L3 → L2 → L1 → L0`) skips it correctly. Drawing the registry as a sibling
box beside L2 (both on L1) would match the actual dependency story.

### S5 — Concurrency posture is consistent and honest — keep it visible at v2 time (no action now)

"Single-threaded, caller serializes" is stated in all four places it matters
(contract §4, model container §6 "serialized" bullet, L2 §1, L3 §1), and the
model container correctly derives that its watermark recovery *depends* on
it. The docs even flag what v2 needs (global mutation lock or per-mutation id
ranges). Good. The `.bss` buffers of F4 quietly add a fifth dependency on
this assumption (shared mutable buffers), which is worth listing in the
contract's concurrency paragraph so a future v2 doesn't miss it.

### S6 — The demo still binds the root directly (tracked, fine)

`app/src/main.c` persists the boot counter at `BLOB_DB_ROOT_ID` — legitimate
in a registry-less build and already tracked as impl §13.6. Once rootreg is
implemented, remember the migration also invalidates any store formatted by
the current demo (registry `-ENOTSUP` on a foreign id-1 payload) — the doc's
own §8 point, worth a line in the demo when it happens.

---

## 4. Doc hygiene (quick sweep)

- `architecture.md` §1 status is otherwise accurate and commendably specific
  (scaffolded-vs-implemented distinction, test-suite names) — it just misses
  the UBI/backend work (F2) and overstates batch ops (F3).
- `l1_root_registry.md` §5's API block omits `rootreg_init()` even though §6
  specifies it and the header ships it — add it to the API list.
- `l1_root_registry.md` §9 sketch (`config BLOB_ROOTREG … depends on
  BLOB_DB`) matches the merged Kconfig — nice; the `range 1 1000` in the real
  Kconfig relies on the BUILD_ASSERT to catch payload overflow, matching the
  doc.
- Terminology is impressively consistent across the set (i-node, bind,
  watermark, staged window). One exception: the contract calls iteration
  "diagnostics and fsck-like repair, not a data path" while `l3_interfaces.md`
  §3 wires `kvdb_foreach` — a user-facing data-path API — onto container
  `iterate`. If `foreach` inherits "not a data path" performance *and* the
  no-mutation UB, its header docs must say so as loudly as L1's do (the doc
  does inherit the UB rule; the cost expectation is the part users will trip
  on with kvlist/kvhash backends).

---

## 5. Prioritized recommendations

1. **(F1)** Rewrite the contract's root convention around the implemented
   reserved-root model; ripple to `architecture.md`, model container,
   registry §6/§10, impl §11/§13.1. Record it as a numbered decision with the
   old convention as the rejected alternative.
2. **(F2)** Rewrite `l0_flash.md` around the `blob_db_store` seam (or add an
   ADR superseding the virtual-flash_area plan); restate the torn-write
   blast-radius contract on the seam; update `architecture.md` §1/§2/§4 and
   P1; answer the cross-backend mount-detection question.
3. **(F3)** Sync contract §4 with `blob_db.h`: add `BLOB_DB_ROOT_ID`,
   `erase_all` (with its explicit atomicity carve-out), `prepare`; mark D6
   batch ops "specified, not implemented".
4. **(S1)** Decide and document the container-teardown story before L2
   implementation starts — it is the one P7 hole the current pattern can't
   cover.
5. **(S3)** Fix the kvhash/kvtree default-sizing contradictions before the
   skeletons grow code.
6. **(F6)** Align doc Kconfig names with the merged skeleton symbols (or
   rename the skeletons) now, while it's a rename with zero users.
7. **(F4, F5)** Re-word R7/P2 for the .bss buffers and document the
   large-sector RAM cost; close impl §13.3; fix the O(n²) vs full-scan
   discrepancy, the stale mount pseudocode, and the four broken
   cross-references.
