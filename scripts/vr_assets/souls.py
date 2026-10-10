"""FromSoftware container and model formats as Bloodborne (PS4) uses them, read-only, stdlib only.

    DCX     compressed wrapper: DFLT (zlib) is decompressed; KRAK (Oodle), ZSTD, EDGE and other
            types are detected and reported (DcxError says which).
    BND4    archive of named files (.partsbnd, .chrbnd, .anibnd, .parambnd, .msgbnd, ...).
    FLVER2  model: bones, dummies (reference points), materials, meshes (vertices, face sets).
    FMG     text table (item names).
    PARAMDEF / PARAM  game parameter tables (EquipParamWeapon, EquipParamProtector, ...).

Layouts follow SoulsFormats (MIT, JKAnderson) as far as Bloodborne uses them; only what the VR
sandbox needs is decoded. Nothing here writes into the game folder.
"""
from __future__ import annotations

import math
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path


class DcxError(Exception):
    """A DCX this module cannot decompress (unknown or unsupported compression)."""


# ---- DCX ----------------------------------------------------------------------------------------

@dataclass
class DcxInfo:
    kind: str               # "DFLT", "KRAK", "ZSTD", "EDGE", "none", ...
    uncompressed_size: int
    compressed_size: int
    level: int = 0


def dcx_info(data: bytes) -> DcxInfo:
    """What kind of DCX `data` is (kind "none" when it is not DCX-wrapped)."""
    if data[:4] != b'DCX\0':
        return DcxInfo('none', len(data), len(data))
    if data[4:8] == b'\0\0\0\0' and data[0x18:0x1c] != b'DCS\0':
        # DCX header variants all keep DCS at 0x18 in Bloodborne/DS3-era files.
        pass
    dcs = data.find(b'DCS\0', 4, 0x40)
    dcp = data.find(b'DCP\0', 4, 0x60)
    if dcs < 0 or dcp < 0:
        raise DcxError('DCX header without DCS/DCP blocks')
    uncompressed, compressed = struct.unpack_from('>II', data, dcs + 4)
    kind = data[dcp + 4:dcp + 8].decode('ascii', 'replace')
    level = data[dcp + 12] if len(data) > dcp + 12 else 0
    return DcxInfo(kind, uncompressed, compressed, level)


def dcx_decompress(data: bytes) -> bytes:
    """The payload of a DCX file (or `data` unchanged when it is not DCX)."""
    info = dcx_info(data)
    if info.kind == 'none':
        return data
    if info.kind != 'DFLT':
        raise DcxError(f'DCX compression {info.kind!r} is not supported '
                       f'({"Oodle Kraken: needs oo2core" if info.kind == "KRAK" else "unknown"}); '
                       f'{info.compressed_size} -> {info.uncompressed_size} bytes')
    dca = data.find(b'DCA\0', 0x20, 0x80)
    if dca < 0:
        raise DcxError('DFLT DCX without a DCA block')
    (dca_size,) = struct.unpack_from('>I', data, dca + 4)
    start = dca + dca_size
    out = zlib.decompress(data[start:start + info.compressed_size])
    if len(out) != info.uncompressed_size:
        raise DcxError(f'DCX size mismatch: {len(out)} != {info.uncompressed_size}')
    return out


def read_file(path: str | Path) -> bytes:
    """A game file's bytes, DCX-decompressed when wrapped."""
    return dcx_decompress(Path(path).read_bytes())


# ---- BND4 ---------------------------------------------------------------------------------------

@dataclass
class BndFile:
    id: int
    name: str
    flags: int
    data: bytes

    @property
    def basename(self) -> str:
        return self.name.replace('\\', '/').rsplit('/', 1)[-1]


def _reverse_bits(b: int) -> int:
    return int(f'{b:08b}'[::-1], 2)


