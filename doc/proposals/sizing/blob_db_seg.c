/*
 * Size-estimation sketch of the Stage-2 segmentation layer described in
 * doc/proposals/2026-08-09-large-payloads.md §6. Not production code — it
 * exists to get a defensible -Os .text figure for the feature. The logic is
 * the real logic (write path with master intent, pread, orphan sweep); only
 * the surrounding integration is stubbed.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include <app/lib/blob_db.h>

#include "blob_db_internal.h"
#include "blob_db_store.h"

LOG_MODULE_REGISTER(blob_db_seg, CONFIG_BLOB_DB_LOG_LEVEL);

#define BLOB_DB_SLOT_F_INDEXED    (1u << 2)
#define BLOB_DB_SLOT_F_SEGMENT    (1u << 3)

struct __packed blob_db_index_hdr {
	uint8_t  magic[2];
	uint8_t  version;
	uint8_t  flags;
	uint32_t total_len;
	uint32_t payload_crc32;
	uint16_t seg_count;
	uint16_t seg_len;
};

struct __packed blob_db_seg_hdr {
	uint64_t owner_id;
	uint16_t seq;
	uint16_t reserved;
};

/* --- provided by blob_db.c in the real integration --------------------- */
struct slot_view {
	uint64_t id;
	uint8_t  flags;
	uint16_t val_len;
	size_t   total_size;
};
struct bucket_walk {
	off_t    write_cursor;
	off_t    target_slot_off;
	uint8_t  target_flags;
	uint16_t target_val_len;
};

extern uint8_t g_bbuf[];
extern uint8_t g_bbuf_new[];
extern size_t   bdb_peb_size(void);
extern uint16_t bdb_n_buckets(void);
extern uint16_t bdb_id_to_bucket(uint64_t id);
extern off_t    bdb_bucket_offset(uint16_t bid);
extern int      bdb_read_bucket(uint16_t bid, uint8_t *buf);
extern bool     bdb_bucket_hdr_valid(const uint8_t *buf, uint16_t bid);
extern bool     bdb_slot_view_at(const uint8_t *buf, off_t off, struct slot_view *out);
extern void     bdb_walk_bucket(const uint8_t *buf, uint64_t id, struct bucket_walk *r);
extern int      bdb_append(uint64_t id, uint8_t extra_flags,
			   const void *payload, uint16_t val_len);
/* Scatter variant: writes `hdr` then `payload` as one slot, so a segment
 * needs no staging buffer of its own. */
extern int      bdb_append2(uint64_t id, uint8_t extra_flags,
			    const void *hdr, uint16_t hdr_len,
			    const void *payload, uint16_t val_len);
extern int      bdb_set_seg_owner(uint64_t owner);
extern size_t   bdb_inline_max(void);

#define IDX_HDR_SZ  ((off_t)sizeof(struct blob_db_index_hdr))
#define SEG_HDR_SZ  ((off_t)sizeof(struct blob_db_seg_hdr))

/* Segment-id tables. Sized by the max segment COUNT, not by payload size —
 * the whole RAM story of Stage 2 lives in these two arrays. Static, not
 * stack: same single-threaded contract as g_bbuf. */
#define SEG_TABLE_SZ  (8u * CONFIG_BLOB_DB_MAX_SEGMENTS)
static uint8_t g_idx_a[SEG_TABLE_SZ];
static uint8_t g_idx_b[SEG_TABLE_SZ];

/* Chunk size: the largest slot payload minus the per-segment header. */
static inline size_t seg_chunk_len(void)
{
	return bdb_inline_max() - sizeof(struct blob_db_seg_hdr);
}

static inline uint16_t idx_max_segs(void)
{
	return (uint16_t)((bdb_inline_max() - sizeof(struct blob_db_index_hdr)) /
			  sizeof(uint64_t));
}

/* Load the index record for `id` into `dst` (must hold one inline payload).
 * Returns the segment count, 0 if the id holds an inline payload, or a
 * negative errno. */
static int index_load(uint64_t id, uint8_t *dst, struct blob_db_index_hdr *h)
{
	const uint16_t bid = bdb_id_to_bucket(id);
	int rc = bdb_read_bucket(bid, g_bbuf);

	if (rc < 0) {
		return rc;
	}
	if (!bdb_bucket_hdr_valid(g_bbuf, bid)) {
		return -ENOENT;
	}

	struct bucket_walk w;

	bdb_walk_bucket(g_bbuf, id, &w);
	if (w.target_slot_off < 0 || (w.target_flags & BLOB_DB_SLOT_F_TOMBSTONE)) {
		return -ENOENT;
	}
	if (!(w.target_flags & BLOB_DB_SLOT_F_INDEXED)) {
		return 0;   /* inline payload */
	}
	if (w.target_val_len < IDX_HDR_SZ) {
		return -EIO;
	}

	const off_t poff = w.target_slot_off + sizeof(struct blob_db_slot_hdr) +
			   sizeof(uint64_t);

	memcpy(h, g_bbuf + poff, sizeof(*h));
	if (h->magic[0] != 'B' || h->magic[1] != 'X' || h->version != 1) {
		return -EIO;
	}
	if (h->seg_count > idx_max_segs() ||
	    w.target_val_len < IDX_HDR_SZ + (off_t)h->seg_count * 8) {
		return -EIO;
	}
	memcpy(dst, g_bbuf + poff + IDX_HDR_SZ, (size_t)h->seg_count * 8);
	return h->seg_count;
}

