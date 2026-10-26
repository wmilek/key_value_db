#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0
#
# Performance of the whole stack, as app_cbor_persondb sees it — at one scale,
# and as the number of credentials grows.
#
# THIS IS NOT BUILT. It is a host-side tool, stdlib only, in the style of
# tools/sizing.py and app_perf_l0/tools/l0_timing.py.
#
#   run      build + run the app on native_sim at each scale    -> sweep.json
#   parse    any console capture (a DK UART log, a saved run)   -> runs.json
#   results  the raw captures in RESULTS.md §8                  -> runs.json
#   report   one or more of the above                           -> markdown
#
#     python3 tools/perf_sweep.py run -o sweep.json
#     python3 tools/perf_sweep.py results RESULTS.md -o dk.json
#     python3 tools/perf_sweep.py report sweep.json dk.json
#
# Why a sweep, and why credentials. The benchmark's person count is a fixed
# constant (Kconfig APP_CBOR_PERSONDB_N_PERSONS, README practice 10), so the
# app answers "what does an operation cost at 8 000 persons". Every DK run
# above it shows the answer moves with scale, and not monotonically: reads got
# cheaper from 1 000 to 10 000 persons while `put` got 14x dearer
# (RESULTS.md §5f). One number cannot say that. A sweep rebuilds the same
# app at several N_PERSONS and nothing else, so every point is the shipped
# benchmark at a different size. The dataset gives each person 1..4 cards
# (dataset.c, mean 2.49), and the credential map is the one an access
# decision lands in first, so the x axis is reported as credentials stored.
#
# What a native_sim point means (RESULTS.md §1): the I/O counters transfer to
# the DK — the overlay mirrors its geometry, and §3b's counts matched the
# board within 2 % — while the host microseconds do not. So every point
# carries both, and the report never mixes them. Hardware time for the READ
# path is projected from the counters with the affine cost models RESULTS.md
# already validated (§5c, §5f: within ~1 %). The write path is not projected:
# the same model is out by 93 % on `put` at 10 000 persons, because its cost is
# sector erase that no counter reports (FINDINGS.md B3). For writes the report
# gives the counters and points at the DK captures.

import argparse
import datetime
import json
import os
import platform
import re
import statistics
import subprocess
import sys
from pathlib import Path

SCHEMA = "persondb-perf/1"

APP_DIR = Path(__file__).resolve().parent.parent

BENCHES = ("check", "byid", "miss", "put", "cbor")
READ_BENCHES = ("check", "byid", "miss")

# The scales the sweep visits by default. 8 000 is the benchmark constant and
# 10 000 the DK's full-scale run (RESULTS.md §5f); the points below fill in the
# curve, the ones above look for where it stops.
DEFAULT_PERSONS = (250, 500, 1000, 2000, 4000, 8000, 10000, 12000, 14000, 16000)

# --- cost models -----------------------------------------------------------
#
# Hardware time of a read-path operation, from its counters:
#
#     t = txn_us * flash_transactions + byte_us * flash_bytes + cpu_us
#
# cpu_us is the CBOR decode the operation performs, priced at the DK's own
# `cbor` bench (encode + decode, 0.95 ms) the way RESULTS.md §5c prices it.
# A `miss` decodes nothing.
MODELS = {
    "ubi-v0.1.0": {
        "label": "nRF5340-DK, UBI backend on zephyr-ubi v0.1.0 (the default)",
        "txn_us": 75.0,
        "byte_us": 0.63,
        "cbor_us": 950.0,
        "source": "app_perf_blob_db/RESULTS.md: flash_area 65 us/transaction "
                  "and 0.63 us/B (run 2), plus UBI v0.1.0's 7-13 us per "
                  "access (midpoint 10); cbor from app_cbor_persondb/"
                  "RESULTS.md §5g",
    },
    "ubi-fork": {
        "label": "nRF5340-DK, UBI backend on the old wmilek/ubi fork",
        "txn_us": 178.0,
        "byte_us": 0.616,
        "cbor_us": 950.0,
        "source": "app_cbor_persondb/RESULTS.md §5c, validated in §5e/§5f",
    },
    "flash_area": {
        "label": "nRF5340-DK, raw flash_area backend",
        "txn_us": 65.0,
        "byte_us": 0.63,
        "cbor_us": 950.0,
        "source": "app_perf_blob_db/RESULTS.md run 2; FINDINGS.md N1",
    },
}
DEFAULT_MODEL = "ubi-v0.1.0"

CPU_TERM = {"check": True, "byid": True, "miss": False}


