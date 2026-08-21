#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0
#
# The timing model: from an app_perf_l0 capture to predicted hardware
# wall-clock for a run that happened on native_sim.
#
# THIS IS NOT BUILT. It is a host-side tool, stdlib only, in the style of
# app_cbor_persondb/tools/sizing.py.
#
#   1. fit      app_perf_l0 capture (hardware)  ->  model.json
#   2. predict  any run's blob_db I/O counters  +  model.json  ->  milliseconds
#   3. verify   a capture with BOTH counters and wall-clock -> how wrong it is
#   4. simconf  model.json -> CONFIG_FLASH_SIMULATOR_* knobs
#
# The model is affine in each operation class:
#
#     read  t(n) = R0 + R1*n         n = bytes in the transfer
#     write t(n) = W0 + W1*n
#     erase t(m) = E0 + E1*m         m = erase blocks the call covers
#
# Affine is not a convenience. It is the reason the prediction can be made at
# all: summing an affine cost over a set of operations depends only on the
# COUNT and the TOTAL SIZE, never on how the bytes were distributed between
# calls. Those two numbers per class are exactly what CONFIG_BLOB_DB_IOSTATS
# already counts, so
#
#     T = R0*reads  + R1*bytes_read
#       + W0*writes + W1*bytes_written
#       + E0*erases + E1*blocks_erased
#
# is exact for the model, with no per-operation trace required. Where the real
# part is not affine — a NOR page program is a step function of the transfer
# size, not a line — the error shows up in the fit residuals, which every
# command here prints rather than hides.
#
# What the prediction does NOT include: the CPU time the layers above L0 spend
# between flash calls (CRC, memcpy, the slot scan). On a part where an erase is
# a second and a page program is milliseconds that is noise; on a fast part it
# is not, and `verify` is how you find out which case you are in.

import argparse
import json
import re
import sys
from pathlib import Path

SCHEMA = "l0-timing-model/1"

# --- parsing the benchmark capture --------------------------------------


def _kv(line):
    """`l0raw op=read size=64 ops=512 ...` -> dict of str->str."""
    out = {}
    for tok in line.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            out[k] = v
    return out


def _int(d, key, default=None):
    v = d.get(key)
    if v is None:
        return default
    try:
        return int(v, 0)
    except ValueError:
        return default


def parse_l0_capture(text):
    """Pull the l0geom / l0raw records out of a console capture.

    Tolerant of everything else in the stream: log lines, boot banners, the
    human-readable table, and the CR/LF and stray bytes a UART capture picks
    up. Only lines that *start* with a known record name are read.
    """
    geom = None
    rows = {"read": [], "write": [], "write_unaligned": [], "write_pg": [],
            "erase": [], "erase1": []}
    lin = {}
    status = None

    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("l0geom "):
            geom = _kv(line)
        elif line.startswith("l0raw "):
            d = _kv(line)
            op = d.get("op")
            if op not in rows:
                continue
            ops = _int(d, "ops", 0)
            total = _int(d, "total_ns", 0)
            if not ops or total is None:
                continue
            rec = {
                "ops": ops,
                "total_ns": total,
                "ns_per_op": total / ops,
            }
            if op in ("erase", "erase1"):
                rec["blocks"] = _int(d, "blocks", 1)
                rec["min_ns"] = _int(d, "min_ns", 0)
                rec["max_ns"] = _int(d, "max_ns", 0)
            else:
                rec["size"] = _int(d, "size", 0)
            rows[op].append(rec)
        elif line.startswith("l0lin "):
            # The device's own linearity summary: the range of the marginal
            # cost column, in centi-ns per byte. Kept because it is the
            # measurement the fit's single slope replaces, and a reader of the
            # model should be able to see what was averaged away.
            d = _kv(line)
            op = d.get("op")
            lo = _int(d, "cmarg_min", 0)
            hi = _int(d, "cmarg_max", 0)
            if op:
                lin[op] = {
                    "marginal_min_ns_per_b": lo / 100.0,
                    "marginal_max_ns_per_b": hi / 100.0,
                    "spread": (hi / lo) if lo > 0 else None,
                    # Newer captures measure each point several times and let
                    # the device drop steps whose change is smaller than the
                    # two points' own pass-to-pass spread. Whether it could do
                    # that changes what its range means, so record it.
                    "overall_ns_per_b": (_int(d, "overall_cmarg", None) / 100.0
                                         if d.get("overall_cmarg") else None),
                    "used": _int(d, "used", None),
                    "skipped": _int(d, "skipped", None),
                }
        elif line.startswith("l0end "):
            status = _int(_kv(line), "status", None)

    if geom is None:
        raise SystemExit(
            "no l0geom line in the capture — is this an app_perf_l0 log?"
        )
    return geom, rows, lin, status


# --- the fit -------------------------------------------------------------


def wls(points, weight="rel"):
    """Weighted least squares for y = a + b*x over (x, y) pairs.

    `weight='rel'` weights each point by 1/y^2, which minimises RELATIVE
    error. That is the right objective here and the default: the sweep spans
    five decades of transfer size, and unweighted least squares would fit the
    64 KB point almost exclusively — while the operations this model is asked
    to predict are overwhelmingly tens of bytes.

    Returns (a, b, note). Both coefficients are clamped to >= 0: a negative
    fixed cost or a negative cost per byte is not a model, it is an artefact
    of noise in a nearly flat curve, and it would silently subtract time from
    every prediction. When a clamp fires the other coefficient is re-fitted
    with the clamped one held at zero, so the result is still the best model
    of that (restricted) shape rather than half of a discarded one.
    """
    n = len(points)
    if n == 0:
        return 0.0, 0.0, "no points"
    if n == 1:
        x, y = points[0]
        return (y, 0.0, "single point: all cost attributed to the fixed term")

    def solve(pts):
        """Centered normal equations, not the textbook raw-moment ones.

        The raw form computes b as (S*Sxy - Sx*Sy) / (S*Sxx - Sx^2), and it
        failed here in two different ways before this was rewritten — both of
        them silent.

        On the DK capture it failed at the *guard*: the code rejected a
        near-singular system with `abs(det) < 1e-12`, but det carries the
        units of the weights, and weight="rel" makes those 1/y^2. The capture
        emits nanoseconds, so det landed at 1.2e-13 for a perfectly
        conditioned 15-point sweep and the guard threw it away, returning the
        weighted mean with slope zero. The same data in microseconds gives
        det=0.12 and passes: a pure change of time unit flipped the model from
        affine to flat.

        On synthetic data it failed at the *arithmetic*: with weights spanning
        ten decades and x spanning five, both halves of each difference are
        enormous and nearly equal, the subtraction cancels away most of the
        significant digits, and the slope came back NEGATIVE on points lying
        exactly on a line — which then tripped the non-negativity clamp and
        produced a flat model at 99.8 % error.

        Centering on the weighted mean removes both. There is no det to scale
        a guard against, and the sums being accumulated are already deviations,
        so nothing large is subtracted from anything large. Sxx is a sum of
        non-negative terms and is zero only when every x is identical, which
        is the one genuinely degenerate case.
        """
        sw = sx = sy = 0.0
        for x, y in pts:
            w = 1.0 / (y * y) if (weight == "rel" and y > 0) else 1.0
            sw += w
            sx += w * x
            sy += w * y
        if sw <= 0:
            return 0.0, 0.0
        xbar, ybar = sx / sw, sy / sw

        sxx = sxy = 0.0
        for x, y in pts:
            w = 1.0 / (y * y) if (weight == "rel" and y > 0) else 1.0
            dx = x - xbar
            sxx += w * dx * dx
            sxy += w * dx * (y - ybar)
        if sxx <= 0:
            return ybar, 0.0
        b = sxy / sxx
        return ybar - b * xbar, b

    a, b = solve(points)
    note = ""

    # Snap a slope that is indistinguishable from zero. On a flat curve the
    # fit lands on a tiny positive number rather than a negative one, which
    # survives the clamp below and then shows up downstream as a throughput
    # "ceiling" of 10^20 KB/s. If the per-unit term contributes less than 1 %
    # of the cheapest measured operation across the WHOLE swept range, the
    # sweep did not measure it.
    max_x = max(x for x, _ in points)
    min_y = min(y for _, y in points)
    if 0 < b * max_x < 0.01 * min_y:
        b = 0.0
        note = ("per-unit cost is below what this sweep can resolve; "
                "clamped to a flat per-call cost")

    if b <= 0:
        # Cost does not grow with size at all (the flash simulator, or a part
        # whose per-call overhead swamps the transfer). Flat model.
        num = den = 0.0
        for x, y in points:
            w = 1.0 / (y * y) if (weight == "rel" and y > 0) else 1.0
            num += w * y
            den += w
        a, b = (num / den if den else 0.0), 0.0
        if not note:
            note = ("per-unit cost fitted negative; clamped to a flat "
                    "per-call cost")
    elif a < 0:
        # Pure throughput, no measurable per-call overhead.
        num = den = 0.0
        for x, y in points:
            w = 1.0 / (y * y) if (weight == "rel" and y > 0) else 1.0
            num += w * x * y
            den += w * x * x
        a, b = 0.0, (num / den if den else 0.0)
        note = "fixed cost fitted negative; clamped to zero"
    return a, b, note


