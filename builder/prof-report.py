#!/usr/bin/env python3
"""Attribute an on-console profile (sdmc:/switch/melee-nx/prof.bin) to functions.

  python builder/prof-report.py PROF.BIN ELF [--top N] [--nm PATH] [--json OUT] [--from-ms A] [--to-ms B]

PROF.BIN is written by switch/src/prof_switch.cpp when perf.cfg has `prof_seconds N`. ELF must be the
one the profile was recorded with: the header's anchor address and the ELF's melee_nx_prof_anchor give
the NRO's load bias. Samples are attributed to the function whose symbol contains the PC, per thread
role (0 game, 1 FIFO, 2 render, 3 shader compile). No debug info is needed; GCC's .constprop, .isra,
.part and .cold clones count toward their function.
"""
import argparse
import bisect
import collections
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HEADER = struct.Struct('<8sQII48s')
RECORD = struct.Struct('<IIQQ')
ROLE_NAMES = {0: 'game', 1: 'fifo', 2: 'render', 3: 'compile'}
TOOL_DIRS = [Path(r'E:\tools\devkitPro\devkitA64\bin')]
CLONE = re.compile(r'\.(constprop|isra|part|cold|lto_priv)(\.\d+)*$')


def find_nm(explicit=None):
    if explicit:
        return str(explicit)
    for directory in TOOL_DIRS:
        for name in ('aarch64-none-elf-nm.exe', 'aarch64-none-elf-nm'):
            if (directory / name).is_file():
                return str(directory / name)
    for name in ('aarch64-none-elf-nm', 'llvm-nm', 'nm'):
        found = shutil.which(name)
        if found:
            return found
    sys.exit('no nm found (pass --nm)')


def read_profile(path, from_ms=None, to_ms=None):
    """(header, [(role, pc)]) from a prof.bin; a torn last record is dropped. from_ms/to_ms keep only
    samples in that window (milliseconds since recording started)."""
    data = Path(path).read_bytes()
    magic, anchor, hz, record_size, stamp = HEADER.unpack_from(data, 0)
    if magic.rstrip(b'\0') != b'MNXPROF1' or record_size != RECORD.size:
        sys.exit(f'{path}: not a melee-nx profile (magic {magic!r}, record size {record_size})')
    body = memoryview(data)[HEADER.size:]
    usable = len(body) - len(body) % RECORD.size
    samples, last_ms = [], 0
    for t_ms, role, pc, _lr in RECORD.iter_unpack(body[:usable]):
        last_ms = t_ms
        if (from_ms is None or t_ms >= from_ms) and (to_ms is None or t_ms < to_ms):
            samples.append((role, pc))
    return {'anchor': anchor, 'hz': hz, 'records': len(samples), 'seconds': last_ms / 1000.0,
            'stamp': stamp.split(b'\0', 1)[0].decode('ascii', 'replace')}, samples


def parse_nm(text):
    """Sorted [(address, size, name)] of the code symbols in `nm -C -S --defined-only` output."""
    symbols = []
    for line in text.splitlines():
        parts = line.split(None, 3)
        if len(parts) < 4 or parts[2] not in 'tTwW':
            continue
        try:
            address, size = int(parts[0], 16), int(parts[1], 16)
        except ValueError:
            continue
        symbols.append((address, size, parts[3].strip()))
    symbols.sort()
    return symbols


def attribute(samples, symbols, bias, slack=0x400):
    """{role: Counter{function: samples}}. A PC past its symbol's end (and `slack`) is '(unknown)'."""
    starts = [s[0] for s in symbols]
    by_role = collections.defaultdict(collections.Counter)
    for role, pc in samples:
        address = pc - bias
        i = bisect.bisect_right(starts, address) - 1
        name = '(unknown)'
        if i >= 0:
            start, size, symbol = symbols[i]
            if address < start + max(size, 4) + slack:
                name = CLONE.sub('', symbol)
        by_role[role][name] += 1
    return by_role


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('prof', type=Path)
    ap.add_argument('elf', type=Path)
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--nm')
    ap.add_argument('--json', type=Path)
    ap.add_argument('--from-ms', type=int, help='only samples at or after this time (ms since recording began)')
    ap.add_argument('--to-ms', type=int, help='only samples before this time')
    args = ap.parse_args()
    header, samples = read_profile(args.prof, args.from_ms, args.to_ms)
    nm = subprocess.run([find_nm(args.nm), '-C', '-S', '--defined-only', str(args.elf)], capture_output=True,
                        text=True, errors='replace')
    symbols = parse_nm(nm.stdout)
    anchor = [a for a, _, name in symbols if name == 'melee_nx_prof_anchor']
    if not anchor:
        sys.exit(f'{args.elf}: no melee_nx_prof_anchor symbol (is it the ELF this profile came from?)')
    bias = header['anchor'] - anchor[0]
    by_role = attribute(samples, symbols, bias)
    print(f"{args.prof}: {header['records']} samples over {header['seconds']:.0f} s at {header['hz']} Hz, "
          f"build {header['stamp']}")
    report = {'header': header, 'roles': {}}
    for role in sorted(by_role):
        counter = by_role[role]
        total = sum(counter.values())
        print(f'\n== role {role} ({ROLE_NAMES.get(role, "?")}): {total} samples')
        rows, acc = [], 0
        for name, n in counter.most_common(args.top):
            acc += n
            rows.append({'function': name, 'samples': n, 'percent': round(100.0 * n / total, 2)})
            print(f'{100.0 * n / total:6.2f}% {100.0 * acc / total:6.2f}%  {name[:120]}')
        report['roles'][ROLE_NAMES.get(role, str(role))] = {'samples': total, 'top': rows}
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
