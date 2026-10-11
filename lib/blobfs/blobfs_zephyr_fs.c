/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blobfs <-> Zephyr VFS glue.
 *
 * Registers a struct fs_file_system_t whose ops forward to blobfs. All this
 * file owns is what the VFS needs and blobfs deliberately does not have:
 *
 * - A file handle. It binds to the file's *body id* — its durable identity —
 *   not its name, so a handle follows the file through a rename and fails
 *   cleanly (-ENOENT) once the file is unlinked, instead of silently
 *   attaching to whatever file owns the name next.
 * - A lock. The stack below is single-threaded by contract, but fs_* callers
 *   are legitimately concurrent (the shell, MCUmgr's fs_mgmt workqueue), so
 *   every op serializes on one mutex — the same discipline littlefs's and
 *   FAT's drivers apply.
 * - The access-mode checks that go with a handle (read/write/truncate).
 * - A directory handle: the directory's id and the last name readdir
 *   returned. blobfs lists by name cursor, so that is the whole of a
 *   listing's state.
 *
 * Ops blobfs v1 cannot honor are left NULL — the VFS core NULL-checks every
 * op and reports -ENOTSUP itself, so there is nothing to stub here.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>
#include <app/lib/blobfs.h>
#include <app/lib/blobfs_fs.h>
#include <app/lib/rootreg.h>

LOG_MODULE_DECLARE(blobfs, CONFIG_BLOBDB_BLOBFS_LOG_LEVEL);

/* fs_stat() reports names through a fixed-size field; a name blobfs accepts
 * must fit it, or callers would see silently truncated names.
 */
BUILD_ASSERT(BLOBFS_NAME_MAX <= MAX_FILE_NAME,
	     "CONFIG_BLOBFS_MAX_NAME_LEN exceeds the VFS name field; raise "
	     "CONFIG_FILE_SYSTEM_MAX_FILE_NAME to match");

/** A synthesized handle: the file's identity and a position.
 *
 * `dead` marks a handle orphaned by unmount: its slot stays taken (so a
 * later open cannot alias it) and every op but close reports -EBADF until
 * the owner closes it.
 */
struct blobfs_fh {
	bool     used;
	bool     dead;
	uint64_t body_id;
	off_t    pos;
};

static struct blobfs_fh fh_pool[CONFIG_BLOBFS_FS_MAX_OPEN_FILES];

/** A directory handle: the cursor of a listing in progress.
 *
 * Like a file handle it binds to the directory's identity (its Map root),
 * not its path, so it follows a rename and reports -ENOENT once the
 * directory is removed. `last` is the name readdir returned last ("" before
 * the first); `done`
 * marks a listing that has reported its end, so later readdirs keep
 * reporting it. `dead` is as for file handles.
 */
struct blobfs_dh {
	bool used;
	bool dead;
	bool done;
	uint64_t dir_id;
	char last[BLOBFS_NAME_MAX + 1];
};

static struct blobfs_dh dh_pool[CONFIG_BLOBFS_FS_MAX_OPEN_DIRS];

/* v1 supports a single mounted instance (blobfs itself is a singleton). */
static bool fs_mounted;

/* One lock over every op: the stack below is single-threaded by contract. */
static K_MUTEX_DEFINE(blobfs_fs_lock);

static inline void lock(void)
{
	k_mutex_lock(&blobfs_fs_lock, K_FOREVER);
}

static inline void unlock(void)
{
	k_mutex_unlock(&blobfs_fs_lock);
}

static struct blobfs_fh *fh_alloc(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(fh_pool); i++) {
		if (!fh_pool[i].used) {
			fh_pool[i] = (struct blobfs_fh){ .used = true };
			return &fh_pool[i];
		}
	}
	return NULL;
}

static void fh_free(struct blobfs_fh *fh)
{
	*fh = (struct blobfs_fh){ 0 };
}

/* A live handle, or NULL. Callers report -EBADF for NULL. */
static struct blobfs_fh *fh_of(struct fs_file_t *filp)
{
	struct blobfs_fh *fh = filp->filep;

	if (fh == NULL || !fh->used || fh->dead) {
		return NULL;
	}
	return fh;
}

static struct blobfs_dh *dh_alloc(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(dh_pool); i++) {
		if (!dh_pool[i].used) {
			dh_pool[i] = (struct blobfs_dh){ .used = true };
			return &dh_pool[i];
		}
	}
	return NULL;
}

