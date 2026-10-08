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
 *
 * **Keys and values.** A key is a non-empty byte string: a NULL key or a
 * @c klen of 0 is -EINVAL on @ref get, @ref set and @ref del. That keeps the
 * empty key free to mean "the start" for @ref next. A value may be empty, and
 * an empty value is stored like any other — @ref get finds it with length 0,
 * which is not the same as -ENOENT. A NULL @c val is legal only when
 * @c vlen is 0. Upper bounds on key and value length are the provider's (see
 * @ref map_info.entry_bytes_limit).
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
	 * @retval -EINVAL  @p key NULL or @p klen 0
	 */
	int (*get)(uint64_t root, const void *key, size_t klen,
		   void *out, size_t out_sz, size_t *out_len);

	/**
	 * @brief Insert @p key or replace its value. Keeps map identity.
	 *
	 * @retval 0        stored
	 * @retval -ENOSPC  the record holding this key would overflow. A map
	 *                  cannot be re-shaped after create, so a caller that
	 *                  meets this has no recovery beyond rebuilding at a
	 *                  larger declaration — size the map at @ref create,
	 *                  which is the only point where this is preventable.
	 * @retval -EINVAL  @p key NULL or @p klen 0, or @p val NULL with a
	 *                  non-zero @p vlen
	 * @retval -EIO     flash error
	 */
	int (*set)(uint64_t root, const void *key, size_t klen,
		   const void *val, size_t vlen);

	/**
	 * @brief Remove @p key.
	 *
	 * @retval 0        removed
	 * @retval -ENOENT  key not present
	 * @retval -EINVAL  @p key NULL or @p klen 0
	 */
	int (*del)(uint64_t root, const void *key, size_t klen);

	/**
	 * @brief Return the entry that follows @p key in the map's enumeration
	 *        order: copy its key into @p kout and its value into @p vout.
	 *
	 * Enumeration is stateless: the key *is* the cursor. The map holds no
	 * iterator, so there is nothing to open, close or invalidate, and a walk
	 * can be resumed from a saved key at any later time — after a reboot
	 * included. A walk starts with @p klen == 0 (@p key is then ignored and
	 * may be NULL) and continues by passing back each key it is given, until
	 * the call answers -ENODATA.
	 *
	 * **Order.** A provider-defined total order on keys. It is not sorted
	 * and not insertion order, and callers must not assume either. It is a
	 * function of the key bytes and of what @ref create fixed, nothing else,
	 * which is what makes the following hold:
	 *
	 * - The cursor need not be present. A deleted key, or one that never
	 *   existed, still has a well-defined successor.
	 * - The order never changes: not across reboots, not when other keys are
	 *   inserted or deleted, and not when a key's value is replaced.
	 * - Every key present for the whole of a walk is returned exactly once,
	 *   and the walk ends: each call returns a key strictly after its cursor.
	 *   A key inserted or deleted during the walk may or may not be returned,
	 *   depending on which side of the cursor it falls.
	 *
	 * **Mutation between calls is allowed.** @ref set and @ref del may be
	 * called freely between two @c next calls, including on the key just
	 * returned; the walk stays correct, per the rules above.
	 *
	 * **Buffers.** @p kout may be NULL when @p kout_sz is 0, and @p vout when
	 * @p vout_sz is 0. On 0 and on -ENOMEM, @p kout_len and @p vout_len (each
	 * when non-NULL) are set to the found entry's true key and value lengths.
	 * If either buffer is too small the call returns -ENOMEM, and the buffer
	 * contents are unspecified; nothing has moved, so the caller retries the
	 * same cursor with larger buffers. A caller that wants to continue the
	 * walk needs the key, so in practice @p kout_len is non-NULL.
	 *
	 * **Cost.** Each call is independent, so each pays a lookup of its
	 * cursor plus a forward search to the next entry; a walk of n entries is
	 * n such calls. It is meant for enumeration (listing, export, rebuilding
	 * a map at a new geometry), not as a hot data path.
	 *
	 * @param root      the map
	 * @param key       cursor key, or ignored when @p klen is 0
	 * @param klen      cursor key length; 0 starts at the first entry
	 * @param kout      receives the next entry's key
	 * @param kout_sz   capacity of @p kout
	 * @param kout_len  receives the next entry's key length (may be NULL)
	 * @param vout      receives the next entry's value
	 * @param vout_sz   capacity of @p vout
	 * @param vout_len  receives the next entry's value length (may be NULL)
	 *
	 * @retval 0        entry returned; key and value copied, lengths set
	 * @retval -ENODATA no entry follows @p key (an empty map answers this to
	 *                  the first call)
	 * @retval -ENOMEM  @p kout_sz or @p vout_sz too small; lengths set
	 * @retval -ENOENT  @p root does not identify a map (never built, or
	 *                  destroyed) — distinct from the end of a walk
	 * @retval -EINVAL  @p key NULL with a non-zero @p klen, @p klen longer
	 *                  than any storable key, or a NULL buffer with a
	 *                  non-zero size
	 * @retval -EIO     flash error
	 */
	int (*next)(uint64_t root, const void *key, size_t klen,
		    void *kout, size_t kout_sz, size_t *kout_len,
		    void *vout, size_t vout_sz, size_t *vout_len);

	/**
	 * @brief Destroy the map at @p root, releasing every blob it owns.
	 *
	 * The mirror of @ref create. Only the provider knows which i-nodes its
	 * root reaches, so releasing them has to be a container operation.
	 *
	 * Returning 0 is the only completion: on any other result the caller
	 * repeats the call, and must keep its own record of @p root until one
	 * returns 0 (l2_containers.md §2.4). Repeating is always safe — a
	 * part-way destroyed map resumes; one never built, or already gone,
	 * reports -ENOENT.
	 *
	 * Once a destroy has begun, @ref get, @ref set and @ref del answer
	 * -ENOENT for every key, so a half-destroyed map is never observable.
	 *
	 * @ref create is the exception and must not be called on a destroyed
	 * root: it writes directly, and blob_db makes `update` on a deleted id
	 * undefined behaviour (decision D3). Build a replacement map at a fresh
	 * `blob_db_alloc_id()`.
	 *
	 * @retval 0        destroyed; every blob released
	 * @retval -ENOENT  @p root does not identify a map
	 * @retval -ENOSPC  no room to commit; the map is untouched
	 * @retval -EIO     flash error; the map is untouched or part-way
	 *                  released, and the call should be repeated
	 */
	int (*destroy)(uint64_t root);
};

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_CONTAINERS_SHAPE_MAP_H_ */
