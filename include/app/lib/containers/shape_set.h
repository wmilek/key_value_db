/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_CONTAINERS_SHAPE_SET_H_
#define APP_LIB_CONTAINERS_SHAPE_SET_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_shape_set Set shape (set_ops)
 * @ingroup lib
 * @{
 *
 * @brief The abstract Set contract (L2): a collection of distinct keys with
 *        no values, answering membership.
 *
 * A Set instance is a blob_db root id plus a provider's op vector, exactly as
 * a Map is (shape_map.h). The contract mirrors @ref map_ops op for op, so a
 * caller who knows one knows the other. Where this header says "as
 * map_ops.x", the text of shape_map.h applies with "entry" read as "member".
 *
 * **Members.** A member is a non-empty byte string: a NULL key or a @c klen
 * of 0 is -EINVAL on @ref set_ops.has, @ref set_ops.add and @ref set_ops.del,
 * which keeps the empty key free to mean "the start" for @ref set_ops.next.
 * Two members are equal when their lengths and bytes are equal. There are no
 * duplicates: adding a present member changes nothing and writes nothing.
 *
 * **Order.** None is part of the contract. @ref set_ops.next enumerates in a
 * provider-defined order with the same guarantees as map_ops.next: fixed by
 * the key bytes and the geometry alone, stable across reboots and mutation,
 * and not sorted unless the provider says so.
 *
 * Providers: set_over_map.h adapts any @ref map_ops. See
 * doc/proposals/2026-10-31-set-shape.md.
 *
 * Single-threaded, like everything below it: the caller serializes all calls.
 */

/**
 * @brief What the application knows about the population it will store.
 *
 * Same rules as @ref map_config: the zero value means "I do not know" and is
 * always legal, a field that IS set is binding (honoured or the create
 * fails), and the values are fixed on flash once the set exists.
 */
struct set_config {
	/** How many members will live here (0 = unknown). */
	size_t expected_members;
	/** Mean key bytes of a member (0 = unknown). */
	size_t typical_key_bytes;
	/** Largest key bytes any member will reach (0 = unknown). */
	size_t max_key_bytes;
};

/**
 * @brief What a provider actually built, reported back. Same meaning and
 *        cost as @ref map_info.
 */
struct set_info {
	/** Levels of indirection above the records (1 = flat). */
	uint8_t depth;
	/** Slots in the top level; 0 when @ref depth is 1. */
	uint16_t fanout;
	/** Total records the set can spread members over. */
	uint32_t buckets;
	/** Largest key bytes a single member may reach. */
	size_t key_bytes_limit;
};

/**
 * @brief The Set operation vector. All operations act on @p root, the
 *        blob_db id of the container's structure.
 */
struct set_ops {
	/**
	 * @brief Build a fresh, empty set at @p root. As map_ops.create.
	 *
	 * @retval 0        created
	 * @retval -EINVAL  a set cfg field cannot be satisfied (nothing written)
	 * @retval -ENOSPC  the medium cannot hold the geometry right now
	 * @retval -EIO     flash error
	 */
	int (*create)(uint64_t root, const struct set_config *cfg);

	/**
	 * @brief Report the geometry this set was built with. As map_ops.stat.
	 *
	 * @retval 0        *out filled
	 * @retval -EINVAL  @p out is NULL
	 * @retval -EIO     flash error, or @p root does not hold a valid set
	 */
	int (*stat)(uint64_t root, struct set_info *out);

	/**
	 * @brief Is @p key a member?
	 *
	 * @retval 0        present
	 * @retval -ENOENT  absent (or @p root has been destroyed)
	 * @retval -EINVAL  @p key NULL or @p klen 0
	 * @retval -EIO     flash error, or @p root holds something that is not
	 *                  a valid set
	 */
	int (*has)(uint64_t root, const void *key, size_t klen);

	/**
	 * @brief Add @p key.
	 *
	 * A repeated add is not an error of the caller's intent: the set ends
	 * in the requested state either way. It answers -EEXIST rather than 0
	 * so that a caller keeping its own member count (see @ref count) learns
	 * insert from no-op without a probe, and it writes nothing.
	 *
	 * @retval 0        added
	 * @retval -EEXIST  already a member; nothing was written
	 * @retval -ENOSPC  the record holding this key would overflow (as
	 *                  map_ops.set: size the set at @ref create)
	 * @retval -EINVAL  @p key NULL or @p klen 0
	 * @retval -EIO     flash error
	 */
	int (*add)(uint64_t root, const void *key, size_t klen);

	/**
	 * @brief Remove @p key.
	 *
	 * @retval 0        removed
	 * @retval -ENOENT  not a member
	 * @retval -EINVAL  @p key NULL or @p klen 0
	 * @retval -EIO     flash error
	 */
	int (*del)(uint64_t root, const void *key, size_t klen);

	/**
	 * @brief Return the member that follows @p key in enumeration order.
	 *
	 * As map_ops.next without the value buffers: stateless (the key is the
	 * cursor), a walk starts with @p klen == 0 and ends with -ENODATA, the
	 * cursor need not be a member, and @ref add / @ref del between calls
	 * are allowed.
	 *
	 * @retval 0        member returned; key copied, *kout_len set
	 * @retval -ENODATA no member follows @p key
	 * @retval -ENOMEM  @p kout_sz too small; *kout_len set, contents
	 *                  unspecified, retry the same cursor
	 * @retval -ENOENT  @p root does not identify a set
	 * @retval -EINVAL  @p key NULL with a non-zero @p klen, or @p kout NULL
	 *                  with a non-zero @p kout_sz
	 * @retval -EIO     flash error, or @p root holds something that is not
	 *                  a valid set
	 */
	int (*next)(uint64_t root, const void *key, size_t klen,
		    void *kout, size_t kout_sz, size_t *kout_len);

	/**
	 * @brief Report how many members the set holds. As map_ops.count:
	 *        exact at any time, nothing stored, every call walks the set.
	 *
	 * @retval 0        *out set
	 * @retval -ENOENT  @p root does not identify a set
	 * @retval -EINVAL  @p out is NULL
	 * @retval -EIO     flash error, or not a valid set
	 */
	int (*count)(uint64_t root, size_t *out);

	/**
	 * @brief Destroy the set at @p root, releasing every blob it owns.
	 *        As map_ops.destroy, including l2_containers.md §2.4: repeat
	 *        until it returns 0, keep the record of @p root until then.
	 *
	 * @retval 0        destroyed
	 * @retval -ENOENT  @p root does not identify a set
	 * @retval -ENOSPC  no room to commit; the set is untouched
	 * @retval -EIO     flash error; repeat the call
	 */
	int (*destroy)(uint64_t root);
};

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_CONTAINERS_SHAPE_SET_H_ */
