#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0
#
# Offline capacity check for app_cbor_persondb.
#
# THIS IS NOT BUILT. It exists because a kvhash map cannot be sized safely by
# reasoning about it: a bucket overflows at CONFIG_BLOB_DB_MAX_PAYLOAD_LEN
# while the store is nearly empty (FINDINGS.md K2), there is no per-bucket
# occupancy query (K10), and the bucket count is fixed at create time (K3). An
# analytic compound-Poisson estimate was tried first and was wrong — the fill
# hit -ENOSPC at person 9232. See DESIGN.md §6.1 and RESULTS.md §9.
#
# So the number is obtained by enumeration: this script replays the same
# fnv1a-32, the same "p%08X" key format and the same CBOR sizing rules as the
# firmware, over the whole population, and reports the fullest bucket.
#
#     python3 tools/sizing.py
#
# Rerun it whenever the record shape, the person count or
# CONFIG_BLOB_DB_MAX_PAYLOAD_LEN changes. The app no longer shards (DESIGN.md
# §12.2), so what this picks is the payload, not a map count. Keep the constants below in step with
# src/dataset.c and src/person_cbor.c — nothing enforces that they match, which
# is itself a consequence of the missing introspection.

M32 = 0xFFFFFFFF
def mix(x, salt):
    x = (x + salt * 0x9e3779b9) & M32
    x ^= x >> 16; x = (x * 0x7feb352d) & M32
    x ^= x >> 15; x = (x * 0x846ca68b) & M32
    x ^= x >> 16
    return x
def fnv(bs):
    h = 0x811c9dc5
    for b in bs:
        h ^= b; h = (h * 0x01000193) & M32
    return h

VOCAB_NEW = ["door.hq.lobby","door.hq.east","door.hq.west","door.hq.roof","door.hq.loading",
"lab.chemistry.wet","lab.electronics","lab.cleanroom.a","lab.prototype.3d","gate.perimeter.n",
"gate.perimeter.s","gate.yard.vehicle","room.server.core","room.network.mdf","room.hvac.plant",
"room.power.ups","room.archive.cold","room.vault.secure","room.medical.aid","room.canteen",
"lift.tower.north","lift.freight.dock","park.surface","park.basement.b2","shift.day",
"shift.evening","shift.night","shift.weekend","admin.enroll","admin.revoke","admin.audit.read",
"admin.configure"]
FIRST=["Ada","Bo","Cira","Dov","Eve","Finn","Gita","Hal","Iris","Juno","Kai","Lena","Milo","Nils","Oona","Pax"]
LAST=["Aalto","Bercu","Cheng","Duarte","Eriksen","Farhi","Gruber","Haddad","Ivanov","Jarvis","Kovac","Lindqvist","Moreau","Nagy","Okafor","Pandit"]
DEPT=["Engineering","Operations","Facilities","Security","Research","Logistics","Finance","Support"]
TITLE=["Technician","Engineer","Supervisor","Analyst","Coordinator","Specialist","Manager","Contractor"]

def person_cbor_len(i, vocab, nperm_lo, nperm_span):
    name = f"{FIRST[mix(i,1)%16]} {LAST[mix(i,2)%16]}-{i%10000:04d}"
    dept = DEPT[mix(i,4)%8]; title = TITLE[mix(i,5)%8]
    npd = nperm_lo + (mix(i,8) % nperm_span)
    start = mix(i,9) % 32; step = (mix(i,10)%16)*2+1
    perms = [vocab[(start + k*step) % 32] for k in range(npd)]
    ncards = 1 + (mix(i,3) % 4)
    n = 1 + 6                                  # map hdr + id
    n += 2+len(name); n += 2+len(dept); n += 2+len(title)
    n += 6 + 6 + 18                            # valid_from, valid_until, pin
    n += 2 + sum(1+len(p) for p in perms)      # key + list hdr + entries
    n += 2 + 15*ncards
    return n, ncards

# The benchmark's person count (CONFIG_APP_CBOR_PERSONDB_N_PERSONS). Keep in
# step with Kconfig; DESIGN.md §6.5 records why it is 8 000 and not 10 000.
N_PERSONS = 8000


