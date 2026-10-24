/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui — a full-screen person browser with permission dialogs.
 *
 * A frontend, so the same rule as the other two applies: parsing and drawing
 * only, and every action is one call into scenario.h or persondb.h
 * (DESIGN.md F15). What it adds is a way to see a person and change what they
 * may open — and who they are, and for how long — without typing ids into
 * a command line.
 *
 *  ┌ Persons ──────────────────────────┐┌ 100042 ───────────────────────┐
 *  │     id  name           dept   P C ││ Anna Kowalska                  │
 *  │ 100042  Anna Kowalska  R&D   18 3 ││ Permissions (18)               │
 *  └───────────────────────────────────┘└────────────────────────────────┘
 *   status: what the last operation did, its return code, its time
 *   keys: a grant, d revoke, n name, v valid from/to, g goto, c card
 *
 * Three things below are shaped by the store rather than by taste:
 *
 *  - The list is dataset indices 0..populated-1. A kvhash map cannot be
 *    iterated (FINDINGS.md K6), so the only persons this screen can list are
 *    the ones whose ids the generator can name. A person put by id from
 *    outside the dataset exists and can be reached by card, but not listed.
 *    There is no search by name for the same reason: it would read all 8 000
 *    records, 1-2 minutes on the DK.
 *
 *  - Every row is one persondb_person_get — 7.8 ms on the DK at the measured
 *    fill, more on a fuller store (RESULTS.md). A page is therefore read one
 *    row at a time and each row is drawn as it arrives, and only a ~44-byte
 *    summary per row is kept: the full record (~720 B) is held for the
 *    selected person alone.
 *
 *  - After any write the person is read back from the store, never
 *    patched in RAM. What the screen shows is what was stored — and the
 *    status line reports the return code and the time the write took.
 *
 * A person changed here no longer matches the generator, so `persondb
 * verify` will count it as a bad record if it samples that index. That is
 * verify doing its job, not a fault in either.
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

/** One list row: what the list shows, without the ~720 B record. */
struct row {
	bool     used;          /* a person index, not blank space past the end */
	int      rc;            /* the read's result; non-zero shows as an error */
	uint32_t id;
	uint8_t  n_perms;
	uint8_t  n_cards;
	char     name[21];
	char     dept[10];
};

enum focus { FOCUS_LIST, FOCUS_PERMS };

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
	FOR_REVOKE,         /* pick: a held permission to revoke */
	FOR_REVOKE_CONFIRM, /* confirm: revoke g.perm */
	FOR_GOTO,           /* input: a person id or dataset index */
	FOR_CARD,           /* input: a card id, to jump to its holder */
	FOR_RENAME,         /* input: the person's new name */
	FOR_VALID_FROM,     /* input: first valid day, then ... */
	FOR_VALID_UNTIL,    /* input: ... last valid day */
};

static const char OTHER[] = "<other...>";

static struct {
	bool                 active;
	const struct shell  *sh;
	struct scenario     *s;
	bool                 logs_muted;

	uint32_t             n;          /* persons listable: populated count */
	uint32_t             n_persons;
	uint32_t             rev;

	uint32_t             top;        /* index of the first row on the page */
	uint32_t             cur;        /* selected index */
	enum focus           focus;
	uint8_t              perm_cur;
	uint8_t              perm_top;

	struct row           rows[LIST_ROWS];
	struct persondb_person sel;      /* the selected person, as stored */
	int                  sel_rc;

	enum mode            mode;
	enum purpose         purpose;
	struct tui_pick      pick;
	struct tui_input     input;
	struct tui_confirm   confirm;
	char                 perm[PERSONDB_PERM_MAX + 1];
	uint32_t             new_from;   /* carried from one validity dialog
					  * to the next */

	char                 status[TERM_COLS];
	bool                 status_err;
} g;

/* Reading a page needs a whole person per row; one buffer serves all of them,
 * kept off the shell thread's stack, which blob_db's write path already
 * claims most of (Kconfig, SHELL_STACK_SIZE). */
static struct persondb_person g_scratch;

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

static void summarize(struct row *r, const struct persondb_person *p, int rc)
{
	r->used = true;
	r->rc = rc;
	if (rc != 0) {
		return;
	}
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
	const char *right = (g.focus == FOCUS_LIST) ? "[list] "
						    : "[permissions] ";

	snprintf(left, sizeof(left), " persondb | %u of %u persons | rev %u",
		 g.n, g.n_persons, g.rev);
	term_attr(TERM_BOLD_REVERSE);
	term_field(TITLE_ROW, 1, TERM_COLS - (int)strlen(right), left);
	term_puts(right);
	term_attr(TERM_NORMAL);
}

