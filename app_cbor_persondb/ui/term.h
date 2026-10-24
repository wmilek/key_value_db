/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * term — VT100 output and key decoding for the full-screen frontend.
 *
 * The only file that knows what an escape sequence is. Everything above it
 * says "move here, draw a box, reverse video" and receives keys as values,
 * never as bytes.
 *
 * Output goes through the shell's own transport (shell_fprintf), so it shares
 * the shell's buffering and works on whatever backend the shell runs on —
 * UART on the DK, a pseudo-terminal on native_sim. Escape codes are plain
 * VT100 plus the DEC line-drawing set, which every serial terminal worth
 * using understands (PuTTY, minicom, picocom, screen, Tera Term, xterm).
 *
 * The screen is assumed to be 80x24. A terminal cannot be asked its size
 * without a round trip the shell transport has no clean way to wait for, and
 * 80x24 is the one size every serial terminal opens at.
 */

#ifndef APP_CBOR_PERSONDB_UI_TERM_H_
#define APP_CBOR_PERSONDB_UI_TERM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERM_ROWS 24
#define TERM_COLS 80

/* -- output ------------------------------------------------------------- */

enum term_attr {
	TERM_NORMAL = 0,
	TERM_BOLD,
	TERM_DIM,
	TERM_REVERSE,
	TERM_BOLD_REVERSE,
};

/** Bind output to @p sh. Everything below writes there until the next call. */
void term_bind(const struct shell *sh);

void term_puts(const char *s);
void term_printf(const char *fmt, ...) __printf_like(1, 2);

/** Send whatever is buffered. Call once per frame, not per string. */
void term_flush(void);

/** Enter full-screen mode: alternate screen, cursor hidden, cleared. */
void term_enter(void);
/** Leave it: attributes off, cursor shown, primary screen back. */
void term_leave(void);

void term_clear(void);
void term_move(int row, int col);           /* 1-based, like VT100 */
void term_attr(enum term_attr a);
void term_cursor(bool visible);

/**
 * @brief Print @p s at (@p row, @p col), truncated or space-padded to
 *        exactly @p width cells.
 *
 * Padding is what makes partial redraws safe: a shorter string overwrites
 * the whole of a longer one it replaces, so no row ever needs clearing first.
 */
void term_field(int row, int col, int width, const char *s);

/** printf into a fixed-width field — term_field with formatting. */
void term_fieldf(int row, int col, int width, const char *fmt, ...)
	__printf_like(4, 5);

/**
 * @brief Draw a box with its top-left corner at (@p row, @p col).
 *
 * @param title  (optional) printed in the top border
 * @param footer (optional) printed in the bottom border
 * @param fill   blank the interior too — what a dialog needs to cover the
 *               screen beneath it
 */
void term_box(int row, int col, int height, int width, const char *title,
	      const char *footer, bool fill);

/* -- input -------------------------------------------------------------- */

/*
 * A key is either a printable byte (its own value, 0x20..0x7e) or one of
 * these. Control characters that mean nothing here are dropped by the
 * decoder rather than passed up as noise.
 */
enum term_key {
	TERM_KEY_NONE = 0,
	TERM_KEY_CTRL_C = 0x03,
	TERM_KEY_CTRL_D = 0x04,
	TERM_KEY_BACKSPACE = 0x08,
	TERM_KEY_TAB = 0x09,
	TERM_KEY_CTRL_L = 0x0c,
	TERM_KEY_ENTER = 0x0d,
	TERM_KEY_ESC = 0x1b,

	TERM_KEY_UP = 0x100,
	TERM_KEY_DOWN,
	TERM_KEY_LEFT,
	TERM_KEY_RIGHT,
	TERM_KEY_HOME,
	TERM_KEY_END,
	TERM_KEY_PGUP,
	TERM_KEY_PGDN,
	TERM_KEY_DEL,
};

/**
 * @brief Feed one received byte to the decoder.
 *
 * @return the key it completes, or TERM_KEY_NONE if it is part of a sequence
 *         still arriving (or meaningless on its own)
 */
int term_key_feed(uint8_t c);

/**
 * @brief True while the decoder holds the start of an escape sequence.
 *
 * A lone Esc and the first byte of an arrow key are the same byte. The
 * decoder cannot tell them apart until the next byte arrives — or does not —
 * so its caller waits a few milliseconds and then calls term_key_timeout().
 */
bool term_key_pending(void);

/**
 * @brief Nothing followed: resolve what the decoder is holding.
 *
 * @return TERM_KEY_ESC if all it held was Esc, otherwise TERM_KEY_NONE (an
 *         incomplete sequence is discarded, not guessed at)
 */
int term_key_timeout(void);

/** Forget any partial sequence. */
void term_key_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_UI_TERM_H_ */
