/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Model container — executable form of doc/layers/l1_model_container.md.
 * Built on nothing but the blob_db L1 contract. See model_container.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>

#include "model_container.h"

LOG_MODULE_REGISTER(model_container, LOG_LEVEL_INF);

#define MC_MAX_ENTRIES  15   /* (256 - 12) / 16, one payload */
#define MC_MAX_DEL       4
#define MC_MAX_KEY      32
#define MC_MAX_VAL      64

/* ---- RAM shapes (parsed from flash payloads) -------------------------- */

struct mc_list {
	uint64_t intent_id;
	uint32_t count;
	struct {
		uint64_t kid;
		uint64_t vid;
	} e[MC_MAX_ENTRIES];
};

struct mc_intent {
	uint64_t w;                 /* watermark; 0 == no mutation in flight */
	uint32_t ndel;
	uint64_t del[MC_MAX_DEL];
};

static bool mc_ready;

/* ---- (de)serialization ------------------------------------------------ */

static size_t list_serialize(const struct mc_list *l, uint8_t *buf)
{
	size_t off = 0;

	memcpy(buf + off, &l->intent_id, sizeof(l->intent_id));
	off += sizeof(l->intent_id);
	memcpy(buf + off, &l->count, sizeof(l->count));
	off += sizeof(l->count);
	for (uint32_t i = 0; i < l->count; i++) {
		memcpy(buf + off, &l->e[i].kid, 8);
		off += 8;
		memcpy(buf + off, &l->e[i].vid, 8);
		off += 8;
	}
	return off;
}

static int list_load(struct mc_list *l)
{
	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t got;
	int rc = blob_db_get(MC_ROOT_ID, buf, sizeof(buf), &got);

	if (rc < 0) {
		return rc;
	}
	memset(l, 0, sizeof(*l));
	size_t off = 0;

	memcpy(&l->intent_id, buf + off, 8);
	off += 8;
	memcpy(&l->count, buf + off, 4);
	off += 4;
	if (l->count > MC_MAX_ENTRIES) {
		return -EIO;
	}
	for (uint32_t i = 0; i < l->count; i++) {
		memcpy(&l->e[i].kid, buf + off, 8);
		off += 8;
		memcpy(&l->e[i].vid, buf + off, 8);
		off += 8;
	}
	return 0;
}

static int list_store(const struct mc_list *l)
{
	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t len = list_serialize(l, buf);

	return blob_db_update(MC_ROOT_ID, buf, len);
}

static int intent_load(uint64_t intent_id, struct mc_intent *in)
{
	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t got;
	int rc = blob_db_get(intent_id, buf, sizeof(buf), &got);

	if (rc < 0) {
		return rc;
	}
	memset(in, 0, sizeof(*in));
	if (got == 0) {
		return 0;   /* freshly allocated, treated as empty */
	}
	size_t off = 0;

	memcpy(&in->w, buf + off, 8);
	off += 8;
	memcpy(&in->ndel, buf + off, 4);
	off += 4;
	if (in->ndel > MC_MAX_DEL) {
		return -EIO;
	}
	for (uint32_t i = 0; i < in->ndel; i++) {
		memcpy(&in->del[i], buf + off, 8);
		off += 8;
	}
	return 0;
}

static int intent_store(uint64_t intent_id, const struct mc_intent *in)
{
	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t off = 0;

	memcpy(buf + off, &in->w, 8);
	off += 8;
	memcpy(buf + off, &in->ndel, 4);
	off += 4;
	for (uint32_t i = 0; i < in->ndel; i++) {
		memcpy(buf + off, &in->del[i], 8);
		off += 8;
	}
	return blob_db_update(intent_id, buf, off);
}

static int intent_clear(uint64_t intent_id)
{
	struct mc_intent empty = { 0 };

	return intent_store(intent_id, &empty);
}

/* ---- helpers ---------------------------------------------------------- */

static bool list_refs_id(const struct mc_list *l, uint64_t id)
{
	for (uint32_t i = 0; i < l->count; i++) {
		if (l->e[i].kid == id || l->e[i].vid == id) {
			return true;
		}
	}
	return false;
}

static bool list_refs_any_ge(const struct mc_list *l, uint64_t w)
{
	for (uint32_t i = 0; i < l->count; i++) {
		if (l->e[i].kid >= w || l->e[i].vid >= w) {
			return true;
		}
	}
	return false;
}

