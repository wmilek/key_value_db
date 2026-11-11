# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

"""`diag scan`, `diag roots` and `diag show` on the native_sim build.

The store is filled with `blob_db import` from diag_fixtures.py: one object
in each structure format diag knows, the current ones and those earlier
firmware wrote (one-level kvhash, the inline logring ring, the 16-byte blobfs
meta, the persondb superblock v1), plus a few states and oddities. Each is
checked to come back with the right type and variant, and its decoding to
show the fields that matter.
"""

import re
import sys
from pathlib import Path

import pytest
from twister_harness import DeviceAdapter, Shell

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'scripts'))
sys.path.insert(0, str(HERE))
import blob_db_export as bx  # noqa: E402
import diag_fixtures as fx   # noqa: E402


def _has(lines: list[str], text: str) -> bool:
    return any(text in line for line in lines)


def _row(lines: list[str], oid: int) -> str:
    """The scan or summary line of object @p oid."""
    rows = [ln for ln in lines if re.match(rf'\s*{oid}\s{{2,}}\S', ln)]
    assert rows, (oid, lines)
    return rows[0]


@pytest.fixture(scope='module')
def store(dut: DeviceAdapter, shell: Shell):
    lines = shell.exec_command('blob_db format yes')
    assert _has(lines, 'formatted and mounted'), lines
    for line in bx.pack_lines({'next_id': fx.NEXT_ID}, fx.objects()):
        dut.write(f'blob_db import {line}\n'.encode())
        replies = [r for r in bx.scan(dut.readlines_until(
            regex=r'%BDB1 [AN] ', timeout=5.0))[0] if r.type in ('A', 'N')]
        assert len(replies) == 1 and replies[0].type == 'A', (line, replies)
    return shell


# id -> (type, variant prefix, a field of its summary)
EXPECTED = {
    1:  ('rootreg', 'v1', 'entries=5'),
    10: ('kvdb', 'v1', 'name="cfg" backend=hash map=11'),
    11: ('kvhash', 'v3', 'n=4 depth=1 bound=2'),
    20: ('blobfs', 'v2', 'dir_root=21 rename-in-flight'),
    24: ('kvhash', 'v2', 'n=1 depth=1 bound=0'),
    30: ('persondb', 'v2', 'tables=2 persons=5 rev=9'),
    31: ('kvhash', 'v3', 'n=2 depth=2 bound=2'),
    40: ('logring', 'chain v1', 'epoch=deadbeef count=2 head=41'),
    50: ('logring', 'ring v1', 'records=2 used=27 next_seq=3'),
    51: ('logring', 'ring v1', 'records=1 used=36'),
    52: ('kvhash', 'v1', 'n=2 depth=1 bound=1'),
    54: ('blobfs', 'v1 bare', 'dir_root=55'),
    56: ('blobfs', 'v1 intent', 'dir_root=55'),
    58: ('blobfs', 'v1 intent', 'rename-in-flight'),
    59: ('blobfs', 'v2', 'rename-in-flight'),
    57: ('persondb', 'v1', 'people_maps=2 cred=72'),
}

# Recognised by layout alone: listed by `scan -a` only.
SHAPES = {
    12: ('kvbucket', 'entries=2 spilled=2'),
    22: ('kvbucket', 'entries=2'),
    35: ('kvbucket', 'entries=1'),
    41: ('logentry', 'next=42 len=5'),
    65: ('cbor', '[1, 2, "x"]'),
}


