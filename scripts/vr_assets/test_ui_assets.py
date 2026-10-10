#!/usr/bin/env python3
"""Offline tests of the VR menu art pipeline (tpf.py, gfxfont.py, ui_extract.py):
python scripts/vr_assets/test_ui_assets.py [-v]

Format tests run on synthetic data. With Pillow installed, random BC7 blocks are also checked
against Pillow's decoder (skipped otherwise; Pillow is not needed by the pipeline). Tests marked
"dump" read the user's own Bloodborne dump (BB_GAME_DIR, default C:/games/CUSA03173) and are
skipped without it; nothing is written into the dump.
"""
from __future__ import annotations

import io
import os
import random
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import catalog  # noqa: E402
import gfxfont  # noqa: E402
import tpf  # noqa: E402
import ui_extract  # noqa: E402

GAME = Path(os.environ.get('BB_GAME_DIR', 'C:/games/CUSA03173'))
HAVE_DUMP = (catalog.game_root(GAME) / 'menu' / 'menu_knowledge_00001.tpf.dcx').exists()
try:
    from PIL import Image  # noqa: F401
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


def ps4_tile(linear: bytes, width_units: int, height_units: int, unit: int) -> bytes:
    """The inverse of tpf.ps4_untile (for the tests)."""
    tiles_x, tiles_y = (width_units + 7) // 8, (height_units + 7) // 8
    out = bytearray(tiles_x * tiles_y * 64 * unit)
    at = 0
    for ty in range(tiles_y):
        for tx in range(tiles_x):
            for t in range(64):
                x = tx * 8 + ((t & 1) | ((t >> 1) & 2) | ((t >> 2) & 4))
                y = ty * 8 + (((t >> 1) & 1) | ((t >> 2) & 2) | ((t >> 3) & 4))
                if x < width_units and y < height_units:
                    src = (y * width_units + x) * unit
                    out[at:at + unit] = linear[src:src + unit]
                at += unit
    return bytes(out)


def make_tpf(name: str, width: int, height: int, dxgi: int, fmt: int, payload: bytes) -> bytes:
    """A PS4 TPF with one texture (UTF-16 names), like menu/*.tpf."""
    name_bytes = name.encode('utf-16-le') + b'\0\0'
    entry = struct.pack('<IiBBBBhhiiIii', 0, len(payload), fmt, 0, 1, 0, width, height, 1, 0xD,
                        0, 0, dxgi)
    header_size = 0x10 + len(entry)
    name_at = header_size
    data_at = (name_at + len(name_bytes) + 15) & ~15
    entry = struct.pack('<IiBBBBhhiiIii', data_at, len(payload), fmt, 0, 1, 0, width, height, 1,
                        0xD, name_at, 0, dxgi)
    head = b'TPF\0' + struct.pack('<ii', len(payload), 1) + bytes([4, 3, 1, 0])
    blob = head + entry + name_bytes
    return blob + b'\0' * (data_at - len(blob)) + payload


class Bc7Tests(unittest.TestCase):
    def test_tables_agree(self):
        for p in range(64):
            self.assertEqual(tpf.BC7_PARTITIONS_2[p][0], 0)
            self.assertEqual(tpf.BC7_PARTITIONS_2[p][tpf.BC7_ANCHOR_2[p]], 1)
            self.assertEqual(tpf.BC7_PARTITIONS_3[p][tpf.BC7_ANCHOR_3A[p]], 1)
            self.assertEqual(tpf.BC7_PARTITIONS_3[p][tpf.BC7_ANCHOR_3B[p]], 2)

    def test_mode6_solid_endpoints(self):
        # Mode 6: endpoints 7 bits + p-bit per channel, 4-bit indices (pixel 0's MSB implied).
        # Both endpoints (r, g, b, a) = (100, 50, 25, 127) << 1 | p, p0 = 0, p1 = 1.
        bits = [(1 << 6, 7)]  # mode 6: six zeros then a one
        for value in (100, 100, 50, 50, 25, 25, 127, 127):  # R0 R1 G0 G1 B0 B1 A0 A1
            bits.append((value, 7))
        bits += [(0, 1), (1, 1)]
        bits += [(0, 3)] + [(15, 4)] * 15  # pixel 0 index 0, the rest 15 (endpoint 1)
        v, pos = 0, 0
        for value, n in bits:
            v |= value << pos
            pos += n
        pixels = tpf.bc7_decode_block(v.to_bytes(16, 'little'))
        self.assertEqual(pixels[0], bytes((200, 100, 50, 254)))
        self.assertEqual(pixels[5], bytes((201, 101, 51, 255)))

    def test_reserved_mode_is_transparent(self):
        self.assertEqual(tpf.bc7_decode_block(bytes(16)), [b'\0\0\0\0'] * 16)

    @unittest.skipUnless(HAVE_PIL, 'Pillow not installed')
    def test_random_blocks_match_pillow(self):
        from PIL import Image
        rng = random.Random(7)
        blocks = 16 * 16
        data = bytearray(rng.getrandbits(8) for _ in range(blocks * 16))
        for b in range(blocks):  # no reserved mode: Pillow makes it opaque, the spec transparent
            data[b * 16] |= 1 << rng.randrange(8)
        data = bytes(data)
        tex = tpf.Texture('t', 102, 0, 1, 0, 64, 64, 98, tpf.PLATFORM_PC, data)
        ours = tpf.decode(98, 64, 64, data)
        dds = tpf.to_dds(tex)
        theirs = Image.open(io.BytesIO(dds)).convert('RGBA').tobytes()
        self.assertEqual(ours, theirs)


