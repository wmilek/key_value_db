/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Model-container performance benchmark.
 *
 * Measures the L1 sufficiency-proof container (tests/lib/blob_db_contract/
 * src/model_container.c) — a key->value map where every key, every value,
 * and the pair list are separate blobs, the list root is resolved through
 * the root registry (id 1), and every mutation runs the full 5-step
 * crash-safe discipline (stage intent / prepare / commit / cleanup / clear).
 *
 * That discipline is the price being measured: one mc_set() on an existing
 * key costs ~2 blob reads + 4 blob updates + 1 delete, so expect roughly
 * 1/5th of raw blob_db update throughput.
 *
 * Phases (single erase_all + prepare up front; phases share the store):
 *   set       — insert N_KEYS fresh keys
 *   get       — N_GET lookups cycling over the keys (read-only)
 *   overwrite — N_OVW value replacements cycling over the keys
 *   delete    — remove all N_KEYS keys
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
#include <app/lib/rootreg.h>

#include <zephyr/app_version.h>

#include "model_container.h"

LOG_MODULE_REGISTER(app_perf_mc, CONFIG_APP_PERF_MC_LOG_LEVEL);

/* This app is the container's CLIENT — it owns root-id persistence and
 * registers the container's root under its own key. */
#define MC_ROOTREG_KEY  ROOTREG_KEY(0x4d434e54 /* 'MCNT' */, 0)

#define N_KEYS  CONFIG_APP_PERF_MC_N_KEYS
#define N_GET   CONFIG_APP_PERF_MC_N_GET
#define N_OVW   CONFIG_APP_PERF_MC_N_OVW

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

static void key_name(char *buf, size_t sz, int i)
{
	snprintf(buf, sz, "key%02d", i);
}

int main(void)
{
	printk("model-container perf %s  (N_KEYS=%u  N_GET=%u  N_OVW=%u  VAL_LEN=%u)\n",
	       APP_VERSION_STRING, (unsigned)N_KEYS, (unsigned)N_GET,
	       (unsigned)N_OVW, (unsigned)VAL_LEN);

	int rc = blob_db_mount();

	if (rc < 0) {
		LOG_ERR("mount: %d", rc);
		return 0;
	}

	/* Fresh store, then pay all sector erases up front so the timed
	 * phases run on the warm append-only path throughout. */
	rc = blob_db_erase_all();
	if (rc < 0) {
		LOG_ERR("erase_all: %d", rc);
		goto out;
	}

	int64_t t = k_uptime_get();
	int prepared = blob_db_prepare(CONFIG_APP_PERF_MC_PREPARE);

	if (prepared < 0) {
		LOG_ERR("prepare: %d", prepared);
		goto out;
	}
	bench_line("prepare", prepared, k_uptime_delta(&t));

	/* Client-side open: bootstrap the registry, resolve (or register)
	 * the container's root, hand the id to the container. */
	rc = rootreg_init();
	if (rc < 0) {
		LOG_ERR("rootreg_init: %d", rc);
		goto out;
	}

	uint64_t root;

	rc = rootreg_get_or_create(MC_ROOTREG_KEY, &root);
	if (rc < 0) {
		LOG_ERR("get_or_create: %d", rc);
		goto out;
	}
	rc = mc_open(root);
	if (rc < 0) {
		LOG_ERR("mc_open: %d", rc);
		goto out;
	}

	uint8_t val[VAL_LEN];

	for (size_t i = 0; i < sizeof(val); i++) {
		val[i] = (uint8_t)i;
	}

	char key[8];

	/* ============ set (fresh inserts) ============================== */
	t = k_uptime_get();
	for (int i = 0; i < N_KEYS; i++) {
		key_name(key, sizeof(key), i);
		val[0] = (uint8_t)i;
		rc = mc_set(key, val, sizeof(val), MC_STEP_NONE);
		if (rc < 0) {
			LOG_ERR("set %d: %d", i, rc);
			goto out;
		}
	}
	bench_line("set", N_KEYS, k_uptime_delta(&t));

	/* ============ get (lookups, cycling keys) ====================== */
	uint32_t sum = 0;
	uint8_t rbuf[VAL_LEN];

	t = k_uptime_get();
	for (int i = 0; i < N_GET; i++) {
		size_t got;

		key_name(key, sizeof(key), i % N_KEYS);
		rc = mc_get(key, rbuf, sizeof(rbuf), &got);
		if (rc < 0) {
			LOG_ERR("get %d: %d", i, rc);
			goto out;
		}
		sum = sum * 33u + rbuf[0];   /* defeat dead-read elision */
	}
	bench_line("get", N_GET, k_uptime_delta(&t));

	/* ============ overwrite (value replacement) ==================== */
	t = k_uptime_get();
	for (int i = 0; i < N_OVW; i++) {
		key_name(key, sizeof(key), i % N_KEYS);
		val[0] = (uint8_t)(0x80 | i);
		rc = mc_set(key, val, sizeof(val), MC_STEP_NONE);
		if (rc < 0) {
			LOG_ERR("overwrite %d: %d", i, rc);
			goto out;
		}
	}
	bench_line("overwrite", N_OVW, k_uptime_delta(&t));

	/* ============ delete =========================================== */
	t = k_uptime_get();
	for (int i = 0; i < N_KEYS; i++) {
		key_name(key, sizeof(key), i);
		rc = mc_del(key, MC_STEP_NONE);
		if (rc < 0) {
			LOG_ERR("del %d: %d", i, rc);
			goto out;
		}
	}
	bench_line("delete", N_KEYS, k_uptime_delta(&t));

	printk("get checksum: 0x%08x\n", sum);

	size_t entries;

	rc = mc_entry_count(&entries);
	if (rc == 0 && entries != 0) {
		LOG_ERR("expected empty container at end, got %zu", entries);
	}
	printk("live blobs at end: %zu (structural floor: registry + list + intent)\n",
	       blob_db_count());

out:
	blob_db_unmount();
	return 0;
}
