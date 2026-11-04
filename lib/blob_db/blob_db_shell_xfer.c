/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db export / import — the store's live objects as checksummed text
 * lines, and back. The format is doc/tools/blob_db_export.md; in short:
 *
 *   %BDB1 H fmt=5.0 backend=ubi ... next_id=231 *1a2b   header
 *   %BDB1 O <id> <len> *....                            object begins
 *   %BDB1 D <id> <off> <hex, up to 32 bytes> *....      data
 *   %BDB1 E <id> <len> <crc32> *....                    object ends
 *   %BDB1 X id <id> <err> *....                         unreadable object
 *   %BDB1 X bucket <bid> <err> *....                    unreadable bucket
 *   %BDB1 T objects=.. bytes=.. errors=.. lines=.. *.... trailer
 *
 * Every line carries a CRC-16 of the text before " *", so a host can pick
 * the lines out of a console that interleaves log output with them, and drop
 * one that a log message split or the serial line garbled.
 *
 * Export uses the inspect API to enumerate each bucket's live ids and the
 * public blob_db_size() / blob_db_read() to read them, so a large object is
 * dumped as its content, not its segment table. blob_db_iterate() cannot
 * serve: its callback must not read (a read stages its bucket in the sector
 * buffer the walk is using) and it hands a large object's index record as
 * the payload. Import uses only the public API: it allocates ids up to the
 * source's next_id on a freshly formatted store, then binds each object with
 * blob_db_update(), so every id, and the id counter, survive the trip.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_inspect.h>

#include "blob_db_shell_xfer.h"

#define XFER_PREFIX    "%BDB1"
/* The longest line is a D record: prefix, type, a 20-digit id, a 10-digit
 * offset, 64 hex digits and the CRC; about 130 characters. */
#define XFER_LINE_MAX  192
/* Payload bytes per D line. */
#define XFER_CHUNK     32
/* Live ids gathered from a bucket per pass of its sector. */
#define XFER_IDS       32
/* Most ids `import` allocates to reach the source's next_id. Each 256 cost a
 * master record (BLOB_DB_ID_HINT_STEP); a larger gap needs a command that
 * sets the counter, which blob_db does not offer yet. */
#define XFER_ALLOC_MAX (1ULL << 24)

#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)
#define XFER_MAX_OBJECT CONFIG_BLOB_DB_MAX_OBJECT_LEN
#else
#define XFER_MAX_OBJECT CONFIG_BLOB_DB_MAX_PAYLOAD_LEN
#endif

/* One line, built up field by field. */
struct rec {
	char buf[XFER_LINE_MAX];
	size_t len;
	bool overflow;
};

static __printf_like(2, 3) void rec_add(struct rec *r, const char *fmt, ...)
{
	const size_t room = sizeof(r->buf) - r->len;
	va_list ap;

	va_start(ap, fmt);
	const int n = vsnprintk(r->buf + r->len, room, fmt, ap);

	va_end(ap);
	if (n < 0 || (size_t)n >= room) {
		r->overflow = true;
		r->len = sizeof(r->buf) - 1;
		return;
	}
	r->len += (size_t)n;
}

static void rec_begin(struct rec *r, const char *type)
{
	r->len = 0;
	r->overflow = false;
	rec_add(r, "%s %s", XFER_PREFIX, type);
}

static void rec_hex(struct rec *r, const uint8_t *p, size_t n)
{
	if (r->len + 1 + 2 * n >= sizeof(r->buf)) {
		r->overflow = true;
		return;
	}
	r->buf[r->len++] = ' ';
	r->len += bin2hex(p, n, r->buf + r->len, sizeof(r->buf) - r->len);
}

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xffff, no reflection, no final
 * xor); Python's binascii.crc_hqx(line, 0xffff). */
static uint16_t line_crc(const char *s, size_t n)
{
	return crc16_itu_t(0xffff, (const uint8_t *)s, n);
}

static void rec_emit(const struct shell *sh, const struct rec *r)
{
	__ASSERT(!r->overflow, "export line overflow");
	shell_print(sh, "%s *%04x", r->buf, line_crc(r->buf, r->len));
}

