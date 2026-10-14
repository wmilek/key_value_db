/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BLOB_DB_TEST_H_
#define APP_LIB_BLOB_DB_TEST_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_blob_db_test blob_db test hooks
 * @ingroup lib_blob_db
 * @{
 *
 * @brief Fault injection for blob_db's own crash tests
 *        (`CONFIG_BLOB_DB_TEST_CRASH_HOOKS`).
 *
 * **Not part of the blob_db contract** (`<app/lib/blob_db.h>`), and never for
 * a product build: these deliberately leave the store in the state a power
 * cut or a torn compaction would.
 */

#if defined(CONFIG_BLOB_DB_TEST_CRASH_HOOKS)
/**
 * @brief Where to cut a segmented write or delete, for crash tests.
 *
 * Names the steps of the segmented write sequence (proposal §6.4). Setting
 * `blob_db_test_cut` makes the *next* segmented `blob_db_update()` or
 * `blob_db_delete()` stop dead after that step and return `-EINTR`, leaving
 * flash exactly as a power cut at that instant would. The caller then
 * unmounts and remounts to run recovery.
 *
 * The point of interest is `BLOB_DB_CUT_AFTER_COMMIT`: step 3 writes the
 * index record and is the single commit point, so a cut before it must leave
 * the object wholly old, and a cut at or after it wholly new — never torn.
 * Every cut must also leave no unreferenced segment behind once the sweep has
 * run.
 *
 * Test builds only (`CONFIG_BLOB_DB_TEST_CRASH_HOOKS`).
 */
enum blob_db_test_cut {
	BLOB_DB_CUT_NONE = 0,        /**< no cut — normal operation      */
	BLOB_DB_CUT_AFTER_OWNER_SET, /**< after step 1: sweep window open */
	BLOB_DB_CUT_MID_SEGMENTS,    /**< inside step 2: one chunk written */
	BLOB_DB_CUT_AFTER_SEGMENTS,  /**< after step 2: all chunks, no index */
	BLOB_DB_CUT_AFTER_COMMIT,    /**< after step 3: the commit point  */
	BLOB_DB_CUT_AFTER_RELEASE,   /**< after step 4: owner still set   */
};

/**
 * @brief Arm a cut. Cleared automatically when it fires, so it cuts once.
 */
extern enum blob_db_test_cut blob_db_test_cut;

/**
 * @brief Put the store into the state a torn compaction leaves.
 *
 * Reaching that state for real needs an I/O error inside compaction's atomic
 * window, which no test can provoke on a working flash simulator. This sets
 * the same flag so the *consequences* — mutations refused with `-EIO`, reads
 * still served, a remount clearing it — can be tested directly.
 */
void blob_db_test_wedge(void);
#endif /* CONFIG_BLOB_DB_TEST_CRASH_HOOKS */

/** @} */ /* end of lib_blob_db_test */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_BLOB_DB_TEST_H_ */
