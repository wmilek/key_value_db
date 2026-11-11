#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

"""Host side of `blob_db export` / `blob_db import` (app_diag shell).

The line format is doc/tools/blob_db_export.md. Every record is one line

    %BDB1 <type> <fields...> *<crc16>

found anywhere in a console capture, so log output and shell prompts around
or between the records do no harm. A line whose CRC fails is dropped and
reported; the data it carried can be fetched again with the
`blob_db export <id> <off> <len>` commands `verify` prints.

    verify   LOG...           check a capture; list what to resend
    extract  LOG... -o DIR    write DIR/manifest.json and DIR/<id>.bin
    pack     DIR              a dump from such a directory, to stdout
    commands LOG...|DIR       `blob_db import` lines to feed the shell

Several LOG files are merged, so the output of resend commands can be passed
after the original capture. `-` reads stdin.
"""

from __future__ import annotations

import argparse
import binascii
import json
import re
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

PREFIX = '%BDB1'
CHUNK = 32

_ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')
# The text the CRC covers holds no '*' and no control characters.
_RECORD = re.compile(re.escape(PREFIX) + r' ([^*\x00-\x1f]*?) \*([0-9a-fA-F]{4})\b')


def line_crc(text: str) -> int:
    """CRC-16/CCITT-FALSE of a record's text, as the device computes it."""
    return binascii.crc_hqx(text.encode('ascii'), 0xFFFF)


def make_line(*fields: object) -> str:
    """One record line: prefix, fields, checksum."""
    text = ' '.join([PREFIX, *(str(f) for f in fields)])
    return f'{text} *{line_crc(text):04x}'


@dataclass
class Record:
    text: str           # the checked text, prefix included
    fields: list[str]   # type first

    @property
    def type(self) -> str:
        return self.fields[0]

    def kv(self) -> dict[str, str]:
        return dict(f.split('=', 1) for f in self.fields[1:] if '=' in f)


def scan(lines) -> tuple[list[Record], int]:
    """Records found in console lines, and how many failed their CRC."""
    records, bad = [], 0
    for raw in lines:
        line = _ANSI.sub('', raw)
        for m in _RECORD.finditer(line):
            text = f'{PREFIX} {m.group(1)}'
            if line_crc(text) != int(m.group(2), 16):
                bad += 1
                continue
            records.append(Record(text, m.group(1).split(' ')))
    return records, bad


@dataclass
class Obj:
    length: int | None = None
    crc: int | None = None          # from E
    chunks: dict[int, bytes] = field(default_factory=dict)
    error: str | None = None        # from X

    def gaps(self) -> list[tuple[int, int]]:
        """[off, len) ranges with no data, given a known length."""
        out, pos = [], 0
        for off in sorted(self.chunks):
            if off > pos:
                out.append((pos, off - pos))
            pos = max(pos, off + len(self.chunks[off]))
        if self.length is not None and pos < self.length:
            out.append((pos, self.length - pos))
        return out

    def data(self) -> bytes | None:
        if self.length is None or self.gaps():
            return None
        buf = bytearray(self.length)
        for off, b in self.chunks.items():
            buf[off:off + len(b)] = b
        return bytes(buf)


@dataclass
class Dump:
    header: dict[str, str] | None = None
    trailer: dict[str, str] | None = None
    objects: dict[int, Obj] = field(default_factory=dict)
    bucket_errors: list[str] = field(default_factory=list)
    bad_lines: int = 0
    records_in_first: int = 0   # H .. before T, of the first dump

    def add(self, records: list[Record]) -> None:
        counting = False
        for r in records:
            t, f = r.type, r.fields
            if t == 'H':
                if self.header is None:
                    self.header = r.kv()
                    counting = True
            elif t == 'O':
                o = self.objects.setdefault(int(f[1]), Obj())
                o.length = int(f[2])
            elif t == 'D':
                o = self.objects.setdefault(int(f[1]), Obj())
                o.chunks[int(f[2])] = bytes.fromhex(f[3])
            elif t == 'E':
                o = self.objects.setdefault(int(f[1]), Obj())
                o.length, o.crc = int(f[2]), int(f[3], 16)
            elif t == 'X' and f[1] == 'id':
                self.objects.setdefault(int(f[2]), Obj()).error = f[3]
            elif t == 'X':
                self.bucket_errors.append(f'{f[1]} {f[2]}: {f[3]}')
            elif t == 'T':
                if counting:
                    self.trailer = r.kv()
                    counting = False
                continue
            else:
                continue    # A / N replies, echoed import commands' types
            if counting:
                self.records_in_first += 1

    def problems(self) -> tuple[list[str], list[str]]:
        """What is wrong, and the shell commands that fetch it again."""
        issues, resend = [], []
        if self.header is None:
            issues.append('no H (header) record')
        if self.trailer is None:
            issues.append('no T (trailer) record: the capture is cut short')
        for oid in sorted(self.objects):
            o = self.objects[oid]
            if o.error is not None:
                issues.append(f'id {oid}: unreadable on the device ({o.error})')
                continue
            if o.length is None or o.crc is None:
                issues.append(f'id {oid}: O or E record missing')
                resend.append(f'blob_db export {oid}')
                continue
            gaps = o.gaps()
            for off, n in gaps:
                issues.append(f'id {oid}: bytes {off}..{off + n - 1} missing')
                resend.append(f'blob_db export {oid} {off} {n}')
            if not gaps and zlib.crc32(o.data()) != o.crc:
                issues.append(f'id {oid}: CRC-32 mismatch')
                resend.append(f'blob_db export {oid}')
        for e in self.bucket_errors:
            issues.append(f'unreadable on the device: {e}')
        if self.trailer is not None:
            good = [o for o in self.objects.values()
                    if o.error is None and o.crc is not None]
            t = self.trailer
            if int(t['objects']) != len(good):
                issues.append(f"trailer says {t['objects']} objects, "
                              f'{len(good)} seen')
            n_err = (sum(o.error is not None for o in self.objects.values())
                     + len(self.bucket_errors))
            if int(t['errors']) != n_err:
                issues.append(f"trailer says {t['errors']} errors, "
                              f'{n_err} seen')
        return issues, resend

    def warnings(self) -> list[str]:
        """What is readable but will not restore as it is."""
        out = []
        try:
            next_id = int(self.header['next_id'])
        except (TypeError, KeyError, ValueError):
            return out
        late = sorted(oid for oid in self.complete() if oid >= next_id)
        if late:
            # The header is taken before the walk: an id allocated while the
            # export ran can be in the dump. Import refuses it.
            out.append(f'{len(late)} objects have ids at or past the '
                       f"header's next_id {next_id} ({late[0]}..{late[-1]}): "
                       'the store was written during the export, and import '
                       'refuses them. Export from app_diag, or with the '
                       "application's writers stopped.")
        return out

    def complete(self) -> dict[int, bytes]:
        out = {}
        for oid, o in self.objects.items():
            d = o.data() if o.error is None else None
            if d is not None and zlib.crc32(d) == o.crc:
                out[oid] = d
        return out


