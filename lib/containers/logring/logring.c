/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * logring — bounded per-entry log container (L2).
 *
 * A forward-linked chain of per-entry i-nodes with pre-reserved ids: each
 * entry's next_id points at an id alloc_id() returned but not yet bound, so an
 * append binds that id (one new blob, nothing rewritten). A RAM handle caches
 * the tail; the root i-node is written only at checkpoints (~every K appends)
 * and carries a waypoint near the end to rebuild the tail at open. Eviction is
 * batched onto the checkpoint write, its intent held inline in the root and its
 * run deleted suffix-first for re-enterable recovery. Reads use an opaque
 * cursor exporting a public {epoch, id} moniker; the per-log random epoch
 * rejects a stale/foreign moniker.
 *
 * Design of record: doc/proposals/2026-10-08-logring.md.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/toolchain.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/logring.h>

LOG_MODULE_REGISTER(logring, CONFIG_BLOB_CONTAINER_LOGRING_LOG_LEVEL);

/* On-flash format v1 — frozen. */
static const uint8_t CLOG_MAGIC[4] = { 'C', 'L', 'O', 'G' };
#define CLOG_VERSION 1

/* Root i-node payload: pointers + hints + the inline eviction intent. */
struct __packed clog_root {
	uint8_t  magic[4];
	uint8_t  version;
	uint8_t  flags;
	uint16_t rsvd;
	uint32_t epoch;           /* per-log incarnation tag (non-zero) */
	uint32_t capacity_bytes;  /* soft retention bound */
	uint32_t live_bytes;      /* hint: sum of live record lengths */
	uint32_t count;           /* hint: live entry count */
	uint64_t waypoint_id;     /* live entry near the end; open walks from here */
	uint64_t head_id;         /* oldest live entry (0 = empty) */
	uint64_t evict_from;      /* inline eviction intent: run start (0 = none) */
	uint64_t evict_to;        /* run end, exclusive */
};
BUILD_ASSERT(sizeof(struct clog_root) == 56, "clog_root layout drift");
BUILD_ASSERT(sizeof(struct clog_root) <= CONFIG_BLOB_DB_MAX_PAYLOAD_LEN,
	     "clog_root exceeds BLOB_DB_MAX_PAYLOAD_LEN");

/* Entry i-node payload: a forward link + the opaque record. */
struct __packed clog_entry_hdr {
	uint64_t next_id;         /* reserved id of the next entry (unbound = end) */
	uint16_t len;             /* record length; len bytes follow */
};
#define CLOG_ENTRY_HDR 10u
BUILD_ASSERT(sizeof(struct clog_entry_hdr) == CLOG_ENTRY_HDR, "clog_entry_hdr drift");

#define CLOG_MAX_RECORD (CONFIG_BLOB_DB_MAX_PAYLOAD_LEN - CLOG_ENTRY_HDR)

/* Checkpoint interval K: appends between root writes, and the most entries a
 * single checkpoint evicts (so a full interval's worth can be reclaimed at
 * once). The eviction run buffer is sized to it. */
#define CLOG_K CONFIG_BLOB_CONTAINER_LOGRING_CHECKPOINT
BUILD_ASSERT(CLOG_K >= 1 && CLOG_K <= 64, "checkpoint interval must be 1..64");

/* ---- entry I/O --------------------------------------------------------- */

/* Read an entry: its next_id (always), and optionally its payload into out.
 * Returns -EMSGSIZE if out_sz < len (nothing copied), -ENOENT if unbound/dead. */
static int entry_read(uint64_t id, uint64_t *next_id,
		      void *out, size_t out_sz, size_t *out_len)
{
	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t got;
	int rc = blob_db_get(id, buf, sizeof(buf), &got);

	if (rc < 0) {
		return rc;
	}
	if (got < CLOG_ENTRY_HDR) {
		return -EIO;
	}

	struct clog_entry_hdr h;

	memcpy(&h, buf, CLOG_ENTRY_HDR);
	if ((size_t)CLOG_ENTRY_HDR + h.len != got) {
		return -EIO;
	}
	if (next_id) {
		*next_id = h.next_id;
	}
	if (out) {
		if (h.len > out_sz) {
			return -EMSGSIZE;
		}
		memcpy(out, buf + CLOG_ENTRY_HDR, h.len);
	}
	if (out_len) {
		*out_len = h.len;
	}
	return 0;
}

