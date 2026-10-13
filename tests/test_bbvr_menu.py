"""Start-up settings menu of the tester launcher (tools/release/zip/bbvr_settings.py, bbvr_menu.py)."""
from paths import ROOT
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

ZIP = ROOT / 'tools' / 'release' / 'zip'
sys.path.insert(0, str(ZIP))
import bbvr_menu  # noqa: E402
import bbvr_settings as S  # noqa: E402

# The env block of the old out/vr_all.sh session (kept here so the test also runs without out/).
GOLDEN_ENV = {
    'BB_XR_AA': 'fxaa', 'BB_XR_UPSCALE': 'fsr1', 'BB_XR_SHARPEN': '0.5', 'BB_VR': '1', 'BB_XR_MODE': 'stereo',
    'BB_VR_HEAD': '1', 'BB_VR_HEAD_LEVEL': '1', 'BB_VR_FP': '1', 'BB_VR_FP_LOG': '0', 'BB_VR_DASH': '1',
    'BB_VR_DASH_LOG': '0', 'BB_VR_STEREO_LOG': '0', 'BB_VR_STAMINA': 'defer', 'BB_VR_STAMINA_LOG': '0',
    'BB_VR_STAMINA_TRACE': '0', 'BB_VR_ARMS': '1', 'BB_VR_UI': '1', 'BB_VR_UI_PROBE': '0', 'BB_VR_GUN_AIM': '1',
    'BB_VR_HIT_SCALE': '1', 'BB_UPSCALER': 'off', 'BB_FPS_LIMIT': '72',
    'BB_PATCHES': 'Disable Motion Blur (perf increase);Disable AA;Disable DoF;Disable Chromatic Aberration',
    'BB_HOOKS': 'VR Camera Hook H1 (draw PersCam);VR Camera Hook H2 (draw main context);VR LOD Fix (SetCamera);'
                'VR FP Camera Heading (in-game step);VR Stamina Cost (behavior);VR Stamina Hits;'
                'VR Arms IK (AnimationToModel);VR UI Equip (HUD input);VR Gun Aim (bullet spawn);'
                'VR Hitbox Scale (attack shape);VR Attack Speed (chr dt)',
}
PATH_VARS = {'BB_VR_FP_CONTROL': 'fp.txt', 'BB_VR_STEREO_CONTROL': 'stereo.txt'}
EXTRA_VARS = {'BB_PROBE', 'BB_GAME_DIR', 'BB_VR_ASSETS', 'BB_XR_IQ_CONTROL', 'BB_XR_DLSSNR', 'BB_STEREO_FULL'}  # BB_XR_DLSSNR=0: DLSS-NR off; BB_STEREO_FULL=1: full stereo
TODAY_FP = (ZIP / 'defaults' / 'fp.txt').read_text(encoding='utf-8')
TODAY_STEREO = (ZIP / 'defaults' / 'stereo.txt').read_text(encoding='utf-8')

NO_GPU = {'gpus': [], 'dlss': {'ok': False, 'reason': 'no RTX', 'exe': None, 'native': False, 'dir': None},
          'runtimes': {'active': None, 'runtimes': [], 'warnings': []}}


def paths_in(tmp):
    return S.make_paths(Path(tmp), Path(tmp) / 'game')


def parse_vr_all(text):
    text = text.replace('\\\n', ' ')
    text = text.replace('$MODE', 'stereo')
    text = re.sub(r'\$\{(\w+):-([^}]*)\}', r'\2', text)
    text = re.sub(r'\$\{(\w+)-([^}]*)\}', r'\2', text)
    command = next(line for line in text.splitlines() if 'BB_VR=1' in line or 'BB_GAME_DIR' in line)
    start = text.index('BB_GAME_DIR=')
    end = text.index('python run.py')
    env = {}
    for tok in shlex.split(text[start:end]):
        k, _, v = tok.partition('=')
        env[k] = v
    del command
    return env


