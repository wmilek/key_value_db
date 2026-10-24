/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui — a full-screen person browser with permission dialogs.
 *
 * A frontend, so the same rule as the other two applies: parsing and drawing
 * only, and every action is one call into scenario.h or persondb.h
 * (DESIGN.md F15). What it adds is a way to see a person and change what they
 * may open, which cards they carry, who they are and for how long, without
 * typing ids into a command line.
 *
 *  ┌ Persons ──────────────────────────┐┌ 100042 ───────────────────────┐
 *  │     id  name           dept   P C ││ Anna Kowalska                  │
 *  │ 100042  Anna Kowalska  R&D   18 3 ││ Permissions (18)               │
 *  └───────────────────────────────────┘└────────────────────────────────┘
 *   status: what the last operation did, its return code, its time
 *   keys: Tab cycles list / permissions / cards; a adds and d removes in
 *   the focused pane (persons, permissions, cards); n name, v valid from/to,
 *   g open by id, c open by card, q / Ctrl-C / Ctrl-D back to the shell
 *
 * Three things below are shaped by the store rather than by taste:
 *
 *  - The list is a walk of the people map (persondb_person_next), so it shows
 *    every stored person — dataset or added by hand — in the container's
 *    order: stable, but not sorted. The walk is stateless and forward-only,
 *    keyed by the last id read, so a page is "the 17 persons after this id".
 *    The start of each page visited is remembered (PAGE_CURSORS of them), so
 *    PgUp is free; End and far jumps walk, with progress, and any key stops
 *    them. There is no search by name: it would read every record, ~49 s on
 *    the DK at 8 000 persons. `g` (id) and `c` (card) go straight to one
 *    person instead.
 *
 *  - Every row costs about one persondb_person_get (~6 ms of flash reads on
 *    the DK), so a page is drawn row by row as the walk returns them, and only
 *    a ~44-byte summary per row is kept: the full record (~720 B) is held for
 *    the selected person alone. The total comes from persondb_person_count()
 *    once, at entry (~2 s on the DK), and is kept up to date from there.
 *
 *  - After any write the person is read back from the store, never
 *    patched in RAM. What the screen shows is what was stored — and the
 *    status line reports the return code and the time the write took.
 *
 * A dataset person changed here no longer matches the generator, so `persondb
 * verify` will count it as a bad record (or, deleted, as missing) if it
 * samples that index. That is verify doing its job, not a fault in either.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_log_backend.h>
#include <zephyr/sys/util.h>

#include "scenario.h"
#include "term.h"
#include "tui_widgets.h"
#include "ui_tui.h"

/* -- layout (80x24) ------------------------------------------------------ */

#define TITLE_ROW   1
#define BOX_ROW     2
#define BOX_H       20                  /* rows 2..21 */

#define LIST_COL    1
#define LIST_W      46
#define LIST_HDR    (BOX_ROW + 1)
#define LIST_TOP    (BOX_ROW + 2)       /* first person row */
#define LIST_ROWS   (BOX_H - 3)         /* 17 */

#define DET_COL     (LIST_COL + LIST_W)
#define DET_W       (TERM_COLS - LIST_W)
#define DET_IN      (DET_W - 2)
#define DET_NAME    (BOX_ROW + 1)
#define DET_DEPT    (BOX_ROW + 2)
#define DET_VALID   (BOX_ROW + 3)
#define DET_PHDR    (BOX_ROW + 4)
#define DET_PTOP    (BOX_ROW + 5)
#define PERM_ROWS   8
#define DET_CHDR    (DET_PTOP + PERM_ROWS)
#define DET_CTOP    (DET_CHDR + 1)      /* PERSONDB_CARDS_MAX rows follow */

#define STATUS_ROW  22
#define HELP_ROW    23                  /* two rows: actions, then movement */

/* How long a lone Esc waits to find out whether it starts an arrow key. At
 * 9600 baud the rest of a sequence is 2 ms behind its first byte; 30 ms is
 * imperceptible to a person and generous to a slow link. */
#define ESC_WAIT_MS 30

BUILD_ASSERT(DET_CTOP + PERSONDB_CARDS_MAX <= BOX_ROW + BOX_H - 1,
	     "the card list must fit in the detail box");

/* -- state --------------------------------------------------------------- */

/* Page starts remembered for PgUp: 128 pages is 2 176 persons back, 512 B.
 * Further back costs a walk from the nearest one remembered. */
#define PAGE_CURSORS 128

/** One list row: what the list shows, without the ~720 B record. */
struct row {
	bool     used;          /* a person, not blank space past the end */
	uint32_t id;
	uint8_t  n_perms;
	uint8_t  n_cards;
	char     name[21];
	char     dept[10];
};

/* The three panes Tab cycles through; a and d act on the focused one: persons
 * in the list, permissions, cards. */
enum focus { FOCUS_LIST, FOCUS_PERMS, FOCUS_CARDS };

enum mode {
	MODE_MAIN,
	MODE_PICK,
	MODE_INPUT,
	MODE_CONFIRM,
};

/* What the open dialog was opened for — decides what its answer does. */
enum purpose {
	FOR_GRANT,          /* pick: a permission to grant */
	FOR_GRANT_OTHER,    /* input: a permission name not in the vocabulary */
	FOR_REVOKE_CONFIRM, /* confirm: revoke g.perm */
	FOR_GOTO,           /* input: a person id */
	FOR_CARD,           /* input: a card id, to jump to its holder */
	FOR_RENAME,         /* input: the person's new name */
	FOR_VALID_FROM,     /* input: first valid day, then ... */
	FOR_VALID_UNTIL,    /* input: ... last valid day */
	FOR_CARD_ADD,       /* input: a card id to assign */
	FOR_CARD_REMOVE,    /* confirm: remove g.card */
	FOR_NEW_ID,         /* input: a new person's id, then ... */
	FOR_NEW_NAME,       /* ... name, then ... */
	FOR_NEW_FROM,       /* ... first valid day, then ... */
	FOR_NEW_UNTIL,      /* ... last valid day; written only after this one */
	FOR_DELETE,         /* confirm: delete the selected person */
};

static const char OTHER[] = "<other...>";

static struct {
	bool                 active;
	const struct shell  *sh;
	struct scenario     *s;
	bool                 logs_muted;

	uint32_t             n;          /* persons stored (count at entry,
					  * kept up to date since) */
	uint32_t             populated;  /* dataset fill, for the title */
	uint32_t             n_persons;
	uint32_t             rev;

	uint32_t             top;        /* walk position of the page's first row */
	uint32_t             cur;        /* walk position of the selected row */
	bool                 detached;   /* g.sel was opened by id or card and
					  * is not (known to be) on this page */

	/* The page on screen: the cursor it was read after, and the one the
	 * next page is read after (its last row), when it was full. */
	bool                 shown_valid;
	uint32_t             shown_page;
	bool                 shown_has_after;
	uint32_t             shown_after;
	bool                 next_valid;
	uint32_t             next_after;
	bool                 approx;     /* positions may be off since an edit;
					  * see pages_forget() */
	enum focus           focus;
	uint8_t              perm_cur;
	uint8_t              perm_top;
	uint8_t              card_cur;

	struct row           rows[LIST_ROWS];
	struct persondb_person sel;      /* the selected person, as stored */
	int                  sel_rc;

	enum mode            mode;
	enum purpose         purpose;
	struct tui_pick      pick;
	struct tui_input     input;
	struct tui_confirm   confirm;
	char                 perm[PERSONDB_PERM_MAX + 1];
	char                 card[PERSONDB_CARD_LEN + 1];
	uint32_t             new_from;   /* carried from one validity dialog
					  * to the next */
	uint32_t             new_id;     /* a person being added, staged */
	char                 new_name[PERSONDB_NAME_MAX + 1];

	char                 status[TERM_COLS];
	bool                 status_err;
} g;

/* Reading a page needs a whole person per row; one buffer serves all of them,
 * kept off the shell thread's stack, which blob_db's write path already
 * claims most of (Kconfig, SHELL_STACK_SIZE). */