/* Copy one segment's chunk (or part of it) out of its bucket. */
static int seg_fetch(uint64_t owner, uint64_t seg_id, uint16_t seq,
		     size_t skip, void *out, size_t len)
{
	const uint16_t bid = bdb_id_to_bucket(seg_id);
	int rc = bdb_read_bucket(bid, g_bbuf);

	if (rc < 0) {
		return rc;
	}
	if (!bdb_bucket_hdr_valid(g_bbuf, bid)) {
		return -EIO;
	}

	struct bucket_walk w;

	bdb_walk_bucket(g_bbuf, seg_id, &w);
	if (w.target_slot_off < 0 ||
	    (w.target_flags & BLOB_DB_SLOT_F_TOMBSTONE) ||
	    !(w.target_flags & BLOB_DB_SLOT_F_SEGMENT)) {
		return -EIO;
	}

	const off_t poff = w.target_slot_off + sizeof(struct blob_db_slot_hdr) +
			   sizeof(uint64_t);
	struct blob_db_seg_hdr sh;

	memcpy(&sh, g_bbuf + poff, sizeof(sh));
	if (sh.owner_id != owner || sh.seq != seq) {
		return -EIO;
	}
	if (skip + len > (size_t)w.target_val_len - sizeof(sh)) {
		return -EIO;
	}
	memcpy(out, g_bbuf + poff + SEG_HDR_SZ + (off_t)skip, len);
	return 0;
}

int blob_db_size(uint64_t id, size_t *out_size)
{
	struct blob_db_index_hdr h;
	uint8_t *ids = g_idx_a;

	if (!out_size) {
		return -EINVAL;
	}

	int k = index_load(id, ids, &h);

	if (k < 0) {
		return k;
	}
	if (k == 0) {
		size_t n;
		int rc = blob_db_get(id, NULL, 0, &n);

		if (rc == -ENOMEM || rc == 0) {
			*out_size = n;
			return 0;
		}
		return rc;
	}
	*out_size = h.total_len;
	return 0;
}

int blob_db_read(uint64_t id, size_t offset, void *out, size_t len,
		 size_t *out_read)
{
	struct blob_db_index_hdr h;
	uint8_t *ids = g_idx_a;
	uint8_t *dst = out;
	size_t done = 0;

	if (!out && len) {
		return -EINVAL;
	}

	int k = index_load(id, ids, &h);

	if (k < 0) {
		return k;
	}
	if (k == 0) {
		return -ENOTSUP;   /* inline: caller uses get() */
	}
	if (offset >= h.total_len) {
		if (out_read) {
			*out_read = 0;
		}
		return 0;
	}
	if (len > h.total_len - offset) {
		len = h.total_len - offset;
	}

	while (done < len) {
		const size_t pos  = offset + done;
		const uint16_t seq = (uint16_t)(pos / h.seg_len);
		const size_t skip = pos % h.seg_len;
		size_t n = h.seg_len - skip;
		uint64_t sid;

		if (n > len - done) {
			n = len - done;
		}
		memcpy(&sid, ids + (size_t)seq * 8, sizeof(sid));

		int rc = seg_fetch(id, sid, seq, skip, dst + done, n);

		if (rc < 0) {
			return rc;
		}
		done += n;
	}

	if (out_read) {
		*out_read = done;
	}
	return 0;
}

/* Tombstone every segment id in `ids[0..k)`. Best effort: a failure here
 * leaves orphans that the sweep reclaims. */
static void seg_release(const uint8_t *ids, uint16_t k)
{
	for (uint16_t j = 0; j < k; j++) {
		uint64_t sid;

		memcpy(&sid, ids + (size_t)j * 8, sizeof(sid));
		(void)bdb_append(sid, BLOB_DB_SLOT_F_TOMBSTONE |
				      BLOB_DB_SLOT_F_SEGMENT, NULL, 0);
	}
}

