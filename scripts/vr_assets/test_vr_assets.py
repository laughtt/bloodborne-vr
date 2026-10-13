#!/usr/bin/env python3
"""Offline tests of the VR asset pipeline: python scripts/vr_assets/test_vr_assets.py [-v]

Format tests run on synthetic data. Tests marked "dump" read the user's own Bloodborne dump
(BB_GAME_DIR, default C:/games/CUSA03173) and are skipped without it; they extract into a temporary
folder, never into the dump.
"""
from __future__ import annotations

import json
import math
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bbmodel  # noqa: E402
import catalog  # noqa: E402
import extract  # noqa: E402
import held_extract  # noqa: E402
import souls  # noqa: E402

GAME = Path(os.environ.get('BB_GAME_DIR', 'C:/games/CUSA03173'))
HAVE_DUMP = (catalog.game_root(GAME) / 'chr' / 'c0000.chrbnd.dcx').exists()


def make_dcx(payload: bytes, kind: bytes = b'DFLT') -> bytes:
    """A DCX_DFLT_10000_44_9 wrapper like Bloodborne's."""
    body = zlib.compress(payload, 9) if kind == b'DFLT' else payload
    header = (b'DCX\0' + struct.pack('>IIIII', 0x10000, 0x18, 0x24, 0x44, 0x4c) +
              b'DCS\0' + struct.pack('>II', len(payload), len(body)) +
              b'DCP\0' + kind + struct.pack('>I', 0x20) + bytes([9, 0, 0, 0]) +
              struct.pack('>III', 0, 0, 0) + struct.pack('>I', 0x00010100) +
              b'DCA\0' + struct.pack('>I', 8))
    return header + body


def make_bnd4(files: list) -> bytes:
    """A BND4 (little endian, UTF-16 names, IDs, 32-bit offsets) of (id, name, data)."""
    count = len(files)
    entry = 0x24
    names_at = 0x40 + count * entry
    names = b''
    name_offsets = []
    for _fid, name, _data in files:
        name_offsets.append(names_at + len(names))
        names += name.encode('utf-16-le') + b'\0\0'
    data_at = names_at + len(names)
    data_at = (data_at + 15) & ~15
    blob = b''
    entries = b''
    for (fid, _name, data), noff in zip(files, name_offsets):
        offset = data_at + len(blob)
        entries += struct.pack('<B3xiqqIiI', 0x40, -1, len(data), len(data), offset, fid, noff)
        blob += data
    header = (b'BND4' + bytes([0, 0, 0, 0, 0, 0, 1, 0]) + struct.pack('<i', count) +
              struct.pack('<q', 0x40) + b'07D7R6\0\0' + struct.pack('<q', entry) +
              struct.pack('<q', data_at) + bytes([1, 0x74, 0, 0]) + struct.pack('<iq', 0, 0))
    assert len(header) == 0x40
    raw = header + entries + names
    return raw + b'\0' * (data_at - len(raw)) + blob


class FormatTests(unittest.TestCase):
    def test_dcx_dflt_roundtrip(self):
        payload = bytes(range(256)) * 50
        packed = make_dcx(payload)
        info = souls.dcx_info(packed)
        self.assertEqual((info.kind, info.uncompressed_size), ('DFLT', len(payload)))
        self.assertEqual(souls.dcx_decompress(packed), payload)

    def test_dcx_reports_kraken(self):
        packed = make_dcx(b'\x8c' * 64, b'KRAK')
        self.assertEqual(souls.dcx_info(packed).kind, 'KRAK')
        with self.assertRaises(souls.DcxError) as raised:
            souls.dcx_decompress(packed)
        self.assertIn('KRAK', str(raised.exception))

    def test_plain_data_passes_through(self):
        self.assertEqual(souls.dcx_decompress(b'FLVER\0L\0'), b'FLVER\0L\0')

    def test_bnd4(self):
        files = [(200, 'N:\\x\\a.flver', b'FLVER-ish'), (100, 'N:\\x\\a.tpf', b'TPF\0data')]
        read = souls.bnd4_read(make_dcx(make_bnd4(files)))
        self.assertEqual([(f.id, f.name, f.data) for f in read], files)
        self.assertEqual(read[0].basename, 'a.flver')

    def test_mirror_matrix(self):
        # Row-vector LH matrix: rotate 90 deg about Y, translate (1, 2, 3).
        w = souls.mat_mul(souls.mat_rot_y(math.pi / 2), souls.mat_translate((1, 2, 3)))
        m = bbmodel.mirror_matrix(w)
        p_lh = souls.transform_point(w, (0.5, 0.0, 0.0))
        p_rh = bbmodel.apply(m, bbmodel.mirror((0.5, 0.0, 0.0)))
        for a, b in zip(bbmodel.mirror(p_lh), p_rh):
            self.assertAlmostEqual(a, b, places=6)
        # Still a proper rotation (det +1) after mirroring both sides.
        r = [[m[c * 4 + r] for c in range(3)] for r in range(3)]
        det = (r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
               r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
               r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]))
        self.assertAlmostEqual(det, 1.0, places=6)

    def test_bbmodel_roundtrip(self):
        model = bbmodel.Model('tiny')
        model.bones.append({'name': 'root', 'parent': -1, 'deform': True,
                            'bind_world': [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]})
        verts = [((0, 0, 0), (0, 0, 1), (0, 0), (0, 0, 0, 0), (1, 0, 0, 0)),
                 ((1, 0, 0), (0, 0, 1), (1, 0), (0, 0, 0, 0), (1, 0, 0, 0)),
                 ((0, 1, 0), (0, 0, 1), (0, 1), (0, 0, 0, 0), (1, 0, 0, 0))]
        model.add_mesh({'name': 'tri'}, verts, [0, 1, 2])
        with tempfile.TemporaryDirectory() as tmp:
            doc, blob = bbmodel.load(model.write(Path(tmp)))
        self.assertEqual(doc['vertex_count'], 3)
        self.assertEqual(bbmodel.indices_of(doc, blob, doc['meshes'][0]), (0, 1, 2))
        self.assertEqual([v[0] for v in bbmodel.vertices_of(doc, blob)],
                         [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)])

    def test_mask_ids(self):
        self.assertEqual(extract.mask_of('#44#'), 44)
        self.assertEqual(extract.mask_of('Material #49'), -1)
        self.assertEqual(extract.mask_of('arm'), -1)


