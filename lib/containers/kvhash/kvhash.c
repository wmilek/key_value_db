/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * kvhash — O(1) Map-shape container (map_ops), built on blob_db.
 *
 * On-flash layout
 * ---------------
 * Every level is the same record, which is what makes depth a parameter
 * rather than a rewrite:
 *
 *   directory blob:
 *     [u32 magic 'KVHA'] [u16 n] [u8 version] [u8 depth] [u64 child_id]*n
 *   bucket blob (packed pair list):
 *     ( [u16 klen] [u16 vlen] [key bytes] [val bytes] )*
 *
 * At depth 1 the root IS the directory: n is the bucket count and each child
 * is a bucket blob, created lazily (id 0 means "empty"). At depth 2 the root's
 * children are the roots of n depth-1 sub-maps, created eagerly at create, so
 * the top level is written exactly once and never again. That is why a second
 * level costs no new crash-consistency: only leaf directories are ever
 * rewritten, exactly as at depth 1.
 *
 * Why two levels. A one-level map stores every bucket id in one blob, so that
 * blob grows with the bucket count and is re-read on every operation. Two
 * levels make it O(sqrt(n)) instead, which is what lets a map hold enough
 * buckets to keep each one small — and small buckets are what keep writes
 * cheap, because a set rewrites its whole bucket. See
 * doc/proposals/2026-08-20-kvhash-second-level.md.
 *
 * Geometry is derived from the population the caller declares, never from a
 * bucket count it passes in: an application states what it will store and the
 * container decides how. See derive_geometry().
 *
 * Concurrency. Single-threaded, per the blob_db v1 contract: two file-scope
 * scratch buffers are reused across calls, so the caller must serialize. Two
 * levels do not need a third buffer — the top directory is consumed to find
 * the sub-map root and then dir_buf is reused for the leaf.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/kvhash.h>

LOG_MODULE_REGISTER(kvhash, CONFIG_BLOB_CONTAINER_KVHASH_LOG_LEVEL);

#define KVHASH_DIR_MAGIC   0x4b564841u /* 'KVHA' */
#define KVHASH_VERSION     2

#define MAX_PAYLOAD        CONFIG_BLOB_DB_MAX_PAYLOAD_LEN
#define DIR_HDR_LEN        8u                          /* magic+n+version+depth */
#define MAX_BUCKETS        ((MAX_PAYLOAD - DIR_HDR_LEN) / 8u)
#define ENTRY_HDR_LEN      4u                          /* klen+vlen */
#define ENTRY_LIMIT        (MAX_PAYLOAD - ENTRY_HDR_LEN)

/* Geometry policy. Constants, not format: a map records what it was built
 * with, so moving these changes only maps created afterwards. */
#define DEFAULT_BUCKETS         8u   /* what "I do not know" builds */
#define SMALL_MAP_LOAD          4u   /* entries per bucket while one level fits */
#define ONE_LEVEL_MAX_BUCKETS   255u /* past this, spend a second level */
#define MIN_BUCKET_BYTES        4096u /* below this, per-blob overhead dominates */
#define NEAR_FULL_PCT           60u  /* warn when a bucket passes this */

BUILD_ASSERT(MAX_BUCKETS >= 2, "MAX_PAYLOAD too small to hold a bucket directory");

/* Reused across calls (single-threaded contract). One holds the directory
 * currently being walked, the other the packed bucket being read/rewritten. */
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

/*
 * One hash, two indices, taken from disjoint halves.
 *
 * The two indices must not be two moduli of the same value: the geometry
 * below is square, so n_top == n_sub is the normal case, and there `h % m`
 * and `h % n` are the same number — every key would land on the diagonal.
 * crc32_ieee is used rather than a hand-rolled hash because tools/sizing.py
 * verifies this distribution offline and zlib.crc32 is the same function, so
 * the check cannot drift from the code.
 */
static inline uint32_t key_hash(const void *key, size_t klen)
{
	return crc32_ieee(key, klen);
}

static inline uint16_t idx_top(uint32_t h, uint16_t n)
{
	return (uint16_t)((h >> 16) % n);
}

static inline uint16_t idx_sub(uint32_t h, uint16_t n)
{
	return (uint16_t)((h & 0xffffu) % n);
}

static inline size_t dir_len(uint16_t n)
{
	return DIR_HDR_LEN + (size_t)n * 8u;
}

static inline uint64_t dir_child(const uint8_t *buf, uint16_t idx)
{
	return get_u64(&buf[DIR_HDR_LEN + (size_t)idx * 8u]);
}

