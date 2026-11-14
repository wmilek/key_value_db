/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * `diag`: name and decode the structures in a mounted store.
 *
 *   diag scan [-a]          every live object, classified; a census by type
 *   diag roots              the root registry, each root, what it points at
 *   diag show <id> [type]   one object decoded in full
 *   diag formats            the formats and past variants this build knows
 *
 * Read-only: only blob_db_size(), blob_db_read() and the inspect API are
 * called. The formats are in diag_decode.c; nothing here calls a container.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_inspect.h>

#include "diag.h"

/* Ids gathered from a bucket per pass of its sector, as export does. */
#define SCAN_IDS  32
/* References followed per root by `roots` and the scan's labels. */
#define REFS_MAX  8

/* One object at a time: the shell thread is the only user. */
static uint8_t __aligned(4) g_buf[CONFIG_APP_DIAG_BUF_SIZE];

/* What points at an id, from the registry down one level: the labels the
 * scan prints, and the roots it checks are bound. */
struct hint {
	uint64_t id;
	uint64_t key;       /* registry key, for a root                    */
	uint64_t parent;    /* the root it hangs off; 0 for a root          */
	const char *label;  /* field in the parent                          */
	bool seen;
};

static struct hint g_hint[CONFIG_APP_DIAG_HINTS_MAX];
static int g_n_hint;
static bool g_hint_full;

static int parse_u64(const struct shell *sh, const char *s, uint64_t *out)
{
	char *end;

	errno = 0;
	*out = strtoull(s, &end, 0);
	if (errno || end == s || *end != '\0') {
		shell_error(sh, "not a number: %s", s);
		return -EINVAL;
	}
	return 0;
}

static int need_mount(const struct shell *sh, struct blob_db_inspect_info *in)
{
	int rc = blob_db_inspect_info_get(in);

	if (rc == -ENODEV) {
		shell_error(sh, "blob_db not mounted");
	} else if (rc < 0) {
		shell_error(sh, "info: %d", rc);
	}
	return rc;
}

/* Read an object, or its first sizeof(g_buf) bytes, into g_buf. */
static int read_obj(uint64_t id, size_t *size, size_t *n)
{
	int rc = blob_db_size(id, size);

	if (rc < 0) {
		return rc;
	}
	const size_t want = MIN(*size, sizeof(g_buf));
	size_t off = 0;

	while (off < want) {
		size_t got;

		rc = blob_db_read(id, off, g_buf + off, want - off, &got);
		if (rc < 0) {
			return rc;
		}
		if (got == 0) {
			return -EIO;   /* shrank since its size was taken */
		}
		off += got;
	}
	*n = want;
	return 0;
}

/* ---- hints ----------------------------------------------------------- */

static void hint_add(uint64_t id, uint64_t key, uint64_t parent,
		     const char *label)
{
	if (g_n_hint == ARRAY_SIZE(g_hint)) {
		g_hint_full = true;
		return;
	}
	g_hint[g_n_hint++] = (struct hint){
		.id = id, .key = key, .parent = parent, .label = label,
	};
}

static void hint_root(uint64_t key, uint64_t root, void *user)
{
	ARG_UNUSED(user);
	hint_add(root, key, 0, NULL);
}

/* The registry's roots, then what each root points at. Leaves g_buf
 * holding the last root read. */
static void hints_load(void)
{
	size_t size, n;

	g_n_hint = 0;
	g_hint_full = false;
	if (read_obj(BLOB_DB_ROOT_ID, &size, &n) < 0 ||
	    diag_rreg_foreach(g_buf, n, hint_root, NULL) < 0) {
		return;
	}
	const int n_roots = g_n_hint;

	for (int i = 0; i < n_roots; i++) {
		struct diag_class c;
		struct diag_ref refs[REFS_MAX];

		if (read_obj(g_hint[i].id, &size, &n) < 0) {
			continue;
		}
		diag_classify(g_hint[i].id, g_buf, n, size, &c);
		const int k = diag_refs(&c, g_buf, n, refs, ARRAY_SIZE(refs));

		for (int j = 0; j < k; j++) {
			hint_add(refs[j].id, g_hint[i].key, g_hint[i].id,
				 refs[j].label);
		}
	}
}