class FormatTests(unittest.TestCase):
    def test_bc1(self):
        # c0 pure red (0xF800) > c1 pure blue (0x001F): four colors; indices 0,1,2,3 in a row.
        block = struct.pack('<HHI', 0xF800, 0x001F, 0b11100100 | (0b11100100 << 8))
        out = tpf.decode(71, 4, 4, block)
        px = [out[i * 4:i * 4 + 4] for i in range(16)]
        self.assertEqual(px[0], bytes((255, 0, 0, 255)))
        self.assertEqual(px[1], bytes((0, 0, 255, 255)))
        self.assertEqual(px[2], bytes((170, 0, 85, 255)))
        self.assertEqual(px[3], bytes((85, 0, 170, 255)))
        # c0 <= c1: three colors and transparent black.
        block = struct.pack('<HHI', 0x001F, 0xF800, 3)
        self.assertEqual(tpf.decode(71, 4, 4, block)[:4], b'\0\0\0\0')

    def test_ps4_untile_roundtrip(self):
        w, h, unit = 13, 5, 2  # not whole tiles: padding
        linear = bytes((i * 7 + k) & 255 for i in range(w * h) for k in range(unit))
        self.assertEqual(tpf.ps4_untile(ps4_tile(linear, w, h, unit), w, h, unit), linear)

    def test_tpf_ps4_bc1(self):
        # 8x8 BC1 = 2x2 blocks, tiled into one 8x8-block tile (64 blocks of 8 bytes).
        blocks = [struct.pack('<HHI', 0xF800, 0xF800, 0), struct.pack('<HHI', 0x07E0, 0x07E0, 0),
                  struct.pack('<HHI', 0x001F, 0x001F, 0), struct.pack('<HHI', 0xFFFF, 0xFFFF, 0)]
        payload = ps4_tile(b''.join(blocks), 2, 2, 8)
        textures = tpf.tpf_read(make_tpf('MENU_Test', 8, 8, 71, 0, payload))
        self.assertEqual([t.name for t in textures], ['MENU_Test'])
        w, h, rgba = tpf.texture_rgba(textures[0])
        self.assertEqual((w, h), (8, 8))
        self.assertEqual(rgba[0:4], bytes((255, 0, 0, 255)))          # block (0, 0)
        self.assertEqual(rgba[4 * 4:4 * 4 + 4], bytes((0, 255, 0, 255)))  # block (1, 0)
        self.assertEqual(rgba[(4 * 8) * 4:(4 * 8) * 4 + 4], bytes((0, 0, 255, 255)))  # (0, 1)
        dxgi, dw, dh, data = tpf.dds_read(tpf.to_dds(textures[0]))
        self.assertEqual((dxgi, dw, dh), (71, 8, 8))
        self.assertEqual(data, b''.join(blocks))

    def test_png_gray_and_rgba(self):
        import zlib
        png = tpf.png_bytes(3, 2, bytes(range(6)), channels=1)
        self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
        idat = png[png.index(b'IDAT') + 4:]
        raw = zlib.decompressobj().decompress(idat)
        self.assertEqual(raw[:8], b'\0\0\1\2\0\3\4\5')

    def test_resize_and_bleed(self):
        # A 4x4 opaque red square in an 8x8 transparent image, halved: 2x2 red in 4x4.
        px = bytearray(8 * 8 * 4)
        for y in range(2, 6):
            for x in range(2, 6):
                px[(y * 8 + x) * 4:(y * 8 + x) * 4 + 4] = bytes((255, 0, 0, 255))
        small = ui_extract.resize_rgba(8, 8, bytes(px), 4, 4)
        self.assertEqual(small[(1 * 4 + 1) * 4:(1 * 4 + 1) * 4 + 4], bytes((255, 0, 0, 255)))
        self.assertEqual(small[3], 0)
        bled = ui_extract.bleed(4, 4, small)
        self.assertEqual(bled[0:4], bytes((255, 0, 0, 0)))  # color copied, still transparent


