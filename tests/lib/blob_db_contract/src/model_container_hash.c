/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Model container — executable form of doc/layers/l1_model_container.md.
 * Built on nothing but the blob_db L1 contract. See model_container_hash.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>

#include "model_container_hash.h"

LOG_MODULE_REGISTER(model_container_hash, LOG_LEVEL_INF);

#define MC_MAX_ENTRIES  15   /* (256 - 12) / 16, one payload */
#define MC_MAX_NEW       2   /* insert: key + value blob */
#define MC_MAX_DEL       4
#define MC_MAX_KEY      32
#define MC_MAX_VAL      64

/* The list root id, handed in by the client on mc_open(). How it is
 * persisted (root registry, direct id-1 binding) is the client's business. */
static uint64_t mc_root;

/* ---- RAM shapes (parsed from flash payloads) -------------------------- */

struct mc_list {
	uint64_t intent_id;
	uint32_t count;
	struct {
		uint64_t kid;
		uint64_t vid;
	} e[MC_MAX_ENTRIES];
};

/* Both counts 0 == no mutation in flight. */
struct mc_intent {
	uint16_t nnew;
	uint16_t ndel;
	uint64_t new_ids[MC_MAX_NEW];   /* allocated before STAGE; die on rollback */
	uint64_t del[MC_MAX_DEL];       /* die on commit */
};

static bool mc_ready;

/* A clean failure whose rollback could not finish either: the intent is
 * still staged. The next mutation must finish recovery before it stages,
 * or its STAGE would overwrite the only record of those ids. */
static bool mc_pending;

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
	int rc = blob_db_get(mc_root, buf, sizeof(buf), &got);

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

	return blob_db_update(mc_root, buf, len);
}

/* Intent payload: u16 nnew, u16 ndel, then nnew + ndel u64 ids. */
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
	if (got < 4) {
		return -EIO;
	}
	memcpy(&in->nnew, buf, 2);
	memcpy(&in->ndel, buf + 2, 2);
	if (in->nnew > MC_MAX_NEW || in->ndel > MC_MAX_DEL ||
	    got != 4 + 8 * ((size_t)in->nnew + in->ndel)) {
		return -EIO;
	}
	memcpy(in->new_ids, buf + 4, 8 * in->nnew);
	memcpy(in->del, buf + 4 + 8 * in->nnew, 8 * in->ndel);
	return 0;
}

static int intent_store(uint64_t intent_id, const struct mc_intent *in)
{
	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];

	memcpy(buf, &in->nnew, 2);
	memcpy(buf + 2, &in->ndel, 2);
	memcpy(buf + 4, in->new_ids, 8 * in->nnew);
	memcpy(buf + 4 + 8 * in->nnew, in->del, 8 * in->ndel);
	return blob_db_update(intent_id, buf, 4 + 8 * ((size_t)in->nnew + in->ndel));
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

