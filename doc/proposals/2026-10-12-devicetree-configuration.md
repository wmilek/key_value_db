# Design change proposal — configuring the stack from devicetree

Status: **implemented (draft)** · 2026-10-12 · what was built, and where it
differs from this design: §9
· Target contract: `doc/layers/l1_blob_db.md` (mount), `doc/layers/l1_root_registry.md`
(`rootreg_init`), `include/app/lib/blobfs_fs.h` (mounting)
· Target implementation: `lib/blob_db/` (`blob_db.c`, `blob_db_store_ubi.c`,
`blob_db_store_flash.c`, `Kconfig`), `lib/rootreg/rootreg.c`,
`lib/blobfs/blobfs_zephyr_fs.c`, new bindings under `dts/bindings/`
· Precedent: Zephyr's littlefs fstab (`subsys/fs/littlefs_fs.c`), Linux's
`linux,ubi` partition binding
· Governed by `doc/principles.md`; amends P4 (§7)

**The ask.** The board's devicetree should say what the storage is made of. In
other words, *this partition is a UBI device, it holds a volume, and blob_db
lives on that volume*. `main()` should no longer have to call anything to bring
the stack up.

**Short answer.** Three changes do it, and each one is independently useful:

1. **Layout in devicetree.** A partition with `compatible = "kvdb,ubi"` holds
   its UBI volumes as child nodes. A `kvdb,blob-db` node points at one volume
   with a phandle, or at a plain partition for the raw backend. Code stops
   naming `storage_partition`, and the backend choice follows from the
   devicetree.
2. **blob_db becomes a Zephyr device.** The device's init function mounts the
   store, so `device_is_ready()` reports whether mounting worked.
   `zephyr,deferred-init` covers boards whose UBI key only exists after `main`
   has provisioned it.
3. **Everything above blob_db starts lazily.** The first call into blob_db
   mounts the store if nothing has mounted it yet. rootreg bootstraps itself on
   first use, and blobfs can be mounted from fstab with `automount`, like
   littlefs.

The on-flash format does not change. A device in the field keeps its data as
long as its overlay keeps the same partition and the volume name `"blobdb"`.

---

## 1. How the stack is brought up today

| What | Where it is decided today |
|---|---|
| Which partition | `PARTITION_ID(storage_partition)`, hard-coded in `blob_db_store_ubi.c:45` and `blob_db_store_flash.c:20` |
| Raw flash or UBI | Kconfig choice `BLOB_DB_BACKEND_UBI` / `BLOB_DB_BACKEND_FLASH_AREA` |
| UBI volume name, spare LEBs | `#define BLOB_DB_UBI_VOL_NAME "blobdb"`, `#define BLOB_DB_UBI_SPARE_LEBS 4` (`blob_db_store_ubi.c`) |
| UBI heap | `BLOB_DB_UBI_HEAP_SIZE`, defaulted from `$(dt_nodelabel_reg_size_int,storage_partition)`, overridden to 6144 in every nRF5340 board file |
| Format a corrupt store or not | `BLOB_DB_AUTOFORMAT_ON_CORRUPT` |
| Partition label | `BLOB_DB_PARTITION_LABEL`, which is **read by nothing** (`app_cbor_persondb/FINDINGS.md` B11) |
| Bring-up | every `main()`: `blob_db_mount()`, then `rootreg_init()`, then `kvdb_open()` / `fs_mount()` (`app/`, `samples/kvhash/`, all `app_perf*/`, `app_cbor_persondb/persondb/persondb.c`) |

One piece of lazy start-up already exists. `blobfs_fs_mount()`
(`blobfs_zephyr_fs.c:587`) calls `blob_db_mount()` and `rootreg_init()` itself
and treats `-EALREADY` as success. So the stack can already start on demand;
it just only does so for blobfs.

Three problems follow from the table:

- **A board cannot say where the store lives.** It can only move the node
  labelled `storage_partition`. Two stores, or a store on a partition with a
  different label, need code changes.
- **The backend is chosen in Kconfig, yet it is a property of the layout.**
  A raw store and a UBI store on the same partition are not interchangeable
  (`lib/blob_db/Kconfig`), so the choice describes what is on the flash.
  That is the job devicetree already does for partitions.
