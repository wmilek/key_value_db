# L2 — Container Layer

Status: draft (pre-implementation)
· Part of the stack in `doc/architecture.md` · Governed by `doc/principles.md`
· Builds on L1: `doc/layers/l1_blob_db.md`

---

## 1. Role

L2 turns the flat pool of i-nodes provided by `blob_db` into **data structures**:
lists, maps, trees. `blob_db` deliberately knows nothing about keys, order, or
navigation; every pointer between pieces of data is an **i-node id stored inside
another i-node's payload**, and L2 is the layer that writes and follows those
pointers.

The defining model (principle P5):

> A container **is** a root i-node id plus a rule for interpreting the i-nodes
> reachable from it. One `uint64_t` — canonically id = 1 — bootstraps the whole
> structure after reboot. No side-band state, no mount-time rebuild.

## 2. Common mechanics

### 2.1 Nodes are i-nodes

Every container node (list element, hash bucket, tree node) is its own `blob_db`
blob with its own stable id. Consequences, inherited from L1's contract:

- Rewriting a node (`blob_db_update`) keeps its id — parents pointing at it stay
  valid without being touched.
- Deleting a node tombstones an id that is never reused — a stale pointer can be
  *detected* (`-ENOENT`), never *aliased* to unrelated data.
- Each node mutation is individually crash-atomic.

### 2.2 Mutation protocol: prepare / commit / cleanup

A mutation touching several i-nodes must not be observable half-done (P7).
Every container mutation follows the same three-phase shape, built from plain
`blob_db` calls (each individually atomic per L1 §2):

| Phase | blob_db calls | Crash here leaves |
|---|---|---|
| **1 Prepare** | N × `put` — write every *new* object (values, nodes) | the prepared i-nodes, **unreferenced** — invisible to readers |
| **2 Commit** | exactly **one** `update` of the node that makes the new objects reachable (list blob, parent, root) | *before* the write lands: same as phase 1; *after*: mutation fully visible. Never a third state. |
| **3 Cleanup** | M × `delete` of the objects the commit superseded | the superseded i-nodes, now unreferenced |

The rules that follow from this, and that every container must obey:

- **Only phase 1 and phase 3 can generate unreferenced objects.** The commit
  itself cannot: L1 guarantees the single `update` either lands completely or
  not at all, and it atomically swaps *which* set of i-nodes is referenced.
- **A reader (or a remount after crash) always sees either the complete old
  state or the complete new state** — never a torn structure, never a
  reference to an i-node that doesn't exist.
- **The leak per crash is bounded** by one mutation's object count: N prepared
  + M not-yet-cleaned i-nodes. They waste space but are harmless — nothing
  reachable points at them.
- When the mutation fits one node (inline value, single-node change), phases
  1 and 3 are empty and the whole mutation is one `update`: zero possible
  garbage.

v1 policy on the leaked space: accept it. An optional mark-and-sweep
(walk from id = 1, `blob_db_iterate` the rest, delete the difference) can be
added later behind its own Kconfig symbol.

### 2.3 Worked example: `set("foo", "bar")` on a simple k→v list

The kvlist root is one i-node holding the whole pair array
`[(k1, v1), (k2, v2), …]` (§4.2), with large values indirected to their own
i-nodes. Inserting a new key whose value is indirected:

```
step 1  blob_db_put("bar") → vid          PREPARE   value blob on flash,
                                                    referenced by nothing yet
step 2  blob_db_update(list_id,           COMMIT    the array image is replaced
          [(k1,v1), (k2,v2), ("foo",vid)])          atomically (L1 §2)
```

| Crash after | State on remount | Garbage |
|---|---|---|
| step 1 | list unchanged; `get("foo")` → not found | 1 i-node (`vid`) |
| step 2 | `get("foo")` → "bar"; mutation complete | none |

Overwriting an existing indirected key (`"foo"` → `"baz"`) adds cleanup:

```
step 1  blob_db_put("baz") → vid2                  PREPARE
step 2  blob_db_update(list_id, […("foo",vid2)…])  COMMIT   old vid unreferenced from here
step 3  blob_db_delete(vid)                        CLEANUP
```

