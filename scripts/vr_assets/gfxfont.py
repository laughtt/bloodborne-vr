"""Scaleform GFX / SWF fonts: DefineFont3 glyph outlines -> signed distance field atlas, stdlib only.

    font_read(gfx bytes)        the first DefineFont3 (tag 75): name, ascent/descent/leading,
                                per glyph its code point, outline (edges: lines and quadratic curves),
                                advance and bounds, and the kerning pairs; coordinates in EM units
                                of 1024 * 20 (DefineFont3), y down
    sdf_atlas(font, chars, ...) renders the glyphs as a single-channel signed distance field
                                (0.5 = the outline, larger inside) and packs them into one atlas

Bloodborne's English menus use menu/engus/font.gfx: one DefineFont3 "TT-Reimin Y10 B" (a serif
type family, 7652 glyphs). The SDF keeps text sharp at any size and distance in VR. Nothing here
writes into the game folder.
"""
from __future__ import annotations

import math
import struct
import zlib
from dataclasses import dataclass, field

EM = 1024 * 20  # DefineFont3 EM square


@dataclass
class Glyph:
    code: int
    segments: list          # outline edges (x0, y0, (cx, cy) or None, x1, y1), fill on the right
    advance: float          # EM units
    bounds: tuple           # (xmin, xmax, ymin, ymax), EM units, y down (GFX often leaves 0s)

    def outline_bounds(self) -> tuple:
        """(xmin, xmax, ymin, ymax) of the outline (control points included)."""
        xs, ys = [], []
        for x0, y0, ctrl, x1, y1 in self.segments:
            xs += [x0, x1]
            ys += [y0, y1]
            if ctrl:
                xs.append(ctrl[0])
                ys.append(ctrl[1])
        return (min(xs), max(xs), min(ys), max(ys)) if xs else (0, 0, 0, 0)


@dataclass
class Font:
    name: str
    ascent: float
    descent: float
    leading: float
    glyphs: dict = field(default_factory=dict)    # code point -> Glyph
    kerning: dict = field(default_factory=dict)   # (left code, right code) -> adjustment (EM)


class _Bits:
    """MSB-first bit reader (SWF)."""

    def __init__(self, data: bytes, at: int):
        self.data = data
        self.pos = at * 8

    def u(self, n: int) -> int:
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def s(self, n: int) -> int:
        if n == 0:
            return 0
        v = self.u(n)
        return v - (1 << n) if v & (1 << (n - 1)) else v

    def align(self) -> int:
        self.pos = (self.pos + 7) & ~7
        return self.pos >> 3


def swf_tags(data: bytes) -> list:
    """(data, [(code, offset, length)]) of an SWF/GFX file: the (decompressed) bytes and its tags."""
    if data[:3] in (b'CFX', b'CWS'):
        data = data[:8] + zlib.decompress(data[8:])
    if data[:3] not in (b'GFX', b'FWS'):
        raise ValueError(f'not GFX/SWF: {data[:3]!r}')
    nbits = data[8] >> 3
    at = 8 + (5 + nbits * 4 + 7) // 8 + 4
    tags = []
    while at + 2 <= len(data):
        (code_len,) = struct.unpack_from('<H', data, at)
        at += 2
        code, length = code_len >> 6, code_len & 0x3F
        if length == 0x3F:
            (length,) = struct.unpack_from('<I', data, at)
            at += 4
        tags.append((code, at, length))
        at += length
        if code == 0:
            break
    return data, tags


def _shape(data: bytes, at: int, end: int) -> list:
    """Contours of a glyph SHAPE. Edges whose fill is on the left (fill style 0) are reversed, so
    every contour has its fill on the same side and the nonzero rule fills the glyph."""
    bits = _Bits(data, at)
    fill_bits = bits.u(4)
    line_bits = bits.u(4)
    x = y = 0
    fill0 = fill1 = 0
    edges = []  # (fill on right, x0, y0, ctrl or None, x1, y1)
    while bits.pos < end * 8:
        if bits.u(1) == 0:
            flags = bits.u(5)
            if flags == 0:
                break
            if flags & 1:  # move to (absolute)
                n = bits.u(5)
                x, y = bits.s(n), bits.s(n)
            if flags & 2:
                fill0 = bits.u(fill_bits)
            if flags & 4:
                fill1 = bits.u(fill_bits)
            if flags & 8:
                bits.u(line_bits)
            if flags & 16:  # new styles: not used by glyphs
                raise ValueError('glyph shape with new styles')
        elif bits.u(1):  # straight
            n = bits.u(4) + 2
            if bits.u(1):
                dx, dy = bits.s(n), bits.s(n)
            elif bits.u(1):
                dx, dy = 0, bits.s(n)
            else:
                dx, dy = bits.s(n), 0
            edges.append((fill0, fill1, x, y, None, x + dx, y + dy))
            x, y = x + dx, y + dy
        else:  # curved
            n = bits.u(4) + 2
            cx, cy = x + bits.s(n), y + bits.s(n)
            ax, ay = cx + bits.s(n), cy + bits.s(n)
            edges.append((fill0, fill1, x, y, (cx, cy), ax, ay))
            x, y = ax, ay
    segments = []
    for f0, f1, x0, y0, ctrl, x1, y1 in edges:
        if f0 and f1:
            continue  # an edge between two filled areas
        if f0 and not f1:
            x0, y0, x1, y1 = x1, y1, x0, y0
        segments.append((x0, y0, ctrl, x1, y1))
    return segments


