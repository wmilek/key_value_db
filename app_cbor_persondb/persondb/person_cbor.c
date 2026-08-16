/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * person_cbor — zcbor encode/decode for the three record types.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include "person_cbor.h"

/* Nesting never exceeds map -> list, so two backups is enough; a third costs
 * one state struct and removes the need to think about it again. */
#define NBACKUPS 3

/* Map keys. Never renumber: the store outlives the build that wrote it, and
 * the superblock carries a version for exactly the case where they must. */
enum {
	K_ID = 1, K_NAME, K_DEPT, K_TITLE, K_VALID_FROM,
	K_VALID_UNTIL, K_PIN, K_PERMS, K_CARDS,
};
#define PERSON_PAIRS 9

enum {
	SB_VERSION = 1, SB_PEOPLE_ROOTS, SB_CRED_ROOTS,
	SB_N_PERSONS, SB_POPULATED, SB_REV, SB_N_BUCKETS,
};
#define SB_PAIRS 7

/* Copy a decoded text slice into a fixed field, rejecting anything that would
 * not fit. A silent truncation here would surface as a verification failure
 * far away, so it is an error, not a clamp. */
static int copy_tstr(const struct zcbor_string *s, char *dst, size_t cap)
{
	if (s->len >= cap) {
		return -EILSEQ;
	}
	memcpy(dst, s->value, s->len);
	dst[s->len] = '\0';
	return 0;
}

int person_cbor_encode(const struct persondb_person *p, uint8_t *buf,
		       size_t buf_sz, size_t *out_len)
{
	if (p == NULL || buf == NULL) {
		return -EINVAL;
	}
	if (p->n_perms > PERSONDB_PERMS_MAX || p->n_cards > PERSONDB_CARDS_MAX) {
		return -EINVAL;
	}

	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, PERSON_PAIRS) &&
		  zcbor_uint32_put(zs, K_ID) && zcbor_uint32_put(zs, p->id) &&
		  zcbor_uint32_put(zs, K_NAME) &&
		  zcbor_tstr_put_term(zs, p->name, PERSONDB_NAME_MAX) &&
		  zcbor_uint32_put(zs, K_DEPT) &&
		  zcbor_tstr_put_term(zs, p->dept, PERSONDB_DEPT_MAX) &&
		  zcbor_uint32_put(zs, K_TITLE) &&
		  zcbor_tstr_put_term(zs, p->title, PERSONDB_TITLE_MAX) &&
		  zcbor_uint32_put(zs, K_VALID_FROM) &&
		  zcbor_uint32_put(zs, p->valid_from) &&
		  zcbor_uint32_put(zs, K_VALID_UNTIL) &&
		  zcbor_uint32_put(zs, p->valid_until) &&
		  zcbor_uint32_put(zs, K_PIN) &&
		  zcbor_bstr_encode_ptr(zs, (const char *)p->pin_hash,
					PERSONDB_PIN_LEN);

	ok = ok && zcbor_uint32_put(zs, K_PERMS) &&
	     zcbor_list_start_encode(zs, PERSONDB_PERMS_MAX);
	for (uint8_t i = 0; ok && i < p->n_perms; i++) {
		ok = zcbor_tstr_put_term(zs, p->perm[i], PERSONDB_PERM_MAX);
	}
	ok = ok && zcbor_list_end_encode(zs, PERSONDB_PERMS_MAX);

	ok = ok && zcbor_uint32_put(zs, K_CARDS) &&
	     zcbor_list_start_encode(zs, PERSONDB_CARDS_MAX);
	for (uint8_t i = 0; ok && i < p->n_cards; i++) {
		ok = zcbor_tstr_put_term(zs, p->card[i], PERSONDB_CARD_LEN);
	}
	ok = ok && zcbor_list_end_encode(zs, PERSONDB_CARDS_MAX);

	ok = ok && zcbor_map_end_encode(zs, PERSON_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	if (out_len) {
		*out_len = (size_t)(zs->payload - buf);
	}
	return 0;
}