def run(nmaps, vocab, lo, span, N=N_PERSONS, buckets=511, cap=4096):
    load = {}
    tot = 0
    for i in range(N):
        pid = 100000 + i
        clen, ncards = person_cbor_len(i, vocab, lo, span)
        entry = 4 + 9 + clen
        tot += entry + ncards*(4+14+5)
        shard = fnv(pid.to_bytes(4,'little')) % nmaps
        key = f"p{pid:08X}".encode()
        b = (shard, fnv(key) % buckets)
        load[b] = load.get(b, 0) + entry
    mx = max(load.values()); over = sum(1 for v in load.values() if v > cap)
    return tot, mx, over, sum(load.values())/len(load)

print(f"{'maps':>4} {'total MiB':>10} {'fill%':>6} {'mean bkt':>9} {'max bkt':>8} {'over':>5}")
for nmaps in (8, 12, 16, 20, 24, 32):
    tot, mx, over, mean = run(nmaps, VOCAB_NEW, 10, 13)
    print(f"{nmaps:>4} {tot/1048576:>10.2f} {100*tot/8388608:>6.1f} {mean:>9.0f} {mx:>8} {over:>5}")

# credential map: one shard, 23 B entries
def cred_run(N=N_PERSONS, buckets=511, cap=4096):
    load = {}
    for i in range(N):
        ncards = 1 + (mix(i,3) % 4)
        for slot in range(ncards):
            n = i*(5+3) + slot
            uid = (n * 0x9e3779b97f4a7d) & 0x00ffffffffffff
            key = f"{uid:014X}".encode()
            b = fnv(key) % buckets
            load[b] = load.get(b, 0) + 4 + 14 + 5
    return max(load.values()), sum(1 for v in load.values() if v > cap), len(load)

mx, over, used = cred_run()
print(f"\ncredential map (1 shard): max bucket {mx} B, buckets used {used}/511, over {over}")

# how much of the win is the tail vs the mean?
for nmaps in (12, 16):
    tot, mx, over, mean = run(nmaps, VOCAB_NEW, 10, 13)
    print(f"{nmaps} maps: max {mx} B = {100*mx/4096:.0f}% of a bucket; "
          f"non-empty buckets ~{int(nmaps*511*(1-2.718**(-10000/(nmaps*511))))}")

# ---------------------------------------------------------------------------
# The L3 re-check (DESIGN.md §12).
#
# CONFIG_BLOB_DB_MAX_PAYLOAD_LEN was capped at 4096 when D10 was taken; the
# range is now 1..65535, bound at mount by the geometry to
# (sector - 16) / 2 - 14 = 32 746 B on the DK's 64 KB sectors. That lifts C2's
# 2.09 MB per map, so "does the dataset fit ONE map?" has to be re-asked — a
# one-map dataset is one kvdb instance, and the whole D10 argument was that
# seventeen of them do not fit.
#
# Two costs move in opposite directions and both land on R-D, so the sweep
# reports both:
#   directory  - read by dir_load on EVERY get/set/del (K11), 8 + 8*n_buckets
#   bucket     - the second read, and the whole rewrite on a set (K4)
# Fewer maps means fewer buckets to spread the same bytes over, so one of the
# two must grow. Column "get B" is their sum: the bytes one map get moves.

def l3_recheck():
    print(f"\n{'payload':>7} {'maps':>4} {'buckets':>7} {'dir B':>6} {'mean bkt':>8} "
          f"{'max bkt':>7} {'%ceil':>5} {'get B':>6} {'vs now':>6} {'dir traffic':>11}")
    base = None
    for pay, nmaps, nb in ((4096, 16, 511),      # as built
                           (4096,  8, 511),
                           (16384, 1, 511), (16384, 1, 1023), (16384, 1, 2047),
                           (32746, 1, 511), (32746, 1, 2047), (32746, 1, 4092),
                           (32746, 2, 4092)):
        if nb > (pay - 8) // 8:
            continue
        # N=10000: this table reproduces the D10 argument at the scale it
        # was argued at, before §6.5 re-sized the benchmark to 8 000.
        tot, mx, over, mean = run(nmaps, VOCAB_NEW, 10, 13, N=10000, buckets=nb, cap=pay)
        dirb = 8 + 8 * nb
        getb = dirb + mean
        base = base or getb
        # Every fresh bucket rewrites the whole directory (K5), once per bucket.
        traffic = nmaps * nb * dirb
        flag = '' if over == 0 else f'  OVER x{over}'
        print(f"{pay:>7} {nmaps:>4} {nb:>7} {dirb:>6} {mean:>8.0f} {mx:>7} "
              f"{100*mx/pay:>4.0f}% {getb:>6.0f} {getb/base:>5.1f}x "
              f"{traffic/1048576:>9.1f} MiB{flag}")

