# blobfs — test suite

Covers blobfs (L3) through its Zephyr filesystem interop shim
(`CONFIG_BLOBFS_FS_INTEROP`), on `native_sim`.

Most of the suite is **Zephyr's own filesystem conformance bodies**, compiled
unmodified from the Zephyr tree (`$ZEPHYR_BASE/tests/subsys/fs/common`) and
pointed at a blobfs mount at `/blob`:

| Body | What it drives |
|---|---|
| `test_fs_basic.c` | mount, create/write/stat, read back, seek (SET/CUR/END), truncate, unlink, sync, unmount + remount persistence |
| `test_fs_open_flags.c` | the `fs_open()` flag matrix — access modes, `FS_O_CREATE`, `FS_O_APPEND`, `FS_O_TRUNC` and their combinations |
| `test_fs_util.c` | helpers the two above use |

These are the same bodies FAT and littlefs answer to, so passing them is what
makes "blobfs behaves like a Zephyr filesystem" a result rather than a claim.

`test_fs_dirops.c` is deliberately **not** compiled in: v1 is a flat namespace
and `map_ops` has no iterate op, so `mkdir`/`readdir` are unimplemented. The
local tests in `src/main.c` assert that those paths report `-ENOTSUP` through
the VFS instead of failing in some other way, and cover the glue's own edges:
the flat namespace, the one-payload file size cap, rename, and the
single-mount limit.

```shell
west twister -T key_value_db/tests/lib/blobfs -p native_sim -v --inline-logs
```