int person_cbor_decode(const uint8_t *buf, size_t len,
		       struct persondb_person *out)
{
	if (buf == NULL || out == NULL) {
		return -EINVAL;
	}

	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	struct zcbor_string s;
	bool ok = zcbor_map_start_decode(zs) &&
		  zcbor_uint32_expect(zs, K_ID) &&
		  zcbor_uint32_decode(zs, &out->id) &&
		  zcbor_uint32_expect(zs, K_NAME) && zcbor_tstr_decode(zs, &s);

	if (ok && copy_tstr(&s, out->name, sizeof(out->name)) != 0) {
		return -EILSEQ;
	}

	ok = ok && zcbor_uint32_expect(zs, K_DEPT) && zcbor_tstr_decode(zs, &s);
	if (ok && copy_tstr(&s, out->dept, sizeof(out->dept)) != 0) {
		return -EILSEQ;
	}

	ok = ok && zcbor_uint32_expect(zs, K_TITLE) && zcbor_tstr_decode(zs, &s);
	if (ok && copy_tstr(&s, out->title, sizeof(out->title)) != 0) {
		return -EILSEQ;
	}

	ok = ok && zcbor_uint32_expect(zs, K_VALID_FROM) &&
	     zcbor_uint32_decode(zs, &out->valid_from) &&
	     zcbor_uint32_expect(zs, K_VALID_UNTIL) &&
	     zcbor_uint32_decode(zs, &out->valid_until) &&
	     zcbor_uint32_expect(zs, K_PIN) && zcbor_bstr_decode(zs, &s);
	if (ok && s.len != PERSONDB_PIN_LEN) {
		return -EILSEQ;
	}
	if (ok) {
		memcpy(out->pin_hash, s.value, PERSONDB_PIN_LEN);
	}

	ok = ok && zcbor_uint32_expect(zs, K_PERMS) && zcbor_list_start_decode(zs);
	while (ok && !zcbor_array_at_end(zs)) {
		if (out->n_perms >= PERSONDB_PERMS_MAX ||
		    !zcbor_tstr_decode(zs, &s) ||
		    copy_tstr(&s, out->perm[out->n_perms],
			      sizeof(out->perm[0])) != 0) {
			return -EILSEQ;
		}
		out->n_perms++;
	}
	ok = ok && zcbor_list_end_decode(zs);

	ok = ok && zcbor_uint32_expect(zs, K_CARDS) && zcbor_list_start_decode(zs);
	while (ok && !zcbor_array_at_end(zs)) {
		if (out->n_cards >= PERSONDB_CARDS_MAX ||
		    !zcbor_tstr_decode(zs, &s) ||
		    copy_tstr(&s, out->card[out->n_cards],
			      sizeof(out->card[0])) != 0) {
			return -EILSEQ;
		}
		out->n_cards++;
	}
	ok = ok && zcbor_list_end_decode(zs);

	ok = ok && zcbor_map_end_decode(zs);

	return ok ? 0 : -EILSEQ;
}

int cred_cbor_encode(uint32_t person_id, uint8_t *buf, size_t buf_sz,
		     size_t *out_len)
{
	ZCBOR_STATE_E(zs, 1, buf, buf_sz, 1);

	if (!zcbor_uint32_put(zs, person_id)) {
		return -ENOMEM;
	}
	if (out_len) {
		*out_len = (size_t)(zs->payload - buf);
	}
	return 0;
}

int cred_cbor_decode(const uint8_t *buf, size_t len, uint32_t *person_id)
{
	ZCBOR_STATE_D(zs, 1, buf, len, 1, 0);

	return zcbor_uint32_decode(zs, person_id) ? 0 : -EILSEQ;
}

