/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Model container list — executable form of
 * doc/layers/l1_model_container_list.md. Built on nothing but the blob_db
 * L1 contract. See model_container_list.h.
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>

#include "model_container_list.h"

LOG_MODULE_REGISTER(model_container_list, LOG_LEVEL_INF);

#define MCL_MAX_NEW     2    /* insert: node + value blob */
#define MCL_MAX_DEL     2    /* remove: node + value blob */
#define MCL_MAX_VAL    64
#define MCL_MAX_NODES 4096   /* mcl_check gives up past this: a cycle */

/* The root id, handed in by the client on mcl_open(). */
static uint64_t mcl_root;
static bool mcl_ready;

/* Test hook (mcl_fail_after). */
static enum mcl_crash_point mcl_fail_step;
static int mcl_fail_rc;

/* ---- RAM shapes (parsed from flash payloads) -------------------------- */

/* Root: the intent id and both ends. `head` starts the authoritative
 * forward chain; `tail` is derived, like every node's `prev`. */
struct mcl_root {
	uint64_t intent_id;
	uint64_t head;
	uint64_t tail;
};

/* Node: its links and its value blob. 0 is "no node" (past either end). */
struct mcl_node {
	uint64_t prev;   /* derived: mirrors the forward chain */
	uint64_t next;   /* authoritative */
	uint64_t vid;
};

/* Both counts 0 == no mutation in flight. Besides the ids the mutation
 * creates and retires, the intent names its two link writes, so recovery
 * can tell whether the commit landed and redo the repair if it did:
 *
 *   COMMIT  fwd(anchor) := link    (anchor 0 = root.head)
 *   REPAIR  back(succ)  := back    (succ 0   = root.tail)
 */
struct mcl_intent {
	uint16_t nnew;
	uint16_t ndel;
	uint64_t anchor;
	uint64_t link;
	uint64_t succ;
	uint64_t back;
	uint64_t new_ids[MCL_MAX_NEW];   /* allocated before STAGE; die on rollback */
	uint64_t del[MCL_MAX_DEL];       /* die on commit */
};

#define INTENT_HDR  (4 + 4 * 8)   /* counts + anchor, link, succ, back */

/* ---- (de)serialization ------------------------------------------------ */

static int root_load(struct mcl_root *r)
{
	size_t got;
	int rc = blob_db_get(mcl_root, r, sizeof(*r), &got);

	if (rc < 0) {
		return rc;
	}
	return got == sizeof(*r) ? 0 : -EIO;
}

static int root_store(const struct mcl_root *r)
{
	return blob_db_update(mcl_root, r, sizeof(*r));
}

/* A blob that is not shaped like a node is not a node: -ENOENT, the same
 * answer as for an id that was never bound. */
static int node_load(uint64_t id, struct mcl_node *n)
{
	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t got;
	int rc;

	if (id == 0 || id == mcl_root) {
		return -ENOENT;
	}
	rc = blob_db_get(id, buf, sizeof(buf), &got);
	if (rc < 0) {
		return rc;
	}
	if (got != sizeof(*n)) {
		return -ENOENT;
	}
	memcpy(n, buf, sizeof(*n));
	return 0;
}

static int node_store(uint64_t id, const struct mcl_node *n)
{
	return blob_db_update(id, n, sizeof(*n));
}

/* Intent payload: u16 nnew, u16 ndel, u64 anchor, link, succ, back, then
 * nnew + ndel u64 ids. */
static int intent_load(uint64_t intent_id, struct mcl_intent *in)
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
	if (got < INTENT_HDR) {
		return -EIO;
	}
	memcpy(&in->nnew, buf, 2);
	memcpy(&in->ndel, buf + 2, 2);
	if (in->nnew > MCL_MAX_NEW || in->ndel > MCL_MAX_DEL ||
	    got != INTENT_HDR + 8 * ((size_t)in->nnew + in->ndel)) {
		return -EIO;
	}
	memcpy(&in->anchor, buf + 4, 8);
	memcpy(&in->link, buf + 12, 8);
	memcpy(&in->succ, buf + 20, 8);
	memcpy(&in->back, buf + 28, 8);
	memcpy(in->new_ids, buf + INTENT_HDR, 8 * in->nnew);
	memcpy(in->del, buf + INTENT_HDR + 8 * in->nnew, 8 * in->ndel);
	return 0;
}

