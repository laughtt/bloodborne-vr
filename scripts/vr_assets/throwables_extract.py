"""Hand-thrown goods for the sandbox (docs/vr/findings/35-throwables.md): the Bullet table and the
flight meshes. Used by extract.py (--throwables, --sfx-mesh); the dump is only read.

    python scripts/vr_assets/extract.py --throwables [--sfx-mesh 10010 ...] [--game C:/games/CUSA03173]

Writes under out/vr_assets/throwables/:
    throwables.json    per goods id: the Bullet row (initial velocity, gravity in/out of range,
                       acceleration, max velocity, life, range, count, fan angle, penetrate, SFX
                       ids), read by sandbox3d's throwables (throwables.h) over its built-ins
    th_<goods>.json/.bin   the item's mesh in the bbmodel format (bbmodel.py), recentred on its
                       bounding box (the sandbox holds it by that centre)
    sfx_s<id>.json/.bin    any other flight-effect mesh asked for with --sfx-mesh

EquipParamGoods.modelId is 0 for every thrown item: the models are the flight effects' meshes
(sfx/frpg_sfxbnd_commoneffects.ffxbnd.dcx, s#####.flver), found through the f0000<sfxId>.fxr
files, which list the s##### ids they draw (SFX_MESH below says which one is the item).
"""
from __future__ import annotations

import json
import re
import struct
from pathlib import Path

import bbmodel
import item_tex
import souls

# goods id -> (name, [(sfx mesh id, how it was found)]); the first mesh is the item's.
#   "fxr":   the id appears in the item's own flight .fxr and in (almost) no other effect
#   "fxr-x": the .fxr of a sibling item (same bottle/knife) or a generic ring of effects
#   "name":  matched by the mesh's material name (e.g. "ariannas_blood", "coin", "Knife_03")
SFX_MESH = {
    1200: ('Molotov Cocktail', 10010, 'fxr f000020100 (fire pot, 4 effects)'),
    1201: ('Delayed Molotov', 10010, 'fxr-x f000020100 (no own flight .fxr for 20120)'),
    1210: ('Poison Knife', 10020, 'name Knife_03 (flight .fxr 20105 lists only generic meshes)'),
    1220: ('Oil Urn', 10014, 'fxr f000020101 (the only effects: 20001 and 20101)'),
    1230: ('Pebble', 10012, 'fxr f000020103 (stone; also the 250-257 throw effects)'),
    1240: ('Throwing Knife', 10020, 'name Knife_03 (flight .fxr 20104 lists only generic meshes)'),
    1250: ('Rope Molotov Cocktail', 10010, 'fxr-x f000020100'),
    1251: ('Delayed Rope Molotov', 10010, 'fxr-x f000020100'),
    1260: ('Pungent Blood Cocktail', 10015, 'fxr f000020107 (ariannas_blood vial)'),
    1270: ('Numbing Mist', 10009, 'fxr f000020108 (vial, material "hp")'),
    1410: ('Shining Coins', 10004, 'fxr f000020133 (material "coin", a flat disc)'),
    2070: ('Tiny Tonitrus', 11010, 'name thundermagic (only f000200007; the item is a stone)'),
    2080: ("Executioner's Gloves", 11021, 'fxr f000202008 (only that effect)'),
    2120: ("Blacksky Eye", None, 'none: f000202050 is not in the bundle'),
    2130: ('Accursed Brew', 10023, 'name Medicine_of_lead flask (f000202051 is not in the bundle)'),
}
GOODS = tuple(SFX_MESH)
BULLET_FIELDS = ('sfxId_Bullet', 'sfxId_Hit', 'life', 'dist', 'initVellocity', 'gravityInRange',
                 'gravityOutRange', 'accelInRange', 'maxVellocity', 'numShoot', 'shootAngle',
                 'isPenetrate', 'launchConditionType', 'atkId_Bullet', 'HitBulletID',
                 'isAttackSFX')


def _sfx_entries(root: Path):
    data = souls.bnd4_read((root / 'sfx' / 'frpg_sfxbnd_commoneffects.ffxbnd.dcx').read_bytes())
    meshes = {}
    fxrs = {}
    for e in data:
        m = re.match(r's(\d+)\.flver$', e.basename)
        if m:
            meshes[int(m.group(1))] = e
        m = re.match(r'f0*(\d+)\.fxr$', e.basename)
        if m:
            fxrs[int(m.group(1))] = e
    return meshes, fxrs


def _sfx_tpfs(root: Path) -> dict:
    data = souls.bnd4_read((root / 'sfx' / 'frpg_sfxbnd_commoneffects.ffxbnd.dcx').read_bytes())
    return item_tex.index_bundle(data)


