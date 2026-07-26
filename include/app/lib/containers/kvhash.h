/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_CONTAINERS_KVHASH_H_
#define APP_LIB_CONTAINERS_KVHASH_H_

#include <app/lib/containers/shape_map.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief kvhash's Map provider — O(1) hash-bucket container.
 *
 * Bind this into an L3 interface via its @ref map_ops. See kvhash.c for the
 * on-flash layout (directory blob of bucket ids + one packed blob per bucket).
 */
extern const struct map_ops kvhash_map_ops;

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_CONTAINERS_KVHASH_H_ */
