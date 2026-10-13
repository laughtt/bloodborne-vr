"""The game's own in-hand models of the goods, for the VR hand (docs/vr/findings/40-custom-hands.md
section 9). Used by extract.py (--held); the dump is only read.

    python scripts/vr_assets/extract.py --held [--game C:/games/CUSA03173] [--out out/vr_assets]

Writes under out/vr_assets/held/ (never committed: game data):
    <goods>.json/.bin   the model as the game draws it in the hand (bbmodel.py): the effect mesh with
                        its FLVER bone transforms and the effect's model scale applied, in metres, in
                        the frame the effect is spawned in (the dummy poly; not recentred)
    held.json           the index: per goods the mesh, its real size and centre, the two sockets (where
                        that frame sits in the right hand: "socket" in the R_Weapon bone frame, X
                        mirrored like the weapon models; "grip_socket" in the controller's grip frame
                        through the arms IK's hand placement), the texture lift, the confidence and
                        the whole chain it was found through; "missing" lists the goods without one

EquipParamGoods.modelId is 0 for every row: the game draws a used item as an SFX model. The chain,
all of it read from the dump (findings/40 section 9):
  1. EquipParamGoods.goodsUseAnim N -> the behaviour graph (c0000.anibnd Behaviors/c0000.hkx) state
     "UseItem_N Selector" -> its clip a000_<anim>.
  2. a00.tae, that anim's events: type 96 (SpawnFFX: i32 ffx, i32 dummy) and type 114 (the goods'
     effect: i32 base, i16 -, i16 dummy; ffx = base * 100 + EquipParamGoods.sfxVariationId). The
     ones at a dummy on the hand (R_Hand / L_Hand) are the item in the hand. (Checked: a000_100080,
     the Blood Vial's, spawns 20056 and 20256 where a000_100081, the blood samples', has 114 with
     bases 200 and 202, so base * 100 + variation 56 is the vial's own effect.)
  3. Without one: the goods' SpEffect (refCategory 2) -> SpEffectParam.stateInfo -> SpEffectVfxParam
     midst/init sfx (the Monocular at the head, the Lantern), then the Bullet's flight effect
     (refCategory 1, Bullet.sfxId_Bullet: the throwables' in-flight model).
  4. The effect f<ffx>.fxr (sfx/frpg_sfxbnd_{commoneffects,dlceffects}): each model layer is the
     record (41, 0, mesh id, 0) followed by three keyframe properties (11, ...): the X/Y/Z scale at
     t = 0. Layers with a zero scale at spawn are hidden and skipped. The mesh is s<id>.flver.
  5. The FLVER: a static mesh's vertices are in its default bone's space, so the bone's bind
     transform (non-uniform scales included: the vial is a cylinder scaled 1 x 2.9 x 1 mm) is applied;
     the old throwables/consumables export skipped it (the vial came out 40 x 61 x 40 "metres").
The socket: the dummy's frame (XNA CreateWorld: Z = -forward, Y = upward) in the bind pose of
c0000.flver, relative to the R_Weapon bone (L_Weapon for a left-hand dummy, mirrored to the right),
both X mirrored; rot_deg is Euler X, then Y, then Z (m3::FromEulerDeg). The axis convention was
checked on the animations: in a000_100050 (Sedative) the jar's +Y (its neck) points down toward the
face while drinking, in a000_100080 (Blood Vial) the needle end points into the thigh at the stab.
grip_socket: the same frame in the OpenXR grip frame, through vam_hand_rotation / vam_grip_wrist of
src/vr_arm_math.h with the default grip offset; it equals (R_Weapon in the grip) x socket. Goods whose
model comes from step 3 (or a guess) have no hand dummy: both null, the bridge centres the mesh on the
hold category's socket. Scale: the FXR scale is kept even where it is large (Tiny Tonitrus x3.25, 0.87 m:
in a000_100350 the tip then reaches the ground at the slam, at x1 it stops 0.2 m above it).
"""
from __future__ import annotations

