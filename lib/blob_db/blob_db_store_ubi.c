/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db storage backend: a UBI volume (zephyr-ubi v0.1).
 *
 * Each blob_db PEB maps 1:1 onto a UBI LEB. blob_db's core access pattern —
 * erase a bucket, write its header, then append slot records at growing
 * offsets — maps directly onto UBI's operations:
 *
 *   blob_db_store_erase(peb)          -> ubi_leb_erase(lnum)
 *   blob_db_store_write(peb, off, ..) -> ubi_leb_write_at(lnum, off, ..)  (in place)
 *   blob_db_store_read (peb, off, ..) -> ubi_leb_read(lnum, off, ..)
 *
 * The in-place slot append is exactly ubi_leb_write_at(): no read-modify-write,
 * no whole-LEB rewrite. An unmapped LEB reads back as the erased value, so
 * blob_db's "erased sector" expectations hold.
 *
 * Erase is ubi_leb_erase(), not ubi_leb_unmap(). An unmap writes nothing, and
 * the next attach maps the block back with its old contents until a reclaim
 * has erased it. blob_db's recovery reasons about erases that have happened
 * (compaction erases the scratch sector to retire a sealed image), so it is
 * given the erase it asks for, at the cost of one flash erase per call — what
 * the raw partition charges too.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include <psa/crypto.h>
#include <ubi/ubi.h>

#include <app/lib/blob_db_ubi.h>

#include "blob_db_internal.h"
#include "blob_db_store.h"

LOG_MODULE_REGISTER(blob_db_store, CONFIG_BLOB_DB_LOG_LEVEL);

#define BLOB_DB_UBI_PARTITION   storage_partition
#define BLOB_DB_UBI_VOL_NAME    "blobdb"

/* LEBs left out of the volume, on top of the ones UBI holds back for its
 * volume table (CONFIG_BLOB_DB_UBI_SPARE_LEBS). */
#define BLOB_DB_UBI_SPARE_LEBS  CONFIG_BLOB_DB_UBI_SPARE_LEBS

/* Separates this partition's derived keys from any other partition sealed
 * under the same keying material (struct ubi_config.key_context). */
static const uint8_t g_key_context[] = "blob_db";

static struct ubi_device *g_ubi;
static uint32_t g_vol_id = UBI_VOL_ID_INVALID;
static uint32_t g_leb_size;
static psa_key_id_t g_key_id;

static const char *event_name(enum ubi_event_type type)
{
	switch (type) {
	case UBI_EVENT_HDR_CORRUPT:
		return "header corrupt";
	case UBI_EVENT_HDR_TAMPERED:
		return "header tampered";
	case UBI_EVENT_VOLUME_TABLE_CORRUPT:
		return "volume table copy corrupt";
	case UBI_EVENT_VOLUME_TABLE_DEGRADED:
		return "volume table degraded";
	case UBI_EVENT_LEB_ORPHANED:
		return "orphaned LEB";
	case UBI_EVENT_PEB_BAD:
		return "PEB retired";
	case UBI_EVENT_DATA_CORRUPT:
		return "data corrupt";
	default:
		return "unknown";
	}
}

/* Runs under UBI's device lock: report, never call back in. UBI has already
 * acted on what it found; the actions that are ours to take (repair) run at
 * the next mount. */
static void on_event(const struct ubi_event *event, void *user_context)
{
	ARG_UNUSED(user_context);

	LOG_WRN("ubi: %s (PEB %u, vol %d, LEB %u)", event_name(event->type),
		event->pnum,
		event->vol_id == UBI_VOL_ID_INVALID ? -1 : (int)event->vol_id,
		event->lnum);
}

static enum ubi_state_verdict on_state(const struct ubi_device_info *info,
				       void *user_context)
{
	ARG_UNUSED(user_context);

	return blob_db_ubi_state_check(info);
}

/* No rollback store of our own to compare against: trust, unless the
 * application brings one (blob_db_ubi.h). */
__weak enum ubi_state_verdict blob_db_ubi_state_check(const struct ubi_device_info *info)
{
	ARG_UNUSED(info);

	return UBI_STATE_TRUSTED;
}

#if defined(CONFIG_BLOB_DB_UBI_KEY_BUILTIN)
/* A fixed, published key: UBI's MAC then catches damage, not tampering, which
 * is all the previous (unauthenticated) UBI backend offered. Imported as a
 * volatile key at every open and destroyed at close. */
static int key_acquire(psa_key_id_t *key_id)
{
	static const uint8_t ikm[32] = "blob_db UBI dev key, not secret";
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attr, PSA_KEY_TYPE_DERIVE);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);

	const psa_status_t status = psa_import_key(&attr, ikm, sizeof(ikm), key_id);

	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_import_key: %d", (int)status);
		return -EIO;
	}
	return 0;
}

