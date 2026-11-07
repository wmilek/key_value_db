/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db slot index — see blob_db_slot_index.h for what it promises and
 * doc/impl/l1_bucketlog.md §5.4a for why it exists.
 *
 * An open-addressed hash table (linear probing, backward-shift delete) from id
 * to slot offset, and one append cursor per bucket. Fixed size, in .bss: the
 * RAM is set by Kconfig and does not grow with the store. When the table fills,
 * further ids are simply not indexed and their lookups walk.
 */

#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "blob_db_slot_index.h"

LOG_MODULE_DECLARE(blob_db, CONFIG_BLOB_DB_LOG_LEVEL);

#define CAP        CONFIG_BLOB_DB_SLOT_INDEX_ENTRIES
#define MAX_FILL   (CAP - CAP / 8)   /* keep probe runs short */
#define MAX_BKT    CONFIG_BLOB_DB_SLOT_INDEX_MAX_BUCKETS

BUILD_ASSERT((CAP & (CAP - 1)) == 0,
	     "CONFIG_BLOB_DB_SLOT_INDEX_ENTRIES must be a power of two");

/* id 0 is never a blob (alloc_id never returns it), so it marks a free cell. */
static uint64_t g_ids[CAP];
static uint32_t g_offs[CAP];
static uint32_t g_used;

#if defined(CONFIG_BLOB_DB_SLOT_INDEX_CLOCK)
/*
 * CLOCK replacement. The table is a cache of the ids that are looked up:
 * an entry is created when a walk finds an id (learn), and an append or a
 * compaction only refreshes an entry already present. When the table is full,
 * a hand sweeps the cells: a cell whose reference bit is set (looked up since
 * the hand last passed) has it cleared and is skipped, the first one without
 * is evicted. Hot ids are touched more often than the hand comes round, so
 * they stay; ids written and never read never get in.
 *
 * Evicting is always safe: the index is a hint, and an id with no entry is
 * walked, exactly as on a miss. The bits travel with their cells when a
 * deletion shifts a probe run back (remove_at).
 */
static uint8_t g_ref[CAP / 8];
static uint8_t g_seen[CAP / 8];  /* compaction refresh: survivors seen */
static uint32_t g_hand;
#define BIT_GET(a, i)  (((a)[(i) >> 3] >> ((i) & 7)) & 1u)
#define BIT_SET(a, i)  ((a)[(i) >> 3] |= (uint8_t)(1u << ((i) & 7)))
#define BIT_CLR(a, i)  ((a)[(i) >> 3] &= (uint8_t)~(1u << ((i) & 7)))
#define BIT_PUT(a, i, v) do { if (v) { BIT_SET(a, i); } else { BIT_CLR(a, i); } } while (0)
#endif

/* 0 = unknown. A real cursor is at least the bucket header's length. */
static uint32_t g_cursor[MAX_BKT];
/* Entries per bucket, so forgetting a bucket with none costs nothing -- mount
 * begins every bucket, and must not sweep the table for each one. */
static uint16_t g_bcnt[MAX_BKT];
static uint16_t g_n_buckets;   /* 0 = index disabled */

static inline uint32_t home(uint64_t id)
{
	/* Fibonacci hashing: ids are sequential, and consecutive ids must not
	 * land in consecutive cells or every probe run merges into one. */
	return (uint32_t)((id * 0x9e3779b97f4a7c15ull) >> 32) & (CAP - 1);
}

static inline uint16_t bucket_of(uint64_t id)
{
	return (uint16_t)(id % g_n_buckets);
}

static int32_t find(uint64_t id)
{
	for (uint32_t i = home(id), n = 0; n < CAP; i = (i + 1) & (CAP - 1), n++) {
		if (g_ids[i] == id) {
			return (int32_t)i;
		}
		if (g_ids[i] == 0) {
			return -1;
		}
	}
	return -1;
}

