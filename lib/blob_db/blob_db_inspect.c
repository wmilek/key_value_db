/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db inspection — geometry and per-bucket occupancy of the bucket-log
 * implementation (<app/lib/blob_db_inspect.h>, CONFIG_BLOB_DB_INSPECT).
 *
 * Kept out of blob_db.c so the core and <app/lib/blob_db.h> carry only the
 * contract. Reaches the core through the hooks in blob_db_internal.h and reads
 * flash through the store seam; never writes.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <app/lib/blob_db_inspect.h>

#include "blob_db_internal.h"
#include "blob_db_store.h"

int blob_db_inspect_info_get(struct blob_db_inspect_info *out)
{
	struct blob_db_core_state st;

	if (!out) {
		return -EINVAL;
	}
	blob_db_core_state_get(&st);
	if (!st.mounted) {
		return -ENODEV;
	}

	*out = (struct blob_db_inspect_info){
		.partition_size  = st.fa_size,
		.sector_size     = st.peb_size,
		.write_align     = st.write_align,
		.n_sectors       = st.n_pebs,
		.n_buckets       = st.n_buckets,
		.bucket_capacity = st.peb_size - BLOB_DB_BUCKET_DATA_OFF,
		.max_payload_len = CONFIG_BLOB_DB_MAX_PAYLOAD_LEN,
		.slot_overhead   = BLOB_DB_SLOT_OVERHEAD,
		.format_major    = BLOB_DB_FORMAT_MAJOR,
		.format_minor    = BLOB_DB_FORMAT_MINOR,
		.active_master   = st.active_master,
		.master_gen      = st.master_gen,
		.next_id         = st.next_id,
		.next_id_hint    = st.next_id_hint,
		.seg_owner       = st.seg_owner,
		.wedged          = st.wedged,
	};
	return 0;
}

int blob_db_inspect_bucket_get(uint16_t bid,
			       struct blob_db_inspect_bucket *out,
			       blob_db_inspect_slot_cb_t cb, void *user)
{
	struct blob_db_core_state st;

	if (!out) {
		return -EINVAL;
	}
	blob_db_core_state_get(&st);
	if (!st.mounted) {
		return -ENODEV;
	}
	if (bid >= st.n_buckets) {
		return -EINVAL;
	}

	const uint32_t capacity = st.peb_size - BLOB_DB_BUCKET_DATA_OFF;
	uint8_t *const buf = blob_db_core_sector_buf();

	*out = (struct blob_db_inspect_bucket){
		.capacity   = capacity,
		.free_bytes = capacity,
	};

	int rc = blob_db_store_read(
		(off_t)(BLOB_DB_FIRST_BUCKET + bid) * (off_t)st.peb_size, buf,
		st.peb_size);

	if (rc < 0) {
		return rc;
	}
	if (!blob_db_core_bucket_hdr_valid(buf, bid)) {
		/* Never used, or invalidated by erase_all: the next write
		 * formats it, so all of it is available. */
		return 0;
	}

	out->formatted = true;
	out->gen = ((const struct blob_db_bucket_hdr *)buf)->gen;

	/* Same liveness rule as compaction: a slot is kept iff it is the newest
	 * for its id and not a tombstone. */
	off_t cursor = BLOB_DB_BUCKET_DATA_OFF;

	for (;;) {
		struct blob_db_core_slot sv;

		if (!blob_db_core_slot_at(buf, cursor, &sv)) {
			break;
		}

		bool superseded = false;
		off_t scan = cursor + sv.total_size;

		for (;;) {
			struct blob_db_core_slot future;

			if (!blob_db_core_slot_at(buf, scan, &future)) {
				break;
			}
			if (future.id == sv.id) {
				superseded = true;
				break;
			}
			scan += future.total_size;
		}

		struct blob_db_inspect_slot si = {
			.offset  = (uint32_t)cursor,
			.size    = (uint32_t)sv.total_size,
			.id      = sv.id,
			.val_len = sv.val_len,
			.flags   = sv.flags,
			.segment = IS_ENABLED(CONFIG_BLOB_DB_LARGE_PAYLOADS) &&
				   (sv.flags & BLOB_DB_SLOT_F_SEGMENT),
			.index   = IS_ENABLED(CONFIG_BLOB_DB_LARGE_PAYLOADS) &&
				   (sv.flags & BLOB_DB_SLOT_F_INDEXED),
		};

		if (sv.flags & BLOB_DB_SLOT_F_TOMBSTONE) {
			si.state = BLOB_DB_INSPECT_TOMBSTONE;
			out->tombstones++;
			out->garbage_bytes += si.size;
		} else if (superseded) {
			si.state = BLOB_DB_INSPECT_SUPERSEDED;
			out->superseded++;
			out->garbage_bytes += si.size;
		} else {
			si.state = BLOB_DB_INSPECT_LIVE;
			if (si.segment) {
				out->segments++;
			} else {
				out->objects++;
			}
			out->live_bytes += si.size;
			out->payload_bytes += si.val_len;
		}

		if (cb) {
			rc = cb(&si, user);
			if (rc) {
				return rc;
			}
		}

		cursor += sv.total_size;
	}

	/* Anything programmed past the readable log — a torn append, or a slot
	 * whose CRC failed and everything behind it — is neither live nor free:
	 * it is reclaimed only by the next compaction. */
	off_t end = (off_t)st.peb_size;

	while (end > cursor && buf[end - 1] == 0xff) {
		end--;
	}

	out->tail_bytes = (uint32_t)(end - cursor);
	out->free_bytes = capacity - out->live_bytes - out->garbage_bytes -
			  out->tail_bytes;
	return 0;
}
