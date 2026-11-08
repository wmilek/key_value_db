# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

"""`blob_db export` and `blob_db import` on the native_sim build of app_diag.

A dump made on the host (scripts/blob_db_export.py, the format's second
implementation) is imported line by line into a freshly formatted store, the
way a host script drives a device: one line, then its reply. Exported again,
the store gives back every object at its own id, and next_id. On the way the
test checks the replies to a damaged line, a resent line and a header sent to
a store that is no longer empty.

The objects put more than one enumeration pass's worth of ids in one bucket,
and, in a build with CONFIG_BLOB_DB_LARGE_PAYLOADS, a segmented object.
"""

import random
import sys
from pathlib import Path

from twister_harness import DeviceAdapter, Shell

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import blob_db_export as bx  # noqa: E402


def _has(lines: list[str], text: str) -> bool:
    return any(text in line for line in lines)


def _replies(lines: list[str]) -> list[bx.Record]:
    # The shell echoes the command, record included; replies are A or N.
    return [r for r in bx.scan(lines)[0] if r.type in ('A', 'N')]


def _send(dut: DeviceAdapter, line: str) -> bx.Record:
    # Not exec_command: it waits for the echo of the command, which the
    # shell wraps at the terminal width. Wait for the reply instead.
    dut.write(f'blob_db import {line}\n'.encode())
    replies = _replies(dut.readlines_until(regex=r'%BDB1 [AN] ',
                                           timeout=5.0))
    assert len(replies) == 1, replies
    return replies[0]


def _export(shell: Shell, *args) -> bx.Dump:
    lines = shell.exec_command(' '.join(['blob_db export', *map(str, args)]))
    dump = bx.Dump()
    records, bad = bx.scan(lines)
    assert bad == 0, lines
    dump.add(records)
    return dump


def test_export_import_round_trip(dut: DeviceAdapter, shell: Shell):
    lines = shell.exec_command('blob_db format yes')
    assert _has(lines, 'formatted and mounted'), lines

    # A fresh store holds the root alone, bound to nothing.
    fresh = _export(shell)
    assert fresh.problems() == ([], []), fresh.problems()
    assert fresh.complete() == {1: b''}
    hdr = fresh.header
    n_buckets = int(hdr['buckets'])
    max_payload = int(hdr['max_payload'])
    max_object = int(hdr['max_object'])

    rnd = random.Random(7)
    objs = {1: b'root registry stand-in', 17: b'',
            23: rnd.randbytes(max_payload)}
    # 41 ids in bucket 5: more than export gathers from a bucket per pass.
    for k in range(41):
        objs[5 + n_buckets * k] = rnd.randbytes(1 + k * 3)
    if max_object > max_payload:
        objs[30] = rnd.randbytes(3 * max_payload + 5)    # segmented
    src_next = max(objs) + 10
    dump = bx.pack_lines({'fmt': hdr['fmt'], 'next_id': src_next}, objs)

    # A damaged line is refused and changes nothing; sent again whole, it
    # is taken.
    head = dump[0]
    bad = head[:-1] + ('0' if head[-1] != '0' else '1')
    r = _send(dut, bad)
    assert r.type == 'N' and r.fields[1] == '-', r

    acks = []
    for line in dump:
        r = _send(dut, line)
        assert r.type == 'A', (line, r.text)
        acks.append(r)
    assert acks[0].kv()['next_id'] == str(src_next)

    # The last line again, as after a lost reply: the same reply, not a
    # second application.
    assert _send(dut, dump[-1]).text == acks[-1].text

    # A header is taken only by a freshly formatted store.
    r = _send(dut, bx.make_line('H', f'next_id={src_next}'))
    assert r.type == 'N' and r.kv()['err'] == '-39', r

    back = _export(shell)
    assert back.problems() == ([], []), back.problems()
    assert back.complete() == objs
    # Segments take ids of their own past the source's counter.
    if 30 in objs:
        assert int(back.header['next_id']) > src_next
    else:
        assert int(back.header['next_id']) == src_next

    # A range, as `verify` asks for after a damaged line: data lines only.
    oid = max(objs, key=lambda i: len(objs[i]))
    part = _export(shell, oid, 10, 40)
    assert part.header is None and part.trailer is None
    chunks = part.objects[oid].chunks
    assert sorted(chunks) == [10, 42]
    assert chunks[10] + chunks[42] == objs[oid][10:50]

    lines = shell.exec_command('blob_db stats')
    assert _has(lines, f'{len(objs)} live ids'), lines


def test_import_restarts_after_format(dut: DeviceAdapter, shell: Shell):
    """The same header, sent after `format yes`, starts over on the new
    store: it allocates the ids again rather than replaying its old reply."""
    head = bx.make_line('H', 'next_id=50')
    data = b'after a restart'
    obj = bx.pack_lines({'next_id': 50}, {40: data})[1:-1]   # O, D, E

    for _ in range(2):
        lines = shell.exec_command('blob_db format yes')
        assert _has(lines, 'formatted and mounted'), lines
        r = _send(dut, head)
        assert r.type == 'A' and r.kv()['next_id'] == '50', r

    for line in obj:
        r = _send(dut, line)
        assert r.type == 'A', (line, r.text)
    r = _send(dut, bx.make_line('T', 'objects=1', f'bytes={len(data)}',
                                'errors=0'))
    assert r.type == 'A', r

    back = _export(shell)
    assert back.complete() == {1: b'', 40: data}
