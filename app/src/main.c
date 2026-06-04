/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app_version.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

int main(void)
{
	printk("Zephyr Key-Value DB %s\n", APP_VERSION_STRING);

	LOG_INF("Application started");
	LOG_DBG("Debug logging is enabled");

	while (1) {
		LOG_DBG("tick");
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
