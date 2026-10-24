/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * tui_widgets — pick list, text input, confirmation. See tui_widgets.h.
 */

#include <string.h>

#include <zephyr/sys/util.h>

#include "term.h"
#include "tui_widgets.h"

/* Every dialog is this wide and centred, so they line up when one opens
 * another (pick "<other...>" -> input). */
#define DLG_W   44
#define DLG_COL ((TERM_COLS - DLG_W) / 2 + 1)
#define DLG_IN  (DLG_W - 4)    /* text width inside, one space each side */

/* -- pick list ------------------------------------------------------------ */

void tui_pick_draw(const struct tui_pick *w)
{
	int vis = MIN(w->n, TUI_PICK_VISIBLE);
	int row = 6;

	term_attr(TERM_NORMAL);
	term_box(row, DLG_COL, vis + 2, DLG_W, w->title,
		 "Enter select  Esc cancel", true);

	for (int i = 0; i < vis; i++) {
		int idx = w->top + i;

		term_attr(idx == w->cur ? TERM_REVERSE : TERM_NORMAL);
		term_fieldf(row + 1 + i, DLG_COL + 1, DLG_W - 2, " %s",
			    w->item[idx]);
	}
	term_attr(TERM_NORMAL);

	/* Scroll marks in the right border: there is more above / below. */
	if (w->top > 0) {
		term_field(row + 1, DLG_COL + DLG_W - 1, 1, "^");
	}
	if (w->top + vis < w->n) {
		term_field(row + vis, DLG_COL + DLG_W - 1, 1, "v");
	}
}

enum tui_outcome tui_pick_key(struct tui_pick *w, int key)
{
	int last = (int)w->n - 1;
	int cur = w->cur;

	switch (key) {
	case TERM_KEY_UP:   cur--; break;
	case TERM_KEY_DOWN: cur++; break;
	case TERM_KEY_PGUP: cur -= TUI_PICK_VISIBLE; break;
	case TERM_KEY_PGDN: cur += TUI_PICK_VISIBLE; break;
	case TERM_KEY_HOME: cur = 0; break;
	case TERM_KEY_END:  cur = last; break;
	case TERM_KEY_ENTER:
		return (w->n > 0) ? TUI_DONE : TUI_CANCEL;
	case TERM_KEY_ESC:
	case TERM_KEY_CTRL_C:
		return TUI_CANCEL;
	default:
		return TUI_OPEN;
	}

	w->cur = (uint8_t)CLAMP(cur, 0, MAX(last, 0));
	if (w->cur < w->top) {
		w->top = w->cur;
	} else if (w->cur >= w->top + TUI_PICK_VISIBLE) {
		w->top = w->cur - TUI_PICK_VISIBLE + 1;
	}
	tui_pick_draw(w);
	return TUI_OPEN;
}

/* -- one-line text input ------------------------------------------------- */

#define INPUT_ROW 9

void tui_input_draw(const struct tui_input *w)
{
	term_attr(TERM_NORMAL);
	term_box(INPUT_ROW, DLG_COL, 5, DLG_W, w->title,
		 "Enter accept  Esc cancel", true);
	term_field(INPUT_ROW + 1, DLG_COL + 2, DLG_IN, w->prompt);

	/* The field is drawn at its full capacity, so the user can see how
	 * long a name may be before typing it rather than after. */
	term_attr(TERM_REVERSE);
	term_field(INPUT_ROW + 2, DLG_COL + 2, w->max + 1, w->buf);
	term_attr(TERM_NORMAL);

	term_move(INPUT_ROW + 2, DLG_COL + 2 + w->len);
	term_cursor(true);
}

enum tui_outcome tui_input_key(struct tui_input *w, int key)
{
	switch (key) {
	case TERM_KEY_ENTER:
		return TUI_DONE;
	case TERM_KEY_ESC:
	case TERM_KEY_CTRL_C:
		return TUI_CANCEL;
	case TERM_KEY_BACKSPACE:
	case TERM_KEY_LEFT:
		if (w->len > 0) {
			w->buf[--w->len] = '\0';
		}
		break;
	default:
		if (key < 0x20 || key > 0x7e || w->len >= w->max) {
			return TUI_OPEN;
		}
		w->buf[w->len++] = (char)key;
		w->buf[w->len] = '\0';
		break;
	}

	term_attr(TERM_REVERSE);
	term_field(INPUT_ROW + 2, DLG_COL + 2, w->max + 1, w->buf);
	term_attr(TERM_NORMAL);
	term_move(INPUT_ROW + 2, DLG_COL + 2 + w->len);
	return TUI_OPEN;
}

/* -- yes / no ------------------------------------------------------------ */

#define CONFIRM_ROW 9

void tui_confirm_draw(const struct tui_confirm *w)
{
	term_attr(TERM_NORMAL);
	term_box(CONFIRM_ROW, DLG_COL, 5, DLG_W, w->title, "y yes  n/Esc no",
		 true);
	term_attr(TERM_BOLD);
	term_field(CONFIRM_ROW + 2, DLG_COL + 2, DLG_IN, w->msg);
	term_attr(TERM_NORMAL);
}

enum tui_outcome tui_confirm_key(struct tui_confirm *w, int key)
{
	ARG_UNUSED(w);

	switch (key) {
	case 'y':
	case 'Y':
		return TUI_DONE;
	case 'n':
	case 'N':
	case TERM_KEY_ESC:
	case TERM_KEY_CTRL_C:
	case TERM_KEY_ENTER:     /* the default is No: Enter alone must not
				  * revoke anything */
		return TUI_CANCEL;
	default:
		return TUI_OPEN;
	}
}
