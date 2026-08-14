/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite for the blobfs Zephyr-filesystem interop shim.
 *
 * The bulk of the coverage is Zephyr's own file system conformance bodies
 * (zephyr/tests/subsys/fs/common), compiled unmodified and pointed at a
 * blobfs mount: passing what FAT and littlefs answer to is the claim that
 * blobfs behaves like a Zephyr file system. The suites below that cover the
 * glue's own boundaries — what v1 does not support, and where its limits
 * are.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/blobfs.h>
#include <app/lib/blobfs_fs.h>
#include <app/lib/rootreg.h>

#define MNT_POINT "/blob"

static struct fs_mount_t blobfs_mnt = {
	.type = BLOBFS_FS_TYPE,
	.mnt_point = MNT_POINT,
};

/* Hooks the upstream test bodies expect from their runner. */
struct fs_mount_t *fs_basic_test_mp = &blobfs_mnt;
static char open_flags_path[] = MNT_POINT "/the_file";
char *test_fs_open_flags_file_path = open_flags_path;

void test_fs_basic(void);
void test_fs_open_flags(void);

static void blobfs_before(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Start every test from a mounted, empty store and nothing mounted on
	 * the VFS side.
	 */
	(void)fs_unmount(&blobfs_mnt);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
	zassert_ok(rootreg_init());
}

static void blobfs_after(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)fs_unmount(&blobfs_mnt);
	blob_db_unmount();
}

ZTEST_SUITE(blobfs, NULL, NULL, blobfs_before, blobfs_after, NULL);

/* Zephyr's basic file system suite: create/write/stat, read back, seek,
 * truncate, unlink, sync, and content surviving unmount + remount. It mounts
 * and unmounts the file system itself.
 */
ZTEST(blobfs, test_zephyr_fs_basic)
{
	test_fs_basic();
}

/* Zephyr's fs_open() flag matrix: access modes, FS_O_CREATE, FS_O_APPEND,
 * FS_O_TRUNC and their combinations.
 */
ZTEST(blobfs, test_zephyr_fs_open_flags)
{
	zassert_ok(fs_mount(&blobfs_mnt));

	test_fs_open_flags();

	zassert_ok(fs_unmount(&blobfs_mnt));
}

/* What v1 does not carry is reported, not faked: directories and free-space
 * accounting come back -ENOTSUP through the VFS.
 */
ZTEST(blobfs, test_unsupported_ops_report_enotsup)
{
	struct fs_dir_t dir;
	struct fs_statvfs vfs;

	zassert_ok(fs_mount(&blobfs_mnt));

	fs_dir_t_init(&dir);
	zassert_equal(fs_mkdir(MNT_POINT "/sub"), -ENOTSUP);
	zassert_equal(fs_opendir(&dir, MNT_POINT), -ENOTSUP);
	zassert_equal(fs_statvfs(MNT_POINT, &vfs), -ENOTSUP);

	zassert_ok(fs_unmount(&blobfs_mnt));
}

/* The flat namespace and the one-payload body cap, as seen through the VFS. */
ZTEST(blobfs, test_namespace_and_size_limits)
{
	static uint8_t oversized[BLOBFS_FILE_MAX + 1];
	struct fs_file_t file;

	zassert_ok(fs_mount(&blobfs_mnt));
	fs_file_t_init(&file);

	/* No directory can exist, so nothing can live under one. */
	zassert_equal(fs_open(&file, MNT_POINT "/sub/file",
			      FS_O_CREATE | FS_O_RDWR),
		      -ENOENT);

	/* Names are bounded by CONFIG_BLOBFS_MAX_NAME_LEN. */
	zassert_equal(fs_open(&file, MNT_POINT "/a_name_that_is_far_too_long",
			      FS_O_CREATE | FS_O_RDWR),
		      -ENAMETOOLONG);

	/* A body is one blob payload; one byte more is refused, and the file
	 * is left exactly as it was.
	 */
	zassert_ok(fs_open(&file, MNT_POINT "/big", FS_O_CREATE | FS_O_RDWR));
	zassert_equal(fs_write(&file, oversized, sizeof(oversized)), -ENOSPC);
	zassert_equal(fs_write(&file, oversized, BLOBFS_FILE_MAX),
		      BLOBFS_FILE_MAX);
	zassert_ok(fs_close(&file));

	struct fs_dirent stat;

	zassert_ok(fs_stat(MNT_POINT "/big", &stat));
	zassert_equal(stat.size, BLOBFS_FILE_MAX);

	zassert_ok(fs_unmount(&blobfs_mnt));
}

/* Rename keeps the body (and its content) and refuses to clobber. */
ZTEST(blobfs, test_rename_keeps_content)
{
	static const char payload[] = "renamed";
	struct fs_file_t file;
	char buf[sizeof(payload)];
	struct fs_dirent stat;

	zassert_ok(fs_mount(&blobfs_mnt));
	fs_file_t_init(&file);

	zassert_ok(fs_open(&file, MNT_POINT "/from", FS_O_CREATE | FS_O_RDWR));
	zassert_equal(fs_write(&file, payload, sizeof(payload)),
		      sizeof(payload));
	zassert_ok(fs_close(&file));

	zassert_ok(fs_rename(MNT_POINT "/from", MNT_POINT "/to"));
	zassert_equal(fs_stat(MNT_POINT "/from", &stat), -ENOENT);
	zassert_ok(fs_stat(MNT_POINT "/to", &stat));
	zassert_equal(stat.size, sizeof(payload));

	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, MNT_POINT "/to", FS_O_READ));
	zassert_equal(fs_read(&file, buf, sizeof(buf)), sizeof(buf));
	zassert_mem_equal(buf, payload, sizeof(payload));
	zassert_ok(fs_close(&file));

	/* An occupied destination is refused rather than silently replaced. */
	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, MNT_POINT "/other", FS_O_CREATE | FS_O_RDWR));
	zassert_ok(fs_close(&file));
	zassert_equal(fs_rename(MNT_POINT "/to", MNT_POINT "/other"), -EEXIST);

	zassert_ok(fs_unmount(&blobfs_mnt));
}

/* One mount at a time in v1, and the VFS is told so. */
ZTEST(blobfs, test_second_mount_is_busy)
{
	struct fs_mount_t second = {
		.type = BLOBFS_FS_TYPE,
		.mnt_point = "/blob2",
	};

	zassert_ok(fs_mount(&blobfs_mnt));
	zassert_equal(fs_mount(&second), -EBUSY);
	zassert_ok(fs_unmount(&blobfs_mnt));
}
