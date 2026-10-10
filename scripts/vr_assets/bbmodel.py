"""The sandbox's model format ("bbmodel"): a JSON description plus a binary blob of vertices and
indices, written by extract.py and read by gpu/xr_bridge/sandbox3d (and by tests and previews).

Space: right-handed meters as OpenXR uses it, +X right, +Y up, -Z forward. FromSoftware models
are left-handed with the character facing -Z and its left side at +X, so X is mirrored on export
(positions, normals, bone and dummy transforms) and triangle winding is flipped to match. The
hunter then faces -Z with its left hand at -X, like a user looking down -Z.

Matrices are 4x4 column-major (OpenGL/Vulkan/GLM order: the translation is elements 12..14).

Vertex (stride 56): position f32x3, normal f32x3, uv f32x2, bones u16x4 (indices into "bones"),
weights f32x4 (sum 1). Indices: u32 triangle lists, counter-clockwise front faces.
"""
from __future__ import annotations

import json
import math
import struct
from dataclasses import dataclass, field
from pathlib import Path

FORMAT = 'bbmodel'
VERSION = 1
VERTEX = struct.Struct('<3f3f2f4H4f')
assert VERTEX.size == 56

# Row-major 4x4 (row-vector convention, SoulsFormats) -> mirrored X, column-major (column vectors).
# For a row-vector matrix W the column-vector matrix is W^T; mirrored: M W^T M with M = diag(-1,1,1,1).
# Flattened column-major that is the row-major flattening of M W M: negate the entries where
# exactly one of row/column is 0.
_MIRROR_SIGNS = tuple(-1.0 if (r == 0) != (c == 0) else 1.0 for r in range(4) for c in range(4))


def mirror_matrix(row_major_w) -> list:
    return [float(v) * s for v, s in zip(row_major_w, _MIRROR_SIGNS)]


def mirror(p) -> list:
    return [-float(p[0]), float(p[1]), float(p[2])]


@dataclass
class Model:
    name: str
    bones: list = field(default_factory=list)      # dicts: name, parent, bind_world, deform
    dummies: list = field(default_factory=list)    # dicts: ref, parent, attach, position...
    meshes: list = field(default_factory=list)     # dicts: name, part, material, ... ranges
    vertices: bytearray = field(default_factory=bytearray)
    indices: bytearray = field(default_factory=bytearray)
    extra: dict = field(default_factory=dict)

    @property
    def vertex_count(self) -> int:
        return len(self.vertices) // VERTEX.size

    def add_mesh(self, info: dict, vertices: list, triangles: list):
        """vertices: (pos, normal, uv, bones4, weights4) in the model's space already mirrored;
        triangles: flat index list into `vertices` (already in the output winding)."""
        base = self.vertex_count
        first_index = len(self.indices) // 4
        for pos, normal, uv, bones, weights in vertices:
            self.vertices += VERTEX.pack(*pos, *normal, *uv, *bones, *weights)
        self.indices += struct.pack(f'<{len(triangles)}I', *(base + i for i in triangles))
        info = dict(info)
        info.update(first_vertex=base, vertex_count=len(vertices), first_index=first_index,
                    index_count=len(triangles))
        self.meshes.append(info)

    def bounds(self):
        lo = [math.inf] * 3
        hi = [-math.inf] * 3
        for i in range(self.vertex_count):
            p = VERTEX.unpack_from(self.vertices, i * VERTEX.size)[:3]
            for k in range(3):
                lo[k] = min(lo[k], p[k])
                hi[k] = max(hi[k], p[k])
        return lo, hi

    def write(self, out_dir: Path) -> Path:
        out_dir.mkdir(parents=True, exist_ok=True)
        bin_name = f'{self.name}.bin'
        blob = bytes(self.vertices) + bytes(self.indices)
        (out_dir / bin_name).write_bytes(blob)
        lo, hi = self.bounds() if self.vertex_count else ([0] * 3, [0] * 3)
        doc = {
            'format': FORMAT, 'version': VERSION, 'name': self.name,
            'space': 'right-handed meters, +X right, +Y up, -Z forward (FromSoftware X mirrored)',
            'matrix_order': 'column-major',
            'bin': bin_name,
            'vertex_layout': {'stride': VERTEX.size, 'position': [0, 'f32x3'],
                              'normal': [12, 'f32x3'], 'uv': [24, 'f32x2'],
                              'bones': [32, 'u16x4'], 'weights': [40, 'f32x4']},
            'vertex_count': self.vertex_count,
            'vertices_offset': 0,
            'index_count': len(self.indices) // 4,
            'indices_offset': len(self.vertices),
            'bounds': {'min': lo, 'max': hi},
            'bones': self.bones,
            'dummies': self.dummies,
            'meshes': self.meshes,
        }
        doc.update(self.extra)
        path = out_dir / f'{self.name}.json'
        path.write_text(json.dumps(doc, indent=1, ensure_ascii=False), encoding='utf-8')
        return path


def load(path: Path) -> tuple[dict, bytes]:
    """(json document, binary blob) of a written model."""
    doc = json.loads(Path(path).read_text(encoding='utf-8'))
    blob = (Path(path).parent / doc['bin']).read_bytes()
    return doc, blob


def vertices_of(doc: dict, blob: bytes, mesh: dict | None = None):
    """Yields (pos, normal, uv, bones, weights) for a mesh (or the whole model)."""
    first = mesh['first_vertex'] if mesh else 0
    count = mesh['vertex_count'] if mesh else doc['vertex_count']
    for i in range(first, first + count):
        v = VERTEX.unpack_from(blob, doc['vertices_offset'] + i * VERTEX.size)
        yield v[0:3], v[3:6], v[6:8], v[8:12], v[12:16]


def indices_of(doc: dict, blob: bytes, mesh: dict) -> tuple:
    start = doc['indices_offset'] + mesh['first_index'] * 4
    return struct.unpack_from(f'<{mesh["index_count"]}I', blob, start)


# ---- Column-major 4x4 helpers (for previews and tests) -----------------------------------------

def mul(a, b):
    """Column-major a*b."""
    return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4)) for c in range(4) for r in range(4)]


def apply(m, p):
    x, y, z = p
    return (m[0] * x + m[4] * y + m[8] * z + m[12],
            m[1] * x + m[5] * y + m[9] * z + m[13],
            m[2] * x + m[6] * y + m[10] * z + m[14])


def apply_dir(m, d):
    x, y, z = d
    return (m[0] * x + m[4] * y + m[8] * z,
            m[1] * x + m[5] * y + m[9] * z,
            m[2] * x + m[6] * y + m[10] * z)


def inverse_rigid(m):
    """Inverse of a rotation+translation (+uniform scale) column-major matrix."""
    # rotation part transposed, scaled by 1/s^2
    s2 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2]
    inv_s2 = 1.0 / s2 if s2 > 1e-12 else 1.0
    r = [m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]]
    r = [x * inv_s2 for x in r]
    t = (m[12], m[13], m[14])
    it = (-(r[0] * t[0] + r[3] * t[1] + r[6] * t[2]),
          -(r[1] * t[0] + r[4] * t[1] + r[7] * t[2]),
          -(r[2] * t[0] + r[5] * t[1] + r[8] * t[2]))
    return [r[0], r[1], r[2], 0.0, r[3], r[4], r[5], 0.0, r[6], r[7], r[8], 0.0,
            it[0], it[1], it[2], 1.0]
