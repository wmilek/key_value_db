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
 *     (magic 'KVHD' instead means a destroy has committed and the children are
 *      still being released -- see kvhash_destroy)
 *   bucket blob (packed pair list):
 *     ( [u16 klen|KS] [u16 vlen|VS] [key part] [value part] )*
 *       key part   KS=0: key bytes         KS=1: [u32 key_fp] [u64 key_id]
 *       value part VS=0: value bytes       VS=1: [u64 val_id]
 *     KS/VS are bit 15 of the length field; the low 15 bits are always the
 *     true length, wherever the bytes live. A spilled key or value is a blob
 *     holding exactly those bytes. (v3; a v2 bucket is the same with both
 *     bits always clear — see "Spilling" below.)
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
 * Spilling. A key longer than key_inline_max or a value longer than
 * val_inline_max is written to a blob of its own (v3 maps only). The
 * thresholds are a write policy, not a format: every entry says where its
 * parts live, every read goes by that alone, and the thresholds are consulted
 * in exactly one place — when kvhash_set builds a new entry. So they can
 * change between builds, or at run time, and every entry written under an
 * earlier value stays readable; it moves to the current placement only when
 * its key is next set. See doc/proposals/2026-10-09-kvhash-spill.md.
 *
 * The flag bits are free in every v2 bucket because blob_db refuses at mount
 * any payload cap above ~32 KB (BUILD_ASSERTed below), so no inline length
 * reaches 0x8000. v2 maps stay readable and writable by this code but are
 * never spilled: their version byte still tells v2 firmware it may parse them.
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
#define KVHASH_DIR_DYING   0x4b564844u /* 'KVHD' — destroy committed, release pending */
#define KVHASH_VERSION     3   /* what create writes: entries may spill */
#define KVHASH_VERSION_MIN 2   /* oldest still served: never spilled */

#define MAX_PAYLOAD        CONFIG_BLOB_DB_MAX_PAYLOAD_LEN
#define DIR_HDR_LEN        8u                          /* magic+n+version+depth */
#define MAX_BUCKETS        ((MAX_PAYLOAD - DIR_HDR_LEN) / 8u)
#define ENTRY_HDR_LEN      4u                          /* klen+vlen */
#define ENTRY_LIMIT        (MAX_PAYLOAD - ENTRY_HDR_LEN)
#define ENT_SPILL          0x8000u  /* in klen / vlen: the part is a blob */
#define ENT_LEN_MASK       0x7fffu  /* the true length */
#define KEY_REF_LEN        12u      /* spilled key part: u32 fp + u64 id */
#define VAL_REF_LEN        8u       /* spilled value part: u64 id */
#define KEY_CHUNK          64u      /* stack buffer for streamed key compares */

/* Geometry policy. Constants, not format: a map records what it was built
 * with, so moving these changes only maps created afterwards. */
#define DEFAULT_BUCKETS         8u   /* what "I do not know" builds */
#define SMALL_MAP_LOAD          4u   /* entries per bucket while one level fits */
#define ONE_LEVEL_MAX_BUCKETS   255u /* past this, spend a second level */
#define MIN_BUCKET_BYTES        4096u /* below this, per-blob overhead dominates */
#define BUCKET_FILL_PCT         60u  /* of a payload a full bucket should reach */

BUILD_ASSERT(MAX_BUCKETS >= 2, "MAX_PAYLOAD too small to hold a bucket directory");
BUILD_ASSERT(MAX_PAYLOAD <= ENT_LEN_MASK,
	     "an inline length must leave bit 15 free for the spill flag "
	     "(blob_db cannot mount a payload cap this large anyway)");

/* Write policy (see "Spilling"). Read by kvhash_set only. */
static size_t key_inline_max = CONFIG_BLOB_CONTAINER_KVHASH_KEY_INLINE_MAX;
static size_t val_inline_max = CONFIG_BLOB_CONTAINER_KVHASH_VAL_INLINE_MAX;

#if defined(CONFIG_BLOB_CONTAINER_KVHASH_TEST_HOOKS)
uint32_t kvhash_test_fp_mask = 0xffffffffu;
#define FP_MASK kvhash_test_fp_mask
#else
#define FP_MASK 0xffffffffu
#endif

/*
 * Every flash write kvhash makes goes through these two, so a test can cut
 * power between any two of them (kvhash_test_cut_after). blob_db makes each
 * write atomic, so "between writes" is every state a power cut can leave.
 */
#if defined(CONFIG_BLOB_CONTAINER_KVHASH_TEST_HOOKS)
int kvhash_test_cut_after = -1;
bool kvhash_test_cut_fired;

static int power_cut(void)
{
	if (kvhash_test_cut_after < 0) {
		return 0;
	}
	if (kvhash_test_cut_after == 0) {
		kvhash_test_cut_fired = true;
		return -EINTR; /* power is off: this and every later write */
	}
	kvhash_test_cut_after--;
	return 0;
}
#else
static inline int power_cut(void)
{
	return 0;
}
#endif

