/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the UBI backend adds on top of the store seam: refusing a substrate it
 * cannot read, discarding it only through blob_db_format(), and the two
 * application hooks of <app/lib/blob_db_ubi.h>.
 *
 * The hook cases need the hooks defined here, so they run only in the
 * lib.blob_db.ubi.app_key scenario (CONFIG_BLOB_DB_UBI_KEY_APP); the
 * lib.blob_db.ubi scenario keeps the shipped defaults — the built-in key and
 * the weak trust-everything state check.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include <psa/crypto.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_ubi.h>

#define PARTITION_ID_UNDER_TEST PARTITION_ID(storage_partition)

#if defined(CONFIG_BLOB_DB_UBI_KEY_APP)

/* Which key blob_db_ubi_ikm_key() hands out, and whether the state check
 * trusts the device. Reset after every case. */
static unsigned int g_key_variant;
static bool g_untrusted;
static psa_key_id_t g_keys[2];

int blob_db_ubi_ikm_key(psa_key_id_t *key_id)
{
	if (g_keys[g_key_variant] == PSA_KEY_ID_NULL) {
		uint8_t ikm[32];
		psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

		memset(ikm, 0xa5 + g_key_variant, sizeof(ikm));
		psa_set_key_type(&attr, PSA_KEY_TYPE_DERIVE);
		psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
		psa_set_key_algorithm(&attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
		psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);

		if (psa_import_key(&attr, ikm, sizeof(ikm),
				   &g_keys[g_key_variant]) != PSA_SUCCESS) {
			return -EIO;
		}
	}

	*key_id = g_keys[g_key_variant];
	return 0;
}

enum ubi_state_verdict blob_db_ubi_state_check(const struct ubi_device_info *info)
{
	ARG_UNUSED(info);

	return g_untrusted ? UBI_STATE_UNTRUSTED : UBI_STATE_TRUSTED;
}

#endif /* CONFIG_BLOB_DB_UBI_KEY_APP */

static void ubi_before(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
	zassert_ok(blob_db_mount(), "mount failed in setup");
	zassert_ok(blob_db_format(), "format failed in setup");
}

static void ubi_after(void *fixture)
{
	ARG_UNUSED(fixture);
	blob_db_unmount();
#if defined(CONFIG_BLOB_DB_UBI_KEY_APP)
	g_key_variant = 0;
	g_untrusted = false;
#endif
}

ZTEST_SUITE(blob_db_ubi, NULL, NULL, ubi_before, ubi_after, NULL);

/* A partition that is neither blank nor a UBI device this release reads — the
 * previous UBI backend's format, or a flash_area store — is refused with
 * -ENOTSUP and left byte-identical. Only blob_db_format() replaces it. */
ZTEST(blob_db_ubi, test_foreign_substrate_is_refused_until_formatted)
{
	/* The first bytes of a PEB written by the previous UBI backend: its
	 * little-endian device header magic. */
	static const uint8_t foreign[16] = { 0x25, 0x49, 0x42, 0x55, 1, 2, 3, 4,
					     5, 6, 7, 8, 9, 10, 11, 12 };
	const struct flash_area *fa;
	uint8_t back[sizeof(foreign)];

	blob_db_unmount();
	zassert_ok(flash_area_open(PARTITION_ID_UNDER_TEST, &fa));
	zassert_ok(flash_area_erase(fa, 0, fa->fa_size));
	zassert_ok(flash_area_write(fa, 0, foreign, sizeof(foreign)));

	zassert_equal(blob_db_mount(), -ENOTSUP,
		      "a foreign substrate must be refused, not formatted");
	zassert_ok(flash_area_read(fa, 0, back, sizeof(back)));
	zassert_mem_equal(back, foreign, sizeof(foreign),
			  "a refused mount must leave the partition untouched");

	/* Format leaves the store it built mounted. */
	zassert_ok(blob_db_format(), "format must discard what mount refuses");
	zassert_equal(blob_db_count(), 1, "want a fresh store holding only root");

	blob_db_unmount();
	zassert_ok(blob_db_mount(), "the discarded partition must now mount");

	flash_area_close(fa);
}

/* A blank partition is the one -ENODEV that mount formats on its own. */
ZTEST(blob_db_ubi, test_blank_partition_is_formatted_by_mount)
{
	const struct flash_area *fa;

	blob_db_unmount();
	zassert_ok(flash_area_open(PARTITION_ID_UNDER_TEST, &fa));
	zassert_ok(flash_area_erase(fa, 0, fa->fa_size));
	flash_area_close(fa);

	zassert_ok(blob_db_mount());
	zassert_equal(blob_db_count(), 1, "want a fresh store holding only root");
}

/* Other keying material makes the store unreadable — and mount must not
 * mistake that for a blank partition and format over it. */
ZTEST(blob_db_ubi, test_wrong_key_is_refused_and_store_survives)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_BLOB_DB_UBI_KEY_APP);
#if defined(CONFIG_BLOB_DB_UBI_KEY_APP)
	const uint8_t pl[] = "survives a wrong key";
	uint8_t buf[sizeof(pl)];
	size_t got;
	const uint64_t id = blob_db_alloc_id();

	zassert_ok(blob_db_update(id, pl, sizeof(pl)));
	blob_db_unmount();

	g_key_variant = 1;
	zassert_equal(blob_db_mount(), -EBADMSG,
		      "metadata sealed under another key must not verify");

	g_key_variant = 0;
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_equal(got, sizeof(pl));
	zassert_mem_equal(buf, pl, sizeof(pl));
#endif
}

/* The state check is the application's rollback detector: a refusal at
 * attach fails the mount, and the device mounts again once it is trusted. */
