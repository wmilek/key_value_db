/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>

#include <app_version.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

int main(void)
{
	printk("Zephyr Key-Value DB %s\n", APP_VERSION_STRING);

	int rc = blob_db_mount();
	if (rc < 0) {
		LOG_ERR("blob_db_mount failed: %d", rc);
		return 0;
	}
	LOG_INF("blob_db mounted");

	/* Stage 3: nothing else to do; subsequent stages will exercise the API. */

	blob_db_unmount();
	LOG_INF("blob_db unmounted; bye");
	return 0;
}
