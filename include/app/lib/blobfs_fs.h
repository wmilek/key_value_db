/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BLOBFS_FS_H_
#define APP_LIB_BLOBFS_FS_H_

#include <zephyr/fs/fs.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_blobfs_fs blobfs — Zephyr filesystem interop
 * @ingroup lib
 * @{
 *
 * @brief Registers blobfs with Zephyr's virtual file system, so the stack is
 *        reachable through the ordinary `fs_*` API.
 *
 * The driver registers itself at boot (`SYS_INIT`, `POST_KERNEL`); a caller
 * only has to mount it:
 *
 * ```c
 * static struct fs_mount_t mnt = {
 *         .type = BLOBFS_FS_TYPE,
 *         .mnt_point = "/blob",
 * };
 *
 * fs_mount(&mnt);
 * fs_open(&file, "/blob/wifi.ssid", FS_O_CREATE | FS_O_RDWR);
 * ```
 *
 * Mounting brings the stack up on its own — `blob_db_mount()` and
 * `rootreg_init()` are called if the application has not already run them —
 * and unmounting releases only blobfs, leaving blob_db mounted for whatever
 * else shares it (kvdb, for instance).
 *
 * @section blobfs_fs_fstab Mounting from devicetree
 *
 * Like littlefs, the mount can be described in devicetree instead
 * (dts/bindings/fs/kvdb,fstab,blobfs.yaml), and with `automount` it is
 * mounted during boot, so `main()` calls nothing:
 *
 * ```dts
 * fstab {
 *         compatible = "zephyr,fstab";
 *         blobfs0: blobfs {
 *                 compatible = "kvdb,fstab,blobfs";
 *                 mount-point = "/blob";
 *                 db = <&blobdb0>;
 *                 automount;
 *         };
 * };
 * ```
 *
 * The driver defines the node's `struct fs_mount_t`; reach it with
 * `FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(blobfs0))` and
 * `&FS_FSTAB_ENTRY(DT_NODELABEL(blobfs0))` to mount it yourself (without
 * `automount`) or to unmount it. `read-only` is enforced by the VFS;
 * `no-format` belongs on the `kvdb,blob-db` node, not here. Automount runs
 * at the APPLICATION init level and only logs a failure, as littlefs does;
 * `fs_stat()` on the mount point tells whether it is there.
 *
 * @section blobfs_fs_scope What the driver supports
 *
 * Everything a file needs: `fs_open` (with `FS_O_CREATE`, `FS_O_APPEND`,
 * `FS_O_TRUNC` and the access modes), `fs_read`, `fs_write`, `fs_seek`,
 * `fs_tell`, `fs_truncate`, `fs_sync`, `fs_close`, `fs_stat`, `fs_unlink`
 * and `fs_rename`; and for directories `fs_mkdir`,
 * `fs_opendir`/`fs_readdir`/`fs_closedir`, `fs_stat` (reporting
 * `FS_DIR_ENTRY_DIR`) and `fs_unlink` of an empty directory (-ENOTEMPTY
 * otherwise). `fs_rename` stays within one directory: across directories it
 * reports -EXDEV.
 *
 * Not supported, and left NULL so the VFS reports -ENOTSUP: `fs_statvfs`
 * (blob_db exposes no free-space accounting). `fs_sync` is a no-op that
 * succeeds: every write is already durable when it returns.
 *
 * @section blobfs_fs_readdir Listing
 *
 * `fs_readdir` returns entries in the directory Map's order — not sorted,
 * not creation order (see @ref blobfs_next_id) — and never "." or "..".
 * Entries may be created, unlinked and renamed while a listing is open:
 * every entry present for the whole listing is returned exactly once, so
 * deleting each one as it is listed empties the directory. `fs_opendir`
 * on a file reports -ENOTDIR, on a missing path -ENOENT. A listing binds
 * to the directory, not its path, like a file handle: it follows a rename
 * and reports -ENOENT once the directory is removed. With blobfs
 * mounted at "/", `fs_opendir("/")` lists the VFS mount points instead:
 * the VFS core answers it without calling the driver.
 *
 * @section blobfs_fs_semantics Handles and threads
 *
 * Every op serializes on an internal mutex, so the `fs_*` API may be called
 * from multiple threads (the shell, MCUmgr's fs_mgmt workqueue, the
 * application) even though the storage stack below is single-threaded by
 * contract. The mutex does not cover callers using the blobfs API directly
 * alongside the shim.
 *
 * A handle binds to the file's identity (its body id), not its name:
 * it follows the file through `fs_rename`, and after `fs_unlink` its
 * reads and writes report -ENOENT — never touching whatever file takes
 * the name next. Unmounting with files still open orphans their handles:
 * each keeps its slot, reports -EBADF, and is released by `fs_close`.
 *
 * @section blobfs_fs_limits Limits
 *
 * One mount point at a time; a second `fs_mount()` of this type reports
 * -EBUSY. Each path component is capped at @ref BLOBFS_NAME_MAX and file
 * bodies at
 * @ref BLOBFS_FILE_MAX (one blob payload) — a write past that reports
 * -ENOSPC. Up to `CONFIG_BLOBFS_FS_MAX_OPEN_FILES` files and
 * `CONFIG_BLOBFS_FS_MAX_OPEN_DIRS` listings may be open at once; beyond
 * that `fs_open` or `fs_opendir` reports -EMFILE. Unmounting orphans open
 * listings the same way as open files.
 */

/**
 * @brief Filesystem type to put in `struct fs_mount_t::type`.
 *
 * Offset within the VFS's external range by `CONFIG_BLOBFS_FS_TYPE_OFFSET`,
 * so a build carrying several out-of-tree filesystems can keep them apart.
 */
#define BLOBFS_FS_TYPE (FS_TYPE_EXTERNAL_BASE + CONFIG_BLOBFS_FS_TYPE_OFFSET)

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_BLOBFS_FS_H_ */
