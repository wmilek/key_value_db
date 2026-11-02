# Design change proposal — kvdb tools: one shell, on the device and on images

Status: **proposal**, step 1 landed (`app_diag/`) · 2026-11-02
· Builds on: `lib/blob_db/blob_db_shell.c`, `include/app/lib/blob_db_inspect.h`,
  the store seam `lib/blob_db/blob_db_store.h`
· Governed by `doc/principles.md`

**The ask.** A set of tools to create and manage images of the store, check
them (fsck), and analyse them offline. Part of the set runs on the device: an
application that does nothing but expose a shell. Its commands can go further
than a product needs: maintenance, repair and deliberate damage.

**Short answer.** Every command is a Zephyr shell command group, kept in the
library it belongs to, as `blob_db` already is. One application, `app_diag`,
mounts nothing on its own and exposes the shell. Built for a board, it is the
on-device diagnostics firmware. Built for `native_sim`, with the store on a
simulated flash device whose backing file is a partition image, it is the
offline tool. Both builds run the same code, so the host tool cannot drift
from the format the device writes.

## 1. The idea

### 1.1 One command set, two places to run it

| Build | What it is | Reached through |
|---|---|---|
| Board (nRF5340-DK first) | on-device diagnostics | console UART; later RTT, USB CDC, MCUmgr shell |
| `native_sim` | offline tool on an image file | the invoking terminal, or a pipe |

Zephyr's flash simulator already takes its backing file from the command
line. If the storage partition starts at offset 0 of a simulated device of its
own and fills it, that file is byte for byte a partition image. A dump of the
device opens unchanged, and an image produced on the host can be programmed
back.

The alternative, a separate host parser of the format (Python, say), was
considered and left out. It would be a second implementation of the format,
and every format change would have to land twice. It also could not answer
what recovery *would* do, because it does not run the recovery code. It may
come back later as an independent check of the format spec, not as the tool.

### 1.2 Commands live in the libraries

A command group sits next to the module it serves and is gated by that
module's Kconfig (`BLOB_DB_SHELL` today). An application such as
`app_cbor_persondb` adds its own groups for its own data model. The
diagnostics app chooses which groups to enable. It adds none of its own.

Planned groups, by layer (none beyond `blob_db`, `flash`, `kernel` and
`device` exists yet):

```
blob_db  info · stats · buckets · bucket · id · dump · maintain · iostats    exists
         ls · get · verify                                                   later
ubi      info · peb · ec-hist (wear)                                         L0
rootreg  ls · get · set · rm                                                 L1½
kvdb     stores · ls · get · set · rm · stats                                L3
blobfs   tree · ls · cat · put · rm                                          L3
fsck     [--level l0|l1|l2|l3] [--repair]
image    export · import · diff · mkimage <manifest>                         mostly host
danger   unlock · format · erase-all · set-next-id · corrupt · tear
```

### 1.3 Mounting is never implicit

`blob_db_mount()` writes. It finishes an interrupted compaction from the
scratch seal, formats a partition whose two master sectors read as erased,
and on UBI attaches or formats the volume. A diagnostic tool that mounts on
start can therefore change, or destroy, the state it was started to examine.
So:

- Nothing mounts at boot unless the build asks for it
  (`CONFIG_APP_DIAG_MOUNT_AT_BOOT`, default `n`).
- `BLOB_DB_AUTOFORMAT_ON_CORRUPT` and `BLOB_DB_MAINT_WORK` are off in the
  diagnostics build. blobfs, whose file-system registration is another way to
  a mount, is not built.
- Later steps add an explicit `mount` command, and a copy-on-write store
  under the store seam (§2, step 3). Reads come from the image; writes go to a
  RAM overlay that is discarded unless committed. "What would recovery do to
  this image?" then becomes a question that is safe to ask.

### 1.4 Geometry and format are build-time facts

Erase-block size, write alignment, partition size, the backend, the UBI key
and the format options (`BLOB_DB_LARGE_PAYLOADS`, `BLOB_DB_MAX_PAYLOAD_LEN`)
are compiled in. The host tool is therefore built once per target profile,
and it must refuse an image whose master disagrees with the build
(`format_major`, `format_minor`) rather than misread it. Step 1 carries one
profile, the nRF5340-DK's MX25R64 partition. An image sealed with
`BLOB_DB_UBI_KEY_APP` needs its key supplied to the host build.

