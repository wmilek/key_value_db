/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * persondb — the person management API, internal to this application.
 *
 * Domain vocabulary only: persons, credentials, permissions, decisions. No
 * keys, no shards, no blob ids, no CBOR. Everything above this header is
 * written against these operations, which is what lets the storage layout in
 * persondb.c be replaced without touching the scenario layer or either
 * frontend (DESIGN.md F12; checked as acceptance criterion A7).
 *
 * Concurrency: single-threaded, inherited from blob_db's v1 contract. The
 * caller serializes every call — including across different structures, since
 * kvhash's scratch buffers are shared by every map in the image
 * (FINDINGS.md K7).
 */

#ifndef APP_CBOR_PERSONDB_PERSONDB_H_
#define APP_CBOR_PERSONDB_PERSONDB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Field capacities. Fixed, so a person is a plain value type that lives on
 * the stack — no heap anywhere in this application (P2/P3). */
#define PERSONDB_NAME_MAX   31
#define PERSONDB_DEPT_MAX   23
#define PERSONDB_TITLE_MAX  23
#define PERSONDB_PERM_MAX   23
#define PERSONDB_PERMS_MAX  22
#define PERSONDB_CARD_LEN   14   /* 7-byte UID as hex, like a real NFC card */
#define PERSONDB_CARDS_MAX   5   /* 4 permanent + 1 temporary (mutation round) */
#define PERSONDB_PIN_LEN    16

/** A person. Permissions and cards are text, per R-C2 / R-C3. */
struct persondb_person {
	uint32_t id;
	char     name [PERSONDB_NAME_MAX  + 1];
	char     dept [PERSONDB_DEPT_MAX  + 1];
	char     title[PERSONDB_TITLE_MAX + 1];
	uint32_t valid_from;                    /* epoch seconds, inclusive */
	uint32_t valid_until;                   /* epoch seconds, exclusive */
	uint8_t  pin_hash[PERSONDB_PIN_LEN];
	uint8_t  n_perms;
	char     perm[PERSONDB_PERMS_MAX][PERSONDB_PERM_MAX + 1];
	uint8_t  n_cards;
	char     card[PERSONDB_CARDS_MAX][PERSONDB_CARD_LEN + 1];
};

/** The outcome of an access request. */
enum persondb_decision {
	PERSONDB_GRANTED = 0,
	PERSONDB_DENIED,        /* card known, permission not held */
	PERSONDB_UNKNOWN_CARD,  /* no such credential */
	PERSONDB_EXPIRED,       /* card known, outside the validity window */
};

/**
 * @brief What the store looks like from outside.
 *
 * Most of this exists because no layer below reports it (FINDINGS.md B3, K10):
 * the partition size is read from devicetree, and the op counters are kept by
 * this module so amplification can be computed at all.
 */
struct persondb_stat {
	uint32_t n_persons;          /* dataset size recorded at create time */
	uint32_t populated;          /* persons written so far */
	uint32_t rev;                /* mutation revision */
	uint8_t  n_people_maps;
	uint16_t buckets_per_map;    /* what kvhash actually gave us (K9) */

	size_t   partition_bytes;    /* from devicetree — see B3 */
	size_t   sector_bytes;       /* erase-block size, ditto */

	uint32_t enospc_hits;        /* F11: bucket overflows, never expected */

	/* Flash traffic, counted here because nothing else counts it. Every
	 * map operation is at least two blob_db operations (K11), and every
	 * blob_db operation moves a whole sector (B1). */
	uint64_t map_gets;
	uint64_t map_sets;
	uint64_t map_dels;
	uint64_t blob_ops;
};

/** Opaque handle; the instance is owned by persondb.c (one store per image). */
struct persondb;

/* -- lifecycle ---------------------------------------------------------- */

/**
 * @brief Attach to the store, building it if this is a virgin device.
 *
 * Requires blob_db_mount() and rootreg_init() to have run. Verifies that the
 * configured payload size is actually usable on this flash geometry before
 * touching anything (FINDINGS.md B9/B10).
 *
 * @param db          (out) handle
 * @param n_persons   dataset size to record when creating; ignored when
 *                    attaching to a store that already exists
 *
 * @retval 0        ready
 * @retval -ENOTSUP the configured payload does not fit this geometry
 * @retval -EIO     flash error or a corrupt superblock
 */
