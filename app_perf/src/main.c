/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db performance benchmark.
 *
 * Builds a singly-linked list on top of the blob_db API:
 *   - each node is one blob: { uint64_t next_id; uint8_t val[VAL_LEN]; }
 *   - the head id is stored at BLOB_DB_ROOT_ID (empty root == empty list)
 *
 * Four workloads are measured (each preceded by an erase_all so the
 * numbers are reproducible):
 *   prepend — alloc_id + bind(new, next=head) + update(root=new_id)
 *   append  — alloc_id + bind(new, next=0) + update(prev_tail, next=new_id)
 *             (both cold: each new id lands in a "not yet formatted"
 *             bucket, so the sector erase on 64 KB QSPI NOR dominates)
 *   read    — traverse from head, get() every node
 *   update  — rewrite the payload of every existing node (warm: buckets
 *             are already formatted, so the write path is append-only)
 *
 * Wall-clock is via k_uptime_delta(); ops/sec is derived at millisecond
 * resolution — for flash operations that costs less than 1 % vs a raw
 * cycle counter, and stays sane over runs measured in seconds.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include <app/lib/blob_db.h>

#include <zephyr/app_version.h>

LOG_MODULE_REGISTER(app_perf, CONFIG_APP_PERF_LOG_LEVEL);

#define N_OPS   CONFIG_APP_PERF_N_OPS
#define VAL_LEN CONFIG_APP_PERF_VAL_LEN

struct __packed node {
	uint64_t next_id;
	uint8_t  val[VAL_LEN];
};

BUILD_ASSERT(sizeof(struct node) <= CONFIG_BLOB_DB_MAX_PAYLOAD_LEN,
	     "node exceeds CONFIG_BLOB_DB_MAX_PAYLOAD_LEN");

/* --- list ops on top of blob_db --------------------------------------- */

static int list_head(uint64_t *head_out)
{
	uint64_t head = 0;
	size_t got;
	int rc = blob_db_get(BLOB_DB_ROOT_ID, &head, sizeof(head), &got);

	if (rc < 0) {
		return rc;
	}
	*head_out = (got == sizeof(head)) ? head : 0;
	return 0;
}

static int list_prepend(uint64_t new_id, const uint8_t *val)
{
	uint64_t head;
	int rc = list_head(&head);

	if (rc < 0) {
		return rc;
	}

	struct node n = { .next_id = head };

	memcpy(n.val, val, VAL_LEN);

	rc = blob_db_update(new_id, &n, sizeof(n));
	if (rc < 0) {
		return rc;
	}
	return blob_db_update(BLOB_DB_ROOT_ID, &new_id, sizeof(new_id));
}

/* Append `new_id` after `prev_tail`, or at the head if the list is empty.
 * Two flash writes per call (same as prepend); the caller keeps track of
 * the current tail to avoid an O(n) walk on every append. */
static int list_append(uint64_t new_id, uint64_t prev_tail, const uint8_t *val)
{
	struct node n = { .next_id = 0 };

	memcpy(n.val, val, VAL_LEN);

	int rc = blob_db_update(new_id, &n, sizeof(n));

	if (rc < 0) {
		return rc;
	}

	if (prev_tail == 0) {
		/* Empty list — new node becomes the head. */
		return blob_db_update(BLOB_DB_ROOT_ID, &new_id, sizeof(new_id));
	}

	/* Splice new_id in after prev_tail. */
	struct node prev;
	size_t got;

	rc = blob_db_get(prev_tail, &prev, sizeof(prev), &got);
	if (rc < 0) {
		return rc;
	}
	prev.next_id = new_id;
	return blob_db_update(prev_tail, &prev, sizeof(prev));
}

/* Returns number of nodes visited (0 on empty list, negative on error). */
static int list_traverse(uint32_t *checksum_out)
{
	uint64_t cur;
	int rc = list_head(&cur);

	if (rc < 0) {
		return rc;
	}

	uint32_t sum = 0;
	int n = 0;

	while (cur != 0) {
		struct node node;
		size_t got;

		rc = blob_db_get(cur, &node, sizeof(node), &got);
		if (rc < 0) {
			return rc;
		}
		if (got != sizeof(node)) {
			LOG_ERR("bad node at id=%llu got=%zu", cur, got);
			return -EIO;
		}
		/* Fold the payload into a checksum so the compiler cannot
		 * elide the reads. */
		for (size_t i = 0; i < VAL_LEN; i++) {
			sum = sum * 33u + node.val[i];
		}
		n++;
		cur = node.next_id;
	}
	if (checksum_out) {
		*checksum_out = sum;
	}
	return n;
}

/* --- benchmark harness ------------------------------------------------- */

