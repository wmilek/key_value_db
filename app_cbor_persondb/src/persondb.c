/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * persondb — the person management API, on L2 map_ops + rootreg + blob_db.
 *
 * Layout (DESIGN.md §12). One registry key, one app-owned superblock, N + 1
 * maps; everything is reachable from the integer 1 (P5), and boot costs two
 * sector reads rather than the eighteen an equivalent set of named kvdb
 * instances would:
 *
 *   rootreg[ ROOTREG_KEY('PADB', 1) ] -> superblock -> people_root[0..N-1]
 *                                                   -> cred_root
 *
 * This is the only file in the application that mentions a key, a shard, a
 * blob id or a map operation.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/kvhash.h>
#include <app/lib/rootreg.h>

#include "person_cbor.h"
#include "persondb.h"

LOG_MODULE_REGISTER(persondb, CONFIG_APP_CBOR_PERSONDB_LOG_LEVEL);

#define PADB_MAGIC        0x50414442u /* 'PADB' */
#define PADB_ROOTREG_KEY  ROOTREG_KEY(PADB_MAGIC, 1)
#define SUPERBLOCK_VERSION 1

#define N_PEOPLE_MAPS CONFIG_APP_CBOR_PERSONDB_PEOPLE_MAPS

/* Person keys are "pXXXXXXXX": fixed width, so every key is 9 bytes and the
 * entry-size arithmetic in DESIGN.md §6 has no variance from key length. */
#define PERSON_KEY_SZ 10

/*
 * kvhash's capacity formula is private to kvhash.c, so an application that
 * wants a full-size map has to restate it — FINDINGS.md K9(c). Worse, the
 * request is silently clamped: ask for more than this and `create` returns 0
 * having given you fewer buckets, with no way to read back what you got
 * (K9(b), K10). Restating it here at least makes the assumption checkable.
 */
#define KVHASH_DIR_HDR_LEN 8u
#define KVHASH_MAX_BUCKETS ((CONFIG_BLOB_DB_MAX_PAYLOAD_LEN - KVHASH_DIR_HDR_LEN) / 8u)

/*
 * A payload must fit twice between the bucket header and the sector end, or
 * it can be written once and never rewritten (FINDINGS.md B10). Nothing in
 * blob_db checks this (B9), so the app does — at build time against the
 * geometry it expects, and again at open() against the geometry it got.
 */
#define SLOT_OVERHEAD    14u
#define BUCKET_HDR_LEN   16u
#define MIN_SECTOR_FOR_PAYLOAD \
	(((CONFIG_BLOB_DB_MAX_PAYLOAD_LEN + SLOT_OVERHEAD) * 2u) + BUCKET_HDR_LEN)

BUILD_ASSERT(CONFIG_BLOB_DB_SECTOR_BUF_SIZE >= MIN_SECTOR_FOR_PAYLOAD,
	     "CONFIG_BLOB_DB_MAX_PAYLOAD_LEN cannot be rewritten in a sector "
	     "this size — see FINDINGS.md B10");
BUILD_ASSERT(KVHASH_MAX_BUCKETS >= 2, "payload too small for a bucket directory");

struct persondb {
	bool                  open;
	uint64_t              sb_id;
	struct superblock     sb;
	const struct map_ops *ops;
	struct persondb_stat  st;
};

/* One store per image: blob_db is a single global instance and the whole
 * stack is single-threaded (C7), so a second handle would be a lie. */
static struct persondb g_db;

/* -- plumbing ----------------------------------------------------------- */

static uint32_t fnv1a32(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t h = 0x811c9dc5u;

	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 0x01000193u;
	}
	return h;
}

/* Shard on a hash of the id rather than the id itself, so persondb stays
 * independent of how the caller numbers people — a caller allocating ids
 * sequentially and one allocating them sparsely get the same distribution,
 * which is what DESIGN.md §6.1's sizing assumes. */
static uint64_t people_root(struct persondb *db, uint32_t id)
{
	return db->sb.people_root[fnv1a32(&id, sizeof(id)) % db->sb.n_people_maps];
}

static void person_key(char *buf, uint32_t id)
{
	snprintf(buf, PERSON_KEY_SZ, "p%08X", (unsigned)id);
}

/* Every map operation is counted here because no layer below counts anything
 * (FINDINGS.md B3). A get is two blob_db operations — directory then bucket
 * (K11) — and a set or delete is three; each of those moves a whole sector
 * (B1). That is the entire basis of the amplification figures. */