def project_us(model, bench, per_op):
    """Projected hardware µs/op for a read bench, or None."""
    if bench not in READ_BENCHES or per_op.get("flash_ops") is None:
        return None
    t = (model["txn_us"] * per_op["flash_ops"]
         + model["byte_us"] * per_op["flash_bytes"])
    if CPU_TERM[bench]:
        t += model["cbor_us"]
    return t


# --- parsing a console capture --------------------------------------------
#
# Tolerant of everything else in the stream — log lines, ANSI colour, UART
# CR/LF — because the same parser reads a native_sim run and a capture pasted
# from a serial terminal. Lines are matched anywhere, not anchored, for the
# same reason.

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

BOOT_RE = re.compile(r"\*\*\* Booting Zephyr OS build (\S+)")
APP_RE = re.compile(r"persondb (\S+) — CBOR person/credential database")
CREATED_RE = re.compile(
    r"created store for (\d+) persons: people map depth (\d+), (\d+) buckets;"
    r" credential map depth (\d+), (\d+) buckets")
OPEN_RE = re.compile(r"^open\s+:\s+(-?\d+) ms")
PREPARE_RE = re.compile(r"^prepare\s+:\s+(-?\d+) ms \((\d+) buckets formatted\)")
FILL_RE = re.compile(
    r"^fill\s+:\s+(-?\d+) ms\s+(\d+) written, (\d+)/(\d+) total, (\d+) commits")
FILL_DONE_RE = re.compile(r"^fill\s+: already complete \((\d+) persons\)")
VERIFY_RE = re.compile(
    r"^(verify|re-verify)\s+:\s+(-?\d+) ms\s+(\d+) persons, (\d+) cards")
MUTATE_RE = re.compile(
    r"^mutate\s+:\s+(-?\d+) ms\s+rev (\d+) -> (\d+)\s+"
    r"\((\d+) revoked, (\d+) assigned\)")
BENCH_RE = re.compile(
    r"^bench (\w+)\s*:\s+(\d+) ops in\s+(\d+) us -> \s*[\d.]+ ops/s\s+"
    r"(\d+) us/op\s+(\d+) store ops\s+(\d+) flash ops\s+(\d+) B(\??)\s+"
    r"amp (\d+)x")
# Before the large-payload merge the line carried blob ops and a *modelled*
# amplification, and no flash counters (RESULTS.md §8). Read for its time only.
LEGACY_BENCH_RE = re.compile(
    r"^bench (\w+)\s*:\s+(\d+) ops in\s+(\d+) us -> \s*[\d.]+ ops/s\s+"
    r"(\d+) us/op\s+(\d+) blob ops\s+amp (\d+)x")
IO_RE = re.compile(
    r"^io (\w+)\s*:\s+rd\s+(\d+) ops/\s*(\d+) B\s+wr\s+(\d+) ops/\s*(\d+) B"
    r"\s+er\s+(\d+) ops/\s*(\d+) B")
DECIDE_RE = re.compile(
    r"^:\s+(\d+) granted, (\d+) denied, (\d+) unknown, (\d+) expired")
PART_RE = re.compile(r"^partition\s+:\s+(\d+) B")
MAPS_RE = re.compile(
    r"^maps\s+:\s+(?:\d+ )?person \(depth (\d+), (\d+) buckets\) \+ "
    r"(?:\d+ )?credential \(depth (\d+), (\d+) buckets\)")
PERSONS_RE = re.compile(r"^persons\s+:\s+(\d+) of (\d+)\s+credentials: (\d+)")
MEAN_RE = re.compile(r"^mean entry\s+:\s+(\d+) B")
LIVE_RE = re.compile(r"^live content: (\d+) B = (\d+)\.(\d) %")
OVERFLOW_RE = re.compile(r"^bucket overflows: (\d+)")
NEARFULL_RE = re.compile(r"^buckets past near-full: (\d+)")
STACK_RE = re.compile(r"^main stack\s+:\s+(\d+) of (\d+) B used")
DONE_RE = re.compile(r"persondb: done — store at rev (\d+)")
PASS_RE = re.compile(r"^VERIFY PASS")
FAIL_RE = re.compile(r"^(VERIFY FAIL.*)")
ERR_RE = re.compile(
    r"^((?:open|prepare|fill|verify|re-verify|mutate|bench \w+) failed: -?\d+"
    r"|fill incomplete.*)")
PUT_ERR_RE = re.compile(r"put person (\d+): (-?\d+)")
BOUNDARY_RE = re.compile(r"\*\*\* Booting Zephyr OS|persondb \S+ — CBOR")


