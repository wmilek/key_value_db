/*
 * blob_db storage backend for the host harness: a RAM-backed NOR model with
 * the DK's geometry (8 MiB, 64 KB erase blocks, 4-byte write alignment).
 *
 * Program semantics are AND, like real NOR: a write can only clear bits, which
 * is what blob_db relies on when it invalidates a bucket header in place.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "blob_db_store.h"

#ifndef HB_PARTITION_BYTES
#define HB_PARTITION_BYTES (8u * 1024u * 1024u)
#endif
#ifndef HB_PEB_SIZE
#define HB_PEB_SIZE (64u * 1024u)
#endif
#ifndef HB_WRITE_ALIGN
#define HB_WRITE_ALIGN 4u
#endif

static uint8_t *g_mem;

int blob_db_store_open(struct blob_db_store_geom *geom)
{
	if (g_mem == NULL) {
		g_mem = malloc(HB_PARTITION_BYTES);
		if (g_mem == NULL) {
			return -ENOMEM;
		}
		memset(g_mem, 0xff, HB_PARTITION_BYTES);
	}
	geom->peb_size = HB_PEB_SIZE;
	geom->write_align = HB_WRITE_ALIGN;
	geom->n_pebs = (uint16_t)(HB_PARTITION_BYTES / HB_PEB_SIZE);
	return 0;
}

void blob_db_store_close(void) { }

/* Wipe the model between configurations without freeing it. */
void hb_store_wipe(void)
{
	if (g_mem) {
		memset(g_mem, 0xff, HB_PARTITION_BYTES);
	}
}

int blob_db_store_read(off_t off, void *buf, size_t len)
{
	if (off < 0 || (size_t)off + len > HB_PARTITION_BYTES) {
		return -EINVAL;
	}
	BLOB_DB_IO_NOTE(BLOB_DB_IO_READ, len);
	memcpy(buf, &g_mem[off], len);
	return 0;
}

int blob_db_store_write(off_t off, const void *buf, size_t len)
{
	const uint8_t *src = buf;

	if (off < 0 || (size_t)off + len > HB_PARTITION_BYTES) {
		return -EINVAL;
	}
	BLOB_DB_IO_NOTE(BLOB_DB_IO_WRITE, len);
	for (size_t i = 0; i < len; i++) {
		g_mem[off + i] &= src[i];   /* NOR program: 1 -> 0 only */
	}
	return 0;
}

int blob_db_store_erase(off_t off, size_t len)
{
	if (off < 0 || (size_t)off + len > HB_PARTITION_BYTES) {
		return -EINVAL;
	}
	BLOB_DB_IO_NOTE(BLOB_DB_IO_ERASE, len);
	memset(&g_mem[off], 0xff, len);
	return 0;
}

/* Direct view of the model, for the harness's own bucket census. */
const uint8_t *hb_store_ptr(void) { return g_mem; }
size_t hb_store_size(void) { return HB_PARTITION_BYTES; }
size_t hb_store_peb(void) { return HB_PEB_SIZE; }