static int map_get(struct persondb *db, uint64_t root, const char *key,
		   void *out, size_t out_sz, size_t *out_len)
{
	db->st.map_gets++;
	db->st.blob_ops += 2;
	return db->ops->get(root, key, strlen(key), out, out_sz, out_len);
}

static int map_set(struct persondb *db, uint64_t root, const char *key,
		   const void *val, size_t len)
{
	int rc;

	db->st.map_sets++;
	db->st.blob_ops += 3;
	rc = db->ops->set(root, key, strlen(key), val, len);
	if (rc == -ENOSPC) {
		/* A single bucket overflowed while the medium is nearly empty
		 * (K2). DESIGN.md §6.1 sizes this away; if it fires, that
		 * sizing rule was wrong (A8). */
		db->st.enospc_hits++;
		LOG_ERR("bucket overflow on key '%s' — see FINDINGS.md K2", key);
	}
	return rc;
}

static int map_del(struct persondb *db, uint64_t root, const char *key)
{
	db->st.map_dels++;
	db->st.blob_ops += 3;
	return db->ops->del(root, key, strlen(key));
}

static int cred_put(struct persondb *db, const char *card, uint32_t person_id)
{
	uint8_t buf[CRED_CBOR_MAX];
	size_t len;
	int rc = cred_cbor_encode(person_id, buf, sizeof(buf), &len);

	if (rc != 0) {
		return rc;
	}
	return map_set(db, db->sb.cred_root, card, buf, len);
}

static int sb_commit(struct persondb *db)
{
	uint8_t buf[SUPERBLOCK_CBOR_MAX];
	size_t len;
	int rc = superblock_cbor_encode(&db->sb, buf, sizeof(buf), &len);

	if (rc != 0) {
		return rc;
	}
	db->st.blob_ops++;
	return blob_db_update(db->sb_id, buf, len);
}

/* -- geometry ----------------------------------------------------------- */

/*
 * blob_db knows the partition size, the sector size and the bucket count, and
 * exposes none of them (FINDINGS.md B3). To answer "how full is the store, as
 * a fraction of the external flash" — which is the whole of R-E — the
 * application has to rediscover the geometry through the same flash_area API
 * the library used, behind its back.
 */
static int geometry(size_t *partition_bytes, size_t *sector_bytes)
{
	const struct flash_area *fa;
	struct flash_sector sector;
	uint32_t count = 1;
	int rc = flash_area_open(PARTITION_ID(storage_partition), &fa);

	if (rc < 0) {
		return rc;
	}

	*partition_bytes = fa->fa_size;
	rc = flash_area_sectors(fa, &count, &sector);
	if ((rc < 0 && rc != -ENOMEM) || count == 0) {
		flash_area_close(fa);
		return rc < 0 ? rc : -EIO;
	}
	*sector_bytes = sector.fs_size;

	flash_area_close(fa);
	return 0;
}

/* -- lifecycle ---------------------------------------------------------- */

/*
 * Build a virgin store: allocate the map roots, create each map, and bind the
 * superblock LAST. That single write publishes the whole structure atomically.
 *
 * A crash before it leaves the maps allocated and unreferenced, and blob_db
 * has no reachability GC — compaction reclaims tombstones and superseded
 * slots only — so those blobs are lost until a format (FINDINGS.md B8). It is
 * the same window kvdb has at kvdb.c:118, and the reason P7's "no permanent
 * leak" does not currently hold for any multi-blob structure.
 */
static int create_store(struct persondb *db, uint32_t n_persons)
{
	struct superblock *sb = &db->sb;

	memset(sb, 0, sizeof(*sb));
	sb->version = SUPERBLOCK_VERSION;
	sb->n_people_maps = N_PEOPLE_MAPS;
	sb->n_persons = n_persons;

	const struct map_config cfg = { .initial_capacity = KVHASH_MAX_BUCKETS };

	for (uint8_t i = 0; i < sb->n_people_maps; i++) {
		sb->people_root[i] = blob_db_alloc_id();
		if (sb->people_root[i] == 0) {
			return -EIO;
		}
		int rc = db->ops->create(sb->people_root[i], &cfg);

		if (rc != 0) {
			return rc;
		}
	}

	sb->cred_root = blob_db_alloc_id();
	if (sb->cred_root == 0) {
		return -EIO;
	}

	int rc = db->ops->create(sb->cred_root, &cfg);

	if (rc != 0) {
		return rc;
	}

	LOG_INF("created store: %u people maps + 1 credential map, "
		"%u buckets each, %u persons planned",
		sb->n_people_maps, (unsigned)KVHASH_MAX_BUCKETS, n_persons);

	return sb_commit(db);   /* the commit point */
}

