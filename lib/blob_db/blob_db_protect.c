/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db — keying material and the primitives of CONFIG_BLOB_DB_PROTECT.
 * See blob_db_protect.h for the key schedule.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <psa/crypto.h>

#include <app/lib/blob_db_security.h>
#include <app/lib/blob_db_test.h>
#if defined(CONFIG_BLOB_DB_BACKEND_UBI)
#include <app/lib/blob_db_ubi.h>
#endif

#include "blob_db_internal.h"
#include "blob_db_protect.h"

LOG_MODULE_DECLARE(blob_db, CONFIG_BLOB_DB_LOG_LEVEL);

/* Keying material --------------------------------------------------------- */

static psa_key_id_t g_ikm = PSA_KEY_ID_NULL;

#if defined(CONFIG_BLOB_DB_KEY_BUILTIN)
/* A fixed, published key: the MACs then catch damage, not tampering, and AEAD
 * only obfuscates. The bytes are the ones the UBI backend imported before
 * keying moved here, so UBI devices written by earlier builds still attach.
 * Imported as a volatile key at every open and destroyed at close. */
static int ikm_acquire(psa_key_id_t *key_id)
{
	static const uint8_t ikm[32] = "blob_db UBI dev key, not secret";
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attr, PSA_KEY_TYPE_DERIVE);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);

	const psa_status_t status = psa_import_key(&attr, ikm, sizeof(ikm), key_id);

	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_import_key: %d", (int)status);
		return -EIO;
	}
	return 0;
}

static void ikm_release(psa_key_id_t key_id)
{
	(void)psa_destroy_key(key_id);
}
#else
#if defined(CONFIG_BLOB_DB_BACKEND_UBI)
/* An application written against the UBI-only hook keeps working for one
 * release; defining blob_db_ikm_key() replaces this. */
__weak int blob_db_ikm_key(psa_key_id_t *key_id)
{
	return blob_db_ubi_ikm_key(key_id);
}
#endif

static int ikm_acquire(psa_key_id_t *key_id)
{
	return blob_db_ikm_key(key_id);
}

static void ikm_release(psa_key_id_t key_id)
{
	/* The application's key: it outlives the store. */
	ARG_UNUSED(key_id);
}
#endif

#if defined(CONFIG_BLOB_DB_PROTECTED)
static void keys_forget(void);
#endif

int blob_db_keys_open(void)
{
	if (g_ikm != PSA_KEY_ID_NULL) {
		return 0;
	}

	const psa_status_t status = psa_crypto_init();

	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init: %d", (int)status);
		return -EIO;
	}

	psa_key_id_t id = PSA_KEY_ID_NULL;
	const int rc = ikm_acquire(&id);

	if (rc != 0) {
		LOG_ERR("no keying material: %d", rc);
		return rc;
	}
	g_ikm = id;
	return 0;
}

void blob_db_keys_close(void)
{
#if defined(CONFIG_BLOB_DB_PROTECTED)
	keys_forget();
#endif
	if (g_ikm != PSA_KEY_ID_NULL) {
		ikm_release(g_ikm);
		g_ikm = PSA_KEY_ID_NULL;
	}
}

psa_key_id_t blob_db_keys_ikm(void)
{
	return g_ikm;
}

#if defined(CONFIG_BLOB_DB_PROTECTED)

/* Derived keys ------------------------------------------------------------ */

static const uint8_t derivation_salt[] = "blob_db/v1";
#if defined(CONFIG_BLOB_DB_PROTECT_AUTH)
static const uint8_t label_slot[] = "blob_db/slot-mac/v1";
#else
static const uint8_t label_slot[] = "blob_db/slot-aead/v1";
#endif
static const uint8_t label_meta[] = "blob_db/meta/v1";

#define TAG_LEN  BLOB_DB_TAG_LEN