static struct persondb_person g_scratch;

/* g_page_after[k]: the id of the last person on page k-1, i.e. the cursor
 * that page k is read after. Page 0 is read from the start and needs none. */
static uint32_t g_page_after[PAGE_CURSORS];
static uint8_t  g_page_known[PAGE_CURSORS / 8];

static bool page_known(uint32_t k)
{
	return k == 0 ||
	       (k < PAGE_CURSORS && (g_page_known[k / 8] & BIT(k % 8)));
}

static void page_remember(uint32_t k, uint32_t after)
{
	if (k > 0 && k < PAGE_CURSORS && !g.approx) {
		g_page_after[k] = after;
		g_page_known[k / 8] |= BIT(k % 8);
	}
}

/*
 * After an insert or a delete, forget the remembered page starts.
 *
 * The starts are ids, and an id stays a valid cursor whatever happens to it.
 * What goes stale is the page *boundaries*: insert a person into page j and
 * page j's 17 rows now end one person early, while page j+1 still starts
 * after the old last one — who is then on neither page. So the cache is
 * dropped, and the next jump walks from page 0 and rebuilds it.
 *
 * The page on screen keeps its own start (g.shown_after), so the screen does
 * not jump, and PgDn from it still works; but its position numbers may now be
 * off, so they are shown as approximate until that walk from page 0.
 */
static void pages_forget(void)
{
	memset(g_page_known, 0, sizeof(g_page_known));
	if (g.shown_valid && g.shown_page > 0) {
		g.approx = true;
	}
}

/* -- small helpers ------------------------------------------------------- */

static void set_status(bool err, const char *fmt, ...) __printf_like(2, 3);

static void set_status(bool err, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g.status, sizeof(g.status), fmt, ap);
	va_end(ap);
	g.status_err = err;
}

/* A duration for the status line: microseconds below 10 ms, where a
 * millisecond figure would read "0 ms" for every native_sim operation and
 * hide the difference between a cached and a flash-bound read on the DK. */
static const char *dur_since(int64_t t0_us)
{
	static char buf[24];
	int64_t us = scenario_since_us(t0_us);

	if (us < 10000) {
		snprintf(buf, sizeof(buf), "%lld us", (long long)us);
	} else {
		snprintf(buf, sizeof(buf), "%lld ms", (long long)(us + 500) / 1000);
	}
	return buf;
}

/* Epoch seconds to YYYY-MM-DD, proleptic Gregorian (H. Hinnant's
 * civil_from_days). No libc time functions: the board has no timezone and
 * the validity window is a UTC instant anyway. */
static void fmt_date(char *out, size_t sz, uint32_t epoch)
{
	int32_t z = (int32_t)(epoch / 86400u) + 719468;
	int32_t era = z / 146097;
	uint32_t doe = (uint32_t)(z - era * 146097);
	uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int32_t y = (int32_t)yoe + era * 400;
	uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint32_t mp = (5 * doy + 2) / 153;
	uint32_t d = doy - (153 * mp + 2) / 5 + 1;
	uint8_t m = (uint8_t)(mp < 10 ? mp + 3 : mp - 9);

	snprintf(out, sz, "%04d-%02u-%02u", (int)(y + (m <= 2)), m,
		 (uint8_t)d);
}

/* The inverse: days since 1970-01-01 for a civil date (days_from_civil). */
static int64_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
	y -= m <= 2;
	int32_t era = (y >= 0 ? y : y - 399) / 400;
	uint32_t yoe = (uint32_t)(y - era * 400);
	uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return (int64_t)era * 146097 + doe - 719468;
}

/**
 * @brief Parse "YYYY-MM-DD" to the epoch second its day starts at (UTC).
 *
 * Strict: exactly that shape, a real calendar day, and a result that fits
 * the record's uint32_t. Anything else is refused, not guessed at — a typo
 * in a validity window is a card that silently stops opening doors.
 *
 * @retval 0 parsed; @retval -EINVAL not a date this record can hold
 */
static int parse_date(const char *s, int64_t *epoch)
{
	static const uint8_t dim[] = { 31, 29, 31, 30, 31, 30,
				       31, 31, 30, 31, 30, 31 };
	unsigned int y = 0, m = 0, d = 0;

	for (int i = 0; i < 10; i++) {
		bool dash = (i == 4 || i == 7);

		if (dash ? s[i] != '-' : (s[i] < '0' || s[i] > '9')) {
			return -EINVAL;
		}
	}
	if (s[10] != '\0') {
		return -EINVAL;
	}
	for (int i = 0; i < 4; i++) {
		y = y * 10 + (unsigned int)(s[i] - '0');
	}
	m = (unsigned int)(s[5] - '0') * 10 + (unsigned int)(s[6] - '0');
	d = (unsigned int)(s[8] - '0') * 10 + (unsigned int)(s[9] - '0');

	bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;

	if (m < 1 || m > 12 || d < 1 || d > dim[m - 1] ||
	    (m == 2 && d == 29 && !leap)) {
		return -EINVAL;
	}

	int64_t t = days_from_civil((int32_t)y, m, d) * 86400;

	if (t < 0 || t > UINT32_MAX) {
		return -EINVAL;
	}
	*epoch = t;
	return 0;
}

/* The validity window is [from, until) — until is the first instant that is
 * no longer valid. People think in last valid days, so that is what the
 * screen shows and what the dialog asks for. */
static void fmt_last_day(char *out, size_t sz, uint32_t until)
{
	fmt_date(out, sz, until ? until - 1 : 0);
}

static bool sel_holds(const char *perm)
{
	for (uint8_t i = 0; i < g.sel.n_perms; i++) {
		if (strcmp(g.sel.perm[i], perm) == 0) {
			return true;
		}
	}
	return false;
}

static void summarize(struct row *r, const struct persondb_person *p)
{
	r->used = true;
	r->id = p->id;
	r->n_perms = p->n_perms;
	r->n_cards = p->n_cards;
	/* Cut to what the list column shows; the detail pane has the rest. */
	strncpy(r->name, p->name, sizeof(r->name) - 1);
	r->name[sizeof(r->name) - 1] = '\0';
	strncpy(r->dept, p->dept, sizeof(r->dept) - 1);
	r->dept[sizeof(r->dept) - 1] = '\0';
}

/* -- drawing ------------------------------------------------------------- */

static void draw_title(void)
{
	char left[TERM_COLS];
	static const char *const pane[] = {
		[FOCUS_LIST]  = "[list] ",
		[FOCUS_PERMS] = "[permissions] ",
		[FOCUS_CARDS] = "[cards] ",
	};
	const char *right = pane[g.focus];

	snprintf(left, sizeof(left),
		 " persondb | %u stored | dataset %u/%u | rev %u", g.n,
		 g.populated, g.n_persons, g.rev);
	term_attr(TERM_BOLD_REVERSE);
	term_field(TITLE_ROW, 1, TERM_COLS - (int)strlen(right), left);
	term_puts(right);
	term_attr(TERM_NORMAL);
}

static void draw_row(int i)
{
	const struct row *r = &g.rows[i];
	uint32_t pos = g.top + (uint32_t)i;
	int row = LIST_TOP + i;
	bool sel = r->used && !g.detached && pos == g.cur;

	if (sel) {
		term_attr(g.focus == FOCUS_LIST ? TERM_REVERSE : TERM_BOLD);
	} else {
		term_attr(TERM_NORMAL);
	}

	if (!r->used) {
		term_field(row, LIST_COL + 1, LIST_W - 2, "");
	} else {
		term_fieldf(row, LIST_COL + 1, LIST_W - 2,
			    " %6u  %-20.20s %-9.9s %2u %u", r->id, r->name,
			    r->dept, r->n_perms, r->n_cards);
	}
	term_attr(TERM_NORMAL);
}