def test_scan(store: Shell):
    lines = store.exec_command('diag scan')
    for oid, (typ, var, field) in EXPECTED.items():
        row = _row(lines, oid)
        assert f' {typ} ' in row and f' {var}' in row and field in row, row
        assert 'suspect' not in row, row
    # Shape-only guesses are counted, not listed, unless a root points at
    # them (41 is the logring's head).
    for oid in (12, 22, 35, 65):
        assert not any(re.match(rf'\s*{oid}\s{{2,}}', ln) for ln in lines), oid
    assert '<- 40.head' in _row(lines, 41)

    # Where each root and the first level below it hang from.
    assert "<- rootreg 'KVDB'/0x" in _row(lines, 10)
    assert '<- 10.struct_root' in _row(lines, 11)
    assert '<- 20.rename_body' in _row(lines, 23)
    assert '<- 40.waypoint' in _row(lines, 42)

    # A dying directory, and one whose length lies.
    assert _has(lines, 'destroy committed, children being released')
    assert 'suspect: length disagrees with child count' in _row(lines, 62)

    assert _has(lines, f'{len(fx.objects())} objects')
    assert _has(lines, '1 suspect')
    assert _has(lines, "unbound: 99  <- rootreg 'XXXX'/0x00000007")


def test_scan_all(store: Shell):
    lines = store.exec_command('diag scan -a')
    for oid, (typ, field) in SHAPES.items():
        row = _row(lines, oid)
        assert f' {typ} ' in row and field in row and '[shape]' in row, row
    for oid in (14, 15, 23, 66):
        assert ' unknown ' in _row(lines, oid)
    assert ' empty ' in _row(lines, 64)


def test_roots(store: Shell):
    lines = store.exec_command('diag roots')
    text = '\n'.join(lines)
    for frag in ("'KVDB'/0x", "'BFS1'/0x00000000 blobfs",
                 "'PADB'/0x00000000 persondb", "'LOGR'/0x00000001"):
        assert frag in text, text
    assert re.search(r'struct_root\s+11\s+kvhash', text), text
    assert re.search(r'credentials\s+32\s+kvhash', text), text
    assert re.search(r'head\s+41\s+logentry', text), text
    assert re.search(r'99\s+\(unbound\)', text), text


@pytest.mark.parametrize('cmd,frags', [
    ('diag show 1', ["'PADB'/0x00000000 (persondb)   -> 30",
                     "'XXXX'/0x00000007              -> 99"]),
    ('diag show 20', ['name limit 12', 'from dir 21, to dir 21',
                      '"a.txt" -> "b.txt"']),
    ('diag show 58', ['version 1', 'rename in flight: body 23',
                      '"old" -> "new"']),
    ('diag show 59', ['name limit 20', 'from dir 21, to dir 24',
                      '"x" -> "yy"']),
    ('diag show 22', ['"a.txt" = dirent file body -> 23',
                      '"sub" = dirent dir root -> 24']),
    ('diag show 12', ['"alpha" = "1"',
                      'spilled 300 B fp=a5a5a5a5 -> 14 = spilled 400 B -> 15']),
    ('diag show 35', ['"p00000001" = {1: 1, 2: "Ada"']),
    ('diag show 30', ['table 1 people      -> 31',
                      'table 2 credentials -> 32']),
    ('diag show 57', ['people[1] -> 71', 'credentials -> 72',
                      'people 70 is not bound']),
    ('diag show 50', ['seq 1           3 B  "abc"',
                      'seq 2           4 B  "defg"']),
    ('diag show 40', ['head -> 41, waypoint -> 42']),
    ('diag show 42', ['next -> 43', 'record 6 B: "second"',
                      'next 43 is not bound']),
    ('diag show 52', ['version 1, depth 1, 2 children', '[0] -> 53']),
    ('diag show 53 kvbucket', ['"k" = "v"']),
    ('diag show 23 logentry', ['decoded as asked']),
])
def test_show(store: Shell, cmd: str, frags: list[str]):
    lines = store.exec_command(cmd)
    for frag in frags:
        assert _has(lines, frag), (frag, lines)


def test_show_refuses(store: Shell):
    assert _has(store.exec_command('diag show 64 kvdb'), 'too short for kvdb')
    assert _has(store.exec_command('diag show 98'), 'not bound')
    assert _has(store.exec_command('diag show 1 nosuch'), 'unknown type')


def test_formats(store: Shell):
    lines = store.exec_command('diag formats')
    for frag in ('v1 bare (2026-08)', 'ring v1 (2026-07)',
                 'v3 spill (2026-10)', 'v1 shards (2026-08)'):
        assert _has(lines, frag), lines