int persondb_open(struct persondb **out, uint32_t n_persons)
{
	struct persondb *db = &g_db;
	size_t partition_bytes, sector_bytes;
	int rc;

	if (out == NULL) {
		return -EINVAL;
	}

	memset(&db->st, 0, sizeof(db->st));
	db->ops = &kvhash_map_ops;

	rc = geometry(&partition_bytes, &sector_bytes);
	if (rc != 0) {
		LOG_ERR("cannot read partition geometry: %d", rc);
		return rc;
	}

	/* The build-time assert used CONFIG_BLOB_DB_SECTOR_BUF_SIZE, which is
	 * only an upper bound. This is the real geometry. */
	if (MIN_SECTOR_FOR_PAYLOAD > sector_bytes) {
		LOG_ERR("payload %u needs a sector of at least %u B, this "
			"flash has %zu B — a full bucket could be written once "
			"and never rewritten (FINDINGS.md B10)",
			(unsigned)CONFIG_BLOB_DB_MAX_PAYLOAD_LEN,
			(unsigned)MIN_SECTOR_FOR_PAYLOAD, sector_bytes);
		return -ENOTSUP;
	}

	db->st.partition_bytes = partition_bytes;
	db->st.sector_bytes = sector_bytes;

	rc = rootreg_get_or_create(PADB_ROOTREG_KEY, &db->sb_id);
	if (rc != 0) {
		LOG_ERR("rootreg: %d", rc);
		return rc;
	}

	uint8_t buf[SUPERBLOCK_CBOR_MAX];
	size_t got = 0;

	db->st.blob_ops++;
	rc = blob_db_get(db->sb_id, buf, sizeof(buf), &got);
	if (rc == -ENOENT || (rc == 0 && got == 0)) {
		/* Virgin, or a crash before a previous create's commit. */
		rc = create_store(db, n_persons);
	} else if (rc == 0) {
		rc = superblock_cbor_decode(buf, got, &db->sb);
		if (rc == 0 && db->sb.version != SUPERBLOCK_VERSION) {
			LOG_ERR("superblock version %u, expected %u",
				db->sb.version, SUPERBLOCK_VERSION);
			rc = -EIO;
		}
		if (rc == 0 && db->sb.n_people_maps != N_PEOPLE_MAPS) {
			/* The shard count is baked into where every key lives,
			 * so a build that disagrees cannot read the store.
			 * kvhash cannot rehash (K3) and cannot be iterated
			 * (K6), so there is no migration path — say so plainly
			 * rather than returning wrong answers. */
			LOG_ERR("store has %u people maps, this build has %u — "
				"reformat required (FINDINGS.md K3/K6)",
				db->sb.n_people_maps, N_PEOPLE_MAPS);
			rc = -ENOTSUP;
		}
	}
	if (rc != 0) {
		return rc;
	}

	db->st.buckets_per_map = KVHASH_MAX_BUCKETS;
	db->open = true;
	*out = db;
	return 0;
}

int persondb_close(struct persondb *db)
{
	if (db) {
		db->open = false;
	}
	return 0;
}

/* -- enrollment --------------------------------------------------------- */

