/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_bench — the unattended run, and the source of RESULTS.md.
 *
 * Parsing and printing only: every operation below is one call into
 * scenario.h (DESIGN.md F15). Nothing here mentions a key, a shard, a blob id
 * or a CBOR byte.
 *
 * Sequence: open -> prepare -> fill (resuming, in batches) -> verify ->
 * mutate -> re-verify -> benchmarks -> report.
 *
 * Rerunning matters. The verify before the mutation round checks content that
 * a *previous boot* wrote, so persistence is proven rather than asserted:
 *
 *   native_sim : ./zephyr.exe --flash=persondb.bin   (add --flash_erase to
 *                                                     start over)
 *   hardware   : reset the board; set
 *                CONFIG_APP_CBOR_PERSONDB_FRESH_START=y once to wipe.
 */

#include <inttypes.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/app_version.h>

#include "scenario.h"

#ifdef CONFIG_ARCH_POSIX
#include "posix_board_if.h"
#endif

#define VERIFY_SAMPLES CONFIG_APP_CBOR_PERSONDB_VERIFY_SAMPLES
#define BENCH_SAMPLES  CONFIG_APP_CBOR_PERSONDB_BENCH_SAMPLES

static void print_bench(const struct bench_result *b)
{
	uint64_t ops_per_s = (b->us > 0)
				     ? (uint64_t)b->ops * 1000000ULL /
					       (uint64_t)b->us
				     : 0;

	printk("bench %-6s: %5u ops in %8" PRId64 " us -> %7" PRIu64 " ops/s  "
	       "%7u us/op  %6" PRIu64 " blob ops  amp %ux\n",
	       b->name, b->ops, b->us, ops_per_s, b->us_per_op, b->blob_ops,
	       b->amplification);
}

static void print_report(const struct scenario_report *r)
{
	printk("\nstore\n");
	printk("  partition   : %zu B (%zu KiB), %zu B sectors\n",
	       r->st.partition_bytes, r->st.partition_bytes / 1024,
	       r->st.sector_bytes);
	printk("  maps        : %u person + 1 credential, %u buckets each\n",
	       r->st.n_people_maps, r->st.buckets_per_map);
	printk("  persons     : %u of %u   credentials: %u\n",
	       r->st.populated, r->st.n_persons, r->credentials);
	printk("  mean entry  : %u B\n", r->mean_record);
	printk("  live content: %" PRIu64 " B = %u.%u %% of the partition\n",
	       r->logical_bytes, r->fill_permille / 10, r->fill_permille % 10);
	printk("  bucket overflows: %u\n", r->st.enospc_hits);
	if (r->stack_size) {
		printk("  main stack  : %zu of %zu B used\n", r->stack_used,
		       r->stack_size);
	}
	printk("  NOTE: physical occupancy (live + uncompacted garbage) is not\n"
	       "        observable through the API — see FINDINGS.md B3.\n");
}

