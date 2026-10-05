/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite for the blobfs Zephyr-filesystem interop shim.
 *
 * The bulk of the coverage is Zephyr's own file system conformance bodies
 * (zephyr/tests/subsys/fs/common), compiled unmodified and pointed at a
 * blobfs mount: passing what FAT and littlefs answer to is the claim that
 * blobfs behaves like a Zephyr file system. The suites below that cover
 * what the conformance bodies never cross: handle lifetime against rename
 * and unlink, access-mode enforcement, failure atomicity at the API edge,
 * mid-rename crash recovery, and the v1 boundaries.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/blobfs.h>
#include <app/lib/blobfs_fs.h>
#include <app/lib/rootreg.h>

#include "blobfs_internal.h" /* forged crash states for recovery tests */

#define MNT_POINT "/blob"

static struct fs_mount_t blobfs_mnt = {
	.type = BLOBFS_FS_TYPE,
	.mnt_point = MNT_POINT,
};

/* A second mount point for the mount-at-root test. */
static struct fs_mount_t root_mnt = {
	.type = BLOBFS_FS_TYPE,
	.mnt_point = "/",
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
	(void)fs_unmount(&root_mnt);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
	zassert_ok(rootreg_init());
}

static void blobfs_after(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)fs_unmount(&blobfs_mnt);
	(void)fs_unmount(&root_mnt);
	blob_db_unmount();
}

ZTEST_SUITE(blobfs, NULL, NULL, blobfs_before, blobfs_after, NULL);

/* Create @p path holding @p len bytes of @p data through the fs API. */
static void make_file(const char *path, const void *data, size_t len)
{
	struct fs_file_t file;

	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, path, FS_O_CREATE | FS_O_RDWR),
		   "create '%s'", path);
	if (len > 0) {
		zassert_equal(fs_write(&file, data, len), (ssize_t)len,
			      "fill '%s'", path);
	}
	zassert_ok(fs_close(&file));
}

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

	/* The POSIX special entries are refused, not stored: a file literally
	 * named ".." would be unaddressable through path-normalizing clients.
	 */
	zassert_equal(fs_open(&file, MNT_POINT "/.", FS_O_CREATE | FS_O_RDWR),
		      -EINVAL);
	zassert_equal(fs_open(&file, MNT_POINT "/..", FS_O_CREATE | FS_O_RDWR),
		      -EINVAL);

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

	make_file(MNT_POINT "/from", payload, sizeof(payload));

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
	make_file(MNT_POINT "/other", NULL, 0);
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

/* fs_truncate arrives at the driver unchecked (the VFS core only gates the
 * FS_O_TRUNC open flag), so the driver must enforce write access itself.
 */
ZTEST(blobfs, test_truncate_requires_write_access)
{
	static const char payload[] = "1234";
	struct fs_file_t file;
	struct fs_dirent stat;

	zassert_ok(fs_mount(&blobfs_mnt));

	make_file(MNT_POINT "/cfg", payload, sizeof(payload));

	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, MNT_POINT "/cfg", FS_O_READ));
	zassert_equal(fs_truncate(&file, 0), -EACCES);
	zassert_ok(fs_close(&file));

	zassert_ok(fs_stat(MNT_POINT "/cfg", &stat));
	zassert_equal(stat.size, sizeof(payload), "read-only truncate wiped");

	/* With write access it works. */
	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, MNT_POINT "/cfg", FS_O_WRITE));
	zassert_ok(fs_truncate(&file, 2));
	zassert_ok(fs_close(&file));
	zassert_ok(fs_stat(MNT_POINT "/cfg", &stat));
	zassert_equal(stat.size, 2);

	zassert_ok(fs_unmount(&blobfs_mnt));
}

/* A handle binds to the file, not the name: it follows a rename, and a new
 * file under the old name is a different file.
 */
ZTEST(blobfs, test_handle_follows_rename)
{
	struct fs_file_t moved, fresh, check;
	struct fs_dirent stat;
	char buf[8];

	zassert_ok(fs_mount(&blobfs_mnt));

	fs_file_t_init(&moved);
	zassert_ok(fs_open(&moved, MNT_POINT "/src", FS_O_CREATE | FS_O_RDWR));
	zassert_equal(fs_write(&moved, "AAAA", 4), 4);

	zassert_ok(fs_rename(MNT_POINT "/src", MNT_POINT "/dst"));

	/* The open handle keeps addressing the renamed file. */
	zassert_equal(fs_write(&moved, "BBBB", 4), 4);
	zassert_ok(fs_close(&moved));

	zassert_ok(fs_stat(MNT_POINT "/dst", &stat));
	zassert_equal(stat.size, 8, "write after rename missed the file");

	/* A new file under the old name is unrelated to the moved one. */
	fs_file_t_init(&fresh);
	zassert_ok(fs_open(&fresh, MNT_POINT "/src", FS_O_CREATE | FS_O_RDWR));
	zassert_equal(fs_write(&fresh, "CC", 2), 2);
	zassert_ok(fs_close(&fresh));

	fs_file_t_init(&check);
	zassert_ok(fs_open(&check, MNT_POINT "/dst", FS_O_READ));
	zassert_equal(fs_read(&check, buf, sizeof(buf)), 8);
	zassert_mem_equal(buf, "AAAABBBB", 8);
	zassert_ok(fs_close(&check));

	zassert_ok(fs_unmount(&blobfs_mnt));
}

