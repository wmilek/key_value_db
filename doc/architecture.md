# Architecture — Layered Storage Stack

Status: draft · Companion to `doc/design.md` (which details layer L1 in depth)

---

## 0. Purpose

`doc/design.md` specifies **one** module — `blob_db`, the stable-id blob store — in
full detail. This document zooms out and describes the **whole stack** that
`blob_db` sits in the middle of: what is below it, what is built on top of it, and
how each layer is independently selectable and configurable via Kconfig so that a
given firmware image ships only the layers and variants it actually needs.

The stack has four layers:

```
┌──────────────────────────────────────────────────────────────────────┐
│  L3  Access interfaces      kvdb · blobfs · settings-registry · …     │  choose interface(s)
│      (client-facing APIs)                                              │
├──────────────────────────────────────────────────────────────────────┤
│  L2  Containers             seq · kv-list · kv-hash · kv-tree         │  choose backing container(s)
│      (data structures over i-nodes)                                   │
├──────────────────────────────────────────────────────────────────────┤
│  L1  i-node allocation      blob_db  (stable u64 id → blob)           │  always present
│      (THIS repo's core; see design.md)                                │
├──────────────────────────────────────────────────────────────────────┤
│  L0  Flash translation      flash_area today · UBI-like FTL tomorrow  │  swappable provider
│      (erase-block management)                                         │
└──────────────────────────────────────────────────────────────────────┘
```

The load-bearing idea that ties the layers together:

> **Every persistent structure is reachable from a single integer.**
> An i-node id (a `uint64_t`) *is* a persistent pointer. A container is nothing
> more than "a root i-node id plus a rule for interpreting the blobs reachable
> from it". A client that remembers one integer — by convention **id = 1**, the
> root i-node — can re-open an arbitrarily large structure after reboot with no
> other stored state.

---

## 1. Layer L0 — Flash translation (the layer below us)

### 1.1 What it is

The bottom layer owns raw flash: erase blocks, wear, bad-block handling, and the
mapping from a linear byte-addressable partition onto physical erase blocks.
`blob_db` consumes it strictly through Zephyr's **`flash_area`** interface
(`<zephyr/storage/flash_map.h>`): `open/close/read/write/erase`, plus geometry
(`get_size`, `get_sectors`, `align`).

### 1.2 Today vs. tomorrow

| | Provider | Erase-block management | Wear leveling | Bad blocks |
|---|---|---|---|---|
| **Today (v1)** | Zephyr `flash_area` over a fixed partition | 1 sector = 1 erase block, static | none | none (NOR / native_sim) |
| **Tomorrow** | **UBI-like FTL** presenting a `flash_area`-shaped façade | logical→physical block map | yes | yes (NAND) |

The contract `blob_db` depends on is deliberately narrow — read/write/erase of
aligned sectors plus geometry. A UBI-style translation layer that exposes the
*same* `flash_area` API can be slotted underneath **without changing a single line
of L1 code** (see design.md §17). L0 is therefore a swap point, not a rewrite
point.

### 1.3 Contract L0 must honor (so L1 stays correct)

- A committed sector write is durable and readable verbatim after reboot.
- `erase` returns a sector to the all-`0xff` state.
- `align()` reports the write-block size; L1 rounds every slot to it.
- A torn write (power loss mid-write) may leave arbitrary bytes in the affected
  sector, but must not corrupt *other* sectors. L1's CRCs and double-buffered
  master absorb the torn sector; L0 must contain the blast radius to that sector.

Everything above L0 is flash-technology-agnostic.

---

## 2. Layer L1 — i-node allocation (`blob_db`, what we have now)

### 2.1 Role in the stack

L1 is the **allocator of persistent identity**. It turns a flat flash partition
into a pool of **i-nodes**: opaque, variable-length blobs each named by a stable
`uint64_t` id. It is fully specified in `doc/design.md`; this section only restates
the properties the layers above rely on.

> **Terminology.** design.md calls the unit a "blob" and its name an "id". At the
> architecture level we call the same thing an **i-node**: a stably-addressed,
> independently-rewritable, crash-atomic unit of storage. The two words are
> interchangeable — "i-node id" == "blob id".

### 2.2 The primitives L2 builds on

