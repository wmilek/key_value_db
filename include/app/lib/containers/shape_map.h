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
 * A Map instance is a blob_db root id plus a provider's op vector. The op
 * vector abstracts *how* the container stores pairs (linear list, hash
 * buckets, ordered tree); an L3 interface (kvdb) binds to this shape, never
 * to a concrete container. See doc/layers/l2_containers.md.
 *
 * The shape is deliberately independent of L3: nothing here references kvdb.
 */

/**
 * @brief Creation parameters, consumed once when a map is first built.
 *
 * These are hints, not a contract: a container applies what is meaningful to
 * it and ignores the rest (a hash uses @ref initial_capacity to size its
 * bucket directory; a linear list ignores it). Once a map exists the values
 * that shaped it are fixed on flash and a later open cannot change them.
 */
struct map_config {
	size_t initial_capacity; /**< expected entry count (0 = provider default) */
};

/**
 * @brief The Map operation vector. All operations act on @p root, the
 *        blob_db id of the container's structure (its on-flash root record).
 *
 * Single-threaded, like everything below it (blob_db v1): the caller
 * serializes all calls.
 */
struct map_ops {
	/**
	 * @brief Build a fresh, empty map at @p root.
	 *
	 * Called exactly once, when the structure does not yet exist. @p cfg
	 * may be NULL for provider defaults. On return @p root holds a valid,
	 * empty structure.
	 *
	 * @retval 0        created
	 * @retval -ENOSPC  requested capacity cannot fit the on-flash record
	 * @retval -EIO     flash error
	 */
	int (*create)(uint64_t root, const struct map_config *cfg);

	/**
	 * @brief Look up @p key; copy its value into @p out.
	 *
	 * @p out may be NULL when @p out_sz is 0 (existence probe). If the key
	 * exists but @p out_sz is smaller than the value, returns -ENOMEM with
	 * @p out_len (when non-NULL) set to the true length.
	 *
	 * @retval 0        found; value copied, *out_len set
	 * @retval -ENOENT  key not present
	 * @retval -ENOMEM  out_sz too small (key exists; *out_len set)
	 */
	int (*get)(uint64_t root, const void *key, size_t klen,
		   void *out, size_t out_sz, size_t *out_len);

	/**
	 * @brief Insert @p key or replace its value. Keeps map identity.
	 *
	 * @retval 0        stored
	 * @retval -ENOSPC  the record holding this key would overflow
	 * @retval -EIO     flash error
	 */
	int (*set)(uint64_t root, const void *key, size_t klen,
		   const void *val, size_t vlen);

	/**
	 * @brief Remove @p key.
	 *
	 * @retval 0        removed
	 * @retval -ENOENT  key not present
	 */
	int (*del)(uint64_t root, const void *key, size_t klen);
};

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_CONTAINERS_SHAPE_MAP_H_ */