/* After unlink, an open handle fails -ENOENT — including for zero-length
 * writes — and never touches a successor file of the same name.
 */
ZTEST(blobfs, test_unlink_with_open_handle)
{
	struct fs_file_t old, fresh;
	struct fs_dirent stat;
	char buf[4];

	zassert_ok(fs_mount(&blobfs_mnt));

	fs_file_t_init(&old);
	zassert_ok(fs_open(&old, MNT_POINT "/gone", FS_O_CREATE | FS_O_RDWR));
	zassert_equal(fs_write(&old, "data", 4), 4);

	zassert_ok(fs_unlink(MNT_POINT "/gone"));

	zassert_equal(fs_read(&old, buf, sizeof(buf)), -ENOENT);
	zassert_equal(fs_write(&old, "data", 4), -ENOENT);
	zassert_equal(fs_write(&old, buf, 0), -ENOENT,
		      "zero-length write must still report the missing file");

	/* Recreate the name: the stale handle must not reach the new file. */
	fs_file_t_init(&fresh);
	zassert_ok(fs_open(&fresh, MNT_POINT "/gone", FS_O_CREATE | FS_O_RDWR));
	zassert_equal(fs_write(&old, "data", 4), -ENOENT);
	zassert_equal(fs_write(&fresh, "XY", 2), 2);
	zassert_ok(fs_close(&fresh));
	zassert_ok(fs_close(&old));

	zassert_ok(fs_stat(MNT_POINT "/gone", &stat));
	zassert_equal(stat.size, 2, "stale handle leaked into the new file");

	zassert_ok(fs_unmount(&blobfs_mnt));
}

/* A failed open has no side effect: running out of handles must not leave a
 * freshly created file behind.
 */
ZTEST(blobfs, test_emfile_open_leaves_no_file)
{
	struct fs_file_t files[CONFIG_BLOBFS_FS_MAX_OPEN_FILES];
	struct fs_file_t extra;
	struct fs_dirent stat;
	char path[32];

	zassert_ok(fs_mount(&blobfs_mnt));

	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		snprintf(path, sizeof(path), MNT_POINT "/f%zu", i);
		fs_file_t_init(&files[i]);
		zassert_ok(fs_open(&files[i], path, FS_O_CREATE | FS_O_RDWR));
	}

	fs_file_t_init(&extra);
	zassert_equal(fs_open(&extra, MNT_POINT "/phantom",
			      FS_O_CREATE | FS_O_RDWR),
		      -EMFILE);
	zassert_equal(fs_stat(MNT_POINT "/phantom", &stat), -ENOENT,
		      "failed open left a file behind");

	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		zassert_ok(fs_close(&files[i]));
	}

	zassert_ok(fs_unmount(&blobfs_mnt));
}

/* A failed write moves nothing: in append mode the position must stay where
 * it was, not jump to EOF.
 */
ZTEST(blobfs, test_failed_append_keeps_position)
{
	static uint8_t full[BLOBFS_FILE_MAX];
	struct fs_file_t file;

	zassert_ok(fs_mount(&blobfs_mnt));

	make_file(MNT_POINT "/full", full, sizeof(full));

	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, MNT_POINT "/full",
			   FS_O_APPEND | FS_O_WRITE));
	zassert_ok(fs_seek(&file, 5, FS_SEEK_SET));
	zassert_equal(fs_tell(&file), 5);

	/* The file is at the body cap: appending one byte must fail... */
	zassert_equal(fs_write(&file, "x", 1), -ENOSPC);
	/* ...and must not have moved the position as a side effect. */
	zassert_equal(fs_tell(&file), 5, "failed append moved the position");

	zassert_ok(fs_close(&file));
	zassert_ok(fs_unmount(&blobfs_mnt));
}

/* Mounting at "/" is accepted by the VFS core, and there the stripped path
 * has no leading slash — names must survive that spelling.
 */
