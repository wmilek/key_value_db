/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * persondb — the person management API, on L2 map_ops + rootreg + blob_db.
 *
 * Layout (DESIGN.md §12). One registry key, one app-owned superblock, two
 * kvhash instances; everything is reachable from the integer 1 (P5):
 *
 *   rootreg[ ROOTREG_KEY('PADB', 0) ] -> superblock -> people_root[0]
 *                                                   -> cred_root
 *
 * Two containers, because the domain has two collections. The people map was
 * sixteen shards until DESIGN.md §12.2, to keep any one bucket under a 4 KB
 * payload cap that has since been lifted; that was a workaround for K2, and
 * workarounds hide the limitations this application exists to find (DESIGN.md
 * §1). The loop below still runs over n_people_maps because the superblock
 * format carries the count -- it is 1.
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
/* Instance 0: this application is one database, and a database is one
 * structure with one root (P5) -- so it takes one registry entry, and that
 * entry is the magic's first. The instance field exists to distinguish a
 * *second*, independent store of the same type (rootreg contract, section 3);
 * numbering the only one 1 would imply a ('PADB', 0) that does not exist.
 */
#define PADB_ROOTREG_KEY  ROOTREG_KEY(PADB_MAGIC, 0)
#define SUPERBLOCK_VERSION 1

#define N_PEOPLE_MAPS CONFIG_APP_CBOR_PERSONDB_PEOPLE_MAPS

/* Person keys are "pXXXXXXXX": fixed width, so every key is 9 bytes and the
 * entry-size arithmetic in DESIGN.md §6 has no variance from key length. */
#define PERSON_KEY_SZ 10

/* kvhash charges 4 bytes of framing per entry, on top of key and value. */
#define ENTRY_OVERHEAD 4u
#define PERSON_KEY_LEN 9u

/*
 * This app used to restate blob_db's slot and bucket-header sizes here, to
 * assert that CONFIG_BLOB_DB_MAX_PAYLOAD_LEN could actually be *rewritten* on
 * the target geometry (FINDINGS.md B9/B10, which this app raised).
 *
 * That is gone. blob_db now performs the same check at mount, against the real
 * peb_size instead of the CONFIG_BLOB_DB_SECTOR_BUF_SIZE upper bound the app
 * had to use as a proxy — so the library's version is not merely equivalent,
 * it is strictly correct where the app's could pass and then fail at mount
 * anyway. Three private constants (slot overhead, bucket header, the
 * sustainable-payload formula) left the application with it.
 *
 * What is left below is the same problem unsolved one layer up: kvhash's
 * capacity formula is still private, so the app still restates it.
 */
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

/* Map-level counters. The flash bytes underneath come from blob_db's own
 * iostats (see persondb_stat) rather than from a model of what a map operation
 * ought to cost — the model was "two blob_db ops per get, each a whole sector",
 * and slot-header walking made it wrong. */
static int map_get(struct persondb *db, uint64_t root, const char *key,
		   void *out, size_t out_sz, size_t *out_len)
{
	db->st.map_gets++;
	return db->ops->get(root, key, strlen(key), out, out_sz, out_len);
}

static int map_set(struct persondb *db, uint64_t root, const char *key,
		   const void *val, size_t len)
{
	int rc;

	db->st.map_sets++;
	rc = db->ops->set(root, key, strlen(key), val, len);
	if (rc == -ENOSPC) {
		/* Two different walls arrive here as the same errno, and the
		 * application cannot tell them apart:
		 *
		 *   K2   a single bucket overflowed while the medium is nearly
		 *        empty — the sizing rule of DESIGN.md §6.1 was wrong (A8)
		 *   K13  the bucket directory can no longer be placed in any
		 *        erase block, so the map cannot take a key in a bucket
		 *        it has not used yet — at the shipped payload this is
		 *        the store's real ceiling, 9 670 persons
		 *
		 * The message below names K2 because that is the one an
		 * application can act on. It is wrong for K13, and there is no
		 * query that would let it be right: no per-bucket occupancy
		 * (K10), no physical occupancy (B3), no fill level (V3). The
		 * misdiagnosis is left in place, and recorded in K13, because
		 * it is what the stack leaves an application able to say.
		 */
		db->st.enospc_hits++;
		LOG_ERR("bucket overflow on key '%s' — see FINDINGS.md K2", key);
	}
	return rc;
}