def bnd4_read(data: bytes) -> list[BndFile]:
    """The files of a BND4 archive (DCX-decompressed first if needed)."""
    data = dcx_decompress(data)
    if data[:4] != b'BND4':
        raise ValueError(f'not BND4: {data[:4]!r}')
    big_endian = data[9] != 0
    bit_big_endian = data[10] == 0
    e = '>' if big_endian else '<'
    (count,) = struct.unpack_from(e + 'i', data, 0x0c)
    (header_size,) = struct.unpack_from(e + 'q', data, 0x10)
    entry_size, = struct.unpack_from(e + 'q', data, 0x20)
    unicode = data[0x30] != 0
    raw_format = data[0x31]
    reverse = bit_big_endian or ((raw_format & 1) != 0 and (raw_format & 0x80) == 0)
    fmt = raw_format if reverse else _reverse_bits(raw_format)
    has_ids = bool(fmt & 0x02)
    has_names = bool(fmt & 0x0c)
    long_offsets = bool(fmt & 0x10)
    compressed = bool(fmt & 0x20)
    files = []
    for i in range(count):
        at = header_size + i * entry_size
        flags = data[at]
        at += 8  # flags, 3 zero bytes, int -1
        (size,) = struct.unpack_from(e + 'q', data, at)
        at += 8
        if compressed:
            at += 8  # uncompressed size
        if long_offsets:
            (offset,) = struct.unpack_from(e + 'q', data, at)
            at += 8
        else:
            (offset,) = struct.unpack_from(e + 'I', data, at)
            at += 4
        file_id = -1
        if has_ids:
            (file_id,) = struct.unpack_from(e + 'i', data, at)
            at += 4
        name = ''
        if has_names:
            (name_offset,) = struct.unpack_from(e + 'I', data, at)
            name = _read_string(data, name_offset, unicode, big_endian)
        payload = data[offset:offset + size]
        if flags & 0x01 and payload[:4] == b'DCX\0':  # per-file compression (rare)
            payload = dcx_decompress(payload)
        files.append(BndFile(file_id, name, flags, payload))
    return files


def _read_string(data: bytes, offset: int, unicode: bool, big_endian: bool = False) -> str:
    if unicode:
        end = offset
        while data[end:end + 2] != b'\0\0':
            end += 2
        return data[offset:end].decode('utf-16-be' if big_endian else 'utf-16-le')
    end = data.index(b'\0', offset)
    return data[offset:end].decode('shift_jis', 'replace')


# ---- FLVER2 -------------------------------------------------------------------------------------

@dataclass
class Bone:
    name: str
    parent: int
    child: int
    next_sibling: int
    prev_sibling: int
    translation: tuple
    rotation: tuple   # Euler radians; local = S * Rx * Rz * Ry * T (row vectors)
    scale: tuple
    bbox_min: tuple
    bbox_max: tuple
    unk3c: int


@dataclass
class Dummy:
    position: tuple   # in the space of parent_bone (model space when -1)
    color: tuple
    forward: tuple
    reference_id: int
    parent_bone: int  # SoulsFormats ParentBoneIndex: the space the position is given in
    upward: tuple
    attach_bone: int  # SoulsFormats AttachBoneIndex: the bone it follows when animated
    flag1: int
    use_upward: bool
    unk30: int
    unk34: int


@dataclass
class Material:
    name: str
    mtd: str
    texture_index: int
    texture_count: int
    flags: int


@dataclass
class Texture:
    path: str
    type: str
    scale: tuple


@dataclass
class LayoutMember:
    unk00: int
    offset: int
    type: int
    semantic: int
    index: int


@dataclass
class FaceSet:
    flags: int
    strip: bool
    cull_backfaces: bool
    indices: list

    def triangles(self) -> list:
        """Triangle list (restarts and degenerate triangles dropped)."""
        idx = self.indices
        if not self.strip:
            return list(idx)
        out = []
        flip = False
        for i in range(len(idx) - 2):
            a, b, c = idx[i], idx[i + 1], idx[i + 2]
            if a == 0xFFFF or b == 0xFFFF or c == 0xFFFF or a == 0xFFFFFFFF:
                flip = False
                continue
            if a != b and b != c and a != c:
                out.extend((c, b, a) if flip else (a, b, c))
            flip = not flip
        return out