int blob_db_seg_update(uint64_t id, const void *payload, size_t len)
{
	uint8_t *old_ids = g_idx_a;
	uint8_t *new_ids = g_idx_b;
	struct blob_db_index_hdr h;
	const uint8_t *src = payload;
	const size_t chunk = seg_chunk_len();
	const size_t k = (len + chunk - 1) / chunk;

	if (k > idx_max_segs() || k > CONFIG_BLOB_DB_MAX_SEGMENTS) {
		return -EFBIG;
	}

	int old_k = index_load(id, old_ids, &h);

	if (old_k < 0 && old_k != -ENOENT) {
		return old_k;
	}
	if (old_k < 0) {
		old_k = 0;
	}

	int rc = bdb_set_seg_owner(id);

	if (rc < 0) {
		return rc;
	}

	/* Segments. Each is an ordinary slot: seg header + chunk. */
	for (size_t j = 0; j < k; j++) {
		const size_t n = (j == k - 1) ? len - j * chunk : chunk;
		uint64_t sid = blob_db_alloc_id();

		if (sid == 0) {
			return -EIO;
		}
		memcpy(new_ids + j * 8, &sid, sizeof(sid));

		struct blob_db_seg_hdr sh = {
			.owner_id = id, .seq = (uint16_t)j, .reserved = 0,
		};

		rc = bdb_append2(sid, BLOB_DB_SLOT_F_SEGMENT,
				 &sh, (uint16_t)SEG_HDR_SZ,
				 src + j * chunk, (uint16_t)n);
		if (rc < 0) {
			return rc;
		}
	}

	/* Index record — the commit write. */
	struct blob_db_index_hdr nh = {
		.magic         = { 'B', 'X' },
		.version       = 1,
		.flags         = 0,
		.total_len     = (uint32_t)len,
		.payload_crc32 = crc32_ieee(src, len),
		.seg_count     = (uint16_t)k,
		.seg_len       = (uint16_t)chunk,
	};

	rc = bdb_append2(id, BLOB_DB_SLOT_F_INDEXED,
			 &nh, (uint16_t)IDX_HDR_SZ,
			 new_ids, (uint16_t)(k * 8));
	if (rc < 0) {
		return rc;
	}

	seg_release(old_ids, (uint16_t)old_k);
	return bdb_set_seg_owner(0);
}

int blob_db_seg_delete(uint64_t id)
{
	struct blob_db_index_hdr h;
	uint8_t *ids = g_idx_a;

	int k = index_load(id, ids, &h);

	if (k <= 0) {
		return k;   /* inline or error — caller's scalar path handles it */
	}

	int rc = bdb_set_seg_owner(id);

	if (rc < 0) {
		return rc;
	}
	rc = bdb_append(id, BLOB_DB_SLOT_F_TOMBSTONE, NULL, 0);
	if (rc < 0) {
		return rc;
	}
	seg_release(ids, (uint16_t)k);
	return bdb_set_seg_owner(0);
}

static bool index_lists(const uint8_t *ids, uint16_t k, uint64_t sid)
{
	for (uint16_t j = 0; j < k; j++) {
		uint64_t v;

		memcpy(&v, ids + (size_t)j * 8, sizeof(v));
		if (v == sid) {
			return true;
		}
	}
	return false;
}

/* Mount-time reclaim: tombstone every live SEGMENT slot owned by `owner`
 * that the owner's live index does not list. Idempotent; O(1) RAM beyond
 * the two sector buffers. */
int blob_db_seg_sweep(uint64_t owner)
{
	struct blob_db_index_hdr h;
	uint8_t *ids = g_idx_a;
	uint16_t k = 0;

	int rc = index_load(owner, ids, &h);

	if (rc > 0) {
		k = (uint16_t)rc;
	} else if (rc < 0 && rc != -ENOENT) {
		return rc;
	}

	const uint16_t nb = bdb_n_buckets();
	size_t dropped = 0;

	for (uint16_t bid = 0; bid < nb; bid++) {
		if (bdb_read_bucket(bid, g_bbuf_new) < 0) {
			continue;
		}
		if (!bdb_bucket_hdr_valid(g_bbuf_new, bid)) {
			continue;
		}

		off_t cursor = BLOB_DB_BUCKET_DATA_OFF;

		for (;;) {
			struct slot_view sv;

			if (!bdb_slot_view_at(g_bbuf_new, cursor, &sv)) {
				break;
			}
			cursor += sv.total_size;

			if (!(sv.flags & BLOB_DB_SLOT_F_SEGMENT) ||
			    (sv.flags & BLOB_DB_SLOT_F_TOMBSTONE) ||
			    sv.val_len < SEG_HDR_SZ) {
				continue;
			}

			struct blob_db_seg_hdr sh;
			const off_t poff = cursor - sv.total_size +
					   sizeof(struct blob_db_slot_hdr) +
					   sizeof(uint64_t);

			memcpy(&sh, g_bbuf_new + poff, sizeof(sh));
			if (sh.owner_id != owner) {
				continue;
			}
			if (index_lists(ids, k, sv.id)) {
				continue;
			}

			(void)bdb_append(sv.id, BLOB_DB_SLOT_F_TOMBSTONE |
					        BLOB_DB_SLOT_F_SEGMENT, NULL, 0);
			dropped++;
		}
	}

	LOG_INF("sweep owner=%llu: dropped %zu orphan segments",
		(unsigned long long)owner, dropped);
	return bdb_set_seg_owner(0);
}
