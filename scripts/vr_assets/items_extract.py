"""A 3D model for every goods id the game has a model for (docs/vr/findings/39-physical-items-plan.md).
Used by extract.py (--items); the dump is only read.

    python scripts/vr_assets/extract.py --items [--game C:/games/CUSA03173]

Writes under out/vr_assets/items/:
    <goods id>.json/.bin   the mesh in the bbmodel format (bbmodel.py), recentred on its bounding box,
                           in metres (the same format and conventions as throwables/th_*.json)
    items.json             the index: one row per goods id with name, group, mesh file or null, icon
                           reference, bounding box size in metres, a suggested grip category and, for
                           a mesh, where it came from and how sure the match is

EquipParamGoods.modelId is 0 for every row of the game: no item has a model id. The 3D data that does
exist for items is (a) the flight / use effect meshes of sfx/frpg_sfxbnd_{commoneffects,dlceffects}
(s#####.flver, picked by material name and by the effects that use them: docs 35 and 36), and (b) the
pickup props of obj/o2100xx.objbnd.dcx and a few more. Each goods id is matched to one of those by
ITEM_MESH below (the sandbox's tables SFX_MESH / CONSUMABLE_MESH are included verbatim, "sandbox");
"guess" means the shape and the material name fit but nothing in the data links the two. Goods without
a model in the game use their menu icon (ui/icons.json) as a textured card: "icon" in the index.

items.json also keeps whatever the doc-17 catalogue (older items.json, consumables.cpp and
ui_extract.merge_catalog read it) had: the rows' existing keys are never touched, the mesh/icon/grip
keys are added next to them, and the original catalogue is copied once to items.catalog17.json.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

import bbmodel
import catalog
import consumables_extract as cx
import item_tex
import souls
import throwables_extract as tx

FMG_GOODS = 10
INDEX_MARK = 'bbitems'

# goods id -> (source, how, confidence). source: ('sfx', bundle, mesh id) or ('obj', object id).
ITEM_MESH = {
    # Potion bottles of the sfx bundle: s10000 plain flask, s10001 flask with a sash (both material
    # "m_kaifuku", i.e. "recovery"); not used by any sandbox row.
    1100: (('sfx', 'commoneffects', 10000), 'material m_kaifuku (recovery) flask', 'guess'),
    1101: (('sfx', 'commoneffects', 10000), 'material m_kaifuku (recovery) flask', 'guess'),
    1120: (('sfx', 'commoneffects', 10001), 'material m_kaifuku (recovery) flask with a sash', 'guess'),
    # The third "ariannas_blood" mesh (a 23 cm test tube): the other blood samples.
    701: (('sfx', 'commoneffects', 10019), 'material ariannas_blood tube', 'guess'),
    702: (('sfx', 'commoneffects', 10019), 'material ariannas_blood tube', 'guess'),
    705: (('sfx', 'commoneffects', 10019), 'material ariannas_blood tube', 'guess'),
    1411: (('sfx', 'commoneffects', 10024), 'material "telescop"', 'name'),
    1412: (('sfx', 'commoneffects', 10021), 'material "lamp_001" (a hand lantern)', 'name'),
    700: (('sfx', 'commoneffects', 11012), 'a box with a crank and four feet (music box)', 'shape'),
    2140: (('sfx', 'dlceffects', 11050), 'a small whistle with a ring (DLC effect s11050)', 'shape'),
    2060: (('sfx', 'commoneffects', 11015), 'a 51 cm bone', 'shape'),
    2090: (('sfx', 'commoneffects', 11015), 'a 51 cm bone', 'guess'),
    4014: (('obj', 210007), 'obj o210007: a skull, material "bodygem"', 'shape'),
    4312: (('obj', 289930), 'obj o289930: hand bell (materials c1050_Bell_*)', 'name'),
    4301: (('sfx', 'commoneffects', 5520), 'a tear-drop crystal', 'shape'),
    3000: (('sfx', 'commoneffects', 801), 'a faceted crystal shard', 'guess'),
    3010: (('sfx', 'commoneffects', 801), 'a faceted crystal shard', 'guess'),
    3020: (('sfx', 'commoneffects', 801), 'a faceted crystal shard', 'guess'),
    3030: (('obj', 210004), 'obj o210004: a rough rock', 'guess'),
}
# Chalices (goodsType 6): the pickup goblets of obj/o21000x. Root chalices get the ornate one.
CHALICE_PLAIN = (('obj', 210002), 'obj o210002: goblet (the chalice pickup props o210001-3)', 'guess')
CHALICE_ROOT = (('obj', 210006), 'obj o210006: ornate chalice with roots', 'guess')

# goods id -> grip category when the default of the row's group is not right.
GRIP = {
    **{g: 'bottle' for g in (1000, 703, 706, 2030, 1100, 1101, 1120, 701, 702, 705, 1200, 1201, 1250, 1251,
                             1260, 1270, 2130, 1220)},
    **{g: 'bell' for g in (200, 201, 205, 206, 225, 226, 2050, 4312)},
    **{g: 'weapon' for g in (1210, 1240, 2090)},
    **{g: 'paper' for g in (1300, 1310, 1320, 1330, 120, 130)},
    **{g: 'tool' for g in (700, 1411, 1412, 2060, 2140, 2000, 2010, 2020, 2070, 2080, 2110, 2120, 131)},
}
THROWABLE_GOODS = set(tx.GOODS)

# Targets of the suggested grip category: the longest side (m) a hand holds comfortably. The bridge
# scales a mesh down to the cap and up to half the cap (vr_held_mesh.h ItemsIndex).
GRIP_SIZE = {'bottle': 0.22, 'small': 0.10, 'paper': 0.18, 'bell': 0.16, 'tool': 0.20, 'weapon': 0.45,
             'none': 0.10}


def group_of(row: dict, name: str) -> str:
    gid = row['id']
    icon = row.get('iconId') or 0
    gt = row.get('goodsType') or 0
    if icon >= 5000:
        return 'gesture'
    if gid in (200, 201, 205, 206, 225, 226, 2050, 4312, 4003, 4308, 111):
        return 'bell/multiplayer'
    if gid in (900, 901, 910):
        return 'ammo'
    if gid in THROWABLE_GOODS:
        return 'tool/throwable'
    if 1600 <= gid <= 1612:
        return 'rune'
    if 1650 <= gid <= 1684:
        return 'gem'
    if gt == 6:
        return 'chalice'
    if gt == 2:
        return 'material/blood echo'
    if gid in (1500, 1501) or 1510 <= gid <= 1594:
        return 'material/blood echo'
    if gt == 1 or 4000 <= gid < 5000:
        return 'key item'
    if 2000 <= gid < 2200 or 1411 <= gid <= 1412 or gid in (700, 100, 120, 130, 131):
        return 'tool/throwable'
    if row.get('isConsume') or 1000 <= gid < 1500 or 700 <= gid < 710:
        return 'consumable'
    return 'consumable' if row.get('refCategory') in (1, 2) else 'key item'


def grip_of(gid: int, group: str, name: str) -> str:
    if gid in GRIP:
        return GRIP[gid]
    if 'Paper' in name:
        return 'paper'
    if group in ('rune', 'gem', 'material/blood echo', 'key item', 'ammo', 'consumable'):
        return 'small'
    if group in ('chalice', 'tool/throwable'):
        return 'tool'
    if group == 'bell/multiplayer':
        return 'bell'
    return 'small'


def mesh_source(gid: int, group: str, name: str):
    """(source, how, confidence) of a goods id, None when the game has no model for it."""
    if gid in tx.SFX_MESH and tx.SFX_MESH[gid][1] is not None:
        _n, mesh, how = tx.SFX_MESH[gid]
        return ('sfx', 'commoneffects', mesh), how, 'sandbox'
    if gid in cx.CONSUMABLE_MESH:
        _n, mesh, how = cx.CONSUMABLE_MESH[gid]
        return ('sfx', 'commoneffects', mesh), how, 'sandbox'
    if gid in ITEM_MESH:
        return ITEM_MESH[gid]
    if group == 'chalice':
        return CHALICE_ROOT if 'Root' in name else CHALICE_PLAIN
    return None


class Sources:
    """The flver entries (sfx meshes, obj props) of the dump, opened once."""

    def __init__(self, root: Path):
        self.root = root
        self.sfx = {}
        self.sfx_tpf = {}
        self.obj = {}
        self.obj_tpf = {}

    def entry(self, source):
        if source[0] == 'sfx':
            bundle = source[1]
            if bundle not in self.sfx:
                files = souls.bnd4_read((self.root / 'sfx' / f'frpg_sfxbnd_{bundle}.ffxbnd.dcx').read_bytes())
                self.sfx_tpf[bundle] = item_tex.index_bundle(files)
                self.sfx[bundle] = {int(m.group(1)): f for f in files
                                    if (m := re.match(r's(\d+)\.flver$', f.basename))}
            return self.sfx[bundle].get(source[2])
        oid = source[1]
        if oid not in self.obj:
            path = self.root / 'obj' / f'o{oid:06d}.objbnd.dcx'
            files = souls.bnd4_read(path.read_bytes()) if path.is_file() else []
            self.obj_tpf[oid] = item_tex.index_bundle(files)
            self.obj[oid] = next((f for f in files if f.basename.lower() == f'o{oid:06d}.flver'), None)
        return self.obj[oid]

    def tpfs(self, source) -> dict:
        return self.sfx_tpf.get(source[1], {}) if source[0] == 'sfx' else self.obj_tpf.get(source[1], {})

    def label(self, source) -> str:
        if source[0] == 'sfx':
            return f'sfx/frpg_sfxbnd_{source[1]}.ffxbnd.dcx s{source[2]:05d}.flver'
        return f'obj/o{source[1]:06d}.objbnd.dcx o{source[1]:06d}.flver'


def goods_rows(root: Path) -> list:
    files = souls.bnd4_read((root / 'msg' / 'engus' / 'item.msgbnd.dcx').read_bytes())
    names = {f.id: souls.fmg_read(f.data) for f in files}.get(FMG_GOODS, {})
    params, defs = catalog.load_params(root)
    rows = []
    for row in souls.param_table(params, defs, 'EquipParamGoods'):
        name = names.get(row['id'], '')
        if name == '*':
            name = ''
        rows.append((row, name))
    return rows


def export(root: Path, out: Path, quiet: bool = False) -> list:
    items_dir = out / 'items'
    items_dir.mkdir(parents=True, exist_ok=True)
    icons = {}
    icons_path = out / 'ui' / 'icons.json'
    atlas = None
    if icons_path.is_file():
        atlas = json.loads(icons_path.read_text(encoding='utf-8'))
        icons = atlas.get('icons', {})
    rows = goods_rows(root)
    src = Sources(root)
    index = []
    named_icon_mesh = {}  # (name or icon) -> mesh record, so the unnamed duplicates (201, 206...) inherit
    for row, name in sorted(rows, key=lambda r: r[0]['id']):
        gid = row['id']
        icon = row.get('iconId') or 0
        if icon <= 0 and not name:
            continue  # placeholder / internal rows (1, 2, 999, 10900...): not an item
        group = group_of(row, name)
        rec = {'id': gid, 'name': name or f'(unnamed goods {gid})', 'group': group,
               'mesh': None, 'icon': None, 'size_m': None, 'grip': 'none' if group == 'gesture' else
               grip_of(gid, group, name), 'iconId': icon}
        if str(icon) in icons:
            x, y = icons[str(icon)]
            rec['icon'] = {'atlas': 'ui/icons.png', 'x': x, 'y': y, 'size': atlas.get('cell', 128),
                           'width': atlas.get('width'), 'height': atlas.get('height'), 'iconId': icon}
        hit = mesh_source(gid, group, name)
        if hit is None and icon in named_icon_mesh:
            hit = named_icon_mesh[icon]  # same picture, another row (the unnamed bell variants)
        if hit is not None:
            source, how, confidence = hit
            entry = src.entry(source)
            if entry is None:
                if not quiet:
                    print(f'{gid}: {rec["name"]}: {src.label(source)} not in the dump')
            else:
                try:
                    out_model = tx.convert_sfx_mesh(entry, str(gid), tex=(out, src.tpfs(source)))
                except ValueError:  # an empty mesh
                    out_model = None
                if out_model is not None:
                    out_model.extra.update(goods=gid, name=rec['name'], source=src.label(source),
                                           how=how, confidence=confidence)
                    out_model.extra['kind'] = 'item'
                    path = out_model.write(items_dir)
                    size = out_model.extra['size']
                    rec.update(mesh=path.name, size_m=[round(v, 4) for v in size],
                               source=src.label(source), how=how, confidence=confidence)
                    if icon:
                        named_icon_mesh.setdefault(icon, hit)
                    if not quiet:
                        print(f'{path.name}: {rec["name"]} <- {src.label(source)} '
                              f'({size[0]:.2f} x {size[1]:.2f} x {size[2]:.2f} m, {confidence})')
        index.append(rec)
    item_tex.flush(out)
    write_index(items_dir, index, root)
    return index


def write_index(items_dir: Path, index: list, root: Path):
    """items.json: the doc-17 catalogue (when there) with the new keys added, else just the rows."""
    path = items_dir / 'items.json'
    doc = {}
    if path.is_file():
        try:
            doc = json.loads(path.read_text(encoding='utf-8'))
        except ValueError:
            doc = {}
        if doc.get('format') != INDEX_MARK and doc:
            backup = items_dir / 'items.catalog17.json'
            if not backup.exists():
                backup.write_text(path.read_text(encoding='utf-8'), encoding='utf-8')
    rows = {r['id']: r for r in doc.get('items', [])}
    for rec in index:
        row = rows.get(rec['id'])
        if row is None:
            row = {'id': rec['id'], 'name': rec['name'], 'category': 'other'}
            rows[rec['id']] = row
        row.update({k: v for k, v in rec.items() if k not in ('id', 'name')})
    stats = coverage(index)
    doc.update({'format': INDEX_MARK, 'version': 1, 'game': str(root),
                'mesh_format': 'bbmodel (items/<id>.json + .bin), metres, recentred on the bounding box',
                'grip_size_m': GRIP_SIZE, 'coverage': stats,
                'items': [rows[k] for k in sorted(rows)]})
    path.write_text(json.dumps(doc, indent=1, ensure_ascii=False), encoding='utf-8')


def coverage(index: list) -> dict:
    """Totals: all item rows (gestures apart), with a game mesh, icon-card fallback only, neither."""
    out = {}
    for group in sorted({r['group'] for r in index}) + ['TOTAL (without gestures)']:
        rows = [r for r in index if (r['group'] == group if not group.startswith('TOTAL')
                                     else r['group'] != 'gesture')]
        mesh = sum(1 for r in rows if r['mesh'])
        icon = sum(1 for r in rows if not r['mesh'] and r['icon'])
        out[group] = {'total': len(rows), 'mesh': mesh, 'icon_card': icon,
                      'neither': len(rows) - mesh - icon}
    return out
