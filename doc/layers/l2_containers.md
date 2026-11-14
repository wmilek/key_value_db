# L2 — Container Layer

Status: draft (pre-implementation)
· Part of the stack in `doc/architecture.md` · Governed by `doc/principles.md`
· Builds on L1: `doc/layers/l1_blob_db.md`
· Provider implementations (non-normative): `doc/impl/l2_kvhash.md`

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

1. **Stage** — record the mutation in the container's intent blob: the ids
   of every new object, reserved with `alloc_id` beforehand (`new[]`), and
   the delete set (`del[]`);
2. **Prepare** — N × bind `update` of every new object, still unreferenced;
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

One container departs from that last sentence on purpose: `seq` (§4.1) writes
an element's id into its slot *before* binding the id, and deletes an element
*before* clearing its slot, so for `seq` a slot whose id does not resolve is
the ordinary encoding of "empty", not an anomaly. This is still P7 — a
reference that does not resolve is absent — and it is what lets every `seq`
mutation keep one commit with no intent blob.

Every mutation a container defines must be reducible to this pattern; one that
cannot (e.g. needing two commits) is outside the contract and requires its own
crash analysis against P7.

### 2.3 Handles and RAM

`open(root_id)` performs no scan — it reads at most the root i-node to validate
magic/type. Handles (`seq_t`, `map_t`) hold the root id plus O(1) cursor state
(P3). All node I/O uses ≤ 4 KB transient stack buffers, same budget as L1.

Each container type tags its root node with a 4-byte magic, so `open` fails fast
with `-EINVAL` when pointed at a root of the wrong type.

### 2.4 Destroying a container

A container is destroyed by whoever holds its root id — a root-registry entry
(`l1_root_registry.md`), or a field in another container's node.

> A holder keeps its record of the root id until `destroy` returns 0, and
> removes it only afterwards.

That record is the only thing that can name the structure after a crash: remove
it first and crash mid-destroy, and the container's i-nodes are stranded on
flash with nothing left to name them.

Returning 0 is the only completion — an error, or a reboot with no answer, means
call it again. Repeating is always safe: a part-way destroyed container resumes
and returns 0; one never built, or already gone, returns `-ENOENT`. Once destroy
has begun, reads answer `-ENOENT` for every key, so a caller never observes a
half-destroyed container.

The full sequence:

    1. holder records its decision to drop the entry   (its own single-update commit)
    2. ops->destroy(root)                              repeat until it returns 0
    3. holder removes the entry

Step 1 is what makes step 2 happen again after a crash: without it the holder
rediscovers an ordinary entry naming an ordinary root, and never retries.

## 3. The two abstract shapes

L3 binds against a *shape*, never a concrete container (P6). Two shapes cover
the planned interfaces:

**Sequence** — a sparse array, index-addressed (`seq_ops`):
`create · destroy · stat · len · get(index) · set(index) · del(index) · append · truncate · next`

**Map** — key-addressed (`map_ops`):
`create · destroy · open · get(key) · set(key) · del(key) · has(key) · iterate`

> Implemented so far (`include/app/lib/containers/shape_map.h`): the point
> subset `create(root, map_config) · get · set · del · destroy`, plus
> `map_config` for create-time hints (e.g. hash bucket count), plus `next` for
> enumeration and `count` for the number of keys (computed by a walk, never
> stored — meant for mount and recovery). `open` collapses into "resolve root, then call these"; `has` is
> an L3 convenience over `get`.
>
> `next` takes the place of a callback `iterate` in the Map shape. It is
> stateless: the caller passes the last key it saw and gets the one after it,
> in a provider-defined order fixed by the keys alone. So there is no iterator
> to hold, a walk resumes from a saved key after a reboot, and mutating between
> calls is allowed (the callback rule below does not apply to it).
> `shape_map.h` holds the full contract.
>
> `destroy` is the one op an L3 caller cannot assemble for itself: only the
> provider knows which i-nodes its root reaches, so releasing them has to be
> a container operation or the "L3 binds to a shape, never a container"
> rule (P6) breaks the first time an interface wants to drop an instance.
> Destroying a container is not a §2.2 mutation of it but its removal, so its
> contract is §2.4: the holder keeps its record of the root until `destroy`
> returns 0, and repeats the call after a crash.
>
> Note the asymmetry with `create`: `get`/`set`/`del`/`destroy` all read the
> root before touching it, so they answer `-ENOENT` on a destroyed root,
> while `create` writes directly and inherits D3's "update on a deleted id is
> undefined". A replacement map therefore goes at a fresh `alloc_id`, never
> back on the old root.