static int map_del(struct persondb *db, uint64_t root, const char *key)
{
	db->st.map_dels++;
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

/* Encode a person and write it to its shard. One map set, so one atomic write:
 * a crash either leaves the previous record or this one, never a blend. */
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

static bool person_lists_card(const struct persondb_person *p, const char *card)
{
	for (uint8_t i = 0; i < p->n_cards; i++) {
		if (strcmp(p->card[i], card) == 0) {
			return true;
		}
	}
	return false;
}

/*
 * Reject a card id that cannot be stored *consistently*, before the first write
 * rather than after.
 *
 * The record holds a fixed-width copy of the id while the credential index is
 * keyed by the caller's string at its full length, so an over-long id would be
 * silently truncated on one side and not the other: the card would resolve
 * through the index to a person whose record lists a different string. Nothing
 * afterwards could reconcile them — revoke would delete the index entry and
 * then fail to find the card in the record, leaving it listed forever.
 *
 * An empty id is rejected for a duller reason with the same shape: kvhash
 * refuses a zero-length key with -EINVAL, which lands *after* the record has
 * been written, leaving a partially-applied change.
 */
static int card_valid(const char *card)
{
	size_t n = card ? strlen(card) : 0;

	return (n == 0 || n > PERSONDB_CARD_LEN) ? -EINVAL : 0;
}

/* Same argument, one structure: a truncated permission is stored, and then no
 * caller can revoke it by the name they granted. */
static int perm_valid(const char *perm)
{
	size_t n = perm ? strlen(perm) : 0;

	return (n == 0 || n > PERSONDB_PERM_MAX) ? -EINVAL : 0;
}

static int sb_commit(struct persondb *db)
{
	uint8_t buf[SUPERBLOCK_CBOR_MAX];
	size_t len;
	int rc = superblock_cbor_encode(&db->sb, buf, sizeof(buf), &len);

	if (rc != 0) {
		return rc;
	}
	return blob_db_update(db->sb_id, buf, len);
}

/* -- geometry ----------------------------------------------------------- */

/*
 * The one number this app still has to fetch behind blob_db's back: the size of
 * the medium, so "how full is the store, as a fraction of the external flash"
 * — the whole of R-E — can be answered at all. blob_db computes it at mount and
 * exposes nothing (FINDINGS.md B3, X1 row 7).
 *
 * Two hazards come with it, and the app can detect neither:
 *
 *  - It names `storage_partition` directly, because that is what the
 *    flash_area backend hardcodes. CONFIG_BLOB_DB_PARTITION_LABEL looks like
 *    the right thing to honour instead and is never read by anything (B11).
 *  - On CONFIG_BLOB_DB_BACKEND_UBI the answer is simply wrong: blob_db is on a
 *    UBI volume over a *different* partition, its erase unit is a UBI LEB
 *    rather than a flash sector, and UBI reserves PEBs the raw partition size
 *    knows nothing about. The fill percentage would be quietly computed
 *    against the wrong denominator.
 *
 * The sector size used to be fetched here too, to model flash traffic as
 * "operations x sector size". blob_db_iostats_get() measures that directly
 * now, so it is gone — the leak shrank because a real API replaced a guess.
 */
static int geometry(size_t *partition_bytes)
{
	const struct flash_area *fa;
	int rc = flash_area_open(PARTITION_ID(storage_partition), &fa);

	if (rc < 0) {
		return rc;
	}
	*partition_bytes = fa->fa_size;
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

	/*
	 * "Give me the largest map you can build."
	 *
	 * This app wants every bucket it can get — more buckets means a lower
	 * load factor and more distance from K2's per-bucket cliff — and there
	 * is no way to *ask* for the maximum. But kvhash clamps any request
	 * above its capacity down to it, so asking for more than could possibly
	 * fit expresses the intent exactly, and the application never learns the
	 * formula.
	 *
	 * What this costs is now visible, and is left visible. The clamp lands
	 * on 2047 buckets, so the directory is a 16 384 B blob — and kvhash
	 * reads all of it on every get, set and delete (K11). Enumerating
	 * offline says ~683 buckets would move 40 % of those bytes
	 * (tools/sizing.py). The app does not pass 683: hand-tuning a bucket
	 * count against another layer's read amplification is a workaround, and
	 * it would bury K11 under a magic number. The number is measured
	 * instead (RESULTS.md).
	 *
	 * "The largest map you can build" is also, at this geometry's largest
	 * payload, a map that cannot be written to — 4089 buckets make the
	 * directory an entire erase block. prj.conf carries that arithmetic and
	 * FINDINGS.md K12 the finding.
	 *
	 * This used to restate `(MAX_PAYLOAD - 8) / 8` here. That was a private
	 * constant from another layer, wrong by 8x after one Kconfig edit, and
	 * it bought nothing: the number was only ever passed straight back down.
	 *
	 * Note what this does to K9. The silent clamp is a defect when you ask
	 * for a specific size and quietly get less — and it is the very thing
	 * that makes "as much as possible" expressible. Both are true.
	 */
	const struct map_config cfg = { .initial_capacity = SIZE_MAX };

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

	LOG_INF("created store: %u people map(s) + 1 credential map, "
		"max buckets each, %u persons planned",
		sb->n_people_maps, n_persons);

	return sb_commit(db);   /* the commit point */
}

int persondb_open(struct persondb **out, uint32_t n_persons)
{
	struct persondb *db = &g_db;
	size_t partition_bytes;
	int rc;

	if (out == NULL) {
		return -EINVAL;
	}

	memset(&db->st, 0, sizeof(db->st));
	db->ops = &kvhash_map_ops;

	/* This module owns the storage stack end to end, so nothing above it
	 * ever names blob_db or rootreg (DESIGN.md F12, checked by A7). */
	rc = blob_db_mount();
	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("blob_db_mount: %d", rc);
		return rc;
	}

	if (IS_ENABLED(CONFIG_APP_CBOR_PERSONDB_FRESH_START)) {
		rc = blob_db_erase_all();
		if (rc != 0) {
			LOG_ERR("erase_all: %d", rc);
			return rc;
		}
	}

	rc = rootreg_init();
	if (rc != 0) {
		LOG_ERR("rootreg_init: %d", rc);
		return rc;
	}

	rc = geometry(&partition_bytes);
	if (rc != 0) {
		LOG_ERR("cannot read partition geometry: %d", rc);
		return rc;
	}

	db->st.partition_bytes = partition_bytes;

	rc = rootreg_get_or_create(PADB_ROOTREG_KEY, &db->sb_id);
	if (rc != 0) {
		LOG_ERR("rootreg: %d", rc);
		return rc;
	}

	uint8_t buf[SUPERBLOCK_CBOR_MAX];
	size_t got = 0;

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

	db->open = true;
	*out = db;
	return 0;
}

