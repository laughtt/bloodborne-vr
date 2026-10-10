#!/usr/bin/env python3
"""Wireframe previews of extracted models as PNG (stdlib only), to check an extraction without the
headset: python scripts/vr_assets/preview.py [out/vr_assets]

    preview/hunter_front.png, _side.png, _top.png  body vertices (grey dots), skeleton (spine
        white, left arm cyan, right arm red, fingers yellow, legs green), dummy points on deform
        bones (green), and the first weapon in the right hand at the R_Weapon bone (orange)
    preview/<weapon>.png  each weapon (front and side), dummies labelled with their reference ids
"""
from __future__ import annotations

import json
import math
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bbmodel  # noqa: E402

FONT = {
    '0': (0x3E, 0x51, 0x49, 0x45, 0x3E), '1': (0x00, 0x42, 0x7F, 0x40, 0x00),
    '2': (0x42, 0x61, 0x51, 0x49, 0x46), '3': (0x21, 0x41, 0x45, 0x4B, 0x31),
    '4': (0x18, 0x14, 0x12, 0x7F, 0x10), '5': (0x27, 0x45, 0x45, 0x45, 0x39),
    '6': (0x3C, 0x4A, 0x49, 0x49, 0x30), '7': (0x01, 0x71, 0x09, 0x05, 0x03),
    '8': (0x36, 0x49, 0x49, 0x49, 0x36), '9': (0x06, 0x49, 0x49, 0x29, 0x1E),
    'A': (0x7E, 0x11, 0x11, 0x11, 0x7E), 'B': (0x7F, 0x49, 0x49, 0x49, 0x36),
    'C': (0x3E, 0x41, 0x41, 0x41, 0x22), 'D': (0x7F, 0x41, 0x41, 0x41, 0x3E),
    'E': (0x7F, 0x49, 0x49, 0x49, 0x41), 'F': (0x7F, 0x09, 0x09, 0x09, 0x01),
    'G': (0x3E, 0x41, 0x49, 0x49, 0x7A), 'H': (0x7F, 0x08, 0x08, 0x08, 0x7F),
    'I': (0x00, 0x41, 0x7F, 0x41, 0x00), 'J': (0x20, 0x40, 0x41, 0x3F, 0x01),
    'K': (0x7F, 0x08, 0x14, 0x22, 0x41), 'L': (0x7F, 0x40, 0x40, 0x40, 0x40),
    'M': (0x7F, 0x02, 0x0C, 0x02, 0x7F), 'N': (0x7F, 0x04, 0x08, 0x10, 0x7F),
    'O': (0x3E, 0x41, 0x41, 0x41, 0x3E), 'P': (0x7F, 0x09, 0x09, 0x09, 0x06),
    'Q': (0x3E, 0x41, 0x51, 0x21, 0x5E), 'R': (0x7F, 0x09, 0x19, 0x29, 0x46),
    'S': (0x46, 0x49, 0x49, 0x49, 0x31), 'T': (0x01, 0x01, 0x7F, 0x01, 0x01),
    'U': (0x3F, 0x40, 0x40, 0x40, 0x3F), 'V': (0x1F, 0x20, 0x40, 0x20, 0x1F),
    'W': (0x3F, 0x40, 0x38, 0x40, 0x3F), 'X': (0x63, 0x14, 0x08, 0x14, 0x63),
    'Y': (0x07, 0x08, 0x70, 0x08, 0x07), 'Z': (0x61, 0x51, 0x49, 0x45, 0x43),
    '-': (0x08, 0x08, 0x08, 0x08, 0x08), ':': (0x00, 0x36, 0x36, 0x00, 0x00),
    '.': (0x00, 0x60, 0x60, 0x00, 0x00), '_': (0x40, 0x40, 0x40, 0x40, 0x40),
    '(': (0x00, 0x1C, 0x22, 0x41, 0x00), ')': (0x00, 0x41, 0x22, 0x1C, 0x00),
    '/': (0x20, 0x10, 0x08, 0x04, 0x02), '+': (0x08, 0x08, 0x3E, 0x08, 0x08),
    ',': (0x00, 0x50, 0x30, 0x00, 0x00), "'": (0x00, 0x05, 0x03, 0x00, 0x00),
}

# Projections: (screen x, screen y up) from a right-handed point.
VIEWS = {
    'front': (lambda p: (-p[0], p[1]), 'FRONT (LOOKING AT THE HUNTER, +Z)'),
    'side': (lambda p: (-p[2], p[1]), 'SIDE (FROM ITS LEFT, FACING RIGHT)'),
    'top': (lambda p: (p[0], -p[2]), 'TOP (FORWARD -Z IS UP)'),
}


