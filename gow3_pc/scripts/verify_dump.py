#!/usr/bin/env python3
"""Check a game folder for the damage a bad dump leaves, before preparing it.

A dump copied from a console can be damaged with no error shown: a file whose reads failed is all
zeros, a 64 KiB block that came back short ends in zeros, a page of an executable holds random
bytes. A PKG extractor can leave the same damage: LibOrbisPkg's PFSC reader inflates each 64 KiB
block with one read, which on .NET 6+ may return part of it (the CUSA02344 dump of 14 October 2026,
112 damaged files). prepare.py can still succeed (eboot.bin may be whole while level data is not),
and the game then fails far from the cause. Nothing in the folder is changed.

The checks find damage; they cannot prove there is none: a damaged page inside packed level data
looks like any other data. Two dumps whose files hash the same are the stronger proof.

Exit status: 0 nothing found, 1 damage found, 2 the folder could not be checked.
"""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import sys
import zlib

from prepare import parse_self, sfo, span, unpack

SELF_MAGIC = b'O\x15=\x1d'
# All zeros by design: the PKG's reserved area (PS4 PKG Tool writes the same bytes).
ZERO_BY_DESIGN = {'sce_sys/psreserved.dat'}
EXECUTABLE_MAGIC = (SELF_MAGIC, b'\x7fELF')
# Formats whose files always begin with these bytes.
MAGIC = {'.psarc': (b'PSAR',), '.xvag': (b'XVAG',), '.dds': (b'DDS ',), '.vag': (b'VAGp', b'VAGi'),
         '.png': (b'\x89PNG\r\n\x1a\n',), '.wav': (b'RIFF',), '.at9': (b'RIFF',),
         '.elf': EXECUTABLE_MAGIC, '.prx': EXECUTABLE_MAGIC, '.sprx': EXECUTABLE_MAGIC}
TEXT = ('.txt', '.json', '.csv', '.xml')
CONTROL = re.compile(rb'[\x00-\x08\x0e-\x1f]+')
HOLE_GAP = HOLE_MIN = 64  # control bytes closer than this join one hole; shorter holes are ignored
# Executables: zlib level-1 size of a WINDOW of bytes. Code comes out near 1300 and never above
# ~1700; random bytes do not shrink at all (2048 and up).
WINDOW, STEP, RANDOM = 2048, 512, 2000


def merge(starts, length, gap=0):
    """Sorted, disjoint [start, end) spans covering [s, s + length) for each s in starts."""
    spans = []
    for start in sorted(starts):
        if spans and start <= spans[-1][1] + gap:
            spans[-1][1] = max(spans[-1][1], start + length)
        else:
            spans.append([start, start + length])
    return [tuple(s) for s in spans]


def all_zero(path):
    with open(path, 'rb') as f:
        while chunk := f.read(1 << 20):
            if chunk.strip(b'\0'):
                return False
    return True


def text_holes(data):
    """[start, end) spans of a text file that hold no text: zeros a short read left, or random bytes."""
    if data[:2] in (b'\xff\xfe', b'\xfe\xff'):
        return []  # UTF-16: every other byte of Latin text is zero
    holes = []
    for m in CONTROL.finditer(data):
        if holes and m.start() - holes[-1][1] < HOLE_GAP:
            holes[-1][1] = m.end()
        else:
            holes.append([m.start(), m.end()])
    return [(a, z) for a, z in holes if z - a >= HOLE_MIN]


def random_spans(data, base=0):
    """Spans of data that do not compress at all, offset by base."""
    hits = [o for o in range(0, len(data) - WINDOW + 1, STEP)
            if len(zlib.compress(data[o:o + WINDOW], 1)) >= RANDOM]
    return [(base + a, base + z) for a, z in merge(hits, WINDOW)]


def code_range(elf, ph):
    """[start, end) of the functions the unwind table (.eh_frame_hdr) lists; None without one or
    when its encoding is not the usual (pcrel/datarel 4-byte) one."""
    header = next((p for p in ph if p['type'] == 0x6474e550), None)
    if not header:
        return None
    loads = [p for p in ph if p['type'] == 1]
    def read(fmt, address):
        p = next((p for p in loads if p['vaddr'] <= address < p['vaddr'] + p['filesz']), None)
        if not p:
            raise ValueError(f'{address:#x} is outside the image')
        return unpack(fmt, elf, p['offset'] + address - p['vaddr'])
    h = header['vaddr']
    version, pointer, count_format, table = read('<4B', h)
    if (version, pointer, count_format, table) != (1, 0x1b, 0x03, 0x3b):
        return None
    count, = read('<I', h + 8)
    start, end = None, 0
    for i in range(count):
        location, fde = (h + v for v in read('<ii', h + 12 + i * 8))
        begin = fde + 8 + read('<i', fde + 8)[0]
        if begin != location:
            raise ValueError(f'function {i} of the unwind table does not match its record')
        start = location if start is None else min(start, location)
        end = max(end, location + read('<I', fde + 12)[0])
    return (start, end) if start is not None else None


