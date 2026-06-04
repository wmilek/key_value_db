/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db demo — increments a "boot count" persisted at the root id (1)
 * across reboots, exercising the stability contract documented in
 * include/app/lib/blob_db.h.
 *
 * Run with a backing flash file to see the count persist:
 *   ./zephyr.exe --flash=/tmp/blob.bin --stop_at=1
 * Run again — it should print boot_count = 2.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>

#include <app_version.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define BLOB_DB_ROOT_ID  1

int main(void)
{
	printk("Zephyr Key-Value DB %s\n", APP_VERSION_STRING);

	int rc = blob_db_mount();
	if (rc < 0) {
		LOG_ERR("blob_db_mount failed: %d", rc);
		return 0;
	}

	/* Read the root blob (boot count). On first run it doesn't exist
	 * yet; put a fresh one which (by the root convention) becomes id=1. */
	uint32_t boot_count = 0;
	size_t got;

	rc = blob_db_get(BLOB_DB_ROOT_ID, &boot_count, sizeof(boot_count), &got);
	if (rc == -ENOENT) {
		boot_count = 1;
		uint64_t assigned;

		rc = blob_db_put(&boot_count, sizeof(boot_count), &assigned);
		if (rc < 0) {
			LOG_ERR("first-boot put failed: %d", rc);
			goto out;
		}
		if (assigned != BLOB_DB_ROOT_ID) {
			LOG_ERR("root-convention broken: first put returned id=%llu "
				"(expected %u)",
				(unsigned long long)assigned, BLOB_DB_ROOT_ID);
			goto out;
		}
		LOG_INF("first boot — wrote boot_count=1 at id=%u", BLOB_DB_ROOT_ID);
	} else if (rc == 0) {
		if (got != sizeof(boot_count)) {
			LOG_ERR("root blob has unexpected size %zu", got);
			goto out;
		}
		boot_count++;
		rc = blob_db_update(BLOB_DB_ROOT_ID, &boot_count, sizeof(boot_count));
		if (rc < 0) {
			LOG_ERR("update root failed: %d", rc);
			goto out;
		}
		LOG_INF("boot #%u", boot_count);
	} else {
		LOG_ERR("get root failed: %d", rc);
		goto out;
	}

	LOG_INF("blobs in store: %zu", blob_db_count());

out:
	blob_db_unmount();
	return 0;
}