import bisect
import json
import math
import re
import struct
from pathlib import Path

import bbmodel
import catalog
import item_tex
import souls
from hitbox_extract import HkPack, tae_read

FMG_GOODS = 10
FORMAT = 'bbheld'
# Goods the game draws nothing in the hand for (no effect model in the chain), given the closest real
# mesh of the bundles instead: (mesh id, why). Marked "guess" in the index.
GUESS = {
    1100: (10000, 'Antidote: its use animation a000_100000 spawns only the body effect 20255. s10000 is '
                  'a "m_kaifuku" (recovery medicine) flask drawn by f20002 / f20050-52, effects no goods '
                  'variation reaches'),
    1410: (10004, 'Shining Coins: a000_100040 spawns nothing and the flight f20133 has no standard model '
                  'layer; s10004 is the "coin" disc'),
}
BUNDLES = ('commoneffects', 'dlceffects')
HAND_BONES = {'R_Hand': 'R_Weapon', 'L_Hand': 'L_Weapon', 'R_Weapon': 'R_Weapon', 'L_Weapon': 'L_Weapon'}
SKIP_CLASSES = ('hkbVariableBindingSet', 'hkbClipTriggerArray', 'hkbStateMachineTransitionInfoArray')


# ---- 1. goodsUseAnim -> animation (the behaviour graph) ---------------------------------------------

def use_anim_clips(anibnd_files) -> dict:
    """goodsUseAnim -> [anim id] from the 'UseItem_<N> Selector' generators of the behaviour."""
    beh = next(f for f in anibnd_files if 'behaviors' in f.name.lower() and f.name.lower().endswith('.hkx'))
    hk = HkPack(beh.data)
    objs = sorted(hk.objects)
    offs = [o for o, _ in objs]
    cls = dict(objs)

    def end(off):
        i = bisect.bisect_right(offs, off)
        return offs[i] if i < len(offs) else off + 0x200

    def children(off):
        out = []
        for src in range(off, end(off), 8):
            d = hk.ptr.get(src)
            if d in cls and cls[d] not in SKIP_CLASSES:
                out.append(d)
        return out

    def clips(off, depth=0, seen=None):
        seen = set() if seen is None else seen
        if off in seen or depth > 8:
            return []
        seen.add(off)
        if cls[off] == 'hkbClipGenerator':
            name = hk.cstr(off + 144) or ''
            m = re.match(r'a000_(\d{6})$', name)
            return [int(m.group(1))] if m else []
        found = []
        for c in children(off):
            found += clips(c, depth + 1, seen)
        return found

    out = {}
    for off, c in objs:
        if c not in ('hkbManualSelectorGenerator', 'CustomManualSelectorGenerator'):
            continue
        try:
            name = hk.cstr(off + 56)
        except (ValueError, struct.error):
            continue
        m = re.match(r'UseItem_(\d+) Selector$', name or '')
        if m:
            out[int(m.group(1))] = sorted(set(clips(off)))
    return out


# ---- 2. the use animation's effects ------------------------------------------------------------------

def anim_effects(tae: dict, anim_id: int, variation: int) -> list:
    """[(start s, ffx id, dummy, how)] of one a00 anim: SpawnFFX (96) and the goods' effect (114)."""
    out = []
    for aid, events in tae['anims']:
        if aid != anim_id:
            continue
        for ev_type, start, _end, p in events:
            if ev_type == 96 and len(p) >= 8:
                ffx, dummy = struct.unpack_from('<ii', p)
                out.append((start, ffx, dummy, f'TAE 96 ffx {ffx} dummy {dummy}'))
            elif ev_type == 114 and len(p) >= 8 and variation >= 0:
                base, _unk, dummy = struct.unpack_from('<ihh', p)
                out.append((start, base * 100 + variation, dummy,
                            f'TAE 114 base {base} x 100 + sfxVariationId {variation}, dummy {dummy}'))
    return sorted(out)


# ---- 4. effect model layers ----------------------------------------------------------------------------

