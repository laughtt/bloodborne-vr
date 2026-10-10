#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Dodge, backstep, quickstep and jump animations of the player, every frame, as JSON for the VR sandbox.

    python scripts/vr_assets/dodge_extract.py [--game C:/games/CUSA03173] [--out out/vr_assets/dash]
        [--bnd md[,lo,hi]] [--anims all|7100,7400-7407,...] [--no-validate]

Reads only (never writes) these files under --game/dvdroot_ps4/chr:
  c0000.anibnd.dcx          skeleton.HKX (108 bones, Havok order) and the TAE files (a00.tae holds a000_*)
  c0000.chrbnd.dcx          c0000.flver: the Head bone and the eye dummy (reference id 58, two instances)
  c0000_a00_md.anibnd.dcx   a000_005000 ... a000_008250: turns, dashes, dodges, falls (the default --bnd).
                            lo (idle, walk) and hi (damage, gestures) can be added; they hold no dodges.
Writes <out>/dodges.json (format "bbdodge" v1) and prints one table row per animation, then the checks
(the same numbers go into the JSON under "validation").

Animation data: hk_2014.1.0-r1 packfile, hkaSplineCompressedAnimation, decoded for every frame and block
(hitbox_extract.spline_first_frame only does frame 0 of block 0; this is the same port of
SoulsAssetPipeline's SplineCompressedAnimation.cs, extended). Member offsets, checked against the data
((numFrames - 1) * frameDuration == duration, block sizes == floatBlockOffsets):
  hkaAnimation: duration +16, numberOfTransformTracks +20, numberOfFloatTracks +24, extractedMotion +32
  hkaSplineCompressedAnimation: numFrames +56, numBlocks +60, maxFramesPerBlock +64,
    maskAndQuantizationSize +68, blockDuration +72, blockInverseDuration +76, frameDuration +80,
    blockOffsets +88, floatBlockOffsets +104, data +152
  Block b starts at frame b * (maxFramesPerBlock - 1) (blockDuration is 255 frame intervals, so
  consecutive blocks share one frame; checked on the 27 multi-block animations of the hi bank) and its
  B-spline knots count frames from there. floatBlockOffsets[b] is the end of block b's transform data,
  relative to blockOffsets[b]. Track -> bone: hkaAnimationBinding.transformTrackToBoneIndices; a bone
  without a track keeps the skeleton's reference pose.
Root motion is the extracted motion, an hkaDefaultAnimatedReferenceFrame: up +16, forward +32,
duration +48, referenceFrameSamples +56 = one hkVector4 per frame: (x, y, z) translation in the model
space of the start pose and w = rotation about `up` in radians (hkQuaternion(up, w), so the character's
world position is root + R_up(w) * model point; the planted-foot test in the checks confirms the sign).
Animations without that object (in place) have no root motion.
Eye: the midpoint of the two instances of dummy 58 (Dummy_eye) is kept as an offset in the space of the
bone it follows (Head); each frame the animated Head bone (FK from Master) carries it, then the root motion.
head_q is the Head bone's world orientation; its bone axes are not the model axes (the reference pose
has it turned), so cam_q = head_q * conj(head_q_rest) is also given: identity at the reference pose,
i.e. the orientation of a camera that looks along -Z with +Y up when the hunter stands straight.
TAE: a00.tae anim id == the six digits of a000_NNNNNN; event type 0 (ChrActionFlag) carries the flag id in
its first i32 (8 = invincible). The TAE has no clip length: tae.length is its last event time, which can
exceed the clip. kind_guess (roll | quickstep | backstep | jump | other) is a heuristic on the measured eye
and root paths and the invincible flag, with the evidence in "why".

Space of the output (the same as extract.py / hitbox_extract.py): metres, right-handed, +X right, +Y up,
-Z forward, i.e. FromSoftware model space with X mirrored (x' = -x). The character starts at the origin
facing -Z. A FromSoftware point (x, y, z) becomes (-x, y, z); a rotation quaternion (x, y, z, w) becomes
(x, -y, -z, w); a yaw about +Y flips sign. root_yaw_deg is a rotation about +Y of that right-handed space,
so positive turns the character to the left (counter-clockwise seen from above). Stdlib only; DCX/BND4/FLVER
parsing comes from souls.py, packfile and TAE parsing from hitbox_extract.py.
"""
from __future__ import annotations

import argparse
import decimal
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
    from hitbox_extract import (HkPack, _ROT_QUANT, _bspline, model_dummies, quat_rotate,  # noqa: E402
                                spline_first_frame, tae_read)
except ImportError as exc:  # pragma: no cover
    sys.exit(f'dodge_extract: needs scripts/vr_assets/souls.py and hitbox_extract.py next to this script ({exc})')

FORMAT = 'bbdodge'
VERSION = 1
SPACE = ('right-handed meters, +X right, +Y up, -Z forward (FromSoftware X mirrored); '
         'the character starts at the origin facing -Z')
EYE = 'midpoint of dummy 58 (Dummy_eye) on the Head bone'
EYE_DUMMY = 58
HEAD_BONE = 'Head'
BANKS = {'md': 'c0000_a00_md.anibnd.dcx', 'lo': 'c0000_a00_lo.anibnd.dcx', 'hi': 'c0000_a00_hi.anibnd.dcx'}
# TAE event 0 (ChrActionFlag) ids named in docs/vr/findings/13-hands-arms-weapons.md section 7.4
# (DSAnimStudio TAE.Template.BB.xml); every other flag is reported by number only.
FLAG_NAMES = {1: 'rh_attack_input', 4: 'rh_attack_cancel', 5: 'parry_possible', 7: 'disable_turning',
              8: 'invincible', 9: 'lh_attack_input', 16: 'lh_attack_cancel', 24: 'hyper_armour',
              25: 'dodge_input', 26: 'dodge_cancel', 32: 'generic_cancel'}
INVINCIBLE = 8
DODGE_IDS = range(7000, 7500)   # a00 7000s are the dodges (docs/vr/findings/13 section 7.4)
STILL_SPEED = 0.2               # m/s: root speed below this "for good" ends the motion
STAND_EYE = 1.6                 # m: eye height of a standing hunter in the animations (bind pose: 1.654)
IDENT_Q = (0.0, 0.0, 0.0, 1.0)
ONES = (1.0, 1.0, 1.0)


# ---- small math: quaternions (x, y, z, w) and Havok hkQsTransform (t, q, s) -------------------------------

def qmul(a, b):
    """Hamilton product a*b (rotate by b, then by a)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def qaxis(axis, angle):
    s = math.sin(angle / 2) / (math.sqrt(sum(c * c for c in axis)) or 1.0)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(angle / 2))


