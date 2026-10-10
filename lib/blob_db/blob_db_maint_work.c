/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db background maintenance (CONFIG_BLOB_DB_MAINT_WORK): a work item
 * that runs blob_db_maintain() one step at a time on a work queue the
 * application names.
 *
 * Kept out of blob_db.c so a build without the option compiles none of it.
 * Reaches the core only through the public blob_db_maintain(), the library
 * lock (blob_db_internal.h), and blob_db_maint_kick(), which the core and the
 * backends call when work appears (blob_db_store.h).
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>

#include "blob_db_internal.h"
#include "blob_db_store.h"

LOG_MODULE_DECLARE(blob_db, CONFIG_BLOB_DB_LOG_LEVEL);

static void maint_work_handler(struct k_work *work);

static K_WORK_DEFINE(g_maint_work, maint_work_handler);

/* The queue the helper runs on; NULL while stopped. Guarded by the blob_db
 * lock. */
static struct k_work_q *g_maint_q;

void blob_db_maint_kick(void)
{
	if (g_maint_q != NULL) {
		(void)k_work_submit_to_queue(g_maint_q, &g_maint_work);
	}
}

/* One step per run, then back to the end of the queue, so the queue's other
 * items and every blob_db caller wait at most one erase. */
static void maint_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct blob_db_maint_result res;
	const int rc = blob_db_maintain(1, &res);

	if (rc < 0) {
		/* -ENODEV: not mounted; mount kicks again. */
		if (rc != -ENODEV) {
			LOG_WRN("background maintenance: %d", rc);
		}
		return;
	}
	if (res.more) {
		blob_db_lock();
		blob_db_maint_kick();
		blob_db_unlock();
	}
}

int blob_db_maint_start(struct k_work_q *queue)
{
	blob_db_lock();
	g_maint_q = queue != NULL ? queue : &k_sys_work_q;
	/* A run on an unmounted store finds -ENODEV and stops. */
	blob_db_maint_kick();
	blob_db_unlock();
	return 0;
}

void blob_db_maint_stop(void)
{
	struct k_work_sync sync;

	/* Cleared under the lock, so a run that is already past its step
	 * cannot resubmit; then wait for that run outside it, since the run
	 * takes the lock itself. */
	blob_db_lock();
	g_maint_q = NULL;
	blob_db_unlock();
	(void)k_work_cancel_sync(&g_maint_work, &sync);
}
