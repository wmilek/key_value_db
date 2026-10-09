/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db_maintain(), the background helper that runs it on a work queue
 * (CONFIG_BLOB_DB_MAINT_WORK), and the library lock that makes both safe next
 * to another thread's blob_db calls.
 *
 * What a step does is the backend's business, so the cases here hold on both:
 * the work drains, draining changes nothing a reader sees, and calls from two
 * threads interleave without losing a write. The backend-specific halves are
 * the flash_area case below (fresh buckets get formatted) and the UBI cases
 * in src/ubi.c (blocks get reclaimed, and no blob_db call erases meanwhile).
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_inspect.h>

/* UBI without atomic replace erases every block inline, so it never defers
 * any: the cases that need work to exist skip there. */
#define SKIP_UNLESS_WORK_IS_DEFERRED()					\
	do {								\
		if (IS_ENABLED(CONFIG_BLOB_DB_BACKEND_UBI) &&		\
		    !IS_ENABLED(CONFIG_BLOB_DB_UBI_ATOMIC_REPLACE)) {	\
			ztest_test_skip();				\
		}							\
	} while (0)

/* More steps than any store in this suite has blocks: the 8 MB partition of
 * 4 KB sectors has about 2 000. */
#define DRAIN_LIMIT 8192

static void maint_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount(), "mount failed in setup");
	zassert_ok(blob_db_format(), "format failed in setup");
}

static void maint_after(void *fixture)
{
	ARG_UNUSED(fixture);
#if defined(CONFIG_BLOB_DB_MAINT_WORK)
	blob_db_maint_stop();
#endif
	blob_db_unmount();
}

ZTEST_SUITE(blob_db_maint, NULL, NULL, maint_before, maint_after, NULL);

/* Run one step at a time until nothing is left; return the steps taken. */
static uint32_t drain(void)
{
	uint32_t total = 0;

	for (int i = 0; i < DRAIN_LIMIT; i++) {
		struct blob_db_maint_result res;

		zassert_ok(blob_db_maintain(1, &res));
		zassert_true(res.performed <= 1, "a step budget of 1 ran %u",
			     res.performed);
		total += res.performed;
		if (!res.more) {
			return total;
		}
	}
	zassert_unreachable("maintenance did not drain in %d steps", DRAIN_LIMIT);
	return total;
}

/* Rebind one id until its bucket has compacted a few times. On UBI each
 * compaction leaves a block waiting for reclaim; on flash_area it leaves
 * fresh buckets fresh. Either way there is work afterwards. */
