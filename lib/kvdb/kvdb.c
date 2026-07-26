/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * kvdb — string key -> value interface over any Map container.
 *
 * kvdb owns a small *meta* blob per named instance (the root-registry entry
 * points at it). The meta records which backend built the store and the id of
 * the container's own structure root; string keys are forwarded to the bound
 * map_ops as byte slices. The container never sees the meta — this keeps L2
 * ignorant of L3.
 *
 *   rootreg[ KVDB_KEY(hash(name)) ]  ->  meta blob  ->  { backend, struct_root }
 *                                                            |
 *                                        map_ops(struct_root) operates the map
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>
#include <app/lib/kvdb.h>
#include <app/lib/containers/kvhash.h>

LOG_MODULE_REGISTER(kvdb, CONFIG_BLOBDB_KVDB_LOG_LEVEL);

#define KVDB_MAGIC       0x4b564442u /* 'KVDB' */
#define KVDB_META_VERSION 1

/* Kconfig-selected backend used when the caller passes KVDB_BACKEND_DEFAULT. */
#if defined(CONFIG_KVDB_DEFAULT_BACKEND_TREE)
#define KVDB_DEFAULT_BACKEND KVDB_BACKEND_TREE
#elif defined(CONFIG_KVDB_DEFAULT_BACKEND_LIST)
#define KVDB_DEFAULT_BACKEND KVDB_BACKEND_LIST
#else
#define KVDB_DEFAULT_BACKEND KVDB_BACKEND_HASH
#endif

/* Persisted per-instance metadata. Fixed 32-byte layout, no padding. */
struct kvdb_meta {
	uint32_t magic;
	uint8_t  backend;   /* resolved enum kvdb_backend (never _DEFAULT) */
	uint8_t  version;
	uint8_t  name_len;
	uint8_t  rsvd;
	uint64_t struct_root;
	char     name[KVDB_NAME_MAX + 1];
};
BUILD_ASSERT(sizeof(struct kvdb_meta) == 32, "kvdb_meta must be a tight 32 bytes");

/* Map a stored backend id onto the provider compiled into this build. Backends
 * not enabled in Kconfig resolve to NULL -> -ENOTSUP. */
static const struct map_ops *resolve(enum kvdb_backend b)
{
	switch (b) {
#ifdef CONFIG_BLOB_CONTAINER_KVHASH
	case KVDB_BACKEND_HASH:
		return &kvhash_map_ops;
#endif
	/* kvtree / kvlist providers slot in here once implemented. */
	default:
		return NULL;
	}
}

static uint32_t name_hash(const char *name, size_t len)
{
	uint32_t h = 0x811c9dc5u;

	for (size_t i = 0; i < len; i++) {
		h ^= (uint8_t)name[i];
		h *= 0x01000193u;
	}
	return h;
}

static int create_instance(kvdb_t *db, uint64_t meta_id, const char *name,
			   size_t nlen, const struct kvdb_config *cfg)
{
	enum kvdb_backend want = (cfg && cfg->backend != KVDB_BACKEND_DEFAULT)
					 ? cfg->backend
					 : KVDB_DEFAULT_BACKEND;
	const struct map_ops *ops = resolve(want);

	if (ops == NULL) {
		LOG_ERR("backend %d not built in", want);
		return -ENOTSUP;
	}

	uint64_t struct_root = blob_db_alloc_id();

	if (struct_root == 0) {
		return -EIO;
	}

	struct map_config mc = {
		.initial_capacity = cfg ? cfg->initial_capacity : 0,
	};
	int rc = ops->create(struct_root, &mc);

	if (rc != 0) {
		LOG_ERR("backend create: %d", rc);
		return rc;
	}

	struct kvdb_meta meta = {
		.magic = KVDB_MAGIC,
		.backend = (uint8_t)want,
		.version = KVDB_META_VERSION,
		.name_len = (uint8_t)nlen,
		.struct_root = struct_root,
	};
	memcpy(meta.name, name, nlen);

	/* A crash before this commit leaves struct_root orphaned (reclaimed by a
	 * later format) and the meta unbound, so the next open simply re-creates. */
	rc = blob_db_update(meta_id, &meta, sizeof(meta));
	if (rc != 0) {
		return rc;
	}

	db->root = struct_root;
	db->ops = ops;
	LOG_DBG("created '%.*s' backend=%u root=%llu", (int)nlen, name,
		want, (unsigned long long)struct_root);
	return 0;
}

int kvdb_open(kvdb_t *db, const char *name, const struct kvdb_config *cfg)
{
	if (db == NULL || name == NULL) {
		return -EINVAL;
	}

	size_t nlen = 0;

	while (nlen <= KVDB_NAME_MAX && name[nlen] != '\0') {
		nlen++;
	}
	if (nlen == 0 || nlen > KVDB_NAME_MAX) {
		return -EINVAL;
	}

	uint64_t key = ROOTREG_KEY(KVDB_MAGIC, name_hash(name, nlen));
	uint64_t meta_id = 0;
	int rc = rootreg_get_or_create(key, &meta_id);

	if (rc != 0) {
		return rc;
	}

	struct kvdb_meta meta;
	size_t got = 0;

	rc = blob_db_get(meta_id, &meta, sizeof(meta), &got);
	if (rc == -ENOENT || (rc == 0 && got == 0)) {
		return create_instance(db, meta_id, name, nlen, cfg);
	}
	if (rc != 0) {
		return rc;
	}

	/* Attach to an existing instance. */
	if (got < sizeof(meta) || meta.magic != KVDB_MAGIC) {
		return -EIO;
	}
	if (meta.name_len != nlen || memcmp(meta.name, name, nlen) != 0) {
		/* Same 32-bit id, different name — a genuine collision. */
		LOG_ERR("name '%.*s' collides with stored '%.*s'", (int)nlen, name,
			(int)meta.name_len, meta.name);
		return -EEXIST;
	}

	const struct map_ops *ops = resolve((enum kvdb_backend)meta.backend);

	if (ops == NULL) {
		LOG_ERR("stored backend %u not built in", meta.backend);
		return -ENOTSUP;
	}

	db->root = meta.struct_root;
	db->ops = ops;
	return 0;
}

int kvdb_set(kvdb_t *db, const char *key, const void *val, size_t len)
{
	if (db == NULL || key == NULL) {
		return -EINVAL;
	}
	return db->ops->set(db->root, key, strlen(key), val, len);
}

int kvdb_get(kvdb_t *db, const char *key, void *out, size_t out_sz, size_t *out_len)
{
	if (db == NULL || key == NULL) {
		return -EINVAL;
	}
	return db->ops->get(db->root, key, strlen(key), out, out_sz, out_len);
}

int kvdb_delete(kvdb_t *db, const char *key)
{
	if (db == NULL || key == NULL) {
		return -EINVAL;
	}
	return db->ops->del(db->root, key, strlen(key));
}

bool kvdb_has(kvdb_t *db, const char *key)
{
	if (db == NULL || key == NULL) {
		return false;
	}
	int rc = db->ops->get(db->root, key, strlen(key), NULL, 0, NULL);

	return rc != -ENOENT;
}