def fxr_models(fxr: bytes) -> list:
    """[(mesh id, (sx, sy, sz))] of the model layers of an .fxr, in file order."""
    n = len(fxr) // 4
    w = struct.unpack_from(f'<{n}i', fxr)
    out = []
    for i in range(2, n - 3):
        if w[i - 2] == 41 and w[i - 1] == 0 and w[i + 1] == 0 and w[i + 2] == 11:
            scale = []
            at = (i + 2) * 4
            for _ in range(3):
                if at + 32 > len(fxr) or struct.unpack_from('<i', fxr, at)[0] != 11:
                    break
                _t, _z, times, _z2, values, _z3, count = struct.unpack_from('<7i', fxr, at)
                if 0 < count < 256 and 0 < values + 4 <= len(fxr):
                    scale.append(struct.unpack_from('<f', fxr, values)[0])
                at += 32
            out.append((w[i], tuple(scale) if len(scale) == 3 else (1.0, 1.0, 1.0)))
    return out


class Sfx:
    """The effect bundles (meshes, effects, textures), opened once."""

    def __init__(self, root: Path):
        self.meshes, self.fxr, self.tpfs = {}, {}, {}
        for bundle in BUNDLES:
            path = root / 'sfx' / f'frpg_sfxbnd_{bundle}.ffxbnd.dcx'
            if not path.is_file():
                continue
            files = souls.bnd4_read(path.read_bytes())
            tpfs = item_tex.index_bundle(files)
            for f in files:
                if m := re.match(r's(\d+)\.flver$', f.basename):
                    self.meshes.setdefault(int(m.group(1)), (bundle, f, tpfs))
                elif m := re.match(r'f0*(\d+)\.fxr$', f.basename):
                    self.fxr.setdefault(int(m.group(1)), (bundle, f))

    def visible_models(self, ffx: int) -> list:
        """[(mesh id, scale)] drawn when effect `ffx` spawns (no zero-scale layer, mesh in a bundle)."""
        hit = self.fxr.get(ffx)
        if hit is None:
            return []
        out = []
        for mesh, scale in fxr_models(hit[1].data):
            if scale[0] != 1.0 and scale[1] == 1.0 and scale[2] == 1.0:
                # The DLC effects' layers (f20063, f200050-52) set only the first value: read as a
                # uniform scale (the Accursed Brew's skull is 21 cm then, 19 x 33 x 26 cm squashed).
                scale = (scale[0], scale[0], scale[0])
            if mesh in self.meshes and all(abs(s) > 1e-6 for s in scale):
                if all(m != mesh for m, _ in out):
                    out.append((mesh, scale))
        return out


# ---- 5. the mesh as the game draws it -----------------------------------------------------------------

def _inv3(m):
    """Inverse of a 3x3 (rows), None when singular."""
    a, b, c = m
    det = (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
           a[2] * (b[0] * c[1] - b[1] * c[0]))
    if abs(det) < 1e-12:
        return None
    return [[(b[1] * c[2] - b[2] * c[1]) / det, (a[2] * c[1] - a[1] * c[2]) / det, (a[1] * b[2] - a[2] * b[1]) / det],
            [(b[2] * c[0] - b[0] * c[2]) / det, (a[0] * c[2] - a[2] * c[0]) / det, (a[2] * b[0] - a[0] * b[2]) / det],
            [(b[0] * c[1] - b[1] * c[0]) / det, (a[1] * c[0] - a[0] * c[1]) / det, (a[0] * b[1] - a[1] * b[0]) / det]]


def _unit(v, fallback=(0.0, 1.0, 0.0)):
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v] if n > 1e-12 else list(fallback)


