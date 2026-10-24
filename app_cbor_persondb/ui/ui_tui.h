/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui — a full-screen person browser inside the shell frontend.
 *
 * `persondb tui` takes over the shell's terminal: a page of stored persons on
 * the left, the selected one's permissions and cards on the right, and dialogs
 * to add and delete persons, grant and revoke permissions, add and remove
 * cards, rename a person and set the days their cards are valid. `q`, Ctrl-C
 * or Ctrl-D gives the terminal back to the shell.
 */

#ifndef APP_CBOR_PERSONDB_UI_TUI_H_
#define APP_CBOR_PERSONDB_UI_TUI_H_

#include <stdint.h>

#include <zephyr/shell/shell.h>

#include "persondb.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Everything the TUI is given, rather than reaches for.
 *
 * The TUI is a client of the person management API (persondb.h, the
 * application's L3) and of nothing else: it does not include scenario.h or
 * dataset.h, and refuses to build if either is pulled in. The few facts it
 * needs that are not about stored persons — which ids are free for people
 * added by hand, what "today" is — come from whoever starts it, which in
 * this application is the shell frontend.
 */
struct ui_tui_config {
	/** The open store. Must stay open until the TUI exits. */
	struct persondb   *db;

	/** Persons added by hand get ids from here up. Ids below belong to
	 *  someone else (here, the generated dataset, which `fill` rewrites). */
	uint32_t           new_id_min;

	/** Default first valid day for a new person, epoch seconds. */
	uint32_t           today;

	/** A microsecond clock that advances on this target, for the timings
	 *  on the status line. */
	int64_t          (*now_us)(void);
};

/**
 * @brief Take over @p sh's terminal and draw the first page.
 *
 * Returns as soon as the screen is up: from then on the frontend runs on the
 * shell thread, inside the shell's bypass callback, one received chunk of
 * bytes at a time. Every persondb call it makes is therefore on the same
 * thread as every shell command's — the single-caller rule persondb.h asks
 * for holds without a lock.
 *
 * @param cfg copied; the strings and the store it points at must outlive the
 *            TUI session
 */
int ui_tui_enter(const struct shell *sh, const struct ui_tui_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_UI_TUI_H_ */
