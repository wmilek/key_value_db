# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

"""Objects in every structure format `diag` knows, current and past.

Built byte by byte from the layouts the libraries wrote (see the comments in
app_diag/src/diag_decode.c), so a store can hold formats no current firmware
writes: a one-level kvhash, the inline logring ring, the 16-byte blobfs meta,
the persondb superblock v1. Imported with `blob_db import`, they give `diag`
something to recognise. Every object fits a 256-byte payload.

Run as a script, it prints the shell commands that format a store and import
the objects, for piping into the native_sim build by hand:

    python3 app_diag/pytest/diag_fixtures.py | \\
        build/diag/zephyr/zephyr.exe --kvdb_in_ram -no-rt -stop_at=5
"""

import struct
import sys
from pathlib import Path

NEXT_ID = 100


def fourcc(s: str) -> int:
    return int.from_bytes(s.encode(), 'big')


def fnv1a32(s: str) -> int:
    h = 0x811c9dc5
    for b in s.encode():
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def rreg(entries: list[tuple[int, int]]) -> bytes:
    out = b'RREG' + struct.pack('<HH', 1, len(entries))
    for key, root in entries:
        out += struct.pack('<QQ', key, root)
    return out


def kvdb_meta(name: str, backend: int, struct_root: int) -> bytes:
    n = name.encode()
    return struct.pack('<IBBBBQ', fourcc('KVDB'), backend, 1, len(n), 0,
                       struct_root) + n.ljust(16, b'\0')


def kvhash_dir(version: int, depth: int, children: list[int],
               magic: str = 'KVHA') -> bytes:
    if version == 1:   # one level: [u16 n] [u16 version]
        head = struct.pack('<IHH', fourcc(magic), len(children), 1)
    else:
        head = struct.pack('<IHBB', fourcc(magic), len(children), version,
                           depth)
    return head + b''.join(struct.pack('<Q', c) for c in children)


def kv_entry(key, value) -> bytes:
    """key, value: bytes inline, or ('spill', len, id[, fp]) spilled."""
    if isinstance(key, tuple):
        _, klen, kid, fp = key
        kl, kpart = klen | 0x8000, struct.pack('<IQ', fp, kid)
    else:
        kl, kpart = len(key), key
    if isinstance(value, tuple):
        _, vlen, vid = value
        vl, vpart = vlen | 0x8000, struct.pack('<Q', vid)
    else:
        vl, vpart = len(value), value
    return struct.pack('<HH', kl, vl) + kpart + vpart


def dirent(kind: int, target: int) -> bytes:
    return struct.pack('<BBHIQ', kind, 1, 0, 0, target)


def blobfs_v1_bare(dir_root: int) -> bytes:
    return struct.pack('<IB3xQ', fourcc('BFS1'), 1, dir_root)


def _pad(b: bytes, align: int) -> bytes:
    """The blobfs meta is not packed: padded to the writer's u64 alignment,
    8 on ARM, 4 on native_sim (x86-32)."""
    return b + b'\0' * (-len(b) % align)


def blobfs_v1_intent(dir_root: int, body: int = 0, frm: str = '',
                     to: str = '', name_max: int = 12,
                     align: int = 8) -> bytes:
    f, t = frm.encode(), to.encode()
    return _pad(struct.pack('<IBBBBQQ', fourcc('BFS1'), 1, int(body != 0),
                            len(f), len(t), dir_root, body) +
                f.ljust(name_max + 1, b'\0') + t.ljust(name_max + 1, b'\0'),
                align)


def blobfs_v2(dir_root: int, body: int = 0, frm: str = '', to: str = '',
              from_dir: int = 0, to_dir: int = 0, name_max: int = 12,
              align: int = 8) -> bytes:
    f, t = frm.encode(), to.encode()
    return _pad(struct.pack('<IBBBBQQQQ', fourcc('BFS1'), 2, int(body != 0),
                            len(f), len(t), dir_root, body, from_dir,
                            to_dir) +
                f.ljust(name_max + 1, b'\0') + t.ljust(name_max + 1, b'\0'),
                align)


def clog_chain_root(epoch: int, head: int, waypoint: int, count: int,
                    live: int, capacity: int = 1000, evict=(0, 0)) -> bytes:
    return b'CLOG' + struct.pack('<BBHIIIIQQQQ', 1, 0, 0, epoch, capacity,
                                 live, count, waypoint, head, *evict)


def clog_entry(next_id: int, record: bytes) -> bytes:
    return struct.pack('<QH', next_id, len(record)) + record


def clog_ring(records: list[tuple[int, bytes]], next_seq: int) -> bytes:
    data = b''.join(struct.pack('<QH', s, len(r)) + r for s, r in records)
    return b'CLOG' + struct.pack('<BBHHHQ', 1, 0, len(records), len(data), 0,
                                 next_seq) + data


