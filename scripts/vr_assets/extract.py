#!/usr/bin/env python3
"""Extracts the hunter body and weapons from the user's own Bloodborne dump into the sandbox's
model format (bbmodel.py) under out/vr_assets/ (git-ignored; never commit game assets).

    python scripts/vr_assets/extract.py [--game C:/games/CUSA03173] [--out out/vr_assets]
        [--outfit hunter|foreign|bare|ID,ID,ID,ID] [--gender m|f]
        [--weapon NAME_OR_MODEL ...] [--no-preview] [--quiet]
    python scripts/vr_assets/extract.py --throwables [--sfx-mesh ID ...]   (hand-thrown goods)
    python scripts/vr_assets/extract.py --consumables                      (item wheel meshes)

The dump is only read. Writes:
    hunter.json/.bin   the body: c0000's skeleton (+ face/cloth bones from the parts), its dummy
                       points, and the meshes of the bare body (fc_<g>_0000, which holds the skin
                       of hands, arms and head) plus the four armor parts of the outfit; each
                       mesh keeps its model-mask id (#NN# in the material name) and meshes the
                       outfit masks (EquipParamProtector invisibleFlagNN) are flagged hidden
    wp_a_XXXX.json/.bin  each weapon in its own model space (grip at the origin, blade along -Y)
    assets.json        index for the sandbox: body file, weapons with names and ids
    catalog.json       every weapon/armor name -> part file (catalog.py)
    preview/*.png      wireframes of the skeleton, dummies and weapon (preview.py), unless
                       --no-preview
It also prints the skeleton (arm, hand and finger chains) and dummy counts.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bbmodel  # noqa: E402
import catalog  # noqa: E402
import souls  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / 'out' / 'vr_assets'

# EquipParamProtector ids (head, body, arm, leg).
OUTFITS = {
    'hunter': (70000, 71000, 72000, 73000),     # Hunter Hat/Garb/Gloves/Trousers (*_2200)
    'foreign': (230000, 231000, 232000, 233000),  # starting set: Black Hood, Foreign Garb,
                                                  # Sullied Bandage, Foreign Trousers (*_8000)
    'bare': (900000, 901000, 902000, 903000),    # nothing equipped: only the body skin
}
# Weapons extracted by default (EquipParamWeapon base ids).
DEFAULT_WEAPONS = ('Saw Cleaver', 'Hunter Axe', 'Threaded Cane', 'Kirkhammer', 'Saw Spear',
                   'Hunter Pistol', 'Hunter Blunderbuss', 'Torch')
SLOTS = ('head', 'body', 'arm', 'leg')
# Bones the sandbox animates or reads; printed to check the skeleton.
CHAINS = {
    'spine': ('Master', 'Root', 'Pelvis', 'Spine', 'Spine1', 'Spine2', 'Neck', 'Head'),
    'left arm': ('L_Clavicle', 'L_UpperArm', 'L_Forearm', 'L_Hand', 'L_Weapon'),
    'right arm': ('R_Clavicle', 'R_UpperArm', 'R_Forearm', 'R_Hand', 'R_Weapon'),
    'left fingers': tuple(f'L_Finger{f}{s}' for f in range(5) for s in ('', '1', '2')),
    'right fingers': tuple(f'R_Finger{f}{s}' for f in range(5) for s in ('', '1', '2')),
}


def finger_name(name: str) -> str:
    # Fingers are named L_Finger0, L_Finger01, L_Finger02 (thumb), L_Finger1, L_Finger11, ...
    return name


def log(quiet: bool, *args):
    if not quiet:
        print(*args)


def first_flver(files: list, stem: str | None = None):
    """The archive's main FLVER (the one named like the archive when given)."""
    flvers = [f for f in files if f.basename.lower().endswith('.flver')]
    if stem:
        for f in flvers:
            if f.basename.lower() == f'{stem.lower()}.flver':
                return f
    return flvers[0] if flvers else None


def mask_of(material_name: str) -> int:
    """The model-mask id in a material name ("#44#" -> 44), -1 when none."""
    parts = material_name.split('#')
    if len(parts) >= 3 and parts[1].isdigit():
        return int(parts[1])
    return -1


