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
- finds its root through the **root registry** (P5);
- inherits L1's v1 concurrency contract: single-threaded, caller serializes.

## 2. Root ownership — via the root registry

Id = 1 is owned by the root registry (`doc/layers/l1_root_registry.md`);
interfaces never touch it directly. Each interface holds a compile-time
registry key and resolves its container root through it:

```c
#define KVDB_ROOT    ROOTREG_KEY('KVDB', 0)
#define BLOBFS_ROOT  ROOTREG_KEY('BLFS', 0)

/* in open(): */
rootreg_get_or_create(KVDB_ROOT, &root_id);
/* if blob_db_get(root_id) == -ENOENT: freshly allocated — bind the empty
 * container now (defined, recoverable state; registry doc §7). */
```

Each L3 interface `select`s `BLOB_ROOTREG`. Enabling a new interface later is
just a new key — no root-format migration. The single-integer principle is
preserved: the whole system still hangs off id = 1, through the registry.

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
`kvdb_foreach` and `blobfs_readdir` inherit the iterator rules from the L1
contract §4: callback returns 0/non-zero (Zephyr convention), and mutating
the store from inside the callback is undefined behavior.

## 4. `blobfs` — filesystem-like interface

Hierarchical `path → file` storage. Composition:

- **Directory** = a Map container instance: `name → dirent`, where a dirent
  encodes `{ type: dir|file, i-node id }`. Nested directories are just Map roots
  referenced from their parent — nesting gives paths.
- **File body** = inline value for small files; a `seq` chunk chain
  (`BLOBFS_FILE_CHUNKED`) for large or streamed content.

The API is **handle-free**: there is no open/close and no file-position
state — reads and writes pass an explicit offset (pread/pwrite style):

```c
int blobfs_mkdir   (const char *path);
int blobfs_create  (const char *path);                     /* empty file */
int blobfs_read    (const char *path, size_t off,
                    void *buf, size_t n, size_t *rd);      /* short read at EOF */
int blobfs_write   (const char *path, size_t off,
                    const void *buf, size_t n);            /* extends as needed */
int blobfs_truncate(const char *path, size_t size);
int blobfs_unlink  (const char *path);
int blobfs_rename  (const char *from, const char *to);
int blobfs_stat    (const char *path, blobfs_stat_t *st);
int blobfs_readdir (const char *path, blobfs_dirent_cb_t cb, void *user);
```

No handles means no cursor ownership, no unlink-while-open semantics, and no
`O_*` flag matrix — those questions do not exist in this API. The cost is
path resolution on every call (one Map lookup per component: `/a/b/c` ⇒ 3);
acceptable for embedded use, and a caching or handle layer can be added later
without changing these semantics. The Zephyr `fs_file_system_t` interop shim
(`BLOBFS_FS_INTEROP`) synthesizes handles on top of this API for
`fs_open("/blob/…")` compatibility.

`rename` within a directory is a single Map mutation (atomic, P7).
Cross-directory rename crash-semantics are **deferred to the blobfs
implementation design** (`doc/impl/`, when written); until specified, v1 may
restrict `rename` to within one directory (`-ENOTSUP` otherwise).

## 5. `settings` registry (optional)

A flat, typed configuration store (`"net/ip" → typed value`) over a Map backend
— and/or a **Zephyr `settings` backend** (`settings_backend` glue), letting
existing Zephyr subsystems (BT bonding, net config) persist through this stack
instead of NVS. Smallest of the three; may ship after `kvdb`/`blobfs`.

## 6. Kconfig

```
config KVDB                        bool "Key/value DB interface"   depends on BLOB_CONTAINERS
                                   select BLOB_ROOTREG
choice KVDB_BACKEND                prompt "kvdb backing container" depends on KVDB
  config KVDB_BACKEND_KVLIST       select CONTAINER_KVLIST
  config KVDB_BACKEND_KVHASH       select CONTAINER_KVHASH
  config KVDB_BACKEND_KVTREE       select CONTAINER_KVTREE
endchoice
config KVDB_MAX_KEY_LEN            int "Max key length"            default 64

config BLOBFS                      bool "Filesystem-like interface" depends on BLOB_CONTAINERS
                                   select BLOB_ROOTREG
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
- **`blobfs`**: path walking, deep nesting, readdir, within-directory rename
  (id stability), offset read/write at chunk boundaries, truncate grow/shrink,
  short reads at EOF.
- **Cross-interface**: enable `kvdb` + `blobfs` together; verify id = 1 root
  directory dispatch (§2) and mutual isolation.
- **Persistence & crash**: remount and torn-write cases at this level are smoke
  tests only — the real coverage lives in L1/L2 suites; L3 adds no new on-flash
  mechanics.
