/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db storage backend: a dynamic UBI volume.
 *
 * Each blob_db PEB maps 1:1 onto a UBI LEB. blob_db's core access pattern —
 * erase a bucket, write its header, then append slot records at growing
 * offsets — maps directly onto UBI's operations:
 *
 *   blob_db_store_erase(peb)          -> ubi_leb_unmap(lnum) + dirty reclaim
 *   blob_db_store_write(peb, off, ..) -> ubi_leb_write_at(lnum, off, ..)  (in place)
 *   blob_db_store_read (peb, off, ..) -> ubi_leb_read(lnum, off, ..)
 *
 * The in-place slot append is exactly ubi_leb_write_at(): no read-modify-write,
 * no whole-LEB rewrite. An unmapped (erased) LEB reads back as 0xff so
 * blob_db's "erased sector" expectations hold.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include <ubi.h>

#include "blob_db_store.h"

LOG_MODULE_REGISTER(blob_db_store, CONFIG_BLOB_DB_LOG_LEVEL);

#define BLOB_DB_UBI_PARTITION   storage_partition
#define BLOB_DB_UBI_VOL_NAME    "blobdb"

/* PEBs held back from the volume so erase/rewrite churn always has a free PEB
 * to map into after an unmapped block is reclaimed to the dirty pool. */
#define BLOB_DB_UBI_SPARE_PEBS  4

/* Upper bound on volume ids probed when re-attaching an existing volume. */
#define BLOB_DB_UBI_VOL_PROBE   16

static struct ubi_device *g_ubi;
static int g_vol_id = -1;
static size_t g_leb_size;

static int find_existing_volume(struct ubi_device *ubi, int *vol_id_out)
{
	for (int id = 0; id < BLOB_DB_UBI_VOL_PROBE; id++) {
		struct ubi_volume_config cfg = { 0 };
		size_t alloc_lebs = 0;

		if (ubi_volume_get_info(ubi, id, &cfg, &alloc_lebs) != 0) {
			continue;
		}
		if (strncmp(cfg.name, BLOB_DB_UBI_VOL_NAME, sizeof(cfg.name)) == 0) {
			*vol_id_out = id;
			return 0;
		}
	}
	return -ENOENT;
}

int blob_db_store_open(struct blob_db_store_geom *geom)
{
	const struct device *flash_dev = PARTITION_DEVICE(BLOB_DB_UBI_PARTITION);
	struct flash_pages_info page_info = { 0 };

	int rc = flash_get_page_info_by_offs(flash_dev, 0, &page_info);

	if (rc != 0) {
		LOG_ERR("flash page info: %d", rc);
		return rc;
	}

	struct ubi_flash_desc flash = {
		.partition_id = PARTITION_ID(BLOB_DB_UBI_PARTITION),
		.erase_block_size = page_info.size,
		.write_block_size = flash_get_write_block_size(flash_dev),
	};

	rc = ubi_device_init(&flash, NULL, &g_ubi);
	if (rc != 0) {
		LOG_ERR("ubi_device_init: %d", rc);
		return rc;
	}

	struct ubi_device_info info = { 0 };

	rc = ubi_device_get_info(g_ubi, &info);
	if (rc != 0) {
		LOG_ERR("ubi_device_get_info: %d", rc);
		goto err_deinit;
	}
	g_leb_size = info.leb_size;

	if (info.total_peb_count <= BLOB_DB_UBI_SPARE_PEBS) {
		LOG_ERR("device too small: %zu PEBs", info.total_peb_count);
		rc = -EINVAL;
		goto err_deinit;
	}
	const size_t leb_count = info.total_peb_count - BLOB_DB_UBI_SPARE_PEBS;

	/* Re-attach an existing volume, else create one. leb_count is derived
	 * deterministically from device geometry, so it is stable across boots
	 * (blob_db hashes ids modulo the PEB count and must see the same value). */
	rc = find_existing_volume(g_ubi, &g_vol_id);
	if (rc == -ENOENT) {
		struct ubi_volume_config cfg = {
			.type = UBI_VOLUME_TYPE_DYNAMIC,
			.leb_count = leb_count,
		};
		strncpy(cfg.name, BLOB_DB_UBI_VOL_NAME, sizeof(cfg.name) - 1);

		rc = ubi_volume_create(g_ubi, &cfg, &g_vol_id);
		if (rc != 0) {
			LOG_ERR("ubi_volume_create: %d", rc);
			goto err_deinit;
		}
		LOG_INF("created UBI volume '%s' id=%d leb_count=%zu",
			BLOB_DB_UBI_VOL_NAME, g_vol_id, leb_count);
	} else if (rc != 0) {
		goto err_deinit;
	} else {
		LOG_INF("attached UBI volume '%s' id=%d", BLOB_DB_UBI_VOL_NAME, g_vol_id);
	}

	geom->peb_size = g_leb_size;
	geom->write_align = flash.write_block_size ? flash.write_block_size : 1;
	geom->n_pebs = (uint16_t)leb_count;
	return 0;

err_deinit:
	ubi_device_deinit(g_ubi);
	g_ubi = NULL;
	g_vol_id = -1;
	return rc;
}

void blob_db_store_close(void)
{
	if (g_ubi) {
		ubi_device_deinit(g_ubi);
		g_ubi = NULL;
		g_vol_id = -1;
	}
}

int blob_db_store_read(off_t off, void *buf, size_t len)
{
	const size_t lnum = (size_t)off / g_leb_size;
	const size_t within = (size_t)off % g_leb_size;

	/* An unmapped (never-written / erased) LEB has no backing PEB; blob_db
	 * expects erased flash to read as 0xff. Checking is_mapped first avoids
	 * the per-read error log ubi_leb_read emits on an unmapped LEB (blob_db
	 * probes every bucket at mount, most of them empty). */
	bool mapped = false;

	if (ubi_leb_is_mapped(g_ubi, g_vol_id, lnum, &mapped) == 0 && !mapped) {
		memset(buf, 0xff, len);
		return 0;
	}

	int rc = ubi_leb_read(g_ubi, g_vol_id, lnum, within, buf, len);

	if (rc == -ENOENT) {
		memset(buf, 0xff, len);
		return 0;
	}
	return rc;
}

int blob_db_store_write(off_t off, const void *buf, size_t len)
{
	const size_t lnum = (size_t)off / g_leb_size;
	const size_t within = (size_t)off % g_leb_size;

	return ubi_leb_write_at(g_ubi, g_vol_id, lnum, within, buf, len);
}

int blob_db_store_erase(off_t off, size_t len)
{
	if (len == 0) {
		return 0;
	}

	const size_t first = (size_t)off / g_leb_size;
	const size_t last = ((size_t)off + len - 1) / g_leb_size;

	for (size_t lnum = first; lnum <= last; lnum++) {
		int rc = ubi_leb_unmap(g_ubi, g_vol_id, lnum);

		if (rc != 0) {
			LOG_ERR("ubi_leb_unmap(%zu): %d", lnum, rc);
			return rc;
		}
	}

	/* Reclaim the just-dirtied PEBs so the free pool keeps up with the
	 * erase/rewrite churn. Bounded so a stuck reclaim can't spin. */
	for (size_t i = 0; i <= (last - first); i++) {
		struct ubi_device_info info = { 0 };

		if (ubi_device_get_info(g_ubi, &info) != 0 || info.dirty_peb_count == 0) {
			break;
		}
		if (ubi_device_erase_peb(g_ubi) != 0) {
			break;
		}
	}

	return 0;
}
