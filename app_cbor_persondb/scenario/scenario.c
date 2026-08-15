/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * scenario — fill, verify, mutate, measure, report. No printing.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dataset.h"
#include "scenario.h"

LOG_MODULE_DECLARE(persondb, CONFIG_APP_CBOR_PERSONDB_LOG_LEVEL);

#define N_PERSONS     CONFIG_APP_CBOR_PERSONDB_N_PERSONS
#define FILL_BATCH    CONFIG_APP_CBOR_PERSONDB_FILL_BATCH
#define MUTATE_COUNT  CONFIG_APP_CBOR_PERSONDB_MUTATE_COUNT


/* -- clock -------------------------------------------------------------- */

#ifdef CONFIG_ARCH_POSIX
#include "native_rtc.h"
#endif

int64_t scenario_now_us(void)
{
#ifdef CONFIG_ARCH_POSIX
	/* native_sim's simulated clock only advances when a modelled event
	 * fires. RAM-backed flash models none, so k_uptime_get() would report
	 * zero for every phase here. Host real time at least measures the
	 * work; it is not comparable to hardware, and RESULTS.md says so. */
	return (int64_t)native_rtc_gettime_us(RTC_CLOCK_PSEUDOHOSTREALTIME);
#else
	return k_uptime_get() * 1000;
#endif
}

int64_t scenario_since_us(int64_t since_us)
{
	return scenario_now_us() - since_us;
}

/* -- lifecycle ---------------------------------------------------------- */

int scenario_open(struct scenario *s)
{
	int rc = persondb_open(&s->db, N_PERSONS);
	if (rc != 0) {
		return rc;
	}

	struct persondb_stat st;

	persondb_stat(s->db, &st);
	s->n_persons = st.n_persons;
	return 0;
}

int scenario_close(struct scenario *s)
{
	if (s->db) {
		persondb_close(s->db);
		s->db = NULL;
	}
	return 0;
}

int scenario_erase(struct scenario *s)
{
	int rc = persondb_erase(&s->db, N_PERSONS);

	if (rc == 0) {
		s->n_persons = N_PERSONS;
	}
	return rc;
}

int scenario_prepare(struct scenario *s, int *buckets_formatted, int64_t *ms)
{
	int64_t t = scenario_now_us();
	int rc = persondb_prepare(buckets_formatted);

	ARG_UNUSED(s);
	if (ms) {
		*ms = scenario_since_us(t) / 1000;
	}
	return rc;
}

/* -- population --------------------------------------------------------- */

int scenario_fill(struct scenario *s, uint32_t budget,
		  struct scenario_fill_report *out)
{
	struct persondb_person p;
	struct persondb_stat st0;
	uint32_t populated, rev;
	int64_t t = scenario_now_us();
	int rc;

	memset(out, 0, sizeof(*out));
	persondb_stat(s->db, &st0);
	persondb_progress_get(s->db, &populated, &rev);

	out->target = s->n_persons;
	if (budget == 0) {
		budget = s->n_persons;
	}

	while (populated < s->n_persons && out->written < budget) {
		uint32_t batch = MIN(MIN((uint32_t)FILL_BATCH,
					 s->n_persons - populated),
				     budget - out->written);

		for (uint32_t i = 0; i < batch; i++) {
			dataset_person(populated + i, rev, s->n_persons, &p);
			rc = persondb_person_put(s->db, &p);
			if (rc != 0) {
				LOG_ERR("put person %u: %d", populated + i, rc);
				goto done;
			}
		}

		populated += batch;
		out->written += batch;

		/* Commit the batch. Everything before this point is already on
		 * flash and idempotent, so a crash costs at most one batch of
		 * repeated work — never a corrupt store, and never a restart. */
		rc = persondb_progress_set(s->db, populated, rev);
		if (rc != 0) {
			LOG_ERR("commit progress: %d", rc);
			goto done;
		}
		out->batches++;
	}
	rc = 0;

done:
	{
		struct persondb_stat st1;

		persondb_stat(s->db, &st1);
		out->enospc = st1.enospc_hits - st0.enospc_hits;
	}
	out->populated = populated;
	out->complete = (populated >= s->n_persons);
	out->ms = scenario_since_us(t) / 1000;
	return rc;
}