def convert(entry, tpfs, name: str, scale, out: Path, texture: bool = True) -> bbmodel.Model:
    """The flver in the effect's frame: each static mesh through its default bone's bind transform,
    times the layer scale, X mirrored (winding flipped)."""
    flver = souls.flver_read(entry.data)
    worlds = souls.bone_worlds(flver.bones)
    model = bbmodel.Model(name)
    model.bones.append({'name': 'root', 'parent': -1, 'deform': True,
                        'bind_world': [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]})
    sx, sy, sz = scale
    for mi, mesh in enumerate(flver.meshes):
        if not mesh.positions:
            continue
        face_set = next((fs for fs in mesh.face_sets if fs.flags == 0),
                        min(mesh.face_sets, key=lambda fs: fs.flags) if mesh.face_sets else None)
        tris = face_set.triangles() if face_set else []
        if not tris:
            continue
        if not mesh.dynamic and 0 <= mesh.default_bone < len(worlds):
            w = worlds[mesh.default_bone]
        else:
            w = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)  # skinned: bind pose in model space
        # Row-vector linear part times the layer scale; normals by its inverse transpose.
        lin = [[w[r * 4 + 0] * sx, w[r * 4 + 1] * sy, w[r * 4 + 2] * sz] for r in range(3)]
        inv = _inv3(lin)
        if inv is None:
            continue  # a bone scaled to nothing: not drawn
        tx, ty, tz = w[12] * sx, w[13] * sy, w[14] * sz
        verts = []
        for i, p in enumerate(mesh.positions):
            q = [p[0] * lin[0][k] + p[1] * lin[1][k] + p[2] * lin[2][k] for k in range(3)]
            q = [q[0] + tx, q[1] + ty, q[2] + tz]
            n = mesh.normals[i] if i < len(mesh.normals) else (0.0, 1.0, 0.0)
            # n' = n * (lin^-1)^T  (row vector): n'_k = sum_j n_j * inv[k][j]
            nn = _unit([sum(n[j] * inv[k][j] for j in range(3)) for k in range(3)])
            uv = tuple(mesh.uvs[i]) if i < len(mesh.uvs) else (0.0, 0.0)
            verts.append((bbmodel.mirror(q), bbmodel.mirror(nn), uv, (0, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)))
        flipped = []
        for k in range(0, len(tris) - 2, 3):
            a, b, c = tris[k], tris[k + 1], tris[k + 2]
            flipped.extend((a, c, b))
        material = flver.materials[mesh.material] if mesh.material < len(flver.materials) else None
        info = {'name': f'{name}/{mi}', 'part': name, 'slot': 'weapon',
                'material': material.name if material else '', 'mask': -1, 'hidden': False,
                'dominant_bone': 0}
        if texture and material is not None:
            stem = item_tex.albedo_stem(flver, material)
            key = item_tex.add(out, stem, tpfs, '_alp' not in (material.mtd or '').lower()) if stem else None
            if key:
                info['texture'] = key
                info['texture_luma'] = round(texture_luma(key), 4)
        model.add_mesh(info, verts, flipped)
    return model


def texture_luma(key: str) -> float:
    """Mean luma (sRGB 0..1) of a decoded albedo over its visible texels."""
    w, h, px = item_tex._cache[key]
    total = count = 0
    for i in range(0, len(px), 4 * 7):  # every 7th texel is plenty
        if px[i + 3] >= 128:
            total += 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2]
            count += 1
    return total / count / 255.0 if count else 0.5


def texture_gain(model: bbmodel.Model) -> float:
    """How much the bridge lifts the texture (flat lighting, no specular): dark metal (the knives' albedo
    is ~0.08) would draw near black. 1 for a texture of mean luma 0.3 or brighter, at most 2.5."""
    lumas = [m['texture_luma'] for m in model.meshes if 'texture_luma' in m]
    if not lumas:
        return 1.0
    mean = sum(lumas) / len(lumas)
    return round(min(2.5, max(1.0, 0.3 / max(mean, 1e-3))), 3)


# ---- the socket: the dummy frame in the R_Weapon frame -------------------------------------------------