- **Every application repeats the same boilerplate**, and gets the order
  right by convention. An application that forgets `rootreg_init()` gets
  `-ENOTSUP` from `rootreg_get_or_create()`.

## 2. Precedents

### 2.1 littlefs: fstab nodes

```dts
/ {
	fstab {
		compatible = "zephyr,fstab";
		lfs1: lfs1 {
			compatible = "zephyr,fstab,littlefs";
			mount-point = "/lfs1";
			partition = <&lfs1_part>;
			automount;
			read-size = <16>;
			prog-size = <16>;
			cache-size = <64>;
			lookahead-size = <32>;
			block-cycles = <512>;
		};
	};
};
```

From `samples/subsys/fs/littlefs/boards/bmd_345_eval.overlay`. The driver
(`subsys/fs/littlefs_fs.c:1094-1181`) turns it into code in four steps:

1. **One static instance per node.** `DT_INST_FOREACH_STATUS_OKAY(DEFINE_FS)`
   emits the config, buffers and `struct fs_mount_t` for each enabled node, so
   nothing is allocated at run time.
2. **Geometry checked at build time.** `BUILD_ASSERT`s relate cache, read and
   prog sizes to each other.
3. **Mounting at boot.** One `SYS_INIT(littlefs_init, POST_KERNEL, ...)`
   registers the file system and calls `fs_mount()` on every node with
   `automount`.
4. **The application can still reach the mount.** `FS_FSTAB_DECLARE_ENTRY(node)`
   and `&FS_FSTAB_ENTRY(node)` name the generated `fs_mount_t`, and
   `FSTAB_ENTRY_DT_MOUNT_FLAGS(node)` tells the application whether to mount it
   itself.

`read-only` and `no-format` become mount flags; they come from
`zephyr,fstab-common.yaml`, which any fstab binding can include.

**The part not to copy:** if automount fails, littlefs logs an error and
nothing else. The application has no way to ask whether its file system is
there. §3.4 fixes this by making blob_db a device.

### 2.2 settings: a chosen partition

NVS, ZMS and FCB settings find their partition through
`DT_PARTITION_ID(DT_CHOSEN(zephyr_settings_partition))`
(`subsys/settings/src/settings_*.c`). This is the minimal form, a chosen node
pointing at a partition. It has no place to put per-store flags such as
`automount` or `no-format`, and no node to make into a device, so it is the
fallback considered in §6.1 rather than the design.

### 2.3 Linux: `linux,ubi`

Linux marks a flash partition as UBI with `compatible = "linux,ubi"`. Its UBI
volumes are child nodes under `volumes { ubi-volume-<x> { volname = "..."; }; }`,
and consumers point at a volume node, for example an NVMEM layout inside it.
That is exactly the chain this repository has: partition, then UBI, then a
volume, then a consumer. The proposed bindings keep Linux's shape and property
names (`volumes`, `volname`, `volid`) so the layout reads the same to anyone
who knows that binding.

## 3. Design

### 3.1 What a board writes

The nRF5340-DK overlay becomes:

```dts
/delete-node/ &storage_partition;

&mx25r64 {
	partitions {
		compatible = "fixed-partitions";
		#address-cells = <1>;
		#size-cells = <1>;

		ubi0: partition@0 {
			compatible = "kvdb,ubi";
			label = "storage";
			reg = <0x00000000 0x00800000>;
			spare-lebs = <4>;

			volumes {
				blobdb_vol: ubi-volume-blobdb {
					volname = "blobdb";
				};
			};
		};
	};
};

/ {
	chosen {
		kvdb,blob-db = &blobdb0;
	};

	blobdb0: blob-db {
		compatible = "kvdb,blob-db";
		storage = <&blobdb_vol>;
		automount;
	};

	fstab {
		compatible = "zephyr,fstab";
		blobfs0: blobfs {
			compatible = "kvdb,fstab,blobfs";
			mount-point = "/blob";
			db = <&blobdb0>;
			automount;
		};
	};
};
```

A board that wants the raw backend points `storage` at an ordinary partition
and declares no `kvdb,ubi` node:

