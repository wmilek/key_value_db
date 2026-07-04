/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db demo — increments a boot counter persisted at BLOB_DB_ROOT_ID
 * across reboots, exercising the "root always exists" contract documented
 * in include/app/lib/blob_db.h (@blob_db_root).
 *
 * Every ERASE_EVERY_N boots the demo calls blob_db_erase_all() to show the
 * fast wipe: prior blobs vanish, root is re-bound with an empty payload,
 * next allocation restarts at 2, and the boot counter resets to 1.
 *
 * Run natively with a backing flash file to see the count persist:
 *   ./zephyr.exe --flash=/tmp/blob.bin --stop_at=1
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>

#include <zephyr/app_version.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define ERASE_EVERY_N  5

int main(void)
{
	printk("Zephyr Key-Value DB %s\n", APP_VERSION_STRING);

	int rc = blob_db_mount();
	if (rc < 0) {
		LOG_ERR("blob_db_mount failed: %d", rc);
		return 0;
	}

	uint32_t boot_count;
	size_t got;

	/* Root is guaranteed live after mount; distinguish virgin from used
	 * by the payload size — format() leaves it empty. */
	rc = blob_db_get(BLOB_DB_ROOT_ID, &boot_count, sizeof(boot_count), &got);
	if (rc < 0) {
		LOG_ERR("get root failed: %d", rc);
		goto out;
	}

	if (got == 0) {
		boot_count = 1;
		LOG_INF("first boot on a fresh store — boot_count=1");
	} else if (got == sizeof(boot_count)) {
		boot_count++;
		LOG_INF("boot #%u", boot_count);
	} else {
		LOG_ERR("root blob has unexpected size %zu", got);
		goto out;
	}

	rc = blob_db_update(BLOB_DB_ROOT_ID, &boot_count, sizeof(boot_count));
	if (rc < 0) {
		LOG_ERR("update root failed: %d", rc);
		goto out;
	}

	LOG_INF("blobs in store: %zu", blob_db_count());

	if (boot_count % ERASE_EVERY_N == 0) {
		LOG_INF("boot_count reached %u — exercising blob_db_erase_all()",
			boot_count);
		rc = blob_db_erase_all();
		if (rc < 0) {
			LOG_ERR("erase_all failed: %d", rc);
			goto out;
		}
		LOG_INF("erase_all done; blobs in store now: %zu "
			"(next boot resets counter)", blob_db_count());
	}

out:
	blob_db_unmount();
	return 0;
}