#if defined(CONFIG_BLOB_DB_PROTECT_AUTH)
#if TAG_LEN == 16
#define SLOT_ALG  PSA_ALG_CMAC
#else
#define SLOT_ALG  PSA_ALG_TRUNCATED_MAC(PSA_ALG_CMAC, TAG_LEN)
#endif
#else
#define SLOT_ALG  PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, TAG_LEN)
#endif

static psa_key_id_t g_k_slot = PSA_KEY_ID_NULL;
static psa_key_id_t g_k_meta = PSA_KEY_ID_NULL;
static uint8_t g_uuid[BLOB_DB_UUID_LEN];

static void keys_forget(void)
{
	if (g_k_slot != PSA_KEY_ID_NULL) {
		(void)psa_destroy_key(g_k_slot);
		g_k_slot = PSA_KEY_ID_NULL;
	}
	if (g_k_meta != PSA_KEY_ID_NULL) {
		(void)psa_destroy_key(g_k_meta);
		g_k_meta = PSA_KEY_ID_NULL;
	}
}

/* One HKDF-SHA256 derivation into a volatile AES-128 key, as ubi_key.c's
 * key_derive_one(): salt first, then the secret, then info = label || uuid. */
static int derive_one(const uint8_t *label, size_t label_len,
		      const uint8_t *uuid, psa_algorithm_t alg,
		      psa_key_usage_t usage, psa_key_id_t *key_id)
{
	uint8_t info[sizeof(label_slot) + BLOB_DB_UUID_LEN];
	psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_status_t status;

	BUILD_ASSERT(sizeof(label_meta) <= sizeof(label_slot),
		     "the HKDF info is sized for the longer label");

	memcpy(info, label, label_len);
	memcpy(&info[label_len], uuid, BLOB_DB_UUID_LEN);

	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 128);
	psa_set_key_algorithm(&attr, alg);
	psa_set_key_usage_flags(&attr, usage);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);

	status = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	if (status == PSA_SUCCESS) {
		status = psa_key_derivation_input_bytes(
			&op, PSA_KEY_DERIVATION_INPUT_SALT, derivation_salt,
			sizeof(derivation_salt) - 1);
	}
	if (status == PSA_SUCCESS) {
		status = psa_key_derivation_input_key(
			&op, PSA_KEY_DERIVATION_INPUT_SECRET, g_ikm);
	}
	if (status == PSA_SUCCESS) {
		status = psa_key_derivation_input_bytes(
			&op, PSA_KEY_DERIVATION_INPUT_INFO, info,
			label_len + BLOB_DB_UUID_LEN);
	}
	if (status == PSA_SUCCESS) {
		status = psa_key_derivation_output_key(&attr, &op, key_id);
	}

	psa_key_derivation_abort(&op);
	psa_reset_key_attributes(&attr);

	if (status != PSA_SUCCESS) {
		LOG_ERR("key derivation: %d", (int)status);
		return (status == PSA_ERROR_NOT_PERMITTED ||
			status == PSA_ERROR_INVALID_HANDLE) ? -EACCES : -EIO;
	}
	return 0;
}

int blob_db_keys_derive(const uint8_t *uuid)
{
	if (g_k_slot != PSA_KEY_ID_NULL &&
	    memcmp(g_uuid, uuid, BLOB_DB_UUID_LEN) == 0) {
		return 0;
	}
	keys_forget();

#if defined(CONFIG_BLOB_DB_PROTECT_AUTH)
	const psa_key_usage_t slot_usage =
		PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE;
#else
	const psa_key_usage_t slot_usage =
		PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT;
#endif
	int rc = derive_one(label_slot, sizeof(label_slot) - 1, uuid, SLOT_ALG,
			    slot_usage, &g_k_slot);

	if (rc == 0) {
		rc = derive_one(label_meta, sizeof(label_meta) - 1, uuid,
				PSA_ALG_CMAC,
				PSA_KEY_USAGE_SIGN_MESSAGE |
					PSA_KEY_USAGE_VERIFY_MESSAGE,
				&g_k_meta);
	}
	if (rc != 0) {
		keys_forget();
		return rc;
	}
	memcpy(g_uuid, uuid, BLOB_DB_UUID_LEN);
	return 0;
}