```dts
blobdb0: blob-db {
	compatible = "kvdb,blob-db";
	storage = <&storage_partition>;
};
```

The native_sim overlays keep their 16 MB sim-flash and 8 MB partition; the
only addition is the `compatible`, the `volumes` child and the `blob-db` node.

### 3.2 Bindings

**`kvdb,ubi`**: a fixed partition holding a UBI device
(`dts/bindings/mtd/kvdb,ubi.yaml`, including `fixed-partitions` children's
properties: `reg`, `label`, `read-only`).

| Property | Type | Meaning | Replaces |
|---|---|---|---|
| `spare-lebs` | int, default 4 | LEBs left out of the volumes, for relocation and retired blocks | `BLOB_DB_UBI_SPARE_LEBS` |
| `volumes` / `ubi-volume-*` child | node | One UBI volume each | — |
| `volname` (in a volume) | string, required | Name looked up with `ubi_volume_find()`, created if missing | `BLOB_DB_UBI_VOL_NAME` |
| `volid` (in a volume) | int, optional | Fixed volume id; absent means UBI assigns one | — |
| `size` (in a volume) | int, optional | Volume size in bytes, rounded up to LEBs; absent means "the rest of the device" | today's implicit "all LEBs minus spare" |

The UBI binding has no consumer-specific properties. blob_db is one consumer of
a volume; another volume on the same device could hold something else. With
more than one volume, at most one may omit `size`.

**`kvdb,blob-db`**: one blob_db store (`dts/bindings/storage/kvdb,blob-db.yaml`).

| Property | Type | Meaning | Replaces |
|---|---|---|---|
| `storage` | phandle, required | A `ubi-volume-*` node (UBI backend) or a fixed partition (flash_area backend) | `storage_partition` in code, `BLOB_DB_BACKEND_*` choice |
| `automount` | boolean | Mount during device init (§3.4) | `blob_db_mount()` in `main` |
| `no-format` | boolean | Never format; an unreadable store fails mount with `-EIO` | `BLOB_DB_AUTOFORMAT_ON_CORRUPT=n` |
| `zephyr,deferred-init` | boolean (from `base.yaml`) | Do not initialize at boot; the application calls `device_init()` | — |

**`kvdb,fstab,blobfs`**: a blobfs mount, child of `zephyr,fstab`, including
`zephyr,fstab-common.yaml` (`mount-point`, `automount`, `read-only`,
`no-format`, `disk-access`) plus `db`, a phandle to a `kvdb,blob-db` node.
`disk-access` is rejected with a `BUILD_ASSERT`; blobfs has no disk backend.

**Vendor prefix.** `kvdb` is this module's own prefix. A UBI binding belongs in
zephyr-ubi eventually; if that module grows one, `kvdb,ubi` becomes an alias
for it and is later removed (§6.3).

### 3.3 The backend follows the devicetree

Zephyr generates a `DT_HAS_<COMPAT>_ENABLED` Kconfig symbol for every
compatible that has an enabled node, which is what drivers use to switch
themselves on. blob_db does the same:

```kconfig
config BLOB_DB
	bool "Stable-id blob storage library"
	default y
	depends on DT_HAS_KVDB_BLOB_DB_ENABLED
	...

choice BLOB_DB_BACKEND
	default BLOB_DB_BACKEND_UBI if DT_HAS_KVDB_UBI_ENABLED
	default BLOB_DB_BACKEND_FLASH_AREA
```

`CONFIG_BLOB_DB=y` then disappears from every `prj.conf`; the overlay alone
decides. The choice remains a choice, so a `-DCONFIG_...` override still works
for experiments, but a `BUILD_ASSERT` in each backend checks that it agrees with
the node `storage` points at:

```c
/* blob_db_store_ubi.c */
#define BLOB_DB_NODE   DT_CHOSEN(kvdb_blob_db)
#define STORAGE_NODE   DT_PHANDLE(BLOB_DB_NODE, storage)
#define UBI_NODE       DT_GPARENT(STORAGE_NODE)   /* volume -> volumes -> partition */

BUILD_ASSERT(DT_NODE_HAS_COMPAT(UBI_NODE, kvdb_ubi),
	     "blob-db 'storage' must point at a UBI volume for the UBI backend");

/* replaces BLOB_DB_UBI_PARTITION / BLOB_DB_UBI_VOL_NAME / BLOB_DB_UBI_SPARE_LEBS */
#define UBI_PARTITION_ID  DT_PARTITION_ID(UBI_NODE)
#define UBI_VOL_NAME      DT_PROP(STORAGE_NODE, volname)
#define UBI_SPARE_LEBS    DT_PROP_OR(UBI_NODE, spare_lebs, 4)
```

