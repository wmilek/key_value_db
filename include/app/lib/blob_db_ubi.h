/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BLOB_DB_UBI_H_
#define APP_LIB_BLOB_DB_UBI_H_

#include <psa/crypto_types.h>
#include <ubi/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_blob_db_ubi blob_db UBI backend hooks
 * @ingroup lib_blob_db
 * @{
 *
 * @brief What an application supplies, and what it may drive, when blob_db
 *        runs on a UBI volume (CONFIG_BLOB_DB_BACKEND_UBI).
 *
 * zephyr-ubi seals its own metadata with AES-CMAC under keys derived from a
 * PSA key handle, and asks the application whether to trust the device at
 * every attach and every CONFIG_UBI_STATE_CHECK_INTERVAL writes. blob_db owns
 * the UBI device, so these two decisions reach the application through the
 * hooks below. Application data is not encrypted either way.
 *
 * The same ownership means the application cannot reach UBI's maintenance
 * itself, so blob_db forwards it: blob_db_ubi_maintenance() and
 * blob_db_ubi_device_info(). Like the rest of the UBI backend they are outside
 * the blob_db contract; containers and interfaces must not depend on them.
 */

/**
 * @brief Hand blob_db the keying material UBI derives its keys from.
 *
 * Called by blob_db_mount() (and blob_db_format() on an unmountable store)
 * after psa_crypto_init(), each time the store is opened. Defined by the
 * application when CONFIG_BLOB_DB_UBI_KEY_APP is set; the build fails to link
 * otherwise.
 *
 * The key must carry PSA_KEY_USAGE_DERIVE, permit
 * PSA_ALG_HKDF(PSA_ALG_SHA_256), be unique per device and be the same at every
 * boot: other keying material makes the store unreadable. blob_db never
 * destroys it.
 *
 * @param[out] key_id Handle of the key in the PSA key store.
 *
 * @retval 0 Success.
 * @retval <0 Negative errno; blob_db_mount() returns it.
 */
int blob_db_ubi_ikm_key(psa_key_id_t *key_id);

/**
 * @brief Decide whether UBI may carry on with the device as it stands.
 *
 * Rollback detection belongs here: compare @p info with a store the flash
 * cannot reach (PSA ITS, a monotonic counter) and commit the new values there
 * before trusting. Runs with UBI's device lock held, so it must not block or
 * call into blob_db or UBI.
 *
 * blob_db supplies a weak definition that trusts every device. Define this
 * function in the application to replace it.
 *
 * @param info State of the device, from RAM.
 *
 * @return UBI_STATE_UNTRUSTED makes blob_db_mount() fail with -EROFS, or every
 *         later write fail with -EROFS until the next mount.
 */
enum ubi_state_verdict blob_db_ubi_state_check(const struct ubi_device_info *info);

/**
 * @brief Report the state of the UBI device under the mounted store.
 *
 * The counters that say what maintenance would do: @c free_pebs ready for a
 * write, @c reclaimable_pebs a write would otherwise erase first,
 * @c relocatable_pebs wear levelling would move, @c corrupt_pebs and
 * @c bad_pebs out of service, and the erase count spread.
 *
 * @param[out] info Device state, from RAM; nothing is read from flash.
 *
 * @retval 0 Success.
 * @retval -ENODEV Not mounted.
 * @retval <0 Negative errno from ubi_device_get_info().
 */
int blob_db_ubi_device_info(struct ubi_device_info *info);

/**
 * @brief Run one UBI maintenance operation on the mounted store, up to a
 *        budget of steps.
 *
 * blob_db_maintain() already runs the two operations that keep erases out
 * of blob_db's own calls, RECLAIM and then RELOCATE, and is the call to use
 * for that. This one runs a single operation by name: for an application
 * that schedules them itself, and for DISCARD, which blob_db never runs:
 *
 * - @c UBI_MAINTENANCE_RECLAIM erases a block left blank by a device format
 *   or released by a replace, and adds it to the free pool.
 * - @c UBI_MAINTENANCE_RELOCATE moves rarely rewritten data onto a more worn
 *   block, then erases the block it left.
 * - @c UBI_MAINTENANCE_REPAIR restores the volume table copies and retries
 *   retired blocks. blob_db_mount() runs it to completion.
 * - @c UBI_MAINTENANCE_DISCARD erases the blocks UBI keeps as corrupt and
 *   returns them to service. blob_db never runs it: such a block may hold
 *   the only copy of something, and only the application can decide it is
 *   not worth keeping. UBI refuses to attach once they reach 1/20 of the
 *   device.
 *
 * Each step erases at least one block, and the blob_db lock is held for the
 * whole call, so other threads' blob_db calls wait @p budget erases. None of
 * the operations changes what blob_db reads back.
 *
 * @param op Operation to run.
 * @param budget Most steps to run. 0 runs none and only reports the work
 *        left in @p result.
 * @param[out] result Steps done and left; may be NULL.
 *
 * @retval 0 Done, or nothing to do.
 * @retval -ENODEV Not mounted.
 * @retval -EINVAL @p op is not an operation UBI knows.
 * @retval <0 Negative errno from ubi_maintenance(): -EROFS once the device
 *         is read-only, -EBADMSG for a block that failed verification and
 *         stayed where it was, -EIO for a flash failure.
 */
int blob_db_ubi_maintenance(enum ubi_maintenance_op op, uint32_t budget,
			    struct ubi_maintenance_result *result);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_BLOB_DB_UBI_H_ */
