/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mount-time geometry check for the inline payload cap (l1_bucketlog.md §13.3)
 * on flash whose write unit is 16 B. Each testcase.yaml scenario picks one
 * MAX_PAYLOAD_LEN; the suite asserts the outcome the geometry implies.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <app/lib/blob_db.h>

/* boards/native_sim.overlay: 4 KB sectors, write-block-size 16. */
#define SECTOR_SZ        4096
#define WRITE_ALIGN      16
#define BUCKET_DATA_OFF  16
#define SLOT_OVERHEAD    14
#define SUSTAINABLE \
	((((SECTOR_SZ - BUCKET_DATA_OFF) / 2) & ~(WRITE_ALIGN - 1)) - SLOT_OVERHEAD)

BUILD_ASSERT(SUSTAINABLE == 2018, "geometry arithmetic drift");

static uint8_t payload[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];
static uint8_t readback[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];

ZTEST(blob_db_geometry, test_payload_cap_matches_aligned_slot_size)
{
	int rc = blob_db_mount();

	if (CONFIG_BLOB_DB_MAX_PAYLOAD_LEN > SUSTAINABLE) {
		zassert_equal(rc, -ENOTSUP,
			      "mount accepted MAX_PAYLOAD_LEN=%d, whose %d-aligned "
			      "slot cannot be rebound in a %d B sector (rc %d)",
			      CONFIG_BLOB_DB_MAX_PAYLOAD_LEN, WRITE_ALIGN,
			      SECTOR_SZ, rc);
		return;
	}

	zassert_ok(rc, "mount refused MAX_PAYLOAD_LEN=%d, which fits",
		   CONFIG_BLOB_DB_MAX_PAYLOAD_LEN);

	/* An accepted cap must stay rebindable: a lone blob of exactly that
	 * size can be rewritten indefinitely, each rewrite compacting away the
	 * previous copy. */
	const uint64_t id = blob_db_alloc_id();

	zassert_not_equal(id, 0);
	for (int i = 0; i < 6; i++) {
		memset(payload, 'a' + i, sizeof(payload));
		zassert_ok(blob_db_update(id, payload, sizeof(payload)),
			   "rebind %d of a max-size blob failed", i);
	}

	size_t got;

	zassert_ok(blob_db_get(id, readback, sizeof(readback), &got));
	zassert_equal(got, sizeof(payload));
	zassert_mem_equal(readback, payload, sizeof(payload));
	zassert_ok(blob_db_unmount());
}

ZTEST_SUITE(blob_db_geometry, NULL, NULL, NULL, NULL, NULL);
