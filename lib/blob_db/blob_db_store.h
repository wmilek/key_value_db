/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db storage backend seam.
 *
 * blob_db addresses flash as a uniform array of PEBs (erase blocks), each
 * peb_size bytes, referenced by a flat byte offset (peb_index * peb_size +
 * within). This header abstracts that access so the same blob_db algorithm
 * can run on two substrates, selected at build time:
 *
 *   - UBI (default) — a dynamic UBI volume; each PEB maps 1:1 to a LEB and
 *                     blob_db's in-place slot appends map directly onto
 *                     ubi_leb_write_at().
 *   - flash_area    — raw partition via Zephyr's flash_area API. No wear
 *                     leveling and no bad-block handling; faster.
 *
 * The layouts are NOT interchangeable — see doc/impl/l0_backends.md §4 for
 * what happens when a build meets the other one.
 *
 * A byte offset passed to read/write/erase never crosses a PEB boundary
 * (blob_db operates one bucket/master/scratch sector at a time), so a backend
 * may translate off -> (peb = off / peb_size, within = off % peb_size).
 */

#ifndef LIB_BLOB_DB_STORE_H_
#define LIB_BLOB_DB_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Geometry reported by the backend at open time. */
struct blob_db_store_geom {
	size_t   peb_size;    /* usable bytes per PEB (erase block / LEB) */
	size_t   write_align; /* minimum write granularity in bytes */
	uint16_t n_pebs;      /* total addressable PEBs */
	/* Whether blob_db_maintain() should format never-written buckets ahead
	 * of use. True where that is what takes the erase off the first write
	 * (flash_area). A backend that hands out erased blocks of its own (UBI)
	 * says false: formatting ahead would only use those blocks up early. */
	bool     preformat;
};

/*
 * Open the backing store and report its geometry. On success the store is
 * ready for read/write/erase. Returns 0 or a negative errno.
 *
 * discard is true when the caller is about to erase the whole store
 * (blob_db_format()). A backend that keeps metadata of its own may then
 * replace a substrate it cannot otherwise open — one written by another
 * backend or another release — instead of refusing it. With discard false it
 * must refuse such a substrate and leave it untouched.
 */
int blob_db_store_open(struct blob_db_store_geom *geom, bool discard);

/* Release the backing store. Idempotent. */
void blob_db_store_close(void);

/* Read len bytes at flat byte offset off (must stay within one PEB). */
int blob_db_store_read(off_t off, void *buf, size_t len);

/* Write len bytes at flat byte offset off (must stay within one PEB). */
int blob_db_store_write(off_t off, const void *buf, size_t len);

/*
 * Erase the region [off, off+len). blob_db only ever erases whole PEBs: len is
 * either peb_size (one PEB) or the whole device. After erase the region reads
 * back as the erased value (0xff).
 */
int blob_db_store_erase(off_t off, size_t len);

/*
 * Replace the PEB at off with len bytes from buf: the PEB reads as buf
 * followed by the erased value, and later writes may append after it. Equal to
 * blob_db_store_erase(off, peb_size) then blob_db_store_write(off, buf, len),
 * except that a backend reporting blob_db_store_replace_is_atomic() promises
 * a power loss leaves either the old contents or the new ones, never a mix
 * and never an erased PEB. off is PEB-aligned and len fits one PEB.
 */
int blob_db_store_replace(off_t off, const void *buf, size_t len);

/* Whether blob_db_store_replace() is atomic across power loss. When it is,
 * compaction replaces a bucket in one call instead of staging the image
 * through the scratch sector. */
bool blob_db_store_replace_is_atomic(void);

/*
 * Run up to budget steps of the backend's own deferred work; one step erases
 * at most one block. *performed is set to the steps run, *more to whether
 * work is left. The work must not change what blob_db reads back. Called with
 * the blob_db lock held. A backend with nothing to defer reports 0 and false.
 */
int blob_db_store_maintain(uint32_t budget, uint32_t *performed, bool *more);

/*
 * Tell the background maintenance helper (CONFIG_BLOB_DB_MAINT_WORK) there may
 * be work: a backend calls it when it has just deferred some, e.g. queued a
 * block for erase. Called with the blob_db lock held; never blocks.
 */
#if defined(CONFIG_BLOB_DB_MAINT_WORK)
void blob_db_maint_kick(void);
#else
static inline void blob_db_maint_kick(void)
{
}
#endif

/* I/O accounting, counted here so it covers both backends and every caller
 * uniformly (CONFIG_BLOB_DB_IOSTATS; compiles to nothing when disabled). */
#if defined(CONFIG_BLOB_DB_IOSTATS)
enum blob_db_io_op {
	BLOB_DB_IO_READ,
	BLOB_DB_IO_WRITE,
	BLOB_DB_IO_ERASE,
};
void blob_db_io_note(enum blob_db_io_op op, size_t bytes);
#define BLOB_DB_IO_NOTE(op, bytes) blob_db_io_note((op), (bytes))
#else
#define BLOB_DB_IO_NOTE(op, bytes) ((void)0)
#endif

#endif /* LIB_BLOB_DB_STORE_H_ */
