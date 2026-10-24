/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite for app_cbor_persondb's superblock: what persondb_open() does
 * with a store written by an older or a newer build (DESIGN.md §12.3), and
 * what persondb_destroy() removes — this store, nothing else (§12.4).
 *
 * Each test starts from a fresh store, rewrites its superblock the way another
 * build would have left it, and reopens. The rule under test is that a store
 * is either opened with every table handled by its class, or refused with the
 * store left exactly as it was found.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>
#include <zcbor_encode.h>

#include <app/lib/blob_db.h>
#include <app/lib/containers/kvhash.h>
#include <app/lib/rootreg.h>

#include "person_cbor.h"
#include "persondb.h"

#define N_PERSONS 16
#define PADB_KEY  ROOTREG_KEY(0x50414442u, 0)   /* 'PADB', as persondb.c */

static struct persondb *db;

/* -- helpers ------------------------------------------------------------ */

static uint64_t sb_id(void)
{
	uint64_t id;

	zassert_ok(rootreg_get(PADB_KEY, &id));
	return id;
}

static size_t sb_raw(uint8_t *buf, size_t sz)
{
	size_t got = 0;

	zassert_ok(blob_db_get(sb_id(), buf, sz, &got));
	return got;
}

static void sb_read(struct superblock *sb)
{
	uint8_t buf[SUPERBLOCK_CBOR_MAX];
	size_t len = sb_raw(buf, sizeof(buf));

	zassert_ok(superblock_cbor_decode(buf, len, sb));
}

static void sb_write(const struct superblock *sb)
{
	uint8_t buf[SUPERBLOCK_CBOR_MAX];
	size_t len;

	zassert_ok(superblock_cbor_encode(sb, buf, sizeof(buf), &len));
	zassert_ok(blob_db_update(sb_id(), buf, len));
}

/* Append a table from "a newer build" to the stored directory. */
static void sb_add_table(uint32_t id, uint64_t root, uint32_t cls,
			 uint32_t schema)
{
	struct superblock sb;

	sb_read(&sb);
	zassert_true(sb.n_tables < SB_TABLES_MAX);
	sb.table[sb.n_tables++] = (struct sb_table){
		.id = id, .root = root, .cls = cls, .schema = schema,
	};
	sb_write(&sb);
}

/* A version 1 superblock, as the build before the table directory wrote it. */
static void sb_write_v1(const uint64_t *people_roots, size_t n,
			uint64_t cred_root, uint32_t populated)
{
	uint8_t buf[SUPERBLOCK_CBOR_MAX];

	ZCBOR_STATE_E(zs, 2, buf, sizeof(buf), 1);

	bool ok = zcbor_map_start_encode(zs, 6) &&
		  zcbor_uint32_put(zs, 1) && zcbor_uint32_put(zs, 1) &&
		  zcbor_uint32_put(zs, 2) && zcbor_list_start_encode(zs, 16);

	for (size_t i = 0; ok && i < n; i++) {
		ok = zcbor_uint64_put(zs, people_roots[i]);
	}
	ok = ok && zcbor_list_end_encode(zs, 16) &&
	     zcbor_uint32_put(zs, 3) && zcbor_uint64_put(zs, cred_root) &&
	     zcbor_uint32_put(zs, 4) && zcbor_uint32_put(zs, N_PERSONS) &&
	     zcbor_uint32_put(zs, 5) && zcbor_uint32_put(zs, populated) &&
	     zcbor_uint32_put(zs, 6) && zcbor_uint32_put(zs, 0) &&
	     zcbor_map_end_encode(zs, 6);
	zassert_true(ok);
	zassert_ok(blob_db_update(sb_id(), buf, zs->payload - buf));
}

static uint64_t table_root(uint32_t id)
{
	struct superblock sb;

	sb_read(&sb);
	for (uint8_t i = 0; i < sb.n_tables; i++) {
		if (sb.table[i].id == id) {
			return sb.table[i].root;
		}
	}
	ztest_test_fail();
	return 0;
}