```c
int  blob_db_put   (const void *payload, size_t len, uint64_t *out_id); /* alloc i-node */
int  blob_db_get   (uint64_t id, void *out, size_t out_sz, size_t *out_len);
int  blob_db_update(uint64_t id, const void *payload, size_t len);      /* same id, new bytes */
int  blob_db_delete(uint64_t id);
bool blob_db_exists(uint64_t id);
int  blob_db_iterate(blob_db_iter_cb_t cb, void *user);
```

### 2.3 Guarantees L2 leans on (from design.md §2)

- **Stable ids** — an id names the same logical i-node for its whole life; `update`
  keeps the id, compaction never changes it.
- **No reuse** — a deleted id is never re-assigned; ids are monotonic.
- **Single-operation atomicity** — each put/update/delete either fully commits or
  is invisible after the next mount.
- **The root convention** — the first `put` after format returns **id = 1**. This
  is the anchor every container uses as its entry point.

### 2.4 Why containers, not "just use blob_db"

`blob_db` intentionally knows nothing about keys, order, or navigation
(design.md §1: *"blob_db is intentionally not a key-value store"*). That policy is
what keeps L1 tiny and crash-proof. The **structure** — how i-nodes point at each
other to form lists, maps, trees — is pushed **up** into L2, where it can be
enabled à la carte. L1 is the mechanism; L2 is the policy.

---

## 3. Layer L2 — Containers (built on the i-node allocator)

### 3.1 The unifying model: a container is a root id + an interpretation

Every container is bootstrapped from **one integer**. The client stores that
integer (canonically id = 1, the root i-node) and nothing else. On mount it hands
the integer to the container's `open`, and the container walks i-node → i-node from
there to reconstruct the full structure lazily, on demand — no in-RAM index is
rebuilt at open time.

```
        client keeps ──►  root_id (uint64_t)          ← a single integer, persisted as id=1
                              │
                    container open(root_id)
                              │
             ┌────────────────┴─────────────────┐
             ▼                                   ▼
        root i-node (blob)                 ...child i-nodes...
     e.g. { head_id, count, … }          each its own blob_db id
```

Each container node is its own i-node (its own `blob_db` id), so every structural
mutation is a `blob_db_put`/`update`/`delete` — i.e. it inherits L1's crash
atomicity for free. A container mutation that spans several i-nodes uses
**copy-on-write up the parent chain, committed by a single root `update`** as its
linearization point (the same trick the tree in design.md Appendix A.2 uses).

### 3.2 Two abstract container interfaces

L2 exposes exactly two shapes to L3, so that an L3 interface can bind to any
implementation of the shape it needs:

**Sequence** — ordered, index-addressed:

```c
struct seq_ops {
    int (*create)(uint64_t *root_id_out);
    int (*open)  (uint64_t root_id, seq_t *out);
    int (*append)(seq_t *s, const void *val, size_t len, size_t *index_out);
    int (*get)   (seq_t *s, size_t index, void *out, size_t out_sz, size_t *len_out);
    int (*set)   (seq_t *s, size_t index, const void *val, size_t len);
    int (*remove)(seq_t *s, size_t index);
    size_t (*len)(seq_t *s);
    int (*iterate)(seq_t *s, seq_iter_cb_t cb, void *user);
};
```

**Map** — key → value:

```c
struct map_ops {
    int (*create)(uint64_t *root_id_out);
    int (*open)  (uint64_t root_id, map_t *out);
    int (*get)   (map_t *m, const void *key, size_t klen,
                  void *out, size_t out_sz, size_t *len_out);
    int (*set)   (map_t *m, const void *key, size_t klen,
                  const void *val, size_t vlen);
    int (*del)   (map_t *m, const void *key, size_t klen);
    bool (*has)  (map_t *m, const void *key, size_t klen);
    int (*iterate)(map_t *m, map_iter_cb_t cb, void *user);
};
```

Both structs are populated at build time by whichever concrete container is
enabled. `seq_t` / `map_t` are small stack/handle structs holding the root id and
transient cursor state (still O(1) steady-state RAM, per the L1 constraint).

### 3.3 The four concrete containers