static void dh_free(struct blobfs_dh *dh)
{
	*dh = (struct blobfs_dh){ 0 };
}

/* Path as blobfs sees it: the mount point prefix belongs to the VFS. The
 * result may or may not carry a leading '/' (it does not when the mount
 * point is "/" itself); blobfs accepts both spellings.
 */
static const char *strip_mnt(const struct fs_mount_t *mp, const char *path)
{
	path += mp->mountp_len;
	return (*path != '\0') ? path : "/";
}

/* Copy the last component of @p path into fs_dirent::name. */
static void last_name(char dst[MAX_FILE_NAME + 1], const char *path)
{
	size_t end = strlen(path);

	while (end > 0 && path[end - 1] == '/') {
		end--;
	}

	size_t start = end;

	while (start > 0 && path[start - 1] != '/') {
		start--;
	}

	size_t n = MIN(end - start, (size_t)MAX_FILE_NAME);

	memcpy(dst, path + start, n);
	dst[n] = '\0';
}

static enum fs_dir_entry_type vfs_type(enum blobfs_entry_type type)
{
	return (type == BLOBFS_ENTRY_DIR) ? FS_DIR_ENTRY_DIR
					  : FS_DIR_ENTRY_FILE;
}

static int blobfs_fs_open(struct fs_file_t *filp, const char *fs_path,
			  fs_mode_t flags)
{
	const char *path = strip_mnt(filp->mp, fs_path);
	int rc;

	lock();

	/* Claim the handle before touching storage, so an open that cannot
	 * complete (-EMFILE) leaves nothing behind on flash.
	 */
	struct blobfs_fh *fh = fh_alloc();

	if (fh == NULL) {
		LOG_ERR("open '%s': all %zu handles in use", path,
			ARRAY_SIZE(fh_pool));
		rc = -EMFILE;
		goto out;
	}

	uint64_t body_id = 0;

	rc = blobfs_lookup(path, &body_id);
	if (rc == -ENOENT && (flags & FS_O_CREATE) != 0) {
		rc = blobfs_create(path);
		if (rc == 0) {
			rc = blobfs_lookup(path, &body_id);
		}
	}
	if (rc != 0) {
		fh_free(fh);
		goto out;
	}

	fh->body_id = body_id;
	filp->filep = fh;

	/* FS_O_TRUNC is applied by the VFS core, which calls truncate(0) right
	 * after this returns (with filp->flags already set).
	 */
out:
	unlock();
	return rc;
}

static int blobfs_fs_close(struct fs_file_t *filp)
{
	lock();

	struct blobfs_fh *fh = filp->filep;

	if (fh != NULL) {
		fh_free(fh);
		filp->filep = NULL;
	}

	unlock();
	return 0;
}

static ssize_t blobfs_fs_read(struct fs_file_t *filp, void *dest, size_t nbytes)
{
	ssize_t ret;

	lock();

	struct blobfs_fh *fh = fh_of(filp);

	if (fh == NULL) {
		ret = -EBADF;
	} else if ((filp->flags & FS_O_READ) == 0) {
		ret = -EACCES;
	} else {
		size_t rd = 0;
		int rc = blobfs_read_id(fh->body_id, (size_t)fh->pos, dest,
					nbytes, &rd);

		if (rc != 0) {
			ret = rc;
		} else {
			fh->pos += (off_t)rd;
			ret = (ssize_t)rd;
		}
	}

	unlock();
	return ret;
}

static ssize_t blobfs_fs_write(struct fs_file_t *filp, const void *src,
			       size_t nbytes)
{
	ssize_t ret;

	lock();

	struct blobfs_fh *fh = fh_of(filp);

	if (fh == NULL) {
		ret = -EBADF;
		goto out;
	}
	if ((filp->flags & FS_O_WRITE) == 0) {
		ret = -EACCES;
		goto out;
	}

	/* Work on a local position: a failed write must not move the file
	 * position, appending or not.
	 */
	off_t pos = fh->pos;

	if ((filp->flags & FS_O_APPEND) != 0) {
		size_t size = 0;
		int rc = blobfs_size_id(fh->body_id, &size);

		if (rc != 0) {
			ret = rc;
			goto out;
		}
		pos = (off_t)size;
	}

	int rc = blobfs_write_id(fh->body_id, (size_t)pos, src, nbytes);

	if (rc != 0) {
		ret = rc;
		goto out;
	}

	fh->pos = pos + (off_t)nbytes;
	ret = (ssize_t)nbytes;

out:
	unlock();
	return ret;
}