ZTEST(blob_db_ubi, test_untrusted_state_refuses_mount)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_BLOB_DB_UBI_KEY_APP);
#if defined(CONFIG_BLOB_DB_UBI_KEY_APP)
	blob_db_unmount();

	g_untrusted = true;
	zassert_equal(blob_db_mount(), -EROFS);

	g_untrusted = false;
	zassert_ok(blob_db_mount());
#endif
}

/* Trust withdrawn at run time stops writes within
 * CONFIG_UBI_STATE_CHECK_INTERVAL flash writes, while what was written
 * before stays readable after a remount. */
ZTEST(blob_db_ubi, test_trust_withdrawn_at_run_time_stops_writes)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_BLOB_DB_UBI_KEY_APP);
#if defined(CONFIG_BLOB_DB_UBI_KEY_APP)
	const uint8_t pl[] = "written while trusted";
	uint8_t buf[sizeof(pl)];
	size_t got;
	const uint64_t id = blob_db_alloc_id();
	int rc = 0;

	zassert_ok(blob_db_update(id, pl, sizeof(pl)));

	g_untrusted = true;
	for (int i = 0; i < 4 * CONFIG_UBI_STATE_CHECK_INTERVAL && rc == 0; i++) {
		rc = blob_db_update(blob_db_alloc_id(), pl, sizeof(pl));
	}
	zassert_equal(rc, -EROFS, "writes must stop once trust is withdrawn");

	blob_db_unmount();
	g_untrusted = false;
	zassert_ok(blob_db_mount());
	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_mem_equal(buf, pl, sizeof(pl));
#endif
}

/* Erase-block size of the simulated partition (boards/native_sim.overlay). */
#define RAW_PEB_SZ 4096

/* Find the newest copy of a blob_db master anywhere on the raw partition, and
 * whether the copy two generations older — the one that master's slot held
 * before — is still on flash too. Masters are found by their magic; their
 * generation follows the 12-byte compatibility prefix. */
static off_t newest_master_copy(bool *older_copy_present)
{
	static uint8_t peb[RAW_PEB_SZ];
	const struct flash_area *fa;
	uint32_t newest_gen = 0;
	off_t newest_off = -1;
	uint32_t gens_seen[8];
	size_t n_seen = 0;

	zassert_ok(flash_area_open(PARTITION_ID_UNDER_TEST, &fa));
	for (off_t base = 0; base < (off_t)fa->fa_size; base += RAW_PEB_SZ) {
		zassert_ok(flash_area_read(fa, base, peb, sizeof(peb)));
		for (size_t at = 0; at + 16 <= sizeof(peb); at += 4) {
			if (memcmp(&peb[at], "BDMS", 4) != 0) {
				continue;
			}
			uint32_t gen;

			memcpy(&gen, &peb[at + 12], sizeof(gen));
			if (n_seen < ARRAY_SIZE(gens_seen)) {
				gens_seen[n_seen++] = gen;
			}
			if (gen > newest_gen) {
				newest_gen = gen;
				newest_off = base + (off_t)at;
			}
		}
	}
	flash_area_close(fa);

	*older_copy_present = false;
	for (size_t i = 0; i < n_seen; i++) {
		if (newest_gen >= 2 && gens_seen[i] == newest_gen - 2) {
			*older_copy_present = true;
		}
	}
	return newest_off;
}

/* UBI answers a change whose data fails its seal by standing the block's
 * previous copy back up, if reclaim has not taken it yet. When that block is
 * the newest master, both masters then read valid, the newer of them with a
 * ceiling that trails ids already on flash — a rollback the master headers
 * cannot show. The damage UBI reports at attach must make mount scan. */
ZTEST(blob_db_ubi, test_rolled_back_master_still_raises_ceiling)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_BLOB_DB_UBI_ATOMIC_REPLACE);

	/* After format the first master sits in LEB 0 and the second is
	 * unmapped. The first alloc_id persists a ceiling one step
	 * (CONFIG_BLOB_DB_ID_HINT_STEP) ahead to LEB 1, and the allocation that
	 * reaches it persists the next to LEB 0 — so a step and a quarter of
	 * allocations leave LEB 0 newest, its previous copy awaiting reclaim,
	 * and LEB 1's ceiling below the last id. */
	uint64_t id = 0;

	for (int i = 0; i < CONFIG_BLOB_DB_ID_HINT_STEP + CONFIG_BLOB_DB_ID_HINT_STEP / 4; i++) {
		id = blob_db_alloc_id();
		zassert_not_equal(id, 0);
	}
	zassert_ok(blob_db_update(id, "HIGH", 4));
	blob_db_unmount();

	bool older_present;
	const off_t newest = newest_master_copy(&older_present);

	zassert_true(newest >= 0, "setup: no master copy found");
	zassert_true(older_present,
		     "setup: the newest master's previous copy was reclaimed");

	/* Clear one bit of the magic: NOR-legal, and enough to fail UBI's
	 * data seal over the whole copy. */
	const struct flash_area *fa;
	uint8_t byte;

	zassert_ok(flash_area_open(PARTITION_ID_UNDER_TEST, &fa));
	zassert_ok(flash_area_read(fa, newest, &byte, 1));
	byte &= (uint8_t)~0x02;
	zassert_ok(flash_area_write(fa, newest, &byte, 1));
	flash_area_close(fa);

	zassert_ok(blob_db_mount());

	uint8_t buf[8];
	size_t got;

	zassert_ok(blob_db_get(id, buf, sizeof(buf), &got));
	zassert_mem_equal(buf, "HIGH", 4);
	zassert_ok(blob_db_update(id, "HIGH", 4),
		   "bound id above the adopted ceiling refused");

	const uint64_t next = blob_db_alloc_id();

	zassert_true(next > id, "re-issued a bound id: next=%llu id=%llu",
		     (unsigned long long)next, (unsigned long long)id);
}
