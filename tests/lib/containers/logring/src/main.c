/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite for the logring container (per-entry chain). Mirrors the test
 * plan in doc/proposals/2026-10-08-logring.md §10. CHECKPOINT is small (see
 * prj.conf) so a handful of appends exercises checkpoints and eviction.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/logring.h>

static void lr_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
}

static void lr_after(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
}

ZTEST_SUITE(logring, NULL, NULL, lr_before, lr_after, NULL);

/* Drain oldest->newest into arrays; returns the terminal code. */
struct acc {
	int n;
	uint64_t ids[256];
	size_t lens[256];
	uint8_t fb[256];
};

static int drain(logring_t *h, logring_cursor *c, struct acc *a)
{
	uint8_t buf[64];
	size_t len;
	uint64_t id;
	int rc;

	a->n = 0;
	while ((rc = logring_next(h, c, buf, sizeof(buf), &len, &id)) == 0) {
		if (a->n < (int)ARRAY_SIZE(a->ids)) {
			a->ids[a->n] = id;
			a->lens[a->n] = len;
			a->fb[a->n] = len ? buf[0] : 0;
		}
		a->n++;
	}
	return rc;
}

static int mk(struct logring_cfg cfg, uint64_t *root, logring_t *h)
{
	int rc = logring_create(&cfg, root);

	if (rc) {
		return rc;
	}
	return logring_open(*root, h);
}

/* §10.1 — create/open/empty. */
ZTEST(logring, test_create_open_empty)
{
	struct logring_cfg cfg = { .capacity_bytes = 100000 };
	uint64_t root;
	logring_t h;

	zassert_ok(mk(cfg, &root, &h));

	struct logring_stats st;

	zassert_ok(logring_stats(&h, &st));
	zassert_equal(st.count, 0);
	zassert_equal(st.newest_id, 0);

	logring_cursor c;
	struct acc a;

	zassert_ok(logring_seek_oldest(&h, &c));
	zassert_equal(drain(&h, &c, &a), -ENOENT);
	zassert_equal(a.n, 0);

	zassert_ok(logring_seek_newest(&h, &c));
	uint8_t one[8];

	zassert_equal(logring_next(&h, &c, one, sizeof(one), NULL, NULL), -ENOENT);
}

/* §10.2 — append/drain order, increasing ids, seek_newest. */
ZTEST(logring, test_append_drain_and_newest)
{
	struct logring_cfg cfg = { .capacity_bytes = 100000 };
	uint64_t root, id;
	logring_t h;

	zassert_ok(mk(cfg, &root, &h));
	for (int i = 0; i < 10; i++) {
		char r = (char)('A' + i);

		zassert_ok(logring_append(&h, &r, 1, &id));
	}

	struct logring_stats st;

	zassert_ok(logring_stats(&h, &st));
	zassert_equal(st.count, 10);

	logring_cursor c;
	struct acc a;

	zassert_ok(logring_seek_oldest(&h, &c));
	zassert_equal(drain(&h, &c, &a), -ENOENT);
	zassert_equal(a.n, 10);
	for (int i = 0; i < 10; i++) {
		zassert_equal(a.fb[i], 'A' + i);
		if (i) {
			zassert_true(a.ids[i] > a.ids[i - 1]);
		}
	}

	uint8_t one[8];

	zassert_ok(logring_seek_newest(&h, &c));
	zassert_ok(logring_next(&h, &c, one, sizeof(one), NULL, &id));
	zassert_equal(one[0], 'J');
	zassert_equal(logring_next(&h, &c, one, sizeof(one), NULL, &id), -ENOENT);
}

/* §10.3 — pre-reservation: tail.next_id is unbound; next append binds it. */
ZTEST(logring, test_prereservation)
{
	struct logring_cfg cfg = { .capacity_bytes = 100000 };
	uint64_t root, id1, id2;
	logring_t h;

	zassert_ok(mk(cfg, &root, &h));
	zassert_ok(logring_append(&h, "a", 1, &id1));
	/* next_free is reserved (allocated, unbound) → not yet a live blob */
	uint64_t nf = h.next_free;

	zassert_false(blob_db_exists(nf));
	zassert_ok(logring_append(&h, "b", 1, &id2));
	zassert_equal(id2, nf, "second append binds exactly the reserved id");
}