> Specified, not yet implemented (`include/app/lib/containers/shape_seq.h`):
> the Sequence shape mirrors the Map shape's conventions — `create(root,
> seq_config)` with a binding `max_elems`, `stat`, `get` with the same
> `-ENOMEM` probe, `destroy` under §2.4 — over a **sparse array of slots**
> instead of keys. A slot is empty or holds one element, an opaque byte string
> the container owns: callers pass bytes, never blob ids, so no caller ever
> holds a blob that nothing references yet. Any slot below the capacity may be
> set; `del` empties one; `len` is one past the highest slot set since the
> last `truncate` (stored, so O(1) — unlike Map `count`); `append` is `set` at
> `len`. `next(from)` is the Map `next` with the index as the cursor: the first
> occupied slot at or after `from`, in ascending order, mutation between calls
> allowed.
>
> What earns the shape a place beside L1's large payloads: an L1 object is
> **one** value addressed by byte offset (`blob_db_read`/`blob_db_write`); a
> sequence is **many** values addressed by index, each replaced on its own, at
> its own size. Use cases are listed in §4.1.

Keys and values are opaque byte strings (`ptr + len`); key semantics (strings,
paths, hashes) belong to L3. Concrete containers export a `const struct
map_ops`/`seq_ops` instance; the L3 backend `choice` (P4) decides which instance
an interface is compiled against — a compile-time binding, no runtime dispatch
cost beyond one indirection.

Neither shape has a callback `iterate`: both enumerate with a stateless
`next` whose cursor is the last key (Map) or index (Sequence) seen, so a walk
holds no iterator, survives a reboot, and may be interleaved with mutations.
The L1 callback rules (contract §4, decision D5 — mutating from inside the
callback is undefined) apply to L1's own `iterate` only. Enumeration is for
listing, export and repair, not a hot data path.

## 4. The containers

| Container | Shape | Lookup | Ordered | Kconfig |
|---|---|---|---|---|
| §4.1 `seq` — sparse indexed array | Sequence | O(1), ≤ 3 reads | by index | `CONTAINER_SEQ` |
| §4.2 `kvlist` — simple k→v list | Map | O(n) | insertion | `CONTAINER_KVLIST` |
| §4.3 `kvhash` — k→v hash | Map | O(1) avg | no | `CONTAINER_KVHASH` |
| §4.4 `kvtree` — k→v tree | Map | O(log n) | by key | `CONTAINER_KVTREE` |
| §4.5 `logring` — bounded log | *(own API)* | append/scan | insertion | `CONTAINER_LOGRING` |

The normative parts of each container are its shape, cost class, the §2.2
discipline, and the §5 invariants. The node layouts sketched below are
implementation designs — they show feasibility and may be fine-tuned when
each container gets its own implementation document under `doc/impl/`.

### 4.1 `seq` — sparse indexed array

A fixed-capacity array of slots; each occupied slot names one element blob.
One or two levels of id arrays, like the `kvhash` directory (§4.3), and like
it the geometry is derived at create from what the caller declares
(`seq_config.max_elems`) and fixed thereafter.

```
root  { magic 'CSEQ', version, depth, flags, capacity, len,
        trim_from, trim_to, child_id[] }
  depth 1: child_id[i] is slot i — an element id, or 0
  depth 2: child_id[i] is a leaf, or 0 (no leaf yet: all its slots empty)
