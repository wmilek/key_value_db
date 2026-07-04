# L1½ — Root Registry: Global Registry of Structure Roots

Status: v1 (specified, pre-implementation)
· Sits directly on `doc/layers/l1_blob_db.md` · Governed by `doc/principles.md`
· Uses the basic flow of `doc/layers/l1_model_container.md` §3 exclusively

---

## 1. Purpose

Every structure in the stack is reachable from one root i-node id (P5) — but a
client that *creates* a hash, tree, or any other container has nowhere good to
keep that id. The root registry is the answer: a *global registry of
structure roots*, mapping compile-time keys to root ids.

The registry is a **helper, not a mandate**: a build that does not enable it
may bind id = 1 directly (L1 contract, root convention). When it *is* enabled,
it is the single **owner of id = 1** — nothing else in that system may bind,
rebind, or assume anything about id = 1.

Design posture, by requirement:

- **May be slow.** Every operation reads the one registry blob — one flash
  read. Linear scan of a handful of entries. No caching, no indexing.
- **Limited capacity.** A few registered roots (Kconfig-bounded, must fit one
  i-node payload). It is a registry, not a database.
- **Very stable.** Fixed-size entries, frozen on-flash format with a version
  field, and — decisively — every mutation is a **single atomic `update`**
  (the basic flow): no intent, no recovery procedure, no residue possible.

## 2. Position in the stack

```
        L3  kvdb · blobfs · settings      L2 containers' *clients*
                    │  rootreg_get_or_create(KEY) → root_id
                    ▼
        L1½ root registry (lib/rootreg)   ← this module; owns id = 1
                    │  alloc_id / update / get
                    ▼
        L1  blob_db
```

The registry depends only on `blob_db`. Containers (L2) do not use it — their
*clients* (L3 interfaces, application code) use it to persist the root ids of
the container instances they create. It is deliberately a sibling of the
containers, not one of them: it is the productionized **model container** in
its simplest possible form — a pair list in a single i-node.

## 3. Keys — a magic embedded in client source

A registry key is a `uint64_t` composed of a FourCC-style magic and an
instance number, both compile-time constants in the client's source:

```c
#define ROOTREG_KEY(magic32, instance32) \
        (((uint64_t)(magic32) << 32) | (uint32_t)(instance32))

/* examples */
ROOTREG_KEY('KVDB', 0)      /* the kvdb interface's map root            */
ROOTREG_KEY('BLFS', 0)      /* blobfs root directory                    */
ROOTREG_KEY('BOOT', 0)      /* application: boot-count blob             */
ROOTREG_KEY('KVDB', 1)      /* a second, independent kvdb instance      */
```

- FourCC magics are greppable in source and legible in a hexdump.
- The instance field supports multiple stores of the same type without new
  magics.
- Magics are project-coordinated constants (like devicetree `compatible`
  strings). Uniqueness is enforced at runtime: registering an existing key
  fails with `-EEXIST`, so a collision surfaces on the first boot of the bad
  build, not in the field.
- Key 0 is invalid (reserved).

## 4. On-flash format (implementation sketch, non-normative)

The contract-level requirements are: one i-node bound at id = 1, fixed-size
entries, a frozen versioned format, capacity within one payload. The layout
below is the intended realization — details may be fine-tuned during
implementation:

```
magic[4]      = 'R','R','E','G'
version[2]    = 1                       /* frozen format; bump = new format */
count[2]
entry[count]:                           /* 16 B each, fixed */
    key[8]        LE                    /* ROOTREG_KEY(...)  */
    root_id[8]    LE                    /* the registered structure's root  */
```

Capacity = what fits one payload: with the default
`BLOB_DB_MAX_PAYLOAD_LEN = 256`, 8 B header + 15 × 16 B entries. Bounded by
`CONFIG_ROOTREG_MAX_ROOTS` (default 8) with a `BUILD_ASSERT` against the
payload limit. There is no overflow chaining — if you need more roots than
fit, register one container and put your namespace inside it.

## 5. API

```c
/* Look up a registered root. -ENOENT if the key is not registered. */
int rootreg_get(uint64_t key, uint64_t *root_id);

/* Look up, or atomically allocate-and-register a fresh root id for this key.
 * The returned id may be allocated-but-unbound (§7) — the caller binds it. */
int rootreg_get_or_create(uint64_t key, uint64_t *root_id);

/* Register an existing structure's root. -EEXIST if key already present,
 * -ENOSPC if the registry is full. */
int rootreg_set(uint64_t key, uint64_t root_id);

/* Drop the entry. The structure behind it is NOT touched (§8). */
int rootreg_unregister(uint64_t key);

typedef int (*rootreg_iter_cb_t)(uint64_t key, uint64_t root_id, void *user);
int rootreg_iterate(rootreg_iter_cb_t cb, void *user);
```

