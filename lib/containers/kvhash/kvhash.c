/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * kvhash — O(1) Map-shape container (map_ops), built on blob_db.
 *
 * On-flash layout
 * ---------------
 * A kvhash instance is a *directory* blob (the map's root id) plus one blob
 * per non-empty bucket:
 *
 *   root blob (the directory):
 *     [u32 magic 'KVHA'] [u16 n_buckets] [u16 version] [u64 bucket_id]*n
 *     (magic 'KVHD' instead means a destroy has committed and the buckets are
 *      still being released -- see kvhash_destroy)
 *   bucket blob (packed pair list):
 *     ( [u16 klen] [u16 vlen] [key bytes] [val bytes] )*
 *
 * n_buckets is fixed when the map is created (from map_config.initial_capacity)
 * and never changes; a key lands in bucket fnv1a(key) % n_buckets. Each bucket
 * is a tiny linear list, so lookups are O(load factor). A bucket blob is
 * created lazily on first insert (id 0 in the directory means "empty").
 *
 * Bounds. The directory must fit one blob payload, so n_buckets is capped at
 * (MAX_PAYLOAD - 8) / 8. A single bucket's packed list must also fit one
 * payload; an insert that would overflow it returns -ENOSPC. Both are v1
 * limits, not fundamental (a future rev can chain overflow blobs / rehash).
 *
 * Concurrency. Single-threaded, per the blob_db v1 contract: two file-scope
 * scratch buffers are reused across calls, so the caller must serialize.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/kvhash.h>

LOG_MODULE_REGISTER(kvhash, CONFIG_BLOB_CONTAINER_KVHASH_LOG_LEVEL);

#define KVHASH_DIR_MAGIC   0x4b564841u /* 'KVHA' */
#define KVHASH_DIR_DYING   0x4b564844u /* 'KVHD' — destroy committed, release pending */
#define KVHASH_VERSION     1

#define MAX_PAYLOAD        CONFIG_BLOB_DB_MAX_PAYLOAD_LEN
#define DIR_HDR_LEN        8u                          /* magic+n_buckets+version */
#define MAX_BUCKETS        ((MAX_PAYLOAD - DIR_HDR_LEN) / 8u)
#define DEFAULT_BUCKETS    8u

BUILD_ASSERT(MAX_BUCKETS >= 2, "MAX_PAYLOAD too small to hold a bucket directory");

/* Reused across calls (single-threaded contract). One holds the directory,
 * the other the packed bucket currently being read/rewritten. */
static uint8_t dir_buf[MAX_PAYLOAD];
static uint8_t bkt_buf[MAX_PAYLOAD];

/* Host-order packed accessors — the store is read back on the same CPU that
 * wrote it, matching blob_db's own convention. */
static inline uint16_t get_u16(const uint8_t *p)
{
	uint16_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

static inline void put_u16(uint8_t *p, uint16_t v)
{
	memcpy(p, &v, sizeof(v));
}

static inline uint64_t get_u64(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

static inline void put_u64(uint8_t *p, uint64_t v)
{
	memcpy(p, &v, sizeof(v));
}

static uint32_t fnv1a(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t h = 0x811c9dc5u;

	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 0x01000193u;
	}
	return h;
}

/* Clamp a requested capacity to a bucket count that fits one payload. */
static uint16_t buckets_for(size_t initial_capacity)
{
	size_t n = initial_capacity ? initial_capacity : DEFAULT_BUCKETS;

	if (n < 2) {
		n = 2;
	}
	if (n > MAX_BUCKETS) {
		n = MAX_BUCKETS;
	}
	return (uint16_t)n;
}

static inline size_t dir_len(uint16_t n_buckets)
{
	return DIR_HDR_LEN + (size_t)n_buckets * 8u;
}

/*
 * Load and validate the directory into dir_buf; report the bucket count and
 * whether a destroy has already committed on this root.
 */
static int dir_load_raw(uint64_t root, uint16_t *n_buckets, bool *dying)
{
	size_t got = 0;
	int rc = blob_db_get(root, dir_buf, sizeof(dir_buf), &got);

	if (rc != 0) {
		return rc;
	}
	if (got < DIR_HDR_LEN) {
		return -EIO;
	}

	uint32_t magic;

	memcpy(&magic, &dir_buf[0], sizeof(magic));
	if (magic == KVHASH_DIR_MAGIC) {
		*dying = false;
	} else if (magic == KVHASH_DIR_DYING) {
		*dying = true;
	} else {
		return -EIO;
	}

	uint16_t n = get_u16(&dir_buf[4]);

	if (n < 2 || n > MAX_BUCKETS || got < dir_len(n)) {
		return -EIO;
	}
	*n_buckets = n;
	return 0;
}

/*
 * The data-path load. A root whose destroy has committed is gone as far as
 * every reader is concerned (l2_containers.md 2.4), so the whole container
 * answers -ENOENT and no caller sees the part of it still on flash.
 */
static int dir_load(uint64_t root, uint16_t *n_buckets)
{
	bool dying = false;
	int rc = dir_load_raw(root, n_buckets, &dying);

	if (rc != 0) {
		return rc;
	}
	return dying ? -ENOENT : 0;
}

static inline uint64_t dir_bucket(uint16_t idx)
{
	return get_u64(&dir_buf[DIR_HDR_LEN + (size_t)idx * 8u]);
}

static inline void dir_set_bucket(uint16_t idx, uint64_t id)
{
	put_u64(&dir_buf[DIR_HDR_LEN + (size_t)idx * 8u], id);
}

/*
 * Scan a packed bucket for @key. On a hit, returns the byte offset of its
 * entry and fills *entry_len / *val_off / *val_len. On a miss, returns
 * SIZE_MAX.
 */
static size_t bkt_find(const uint8_t *buf, size_t used,
		       const void *key, size_t klen,
		       size_t *entry_len, size_t *val_off, size_t *val_len)
{
	size_t off = 0;

	while (off + 4u <= used) {
		size_t kl = get_u16(&buf[off]);
		size_t vl = get_u16(&buf[off + 2]);
		size_t elen = 4u + kl + vl;

		if (off + elen > used) {
			break; /* truncated / corrupt — stop */
		}
		if (kl == klen && memcmp(&buf[off + 4], key, klen) == 0) {
			if (entry_len) {
				*entry_len = elen;
			}
			if (val_off) {
				*val_off = off + 4u + kl;
			}
			if (val_len) {
				*val_len = vl;
			}
			return off;
		}
		off += elen;
	}
	return SIZE_MAX;
}

/* ---- map_ops ---- */

static int kvhash_create(uint64_t root, const struct map_config *cfg)
{
	uint16_t n = buckets_for(cfg ? cfg->initial_capacity : 0);
	uint32_t magic = KVHASH_DIR_MAGIC;

	memset(dir_buf, 0, dir_len(n));
	memcpy(&dir_buf[0], &magic, sizeof(magic));
	put_u16(&dir_buf[4], n);
	put_u16(&dir_buf[6], KVHASH_VERSION);

	int rc = blob_db_update(root, dir_buf, dir_len(n));

	if (rc == 0) {
		LOG_DBG("created map root=%llu n_buckets=%u", (unsigned long long)root, n);
	}
	return rc;
}

static int kvhash_get(uint64_t root, const void *key, size_t klen,
		      void *out, size_t out_sz, size_t *out_len)
{
	if (key == NULL || klen == 0 || klen > 0xffffu) {
		return -EINVAL;
	}

	uint16_t n;
	int rc = dir_load(root, &n);

	if (rc != 0) {
		return rc;
	}

	uint64_t bid = dir_bucket((uint16_t)(fnv1a(key, klen) % n));

	if (bid == 0) {
		return -ENOENT;
	}

	size_t used = 0;

	rc = blob_db_get(bid, bkt_buf, sizeof(bkt_buf), &used);
	if (rc != 0) {
		return rc;
	}

	size_t val_off, val_len;

	if (bkt_find(bkt_buf, used, key, klen, NULL, &val_off, &val_len) == SIZE_MAX) {
		return -ENOENT;
	}

	if (out_len) {
		*out_len = val_len;
	}
	if (val_len > out_sz) {
		return -ENOMEM;
	}
	if (val_len) {
		memcpy(out, &bkt_buf[val_off], val_len);
	}
	return 0;
}

static int kvhash_set(uint64_t root, const void *key, size_t klen,
		      const void *val, size_t vlen)
{
	if (key == NULL || klen == 0 || klen > 0xffffu || vlen > 0xffffu ||
	    (val == NULL && vlen != 0)) {
		return -EINVAL;
	}

	uint16_t n;
	int rc = dir_load(root, &n);

	if (rc != 0) {
		return rc;
	}

	uint16_t idx = (uint16_t)(fnv1a(key, klen) % n);
	uint64_t bid = dir_bucket(idx);
	size_t used = 0;

	if (bid != 0) {
		rc = blob_db_get(bid, bkt_buf, sizeof(bkt_buf), &used);
		if (rc != 0) {
			return rc;
		}

		/* Drop any existing entry for this key (in-place compaction). */
		size_t off, elen;

		off = bkt_find(bkt_buf, used, key, klen, &elen, NULL, NULL);
		if (off != SIZE_MAX) {
			memmove(&bkt_buf[off], &bkt_buf[off + elen], used - off - elen);
			used -= elen;
		}
	}

	size_t need = 4u + klen + vlen;

	if (used + need > sizeof(bkt_buf)) {
		return -ENOSPC;
	}

	put_u16(&bkt_buf[used], (uint16_t)klen);
	put_u16(&bkt_buf[used + 2], (uint16_t)vlen);
	memcpy(&bkt_buf[used + 4], key, klen);
	if (vlen) {
		memcpy(&bkt_buf[used + 4 + klen], val, vlen);
	}
	used += need;

	bool fresh_bucket = (bid == 0);

	if (fresh_bucket) {
		bid = blob_db_alloc_id();
		if (bid == 0) {
			return -EIO;
		}
	}

	rc = blob_db_update(bid, bkt_buf, used);
	if (rc != 0) {
		return rc;
	}

	if (fresh_bucket) {
		/* Publish the new bucket into the directory. A crash between the
		 * bucket write above and this update leaves an unreferenced blob
		 * (reclaimed by a later format), never a corrupt map. */
		dir_set_bucket(idx, bid);
		rc = blob_db_update(root, dir_buf, dir_len(n));
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static int kvhash_del(uint64_t root, const void *key, size_t klen)
{
	if (key == NULL || klen == 0 || klen > 0xffffu) {
		return -EINVAL;
	}

	uint16_t n;
	int rc = dir_load(root, &n);

	if (rc != 0) {
		return rc;
	}

	uint64_t bid = dir_bucket((uint16_t)(fnv1a(key, klen) % n));

	if (bid == 0) {
		return -ENOENT;
	}

	size_t used = 0;

	rc = blob_db_get(bid, bkt_buf, sizeof(bkt_buf), &used);
	if (rc != 0) {
		return rc;
	}

	size_t off, elen;

	off = bkt_find(bkt_buf, used, key, klen, &elen, NULL, NULL);
	if (off == SIZE_MAX) {
		return -ENOENT;
	}

	memmove(&bkt_buf[off], &bkt_buf[off + elen], used - off - elen);
	used -= elen;

	return blob_db_update(bid, bkt_buf, used);
}

/*
 * Destroy, per l2_containers.md 2.4.
 *
 * Stamping the directory's magic is the commit: one atomic update that takes
 * the whole container out of view while leaving every bucket id in place. That
 * is what lets an interrupted release resume -- the work list is re-read from
 * the root rather than journalled -- and it is why the root has to be released
 * last. Deleting the root first would take the bucket ids with it and strand
 * the buckets with nothing left anywhere to name them.
 *
 * Returning 0 is the only completion; on anything else the caller repeats the
 * call and the release picks up where it stopped.
 */
static int kvhash_destroy(uint64_t root)
{
	uint16_t n;
	bool dying = false;
	int rc = dir_load_raw(root, &n, &dying);

	if (rc != 0) {
		return rc;
	}

	if (!dying) {
		uint32_t magic = KVHASH_DIR_DYING;

		memcpy(&dir_buf[0], &magic, sizeof(magic));

		rc = blob_db_update(root, dir_buf, dir_len(n));
		if (rc != 0) {
			return rc; /* nothing released; the container is intact */
		}
	}

	/* Release the buckets. A bucket already gone is the resumed case, not an
	 * error. A real failure is remembered but does not stop the others --
	 * every one released is progress the repeat need not redo. */
	int first_err = 0;

	for (uint16_t i = 0; i < n; i++) {
		uint64_t bid = dir_bucket(i);

		if (bid == 0) {
			continue;
		}

		int drc = blob_db_delete(bid);

		if (drc != 0 && drc != -ENOENT) {
			LOG_WRN("root=%llu: bucket %u (id %llu) not released: %d",
				(unsigned long long)root, i,
				(unsigned long long)bid, drc);
			if (first_err == 0) {
				first_err = drc;
			}
		}
	}

	if (first_err != 0) {
		return first_err; /* still dying; the caller repeats */
	}

	rc = blob_db_delete(root);
	if (rc == 0) {
		LOG_DBG("destroyed map root=%llu n_buckets=%u",
			(unsigned long long)root, n);
	}
	return rc;
}

const struct map_ops kvhash_map_ops = {
	.create = kvhash_create,
	.get = kvhash_get,
	.set = kvhash_set,
	.del = kvhash_del,
	.destroy = kvhash_destroy,
};