def unit(q):
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / n for c in q)


def quat_angle(a, b):
    """Rotation angle between two unit quaternions, radians."""
    return 2.0 * math.acos(min(1.0, abs(sum(x * y for x, y in zip(a, b)))))


def near(q, prev):
    """q or -q (the same rotation), whichever is closer to the previous frame's quaternion (w >= 0 for the
    first frame)."""
    dot = sum(a * b for a, b in zip(q, prev)) if prev else q[3]
    return tuple(-c for c in q) if dot < 0 else q


def qs_mul(a, b):
    """hkQsTransform a*b: b expressed in the space of a (translation, rotation, scale)."""
    (at, aq, a_s), (bt, bq, b_s) = a, b
    r = quat_rotate(aq, (a_s[0] * bt[0], a_s[1] * bt[1], a_s[2] * bt[2]))
    return ((at[0] + r[0], at[1] + r[1], at[2] + r[2]), qmul(aq, bq),
            (a_s[0] * b_s[0], a_s[1] * b_s[1], a_s[2] * b_s[2]))


def out_pt(v):
    """FromSoftware model space -> sandbox space (mirror X)."""
    return (-v[0], v[1], v[2])


def out_q(q):
    """Rotation quaternion, same mirror: (x, y, z, w) -> (x, -y, -z, w)."""
    return (q[0], -q[1], -q[2], q[3])


def rnd(v, digits=4):
    return round(v, digits) + 0.0     # + 0.0 turns -0.0 into 0.0


def rnd_list(vs, digits=4):
    return [rnd(v, digits) for v in vs]


# ---- hkaSplineCompressedAnimation, every frame -----------------------------------------------------------

class Vec3Track:
    """Position or scale of one transform track: static components and/or B-spline components."""
    __slots__ = ('value', 'spline')

    def __init__(self, value, spline=None):
        self.value, self.spline = tuple(value), spline     # spline: (degree, knots, {axis: control points})

    def at(self, frame):
        if self.spline is None:
            return self.value
        deg, knots, chans = self.spline
        v = list(self.value)
        for k, pts in chans.items():
            v[k] = _bspline(deg, frame, knots, pts)
        return tuple(v)


class QuatTrack:
    """Rotation of one transform track: a static quaternion or a B-spline of quaternions."""
    __slots__ = ('q', 'spline')

    def __init__(self, q, spline=None):
        self.q, self.spline = q, spline                     # spline: (degree, knots, control quaternions)

    def at(self, frame):
        if self.spline is None:
            return self.q
        deg, knots, pts = self.spline
        return unit(_bspline(deg, frame, knots, pts))


def _align(p, a):
    return (p + a - 1) // a * a


def _read_vec3(data, p, flags, quant, default):
    """One position/scale channel group (the layout of spline_first_frame's vec3, keeping the splines)."""
    value, spline = list(default), None
    if flags & 0x70:
        n, deg = struct.unpack_from('<hB', data, p)
        knots = list(data[p + 3:p + 3 + n + deg + 2])
        p = _align(p + 3 + n + deg + 2, 4)
        bounds = {}
        for k in range(3):
            if flags & (0x10 << k):
                bounds[k] = struct.unpack_from('<2f', data, p)
                p += 8
            elif flags & (1 << k):
                value[k] = struct.unpack_from('<f', data, p)[0]
                p += 4
        chans = {k: [] for k in bounds}
        for _ in range(n + 1):
            for k in sorted(bounds):
                if quant == 0:
                    ratio, p = data[p] / 255.0, p + 1
                else:
                    ratio, p = struct.unpack_from('<H', data, p)[0] / 65535.0, p + 2
                chans[k].append(bounds[k][0] + (bounds[k][1] - bounds[k][0]) * ratio)
        spline = (deg, knots, chans)
    else:
        for k in range(3):
            if flags & (1 << k):
                value[k] = struct.unpack_from('<f', data, p)[0]
                p += 4
    return Vec3Track(value, spline), p


def _read_rot(data, p, flags, kind):
    if not flags & 0xFF:
        return QuatTrack(IDENT_Q), p
    if kind not in _ROT_QUANT:
        raise ValueError(f'rotation quantization {kind} (THREECOMP24 / STRAIGHT16) is not supported')
    read, al = _ROT_QUANT[kind]
    if flags & 0xF0:
        n, deg = struct.unpack_from('<hB', data, p)
        knots = list(data[p + 3:p + 3 + n + deg + 2])
        p = _align(p + 3 + n + deg + 2, al)
        pts = []
        for _ in range(n + 1):
            v, size = read(data, p)
            pts.append(tuple(v))
            p += size
        return QuatTrack(None, (deg, knots, pts)), p
    p = _align(p, al)
    q, size = read(data, p)
    return QuatTrack(unit(q)), p + size


def parse_block(data: bytes, base: int, n_tracks: int):
    """The transform tracks of the spline block starting at data[base]: (list of (pos, rot, scale), end).
    Layout (SplineCompressedAnimation.cs ReadSplineCompressedAnimByteBlock): 4 mask bytes per track
    (quantization, position, rotation, scale flags), then per track its position, rotation and scale
    data, each part padded to 4 (the rotation to its own alignment). `end` is where the transform data
    stops (the float tracks would start there); the next block follows at the next multiple of 16."""
    p = base + 4 * n_tracks
    masks = [tuple(data[base + 4 * i:base + 4 * i + 4]) for i in range(n_tracks)]
    tracks = []
    for quant, pos_flags, rot_flags, scale_flags in masks:
        pos, p = _read_vec3(data, p, pos_flags, quant & 3, (0.0, 0.0, 0.0))
        p = _align(p, 4)
        rot, p = _read_rot(data, p, rot_flags, (quant >> 2) & 0xF)
        p = _align(p, 4)
        scl, p = _read_vec3(data, p, scale_flags, (quant >> 6) & 3, ONES)
        p = _align(p, 4)
        tracks.append((pos, rot, scl))
    return tracks, p