def _split_boots(text):
    """One capture may hold several boots back to back; split them."""
    lines = [ANSI_RE.sub("", l).replace("\r", "") for l in text.splitlines()]
    boots, cur = [], []
    for line in lines:
        if BOUNDARY_RE.search(line) and any(
                BENCH_RE.search(l.strip()) or OPEN_RE.search(l.strip())
                for l in cur):
            boots.append(cur)
            cur = []
        cur.append(line)
    if cur:
        boots.append(cur)
    return boots


def _new_run():
    return {
        "zephyr": None, "app_version": None,
        "persons": None, "credentials": None,
        "kind": None,              # "fill" (this boot wrote persons) | "steady"
        "phases": {}, "bench": {}, "store": {}, "created": None,
        "verify_pass": 0, "status": "incomplete", "error": None,
    }


def parse_boot(lines):
    run = _new_run()
    for raw in lines:
        line = raw.strip()
        m = BOOT_RE.search(line)
        if m:
            run["zephyr"] = m.group(1)
        m = APP_RE.search(line)
        if m:
            run["app_version"] = m.group(1)
        m = CREATED_RE.search(line)
        if m:
            run["created"] = {
                "persons": int(m.group(1)),
                "people_depth": int(m.group(2)),
                "people_buckets": int(m.group(3)),
                "cred_depth": int(m.group(4)),
                "cred_buckets": int(m.group(5)),
            }
        m = PUT_ERR_RE.search(line)
        if m and run["error"] is None:
            run["error"] = f"put person {m.group(1)}: {m.group(2)}"
            run["phases"]["fill_failed_at"] = int(m.group(1))

        # Everything below is printed by ui_bench.c with a fixed prefix.
        if (m := OPEN_RE.search(line)):
            run["phases"]["open"] = {"ms": int(m.group(1))}
        elif (m := PREPARE_RE.search(line)):
            run["phases"]["prepare"] = {"ms": int(m.group(1)),
                                        "buckets": int(m.group(2))}
        elif (m := FILL_RE.search(line)):
            run["phases"]["fill"] = {
                "ms": int(m.group(1)), "written": int(m.group(2)),
                "populated": int(m.group(3)), "target": int(m.group(4)),
                "commits": int(m.group(5)),
            }
            run["kind"] = "fill"
        elif (m := FILL_DONE_RE.search(line)):
            run["phases"]["fill"] = {"ms": 0, "written": 0,
                                     "populated": int(m.group(1)),
                                     "target": int(m.group(1)), "commits": 0}
            run["kind"] = "steady"
        elif (m := VERIFY_RE.search(line)):
            key = "verify" if m.group(1) == "verify" else "reverify"
            run["phases"][key] = {"ms": int(m.group(2)),
                                  "persons": int(m.group(3)),
                                  "cards": int(m.group(4))}
        elif (m := MUTATE_RE.search(line)):
            run["phases"]["mutate"] = {
                "ms": int(m.group(1)), "rev_from": int(m.group(2)),
                "rev_to": int(m.group(3)), "revoked": int(m.group(4)),
                "assigned": int(m.group(5)),
            }
        elif PASS_RE.search(line):
            run["verify_pass"] += 1
        elif (m := FAIL_RE.search(line)):
            run["error"] = m.group(1)
            run["status"] = "verify-fail"
        elif (m := ERR_RE.search(line)):
            run["error"] = (run["error"] + "; " if run["error"] else "") \
                + m.group(1)
            run["status"] = "failed"
        elif (m := BENCH_RE.search(line)):
            ops = int(m.group(2))
            us = int(m.group(3))
            run["bench"][m.group(1)] = {
                "ops": ops, "us": us,
                "store_ops": int(m.group(5)), "flash_ops": int(m.group(6)),
                "flash_bytes": int(m.group(7)),
                "measured": m.group(8) != "?", "amp": int(m.group(9)),
                "io": None,
            }
        elif (m := LEGACY_BENCH_RE.search(line)):
            run["bench"][m.group(1)] = {
                "ops": int(m.group(2)), "us": int(m.group(3)),
                "store_ops": int(m.group(5)), "flash_ops": None,
                "flash_bytes": None, "measured": False, "amp": None,
                "io": None, "legacy": True,
            }
        elif (m := IO_RE.search(line)):
            b = run["bench"].get(m.group(1))
            if b is not None:
                b["io"] = {
                    "reads": int(m.group(2)), "bytes_read": int(m.group(3)),
                    "writes": int(m.group(4)),
                    "bytes_written": int(m.group(5)),
                    "erases": int(m.group(6)), "bytes_erased": int(m.group(7)),
                }
        elif (m := DECIDE_RE.search(line)):
            b = run["bench"].get("check")
            if b is not None:
                b["decisions"] = {
                    "granted": int(m.group(1)), "denied": int(m.group(2)),
                    "unknown": int(m.group(3)), "expired": int(m.group(4)),
                }
        elif (m := PART_RE.search(line)):
            run["store"]["partition_bytes"] = int(m.group(1))
        elif (m := MAPS_RE.search(line)):
            run["store"].update({
                "people_depth": int(m.group(1)),
                "people_buckets": int(m.group(2)),
                "cred_depth": int(m.group(3)),
                "cred_buckets": int(m.group(4)),
            })
        elif (m := PERSONS_RE.search(line)):
            run["store"].update({"populated": int(m.group(1)),
                                 "n_persons": int(m.group(2)),
                                 "credentials": int(m.group(3))})
        elif (m := MEAN_RE.search(line)):
            run["store"]["mean_entry"] = int(m.group(1))
        elif (m := LIVE_RE.search(line)):
            run["store"]["live_bytes"] = int(m.group(1))
            run["store"]["fill_pct"] = int(m.group(2)) + int(m.group(3)) / 10
        elif (m := OVERFLOW_RE.search(line)):
            run["store"]["overflows"] = int(m.group(1))
        elif (m := NEARFULL_RE.search(line)):
            run["store"]["near_full"] = int(m.group(1))
        elif (m := STACK_RE.search(line)):
            run["store"]["stack_used"] = int(m.group(1))
            run["store"]["stack_size"] = int(m.group(2))
        elif (m := DONE_RE.search(line)):
            run["store"]["rev"] = int(m.group(1))
            if run["status"] == "incomplete":
                run["status"] = "ok"

    st = run["store"]
    fill = run["phases"].get("fill")
    run["persons"] = (st.get("n_persons")
                      or (run["created"] or {}).get("persons")
                      or (fill or {}).get("target"))
    run["credentials"] = st.get("credentials")
    if run["kind"] is None and fill is None and run["phases"]:
        run["kind"] = "fill"   # a fill that failed before printing its line
    return run