int blob_db_random(void *buf, size_t len)
{
	const psa_status_t status = psa_generate_random(buf, len);

	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_generate_random: %d", (int)status);
		return -EIO;
	}
	return 0;
}

/* MACs -------------------------------------------------------------------- */

static int mac_run(psa_key_id_t key, psa_algorithm_t alg, bool verify,
		   const struct blob_db_iov *iov, size_t n, uint8_t *mac,
		   size_t mac_len)
{
	psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
	psa_status_t status = verify ? psa_mac_verify_setup(&op, key, alg)
				     : psa_mac_sign_setup(&op, key, alg);

	for (size_t i = 0; i < n && status == PSA_SUCCESS; i++) {
		if (iov[i].n > 0) {
			status = psa_mac_update(&op, iov[i].p, iov[i].n);
		}
	}
	if (status == PSA_SUCCESS) {
		size_t out_len = 0;

		status = verify ? psa_mac_verify_finish(&op, mac, mac_len)
				: psa_mac_sign_finish(&op, mac, mac_len,
						      &out_len);
		if (status == PSA_SUCCESS && !verify && out_len != mac_len) {
			status = PSA_ERROR_GENERIC_ERROR;
		}
	}
	if (status == PSA_SUCCESS) {
		return 0;
	}
	(void)psa_mac_abort(&op);
	if (status == PSA_ERROR_INVALID_SIGNATURE) {
		return -EBADMSG;
	}
	LOG_ERR("psa mac: %d", (int)status);
	return -EIO;
}

int blob_db_meta_mac(const struct blob_db_iov *iov, size_t n, uint8_t *mac)
{
	return mac_run(g_k_meta, PSA_ALG_CMAC, false, iov, n, mac,
		       BLOB_DB_META_MAC_LEN);
}

int blob_db_meta_verify(const struct blob_db_iov *iov, size_t n,
			const uint8_t *mac)
{
	return mac_run(g_k_meta, PSA_ALG_CMAC, true, iov, n, (uint8_t *)mac,
		       BLOB_DB_META_MAC_LEN);
}

#if defined(CONFIG_BLOB_DB_PROTECT_AUTH)
int blob_db_slot_mac(const struct blob_db_iov *iov, size_t n, uint8_t *tag)
{
	return mac_run(g_k_slot, SLOT_ALG, false, iov, n, tag, TAG_LEN);
}

int blob_db_slot_mac_verify(const struct blob_db_iov *iov, size_t n,
			    const uint8_t *tag)
{
	return mac_run(g_k_slot, SLOT_ALG, true, iov, n, (uint8_t *)tag,
		       TAG_LEN);
}
#endif /* CONFIG_BLOB_DB_PROTECT_AUTH */

#if defined(CONFIG_BLOB_DB_PROTECT_AEAD)

/* AES-CCM ----------------------------------------------------------------- *
 *
 * Multipart, through a small staging chunk on the stack. Every input chunk is
 * copied out before the call that produces its output, and output never runs
 * ahead of the input consumed, so the output may overwrite the input in place
 * or trail it -- which is how a slot is sealed where it was assembled and
 * opened into the buffer it was read into, with no second sector-sized
 * buffer. Anything a backend holds back is drained into a chunk with a block
 * of headroom.
 */

#define CCM_CHUNK  64

static int aead_begin(psa_aead_operation_t *op, bool encrypt,
		      const uint8_t *nonce, const struct blob_db_iov *aad,
		      size_t n_aad, size_t len)
{
	size_t aad_len = 0;

	for (size_t i = 0; i < n_aad; i++) {
		aad_len += aad[i].n;
	}

	psa_status_t status = encrypt
		? psa_aead_encrypt_setup(op, g_k_slot, SLOT_ALG)
		: psa_aead_decrypt_setup(op, g_k_slot, SLOT_ALG);

	if (status == PSA_SUCCESS) {
		status = psa_aead_set_lengths(op, aad_len, len);
	}
	if (status == PSA_SUCCESS) {
		status = psa_aead_set_nonce(op, nonce, BLOB_DB_NONCE_LEN);
	}
	for (size_t i = 0; i < n_aad && status == PSA_SUCCESS; i++) {
		if (aad[i].n > 0) {
			status = psa_aead_update_ad(op, aad[i].p, aad[i].n);
		}
	}
	return status;
}

