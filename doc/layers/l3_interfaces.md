# L3 — Access Interfaces

Status: draft (pre-implementation)
· Part of the stack in `doc/architecture.md` · Governed by `doc/principles.md`
· Builds on L2: `doc/layers/l2_containers.md`

---

## 1. Role

L3 is what the **rest of the firmware** calls. It translates a domain vocabulary
— string keys, filesystem paths, typed settings — into `map_ops`/`seq_ops` calls
on a container. L3 modules define **no on-flash format of their own** beyond the
payload encodings they store *through* the container; all persistence mechanics
(atomicity, recovery, reachability) are inherited from L2/L1.

Each interface:

- binds to a **shape** (Map, Sequence), never to a concrete container (P6);
- gets its concrete backend from a Kconfig `choice` (P4);
- anchors at the id = 1 root i-node, or at a field inside it (P5).

## 2. Root ownership

Only one thing can *be* id = 1. When a single L3 interface is enabled, its
container root is simply id = 1. When several are enabled, id = 1 holds a tiny
**root directory blob** mapping each enabled interface to its container root id:

```
root (id=1) { magic 'ROOT', version,
              kvdb_root_id, blobfs_root_id, … }     /* 0 = interface not initialized */
```

Fields are allocated per enabled interface at first mount; an interface's
`open` reads id = 1, finds (or lazily creates) its own root, and proceeds. The
single-integer principle is preserved: the whole system still hangs off id = 1.

## 3. `kvdb` — key/value database

The headline interface: `NUL-terminated string key → opaque byte value`. A thin
veneer over any **Map** container — keys pass through as bytes (without the
NUL), values verbatim.

```c
int  kvdb_open   (kvdb_t *db);                 /* attach via id=1; lazy-create */
int  kvdb_set    (kvdb_t *db, const char *key, const void *val, size_t len);
int  kvdb_get    (kvdb_t *db, const char *key, void *out, size_t out_sz, size_t *len);
int  kvdb_del    (kvdb_t *db, const char *key);
bool kvdb_has    (kvdb_t *db, const char *key);
int  kvdb_foreach(kvdb_t *db, kvdb_cb_t cb, void *user);
```

Backend `choice` and resulting profile:

| Backend | Cost profile | Pick when |
|---|---|---|
| `KVDB_BACKEND_KVLIST` | O(n), smallest code | ≲ tens of keys |
| `KVDB_BACKEND_KVHASH` | O(1) point ops | many keys, no ordering needed |
| `KVDB_BACKEND_KVTREE` | O(log n), sorted `foreach`, prefix/range scans | ordered iteration matters |

The API is identical across backends; only costs (and `foreach` order) change.

## 4. `blobfs` — filesystem-like interface

Hierarchical `path → file` storage. Composition:

- **Directory** = a Map container instance: `name → dirent`, where a dirent
  encodes `{ type: dir|file, i-node id }`. Nested directories are just Map roots
  referenced from their parent — nesting gives paths.
- **File body** = inline value for small files; a `seq` chunk chain
  (`BLOBFS_FILE_CHUNKED`) for large or streamed content.

```c
int blobfs_mkdir  (const char *path);
int blobfs_open   (const char *path, int flags, blobfs_file_t *f);   /* O_CREAT, O_TRUNC, O_APPEND */
int blobfs_read   (blobfs_file_t *f, void *buf, size_t n, size_t *rd);
int blobfs_write  (blobfs_file_t *f, const void *buf, size_t n);
int blobfs_close  (blobfs_file_t *f);
int blobfs_unlink (const char *path);
int blobfs_rename (const char *from, const char *to);
int blobfs_stat   (const char *path, blobfs_stat_t *st);
int blobfs_readdir(const char *path, blobfs_dirent_cb_t cb, void *user);
```

Path resolution walks one Map lookup per component (`/a/b/c` ⇒ 3 lookups).
`rename` within a directory is one Map mutation; across directories it is
insert-then-delete — the file's i-node id never changes, so open handles and
the file body are untouched. Optionally registrable as a Zephyr `fs_file_system_t`
backend (`BLOBFS_FS_INTEROP`) so `fs_open("/blob/…")` and friends work.

## 5. `settings` registry (optional)

A flat, typed configuration store (`"net/ip" → typed value`) over a Map backend
— and/or a **Zephyr `settings` backend** (`settings_backend` glue), letting
existing Zephyr subsystems (BT bonding, net config) persist through this stack
instead of NVS. Smallest of the three; may ship after `kvdb`/`blobfs`.

## 6. Kconfig

```
config KVDB                        bool "Key/value DB interface"   depends on BLOB_CONTAINERS
choice KVDB_BACKEND                prompt "kvdb backing container" depends on KVDB
  config KVDB_BACKEND_KVLIST       select CONTAINER_KVLIST
  config KVDB_BACKEND_KVHASH       select CONTAINER_KVHASH
  config KVDB_BACKEND_KVTREE       select CONTAINER_KVTREE
endchoice
config KVDB_MAX_KEY_LEN            int "Max key length"            default 64

config BLOBFS                      bool "Filesystem-like interface" depends on BLOB_CONTAINERS
choice BLOBFS_DIR_BACKEND          prompt "directory container"    depends on BLOBFS
  config BLOBFS_DIR_KVHASH         select CONTAINER_KVHASH
  config BLOBFS_DIR_KVTREE         select CONTAINER_KVTREE         # sorted readdir
endchoice
config BLOBFS_FILE_CHUNKED         bool "Chunked file bodies"      select CONTAINER_SEQ
config BLOBFS_MAX_PATH_LEN         int "Max path length"           default 128
config BLOBFS_FS_INTEROP           bool "Register as Zephyr fs backend"  depends on FILE_SYSTEM

config SETTINGS_KVDB               bool "Zephyr settings backend over kvdb"  depends on KVDB && SETTINGS
```

Interfaces are independent: any subset may be enabled, and each pulls in exactly
the containers it needs via `select` (P4). Enabling `KVDB_BACKEND_KVLIST` alone
links `blob_db + kvlist + kvdb` and nothing else.

## 7. Repository layout

```
lib/kvdb/    { kvdb.c,   Kconfig, CMakeLists.txt }
lib/blobfs/  { blobfs.c, blobfs_zephyr_fs.c*, Kconfig, CMakeLists.txt }   * ifdef INTEROP
include/app/lib/  kvdb.h  blobfs.h
tests/lib/kvdb/   ztest, run per enabled backend (twister scenarios)
tests/lib/blobfs/ ztest
```

## 8. Testing strategy

- **`kvdb`**: one functional suite, executed as a twister scenario **per
  backend** (`extra_configs` swaps the `choice`), asserting identical observable
  behavior — the conformance guarantee that makes backends swappable.
- **`blobfs`**: path walking, deep nesting, readdir, rename semantics
  (id stability across rename), chunked read/write at chunk boundaries,
  unlink-while-open policy.
- **Cross-interface**: enable `kvdb` + `blobfs` together; verify id = 1 root
  directory dispatch (§2) and mutual isolation.
- **Persistence & crash**: remount and torn-write cases at this level are smoke
  tests only — the real coverage lives in L1/L2 suites; L3 adds no new on-flash
  mechanics.
