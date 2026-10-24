/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * tui_widgets — the three dialogs the full-screen frontend needs.
 *
 * Each is a plain struct plus two functions: draw it, and give it a key. A
 * key returns an outcome rather than calling anything back, so a widget knows
 * nothing about persons or storage and the screen that opened it decides what
 * "chosen" means. All of them are modal and centred, and draw over whatever
 * is beneath; the caller redraws the screen once the dialog closes.
 */

#ifndef APP_CBOR_PERSONDB_UI_TUI_WIDGETS_H_
#define APP_CBOR_PERSONDB_UI_TUI_WIDGETS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum tui_outcome {
	TUI_OPEN = 0,   /* still open; nothing for the caller to do */
	TUI_DONE,       /* confirmed: Enter, or 'y' */
	TUI_CANCEL,     /* Esc, Ctrl-C, or 'n' */
};

/* -- pick list ------------------------------------------------------------ */

#define TUI_PICK_MAX     40
#define TUI_PICK_VISIBLE 10

struct tui_pick {
	const char *title;
	const char *item[TUI_PICK_MAX];
	uint8_t     n;
	uint8_t     cur;
	uint8_t     top;
};

void tui_pick_draw(const struct tui_pick *w);
enum tui_outcome tui_pick_key(struct tui_pick *w, int key);

/* -- one-line text input ------------------------------------------------- */

#define TUI_INPUT_MAX 31

struct tui_input {
	const char *title;
	const char *prompt;
	char        buf[TUI_INPUT_MAX + 1];
	uint8_t     len;
	uint8_t     max;    /* <= TUI_INPUT_MAX; typing stops there */
};

void tui_input_draw(const struct tui_input *w);
enum tui_outcome tui_input_key(struct tui_input *w, int key);

/* -- yes / no ------------------------------------------------------------ */

struct tui_confirm {
	const char *title;
	char        msg[56];
	char        detail[56];     /* optional second line; "" for none */
};

void tui_confirm_draw(const struct tui_confirm *w);
enum tui_outcome tui_confirm_key(struct tui_confirm *w, int key);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_UI_TUI_WIDGETS_H_ */