def font_read(data: bytes, codes=None) -> Font:
    """The first DefineFont3 of a GFX/SWF file; with `codes` (code points or characters), only those
    glyphs' outlines are decoded (the others keep their metrics and an empty outline)."""
    data, tags = swf_tags(data)
    wanted = None if codes is None else {c if isinstance(c, int) else ord(c) for c in codes}
    for code, at, length in tags:
        if code == 75:
            return _define_font3(data, at, at + length, wanted)
    raise ValueError('no DefineFont3 in the file')


def _define_font3(data: bytes, at: int, end: int, wanted=None) -> Font:
    flags = data[at + 2]
    has_layout, wide_offsets = bool(flags & 0x80), bool(flags & 0x08)
    wide_codes = bool(flags & 0x04)
    name_len = data[at + 4]
    name = data[at + 5:at + 5 + name_len].split(b'\0', 1)[0].decode('utf-8', 'replace')
    p = at + 5 + name_len
    (count,) = struct.unpack_from('<H', data, p)
    p += 2
    table = p
    ofmt = '<I' if wide_offsets else '<H'
    osize = 4 if wide_offsets else 2
    offsets = [struct.unpack_from(ofmt, data, table + i * osize)[0] for i in range(count)]
    (code_table,) = struct.unpack_from(ofmt, data, table + count * osize)
    shapes = []
    for i in range(count):
        start = table + offsets[i]
        stop = table + (offsets[i + 1] if i + 1 < count else code_table)
        shapes.append((start, stop))
    p = table + code_table
    cfmt, csize = ('<H', 2) if wide_codes else ('<B', 1)
    codes = [struct.unpack_from(cfmt, data, p + i * csize)[0] for i in range(count)]
    p += count * csize
    ascent = descent = leading = 0
    advances = [0] * count
    bounds = [(0, 0, 0, 0)] * count
    kerning = {}
    if has_layout:
        ascent, descent, leading = struct.unpack_from('<hhh', data, p)
        p += 6
        advances = list(struct.unpack_from(f'<{count}h', data, p))
        p += 2 * count
        bits = _Bits(data, p)
        rects = []
        for _ in range(count):
            n = bits.u(5)
            rects.append((bits.s(n), bits.s(n), bits.s(n), bits.s(n)))
            p = bits.align()
            bits = _Bits(data, p)
        bounds = rects
        (kern_count,) = struct.unpack_from('<H', data, p)
        p += 2
        for _ in range(kern_count):
            if wide_codes:
                a, b, adj = struct.unpack_from('<HHh', data, p)
                p += 6
            else:
                a, b, adj = struct.unpack_from('<BBh', data, p)
                p += 4
            kerning[(a, b)] = adj
    font = Font(name, ascent, descent, leading, kerning=kerning)
    for i in range(count):
        outline = _shape(data, *shapes[i]) if wanted is None or codes[i] in wanted else []
        font.glyphs[codes[i]] = Glyph(codes[i], outline, advances[i], bounds[i])
    return font


# ---- Signed distance field --------------------------------------------------------------------------

def _flatten(segments: list, scale: float, ox: float, oy: float, tolerance: float = 0.2) -> list:
    """Line segments (x0, y0, x1, y1) in pixels: EM units * scale + offset; curves subdivided until
    the chord error is under `tolerance` pixels."""
    out = []
    for x0, y0, ctrl, x1, y1 in segments:
        a = (x0 * scale + ox, y0 * scale + oy)
        b = (x1 * scale + ox, y1 * scale + oy)
        if ctrl is None:
            out.append((a[0], a[1], b[0], b[1]))
            continue
        c = (ctrl[0] * scale + ox, ctrl[1] * scale + oy)
        # Error of a quadratic's chord ~ |a - 2c + b| / 4 per subdivision level.
        dev = math.hypot(a[0] - 2 * c[0] + b[0], a[1] - 2 * c[1] + b[1]) / 4
        n = max(1, min(32, int(math.ceil(math.sqrt(dev / tolerance))))) if dev > 0 else 1
        prev = a
        for k in range(1, n + 1):
            t = k / n
            u = 1 - t
            pt = (u * u * a[0] + 2 * u * t * c[0] + t * t * b[0],
                  u * u * a[1] + 2 * u * t * c[1] + t * t * b[1])
            out.append((prev[0], prev[1], pt[0], pt[1]))
            prev = pt
    return out


