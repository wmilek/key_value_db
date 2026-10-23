# L1 — Model Container Map: Sufficiency Proof for the `blob_db` Contract

Status: v4 · Companion to `doc/layers/l1_blob_db.md` (the contract)
· Governed by `doc/principles.md`

This document is neither a contract nor an implementation design. It is a
**proof by construction**: using nothing but the L1 contract, it builds the
smallest complete crash-safe structure — demonstrating that `blob_db` as
specified is *sufficient* to implement every layer above it, with no
additional API or guarantee needed.

---

## 1. Purpose

The **model container map** (`model_container_map`, implemented in
`tests/lib/blob_db_contract/src/model_container_map.c`) is the smallest
structure that exercises every element of the `blob_db` contract: stable ids
as persistent references, single-operation atomicity, and the discipline that
makes multi-i-node mutations crash-safe.

Model containers are teaching structures. Each one shows, in a typical
situation, how i-nodes and an intent record turn a multi-step mutation into
one that is atomic against power loss, and how recovery finishes or undoes
it. They are kept free of every optimization — no payload inlined into the
referencing i-node, no in-place shortcut where the full discipline can be
shown — so that each step of the contract stays visible. Their use in a
product is limited by design. The map is the first; any further model
container follows the same discipline and registers under its own `'MC_x'`
key. The second is the doubly linked list (`l1_model_container_list.md`,
`'MC_L'`), whose mutations each write two existing i-nodes.

It is deliberately **not a usable container** (O(n) flash reads per lookup, one
i-node per key and per value), and it is **without compromises**: it satisfies
P7 in full, including *no permanent leak* — every crash residue is reclaimed
by bounded, idempotent recovery. Three roles follow:

- **Sufficiency proof.** If the model container map works, the contract is
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
mutations that a single operation cannot express, where the intent blob, naming
every id the mutation creates or retires, closes every crash window.

## 2. The structure

A key→value mapping built from three kinds of i-node, fully indirected —
every piece of data is its own i-node, every reference an explicit id:

```
key blob     "foo"                            own id: kid
value blob   "bar"                            own id: vid
list blob    [(kid1,vid1), (kid2,vid2), …]    own id: list_id   ← container root
```

The client persists nothing but `list_id` (P5) — in a minimal build the list
lives at id = 1 directly (id 1 always exists after mount; the client simply
`update`s it, no allocation needed), or typically `list_id` is kept as an
entry in the root registry helper (`l1_root_registry.md`), which itself
occupies id = 1 — `model_container_map` registers under the key `('MC_M', 0)`.
Everything is reachable from it.

## 3. The basic flow

### 3.1 Create — the container comes into existence

```
list_id = blob_db_alloc_id()                  ← RAM only, nothing on flash
blob_db_update(list_id, empty pair list)      ← one atomic write: binds the root
```

The container exists from the moment the `update` commits; the client
persists `list_id` (by building the list at id = 1 itself — skipping the
alloc — or by recording `list_id` in the id = 1 registry). Crash before the
bind: an allocated id and nothing on flash — no residue at all. Crash after:
an empty, fully valid container.

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

## 4. Extension: the intent blob

For reference-graph mutations the structure grows a fourth i-node:

```
intent blob  {} or { new[], del[] }           own id: intent_id ← mutation journal
list blob    { intent_id, [(kid1,vid1), …] }  own id: list_id   ← root now records intent_id
```

The intent blob is created once, together with the container; its id is
stored in the root and never changes (id stability — the journal is only ever
`update`d). It is empty whenever no mutation is in flight.

While a mutation is in flight the intent **names every id it touches**:

- `new[]` — every id the mutation will bind. Each is reserved with
  `blob_db_alloc_id()` (spec §4) *before* STAGE, so the list is complete
  before anything is written. These die if the mutation never commits.
- `del[]` — every id the commit will unlink. These die once it has.