static void draw_list_frame(void)
{
	char title[32] = "Persons";

	if (g.n > 0) {
		uint32_t used = 0;

		while (used < LIST_ROWS && g.rows[used].used) {
			used++;
		}
		/* Positions in walk order, from 1; "~" while an edit may have
		 * shifted them (pages_forget). */
		snprintf(title, sizeof(title), "Persons %s%u-%u of %u",
			 g.approx ? "~" : "", g.top + 1,
			 g.top + MAX(used, 1u), g.n);
	}
	term_box(BOX_ROW, LIST_COL, BOX_H, LIST_W, title, NULL, false);
	term_attr(TERM_DIM);
	term_fieldf(LIST_HDR, LIST_COL + 1, LIST_W - 2,
		    " %6s  %-20s %-9s %2s %s", "id", "name", "dept", "P", "C");
	term_attr(TERM_NORMAL);
}

/* A scroll mark in the detail box's right border — "more above / below" —
 * or the border itself when there is nothing more. */
static void border_mark(int row, bool more, char mark)
{
	term_move(row, DET_COL + DET_W - 1);
	if (more) {
		term_printf("%c", mark);
	} else {
		term_puts("\x1b(0x\x1b(B");
	}
}

static void draw_card_row(int i)
{
	int row = DET_CTOP + i;

	if (g.sel_rc != 0 || i >= g.sel.n_cards) {
		term_field(row, DET_COL + 1, DET_IN, "");
		return;
	}
	if (i == g.card_cur && g.focus == FOCUS_CARDS) {
		term_attr(TERM_REVERSE);
	}
	term_fieldf(row, DET_COL + 1, DET_IN, " %c %s",
		    (i == g.card_cur && g.focus == FOCUS_CARDS) ? '>' : ' ',
		    g.sel.card[i]);
	term_attr(TERM_NORMAL);
}

static void draw_perm_row(int i)
{
	uint8_t idx = g.perm_top + (uint8_t)i;
	int row = DET_PTOP + i;

	if (g.sel_rc != 0 || idx >= g.sel.n_perms) {
		term_field(row, DET_COL + 1, DET_IN, "");
		return;
	}
	if (idx == g.perm_cur && g.focus == FOCUS_PERMS) {
		term_attr(TERM_REVERSE);
	}
	term_fieldf(row, DET_COL + 1, DET_IN, " %c %s",
		    idx == g.perm_cur ? '>' : ' ', g.sel.perm[idx]);
	term_attr(TERM_NORMAL);
}

static void draw_detail(void)
{
	char title[24];
	char from[24], until[24];

	if (g.sel_rc == 0) {
		/* "by id": opened with g or c, not picked from this page. */
		snprintf(title, sizeof(title), g.detached ? "%u, by id" : "%u",
			 g.sel.id);
	} else {
		snprintf(title, sizeof(title), "-");
	}
	term_box(BOX_ROW, DET_COL, BOX_H, DET_W, title, NULL, false);

	if (g.sel_rc != 0) {
		for (int r = BOX_ROW + 1; r < BOX_ROW + BOX_H - 1; r++) {
			term_field(r, DET_COL + 1, DET_IN, "");
		}
		if (g.n > 0) {
			term_fieldf(DET_NAME, DET_COL + 1, DET_IN,
				    " not readable: %d", g.sel_rc);
		} else {
			term_field(DET_NAME, DET_COL + 1, DET_IN,
				   " store is empty");
			term_field(DET_DEPT, DET_COL + 1, DET_IN,
				   " a: add a person, or");
			term_field(DET_VALID, DET_COL + 1, DET_IN,
				   " q, then: persondb fill 200");
		}
		return;
	}

	fmt_date(from, sizeof(from), g.sel.valid_from);
	fmt_last_day(until, sizeof(until), g.sel.valid_until);

	term_attr(TERM_BOLD);
	term_fieldf(DET_NAME, DET_COL + 1, DET_IN, " %s", g.sel.name);
	term_attr(TERM_NORMAL);
	if (g.sel.dept[0] != '\0' || g.sel.title[0] != '\0') {
		term_fieldf(DET_DEPT, DET_COL + 1, DET_IN, " %s / %s",
			    g.sel.dept, g.sel.title);
	} else {
		term_field(DET_DEPT, DET_COL + 1, DET_IN, " (no department)");
	}
	term_fieldf(DET_VALID, DET_COL + 1, DET_IN, " valid %s .. %s", from,
		    until);

	term_attr(TERM_DIM);
	term_fieldf(DET_PHDR, DET_COL + 1, DET_IN, " Permissions (%u)%s",
		    g.sel.n_perms,
		    g.sel.n_perms > PERM_ROWS ? "  PgUp/PgDn" : "");
	term_attr(TERM_NORMAL);
	for (int i = 0; i < PERM_ROWS; i++) {
		draw_perm_row(i);
	}
	border_mark(DET_PTOP, g.perm_top > 0, '^');
	border_mark(DET_PTOP + PERM_ROWS - 1,
		    g.perm_top + PERM_ROWS < g.sel.n_perms, 'v');

	term_attr(TERM_DIM);
	term_fieldf(DET_CHDR, DET_COL + 1, DET_IN, " Cards (%u)", g.sel.n_cards);
	term_attr(TERM_NORMAL);
	for (int i = 0; i < PERSONDB_CARDS_MAX; i++) {
		draw_card_row(i);
	}
}

static void draw_status(void)
{
	term_attr(g.status_err ? TERM_BOLD : TERM_NORMAL);
	term_fieldf(STATUS_ROW, 1, TERM_COLS, " %s", g.status);
	term_attr(TERM_NORMAL);
}

static void draw_help(void)
{
	static const char *const act[] = {
		[FOCUS_LIST]  = " a add person  d delete  n name  v valid"
				" from/to  g goto id  c card  q shell",
		[FOCUS_PERMS] = " a grant  d revoke  n name  v valid from/to"
				"  g goto id  c find card  q shell",
		[FOCUS_CARDS] = " a add card  d remove card  n name"
				"  v valid from/to  g goto id  c find  q shell",
	};
	static const char *const move[] = {
		[FOCUS_LIST]  = " arrows/PgUp/PgDn/Home move  End walks to the"
				" last  Tab next pane  r reload",
		[FOCUS_PERMS] = " arrows/PgUp/PgDn move  Tab next pane  d/Del"
				" revokes the selected permission",
		[FOCUS_CARDS] = " Up/Dn move  Tab next pane  d/Del removes the"
				" selected card",
	};

	term_attr(TERM_REVERSE);
	term_field(HELP_ROW, 1, TERM_COLS, act[g.focus]);
	/* 79 columns, not 80: writing the bottom-right cell makes some
	 * terminals scroll the whole screen up a line. */
	term_field(HELP_ROW + 1, 1, TERM_COLS - 1, move[g.focus]);
	term_attr(TERM_NORMAL);
}

static void draw_dialog(void)
{
	switch (g.mode) {
	case MODE_PICK:    tui_pick_draw(&g.pick); break;
	case MODE_INPUT:   tui_input_draw(&g.input); break;
	case MODE_CONFIRM: tui_confirm_draw(&g.confirm); break;
	case MODE_MAIN:
	default:           break;
	}
}

/** Everything, from the cache: no store reads. */
static void draw_all(void)
{
	term_cursor(false);
	term_attr(TERM_NORMAL);
	term_clear();
	draw_title();
	draw_list_frame();
	for (int i = 0; i < LIST_ROWS; i++) {
		draw_row(i);
	}
	draw_detail();
	draw_status();
	draw_help();
	draw_dialog();
	term_flush();
}

/* -- reading the store --------------------------------------------------- */

/** Keep the permission and card cursors on held items, and in view. */
static void cursor_clamp(void)
{
	if (g.sel_rc != 0 || g.card_cur >= g.sel.n_cards) {
		g.card_cur = (g.sel_rc == 0 && g.sel.n_cards) ?
			     g.sel.n_cards - 1 : 0;
	}
	if (g.sel_rc != 0 || g.sel.n_perms == 0) {
		g.perm_cur = 0;
		g.perm_top = 0;
		return;
	}
	if (g.perm_cur >= g.sel.n_perms) {
		g.perm_cur = g.sel.n_perms - 1;
	}
	if (g.perm_cur < g.perm_top) {
		g.perm_top = g.perm_cur;
	} else if (g.perm_cur >= g.perm_top + PERM_ROWS) {
		g.perm_top = g.perm_cur - PERM_ROWS + 1;
	}
	if (g.perm_top + PERM_ROWS > g.sel.n_perms) {
		/* Shrunk by a revoke: don't leave blank rows at the bottom
		 * while permissions above are scrolled out of view. */
		g.perm_top = g.sel.n_perms > PERM_ROWS ?
			     g.sel.n_perms - PERM_ROWS : 0;
	}
}

