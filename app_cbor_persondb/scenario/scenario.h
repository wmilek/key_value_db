/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * scenario — everything one does *with* a population, above persondb.
 *
 * This layer exists so that neither frontend contains logic (DESIGN.md R-I,
 * F14/F15). It fills, verifies, mutates, measures and reports; it never
 * prints. Results come back as plain structs, which is what lets the
 * benchmark render a one-line summary and the shell render a table from the
 * same call — and would let a third frontend emit JSON without touching
 * anything here.
 */

#ifndef APP_CBOR_PERSONDB_SCENARIO_H_
#define APP_CBOR_PERSONDB_SCENARIO_H_

#include <stdbool.h>
#include <stdint.h>

#include "persondb.h"

#ifdef __cplusplus
extern "C" {
#endif

struct scenario {
	struct persondb *db;
	uint32_t         n_persons;
};

/**
 * @brief Microseconds from a clock that actually advances on this target.
 *
 * On hardware this is `k_uptime_get()`. On `native_sim` it is host real time,
 * because the simulator's clock only moves when a modelled event fires — the
 * flash simulator is RAM and the CPU is not cycle-accurate, so a run that
 * takes a second of wall time advances simulated time by nothing at all. Every
 * duration `native_sim` reports would otherwise be zero.
 *
 * `native_sim` timings are still not comparable to hardware; only the
 * operation counts and amplification figures are. See RESULTS.md.
 */
int64_t scenario_now_us(void);

/** Microseconds since @p since_us, from @ref scenario_now_us. */
int64_t scenario_since_us(int64_t since_us);

/* -- lifecycle ---------------------------------------------------------- */

/** Mount, bootstrap the registry, open (or build) the store. */
int scenario_open(struct scenario *s);
int scenario_close(struct scenario *s);

/** Drop everything and start over (CONFIG_..._FRESH_START, or `reset`). */
int scenario_erase(struct scenario *s);

/* -- population --------------------------------------------------------- */

struct scenario_fill_report {
	uint32_t written;     /* persons written by this call */
	uint32_t populated;   /* total now in the store */
	uint32_t target;
	bool     complete;
	uint32_t enospc;      /* bucket overflows — expected to be zero (A8) */
	int64_t  ms;
	uint32_t batches;     /* progress commits, i.e. resume points */
};

/**
 * @brief Write up to @p budget more persons, committing progress per batch.
 *
 * Resumable: a run that is interrupted anywhere resumes from the last
 * committed count, and replaying a partial batch rewrites identical values,
 * so replay costs time and nothing else (DESIGN.md F8). Pass 0 for "as many
 * as it takes".
 */
int scenario_fill(struct scenario *s, uint32_t budget,
		  struct scenario_fill_report *out);

/** Pre-erase the store's flash blocks so first-touch erases leave the timed
 *  loops — and so their cost shows up as its own line rather than inside the
 *  fill. Reports how many were formatted.
 */
int scenario_prepare(struct scenario *s, int *buckets_formatted, int64_t *ms);

/* -- verification ------------------------------------------------------- */

struct scenario_verify_report {
	uint32_t persons_checked;
	uint32_t cards_checked;
	uint32_t bad_records;   /* content differs from the generator */
	uint32_t bad_cards;     /* credential resolves to the wrong person */
	uint32_t missing;       /* should be present, is not */
	int64_t  ms;
};

/**
 * @brief Re-derive a deterministic sample and compare it with the store.
 *
 * Each sampled person is compared by canonical CBOR encoding — equal bytes
 * means every field matched — and each of its cards is resolved back through
 * the credential index to that same person.
 */
int scenario_verify(struct scenario *s, uint32_t samples,
		    struct scenario_verify_report *out);

/* -- mutation ----------------------------------------------------------- */

struct scenario_mutate_report {
	uint32_t rev_from, rev_to;
	uint32_t revoked, assigned, skipped;
	int64_t  ms;
};

/**
 * @brief Advance one revision: revoke the old subset's temporary cards, grant
 *        the new subset's, commit.
 *
 * Exercises both fail-safe orderings (F5) and, because the expected state is
 * a pure function of (index, rev), lets the next boot prove that this run's
 * writes survived. Idempotent, so an interrupted round can simply be redone.
 */
int scenario_mutate(struct scenario *s, struct scenario_mutate_report *out);

/* -- measurement -------------------------------------------------------- */

/*
 * The instant an access decision is evaluated at, shared by the benchmark and
 * the shell so the two cannot drift into asking different questions.
 *
 * It sits inside the validity window of ~99.5 % of generated persons, not all
 * of them: dataset.c issues badges across a two-year span ending at
 * 1 830 297 599, so the last ~0.47 % are not yet valid here and their decision
 * short-circuits on EXPIRED before the permission compare. The benchmark
 * reports those separately — RESULTS.md §8's 10 000-person capture records 2 of
 * 200 samples, and the 1 000- and 200-person runs record none, because which
 * indices a sample lands on decides whether it meets one.
 *
 * They are left alone rather than tuned away. A decision path that never takes
 * its cheap exit is not the one a product sees either, and moving this constant
 * would silently redefine the benchmark.
 */
#define SCENARIO_BENCH_NOW 1830000000u

enum scenario_bench_kind {
	SCENARIO_BENCH_CHECK = 0, /* card -> person -> permission (R-D) */
	SCENARIO_BENCH_BYID,      /* person id -> record */
	SCENARIO_BENCH_MISS,      /* unknown card */
	SCENARIO_BENCH_PUT,       /* rewrite a record with identical content */
	SCENARIO_BENCH_CBOR,      /* encode + decode, no flash */
	SCENARIO_BENCH_KINDS,
};

struct bench_result {
	const char *name;
	uint32_t    ops;
	int64_t     us;             /* total; the fast phases are sub-millisecond */
	int64_t     ms;
	uint32_t    us_per_op;
	uint64_t    store_ops;      /* persondb get/set/delete this phase performed */
	uint64_t    flash_ops;      /* flash reads + writes + erases underneath */
	uint64_t    flash_bytes;    /* bytes actually moved, measured underneath */
	uint64_t    payload_bytes;  /* what the application actually asked for */
	uint32_t    amplification;  /* flash_bytes / payload_bytes */
	bool        measured;       /* false if CONFIG_BLOB_DB_IOSTATS is off */
	uint32_t    granted, denied, unknown, expired;  /* CHECK only */
};

int scenario_bench(struct scenario *s, enum scenario_bench_kind which,
		   uint32_t samples, struct bench_result *out);

const char *scenario_bench_name(enum scenario_bench_kind which);

/* -- reporting ---------------------------------------------------------- */

struct scenario_report {
	struct persondb_stat st;
	uint64_t logical_bytes;   /* keys + values + per-entry overhead */
	uint32_t fill_permille;   /* of the partition */
	uint32_t mean_record;     /* mean encoded person, bytes */
	uint32_t credentials;

	/* Peak main-stack usage, when CONFIG_INIT_STACKS makes it knowable.
	 * Worth reporting: blob_db builds every slot on the caller's stack
	 * (CONFIG_BLOB_DB_MAX_PAYLOAD_LEN + 46 bytes of it), so the stack this
	 * app must reserve is set by a library constant, not by its own call
	 * depth. 0 means the build cannot measure it. */
	size_t   stack_used;
	size_t   stack_size;
};

/**
 * @brief Account for what the store holds.
 *
 * Computed from the generator, not from the store: there is no iteration
 * (FINDINGS.md K6) and no occupancy query (B3), so the only way an
 * application can say how full its flash is, is to re-derive its own content
 * and add it up. Physical occupancy — live bytes plus uncompacted garbage —
 * stays unobservable.
 */
int scenario_report_get(struct scenario *s, struct scenario_report *out);

/* -- single-record access, for the shell -------------------------------- */

int scenario_person_show(struct scenario *s, uint32_t index,
			 struct persondb_person *out);
int scenario_card_of(struct scenario *s, uint32_t index, uint8_t slot,
		     char *out, size_t sz);

/**
 * @brief The dataset index a person id came from.
 *
 * Only the generator knows how ids are numbered. Exposing the inverse here
 * keeps that knowledge below the frontends, which otherwise have to include
 * dataset.h to print a person's index — a dependency from the top layer
 * straight to the bottom one.
 */
uint32_t scenario_index_of(uint32_t person_id);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_SCENARIO_H_ */
