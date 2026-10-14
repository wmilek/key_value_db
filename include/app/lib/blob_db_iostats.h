/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BLOB_DB_IOSTATS_H_
#define APP_LIB_BLOB_DB_IOSTATS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_blob_db_iostats blob_db flash I/O counters
 * @ingroup lib_blob_db
 * @{
 *
 * @brief Count the flash I/O blob_db performs, for benchmarks and regression
 *        guards (`CONFIG_BLOB_DB_IOSTATS`).
 *
 * **Not part of the blob_db contract** (`<app/lib/blob_db.h>`): it measures
 * an implementation, and another one need not count — or cost — the same.
 */

#if defined(CONFIG_BLOB_DB_IOSTATS)
/**
 * @brief Flash I/O actually performed, counted at the storage seam.
 *
 * Operation counts and byte counts are both kept, because they answer
 * different questions and can point opposite ways: a change that reads fewer
 * bytes may issue more transactions, and on a serial part each transaction
 * carries a fixed command-and-address cost. Judging such a change on bytes
 * alone flatters it; judging on operations alone condemns it.
 *
 * Counted below both backends, so `flash_area` and UBI are directly
 * comparable for the same workload. Deterministic — the same workload yields
 * the same numbers — which makes these usable as regression guards where
 * wall-clock is not.
 */
struct blob_db_iostats {
	uint32_t reads;          /**< read operations issued          */
	uint32_t writes;         /**< write operations issued         */
	uint32_t erases;         /**< erase operations issued         */
	uint64_t bytes_read;     /**< bytes requested from the store  */
	uint64_t bytes_written;  /**< bytes handed to the store       */
	uint64_t bytes_erased;   /**< bytes covered by erase requests */

	/* Why the erases above happened. The flash counters say what was done,
	 * not on whose behalf — and on NOR it is the erases that dominate, so a
	 * phase that erased more than expected needs these to say why. Their
	 * cost is already included in the counters above, never in addition to
	 * them. Counted above the storage seam, so both backends report the same
	 * events; both also charge each master write and bucket format as one
	 * erase of a whole block plus one write. */
	uint32_t master_writes;  /**< master records written, one block erase
				  *   each — including the two every
				  *   non-atomic compaction writes */
	uint32_t bucket_formats; /**< bucket headers written to a fresh block:
				  *   prepare(), a bucket's first use, and the
				  *   root bucket in format/erase_all */
	uint32_t compactions;    /**< buckets compacted (each erases the bucket
				  *   and, unless the backend replaces
				  *   atomically, the scratch block twice) */
	uint64_t bytes_compacted;/**< live bytes those compactions rewrote —
				  *   the write amplification garbage costs */
};

/** @brief Snapshot the counters. */
void blob_db_iostats_get(struct blob_db_iostats *out);

/** @brief Zero the counters, to measure one operation or phase in isolation. */
void blob_db_iostats_reset(void);
#endif /* CONFIG_BLOB_DB_IOSTATS */

/** @} */ /* end of lib_blob_db_iostats */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_BLOB_DB_IOSTATS_H_ */
