/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_CONTAINERS_KVHASH_H_
#define APP_LIB_CONTAINERS_KVHASH_H_

#include <stdbool.h>

#include <app/lib/containers/shape_map.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief kvhash's Map provider — O(1) hash-bucket container.
 *
 * Bind this into an L3 interface via its @ref map_ops. See kvhash.c for the
 * on-flash layout (directory blob of bucket ids + one packed blob per bucket).
 */
extern const struct map_ops kvhash_map_ops;

/**
 * @brief Set the spill thresholds for writes from now on.
 *
 * A key longer than @p key_max, or a value longer than @p val_max, is stored
 * in a blob of its own instead of inside its bucket. The thresholds are a
 * write policy only: every entry records its own placement and every read goes
 * by that, so changing them never invalidates stored data. Existing entries
 * keep their placement until their key is next set. Maps created before
 * kvhash v3 never spill, whatever the thresholds.
 *
 * Initialised from CONFIG_BLOB_CONTAINER_KVHASH_KEY_INLINE_MAX and
 * CONFIG_BLOB_CONTAINER_KVHASH_VAL_INLINE_MAX. Values above 32767 are
 * clamped to 32767 (never spill).
 */
void kvhash_set_inline_max(size_t key_max, size_t val_max);

#if defined(CONFIG_BLOB_CONTAINER_KVHASH_TEST_HOOKS)
/**
 * @brief AND-mask applied to every key fingerprint (default all ones).
 *
 * Test builds only: 0 makes every fingerprint equal, so lookups and the
 * enumeration order must fall back to comparing key bytes.
 */
extern uint32_t kvhash_test_fp_mask;

/**
 * @brief Cut power after this many flash writes (default -1: never).
 *
 * Test builds only. Counts every blob_db update and delete kvhash issues;
 * when it reaches 0, that write and every later one fail with -EINTR and do
 * nothing, as if power had gone. blob_db writes are atomic, so cutting
 * between writes reaches every state a real power loss can leave. A test then
 * sets it back to -1 and remounts.
 */
extern int kvhash_test_cut_after;

/**
 * @brief Set when a cut refused a write; the test clears it.
 *
 * The operation's return value cannot say this: a release after the commit
 * point swallows its error, so a cut there still returns 0.
 */
extern bool kvhash_test_cut_fired;
#endif

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_CONTAINERS_KVHASH_H_ */
