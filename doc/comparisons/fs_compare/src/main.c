/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * littlefs vs blobfs — one workload, every variant.
 *
 * The same fs_* calls run against whichever file system the variant
 * fragment (variants/*.conf) enables, so the two stacks answer an identical
 * request stream. On native_sim every flash operation is counted at the
 * flash simulator — below both stacks, so neither is measured by its own
 * instrumentation — and printed per phase as an `io` line that
 * app_perf_l0/tools/l0_timing.py reads:
 *
 *   python3 app_perf_l0/tools/l0_timing.py predict \
 *       -m app_perf_l0/models/mx25r64_nrf5340dk_direct.json run.log
 *
 * Run it twice on the same flash file. The first run finds an erased
 * partition and runs the whole workload; the second finds the files and
 * measures a cold boot: mount, the first write after it, and reading
 * everything back.
 *
 * On a board the phases still run (as a functional check) but nothing is
 * counted; the board build exists for its image size.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#if defined(CONFIG_FLASH_SIMULATOR_STATS)
#include <zephyr/stats/stats.h>
#endif

#if defined(CONFIG_FILE_SYSTEM)
#include <zephyr/fs/fs.h>
#endif

#if defined(CONFIG_FILE_SYSTEM_LITTLEFS)
#include <zephyr/fs/littlefs.h>
#endif

#if defined(CONFIG_BLOBFS_FS_INTEROP)
#include <app/lib/blobfs_fs.h>
#endif

#if defined(CONFIG_BLOBDB_KVDB)
#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>
#include <app/lib/kvdb.h>
#endif

#if defined(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#define N_FILES    32
#define FILE_LEN   128
#define REWRITES   8
#define CHURN      32768
#define APPENDS    15
#define APPEND_LEN 16
#define DIR_FILES  8
#define N_KEYS     32
#define KEY_LEN    32
#define BIG_LEN    4096

/* --- flash I/O counters (native_sim only) ------------------------------ */

struct io {
	uint32_t rd, rdb, wr, wrb, er, erblk;
};

#if defined(CONFIG_FLASH_SIMULATOR_STATS)
static int io_walk(struct stats_hdr *hdr, void *arg, const char *name,
		   uint16_t off)
{
	struct io *io = arg;
	uint32_t v = *(uint32_t *)((uint8_t *)hdr + off);

	if (strcmp(name, "bytes_read") == 0) {
		io->rdb = v;
	} else if (strcmp(name, "bytes_written") == 0) {
		io->wrb = v;
	} else if (strcmp(name, "flash_read_calls") == 0) {
		io->rd = v;
	} else if (strcmp(name, "flash_write_calls") == 0) {
		io->wr = v;
	} else if (strcmp(name, "flash_erase_calls") == 0) {
		io->er = v;
	} else if (strncmp(name, "erase_cycles_unit", 17) == 0) {
		io->erblk += v;
	}
	return 0;
}

static void io_snap(struct io *io)
{
	struct stats_hdr *hdr = stats_group_find("flash_sim_stats");

	memset(io, 0, sizeof(*io));
	if (hdr) {
		stats_walk(hdr, io_walk, io);
	}
}

static uint32_t erase_block_size(void)
{
	const struct flash_area *fa;
	struct flash_pages_info pi = {0};

	if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa) == 0) {
		flash_get_page_info_by_offs(fa->fa_dev, fa->fa_off, &pi);
		flash_area_close(fa);
	}
	return pi.size;
}
#else
static void io_snap(struct io *io)
{
	memset(io, 0, sizeof(*io));
}

static uint32_t erase_block_size(void)
{
	return 0;
}
#endif

static struct io io0;
static uint32_t t0;

static void phase_begin(void)
{
	io_snap(&io0);
	t0 = k_uptime_get_32();
}

static void phase_end(const char *name, int rc, unsigned int ops)
{
	struct io io1;

	io_snap(&io1);
	printf("phase %-12s: rc %d, %u ops\n", name, rc, ops);
	printf("io %s : rd %u ops/ %u B wr %u ops/ %u B er %u ops/ %u B\n",
	       name, io1.rd - io0.rd, io1.rdb - io0.rdb,
	       io1.wr - io0.wr, io1.wrb - io0.wrb,
	       io1.er - io0.er, (io1.erblk - io0.erblk) * erase_block_size());
}

/* --- the file system under test ---------------------------------------- */