static int parse_u64(const char *s, int base, uint64_t *out)
{
	char *end;

	errno = 0;
	*out = strtoull(s, &end, base);
	return (errno || end == s || *end != '\0') ? -EINVAL : 0;
}

/* blob_db export [id [offset len]] --------------------------------------- */

struct export_ctx {
	const struct shell *sh;
	uint32_t lines;
	uint32_t objects;
	uint32_t errors;
	uint64_t bytes;
};

static void ex_emit(struct export_ctx *x, const struct rec *r)
{
	rec_emit(x->sh, r);
	x->lines++;
}

static void ex_error(struct export_ctx *x, const char *what, uint64_t which,
		     int err)
{
	struct rec r;

	rec_begin(&r, "X");
	rec_add(&r, " %s %llu %d", what, (unsigned long long)which, err);
	ex_emit(x, &r);
	x->errors++;
}

/* D lines for [off, end) of a live id; the CRC-32 of the bytes is added to
 * @p crc when it is not NULL. */
static int ex_data(struct export_ctx *x, uint64_t id, size_t off, size_t end,
		   uint32_t *crc)
{
	uint8_t __aligned(4) buf[XFER_CHUNK];
	struct rec r;

	while (off < end) {
		const size_t want = MIN(sizeof(buf), end - off);
		size_t got;
		int rc = blob_db_read(id, off, buf, want, &got);

		if (rc < 0) {
			return rc;
		}
		if (got != want) {
			return -EIO;    /* shrank since its size was taken */
		}
		if (crc) {
			*crc = crc32_ieee_update(*crc, buf, got);
		}
		rec_begin(&r, "D");
		rec_add(&r, " %llu %zu", (unsigned long long)id, off);
		rec_hex(&r, buf, got);
		ex_emit(x, &r);
		off += got;
	}
	return 0;
}

static int ex_object(struct export_ctx *x, uint64_t id)
{
	struct rec r;
	size_t size;
	uint32_t crc = 0;
	int rc = blob_db_size(id, &size);

	if (rc == 0) {
		rec_begin(&r, "O");
		rec_add(&r, " %llu %zu", (unsigned long long)id, size);
		ex_emit(x, &r);
		rc = ex_data(x, id, 0, size, &crc);
	}
	if (rc < 0) {
		ex_error(x, "id", id, rc);
		return rc;
	}
	rec_begin(&r, "E");
	rec_add(&r, " %llu %zu %08x", (unsigned long long)id, size, crc);
	ex_emit(x, &r);
	x->objects++;
	x->bytes += size;
	return 0;
}

/* Gathers a bucket's live object ids, segments excluded: those are chunks
 * of an object, read through its id. A bucket with more than XFER_IDS is
 * taken in several passes; each skips the ids the earlier ones returned. */
struct id_batch {
	uint64_t ids[XFER_IDS];
	uint32_t skip;
	uint16_t n;
	bool more;
};

static int collect_live(const struct blob_db_inspect_slot *s, void *user)
{
	struct id_batch *b = user;

	if (s->state != BLOB_DB_INSPECT_LIVE || s->segment) {
		return 0;
	}
	if (b->skip) {
		b->skip--;
		return 0;
	}
	if (b->n == XFER_IDS) {
		b->more = true;
		return 1;
	}
	b->ids[b->n++] = s->id;
	return 0;
}

static int ex_all(struct export_ctx *x, uint16_t n_buckets)
{
	for (uint16_t bid = 0; bid < n_buckets; bid++) {
		for (uint32_t pass = 0;; pass++) {
			struct id_batch b = { .skip = pass * XFER_IDS };
			struct blob_db_inspect_bucket bs;
			int rc = blob_db_inspect_bucket_get(bid, &bs,
							    collect_live, &b);

			if (rc == -ENODEV) {
				return rc;
			}
			if (rc < 0) {
				ex_error(x, "bucket", bid, rc);
				break;
			}
			/* Read outside the walk: its callback must not call
			 * blob_db. */
			for (uint16_t i = 0; i < b.n; i++) {
				if (ex_object(x, b.ids[i]) == -ENODEV) {
					return -ENODEV;
				}
			}
			if (!b.more) {
				break;
			}
		}
	}
	return 0;
}