leaf  { slot_id[] }                                   one i-node, created lazily
element = the caller's bytes, verbatim                 one i-node per element
```

A slot is **occupied** when its id resolves (`blob_db_get` succeeds) and
**empty** otherwise: id 0, an id reserved but never bound, or a deleted id all
read as empty (§2.2). Slots at or past `len` read as empty whatever they hold.

**Capacity.** With a ~24 B root header and 8 B per id, one level addresses
about 29 slots at the 256 B default payload and about 249 at 2016 B (the
`BLOB_DB_LARGE_PAYLOADS` default); two levels about 29 × 32 ≈ 900 and
249 × 252 ≈ 62 000. A `max_elems` past two levels is `-EINVAL` at create. An
element is its own blob, so its size limit is blob_db's — with large payloads
on, an element can itself be segmented.

**Cost** (depth 2; depth 1 saves the leaf read and write):

| op | reads | writes |
|---|---|---|
| `get`, `len`, `stat` | root + leaf + element / root / root | 0 |
| `set`, slot occupied | root + leaf + element | **1** — the element, updated in place, id kept |
| `set`, empty slot below `len` | root + leaf + probe | 2 — leaf, element |
| `set`/`append` at or past `len` | root + leaf + probe | 3 — leaf, element, root |
| `del` | root + leaf + probe | 2 — element tombstone, leaf |
| `truncate` | root + leaves in range | root, then one tombstone per element and one write per leaf, then root |
| `next` | leaves in range + one probe per non-zero slot (+ element if asked) | 0 |

Replacing an element is the common case for slot tables and costs one write
with no stage: the element blob belongs to the sequence alone, so it is
rewritten in place (`l1_model_container.md` §3.4) and nothing that names it
changes.

**Mutation order.** Every mutation has one commit and needs no intent blob,
because blob_db reserves ids without writing (`alloc_id` → bind) and reads an
unbound or deleted id as `-ENOENT`:

| op | order | commit | residue of a crash |
|---|---|---|---|
| `set`, slot occupied | update the element | that update | none |
| `set`, empty slot below `len` | `alloc_id`; write the id arrays on the path holding the new id, bottom-up; bind the element | the bind | a slot naming an unbound id — reads empty, nothing on flash |
| `set`/`append` at or past `len` | as above, then write the root with the new `len` | the root write | a bound element just past `len` — reachable from its id array, reused by the next `set`/`append` there, released by `truncate`/`destroy` |
| `del` | delete the element; clear the slot | the delete | a slot naming a deleted id — reads empty |
| `truncate`, shrinking | write the root with the new `len` and `trim_from/trim_to`; delete each element in the range, then clear it from its leaf; delete leaves wholly past `len`; write the root clearing the range and their entries | the first root write | elements in the trim range, still reachable; finished by the next mutation |
| `truncate`, growing | write the root with the new `len` | that write | none |

Two rules make the table hold:

- **`set` binds only an id it has just reserved, or one that resolves.** A
  slot whose id answers `-ENOENT` gets a fresh `alloc_id`, because the
  sequence cannot tell an id reserved but never bound from a deleted one, and
  binding a deleted id is undefined (L1 D3).
- **A pending trim is finished before any other mutation.** The trim range is
  inline in the root, as `logring` keeps its eviction range (§4.5), so an
  interrupted `truncate` is completed by whichever mutation comes next, and
  is bounded by the range.

At depth 1 the root is also the id array, so a fill at or past `len` writes it
twice — once naming the reserved id, once with the new `len`. Folding those
into one is an implementation question for `doc/impl/` when `seq` is built.

**Destroy** (§2.4) writes the root with a destroying flag — every op answers
`-ENOENT` from then on — deletes each leaf's elements and then the leaf, and
deletes the root last; `-ENOENT` from any delete counts as done, so a repeat
resumes where the last attempt stopped.

**What it is for** — values addressed by a dense integer, each large enough or
changed often enough to deserve its own blob:

- **numbered slot tables**: users, keys and certificates, biometric templates,
  presets, schedule and holiday entries — fixed capacity, holes, a slot
  replaced on its own. In a Map they would pay for stored keys, hashing, and
  a bucket that rewrites its neighbours; here a lookup is two reads to the
  element's id, and `next` lists the occupants or finds a free slot;
- **arrays of container roots** past what one superblock payload holds —
  per-period logs, per-entity sub-maps — with the root id stored as an
  8-byte element (the sequence does not destroy what it names);
- **record streams read back by position that must not drop data** —
  store-and-forward batches, sample blocks indexed by time — where `logring`
  would evict and an L1 object would cap the total and be one value.

**What it is not for:** one large value (an L1 payload, with
`blob_db_read`/`blob_db_write`); an array small enough to live inside its
owner's record; a bounded log (`logring`, §4.5); lookup by an arbitrary key
(a Map); an ordered list that needs inserts and removals in the middle.

> **History.** Until 2026-11 this section described a byte-stream `seq` — a
> chain of chunk nodes behind `len/read/append/truncate` on bytes — meant for
> file bodies. L1 decision D4 (amended; proposal D10) moved large values into
> L1 itself, with O(1) seek and pread/pwrite, and blobfs bodies are L1 blobs,
> so that role is gone. The shape was redefined around what L1 cannot do:
> many separately replaced values under one root.

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

### 4.3 `kvhash` — k→v hash  *(implemented)*

Root holds a fixed array of child ids; each bucket is a single blob holding a
packed pair list. A map is one or two levels deep, and the root says which.

```
root   { magic 'KVHA', n, version, depth, child_id[n] }   geometry fixed at create
  depth 1: child_id[i] is a bucket
  depth 2: child_id[i] is a sub-directory, itself a depth-1 root
