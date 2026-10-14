/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Model container list — the executable form of
 * doc/layers/l1_model_container_list.md.
 *
 * A deliberately un-optimized doubly linked list (one i-node per node and
 * per value, no count, O(n) walks) built on NOTHING but the blob_db L1
 * contract. The map (model_container_map.h) has one root that references
 * every i-node, so each mutation is one commit of that root. Here every node
 * is referenced twice — by its predecessor's `next` and its successor's
 * `prev` — so linking or unlinking one node must change two existing
 * i-nodes, and only one of those writes can be the commit.
 *
 * The answer this model teaches: one direction is authoritative and the
 * other is derived. The forward chain (root.head, every node's `next`) is
 * the structure; the backward chain (every `prev`, root.tail) mirrors it.
 * A mutation commits with one write to the forward chain, then REPAIRs the
 * one back link the commit made stale. The intent names that repair next to
 * the ids the mutation creates and retires, so recovery after the commit
 * re-applies it (roll forward), and before the commit has nothing to undo
 * but the new ids (roll back).
 *
 * Node ids are the handles: a client holds a node id as a stable cursor
 * (ids never change and are never reissued) and passes it back to insert
 * next to it, remove it, or read and overwrite its value.
 *
 * As for the map, the CLIENT owns the root id's persistence (typically via
 * rootreg_get_or_create()) and passes it to mcl_open().
 */

#ifndef MODEL_CONTAINER_LIST_H_
#define MODEL_CONTAINER_LIST_H_

#include <stddef.h>
#include <stdint.h>

/* Crash-injection points (l1_model_container_list.md §4). A mutation runs up
 * to and INCLUDING the named step, then returns MCL_STOPPED without doing
 * the rest; the caller remounts and runs mcl_open() to recover from there.
 * MCL_STEP_NONE runs the mutation to completion. */
enum mcl_crash_point {
	MCL_STEP_NONE = 0,   /* run to completion */
	MCL_STEP_STAGE,      /* stop after step 1: intent staged             */
	MCL_STEP_PREPARE,    /* stop after step 2: new blobs bound            */
	MCL_STEP_COMMIT,     /* stop after step 3: forward link swapped       */
	MCL_STEP_REPAIR,     /* stop after step 4: back link brought in line  */
	MCL_STEP_CLEANUP,    /* stop after step 5: unlinked blobs deleted     */
	/* step 6 (CLEAR) completes the mutation — that's MCL_STEP_NONE. */
};

/* Signals a mutation stopped early at its injected crash point. */
#define MCL_STOPPED  1

/* Open the list rooted at `root_id`, creating an empty one if the id is
 * allocated-but-unbound, and running intent recovery (§5) if a mutation was
 * interrupted. Idempotent — safe to call on every mount. */
int mcl_open(uint64_t root_id);

/* Insert a new node holding `val` right after node `at`, or at the front if
 * `at` is 0. Stores the new node's id in `*out_id` (may be NULL), valid when
 * 0 is returned. Returns 0, MCL_STOPPED at the injected crash point, -ENOENT if `at`
 * is not a node of this list, or a negative errno.
 *
 * A step that fails without a power cut aborts through the same recovery a
 * remount runs: before the commit the insert is rolled back and the error
 * returned; after it the insert is finished and 0 returned. */
int mcl_insert_after(uint64_t at, const void *val, size_t vlen,
		     uint64_t *out_id, enum mcl_crash_point crash_at);

/* As mcl_insert_after, but right before node `at`, or at the back if `at`
 * is 0. */
int mcl_insert_before(uint64_t at, const void *val, size_t vlen,
		      uint64_t *out_id, enum mcl_crash_point crash_at);

/* Unlink node `id` and delete it with its value. Crash points and failures
 * as for the inserts. */
int mcl_remove(uint64_t id, enum mcl_crash_point crash_at);

/* Overwrite the value of node `id` in place: one atomic update of the value
 * blob, the basic flow of l1_model_container.md §3.4. No intent, no crash
 * point — there is no window to inject into. */
int mcl_set(uint64_t id, const void *val, size_t vlen);

/* Read the value of node `id`. Reads trust the handle: unlike a write,
 * mcl_get and mcl_next do not check that `id` is linked into this list. */
int mcl_get(uint64_t id, void *out, size_t out_sz, size_t *out_len);

/* Walk the list. mcl_next(0) is the head, mcl_prev(0) the tail; 0 comes
 * back past either end. mcl_next follows the authoritative chain and never
 * writes. mcl_prev follows the derived chain, so it first finishes a
 * mutation an earlier failed abort left staged (one read when none is). */
int mcl_next(uint64_t id, uint64_t *out);
int mcl_prev(uint64_t id, uint64_t *out);

/* Test hook: make the next mutation fail cleanly with `err` right after
 * `step`, as if the following blob_db call had returned it, so it takes the
 * abort path with no remount. One-shot; MCL_STEP_NONE disarms. */
void mcl_fail_after(enum mcl_crash_point step, int err);

/* Tear the list down: every node and value blob, the intent, and the root.
 * Afterwards every mcl_* call fails with -ENODEV until a fresh mcl_open().
 * The client then drops its record of the root id (teardown before
 * unregistration, l1_root_registry.md §8).
 *
 * Not crash-atomic: it deletes from the tail backwards, so a crash leaves a
 * prefix of the forward chain still linked from the root, and a rerun after
 * mcl_open() finishes the job (deletes tolerate -ENOENT). */
int mcl_destroy(void);

/* Diagnostics for tests: walk the forward chain, check that every back link
 * mirrors it (each node's `prev` is the node before it, root.tail is the
 * last), and report the node count. -EIO on any mismatch. */
int mcl_check(size_t *count);

#endif /* MODEL_CONTAINER_LIST_H_ */