/** Re-read the selected person into g.sel, and refresh its list row. */
static void load_sel(void)
{
	uint32_t id;

	if (g.detached) {
		id = g.sel.id;
	} else if (g.n > 0 && g.cur >= g.top && g.cur < g.top + LIST_ROWS &&
		   g.rows[g.cur - g.top].used) {
		id = g.rows[g.cur - g.top].id;
	} else {
		g.sel_rc = -ENOENT;
		cursor_clamp();
		return;
	}
	g.sel_rc = persondb_person_get(g.s->db, id, &g.sel);
	if (!g.detached && g.sel_rc == 0) {
		summarize(&g.rows[g.cur - g.top], &g.sel);
	}
	cursor_clamp();
}

/* Any byte from the terminal: the operator wants a long walk to stop. Read
 * straight from the transport, as bypass_cb does for Esc; the byte itself is
 * dropped — it was a request to stop, not a command. */
static bool key_waiting(void)
{
	uint8_t b[8];
	size_t got = 0;

	return g.sh->iface->api->read(g.sh->iface, b, sizeof(b), &got) == 0 &&
	       got > 0;
}

/**
 * @brief Walk to the start of page @p want.
 *
 * Starts from the nearest page start remembered at or before it, reads and
 * discards whole pages up to it, and remembers each start passed. Stops early
 * at the end of the store, or when a key is pressed.
 *
 * @param[out] has_after false: the page is read from the start of the walk
 * @param[out] after     otherwise, the id it is read after
 * @return the page reached, @p want or earlier
 */
static uint32_t seek_page(uint32_t want, bool *has_after, uint32_t *after)
{
	uint32_t shown = g.shown_page;
	int64_t t0 = scenario_now_us();
	uint32_t walked = 0;

	/* The page on screen and the one after it are known without a cache:
	 * reload, and PgDn, cost no walk — even while positions are approx. */
	if (g.shown_valid && want == shown) {
		*has_after = g.shown_has_after;
		*after = g.shown_after;
		return want;
	}
	if (g.shown_valid && want == shown + 1 && g.next_valid) {
		*has_after = true;
		*after = g.next_after;
		return want;
	}
	if (g.approx) {
		/* Anywhere else: walk from page 0, which makes the positions
		 * exact again and rebuilds the cache as it goes. */
		g.approx = false;
		memset(g_page_known, 0, sizeof(g_page_known));
	}

	uint32_t k = MIN(want, (uint32_t)PAGE_CURSORS - 1);

	while (!page_known(k)) {
		k--;
	}
	*has_after = (k > 0);
	*after = (k > 0) ? g_page_after[k] : 0;

	while (k < want) {
		bool has = *has_after;
		uint32_t id = *after;
		int rc = 0;

		for (int i = 0; i < LIST_ROWS && rc == 0; i++) {
			rc = persondb_person_next(g.s->db, has ? &id : NULL,
						  &g_scratch);
			if (rc == 0) {
				id = g_scratch.id;
				has = true;
				walked++;
			}
		}
		if (rc != 0) {
			break;          /* page k is the last one */
		}
		k++;
		*has_after = true;
		*after = id;
		page_remember(k, id);

		if (want - k > 1) {
			set_status(false, "walking: %u persons read in %s ..."
				   " any key stops", walked, dur_since(t0));
			draw_status();
			term_flush();
			if (key_waiting()) {
				set_status(false, "stopped after %u persons",
					   walked);
				break;
			}
		}
	}
	return k;
}

/**
 * @brief Read and draw the page that g.top falls on, one row at a time.
 *
 * Each row is flushed to the terminal as soon as it is read, so a slow page
 * fills in visibly instead of freezing the screen for the whole of it.
 */
static void load_page(void)
{
	bool has;
	uint32_t after;
	uint32_t page = seek_page(g.top / LIST_ROWS, &has, &after);
	int64_t t0 = scenario_now_us();
	int rows = 0;
	int rc = 0;

	g.top = page * LIST_ROWS;
	g.shown_valid = true;
	g.shown_page = page;
	g.shown_has_after = has;
	g.shown_after = after;
	memset(g.rows, 0, sizeof(g.rows));
	draw_list_frame();
	for (int i = 0; i < LIST_ROWS; i++) {
		draw_row(i);
	}
	term_flush();

	for (; rows < LIST_ROWS; rows++) {
		rc = persondb_person_next(g.s->db, has ? &after : NULL,
					  &g_scratch);
		if (rc != 0) {
			break;
		}
		after = g_scratch.id;
		has = true;
		summarize(&g.rows[rows], &g_scratch);
		if (!g.detached && g.top + rows == g.cur) {
			g.sel = g_scratch;
			g.sel_rc = 0;
		}
		draw_row(rows);
		term_flush();
	}
	g.next_valid = (rows == LIST_ROWS);
	g.next_after = after;
	draw_list_frame();      /* the title counts the rows just read */
	if (rows == LIST_ROWS) {
		page_remember(page + 1, after);
	}

	if (rc != 0 && rc != -ENODATA) {
		set_status(true, "reading the store failed | rc %d", rc);
		return;
	}
	if (rows == 0 && page > 0) {
		/* The walk ended exactly at a page boundary — the last page was
		 * full, or a delete just emptied this one. Show the one before. */
		g.top -= LIST_ROWS;
		load_page();
		return;
	}
	if (rows < LIST_ROWS && !g.approx) {
		/* The walk ended on this page, so the true total is known here
		 * whatever the count said — when the page's position is. */
		g.n = g.top + (uint32_t)rows;
		draw_title();
		draw_list_frame();
	}
	if (!g.detached) {
		if (g.n == 0) {
			g.sel_rc = -ENOENT;
		} else if (g.cur < g.top || g.cur >= g.top + (uint32_t)rows) {
			g.cur = MIN(MAX(g.cur, g.top), g.top + rows - 1);
			load_sel();
			draw_row((int)(g.cur - g.top));
		}
	}
	cursor_clamp();   /* a reload may find fewer permissions than before */
	if (g.n == 0) {
		set_status(false, "the store holds no persons yet");
	} else {
		set_status(false, "read %d persons in %s", rows, dur_since(t0));
	}
}

/** Select walk position @p pos, paging if it is off the current page. */
static void select_pos(uint32_t pos)
{
	if (g.n == 0) {
		return;
	}
	pos = MIN(pos, g.n - 1);

	uint32_t old = g.cur;
	bool was_detached = g.detached;
	uint32_t top = pos - pos % LIST_ROWS;

	g.cur = pos;
	g.detached = false;
	g.perm_cur = 0;
	g.perm_top = 0;
	g.card_cur = 0;

	if (top != g.top) {
		g.top = top;
		load_page();
	} else if (pos != old || was_detached) {
		int64_t t0 = scenario_now_us();

		draw_row((int)(old - g.top));
		load_sel();
		draw_row((int)(pos - g.top));
		if (g.sel_rc == 0) {
			set_status(false, "read %u in %s", g.sel.id,
				   dur_since(t0));
		} else {
			set_status(true, "read failed: %d", g.sel_rc);
		}
	}
	draw_detail();
	draw_status();
}

/**
 * @brief Show person @p id: on its row if it is on this page, otherwise in
 *        the detail pane alone ("by id") — finding its row would mean walking.
 */
