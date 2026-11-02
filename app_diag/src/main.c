/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * kvdb diagnostics: an application that does nothing but expose the shell.
 *
 * The commands are the ones the libraries and Zephyr register (blob_db,
 * flash, kernel, device); this file adds none. It says what was built, and
 * mounts the store only when CONFIG_APP_DIAG_MOUNT_AT_BOOT asks for it: by
 * default nothing writes the storage partition until a command typed at the
 * shell does (`blob_db mount`, `blob_db format yes`, `flash write`).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

#include <app/lib/blob_db.h>

LOG_MODULE_REGISTER(app_diag, CONFIG_APP_DIAG_LOG_LEVEL);

int main(void)
{
	LOG_INF("kvdb diag: %s backend, storage partition %u KiB on %s",
		IS_ENABLED(CONFIG_BLOB_DB_BACKEND_UBI) ? "UBI" : "flash_area",
		(unsigned int)(PARTITION_SIZE(storage_partition) / 1024U),
		PARTITION_DEVICE(storage_partition)->name);

	if (!IS_ENABLED(CONFIG_APP_DIAG_MOUNT_AT_BOOT)) {
		LOG_INF("kvdb diag: not mounted; `blob_db mount` mounts");
		return 0;
	}

	int rc = blob_db_mount();

	if (rc != 0) {
		LOG_ERR("kvdb diag: mount failed: %d", rc);
		return 0;
	}
	LOG_INF("kvdb diag: mounted");
	return 0;
}
