#!/usr/bin/env python3
"""Item icons, item names and the menu font from the user's own Bloodborne dump, for the VR menu
prototypes in sandbox3d (gpu/xr_bridge/sandbox3d/ui*). Output goes to out/vr_assets/ui/
(git-ignored; never commit game assets).

    python scripts/vr_assets/ui_extract.py [--game C:/games/CUSA03173] [--out out/vr_assets]
        [--cell 128] [--armor] [--gestures] [--jobs N] [--lang engus] [--quiet]

The dump is only read. Writes:
    ui/icons.png    RGBA atlas: one `cell` x `cell` picture per icon id (menu/menu_knowledge_
                    <iconId:05d>.tpf.dcx, 400x400 BC7, PS4-tiled), area-filtered, colors bled into
                    the transparent border so mipmaps and bilinear filtering keep no dark fringe
    ui/icons.json   {cell, width, height, icons: {icon id: [x, y]}, items: [...]} where an item is
                    {kind goods|weapon|armor, id, name, icon, ...}: goods with max (stack size),
                    type (0 item, 1 key, 2 material, 6 chalice) and category; weapons with hand
                    (left: firearms, torch, shields), model (wp_a_XXXX) and category; names from
                    msg/<lang>/item.msgbnd.dcx (FMG 10 goods, 11 weapons, 12 armor). With the
                    items catalog (out/vr_assets/items/items.json, doc 17) goods also get "use"
                    (heal, ammo, throw, buff, tool, other: what using it does) and "effect" (its
                    SpEffect / Bullet summary, e.g. "fire attack +80, 60 s")
    ui/font.png     8-bit signed distance field atlas of the menu font (menu/<lang>/font.gfx,
    ui/font.json    DefineFont3) for ASCII and every character of the exported names
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
from multiprocessing import Pool
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import catalog  # noqa: E402
import gfxfont  # noqa: E402
import souls  # noqa: E402
import tpf  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / 'out' / 'vr_assets'
ATLAS_WIDTH = 2048
FMG_GOODS, FMG_WEAPONS, FMG_ARMOR, FMG_GOODS_INFO = 10, 11, 12, 20
GOODS_TYPES = {0: 'item', 1: 'key', 2: 'material', 6: 'chalice'}


USES = {'restorative': 'heal', 'bullets': 'ammo', 'thrown': 'throw', 'buff': 'buff',
        'bullet-driven': 'tool'}


def merge_catalog(items: list, catalog_path: Path) -> int:
    """Adds "use" and "effect" to goods from doc 17's items.json (when it exists)."""
    if not catalog_path.exists():
        return 0
    rows = {row['id']: row for row in json.loads(catalog_path.read_text(encoding='utf-8'))['items']}
    merged = 0
    for item in items:
        row = rows.get(item['id']) if item['kind'] == 'goods' else None
        if not row:
            continue
        category = row.get('category', '').lower()
        item['use'] = next((v for k, v in USES.items() if category.startswith(k)), 'other')
        effect = []
        for text in (row.get('spEffect') or {}).get('summary') or []:
            if text.startswith('transfusion HP '):
                value = text.split()[2].rstrip('%')
                try:
                    effect.append(f'restores {abs(float(value)):g}% HP')
                except ValueError:
                    effect.append(text)
            elif text.startswith('duration '):
                effect.append(text.replace('duration ', '').replace('.0s', ' s'))
            else:
                effect.append(text)
        bullet = row.get('bullet') or {}
        if bullet.get('dist'):
            effect.append(f'thrown, reaches {bullet["dist"]:g} m')
        item['effect'] = ', '.join(effect)
        merged += 1
    return merged


def knowledge_path(root: Path, icon: int) -> Path:
    return root / 'menu' / f'menu_knowledge_{icon:05d}.tpf.dcx'


def goods_category(row: dict, name: str) -> str:
    icon = row.get('iconId') or 0
    if icon >= 5000:
        return 'gesture'
    kind = GOODS_TYPES.get(row.get('goodsType'), 'item')
    if kind != 'item':
        return kind
    if name in ('Quicksilver Bullets', 'Blood Bullets'):
        return 'ammo'
    if row.get('isConsume') and (row.get('maxNum') or 0) > 1:
        return 'consumable'
    return 'tool'