def cbor(v) -> bytes:
    """Canonical CBOR for ints >= 0, str, bytes, list, dict."""
    def head(major: int, n: int) -> bytes:
        if n < 24:
            return bytes([major << 5 | n])
        for ai, w in ((24, 1), (25, 2), (26, 4), (27, 8)):
            if n < 1 << (8 * w):
                return bytes([major << 5 | ai]) + n.to_bytes(w, 'big')
        raise ValueError(n)
    if isinstance(v, int):
        return head(0, v)
    if isinstance(v, bytes):
        return head(2, len(v)) + v
    if isinstance(v, str):
        return head(3, len(v.encode())) + v.encode()
    if isinstance(v, list):
        return head(4, len(v)) + b''.join(map(cbor, v))
    if isinstance(v, dict):
        return head(5, len(v)) + b''.join(cbor(k) + cbor(x)
                                          for k, x in v.items())
    raise TypeError(v)


def objects() -> dict[int, bytes]:
    person = cbor({1: 1, 2: 'Ada', 3: 'R&D', 4: 'Eng', 5: 0, 6: 0, 7: 1234,
                   8: 3, 9: [b'\x01\x02\x03\x04']})
    return {
        # The registry: four roots this tool knows by key, one it does not,
        # and one that names no object.
        1: rreg([(fourcc('KVDB') << 32 | fnv1a32('cfg'), 10),
                 (fourcc('BFS1') << 32, 20),
                 (fourcc('PADB') << 32, 30),
                 (fourcc('LOGR') << 32 | 1, 40),
                 (fourcc('XXXX') << 32 | 7, 99)]),

        # kvdb "cfg": a v3 kvhash, two buckets, one entry spilled both ways.
        10: kvdb_meta('cfg', 1, 11),
        11: kvhash_dir(3, 1, [12, 0, 0, 13]),
        12: kv_entry(b'alpha', b'1') +
            kv_entry(('spill', 300, 14, 0xa5a5a5a5), ('spill', 400, 15)),
        13: kv_entry(b'beta', b'\x00\x01\x02'),
        14: b'\xee' * 40,            # stands for a spilled key
        15: b'\xdd' * 40,            # and a spilled value

        # blobfs v2, a rename caught in flight.
        20: blobfs_v2(21, body=23, frm='a.txt', to='b.txt', from_dir=21,
                      to_dir=21),
        21: kvhash_dir(3, 1, [22, 0]),
        22: kv_entry(b'a.txt', dirent(1, 23)) + kv_entry(b'sub',
                                                         dirent(2, 24)),
        23: b'hello world',
        24: kvhash_dir(2, 1, [0]),

        # persondb v2: a two-level people map and a credentials map.
        30: cbor({1: 2, 2: {1: {1: 31, 2: 0, 3: 1}, 2: {1: 32, 2: 0, 3: 1}},
                  3: 5, 4: 1, 5: 9}),
        31: kvhash_dir(3, 2, [33, 34]),
        32: kvhash_dir(3, 1, [0]),
        33: kvhash_dir(3, 1, [35, 0]),
        34: kvhash_dir(3, 1, [0, 0]),
        35: kv_entry(b'p00000001', person),

        # A logring chain: root, two entries, 43 reserved for the next.
        40: clog_chain_root(0xdeadbeef, head=41, waypoint=42, count=2,
                            live=11),
        41: clog_entry(42, b'first'),
        42: clog_entry(43, b'second'),

        # Past formats.
        50: clog_ring([(1, b'abc'), (2, b'defg')], next_seq=3),
        51: clog_ring([(7, b'x' * 26)], next_seq=8),     # 56 bytes, too
        52: kvhash_dir(1, 1, [53, 0]),
        53: kv_entry(b'k', b'v'),
        54: blobfs_v1_bare(55),
        56: blobfs_v1_intent(55),
        58: blobfs_v1_intent(55, body=23, frm='old', to='new', align=4),
        59: blobfs_v2(21, body=23, frm='x', to='yy', from_dir=21,
                      to_dir=24, name_max=20, align=4),
        57: cbor({1: 1, 2: [70, 71], 3: 72, 4: 3, 5: 1, 6: 2}),

        # States and oddities.
        61: kvhash_dir(3, 1, [0], magic='KVHD'),
        62: kvhash_dir(3, 1, [1, 2])[:-8],               # length lies
        64: b'',
        65: cbor([1, 2, 'x']),
        66: bytes(range(7, 47)),
    }


if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
    import blob_db_export as bx
    print('blob_db format yes')
    for line in bx.pack_lines({'next_id': NEXT_ID}, objects()):
        print(f'blob_db import {line}')
    for cmd in ('diag scan', 'diag scan -a', 'diag roots', 'diag formats',
                *sys.argv[1:]):
        print(cmd)
