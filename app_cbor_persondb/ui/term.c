/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * term — VT100 output and key decoding. See term.h.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "term.h"

/* -- output ------------------------------------------------------------- */

static const struct shell *g_sh;

/* Batches escape codes and text into one shell write per ~128 bytes. Writing
 * each cursor move separately costs a shell_fprintf call apiece, and a full
 * screen is several hundred of them. */
static char g_out[128];
static size_t g_out_len;

void term_bind(const struct shell *sh)
{
	g_sh = sh;
	g_out_len = 0;
}

void term_flush(void)
{
	if (g_out_len == 0 || g_sh == NULL) {
		return;
	}
	g_out[g_out_len] = '\0';
	/* "%s", never the buffer itself as the format: a person's name is
	 * data, and a '%' in it must stay a '%'. */
	shell_fprintf(g_sh, SHELL_NORMAL, "%s", g_out);
	g_out_len = 0;
}

void term_puts(const char *s)
{
	while (*s != '\0') {
		if (g_out_len == sizeof(g_out) - 1) {
			term_flush();
		}
		g_out[g_out_len++] = *s++;
	}
}

void term_printf(const char *fmt, ...)
{
	char buf[TERM_COLS + 1];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	term_puts(buf);
}

void term_enter(void)
{
	/* Alternate screen, so leaving restores the shell's scrollback on the
	 * terminals that have one; the clear covers the ones that do not. */
	term_puts("\x1b[?1049h");
	term_attr(TERM_NORMAL);
	term_cursor(false);
	term_clear();
}

void term_leave(void)
{
	term_attr(TERM_NORMAL);
	term_puts("\x1b(B");
	term_clear();
	term_cursor(true);
	term_puts("\x1b[?1049l");
	term_flush();
}

void term_clear(void)
{
	term_puts("\x1b[2J\x1b[H");
}

void term_move(int row, int col)
{
	term_printf("\x1b[%d;%dH", row, col);
}

void term_attr(enum term_attr a)
{
	static const char *const sgr[] = {
		[TERM_NORMAL]       = "\x1b[0m",
		[TERM_BOLD]         = "\x1b[0;1m",
		[TERM_DIM]          = "\x1b[0;2m",
		[TERM_REVERSE]      = "\x1b[0;7m",
		[TERM_BOLD_REVERSE] = "\x1b[0;1;7m",
	};

	term_puts(sgr[a]);
}

void term_cursor(bool visible)
{
	term_puts(visible ? "\x1b[?25h" : "\x1b[?25l");
}

void term_field(int row, int col, int width, const char *s)
{
	char buf[TERM_COLS + 1];
	size_t n = strlen(s);

	if (width > TERM_COLS) {
		width = TERM_COLS;
	}
	if (n > (size_t)width) {
		n = (size_t)width;
	}
	memcpy(buf, s, n);
	memset(buf + n, ' ', (size_t)width - n);
	buf[width] = '\0';

	term_move(row, col);
	term_puts(buf);
}

void term_fieldf(int row, int col, int width, const char *fmt, ...)
{
	char buf[TERM_COLS + 1];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	term_field(row, col, width, buf);
}

/* DEC special graphics: in this character set 'l' 'k' 'm' 'j' are the four
 * corners, 'q' a horizontal and 'x' a vertical line. Chosen over UTF-8 box
 * characters because a serial terminal left in its default 8-bit or Latin-1
 * mode — minicom's default — renders those as mojibake. */
#define DEC_ON  "\x1b(0"
#define DEC_OFF "\x1b(B"

static void hline(int row, int col, int width, char left, char right,
		  const char *label)
{
	char buf[TERM_COLS + 1];
	int inner = width - 2;

	memset(buf, 'q', (size_t)inner);
	buf[inner] = '\0';

	term_move(row, col);
	term_puts(DEC_ON);
	term_printf("%c%s%c", left, buf, right);
	term_puts(DEC_OFF);

	if (label != NULL && label[0] != '\0' && inner > 4) {
		/* " label " over the line, starting one cell in. */
		term_fieldf(row, col + 2,
			    MIN((int)strlen(label) + 2, inner - 2), " %s ",
			    label);
	}
}

