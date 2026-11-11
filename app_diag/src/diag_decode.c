/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_diag: the on-flash formats of the structures above blob_db, current and
 * past, and how to recognise and print each.
 *
 * This file is a second copy of those formats, kept here so that the
 * libraries carry no diagnostic code. The source of truth is the library;
 * each block below names the file and the commits whose layout it copies.
 * When a library changes a format, add the new variant here and keep the old
 * one: images written by older firmware are what this tool is for.
 *
 * All multi-byte fields are little-endian: every library writes host order
 * and every target so far is little-endian.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "diag.h"

/* ---- formats --------------------------------------------------------- */

/*
 * Root registry, lib/rootreg/rootreg.c (65c4b40, frozen v1):
 *   [u8 'RREG'] [u16 version=1] [u16 count] ([u64 key] [u64 root_id])*count
 */
#define RREG_HDR      8u
#define RREG_ENTRY    16u

/*
 * kvdb meta, lib/kvdb/kvdb.c (c4a7bda, v1, unchanged since): 32 bytes
 *   [u32 'KVDB'] [u8 backend] [u8 version=1] [u8 name_len] [u8 rsvd]
 *   [u64 struct_root] [char name[16]]
 * Registered as ROOTREG_KEY('KVDB', fnv1a32(name)).
 */
#define KVDB_MAGIC    0x4b564442u
#define KVDB_META_LEN 32u
#define KVDB_NAME_MAX 15u

/*
 * blobfs meta, lib/blobfs/blobfs_internal.h. Registered as ('BFS1', 0).
 *   v1, 83302d0 (2026-08-14): 16 bytes
 *     [u32 'BFS1'] [u8 version=1] [u8 rsvd[3]] [u64 dir_root]
 *   v1 with rename intent, b5fd32b (2026-09-24): same version byte
 *     [u32 magic] [u8 version=1] [u8 in_flight] [u8 from_len] [u8 to_len]
 *     [u64 dir_root] [u64 rename_body] [char from[N+1]] [char to[N+1]]
 *   v2, 9613f7a (2026-10-11, directories): the intent names both directories
 *     ... [u64 dir_root] [u64 rename_body] [u64 from_dir] [u64 to_dir]
 *     [char from[N+1]] [char to[N+1]]
 * N is CONFIG_BLOBFS_MAX_NAME_LEN of the writing build (default 12). The
 * struct is not packed: it is padded to the writer's u64 alignment, 8 on ARM
 * and 4 on native_sim (x86-32), so a v2 meta with N = 12 is 72 bytes on the
 * DK and 68 on native_sim. N is inferred from the length under either.
 * A directory is a Map (kvhash) of name -> 16-byte dirent:
 *     [u8 type 1=file 2=dir] [u8 version=1] [u16 rsvd] [u32 rsvd]
 *     [u64 body_id | dir_root]
 */
#define BFS_MAGIC       0x42465331u
#define BFS_V1_FIXED    24u
#define BFS_V2_FIXED    40u
#define BFS_NAME_DEFAULT 12u
#define BFS_DIRENT_LEN  16u

/*
 * kvhash, lib/containers/kvhash/kvhash.c. A directory blob:
 *   v1, c4a7bda (2026-07-26), one level:
 *     [u32 'KVHA'] [u16 n_buckets] [u16 version=1] [u64 bucket_id]*n
 *   v2, fa6cd1b (2026-08-20), any depth; v3, 7c34a57 (2026-10-09), spill:
 *     [u32 'KVHA'] [u16 n] [u8 version] [u8 depth] [u64 child_id]*n
 *   'KVHD' in place of 'KVHA': a destroy has committed, the children are
 *   being released.
 * At depth 1 the children are buckets (0 = empty), at depth 2 the roots of
 * depth-1 sub-maps. A bucket blob, v1..v3 alike (the flags are always clear
 * before v3):
 *   ( [u16 klen|KS<<15] [u16 vlen|VS<<15] [key part] [value part] )*
 *   key part   KS=0: key bytes   KS=1: [u32 key_fp] [u64 key_id]
 *   value part VS=0: value bytes VS=1: [u64 val_id]
 */
#define KVH_MAGIC     0x4b564841u
#define KVH_DYING     0x4b564844u
#define KVH_HDR       8u
#define KVH_SPILL     0x8000u
#define KVH_LEN_MASK  0x7fffu

/*
 * logring, lib/containers/logring/logring.c. Both formats say 'CLOG' v1.
 *   ring, 16d5890 (2026-07-05): the whole log inline in one blob
 *     [u8 'CLOG'] [u8 version=1] [u8 rsvd] [u16 count] [u16 used]
 *     [u16 rsvd2] [u64 next_seq]  then ([u64 seq] [u16 len] [len bytes])*
 *     and the blob is exactly 20 + used bytes
 *   chain, 824042b (2026-10-09): a root i-node of exactly 56 bytes
 *     [u8 'CLOG'] [u8 version=1] [u8 flags] [u16 rsvd=0] [u32 epoch]
 *     [u32 capacity_bytes] [u32 live_bytes] [u32 count] [u64 waypoint_id]
 *     [u64 head_id] [u64 evict_from] [u64 evict_to]
 *   and one blob per entry: [u64 next_id] [u16 len] [len bytes]; next_id is
 *   an id reserved for the next entry, unbound at the tail.
 * A 56-byte ring holds records, so its count (bytes 6..7) is not 0; the
 * chain's reserved field there is.
 */
#define CLOG_RING_HDR   20u
#define CLOG_RING_REC   10u
#define CLOG_ROOT_LEN   56u
#define CLOG_ENTRY_HDR  10u