static int intent_store(uint64_t intent_id, const struct mcl_intent *in)
{
	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];

	memcpy(buf, &in->nnew, 2);
	memcpy(buf + 2, &in->ndel, 2);
	memcpy(buf + 4, &in->anchor, 8);
	memcpy(buf + 12, &in->link, 8);
	memcpy(buf + 20, &in->succ, 8);
	memcpy(buf + 28, &in->back, 8);
	memcpy(buf + INTENT_HDR, in->new_ids, 8 * in->nnew);
	memcpy(buf + INTENT_HDR + 8 * in->nnew, in->del, 8 * in->ndel);
	return blob_db_update(intent_id, buf,
			      INTENT_HDR + 8 * ((size_t)in->nnew + in->ndel));
}

static int intent_clear(uint64_t intent_id)
{
	struct mcl_intent empty = { 0 };

	return intent_store(intent_id, &empty);
}

/* ---- links ------------------------------------------------------------ */

/* The forward link out of `id`: root.head for 0, else the node's `next`.
 * Every write to it is a commit. */
static int fwd_get(uint64_t id, uint64_t *out)
{
	struct mcl_root r;
	struct mcl_node n;
	int rc = id == 0 ? root_load(&r) : node_load(id, &n);

	if (rc == 0) {
		*out = id == 0 ? r.head : n.next;
	}
	return rc;
}

static int fwd_set(uint64_t id, uint64_t to)
{
	struct mcl_root r;
	struct mcl_node n;
	int rc;

	if (id == 0) {
		rc = root_load(&r);
		r.head = to;
		return rc < 0 ? rc : root_store(&r);
	}
	rc = node_load(id, &n);
	n.next = to;
	return rc < 0 ? rc : node_store(id, &n);
}

/* The back link into `id`: root.tail for 0, else the node's `prev`. Every
 * write to it is a repair. */
static int back_get(uint64_t id, uint64_t *out)
{
	struct mcl_root r;
	struct mcl_node n;
	int rc = id == 0 ? root_load(&r) : node_load(id, &n);

	if (rc == 0) {
		*out = id == 0 ? r.tail : n.prev;
	}
	return rc;
}

static int back_set(uint64_t id, uint64_t to)
{
	struct mcl_root r;
	struct mcl_node n;
	int rc;

	if (id == 0) {
		rc = root_load(&r);
		r.tail = to;
		return rc < 0 ? rc : root_store(&r);
	}
	rc = node_load(id, &n);
	n.prev = to;
	return rc < 0 ? rc : node_store(id, &n);
}

/* Load node `id` and check it is linked into this list: its predecessor's
 * forward link names it. The double link makes membership O(1). It relies
 * on the back link, so callers run recovery first. */
static int node_linked(uint64_t id, struct mcl_node *n)
{
	uint64_t f;
	int rc = node_load(id, n);

	if (rc == 0) {
		rc = fwd_get(n->prev, &f);
	}
	if (rc == -ENOENT || (rc == 0 && f != id)) {
		return -ENOENT;
	}
	return rc;
}

/* ---- recovery (l1_model_container_list.md §5) ------------------------- */

/* Finish or undo the mutation the intent records. Reads the root and the
 * intent when nothing is in flight; otherwise touches only the anchor, the
 * successor and the ids the intent names. Idempotent: a crash in here
 * re-enters the same branch (the commit state cannot change), the repair
 * writes the same value again, deletes tolerate -ENOENT, CLEAR is last. */
static int mcl_recover(bool *committed_out)
{
	struct mcl_root r;
	struct mcl_intent in;
	uint64_t cur;
	int rc = root_load(&r);

	if (committed_out != NULL) {
		*committed_out = false;
	}
	if (rc == 0) {
		rc = intent_load(r.intent_id, &in);
	}
	if (rc < 0) {
		return rc;
	}
	if (in.nnew == 0 && in.ndel == 0) {
		return 0;   /* empty intent — nothing in flight */
	}

	/* Committed iff the forward link already says what the commit
	 * writes. Before the commit it names something else: the old
	 * successor (insert) or the node being removed (remove). */
	rc = fwd_get(in.anchor, &cur);
	if (rc < 0) {
		return rc;
	}

	bool committed = cur == in.link;
	const uint64_t *victims = committed ? in.del : in.new_ids;
	uint16_t n = committed ? in.ndel : in.nnew;

	LOG_INF("recover: interrupted mutation nnew=%u ndel=%u -> roll %s",
		in.nnew, in.ndel, committed ? "forward" : "back");
	if (committed_out != NULL) {
		*committed_out = committed;
	}

	/* Roll forward redoes the repair; before the commit no back link
	 * was touched, so roll back has nothing to undo but new[]. */
	if (committed) {
		rc = back_set(in.succ, in.back);
		if (rc < 0) {
			return rc;
		}
	}

	for (uint16_t i = 0; i < n; i++) {
		/* -ENOENT: already deleted, or allocated and never written. */
		rc = blob_db_delete(victims[i]);
		if (rc < 0 && rc != -ENOENT) {
			return rc;
		}
	}

	return intent_clear(r.intent_id);   /* CLEAR — last, idempotent */
}

