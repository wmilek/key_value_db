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
 * SKELETON — API prototypes only; see doc/layers/l1_root_registry.md for the
 * full contract. Owner of id = 1 when CONFIG_BLOB_ROOTREG is enabled. Every
 * mutation is a single atomic blob_db_update(1, ...) (basic flow — no intent,
 * no recovery machinery). Not yet implemented.
 */

/** Compose a registry key from a FourCC magic and an instance number. */
#define ROOTREG_KEY(magic32, instance32) \
	(((uint64_t)(uint32_t)(magic32) << 32) | (uint32_t)(instance32))

/** Bind id = 1 to the registry, bootstrapping a virgin store if needed. */
int rootreg_init(void);

/** Look up a registered root. -ENOENT if the key is not registered. */
int rootreg_get(uint64_t key, uint64_t *root_id);

/** Look up, or atomically allocate-and-register a fresh (possibly unbound)
 *  root id for this key. */
int rootreg_get_or_create(uint64_t key, uint64_t *root_id);

/** Register an existing structure's root. -EEXIST / -ENOSPC on failure. */
int rootreg_set(uint64_t key, uint64_t root_id);

/** Drop the entry. The structure behind it is NOT touched. */
int rootreg_unregister(uint64_t key);

typedef int (*rootreg_iter_cb_t)(uint64_t key, uint64_t root_id, void *user);
int rootreg_iterate(rootreg_iter_cb_t cb, void *user);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_ROOTREG_H_ */