static void show_person(uint32_t id)
{
	for (int i = 0; i < LIST_ROWS; i++) {
		if (g.rows[i].used && g.rows[i].id == id) {
			select_pos(g.top + (uint32_t)i);
			return;
		}
	}

	int rc = persondb_person_get(g.s->db, id, &g_scratch);

	if (rc != 0) {
		set_status(true, rc == -ENOENT ? "no person %u is stored"
				 : "person %u: read failed", id);
		return;
	}
	if (g.n > 0 && !g.detached) {
		uint32_t old = g.cur;

		g.detached = true;
		draw_row((int)(old - g.top));
	}
	g.detached = true;
	g.sel = g_scratch;
	g.sel_rc = 0;
	g.perm_cur = 0;
	g.perm_top = 0;
	g.card_cur = 0;
	cursor_clamp();
	draw_detail();
}

/* -- the two writes ------------------------------------------------------ */

static void do_grant(const char *perm)
{
	uint32_t id = g.sel.id;

	if (sel_holds(perm)) {
		set_status(false, "%u already holds '%s'", id, perm);
		return;
	}

	set_status(false, "granting '%s' to %u ...", perm, id);
	draw_status();
	term_flush();

	int64_t t0 = scenario_now_us();
	int rc = persondb_permission_grant(g.s->db, id, perm);
	char took[24];

	snprintf(took, sizeof(took), "%s", dur_since(t0));

	/* Read back whatever happened — including after a failure, which may
	 * have been a store that changed under us. */
	load_sel();

	switch (rc) {
	case 0:
		/* Put the cursor on what was just granted. */
		for (uint8_t i = 0; g.sel_rc == 0 && i < g.sel.n_perms; i++) {
			if (strcmp(g.sel.perm[i], perm) == 0) {
				g.perm_cur = i;
			}
		}
		cursor_clamp();
		set_status(false, "granted '%s' to %u | %s", perm, id, took);
		break;
	case -EINVAL:
		set_status(true, "a permission name is 1..%d characters | rc %d",
			   PERSONDB_PERM_MAX, rc);
		break;
	case -ENOSPC:
		set_status(true, "%u already holds %d permissions, the maximum"
			   " | rc %d", id, PERSONDB_PERMS_MAX, rc);
		break;
	case -ENOENT:
		set_status(true, "person %u is not in the store | rc %d", id, rc);
		break;
	default:
		set_status(true, "grant '%s' to %u failed | rc %d | %s",
			   perm, id, rc, took);
		break;
	}
}

static void do_revoke(const char *perm)
{
	uint32_t id = g.sel.id;

	set_status(false, "revoking '%s' from %u ...", perm, id);
	draw_status();
	term_flush();

	int64_t t0 = scenario_now_us();
	int rc = persondb_permission_revoke(g.s->db, id, perm);
	char took[24];

	snprintf(took, sizeof(took), "%s", dur_since(t0));

	load_sel();

	switch (rc) {
	case 0:
		set_status(false, "revoked '%s' from %u | %s", perm, id, took);
		break;
	case -ENOENT:
		set_status(true, "%u does not hold '%s' | rc %d", id, perm, rc);
		break;
	default:
		set_status(true, "revoke '%s' from %u failed | rc %d | %s",
			   perm, id, rc, took);
		break;
	}
}

static void do_rename(const char *name)
{
	uint32_t id = g.sel.id;

	set_status(false, "renaming %u ...", id);
	draw_status();
	term_flush();

	int64_t t0 = scenario_now_us();
	int rc = persondb_person_rename(g.s->db, id, name);
	char took[24];

	snprintf(took, sizeof(took), "%s", dur_since(t0));
	load_sel();

	switch (rc) {
	case 0:
		set_status(false, "%u is now '%s' | %s", id, g.sel.name, took);
		break;
	case -EINVAL:
		set_status(true, "a name is 1..%d characters | rc %d",
			   PERSONDB_NAME_MAX, rc);
		break;
	default:
		set_status(true, "rename %u failed | rc %d | %s", id, rc, took);
		break;
	}
}

static void do_set_validity(uint32_t from, uint32_t until)
{
	uint32_t id = g.sel.id;

	set_status(false, "setting validity of %u ...", id);
	draw_status();
	term_flush();

	int64_t t0 = scenario_now_us();
	int rc = persondb_person_set_validity(g.s->db, id, from, until);
	char took[24];

	snprintf(took, sizeof(took), "%s", dur_since(t0));
	load_sel();

	switch (rc) {
	case 0: {
		char f[24], u[24];

		fmt_date(f, sizeof(f), g.sel.valid_from);
		fmt_last_day(u, sizeof(u), g.sel.valid_until);
		set_status(false, "%u valid %s .. %s | %s", id, f, u, took);
		break;
	}
	case -EINVAL:
		set_status(true, "the last day cannot be before the first | rc %d",
			   rc);
		break;
	default:
		set_status(true, "set validity of %u failed | rc %d | %s", id,
			   rc, took);
		break;
	}
}

static void do_card_add(const char *card)
{
	uint32_t id = g.sel.id;
	bool listed = false;

	for (uint8_t i = 0; i < g.sel.n_cards; i++) {
		listed |= (strcmp(g.sel.card[i], card) == 0);
	}

	set_status(false, "assigning card %s to %u ...", card, id);
	draw_status();
	term_flush();

	/* Called even when the record already lists the card: assignment is
	 * the repair for a crash between its two writes, and completes a
	 * missing index entry rather than reporting the card as present. */
	int64_t t0 = scenario_now_us();
	int rc = persondb_card_assign(g.s->db, id, card);
	char took[24];

	snprintf(took, sizeof(took), "%s", dur_since(t0));
	load_sel();

	switch (rc) {
	case 0:
		for (uint8_t i = 0; g.sel_rc == 0 && i < g.sel.n_cards; i++) {
			if (strcmp(g.sel.card[i], card) == 0) {
				g.card_cur = i;
			}
		}
		if (listed) {
			set_status(false, "%u already holds card %s | %s", id,
				   card, took);
		} else {
			set_status(false, "card %s assigned to %u | %s", card,
				   id, took);
		}
		break;
	case -EEXIST: {
		uint32_t owner;

		/* Say who: the operator has to go there to revoke it. */
		if (persondb_card_owner(g.s->db, card, &owner) == 0) {
			set_status(true, "card %s is held by %u; remove it there"
				   " first | rc %d", card, owner, rc);
		} else {
			set_status(true, "card %s is held by someone else | rc %d",
				   card, rc);
		}
		break;
	}
	case -ENOSPC:
		set_status(true, "%u already holds %d cards, the maximum | rc %d",
			   id, PERSONDB_CARDS_MAX, rc);
		break;
	case -EINVAL:
		set_status(true, "a card id is 1..%d characters | rc %d",
			   PERSONDB_CARD_LEN, rc);
		break;
	default:
		set_status(true, "assign card %s to %u failed | rc %d | %s",
			   card, id, rc, took);
		break;
	}
}

static void do_card_remove(const char *card)
{
	uint32_t id = g.sel.id;

	set_status(false, "removing card %s from %u ...", card, id);
	draw_status();
	term_flush();

	/* The owner-qualified revoke, because the person is known: a removal
	 * interrupted between its two writes is finished by doing it again.
	 * The card-only form loses the person after its first write. */
	int64_t t0 = scenario_now_us();
	int rc = persondb_card_revoke_from(g.s->db, id, card);
	char took[24];

	snprintf(took, sizeof(took), "%s", dur_since(t0));
	load_sel();

	if (rc == 0) {
		set_status(false, "card %s removed from %u | %s", card, id, took);
	} else {
		set_status(true, "remove card %s from %u failed | rc %d | %s",
			   card, id, rc, took);
	}
}