class Skeleton:
    """c0000's bones, extended by name with bones the parts use that c0000 lacks."""

    def __init__(self, flver: souls.Flver):
        self.bones = []      # (name, parent index, bind world row-major LH)
        self.index = {}
        worlds = souls.bone_worlds(flver.bones)
        for i, b in enumerate(flver.bones):
            self.index.setdefault(b.name, i)
            self.bones.append((b.name, b.parent, worlds[i]))
        self.base_count = len(self.bones)

    def resolve(self, part: souls.Flver, part_worlds: list, part_bone: int) -> int:
        """The skeleton index of a part's bone, appending it (and missing parents) if needed."""
        name = part.bones[part_bone].name
        if name in self.index:
            return self.index[name]
        parent = part.bones[part_bone].parent
        parent_index = (self.resolve(part, part_worlds, parent)
                        if 0 <= parent < len(part.bones) else -1)
        self.index[name] = len(self.bones)
        self.bones.append((name, parent_index, part_worlds[part_bone]))
        return self.index[name]


def convert_mesh(flver: souls.Flver, mesh: souls.Mesh, bone_map) -> tuple[list, list]:
    """(vertices, triangles) of a mesh's LOD0 face set, mirrored to the right-handed space and
    with bone indices mapped through bone_map(part bone index) -> output bone index."""
    vertices = []
    for i, pos in enumerate(mesh.positions):
        ids = mesh.bone_ids[i]
        ws = mesh.bone_weights[i]
        if not mesh.dynamic:
            # Static meshes follow one bone: normal.w (into the mesh bone list when it has one).
            ids = (mesh.normal_w[i], 0, 0, 0)
            ws = (1.0, 0.0, 0.0, 0.0)
        out_ids = []
        for b, w in zip(ids, ws):
            part_bone = mesh.bone_indices[b] if mesh.bone_indices and b < len(mesh.bone_indices) \
                else b
            out_ids.append(bone_map(part_bone) if w > 0.0 else 0)
        total = sum(ws) or 1.0
        weights = tuple(w / total for w in ws)
        n = mesh.normals[i]
        vertices.append((bbmodel.mirror(pos), bbmodel.mirror(n), tuple(mesh.uvs[i]),
                         tuple(out_ids), weights))
    face_set = next((fs for fs in mesh.face_sets if fs.flags == 0), None)
    if face_set is None and mesh.face_sets:
        face_set = min(mesh.face_sets, key=lambda fs: fs.flags)
    tris = face_set.triangles() if face_set else []
    flipped = []
    for k in range(0, len(tris) - 2, 3):
        a, b, c = tris[k], tris[k + 1], tris[k + 2]
        flipped.extend((a, c, b))  # mirroring X reverses the winding
    return vertices, flipped


def dominant_bone(vertices: list) -> int:
    weight = {}
    for _p, _n, _uv, ids, ws in vertices:
        for b, w in zip(ids, ws):
            weight[b] = weight.get(b, 0.0) + w
    return max(weight, key=weight.get) if weight else -1


def dummy_dict(d: souls.Dummy, worlds: list, bone_map) -> dict:
    pos, fwd, up = souls.dummy_model(d, worlds)
    return {'ref': d.reference_id,
            'parent': bone_map(d.parent_bone) if d.parent_bone >= 0 else -1,
            'attach': bone_map(d.attach_bone) if d.attach_bone >= 0 else -1,
            'position': bbmodel.mirror(pos), 'forward': bbmodel.mirror(fwd),
            'upward': bbmodel.mirror(up), 'use_upward': d.use_upward}


