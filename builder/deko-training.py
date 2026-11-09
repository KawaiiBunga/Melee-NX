#!/usr/bin/env python3
"""Merge the native renderer's shader caches recorded on consoles into one seed.

  python builder/deko-training.py pack-dksh OUT.bin INPUT.bin...

INPUT is a seed (switch/deko/dksh_file.h) followed by each console's
sdmc:/switch/melee-nx/Cache/deko/dksh_local.bin. Every input must carry the first one's compiler id:
a file written by another uam, Tint post-pass or GX config version is stale and is refused. Records
are checked as the console checks them (CRC, layout). A shader is kept once per stage and GLSL hash
(a compiled one replaces a failed one), a module once per hash, and of the named/stage records the
last one of each name, which is what the console keeps when it reads the seed and then its local
file. Writes OUT and OUT.json.

Drop the result on the SD card as sdmc:/switch/melee-nx/initial_dksh_cache.bin: the renderer reads
that path when the NRO's RomFS has no romfs:/deko/initial_dksh_cache.bin, so no rebuild is needed.

The pipeline recipes need no tool here: they are Aurora's own database, and melee-nx already seeds
them (sdmc:/switch/melee-nx/initial_pipeline_cache.db). The recipes are backend independent, so one
seed serves both MELEE_NX_GFX builds. Adapted from KartPadNX builder/deko-training.py.
"""
import hashlib, json, struct, sys, zlib
from pathlib import Path

HEADER = struct.Struct('<4sIQ')
RECORD = struct.Struct('<BII')
KIND_SHADER, KIND_MODULE, KIND_NAMED, KIND_STAGE = 1, 2, 3, 4
STAGES = (0, 4)  # ShaderStage: vertex, fragment


def read_dksh(path):
    data = Path(path).read_bytes()
    if len(data) < HEADER.size:
        sys.exit(f'{path}: shorter than a header')
    magic, version, compiler = HEADER.unpack_from(data, 0)
    if magic != b'SWDK' or version != 1:
        sys.exit(f'{path}: not a version-1 SWDK file')
    records, bad, i = [], 0, HEADER.size
    while i + RECORD.size <= len(data):
        kind, size, crc = RECORD.unpack_from(data, i)
        if i + RECORD.size + size > len(data):
            break  # cut short: the file ends here
        payload = data[i + RECORD.size:i + RECORD.size + size]
        i += RECORD.size + size
        if zlib.crc32(payload, zlib.crc32(bytes([kind]))) != crc:
            bad += 1
            continue
        key = record_key(kind, payload)
        if key is not None:
            records.append((key, kind, payload))
    return compiler, records, {'path': str(path), 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data),
                               'complete_bytes': i, 'bad_crc': bad, 'torn_bytes': len(data) - i}


def record_key(kind, p):
    """The record's identity, or None for one read_dksh_file would skip."""
    if kind == KIND_SHADER and len(p) >= 9 and p[0] in STAGES:
        return ('shader', p[0], p[1:9])
    if kind == KIND_MODULE and len(p) == 24:
        return ('module', p[0:8])
    if kind in (KIND_NAMED, KIND_STAGE) and len(p) >= 2 and p[0] in STAGES:
        n = p[1]
        if len(p) < 2 + n + 9:
            return None
        stride = 18 if kind == KIND_STAGE else 4
        if len(p) != 2 + n + 9 + stride * p[2 + n + 8]:
            return None
        return ('named', p[2:2 + n])
    return None


def pack_dksh(out, inputs):
    compiler, shaders, modules, named, meta = None, {}, {}, {}, []
    for path in inputs:
        cid, records, info = read_dksh(path)
        if compiler is None:
            compiler = cid
        elif cid != compiler:
            sys.exit(f'{path}: compiler id {cid:016x}, not {compiler:016x}: its shaders are stale')
        meta.append(info)
        for key, kind, payload in records:
            if key[0] == 'shader':
                old = shaders.get(key)
                if old is None or (len(old[1]) == 9 and len(payload) > 9):
                    shaders[key] = (kind, payload)
            elif key[0] == 'module':
                modules.setdefault(key, (kind, payload))
            else:
                named.pop(key, None)  # re-insert: dict order becomes the order of each name's last record
                named[key] = (kind, payload)
    body = bytearray(HEADER.pack(b'SWDK', 1, compiler))
    for kind, payload in list(shaders.values()) + list(modules.values()) + list(named.values()):
        body += RECORD.pack(kind, len(payload), zlib.crc32(payload, zlib.crc32(bytes([kind])))) + payload
    Path(out).write_bytes(body)
    compiled = sum(1 for _, p in shaders.values() if len(p) > 9)
    summary = {'compiler_id': f'{compiler:016x}', 'shaders': compiled, 'failed': len(shaders) - compiled,
               'modules': len(modules), 'binding_records': len(named), 'bytes': len(body),
               'sha256': hashlib.sha256(body).hexdigest(), 'inputs': meta}
    Path(out).with_suffix('.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k != 'inputs'}))


if __name__ == '__main__':
    if len(sys.argv) < 4 or sys.argv[1] != 'pack-dksh':
        sys.exit(__doc__)
    pack_dksh(sys.argv[2], sys.argv[3:])