static inline void dir_set_child(uint8_t *buf, uint16_t idx, uint64_t id)
{
	put_u64(&buf[DIR_HDR_LEN + (size_t)idx * 8u], id);
}

/* Format an empty directory of @n children at @depth into @buf. */
static void dir_format(uint8_t *buf, uint16_t n, uint8_t depth)
{
	uint32_t magic = KVHASH_DIR_MAGIC;

	memset(buf, 0, dir_len(n));
	memcpy(&buf[0], &magic, sizeof(magic));
	put_u16(&buf[4], n);
	buf[6] = KVHASH_VERSION;
	buf[7] = depth;
}

/* Load and validate a directory into dir_buf; report its n and depth. */
static int dir_load(uint64_t root, uint16_t *n_out, uint8_t *depth_out)
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
	if (magic != KVHASH_DIR_MAGIC) {
		return -EIO;
	}
	if (dir_buf[6] != KVHASH_VERSION) {
		return -EIO;
	}

	uint16_t n = get_u16(&dir_buf[4]);
	uint8_t depth = dir_buf[7];

	if (n < 2 || n > MAX_BUCKETS || got < dir_len(n)) {
		return -EIO;
	}
	if (depth != 1 && depth != 2) {
		return -EIO;
	}
	*n_out = n;
	if (depth_out) {
		*depth_out = depth;
	}
	return 0;
}

/*
 * Walk from @root to the leaf directory holding @key, leaving that directory
 * in dir_buf. Reports the leaf's own root id (what a fresh bucket must be
 * published into) and the bucket index within it.
 *
 * At depth 2 this reads the top directory, takes the 8 bytes it needs, and
 * reuses dir_buf for the leaf — the top level is never needed again, which is
 * why a second level costs a read rather than a buffer.
 */