void term_box(int row, int col, int height, int width, const char *title,
	      const char *footer, bool fill)
{
	hline(row, col, width, 'l', 'k', title);
	for (int r = row + 1; r < row + height - 1; r++) {
		term_move(r, col);
		term_puts(DEC_ON "x" DEC_OFF);
		if (fill) {
			term_field(r, col + 1, width - 2, "");
		}
		term_move(r, col + width - 1);
		term_puts(DEC_ON "x" DEC_OFF);
	}
	hline(row + height - 1, col, width, 'm', 'j', footer);
}

/* -- input -------------------------------------------------------------- */

enum key_state {
	KS_GROUND,
	KS_ESC,       /* ESC seen */
	KS_CSI,       /* ESC [ seen, collecting parameters */
	KS_SS3,       /* ESC O seen */
};

static enum key_state g_ks;
static unsigned int g_param;
static bool g_after_cr;

void term_key_reset(void)
{
	g_ks = KS_GROUND;
	g_param = 0;
}

bool term_key_pending(void)
{
	return g_ks != KS_GROUND;
}

int term_key_timeout(void)
{
	int key = (g_ks == KS_ESC) ? TERM_KEY_ESC : TERM_KEY_NONE;

	term_key_reset();
	return key;
}

static int csi_final(uint8_t c, unsigned int param)
{
	switch (c) {
	case 'A': return TERM_KEY_UP;
	case 'B': return TERM_KEY_DOWN;
	case 'C': return TERM_KEY_RIGHT;
	case 'D': return TERM_KEY_LEFT;
	case 'H': return TERM_KEY_HOME;
	case 'F': return TERM_KEY_END;
	case '~':
		/* VT220 editing keypad. Home and End have two codes each,
		 * depending on which terminal emulates which keyboard. */
		switch (param) {
		case 1: case 7: return TERM_KEY_HOME;
		case 3:         return TERM_KEY_DEL;
		case 4: case 8: return TERM_KEY_END;
		case 5:         return TERM_KEY_PGUP;
		case 6:         return TERM_KEY_PGDN;
		default:        return TERM_KEY_NONE;
		}
	default:
		return TERM_KEY_NONE;
	}
}

int term_key_feed(uint8_t c)
{
	bool after_cr = g_after_cr;

	g_after_cr = false;

	switch (g_ks) {
	case KS_ESC:
		if (c == '[') {
			g_ks = KS_CSI;
			g_param = 0;
			return TERM_KEY_NONE;
		}
		if (c == 'O') {
			g_ks = KS_SS3;
			return TERM_KEY_NONE;
		}
		/* Esc followed by something else: the Esc was a key of its
		 * own. Report it and drop the byte rather than return two keys
		 * from one call — a second key typed within the same
		 * millisecond is not worth the bookkeeping. */
		term_key_reset();
		return TERM_KEY_ESC;

	case KS_CSI:
		if (c >= '0' && c <= '9') {
			if (g_param < 1000) {
				g_param = g_param * 10 + (c - '0');
			}
			return TERM_KEY_NONE;
		}
		if (c == ';' || (c >= 0x20 && c <= 0x3f)) {
			/* Modifier parameters (ESC [ 1 ; 5 A): keep the first
			 * number, ignore the rest. */
			return TERM_KEY_NONE;
		}
		{
			unsigned int param = g_param;

			term_key_reset();
			return csi_final(c, param);
		}

	case KS_SS3:
		term_key_reset();
		return csi_final(c, 0);

	case KS_GROUND:
	default:
		break;
	}

	switch (c) {
	case 0x1b:
		g_ks = KS_ESC;
		return TERM_KEY_NONE;
	case '\r':
		g_after_cr = true;
		return TERM_KEY_ENTER;
	case '\n':
		/* CR LF is one Enter; a bare LF (some terminals) is one too. */
		return after_cr ? TERM_KEY_NONE : TERM_KEY_ENTER;
	case 0x7f:
	case 0x08:
		return TERM_KEY_BACKSPACE;
	case TERM_KEY_TAB:
	case TERM_KEY_CTRL_C:
	case TERM_KEY_CTRL_D:
	case TERM_KEY_CTRL_L:
		return c;
	default:
		return (c >= 0x20 && c <= 0x7e) ? c : TERM_KEY_NONE;
	}
}