static void ex_header(struct export_ctx *x,
		      const struct blob_db_inspect_info *in)
{
	struct rec r;

	rec_begin(&r, "H");
	rec_add(&r, " fmt=%u.%u backend=%s sector=%zu buckets=%u "
		"max_payload=%zu max_object=%u next_id=%llu",
		in->format_major, in->format_minor,
		IS_ENABLED(CONFIG_BLOB_DB_BACKEND_UBI) ? "ubi" : "flash_area",
		in->sector_size, in->n_buckets, in->max_payload_len,
		(unsigned int)XFER_MAX_OBJECT,
		(unsigned long long)in->next_id);
	ex_emit(x, &r);
}

static void ex_trailer(struct export_ctx *x)
{
	struct rec r;

	rec_begin(&r, "T");
	rec_add(&r, " objects=%u bytes=%llu errors=%u lines=%u", x->objects,
		(unsigned long long)x->bytes, x->errors, x->lines);
	ex_emit(x, &r);
}

int blob_db_shell_cmd_export(const struct shell *sh, size_t argc, char **argv)
{
	struct blob_db_inspect_info in;
	struct export_ctx x = { .sh = sh };
	uint64_t id = 0, off = 0, len = 0;
	int rc = blob_db_inspect_info_get(&in);

	if (rc == -ENODEV) {
		shell_error(sh, "blob_db not mounted");
		return rc;
	}
	if (rc < 0) {
		shell_error(sh, "info: %d", rc);
		return rc;
	}
	if (argc == 3) {
		shell_error(sh, "usage: export [id [offset len]]");
		return -EINVAL;
	}
	if ((argc > 1 && parse_u64(argv[1], 0, &id) < 0) ||
	    (argc > 3 && (parse_u64(argv[2], 0, &off) < 0 ||
			  parse_u64(argv[3], 0, &len) < 0))) {
		shell_error(sh, "not a number");
		return -EINVAL;
	}

	if (argc > 3) {
		/* A range, to resend lines that arrived damaged: D lines
		 * only, clipped to the object. */
		size_t size;

		rc = blob_db_size(id, &size);
		if (rc == 0) {
			off = MIN(off, (uint64_t)size);
			len = MIN(len, (uint64_t)size - off);
			rc = ex_data(&x, id, (size_t)off, (size_t)(off + len),
				     NULL);
		}
		if (rc < 0) {
			ex_error(&x, "id", id, rc);
		}
		return rc;
	}

	ex_header(&x, &in);
	if (argc > 1) {
		(void)ex_object(&x, id);
	} else {
		rc = ex_all(&x, in.n_buckets);
		if (rc == -ENODEV) {
			shell_error(sh, "blob_db unmounted during export");
			return rc;
		}
	}
	ex_trailer(&x);
	return 0;
}

#if defined(CONFIG_BLOB_DB_SHELL_ADMIN)
/* blob_db import <line> ------------------------------------------------------
 *
 * One exported line per call. Each line gets a reply line in the same
 * format: `A <type> ...` accepted, `N <type> ... err=<errno>` refused (with
 * the reason on a plain line above it). A host that waits for each reply
 * before sending the next line needs no flow control from the serial link,
 * and resending the line just accepted (its reply was lost) is acknowledged
 * again without being applied twice.
 */

static uint8_t __aligned(4) imp_buf[CONFIG_BLOB_DB_SHELL_IMPORT_BUF_SIZE];

static struct {
	bool open;              /* an H was accepted on a fresh store       */
	uint64_t src_next_id;   /* the source's next_id, from H             */
	bool in_obj;            /* between O and E                          */
	uint64_t id;            /* the object being received                */
	size_t len;
	size_t have;            /* bytes received, in order                 */
	uint32_t objects;       /* restored                                 */
	uint64_t bytes;
	uint32_t missing;       /* X records: the source could not read it  */
	struct rec last_line;   /* the last line accepted ...               */
	struct rec last_ack;    /* ... and the reply it got                 */
} imp;