#if defined(CONFIG_FILE_SYSTEM_LITTLEFS)
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_data);
static struct fs_mount_t mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &lfs_data,
	.storage_dev = (void *)FIXED_PARTITION_ID(lfs_partition),
	.mnt_point = "/fs",
};
#define HAVE_FS 1
#elif defined(CONFIG_BLOBFS_FS_INTEROP)
static struct fs_mount_t mnt = {
	.type = BLOBFS_FS_TYPE,
	.mnt_point = "/fs",
};
#define HAVE_FS 1
#endif

#if defined(HAVE_FS)

static uint8_t buf[BIG_LEN];

static void path(char *out, size_t n, const char *fmt, unsigned int i)
{
	char name[16];

	snprintf(name, sizeof(name), fmt, i);
	snprintf(out, n, "%s/%s", mnt.mnt_point, name);
}

static int put(const char *p, fs_mode_t flags, size_t off, size_t len,
	       uint8_t fill)
{
	struct fs_file_t f;
	int rc;

	fs_file_t_init(&f);
	rc = fs_open(&f, p, flags);
	if (rc < 0) {
		return rc;
	}
	if (off > 0) {
		rc = fs_seek(&f, off, FS_SEEK_SET);
	}
	memset(buf, fill, len);
	if (rc >= 0) {
		rc = fs_write(&f, buf, len);
		if (rc >= 0 && rc != (int)len) {
			rc = -EIO;
		}
	}
	int crc = fs_close(&f);

	return rc < 0 ? rc : crc;
}

static int get(const char *p, size_t len)
{
	struct fs_file_t f;
	int rc;

	fs_file_t_init(&f);
	rc = fs_open(&f, p, FS_O_READ);
	if (rc < 0) {
		return rc;
	}
	rc = fs_read(&f, buf, len);
	fs_close(&f);
	return rc;
}

