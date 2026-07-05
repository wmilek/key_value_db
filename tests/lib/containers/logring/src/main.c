/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite for the logring container — cases mirror the shape-conformance
 * checklist (l2_containers.md §8) plus logring's own eviction / sequence
 * semantics (§4.5).
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/logring.h>

static void logring_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_format());
}

static void logring_after(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
}

ZTEST_SUITE(logring, NULL, NULL, logring_before, logring_after, NULL);

/* ---- iteration helper ------------------------------------------------- */

struct rec_acc {
	int n;
	uint64_t seqs[64];
	size_t lens[64];
	uint8_t first_byte[64];
};

static int collect_cb(uint64_t seq, const void *data, size_t len, void *user)
{
	struct rec_acc *a = user;

	if (a->n < (int)ARRAY_SIZE(a->seqs)) {
		a->seqs[a->n] = seq;
		a->lens[a->n] = len;
		a->first_byte[a->n] = len ? ((const uint8_t *)data)[0] : 0;
	}
	a->n++;
	return 0;
}

static int stop_cb(uint64_t seq, const void *data, size_t len, void *user)
{
	ARG_UNUSED(seq);
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	ARG_UNUSED(user);
	return 55;
}

/* ---- basics ----------------------------------------------------------- */

/* create -> open -> empty behavior. */
ZTEST(logring, test_create_open_empty)
{
	uint64_t root;

	zassert_ok(logring_create(&root));
	zassert_true(root >= 2, "root must be a user id");
	zassert_ok(logring_open(root));

	size_t count;

	zassert_ok(logring_count(root, &count));
	zassert_equal(count, 0);

	uint64_t seq;

	zassert_equal(logring_oldest_seq(root, &seq), -ENOENT);
	zassert_equal(logring_newest_seq(root, &seq), -ENOENT);

	struct rec_acc a = { 0 };

	zassert_ok(logring_iterate(root, collect_cb, &a));
	zassert_equal(a.n, 0);
}

/* append/iterate round-trip; sequence numbers start at 1 and increase. */
ZTEST(logring, test_append_iterate_roundtrip)
{
	uint64_t root, seq;

	zassert_ok(logring_create(&root));

	zassert_ok(logring_append(root, "aa", 2, &seq));
	zassert_equal(seq, 1);
	zassert_ok(logring_append(root, "bbb", 3, &seq));
	zassert_equal(seq, 2);
	zassert_ok(logring_append(root, "c", 1, &seq));
	zassert_equal(seq, 3);

	size_t count;

	zassert_ok(logring_count(root, &count));
	zassert_equal(count, 3);

	zassert_ok(logring_oldest_seq(root, &seq));
	zassert_equal(seq, 1);
	zassert_ok(logring_newest_seq(root, &seq));
	zassert_equal(seq, 3);

	struct rec_acc a = { 0 };

	zassert_ok(logring_iterate(root, collect_cb, &a));
	zassert_equal(a.n, 3);
	zassert_equal(a.seqs[0], 1);
	zassert_equal(a.lens[0], 2);
	zassert_equal(a.first_byte[0], 'a');
	zassert_equal(a.seqs[1], 2);
	zassert_equal(a.lens[1], 3);
	zassert_equal(a.first_byte[1], 'b');
	zassert_equal(a.seqs[2], 3);
	zassert_equal(a.lens[2], 1);
	zassert_equal(a.first_byte[2], 'c');
}

/* Zero-length records are allowed and consume a slot + a sequence number. */
ZTEST(logring, test_zero_length_record)
{
	uint64_t root, seq;

	zassert_ok(logring_create(&root));
	zassert_ok(logring_append(root, NULL, 0, &seq));
	zassert_equal(seq, 1);

	size_t count;

	zassert_ok(logring_count(root, &count));
	zassert_equal(count, 1);

	struct rec_acc a = { 0 };

	zassert_ok(logring_iterate(root, collect_cb, &a));
	zassert_equal(a.n, 1);
	zassert_equal(a.lens[0], 0);
}