/* §10.9 — persistence + tail rebuild across unmount/mount. */
ZTEST(logring, test_persist_remount)
{
	struct logring_cfg cfg = { .capacity_bytes = 100000 };
	uint64_t root, id;
	logring_t h;

	zassert_ok(mk(cfg, &root, &h));
	for (int i = 0; i < 10; i++) {
		char r = (char)('a' + i);

		zassert_ok(logring_append(&h, &r, 1, &id));
	}

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	logring_t h2;

	zassert_ok(logring_open(root, &h2));

	struct logring_stats st;

	zassert_ok(logring_stats(&h2, &st));
	zassert_equal(st.count, 10, "count exact after remount (walk recovers delta)");

	logring_cursor c;
	struct acc a;

	zassert_ok(logring_seek_oldest(&h2, &c));
	zassert_equal(drain(&h2, &c, &a), -ENOENT);
	zassert_equal(a.n, 10);
	zassert_equal(a.fb[9], 'j');

	/* append continues after remount */
	zassert_ok(logring_append(&h2, "k", 1, &id));
	uint8_t one[8];

	zassert_ok(logring_seek_newest(&h2, &c));
	zassert_ok(logring_next(&h2, &c, one, sizeof(one), NULL, &id));
	zassert_equal(one[0], 'k');
}

/* §10.6 — cursor resume via moniker across unmount/mount. */
ZTEST(logring, test_cursor_resume_moniker)
{
	struct logring_cfg cfg = { .capacity_bytes = 100000 };
	uint64_t root, id;
	logring_t h;
	logring_cursor c;
	uint8_t one[8];

	zassert_ok(mk(cfg, &root, &h));
	for (int i = 0; i < 6; i++) {
		char r = (char)('0' + i);

		zassert_ok(logring_append(&h, &r, 1, &id));
	}
	zassert_ok(logring_seek_oldest(&h, &c));
	zassert_ok(logring_next(&h, &c, one, sizeof(one), NULL, &id));
	zassert_equal(one[0], '0');
	zassert_ok(logring_next(&h, &c, one, sizeof(one), NULL, &id));
	zassert_equal(one[0], '1');

	struct logring_moniker m;

	logring_cursor_to_moniker(&c, &m);

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	logring_t h2;
	logring_cursor c2;

	zassert_ok(logring_open(root, &h2));
	zassert_ok(logring_cursor_from_moniker(&c2, &m));
	zassert_ok(logring_next(&h2, &c2, one, sizeof(one), NULL, &id));
	zassert_equal(one[0], '2', "resume continues after the saved position");
}

/* §10.7 — epoch (G2): stale/foreign/random moniker → -ESTALE. */
ZTEST(logring, test_epoch_rejects_stale)
{
	struct logring_cfg cfg = { .capacity_bytes = 100000 };
	uint64_t ra, rb, id;
	logring_t ha, hb;
	logring_cursor c;
	uint8_t one[8];

	zassert_ok(mk(cfg, &ra, &ha));
	zassert_ok(logring_append(&ha, "x", 1, &id));
	zassert_ok(logring_seek_oldest(&ha, &c));
	zassert_ok(logring_next(&ha, &c, one, sizeof(one), NULL, &id));

	struct logring_moniker m;

	logring_cursor_to_moniker(&c, &m);

	/* A different log (independent epoch): the moniker must not resolve. */
	zassert_ok(mk(cfg, &rb, &hb));
	zassert_ok(logring_append(&hb, "y", 1, &id));

	logring_cursor cb;

	zassert_ok(logring_cursor_from_moniker(&cb, &m));
	zassert_equal(logring_next(&hb, &cb, one, sizeof(one), NULL, &id), -ESTALE);

	/* A random moniker → -ESTALE (epoch mismatch). */
	struct logring_moniker mr = { .epoch = m.epoch ^ 0xA5A5A5A5u, .id = 123456 };

	zassert_ok(logring_cursor_from_moniker(&cb, &mr));
	zassert_equal(logring_next(&hb, &cb, one, sizeof(one), NULL, &id), -ESTALE);
}