/* Abort path for a mutation that failed without a power cut: run the same
 * recovery a remount would. Committed: finished, report success. Not
 * committed: rolled back, report the error. If recovery fails too, the
 * intent stays staged for the next operation to finish. */
static int mcl_abort(int err)
{
	bool committed;

	if (mcl_recover(&committed) < 0) {
		return err;
	}
	return committed ? 0 : err;
}

/* ---- create (virgin store) -------------------------------------------- */

static int mcl_create(void)
{
	uint64_t intent_id = blob_db_alloc_id();
	int rc = intent_clear(intent_id);   /* bind intent blob */

	if (rc < 0) {
		return rc;
	}

	struct mcl_root r = { .intent_id = intent_id };

	return root_store(&r);   /* COMMIT — the empty list exists */
}

/* ---- public API ------------------------------------------------------- */

int mcl_open(uint64_t root_id)
{
	if (root_id == 0) {
		return -EINVAL;
	}
	mcl_root = root_id;
	mcl_ready = false;

	struct mcl_root r;
	int rc = root_load(&r);

	/* Allocated-but-unbound root: finish creating it. */
	if (rc == -ENOENT) {
		rc = mcl_create();
	} else if (rc == 0) {
		rc = mcl_recover(NULL);
	}
	if (rc < 0) {
		return rc;
	}
	mcl_ready = true;
	return 0;
}

void mcl_fail_after(enum mcl_crash_point step, int err)
{
	mcl_fail_step = step;
	mcl_fail_rc = err;
}

/* Every operation that leans on a back link starts here: finish whatever
 * an earlier failed abort left staged, so the back chain mirrors the
 * forward one again and a STAGE never overwrites another mutation's ids.
 * One root and one intent read when nothing is in flight. */
static int mcl_begin(void)
{
	return mcl_ready ? mcl_recover(NULL) : -ENODEV;
}

/* End of a step. At the injected crash point, stop and leave the intent
 * staged as a power cut would. At the injected failure point, take the
 * abort path as a clean blob_db error would. */
#define CHECKPOINT(cp) do {						\
	if (crash_at == (cp)) {						\
		return MCL_STOPPED;					\
	}								\
	if (mcl_fail_step == (cp)) {					\
		mcl_fail_step = MCL_STEP_NONE;				\
		return mcl_abort(mcl_fail_rc);				\
	}								\
} while (0)

/* Link a new node between `p` and `s` (0 = the root's end), which must be
 * adjacent: fwd(p) == s and back(s) == p. */
static int mcl_insert(uint64_t p, uint64_t s, const void *val, size_t vlen,
		      uint64_t *out_id, enum mcl_crash_point crash_at)
{
	/* step 0: allocate every id this mutation will create (RAM only). */
	uint64_t nid = blob_db_alloc_id();
	uint64_t vid = blob_db_alloc_id();
	int rc;

	if (nid == 0 || vid == 0) {
		return -EIO;
	}
	/* Set now: an abort after the commit also returns 0. */
	if (out_id != NULL) {
		*out_id = nid;
	}

	struct mcl_intent in = {
		.nnew = 2,
		.new_ids = { nid, vid },
		.anchor = p, .link = nid,   /* COMMIT: fwd(p) := nid */
		.succ = s, .back = nid,     /* REPAIR: back(s) := nid */
	};
	struct mcl_root r;

	rc = root_load(&r);
	if (rc < 0) {
		return rc;
	}

	/* step 1: STAGE — name the new ids and both link writes. */
	rc = intent_store(r.intent_id, &in);
	if (rc < 0) {
		return rc;   /* atomic: the intent is still empty */
	}
	CHECKPOINT(MCL_STEP_STAGE);

	/* step 2: PREPARE — bind the value, then the node already pointing
	 * at both neighbours. Nothing links to it yet. */
	struct mcl_node n = { .prev = p, .next = s, .vid = vid };

	rc = blob_db_update(vid, val, vlen);
	if (rc == 0) {
		rc = node_store(nid, &n);
	}
	if (rc < 0) {
		return mcl_abort(rc);
	}
	CHECKPOINT(MCL_STEP_PREPARE);

	/* step 3: COMMIT — the forward chain now runs p -> nid -> s. */
	rc = fwd_set(p, nid);
	if (rc < 0) {
		return mcl_abort(rc);
	}
	CHECKPOINT(MCL_STEP_COMMIT);

	/* step 4: REPAIR — the back chain catches up: s <- nid. */
	rc = back_set(s, nid);
	if (rc < 0) {
		return mcl_abort(rc);
	}
	CHECKPOINT(MCL_STEP_REPAIR);

	/* step 5: CLEANUP — an insert retires nothing. */
	CHECKPOINT(MCL_STEP_CLEANUP);

	/* step 6: CLEAR. */
	rc = intent_clear(r.intent_id);
	return rc < 0 ? mcl_abort(rc) : 0;
}

