# L1 — Model Container: Sufficiency Proof for the `blob_db` Contract

Status: v3 · Companion to `doc/layers/l1_blob_db.md` (the contract)
· Governed by `doc/principles.md`

This document is neither a contract nor an implementation design. It is a
**proof by construction**: using nothing but the L1 contract, it builds the
smallest complete crash-safe structure — demonstrating that `blob_db` as
specified is *sufficient* to implement every layer above it, with no
additional API or guarantee needed.

---

## 1. Purpose

The **model container** is the smallest structure that exercises every
element of the `blob_db` contract — stable ids as persistent references,
single-operation atomicity, and the discipline that makes multi-i-node
mutations crash-safe.

It is deliberately **not a usable container** (O(n) flash reads per lookup, one
i-node per key and per value), and it is **without compromises**: it satisfies
P7 in full, including *no permanent leak* — every crash residue is reclaimed
by bounded, idempotent recovery. Three roles follow:

- **Sufficiency proof.** If the model container works, the contract is
  enough; conversely, any capability the model needs that the contract lacks
  is a contract gap (this is how `alloc_id` earned its place in §4 of the
  contract).
- **Reference pattern for every real container** (`l2_containers.md`): a
  container built by composing these step patterns inherits the proof; one
  that deviates needs its own crash analysis against P7 (§9).
- **Blueprint of the L1 acceptance test**: a ztest suite implementing it
  verbatim, with crash injection between every step, validates any allocator
  claiming to implement the contract — independent of real L2 code.

The document builds up in two stages: the **basic flow** (§2–§3), where every
mutation is a single atomic operation and no extra machinery exists at all —
this covers the most common operations — and the **extension** (§4–§8) for
mutations that a single operation cannot express, where the intent blob and
the id watermark close every crash window.

## 2. The structure

A key→value mapping built from three kinds of i-node, fully indirected —
every piece of data is its own i-node, every reference an explicit id:

```
key blob     "foo"                            own id: kid
value blob   "bar"                            own id: vid
list blob    [(kid1,vid1), (kid2,vid2), …]    own id: list_id   ← container root
```

The client persists nothing but `list_id` (P5) — bound to id = 1 directly in
a minimal build, or typically kept as an entry in the root registry helper
(`l1_root_registry.md`), which itself hangs off id = 1. Everything is
reachable from it.

## 3. The basic flow

### 3.1 Create — the container comes into existence

```
list_id = blob_db_alloc_id()                  ← RAM only, nothing on flash
blob_db_update(list_id, empty pair list)      ← one atomic write: binds the root
```

The container exists from the moment the `update` commits; the client
persists `list_id` (as id = 1, or in the id = 1 root). Crash before the bind:
an allocated id and nothing on flash — no residue at all. Crash after: an
empty, fully valid container.

### 3.2 First insert — `set("foo", "bar")`

New data means new i-nodes, and the one ordering rule of the basic flow is:
**bind the referenced blobs first, make them reachable last** — the commit
is the final `update` of the list:

```
step 1  kid = blob_db_alloc_id()                    ┐ RAM only — a crash here
        vid = blob_db_alloc_id()                    ┘ leaves nothing on flash
step 2  blob_db_update(kid, "foo")                  new blob, unreferenced so far
step 3  blob_db_update(vid, "bar")                  new blob, unreferenced so far
step 4  blob_db_update(list_id, …+(kid,vid))        COMMIT — atomic (spec §2)
```

| Crash after | Visible state on remount | Leftover on flash |
|---|---|---|
| step 1 | list unchanged, `get("foo")` → not found | none (ids burned, free) |
| step 2 | list unchanged, `get("foo")` → not found | `kid`, unreferenced |
| step 3 | list unchanged, `get("foo")` → not found | `kid`, `vid`, unreferenced |
| step 4 | `get("foo")` → "bar", mutation complete | none |