def parse_capture(text, label=None):
    runs = []
    for boot in _split_boots(text):
        run = parse_boot(boot)
        if not run["phases"] and not run["bench"]:
            continue
        run["label"] = label
        runs.append(run)
    return runs


def per_op(b):
    """Per-operation view of one bench line. Floats; never truncated."""
    ops = b["ops"] or 1
    out = {
        "us_per_op": b["us"] / ops,
        "ops_per_s": (b["ops"] * 1e6 / b["us"]) if b["us"] else None,
        "store_ops": b["store_ops"] / ops,
        "flash_ops": b["flash_ops"] / ops if b.get("measured", True) else None,
        "flash_bytes": (b["flash_bytes"] / ops
                        if b.get("measured", True) else None),
        "amp": b["amp"],
    }
    io = b.get("io")
    if io:
        for k, v in io.items():
            out[k] = v / ops
    return out


# --- the dataset, for the one figure the console does not print ----------
#
# A fill that fails prints the person it failed on, not how many credentials
# were stored by then — and the x axis of a scaling curve is credentials. The
# count is a pure function of the index (README practice 7), so replay it: the
# same mixer as dataset.c, as tools/sizing.py does. Keep in step with
# dataset_n_cards().

def _mix(x, salt):
    m = 0xFFFFFFFF
    x = (x + salt * 0x9e3779b9) & m
    x ^= x >> 16
    x = (x * 0x7feb352d) & m
    x ^= x >> 15
    x = (x * 0x846ca68b) & m
    x ^= x >> 16
    return x


def credentials_of(persons):
    """Permanent credentials held by persons 0 .. persons-1 (no temp cards)."""
    return sum(1 + _mix(i, 3) % 4 for i in range(persons))


# --- aggregation: one point per scale -------------------------------------