@dataclass
class Mesh:
    dynamic: bool
    material: int
    default_bone: int
    bone_indices: list      # mesh-local bone list (vertex bone indices index into it)
    face_sets: list
    buffer_indices: list
    # Decoded vertices (parallel lists).
    positions: list = field(default_factory=list)
    normals: list = field(default_factory=list)
    normal_w: list = field(default_factory=list)
    uvs: list = field(default_factory=list)
    bone_ids: list = field(default_factory=list)       # 4 per vertex, as stored
    bone_weights: list = field(default_factory=list)   # 4 per vertex
    colors: list = field(default_factory=list)
    layout_summary: str = ''


@dataclass
class Flver:
    version: int
    big_endian: bool
    bbox_min: tuple
    bbox_max: tuple
    bones: list
    dummies: list
    materials: list
    textures: list
    meshes: list
    layouts: list


# Vertex layout types (FLVER.LayoutType) and semantics (FLVER.LayoutSemantic).
LT_FLOAT2, LT_FLOAT3, LT_FLOAT4 = 0x01, 0x02, 0x03
LT_BYTE4A, LT_BYTE4B, LT_SHORT2_FLOAT2, LT_BYTE4C = 0x10, 0x11, 0x12, 0x13
LT_UV, LT_UVPAIR, LT_SHORT_BONES = 0x15, 0x16, 0x18
LT_SHORT4_FLOAT4A, LT_SHORT4_FLOAT4B, LT_BYTE4E = 0x1A, 0x2E, 0x2F
LT_SIZE = {LT_FLOAT2: 8, LT_FLOAT3: 12, LT_FLOAT4: 16, LT_BYTE4A: 4, LT_BYTE4B: 4,
           LT_SHORT2_FLOAT2: 4, LT_BYTE4C: 4, LT_UV: 4, LT_UVPAIR: 8, LT_SHORT_BONES: 8,
           LT_SHORT4_FLOAT4A: 8, LT_SHORT4_FLOAT4B: 8, LT_BYTE4E: 4}
SEM_POSITION, SEM_WEIGHTS, SEM_BONES, SEM_NORMAL = 0, 1, 2, 3
SEM_UV, SEM_TANGENT, SEM_BITANGENT, SEM_COLOR = 5, 6, 7, 10
SEM_NAMES = {0: 'pos', 1: 'weights', 2: 'bones', 3: 'normal', 5: 'uv', 6: 'tangent',
             7: 'bitangent', 10: 'color'}

# Face set flags.
FS_LOD1, FS_LOD2, FS_EDGE, FS_MOTION_BLUR = 0x01000000, 0x02000000, 0x40000000, 0x80000000