static void key_release(psa_key_id_t key_id)
{
	(void)psa_destroy_key(key_id);
}
#else
static int key_acquire(psa_key_id_t *key_id)
{
	return blob_db_ubi_ikm_key(key_id);
}

static void key_release(psa_key_id_t key_id)
{
	/* The application's key: it outlives the store. */
	ARG_UNUSED(key_id);
}
#endif

/* True when every byte of the partition reads as erased. Read in small
 * pieces: this runs only when UBI found no device of its own, which is the
 * first boot or a substrate that is not ours. */
static int partition_is_blank(uint8_t fa_id, bool *blank)
{
	const struct flash_area *fa;
	uint8_t chunk[256];
	int rc = flash_area_open(fa_id, &fa);

	if (rc != 0) {
		return rc;
	}

	const uint8_t erased = flash_area_erased_val(fa);

	*blank = true;
	for (size_t off = 0; off < fa->fa_size && *blank; off += sizeof(chunk)) {
		const size_t n = MIN(sizeof(chunk), fa->fa_size - off);

		rc = flash_area_read(fa, off, chunk, n);
		if (rc != 0) {
			break;
		}
		for (size_t i = 0; i < n; i++) {
			if (chunk[i] != erased) {
				*blank = false;
				break;
			}
		}
	}

	flash_area_close(fa);
	return rc;
}

/* Attach, formatting only where nothing can be lost.
 *
 * UBI answers -ENODEV both for a blank partition and for one holding someone
 * else's bytes: a flash_area blob_db store, or one written by the previous UBI
 * backend (wmilek/ubi, whose headers this release does not recognise). Mount
 * formats only the first; the others are refused with -ENOTSUP and left
 * untouched, as blob_db refuses any format it does not understand.
 *
 * With discard (blob_db_format()) the caller has asked for the store to go,
 * so anything UBI cannot attach for want of a readable device of its own is
 * formatted: foreign bytes, metadata that will not verify under this key, or
 * a release this build cannot read. Failures that say nothing about what the
 * partition holds — flash or crypto errors, no heap, a refused rollback check
 * — are returned either way. */
static int attach(const struct ubi_config *config, bool discard)
{
	int rc = ubi_device_init(g_ubi, config);

	if (rc == 0) {
		return 0;
	}

	if (discard) {
		if (rc != -ENODEV && rc != -EBADMSG && rc != -ENOTSUP) {
			return rc;
		}
		LOG_WRN("discarding a partition UBI cannot attach (%d)", rc);
	} else {
		bool blank = false;

		if (rc != -ENODEV) {
			return rc;
		}

		rc = partition_is_blank(config->flash_area_id, &blank);
		if (rc != 0) {
			LOG_ERR("reading the partition: %d", rc);
			return rc;
		}
		if (!blank) {
			LOG_ERR("partition holds data but no UBI device this release "
				"can read (another backend's store, or the previous "
				"UBI format); blob_db_format() discards it");
			return -ENOTSUP;
		}
		LOG_INF("blank partition, formatting it as a UBI device");
	}

	rc = ubi_device_format(config);
	if (rc != 0) {
		LOG_ERR("ubi_device_format: %d", rc);
		return rc;
	}
	return ubi_device_init(g_ubi, config);
}

/* Find the volume, or create it on the first mount. Its size is whatever it
 * was created with: blob_db hashes ids modulo the PEB count, so the count is
 * read back rather than re-derived. */
static int open_volume(const struct ubi_device_info *dev, uint32_t *leb_count)
{
	struct ubi_volume_info vol = { 0 };
	int rc = ubi_volume_find(g_ubi, BLOB_DB_UBI_VOL_NAME, &g_vol_id);

	if (rc == -ENOENT) {
		if (dev->free_lebs <= BLOB_DB_UBI_SPARE_LEBS) {
			LOG_ERR("device too small: %u free LEBs", dev->free_lebs);
			return -EINVAL;
		}

		const struct ubi_volume_config cfg = {
			.name = BLOB_DB_UBI_VOL_NAME,
			.leb_count = dev->free_lebs - BLOB_DB_UBI_SPARE_LEBS,
		};

		rc = ubi_volume_create(g_ubi, &cfg, &g_vol_id);
		if (rc != 0) {
			LOG_ERR("ubi_volume_create: %d", rc);
			return rc;
		}
		LOG_INF("created UBI volume '%s' id=%u leb_count=%u",
			BLOB_DB_UBI_VOL_NAME, g_vol_id, cfg.leb_count);
	} else if (rc != 0) {
		LOG_ERR("ubi_volume_find: %d", rc);
		return rc;
	}

	rc = ubi_volume_get_info(g_ubi, g_vol_id, &vol);
	if (rc != 0) {
		LOG_ERR("ubi_volume_get_info: %d", rc);
		return rc;
	}
	if (vol.leb_count > UINT16_MAX) {
		LOG_ERR("volume of %u LEBs exceeds blob_db's 16-bit PEB count",
			vol.leb_count);
		return -ENOTSUP;
	}

	*leb_count = vol.leb_count;
	return 0;
}

