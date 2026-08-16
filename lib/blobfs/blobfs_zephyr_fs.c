/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blobfs <-> Zephyr VFS glue.
 *
 * Registers a struct fs_file_system_t whose ops forward to the handle-free
 * blobfs API. All this file owns is what the VFS needs and blobfs
 * deliberately does not have: a file handle (name + read/write position) and
 * the access-mode checks that go with one. Everything durable happens below.
 *
 * Ops blobfs v1 cannot honor are left NULL — the VFS core NULL-checks every
 * op and reports -ENOTSUP itself, so there is nothing to stub here.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/init.h>
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

/** A synthesized handle: what blobfs does not keep, the VFS requires. */
struct blobfs_fh {
	bool  used;
	char  name[BLOBFS_NAME_MAX + 1];
	off_t pos;
};

static struct blobfs_fh fh_pool[CONFIG_BLOBFS_FS_MAX_OPEN_FILES];

/* v1 supports a single mounted instance (blobfs itself is a singleton). */
static bool fs_mounted;

static struct blobfs_fh *fh_alloc(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(fh_pool); i++) {
		if (!fh_pool[i].used) {
			fh_pool[i].used = true;
			fh_pool[i].pos = 0;
			return &fh_pool[i];
		}
	}
	return NULL;
}

static void fh_free(struct blobfs_fh *fh)
{
	fh->used = false;
	fh->name[0] = '\0';
	fh->pos = 0;
}

/* Path as blobfs sees it: the mount point prefix belongs to the VFS. */
static const char *strip_mnt(const struct fs_mount_t *mp, const char *path)
{
	path += mp->mountp_len;
	return (*path != '\0') ? path : "/";
}

static int blobfs_fs_open(struct fs_file_t *filp, const char *fs_path,
			  fs_mode_t flags)
{
	const char *path = strip_mnt(filp->mp, fs_path);
	struct blobfs_stat stat;
	int rc = blobfs_stat(path, &stat);

	if (rc == -ENOENT && (flags & FS_O_CREATE) != 0) {
		rc = blobfs_create(path);
	}
	if (rc != 0) {
		return rc;
	}

	struct blobfs_fh *fh = fh_alloc();

	if (fh == NULL) {
		LOG_ERR("open '%s': all %zu handles in use", path,
			ARRAY_SIZE(fh_pool));
		return -EMFILE;
	}

	/* blobfs validated the name on the way in, so it fits. */
	strncpy(fh->name, path + 1, sizeof(fh->name) - 1);
	fh->name[sizeof(fh->name) - 1] = '\0';
	filp->filep = fh;

	/* FS_O_TRUNC is applied by the VFS core, which calls truncate(0) right
	 * after this returns.
	 */
	return 0;
}

static int blobfs_fs_close(struct fs_file_t *filp)
{
	struct blobfs_fh *fh = filp->filep;

	if (fh != NULL) {
		fh_free(fh);
		filp->filep = NULL;
	}
	return 0;
}

static ssize_t blobfs_fs_read(struct fs_file_t *filp, void *dest, size_t nbytes)
{
	struct blobfs_fh *fh = filp->filep;

	if (fh == NULL) {
		return -EBADF;
	}
	if ((filp->flags & FS_O_READ) == 0) {
		return -EACCES;
	}

	size_t rd = 0;
	int rc = blobfs_read(fh->name, (size_t)fh->pos, dest, nbytes, &rd);

	if (rc != 0) {
		return rc;
	}

	fh->pos += (off_t)rd;
	return (ssize_t)rd;
}

static ssize_t blobfs_fs_write(struct fs_file_t *filp, const void *src,
			       size_t nbytes)
{
	struct blobfs_fh *fh = filp->filep;

	if (fh == NULL) {
		return -EBADF;
	}
	if ((filp->flags & FS_O_WRITE) == 0) {
		return -EACCES;
	}

	if ((filp->flags & FS_O_APPEND) != 0) {
		struct blobfs_stat stat;
		int rc = blobfs_stat(fh->name, &stat);

		if (rc != 0) {
			return rc;
		}
		fh->pos = (off_t)stat.size;
	}

	int rc = blobfs_write(fh->name, (size_t)fh->pos, src, nbytes);

	if (rc != 0) {
		return rc;
	}

	fh->pos += (off_t)nbytes;
	return (ssize_t)nbytes;
}

