/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * dataset — the synthetic population, as a pure function of an index.
 * See dataset.h for why purity is load-bearing here.
 */

#include <stdio.h>
#include <string.h>

#include "dataset.h"

/* Access-control vocabulary: 32 permissions averaging ~13 characters, which
 * is what puts the mean person record near 350 B and the whole dataset near
 * 4 MiB (DESIGN.md §6). Realistic strings, not padding. */
static const char *const perm_vocab[DATASET_PERM_VOCAB] = {
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

/* Three independent mixers, so the fields of one person are not correlated
 * with each other — a population where everyone with the same first name also
 * has the same permission count would flatter the hash distribution that
 * DESIGN.md §6.1's sizing depends on. */
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

const char *dataset_permission(uint8_t vocab_index)
{
	return perm_vocab[vocab_index % DATASET_PERM_VOCAB];
}

uint8_t dataset_n_cards(uint32_t index)
{
	return (uint8_t)(1u + (mix(index, 3) % 4u));   /* 1..4, mean 2.5 */
}

void dataset_card(uint32_t index, uint8_t slot, char *out)
{
	/* A 7-byte UID, formatted as 14 hex characters like a real card.
	 * Multiplying by an odd constant is a bijection modulo 2^56, so
	 * distinct (index, slot) pairs never collide — which matters, because
	 * a duplicate credential would silently reassign a card to a second
	 * person and verification would blame the store. */
	uint64_t n = (uint64_t)index * (PERSONDB_CARDS_MAX + 3u) + slot;
	uint64_t uid = (n * 0x9e3779b97f4a7dULL) & 0x00ffffffffffffULL;

	snprintf(out, PERSONDB_CARD_LEN + 1, "%014llX", (unsigned long long)uid);
}

bool dataset_has_temp_card(uint32_t index, uint32_t rev, uint32_t n_persons,
			   uint32_t mutate_count)
{
	if (rev == 0 || mutate_count == 0 || n_persons == 0) {
		return false;
	}
	for (uint32_t i = 0; i < mutate_count; i++) {
		if (dataset_mutate_index(i, rev, n_persons, mutate_count) == index) {
			return true;
		}
	}
	return false;
}

uint32_t dataset_mutate_index(uint32_t nth, uint32_t rev, uint32_t n_persons,
			      uint32_t mutate_count)
{
	/* Stride the subset across the whole population so consecutive
	 * revisions touch disjoint persons and their writes scatter. A
	 * contiguous run would concentrate a whole round into whatever few
	 * places the store happens to keep neighbouring ids, and measure that
	 * instead of the write path. What those places are is not this
	 * module's business — scattering is correct against any of them. */
	uint32_t stride = (mutate_count && n_persons / mutate_count)
				  ? n_persons / mutate_count
				  : 1u;

	return (rev + nth * stride) % n_persons;
}

uint32_t dataset_sample_index(uint32_t nth, uint32_t count, uint32_t n_persons)
{
	if (count == 0 || n_persons == 0) {
		return 0;
	}
	/* Spread the sample rather than taking a prefix: a prefix would sit in
	 * one shard's low buckets and measure a warmer store than the average. */
	return (uint32_t)(((uint64_t)nth * n_persons) / count) % n_persons;
}

void dataset_person(uint32_t index, uint32_t rev, uint32_t n_persons,
		    struct persondb_person *out)
{
	memset(out, 0, sizeof(*out));

	out->id = dataset_id_of(index);

	snprintf(out->name, sizeof(out->name), "%s %s-%04u",
		 first_names[mix(index, 1) % 16], last_names[mix(index, 2) % 16],
		 (unsigned)(index % 10000u));
	snprintf(out->dept, sizeof(out->dept), "%s",
		 departments[mix(index, 4) % 8]);
	snprintf(out->title, sizeof(out->title), "%s", titles[mix(index, 5) % 8]);

	/* A plausible badge validity window: issued somewhere in a two-year
	 * span from 2026-01-01, valid for three years. */
	out->valid_from = 1767225600u + (mix(index, 6) % (2u * 365u * 86400u));
	out->valid_until = out->valid_from + 3u * 365u * 86400u;

	for (unsigned i = 0; i < PERSONDB_PIN_LEN; i++) {
		out->pin_hash[i] = (uint8_t)(mix(index, 7u + i) >> 13);
	}

	/* 10..22 permissions, mean 16. Drawn without replacement by walking
	 * the vocabulary from a per-person offset with a per-person odd step,
	 * which visits distinct entries because 32 is a power of two. */
	uint8_t n_perms = (uint8_t)(10u + (mix(index, 8) % 13u));
	uint8_t start = (uint8_t)(mix(index, 9) % DATASET_PERM_VOCAB);
	uint8_t step = (uint8_t)((mix(index, 10) % 16u) * 2u + 1u);

	out->n_perms = n_perms;
	for (uint8_t i = 0; i < n_perms; i++) {
		uint8_t v = (uint8_t)((start + (uint16_t)i * step) %
				      DATASET_PERM_VOCAB);

		snprintf(out->perm[i], sizeof(out->perm[i]), "%s", perm_vocab[v]);
	}

	uint8_t n_cards = dataset_n_cards(index);

	out->n_cards = n_cards;
	for (uint8_t i = 0; i < n_cards; i++) {
		dataset_card(index, i, out->card[i]);
	}

	/* The temporary card is what the mutation round moves around; whether
	 * it is present is the one part of a person that depends on rev. */
	if (dataset_has_temp_card(index, rev, n_persons,
				  CONFIG_APP_CBOR_PERSONDB_MUTATE_COUNT)) {
		dataset_card(index, DATASET_TEMP_CARD_SLOT,
			     out->card[out->n_cards]);
		out->n_cards++;
	}
}