class HeldFormatTests(unittest.TestCase):
    """held_extract.py on synthetic data: the FXR model layer and the TAE goods effect."""

    def test_fxr_model_layer_and_scale(self):
        words = [0] * 8
        at = len(words) * 4
        layer = [41, 0, 10002, 0]
        props_at = at + len(layer) * 4
        values_at = props_at + 3 * 32
        props = []
        for k in range(3):
            props += [11, 0, values_at + 8 * k, 0, values_at + 8 * k + 4, 0, 1, 0]
        blob = struct.pack(f'<{len(words) + len(layer) + len(props)}i', *words, *layer, *props)
        blob += struct.pack('<6f', 0.0, 0.75, 0.0, 0.75, 0.0, 0.75) + bytes(16)
        self.assertEqual(held_extract.fxr_models(blob), [(10002, (0.75, 0.75, 0.75))])
        # a record that is not a model layer (no 11 after it) is not one
        self.assertEqual(held_extract.fxr_models(struct.pack('<12i', 0, 0, 41, 0, 10002, 0, 7, 0, 0, 0, 0, 0)), [])

    def test_tae_goods_effect(self):
        # type 114: i32 base, i16 -, i16 dummy -> ffx base * 100 + sfxVariationId; type 96: ffx, dummy
        tae = {'anims': [(100081, [(114, 0.833, 1.333, struct.pack('<ihhi', 200, 0, 1, 2)),
                                   (96, 0.333, 1.967, struct.pack('<iii', 20156, 1, 1)),
                                   (114, 0.9, 4.0, struct.pack('<ihhi', 202, 0, 220, 3))])]}
        got = held_extract.anim_effects(tae, 100081, 75)
        self.assertEqual([(ffx, dummy) for _t, ffx, dummy, _how in got], [(20156, 1), (20075, 1), (20275, 220)])
        self.assertEqual([ffx for _t, ffx, _d, _h in held_extract.anim_effects(tae, 100081, -1)], [20156])