static int check_val(size_t vlen)
{
	return vlen > MCL_MAX_VAL ? -EINVAL : 0;
}

int mcl_insert_after(uint64_t at, const void *val, size_t vlen,
		     uint64_t *out_id, enum mcl_crash_point crash_at)
{
	struct mcl_node n = { 0 };
	uint64_t s = 0;
	int rc = check_val(vlen);

	if (rc == 0) {
		rc = mcl_begin();
	}
	if (rc == 0) {
		rc = at == 0 ? fwd_get(0, &s) : node_linked(at, &n);
	}
	if (rc < 0) {
		return rc;
	}
	return mcl_insert(at, at == 0 ? s : n.next, val, vlen, out_id,
			  crash_at);
}

int mcl_insert_before(uint64_t at, const void *val, size_t vlen,
		      uint64_t *out_id, enum mcl_crash_point crash_at)
{
	struct mcl_node n = { 0 };
	uint64_t p = 0;
	int rc = check_val(vlen);

	if (rc == 0) {
		rc = mcl_begin();
	}
	if (rc == 0) {
		rc = at == 0 ? back_get(0, &p) : node_linked(at, &n);
	}
	if (rc < 0) {
		return rc;
	}
	return mcl_insert(at == 0 ? p : n.prev, at, val, vlen, out_id,
			  crash_at);
}

int mcl_remove(uint64_t id, enum mcl_crash_point crash_at)
{
	struct mcl_node n = { 0 };
	struct mcl_root r = { 0 };
	int rc = mcl_begin();

	if (rc == 0) {
		rc = node_linked(id, &n);
	}
	if (rc == 0) {
		rc = root_load(&r);
	}
	if (rc < 0) {
		return rc;
	}

	/* step 0: a remove creates nothing — no ids to allocate. */
	struct mcl_intent in = {
		.ndel = 2,
		.del = { id, n.vid },
		.anchor = n.prev, .link = n.next,   /* COMMIT: fwd(p) := s */
		.succ = n.next, .back = n.prev,     /* REPAIR: back(s) := p */
	};

	/* step 1: STAGE. */
	rc = intent_store(r.intent_id, &in);
	if (rc < 0) {
		return rc;   /* atomic: the intent is still empty */
	}
	CHECKPOINT(MCL_STEP_STAGE);

	/* (a remove binds nothing — no PREPARE.) */
	CHECKPOINT(MCL_STEP_PREPARE);

	/* step 3: COMMIT — the forward chain skips the node. */
	rc = fwd_set(n.prev, n.next);
	if (rc < 0) {
		return mcl_abort(rc);
	}
	CHECKPOINT(MCL_STEP_COMMIT);

	/* step 4: REPAIR — so does the back chain. */
	rc = back_set(n.next, n.prev);
	if (rc < 0) {
		return mcl_abort(rc);
	}
	CHECKPOINT(MCL_STEP_REPAIR);

	/* step 5: CLEANUP — delete the unlinked node and its value. */
	for (uint16_t i = 0; i < in.ndel; i++) {
		rc = blob_db_delete(in.del[i]);
		if (rc < 0 && rc != -ENOENT) {
			return mcl_abort(rc);
		}
	}
	CHECKPOINT(MCL_STEP_CLEANUP);

	/* step 6: CLEAR. */
	rc = intent_clear(r.intent_id);
	return rc < 0 ? mcl_abort(rc) : 0;
}