Correctness is already perfect: at every crash point a reader or remount sees
the complete old or the complete new mapping — never a torn list, never a
dangling reference. The commit-last ordering alone gives P7's must-tier
atomicity. Note the leftover column, though — it returns in §3.5.

### 3.3 Lookup — `get("foo")`

```
blob_db_get(list_id) → the pair list
for each (kid_i, vid_i):  blob_db_get(kid_i), compare with "foo"
blob_db_get(vid_i of the match) → "bar"
```

O(n) flash reads — the honest price of full indirection. Read-only: no crash
windows.

### 3.4 Overwrite — `set("foo", "baz")` when "foo" already exists

Ids are stable and payloads mutable (spec §2, decision D2): the pair
`(kid, vid)` keeps referencing `vid`, and only `vid`'s content changes.

```
blob_db_update(vid, "baz")                    ← the entire mutation
```

One atomic operation (spec §2): after a crash the value is complete "bar"
or complete "baz" — never torn, never absent. Nothing was created, nothing
must die, there is no residue and nothing to recover.

This is the load-bearing principle of the basic flow:

> **A mutation that does not change the reference graph collapses to a single
> atomic `update`. Only mutations that change *which ids are referenced*
> need more (the ordering rule of §3.2 at minimum).**

In the basic flow the whole crash story is L1's single-operation atomicity
plus the commit-last ordering. A well-designed container routes every
mutation it can through the single-`update` form.

### 3.5 Where the basic flow ends

Look back at the leftover column of §3.2 — and delete has the mirror problem:
the pair's blobs can only be deleted *after* the commit that unlinks them,
and a crash in between strands them. The visible state is always correct, but
nothing records that `kid`/`vid` were created (or were scheduled to die), so
after a crash nothing can ever reclaim them. Each interrupted mutation leaks
a few blobs; over a device's lifetime that is unbounded, which violates P7's
**no-permanent-leak** requirement.

Closing exactly this gap — reclamation, not correctness — is what the
extension adds.

## 4. Extension: the intent blob and the id watermark

For reference-graph mutations the structure grows a fourth i-node, and the
container uses one more API:

```
intent blob  {} or { W, del[] }               own id: intent_id ← mutation journal
list blob    { intent_id, [(kid1,vid1), …] }  own id: list_id   ← root now records intent_id
```

The intent blob is created once, together with the container; its id is
stored in the root and never changes (id stability — the journal is only ever
`update`d). It is empty whenever no mutation is in flight.

Recovery is built on `blob_db_alloc_id()` (spec §4). Because ids are strictly
monotonic and never reused across any crash (spec §2), the
mutation's **first allocation is its watermark**: `W = alloc_id()`, and every
id the mutation allocates afterwards is > W — while nothing else in the
database has an id ≥ W. (`W` itself is never bound; recovery's `delete(W)`
hits `-ENOENT`, which it tolerates.) Durability of `W` comes from writing it
into the intent blob, not from the allocation.

### 4.1 Crash-residue requirements (P7 applied)

| Tier | Requirement | Provided by |
|---|---|---|
| **must** | A partially written, power-interrupted operation is never data | L1: a torn write is detected and discarded (spec §2). Model: a reference that does not resolve is treated as *absent*, never surfaced as data |
| **must** | Visible state flips atomically old → new | one commit per mutation: the single `update` of §3.4, or step COMMIT of §5 |
| **must** | No permanent leak — residue reclaimed | intent record + watermark recovery (§6) |
| advisable | Self-healing of impossible residue | dangling pair → entry dropped (§7) |
| advisable | Avoidance: ordering minimizes residue windows | prepare-before-commit, cleanup-after-commit (§5) |

## 5. The extended mutation discipline

Every reference-graph mutation follows one shape; each step is an
individually atomic `blob_db` call:

```
step 0  W = alloc_id()                    (RAM only — the watermark)
step 1  update(intent_id, {W, del[]})     STAGE   declare the mutation durable:
                                                  "ids ≥ W are mine; del[] dies on commit"
step 2  N × (alloc_id + update)           PREPARE new blobs, all ids > W,
                                                  unreferenced so far
step 3  one update(list_id, new image)    COMMIT  the linearization point
step 4  M × delete(del[])                 CLEANUP superseded blobs removed
step 5  update(intent_id, {})             CLEAR   mutation sealed
```

- Only steps 2 and 4 can produce unreferenced i-nodes, and both lie inside
  the staged window — recovery (§6) reclaims them. The commit itself can
  never leak: it atomically swaps *which* set of i-nodes is referenced.
- A reader, or a remount at any crash point, sees the complete old or the
  complete new mapping — never a torn list, never a dangling reference.
- A single-`update` mutation (§3.4) is this shape with steps 0–2 and 4–5
  empty: the commit alone. No stage, no residue possible. §3.2's first insert
  is this shape with steps 0–1 and 4–5 missing — which is exactly why it
  leaked.

Cost: two intent writes per reference-graph mutation. That is the price of
zero leak; real containers may amortize it (§9) but the model pays it plainly.

**Failure without power loss (abort path).** If a step fails cleanly — e.g.
`-ENOSPC` during PREPARE — the mutation aborts: delete the blobs prepared so
far, then CLEAR the intent (the mirror of cleanup). The visible state was
never touched. A crash *during* the abort is indistinguishable from a crash
during the mutation itself, so recovery (§6) handles it with no special
state — abort needs no new machinery.

## 6. Recovery — bounded and idempotent

On `open(list_id)` (and thus on every remount), read the intent blob. If it is
empty, there is nothing to do — the normal case costs one read. Otherwise a
mutation was interrupted, and `{W, del[]}` decides:

```
committed? :=  the list references any id ≥ W        (mutation that created blobs)
               or, if it created none (pure delete):
               the list no longer references del[]

if committed:   ROLL FORWARD   delete every id in del[]   (-ENOENT tolerated)
else:           ROLL BACK      iterate ids ≥ W, delete any the list
                               does not reference
finally:        update(intent_id, {})                      (CLEAR)
```

Properties:

- **Bounded**: at most one mutation's blobs are touched; the iterate in the
  rollback branch runs only after a crash inside a staged window.
- **Idempotent**: a crash during recovery re-enters the same branch — the
  commit state cannot change, deletes tolerate `-ENOENT`, and CLEAR is last.
- **Complete**: every crash point in §8 lands in exactly one branch; the
  tables show there is no window that leaks past recovery.
- **Serialized**: assumes one mutation in flight per database — "unreferenced
  ids ≥ W are mine" holds only if nothing else allocated ids inside the
  staged window. Given by the v1 single-threaded contract; recovery itself
  runs at `open`, before any other traffic. A concurrent v2 needs a global
  mutation lock or per-mutation id ranges.

## 7. Self-healing (defense in depth)

Under the discipline of §5 a **dangling reference** — a pair whose `kid` or
`vid` no longer resolves — cannot occur. If one is observed anyway (software
defect, external interference), it is *impossible residue* and must not be
carried:

- lookup treats the pair as absent (`-ENOENT` from `get(kid)` ⇒ key not
  present) — the must-tier rule that unresolvable data is not data;
- the next mutation of the list (or an explicit repair pass) drops the entry
  as part of its own commit.

## 8. The extended operations, traced

### 8.1 Insert — `set("foo", "bar")`

The first insert of §3.2, completed: the same calls, wrapped in
stage/clear so the leftovers of its crash table become reclaimable.

```
0  W = blob_db_alloc_id()                             (RAM only)
1  blob_db_update(intent_id, {W})                     STAGE
2  kid = blob_db_alloc_id(); blob_db_update(kid, "foo")   PREPARE
3  vid = blob_db_alloc_id(); blob_db_update(vid, "bar")   PREPARE
4  blob_db_update(list_id, […, (kid,vid)])            COMMIT
5  blob_db_update(intent_id, {})                      CLEAR
```