int ui_main(void)
{
	struct scenario s = { 0 };
	struct scenario_report rep;
	int rc;

	printk("\npersondb %s — CBOR person/credential database\n",
	       APP_VERSION_STRING);

	int64_t t = k_uptime_get();

	rc = scenario_open(&s);
	if (rc != 0) {
		printk("open failed: %d\n", rc);
		goto out;
	}
	printk("open         : %6" PRId64 " ms\n", k_uptime_delta(&t));

	/* Take the one-off sector erases out of the timed loops so the fill
	 * measures the warm write path (and so their cost is visible). */
	int prepared = 0;
	int64_t prep_ms = 0;

	rc = scenario_prepare(&s, &prepared, &prep_ms);
	if (rc != 0) {
		printk("prepare failed: %d\n", rc);
		goto out;
	}
	printk("prepare      : %6" PRId64 " ms (%d buckets formatted)\n",
	       prep_ms, prepared);

	/* Fill. Resumable across reboots: this returns when the dataset is
	 * complete, so a reset mid-fill costs at most one batch. */
	struct scenario_fill_report fill;

	rc = scenario_fill(&s, 0, &fill);
	if (rc != 0) {
		printk("fill failed: %d\n", rc);
		goto out;
	}
	if (fill.written) {
		printk("fill         : %6" PRId64 " ms  %u written, %u/%u total, "
		       "%u commits\n",
		       fill.ms, fill.written, fill.populated, fill.target,
		       fill.batches);
	} else {
		printk("fill         : already complete (%u persons)\n",
		       fill.populated);
	}
	if (fill.enospc) {
		printk("*** %u bucket overflows — the sizing rule in DESIGN.md "
		       "§6.1 is wrong (FINDINGS.md K2)\n", fill.enospc);
	}
	if (!fill.complete) {
		printk("fill incomplete — rerun to continue\n");
		goto out;
	}

	/* Verify what a previous boot wrote (on a rerun) or what the fill just
	 * wrote (on the first run). */
	struct scenario_verify_report ver;

	rc = scenario_verify(&s, VERIFY_SAMPLES, &ver);
	if (rc != 0) {
		printk("verify failed: %d\n", rc);
		goto out;
	}
	printk("verify       : %6" PRId64 " ms  %u persons, %u cards\n", ver.ms,
	       ver.persons_checked, ver.cards_checked);
	if (ver.bad_records || ver.bad_cards || ver.missing) {
		printk("VERIFY FAIL: %u bad records, %u bad cards, %u missing\n",
		       ver.bad_records, ver.bad_cards, ver.missing);
		rc = -EILSEQ;
		goto out;   /* leave the store intact for inspection */
	}
	printk("VERIFY PASS\n");

	/* One mutation round, then prove it took. */
	struct scenario_mutate_report mut;

	rc = scenario_mutate(&s, &mut);
	if (rc != 0) {
		printk("mutate failed: %d\n", rc);
		goto out;
	}
	printk("mutate       : %6" PRId64 " ms  rev %u -> %u  "
	       "(%u revoked, %u assigned)\n",
	       mut.ms, mut.rev_from, mut.rev_to, mut.revoked, mut.assigned);

	rc = scenario_verify(&s, VERIFY_SAMPLES, &ver);
	if (rc != 0) {
		printk("re-verify failed: %d\n", rc);
		goto out;
	}
	printk("re-verify    : %6" PRId64 " ms  %u persons, %u cards\n", ver.ms,
	       ver.persons_checked, ver.cards_checked);
	if (ver.bad_records || ver.bad_cards || ver.missing) {
		printk("VERIFY FAIL after mutation: %u bad records, %u bad "
		       "cards, %u missing\n",
		       ver.bad_records, ver.bad_cards, ver.missing);
		rc = -EILSEQ;
		goto out;
	}
	printk("VERIFY PASS\n\n");

	for (enum scenario_bench_kind k = 0; k < SCENARIO_BENCH_KINDS; k++) {
		struct bench_result b;

		rc = scenario_bench(&s, k, BENCH_SAMPLES, &b);
		if (rc != 0) {
			printk("bench %s failed: %d\n",
			       scenario_bench_name(k), rc);
			goto out;
		}
		print_bench(&b);
		if (k == SCENARIO_BENCH_CHECK) {
			printk("             : %u granted, %u denied, %u unknown, "
			       "%u expired\n",
			       b.granted, b.denied, b.unknown, b.expired);
		}
	}

	rc = scenario_report_get(&s, &rep);
	if (rc == 0) {
		print_report(&rep);
	}

	printk("\npersondb: done — store at rev %u; rerun to prove persistence\n",
	       mut.rev_to);
	rc = 0;

out:
	scenario_close(&s);
#ifdef CONFIG_ARCH_POSIX
	posix_exit(rc == 0 ? 0 : 1);
#endif
	return 0;
}