static void imp_reply(struct rec *r, bool ok, const char *type, int n_keys, const uint64_t *keys)
{
	rec_begin(r, ok ? "A" : "N");
	rec_add(r, " %s", type);
	for (int i = 0; i < n_keys; i++) {
		rec_add(r, " %llu", (unsigned long long)keys[i]);
	}
}

static int imp_refuse(const struct shell *sh, const char *type, int n_keys,
		      const uint64_t *keys, int err, const char *why)
{
	struct rec r;

	shell_error(sh, "import: %s", why);
	imp_reply(&r, false, type, n_keys, keys);
	rec_add(&r, " err=%d", err);
	rec_emit(sh, &r);
	return err;
}

/* `key=value` among the fields; -ENOENT if absent. */
static int kv_u64(char **f, size_t nf, const char *key, int base,
		  uint64_t *out)
{
	const size_t kl = strlen(key);

	for (size_t i = 0; i < nf; i++) {
		if (strncmp(f[i], key, kl) == 0 && f[i][kl] == '=') {
			return parse_u64(f[i] + kl + 1, base, out);
		}
	}
	return -ENOENT;
}

/* Freshly formatted: the root's empty slot is the only slot in the store,
 * nothing torn, no segmented write in flight. Any other state could hold an
 * id that was deleted, and binding a deleted id is undefined (D3). */
static int store_fresh(uint64_t *next_id)
{
	struct blob_db_inspect_info in;
	uint32_t slots = 0;
	size_t root_len;
	int rc = blob_db_inspect_info_get(&in);

	if (rc < 0) {
		return rc;
	}
	if (in.seg_owner || in.wedged) {
		return -ENOTEMPTY;
	}
	for (uint16_t bid = 0; bid < in.n_buckets; bid++) {
		struct blob_db_inspect_bucket bs;

		rc = blob_db_inspect_bucket_get(bid, &bs, NULL, NULL);
		if (rc < 0) {
			return rc;
		}
		slots += bs.objects + bs.segments + bs.superseded +
			 bs.tombstones + bs.rotten + (bs.tail_bytes ? 1 : 0);
	}
	rc = blob_db_size(BLOB_DB_ROOT_ID, &root_len);
	if (rc < 0) {
		return rc;
	}
	if (slots != 1 || root_len != 0) {
		return -ENOTEMPTY;
	}
	*next_id = in.next_id;
	return 0;
}

static int imp_header(const struct shell *sh, char **f, size_t nf,
		      struct rec *ack)
{
	uint64_t src_next, next;
	int rc = kv_u64(f, nf, "next_id", 0, &src_next);

	if (rc < 0 || src_next < 2) {
		return imp_refuse(sh, "H", 0, NULL, -EINVAL,
				  "header without a valid next_id");
	}

	rc = store_fresh(&next);
	if (rc == -ENODEV) {
		return imp_refuse(sh, "H", 0, NULL, rc, "blob_db not mounted");
	}
	if (rc == -ENOTEMPTY) {
		return imp_refuse(sh, "H", 0, NULL, rc,
				  "the store is not freshly formatted; run "
				  "`blob_db format yes` first");
	}
	if (rc < 0) {
		return imp_refuse(sh, "H", 0, NULL, rc, "reading the store");
	}
	if (src_next > next && src_next - next > XFER_ALLOC_MAX) {
		return imp_refuse(sh, "H", 0, NULL, -E2BIG,
				  "next_id too far ahead to allocate up to");
	}

	/* Make every id below the source's next_id allocated, so each object
	 * can be bound at its own id, and the ids the source had deleted stay
	 * unused, as they were there. A target that is already further keeps
	 * its counter: the ids below it are allocated just the same. */
	while (next < src_next) {
		const uint64_t id = blob_db_alloc_id();

		if (id == BLOB_DB_INVALID_ID) {
			return imp_refuse(sh, "H", 0, NULL, -EIO,
					  "allocating ids failed");
		}
		next = id + 1;
	}

	memset(&imp, 0, sizeof(imp));
	imp.open = true;
	imp.src_next_id = src_next;

	imp_reply(ack, true, "H", 0, NULL);
	rec_add(ack, " next_id=%llu", (unsigned long long)next);
	return 0;
}