static int entry_write(uint64_t id, uint64_t next_id,
		       const void *rec, size_t len)
{
	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	struct clog_entry_hdr h = { .next_id = next_id, .len = (uint16_t)len };

	memcpy(buf, &h, CLOG_ENTRY_HDR);
	if (len) {
		memcpy(buf + CLOG_ENTRY_HDR, rec, len);
	}
	return blob_db_update(id, buf, CLOG_ENTRY_HDR + len);
}

/* ---- root I/O ---------------------------------------------------------- */

static int root_load(uint64_t root, struct clog_root *r)
{
	uint8_t buf[sizeof(struct clog_root)];
	size_t got;
	int rc = blob_db_get(root, buf, sizeof(buf), &got);

	if (rc < 0) {
		return rc;
	}
	if (got < sizeof(*r)) {
		return -ENOTSUP;   /* too small / foreign payload */
	}
	memcpy(r, buf, sizeof(*r));
	if (memcmp(r->magic, CLOG_MAGIC, 4) != 0 || r->version != CLOG_VERSION) {
		return -ENOTSUP;
	}
	return 0;
}

/* Serialize the handle's state into the root and commit it. */
static int root_write(logring_t *h, uint64_t evict_from, uint64_t evict_to)
{
	struct clog_root r = { 0 };

	memcpy(r.magic, CLOG_MAGIC, 4);
	r.version        = CLOG_VERSION;
	r.epoch          = h->epoch;
	r.capacity_bytes = h->capacity_bytes;
	r.live_bytes     = h->live_bytes;
	r.count          = h->count;
	r.waypoint_id    = h->tail_id ? h->tail_id : h->next_free;
	r.head_id        = h->head_id;
	r.evict_from     = evict_from;
	r.evict_to       = evict_to;
	return blob_db_update(h->root, &r, sizeof(r));
}

/* ---- eviction ---------------------------------------------------------- */

/* Delete the run [from, to) suffix-first (from is deleted last), so a crash
 * mid-delete leaves a prefix still walkable from `from` for idempotent
 * recovery. Best-effort: delete errors leave reclaimable garbage, no
 * correctness impact. */
static void run_delete(uint64_t from, uint64_t to)
{
	uint64_t run[CLOG_K];
	int n = 0;
	uint64_t cur = from;

	while (n < (int)ARRAY_SIZE(run) && cur != to) {
		uint64_t nxt;
		int rc = entry_read(cur, &nxt, NULL, 0, NULL);

		if (rc == -ENOENT) {
			break;   /* reached the already-deleted suffix */
		}
		if (rc < 0) {
			break;   /* best-effort */
		}
		run[n++] = cur;
		cur = nxt;
	}
	for (int i = n - 1; i >= 0; i--) {
		(void)blob_db_delete(run[i]);
	}
}

/* Checkpoint: optionally evict a run of oldest entries (when over the soft
 * budget, or when @p force), then commit the root — a single write that
 * advances head, refreshes the waypoint, and records the inline eviction
 * intent. The evicted run is deleted suffix-first after the commit. */
static int checkpoint(logring_t *h, bool force)
{
	uint64_t run[CLOG_K];
	int n = 0;
	uint32_t freed = 0;
	uint64_t from = 0, to = 0;
	bool want = force || h->live_bytes > h->capacity_bytes;

	if (want && h->head_id != 0) {
		uint64_t cur = h->head_id;

		while (n < (int)ARRAY_SIZE(run) && cur != 0 && cur != h->tail_id) {
			uint64_t nxt;
			size_t len;
			int rc = entry_read(cur, &nxt, NULL, 0, &len);

			if (rc < 0) {
				return rc;
			}
			run[n++] = cur;
			freed += (uint32_t)len;
			cur = nxt;
			if (!force &&
			    (h->live_bytes - freed) <= h->capacity_bytes) {
				break;
			}
		}
		if (n > 0) {
			from = h->head_id;
			to = cur;
			h->head_id = cur;
			h->live_bytes -= freed;
			h->count -= (uint32_t)n;
		}
	}

	int rc = root_write(h, from, to);   /* COMMIT */

	if (rc < 0) {
		return rc;
	}
	if (n > 0) {
		run_delete(from, to);   /* suffix-first, best-effort */
	}
	h->since_ckpt = 0;
	return 0;
}

/* ---- lifecycle --------------------------------------------------------- */