def _median(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def _spread(xs):
    xs = [x for x in xs if x is not None]
    return [min(xs), max(xs)] if xs else None


def aggregate(runs):
    """Fold the runs of each scale into one point.

    Per-operation figures are the median over every run at that scale — the
    fill run and the steady-state reruns alike, since each benches the store
    as that boot found it — with the min/max beside it. The spread is not
    noise to be averaged away: per-operation flash traffic follows write
    history and where compaction last ran (RESULTS.md §3b), and at a few
    thousand persons `check` moves by 1.6x between consecutive runs. A single
    run would report whichever end of that range it happened to land on.
    """
    by_n = {}
    for r in runs:
        if r.get("persons"):
            by_n.setdefault(r["persons"], []).append(r)

    points = []
    for n in sorted(by_n):
        rs = by_n[n]
        fill_runs = [r for r in rs if r["kind"] == "fill"]
        steady = [r for r in rs if r["kind"] == "steady" and r["bench"]]
        basis = [r for r in rs if r["bench"]]
        ok = any(r["status"] == "ok" for r in rs)
        err = next((r["error"] for r in rs if r["error"]), None)
        fr = fill_runs[0] if fill_runs else None
        store = next((r["store"] for r in reversed(rs) if r["store"]), {})

        bench = {}
        for name in BENCHES:
            views = [per_op(r["bench"][name]) for r in basis
                     if name in r["bench"]]
            if not views:
                continue
            agg = {k: _median([v.get(k) for v in views]) for k in views[0]}
            for k in ("us_per_op", "flash_ops", "flash_bytes", "erases"):
                agg[f"spread_{k}"] = _spread([v.get(k) for v in views])
            agg["samples"] = basis[0]["bench"][name]["ops"]
            agg["runs"] = len(views)
            bench[name] = agg

        fill = None
        if fr and fr["phases"].get("fill"):
            f = fr["phases"]["fill"]
            fill = {"ms": f["ms"], "written": f["written"],
                    "ms_per_person": (f["ms"] / f["written"]
                                      if f["written"] else None),
                    "commits": f["commits"]}
        if fr and "fill_failed_at" in fr["phases"]:
            fill = fill or {}
            fill["failed_at"] = fr["phases"]["fill_failed_at"]
            fill["credentials_at_failure"] = credentials_of(fill["failed_at"])

        phases = {}
        for key in ("open", "prepare", "verify", "mutate", "reverify"):
            src = fr if (key in ("open", "prepare") and fr) else (
                basis[0] if basis else None)
            if src and key in src["phases"]:
                phases[key] = src["phases"][key]

        points.append({
            "persons": n,
            "credentials": store.get("credentials"),
            "status": "ok" if ok else (err or "incomplete"),
            "runs": len(rs),
            "steady_runs": len(steady),
            "store": store,
            "created": next((r["created"] for r in rs if r["created"]), None),
            "fill": fill,
            "phases": phases,
            "bench": bench,
        })
    return points


# --- run: build and execute on native_sim ---------------------------------


def _git(*args):
    try:
        return subprocess.run(["git", "-C", str(APP_DIR), *args],
                              capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def _host():
    cpu = None
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    return {"machine": platform.machine(), "cpu": cpu,
            "python": platform.python_version()}


def _build(args, n, bdir, log):
    cmd = ["west", "build", "-b", args.board, "-d", str(bdir),
           "-p", "always" if args.pristine else "auto", str(APP_DIR), "--",
           f"-DCONFIG_APP_CBOR_PERSONDB_N_PERSONS={n}",
           f"-DCONFIG_APP_CBOR_PERSONDB_BENCH_SAMPLES={args.samples}"]
    cmd += [f"-D{c}" if not c.startswith("-D") else c for c in args.config]
    with open(log, "w") as fh:
        rc = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT).returncode
    return rc, cmd


def _execute(exe, flash, fresh, timeout, log):
    cmd = [str(exe), f"--flash={flash}"]
    if fresh:
        cmd.append("--flash_erase")
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           errors="replace", timeout=timeout)
        out, rc = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode(errors="replace") \
            if isinstance(e.stdout, bytes) else (e.stdout or "")
        out += f"\n[perf_sweep] timed out after {timeout} s\n"
        rc = None
    Path(log).write_text(out)
    return out, rc