class Hunter:
    """c0000.flver bind pose: bones and dummies."""

    def __init__(self, chr_files):
        fl = souls.flver_read(next(f for f in chr_files if f.name.lower().endswith('c0000.flver')).data,
                              decode_vertices=False)
        self.bones = fl.bones
        self.names = [b.name for b in fl.bones]
        self.worlds = souls.bone_worlds(fl.bones)
        self.dummies = {}
        for d in fl.dummies:
            self.dummies.setdefault(d.reference_id, d)

    def hand_of(self, dummy_id: int) -> str | None:
        """'R_Weapon' / 'L_Weapon' when the dummy follows a hand, else None."""
        d = self.dummies.get(dummy_id)
        if d is None or not (0 <= d.attach_bone < len(self.names)):
            return None
        return HAND_BONES.get(self.names[d.attach_bone])

    def socket(self, dummy_id: int) -> dict | None:
        """The dummy's frame in its hand's weapon-bone frame, X mirrored, as the right hand's socket."""
        weapon = self.hand_of(dummy_id)
        if weapon is None:
            return None
        d = self.dummies[dummy_id]
        pos, fwd, up = souls.dummy_model(d, self.worlds)
        f = _unit(fwd, (0.0, 0.0, 1.0))
        u = _unit(up) if d.use_upward else [0.0, 1.0, 0.0]
        x = _unit([f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]], (1.0, 0.0, 0.0))
        y = _unit([x[1] * f[2] - x[2] * f[1], x[2] * f[0] - x[0] * f[2], x[0] * f[1] - x[1] * f[0]])
        dm = (x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, -f[0], -f[1], -f[2], 0, pos[0], pos[1], pos[2], 1)
        wm = self.worlds[self.names.index(weapon)]
        rel = souls.mat_mul(dm, _inv_rigid(wm))           # row vectors: dummy frame in the bone frame
        col = bbmodel.mirror_matrix(rel)                   # column-major, X mirrored
        r = [[col[c * 4 + rr] for c in range(3)] for rr in range(3)]  # r[row][col]
        t = [col[12], col[13], col[14]]
        ex = math.degrees(math.atan2(r[2][1], r[2][2]))
        ey = math.degrees(math.asin(max(-1.0, min(1.0, -r[2][0]))))
        ez = math.degrees(math.atan2(r[1][0], r[0][0]))
        if weapon == 'L_Weapon':  # the bridge mirrors a right-hand socket for the left hand
            t[0], ey, ez = -t[0], -ey, -ez
        return {'pos': [round(v, 5) for v in t], 'rot_deg': [round(ex, 3), round(ey, 3), round(ez, 3)],
                'dummy': dummy_id, 'hand_bone': self.names[d.attach_bone], 'frame': weapon}


    # ---- the same item in the controller's grip frame, through the arms IK's hand placement ----

    def _mirrored(self, bone: int):
        """(R 3x3 rows, t) of a bone's bind world, X mirrored, column vectors."""
        col = bbmodel.mirror_matrix(self.worlds[bone])
        return [[col[c * 4 + r] for c in range(3)] for r in range(3)], [col[12], col[13], col[14]]

    def grip_socket(self, dummy_id: int) -> dict | None:
        """The dummy's frame in the OpenXR grip frame of the controller holding the game's hand, as the
        arms IK places that hand (src/vr_arm_math.h vam_hand_rotation / vam_grip_wrist, the default
        grip -> wrist offset (0, -0.01, 0.07) m and -15 deg pitch of src/runtime_vr_arms.c): the bind
        finger direction (wrist -> mean of Finger1..4) onto grip -Z, the thumb (Finger0) onto +Y.
        Used where the item goes to the controller (no hand bone from the host, the custom hands)."""
        weapon = self.hand_of(dummy_id)
        if weapon is None:
            return None
        side = weapon[0]  # 'R' / 'L'
        d = self.dummies[dummy_id]
        hand = self.names.index(f'{side}_Hand')
        _rh, wrist = self._mirrored(hand)
        knuckles = [self._mirrored(self.names.index(f'{side}_Finger{f}'))[1] for f in range(1, 5)]
        knuckles = [sum(k[i] for k in knuckles) / 4.0 for i in range(3)]
        thumb_base = self._mirrored(self.names.index(f'{side}_Finger0'))[1]
        fingers = _unit([knuckles[i] - wrist[i] for i in range(3)])
        tb = [thumb_base[i] - wrist[i] for i in range(3)]
        dot = sum(tb[i] * fingers[i] for i in range(3))
        thumb = _unit([tb[i] - dot * fingers[i] for i in range(3)])

        def frame(x, y):  # columns x, y, x cross y
            z = [x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]]
            return [[x[r], y[r], z[r]] for r in range(3)]

        def mul(a, b):
            return [[sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3)] for r in range(3)]

        def tr(a):
            return [[a[c][r] for c in range(3)] for r in range(3)]

        basis = mul(frame([0.0, 0.0, -1.0], [0.0, 1.0, 0.0]), tr(frame(fingers, thumb)))
        a = math.radians(-15.0)
        off_q = [[1, 0, 0], [0, math.cos(a), -math.sin(a)], [0, math.sin(a), math.cos(a)]]
        off_p = [0.0, -0.01, 0.07]
        if side == 'L':  # vam_side_offset: x mirrored (a rotation about X keeps its sign)
            off_p[0] = -off_p[0]
        # The dummy frame, X mirrored (the same XNA frame as socket()).
        pos, fwd, up = souls.dummy_model(d, self.worlds)
        f = _unit(fwd, (0.0, 0.0, 1.0))
        u = _unit(up) if d.use_upward else [0.0, 1.0, 0.0]
        x = _unit([f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]], (1.0, 0.0, 0.0))
        y = _unit([x[1] * f[2] - x[2] * f[1], x[2] * f[0] - x[0] * f[2], x[0] * f[1] - x[1] * f[0]])
        dm = (x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, -f[0], -f[1], -f[2], 0, pos[0], pos[1], pos[2], 1)
        col = bbmodel.mirror_matrix(dm)
        rd = [[col[c * 4 + r] for c in range(3)] for r in range(3)]
        pd = [col[12], col[13], col[14]]
        g = mul(off_q, basis)
        rot = mul(g, rd)
        rel = [pd[i] - wrist[i] for i in range(3)]
        t = [off_p[r] + sum(g[r][k] * rel[k] for k in range(3)) for r in range(3)]
        ex = math.degrees(math.atan2(rot[2][1], rot[2][2]))
        ey = math.degrees(math.asin(max(-1.0, min(1.0, -rot[2][0]))))
        ez = math.degrees(math.atan2(rot[1][0], rot[0][0]))
        if side == 'L':
            t[0], ey, ez = -t[0], -ey, -ez
        return {'pos': [round(v, 5) for v in t], 'rot_deg': [round(ex, 3), round(ey, 3), round(ez, 3)],
                'dummy': dummy_id, 'hand_bone': self.names[d.attach_bone], 'frame': 'controller grip (arms IK)'}