static void bench_line(const char *what, int ops, int64_t ms)
{
	/* Print ops/s with three fractional digits so sub-1-op/s workloads
	 * (e.g. a cold add pass on QSPI) still show a meaningful number. */
	uint64_t milli_ops_per_s =
		(ms > 0) ? (uint64_t)ops * 1000000ULL / (uint64_t)ms : 0;
	uint64_t us_per_op =
		(ops > 0) ? (uint64_t)ms * 1000ULL / (uint64_t)ops : 0;

	printk("bench %-6s : %4d ops in %7" PRId64 " ms  -> "
	       "%4u.%03u ops/s  (%9llu us/op)\n",
	       what, ops, ms,
	       (unsigned)(milli_ops_per_s / 1000),
	       (unsigned)(milli_ops_per_s % 1000),
	       (unsigned long long)us_per_op);
}

/* Run every workload from an empty list (erase_all) so timings are
 * apples-to-apples across phases. Returns 0 on success, negative errno on
 * blob_db failures. */
static int bench_phase_read_and_update(uint8_t *val, uint32_t *checksum)
{
	int64_t t = k_uptime_get();
	int visited = list_traverse(checksum);
	int64_t t_read = k_uptime_delta(&t);

	if (visited < 0) {
		return visited;
	}
	if (visited != N_OPS) {
		LOG_ERR("traverse saw %d, expected %u", visited,
			(unsigned)N_OPS);
	}
	bench_line("read", visited, t_read);

	/* Warm update: walk again and rewrite every node in place. The
	 * bucket is already formatted, so no sector erase runs. */
	uint64_t cur;
	int rc = list_head(&cur);

	if (rc < 0 || cur == 0) {
		return rc;
	}

	t = k_uptime_get();
	int updated = 0;

	while (cur != 0) {
		struct node node;
		size_t got;

		rc = blob_db_get(cur, &node, sizeof(node), &got);
		if (rc < 0) {
			return rc;
		}
		node.val[0]++;
		rc = blob_db_update(cur, &node, sizeof(node));
		if (rc < 0) {
			return rc;
		}
		updated++;
		cur = node.next_id;
	}
	int64_t t_upd = k_uptime_delta(&t);

	bench_line("update", updated, t_upd);

	(void)val;
	return 0;
}

int main(void)
{
	printk("blob_db perf %s  (N_OPS=%u  VAL_LEN=%u  node=%u B)\n",
	       APP_VERSION_STRING, (unsigned)N_OPS, (unsigned)VAL_LEN,
	       (unsigned)sizeof(struct node));

	int rc = blob_db_mount();

	if (rc < 0) {
		LOG_ERR("mount: %d", rc);
		return 0;
	}

	uint8_t val[VAL_LEN];

	for (size_t i = 0; i < VAL_LEN; i++) {
		val[i] = (uint8_t)i;
	}

	/* ============ prepend phase ==================================== */
	rc = blob_db_erase_all();
	if (rc < 0) {
		LOG_ERR("erase_all: %d", rc);
		goto out;
	}

	int64_t t0 = k_uptime_get();

	for (int i = 0; i < N_OPS; i++) {
		uint64_t id = blob_db_alloc_id();

		if (id == 0) {
			LOG_ERR("alloc_id returned 0 at i=%d", i);
			goto out;
		}
		val[0] = (uint8_t)i;
		rc = list_prepend(id, val);
		if (rc < 0) {
			LOG_ERR("prepend %d: %d", i, rc);
			goto out;
		}
	}
	bench_line("prepend", N_OPS, k_uptime_delta(&t0));

	uint32_t sum = 0;

	rc = bench_phase_read_and_update(val, &sum);
	if (rc < 0) {
		goto out;
	}
	printk("prepend checksum: 0x%08x\n", sum);

	/* ============ append phase ===================================== */
	rc = blob_db_erase_all();
	if (rc < 0) {
		LOG_ERR("erase_all: %d", rc);
		goto out;
	}

	int64_t t1 = k_uptime_get();
	uint64_t tail = 0;   /* caller-side tail cache; O(1) append */

	for (int i = 0; i < N_OPS; i++) {
		uint64_t id = blob_db_alloc_id();

		if (id == 0) {
			LOG_ERR("alloc_id returned 0 at i=%d", i);
			goto out;
		}
		val[0] = (uint8_t)i;
		rc = list_append(id, tail, val);
		if (rc < 0) {
			LOG_ERR("append %d: %d", i, rc);
			goto out;
		}
		tail = id;
	}
	bench_line("append", N_OPS, k_uptime_delta(&t1));

	sum = 0;
	rc = bench_phase_read_and_update(val, &sum);
	if (rc < 0) {
		goto out;
	}
	printk("append checksum:  0x%08x\n", sum);

out:
	blob_db_unmount();
	return 0;
}
