#!/usr/bin/env python3
"""Self-test for builder/deko-training.py pack-dksh and builder/integrate-deko.py (no Switch tools).

  python builder/test-deko-training.py

pack-dksh: merges two tiny SWDK files (a compiled shader replaces a failed one, the later named
record wins, a torn tail is ignored) and refuses a file with another compiler id.
integrate-deko: stages a copy of Aurora when ref/melee-pc is present, applies the edits twice
(idempotent), and checks every touched file keeps balanced preprocessor conditionals.
"""
import json
import re
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct('<4sIQ')
RECORD = struct.Struct('<BII')


def record(kind, payload):
    return RECORD.pack(kind, len(payload), zlib.crc32(payload, zlib.crc32(bytes([kind])))) + payload


def shader(stage, glsl_hash, dksh=b''):
    return record(1, bytes([stage]) + struct.pack('<Q', glsl_hash) + dksh)


def named(stage, name, glsl_hash, bindings):
    body = bytes([stage, len(name)]) + name + struct.pack('<Q', glsl_hash) + bytes([len(bindings)])
    for b in bindings:
        body += bytes(b)
    return record(3, body)


def test_pack_dksh(tmp):
    a = HEADER.pack(b'SWDK', 1, 0x1122) + shader(0, 1) + shader(4, 2, b'DKSH') + named(0, b'k', 1, [(0, 0, 1, 0)])
    b = HEADER.pack(b'SWDK', 1, 0x1122) + shader(0, 1, b'CODE') + named(0, b'k', 9, [(0, 1, 1, 0)]) + b'\x01\x00'
    bad = HEADER.pack(b'SWDK', 1, 0x9999) + shader(0, 5)
    (tmp / 'a.bin').write_bytes(a)
    (tmp / 'b.bin').write_bytes(b)
    (tmp / 'bad.bin').write_bytes(bad)
    script = str(ROOT / 'builder' / 'deko-training.py')
    r = subprocess.run([sys.executable, '-I', script, 'pack-dksh', str(tmp / 'out.bin'), str(tmp / 'a.bin'),
                        str(tmp / 'b.bin')], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    summary = json.loads((tmp / 'out.json').read_text())
    assert summary['shaders'] == 2 and summary['failed'] == 0, summary
    assert summary['binding_records'] == 1 and summary['compiler_id'] == '0000000000001122', summary
    assert summary['inputs'][1]['torn_bytes'] == 2, summary['inputs'][1]
    out = (tmp / 'out.bin').read_bytes()
    assert struct.pack('<Q', 9) in out and out.count(b'DKSH') == 1 and out.count(b'CODE') == 1
    r = subprocess.run([sys.executable, '-I', script, 'pack-dksh', str(tmp / 'x.bin'), str(tmp / 'a.bin'),
                        str(tmp / 'bad.bin')], capture_output=True, text=True)
    assert r.returncode != 0 and 'stale' in (r.stderr + r.stdout), (r.stdout, r.stderr)
    print('pack-dksh: ok')


def test_integrate(tmp):
    src = ROOT / 'ref/melee-pc/extern/aurora'
    if not (src / 'CMakeLists.txt').is_file():
        print('integrate-deko: skipped (ref/melee-pc not present)')
        return
    module = runpy.run_path(str(ROOT / 'builder' / 'integrate-deko.py'))
    dst = tmp / 'aurora'
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns('.git'))
    module['apply'](dst)
    module['apply'](dst)  # idempotent
    touched = ['lib/webgpu/gpu.cpp', 'lib/webgpu/gpu.hpp', 'lib/aurora.cpp', 'lib/gfx/depth_peek.cpp',
               'lib/gfx/pipeline_cache.cpp', 'lib/webgpu/gpu_cache.cpp', 'lib/gfx/texture_convert.cpp',
               'lib/gx/shader.cpp']
    for name in touched:
        depth = 0
        for line in (dst / name).read_text(encoding='utf-8').splitlines():
            text = line.strip()
            if re.match(r'#\s*(if|ifdef|ifndef)\b', text):
                depth += 1
            elif re.match(r'#\s*endif\b', text):
                depth -= 1
            assert depth >= 0, f'{name}: #endif without #if'
        assert depth == 0, f'{name}: unbalanced preprocessor conditionals'
        assert 'AURORA_GFX_DEKO3D' in (dst / name).read_text(encoding='utf-8'), f'{name}: no deko edit'
    print(f'integrate-deko: ok ({len(touched)} files, second apply a no-op)')


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        test_pack_dksh(tmp)
        test_integrate(tmp)
