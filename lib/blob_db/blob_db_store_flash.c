/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db storage backend: raw partition via Zephyr's flash_area API.
 * This is the default backend and preserves blob_db's original on-flash
 * behaviour exactly (peb_size = flash sector size, direct byte addressing).
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

#include "blob_db_store.h"

LOG_MODULE_REGISTER(blob_db_store, CONFIG_BLOB_DB_LOG_LEVEL);

#define BLOB_DB_PARTITION_ID  PARTITION_ID(storage_partition)

static const struct flash_area *g_fa;

int blob_db_store_open(struct blob_db_store_geom *geom)
{
	int rc = flash_area_open(BLOB_DB_PARTITION_ID, &g_fa);

	if (rc < 0) {
		LOG_ERR("flash_area_open: %d", rc);
		return rc;
	}

	const size_t fa_size = g_fa->fa_size;

	/* Derive sector size: get the first sector, assume uniform. */
	struct flash_sector sectors[4];
	uint32_t scount = ARRAY_SIZE(sectors);

	rc = flash_area_sectors(g_fa, &scount, sectors);
	if (rc < 0 && rc != -ENOMEM) {
		LOG_ERR("flash_area_get_sectors: %d", rc);
		goto err_close;
	}
	if (scount == 0) {
		LOG_ERR("partition reports zero sectors");
		rc = -EIO;
		goto err_close;
	}

	size_t write_align = flash_area_align(g_fa);

	if (write_align == 0) {
		write_align = 1;
	}

	const size_t peb_size = sectors[0].fs_size;

	for (uint32_t i = 1; i < scount; i++) {
		if (sectors[i].fs_size != peb_size) {
			LOG_ERR("non-uniform sectors not supported");
			rc = -ENOTSUP;
			goto err_close;
		}
	}

	if (fa_size % peb_size != 0) {
		LOG_ERR("partition size %zu not a multiple of sector %zu", fa_size, peb_size);
		rc = -EINVAL;
		goto err_close;
	}

	geom->peb_size = peb_size;
	geom->write_align = write_align;
	geom->n_pebs = (uint16_t)(fa_size / peb_size);
	return 0;

err_close:
	flash_area_close(g_fa);
	g_fa = NULL;
	return rc;
}

void blob_db_store_close(void)
{
	if (g_fa) {
		flash_area_close(g_fa);
		g_fa = NULL;
	}
}

int blob_db_store_read(off_t off, void *buf, size_t len)
{
	return flash_area_read(g_fa, off, buf, len);
}

int blob_db_store_write(off_t off, const void *buf, size_t len)
{
	return flash_area_write(g_fa, off, buf, len);
}

int blob_db_store_erase(off_t off, size_t len)
{
	return flash_area_erase(g_fa, off, len);
}