bucket = ( u16 klen|KS, u16 vlen|VS, key part, val part )*  0 = empty, created lazily
         KS/VS set: that part is a blob id (key: + u32 fingerprint) — see l2_kvhash.md §1
```

A directory must fit one i-node payload, so one level addresses at most
`(payload - 8) / 8` buckets — 31 at the 256 B default. A population past that
is built as a directory of sub-directories instead: two independent indices cut
from one hash, `n` × `n` buckets, still a fixed geometry with no online resize.
The caller does not choose depth; it declares a population in `map_config` and
`create` derives the geometry, returning `-EINVAL` if it cannot honour the
declaration (`doc/impl/l2_kvhash.md` §1.2).

Lookup: hash key (CRC-32) → read root → (depth 2: read one sub-directory) →
scan one bucket blob; with n/nbuckets small this is O(1) average, 2 flash reads
one level deep and 3 at two. Set/del rewrite one bucket blob and, only when a
bucket is first created, its immediate parent directory — one or two atomic
updates, parent last as the commit point. A second level therefore costs one
extra read per operation and buys metadata that scales as √N rather than N.

> **v1 implementation note** (full design: `doc/impl/l2_kvhash.md`). The
> shipped bucket is a *self-contained packed pair-list blob*, not a `kvlist`
> chain — so kvhash does **not** select kvlist,
> and a bucket that outgrows one payload returns `-ENOSPC` (rather than
> overflow-chaining). Promoting buckets to full kvlist chains (§4.2), and
> online resize, are future revisions; the on-flash `version` byte reserves
> room to evolve the layout.

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

### 4.5 `logring` — bounded per-entry log

Where `seq` (§4.1) is a *fixed-capacity, index-addressed* array that never
drops an element on its own — the right shape for slot tables and for records
read back by position — `logring` is purpose-built for
**logging**: a *bounded* store that keeps the most recent records and drops the
oldest, scaling from a couple of MB up to near the whole partition. It does not
implement the generic `seq_ops`; it exports its own append/scan API
(`logring.h`). **Implemented**, not scaffolded.

```
root  { magic 'CLOG', version, epoch, capacity_bytes, live_bytes, count,
        waypoint_id, head_id, evict_from, evict_to }