static int imp_object(const struct shell *sh, char **f, size_t nf,
		      struct rec *ack)
{
	uint64_t id, len;

	if (nf != 2 || parse_u64(f[0], 10, &id) < 0 ||
	    parse_u64(f[1], 10, &len) < 0) {
		return imp_refuse(sh, "O", 0, NULL, -EINVAL, "usage: O <id> <len>");
	}
	if (!imp.open) {
		return imp_refuse(sh, "O", 1, &id, -EPROTO, "no header yet");
	}
	if (id == BLOB_DB_INVALID_ID || id >= imp.src_next_id) {
		return imp_refuse(sh, "O", 1, &id, -EINVAL,
				  "id at or past the header's next_id");
	}
	if (len > sizeof(imp_buf)) {
		return imp_refuse(sh, "O", 1, &id, -EFBIG,
				  "larger than the import buffer "
				  "(CONFIG_BLOB_DB_SHELL_IMPORT_BUF_SIZE)");
	}

	/* An object left unfinished is abandoned: the host started over. */
	imp.in_obj = true;
	imp.id = id;
	imp.len = (size_t)len;
	imp.have = 0;

	imp_reply(ack, true, "O", 1, &id);
	return 0;
}

static int imp_data(const struct shell *sh, char **f, size_t nf,
		    struct rec *ack)
{
	uint8_t data[XFER_LINE_MAX / 2];
	uint64_t keys[2];
	size_t hexlen;

	if (nf != 3 || parse_u64(f[0], 10, &keys[0]) < 0 ||
	    parse_u64(f[1], 10, &keys[1]) < 0) {
		return imp_refuse(sh, "D", 0, NULL, -EINVAL,
				  "usage: D <id> <off> <hex>");
	}
	hexlen = strlen(f[2]);
	if (hexlen % 2 || hexlen / 2 > sizeof(data) ||
	    hex2bin(f[2], hexlen, data, sizeof(data)) != hexlen / 2) {
		return imp_refuse(sh, "D", 2, keys, -EINVAL, "bad hex");
	}
	if (!imp.in_obj || keys[0] != imp.id) {
		return imp_refuse(sh, "D", 2, keys, -EPROTO,
				  "not the object being received");
	}

	const size_t n = hexlen / 2;

	if (keys[1] != imp.have) {
		struct rec r;

		shell_error(sh, "import: expected offset %zu", imp.have);
		imp_reply(&r, false, "D", 2, keys);
		rec_add(&r, " err=%d expect=%zu", -EINVAL, imp.have);
		rec_emit(sh, &r);
		return -EINVAL;
	}
	if (n > imp.len - imp.have) {
		return imp_refuse(sh, "D", 2, keys, -EINVAL,
				  "data past the object's length");
	}
	memcpy(imp_buf + imp.have, data, n);
	imp.have += n;

	imp_reply(ack, true, "D", 2, keys);
	return 0;
}

static int imp_verify(uint64_t id, size_t len, uint32_t crc)
{
	uint8_t __aligned(4) buf[64];
	uint32_t got_crc = 0;
	size_t size;
	int rc = blob_db_size(id, &size);

	if (rc < 0) {
		return rc;
	}
	if (size != len) {
		return -EIO;
	}
	for (size_t off = 0; off < len;) {
		size_t got;

		rc = blob_db_read(id, off, buf, MIN(sizeof(buf), len - off),
				  &got);
		if (rc < 0) {
			return rc;
		}
		if (got == 0) {
			return -EIO;
		}
		got_crc = crc32_ieee_update(got_crc, buf, got);
		off += got;
	}
	return got_crc == crc ? 0 : -EIO;
}

static int imp_end(const struct shell *sh, char **f, size_t nf,
		   struct rec *ack)
{
	uint64_t id, len, crc;

