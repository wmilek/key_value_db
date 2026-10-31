/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_CONTAINERS_SET_OVER_MAP_H_
#define APP_LIB_CONTAINERS_SET_OVER_MAP_H_

#include <app/lib/containers/shape_map.h>
#include <app/lib/containers/shape_set.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_set_over_map Set over any Map (set_over_map)
 * @ingroup lib
 * @{
 *
 * @brief A @ref set_ops provider built on any @ref map_ops: a member is a
 *        map key whose value is empty.
 *
 * It gets the Set semantics right: an add of a present member writes nothing
 * and answers -EEXIST, a del of an absent one answers -ENOENT. What it does
 * not get is a set's density (the map's record still carries a value length)
 * or a single pass on insert: @ref set_ops.add probes with map_ops.get and
 * then calls map_ops.set, so a new member reads its path twice. A re-add reads
 * it once and writes nothing.
 *
 * Order, cost and crash behaviour are the map's. Over kvhash the walk order is
 * kvhash's; over an ordered map it is sorted.
 *
 * **On flash** an adapter set is indistinguishable from a map whose values are
 * all empty, so the holder of the root records which it is (for example in a
 * table directory). A value that is not empty means the root is not such a
 * set, and @ref set_ops.has and @ref set_ops.next answer -EIO for it.
 *
 * Binding is compile-time, like everything else in the stack:
 *
 * @code
 * SET_OVER_MAP_DEFINE(my_set_ops, &kvhash_map_ops);
 * @endcode
 *
 * defines a <tt>const struct set_ops my_set_ops</tt> whose ops forward to the
 * functions below with that map. With kvhash built in, @ref set_over_kvhash_ops
 * is predefined.
 *
 * See doc/proposals/2026-10-31-set-shape.md §3.
 */

/** @cond INTERNAL_HIDDEN */
int set_over_map_create(const struct map_ops *m, uint64_t root,
			const struct set_config *cfg);
int set_over_map_stat(const struct map_ops *m, uint64_t root,
		      struct set_info *out);
int set_over_map_has(const struct map_ops *m, uint64_t root,
		     const void *key, size_t klen);
int set_over_map_add(const struct map_ops *m, uint64_t root,
		     const void *key, size_t klen);
int set_over_map_del(const struct map_ops *m, uint64_t root,
		     const void *key, size_t klen);
int set_over_map_next(const struct map_ops *m, uint64_t root,
		      const void *key, size_t klen,
		      void *kout, size_t kout_sz, size_t *kout_len);
int set_over_map_count(const struct map_ops *m, uint64_t root, size_t *out);
int set_over_map_destroy(const struct map_ops *m, uint64_t root);
/** @endcond */

/**
 * @brief Define <tt>const struct set_ops name</tt> over the map @p map_ops_ptr.
 *
 * @param name         identifier of the set_ops instance to define
 * @param map_ops_ptr  a <tt>const struct map_ops *</tt> constant expression
 */
#define SET_OVER_MAP_DEFINE(name, map_ops_ptr)                                    \
	static int name##_create(uint64_t r, const struct set_config *c)         \
	{ return set_over_map_create((map_ops_ptr), r, c); }                     \
	static int name##_stat(uint64_t r, struct set_info *o)                   \
	{ return set_over_map_stat((map_ops_ptr), r, o); }                       \
	static int name##_has(uint64_t r, const void *k, size_t kl)              \
	{ return set_over_map_has((map_ops_ptr), r, k, kl); }                    \
	static int name##_add(uint64_t r, const void *k, size_t kl)              \
	{ return set_over_map_add((map_ops_ptr), r, k, kl); }                    \
	static int name##_del(uint64_t r, const void *k, size_t kl)              \
	{ return set_over_map_del((map_ops_ptr), r, k, kl); }                    \
	static int name##_next(uint64_t r, const void *k, size_t kl, void *ko,   \
			       size_t ks, size_t *kol)                           \
	{ return set_over_map_next((map_ops_ptr), r, k, kl, ko, ks, kol); }      \
	static int name##_count(uint64_t r, size_t *o)                           \
	{ return set_over_map_count((map_ops_ptr), r, o); }                      \
	static int name##_destroy(uint64_t r)                                    \
	{ return set_over_map_destroy((map_ops_ptr), r); }                       \
	const struct set_ops name = {                                            \
		.create = name##_create,                                         \
		.stat = name##_stat,                                             \
		.has = name##_has,                                               \
		.add = name##_add,                                               \
		.del = name##_del,                                               \
		.next = name##_next,                                             \
		.count = name##_count,                                           \
		.destroy = name##_destroy,                                       \
	}

#if defined(CONFIG_BLOB_CONTAINER_KVHASH)
/** The Set shape over kvhash_map_ops. */
extern const struct set_ops set_over_kvhash_ops;
#endif

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_CONTAINERS_SET_OVER_MAP_H_ */