int persondb_close(struct persondb *db)
{
	if (db) {
		db->open = false;
	}
	blob_db_unmount();
	return 0;
}

int persondb_erase(struct persondb **db, uint32_t n_persons)
{
	int rc;

	if (db && *db) {
		(*db)->open = false;
		*db = NULL;
	}

	/* Logical wipe: one master write, rather than the tens of seconds a
	 * full-partition erase costs on an 8 MiB QSPI part. */
	rc = blob_db_erase_all();
	if (rc != 0) {
		return rc;
	}
	rc = rootreg_init();
	if (rc != 0) {
		return rc;
	}
	return persondb_open(db, n_persons);
}

int persondb_prepare(int *buckets_formatted)
{
	/* How many buckets to ask for is a guess: blob_db reports neither its
	 * bucket count nor which are already formatted (FINDINGS.md B3/B7).
	 * Asking for more than exist is harmless — the call caps at the total. */
	int n = blob_db_prepare((size_t)-1);

	if (buckets_formatted) {
		*buckets_formatted = (n < 0) ? 0 : n;
	}
	return n < 0 ? n : 0;
}

/* -- enrollment --------------------------------------------------------- */

int persondb_person_put(struct persondb *db, const struct persondb_person *p)
{
	struct persondb_person old;
	bool replacing;
	int rc;

	if (p->n_cards > PERSONDB_CARDS_MAX || p->n_perms > PERSONDB_PERMS_MAX) {
		return -EINVAL;
	}
	for (uint8_t i = 0; i < p->n_cards; i++) {
		rc = card_valid(p->card[i]);
		if (rc != 0) {
			return rc;
		}
	}

	/*
	 * This operation *replaces*, so the cards the previous version listed
	 * and this one does not must lose their index entries. Skipping that
	 * reaches the one outcome F5 exists to prevent — a credential granting
	 * access on behalf of a person who does not list it — and reaches it
	 * with no crash involved at all, just two ordinary calls.
	 *
	 * The two halves therefore take opposite orders, which is F5 applied
	 * twice: drop an index entry *before* the record stops listing its
	 * card, add one *after* the record starts. Every intermediate state
	 * denies, in both directions.
	 *
	 * The cost is one extra map get per put, paid on inserts too, where it
	 * only ever answers -ENOENT. That is the price of the operation meaning
	 * what its name says; a caller that knows it is inserting still pays it.
	 */
	rc = persondb_person_get(db, p->id, &old);
	if (rc != 0 && rc != -ENOENT) {
		return rc;
	}
	replacing = (rc == 0);

	if (replacing) {
		for (uint8_t i = 0; i < old.n_cards; i++) {
			if (person_lists_card(p, old.card[i])) {
				continue;
			}
			rc = map_del(db, db->sb.cred_root, old.card[i]);
			if (rc != 0 && rc != -ENOENT) {
				return rc;
			}
		}
	}

	rc = person_store(db, p);
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
	uint32_t owner;
	int rc = card_valid(card);

	if (rc != 0) {
		return rc;
	}

	/*
	 * A card is a physical object: it is in one person's hand or nobody's.
	 * Without this check, assigning a card that is already live silently
	 * leaves its previous holder still listing it — and deleting *that*
	 * person then walks their record and revokes the new holder's working
	 * credential. Fail-closed, but a lockout with no visible cause.
	 */
	rc = persondb_card_owner(db, card, &owner);
	if (rc == 0 && owner != person_id) {
		return -EEXIST;
	}
	if (rc != 0 && rc != -ENOENT) {
		return rc;
	}

	rc = persondb_person_get(db, person_id, &p);
	if (rc != 0) {
		return rc;
	}

	if (!person_lists_card(&p, card)) {
		if (p.n_cards >= PERSONDB_CARDS_MAX) {
			return -ENOSPC;
		}
		snprintf(p.card[p.n_cards], sizeof(p.card[0]), "%s", card);
		p.n_cards++;

		/* Person first, index second — see F5. The reverse order would
		 * leave a credential that grants access on behalf of a person
		 * who does not list it: a crash that fails *open*. */
		rc = person_store(db, &p);
		if (rc != 0) {
			return rc;
		}
	}

	/*
	 * Deliberately not an `else`. When the record already lists the card,
	 * the index entry may still be missing — that is precisely the state a
	 * crash between the two writes above leaves behind, and it is the state
	 * a redo has to *finish*, not skip.
	 *
	 * This function used to return 0 here, calling itself idempotent. It was
	 * idempotent in the weak sense that redoing it wrote nothing; it was not
	 * *convergent*, so an interrupted assign became permanent — the card
	 * stayed listed and unresolvable, and every later verify reported it.
	 * Fail-safe is not the same property as repairable, and ordering buys
	 * only the first (F5, C8). Making the second write unconditional buys
	 * the second, for the cost of one map set on a path only replay reaches.
	 */
	return cred_put(db, card, person_id);
}