| Container | Impl module | Shape | Lookup | Ordered? | Node layout (per i-node) | Good for |
|---|---|---|---|---|---|---|
| **Simple list** | `seq` | Sequence | O(n) | insertion order | `{ next_id, val… }` singly-linked, or a chunked-array root | small ordered logs, queues, sets |
| **Simple k→v list** | `kvlist` | Map | O(n) | insertion order | `{ next_id, klen, key…, val_id }` | tiny namespaces (≲ tens of keys) |
| **k→v hash** | `kvhash` | Map | O(1) avg | no | root `{ nbuckets, bucket_id[] }`; each bucket a kvlist i-node | flat key spaces, many keys, point lookups |
| **k→v tree** | `kvtree` | Map | O(log n) | sorted by key | node `{ child_id[], key…, val_id }` (B-tree / radix) | ordered scans, range queries, prefixes |

Design notes:

- **Simple list (`seq`).** The linked form costs one i-node per element (one flash
  read per hop). A chunked variant packs many elements per i-node to cut read
  amplification; both satisfy `seq_ops`. Appendix A.1 of design.md is the
  archetype.
- **Simple k→v list (`kvlist`).** The linked list from design.md Appendix A.1,
  promoted to a first-class container. Values can be inlined in the node or stored
  as a separate `val_id` i-node when large.
- **k→v hash (`kvhash`).** Root i-node holds a fixed (or client-resized) array of
  bucket i-node ids; each bucket is itself a `kvlist`. Mirrors — one level up — the
  bucket structure L1 already uses internally (design.md Appendix A.3). Resize
  policy is the container's, not L1's.
- **k→v tree (`kvtree`).** Each tree node is an i-node holding child ids + keys.
  Mutations rewrite the root→leaf path copy-on-write and commit with a single root
  `update`, so a crash mid-mutation leaves the old tree fully intact
  (design.md Appendix A.2).

### 3.4 Independent enable/disable

Each container is its own translation unit under `lib/containers/<name>/`, guarded
by its own Kconfig symbol, and added to the build only via
`add_subdirectory_ifdef`. A container you don't enable contributes **zero** flash
and RAM. Enabling several at once is legal — different L3 interfaces (or different
instances) may pick different backends.

### 3.5 Invariant every container must uphold

1. **Single-integer reachability.** The entire structure is recoverable from its
   root i-node id and nothing else.
2. **Root-convention compatibility.** A container may *be* the id = 1 root, or hang
   off a field of the client's root blob. Either way, one integer bootstraps it.
3. **Crash atomicity via the root.** Every externally-visible mutation is finalized
   by exactly one `blob_db_put`/`update` (usually of the root or a parent),
   inheriting L1's all-or-nothing commit. No mutation is visible until that write
   lands.
4. **O(1) steady-state RAM.** Like L1, containers hold no per-element RAM between
   calls; they re-read i-nodes from flash on demand.

---

## 4. Layer L3 — Access interfaces (client-facing)

### 4.1 Role

L3 turns a container into an **ergonomic, purpose-built API** that the rest of the
firmware calls directly. L3 modules contain no on-flash format of their own — they
translate a domain vocabulary (keys, paths, records) into `map_ops` / `seq_ops`
calls. Which container backs a given interface is a **build-time choice**.

### 4.2 Interfaces

**`kvdb` — key/value database.** The headline interface: `string key → byte value`.
Thin veneer over a **Map** container.

```c
int kvdb_open (kvdb_t *db);                              /* attaches to id=1 root */
int kvdb_set  (kvdb_t *db, const char *key, const void *val, size_t len);
int kvdb_get  (kvdb_t *db, const char *key, void *out, size_t out_sz, size_t *len);
int kvdb_del  (kvdb_t *db, const char *key);
int kvdb_foreach(kvdb_t *db, kvdb_cb_t cb, void *user);
```

Backend selectable: `kvlist` (tiny, code-minimal), `kvhash` (fast point lookups),
or `kvtree` (ordered / range scans). The API is identical across backends; only
cost characteristics change.

**`blobfs` — filesystem-like interface.** Hierarchical `path → file`. A **directory**
is a Map container (`name → child i-node id`); a **file** is either an inline value
or a **Sequence** of data chunks for large/streamed content. Nesting directories
gives paths.

```c
int blobfs_mkdir (const char *path);
int blobfs_open  (const char *path, int flags, blobfs_file_t *out);
int blobfs_read  (blobfs_file_t *f, void *buf, size_t n, size_t *rd);
int blobfs_write (blobfs_file_t *f, const void *buf, size_t n);
int blobfs_unlink(const char *path);
int blobfs_readdir(const char *path, blobfs_dirent_cb_t cb, void *user);
```