/* -- verification ------------------------------------------------------- */

int scenario_verify(struct scenario *s, uint32_t samples,
		    struct scenario_verify_report *out)
{
	struct persondb_person want, got;
	uint32_t populated, rev;
	int64_t t = scenario_now_us();

	memset(out, 0, sizeof(*out));
	persondb_progress_get(s->db, &populated, &rev);
	if (populated == 0) {
		out->ms = scenario_since_us(t) / 1000;
		return 0;
	}
	if (samples > populated) {
		samples = populated;
	}

	for (uint32_t n = 0; n < samples; n++) {
		uint32_t idx = dataset_sample_index(n, samples, populated);

		dataset_person(idx, rev, s->n_persons, &want);

		int rc = persondb_person_get(s->db, want.id, &got);

		out->persons_checked++;
		if (rc == -ENOENT) {
			out->missing++;
			continue;
		}
		if (rc != 0) {
			LOG_ERR("verify: get person %u: %d", want.id, rc);
			out->bad_records++;
			continue;
		}
		if (!persondb_person_equal(&want, &got)) {
			LOG_ERR("verify: person %u differs from the generator",
				want.id);
			out->bad_records++;
			continue;
		}

		/* Every card the record lists must resolve, through the index,
		 * back to this same person. This is what would catch a torn
		 * assign/revoke that left the two structures disagreeing. */
		for (uint8_t c = 0; c < want.n_cards; c++) {
			uint32_t owner = 0;

			rc = persondb_card_owner(s->db, want.card[c], &owner);
			out->cards_checked++;
			if (rc != 0 || owner != want.id) {
				LOG_ERR("verify: card %s -> %u (rc=%d), expected %u",
					want.card[c], owner, rc, want.id);
				out->bad_cards++;
			}
		}
	}

	out->ms = scenario_since_us(t) / 1000;
	return 0;
}

/* -- mutation ----------------------------------------------------------- */

int scenario_mutate(struct scenario *s, struct scenario_mutate_report *out)
{
	uint32_t populated, rev;
	char card[PERSONDB_CARD_LEN + 1];
	int64_t t = scenario_now_us();
	int rc;

	memset(out, 0, sizeof(*out));
	persondb_progress_get(s->db, &populated, &rev);
	out->rev_from = rev;
	out->rev_to = rev + 1;

	if (populated == 0) {
		out->ms = scenario_since_us(t) / 1000;
		return 0;
	}

	/* Revoke the outgoing subset. Index first, then the record (F5). */
	if (rev > 0) {
		for (uint32_t n = 0; n < MUTATE_COUNT; n++) {
			uint32_t idx = dataset_mutate_index(n, rev, s->n_persons,
							    MUTATE_COUNT);

			if (idx >= populated) {
				out->skipped++;
				continue;
			}
			dataset_card(idx, DATASET_TEMP_CARD_SLOT, card);
			/* The owner-qualified form, because this caller knows
			 * the person: a round interrupted between the index
			 * delete and the record rewrite is then finished by the
			 * redo. The card-only form would resolve -ENOENT and
			 * leave the record listing a card forever. */
			rc = persondb_card_revoke_from(s->db, dataset_id_of(idx),
						       card);
			if (rc == 0) {
				out->revoked++;
			} else if (rc != -ENOENT) {
				LOG_ERR("revoke %s: %d", card, rc);
				goto done;
			}
		}
	}

	/* Grant the incoming subset. Record first, then the index (F5). */
	for (uint32_t n = 0; n < MUTATE_COUNT; n++) {
		uint32_t idx = dataset_mutate_index(n, rev + 1, s->n_persons,
						    MUTATE_COUNT);

		if (idx >= populated) {
			out->skipped++;
			continue;
		}
		dataset_card(idx, DATASET_TEMP_CARD_SLOT, card);
		rc = persondb_card_assign(s->db, dataset_id_of(idx), card);
		if (rc != 0) {
			LOG_ERR("assign %s: %d", card, rc);
			goto done;
		}
		out->assigned++;
	}

	/* Commit the revision. Until this lands the store still reads as the
	 * previous revision, and redoing the round is free. */
	rc = persondb_progress_set(s->db, populated, rev + 1);

done:
	out->ms = scenario_since_us(t) / 1000;
	return rc;
}