int logring_create(const struct logring_cfg *cfg, uint64_t *out_root)
{
	if (!cfg || !out_root || cfg->capacity_bytes == 0) {
		return -EINVAL;
	}

	uint64_t root = blob_db_alloc_id();
	uint64_t first = blob_db_alloc_id();

	if (root == 0 || first == 0) {
		return -EIO;   /* not mounted / id ceiling */
	}

	uint32_t epoch = sys_rand32_get();

	if (epoch == 0) {
		epoch = 1;   /* 0 is the "unchecked" sentinel in a cursor */
	}

	logring_t h = {
		.root = root, .epoch = epoch, .capacity_bytes = cfg->capacity_bytes,
		.tail_id = 0, .next_free = first, .head_id = 0,
		.live_bytes = 0, .count = 0, .since_ckpt = 0,
	};

	int rc = root_write(&h, 0, 0);   /* bind the empty log (waypoint = first) */

	if (rc < 0) {
		return rc;
	}
	*out_root = root;
	LOG_DBG("created log root=%llu epoch=%u", (unsigned long long)root, epoch);
	return 0;
}

int logring_open(uint64_t root, logring_t *h)
{
	if (root == 0 || !h) {
		return -EINVAL;
	}

	struct clog_root r;
	int rc = root_load(root, &r);

	if (rc < 0) {
		return rc;
	}

	/* Finish an interrupted eviction (idempotent, suffix-first). */
	if (r.evict_from != 0) {
		run_delete(r.evict_from, r.evict_to);
	}

	/* Rebuild the tail: walk from the waypoint to the first unbound id.
	 * ids strictly increase along the chain, which bounds the walk and
	 * catches a cycle. The walk also recovers the exact count/bytes of the
	 * entries appended since the last checkpoint (those from the waypoint
	 * onward), so the hints stay exact across reopen rather than lagging. */
	uint64_t cur = r.waypoint_id, tail = 0, next_free = r.waypoint_id;
	uint32_t w_bound = 0;
	uint64_t w_bytes = 0;
	size_t first_len = 0;
	bool first = true;

	for (;;) {
		uint64_t nxt;
		size_t len;
		int rc2 = entry_read(cur, &nxt, NULL, 0, &len);

		if (rc2 == -ENOENT) {
			next_free = cur;   /* unbound frontier */
			break;
		}
		if (rc2 < 0) {
			return rc2;
		}
		if (nxt <= cur) {
			return -EIO;   /* ids must increase */
		}
		if (first) {
			first_len = len;
			first = false;
		}
		w_bound++;
		w_bytes += len;
		tail = cur;
		cur = nxt;
	}

	h->root = root;
	h->epoch = r.epoch;
	h->capacity_bytes = r.capacity_bytes;
	h->tail_id = tail;
	h->next_free = next_free;
	h->head_id = r.head_id;
	h->since_ckpt = 0;

	if (r.head_id == 0) {
		/* Empty, or a pre-first-checkpoint crash: the root's totals are
		 * virgin and the waypoint (if bound) is an uncounted entry. */
		h->count = w_bound;
		h->live_bytes = (uint32_t)w_bytes;
		if (tail != 0) {
			h->head_id = r.waypoint_id;   /* the oldest, nothing evicted yet */
		}
	} else {
		/* The waypoint was the tail at the last checkpoint (already in
		 * the root totals); add only the entries after it. */
		h->count = r.count + (w_bound - 1);
		h->live_bytes = r.live_bytes + (uint32_t)(w_bytes - first_len);
	}
	return 0;
}

int logring_destroy(logring_t *h)
{
	if (!h) {
		return -EINVAL;
	}

	/* Finish any pending eviction, then delete the live chain, then the
	 * root. Best-effort on individual deletes. */
	struct clog_root r;

	if (root_load(h->root, &r) == 0 && r.evict_from != 0) {
		run_delete(r.evict_from, r.evict_to);
	}

	uint64_t cur = h->head_id;

	while (cur != 0 && cur != h->next_free) {
		uint64_t nxt;
		int rc = entry_read(cur, &nxt, NULL, 0, NULL);

		if (rc < 0) {
			break;
		}
		uint64_t del = cur;

		cur = nxt;
		(void)blob_db_delete(del);
	}
	return blob_db_delete(h->root);
}

/* ---- append ------------------------------------------------------------ */