static int blobfs_fs_lseek(struct fs_file_t *filp, off_t off, int whence)
{
	struct blobfs_fh *fh = filp->filep;

	if (fh == NULL) {
		return -EBADF;
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
		struct blobfs_stat stat;
		int rc = blobfs_stat(fh->name, &stat);

		if (rc != 0) {
			return rc;
		}
		pos = (off_t)stat.size + off;
		break;
	}
	default:
		return -EINVAL;
	}

	if (pos < 0) {
		return -EINVAL;
	}

	fh->pos = pos;
	return 0;
}

static off_t blobfs_fs_tell(struct fs_file_t *filp)
{
	struct blobfs_fh *fh = filp->filep;

	if (fh == NULL) {
		return -EBADF;
	}
	return fh->pos;
}

static int blobfs_fs_truncate(struct fs_file_t *filp, off_t length)
{
	struct blobfs_fh *fh = filp->filep;

	if (fh == NULL) {
		return -EBADF;
	}
	if (length < 0) {
		return -EINVAL;
	}

	/* POSIX: the file position is not moved by a truncate. */
	return blobfs_truncate(fh->name, (size_t)length);
}

static int blobfs_fs_sync(struct fs_file_t *filp)
{
	/* Every write is committed by one atomic blob_db_update() before it
	 * returns, so there is nothing buffered to flush.
	 */
	return (filp->filep != NULL) ? 0 : -EBADF;
}

static int blobfs_fs_stat(struct fs_mount_t *mountp, const char *path,
			  struct fs_dirent *entry)
{
	const char *name = strip_mnt(mountp, path);

	if (strcmp(name, "/") == 0) {
		/* The mount point itself: the one directory v1 has. */
		entry->type = FS_DIR_ENTRY_DIR;
		entry->size = 0;
		entry->name[0] = '/';
		entry->name[1] = '\0';
		return 0;
	}

	struct blobfs_stat stat;
	int rc = blobfs_stat(name, &stat);

	if (rc != 0) {
		return rc;
	}

	entry->type = FS_DIR_ENTRY_FILE;
	entry->size = stat.size;
	strncpy(entry->name, name + 1, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';
	return 0;
}

static int blobfs_fs_unlink(struct fs_mount_t *mountp, const char *name)
{
	return blobfs_unlink(strip_mnt(mountp, name));
}

static int blobfs_fs_rename(struct fs_mount_t *mountp, const char *from,
			    const char *to)
{
	/* Both paths are within this mount: the VFS resolved them here. */
	return blobfs_rename(strip_mnt(mountp, from), strip_mnt(mountp, to));
}

static int blobfs_fs_mount(struct fs_mount_t *mountp)
{
	ARG_UNUSED(mountp);

	if (fs_mounted) {
		LOG_ERR("blobfs is already mounted elsewhere");
		return -EBUSY;
	}

	/* Bring the stack up if the application has not already done so. */
	int rc = blob_db_mount();

	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("blob_db_mount: %d", rc);
		return rc;
	}

	rc = rootreg_init();
	if (rc != 0) {
		LOG_ERR("rootreg_init: %d", rc);
		return rc;
	}

	rc = blobfs_mount();
	if (rc != 0) {
		LOG_ERR("blobfs_mount: %d", rc);
		return rc;
	}

	memset(fh_pool, 0, sizeof(fh_pool));
	fs_mounted = true;
	return 0;
}

static int blobfs_fs_unmount(struct fs_mount_t *mountp)
{
	ARG_UNUSED(mountp);

	/* blob_db stays mounted: other interfaces may still be using it. */
	(void)blobfs_unmount();
	memset(fh_pool, 0, sizeof(fh_pool));
	fs_mounted = false;
	return 0;
}

/* mkdir, opendir/readdir/closedir and statvfs stay NULL: the VFS core turns a
 * NULL op into -ENOTSUP, which is exactly what v1 owes those callers.
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
	.unlink = blobfs_fs_unlink,
	.rename = blobfs_fs_rename,
	.stat = blobfs_fs_stat,
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
