/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * scenario — fill, verify, mutate, measure, report. No printing.
 */

#include <errno.h>
#include <stdio.h>
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
	/* Ticks, not k_uptime_get(): the millisecond clock was reporting whole
	 * milliseconds and multiplying by 1000 to look like microseconds, which
	 * is invisible when a phase averages over 200 samples and useless for
	 * the shell's single-shot `check` — 27 ms quantised to 1 ms is +-3.7 %,
	 * and it cannot separate the 26.79 and 27.35 ms two benchmark runs
	 * measured for the same operation (RESULTS.md §5f).
	 *
	 * On this SoC CONFIG_SYS_CLOCK_TICKS_PER_SEC is 32 768, so this reads
	 * to ~30.5 us — the same resolution k_cycle_get_32() offers here,
	 * because the RTC is both the tick and the cycle source.
	 *
	 * k_uptime_ticks() rather than k_cycle_get_32() because this function
	 * returns an ABSOLUTE time that scenario_since_us() subtracts, and the
	 * 32-bit cycle counter wraps every 36 h at 32 768 Hz — the nRF5340 does
	 * not set CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER, so there is no 64-bit
	 * cycle counter to reach for. Ticks are int64 and monotonic.
	 *
	 * For sub-microsecond work the portable escape is CONFIG_TIMING_FUNCTIONS
	 * (timing_counter_get(), DWT-backed at 128 MHz on Cortex-M), but it
	 * needs explicit init and its 32-bit counters wrap in 33.5 s there —
	 * shorter than a single fill phase.
	 */
	return k_ticks_to_us_floor64(k_uptime_ticks());
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
			/* Named, not just counted: a person in the filled range
			 * with no record was deleted (persondb tui) or lost, and
			 * `repair <person-id>` needs to know which. */
			LOG_ERR("verify: person %u missing", want.id);
			out->missing++;
			continue;
		}
		if (rc != 0) {
			LOG_ERR("verify: get person %u: %d", want.id, rc);
			out->bad_records++;
			continue;
		}
		if (!persondb_person_equal(&want, &got)) {
			char why[96];

			scenario_diff_describe(scenario_person_diff(&want, &got),
					       why, sizeof(why));
			LOG_ERR("verify: person %u differs from the generator: "
				"%s", want.id, why);
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

/* 0: the same list. 1: the same members in another order. 2: different
 * members. The lists are rows of a char matrix, @p stride bytes apart. */
static int list_cmp(const char *a, uint8_t na, const char *b, uint8_t nb,
		    size_t stride)
{
	bool same_order = true;

	if (na != nb) {
		return 2;
	}
	for (uint8_t i = 0; i < na; i++) {
		if (strcmp(a + i * stride, b + i * stride) != 0) {
			same_order = false;
			break;
		}
	}
	if (same_order) {
		return 0;
	}
	for (uint8_t i = 0; i < na; i++) {
		bool found = false;

		for (uint8_t j = 0; j < nb && !found; j++) {
			found = (strcmp(a + i * stride, b + j * stride) == 0);
		}
		if (!found) {
			return 2;
		}
	}
	return 1;
}

uint32_t scenario_person_diff(const struct persondb_person *want,
			      const struct persondb_person *got)
{
	uint32_t d = 0;
	int c;

	if (want->id != got->id) {
		d |= SCENARIO_DIFF_ID;
	}
	if (strcmp(want->name, got->name) != 0) {
		d |= SCENARIO_DIFF_NAME;
	}
	if (strcmp(want->dept, got->dept) != 0) {
		d |= SCENARIO_DIFF_DEPT;
	}
	if (strcmp(want->title, got->title) != 0) {
		d |= SCENARIO_DIFF_TITLE;
	}
	if (want->valid_from != got->valid_from ||
	    want->valid_until != got->valid_until) {
		d |= SCENARIO_DIFF_VALIDITY;
	}
	if (memcmp(want->pin_hash, got->pin_hash, sizeof(want->pin_hash)) != 0) {
		d |= SCENARIO_DIFF_PIN;
	}

	/* Order is its own reason because the comparison is by encoded bytes:
	 * revoking a permission and granting it back restores the set but not
	 * the order, and verify still fails. Saying so saves the hunt. */
	c = list_cmp(want->perm[0], want->n_perms, got->perm[0], got->n_perms,
		     sizeof(want->perm[0]));
	d |= (c == 2) ? SCENARIO_DIFF_PERMS : (c == 1) ? SCENARIO_DIFF_PERM_ORDER : 0;
	c = list_cmp(want->card[0], want->n_cards, got->card[0], got->n_cards,
		     sizeof(want->card[0]));
	d |= (c == 2) ? SCENARIO_DIFF_CARDS : (c == 1) ? SCENARIO_DIFF_CARD_ORDER : 0;

	if (d == 0 && !persondb_person_equal(want, got)) {
		d = SCENARIO_DIFF_OTHER;
	}
	return d;
}

static const char *const diff_names[] = {
	"missing", "unreadable", "id", "name", "dept", "title", "validity",
	"pin", "perms", "perm order", "cards", "card order", "card index",
	"other",
};
BUILD_ASSERT(1u << (ARRAY_SIZE(diff_names) - 1) == SCENARIO_DIFF_OTHER,
	     "one name per enum scenario_diff bit");

void scenario_diff_describe(uint32_t diff, char *buf, size_t sz)
{
	size_t len = 0;

	if (sz == 0) {
		return;
	}
	buf[0] = '\0';
	if (diff == 0) {
		snprintf(buf, sz, "none");
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(diff_names) && len < sz; i++) {
		if (diff & (1u << i)) {
			int n = snprintf(buf + len, sz - len, "%s%s",
					 len ? ", " : "", diff_names[i]);

			if (n < 0) {
				break;
			}
			len += (size_t)n;
		}
	}
}

/* -- mutation ----------------------------------------------------------- */

int scenario_mutate(struct scenario *s, struct scenario_mutate_report *out)
{
	uint32_t populated, rev;
	char card[PERSONDB_CARD_LEN + 1];
	int64_t t = scenario_now_us();
	int rc;

	/* Never ask for more distinct persons than exist: the generator strides
	 * the subset across the population, and a stride that rounds to zero
	 * would hand back the same index repeatedly — a round that reports 64
	 * mutations having touched one person. Kconfig permits the combination
	 * (MUTATE_COUNT up to 4096, N_PERSONS down to 8), so clamp it here. */
	const uint32_t count = MIN((uint32_t)MUTATE_COUNT, s->n_persons);

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
		for (uint32_t n = 0; n < count; n++) {
			uint32_t idx = dataset_mutate_index(n, rev,
							    s->n_persons, count);

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
	for (uint32_t n = 0; n < count; n++) {
		uint32_t idx = dataset_mutate_index(n, rev + 1, s->n_persons,
						    count);

		if (idx >= populated) {
			out->skipped++;
			continue;
		}
		dataset_card(idx, DATASET_TEMP_CARD_SLOT, card);
		rc = persondb_card_assign(s->db, dataset_id_of(idx), card);
		if (rc == -ENOENT || rc == -ENOSPC || rc == -EEXIST) {
			/* A hand edit (persondb tui) made this person's generated
			 * state unreachable: deleted (-ENOENT), given a fifth card
			 * by hand so the temporary one has no slot (-ENOSPC), or
			 * the temporary card's id assigned to someone else
			 * (-EEXIST). The dataset never produces any of these.
			 * Skipped rather than fatal, or every round whose subset
			 * includes this person would fail forever. */
			LOG_WRN("mutate: %s for index %u skipped: %d", card, idx,
				rc);
			out->skipped++;
			continue;
		}
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

int scenario_bench(struct scenario *s, enum scenario_bench_kind which,
		   uint32_t samples, struct bench_result *out)
{
	struct persondb_person want, got;
	struct persondb_stat st1;
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

	/* Reset rather than subtract a baseline: the counters start at zero for
	 * this phase, so the reading taken at the end *is* the delta. */
	persondb_counters_reset(s->db);
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
					    SCENARIO_BENCH_NOW, &d, NULL);
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

	out->store_ops = st1.map_gets + st1.map_sets + st1.map_dels;
	out->flash_ops = st1.flash_reads + st1.flash_writes + st1.flash_erases;
	out->flash_bytes = st1.bytes_read + st1.bytes_written + st1.bytes_erased;
	out->reads = st1.flash_reads;
	out->writes = st1.flash_writes;
	out->erases = st1.flash_erases;
	out->bytes_read = st1.bytes_read;
	out->bytes_written = st1.bytes_written;
	out->bytes_erased = st1.bytes_erased;
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

	/* Re-derive the content and add it up. This predates map_ops.next
	 * (K6, since closed) and there is still no occupancy query (B3); it
	 * works only because the dataset is a pure function of its index. A
	 * real deployment, whose data comes from outside, would instead walk
	 * its maps with map_ops.next. Persons added by hand (persondb tui)
	 * are not in this figure. */
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

int scenario_repair(struct scenario *s, uint32_t person_id,
		    struct scenario_repair_report *out)
{
	struct persondb_person want, got;
	/* An id below the base wraps to a huge index, so one test covers both
	 * ends — and ids above the dataset, which `persondb tui` hands out. */
	uint32_t index = dataset_index_of(person_id);
	uint32_t populated, rev;
	int rc;

	memset(out, 0, sizeof(*out));
	out->id = person_id;
	if (index >= s->n_persons) {
		return -EINVAL;
	}
	persondb_progress_get(s->db, &populated, &rev);
	if (index >= populated) {
		return -ERANGE;
	}

	dataset_person(index, rev, s->n_persons, &want);

	rc = persondb_person_get(s->db, want.id, &got);
	if (rc == -ENOENT) {
		out->diff = SCENARIO_DIFF_MISSING;
	} else if (rc == -EILSEQ) {
		/* persondb_person_put() reads the old record to drop the index
		 * entries of cards it no longer lists, and refuses when it
		 * cannot. Overwriting blind could leave one of those entries
		 * granting on behalf of a record that does not list it. */
		out->diff = SCENARIO_DIFF_UNREADABLE;
		return rc;
	} else if (rc != 0) {
		return rc;
	} else {
		out->diff = scenario_person_diff(&want, &got);
	}

	/* Every generated card must be free or already this person's. The
	 * check also catches the state verify skips once a record is bad: a
	 * matching record whose cards do not resolve back to it. */
	for (uint8_t c = 0; c < want.n_cards; c++) {
		uint32_t owner;

		rc = persondb_card_owner(s->db, want.card[c], &owner);
		if (rc == -ENOENT) {
			/* Said already, for a deleted person: delete drops
			 * every card along with the record. */
			if (!(out->diff & SCENARIO_DIFF_MISSING)) {
				out->diff |= SCENARIO_DIFF_CARD_INDEX;
			}
			continue;
		}
		if (rc != 0) {
			return rc;
		}
		if (owner != want.id) {
			snprintf(out->held_card, sizeof(out->held_card), "%s",
				 want.card[c]);
			out->held_by = owner;
			out->diff |= SCENARIO_DIFF_CARD_INDEX;
			return -EEXIST;
		}
	}

	if (out->diff == 0) {
		return 0;
	}
	rc = persondb_person_put(s->db, &want);
	out->written = (rc == 0);
	return rc;
}

uint32_t scenario_index_of(uint32_t person_id)
{
	return dataset_index_of(person_id);
}

uint32_t scenario_id_of(uint32_t index)
{
	return dataset_id_of(index);
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
