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
 * @brief What an application supplies when blob_db runs on a UBI volume
 *        (CONFIG_BLOB_DB_BACKEND_UBI).
 *
 * zephyr-ubi seals its own metadata with AES-CMAC under keys derived from a
 * PSA key handle, and asks the application whether to trust the device at
 * every attach and every CONFIG_UBI_STATE_CHECK_INTERVAL writes. blob_db owns
 * the UBI device, so these two decisions reach the application through the
 * hooks below. Application data is not encrypted either way.
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

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_BLOB_DB_UBI_H_ */
