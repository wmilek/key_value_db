#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0
"""Render a twister run as a GitHub Actions step summary.

Twister's own summary only reaches the raw job log, where it is buried under
tens of thousands of build lines — the reason a green run reads as "no tests
ran". This turns twister-out/twister.json into a markdown table that the
Actions UI shows directly on the run page.

Usage:  twister_summary.py <twister.json>   (markdown is written to stdout)

Never fails the job: reporting is not a test result. If the report is missing
or unreadable this says so and exits 0, leaving the twister step itself as the
authority on pass/fail.
"""
import json
import sys
from pathlib import Path

# Suite statuses twister assigns to a configuration that was deliberately not
# executed, as opposed to one that ran. Anything else counts as "executed".
NOT_EXECUTED = {"notrun", "not run", "filtered", "skipped"}

ICONS = {
    "passed": "✅",
    "failed": "❌",
    "error": "💥",
    "skipped": "⏭️",
    "filtered": "⏭️",
    "notrun": "🔨",
    "not run": "🔨",
}


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} <twister.json>", file=sys.stderr)
        return 0

    report = Path(sys.argv[1])
    try:
        suites = json.loads(report.read_text())["testsuites"]
    except (OSError, ValueError, KeyError) as err:
        print(f"_No twister report at `{report}` ({err})._")
        return 0

    executed = [s for s in suites if s.get("status") not in NOT_EXECUTED]
    built_only = len(suites) - len(executed)
    # A case a suite chose to skip (e.g. a flash_area-layout test under the UBI
    # backend) never ran, so it belongs in neither half of the pass ratio.
    cases = [c for s in executed for c in s.get("testcases", [])]
    ran = [c for c in cases if c.get("status") not in NOT_EXECUTED]
    passed = [c for c in ran if c.get("status") == "passed"]
    skipped = len(cases) - len(ran)
    bad = [s for s in suites if s.get("status") in ("failed", "error")]

    verdict = "❌ failures" if bad else "✅ all green"
    print("### Twister — native_sim\n")
    print(
        f"{verdict} — **{len(passed)}/{len(ran)} test cases passed** across "
        f"{len(executed)} executed configuration(s); "
        f"{skipped} skipped, {built_only} configuration(s) built but not run.\n"
    )

    print("| Configuration | Status | Cases | Time |")
    print("| --- | --- | ---: | ---: |")
    for suite in suites:
        status = suite.get("status", "?")
        icon = ICONS.get(status, "•")
        tcs = suite.get("testcases", [])
        # Built-only configurations have testcases listed but never executed;
        # showing a count there would imply they ran.
        count = len(tcs) if status not in NOT_EXECUTED else "—"
        print(
            f"| `{suite.get('name', '?')}` | {icon} {status} | {count} "
            f"| {suite.get('execution_time', '0')}s |"
        )

    if bad:
        print("\n<details open><summary>Failures</summary>\n")
        for suite in bad:
            print(f"**`{suite.get('name', '?')}`** — {suite.get('reason', 'no reason given')}")
            for case in suite.get("testcases", []):
                if case.get("status") in ("failed", "error"):
                    print(f"- `{case.get('identifier', '?')}`")
            print()
        print("</details>")

    return 0


if __name__ == "__main__":
    sys.exit(main())