/* Remove cell @p i and close the gap, so no probe run is broken. */
static void remove_at(uint32_t i)
{
	uint32_t j = i;

	g_bcnt[bucket_of(g_ids[i])]--;
	g_ids[i] = 0;
	g_used--;
	for (;;) {
		j = (j + 1) & (CAP - 1);
		if (g_ids[j] == 0) {
			return;
		}
		uint32_t h = home(g_ids[j]);

		/* Move j back into the hole at i unless its home lies in the
		 * cyclic range (i, j] -- then i is not on its probe path. */
		bool stays = (i <= j) ? (i < h && h <= j) : (i < h || h <= j);

		if (!stays) {
			g_ids[i] = g_ids[j];
			g_offs[i] = g_offs[j];
#if defined(CONFIG_BLOB_DB_SLOT_INDEX_CLOCK)
			BIT_PUT(g_ref, i, BIT_GET(g_ref, j));
			BIT_PUT(g_seen, i, BIT_GET(g_seen, j));
#endif
			g_ids[j] = 0;
			i = j;
		}
	}
}

#if defined(CONFIG_BLOB_DB_SLOT_INDEX_CLOCK)
/* Free one cell: sweep from the hand, giving each referenced cell a second
 * chance. Ends within two turns: every cell passed has its bit cleared. */
static void evict_one(void)
{
	for (;;) {
		uint32_t i = g_hand;

		if (g_ids[i] != 0) {
			if (!BIT_GET(g_ref, i)) {
				/* remove_at may pull a later cell into i; the hand
				 * stays, so that cell is the next one examined. */
				remove_at(i);
				return;
			}
			BIT_CLR(g_ref, i);
		}
		g_hand = (g_hand + 1) & (CAP - 1);
	}
}

/* Refresh @p id's entry if the table holds one; never create one. */
static bool refresh(uint64_t id, uint32_t off)
{
	int32_t i = find(id);

	if (i < 0) {
		return false;
	}
	g_offs[i] = off;
	BIT_SET(g_seen, (uint32_t)i);
	return true;
}
#endif

static void put(uint64_t id, uint32_t off)
{
	uint32_t i = home(id);

	for (uint32_t n = 0; n < CAP; i = (i + 1) & (CAP - 1), n++) {
		if (g_ids[i] == id) {
			g_offs[i] = off;
			return;
		}
		if (g_ids[i] == 0) {
			break;
		}
	}
	if (g_used >= MAX_FILL) {
#if defined(CONFIG_BLOB_DB_SLOT_INDEX_CLOCK)
		evict_one();
		put(id, off);   /* the eviction may have moved this id's cell */
		return;
#else
		return;   /* full: this id is looked up by walking */
#endif
	}
	g_ids[i] = id;
	g_offs[i] = off;
	g_used++;
	g_bcnt[bucket_of(id)]++;
#if defined(CONFIG_BLOB_DB_SLOT_INDEX_CLOCK)
	/* One pass of protection: a new entry may land just ahead of the
	 * hand, and should get one more lookup before it can go. */
	BIT_SET(g_ref, i);
	BIT_SET(g_seen, i);
#endif
}

static void drop(uint64_t id)
{
	int32_t i = find(id);

	if (i >= 0) {
		remove_at((uint32_t)i);
	}
}

static inline bool on(uint16_t bid)
{
	return g_n_buckets != 0 && bid < g_n_buckets;
}

void blob_db_sidx_reset(uint16_t n_buckets)
{
	memset(g_ids, 0, sizeof(g_ids));
#if defined(CONFIG_BLOB_DB_SLOT_INDEX_CLOCK)
	memset(g_ref, 0, sizeof(g_ref));
	memset(g_seen, 0, sizeof(g_seen));
	g_hand = 0;
#endif
	memset(g_cursor, 0, sizeof(g_cursor));
	memset(g_bcnt, 0, sizeof(g_bcnt));
	g_used = 0;
	g_n_buckets = 0;

	if (n_buckets > MAX_BKT) {
		LOG_WRN("slot index off: %u buckets, room for %u "
			"(CONFIG_BLOB_DB_SLOT_INDEX_MAX_BUCKETS)",
			n_buckets, MAX_BKT);
		return;
	}
	g_n_buckets = n_buckets;
}

void blob_db_sidx_disable(void)
{
	blob_db_sidx_reset(0);
}