def flver_read(data: bytes, decode_vertices: bool = True) -> Flver:
    """Parses a FLVER2 model (DCX-decompressed first if needed)."""
    data = dcx_decompress(data)
    if data[:6] != b'FLVER\0':
        raise ValueError(f'not FLVER: {data[:6]!r}')
    big_endian = data[6:8] == b'B\0'
    e = '>' if big_endian else '<'

    def u(fmt, at):
        return struct.unpack_from(e + fmt, data, at)

    (version, data_offset, _data_length, dummy_count, material_count, bone_count, mesh_count,
     buffer_count) = u('8i', 8)
    bbox_min = u('3f', 0x28)
    bbox_max = u('3f', 0x34)
    index_size_header = data[0x48]
    unicode = data[0x49] != 0
    (face_set_count, layout_count, texture_count) = u('3i', 0x50)
    at = 0x80

    def string(offset):
        return _read_string(data, offset, unicode, big_endian) if offset > 0 else ''

    dummies = []
    for _ in range(dummy_count):
        pos = u('3f', at)
        color = tuple(data[at + 12:at + 16])
        fwd = u('3f', at + 16)
        ref_id, parent = u('hh', at + 28)
        up = u('3f', at + 32)
        attach, = u('h', at + 44)
        flag1 = data[at + 46]
        use_up = data[at + 47] != 0
        unk30, unk34 = u('ii', at + 48)
        dummies.append(Dummy(pos, color, fwd, ref_id, parent, up, attach, flag1, use_up, unk30,
                             unk34))
        at += 0x40

    materials = []
    for _ in range(material_count):
        name_off, mtd_off, tex_count, tex_index, flags = u('5i', at)
        materials.append(Material(string(name_off), string(mtd_off), tex_index, tex_count, flags))
        at += 0x20

    bones = []
    for _ in range(bone_count):
        t = u('3f', at)
        name_off, = u('i', at + 12)
        r = u('3f', at + 16)
        parent, child = u('hh', at + 28)
        s = u('3f', at + 32)
        nxt, prv = u('hh', at + 44)
        bmin = u('3f', at + 48)
        unk3c, = u('i', at + 60)
        bmax = u('3f', at + 64)
        bones.append(Bone(string(name_off), parent, child, nxt, prv, t, r, s, bmin, bmax, unk3c))
        at += 0x80

    mesh_headers = []
    for _ in range(mesh_count):
        dynamic = data[at] != 0
        material, = u('i', at + 4)
        default_bone, bone_n, _bbox_off, bone_off, fs_n, fs_off, vb_n, vb_off = u('8i', at + 16)
        mesh_headers.append((dynamic, material, default_bone, bone_n, bone_off, fs_n, fs_off,
                             vb_n, vb_off))
        at += 0x30

    face_sets = []
    for _ in range(face_set_count):
        flags, = u('I', at)
        strip = data[at + 4] != 0
        cull = data[at + 5] != 0
        index_count, indices_off = u('ii', at + 8)
        index_size = 0
        if version > 0x20005:
            _length, _zero, index_size = u('iii', at + 16)
        index_size = index_size or index_size_header or 16
        start = data_offset + indices_off
        if index_size == 16:
            indices = list(u(f'{index_count}H', start))
        elif index_size == 32:
            indices = list(u(f'{index_count}I', start))
        else:
            raise ValueError(f'index size {index_size}')
        face_sets.append(FaceSet(flags, strip, cull, indices))
        at += 0x20

    buffers = []
    for _ in range(buffer_count):
        buffer_index, layout_index, vertex_size, vertex_count = u('4i', at)
        _length, buffer_off = u('ii', at + 24)
        buffers.append((buffer_index, layout_index, vertex_size, vertex_count,
                        data_offset + buffer_off))
        at += 0x20

    layouts = []
    for _ in range(layout_count):
        member_count, _z1, _z2, members_off = u('4i', at)
        members = []
        for m in range(member_count):
            unk00, offset, mtype, sem, index = u('5i', members_off + m * 0x14)
            members.append(LayoutMember(unk00, offset, mtype, sem, index))
        layouts.append(members)
        at += 0x10

    textures = []
    for _ in range(texture_count):
        path_off, type_off = u('ii', at)
        scale = u('2f', at + 8)
        textures.append(Texture(string(path_off), string(type_off), scale))
        at += 0x20

    uv_factor = 2048.0 if version >= 0x2000F else 1024.0
    meshes = []
    for (dynamic, material, default_bone, bone_n, bone_off, fs_n, fs_off, vb_n,
         vb_off) in mesh_headers:
        bone_list = list(u(f'{bone_n}i', bone_off)) if bone_n else []
        fs_ids = list(u(f'{fs_n}i', fs_off)) if fs_n else []
        vb_ids = list(u(f'{vb_n}i', vb_off)) if vb_n else []
        mesh = Mesh(dynamic, material, default_bone, bone_list, [face_sets[i] for i in fs_ids],
                    vb_ids)
        if decode_vertices:
            _decode_vertices(mesh, buffers, layouts, data, e, uv_factor)
        meshes.append(mesh)
    return Flver(version, big_endian, bbox_min, bbox_max, bones, dummies, materials, textures,
                 meshes, layouts)