int mcl_set(uint64_t id, const void *val, size_t vlen)
{
	struct mcl_node n = { 0 };
	int rc = check_val(vlen);

	if (rc == 0) {
		rc = mcl_begin();
	}
	if (rc == 0) {
		rc = node_linked(id, &n);
	}
	/* The whole mutation: the reference graph does not change. */
	return rc < 0 ? rc : blob_db_update(n.vid, val, vlen);
}

int mcl_get(uint64_t id, void *out, size_t out_sz, size_t *out_len)
{
	struct mcl_node n = { 0 };
	int rc = mcl_ready ? node_load(id, &n) : -ENODEV;

	return rc < 0 ? rc : blob_db_get(n.vid, out, out_sz, out_len);
}

int mcl_next(uint64_t id, uint64_t *out)
{
	/* The authoritative chain: right at every instant, even with a
	 * mutation staged, so no recovery and no write. */
	return mcl_ready ? fwd_get(id, out) : -ENODEV;
}

int mcl_prev(uint64_t id, uint64_t *out)
{
	int rc = mcl_begin();

	return rc < 0 ? rc : back_get(id, out);
}

/* The last node still linked. Normally root.tail. After an interrupted
 * destroy, which deletes from the tail backwards, the tail is gone and what
 * is left is a prefix of the forward chain: walk to its end. */
static int last_linked(const struct mcl_root *r, uint64_t *out)
{
	struct mcl_node n;
	uint64_t x = 0;
	uint64_t next = r->head;
	int rc = r->tail == 0 ? -ENOENT : node_load(r->tail, &n);

	if (rc != -ENOENT) {
		*out = r->tail;
		return rc;
	}
	while (next != 0) {
		rc = node_load(next, &n);
		if (rc == -ENOENT) {
			break;
		}
		if (rc < 0) {
			return rc;
		}
		x = next;
		next = n.next;
	}
	*out = x;
	return 0;
}

int mcl_destroy(void)
{
	struct mcl_root r;
	struct mcl_node n;
	uint64_t x = 0;
	int rc;

	if (!mcl_ready) {
		return -ENODEV;
	}

	/* Finish a staged mutation first: its new[] blobs are not linked, so
	 * the walk below would never reach them. -ENOENT: the intent is
	 * already gone (a rerun of an interrupted destroy). */
	rc = mcl_recover(NULL);
	if (rc < 0 && rc != -ENOENT) {
		return rc;
	}
	rc = root_load(&r);
	if (rc == 0) {
		rc = last_linked(&r, &x);
	}
	if (rc < 0) {
		return rc;
	}

	/* Tail first, following the back links, the root last: a crash
	 * anywhere leaves a forward prefix a rerun finds again. */
	while (x != 0) {
		rc = node_load(x, &n);
		if (rc < 0) {
			return rc;
		}
		rc = blob_db_delete(n.vid);
		if (rc < 0 && rc != -ENOENT) {
			return rc;
		}
		rc = blob_db_delete(x);
		if (rc < 0) {
			return rc;
		}
		x = n.prev;
	}
	rc = blob_db_delete(r.intent_id);
	if (rc < 0 && rc != -ENOENT) {
		return rc;
	}
	rc = blob_db_delete(mcl_root);
	if (rc < 0 && rc != -ENOENT) {
		return rc;
	}

	mcl_ready = false;
	mcl_root = 0;
	return 0;
}

int mcl_check(size_t *count)
{
	struct mcl_root r;
	struct mcl_node n;
	uint64_t before = 0;
	size_t c = 0;
	int rc = mcl_ready ? root_load(&r) : -ENODEV;

	if (rc < 0) {
		return rc;
	}
	for (uint64_t x = r.head; x != 0; x = n.next) {
		if (++c > MCL_MAX_NODES) {
			return -ELOOP;
		}
		rc = node_load(x, &n);
		if (rc < 0) {
			return rc;
		}
		if (n.prev != before) {
			LOG_ERR("node %llu: prev %llu, want %llu",
				(unsigned long long)x,
				(unsigned long long)n.prev,
				(unsigned long long)before);
			return -EIO;
		}
		before = x;
	}
	if (r.tail != before) {
		LOG_ERR("tail %llu, want %llu", (unsigned long long)r.tail,
			(unsigned long long)before);
		return -EIO;
	}
	*count = c;
	return 0;
}
