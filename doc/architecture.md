# Architecture — Layered Storage Stack

Status: v2 · Top-level document; per-layer detail lives in `doc/layers/`

---

## 1. Documentation map

| Document | Contents |
|---|---|
| **this file** | The stack: layers, boundaries, composition model |
| `doc/principles.md` | Binding design principles (P1–P8) for every layer |
| `doc/layers/l0_flash.md` | L0 — flash translation: `flash_area` contract, future UBI-like FTL |
| `doc/layers/l1_blob_db.md` | L1 — i-node allocation (`blob_db`): full implementation-level spec |
| `doc/layers/l1_model_container.md` | L1 — the model container: blob_db's client contract by example |
| `doc/layers/l1_root_registry.md` | L1½ — root registry: owner of id = 1, key → structure-root map |
| `doc/layers/l2_containers.md` | L2 — containers: seq, kvlist, kvhash, kvtree |
| `doc/layers/l3_interfaces.md` | L3 — access interfaces: kvdb, blobfs, settings |

L1 is implemented and tested; L0's interface is fixed (provider `a` in use);
L2/L3 are specified, pre-implementation.

## 2. The stack

```
┌──────────────────────────────────────────────────────────────────────┐
│  L3  Access interfaces      kvdb · blobfs · settings-registry        │  enable interface(s)
│      what the firmware calls: keys, paths, records                   │
├──────────────────────────────────────────────────────────────────────┤
│  L2  Containers             seq · kvlist · kvhash · kvtree           │  choose backing container(s)
│      data structures wired out of i-nodes                            │
├──────────────────────────────────────────────────────────────────────┤
│  L1  i-node allocation      blob_db: stable u64 id → blob            │  the always-present core
│      crash-atomic alloc_id/update/get/delete by id                        │
├──────────────────────────────────────────────────────────────────────┤
│  L0  Flash translation      flash_area today · UBI-like FTL later    │  swappable provider
│      erase blocks, alignment, (wear/bad blocks in FTL form)          │
└──────────────────────────────────────────────────────────────────────┘
```

Dependencies point strictly downward (P6); each boundary is a narrow contract:

```
L3 ──map_ops / seq_ops──► L2 ──blob_db API──► L1 ──flash_area API──► L0
```

## 3. The unifying idea

> **Every persistent structure is reachable from a single integer.**

An i-node id (`uint64_t`) is a persistent pointer: L1 guarantees it stays valid
for the blob's lifetime and is never reused. A container is nothing more than a
root i-node id plus a rule for interpreting the i-nodes reachable from it. The
first id ever assigned is **1** (L1's root convention), so a client — or the
whole L3 layer — that remembers only the integer 1 re-opens everything after
reboot: no journal replay, no index rebuild, no side-band state (P3, P5).

## 4. Layer summaries

**L0 — Flash translation.** Owns raw flash behind the fixed `flash_area`
interface. Today: a device-tree partition, 1 sector = 1 erase block, no
management (NOR/native_sim). Later: a UBI-like FTL adding wear leveling and
bad-block remapping behind the *same* interface — a pointer swap via
`CONFIG_BLOB_DB_PARTITION_LABEL`, zero change above. → `layers/l0_flash.md`

**L1 — i-node allocation (`blob_db`).** The always-present core: turns the
partition into a pool of stably-identified, crash-atomically updatable blobs
("i-nodes"). The public API is the contract; the allocator behind it
(v1: hash-bucketed append-log) is exchangeable — FAT-like or extent-based
variants can replace it at the cost of a reformat (`l1_blob_db.md` Appendix B).
→ `layers/l1_blob_db.md`

**L2 — Containers.** Data structures whose nodes are i-nodes and whose edges
are ids stored in payloads. Four providers behind two abstract shapes — **Map**
(`kvlist` O(n) · `kvhash` O(1) · `kvtree` O(log n), ordered) and **Sequence**
(`seq`). Mutations are copy-on-write with a single root `update` as the atomic
commit point. Each container independently enable-able. → `layers/l2_containers.md`

**L3 — Access interfaces.** Ergonomic APIs for the rest of the system: `kvdb`
(string key → value over any Map), `blobfs` (paths/files: Map directories +
Sequence file bodies), optional Zephyr `settings` backend. Interfaces bind to a
shape; Kconfig binds the shape to a container. → `layers/l3_interfaces.md`

## 5. Composition via Kconfig

Everything above L1 is à la carte (P4). Symbols live with their layers (see the
layer docs); the composition rule is:

- every module: own symbol + `add_subdirectory_ifdef` ⇒ disabled = zero cost;
- L3 backend `choice`s **select** the L2 container they need;
- L2 depends on `BLOB_DB`; `BLOB_DB` selects `FLASH`, `FLASH_MAP`, `CRC`.

So enabling an interface auto-resolves its whole downward slice, and invalid
combinations are unrepresentable.

| Use case | Enable | Image contains |
|---|---|---|
| Tiny settings store, <20 keys | `KVDB` + backend `KVLIST` | blob_db + kvlist + kvdb |
| Fast device DB, 10k keys | `KVDB` + backend `KVHASH` | + kvhash |
| Ordered / range queries | `KVDB` + backend `KVTREE` | kvtree instead of kvhash |
| File-like storage | `BLOBFS` + dir `KVHASH` + `FILE_CHUNKED` | + kvhash, seq, blobfs |
| Test image | everything | all containers + all interfaces |

## 6. End-to-end walk

`kvdb_set("wifi.ssid", "home", 4)` with the `kvhash` backend:

```
kvdb_set("wifi.ssid","home")                                  ── L3
  └─ map_ops.set(m, "wifi.ssid", 9, "home", 4)                ── L2 kvhash
       ├─ b = fnv1a("wifi.ssid") % nbuckets                    (root holds bucket_id[])
       ├─ blob_db_get(bucket_id[b])          read bucket node  ── L1 ── flash_area ── L0
       └─ blob_db_update(bucket_id[b], …)    upsert, COW node  ── L1  ← atomic commit
```

Reboot recovery: `kvdb_open` → `rootreg_get(ROOTREG_KEY('KVDB',0))` (one read
of id = 1) → hash root → done. The only persisted "pointer" anyone remembered
is the integer 1.

## 7. Repository layout (target)

```
lib/
  blob_db/            L1  (exists)
  rootreg/            L1½ root registry (owner of id = 1)
  containers/         L2  seq/ kvlist/ kvhash/ kvtree/  (+ shared intent/)
  kvdb/  blobfs/      L3
include/app/lib/      blob_db.h · rootreg.h · containers/{shape_map,shape_seq,…}.h
                      · kvdb.h · blobfs.h
tests/lib/            blob_db/ (exists) · blob_db_contract/ (model container)
                      · rootreg/ · containers/* · kvdb/ · blobfs/
doc/                  this file · principles.md · layers/*.md
```

Each new module follows the `lib/blob_db/` pattern: own `Kconfig`,
`CMakeLists.txt`, `add_subdirectory_ifdef` registration, ztest suite (P1, P8).