/* §10.5/§10.8 — soft ring eviction + evicted-position detection. */
ZTEST(logring, test_eviction)
{
	struct logring_cfg cfg = { .capacity_bytes = 32 };   /* ~32 bytes of records */
	uint64_t root, id, first = 0;
	logring_t h;

	zassert_ok(mk(cfg, &root, &h));
	for (int i = 0; i < 50; i++) {
		uint8_t r[4] = { 'p', 'q', 'r', 's' };
		uint64_t gid;

		zassert_ok(logring_append(&h, r, sizeof(r), &gid));
		if (i == 0) {
			first = gid;
		}
	}

	struct logring_stats st;

	zassert_ok(logring_stats(&h, &st));
	zassert_true(st.count < 50 && st.count >= 1, "oldest evicted");
	zassert_true(st.bytes <= cfg.capacity_bytes +
		     (CONFIG_BLOB_CONTAINER_LOGRING_CHECKPOINT + 1) * 4,
		     "within one checkpoint's overshoot of the budget");

	/* a moniker at the long-evicted first entry → -ESTALE */
	struct logring_moniker m = { .epoch = h.epoch, .id = first };
	logring_cursor c;
	uint8_t one[8];

	zassert_ok(logring_cursor_from_moniker(&c, &m));
	zassert_equal(logring_next(&h, &c, one, sizeof(one), NULL, &id), -ESTALE);

	/* survivors drain cleanly; reopen stays consistent */
	struct acc a;

	zassert_ok(logring_seek_oldest(&h, &c));
	zassert_equal(drain(&h, &c, &a), -ENOENT);
	zassert_equal((uint32_t)a.n, st.count);

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	logring_t h2;
	struct acc a2;

	zassert_ok(logring_open(root, &h2));
	zassert_ok(logring_seek_oldest(&h2, &c));
	zassert_equal(drain(&h2, &c, &a2), -ENOENT);
	zassert_equal(a2.n, a.n, "eviction state survives remount");
}

/* §10.2 — zero-length and binary records. */
ZTEST(logring, test_zero_and_binary)
{
	struct logring_cfg cfg = { .capacity_bytes = 100000 };
	uint64_t root, id;
	logring_t h;
	logring_cursor c;

	zassert_ok(mk(cfg, &root, &h));
	zassert_ok(logring_append(&h, NULL, 0, &id));
	const uint8_t bin[3] = { 0x00, 0xff, 0x00 };

	zassert_ok(logring_append(&h, bin, sizeof(bin), &id));

	zassert_ok(logring_seek_oldest(&h, &c));
	uint8_t buf[8];
	size_t len;

	zassert_ok(logring_next(&h, &c, buf, sizeof(buf), &len, &id));
	zassert_equal(len, 0);
	zassert_ok(logring_next(&h, &c, buf, sizeof(buf), &len, &id));
	zassert_equal(len, 3);
	zassert_equal(buf[1], 0xff);
}

/* §10.11 — reset then append; destroy removes everything. */
ZTEST(logring, test_reset_and_destroy)
{
	struct logring_cfg cfg = { .capacity_bytes = 100000 };
	uint64_t root, id;
	logring_t h;
	logring_cursor c;
	uint8_t one[8];

	zassert_ok(mk(cfg, &root, &h));
	for (int i = 0; i < 5; i++) {
		zassert_ok(logring_append(&h, "x", 1, &id));
	}
	zassert_ok(logring_reset(&h));

	struct logring_stats st;

	zassert_ok(logring_stats(&h, &st));
	zassert_equal(st.count, 0);
	zassert_ok(logring_append(&h, "Z", 1, &id));
	zassert_ok(logring_seek_oldest(&h, &c));
	zassert_ok(logring_next(&h, &c, one, sizeof(one), NULL, &id));
	zassert_equal(one[0], 'Z');

	zassert_ok(logring_destroy(&h));
	logring_t h2;

	zassert_equal(logring_open(root, &h2), -ENOENT);
}

/* Typing and argument validation. */
ZTEST(logring, test_wrong_type_and_args)
{
	struct logring_cfg cfg = { .capacity_bytes = 100000 };
	uint64_t root, id;
	logring_t h;

	/* a foreign blob is not a logring root */
	uint64_t foreign = blob_db_alloc_id();

	zassert_true(foreign >= 2);
	zassert_ok(blob_db_update(foreign, "not a logring blob!!", 20));
	zassert_equal(logring_open(foreign, &h), -ENOTSUP);

	zassert_equal(logring_create(NULL, &root), -EINVAL);
	cfg.capacity_bytes = 0;
	zassert_equal(logring_create(&cfg, &root), -EINVAL);
	cfg.capacity_bytes = 100000;

	zassert_ok(mk(cfg, &root, &h));
	zassert_equal(logring_open(0, &h), -EINVAL);
	zassert_equal(logring_append(NULL, "x", 1, &id), -EINVAL);
	zassert_equal(logring_append(&h, NULL, 4, &id), -EINVAL);

	static uint8_t big[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];

	zassert_equal(logring_append(&h, big, sizeof(big), &id), -EMSGSIZE);
}