static bool list_refs_any(const struct mc_list *l, const uint64_t *ids, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (list_refs_id(l, ids[i])) {
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

/* ---- recovery (l1_model_container.md §6) ------------------------------ */

/* Finish or undo the mutation the intent records. Reads one blob when
 * nothing is in flight; otherwise deletes at most the ids the intent names
 * and never looks at the rest of the store. Idempotent: a crash in here
 * re-enters the same branch, deletes tolerate -ENOENT, CLEAR is last. */
static int mc_recover(const struct mc_list *l)
{
	struct mc_intent in;
	int rc = intent_load(l->intent_id, &in);

	if (rc < 0) {
		return rc;
	}
	if (in.nnew == 0 && in.ndel == 0) {
		return 0;   /* empty intent — nothing in flight */
	}

	/* A mutation that created blobs committed iff the list links one of
	 * them; a pure delete committed iff the list no longer links del[]. */
	bool committed = in.nnew > 0 ? list_refs_any(l, in.new_ids, in.nnew)
				     : !list_refs_any(l, in.del, in.ndel);
	const uint64_t *victims = committed ? in.del : in.new_ids;
	uint16_t n = committed ? in.ndel : in.nnew;

	LOG_INF("recover: interrupted mutation nnew=%u ndel=%u -> roll %s",
		in.nnew, in.ndel, committed ? "forward" : "back");

	for (uint16_t i = 0; i < n; i++) {
		/* -ENOENT: already deleted, or allocated and never written. */
		rc = blob_db_delete(victims[i]);
		if (rc < 0 && rc != -ENOENT) {
			return rc;
		}
	}

	return intent_clear(l->intent_id);   /* CLEAR — last, idempotent */
}

/* Abort path for a mutation that failed without a power cut: run the same
 * recovery against what is actually on flash, then report the original
 * error. If recovery fails too, remember it so the next mutation finishes
 * it before staging its own intent. */
static int mc_abort(int err)
{
	struct mc_list l;

	mc_pending = list_load(&l) < 0 || mc_recover(&l) < 0;
	return err;
}

/* Run before a mutation stages: finish an earlier failed abort. */
static int mc_settle(const struct mc_list *l)
{
	if (!mc_pending) {
		return 0;
	}

	int rc = mc_recover(l);

	if (rc == 0) {
		mc_pending = false;
	}
	return rc;
}

/* ---- create (virgin store, basic flow §3.1 + registry §7) ------------- */

static int mc_create(void)
{
	/* mc_root is allocated but not yet bound (the client's durable record
	 * of it — e.g. a registry entry — is the creation intent). Allocate
	 * and bind the intent blob, then bind the initial list image; the
	 * final update is the commit that makes the container exist. */
	uint64_t intent_id = blob_db_alloc_id();
	struct mc_intent empty = { 0 };
	int rc = intent_store(intent_id, &empty);   /* bind intent blob */

	if (rc < 0) {
		return rc;
	}

	struct mc_list l = { .intent_id = intent_id, .count = 0 };

	return list_store(&l);   /* COMMIT — container exists at mc_root */
}

/* ---- public API ------------------------------------------------------- */

int mc_open(uint64_t root_id)
{
	if (root_id == 0) {
		return -EINVAL;
	}
	mc_root = root_id;
	mc_pending = false;

	uint8_t probe[4];
	size_t got;
	int rc = blob_db_get(mc_root, probe, sizeof(probe), &got);

	/* Allocated-but-unbound root (the client's record exists, the blob
	 * doesn't — registry §7 "creation in progress"): finish it now. */
	if (rc == -ENOENT) {
		rc = mc_create();
		if (rc < 0) {
			return rc;
		}
		mc_ready = true;
		return 0;
	}
	/* -ENOMEM just means the list is bigger than the probe buffer —
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

/* Stop at the injected crash point, leaving the intent staged as a power
 * cut would. Not an abort: the caller remounts and mc_open() recovers. */
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
	rc = mc_settle(&l);
	if (rc < 0) {
		return rc;
	}

	int idx = list_find(&l, key, klen);

	/* Refuse before anything is written, so a full list costs nothing. */
	if (idx < 0 && l.count >= MC_MAX_ENTRIES) {
		return -ENOSPC;
	}

	/* step 0: allocate every id this mutation will create (RAM only). */
	struct mc_intent in = { 0 };
	uint64_t vid2 = blob_db_alloc_id();
	uint64_t kid = idx < 0 ? blob_db_alloc_id() : 0;

	if (vid2 == 0 || (idx < 0 && kid == 0)) {
		return -EIO;
	}
	in.new_ids[in.nnew++] = vid2;
	if (idx < 0) {
		in.new_ids[in.nnew++] = kid;
	} else {
		/* overwrite: the old value blob dies on commit (§8.2). */
		in.del[in.ndel++] = l.e[idx].vid;
	}

	/* step 1: STAGE — name the new ids and the doomed ones. */
	rc = intent_store(l.intent_id, &in);
	if (rc < 0) {
		return rc;   /* atomic: the intent is still empty */
	}
	STOP_IF(MC_STEP_STAGE);

	/* step 2: PREPARE — bind the new blobs, unreferenced so far. */
	rc = blob_db_update(vid2, val, vlen);
	if (rc < 0) {
		return mc_abort(rc);
	}
	if (idx < 0) {
		rc = blob_db_update(kid, key, klen);
		if (rc < 0) {
			return mc_abort(rc);
		}
	}
	STOP_IF(MC_STEP_PREPARE);

	/* step 3: COMMIT — swap the list image (linearization point). */
	if (idx >= 0) {
		l.e[idx].vid = vid2;
	} else {
		l.e[l.count].kid = kid;
		l.e[l.count].vid = vid2;
		l.count++;
	}
	rc = list_store(&l);
	if (rc < 0) {
		return mc_abort(rc);
	}
	STOP_IF(MC_STEP_COMMIT);

	/* step 4: CLEANUP — delete superseded blobs. */
	for (uint16_t i = 0; i < in.ndel; i++) {
		rc = blob_db_delete(in.del[i]);
		if (rc < 0 && rc != -ENOENT) {
			return mc_abort(rc);
		}
	}
	STOP_IF(MC_STEP_CLEANUP);

	/* step 5: CLEAR. */
	rc = intent_clear(l.intent_id);
	return rc < 0 ? mc_abort(rc) : 0;
}

int mc_destroy(void)
{
	if (!mc_ready) {
		return -ENODEV;
	}

	struct mc_list l;
	int rc = list_load(&l);

	if (rc < 0) {
		return rc;
	}

	/* Delete leaves first, the root last, so a crash anywhere leaves a
	 * re-runnable state: the root still loads, dangling entry refs read
	 * as absent, and every delete below tolerates -ENOENT on rerun. */
	for (uint32_t i = 0; i < l.count; i++) {
		rc = blob_db_delete(l.e[i].kid);
		if (rc < 0 && rc != -ENOENT) {
			return rc;
		}
		rc = blob_db_delete(l.e[i].vid);
		if (rc < 0 && rc != -ENOENT) {
			return rc;
		}
	}
	rc = blob_db_delete(l.intent_id);
	if (rc < 0 && rc != -ENOENT) {
		return rc;
	}
	rc = blob_db_delete(mc_root);
	if (rc < 0 && rc != -ENOENT) {
		return rc;
	}

	mc_ready = false;
	mc_root = 0;
	return 0;
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
	rc = mc_settle(&l);
	if (rc < 0) {
		return rc;
	}

	int idx = list_find(&l, key, strlen(key));

	if (idx < 0) {
		return -ENOENT;
	}

	/* step 0: a delete creates nothing — no ids to allocate. */
	struct mc_intent in = {
		.ndel = 2,
		.del = { l.e[idx].kid, l.e[idx].vid },
	};

	/* step 1: STAGE. */
	rc = intent_store(l.intent_id, &in);
	if (rc < 0) {
		return rc;   /* atomic: the intent is still empty */
	}
	STOP_IF(MC_STEP_STAGE);

	/* (delete creates nothing — no PREPARE.) */
	STOP_IF(MC_STEP_PREPARE);

	/* step 2: COMMIT — unlink the entry. */
	l.e[idx] = l.e[l.count - 1];
	l.count--;
	rc = list_store(&l);
	if (rc < 0) {
		return mc_abort(rc);
	}
	STOP_IF(MC_STEP_COMMIT);

	/* step 3/4: CLEANUP — delete the now-unlinked blobs. */
	for (uint16_t i = 0; i < in.ndel; i++) {
		rc = blob_db_delete(in.del[i]);
		if (rc < 0 && rc != -ENOENT) {
			return mc_abort(rc);
		}
	}
	STOP_IF(MC_STEP_CLEANUP);

	/* step 5: CLEAR. */
	rc = intent_clear(l.intent_id);
	return rc < 0 ? mc_abort(rc) : 0;
}
