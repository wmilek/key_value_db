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
> reachable from it. One `uint64_t` — id = 1, or a root-registry entry hanging
> off it — bootstraps the whole structure after reboot. No side-band state,
> no mount-time rebuild.

Concurrency: containers inherit L1's v1 contract — single-threaded, caller
serializes, at most one mutation in flight per database.

## 2. Common mechanics

### 2.1 Nodes are i-nodes

Every container node (list element, hash bucket, tree node) is its own `blob_db`
blob with its own stable id. Consequences, inherited from L1's contract:

- Rewriting a node (`blob_db_update`) keeps its id — parents pointing at it stay
  valid without being touched.
- Deleting a node tombstones an id that is never reused — a stale pointer can be
  *detected* (`-ENOENT`), never *aliased* to unrelated data.
- Each node mutation is individually crash-atomic.

### 2.2 Mutation protocol: stage / prepare / commit / cleanup / clear

The protocol every container mutation must follow is defined — with full call
traces, per-crash-point residue tables, and the recovery procedure — by the
**model container** (`doc/layers/l1_model_container.md`). Summary:

1. **Stage** — record the mutation in the container's intent blob
   (id watermark `W` + delete set);
2. **Prepare** — N × (`alloc_id` + bind `update`) of every new object, still unreferenced;
3. **Commit** — exactly **one** `update` of the i-node that makes them
   reachable: the single linearization point;
4. **Cleanup** — M × `delete` of the superseded objects;
5. **Clear** — empty the intent blob.

Only prepare and cleanup can generate unreferenced i-nodes; the commit never
can. A crash leaves the complete old or complete new state, and any residue
lies inside the staged window: `open` runs the model container's recovery
(roll forward or back, then clear) — bounded, idempotent, **no permanent
leak** (P7). A mutation that fits one node (inline value, single-node change)
is the commit alone — no stage, no possible residue. Dangling references are
impossible under this ordering; if one is ever observed, it is treated as
absent and repaired, never returned as data.

Every mutation a container defines must be reducible to this pattern; one that
cannot (e.g. needing two commits) is outside the contract and requires its own
crash analysis against P7.

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

Both shapes' `iterate` follow the L1 iterator rules (contract §4, decision D5): Zephyr
convention (callback returns 0 = continue, non-zero = stop, value propagated
to the caller), and **mutating the container from inside the callback is
undefined behavior** — collect-then-mutate is the supported pattern.
Iteration is a diagnostic/repair facility, not a data path.

## 4. The containers

| Container | Shape | Lookup | Ordered | Kconfig |
|---|---|---|---|---|
| §4.1 `seq` — simple list | Sequence | O(n) | insertion | `CONTAINER_SEQ` |
| §4.2 `kvlist` — simple k→v list | Map | O(n) | insertion | `CONTAINER_KVLIST` |
| §4.3 `kvhash` — k→v hash | Map | O(1) avg | no | `CONTAINER_KVHASH` |
| §4.4 `kvtree` — k→v tree | Map | O(log n) | by key | `CONTAINER_KVTREE` |
| §4.5 `logring` — bounded log | *(own API)* | append/scan | insertion | `CONTAINER_LOGRING` |

The normative parts of each container are its shape, cost class, the §2.2
discipline, and the §5 invariants. The node layouts sketched below are
implementation designs — they show feasibility and may be fine-tuned when
each container gets its own implementation document under `doc/impl/`.

### 4.1 `seq` — simple list

Singly-linked chain of chunk nodes; the root holds head/tail ids and a count.

```
root  { magic 'CSEQ', head_id, tail_id, count }
chunk { next_id, nelems, elem[]… }          each elem: { len, bytes… }
```

Chunking (several elements per i-node, up to the payload limit) keeps read
amplification low; a chunk splits when an insert doesn't fit. `append` is O(1)
(tail id known from the root); `get(index)` is O(n/chunk_factor). Intended use:
queues, file-data chunk chains — not random access at scale. For *bounded*
logging with automatic eviction of old records, use `logring` (§4.5) instead
of an ever-growing `seq`.

### 4.2 `kvlist` — simple k→v list

The ground representation is full indirection — this container *is* the model
container of `l1_model_container.md`, promoted to a build-able module: the
root i-node holds only **id pairs**; keys and values are each their own i-node.

```
root { magic 'CKVL', count, (key_id, val_id)[count] }
```

Every mutation rewrites the pair list and commits it with a **single `update`
of the root**, with key/value alloc-and-bind `update`s before it and `delete`s after it as
prepare/cleanup — exactly the traces in the model-container document §8 (and
its §3.4 in-place fast path whenever the value blob is exclusively owned).

Two optimizations of the ground case, protocol unchanged:

- **Inlining.** Keys/values up to `CONFIG_CONTAINER_KVLIST_INLINE_MAX` are
  stored in the root's pair entry instead of their own i-node — fewer flash
  reads per lookup, fewer prepare/cleanup steps (and leak windows) per
  mutation.
