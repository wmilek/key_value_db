/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_shell — the same operations, driven by hand.
 *
 * Parsing and printing only; every command is one call into scenario.h or
 * persondb.h (DESIGN.md F15). Two groups, one per layer:
 *
 *   persondb  the L3 API, one command per persondb_*() call and named after
 *             it: `persondb card assign` is persondb_card_assign(). Nothing
 *             here knows about the dataset or the benchmark.
 *   scenario  the dataset generator and the benchmark: fill, verify, mutate,
 *             bench, records addressed by dataset index, and reset.
 *
 * This frontend exists because some claims can only be demonstrated
 * interactively. `scenario fill 500`, pull the power, reboot, `persondb stat`
 * shows the resumable fill (F8) surviving; `persondb card assign` / `revoke`
 * followed by a power cut shows that both crash windows deny rather than
 * grant (F5). Neither is something an unattended benchmark can show you.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "scenario.h"
#if defined(CONFIG_APP_CBOR_PERSONDB_TUI)
#include "ui_tui.h"
#endif

/* Persons `persondb person list` prints when not told how many. */
#define LIST_PAGE 10

static struct scenario g_s;
static bool g_open;

static int need_open(const struct shell *sh)
{
	if (!g_open) {
		shell_error(sh, "store not open; `persondb open` opens it");
		return -ENODEV;
	}
	return 0;
}

/* A whole decimal (or 0x) number, or -EINVAL: "12x" and "" are not ids. */
static int parse_u32(const struct shell *sh, const char *arg, uint32_t *out)
{
	char *end;
	unsigned long v = strtoul(arg, &end, 0);

	if (*arg == '\0' || *end != '\0' || v > UINT32_MAX) {
		shell_error(sh, "not a number: '%s'", arg);
		return -EINVAL;
	}
	*out = (uint32_t)v;
	return 0;
}

/* Copy a field, refusing what would not fit — persondb refuses rather than
 * truncates (persondb.h), and the shell should not truncate on its behalf. */
static int copy_field(const struct shell *sh, const char *what, char *dst,
		      size_t cap, const char *src)
{
	if (strlen(src) >= cap) {
		shell_error(sh, "%s longer than %u characters", what,
			    (unsigned)(cap - 1));
		return -EINVAL;
	}
	strcpy(dst, src);
	return 0;
}

static void show_person(const struct shell *sh, const struct persondb_person *p)
{
	shell_print(sh, "id       : %u", p->id);
	shell_print(sh, "name     : %s", p->name);
	shell_print(sh, "dept     : %s / %s", p->dept, p->title);
	shell_print(sh, "valid    : %u .. %u", p->valid_from, p->valid_until);
	shell_print(sh, "perms    : %u", p->n_perms);
	for (uint8_t i = 0; i < p->n_perms; i++) {
		shell_print(sh, "  %s", p->perm[i]);
	}
	shell_print(sh, "cards    : %u", p->n_cards);
	for (uint8_t i = 0; i < p->n_cards; i++) {
		shell_print(sh, "  %s", p->card[i]);
	}
}

/* ======================================================================== */
/* persondb — the L3 API                                                    */
/* ======================================================================== */

/* -- lifecycle ---------------------------------------------------------- */

/* Builds the store if there is none, as persondb_open() does. */
static int cmd_open(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (g_open) {
		shell_error(sh, "already open");
		return -EALREADY;
	}
	rc = scenario_open(&g_s);
	g_open = (rc == 0);
	shell_print(sh, "open: %d", rc);
	return rc;
}

/* Unmounts blob_db too (persondb.h), so its shell group goes quiet until
 * the next open. */
static int cmd_close(const struct shell *sh, size_t argc, char **argv)
{
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (rc) {
		return rc;
	}
	rc = scenario_close(&g_s);
	g_open = false;
	shell_print(sh, "close: %d", rc);
	return rc;
}

/* Needs no open store: it is how a store this build refuses — a newer
 * build's, or one a destroy lost power in — gets removed. Only this store
 * goes; the rest of the partition is untouched. The handle is gone either
 * way, since a failed destroy may have released part of the store. */