static int blobfs_fs_lseek(struct fs_file_t *filp, off_t off, int whence)
{
	int ret = 0;

	lock();

	struct blobfs_fh *fh = fh_of(filp);

	if (fh == NULL) {
		ret = -EBADF;
		goto out;
	}

	off_t pos;

	switch (whence) {
	case FS_SEEK_SET:
		pos = off;
		break;
	case FS_SEEK_CUR:
		pos = fh->pos + off;
		break;
	case FS_SEEK_END: {
		size_t size = 0;
		int rc = blobfs_size_id(fh->body_id, &size);

		if (rc != 0) {
			ret = rc;
			goto out;
		}
		pos = (off_t)size + off;
		break;
	}
	default:
		ret = -EINVAL;
		goto out;
	}

	if (pos < 0) {
		ret = -EINVAL;
		goto out;
	}

	fh->pos = pos;

out:
	unlock();
	return ret;
}

static off_t blobfs_fs_tell(struct fs_file_t *filp)
{
	off_t ret;

	lock();

	struct blobfs_fh *fh = fh_of(filp);

	ret = (fh == NULL) ? -EBADF : fh->pos;

	unlock();
	return ret;
}

static int blobfs_fs_truncate(struct fs_file_t *filp, off_t length)
{
	int ret;

	lock();

	struct blobfs_fh *fh = fh_of(filp);

	if (fh == NULL) {
		ret = -EBADF;
	} else if ((filp->flags & FS_O_WRITE) == 0) {
		/* The VFS core enforces write access only for the FS_O_TRUNC
		 * open flag; a plain fs_truncate() arrives unchecked.
		 */
		ret = -EACCES;
	} else if (length < 0) {
		ret = -EINVAL;
	} else {
		/* POSIX: the file position is not moved by a truncate. */
		ret = blobfs_truncate_id(fh->body_id, (size_t)length);
	}

	unlock();
	return ret;
}

static int blobfs_fs_sync(struct fs_file_t *filp)
{
	int ret;

	lock();

	/* Every write is committed by one atomic L1 write before it returns,
	 * so there is nothing buffered to flush.
	 */
	ret = (fh_of(filp) != NULL) ? 0 : -EBADF;

	unlock();
	return ret;
}

static int blobfs_fs_stat(struct fs_mount_t *mountp, const char *path,
			  struct fs_dirent *entry)
{
	int rc;

	/* No mount-point special case here: the VFS core answers fs_stat on
	 * "/" and on the mount point itself without calling the driver.
	 */
	lock();

	const char *name = strip_mnt(mountp, path);
	struct blobfs_stat stat;

	rc = blobfs_stat(name, &stat);
	if (rc == 0) {
		entry->type = vfs_type(stat.type);
		entry->size = stat.size;
		last_name(entry->name, name);
	}

	unlock();
	return rc;
}

static int blobfs_fs_mkdir(struct fs_mount_t *mountp, const char *name)
{
	int rc;

	lock();
	rc = blobfs_mkdir(strip_mnt(mountp, name));
	unlock();
	return rc;
}

static int blobfs_fs_unlink(struct fs_mount_t *mountp, const char *name)
{
	int rc;

	lock();
	rc = blobfs_unlink(strip_mnt(mountp, name));
	unlock();
	return rc;
}

static int blobfs_fs_rename(struct fs_mount_t *mountp, const char *from,
			    const char *to)
{
	int rc;

	lock();
	/* Both paths are within this mount: the VFS resolved them here. */
	rc = blobfs_rename(strip_mnt(mountp, from), strip_mnt(mountp, to));
	unlock();
	return rc;
}

static int blobfs_fs_opendir(struct fs_dir_t *dirp, const char *fs_path)
{
	int rc;

	/* fs_opendir("/") lists the VFS mount points and never reaches here,
	 * so a mount at "/" is listed as the VFS root, not by blobfs.
	 */
	lock();

	struct blobfs_dh *dh = dh_alloc();

	if (dh == NULL) {
		LOG_ERR("opendir: all %zu directory handles in use",
			ARRAY_SIZE(dh_pool));
		rc = -EMFILE;
		goto out;
	}

	rc = blobfs_opendir(strip_mnt(dirp->mp, fs_path), &dh->dir_id);
	if (rc != 0) {
		dh_free(dh);
		goto out;
	}

	dirp->dirp = dh;

out:
	unlock();
	return rc;
}

