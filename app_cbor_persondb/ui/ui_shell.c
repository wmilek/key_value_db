/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_shell — the same operations, driven by hand.
 *
 * Parsing and printing only; every command is one call into scenario.h or
 * persondb.h (DESIGN.md F15).
 *
 * This frontend exists because some claims can only be demonstrated
 * interactively. `persondb fill 500`, pull the power, reboot, `persondb stat`
 * shows the resumable fill (F8) surviving; `persondb assign` / `unassign`
 * followed by a power cut shows that both crash windows deny rather than
 * grant (F5). Neither is something an unattended benchmark can show you.
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "scenario.h"

static struct scenario g_s;
static bool g_open;

static int need_open(const struct shell *sh)
{
	if (!g_open) {
		shell_error(sh, "store not open");
		return -ENODEV;
	}
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

static int cmd_stat(const struct shell *sh, size_t argc, char **argv)
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

	shell_print(sh, "partition   : %zu B, %zu B sectors",
		    r.st.partition_bytes, r.st.sector_bytes);
	shell_print(sh, "maps        : %u person + 1 credential "
			"(bucket count not observable)", r.st.n_people_maps);
	shell_print(sh, "persons     : %u of %u (rev %u)", r.st.populated,
		    r.st.n_persons, r.st.rev);
	shell_print(sh, "credentials : %u", r.credentials);
	shell_print(sh, "live content: %llu B = %u.%u %% of the partition",
		    (unsigned long long)r.logical_bytes, r.fill_permille / 10,
		    r.fill_permille % 10);
	shell_print(sh, "overflows   : %u", r.st.enospc_hits);
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
		shell_error(sh, "usage: persondb show <dataset-index>");
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

static int cmd_card(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t owner;
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	if (argc < 2) {
		shell_error(sh, "usage: persondb card <card-id>");
		return -EINVAL;
	}

	rc = persondb_card_owner(g_s.db, argv[1], &owner);
	if (rc != 0) {
		shell_error(sh, "card %s: %d", argv[1], rc);
		return rc;
	}
	shell_print(sh, "%s -> person %u (dataset index %u)", argv[1], owner,
		    scenario_index_of(owner));
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
		shell_error(sh, "usage: persondb cardof <index> [slot]");
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

static int cmd_check(const struct shell *sh, size_t argc, char **argv)
{
	struct persondb_person p;
	enum persondb_decision d;
	int64_t t;
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	if (argc < 3) {
		shell_error(sh, "usage: persondb check <card-id> <permission>");
		return -EINVAL;
	}

	t = k_uptime_get();
	rc = persondb_check(g_s.db, argv[1], argv[2], 1830000000u, &d, &p);
	if (rc != 0) {
		shell_error(sh, "check: %d", rc);
		return rc;
	}
	shell_print(sh, "%s in %lld ms", persondb_decision_str(d),
		    (long long)k_uptime_delta(&t));
	if (d != PERSONDB_UNKNOWN_CARD) {
		shell_print(sh, "  %s (%u), %s / %s", p.name, p.id, p.dept,
			    p.title);
	}
	return 0;
}

static int cmd_grant(const struct shell *sh, size_t argc, char **argv)
{
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	if (argc < 3) {
		shell_error(sh, "usage: persondb grant <person-id> <permission>");
		return -EINVAL;
	}
	rc = persondb_permission_grant(g_s.db,
				       (uint32_t)strtoul(argv[1], NULL, 0),
				       argv[2]);
	shell_print(sh, "grant: %d", rc);
	return rc;
}

static int cmd_revoke(const struct shell *sh, size_t argc, char **argv)
{
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	if (argc < 3) {
		shell_error(sh, "usage: persondb revoke <person-id> <permission>");
		return -EINVAL;
	}
	rc = persondb_permission_revoke(g_s.db,
					(uint32_t)strtoul(argv[1], NULL, 0),
					argv[2]);
	shell_print(sh, "revoke: %d", rc);
	return rc;
}

static int cmd_assign(const struct shell *sh, size_t argc, char **argv)
{
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	if (argc < 3) {
		shell_error(sh, "usage: persondb assign <person-id> <card-id>");
		return -EINVAL;
	}
	rc = persondb_card_assign(g_s.db, (uint32_t)strtoul(argv[1], NULL, 0),
				  argv[2]);
	shell_print(sh, "assign: %d", rc);
	return rc;
}

static int cmd_unassign(const struct shell *sh, size_t argc, char **argv)
{
	int rc = need_open(sh);

	if (rc) {
		return rc;
	}
	if (argc < 2) {
		shell_error(sh, "usage: persondb unassign <card-id>");
		return -EINVAL;
	}
	rc = persondb_card_revoke(g_s.db, argv[1]);
	shell_print(sh, "unassign: %d", rc);
	return rc;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	rc = need_open(sh);
	if (rc) {
		return rc;
	}
	rc = scenario_erase(&g_s);
	shell_print(sh, "reset: %d", rc);
	return rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	persondb_cmds,
	SHELL_CMD_ARG(stat, NULL, "Store geometry, fill and counters", cmd_stat, 1, 0),
	SHELL_CMD_ARG(fill, NULL, "fill [n] — populate n more persons", cmd_fill, 1, 1),
	SHELL_CMD_ARG(verify, NULL, "verify [n] — check n sampled persons", cmd_verify, 1, 1),
	SHELL_CMD_ARG(mutate, NULL, "Advance one mutation revision", cmd_mutate, 1, 0),
	SHELL_CMD_ARG(bench, NULL, "bench [kind] [n] — measure", cmd_bench, 1, 2),
	SHELL_CMD_ARG(show, NULL, "show <index> — print a stored person", cmd_show, 2, 0),
	SHELL_CMD_ARG(card, NULL, "card <card-id> — resolve to its owner", cmd_card, 2, 0),
	SHELL_CMD_ARG(cardof, NULL, "cardof <index> [slot] — the card id", cmd_cardof, 2, 1),
	SHELL_CMD_ARG(check, NULL, "check <card-id> <perm> — the decision", cmd_check, 3, 0),
	SHELL_CMD_ARG(grant, NULL, "grant <person-id> <perm>", cmd_grant, 3, 0),
	SHELL_CMD_ARG(revoke, NULL, "revoke <person-id> <perm>", cmd_revoke, 3, 0),
	SHELL_CMD_ARG(assign, NULL, "assign <person-id> <card-id>", cmd_assign, 3, 0),
	SHELL_CMD_ARG(unassign, NULL, "unassign <card-id>", cmd_unassign, 2, 0),
	SHELL_CMD_ARG(reset, NULL, "Erase the store and start over", cmd_reset, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(persondb, &persondb_cmds, "CBOR person/credential database", NULL);

int ui_main(void)
{
	int rc = scenario_open(&g_s);

	if (rc != 0) {
		printk("persondb: open failed: %d\n", rc);
		return rc;
	}
	g_open = true;

	struct persondb_stat st;

	persondb_stat(g_s.db, &st);
	printk("\npersondb shell ready — %u/%u persons at rev %u.\n",
	       st.populated, st.n_persons, st.rev);
	printk("Try: persondb stat | persondb fill 500 | persondb verify\n\n");
	return 0;
}