def _decode_vertices(mesh: Mesh, buffers, layouts, data: bytes, e: str, uv_factor: float):
    count = None
    summary = []
    for vb in mesh.buffer_indices:
        _bi, layout_index, vsize, vcount, start = buffers[vb]
        if count is None:
            count = vcount
            mesh.positions = [(0.0, 0.0, 0.0)] * vcount
            mesh.normals = [(0.0, 0.0, 1.0)] * vcount
            mesh.normal_w = [0] * vcount
            mesh.uvs = [(0.0, 0.0)] * vcount
            mesh.bone_ids = [(0, 0, 0, 0)] * vcount
            mesh.bone_weights = [(1.0, 0.0, 0.0, 0.0)] * vcount
            mesh.colors = [(1.0, 1.0, 1.0, 1.0)] * vcount
        members = layouts[layout_index]
        summary.append('+'.join(f'{SEM_NAMES.get(m.semantic, m.semantic)}{m.index}:{m.type:#x}'
                                for m in members))
        for m in members:
            _decode_member(mesh, m, data, e, start, vsize, vcount, uv_factor)
    mesh.layout_summary = ' | '.join(summary)


def _decode_member(mesh, m, data, e, start, vsize, count, uv_factor):
    t, sem = m.type, m.semantic
    offs = [start + i * vsize + m.offset for i in range(count)]

    def read(fmt):
        st = struct.Struct(e + fmt)
        return [st.unpack_from(data, o) for o in offs]

    if sem == SEM_POSITION:
        if t in (LT_FLOAT3, LT_FLOAT4):
            mesh.positions = [v[:3] for v in read('3f')]
        elif t == LT_SHORT4_FLOAT4B:  # unusual for positions
            mesh.positions = [tuple((x - 32767) / 32767.0 for x in v[:3]) for v in read('4H')]
    elif sem == SEM_NORMAL:
        if t in (LT_FLOAT3,):
            mesh.normals = read('3f')
        elif t == LT_FLOAT4:
            vals = read('4f')
            mesh.normals = [v[:3] for v in vals]
            mesh.normal_w = [int(v[3]) for v in vals]
        elif t in (LT_BYTE4A, LT_BYTE4B, LT_BYTE4C, LT_BYTE4E):
            vals = read('4B')
            mesh.normals = [((a - 127) / 127.0, (b - 127) / 127.0, (c - 127) / 127.0)
                            for a, b, c, _ in vals]
            mesh.normal_w = [w for *_, w in vals]
        elif t == LT_SHORT4_FLOAT4A:
            vals = read('4h')
            mesh.normals = [(a / 32767.0, b / 32767.0, c / 32767.0) for a, b, c, _ in vals]
            mesh.normal_w = [w for *_, w in vals]
        elif t == LT_SHORT4_FLOAT4B:
            vals = read('4H')
            mesh.normals = [((a - 32767) / 32767.0, (b - 32767) / 32767.0, (c - 32767) / 32767.0)
                            for a, b, c, _ in vals]
            mesh.normal_w = [w for *_, w in vals]
    elif sem == SEM_UV and m.index == 0:
        if t == LT_FLOAT2:
            mesh.uvs = read('2f')
        elif t in (LT_FLOAT3,):
            mesh.uvs = [v[:2] for v in read('3f')]
        elif t in (LT_SHORT2_FLOAT2, LT_UV):
            mesh.uvs = [(a / uv_factor, b / uv_factor) for a, b in read('2h')]
        elif t == LT_UVPAIR:
            mesh.uvs = [(a / uv_factor, b / uv_factor) for a, b, _, _ in read('4h')]
        elif t == LT_SHORT4_FLOAT4B:
            mesh.uvs = [(a / uv_factor, b / uv_factor) for a, b, _, _ in read('4h')]
    elif sem == SEM_BONES:
        if t in (LT_BYTE4B, LT_BYTE4E, LT_BYTE4A):
            mesh.bone_ids = read('4B')
        elif t == LT_SHORT_BONES:
            mesh.bone_ids = read('4H')
    elif sem == SEM_WEIGHTS:
        if t == LT_BYTE4A:
            mesh.bone_weights = [tuple(x / 127.0 for x in v) for v in read('4b')]
        elif t == LT_BYTE4C:
            mesh.bone_weights = [tuple(x / 255.0 for x in v) for v in read('4B')]
        elif t in (LT_UVPAIR, LT_SHORT4_FLOAT4A):
            mesh.bone_weights = [tuple(x / 32767.0 for x in v) for v in read('4h')]
        elif t == LT_FLOAT4:
            mesh.bone_weights = read('4f')
    elif sem == SEM_COLOR and m.index == 0:
        if t == LT_FLOAT4:
            mesh.colors = read('4f')
        elif t in (LT_BYTE4A, LT_BYTE4C):
            mesh.colors = [tuple(x / 255.0 for x in v) for v in read('4B')]