/*
 * app_cbor_persondb superblock, app_cbor_persondb/persondb/person_cbor.c,
 * canonical CBOR. Registered as ('PADB', 0).
 *   v1, 2c58710 (2026-08-09):
 *     {1: 1, 2: [people_root, ...], 3: cred_root, 4: n_persons,
 *      5: populated, 6: rev}
 *   v2, 35dac28 / c5ece5b (2026-10-24), a table directory:
 *     {1: 2, 2: {table_id: {1: root, 2: class, 3: schema}, ...},
 *      3: n_persons, 4: populated, 5: rev}
 *   table 1 = people, 2 = credentials; class 0 required, 1 preserve,
 *   2 derived. Every table is a kvhash map with CBOR values.
 */
#define PADB_MAGIC     0x50414442u
#define PADB_TABLES_MAX 8
#define PADB_V1_ROOTS_MAX 16

/* Registry FourCCs this tool can name. */
#define FOURCC_KVDB  KVDB_MAGIC
#define FOURCC_BFS1  BFS_MAGIC
#define FOURCC_PADB  PADB_MAGIC

/* ---- type table ------------------------------------------------------ */

enum { BFS_V1_BARE, BFS_V1_INTENT, BFS_V2 };
enum { PADB_V1, PADB_V2 };
enum { KVH_V1, KVH_V2, KVH_V3 };
enum { CLOG_RING, CLOG_CHAIN };

static const struct diag_type_info types[DIAG_T_COUNT] = {
	[DIAG_T_RREG] = { "rootreg", "root registry: key -> structure root",
			  { "v1" } },
	[DIAG_T_KVDB_META] = { "kvdb", "kvdb instance meta: name -> map root",
			       { "v1" } },
	[DIAG_T_BLOBFS_META] = { "blobfs", "blobfs meta: root directory, "
				 "rename intent",
				 { "v1 bare (2026-08)", "v1 intent (2026-09)",
				   "v2 dirs (2026-10)" } },
	[DIAG_T_PADB_SB] = { "persondb", "app_cbor_persondb superblock (CBOR)",
			     { "v1 shards (2026-08)",
			       "v2 tables (2026-10)" } },
	[DIAG_T_KVHASH_DIR] = { "kvhash", "kvhash directory: map root or "
				"sub-map",
				{ "v1 (2026-07)", "v2 (2026-08)",
				  "v3 spill (2026-10)" } },
	[DIAG_T_CLOG_ROOT] = { "logring", "logring: whole ring, or chain root",
			       { "ring v1 (2026-07)",
				 "chain v1 (2026-10)" } },
	[DIAG_T_KVHASH_BUCKET] = { "kvbucket", "kvhash bucket: packed "
				   "key/value pairs", { "" } },
	[DIAG_T_CLOG_ENTRY] = { "logentry", "logring chain entry", { "" } },
	[DIAG_T_CBOR] = { "cbor", "one CBOR array or map (e.g. a spilled "
			  "record)", { "" } },
	[DIAG_T_EMPTY] = { "empty", "zero-length payload", { "" } },
	[DIAG_T_UNKNOWN] = { "unknown", "not recognised", { "" } },
};

const struct diag_type_info *diag_type_info(enum diag_type t)
{
	return (t < DIAG_T_COUNT) ? &types[t] : &types[DIAG_T_UNKNOWN];
}

int diag_type_by_name(const char *name, enum diag_type *out)
{
	for (int t = 0; t < DIAG_T_COUNT; t++) {
		if (strcmp(name, types[t].name) == 0) {
			*out = (enum diag_type)t;
			return 0;
		}
	}
	return -ENOENT;
}

/* ---- helpers --------------------------------------------------------- */

static inline uint16_t le16(const uint8_t *p) { return sys_get_le16(p); }
static inline uint32_t le32(const uint8_t *p) { return sys_get_le32(p); }
static inline uint64_t le64(const uint8_t *p) { return sys_get_le64(p); }


static void fourcc(uint32_t v, char out[5])
{
	for (int i = 0; i < 4; i++) {
		const char c = (char)(v >> (24 - 8 * i));

		out[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
	}
	out[4] = '\0';
}

static bool printable(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i] < 0x20 || p[i] >= 0x7f) {
			return false;
		}
	}
	return true;
}

/* A short rendering of bytes of unknown meaning: text, CBOR, or hex. */
static void preview(char *out, size_t sz, const uint8_t *p, size_t n)
{
	size_t len;

	if (n == 0) {
		snprintk(out, sz, "(empty)");
	} else if (printable(p, n)) {
		snprintk(out, sz, "\"%.*s\"%s", (int)MIN(n, sz - 6), p,
			 n > sz - 6 ? ".." : "");
	} else if (n >= 2 && ((p[0] >> 5) == 4 || (p[0] >> 5) == 5) &&
		   diag_cbor_item_len(p, n, &len) == 0 && len == n &&
		   diag_cbor_format(p, n, out, sz) > 0) {
		/* formatted in place */
	} else {
		size_t o = 0;
		const size_t show = MIN(n, (sz - 3) / 2);

		for (size_t i = 0; i < show; i++) {
			o += snprintk(out + o, sz - o, "%02x", p[i]);
		}
		if (show < n) {
			snprintk(out + o, sz - o, "..");
		}
	}
}

static void name_copy(char *out, size_t sz, const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n && i + 1 < sz; i++) {
		out[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '?';
	}
	out[i] = '\0';
}

/* ---- per-format checks ----------------------------------------------- */

/* Walk a pre-chain logring's record run: it must split [0, used) into
 * exactly `count` records. */
static bool clog_ring_ok(const uint8_t *buf, size_t size)
{
	const uint16_t count = le16(buf + 6);
	const uint16_t used = le16(buf + 8);
	const uint8_t *p = buf + CLOG_RING_HDR;
	size_t off = 0;
	uint32_t seen = 0;

	if (size != CLOG_RING_HDR + used) {
		return false;
	}
	while (off < used) {
		if (used - off < CLOG_RING_REC) {
			return false;
		}
		const uint16_t len = le16(p + off + 8);

		off += CLOG_RING_REC;
		if (len > used - off) {
			return false;
		}
		off += len;
		seen++;
	}
	return seen == count;
}