def _inv_rigid(m):
    """Inverse of a row-vector rigid transform (rotation + translation, unit scale)."""
    r = [[m[i * 4 + j] for j in range(3)] for i in range(3)]
    rt = [[r[j][i] for j in range(3)] for i in range(3)]
    t = (m[12], m[13], m[14])
    nt = [-(t[0] * rt[0][k] + t[1] * rt[1][k] + t[2] * rt[2][k]) for k in range(3)]
    return (rt[0][0], rt[0][1], rt[0][2], 0, rt[1][0], rt[1][1], rt[1][2], 0,
            rt[2][0], rt[2][1], rt[2][2], 0, nt[0], nt[1], nt[2], 1)


# ---- the chain per goods -------------------------------------------------------------------------------

def find_model(g: dict, clips: dict, tae: dict, hunter: Hunter, sfx: Sfx, sp: dict, vfx: dict, bullets: dict):
    """(models [(mesh, scale)], socket or None, chain dict) of one goods row; models empty when none."""
    use_anim = g.get('goodsUseAnim')
    var = g.get('sfxVariationId')
    var = -1 if var is None else var
    chain = {'goodsUseAnim': use_anim, 'sfxVariationId': var}
    anims = clips.get(use_anim, []) if use_anim is not None else []
    chain['anims'] = [f'a000_{a:06d}' for a in anims]
    tried = []
    for anim in anims:
        for start, ffx, dummy, how in anim_effects(tae, anim, var):
            if hunter.hand_of(dummy) is None:
                continue
            models = sfx.visible_models(ffx)
            tried.append(ffx)
            if models:
                chain.update(step='use animation', anim=f'a000_{anim:06d}', event=how, start_s=round(start, 3),
                             ffx=ffx, fxr=f'f{ffx:09d}.fxr ({sfx.fxr[ffx][0]})', dummy=dummy)
                return models, hunter.socket(dummy), chain
    chain['hand_effects_without_model'] = tried
    if g.get('refCategory') == 2 and g.get('refId', -1) >= 0:
        s = sp.get(g['refId'])
        v = vfx.get(s.get('stateInfo')) if s else None
        if v:
            for key in ('midstSfxId', 'initSfxId'):
                ffx = v.get(key, -1)
                models = sfx.visible_models(ffx) if ffx and ffx > 0 else []
                if models:
                    chain.update(step='SpEffect vfx', spEffect=g['refId'], stateInfo=s.get('stateInfo'),
                                 event=f'SpEffectVfxParam {key} {ffx} at dummy {v.get(key.replace("SfxId", "DmyId"))}',
                                 ffx=ffx, fxr=f'f{ffx:09d}.fxr ({sfx.fxr[ffx][0]})')
                    return models, None, chain
    if g.get('refCategory') == 1 and g.get('refId', -1) >= 0:
        b = bullets.get(g['refId'])
        ffx = b.get('sfxId_Bullet', -1) if b else -1
        models = sfx.visible_models(ffx) if ffx and ffx > 0 else []
        if models:
            chain.update(step='Bullet flight', bullet=g['refId'], event=f'Bullet.sfxId_Bullet {ffx}',
                         ffx=ffx, fxr=f'f{ffx:09d}.fxr ({sfx.fxr[ffx][0]})')
            return models, None, chain
    return [], None, chain


