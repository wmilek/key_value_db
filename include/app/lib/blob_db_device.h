/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BLOB_DB_DEVICE_H_
#define APP_LIB_BLOB_DB_DEVICE_H_

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

/**
 * @defgroup lib_blob_db_device blob_db as a Zephyr device
 * @ingroup lib_blob_db
 * @{
 *
 * @brief The devicetree node that configures blob_db, seen as a device.
 *
 * The store is described in devicetree: a `kvdb,blob-db` node, named by the
 * chosen node `kvdb,blob-db`, whose `storage` property points at a UBI volume
 * or a fixed partition (dts/bindings/storage/kvdb,blob-db.yaml). That node is
 * a device, which is what lets an application use the store without calling
 * an init function:
 *
 * - **`automount`**: the device's init mounts the store before `main()`.
 *   `device_is_ready(BLOB_DB_DEVICE)` is false when that failed; the store
 *   is then mounted by the first blob_db call, which returns the error if it
 *   fails again.
 * - **no `automount`**: init does nothing and the device is always ready.
 *   The first blob_db call that needs the store mounts it.
 * - **`zephyr,deferred-init`**: init waits for the application to call
 *   `device_init(BLOB_DB_DEVICE)`, for instance after it has provisioned
 *   the UBI key (`CONFIG_BLOB_DB_UBI_KEY_APP`). With `automount`, that call
 *   mounts the store and returns the mount's result.
 *
 * `blob_db_mount()`, `blob_db_unmount()` and `blob_db_format()` keep their
 * meaning. After an explicit `blob_db_unmount()` the store stays unmounted
 * (calls report -ENODEV) until `blob_db_mount()` or `blob_db_format()`.
 */

/** The blob_db device, for `device_is_ready()` and `device_init()`. */
#define BLOB_DB_DEVICE DEVICE_DT_GET(DT_CHOSEN(kvdb_blob_db))

/** @} */

#endif /* APP_LIB_BLOB_DB_DEVICE_H_ */
