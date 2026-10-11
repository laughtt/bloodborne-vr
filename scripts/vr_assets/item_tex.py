"""Albedo textures of the held-item meshes (the sfx/obj flvers of throwables/consumables/items_extract).

The bundles hold each texture as sfx/tex/<stem>.tpf (PS4, BC1/BC7); a flver material names it as
"...<stem>.tif". resolve() finds the albedo of a material, add() decodes it once into a shared sheet:

    out/vr_assets/items/tex/<key>.rgba      decoded pixels (u32 width, u32 height, RGBA8), the cache
    out/vr_assets/items/tex_sheet.png/.json  every cached texture shelf-packed into one image (2 px
                                             padding, edge-extended) and key -> [x, y, w, h]

The bridge appends the sheet below its icon atlas (UiAtlas::Load) and the held meshes name their
texture per submesh ("texture": key in the bbmodel json). Nothing here is committed (game-derived).
"""
from __future__ import annotations

import json
import re
import struct
from pathlib import Path

import tpf

SHEET_W = 2048
PAD = 2
_cache: dict = {}
_dir: Path | None = None


def index_bundle(files) -> dict:
    """basename (lower case, e.g. 's10009.tpf') -> BndFile, of the entries under sfx/tex."""
    return {f.basename.lower(): f for f in files if f.basename.lower().endswith('.tpf')}


def albedo_stem(flver, material) -> str | None:
    """The diffuse texture stem of a material: the 'diffuse'/'albedo' typed slot, else the first
    texture whose stem has no _n/_r/_s suffix and is not a SysTex dummy."""
    texs = flver.textures[material.texture_index:material.texture_index + material.texture_count]
    best = None
    for t in texs:
        path = (t.path or '').replace(chr(92), '/')
        stem = path.rsplit('/', 1)[-1].rsplit('.', 1)[0].lower()
        if not stem or stem.startswith('systex') or 'dummy' in stem:
            continue
        if re.search(r'_[nrsh]$', stem):
            continue
        if 'diffuse' in (t.type or '').lower() or 'albedo' in (t.type or '').lower():
            return stem
        best = best or stem
    return best


def _load_dir(out: Path):
    global _dir
    if _dir == out:
        return
    _dir = out
    _cache.clear()
    d = out / 'items' / 'tex'
    if d.is_dir():
        for p in d.glob('*.rgba'):
            raw = p.read_bytes()
            w, h = struct.unpack_from('<II', raw)
            _cache[p.stem] = (w, h, raw[8:])


def add(out: Path, stem: str, bundle_tpfs: dict, opaque: bool = True) -> str | None:
    """Decodes `stem` from the bundle (once) and returns its sheet key, None when it is not there."""
    _load_dir(out)
    key = stem
    if key in _cache:
        return key
    entry = bundle_tpfs.get(stem + '.tpf')
    if entry is None:
        return None
    try:
        textures = tpf.tpf_read(entry.data)
        w, h, px = tpf.texture_rgba(textures[0])
    except Exception as e:  # noqa: BLE001 - a texture we cannot decode just stays flat
        print(f'texture {stem}: {e}')
        return None
    px = bytearray(px)
    if opaque:
        px[3::4] = b'\xff' * (len(px) // 4)
    _cache[key] = (w, h, bytes(px))
    d = out / 'items' / 'tex'
    d.mkdir(parents=True, exist_ok=True)
    (d / f'{key}.rgba').write_bytes(struct.pack('<II', w, h) + bytes(px))
    return key


def flush(out: Path):
    """Packs every cached texture into items/tex_sheet.png/.json."""
    _load_dir(out)
    if not _cache:
        return
    rects = {}
    x = y = shelf = 0
    for key in sorted(_cache, key=lambda k: (-_cache[k][1], k)):
        w, h, _ = _cache[key]
        if x + w + 2 * PAD > SHEET_W:
            x, y, shelf = 0, y + shelf, 0
        rects[key] = [x + PAD, y + PAD, w, h]
        x += w + 2 * PAD
        shelf = max(shelf, h + 2 * PAD)
    height = y + shelf
    sheet = bytearray(SHEET_W * height * 4)
    for key, (rx, ry, w, h) in rects.items():
        _w, _h, px = _cache[key]
        for row in range(-PAD, h + PAD):  # edge-extended padding
            sy = min(max(row, 0), h - 1)
            line = px[sy * w * 4:(sy + 1) * w * 4]
            line = line[:4] * PAD + line + line[-4:] * PAD
            o = ((ry + row) * SHEET_W + rx - PAD) * 4
            sheet[o:o + len(line)] = line
    items = out / 'items'
    tpf.write_png(items / 'tex_sheet.png', SHEET_W, height, bytes(sheet))
    (items / 'tex_sheet.json').write_text(
        json.dumps({'width': SHEET_W, 'height': height, 'pad': PAD, 'textures': rects}, indent=1),
        encoding='utf-8')
