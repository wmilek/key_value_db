/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_footprint — a caller for every public entry point of each layer the
 * build enables, so the linker keeps what a real application would link.
 *
 * Nothing here is selected by an option of the app's own: each block is gated
 * on the library's Kconfig symbol, and the build's fragments (stages/,
 * variants/) decide which are on. Only the topmost raw layer is driven
 * directly — flash_area when nothing sits on it, UBI when blob_db does not —
 * because once blob_db is in, it is blob_db that calls them.
 *
 * The image is for measuring, not for running: it does what a minimal real
 * caller would, and checks nothing beyond the return codes.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#if defined(CONFIG_FLASH_MAP)
#include <zephyr/storage/flash_map.h>
#endif

#if defined(CONFIG_UBI) && !defined(CONFIG_BLOB_DB)
#include <psa/crypto.h>
#include <ubi/ubi.h>
#endif

#if defined(CONFIG_BLOB_DB)
#include <app/lib/blob_db.h>
#endif
#if defined(CONFIG_BLOB_DB_IOSTATS)
#include <app/lib/blob_db_iostats.h>
#endif
#if defined(CONFIG_BLOB_DB_INSPECT)
#include <app/lib/blob_db_inspect.h>
#endif
#if defined(CONFIG_BLOB_ROOTREG)
#include <app/lib/rootreg.h>
#endif
#if defined(CONFIG_BLOB_CONTAINER_KVHASH)
#include <app/lib/containers/kvhash.h>
#endif
#if defined(CONFIG_BLOB_CONTAINER_LOGRING)
#include <app/lib/containers/logring.h>
#endif
#if defined(CONFIG_BLOBDB_KVDB)
#include <app/lib/kvdb.h>
#endif
#if defined(CONFIG_BLOBDB_BLOBFS)
#include <app/lib/blobfs.h>
#endif

#define STORAGE_ID PARTITION_ID(storage_partition)

static __maybe_unused uint8_t buf[64];

/* Folds every result into one value printed at the end, so no call is dead. */
static int acc;

static __maybe_unused void use(int rc)
{
	acc = acc * 31 + rc;
}

/* --- L0, raw: the flash driver and flash_map, with nothing on top --------- */

#if defined(CONFIG_FLASH_MAP) && !defined(CONFIG_UBI) && !defined(CONFIG_BLOB_DB)
static void run_flash_area(void)
{
	const struct flash_area *fa;
	int rc = flash_area_open(STORAGE_ID, &fa);

	use(rc);
	if (rc != 0) {
		return;
	}
	use(flash_area_erase(fa, 0, 4096));
	use(flash_area_write(fa, 0, buf, sizeof(buf)));
	use(flash_area_read(fa, 0, buf, sizeof(buf)));
	use((int)fa->fa_size);
	flash_area_close(fa);
}
#endif

/* --- L0, UBI: driven directly, the calls blob_db's UBI backend makes ------ */

#if defined(CONFIG_UBI) && !defined(CONFIG_BLOB_DB)
static enum ubi_state_verdict on_state(const struct ubi_device_info *info,
				       void *user)
{
	ARG_UNUSED(info);
	ARG_UNUSED(user);
	return UBI_STATE_TRUSTED;
}

static void run_ubi(void)
{
	static const uint8_t ikm[32] = "footprint UBI key, not secret";
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	struct ubi_config config = {
		.flash_area_id = STORAGE_ID,
		.state_cb = on_state,
	};
	struct ubi_maintenance_result result;
	struct ubi_volume_info vinfo;
	struct ubi_device_info dinfo;
	struct ubi_device *ubi;
	uint32_t vol;
	int rc;

	if (psa_crypto_init() != PSA_SUCCESS) {
		return;
	}
	psa_set_key_type(&attr, PSA_KEY_TYPE_DERIVE);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	if (psa_import_key(&attr, ikm, sizeof(ikm), &config.ikm_key_id) != PSA_SUCCESS) {
		return;
	}

	ubi = k_calloc(1, ubi_device_size());
	if (ubi == NULL) {
		return;
	}

	rc = ubi_device_init(ubi, &config);
	if (rc == -ENODEV) {
		use(ubi_device_format(&config));
		rc = ubi_device_init(ubi, &config);
	}
	use(rc);
	use(ubi_device_get_info(ubi, &dinfo));

	rc = ubi_volume_find(ubi, "fp", &vol);
	if (rc == -ENOENT) {
		const struct ubi_volume_config want = { .name = "fp", .leb_count = 4 };

		rc = ubi_volume_create(ubi, &want, &vol);
	}
	use(rc);
	use(ubi_volume_get_info(ubi, vol, &vinfo));

	use(ubi_leb_write_at(ubi, vol, 0, 0, buf, sizeof(buf)));
	use(ubi_leb_read(ubi, vol, 0, 0, buf, sizeof(buf)));
	use(ubi_leb_change(ubi, vol, 1, buf, sizeof(buf)));
	use(ubi_leb_erase(ubi, vol, 1));
	use(ubi_leb_unmap(ubi, vol, 0));
	use(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	use(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));

	use(ubi_device_deinit(ubi));
	k_free(ubi);
	psa_destroy_key(config.ikm_key_id);
}
#endif

/* --- L1, blob_db ---------------------------------------------------------- */

#if defined(CONFIG_BLOB_DB)
static int count_cb(uint64_t id, const void *payload, size_t len, void *user)
{
	ARG_UNUSED(id);
	ARG_UNUSED(payload);
	*(size_t *)user += len;
	return 0;
}

