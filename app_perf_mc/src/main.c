/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Model-container performance benchmark.
 *
 * Measures the two L1 sufficiency-proof containers in
 * tests/lib/blob_db_contract/src/, one after the other on the same store.
 * Both keep every value in its own blob, have their root resolved through
 * the root registry (id 1) by this app, and run every structural mutation
 * through an intent-logged crash-safe discipline. That discipline is the
 * price being measured.
 *
 * The map (model_container_map.c): key -> value, every key, every value and
 * the pair list separate blobs; 5 steps (stage / prepare / commit / cleanup
 * / clear). One mc_set() on an existing key costs ~2 blob reads + 4 blob
 * updates + 1 delete, so expect roughly 1/5th of raw blob_db update
 * throughput. Lookup is an O(n) key scan.
 *
 *   set       — insert N_KEYS fresh keys
 *   get       — N_GET lookups cycling over the keys (read-only)
 *   overwrite — N_OVW value replacements cycling over the keys
 *   delete    — remove all N_KEYS keys
 *   cleanup   — full teardown: mc_destroy() + rootreg_unregister(), leaving
 *               the store at the absolute floor (the empty registry alone)
 *
 * The list (model_container_list.c): a doubly linked list addressed by node
 * handle; 6 steps (stage / prepare / commit / repair / cleanup / clear).
 * Every access is O(1) in the list length — the handle is the node id — so
 * against the map this isolates what the discipline costs from what the
 * map's key scan costs.
 *
 *   l.insert  — append N_NODES nodes at the back (mcl_insert_before(0))
 *   l.get     — N_GET value reads by handle, cycling over the nodes
 *   l.set     — N_OVW in-place value overwrites by handle (no intent)
 *   l.next    — N_GET forward steps (mcl_next), wrapping through the root;
 *               the authoritative chain, never recovers, never writes
 *   l.prev    — the same backwards (mcl_prev), which checks for a staged
 *               intent first
 *   l.remove  — remove all N_NODES nodes from the front
 *   l.cleanup — mcl_destroy() + rootreg_unregister(), back to the floor
 *
 * With CONFIG_BLOB_DB_IOSTATS every bench line is followed by an `io` line
 * with the phase's flash operations, in the shape
 * app_perf_l0/tools/l0_timing.py parses. On native_sim, where the flash
 * simulator models no latency, those counters are the only figure that
 * means anything.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_iostats.h>
#include <app/lib/rootreg.h>

#include <zephyr/app_version.h>

#include "model_container_list.h"
#include "model_container_map.h"

#ifdef CONFIG_ARCH_POSIX
#include "posix_board_if.h"   /* posix_exit() — end the sim run cleanly */
#endif

LOG_MODULE_REGISTER(app_perf_mc, CONFIG_APP_PERF_MC_LOG_LEVEL);

/* This app is the containers' CLIENT — it owns root-id persistence and
 * registers each container's root under its own key. */
#define MC_ROOTREG_KEY   ROOTREG_KEY(0x4d435f4d /* 'MC_M': model container, map */, 0)
#define MCL_ROOTREG_KEY  ROOTREG_KEY(0x4d435f4c /* 'MC_L': model container, list */, 0)

#define N_KEYS   CONFIG_APP_PERF_MC_N_KEYS
#define N_NODES  CONFIG_APP_PERF_MC_N_NODES
#define N_GET    CONFIG_APP_PERF_MC_N_GET
#define N_OVW    CONFIG_APP_PERF_MC_N_OVW

#define VAL_LEN 24

static void bench_line(const char *what, int ops, int64_t ms)
{
	uint64_t milli_ops_per_s =
		(ms > 0) ? (uint64_t)ops * 1000000ULL / (uint64_t)ms : 0;
	uint64_t us_per_op =
		(ops > 0) ? (uint64_t)ms * 1000ULL / (uint64_t)ops : 0;

	printk("bench %-9s: %4d ops in %7" PRId64 " ms  -> "
	       "%4u.%03u ops/s  (%9llu us/op)\n",
	       what, ops, ms,
	       (unsigned)(milli_ops_per_s / 1000),
	       (unsigned)(milli_ops_per_s % 1000),
	       (unsigned long long)us_per_op);
}

