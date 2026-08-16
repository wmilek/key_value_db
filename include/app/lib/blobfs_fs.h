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
 * @section blobfs_fs_scope What the driver supports
 *
 * Everything a file needs: `fs_open` (with `FS_O_CREATE`, `FS_O_APPEND`,
 * `FS_O_TRUNC` and the access modes), `fs_read`, `fs_write`, `fs_seek`,
 * `fs_tell`, `fs_truncate`, `fs_sync`, `fs_close`, `fs_stat`, `fs_unlink`
 * and `fs_rename`.
 *
 * Not supported, and left NULL so the VFS reports -ENOTSUP: `fs_mkdir`,
 * `fs_opendir`/`fs_readdir`/`fs_closedir` (v1 is a flat namespace and the
 * Map shape has no iterate op) and `fs_statvfs` (blob_db exposes no
 * free-space accounting). `fs_sync` is a no-op that succeeds: every write
 * is already durable when it returns.
 *
 * @section blobfs_fs_limits Limits
 *
 * One mount point at a time; a second `fs_mount()` of this type reports
 * -EBUSY. File names are capped at @ref BLOBFS_NAME_MAX and file bodies at
 * @ref BLOBFS_FILE_MAX (one blob payload) — a write past that reports
 * -ENOSPC. Up to `CONFIG_BLOBFS_FS_MAX_OPEN_FILES` files may be open at
 * once; beyond that `fs_open` reports -EMFILE.
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
