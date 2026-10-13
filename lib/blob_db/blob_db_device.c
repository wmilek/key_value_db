/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * The kvdb,blob-db devicetree node as a Zephyr device
 * (include/app/lib/blob_db_device.h).
 *
 * The device carries no API: blob_db stays a handle-less singleton. It exists
 * for initialization -- ordering after the flash driver, mounting at boot when
 * the node says "automount", zephyr,deferred-init -- and so device_is_ready()
 * can report whether that mount worked.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>

#include "blob_db_dt.h"

LOG_MODULE_DECLARE(blob_db, CONFIG_BLOB_DB_LOG_LEVEL);

/* blob_db's state is a static singleton: one store per image. */
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(kvdb_blob_db) == 1,
	     "blob_db supports exactly one enabled kvdb,blob-db node");

static int blob_db_device_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	if (!DT_PROP(BLOB_DB_DT_NODE, automount)) {
		return 0;   /* mounted by the first call that needs it */
	}

	int rc = blob_db_mount();

	if (rc == -EALREADY) {
		return 0;   /* something already mounted it: as good */
	}
	if (rc != 0) {
		LOG_ERR("automount failed: %d", rc);
	}
	return rc;
}

DEVICE_DT_DEFINE(BLOB_DB_DT_NODE, blob_db_device_init, NULL, NULL, NULL,
		 POST_KERNEL, CONFIG_BLOB_DB_INIT_PRIORITY, NULL);