# ---- Bone math ------------------------------------------------------------------------------------
# Row-vector 4x4 matrices (v' = v * M) as flat 16-tuples, like System.Numerics in SoulsFormats.

def mat_mul(a, b):
    return tuple(sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4))
                 for r in range(4) for c in range(4))


def mat_scale(s):
    return (s[0], 0, 0, 0, 0, s[1], 0, 0, 0, 0, s[2], 0, 0, 0, 0, 1)


def mat_translate(t):
    return (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t[0], t[1], t[2], 1)


def mat_rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return (1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1)


def mat_rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return (c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1)


def mat_rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return (c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)


def bone_local(bone: Bone):
    """SoulsFormats FLVER.Bone.ComputeLocalTransform: S * Rx * Rz * Ry * T."""
    m = mat_scale(bone.scale)
    m = mat_mul(m, mat_rot_x(bone.rotation[0]))
    m = mat_mul(m, mat_rot_z(bone.rotation[2]))
    m = mat_mul(m, mat_rot_y(bone.rotation[1]))
    return mat_mul(m, mat_translate(bone.translation))


def bone_worlds(bones: list) -> list:
    """Bind-pose model-space transform of every bone (local * parent world)."""
    worlds = [None] * len(bones)

    def world(i):
        if worlds[i] is None:
            local = bone_local(bones[i])
            p = bones[i].parent
            worlds[i] = mat_mul(local, world(p)) if 0 <= p < len(bones) and p != i else local
        return worlds[i]

    for i in range(len(bones)):
        world(i)
    return worlds


def transform_point(m, p):
    x, y, z = p
    return (x * m[0] + y * m[4] + z * m[8] + m[12],
            x * m[1] + y * m[5] + z * m[9] + m[13],
            x * m[2] + y * m[6] + z * m[10] + m[14])


def transform_dir(m, d):
    x, y, z = d
    return (x * m[0] + y * m[4] + z * m[8],
            x * m[1] + y * m[5] + z * m[9],
            x * m[2] + y * m[6] + z * m[10])


def dummy_model(dummy: Dummy, worlds: list):
    """A dummy's bind-pose position, forward and upward in model space."""
    if 0 <= dummy.parent_bone < len(worlds):
        m = worlds[dummy.parent_bone]
        return (transform_point(m, dummy.position), transform_dir(m, dummy.forward),
                transform_dir(m, dummy.upward))
    return dummy.position, dummy.forward, dummy.upward


# ---- FMG (text) -----------------------------------------------------------------------------------

def fmg_read(data: bytes) -> dict:
    """id -> text of an FMG (version 1/2 as Bloodborne uses; little endian, UTF-16)."""
    data = dcx_decompress(data)
    big_endian = data[1] == 0xFF if len(data) > 1 else False
    e = '>' if big_endian else '<'
    version = data[2]
    wide = version >= 2
    if wide:
        (_size, _unk, group_count, string_count, _unk2, string_offsets) = struct.unpack_from(
            e + 'iiiiiq', data, 4)
        group_at, group_size = 0x28, 0x10
    else:
        (_size, _unk, group_count, string_count, string_offsets) = struct.unpack_from(
            e + 'iiiii', data, 4)
        string_offsets = struct.unpack_from(e + 'i', data, 0x18)[0]
        group_at, group_size = 0x1c, 0x0c
    out = {}
    for g in range(group_count):
        at = group_at + g * group_size
        if wide:
            offset_index, first_id, last_id = struct.unpack_from(e + 'iii', data, at)
        else:
            offset_index, first_id, last_id = struct.unpack_from(e + 'iii', data, at)
        for k, text_id in enumerate(range(first_id, last_id + 1)):
            idx = offset_index + k
            if wide:
                (off,) = struct.unpack_from(e + 'q', data, string_offsets + idx * 8)
            else:
                (off,) = struct.unpack_from(e + 'i', data, string_offsets + idx * 4)
            if off > 0:
                out[text_id] = _read_string(data, off, True, big_endian)
    return out