def collect_items(root: Path, lang: str, armor: bool, gestures: bool) -> list:
    files = souls.bnd4_read((root / 'msg' / lang / 'item.msgbnd.dcx').read_bytes())
    fmg = {f.id: souls.fmg_read(f.data) for f in files}
    params, defs = catalog.load_params(root)
    items = []
    seen_goods = set()
    for row in souls.param_table(params, defs, 'EquipParamGoods'):
        name = fmg.get(FMG_GOODS, {}).get(row['id'], '')
        icon = row.get('iconId') or 0
        if not name or name == '*' or icon <= 0:
            continue
        category = goods_category(row, name)
        if category == 'gesture' and not gestures:
            continue
        key = (name, icon)
        if key in seen_goods:  # e.g. the six "Petrified Blood Gem" rows of each kind
            continue
        seen_goods.add(key)
        items.append({'kind': 'goods', 'id': row['id'], 'name': name, 'icon': icon,
                      'max': row.get('maxNum') or 1, 'type': row.get('goodsType') or 0,
                      'category': category, 'sort': row.get('sortId'),
                      'info': fmg.get(FMG_GOODS_INFO, {}).get(row['id'], '')})
    parts_dir = root / 'parts'
    seen_icons = set()
    for row in souls.param_table(params, defs, 'EquipParamWeapon'):
        name = fmg.get(FMG_WEAPONS, {}).get(row['id'], '')
        icon = row.get('iconId') or 0
        if not name or name == '*' or icon <= 0 or icon in seen_icons:
            continue  # the first named row of each picture: the base weapon
        seen_icons.add(icon)
        left = bool(row.get('leftHandEquipable'))
        model = row.get('equipModelId')
        items.append({'kind': 'weapon', 'id': row['id'], 'name': name, 'icon': icon,
                      'hand': 'left' if left else 'right',
                      'category': 'firearm' if left and row.get('weaponCategory') == 0 else
                                  'left' if left else 'trick',
                      'model': catalog.resolve_part(parts_dir, 'wp', model, 'a')
                               if model is not None else None,
                      'sort': row.get('sortId')})
    if armor:
        seen_icons = set()
        for row in souls.param_table(params, defs, 'EquipParamProtector'):
            name = fmg.get(FMG_ARMOR, {}).get(row['id'], '')
            icon = row.get('iconIdM') or 0
            if not name or name in ('*', 'NOT NEEDED') or icon <= 0 or icon in seen_icons:
                continue
            seen_icons.add(icon)
            slot = next((s for s in ('head', 'body', 'arm', 'leg') if row.get(f'{s}Equip')), '')
            items.append({'kind': 'armor', 'id': row['id'], 'name': name, 'icon': icon,
                          'category': slot, 'sort': row.get('sortId')})
    return items


# ---- Icons ----------------------------------------------------------------------------------------

def _taps(src: int, dst: int) -> list:
    scale = src / dst
    out = []
    for i in range(dst):
        a, b = i * scale, (i + 1) * scale
        taps = []
        j = int(a)
        while j < b and j < src:
            w = min(b, j + 1) - max(a, j)
            if w > 1e-9:
                taps.append((j, w / scale))
            j += 1
        out.append(taps)
    return out


def resize_rgba(width: int, height: int, rgba: bytes, new_w: int, new_h: int) -> bytes:
    """Area-filtered resize with premultiplied alpha; straight alpha out."""
    pre = []
    for i in range(0, len(rgba), 4):
        a = rgba[i + 3]
        f = a / 255.0
        pre.append((rgba[i] * f, rgba[i + 1] * f, rgba[i + 2] * f, float(a)))
    taps_x, taps_y = _taps(width, new_w), _taps(height, new_h)
    rows = []
    for y in range(height):
        base = y * width
        row = []
        for taps in taps_x:
            r = g = b = a = 0.0
            for j, w in taps:
                p = pre[base + j]
                r += p[0] * w
                g += p[1] * w
                b += p[2] * w
                a += p[3] * w
            row.append((r, g, b, a))
        rows.append(row)
    out = bytearray(new_w * new_h * 4)
    for y, taps in enumerate(taps_y):
        for x in range(new_w):
            r = g = b = a = 0.0
            for j, w in taps:
                p = rows[j][x]
                r += p[0] * w
                g += p[1] * w
                b += p[2] * w
                a += p[3] * w
            o = (y * new_w + x) * 4
            if a > 0.5:
                f = 255.0 / a
                out[o] = min(255, int(r * f + 0.5))
                out[o + 1] = min(255, int(g * f + 0.5))
                out[o + 2] = min(255, int(b * f + 0.5))
            out[o + 3] = min(255, int(a + 0.5))
    return bytes(out)


