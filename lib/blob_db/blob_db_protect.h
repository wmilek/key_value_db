/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db — keying material and the cryptographic primitives behind
 * CONFIG_BLOB_DB_PROTECT. Private to lib/blob_db.
 *
 * The keys (doc/proposals/2026-10-15-blob-db-protection.md §3.1):
 *
 *   IKM     the application's key handle, or the built-in development key
 *           (CONFIG_BLOB_DB_KEY); also what the UBI backend hands to UBI
 *   K_slot  HKDF-SHA256(IKM, "blob_db/v1", "blob_db/slot-mac/v1"  || uuid)
 *           AES-CMAC truncated to the tag length              (AUTH)
 *         | HKDF-SHA256(IKM, "blob_db/v1", "blob_db/slot-aead/v1" || uuid)
 *           AES-CCM with a shortened tag                      (AEAD)
 *   K_meta  HKDF-SHA256(IKM, "blob_db/v1", "blob_db/meta/v1"      || uuid)
 *           AES-CMAC, full 16 bytes
 *
 * The derivation is zephyr-ubi's (library/ubi_key.c): a fixed salt, the info
 * a label followed by a context, output into volatile AES-128 keys. The salt
 * and labels differ from UBI's, so the same IKM yields independent keys, and
 * the store uuid takes the place of UBI's constant context: a reformat starts
 * the nonce counter again, so it must also change the key.
 *
 * Nothing here knows the on-flash layout; blob_db.c decides what is covered.
 */

#ifndef LIB_BLOB_DB_PROTECT_H_
#define LIB_BLOB_DB_PROTECT_H_

#include <stddef.h>
#include <stdint.h>

#if defined(CONFIG_BLOB_DB_KEYS)
#include <psa/crypto_types.h>

/** psa_crypto_init() and acquire the IKM. Once per open of the store. */
int blob_db_keys_open(void);

/** Destroy every derived key and release the IKM. Safe to repeat. */
void blob_db_keys_close(void);

/** The IKM handle; valid between blob_db_keys_open() and _close(). */
psa_key_id_t blob_db_keys_ikm(void);
#endif /* CONFIG_BLOB_DB_KEYS */

#if defined(CONFIG_BLOB_DB_PROTECTED)

/** One piece of a message that is not contiguous in memory. */
struct blob_db_iov {
	const void *p;
	size_t n;
};

/** Derive K_slot and K_meta for the store @p uuid, replacing the keys of any
 *  other uuid. Nothing to do if they are already this uuid's. */
int blob_db_keys_derive(const uint8_t *uuid);

/** Fill @p buf from the PSA random generator. */
int blob_db_random(void *buf, size_t len);

/** AES-CMAC-128 under K_meta over the pieces of @p iov. */
int blob_db_meta_mac(const struct blob_db_iov *iov, size_t n, uint8_t *mac);

/** Check a K_meta MAC in constant time: 0, -EBADMSG if it does not verify,
 *  -EIO if the crypto backend failed. */
int blob_db_meta_verify(const struct blob_db_iov *iov, size_t n,
			const uint8_t *mac);

#if defined(CONFIG_BLOB_DB_PROTECT_AUTH)
/** A slot's truncated AES-CMAC under K_slot, BLOB_DB_TAG_LEN bytes. */
int blob_db_slot_mac(const struct blob_db_iov *iov, size_t n, uint8_t *tag);

/** 0, -EBADMSG if @p tag does not verify, -EIO on a backend failure. */
int blob_db_slot_mac_verify(const struct blob_db_iov *iov, size_t n,
			    const uint8_t *tag);
#endif

#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)
/** Encrypt @p len bytes of @p buf in place under K_slot and @p nonce, with
 *  the pieces of @p aad authenticated alongside, and write the tag
 *  (BLOB_DB_TAG_LEN bytes) to @p tag, which must not overlap @p buf. */
int blob_db_slot_seal(const uint8_t *nonce, const struct blob_db_iov *aad,
		      size_t n_aad, uint8_t *buf, size_t len, uint8_t *tag);

/** Decrypt and verify @p len bytes at @p in into @p out. @p out may equal
 *  @p in or lie before it (it never overtakes the input), and may be NULL to
 *  verify only. On -EBADMSG @p out holds garbage. @p tag follows the
 *  ciphertext and is not overwritten. */
int blob_db_slot_open(const uint8_t *nonce, const struct blob_db_iov *aad,
		      size_t n_aad, const uint8_t *in, uint8_t *out,
		      size_t len, const uint8_t *tag);
#endif

#endif /* CONFIG_BLOB_DB_PROTECTED */

#endif /* LIB_BLOB_DB_PROTECT_H_ */