| Crash after | State on remount | Garbage |
|---|---|---|
| step 1 | old mapping intact | `vid2` |
| step 2 | new mapping intact | old `vid` (cleanup pending) |
| step 3 | new mapping intact | none |

With the value inline in the array (the small-value fast path), the entire
mutation is step 2 alone. Deleting a key is the mirror image: commit first
(`update` the array without the pair), then cleanup (`delete` the value
i-node).

### 2.4 Handles and RAM

`open(root_id)` performs no scan — it reads at most the root i-node to validate
magic/type. Handles (`seq_t`, `map_t`) hold the root id plus O(1) cursor state
(P3). All node I/O uses ≤ 4 KB transient stack buffers, same budget as L1.

Each container type tags its root node with a 4-byte magic, so `open` fails fast
with `-EINVAL` when pointed at a root of the wrong type.

## 3. The two abstract shapes

L3 binds against a *shape*, never a concrete container (P6). Two shapes cover
the planned interfaces:

**Sequence** — ordered, index-addressed (`seq_ops`):
`create · open · append · get(index) · set(index) · remove(index) · len · iterate`

**Map** — key-addressed (`map_ops`):
`create · open · get(key) · set(key) · del(key) · has(key) · iterate`

Keys and values are opaque byte strings (`ptr + len`); key semantics (strings,
paths, hashes) belong to L3. Concrete containers export a `const struct
map_ops`/`seq_ops` instance; the L3 backend `choice` (P4) decides which instance
an interface is compiled against — a compile-time binding, no runtime dispatch
cost beyond one indirection.

## 4. The four containers

| Container | Shape | Lookup | Ordered | Kconfig |
|---|---|---|---|---|
| §4.1 `seq` — simple list | Sequence | O(n) | insertion | `CONTAINER_SEQ` |
| §4.2 `kvlist` — simple k→v list | Map | O(n) | insertion | `CONTAINER_KVLIST` |
| §4.3 `kvhash` — k→v hash | Map | O(1) avg | no | `CONTAINER_KVHASH` |
| §4.4 `kvtree` — k→v tree | Map | O(log n) | by key | `CONTAINER_KVTREE` |

### 4.1 `seq` — simple list

Singly-linked chain of chunk nodes; the root holds head/tail ids and a count.

```
root  { magic 'CSEQ', head_id, tail_id, count }
chunk { next_id, nelems, elem[]… }          each elem: { len, bytes… }
```

Chunking (several elements per i-node, up to the payload limit) keeps read
amplification low; a chunk splits when an insert doesn't fit. `append` is O(1)
(tail id known from the root); `get(index)` is O(n/chunk_factor). Intended use:
logs, queues, file-data chunk chains — not random access at scale.

### 4.2 `kvlist` — simple k→v list

The whole pair array lives in **one i-node**:

```
root { magic 'CKVL', count, pair[count]… }
pair { klen, vlen_or_ref, key…, (val… | val_id) }
```

Values up to `CONFIG_CONTAINER_KVLIST_INLINE_MAX` are inline; larger ones get
their own i-node, referenced by `val_id` — the indirection pattern all
containers reuse. Every mutation rewrites the array image and commits it with
a **single `update` of the root** (§2.2; worked through step by step in §2.3),
with value-i-node `put`/`delete` around it as prepare/cleanup.

If the array outgrows one i-node payload, the node ends with a `next_id`
chaining to an overflow node of the same layout; a mutation then rewrites only
the affected node of the chain. O(n) everything; smallest code of the Map
providers. Intended for ≲ tens of keys, and as `kvhash`'s per-bucket
representation.

### 4.3 `kvhash` — k→v hash

Root holds a fixed array of bucket ids; each bucket is a `kvlist` chain
(therefore `CONTAINER_KVHASH` **selects** `CONTAINER_KVLIST`).

```
root   { magic 'CKVH', nbuckets, bucket_id[nbuckets] }     nbuckets fixed at create
bucket = kvlist pair-array node (§4.2; 0 = empty bucket, created lazily)
```