static int cmd_destroy(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	rc = scenario_destroy(&g_s);
	g_open = false;
	shell_print(sh, "destroy: %d", rc);
	return rc;
}

static int cmd_prepare(const struct shell *sh, size_t argc, char **argv)
{
	int64_t ms;
	int formatted;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (rc) {
		return rc;
	}
	rc = scenario_prepare(&g_s, &formatted, &ms);
	shell_print(sh, "prepare: %d — %d buckets formatted in %lld ms", rc,
		    formatted, (long long)ms);
	return rc;
}

/* persondb_stat(); `stat reset` is persondb_counters_reset(). */
static int cmd_stat(const struct shell *sh, size_t argc, char **argv)
{
	struct persondb_stat st;
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	if (argc > 1) {
		if (strcmp(argv[1], "reset") != 0) {
			shell_error(sh, "usage: persondb stat [reset]");
			return -EINVAL;
		}
		persondb_counters_reset(g_s.db);
		shell_print(sh, "counters reset");
		return 0;
	}

	rc = persondb_stat(g_s.db, &st);
	if (rc != 0) {
		shell_error(sh, "stat: %d", rc);
		return rc;
	}
	shell_print(sh, "partition   : %zu B", st.partition_bytes);
	shell_print(sh, "maps        : person (depth %u, %u buckets) + "
			"credential (depth %u, %u buckets)", st.people_depth,
		    st.people_buckets, st.cred_depth, st.cred_buckets);
	shell_print(sh, "persons     : %u of %u (rev %u)", st.populated,
		    st.n_persons, st.rev);
	shell_print(sh, "overflows   : %u", st.enospc_hits);
	shell_print(sh, "map ops     : %llu get, %llu set, %llu del",
		    (unsigned long long)st.map_gets,
		    (unsigned long long)st.map_sets,
		    (unsigned long long)st.map_dels);
	if (st.io_measured) {
		shell_print(sh, "flash       : %llu reads (%llu B), %llu writes "
				"(%llu B), %llu erases (%llu B)",
			    (unsigned long long)st.flash_reads,
			    (unsigned long long)st.bytes_read,
			    (unsigned long long)st.flash_writes,
			    (unsigned long long)st.bytes_written,
			    (unsigned long long)st.flash_erases,
			    (unsigned long long)st.bytes_erased);
	}
	return 0;
}

/* -- person ------------------------------------------------------------- */

static int cmd_person_get(const struct shell *sh, size_t argc, char **argv)
{
	struct persondb_person p;
	uint32_t id;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	if (rc || (rc = parse_u32(sh, argv[1], &id))) {
		return rc;
	}
	rc = persondb_person_get(g_s.db, id, &p);
	if (rc != 0) {
		shell_error(sh, "get %u: %d", id, rc);
		return rc;
	}
	show_person(sh, &p);
	return 0;
}

/*
 * persondb_person_add() with the fields a command line can carry: no cards,
 * no permissions, valid forever. `card assign`, `perm grant` and `person
 * validity` add the rest. persondb_person_put() has no command — it replaces
 * a whole record, and this composition covers what a person at a keyboard
 * would use it for without the way to silently drop someone's cards.
 */
static int cmd_person_add(const struct shell *sh, size_t argc, char **argv)
{
	struct persondb_person p = { .valid_from = 0, .valid_until = UINT32_MAX };
	int rc = need_open(sh);

	if (rc || (rc = parse_u32(sh, argv[1], &p.id)) ||
	    (rc = copy_field(sh, "name", p.name, sizeof(p.name), argv[2])) ||
	    (argc > 3 &&
	     (rc = copy_field(sh, "dept", p.dept, sizeof(p.dept), argv[3]))) ||
	    (argc > 4 &&
	     (rc = copy_field(sh, "title", p.title, sizeof(p.title), argv[4])))) {
		return rc;
	}

	/* Not refused: persondb has no such rule. But `scenario fill` rewrites
	 * every id in the dataset's range (the TUI refuses them for this). */
	if (p.id < scenario_id_of(g_s.n_persons)) {
		shell_warn(sh, "id %u is in the dataset's range: `scenario fill` "
			       "may overwrite it", p.id);
	}
	rc = persondb_person_add(g_s.db, &p);
	shell_print(sh, "add: %d", rc);
	return rc;
}

