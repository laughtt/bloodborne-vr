#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The game's shader binaries by the hash the renderer knows them by (docs/vr/findings/23).

Bloodborne ships its shaders compiled (dvdroot_ps4/shader/*.shaderbnd.dcx: BND4 archives of PS4
.vpo/.ppo/.dpo/.cpo binaries) with their reflection data: constant buffer names and every
member's name, byte offset and size. shadPS4 names a shader by BinaryInfo.shader_hash (the
pgm_hash in logs, frame captures and the shader cache), which sits after the "OrbShdr"
signature of each binary. This tool indexes all of them:

    python tools/vr_shader_reflect.py --game-dir C:/games/CUSA03173 [--json out.json]
        [--show HASH ...] [--cbuffer NAME] [--member REGEX]

--show prints a shader's file and constant buffer layouts (floats, as the census dumps them);
--cbuffer lists the shaders using a constant buffer; --member lists members matching a regex
with the number of shaders using each (e.g. 'View|Proj|Camera|WVP|Eye' finds every camera input).
Reads the game folder only.
"""
import argparse
import collections
import json
import re
import struct
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT / 'scripts' / 'vr_assets'))
import souls  # noqa: E402

NAME = re.compile(rb'[A-Za-z_][A-Za-z0-9_.\[\]()]{2,}\x00')


def members(data):
    """Constant buffer members: (name, byte offset, size). Records are 36 bytes: flags, offset,
    size, 0, -1, 0, 0, name and type name offsets (relative to the field), so a name offset at p
    belongs to the record starting at p - 28."""
    strings = {m.start(): m.group()[:-1].decode() for m in NAME.finditer(data)}
    out = []
    for p in range(28, len(data) - 8, 4):
        rel = struct.unpack_from('<I', data, p)[0]
        if not rel or p + rel not in strings:
            continue
        _flags, offset, size, _zero, minus1 = struct.unpack_from('<IIIIi', data, p - 28)
        if minus1 == -1 and 0 < size <= 0x1000 and offset < 0x4000:
            out.append((strings[p + rel], offset, size))
    cbuffers = [s for s in strings.values() if s.startswith('cb')]
    return cbuffers, out


def index_shaders(game_dir):
    index = {}
    for archive in sorted((Path(game_dir) / 'dvdroot_ps4' / 'shader').glob('*.shaderbnd.dcx')):
        for f in souls.bnd4_read(souls.read_file(archive)):
            at = f.data.rfind(b'OrbShdr')
            if at < 0:
                continue
            # BinaryInfo: signature[7], version, packed u32, 4 bytes, shader_hash u64, crc32
            shader_hash = struct.unpack_from('<Q', f.data, at + 7 + 1 + 4 + 4)[0]
            cbuffers, mems = members(f.data)
            name = f.name.replace('\\', '/')
            index['%08x' % shader_hash] = {'archive': archive.name.split('.')[0], 'file': name,
                                           'cbuffers': cbuffers, 'members': mems}
    return index


def groups(entry):
    """Members split into constant buffers (offsets restart at each buffer)."""
    out, last = [], None
    for name, offset, size in entry['members']:
        if last is None or offset <= last:
            out.append([])
        out[-1].append((name, offset, size))
        last = offset
    return out


def show(h, entry):
    print(f'{h}  {entry["archive"]}/{entry["file"]}')
    names = entry['cbuffers']
    for i, g in enumerate(groups(entry)):
        size = max(o + s for _, o, s in g)
        label = names[i] if i < len(names) and len(groups(entry)) == len(names) else f'#{i}'
        print(f'  {label} ({size} bytes)')
        for name, offset, size in g:
            print(f'    float {offset // 4:3d}..{(offset + size) // 4 - 1:3d}  {name}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game-dir', default='C:/games/CUSA03173')
    ap.add_argument('--json', help='write the index here')
    ap.add_argument('--show', nargs='*', default=[], help='shader hashes (hex, 8 digits)')
    ap.add_argument('--cbuffer', help='list the shaders that use this constant buffer')
    ap.add_argument('--member', help='count the shaders using members that match this regex')
    a = ap.parse_args()
    index = index_shaders(a.game_dir)
    print(f'{len(index)} shaders indexed')
    if a.json:
        Path(a.json).write_text(json.dumps(index, indent=0))
    for h in a.show:
        h = h.lower().removeprefix('0x').lstrip('0').rjust(8, '0')
        if h in index:
            show(h, index[h])
        else:
            print(f'{h}: not in the game\'s shader archives')
    if a.cbuffer:
        users = sorted((e['archive'], e['file'], h) for h, e in index.items()
                       if a.cbuffer in e['cbuffers'])
        print(f'{len(users)} shaders use {a.cbuffer}')
        for archive, file, h in users[:40]:
            print(f'  {h} {archive}/{file}')
    if a.member:
        rx = re.compile(a.member, re.I)
        count = collections.Counter()
        for e in index.values():
            for name, offset, size in e['members']:
                if rx.search(name):
                    count[(name, offset // 4, size // 4)] += 1
        for (name, f0, n), c in sorted(count.items()):
            print(f'  {name:34s} float {f0:3d} (+{n:3d})  {c:5d} shaders')


if __name__ == '__main__':
    main()
