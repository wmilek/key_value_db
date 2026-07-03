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

### 2.2 Mutation protocol: COW with a single commit point

A mutation touching several nodes must not be observable half-done (P7). The
protocol, for any container:

1. Write **new** versions of changed nodes as fresh i-nodes (`put`) — these are
   unreachable garbage until committed.
2. Commit with **one** `blob_db_update` of the highest unchanged ancestor
   (often the root). That single atomic write is the linearization point.
3. Delete the superseded nodes (`delete`). A crash between 2 and 3 leaks
   garbage i-nodes but never corrupts the structure.

Where a mutation is confined to one node (e.g. appending a key to one hash
bucket), step 2 collapses into a single in-place `update` — the common fast path.

**Garbage after a crash:** i-nodes written in step 1 or not yet deleted in
step 3 are unreachable but occupy space. v1 policy: accept the leak (bounded by
one mutation's node count per crash); an optional mark-and-sweep walking from
the root via `blob_db_iterate` can be added later behind its own Kconfig symbol.

### 2.3 Handles and RAM

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

The archetype from `l1_blob_db.md` Appendix A.1, promoted to a first-class
module. A linked list of entry nodes:

```
root  { magic 'CKVL', head_id, count }
entry { next_id, klen, vlen, key…, val… }   val inline; or val_id when large
```

Values larger than `CONFIG_CONTAINER_KVLIST_INLINE_MAX` are stored as their own
i-node and referenced by id — the same indirection pattern all containers reuse.
O(n) everything; smallest code of the Map providers. Intended for ≲ tens of
keys, and as `kvhash`'s per-bucket representation.

### 4.3 `kvhash` — k→v hash

Root holds a fixed array of bucket ids; each bucket is a `kvlist` chain
(therefore `CONTAINER_KVHASH` **selects** `CONTAINER_KVLIST`).

```
root   { magic 'CKVH', nbuckets, bucket_id[nbuckets] }     nbuckets fixed at create
bucket = kvlist entry chain (0 = empty bucket, created lazily)
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
