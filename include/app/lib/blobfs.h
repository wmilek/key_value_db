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
 * @brief File access over Map containers: name -> dirent -> body blob.
 *
 * blobfs stores each directory as a Map container instance
 * (`name -> dirent`) and each file body as its own blob: a file's dirent
 * records the body's stable i-node id, and the body's payload length *is*
 * the file size. Reads and writes go straight to blob_db's pread/pwrite
 * calls, so every mutation is one crash-atomic L1 operation. A
 * subdirectory's dirent records that directory's Map root instead.
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
 * The namespace is a **tree**: @ref blobfs_mkdir makes directories,
 * @ref blobfs_next_id lists them, and @ref blobfs_unlink removes empty
 * ones. A path is a series of names separated by `/`, with or without a
 * leading slash (`"cfg/net"` and `"/cfg/net"` name the same file), and
 * repeated slashes collapse. Each component is capped at
 * @ref BLOBFS_NAME_MAX, and resolving a path costs one Map lookup per
 * directory on the way; there is no cap on depth or total length. A
 * missing directory on the way reports -ENOENT and a file on the way
 * -ENOTDIR. `"."`/`".."` are refused (-EINVAL) so every stored name stays
 * addressable through path-normalizing clients. A trailing `/` asks for a
 * directory: on a file it reports -ENOTDIR.
 *
 * Rename moves an entry **within one directory** only; across directories
 * it reports -EXDEV.
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

/** Longest name of one path component, in bytes (excluding the NUL). */
#define BLOBFS_NAME_MAX CONFIG_BLOBFS_MAX_NAME_LEN

/** Largest file body, in bytes: one blob payload (see @ref blobfs_v1). */
#define BLOBFS_FILE_MAX CONFIG_BLOB_DB_MAX_PAYLOAD_LEN

/** What a directory entry is. */
enum blobfs_entry_type {
	BLOBFS_ENTRY_FILE, /**< a file */
	BLOBFS_ENTRY_DIR,  /**< a directory */
};

