/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Model container — the executable form of doc/layers/l1_model_container.md.
 *
 * A deliberately un-optimized key->value map (O(n) lookup, one i-node per key
 * and per value) built on NOTHING but the blob_db L1 contract. Its job is not
 * speed — it is a *sufficiency proof*: if this survives every crash point with
 * correct visible state AND zero permanent leak, the L1 contract is enough to
 * carry every layer above it.
 *
 * The whole structure is reachable from the single integer id = 1 (P5): the
 * client (this test) persists nothing else.
 */

#ifndef MODEL_CONTAINER_H_
#define MODEL_CONTAINER_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Root convention: the list root lives at id = 1 (this build has no root
 * registry, so the model binds id = 1 directly — l1_model_container.md §2). */
#define MC_ROOT_ID  1u

/* Crash-injection points for the extended mutation discipline
 * (l1_model_container.md §5). A mutation runs up to and INCLUDING the named
 * step, then returns MC_STOPPED without doing the rest — the caller then
 * unmounts/remounts and runs mc_open() to exercise recovery from that exact
 * point. MC_STEP_NONE runs the mutation to completion. */
enum mc_crash_point {
	MC_STEP_NONE = 0,   /* run to completion */
	MC_STEP_STAGE,      /* stop after step 1: intent staged           */
	MC_STEP_PREPARE,    /* stop after step 2: new blobs bound          */
	MC_STEP_COMMIT,     /* stop after step 3: list image swapped       */
	MC_STEP_CLEANUP,    /* stop after step 4: superseded blobs deleted */
	/* step 5 (CLEAR) completes the mutation — that's MC_STEP_NONE. */
};

/* Signals a mutation stopped early at its injected crash point. */
#define MC_STOPPED  1

/* Open (and, on a virgin store, create) the container rooted at id = 1,
 * running intent recovery (§6) if a mutation was interrupted. Idempotent —
 * safe to call on every mount. */
int mc_open(void);

/* Look up a key. Returns 0 and fills out/out_len on hit, -ENOENT on miss. */
int mc_get(const char *key, void *out, size_t out_sz, size_t *out_len);

/* Insert or overwrite key -> value. `crash_at` injects a stop point (§5);
 * pass MC_STEP_NONE for a normal completed mutation. Returns 0 on completion,
 * MC_STOPPED if it stopped at the injected point, or a negative errno. */
int mc_set(const char *key, const void *val, size_t vlen,
	   enum mc_crash_point crash_at);

/* Remove a key (and its blobs). `crash_at` as in mc_set. */
int mc_del(const char *key, enum mc_crash_point crash_at);

/* Diagnostics for tests: number of entries in the list, and the total live
 * blob count in the whole store (root + intent + all key/value blobs). The
 * latter is how the tests prove "no permanent leak". */
int mc_entry_count(size_t *out);

#endif /* MODEL_CONTAINER_H_ */