static int cmd_person_delete(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t id;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	if (rc || (rc = parse_u32(sh, argv[1], &id))) {
		return rc;
	}
	rc = persondb_person_delete(g_s.db, id);
	shell_print(sh, "delete: %d", rc);
	return rc;
}

/* persondb_person_next(), a page at a time: the cursor is the last id
 * printed, so `list <that id>` carries on where this one stopped. */
static int cmd_person_list(const struct shell *sh, size_t argc, char **argv)
{
	struct persondb_person p;
	uint32_t after = 0, n = LIST_PAGE, shown = 0;
	bool from_start = true;
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	if (argc > 1) {
		rc = parse_u32(sh, argv[1], &after);
		if (rc) {
			return rc;
		}
		from_start = false;
	}
	if (argc > 2 && (rc = parse_u32(sh, argv[2], &n))) {
		return rc;
	}

	for (; shown < n; shown++) {
		rc = persondb_person_next(g_s.db, from_start ? NULL : &after, &p);
		if (rc != 0) {
			break;
		}
		shell_print(sh, "%10u  %-31s  %s / %s  (%u perms, %u cards)",
			    p.id, p.name, p.dept, p.title, p.n_perms,
			    p.n_cards);
		after = p.id;
		from_start = false;
	}
	if (rc == -ENODATA) {
		shell_print(sh, "-- end, %u shown", shown);
		return 0;
	}
	if (rc != 0) {
		shell_error(sh, "list: %d", rc);
		return rc;
	}
	shell_print(sh, "-- more: persondb person list %u", after);
	return 0;
}

static int cmd_person_count(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (rc) {
		return rc;
	}
	rc = persondb_person_count(g_s.db, &n);
	if (rc != 0) {
		shell_error(sh, "count: %d", rc);
		return rc;
	}
	shell_print(sh, "%u persons", n);
	return 0;
}

static int cmd_person_rename(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t id;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	if (rc || (rc = parse_u32(sh, argv[1], &id))) {
		return rc;
	}
	rc = persondb_person_rename(g_s.db, id, argv[2]);
	shell_print(sh, "rename: %d", rc);
	return rc;
}

/* persondb_person_set_validity(): [from, until), epoch seconds. */
static int cmd_person_validity(const struct shell *sh, size_t argc,
			       char **argv)
{
	uint32_t id, from, until;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	if (rc || (rc = parse_u32(sh, argv[1], &id)) ||
	    (rc = parse_u32(sh, argv[2], &from)) ||
	    (rc = parse_u32(sh, argv[3], &until))) {
		return rc;
	}
	rc = persondb_person_set_validity(g_s.db, id, from, until);
	shell_print(sh, "validity: %d", rc);
	return rc;
}

/* -- card --------------------------------------------------------------- */

static int cmd_card_owner(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t owner;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	if (rc) {
		return rc;
	}
	rc = persondb_card_owner(g_s.db, argv[1], &owner);
	if (rc != 0) {
		shell_error(sh, "owner %s: %d", argv[1], rc);
		return rc;
	}
	shell_print(sh, "%s -> person %u", argv[1], owner);
	return 0;
}

static int cmd_card_count(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (rc) {
		return rc;
	}
	rc = persondb_card_count(g_s.db, &n);
	if (rc != 0) {
		shell_error(sh, "count: %d", rc);
		return rc;
	}
	shell_print(sh, "%u cards", n);
	return 0;
}

static int cmd_card_assign(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t id;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	if (rc || (rc = parse_u32(sh, argv[1], &id))) {
		return rc;
	}
	rc = persondb_card_assign(g_s.db, id, argv[2]);
	shell_print(sh, "assign: %d", rc);
	return rc;
}

