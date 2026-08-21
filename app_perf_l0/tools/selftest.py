#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0
#
# Self-test for l0_timing.py, against a substrate whose cost is known exactly
# because this file defines it.
#
#     python3 app_perf_l0/tools/selftest.py
#
# THIS GENERATES SYNTHETIC DATA. Nothing it prints is a measurement of any
# part, and no model it produces should ever be saved into models/. What it
# tests is the ANALYSIS: given a capture from a device with a known cost
# curve, does the tool recover that curve, and does it say the right thing
# about linearity?
#
# The native_sim run in RESULTS.md §1 is the other half of this. It is a real
# run, but the simulator's cost is flat, so it can only prove that the fitter
# does not invent structure. This proves the opposite direction: that the
# fitter finds structure that is there, and — the case that matters for write
# — reports a staircase as a staircase instead of quietly averaging it into a
# slope.
#
# The costs below are shaped like an MX25R6435F on 8 MHz Quad-SPI, close to
# the provisional model in models/. They are not that part's measured costs
# and are not used as such anywhere.

import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

PAGE = 256
BLOCK = 65536
ALIGN = 4
MAX_XFER = 65536

# The cost model the synthetic device obeys.
READ_FIXED_NS = 67_700
READ_PER_B_NS = 631.0
WRITE_FIXED_NS = 90_000
WRITE_PER_PAGE_NS = 7_560_000        # one page program
ERASE_PER_BLOCK_NS = 1_106_000_000
ERASE_FIXED_NS = 300_000


def read_ns(n):
    return READ_FIXED_NS + READ_PER_B_NS * n


def pages_touched(off, n):
    return (off + n - 1) // PAGE - off // PAGE + 1


def write_ns_pagewise(n, stride):
    """A part that programs whole pages: cost is the pages a transfer touches.

    Simulated over the app's ACTUAL access pattern — consecutive transfers at
    `stride`, exactly as phase_write() issues them — rather than approximated.
    That matters: when the transfer size divides the page, every transfer is
    page-aligned and the curve is flat at one program all the way from 4 B to
    256 B. When it does not, transfers straddle and the average is fractional.
    An approximation would have smoothed away precisely the structure this
    test exists to detect.
    """
    reps = 64
    total = sum(pages_touched(i * stride, n) for i in range(reps))
    return WRITE_FIXED_NS + WRITE_PER_PAGE_NS * (total / reps)


def write_ns_affine(n, stride):
    """A part whose program time scales with the bytes actually written.

    Not a strawman: the provisional DK model in models/ fits 36 hardware
    observations at 1.4 % median error with exactly this shape, which is
    evidence that the MX25R64 on that board behaves this way rather than
    charging a full page for a 48 B write. Which of the two shapes is real is
    the question the hardware run settles; the tool has to report either one
    correctly.
    """
    del stride
    return WRITE_FIXED_NS + (WRITE_PER_PAGE_NS / PAGE) * n


def erase_ns(blocks):
    return ERASE_FIXED_NS + ERASE_PER_BLOCK_NS * blocks


def sizes(base):
    """The app's sweep: powers of two, each followed by its 1.5x midpoint."""
    out, s = [], base
    while s <= MAX_XFER:
        out.append(s)
        mid = s + s // 2
        if mid <= MAX_XFER and s // 2 >= base and (s // 2) % base == 0:
            out.append(mid)
        s *= 2
    return out