class FontTests(unittest.TestCase):
    def make_font(self) -> bytes:
        """A GFX file with one DefineFont3: 'A' is a 0.5 em square, 'B' the same with a hole."""
        def bits(fields):
            v = n = 0
            for value, width in fields:
                v = (v << width) | (value & ((1 << width) - 1))
                n += width
            pad = (-n) % 8
            return (v << pad).to_bytes((n + pad) // 8, 'big')

        def square(x0, y0, size, fill0, fill1):
            nb = 16
            # Edge: type 1, straight 1, NumBits - 2, general 0, vertical flag, delta.
            return [(0, 1), (0b00111, 5), (nb, 5), (x0, nb), (y0, nb), (fill0, 1), (fill1, 1),
                    (1, 1), (1, 1), (nb - 2, 4), (0, 1), (0, 1), (size, nb),   # right
                    (1, 1), (1, 1), (nb - 2, 4), (0, 1), (1, 1), (size, nb),   # down
                    (1, 1), (1, 1), (nb - 2, 4), (0, 1), (0, 1), (-size, nb),  # left
                    (1, 1), (1, 1), (nb - 2, 4), (0, 1), (1, 1), (-size, nb)]  # up

        em = gfxfont.EM
        shape_a = bits([(1, 4), (0, 4)] + square(0, -em // 2, em // 2, 0, 1) + [(0, 6)])
        # B: the outer square, and a hole traced the same way with the fill on the other side.
        shape_b = bits([(1, 4), (0, 4)] + square(0, -em // 2, em // 2, 0, 1) +
                       square(em // 8, -3 * em // 8, em // 4, 1, 0) + [(0, 6)])
        name = b'Test'
        count = 2
        offsets_size = 4 * (count + 1)
        table = struct.pack('<III', offsets_size, offsets_size + len(shape_a),
                            offsets_size + len(shape_a) + len(shape_b))
        body = (struct.pack('<HBBB', 1, 0x8C, 1, len(name)) + name + struct.pack('<H', count) +
                table + shape_a + shape_b + struct.pack('<HH', ord('A'), ord('B')) +
                struct.pack('<hhh', 18000, 2400, 0) + struct.pack('<hh', em // 2, em // 2) +
                bytes([0, 0]) + struct.pack('<H', 0))
        tag = struct.pack('<HI', (75 << 6) | 0x3F, len(body)) + body
        end = struct.pack('<H', 0)
        header_rect = bytes([0x00])  # nbits 0
        movie = header_rect + struct.pack('<HH', 0x1800, 1) + tag + end
        return b'GFX\x0b' + struct.pack('<I', 8 + len(movie)) + movie

    def test_define_font3_and_sdf(self):
        font = gfxfont.font_read(self.make_font())
        self.assertEqual(font.name, 'Test')
        self.assertEqual(sorted(font.glyphs), [ord('A'), ord('B')])
        a = font.glyphs[ord('A')]
        self.assertEqual(len(a.segments), 4)
        self.assertEqual(a.outline_bounds(), (0, gfxfont.EM // 2, -gfxfont.EM // 2, 0))
        width, height, pixels, metrics = gfxfont.sdf_atlas(font, 'AB', em_px=32, spread=4.0,
                                                           atlas_width=128)
        for ch, center_inside in (('A', True), ('B', False)):
            g = metrics['glyphs'][ord(ch)]
            cx, cy = g['x'] + g['w'] // 2, g['y'] + g['h'] // 2
            value = pixels[cy * width + cx]
            self.assertEqual(value > 128, center_inside, (ch, value))
            corner = pixels[g['y'] * width + g['x']]
            self.assertLess(corner, 128)
        self.assertAlmostEqual(metrics['glyphs'][ord('A')]['advance'], 0.5)


@unittest.skipUnless(HAVE_DUMP, f'no Bloodborne dump at {GAME} (set BB_GAME_DIR)')
class DumpTests(unittest.TestCase):
    root = catalog.game_root(GAME)

    def test_knowledge_picture(self):
        tex = tpf.tpf_read(ui_extract.knowledge_path(self.root, 1007).read_bytes())[0]
        self.assertEqual((tex.width, tex.height, tex.dxgi, tex.platform), (400, 400, 98, 4))
        w, h, rgba = tpf.texture_rgba(tex)
        alpha = rgba[3::4]
        self.assertEqual((w, h), (400, 400))
        self.assertEqual(alpha[0], 0)  # transparent corner
        opaque = sum(1 for a in alpha if a == 255) / len(alpha)
        self.assertTrue(0.15 < opaque < 0.7, opaque)  # the Saw Cleaver, diagonal

    def test_items(self):
        items = ui_extract.collect_items(self.root, 'engus', False, False)
        by_name = {i['name']: i for i in items}
        self.assertEqual(by_name['Blood Vial']['icon'], 146)
        self.assertEqual(by_name['Blood Vial']['max'], 20)
        self.assertEqual(by_name['Quicksilver Bullets']['category'], 'ammo')
        self.assertEqual(by_name['Saw Cleaver']['model'], 'wp_a_0800')
        self.assertEqual(by_name['Hunter Pistol']['hand'], 'left')

    def test_menu_font(self):
        font = gfxfont.font_read((self.root / 'menu' / 'engus' / 'font.gfx').read_bytes(), 'Ag')
        self.assertGreater(len(font.glyphs), 1000)
        self.assertGreater(len(font.glyphs[ord('A')].segments), 10)
        self.assertAlmostEqual(font.ascent / gfxfont.EM, 0.88, places=2)


if __name__ == '__main__':
    unittest.main()