static uint64_t make_work(void)
{
	static uint8_t payload[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	const uint64_t id = blob_db_alloc_id();

	for (int i = 0; i < 64; i++) {
		memset(payload, i, sizeof(payload));
		zassert_ok(blob_db_update(id, payload, sizeof(payload)));
	}
	return id;
}

ZTEST(blob_db_maint, test_not_mounted)
{
	struct blob_db_maint_result res = { .performed = 7, .more = true };

	blob_db_unmount();
	zassert_equal(blob_db_maintain(1, &res), -ENODEV);
	zassert_equal(res.performed, 0);
	zassert_false(res.more);
}

/* Budget 0 reports and runs nothing: a second report says the same. */
ZTEST(blob_db_maint, test_zero_budget_only_reports)
{
	SKIP_UNLESS_WORK_IS_DEFERRED();

	struct blob_db_maint_result first;
	struct blob_db_maint_result again;

	(void)make_work();
	zassert_ok(blob_db_maintain(0, &first));
	zassert_equal(first.performed, 0);
	zassert_true(first.more, "make_work() must leave work to report");
	zassert_ok(blob_db_maintain(0, &again));
	zassert_equal(again.performed, 0);
	zassert_true(again.more);
	zassert_ok(blob_db_maintain(0, NULL));
}

/* Once drained there is nothing left, and a later call does nothing. */
ZTEST(blob_db_maint, test_drains_and_stays_drained)
{
	SKIP_UNLESS_WORK_IS_DEFERRED();

	struct blob_db_maint_result res;

	(void)make_work();
	zassert_true(drain() > 0, "make_work() must leave work to drain");
	zassert_ok(blob_db_maintain(8, &res));
	zassert_equal(res.performed, 0);
	zassert_false(res.more);
}

/* Draining changes nothing a reader sees, the root included, and the store
 * keeps working afterwards — across a remount too. */
ZTEST(blob_db_maint, test_drain_preserves_contents)
{
	static const uint8_t root[] = "root survives";
	static const uint8_t small[] = "small";
	uint8_t big[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	uint8_t buf[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
	size_t got;

	zassert_ok(blob_db_update(BLOB_DB_ROOT_ID, root, sizeof(root)));
	const uint64_t a = make_work();
	const uint64_t b = blob_db_alloc_id();

	zassert_ok(blob_db_update(b, small, sizeof(small)));
	(void)drain();

	memset(big, 63, sizeof(big));   /* make_work()'s last payload */
	zassert_ok(blob_db_get(a, buf, sizeof(buf), &got));
	zassert_equal(got, sizeof(big));
	zassert_mem_equal(buf, big, sizeof(big));
	zassert_ok(blob_db_get(BLOB_DB_ROOT_ID, buf, sizeof(buf), &got));
	zassert_mem_equal(buf, root, sizeof(root));

	const uint64_t c = blob_db_alloc_id();

	zassert_ok(blob_db_update(c, small, sizeof(small)));

	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_get(b, buf, sizeof(buf), &got));
	zassert_mem_equal(buf, small, sizeof(small));
	zassert_ok(blob_db_get(c, buf, sizeof(buf), &got));
	zassert_mem_equal(buf, small, sizeof(small));
	zassert_equal(blob_db_count(), 4);
}

/* flash_area: a step formats one never-written bucket, until every bucket
 * has a header; the steps taken are exactly the fresh buckets. */
ZTEST(blob_db_maint, test_flash_area_formats_fresh_buckets)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_BLOB_DB_BACKEND_FLASH_AREA);

	struct blob_db_inspect_info info;
	struct blob_db_inspect_bucket bucket;
	uint32_t fresh = 0;

	zassert_ok(blob_db_inspect_info_get(&info));
	for (uint16_t bid = 0; bid < info.n_buckets; bid++) {
		zassert_ok(blob_db_inspect_bucket_get(bid, &bucket, NULL, NULL));
		fresh += bucket.formatted ? 0 : 1;
	}
	zassert_true(fresh > 0, "a formatted store has fresh buckets");

	zassert_equal(drain(), fresh);
	for (uint16_t bid = 0; bid < info.n_buckets; bid++) {
		zassert_ok(blob_db_inspect_bucket_get(bid, &bucket, NULL, NULL));
		zassert_true(bucket.formatted, "bucket %u left fresh", bid);
	}

	/* The headers are on flash: a remount finds nothing to do. */
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	zassert_equal(drain(), 0);
}

/* The lock: two threads binding their own ids, interleaved at every call,
 * with maintenance in between, lose nothing. */
#define WORKER_IDS   24
#define WORKER_STACK 16384

static K_THREAD_STACK_DEFINE(g_worker_stack, WORKER_STACK);
static struct k_thread g_worker;
static uint64_t g_worker_ids[WORKER_IDS];

static void fill(uint8_t *buf, size_t len, uint64_t id)
{
	for (size_t i = 0; i < len; i++) {
		buf[i] = (uint8_t)(id * 31 + i);
	}
}

static void bind_ids(uint64_t *ids, size_t n)
{
	uint8_t payload[48];

	for (size_t i = 0; i < n; i++) {
		ids[i] = blob_db_alloc_id();
		fill(payload, sizeof(payload), ids[i]);
		zassert_ok(blob_db_update(ids[i], payload, sizeof(payload)));
		k_yield();
		(void)blob_db_maintain(1, NULL);
		k_yield();
	}
}

static void worker(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	bind_ids(g_worker_ids, WORKER_IDS);
}

