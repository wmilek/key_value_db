/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BLOBFS_H_
#define APP_LIB_BLOBFS_H_

#include <stddef.h>
#include <stdint.h>

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
 * i-node id, and the body's payload length *is* the file size. Reads and
 * writes go straight to blob_db's pread/pwrite calls, so every mutation is
 * one crash-atomic L1 operation.
 *
 * @section blobfs_identity Names and identity
 *
 * The **body id is a file's durable identity**; names are directory entries
 * pointing at it. @ref blobfs_lookup turns a name into that id, and the
 * `_id` calls operate on it directly — which is what a handle layer should
 * bind to, because it gives POSIX-shaped lifetimes for free:
 *
 * - @ref blobfs_rename re-points names and never touches the id, so an id
 *   obtained before a rename still addresses the same file after it.
 * - @ref blobfs_unlink deletes the body, so `_id` calls on an unlinked
 *   file's id report -ENOENT from then on (there is no orphan-until-close
 *   state; blob_db deletes immediately).
 *
 * Rename is atomic against power loss: an intent recorded in the filesystem
 * metadata before the directory is touched lets @ref blobfs_mount roll a
 * half-done rename forward or back, so no caller ever observes two names
 * bound to one body.
 *
 * @section blobfs_v1 v1 scope
 *
 * The namespace is **flat**: one directory, holding files. `mkdir` and
 * directory iteration are not provided — the Map shape's enumeration op
 * (@ref map_ops `next`) is not wired into blobfs yet, and nesting waits on
 * it. Paths are therefore a single name, with or without a leading
 * slash (`"cfg"` and `"/cfg"` name the same file); an embedded `/` reports
 * -ENOENT because no such directory can exist, and `"."`/`".."` are
 * refused (-EINVAL) so every stored name stays addressable through
 * path-normalizing clients.
 *
 * A file body is a single inline blob payload, so
 * `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` caps file size; a write past that cap
 * reports -ENOSPC. blob_db's segmented objects are what will lift the cap.
 *
 * @section blobfs_concurrency Concurrency
 *
 * Single-threaded, inheriting the blob_db v1 contract: the caller
 * serializes all calls. The Zephyr `fs_file_system_t` shim
 * (`CONFIG_BLOBFS_FS_INTEROP`, see `include/app/lib/blobfs_fs.h`) carries
 * its own mutex and is safe to call from multiple threads — but it does not
 * cover callers using this API directly alongside it.
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
 * Finds (or registers) the root directory through the root registry, binds
 * it, and completes any rename a crash left half done (see @ref
 * blobfs_identity). Requires blob_db_mount() and rootreg_init() to have
 * run.
 *
 * @retval 0         mounted
 * @retval -EALREADY already mounted
 * @retval -EINVAL   the directory cannot hold BLOBFS_NAME_MAX-sized entries
 *                   under the configured blob payload cap (a build
 *                   configuration mistake — deterministic on every boot)
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
 * @retval -EINVAL       @p path is "." or ".."
 * @retval -ENAMETOOLONG name longer than @ref BLOBFS_NAME_MAX
 * @retval -ENOENT       @p path contains a directory component (v1 is flat)
 * @retval -ENODEV       not mounted
 * @retval -ENOSPC       store full
 * @retval -EIO          flash error
 */
int blobfs_create(const char *path);

/**
 * @brief Resolve a name to the file's body id — its durable identity.
 *
 * @retval 0       found; *body_id filled
 * @retval -ENOENT no such file
 * @retval -EINVAL bad arguments
 */
int blobfs_lookup(const char *path, uint64_t *body_id);

/**
 * @brief Report a file's size by body id.
 *
 * @retval 0       *size filled
 * @retval -ENOENT the file was unlinked
 * @retval -EINVAL size is NULL
 */
int blobfs_size_id(uint64_t body_id, size_t *size);

/**
 * @brief Read at most @p len bytes from @p off, by body id.
 *
 * A read that starts at or past EOF is a short read of 0 bytes, not an
 * error.
 *
 * @retval 0       read (possibly short); @p out_read set
 * @retval -ENOENT the file was unlinked
 * @retval -EINVAL bad arguments
 */
int blobfs_read_id(uint64_t body_id, size_t off, void *buf, size_t len,
		   size_t *out_read);

/**
 * @brief Write @p len bytes at @p off, by body id, extending as needed.
 *
 * A write starting past EOF zero-fills the gap. The whole call is one
 * crash-atomic L1 write: on the next mount the file holds either its
 * previous content or the new content, never a mixture. A zero-length
 * write succeeds only if the file still exists.
 *
 * @retval 0       written
 * @retval -ENOENT the file was unlinked
 * @retval -ENOSPC the result would exceed @ref BLOBFS_FILE_MAX, or the
 *                 store is full
 * @retval -EIO    flash error
 */
int blobfs_write_id(uint64_t body_id, size_t off, const void *buf, size_t len);

/**
 * @brief Resize the file, by body id. Growing zero-fills; shrinking
 *        discards the tail.
 *
 * @retval 0       resized
 * @retval -ENOENT the file was unlinked
 * @retval -ENOSPC @p size exceeds @ref BLOBFS_FILE_MAX
 * @retval -EIO    flash error
 */
int blobfs_truncate_id(uint64_t body_id, size_t size);

/**
 * @brief Report a file's size. Path form of @ref blobfs_size_id.
 *
 * @retval 0       found; @p st filled
 * @retval -ENOENT no such file
 * @retval -EINVAL st is NULL
 */
int blobfs_stat(const char *path, struct blobfs_stat *st);

/** @brief Read by path — @ref blobfs_lookup + @ref blobfs_read_id. */
int blobfs_read(const char *path, size_t off, void *buf, size_t len,
		size_t *out_read);

/** @brief Write by path — @ref blobfs_lookup + @ref blobfs_write_id. */
int blobfs_write(const char *path, size_t off, const void *buf, size_t len);

/** @brief Resize by path — @ref blobfs_lookup + @ref blobfs_truncate_id. */
int blobfs_truncate(const char *path, size_t size);

/**
 * @brief Remove a file.
 *
 * The name is dropped from the directory first (one atomic Map mutation),
 * then the body blob is deleted. A crash between the two leaves an
 * unreachable body — garbage a later compaction reclaims — never a dangling
 * name. Outstanding body ids for the file report -ENOENT from then on.
 *
 * @retval 0       removed
 * @retval -ENOENT no such file
 * @retval -EIO    flash error
 */
int blobfs_unlink(const char *path);

/**
 * @brief Rename a file within the (single) directory.
 *
 * Atomic against power loss via the recorded intent (see @ref
 * blobfs_identity): after a crash at any point, mount resolves the rename
 * so that exactly one of the two names exists. The file keeps its body id,
 * so ids resolved before the rename remain valid. Refuses to overwrite an
 * existing destination.
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