All mutations rewrite the registry image in RAM and commit with **one**
`blob_db_update(1, …)` — basic flow, atomic, zero residue.

## 6. Bootstrap — how id = 1 comes to exist

Run once after `blob_db_mount()`, before any other storage user:

```
rootreg_init():
    rc = blob_db_get(1, buf, ...)
    if rc == 0:
        verify magic 'RREG' + version        → ready
        else                                 → -ENOTSUP (foreign format; refuse)
    if rc == -ENOENT:                        # virgin store
        id = blob_db_alloc_id()              # must return 1 (root convention)
        if id != 1: return -EIO              # store is not virgin — refuse
        blob_db_update(1, empty registry)    # bind: one atomic write
```

A crash between `alloc_id` and the bind leaves nothing on flash; the next
boot repeats the sequence and gets id 1 again (nothing was written, the
counter recovers to 1). After this point, id = 1 is bound to the registry for
the lifetime of the store.

## 7. `get_or_create` — the entry is the creation intent

Creating a *registered* structure used to be the awkward chicken-and-egg case
(who records the new root id before the structure exists?). The alloc/bind
split (spec §2, id lifecycle) dissolves it:

```
get_or_create(key):
    read registry (1 flash read)
    if key present: return its root_id
    id = blob_db_alloc_id()                  RAM only — nothing on flash
    blob_db_update(1, registry + (key,id))   COMMIT — entry durable
    return id                                caller now binds/populates the root
```

| Crash after | Next boot sees | Outcome |
|---|---|---|
| alloc | no entry, nothing on flash | repeat from scratch; ids burned, free |
| commit, before caller binds | entry `(key, id)` where `get(id)` → `-ENOENT` | `get_or_create` returns the **same id** (never reused); `update(id, …)` on an allocated id is defined — caller simply binds it now |
| caller's bind | entry + bound root | done |

A registered-but-unbound root is therefore **not residue and not an error**
— it is a defined, recoverable state meaning "creation in progress", and the
registry entry itself is the durable record that makes it recoverable. No
intent blob is needed anywhere in this path. Clients must treat
`get_or_create` → id whose `get` returns `-ENOENT` as "mine to initialize",
lazily binding the empty structure.

## 8. What the registry must never do

- **Walk into registered structures.** It stores ids; it does not understand
  what they point at. `unregister` drops the 16-byte entry and nothing else —
  tearing down the structure behind a root is the caller's job *before*
  unregistering (or a future global sweep's).
- **Grow clever.** No caching, no hashing, no chaining, no in-place entry
  edits. One blob, linear scan, whole-image single-`update` commits. Its
  value is that it is too simple to fail.
- **Share id = 1.** In a build with the registry enabled, any other binding
  of id = 1 is a system-integrity bug. (Registry-less builds own id = 1
  themselves — but then the registry must never be enabled later against the
  same store without a reformat; its `-ENOTSUP` magic check enforces this.)

## 9. Kconfig & placement

```
config BLOB_ROOTREG            bool "Root registry"   depends on BLOB_DB
config ROOTREG_MAX_ROOTS       int  "Max registered roots"  default 8
                               # BUILD_ASSERT: 8 + 16*MAX ≤ BLOB_DB_MAX_PAYLOAD_LEN
```

```
lib/rootreg/          { rootreg.c, Kconfig, CMakeLists.txt }
include/app/lib/      rootreg.h
tests/lib/rootreg/    ztest
```

L3 interfaces gain `select BLOB_ROOTREG`; their previous ad-hoc root
handling (fixed-field root blob, `l3_interfaces.md` §2) is superseded by
registry keys — enabling a new interface later is just a new key, no root
format migration.

## 10. Testing

1. virgin bootstrap: mount → init → id 1 bound, magic/version correct
2. foreign id 1 (wrong magic) → `-ENOTSUP`, registry refuses to touch it
3. get/set/unregister round-trips; `-EEXIST` on duplicate; `-ENOSPC` at cap
4. `get_or_create` idempotence: same key → same id across remounts
5. crash injection at every §6/§7 step: virgin re-bootstrap; registered-but-
   unbound root returned again and bindable; no residue anywhere
6. persistence across remount; registry readable with all entries after
   power loss during an unrelated registry mutation (old or new image, never
   torn)