def gated_marginals(points, a, b):
    """Marginal cost (ns/B) over pairs that can actually support the estimate.

    A first difference between *adjacent* sweep points is a poor estimator of
    the slope, and it gets worse as the sweep gets finer. The error in dt/dn
    is about 2*eps*t/dn for a per-point noise eps, so it grows as dn shrinks:
    on the DK matrix capture the 1.5x midpoints (dn/n = 0.33..0.5) turned ~5 %
    batch-to-batch variance into an 18 % swing, and one outlying batch — a 6 B
    read measuring 112.8 us where its neighbours measured 63..79 — produced a
    marginal of -18752 ns/B. A cost that cannot be negative was reported as
    negative, and the verdict built on that range called an affine part "NOT
    affine". So the sweep that was made finer to *see* structure made this
    particular test blinder.

    Two gates, both structural rather than tuned:

    - **Only above the fixed-cost crossover, n >= a/b.** Below it the byte
      term is a minority of the measurement, so a difference there is mostly
      noise. This is the same crossover the throughput lines already print
      (266 B read, 13 B write on the DK), not a new constant.
    - **Pair across an octave, not with the neighbour.** Each surviving point
      is paired with the first later point at >= 2x its size, so dn >= n and
      noise passes through at ~2*eps instead of being amplified by n/dn.

    `b` comes from the fit, which is weighted least squares over every point,
    so it does not inherit this problem.

    A staircase still trips the test: secants across an octave of a
    page-quantised cost disagree with each other (29.5 vs 19.7 us/B for a
    256 B page), which is what the spread is read for.

    Returns (slopes, n_used, n_excluded) with slopes as
    (lo_size, hi_size, ns_per_b).
    """
    if b <= 0:
        return [], 0, len(points)
    crossover = a / b
    pts = sorted(points)
    usable = [(x, y) for x, y in pts if x >= crossover]
    excluded = len(pts) - len(usable)
    slopes = []
    for i, (x0, y0) in enumerate(usable):
        for x1, y1 in usable[i + 1:]:
            if x1 >= 2 * x0:
                slopes.append((x0, x1, (y1 - y0) / (x1 - x0)))
                break
    return slopes, len(slopes), excluded


def residuals(points, a, b):
    """Per-point relative error of the fitted line, worst-first summary."""
    out = []
    for x, y in points:
        pred = a + b * x
        rel = (pred - y) / y if y else 0.0
        out.append({"x": x, "measured_ns": y, "predicted_ns": pred, "rel": rel})
    worst = max((abs(r["rel"]) for r in out), default=0.0)
    return out, worst


