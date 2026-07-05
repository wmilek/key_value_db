/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_KVDB_H_
#define APP_LIB_KVDB_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_kvdb kvdb — string key/value interface (L3)
 * @ingroup lib
 * @{
 *
 * @brief Ergonomic string key -> value store over any Map container.
 *
 * SKELETON — API prototypes only; see doc/layers/l3_interfaces.md. The
 * backend Map container is chosen by Kconfig. Not yet implemented.
 */

int kvdb_open(void);
int kvdb_set(const char *key, const void *val, size_t len);
int kvdb_get(const char *key, void *out, size_t out_sz, size_t *out_len);
int kvdb_delete(const char *key);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_KVDB_H_ */