static struct hint *hint_find(uint64_t id)
{
	for (int i = 0; i < g_n_hint; i++) {
		if (g_hint[i].id == id) {
			return &g_hint[i];
		}
	}
	return NULL;
}

static void key_text(uint64_t key, char *out, size_t sz)
{
	const char *name = diag_rreg_key_name(key);
	char cc[5];

	for (int i = 0; i < 4; i++) {
		const char ch = (char)(key >> (56 - 8 * i));

		cc[i] = (ch >= 0x20 && ch < 0x7f) ? ch : '.';
	}
	cc[4] = '\0';
	snprintk(out, sz, "'%s'/0x%08x%s%s", cc, (uint32_t)key,
		 name ? " " : "", name ? name : "");
}

static void hint_text(const struct hint *h, char *out, size_t sz)
{
	char key[40];

	if (!h) {
		out[0] = '\0';
		return;
	}
	key_text(h->key, key, sizeof(key));
	if (h->parent) {
		snprintk(out, sz, "  <- %llu.%s", (unsigned long long)h->parent,
			 h->label);
	} else {
		snprintk(out, sz, "  <- rootreg %s", key);
	}
}

/* ---- diag scan ------------------------------------------------------- */

struct scan {
	const struct shell *sh;
	bool all;
	uint32_t count[DIAG_T_COUNT][DIAG_VARIANTS_MAX];
	uint64_t bytes[DIAG_T_COUNT][DIAG_VARIANTS_MAX];
	uint32_t objects, suspects, errors, printed;
	uint64_t total;
};

struct id_batch {
	uint64_t ids[SCAN_IDS];
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
	if (b->n == SCAN_IDS) {
		b->more = true;
		return 1;
	}
	b->ids[b->n++] = s->id;
	return 0;
}

static void scan_one(struct scan *s, uint64_t id)
{
	struct diag_class c;
	size_t size, n;
	char suffix[64];
	int rc = read_obj(id, &size, &n);

	if (rc < 0) {
		shell_print(s->sh, "%-6llu unreadable: %d",
			    (unsigned long long)id, rc);
		s->errors++;
		return;
	}
	diag_classify(id, g_buf, n, size, &c);

	struct hint *h = hint_find(id);

	if (h) {
		h->seen = true;
	}
	s->objects++;
	s->total += size;
	s->count[c.type][c.variant]++;
	s->bytes[c.type][c.variant] += size;
	s->suspects += c.suspect;

	/* By default list what heads a structure, and anything odd; the
	 * shape-only guesses and the unknown are counted, not listed. */
	const bool head = !c.shape && c.type != DIAG_T_UNKNOWN &&
			  c.type != DIAG_T_EMPTY;

	if (s->all || head || h || c.suspect || c.type == DIAG_T_PADB_SB) {
		if (s->printed++ == 0) {
			shell_print(s->sh, "%-6s %-8s %-19s %6s  %s", "id",
				    "type", "variant", "len", "fields");
		}
		hint_text(h, suffix, sizeof(suffix));
		diag_summary(s->sh, "", id, &c, g_buf, n, size, suffix);
	}
}

