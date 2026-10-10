#!/usr/bin/env python3
"""Which part file holds which weapon or armor piece, from the game's own tables.

    python scripts/vr_assets/catalog.py [--game C:/games/CUSA03173] [--out out/vr_assets]
        [--find "saw cleaver"]

Reads (never writes) the dump: param/gameparam (EquipParamWeapon, EquipParamProtector),
paramdef, msg/engus/item.msgbnd (names). Writes out/vr_assets/catalog.json:
    weapons:    id, name, model (wp_a_XXXX), the equipModelId and weapon fields the sandbox uses
    protectors: id, name, slot (head/body/arm/leg), model (hd_m_XXXX ...), gender/category fields
Model naming: weapons wp_a_<equipModelId:04d>; armor <slot>_<m|f|a>_<equipModelId:04d> where
m/f is the gendered pair (equipModelGender) and a the unisex file (see resolve_part).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import souls  # noqa: E402

DEFAULT_GAME = Path('C:/games/CUSA03173')
REPO = Path(__file__).resolve().parents[2]

# FMG ids inside msg/<lang>/item.msgbnd.dcx (Bloodborne): weapon and armor names.
FMG_WEAPON_NAMES = 11
FMG_PROTECTOR_NAMES = 12
SLOT_PREFIX = {'head': 'hd', 'body': 'bd', 'arm': 'am', 'leg': 'lg'}


def game_root(path: Path) -> Path:
    """The dvdroot_ps4 folder of a dump (accepts the dump root or dvdroot_ps4 itself)."""
    path = Path(path)
    return path / 'dvdroot_ps4' if (path / 'dvdroot_ps4').is_dir() else path


def load_names(root: Path, lang: str = 'engus') -> tuple[dict, dict]:
    files = souls.bnd4_read((root / 'msg' / lang / 'item.msgbnd.dcx').read_bytes())
    by_id = {f.id: f for f in files}
    weapons = souls.fmg_read(by_id[FMG_WEAPON_NAMES].data)
    protectors = souls.fmg_read(by_id[FMG_PROTECTOR_NAMES].data)
    return weapons, protectors


def load_params(root: Path) -> tuple[list, list]:
    params = souls.bnd4_read((root / 'param/gameparam/gameparam.parambnd.dcx').read_bytes())
    defs = souls.bnd4_read((root / 'paramdef/paramdef.paramdefbnd.dcx').read_bytes())
    return params, defs


def resolve_part(parts_dir: Path, prefix: str, model_id: int, gender: str = 'm') -> str | None:
    """The existing part file stem for a model id: <prefix>_<gender>_<id>, else <prefix>_a_<id>."""
    for g in (gender, 'a', 'm', 'f'):
        stem = f'{prefix}_{g}_{model_id:04d}'
        if (parts_dir / f'{stem}.partsbnd.dcx').exists():
            return stem
    return None


def build(game: Path) -> dict:
    root = game_root(game)
    parts_dir = root / 'parts'
    weapon_names, protector_names = load_names(root)
    params, defs = load_params(root)
    weapons = []
    for row in souls.param_table(params, defs, 'EquipParamWeapon'):
        model = row.get('equipModelId')
        name = weapon_names.get(row['id'], '')
        if model is None:
            continue
        stem = resolve_part(parts_dir, 'wp', model, 'a')
        weapons.append({
            'id': row['id'], 'name': name, 'equipModelId': model, 'model': stem,
            'wepmotionCategory': row.get('wepmotionCategory'),
            'weaponCategory': row.get('weaponCategory'),
            'isLeftHand': row.get('isLeftHand') or row.get('leftHandEquipable'),
            'isRightHand': row.get('rightHandEquipable'),
        })
    protectors = []
    for row in souls.param_table(params, defs, 'EquipParamProtector'):
        slot = next((s for s in ('head', 'body', 'arm', 'leg') if row.get(f'{s}Equip')), None)
        model = row.get('equipModelId')
        if slot is None or model is None:
            continue
        gender_flag = row.get('equipModelGender')
        entry = {'id': row['id'], 'name': protector_names.get(row['id'], ''), 'slot': slot,
                 'equipModelId': model, 'equipModelGender': gender_flag,
                 'equipModelCategory': row.get('equipModelCategory')}
        for g in ('m', 'f'):
            entry[f'model_{g}'] = resolve_part(parts_dir, SLOT_PREFIX[slot], model, g)
        protectors.append(entry)
    return {'game': str(game), 'weapons': weapons, 'protectors': protectors}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--game', type=Path, default=DEFAULT_GAME)
    parser.add_argument('--out', type=Path, default=REPO / 'out' / 'vr_assets')
    parser.add_argument('--find', action='append', default=[],
                        help='print catalog rows whose name contains this (case-insensitive)')
    args = parser.parse_args()
    catalog = build(args.game)
    args.out.mkdir(parents=True, exist_ok=True)
    target = args.out / 'catalog.json'
    target.write_text(json.dumps(catalog, indent=1, ensure_ascii=False), encoding='utf-8')
    named_w = sum(1 for w in catalog['weapons'] if w['name'])
    print(f'{target}: {len(catalog["weapons"])} weapons ({named_w} named), '
          f'{len(catalog["protectors"])} armor pieces')
    for text in args.find:
        low = text.lower()
        for w in catalog['weapons']:
            if low in w['name'].lower() and '+' not in w['name']:
                print(f'  weapon {w["id"]:8d} {w["name"]:32s} model {w["model"]} '
                      f'(equipModelId {w["equipModelId"]})')
        for p in catalog['protectors']:
            if low in p['name'].lower():
                print(f'  {p["slot"]:4s}   {p["id"]:8d} {p["name"]:32s} model {p["model_m"]} / '
                      f'{p["model_f"]} (equipModelId {p["equipModelId"]})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