- **Overflow chaining.** When the pair list outgrows one i-node payload, the
  node ends with a `next_id` to an overflow node of the same layout; a
  mutation rewrites only the affected node of the chain.

O(n) everything; smallest code of the Map providers. Intended for ≲ tens of
keys, and as `kvhash`'s per-bucket representation.

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

Mutations rewrite the root→leaf path copy-on-write per §2.2 — depth+1 alloc-and-binds,
one root `update` as the commit, then dead-path `delete`s. At fan-out 8, 100k
keys ⇒ depth ≈ 6 ⇒ ~6 flash reads per lookup. Splits/merges follow standard
B-tree rules, still committed by the single root write.

### 4.5 `logring` — bounded circular log

Where `seq` (§4.1) is an *unbounded, index-addressed* sequence — the right
shape for file bodies and chunk chains — `logring` is its opposite trade,
purpose-built for **logging**: a *bounded* store that keeps the most recent
records and silently drops the oldest. It is the embedded "flight recorder" —
boot reasons, fault codes, the last lines before a crash — and so it does not
implement the generic `seq_ops`; it exports its own small append/scan API
(`logring.h`). **Implemented**, not scaffolded.

```
root { magic 'CLOG', version, count, used, next_seq, rec[]… }
rec  { seq (u64), len (u16), bytes… }         packed, oldest → newest
```

The entire ring lives inline in the **one** root i-node, exactly like
`rootreg` (`l1_root_registry.md`): every mutation — `append`, `reset`,
`create` — is a single atomic `blob_db_update(root, …)`, the sole
linearization point (§2.2). There is no prepare/cleanup phase, hence **no
residue and no recovery pass** — `open` only validates the magic. `append`
evicts whole records from the head until the newcomer fits, so it returns
`-ENOSPC` only when a *single* record exceeds the ring's capacity
(`BLOB_DB_MAX_PAYLOAD_LEN` − header), never merely because the ring is full.

Each record carries a strictly-increasing, never-reused `uint64_t` **sequence
number** — durable across eviction, `reset`, and remount (it lives in
`next_seq`, written atomically with the ring). A consumer that remembers the
last sequence number it processed can therefore detect exactly how many
records were dropped while it was away (oldest surviving seq − last seen − 1):
the same single-integer "where was I" idea L1 applies to ids (P5), applied to
the log cursor. Capacity is one i-node payload; for larger retention, run
several rings (e.g. per subsystem) side by side — each is one `uint64_t` root.

Cost: `append` = 1 read + 1 write, O(payload) transient RAM; `iterate` walks
the inline run oldest→newest. Design detail: `doc/impl/l2_logring.md`.

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
config CONTAINER_LOGRING           bool "Bounded circular log"
```

`logring` needs no capacity tunable of its own: its ring is one i-node payload,
so `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` alone bounds it (a `BUILD_ASSERT` keeps the
20-byte header + a one-byte record within the payload).

Each container is its own translation unit under `lib/containers/<name>/`,
included via `add_subdirectory_ifdef` — a disabled container costs zero flash
and RAM (P4). Enabling several at once is legal; different L3 interfaces (or
different roots) may use different containers side by side.

Tunable compatibility is an **implementation detail**: every container node
must fit one L1 payload (`BLOB_DB_MAX_PAYLOAD_LEN`), so each container's
implementation enforces its sizing rule with a `BUILD_ASSERT` (e.g. hash
root: header + 8 B × buckets; tree node: fan-out × max entry size) and its
defaults must form a valid combination — specified per container in its
implementation design under `doc/impl/`.

## 7. Repository layout

```
lib/containers/
  CMakeLists.txt   Kconfig
  seq/  kvlist/  kvhash/  kvtree/        each { <name>.c, Kconfig, CMakeLists.txt }
  logring/                              logring.c (implemented)
include/app/lib/containers/
  seq_ops.h  map_ops.h  seq.h  kvlist.h  kvhash.h  kvtree.h  logring.h
tests/lib/containers/
  seq/  kvlist/  kvhash/  kvtree/        ztest per container
  logring/                              ztest (implemented)
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
6. crash injection: kill between every step of §2.2, remount + `open`, verify
   the visible state is complete-old or complete-new AND recovery reclaimed
   all residue (no unreferenced i-nodes survive, intent blob cleared)
7. capacity: fill to `-ENOSPC`, verify structure still consistent
8. `kvhash`: distribution sanity; collision chains. `kvtree`: split/merge at
   fan-out boundaries; range scans.
9. `logring`: append/iterate order; oldest-first eviction past capacity;
   sequence numbers monotonic, contiguous, and durable across eviction /
   `reset` / remount; over-capacity record rejected with `-ENOSPC`;
   wrong-type `open` → `-ENOTSUP`. (`logring` uses its own append/scan API,
   not `seq_ops`/`map_ops`, so it is exercised by its own suite rather than
   the shared shape-conformance suite.)
