/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_CONTAINERS_SHAPE_SEQ_H_
#define APP_LIB_CONTAINERS_SHAPE_SEQ_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_shape_seq Sequence shape (seq_ops)
 * @ingroup lib
 * @{
 *
 * @brief The abstract Sequence contract (L2): a sparse array of values
 *        addressed by index.
 *
 * A Sequence instance is a blob_db root id plus a provider's op vector. It
 * holds up to @ref seq_info.capacity **slots**, numbered from 0. A slot is
 * either empty or holds one **element**: an opaque byte string, stored and
 * replaced independently of every other element. The container owns its
 * elements — the caller passes bytes, never blob ids, and @ref set,
 * @ref del, @ref truncate and @ref destroy release what they replace.
 *
 * What it is for: values addressed by a dense integer, each large enough or
 * changed often enough to deserve its own blob — numbered slot tables
 * (users, keys, templates, presets, schedule entries), arrays of other
 * containers' roots (store the 8-byte root id as the element), and record
 * streams read back by position that must not drop data.
 *
 * What it is not for: one large value addressed by byte offset (that is a
 * blob_db payload, with blob_db_read()/blob_db_write()); a bounded log that
 * evicts its oldest records (logring); lookup by an arbitrary key (map_ops).
 *
 * Single-threaded, like everything below it (blob_db v1): the caller
 * serializes all calls. See doc/layers/l2_containers.md §3 and §4.1.
 *
 * SKELETON — the contract is specified; no provider is implemented yet.
 */

/**
 * @brief What the application knows about the sequence it will store.
 *
 * As for map_config, **the zero value means "I do not know"**: a NULL @p cfg
 * and an all-zero struct are identical and always legal, and the provider
 * chooses. **A field that is set is binding**: the provider honours it
 * exactly or fails the create.
 *
 * Once a sequence exists its geometry is fixed on flash, and a later call
 * cannot change it.
 */
struct seq_config {
	/**
	 * Number of slots (0 = provider default). Valid indexes are
	 * 0 .. max_elems - 1, exactly — the provider picks the depth (one or
	 * two levels) that addresses this many, but does not round the
	 * capacity up.
	 */
	uint32_t max_elems;
};

/**
 * @brief What a provider actually built, reported back.
 *
 * Every field is fixed at create; reading it costs at most a blob read and
 * never a write.
 */
struct seq_info {
	/** Levels of id arrays above the elements (1 = the root holds them). */
	uint8_t depth;
	/** Number of slots; valid indexes are 0 .. capacity - 1. */
	uint32_t capacity;
	/** Largest element, in bytes, @ref set and @ref append accept. */
	size_t elem_bytes_limit;
};

/**
 * @brief The Sequence operation vector. All operations act on @p root, the
 *        blob_db id of the container's structure (its on-flash root record).
 *
 * **Slots and length.** Any slot below the capacity may be set, in any
 * order; a slot never set, or cleared by @ref del or @ref truncate, is empty
 * and answers -ENOENT. An empty element (length 0) is an element like any
 * other and is not the same as an empty slot. The **length** is one past
 * the highest slot set since the last @ref truncate: @ref set beyond it
 * raises it, @ref del leaves it alone, @ref truncate moves it. @ref append
 * sets the slot at the length.
 *
 * **Index bounds.** An index at or past the capacity is -EINVAL on
 * @ref get, @ref set and @ref del: the capacity is known from @ref stat, so
 * such an index is a caller bug, not a state.
 *
 * **Atomicity.** Every mutation is crash-atomic with one commit point
 * (l2_containers.md §2.2). After a power cut at any instant, every slot
 * holds its complete old or complete new element, and the length is its old
 * or new value.
 */
struct seq_ops {
	/**
	 * @brief Build a fresh, empty sequence at @p root.
	 *
	 * Called exactly once, when the structure does not yet exist. @p cfg
	 * may be NULL for provider defaults. A set field that cannot be
	 * satisfied fails the call and the provider has written nothing.
	 *
	 * @retval 0        created; length 0, every slot empty
	 * @retval -EINVAL  @p cfg cannot be satisfied by any medium state —
	 *                  more slots than two levels address. Deterministic:
	 *                  a build-time mistake, not a condition to retry.
	 * @retval -ENOSPC  the medium cannot hold the root right now
	 * @retval -EIO     flash error
	 */
	int (*create)(uint64_t root, const struct seq_config *cfg);

	/**
	 * @brief Report the geometry this sequence was built with.
	 *
	 * @retval 0        *out filled
	 * @retval -ENOENT  @p root does not identify a sequence (never built,
	 *                  or a @ref destroy has begun on it)
	 * @retval -EINVAL  @p out is NULL
	 * @retval -EIO     flash error, or @p root holds something that is not
	 *                  a valid sequence
	 */
	int (*stat)(uint64_t root, struct seq_info *out);

	/**
	 * @brief Report the length: one past the highest slot set since the
	 *        last @ref truncate.
	 *
	 * Stored in the root, so it costs one blob read and never a write. It
	 * is not the number of occupied slots — a sparse sequence has holes
	 * below its length; @ref next walks the occupied ones.
	 *
	 * @retval 0        *out_len set
	 * @retval -ENOENT  @p root does not identify a sequence
	 * @retval -EINVAL  @p out_len is NULL
	 * @retval -EIO     flash error
	 */
	int (*len)(uint64_t root, uint32_t *out_len);

	/**
	 * @brief Copy the element at @p index into @p out.
	 *
	 * @p out may be NULL when @p out_sz is 0 (existence probe). If the
	 * slot is occupied but @p out_sz is smaller than the element, returns
	 * -ENOMEM with @p out_len (when non-NULL) set to the true length.
	 *
	 * @retval 0        element copied, *out_len set
	 * @retval -ENOENT  slot empty, or @p root does not identify a sequence
	 * @retval -ENOMEM  @p out_sz too small (slot occupied; *out_len set)
	 * @retval -EINVAL  @p index at or past the capacity, or @p out NULL
	 *                  with a non-zero @p out_sz
	 * @retval -EIO     flash error
	 */
	int (*get)(uint64_t root, uint32_t index,
		   void *out, size_t out_sz, size_t *out_len);

	/**
	 * @brief Store @p data in the slot at @p index, filling it or
	 *        replacing its element. Raises the length past @p index if it
	 *        is not already.
	 *
	 * Replacing an element rewrites only that element; filling an empty
	 * slot also writes the id array that names it, and the root when the
	 * length grows.
	 *
	 * @retval 0        stored
	 * @retval -ENOENT  @p root does not identify a sequence
	 * @retval -EINVAL  @p index at or past the capacity, @p data NULL with a
	 *                  non-zero @p len, or @p len past
	 *                  @ref seq_info.elem_bytes_limit
	 * @retval -ENOSPC  the medium is full; the slot is unchanged
	 * @retval -EIO     flash error
	 */
	int (*set)(uint64_t root, uint32_t index, const void *data, size_t len);

	/**
	 * @brief Empty the slot at @p index, releasing its element. Does not
	 *        change the length.
	 *
	 * @retval 0        slot emptied
	 * @retval -ENOENT  slot already empty, or @p root does not identify a
	 *                  sequence
	 * @retval -EINVAL  @p index at or past the capacity
	 * @retval -ENOSPC  the medium is full; the slot is unchanged
	 * @retval -EIO     flash error
	 */
	int (*del)(uint64_t root, uint32_t index);

	/**
	 * @brief Store @p data in the slot at the current length — the same as
	 *        @ref set at that index — and report which index it took.
	 *
	 * @param out_index  receives the index used (may be NULL)
	 *
	 * @retval 0        stored at *out_index; the length grew by one
	 * @retval -EFBIG   the length equals the capacity. Deterministic: the
	 *                  sequence cannot be re-shaped after create, so the
	 *                  only recovery is a larger one built elsewhere.
	 * @retval -ENOENT  @p root does not identify a sequence
	 * @retval -EINVAL  @p data NULL with a non-zero @p len, or @p len past
	 *                  @ref seq_info.elem_bytes_limit
	 * @retval -ENOSPC  the medium is full; nothing changed
	 * @retval -EIO     flash error
	 */
	int (*append)(uint64_t root, const void *data, size_t len,
		      uint32_t *out_index);

	/**
	 * @brief Set the length to @p new_len. Every slot at or past it is
	 *        emptied; growing the length leaves the new slots empty.
	 *
	 * Shrinking takes effect at its commit — the slots past @p new_len
	 * read empty from then on — and the elements are released after it.
	 *
	 * @retval 0        length set
	 * @retval -ENOENT  @p root does not identify a sequence
	 * @retval -EINVAL  @p new_len past the capacity
	 * @retval -ENOSPC  the medium is full; nothing changed
	 * @retval -EIO     flash error; the length is old or new, and a repeat
	 *                  finishes releasing the elements
	 */
	int (*truncate)(uint64_t root, uint32_t new_len);

	/**
	 * @brief Return the first occupied slot at or after @p from: its index
	 *        in @p out_index and its element in @p out.
	 *
	 * Enumeration is stateless and the index is the cursor: a walk starts
	 * with @p from = 0 and continues with @p from = *out_index + 1 until the
	 * call answers -ENODATA. There is nothing to open, close or invalidate,
	 * and a walk can be resumed from a saved index after a reboot.
	 *
	 * **Order** is ascending index. Every slot occupied for the whole walk
	 * is returned exactly once. A slot set or emptied during the walk may
	 * or may not be returned, depending on which side of the cursor it
	 * falls. @ref set, @ref del, @ref append and @ref truncate may be called
	 * freely between two calls.
	 *
	 * **Buffers.** As for @ref get: @p out may be NULL when @p out_sz is 0.
	 * On 0 and on -ENOMEM, *out_index and (when non-NULL) *out_len are set,
	 * so the caller can retry the same index with a larger buffer or skip
	 * past it.
	 *
	 * **Cost.** Empty ranges are skipped without reading elements; each
	 * occupied slot costs one existence check, and the element is read
	 * only when @p out_sz is non-zero (l2_containers.md §4.1). A walk with @p out_sz 0 lists the occupied indexes
	 * cheaply — the way to enumerate a slot table or find a free slot.
	 *
	 * @param root       the sequence
	 * @param from       first index to consider; at or past the capacity
	 *                   answers -ENODATA
	 * @param out_index  receives the index found (must not be NULL)
	 * @param out        receives its element
	 * @param out_sz     capacity of @p out
	 * @param out_len    receives the element's length (may be NULL)
	 *
	 * @retval 0        slot found; element copied, *out_index and *out_len set
	 * @retval -ENODATA no occupied slot at or after @p from
	 * @retval -ENOMEM  @p out_sz too small; *out_index and *out_len set
	 * @retval -ENOENT  @p root does not identify a sequence — distinct from
	 *                  the end of a walk
	 * @retval -EINVAL  @p out_index NULL, or @p out NULL with a non-zero
	 *                  @p out_sz
	 * @retval -EIO     flash error
	 */
	int (*next)(uint64_t root, uint32_t from, uint32_t *out_index,
		    void *out, size_t out_sz, size_t *out_len);

	/**
	 * @brief Destroy the sequence at @p root, releasing every blob it owns:
	 *        its elements, its id arrays, and the root last.
	 *
	 * The mirror of @ref create, with the same contract as map_ops.destroy
	 * (l2_containers.md §2.4). Returning 0 is the only completion: on any
	 * other result the caller repeats the call, and keeps its own record
	 * of @p root until one returns 0. Repeating is always safe — a part-way
	 * destroyed sequence resumes; one never built, or already gone, reports
	 * -ENOENT.
	 *
	 * Once a destroy has begun, every other op answers -ENOENT, so a
	 * half-destroyed sequence is never observable.
	 *
	 * @ref create must not be called on a destroyed root: it writes
	 * directly, and blob_db makes `update` on a deleted id undefined
	 * (decision D3). Build a replacement at a fresh `blob_db_alloc_id()`.
	 *
	 * An element holding another container's root id is a value like any
	 * other: destroying the sequence does not destroy that container.
	 *
	 * @retval 0        destroyed; every blob released
	 * @retval -ENOENT  @p root does not identify a sequence
	 * @retval -ENOSPC  no room to commit; the sequence is untouched
	 * @retval -EIO     flash error; untouched or part-way released, and the
	 *                  call should be repeated
	 */
	int (*destroy)(uint64_t root);
};

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_CONTAINERS_SHAPE_SEQ_H_ */