int persondb_open(struct persondb **db, uint32_t n_persons);

/** Detach. The store is untouched. */
int persondb_close(struct persondb *db);

/* -- enrollment --------------------------------------------------------- */

/**
 * @brief Insert or replace a person record, and index every card it lists.
 *
 * @retval 0       stored
 * @retval -ENOSPC a kvhash bucket overflowed (K2) — counted in the stat block
 */
int persondb_person_put(struct persondb *db, const struct persondb_person *p);

/**
 * @retval 0       found
 * @retval -ENOENT no such person
 */
int persondb_person_get(struct persondb *db, uint32_t id,
			struct persondb_person *out);

/** Remove a person and every credential pointing at it. */
int persondb_person_delete(struct persondb *db, uint32_t id);

/* -- credentials -------------------------------------------------------- */

/**
 * @brief Assign a card to a person.
 *
 * Writes the person record first and the index second. There is no multi-key
 * transaction (C8), so a crash between the two must be survivable in one
 * direction only: this order leaves a card the person lists but that resolves
 * to nothing, which *denies*. The reverse order would leave a credential
 * granting access on behalf of a person who does not list it.
 */
int persondb_card_assign(struct persondb *db, uint32_t person_id,
			 const char *card);

/**
 * @brief Revoke a card.
 *
 * Deletes the index entry first and rewrites the person second — the mirror
 * of assignment, and fail-safe for the same reason: after the first write the
 * card already resolves to nothing.
 */
int persondb_card_revoke(struct persondb *db, const char *card);

/**
 * @retval 0       resolved; *person_id set
 * @retval -ENOENT unknown credential
 */
int persondb_card_owner(struct persondb *db, const char *card,
			uint32_t *person_id);

/* -- permissions -------------------------------------------------------- */

/** Add @p perm to a person, if not already held. */
int persondb_permission_grant(struct persondb *db, uint32_t person_id,
			      const char *perm);

/** Remove @p perm from a person. */
int persondb_permission_revoke(struct persondb *db, uint32_t person_id,
			       const char *perm);

/* -- the decision (R-D) ------------------------------------------------- */

/**
 * @brief Resolve a credential and decide whether it grants @p perm at @p now.
 *
 * Implemented the obvious way: resolve the card to a person id, load the
 * person, check the validity window, compare permission strings. It does not
 * shortcut the decode, and the credential index holds nothing but a person id
 * — no cached permission bitmask (DESIGN.md D2). The cost of that honesty is
 * measured rather than avoided: two map gets, which by B1/K11 is four
 * whole-sector reads.
 *
 * @param who (optional) receives the person on any decision but UNKNOWN_CARD
 *
 * @retval 0     a decision was reached; *out holds it
 * @retval -EIO  the store could not be read — never treated as a grant
 */
int persondb_check(struct persondb *db, const char *card, const char *perm,
		   uint32_t now, enum persondb_decision *out,
		   struct persondb_person *who);

/** Human-readable form of a decision, for logs and the shell. */
const char *persondb_decision_str(enum persondb_decision d);

/* -- app-owned persistent state ----------------------------------------- */

/**
 * @brief Read the fill progress and mutation revision.
 *
 * Kept in the app's own superblock rather than a separate store, so
 * committing progress is one atomic blob_db_update (DESIGN.md §12).
 */
int persondb_progress_get(struct persondb *db, uint32_t *populated,
			  uint32_t *rev);

/** Commit fill progress and revision. Atomic. */
int persondb_progress_set(struct persondb *db, uint32_t populated,
			  uint32_t rev);

/* -- introspection ------------------------------------------------------ */

int persondb_stat(struct persondb *db, struct persondb_stat *out);

/** Zero the traffic counters, so a phase can be measured in isolation. */
void persondb_counters_reset(struct persondb *db);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_PERSONDB_H_ */