class GoldenTests(unittest.TestCase):
    def test_default_profile_is_todays_session(self):
        with tempfile.TemporaryDirectory() as tmp:
            plan = S.build_launch(S.effective_values(S.default_settings()), paths_in(tmp), NO_GPU, public=False)
        env = plan['env']
        for key, want in GOLDEN_ENV.items():
            self.assertEqual(env.get(key), want, key)
        for key, name in PATH_VARS.items():
            self.assertEqual(Path(env[key]).name, name)
        extra = set(env) - set(GOLDEN_ENV) - set(PATH_VARS) - EXTRA_VARS
        self.assertEqual(extra, set(), 'the Default profile adds no other variable')
        self.assertEqual(env['BB_XR_DLSSNR'], '0')

    def test_matches_vr_all_sh_when_present(self):
        script = ROOT / 'out' / 'vr_all.sh'
        if not script.is_file():
            self.skipTest('out/vr_all.sh not present')
        old = parse_vr_all(script.read_text(encoding='utf-8'))
        with tempfile.TemporaryDirectory() as tmp:
            env = S.build_launch(S.effective_values(S.default_settings()), paths_in(tmp), NO_GPU, public=False)['env']
        for key, want in old.items():
            if key in ('BB_PROBE', 'BB_GAME_DIR') or key.startswith('BB_DEBUG_') or key.endswith('_LOG') or want == '':
                continue  # debug-only (not in the release), debug logs (off by default in the release) or empty entries
            if key in PATH_VARS:
                self.assertEqual(Path(env[key]).name, Path(want).name)
            else:
                self.assertEqual(env.get(key), want, key)

    def test_default_leaves_todays_control_files_byte_identical(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = paths_in(tmp)
            paths['out'].mkdir()
            paths['fp'].write_text(TODAY_FP, encoding='utf-8', newline='\n')
            paths['stereo'].write_text(TODAY_STEREO, encoding='utf-8', newline='\n')
            plan = S.build_launch(S.effective_values(S.default_settings()), paths, NO_GPU, public=False)
            changed = S.apply_files(plan, paths)
            self.assertEqual(paths['fp'].read_text(encoding='utf-8'), TODAY_FP)
            self.assertEqual(paths['stereo'].read_text(encoding='utf-8'), TODAY_STEREO)
            self.assertEqual([Path(c).name for c in changed], ['iq.txt'])
            self.assertFalse(paths['fp'].with_name('fp.txt.bak').exists())


class MergeTests(unittest.TestCase):
    def test_no_clobber(self):
        text = 'fp=1 mystery=7 turn=snap\nnear=0.10  # keep\nlast=x\n'
        out = S.merge_control_text(text, {'turn': 'smooth', 'near': '0.1', 'up': '-0.08', 'gone': None})
        self.assertIn('mystery=7', out)
        self.assertIn('last=x', out)
        self.assertIn('turn=smooth', out)
        self.assertIn('near=0.10', out)  # same number: spelling kept
        self.assertTrue(out.endswith('up=-0.08\n'))
        self.assertEqual(S.merge_control_text(out, {'turn': 'smooth'}), out)

    def test_remove_and_backup_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / 'iq.txt'
            f.write_text('upscale=dlss res=2 other=1\n', encoding='utf-8')
            self.assertTrue(S.write_control(f, {'res': None, 'upscale': 'fsr1'}))
            self.assertEqual(f.read_text(encoding='utf-8'), 'upscale=fsr1 other=1\n')
            self.assertEqual(f.with_name('iq.txt.bak').read_text(encoding='utf-8'), 'upscale=dlss res=2 other=1\n')
            S.write_control(f, {'upscale': 'off'})
            self.assertIn('upscale=dlss', f.with_name('iq.txt.bak').read_text(encoding='utf-8'))

    def test_item_wheel_on_by_default(self):
        # the bridge's built-in layout has no wheel in game.active; the launcher adds it (off takes it out again)
        with tempfile.TemporaryDirectory() as tmp:
            plan = S.build_launch(S.effective_values(S.default_settings()), paths_in(tmp), NO_GPU, public=False)
            self.assertEqual(plan['ui']['active'], S.WHEEL_MENUS)
            a = Path(tmp)
            live = a / 'ui_layouts.json'
            live.write_text(json.dumps({'version': 2, 'game': {'active': S.NO_WHEEL_MENUS}}), encoding='utf-8')
            self.assertTrue(S.apply_ui_overrides(a, plan['ui']))
            self.assertIn('quick_wheel', json.loads(live.read_text())['game']['active'])
            values = dict(S.effective_values(S.default_settings()), item_wheel=False)
            off = S.build_launch(values, paths_in(tmp), NO_GPU, public=False)['ui']
            S.apply_ui_overrides(a, off)
            self.assertNotIn('quick_wheel', json.loads(live.read_text())['game']['active'])

    def test_ui_overrides_do_not_clobber_hand_edits(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = Path(tmp)
            live = a / 'ui_layouts.json'
            live.write_text(json.dumps({'version': 1, 'game': {'lock_on': False, 'hud': {'scale': 1.0, 'wrist': True}}}),
                            encoding='utf-8')
            self.assertFalse(S.apply_ui_overrides(a, {'hud.scale': 1.0}))  # nothing differs: untouched
            self.assertFalse((a / 'ui_layouts.user.json').exists())
            self.assertTrue(S.apply_ui_overrides(a, {'hud.scale': 1.5}))
            self.assertEqual(json.loads(live.read_text())['game']['hud']['scale'], 1.5)
            self.assertEqual(json.loads(live.read_text())['game']['hud']['wrist'], True)
            data = json.loads(live.read_text())
            data['game']['lock_on'] = True  # a hand edit between two runs
            live.write_text(json.dumps(data), encoding='utf-8')
            S.apply_ui_overrides(a, {'hud.scale': 1.25})
            merged = json.loads(live.read_text())['game']
            self.assertTrue(merged['lock_on'])
            self.assertEqual(merged['hud']['scale'], 1.25)
            S.apply_ui_overrides(a, {'hud.scale': 1.0})  # back to the base values: the base (with the hand edit) returns
            merged = json.loads(live.read_text())['game']
            self.assertTrue(merged['lock_on'])
            self.assertEqual(merged['hud']['scale'], 1.0)


class ProfileTests(unittest.TestCase):
    def test_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / 'u' / 'settings.json'
            s = S.apply_edit(S.default_settings(), {'turn': 'snap', 'snap': 45, 'hit_scale': 2.0})
            self.assertEqual(s['profile'], 'Custom')
            S.save_settings(f, s)
            back = S.load_settings(f)
            self.assertEqual(S.effective_values(back), S.effective_values(s))
            self.assertEqual(S.effective_values(back)['turn'], 'snap')
            self.assertEqual(S.effective_values(back)['snap'], 45.0)

    def test_unchanged_page_keeps_profile(self):
        s = S.default_settings()
        again = S.apply_edit(s, S.effective_values(s))
        self.assertEqual(again['profile'], 'Default')

    def test_every_profile_builds_and_default_is_fsr1(self):
        self.assertEqual(S.effective_values({'profile': 'Default'})['upscale'], 'fsr1')
        with tempfile.TemporaryDirectory() as tmp:
            for name in S.PROFILE_ORDER:
                S.build_launch(S.effective_values({'profile': name, 'custom': {}}), paths_in(tmp), NO_GPU, public=False)

    def test_option_ids_and_targets_are_unique(self):
        ids = [o['id'] for o in S.SCHEMA]
        self.assertEqual(len(ids), len(set(ids)))
        for name, over in S.PROFILES.items():
            for k in over:
                self.assertIn(k, S.BY_ID, name)


class DlssTests(unittest.TestCase):
    OK = {'gpus': [], 'dlss': {'ok': True, 'reason': '', 'exe': Path('bb-probe.exe'), 'native': True, 'dir': Path('.')},
          'runtimes': {'active': None, 'runtimes': [], 'warnings': []}}

    def test_default_is_dlss_when_the_dlss_check_passes_else_fsr1(self):
        on = S.effective_values(S.default_settings(), self.OK)
        self.assertEqual((on['upscale'], on['dlss_quality'], on['dlss_preset'], on['dlss_sharpen']), ('dlss', 'quality', 'K', 0.2))
        self.assertEqual(S.effective_values(S.default_settings(), NO_GPU)['upscale'], 'fsr1')
        self.assertEqual(S.effective_values(S.default_settings())['upscale'], 'fsr1')

    def test_a_saved_choice_wins_over_the_hardware_default(self):
        perf = dict(S.default_settings(), profile='Performance')
        self.assertEqual(S.effective_values(perf, self.OK)['upscale'], 'fsr1')
        custom = dict(S.default_settings(), profile='Custom', custom={'upscale': 'fsr1'})
        self.assertEqual(S.effective_values(custom, self.OK)['upscale'], 'fsr1')

    def make_exe(self, folder, marker=True, dlls=True):
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'bb-probe.exe').write_bytes(b'MZ' + (b'BB_XR_DLSS_QUALITY dlss_sharpen' if marker else b'x') + b'\0' * 64)
        if dlls:
            (folder / 'bbport_dlss.dll').write_bytes(b'x')
            (folder / 'nvngx_dlss.dll').write_bytes(b'x')

    RTX = [{'name': 'NVIDIA GeForce RTX 4090', 'rtx': True}]

    def test_greyed_without_rtx(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.make_exe(Path(tmp) / 'out')
            d = S.detect_dlss(Path(tmp), None, [{'name': 'AMD Radeon RX 7900', 'rtx': False}])
            self.assertFalse(d['ok'])
            self.assertIn('RTX', d['reason'])

    def test_before_merge_uses_out_dlss_vr_and_after_merge_the_main_exe(self):
        with tempfile.TemporaryDirectory() as tmp:
            port = Path(tmp)
            self.make_exe(port / 'out', marker=False, dlls=False)
            self.make_exe(port / 'out' / 'dlss-vr')
            d = S.detect_dlss(port, None, self.RTX)
            self.assertTrue(d['ok'])
            self.assertFalse(d['native'])
            self.assertEqual(Path(d['exe']).parent.name, 'dlss-vr')
            self.make_exe(port / 'out')  # the main build now has DLSS
            d = S.detect_dlss(port, None, self.RTX)
            self.assertTrue(d['native'])
            self.assertEqual(Path(d['exe']).parent.name, 'out')

    def test_missing_dll_reason_and_fallback_to_fsr1(self):
        with tempfile.TemporaryDirectory() as tmp:
            port = Path(tmp)
            self.make_exe(port / 'out', dlls=False)
            d = S.detect_dlss(port, None, self.RTX)
            self.assertFalse(d['ok'])
            self.assertIn('nvngx_dlss.dll', d['reason'])
            caps = dict(NO_GPU, dlss=d)
            plan = S.build_launch(S.effective_values({'profile': 'Quality (DLSS)'}), paths_in(tmp), caps, public=False)
            self.assertEqual(plan['env']['BB_XR_UPSCALE'], 'fsr1')
            self.assertTrue(plan['warnings'])
            self.assertNotIn('BB_XR_DLSS_QUALITY', plan['env'])

    def test_dlss_plan_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            port = Path(tmp)
            self.make_exe(port / 'out')
            caps = dict(NO_GPU, dlss=S.detect_dlss(port, None, self.RTX))
            vals = S.effective_values({'profile': 'Custom', 'custom': {'upscale': 'dlss', 'dlss_quality': 'performance',
                                                                        'dlss_preset': 'L', 'dlss_sharpen': 0.3, 'jitter_sign': '-1',
                                                                        'eye_res': '3840x2160'}})
            plan = S.build_launch(vals, paths_in(tmp), caps, public=False)
            env = plan['env']
            self.assertEqual((env['BB_XR_UPSCALE'], env['BB_XR_DLSS_QUALITY'], env['BB_XR_DLSS_PRESET'],
                              env['BB_XR_DLSS_SHARPEN'], env['BB_XR_DLSS_JITTER_SIGN'], env['BB_XR_EYE_RES']),
                             ('dlss', 'performance', 'L', '0.3', '-1', '3840x2160'))
            iq = plan['files']['iq']
            self.assertEqual((iq['upscale'], iq['dlss_quality'], iq['dlss_preset'], iq['dlss_sharpen'], iq['res']),
                             ('dlss', 'performance', 'L', '0.3', '3840x2160'))
            # The arm/weapon bias mask: on at 0.9 m unless changed, live through the control file.
            self.assertEqual((env['BB_XR_DLSS_MASK'], env['BB_XR_DLSS_MASK_M']), ('1', '0.9'))
            self.assertEqual((iq['dlss_mask'], iq['dlss_mask_m']), ('1', '0.9'))
            opts = {o['id']: o for o in S.SCHEMA}
            for key in ('dlss_mask', 'dlss_mask_m'):
                self.assertEqual(opts[key]['tab'], 'dlss')
                self.assertTrue(S.is_live(opts[key]))
            vals = S.effective_values({'profile': 'Custom', 'custom': {'upscale': 'dlss', 'dlss_mask': False,
                                                                        'dlss_mask_m': 1.2}})
            plan = S.build_launch(vals, paths_in(tmp), caps, public=False)
            self.assertEqual((plan['env']['BB_XR_DLSS_MASK'], plan['env']['BB_XR_DLSS_MASK_M']), ('0', '1.2'))
            self.assertEqual((plan['files']['iq']['dlss_mask'], plan['files']['iq']['dlss_mask_m']), ('0', '1.2'))

    def test_dlss_mask_keys_stay_out_without_dlss(self):
        with tempfile.TemporaryDirectory() as tmp:
            plan = S.build_launch(S.effective_values({'profile': 'Custom', 'custom': {'upscale': 'fsr1'}}),
                                  paths_in(tmp), NO_GPU, public=False)
            self.assertNotIn('dlss_mask', plan['files']['iq'])
            self.assertNotIn('BB_XR_DLSS_MASK', plan['env'])


class DlssNrTests(unittest.TestCase):
    RTX = [{'name': 'NVIDIA GeForce RTX 4090', 'rtx': True}]
    NR_IDS = ('dlssnr_passes', 'dlssnr_place', 'dlssnr_style', 'dlssnr_intensity', 'dlssnr_tone', 'dlssnr_structure', 'dlssnr_skin',
              'dlssnr_fovea', 'dlssnr_inner')

    def make(self, port, exe_nr=True, bridge_nr=True, nr_dll=True):
        out = Path(port) / 'out'
        out.mkdir(parents=True, exist_ok=True)
        (out / 'bb-probe.exe').write_bytes(b'MZ BB_XR_DLSS_QUALITY dlss_sharpen' + (b' BB_XR_DLSSNR' if exe_nr else b'') + bytes(64))
        (out / 'bbport_dlss.dll').write_bytes(b'MZ' + (b' BbDlssGetNrApi' if bridge_nr else b'') + bytes(16))
        (out / 'nvngx_dlss.dll').write_bytes(b'x')
        if nr_dll:
            (out / 'nvngx_dlssnr.dll').write_bytes(b'x')
        return out

    def test_schema(self):
        by = {o['id']: o for o in S.SCHEMA}
        want = {'dlssnr': ('BB_XR_DLSSNR', False), 'dlssnr_passes': ('BB_XR_DLSSNR_PASSES', 1),
                'dlssnr_place': ('BB_XR_DLSSNR_PLACE', 'post'),
                'dlssnr_style': ('BB_XR_DLSSNR_STYLE', '0'), 'dlssnr_intensity': ('BB_XR_DLSSNR_INTENSITY', 2.0),
                'dlssnr_tone': ('BB_XR_DLSSNR_TONE', 0.6), 'dlssnr_structure': ('BB_XR_DLSSNR_STRUCTURE', 1.0),
                'dlssnr_skin': ('BB_XR_DLSSNR_SKIN', 1.0), 'dlssnr_fovea': ('BB_XR_DLSSNR_FOVEA', 0.66),
                'dlssnr_inner': ('BB_XR_DLSSNR_INNER', 0.4)}
        for oid, (env, default) in want.items():
            o = by[oid]
            self.assertEqual((o['tab'], o['target'], o['default']), ('dlss', ('iq', oid, env), default), oid)
            if oid != 'dlssnr':
                self.assertEqual(o['only_if'], {'dlssnr': True})
        self.assertFalse(S.base_values()['dlssnr'])
        for name in S.PROFILE_ORDER:
            self.assertFalse(S.effective_values({'profile': name}, DlssTests.OK).get('dlssnr'), name)

    def test_detect(self):
        with tempfile.TemporaryDirectory() as tmp:
            port = Path(tmp)
            self.make(port)
            d = S.detect_dlssnr(port, None, self.RTX)
            self.assertTrue(d['ok'], d)
            self.assertEqual(Path(d['dir']).name, 'out')
            self.assertFalse(S.detect_dlssnr(port, None, [{'name': 'AMD', 'rtx': False}])['ok'])
            self.assertIn('needs DLSS', S.detect_dlssnr(port, None, [{'name': 'AMD', 'rtx': False}])['reason'])
        for kw, text in (({'nr_dll': False}, 'nvngx_dlssnr.dll'), ({'exe_nr': False}, 'no DLSS-NR support'),
                         ({'bridge_nr': False}, 'too old')):
            with tempfile.TemporaryDirectory() as tmp:
                self.make(Path(tmp), **kw)
                d = S.detect_dlssnr(Path(tmp), None, self.RTX)
                self.assertFalse(d['ok'])
                self.assertIn(text, d['reason'])

    def plan(self, tmp, on, ok, **custom):
        caps = dict(NO_GPU, dlss=dict(DlssTests.OK['dlss']), dlssnr={'ok': ok, 'reason': 'no dll', 'dir': None})
        caps['dlss'] = dict(caps['dlss'], dir=None)
        vals = S.effective_values({'profile': 'Custom', 'custom': dict({'upscale': 'dlss', 'dlssnr': on}, **custom)})
        return S.build_launch(vals, paths_in(tmp), caps, public=False)

    def test_on_and_available(self):
        with tempfile.TemporaryDirectory() as tmp:
            plan = self.plan(tmp, True, True, dlssnr_passes=2, dlssnr_fovea=0.5)
            env, iq = plan['env'], plan['files']['iq']
            self.assertEqual({k: v for k, v in env.items() if k.startswith('BB_XR_DLSSNR')},
                             {'BB_XR_DLSSNR': '1', 'BB_XR_DLSSNR_PASSES': '2', 'BB_XR_DLSSNR_PLACE': 'post', 'BB_XR_DLSSNR_STYLE': '0',
                              'BB_XR_DLSSNR_INTENSITY': '2', 'BB_XR_DLSSNR_TONE': '0.6', 'BB_XR_DLSSNR_STRUCTURE': '1',
                              'BB_XR_DLSSNR_SKIN': '1', 'BB_XR_DLSSNR_FOVEA': '0.5', 'BB_XR_DLSSNR_INNER': '0.4'})
            self.assertEqual((iq['dlssnr'], iq['dlssnr_passes'], iq['dlssnr_fovea'], iq['dlssnr_style']), ('1', '2', '0.5', '0'))
            self.assertEqual([w for w in plan['warnings'] if 'DLSS-NR' in w], [])

    def test_on_but_unavailable_warns_and_writes_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            plan = self.plan(tmp, True, False)
            self.assertTrue(any('DLSS-NR is not available (no dll)' in w for w in plan['warnings']))
            self.assertEqual(plan['env']['BB_XR_DLSSNR'], '0')
            self.assertEqual(plan['files']['iq']['dlssnr'], '0')
            self.assertFalse([k for k in plan['env'] if k.startswith('BB_XR_DLSSNR_')])

    def test_off_keeps_only_the_switch(self):
        with tempfile.TemporaryDirectory() as tmp:
            plan = self.plan(tmp, False, True)
            self.assertEqual(plan['env']['BB_XR_DLSSNR'], '0')
            self.assertEqual(plan['warnings'], [])
            iq = plan['files']['iq']
            self.assertEqual(iq['dlssnr'], '0')
            for k in self.NR_IDS:
                self.assertNotIn(k, iq)

    def test_note_and_state(self):
        caps = dict(NO_GPU, dlssnr={'ok': False, 'reason': 'Put it here.', 'dir': None})
        self.assertIn('Put it here.', S.dlss_note_text(caps))
        self.assertIn('available.', S.dlss_note_text(dict(NO_GPU, dlssnr={'ok': True, 'reason': '', 'dir': None})))
        st = S.page_state(S.default_settings(), caps, public=False)
        self.assertFalse(st['caps']['dlssnr']['ok'])


class RuntimeTests(unittest.TestCase):
    def test_pimax_steamvr_warning(self):
        info = {'active': r'C:\sv\steamxr_win64.json', 'warnings': [],
                'runtimes': [{'path': r'C:\sv\steamxr_win64.json', 'kind': 'steamvr', 'name': 'SteamVR', 'active': True},
                             {'path': r'C:\Pimax\PiRuntime.json', 'kind': 'pimax', 'name': 'Pimax', 'active': False}]}
        self.assertIn('double images', S.runtime_warning(info, ''))
        self.assertEqual(S.runtime_warning(info, r'C:\Pimax\PiRuntime.json'), '')

    def test_runtime_choice_sets_only_the_child_environment(self):
        before = os.environ.get('XR_RUNTIME_JSON')
        with tempfile.TemporaryDirectory() as tmp:
            plan = S.build_launch(S.effective_values({'profile': 'Custom', 'custom': {'runtime': r'C:\x\rt.json'}}),
                                  paths_in(tmp), NO_GPU, public=False)
        self.assertEqual(plan['env']['XR_RUNTIME_JSON'], r'C:\x\rt.json')
        self.assertEqual(os.environ.get('XR_RUNTIME_JSON'), before)


class DisplaySizeTests(unittest.TestCase):
    """Display size (docs/vr/findings/41-display-size.md): the render / eye sizes chosen before the game starts."""
    RTX = [{'name': 'NVIDIA GeForce RTX 4090', 'rtx': True}]
    AMD = [{'name': 'AMD Radeon RX 7800 XT', 'rtx': False}]
    PIMAX_RT = {'active': r'C:\Program Files\Pimax\Runtime\PiOpenXR_64.json', 'warnings': [],
                'runtimes': [{'path': r'C:\Program Files\Pimax\Runtime\PiOpenXR_64.json', 'name': 'Pimax OpenXR',
                              'kind': 'pimax', 'active': True}]}
    STEAM_RT = {'active': r'C:\sv\steamxr_win64.json', 'warnings': [],
                'runtimes': [{'path': r'C:\sv\steamxr_win64.json', 'name': 'SteamVR', 'kind': 'steamvr', 'active': True}]}

    def plan(self, custom, caps=NO_GPU, hw=None):
        with tempfile.TemporaryDirectory() as tmp:
            return S.build_launch(S.effective_values({'profile': 'Custom', 'custom': custom, 'hw': hw or {}}, caps),
                                  paths_in(tmp), caps, public=False)

    def test_quest3_at_100_keeps_todays_sizes(self):
        # FSR 1: the 1080p frame (no startup patch) and 3840x2160 eyes, as before Display size existed
        p = self.plan({'upscale': 'fsr1'}, hw={'display_size': 'quest3'})
        self.assertNotIn('BB_RENDER_RES', p['env'])
        self.assertEqual(p['env']['BB_XR_EYE_RES'], '3840x2160')
        self.assertEqual(p['files']['iq']['res'], '3840x2160')
        # DLSS quality: 1080p -> 2880x1620 (the game sizes the eye from the render size)
        d = S.display_sizes({'display_size': 'quest3', 'display_scale': '100', 'dlss_quality': 'quality'}, None, 'dlss')
        self.assertEqual((d['render'], d['eye'], d['native']), ([1920, 1080], [2880, 1620], True))

    def test_smaller_headset_patches_a_smaller_render_size(self):
        p = self.plan({'upscale': 'fsr1'}, hw={'display_size': 'quest2'})
        self.assertEqual(p['env']['BB_RENDER_RES'], '1668x938')
        self.assertEqual(p['env']['BB_OUTPUT_RES'], '1668x938')
        self.assertNotIn('BB_DMEM_MB', p['env'])
        self.assertEqual(p['env']['BB_XR_EYE_RES'], '3414x1920')
        self.assertEqual(p['display']['render'], [1668, 938])

    def test_above_1080p_raises_direct_memory_and_is_clamped(self):
        p = self.plan({'upscale': 'fsr1', 'display_scale': '150'}, hw={'display_size': 'dreamair'})
        w, h = (int(x) for x in p['env']['BB_RENDER_RES'].split('x'))
        self.assertLessEqual(h, 1440)
        self.assertGreater(w * h, 1920 * 1080)
        self.assertEqual(p['env']['BB_DMEM_MB'], '9152')
        self.assertLessEqual(int(p['env']['BB_XR_EYE_RES'].split('x')[1]), 2880)
        for name in S.HEADSET_BY_ID:  # every preset at every step: 16:9, even, render <= eye, inside the limits
            for sc in S.DISPLAY_SCALES:
                for up in ('fsr1', 'dlss', 'off'):
                    d = S.display_sizes({'display_size': name, 'display_scale': sc}, None, up)
                    (rw, rh), (ew, eh) = d['render'], d['eye']
                    self.assertTrue(540 <= rh <= 1440 and 720 <= eh <= 2880 and rh <= eh, (name, sc, up, d))
                    self.assertTrue(rw % 2 == 0 and rh % 2 == 0 and abs(rw / rh - 16 / 9) < 0.01, (name, sc, d))

    def test_game_default_and_unknown_headset_change_nothing(self):
        base = self.plan({})['env']
        self.assertNotIn('BB_XR_EYE_RES', base)  # Auto, no runtime and no log: today's session (golden test too)
        game = self.plan({'eye_res': '1.5'}, hw={'display_size': 'game'})['env']
        self.assertEqual(game['BB_XR_EYE_RES'], '1.5')
        self.assertNotIn('BB_RENDER_RES', game)

    def test_dlss_ratio_sets_the_render_size(self):
        caps = dict(NO_GPU, dlss={'ok': True, 'reason': '', 'exe': Path('bb-probe.exe'), 'native': True, 'dir': Path('.')})
        p = self.plan({'upscale': 'dlss', 'dlss_quality': 'performance'}, caps, hw={'display_size': 'quest3'})
        self.assertEqual(p['upscale'], 'dlss')
        self.assertEqual(p['env']['BB_RENDER_RES'], '1422x800')  # 2846x1601 eye / 2

    def test_custom_size_and_extra_env_override(self):
        p = self.plan({'upscale': 'fsr1'}, hw={'display_size': 'custom', 'display_custom': '1832x1920'})
        self.assertEqual(p['env']['BB_RENDER_RES'], '1668x938')
        self.assertIsNone(S.display_sizes({'display_size': 'custom', 'display_custom': 'big'}))
        p = self.plan({'upscale': 'fsr1', 'extra_env': 'BB_RENDER_RES=2560x1440'}, hw={'display_size': 'quest2'})
        self.assertEqual(p['env']['BB_RENDER_RES'], '2560x1440')

    def test_gpu_tier_and_performance_scale(self):
        self.assertEqual(S.effective_values(S.default_settings(), dict(NO_GPU, gpus=self.RTX))['display_scale'], '100')
        self.assertEqual(S.effective_values(S.default_settings(), dict(NO_GPU, gpus=self.AMD))['display_scale'], '85')
        self.assertEqual(S.effective_values({'profile': 'Performance'}, dict(NO_GPU, gpus=self.RTX))['display_scale'], '70')

    def test_headset_choice_is_sticky_across_profiles(self):
        s = S.apply_edit(S.default_settings(), dict(S.effective_values(S.default_settings()), display_size='quest2'))
        self.assertEqual(s['profile'], 'Default')
        self.assertEqual(s['hw'], {'display_size': 'quest2'})
        s['profile'] = 'Performance'
        self.assertEqual(S.effective_values(s)['display_size'], 'quest2')
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / 'settings.json'
            S.save_settings(f, s)
            self.assertEqual(S.load_settings(f)['hw'], {'display_size': 'quest2'})
        back = S.apply_edit(s, dict(S.effective_values(s), display_size='game'))  # the default (native 1080p)
        self.assertEqual(back['hw'], {})

    def write_log(self, folder, runtime, headset, rec):
        (Path(folder) / 'bbvr-20261011-101704.log').write_text(
            f'Patches: x\nVR: runtime {runtime}\nVR: headset {headset}, max swapchain 16384x16384\n'
            f'VR: eye images: recommended {rec} and {rec}, max 16384x16384\n', encoding='utf-8')

    def test_auto_uses_the_last_session_on_the_same_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.write_log(tmp, 'Pimax OpenXR 0.1.0', 'Pimax Dream Air', '3832x3076')
            det = S.detect_headset(self.PIMAX_RT, Path(tmp))
            self.assertEqual((det['source'], det['rec'], det['preset']), ('log', [3832, 3076], 'dreamair'))
            # another runtime is active now: its typical headset instead of the old log
            det = S.detect_headset(self.STEAM_RT, Path(tmp))
            self.assertEqual((det['source'], det['preset'], det['rec']), ('runtime', 'index', [2016, 2240]))
        self.assertIsNone(S.detect_headset({'active': None, 'runtimes': []}, None)['rec'])
        caps = dict(NO_GPU, headset={'source': 'log', 'rec': [2064, 2208], 'name': 'Meta Quest 3'})
        self.assertEqual(self.plan({'upscale': 'fsr1'}, caps, hw={'display_size': 'auto'})['env']['BB_XR_EYE_RES'], '3840x2160')

    def test_steamvr_is_the_default_runtime_except_on_pimax(self):
        self.assertEqual(S.BY_ID['runtime']['default'], 'steamvr')
        self.assertEqual(S.BY_ID['display_size']['default'], 'game')
        steam = {'active': r'C:\oc\oculus_openxr_64.json', 'warnings': [],
                 'runtimes': [{'path': r'C:\sv\steamxr_win64.json', 'name': 'SteamVR', 'kind': 'steamvr', 'active': False}]}
        self.assertEqual(S.resolve_runtime('steamvr', steam), r'C:\sv\steamxr_win64.json')
        pimax = dict(steam, active=r'C:\Program Files\Pimax\Runtime\PiRuntime.json')
        self.assertEqual(S.resolve_runtime('steamvr', pimax), '')  # Pimax keeps its own runtime
        self.assertEqual(S.resolve_runtime('steamvr', {'active': None, 'runtimes': []}), '')  # no SteamVR: the system's
        self.assertEqual(S.resolve_runtime('', steam), '')
        self.assertEqual(S.resolve_runtime(r'C:\x\vdxr.json', steam), r'C:\x\vdxr.json')

    def test_runtime_kinds_and_headset_names(self):
        self.assertEqual(S.classify_runtime(r'C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json',
                                            'VirtualDesktopXR'), 'vdxr')
        self.assertEqual(S.classify_runtime(r'C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_64.json',
                                            'Oculus'), 'oculus')
        self.assertEqual(S.classify_runtime(r'C:\Varjo\VarjoOpenXR.json', 'Varjo'), 'varjo')
        for name, want in (('Meta Quest 3', 'quest3'), ('Meta Quest 3S', 'quest2'), ('Oculus Quest2', 'quest2'),
                           ('Meta Quest Pro', 'questpro'), ('Oculus Quest', 'quest1'), ('Pimax Dream Air', 'dreamair'),
                           ('Valve Index', 'index'), ('SteamVR/OpenXR : lighthouse', None)):
            self.assertEqual(S.headset_preset(name), want, name)

    def test_page_state_reports_detection_and_sizes(self):
        caps = dict(NO_GPU, headset={'kind': 'oculus', 'source': 'runtime', 'rec': [1832, 1920],
                                     'name': 'Meta Quest 2 / Quest 3S', 'preset': 'quest2'})
        s = S.default_settings()
        s['hw'] = {'display_size': 'auto'}  # the default is 'game' (native 1080p); Auto sizes for the headset
        st = S.page_state(s, caps, public=False)
        self.assertEqual(st['display']['detected']['runtime_label'], 'Meta Quest Link / Air Link')
        self.assertEqual(st['display']['sizes']['render'], [1668, 938])
        opt = next(o for o in st['options'] if o['id'] == 'display_size')
        self.assertEqual(opt['tab'], 'display')
        self.assertIn('quest3', opt['choices'])


class PublicTests(unittest.TestCase):
    def test_no_online_options_in_public_page_data(self):
        st = S.page_state(S.default_settings(), NO_GPU, public=True)
        self.assertFalse([o for o in st['options'] if o.get('online')])
        self.assertNotIn('online', st['values'])
        self.assertNotIn('BB_ONLINE', json.dumps(st))
        self.assertTrue([o for o in S.page_state(S.default_settings(), NO_GPU, public=False)['options'] if o.get('online')])

    def test_online_value_is_dropped_in_public_launch(self):
        vals = S.effective_values({'profile': 'Custom', 'custom': {'online': True, 'extra_env': 'BB_FOO=1'}})
        with tempfile.TemporaryDirectory() as tmp:
            pub = S.build_launch(vals, paths_in(tmp), NO_GPU, public=True)
            priv = S.build_launch(vals, paths_in(tmp), NO_GPU, public=False)
        self.assertNotIn('BB_ONLINE', pub['env'])
        self.assertNotIn('BB_FOO', pub['env'])
        self.assertEqual(priv['env'].get('BB_ONLINE'), '1')

    def test_export_flag_line_exists(self):
        # export_public.py flips this line to True in the public tree (the private repo keeps False)
        self.assertRegex((ZIP / 'bbvr_settings.py').read_text(encoding='utf-8'), r'(?m)^PUBLIC_BUILD = (False|True)')


class ServerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        paths = paths_in(self.tmp.name)
        self.menu = bbvr_menu.Menu(Path(self.tmp.name) / 'user' / 's.json', paths, NO_GPU)
        self.menu.start()
        self.base = f'http://127.0.0.1:{self.menu.port}'

    def tearDown(self):
        self.menu.stop()
        self.tmp.cleanup()

    def call(self, path, body=None, token=True, host=None, ctype='application/json'):
        headers = {}
        if token:
            headers['X-BB-Token'] = self.menu.token
        if host:
            headers['Host'] = host
        if body is not None:
            headers['Content-Type'] = ctype
        req = urllib.request.Request(self.base + path, data=None if body is None else json.dumps(body).encode(), headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, b''

    def test_loopback_only(self):
        self.assertEqual(self.menu.httpd.server_address[0], '127.0.0.1')

    def test_token_and_host_checks(self):
        self.assertEqual(self.call('/api/state', token=False)[0], 403)
        self.assertEqual(self.call('/api/state', host='evil.example')[0], 403)
        self.assertEqual(self.call('/api/state')[0], 200)
        self.assertEqual(self.call('/?t=' + self.menu.token, token=False)[0], 200)
        self.assertEqual(self.call('/api/save', {'show_menu': False}, ctype='text/plain')[0], 403)
        self.assertEqual(self.call('/api/save', {'show_menu': False}, token=False)[0], 403)

    def test_save_and_state(self):
        status, data = self.call('/api/save', {'values': dict(S.effective_values(S.default_settings()), turn='snap')})
        self.assertEqual(status, 200)
        st = json.loads(data)
        self.assertEqual(st['profile'], 'Custom')
        self.assertEqual(st['values']['turn'], 'snap')
        self.assertEqual(S.load_settings(self.menu.settings_path)['profile'], 'Custom')
        st = json.loads(self.call('/api/save', {'profile': 'Comfort'})[1])
        self.assertEqual(st['values']['anim_lock'], '2')
        self.assertFalse(json.loads(self.call('/api/save', {'show_menu': False})[1])['show_menu'])

    def test_state_has_greyed_dlss_reason_and_note(self):
        st = json.loads(self.call('/api/state')[1])
        self.assertFalse(st['caps']['dlss']['ok'])
        self.assertIn('57 fps', st['dlss_note'])
        tabs = [t['id'] for t in st['tabs']]
        self.assertEqual(tabs, ['display', 'dlss', 'comfort', 'combat', 'body', 'ui', 'patches', 'advanced'])

    def test_launch_sets_event(self):
        self.assertEqual(self.call('/api/launch', {})[0], 200)
        self.assertTrue(self.menu.launch_event.is_set())

    def test_live_write_while_running(self):
        paths = self.menu.paths
        paths['out'].mkdir(parents=True, exist_ok=True)
        paths['fp'].write_text('fp=1 turn=smooth keep=1\n', encoding='utf-8')
        self.menu.game_running = True
        self.call('/api/save', {'values': dict(S.effective_values(S.default_settings()), turn='snap')})
        self.assertIn('turn=snap', paths['fp'].read_text(encoding='utf-8'))
        self.assertIn('keep=1', paths['fp'].read_text(encoding='utf-8'))


class PortabilityTests(unittest.TestCase):
    CODE = ("import sys; sys.modules['tkinter'] = None; sys.path.insert(0, %r); import bbvr_settings as S, bbvr_menu; "
            "import tempfile; p = S.make_paths(tempfile.mkdtemp(), 'g'); "
            "plan = S.build_launch(S.effective_values(S.default_settings()), p, None); print(plan['env']['BB_XR_UPSCALE'])")

    def test_no_tkinter_and_runs_isolated(self):
        for name in ('bbvr_settings.py', 'bbvr_menu.py', 'bbvr_launcher.py'):
            self.assertNotIn('tkinter', (ZIP / name).read_text(encoding='utf-8').replace('does not use tkinter', ''))
        r = subprocess.run([sys.executable, '-I', '-c', self.CODE % str(ZIP)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), 'fsr1')

    def test_embeddable_python_when_available(self):
        zips = list((ROOT / 'out' / 'release_cache').glob('python-embed*.zip'))
        if not zips:
            self.skipTest('no cached embeddable Python')
        import zipfile
        with tempfile.TemporaryDirectory() as tmp:
            zipfile.ZipFile(zips[0]).extractall(tmp)
            exe = Path(tmp) / 'python.exe'
            r = subprocess.run([str(exe), '-c', self.CODE % str(ZIP)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), 'fsr1')


if __name__ == '__main__':
    unittest.main()