`blob_db_store_flash.c` does the same with
`DT_PARTITION_ID(DT_PHANDLE(BLOB_DB_NODE, storage))` and asserts that the target
is *not* a UBI volume. `BLOB_DB_PARTITION_LABEL` is deleted, which closes B11.

**UBI heap default.** `BLOB_DB_UBI_HEAP_SIZE` is sized from the UBI partition
instead of the `storage_partition` label. Kconfig preprocessor functions split
arguments on commas, so the chosen name goes through a variable, as Zephyr's
own Kconfig does with `DT_CHOSEN_Z_FLASH`:

```kconfig
DT_CHOSEN_BLOB_DB := kvdb,blob-db
dt_blob_db_ubi := $(dt_node_parent,$(dt_node_parent,$(dt_node_ph_prop_path,$(dt_chosen_path,$(DT_CHOSEN_BLOB_DB)),storage)))

config BLOB_DB_UBI_HEAP_SIZE
	default $(add,$(div,$(dt_node_reg_size_int,$(dt_blob_db_ubi)),512),4096)
```

All four functions (`dt_chosen_path`, `dt_node_ph_prop_path`,
`dt_node_parent`, `dt_node_reg_size_int`) exist in the tree's
`scripts/kconfig/kconfigfunctions.py`. Whether the nesting evaluates as written
is the first thing the prototype checks (§8, step 1).

### 3.4 Bring-up: blob_db as a device

The `kvdb,blob-db` node becomes a device:

```c
#define DT_DRV_COMPAT kvdb_blob_db

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
	     "blob_db is a single store; one kvdb,blob-db node at most");

static int blob_db_dev_init(const struct device *dev)
{
	if (!DT_INST_PROP(0, automount)) {
		return 0;               /* lazy: first API call mounts (§3.5) */
	}
	return blob_db_mount();
}

DEVICE_DT_INST_DEFINE(0, blob_db_dev_init, NULL, NULL, NULL,
		      POST_KERNEL, CONFIG_BLOB_DB_INIT_PRIORITY, NULL);
```

The device model gives three things a bare `SYS_INIT` would not:

- **A status `main` can ask for.** `device_is_ready(DEVICE_DT_GET(...))` is false
  when the automatic mount failed, and the error is the init function's return
  value. This fixes the littlefs gap in §2.1.
- **Ordering against the flash.** With `CONFIG_BLOB_DB_INIT_PRIORITY` above
  the QSPI flash driver's priority, the flash is ready before blob_db mounts.
  A `BUILD_ASSERT` against the flash device's priority could enforce this later.
- **Deferred start without new mechanism.** `zephyr,deferred-init;` on the node
  (a standard property in `base.yaml`) skips init at boot; the application
  calls `device_init(dev)` when it is ready. This is the answer for
  `BLOB_DB_UBI_KEY_APP`. That configuration's `blob_db_ubi_ikm_key()` usually
  has no key until `main` has provisioned it on first boot, so mounting at boot
  would fail.

The public API stays handle-less. The device exists for initialization and
readiness, not as a parameter to `blob_db_get()`. Multi-instance is a separate
decision (§6.2), and the `BUILD_ASSERT` keeps it from arriving by accident
through a second node.

`psa_crypto_init()` stays inside `blob_db_store_open()`, where it is today; it
is idempotent and has no init level of its own to order against.

### 3.5 Lazy start of everything above the device

Every public blob_db entry point that today begins with
`if (!st.mounted) return -ENODEV;` (about a dozen, `blob_db.c:2270-3152`)
instead begins with:

```c
static int ensure_mounted(void)
{
	if (st.mounted) {
		return 0;
	}
	if (st.held_down) {
		return -ENODEV;     /* explicitly unmounted: stay down (below) */
	}
	return blob_db_mount();
}
```

