"""Meshes for the sandbox's consumables (docs/vr/findings/36-vr-consumables.md). Used by
extract.py (--consumables); the dump is only read.

    python scripts/vr_assets/extract.py --consumables [--game C:/games/CUSA03173]

Writes under out/vr_assets/consumables/ cs_<goods>.json/.bin (bbmodel format, recentred on the
bounding box like the throwables, throwables_extract.convert_sfx_mesh). EquipParamGoods.modelId is 0
for these goods too and c0000's chrbnd holds no item props, so, like the throwables, the only 3D data
is the flight/use effect meshes of sfx/frpg_sfxbnd_commoneffects.ffxbnd.dcx (s#####.flver). The match is
by material name and by the one or two effects that use the mesh; both are guesses:

    s10009  material "hp" (0.13 x 0.23 m vial; only the Numbing Mist effects 20008/20108)  -> Blood Vial
    s10015  material "ariannas_blood" (0.11 x 0.25 m vial; effects 20007/20107)           -> Iosefka's Blood Vial
    s10022  material "ariannas_blood" (0.11 x 0.21 m vial; effect 20057)                  -> Blood of Adeline
    s10023  material "Medicine_of_lead" (0.09 x 0.12 m flask; effect 20010)               -> Lead Elixir
    s10025  material "c2730_Bell_cloth" (0.13 m bell; effect 200005)                      -> the four bells
Everything else (papers, pellets, elixirs, bones, marks) has no mesh in the dump that could be
identified and uses the procedural stand-ins of the sandbox (throwables.cpp MakeThrowModel).
"""
from __future__ import annotations

from pathlib import Path

import throwables_extract as tx

# goods id -> (name, sfx mesh id, how it was found)
CONSUMABLE_MESH = {
    1000: ('Blood Vial', 10009, 'name "hp" vial (Numbing Mist effects only)'),
    703: ("Iosefka's Blood Vial", 10015, 'name ariannas_blood vial'),
    706: ('Blood of Adeline', 10022, 'name ariannas_blood vial (effect 20057)'),
    2030: ('Lead Elixir', 10023, 'name Medicine_of_lead flask'),
    200: ('Beckoning Bell', 10025, 'name c2730_Bell_cloth'),
    205: ('Small Resonant Bell', 10025, 'name c2730_Bell_cloth'),
    225: ('Sinister Resonant Bell', 10025, 'name c2730_Bell_cloth'),
    2050: ('Choir Bell', 10025, 'name c2730_Bell_cloth'),
}


def export(root: Path, out: Path, quiet: bool = False) -> list:
    out = out / 'consumables'
    out.mkdir(parents=True, exist_ok=True)
    meshes, _fxrs = tx._sfx_entries(root)
    done = []
    for goods, (name, mesh, how) in CONSUMABLE_MESH.items():
        if mesh not in meshes:
            if not quiet:
                print(f'cs_{goods}: {name}: s{mesh:05d} not in the bundle')
            continue
        model = tx.convert_sfx_mesh(meshes[mesh], f'cs_{goods}')
        model.extra.update(goods=goods, name=name, sfx_mesh=mesh, how=how,
                           source=f'sfx/frpg_sfxbnd_commoneffects.ffxbnd.dcx s{mesh:05d}.flver')
        model.extra['kind'] = 'consumable'
        path = model.write(out)
        done.append(path)
        if not quiet:
            size = model.extra['size']
            print(f'{path.name}: {name} <- s{mesh:05d} ({model.vertex_count} vertices, '
                  f'{size[0]:.2f} x {size[1]:.2f} x {size[2]:.2f} m; {how})')
    return done
