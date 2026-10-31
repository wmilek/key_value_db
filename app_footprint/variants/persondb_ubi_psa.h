/*
 * TF-PSA-Crypto settings for zephyr-ubi, included after Zephyr's own
 * config-tf-psa-crypto.h (CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE, set in
 * persondb_ubi_minimal.conf). UBI uses AES only inside AES-CMAC, which
 * only encrypts, and only with 128-bit keys (UBI_KEY_BITS in ubi_key.h).
 */
#define MBEDTLS_BLOCK_CIPHER_NO_DECRYPT
#define MBEDTLS_AES_ONLY_128_BIT_KEY_LENGTH