/* Run a maintenance operation to the end of its budget; failures are logged,
 * never fatal: the store works without it, only less evenly worn. */
static void maintain(enum ubi_maintenance_op op, uint32_t budget)
{
	struct ubi_maintenance_result res = { 0 };
	const int rc = ubi_maintenance(g_ubi, op, budget, &res);

	if (rc != 0) {
		LOG_WRN("ubi_maintenance(%d): %d (%u done, %u left)", (int)op, rc,
			res.performed, res.remaining);
	}
}

int blob_db_store_open(struct blob_db_store_geom *geom, bool discard)
{
	if (g_ubi != NULL) {
		return -EALREADY;
	}

	psa_status_t status = psa_crypto_init();

	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init: %d", (int)status);
		return -EIO;
	}

	struct ubi_config config = {
		.flash_area_id = PARTITION_ID(BLOB_DB_UBI_PARTITION),
		.key_context = g_key_context,
		.key_context_size = sizeof(g_key_context) - 1,
		.event_cb = on_event,
		.state_cb = on_state,
	};

	int rc = key_acquire(&config.ikm_key_id);

	if (rc != 0) {
		LOG_ERR("no UBI keying material: %d", rc);
		return rc;
	}
	g_key_id = config.ikm_key_id;

	g_ubi = k_calloc(1, ubi_device_size());
	if (g_ubi == NULL) {
		LOG_ERR("no heap for the UBI handle");
		rc = -ENOMEM;
		goto err_key;
	}

	rc = attach(&config, discard);
	if (rc != 0) {
		LOG_ERR("ubi attach: %d", rc);
		goto err_free;
	}

	struct ubi_device_info info = { 0 };

	rc = ubi_device_get_info(g_ubi, &info);
	if (rc != 0) {
		LOG_ERR("ubi_device_get_info: %d", rc);
		goto err_deinit;
	}
	g_leb_size = info.leb_size;

	/* A degraded volume table or a retired block is ours to repair, and
	 * mount is where nothing is waiting on the latency. */
	maintain(UBI_MAINTENANCE_REPAIR, info.peb_count);
	if (info.corrupt_pebs != 0) {
		LOG_WRN("%u PEBs kept as corrupt; UBI refuses to attach once "
			"they reach 1/20 of the device", info.corrupt_pebs);
	}

	uint32_t leb_count = 0;

	rc = open_volume(&info, &leb_count);
	if (rc != 0) {
		goto err_deinit;
	}

	geom->peb_size = g_leb_size;
	geom->write_align = info.write_block_size ? info.write_block_size : 1;
	geom->n_pebs = (uint16_t)leb_count;
	/* A fresh bucket's first write takes an erased block from UBI's free
	 * pool, which blob_db_store_maintain() keeps full; formatting buckets
	 * ahead would only spend that pool early. */
	geom->preformat = false;
	return 0;

err_deinit:
	(void)ubi_device_deinit(g_ubi);
err_free:
	k_free(g_ubi);
	g_ubi = NULL;
	g_vol_id = UBI_VOL_ID_INVALID;
err_key:
	key_release(g_key_id);
	return rc;
}

void blob_db_store_close(void)
{
	if (g_ubi == NULL) {
		return;
	}

	(void)ubi_device_deinit(g_ubi);
	k_free(g_ubi);
	g_ubi = NULL;
	g_vol_id = UBI_VOL_ID_INVALID;
	key_release(g_key_id);
}

int blob_db_store_read(off_t off, void *buf, size_t len)
{
	const uint32_t lnum = (uint32_t)((size_t)off / g_leb_size);
	const uint32_t within = (uint32_t)((size_t)off % g_leb_size);

	BLOB_DB_IO_NOTE(BLOB_DB_IO_READ, len);

	/* An unmapped LEB reads as erased without touching the flash. */
	return ubi_leb_read(g_ubi, g_vol_id, lnum, within, buf, len);
}

