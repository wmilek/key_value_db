# L1 — Model Container: the `blob_db` Client Contract by Example

Status: v2 · Companion to `doc/layers/l1_blob_db.md` (the blob_db spec)
· Governed by `doc/principles.md`

---

## 1. Purpose

This document defines the **model container**: the smallest structure that
exercises every element of the `blob_db` client contract — stable ids as
persistent references, single-operation atomicity, and the discipline that
makes multi-i-node mutations crash-safe.

It is deliberately **not a usable container** (O(n) flash reads per lookup, one
i-node per key and per value). Its role is normative, and therefore it is
**without compromises**: it satisfies P7 in full, including *no permanent
leak* — every crash residue is reclaimed by bounded, idempotent recovery.

- it **defines the contract**: what a correct client may assume of `blob_db`
  and the obligations it carries in return;
- it is the **reference for every real container** (`l2_containers.md`) —
  each is a composition of exactly these step patterns;
- it is the natural shape of the **L1 acceptance test**: a ztest suite
  implementing it verbatim, with crash injection between every step,
  validates the contract independent of any real L2 code.

## 2. The structure

A key→value mapping built from four kinds of i-node, fully indirected —
every piece of data is its own i-node, every reference an explicit id:

```
key blob     "foo"                                  own id: kid
value blob   "bar"                                  own id: vid
list blob    { intent_id, [(kid1,vid1), …] }        own id: list_id   ← container root
intent blob  {} or { W, del[] }                     own id: intent_id ← mutation journal
```

The client persists nothing but `list_id` (in practice id = 1, or a field of
the id = 1 root — P5). The intent blob is created together with the container;
its id is recorded in the root and never changes. It is empty whenever no
mutation is in flight.

Recovery is built on `blob_db_next_id()` (spec §9) — a pure-RAM accessor for
the next id `put` would assign. Because ids are strictly monotonic and never
reused across any crash (spec §2, with the scan and compaction rules of
§7.1/§7.6 that enforce it), the value `W` read before a mutation is a
**watermark**: every blob the mutation creates has id ≥ W, and nothing else in
the database does. Durability of `W` comes from the intent write below, not
from the accessor.

## 3. Crash-residue requirements (P7 applied)

| Tier | Requirement | Provided by |
|---|---|---|
| **must** | A partially written, power-interrupted operation is never data | L1: torn slot fails CRC → invisible (spec §8). Model: a reference that does not resolve is treated as *absent*, never surfaced as data |
| **must** | Visible state flips atomically old → new | the single commit `update` (§4) |
| **must** | No permanent leak — residue reclaimed | intent record + watermark recovery (§5) |
| advisable | Self-healing of impossible residue | dangling pair → entry dropped (§6) |
| advisable | Avoidance: ordering minimizes residue windows | prepare-before-commit, cleanup-after-commit (§4) |

## 4. The mutation discipline

Every mutation follows one shape; each step is an individually atomic
`blob_db` call:

```
step 0  W = blob_db_next_id()                     (RAM read, no I/O)
step 1  update(intent_id, {W, del[]})     STAGE   declare the mutation durable:
                                                  "ids ≥ W are mine; del[] dies on commit"
step 2  N × put(new objects)              PREPARE all get ids ≥ W, unreferenced so far
step 3  one update(list_id, new image)    COMMIT  the linearization point
step 4  M × delete(del[])                 CLEANUP superseded blobs removed
step 5  update(intent_id, {})             CLEAR   mutation sealed
```

- Only steps 2 and 4 can produce unreferenced i-nodes, and both lie inside
  the staged window — recovery (§5) reclaims them. The commit itself can
  never leak: it atomically swaps *which* set of i-nodes is referenced.
- A reader, or a remount at any crash point, sees the complete old or the
  complete new mapping — never a torn list, never a dangling reference.
- Fast path: a mutation that creates and deletes no blobs (e.g. an inline
  change confined to the root) is step 3 alone — no stage, no residue
  possible.

Cost: two intent writes per residue-capable mutation. That is the price of
zero leak; real containers may amortize it (§7) but the model pays it plainly.

## 5. Recovery — bounded and idempotent

On `open(list_id)` (and thus on every remount), read the intent blob. If it is
empty, there is nothing to do — the fast path costs one read. Otherwise a
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
- **Complete**: every crash point in §4 lands in exactly one branch; the
  tables below show there is no window that leaks past recovery.
- **Serialized**: assumes one mutation in flight per database — "unreferenced
  ids ≥ W are mine" holds only if nothing else allocated ids inside the
  staged window. Given by the v1 single-threaded contract; recovery itself
  runs at `open`, before any other traffic. A concurrent v2 needs a global
  mutation lock or per-mutation id ranges.

## 6. Self-healing (defense in depth)

Under the discipline of §4 a **dangling reference** — a pair whose `kid` or
`vid` no longer resolves — cannot occur. If one is observed anyway (software
defect, external interference), it is *impossible residue* and must not be
carried:

- lookup treats the pair as absent (`-ENOENT` from `get(kid)` ⇒ key not
  present) — the must-tier rule that unresolvable data is not data;
- the next mutation of the list (or an explicit repair pass) drops the entry
  as part of its own commit.

## 7. The operations, traced

### 7.1 Insert — `set("foo", "bar")`

```
0  W = blob_db_next_id()
1  blob_db_update(intent_id, {W})                     STAGE
2  kid = blob_db_put("foo")                           PREPARE
3  vid = blob_db_put("bar")                           PREPARE
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

### 7.2 Overwrite — `set("foo", "baz")` (key blob reused)

```
0  W = blob_db_next_id()
1  blob_db_update(intent_id, {W, del:[vid]})          STAGE
2  vid2 = blob_db_put("baz")                          PREPARE
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

### 7.3 Delete — `del("foo")`

```
0  W = blob_db_next_id()
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

### 7.4 Lookup — `get("foo")`

```
blob_db_get(list_id) → intent check (§5) → pair list
for each (kid_i, vid_i):  blob_db_get(kid_i), compare
blob_db_get(vid_i of the match) → "bar"
```

O(n) flash reads — the honest price of full indirection. No mutation, no
residue windows.

## 8. What real containers change — and what they must not

Real containers (`l2_containers.md`) optimize the model in three ways, none of
which may weaken §3:

- **Inlining** small keys/values into the referencing i-node — fewer blobs,
  fewer staged steps, more mutations on the zero-residue fast path;
- **Better reference-graph shapes** (hash buckets, tree nodes) — O(n) → O(1)
  / O(log n);
- **Amortizing the intent cost** — one intent blob per container (as here)
  serves any mutation; a container may also batch several mutations under one
  stage/clear pair, at the price of a wider (still bounded, still recoverable)
  staged window.

A mutation that cannot be expressed as *stage → prepare → one commit →
cleanup → clear* (e.g. one needing two commits) is outside the contract and
requires its own crash analysis against P7.

A coarser alternative to the intent record is a **global sweep** (mark
reachable from id = 1, `blob_db_iterate` everything, delete the difference).
It needs no per-mutation writes and no watermark API, but it must see the
*whole* database (a single container cannot sweep safely next to others) and
costs a full scan whenever it runs. The intent record is the composable,
normative mechanism; a sweep may exist additionally as a maintenance tool.