/* persondb_card_revoke(): the holder is found through the index. */
static int cmd_card_revoke(const struct shell *sh, size_t argc, char **argv)
{
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	if (rc) {
		return rc;
	}
	rc = persondb_card_revoke(g_s.db, argv[1]);
	shell_print(sh, "revoke: %d", rc);
	return rc;
}

/* persondb_card_revoke_from(): the repairable form, for a known holder. */
static int cmd_card_revoke_from(const struct shell *sh, size_t argc,
				char **argv)
{
	uint32_t id;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	if (rc || (rc = parse_u32(sh, argv[1], &id))) {
		return rc;
	}
	rc = persondb_card_revoke_from(g_s.db, id, argv[2]);
	shell_print(sh, "revoke-from: %d", rc);
	return rc;
}

/* -- perm --------------------------------------------------------------- */

static int cmd_perm_grant(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t id;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	if (rc || (rc = parse_u32(sh, argv[1], &id))) {
		return rc;
	}
	rc = persondb_permission_grant(g_s.db, id, argv[2]);
	shell_print(sh, "grant: %d", rc);
	return rc;
}

static int cmd_perm_revoke(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t id;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	if (rc || (rc = parse_u32(sh, argv[1], &id))) {
		return rc;
	}
	rc = persondb_permission_revoke(g_s.db, id, argv[2]);
	shell_print(sh, "revoke: %d", rc);
	return rc;
}

/* -- the rights vocabulary ---------------------------------------------- */

static int cmd_right_list(const struct shell *sh, size_t argc, char **argv)
{
	char cur[PERSONDB_PERM_MAX + 1] = "";
	char name[PERSONDB_PERM_MAX + 1];
	uint32_t n = 0;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (rc) {
		return rc;
	}
	while ((rc = persondb_right_next(g_s.db, cur, name, sizeof(name))) == 0) {
		shell_print(sh, "  %s", name);
		strcpy(cur, name);
		n++;
	}
	if (rc != -ENODATA) {
		shell_error(sh, "list: %d", rc);
		return rc;
	}
	shell_print(sh, "%u defined (set order, not sorted)", n);
	return 0;
}

static int cmd_right_define(const struct shell *sh, size_t argc, char **argv)
{
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	if (rc) {
		return rc;
	}
	rc = persondb_right_define(g_s.db, argv[1]);
	shell_print(sh, "define: %d%s", rc,
		    rc == -EEXIST ? " (already defined, nothing written)" : "");
	return rc == -EEXIST ? 0 : rc;
}

static int cmd_right_undefine(const struct shell *sh, size_t argc, char **argv)
{
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	if (rc) {
		return rc;
	}
	rc = persondb_right_undefine(g_s.db, argv[1]);
	shell_print(sh, "undefine: %d", rc);
	return rc;
}

/* -- the decision ------------------------------------------------------- */

static int cmd_check(const struct shell *sh, size_t argc, char **argv)
{
	struct persondb_person p;
	enum persondb_decision d;
	uint32_t now = SCENARIO_BENCH_NOW;
	int64_t t;
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	/* The board has no clock, so "now" defaults to the instant the
	 * benchmark decides at; persondb_check() takes it as an argument. */
	if (argc > 3 && (rc = parse_u32(sh, argv[3], &now))) {
		return rc;
	}

	/* scenario_now_us(), not k_uptime_get(): on native_sim the simulated
	 * clock only advances when a modelled event fires, and a RAM-backed
	 * flash model fires none — so the millisecond clock reported "0 ms" for
	 * every decision here, which is the one number this command exists to
	 * show. Microseconds, for the same reason. */
	t = scenario_now_us();
	rc = persondb_check(g_s.db, argv[1], argv[2], now, &d, &p);
	if (rc != 0) {
		shell_error(sh, "check: %d", rc);
		return rc;
	}
	shell_print(sh, "%s in %lld us", persondb_decision_str(d),
		    (long long)scenario_since_us(t));
	if (d != PERSONDB_UNKNOWN_CARD) {
		shell_print(sh, "  %s (%u), %s / %s", p.name, p.id, p.dept,
			    p.title);
	}
	return 0;
}