/* A real map with one key in it, standing in for a newer build's table. */
static uint64_t make_map(void)
{
	const struct map_config cfg = {
		.expected_entries = 4, .typical_entry_bytes = 16,
		.max_entry_bytes = 16,
	};
	uint64_t root = blob_db_alloc_id();

	zassert_not_equal(root, 0);
	zassert_ok(kvhash_map_ops.create(root, &cfg));
	zassert_ok(kvhash_map_ops.set(root, "k", 1, "v", 1));
	return root;
}

static bool map_alive(uint64_t root)
{
	return kvhash_map_ops.get(root, "k", 1, NULL, 0, NULL) == -ENOMEM;
}

static void put_person(uint32_t id)
{
	struct persondb_person p = {
		.id = id, .name = "Test Person", .dept = "QA", .title = "Probe",
		.valid_from = 0, .valid_until = UINT32_MAX,
		.n_perms = 1, .perm = { "door.test" },
		.n_cards = 1, .card = { "0000000000000A" },
	};

	zassert_ok(persondb_person_put(db, &p));
}

static int reopen(void)
{
	persondb_close(db);
	db = NULL;
	return persondb_open(&db, N_PERSONS);
}

/* Reopen, expecting a refusal that leaves the superblock byte-identical. */
static void expect_refused(int want)
{
	uint8_t before[SUPERBLOCK_CBOR_MAX], after[SUPERBLOCK_CBOR_MAX];
	size_t n_before = sb_raw(before, sizeof(before));

	zassert_equal(reopen(), want);

	/* A refused open leaves blob_db mounted, so the store can be read. */
	size_t n_after = sb_raw(after, sizeof(after));

	zassert_equal(n_after, n_before);
	zassert_mem_equal(after, before, n_before, "refusal wrote the superblock");
}

/* Wipe the whole partition without opening first: the previous test may have
 * left a store that persondb_open() refuses, or one persondb_destroy() cannot
 * remove. The test owns the partition, so this is the fixture's call to make,
 * not persondb's. */
static void before(void *f)
{
	ARG_UNUSED(f);
	int rc = blob_db_mount();

	zassert_true(rc == 0 || rc == -EALREADY, "mount: %d", rc);
	zassert_ok(blob_db_erase_all());
	zassert_ok(rootreg_init());
	zassert_ok(persondb_open(&db, N_PERSONS));
}

static void after(void *f)
{
	ARG_UNUSED(f);
	persondb_close(db);
	db = NULL;
}

ZTEST_SUITE(persondb_superblock, NULL, NULL, before, after, NULL);

/* -- a store this build writes ------------------------------------------ */

ZTEST(persondb_superblock, test_fresh_store_lists_two_required_tables)
{
	struct superblock sb;

	sb_read(&sb);
	zassert_equal(sb.version, SUPERBLOCK_VERSION);
	zassert_equal(sb.n_tables, 2);
	zassert_equal(sb.table[0].id, SB_TABLE_PEOPLE);
	zassert_equal(sb.table[0].cls, SB_CLASS_REQUIRED);
	zassert_equal(sb.table[0].schema, SB_SCHEMA_PEOPLE);
	zassert_equal(sb.table[1].id, SB_TABLE_CRED);
	zassert_equal(sb.table[1].cls, SB_CLASS_REQUIRED);
	zassert_equal(sb.table[1].schema, SB_SCHEMA_CRED);
	zassert_equal(sb.n_persons, N_PERSONS);

	zassert_ok(reopen());
}

/* -- a store an older build wrote --------------------------------------- */

ZTEST(persondb_superblock, test_v1_store_is_upgraded_in_place)
{
	struct persondb_person got;
	struct superblock sb;
	uint32_t owner;
	uint64_t people = table_root(SB_TABLE_PEOPLE);

	put_person(100001);
	zassert_ok(persondb_progress_set(db, 1, 0));
	sb_write_v1(&people, 1, table_root(SB_TABLE_CRED), 1);

	zassert_ok(reopen());

	sb_read(&sb);
	zassert_equal(sb.version, SUPERBLOCK_VERSION, "not rewritten as v2");
	zassert_equal(sb.n_tables, 2);
	zassert_equal(sb.table[0].root, people);
	zassert_equal(sb.populated, 1);

	/* No record moved: the same maps answer as before. */
	zassert_ok(persondb_person_get(db, 100001, &got));
	zassert_ok(persondb_card_owner(db, "0000000000000A", &owner));
	zassert_equal(owner, 100001);
}

