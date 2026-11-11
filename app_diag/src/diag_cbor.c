/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * A small CBOR (RFC 8949) reader for app_diag: check that bytes are one
 * well-formed item, print it in diagnostic notation, and pick known layouts
 * apart field by field. It knows nothing of any schema, so it prints a record
 * a newer firmware wrote as faithfully as one this build could write.
 *
 * zcbor is not used: the decoders it generates take one schema each, and the
 * point here is to show bytes whose schema may not be the one compiled in.
 */

#include <errno.h>
#include <stdarg.h>
#include <string.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "diag.h"

#define CBOR_DEPTH_MAX 16
#define CBOR_BREAK     0xff
#define CBOR_AI_INDEF  31

/* Read an item head: major type, argument, and whether the length is
 * indefinite. Advances *pp. */
static int head(const uint8_t **pp, const uint8_t *end, uint8_t *major,
		uint64_t *arg, bool *indef)
{
	const uint8_t *p = *pp;

	if (p >= end) {
		return -ENODATA;
	}
	const uint8_t ib = *p++;
	const uint8_t ai = ib & 0x1f;

	*major = ib >> 5;
	*indef = false;
	if (ai < 24) {
		*arg = ai;
	} else if (ai <= 27) {
		const size_t w = 1u << (ai - 24);

		if ((size_t)(end - p) < w) {
			return -ENODATA;
		}
		*arg = 0;
		for (size_t i = 0; i < w; i++) {
			*arg = (*arg << 8) | *p++;
		}
	} else if (ai == CBOR_AI_INDEF &&
		   (*major == 2 || *major == 3 || *major == 4 || *major == 5)) {
		*indef = true;
		*arg = 0;
	} else {
		return -EILSEQ;   /* reserved, or a stray break */
	}
	*pp = p;
	return 0;
}

static int skip(const uint8_t **pp, const uint8_t *end, int depth)
{
	uint8_t major;
	uint64_t arg;
	bool indef;

	if (depth > CBOR_DEPTH_MAX) {
		return -ELOOP;
	}
	int rc = head(pp, end, &major, &arg, &indef);

	if (rc < 0) {
		return rc;
	}
	switch (major) {
	case 0: case 1: case 7:
		return 0;
	case 2: case 3:
		if (indef) {
			/* Chunks of the same major type, then a break. */
			while (*pp < end && **pp != CBOR_BREAK) {
				if ((**pp >> 5) != major) {
					return -EILSEQ;
				}
				rc = skip(pp, end, depth + 1);
				if (rc < 0) {
					return rc;
				}
			}
			if (*pp >= end) {
				return -ENODATA;
			}
			(*pp)++;
			return 0;
		}
		if (arg > (uint64_t)(end - *pp)) {
			return -ENODATA;
		}
		*pp += arg;
		return 0;
	case 6:
		return skip(pp, end, depth + 1);
	default: { /* 4: array, 5: map */
		const uint64_t per = (major == 5) ? 2 : 1;

		if (indef) {
			uint64_t items = 0;

			while (*pp < end && **pp != CBOR_BREAK) {
				rc = skip(pp, end, depth + 1);
				if (rc < 0) {
					return rc;
				}
				items++;
			}
			if (*pp >= end || items % per) {
				return *pp >= end ? -ENODATA : -EILSEQ;
			}
			(*pp)++;
			return 0;
		}
		/* Every item takes at least one byte: a count past the end
		 * is malformed, and bounding it here bounds the loop. */
		if (arg > (uint64_t)(end - *pp)) {
			return -ENODATA;
		}
		for (uint64_t i = 0; i < arg * per; i++) {
			rc = skip(pp, end, depth + 1);
			if (rc < 0) {
				return rc;
			}
		}
		return 0;
	}
	}
}

int diag_cbor_item_len(const uint8_t *p, size_t n, size_t *len)
{
	const uint8_t *q = p;
	int rc = skip(&q, p + n, 0);

	if (rc < 0) {
		return rc;
	}
	*len = (size_t)(q - p);
	return 0;
}

/* ---- diagnostic notation --------------------------------------------- */

struct fmt {
	char *buf;
	size_t sz;
	size_t len;
	bool full;
};

static __printf_like(2, 3) void put(struct fmt *f, const char *s, ...)
{
	if (f->full) {
		return;
	}
	va_list ap;

	va_start(ap, s);
	const int n = vsnprintk(f->buf + f->len, f->sz - f->len, s, ap);

	va_end(ap);
	/* Four bytes are always kept free for the "..." that marks a
	 * truncated line. */
	if (n < 0 || f->len + (size_t)n + 4 > f->sz) {
		f->full = true;
		strcpy(f->buf + f->sz - 4, "...");
		f->len = f->sz - 1;
		return;
	}
	f->len += (size_t)n;
}

#define FMT_BYTES_MAX 16

