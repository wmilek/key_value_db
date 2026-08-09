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
# Rerun it whenever the record shape, the person count, the shard count or
# CONFIG_BLOB_DB_MAX_PAYLOAD_LEN changes. Keep the constants below in step with
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

def run(nmaps, vocab, lo, span, N=10000, buckets=511, cap=4096):
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
def cred_run(N=10000, buckets=511, cap=4096):
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
