/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * set_over_map — the Set shape (set_ops) over any Map provider (map_ops).
 *
 * A member is a map key whose value is empty. Everything that is not a
 * membership question is forwarded: create and stat translate their structs,
 * del, count and destroy pass straight through. The two ops with work of their
 * own are add, which probes before it writes so that a repeated add writes
 * nothing, and next, which asks the map for no value at all.
 *
 * See doc/proposals/2026-10-31-set-shape.md §3.
 */

#include <errno.h>

#include <app/lib/containers/set_over_map.h>

#if defined(CONFIG_BLOB_CONTAINER_KVHASH)
#include <app/lib/containers/kvhash.h>
#endif

int set_over_map_create(const struct map_ops *m, uint64_t root,
			const struct set_config *cfg)
{
	struct map_config mc = { 0 };

	if (cfg != NULL) {
		/* A member's "entry" is its key: the value is always empty. */
		mc.expected_entries = cfg->expected_members;
		mc.typical_entry_bytes = cfg->typical_key_bytes;
		mc.max_entry_bytes = cfg->max_key_bytes;
	}
	return m->create(root, &mc);
}

int set_over_map_stat(const struct map_ops *m, uint64_t root,
		      struct set_info *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	struct map_info mi;
	int rc = m->stat(root, &mi);

	if (rc != 0) {
		return rc;
	}
	out->depth = mi.depth;
	out->fanout = mi.fanout;
	out->buckets = mi.buckets;
	out->key_bytes_limit = mi.entry_bytes_limit;
	return 0;
}

int set_over_map_has(const struct map_ops *m, uint64_t root,
		     const void *key, size_t klen)
{
	if (key == NULL || klen == 0) {
		return -EINVAL;
	}

	size_t vlen = 0;
	int rc = m->get(root, key, klen, NULL, 0, &vlen);

	/* -ENOMEM: the key is there with a value, which no add made. */
	return rc == -ENOMEM ? -EIO : rc;
}

int set_over_map_add(const struct map_ops *m, uint64_t root,
		     const void *key, size_t klen)
{
	int rc = set_over_map_has(m, root, key, klen);

	if (rc == 0) {
		return -EEXIST; /* nothing written */
	}
	if (rc != -ENOENT) {
		return rc;
	}
	return m->set(root, key, klen, NULL, 0);
}

int set_over_map_del(const struct map_ops *m, uint64_t root,
		     const void *key, size_t klen)
{
	if (key == NULL || klen == 0) {
		return -EINVAL;
	}
	return m->del(root, key, klen);
}

int set_over_map_next(const struct map_ops *m, uint64_t root,
		      const void *key, size_t klen,
		      void *kout, size_t kout_sz, size_t *kout_len)
{
	size_t kl = 0, vl = 0;
	int rc = m->next(root, key, klen, kout, kout_sz, &kl, NULL, 0, &vl);

	if (rc != 0 && rc != -ENOMEM) {
		return rc;
	}
	if (vl != 0) {
		return -EIO; /* a value no add made: not an adapter set */
	}
	if (kout_len != NULL) {
		*kout_len = kl;
	}
	return rc;
}

int set_over_map_count(const struct map_ops *m, uint64_t root, size_t *out)
{
	return m->count(root, out);
}

int set_over_map_destroy(const struct map_ops *m, uint64_t root)
{
	return m->destroy(root);
}

#if defined(CONFIG_BLOB_CONTAINER_KVHASH)
SET_OVER_MAP_DEFINE(set_over_kvhash_ops, &kvhash_map_ops);
#endif