/* Run @p len bytes from @p in through @p op, writing to @p out (or nowhere). */
static psa_status_t aead_stream(psa_aead_operation_t *op, const uint8_t *in,
				uint8_t *out, size_t len, size_t *out_pos)
{
	uint8_t tin[CCM_CHUNK];
	uint8_t tout[CCM_CHUNK + 16];

	for (size_t pos = 0; pos < len;) {
		const size_t c = MIN(sizeof(tin), len - pos);
		size_t o = 0;

		memcpy(tin, in + pos, c);
		psa_status_t status =
			psa_aead_update(op, tin, c, tout, sizeof(tout), &o);

		if (status != PSA_SUCCESS) {
			return status;
		}
		if (out != NULL && o > 0) {
			memcpy(out + *out_pos, tout, o);
		}
		*out_pos += o;
		pos += c;
	}
	return PSA_SUCCESS;
}

int blob_db_slot_seal(const uint8_t *nonce, const struct blob_db_iov *aad,
		      size_t n_aad, uint8_t *buf, size_t len, uint8_t *tag)
{
	psa_aead_operation_t op = PSA_AEAD_OPERATION_INIT;
	size_t out_pos = 0;
	psa_status_t status = aead_begin(&op, true, nonce, aad, n_aad, len);

	if (status == PSA_SUCCESS) {
		status = aead_stream(&op, buf, buf, len, &out_pos);
	}
	if (status == PSA_SUCCESS) {
		uint8_t tout[16];
		size_t o = 0, tag_len = 0;

		status = psa_aead_finish(&op, tout, sizeof(tout), &o, tag,
					 TAG_LEN, &tag_len);
		if (status == PSA_SUCCESS) {
			memcpy(buf + out_pos, tout, o);
			out_pos += o;
			if (out_pos != len || tag_len != TAG_LEN) {
				status = PSA_ERROR_GENERIC_ERROR;
			}
		}
	}
	if (status != PSA_SUCCESS) {
		(void)psa_aead_abort(&op);
		LOG_ERR("psa aead seal: %d", (int)status);
		return -EIO;
	}
#if defined(CONFIG_BLOB_DB_TEST_PROTECT_HOOKS)
	blob_db_test_nonce_used(nonce);
#endif
	return 0;
}

int blob_db_slot_open(const uint8_t *nonce, const struct blob_db_iov *aad,
		      size_t n_aad, const uint8_t *in, uint8_t *out,
		      size_t len, const uint8_t *tag)
{
	psa_aead_operation_t op = PSA_AEAD_OPERATION_INIT;
	size_t out_pos = 0;
	psa_status_t status = aead_begin(&op, false, nonce, aad, n_aad, len);

	if (status == PSA_SUCCESS) {
		status = aead_stream(&op, in, out, len, &out_pos);
	}
	if (status == PSA_SUCCESS) {
		uint8_t tout[16];
		size_t o = 0;

		status = psa_aead_verify(&op, tout, sizeof(tout), &o, tag,
					 TAG_LEN);
		if (status == PSA_SUCCESS && out != NULL && o > 0) {
			memcpy(out + out_pos, tout, o);
		}
	}
	if (status == PSA_SUCCESS) {
		return 0;
	}
	(void)psa_aead_abort(&op);
	if (status == PSA_ERROR_INVALID_SIGNATURE) {
		return -EBADMSG;
	}
	LOG_ERR("psa aead open: %d", (int)status);
	return -EIO;
}
#endif /* CONFIG_BLOB_DB_PROTECT_AEAD */

#endif /* CONFIG_BLOB_DB_PROTECTED */
