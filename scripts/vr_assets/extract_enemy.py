#!/usr/bin/env python3
"""Extracts an enemy character from the user's own Bloodborne dump into the sandbox's model format
(bbmodel.py), as the training target of sandbox3d's weapon collision (gpu/xr_bridge/sandbox3d,
physics_weapon.h). Game assets go to out/vr_assets/ (git-ignored; never commit them).

    python scripts/vr_assets/extract_enemy.py [--game C:/games/CUSA03173] [--out out/vr_assets]
        [--chr c2632] [--npc 263203] [--quiet]

Default: c2632, the large Huntsman of Central Yharnam ("群衆A（大）", torch and axe), NpcParam row
263203 (clothes pattern 1, the early-game Central Yharnam placement). Its model holds every
variant (clothes, torch, axe, saw, pitchfork ...) behind model-mask ids (#NN# in the material
name); the row's NpcParam modelDispMask0..31 picks the ones shown, the others are flagged hidden.

The dump is only read. Writes enemy_<chr>.json/.bin: the FLVER's bones (bind pose, X mirrored like
the hunter), dummies and meshes. The sandbox poses the enemy at its bind pose and puts the
hurtboxes of hitbox/chr_<chr>.json (scripts/vr_assets/hitbox_extract.py) on its bones.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bbmodel  # noqa: E402
import catalog  # noqa: E402
import extract  # noqa: E402
import souls  # noqa: E402

DEFAULT_CHR = 'c2632'
DEFAULT_NPC = 263203


def display_masks(root: Path, chr_id: str, npc_id: int | None) -> tuple[int | None, set | None]:
    """(NpcParam row id, model-mask ids it shows) for the character; the row `npc_id` when it
    belongs to the character, else its first row. (None, None) when it has no row."""
    params, defs = catalog.load_params(root)
    number = int(chr_id.lstrip('c'))
    rows = [r for r in souls.param_table(params, defs, 'NpcParam') if r['id'] // 100 == number]
    if not rows:
        return None, None
    row = next((r for r in rows if r['id'] == npc_id), rows[0])
    masks = {int(k[len('modelDispMask'):]) for k, v in row.items()
             if k.startswith('modelDispMask') and v}
    return row['id'], masks


def extract_enemy(root: Path, chr_id: str, npc_id: int | None, out: Path) -> tuple[Path, dict]:
    files = souls.bnd4_read((root / 'chr' / f'{chr_id}.chrbnd.dcx').read_bytes())
    main = extract.first_flver(files, chr_id)
    if main is None:
        raise SystemExit(f'chr/{chr_id}.chrbnd.dcx has no FLVER')
    flver = souls.flver_read(main.data)
    worlds = souls.bone_worlds(flver.bones)
    row_id, masks = display_masks(root, chr_id, npc_id)
    model = bbmodel.Model(f'enemy_{chr_id}')
    for i, b in enumerate(flver.bones):
        model.bones.append({'name': b.name, 'parent': b.parent,
                            'bind_world': [round(v, 7) for v in bbmodel.mirror_matrix(worlds[i])],
                            'deform': True})
    shown = hidden = 0
    for mi, mesh in enumerate(flver.meshes):
        vertices, tris = extract.convert_mesh(flver, mesh, lambda b: b)
        if not tris:
            continue
        material = flver.materials[mesh.material] if mesh.material < len(flver.materials) \
            else None
        mask = extract.mask_of(material.name) if material else -1
        is_hidden = mask >= 0 and masks is not None and mask not in masks
        hidden += is_hidden
        shown += not is_hidden
        dom = extract.dominant_bone(vertices)
        model.add_mesh({'name': f'{chr_id}/{mi}', 'part': chr_id, 'slot': 'enemy',
                        'material': material.name if material else '',
                        'mask': mask, 'hidden': is_hidden, 'dominant_bone': dom,
                        'dominant_bone_name': model.bones[dom]['name'] if dom >= 0 else ''},
                       vertices, tris)
    for d in flver.dummies:
        model.dummies.append(extract.dummy_dict(d, worlds, lambda b: b))
    model.extra = {'kind': 'enemy', 'chr': chr_id, 'npc_row': row_id,
                   'display_masks': sorted(masks) if masks is not None else None,
                   'source': f'chr/{chr_id}.chrbnd.dcx ({main.basename})'}
    path = model.write(out)
    return path, {'shown': shown, 'hidden': hidden, 'row': row_id, 'bones': len(flver.bones)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--game', type=Path, default=catalog.DEFAULT_GAME)
    parser.add_argument('--out', type=Path, default=extract.DEFAULT_OUT)
    parser.add_argument('--chr', default=DEFAULT_CHR, help='character id, e.g. c2632')
    parser.add_argument('--npc', type=int, default=DEFAULT_NPC,
                        help='NpcParam row whose display masks pick the meshes')
    parser.add_argument('--quiet', action='store_true')
    args = parser.parse_args()
    root = catalog.game_root(args.game)
    if not (root / 'chr' / f'{args.chr}.chrbnd.dcx').exists():
        print(f'{root}: no chr/{args.chr}.chrbnd.dcx (is --game the CUSA03173 dump?)')
        return 1
    path, info = extract_enemy(root, args.chr, args.npc, args.out)
    if not args.quiet:
        doc, _blob = bbmodel.load(path)
        print(f'{path}: {doc["vertex_count"]} vertices, {doc["index_count"] // 3} triangles, '
              f'{info["bones"]} bones, {info["shown"]} meshes shown and {info["hidden"]} hidden '
              f'by NpcParam {info["row"]} (masks {doc["display_masks"]})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