@unittest.skipUnless(HAVE_DUMP, f'no Bloodborne dump at {GAME} (set BB_GAME_DIR)')
class DumpTests(unittest.TestCase):
    root = catalog.game_root(GAME)

    def test_skeleton_has_arms_hands_fingers(self):
        files = souls.bnd4_read((self.root / 'chr' / 'c0000.chrbnd.dcx').read_bytes())
        flver = souls.flver_read(extract.first_flver(files, 'c0000').data, decode_vertices=False)
        names = [b.name for b in flver.bones]
        for chain in extract.CHAINS.values():
            for bone in chain:
                self.assertIn(bone, names)
        worlds = souls.bone_worlds(flver.bones)
        hand = souls.transform_point(worlds[names.index('R_Hand')], (0, 0, 0))
        self.assertAlmostEqual(hand[0], -0.563, places=2)  # FromSoftware LH: right at -X
        self.assertAlmostEqual(hand[1], 1.049, places=2)

    def test_gloves_decode(self):
        files = souls.bnd4_read((self.root / 'parts' / 'am_m_2200.partsbnd.dcx').read_bytes())
        flver = souls.flver_read(extract.first_flver(files, 'am_m_2200').data)
        mesh = flver.meshes[0]
        self.assertGreater(len(mesh.positions), 1000)
        for ws in mesh.bone_weights[::97]:
            self.assertAlmostEqual(sum(ws), 1.0, delta=0.02)
        for n in mesh.normals[::97]:
            self.assertAlmostEqual(math.sqrt(sum(c * c for c in n)), 1.0, delta=0.03)

    def test_catalog_names(self):
        cat = catalog.build(GAME)
        models = {w['name']: w['model'] for w in cat['weapons']}
        self.assertEqual(models['Saw Cleaver'], 'wp_a_0800')
        self.assertEqual(models['Hunter Axe'], 'wp_a_1020')
        self.assertEqual(models['Hunter Pistol'], 'wp_a_2500')

    def test_extract(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            body, info = extract.extract_body(self.root, extract.OUTFITS['hunter'], 'm',
                                              'hunter', out, quiet=True)
            doc, blob = bbmodel.load(body)
            names = {b['name']: i for i, b in enumerate(doc['bones'])}
            for bone in ('R_Hand', 'R_Finger12', 'L_Finger02', 'R_Weapon', 'Head'):
                self.assertIn(bone, names)
            # Mirrored to right-handed: the right hand is at +X.
            self.assertGreater(doc['bones'][names['R_Hand']]['bind_world'][12], 0.5)
            self.assertIn(44, info['hidden_masks'])  # Hunter Gloves hide the bare hands
            visible = [m for m in doc['meshes'] if not m['hidden']]
            self.assertTrue(any(m['slot'] == 'arm' for m in visible))
            for _p, _n, _uv, bones, weights in list(bbmodel.vertices_of(doc, blob))[::211]:
                self.assertAlmostEqual(sum(weights), 1.0, delta=0.02)
                self.assertTrue(all(b < len(doc['bones']) for b in bones))
            cat = catalog.build(GAME)
            cleaver = extract.pick_weapons(cat, ['Saw Cleaver'])[0]
            wdoc, _ = bbmodel.load(extract.extract_weapon(self.root, cleaver['model'], cleaver,
                                                          out))
            refs = {d['ref'] for d in wdoc['dummies']}
            self.assertTrue({100, 110}.issubset(refs))

    def test_held_models(self):
        """The game's own in-hand models (held_extract.py): the use-animation chain, the real sizes."""
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            index = held_extract.export(self.root, out, quiet=True)
            self.assertEqual(index['use_anims']['26'], ['a000_100080'])  # Blood Vial
            self.assertEqual(index['use_anims']['3'], ['a000_100030'])   # the bottles' throw
            rows = {r['goods']: r for r in index['items']}
            expect = {1000: 10002, 703: 10002, 1101: 10022, 1120: 10017, 2030: 10023, 1200: 10010,
                      1220: 10014, 1230: 10012, 1240: 10016, 1250: 10010, 1260: 10015, 1270: 10009,
                      1201: 10051, 200: 10005, 205: 10006, 225: 10007, 2050: 10025, 2090: 10020, 700: 11012}
            for goods, mesh in expect.items():
                self.assertIn(goods, rows)
                self.assertEqual(rows[goods]['meshes'], [mesh], goods)
                self.assertEqual(rows[goods]['confidence'], 'game', goods)
                self.assertIsNotNone(rows[goods]['socket'], goods)
                self.assertIsNotNone(rows[goods]['grip_socket'], goods)
            # the FLVER bone transform is applied: the vial is 17.6 cm, not 61 units
            self.assertAlmostEqual(max(rows[1000]['size_m']), 0.176, delta=0.01)
            self.assertAlmostEqual(max(rows[1200]['size_m']), 0.296, delta=0.01)
            self.assertEqual(rows[1000]['chain']['anim'], 'a000_100080')
            self.assertEqual(rows[1000]['chain']['ffx'], 20156)
            self.assertEqual(rows[1101]['chain']['ffx'], 20057)  # 200 x 100 + sfxVariationId 57
            # no model in the game: the stand-ins stay (papers, pellets, coldblood)
            missing = {m['goods'] for m in index['missing']}
            self.assertTrue({1300, 1310, 1320, 1330, 1110, 1500}.issubset(missing))
            self.assertEqual(rows[1100]['confidence'], 'guess')
            doc, _blob = bbmodel.load(out / 'held' / '1000.json')
            self.assertEqual(doc['meshes'][0].get('texture'), 's10002')


if __name__ == '__main__':
    unittest.main()