static void run_blob_db(void)
{
	size_t len = 0;
	uint64_t id;

	use(blob_db_mount());
	use(blob_db_prepare(1));
	id = blob_db_alloc_id();
	use(blob_db_update(id, buf, sizeof(buf)));
	use(blob_db_get(id, buf, sizeof(buf), &len));
	use(blob_db_size(id, &len));
	use(blob_db_read(id, 1, buf, 8, &len));
	use(blob_db_write(id, 1, buf, 8));
	use(blob_db_exists(id));
	use((int)blob_db_count());
	use(blob_db_iterate(count_cb, &len));
	use(blob_db_delete(id));
#if defined(CONFIG_BLOB_DB_IOSTATS)
	{
		struct blob_db_iostats io;

		blob_db_iostats_get(&io);
		use((int)io.erases);
		blob_db_iostats_reset();
	}
#endif
#if defined(CONFIG_BLOB_DB_INSPECT)
	{
		struct blob_db_inspect_info info;
		struct blob_db_inspect_bucket bucket;

		use(blob_db_inspect_info_get(&info));
		use(blob_db_inspect_bucket_get(1, &bucket, NULL, NULL));
	}
#endif
	if (acc == 0x5a5a5a5a) {	/* never; keeps the destructive calls linked */
		use(blob_db_erase_all());
		use(blob_db_format());
	}
}
#endif

/* --- L1½, rootreg --------------------------------------------------------- */

#if defined(CONFIG_BLOB_ROOTREG)
static int root_cb(uint64_t key, uint64_t root_id, void *user)
{
	ARG_UNUSED(key);
	*(uint64_t *)user ^= root_id;
	return 0;
}

static void run_rootreg(void)
{
	const uint64_t key = ROOTREG_KEY(0x46545052u /* 'FTPR' */, 0);
	uint64_t root = 0;

	use(rootreg_init());
	use(rootreg_get_or_create(key, &root));
	use(rootreg_get(key, &root));
	use(rootreg_set(key, root));
	use(rootreg_iterate(root_cb, &root));
	use(rootreg_unregister(key));
}
#endif

/* --- L2, containers ------------------------------------------------------- */

#if defined(CONFIG_BLOB_CONTAINER_KVHASH)
static void run_kvhash(void)
{
	const struct map_ops *m = &kvhash_map_ops;
	const struct map_config cfg = { .expected_entries = 32 };
	struct map_info info;
	size_t klen, vlen, n;
	uint8_t k[16];
	uint64_t root = blob_db_alloc_id();

	use(m->create(root, &cfg));
	use(m->stat(root, &info));
	use(m->set(root, "k", 1, buf, 16));
	use(m->get(root, "k", 1, buf, sizeof(buf), &vlen));
	use(m->next(root, NULL, 0, k, sizeof(k), &klen, buf, sizeof(buf), &vlen));
	use(m->count(root, &n));
	use(m->del(root, "k", 1));
	use(m->destroy(root));
}
#endif

#if defined(CONFIG_BLOB_CONTAINER_LOGRING)
static void run_logring(void)
{
	const struct logring_cfg cfg = { .capacity_bytes = 4096 };
	struct logring_stats st;
	logring_cursor c;
	logring_t h;
	uint64_t root, id;
	size_t len;

	use(logring_create(&cfg, &root));
	use(logring_open(root, &h));
	use(logring_append(&h, buf, 16, &id));
	use(logring_seek_oldest(&h, &c));
	use(logring_next(&h, &c, buf, sizeof(buf), &len, &id));
	use(logring_seek_newest(&h, &c));
	use(logring_stats(&h, &st));
	use(logring_reset(&h));
	use(logring_destroy(&h));
}
#endif

/* --- L3, interfaces ------------------------------------------------------- */

#if defined(CONFIG_BLOBDB_KVDB)
static void run_kvdb(void)
{
	size_t len;
	kvdb_t db;

	use(kvdb_open(&db, "fp", NULL));
	use(kvdb_set(&db, "a.b", buf, 8));
	use(kvdb_get(&db, "a.b", buf, sizeof(buf), &len));
	use(kvdb_has(&db, "a.b"));
	use(kvdb_delete(&db, "a.b"));
}
#endif

#if defined(CONFIG_BLOBDB_BLOBFS)
static void run_blobfs(void)
{
	struct blobfs_stat st;
	size_t len;

	use(blobfs_mount());
	use(blobfs_mkdir("/d"));
	use(blobfs_create("/d/f"));
	use(blobfs_write("/d/f", 0, buf, 16));
	use(blobfs_read("/d/f", 0, buf, 16, &len));
	use(blobfs_stat("/d/f", &st));
	use(blobfs_rename("/d/f", "/d/g"));
	use(blobfs_truncate("/d/g", 4));
	use(blobfs_unlink("/d/g"));
	use(blobfs_unmount());
}
#endif

int main(void)
{
	memset(buf, 0xa5, sizeof(buf));

#if defined(CONFIG_FLASH_MAP) && !defined(CONFIG_UBI) && !defined(CONFIG_BLOB_DB)
	run_flash_area();
#endif
#if defined(CONFIG_UBI) && !defined(CONFIG_BLOB_DB)
	run_ubi();
#endif
#if defined(CONFIG_BLOB_DB)
	run_blob_db();
#endif
#if defined(CONFIG_BLOB_ROOTREG)
	run_rootreg();
#endif
#if defined(CONFIG_BLOB_CONTAINER_KVHASH)
	run_kvhash();
#endif
#if defined(CONFIG_BLOB_CONTAINER_LOGRING)
	run_logring();
#endif
#if defined(CONFIG_BLOBDB_KVDB)
	run_kvdb();
#endif
#if defined(CONFIG_BLOBDB_BLOBFS)
	run_blobfs();
#endif

	printk("footprint: %d\n", acc);
	return 0;
}