#if defined(CONFIG_BLOB_DB_IOSTATS)
static struct blob_db_iostats io_mark;

static void io_reset(void)
{
	blob_db_iostats_get(&io_mark);
}

/* The flash the phase just timed actually touched. Same shape as
 * app_perf_blob_db's and app_perf_kvdb's line, so
 * app_perf_l0/tools/l0_timing.py can turn a native_sim capture into
 * predicted hardware time. */
static void io_line(const char *what)
{
	struct blob_db_iostats now;

	blob_db_iostats_get(&now);
	printk("   io %-9s: rd %6u ops/%8llu B   wr %5u ops/%8llu B   "
	       "er %4u ops/%8llu B\n",
	       what, now.reads - io_mark.reads,
	       (unsigned long long)(now.bytes_read - io_mark.bytes_read),
	       now.writes - io_mark.writes,
	       (unsigned long long)(now.bytes_written - io_mark.bytes_written),
	       now.erases - io_mark.erases,
	       (unsigned long long)(now.bytes_erased - io_mark.bytes_erased));
}
#else
#define io_reset()     ((void)0)
#define io_line(what)  ((void)0)
#endif

/* Start timing a phase. */
static int64_t phase_begin(void)
{
	io_reset();
	return k_uptime_get();
}

/* Stop timing it: the bench line, then (with IOSTATS) its io line. The
 * clock is read first so printing the counters is not timed. */
static void phase_end(const char *what, int ops, int64_t *t)
{
	int64_t ms = k_uptime_delta(t);

	bench_line(what, ops, ms);
	io_line(what);
}

static void key_name(char *buf, size_t sz, int i)
{
	snprintf(buf, sz, "key%02d", i);
}

/* Client-side open: resolve (or register) a container's root id. */
static int root_for(uint64_t key, uint64_t *root)
{
	int rc = rootreg_get_or_create(key, root);

	if (rc < 0) {
		LOG_ERR("get_or_create: %d", rc);
	}
	return rc;
}

/* ===================================================================== */
/* The map                                                               */
/* ===================================================================== */

static int bench_map(uint32_t *sum_out)
{
	uint64_t root;
	int rc = root_for(MC_ROOTREG_KEY, &root);

	if (rc < 0) {
		return rc;
	}
	rc = mc_open(root);
	if (rc < 0) {
		LOG_ERR("mc_open: %d", rc);
		return rc;
	}

	uint8_t val[VAL_LEN];

	for (size_t i = 0; i < sizeof(val); i++) {
		val[i] = (uint8_t)i;
	}

	char key[8];
	int64_t t;

	/* ============ set (fresh inserts) ============================== */
	t = phase_begin();
	for (int i = 0; i < N_KEYS; i++) {
		key_name(key, sizeof(key), i);
		val[0] = (uint8_t)i;
		rc = mc_set(key, val, sizeof(val), MC_STEP_NONE);
		if (rc < 0) {
			LOG_ERR("set %d: %d", i, rc);
			return rc;
		}
	}
	phase_end("set", N_KEYS, &t);

	/* ============ get (lookups, cycling keys) ====================== */
	uint32_t sum = 0;
	uint8_t rbuf[VAL_LEN];

	t = phase_begin();
	for (int i = 0; i < N_GET; i++) {
		size_t got;

		key_name(key, sizeof(key), i % N_KEYS);
		rc = mc_get(key, rbuf, sizeof(rbuf), &got);
		if (rc < 0) {
			LOG_ERR("get %d: %d", i, rc);
			return rc;
		}
		sum = sum * 33u + rbuf[0];   /* defeat dead-read elision */
	}
	phase_end("get", N_GET, &t);

	/* ============ overwrite (value replacement) ==================== */
	t = phase_begin();
	for (int i = 0; i < N_OVW; i++) {
		key_name(key, sizeof(key), i % N_KEYS);
		val[0] = (uint8_t)(0x80 | i);
		rc = mc_set(key, val, sizeof(val), MC_STEP_NONE);
		if (rc < 0) {
			LOG_ERR("overwrite %d: %d", i, rc);
			return rc;
		}
	}
	phase_end("overwrite", N_OVW, &t);

	/* ============ delete =========================================== */
	t = phase_begin();
	for (int i = 0; i < N_KEYS; i++) {
		key_name(key, sizeof(key), i);
		rc = mc_del(key, MC_STEP_NONE);
		if (rc < 0) {
			LOG_ERR("del %d: %d", i, rc);
			return rc;
		}
	}
	phase_end("delete", N_KEYS, &t);

	size_t entries;

	rc = mc_entry_count(&entries);
	if (rc == 0 && entries != 0) {
		LOG_ERR("expected empty container after delete, got %zu",
			entries);
		return -EIO;
	}

	/* ============ cleanup (unset everything) ======================= */
	t = phase_begin();
	rc = mc_destroy();   /* unsets all keys + structure blobs */
	if (rc < 0) {
		LOG_ERR("mc_destroy: %d", rc);
		return rc;
	}
	rc = rootreg_unregister(MC_ROOTREG_KEY);
	if (rc < 0) {
		LOG_ERR("unregister: %d", rc);
		return rc;
	}
	phase_end("cleanup", 1, &t);

	*sum_out = sum;
	return 0;
}