The first `blob_db_get()` on a node without `automount` therefore mounts the
store. That first call takes as long as a mount, which on UBI includes the
attach scan. Boards that want a predictable first call set `automount`.

**Explicit control still wins.** `blob_db_unmount()` sets `held_down`, and
`blob_db_mount()` and `blob_db_format()` clear it. A test or an application
that unmounts on purpose (the power-cut tests, `persondb_close()`, the
`app_perf*` benchmarks between phases) keeps today's semantics: an unmounted
store answers `-ENODEV` until someone mounts it again. Only a store that has
never been unmounted mounts on its own.

**rootreg bootstraps on first use.** `rootreg_init()` runs `rr_load()` and, on
the virgin signal (an empty root payload), writes an empty registry. The same
check moves into the first `rootreg_get_or_create()`; `rootreg_get()` on a
virgin store reports `-ENOENT` without writing. `rootreg_init()` stays in the
API as an explicit, idempotent call, but nothing needs it, and the
`-ENOTSUP` "registry not bootstrapped" outcome (`rootreg.h:57`) goes away. The
refusal to overwrite a foreign payload at id 1 is unchanged.

**kvdb needs nothing.** `kvdb_open()` already goes through
`rootreg_get_or_create()` and `blob_db_get()`, so it inherits both.

### 3.6 blobfs from fstab

The blobfs interop driver mirrors littlefs: one `fs_mount_t` per
`kvdb,fstab,blobfs` node, named through `FS_FSTAB_ENTRY(node)`, and mounted in
its `SYS_INIT` when the node has `automount`. `blobfs_fs_mount()` already
brings the stack up itself; with §3.5 that code shrinks to `blobfs_mount()`.

The fstab automount runs at `CONFIG_FILE_SYSTEM_INIT_PRIORITY`. If the
blob_db device is `zephyr,deferred-init`, automounting blobfs at boot would
mount blob_db lazily *before* the application provided the key; a
`BUILD_ASSERT` refuses that combination.

### 3.7 Errors

| Situation | What the application sees |
|---|---|
| `automount`, mount fails | `device_is_ready()` false; every API call returns the mount's error |
| No `automount`, first call | the call mounts; if mounting fails, that call returns the mount's error and the next call tries again |
| `zephyr,deferred-init`, before `device_init()` | API calls mount lazily, like the row above; an application using this flag calls `device_init()` first |
| Explicitly unmounted | `-ENODEV` until `blob_db_mount()` / `blob_db_format()` |

Retrying a failed lazy mount on every call is deliberate: the usual cause of a
failure at boot (no key yet, flash not powered) is gone by the next call. A
store that fails because it is corrupt or foreign keeps failing with the same
code, which is the existing contract (`l1_blob_db.md`: mount never destroys a
store it does not understand).

## 4. What `main()` looks like

`app/src/main.c` today:

```c
int rc = blob_db_mount();
if (rc < 0) {
	LOG_ERR("blob_db_mount failed: %d", rc);
	goto out;
}
rc = blob_db_get(BLOB_DB_ROOT_ID, &boot_count, sizeof(boot_count), &got);
```

After this change:

```c
rc = blob_db_get(BLOB_DB_ROOT_ID, &boot_count, sizeof(boot_count), &got);
```

`samples/kvhash` loses `blob_db_mount()` and `rootreg_init()`. A blobfs
application with an fstab node writes no init code at all; it calls
`fs_open("/blob/wifi.ssid", ...)`. An application with a provisioned UBI key
writes:

```c
provision_key_if_needed();
device_init(DEVICE_DT_GET(DT_CHOSEN(kvdb_blob_db)));
```

The samples keep their narration of the bring-up where teaching it is the
point (`samples/kvhash` prints each call); they would show the device's
readiness instead of the mount call.

## 5. What stays in Kconfig

Zephyr's devicetree guidelines keep software configuration out of devicetree.
The rule this proposal follows: **devicetree describes what is on the flash and
which instances exist; Kconfig describes how the software is built.**
Partitions and fstab are the accepted precedents for the first half, and the
UBI device, its volumes and the store sitting on one of them are the same kind
of fact.