static int cmd_scan(const struct shell *sh, size_t argc, char **argv)
{
	struct blob_db_inspect_info in;
	static struct scan s;   /* the counters are too big for the stack */
	int rc = need_mount(sh, &in);

	if (rc < 0) {
		return rc;
	}
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "-a") != 0)) {
		shell_error(sh, "usage: diag scan [-a]");
		return -EINVAL;
	}
	memset(&s, 0, sizeof(s));
	s.sh = sh;
	s.all = (argc == 2);
	hints_load();

	for (uint16_t bid = 0; bid < in.n_buckets; bid++) {
		for (uint32_t pass = 0;; pass++) {
			struct id_batch b = { .skip = pass * SCAN_IDS };
			struct blob_db_inspect_bucket bs;

			rc = blob_db_inspect_bucket_get(bid, &bs,
							collect_live, &b);
			if (rc == -ENODEV) {
				shell_error(sh, "unmounted during the scan");
				return rc;
			}
			if (rc < 0) {
				shell_print(sh, "bucket %u unreadable: %d", bid,
					    rc);
				s.errors++;
				break;
			}
			/* Read outside the walk: its callback must not call
			 * blob_db. */
			for (uint16_t i = 0; i < b.n; i++) {
				scan_one(&s, b.ids[i]);
			}
			if (!b.more) {
				break;
			}
		}
	}

	shell_print(sh, "");
	shell_print(sh, "%-9s %-19s %7s %10s", "type", "variant", "objects",
		    "bytes");
	for (int t = 0; t < DIAG_T_COUNT; t++) {
		const struct diag_type_info *ti = diag_type_info(t);

		for (int v = 0; v < DIAG_VARIANTS_MAX; v++) {
			if (s.count[t][v] == 0) {
				continue;
			}
			shell_print(sh, "%-9s %-19s %7u %10llu", ti->name,
				    ti->variant[v] ? ti->variant[v] : "",
				    s.count[t][v],
				    (unsigned long long)s.bytes[t][v]);
		}
	}
	shell_print(sh, "%u objects, %llu bytes, %u suspect, %u unreadable; "
		    "next_id %llu", s.objects, (unsigned long long)s.total,
		    s.suspects, s.errors, (unsigned long long)in.next_id);

	/* A registered root or a head's reference that names no object:
	 * normal for a structure created lazily, or one a crash interrupted
	 * before its first bind; a leak's opposite. */
	for (int i = 0; i < g_n_hint; i++) {
		char text[64];

		if (g_hint[i].seen) {
			continue;
		}
		hint_text(&g_hint[i], text, sizeof(text));
		shell_print(sh, "unbound: %llu%s",
			    (unsigned long long)g_hint[i].id, text);
	}
	if (g_hint_full) {
		shell_print(sh, "(more references than "
			    "CONFIG_APP_DIAG_HINTS_MAX; labels incomplete)");
	}
	return 0;
}

/* ---- diag roots ------------------------------------------------------ */

static void print_obj(const struct shell *sh, const char *prefix,
		      uint64_t id, struct diag_class *c, size_t *size,
		      size_t *n)
{
	int rc = read_obj(id, size, n);

	if (rc == -ENOENT) {
		shell_print(sh, "%s%-6llu (unbound)", prefix,
			    (unsigned long long)id);
	} else if (rc < 0) {
		shell_print(sh, "%s%-6llu unreadable: %d", prefix,
			    (unsigned long long)id, rc);
	}
	if (rc < 0) {
		c->type = DIAG_T_COUNT;
		return;
	}
	diag_classify(id, g_buf, *n, *size, c);
	diag_summary(sh, prefix, id, c, g_buf, *n, *size, "");
}