int logring_append(logring_t *h, const void *rec, size_t len, uint64_t *out_id)
{
	if (!h || (!rec && len > 0)) {
		return -EINVAL;
	}
	if (len > CLOG_MAX_RECORD) {
		return -EMSGSIZE;
	}

	uint64_t r_id = h->next_free;
	uint64_t r2 = blob_db_alloc_id();

	if (r2 == 0) {
		return -EIO;
	}

	int rc = entry_write(r_id, r2, rec, len);

	if (rc == -ENOSPC) {
		/* Hard backstop: free space by evicting, then retry the bind.
		 * Loop until it fits or the log cannot shrink further. */
		while (rc == -ENOSPC && h->head_id != 0 &&
		       h->head_id != h->tail_id) {
			int ec = checkpoint(h, true);

			if (ec < 0) {
				return ec;
			}
			rc = entry_write(r_id, r2, rec, len);
		}
	}
	if (rc < 0) {
		return rc;   /* -ENOSPC exhausted, or -EIO */
	}

	bool was_empty = (h->tail_id == 0);

	h->tail_id = r_id;
	h->next_free = r2;
	h->live_bytes += (uint32_t)len;
	h->count += 1;
	h->since_ckpt += 1;

	if (was_empty) {
		h->head_id = r_id;
		rc = checkpoint(h, false);   /* persist head + waypoint */
	} else if (h->since_ckpt >= CLOG_K) {
		rc = checkpoint(h, false);
	}
	if (rc < 0) {
		/* The entry is committed and reachable; only the checkpoint
		 * hint write failed. Report success — open rebuilds from the
		 * last good checkpoint. */
		LOG_WRN("checkpoint after append failed: %d", rc);
	}

	if (out_id) {
		*out_id = r_id;
	}
	return 0;
}

/* ---- reading ----------------------------------------------------------- */

int logring_seek_oldest(logring_t *h, logring_cursor *c)
{
	if (!h || !c) {
		return -EINVAL;
	}
	c->pos = 0;          /* 0 = oldest; resolved at next() */
	c->epoch = h->epoch;
	return 0;
}

int logring_seek_newest(logring_t *h, logring_cursor *c)
{
	if (!h || !c) {
		return -EINVAL;
	}
	c->pos = h->tail_id; /* 0 if empty → next() returns -ENOENT */
	c->epoch = h->epoch;
	return 0;
}

int logring_next(logring_t *h, logring_cursor *c, void *out, size_t out_sz,
		 size_t *out_len, uint64_t *out_id)
{
	if (!h || !c || (!out && out_sz > 0)) {
		return -EINVAL;
	}
	if (c->epoch != 0 && c->epoch != h->epoch) {
		c->pos = 0;
		c->epoch = h->epoch;
		return -ESTALE;   /* moniker from another incarnation */
	}

	uint64_t start = c->pos ? c->pos : h->head_id;

	if (start == 0) {
		return -ENOENT;   /* empty log */
	}

	uint64_t nxt;
	size_t len;
	int rc = entry_read(start, &nxt, out, out_sz, &len);

	if (rc == -ENOENT) {
		if (start < h->next_free) {
			c->pos = 0;
			c->epoch = h->epoch;
			return -ESTALE;   /* was allocated, now evicted */
		}
		return -ENOENT;           /* unbound frontier → caught up */
	}
	if (rc < 0) {
		return rc;                /* -EMSGSIZE (no advance) or -EIO */
	}

	c->pos = nxt;
	c->epoch = h->epoch;
	if (out_len) {
		*out_len = len;
	}
	if (out_id) {
		*out_id = start;
	}
	return 0;
}

void logring_cursor_to_moniker(const logring_cursor *c,
			       struct logring_moniker *out)
{
	out->epoch = c->epoch;
	out->id = c->pos;
}

int logring_cursor_from_moniker(logring_cursor *c,
				const struct logring_moniker *m)
{
	if (!c || !m) {
		return -EINVAL;
	}
	c->pos = m->id;
	c->epoch = m->epoch;
	return 0;
}

/* ---- introspection / maintenance -------------------------------------- */

int logring_stats(logring_t *h, struct logring_stats *out)
{
	if (!h || !out) {
		return -EINVAL;
	}
	out->oldest_id = h->head_id;
	out->newest_id = h->tail_id;
	out->count = h->count;
	out->bytes = h->live_bytes;
	return 0;
}

int logring_reset(logring_t *h)
{
	if (!h) {
		return -EINVAL;
	}

	/* Delete the live chain. */
	uint64_t cur = h->head_id;

	while (cur != 0 && cur != h->next_free) {
		uint64_t nxt;
		int rc = entry_read(cur, &nxt, NULL, 0, NULL);

		if (rc < 0) {
			break;
		}
		uint64_t del = cur;

		cur = nxt;
		(void)blob_db_delete(del);
	}

	/* Re-init to empty, keeping the epoch (same incarnation). A fresh
	 * reservation anchors the new (empty) chain. */
	uint64_t first = blob_db_alloc_id();

	if (first == 0) {
		return -EIO;
	}
	h->tail_id = 0;
	h->next_free = first;
	h->head_id = 0;
	h->live_bytes = 0;
	h->count = 0;
	h->since_ckpt = 0;
	return root_write(h, 0, 0);
}
