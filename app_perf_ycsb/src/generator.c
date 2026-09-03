/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>

#include "generator.h"

#define FNV64_OFFSET_BASIS 0xcbf29ce484222325ULL
#define FNV64_PRIME        0x100000001b3ULL

/* YCSB's Utils.fnvhash64: FNV-1a folded over the eight bytes of the value,
 * sign bit cleared (the Java original takes Math.abs of a signed long). */
static uint64_t fnvhash64(uint64_t val)
{
	uint64_t h = FNV64_OFFSET_BASIS;

	for (int i = 0; i < 8; i++) {
		h ^= (val & 0xffu);
		h *= FNV64_PRIME;
		val >>= 8;
	}
	return h & 0x7fffffffffffffffULL;
}

void ycsb_rng_seed(struct ycsb_rng *r, uint64_t seed)
{
	/* xorshift64* degenerates to zero from a zero state. */
	r->s = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

uint64_t ycsb_rng_next(struct ycsb_rng *r)
{
	uint64_t x = r->s;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	r->s = x;
	return x * 0x2545f4914f6cdd1dULL;
}

double ycsb_rng_double(struct ycsb_rng *r)
{
	/* Top 53 bits: exactly the mantissa a double can hold, so the result
	 * is uniform over the representable values rather than clumped. */
	return (double)(ycsb_rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

uint64_t ycsb_rng_below(struct ycsb_rng *r, uint64_t n)
{
	return n ? ycsb_rng_next(r) % n : 0;
}

double ycsb_zeta(uint64_t n, double theta)
{
	double sum = 0.0;

	for (uint64_t i = 1; i <= n; i++) {
		sum += 1.0 / pow((double)i, theta);
	}
	return sum;
}

void ycsb_zipfian_init(struct ycsb_zipfian *z, uint64_t n, double theta, bool scramble)
{
	const double zeta2 = ycsb_zeta(2, theta);

	z->n = n;
	z->theta = theta;
	z->zetan = ycsb_zeta(n, theta);
	z->alpha = 1.0 / (1.0 - theta);
	z->eta = (1.0 - pow(2.0 / (double)n, 1.0 - theta)) / (1.0 - zeta2 / z->zetan);
	z->half_pow_theta = pow(0.5, theta);
	z->scramble = scramble;
}

uint64_t ycsb_zipfian_next(const struct ycsb_zipfian *z, struct ycsb_rng *r)
{
	const double u = ycsb_rng_double(r);
	const double uz = u * z->zetan;
	uint64_t ret;

	if (uz < 1.0) {
		ret = 0;
	} else if (uz < 1.0 + z->half_pow_theta) {
		ret = 1;
	} else {
		ret = (uint64_t)((double)z->n * pow(z->eta * u - z->eta + 1.0, z->alpha));
		if (ret >= z->n) {
			ret = z->n - 1; /* guard the tail against rounding */
		}
	}

	/* The scramble is where this differs from a textbook Zipfian, and it
	 * is deliberate: without it the hot ranks are the low key numbers,
	 * kvhash buckets them by fnv1a(key) into a handful of buckets, and the
	 * benchmark measures bucket contention instead of the workload. */
	return z->scramble ? fnvhash64(ret) % z->n : ret;
}

uint64_t ycsb_latest_next(const struct ycsb_zipfian *z, struct ycsb_rng *r,
			  uint64_t max_key)
{
	/* Rejection rather than modulo: the key space grows during workload D
	 * (5 % inserts), and folding an out-of-range rank back with a modulo
	 * would fold tail mass onto the head and make the skew look stronger
	 * than the spec's. Zipfian draws sit near the head, so a rejection is
	 * rare; the bounded fallback only exists so this cannot spin. */
	for (int tries = 0; tries < 64; tries++) {
		uint64_t rank = ycsb_zipfian_next(z, r);

		if (rank <= max_key) {
			return max_key - rank;
		}
	}
	return max_key - (ycsb_zipfian_next(z, r) % (max_key + 1));
}