/* -- measurement -------------------------------------------------------- */

static const char *const bench_names[SCENARIO_BENCH_KINDS] = {
	"check", "byid", "miss", "put", "cbor",
};

const char *scenario_bench_name(enum scenario_bench_kind which)
{
	return which < SCENARIO_BENCH_KINDS ? bench_names[which] : "?";
}

/*
 * The instant the access decision is benchmarked at.
 *
 * It sits inside the validity window of ~99.5 % of generated persons, not all
 * of them: dataset.c issues badges across a two-year span ending at
 * 1 830 297 599, so the last ~0.47 % are not yet valid here and their `check`
 * short-circuits on EXPIRED before the permission compare. The benchmark
 * reports those separately: RESULTS.md §8's 10 000-person capture records 2 of
 * 200 samples, and the 1 000- and 200-person runs record none, because which
 * indices a sample lands on decides whether it meets one.
 *
 * They are left alone rather than tuned away. A decision path that never takes
 * its cheap exit is not the one a product sees either, and moving this constant
 * would silently redefine the benchmark.
 */
#define BENCH_NOW 1830000000u

int scenario_bench(struct scenario *s, enum scenario_bench_kind which,
		   uint32_t samples, struct bench_result *out)
{
	struct persondb_person want, got;
	struct persondb_stat st0, st1;
	char card[PERSONDB_CARD_LEN + 1];
	uint32_t populated, rev;
	int64_t t;

	memset(out, 0, sizeof(*out));
	out->name = scenario_bench_name(which);

	persondb_progress_get(s->db, &populated, &rev);
	if (populated == 0) {
		return -ENODATA;
	}
	if (samples > populated) {
		samples = populated;
	}

	persondb_counters_reset(s->db);
	persondb_stat(s->db, &st0);
	t = scenario_now_us();

	for (uint32_t n = 0; n < samples; n++) {
		uint32_t idx = dataset_sample_index(n, samples, populated);
		size_t len = 0;
		int rc;

		dataset_person(idx, rev, s->n_persons, &want);

		switch (which) {
		case SCENARIO_BENCH_CHECK: {
			enum persondb_decision d;

			rc = persondb_check(s->db, want.card[0],
					    want.perm[n % want.n_perms],
					    BENCH_NOW, &d, NULL);
			if (rc != 0) {
				return rc;
			}
			switch (d) {
			case PERSONDB_GRANTED:      out->granted++; break;
			case PERSONDB_DENIED:       out->denied++; break;
			case PERSONDB_UNKNOWN_CARD: out->unknown++; break;
			case PERSONDB_EXPIRED:      out->expired++; break;
			}
			/* What the application asked for: one record plus the
			 * one index entry that found it. What it moved to get
			 * there is counted below. */
			out->payload_bytes +=
				persondb_person_record_bytes(&want) +
				persondb_person_credential_bytes(&want) /
					want.n_cards;
			break;
		}
		case SCENARIO_BENCH_BYID:
			rc = persondb_person_get(s->db, want.id, &got);
			if (rc != 0) {
				return rc;
			}
			out->payload_bytes += persondb_person_record_bytes(&want);
			break;

		case SCENARIO_BENCH_MISS:
			/* Slot 6 is never assigned to anyone, so this is a
			 * well-formed card that simply does not exist. */
			dataset_card(idx, 6, card);
			{
				uint32_t owner;

				rc = persondb_card_owner(s->db, card, &owner);
				if (rc != -ENOENT) {
					LOG_ERR("miss: card %s resolved (rc=%d)",
						card, rc);
					return rc == 0 ? -EILSEQ : rc;
				}
			}
			/* Nothing comes back, so the payload is the index entry
			 * the lookup would have returned had the card existed. */
			out->payload_bytes +=
				persondb_person_credential_bytes(&want) /
				want.n_cards;
			break;

		case SCENARIO_BENCH_PUT:
			/* Rewrite the record the generator says should be
			 * there: measures the write path without changing a
			 * byte of content, so an interrupted benchmark cannot
			 * leave the store disagreeing with the generator. */
			rc = persondb_person_put(s->db, &want);
			if (rc != 0) {
				return rc;
			}
			out->payload_bytes +=
				persondb_person_record_bytes(&want) +
				persondb_person_credential_bytes(&want);
			break;

		case SCENARIO_BENCH_CBOR:
			/* No flash at all: the control that says whether the
			 * serialization choice is worth arguing about. */
			rc = persondb_person_roundtrip(&want, &len);
			if (rc != 0) {
				return rc;
			}
			out->payload_bytes += len;
			break;
		default:
			return -EINVAL;
		}
		out->ops++;
	}

	out->us = scenario_since_us(t);
	out->ms = out->us / 1000;
	persondb_stat(s->db, &st1);

	out->map_ops = st1.map_gets + st1.map_sets + st1.map_dels;
	out->flash_ops = st1.flash_reads + st1.flash_writes + st1.flash_erases;
	out->flash_bytes = st1.bytes_read + st1.bytes_written + st1.bytes_erased;
	out->measured = st1.io_measured;
	/* Microseconds, not milliseconds/ops: `cbor` runs in a few hundred
	 * microseconds in total, and rounding that to whole milliseconds is
	 * exactly the comparison this benchmark exists to make. */
	out->us_per_op = out->ops ? (uint32_t)(out->us / out->ops) : 0;
	out->amplification = out->payload_bytes
				     ? (uint32_t)(out->flash_bytes /
						  out->payload_bytes)
				     : 0;
	return 0;
}