def cmd_run(args):
    if args.board != "native_sim":
        print("note: only native_sim can be run from here; for a board, "
              "flash it and feed the UART log to `parse`.", file=sys.stderr)
        return 2

    work = Path(args.work).resolve()
    (work / "logs").mkdir(parents=True, exist_ok=True)
    persons = ([int(x) for x in args.persons.split(",")]
               if args.persons else list(DEFAULT_PERSONS))

    doc = {
        "schema": SCHEMA,
        "generated": datetime.datetime.now(datetime.timezone.utc)
        .isoformat(timespec="seconds"),
        "source": {
            "kind": "native_sim",
            "board": args.board,
            "commit": _git("rev-parse", "--short=12", "HEAD"),
            # Tracked changes only: an untracked capture or sweep output in
            # the tree does not change what was built.
            "dirty": bool(_git("status", "--porcelain",
                               "--untracked-files=no", "--", ".")),
            "host": _host(),
            "samples": args.samples,
            "steady_runs": args.steady,
            "work": str(work),
            "config": args.config,
            "note": "I/O counters transfer to the DK (RESULTS.md §1, §3b); "
                    "host microseconds do not.",
        },
        "models": MODELS,
        "runs": [],
    }

    for n in persons:
        bdir = work / f"build-n{n}"
        flash = work / f"flash-n{n}.bin"
        print(f"[n={n}] build", flush=True)
        rc, _ = _build(args, n, bdir, work / "logs" / f"n{n}-build.log")
        if rc != 0:
            print(f"[n={n}] build failed (rc {rc}); see "
                  f"{work / 'logs' / f'n{n}-build.log'}", file=sys.stderr)
            doc["runs"].append({**_new_run(), "persons": n, "label": None,
                                "status": "build-failed",
                                "error": f"west build rc {rc}"})
            continue
        exe = bdir / "zephyr" / "zephyr.exe"

        for k in range(1 + args.steady):
            log = work / "logs" / f"n{n}-run{k + 1}.log"
            out, rc = _execute(exe, flash, k == 0, args.timeout, log)
            runs = parse_capture(out)
            if not runs:
                runs = [{**_new_run(), "persons": n, "status": "no-output"}]
            for r in runs:
                r["persons"] = r["persons"] or n
                r["label"] = f"native_sim n={n} run {k + 1}"
                r["log"] = str(log.relative_to(work))
                r["exit"] = rc
                if rc is None:
                    r["status"], r["error"] = "timeout", "timed out"
                doc["runs"].append(r)
            r = runs[-1]
            chk = r["bench"].get("check")
            print(f"[n={n}] run {k + 1} ({r['kind']}): {r['status']}"
                  + (f", check {chk['flash_ops'] / chk['ops']:.1f} flash "
                     f"ops/op" if chk else "")
                  + (f", {r['error']}" if r["error"] else ""), flush=True)
            if r["status"] != "ok":
                break   # a store that did not finish has no steady state

    doc["points"] = aggregate(doc["runs"])
    _write(doc, args.out)
    return 0


# --- parse / results: captures that did not come from `run` ---------------


def cmd_parse(args):
    runs = []
    for path in args.captures:
        text = Path(path).read_text(errors="replace")
        for r in parse_capture(text, args.label or Path(path).stem):
            r["log"] = str(path)
            runs.append(r)
    if not runs:
        raise SystemExit("no persondb bench output found in the capture(s)")
    doc = {"schema": SCHEMA,
           "generated": datetime.datetime.now(datetime.timezone.utc)
           .isoformat(timespec="seconds"),
           "source": {"kind": "capture", "board": args.board,
                      "files": [str(p) for p in args.captures]},
           "models": MODELS, "runs": runs, "points": aggregate(runs)}
    _write(doc, args.out)
    return 0


SECTION_RE = re.compile(r"^(#{2,3}) (.*)$")


def cmd_results(args):
    """Every fenced block under a RESULTS.md heading that holds a bench run.

    Reading the file rather than copies of it keeps one record: a capture
    appended to §8 later is picked up by rerunning this, with its heading as
    its label.
    """
    text = Path(args.results).read_text()
    runs = []
    h2 = h3 = None
    block = None
    for line in text.splitlines():
        if block is not None:
            if line.startswith("```"):
                label = h2 + (f" / {h3}" if h3 else "")
                for r in parse_capture("\n".join(block), label):
                    r["log"] = f"{args.results}: {label}"
                    if args.board:
                        r["board"] = args.board
                    runs.append(r)
                block = None
            else:
                block.append(line)
            continue
        if line.startswith("```"):
            block = []
            continue
        m = SECTION_RE.match(line)
        if m:
            if m.group(1) == "##":
                h2, h3 = m.group(2), None
            else:
                h3 = m.group(2)

    if args.match:
        runs = [r for r in runs if re.search(args.match, r["label"] or "")]
    if not runs:
        raise SystemExit("no bench captures found")
    # Different builds share a scale in RESULTS.md (§8a and §8c are both
    # 1 000 persons), so aggregate per section: a section is one build, and
    # its "Run 1" / "Run 2" subsections are that build's fill and steady state.
    sections = {}
    for r in runs:
        sections.setdefault(r["label"].split(" / ")[0], []).append(r)
    points = []
    for sec, rs in sections.items():
        for p in aggregate(rs):
            points.append({**p, "label": sec})

    doc = {"schema": SCHEMA,
           "generated": datetime.datetime.now(datetime.timezone.utc)
           .isoformat(timespec="seconds"),
           "source": {"kind": "results", "file": str(args.results),
                      "match": args.match},
           "models": MODELS, "runs": runs, "points": points}
    _write(doc, args.out)
    return 0


def _write(doc, out):
    text = json.dumps(doc, indent=1) + "\n"
    if out:
        Path(out).write_text(text)
        print(f"wrote {out}", file=sys.stderr)
    else:
        sys.stdout.write(text)


