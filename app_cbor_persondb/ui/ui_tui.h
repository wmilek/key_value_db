/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui — a full-screen person browser inside the shell frontend.
 *
 * `persondb tui` takes over the shell's terminal: a page of persons on the
 * left, the selected one's permissions and cards on the right, and dialogs to
 * grant and revoke permissions. `q` gives the terminal back to the shell.
 */

#ifndef APP_CBOR_PERSONDB_UI_TUI_H_
#define APP_CBOR_PERSONDB_UI_TUI_H_

#include <zephyr/shell/shell.h>

#include "scenario.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Take over @p sh's terminal and draw the first page.
 *
 * Returns as soon as the screen is up: from then on the frontend runs on the
 * shell thread, inside the shell's bypass callback, one received chunk of
 * bytes at a time. Every persondb call it makes is therefore on the same
 * thread as every shell command's — the single-caller rule persondb.h asks
 * for holds without a lock.
 *
 * @param s an open scenario; must stay open until the TUI exits
 */
int ui_tui_enter(const struct shell *sh, struct scenario *s);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_UI_TUI_H_ */