def fxr_mesh_refs(fxr: bytes, mesh_ids) -> list:
    """The s##### ids a .fxr names: 32-bit words equal to a mesh id (the resource ids of the
    effect's model layers; FXR is not parsed further)."""
    return sorted({v for (v,) in struct.iter_unpack('<I', fxr[:len(fxr) // 4 * 4])
                   if v in mesh_ids})


def convert_sfx_mesh(entry, name: str, center: bool = True, tex=None) -> bbmodel.Model:
    """tex: (out dir, {basename: tpf entry} of the bundle) to also decode each submesh's albedo
    (item_tex.py) and name it in the submesh's "texture"."""
    flver = souls.flver_read(entry.data)
    model = bbmodel.Model(name)
    model.bones.append({'name': 'root', 'parent': -1, 'deform': True,
                        'bind_world': [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]})
    allv = []
    pieces = []
    for mi, mesh in enumerate(flver.meshes):
        if not mesh.positions:
            continue
        face_set = next((fs for fs in mesh.face_sets if fs.flags == 0),
                        min(mesh.face_sets, key=lambda fs: fs.flags) if mesh.face_sets else None)
        tris = face_set.triangles() if face_set else []
        if not tris:
            continue
        verts = []
        for i, pos in enumerate(mesh.positions):
            verts.append([bbmodel.mirror(pos), bbmodel.mirror(mesh.normals[i]),
                          tuple(mesh.uvs[i])])
        flipped = []
        for k in range(0, len(tris) - 2, 3):
            a, b, c = tris[k], tris[k + 1], tris[k + 2]
            flipped.extend((a, c, b))  # X mirrored: the winding flips
        pieces.append((mi, mesh, verts, flipped))
        allv.extend(v[0] for v in verts)
    lo = [min(p[k] for p in allv) for k in range(3)]
    hi = [max(p[k] for p in allv) for k in range(3)]
    c = [(lo[k] + hi[k]) * 0.5 for k in range(3)] if center else [0.0, 0.0, 0.0]
    for mi, mesh, verts, tris in pieces:
        out = [([p[k] - c[k] for k in range(3)], n, uv, (0, 0, 0, 0), (1.0, 0.0, 0.0, 0.0))
               for p, n, uv in verts]
        material = flver.materials[mesh.material] if mesh.material < len(flver.materials) else None
        info = {'name': f'{name}/{mi}', 'part': name, 'slot': 'weapon',
                'material': material.name if material else '',
                'mask': -1, 'hidden': False, 'dominant_bone': 0}
        if tex is not None and material is not None:
            stem = item_tex.albedo_stem(flver, material)
            key = item_tex.add(tex[0], stem, tex[1], '_alp' not in (material.mtd or '').lower()) if stem else None
            if key:
                info['texture'] = key
        model.add_mesh(info, out, tris)
    model.extra = {'kind': 'throwable', 'source_center': c,
                   'size': [hi[k] - lo[k] for k in range(3)]}
    return model


def bullet_table(root: Path) -> list:
    defs = souls.bnd4_read((root / 'paramdef' / 'paramdef.paramdefbnd.dcx').read_bytes())
    params = souls.bnd4_read((root / 'param' / 'gameparam' / 'gameparam.parambnd.dcx').read_bytes())
    goods = {g['id']: g for g in souls.param_table(params, defs, 'EquipParamGoods')}
    bullets = {b['id']: b for b in souls.param_table(params, defs, 'Bullet')}
    rows = []
    for gid in GOODS:
        g = goods.get(gid)
        b = bullets.get(g.get('refId')) if g and g.get('refCategory') == 1 else None
        if b is None:
            continue
        name, mesh, how = SFX_MESH[gid]
        rows.append({'goods': gid, 'name': name, 'bullet': b['id'],
                     'sfx_mesh': mesh, 'sfx_mesh_how': how,
                     **{k: b.get(k) for k in BULLET_FIELDS}})
    return rows


def export(root: Path, out: Path, extra_meshes=(), quiet: bool = False) -> list:
    out = out / 'throwables'
    out.mkdir(parents=True, exist_ok=True)
    meshes, fxrs = _sfx_entries(root)
    tex = (out.parent, _sfx_tpfs(root))
    rows = bullet_table(root)
    # Which flight effects name which mesh (for the findings doc and a sanity check).
    for r in rows:
        fxr = fxrs.get(r['sfxId_Bullet'])
        r['fxr_mesh_refs'] = fxr_mesh_refs(fxr.data, meshes) if fxr else None
    (out / 'throwables.json').write_text(json.dumps({'source': 'param/gameparam Bullet + '
                                                     'sfx commoneffects', 'items': rows},
                                                    indent=1), encoding='utf-8')
    done = []
    for r in rows:
        mesh = r['sfx_mesh']
        if mesh is None or mesh not in meshes:
            if not quiet:
                print(f'th_{r["goods"]}: {r["name"]}: no mesh ({r["sfx_mesh_how"]})')
            continue
        model = convert_sfx_mesh(meshes[mesh], f'th_{r["goods"]}', tex=tex)
        model.extra.update(goods=r['goods'], name=r['name'], sfx_mesh=mesh,
                           source=f'sfx/frpg_sfxbnd_commoneffects.ffxbnd.dcx s{mesh:05d}.flver',
                           how=r['sfx_mesh_how'])
        path = model.write(out)
        done.append(path)
        if not quiet:
            size = model.extra['size']
            print(f'{path.name}: {r["name"]} <- s{mesh:05d} ({model.vertex_count} vertices, '
                  f'{size[0]:.2f} x {size[1]:.2f} x {size[2]:.2f} m; {r["sfx_mesh_how"]})')
    for mesh in extra_meshes:
        if mesh not in meshes:
            print(f'sfx mesh s{mesh:05d}: not in the bundle')
            continue
        model = convert_sfx_mesh(meshes[mesh], f'sfx_s{mesh:05d}', tex=tex)
        done.append(model.write(out))
        if not quiet:
            print(f'{done[-1].name}: s{mesh:05d} ({model.vertex_count} vertices)')
    item_tex.flush(out.parent)
    return done