### 1.5 fsck has layers, and the top one needs the containers

- **L0:** UBI headers, volume table, erase counters.
- **L1:** slot CRCs, master generations, ids bound twice, seal consistency.
  `blob_db_iterate()` and the inspect hooks cover most of it.
- **L1½ to L3:** reachability. Mark everything reachable from id 1 through
  rootreg and each container, then sweep: a bound id nothing reaches is a
  leaked i-node.

Reachability needs one new contract: per container type, a function that
walks the ids an instance references. An application data model provides its
own walker, the same way it provides its own shell group. This is what the
single superblock per application in `doc/application_model.md` makes
possible.

### 1.6 Dangerous commands

Commands that only a diagnostics or test build should carry (format,
erase-all, setting `next_id`, flipping a bit at an offset, tearing an
operation at a chosen step) sit behind two gates:

- **Build time:** one Kconfig option, off by default. A production firmware
  carries at most the read-only subset.
- **Run time:** an `unlock` command must be run before any of them.

Fault injection reuses `tests/support/flash_fault`.

### 1.7 Images in and out

- **mkimage:** builds a partition image for a target profile from a manifest
  (kvdb stores and their keys and values, a directory tree for blobfs, raw
  blobs). Used for factory provisioning and for test fixtures. The output
  must be reproducible: the same manifest gives the same bytes.
- **Getting an image off a device:** a debug-probe read of the partition
  range. Streaming it over the shell is the slow fallback.
- **diff:** compares two images at the logical level: ids, keys and paths
  added, changed or removed, not bytes. Run against the replay bench in
  `kvdb-workloads`, it shows what a workload did to the store.

## 2. Plan

| Step | Content | New commands |
|---|---|---|
| **1** (landed) | `app_diag/`: shell only, existing groups (`blob_db`, `blob_db iostats`, `flash`, `kernel`, `device`); no mount unless `CONFIG_APP_DIAG_MOUNT_AT_BOOT=y`; nRF5340-DK build plus a `native_sim` build whose store flash is a partition image (`--kvdb=<file>`); twister runs both variants | none |
| 2 | Explicit `blob_db mount` / `unmount`; read-only groups for `rootreg`, `kvdb`, `blobfs` | yes |
| 3 | Copy-on-write store under the seam, so a mount on an image writes nothing unless committed; `west kvdb` wrapper (build per profile, run on an image, batch input, machine-readable output) | wrapper only |
| 4 | fsck L0/L1, then container walkers and reachability | `fsck` |
| 5 | `danger` group behind a Kconfig option and `unlock`; fault injection | `danger` |
| 6 | mkimage from a manifest | `image mkimage` |
| 7 | Logical diff, wear analysis, workload replay against an image | `image diff`, `ubi ec-hist` |

Each step stands on its own: step 1 is useful as soon as an image exists, and
no later step changes what an earlier one does.

## 3. Step 1 as built

- `app_diag/src/main.c` logs the backend and the storage partition, mounts
  only with `CONFIG_APP_DIAG_MOUNT_AT_BOOT`, and returns. The shell does the
  rest.
- `app_diag/boards/native_sim.overlay` puts `storage_partition` on a second
  `zephyr,sim-flash` device labelled `kvdb`: 8 MB, 64 KB erase blocks, 4 B
  writes. The board's own flash keeps its other partitions.
- `app_diag/boards/native_sim.conf` puts the shell on the invoking terminal
  (`CONFIG_UART_NATIVE_PTY_0_ON_STDINOUT`), so commands can be piped in.
- Measured on `native_sim`: a session that does not mount (`flash read`,
  `blob_db iostats`, `blob_db info` answering "not mounted") leaves the
  image's md5 unchanged. With the opt-in, a blank image is formatted (UBI,
  then blob_db) and mounted, and a second mount of the same image leaves it
  unchanged.

## 4. Open questions

- **Target profiles:** which ones beyond the DK's 64 KB external NOR? Internal
  flash at 4 KB and the nRF52840-DK are the obvious next ones.
- **Production subset:** should a product firmware carry the read-only
  groups, or only diagnostics builds?
- **Format spec check:** is an independent parser wanted later as a check of
  `doc/impl/l1_bucketlog.md`?