ZTEST(persondb_superblock, test_v1_store_of_several_people_maps_is_refused)
{
	uint64_t roots[2] = { table_root(SB_TABLE_PEOPLE), make_map() };

	sb_write_v1(roots, 2, table_root(SB_TABLE_CRED), 0);
	expect_refused(-ENOTSUP);
}

/* -- a store a newer build wrote ---------------------------------------- */

ZTEST(persondb_superblock, test_newer_superblock_version_is_refused)
{
	/* { 1: 3 } — whatever a version 3 holds, this build cannot know. */
	static const uint8_t v3[] = { 0xa1, 0x01, 0x03 };

	zassert_ok(blob_db_update(sb_id(), v3, sizeof(v3)));
	expect_refused(-ENOTSUP);
}

ZTEST(persondb_superblock, test_unknown_required_table_is_refused)
{
	sb_add_table(9, make_map(), SB_CLASS_REQUIRED, 1);
	expect_refused(-ENOTSUP);
}

ZTEST(persondb_superblock, test_unknown_class_is_treated_as_required)
{
	sb_add_table(9, make_map(), 7, 1);
	expect_refused(-ENOTSUP);
}

ZTEST(persondb_superblock, test_unknown_preserve_table_is_kept)
{
	struct superblock sb;
	uint64_t root = make_map();

	sb_add_table(9, root, SB_CLASS_PRESERVE, 4);
	zassert_ok(reopen());

	/* Survives this build's own superblock rewrites, untouched. */
	put_person(100001);
	zassert_ok(persondb_progress_set(db, 1, 0));
	sb_read(&sb);
	zassert_equal(sb.n_tables, 3);
	zassert_equal(sb.table[2].id, 9);
	zassert_equal(sb.table[2].root, root);
	zassert_equal(sb.table[2].cls, SB_CLASS_PRESERVE);
	zassert_equal(sb.table[2].schema, 4);
	zassert_true(map_alive(root));
}

ZTEST(persondb_superblock, test_unknown_derived_table_is_dropped)
{
	struct persondb_person got;
	struct superblock sb;
	uint64_t root = make_map();

	put_person(100001);
	sb_add_table(9, root, SB_CLASS_DERIVED, 1);
	zassert_ok(reopen());

	sb_read(&sb);
	zassert_equal(sb.n_tables, 2, "derived table still listed");
	zassert_equal(kvhash_map_ops.get(root, "k", 1, NULL, 0, NULL), -ENOENT,
		      "derived table not destroyed");
	zassert_ok(persondb_person_get(db, 100001, &got));
}

/* Power lost after the destroy, before the directory was rewritten. */
ZTEST(persondb_superblock, test_drop_resumes_after_a_crash)
{
	struct superblock sb;
	uint64_t root = make_map();

	sb_add_table(9, root, SB_CLASS_DERIVED, 1);
	zassert_ok(kvhash_map_ops.destroy(root));
	zassert_ok(reopen());

	sb_read(&sb);
	zassert_equal(sb.n_tables, 2);
}

/* A refusal is decided before anything is dropped. */
ZTEST(persondb_superblock, test_refusal_drops_nothing)
{
	uint64_t derived = make_map();

	sb_add_table(9, derived, SB_CLASS_DERIVED, 1);
	sb_add_table(10, make_map(), SB_CLASS_REQUIRED, 1);
	expect_refused(-ENOTSUP);
	zassert_true(map_alive(derived), "destroyed a table before refusing");
}

/* -- this build's own tables -------------------------------------------- */

/* A corrupt directory naming the people map as a newer build's index. */
ZTEST(persondb_superblock, test_derived_entry_cannot_drop_an_owned_map)
{
	struct persondb_person got;

	put_person(100001);
	sb_add_table(9, table_root(SB_TABLE_PEOPLE), SB_CLASS_DERIVED, 1);
	expect_refused(-EIO);

	/* Still there for a build that can open the store. */
	struct superblock sb;

	sb_read(&sb);
	sb.n_tables = 2;
	sb_write(&sb);
	zassert_ok(reopen());
	zassert_ok(persondb_person_get(db, 100001, &got));
}