def bad_relocations(elf, ph):
    """ELF offsets of the relocation entries whose target is outside the image."""
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    dynamic = next((p for p in ph if p['type'] == 2), None)
    lib = next((p for p in ph if p['type'] == 0x61000000), None)
    if not dynamic or not lib:
        return []
    tags = {}
    for pos in range(dynamic['offset'], dynamic['offset'] + dynamic['filesz'], 16):
        tag, value = unpack('<QQ', elf, pos)
        if not tag:
            break
        tags.setdefault(tag, value)
    bad = []
    for offset_tag, size_tag in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
        if offset_tag not in tags:
            continue
        start = lib['offset'] + tags[offset_tag]
        table = span(elf, start, tags.get(size_tag, 0))
        for pos in range(0, len(table) - 23, 24):
            target, = struct.unpack_from('<Q', table, pos)
            if not any(p['vaddr'] <= target and target + 8 <= p['vaddr'] + p['memsz'] for p in loads):
                bad.append(start + pos)
    return bad


def executable_problems(data):
    """What is damaged in a plaintext SELF: relocations that point outside the image, and random
    bytes inside its code or its linking data, where real bytes never are. Tables of random-looking
    constants lie outside both."""
    try:
        elf, header, ph, _, _ = parse_self(data)
    except (ValueError, struct.error) as error:
        return [f'not a usable plaintext executable: {error}']
    problems = []
    try:
        if bad := bad_relocations(elf, ph):
            problems.append(f'{len(bad)} relocation entries point outside the image (first at ELF offset {bad[0]:#x})')
    except (ValueError, struct.error) as error:
        problems.append(f'unreadable relocation tables: {error}')
    try:
        code = code_range(elf, ph)
    except (ValueError, struct.error) as error:
        code = None
        problems.append(f'unreadable unwind table: {error}')
    if code:
        p = next(p for p in ph if p['type'] == 1 and p['vaddr'] <= code[0] < p['vaddr'] + p['filesz'])
        start = p['offset'] + code[0] - p['vaddr']
        body = bytes(elf[start:start + min(code[1], p['vaddr'] + p['filesz']) - code[0]])
        problems += [f'random bytes in its code near {a:#x}..{z:#x}' for a, z in random_spans(body, code[0])]
    lib = next((p for p in ph if p['type'] == 0x61000000), None)
    if lib:
        blob = bytes(elf[lib['offset']:lib['offset'] + lib['filesz']])
        problems += [f'random bytes in its linking data near ELF offset {a:#x}..{z:#x}'
                     for a, z in random_spans(blob, lib['offset'])]
    return problems


def describe_holes(data, holes):
    shown = ', '.join(f'{a:#x}..{z:#x}' for a, z in holes[:3]) + (f' and {len(holes) - 3} more' if len(holes) > 3 else '')
    kind = 'zero-filled' if all(not data[a:z].strip(b'\0') for a, z in holes) else 'not text'
    return f'{kind} at {shown}'


def verify(game):
    """(files checked, [(path relative to game, problem)]) for a game folder."""
    findings = []
    for required in ('eboot.bin', 'sce_sys/param.sfo', 'sce_module/libc.prx'):
        if not (game / required).is_file():
            findings.append((required, 'missing'))
    if (game / 'sce_sys/param.sfo').is_file():
        try:
            sfo((game / 'sce_sys/param.sfo').read_bytes())
        except (OSError, ValueError, struct.error) as error:
            findings.append(('sce_sys/param.sfo', f'unreadable: {error}'))
    checked = 0
    for root, dirs, files in os.walk(game):
        dirs.sort()
        for name in sorted(files):
            path = Path(root) / name
            relative = path.relative_to(game).as_posix()
            checked += 1
            try:
                size = path.stat().st_size
                with path.open('rb') as f:
                    head = f.read(4096)
                if not size:
                    if relative in ('eboot.bin', 'sce_sys/param.sfo') or path.suffix.lower() in MAGIC:
                        findings.append((relative, 'empty (0 bytes)'))
                    continue
                if not head.strip(b'\0') and all_zero(path):
                    if relative not in ZERO_BY_DESIGN:
                        findings.append((relative, f'all {size:,} bytes are zero'))
                    continue
                suffix = path.suffix.lower()
                expected = EXECUTABLE_MAGIC if relative == 'eboot.bin' else MAGIC.get(suffix)
                if expected and not head.startswith(expected):
                    findings.append((relative, f'starts with {head[:4].hex()}, not {expected[0]!r}'))
                    continue
                if suffix in TEXT:
                    data = path.read_bytes()
                    if holes := text_holes(data):
                        findings.append((relative, describe_holes(data, holes)))
                elif head.startswith(SELF_MAGIC) and not relative.startswith('sce_sys/'):
                    findings += [(relative, problem) for problem in executable_problems(path.read_bytes())]
            except OSError as error:
                findings.append((relative, f'unreadable: {error}'))
    return checked, findings


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path, help='the game folder (eboot.bin, sce_sys, ...)')
    parser.add_argument('--json', type=Path, help='also write the findings to this file')
    args = parser.parse_args(argv)
    if not args.game.is_dir():
        print(f'Not a folder: {args.game}')
        return 2
    checked, findings = verify(args.game)
    for path, problem in findings:
        print(f'{path}: {problem}')
    damaged = len({path for path, _ in findings})
    print(f'{checked:,} files checked in {args.game}: '
          + (f'{damaged} damaged or missing. Dump or extract the game again; see analysis/ROADMAP.md (dump check).'
             if damaged else 'no damage found.'))
    if args.json:
        args.json.write_text(json.dumps(dict(game=str(args.game), files=checked,
                                             findings=[dict(path=p, problem=q) for p, q in findings]),
                                        ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
    return 1 if findings else 0


if __name__ == '__main__':
    # Paths can hold any language; a console or pipe in a legacy code page cannot encode them all.
    sys.stdout.reconfigure(errors='replace')
    sys.exit(main())
