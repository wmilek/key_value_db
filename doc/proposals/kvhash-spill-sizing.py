#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0
"""Bucket-load enumeration for the kvhash value-spill proposal.

Companion to `2026-08-16-kvhash-value-spill.md` §4. Answers one question:
with 363-byte person records moved out of the bucket and replaced by an
8-byte reference, how full does the fullest bucket get?

Enumerating rather than modelling is deliberate. `app_cbor_persondb`
DESIGN.md §6.1 records what happened when a compound-Poisson estimate was
trusted instead: it put eight person maps at "5.5 sigma of headroom", and the
implementation hit -ENOSPC at person 9 232.

The person record model, the FNV hash and the key format all come from
`app_cbor_persondb/tools/sizing.py` by import, so this script cannot drift
from the numbers that tool produces. That module prints its own table at
import time; the banner is suppressed below.

Run:  python3 doc/proposals/kvhash-spill-sizing.py
"""

import contextlib
import importlib.util
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIZING = os.path.join(HERE, os.pardir, os.pardir,
                      "app_cbor_persondb", "tools", "sizing.py")


def load_app_model():
    """Import the app's sizing tool, swallowing its import-time table."""
    spec = importlib.util.spec_from_file_location("persondb_sizing", SIZING)
    if spec is None or spec.loader is None:
        sys.exit(f"cannot load {SIZING}")
    mod = importlib.util.module_from_spec(spec)
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(mod)
    return mod


M = load_app_model()

N = 10000          # persons, matching the shipped dataset
BUCKETS = 511      # (CONFIG_BLOB_DB_MAX_PAYLOAD_LEN 4096 - 8) / 8, kvhash.c:47
CAP = 4096         # one blob payload = the bucket ceiling (K2)
KLEN = 9           # "pXXXXXXXX"
HDR = 4            # u16 klen + u16 vlen, kvhash.c:15
SLOT_HDR = 14      # blob_db slot header, per the large-payload proposal §1

INLINE_ENTRY = None            # computed per person (record size varies)
SPILL_ENTRY = HDR + KLEN + 8   # the reference replaces the record


def enumerate_buckets(nmaps, spill):
    """Return (bucket_bytes, value_blob_bytes) for the whole population."""
    load, values = {}, 0
    for i in range(N):
        pid = 100000 + i
        clen, _ = M.person_cbor_len(i, M.VOCAB_NEW, 10, 13)
        if spill:
            entry = SPILL_ENTRY
            values += clen + SLOT_HDR
        else:
            entry = HDR + KLEN + clen
        shard = M.fnv(pid.to_bytes(4, "little")) % nmaps
        b = (shard, M.fnv(f"p{pid:08X}".encode()) % BUCKETS)
        load[b] = load.get(b, 0) + entry
    return load, values


def report(nmaps, spill):
    load, values = enumerate_buckets(nmaps, spill)
    mx = max(load.values())
    over = sum(1 for v in load.values() if v > CAP)
    return {
        "maps": nmaps,
        "occupied": len(load),
        "max_bucket": mx,
        "pct_of_cap": 100 * mx / CAP,
        "over": over,
        "index_bytes": sum(load.values()),
        "value_bytes": values,
    }


def main():
    print(f"{N} persons, {BUCKETS} buckets/map, {CAP} B bucket ceiling\n")
    print(f"{'':<8} {'maps':>5} {'occupied':>9} {'max bkt':>8} "
          f"{'% of cap':>9} {'over':>5}")

    base = report(16, spill=False)
    print(f"{'today':<8} {base['maps']:>5} {base['occupied']:>9} "
          f"{base['max_bucket']:>8} {base['pct_of_cap']:>8.0f}% "
          f"{base['over']:>5}")

    for nmaps in (16, 4, 2, 1):
        r = report(nmaps, spill=True)
        print(f"{'spill':<8} {r['maps']:>5} {r['occupied']:>9} "
              f"{r['max_bucket']:>8} {r['pct_of_cap']:>8.0f}% {r['over']:>5}")

    # Flash accounting, both sides counted the same way: packed bucket bytes
    # plus one blob_db slot header for every blob that exists. Omitting the
    # bucket blobs' own headers while counting the value blobs' would flatter
    # today's layout by ~78 KiB.
    print("\nFlash held by person data (packed bytes + every blob's slot header):")
    b = report(16, spill=False)
    s = report(1, spill=True)
    mib = 1024 * 1024

    today = b["index_bytes"] + b["occupied"] * SLOT_HDR
    spill_total = (s["index_bytes"] + s["occupied"] * SLOT_HDR
                   + s["value_bytes"])
    print(f"  today (16 maps): {b['index_bytes']/mib:.2f} MiB entries"
          f" + {b['occupied']*SLOT_HDR/1024:.0f} KiB headers"
          f" over {b['occupied']} blobs"
          f" = {today/mib:.2f} MiB")
    print(f"  spill  (1 map):  {s['index_bytes']/1024:.0f} KiB index"
          f" + {s['value_bytes']/mib:.2f} MiB values (headers included)"
          f" over {s['occupied']+N} blobs"
          f" = {spill_total/mib:.2f} MiB")

    delta = spill_total - today
    print(f"  delta: {delta/1024:+.0f} KiB ({100*delta/today:+.1f}%)"
          f" — per person, +8 B for the reference and"
          f" +{SLOT_HDR} B for the value blob's own slot header")


if __name__ == "__main__":
    main()