static int cmd_roots(const struct shell *sh, size_t argc, char **argv)
{
	struct blob_db_inspect_info in;
	struct diag_class c;
	size_t size, n;
	int rc = need_mount(sh, &in);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (rc < 0) {
		return rc;
	}
	print_obj(sh, "", BLOB_DB_ROOT_ID, &c, &size, &n);
	if (c.type != DIAG_T_RREG) {
		shell_print(sh, "id 1 holds no root registry");
		return 0;
	}
	hints_load();

	for (int i = 0; i < g_n_hint && g_hint[i].parent == 0; i++) {
		struct diag_ref refs[REFS_MAX];
		char key[40];

		key_text(g_hint[i].key, key, sizeof(key));
		shell_print(sh, "");
		shell_print(sh, "%s", key);
		print_obj(sh, "  ", g_hint[i].id, &c, &size, &n);
		if (c.type == DIAG_T_COUNT) {
			continue;
		}
		const int k = diag_refs(&c, g_buf, n, refs, ARRAY_SIZE(refs));

		for (int j = 0; j < k; j++) {
			char prefix[32];

			snprintk(prefix, sizeof(prefix), "    %-12s",
				 refs[j].label);
			print_obj(sh, prefix, refs[j].id, &c, &size, &n);
		}
	}
	return 0;
}

/* ---- diag show ------------------------------------------------------- */

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	struct blob_db_inspect_info in;
	struct diag_class c;
	enum diag_type t;
	uint64_t id;
	size_t size, n;
	int rc = need_mount(sh, &in);

	if (rc < 0) {
		return rc;
	}
	if (parse_u64(sh, argv[1], &id) < 0) {
		return -EINVAL;
	}
	if (argc > 2 && diag_type_by_name(argv[2], &t) < 0) {
		shell_error(sh, "unknown type %s; see `diag formats`", argv[2]);
		return -EINVAL;
	}
	rc = read_obj(id, &size, &n);
	if (rc == -ENOENT) {
		shell_error(sh, "%llu: not bound", (unsigned long long)id);
		return rc;
	}
	if (rc < 0) {
		shell_error(sh, "%llu: read: %d", (unsigned long long)id, rc);
		return rc;
	}
	if (argc > 2) {
		rc = diag_force(id, t, g_buf, n, size, &c);
		if (rc < 0) {
			shell_error(sh, "%llu: %zu bytes, too short for %s",
				    (unsigned long long)id, size, argv[2]);
			return rc;
		}
	} else {
		diag_classify(id, g_buf, n, size, &c);
	}
	diag_decode(sh, id, &c, g_buf, n, size);

	/* A reference to nothing: a logring's reserved next id at the tail,
	 * a structure created lazily, or damage. */
	struct diag_ref refs[REFS_MAX];
	const int k = diag_refs(&c, g_buf, n, refs, ARRAY_SIZE(refs));

	for (int i = 0; i < k; i++) {
		if (!blob_db_exists(refs[i].id)) {
			shell_print(sh, "  %s %llu is not bound", refs[i].label,
				    (unsigned long long)refs[i].id);
		}
	}
	return 0;
}

/* ---- diag formats ---------------------------------------------------- */

static int cmd_formats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	for (int t = 0; t < DIAG_T_COUNT; t++) {
		const struct diag_type_info *ti = diag_type_info(t);

		shell_print(sh, "%-9s %s", ti->name, ti->what);
		for (int v = 0; v < DIAG_VARIANTS_MAX && ti->variant[v] &&
		     ti->variant[v][0]; v++) {
			shell_print(sh, "%-9s   %s", "", ti->variant[v]);
		}
	}
	shell_print(sh, "Types without a magic (kvbucket, logentry, cbor, and "
		    "persondb) are guessed from their layout: [shape].");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_diag,
	SHELL_CMD_ARG(scan, NULL,
		      "[-a] classify every live object; -a lists all, not "
		      "only structure heads",
		      cmd_scan, 1, 1),
	SHELL_CMD_ARG(roots, NULL,
		      "the root registry, each root and what it points at",
		      cmd_roots, 1, 0),
	SHELL_CMD_ARG(show, NULL,
		      "<id> [type] decode one object, as recognised or as "
		      "<type>",
		      cmd_show, 2, 1),
	SHELL_CMD_ARG(formats, NULL,
		      "the structure formats and past variants this build "
		      "knows",
		      cmd_formats, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(diag, &sub_diag,
		   "recognise and decode the structures in the store", NULL);