/* ===================================================================== */
/* The list                                                              */
/* ===================================================================== */

static uint64_t node_ids[N_NODES];

static int bench_list(uint32_t *sum_out)
{
	uint64_t root;
	int rc = root_for(MCL_ROOTREG_KEY, &root);

	if (rc < 0) {
		return rc;
	}
	rc = mcl_open(root);
	if (rc < 0) {
		LOG_ERR("mcl_open: %d", rc);
		return rc;
	}

	uint8_t val[VAL_LEN];

	for (size_t i = 0; i < sizeof(val); i++) {
		val[i] = (uint8_t)i;
	}

	int64_t t;

	/* ============ l.insert (append at the back) ==================== */
	t = phase_begin();
	for (int i = 0; i < N_NODES; i++) {
		val[0] = (uint8_t)i;
		rc = mcl_insert_before(0, val, sizeof(val), &node_ids[i],
				       MCL_STEP_NONE);
		if (rc < 0) {
			LOG_ERR("l.insert %d: %d", i, rc);
			return rc;
		}
	}
	phase_end("l.insert", N_NODES, &t);

	size_t count;

	rc = mcl_check(&count);
	if (rc < 0 || count != N_NODES) {
		LOG_ERR("mcl_check after insert: %d, %zu nodes (expect %u)",
			rc, count, (unsigned)N_NODES);
		return rc < 0 ? rc : -EIO;
	}

	/* ============ l.get (value reads by handle) ==================== */
	uint32_t sum = 0;
	uint8_t rbuf[VAL_LEN];

	t = phase_begin();
	for (int i = 0; i < N_GET; i++) {
		size_t got;

		rc = mcl_get(node_ids[i % N_NODES], rbuf, sizeof(rbuf), &got);
		if (rc < 0) {
			LOG_ERR("l.get %d: %d", i, rc);
			return rc;
		}
		sum = sum * 33u + rbuf[0];   /* defeat dead-read elision */
	}
	phase_end("l.get", N_GET, &t);

	/* ============ l.set (in-place value overwrite) ================= */
	t = phase_begin();
	for (int i = 0; i < N_OVW; i++) {
		val[0] = (uint8_t)(0x80 | i);
		rc = mcl_set(node_ids[i % N_NODES], val, sizeof(val));
		if (rc < 0) {
			LOG_ERR("l.set %d: %d", i, rc);
			return rc;
		}
	}
	phase_end("l.set", N_OVW, &t);

	/* ============ l.next / l.prev (walks) ========================== */
	/* N_GET single steps, wrapping through the root (0) at the ends, so
	 * the root's own read is in the count once per lap. The order is
	 * checked against the handles as it goes: a compare per step, noise
	 * next to a flash read. */
	uint64_t cur = 0;
	int pos = -1;   /* index of `cur` in node_ids, -1 for the root */

	t = phase_begin();
	for (int i = 0; i < N_GET; i++) {
		rc = mcl_next(cur, &cur);
		if (rc < 0) {
			LOG_ERR("l.next %d: %d", i, rc);
			return rc;
		}
		pos = (pos + 1 == N_NODES) ? -1 : pos + 1;
		if (cur != (pos < 0 ? 0 : node_ids[pos])) {
			LOG_ERR("l.next %d: out of order", i);
			return -EIO;
		}
	}
	phase_end("l.next", N_GET, &t);

	cur = 0;
	pos = N_NODES;
	t = phase_begin();
	for (int i = 0; i < N_GET; i++) {
		rc = mcl_prev(cur, &cur);
		if (rc < 0) {
			LOG_ERR("l.prev %d: %d", i, rc);
			return rc;
		}
		pos = (pos == 0) ? N_NODES : pos - 1;
		if (cur != (pos == N_NODES ? 0 : node_ids[pos])) {
			LOG_ERR("l.prev %d: out of order", i);
			return -EIO;
		}
	}
	phase_end("l.prev", N_GET, &t);

	/* ============ l.remove (from the front) ======================== */
	t = phase_begin();
	for (int i = 0; i < N_NODES; i++) {
		rc = mcl_remove(node_ids[i], MCL_STEP_NONE);
		if (rc < 0) {
			LOG_ERR("l.remove %d: %d", i, rc);
			return rc;
		}
	}
	phase_end("l.remove", N_NODES, &t);

	rc = mcl_check(&count);
	if (rc < 0 || count != 0) {
		LOG_ERR("mcl_check after remove: %d, %zu nodes (expect 0)",
			rc, count);
		return rc < 0 ? rc : -EIO;
	}

	/* ============ l.cleanup ======================================== */
	t = phase_begin();
	rc = mcl_destroy();
	if (rc < 0) {
		LOG_ERR("mcl_destroy: %d", rc);
		return rc;
	}
	rc = rootreg_unregister(MCL_ROOTREG_KEY);
	if (rc < 0) {
		LOG_ERR("unregister: %d", rc);
		return rc;
	}
	phase_end("l.cleanup", 1, &t);

	*sum_out = sum;
	return 0;
}