/* Find the entry index for `key`, or -1. */
static int list_find(const struct mc_list *l, const char *key, size_t klen)
{
	for (uint32_t i = 0; i < l->count; i++) {
		uint8_t kb[MC_MAX_KEY];
		size_t got;

		if (blob_db_get(l->e[i].kid, kb, sizeof(kb), &got) < 0) {
			continue;   /* dangling ref — treated as absent (§7) */
		}
		if (got == klen && memcmp(kb, key, klen) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/* ---- rollback scan (collect-then-delete; iterate is read-only, D5) ----- */

struct rollback_ctx {
	const struct mc_list *l;
	uint64_t intent_id;
	uint64_t w;
	uint64_t victims[MC_MAX_ENTRIES * 2 + MC_MAX_DEL];
	size_t   nvictims;
};

static int rollback_collect(uint64_t id, const void *p, size_t len, void *user)
{
	struct rollback_ctx *c = user;

	ARG_UNUSED(p);
	ARG_UNUSED(len);
	if (id < c->w) {
		return 0;   /* below watermark — not this mutation's */
	}
	if (id == MC_ROOT_ID || id == c->intent_id) {
		return 0;   /* structural */
	}
	if (list_refs_id(c->l, id)) {
		return 0;   /* linked — keep */
	}
	if (c->nvictims < ARRAY_SIZE(c->victims)) {
		c->victims[c->nvictims++] = id;
	}
	return 0;
}

/* ---- recovery (l1_model_container.md §6) ------------------------------ */

static int mc_recover(struct mc_list *l)
{
	struct mc_intent in;
	int rc = intent_load(l->intent_id, &in);

	if (rc < 0) {
		return rc;
	}
	if (in.w == 0) {
		return 0;   /* empty intent — nothing in flight */
	}

	LOG_INF("recover: interrupted mutation W=%llu ndel=%u",
		(unsigned long long)in.w, in.ndel);

	bool committed = list_refs_any_ge(l, in.w);

	if (!committed && in.ndel > 0) {
		/* Pure-delete case: committed iff the list no longer
		 * references any del[] id. */
		bool any_del_referenced = false;

		for (uint32_t i = 0; i < in.ndel; i++) {
			if (list_refs_id(l, in.del[i])) {
				any_del_referenced = true;
				break;
			}
		}
		committed = !any_del_referenced;
	}

	if (committed) {
		/* ROLL FORWARD: delete every id in del[] (tolerate -ENOENT). */
		for (uint32_t i = 0; i < in.ndel; i++) {
			rc = blob_db_delete(in.del[i]);
			if (rc < 0 && rc != -ENOENT) {
				return rc;
			}
		}
	} else {
		/* ROLL BACK: delete bound ids >= W the list does not link. */
		struct rollback_ctx c = {
			.l = l, .intent_id = l->intent_id, .w = in.w,
		};

		rc = blob_db_iterate(rollback_collect, &c);
		if (rc < 0) {
			return rc;
		}
		for (size_t i = 0; i < c.nvictims; i++) {
			rc = blob_db_delete(c.victims[i]);
			if (rc < 0 && rc != -ENOENT) {
				return rc;
			}
		}
	}

	return intent_clear(l->intent_id);   /* CLEAR — last, idempotent */
}

/* ---- create (virgin store, basic flow §3.1) --------------------------- */

static int mc_create(void)
{
	/* Root (id = 1) is pre-bound with an empty payload by blob_db_format()
	 * — see @blob_db_root in blob_db.h. Allocate the intent blob, then
	 * rebind the root with the initial list image. */
	uint64_t intent_id = blob_db_alloc_id();
	struct mc_intent empty = { 0 };
	int rc = intent_store(intent_id, &empty);   /* bind intent blob */

	if (rc < 0) {
		return rc;
	}

	struct mc_list l = { .intent_id = intent_id, .count = 0 };

	return list_store(&l);   /* COMMIT — container exists at id = 1 */
}

/* ---- public API ------------------------------------------------------- */

int mc_open(void)
{
	uint8_t probe[4];
	size_t got;
	int rc = blob_db_get(MC_ROOT_ID, probe, sizeof(probe), &got);

	/* Virgin detection: root always exists after format, but with an
	 * empty payload — that is the "no container yet" signal. */
	if (rc == 0 && got == 0) {
		rc = mc_create();
		if (rc < 0) {
			return rc;
		}
		mc_ready = true;
		return 0;
	}
	/* -ENOMEM just means the root is bigger than the probe buffer —
	 * that's fine, it exists. Any other negative is a real error. */
	if (rc < 0 && rc != -ENOMEM) {
		return rc;
	}

	struct mc_list l;

	rc = list_load(&l);
	if (rc < 0) {
		return rc;
	}
	rc = mc_recover(&l);
	if (rc < 0) {
		return rc;
	}
	mc_ready = true;
	return 0;
}

int mc_get(const char *key, void *out, size_t out_sz, size_t *out_len)
{
	if (!mc_ready) {
		return -ENODEV;
	}

	struct mc_list l;
	int rc = list_load(&l);

	if (rc < 0) {
		return rc;
	}

	int idx = list_find(&l, key, strlen(key));

	if (idx < 0) {
		return -ENOENT;
	}
	return blob_db_get(l.e[idx].vid, out, out_sz, out_len);
}

int mc_entry_count(size_t *out)
{
	struct mc_list l;
	int rc = list_load(&l);

	if (rc < 0) {
		return rc;
	}
	*out = l.count;
	return 0;
}

/* STAGE helper: build+store the intent, honoring the crash point. */
#define STOP_IF(cp)  do { if (crash_at == (cp)) { return MC_STOPPED; } } while (0)

int mc_set(const char *key, const void *val, size_t vlen,
	   enum mc_crash_point crash_at)
{
	if (!mc_ready) {
		return -ENODEV;
	}
	size_t klen = strlen(key);

	if (klen == 0 || klen > MC_MAX_KEY || vlen > MC_MAX_VAL) {
		return -EINVAL;
	}

	struct mc_list l;
	int rc = list_load(&l);

	if (rc < 0) {
		return rc;
	}

	int idx = list_find(&l, key, klen);

	/* step 0: watermark (RAM only). */
	uint64_t w = blob_db_alloc_id();

	struct mc_intent in = { .w = w, .ndel = 0 };

	if (idx >= 0) {
		/* overwrite: the old value blob dies on commit (§8.2). */
		in.ndel = 1;
		in.del[0] = l.e[idx].vid;
	}

	/* step 1: STAGE. */
	rc = intent_store(l.intent_id, &in);
	if (rc < 0) {
		return rc;
	}
	STOP_IF(MC_STEP_STAGE);

	/* step 2: PREPARE — bind the new blobs (ids > W, unreferenced). */
	uint64_t vid2 = blob_db_alloc_id();

	rc = blob_db_update(vid2, val, vlen);
	if (rc < 0) {
		return rc;
	}

	uint64_t kid = 0;

	if (idx < 0) {
		kid = blob_db_alloc_id();
		rc = blob_db_update(kid, key, klen);
		if (rc < 0) {
			return rc;
		}
	}
	STOP_IF(MC_STEP_PREPARE);

	/* step 3: COMMIT — swap the list image (linearization point). */
	if (idx >= 0) {
		l.e[idx].vid = vid2;
	} else {
		if (l.count >= MC_MAX_ENTRIES) {
			return -ENOSPC;
		}
		l.e[l.count].kid = kid;
		l.e[l.count].vid = vid2;
		l.count++;
	}
	rc = list_store(&l);
	if (rc < 0) {
		return rc;
	}
	STOP_IF(MC_STEP_COMMIT);

	/* step 4: CLEANUP — delete superseded blobs. */
	for (uint32_t i = 0; i < in.ndel; i++) {
		rc = blob_db_delete(in.del[i]);
		if (rc < 0 && rc != -ENOENT) {
			return rc;
		}
	}
	STOP_IF(MC_STEP_CLEANUP);

	/* step 5: CLEAR. */
	return intent_clear(l.intent_id);
}

int mc_del(const char *key, enum mc_crash_point crash_at)
{
	if (!mc_ready) {
		return -ENODEV;
	}

	struct mc_list l;
	int rc = list_load(&l);

	if (rc < 0) {
		return rc;
	}

	int idx = list_find(&l, key, strlen(key));

	if (idx < 0) {
		return -ENOENT;
	}

	/* step 0: watermark. */
	uint64_t w = blob_db_alloc_id();
	struct mc_intent in = {
		.w = w, .ndel = 2,
		.del = { l.e[idx].kid, l.e[idx].vid },
	};

	/* step 1: STAGE. */
	rc = intent_store(l.intent_id, &in);
	if (rc < 0) {
		return rc;
	}
	STOP_IF(MC_STEP_STAGE);

	/* (delete creates nothing — no PREPARE.) */
	STOP_IF(MC_STEP_PREPARE);

	/* step 2: COMMIT — unlink the entry. */
	l.e[idx] = l.e[l.count - 1];
	l.count--;
	rc = list_store(&l);
	if (rc < 0) {
		return rc;
	}
	STOP_IF(MC_STEP_COMMIT);

	/* step 3/4: CLEANUP — delete the now-unlinked blobs. */
	rc = blob_db_delete(in.del[0]);
	if (rc < 0 && rc != -ENOENT) {
		return rc;
	}
	rc = blob_db_delete(in.del[1]);
	if (rc < 0 && rc != -ENOENT) {
		return rc;
	}
	STOP_IF(MC_STEP_CLEANUP);

	/* step 5: CLEAR. */
	return intent_clear(l.intent_id);
}