static void do_person_add(uint32_t until)
{
	struct persondb_person *p = &g_scratch;

	memset(p, 0, sizeof(*p));
	p->id = g.new_id;
	memcpy(p->name, g.new_name, sizeof(p->name));
	p->valid_from = g.new_from;
	p->valid_until = until;

	set_status(false, "adding %u ...", p->id);
	draw_status();
	term_flush();

	int64_t t0 = scenario_now_us();
	int rc = scenario_person_add(g.s, p);
	char took[24];

	snprintf(took, sizeof(took), "%s", dur_since(t0));

	switch (rc) {
	case 0:
		g.n++;
		pages_forget();
		/* Re-read the page on screen, so the start of the next one is
		 * taken from what it now holds. Where the walk puts the new
		 * person is unknown without walking, so open them by id. */
		load_page();
		show_person(g.new_id);
		set_status(false, "added %u '%s' | %s", g.new_id, g.new_name,
			   took);
		break;
	case -ERANGE:
		set_status(true, "ids %u..%u belong to the dataset | rc %d",
			   scenario_id_of(0), scenario_id_of(g.n_persons) - 1, rc);
		break;
	case -EEXIST:
		set_status(true, "id %u is taken | rc %d", g.new_id, rc);
		break;
	case -EINVAL:
		set_status(true, "the last day cannot be before the first | rc %d",
			   rc);
		break;
	default:
		set_status(true, "add %u failed | rc %d | %s", g.new_id, rc, took);
		break;
	}
}

static void do_person_delete(void)
{
	uint32_t id = g.sel.id;

	set_status(false, "deleting %u ...", id);
	draw_status();
	term_flush();

	int64_t t0 = scenario_now_us();
	int rc = persondb_person_delete(g.s->db, id);
	char took[24];

	snprintf(took, sizeof(took), "%s", dur_since(t0));

	if (rc != 0) {
		load_sel();
		set_status(true, "delete %u failed | rc %d | %s", id, rc, took);
		return;
	}
	g.n = g.n ? g.n - 1 : 0;
	g.detached = false;
	pages_forget();
	load_page();    /* re-reads the page from its start; draws as it goes */
	set_status(false, "deleted %u and their cards | %s", id, took);
}

/* -- dialogs ------------------------------------------------------------- */

static void open_input(enum purpose why, const char *title, const char *prompt,
		       uint8_t max, const char *initial)
{
	memset(&g.input, 0, sizeof(g.input));
	g.input.title = title;
	g.input.prompt = prompt;
	g.input.max = MIN(max, TUI_INPUT_MAX);
	if (initial != NULL) {
		/* Pre-filled with the current value: editing a name or a date
		 * is usually a small change, not a retype. */
		strncpy(g.input.buf, initial, g.input.max);
		g.input.buf[g.input.max] = '\0';
		g.input.len = (uint8_t)strlen(g.input.buf);
	}
	g.purpose = why;
	g.mode = MODE_INPUT;
	tui_input_draw(&g.input);
}

static void open_grant(void)
{
	static char title[32];

	if (g.sel_rc != 0) {
		set_status(true, "no person selected");
		draw_status();
		return;
	}

	memset(&g.pick, 0, sizeof(g.pick));
	snprintf(title, sizeof(title), "Grant to %u", g.sel.id);
	g.pick.title = title;

	/* The vocabulary, less what is already held: offering a permission
	 * the person has would make Enter a no-op that looks like a success. */
	for (uint8_t i = 0; g.pick.n < TUI_PICK_MAX - 1; i++) {
		const char *name = scenario_permission_name(i);

		if (name == NULL) {
			break;
		}
		if (!sel_holds(name)) {
			g.pick.item[g.pick.n++] = name;
		}
	}
	g.pick.item[g.pick.n++] = OTHER;

	g.purpose = FOR_GRANT;
	g.mode = MODE_PICK;
	tui_pick_draw(&g.pick);
}

static void open_revoke_confirm(const char *perm)
{
	snprintf(g.perm, sizeof(g.perm), "%s", perm);
	g.confirm.title = "Revoke permission";
	g.confirm.detail[0] = '\0';
	snprintf(g.confirm.msg, sizeof(g.confirm.msg), "Revoke '%s' from %u?",
		 g.perm, g.sel.id);
	g.purpose = FOR_REVOKE_CONFIRM;
	g.mode = MODE_CONFIRM;
	tui_confirm_draw(&g.confirm);
}

static void open_revoke(void)
{
	if (g.sel_rc != 0) {
		set_status(true, "no person selected");
		draw_status();
		return;
	}
	if (g.sel.n_perms == 0) {
		set_status(false, "%u holds no permissions", g.sel.id);
		draw_status();
		return;
	}
	/* The cursor already says which one. */
	open_revoke_confirm(g.sel.perm[g.perm_cur]);
}

static bool need_sel(void)
{
	if (g.sel_rc != 0) {
		set_status(true, "no person selected");
		draw_status();
		return false;
	}
	return true;
}

static void open_card_add(void)
{
	static char title[32];

	if (!need_sel()) {
		return;
	}
	snprintf(title, sizeof(title), "Add card to %u", g.sel.id);
	open_input(FOR_CARD_ADD, title, "Card id (up to 14 characters):",
		   PERSONDB_CARD_LEN, NULL);
}

static void open_card_remove(void)
{
	if (!need_sel()) {
		return;
	}
	if (g.sel.n_cards == 0) {
		set_status(false, "%u holds no cards", g.sel.id);
		draw_status();
		return;
	}
	snprintf(g.card, sizeof(g.card), "%s", g.sel.card[g.card_cur]);
	g.confirm.title = "Remove card";
	g.confirm.detail[0] = '\0';
	snprintf(g.confirm.msg, sizeof(g.confirm.msg),
		 "Remove card %s from %u?", g.card, g.sel.id);
	g.purpose = FOR_CARD_REMOVE;
	g.mode = MODE_CONFIRM;
	tui_confirm_draw(&g.confirm);
}

static void open_rename(void)
{
	static char title[32];

	if (!need_sel()) {
		return;
	}
	snprintf(title, sizeof(title), "Rename %u", g.sel.id);
	open_input(FOR_RENAME, title, "Name:", PERSONDB_NAME_MAX, g.sel.name);
}

static void open_validity(void)
{
	static char title[32];
	char from[24];

	if (!need_sel()) {
		return;
	}
	snprintf(title, sizeof(title), "Valid from/to, %u", g.sel.id);
	fmt_date(from, sizeof(from), g.sel.valid_from);
	open_input(FOR_VALID_FROM, title, "First valid day (YYYY-MM-DD, UTC):",
		   10, from);
}

static void open_person_add(void)
{
	char suggest[16] = "";
	uint32_t id;

	/* A suggestion, not a reservation: the id is checked again when the
	 * person is written. */
	if (scenario_free_id(g.s, &id) == 0) {
		snprintf(suggest, sizeof(suggest), "%u", id);
	}
	open_input(FOR_NEW_ID, "Add person", "Id (outside the dataset range):",
		   10, suggest);
}

static void open_person_delete(void)
{
	if (!need_sel()) {
		return;
	}
	g.confirm.title = "Delete person";
	snprintf(g.confirm.msg, sizeof(g.confirm.msg), "Delete person %u?",
		 g.sel.id);
	snprintf(g.confirm.detail, sizeof(g.confirm.detail),
		 "%.24s, %u card%s", g.sel.name, g.sel.n_cards,
		 g.sel.n_cards == 1 ? "" : "s");
	g.purpose = FOR_DELETE;
	g.mode = MODE_CONFIRM;
	tui_confirm_draw(&g.confirm);
}

static void close_dialog(void);

/* The add dialogs, one step each. Nothing is written until the last one: Esc
 * at any step leaves the store as it was. Each returns true when it opened
 * the next dialog. */

static bool new_id_done(const char *text)
{
	char *end;
	unsigned long v = strtoul(text, &end, 10);
	struct persondb_person *p = &g_scratch;

	if (text[0] == '\0' || *end != '\0' || v > UINT32_MAX) {
		set_status(true, "'%s' is not an id", text);
		return false;
	}
	if (scenario_id_reserved(g.s, (uint32_t)v)) {
		set_status(true, "ids %u..%u belong to the dataset",
			   scenario_id_of(0), scenario_id_of(g.n_persons) - 1);
		return false;
	}
	/* Asked now, so a taken id fails before the name is typed; the write
	 * asks again. */
	if (persondb_person_get(g.s->db, (uint32_t)v, p) == 0) {
		set_status(true, "id %lu is taken by '%s'", v, p->name);
		return false;
	}
	g.new_id = (uint32_t)v;
	close_dialog();
	open_input(FOR_NEW_NAME, "Add person", "Name:", PERSONDB_NAME_MAX, NULL);
	return true;
}