class Canvas:
    def __init__(self, width: int, height: int, background=(18, 18, 22)):
        self.w, self.h = width, height
        self.px = bytearray(bytes(background) * (width * height))

    def plot(self, x: int, y: int, color, alpha: float = 1.0):
        if 0 <= x < self.w and 0 <= y < self.h:
            i = (y * self.w + x) * 3
            if alpha >= 1.0:
                self.px[i:i + 3] = bytes(color)
            else:
                for k in range(3):
                    self.px[i + k] = int(self.px[i + k] * (1 - alpha) + color[k] * alpha)

    def line(self, x0, y0, x1, y1, color, alpha=1.0, width=1):
        x0, y0, x1, y1 = int(round(x0)), int(round(y0)), int(round(x1)), int(round(y1))
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        r = width // 2
        for _ in range(4096):
            for ox in range(-r, r + 1):
                for oy in range(-r, r + 1):
                    self.plot(x0 + ox, y0 + oy, color, alpha)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def dot(self, x, y, color, radius=2):
        x, y = int(round(x)), int(round(y))
        for ox in range(-radius, radius + 1):
            for oy in range(-radius, radius + 1):
                if ox * ox + oy * oy <= radius * radius:
                    self.plot(x + ox, y + oy, color)

    def text(self, x, y, s, color, scale=2):
        for ch in s.upper():
            glyph = FONT.get(ch)
            if glyph:
                for col in range(5):
                    for row in range(7):
                        if glyph[col] & (1 << row):
                            for a in range(scale):
                                for b in range(scale):
                                    self.plot(x + col * scale + a, y + row * scale + b, color)
            x += 6 * scale

    def png(self) -> bytes:
        raw = b''.join(b'\0' + bytes(self.px[y * self.w * 3:(y + 1) * self.w * 3])
                       for y in range(self.h))

        def chunk(kind, data):
            return (struct.pack('>I', len(data)) + kind + data +
                    struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF))

        return (b'\x89PNG\r\n\x1a\n' +
                chunk(b'IHDR', struct.pack('>IIBBBBB', self.w, self.h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


class Frame:
    """Maps a projected 2D box to the canvas, keeping the aspect."""

    def __init__(self, canvas: Canvas, lo, hi, margin=60):
        self.c = canvas
        span = max(hi[0] - lo[0], hi[1] - lo[1], 1e-6)
        self.scale = (min(canvas.w, canvas.h) - 2 * margin) / span
        self.cx = (lo[0] + hi[0]) / 2
        self.cy = (lo[1] + hi[1]) / 2

    def __call__(self, q):
        return (self.c.w / 2 + (q[0] - self.cx) * self.scale,
                self.c.h / 2 - (q[1] - self.cy) * self.scale)


def bone_color(name: str):
    if 'Finger' in name:
        return (240, 220, 60)
    if name.startswith('L_') and any(k in name for k in ('Clavicle', 'Arm', 'Forearm', 'Hand',
                                                          'Weapon')):
        return (60, 220, 240)
    if name.startswith('R_') and any(k in name for k in ('Clavicle', 'Arm', 'Forearm', 'Hand',
                                                          'Weapon')):
        return (240, 70, 60)
    if any(k in name for k in ('Thigh', 'Calf', 'Foot', 'Toe')):
        return (90, 220, 110)
    return (230, 230, 230)


def weapon_in_hand(body: dict, weapon_doc: dict, weapon_blob: bytes, hand='R_Weapon'):
    """World-space weapon vertices, edges and dummies with the weapon's origin at the hand's
    weapon bone (bind pose)."""
    names = {b['name']: i for i, b in enumerate(body['bones'])}
    m = body['bones'][names[hand]]['bind_world']
    verts = [bbmodel.apply(m, v[0]) for v in bbmodel.vertices_of(weapon_doc, weapon_blob)]
    edges = set()
    for mesh in weapon_doc['meshes']:
        idx = bbmodel.indices_of(weapon_doc, weapon_blob, mesh)
        for k in range(0, len(idx), 3):
            a, b, c = idx[k], idx[k + 1], idx[k + 2]
            edges.update({(min(a, b), max(a, b)), (min(b, c), max(b, c)),
                          (min(a, c), max(a, c))})
    dummies = [(d['ref'], bbmodel.apply(m, d['position'])) for d in weapon_doc['dummies']]
    return verts, edges, dummies


def render_body(out_dir: Path, body_doc: dict, body_blob: bytes, weapon=None) -> list:
    bones = body_doc['bones']
    joints = [b['bind_world'][12:15] for b in bones]
    points = []
    for mesh in body_doc['meshes']:
        if mesh['hidden']:
            continue
        for v in bbmodel.vertices_of(body_doc, body_blob, mesh):
            points.append(v[0])
    wverts, wedges, wdummies = weapon if weapon else ([], set(), [])
    written = []
    for view, (project, label) in VIEWS.items():
        canvas = Canvas(1024, 1024)
        pts = [project(p) for p in points] + [project(p) for p in wverts]
        lo = (min(q[0] for q in pts), min(q[1] for q in pts))
        hi = (max(q[0] for q in pts), max(q[1] for q in pts))
        frame = Frame(canvas, lo, hi)
        for p in points:
            x, y = frame(project(p))
            canvas.plot(int(x), int(y), (150, 150, 160), 0.35)
        for a, b in wedges:
            pa, pb = frame(project(wverts[a])), frame(project(wverts[b]))
            canvas.line(pa[0], pa[1], pb[0], pb[1], (255, 150, 40), 0.5)
        for i, b in enumerate(bones):
            if not b['deform'] or b['parent'] < 0 or i >= body_doc['skeleton_bones']:
                continue
            if b['name'].startswith('Support_') or '_Case_' in b['name'] or \
                    '_Target' in b['name']:
                continue  # twist helpers and weapon holsters overlap the limbs
            pa, pb = frame(project(joints[i])), frame(project(joints[b['parent']]))
            canvas.line(pa[0], pa[1], pb[0], pb[1], bone_color(b['name']), 1.0, 2)
            canvas.dot(pa[0], pa[1], bone_color(b['name']), 2)
        for d in body_doc['dummies']:
            if d['parent'] >= 0 and bones[d['parent']]['deform'] and d['parent'] < 108:
                x, y = frame(project(d['position']))
                canvas.dot(x, y, (60, 255, 90), 1)
        for ref, p in wdummies:
            x, y = frame(project(p))
            canvas.dot(x, y, (255, 60, 255), 3)
            canvas.text(int(x) + 6, int(y) - 6, str(ref), (255, 120, 255), 1)
        canvas.text(16, 16, f'{body_doc["name"]} {label}', (255, 255, 255), 2)
        canvas.text(16, 40, f'{len(points)} VERTS  {len(bones)} BONES  '
                            f'{len(body_doc["dummies"])} DUMMIES', (180, 180, 180), 2)
        canvas.text(16, 64, 'ARMS L CYAN R RED  FINGERS YELLOW  WEAPON ORANGE', (180, 180, 180), 2)
        path = out_dir / f'{body_doc["name"]}_{view}.png'
        path.write_bytes(canvas.png())
        written.append(path)
    return written


def render_weapon(out_dir: Path, doc: dict, blob: bytes) -> Path:
    verts = [v[0] for v in bbmodel.vertices_of(doc, blob)]
    canvas = Canvas(1400, 800)
    name = doc.get('weapon', {}).get('name', doc['name'])
    for half, (view, project) in enumerate((('FRONT XY', lambda p: (p[0], p[1])),
                                            ('SIDE ZY', lambda p: (-p[2], p[1])))):
        sub = Canvas(700, 800)
        pts = [project(p) for p in verts] + [project(d['position']) for d in doc['dummies']]
        pts.append((0.0, 0.0))
        lo = (min(q[0] for q in pts), min(q[1] for q in pts))
        hi = (max(q[0] for q in pts), max(q[1] for q in pts))
        frame = Frame(sub, lo, hi, 80)
        for mesh in doc['meshes']:
            idx = bbmodel.indices_of(doc, blob, mesh)
            for k in range(0, len(idx), 3):
                tri = [frame(project(verts[idx[k + j]])) for j in range(3)]
                for j in range(3):
                    a, b = tri[j], tri[(j + 1) % 3]
                    sub.line(a[0], a[1], b[0], b[1], (255, 150, 40), 0.25)
        ox, oy = frame((0.0, 0.0))
        sub.line(ox - 12, oy, ox + 12, oy, (255, 255, 255))
        sub.line(ox, oy - 12, ox, oy + 12, (255, 255, 255))
        sub.text(int(ox) + 6, int(oy) + 6, 'GRIP', (255, 255, 255), 1)
        for d in doc['dummies']:
            x, y = frame(project(d['position']))
            sub.dot(x, y, (255, 60, 255), 3)
            sub.text(int(x) + 6, int(y) - 4, str(d['ref']), (255, 140, 255), 2)
        sub.text(12, 12, f'{name} {view}', (255, 255, 255), 2)
        for y in range(800):
            row = sub.px[y * 700 * 3:(y + 1) * 700 * 3]
            start = (y * 1400 + half * 700) * 3
            canvas.px[start:start + 700 * 3] = row
    path = out_dir / f'{doc["name"]}.png'
    path.write_bytes(canvas.png())
    return path


def render_all(asset_dir: Path) -> list:
    asset_dir = Path(asset_dir)
    index = json.loads((asset_dir / 'assets.json').read_text(encoding='utf-8'))
    out_dir = asset_dir / 'preview'
    out_dir.mkdir(parents=True, exist_ok=True)
    body_doc, body_blob = bbmodel.load(asset_dir / index['body'])
    weapon = None
    written = []
    for i, w in enumerate(index['weapons']):
        doc, blob = bbmodel.load(asset_dir / w['file'])
        written.append(render_weapon(out_dir, doc, blob))
        if i == 0:
            weapon = weapon_in_hand(body_doc, doc, blob)
    written += render_body(out_dir, body_doc, body_blob, weapon)
    return written


if __name__ == '__main__':
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        Path(__file__).resolve().parents[2] / 'out' / 'vr_assets'
    for p in render_all(target):
        print(p)