def build_model(geom, rows, lin=None, name=None, weight="rel",
                size_range=None):
    block = _int(geom, "block_bytes", 0)

    def pts_rw(op):
        p = []
        for r in rows[op]:
            size = r["size"]
            if size_range and not (size_range[0] <= size <= size_range[1]):
                continue
            p.append((float(size), float(r["ns_per_op"])))
        return sorted(p)

    # Erase points come from both erase records: the span sweep gives the
    # slope (is a 16-block call cheaper than sixteen 1-block calls?) and the
    # erase1 record gives a well-averaged 1-block anchor.
    pts_er = []
    for r in rows["erase"]:
        pts_er.append((float(r["blocks"]), float(r["ns_per_op"])))
    for r in rows["erase1"]:
        pts_er.append((1.0, float(r["ns_per_op"])))
    pts_er.sort()

    model = {}
    curve = {}
    gated = {}
    for op, pts, unit in (
        ("read", pts_rw("read"), "per_byte_ns"),
        ("write", pts_rw("write"), "per_byte_ns"),
        ("erase", pts_er, "per_block_ns"),
    ):
        a, b, note = wls(pts, weight)
        res, worst = residuals(pts, a, b)
        model[op] = {
            "fixed_ns": a,
            unit: b,
            "points": len(pts),
            "max_rel_error": worst,
            "note": note,
            "residuals": res,
        }
        curve[op] = [[x, y] for x, y in pts]

        # Gated marginal range: the honest version of the device's l0lin
        # summary. Only for the transfer classes — erase is swept over block
        # counts, where every adjacent pair already spans an octave or more.
        if op in ("read", "write") and b > 0 and len(pts) >= 3:
            slopes, used, rej = gated_marginals(pts, a, b)
            if used:
                vals = [m for _, _, m in slopes]
                gated[op] = {
                    "min": min(vals),
                    "max": max(vals),
                    "used": used,
                    "rejected": rej,
                    "pairs": [[lo, hi, m] for lo, hi, m in slopes],
                }

    if gated:
        model["gated_marginal"] = gated

    # The page-straddle probe is reported, never folded into the fit: it is a
    # different operation (one transfer, two program pages) and averaging it
    # into the aligned curve would make every prediction slightly pessimistic
    # and the residuals slightly prettier.
    straddle = []
    aligned = {r["size"]: r["ns_per_op"] for r in rows["write"]}
    for r in rows["write_unaligned"]:
        base = aligned.get(r["size"])
        straddle.append(
            {
                "size": r["size"],
                "aligned_ns": base,
                "unaligned_ns": r["ns_per_op"],
                "penalty": (r["ns_per_op"] / base) if base else None,
            }
        )

    # The page-program staircase: page-aligned transfers, so the cost is
    # exactly ceil(size/page) programs if the part programs by page. Reported,
    # never fitted — it is a different operation from the packed sweep, and it
    # exists to explain the packed sweep's residuals rather than to improve
    # them.
    page = _int(geom, "program_page", 0) or 0
    pg_rows = sorted(((r["size"], r["ns_per_op"]) for r in rows["write_pg"]))
    one_page = None
    for sz, ns in pg_rows:
        if sz <= page:
            one_page = ns if one_page is None else min(one_page, ns)
    staircase = []
    for sz, ns in pg_rows:
        expect = -(-sz // page) if page else 0
        staircase.append({
            "size": sz,
            "ns": ns,
            "expected_programs": expect,
            "implied_programs": (ns / one_page) if one_page else None,
        })

    out = {
        "schema": SCHEMA,
        "name": name or geom.get("board", "unnamed"),
        "source": geom.get("source", "unknown"),
        "timing": geom.get("timing", "unknown"),
        "board": geom.get("board", "unknown"),
        "geometry": {
            "part_bytes": _int(geom, "part_bytes", 0),
            "block_bytes": block,
            "blocks": _int(geom, "blocks", 0),
            "write_align": _int(geom, "write_align", 1),
            "erased_val": _int(geom, "erased_val", 0xFF),
        },
        "fit": {"weight": weight, "size_range": list(size_range) if size_range else None},
        "model": model,
        "page_straddle": straddle,
        "program_page": page,
        "page_program": staircase,
        "linearity": lin or {},
        "curve": curve,
    }
    return out


# --- parsing a run to predict -------------------------------------------

# `   io read      : rd    605 ops/    8650 B   wr     0 ops/       0 B   er    0 ...`
# The erase field gained an `ops/N B` tail when this tool was written; both
# spellings parse, and the older one is flagged where it matters.
IO_RE = re.compile(
    r"io\s+(?P<phase>\S+(?:\s+\S+)*?)\s*:\s*"
    r"rd\s+(?P<rd>\d+)\s+ops/\s*(?P<rdb>\d+)\s*B\s+"
    r"wr\s+(?P<wr>\d+)\s+ops/\s*(?P<wrb>\d+)\s*B\s+"
    r"er\s+(?P<er>\d+)(?:\s+ops/\s*(?P<erb>\d+)\s*B)?"
)

# `bench read   :  100 ops in      46 ms  ->  ...`
BENCH_RE = re.compile(
    r"bench\s+(?P<phase>\S+(?:\s+\S+)*?)\s*:\s*(?P<ops>\d+)\s+ops\s+in\s+"
    r"(?P<ms>\d+)\s+ms"
)

# blob_db's own geometry line, printed at CONFIG_BLOB_DB_LOG_LEVEL_INF.
GEOM_RE = re.compile(
    r"partition\s+(?P<part>\d+)\s*B,\s*(?P<n>\d+)\s+sectors\s+of\s+"
    r"(?P<sec>\d+)\s*B"
)


def parse_run(text):
    """Extract per-phase I/O counters (and measured ms, where present)."""
    phases = []
    by_name = {}
    geom = None
    pending_bench = []

    for raw in text.splitlines():
        line = raw.strip()

        m = GEOM_RE.search(line)
        if m and geom is None:
            geom = {
                "part_bytes": int(m.group("part")),
                "block_bytes": int(m.group("sec")),
                "blocks": int(m.group("n")),
            }

        m = BENCH_RE.search(line)
        if m:
            pending_bench.append((m.group("phase").strip(), int(m.group("ops")),
                                  int(m.group("ms"))))
            continue

        m = IO_RE.search(line)
        if m:
            name = m.group("phase").strip()
            rec = {
                "phase": name,
                "reads": int(m.group("rd")),
                "bytes_read": int(m.group("rdb")),
                "writes": int(m.group("wr")),
                "bytes_written": int(m.group("wrb")),
                "erases": int(m.group("er")),
                "bytes_erased": (int(m.group("erb"))
                                 if m.group("erb") is not None else None),
                "measured_ms": None,
                "ops": None,
            }
            # An `io X` line is emitted right after the `bench X` line that
            # timed the same phase; pair them by name, most recent first.
            for i in range(len(pending_bench) - 1, -1, -1):
                bname, bops, bms = pending_bench[i]
                if bname == name:
                    rec["measured_ms"] = bms
                    rec["ops"] = bops
                    del pending_bench[i]
                    break
            phases.append(rec)
            by_name.setdefault(name, []).append(rec)

    return phases, geom


# --- prediction ----------------------------------------------------------


def predict_phase(model, ph, block_bytes):
    m = model["model"]
    r0, r1 = m["read"]["fixed_ns"], m["read"]["per_byte_ns"]
    w0, w1 = m["write"]["fixed_ns"], m["write"]["per_byte_ns"]
    e0, e1 = m["erase"]["fixed_ns"], m["erase"]["per_block_ns"]

    warn = None
    if ph["bytes_erased"] is not None and block_bytes:
        blocks = ph["bytes_erased"] / block_bytes
    else:
        blocks = float(ph["erases"])
        if ph["erases"]:
            warn = ("erase bytes not in the log; assuming 1 block per erase "
                    "call — a whole-partition erase_all() is one call over "
                    "many blocks, so this UNDER-counts")

    rd = r0 * ph["reads"] + r1 * ph["bytes_read"]
    wr = w0 * ph["writes"] + w1 * ph["bytes_written"]
    er = e0 * ph["erases"] + e1 * blocks
    return {
        "read_ns": rd,
        "write_ns": wr,
        "erase_ns": er,
        "total_ns": rd + wr + er,
        "blocks": blocks,
        "warn": warn,
    }


def fmt_time(ns):
    if ns >= 1e9:
        return f"{ns / 1e9:10.3f} s "
    if ns >= 1e6:
        return f"{ns / 1e6:10.3f} ms"
    return f"{ns / 1e3:10.3f} us"


def check_geometry(model, run_geom, strict):
    """A prediction is only meaningful if the run's geometry is the target's.

    blob_db derives its bucket count, slot alignment and segment size from the
    geometry it finds at mount, so a native_sim run on the simulator's native
    4 KB blocks issues a different NUMBER of operations than the same code on
    a 64 KB part. Multiplying those counts by the target's per-operation cost
    produces a confident answer to the wrong question, which is worse than no
    answer — hence the check, and hence geometry/mx25r64.overlay.
    """
    if not run_geom:
        return ["run log carries no geometry line (build it with "
                "CONFIG_BLOB_DB_LOG_LEVEL_INF=y to enable this check)"]
    msgs = []
    mg = model["geometry"]
    for key, label in (("block_bytes", "erase block"), ("part_bytes", "partition")):
        if run_geom.get(key) and mg.get(key) and run_geom[key] != mg[key]:
            msgs.append(
                f"{label} mismatch: model {mg[key]} B, run {run_geom[key]} B"
                " — the run's operation counts are not the ones the target"
                " would produce"
            )
    if msgs and strict:
        raise SystemExit("geometry mismatch:\n  " + "\n  ".join(msgs))
    return msgs


# --- deriving a model without an L0 run ---------------------------------

# Column order of the design matrix used by `derive`.
TERMS = ("reads", "bytes_read", "writes", "bytes_written", "erases", "blocks")


def nnls_rel(rows, ys, iters=500):
    """Non-negative least squares, minimising RELATIVE error.

    Coordinate descent: with an all-non-negative design matrix and a convex
    objective it converges quickly and needs no linear algebra library, which
    keeps this tool stdlib-only like the rest of the repository's tooling.

    Non-negativity is not a nicety. Unconstrained, this system happily returns
    a negative cost per read that cancels against an inflated erase cost — a
    perfect fit to these observations and nonsense on any other workload.
    """
    n = len(TERMS)
    x = [0.0] * n
    w = [1.0 / (y * y) if y > 0 else 0.0 for y in ys]

    # Start from a plausible point: all the time attributed to whichever term
    # is largest in each row does not matter, but a zero start makes the first
    # sweep behave like a greedy fit, which is fine.
    for _ in range(iters):
        moved = 0.0
        for j in range(n):
            num = den = 0.0
            for i, row in enumerate(rows):
                a = row[j]
                if a == 0.0:
                    continue
                # Residual with column j removed.
                pred = sum(row[k] * x[k] for k in range(n)) - a * x[j]
                num += w[i] * a * (ys[i] - pred)
                den += w[i] * a * a
            new = max(0.0, num / den) if den > 0 else 0.0
            moved = max(moved, abs(new - x[j]))
            x[j] = new
        if moved < 1e-12:
            break
    return x


def derive_observations(paths):
    obs = []
    for p in paths:
        phases, _ = parse_run(Path(p).read_text(errors="replace"))
        for ph in phases:
            if ph["measured_ms"] is None:
                continue
            obs.append((p, ph))
    if not obs:
        raise SystemExit(
            "no usable observations: `derive` needs phases that print BOTH "
            "an `io …` counter line and a `bench …` wall-clock line"
        )
    return obs


def solve_derived(obs, block_bytes):
    rows, ys = [], []
    collinear = True
    for _, ph in obs:
        if ph["bytes_erased"] is not None and block_bytes:
            blocks = ph["bytes_erased"] / block_bytes
        else:
            blocks = float(ph["erases"])
        if blocks != float(ph["erases"]):
            collinear = False
        rows.append([
            float(ph["reads"]), float(ph["bytes_read"]),
            float(ph["writes"]), float(ph["bytes_written"]),
            float(ph["erases"]), blocks,
        ])
        ys.append(ph["measured_ms"] * 1e6)  # ns

    if collinear:
        # Every observation has exactly one block per erase call, so the
        # per-call and per-block terms cannot be told apart. Attributing the
        # cost to the PER-BLOCK term is the safe half of that choice: a NOR
        # part erases block by block, and a model that put the cost on the
        # call would predict a whole-partition erase_all() — one call, many
        # blocks — as costing a single block erase.
        for r in rows:
            r[4] = 0.0

    x = nnls_rel(rows, ys)
    return x, rows, ys, collinear


def cmd_derive(args):
    """Fit a model from stack-level hardware observations, not from an L0 run.

    An app_perf capture already contains, for each phase, the I/O counters and
    the wall-clock they produced. That is a linear system in the same six
    coefficients the L0 sweep measures directly, so it can be solved for them
    — and it can be solved from captures taken years ago, on a board that is
    no longer on the desk.

    What it cannot do is separate flash time from the CPU time above it: any
    per-operation CPU cost lands in the fixed terms and inflates them. So a
    derived model is a PROVISIONAL stand-in, honest about its provenance, and
    an app_perf_l0 run on the board supersedes it.
    """
    obs = derive_observations(args.captures)
    block = args.block_bytes
    x, rows, ys, collinear = solve_derived(obs, block)

    def pred(row):
        return sum(a * b for a, b in zip(row, x))

    model = {
        "schema": SCHEMA,
        "name": args.name or "derived",
        "source": "derived",
        "timing": "real",
        "board": args.board or "unknown",
        "geometry": {
            "part_bytes": args.part_bytes,
            "block_bytes": block,
            "blocks": (args.part_bytes // block) if (block and args.part_bytes)
                      else 0,
            "write_align": args.write_align,
            "erased_val": 0xFF,
        },
        "fit": {"weight": "rel", "size_range": None, "method": "nnls"},
        "model": {
            "read": {"fixed_ns": x[0], "per_byte_ns": x[1],
                     "points": len(obs), "max_rel_error": 0.0, "note": "",
                     "residuals": []},
            "write": {"fixed_ns": x[2], "per_byte_ns": x[3],
                      "points": len(obs), "max_rel_error": 0.0, "note": "",
                      "residuals": []},
            "erase": {"fixed_ns": x[4], "per_block_ns": x[5],
                      "points": len(obs), "max_rel_error": 0.0, "note": "",
                      "residuals": []},
        },
        "page_straddle": [],
        "curve": {"read": [], "write": [], "erase": []},
        "notes": [
            "derived from stack-level observations (counters + wall-clock), "
            "not from a direct L0 measurement",
            "per-operation CPU time above L0 is folded into the fixed terms",
        ],
        "derivation": {
            "captures": list(args.captures),
            "observations": [
                {"source": src, "phase": ph["phase"],
                 "reads": ph["reads"], "bytes_read": ph["bytes_read"],
                 "writes": ph["writes"], "bytes_written": ph["bytes_written"],
                 "erases": ph["erases"], "bytes_erased": ph["bytes_erased"],
                 "measured_ms": ph["measured_ms"]}
                for src, ph in obs
            ],
        },
    }
    if collinear:
        model["notes"].append(
            "every observation erased exactly one block per call, so the "
            "per-call erase term is unidentifiable; all erase cost is "
            "attributed per block"
        )

    worst = 0.0
    print(f"\nderived from {len(obs)} observation(s) in "
          f"{len(set(s for s, _ in obs))} capture(s)\n")
    print("phase              predicted      measured    error")
    for (src, ph), row, y in zip(obs, rows, ys):
        p = pred(row)
        rel = (p - y) / y if y else 0.0
        worst = max(worst, abs(rel))
        print(f"{ph['phase']:<16} {fmt_time(p)} {fmt_time(y)}  "
              f"{rel * 100:+7.1f} %")
    for cls in ("read", "write", "erase"):
        model["model"][cls]["max_rel_error"] = worst
    print(f"\nworst residual {worst * 100:.1f} %")

    if args.loo:
        print("\nleave-one-out cross-validation "
              "(refit without each phase, then predict it):")
        errs = []
        for i in range(len(obs)):
            sub = [o for j, o in enumerate(obs) if j != i]
            xs, _, _, _ = solve_derived(sub, block)
            p = sum(a * b for a, b in zip(rows[i], xs))
            rel = (p - ys[i]) / ys[i] if ys[i] else 0.0
            errs.append(abs(rel))
            print(f"  {obs[i][1]['phase']:<16} predicted {fmt_time(p)} "
                  f"vs {fmt_time(ys[i])}  {rel * 100:+7.1f} %")
        errs.sort()
        print(f"  median |error| {errs[len(errs) // 2] * 100:.1f} %, "
              f"worst {errs[-1] * 100:.1f} %")
        model["derivation"]["loo_median_abs_error"] = errs[len(errs) // 2]
        model["derivation"]["loo_worst_abs_error"] = errs[-1]

    if args.out:
        Path(args.out).write_text(json.dumps(model, indent=2) + "\n")
        print(f"\nwrote {args.out}")
    show_model(model)
    return 0



# --- measured against the datasheet ---------------------------------------

SPEC_SCHEMA = "l0-part-spec/1"


def verdict(measured, typ, mx):
    """Where a measured value falls in the part's specified envelope.

    Typ is not a limit — it is one operating point (25 C, typical VCC), and a
    part sitting above it is normal. Max IS a limit, quoted at 85 C and
    minimum VCC, so a room-temperature measurement above max is the part or
    the driver doing something the datasheet does not allow for.
    """
    if mx and measured > mx:
        return "OVER MAX", f"{measured / mx:.2f}x max"
    if typ and measured < typ:
        return "under typ", f"{measured / typ:.2f}x typ"
    if typ:
        return "typ..max", f"{measured / typ:.2f}x typ"
    return "?", ""


def cmd_spec(args):
    """Compare a fitted model against a part's datasheet.

    The interesting output is not the pass/fail. It is which MODE the numbers
    are consistent with: on this part the two modes differ by 2-4x, the mode
    bit is volatile with a part-code-dependent power-on value, and Zephyr's
    driver never writes it — so which one a board is actually running in is
    not knowable from the schematic, only from a measurement.
    """
    model = load_model(args.model)
    spec = json.loads(Path(args.spec).read_text())
    if spec.get("schema") != SPEC_SCHEMA:
        raise SystemExit(f"{args.spec}: not a {SPEC_SCHEMA} file")

    block = model["geometry"]["block_bytes"]
    page = spec.get("page_bytes") or model.get("program_page") or 256
    m = model["model"]

    # What the model says the part does, reduced to the two quantities the
    # datasheet specifies. The fixed terms are deliberately not compared:
    # they are bus time and driver overhead, which the datasheet's internal
    # erase/program figures do not include.
    meas_erase = m["erase"]["per_block_ns"]
    meas_page = m["write"]["per_byte_ns"] * page
    meas_byte = m["write"]["per_byte_ns"]
    page_src = "per-byte slope x page"

    # A directly measured page-aligned point beats the slope, when there is
    # one: it is the same operation the datasheet is specifying.
    for st in (model.get("page_program") or []):
        if st["size"] == page and st["ns"]:
            meas_page = st["ns"] - m["write"]["fixed_ns"]
            page_src = "write_pg row at one page, less the fixed term"
            break

    print(f"\nmodel '{model['name']}' vs {spec['part']}")
    print(f"  {spec['source']}")
    if model["source"] != "hardware":
        print(f"  !! the model's source is '{model['source']}', so what is "
              "being checked against the datasheet\n     is not a direct "
              "measurement of the part")
    print(f"\n  measured: erase {meas_erase / 1e6:.1f} ms per {block} B "
          f"block; program {meas_page / 1e6:.3f} ms per {page} B page "
          f"({page_src});\n            {meas_byte / 1000:.2f} us per byte")

    viable = []
    for mode, sp in spec["modes"].items():
        eb = (sp.get("erase_block_ns") or {}).get(str(block))
        pp = sp.get("page_program")
        bp = sp.get("byte_program")
        print(f"\n  {mode}  ({sp['config']})")
        print("    parameter            measured        typ        max   "
              "verdict")
        ok = True
        rows = []
        if eb:
            rows.append((f"block erase {block} B", meas_erase,
                         eb.get("typ_ns"), eb.get("max_ns"), 1e6, "ms"))
        if pp:
            rows.append((f"page program {page} B", meas_page,
                         pp.get("typ_ns"), pp.get("max_ns"), 1e6, "ms"))
        if bp:
            rows.append(("per byte programmed", meas_byte,
                         bp.get("typ_ns"), bp.get("max_ns"), 1e3, "us"))
        for label, val, typ, mx, sc, unit in rows:
            v, how = verdict(val, typ, mx)
            if v == "OVER MAX":
                ok = False
            print(f"    {label:<20} {val / sc:8.2f}{unit} "
                  f"{(typ or 0) / sc:8.2f}{unit} {(mx or 0) / sc:8.2f}{unit}   "
                  f"{v} ({how})")
        if ok:
            viable.append(mode)

    print()
    if len(viable) == 1:
        print(f"  -> consistent with {viable[0]} only: every other mode has a "
              "maximum this\n     measurement exceeds. Since the mode bit is "
              "volatile and nothing in the\n     firmware writes it, that is "
              "the mode the part powers up in.")
    elif not viable:
        print("  -> consistent with NO mode: something exceeds a datasheet "
              "maximum. Either the\n     part is not what the board file "
              "says, or the measurement includes time\n     that is not the "
              "part's (a driver retry, a busy-poll interval, a suspended\n"
              "     erase), or it was taken far from room temperature.")
    else:
        print(f"  -> consistent with {' and '.join(viable)}; this measurement "
              "does not separate them.\n     Compare against typ rather than "
              "max to see which it sits closer to.")

    print("\n  Not compared: the model's fixed per-call terms "
          f"(read {m['read']['fixed_ns'] / 1000:.1f} us, "
          f"write {m['write']['fixed_ns'] / 1000:.1f} us). Those are bus\n"
          "  time and driver overhead; the datasheet figures above are the "
          "part's internal\n  erase and program times and do not include "
          "them.")
    for n in spec.get("notes", [])[:2]:
        print(f"\n  note: {n}")
    return 0


# --- commands ------------------------------------------------------------


def cmd_fit(args):
    text = Path(args.capture).read_text(errors="replace")
    geom, rows, lin, status = parse_l0_capture(text)

    if status not in (0, None):
        print(f"warning: capture reports l0end status={status}; the sweep did "
              f"not finish cleanly", file=sys.stderr)

    measured = sum(r["total_ns"] for op in rows for r in rows[op])
    if measured == 0:
        raise SystemExit(
            "every span in that capture timed to 0 ns, so there is nothing to "
            "fit. On native_sim this means the build lacks "
            "CONFIG_FLASH_SIMULATOR_SIMULATE_TIMING: the clock measures "
            "SIMULATED time and host CPU work costs none of it. See "
            "boards/native_sim.conf."
        )

    rng = None
    if args.size_range:
        lo, _, hi = args.size_range.partition(":")
        rng = (int(lo, 0), int(hi, 0))

    model = build_model(geom, rows, lin, name=args.name,
                        weight=args.weight, size_range=rng)

    if model["source"] != "hardware":
        model.setdefault("notes", []).append(
            "fitted from the flash simulator, not from a part: use for "
            "self-test only"
        )

    out = Path(args.out) if args.out else None
    if out:
        out.write_text(json.dumps(model, indent=2, sort_keys=False) + "\n")
        print(f"wrote {out}")
    show_model(model)
    if not out:
        print("\n(no -o given; model not saved)")
    return 0


def show_model(model):
    g = model["geometry"]
    print(f"\nmodel: {model['name']}   source={model['source']} "
          f"timing={model['timing']} board={model['board']}")
    print(f"geometry: partition {g['part_bytes']} B, {g['blocks']} blocks of "
          f"{g['block_bytes']} B, write align {g['write_align']} B")
    if model["source"] == "derived":
        print("!! PROVISIONAL — solved from stack-level observations, not "
              "measured at L0")
    elif model["source"] != "hardware":
        print("!! NOT A HARDWARE MODEL — fitted from "
              f"{model['source']}; predictions from it describe the simulator")

    m = model["model"]
    print("\n  class   fixed per call        per unit            points  "
          "max rel err")
    print(f"  read  {m['read']['fixed_ns'] / 1000:12.3f} us  "
          f"{m['read']['per_byte_ns']:10.1f} ns/B    "
          f"{m['read']['points']:5d}  {m['read']['max_rel_error'] * 100:8.1f} %")
    print(f"  write {m['write']['fixed_ns'] / 1000:12.3f} us  "
          f"{m['write']['per_byte_ns']:10.1f} ns/B    "
          f"{m['write']['points']:5d}  {m['write']['max_rel_error'] * 100:8.1f} %")
    print(f"  erase {m['erase']['fixed_ns'] / 1000:12.3f} us  "
          f"{m['erase']['per_block_ns'] / 1e6:10.3f} ms/blk  "
          f"{m['erase']['points']:5d}  {m['erase']['max_rel_error'] * 100:8.1f} %")

    for op in ("read", "write", "erase"):
        if m[op]["note"]:
            print(f"  note ({op}): {m[op]['note']}")

    # Derived numbers a human actually reasons with.
    r = m["read"]
    w = m["write"]
    # Throughput is a consequence of BOTH terms, and saying so here is the
    # guard against reading a constant marginal cost as a constant speed. The
    # fixed cost is charged on every call whatever it carries, so a caller
    # below the break-even size gets a fraction of the ceiling.
    for label, cls in (("read ", r), ("write", w)):
        if cls["per_byte_ns"] <= 0:  # the sweep resolved no size term
            continue
        ceil_kib = 1e9 / cls["per_byte_ns"] / 1024
        be = _breakeven(cls, "per_byte_ns")
        small = 64.0
        got = small / ((cls["fixed_ns"] + cls["per_byte_ns"] * small) / 1e9) / 1024
        print(f"\n  {label} ceiling {ceil_kib:.0f} KiB/s, but only for large "
              f"transfers: the fixed cost\n        dominates below "
              f"{be:.0f} B, so a {small:.0f} B transfer gets "
              f"{got:.0f} KiB/s ({got / ceil_kib * 100:.0f} % of it)")

    # The linearity evidence, which is the part of the capture the single
    # fitted slope replaces. Printed next to the fit so the two are read
    # together: a slope with a 10x marginal spread behind it is an average,
    # not a description.
    lin = model.get("linearity") or {}
    gm = (model.get("model") or {}).get("gated_marginal") or {}
    if lin or gm:
        print("\n  marginal cost across the sweep (d ns / d B) — constant "
              "iff the cost is affine:")
        for op in ("read", "write"):
            e = lin.get(op)
            g = gm.get(op)
            if not e and not g:
                continue
            if e is None:
                # Capture predates the device's l0lin line. The gated range is
                # computed from the swept points, so it needs nothing from it.
                print(f"    {op:<6} device summary absent in this capture")
                e = None

            # The device differences ADJACENT points, which cannot support a
            # verdict — see gated_marginals(). Its range is still printed,
            # because it is what the board measured and a reader should be
            # able to see what the gate removed, but the verdict comes from
            # the gated pairs.
            if e is not None:
                if e.get("used") is not None:
                    label = (f"device, adjacent pairs, noise-gated "
                             f"({e['used']} used, {e['skipped']} below the "
                             f"points' own spread)")
                else:
                    label = "device, adjacent pairs, ungated"
                print(f"    {op:<6} {label}:\n           "
                      f"{e['marginal_min_ns_per_b']:9.2f} .. "
                      f"{e['marginal_max_ns_per_b']:9.2f} ns/B"
                      + (f"   (whole-sweep slope "
                         f"{e['overall_ns_per_b']:.2f} ns/B)"
                         if e.get("overall_ns_per_b") else ""))

            if not g or g["used"] < 2:
                print(f"    {'':<6} gated: fewer than two octave pairs "
                      f"above the crossover -> no verdict")
                continue

            lo, hi = g["min"], g["max"]
            sp = (hi / lo) if lo > 0 else None
            if hi <= 0:
                verdict = "flat: cost does not depend on size"
            elif sp is None:
                verdict = ("NOT affine: flat over part of the sweep, "
                           "stepped elsewhere")
            elif sp <= 1.25:
                verdict = "affine"
            else:
                verdict = f"NOT affine, {sp:.2f}x spread"
            print(f"    {'':<6} gated, octave pairs ({g['used']} used, "
                  f"{g['rejected']} points below the crossover excluded): "
                  f"{lo:9.2f} .. {hi:9.2f} ns/B   -> {verdict}")

    stair = model.get("page_program") or []
    if stair:
        page = model.get("program_page") or 0
        print(f"\n  page-program staircase (page-aligned transfers, "
              f"page = {page} B):")
        print("      size        us/op   programs  expected  verdict")
        agree = 0
        for st in stair:
            imp = st["implied_programs"]
            exp = st["expected_programs"]
            if imp is None:
                continue
            near = abs(imp - exp) <= 0.25
            agree += 1 if near else 0
            print(f"    {st['size']:6d} B  {st['ns'] / 1000:11.3f}  "
                  f"{imp:9.2f}  {exp:8d}  {'step' if near else 'off'}")
        if agree == len([x for x in stair if x['implied_programs']]):
            print("    -> write cost tracks ceil(size / page): it is a "
                  "staircase, and the fitted\n       per-byte slope is an "
                  "average over it. A transfer one byte past a page\n"
                  "       boundary costs a whole extra program.")
        else:
            print("    -> cost does NOT track ceil(size / page); the part or "
                  "driver is not\n       programming a page at a time.")

    for s in model.get("page_straddle", []):
        if s["penalty"]:
            print(f"  page straddle at {s['size']} B: "
                  f"{s['penalty']:.2f}x the aligned cost")

    for n in model.get("notes", []):
        print(f"  note: {n}")

    # Always printed, never behind a flag. A fitted slope is a claim about
    # data, and this table is the data the claim was made from: every swept
    # point, what the straight line says it should have cost, and by how much
    # the two differ. Without it a model is an assertion; with it the reader
    # can see where the interpolation holds and where it does not — which is
    # not uniform, and matters most exactly where a workload's transfers sit.
    for op in ("read", "write", "erase"):
        res = m[op]["residuals"]
        if not res:
            continue
        unit = "blk" if op == "erase" else "B"
        sc, us = (1e6, "ms") if op == "erase" else (1e3, "us")
        a = m[op]["fixed_ns"]
        b = m[op].get("per_byte_ns", m[op].get("per_block_ns", 0.0))
        print(f"\n  {op}: t(n) = {a / sc:.3f} {us} + "
              f"{b / (sc if op == 'erase' else 1):.4f} "
              f"{us + '/' + unit if op == 'erase' else 'ns/B'} * n")
        print(f"    {'n':>9}     measured      fitted     error")
        for r_ in res:
            print(f"    {r_['x']:9.0f} {unit} {r_['measured_ns'] / sc:11.3f}{us}"
                  f" {r_['predicted_ns'] / sc:10.3f}{us} "
                  f"{r_['rel'] * 100:+8.1f} %")
        worst = max(res, key=lambda r_: abs(r_["rel"]))
        print(f"    worst {worst['rel'] * 100:+.1f} % at {worst['x']:.0f} "
              f"{unit}")


def _breakeven(cls, unit_key):
    """Transfer size at which the per-unit term equals the fixed term."""
    if cls[unit_key] <= 0:
        return float("inf")
    return cls["fixed_ns"] / cls[unit_key]


def load_model(path):
    model = json.loads(Path(path).read_text())
    if model.get("schema") != SCHEMA:
        raise SystemExit(f"{path}: not a {SCHEMA} file")
    return model


def _phases_from_args(args):
    if args.run:
        text = Path(args.run).read_text(errors="replace")
        phases, geom = parse_run(text)
        if not phases:
            raise SystemExit(
                f"{args.run}: no `io …` counter lines found. The run needs "
                "CONFIG_BLOB_DB_IOSTATS=y and an app that prints the counters."
            )
        return phases, geom
    # Counters given directly, for a run whose output shape this tool does not
    # know: anything that can report blob_db_iostats_get() can be predicted.
    return [{
        "phase": "counters",
        "reads": args.reads,
        "bytes_read": args.bytes_read,
        "writes": args.writes,
        "bytes_written": args.bytes_written,
        "erases": args.erases,
        "bytes_erased": args.bytes_erased,
        "measured_ms": None,
        "ops": None,
    }], None


def cmd_predict(args):
    model = load_model(args.model)
    phases, run_geom = _phases_from_args(args)

    if model["source"] == "derived":
        print("note: this model was derived from stack-level observations, "
              "not measured at L0;\n      an app_perf_l0 run on the board "
              "supersedes it.", file=sys.stderr)
    elif model["source"] != "hardware" and not args.allow_simulated:
        raise SystemExit(
            f"{args.model} was fitted from '{model['source']}', not from "
            "hardware; predicting a board's timing from it would be "
            "circular. Pass --allow-simulated if that is what you meant."
        )

    for msg in check_geometry(model, run_geom, args.strict_geometry):
        print(f"warning: {msg}", file=sys.stderr)

    block = model["geometry"]["block_bytes"]
    print(f"\npredicting on model '{model['name']}' "
          f"({model['board']}, {block} B blocks)\n")
    print("phase             reads/bytes        writes/bytes      "
          "erases/blocks       read      write      erase        TOTAL")

    total = 0.0
    warned = set()
    results = []
    for ph in phases:
        p = predict_phase(model, ph, block)
        total += p["total_ns"]
        results.append((ph, p))
        if p["warn"]:
            warned.add(p["warn"])
        print(f"{ph['phase']:<16} {ph['reads']:6d}/{ph['bytes_read']:<10d} "
              f"{ph['writes']:6d}/{ph['bytes_written']:<10d} "
              f"{ph['erases']:5d}/{p['blocks']:<8.0f} "
              f"{p['read_ns'] / 1e6:9.1f}m {p['write_ns'] / 1e6:9.1f}m "
              f"{p['erase_ns'] / 1e6:9.1f}m {fmt_time(p['total_ns'])}")

    print(f"\n{'sum of phases':<16} {'':>58}{fmt_time(total)}")
    print("\n(times in the three class columns are milliseconds; they are the "
          "flash\n cost only — CPU time between flash calls is not modelled)")
    for w in warned:
        print(f"\nwarning: {w}", file=sys.stderr)

    if args.json:
        Path(args.json).write_text(json.dumps(
            {"model": model["name"],
             "phases": [{**ph, "predicted": p} for ph, p in results],
             "total_ns": total}, indent=2) + "\n")
        print(f"\nwrote {args.json}")
    return 0


def cmd_verify(args):
    """Predict a capture that also contains its own measured wall-clock.

    This is the only command that can tell you whether the model is any good,
    and it is deliberately unforgiving: it prints the ratio for every phase,
    not an average, because a model that is right on reads and 3x wrong on
    erases is not 'about right'.
    """
    model = load_model(args.model)
    text = Path(args.run).read_text(errors="replace")
    phases, run_geom = parse_run(text)
    if not phases:
        raise SystemExit(f"{args.run}: no `io …` counter lines found")

    for msg in check_geometry(model, run_geom, args.strict_geometry):
        print(f"warning: {msg}", file=sys.stderr)

    block = model["geometry"]["block_bytes"]
    print(f"\nverifying model '{model['name']}' against {args.run}\n")
    print("phase              predicted      measured    ratio   "
          "dominant class")

    ratios = []
    for ph in phases:
        if ph["measured_ms"] is None:
            continue
        p = predict_phase(model, ph, block)
        meas_ns = ph["measured_ms"] * 1e6
        ratio = p["total_ns"] / meas_ns if meas_ns else float("inf")
        ratios.append((ph["phase"], ratio))
        dom = max((("read", p["read_ns"]), ("write", p["write_ns"]),
                   ("erase", p["erase_ns"])), key=lambda t: t[1])
        share = dom[1] / p["total_ns"] * 100 if p["total_ns"] else 0
        print(f"{ph['phase']:<16} {fmt_time(p['total_ns'])} "
              f"{fmt_time(meas_ns)}  {ratio:7.2f}x   {dom[0]} ({share:.0f}%)")

    if not ratios:
        raise SystemExit(
            "no phase in that capture has both counters and a wall-clock "
            "line; verify needs a hardware run of an app that prints both"
        )

    vals = sorted(r for _, r in ratios)
    med = vals[len(vals) // 2]
    worst = max(ratios, key=lambda t: abs(1.0 - t[1]))
    print(f"\nmedian ratio {med:.2f}x over {len(ratios)} phases; "
          f"worst {worst[0]} at {worst[1]:.2f}x")
    print("ratio > 1: the model over-predicts (it charges for flash the run "
          "did not pay for).\nratio < 1: under-predicts — usually CPU time "
          "above L0 that the model omits,\n           or a non-affine cost "
          "the affine fit smoothed away.")
    return 0


def cmd_simconf(args):
    """Kconfig for the flash simulator that makes native_sim tick like a part.

    The simulator charges ONE flat cost per call, with no per-byte and no
    per-block term, so this can only ever be a single point on the model's
    line. Which point matters: pick it from the operation mix of the workload
    you care about, not from the middle of the sweep. With --run, the mean
    transfer size of that run is used; without it, the mean of the swept sizes.

    A native_sim build configured this way is a rough stand-in for pacing and
    watchdog behaviour. It is NOT a substitute for `predict`, which uses the
    whole model.
    """
    model = load_model(args.model)
    m = model["model"]
    block = model["geometry"]["block_bytes"]

    if args.run:
        phases, _ = parse_run(Path(args.run).read_text(errors="replace"))
        reads = sum(p["reads"] for p in phases)
        writes = sum(p["writes"] for p in phases)
        erases = sum(p["erases"] for p in phases)
        brd = sum(p["bytes_read"] for p in phases)
        bwr = sum(p["bytes_written"] for p in phases)
        ber = sum((p["bytes_erased"] or 0) for p in phases)
        rd_size = brd / reads if reads else 0
        wr_size = bwr / writes if writes else 0
        er_blocks = (ber / block / erases) if (erases and block and ber) else 1.0
        src = f"operation mix of {args.run}"
    else:
        c = model["curve"]
        rd_size = sum(x for x, _ in c["read"]) / max(len(c["read"]), 1)
        wr_size = sum(x for x, _ in c["write"]) / max(len(c["write"]), 1)
        er_blocks = 1.0
        src = "mean of the swept sizes (no --run given)"
        if not c["read"] and not c["write"]:
            print("# WARNING: this model carries no sweep curve (a derived "
                  "model does not),\n#          so the operating point is 0 "
                  "bytes: only the fixed terms are used.\n#          Pass "
                  "--run to pick the point from a real operation mix.")

    rd_us = (m["read"]["fixed_ns"] + m["read"]["per_byte_ns"] * rd_size) / 1000
    wr_us = (m["write"]["fixed_ns"] + m["write"]["per_byte_ns"] * wr_size) / 1000
    er_us = (m["erase"]["fixed_ns"] + m["erase"]["per_block_ns"] * er_blocks) / 1000

    def clamp(v):
        return max(1, min(1000000, int(round(v))))

    print(f"# flash-simulator timing from model '{model['name']}'")
    print(f"# operating point: {src}")
    print(f"#   read  at {rd_size:.0f} B/op, write at {wr_size:.0f} B/op, "
          f"erase at {er_blocks:.2f} block(s)/call")
    print("# The simulator has no per-byte term, so an operation of any other")
    print("# size is charged this same cost. Use `predict` for numbers.")
    print("CONFIG_FLASH_SIMULATOR_SIMULATE_TIMING=y")
    print(f"CONFIG_FLASH_SIMULATOR_MIN_READ_TIME_US={clamp(rd_us)}")
    print(f"CONFIG_FLASH_SIMULATOR_MIN_WRITE_TIME_US={clamp(wr_us)}")
    print(f"CONFIG_FLASH_SIMULATOR_MIN_ERASE_TIME_US={clamp(er_us)}")
    if er_us > 1000000:
        print(f"# NOTE: the true erase cost is {er_us / 1000:.0f} ms, above "
              f"the Kconfig ceiling of 1 s; it is clamped.")
    return 0


def cmd_show(args):
    show_model(load_model(args.model))
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="L0 timing model: fit a flash_area cost model from an "
                    "app_perf_l0 capture, then predict hardware wall-clock "
                    "from a native_sim run's I/O counters.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("fit", help="capture -> model.json")
    p.add_argument("capture", help="console capture from app_perf_l0")
    p.add_argument("-o", "--out", help="write the model here")
    p.add_argument("--name", help="model name (default: the board target)")
    p.add_argument("--weight", choices=("rel", "abs"), default="rel",
                   help="least-squares objective: relative error (default) "
                        "or absolute")
    p.add_argument("--size-range", metavar="LO:HI",
                   help="fit only transfer sizes in this range, e.g. 4:4096 "
                        "— use it when the workload lives in one size regime")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="accepted and ignored: the per-point measured-vs-"
                        "fitted table is always printed, because it is the "
                        "evidence for the fit")
    p.set_defaults(func=cmd_fit)

    p = sub.add_parser(
        "derive",
        help="fit a provisional model from stack-level captures "
             "(counters + wall-clock), for a board with no L0 run")
    p.add_argument("captures", nargs="+",
                   help="captures — or a RESULTS.md holding them — that "
                        "print both `io …` and `bench …` lines")
    p.add_argument("-o", "--out")
    p.add_argument("--name")
    p.add_argument("--board", help="board target the captures came from")
    p.add_argument("--block-bytes", type=int, required=True,
                   help="erase block size of the part the captures ran on")
    p.add_argument("--part-bytes", type=int, default=0,
                   help="partition size, for the geometry check")
    p.add_argument("--write-align", type=int, default=1)
    p.add_argument("--loo", action="store_true",
                   help="leave-one-out cross-validation: the honest estimate "
                        "of how this model does on a phase it never saw")
    p.set_defaults(func=cmd_derive)

    p = sub.add_parser("show", help="print a saved model")
    p.add_argument("model")
    p.set_defaults(func=cmd_show)

    p = sub.add_parser("predict", help="I/O counters + model -> milliseconds")
    p.add_argument("-m", "--model", required=True)
    p.add_argument("run", nargs="?", help="a run's console capture "
                                          "(native_sim or hardware)")
    p.add_argument("--reads", type=int, default=0)
    p.add_argument("--bytes-read", type=int, default=0)
    p.add_argument("--writes", type=int, default=0)
    p.add_argument("--bytes-written", type=int, default=0)
    p.add_argument("--erases", type=int, default=0)
    p.add_argument("--bytes-erased", type=int, default=None)
    p.add_argument("--json", help="also write the prediction here")
    p.add_argument("--strict-geometry", action="store_true",
                   help="fail, rather than warn, when the run's geometry is "
                        "not the model's")
    p.add_argument("--allow-simulated", action="store_true",
                   help="permit a model that was fitted from the flash "
                        "simulator")
    p.set_defaults(func=cmd_predict)

    p = sub.add_parser("verify", help="predicted vs measured, per phase")
    p.add_argument("-m", "--model", required=True)
    p.add_argument("run", help="a capture with BOTH counters and wall-clock")
    p.add_argument("--strict-geometry", action="store_true")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser(
        "spec", help="measured model vs a part datasheet: in spec, and in "
                     "which mode?")
    p.add_argument("-m", "--model", required=True)
    p.add_argument("--spec", required=True,
                   help="an l0-part-spec/1 file, e.g. "
                        "models/mx25r6435f_datasheet.json")
    p.set_defaults(func=cmd_spec)

    p = sub.add_parser("simconf", help="model -> CONFIG_FLASH_SIMULATOR_*")
    p.add_argument("-m", "--model", required=True)
    p.add_argument("--run", help="pick the operating point from this run's "
                                 "operation mix")
    p.set_defaults(func=cmd_simconf)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