	if (nf != 3 || parse_u64(f[0], 10, &id) < 0 ||
	    parse_u64(f[1], 10, &len) < 0 || parse_u64(f[2], 16, &crc) < 0) {
		return imp_refuse(sh, "E", 0, NULL, -EINVAL,
				  "usage: E <id> <len> <crc32>");
	}
	if (!imp.in_obj || id != imp.id) {
		return imp_refuse(sh, "E", 1, &id, -EPROTO,
				  "not the object being received");
	}
	if (len != imp.len || imp.have != imp.len) {
		struct rec r;

		shell_error(sh, "import: %zu of %zu bytes received", imp.have,
			    imp.len);
		imp_reply(&r, false, "E", 1, &id);
		rec_add(&r, " err=%d expect=%zu", -EINVAL, imp.have);
		rec_emit(sh, &r);
		return -EINVAL;
	}
	if (crc32_ieee(imp_buf, imp.len) != (uint32_t)crc) {
		/* Every line passed its own check; the object as a whole did
		 * not. Start it over. */
		imp.in_obj = false;
		return imp_refuse(sh, "E", 1, &id, -EBADMSG,
				  "object CRC-32 mismatch; resend it from O");
	}

	int rc = blob_db_update(id, imp_buf, imp.len);

	if (rc < 0) {
		return imp_refuse(sh, "E", 1, &id, rc, "blob_db_update failed");
	}
	rc = imp_verify(id, imp.len, (uint32_t)crc);
	if (rc < 0) {
		return imp_refuse(sh, "E", 1, &id, rc,
				  "read-back does not match");
	}

	imp.in_obj = false;
	imp.objects++;
	imp.bytes += imp.len;

	imp_reply(ack, true, "E", 1, &id);
	return 0;
}

static int imp_missing(const struct shell *sh, char **f, size_t nf,
		       struct rec *ack)
{
	if (!imp.open) {
		return imp_refuse(sh, "X", 0, NULL, -EPROTO, "no header yet");
	}
	/* The source could not read an object, or a whole bucket; whatever
	 * was in it is not in this store. Counted for the trailer. */
	shell_warn(sh, "import: the source could not read %s %s (%s); it is "
		   "missing here", nf > 0 ? f[0] : "?", nf > 1 ? f[1] : "?",
		   nf > 2 ? f[2] : "?");
	imp.in_obj = false;
	imp.missing++;
	imp_reply(ack, true, "X", 0, NULL);
	return 0;
}

static int imp_trailer(const struct shell *sh, char **f, size_t nf,
		       struct rec *ack)
{
	uint64_t objects, bytes, errors;

	if (kv_u64(f, nf, "objects", 10, &objects) < 0 ||
	    kv_u64(f, nf, "bytes", 10, &bytes) < 0 ||
	    kv_u64(f, nf, "errors", 10, &errors) < 0) {
		return imp_refuse(sh, "T", 0, NULL, -EINVAL,
				  "trailer without objects, bytes, errors");
	}
	if (!imp.open || imp.in_obj) {
		return imp_refuse(sh, "T", 0, NULL, -EPROTO,
				  imp.open ? "an object is unfinished"
					   : "no header yet");
	}
	if (objects != imp.objects || bytes != imp.bytes ||
	    errors != imp.missing) {
		struct rec r;

		shell_error(sh, "import: the dump has %llu objects, %llu B, "
			    "%llu errors; restored %u objects, %llu B, %u "
			    "errors", (unsigned long long)objects,
			    (unsigned long long)bytes,
			    (unsigned long long)errors, imp.objects,
			    (unsigned long long)imp.bytes, imp.missing);
		imp_reply(&r, false, "T", 0, NULL);
		rec_add(&r, " err=%d objects=%u bytes=%llu errors=%u", -EIO,
			imp.objects, (unsigned long long)imp.bytes,
			imp.missing);
		rec_emit(sh, &r);
		return -EIO;
	}
	if (imp.missing) {
		shell_warn(sh, "import: complete, but %u objects or buckets of "
			   "the source are missing", imp.missing);
	}

	imp.open = false;
	imp_reply(ack, true, "T", 0, NULL);
	rec_add(ack, " objects=%u bytes=%llu errors=%u", imp.objects,
		(unsigned long long)imp.bytes, imp.missing);
	return 0;
}

