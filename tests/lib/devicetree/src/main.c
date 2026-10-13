/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Configuring the stack from devicetree
 * (doc/proposals/2026-10-12-devicetree-configuration.md).
 *
 * Every scenario in testcase.yaml builds this program over a different
 * variation of the kvdb,blob-db node. The suite reads that variation back
 * from devicetree and checks the behaviour it promises:
 *
 * - where the store mounts: before main() ("automount"), on device_init()
 *   ("zephyr,deferred-init"), or on the first call that needs it (neither);
 * - that an explicit blob_db_unmount() is not undone by a later call;
 * - that the Kconfig defaults (backend, autoformat, UBI heap) follow the node;
 * - that an fstab node mounts blobfs at boot.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_device.h>
#include <app/lib/blob_db_inspect.h>
#include <app/lib/rootreg.h>

#if DT_HAS_COMPAT_STATUS_OKAY(kvdb_fstab_blobfs)
#include <zephyr/fs/fs.h>
#endif

#define NODE      DT_CHOSEN(kvdb_blob_db)
#define STORAGE   DT_PHANDLE(NODE, storage)
#define ON_UBI    DT_NODE_HAS_COMPAT(DT_GPARENT(STORAGE), kvdb_ubi)
#define AUTOMOUNT DT_PROP(NODE, automount)
#define DEFERRED  DT_PROP_OR(NODE, zephyr_deferred_init, 0)

#define FSTAB_NODE DT_INST(0, kvdb_fstab_blobfs)
#define FSTAB_AUTOMOUNT \
	COND_CODE_1(DT_NODE_EXISTS(FSTAB_NODE), \
		    (DT_PROP(FSTAB_NODE, automount)), (0))

#define KEY ROOTREG_KEY(0x44545354 /* 'DTST' */, 0)

/* What boot left behind, captured before any test touches the store. */
static bool mounted_at_start;
static bool ready_at_start;

static bool store_mounted(void)
{
	struct blob_db_inspect_info in;

	/* Unlike the blob_db API, inspection never mounts on its own. */
	return blob_db_inspect_info_get(&in) == 0;
}

static void *suite_setup(void)
{
	mounted_at_start = store_mounted();
	ready_at_start = device_is_ready(BLOB_DB_DEVICE);
	return NULL;
}

ZTEST_SUITE(devicetree, NULL, suite_setup, NULL, NULL, NULL);

/* The whole life of the store in one case, because it is one sequence:
 * how it came up, then an explicit unmount, then an explicit mount. */
ZTEST(devicetree, test_lifecycle)
{
	uint64_t id, again;
	uint8_t buf[8];
	size_t got;

	if (DEFERRED) {
		zassert_false(ready_at_start, "deferred device must wait");
		zassert_false(mounted_at_start, "deferred store must wait");
		zassert_ok(device_init(BLOB_DB_DEVICE));
		zassert_true(device_is_ready(BLOB_DB_DEVICE));
		zassert_true(store_mounted(), "device_init() must automount");
	} else if (AUTOMOUNT || FSTAB_AUTOMOUNT) {
		zassert_true(ready_at_start);
		zassert_true(mounted_at_start, "store must be mounted before main()");
	} else {
		/* Nothing mounted it, and nothing needs to. */
		zassert_true(ready_at_start);
		zassert_false(mounted_at_start, "store mounted without automount");
		zassert_ok(rootreg_get_or_create(KEY, &id),
			   "the first call must mount the store");
		zassert_true(store_mounted());
	}

	/* Whichever way it came up, it is the same store now. */
	zassert_equal(blob_db_mount(), -EALREADY);
	zassert_ok(rootreg_get_or_create(KEY, &id));

	/* An explicit unmount holds: nothing mounts the store behind the
	 * caller's back until the caller mounts it again. */
	zassert_ok(blob_db_unmount());
	zassert_false(store_mounted());
	zassert_equal(blob_db_get(BLOB_DB_ROOT_ID, buf, sizeof(buf), &got),
		      -ENODEV);
	zassert_equal(blob_db_count(), 0);
	zassert_equal(blob_db_alloc_id(), 0);
	zassert_equal(rootreg_get(KEY, &again), -ENODEV);
	zassert_false(store_mounted(), "a call undid the explicit unmount");

	zassert_ok(blob_db_mount());
	zassert_ok(rootreg_get(KEY, &again));
	zassert_equal(id, again, "the registry survives the remount");
}

ZTEST(devicetree, test_kconfig_defaults_follow_devicetree)
{
	zassert_equal(IS_ENABLED(CONFIG_BLOB_DB_BACKEND_UBI), ON_UBI,
		      "backend must follow what 'storage' points at");
	zassert_equal(IS_ENABLED(CONFIG_BLOB_DB_BACKEND_FLASH_AREA), !ON_UBI);
	zassert_equal(IS_ENABLED(CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT),
		      !DT_PROP(NODE, no_format),
		      "autoformat must follow 'no-format'");

	/* Either way the partition under the store is storage_partition. */
	zassert_equal(CONFIG_BLOB_DB_UBI_HEAP_SIZE,
		      DT_REG_SIZE(DT_NODELABEL(storage_partition)) / 512 + 4096,
		      "UBI heap must be sized from the store's partition");
}

/* The store opened the partition devicetree named: raw, all of it; on UBI,
 * all of it less UBI's headers, volume table and spare blocks. */
ZTEST(devicetree, test_store_is_on_the_partition_devicetree_names)
{
	struct blob_db_inspect_info in;
	const size_t part = DT_REG_SIZE(DT_NODELABEL(storage_partition));

	(void)blob_db_count();   /* mounts the store if no case has yet */
	zassert_ok(blob_db_inspect_info_get(&in));
	if (ON_UBI) {
		zassert_true(in.partition_size < part);
		zassert_true(in.partition_size > part / 2);
	} else {
		zassert_equal(in.partition_size, part);
	}
}

#if DT_HAS_COMPAT_STATUS_OKAY(kvdb_fstab_blobfs)
FS_FSTAB_DECLARE_ENTRY(FSTAB_NODE);

#define MNT DT_PROP(FSTAB_NODE, mount_point)

ZTEST(devicetree, test_fstab_mounts_blobfs_at_boot)
{
	struct fs_mount_t *mp = &FS_FSTAB_ENTRY(FSTAB_NODE);
	struct fs_dirent st;
	struct fs_file_t file;
	char out[8] = { 0 };
	static const char msg[] = "hello";

	zassert_true(mp->flags & FS_MOUNT_FLAG_AUTOMOUNT);
	zassert_ok(strcmp(mp->mnt_point, MNT));
	zassert_ok(fs_stat(MNT, &st), "fstab automount did not mount " MNT);

	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, MNT "/hello", FS_O_CREATE | FS_O_RDWR));
	zassert_equal(fs_write(&file, msg, sizeof(msg)), sizeof(msg));
	zassert_ok(fs_seek(&file, 0, FS_SEEK_SET));
	zassert_equal(fs_read(&file, out, sizeof(out)), sizeof(msg));
	zassert_ok(fs_close(&file));
	zassert_mem_equal(out, msg, sizeof(msg));

	/* A second mount of the same entry is refused, not duplicated. */
	zassert_not_equal(fs_mount(mp), 0);
}
#endif