def bleed(width: int, height: int, rgba: bytes, passes: int = 8) -> bytes:
    """Copies colors into fully transparent pixels from their visible neighbours (alpha stays 0),
    so filtering across an icon's edge does not darken it."""
    px = bytearray(rgba)
    known = [px[i * 4 + 3] > 0 for i in range(width * height)]
    for _ in range(passes):
        grow = []
        for y in range(height):
            for x in range(width):
                i = y * width + x
                if known[i]:
                    continue
                r = g = b = n = 0
                for dy in (-1, 0, 1):
                    yy = y + dy
                    if yy < 0 or yy >= height:
                        continue
                    for dx in (-1, 0, 1):
                        xx = x + dx
                        if 0 <= xx < width and known[yy * width + xx]:
                            o = (yy * width + xx) * 4
                            r += px[o]
                            g += px[o + 1]
                            b += px[o + 2]
                            n += 1
                if n:
                    grow.append((i, r // n, g // n, b // n))
        if not grow:
            break
        for i, r, g, b in grow:
            px[i * 4:i * 4 + 3] = bytes((r, g, b))
            known[i] = True
    return bytes(px)


def icon_job(args):
    """(icon id, cell RGBA or None, error) for one knowledge texture (runs in a worker)."""
    path, icon, cell = args
    try:
        textures = tpf.tpf_read(Path(path).read_bytes())
        w, h, rgba = tpf.texture_rgba(textures[0])
        small = resize_rgba(w, h, rgba, cell, cell)
        return icon, bleed(cell, cell, small), ''
    except (OSError, ValueError, tpf.TextureError) as error:
        return icon, None, str(error)


def build_atlas(root: Path, icons: list, cell: int, jobs: int, log) -> tuple:
    work = [(str(knowledge_path(root, i)), i, cell) for i in icons
            if knowledge_path(root, i).exists()]
    missing = sorted(set(icons) - {w[1] for w in work})
    started = time.time()
    with Pool(max(1, jobs)) as pool:
        results = pool.map(icon_job, work, chunksize=2)
    cells = {icon: px for icon, px, _err in results if px}
    errors = {icon: err for icon, px, err in results if not px}
    columns = ATLAS_WIDTH // cell
    rows = max(1, math.ceil(len(cells) / columns))
    height = rows * cell
    atlas = bytearray(ATLAS_WIDTH * height * 4)
    placed = {}
    for k, icon in enumerate(sorted(cells)):
        x0, y0 = (k % columns) * cell, (k // columns) * cell
        src = cells[icon]
        for row in range(cell):
            o = ((y0 + row) * ATLAS_WIDTH + x0) * 4
            atlas[o:o + cell * 4] = src[row * cell * 4:(row + 1) * cell * 4]
        placed[icon] = [x0, y0]
    log(f'icons: {len(cells)} decoded in {time.time() - started:.1f} s ({jobs} jobs), '
        f'{len(missing)} without a texture{": " + str(missing[:12]) if missing else ""}'
        f'{", errors " + str(errors) if errors else ""}')
    return ATLAS_WIDTH, height, bytes(atlas), placed


# ---- Font -----------------------------------------------------------------------------------------

def build_font(root: Path, lang: str, items: list, pool) -> tuple:
    path = root / 'menu' / lang / 'font.gfx'
    chars = {chr(c) for c in range(32, 127)} | set('×…–—’‘“”•·°')
    for item in items:
        chars |= set(item['name'])
    font = gfxfont.font_read(path.read_bytes(), chars)
    return gfxfont.sdf_atlas(font, chars, pool=pool)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--game', type=Path, default=catalog.DEFAULT_GAME)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    parser.add_argument('--cell', type=int, default=128, help='icon size in the atlas (px)')
    parser.add_argument('--armor', action='store_true', help='also the armor pictures')
    parser.add_argument('--gestures', action='store_true', help='also the gesture pictures')
    parser.add_argument('--jobs', type=int, default=min(32, os.cpu_count() or 4))
    parser.add_argument('--lang', default='engus')
    parser.add_argument('--quiet', action='store_true')
    args = parser.parse_args()

    def log(*text):
        if not args.quiet:
            print(*text)

    root = catalog.game_root(args.game)
    out = args.out / 'ui'
    out.mkdir(parents=True, exist_ok=True)
    items = collect_items(root, args.lang, args.armor, args.gestures)
    merged = merge_catalog(items, args.out / 'items' / 'items.json')
    if merged:
        log(f'items catalog (doc 17): use and effect for {merged} goods')
    icons = sorted({item['icon'] for item in items})
    log(f'items: {len(items)} ({sum(i["kind"] == "goods" for i in items)} goods, '
        f'{sum(i["kind"] == "weapon" for i in items)} weapons, '
        f'{sum(i["kind"] == "armor" for i in items)} armor), {len(icons)} pictures')
    width, height, atlas, placed = build_atlas(root, icons, args.cell, args.jobs, log)
    tpf.write_png(out / 'icons.png', width, height, atlas)
    for item in items:
        item['has_icon'] = item['icon'] in placed
    doc = {'source': str(args.game), 'cell': args.cell, 'width': width, 'height': height,
           'icons': {str(k): v for k, v in sorted(placed.items())}, 'items': items}
    (out / 'icons.json').write_text(json.dumps(doc, indent=1, ensure_ascii=False) + '\n',
                                    encoding='utf-8', newline='\n')
    log(f'{out / "icons.png"}: {width}x{height}, {len(placed)} icons of {args.cell} px')
    started = time.time()
    with Pool(max(1, args.jobs)) as pool:
        fw, fh, fpx, metrics = build_font(root, args.lang, items, pool)
    tpf.write_png(out / 'font.png', fw, fh, fpx, channels=1)
    metrics['width'], metrics['height'] = fw, fh
    metrics['glyphs'] = {str(k): v for k, v in sorted(metrics['glyphs'].items())}
    (out / 'font.json').write_text(json.dumps(metrics, indent=1, ensure_ascii=False) + '\n',
                                   encoding='utf-8', newline='\n')
    log(f'{out / "font.png"}: {fw}x{fh}, {len(metrics["glyphs"])} glyphs of "{metrics["font"]}" '
        f'({time.time() - started:.1f} s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