int persondb_card_revoke_from(struct persondb *db, uint32_t person_id,
			      const char *card)
{
	struct persondb_person p;
	uint8_t n = 0;
	int rc = card_valid(card);

	if (rc != 0) {
		return rc;
	}

	/* Index first, person second — the mirror of assignment, fail-safe for
	 * the same reason: after this write the card resolves to nothing. */
	rc = map_del(db, db->sb.cred_root, card);
	if (rc != 0 && rc != -ENOENT) {
		return rc;
	}

	/* -ENOENT above is tolerated rather than reported, which is what makes
	 * a redo converge: the caller named the person, so the second write can
	 * still be completed whether or not the first one already landed. */
	rc = persondb_person_get(db, person_id, &p);
	if (rc != 0) {
		return rc;
	}

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

	return person_store(db, &p);
}

int persondb_card_revoke(struct persondb *db, const char *card)
{
	uint32_t person_id;
	int rc = card_valid(card);

	if (rc != 0) {
		return rc;
	}

	/*
	 * Resolving the owner through the index is the only way to reach the
	 * record from a card alone — and it is exactly why this form cannot
	 * repair itself. Once the index entry is gone the owner is unknown, so
	 * an interrupted revoke redone here returns -ENOENT and stops, with the
	 * card still listed in a record nobody can now identify.
	 *
	 * A caller that can name the person should call
	 * persondb_card_revoke_from() instead and get a redo that converges.
	 * The asymmetry is not an oversight in this API: it is what a single
	 * index and no multi-key transaction (C8) actually buy you.
	 */
	rc = persondb_card_owner(db, card, &person_id);
	if (rc != 0) {
		return rc;
	}
	return persondb_card_revoke_from(db, person_id, card);
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

int persondb_permission_grant(struct persondb *db, uint32_t person_id,
			      const char *perm)
{
	struct persondb_person p;
	int rc = perm_valid(perm);

	if (rc != 0) {
		return rc;
	}

	rc = persondb_person_get(db, person_id, &p);
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
	uint8_t n = 0;
	int rc = perm_valid(perm);

	if (rc != 0) {
		return rc;
	}

	rc = persondb_person_get(db, person_id, &p);
	if (rc != 0) {
		return rc;
	}

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
		/* The index resolved to a person that is not there. No sequence
		 * of operations in this file can produce that: every write
		 * order here drops an index entry before, and adds it after,
		 * the record that owns it (F5) — including the replace path in
		 * persondb_person_put(), which is where this claim was false
		 * until it learned to unindex the cards it drops. So this is
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

/* -- record helpers ----------------------------------------------------- */

bool persondb_person_equal(const struct persondb_person *a,
			   const struct persondb_person *b)
{
	uint8_t ba[PERSON_CBOR_MAX], bb[PERSON_CBOR_MAX];
	size_t la, lb;

	/* An unencodable person is reported unequal even to an identical one.
	 * That is deliberate: this function answers "are these the same *stored*
	 * record", and something that cannot be encoded was never stored. The
	 * safe direction is also the useful one — verification flags it instead
	 * of passing on a record neither side could have written. */
	if (person_cbor_encode(a, ba, sizeof(ba), &la) != 0 ||
	    person_cbor_encode(b, bb, sizeof(bb), &lb) != 0) {
		return false;
	}
	return la == lb && memcmp(ba, bb, la) == 0;
}

size_t persondb_person_record_bytes(const struct persondb_person *p)
{
	uint8_t buf[PERSON_CBOR_MAX];
	size_t len = 0;

	if (person_cbor_encode(p, buf, sizeof(buf), &len) != 0) {
		return 0;
	}
	return ENTRY_OVERHEAD + PERSON_KEY_LEN + len;
}

size_t persondb_person_credential_bytes(const struct persondb_person *p)
{
	uint8_t buf[CRED_CBOR_MAX];
	size_t total = 0;

	for (uint8_t i = 0; i < p->n_cards; i++) {
		size_t len = 0;

		if (cred_cbor_encode(p->id, buf, sizeof(buf), &len) != 0) {
			return 0;
		}
		total += ENTRY_OVERHEAD + PERSONDB_CARD_LEN + len;
	}
	return total;
}

int persondb_person_roundtrip(const struct persondb_person *p,
			      size_t *encoded_len)
{
	uint8_t buf[PERSON_CBOR_MAX];
	struct persondb_person tmp;
	size_t len = 0;
	int rc = person_cbor_encode(p, buf, sizeof(buf), &len);

	if (rc != 0) {
		return rc;
	}
	rc = person_cbor_decode(buf, len, &tmp);
	if (rc != 0) {
		return rc;
	}
	if (encoded_len) {
		*encoded_len = len;
	}
	return 0;
}

/* -- introspection ------------------------------------------------------ */

int persondb_stat(struct persondb *db, struct persondb_stat *out)
{
	*out = db->st;

#ifdef CONFIG_BLOB_DB_IOSTATS
	{
		struct blob_db_iostats io;

		blob_db_iostats_get(&io);
		out->flash_reads = io.reads;
		out->flash_writes = io.writes;
		out->flash_erases = io.erases;
		out->bytes_read = io.bytes_read;
		out->bytes_written = io.bytes_written;
		out->bytes_erased = io.bytes_erased;
		out->io_measured = true;
	}
#endif
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
#ifdef CONFIG_BLOB_DB_IOSTATS
	blob_db_iostats_reset();
#endif
}