static int format(const uint8_t **pp, const uint8_t *end, struct fmt *f,
		  int depth)
{
	uint8_t major;
	uint64_t arg;
	bool indef;

	if (depth > CBOR_DEPTH_MAX) {
		return -ELOOP;
	}
	const uint8_t ib = (*pp < end) ? **pp : 0;
	int rc = head(pp, end, &major, &arg, &indef);

	if (rc < 0) {
		return rc;
	}
	switch (major) {
	case 0:
		put(f, "%llu", (unsigned long long)arg);
		return 0;
	case 1:
		if (arg == UINT64_MAX) {
			put(f, "-18446744073709551616");
		} else {
			put(f, "-%llu", (unsigned long long)arg + 1);
		}
		return 0;
	case 2: case 3:
		if (indef) {
			/* Shown as the chunks it was sent in, RFC 8949 §8. */
			put(f, "(_ ");
			for (int i = 0; *pp < end && **pp != CBOR_BREAK; i++) {
				put(f, i ? ", " : "");
				rc = format(pp, end, f, depth + 1);
				if (rc < 0) {
					return rc;
				}
			}
			if (*pp >= end) {
				return -ENODATA;
			}
			(*pp)++;
			put(f, ")");
			return 0;
		}
		if (arg > (uint64_t)(end - *pp)) {
			return -ENODATA;
		}
		if (major == 2) {
			put(f, "h'");
			for (uint64_t i = 0; i < MIN(arg, FMT_BYTES_MAX); i++) {
				put(f, "%02x", (*pp)[i]);
			}
			put(f, arg > FMT_BYTES_MAX ? "..'" : "'");
		} else {
			put(f, "\"");
			for (uint64_t i = 0; i < arg; i++) {
				const char c = (char)(*pp)[i];

				put(f, "%c", (c >= 0x20 && c < 0x7f && c != '"') ?
				    c : '.');
			}
			put(f, "\"");
		}
		*pp += arg;
		return 0;
	case 6:
		put(f, "%llu(", (unsigned long long)arg);
		rc = format(pp, end, f, depth + 1);
		put(f, ")");
		return rc;
	case 7:
		switch (arg) {
		case 20: put(f, "false"); break;
		case 21: put(f, "true"); break;
		case 22: put(f, "null"); break;
		case 23: put(f, "undefined"); break;
		default:
			/* Floats are shown by their bits: printk has no %f. */
			if (ib >= 0xf9 && ib <= 0xfb) {
				put(f, "float(0x%llx)", (unsigned long long)arg);
			} else {
				put(f, "simple(%llu)", (unsigned long long)arg);
			}
			break;
		}
		return 0;
	default: { /* 4: array, 5: map */
		const bool map = (major == 5);

		put(f, map ? "{" : "[");
		put(f, indef ? "_ " : "");
		if (!indef && arg > (uint64_t)(end - *pp)) {
			return -ENODATA;
		}
		for (uint64_t i = 0;; i++) {
			if (indef) {
				if (*pp >= end) {
					return -ENODATA;
				}
				if (**pp == CBOR_BREAK) {
					(*pp)++;
					break;
				}
			} else if (i == arg) {
				break;
			}
			put(f, i ? ", " : "");
			rc = format(pp, end, f, depth + 1);
			if (rc < 0) {
				return rc;
			}
			if (map) {
				put(f, ": ");
				rc = format(pp, end, f, depth + 1);
				if (rc < 0) {
					return rc;
				}
			}
		}
		put(f, map ? "}" : "]");
		return 0;
	}
	}
}

int diag_cbor_format(const uint8_t *p, size_t n, char *out, size_t out_sz)
{
	struct fmt f = { .buf = out, .sz = out_sz };
	const uint8_t *q = p;

	if (out_sz < 8) {
		return -ENOMEM;
	}
	out[0] = '\0';
	int rc = format(&q, p + n, &f, 0);

	return rc < 0 ? rc : (int)(q - p);
}

/* ---- field reader ---------------------------------------------------- */

int diag_cbor_peek(const struct diag_cbor *c)
{
	return (c->err || c->p >= c->end) ? -1 : (*c->p >> 5);
}

static bool expect(struct diag_cbor *c, uint8_t want, uint64_t *v)
{
	uint8_t major;
	bool indef;

	if (c->err || head(&c->p, c->end, &major, v, &indef) < 0 ||
	    major != want || indef) {
		c->err = true;
		return false;
	}
	return true;
}

bool diag_cbor_uint(struct diag_cbor *c, uint64_t *v)
{
	return expect(c, 0, v);
}

bool diag_cbor_array(struct diag_cbor *c, uint64_t *n)
{
	return expect(c, 4, n);
}

bool diag_cbor_map(struct diag_cbor *c, uint64_t *n)
{
	return expect(c, 5, n);
}

bool diag_cbor_skip(struct diag_cbor *c)
{
	if (c->err || skip(&c->p, c->end, 0) < 0) {
		c->err = true;
		return false;
	}
	return true;
}