# --- report ----------------------------------------------------------------


def _f(x, nd=0, unit=""):
    if x is None:
        return "—"
    s = f"{x:,.{nd}f}".replace(",", " ")
    return s + unit


def _ms(us):
    return None if us is None else us / 1000


def _load(paths):
    docs = []
    for p in paths:
        d = json.loads(Path(p).read_text())
        if d.get("schema") != SCHEMA:
            raise SystemExit(f"{p}: not a {SCHEMA} file")
        docs.append(d)
    return docs


def report(docs, model_name, at):
    model = MODELS[model_name]
    out = []
    w = out.append

    sims = [d for d in docs if d["source"]["kind"] == "native_sim"]
    hws = [d for d in docs if d["source"]["kind"] != "native_sim"]

    for d in sims:
        src = d["source"]
        pts = [p for p in d["points"] if p["bench"]]
        if not pts:
            continue
        ref = next((p for p in pts if p["persons"] == at), None) or pts[-1]
        w(f"# persondb performance — native_sim sweep at "
          f"`{src.get('commit')}`{' (dirty)' if src.get('dirty') else ''}")
        w("")
        w(f"Generated {d['generated']}, {src['samples']} samples per bench, "
          f"{src['steady_runs']} steady-state run(s) per scale after the "
          f"fill; every figure is the median over a scale's runs. Host: "
          f"{src['host'].get('cpu') or src['host']['machine']}.")
        w("")
        w("Counters (transactions, bytes) transfer to the nRF5340-DK; host "
          "µs do not (RESULTS.md §1). *DK ms* is projected from the counters "
          f"with **{model_name}** — {model['label']}: "
          f"{model['txn_us']:g} µs/transaction + {model['byte_us']:g} µs/B"
          f" + {model['cbor_us']:g} µs CBOR where a record is decoded. "
          "Read path only: writes are erase-bound and the model does not "
          "cover them (RESULTS.md §5f).")
        w("")

        st = ref["store"]
        w(f"## Current performance — {_f(ref['persons'])} persons, "
          f"{_f(ref['credentials'])} credentials")
        w("")
        w(f"Store: {_f(st.get('fill_pct'), 1)} % of the partition live, "
          f"people map depth {st.get('people_depth')} × "
          f"{st.get('people_buckets')} buckets, credential map depth "
          f"{st.get('cred_depth')} × {st.get('cred_buckets')} buckets, "
          f"{st.get('overflows')} bucket overflows.")
        w("")
        w("| operation | flash txns/op | flash B/op | amp | host µs/op "
          "| **DK ms/op (proj.)** | **DK ops/s (proj.)** |")
        w("|---|--:|--:|--:|--:|--:|--:|")
        names = {"check": "`check` — card → person → permission",
                 "byid": "`byid` — person id → record",
                 "miss": "`miss` — unknown card",
                 "put": "`put` — replace a record + its index entries",
                 "cbor": "`cbor` — encode + decode, no flash"}
        for b in BENCHES:
            v = ref["bench"].get(b)
            if not v:
                continue
            pu = project_us(model, b, v)
            w(f"| {names[b]} | {_f(v['flash_ops'], 1)} "
              f"| {_f(v['flash_bytes'])} | {v['amp']}× "
              f"| {_f(v['us_per_op'], 1)} | {_f(_ms(pu), 2)} "
              f"| {_f(1e6 / pu if pu else None, 1)} |")
        fl = ref.get("fill") or {}
        w("")
        w(f"Fill (first run): {_f(fl.get('ms'))} ms host for "
          f"{_f(fl.get('written'))} persons — "
          f"{_f(fl.get('ms_per_person'), 2)} ms/person on the host.")
        w("")

        w("## Scaling with the number of credentials")
        w("")
        w("| persons | credentials | live | people / cred depth "
          "| check txns | check B | **check DK ms** | byid DK ms "
          "| miss DK ms | put txns | put B | put erases | host fill ms/person "
          "| status |")
        w("|--:|--:|--:|:-:|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|")
        for p in d["points"]:
            b = p["bench"]
            st = p["store"]
            c, y, m_, pt = (b.get(k) or {} for k in
                            ("check", "byid", "miss", "put"))
            fl = p.get("fill") or {}
            depth = (f"{st.get('people_depth')} / {st.get('cred_depth')}"
                     if st else "—")
            status = p["status"]
            if fl.get("failed_at") is not None:
                status = f"fill stopped at {fl['failed_at']:,}".replace(
                    ",", " ")
            w(f"| {p['persons']:,} | {_f(p['credentials'])} "
              f"| {_f(st.get('fill_pct'), 1, ' %')} | {depth} "
              f"| {_f(c.get('flash_ops'), 1)} | {_f(c.get('flash_bytes'))} "
              f"| {_f(_ms(project_us(model, 'check', c) if c else None), 2)} "
              f"| {_f(_ms(project_us(model, 'byid', y) if y else None), 2)} "
              f"| {_f(_ms(project_us(model, 'miss', m_) if m_ else None), 2)} "
              f"| {_f(pt.get('flash_ops'), 1)} | {_f(pt.get('flash_bytes'))} "
              f"| {_f(pt.get('erases'), 3)} "
              f"| {_f(fl.get('ms_per_person'), 2)} | {status} |"
              .replace(",", " "))
        w("")

    for d in hws:
        w(f"## Hardware captures — {d['source'].get('file') or ''}")
        w("")
        w("Measured on the board. *pred.* applies each model to the "
          "capture's own counters, so the column says how far the model "
          "can be trusted at that scale.")
        w("")
        w("| capture | persons | credentials | check ms | check pred. "
          "(fork / v0.1.0) | byid ms | miss ms | put ms | fill ms/person |")
        w("|---|--:|--:|--:|--:|--:|--:|--:|--:|")
        for r in d["runs"]:
            b = r["bench"]
            if "check" not in b:
                continue
            c = per_op(b["check"])
            preds = " / ".join(
                _f(_ms(project_us(MODELS[k], "check", c)), 2)
                for k in ("ubi-fork", "ubi-v0.1.0"))
            fl = r["phases"].get("fill") or {}
            mpp = fl["ms"] / fl["written"] if fl.get("written") else None
            w(f"| {r['label']} | {_f(r['persons'])} | {_f(r['credentials'])} "
              f"| {_f(_ms(c['us_per_op']), 2)} | {preds} | "
              + " | ".join(_f(_ms(per_op(b[k])['us_per_op']) if k in b
                              else None, 2) for k in ("byid", "miss", "put"))
              + f" | {_f(mpp, 1)} |")
        w("")
    return "\n".join(out) + "\n"