static int kv_update(uint64_t id, const void *payload, size_t len)
{
	int rc = power_cut();

	return rc != 0 ? rc : blob_db_update(id, payload, len);
}

static int kv_delete(uint64_t id)
{
	int rc = power_cut();

	return rc != 0 ? rc : blob_db_delete(id);
}

void kvhash_set_inline_max(size_t key_max, size_t val_max)
{
	key_inline_max = MIN(key_max, (size_t)ENT_LEN_MASK);
	val_inline_max = MIN(val_max, (size_t)ENT_LEN_MASK);
}

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

static inline uint32_t get_u32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

static inline void put_u32(uint8_t *p, uint32_t v)
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

/*
 * A key's fingerprint, kept in a spilled key's entry so a lookup reads the
 * key blob only on a likely match. It is format: fixed for v3, never
 * configurable. CRC-32C rather than the bucket hash, because every key in a
 * bucket shares the bucket hash bits that chose it.
 */
static inline uint32_t key_fp(const void *key, size_t klen)
{
	return crc32_c(0, key, klen, true, true) & FP_MASK;
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

/*
 * Load and validate a directory into dir_buf; report its n, its depth, and
 * whether a destroy has already committed on this root.
 */
static int dir_load_raw(uint64_t root, uint16_t *n_out, uint8_t *depth_out,
			bool *dying)
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
	if (dir_buf[6] < KVHASH_VERSION_MIN || dir_buf[6] > KVHASH_VERSION) {
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
 * The data-path load. A root whose destroy has committed is gone as far as
 * every reader is concerned (l2_containers.md 2.4), so the whole container
 * answers -ENOENT and no caller sees the part of it still on flash.
 */
static int dir_load(uint64_t root, uint16_t *n_out, uint8_t *depth_out)
{
	bool dying = false;
	int rc = dir_load_raw(root, n_out, depth_out, &dying);

	if (rc != 0) {
		return rc;
	}
	return dying ? -ENOENT : 0;
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
 * One entry of a packed bucket, as the parser reports it.
 */
struct ent {
	size_t len;       /* stored length, header included */
	size_t kl, vl;    /* true key and value lengths */
	bool kspill;      /* key part is [u32 fp][u64 id] */
	bool vspill;      /* value part is [u64 id] */
	size_t kpos;      /* offset of the key part in the bucket */
	size_t vpos;      /* offset of the value part in the bucket */
};

/*
 * The one parser of a packed bucket. Reports the entry at @off and true, or
 * false at the end of the bucket. A truncated tail -- an entry that runs past
 * @used -- ends the bucket, and this is the only place that rule lives, so
 * get, next and count can never disagree about what a damaged record holds.
 * The flags decide the stored size; no threshold is consulted here.
 *
 *   for (off = 0; bkt_entry(buf, used, off, &e); off += e.len)
 */
static bool bkt_entry(const uint8_t *buf, size_t used, size_t off, struct ent *e)
{
	if (off + ENTRY_HDR_LEN > used) {
		return false;
	}

	uint16_t kf = get_u16(&buf[off]);
	uint16_t vf = get_u16(&buf[off + 2]);

	e->kspill = (kf & ENT_SPILL) != 0;
	e->vspill = (vf & ENT_SPILL) != 0;
	e->kl = kf & ENT_LEN_MASK;
	e->vl = vf & ENT_LEN_MASK;
	e->kpos = off + ENTRY_HDR_LEN;
	e->vpos = e->kpos + (e->kspill ? KEY_REF_LEN : e->kl);
	e->len = ENTRY_HDR_LEN + (e->kspill ? KEY_REF_LEN : e->kl) +
		 (e->vspill ? VAL_REF_LEN : e->vl);
	return off + e->len <= used;
}

static inline uint32_t ent_fp(const uint8_t *buf, const struct ent *e)
{
	return e->kspill ? get_u32(&buf[e->kpos]) : key_fp(&buf[e->kpos], e->kl);
}

static inline uint64_t ent_key_id(const uint8_t *buf, const struct ent *e)
{
	return e->kspill ? get_u64(&buf[e->kpos + 4]) : 0;
}

static inline uint64_t ent_val_id(const uint8_t *buf, const struct ent *e)
{
	return e->vspill ? get_u64(&buf[e->vpos]) : 0;
}

/* A key's bytes, wherever they live: in RAM (@p), or in a spilled key blob. */
struct key_src {
	const uint8_t *p;
	uint64_t id;
};

static inline struct key_src ent_key(const uint8_t *buf, const struct ent *e)
{
	struct key_src k = { NULL, 0 };

	if (e->kspill) {
		k.id = get_u64(&buf[e->kpos + 4]);
	} else {
		k.p = &buf[e->kpos];
	}
	return k;
}

/* Copy @n bytes of key @k from @pos into @out. A spilled key that is shorter
 * than its entry says, or has a zero id, is a damaged map: -EIO. */
static int key_chunk(const struct key_src *k, size_t pos, uint8_t *out, size_t n)
{
	if (k->p != NULL) {
		memcpy(out, &k->p[pos], n);
		return 0;
	}
	if (k->id == 0) {
		return -EIO;
	}

	size_t got = 0;
	int rc = blob_db_read(k->id, pos, out, n, &got);

	if (rc != 0) {
		return rc == -ENOENT ? -EIO : rc;
	}
	return got == n ? 0 : -EIO;
}

/*
 * Compare the bytes of two keys of the same length @len, either of which may
 * be spilled; *cmp gets the memcmp sign. With @scratch (at least MAX_PAYLOAD
 * bytes, free to clobber) a spilled side is read whole in one blob read, which
 * is the lookup path; without it both are streamed through the stack in
 * KEY_CHUNK pieces, which is the rare fingerprint-tie path of the order.
 */
static int key_cmp_bytes(const struct key_src *a, const struct key_src *b,
			 size_t len, uint8_t *scratch, int *cmp)
{
	if (a->p != NULL && b->p != NULL) {
		*cmp = memcmp(a->p, b->p, len);
		return 0;
	}

	if (scratch != NULL && len <= MAX_PAYLOAD && (a->p != NULL || b->p != NULL)) {
		const struct key_src *ram = (a->p != NULL) ? a : b;
		const struct key_src *blob = (a->p != NULL) ? b : a;
		int rc = key_chunk(blob, 0, scratch, len);

		if (rc != 0) {
			return rc;
		}
		*cmp = memcmp(a->p != NULL ? ram->p : scratch,
			      a->p != NULL ? scratch : ram->p, len);
		return 0;
	}

	uint8_t ca[KEY_CHUNK], cb[KEY_CHUNK];

	for (size_t pos = 0; pos < len; pos += KEY_CHUNK) {
		size_t n = MIN((size_t)KEY_CHUNK, len - pos);
		int rc = key_chunk(a, pos, ca, n);

		if (rc == 0) {
			rc = key_chunk(b, pos, cb, n);
		}
		if (rc != 0) {
			return rc;
		}
		*cmp = memcmp(ca, cb, n);
		if (*cmp != 0) {
			return 0;
		}
	}
	*cmp = 0;
	return 0;
}

/*
 * Scan a packed bucket for @key (fingerprint @fp). On a hit, returns 0 and
 * the entry's offset and parse; on a miss, -ENOENT; or a read error from a
 * spilled key. A key may be stored either way whatever the thresholds say now
 * -- it was placed by the policy in force when it was last set -- so both
 * placements are checked. A spilled key costs a blob read only when its
 * length and fingerprint both match. @scratch: see key_cmp_bytes.
 */
static int bkt_find(const uint8_t *buf, size_t used, const void *key,
		    size_t klen, uint32_t fp, uint8_t *scratch,
		    size_t *off_out, struct ent *hit)
{
	const struct key_src want = { key, 0 };
	struct ent e;

	for (size_t off = 0; bkt_entry(buf, used, off, &e); off += e.len) {
		if (e.kl != klen) {
			continue;
		}
		if (!e.kspill) {
			if (memcmp(&buf[e.kpos], key, klen) != 0) {
				continue;
			}
		} else {
			if (get_u32(&buf[e.kpos]) != fp) {
				continue;
			}

			struct key_src have = ent_key(buf, &e);
			int cmp;
			int rc = key_cmp_bytes(&have, &want, klen, scratch, &cmp);

			if (rc != 0) {
				return rc;
			}
			if (cmp != 0) {
				continue;
			}
		}
		*off_out = off;
		*hit = e;
		return 0;
	}
	return -ENOENT;
}

/* Delete a blob an entry no longer references. Past the commit, so a failure
 * is a leak (logged), not an error of the operation. */
static void release_part(uint64_t id)
{
	if (id == 0) {
		return;
	}

	int rc = kv_delete(id);

	if (rc != 0 && rc != -ENOENT) {
		LOG_WRN("spilled blob %llu not released: %d", (unsigned long long)id, rc);
	}
}

/*
 * Load the depth-1 sub-map @sub of a two-level map into dir_buf. Sub-maps are
 * created eagerly, so an id of 0 is corruption, as is anything but depth 1.
 */
static int sub_load(uint64_t sub, uint16_t *sub_n)
{
	uint8_t sub_depth;
	int rc;

	if (sub == 0) {
		return -EIO;
	}
	rc = dir_load(sub, sub_n, &sub_depth);
	if (rc != 0) {
		return rc;
	}
	return sub_depth == 1 ? 0 : -EIO;
}

/*
 * Re-fetch child @t of the top directory at @root after a sub-map has been
 * loaded over it. A whole directory read: blob_db stages an inline payload
 * in full before any partial copy, so an 8-byte blob_db_read would cost the
 * same flash traffic, and a third payload-sized buffer is what keeping the
 * top would cost in RAM.
 */
static int top_child(uint64_t root, uint16_t t, uint64_t *sub)
{
	uint16_t n;
	int rc = dir_load(root, &n, NULL);

	if (rc != 0) {
		return rc;
	}
	if (t >= n) {
		return -EIO;
	}
	*sub = dir_child(dir_buf, t);
	return 0;
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

	/* One level, packed: cap the load so a full bucket sits comfortably
	 * inside a payload rather than against the ceiling. The margin is what
	 * absorbs a population whose entries run larger than declared. */
	size_t load = SMALL_MAP_LOAD;

	if (maxent != 0) {
		size_t safe = (size_t)MAX_PAYLOAD * BUCKET_FILL_PCT / 100u;
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
		rc = kv_update(root, dir_buf, dir_len(n_top));
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
		rc = kv_update(sub, bkt_buf, dir_len(n_sub));
		if (rc != 0) {
			return rc;
		}
		dir_set_child(dir_buf, i, sub);
	}

	rc = kv_update(root, dir_buf, dir_len(n_top)); /* commit point */
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

/*
 * Copy an entry's value out, wherever it lives. @out_len gets the true length
 * even on -ENOMEM. A spilled value whose blob disagrees with the entry's
 * length is a damaged map: -EIO.
 */
static int ent_value(const uint8_t *buf, const struct ent *e,
		     void *out, size_t out_sz, size_t *out_len)
{
	if (out_len) {
		*out_len = e->vl;
	}
	if (e->vl > out_sz) {
		return -ENOMEM;
	}
	if (!e->vspill) {
		if (e->vl) {
			memcpy(out, &buf[e->vpos], e->vl);
		}
		return 0;
	}

	uint64_t vid = ent_val_id(buf, e);
	size_t got = 0;
	int rc;

	if (vid == 0) {
		return -EIO;
	}
	rc = blob_db_get(vid, out, out_sz, &got);
	if (rc != 0) {
		return (rc == -ENOENT || rc == -ENOMEM) ? -EIO : rc;
	}
	return got == e->vl ? 0 : -EIO;
}

static int kvhash_get(uint64_t root, const void *key, size_t klen,
		      void *out, size_t out_sz, size_t *out_len)
{
	if (key == NULL || klen == 0 || klen > 0xffffu) {
		return -EINVAL;
	}
	if (klen > ENT_LEN_MASK) {
		return -ENOENT; /* longer than any key that can be stored */
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

	/* dir_buf is done with: it is the scratch for a spilled key. */
	size_t off;
	struct ent e;

	rc = bkt_find(bkt_buf, used, key, klen, key_fp(key, klen), dir_buf,
		      &off, &e);
	if (rc != 0) {
		return rc;
	}
	return ent_value(bkt_buf, &e, out, out_sz, out_len);
}

/*
 * Set. The one place the thresholds are read: the new entry is built under
 * the policy in force now, whatever placement the old one had.
 *
 *   spilled value, same placement, same length:
 *       blob_db_update(val_id)                the whole mutation
 *   otherwise:
 *       1. bind fresh blobs for any spilled part   unreferenced so far
 *       2. rewrite the bucket                      COMMIT
 *          (+ publish a fresh bucket, as before)
 *       3. delete the old entry's blobs the new one does not reuse
 *
 * A spilled key that stays spilled keeps its blob: the bytes are the same.
 * A crash before the commit leaves the old entry and, at worst, fresh blobs
 * nothing references; after it, the new entry and, at worst, old blobs nothing
 * references. Never a dangling id, never a mixed entry. A failure before the
 * commit releases the fresh blobs it bound.
 */
static int kvhash_set(uint64_t root, const void *key, size_t klen,
		      const void *val, size_t vlen)
{
	if (key == NULL || klen == 0 || klen > 0xffffu || vlen > 0xffffu ||
	    (val == NULL && vlen != 0)) {
		return -EINVAL;
	}
	if (klen > ENT_LEN_MASK || vlen > ENT_LEN_MASK) {
		return -ENOSPC; /* the length field has 15 bits */
	}

	uint64_t leaf;
	uint16_t leaf_n, idx;
	int rc = resolve_leaf(root, key, klen, &leaf, &leaf_n, &idx);

	if (rc != 0) {
		return rc;
	}

	/* The leaf directory's version decides whether spilling is allowed:
	 * a v2 map must stay parseable by v2 firmware. */
	bool can_spill = dir_buf[6] >= 3;
	bool kspill = can_spill && klen > key_inline_max;
	bool vspill = can_spill && vlen > val_inline_max;

	if (kspill && klen > MAX_PAYLOAD) {
		return -ENOSPC; /* a spilled key is one slot, so a lookup can hold it */
	}

	uint32_t fp = key_fp(key, klen);
	uint64_t bid = dir_child(dir_buf, idx);
	size_t used = 0;
	bool have_old = false;
	struct ent old;
	uint64_t old_kid = 0, old_vid = 0;

	if (bid != 0) {
		rc = blob_db_get(bid, bkt_buf, sizeof(bkt_buf), &used);
		if (rc != 0) {
			return rc;
		}

		/* The bucket exists, so dir_buf is not needed again: it is the
		 * scratch for a spilled key. */
		size_t off;

		rc = bkt_find(bkt_buf, used, key, klen, fp, dir_buf, &off, &old);
		if (rc == 0) {
			have_old = true;
			old_kid = ent_key_id(bkt_buf, &old);
			old_vid = ent_val_id(bkt_buf, &old);

			if (old.kspill == kspill && old.vspill && vspill &&
			    old.vl == vlen) {
				if (old_vid == 0) {
					return -EIO;
				}
				return kv_update(old_vid, val, vlen);
			}

			/* Drop the old entry (in-place compaction). */
			memmove(&bkt_buf[off], &bkt_buf[off + old.len],
				used - off - old.len);
			used -= old.len;
		} else if (rc != -ENOENT) {
			return rc;
		}
	}

	size_t need = ENTRY_HDR_LEN + (kspill ? KEY_REF_LEN : klen) +
		      (vspill ? VAL_REF_LEN : vlen);

	if (used + need > sizeof(bkt_buf)) {
		return -ENOSPC;
	}

	/* 1. Bind fresh blobs for the spilled parts. */
	uint64_t kid = (kspill && have_old && old.kspill) ? old_kid : 0;
	uint64_t vid = 0;
	bool fresh_kid = false;

	if (kspill && kid == 0) {
		kid = blob_db_alloc_id();
		if (kid == 0) {
			return -EIO;
		}
		rc = kv_update(kid, key, klen);
		if (rc != 0) {
			return rc;
		}
		fresh_kid = true;
	}
	if (vspill) {
		vid = blob_db_alloc_id();
		if (vid == 0) {
			rc = -EIO;
			goto undo;
		}
		rc = kv_update(vid, val, vlen);
		if (rc != 0) {
			vid = 0;
			goto undo;
		}
	}

	/* 2. Append the new entry and commit the bucket. */
	put_u16(&bkt_buf[used], (uint16_t)(klen | (kspill ? ENT_SPILL : 0)));
	put_u16(&bkt_buf[used + 2], (uint16_t)(vlen | (vspill ? ENT_SPILL : 0)));

	size_t pos = used + ENTRY_HDR_LEN;

	if (kspill) {
		put_u32(&bkt_buf[pos], fp);
		put_u64(&bkt_buf[pos + 4], kid);
		pos += KEY_REF_LEN;
	} else {
		memcpy(&bkt_buf[pos], key, klen);
		pos += klen;
	}
	if (vspill) {
		put_u64(&bkt_buf[pos], vid);
	} else if (vlen) {
		memcpy(&bkt_buf[pos], val, vlen);
	}
	used += need;

	bool fresh_bucket = (bid == 0);

	if (fresh_bucket) {
		bid = blob_db_alloc_id();
		if (bid == 0) {
			rc = -EIO;
			goto undo;
		}
	}

	rc = kv_update(bid, bkt_buf, used);
	if (rc != 0) {
		goto undo;
	}

	if (fresh_bucket) {
		/* Publish the new bucket into its leaf directory. A crash
		 * between the bucket write above and this update leaves an
		 * unreferenced blob (reclaimed by a later format), never a
		 * corrupt map. At depth 2 the directory rewritten here is the
		 * sub-map's, not the top's — the top is never written again
		 * after create. */
		dir_set_child(dir_buf, idx, bid);
		rc = kv_update(leaf, dir_buf, dir_len(leaf_n));
		if (rc != 0) {
			goto undo;
		}
	}

	/* 3. Committed: release what only the old entry referenced. */
	if (old_kid != kid) {
		release_part(old_kid);
	}
	release_part(old_vid);
	return 0;

undo:
	release_part(vid);
	if (fresh_kid) {
		release_part(kid);
	}
	return rc;
}

/* Delete: rewrite the bucket without the entry (commit), then release its
 * spilled blobs. */
static int kvhash_del(uint64_t root, const void *key, size_t klen)
{
	if (key == NULL || klen == 0 || klen > 0xffffu) {
		return -EINVAL;
	}
	if (klen > ENT_LEN_MASK) {
		return -ENOENT; /* longer than any key that can be stored */
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

	size_t off;
	struct ent e;

	rc = bkt_find(bkt_buf, used, key, klen, key_fp(key, klen), dir_buf,
		      &off, &e);
	if (rc != 0) {
		return rc;
	}

	uint64_t kid = ent_key_id(bkt_buf, &e);
	uint64_t vid = ent_val_id(bkt_buf, &e);

	memmove(&bkt_buf[off], &bkt_buf[off + e.len], used - off - e.len);
	used -= e.len;

	rc = kv_update(bid, bkt_buf, used);
	if (rc != 0) {
		return rc;
	}
	release_part(kid);
	release_part(vid);
	return 0;
}

/*
 * Enumeration order, per shape_map.h `next`.
 *
 * A key's place is (top index, sub index, klen, fingerprint, key bytes). The
 * first two are where the key lives, so walking directories and buckets in
 * index order is walking the order; the rest break ties inside one bucket.
 * Every term is a function of the key and the fixed geometry -- never of
 * where the entry sits in its bucket (a set moves its entry to the end), and
 * never of whether the key is spilled, so a threshold change cannot reorder a
 * walk. The fingerprint comes before the bytes so that a spilled key's blob is
 * read only on a full fingerprint tie, which is what keeps a walk from
 * reading every spilled key it passes. For an inline key it is computed on
 * the fly: CPU, no reads.
 */
struct okey {
	size_t kl;
	uint32_t fp;
	struct key_src src;
};

static int okey_cmp(const struct okey *a, const struct okey *b, int *cmp)
{
	if (a->kl != b->kl) {
		*cmp = a->kl < b->kl ? -1 : 1;
		return 0;
	}
	if (a->fp != b->fp) {
		*cmp = a->fp < b->fp ? -1 : 1;
		return 0;
	}
	return key_cmp_bytes(&a->src, &b->src, a->kl, NULL, cmp);
}

static inline struct okey ent_okey(const uint8_t *buf, const struct ent *e)
{
	struct okey k = { e->kl, ent_fp(buf, e), ent_key(buf, e) };

	return k;
}

/*
 * Find the first entry of the bucket in @buf, in enumeration order, that sorts
 * after @cursor (or the first outright when @cursor is NULL). The bucket is
 * unsorted on flash, so this is a linear pass -- the whole bucket is already in
 * RAM, which is the same cost bkt_find pays. Returns 0 and the entry's offset,
 * -ENODATA when none qualifies, or a read error from a fingerprint tie.
 */
static int bkt_first_after(const uint8_t *buf, size_t used,
			   const struct okey *cursor, size_t *best_off)
{
	struct ent e, best_e;
	bool found = false;

	for (size_t off = 0; bkt_entry(buf, used, off, &e); off += e.len) {
		struct okey k = ent_okey(buf, &e);
		int cmp, rc;

		if (cursor != NULL) {
			rc = okey_cmp(&k, cursor, &cmp);
			if (rc != 0) {
				return rc;
			}
			if (cmp <= 0) {
				continue;
			}
		}
		if (found) {
			struct okey b = ent_okey(buf, &best_e);

			rc = okey_cmp(&k, &b, &cmp);
			if (rc != 0) {
				return rc;
			}
			if (cmp >= 0) {
				continue;
			}
		}
		*best_off = off;
		best_e = e;
		found = true;
	}
	return found ? 0 : -ENODATA;
}

/*
 * Search the leaf directory in dir_buf from bucket @start onwards. The cursor
 * applies to bucket @start only: every later bucket sorts wholly after it.
 * Leaves the hit's bucket in bkt_buf and its offset in *off.
 */
static int leaf_first_after(uint16_t n, uint16_t start,
			    const struct okey *cursor, size_t *off)
{
	for (uint16_t i = start; i < n; i++) {
		uint64_t bid = dir_child(dir_buf, i);

		if (bid == 0) {
			continue;
		}

		size_t used = 0;
		int rc = blob_db_get(bid, bkt_buf, sizeof(bkt_buf), &used);

		if (rc != 0) {
			return rc;
		}

		rc = bkt_first_after(bkt_buf, used, i == start ? cursor : NULL, off);
		if (rc != -ENODATA) {
			return rc;
		}
	}
	return -ENODATA;
}

/* Search a whole two-level map from sub-map @top_start onwards. */
static int tree_first_after(uint64_t root, uint16_t n_top, uint16_t top_start,
			    const struct okey *cursor, uint32_t ch, size_t *off)
{
	for (uint16_t t = top_start; t < n_top; t++) {
		/* Walking a sub-map loads its directory over the top one, so
		 * the top is re-read on each step across a sub-map boundary. */
		uint64_t sub = dir_child(dir_buf, t);
		uint16_t sub_n;
		int rc;

		if (t != top_start) {
			rc = top_child(root, t, &sub);
			if (rc != 0) {
				return rc;
			}
		}
		rc = sub_load(sub, &sub_n);
		if (rc != 0) {
			return rc;
		}

		bool here = (t == top_start && cursor != NULL);

		rc = leaf_first_after(sub_n, here ? idx_sub(ch, sub_n) : 0,
				      here ? cursor : NULL, off);
		if (rc != -ENODATA) {
			return rc;
		}
	}
	return -ENODATA;
}

static int kvhash_next(uint64_t root, const void *key, size_t klen,
		       void *kout, size_t kout_sz, size_t *kout_len,
		       void *vout, size_t vout_sz, size_t *vout_len)
{
	if ((key == NULL && klen != 0) || klen > 0xffffu ||
	    (kout == NULL && kout_sz != 0) || (vout == NULL && vout_sz != 0)) {
		return -EINVAL;
	}

	/* klen == 0 starts the walk: get/set reject empty keys, so no stored
	 * key can be confused with the start. */
	struct okey cur = { klen, 0, { key, 0 } };
	const struct okey *cursor = NULL;
	uint32_t ch = 0;

	if (klen != 0) {
		cur.fp = key_fp(key, klen);
		cursor = &cur;
		ch = key_hash(key, klen);
	}

	uint16_t n;
	uint8_t depth;
	size_t off = 0;
	int rc = dir_load(root, &n, &depth);

	if (rc != 0) {
		return rc;
	}

	if (depth == 1) {
		rc = leaf_first_after(n, cursor ? idx_sub(ch, n) : 0, cursor, &off);
	} else {
		rc = tree_first_after(root, n, cursor ? idx_top(ch, n) : 0,
				      cursor, ch, &off);
	}
	if (rc != 0) {
		return rc;
	}

	struct ent e;

	if (!bkt_entry(bkt_buf, SIZE_MAX, off, &e)) {
		return -EIO;
	}
	if (kout_len) {
		*kout_len = e.kl;
	}
	if (vout_len) {
		*vout_len = e.vl;
	}
	if (e.kl > kout_sz || e.vl > vout_sz) {
		return -ENOMEM;
	}

	struct key_src k = ent_key(bkt_buf, &e);

	rc = key_chunk(&k, 0, kout, e.kl);
	if (rc != 0) {
		return rc;
	}
	return ent_value(bkt_buf, &e, vout, vout_sz, NULL);
}

/*
 * Count, per shape_map.h `count`.
 *
 * Exactness needs no help from the write path: every set and del commits in
 * one atomic bucket update, and a fresh bucket is published by a directory
 * update only after its contents are written. So what a walk of the
 * directories finds is always exactly what get answers for, whichever of
 * those steps a power cut fell between. A fresh bucket orphaned by a cut is
 * unreachable from the directory, so it is invisible to get and to the count
 * alike.
 *
 * Entries are parsed by bkt_entry, the same parser get uses, so the count
 * cannot disagree with get on a damaged bucket either.
 */
static size_t bkt_entries(const uint8_t *buf, size_t used)
{
	struct ent e;
	size_t n = 0;

	for (size_t off = 0; bkt_entry(buf, used, off, &e); off += e.len) {
		n++;
	}
	return n;
}

/* Add up the buckets named by the leaf directory in dir_buf. An allocated
 * bucket that del has emptied still costs its lookup: it is still named. */
static int leaf_count(uint16_t n, size_t *acc)
{
	for (uint16_t i = 0; i < n; i++) {
		uint64_t bid = dir_child(dir_buf, i);

		if (bid == 0) {
			continue;
		}

		size_t used = 0;
		int rc = blob_db_get(bid, bkt_buf, sizeof(bkt_buf), &used);

		if (rc != 0) {
			return rc;
		}
		*acc += bkt_entries(bkt_buf, used);
	}
	return 0;
}

static int kvhash_count(uint64_t root, size_t *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	uint16_t n;
	uint8_t depth;
	size_t total = 0;
	int rc = dir_load(root, &n, &depth);

	if (rc != 0) {
		return rc;
	}

	if (depth == 1) {
		rc = leaf_count(n, &total);
		if (rc == 0) {
			*out = total;
		}
		return rc;
	}

	/* Two levels: the same walk as next, with the top re-read on each
	 * sub-map crossing (see top_child). */
	uint64_t sub = dir_child(dir_buf, 0);

	for (uint16_t t = 0; t < n; t++) {
		uint16_t sub_n;

		if (t != 0) {
			rc = top_child(root, t, &sub);
			if (rc != 0) {
				return rc;
			}
		}
		rc = sub_load(sub, &sub_n);
		if (rc != 0) {
			return rc;
		}
		rc = leaf_count(sub_n, &total);
		if (rc != 0) {
			return rc;
		}
	}

	*out = total;
	return 0;
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
/*
 * Release one bucket: its spilled blobs first, then the bucket itself, so a
 * bucket that survives an interrupted release still names whatever has not
 * gone yet. A blob already gone is the resumed case, not an error.
 */
static int release_bucket(uint64_t owner, uint16_t i, uint64_t bid)
{
	size_t used = 0;
	int rc = blob_db_get(bid, bkt_buf, sizeof(bkt_buf), &used);

	if (rc == -ENOENT) {
		return 0;
	}
	if (rc != 0) {
		return rc;
	}

	int first_err = 0;
	struct ent e;

	for (size_t off = 0; bkt_entry(bkt_buf, used, off, &e); off += e.len) {
		uint64_t ids[2] = { ent_key_id(bkt_buf, &e), ent_val_id(bkt_buf, &e) };

		for (int j = 0; j < 2; j++) {
			if (ids[j] == 0) {
				continue;
			}

			int drc = kv_delete(ids[j]);

			if (drc != 0 && drc != -ENOENT && first_err == 0) {
				first_err = drc;
			}
		}
	}
	if (first_err != 0) {
		return first_err; /* keep the bucket: it still names the rest */
	}

	rc = kv_delete(bid);
	if (rc != 0 && rc != -ENOENT) {
		LOG_WRN("root=%llu: bucket %u (id %llu) not released: %d",
			(unsigned long long)owner, i, (unsigned long long)bid, rc);
		return rc;
	}
	return 0;
}

/* Release every bucket named by the directory currently in dir_buf. A real
 * failure is remembered but does not stop the others, because every one
 * released is progress the repeat need not redo. */
static int release_buckets(uint64_t owner, uint16_t n)
{
	int first_err = 0;

	for (uint16_t i = 0; i < n; i++) {
		uint64_t bid = dir_child(dir_buf, i);

		if (bid == 0) {
			continue;
		}

		int brc = release_bucket(owner, i, bid);

		if (brc != 0 && first_err == 0) {
			first_err = brc;
		}
	}
	return first_err;
}

/*
 * Destroy is resumable, and at depth 2 that is the whole difficulty: the
 * structure is a tree, so a crash can leave it half-released.
 *
 * The order is what makes a repeat safe. Marking the top 'KVHD' is the commit
 * point -- after it every reader gets -ENOENT (dir_load), so nothing observes
 * the parts still on flash, and a repeat re-enters here with dying already
 * true and simply carries on. Children are released before their parent, so a
 * surviving parent always still names whatever has not gone yet; the reverse
 * order would strand blobs that nothing points at, which is B8's leak.
 *
 * At depth 2, walking a sub-map loads its own directory over the top one, and
 * bkt_buf holds each bucket while its spilled blobs are released, so the top
 * is re-read for every sub-map -- one read each, on an operation that runs
 * once per map.
 */
static int kvhash_destroy(uint64_t root)
{
	uint16_t n;
	uint8_t depth;
	bool dying = false;
	int rc = dir_load_raw(root, &n, &depth, &dying);

	if (rc != 0) {
		return rc;
	}

	if (!dying) {
		uint32_t magic = KVHASH_DIR_DYING;

		memcpy(&dir_buf[0], &magic, sizeof(magic));

		rc = kv_update(root, dir_buf, dir_len(n));
		if (rc != 0) {
			return rc; /* nothing released; the container is intact */
		}
	}

	int first_err;

	if (depth == 1) {
		first_err = release_buckets(root, n);
	} else {
		first_err = 0;

		for (uint16_t i = 0; i < n; i++) {
			uint64_t sub = 0;
			uint16_t sub_n;
			uint8_t sub_depth;
			bool sub_dying = false;
			uint16_t top_n;
			int trc = dir_load_raw(root, &top_n, NULL, &sub_dying);

			if (trc == 0 && i < top_n) {
				sub = dir_child(dir_buf, i);
			} else if (trc == 0) {
				trc = -EIO;
			}
			if (trc != 0) {
				if (first_err == 0) {
					first_err = trc;
				}
				continue;
			}
			if (sub == 0) {
				continue;
			}

			int lrc = dir_load_raw(sub, &sub_n, &sub_depth, &sub_dying);

			if (lrc == -ENOENT) {
				continue;   /* already released; resumed case */
			}
			if (lrc != 0) {
				if (first_err == 0) {
					first_err = lrc;
				}
				continue;
			}

			int brc = release_buckets(sub, sub_n);

			if (brc != 0) {
				if (first_err == 0) {
					first_err = brc;
				}
				continue;   /* leave the sub-map naming its rest */
			}

			int drc = kv_delete(sub);

			if (drc != 0 && drc != -ENOENT && first_err == 0) {
				first_err = drc;
			}
		}
	}

	if (first_err != 0) {
		return first_err; /* still dying; the caller repeats */
	}

	rc = kv_delete(root);
	if (rc == 0) {
		LOG_DBG("destroyed map root=%llu depth=%u n=%u",
			(unsigned long long)root, depth, n);
	}
	return rc;
}

const struct map_ops kvhash_map_ops = {
	.create = kvhash_create,
	.stat = kvhash_stat,
	.get = kvhash_get,
	.set = kvhash_set,
	.del = kvhash_del,
	.next = kvhash_next,
	.count = kvhash_count,
	.destroy = kvhash_destroy,
};