/* Walk a kvhash bucket. Returns the entry count, or -1 when the bytes do
 * not split into whole entries. */
static int kvh_bucket_walk(const uint8_t *buf, size_t size, int *spilled)
{
	size_t off = 0;
	int n = 0;

	*spilled = 0;
	while (off < size) {
		if (size - off < 4) {
			return -1;
		}
		const uint16_t kl = le16(buf + off);
		const uint16_t vl = le16(buf + off + 2);
		const size_t kpart = (kl & KVH_SPILL) ? 12u : (kl & KVH_LEN_MASK);
		const size_t vpart = (vl & KVH_SPILL) ? 8u : (vl & KVH_LEN_MASK);

		if ((kl & KVH_LEN_MASK) == 0) {
			return -1;   /* kvhash stores no empty key */
		}
		off += 4;
		if (kpart + vpart > size - off) {
			return -1;
		}
		off += kpart + vpart;
		*spilled += !!(kl & KVH_SPILL) + !!(vl & KVH_SPILL);
		n++;
	}
	return n;
}

/* True if p[from, to) is all zero. */
static bool zeros(const uint8_t *p, size_t from, size_t to)
{
	for (size_t i = from; i < to; i++) {
		if (p[i]) {
			return false;
		}
	}
	return true;
}

/* blobfs v1-with-intent and v2: infer the writer's name limit from the
 * length, padded to 4 or to 8, preferring the default. blobfs zero-fills
 * both name fields, so with the whole meta at hand a candidate must leave
 * every byte past each name, up to the limit, zero. Returns 0 if none
 * fits. */
static size_t bfs_name_max(const uint8_t *buf, size_t n, size_t size,
			   size_t fixed)
{
	size_t best = 0;

	for (size_t nm = 1; nm <= 255; nm++) {
		const size_t raw = fixed + 2 * (nm + 1);

		if (ROUND_UP(raw, 4u) != size && ROUND_UP(raw, 8u) != size) {
			continue;
		}
		if (n == size) {
			const size_t fl = buf[5] ? buf[6] : 0;
			const size_t tl = buf[5] ? buf[7] : 0;
			const size_t to = fixed + nm + 1;

			if (fl > nm || tl > nm ||
			    !zeros(buf, fixed + fl, to) ||
			    !zeros(buf, to + tl, to + nm + 1)) {
				continue;
			}
		}
		if (nm == BFS_NAME_DEFAULT || best == 0) {
			best = nm;
		}
	}
	return best;
}

/* The persondb superblock, decoded tolerantly: unknown keys are skipped. */
struct padb {
	uint64_t version;
	uint8_t  n_tables;
	struct { uint64_t id, root, cls, schema; } t[PADB_TABLES_MAX];
	uint8_t  n_roots;               /* v1 people roots */
	uint64_t roots[PADB_V1_ROOTS_MAX];
	uint64_t cred_root;             /* v1 */
	uint64_t n_persons, populated, rev;
	bool     more;                  /* entries past what is shown */
};

static int padb_decode(const uint8_t *buf, size_t size, struct padb *sb)
{
	struct diag_cbor c = { .p = buf, .end = buf + size };
	uint64_t pairs, key;

	memset(sb, 0, sizeof(*sb));
	if (!diag_cbor_map(&c, &pairs) || !diag_cbor_uint(&c, &key) ||
	    key != 1 || !diag_cbor_uint(&c, &sb->version)) {
		return -EILSEQ;
	}
	if (sb->version != 1 && sb->version != 2) {
		return -ENOTSUP;
	}
	for (uint64_t i = 1; i < pairs && !c.err; i++) {
		uint64_t n;

		if (!diag_cbor_uint(&c, &key)) {
			break;
		}
		if (sb->version == 1) {
			switch (key) {
			case 2:
				if (!diag_cbor_array(&c, &n)) {
					break;
				}
				for (uint64_t j = 0; j < n && !c.err; j++) {
					uint64_t r;

					if (diag_cbor_uint(&c, &r) &&
					    sb->n_roots < PADB_V1_ROOTS_MAX) {
						sb->roots[sb->n_roots++] = r;
					} else if (!c.err) {
						sb->more = true;
					}
				}
				break;
			case 3: diag_cbor_uint(&c, &sb->cred_root); break;
			case 4: diag_cbor_uint(&c, &sb->n_persons); break;
			case 5: diag_cbor_uint(&c, &sb->populated); break;
			case 6: diag_cbor_uint(&c, &sb->rev); break;
			default: diag_cbor_skip(&c); break;
			}
			continue;
		}
		switch (key) {
		case 2:
			if (!diag_cbor_map(&c, &n)) {
				break;
			}
			for (uint64_t j = 0; j < n && !c.err; j++) {
				uint64_t tid, fields, f, v;
				const bool keep = sb->n_tables < PADB_TABLES_MAX;

				if (!diag_cbor_uint(&c, &tid) ||
				    !diag_cbor_map(&c, &fields)) {
					break;
				}
				if (keep) {
					sb->t[sb->n_tables].id = tid;
				} else {
					sb->more = true;
				}
				for (uint64_t k = 0; k < fields && !c.err; k++) {
					if (!diag_cbor_uint(&c, &f)) {
						break;
					}
					if (f < 1 || f > 3 ||
					    diag_cbor_peek(&c) != 0) {
						diag_cbor_skip(&c);
						continue;
					}
					diag_cbor_uint(&c, &v);
					if (!keep) {
						continue;
					}
					if (f == 1) {
						sb->t[sb->n_tables].root = v;
					} else if (f == 2) {
						sb->t[sb->n_tables].cls = v;
					} else {
						sb->t[sb->n_tables].schema = v;
					}
				}
				sb->n_tables += keep;
			}
			break;
		case 3: diag_cbor_uint(&c, &sb->n_persons); break;
		case 4: diag_cbor_uint(&c, &sb->populated); break;
		case 5: diag_cbor_uint(&c, &sb->rev); break;
		default: diag_cbor_skip(&c); break;
		}
	}
	if (c.err) {
		return -EILSEQ;
	}
	return (c.p == c.end) ? 0 : -EMSGSIZE;
}

