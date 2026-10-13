/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blobfs sample — a file system described in devicetree, with no init code.
 *
 * Everything below main() is declared in boards/<board>.overlay: the storage
 * partition is a UBI device, a volume on it holds the blob_db store, and an
 * fstab node mounts blobfs on that store at "/blob" during boot — the same
 * way Zephyr's littlefs is mounted from a "zephyr,fstab,littlefs" node.
 *
 * So the program is only the application: it keeps a boot counter in a file,
 * leaves a note next to it, and lists the directory. There is no
 * blob_db_mount(), no rootreg_init() and no fs_mount() anywhere.
 *
 * Run it:
 *   west build -b native_sim key_value_db/samples/blobfs
 *   ./build/zephyr/zephyr.exe --flash=blobfs.bin
 * and run it again on the same --flash file to see the counter go up.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/app_version.h>

#ifdef CONFIG_ARCH_POSIX
#include "posix_board_if.h"   /* posix_exit() — end the sim run cleanly */
#endif

/* The mount point is the fstab node's, so it is written down once. */
#define MNT          DT_PROP(DT_NODELABEL(blobfs0), mount_point)
#define COUNTER_PATH MNT "/boot.cnt"
#define NOTE_PATH    MNT "/note.txt"

static void step(int n, const char *title)
{
	printk("\n[%d] %s\n", n, title);
}

/* Read the counter file, or 0 when this is the first boot. */
static int read_counter(uint32_t *count)
{
	struct fs_file_t file;
	int rc;

	fs_file_t_init(&file);
	rc = fs_open(&file, COUNTER_PATH, FS_O_READ);
	if (rc == -ENOENT) {
		printk("    fs_open(\"%s\", READ) -> -ENOENT: first boot\n",
		       COUNTER_PATH);
		*count = 0;
		return 0;
	}
	if (rc != 0) {
		printk("    fs_open(\"%s\") -> %d  FAILED\n", COUNTER_PATH, rc);
		return rc;
	}

	ssize_t got = fs_read(&file, count, sizeof(*count));

	printk("    fs_read(\"%s\") -> %d bytes\n", COUNTER_PATH, (int)got);
	(void)fs_close(&file);
	return (got == sizeof(*count)) ? 0 : -EIO;
}

/* Replace @p path with @p len bytes of @p data. */
static int write_file(const char *path, const void *data, size_t len)
{
	struct fs_file_t file;
	int rc;

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		printk("    fs_open(\"%s\", CREATE|WRITE|TRUNC) -> %d  FAILED\n",
		       path, rc);
		return rc;
	}

	ssize_t put = fs_write(&file, data, len);

	rc = fs_close(&file);
	printk("    fs_write(\"%s\", %u bytes) -> %d, fs_close -> %d\n",
	       path, (unsigned)len, (int)put, rc);
	return (put == (ssize_t)len) ? rc : -EIO;
}

static int list_dir(const char *path)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;
	int rc;

	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, path);
	if (rc != 0) {
		printk("    fs_opendir(\"%s\") -> %d  FAILED\n", path, rc);
		return rc;
	}

	while ((rc = fs_readdir(&dir, &entry)) == 0 && entry.name[0] != '\0') {
		printk("    %-12s %s %u bytes\n", entry.name,
		       entry.type == FS_DIR_ENTRY_DIR ? "dir " : "file",
		       (unsigned)entry.size);
	}
	(void)fs_closedir(&dir);
	return rc;
}

static int run(void)
{
	struct fs_dirent st;
	uint32_t count;
	int rc;

	step(1, "nothing to bring up: devicetree mounted " MNT " at boot");

	/* fs_stat() on the mount point is how to tell that the automount
	 * worked: it failed only with a log line, as littlefs's does. */
	rc = fs_stat(MNT, &st);
	printk("    fs_stat(\"%s\") -> %d\n", MNT, rc);
	if (rc != 0) {
		return rc;
	}

	step(2, "read the boot counter");
	rc = read_counter(&count);
	if (rc != 0) {
		return rc;
	}

	step(3, "write it back, one higher, and leave a note");
	count++;
	rc = write_file(COUNTER_PATH, &count, sizeof(count));
	if (rc != 0) {
		return rc;
	}
	printk("    boot count is now %u\n", count);

	char note[48];
	int n = snprintf(note, sizeof(note), "written on boot %u\n", count);

	rc = write_file(NOTE_PATH, note, (size_t)n);
	if (rc != 0) {
		return rc;
	}

	step(4, "list " MNT);
	return list_dir(MNT);
}

int main(void)
{
	printk("blobfs sample %s — a file system from devicetree, no init code\n",
	       APP_VERSION_STRING);

	int rc = run();

	printk("\n%s (%d)\n", rc == 0 ? "sample complete" : "sample FAILED", rc);
#ifdef CONFIG_ARCH_POSIX
	posix_exit(rc == 0 ? 0 : 1);
#endif
	return 0;
}