/* -- reporting ---------------------------------------------------------- */

int scenario_report_get(struct scenario *s, struct scenario_report *out)
{
	struct persondb_person p;
	uint32_t populated, rev;
	uint64_t person_bytes = 0;
	uint32_t creds = 0;

	memset(out, 0, sizeof(*out));
	persondb_stat(s->db, &out->st);
	persondb_progress_get(s->db, &populated, &rev);

	/* Re-derive the content and add it up. There is no iteration (K6) and
	 * no occupancy query (B3), so this is the only route — and it works
	 * only because the dataset is a pure function of its index. A real
	 * deployment, whose data comes from outside, could not do this at all. */
	for (uint32_t i = 0; i < populated; i++) {
		dataset_person(i, rev, s->n_persons, &p);
		person_bytes += persondb_person_record_bytes(&p) +
				persondb_person_credential_bytes(&p);
		creds += p.n_cards;
	}

	/* Peak stack, when the build can measure it. Enable with
	 *   -DCONFIG_INIT_STACKS=y -DCONFIG_THREAD_STACK_INFO=y
	 * Worth knowing because the number is set by a library constant rather
	 * than by this application's call depth: blob_db's append_slot() builds
	 * every slot in a CONFIG_BLOB_DB_MAX_PAYLOAD_LEN + 46 byte stack frame
	 * (FINDINGS.md B5, job 3). */
/* Not on POSIX: native_sim's main thread runs on a host pthread stack that
	 * Zephyr never paints, so the scan reports a meaningless few bytes. */
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO) && \
	!defined(CONFIG_ARCH_POSIX)
	{
		size_t unused = 0;

		if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
			out->stack_size = CONFIG_MAIN_STACK_SIZE;
			out->stack_used = CONFIG_MAIN_STACK_SIZE - unused;
		}
	}
#endif

	out->credentials = creds;
	out->logical_bytes = person_bytes;
	out->mean_record = populated ? (uint32_t)(person_bytes / populated) : 0;
	out->fill_permille =
		out->st.partition_bytes
			? (uint32_t)((person_bytes * 1000u) /
				     out->st.partition_bytes)
			: 0;
	return 0;
}

/* -- single-record access ----------------------------------------------- */

int scenario_person_show(struct scenario *s, uint32_t index,
			 struct persondb_person *out)
{
	if (index >= s->n_persons) {
		return -EINVAL;
	}
	return persondb_person_get(s->db, dataset_id_of(index), out);
}

uint32_t scenario_index_of(uint32_t person_id)
{
	return dataset_index_of(person_id);
}

int scenario_card_of(struct scenario *s, uint32_t index, uint8_t slot,
		     char *out, size_t sz)
{
	if (index >= s->n_persons || sz < PERSONDB_CARD_LEN + 1) {
		return -EINVAL;
	}
	dataset_card(index, slot, out);
	return 0;
}
