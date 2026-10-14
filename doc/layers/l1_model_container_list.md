# L1 — Model Container List: Two References, One Commit

Status: v1 · Second model container, after `l1_model_container.md` (the map)
· Built on `doc/layers/l1_blob_db.md` (the contract)
· Governed by `doc/principles.md`

This document is a companion to the model container map. It reuses that
document's vocabulary (i-node, intent, STAGE/PREPARE/COMMIT/CLEANUP/CLEAR,
roll forward/back) and adds one thing the map never needs: a mutation that
must **change two existing i-nodes**, of which only one write can be the
commit.

---

## 1. Purpose

The **model container list** (`model_container_list`, implemented in
`tests/lib/blob_db_contract/src/model_container_list.c`) is a doubly linked
list built on nothing but the L1 contract. Like the map it is a teaching
structure: unoptimized (one i-node per node and per value, no stored count,
O(n) walks), complete (no permanent leak, P7), and registered by its client
under its own root-registry key, `('MC_L', 0)`.

The map's root references every i-node, so every reference-graph mutation is
one `update` of that root. In a doubly linked list every node is referenced
**twice** — by its predecessor's `next` and its successor's `prev`. Linking
or unlinking a node therefore changes two i-nodes that already exist, and
`blob_db` makes each `update` atomic on its own, never two together.

Copy-on-write does not help: replacing a neighbour with a fresh id would
require its other neighbour to be rewritten too, and so on to both ends.
The only way left keeps one commit, and that is what this model shows:

> **When a mutation must write two existing i-nodes, make one of the two
> references derived.** The write to the authoritative reference is the
> commit. The write to the derived one is a *repair*: it runs after the
> commit, the intent names it, and recovery re-applies it when rolling
> forward.

It also shows node ids used as **client-held handles**. Ids are stable and
never reissued (spec §2), so a node id is a durable cursor the client can
pass back to insert next to it, remove it, or rewrite its value.

## 2. The structure

```
root blob    { intent_id, head, tail }        own id: root_id   ← container root
node blob    { prev, next, vid }              own id: nid       ← one per element
value blob   "…"                              own id: vid       ← one per element
intent blob  {} or { new[], del[],            own id: intent_id
                     commit: fwd(anchor) := link,
                     repair: back(succ)  := back }
```

`0` means "past the end". Two link functions treat the root as the node
before the first and after the last:

| | id = 0 (the root) | id = a node |
|---|---|---|
| `fwd(id)` — **authoritative** | `root.head` | `node.next` |
| `back(id)` — **derived** | `root.tail` | `node.prev` |

The forward chain *is* the list. The backward chain mirrors it: `back(x)` is
the one id whose `fwd` is `x`. Nothing would be lost if every back link were
erased; they could all be rebuilt from `root.head` in one walk. Which
direction is authoritative is arbitrary; picking one is what matters.

Invariant after every completed mutation and after every recovery:
`fwd(a) = b ⇔ back(b) = a`. Between a commit and its repair exactly one back
link is stale, and the intent names it.

## 3. The basic flow: rewriting a value

`mcl_set(nid, v)` does not change which ids are referenced, so it collapses
to the single atomic `update(vid, v)` of the map's §3.4. No intent, no crash
window, nothing to recover.

## 4. The mutation discipline, with a repair step

```
step 0  N × alloc_id()                         (RAM only — reserve every new id)
step 1  update(intent_id, {new[], del[],       STAGE    name the new ids, the
                commit, repair})                        doomed ids and both
                                                        link writes
step 2  N × update(new id)                     PREPARE  bind the new blobs; a
                                                        new node already points
                                                        at both neighbours
step 3  fwd(anchor) := link                    COMMIT   the linearization point
step 4  back(succ)  := back                    REPAIR   the derived link catches up
step 5  M × delete(del[])                      CLEANUP  unlinked blobs removed
step 6  update(intent_id, {})                  CLEAR    mutation sealed
```

Compared with the map's §5 there is one more step, REPAIR, and it sits
between COMMIT and CLEANUP on purpose: while the back link is stale it still
names a node that exists (on remove, the node being unlinked), so no link
ever dangles, not even for the length of one step.

Each write to a link is a read-modify-write of one i-node (the root or a
node) and therefore one atomic `update`.

## 5. Recovery

On `mcl_open()` read the root and the intent. An empty intent means nothing
is in flight (two reads). Otherwise one test decides, for inserts and
removes alike:

```
committed? :=  fwd(anchor) == link

if committed:   ROLL FORWARD   back(succ) := back            (redo the repair)
                               delete every id in del[]     (-ENOENT tolerated)
else:           ROLL BACK      delete every id in new[]     (-ENOENT tolerated)
finally:        update(intent_id, {})                       (CLEAR)
```

The test is exact because before the commit `fwd(anchor)` names something
the commit replaces: the old successor (insert) or the node being removed
(remove), never `link`. The map needs two predicates (*new[] referenced* /
*del[] unreferenced*); naming the commit write itself gives one.

