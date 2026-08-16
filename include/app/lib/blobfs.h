/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BLOBFS_H_
#define APP_LIB_BLOBFS_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_blobfs blobfs — path/file interface (L3)
 * @ingroup lib
 * @{
 *
 * @brief File access over a Map container: name -> dirent -> body blob.
 *
 * blobfs stores a directory as a Map container instance (`name -> dirent`)
 * and each file body as its own blob: the dirent records the body's stable
 * i-node id, and the body's payload length *is* the file size. Nothing else
 * is persisted, so a file's bytes are exactly one atomic `blob_db_update()`
 * away from durable — writes inherit L1's crash atomicity unchanged.
 *
 * @section blobfs_v1 v1 scope
 *
 * The namespace is **flat**: one directory, holding files. `mkdir` and
 * directory iteration are not provided — iteration needs an `iterate` op on
 * @ref map_ops that the shipped Map shape does not define, and nesting waits
 * on that. Paths are therefore a single name, with or without a leading
 * slash (`"cfg"` and `"/cfg"` name the same file); an embedded `/` reports
 * -ENOENT because no such directory can exist.
 *
 * A file body is a single blob payload, so `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN`
 * caps file size; a write past that cap reports -ENOSPC. Chunked bodies
 * (`seq`) lift the cap and are deferred with it.
 *
 * @section blobfs_api Handle-free API
 *
 * There is no open/close and no file-position state: reads and writes carry
 * an explicit offset (pread/pwrite style), so there is no cursor ownership
 * and no unlink-while-open question. The Zephyr `fs_file_system_t` shim
 * (`CONFIG_BLOBFS_FS_INTEROP`) synthesizes handles on top of this API — see
 * `include/app/lib/blobfs_fs.h`.
 *
 * @section blobfs_concurrency Concurrency
 *
 * Single-threaded, inheriting the blob_db v1 contract: the caller serializes
 * all calls.
 *
 * See doc/layers/l3_interfaces.md for the full design.
 */

/** Longest file name, in bytes (excluding the NUL). */
#define BLOBFS_NAME_MAX CONFIG_BLOBFS_MAX_NAME_LEN

/** Largest file body, in bytes: one blob payload (see @ref blobfs_v1). */
#define BLOBFS_FILE_MAX CONFIG_BLOB_DB_MAX_PAYLOAD_LEN

/** What @ref blobfs_stat reports about a file. */
struct blobfs_stat {
	size_t size; /**< file size in bytes */
};

/**
 * @brief Attach the filesystem, creating it on first use.
 *
 * Finds (or registers) the root directory through the root registry and
 * binds it. Requires blob_db_mount() and rootreg_init() to have run.
 *
 * @retval 0         mounted
 * @retval -EALREADY already mounted
 * @retval -ENODEV   blob_db not mounted / registry not bootstrapped
 * @retval -ENOSPC   registry full, or the directory could not be created
 * @retval -EIO      flash error or corrupt metadata
 */
int blobfs_mount(void);

/**
 * @brief Detach the filesystem. Idempotent.
 *
 * Drops the bound root only — blob_db stays mounted (other users may share
 * it) and nothing on flash is touched.
 *
 * @retval 0 always
 */
int blobfs_unmount(void);

/**
 * @brief Create an empty file.
 *
 * @retval 0             created
 * @retval -EEXIST       the name is already taken
 * @retval -EISDIR       @p path names the root directory
 * @retval -ENAMETOOLONG name longer than @ref BLOBFS_NAME_MAX
 * @retval -ENOENT       @p path contains a directory component (v1 is flat)
 * @retval -ENODEV       not mounted
 * @retval -ENOSPC       store full
 * @retval -EIO          flash error
 */
int blobfs_create(const char *path);

/**
 * @brief Report a file's size.
 *
 * @retval 0       found; @p st filled
 * @retval -ENOENT no such file
 * @retval -EINVAL st is NULL
 */
int blobfs_stat(const char *path, struct blobfs_stat *st);

/**
 * @brief Read at most @p len bytes from @p off.
 *
 * A read that starts at or past EOF is a short read of 0 bytes, not an
 * error.
 *
 * @param path     file name
 * @param off      byte offset to read from
 * @param buf      (out) destination buffer
 * @param len      capacity of @p buf, in bytes
 * @param out_read (out) bytes actually copied
 *
 * @retval 0       read (possibly short); @p out_read set
 * @retval -ENOENT no such file
 * @retval -EINVAL bad arguments
 */
int blobfs_read(const char *path, size_t off, void *buf, size_t len,
		size_t *out_read);

/**
 * @brief Write @p len bytes at @p off, extending the file as needed.
 *
 * A write starting past EOF zero-fills the gap. The whole call is one
 * `blob_db_update()`, so it is atomic against power loss: on the next mount
 * the file holds either its previous content or the new content, never a
 * mixture.
 *
 * @retval 0       written
 * @retval -ENOENT no such file
 * @retval -ENOSPC the result would exceed @ref BLOBFS_FILE_MAX, or the store
 *                 is full
 * @retval -EIO    flash error
 */
int blobfs_write(const char *path, size_t off, const void *buf, size_t len);

/**
 * @brief Resize the file. Growing zero-fills; shrinking discards the tail.
 *
 * @retval 0       resized
 * @retval -ENOENT no such file
 * @retval -ENOSPC @p size exceeds @ref BLOBFS_FILE_MAX
 * @retval -EIO    flash error
 */
int blobfs_truncate(const char *path, size_t size);

/**
 * @brief Remove a file.
 *
 * The name is dropped from the directory first (one atomic Map mutation),
 * then the body blob is deleted. A crash between the two leaves an
 * unreachable body — garbage a later compaction reclaims — never a dangling
 * name.
 *
 * @retval 0       removed
 * @retval -ENOENT no such file
 * @retval -EIO    flash error
 */
int blobfs_unlink(const char *path);

/**
 * @brief Rename a file within the (single) directory.
 *
 * Binds the new name to the same body, then drops the old one. A crash
 * between the two leaves both names bound to one body; content is never
 * lost. Refuses to overwrite an existing destination.
 *
 * @retval 0       renamed
 * @retval -ENOENT @p from does not exist
 * @retval -EEXIST @p to already exists
 * @retval -EIO    flash error
 */
int blobfs_rename(const char *from, const char *to);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_BLOBFS_H_ */