# ---- PARAMDEF / PARAM -------------------------------------------------------------------------------

@dataclass
class ParamField:
    name: str          # internal name (e.g. "equipModelId")
    display: str
    type: str          # display type ("u16", "s32", "f32", "dummy8", ...)
    internal: str      # internal type
    count: int         # array length for dummy8/fixstr, bits for bit fields
    bits: int          # bit field width (0 when not a bit field)


@dataclass
class ParamDef:
    param_type: str
    fields: list


_TYPE_SIZES = {'s8': 1, 'u8': 1, 's16': 2, 'u16': 2, 's32': 4, 'u32': 4, 'f32': 4,
               'b32': 4, 'angle32': 4, 'f64': 8, 'dummy8': 1, 'fixstr': 1, 'fixstrW': 2}
_TYPE_FMT = {'s8': 'b', 'u8': 'B', 's16': 'h', 'u16': 'H', 's32': 'i', 'u32': 'I', 'f32': 'f',
             'b32': 'i', 'angle32': 'f', 'f64': 'd'}


def paramdef_read(data: bytes) -> ParamDef:
    """A PARAMDEF. Bloodborne uses format version 201: 0xD0-byte fields with an inline UTF-16
    display name, display type at +0x40, byte count at +0x64, internal type at +0x70 and internal
    name at +0x90 ("name", "name:bits" or "name[count]")."""
    data = dcx_decompress(data)
    e = '>' if data[0x2c] == 0xFF else '<'
    (_file_size, header_size, _data_version, field_count, field_size) = struct.unpack_from(
        e + 'ihhhh', data, 0)
    unicode = data[0x2d] != 0
    (version,) = struct.unpack_from(e + 'h', data, 0x2e)
    param_type = data[0x0c:0x2c].split(b'\0', 1)[0].decode('ascii', 'replace')
    if version >= 200:
        (at,) = struct.unpack_from(e + 'q', data, 0x30)
    else:
        at = header_size

    def fixstr(start, size):
        return data[start:start + size].split(b'\0', 1)[0].decode('ascii', 'replace')

    fields = []
    for _ in range(field_count):
        raw = data[at:at + 0x40]
        display = (raw.decode('utf-16-le', 'replace') if unicode else
                   raw.decode('shift_jis', 'replace')).split('\0', 1)[0]
        disp_type = fixstr(at + 0x40, 8)
        if version >= 200:
            internal_type = fixstr(at + 0x70, 0x20)
            name = fixstr(at + 0x90, 0x20)
        else:  # 103/104: 32-bit description offset, the rest shifted by 4
            internal_type = fixstr(at + 0x6c, 0x20)
            name = fixstr(at + 0x8c, 0x20)
        count = 1
        bits = 0
        if ':' in name:
            name, bit_text = name.split(':', 1)
            bits = int(bit_text.strip())
        if '[' in name:
            name, arr = name.split('[', 1)
            count = int(arr.rstrip(']'))
        fields.append(ParamField(name.strip(), display, disp_type, internal_type, count, bits))
        at += field_size
    return ParamDef(param_type, fields)


