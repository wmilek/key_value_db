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

`test_fs_dirops.c` is deliberately **not** compiled in yet: it calls
`fs_statvfs`, which blob_db cannot answer, and it expects `rename` to move
entries between directories and to replace an existing file, neither of which
blobfs does. The local tests in `src/main.c` cover the same ground for what
blobfs does support — nested `mkdir`, path walking, removing empty
directories (and refusing non-empty ones, without leaking blobs), listing
(including deleting while listing, listing across a remount, and handles that
follow a renamed directory), renaming a directory with its contents, and
`-EXDEV` across directories — plus the glue's own edges: `statvfs` reporting
`-ENOTSUP`, the one-payload file size cap, mid-rename crash recovery in the
root and in a subdirectory, a rename that fails part way in a running
session (via kvhash's write-cut test hook) being finished before the next
unlink or rename, mounting a store whose meta predates directories and its
upgrade at the first `mkdir`, removing a directory whose Map is already gone,
a listing stepping past a broken entry, and the single-mount limit.

```shell
west twister -T key_value_db/tests/lib/blobfs -p native_sim -v --inline-logs
```
