/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BLOB_DB_SECURITY_H_
#define APP_LIB_BLOB_DB_SECURITY_H_

#include <stdint.h>

#if defined(CONFIG_BLOB_DB_KEYS)
#include <psa/crypto_types.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_blob_db_security blob_db security hooks
 * @ingroup lib_blob_db
 * @{
 *
 * @brief What an application supplies, or may observe, when blob_db
 *        authenticates or encrypts its store (CONFIG_BLOB_DB_PROTECT), or runs
 *        on a UBI volume.
 *
 * Integration hooks, outside the blob_db contract like
 * <app/lib/blob_db_ubi.h>. See doc/proposals/2026-10-15-blob-db-protection.md.
 */

#if defined(CONFIG_BLOB_DB_KEYS) || defined(__DOXYGEN__)
/**
 * @brief Hand blob_db the keying material every key derives from.
 *
 * Called by blob_db_mount() (and blob_db_format() on an unmountable store)
 * after psa_crypto_init(), once each time the store is opened. Defined by the
 * application when CONFIG_BLOB_DB_KEY_APP is set; the build fails to link
 * otherwise. blob_db derives its slot and metadata keys from it, and on the
 * UBI backend hands the same handle to UBI, which derives its own under
 * different labels.
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
int blob_db_ikm_key(psa_key_id_t *key_id);
#endif

/** What a check found. */
enum blob_db_event_kind {
	/** Damage: a CRC failed. Torn writes and bit rot look like this. */
	BLOB_DB_EVENT_CORRUPT,
	/** Tampering: a CRC verified and the MAC or tag over it did not, or a
	 *  header names another bucket or another store. */
	BLOB_DB_EVENT_TAMPERED,
};

/** Where it was found. */
enum blob_db_event_layer {
	/** UBI's own metadata or data seal; @c where is the PEB. */
	BLOB_DB_EVENT_UBI,
	/** A master sector; @c where is 0 or 1. */
	BLOB_DB_EVENT_MASTER,
	/** A bucket header or the compaction seal; @c where is the bucket. */
	BLOB_DB_EVENT_BUCKET,
	/** A slot; @c where is the bucket in the high 16 bits and the slot's
	 *  offset in the low 16 (truncated on sectors above 64 KiB). */
	BLOB_DB_EVENT_SLOT,
};

/**
 * @brief Told about every damaged or tampered object blob_db or UBI finds.
 *
 * blob_db has already acted on it -- a damaged slot falls back to the older
 * version, a tampered one reads -EIO, a bad master loses to the other -- so
 * this is for logging, counting and policy (wiping keys, refusing service),
 * not for repair. Runs on the caller's thread, inside the blob_db call that
 * found it, and for the UBI layer under UBI's device lock: it must not call
 * into blob_db or UBI.
 *
 * blob_db supplies a weak definition that logs. Define this function in the
 * application to replace it.
 */
void blob_db_event(enum blob_db_event_kind kind,
		   enum blob_db_event_layer layer, uint32_t where);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_BLOB_DB_SECURITY_H_ */
