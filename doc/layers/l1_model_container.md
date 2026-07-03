# L1 — Model Container: the `blob_db` Client Contract by Example

Status: v1 · Companion to `doc/layers/l1_blob_db.md` (the blob_db spec)
· Governed by `doc/principles.md`

---

## 1. Purpose

This document defines the **model container**: the smallest structure that
exercises every element of the `blob_db` client contract — stable ids as
persistent references, single-operation atomicity, and the
prepare/commit/cleanup discipline for multi-i-node mutations.

It is deliberately **not a usable container** (O(n) flash reads per lookup, one
i-node per key and per value). Its role is normative, not practical:

- it **defines the contract**: what a correct client may assume of `blob_db`
  and what obligations it carries in return;
- it is the **reference for every real container** (`l2_containers.md`) —
  each of them is a composition of exactly these step patterns, and any
  container mutation must be reducible to the phases shown here;
- it is the natural shape of the **L1 acceptance test**: a ztest suite
  implementing this container verbatim, with crash injection between every
  step, validates the contract independent of any real L2 code.

## 2. The structure

A key→value mapping built from three kinds of i-node, fully indirected —
every piece of data is its own i-node, every reference is an explicit id:

```
key blob     "foo"                            own id: kid
value blob   "bar"                            own id: vid
list blob    [(kid1,vid1), (kid2,vid2), …]    own id: list_id   ← the container
```

The client persists nothing but `list_id` (in practice: id = 1, or a field of
the id = 1 root — principle P5). Everything else is reachable from it.

## 3. The mutation discipline: prepare / commit / cleanup

Every mutation is built from plain `blob_db` calls, each individually atomic
(spec §2 / §8). The ordering discipline is the client's side of the contract:

| Phase | Calls | Rule |
|---|---|---|
| **1 Prepare** | N × `put` — every *new* object | new i-nodes are written first, while still unreferenced |
| **2 Commit** | exactly **one** `update` of the i-node that makes them reachable | the single linearization point |
| **3 Cleanup** | M × `delete` of superseded objects | old i-nodes are deleted only after nothing references them |

What follows from it:

- **Only prepare and cleanup can generate unreferenced i-nodes.** The commit
  cannot: `blob_db` guarantees the one `update` lands completely or not at
  all, and that write atomically swaps *which* set of i-nodes is referenced.
- **A reader, or a remount after a crash, sees the complete old state or the
  complete new state** — never a torn structure, never a reachable id that
  does not resolve.
- **Garbage is bounded**: at most one interrupted mutation's prepared plus
  not-yet-cleaned i-nodes. Unreferenced i-nodes are invisible (nothing
  reachable points at them) and cost only space.
- A reference, once committed, stays valid until its target is deleted —
  guaranteed by id stability and no-reuse (spec §2). A stale id can only ever
  produce `-ENOENT`, never someone else's data.

## 4. The four operations, traced

### 4.1 Insert — `set("foo", "bar")`

```
step 1  kid = blob_db_put("foo")                    PREPARE
step 2  vid = blob_db_put("bar")                    PREPARE
step 3  blob_db_update(list_id,                     COMMIT
          [(kid1,vid1), (kid2,vid2), (kid,vid)])
```

| Crash after | State on remount | Garbage (unreferenced i-nodes) |
|---|---|---|
| step 1 | list unchanged, `get("foo")` → not found | `kid` |
| step 2 | list unchanged, `get("foo")` → not found | `kid`, `vid` |
| step 3 | `get("foo")` → "bar", mutation complete | none |

### 4.2 Overwrite — `set("foo", "baz")`

The key blob is reused; only the value is replaced.

```
step 1  vid2 = blob_db_put("baz")                   PREPARE
step 2  blob_db_update(list_id, […(kid,vid2)…])     COMMIT    old vid unreferenced from here
step 3  blob_db_delete(vid)                         CLEANUP
```

| Crash after | State on remount | Garbage |
|---|---|---|
| step 1 | old mapping intact | `vid2` |
| step 2 | new mapping intact | old `vid` (cleanup pending) |
| step 3 | new mapping intact | none |

### 4.3 Delete — `del("foo")`

The mirror image: commit first, cleanup after.

```
step 1  blob_db_update(list_id, list without (kid,vid))   COMMIT
step 2  blob_db_delete(kid)                               CLEANUP
step 3  blob_db_delete(vid)                               CLEANUP
```

A crash after step 1 or 2 leaves `kid` and/or `vid` as garbage; the mapping is
already correctly gone.

### 4.4 Lookup — `get("foo")`

```
blob_db_get(list_id)              → the pair list
for each (kid_i, vid_i):
    blob_db_get(kid_i)            → compare with "foo"
blob_db_get(vid_i of the match)   → "bar"
```

O(n) flash reads — the honest price of full indirection. No mutation, no
crash windows.

## 5. What real containers change — and what they must not

Real containers (`l2_containers.md`) optimize the model in exactly two ways:

- **Inlining** small keys/values into the referencing i-node — fewer i-nodes,
  fewer prepare/cleanup steps (and leak windows), fewer reads;
- **Better shapes** for the reference graph — hash buckets, tree nodes —
  turning the O(n) scan into O(1)/O(log n).

Neither changes the discipline: every container mutation, however complex the
shape, must still reduce to *prepare (puts) → one commit (update) → cleanup
(deletes)*, and inherits the guarantees of §3 exactly as the model does. A
proposed container mutation that cannot be expressed this way (e.g. one that
needs two commits) is outside the contract and needs its own crash analysis.

## 6. Open point at this level

The only residue the discipline permits is **bounded garbage** from an
interrupted mutation. Policy v1: accept it. An optional collector — mark
reachable from id = 1, `blob_db_iterate` all ids, delete the difference — can
be added behind a Kconfig symbol later; it needs no new L1 mechanism.