entry { next_id, len, bytes… }                       one i-node PER record
```

Each log record is **its own i-node** (R2). Entries form a forward chain using
`blob_db`'s reserve-then-bind: `next_id` points at an id `alloc_id()` returned
but not yet bound, so an `append` just *binds* that pre-reserved id — one new
blob written, **nothing rewritten** (~1× write amplification, versus the
rewrite-amplification of packing records into a shared node). A small RAM
**handle** (`l2_containers.md` §2.3) caches the tail so warm appends are O(1)
and the root is written only at **checkpoints** (~every `CHECKPOINT` appends),
which also carry a **waypoint** near the end that `open` rebuilds the exact
tail from, and which **batch eviction** of the oldest entries. Eviction's
intent lives **inline** in that same root write and its run is deleted
suffix-first for re-enterable recovery — so `logring` needs no shared intent
helper and is self-contained on L1. The soft `capacity_bytes` budget is a hint
(§5-invariant-friendly: the ring sits *near* it), with a hard `-ENOSPC`
backstop.

Reads are **forward only**, through an opaque cursor in the journal/`lseek`
idiom: `seek_oldest` / `seek_newest` (O(1), reaches the real end) / `next`. The
cursor exports a public **moniker `{epoch, id}`** the caller serializes and
integrity-protects itself; the per-log random `epoch` lets `next` reject a
moniker from another incarnation (e.g. after a reformat, which reuses ids) or a
random value — `-ESTALE`, not a misread. An evicted position is likewise
`-ESTALE`.

Cost: `append` = 1 write, 0 reads (warm); `open` = 1 read + a bounded walk;
`next` = 1–2 reads. Every op is O(1) in the log's size. Design detail:
`doc/impl/l2_logring.md`; rationale: `doc/proposals/2026-10-08-logring.md`.

## 5. Container invariants

Every implementation must uphold (checklist for reviews and tests):

1. **Single-integer reachability** — recoverable from the root id alone (P5).
2. **Typed root** — magic-tagged; wrong-type `open` fails cleanly.
3. **One commit point per mutation** — §2.2; no observable intermediate state (P7).
4. **O(1) steady-state RAM** — no per-element RAM, no caches (P3).
5. **Bounded stack** — ≤ 4 KB transient, plus O(depth) ids for `kvtree`;
   stack buffers preferred over heap (P2).
6. **Resumable destruction** — the root outlives every node it owns, so an
   interrupted `destroy` re-derives its work from the root; the operation is
   idempotent and may be repeated after a crash (§2.4).

## 6. Kconfig

```
menuconfig BLOB_CONTAINERS         bool "Container layer"      depends on BLOB_DB

config CONTAINER_SEQ               bool "Sparse indexed array (sequence)"
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
the same scenarios against every enabled `map_ops` provider, so backends stay
behaviorally interchangeable (`seq` is the only `seq_ops` provider, so items
1–7 run in its own suite until a second one exists):

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
10. `seq`: sparse fill in any order; empty slot (`-ENOENT`) vs empty element
    (length 0); `len` raised by `set` past it, kept by `del`, moved both ways
    by `truncate`; `append` at capacity → `-EFBIG`; index at or past capacity
    → `-EINVAL`; `next` returns occupied slots once each in ascending order,
    with and without element buffers, across `set`/`del` between calls; depth
    1 and depth 2 at their capacity boundaries; crash injection at every step
    of every row of the §4.1 mutation-order table, verifying old-or-new state,
    that a slot naming an unbound or deleted id reads empty and is never
    rebound, that an interrupted `truncate` is finished by the next mutation,
    and that `destroy` leaves no element, leaf or root behind.