This rests on two properties of `alloc_id` (spec §2): it is a RAM operation,
so reserving ids up front costs nothing on flash; and an allocated id is never
issued again, across any crash. The second is load-bearing: an id in `new[]`
that was reserved but never written is owned by this mutation alone, so
recovery may `delete` it (it answers `-ENOENT`) without any risk of hitting a
blob some other client bound later.

The cost is size: `new[]` and `del[]` must fit one payload — 4 bytes of
counts plus 8 per id, about 31 ids at 256 bytes. The model needs at most 2 +
2. A mutation that only learns partway through how many blobs it needs may
extend `new[]` with one more intent write before each further batch; it must
never bind an id the intent does not yet name.

### 4.1 Crash-residue requirements (P7 applied)

| Tier | Requirement | Provided by |
|---|---|---|
| **must** | A partially written, power-interrupted operation is never data | L1: a torn write is detected and discarded (spec §2). Model: a reference that does not resolve is treated as *absent*, never surfaced as data |
| **must** | Visible state flips atomically old → new | one commit per mutation: the single `update` of §3.4, or step COMMIT of §5 |
| **must** | No permanent leak — residue reclaimed | intent record naming every id + recovery (§6) |
| **must** | Recovery reclaims only its own residue | recovery deletes only ids the intent names (§6) — never another client's blob |
| advisable | Self-healing of impossible residue | dangling pair → entry dropped (§7) |
| advisable | Avoidance: ordering minimizes residue windows | prepare-before-commit, cleanup-after-commit (§5) |

### 4.2 Why named ids, not a watermark (v3)

Up to v3 the intent held a watermark instead: `W = alloc_id()` as the
mutation's first allocation, `{W, del[]}` staged, and rollback deleted every
bound id ≥ W the list did not reference. That record is constant-size, but
rollback can only find "ids ≥ W" by walking the whole store with
`blob_db_iterate` — a sweep on the recovery path, which §9 reserves for
maintenance. Worse, "unreferenced ids ≥ W are mine" holds only if nothing
else allocates between the crash and this container's `open`. When stores
are opened lazily, another client can bind blobs in that gap; they lie above
W, this list does not reference them, and rollback deleted them
(`test_rollback_spares_other_store` reproduces it). Naming the ids removes
both the scan and the assumption.

## 5. The extended mutation discipline

Every reference-graph mutation follows one shape; each step is an
individually atomic `blob_db` call:

```
step 0  N × alloc_id()                    (RAM only — reserve every new id)
step 1  update(intent_id, {new[], del[]}) STAGE   declare the mutation durable:
                                                  "new[] dies on rollback;
                                                   del[] dies on commit"
step 2  N × update(new id)                PREPARE bind the new blobs,
                                                  unreferenced so far
step 3  one update(list_id, new image)    COMMIT  the linearization point
step 4  M × delete(del[])                 CLEANUP superseded blobs removed
step 5  update(intent_id, {})             CLEAR   mutation sealed
```

- Only steps 2 and 4 can produce unreferenced i-nodes, and both lie inside
  the staged window, on ids the intent names — recovery (§6) reclaims them.
  The commit itself can never leak: it atomically swaps *which* set of
  i-nodes is referenced.
- A reader, or a remount at any crash point, sees the complete old or the
  complete new mapping — never a torn list, never a dangling reference.
- A single-`update` mutation (§3.4) is this shape with steps 0–2 and 4–5
  empty: the commit alone. No stage, no residue possible. §3.2's first insert
  is this shape with steps 1 and 4–5 missing — which is exactly why it
  leaked.

Cost: two intent writes per reference-graph mutation. That is the price of
zero leak; real containers may amortize it (§9) but the model pays it plainly.

**Failure without power loss (abort path).** If a step fails cleanly — e.g.
`-EIO` during PREPARE — the mutation aborts by running recovery (§6) against
what is on flash. Before the commit that deletes `new[]` and the error is
returned; after it, the mutation is finished (`del[]` deleted) and reported
as success, because the new state is already what readers see. A crash
*during* the abort is indistinguishable from a crash during the mutation
itself, so abort needs no new machinery. If the abort's own recovery fails,
the intent stays staged; so every mutation, and destroy, starts by running
recovery (one read when nothing is in flight) — a STAGE must never overwrite
the only record of another mutation's ids. Checks that need no write (a full
list, an oversized key) run before STAGE, so they cost nothing to undo.