int main(void)
{
	printk("model-container perf %s  (N_KEYS=%u  N_NODES=%u  N_GET=%u  "
	       "N_OVW=%u  VAL_LEN=%u)\n",
	       APP_VERSION_STRING, (unsigned)N_KEYS, (unsigned)N_NODES,
	       (unsigned)N_GET, (unsigned)N_OVW, (unsigned)VAL_LEN);

	int rc = blob_db_mount();

	if (rc < 0) {
		LOG_ERR("mount: %d", rc);
		goto out;
	}

	/* Fresh store, then pay all sector erases up front so the timed
	 * phases run on the warm append-only path throughout. */
	rc = blob_db_erase_all();
	if (rc < 0) {
		LOG_ERR("erase_all: %d", rc);
		goto out;
	}

	int64_t t = phase_begin();
	int prepared = blob_db_prepare(CONFIG_APP_PERF_MC_PREPARE);

	if (prepared < 0) {
		rc = prepared;
		LOG_ERR("prepare: %d", prepared);
		goto out;
	}
	phase_end("prepare", prepared, &t);

	rc = rootreg_init();
	if (rc < 0) {
		LOG_ERR("rootreg_init: %d", rc);
		goto out;
	}

	/* The map first, so its phases run on exactly the store state every
	 * earlier capture of this app measured. */
	uint32_t map_sum, list_sum;

	rc = bench_map(&map_sum);
	if (rc < 0) {
		goto out;
	}
	printk("get checksum: 0x%08x\n", map_sum);
	printk("live blobs after map: %zu (expect 1 — the empty registry alone)\n",
	       blob_db_count());

	rc = bench_list(&list_sum);
	if (rc < 0) {
		goto out;
	}
	printk("l.get checksum: 0x%08x\n", list_sum);
	printk("live blobs at end: %zu (expect 1 — the empty registry alone)\n",
	       blob_db_count());

out:
	blob_db_unmount();
#ifdef CONFIG_ARCH_POSIX
	posix_exit(rc == 0 ? 0 : 1);
#endif
	return 0;
}
