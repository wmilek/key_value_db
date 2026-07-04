/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_CONTAINERS_SHAPE_MAP_H_
#define APP_LIB_CONTAINERS_SHAPE_MAP_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_shape_map Map shape (map_ops)
 * @ingroup lib
 * @{
 *
 * @brief The abstract Map contract (L2) shared by kvlist / kvhash / kvtree.
 *
 * SKELETON — the operation vector below is the intended boundary between L3
 * interfaces and L2 map containers; see doc/layers/l2_containers.md. Not yet
 * implemented.
 */

/** A map instance is a root i-node id plus its provider's op vector. */
struct map_ops {
	int (*get)(uint64_t root, const void *key, size_t klen,
		   void *out, size_t out_sz, size_t *out_len);
	int (*set)(uint64_t root, const void *key, size_t klen,
		   const void *val, size_t vlen);
	int (*del)(uint64_t root, const void *key, size_t klen);
};

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_CONTAINERS_SHAPE_MAP_H_ */