static void imp_status(const struct shell *sh)
{
	if (!imp.open) {
		shell_print(sh, "import: idle");
		return;
	}
	shell_print(sh, "import: open, source next_id %llu; %u objects "
		    "(%llu B) restored, %u missing",
		    (unsigned long long)imp.src_next_id, imp.objects,
		    (unsigned long long)imp.bytes, imp.missing);
	if (imp.in_obj) {
		shell_print(sh, "        receiving id %llu, %zu of %zu B",
			    (unsigned long long)imp.id, imp.have, imp.len);
	}
}

/* Fields of a record: prefix, type, and at most this many more. */
#define XFER_FIELDS 12

int blob_db_shell_cmd_import(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1) {
		imp_status(sh);
		return 0;
	}

	/* The rest of the command line, unsplit (SHELL_OPT_ARG_RAW): the text
	 * the CRC covers runs up to the last " *". */
	const char *raw = argv[1];
	const char *star = strrchr(raw, '*');
	struct rec line = { 0 };
	uint64_t crc;

	if (star == NULL || star == raw || star[-1] != ' ' ||
	    strncmp(raw, XFER_PREFIX " ", sizeof(XFER_PREFIX)) != 0) {
		shell_error(sh, "usage: import %s <type> ... *<crc>  (a line "
			    "of `blob_db export` output)", XFER_PREFIX);
		return -EINVAL;
	}
	rec_add(&line, "%.*s", (int)(star - 1 - raw), raw);

	char crc_hex[8] = { 0 };

	/* Tolerate what a terminal may leave at the end: spaces, a CR. */
	strncpy(crc_hex, star + 1, sizeof(crc_hex) - 1);
	for (size_t i = 0; i < sizeof(crc_hex); i++) {
		if (crc_hex[i] == ' ' || crc_hex[i] == '\r') {
			crc_hex[i] = '\0';
		}
	}
	if (line.overflow || parse_u64(crc_hex, 16, &crc) < 0 ||
	    crc > UINT16_MAX || line_crc(line.buf, line.len) != crc) {
		return imp_refuse(sh, "-", 0, NULL, -EBADMSG,
				  "line CRC mismatch; resend the line");
	}

	/* The line just accepted, again: its reply was lost. Not for H, which
	 * is idempotent by itself (allocating ids writes no slot, so the store
	 * is still fresh) and which must start a new import after a format
	 * even when it is the same header. */
	if (line.len == imp.last_line.len && line.buf[sizeof(XFER_PREFIX)] != 'H' &&
	    memcmp(line.buf, imp.last_line.buf, line.len) == 0) {
		rec_emit(sh, &imp.last_ack);
		return 0;
	}

	/* Split a copy at single spaces, which is all an exported line has. */
	static char work[XFER_LINE_MAX];
	char *fld[XFER_FIELDS + 2];
	size_t n = 0;

	memcpy(work, line.buf, line.len + 1);
	for (char *p = work; p != NULL && n < ARRAY_SIZE(fld);) {
		fld[n++] = p;
		p = strchr(p, ' ');
		if (p != NULL) {
			*p++ = '\0';
		}
	}

	if (n < 2 || strlen(fld[1]) != 1 ||
	    (n == ARRAY_SIZE(fld) && strchr(fld[n - 1], ' ') != NULL)) {
		return imp_refuse(sh, "-", 0, NULL, -EINVAL, "unknown record");
	}

	char **f = &fld[2];
	const size_t nf = n - 2;
	struct rec ack;
	int rc;

	switch (fld[1][0]) {
	case 'H':
		rc = imp_header(sh, f, nf, &ack);
		break;
	case 'O':
		rc = imp_object(sh, f, nf, &ack);
		break;
	case 'D':
		rc = imp_data(sh, f, nf, &ack);
		break;
	case 'E':
		rc = imp_end(sh, f, nf, &ack);
		break;
	case 'X':
		rc = imp_missing(sh, f, nf, &ack);
		break;
	case 'T':
		rc = imp_trailer(sh, f, nf, &ack);
		break;
	default:
		return imp_refuse(sh, "-", 0, NULL, -EINVAL, "unknown record");
	}
	if (rc < 0) {
		return rc;
	}

	imp.last_line = line;
	imp.last_ack = ack;
	rec_emit(sh, &ack);
	return 0;
}
#endif /* CONFIG_BLOB_DB_SHELL_ADMIN */
