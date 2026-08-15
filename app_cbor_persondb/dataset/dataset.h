/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * dataset — the synthetic population, as a pure function of an index.
 *
 * Every field of person i is derived from i alone. Nothing here reads or
 * writes storage, and nothing is remembered between calls.
 *
 * That is not a shortcut, it is the only way this application can verify
 * itself: the Map shape has no iteration (FINDINGS.md K6), so a store cannot
 * be enumerated, and a run has no way to discover what a previous run wrote.
 * Making the expected content computable from an index means verification
 * needs no shadow state, no journal, and no RAM proportional to the dataset
 * (P3) — a rerun re-derives what boot N-1 should have stored and checks it.
 *
 * The one time-varying input is `rev`, the mutation revision, which decides
 * only whether a person currently holds its temporary card.
 */

#ifndef APP_CBOR_PERSONDB_DATASET_H_
#define APP_CBOR_PERSONDB_DATASET_H_

#include <stdbool.h>
#include <stdint.h>

#include "persondb.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Person ids start here, so an id never collides with an array index. */
#define DATASET_ID_BASE 100000u

/** Size of the permission vocabulary the generator draws from. */
#define DATASET_PERM_VOCAB 32

/**
 * @brief Fill @p out with person @p index as it should exist at @p rev.
 *
 * @param index 0 .. n_persons-1
 * @param rev   mutation revision; person @p index carries its temporary card
 *              iff @ref dataset_has_temp_card says so
 * @param n_persons dataset size, needed to place the mutation subset
 */
void dataset_person(uint32_t index, uint32_t rev, uint32_t n_persons,
		    struct persondb_person *out);

/** Person id for a dataset index (and its inverse). */
static inline uint32_t dataset_id_of(uint32_t index)
{
	return DATASET_ID_BASE + index;
}
static inline uint32_t dataset_index_of(uint32_t id)
{
	return id - DATASET_ID_BASE;
}

/** How many permanent cards person @p index holds (1..4). */
uint8_t dataset_n_cards(uint32_t index);

/**
 * @brief Write person @p index's card @p slot into @p out.
 *
 * Slots 0..dataset_n_cards()-1 are permanent. Slot
 * @ref DATASET_TEMP_CARD_SLOT is the temporary card the mutation round
 * assigns and revokes; it is generated the same way, so it is unique across
 * the whole population too.
 *
 * @param out buffer of at least PERSONDB_CARD_LEN + 1 bytes
 */
void dataset_card(uint32_t index, uint8_t slot, char *out);

/** The slot number reserved for the mutation round's temporary card. */
#define DATASET_TEMP_CARD_SLOT 7

/**
 * @brief Does person @p index hold its temporary card at revision @p rev?
 *
 * The subset moves with the revision, so consecutive runs touch disjoint
 * persons and a rerun can tell whether the previous run committed.
 */
bool dataset_has_temp_card(uint32_t index, uint32_t rev, uint32_t n_persons,
			   uint32_t mutate_count);

/**
 * @brief The dataset index of the @p nth member of revision @p rev's subset.
 *
 * Lets the mutation round walk exactly the persons it must touch without
 * scanning.
 */
uint32_t dataset_mutate_index(uint32_t nth, uint32_t rev, uint32_t n_persons,
			      uint32_t mutate_count);

/** A permission string from the vocabulary, for benchmarks and the shell. */
const char *dataset_permission(uint8_t vocab_index);

/**
 * @brief Spread @p nth of @p count samples across a population of
 *        @p n_persons, deterministically and without clustering.
 */
uint32_t dataset_sample_index(uint32_t nth, uint32_t count, uint32_t n_persons);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_DATASET_H_ */
