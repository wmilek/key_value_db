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
 * @brief What the application knows about the population it will store.
 *
 * These describe the *data*, not the container's geometry: a provider derives
 * its own shape (bucket count, depth, bucket size) from them. An application
 * that states its population does not need to know how any container is built.
 *
 * **The zero value means "I do not know".** A NULL @p cfg and an all-zero
 * struct are identical and always legal — the provider chooses everything.
 * Each field is independently unset at zero, so partial knowledge is normal:
 * fill in what you know and leave the rest.
 *
 * **A field that IS set is binding.** A provider either honours it or fails
 * the create; it must never silently deliver something else. Unset is the
 * provider's business, set is the caller's, and quietly substituting a
 * different value is neither.
 *
 * Once a map exists the values that shaped it are fixed on flash, and a later
 * open cannot change them.
 */
struct map_config {
	/** How many entries will live here (0 = unknown). */
	size_t expected_entries;
	/** Mean key+value bytes of an entry (0 = unknown). */
	size_t typical_entry_bytes;
	/**
	 * Largest key+value bytes any single entry will reach (0 = unknown).
	 *
	 * Not padding: a provider that packs entries together sizes its
	 * records against the *tail*, not the mean, so this is what keeps a
	 * heavy-tailed population from overflowing a record that the average
	 * would have fitted comfortably.
	 */
	size_t max_entry_bytes;
};

/**
 * @brief What a provider actually built, reported back.
 *
 * Every field is derived from state the map already keeps in order to work —
 * reading it costs at most a blob read and never a write. Deliberately absent:
 * the entry count and the largest stored entry. Neither can be produced
 * without either maintaining a counter on the write path or walking every
 * record, and a diagnostic that costs a write per insert is worse than no
 * diagnostic.
 */
struct map_info {
	/** Levels of indirection above the records (1 = flat). */
	uint8_t depth;
	/** Slots in the top level; 0 when @ref depth is 1. */
	uint16_t fanout;
	/** Total records the map can spread entries over. */
	uint32_t buckets;
	/** Largest key+value bytes a single entry may reach. */
	size_t entry_bytes_limit;
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
	 * may be NULL for provider defaults. To learn what the provider built,
	 * call @ref stat — create deliberately does not also report it. A store
	 * is created once in its life, so an out-parameter here would save one
	 * blob read, ever, at the price of two code paths producing the same
	 * facts and being able to disagree.
	 *
	 * A set @p cfg field that cannot be satisfied fails the call, and the
	 * provider must not have written anything: validation is arithmetic, so
	 * a rejected create leaves no allocated id and no orphan blob behind.
	 *
	 * @retval 0        created
	 * @retval -EINVAL  a set cfg field cannot be satisfied by any medium
	 *                  state — a contradiction, or past what the format
	 *                  allows. Deterministic: it will fail identically on
	 *                  every boot, so it is a build-time mistake to fix in
	 *                  source rather than a condition to retry.
	 * @retval -ENOSPC  the geometry is valid but the medium cannot hold it
	 *                  right now. Stateful: may succeed after compaction or
	 *                  on a fresh device.
	 * @retval -EIO     flash error
	 */
	int (*create)(uint64_t root, const struct map_config *cfg);

	/**
	 * @brief Report the geometry this map was built with.
	 *
	 * Costs at most a blob read and never a write (see @ref map_info).
	 *
	 * @retval 0        *out filled
	 * @retval -EIO     flash error, or @p root does not hold a valid map
	 */
	int (*stat)(uint64_t root, struct map_info *out);

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
	 * A **positive** return still means stored — it is a warning, not a
	 * failure, so callers testing `rc != 0` for failure must test `rc < 0`.
	 * The provider pays nothing to produce it: a set has already read the
	 * record it is about to rewrite, so it knows how full that record is.
	 *
	 * @retval 0        stored
	 * @retval >0       stored, and the record holding this key has passed
	 *                  the provider's near-full threshold. The next few
	 *                  inserts here may return -ENOSPC, and a map that
	 *                  cannot grow has no recovery from that — so this is
	 *                  the point at which to re-plan, not the -ENOSPC.
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