/** What @ref blobfs_stat reports about an entry. */
struct blobfs_stat {
	enum blobfs_entry_type type;
	size_t size; /**< file size in bytes; 0 for a directory */
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
 * @retval -EEXIST       the name is already taken (by a file or directory)
 * @retval -EISDIR       @p path names the root directory, or ends in '/'
 * @retval -EINVAL       a component is "." or ".."
 * @retval -ENAMETOOLONG a component longer than @ref BLOBFS_NAME_MAX
 * @retval -ENOENT       a directory on the way does not exist
 * @retval -ENOTDIR      a component on the way is a file
 * @retval -ENODEV       not mounted
 * @retval -ENOSPC       store full
 * @retval -EIO          flash error
 */
int blobfs_create(const char *path);

/**
 * @brief Create an empty directory.
 *
 * The new directory is its own Map, built before its name is published: a
 * crash in between leaves an unreachable empty Map, never a dangling name.
 *
 * @retval 0             created
 * @retval -EEXIST       the name is already taken (by a file or directory),
 *                       or @p path names the root directory
 * @retval -EINVAL       a component is "." or ".."
 * @retval -ENAMETOOLONG a component longer than @ref BLOBFS_NAME_MAX
 * @retval -ENOENT       a directory on the way does not exist
 * @retval -ENOTDIR      a component on the way is a file
 * @retval -ENODEV       not mounted
 * @retval -ENOSPC       store full
 * @retval -EIO          flash error
 */
int blobfs_mkdir(const char *path);

/**
 * @brief Resolve a name to the file's body id — its durable identity.
 *
 * @retval 0        found; *body_id filled
 * @retval -ENOENT  no such file
 * @retval -EISDIR  @p path names a directory
 * @retval -ENOTDIR a component on the way is a file
 * @retval -EINVAL  bad arguments
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
 * @brief Report whether @p path is a file or a directory, and a file's
 *        size. The root directory is a directory.
 *
 * @retval 0        found; @p st filled
 * @retval -ENOENT  no such entry
 * @retval -ENOTDIR a component on the way is a file
 * @retval -EINVAL  st is NULL
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
 * @brief Remove a file or an empty directory.
 *
 * The name is dropped from its directory first (one atomic Map mutation),
 * then the body blob is deleted, or the directory's Map destroyed. A crash
 * between the two leaves an unreachable body or Map — garbage, never a
 * dangling name. Outstanding body ids for a file report -ENOENT from then
 * on, and so do directory ids for a directory (@ref blobfs_next_id).
 *
 * @retval 0          removed
 * @retval -ENOENT    no such entry
 * @retval -ENOTEMPTY @p path is a directory that still holds entries
 * @retval -EBUSY     @p path names the root directory
 * @retval -EIO       flash error
 */
int blobfs_unlink(const char *path);

/**
 * @brief Rename a file or directory within its directory.
 *
 * Atomic against power loss via the recorded intent (see @ref
 * blobfs_identity): after a crash at any point, mount resolves the rename
 * so that exactly one of the two names exists. The entry keeps its id —
 * a file its body id, a directory its Map root, and with it everything
 * inside — so ids resolved before the rename remain valid. Refuses to
 * overwrite an existing destination.
 *
 * @retval 0       renamed
 * @retval -ENOENT @p from does not exist, or neither does @p to's directory
 * @retval -EEXIST @p to already exists
 * @retval -EXDEV  @p from and @p to are in different directories
 * @retval -EBUSY  either path names the root directory
 * @retval -EIO    flash error
 */
int blobfs_rename(const char *from, const char *to);

/**
 * @brief Resolve a directory path to the directory's id, for listing it.
 *
 * `""` and `"/"` name the root directory. Nothing is opened and nothing
 * has to be closed: a listing is a series of @ref blobfs_next_id calls on
 * the id. Like a body id, the directory id is the directory's identity: it
 * follows the directory through a rename and reports -ENOENT once the
 * directory is removed.
 *
 * @retval 0        *dir_id filled
 * @retval -ENOTDIR @p path names a file, or a component on the way is one
 * @retval -ENOENT  no such directory
 * @retval -EINVAL  @p path or @p dir_id is NULL, or a component is "."/".."
 * @retval -ENAMETOOLONG a component longer than @ref BLOBFS_NAME_MAX
 * @retval -ENODEV  not mounted
 * @retval -EIO     flash error
 */
int blobfs_opendir(const char *path, uint64_t *dir_id);

/**
 * @brief Return the entry that follows @p after in directory @p dir_id.
 *
 * Listing is stateless: the name is the cursor. Start with @p after empty
 * (`""`) and pass back each name returned until the call reports -ENODATA.
 * A listing holds nothing open, so it may be resumed from a saved name at
 * any time, after a reboot included.
 *
 * The order is the directory Map's own (see @ref map_ops `next`): not
 * sorted and not creation order, but fixed by the names alone. Every file
 * present for the whole listing is returned exactly once. Entries may be
 * created, written, renamed and unlinked between calls, including the one
 * just returned; one created, unlinked or renamed during the listing may
 * or may not appear, depending on where its name falls. @p after need not
 * exist any more. Entries are listed by name only: a subdirectory is
 * returned as one entry, never descended into.
 *
 * @param dir_id the directory, from @ref blobfs_opendir
 * @param after  cursor: the previous name returned, or "" to start
 * @param name   receives the next entry's name, NUL-terminated
 * @param st     receives that entry's type and size (may be NULL)
 *
 * @retval 0             next entry returned
 * @retval -ENODATA      no entry follows @p after (an empty directory
 *                       answers this to the first call)
 * @retval -ENOENT       the directory has been removed
 * @retval -EINVAL       @p after or @p name is NULL
 * @retval -ENAMETOOLONG @p after is longer than @ref BLOBFS_NAME_MAX
 * @retval -ENODEV       not mounted
 * @retval -EIO          flash error or a corrupt directory entry
 */
int blobfs_next_id(uint64_t dir_id, const char *after,
		   char name[BLOBFS_NAME_MAX + 1], struct blobfs_stat *st);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_BLOBFS_H_ */