ZTEST(persondb_superblock, test_unreadable_record_format_is_refused)
{
	struct superblock sb;

	sb_read(&sb);
	sb.table[0].schema = SB_SCHEMA_PEOPLE + 1;
	sb_write(&sb);
	expect_refused(-ENOTSUP);
}

ZTEST(persondb_superblock, test_missing_table_is_corruption)
{
	struct superblock sb;

	sb_read(&sb);
	sb.n_tables = 1;   /* people only */
	sb_write(&sb);
	expect_refused(-EIO);
}

/* -- destroying a store ------------------------------------------------- */

/* Another structure sharing the partition: its own key, its own map. */
#define OTHER_KEY ROOTREG_KEY(0x4f544852u, 0)   /* 'OTHR' */

/* After a destroy only the registry, at id 1, is left of the store. */
#define BLOBS_WITHOUT_A_STORE 1u

ZTEST(persondb_superblock, test_destroy_releases_every_blob)
{
	struct persondb_person got;
	uint64_t id;

	put_person(100001);
	put_person(100002);
	zassert_ok(persondb_destroy(&db));
	zassert_is_null(db);

	zassert_equal(rootreg_get(PADB_KEY, &id), -ENOENT);
	zassert_equal(blob_db_count(), BLOBS_WITHOUT_A_STORE,
		      "destroy leaked %u blobs",
		      (unsigned)(blob_db_count() - BLOBS_WITHOUT_A_STORE));

	/* And a fresh store builds in its place. */
	zassert_ok(persondb_open(&db, N_PERSONS));
	zassert_equal(persondb_person_get(db, 100001, &got), -ENOENT);
}

ZTEST(persondb_superblock, test_destroy_leaves_other_structures_alone)
{
	size_t before_other = blob_db_count();
	uint64_t other = make_map();
	size_t other_blobs = blob_db_count() - before_other;
	uint64_t id;

	zassert_ok(rootreg_set(OTHER_KEY, other));
	put_person(100001);
	zassert_ok(persondb_destroy(&db));

	zassert_ok(rootreg_get(OTHER_KEY, &id));
	zassert_equal(id, other);
	zassert_true(map_alive(other), "destroy reached another structure");
	zassert_equal(blob_db_count(), BLOBS_WITHOUT_A_STORE + other_blobs);

	/* The id counter was not reset: the fresh store's ids are new. */
	zassert_ok(persondb_open(&db, N_PERSONS));
	zassert_true(sb_id() > other);
	zassert_true(map_alive(other));
}

ZTEST(persondb_superblock, test_destroy_takes_a_newer_builds_tables)
{
	uint64_t kept = make_map();
	uint64_t derived = make_map();

	sb_add_table(9, kept, SB_CLASS_PRESERVE, 1);
	sb_add_table(10, derived, SB_CLASS_DERIVED, 1);
	zassert_ok(persondb_destroy(&db));

	zassert_false(map_alive(kept));
	zassert_false(map_alive(derived));
	zassert_equal(blob_db_count(), BLOBS_WITHOUT_A_STORE);
}

/* The store this build refuses to open is the one reset exists for. */
ZTEST(persondb_superblock, test_destroy_removes_a_store_open_refuses)
{
	sb_add_table(9, make_map(), SB_CLASS_REQUIRED, 1);
	zassert_equal(reopen(), -ENOTSUP);

	zassert_ok(persondb_destroy(&db));
	zassert_equal(blob_db_count(), BLOBS_WITHOUT_A_STORE);
}

/* Without a readable superblock it cannot know what it owns. */
ZTEST(persondb_superblock, test_destroy_refuses_a_newer_superblock_version)
{
	static const uint8_t v3[] = { 0xa1, 0x01, 0x03 };
	uint8_t after[SUPERBLOCK_CBOR_MAX];
	size_t count;

	zassert_ok(blob_db_update(sb_id(), v3, sizeof(v3)));
	count = blob_db_count();

	zassert_equal(persondb_destroy(&db), -ENOTSUP);
	zassert_equal(sb_raw(after, sizeof(after)), sizeof(v3));
	zassert_mem_equal(after, v3, sizeof(v3));
	zassert_equal(blob_db_count(), count, "refusal released blobs");
}