void blob_db_sidx_forget(uint16_t bid)
{
	if (!on(bid)) {
		return;
	}
	g_cursor[bid] = 0;

	/* A removal can pull a later cell back into slot i, so i is re-read
	 * rather than advanced. Cells that wrap past the end are reached
	 * first, at the low indices, before anything moves them. */
	for (uint32_t i = 0; i < CAP && g_bcnt[bid] != 0;) {
		if (g_ids[i] != 0 && bucket_of(g_ids[i]) == bid) {
			remove_at(i);
		} else {
			i++;
		}
	}
}

void blob_db_sidx_begin(uint16_t bid, uint32_t data_off)
{
	if (!on(bid)) {
		return;
	}
	blob_db_sidx_forget(bid);
	g_cursor[bid] = data_off;
}

void blob_db_sidx_note(uint16_t bid, uint64_t id, uint32_t off,
		       uint32_t size, bool tombstone)
{
	if (!on(bid) || g_cursor[bid] == 0) {
		return;
	}
	if (off != g_cursor[bid]) {
		/* Not where the log ended: whatever happened, the index no
		 * longer knows this bucket. Let the walk find out. */
		blob_db_sidx_forget(bid);
		return;
	}
	g_cursor[bid] = off + size;
	if (tombstone) {
		drop(id);
	} else {
#if defined(CONFIG_BLOB_DB_SLOT_INDEX_CLOCK)
		(void)refresh(id, off);   /* an append does not earn a cell */
#else
		put(id, off);
#endif
	}
}

void blob_db_sidx_rebuild_begin(uint16_t bid, uint32_t data_off)
{
#if defined(CONFIG_BLOB_DB_SLOT_INDEX_CLOCK)
	if (!on(bid)) {
		return;
	}
	/* Keep the bucket's entries: the notes that follow refresh the ones
	 * whose ids survived, and rebuild_end drops the rest. Nothing reads
	 * the index in between. */
	for (uint32_t i = 0; i < CAP; i++) {
		if (g_ids[i] != 0 && bucket_of(g_ids[i]) == bid) {
			BIT_CLR(g_seen, i);
		}
	}
	g_cursor[bid] = data_off;
#else
	blob_db_sidx_begin(bid, data_off);
#endif
}

void blob_db_sidx_rebuild_end(uint16_t bid)
{
#if defined(CONFIG_BLOB_DB_SLOT_INDEX_CLOCK)
	if (!on(bid) || g_cursor[bid] == 0) {
		return;
	}
	for (uint32_t i = 0; i < CAP && g_bcnt[bid] != 0;) {
		if (g_ids[i] != 0 && bucket_of(g_ids[i]) == bid &&
		    !BIT_GET(g_seen, i)) {
			remove_at(i);   /* its id did not survive the compaction */
		} else {
			i++;
		}
	}
#else
	ARG_UNUSED(bid);
#endif
}

void blob_db_sidx_learn(uint16_t bid, uint64_t id, int64_t off,
			uint32_t cursor)
{
	if (!on(bid) || cursor == 0) {
		return;
	}
	if (g_cursor[bid] == 0) {
		/* Unknown implies no entries, so the walk's cursor is all
		 * there is to know about this bucket right now. */
		g_cursor[bid] = cursor;
	} else if (g_cursor[bid] != cursor) {
		blob_db_sidx_forget(bid);
		return;
	}
	if (off >= 0) {
		put(id, (uint32_t)off);
	}
}

bool blob_db_sidx_lookup(uint16_t bid, uint64_t id, uint32_t *off,
			 uint32_t *cursor)
{
	if (!on(bid) || g_cursor[bid] == 0) {
		return false;
	}

	int32_t i = find(id);

	if (i < 0) {
		return false;
	}
	*off = g_offs[i];
	*cursor = g_cursor[bid];
#if defined(CONFIG_BLOB_DB_SLOT_INDEX_CLOCK)
	BIT_SET(g_ref, (uint32_t)i);
#endif
	return true;
}

bool blob_db_sidx_cursor(uint16_t bid, uint32_t *cursor)
{
	if (!on(bid) || g_cursor[bid] == 0) {
		return false;
	}
	*cursor = g_cursor[bid];
	return true;
}

void blob_db_sidx_usage(uint32_t *used, uint32_t *capacity)
{
	*used = g_used;
	*capacity = MAX_FILL;
}