static void draw_row(int i)
{
	const struct row *r = &g.rows[i];
	uint32_t index = g.top + (uint32_t)i;
	int row = LIST_TOP + i;
	bool sel = r->used && index == g.cur;

	if (sel) {
		term_attr(g.focus == FOCUS_LIST ? TERM_REVERSE : TERM_BOLD);
	} else {
		term_attr(TERM_NORMAL);
	}

	if (!r->used) {
		term_field(row, LIST_COL + 1, LIST_W - 2, "");
	} else if (r->rc != 0) {
		term_fieldf(row, LIST_COL + 1, LIST_W - 2, " %6u  <read failed: %d>",
			    scenario_id_of(index), r->rc);
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
		snprintf(title, sizeof(title), "Persons %u-%u of %u", g.top,
			 MIN(g.top + LIST_ROWS, g.n) - 1, g.n);
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
	char title[16];
	char from[24], until[24];

	if (g.sel_rc == 0) {
		snprintf(title, sizeof(title), "%u", g.sel.id);
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
				   " q, then: persondb fill 200");
		}
		return;
	}

	fmt_date(from, sizeof(from), g.sel.valid_from);
	fmt_last_day(until, sizeof(until), g.sel.valid_until);

	term_attr(TERM_BOLD);
	term_fieldf(DET_NAME, DET_COL + 1, DET_IN, " %s", g.sel.name);
	term_attr(TERM_NORMAL);
	term_fieldf(DET_DEPT, DET_COL + 1, DET_IN, " %s / %s", g.sel.dept,
		    g.sel.title);
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
		term_fieldf(DET_CTOP + i, DET_COL + 1, DET_IN, "   %s",
			    i < g.sel.n_cards ? g.sel.card[i] : "");
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
	term_attr(TERM_REVERSE);
	term_field(HELP_ROW, 1, TERM_COLS,
		   " a grant  d revoke  n name  v valid from/to  g goto  c card"
		   "  r reload  q quit");
	/* 79 columns, not 80: writing the bottom-right cell makes some
	 * terminals scroll the whole screen up a line. */
	term_field(HELP_ROW + 1, 1, TERM_COLS - 1,
		   g.focus == FOCUS_LIST ?
		   " arrows/PgUp/PgDn/Home/End move  Tab to permissions" :
		   " arrows/PgUp/PgDn move  Tab to list  d/Del revokes the"
		   " selected permission");
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

