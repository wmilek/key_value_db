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
	/* The bucket count is deliberately absent: the app asks for the largest
	 * map kvhash can build and there is no way to read back what it got
	 * (FINDINGS.md K9(c), K10). Reporting a number the firmware cannot
	 * observe would be inventing one — tools/sizing.py models it instead. */

	size_t   partition_bytes;    /* read from devicetree — see B3, X1 row 7.
				      * Wrong on the UBI backend, undetectably. */

	uint32_t enospc_hits;        /* F11: bucket overflows, never expected */

	/* Map-level work this module performed. */
	uint64_t map_gets;
	uint64_t map_sets;
	uint64_t map_dels;

	/* Flash actually touched underneath, read from blob_db's own counters
	 * when CONFIG_BLOB_DB_IOSTATS is on. Before those existed this had to
	 * be modelled as "map operations x sector size", which stopped being
	 * true the moment blob_db learned to walk buckets by slot header — a
	 * good reason to measure rather than model (FINDINGS.md B3). Zero when
	 * the counters are not compiled in. */
	uint64_t flash_reads;
	uint64_t flash_writes;
	uint64_t flash_erases;
	uint64_t bytes_read;
	uint64_t bytes_written;
	uint64_t bytes_erased;
	bool     io_measured;
};

/** Opaque handle; the instance is owned by persondb.c (one store per image). */
struct persondb;

/* -- lifecycle ---------------------------------------------------------- */

/**
 * @brief Bring the store up: mount, bootstrap the registry, attach — building
 *        it if this is a virgin device.
 *
 * This module owns the whole storage stack, so nothing above it ever calls
 * blob_db or rootreg. Verifies that the configured payload size is actually
 * usable on this flash geometry before touching anything
 * (FINDINGS.md B9/B10).
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

/** Detach and unmount. The store's contents are untouched. */
int persondb_close(struct persondb *db);

/** Drop every record and rebuild an empty store. Reattaches @p db. */
int persondb_erase(struct persondb **db, uint32_t n_persons);

/**
 * @brief Pre-format flash erase blocks so first-touch erases leave the hot
 *        path (~1.1 s each on the DK — FINDINGS.md B7).
 */
int persondb_prepare(int *buckets_formatted);

/* -- enrollment --------------------------------------------------------- */

/**
 * @brief Insert or replace a person record, and index every card it lists.
 *
 * Replace means replace: a card the stored version listed and @p p does not
 * loses its index entry, because leaving it would let it keep resolving to a
 * person who no longer lists it — a grant on a withdrawn credential.
 *
 * Costs one extra map get, paid on inserts too. Idempotent, and repairable:
 * redoing an interrupted call finishes it.
 *
 * @retval 0       stored
 * @retval -EINVAL a card id is empty or longer than PERSONDB_CARD_LEN, or a
 *                 count exceeds its cap — rejected before the first write
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
 *
 * Repairable: redoing an interrupted call completes the index write rather
 * than reporting the card as already assigned and stopping.
 *
 * @retval -EEXIST the card is currently held by a different person; revoke it
 *                 first. A card is a physical object, so co-ownership is not a
 *                 state this API will enter.
 * @retval -EINVAL empty card id, or longer than PERSONDB_CARD_LEN
 * @retval -ENOSPC the person already holds PERSONDB_CARDS_MAX cards
 */
int persondb_card_assign(struct persondb *db, uint32_t person_id,
			 const char *card);

/**
 * @brief Revoke a card from a named person.
 *
 * Deletes the index entry first and rewrites the person second — the mirror
 * of assignment, and fail-safe for the same reason: after the first write the
 * card already resolves to nothing.
 *
 * Prefer this form wherever the caller knows the person. Because it does not
 * need the index to find the record, redoing an interrupted call converges;
 * the card-only form below cannot.
 */
int persondb_card_revoke_from(struct persondb *db, uint32_t person_id,
			      const char *card);

/**
 * @brief Revoke a card, resolving its holder through the index.
 *
 * Convenience for callers holding nothing but a card id — a reader at a door,
 * the shell. It is *not* repairable: the index entry it needs to find the
 * person is the first thing it deletes, so a crash between the two writes
 * leaves the card listed in a record this function can no longer identify,
 * and the redo returns -ENOENT.
 *
 * That is the honest cost of one index and no multi-key transaction, not an
 * omission — see persondb_card_revoke_from().
 *
 * @retval -ENOENT no such credential
 */
int persondb_card_revoke(struct persondb *db, const char *card);

/**
 * @retval 0       resolved; *person_id set
 * @retval -ENOENT unknown credential
 */
int persondb_card_owner(struct persondb *db, const char *card,
			uint32_t *person_id);

/* -- permissions -------------------------------------------------------- */

/**
 * @brief Add @p perm to a person, if not already held.
 *
 * @retval -EINVAL empty, or longer than PERSONDB_PERM_MAX. Truncating instead
 *                 would store a permission that cannot afterwards be revoked
 *                 by the name it was granted under.
 */
int persondb_permission_grant(struct persondb *db, uint32_t person_id,
			      const char *perm);

/** Remove @p perm from a person. @retval -ENOENT not held. */
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

/* -- record helpers ----------------------------------------------------- */

/*
 * These exist so callers can compare and account for persons without knowing
 * how one is serialized. Every one of them is a question about *storage* — are
 * these the same stored record, how much room does it take — so the answer
 * belongs here rather than in whatever is asking.
 */

/**
 * @brief True iff @p a and @p b are the same stored record.
 *
 * Compares canonical encodings, so equal means every field matched — including
 * fields added to the schema after this function was written.
 */
bool persondb_person_equal(const struct persondb_person *a,
			   const struct persondb_person *b);

/** Bytes the person's own entry occupies: framing + key + encoded record. */
size_t persondb_person_record_bytes(const struct persondb_person *p);

/** Bytes this person's credential index entries occupy, in total. */
size_t persondb_person_credential_bytes(const struct persondb_person *p);

/**
 * @brief Encode then decode a person, touching no storage.
 *
 * The control for the codec benchmark: it isolates serialization cost from
 * everything the storage stack does around it.
 */
int persondb_person_roundtrip(const struct persondb_person *p,
			      size_t *encoded_len);

/* -- introspection ------------------------------------------------------ */

int persondb_stat(struct persondb *db, struct persondb_stat *out);

/** Zero the traffic counters, so a phase can be measured in isolation. */
void persondb_counters_reset(struct persondb *db);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_PERSONDB_H_ */
