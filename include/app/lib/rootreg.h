/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_ROOTREG_H_
#define APP_LIB_ROOTREG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_rootreg rootreg — root registry (L1.5)
 * @ingroup lib
 * @{
 *
 * @brief Global registry of structure roots: compile-time key -> root id.
 *
 * Owner of id = 1 when CONFIG_BLOB_ROOTREG is enabled — nothing else in the
 * system may bind, rebind, or assume anything about id 1. Every mutation is
 * a single atomic blob_db_update(1, ...) (basic flow — no intent, no
 * recovery machinery, no residue possible). Every operation costs one flash
 * read of the registry blob plus a linear scan; capacity is
 * CONFIG_ROOTREG_MAX_ROOTS entries. Single-threaded like blob_db v1.
 *
 * Full contract: doc/layers/l1_root_registry.md.
 */

/** Compose a registry key from a FourCC magic and an instance number.
 *  Key 0 is invalid (reserved); magics are project-coordinated constants. */
#define ROOTREG_KEY(magic32, instance32) \
	(((uint64_t)(uint32_t)(magic32) << 32) | (uint32_t)(instance32))

/**
 * @brief Bootstrap the registry. Run once after blob_db_mount(), before any
 *        other storage user.
 *
 * On a virgin store (root exists with an empty payload — the post-format
 * state) this binds id = 1 to an empty registry image. On a store that
 * already holds a valid registry it is a no-op. Idempotent.
 *
 * @retval 0        registry ready
 * @retval -ENOTSUP id = 1 holds a foreign (non-registry) payload; refused
 * @retval -ENODEV  blob_db not mounted
 * @retval -EIO     flash error or corrupt registry image
 */
int rootreg_init(void);

/**
 * @brief Look up a registered root.
 *
 * @retval 0        found; *root_id filled
 * @retval -ENOENT  key not registered
 * @retval -ENOTSUP registry not bootstrapped (run rootreg_init())
 * @retval -EINVAL  key is 0 or root_id is NULL
 */
int rootreg_get(uint64_t key, uint64_t *root_id);

/**
 * @brief Look up, or atomically allocate-and-register a fresh root id.
 *
 * If the key is absent: allocates an id (RAM), appends the entry, and
 * commits with one atomic update — the registry entry itself is the durable
 * creation intent. The returned id may be allocated-but-unbound (a crash hit
 * an earlier creation before the caller's bind); the caller detects that via
 * blob_db_get(id) == -ENOENT and lazily binds the empty structure.
 *
 * @retval 0        *root_id filled (existing or freshly registered)
 * @retval -ENOSPC  registry is full (CONFIG_ROOTREG_MAX_ROOTS)
 * @retval -ENOTSUP registry not bootstrapped
 * @retval -EINVAL  key is 0 or root_id is NULL
 * @retval -EIO     id allocation or flash error
 */
int rootreg_get_or_create(uint64_t key, uint64_t *root_id);

/**
 * @brief Register an existing structure's root under a new key.
 *
 * @retval 0        registered
 * @retval -EEXIST  key already present
 * @retval -ENOSPC  registry is full
 * @retval -ENOTSUP registry not bootstrapped
 * @retval -EINVAL  key or root_id is 0
 */
int rootreg_set(uint64_t key, uint64_t root_id);

/**
 * @brief Drop the entry. The structure behind it is NOT touched — tearing it
 *        down first is the caller's job.
 *
 * @retval 0        entry removed
 * @retval -ENOENT  key not registered
 * @retval -ENOTSUP registry not bootstrapped
 * @retval -EINVAL  key is 0
 */
int rootreg_unregister(uint64_t key);

/** Per-entry callback for rootreg_iterate(); non-zero stops the walk and is
 *  returned by rootreg_iterate(). */
typedef int (*rootreg_iter_cb_t)(uint64_t key, uint64_t root_id, void *user);

/**
 * @brief Walk every registered (key, root_id) pair.
 *
 * @retval 0        full walk completed
 * @retval other    value the callback returned to stop early
 * @retval -ENOTSUP registry not bootstrapped
 * @retval -EINVAL  cb is NULL
 */
int rootreg_iterate(rootreg_iter_cb_t cb, void *user);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_ROOTREG_H_ */