def emit(lines, op, rows):
    """One sweep: l0raw per row, then the l0lin marginal summary."""
    prev = None
    lo = hi = None
    for size, ns in rows:
        ops = max(1, int(50e6 // ns))
        lines.append(f"l0raw op={op} size={size} ops={ops} "
                     f"total_ns={int(ns * ops)} ns_per_op={int(ns)}")
        if prev:
            cmarg = int((ns - prev[1]) * 100 / (size - prev[0]))
            lo = cmarg if lo is None else min(lo, cmarg)
            hi = cmarg if hi is None else max(hi, cmarg)
        prev = (size, ns)
    if lo is not None and op in ("read", "write"):
        lines.append(f"l0lin op={op} cmarg_min={lo} cmarg_max={hi}")


def synth_capture(write_model):
    L = [
        "*** Booting Zephyr OS build synthetic ***",
        "l0 perf 1.0.0 — raw flash_area timing",
        f"l0geom part_bytes=8388608 block_bytes={BLOCK} blocks=128 "
        f"write_align={ALIGN} erased_val=0xff region_blocks=32 "
        f"max_xfer={MAX_XFER} program_page={PAGE} cycles_per_s=32768 "
        f"source=hardware timing=real board=synthetic/selftest",
    ]
    for span in (1, 2, 4, 8, 16, 32):
        ns = erase_ns(span)
        L.append(f"l0raw op=erase blocks={span} ops=3 total_ns={int(ns * 3)} "
                 f"ns_per_op={int(ns)} ns_per_block={int(ns / span)} "
                 f"min_ns={int(ns)} max_ns={int(ns)}")
    ns = erase_ns(1)
    L.append(f"l0raw op=erase1 blocks=1 ops=32 total_ns={int(ns * 32)} "
             f"ns_per_op={int(ns)} ns_per_block={int(ns)} "
             f"min_ns={int(ns)} max_ns={int(ns)}")

    emit(L, "read", [(n, read_ns(n)) for n in sizes(1)])
    # Packed: stride == size, which is what phase_write() does.
    emit(L, "write", [(n, write_model(n, n)) for n in sizes(ALIGN)])
    # Page-aligned: stride rounded up to a page, as phase_write_pages() does.
    emit(L, "write_pg",
         [(n, write_model(n, -(-n // PAGE) * PAGE)) for n in
          (PAGE // 4, PAGE // 2, PAGE - ALIGN, PAGE, PAGE + ALIGN,
           PAGE + PAGE // 2, 2 * PAGE, 2 * PAGE + ALIGN, 3 * PAGE, 4 * PAGE)])
    L.append("l0end status=0")
    return "\n".join(L) + "\n"


def fit(cap, out=None):
    cmd = [sys.executable, str(HERE / "l0_timing.py"), "fit", str(cap),
           "--name", "synthetic"]
    if out:
        cmd += ["-o", str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        raise SystemExit("fit failed on the synthetic capture")
    return r.stdout


def case_affine(failures, verbose):
    """A part whose write cost scales with the bytes written."""
    cap = HERE / "_selftest_affine.log"
    mj = HERE / "_selftest_affine.json"
    cap.write_text(synth_capture(write_ns_affine))
    txt = fit(cap, mj)
    m = json.loads(mj.read_text())["model"]
    if verbose:
        print(txt)

    def want(cond, what):
        if not cond:
            failures.append(f"[affine device] {what}")

    # Every term is recoverable here, because every curve really is a line.
    want(rel(m["erase"]["per_block_ns"], ERASE_PER_BLOCK_NS) < 0.02,
         f"erase per block {m['erase']['per_block_ns'] / 1e6:.1f} ms, "
         f"want {ERASE_PER_BLOCK_NS / 1e6:.1f}")
    want(rel(m["read"]["per_byte_ns"], READ_PER_B_NS) < 0.02,
         f"read per byte {m['read']['per_byte_ns']:.1f}, want {READ_PER_B_NS}")
    want(rel(m["read"]["fixed_ns"], READ_FIXED_NS) < 0.05,
         f"read fixed {m['read']['fixed_ns'] / 1000:.1f} us, "
         f"want {READ_FIXED_NS / 1000:.1f}")
    want(rel(m["write"]["per_byte_ns"], WRITE_PER_PAGE_NS / PAGE) < 0.02,
         f"write per byte {m['write']['per_byte_ns'] / 1000:.2f} us, "
         f"want {WRITE_PER_PAGE_NS / PAGE / 1000:.2f}")

    # ...and the tool must SAY it is a line, in both classes.
    want(txt.count("-> affine") >= 2,
         "read and write were not both reported as affine")
    want("write cost tracks ceil(size / page)" not in txt,
         "a staircase was claimed on a device that has none")
    want(m["write"]["max_rel_error"] < 0.05,
         f"write fit residual {m['write']['max_rel_error'] * 100:.1f} % on "
         "data that is exactly a line")
    cap.unlink(missing_ok=True)
    mj.unlink(missing_ok=True)


def case_pagewise(failures, verbose):
    """A part that programs whole pages, whatever the transfer size."""
    cap = HERE / "_selftest_pagewise.log"
    mj = HERE / "_selftest_pagewise.json"
    cap.write_text(synth_capture(write_ns_pagewise))
    txt = fit(cap, mj)
    m = json.loads(mj.read_text())["model"]
    if verbose:
        print(txt)

    def want(cond, what):
        if not cond:
            failures.append(f"[page-wise device] {what}")

    # Read and erase are untouched by the write model, so they must still be
    # exact: a tool that lost accuracy on them here would be leaking the
    # write curve into the other fits.
    want(rel(m["read"]["per_byte_ns"], READ_PER_B_NS) < 0.02,
         f"read per byte {m['read']['per_byte_ns']:.1f}, want {READ_PER_B_NS}")
    want(rel(m["erase"]["per_block_ns"], ERASE_PER_BLOCK_NS) < 0.02,
         f"erase per block {m['erase']['per_block_ns'] / 1e6:.1f} ms")

    # The findings that matter: write is not a line, and the reason is
    # visible as a staircase.
    want("NOT affine" in txt, "write was not reported as non-affine")
    want("write cost tracks ceil(size / page)" in txt,
         "the page-program staircase was not detected")
    want(m["write"]["max_rel_error"] > 0.10,
         f"write fit claims {m['write']['max_rel_error'] * 100:.1f} % worst "
         "error over a staircase; it should not look that good")
    cap.unlink(missing_ok=True)
    mj.unlink(missing_ok=True)


def rel(got, want_):
    return abs(got - want_) / want_ if want_ else (0.0 if not got else 1.0)


def main():
    verbose = "-v" in sys.argv
    print("Two synthetic devices, same read and erase costs, different write "
          "shape:\n"
          f"  read  {READ_FIXED_NS / 1000:.3f} us + {READ_PER_B_NS} ns/B\n"
          f"  erase {ERASE_FIXED_NS / 1000:.3f} us + "
          f"{ERASE_PER_BLOCK_NS / 1e6:.3f} ms/block\n"
          f"  write (a) affine:   {WRITE_FIXED_NS / 1000:.0f} us + "
          f"{WRITE_PER_PAGE_NS / PAGE / 1000:.2f} us/B\n"
          f"  write (b) by page:  {WRITE_FIXED_NS / 1000:.0f} us + "
          f"{WRITE_PER_PAGE_NS / 1e6:.2f} ms per {PAGE} B page touched\n"
          "The tool must recover each, and must report the shape it actually "
          "sees.\n")

    failures = []
    case_affine(failures, verbose)
    case_pagewise(failures, verbose)

    if failures:
        print("FAILED:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("all self-tests passed "
          "(coefficients recovered, and both verdicts correct)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