static int blobfs_fs_readdir(struct fs_dir_t *dirp, struct fs_dirent *entry)
{
	int rc = 0;

	lock();

	struct blobfs_dh *dh = dirp->dirp;

	if (dh == NULL || !dh->used || dh->dead) {
		rc = -EBADF;
		goto out;
	}

	struct blobfs_stat stat;

	if (!dh->done) {
		rc = blobfs_next_id(dh->dir_id, dh->last, entry->name, &stat);
		if (rc == -ENODATA) {
			dh->done = true;
			rc = 0;
		} else if (rc != 0) {
			goto out;
		}
	}

	if (dh->done) {
		/* The VFS's end-of-directory marker. */
		entry->name[0] = '\0';
		goto out;
	}

	entry->type = vfs_type(stat.type);
	entry->size = stat.size;
	strcpy(dh->last, entry->name);

out:
	unlock();
	return rc;
}

static int blobfs_fs_closedir(struct fs_dir_t *dirp)
{
	lock();

	struct blobfs_dh *dh = dirp->dirp;

	if (dh != NULL) {
		dh_free(dh);
		dirp->dirp = NULL;
	}

	unlock();
	return 0;
}

static int blobfs_fs_mount(struct fs_mount_t *mountp)
{
	ARG_UNUSED(mountp);

	int rc;

	lock();

	if (fs_mounted) {
		LOG_ERR("blobfs is already mounted elsewhere");
		rc = -EBUSY;
		goto out;
	}

	/* Bring the stack up if the application has not already done so. */
	rc = blob_db_mount();
	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("blob_db_mount: %d", rc);
		goto out;
	}

	rc = rootreg_init();
	if (rc != 0) {
		LOG_ERR("rootreg_init: %d", rc);
		goto out;
	}

	rc = blobfs_mount();
	if (rc != 0) {
		LOG_ERR("blobfs_mount: %d", rc);
		goto out;
	}

	/* Handles orphaned by an earlier unmount keep their slots until they
	 * are closed — the pools are deliberately NOT reset here.
	 */
	fs_mounted = true;

out:
	unlock();
	return rc;
}

static int blobfs_fs_unmount(struct fs_mount_t *mountp)
{
	ARG_UNUSED(mountp);

	lock();

	/* The VFS does not refuse unmount while files are open. Orphan any
	 * open handle rather than recycling its slot: its owner sees -EBADF
	 * until it closes, and no later open can alias it.
	 */
	for (size_t i = 0; i < ARRAY_SIZE(fh_pool); i++) {
		if (fh_pool[i].used) {
			fh_pool[i].dead = true;
		}
	}
	for (size_t i = 0; i < ARRAY_SIZE(dh_pool); i++) {
		if (dh_pool[i].used) {
			dh_pool[i].dead = true;
		}
	}

	/* blob_db stays mounted: other interfaces may still be using it. */
	(void)blobfs_unmount();
	fs_mounted = false;

	unlock();
	return 0;
}

/* statvfs stays NULL: the VFS core turns a NULL op into -ENOTSUP, which is
 * exactly what v1 owes its callers.
 */
static const struct fs_file_system_t blobfs_fs = {
	.open = blobfs_fs_open,
	.read = blobfs_fs_read,
	.write = blobfs_fs_write,
	.lseek = blobfs_fs_lseek,
	.tell = blobfs_fs_tell,
	.truncate = blobfs_fs_truncate,
	.sync = blobfs_fs_sync,
	.close = blobfs_fs_close,
	.mount = blobfs_fs_mount,
	.unmount = blobfs_fs_unmount,
	.mkdir = blobfs_fs_mkdir,
	.unlink = blobfs_fs_unlink,
	.rename = blobfs_fs_rename,
	.stat = blobfs_fs_stat,
	.opendir = blobfs_fs_opendir,
	.readdir = blobfs_fs_readdir,
	.closedir = blobfs_fs_closedir,
};

static int blobfs_fs_register(void)
{
	int rc = fs_register(BLOBFS_FS_TYPE, &blobfs_fs);

	if (rc != 0) {
		LOG_ERR("fs_register(%d): %d", BLOBFS_FS_TYPE, rc);
	}
	return rc;
}

SYS_INIT(blobfs_fs_register, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