def goods_names(root: Path) -> dict:
    files = souls.bnd4_read((root / 'msg' / 'engus' / 'item.msgbnd.dcx').read_bytes())
    names = {f.id: souls.fmg_read(f.data) for f in files}.get(FMG_GOODS, {})
    return {k: v for k, v in names.items() if v and v != '*'}


def export(root: Path, out: Path, quiet: bool = False) -> dict:
    held_dir = out / 'held'
    held_dir.mkdir(parents=True, exist_ok=True)
    params, defs = catalog.load_params(root)
    goods = {r['id']: r for r in souls.param_table(params, defs, 'EquipParamGoods')}
    sp = {r['id']: r for r in souls.param_table(params, defs, 'SpEffectParam')}
    vfx = {r['id']: r for r in souls.param_table(params, defs, 'SpEffectVfxParam')}
    bullets = {r['id']: r for r in souls.param_table(params, defs, 'Bullet')}
    names = goods_names(root)
    anibnd = souls.bnd4_read(souls.read_file(root / 'chr' / 'c0000.anibnd.dcx'))
    clips = use_anim_clips(anibnd)
    tae = tae_read(next(f for f in anibnd if f.name.lower().endswith('a00.tae')).data)
    hunter = Hunter(souls.bnd4_read(souls.read_file(root / 'chr' / 'c0000.chrbnd.dcx')))
    sfx = Sfx(root)
    items, missing = [], []
    for gid in sorted(goods):
        g = goods[gid]
        name = names.get(gid, '')
        if not name or (g.get('iconId') or 0) >= 5000:  # unnamed rows and gestures
            continue
        if g.get('goodsUseAnim') in (None, 253) and g.get('refCategory') == 0:
            continue
        models, socket, chain = find_model(g, clips, tae, hunter, sfx, sp, vfx, bullets)
        if not models and gid in GUESS and GUESS[gid][0] in sfx.meshes:
            models = [(GUESS[gid][0], (1.0, 1.0, 1.0))]
            chain.update(step='guess', event=GUESS[gid][1])
        if not models:
            missing.append({'goods': gid, 'name': name, 'chain': chain,
                            'why': 'no effect model in the use animation at the hand, the SpEffect vfx '
                                   'or the bullet'})
            continue
        grip_socket = hunter.grip_socket(chain['dummy']) if socket else None
        model = None
        for k, (mesh, scale) in enumerate(models):
            bundle, entry, tpfs = sfx.meshes[mesh]
            part = convert(entry, tpfs, str(gid), scale, out)
            if model is None:
                model = part
            else:  # several visible layers: one model
                for info in part.meshes:
                    verts = []
                    for i in range(info['first_vertex'], info['first_vertex'] + info['vertex_count']):
                        v = bbmodel.VERTEX.unpack_from(part.vertices, i * bbmodel.VERTEX.size)
                        verts.append((v[0:3], v[3:6], v[6:8], v[8:12], v[12:16]))
                    idx = struct.unpack_from(f'<{info["index_count"]}I', part.indices, info['first_index'] * 4)
                    meta = {k2: v2 for k2, v2 in info.items() if k2 not in
                            ('first_vertex', 'vertex_count', 'first_index', 'index_count')}
                    model.add_mesh(meta, verts, [i - info['first_vertex'] for i in idx])
        lo, hi = model.bounds()
        size = [hi[k] - lo[k] for k in range(3)]
        sources = [f'sfx/frpg_sfxbnd_{sfx.meshes[m][0]}.ffxbnd.dcx s{m:05d}.flver x '
                   f'({s[0]:g}, {s[1]:g}, {s[2]:g})' for m, s in models]
        model.extra = {'kind': 'held', 'goods': gid, 'name': name, 'size': size, 'sources': sources,
                       'chain': chain, 'socket': socket, 'grip_socket': grip_socket,
                       'centre': [round((lo[k] + hi[k]) * 0.5, 5) for k in range(3)]}
        path = model.write(held_dir)
        confidence = {'use animation': 'game', 'SpEffect vfx': 'game (vfx, not at the hand)',
                      'Bullet flight': 'flight model', 'guess': 'guess'}[chain['step']]
        rec = {'goods': gid, 'name': name, 'mesh': path.name, 'meshes': [m for m, _ in models],
               'size_m': [round(v, 4) for v in size], 'centre': model.extra['centre'],
               'socket': socket, 'grip_socket': grip_socket, 'tex_gain': texture_gain(model),
               'confidence': confidence, 'sources': sources, 'chain': chain}
        items.append(rec)
        if not quiet:
            where = (f'socket {socket["pos"]} / {socket["rot_deg"]} deg on {socket["hand_bone"]}' if socket
                     else 'category socket')
            print(f'held/{path.name}: {name} <- {", ".join(f"s{m:05d}" for m, _ in models)} '
                  f'({size[0]:.3f} x {size[1]:.3f} x {size[2]:.3f} m; {chain["step"]}: '
                  f'{chain.get("anim", "")} {chain["event"]}; {where})')
    item_tex.flush(out)
    index = {'format': FORMAT, 'version': 1, 'game': str(root),
             'frame': 'right hand grip = R_Weapon bone frame, X mirrored (the weapon models\' space); '
                      'socket pos in m and rot_deg Euler X then Y then Z (m3::FromEulerDeg) of the mesh '
                      'frame in it; null socket: use the hold category\'s',
             'mesh_format': 'bbmodel held/<goods>.json + .bin, metres, real size, not recentred',
             'use_anims': {str(k): [f'a000_{a:06d}' for a in v] for k, v in sorted(clips.items())},
             'items': items, 'missing': missing}
    (held_dir / 'held.json').write_text(json.dumps(index, indent=1, ensure_ascii=False), encoding='utf-8')
    if not quiet:
        print(f'{held_dir / "held.json"}: {len(items)} goods with the game\'s model, {len(missing)} without')
    return index