## 6. Recovery — bounded and idempotent

On `open(list_id)` (and thus on every remount), read the intent blob. If it is
empty, there is nothing to do — the normal case costs one read. Otherwise a
mutation was interrupted, and `{new[], del[]}` decides:

```
committed? :=  if new[] is non-empty:  the list references any id in new[]
               else (pure delete):     the list references no id in del[]

if committed:   ROLL FORWARD   delete every id in del[]   (-ENOENT tolerated)
else:           ROLL BACK      delete every id in new[]   (-ENOENT tolerated:
                                                           reserved, never bound)
finally:        update(intent_id, {})                      (CLEAR)
```

Properties:

- **Bounded**: recovery reads the list and the intent, and touches at most the
  ids the intent names. It never iterates the store.
- **Own residue only**: every id it deletes was reserved by this mutation, and
  ids are never reissued, so no other client's blob can be among them. Other
  stores may be opened and written in any order relative to this one.
- **Idempotent**: a crash during recovery re-enters the same branch — the
  commit state cannot change, deletes tolerate `-ENOENT`, and CLEAR is last.
- **Complete**: every crash point in §8 lands in exactly one branch; the
  tables show there is no window that leaks past recovery.
- **Serialized per container**: one mutation in flight per container, since
  it has one intent blob. Given by the v1 single-threaded contract. Nothing
  is assumed about other containers in the same database.

## 7. Self-healing (defense in depth)

Under the discipline of §5 a **dangling reference** — a pair whose `kid` or
`vid` no longer resolves — cannot occur. If one is observed anyway (software
defect, external interference), it is *impossible residue* and must not be
carried:

- lookup treats the pair as absent (`-ENOENT` from `get(kid)` ⇒ key not
  present) — the must-tier rule that unresolvable data is not data. Only
  `-ENOENT` counts: any other error from `get(kid)` (`-EIO`) leaves the
  state unknown and is returned, never read as absence — a mutation that
  took it as a miss would insert a second entry for a key that exists;
- the next mutation of the list (or an explicit repair pass) drops the entry
  as part of its own commit.

## 8. The extended operations, traced

### 8.1 Insert — `set("foo", "bar")`

The first insert of §3.2, completed: the same calls, with the ids reserved
first and wrapped in stage/clear so the leftovers of its crash table become
reclaimable.

```
0  kid = blob_db_alloc_id(); vid = blob_db_alloc_id()     (RAM only)
1  blob_db_update(intent_id, {new:[kid,vid]})             STAGE
2  blob_db_update(kid, "foo")                             PREPARE
3  blob_db_update(vid, "bar")                             PREPARE
4  blob_db_update(list_id, […, (kid,vid)])                COMMIT
5  blob_db_update(intent_id, {})                          CLEAR
```

| Crash after | Visible state on remount | Residue | Recovery |
|---|---|---|---|
| 1 | old list | none | not committed (list refs nothing in new[]): delete `kid`, `vid` → `-ENOENT`; clear |
| 2 | old list | `kid` | roll back: delete `kid`; `vid` → `-ENOENT`; clear |
| 3 | old list | `kid`, `vid` | roll back: delete both; clear |
| 4 | **new mapping** | staged intent | committed (list refs new[]): del[] empty; clear |
| 5 | new mapping | none | — |

### 8.2 Value replacement — when in-place `update` (§3.4) won't do

Applies when the value blob must genuinely be replaced: it is shared and an
in-place change would leak to other referrers, or the store follows the
immutable profile (spec decision D2).