static const char *padb_table_name(uint64_t id)
{
	return id == 1 ? "people" : id == 2 ? "credentials" : "table";
}

static const char *padb_class_name(uint64_t cls)
{
	static const char *const names[] = { "required", "preserve",
					     "derived" };

	return cls < ARRAY_SIZE(names) ? names[cls] : "?";
}

static const char *kvdb_backend_name(uint8_t b)
{
	static const char *const names[] = { "default?", "hash", "tree",
					     "list" };

	return b < ARRAY_SIZE(names) ? names[b] : "?";
}

/* ---- classification -------------------------------------------------- */

static void suspect(struct diag_class *c, const char *why)
{
	if (!c->suspect) {
		c->suspect = true;
		c->note = why;
	}
}

static bool classify_magic(const uint8_t *buf, size_t n, size_t size,
			   struct diag_class *c)
{
	const bool whole = (n == size);

	if (n < 8) {
		return false;
	}

	if (memcmp(buf, "RREG", 4) == 0) {
		c->type = DIAG_T_RREG;
		if (le16(buf + 4) != 1) {
			suspect(c, "unknown version");
		} else if (size != RREG_HDR + RREG_ENTRY * le16(buf + 6)) {
			suspect(c, "length disagrees with count");
		}
		return true;
	}

	if (memcmp(buf, "CLOG", 4) == 0) {
		c->type = DIAG_T_CLOG_ROOT;
		if (buf[4] != 1) {
			suspect(c, "unknown version");
		}
		if (size == CLOG_ROOT_LEN && le16(buf + 6) == 0) {
			c->variant = CLOG_CHAIN;
			if (n >= 12 && le32(buf + 8) == 0) {
				suspect(c, "epoch is 0");
			}
		} else if (n >= 10 && size == CLOG_RING_HDR + le16(buf + 8)) {
			c->variant = CLOG_RING;
			if (whole && !clog_ring_ok(buf, size)) {
				suspect(c, "record run inconsistent");
			}
		} else {
			c->variant = (size == CLOG_ROOT_LEN) ? CLOG_CHAIN :
							       CLOG_RING;
			suspect(c, "length fits neither ring nor chain root");
		}
		return true;
	}

	const uint32_t m = le32(buf);

	if (m == KVH_MAGIC || m == KVH_DYING) {
		const uint16_t nb = le16(buf + 4);

		c->type = DIAG_T_KVHASH_DIR;
		if (m == KVH_DYING) {
			c->note = "destroy committed, children being released";
		}
		if (buf[6] == 1 && buf[7] == 0) {
			c->variant = KVH_V1;
		} else if (buf[6] == 2 || buf[6] == 3) {
			c->variant = (buf[6] == 2) ? KVH_V2 : KVH_V3;
			if (buf[7] < 1 || buf[7] > 2) {
				suspect(c, "depth not 1 or 2");
			}
		} else {
			c->variant = KVH_V3;
			suspect(c, "unknown version");
		}
		if (size != KVH_HDR + 8u * nb) {
			suspect(c, "length disagrees with child count");
		}
		return true;
	}

	if (m == KVDB_MAGIC) {
		c->type = DIAG_T_KVDB_META;
		if (size != KVDB_META_LEN) {
			suspect(c, "not 32 bytes");
		} else if (buf[5] != 1) {
			suspect(c, "unknown version");
		} else if (buf[4] < 1 || buf[4] > 3) {
			suspect(c, "unknown backend");
		} else if (buf[6] < 1 || buf[6] > KVDB_NAME_MAX) {
			suspect(c, "bad name length");
		}
		return true;
	}

	if (m == BFS_MAGIC) {
		c->type = DIAG_T_BLOBFS_META;
		if (buf[4] == 1 && size == 16) {
			c->variant = BFS_V1_BARE;
		} else if (buf[4] == 1) {
			c->variant = BFS_V1_INTENT;
			if (bfs_name_max(buf, n, size, BFS_V1_FIXED) == 0) {
				suspect(c, "length fits no name limit");
			}
		} else if (buf[4] == 2) {
			c->variant = BFS_V2;
			if (bfs_name_max(buf, n, size, BFS_V2_FIXED) == 0) {
				suspect(c, "length fits no name limit");
			}
		} else {
			c->variant = BFS_V2;
			suspect(c, "unknown version");
		}
		return true;
	}

	/* The persondb superblock has no magic, but its first bytes are
	 * fixed: a 6-pair map whose key 1 is 1 (v1), or a 5-pair map whose
	 * key 1 is 2 (v2). A person record is a 9-pair map. */
	if ((buf[0] == 0xa6 && buf[1] == 0x01 && buf[2] == 0x01) ||
	    (buf[0] == 0xa5 && buf[1] == 0x01 && buf[2] == 0x02)) {
		struct padb sb;

		if (!whole) {
			c->type = DIAG_T_PADB_SB;
			c->shape = true;
			c->variant = (buf[2] == 1) ? PADB_V1 : PADB_V2;
			return true;
		}
		if (padb_decode(buf, size, &sb) == 0) {
			c->type = DIAG_T_PADB_SB;
			c->shape = true;
			c->variant = (sb.version == 1) ? PADB_V1 : PADB_V2;
			return true;
		}
	}
	return false;
}

static bool classify_shape(uint64_t id, const uint8_t *buf, size_t size,
			   struct diag_class *c)
{
	int spilled;
	size_t len;