static bool new_name_done(const char *text)
{
	char from[24];

	if (text[0] == '\0') {
		set_status(true, "a name is 1..%d characters", PERSONDB_NAME_MAX);
		return false;
	}
	snprintf(g.new_name, sizeof(g.new_name), "%s", text);
	/* The board has no clock. SCENARIO_BENCH_NOW is the instant `check`
	 * decides at, so it is this application's "today". */
	fmt_date(from, sizeof(from), SCENARIO_BENCH_NOW);
	close_dialog();
	open_input(FOR_NEW_FROM, "Add person",
		   "First valid day (YYYY-MM-DD, UTC):", 10, from);
	return true;
}

static bool new_from_done(const char *text)
{
	int64_t t;
	char until[24];

	if (parse_date(text, &t) != 0) {
		set_status(true, "'%s' is not a date: YYYY-MM-DD", text);
		return false;
	}
	g.new_from = (uint32_t)t;
	/* A year, as a default to edit: last day = first day + 364. */
	fmt_date(until, sizeof(until),
		 (uint32_t)MIN((int64_t)t + 364 * 86400, (int64_t)UINT32_MAX));
	close_dialog();
	open_input(FOR_NEW_UNTIL, "Add person",
		   "Last valid day (YYYY-MM-DD, UTC):", 10, until);
	return true;
}

static void new_until_done(const char *text)
{
	int64_t t;

	if (parse_date(text, &t) != 0 || t + 86400 > UINT32_MAX) {
		set_status(true, "'%s' is not a date: YYYY-MM-DD", text);
		return;
	}
	do_person_add((uint32_t)(t + 86400));
}

/* A field left as it was pre-filled keeps the stored instant exactly. The
 * stored window may start or end mid-day (the generated population does),
 * and re-parsing the date shown would move it to midnight unasked. */

/** @return true if the second dialog is now open */
static bool validity_from_done(const char *text)
{
	char shown[24];
	int64_t t;

	fmt_date(shown, sizeof(shown), g.sel.valid_from);
	if (strcmp(text, shown) == 0) {
		g.new_from = g.sel.valid_from;
	} else if (parse_date(text, &t) == 0) {
		g.new_from = (uint32_t)t;
	} else {
		set_status(true, "'%s' is not a date: YYYY-MM-DD", text);
		return false;
	}

	char until[24];
	const char *title = g.input.title;

	fmt_last_day(until, sizeof(until), g.sel.valid_until);
	close_dialog();
	open_input(FOR_VALID_UNTIL, title, "Last valid day (YYYY-MM-DD, UTC):",
		   10, until);
	return true;
}

static void validity_until_done(const char *text)
{
	char shown[24];
	int64_t t;
	uint32_t until;

	fmt_last_day(shown, sizeof(shown), g.sel.valid_until);
	if (strcmp(text, shown) == 0) {
		until = g.sel.valid_until;
	} else if (parse_date(text, &t) == 0 && t + 86400 <= UINT32_MAX) {
		/* Valid through the whole of that day. */
		until = (uint32_t)(t + 86400);
	} else {
		set_status(true, "'%s' is not a date: YYYY-MM-DD", text);
		return;
	}
	do_set_validity(g.new_from, until);
}

static void close_dialog(void)
{
	g.mode = MODE_MAIN;
	draw_all();
}

static void goto_person(const char *text)
{
	char *end;
	unsigned long v = strtoul(text, &end, 10);

	if (text[0] == '\0' || *end != '\0' || v > UINT32_MAX) {
		set_status(true, "'%s' is not an id", text);
		return;
	}
	show_person((uint32_t)v);
}

static void goto_card(const char *card)
{
	uint32_t owner;
	int64_t t0 = scenario_now_us();
	int rc = persondb_card_owner(g.s->db, card, &owner);
	char took[24];

	snprintf(took, sizeof(took), "%s", dur_since(t0));
	if (rc == -ENOENT) {
		set_status(true, "card %s is not assigned | %s", card, took);
		return;
	}
	if (rc != 0) {
		set_status(true, "card %s: rc %d", card, rc);
		return;
	}
	show_person(owner);
	if (g.sel_rc == 0 && g.sel.id == owner) {
		set_status(false, "card %s -> person %u | %s", card, owner, took);
	}
}

static void dialog_key(int key)
{
	enum tui_outcome o;

	switch (g.mode) {
	case MODE_PICK:    o = tui_pick_key(&g.pick, key); break;
	case MODE_INPUT:   o = tui_input_key(&g.input, key); break;
	case MODE_CONFIRM: o = tui_confirm_key(&g.confirm, key); break;
	default:           return;
	}

	if (o == TUI_OPEN) {
		return;
	}
	if (o == TUI_CANCEL) {
		set_status(false, "cancelled");
		close_dialog();
		return;
	}

	switch (g.purpose) {
	case FOR_GRANT: {
		const char *pick = g.pick.item[g.pick.cur];

		if (pick == OTHER) {
			/* One dialog at a time: uncover the screen, then
			 * open the next. */
			close_dialog();
			open_input(FOR_GRANT_OTHER, g.pick.title,
				   "Permission name:", PERSONDB_PERM_MAX, NULL);
			return;
		}
		do_grant(pick);
		break;
	}
	case FOR_GRANT_OTHER:
		do_grant(g.input.buf);
		break;
	case FOR_REVOKE_CONFIRM:
		do_revoke(g.perm);
		break;
	case FOR_GOTO:
		close_dialog();
		goto_person(g.input.buf);
		draw_status();
		return;
	case FOR_CARD:
		close_dialog();
		goto_card(g.input.buf);
		draw_status();
		return;
	case FOR_RENAME:
		do_rename(g.input.buf);
		break;
	case FOR_VALID_FROM:
		if (validity_from_done(g.input.buf)) {
			return;     /* the second dialog is open */
		}
		break;
	case FOR_VALID_UNTIL:
		validity_until_done(g.input.buf);
		break;
	case FOR_CARD_ADD:
		do_card_add(g.input.buf);
		break;
	case FOR_CARD_REMOVE:
		do_card_remove(g.card);
		break;
	case FOR_NEW_ID:
		if (new_id_done(g.input.buf)) {
			return;
		}
		break;
	case FOR_NEW_NAME:
		if (new_name_done(g.input.buf)) {
			return;
		}
		break;
	case FOR_NEW_FROM:
		if (new_from_done(g.input.buf)) {
			return;
		}
		break;
	case FOR_NEW_UNTIL:
		/* Uncover the screen first: the new person is shown by id. */
		close_dialog();
		new_until_done(g.input.buf);
		draw_title();
		draw_list_frame();
		draw_status();
		return;
	case FOR_DELETE:
		close_dialog();
		do_person_delete();
		draw_title();
		draw_detail();
		draw_status();
		return;
	}
	close_dialog();
}

/* -- the main screen ----------------------------------------------------- */

static void leave(void);

static void move_perm(int delta)
{
	int last = (int)g.sel.n_perms - 1;
	int old_top = g.perm_top;

	if (g.sel_rc != 0 || last < 0) {
		return;
	}
	g.perm_cur = (uint8_t)CLAMP((int)g.perm_cur + delta, 0, last);
	if (g.perm_cur < g.perm_top) {
		g.perm_top = g.perm_cur;
	} else if (g.perm_cur >= g.perm_top + PERM_ROWS) {
		g.perm_top = g.perm_cur - PERM_ROWS + 1;
	}
	if (g.perm_top != old_top) {
		draw_detail();
	} else {
		for (int i = 0; i < PERM_ROWS; i++) {
			draw_perm_row(i);
		}
	}
}