static int w_create(void)
{
	char p[32];

	for (unsigned int i = 0; i < N_FILES; i++) {
		path(p, sizeof(p), "c%02u", i);
		int rc = put(p, FS_O_CREATE | FS_O_WRITE, 0, FILE_LEN, i);

		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}

static int w_rewrite(unsigned int n)
{
	char p[32];

	for (unsigned int k = 0; k < n; k++) {
		unsigned int i = k % N_FILES;

		path(p, sizeof(p), "c%02u", i);
		int rc = put(p, FS_O_WRITE, 0, FILE_LEN, (uint8_t)(k + i));

		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}

static int w_append(void)
{
	char p[32];

	path(p, sizeof(p), "log", 0);
	for (unsigned int k = 0; k < APPENDS; k++) {
		int rc = put(p, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND, 0,
			     APPEND_LEN, k);

		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}

static int w_read(void)
{
	char p[32];
	int rc;

	for (unsigned int i = 0; i < N_FILES; i++) {
		path(p, sizeof(p), "c%02u", i);
		rc = get(p, FILE_LEN);
		if (rc != FILE_LEN) {
			return rc < 0 ? rc : -EIO;
		}
	}
	path(p, sizeof(p), "log", 0);
	rc = get(p, APPENDS * APPEND_LEN);
	return rc == APPENDS * APPEND_LEN ? 0 : (rc < 0 ? rc : -EIO);
}

static int w_dir(void)
{
	char p[32], q[32];
	int rc;

	path(p, sizeof(p), "d", 0);
	rc = fs_mkdir(p);
	if (rc < 0) {
		return rc;
	}
	for (unsigned int i = 0; i < DIR_FILES; i++) {
		path(p, sizeof(p), "d/f%u", i);
		rc = put(p, FS_O_CREATE | FS_O_WRITE, 0, FILE_LEN / 2, i);
		if (rc < 0) {
			return rc;
		}
	}
	path(p, sizeof(p), "d/f%u", 0);
	path(q, sizeof(q), "d/g%u", 0);
	rc = fs_rename(p, q);
	if (rc < 0) {
		return rc;
	}
	for (unsigned int i = 1; i <= DIR_FILES / 2; i++) {
		path(p, sizeof(p), "d/f%u", i);
		rc = fs_unlink(p);
		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}

static int w_list(unsigned int *n)
{
	struct fs_dir_t d;
	struct fs_dirent e;
	int rc;

	*n = 0;
	fs_dir_t_init(&d);
	rc = fs_opendir(&d, mnt.mnt_point);
	if (rc < 0) {
		return rc;
	}
	while ((rc = fs_readdir(&d, &e)) == 0 && e.name[0] != '\0') {
		(*n)++;
	}
	fs_closedir(&d);
	return rc;
}

static int w_big(void)
{
	char p[32];

	path(p, sizeof(p), "big", 0);
	return put(p, FS_O_CREATE | FS_O_WRITE, 0, BIG_LEN, 0xa5);
}

#endif /* HAVE_FS */

/* --- the key/value side of the device ---------------------------------- */

#if defined(CONFIG_BLOBDB_KVDB)
static kvdb_t kv;

static int stack_up(void)
{
	int rc = blob_db_mount();

	if (rc != 0 && rc != -EALREADY) {
		return rc;
	}
	rc = rootreg_init();
	if (rc != 0) {
		return rc;
	}
	return kvdb_open(&kv, "kv", NULL);
}

static int w_kv(void)
{
	char key[16];
	uint8_t val[KEY_LEN];

	for (unsigned int i = 0; i < N_KEYS; i++) {
		snprintf(key, sizeof(key), "k%02u", i);
		memset(val, i, sizeof(val));
		int rc = kvdb_set(&kv, key, val, sizeof(val));

		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}
#endif

#if defined(CONFIG_SETTINGS)
static int w_settings(void)
{
	char key[24];
	uint8_t val[KEY_LEN];
	int rc = settings_subsys_init();

	for (unsigned int i = 0; rc == 0 && i < N_KEYS; i++) {
		snprintf(key, sizeof(key), "app/k%02u", i);
		memset(val, i, sizeof(val));
		rc = settings_save_one(key, val, sizeof(val));
	}
	return rc;
}
#endif

int main(void)
{
	int rc = 0;

	printf("fs_compare: erase block %u B\n", erase_block_size());

#if defined(CONFIG_BLOBDB_KVDB)
	phase_begin();
	rc = stack_up();
	phase_end("stack_up", rc, 1);
#endif

#if defined(HAVE_FS)
	struct fs_dirent st;
	char p[32];
	unsigned int n = 0;

	phase_begin();
	rc = fs_mount(&mnt);
	phase_end("fs_mount", rc, 1);
	if (rc < 0) {
		return 0;
	}

	path(p, sizeof(p), "c%02u", 0);
	if (fs_stat(p, &st) == 0) {
		/* Second run: this was a cold boot onto a populated store. */
		phase_begin();
		rc = w_rewrite(1);
		phase_end("first_write", rc, 1);

		phase_begin();
		rc = w_read();
		phase_end("boot_read", rc, N_FILES + 1);
		printf("fs_compare: done (boot)\n");
		return 0;
	}

	phase_begin();
	rc = w_create();
	phase_end("create", rc, N_FILES);

	phase_begin();
	rc = w_rewrite(N_FILES * REWRITES);
	phase_end("rewrite", rc, N_FILES * REWRITES);

	phase_begin();
	rc = w_append();
	phase_end("append", rc, APPENDS);

	phase_begin();
	rc = w_read();
	phase_end("read", rc, N_FILES + 1);

	phase_begin();
	rc = w_dir();
	phase_end("dir", rc, 1 + DIR_FILES + 1 + DIR_FILES / 2);

	phase_begin();
	rc = w_list(&n);
	phase_end("list", rc, n);

	phase_begin();
	rc = w_rewrite(CHURN);
	phase_end("churn", rc, CHURN);
#endif

#if defined(CONFIG_BLOBDB_KVDB)
	phase_begin();
	rc = w_kv();
	phase_end("kv_set", rc, N_KEYS);
#endif

#if defined(CONFIG_SETTINGS)
	phase_begin();
	rc = w_settings();
	phase_end("settings", rc, N_KEYS);
#endif

#if defined(HAVE_FS)
	phase_begin();
	rc = w_big();
	phase_end("big_file", rc, 1);

	struct fs_statvfs sv;

	rc = fs_statvfs(mnt.mnt_point, &sv);
	if (rc == 0) {
		printf("statvfs: %lu blocks of %lu B, %lu free\n",
		       (unsigned long)sv.f_blocks, (unsigned long)sv.f_frsize,
		       (unsigned long)sv.f_bfree);
	} else {
		printf("statvfs: rc %d\n", rc);
	}

	rc = fs_unmount(&mnt);
	printf("fs_unmount: rc %d\n", rc);
#endif

	printf("fs_compare: done\n");
	return 0;
}