The root directory is the id = 1 root i-node — one integer, whole tree.

**`settings` / registry (optional).** A flat, typed configuration store
(`"net/ip" → value`), naturally backed by `kvhash` or `kvtree`. Can also be wired
as a Zephyr `settings` backend so existing subsystems persist through this stack.

### 4.3 Interface ↔ container binding

L3 depends on a **shape** (Map or Sequence), never on a concrete container. The
concrete binding is resolved at build time:

```
kvdb      ──requires Map──►  { kvlist | kvhash | kvtree }
blobfs    ──requires Map (dirs) + Sequence (file data)──►  { … } + { seq }
settings  ──requires Map──►  { kvhash | kvtree }
```

This is what makes the stack composable: pick the interface for how clients *think*
about the data, pick the container for the cost profile you need, and the two are
wired together by Kconfig without either side hard-coding the other.

---

## 5. Configuration model (Kconfig)

The whole point of the layering is **build-time à-la-carte selection**. Nothing you
don't enable is compiled or linked.

### 5.1 Symbol map

```
# ── L1: always required base ───────────────────────────────
CONFIG_BLOB_DB                       (exists today; see lib/blob_db/Kconfig)

# ── L2: containers (menu; each independently toggled) ──────
CONFIG_BLOB_CONTAINERS               bool  "Container layer (L2)"  depends on BLOB_DB
  CONFIG_CONTAINER_SEQ               bool  "Simple list (sequence)"
  CONFIG_CONTAINER_KVLIST            bool  "Simple key→value list"
  CONFIG_CONTAINER_KVHASH            bool  "Key→value hash"          # selects KVLIST (bucket impl)
  CONFIG_CONTAINER_KVTREE            bool  "Key→value tree"
    CONFIG_CONTAINER_KVTREE_FANOUT   int   "B-tree fan-out"  default 8

# ── L3: access interfaces (each selects the shape it needs) ─
CONFIG_KVDB                          bool  "Key/value DB interface"  depends on BLOB_CONTAINERS
  choice  KVDB_BACKEND               "kvdb backing container"
    CONFIG_KVDB_BACKEND_KVLIST         select CONTAINER_KVLIST
    CONFIG_KVDB_BACKEND_KVHASH         select CONTAINER_KVHASH
    CONFIG_KVDB_BACKEND_KVTREE         select CONTAINER_KVTREE
  endchoice

CONFIG_BLOBFS                        bool  "Filesystem-like interface"  depends on BLOB_CONTAINERS
  choice  BLOBFS_DIR_BACKEND         "blobfs directory container"
    CONFIG_BLOBFS_DIR_KVHASH           select CONTAINER_KVHASH
    CONFIG_BLOBFS_DIR_KVTREE           select CONTAINER_KVTREE
  endchoice
  CONFIG_BLOBFS_FILE_CHUNKED         bool  "Store file bodies as a chunked sequence"  select CONTAINER_SEQ
```

The `select` edges enforce the dependency arrows from §4.3: choosing an L3 backend
automatically pulls in exactly the L2 container it needs, which depends on L1. A
minimal image (`CONFIG_KVDB_BACKEND_KVLIST=y`) links only `blob_db` + `kvlist` +
`kvdb`.

### 5.2 Dependency direction

```
L3 (kvdb/blobfs)  depends on & selects  →  L2 (seq/kvlist/kvhash/kvtree)
L2                depends on            →  L1 (blob_db)
L1                depends on            →  L0 (FLASH, FLASH_MAP  — via flash_area)
```

Dependencies point strictly downward; no layer references the layer above it.

### 5.3 Example configurations

| Use case | Enabled symbols | Result |
|---|---|---|
| Tiny settings store, <20 keys | `KVDB` + `KVDB_BACKEND_KVLIST` | `blob_db` + `kvlist` + `kvdb` only; smallest footprint |
| Fast device DB, 10k keys | `KVDB` + `KVDB_BACKEND_KVHASH` | adds `kvhash` (+`kvlist` buckets) |
| Ordered/range queries | `KVDB` + `KVDB_BACKEND_KVTREE` | adds `kvtree` |
| File-ish blob storage | `BLOBFS` + `BLOBFS_DIR_KVHASH` + `BLOBFS_FILE_CHUNKED` | `kvhash` dirs + `seq` file bodies |
| Everything (test image) | all of the above | all containers + both interfaces coexist |