def extract_body(root: Path, protector_ids: tuple, gender: str, name: str, out: Path,
                 quiet: bool = False) -> tuple[Path, dict]:
    """The hunter: c0000 skeleton and dummies, bare body skin and the outfit's four parts."""
    chr_files = souls.bnd4_read((root / 'chr' / 'c0000.chrbnd.dcx').read_bytes())
    skel_flver = souls.flver_read(first_flver(chr_files, 'c0000').data, decode_vertices=False)
    skeleton = Skeleton(skel_flver)
    params, defs = catalog.load_params(root)
    rows = {r['id']: r for r in souls.param_table(params, defs, 'EquipParamProtector')}
    _wnames, pnames = catalog.load_names(root)
    hidden_masks = set()
    parts = [(f'fc_{gender}_0000', 'skin', None)]
    for slot, pid in zip(SLOTS, protector_ids):
        row = rows[pid]
        hidden_masks |= {int(k[len('invisibleFlag'):]) for k, v in row.items()
                         if k.startswith('invisibleFlag') and v}
        stem = catalog.resolve_part(root / 'parts', catalog.SLOT_PREFIX[slot],
                                    row['equipModelId'], gender)
        parts.append((stem, slot, pid))
    model = bbmodel.Model(name)
    used_parts = []
    for stem, slot, pid in parts:
        if stem is None:
            continue
        files = souls.bnd4_read((root / 'parts' / f'{stem}.partsbnd.dcx').read_bytes())
        main = first_flver(files, stem)
        if main is None:
            continue
        flver = souls.flver_read(main.data)
        worlds = souls.bone_worlds(flver.bones)
        cache = {}

        def bone_map(b, flver=flver, worlds=worlds, cache=cache):
            if b not in cache:
                cache[b] = skeleton.resolve(flver, worlds, b) if 0 <= b < len(flver.bones) else 0
            return cache[b]

        used_parts.append({'part': stem, 'slot': slot, 'protector': pid,
                           'name': pnames.get(pid, '') if pid else 'body skin',
                           'meshes': len(flver.meshes)})
        for mi, mesh in enumerate(flver.meshes):
            vertices, tris = convert_mesh(flver, mesh, bone_map)
            if not tris:
                continue
            material = flver.materials[mesh.material] if mesh.material < len(
                flver.materials) else None
            mask = mask_of(material.name) if material else -1
            dom = dominant_bone(vertices)
            model.add_mesh({'name': f'{stem}/{mi}', 'part': stem, 'slot': slot,
                            'material': material.name if material else '',
                            'mtd': material.mtd.replace('\\', '/').rsplit('/', 1)[-1]
                            if material else '',
                            'mask': mask, 'hidden': mask >= 0 and mask in hidden_masks,
                            'dominant_bone': dom}, vertices, tris)
    # Bones (after the parts added theirs) and the skeleton's dummies.
    for i, (bone_name, parent, world) in enumerate(skeleton.bones):
        model.bones.append({'name': bone_name, 'parent': parent,
                            'bind_world': [round(v, 7) for v in bbmodel.mirror_matrix(world)],
                            'deform': parent >= 0 or bone_name == 'Master'})
    skel_worlds = [w for _n, _p, w in skeleton.bones]
    for d in skel_flver.dummies:
        model.dummies.append(dummy_dict(d, skel_worlds, lambda b: b))
    for m in model.meshes:
        m['dominant_bone_name'] = model.bones[m['dominant_bone']]['name'] \
            if m['dominant_bone'] >= 0 else ''
    model.extra = {'kind': 'body', 'gender': gender, 'outfit': list(protector_ids),
                   'parts': used_parts, 'hidden_masks': sorted(hidden_masks),
                   'skeleton_bones': skeleton.base_count,
                   'source': 'chr/c0000.chrbnd.dcx + ' + ', '.join(
                       f'parts/{p["part"]}.partsbnd.dcx' for p in used_parts)}
    path = model.write(out)
    return path, model.extra


def extract_weapon(root: Path, stem: str, info: dict, out: Path) -> Path:
    files = souls.bnd4_read((root / 'parts' / f'{stem}.partsbnd.dcx').read_bytes())
    main = first_flver(files, stem)
    flver = souls.flver_read(main.data)
    worlds = souls.bone_worlds(flver.bones)
    model = bbmodel.Model(stem)
    for i, b in enumerate(flver.bones):
        model.bones.append({'name': b.name, 'parent': b.parent,
                            'bind_world': [round(v, 7) for v in bbmodel.mirror_matrix(worlds[i])],
                            'deform': True})
    for mi, mesh in enumerate(flver.meshes):
        vertices, tris = convert_mesh(flver, mesh, lambda b: b)
        if not tris:
            continue
        material = flver.materials[mesh.material] if mesh.material < len(flver.materials) \
            else None
        model.add_mesh({'name': f'{stem}/{mi}', 'part': stem, 'slot': 'weapon',
                        'material': material.name if material else '',
                        'mask': -1, 'hidden': False, 'dominant_bone': dominant_bone(vertices)},
                       vertices, tris)
    for d in flver.dummies:
        model.dummies.append(dummy_dict(d, worlds, lambda b: b))
    model.extra = {'kind': 'weapon', 'weapon': info,
                   'source': f'parts/{stem}.partsbnd.dcx ({main.basename})',
                   'other_flvers': [f.basename for f in files
                                    if f.basename.lower().endswith('.flver') and f is not main]}
    return model.write(out)


def pick_weapons(cat: dict, wanted: list) -> list:
    """Catalog rows for names ("Saw Cleaver") or model stems ("wp_a_0800"), base versions."""
    out = []
    for want in wanted:
        low = want.lower()
        rows = [w for w in cat['weapons'] if w['model'] and
                (w['name'].lower() == low or w['model'] == low)]
        if not rows:
            rows = [w for w in cat['weapons'] if w['model'] and low in w['name'].lower()
                    and '+' not in w['name']]
        if not rows:
            print(f'warning: no weapon matches {want!r}')
            continue
        rows.sort(key=lambda w: w['id'])
        out.append(rows[0])
    unique = {}
    for w in out:
        unique.setdefault(w['model'], w)
    return list(unique.values())