| Crash after | Visible state on remount | Residue | Recovery |
|---|---|---|---|
| 1 | old list | none | roll back: nothing ≥ W exists; clear |
| 2 | old list | `kid` | roll back: delete `kid`; clear |
| 3 | old list | `kid`, `vid` | roll back: delete both; clear |
| 4 | **new mapping** | staged intent | committed (list refs ids ≥ W): forward; clear |
| 5 | new mapping | none | — |

### 8.2 Value replacement — when in-place `update` (§3.4) won't do

Applies when the value blob must genuinely be replaced: it is shared and an
in-place change would leak to other referrers, or the store follows the
immutable profile (spec decision D2).

```
0  W = blob_db_alloc_id()                             (RAM only)
1  blob_db_update(intent_id, {W, del:[vid]})          STAGE
2  vid2 = blob_db_alloc_id(); blob_db_update(vid2, "baz")   PREPARE
3  blob_db_update(list_id, […(kid,vid2)…])            COMMIT
4  blob_db_delete(vid)                                CLEANUP
5  blob_db_update(intent_id, {})                      CLEAR
```

| Crash after | Visible state | Residue | Recovery |
|---|---|---|---|
| 1 | old mapping | none | not committed (`vid` still referenced): roll back; clear |
| 2 | old mapping | `vid2` | roll back: delete `vid2`; clear |
| 3 | **new mapping** | old `vid` | committed: delete `vid`; clear |
| 4 | new mapping | staged intent | committed: delete `vid` → `-ENOENT`, fine; clear |
| 5 | new mapping | none | — |

### 8.3 Delete — `del("foo")`

```
0  W = blob_db_alloc_id()                             (RAM only)
1  blob_db_update(intent_id, {W, del:[kid,vid]})      STAGE
2  blob_db_update(list_id, list without (kid,vid))    COMMIT
3  blob_db_delete(kid)                                CLEANUP
4  blob_db_delete(vid)                                CLEANUP
5  blob_db_update(intent_id, {})                      CLEAR
```

| Crash after | Visible state | Residue | Recovery |
|---|---|---|---|
| 1 | mapping present | none | not committed (del[] still referenced): roll back; clear |
| 2 | **mapping gone** | `kid`, `vid` | committed (del[] unreferenced): delete both; clear |
| 3 | mapping gone | `vid` | committed: delete both (`kid` → `-ENOENT`); clear |
| 4 | mapping gone | staged intent | committed: deletes → `-ENOENT`; clear |
| 5 | mapping gone | none | — |

## 9. What real containers change — and what they must not

Real containers (`l2_containers.md`) optimize the model in three ways, none of
which may weaken §4.1:

- **Routing mutations through the basic flow** (§3.4) wherever possible —
  in-place value updates, inlining small keys/values into the referencing
  i-node — fewer blobs, fewer staged steps, more mutations with zero residue;
- **Better reference-graph shapes** (hash buckets, tree nodes) — O(n) → O(1)
  / O(log n);
- **Amortizing the intent cost** — one intent blob per container (as here)
  serves any mutation; a container may also batch several mutations under one
  stage/clear pair, at the price of a wider (still bounded, still recoverable)
  staged window.

A mutation that cannot be expressed as *one atomic update* (§3) or as
*stage → prepare → one commit → cleanup → clear* (§5) — e.g. one needing two
commits — is outside the contract and requires its own crash analysis against
P7.

A coarser alternative to the intent record is a **global sweep** (mark
reachable from id = 1, `blob_db_iterate` everything, delete the difference).
It needs no per-mutation writes and no watermark API, but it must see the
*whole* database (a single container cannot sweep safely next to others) and
costs a full scan whenever it runs. The intent record is the composable,
normative mechanism; a sweep may exist additionally as a maintenance tool.