/* Binary data with embedded NULs round-trips intact. */
ZTEST(logring, test_binary_payload)
{
	uint64_t root;
	const uint8_t blob[5] = { 0x00, 0xff, 0x00, 0x41, 0x00 };

	zassert_ok(logring_create(&root));
	zassert_ok(logring_append(root, blob, sizeof(blob), NULL));

	struct rec_acc a = { 0 };

	zassert_ok(logring_iterate(root, collect_cb, &a));
	zassert_equal(a.n, 1);
	zassert_equal(a.lens[0], sizeof(blob));
	zassert_equal(a.first_byte[0], 0x00);
}

/* ---- eviction & sequence semantics ------------------------------------ */

/* Fill well past capacity: oldest records drop, count is bounded, sequence
 * numbers stay monotonic and contiguous, and the gap is detectable. */
ZTEST(logring, test_eviction_keeps_newest)
{
	uint64_t root, seq;

	zassert_ok(logring_create(&root));

	/* 40-byte records => 50 bytes each on-ring. */
	uint8_t rec[40];

	memset(rec, 'x', sizeof(rec));

	const size_t per = sizeof(rec) + LOGRING_REC_OVERHEAD;
	const size_t fit = LOGRING_CAPACITY / per;   /* records that survive */

	zassert_true(fit >= 1 && fit < 100, "unexpected capacity math");

	const int total = (int)fit + 20;   /* force many evictions */

	for (int i = 0; i < total; i++) {
		zassert_ok(logring_append(root, rec, sizeof(rec), &seq));
		zassert_equal(seq, (uint64_t)(i + 1), "seq must be monotonic");
	}

	size_t count;

	zassert_ok(logring_count(root, &count));
	zassert_equal(count, fit, "ring must hold exactly the capacity");

	/* Newest is the last appended; oldest is newest-count+1. */
	zassert_ok(logring_newest_seq(root, &seq));
	zassert_equal(seq, (uint64_t)total);
	zassert_ok(logring_oldest_seq(root, &seq));
	zassert_equal(seq, (uint64_t)(total - (int)fit + 1));

	/* Iteration yields exactly the surviving contiguous window. */
	struct rec_acc a = { 0 };

	zassert_ok(logring_iterate(root, collect_cb, &a));
	zassert_equal((size_t)a.n, fit);
	for (int i = 0; i < a.n; i++) {
		zassert_equal(a.seqs[i],
			      (uint64_t)(total - (int)fit + 1 + i),
			      "surviving seqs must be contiguous & in order");
	}
}

/* A record larger than the whole ring is rejected and changes nothing. */
ZTEST(logring, test_oversized_record_rejected)
{
	uint64_t root, seq;

	zassert_ok(logring_create(&root));
	zassert_ok(logring_append(root, "keep", 4, &seq));
	zassert_equal(seq, 1);

	uint8_t big[LOGRING_MAX_RECORD + 1];

	memset(big, 'z', sizeof(big));
	zassert_equal(logring_append(root, big, sizeof(big), NULL), -ENOSPC);

	/* The pre-existing record is untouched. */
	size_t count;

	zassert_ok(logring_count(root, &count));
	zassert_equal(count, 1);
	zassert_ok(logring_oldest_seq(root, &seq));
	zassert_equal(seq, 1);

	/* The largest allowed record still fits. */
	zassert_ok(logring_append(root, big, LOGRING_MAX_RECORD, &seq));
	zassert_equal(seq, 2);
}

/* ---- reset ------------------------------------------------------------ */

/* reset empties the ring but keeps the sequence space monotonic. */
ZTEST(logring, test_reset_keeps_seq_monotonic)
{
	uint64_t root, seq;

	zassert_ok(logring_create(&root));
	zassert_ok(logring_append(root, "a", 1, NULL));
	zassert_ok(logring_append(root, "b", 1, NULL));
	zassert_ok(logring_append(root, "c", 1, &seq));
	zassert_equal(seq, 3);

	zassert_ok(logring_reset(root));

	size_t count;

	zassert_ok(logring_count(root, &count));
	zassert_equal(count, 0);
	zassert_equal(logring_oldest_seq(root, &seq), -ENOENT);

	/* Next append continues the sequence — a reset looks like a full
	 * eviction to a seq-tracking consumer. */
	zassert_ok(logring_append(root, "d", 1, &seq));
	zassert_equal(seq, 4);
}