| Stays in Kconfig | Why |
|---|---|
| `BLOB_DB_MAX_PAYLOAD_LEN`, `BLOB_DB_SECTOR_BUF_SIZE`, `BLOB_DB_LARGE_PAYLOADS`, `BLOB_DB_MAX_SEGMENTS`, `BLOB_DB_SEGMENT_LEN` | Size `.bss` and stack buffers, or change compiled code |
| `BLOB_DB_UBI_RELOCATE_BUDGET`, `BLOB_DB_UBI_ATOMIC_REPLACE` | Tuning of the code, not a property of the layout |
| `BLOB_DB_UBI_KEY_BUILTIN` / `_APP` | Where code gets a secret; key material never belongs in devicetree |
| `BLOB_DB_UBI_HEAP_SIZE` | Heap reservation; its *default* is computed from devicetree (§3.3) |
| `BLOB_DB_IOSTATS`, `BLOB_DB_INSPECT`, `BLOB_DB_SHELL`, `BLOB_DB_TEST_CRASH_HOOKS` | Diagnostics |
| `BLOBFS_*` sizes, pools, name length | Compiled limits |

| Moves to devicetree | Kconfig symbol removed |
|---|---|
| Which partition, which volume | `BLOB_DB_PARTITION_LABEL` (unused today) |
| Raw or UBI | `BLOB_DB_BACKEND_*` keeps existing, defaulted from devicetree (§3.3) |
| Format on corruption | `BLOB_DB_AUTOFORMAT_ON_CORRUPT` → `no-format` |
| Volume name, spare LEBs | the two `#define`s in `blob_db_store_ubi.c` |
| Enable the library | `CONFIG_BLOB_DB=y` in `prj.conf` → `DT_HAS_KVDB_BLOB_DB_ENABLED` |

`BLOB_DB_AUTOFORMAT_ON_CORRUPT` is removed rather than kept alongside
`no-format`. A devicetree boolean has no "unset" state, so the two could not be
combined without one silently overriding the other. The default behaviour
(format a store whose masters are both unreadable) is unchanged; production
boards that set the symbol to `n` today add `no-format` instead.

## 6. Open questions and costs

### 6.1 Is the `blob-db` node needed, or is a chosen partition enough?

The minimal alternative is the settings shape: `chosen { kvdb,blob-db-storage
= &blobdb_vol; }` and no `blob-db` node. It moves the location into devicetree
and nothing else. It cannot carry `automount`, `no-format` or
`zephyr,deferred-init`, and gives no node to make into a device, so
`device_is_ready()` and deferred init are lost. **Recommendation:** the node,
with the chosen property pointing at it so code and Kconfig find it without a
fixed node label.

### 6.2 One store or several

Devicetree invites several `kvdb,blob-db` nodes. blob_db is a singleton by
design (`st` and the store globals are static), and its API carries no handle.
Supporting several stores means a handle on every call. That is an API change
for every layer above, and it is out of scope here. The `BUILD_ASSERT` in §3.4
keeps the singleton honest until someone decides to change it.

### 6.3 Who owns the UBI binding