l3_recheck()


# ---------------------------------------------------------------------------
# The two-instance layout (DESIGN.md §12).
#
# The app is one people map + one credential map. Sharding is gone, so the
# bucket count is no longer "as many as fit" — kvhash's initial_capacity IS the
# bucket count, and the two costs it drives move in opposite directions:
#
#   directory   8 + 8*n_buckets, read by dir_load on EVERY get/set/del (K11)
#   bucket      total_bytes / n_buckets, the second read
#
# so bytes-per-get has a minimum in n. The fullest bucket must also stay well
# under CONFIG_BLOB_DB_MAX_PAYLOAD_LEN, because it overflows with no warning
# (K2) and the count cannot change afterwards (K3). Report both.

# The app asks kvhash for the largest map it can build, so the bucket count is
# not chosen directly -- MAX_PAYLOAD chooses it, via (MAX_PAYLOAD - 8) / 8.
# This is the sweep DESIGN.md §6.1 reports, and the reason 16384 ships.

def two_instance():
    print(f"\n{'payload':>7} {'buckets':>7} {'dir B':>6} {'mean bkt':>8} "
          f"{'max bkt':>7} {'%ceil':>6} {'get B':>6} {'vs 16-shard':>11}")
    for pay in (4096, 8192, 16384, 32722):
        nb = (pay - 8) // 8
        tot, mx, over, mean = run(1, VOCAB_NEW, 10, 13, buckets=nb, cap=1 << 30)
        dirb = 8 + 8 * nb
        getb = dirb + mean
        note = ''
        if mx > pay:
            note = '  BURSTS'
        # K12: two copies of the directory plus slot headers must leave room in
        # an erase block, or blob_db compacts on every write and the fill dies.
        if 16 + 2 * (dirb + 14) > 65488 - 1024:
            note += '  DIRECTORY OWNS AN ERASE BLOCK (K12)'
        print(f"{pay:>7} {nb:>7} {dirb:>6} {mean:>8.0f} {mx:>7} "
              f"{100*mx/pay:>5.0f}% {getb:>6.0f} {getb/4756:>10.1f}x{note}")

    print("\nShipped: 16384 -- fullest bucket 28 % of the ceiling, and the only")
    print("row that neither bursts a bucket nor hands an erase block to a")
    print("directory. Costs 3.8x the bytes per lookup of the 16-shard build;")
    print("that is FINDINGS.md K11, measured in RESULTS.md section 4, not tuned away.")

    mx, over, used = cred_run(buckets=2047, cap=1 << 30)
    print(f"\ncredential map (2047 buckets): max bucket {mx} B, used {used}/2047")

two_instance()


# ===========================================================================
# The proposed two-level kvhash (doc/proposals/2026-08-20-kvhash-second-level.md)
#
# D0 made this script the distribution check for that design, because a
# correlated derivation is the one defect the application's own VERIFY PASS
# cannot see: every key is still found, and the damage shows up only as
# clustering. So the model below is not a sizing aid — it is the check.
#
# It replicates the decided design exactly:
#   D3d  crc32_ieee, top = (crc >> 16) % m, sub = (crc & 0xffff) % n.
#        zlib.crc32 IS crc32_ieee — same polynomial, same reflection, same
#        init and final xor — so this cannot drift from the firmware. That
#        was the deciding argument for CRC over FNV(FNV(k)), which would be
#        hand-rolled here and in C and kept matching by hand.
#   D3e  {0} builds one level, unchanged from today.
#   D3f  SMALL_MAP_LOAD = 4; two levels only past ONE_LEVEL_MAX_BUCKETS.

import zlib

SMALL_MAP_LOAD        = 4      # D3f
ONE_LEVEL_MAX_BUCKETS = 255    # D3e — 2 048 B directory, one blob at create
SAFE_BUCKET_FRACTION  = 0.6    # D3d/§9.7 — the warning threshold, used here
                               # as the sizing target so a fresh map starts
                               # below the line it would warn at.
MIN_BUCKET_BYTES      = 256    # Floor on the two-level target. §5.2 said the
                               # limit on "more buckets" is per-blob overhead
                               # and left it unquantified; this model quantified
                               # it. At ~1 entry per bucket the credential map
                               # wanted 20 164 buckets for 23 B entries — a blob
                               # per entry, 14 B of slot header on 23 B of data,
                               # 62 % overhead, and 2 311 B per get. At a 256 B
                               # floor: 1 849 buckets, 6 % overhead, 952 B per
                               # get. The floor never binds for entries larger
                               # than itself, so the people map is unaffected.