`nbuckets` defaults to `CONFIG_CONTAINER_KVHASH_BUCKETS` (default 64; root must
fit one i-node payload) and is frozen into the root at `create` — no online
resize in v1. Lookup: hash key (FNV-1a) → read root → walk one bucket chain;
with n/nbuckets small this is O(1) average, ~2–3 flash reads. Set/del mutate one
bucket entry node and, only when a bucket head changes, the root — one or two
atomic updates, root last as the commit point.

### 4.4 `kvtree` — k→v tree

B-tree with Kconfig fan-out (`CONFIG_CONTAINER_KVTREE_FANOUT`, default 8). Keys
sorted, so it is the only Map provider offering **ordered iteration and range
scans** — `map_ops.iterate` visits keys in sort order, and the module exports an
extension op for `iterate_range(from,to)`.

```
root { magic 'CKVT', height, top_id, count }
node { nkeys, child_id[…], (klen,key,val_id|val)[…] }      one i-node per node
```

Mutations rewrite the root→leaf path copy-on-write per §2.2 — depth+1 `put`s,
one root `update` as the commit, then dead-path `delete`s. At fan-out 8, 100k
keys ⇒ depth ≈ 6 ⇒ ~6 flash reads per lookup. Splits/merges follow standard
B-tree rules, still committed by the single root write.

## 5. Container invariants

Every implementation must uphold (checklist for reviews and tests):

1. **Single-integer reachability** — recoverable from the root id alone (P5).
2. **Typed root** — magic-tagged; wrong-type `open` fails cleanly.
3. **One commit point per mutation** — §2.2; no observable intermediate state (P7).
4. **O(1) steady-state RAM** — no per-element RAM, no caches (P3).
5. **Bounded stack** — ≤ 4 KB transient, plus O(depth) ids for `kvtree`;
   stack buffers preferred over heap (P2).

## 6. Kconfig

```
menuconfig BLOB_CONTAINERS         bool "Container layer"      depends on BLOB_DB

config CONTAINER_SEQ               bool "Simple list (sequence)"
config CONTAINER_KVLIST            bool "Simple key→value list"
config CONTAINER_KVLIST_INLINE_MAX int  "Inline value limit"    default 64
config CONTAINER_KVHASH            bool "Key→value hash"        select CONTAINER_KVLIST
config CONTAINER_KVHASH_BUCKETS    int  "Default bucket count"  default 64
config CONTAINER_KVTREE            bool "Key→value tree"
config CONTAINER_KVTREE_FANOUT     int  "B-tree fan-out"        default 8
```

Each container is its own translation unit under `lib/containers/<name>/`,
included via `add_subdirectory_ifdef` — a disabled container costs zero flash
and RAM (P4). Enabling several at once is legal; different L3 interfaces (or
different roots) may use different containers side by side.

## 7. Repository layout

```
lib/containers/
  CMakeLists.txt   Kconfig
  seq/  kvlist/  kvhash/  kvtree/        each { <name>.c, Kconfig, CMakeLists.txt }
include/app/lib/containers/
  seq_ops.h  map_ops.h  seq.h  kvlist.h  kvhash.h  kvtree.h
tests/lib/containers/
  seq/  kvlist/  kvhash/  kvtree/        ztest per container
```

## 8. Testing strategy (per container)

Beyond per-container functional cases, a **shared shape-conformance suite** runs
the same scenarios against every enabled `map_ops` provider (and `seq_ops`
provider), so backends stay behaviorally interchangeable:

1. create → open by root id → empty behavior
2. set/get/del round-trips, including binary keys/values and 0-length values
3. overwrite keeps external references valid (root id unchanged)
4. iteration completeness; sort order for `kvtree`
5. persistence across `blob_db` unmount/mount (single-integer recovery)
6. crash injection: kill between COW steps (§2.2), remount, verify old state
   intact and garbage bounded
7. capacity: fill to `-ENOSPC`, verify structure still consistent
8. `kvhash`: distribution sanity; collision chains. `kvtree`: split/merge at
   fan-out boundaries; range scans.