/* A raw blob where a table should be: a container this build cannot walk. */
ZTEST(persondb_superblock, test_destroy_refuses_before_releasing_anything)
{
	struct persondb_person got;
	struct superblock sb;
	uint64_t raw = blob_db_alloc_id();

	zassert_not_equal(raw, 0);
	zassert_ok(blob_db_update(raw, "not a map", 9));
	put_person(100001);
	sb_add_table(9, raw, SB_CLASS_PRESERVE, 1);

	zassert_equal(persondb_destroy(&db), -EIO);
	zassert_true(blob_db_exists(raw));

	/* The store is whole: drop the foreign entry and it opens as before. */
	sb_read(&sb);
	sb.n_tables = 2;
	sb_write(&sb);
	zassert_ok(persondb_open(&db, N_PERSONS));
	zassert_ok(persondb_person_get(db, 100001, &got));
}

/* Power lost after the people map was released. */
ZTEST(persondb_superblock, test_destroy_resumes_after_a_crash)
{
	put_person(100001);
	zassert_ok(kvhash_map_ops.destroy(table_root(SB_TABLE_PEOPLE)));

	zassert_equal(reopen(), -EINPROGRESS);
	zassert_ok(persondb_destroy(&db));
	zassert_equal(blob_db_count(), BLOBS_WITHOUT_A_STORE);
	zassert_ok(persondb_open(&db, N_PERSONS));
}

/* Power lost after the superblock was deleted, before the unregister. */
ZTEST(persondb_superblock, test_open_after_a_destroy_lost_its_last_step)
{
	uint64_t old = sb_id();

	zassert_ok(kvhash_map_ops.destroy(table_root(SB_TABLE_PEOPLE)));
	zassert_ok(kvhash_map_ops.destroy(table_root(SB_TABLE_CRED)));
	zassert_ok(blob_db_delete(old));

	/* Building at the deleted id would be D3's undefined update. */
	zassert_ok(reopen());
	zassert_not_equal(sb_id(), old);
	put_person(100001);

	zassert_ok(persondb_destroy(&db));
	zassert_equal(blob_db_count(), BLOBS_WITHOUT_A_STORE);
}

ZTEST(persondb_superblock, test_destroy_without_a_store)
{
	zassert_ok(persondb_destroy(&db));
	zassert_ok(persondb_destroy(&db));
	zassert_equal(blob_db_count(), BLOBS_WITHOUT_A_STORE);
}

/* -- the codec ---------------------------------------------------------- */

ZTEST(persondb_superblock, test_directory_must_ascend)
{
	struct superblock sb = { .n_tables = 2 };
	uint8_t buf[SUPERBLOCK_CBOR_MAX];
	size_t len;

	sb.table[0].id = 2;
	sb.table[1].id = 2;
	zassert_equal(superblock_cbor_encode(&sb, buf, sizeof(buf), &len),
		      -EINVAL);
}

ZTEST(persondb_superblock, test_full_directory_fits_the_buffer)
{
	struct superblock sb = { .n_tables = SB_TABLES_MAX,
				 .n_persons = UINT32_MAX,
				 .populated = UINT32_MAX, .rev = UINT32_MAX };
	struct superblock back;
	uint8_t buf[SUPERBLOCK_CBOR_MAX];
	size_t len;

	for (uint8_t i = 0; i < SB_TABLES_MAX; i++) {
		sb.table[i] = (struct sb_table){
			.id = UINT32_MAX - SB_TABLES_MAX + i, .root = UINT64_MAX,
			.cls = UINT32_MAX, .schema = UINT32_MAX,
		};
	}
	zassert_ok(superblock_cbor_encode(&sb, buf, sizeof(buf), &len));
	zassert_ok(superblock_cbor_decode(buf, len, &back));
	zassert_equal(back.n_tables, SB_TABLES_MAX);
	zassert_equal(back.table[SB_TABLES_MAX - 1].root, UINT64_MAX);
}
