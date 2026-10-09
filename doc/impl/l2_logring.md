# Implementation design — `logring` bounded circular log (L2)

> **⚠ Superseded — do not implement from this document.** It describes a
> v1 *single-i-node* `logring` (the code currently on branch
> `claude/log-container-type-0xl2dp`, PR #32). The **accepted** design is the
> **per-entry-i-node chain** in `doc/proposals/2026-10-08-logring.md`; this
> file is rewritten from that proposal when the chain implementation lands
> (proposal §9).

Status: **stub / superseded** (accepted design pending implementation)
· normative contract: `doc/layers/l2_containers.md` §4.5
· design of record: `doc/proposals/2026-10-08-logring.md`.

---

## Why the old design was dropped

The v1 design packed variable-length records inline in a **single `blob_db`
payload** and committed every mutation with one `blob_db_update` — the
`rootreg` pattern, no intent, no recovery. It was correct and simple, but its
capacity is one i-node payload (~236 B at the default geometry). The real
workload is a **large** device log — up to near the whole partition — of
individually-addressable entries (see the proposal's R1–R9/G1), which a single
payload cannot hold.

## What replaces it (summary — see the proposal for the full design)

- **A chain of per-entry i-nodes.** Each log entry is its own `blob_db` blob:
  `entry { next_id, len, bytes }`.
- **Pre-reserved forward links.** Each `next_id` points at an id already
  reserved via `alloc_id()` but not yet bound; append *binds* that id, writing
  one new blob and **rewriting nothing** (~1× write amplification, versus 2×
  for patch-the-predecessor and up to 32× for inline packing).
- **RAM handle + waypoint checkpoint.** A small RAM handle caches the exact
  tail so warm appends are O(1); the `root` holds only a **waypoint** near the
  end plus soft hints, rewritten ~once per K appends (not per append), which
  dissolves the per-append root-write hotspot and is the anchor `open` rebuilds
  the tail from.
- **Opaque forward cursor.** A persistable position token (journal-/`lseek`-
  style): `seek_oldest` / `seek_newest` (O(1), reaches the real end — G1) /
  `next`; O(1) resume; an aged-out position returns `-ESTALE`.
- **Soft retention, batched resumable eviction.** The byte budget is a hint
  (with an `-ENOSPC` hard backstop); eviction detaches a run of oldest entries
  in the same single root write as a checkpoint and reclaims them by following
  the chain via a small resumable intent — `logring`'s one use of the shared
  intent helper.

See `doc/proposals/2026-10-08-logring.md` for objects, append/evict traces,
per-crash-point residue tables, the cursor contract, cost summary, and the
resolved decisions.