zephyr-ubi (`v0.1.0`) has no devicetree support: its own samples and tests use
`DT_NODELABEL(ubi_partition)` or `storage_partition` directly. Defining
`kvdb,ubi` here works now but puts a UBI concept in this repository. Proposing
the binding upstream (as `zephyr,ubi` or under the module's prefix) is the
better long-term home; this repository then aliases to it. Volume creation
(`ubi_volume_create()` when `volname` is not found) stays in the blob_db
backend either way, because zephyr-ubi has no "attach from devicetree" entry
point.

### 6.4 Devicetree maintainers' policy

Upstream Zephyr resists software settings in devicetree; fstab is tolerated
because it describes storage layout. Everything moved here is layout or
instance identity (§5), so the design stays inside that boundary. As an
out-of-tree module this repository is not bound by upstream review, but staying
inside the boundary keeps the UBI binding upstreamable (§6.3).

### 6.5 Tests

The suites mount, unmount and format on purpose, and the power-cut tests
depend on full control. Their overlays omit `automount`, and the `held_down`
rule (§3.5) keeps an explicit `blob_db_unmount()` from being undone by a lazy
mount. Test code that calls `blob_db_mount()` keeps working: the call still
exists, and without `automount` nothing mounts the store before the test's own
first call, so it does not see an unexpected `-EALREADY`.

The tests that name `storage_partition` to corrupt or inspect raw flash
(`tests/lib/blob_db/src/main.c`, `power_cut.c`, `ubi.c`) switch to the same
`DT_PHANDLE(DT_CHOSEN(kvdb_blob_db), storage)` lookup as the backend, so they
always damage the partition blob_db actually uses.

### 6.6 First-call latency

A lazy mount moves the attach scan into whichever call comes first, which may
be on a latency-sensitive path. That conflicts with P2's "deterministic, bounded
cost" unless the board opts into `automount`. The proposal's answer is that
`automount` is the documented default for products and lazy mount is for
samples and for applications that want to control boot order; the
`samples/` overlays would show `automount`.

## 7. Principles

- **P1 (Zephyr-based)** is the motivation: devicetree, the device model, fstab
  and `zephyr,deferred-init` are Zephyr's own mechanisms.
- **P4 (Configurable via Kconfig)** is amended. Proposed wording:
  > Every module above the core has its own symbol; disabled = zero flash/RAM.
  > **Storage layout and instances — which partition, which UBI volume, whether
  > a store mounts at boot — are described in devicetree; Kconfig symbols that
  > depend on them default from it.** Backends are `choice`s; selection flows
  > downward. Tunables are Kconfig options, not magic numbers.
- **P6 (Strict downward layering)** is unchanged. Upper layers still reach the
  store only through the blob_db API; lazy mount happens behind it.
- **P8 (Test-proven)**: §8 lists the tests that prove each piece.

## 8. Implementation plan

Each step builds and passes on its own.

1. **Bindings and lookup, no behaviour change.** Add `kvdb,ubi`,
   `kvdb,blob-db` and `kvdb,fstab,blobfs` bindings. Switch both backends and
   the UBI heap default to the chosen-node lookup (§3.3). Update every overlay
   in the repository (`app*/`, `samples/`, `tests/`). Delete
   `BLOB_DB_PARTITION_LABEL`. Test: the full twister run on native_sim, plus a
   `BUILD_ASSERT` failure test for a mismatched `storage` target.
2. **Backend and enable from devicetree.** Default `BLOB_DB` and
   `BLOB_DB_BACKEND_*` from `DT_HAS_*_ENABLED`; drop `CONFIG_BLOB_DB=y` from
   `prj.conf` files. Test: the tests build each backend by overlay instead of
   by `-DCONFIG_`.
3. **Device and `automount`.** `DEVICE_DT_INST_DEFINE`, `automount`,
   `no-format`. Test: a suite with `automount` that checks `device_is_ready()`,
   and one with a corrupt store plus `no-format` that checks it is false.
4. **Lazy mount and `held_down`.** `ensure_mounted()` in every entry point.
   Test: first call mounts; explicit unmount stays down; power-cut suite
   unchanged.
5. **Lazy rootreg.** Bootstrap on first `rootreg_get_or_create()`. Test: the
   rootreg suite without `rootreg_init()`, plus the foreign-payload refusal.
6. **blobfs fstab and automount.** Test: the blobfs suite through
   `FS_FSTAB_ENTRY` with and without `automount`.
7. **Deferred init with an application key.** Test: a native_sim build with
   `BLOB_DB_UBI_KEY_APP` and `zephyr,deferred-init` that provisions a key and
   then calls `device_init()`.
8. **Remove the boilerplate** from `app/`, the samples and the benchmarks, and
   update `l1_blob_db.md`, `l1_root_registry.md`, `blobfs_fs.h` and P4.

Steps 1 and 2 are worth having even if the rest is declined: they fix B11 and
let a board choose the store's location and backend without touching code.

## 9. As implemented

All eight steps of §8 are in one change. Where the code differs from the
design above, the code is right and this section says why.

| § | Design | Implemented | Why |
|---|---|---|---|
| 3.2 | volumes have `volname`, `volid`, `size` | `volname` and `size` only | zephyr-ubi v0.1.0 assigns volume ids itself; `struct ubi_volume_config` has no id |
| 3.2 | the partition carries `compatible = "kvdb,ubi"` | on native_sim: `"kvdb,ubi", "zephyr,mapped-partition"` | Zephyr main now marks native_sim's partitions `zephyr,mapped-partition`, and that compatible is what makes the node a partition (its id and flash_map entry). QSPI partitions under `fixed-partitions` (nRF5340-DK) take `"kvdb,ubi"` alone, as designed |
| 3.2 | — | `dts/bindings/vendor-prefixes.txt` registers `kvdb` | twister builds with `--edtlib-Werror`, which rejects an unknown vendor prefix |
| 3.3 | a `BUILD_ASSERT` refuses a backend that disagrees with `storage` | an override is honoured: flash_area over a UBI volume uses the UBI device's partition raw; UBI over a plain partition creates volume `"blobdb"` with 4 spare LEBs | CI and `tests/lib/blob_db` build the same overlay on both backends (`-DCONFIG_BLOB_DB_BACKEND_FLASH_AREA=y`), and the benchmarks compare them on one partition. The default still follows devicetree |
| 3.4 | device at the APPLICATION level | POST_KERNEL, `CONFIG_BLOB_DB_INIT_PRIORITY` = 90 | Zephyr refuses devices at the APPLICATION level (`Z_DEVICE_CHECK_INIT_LEVEL`). 90 is after the flash drivers (QSPI NOR: 41, flash simulator: 50); the `storage` phandle makes the flash a devicetree dependency, which `CONFIG_CHECK_INIT_PRIORITIES` checks at link time |
| 3.4 | `<app/lib/blob_db.h>` gets the device | `<app/lib/blob_db_device.h>`, `BLOB_DB_DEVICE` | `blob_db.h` is the contract every blob_db implementation provides; the device is this implementation's configuration |
| 3.7 | after a failed automount "every API call returns the mount's error" | every call retries the mount and returns its error if it fails again | one rule for every case: an unmounted store that was never explicitly unmounted mounts on the next call. A corrupt or foreign store fails the same way each time, so nothing is lost |
| 5 | `BLOB_DB_AUTOFORMAT_ON_CORRUPT` is removed | kept; its default is `n` when the node has `no-format` | the `lib.blob_db.no_autoformat` scenario and existing board files set the symbol directly; keeping it costs nothing, and the devicetree still decides by default |
| 3.6 | blobfs automount in its `SYS_INIT` | a separate `SYS_INIT` at the APPLICATION level | registration stays at POST_KERNEL with the VFS; mounting waits until after the blob_db device |

**Not done:** proposing the UBI binding upstream (§6.3), and multi-instance
(§6.2), both out of scope as designed.

**Where to look.**

| What | Where |
|---|---|
| Bindings | `dts/bindings/mtd/kvdb,ubi.yaml`, `dts/bindings/storage/kvdb,blob-db.yaml`, `dts/bindings/fs/kvdb,fstab,blobfs.yaml` |
| Devicetree lookup | `lib/blob_db/blob_db_dt.h`, used by both backends |
| Kconfig defaults from devicetree | `lib/blob_db/Kconfig` (`KVDB_DT_*` variables) |
| Device, automount | `lib/blob_db/blob_db_device.c`, `include/app/lib/blob_db_device.h` |
| Lazy mount, `held_down` | `ensure_mounted()` in `lib/blob_db/blob_db.c` |
| Lazy rootreg | `rr_load()` in `lib/rootreg/rootreg.c` |
| blobfs fstab | end of `lib/blobfs/blobfs_zephyr_fs.c` |
| No init code in `main()` | `app/` (`automount`), `samples/kvhash/` (mount on first use), `samples/blobfs/` (fstab) |
| Tests | `tests/lib/devicetree/` (one scenario per devicetree variation: lazy, automount, deferred, no-format, raw, fstab), `tests/lib/rootreg` (`test_virgin_store_needs_no_init`) |

The benchmark applications (`app_perf*`, `app_cbor_persondb`) keep their
explicit `blob_db_mount()` / `blob_db_format()` / `blob_db_unmount()` calls
and have no `automount`: they time and reset the store on purpose.