int superblock_cbor_encode(const struct superblock *sb, uint8_t *buf,
			   size_t buf_sz, size_t *out_len)
{
	if (sb->n_people_maps > ARRAY_SIZE(sb->people_root) ||
	    sb->n_cred_maps == 0 ||
	    sb->n_cred_maps > ARRAY_SIZE(sb->cred_root)) {
		return -EINVAL;
	}

	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, SB_PAIRS) &&
		  zcbor_uint32_put(zs, SB_VERSION) &&
		  zcbor_uint32_put(zs, sb->version) &&
		  zcbor_uint32_put(zs, SB_PEOPLE_ROOTS) &&
		  zcbor_list_start_encode(zs, ARRAY_SIZE(sb->people_root));

	for (uint8_t i = 0; ok && i < sb->n_people_maps; i++) {
		ok = zcbor_uint64_put(zs, sb->people_root[i]);
	}

	ok = ok && zcbor_list_end_encode(zs, ARRAY_SIZE(sb->people_root)) &&
	     zcbor_uint32_put(zs, SB_CRED_ROOTS) &&
	     zcbor_list_start_encode(zs, ARRAY_SIZE(sb->cred_root));

	for (uint8_t i = 0; ok && i < sb->n_cred_maps; i++) {
		ok = zcbor_uint64_put(zs, sb->cred_root[i]);
	}

	ok = ok && zcbor_list_end_encode(zs, ARRAY_SIZE(sb->cred_root)) &&
	     zcbor_uint32_put(zs, SB_N_BUCKETS) &&
	     zcbor_uint32_put(zs, sb->n_buckets) &&
	     zcbor_uint32_put(zs, SB_N_PERSONS) &&
	     zcbor_uint32_put(zs, sb->n_persons) &&
	     zcbor_uint32_put(zs, SB_POPULATED) &&
	     zcbor_uint32_put(zs, sb->populated) &&
	     zcbor_uint32_put(zs, SB_REV) && zcbor_uint32_put(zs, sb->rev) &&
	     zcbor_map_end_encode(zs, SB_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	if (out_len) {
		*out_len = (size_t)(zs->payload - buf);
	}
	return 0;
}

int superblock_cbor_decode(const uint8_t *buf, size_t len,
			   struct superblock *out)
{
	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	bool ok = zcbor_map_start_decode(zs) &&
		  zcbor_uint32_expect(zs, SB_VERSION) &&
		  zcbor_uint32_decode(zs, &out->version) &&
		  zcbor_uint32_expect(zs, SB_PEOPLE_ROOTS) &&
		  zcbor_list_start_decode(zs);

	while (ok && !zcbor_array_at_end(zs)) {
		if (out->n_people_maps >= ARRAY_SIZE(out->people_root) ||
		    !zcbor_uint64_decode(zs,
					 &out->people_root[out->n_people_maps])) {
			return -EILSEQ;
		}
		out->n_people_maps++;
	}

	ok = ok && zcbor_list_end_decode(zs) &&
	     zcbor_uint32_expect(zs, SB_CRED_ROOTS) &&
	     zcbor_list_start_decode(zs);

	while (ok && !zcbor_array_at_end(zs)) {
		if (out->n_cred_maps >= ARRAY_SIZE(out->cred_root) ||
		    !zcbor_uint64_decode(zs,
					 &out->cred_root[out->n_cred_maps])) {
			return -EILSEQ;
		}
		out->n_cred_maps++;
	}

	uint32_t n_buckets = 0;

	ok = ok && zcbor_list_end_decode(zs) &&
	     zcbor_uint32_expect(zs, SB_N_BUCKETS) &&
	     zcbor_uint32_decode(zs, &n_buckets) &&
	     zcbor_uint32_expect(zs, SB_N_PERSONS) &&
	     zcbor_uint32_decode(zs, &out->n_persons) &&
	     zcbor_uint32_expect(zs, SB_POPULATED) &&
	     zcbor_uint32_decode(zs, &out->populated) &&
	     zcbor_uint32_expect(zs, SB_REV) &&
	     zcbor_uint32_decode(zs, &out->rev) && zcbor_map_end_decode(zs);

	out->n_buckets = (uint16_t)n_buckets;
	return ok ? 0 : -EILSEQ;
}
