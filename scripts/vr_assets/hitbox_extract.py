#!/usr/bin/env python3
"""Attack hitboxes and hurtboxes from the user's Bloodborne dump, as JSON for the VR sandbox.

    python scripts/vr_assets/hitbox_extract.py --game C:/games/CUSA03173
        [--out out/vr_assets/hitbox] [--weapons all|trick|ID,ID,...] [--chr c0000,c2600|all|none]
        [--no-tae] [--npc-tae]

Background: docs/vr/findings/14-hitboxes.md. The chain this script follows is the game's own:
  TAE event 1 (BehaviorJudgeID)  ->  BehaviorParam[_PC] row  ->  AtkParam_Pc/_Npc row
  ->  up to 4 spheres/capsules (radius, DmyPoly1, DmyPoly2)  ->  dummy polys on the weapon or body model.
  Player row id = 100,000,000 + EquipParamWeapon.behaviorVariationId * 1000 + judge;
  enemy row id  = 200,000,000 + NpcParam.behaviorVariationId * 1000 + judge.
Hurtboxes are the ragdoll rigid bodies (hknp capsules; a few spheres and convex boxes) in
chr/cXXXX.chrbnd/cXXXX.HKX, each tied to the animation bone that drives it, with the damage part
group from cXXXX.hkxpwv.

Reads only (never writes) these files under --game/dvdroot_ps4:
  param/gameparam/gameparam.parambnd.dcx, paramdef/paramdef.paramdefbnd.dcx,
  parts/wp_a_XXXX.partsbnd.dcx, chr/c0000.anibnd.dcx, chr/cXXXX.chrbnd.dcx (+ .anibnd with --npc-tae),
  msg/engus/item.msgbnd.dcx.
Writes <out>/index.json, <out>/wp_<equipId>.json, <out>/chr_cXXXX.json (format "bbhitbox" v1).

Space: the same as scripts/vr_assets/extract.py ("bbmodel"): metres, right-handed, +Y up, FromSoftware
X mirrored (x' = -x). Points are model space of the named model (weapon slot FLVER or character), and
"*_local" points are in the space of the named bone at bind pose, so a renderer that poses that bone can
carry the shape. Stdlib only; FLVER/BND4/PARAM/FMG parsing comes from scripts/vr_assets/souls.py.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    import souls  # noqa: E402  (scripts/vr_assets/souls.py)
except ImportError as exc:  # pragma: no cover
    sys.exit(f'hitbox_extract: needs scripts/vr_assets/souls.py next to this script ({exc})')

FORMAT = 'bbhitbox'
VERSION = 1
SPACE = 'right-handed meters, +X right, +Y up, -Z forward (FromSoftware X mirrored)'
PC_BASE, NPC_BASE = 100_000_000, 200_000_000
HIT_TYPES = {0: 'tip', 1: 'middle', 2: 'root'}
REF_TYPES = {0: 'attack', 1: 'bullet', 2: 'speffect'}
HIT_SOURCES = {0: 'weapon', 1: 'body'}


# ---- small math (row-vector 4x4 matrices as flat 16-tuples, like souls.py) --------------------------

def mat_from_qst(q, t, s=(1.0, 1.0, 1.0)):
    """Havok hkQsTransform (quaternion x,y,z,w; translation; scale) as a row-vector matrix."""
    x, y, z, w = q
    r = (1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
         2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
         2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y))
    return (r[0] * s[0], r[1] * s[0], r[2] * s[0], 0.0,
            r[3] * s[1], r[4] * s[1], r[5] * s[1], 0.0,
            r[6] * s[2], r[7] * s[2], r[8] * s[2], 0.0,
            t[0], t[1], t[2], 1.0)


def mat_inverse(m):
    """Inverse of an affine row-vector matrix (3x3 part + translation row)."""
    a, b, c = m[0], m[1], m[2]
    d, e, f = m[4], m[5], m[6]
    g, h, i = m[8], m[9], m[10]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-12:
        raise ValueError('singular matrix')
    inv = ((e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
           (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
           (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det)
    tx, ty, tz = m[12], m[13], m[14]
    ntx = -(tx * inv[0] + ty * inv[3] + tz * inv[6])
    nty = -(tx * inv[1] + ty * inv[4] + tz * inv[7])
    ntz = -(tx * inv[2] + ty * inv[5] + tz * inv[8])
    return (inv[0], inv[1], inv[2], 0.0, inv[3], inv[4], inv[5], 0.0,
            inv[6], inv[7], inv[8], 0.0, ntx, nty, ntz, 1.0)


def quat_rotate(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz),
            v[2] + w * tz + (x * ty - y * tx))


def out_vec(v, digits=4):
    """FromSoftware model space -> bbmodel space (mirror X), rounded for JSON."""
    return [round(-v[0], digits) + 0.0, round(v[1], digits) + 0.0, round(v[2], digits) + 0.0]


# ---- Havok packfile (hk_2014.1.0-r1, 64-bit little endian) ----------------------------------------
# Member offsets were read from the files' own hkClass reflection (__types__ section):
#   hkaSkeleton: name +16, parentIndices +24, bones +40 (hkaBone 16 B), referencePose +56 (hkQsTransform 48 B)
#   hkaSkeletonMapper: mapping +16 -> skeletonA +0, skeletonB +8, simpleMappings +64 (64 B: boneA, boneB)
#   hknpPhysicsSystemData: bodyCinfos +64 (hknpBodyCinfo 96 B: shape +0, name +32, position +48, orientation +64)
#   hknpRagdollData: skeleton +120, boneToBodyMap +128
#   hknpShape: convexRadius +20; hknpCapsuleShape: a +64, b +80

class HkPack:
    def __init__(self, data: bytes):
        if data[:8] != bytes.fromhex('57e0e05710c0c010'):
            raise ValueError('not a Havok packfile')
        self.b = data
        file_version = struct.unpack_from('<i', data, 0x0C)[0]
        if data[0x10] != 8:
            raise ValueError('only 64-bit packfiles are supported')
        nsec = struct.unpack_from('<i', data, 0x14)[0]
        pad = struct.unpack_from('<h', data, 0x3E)[0] if file_version >= 11 else 0
        at = 0x40 + pad
        sections = []
        for _ in range(nsec):
            tag = data[at:at + 19].split(b'\0', 1)[0].decode('ascii', 'replace')
            sections.append((tag,) + struct.unpack_from('<7I', data, at + 20))
            at += 0x40 if file_version >= 11 else 0x30
        names = {}
        cn = sections[0]
        q, end = cn[1], cn[1] + cn[2]
        while q + 5 < end and data[q + 4] == 9:
            stop = data.index(b'\0', q + 5)
            names[q + 5 - cn[1]] = data[q + 5:stop].decode('ascii', 'replace')
            q = stop + 1
        sec = next(s for s in sections if s[0] == '__data__')
        self.base = base = sec[1]
        self.ptr = {}
        q = base + sec[2]
        while q + 8 <= base + sec[3]:
            src, dst = struct.unpack_from('<II', data, q)
            q += 8
            if src != 0xFFFFFFFF:
                self.ptr[src] = dst
        q = base + sec[3]
        while q + 12 <= base + sec[4]:
            src, _sec, dst = struct.unpack_from('<III', data, q)
            q += 12
            if src != 0xFFFFFFFF:
                self.ptr.setdefault(src, dst)
        self.objects = []
        q = base + sec[4]
        while q + 12 <= base + sec[5]:
            off, _sec, cname = struct.unpack_from('<III', data, q)
            q += 12
            if off != 0xFFFFFFFF:
                self.objects.append((off, names.get(cname, '?')))
        self.cls = dict(self.objects)

    def i32(self, off): return struct.unpack_from('<i', self.b, self.base + off)[0]
    def f32(self, off): return struct.unpack_from('<f', self.b, self.base + off)[0]
    def vec(self, off, n): return struct.unpack_from('<%df' % n, self.b, self.base + off)

    def array(self, off):
        return self.ptr.get(off), self.i32(off + 8)

    def cstr(self, off):
        t = self.ptr.get(off)
        if t is None:
            return None
        a = self.base + t
        return self.b[a:self.b.index(b'\0', a)].decode('ascii', 'replace')

    def skeleton(self, off, local_pose: dict | None = None):
        pi, n = self.array(off + 24)
        parents = list(struct.unpack_from('<%dh' % n, self.b, self.base + pi)) if n else []
        bo, nb = self.array(off + 40)
        bones = [self.cstr(bo + 16 * k) for k in range(nb)]
        ro, nr = self.array(off + 56)
        pose = []
        for k in range(nr):
            v = self.vec(ro + 48 * k, 12)
            pose.append((v[4:8], v[0:3], v[8:11]))
        worlds = []
        for k in range(len(bones)):
            q, t, s = pose[k] if k < len(pose) else ((0, 0, 0, 1), (0, 0, 0), (1, 1, 1))
            if local_pose and k in local_pose:
                t, q, s = local_pose[k]
            local = mat_from_qst(q, t, s)
            p = parents[k] if k < len(parents) else -1
            worlds.append(souls.mat_mul(local, worlds[p]) if 0 <= p < k else local)
        return {'offset': off, 'name': self.cstr(off + 16), 'bones': bones, 'parents': parents,
                'worlds': worlds}

    def skeletons(self):
        return [self.skeleton(off) for off, c in self.objects if c == 'hkaSkeleton']

    def first_frame(self) -> dict:
        """bone index -> (t, q, s) at frame 0 of this file's spline-compressed animation.
        hkaSplineCompressedAnimation: maskAndQuantizationSize +68, data +152;
        hkaAnimationBinding: transformTrackToBoneIndices +32 (empty = track i drives bone i)."""
        anim = next((o for o, c in self.objects if c == 'hkaSplineCompressedAnimation'), None)
        if anim is None:
            return {}
        tracks = self.i32(anim + 68) // 4
        dp, dn = self.array(anim + 152)
        frame = spline_first_frame(self.b[self.base + dp:self.base + dp + dn], tracks)
        bind = next((o for o, c in self.objects if c == 'hkaAnimationBinding'), None)
        bones = list(range(tracks))
        if bind is not None:
            tp, tn = self.array(bind + 32)
            if tn:
                bones = list(struct.unpack_from('<%dh' % tn, self.b, self.base + tp))
        return {bone: frame[k] for k, bone in enumerate(bones) if k < len(frame)}

    def mappers(self):
        out = []
        for off, c in self.objects:
            if c != 'hkaSkeletonMapper':
                continue
            m = off + 16
            so, n = self.array(m + 64)
            pairs = [struct.unpack_from('<hh', self.b, self.base + so + 64 * k) for k in range(n)]
            out.append((self.ptr.get(m), self.ptr.get(m + 8), pairs))
        return out

    def ragdoll(self):
        rd = next((off for off, c in self.objects if c == 'hknpRagdollData'), None)
        if rd is None:
            return None
        bo, nb = self.array(rd + 64)
        bodies = []
        for k in range(nb):
            b = bo + 96 * k
            shape = self.ptr.get(b)
            kind = self.cls.get(shape, '?')
            pos = self.vec(b + 48, 3)
            quat = self.vec(b + 64, 4)
            body = {'index': k, 'name': self.cstr(b + 32), 'shape_class': kind,
                    'radius': self.f32(shape + 20) if shape is not None else 0.0}
            if kind == 'hknpCapsuleShape':
                for key, at in (('a', 64), ('b', 80)):
                    v = quat_rotate(quat, self.vec(shape + at, 3))
                    body[key] = (pos[0] + v[0], pos[1] + v[1], pos[2] + v[2])
            elif shape is not None:
                # hknpConvexShape::m_vertices is an hkRelArray at +40 (u16 size, u16 offset from the
                # field): a sphere stores its centre, a polytope its corner points.
                size, rel = struct.unpack_from('<HH', self.b, self.base + shape + 40)
                pts = []
                for j in range(size):
                    v = quat_rotate(quat, self.vec(shape + 40 + rel + 16 * j, 3))
                    pts.append((pos[0] + v[0], pos[1] + v[1], pos[2] + v[2]))
                pts = list(dict.fromkeys(tuple(round(c, 5) for c in p) for p in pts)) or [pos]
                body['points'] = pts
                body['a'] = body['b'] = tuple(sum(c) / len(pts) for c in zip(*pts))
            bodies.append(body)
        skel = self.skeleton(self.ptr[rd + 120]) if (rd + 120) in self.ptr else None
        mo, nm = self.array(rd + 128)
        bone_to_body = list(struct.unpack_from('<%di' % nm, self.b, self.base + mo)) if nm else []
        return {'skeleton': skel, 'bodies': bodies, 'bone_to_body': bone_to_body}


# ---- hkaSplineCompressedAnimation, first frame (port of SoulsAssetPipeline SplineCompressedAnimation.cs)
# Trick-weapon slots get their resting shape from the slot's a999_000000[_N].hkx animation, not from the
# skeleton reference pose (the Hunter Axe's shaft is collapsed only in that animation).

def _quat40(b, p):
    v = int.from_bytes(b[p:p + 5], 'little')
    mask, frac = (1 << 12) - 1, 0.000345436
    c = [(((v >> (12 * k)) & mask) - (mask >> 1)) * frac for k in range(3)]
    w = 1.0 - sum(x * x for x in c)
    w = math.sqrt(w) if w > 0 else 0.0
    c.insert((v >> 36) & 3, -w if (v >> 38) & 1 else w)
    return tuple(c), 5


def _quat48(b, p):
    x, y, z = struct.unpack_from('<hhh', b, p)
    mask, frac = (1 << 15) - 1, 0.000043161
    c = [((n & mask) - (mask >> 1)) * frac for n in (x, y, z)]
    w = 1.0 - sum(v * v for v in c)
    w = math.sqrt(w) if w > 0 else 0.0
    c.insert(((y >> 14) & 2) | ((x >> 15) & 1), -w if (z >> 15) & 1 else w)
    return tuple(c), 6


def _quat32(b, p):
    c = struct.unpack_from('<I', b, p)[0]
    r = ((c >> 18) & 1023) / 1023.0
    r = 1.0 - r * r
    pt = float(c & 0x3FFFF)
    phi = math.floor(math.sqrt(pt))
    theta = 0.0
    if phi > 0:
        theta = (math.pi / 4) * (pt - phi * phi) / phi
        phi = (math.pi / 2 / 511.0) * phi
    mag = math.sqrt(max(0.0, 1.0 - r * r))
    q = [math.sin(phi) * math.cos(theta) * mag, math.sin(phi) * math.sin(theta) * mag, math.cos(phi) * mag, r]
    return tuple(-v if c & (0x10000000 << k) else v for k, v in enumerate(q)), 4


_ROT_QUANT = {0: (_quat32, 4), 1: (_quat40, 1), 2: (_quat48, 2),
              5: (lambda b, p: (struct.unpack_from('<4f', b, p), 16), 4)}


def _bspline(deg, frame, knots, pts):
    n = len(pts)
    if n == 1:
        return pts[0]
    if frame >= knots[n]:
        span = n - 1
    else:
        lo, hi = deg, n
        span = (lo + hi) // 2
        while frame < knots[span] or frame >= knots[span + 1]:
            lo, hi = (lo, span) if frame < knots[span] else (span, hi)
            span = (lo + hi) // 2
    basis = [1.0, 0.0, 0.0, 0.0, 0.0]
    for i in range(1, deg + 1):
        for j in range(i - 1, -1, -1):
            a = (frame - knots[span - j]) / (knots[span + i - j] - knots[span - j])
            tmp = basis[j] * a
            basis[j + 1] += basis[j] - tmp
            basis[j] = tmp
    if isinstance(pts[0], tuple):
        return tuple(sum(pts[span - i][k] * basis[i] for i in range(deg + 1)) for k in range(len(pts[0])))
    return sum(pts[span - i] * basis[i] for i in range(deg + 1))


def spline_first_frame(data: bytes, tracks: int) -> list:
    """(translation, quaternion, scale) of every transform track at frame 0 of block 0."""
    def align(p, a):
        return (p + a - 1) // a * a

    masks = [tuple(data[4 * i:4 * i + 4]) for i in range(tracks)]
    p = align(4 * tracks, 4)

    def vec3(p, flags, quant, value):
        value = list(value)
        if flags & 0x70:
            n, deg = struct.unpack_from('<hB', data, p)
            knots = list(data[p + 3:p + 3 + n + deg + 2])
            p = align(p + 3 + n + deg + 2, 4)
            bounds = {}
            for k in range(3):
                if flags & (0x10 << k):
                    bounds[k] = struct.unpack_from('<2f', data, p)
                    p += 8
                elif flags & (1 << k):
                    value[k] = struct.unpack_from('<f', data, p)[0]
                    p += 4
            points = {k: [] for k in bounds}
            for _ in range(n + 1):
                for k in sorted(bounds):
                    if quant == 0:
                        ratio, p = data[p] / 255.0, p + 1
                    else:
                        ratio, p = struct.unpack_from('<H', data, p)[0] / 65535.0, p + 2
                    points[k].append(bounds[k][0] + (bounds[k][1] - bounds[k][0]) * ratio)
            for k, pts in points.items():
                value[k] = _bspline(deg, 0.0, knots, pts)
        else:
            for k in range(3):
                if flags & (1 << k):
                    value[k] = struct.unpack_from('<f', data, p)[0]
                    p += 4
        return tuple(value), p

    out = []
    for quant, pos_flags, rot_flags, scale_flags in masks:
        t, p = vec3(p, pos_flags, quant & 3, (0.0, 0.0, 0.0))
        p = align(p, 4)
        q = (0.0, 0.0, 0.0, 1.0)
        if rot_flags & 0xFF:
            read, al = _ROT_QUANT[(quant >> 2) & 0xF]  # KeyError for the 24/16-bit kinds (unused here)
            if rot_flags & 0xF0:
                n, deg = struct.unpack_from('<hB', data, p)
                knots = list(data[p + 3:p + 3 + n + deg + 2])
                p = align(p + 3 + n + deg + 2, al)
                pts = []
                for _ in range(n + 1):
                    v, size = read(data, p)
                    pts.append(v)
                    p += size
                q = _bspline(deg, 0.0, knots, pts)
            else:
                q, size = read(data, align(p, al))
                p = align(p, al) + size
            norm = math.sqrt(sum(c * c for c in q)) or 1.0
            q = tuple(c / norm for c in q)
        p = align(p, 4)
        s3, p = vec3(p, scale_flags, (quant >> 6) & 3, (1.0, 1.0, 1.0))
        p = align(p, 4)
        out.append((t, q, s3))
    return out


# ---- TAE (Bloodborne/DS3: version 0x1000C, 64-bit) ---------------------------------------------------

def tae_read(data: bytes) -> dict:
    """{'id', 'anims': [(anim id, [(type, start, end, params bytes)])]} of a TAE file."""
    b = souls.dcx_decompress(data)
    if b[:4] != b'TAE ' or b[7] != 0xFF:
        raise ValueError('not a 64-bit TAE')
    tae_id, count = struct.unpack_from('<ii', b, 0x50)
    anims_at = struct.unpack_from('<q', b, 0x58)[0]
    anims = []
    for i in range(count):
        anim_id, at = struct.unpack_from('<qq', b, anims_at + 16 * i)
        headers, groups = struct.unpack_from('<qq', b, at)
        nev = struct.unpack_from('<i', b, at + 32)[0]
        raw = []
        for k in range(nev):
            st_at, en_at, ev_at = struct.unpack_from('<qqq', b, headers + 24 * k)
            ev_type = struct.unpack_from('<i', b, ev_at)[0]
            params_at = struct.unpack_from('<q', b, ev_at + 8)[0]
            raw.append([ev_type, struct.unpack_from('<f', b, st_at)[0],
                        struct.unpack_from('<f', b, en_at)[0], params_at, ev_at])
        bounds = sorted({r[4] for r in raw} | ({groups} if groups else set()))
        events = []
        for ev_type, start, end, params_at, _ev_at in raw:
            nxt = [x for x in bounds if x > params_at]
            stop = min(nxt) if nxt else params_at + 0x40
            events.append((ev_type, start, end, b[params_at:stop] if params_at else b''))
        anims.append((anim_id, events))
    return {'id': tae_id, 'anims': anims}


PARRY_FLAGS = (5, 73)  # TAE event 0 (ChrActionFlag) "Parry Possible" / "Parry Possible State"


def tae_attack_windows(tae: dict, category: str) -> tuple[dict, dict]:
    """judge -> [window], anim name -> [blade trail / parry window] from event 1
    (InvokeAttackBehavior), event 118 (SpawnFFX_Blade) and event 0 flags 5/73 (parry possible) of
    one TAE file (DSAnimStudio TAE.Template.BB.xml layouts)."""
    windows, trails = {}, {}
    for anim_id, events in tae['anims']:
        name = f'{category}_{anim_id:06d}'
        for ev_type, start, end, p in events:
            if ev_type == 0 and len(p) >= 4 and struct.unpack_from('<i', p)[0] in PARRY_FLAGS:
                trails.setdefault(name, []).append(
                    {'start': round(start, 4), 'end': round(end, 4), 'parry_flag': struct.unpack_from('<i', p)[0]})
            elif ev_type == 1 and len(p) >= 16:
                atk_type, atk_index, judge, direction, _unk, state = struct.unpack_from('<iiiBBh', p)
                windows.setdefault(judge, []).append(
                    {'anim': name, 'start': round(start, 4), 'end': round(end, 4),
                     'attack_type': atk_type, 'attack_index': atk_index, 'direction': direction})
            elif ev_type == 118 and len(p) >= 12:
                ffx, src, base, tip, slot = struct.unpack_from('<ihhhh', p)
                trails.setdefault(name, []).append(
                    {'start': round(start, 4), 'end': round(end, 4), 'ffx': ffx,
                     'source': {0: 'body', 1: 'left_weapon', 2: 'right_weapon'}.get(src, src),
                     'base_dmy': base, 'tip_dmy': tip, 'fx_slot': slot})
    return windows, trails


# ---- HKXPWV (Bloodborne layout: ragdoll bone -> damage part group) ----------------------------------

def hkxpwv_read(data: bytes) -> list:
    b = souls.dcx_decompress(data)
    nmap, nanim, nrag = struct.unpack_from('<HHH', b, 6)
    if struct.unpack_from('<i', b, 0)[0] != 1 or len(b) != 0x20 + 8 * nmap + 8 * nanim + 16 * nrag:
        raise ValueError('not a Bloodborne-layout hkxpwv')
    at = 0x20 + 8 * nmap + 8 * nanim
    out = []
    for k in range(nrag):
        ragdoll_param, damage_anim, part_group, unk, unk_bb = struct.unpack_from('<ihBBb', b, at + 16 * k)
        out.append({'ragdoll_param': ragdoll_param, 'damage_anim': damage_anim, 'part_group': part_group,
                    'unk': unk})
    return out


# ---- game access ---------------------------------------------------------------------------------------

class Game:
    def __init__(self, root: Path):
        self.root = root
        self.dvd = root / 'dvdroot_ps4'
        if not (self.dvd / 'param' / 'gameparam' / 'gameparam.parambnd.dcx').is_file():
            raise SystemExit(f'not a Bloodborne dump (no dvdroot_ps4/param/gameparam): {root}')
        params = souls.bnd4_read((self.dvd / 'param/gameparam/gameparam.parambnd.dcx').read_bytes())
        paramdefs = souls.bnd4_read((self.dvd / 'paramdef/paramdef.paramdefbnd.dcx').read_bytes())
        self.p = {}
        for name in ('EquipParamWeapon', 'BehaviorParam_PC', 'BehaviorParam', 'AtkParam_Pc',
                     'AtkParam_Npc', 'NpcParam'):
            self.p[name] = {r['id']: r for r in souls.param_table(params, paramdefs, name)}
        self.text = {}
        msg = self.dvd / 'msg' / 'engus' / 'item.msgbnd.dcx'
        if msg.is_file():
            for f in souls.bnd4_read(msg.read_bytes()):
                if f.basename in ('武器名.fmg', 'NPC名.fmg'):
                    self.text[f.basename[:-4]] = souls.fmg_read(f.data)
        self._body = None
        self._player_taes = None

    def weapon_name(self, wid):
        return self.text.get('武器名', {}).get(wid)

    def npc_name(self, name_id):
        return self.text.get('NPC名', {}).get(name_id)

    def player_tae(self, category: str):
        if self._player_taes is None:
            self._player_taes = {f.basename: f.data for f in
                                 souls.bnd4_read((self.dvd / 'chr/c0000.anibnd.dcx').read_bytes())
                                 if f.basename.endswith('.tae')}
        data = self._player_taes.get(category + '.tae')
        return tae_read(data) if data else None

    def body(self):
        """The player base model c0000.flver: bones and dummies (for hitSourceType=body hits)."""
        if self._body is None:
            files = souls.bnd4_read((self.dvd / 'chr/c0000.chrbnd.dcx').read_bytes())
            fl = souls.flver_read(next(f.data for f in files if f.basename.lower() == 'c0000.flver'),
                                  decode_vertices=False)
            self._body = model_dummies(fl, None)
        return self._body


def model_dummies(flver, hk_worlds: dict | None) -> dict:
    """{'bones': [names], 'dummies': {id: [(pos, fwd, up, attach bone name)]}} in FromSoft model space.
    With hk_worlds (bone name -> Havok reference-pose world), dummies that follow a bone are moved
    from the FLVER bind pose to that pose (trick-weapon slots keep their transformed pose there)."""
    worlds = souls.bone_worlds(flver.bones)
    names = [b.name for b in flver.bones]
    out = {}
    for d in flver.dummies:
        pos, fwd, up = souls.dummy_model(d, worlds)
        bone = names[d.attach_bone] if 0 <= d.attach_bone < len(names) else None
        local = None
        if hk_worlds and bone in hk_worlds and d.flag1:
            m = souls.mat_mul(mat_inverse(worlds[d.attach_bone]), hk_worlds[bone])
            pos = souls.transform_point(m, pos)
            fwd, up = souls.transform_dir(m, fwd), souls.transform_dir(m, up)
        elif bone is not None:
            local = souls.transform_point(mat_inverse(worlds[d.attach_bone]), pos)
        out.setdefault(d.reference_id, []).append((pos, fwd, up, bone, local))
    return {'bones': names, 'worlds': worlds, 'dummies': out}


def dummy_json(dmy_id, entries, with_local=False):
    out = []
    for p, f, u, b, local in entries:
        d = {'id': dmy_id, 'pos': out_vec(p), 'fwd': out_vec(f), 'up': out_vec(u), 'bone': b}
        if with_local and local is not None:
            d['pos_local'] = out_vec(local)
        out.append(d)
    return out


# ---- hits ------------------------------------------------------------------------------------------------

def atk_hits(atk: dict) -> list:
    hits = []
    for i in range(4):
        radius = atk.get(f'hit{i}_Radius', 0.0)
        d1, d2 = atk.get(f'hit{i}_DmyPoly1', -1), atk.get(f'hit{i}_DmyPoly2', -1)
        if d1 < 0 or radius <= 0:
            continue
        hits.append((i, radius, d1, d2, atk.get(f'hit{i}_hitType', 0)))
    return hits


def resolve_point(dmy: int, hit_source: int, hand: str, slots: dict, body: dict) -> dict:
    """Where an AtkParam dummy-poly id lives (DSAnimStudio NewChrAsm.GetDummyPolySpawnPlace rules):
    id // 1000 == 10..13 -> right weapon model slot 0..3, 20..23 -> left weapon slot 0..3,
    0 -> the attacking hand's slot 0 (weapon attacks) or the body; ids missing on the chosen
    model fall back to the body model."""
    prefix, local = divmod(dmy, 1000)
    where = None
    if 10 <= prefix <= 13:
        where = ('right', prefix - 10)
    elif 20 <= prefix <= 23:
        where = ('left', prefix - 20)
    elif prefix == 0 and hit_source == 0:
        where = (hand, 0)
    point = {'dmy': dmy, 'local_id': local}
    if where and where[0] == hand and where[1] in slots and local in slots[where[1]]['dummies']:
        entries = slots[where[1]]['dummies'][local]
        point.update(src=f'{where[0]}_weapon', slot=where[1], pos=out_vec(entries[0][0]),
                     count=len(entries))
    elif local in body['dummies']:
        entries = body['dummies'][local]
        point.update(src='body', pos=out_vec(entries[0][0]), bone=entries[0][3], count=len(entries))
        if entries[0][4] is not None:
            point['pos_local'] = out_vec(entries[0][4])
        if where:
            point['fallback'] = True
    else:
        point.update(src=f'{where[0]}_weapon' if where else 'body', unresolved=True)
        if where:
            point['slot'] = where[1]
    return point


def hit_json(i, radius, d1, d2, hit_type, resolve) -> dict:
    p1 = resolve(d1)
    capsule = d2 >= 0 and d2 != d1
    out = {'index': i, 'radius': round(radius, 4), 'hit_type': HIT_TYPES.get(hit_type, hit_type),
           'shape': 'capsule' if capsule else 'sphere', 'p1': p1}
    if capsule:
        out['p2'] = resolve(d2)
    return out


def atk_summary(atk: dict) -> dict:
    return {'atk_name_jp': atk.get('row_name') or None,
            'hit_source': HIT_SOURCES.get(atk.get('hitSourceType'), atk.get('hitSourceType')),
            'motion_value': {k: atk.get(f'atk{k.capitalize()}Correction') for k in
                             ('phys', 'mag', 'fire', 'thun', 'stam')},
            'atk_flat': {k: atk.get(f'atk{k.capitalize()}') for k in ('phys', 'mag', 'fire', 'thun', 'stam')},
            'dmg_level': atk.get('dmgLevel'), 'hit_stop_time': round(atk.get('hitStopTime', 0.0), 4),
            'knockback_dist': round(atk.get('knockbackDist', 0.0), 4),
            'throw_type_id': atk.get('throwTypeId'), 'map_hit_type': atk.get('mapHitType')}


# ---- weapons ---------------------------------------------------------------------------------------------

def weapon_ids(game: Game, spec: str) -> list:
    rows = game.p['EquipParamWeapon']
    if spec not in ('all', 'trick'):
        return [int(x) for x in spec.split(',') if x.strip()]
    seen, out = set(), []
    for wid in sorted(rows):
        r = rows[wid]
        # +0 rows of real weapons; x090000 rows are the "wandering" NPC copies.
        if wid < 1_000_000 or wid % 10_000 or wid % 100_000 == 90_000:
            continue
        key = (r.get('equipModelId'), r.get('behaviorVariationId'))
        part = game.dvd / 'parts' / f"wp_a_{r.get('equipModelId', 0):04d}.partsbnd.dcx"
        if key in seen or not part.is_file():
            continue
        if spec == 'trick' and all(r.get(f'dmypolyId_Slot{s}RightFormB', -1) == -1 and
                                   r.get(f'dmypolyId_Slot{s}LeftFormB', -1) == -1 for s in range(4)):
            continue
        seen.add(key)
        out.append(wid)
    return out


def extract_weapon(game: Game, wid: int, with_tae: bool) -> dict:
    row = game.p['EquipParamWeapon'][wid]
    model_id = row['equipModelId']
    model = f'wp_a_{model_id:04d}'
    files = souls.bnd4_read((game.dvd / 'parts' / f'{model}.partsbnd.dcx').read_bytes())
    hand = 'right' if row.get('rightHandEquipable') or not row.get('leftHandEquipable') else 'left'
    hk_by_slot, categories = {}, set()
    for f in files:
        if f.basename.lower().endswith('.anibnd') and 400 <= f.id <= 403:
            inner = souls.bnd4_read(f.data)
            for g in inner:
                m = re.match(r'a(\d{3})_\d{6}', g.basename)
                if m and m.group(1) not in ('000', '999'):
                    categories.add('a' + m.group(1))
            skel = next((g for g in inner if g.basename.lower().startswith('skeleton')), None)
            if skel is None:
                continue
            hk = HkPack(skel.data)
            pose, label = None, skel.basename + ' reference pose'
            idle = next((g for g in inner if g.basename.lower().startswith('a999_000000')), None)
            if idle is not None:
                try:
                    pose, label = HkPack(idle.data).first_frame(), idle.basename + ' frame 0'
                except (KeyError, ValueError, struct.error, IndexError, ZeroDivisionError):
                    pose = None
            sk = hk.skeleton(next(o for o, c in hk.objects if c == 'hkaSkeleton'), pose)
            hk_by_slot[f.id - 400] = (label, dict(zip(sk['bones'], sk['worlds'])))
    slots = {}
    slot_json = []
    for f in files:
        if not f.basename.lower().endswith('.flver') or not 200 <= f.id <= 203:
            continue
        s = f.id - 200
        fl = souls.flver_read(f.data, decode_vertices=False)
        hk = hk_by_slot.get(s)
        slots[s] = model_dummies(fl, hk[1] if hk else None)
        attach = {}
        for h in ('Right', 'Left'):
            attach[h.lower()] = {k: (None if row.get(f'dmypolyId_Slot{s}{h}{v}', -1) == -1
                                     else row.get(f'dmypolyId_Slot{s}{h}{v}'))
                                 for k, v in (('hang', 'Hang'), ('form_a', 'FormA'), ('form_b', 'FormB'))}
        slot_json.append({'slot': s, 'flver': f.basename, 'pose': hk[0] if hk else 'flver bind',
                          'attach_body_dummy': attach,
                          'dummies': [d for k in sorted(slots[s]['dummies'])
                                      for d in dummy_json(k, slots[s]['dummies'][k])]})
    forms = {'A': sorted(s for s in slots if row.get(f'dmypolyId_Slot{s}{hand.capitalize()}FormA', -1) != -1),
             'B': sorted(s for s in slots if row.get(f'dmypolyId_Slot{s}{hand.capitalize()}FormB', -1) != -1)}
    body = game.body()
    # Which c0000 TAE file (aNNN.tae) holds this weapon's moveset: the weapon's own anibnd names it
    # (a112_033300.hkx ...); otherwise wepmotionCategory >= 100 is the TAE id itself (true for all 17
    # weapons where both are known). Categories < 100 go through a remap table we have not found
    # (28 -> a112, 38 -> a122, 30 -> a103, ...), so those weapons get no timing.
    windows, trails, category, tae_source = {}, {}, None, None
    wmc = row.get('wepmotionCategory', 0)
    if len(categories) == 1:
        category, tae_source = next(iter(categories)), 'weapon anibnd'
    elif not categories and wmc >= 100:
        category, tae_source = f'a{wmc}', 'wepmotionCategory'
    if with_tae and category:
        tae = game.player_tae(category)
        if tae:
            windows, trails = tae_attack_windows(tae, category)
        else:
            category, tae_source = None, None
    var = row['behaviorVariationId']
    behaviors, atks = game.p['BehaviorParam_PC'], game.p['AtkParam_Pc']
    attacks = []
    for bid in sorted(k for k in behaviors if PC_BASE + var * 1000 <= k < PC_BASE + var * 1000 + 1000):
        beh = behaviors[bid]
        judge = bid - PC_BASE - var * 1000
        entry = {'judge': judge, 'behavior_id': bid, 'name_jp': beh.get('row_name') or None,
                 'ref': REF_TYPES.get(beh['refType'], beh['refType']), 'ref_id': beh['refId'],
                 'stamina': beh.get('stamina')}
        atk = atks.get(beh['refId']) if beh['refType'] == 0 else None
        if atk:
            src = atk.get('hitSourceType', 0)
            entry.update(atk_summary(atk))
            entry['hits'] = [hit_json(*h, resolve=lambda d, s=src: resolve_point(d, s, hand, slots, body))
                             for h in atk_hits(atk)]
            used = {p.get('slot') for h in entry['hits'] for p in (h['p1'], h.get('p2') or {})
                    if p.get('src', '').endswith('weapon') and 'slot' in p}
            entry['form'] = ('A' if used and used <= set(forms['A']) else
                             'B' if used and used <= set(forms['B']) else ('A/B' if used else None))
        if judge in windows:
            entry['tae'] = windows[judge]
            entry['trails'] = {w['anim']: [t for t in trails[w['anim']] if 'ffx' in t]
                               for w in windows[judge] if w['anim'] in trails}
        attacks.append(entry)
    return {'format': FORMAT, 'version': VERSION, 'kind': 'weapon', 'space': SPACE,
            'id': wid, 'name_en': game.weapon_name(wid), 'name_jp': row.get('row_name') or None,
            'model': model, 'model_json': f'{model}.json', 'hand': hand,
            'behavior_variation': var, 'wepmotion_category': row.get('wepmotionCategory'),
            'tae_category': category, 'tae_source': tae_source,
            'tae_category_candidates': sorted(categories),
            'forms': forms, 'slots': slot_json, 'attacks': attacks,
            'source': f'parts/{model}.partsbnd.dcx; gameparam EquipParamWeapon/BehaviorParam_PC/AtkParam_Pc'
                      + (f'; chr/c0000.anibnd.dcx {category}.tae' if category else '')}


# ---- characters ------------------------------------------------------------------------------------------

def ragdoll_to_anim_bone(hk: HkPack, rd: dict) -> dict:
    """ragdoll bone index -> the animation bone that drives it. The animation->ragdoll
    hkaSkeletonMapper (skeleton B = ragdoll) is what keyframes the ragdoll bodies from the animated
    pose, so it wins (c0000: Master drives Ragdoll_Pelvis01, Spine1 drives Ragdoll_Spine01); the
    reverse mapper and then name rules fill gaps."""
    skel = rd['skeleton']
    skels = {s['offset']: s for s in hk.skeletons()}
    out = {}
    mappers = hk.mappers()
    for a_off, b_off, pairs in mappers:          # animation -> ragdoll
        if skel and b_off == skel['offset'] and a_off in skels:
            for a, b in pairs:
                if 0 <= a < len(skels[a_off]['bones']):
                    out.setdefault(b, skels[a_off]['bones'][a])
    for a_off, b_off, pairs in mappers:          # ragdoll -> animation
        if skel and a_off == skel['offset'] and b_off in skels:
            for a, b in pairs:
                if 0 <= b < len(skels[b_off]['bones']):
                    out.setdefault(a, skels[b_off]['bones'][b])
    if skel:
        for k, name in enumerate(skel['bones']):
            if k not in out and name:
                out[k] = re.sub(r'\d+$', '', name.replace('Ragdoll_', '')).strip()
    return out


def chr_ids(game: Game, spec: str) -> list:
    if spec == 'none':
        return []
    if spec != 'all':
        return [x.strip() for x in spec.split(',') if x.strip()]
    return sorted(p.name[:5] for p in (game.dvd / 'chr').glob('c*.chrbnd.dcx'))


def extract_character(game: Game, chr_id: str, npc_tae: bool) -> dict | None:
    files = souls.bnd4_read((game.dvd / 'chr' / f'{chr_id}.chrbnd.dcx').read_bytes())
    by = {f.basename.lower(): f for f in files}
    hkx = by.get(f'{chr_id}.hkx')
    flv = by.get(f'{chr_id}.flver')
    if hkx is None or flv is None:
        return None
    hk = HkPack(hkx.data)
    rd = hk.ragdoll()
    fl = souls.flver_read(flv.data, decode_vertices=False)
    model = model_dummies(fl, None)
    bone_index = {n: k for k, n in enumerate(model['bones'])}
    pwv = None
    if f'{chr_id}.hkxpwv' in by:
        try:
            pwv = hkxpwv_read(by[f'{chr_id}.hkxpwv'].data)
        except (ValueError, struct.error):
            pwv = None
    hurt = []
    if rd:
        anim_bone = ragdoll_to_anim_bone(hk, rd)
        body_to_bone = {body: k for k, body in enumerate(rd['bone_to_body']) if body >= 0}
        for body in rd['bodies']:
            rb = body_to_bone.get(body['index'], body['index'])
            bone = anim_bone.get(rb)
            shape = {'hknpCapsuleShape': 'capsule', 'hknpSphereShape': 'sphere',
                     'hknpConvexPolytopeShape': 'convex'}.get(body['shape_class'], body['shape_class'])
            entry = {'body': body['name'], 'shape': shape, 'radius': round(body['radius'], 4),
                     'a': out_vec(body['a']), 'b': out_vec(body['b']), 'bone': bone}
            if shape == 'convex':
                entry['points'] = [out_vec(p) for p in body['points']]
            if bone in bone_index:
                inv = mat_inverse(model['worlds'][bone_index[bone]])
                entry['a_local'] = out_vec(souls.transform_point(inv, body['a']))
                entry['b_local'] = out_vec(souls.transform_point(inv, body['b']))
                if shape == 'convex':
                    entry['points_local'] = [out_vec(souls.transform_point(inv, p)) for p in body['points']]
            if pwv and rb < len(pwv):
                entry['part_group'] = pwv[rb]['part_group']
                if pwv[rb]['damage_anim'] != -1:
                    entry['damage_anim'] = pwv[rb]['damage_anim']
            hurt.append(entry)
    number = int(chr_id[1:])
    # NpcParam row id = chr number * 100 + variant (c2600 -> 260000..260099); c0000 is the player.
    npcs = [r for k, r in sorted(game.p['NpcParam'].items()) if k // 100 == number] if number else []
    windows, extras = {}, {}
    if npc_tae and (game.dvd / 'chr' / f'{chr_id}.anibnd.dcx').is_file():
        for f in souls.bnd4_read((game.dvd / 'chr' / f'{chr_id}.anibnd.dcx').read_bytes()):
            if f.basename.endswith('.tae'):
                w, x = tae_attack_windows(tae_read(f.data), f.basename[:-4])
                for judge, lst in w.items():
                    windows.setdefault(judge, []).extend(lst)
                extras.update(x)
    attacks = []
    behaviors, atks = game.p['BehaviorParam'], game.p['AtkParam_Npc']

    def resolve(d):
        # Enemies carry their weapons in the body model (bones such as Weapon_00), so every id is
        # looked up there; ids with a weapon-slot prefix (>= 10000) are marked as a fallback.
        local = d % 1000 if d >= 10000 else d
        entries = model['dummies'].get(local)
        if not entries:
            return {'dmy': d, 'src': 'body', 'unresolved': True}
        point = {'dmy': d, 'src': 'body', 'pos': out_vec(entries[0][0]), 'bone': entries[0][3],
                 'count': len(entries)}
        if entries[0][4] is not None:
            point['pos_local'] = out_vec(entries[0][4])
        if d >= 10000:
            point['fallback'] = True
        return point

    for var in sorted({r.get('behaviorVariationId', -1) for r in npcs if r.get('behaviorVariationId', -1) >= 0}):
        lo = NPC_BASE + var * 1000
        for bid in sorted(k for k in behaviors if lo <= k < lo + 1000):
            beh = behaviors[bid]
            entry = {'variation': var, 'judge': bid - lo, 'behavior_id': bid,
                     'name_jp': beh.get('row_name') or None,
                     'ref': REF_TYPES.get(beh['refType'], beh['refType']), 'ref_id': beh['refId']}
            atk = atks.get(beh['refId']) if beh['refType'] == 0 else None
            if atk:
                entry.update(atk_summary(atk))
                entry['hits'] = [hit_json(*h, resolve=resolve) for h in atk_hits(atk)]
            if entry['judge'] in windows:
                entry['tae'] = windows[entry['judge']]
                parry = {w['anim']: [x for x in extras.get(w['anim'], []) if 'parry_flag' in x]
                         for w in windows[entry['judge']]}
                parry = {k: v for k, v in parry.items() if v}
                if parry:
                    entry['parry_windows'] = parry
            attacks.append(entry)
    names = [game.npc_name(r.get('nameId')) for r in npcs if r.get('nameId', -1) > 0]
    return {'format': FORMAT, 'version': VERSION, 'kind': 'character', 'space': SPACE, 'chr': chr_id,
            'name_en': next((n for n in names if n), None),
            'npc_rows': [{'id': r['id'], 'name_jp': r.get('row_name') or None,
                          'name_en': game.npc_name(r.get('nameId')),
                          'behavior_variation': r.get('behaviorVariationId'),
                          'hit_radius': round(r.get('hitRadius', 0.0), 4),
                          'hit_height': round(r.get('hitHeight', 0.0), 4),
                          'parts_damage_rate': [round(r.get(f'partsDamageRate{k}', 1.0), 3) for k in range(1, 9)],
                          'weak_parts_damage_rate': round(r.get('weakPartsDamageRate', 1.0), 3)}
                         for r in npcs],
            'hurtboxes': hurt,
            'body_dummies': [d for k in sorted(model['dummies'])
                             for d in dummy_json(k, model['dummies'][k], with_local=True)],
            'attacks': attacks,
            'source': f'chr/{chr_id}.chrbnd.dcx ({chr_id}.HKX ragdoll, {chr_id}.hkxpwv, {chr_id}.flver); '
                      'gameparam NpcParam/BehaviorParam/AtkParam_Npc'}


# ---- main ------------------------------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--game', required=True, type=Path, help='dump root (contains dvdroot_ps4)')
    ap.add_argument('--out', type=Path,
                    default=Path(__file__).resolve().parents[2] / 'out' / 'vr_assets' / 'hitbox')
    ap.add_argument('--weapons', default='all', help="'all', 'trick', 'none' or EquipParamWeapon ids")
    ap.add_argument('--chr', default='c0000', help="'all', 'none' or chr ids like c0000,c2600")
    ap.add_argument('--no-tae', action='store_true', help='skip player TAE attack windows')
    ap.add_argument('--npc-tae', action='store_true',
                    help='also read enemy anibnd TAE: attack windows and parry-possible flags')
    args = ap.parse_args(argv)
    game_root = args.game.resolve()
    out = args.out.resolve()
    if game_root in out.parents or out == game_root:
        raise SystemExit('refusing to write inside the game dump')
    t0 = time.time()
    game = Game(game_root)
    out.mkdir(parents=True, exist_ok=True)
    index = {'format': FORMAT + '-index', 'version': VERSION, 'space': SPACE, 'game': str(game_root),
             'weapons': [], 'characters': []}
    for wid in ([] if args.weapons == 'none' else weapon_ids(game, args.weapons)):
        if wid not in game.p['EquipParamWeapon']:
            print(f'skip weapon {wid}: no EquipParamWeapon row', file=sys.stderr)
            continue
        try:
            doc = extract_weapon(game, wid, not args.no_tae)
        except (OSError, ValueError, KeyError, struct.error) as exc:
            print(f'skip weapon {wid}: {exc}', file=sys.stderr)
            continue
        name = f'wp_{wid}.json'
        (out / name).write_text(json.dumps(doc, ensure_ascii=False, indent=1), encoding='utf-8')
        nhits = sum(len(a.get('hits', [])) for a in doc['attacks'])
        index['weapons'].append({'id': wid, 'name_en': doc['name_en'], 'model': doc['model'], 'file': name,
                                 'attacks': len(doc['attacks']), 'hits': nhits, 'tae': doc['tae_category']})
        print(f"weapon {wid} {doc['name_en']!s:28} {doc['model']} attacks={len(doc['attacks'])} hits={nhits} "
              f"tae={doc['tae_category']}")
    for chr_id in chr_ids(game, args.chr):
        if not (game.dvd / 'chr' / f'{chr_id}.chrbnd.dcx').is_file():
            print(f'skip {chr_id}: no chrbnd', file=sys.stderr)
            continue
        try:
            doc = extract_character(game, chr_id, args.npc_tae)
        except (ValueError, KeyError, struct.error) as exc:
            print(f'skip {chr_id}: {exc}', file=sys.stderr)
            continue
        if doc is None:
            continue
        name = f'chr_{chr_id}.json'
        (out / name).write_text(json.dumps(doc, ensure_ascii=False, indent=1), encoding='utf-8')
        index['characters'].append({'chr': chr_id, 'name_en': doc['name_en'], 'file': name,
                                    'hurtboxes': len(doc['hurtboxes']), 'attacks': len(doc['attacks'])})
        print(f"chr {chr_id} {doc['name_en']!s:28} hurtboxes={len(doc['hurtboxes'])} attacks={len(doc['attacks'])}")
    (out / 'index.json').write_text(json.dumps(index, ensure_ascii=False, indent=1), encoding='utf-8')
    print(f'wrote {len(index["weapons"])} weapons, {len(index["characters"])} characters to {out} '
          f'in {time.time() - t0:.1f}s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