def derive_geometry(expected_entries, typical_entry_bytes, max_entry_bytes,
                    payload):
    """The container's rule (§7.1). Returns (depth, m, n, buckets).

    Small maps pack buckets and stay one level: their write traffic is
    negligible, and eager sub-map creation costs a fresh blob_db bucket each
    (~1.1 s on the DK, B7). Large maps target ~1 entry per bucket, because a
    set rewrites the whole bucket (K4) so fill traffic goes as 1/buckets.
    """
    safe_bucket = int(payload * SAFE_BUCKET_FRACTION)
    load = max(1, min(SMALL_MAP_LOAD, safe_bucket // max(1, max_entry_bytes)))
    small = -(-expected_entries // load)          # ceil

    if small <= ONE_LEVEL_MAX_BUCKETS:
        return 1, 0, small, small

    # ~1 entry per bucket (§5.2) but never buckets smaller than the floor:
    # below it, blob_db's per-slot header dominates the entry it carries.
    target = max(typical_entry_bytes, MIN_BUCKET_BYTES)
    buckets = max(1, -(-(expected_entries * typical_entry_bytes) // target))
    m = int(buckets ** 0.5)
    while m * m < buckets:
        m += 1
    return 2, m, m, m * m


def idx_crc(key, m, n):
    """D3d. The top and sub indices."""
    h = zlib.crc32(key)
    return ((h >> 16) % m, (h & 0xffff) % n) if m else (0, h % n)


def idx_naive(key, m, n):
    """The forbidden derivation, kept so the check demonstrably catches it."""
    h = zlib.crc32(key)
    return ((h % m, h % n) if m else (0, h % n))


def person_keys(N=N_PERSONS):
    for i in range(N):
        clen, _ = person_cbor_len(i, VOCAB_NEW, 10, 13)
        yield f"p{100000 + i:08X}".encode(), 4 + 9 + clen


def credential_keys(N=N_PERSONS):
    for i in range(N):
        for slot in range(1 + (mix(i, 3) % 4)):
            n = i * (5 + 3) + slot
            uid = (n * 0x9e3779b97f4a7d) & 0x00ffffffffffff
            yield f"{uid:014X}".encode(), 4 + 14 + 5


def spread(keys, m, n, idx):
    load = {}
    for key, sz in keys:
        b = idx(key, m, n)
        load[b] = load.get(b, 0) + sz
    v = list(load.values())
    return max(v), sum(v) / len(v), len(v), sum(v)


def two_level_report(payload=16384):
    print(f"\n=== proposed two-level kvhash, payload {payload} B ===")
    warn = int(payload * SAFE_BUCKET_FRACTION)

    for name, keys, entries, typ, mx_entry in (
            ("people",      person_keys,     N_PERSONS, 433, 700),
            ("credentials", credential_keys, sum(1 + (mix(i, 3) % 4)
                                                 for i in range(N_PERSONS)), 23, 23)):
        depth, m, n, buckets = derive_geometry(entries, typ, mx_entry, payload)
        meta = (8 + 8 * n) if depth == 1 else (8 + 8 * m) + (8 + 8 * n)
        mxb, mean, used, total = spread(keys(), m if depth == 2 else 0, n, idx_crc)

        print(f"\n{name}: {entries} entries declared, {typ} B typical")
        print(f"  derived      : depth {depth}, "
              f"{'%d x %d' % (m, n) if depth == 2 else '%d' % n} "
              f"= {buckets} buckets  (D3e/D3f)")
        print(f"  metadata/op  : {meta} B      bucket mean {mean:.0f} B")
        print(f"  bytes/get    : {meta + mean:.0f} B")
        print(f"  fullest      : {mxb} B = {100*mxb/payload:.0f}% of payload"
              f"   {'OVER 60% WARN LINE' if mxb > warn else 'below the warn line'}")
        print(f"  buckets used : {used}/{buckets}")

        if depth == 2:
            nm, _, nused, _ = spread(keys(), m, n, idx_naive)
            print(f"  guard (naive h%m,h%n): fullest {nm} B, {nused}/{buckets} used"
                  f"  <- must be far worse, or the check is not checking")


two_level_report()