```
0  vid2 = blob_db_alloc_id()                              (RAM only)
1  blob_db_update(intent_id, {new:[vid2], del:[vid]})     STAGE
2  blob_db_update(vid2, "baz")                            PREPARE
3  blob_db_update(list_id, […(kid,vid2)…])                COMMIT
4  blob_db_delete(vid)                                    CLEANUP
5  blob_db_update(intent_id, {})                          CLEAR
```

| Crash after | Visible state | Residue | Recovery |
|---|---|---|---|
| 1 | old mapping | none | not committed (`vid2` unreferenced): delete `vid2` → `-ENOENT`; clear |
| 2 | old mapping | `vid2` | roll back: delete `vid2`; clear |
| 3 | **new mapping** | old `vid` | committed (list refs `vid2`): delete `vid`; clear |
| 4 | new mapping | staged intent | committed: delete `vid` → `-ENOENT`, fine; clear |
| 5 | new mapping | none | — |

### 8.3 Delete — `del("foo")`

A delete creates nothing: step 0 is empty and `new[]` stays empty, so
recovery decides by `del[]` alone.

```
1  blob_db_update(intent_id, {new:[], del:[kid,vid]})     STAGE
2  blob_db_update(list_id, list without (kid,vid))        COMMIT
3  blob_db_delete(kid)                                    CLEANUP
4  blob_db_delete(vid)                                    CLEANUP
5  blob_db_update(intent_id, {})                          CLEAR
```

| Crash after | Visible state | Residue | Recovery |
|---|---|---|---|
| 1 | mapping present | none | not committed (del[] still referenced): nothing to delete; clear |
| 2 | **mapping gone** | `kid`, `vid` | committed (del[] unreferenced): delete both; clear |
| 3 | mapping gone | `vid` | committed: delete both (`kid` → `-ENOENT`); clear |
| 4 | mapping gone | staged intent | committed: deletes → `-ENOENT`; clear |
| 5 | mapping gone | none | — |

The intent is not the only way to make a delete leak-free: §9 shows a
variant that uses the list itself as the journal.

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
  staged window and a longer `new[]`/`del[]`.

A mutation that cannot be expressed as *one atomic update* (§3) or as
*stage → prepare → one commit → cleanup → clear* (§5) — e.g. one needing two
commits — is outside the contract and requires its own crash analysis against
P7. A mutation that writes several existing i-nodes is not necessarily such a
case: if all but one of the writes restore derived state, they become repair
steps after the single commit (`l1_model_container_list.md` §9).

A coarser alternative to the intent record is a **global sweep** (mark
reachable from id = 1, `blob_db_iterate` everything, delete the difference).
It needs no per-mutation writes, but it must see the *whole* database (a
single container cannot sweep safely next to others) and costs a full scan
whenever it runs. The intent record is the composable, normative mechanism; a
sweep is a last resort — a maintenance tool for residue that should be
impossible, run on purpose and never on the mount path.

A delete has a cheaper alternative that needs no intent: **the list as its
own journal**. Delete the pair *before* unlinking it — `delete(kid)`, then
`delete(vid)`, then commit the list without the entry. The first delete is
the linearization point: from then on the key reads as absent. A crash leaves
an entry whose `kid` answers `-ENOENT`, and the next scan that passes it
(every miss in a lookup does) finishes the job: delete `vid` (`-ENOENT`
tolerated), then drop the entry in that mutation's own commit. The residue
stays referenced from the list, so nothing leaks permanently, and ids are
never reused, so a dangling id cannot alias another client's blob. It holds
only if `kid` is always deleted first, only `-ENOENT` counts as deleted (any
other read error must not trigger the heal, or a corrupt `kid` would destroy
a valid `vid`), and the heal runs only while no intent is staged. The price:
a dangling reference becomes a normal state instead of impossible residue
(§7), so it no longer flags a defect, and reclamation waits until a scan next
reaches the entry instead of happening at mount. It also covers deletes only
— an insert's new blobs are referenced from nowhere until its commit, so they
still need an intent to name them. The model therefore keeps the intent for
every reference-graph mutation, to show one mechanism throughout.