	c->shape = true;
	if (size >= CLOG_ENTRY_HDR &&
	    size == CLOG_ENTRY_HDR + le16(buf + 8) && le64(buf) > id) {
		c->type = DIAG_T_CLOG_ENTRY;
		return true;
	}
	if (size >= 2 && ((buf[0] >> 5) == 4 || (buf[0] >> 5) == 5) &&
	    diag_cbor_item_len(buf, size, &len) == 0 && len == size) {
		c->type = DIAG_T_CBOR;
		return true;
	}
	if (kvh_bucket_walk(buf, size, &spilled) > 0) {
		c->type = DIAG_T_KVHASH_BUCKET;
		return true;
	}
	c->shape = false;
	return false;
}

void diag_classify(uint64_t id, const uint8_t *buf, size_t n, size_t size,
		   struct diag_class *out)
{
	*out = (struct diag_class){ .type = DIAG_T_UNKNOWN };

	if (size == 0) {
		out->type = DIAG_T_EMPTY;
		return;
	}
	if (classify_magic(buf, n, size, out)) {
		return;
	}
	if (n == size && classify_shape(id, buf, size, out)) {
		return;
	}
	*out = (struct diag_class){ .type = DIAG_T_UNKNOWN };
}

/* Fewest bytes each decoder reads without checking. */
static size_t min_len(enum diag_type t)
{
	switch (t) {
	case DIAG_T_RREG:        return RREG_HDR;
	case DIAG_T_KVDB_META:   return KVDB_META_LEN;
	case DIAG_T_BLOBFS_META: return 16;
	case DIAG_T_KVHASH_DIR:  return KVH_HDR;
	case DIAG_T_CLOG_ROOT:   return CLOG_RING_HDR;
	case DIAG_T_CLOG_ENTRY:  return CLOG_ENTRY_HDR;
	case DIAG_T_PADB_SB:
	case DIAG_T_CBOR:        return 1;
	default:                 return 0;
	}
}

int diag_force(uint64_t id, enum diag_type t, const uint8_t *buf, size_t n,
	       size_t size, struct diag_class *out)
{
	diag_classify(id, buf, n, size, out);
	if (out->type == t) {
		return 0;
	}
	if (n < min_len(t)) {
		return -EMSGSIZE;
	}
	*out = (struct diag_class){ .type = t, .shape = true };
	/* Pick the variant the bytes say, where they say one. */
	if (t == DIAG_T_KVHASH_DIR) {
		out->variant = (buf[6] == 1 && buf[7] == 0) ? KVH_V1 :
			       (buf[6] == 2) ? KVH_V2 : KVH_V3;
	} else if (t == DIAG_T_CLOG_ROOT) {
		out->variant = (size == CLOG_ROOT_LEN) ? CLOG_CHAIN : CLOG_RING;
	} else if (t == DIAG_T_BLOBFS_META) {
		out->variant = (buf[4] == 2) ? BFS_V2 :
			       (size == 16) ? BFS_V1_BARE : BFS_V1_INTENT;
	} else if (t == DIAG_T_PADB_SB) {
		out->variant = (n >= 3 && buf[2] == 1) ? PADB_V1 : PADB_V2;
	}
	out->suspect = true;
	out->note = "decoded as asked, not as recognised";
	return 0;
}

/* ---- registry -------------------------------------------------------- */

const char *diag_rreg_key_name(uint64_t key)
{
	switch ((uint32_t)(key >> 32)) {
	case FOURCC_KVDB: return "kvdb";
	case FOURCC_BFS1: return "blobfs";
	case FOURCC_PADB: return "persondb";
	default: return NULL;
	}
}

int diag_rreg_foreach(const uint8_t *buf, size_t n,
		      void (*cb)(uint64_t key, uint64_t root, void *user),
		      void *user)
{
	if (n < RREG_HDR || memcmp(buf, "RREG", 4) != 0 ||
	    le16(buf + 4) != 1) {
		return -EINVAL;
	}
	const uint16_t count = le16(buf + 6);

	for (uint16_t i = 0; i < count; i++) {
		const size_t off = RREG_HDR + (size_t)i * RREG_ENTRY;

		if (off + RREG_ENTRY > n) {
			break;
		}
		cb(le64(buf + off), le64(buf + off + 8), user);
	}
	return 0;
}

static void rreg_key_text(uint64_t key, char *out, size_t sz)
{
	char cc[5];
	const char *name = diag_rreg_key_name(key);

	fourcc((uint32_t)(key >> 32), cc);
	snprintk(out, sz, "'%s'/0x%08x%s%s%s", cc, (uint32_t)key,
		 name ? " (" : "", name ? name : "", name ? ")" : "");
}

/* ---- references ------------------------------------------------------ */

static int add_ref(struct diag_ref *out, int max, int k, uint64_t id,
		   const char *label)
{
	if (id != 0 && k < max) {
		out[k] = (struct diag_ref){ .id = id, .label = label };
		k++;
	}
	return k;
}

int diag_refs(const struct diag_class *c, const uint8_t *buf, size_t n,
	      struct diag_ref *out, int max)
{
	int k = 0;

