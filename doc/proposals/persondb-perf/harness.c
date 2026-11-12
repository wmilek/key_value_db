/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host harness for doc/proposals/2026-08-16-persondb-case-performance.md.
 *
 * THIS IS NOT PROJECT CODE AND IS NOT BUILT BY CMAKE. It exists so the tables
 * in that document can be re-derived rather than taken on trust — the same
 * reason doc/proposals/sizing/ exists.
 *
 * It replays app_cbor_persondb's *storage traffic* against the real
 * lib/blob_db + lib/containers/kvhash sources, over a RAM-backed NOR model with
 * the nRF5340-DK's geometry (8 MiB, 64 KB erase blocks, 4-byte write align).
 *
 * It does not encode CBOR — it writes byte blobs of exactly the length
 * person_cbor.c would produce (the same length model tools/sizing.py uses, and
 * that RESULTS.md's "mean entry 432 B" confirms). Flash traffic depends only on
 * key and value *lengths*, so this reproduces the counters that matter, and
 * §2 of the document checks that it does against both published runs.
 *
 * Swept by measure.sh: the per-map bucket count the application asks kvhash
 * for (persondb.c asks for SIZE_MAX today, i.e. the maximum), how the person
 * and credential maps are split into shards, and
 * CONFIG_BLOB_DB_MAX_PAYLOAD_LEN.
 *
 *   argv: n_persons samples people_maps people_buckets cred_maps cred_buckets
 *         [verbosity]        (-1 for a bucket count means SIZE_MAX)
 *   env:  HB_CENSUS=1        count slots per blob_db bucket after the fill
 *         HB_INSERT_FAST=1   skip person_put()'s diff-get where the caller
 *                            knows it is inserting (the fill loop does)
 *         HB_TSV=1           also emit one machine-readable row on stderr
 */
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/kvhash.h>

int hb_verbose = 0;
void hb_store_wipe(void);
const uint8_t *hb_store_ptr(void);
size_t hb_store_size(void);
size_t hb_store_peb(void);

/*
 * Bucket census: how many slots a blob_db bucket sector holds, and how many of
 * them are still live. blob_db locates a blob by reading one slot header per
 * slot until the end of the log (scan_bucket_for), so this count IS the
 * per-lookup transaction cost, and the dead fraction is what a compaction pass
 * would remove.
 */
#define HB_SLOT_HDR      4u
#define HB_SLOT_OVERHEAD 14u
#define HB_BUCKET_HDR    16u
#define HB_FIRST_BUCKET  3u

static void census(const char *label)
{
	const uint8_t *m = hb_store_ptr();
	size_t peb = hb_store_peb();
	size_t n_pebs = hb_store_size() / peb;
	uint64_t slots = 0, live = 0, used_buckets = 0;
	static uint64_t seen[200000];

	for (size_t b = HB_FIRST_BUCKET; b < n_pebs; b++) {
		const uint8_t *s = m + b * peb;
		size_t off = HB_BUCKET_HDR, n = 0;
		size_t first = 0;

		while (off + HB_SLOT_OVERHEAD <= peb) {
			uint8_t flags = s[off];
			uint16_t val_len;

			memcpy(&val_len, s + off + 2, 2);
			if (flags == 0xff || !(flags & 1u) || (flags & ~3u) ||
			    val_len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN) {
				break;
			}
			size_t ssz = (HB_SLOT_OVERHEAD + val_len + 3u) & ~3u;

			if (off + ssz > peb) {
				break;
			}
			memcpy(&seen[first + n], s + off + HB_SLOT_HDR, 8);
			n++;
			off += ssz;
		}
		if (n) {
			used_buckets++;
		}
		slots += n;
		/* live = distinct ids in this bucket (last slot wins) */
		for (size_t i = 0; i < n; i++) {
			bool later = false;

			for (size_t j = i + 1; j < n; j++) {
				if (seen[j] == seen[i]) {
					later = true;
					break;
				}
			}
			if (!later) {
				live++;
			}
		}
	}
	printf("  census %-10s %" PRIu64 " slots in %" PRIu64 " sectors "
	       "(%.1f/sector), %" PRIu64 " live (%.0f%% dead)\n",
	       label, slots, used_buckets,
	       used_buckets ? (double)slots / used_buckets : 0.0, live,
	       slots ? 100.0 * (double)(slots - live) / (double)slots : 0.0);
}

/* -- the dataset, ported verbatim from dataset.c / persondb.c ------------- */

#define DATASET_ID_BASE      100000u
#define PERM_VOCAB           32
#define CARDS_MAX            5
#define TEMP_CARD_SLOT       7
#define PIN_LEN              16

static const char *const perm_vocab[PERM_VOCAB] = {
	"door.hq.lobby",     "door.hq.east",      "door.hq.west",
	"door.hq.roof",      "door.hq.loading",   "lab.chemistry.wet",
	"lab.electronics",   "lab.cleanroom.a",   "lab.prototype.3d",
	"gate.perimeter.n",  "gate.perimeter.s",  "gate.yard.vehicle",
	"room.server.core",  "room.network.mdf",  "room.hvac.plant",
	"room.power.ups",    "room.archive.cold", "room.vault.secure",
	"room.medical.aid",  "room.canteen",      "lift.tower.north",
	"lift.freight.dock", "park.surface",      "park.basement.b2",
	"shift.day",         "shift.evening",     "shift.night",
	"shift.weekend",     "admin.enroll",      "admin.revoke",
	"admin.audit.read",  "admin.configure",
};
static const char *const first_names[16] = {
	"Ada", "Bo", "Cira", "Dov", "Eve", "Finn", "Gita", "Hal",
	"Iris", "Juno", "Kai", "Lena", "Milo", "Nils", "Oona", "Pax",
};
static const char *const last_names[16] = {
	"Aalto", "Bercu", "Cheng", "Duarte", "Eriksen", "Farhi",
	"Gruber", "Haddad", "Ivanov", "Jarvis", "Kovac", "Lindqvist",
	"Moreau", "Nagy", "Okafor", "Pandit",
};
static const char *const departments[8] = {
	"Engineering", "Operations", "Facilities", "Security",
	"Research", "Logistics", "Finance", "Support",
};
static const char *const titles[8] = {
	"Technician", "Engineer", "Supervisor", "Analyst",
	"Coordinator", "Specialist", "Manager", "Contractor",
};

static uint32_t mix(uint32_t x, uint32_t salt)
{
	x += salt * 0x9e3779b9u;
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
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

static uint8_t ds_n_cards(uint32_t i) { return (uint8_t)(1u + (mix(i, 3) % 4u)); }

static void ds_card(uint32_t index, uint8_t slot, char *out)
{
	uint64_t n = (uint64_t)index * (CARDS_MAX + 3u) + slot;
	uint64_t uid = (n * 0x9e3779b97f4a7dULL) & 0x00ffffffffffffULL;

	snprintf(out, 15, "%014llX", (unsigned long long)uid);
}

static uint32_t ds_mutate_index(uint32_t nth, uint32_t rev, uint32_t n_persons,
				uint32_t mutate_count)
{
	uint32_t stride = (mutate_count && n_persons / mutate_count)
				  ? n_persons / mutate_count : 1u;
	return (rev + nth * stride) % n_persons;
}

static uint32_t ds_sample_index(uint32_t nth, uint32_t count, uint32_t n_persons)
{
	return (uint32_t)(((uint64_t)nth * n_persons) / count) % n_persons;
}

static bool ds_has_temp_card(uint32_t index, uint32_t rev, uint32_t n_persons,
			     uint32_t mutate_count)
{
	if (rev == 0 || mutate_count == 0) {
		return false;
	}
	for (uint32_t i = 0; i < mutate_count; i++) {
		if (ds_mutate_index(i, rev, n_persons, mutate_count) == index) {
			return true;
		}
	}
	return false;
}

/* -- the CBOR *length* model (person_cbor.c, canonical) ------------------- */

static size_t head(uint64_t v)
{
	if (v < 24)      return 1;
	if (v < 0x100)   return 2;
	if (v < 0x10000) return 3;
	return 5;
}

/* Bytes person_cbor_encode() would emit for dataset person @index at @rev. */
static size_t person_len(uint32_t index, uint32_t rev, uint32_t n_persons,
			 uint32_t mutate_count, uint8_t *n_cards_out)
{
	char name[40];
	const char *dept  = departments[mix(index, 4) % 8];
	const char *title = titles[mix(index, 5) % 8];
	uint32_t id = DATASET_ID_BASE + index;
	uint32_t valid_from = 1767225600u + (mix(index, 6) % (2u * 365u * 86400u));
	uint8_t n_perms = (uint8_t)(10u + (mix(index, 8) % 13u));
	uint8_t start   = (uint8_t)(mix(index, 9) % PERM_VOCAB);
	uint8_t step    = (uint8_t)((mix(index, 10) % 16u) * 2u + 1u);
	uint8_t n_cards = ds_n_cards(index);
	size_t n;

	snprintf(name, sizeof(name), "%s %s-%04u",
		 first_names[mix(index, 1) % 16], last_names[mix(index, 2) % 16],
		 (unsigned)(index % 10000u));

	if (ds_has_temp_card(index, rev, n_persons, mutate_count)) {
		n_cards++;
	}
	if (n_cards_out) {
		*n_cards_out = n_cards;
	}

	n  = head(9);                                   /* map header, 9 pairs */
	n += 1 + head(id);
	n += 1 + head(strlen(name))  + strlen(name);
	n += 1 + head(strlen(dept))  + strlen(dept);
	n += 1 + head(strlen(title)) + strlen(title);
	n += 1 + head(valid_from);
	n += 1 + head((uint64_t)valid_from + 3u * 365u * 86400u);
	n += 1 + head(PIN_LEN) + PIN_LEN;
	n += 1 + head(n_perms);
	for (uint8_t i = 0; i < n_perms; i++) {
		const char *p = perm_vocab[(start + (uint16_t)i * step) % PERM_VOCAB];

		n += head(strlen(p)) + strlen(p);
	}
	n += 1 + head(n_cards) + (size_t)n_cards * (head(14) + 14);
	return n;
}

#define CRED_VAL_LEN 5u   /* cbor uint32 of an id >= 100000 */

/* -- the store, mirroring persondb.c ------------------------------------- */

#define MAX_MAPS 160

static struct {
	const struct map_ops *ops;
	uint8_t  n_people_maps;
	uint8_t  n_cred_maps;
	uint64_t people_root[MAX_MAPS];
	uint64_t cred_root[MAX_MAPS];
	uint64_t sb_id;
	uint32_t n_persons;
	uint64_t map_gets, map_sets, map_dels;
} db;

static uint64_t people_root(uint32_t id)
{
	return db.people_root[fnv1a(&id, sizeof(id)) % db.n_people_maps];
}

/*
 * Shard the credential map on a hash INDEPENDENT of the one kvhash will use on
 * the same string. kvhash buckets by fnv1a(key) % n_buckets; if the shard used
 * that same value, then whenever n_buckets is a multiple of n_maps the shard
 * becomes a function of the bucket index and only n_buckets of the
 * n_maps * n_buckets cells are ever reachable. Measured: -ENOSPC partway
 * through the fill, at 4x the intended per-bucket load.
 *
 * persondb.c does not have this problem for persons — it hashes the raw id
 * while kvhash hashes the formatted "pXXXXXXXX" key — but it would acquire it
 * the moment the credential map were sharded on the card id, where the two
 * hashes see the same bytes.
 */
static uint64_t cred_root(const char *card)
{
	if (db.n_cred_maps == 1) {
		return db.cred_root[0];
	}
	return db.cred_root[mix(fnv1a(card, strlen(card)), 11) % db.n_cred_maps];
}

static void person_key(char *buf, uint32_t id)
{
	snprintf(buf, 10, "p%08X", (unsigned)id);
}

static int map_get(uint64_t root, const char *key, void *out, size_t sz,
		   size_t *len)
{
	db.map_gets++;
	return db.ops->get(root, key, strlen(key), out, sz, len);
}

static int map_set(uint64_t root, const char *key, const void *v, size_t len)
{
	db.map_sets++;
	return db.ops->set(root, key, strlen(key), v, len);
}

static int map_del(uint64_t root, const char *key)
{
	db.map_dels++;
	return db.ops->del(root, key, strlen(key));
}

static uint8_t g_val[8192];

static int create_store(uint32_t n_persons, uint8_t n_maps, uint8_t n_cred_maps,
			size_t people_cap, size_t cred_cap)
{
	const struct map_config pcfg = { .initial_capacity = people_cap };
	const struct map_config ccfg = { .initial_capacity = cred_cap };

	db.ops = &kvhash_map_ops;
	db.n_people_maps = n_maps;
	db.n_cred_maps = n_cred_maps;
	db.n_persons = n_persons;

	for (uint8_t i = 0; i < n_maps; i++) {
		db.people_root[i] = blob_db_alloc_id();
		int rc = db.ops->create(db.people_root[i], &pcfg);

		if (rc != 0) {
			return rc;
		}
	}
	for (uint8_t i = 0; i < n_cred_maps; i++) {
		db.cred_root[i] = blob_db_alloc_id();
		int rc = db.ops->create(db.cred_root[i], &ccfg);

		if (rc != 0) {
			return rc;
		}
	}
	db.sb_id = blob_db_alloc_id();
	memset(g_val, 0xa5, 160);
	return blob_db_update(db.sb_id, g_val, 160);   /* superblock */
}

/* persondb_person_put(): read-old, write record, index every card. */
static int insert_fast;      /* HB_INSERT_FAST=1: skip the diff-get on inserts */
static int known_new;        /* set by the fill loop, which knows it inserts */

static int person_put(uint32_t index, uint32_t rev, uint32_t mutate_count)
{
	char key[10], card[16];
	uint32_t id = DATASET_ID_BASE + index;
	uint8_t n_cards = 0;
	size_t plen = person_len(index, rev, db.n_persons, mutate_count, &n_cards);
	size_t got = 0;
	int rc;

	person_key(key, id);

	/* the diff-get that makes a replace unindex the cards it drops */
	if (!(insert_fast && known_new)) {
		rc = map_get(people_root(id), key, g_val, sizeof(g_val), &got);
		if (rc != 0 && rc != -ENOENT) {
			return rc;
		}
	}

	memset(g_val, 0x5a, plen);
	rc = map_set(people_root(id), key, g_val, plen);
	if (rc != 0) {
		return rc;
	}
	for (uint8_t i = 0; i < n_cards; i++) {
		uint8_t slot = (i < ds_n_cards(index)) ? i : TEMP_CARD_SLOT;

		ds_card(index, slot, card);
		rc = map_set(cred_root(card), card, g_val, CRED_VAL_LEN);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

/* persondb_card_assign(): card_owner + person_get + person_store + cred_put. */
static int card_assign(uint32_t index, uint32_t rev, uint32_t mutate_count)
{
	char key[10], card[16];
	uint32_t id = DATASET_ID_BASE + index;
	uint8_t n_cards = 0;
	size_t plen, got = 0;
	int rc;

	ds_card(index, TEMP_CARD_SLOT, card);
	rc = map_get(cred_root(card), card, g_val, sizeof(g_val), &got);
	if (rc != 0 && rc != -ENOENT) {
		return rc;
	}
	person_key(key, id);
	rc = map_get(people_root(id), key, g_val, sizeof(g_val), &got);
	if (rc != 0) {
		return rc;
	}
	plen = person_len(index, rev, db.n_persons, mutate_count, &n_cards);
	memset(g_val, 0x5a, plen);
	rc = map_set(people_root(id), key, g_val, plen);
	if (rc != 0) {
		return rc;
	}
	return map_set(cred_root(card), card, g_val, CRED_VAL_LEN);
}

/* -- reporting ----------------------------------------------------------- */

struct phase {
	const char *name;
	uint64_t ops, map_ops;
	uint64_t reads, bytes_read, writes, bytes_written, erases, bytes_erased;
	double payload;
};

static void phase_begin(void)
{
	db.map_gets = db.map_sets = db.map_dels = 0;
	blob_db_iostats_reset();
}

static void phase_end(struct phase *p, uint64_t ops, double payload)
{
	struct blob_db_iostats io;

	blob_db_iostats_get(&io);
	p->ops = ops;
	p->map_ops = db.map_gets + db.map_sets + db.map_dels;
	p->reads = io.reads;
	p->bytes_read = io.bytes_read;
	p->writes = io.writes;
	p->bytes_written = io.bytes_written;
	p->erases = io.erases;
	p->bytes_erased = io.bytes_erased;
	p->payload = payload;
}

static uint64_t flash_ops(const struct phase *p)
{
	return p->reads + p->writes + p->erases;
}

static uint64_t flash_bytes(const struct phase *p)
{
	return p->bytes_read + p->bytes_written + p->bytes_erased;
}

/*
 * DK time model.
 *
 * Reads use app_perf's independently fitted constants (RESULTS.md §5):
 * ~65.5 us per transaction + ~0.63 us/B. Those are the ones RESULTS.md
 * validates against this app's own `check` to a fraction of a percent.
 *
 * Erases use the measured MX25R6435F 64 KB erase, ~1072 ms.
 *
 * Writes have no fitted constant published; HB_WR_US_PER_B is fitted here from
 * this app's own measured DK `put` (83.920 ms) at the baseline configuration,
 * and is the weakest term in the model. Read-only phases (check/byid/miss) do
 * not use it at all.
 */
#define HB_RD_US_PER_OP  65.5
#define HB_RD_US_PER_B   0.63
#define HB_WR_US_PER_OP  65.5
#define HB_WR_US_PER_B   3.0
#define HB_ERASE_US_PER_SECTOR 1072000.0

static double dk_us(const struct phase *p)
{
	return (double)p->reads * HB_RD_US_PER_OP +
	       (double)p->bytes_read * HB_RD_US_PER_B +
	       (double)p->writes * HB_WR_US_PER_OP +
	       (double)p->bytes_written * HB_WR_US_PER_B +
	       (double)p->bytes_erased / 65536.0 * HB_ERASE_US_PER_SECTOR;
}

static void row(const struct phase *p)
{
	double per = p->ops ? 1.0 / (double)p->ops : 0;

	printf("  %-6s %8" PRIu64 " %8.2f %10.1f %10.1f %8.1f %9.1f %6.2f %9.3f\n",
	       p->name, p->ops, (double)p->map_ops * per,
	       (double)p->reads * per, (double)p->bytes_read * per,
	       (double)p->writes * per, (double)p->bytes_written * per,
	       (double)p->erases * per, dk_us(p) * per / 1000.0);
}

/* -- one configuration --------------------------------------------------- */

static int run_config(uint32_t n_persons, uint8_t n_maps, uint8_t n_cred_maps,
		      size_t people_cap, size_t cred_cap, uint32_t samples,
		      uint32_t mutate_count, bool detail)
{
	struct phase fill = { .name = "fill" }, mut = { .name = "mutate" };
	struct phase chk = { .name = "check" }, byid = { .name = "byid" };
	struct phase miss = { .name = "miss" }, put = { .name = "put" };
	char key[10], card[16];
	double payload = 0;
	int rc;

	hb_store_wipe();
	memset(&db, 0, sizeof(db));

	rc = blob_db_mount();
	if (rc != 0) {
		fprintf(stderr, "mount: %d\n", rc);
		return rc;
	}
	rc = create_store(n_persons, n_maps, n_cred_maps, people_cap, cred_cap);
	if (rc != 0) {
		fprintf(stderr, "create: %d\n", rc);
		return rc;
	}
	blob_db_prepare((size_t)-1);

	/* fill */
	phase_begin();
	known_new = 1;
	for (uint32_t i = 0; i < n_persons; i++) {
		rc = person_put(i, 0, mutate_count);
		if (rc != 0) {
			fprintf(stderr, "fill: person %u: %d\n", i, rc);
			return rc;
		}
		if ((i + 1) % 256 == 0) {
			memset(g_val, 0xa5, 160);
			blob_db_update(db.sb_id, g_val, 160);
		}
	}
	known_new = 0;
	phase_end(&fill, n_persons, 0);
	if (getenv("HB_CENSUS")) {
		census("after fill");
	}

	/* mutate: rev 0 -> 1, assign each round's temporary card */
	phase_begin();
	for (uint32_t n = 0; n < mutate_count; n++) {
		uint32_t idx = ds_mutate_index(n, 1, n_persons, mutate_count);

		rc = card_assign(idx, 1, mutate_count);
		if (rc != 0) {
			fprintf(stderr, "mutate: %d\n", rc);
			return rc;
		}
	}
	phase_end(&mut, mutate_count, 0);

	if (samples > n_persons) {
		samples = n_persons;
	}

	/* check = card_owner + person_get */
	phase_begin();
	payload = 0;
	for (uint32_t n = 0; n < samples; n++) {
		uint32_t idx = ds_sample_index(n, samples, n_persons);
		uint32_t id = DATASET_ID_BASE + idx;
		size_t got = 0;
		uint8_t nc = 0;

		ds_card(idx, 0, card);
		rc = map_get(cred_root(card), card, g_val, sizeof(g_val), &got);
		if (rc != 0) {
			fprintf(stderr, "check: card %s: %d\n", card, rc);
			return rc;
		}
		person_key(key, id);
		rc = map_get(people_root(id), key, g_val, sizeof(g_val), &got);
		if (rc != 0) {
			fprintf(stderr, "check: person %u: %d\n", idx, rc);
			return rc;
		}
		payload += 4 + 9 + person_len(idx, 1, n_persons, mutate_count, &nc) +
			   4 + 14 + CRED_VAL_LEN;
	}
	phase_end(&chk, samples, payload);

	/* byid */
	phase_begin();
	payload = 0;
	for (uint32_t n = 0; n < samples; n++) {
		uint32_t idx = ds_sample_index(n, samples, n_persons);
		uint32_t id = DATASET_ID_BASE + idx;
		size_t got = 0;
		uint8_t nc = 0;

		person_key(key, id);
		rc = map_get(people_root(id), key, g_val, sizeof(g_val), &got);
		if (rc != 0) {
			return rc;
		}
		payload += 4 + 9 + person_len(idx, 1, n_persons, mutate_count, &nc);
	}
	phase_end(&byid, samples, payload);

	/* miss: slot 6 is never assigned */
	phase_begin();
	payload = 0;
	for (uint32_t n = 0; n < samples; n++) {
		uint32_t idx = ds_sample_index(n, samples, n_persons);
		size_t got = 0;

		ds_card(idx, 6, card);
		rc = map_get(cred_root(card), card, g_val, sizeof(g_val), &got);
		if (rc != -ENOENT) {
			fprintf(stderr, "miss: resolved (%d)\n", rc);
			return -1;
		}
		payload += 4 + 14 + CRED_VAL_LEN;
	}
	phase_end(&miss, samples, payload);

	/* put: rewrite the record the generator says should be there */
	phase_begin();
	payload = 0;
	for (uint32_t n = 0; n < samples; n++) {
		uint32_t idx = ds_sample_index(n, samples, n_persons);
		uint8_t nc = 0;

		rc = person_put(idx, 1, mutate_count);
		if (rc != 0) {
			fprintf(stderr, "put: %d\n", rc);
			return rc;
		}
		payload += 4 + 9 + person_len(idx, 1, n_persons, mutate_count, &nc) +
			   nc * (4 + 14 + CRED_VAL_LEN);
	}
	phase_end(&put, samples, payload);

	printf("N=%u maps=%u/%u buckets=%zu/%zu\n", n_persons, n_maps,
	       n_cred_maps, people_cap, cred_cap);
	if (detail) {
		printf("  %-6s %8s %8s %10s %10s %8s %9s %6s %9s\n", "phase",
		       "ops", "map ops", "rd ops", "rd B", "wr ops", "wr B",
		       "erase", "DK ms/op");
		row(&chk); row(&byid); row(&miss); row(&put);
		printf("  fill  : %.0f s modelled DK, %" PRIu64 " erases, "
		       "%" PRIu64 " flash ops, %" PRIu64 " B\n",
		       dk_us(&fill) / 1e6, fill.erases, flash_ops(&fill),
		       flash_bytes(&fill));
		printf("  mutate: %.1f ms/assign modelled DK\n",
		       dk_us(&mut) / 1000.0 / (mutate_count ? mutate_count : 1));
	} else {
		printf("  check %.3f ms  byid %.3f ms  miss %.3f ms  put %.3f ms"
		       "  fill %.0f s\n",
		       dk_us(&chk) / 1000.0 / samples, dk_us(&byid) / 1000.0 / samples,
		       dk_us(&miss) / 1000.0 / samples, dk_us(&put) / 1000.0 / samples,
		       dk_us(&fill) / 1e6);
	}
	if (getenv("HB_TSV")) {
		fprintf(stderr,
			"%u\t%u\t%zu\t%u\t%zu\t%.2f\t%.1f\t%.3f\t%.2f\t%.1f\t%.3f"
			"\t%.2f\t%.1f\t%.3f\t%.2f\t%.1f\t%.3f\t%.0f\t%" PRIu64 "\n",
			n_persons, n_maps, people_cap, n_cred_maps, cred_cap,
			(double)chk.reads / samples, (double)chk.bytes_read / samples,
			dk_us(&chk) / 1000.0 / samples,
			(double)byid.reads / samples, (double)byid.bytes_read / samples,
			dk_us(&byid) / 1000.0 / samples,
			(double)miss.reads / samples, (double)miss.bytes_read / samples,
			dk_us(&miss) / 1000.0 / samples,
			(double)put.reads / samples, (double)put.bytes_read / samples,
			dk_us(&put) / 1000.0 / samples,
			dk_us(&fill) / 1e6, flash_bytes(&fill));
	}
	fflush(stdout);
	blob_db_unmount();
	return 0;
}

int main(int argc, char **argv)
{
	uint32_t n_persons = (argc > 1) ? (uint32_t)atoi(argv[1]) : 1000;
	uint32_t samples   = (argc > 2) ? (uint32_t)atoi(argv[2]) : 200;
	uint8_t  n_maps    = (argc > 3) ? (uint8_t)atoi(argv[3]) : 16;
	size_t   pcap      = (argc > 4) ? (size_t)atol(argv[4]) : (size_t)-1;
	uint8_t  n_cmaps   = (argc > 5) ? (uint8_t)atoi(argv[5]) : 1;
	size_t   ccap      = (argc > 6) ? (size_t)atol(argv[6]) : (size_t)-1;

	if (argc > 7) {
		hb_verbose = atoi(argv[7]);
	}
	insert_fast = getenv("HB_INSERT_FAST") != NULL;
	return run_config(n_persons, n_maps, n_cmaps, pcap, ccap, samples, 64,
			  true);
}