/** Keep the permission cursor on a held permission and in view. */
static void perm_clamp(void)
{
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

/** Read the selected person into g.sel, and refresh its list row. */
static void load_sel(void)
{
	if (g.n == 0) {
		g.sel_rc = -ENOENT;
		return;
	}
	g.sel_rc = scenario_person_show(g.s, g.cur, &g.sel);
	summarize(&g.rows[g.cur - g.top], &g.sel, g.sel_rc);
	perm_clamp();
}

/**
 * @brief Read and draw the page starting at g.top, one row at a time.
 *
 * Each row is flushed to the terminal as soon as it is read, so a slow page
 * fills in visibly instead of freezing the screen for the whole of it.
 */
static void load_page(void)
{
	int64_t t0 = scenario_now_us();
	uint32_t reads = 0;

	memset(g.rows, 0, sizeof(g.rows));
	draw_list_frame();
	for (int i = 0; i < LIST_ROWS; i++) {
		draw_row(i);
	}
	term_flush();

	for (int i = 0; i < LIST_ROWS; i++) {
		uint32_t index = g.top + (uint32_t)i;

		if (index >= g.n) {
			break;
		}

		int rc = scenario_person_show(g.s, index, &g_scratch);

		reads++;
		summarize(&g.rows[i], &g_scratch, rc);
		if (index == g.cur) {
			g.sel = g_scratch;
			g.sel_rc = rc;
		}
		draw_row(i);
		term_flush();
	}
	perm_clamp();   /* a reload may find fewer permissions than before */
	if (g.n == 0) {
		g.sel_rc = -ENOENT;
		set_status(false, "the store holds no persons yet");
	} else {
		set_status(false, "read %u persons in %s", reads,
			   dur_since(t0));
	}
}

/** Select @p index, paging if it is off the current page. */
static void select_index(uint32_t index)
{
	if (g.n == 0) {
		return;
	}
	index = MIN(index, g.n - 1);

	uint32_t old = g.cur;
	uint32_t top = index - index % LIST_ROWS;

	g.cur = index;
	g.perm_cur = 0;
	g.perm_top = 0;

	if (top != g.top) {
		g.top = top;
		load_page();
	} else if (index != old) {
		int64_t t0 = scenario_now_us();

		draw_row((int)(old - g.top));
		load_sel();
		draw_row((int)(index - g.top));
		if (g.sel_rc == 0) {
			set_status(false, "read %u in %s",
				   scenario_id_of(index), dur_since(t0));
		} else {
			set_status(true, "person %u: read failed: %d",
				   scenario_id_of(index), g.sel_rc);
		}
	}
	draw_detail();
	draw_status();
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
		perm_clamp();
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
	snprintf(g.confirm.msg, sizeof(g.confirm.msg), "Revoke '%s' from %u?",
		 g.perm, g.sel.id);
	g.purpose = FOR_REVOKE_CONFIRM;
	g.mode = MODE_CONFIRM;
	tui_confirm_draw(&g.confirm);
}

static void open_revoke(void)
{
	static char title[32];

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
	if (g.focus == FOCUS_PERMS) {
		/* The cursor already says which one. */
		open_revoke_confirm(g.sel.perm[g.perm_cur]);
		return;
	}

	memset(&g.pick, 0, sizeof(g.pick));
	snprintf(title, sizeof(title), "Revoke from %u", g.sel.id);
	g.pick.title = title;
	for (uint8_t i = 0; i < g.sel.n_perms && i < TUI_PICK_MAX; i++) {
		g.pick.item[g.pick.n++] = g.sel.perm[i];
	}
	g.purpose = FOR_REVOKE;
	g.mode = MODE_PICK;
	tui_pick_draw(&g.pick);
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

static void close_dialog(void);

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
	uint32_t base = scenario_id_of(0);
	uint32_t index;

	if (text[0] == '\0' || *end != '\0') {
		set_status(true, "'%s' is not a number", text);
		return;
	}
	/* Ids and indices do not overlap (DATASET_ID_BASE), so one field
	 * takes either. */
	if (v >= base && scenario_index_of((uint32_t)v) < g.n) {
		index = scenario_index_of((uint32_t)v);
	} else if (v < g.n) {
		index = (uint32_t)v;
	} else {
		set_status(true, "no person %lu among the %u in the store", v,
			   g.n);
		return;
	}
	select_index(index);
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

	uint32_t index = scenario_index_of(owner);

	if (owner < scenario_id_of(0) || index >= g.n) {
		/* Resolves, but to a person outside the listable range (K6). */
		set_status(false, "card %s -> person %u, not in this list",
			   card, owner);
		return;
	}
	select_index(index);
	set_status(false, "card %s -> person %u | %s", card, owner, took);
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
	case FOR_REVOKE:
		close_dialog();
		open_revoke_confirm(g.pick.item[g.pick.cur]);
		return;
	case FOR_REVOKE_CONFIRM:
		do_revoke(g.perm);
		break;
	case FOR_GOTO:
		/* Uncover the list first: a jump may read a whole page, and
		 * its rows are drawn as they arrive. */
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

static void set_focus(enum focus f)
{
	if (f == FOCUS_PERMS && (g.sel_rc != 0 || g.sel.n_perms == 0)) {
		return;
	}
	g.focus = f;
	draw_title();
	if (g.n > 0) {
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
		set_focus(list ? FOCUS_PERMS : FOCUS_LIST);
		return;
	case TERM_KEY_LEFT:
		set_focus(FOCUS_LIST);
		return;
	case TERM_KEY_RIGHT:
		set_focus(FOCUS_PERMS);
		return;
	case 'a':
	case '+':
		open_grant();
		return;
	case 'd':
	case '-':
	case TERM_KEY_DEL:
		open_revoke();
		return;
	case 'n':
		open_rename();
		return;
	case 'v':
		open_validity();
		return;
	case 'g':
		open_input(FOR_GOTO, "Go to person", "Person id or dataset index:",
			   10, NULL);
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
		select_index(cur);
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

static void leave(void)
{
	term_leave();
	g.active = false;
	shell_set_bypass(g.sh, NULL, NULL);
	logs_mute(false);
}

int ui_tui_enter(const struct shell *sh, struct scenario *s)
{
	struct persondb_stat st;
	int rc = persondb_stat(s->db, &st);

	if (rc != 0) {
		return rc;
	}

	memset(&g, 0, sizeof(g));
	g.sh = sh;
	g.s = s;
	g.n = st.populated;
	g.n_persons = st.n_persons;
	g.rev = st.rev;
	g.active = true;

	logs_mute(true);
	term_bind(sh);
	term_key_reset();
	term_enter();
	shell_set_bypass(sh, bypass_cb, NULL);

	draw_all();
	load_page();
	draw_detail();
	draw_status();
	term_flush();
	return 0;
}