def load(paths: list[str]) -> Dump:
    dump = Dump()
    for p in paths:
        if p == '-':
            text = sys.stdin.read()
        else:
            text = Path(p).read_text(encoding='utf-8', errors='replace')
        records, bad = scan(text.splitlines())
        dump.bad_lines += bad
        dump.add(records)
    return dump


def pack_lines(header: dict[str, str], objects: dict[int, bytes]) -> list[str]:
    """A dump of @p objects, as `blob_db export` would print it."""
    lines = [make_line('H', *(f'{k}={v}' for k, v in header.items()))]
    total = 0
    for oid in sorted(objects):
        data = objects[oid]
        lines.append(make_line('O', oid, len(data)))
        for off in range(0, len(data), CHUNK):
            lines.append(make_line('D', oid, off, data[off:off + CHUNK].hex()))
        lines.append(make_line('E', oid, len(data), f'{zlib.crc32(data):08x}'))
        total += len(data)
    lines.append(make_line('T', f'objects={len(objects)}', f'bytes={total}',
                           'errors=0', f'lines={len(lines)}'))
    return lines


def write_dir(dump: Dump, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    objs = dump.complete()
    for oid, data in objs.items():
        (out / f'{oid}.bin').write_bytes(data)
    manifest = {
        'header': dump.header,
        'objects': [{'id': oid, 'len': len(objs[oid]),
                     'crc32': f'{zlib.crc32(objs[oid]):08x}',
                     'file': f'{oid}.bin'} for oid in sorted(objs)],
    }
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


def read_dir(src: Path) -> tuple[dict[str, str], dict[int, bytes]]:
    manifest = json.loads((src / 'manifest.json').read_text())
    objs = {o['id']: (src / o['file']).read_bytes() for o in manifest['objects']}
    return manifest['header'], objs


def _report(dump: Dump) -> bool:
    issues, resend = dump.problems()
    if dump.bad_lines:
        print(f'{dump.bad_lines} lines failed their CRC and were dropped',
              file=sys.stderr)
    for i in issues:
        print(f'problem: {i}', file=sys.stderr)
    for w in dump.warnings():
        print(f'warning: {w}', file=sys.stderr)
    if resend:
        print('resend with:', file=sys.stderr)
        for c in dict.fromkeys(resend):
            print(f'  {c}', file=sys.stderr)
    return not issues


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('verify', help='check a capture')
    p.add_argument('logs', nargs='+')
    p = sub.add_parser('extract', help='write objects and a manifest')
    p.add_argument('logs', nargs='+')
    p.add_argument('-o', '--out', required=True, type=Path)
    p = sub.add_parser('pack', help='a dump from an extracted directory')
    p.add_argument('dir', type=Path)
    p = sub.add_parser('commands', help='`blob_db import` lines for the shell')
    p.add_argument('src', nargs='+', help='capture(s), or one directory')
    p.add_argument('--format', action='store_true',
                   help='start with `blob_db format yes` (destroys the store)')
    args = ap.parse_args(argv)

    if args.cmd == 'verify':
        dump = load(args.logs)
        ok = _report(dump)
        good = dump.complete()
        print(f'{len(good)} objects, {sum(map(len, good.values()))} B complete; '
              f"next_id {dump.header.get('next_id') if dump.header else '?'}")
        return 0 if ok else 1

    if args.cmd == 'extract':
        dump = load(args.logs)
        ok = _report(dump)
        write_dir(dump, args.out)
        return 0 if ok else 1

    if args.cmd == 'pack':
        header, objs = read_dir(args.dir)
        print('\n'.join(pack_lines(header, objs)))
        return 0

    # commands
    if len(args.src) == 1 and Path(args.src[0]).is_dir():
        header, objs = read_dir(Path(args.src[0]))
    else:
        dump = load(args.src)
        if not _report(dump):
            print('not a complete dump; refusing to restore it', file=sys.stderr)
            return 1
        header, objs = dump.header, dump.complete()
    if args.format:
        print('blob_db format yes')
    for line in pack_lines(header, objs):
        print(f'blob_db import {line}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