	switch (c->type) {
	case DIAG_T_KVDB_META:
		if (n >= 16) {
			k = add_ref(out, max, k, le64(buf + 8), "struct_root");
		}
		break;
	case DIAG_T_BLOBFS_META:
		if (n >= 16) {
			k = add_ref(out, max, k, le64(buf + 8), "dir_root");
		}
		if (c->variant != BFS_V1_BARE && n >= 24 && buf[5]) {
			k = add_ref(out, max, k, le64(buf + 16),
				    "rename_body");
		}
		break;
	case DIAG_T_PADB_SB: {
		struct padb sb;

		if (padb_decode(buf, n, &sb) != 0) {
			break;
		}
		for (int i = 0; i < sb.n_tables; i++) {
			k = add_ref(out, max, k, sb.t[i].root,
				    padb_table_name(sb.t[i].id));
		}
		for (int i = 0; i < sb.n_roots; i++) {
			k = add_ref(out, max, k, sb.roots[i], "people");
		}
		k = add_ref(out, max, k, sb.cred_root, "credentials");
		break;
	}
	case DIAG_T_CLOG_ROOT:
		if (c->variant == CLOG_CHAIN && n >= CLOG_ROOT_LEN) {
			k = add_ref(out, max, k, le64(buf + 32), "head");
			k = add_ref(out, max, k, le64(buf + 24), "waypoint");
		}
		break;
	case DIAG_T_CLOG_ENTRY:
		if (n >= 8) {
			k = add_ref(out, max, k, le64(buf), "next");
		}
		break;
	default:
		break;
	}
	return k;
}

/* ---- one-line summary ------------------------------------------------ */

static void summary_fields(char *f, size_t sz, const struct diag_class *c,
			   const uint8_t *buf, size_t n, size_t size)
{
	char tmp[48];

	f[0] = '\0';
	switch (c->type) {
	case DIAG_T_RREG:
		snprintk(f, sz, "entries=%u", le16(buf + 6));
		break;
	case DIAG_T_KVDB_META:
		if (n >= KVDB_META_LEN) {
			name_copy(tmp, sizeof(tmp), buf + 16,
				  MIN(buf[6], KVDB_NAME_MAX));
			snprintk(f, sz, "name=\"%s\" backend=%s map=%llu",
				 tmp, kvdb_backend_name(buf[4]),
				 (unsigned long long)le64(buf + 8));
		}
		break;
	case DIAG_T_BLOBFS_META:
		if (n >= 16) {
			snprintk(f, sz, "dir_root=%llu%s",
				 (unsigned long long)le64(buf + 8),
				 (c->variant != BFS_V1_BARE && buf[5]) ?
				 " rename-in-flight" : "");
		}
		break;
	case DIAG_T_PADB_SB: {
		struct padb sb;

		if (n != size || padb_decode(buf, n, &sb) != 0) {
			break;
		}
		if (sb.version == 1) {
			snprintk(f, sz, "people_maps=%u cred=%llu persons=%llu "
				 "rev=%llu", sb.n_roots,
				 (unsigned long long)sb.cred_root,
				 (unsigned long long)sb.n_persons,
				 (unsigned long long)sb.rev);
		} else {
			snprintk(f, sz, "tables=%u persons=%llu rev=%llu",
				 sb.n_tables, (unsigned long long)sb.n_persons,
				 (unsigned long long)sb.rev);
		}
		break;
	}
	case DIAG_T_KVHASH_DIR: {
		const uint16_t nb = le16(buf + 4);
		const unsigned int depth = (c->variant == KVH_V1) ? 1 : buf[7];
		unsigned int used = 0;

		for (uint16_t i = 0; i < nb && KVH_HDR + 8u * (i + 1) <= n;
		     i++) {
			used += le64(buf + KVH_HDR + 8u * i) != 0;
		}
		snprintk(f, sz, "n=%u depth=%u bound=%u%s", nb, depth, used,
			 KVH_HDR + 8u * nb > n ? "+" : "");
		break;
	}
	case DIAG_T_CLOG_ROOT:
		if (c->variant == CLOG_RING) {
			snprintk(f, sz, "records=%u used=%u next_seq=%llu",
				 le16(buf + 6), le16(buf + 8),
				 n >= 20 ? (unsigned long long)le64(buf + 12) :
				 0ULL);
		} else if (n >= CLOG_ROOT_LEN) {
			snprintk(f, sz, "epoch=%08x count=%u head=%llu%s",
				 le32(buf + 8), le32(buf + 20),
				 (unsigned long long)le64(buf + 32),
				 le64(buf + 40) ? " evicting" : "");
		}
		break;
	case DIAG_T_KVHASH_BUCKET: {
		int spilled;
		const int e = kvh_bucket_walk(buf, n, &spilled);

		snprintk(f, sz, "entries=%d spilled=%d", e, spilled);
		break;
	}
	case DIAG_T_CLOG_ENTRY:
		snprintk(f, sz, "next=%llu len=%u",
			 (unsigned long long)le64(buf), le16(buf + 8));
		break;
	case DIAG_T_CBOR:
	case DIAG_T_UNKNOWN:
		preview(f, MIN(sz, 40u), buf, MIN(n, size));
		break;
	default:
		break;
	}
}

void diag_summary(const struct shell *sh, const char *prefix, uint64_t id,
		  const struct diag_class *c, const uint8_t *buf, size_t n,
		  size_t size, const char *suffix)
{
	const struct diag_type_info *ti = diag_type_info(c->type);
	const char *var = ti->variant[c->variant] ? ti->variant[c->variant] :
						    "";
	char fields[96];

	summary_fields(fields, sizeof(fields), c, buf, n, size);
	shell_print(sh, "%s%-6llu %-8s %-19s %6zu  %s%s%s%s%s%s", prefix,
		    (unsigned long long)id, ti->name, var, size, fields,
		    c->shape ? " [shape]" : "",
		    c->suspect ? " [suspect: " : "",
		    c->suspect ? c->note : "", c->suspect ? "]" : "",
		    suffix);
	if (!c->suspect && c->note) {
		shell_print(sh, "%s       (%s)", prefix, c->note);
	}
}

/* ---- full decode ----------------------------------------------------- */

static void hexdump(const struct shell *sh, const uint8_t *buf, size_t n,
		    size_t max)
{
	if (n > max) {
		shell_hexdump(sh, buf, max);
		shell_print(sh, "  ... %zu more bytes (blob_db dump <id>)",
			    n - max);
	} else {
		shell_hexdump(sh, buf, n);
	}
}

