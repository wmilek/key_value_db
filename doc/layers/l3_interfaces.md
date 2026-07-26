# L3 — Access Interfaces

Status: `kvdb` implemented (kvhash backend); `blobfs`/`settings` draft
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
- picks a backend either from a Kconfig default (P4) **or per-instance at
  runtime**, and persists the choice so a later open re-binds it;
- finds its roots through the **root registry** (P5);
- inherits L1's v1 concurrency contract: single-threaded, caller serializes.

## 2. Roots — multiple named instances via the root registry

Id = 1 is owned by the root registry (`doc/layers/l1_root_registry.md`);
interfaces never touch it directly. An interface is **not** a singleton: it
supports as many independent instances as the caller names, each hanging off
its own registry key. The instance name (a short string) is hashed to a 32-bit
id and combined with the interface's FourCC magic:

```c
key = ROOTREG_KEY('KVDB', fnv1a(name));   /* one per distinct name */
rootreg_get_or_create(key, &meta_id);     /* durable; may be freshly allocated */
```

The registry entry does not point straight at the container. It points at a
small **meta** blob the interface owns, which records *which backend built the
store* and the id of the container's own structure root:

```
rootreg[ ROOTREG_KEY('KVDB', hash(name)) ]  ->  meta { backend, struct_root }
                                                            |
                                          map_ops(struct_root) runs the map
```

This indirection is what lets the backend be chosen at runtime and still be
re-bound correctly on a later open: the meta is read first, its `backend` byte
selects the provider, and only then are `map_ops` calls issued against
`struct_root`. The container never sees the meta — L2 stays ignorant of L3.
The name is stored in the meta too, so a 32-bit hash collision between two
different names is *detected* (`-EEXIST`), never silently merged.

Each L3 interface `select`s `BLOB_ROOTREG`. Enabling a new interface, or a new
named instance, is just another key — no root-format migration.

## 3. `kvdb` — key/value database

The headline interface: `NUL-terminated string key → opaque byte value`. A thin
veneer over any **Map** container — keys pass through as bytes (without the
NUL), values verbatim.

```c
enum kvdb_backend { KVDB_BACKEND_DEFAULT, KVDB_BACKEND_HASH,
                    KVDB_BACKEND_TREE, KVDB_BACKEND_LIST };

struct kvdb_config {
        enum kvdb_backend backend;          /* preferred impl — create-time only */
        size_t            initial_capacity; /* hint (hash: bucket count) */
};

int  kvdb_open  (kvdb_t *db, const char *name, const struct kvdb_config *cfg);
int  kvdb_set   (kvdb_t *db, const char *key, const void *val, size_t len);
int  kvdb_get   (kvdb_t *db, const char *key, void *out, size_t out_sz, size_t *len);
int  kvdb_delete(kvdb_t *db, const char *key);
bool kvdb_has   (kvdb_t *db, const char *key);
```

**Instances.** Distinct `name`s are fully independent stores (§2); the same
name always attaches to the same store. Names are 1..`KVDB_NAME_MAX` bytes.

**Creation config.** `cfg` is a *preference*, consumed **only when the named
instance does not yet exist**. It names the backend to build and a capacity
hint for it. On every subsequent open the persisted structure governs and `cfg`
is ignored (a conflicting `cfg` is not an error — the stored store wins). Pass
`NULL`, or `KVDB_BACKEND_DEFAULT`, to accept the Kconfig-selected default. A
backend requested (or previously stored) but not compiled into the build fails
cleanly with `-ENOTSUP`.

Backend profiles:

| Backend | Cost profile | Pick when | Status |
|---|---|---|---|
| `KVDB_BACKEND_LIST` (kvlist) | O(n), smallest code | ≲ tens of keys | planned |
| `KVDB_BACKEND_HASH` (kvhash) | O(1) point ops | many keys, no ordering needed | **implemented** |
| `KVDB_BACKEND_TREE` (kvtree) | O(log n), sorted scans | ordered iteration matters | planned |

The API is identical across backends; only costs change. Ordered iteration
(`kvdb_foreach`) is **deferred**: it needs an `iterate` op on `map_ops`, which
the shipped Map shape does not yet define. It lands with the first backend that
can order keys (kvtree).

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
config BLOBDB_KVDB                 bool "kvdb (L3)"                depends on BLOB_DB
                                   select BLOB_ROOTREG
# The default backend is a Kconfig choice; a caller may request a *different*
# backend at runtime provided that container is also enabled.
choice                             prompt "kvdb default backend"   depends on BLOBDB_KVDB
  config KVDB_DEFAULT_BACKEND_HASH select BLOB_CONTAINER_KVHASH    # implemented
endchoice
# (KVDB_DEFAULT_BACKEND_{LIST,TREE} join the choice as those containers land.)

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