ZTEST(blobfs, test_mount_at_root)
{
	struct fs_file_t file;
	struct fs_dirent stat;
	char buf[2];

	zassert_ok(fs_mount(&root_mnt));

	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, "/rootfile", FS_O_CREATE | FS_O_RDWR));
	zassert_equal(fs_write(&file, "zz", 2), 2);
	zassert_ok(fs_seek(&file, 0, FS_SEEK_SET));
	zassert_equal(fs_read(&file, buf, sizeof(buf)), 2);
	zassert_mem_equal(buf, "zz", 2);
	zassert_ok(fs_close(&file));

	zassert_ok(fs_stat("/rootfile", &stat));
	zassert_equal(stat.size, 2);
	zassert_str_equal(stat.name, "rootfile",
			  "name mangled under a \"/\" mount");

	zassert_ok(fs_unlink("/rootfile"));
	zassert_ok(fs_unmount(&root_mnt));
}

/* Forge the meta state a crash would leave mid-rename and check that mount
 * resolves it. The intent layout comes from the library's internal header —
 * this is deliberately white-box: it is the only way to reach these states
 * without cutting power.
 */
static void forge_rename_intent(const char *from, const char *to,
				uint64_t body_id)
{
	uint64_t meta_id = 0;
	struct blobfs_meta meta;
	size_t got = 0;

	zassert_ok(rootreg_get(ROOTREG_KEY(BLOBFS_MAGIC, 0), &meta_id));
	zassert_ok(blob_db_get(meta_id, &meta, sizeof(meta), &got));
	zassert_equal(got, sizeof(meta));

	meta.rename_in_flight = 1;
	meta.from_len = (uint8_t)strlen(from);
	meta.to_len = (uint8_t)strlen(to);
	meta.rename_body = body_id;
	memset(meta.from, 0, sizeof(meta.from));
	memset(meta.to, 0, sizeof(meta.to));
	memcpy(meta.from, from, strlen(from));
	memcpy(meta.to, to, strlen(to));

	zassert_ok(blob_db_update(meta_id, &meta, sizeof(meta)));
}

static void check_intent_cleared(void)
{
	uint64_t meta_id = 0;
	struct blobfs_meta meta;
	size_t got = 0;

	zassert_ok(rootreg_get(ROOTREG_KEY(BLOBFS_MAGIC, 0), &meta_id));
	zassert_ok(blob_db_get(meta_id, &meta, sizeof(meta), &got));
	zassert_equal(got, sizeof(meta));
	zassert_equal(meta.rename_in_flight, 0, "intent not cleared");
}

ZTEST(blobfs, test_rename_intent_recovery)
{
	static const char payload[] = "survives";
	struct fs_dirent stat;
	uint64_t body_id = 0;

	zassert_ok(fs_mount(&blobfs_mnt));
	make_file(MNT_POINT "/keep", payload, sizeof(payload));
	zassert_ok(blobfs_lookup("keep", &body_id));
	zassert_ok(fs_unmount(&blobfs_mnt));

	/* Crash point 1: intent committed, directory untouched. Mount must
	 * roll the rename back — the old name intact, the new one absent.
	 */
	forge_rename_intent("keep", "dest", body_id);
	zassert_ok(fs_mount(&blobfs_mnt));
	check_intent_cleared();
	zassert_ok(fs_stat(MNT_POINT "/keep", &stat));
	zassert_equal(stat.size, sizeof(payload));
	zassert_equal(fs_stat(MNT_POINT "/dest", &stat), -ENOENT);
	zassert_ok(fs_unmount(&blobfs_mnt));

	/* Crash point 2: rename fully applied but the intent not yet cleared
	 * (the state after del(from), before the final meta write). Mount
	 * must roll forward — clear the intent and leave the file in place.
	 */
	forge_rename_intent("ghost", "keep", body_id);
	zassert_ok(fs_mount(&blobfs_mnt));
	check_intent_cleared();
	zassert_ok(fs_stat(MNT_POINT "/keep", &stat));
	zassert_equal(stat.size, sizeof(payload));
	zassert_equal(fs_stat(MNT_POINT "/ghost", &stat), -ENOENT);

	/* The filesystem stays fully usable: a real rename still works. */
	zassert_ok(fs_rename(MNT_POINT "/keep", MNT_POINT "/moved"));
	check_intent_cleared();
	zassert_ok(fs_stat(MNT_POINT "/moved", &stat));
	zassert_equal(stat.size, sizeof(payload));
	zassert_equal(fs_stat(MNT_POINT "/keep", &stat), -ENOENT);

	zassert_ok(fs_unmount(&blobfs_mnt));
}