static void decode_rreg(const struct shell *sh, const uint8_t *buf, size_t n)
{
	const uint16_t count = le16(buf + 6);
	char key[48];

	shell_print(sh, "  version %u, %u entries", le16(buf + 4), count);
	for (uint16_t i = 0; i < count; i++) {
		const size_t off = RREG_HDR + (size_t)i * RREG_ENTRY;

		if (off + RREG_ENTRY > n) {
			shell_print(sh, "  (truncated)");
			break;
		}
		rreg_key_text(le64(buf + off), key, sizeof(key));
		shell_print(sh, "  [%u] key %-30s -> %llu", i, key,
			    (unsigned long long)le64(buf + off + 8));
	}
}

static void decode_kvdb(const struct shell *sh, const uint8_t *buf, size_t n)
{
	char name[KVDB_NAME_MAX + 1];

	if (n < KVDB_META_LEN) {
		return;
	}
	name_copy(name, sizeof(name), buf + 16, MIN(buf[6], KVDB_NAME_MAX));
	shell_print(sh, "  version %u, backend %u (%s)", buf[5], buf[4],
		    kvdb_backend_name(buf[4]));
	shell_print(sh, "  name \"%s\" (%u bytes)", name, buf[6]);
	shell_print(sh, "  struct_root -> %llu (the map)",
		    (unsigned long long)le64(buf + 8));
}

static void decode_blobfs(const struct shell *sh, const struct diag_class *c,
			  const uint8_t *buf, size_t n, size_t size)
{
	shell_print(sh, "  version %u", buf[4]);
	shell_print(sh, "  dir_root -> %llu (root directory, a kvhash map)",
		    (unsigned long long)le64(buf + 8));
	if (c->variant == BFS_V1_BARE || n < BFS_V1_FIXED) {
		return;
	}
	const size_t fixed = (c->variant == BFS_V2) ? BFS_V2_FIXED :
						      BFS_V1_FIXED;
	const size_t nm = bfs_name_max(buf, n, size, fixed);

	shell_print(sh, "  name limit %zu%s", nm,
		    nm ? " (inferred from the length)" : " (unknown)");
	if (!buf[5]) {
		shell_print(sh, "  no rename in flight");
		return;
	}
	shell_print(sh, "  rename in flight: body %llu",
		    (unsigned long long)le64(buf + 16));
	if (c->variant == BFS_V2 && n >= BFS_V2_FIXED) {
		shell_print(sh, "    from dir %llu, to dir %llu",
			    (unsigned long long)le64(buf + 24),
			    (unsigned long long)le64(buf + 32));
	}
	if (nm && n >= fixed + 2 * (nm + 1)) {
		char from[256], to[256];

		name_copy(from, sizeof(from), buf + fixed, MIN(buf[6], nm));
		name_copy(to, sizeof(to), buf + fixed + nm + 1,
			  MIN(buf[7], nm));
		shell_print(sh, "    \"%s\" -> \"%s\"", from, to);
	}
}

static void decode_padb(const struct shell *sh, const uint8_t *buf, size_t n)
{
	struct padb sb;
	char text[160];
	int rc = padb_decode(buf, n, &sb);

	if (rc != 0) {
		shell_print(sh, "  does not decode as a superblock: %d", rc);
	} else if (sb.version == 1) {
		shell_print(sh, "  version 1: persons %llu, populated %llu, "
			    "rev %llu", (unsigned long long)sb.n_persons,
			    (unsigned long long)sb.populated,
			    (unsigned long long)sb.rev);
		for (int i = 0; i < sb.n_roots; i++) {
			shell_print(sh, "  people[%d] -> %llu", i,
				    (unsigned long long)sb.roots[i]);
		}
		shell_print(sh, "  credentials -> %llu",
			    (unsigned long long)sb.cred_root);
	} else {
		shell_print(sh, "  version 2: persons %llu, populated %llu, "
			    "rev %llu", (unsigned long long)sb.n_persons,
			    (unsigned long long)sb.populated,
			    (unsigned long long)sb.rev);
		for (int i = 0; i < sb.n_tables; i++) {
			shell_print(sh, "  table %llu %-11s -> %-6llu class %s, "
				    "schema %llu",
				    (unsigned long long)sb.t[i].id,
				    padb_table_name(sb.t[i].id),
				    (unsigned long long)sb.t[i].root,
				    padb_class_name(sb.t[i].cls),
				    (unsigned long long)sb.t[i].schema);
		}
	}
	if (rc == 0 && sb.more) {
		shell_print(sh, "  (more entries than shown)");
	}
	if (diag_cbor_format(buf, n, text, sizeof(text)) > 0) {
		shell_print(sh, "  cbor %s", text);
	}
}

static void decode_kvhash(const struct shell *sh, const struct diag_class *c,
			  const uint8_t *buf, size_t n)
{
	const uint16_t nb = le16(buf + 4);
	const unsigned int depth = (c->variant == KVH_V1) ? 1 : buf[7];
	unsigned int shown = 0;

	shell_print(sh, "  %s, version %u, depth %u, %u children (%s)",
		    le32(buf) == KVH_DYING ? "KVHD (dying)" :
		    le32(buf) == KVH_MAGIC ? "KVHA" : "no magic",
		    c->variant == KVH_V1 ? le16(buf + 6) : buf[6], depth, nb,
		    depth >= 2 ? "sub-map roots" : "buckets; 0 = empty");
	for (uint16_t i = 0; i < nb; i++) {
		const size_t off = KVH_HDR + 8u * i;

		if (off + 8 > n) {
			shell_print(sh, "  (truncated at child %u)", i);
			break;
		}
		const uint64_t child = le64(buf + off);

		if (child) {
			shell_print(sh, "  [%u] -> %llu", i,
				    (unsigned long long)child);
			shown++;
		}
	}
	shell_print(sh, "  %u of %u bound%s", shown, nb,
		    depth >= 2 ? "" : "; show a child with "
		    "`diag show <id> kvbucket`");
}

