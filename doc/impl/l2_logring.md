# Implementation design — `logring` bounded circular log (L2)

> **⚠ Superseded — do not implement from this document.** It once described a
> v1 *single-i-node* `logring` (the code currently on branch
> `claude/log-container-type-0xl2dp`, PR #32). The accepted design is the
> **per-entry-i-node chain** in `doc/proposals/2026-10-08-logring.md`; this
> file is rewritten from that proposal when the chain implementation lands
> (proposal §9).

Status: **stub / superseded** · normative contract: `doc/layers/l2_containers.md`
§4.5 · design of record: `doc/proposals/2026-10-08-logring.md`.

---

## Why the old design was dropped

The v1 design packed variable-length records inline in a **single `blob_db`
payload** and committed every mutation with one `blob_db_update` — the
`rootreg` pattern, no intent, no recovery. It was correct and simple, but its
capacity is one i-node payload (~236 B at the default geometry). The real
workload is a **1–2 MB** device log of individually-addressable entries (see
the proposal's R1–R9), which a single payload cannot hold.

## What replaces it (summary — see the proposal for the full design)

- **A chain of per-entry i-nodes.** Each log entry is its own `blob_db` blob:
  `entry { next_id, len, bytes }`. A small `root` holds the chain endpoints,
  the next reservation, and a soft byte budget.
- **Pre-reserved forward links.** Each `next_id` points at an id already
  reserved via `alloc_id()` but not yet bound; append *binds* that id, writing
  one new blob and **rewriting nothing** (~1× write amplification, versus 2×
  for patch-the-predecessor and up to 32× for inline packing).
- **Entry-id cursor.** The moniker is the entry's own id — persistable,
  O(1)-resumable, and an evicted position is detected via `blob_db`'s
  tombstone-no-reuse and the ids-increase-head→tail invariant.
- **Soft retention, resumable eviction.** The byte budget is a hint (with an
  `-ENOSPC` hard backstop); eviction detaches a run of oldest entries in one
  `root.head_id` commit and reclaims them by following the chain via a small
  resumable intent — `logring`'s one use of the shared intent helper.

See `doc/proposals/2026-10-08-logring.md` for objects, append/evict traces,
per-crash-point residue tables, the cursor contract, cost summary, and the
open review decisions.