def cmd_report(args):
    text = report(_load(args.inputs), args.model, args.at)
    if args.out:
        Path(args.out).write_text(text)
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Whole-stack performance of app_cbor_persondb: one "
                    "scale, and how it moves with the number of credentials.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("run", help="build + run on native_sim per scale")
    p.add_argument("--persons", help="comma-separated person counts "
                   f"(default {','.join(map(str, DEFAULT_PERSONS))})")
    p.add_argument("--steady", type=int, default=2,
                   help="steady-state reruns after the fill (default 2)")
    p.add_argument("--samples", type=int, default=200,
                   help="CONFIG_APP_CBOR_PERSONDB_BENCH_SAMPLES (default 200, "
                        "the shipped value)")
    p.add_argument("--config", action="append", default=[],
                   metavar="CONFIG_X=Y", help="extra Kconfig, repeatable")
    p.add_argument("--board", default="native_sim")
    p.add_argument("--work", default="persondb-sweep",
                   help="build trees, flash images and logs (default "
                        "./persondb-sweep)")
    p.add_argument("--pristine", action="store_true",
                   help="west build -p always")
    p.add_argument("--timeout", type=int, default=3600,
                   help="seconds per run (default 3600)")
    p.add_argument("-o", "--out", help="write the JSON here (default stdout)")
    p.set_defaults(fn=cmd_run)

    p = sub.add_parser("parse", help="console capture(s) -> JSON")
    p.add_argument("captures", nargs="+")
    p.add_argument("--label")
    p.add_argument("--board", default="nrf5340dk/nrf5340/cpuapp")
    p.add_argument("-o", "--out")
    p.set_defaults(fn=cmd_parse)

    p = sub.add_parser("results", help="RESULTS.md raw captures -> JSON")
    p.add_argument("results", nargs="?", default=str(APP_DIR / "RESULTS.md"))
    p.add_argument("--match", help="keep captures whose heading matches "
                   "this regex")
    p.add_argument("--board", default=None)
    p.add_argument("-o", "--out")
    p.set_defaults(fn=cmd_results)

    p = sub.add_parser("report", help="JSON -> markdown")
    p.add_argument("inputs", nargs="+")
    p.add_argument("-m", "--model", choices=sorted(MODELS),
                   default=DEFAULT_MODEL)
    p.add_argument("--at", type=int, default=8000,
                   help="scale for 'current performance' (default 8000, "
                        "the benchmark constant)")
    p.add_argument("-o", "--out")
    p.set_defaults(fn=cmd_report)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