def param_read(data: bytes) -> tuple[str, list]:
    """(param type, [(row id, row name, row bytes)]) of a PARAM file (Bloodborne: 64-bit row
    offsets, rows of id + pad, data offset, name offset)."""
    data = dcx_decompress(data)
    e = '<'
    (strings_offset, _data_start, _unk06, _def_version, row_count) = struct.unpack_from(
        e + 'IHhhH', data, 0)
    param_type = data[0x0c:0x2c].split(b'\0', 1)[0].decode('ascii', 'replace')
    format2d = data[0x2d]
    format2e = data[0x2e]
    long_offsets = bool(format2d & 0x04)
    row_at = 0x40 if long_offsets else 0x30
    entries = []
    for i in range(row_count):
        if long_offsets:
            row_id, data_off, name_off = struct.unpack_from(e + 'i4xqq', data, row_at + i * 0x18)
        else:
            row_id, data_off, name_off = struct.unpack_from(e + 'iII', data, row_at + i * 0x0c)
        entries.append((row_id, data_off, name_off))
    data_offsets = sorted({o[1] for o in entries})
    row_size = min((b - a for a, b in zip(data_offsets, data_offsets[1:]) if b > a),
                   default=strings_offset - (data_offsets[0] if data_offsets else 0))
    rows = []
    for row_id, data_off, name_off in entries:
        name = ''
        if 0 < name_off < len(data):
            if format2e & 0x01:
                name = _read_string(data, name_off, True)
            else:
                end = data.find(b'\0', name_off)
                name = data[name_off:end].decode('shift_jis', 'replace')
        rows.append((row_id, name, data[data_off:data_off + row_size]))
    return param_type, rows


def param_field_offsets(paramdef: ParamDef) -> dict:
    """name -> (byte offset, struct format or None, bit offset, bits) for a paramdef's row.
    Bit fields pack into units of their type (dummy8 bits count as u8, as in SoulsFormats); a
    new unit starts when the type changes or the bits do not fit."""
    out = {}
    byte = 0
    bit = 0       # bits used in the current bit-field unit
    unit = None   # the type of the current bit-field unit
    for f in paramdef.fields:
        t = f.type if f.type in _TYPE_SIZES else f.internal
        if f.bits:
            t = 'u8' if t == 'dummy8' else t
            size = _TYPE_SIZES.get(t, 1)
            if unit != t or bit + f.bits > size * 8:
                if unit is not None:
                    byte += _TYPE_SIZES[unit]
                unit, bit = t, 0
            out[f.name] = (byte, _TYPE_FMT.get(t), bit, f.bits)
            bit += f.bits
            continue
        if unit is not None:
            byte += _TYPE_SIZES[unit]
            unit, bit = None, 0
        out[f.name] = (byte, _TYPE_FMT.get(t) if f.count == 1 else None, 0, 0)
        byte += _TYPE_SIZES.get(t, 0) * f.count
    return out


def param_row_size(paramdef: ParamDef) -> int:
    """The row size the paramdef describes (checks param_field_offsets against the file)."""
    layout = param_field_offsets(paramdef)
    end = 0
    for f in paramdef.fields:
        byte, _fmt, _bit, bits = layout.get(f.name, (0, None, 0, 0))
        t = f.type if f.type in _TYPE_SIZES else f.internal
        if bits:
            t = 'u8' if t == 'dummy8' else t
            end = max(end, byte + _TYPE_SIZES.get(t, 1))
        else:
            end = max(end, byte + _TYPE_SIZES.get(t, 0) * f.count)
    return end


def param_table(params: list, paramdefs: list, name: str) -> list:
    """Rows of the named param (e.g. "EquipParamWeapon") as dicts of field -> value, with "id"
    and "row_name". `params`/`paramdefs` are the BndFile lists of the two archives."""
    param = next(p for p in params if p.basename.split('.')[0] == name)
    param_type, rows = param_read(param.data)
    pdef = next(d for d in (paramdef_read(x.data) for x in paramdefs)
                if d.param_type == param_type)
    layout = param_field_offsets(pdef)
    if rows and param_row_size(pdef) != len(rows[0][2]):
        raise ValueError(f'{name}: paramdef row size {param_row_size(pdef)} != '
                         f'file row size {len(rows[0][2])}')
    out = []
    for row_id, row_name, row in rows:
        values = {'id': row_id, 'row_name': row_name}
        for field_name, field_layout in layout.items():
            value = param_value(row, field_layout)
            if value is not None:
                values[field_name] = value
        out.append(values)
    return out


def param_value(row: bytes, layout: tuple):
    byte, fmt, bit, bits = layout
    if fmt is None or byte + struct.calcsize('<' + fmt) > len(row):
        return None
    (value,) = struct.unpack_from('<' + fmt, row, byte)
    if bits:
        value = (value >> bit) & ((1 << bits) - 1)
    return value