/* ---- persistence ------------------------------------------------------ */

/* The ring — records and sequence counter — survives unmount/mount. */
ZTEST(logring, test_persists_across_remount)
{
	uint64_t root, seq;

	zassert_ok(logring_create(&root));
	zassert_ok(logring_append(root, "one", 3, NULL));
	zassert_ok(logring_append(root, "two", 3, &seq));
	zassert_equal(seq, 2);

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());

	zassert_ok(logring_open(root));

	size_t count;

	zassert_ok(logring_count(root, &count));
	zassert_equal(count, 2);

	struct rec_acc a = { 0 };

	zassert_ok(logring_iterate(root, collect_cb, &a));
	zassert_equal(a.n, 2);
	zassert_equal(a.seqs[0], 1);
	zassert_equal(a.seqs[1], 2);

	/* Sequence counter is durable: the next append is 3, not 1. */
	zassert_ok(logring_append(root, "three", 5, &seq));
	zassert_equal(seq, 3);
}

/* ---- typing / errors -------------------------------------------------- */

/* open/append against a foreign blob fail with -ENOTSUP; against an unbound
 * id with -ENOENT. */
ZTEST(logring, test_wrong_type_and_missing)
{
	uint64_t foreign = blob_db_alloc_id();

	zassert_true(foreign >= 2);
	zassert_ok(blob_db_update(foreign, "not a logring", 13));

	zassert_equal(logring_open(foreign), -ENOTSUP);
	zassert_equal(logring_append(foreign, "x", 1, NULL), -ENOTSUP);

	size_t count;

	zassert_equal(logring_count(foreign, &count), -ENOTSUP);

	/* Allocated but never bound. */
	uint64_t unbound = blob_db_alloc_id();

	zassert_true(unbound >= 2);
	zassert_equal(logring_open(unbound), -ENOENT);
}

/* Argument validation. */
ZTEST(logring, test_invalid_args)
{
	uint64_t root, seq;
	size_t count;

	zassert_ok(logring_create(&root));

	zassert_equal(logring_create(NULL), -EINVAL);
	zassert_equal(logring_open(0), -EINVAL);
	zassert_equal(logring_append(0, "x", 1, NULL), -EINVAL);
	zassert_equal(logring_append(root, NULL, 4, NULL), -EINVAL);
	zassert_equal(logring_count(root, NULL), -EINVAL);
	zassert_equal(logring_count(0, &count), -EINVAL);
	zassert_equal(logring_oldest_seq(root, NULL), -EINVAL);
	zassert_equal(logring_newest_seq(0, &seq), -EINVAL);
	zassert_equal(logring_reset(0), -EINVAL);
	zassert_equal(logring_iterate(root, NULL, NULL), -EINVAL);
}

/* Iteration callback can stop early, propagating its value. */
ZTEST(logring, test_iterate_early_stop)
{
	uint64_t root;

	zassert_ok(logring_create(&root));
	zassert_ok(logring_append(root, "a", 1, NULL));
	zassert_ok(logring_append(root, "b", 1, NULL));

	zassert_equal(logring_iterate(root, stop_cb, NULL), 55);
}

/* Two independent rings do not interfere; each has its own sequence space. */
ZTEST(logring, test_two_independent_rings)
{
	uint64_t r1, r2, seq;

	zassert_ok(logring_create(&r1));
	zassert_ok(logring_create(&r2));
	zassert_not_equal(r1, r2);

	zassert_ok(logring_append(r1, "a", 1, &seq));
	zassert_equal(seq, 1);
	zassert_ok(logring_append(r1, "b", 1, &seq));
	zassert_equal(seq, 2);
	zassert_ok(logring_append(r2, "z", 1, &seq));
	zassert_equal(seq, 1, "second ring has its own sequence space");

	size_t c1, c2;

	zassert_ok(logring_count(r1, &c1));
	zassert_ok(logring_count(r2, &c2));
	zassert_equal(c1, 2);
	zassert_equal(c2, 1);
}
