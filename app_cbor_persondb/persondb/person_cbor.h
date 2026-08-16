/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * person_cbor — the wire format (R-B), and nothing else.
 *
 * This module knows how a person becomes bytes. It does not know where those
 * bytes go, so the schema can change without touching storage and the storage
 * layout can change without touching the schema.
 *
 * Encoding is canonical (CONFIG_ZCBOR_CANONICAL), which makes a record's bytes
 * a pure function of its content — so verification can compare encodings
 * directly instead of decoding and walking two structs.
 *
 * The schema is a CBOR map with small integer keys rather than text keys: at
 * ~350 B per record and 10 000 records, text keys would add roughly 600 KB to
 * a 4 MiB dataset for no benefit inside a closed application.
 *
 *   { 1: id, 2: name, 3: dept, 4: title, 5: valid_from,
 *     6: valid_until, 7: pin, 8: [permissions], 9: [cards] }
 */

#ifndef APP_CBOR_PERSONDB_PERSON_CBOR_H_
#define APP_CBOR_PERSONDB_PERSON_CBOR_H_

#include <stddef.h>
#include <stdint.h>

#include "persondb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Worst case for a fully-populated record: 9 map pairs, 22 permissions of 23
 * characters, 5 cards. Sized so a caller can put one on the stack, because
 * neither map_ops.get nor blob_db_get can report a value's length without
 * paying for the whole read (FINDINGS.md V2). */
#define PERSON_CBOR_MAX 768

/** A credential index entry is just the owner's id. */
#define CRED_CBOR_MAX 16

/** The application superblock: version, map roots, dataset geometry.
 *
 * Sized for the widest shape the Kconfig ranges allow: 64 person roots and 16
 * credential roots, each a CBOR uint64 (9 B), plus the scalars. It was 256 B
 * when a build could only have 16 roots and one credential map. */
#define SUPERBLOCK_CBOR_MAX 896

/**
 * @brief Encode @p p into @p buf.
 *
 * @retval 0        encoded; *out_len set
 * @retval -ENOMEM  buf too small
 * @retval -EINVAL  the person violates the schema's bounds
 */
int person_cbor_encode(const struct persondb_person *p, uint8_t *buf,
		       size_t buf_sz, size_t *out_len);

/**
 * @brief Decode @p len bytes into @p out.
 *
 * Strings are copied into the caller's struct rather than returned as slices
 * into @p buf. Slices would save the copy, but they would put a lifetime rule
 * into the API this application is meant to demonstrate, and the copy is
 * ~350 B against a decision that costs four 64 KB flash reads (B1) — it is not
 * where the time goes, and the `cbor` benchmark exists to prove that.
 *
 * @retval 0        decoded
 * @retval -EILSEQ  not a well-formed record of this schema
 */
int person_cbor_decode(const uint8_t *buf, size_t len,
		       struct persondb_person *out);

/** Encode/decode a credential index entry (the owner's person id). */
int cred_cbor_encode(uint32_t person_id, uint8_t *buf, size_t buf_sz,
		     size_t *out_len);
int cred_cbor_decode(const uint8_t *buf, size_t len, uint32_t *person_id);

/** The superblock's decoded form. */
struct superblock {
	uint32_t version;
	uint8_t  n_people_maps;
	uint64_t people_root[64];      /* matches the Kconfig range, which the
					* 16-element version did not (§8.1) */
	uint8_t  n_cred_maps;
	uint64_t cred_root[16];
	uint16_t n_buckets;            /* buckets per map, 0 = "as many as fit" */
	uint32_t n_persons;
	uint32_t populated;
	uint32_t rev;
};

int superblock_cbor_encode(const struct superblock *sb, uint8_t *buf,
			   size_t buf_sz, size_t *out_len);
int superblock_cbor_decode(const uint8_t *buf, size_t len,
			   struct superblock *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_PERSON_CBOR_H_ */