def glyph_sdf(segments: list, width: int, height: int, spread: float) -> bytes:
    """8-bit SDF of line segments in a width x height cell: 128 + 127 * d / spread, d > 0 inside
    (nonzero winding), clamped."""
    far = spread
    dist = [far] * (width * height)
    for x0, y0, x1, y1 in segments:
        dx, dy = x1 - x0, y1 - y0
        ll = dx * dx + dy * dy
        lo_x = max(0, int(min(x0, x1) - far))
        hi_x = min(width - 1, int(max(x0, x1) + far) + 1)
        lo_y = max(0, int(min(y0, y1) - far))
        hi_y = min(height - 1, int(max(y0, y1) + far) + 1)
        for py in range(lo_y, hi_y + 1):
            cy = py + 0.5
            row = py * width
            for px in range(lo_x, hi_x + 1):
                cx = px + 0.5
                if ll > 0:
                    t = ((cx - x0) * dx + (cy - y0) * dy) / ll
                    t = 0.0 if t < 0 else 1.0 if t > 1 else t
                    ex, ey = x0 + t * dx - cx, y0 + t * dy - cy
                else:
                    ex, ey = x0 - cx, y0 - cy
                d = math.sqrt(ex * ex + ey * ey)
                if d < dist[row + px]:
                    dist[row + px] = d
    out = bytearray(width * height)
    for py in range(height):
        cy = py + 0.5
        crossings = []
        for x0, y0, x1, y1 in segments:
            if (y0 <= cy < y1) or (y1 <= cy < y0):
                t = (cy - y0) / (y1 - y0)
                crossings.append((x0 + t * (x1 - x0), 1 if y1 > y0 else -1))
        crossings.sort()
        k = 0
        winding = 0
        row = py * width
        for px in range(width):
            cx = px + 0.5
            while k < len(crossings) and crossings[k][0] < cx:
                winding += crossings[k][1]
                k += 1
            d = dist[row + px]
            signed = d if winding != 0 else -d
            out[row + px] = max(0, min(255, int(round(128 + 127 * signed / spread))))
    return bytes(out)


def _render_one(args):
    code, segments, width, height, spread = args
    return code, glyph_sdf(segments, width, height, spread)


def sdf_atlas(font: Font, chars, em_px: int = 56, spread: float = 6.0, atlas_width: int = 1024,
              pool=None) -> tuple[int, int, bytes, dict]:
    """(width, height, gray pixels, metrics) of an SDF atlas of `chars` (those in the font).
    metrics: em_px, spread, ascent/descent/line_gap (in em), glyphs {code: {x, y, w, h (atlas px),
    xoff, yoff (em, from the pen on the baseline to the cell's top left, y down), advance (em)}},
    kerning [[left, right, em]]."""
    scale = em_px / EM
    pad = int(math.ceil(spread)) + 1
    jobs = []
    cells = {}
    for ch in sorted(set(chars)):
        code = ord(ch) if isinstance(ch, str) else ch
        glyph = font.glyphs.get(code)
        if glyph is None:
            continue
        xmin, xmax, ymin, ymax = glyph.outline_bounds()
        if not glyph.segments or xmax <= xmin:
            cells[code] = (0, 0, 0, 0, glyph.advance)
            continue
        left = int(math.floor(xmin * scale)) - pad
        top = int(math.floor(ymin * scale)) - pad
        w = int(math.ceil(xmax * scale)) + pad - left
        h = int(math.ceil(ymax * scale)) + pad - top
        segs = _flatten(glyph.segments, scale, -left, -top)
        jobs.append((code, segs, w, h, spread))
        cells[code] = (left, top, w, h, glyph.advance)
    results = dict(pool.map(_render_one, jobs) if pool else map(_render_one, jobs))
    # Shelf packing, tallest first.
    order = sorted(results, key=lambda c: (-cells[c][3], c))
    x = y = shelf = 0
    placed = {}
    for code in order:
        w, h = cells[code][2], cells[code][3]
        if x + w > atlas_width:
            x, y, shelf = 0, y + shelf + 1, 0
        placed[code] = (x, y)
        x += w + 1
        shelf = max(shelf, h)
    height = y + shelf + 1
    height = (height + 3) & ~3
    pixels = bytearray(atlas_width * height)
    glyphs = {}
    for code, (left, top, w, h, advance) in cells.items():
        entry = {'x': 0, 'y': 0, 'w': 0, 'h': 0, 'xoff': left / em_px, 'yoff': top / em_px,
                 'advance': advance / EM}
        if code in placed:
            px, py = placed[code]
            src = results[code]
            for row in range(h):
                pixels[(py + row) * atlas_width + px:(py + row) * atlas_width + px + w] = \
                    src[row * w:(row + 1) * w]
            entry.update(x=px, y=py, w=w, h=h)
        glyphs[code] = entry
    kerning = [[a, b, adj / EM] for (a, b), adj in sorted(font.kerning.items())
               if a in glyphs and b in glyphs]
    metrics = {'font': font.name, 'em_px': em_px, 'spread_px': spread,
               'ascent': font.ascent / EM, 'descent': font.descent / EM,
               'line_gap': font.leading / EM, 'glyphs': glyphs, 'kerning': kerning}
    return atlas_width, height, bytes(pixels), metrics