static void move_card(int delta)
{
	if (g.sel_rc != 0 || g.sel.n_cards == 0) {
		return;
	}
	g.card_cur = (uint8_t)CLAMP((int)g.card_cur + delta, 0,
				    (int)g.sel.n_cards - 1);
	for (int i = 0; i < PERSONDB_CARDS_MAX; i++) {
		draw_card_row(i);
	}
}

static void set_focus(enum focus f)
{
	/* The detail panes stay focusable when empty — adding the first card
	 * or permission starts there — but not when there is no person. */
	if (f != FOCUS_LIST && g.sel_rc != 0) {
		return;
	}
	g.focus = f;
	draw_title();
	if (g.n > 0 && g.cur >= g.top && g.cur < g.top + LIST_ROWS) {
		draw_row((int)(g.cur - g.top));
	}
	draw_detail();
	draw_help();
}

static void main_key(int key)
{
	bool list = (g.focus == FOCUS_LIST);

	switch (key) {
	case 'q':
	case TERM_KEY_CTRL_C:
	case TERM_KEY_CTRL_D:
		leave();
		return;
	case TERM_KEY_CTRL_L:
		draw_all();
		return;
	case 'r':
		load_page();
		draw_detail();
		draw_status();
		return;
	case TERM_KEY_TAB:
		set_focus(g.focus == FOCUS_LIST  ? FOCUS_PERMS :
			  g.focus == FOCUS_PERMS ? FOCUS_CARDS : FOCUS_LIST);
		return;
	case TERM_KEY_LEFT:
		set_focus(FOCUS_LIST);
		return;
	case TERM_KEY_RIGHT:
		set_focus(g.focus == FOCUS_LIST ? FOCUS_PERMS : FOCUS_CARDS);
		return;
	case 'a':
	case '+':
		if (g.focus == FOCUS_CARDS) {
			open_card_add();
		} else if (g.focus == FOCUS_PERMS) {
			open_grant();
		} else {
			open_person_add();
		}
		return;
	case 'd':
	case '-':
	case TERM_KEY_DEL:
		if (g.focus == FOCUS_CARDS) {
			open_card_remove();
		} else if (g.focus == FOCUS_PERMS) {
			open_revoke();
		} else {
			open_person_delete();
		}
		return;
	case 'n':
		open_rename();
		return;
	case 'v':
		open_validity();
		return;
	case 'g':
		open_input(FOR_GOTO, "Go to person", "Person id:", 10, NULL);
		return;
	case 'c':
		open_input(FOR_CARD, "Find card holder", "Card id:",
			   PERSONDB_CARD_LEN, NULL);
		return;
	default:
		break;
	}

	if (list) {
		uint32_t cur = g.cur;
		uint32_t last = g.n ? g.n - 1 : 0;

		if (g.detached && (key == TERM_KEY_UP || key == TERM_KEY_DOWN)) {
			/* Back from a person opened by id to the list row. */
			select_pos(g.cur);
			return;
		}
		switch (key) {
		case TERM_KEY_UP:   cur = cur ? cur - 1 : 0; break;
		case TERM_KEY_DOWN: cur = MIN(cur + 1, last); break;
		case TERM_KEY_PGUP: cur = cur > LIST_ROWS ? cur - LIST_ROWS : 0;
				    break;
		case TERM_KEY_PGDN: cur = MIN(cur + LIST_ROWS, last); break;
		case TERM_KEY_HOME: cur = 0; break;
		case TERM_KEY_END:  cur = last; break;
		default:            return;
		}
		select_pos(cur);
	} else if (g.focus == FOCUS_CARDS) {
		switch (key) {
		case TERM_KEY_UP:   move_card(-1); break;
		case TERM_KEY_DOWN: move_card(1); break;
		case TERM_KEY_HOME: move_card(-PERSONDB_CARDS_MAX); break;
		case TERM_KEY_END:  move_card(PERSONDB_CARDS_MAX); break;
		default:            return;
		}
	} else {
		switch (key) {
		case TERM_KEY_UP:   move_perm(-1); break;
		case TERM_KEY_DOWN: move_perm(1); break;
		case TERM_KEY_PGUP: move_perm(-PERM_ROWS); break;
		case TERM_KEY_PGDN: move_perm(PERM_ROWS); break;
		case TERM_KEY_HOME: move_perm(-PERSONDB_PERMS_MAX); break;
		case TERM_KEY_END:  move_perm(PERSONDB_PERMS_MAX); break;
		default:            return;
		}
	}
}

static void handle_key(int key)
{
	if (g.mode == MODE_MAIN) {
		main_key(key);
	} else {
		dialog_key(key);
	}
}

/* -- terminal ownership -------------------------------------------------- */

static void feed(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len && g.active; i++) {
		int key = term_key_feed(data[i]);

		if (key != TERM_KEY_NONE) {
			handle_key(key);
		}
	}
}

static void bypass_cb(const struct shell *sh, uint8_t *data, size_t len,
		      void *user_data)
{
	ARG_UNUSED(user_data);

	feed(data, len);

	/* A chunk that ends inside an escape sequence: wait briefly for the
	 * rest, reading the transport directly. The callback runs on the shell
	 * thread, which is the transport's only reader, so nothing else is
	 * consuming these bytes meanwhile. Without this a lone Esc would sit
	 * in the decoder until the next key arrived. */
	while (g.active && term_key_pending()) {
		uint8_t buf[8];
		size_t got = 0;

		for (int ms = 0; ms < ESC_WAIT_MS && got == 0; ms++) {
			if (sh->iface->api->read(sh->iface, buf, sizeof(buf),
						 &got) != 0) {
				got = 0;
				break;
			}
			if (got == 0) {
				k_msleep(1);
			}
		}
		if (got > 0) {
			feed(buf, got);
		} else {
			int key = term_key_timeout();

			if (key != TERM_KEY_NONE) {
				handle_key(key);
			}
		}
	}

	if (g.active) {
		term_flush();
	}
}

/* Log lines printed into a full-screen UI land wherever the cursor last was.
 * Halt the shell's log backend for as long as the TUI owns the terminal —
 * what `log halt` does — and resume it on exit. Messages logged meanwhile
 * still reach every other backend. */
static void logs_mute(bool mute)
{
#if defined(CONFIG_SHELL_LOG_BACKEND)
	const struct log_backend *b =
		g.sh->log_backend ? g.sh->log_backend->backend : NULL;

	if (b == NULL) {
		return;
	}
	if (mute && log_backend_is_active(b)) {
		log_backend_deactivate(b);
		g.logs_muted = true;
	} else if (!mute && g.logs_muted) {
		log_backend_activate(b, b->cb->ctx);
		g.logs_muted = false;
	}
#else
	ARG_UNUSED(mute);
#endif
}

/* Back to the shell, exactly as it was: primary screen, cursor, logging. */
static void leave(void)
{
	term_leave();
	g.active = false;
	shell_set_bypass(g.sh, NULL, NULL);
	logs_mute(false);
	shell_print(g.sh, "left persondb tui; `persondb tui` opens it again");
}

int ui_tui_enter(const struct shell *sh, struct scenario *s)
{
	struct persondb_stat st;
	int rc = persondb_stat(s->db, &st);

	if (rc != 0) {
		return rc;
	}

	memset(&g, 0, sizeof(g));
	memset(g_page_known, 0, sizeof(g_page_known));
	g.sh = sh;
	g.s = s;
	g.populated = st.populated;
	g.n_persons = st.n_persons;
	g.rev = st.rev;
	g.active = true;

	logs_mute(true);
	term_bind(sh);
	term_key_reset();
	term_enter();
	shell_set_bypass(sh, bypass_cb, NULL);

	/* The total, counted from flash once (~2 s on the DK at 8 000) and
	 * kept up to date from here: the container keeps no counter. */
	set_status(false, "counting persons ...");
	draw_all();
	rc = persondb_person_count(s->db, &g.n);
	if (rc != 0) {
		g.n = 0;
	}
	draw_title();
	load_page();
	draw_detail();
	if (rc != 0) {
		set_status(true, "counting persons failed | rc %d", rc);
	}
	draw_status();
	term_flush();
	return 0;
}