def print_skeleton(doc: dict):
    names = {b['name']: i for i, b in enumerate(doc['bones'])}
    print(f'skeleton: {len(doc["bones"])} bones ({doc["skeleton_bones"]} from c0000, the rest '
          f'from the parts), {len(doc["dummies"])} dummies')
    for chain, bone_names in CHAINS.items():
        found = []
        for n in bone_names:
            i = names.get(n)
            if i is None:
                found.append(f'{n}=MISSING')
                continue
            t = doc['bones'][i]['bind_world'][12:15]
            found.append(f'{n}#{i}({t[0]:+.2f},{t[1]:.2f},{t[2]:+.2f})')
        print(f'  {chain}: ' + ' '.join(found))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--game', type=Path, default=catalog.DEFAULT_GAME)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    parser.add_argument('--outfit', default='hunter',
                        help='hunter, foreign, bare, or four EquipParamProtector ids head,body,arm,leg')
    parser.add_argument('--gender', choices=('m', 'f'), default='m')
    parser.add_argument('--weapon', action='append', default=[],
                        help='weapon name or wp_a_XXXX (repeatable; default: a starter set)')
    parser.add_argument('--throwables', action='store_true',
                        help='only export the hand-thrown goods: out/vr_assets/throwables/ '
                             '(throwables_extract.py)')
    parser.add_argument('--consumables', action='store_true',
                        help='only export the consumable meshes: out/vr_assets/consumables/ '
                             '(consumables_extract.py)')
    parser.add_argument('--sfx-mesh', type=int, action='append', default=[],
                        help='also export this flight-effect mesh s#####.flver (repeatable; '
                             'implies --throwables)')
    parser.add_argument('--no-preview', action='store_true')
    parser.add_argument('--quiet', action='store_true')
    args = parser.parse_args()
    root = catalog.game_root(args.game)
    if not (root / 'chr' / 'c0000.chrbnd.dcx').exists():
        print(f'{root}: no chr/c0000.chrbnd.dcx (is --game the CUSA03173 dump?)')
        return 1
    if args.outfit in OUTFITS:
        outfit = OUTFITS[args.outfit]
    else:
        outfit = tuple(int(x) for x in args.outfit.split(','))
        if len(outfit) != 4:
            parser.error('--outfit needs four ids: head,body,arm,leg')
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    if args.consumables:
        import consumables_extract  # noqa: PLC0415
        consumables_extract.export(root, out, args.quiet)
        return 0
    if args.throwables or args.sfx_mesh:
        import throwables_extract  # noqa: PLC0415
        throwables_extract.export(root, out, args.sfx_mesh, args.quiet)
        return 0

    cat = catalog.build(args.game)
    (out / 'catalog.json').write_text(json.dumps(cat, indent=1, ensure_ascii=False),
                                      encoding='utf-8')
    body_path, body_info = extract_body(root, outfit, args.gender, 'hunter', out, args.quiet)
    doc, _blob = bbmodel.load(body_path)
    print(f'{body_path}: {doc["vertex_count"]} vertices, {doc["index_count"] // 3} triangles, '
          f'{len(doc["meshes"])} meshes ({sum(1 for m in doc["meshes"] if m["hidden"])} masked '
          f'by the outfit)')
    for p in body_info['parts']:
        print(f'  part {p["part"]:10s} {p["slot"]:4s} {p["name"]}')
    print_skeleton(doc)

    weapons = pick_weapons(cat, args.weapon or list(DEFAULT_WEAPONS))
    index = {'format': 'bbassets', 'version': 1, 'game': str(args.game),
             'body': body_path.name, 'outfit': args.outfit, 'gender': args.gender,
             'weapons': []}
    for w in weapons:
        path = extract_weapon(root, w['model'], w, out)
        wdoc, _ = bbmodel.load(path)
        refs = sorted({d['ref'] for d in wdoc['dummies']})
        print(f'{path}: {w["name"]} (id {w["id"]}), {wdoc["vertex_count"]} vertices, '
              f'{len(wdoc["bones"])} bones, dummies {refs}')
        index['weapons'].append({'file': path.name, 'name': w['name'], 'id': w['id'],
                                 'model': w['model'], 'dummies': refs})
    (out / 'assets.json').write_text(json.dumps(index, indent=1), encoding='utf-8')
    print(f'{out / "assets.json"}: body + {len(weapons)} weapons')
    if not args.no_preview:
        import preview  # noqa: PLC0415
        for png in preview.render_all(out):
            print(png)
    return 0


if __name__ == '__main__':
    sys.exit(main())
