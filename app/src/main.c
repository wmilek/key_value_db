/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>

#include <app_version.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

static void smoke(void)
{
	uint64_t id_hello = 0, id_world = 0, id_big = 0;
	char buf[64];
	size_t got;
	int rc;

	rc = blob_db_put("hello", 5, &id_hello);
	LOG_INF("put 'hello' -> id=%llu rc=%d", (unsigned long long)id_hello, rc);

	rc = blob_db_put("world", 5, &id_world);
	LOG_INF("put 'world' -> id=%llu rc=%d", (unsigned long long)id_world, rc);

	const char big[] = "the quick brown fox jumps over the lazy dog";
	rc = blob_db_put(big, strlen(big), &id_big);
	LOG_INF("put '<43B>' -> id=%llu rc=%d", (unsigned long long)id_big, rc);

	rc = blob_db_get(id_hello, buf, sizeof(buf), &got);
	buf[got < sizeof(buf) ? got : sizeof(buf) - 1] = 0;
	LOG_INF("get %llu -> rc=%d '%s' (%zu)",
		(unsigned long long)id_hello, rc, buf, got);

	rc = blob_db_update(id_hello, "HELLO!", 6);
	LOG_INF("update %llu rc=%d", (unsigned long long)id_hello, rc);

	rc = blob_db_get(id_hello, buf, sizeof(buf), &got);
	buf[got < sizeof(buf) ? got : sizeof(buf) - 1] = 0;
	LOG_INF("get %llu (post-update) -> rc=%d '%s' (%zu)",
		(unsigned long long)id_hello, rc, buf, got);

	rc = blob_db_delete(id_world);
	LOG_INF("delete %llu rc=%d", (unsigned long long)id_world, rc);

	rc = blob_db_get(id_world, buf, sizeof(buf), &got);
	LOG_INF("get %llu (post-delete) -> rc=%d", (unsigned long long)id_world, rc);

	rc = blob_db_delete(id_world);
	LOG_INF("delete %llu (already deleted) -> rc=%d",
		(unsigned long long)id_world, rc);

	LOG_INF("exists %llu = %s", (unsigned long long)id_big,
		blob_db_exists(id_big) ? "true" : "false");
	LOG_INF("exists 9999 = %s",
		blob_db_exists(9999) ? "true" : "false");
}

static int dump_cb(uint64_t id, const void *payload, size_t len, void *user)
{
	ARG_UNUSED(payload);
	ARG_UNUSED(user);
	LOG_INF("  id=%llu len=%zu", (unsigned long long)id, len);
	return 0;
}

/* Pound id_hello with updates to force at least one bucket compaction
 * (slot size ~17 B at 3-byte payloads → ~240 fit in a 4 KB bucket). */
static void compact_test(uint64_t id, int iters)
{
	char buf[8];
	size_t got;

	LOG_INF("compact_test: %d updates to id=%llu",
		iters, (unsigned long long)id);

	for (int i = 0; i < iters; i++) {
		char val[8];
		int n = snprintk(val, sizeof(val), "v%04d", i);
		int rc = blob_db_update(id, val, n);
		if (rc < 0) {
			LOG_ERR("update #%d failed: %d", i, rc);
			return;
		}
	}

	int rc = blob_db_get(id, buf, sizeof(buf), &got);
	buf[got < sizeof(buf) ? got : sizeof(buf) - 1] = 0;
	LOG_INF("after %d updates, get %llu -> '%s' rc=%d",
		iters, (unsigned long long)id, buf, rc);
}

int main(void)
{
	printk("Zephyr Key-Value DB %s\n", APP_VERSION_STRING);

	int rc = blob_db_mount();
	if (rc < 0) {
		LOG_ERR("blob_db_mount failed: %d", rc);
		return 0;
	}
	LOG_INF("blob_db mounted");

	smoke();
	compact_test(1, 500);

	LOG_INF("count = %zu", blob_db_count());
	LOG_INF("iterate:");
	blob_db_iterate(dump_cb, NULL);

	LOG_INF("format -> rc=%d", blob_db_format());
	LOG_INF("count after format = %zu", blob_db_count());

	blob_db_unmount();
	LOG_INF("blob_db unmounted; bye");
	return 0;
}