static int resolve_leaf(uint64_t root, const void *key, size_t klen,
			uint64_t *leaf_root, uint16_t *leaf_n, uint16_t *idx)
{
	uint32_t h = key_hash(key, klen);
	uint16_t n;
	uint8_t depth;
	int rc = dir_load(root, &n, &depth);

	if (rc != 0) {
		return rc;
	}

	if (depth == 1) {
		*leaf_root = root;
		*leaf_n = n;
		*idx = idx_sub(h, n);
		return 0;
	}

	uint64_t sub = dir_child(dir_buf, idx_top(h, n));

	if (sub == 0) {
		return -EIO; /* sub-maps are created eagerly; 0 is corruption */
	}

	uint8_t sub_depth;

	rc = dir_load(sub, &n, &sub_depth);
	if (rc != 0) {
		return rc;
	}
	if (sub_depth != 1) {
		return -EIO;
	}

	*leaf_root = sub;
	*leaf_n = n;
	*idx = idx_sub(h, n);
	return 0;
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

	while (off + ENTRY_HDR_LEN <= used) {
		size_t kl = get_u16(&buf[off]);
		size_t vl = get_u16(&buf[off + 2]);
		size_t elen = ENTRY_HDR_LEN + kl + vl;

		if (off + elen > used) {
			break; /* truncated / corrupt — stop */
		}
		if (kl == klen && memcmp(&buf[off + ENTRY_HDR_LEN], key, klen) == 0) {
			if (entry_len) {
				*entry_len = elen;
			}
			if (val_off) {
				*val_off = off + ENTRY_HDR_LEN + kl;
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

/*
 * Choose a geometry from the population the caller declared.
 *
 * Small maps stay flat and pack their buckets: write traffic is what makes a
 * bucket count worth spending, and a map of a few hundred entries has none
 * worth optimising, while a second level would cost a blob per sub-map at
 * create. Past ONE_LEVEL_MAX_BUCKETS the map is large enough that write cost
 * dominates, and there the target is about one entry per bucket — a set
 * rewrites its whole bucket, so a fill's write volume is inversely
 * proportional to the bucket count. MIN_BUCKET_BYTES stops that running away
 * for tiny entries, where a per-blob header would cost more than the data.
 *
 * A declared field that cannot be honoured fails here, before anything is
 * written.
 */
static int derive_geometry(const struct map_config *cfg,
			   uint8_t *depth, uint16_t *n_top, uint16_t *n_sub)
{
	size_t entries = cfg ? cfg->expected_entries : 0;
	size_t typical = cfg ? cfg->typical_entry_bytes : 0;
	size_t maxent = cfg ? cfg->max_entry_bytes : 0;

	/* Contradictions and impossibilities: deterministic, so -EINVAL. */
	if (maxent != 0 && maxent > ENTRY_LIMIT) {
		LOG_ERR("max_entry_bytes %zu exceeds the %u a record can hold",
			maxent, (unsigned int)ENTRY_LIMIT);
		return -EINVAL;
	}
	if (typical != 0 && maxent != 0 && typical > maxent) {
		LOG_ERR("typical_entry_bytes %zu exceeds max_entry_bytes %zu",
			typical, maxent);
		return -EINVAL;
	}

	if (entries == 0) {
		*depth = 1;
		*n_top = DEFAULT_BUCKETS;
		*n_sub = 0;
		return 0;
	}

	/* One level, packed: cap the load so a full bucket stays under the
	 * near-full threshold rather than against the ceiling. */
	size_t load = SMALL_MAP_LOAD;

	if (maxent != 0) {
		size_t safe = (size_t)MAX_PAYLOAD * NEAR_FULL_PCT / 100u;
		size_t by_size = safe / maxent;

		if (by_size < load) {
			load = by_size;
		}
		if (load == 0) {
			load = 1;
		}
	}

	size_t want = (entries + load - 1) / load;

	if (want < 2) {
		want = 2;
	}
	if (want <= ONE_LEVEL_MAX_BUCKETS && want <= MAX_BUCKETS) {
		*depth = 1;
		*n_top = (uint16_t)want;
		*n_sub = 0;
		return 0;
	}

	/* Two levels: about one entry per bucket, floored so a bucket is worth
	 * its own blob. */
	size_t buckets = entries;

	if (typical != 0) {
		size_t by_bytes = (entries * typical) / MIN_BUCKET_BYTES;

		if (by_bytes < buckets) {
			buckets = by_bytes;
		}
	}
	if (buckets < ONE_LEVEL_MAX_BUCKETS) {
		buckets = ONE_LEVEL_MAX_BUCKETS;
	}

	/* Square fan-out minimises the metadata a lookup touches. */
	size_t side = 1;

	while (side * side < buckets) {
		side++;
	}
	if (side < 2) {
		side = 2;
	}
	if (side > MAX_BUCKETS) {
		LOG_ERR("population of %zu entries needs %zu buckets per level, "
			"past the %u this payload allows",
			entries, side, (unsigned int)MAX_BUCKETS);
		return -EINVAL;
	}

	*depth = 2;
	*n_top = (uint16_t)side;
	*n_sub = (uint16_t)side;
	return 0;
}

static void fill_info(struct map_info *out, uint8_t depth, uint16_t n_top,
		      uint16_t n_sub)
{
	if (out == NULL) {
		return;
	}
	out->depth = depth;
	out->fanout = (depth == 2) ? n_top : 0;
	out->buckets = (depth == 2) ? (uint32_t)n_top * n_sub : n_top;
	out->entry_bytes_limit = ENTRY_LIMIT;
}

/* ---- map_ops ---- */

static int kvhash_create(uint64_t root, const struct map_config *cfg)
{
	uint8_t depth;
	uint16_t n_top, n_sub;
	int rc = derive_geometry(cfg, &depth, &n_top, &n_sub);

	if (rc != 0) {
		return rc; /* nothing written: validation is arithmetic */
	}

	if (depth == 1) {
		dir_format(dir_buf, n_top, 1);
		rc = blob_db_update(root, dir_buf, dir_len(n_top));
		if (rc == 0) {
			LOG_DBG("created map root=%llu depth=1 buckets=%u",
				(unsigned long long)root, n_top);
		}
		return rc;
	}

	/*
	 * Two levels. Every sub-map is the same empty image, so build it once
	 * in bkt_buf and write it to each allocated id, accumulating those ids
	 * into the top directory in dir_buf. The top is bound LAST: until that
	 * single write lands the sub-maps are unreachable and the next boot
	 * simply builds the map again. That is the same one-shot window
	 * blob_db has for any multi-blob structure (FINDINGS.md B8) — it is
	 * paid once per map, not once per operation.
	 */
	dir_format(dir_buf, n_top, 2);
	dir_format(bkt_buf, n_sub, 1);

	for (uint16_t i = 0; i < n_top; i++) {
		uint64_t sub = blob_db_alloc_id();

		if (sub == 0) {
			return -ENOSPC;
		}
		rc = blob_db_update(sub, bkt_buf, dir_len(n_sub));
		if (rc != 0) {
			return rc;
		}
		dir_set_child(dir_buf, i, sub);
	}

	rc = blob_db_update(root, dir_buf, dir_len(n_top)); /* commit point */
	if (rc == 0) {
		LOG_DBG("created map root=%llu depth=2 fanout=%u buckets=%u",
			(unsigned long long)root, n_top,
			(unsigned int)n_top * n_sub);
	}
	return rc;
}

static int kvhash_stat(uint64_t root, struct map_info *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	uint16_t n;
	uint8_t depth;
	int rc = dir_load(root, &n, &depth);

	if (rc != 0) {
		return rc;
	}

	if (depth == 1) {
		fill_info(out, 1, n, 0);
		return 0;
	}

	/* Sub-maps are uniform, so any one of them reports n_sub. */
	uint64_t sub = dir_child(dir_buf, 0);
	uint16_t n_sub;
	uint8_t sub_depth;

	if (sub == 0) {
		return -EIO;
	}
	rc = dir_load(sub, &n_sub, &sub_depth);
	if (rc != 0) {
		return rc;
	}
	fill_info(out, 2, n, n_sub);
	return 0;
}

static int kvhash_get(uint64_t root, const void *key, size_t klen,
		      void *out, size_t out_sz, size_t *out_len)
{
	if (key == NULL || klen == 0 || klen > 0xffffu) {
		return -EINVAL;
	}

	uint64_t leaf;
	uint16_t leaf_n, idx;
	int rc = resolve_leaf(root, key, klen, &leaf, &leaf_n, &idx);

	if (rc != 0) {
		return rc;
	}

	uint64_t bid = dir_child(dir_buf, idx);

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

	uint64_t leaf;
	uint16_t leaf_n, idx;
	int rc = resolve_leaf(root, key, klen, &leaf, &leaf_n, &idx);

	if (rc != 0) {
		return rc;
	}

	uint64_t bid = dir_child(dir_buf, idx);
	size_t used = 0;
	size_t was = 0;

	if (bid != 0) {
		rc = blob_db_get(bid, bkt_buf, sizeof(bkt_buf), &used);
		if (rc != 0) {
			return rc;
		}
		was = used;

		/* Drop any existing entry for this key (in-place compaction). */
		size_t off, elen;

		off = bkt_find(bkt_buf, used, key, klen, &elen, NULL, NULL);
		if (off != SIZE_MAX) {
			memmove(&bkt_buf[off], &bkt_buf[off + elen], used - off - elen);
			used -= elen;
		}
	}

	size_t need = ENTRY_HDR_LEN + klen + vlen;

	if (used + need > sizeof(bkt_buf)) {
		return -ENOSPC;
	}

	put_u16(&bkt_buf[used], (uint16_t)klen);
	put_u16(&bkt_buf[used + 2], (uint16_t)vlen);
	memcpy(&bkt_buf[used + ENTRY_HDR_LEN], key, klen);
	if (vlen) {
		memcpy(&bkt_buf[used + ENTRY_HDR_LEN + klen], val, vlen);
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
		/* Publish the new bucket into its leaf directory. A crash
		 * between the bucket write above and this update leaves an
		 * unreferenced blob (reclaimed by a later format), never a
		 * corrupt map. At depth 2 the directory rewritten here is the
		 * sub-map's, not the top's — the top is never written again
		 * after create. */
		dir_set_child(dir_buf, idx, bid);
		rc = blob_db_update(leaf, dir_buf, dir_len(leaf_n));
		if (rc != 0) {
			return rc;
		}
	}

	/*
	 * Near-full warning. Free: this bucket was just read and rewritten, so
	 * its size is already known. It matters because a map that cannot grow
	 * has no recovery from -ENOSPC — the bucket count is fixed at create
	 * and there is no iteration to copy the data out with — so the useful
	 * moment to react is while there is still room, not at the failure.
	 */
	const size_t near_full = (size_t)MAX_PAYLOAD * NEAR_FULL_PCT / 100u;

	if (used >= near_full) {
		if (was < near_full) {
			LOG_WRN("bucket %u of map %llu is %u%% full (%zu of %u B) "
				"— sized for fewer or larger entries than it holds",
				idx, (unsigned long long)leaf,
				(unsigned int)(used * 100u / MAX_PAYLOAD),
				used, (unsigned int)MAX_PAYLOAD);
		}
		return 1;
	}
	return 0;
}

static int kvhash_del(uint64_t root, const void *key, size_t klen)
{
	if (key == NULL || klen == 0 || klen > 0xffffu) {
		return -EINVAL;
	}

	uint64_t leaf;
	uint16_t leaf_n, idx;
	int rc = resolve_leaf(root, key, klen, &leaf, &leaf_n, &idx);

	if (rc != 0) {
		return rc;
	}

	uint64_t bid = dir_child(dir_buf, idx);

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

const struct map_ops kvhash_map_ops = {
	.create = kvhash_create,
	.stat = kvhash_stat,
	.get = kvhash_get,
	.set = kvhash_set,
	.del = kvhash_del,
};
