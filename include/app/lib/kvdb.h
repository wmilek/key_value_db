/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_KVDB_H_
#define APP_LIB_KVDB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <app/lib/containers/shape_map.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_kvdb kvdb — string key/value interface (L3)
 * @ingroup lib
 * @{
 *
 * @brief Ergonomic NUL-terminated-string key -> opaque byte value store.
 *
 * kvdb is a thin veneer over the Map shape (@ref map_ops): it turns C-string
 * keys into the byte-slice keys the container speaks and otherwise forwards
 * straight through. The backing container (hash / list / tree) is chosen when
 * an instance is *first created* and then persisted, so every later open binds
 * the right one automatically.
 *
 * @section kvdb_instances Multiple instances
 *
 * A kvdb is named. Distinct names are fully independent stores — you can keep
 * a "sessions" map and a "config" map side by side, each with its own backend
 * and contents. The short name is hashed to a 32-bit id and combined with the
 * 'KVDB' magic into a root-registry key (see rootreg); the name is also stored
 * so a hash collision between two different names is detected, not silently
 * merged. Names are 1..@ref KVDB_NAME_MAX bytes.
 *
 * @section kvdb_create Creation parameters
 *
 * @ref kvdb_config carries a *preference* — which backend to build, and a
 * capacity hint for it. It is consumed **only when the named instance does not
 * yet exist**. On every subsequent open the stored structure governs and the
 * config is ignored (a conflicting cfg is not an error — the existing store
 * wins). Pass NULL to accept the Kconfig-selected default backend.
 *
 * @section kvdb_concurrency Concurrency
 *
 * Single-threaded, inheriting the blob_db v1 contract: the caller serializes
 * all calls across every open instance.
 *
 * See doc/layers/l3_interfaces.md for the full design.
 */

/** Longest kvdb instance name, in bytes (excluding the NUL). */
#define KVDB_NAME_MAX 15

/** Which Map container backs a freshly created instance. */
enum kvdb_backend {
	KVDB_BACKEND_DEFAULT = 0, /**< use the Kconfig-selected default */
	KVDB_BACKEND_HASH,        /**< kvhash — O(1) hash buckets */
	KVDB_BACKEND_TREE,        /**< kvtree — ordered (not yet available) */
	KVDB_BACKEND_LIST,        /**< kvlist — O(n) linear (not yet available) */
};

/**
 * @brief Creation preference for a kvdb instance. Consumed only on first
 *        create (see @ref kvdb_create).
 */
struct kvdb_config {
	enum kvdb_backend backend;      /**< preferred backend to build */
	size_t            initial_capacity; /**< entry-count hint (0 = default) */
};

/**
 * @brief An open handle. Opaque — populated by @ref kvdb_open, passed by
 *        pointer to every other call. Copyable; owns no resources to release.
 */
typedef struct {
	uint64_t root;             /**< bound container structure root id */
	const struct map_ops *ops; /**< bound backend op vector */
} kvdb_t;

/**
 * @brief Open the named instance, creating it from @p cfg if it does not exist.
 *
 * Idempotent per name: the first call with a given name creates the store
 * (honoring @p cfg); later calls attach to it and ignore @p cfg. Requires
 * blob_db_mount() and rootreg_init() to have run.
 *
 * @param db    (out) handle to populate
 * @param name  1..@ref KVDB_NAME_MAX byte instance name
 * @param cfg   creation preference, or NULL for defaults (create only)
 *
 * @retval 0        opened; @p db ready
 * @retval -EINVAL  db/name NULL, or name empty / too long
 * @retval -EEXIST  name collides (same 32-bit id) with a different stored name
 * @retval -ENOTSUP requested/stored backend is not compiled into this build
 * @retval -ENOSPC  registry full, or the store could not be created
 * @retval -ENODEV  blob_db not mounted / registry not bootstrapped
 * @retval -EIO     flash error or corrupt metadata
 */
int kvdb_open(kvdb_t *db, const char *name, const struct kvdb_config *cfg);

/**
 * @brief Store @p val under @p key (insert or replace).
 *
 * @retval 0        stored
 * @retval -EINVAL  bad arguments
 * @retval -ENOSPC  value does not fit the backend's per-key budget
 * @retval -EIO     flash error
 */
int kvdb_set(kvdb_t *db, const char *key, const void *val, size_t len);

/**
 * @brief Fetch the value for @p key.
 *
 * @param out_len (out, optional) true value length (set even on -ENOMEM)
 *
 * @retval 0        found; value copied into @p out
 * @retval -ENOENT  key not present
 * @retval -ENOMEM  @p out_sz too small (key exists)
 * @retval -EINVAL  bad arguments
 */
int kvdb_get(kvdb_t *db, const char *key, void *out, size_t out_sz, size_t *out_len);

/**
 * @brief Remove @p key.
 *
 * @retval 0        removed
 * @retval -ENOENT  key not present
 */
int kvdb_delete(kvdb_t *db, const char *key);

/**
 * @brief Predicate: is @p key present?
 *
 * @return true iff @ref kvdb_get would find the key.
 */
bool kvdb_has(kvdb_t *db, const char *key);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_KVDB_H_ */