---

## 6. Repository layout (proposed)

Mirrors the existing `lib/blob_db/` and `lib/custom/` patterns.

```
lib/
  CMakeLists.txt          add_subdirectory_ifdef(CONFIG_BLOB_DB blob_db)
                          add_subdirectory_ifdef(CONFIG_BLOB_CONTAINERS containers)
                          add_subdirectory_ifdef(CONFIG_KVDB    kvdb)
                          add_subdirectory_ifdef(CONFIG_BLOBFS  blobfs)
  Kconfig                 rsource "blob_db/Kconfig"
                          rsource "containers/Kconfig"
                          rsource "kvdb/Kconfig"
                          rsource "blobfs/Kconfig"

  blob_db/                                     ← L1 (exists)

  containers/                                  ← L2
    CMakeLists.txt        add_subdirectory_ifdef(CONFIG_CONTAINER_SEQ    seq)  …
    Kconfig
    seq/     { seq.c,     Kconfig, CMakeLists.txt }
    kvlist/  { kvlist.c,  Kconfig, CMakeLists.txt }
    kvhash/  { kvhash.c,  Kconfig, CMakeLists.txt }
    kvtree/  { kvtree.c,  Kconfig, CMakeLists.txt }

  kvdb/                                         ← L3
    { kvdb.c, Kconfig, CMakeLists.txt }
  blobfs/                                       ← L3
    { blobfs.c, Kconfig, CMakeLists.txt }

include/app/lib/
  blob_db.h               (exists)
  containers/ { seq.h, kvlist.h, kvhash.h, kvtree.h, map_ops.h, seq_ops.h }
  kvdb.h
  blobfs.h

tests/lib/
  blob_db/                (exists)
  containers/{seq,kvlist,kvhash,kvtree}/   ztest per container
  kvdb/                   ztest, parametrized over enabled backends
  blobfs/                 ztest
```

---

## 7. End-to-end example (walking the stack)

A single `kvdb_set("wifi.ssid", "home", 4)` with the `kvhash` backend:

```
kvdb_set("wifi.ssid","home")                                   ── L3
  └─ map_ops.set(m, "wifi.ssid", 9, "home", 4)                 ── L2 (kvhash)
        ├─ b = hash("wifi.ssid") % nbuckets   (root i-node holds bucket_id[])
        ├─ open bucket i-node b  ── blob_db_get(bucket_id[b])  ── L1 → flash_area ── L0
        ├─ upsert key in bucket kvlist (COW the bucket node)
        │     └─ blob_db_update(bucket_id[b], new_bucket_img)  ── L1  (atomic commit)
        └─ if bucket id changed, blob_db_update(root_id, …)    ── L1  (single linearization point)
```

Reboot recovery: the client stored only the integer `1`. `kvdb_open` →
`map_ops.open(1)` → `blob_db_get(1)` yields the hash root → the whole DB is
reachable again, no rebuild scan needed.

---

## 8. Design rationale summary

| Decision | Why |
|---|---|
| Keep L1 structure-free | Tiny, crash-proof allocator; all navigation policy lives above it and is optional. |
| One integer = one structure | Reboot recovery needs no journal replay or index rebuild; matches L1's root convention. |
| Two abstract shapes (Map/Seq) | L3 binds to a shape, not an impl — backends are swappable at build time. |
| Per-container Kconfig + `add_subdirectory_ifdef` | Unused containers cost zero flash/RAM — essential on embedded targets. |
| L3 `select`s its L2 backend | Choosing an interface auto-resolves the container + L1 deps; no invalid combos. |
| COW committed by one root write | Multi-i-node mutations inherit L1 single-op atomicity for free. |
| L0 behind `flash_area` | UBI/NAND FTL slots in later with no change above L0. |

---

## 9. Relationship to existing docs

- **`doc/design.md`** — the authoritative, implementation-level spec of **L1
  (`blob_db`)**. This document treats it as a black box with the contract in §2.
- **This document (`doc/architecture.md`)** — the stack-level view: L0 below, L2/L3
  above, and the Kconfig composition model.

Each L2 container and L3 interface should get its own design-level document
(`doc/containers/<name>.md`, `doc/kvdb.md`, `doc/blobfs.md`) once specified in the
same depth as design.md, describing its node format, mutation/COW protocol, and
crash-recovery argument.