static void decode_bucket(const struct shell *sh, const uint8_t *buf,
			  size_t n)
{
	size_t off = 0;
	int i = 0;
	char key[64], val[96];

	while (off + 4 <= n) {
		const uint16_t kl = le16(buf + off);
		const uint16_t vl = le16(buf + off + 2);
		const size_t klen = kl & KVH_LEN_MASK, vlen = vl & KVH_LEN_MASK;
		const size_t kpart = (kl & KVH_SPILL) ? 12u : klen;
		const size_t vpart = (vl & KVH_SPILL) ? 8u : vlen;

		off += 4;
		if (kpart + vpart > n - off) {
			shell_print(sh, "  [%d] runs past the end", i);
			return;
		}
		const uint8_t *k = buf + off, *v = buf + off + kpart;

		if (kl & KVH_SPILL) {
			snprintk(key, sizeof(key), "spilled %zu B fp=%08x "
				 "-> %llu", klen, le32(k),
				 (unsigned long long)le64(k + 4));
		} else {
			preview(key, sizeof(key), k, klen);
		}
		if (vl & KVH_SPILL) {
			snprintk(val, sizeof(val), "spilled %zu B -> %llu",
				 vlen, (unsigned long long)le64(v));
		} else if (vlen == BFS_DIRENT_LEN && (v[0] == 1 || v[0] == 2) &&
			   v[1] == 1) {
			/* A blobfs directory entry. */
			snprintk(val, sizeof(val), "dirent %s -> %llu",
				 v[0] == 1 ? "file body" : "dir root",
				 (unsigned long long)le64(v + 8));
		} else {
			preview(val, sizeof(val), v, vlen);
		}
		shell_print(sh, "  [%d] %s = %s", i, key, val);
		off += kpart + vpart;
		i++;
	}
	if (off != n) {
		shell_print(sh, "  %zu trailing bytes", n - off);
	}
}

static void decode_clog(const struct shell *sh, const struct diag_class *c,
			const uint8_t *buf, size_t n)
{
	char rec[96];

	if (c->variant == CLOG_CHAIN) {
		if (n < CLOG_ROOT_LEN) {
			return;
		}
		shell_print(sh, "  chain root v%u, flags 0x%02x, epoch %08x",
			    buf[4], buf[5], le32(buf + 8));
		shell_print(sh, "  capacity %u B, live %u B, count %u (hints)",
			    le32(buf + 12), le32(buf + 16), le32(buf + 20));
		shell_print(sh, "  head -> %llu, waypoint -> %llu",
			    (unsigned long long)le64(buf + 32),
			    (unsigned long long)le64(buf + 24));
		if (le64(buf + 40)) {
			shell_print(sh, "  eviction pending: ids %llu..%llu",
				    (unsigned long long)le64(buf + 40),
				    (unsigned long long)le64(buf + 48));
		}
		shell_print(sh, "  follow the chain with "
			    "`diag show <id> logentry`");
		return;
	}
	const uint16_t used = le16(buf + 8);
	size_t off = CLOG_RING_HDR;

	shell_print(sh, "  ring v%u (inline), %u records, %u B used, "
		    "next_seq %llu", buf[4], le16(buf + 6), used,
		    n >= 20 ? (unsigned long long)le64(buf + 12) : 0ULL);
	while (off + CLOG_RING_REC <= MIN(n, CLOG_RING_HDR + used)) {
		const uint16_t len = le16(buf + off + 8);

		if (off + CLOG_RING_REC + len > n) {
			shell_print(sh, "  record at %zu runs past the end", off);
			return;
		}
		preview(rec, sizeof(rec), buf + off + CLOG_RING_REC, len);
		shell_print(sh, "  seq %-8llu %4u B  %s",
			    (unsigned long long)le64(buf + off), len, rec);
		off += CLOG_RING_REC + len;
	}
}

void diag_decode(const struct shell *sh, uint64_t id,
		 const struct diag_class *c, const uint8_t *buf, size_t n,
		 size_t size)
{
	char text[200];

	diag_summary(sh, "", id, c, buf, n, size, "");
	if (n < size) {
		shell_print(sh, "  (decoding the first %zu of %zu bytes)", n,
			    size);
	}
	switch (c->type) {
	case DIAG_T_RREG:
		decode_rreg(sh, buf, n);
		break;
	case DIAG_T_KVDB_META:
		decode_kvdb(sh, buf, n);
		break;
	case DIAG_T_BLOBFS_META:
		decode_blobfs(sh, c, buf, n, size);
		break;
	case DIAG_T_PADB_SB:
		decode_padb(sh, buf, n);
		break;
	case DIAG_T_KVHASH_DIR:
		if (n >= KVH_HDR) {
			decode_kvhash(sh, c, buf, n);
		}
		break;
	case DIAG_T_KVHASH_BUCKET:
		decode_bucket(sh, buf, n);
		break;
	case DIAG_T_CLOG_ROOT:
		if (n >= CLOG_RING_HDR) {
			decode_clog(sh, c, buf, n);
		}
		break;
	case DIAG_T_CLOG_ENTRY:
		if (n >= CLOG_ENTRY_HDR) {
			const uint16_t len = le16(buf + 8);

			shell_print(sh, "  next -> %llu",
				    (unsigned long long)le64(buf));
			preview(text, sizeof(text), buf + CLOG_ENTRY_HDR,
				MIN((size_t)len, n - CLOG_ENTRY_HDR));
			shell_print(sh, "  record %u B: %s", len, text);
		}
		break;
	case DIAG_T_CBOR:
		if (diag_cbor_format(buf, n, text, sizeof(text)) > 0) {
			shell_print(sh, "  %s", text);
		}
		break;
	case DIAG_T_EMPTY:
		break;
	default:
		hexdump(sh, buf, n, 64);
		break;
	}
}