#if defined(CONFIG_APP_CBOR_PERSONDB_TUI)
static int cmd_tui(const struct shell *sh, size_t argc, char **argv)
{
	struct ui_tui_config cfg;
	int rc = need_open(sh);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (rc) {
		return rc;
	}

	/*
	 * The TUI calls persondb and nothing else. What it needs to know about
	 * this application's dataset and benchmark is decided here, where the
	 * scenario already lives:
	 *
	 *  - hand-added persons start above the dataset's ids, so `fill` —
	 *    which rewrites every id in that range — cannot overwrite them;
	 *  - "today" is SCENARIO_BENCH_NOW, the instant `check` decides at,
	 *    because the board has no clock;
	 *  - the clock is the scenario's, the one that advances on native_sim.
	 */
	cfg.db = g_s.db;
	cfg.new_id_min = scenario_id_of(g_s.n_persons);
	cfg.today = SCENARIO_BENCH_NOW;
	cfg.now_us = scenario_now_us;

	/* Returns once the screen is drawn; the TUI then runs from the
	 * shell's bypass callback until 'q' hands the terminal back. */
	return ui_tui_enter(sh, &cfg);
}
#else
/* SHELL_COND_CMD_ARG below still names the handler when the flag is off. */
#define cmd_tui NULL
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	person_cmds,
	SHELL_CMD_ARG(get, NULL, "get <id>", cmd_person_get, 2, 0),
	SHELL_CMD_ARG(add, NULL, "add <id> <name> [dept] [title] — a bare "
		      "person", cmd_person_add, 3, 2),
	SHELL_CMD_ARG(delete, NULL, "delete <id> — and every card pointing "
		      "at it", cmd_person_delete, 2, 0),
	SHELL_CMD_ARG(list, NULL, "list [after-id] [n] — enumeration order",
		      cmd_person_list, 1, 2),
	SHELL_CMD_ARG(count, NULL, "count — reads every bucket", cmd_person_count,
		      1, 0),
	SHELL_CMD_ARG(rename, NULL, "rename <id> <name>", cmd_person_rename, 3, 0),
	SHELL_CMD_ARG(validity, NULL, "validity <id> <from> <until> — epoch s, "
		      "[from, until)", cmd_person_validity, 4, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	card_cmds,
	SHELL_CMD_ARG(owner, NULL, "owner <card>", cmd_card_owner, 2, 0),
	SHELL_CMD_ARG(count, NULL, "count — reads every bucket", cmd_card_count,
		      1, 0),
	SHELL_CMD_ARG(assign, NULL, "assign <person-id> <card>", cmd_card_assign,
		      3, 0),
	SHELL_CMD_ARG(revoke, NULL, "revoke <card> — holder found through the "
		      "index", cmd_card_revoke, 2, 0),
	SHELL_CMD_ARG(revoke-from, NULL, "revoke-from <person-id> <card> — "
		      "repairable", cmd_card_revoke_from, 3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	perm_cmds,
	SHELL_CMD_ARG(grant, NULL, "grant <person-id> <perm>", cmd_perm_grant, 3,
		      0),
	SHELL_CMD_ARG(revoke, NULL, "revoke <person-id> <perm>", cmd_perm_revoke,
		      3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	right_cmds,
	SHELL_CMD_ARG(list, NULL, "list — every defined name", cmd_right_list, 1, 0),
	SHELL_CMD_ARG(define, NULL, "define <name> — offer it for granting",
		      cmd_right_define, 2, 0),
	SHELL_CMD_ARG(undefine, NULL, "undefine <name> — holders keep it",
		      cmd_right_undefine, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	persondb_cmds,
	SHELL_CMD_ARG(open, NULL, "Open the store, building one if there is none",
		      cmd_open, 1, 0),
	SHELL_CMD_ARG(close, NULL, "Detach and unmount", cmd_close, 1, 0),
	SHELL_CMD_ARG(destroy, NULL, "Remove this store, and only it",
		      cmd_destroy, 1, 0),
	SHELL_CMD_ARG(prepare, NULL, "Pre-format erase blocks", cmd_prepare, 1, 0),
	SHELL_CMD_ARG(stat, NULL, "stat [reset] — geometry and counters",
		      cmd_stat, 1, 1),
	SHELL_CMD(person, &person_cmds, "Persons", NULL),
	SHELL_CMD(card, &card_cmds, "Credentials", NULL),
	SHELL_CMD(perm, &perm_cmds, "Permissions", NULL),
	SHELL_CMD(right, &right_cmds, "The rights vocabulary", NULL),
	SHELL_CMD_ARG(check, NULL, "check <card> <perm> [now] — the decision",
		      cmd_check, 3, 1),
	SHELL_COND_CMD_ARG(CONFIG_APP_CBOR_PERSONDB_TUI, tui, NULL,
			   "Full-screen browser: persons, grant/revoke", cmd_tui, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(persondb, &persondb_cmds, "Person database (L3 API)", NULL);

/* ======================================================================== */
/* scenario — the dataset and the benchmark                                 */
/* ======================================================================== */

/* What the store holds, accounted from the generator (scenario.h). */
static int cmd_report(const struct shell *sh, size_t argc, char **argv)
{
	struct scenario_report r;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	rc = need_open(sh);
	if (rc) {
		return rc;
	}
	rc = scenario_report_get(&g_s, &r);
	if (rc != 0) {
		shell_error(sh, "report: %d", rc);
		return rc;
	}
	shell_print(sh, "persons     : %u of %u (rev %u)", r.st.populated,
		    r.st.n_persons, r.st.rev);
	shell_print(sh, "credentials : %u", r.credentials);
	shell_print(sh, "live content: %llu B = %u.%u %% of the partition",
		    (unsigned long long)r.logical_bytes, r.fill_permille / 10,
		    r.fill_permille % 10);
	shell_print(sh, "mean record : %u B", r.mean_record);
	return 0;
}

static int cmd_fill(const struct shell *sh, size_t argc, char **argv)
{
	struct scenario_fill_report f;
	uint32_t budget = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 0) : 0;
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	rc = scenario_fill(&g_s, budget, &f);
	shell_print(sh, "%u written in %lld ms, %u/%u total, %u commits%s",
		    f.written, (long long)f.ms, f.populated, f.target,
		    f.batches, f.complete ? ", complete" : "");
	if (f.rights_defined) {
		shell_print(sh, "%u rights defined", f.rights_defined);
	}
	if (f.enospc) {
		shell_warn(sh, "%u bucket overflows (FINDINGS.md K2)", f.enospc);
	}
	return rc;
}

static int cmd_verify(const struct shell *sh, size_t argc, char **argv)
{
	struct scenario_verify_report v;
	uint32_t n = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 0)
				: CONFIG_APP_CBOR_PERSONDB_VERIFY_SAMPLES;
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	rc = scenario_verify(&g_s, n, &v);
	if (rc != 0) {
		shell_error(sh, "verify: %d", rc);
		return rc;
	}
	shell_print(sh, "%u persons, %u cards in %lld ms", v.persons_checked,
		    v.cards_checked, (long long)v.ms);
	if (v.bad_records || v.bad_cards || v.missing) {
		shell_error(sh, "FAIL: %u bad records, %u bad cards, %u missing",
			    v.bad_records, v.bad_cards, v.missing);
		return -EILSEQ;
	}
	shell_print(sh, "VERIFY PASS");
	return 0;
}

static int cmd_mutate(const struct shell *sh, size_t argc, char **argv)
{
	struct scenario_mutate_report m;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	rc = need_open(sh);
	if (rc) {
		return rc;
	}
	rc = scenario_mutate(&g_s, &m);
	if (rc != 0) {
		shell_error(sh, "mutate: %d", rc);
		return rc;
	}
	shell_print(sh, "rev %u -> %u: %u revoked, %u assigned, %u skipped, %lld ms",
		    m.rev_from, m.rev_to, m.revoked, m.assigned, m.skipped,
		    (long long)m.ms);
	return 0;
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0)
				: CONFIG_APP_CBOR_PERSONDB_BENCH_SAMPLES;
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	for (enum scenario_bench_kind k = 0; k < SCENARIO_BENCH_KINDS; k++) {
		struct bench_result b;

		if (argc > 1 && strcmp(argv[1], scenario_bench_name(k)) != 0) {
			continue;
		}
		rc = scenario_bench(&g_s, k, n, &b);
		if (rc != 0) {
			shell_error(sh, "bench %s: %d", scenario_bench_name(k), rc);
			return rc;
		}
		shell_print(sh, "%-6s %5u ops  %6lld ms  %7u us/op  "
				"%6llu flash ops  amp %ux",
			    b.name, b.ops, (long long)b.ms, b.us_per_op,
			    (unsigned long long)b.flash_ops, b.amplification);
	}
	return 0;
}

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	struct persondb_person p;
	uint32_t index;
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	if (argc < 2) {
		shell_error(sh, "usage: scenario show <dataset-index>");
		return -EINVAL;
	}
	index = (uint32_t)strtoul(argv[1], NULL, 0);
	rc = scenario_person_show(&g_s, index, &p);
	if (rc != 0) {
		shell_error(sh, "show %u: %d", index, rc);
		return rc;
	}
	show_person(sh, &p);
	return 0;
}

static int cmd_cardof(const struct shell *sh, size_t argc, char **argv)
{
	char card[PERSONDB_CARD_LEN + 1];
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	if (argc < 2) {
		shell_error(sh, "usage: scenario cardof <index> [slot]");
		return -EINVAL;
	}

	uint32_t index = (uint32_t)strtoul(argv[1], NULL, 0);
	uint8_t slot = (argc > 2) ? (uint8_t)strtoul(argv[2], NULL, 0) : 0;

	rc = scenario_card_of(&g_s, index, slot, card, sizeof(card));
	if (rc != 0) {
		shell_error(sh, "cardof: %d", rc);
		return rc;
	}
	shell_print(sh, "%s", card);
	return 0;
}

/* persondb destroy + open, in one command: the store this build refuses is
 * the usual reason to run it, so it needs no open store either. */
static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	rc = scenario_reset(&g_s);
	g_open = (rc == 0);
	shell_print(sh, "reset: %d", rc);
	return rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	scenario_cmds,
	SHELL_CMD_ARG(report, NULL, "What the dataset put in the store",
		      cmd_report, 1, 0),
	SHELL_CMD_ARG(fill, NULL, "fill [n] — populate n more persons", cmd_fill, 1, 1),
	SHELL_CMD_ARG(verify, NULL, "verify [n] — check n sampled persons", cmd_verify, 1, 1),
	SHELL_CMD_ARG(mutate, NULL, "Advance one mutation revision", cmd_mutate, 1, 0),
	SHELL_CMD_ARG(bench, NULL, "bench [kind] [n] — measure", cmd_bench, 1, 2),
	SHELL_CMD_ARG(show, NULL, "show <index> — the person at a dataset index",
		      cmd_show, 2, 0),
	SHELL_CMD_ARG(cardof, NULL, "cardof <index> [slot] — the card id", cmd_cardof, 2, 1),
	SHELL_CMD_ARG(reset, NULL, "persondb destroy + open: start over",
		      cmd_reset, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(scenario, &scenario_cmds, "Dataset and benchmark", NULL);

int ui_main(void)
{
	int rc = scenario_open(&g_s);

	if (rc != 0) {
		printk("persondb: open failed: %d\n", rc);
		if (rc == -ENOTSUP || rc == -EINPROGRESS) {
			printk("The log above says why. `scenario reset` "
			       "destroys the store and starts over.\n");
		}
		return rc;
	}
	g_open = true;

	struct persondb_stat st;

	persondb_stat(g_s.db, &st);
	printk("\npersondb shell ready — %u/%u persons at rev %u.\n",
	       st.populated, st.n_persons, st.rev);
	printk("Try: persondb stat | scenario fill 500 | scenario verify\n\n");
	return 0;
}