int persondb_person_put(struct persondb *db, const struct persondb_person *p)
{
	uint8_t buf[PERSON_CBOR_MAX];
	char key[PERSON_KEY_SZ];
	size_t len;
	int rc = person_cbor_encode(p, buf, sizeof(buf), &len);

	if (rc != 0) {
		return rc;
	}

	person_key(key, p->id);
	rc = map_set(db, people_root(db, p->id), key, buf, len);
	if (rc != 0) {
		return rc;
	}

	/* Record first, index second (F5): a crash here leaves a card the
	 * person lists but that resolves to nothing, which denies. */
	for (uint8_t i = 0; i < p->n_cards; i++) {
		rc = cred_put(db, p->card[i], p->id);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

int persondb_person_get(struct persondb *db, uint32_t id,
			struct persondb_person *out)
{
	uint8_t buf[PERSON_CBOR_MAX];
	char key[PERSON_KEY_SZ];
	size_t len = 0;

	person_key(key, id);

	int rc = map_get(db, people_root(db, id), key, buf, sizeof(buf), &len);

	if (rc != 0) {
		return rc;
	}
	return person_cbor_decode(buf, len, out);
}

int persondb_person_delete(struct persondb *db, uint32_t id)
{
	struct persondb_person p;
	int rc = persondb_person_get(db, id, &p);

	if (rc != 0) {
		return rc;
	}

	/* Credentials first (F5): after each delete the card already resolves
	 * to nothing, so every crash point in this loop denies. */
	for (uint8_t i = 0; i < p.n_cards; i++) {
		rc = map_del(db, db->sb.cred_root, p.card[i]);
		if (rc != 0 && rc != -ENOENT) {
			return rc;
		}
	}

	char key[PERSON_KEY_SZ];

	person_key(key, id);
	return map_del(db, people_root(db, id), key);
}

/* -- credentials -------------------------------------------------------- */

int persondb_card_assign(struct persondb *db, uint32_t person_id,
			 const char *card)
{
	struct persondb_person p;
	int rc = persondb_person_get(db, person_id, &p);

	if (rc != 0) {
		return rc;
	}

	for (uint8_t i = 0; i < p.n_cards; i++) {
		if (strcmp(p.card[i], card) == 0) {
			return 0;   /* idempotent: replay of a batch is free */
		}
	}
	if (p.n_cards >= PERSONDB_CARDS_MAX) {
		return -ENOSPC;
	}

	snprintf(p.card[p.n_cards], sizeof(p.card[0]), "%s", card);
	p.n_cards++;

	/* Person first, index second — see F5. The reverse order would leave a
	 * credential that grants access on behalf of a person who does not
	 * list it: a crash that fails *open*. */
	uint8_t buf[PERSON_CBOR_MAX];
	size_t len;
	char key[PERSON_KEY_SZ];

	rc = person_cbor_encode(&p, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	person_key(key, person_id);
	rc = map_set(db, people_root(db, person_id), key, buf, len);
	if (rc != 0) {
		return rc;
	}

	return cred_put(db, card, person_id);
}

int persondb_card_revoke(struct persondb *db, const char *card)
{
	uint32_t person_id;
	int rc = persondb_card_owner(db, card, &person_id);

	if (rc != 0) {
		return rc;
	}

	/* Index first, person second — the mirror of assignment, fail-safe for
	 * the same reason: after this write the card resolves to nothing. */
	rc = map_del(db, db->sb.cred_root, card);
	if (rc != 0 && rc != -ENOENT) {
		return rc;
	}

	struct persondb_person p;

	rc = persondb_person_get(db, person_id, &p);
	if (rc != 0) {
		return rc;
	}

	uint8_t n = 0;

	for (uint8_t i = 0; i < p.n_cards; i++) {
		if (strcmp(p.card[i], card) != 0) {
			if (n != i) {
				memcpy(p.card[n], p.card[i], sizeof(p.card[0]));
			}
			n++;
		}
	}
	if (n == p.n_cards) {
		return 0;   /* already absent from the record */
	}
	p.n_cards = n;

	uint8_t buf[PERSON_CBOR_MAX];
	size_t len;
	char key[PERSON_KEY_SZ];

	rc = person_cbor_encode(&p, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	person_key(key, person_id);
	return map_set(db, people_root(db, person_id), key, buf, len);
}

int persondb_card_owner(struct persondb *db, const char *card,
			uint32_t *person_id)
{
	uint8_t buf[CRED_CBOR_MAX];
	size_t len = 0;
	int rc = map_get(db, db->sb.cred_root, card, buf, sizeof(buf), &len);

	if (rc != 0) {
		return rc;
	}
	return cred_cbor_decode(buf, len, person_id);
}

/* -- permissions -------------------------------------------------------- */

static int person_store(struct persondb *db, const struct persondb_person *p)
{
	uint8_t buf[PERSON_CBOR_MAX];
	char key[PERSON_KEY_SZ];
	size_t len;
	int rc = person_cbor_encode(p, buf, sizeof(buf), &len);

	if (rc != 0) {
		return rc;
	}
	person_key(key, p->id);
	return map_set(db, people_root(db, p->id), key, buf, len);
}

int persondb_permission_grant(struct persondb *db, uint32_t person_id,
			      const char *perm)
{
	struct persondb_person p;
	int rc = persondb_person_get(db, person_id, &p);

	if (rc != 0) {
		return rc;
	}
	for (uint8_t i = 0; i < p.n_perms; i++) {
		if (strcmp(p.perm[i], perm) == 0) {
			return 0;
		}
	}
	if (p.n_perms >= PERSONDB_PERMS_MAX) {
		return -ENOSPC;
	}
	snprintf(p.perm[p.n_perms], sizeof(p.perm[0]), "%s", perm);
	p.n_perms++;

	/* One record, one atomic map set. Permissions live only in the person
	 * record, so — unlike credentials — there is no second structure to
	 * keep in step and no ordering to get right. That is a consequence of
	 * refusing to denormalize (D2), not an accident. */
	return person_store(db, &p);
}

int persondb_permission_revoke(struct persondb *db, uint32_t person_id,
			       const char *perm)
{
	struct persondb_person p;
	int rc = persondb_person_get(db, person_id, &p);

	if (rc != 0) {
		return rc;
	}

	uint8_t n = 0;

	for (uint8_t i = 0; i < p.n_perms; i++) {
		if (strcmp(p.perm[i], perm) != 0) {
			if (n != i) {
				memcpy(p.perm[n], p.perm[i], sizeof(p.perm[0]));
			}
			n++;
		}
	}
	if (n == p.n_perms) {
		return -ENOENT;
	}
	p.n_perms = n;
	return person_store(db, &p);
}

/* -- the decision ------------------------------------------------------- */

int persondb_check(struct persondb *db, const char *card, const char *perm,
		   uint32_t now, enum persondb_decision *out,
		   struct persondb_person *who)
{
	struct persondb_person local;
	struct persondb_person *p = who ? who : &local;
	uint32_t person_id;

	int rc = persondb_card_owner(db, card, &person_id);

	if (rc == -ENOENT) {
		*out = PERSONDB_UNKNOWN_CARD;
		return 0;
	}
	if (rc != 0) {
		return rc;   /* never a grant */
	}

	rc = persondb_person_get(db, person_id, p);
	if (rc == -ENOENT) {
		/* The index resolved to a person that is not there. Neither
		 * write ordering in this file can produce that (F5), so it is
		 * real damage — deny, and say so. */
		LOG_ERR("credential %s resolves to absent person %u", card,
			person_id);
		*out = PERSONDB_UNKNOWN_CARD;
		return 0;
	}
	if (rc != 0) {
		return rc;
	}

	if (now < p->valid_from || now >= p->valid_until) {
		*out = PERSONDB_EXPIRED;
		return 0;
	}

	for (uint8_t i = 0; i < p->n_perms; i++) {
		if (strcmp(p->perm[i], perm) == 0) {
			*out = PERSONDB_GRANTED;
			return 0;
		}
	}
	*out = PERSONDB_DENIED;
	return 0;
}

const char *persondb_decision_str(enum persondb_decision d)
{
	switch (d) {
	case PERSONDB_GRANTED:      return "GRANTED";
	case PERSONDB_DENIED:       return "DENIED";
	case PERSONDB_UNKNOWN_CARD: return "UNKNOWN_CARD";
	case PERSONDB_EXPIRED:      return "EXPIRED";
	default:                    return "?";
	}
}

/* -- app-owned persistent state ----------------------------------------- */

int persondb_progress_get(struct persondb *db, uint32_t *populated,
			  uint32_t *rev)
{
	if (populated) {
		*populated = db->sb.populated;
	}
	if (rev) {
		*rev = db->sb.rev;
	}
	return 0;
}

int persondb_progress_set(struct persondb *db, uint32_t populated, uint32_t rev)
{
	db->sb.populated = populated;
	db->sb.rev = rev;
	return sb_commit(db);
}

/* -- introspection ------------------------------------------------------ */

int persondb_stat(struct persondb *db, struct persondb_stat *out)
{
	*out = db->st;
	out->n_persons = db->sb.n_persons;
	out->populated = db->sb.populated;
	out->rev = db->sb.rev;
	out->n_people_maps = db->sb.n_people_maps;
	return 0;
}

void persondb_counters_reset(struct persondb *db)
{
	db->st.map_gets = 0;
	db->st.map_sets = 0;
	db->st.map_dels = 0;
	db->st.blob_ops = 0;
}