ZTEST(blob_db_maint, test_two_threads_interleave)
{
	uint64_t mine[WORKER_IDS];
	uint8_t want[48];
	uint8_t buf[48];
	size_t got;

	k_thread_create(&g_worker, g_worker_stack, WORKER_STACK, worker, NULL,
			NULL, NULL, k_thread_priority_get(k_current_get()), 0,
			K_NO_WAIT);
	bind_ids(mine, WORKER_IDS);
	zassert_ok(k_thread_join(&g_worker, K_SECONDS(30)));

	for (size_t i = 0; i < WORKER_IDS; i++) {
		const uint64_t ids[] = { mine[i], g_worker_ids[i] };

		for (size_t k = 0; k < ARRAY_SIZE(ids); k++) {
			fill(want, sizeof(want), ids[k]);
			zassert_ok(blob_db_get(ids[k], buf, sizeof(buf), &got));
			zassert_equal(got, sizeof(want));
			zassert_mem_equal(buf, want, sizeof(want));
		}
	}
	zassert_equal(blob_db_count(), 1 + 2 * WORKER_IDS);
}

#if defined(CONFIG_BLOB_DB_MAINT_WORK)
/* Generous: what a step takes on a target is measured there, not here —
 * native_sim runs every thread on a host stack. */
#define MAINT_Q_STACK 8192

static K_THREAD_STACK_DEFINE(g_maint_q_stack, MAINT_Q_STACK);
static struct k_work_q g_maint_q;
static bool g_maint_q_started;

static void maint_q_start(void)
{
	if (!g_maint_q_started) {
		k_work_queue_start(&g_maint_q, g_maint_q_stack, MAINT_Q_STACK,
				   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
		g_maint_q_started = true;
	}
}

/* Wait for the helper to leave nothing to do. */
static void wait_drained(void)
{
	for (int i = 0; i < 500; i++) {
		struct blob_db_maint_result res;

		zassert_ok(blob_db_maintain(0, &res));
		if (!res.more) {
			return;
		}
		k_msleep(10);
	}
	zassert_unreachable("the helper did not drain the work");
}
#endif

/* The helper drains on its own queue once started, and the work it leaves is
 * none. */
ZTEST(blob_db_maint, test_helper_drains_in_background)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_BLOB_DB_MAINT_WORK);
	SKIP_UNLESS_WORK_IS_DEFERRED();
#if defined(CONFIG_BLOB_DB_MAINT_WORK)
	struct blob_db_maint_result res;

	maint_q_start();
	(void)make_work();
	zassert_ok(blob_db_maintain(0, &res));
	zassert_true(res.more, "make_work() must leave work for the helper");

	zassert_ok(blob_db_maint_start(&g_maint_q));
	wait_drained();
	blob_db_maint_stop();
#endif
}

/* Work that appears while the helper is started gets drained too: the
 * library kicks it, the caller does not. Stopped, it leaves work alone.
 *
 * The helper's queue runs at the lowest priority, so it gets the CPU only
 * when this thread sleeps: the work is still there right after it appears. */
ZTEST(blob_db_maint, test_helper_follows_new_work_until_stopped)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_BLOB_DB_MAINT_WORK);
	SKIP_UNLESS_WORK_IS_DEFERRED();
#if defined(CONFIG_BLOB_DB_MAINT_WORK)
	struct blob_db_maint_result res;

	maint_q_start();
	zassert_ok(blob_db_maint_start(&g_maint_q));
	wait_drained();

	/* erase_all leaves fresh buckets on flash_area and released blocks
	 * on UBI, and kicks the helper either way. */
	zassert_ok(blob_db_erase_all());
	zassert_ok(blob_db_maintain(0, &res));
	zassert_true(res.more, "erase_all must leave work");
	wait_drained();

	/* So does a remount. */
	zassert_ok(blob_db_unmount());
	zassert_ok(blob_db_mount());
	wait_drained();

	blob_db_maint_stop();
	zassert_ok(blob_db_erase_all());
	k_msleep(100);
	zassert_ok(blob_db_maintain(0, &res));
	zassert_true(res.more, "a stopped helper must not run");
#endif
}