int blob_db_store_write(off_t off, const void *buf, size_t len)
{
	const uint32_t lnum = (uint32_t)((size_t)off / g_leb_size);
	const uint32_t within = (uint32_t)((size_t)off % g_leb_size);

	BLOB_DB_IO_NOTE(BLOB_DB_IO_WRITE, len);
	return ubi_leb_write_at(g_ubi, g_vol_id, lnum, within, buf, len);
}

/* Static data never moves on its own: UBI levels wear only when asked.
 * A bucket erase is already a slow call, so it carries the step. */
static void relocate_step(void)
{
	if (CONFIG_BLOB_DB_UBI_RELOCATE_BUDGET > 0) {
		maintain(UBI_MAINTENANCE_RELOCATE, CONFIG_BLOB_DB_UBI_RELOCATE_BUDGET);
	}
}

int blob_db_store_erase(off_t off, size_t len)
{
	if (len == 0) {
		return 0;
	}

	BLOB_DB_IO_NOTE(BLOB_DB_IO_ERASE, len);

	const uint32_t first = (uint32_t)((size_t)off / g_leb_size);
	const uint32_t last = (uint32_t)(((size_t)off + len - 1) / g_leb_size);

	for (uint32_t lnum = first; lnum <= last; lnum++) {
		const int rc = ubi_leb_erase(g_ubi, g_vol_id, lnum);

		if (rc != 0) {
			LOG_ERR("ubi_leb_erase(%u): %d", lnum, rc);
			return rc;
		}
	}

	relocate_step();
	blob_db_maint_kick();
	return 0;
}

int blob_db_store_replace(off_t off, const void *buf, size_t len)
{
	if (!IS_ENABLED(CONFIG_BLOB_DB_UBI_ATOMIC_REPLACE)) {
		const int rc = blob_db_store_erase(off, g_leb_size);

		return rc < 0 ? rc : blob_db_store_write(off, buf, len);
	}

	const uint32_t lnum = (uint32_t)((size_t)off / g_leb_size);

	/* Counted as the erase and the write it stands for, so the seam's
	 * counters compare across backends and options. */
	BLOB_DB_IO_NOTE(BLOB_DB_IO_ERASE, g_leb_size);
	BLOB_DB_IO_NOTE(BLOB_DB_IO_WRITE, len);

	const int rc = ubi_leb_change(g_ubi, g_vol_id, lnum, buf, len);

	if (rc != 0) {
		LOG_ERR("ubi_leb_change(%u, %zu B): %d", lnum, len, rc);
		return rc;
	}

	/* The block it released waits for a reclaim. */
	relocate_step();
	blob_db_maint_kick();
	return 0;
}

bool blob_db_store_replace_is_atomic(void)
{
	return IS_ENABLED(CONFIG_BLOB_DB_UBI_ATOMIC_REPLACE);
}

/* Refill the free pool first, then level wear: a relocation needs a free
 * block to move onto, and the fuller the pool the more worn the block it
 * finds. -EBADMSG is a block relocation refused to move; UBI never picks it
 * again, so it is not a reason to stop. */
int blob_db_store_maintain(uint32_t budget, uint32_t *performed, bool *more)
{
	static const enum ubi_maintenance_op ops[] = {
		UBI_MAINTENANCE_RECLAIM,
		UBI_MAINTENANCE_RELOCATE,
	};

	*performed = 0;
	*more = false;

	for (size_t i = 0; i < ARRAY_SIZE(ops); i++) {
		struct ubi_maintenance_result res = { 0 };
		const int rc = ubi_maintenance(g_ubi, ops[i], budget - *performed,
					       &res);

		*performed += res.performed;
		if (res.remaining != 0) {
			*more = true;
		}
		if (rc == -EBADMSG) {
			LOG_WRN("ubi_maintenance(%d): a block failed verification "
				"and stays where it is", (int)ops[i]);
		} else if (rc != 0) {
			LOG_ERR("ubi_maintenance(%d): %d", (int)ops[i], rc);
			return rc;
		}
	}
	return 0;
}

int blob_db_ubi_device_info(struct ubi_device_info *info)
{
	int rc = -ENODEV;

	blob_db_lock();
	if (g_ubi != NULL) {
		rc = ubi_device_get_info(g_ubi, info);
	}
	blob_db_unlock();
	return rc;
}

int blob_db_ubi_maintenance(enum ubi_maintenance_op op, uint32_t budget,
			    struct ubi_maintenance_result *result)
{
	struct ubi_maintenance_result res = { 0 };
	int rc = -ENODEV;

	blob_db_lock();
	if (g_ubi != NULL) {
		rc = ubi_maintenance(g_ubi, op, budget, &res);
	}
	blob_db_unlock();
	if (result != NULL) {
		*result = res;
	}
	return rc;
}