Before the commit no back link has been touched, so roll back has nothing to
undo but `new[]`. After it, the repair writes a value that is already known,
so redoing it any number of times is harmless. The properties of the map's
§6 carry over unchanged: bounded (the anchor, the successor, the ids
named — never a store walk), own residue only, idempotent (CLEAR last),
serialized per container.

## 6. The operations, traced

### 6.1 Insert `n` between `p` and `s`

`mcl_insert_after(p)` takes `s = fwd(p)`; `mcl_insert_before(s)` takes
`p = back(s)`; `p = 0` is the front and `s = 0` the back.

```
0  nid = alloc_id(); vid = alloc_id()                                (RAM)
1  update(intent, {new:[nid,vid], fwd(p):=nid, back(s):=nid})        STAGE
2  update(vid, value); update(nid, {prev:p, next:s, vid})            PREPARE
3  fwd(p)  := nid                                                    COMMIT
4  back(s) := nid                                                    REPAIR
5  (an insert retires nothing)                                       CLEANUP
6  update(intent, {})                                                CLEAR
```

| Crash after | Forward | Backward | Residue | Recovery |
|---|---|---|---|---|
| 1 | old | old | none | `fwd(p) = s`: roll back, delete `nid`, `vid` → `-ENOENT`; clear |
| 2 | old | old | `nid`, `vid` | roll back: delete both; clear |
| 3 | **new** | stale: `back(s) = p` | staged intent | `fwd(p) = nid`: roll forward, `back(s) := nid`; clear |
| 4, 5 | new | new | staged intent | roll forward: repair again (same value); clear |
| 6 | new | new | none | — |

### 6.2 Remove `x`, between `p` and `s`

```
1  update(intent, {del:[x,vid], fwd(p):=s, back(s):=p})              STAGE
2  (a remove binds nothing)                                          PREPARE
3  fwd(p)  := s                                                      COMMIT
4  back(s) := p                                                      REPAIR
5  delete(x); delete(vid)                                            CLEANUP
6  update(intent, {})                                                CLEAR
```

| Crash after | Forward | Backward | Residue | Recovery |
|---|---|---|---|---|
| 1, 2 | old | old | none | `fwd(p) = x`: roll back, `new[]` empty; clear |
| 3 | **new** | stale: `back(s) = x`, `x` still bound | `x`, `vid` | `fwd(p) = s`: roll forward, `back(s) := p`, delete both; clear |
| 4 | new | new | `x`, `vid` | roll forward: repair again, delete both; clear |
| 5 | new | new | staged intent | roll forward: repair again, deletes → `-ENOENT`; clear |
| 6 | new | new | none | — |

### 6.3 The abort path

A step that fails without a power cut aborts through the same recovery, as
in the map: before COMMIT the mutation is rolled back and the error
returned; after it the mutation is finished (repair included) and reported
as success. If that recovery fails too the intent stays staged, and every
operation that leans on a back link (every mutation, `mcl_prev`,
`mcl_destroy`) runs recovery first.

## 7. Reading the two chains

- **Forward** (`mcl_next`, `mcl_get`) is right at every instant: before the
  commit the new node is unreachable, after it the node is in. Forward reads
  never run recovery and never write.
- **Backward** (`mcl_prev`) can be stale between COMMIT and REPAIR. On a
  remount recovery closes that window before anything is read; without one
  (a failed abort left the intent staged), `mcl_prev` runs recovery first.
  One root and one intent read when nothing is in flight.

The double link also makes membership O(1): a node `x` is linked into this
list iff `fwd(back(x)) = x`. Every write checks it, so a stale handle (a
removed node), another structure's blob, or the root itself is refused
with `-ENOENT` before anything is staged. (A node of a *second* list in the
same store would pass; the model, like the map, has one instance open at a
time.)

## 8. Destroy

`mcl_destroy()` runs recovery, then deletes every node and value from the
tail backwards along the back links, then the intent, then the root. Like
the map's destroy it is not crash-atomic, but it is re-runnable: deleting
from the tail leaves a **prefix** of the forward chain linked from
`root.head`. On a rerun `root.tail` no longer resolves, so destroy walks the
forward chain to the last node that does and continues from there.

## 9. What it tells real containers

The map's §9 puts a mutation that needs two commits outside the contract.
This model narrows that boundary:

- A mutation that writes **several existing i-nodes** still has one commit
  if exactly one of those writes changes authoritative state and every
  other write restores **redundant** state derivable from it (back links,
  cached counts, a tail pointer, parent pointers in a tree).
- The derived writes run after the commit as REPAIR steps; the intent names
  each one with the value it writes, so roll forward is bounded and
  idempotent. The intent grows by one id pair per repair.
- Readers of the derived state either run after recovery or check for an
  in-flight intent first.
- If two of the writes are each authoritative, the mutation does need two
  commits, and it stays outside the discipline.

Costs, measured in `update`s per mutation: an insert is 6 (map insert: 5),
a remove 4 plus 2 deletes. The extra write is the repair, the price of being
able to walk backwards.