class SplineAnim:
    """A whole hkaSplineCompressedAnimation: header, all blocks, per-frame evaluation."""

    def __init__(self, hk: HkPack):
        anim = next((o for o, c in hk.objects if c == 'hkaSplineCompressedAnimation'), None)
        if anim is None:
            raise ValueError('no hkaSplineCompressedAnimation')
        self.duration = hk.f32(anim + 16)
        self.n_tracks = hk.i32(anim + 20)
        self.n_float_tracks = hk.i32(anim + 24)
        self.num_frames = hk.i32(anim + 56)
        self.num_blocks = hk.i32(anim + 60)
        self.max_frames_per_block = hk.i32(anim + 64)
        self.mask_size = hk.i32(anim + 68)
        self.block_duration, self.block_inv_duration, self.frame_duration = hk.vec(anim + 72, 3)
        bo, nbo = hk.array(anim + 88)
        self.block_offsets = list(struct.unpack_from('<%dI' % nbo, hk.b, hk.base + bo)) if nbo else []
        fo, nfo = hk.array(anim + 104)
        self.float_block_offsets = list(struct.unpack_from('<%dI' % nfo, hk.b, hk.base + fo)) if nfo else []
        dp, dn = hk.array(anim + 152)
        self.data = bytes(hk.b[hk.base + dp:hk.base + dp + dn])
        # Block b starts at frame b * block_frames; the last frame of a block is the first of the next.
        self.block_frames = int(round(self.block_duration / self.frame_duration))
        if self.block_frames != self.max_frames_per_block - 1:
            raise ValueError(f'blockDuration {self.block_duration} is not {self.max_frames_per_block - 1} '
                             f'frames of {self.frame_duration}')
        if self.mask_size != 4 * self.n_tracks or self.n_float_tracks:
            raise ValueError(f'unexpected track layout (mask size {self.mask_size}, '
                             f'{self.n_tracks} transform / {self.n_float_tracks} float tracks)')
        if len(self.block_offsets) != self.num_blocks or not self.num_blocks:
            raise ValueError(f'{len(self.block_offsets)} block offsets for {self.num_blocks} blocks')
        self.fps = 1.0 / self.frame_duration
        self.blocks = []
        for b, start in enumerate(self.block_offsets):
            if start % 16:
                raise ValueError(f'block {b} offset {start} is not 16-aligned')
            tracks, end = parse_block(self.data, start, self.n_tracks)
            # floatBlockOffsets[b] = end of block b's transform data, relative to the block; the next
            # block starts at the next multiple of 16 (or the data ends).
            want = self.float_block_offsets[b] if b < len(self.float_block_offsets) else None
            nxt = self.block_offsets[b + 1] if b + 1 < self.num_blocks else len(self.data)
            if (want is not None and end - start != want) or _align(end, 16) != nxt:
                raise ValueError(f'block {b} parsed {end - start} bytes (floatBlockOffsets {want}), '
                                 f'ends at {end}, next block at {nxt}')
            self.blocks.append(tracks)
        # Track -> bone (hkaAnimationBinding.transformTrackToBoneIndices; empty = track i drives bone i).
        bind = next((o for o, c in hk.objects if c == 'hkaAnimationBinding'), None)
        self.track_bones = list(range(self.n_tracks))
        self.binding = False
        if bind is not None:
            tp, tn = hk.array(bind + 32)
            if tn:
                self.track_bones = list(struct.unpack_from('<%dh' % tn, hk.b, hk.base + tp))
                self.binding = True
        self.bone_track = {bone: k for k, bone in enumerate(self.track_bones)}
        self.rot_kinds = sorted({(self.data[o + 4 * i] >> 2) & 0xF
                                 for o in self.block_offsets for i in range(self.n_tracks)})

    def local(self, frame: int, track: int):
        """(translation, quaternion, scale) of a track at an integer frame."""
        b = min(frame // self.block_frames, self.num_blocks - 1)
        pos, rot, scl = self.blocks[b][track]
        f = float(frame - b * self.block_frames)
        return pos.at(f), rot.at(f), scl.at(f)

    def bone_local(self, frame: int, bone: int, skeleton) -> tuple:
        track = self.bone_track.get(bone)
        return self.local(frame, track) if track is not None else skeleton.ref[bone]


# ---- skeleton (Havok) -------------------------------------------------------------------------------------

class Skeleton:
    """hkaSkeleton of skeleton.HKX: bone names, parents, local reference pose (translation, quaternion, scale)."""

    def __init__(self, data: bytes):
        hk = HkPack(data)
        off = next(o for o, c in hk.objects if c == 'hkaSkeleton')
        pi, n = hk.array(off + 24)
        self.parents = list(struct.unpack_from('<%dh' % n, hk.b, hk.base + pi))
        bo, nb = hk.array(off + 40)
        self.names = [hk.cstr(bo + 16 * k) for k in range(nb)]
        ro, nr = hk.array(off + 56)
        self.ref = []
        for k in range(nr):
            v = hk.vec(ro + 48 * k, 12)
            self.ref.append((tuple(v[0:3]), tuple(v[4:8]), tuple(v[8:11])))
        if not (len(self.parents) == len(self.names) == len(self.ref)):
            raise ValueError('skeleton arrays disagree in length')
        self.index = {name: k for k, name in enumerate(self.names)}

    def chain(self, bone: int) -> list:
        out = []
        while bone >= 0:
            out.append(bone)
            bone = self.parents[bone]
        return out[::-1]

    def world(self, bone: int, local_of, chain=None):
        """hkQsTransform of `bone` in model space; local_of(k) -> local (t, q, s) of bone k."""
        w = None
        for k in (chain or self.chain(bone)):
            loc = local_of(k)
            w = loc if w is None else qs_mul(w, loc)
        return w


# ---- root motion: hkaDefaultAnimatedReferenceFrame ----------------------------------------------------------

class RootMotion:
    """The extracted motion: per-frame (x, y, z, yaw) in FromSoftware model space, relative to frame 0."""

    def __init__(self, hk: HkPack):
        rf = next((o for o, c in hk.objects if c == 'hkaDefaultAnimatedReferenceFrame'), None)
        self.present = rf is not None
        self.up, self.forward, self.duration = (0.0, 1.0, 0.0), (0.0, 0.0, 1.0), 0.0
        self.samples = []
        if rf is not None:
            self.up = tuple(hk.vec(rf + 16, 3))
            self.forward = tuple(hk.vec(rf + 32, 3))
            self.duration = hk.f32(rf + 48)
            ptr, n = hk.array(rf + 56)
            self.samples = [hk.vec(ptr + 16 * i, 4) for i in range(n)]

    def frame(self, i: int, frames: int):
        """(translation, yaw in radians about `up`) at frame i of `frames`, relative to frame 0 and
        expressed in the start heading (identity without extracted motion). There is one sample per
        frame; otherwise they are interpolated linearly over the duration, like Havok does."""
        if not self.samples:
            return (0.0, 0.0, 0.0), 0.0
        n = len(self.samples)
        x = i * (n - 1) / max(frames - 1, 1)
        k = min(int(x), max(n - 2, 0))
        u = x - k
        a, b = self.samples[k], self.samples[min(k + 1, n - 1)]
        s = [a[c] + (b[c] - a[c]) * u for c in range(4)]
        s0 = self.samples[0]
        d = (s[0] - s0[0], s[1] - s0[1], s[2] - s0[2])
        return quat_rotate(qaxis(self.up, -s0[3]), d), s[3] - s0[3]


# ---- the character: skeleton, Head bone, eye point, TAE ----------------------------------------------------

class Rig:
    """What every animation shares: the 108-bone skeleton, the Head chain, the eye offset, the TAE files."""

    def __init__(self, dvd: Path):
        chr_dir = dvd / 'chr'
        anibnd = souls.bnd4_read((chr_dir / 'c0000.anibnd.dcx').read_bytes())
        skel = next(f for f in anibnd if f.basename.lower() == 'skeleton.hkx')
        self.skeleton = sk = Skeleton(skel.data)
        self.tae_files = {f.basename: f.data for f in anibnd if f.basename.endswith('.tae')}
        self._tae = {}
        # The eye: dummy 58 of the player model, kept in the space of the bone it follows (FLVER bind pose).
        chrbnd = souls.bnd4_read((chr_dir / 'c0000.chrbnd.dcx').read_bytes())
        flver = souls.flver_read(next(f.data for f in chrbnd if f.basename.lower() == 'c0000.flver'),
                                 decode_vertices=False)
        body = model_dummies(flver, None)
        eyes = body['dummies'].get(EYE_DUMMY, [])
        if not eyes or any(e[3] != HEAD_BONE or e[4] is None for e in eyes):
            raise ValueError(f'dummy {EYE_DUMMY} is not attached to {HEAD_BONE}: {[e[3] for e in eyes]}')
        self.eye_dummies = len(eyes)
        self.eye_local = tuple(sum(e[4][k] for e in eyes) / len(eyes) for k in range(3))
        self.eye_bind_model = tuple(sum(e[0][k] for e in eyes) / len(eyes) for k in range(3))
        self.head = sk.index[HEAD_BONE]
        self.chain = sk.chain(self.head)
        self.head_rest = sk.world(self.head, lambda k: sk.ref[k], self.chain)
        self.head_rest_q = unit(self.head_rest[1])
        # Eye point at the reference pose: the Head bone's reference world transform carries the offset.
        self.eye_rest = qs_mul(self.head_rest, (self.eye_local, IDENT_Q, ONES))[0]
        # How well the Havok reference pose and the FLVER bind pose agree: bone origins of the common bones.
        flver_names = body['bones']
        worst, count = 0.0, 0
        for k, name in enumerate(sk.names):
            if name in flver_names:
                w = sk.world(k, lambda j: sk.ref[j])[0]
                f = body['worlds'][flver_names.index(name)]
                worst = max(worst, math.dist(w, (f[12], f[13], f[14])))
                count += 1
        self.bind_common_bones, self.bind_max_diff = count, worst
        # The head's own forward / up axes: the model's -Z / +Y at the reference pose, in Head space.
        inv = qconj(self.head_rest_q)
        self.head_fwd_local = quat_rotate(inv, (0.0, 0.0, -1.0))
        self.head_up_local = quat_rotate(inv, (0.0, 1.0, 0.0))

    def tae(self, group: str):
        """{anim id: [(type, start, end, params)]} of the TAE file for an animation group: a112 -> a112.tae,
        the player's own group a000 -> a00.tae."""
        key = next((k for k in (group + '.tae', group[:3] + '.tae') if k in self.tae_files), None)
        if key not in self._tae:
            self._tae[key] = {a: ev for a, ev in tae_read(self.tae_files[key])['anims']} if key else {}
        return self._tae[key]


def tae_summary(rig: Rig, name: str):
    """The type-0 (ChrActionFlag) events of an animation and its TAE length, or None without a TAE entry."""
    m = re.match(r'a(\d{3})_(\d{6})$', name)
    events = rig.tae(name[:4]).get(int(m.group(2))) if m else None
    if events is None:
        return None
    flags = []
    for ev_type, start, end, params in events:
        if ev_type == 0 and len(params) >= 4:
            flag = struct.unpack_from('<i', params)[0]
            e = {'flag': flag, 'start': rnd(start, 3), 'end': rnd(end, 3)}
            if flag in FLAG_NAMES:
                e['name'] = FLAG_NAMES[flag]
            flags.append(e)
    flags.sort(key=lambda e: (e['start'], e['flag']))
    return {'events': flags,
            'invincible': [[e['start'], e['end']] for e in flags if e['flag'] == INVINCIBLE],
            'length': rnd(max([end for _t, _s, end, _p in events] or [0.0]), 3),
            'event_count': len(events)}


# ---- one animation: samples and summary -----------------------------------------------------------------------

def sample_anim(rig: Rig, anim: SplineAnim, rm: RootMotion) -> dict:
    """Per frame, in sandbox space: root path, root yaw (radians), eye point, Head world orientation, the
    Head's pitch and tilt (degrees, from its forward / up axes in the model's own frame)."""
    sk = rig.skeleton
    out = {k: [] for k in ('root', 'yaw', 'eye', 'head_q', 'cam_q', 'pitch', 'tilt')}
    prev_h = prev_c = None
    for i in range(anim.num_frames):
        head = sk.world(rig.head, lambda k: anim.bone_local(i, k, sk), rig.chain)
        hq = unit(head[1])
        eye_model = qs_mul((head[0], hq, head[2]), (rig.eye_local, IDENT_Q, ONES))[0]
        rt, ry = rm.frame(i, anim.num_frames)
        rq = qaxis(rm.up, ry)
        eye_w = quat_rotate(rq, eye_model)
        eye_w = (eye_w[0] + rt[0], eye_w[1] + rt[1], eye_w[2] + rt[2])
        hw = unit(qmul(rq, hq))                       # Head world orientation: root yaw * model-space FK
        cw = unit(qmul(hw, qconj(rig.head_rest_q)))   # ... relative to the reference pose (identity at rest)
        ho, co = near(out_q(hw), prev_h), near(out_q(cw), prev_c)
        prev_h, prev_c = ho, co
        fwd = quat_rotate(hq, rig.head_fwd_local)
        up = quat_rotate(hq, rig.head_up_local)
        out['root'].append(out_pt(rt))
        out['yaw'].append(-ry)
        out['eye'].append(out_pt(eye_w))
        out['head_q'].append(ho)
        out['cam_q'].append(co)
        out['pitch'].append(math.degrees(math.asin(max(-1.0, min(1.0, fwd[1])))))
        out['tilt'].append(math.degrees(math.acos(max(-1.0, min(1.0, up[1])))))
    return out


def summarize(s: dict, fps: float) -> dict:
    n = len(s['root'])
    d = [s['root'][-1][k] - s['root'][0][k] for k in range(3)]
    dist_h = math.hypot(d[0], d[2])
    eye_y = [e[1] for e in s['eye']]
    i_min = min(range(n), key=lambda i: eye_y[i])
    speeds = [math.dist(s['root'][i], s['root'][i + 1]) * fps for i in range(n - 1)]
    moving = [i for i, v in enumerate(speeds) if v >= STILL_SPEED]
    turn = [quat_angle(s['head_q'][i], s['head_q'][i + 1]) * fps for i in range(n - 1)]
    heading = math.degrees(math.atan2(d[0], -d[2]))
    return {'displacement': rnd_list(d), 'distance_h': rnd(dist_h),
            'direction_deg': (180.0 if heading <= -179.995 else rnd(heading, 2)) if dist_h >= 0.1 else None,
            'eye_rest_y': rnd(eye_y[0]), 'eye_min_y': rnd(eye_y[i_min]), 'eye_max_y': rnd(max(eye_y)),
            't_eye_min': rnd(i_min / fps, 4),
            'root_max_speed': rnd(max(speeds, default=0.0), 3),
            't_motion_end': rnd((moving[-1] + 1) / fps if moving else 0.0, 4),
            'max_head_pitch_deg': rnd(max(abs(p) for p in s['pitch']), 2),
            'head_pitch_range_deg': [rnd(min(s['pitch']), 2), rnd(max(s['pitch']), 2)],
            'max_head_tilt_deg': rnd(max(s['tilt']), 2),
            'max_head_turn_dps': rnd(math.degrees(max(turn, default=0.0)), 1),
            'root_yaw_total_deg': rnd(math.degrees(s['yaw'][-1]), 2)}


def direction_name(deg):
    if deg is None:
        return 'in place'
    names = ['forward', 'forward-right', 'right', 'back-right', 'back', 'back-left', 'left', 'forward-left']
    return names[int(((deg % 360) + 22.5) // 45) % 8]


def guess_kind(anim_id: int, sm: dict, tae, has_root: bool, eye_last: float):
    """(kind, why): roll | quickstep | backstep | jump | other, from the measured path and the TAE flags.
    Dodge family = invincible window or an a00 7000s id, and a real displacement; then a roll is a
    somersault (head upside down or eye near the floor), a backstep a backward hop with the eye staying
    high, anything else a quickstep. A jump leaves the ground (eye above standing height) or drops."""
    inv = tae['invincible'] if tae else []
    d, dy = sm['distance_h'], sm['displacement'][1]
    inv_txt = ('; invincible ' + ', '.join(f'{a:.2f}-{b:.2f} s' for a, b in inv)) if inv else '; no invincible window'
    where = f"{d:.1f} m {direction_name(sm['direction_deg'])}"
    dip = f"eye dips to {sm['eye_min_y']:.2f} m at {sm['t_eye_min']:.2f} s, head tilt {sm['max_head_tilt_deg']:.0f} deg"
    low_start = ''
    if sm['eye_rest_y'] < 0.7:
        low_start = (f"; frame 0 already has the eye at {sm['eye_rest_y']:.2f} m: starts crouched or on the "
                     f"ground (probably a get-up roll)")
    elif sm['eye_rest_y'] < 1.0:
        low_start = f"; starts low (eye {sm['eye_rest_y']:.2f} m at frame 0)"
    if has_root and d >= 1.0 and sm['eye_max_y'] >= max(sm['eye_rest_y'], eye_last) + 0.25:
        return 'jump', (f"{where}; eye rises to {sm['eye_max_y']:.2f} m (standing {STAND_EYE:.1f}), "
                        f"leaves the ground" + inv_txt)
    if has_root and dy <= -0.5:
        return 'jump', f"root drops {-dy:.2f} m while moving {where}: jump down / fall" + inv_txt
    if (inv or anim_id in DODGE_IDS) and d >= 1.0:
        if sm['eye_min_y'] < 0.6 or sm['max_head_tilt_deg'] > 100:
            return 'roll', f"{where}; {dip} (somersault){low_start}" + inv_txt
        if abs(sm['direction_deg']) > 135 and sm['eye_min_y'] >= 1.0:
            return 'backstep', (f"{where}; eye stays above {sm['eye_min_y']:.2f} m, "
                                f"head tilt {sm['max_head_tilt_deg']:.0f} deg" + inv_txt)
        return 'quickstep', f"{where}; {dip}" + inv_txt
    if not has_root or d < 0.5:
        yaw = f"; yaw {sm['root_yaw_total_deg']:.0f} deg" if abs(sm['root_yaw_total_deg']) >= 10 else ''
        return 'other', ('in place' if not has_root or d < 0.1 else f'{d:.2f} m shift') + yaw + inv_txt
    return 'other', (f"{where} in {sm['t_motion_end']:.2f} s, top speed {sm['root_max_speed']:.1f} m/s; "
                     f"not an a00 7000s dodge" + inv_txt)


# ---- checks ---------------------------------------------------------------------------------------------------

def yaw_sign_check(rig: Rig, anim: SplineAnim, rm: RootMotion) -> dict:
    """Planted-foot test of the root yaw sign: a foot on the ground must not slide in the world.
    For each sign s, world foot = root + R_up(s * w) * model foot; the sum over frames of the smaller of
    the two ankle steps is small for the right sign (the stance foot stays put), large for the wrong one."""
    sk = rig.skeleton
    chains = {n: sk.chain(sk.index[n]) for n in ('L_Foot', 'R_Foot')}
    model = [{n: sk.world(sk.index[n], lambda k: anim.bone_local(i, k, sk), chains[n])[0] for n in chains}
             for i in range(anim.num_frames)]
    motion = [rm.frame(i, anim.num_frames) for i in range(anim.num_frames)]

    def world(i, n, sign):
        p = quat_rotate(qaxis(rm.up, sign * motion[i][1]), model[i][n])
        return (p[0] + motion[i][0][0], p[1] + motion[i][0][1], p[2] + motion[i][0][2])

    slide = {}
    for sign in (1, -1):
        slide[sign] = sum(min(math.dist(world(i, n, sign), world(i + 1, n, sign)) for n in chains)
                          for i in range(anim.num_frames - 1))
    return {'with_file_sign': rnd(slide[1], 3), 'with_flipped_sign': rnd(slide[-1], 3)}


def frame0_check(anim: SplineAnim) -> float:
    """Largest difference between this decoder's frame 0 (block 0) and hitbox_extract.spline_first_frame,
    over all tracks (translation, quaternion, scale)."""
    worst = 0.0
    for k, (t, q, s) in enumerate(spline_first_frame(anim.data, anim.n_tracks)):
        tt, qq, ss = anim.local(0, k)
        for a, b in ((t, tt), (q, qq), (s, ss)):
            worst = max(worst, max(abs(x - y) for x, y in zip(a, b)))
    return worst


def multiblock_check(dvd: Path) -> dict:
    """Seam test of the block layout on the animations with more than one block (hi bank): the last frame of
    block b - 1 and the first frame of block b must be the same pose."""
    path = dvd / 'chr' / BANKS['hi']
    if not path.is_file():
        return {'seams': 0}
    worst, count = 0.0, 0
    for f in souls.bnd4_read(path.read_bytes()):
        if not f.basename.endswith('.hkx'):
            continue
        try:
            a = SplineAnim(HkPack(f.data))
        except (ValueError, KeyError, struct.error):
            continue
        for b in range(1, a.num_blocks):
            count += 1
            for tr in range(a.n_tracks):
                p0, r0, _ = a.blocks[b - 1][tr]
                p1, r1, _ = a.blocks[b][tr]
                worst = max(worst, max(abs(x - y) for x, y in zip(p0.at(float(a.block_frames)), p1.at(0.0))),
                            1.0 - abs(sum(x * y for x, y in zip(r0.at(float(a.block_frames)), r1.at(0.0)))))
    return {'seams': count, 'worst_seam_diff': rnd(worst, 6)}


def convention_check(rig: Rig) -> dict:
    """Which way the model faces and where its left is, from the skeleton's own reference pose, and what
    that becomes in sandbox space (forward must be -Z, right +X)."""
    sk = rig.skeleton

    def pos(name):
        return sk.world(sk.index[name], lambda k: sk.ref[k])[0]
    fwd = [pos('L_Toe0')[k] - pos('L_Foot')[k] for k in range(3)]
    left = [pos('L_Thigh')[k] - pos('R_Thigh')[k] for k in range(3)]
    fwd_out = out_pt((fwd[0], 0.0, fwd[2]))
    right_out = out_pt((-left[0], 0.0, -left[2]))
    nf, nr = math.hypot(fwd_out[0], fwd_out[2]), math.hypot(right_out[0], right_out[2])
    return {'forward_model_raw_xz': rnd_list([fwd[0] / nf, fwd[2] / nf], 3),
            'forward_sandbox_xz': rnd_list([fwd_out[0] / nf, fwd_out[2] / nf], 3),
            'right_sandbox_xz': rnd_list([right_out[0] / nr, right_out[2] / nr], 3),
            'eye_z_from_head_origin_raw': rnd(rig.eye_bind_model[2] - pos(HEAD_BONE)[2], 4)}


# ---- JSON ---------------------------------------------------------------------------------------------------

_EXPONENT = re.compile(r'(?<![\w.\-"])-?\d+(?:\.\d+)?e[-+]?\d+(?![\w."])')


def dumps_doc(doc: dict) -> str:
    """JSON with one line per animation sample (compact, diff-friendly). Tiny numbers are written as plain
    decimals (3e-05 -> 0.00003) so that even a minimal JSON reader copes."""
    head = {k: v for k, v in doc.items() if k != 'anims'}
    text = json.dumps(head, indent=1, ensure_ascii=False)
    parts = []
    for a in doc['anims']:
        body = {k: v for k, v in a.items() if k != 'samples'}
        meta = json.dumps(body, ensure_ascii=False, separators=(',', ':'))
        rows = ',\n'.join(json.dumps(s, separators=(',', ':')) for s in a['samples'])
        parts.append(meta[:-1] + ',"samples":[\n' + rows + '\n]}')
    text = text[:-2] + ',\n "anims": [\n' + ',\n'.join(parts) + '\n]\n}\n'
    return _EXPONENT.sub(lambda m: format(decimal.Decimal(m.group(0)), 'f'), text)


def parse_ids(spec: str):
    """'all' or '7100,7400-7407' -> predicate on the 6-digit animation id."""
    if spec == 'all':
        return lambda i: True
    ranges = []
    for tok in spec.split(','):
        lo, _, hi = tok.strip().partition('-')
        ranges.append((int(lo), int(hi or lo)))
    return lambda i: any(a <= i <= b for a, b in ranges)


# ---- main -----------------------------------------------------------------------------------------------------

def run_checks(rig: Rig, dvd: Path, entries: list, records: list) -> dict:
    sk = rig.skeleton
    val = {}
    val['frame0_max_diff_vs_spline_first_frame'] = max(frame0_check(a) for _e, a, _r in records)
    val['frame0_pass'] = val['frame0_max_diff_vs_spline_first_frame'] < 1e-4
    val['reference_pose'] = {
        'eye_y': rnd(rig.eye_rest[1]), 'eye': rnd_list(rig.eye_rest),
        'flver_bind_eye_midpoint': rnd_list(rig.eye_bind_model),
        'fk_vs_flver_mm': rnd(math.dist(rig.eye_rest, rig.eye_bind_model) * 1000, 4),
        'bones_compared': rig.bind_common_bones,
        'havok_vs_flver_bone_origin_max_mm': rnd(rig.bind_max_diff * 1000, 4),
        'pass': abs(rig.eye_rest[1] - 1.654) < 0.005}
    anims = [a for _e, a, _r in records]
    val['tracks_vs_skeleton'] = {
        'skeleton_bones': len(sk.names),
        'transform_tracks': sorted({a.n_tracks for a in anims}),
        'binding_entries': sorted({len(a.track_bones) for a in anims}),
        'binding_present': all(a.binding for a in anims),
        'binding_is_identity': all(a.track_bones == list(range(a.n_tracks)) for a in anims),
        'distinct_bones_driven': sorted({len(set(a.track_bones)) for a in anims}),
        'max_bone_index': max(max(a.track_bones) for a in anims),
        'rotation_quantization_kinds': sorted({k for a in anims for k in a.rot_kinds}),
        'blocks': sorted({a.num_blocks for a in anims}),
        'duration_vs_frames_max_err_s': rnd(max(abs((a.num_frames - 1) * a.frame_duration - a.duration)
                                                for a in anims), 6),
        'pass': all(a.n_tracks == len(sk.names) == len(set(a.track_bones)) and max(a.track_bones) < len(sk.names)
                    for a in anims)}
    val['convention'] = convention_check(rig)
    cv = val['convention']
    val['convention']['pass'] = (cv['forward_sandbox_xz'] == [0.0, -1.0] and cv['right_sandbox_xz'] == [1.0, 0.0]
                                 and cv['eye_z_from_head_origin_raw'] < 0)
    turns = [(e['name'], a, r) for e, a, r in records if r.present and abs(e['summary']['root_yaw_total_deg']) >= 60]
    md = dvd / 'chr' / BANKS['md']
    if not turns and md.is_file():       # the selection has no turn: use the two 90 degree turns of the md bank
        for f in souls.bnd4_read(md.read_bytes()):
            if f.basename in ('a000_006000.hkx', 'a000_006001.hkx'):
                hk = HkPack(f.data)
                turns.append((f.basename[:-4], SplineAnim(hk), RootMotion(hk)))
    val['yaw_sign_planted_foot_slide_m'] = {name: yaw_sign_check(rig, a, r) for name, a, r in turns}
    val['yaw_sign_pass'] = bool(turns) and all(
        v['with_file_sign'] < v['with_flipped_sign'] for v in val['yaw_sign_planted_foot_slide_m'].values())
    rolls = [e for e in entries if e['kind_guess'] == 'roll']
    finals = [e['samples'][-1]['eye'][1] for e in entries if int(e['name'][5:]) in DODGE_IDS]
    sane = {'rolls': len(rolls),
            'roll_eye_min_y_range': [min((e['summary']['eye_min_y'] for e in rolls), default=None),
                                     max((e['summary']['eye_min_y'] for e in rolls), default=None)],
            'rolls_dipping_below_1m': sum(e['summary']['eye_min_y'] < 1.0 for e in rolls),
            'dodge_family_last_frame_eye_y_range': [rnd(min(finals, default=0.0), 3), rnd(max(finals, default=0.0), 3)]}
    lo = dvd / 'chr' / BANKS['lo']
    idle = next((f for f in souls.bnd4_read(lo.read_bytes()) if f.basename == 'a000_000000.hkx'), None) \
        if lo.is_file() else None
    if idle is not None:
        hk = HkPack(idle.data)
        ys = [e[1] for e in sample_anim(rig, SplineAnim(hk), RootMotion(hk))['eye']]
        sane['idle_a000_000000_eye_y_range'] = [rnd(min(ys), 3), rnd(max(ys), 3)]
    val['animation_sanity'] = sane
    val['multiblock_seams'] = multiblock_check(dvd)
    return val


def print_checks(val: dict, count: int):
    ok = lambda b: 'PASS' if b else 'FAIL'  # noqa: E731
    rp, tv, cv, an = val['reference_pose'], val['tracks_vs_skeleton'], val['convention'], val['animation_sanity']
    print('\nchecks')
    print(f"  1. frame 0 of {count} animations, all tracks, vs hitbox_extract.spline_first_frame: max diff "
          f"{val['frame0_max_diff_vs_spline_first_frame']:.2e}  {ok(val['frame0_pass'])}")
    print(f"  2. reference pose FK + dummy offset: eye y {rp['eye_y']} m (expect 1.654), "
          f"{rp['fk_vs_flver_mm']} mm from the FLVER eye; {rp['bones_compared']} bones Havok vs FLVER origin "
          f"max {rp['havok_vs_flver_bone_origin_max_mm']} mm  {ok(rp['pass'])}")
    print(f"  3. conventions: model faces raw (x, z) {cv['forward_model_raw_xz']} -> sandbox "
          f"{cv['forward_sandbox_xz']}, right {cv['right_sandbox_xz']}; eye z from the Head origin "
          f"{cv['eye_z_from_head_origin_raw']} m  {ok(cv['pass'])}")
    print(f"     yaw sign, planted-foot slide (m) with the file's sign vs flipped: "
          f"{val['yaw_sign_planted_foot_slide_m']}  {ok(val['yaw_sign_pass'])}")
    print(f"     sanity: {an}")
    print(f"  4. tracks vs skeleton: {tv['skeleton_bones']} bones; tracks {tv['transform_tracks']}, "
          f"binding entries {tv['binding_entries']} (present {tv['binding_present']}, "
          f"identity {tv['binding_is_identity']}), distinct bones {tv['distinct_bones_driven']}, "
          f"max index {tv['max_bone_index']}; rotation kinds {tv['rotation_quantization_kinds']}; "
          f"blocks {tv['blocks']}  {ok(tv['pass'])}")
    print(f"  multi-block seams (hi bank): {val['multiblock_seams']}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--game', type=Path, default=Path('C:/games/CUSA03173'), help='dump root (contains dvdroot_ps4)')
    ap.add_argument('--out', type=Path,
                    default=Path(__file__).resolve().parents[2] / 'out' / 'vr_assets' / 'dash')
    ap.add_argument('--bnd', default='md', help='animation banks to read: md, lo, hi (comma separated)')
    ap.add_argument('--anims', default='all', help="'all' or ids / ranges such as 7100,7400-7407")
    ap.add_argument('--no-validate', action='store_true', help='skip the checks (and the hi-bank seam test)')
    args = ap.parse_args(argv)
    game_root = args.game.resolve()
    out = args.out.resolve()
    if game_root in out.parents or out == game_root:
        raise SystemExit('refusing to write inside the game dump')
    dvd = game_root / 'dvdroot_ps4'
    if not (dvd / 'chr' / 'c0000.anibnd.dcx').is_file():
        raise SystemExit(f'not a Bloodborne dump (no dvdroot_ps4/chr/c0000.anibnd.dcx): {game_root}')
    banks = [b.strip() for b in args.bnd.split(',') if b.strip()]
    if not banks or any(b not in BANKS for b in banks):
        raise SystemExit(f'--bnd takes {", ".join(BANKS)}')
    try:
        wanted = parse_ids(args.anims)
    except ValueError:
        raise SystemExit(f"--anims takes 'all' or ids / ranges such as 7100,7400-7407, not {args.anims!r}")
    t0 = time.time()
    rig = Rig(dvd)
    entries, records = [], []
    for bank in banks:
        for f in sorted(souls.bnd4_read((dvd / 'chr' / BANKS[bank]).read_bytes()), key=lambda f: f.basename):
            m = re.match(r'a(\d{3})_(\d{6})\.hkx$', f.basename, re.I)
            if not m or not wanted(int(m.group(2))):
                continue
            name = f.basename[:-4]
            hk = HkPack(f.data)
            anim = SplineAnim(hk)
            rm = RootMotion(hk)
            s = sample_anim(rig, anim, rm)
            sm = summarize(s, anim.fps)
            tae = tae_summary(rig, name)
            kind, why = guess_kind(int(m.group(2)), sm, tae, rm.present, s['eye'][-1][1])
            samples = [{'t': rnd(i / anim.fps, 4), 'root': rnd_list(s['root'][i]),
                        'root_yaw_deg': rnd(math.degrees(s['yaw'][i]), 3), 'eye': rnd_list(s['eye'][i]),
                        'head_q': rnd_list(s['head_q'][i], 5), 'cam_q': rnd_list(s['cam_q'][i], 5)}
                       for i in range(anim.num_frames)]
            entries.append({'name': name, 'file': BANKS[bank], 'frames': anim.num_frames,
                            'fps': rnd(anim.fps, 3), 'duration': rnd((anim.num_frames - 1) / anim.fps, 4),
                            'kind_guess': kind, 'why': why, 'root_motion': rm.present, 'summary': sm,
                            'tae': tae, 'samples': samples})
            records.append((entries[-1], anim, rm))
    if not entries:
        raise SystemExit('no animation matched --bnd / --anims')

    print(f"{'name':12} {'fr':>3} {'dur':>5} {'dist_h':>6} {'dir':>7} {'eye_min':>7} {'t_end':>5}  "
          f"{'invincible':18} kind")
    for e in entries:
        sm = e['summary']
        inv = ' '.join(f'{a:.2f}-{b:.2f}' for a, b in e['tae']['invincible']) or '-' if e['tae'] else 'n/a'
        dirs = f"{sm['direction_deg']:7.1f}" if sm['direction_deg'] is not None else '      -'
        print(f"{e['name']:12} {e['frames']:3d} {e['duration']:5.2f} {sm['distance_h']:6.2f} {dirs} "
              f"{sm['eye_min_y']:7.2f} {sm['t_motion_end']:5.2f}  {inv:18} {e['kind_guess']}")

    val = {} if args.no_validate else run_checks(rig, dvd, entries, records)
    doc = {'format': FORMAT, 'version': VERSION, 'space': SPACE, 'eye': EYE,
           'conventions': {
               'root': 'root = accumulated extracted root motion (hkaDefaultAnimatedReferenceFrame), starts at the '
                       'origin; root_yaw_deg is the heading about +Y of the right-handed sandbox space, '
                       'positive = turning left (counter-clockwise seen from above)',
               'direction_deg': '0 = forward (-Z), +90 = right (+X), +-180 = back; null when the horizontal '
                                'displacement is below 0.1 m',
               'eye': 'FK through Head from the animated skeleton, plus the dummy offset in Head space, then the '
                      'root motion (yaw about +Y, then translation)',
               'head_q': 'Head bone world orientation (root yaw * model-space FK); the bone axes are not the model '
                         'axes: see head_q_rest and cam_q',
               'cam_q': 'head_q * conj(head_q_rest): identity at the reference pose; a camera looking along -Z '
                        'with +Y up follows the head when rotated by it',
               'tae': 'type-0 (ChrActionFlag) events in seconds; invincible = flag 8 windows; length = last event '
                      'time (events run to the TAE end, which can exceed the clip)',
               'pitch_tilt': 'max_head_pitch_deg = largest |elevation| of the head forward axis, tilt = angle of '
                             'the head up axis from +Y (180 = upside down)',
               'mirror': 'FromSoftware (x, y, z) -> (-x, y, z); quaternion (x, y, z, w) -> (x, -y, -z, w)'},
           'rig': {'skeleton': 'chr/c0000.anibnd.dcx skeleton.HKX', 'bones': len(rig.skeleton.names),
                   'head_bone': HEAD_BONE, 'head_bone_index': rig.head, 'eye_dummy': EYE_DUMMY,
                   'eye_dummy_instances': rig.eye_dummies,
                   'eye_offset_head_local': rnd_list(rig.eye_local, 5),
                   'eye_rest': rnd_list(out_pt(rig.eye_rest)),
                   'head_q_rest': rnd_list(out_q(rig.head_rest_q), 5)},
           'validation': val, 'anims': entries}
    out.mkdir(parents=True, exist_ok=True)
    path = out / 'dodges.json'
    path.write_text(dumps_doc(doc), encoding='utf-8', newline='\n')
    if val:
        print_checks(val, len(entries))
    print(f"\nwrote {len(entries)} animations ({sum(e['frames'] for e in entries)} frames) to {path} "
          f"in {time.time() - t0:.1f}s")
    failed = [k for k in ('frame0_pass', 'yaw_sign_pass') if val and not val[k]]
    failed += [k for k in ('reference_pose', 'tracks_vs_skeleton', 'convention') if val and not val[k]['pass']]
    if failed:
        print(f"CHECK FAILED: {', '.join(failed)}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
