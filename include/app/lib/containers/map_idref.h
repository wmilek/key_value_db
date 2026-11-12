/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_CONTAINERS_MAP_IDREF_H_
#define APP_LIB_CONTAINERS_MAP_IDREF_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <app/lib/containers/shape_map.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_map_idref Map id-reference helpers
 * @ingroup lib
 * @{
 *
 * @brief Typed accessors for map values that are blob_db ids.
 *
 * Storing "this key points at that blob" is a recurring shape — a blobfs
 * dirent, an interface's meta pointer, the root of a nested structure. Over
 * the raw Map shape (@ref map_ops) it is spelled by hand at every call site:
 * copy a `uint64_t` into a byte buffer, pass its length, and on the way back
 * copy it out and hope the value really was an id. These two inlines do that
 * once, with the checks, so a reference-valued entry is written and read as a
 * `uint64_t` instead of an untyped `(void *, size_t)` pair.
 *
 * @section map_idref_scope What this is not
 *
 * It is a **compile-time typing aid, not a semantic layer.** In particular:
 *
 * - **No ownership.** `map_ops.del` on a reference entry removes the entry and
 *   nothing else; the blob it named is untouched and remains the caller's to
 *   delete. Nothing here reference-counts, cascades, or garbage-collects — a
 *   blob may be named from several entries, and deciding when it dies stays a
 *   question for whoever created it.
 * - **No new shape or container.** These are inlines over the existing
 *   @ref map_ops, so every Map provider gets them for free and none of them
 *   changes.
 * - **No format change.** The stored value is the eight bytes of the id and
 *   nothing more — byte-identical to a hand-rolled `memcpy` of a `uint64_t`,
 *   in the host byte order the containers already use for packed fields
 *   (a store is read back on the CPU that wrote it). A value written by
 *   @ref map_idref_set is readable as a plain 8-byte value by `map_ops.get`,
 *   and vice versa. There is deliberately no type tag on flash: adding one
 *   would make these entries a distinct on-flash kind, which is a format
 *   decision, not a typing one.
 *
 * @section map_idref_checks What is checked
 *
 * The type safety is the C signature plus two runtime guards on read, which
 * together catch the realistic mistake — reading a key whose value was never
 * an id:
 *
 * - the stored value must be exactly @ref MAP_IDREF_VALUE_LEN bytes, so a
 *   shorter or longer value reports `-EINVAL` rather than being reinterpreted
 *   (a longer one would otherwise surface as a bare `-ENOMEM`);
 * - the id must be non-zero. `blob_db_alloc_id()` never yields 0, so a zero
 *   reads as "not a reference" — which also makes zeroed bytes fail loudly
 *   instead of resolving to a plausible-looking id.
 *
 * Neither guard can tell an id from an unrelated 8-byte value that happens to
 * be non-zero; distinguishing those needs a tag, and that is out of scope
 * above.
 */

/** Bytes a reference occupies as a map value. */
#define MAP_IDREF_VALUE_LEN sizeof(uint64_t)

/**
 * @brief Store @p id as the value of @p key (insert or replace).
 *
 * @param ops   the map provider's op vector
 * @param root  the map's structure root id
 * @param key   key bytes
 * @param klen  key length
 * @param id    blob_db id to record; must be non-zero
 *
 * @retval 0        stored
 * @retval -EINVAL  @p ops NULL, @p id zero, or the provider rejected the key
 * @retval -ENOSPC  the entry does not fit the provider's per-key budget
 * @retval -EIO     flash error
 */
static inline int map_idref_set(const struct map_ops *ops, uint64_t root,
				const void *key, size_t klen, uint64_t id)
{
	if (ops == NULL || id == 0) {
		return -EINVAL;
	}
	return ops->set(root, key, klen, &id, sizeof(id));
}

/**
 * @brief Fetch the reference stored under @p key.
 *
 * @param ops     the map provider's op vector
 * @param root    the map's structure root id
 * @param key     key bytes
 * @param klen    key length
 * @param out_id  (out) the recorded blob_db id, untouched on failure
 *
 * @retval 0        found; @p out_id set
 * @retval -ENOENT  key not present
 * @retval -EINVAL  @p ops / @p out_id NULL, or the stored value is not a
 *                  reference (wrong length, or zero)
 * @retval -EIO     flash error
 */
static inline int map_idref_get(const struct map_ops *ops, uint64_t root,
				const void *key, size_t klen, uint64_t *out_id)
{
	if (ops == NULL || out_id == NULL) {
		return -EINVAL;
	}

	uint64_t id = 0;
	size_t got = 0;
	int rc = ops->get(root, key, klen, &id, sizeof(id), &got);

	if (rc == -ENOMEM) {
		return -EINVAL; /* value longer than an id — not a reference */
	}
	if (rc != 0) {
		return rc;
	}
	if (got != sizeof(id) || id == 0) {
		return -EINVAL;
	}

	*out_id = id;
	return 0;
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_CONTAINERS_MAP_IDREF_H_ */
