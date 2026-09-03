/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * YCSB key generators.
 *
 * These reproduce the generators of the YCSB core workloads (Cooper et al.,
 * "Benchmarking Cloud Serving Systems with YCSB", SoCC 2010) closely enough
 * that a number produced here is comparable with one produced by the Java
 * client: the Zipfian is Gray et al.'s method with the same constants, and
 * the scramble is the same FNV-1a fold over the drawn rank.
 *
 * They are kept in their own translation unit for one reason: the benchmark's
 * credibility rests on the key distribution being right, and a generator that
 * lives behind a seam can be checked on its own (generator_selftest(), run
 * before any flash is touched).
 *
 * Everything is seeded and deterministic. Two runs of the same build issue the
 * same key sequence, which is what makes the flash-I/O counters in RESULTS.md
 * a regression guard rather than a sample.
 */

#ifndef APP_PERF_YCSB_GENERATOR_H_
#define APP_PERF_YCSB_GENERATOR_H_

#include <stdbool.h>
#include <stdint.h>

/** xorshift64* — small, fast, and reproducible across compilers. */
struct ycsb_rng {
	uint64_t s;
};

void ycsb_rng_seed(struct ycsb_rng *r, uint64_t seed);
uint64_t ycsb_rng_next(struct ycsb_rng *r);

/** Uniform in [0, 1). */
double ycsb_rng_double(struct ycsb_rng *r);

/** Uniform in [0, n). Returns 0 for n == 0. */
uint64_t ycsb_rng_below(struct ycsb_rng *r, uint64_t n);

/**
 * Zipfian over [0, n), Gray et al. (1994) as used by YCSB's
 * ZipfianGenerator. @p theta is YCSB's zipfianconstant (0.99).
 *
 * With @p scramble set this is YCSB's ScrambledZipfianGenerator: the drawn
 * rank is folded through FNV-1a and taken mod n, so the hot items are spread
 * over the key space instead of clustering at low key numbers. That matters
 * here — kvhash buckets by fnv1a(key), so an unscrambled draw would
 * concentrate the whole workload in a handful of buckets and measure the
 * wrong thing.
 */
struct ycsb_zipfian {
	uint64_t n;
	double theta;
	double zetan;
	double alpha;
	double eta;
	double half_pow_theta; /* pow(0.5, theta), hoisted out of the draw */
	bool scramble;
};

void ycsb_zipfian_init(struct ycsb_zipfian *z, uint64_t n, double theta, bool scramble);
uint64_t ycsb_zipfian_next(const struct ycsb_zipfian *z, struct ycsb_rng *r);

/**
 * Generalized harmonic number sum_{i=1..n} i^-theta — YCSB's zeta().
 * Exposed because the self-test predicts the head's share of the draws from
 * it, rather than hard-coding a number that would silently stop meaning
 * anything if recordcount or theta changed.
 */
double ycsb_zeta(uint64_t n, double theta);

/**
 * Workload D's key chooser: SkewedLatestGenerator. Draws a Zipfian rank and
 * counts *back* from the newest key, so recently inserted records are hot.
 *
 * @p z must be initialized unscrambled over at least @p max_key + 1 items;
 * the draw is rejected and retried while it exceeds @p max_key, so the shape
 * stays right as the key space grows during the run.
 */
uint64_t ycsb_latest_next(const struct ycsb_zipfian *z, struct ycsb_rng *r,
			  uint64_t max_key);

#endif /* APP_PERF_YCSB_GENERATOR_H_ */
