"""Play modes (VR / Co-op / Flat), VR quality presets, render mode and the page layout of the start-up window
(tools/release/zip/bbvr_settings.py, bbvr_menu.py, bbvr_cli.py, bbvr_launcher.py)."""
from paths import ROOT
import argparse
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ZIP = ROOT / 'tools' / 'release' / 'zip'
sys.path.insert(0, str(ZIP))
import bbvr_cli  # noqa: E402
import bbvr_launcher as L  # noqa: E402
import bbvr_menu  # noqa: E402
import bbvr_settings as S  # noqa: E402

NO_GPU = {'gpus': [], 'dlss': {'ok': False, 'reason': 'no RTX', 'exe': None, 'native': False, 'dir': None},
          'runtimes': {'active': None, 'runtimes': [], 'warnings': []}}
RTX = {'gpus': [{'name': 'NVIDIA GeForce RTX 4090', 'rtx': True, 'driver': '551.86'}],
       'dlss': {'ok': True, 'reason': '', 'exe': Path('bb-probe.exe'), 'native': True, 'dir': Path('.')},
       'runtimes': {'active': None, 'runtimes': [], 'warnings': []}}
# the same PC with the player's own nvngx_dlssnr.dll in place (never shipped)
RTX_NR = dict(RTX, dlssnr={'ok': True, 'reason': '', 'dir': Path('.')})
RTX_NO_NR = dict(RTX, dlssnr={'ok': False, 'reason': S.NR_DLL_MISSING,
                              'dir': Path('.')})


def paths_in(tmp):
    return S.make_paths(Path(tmp), Path(tmp) / 'game')


def iq_tokens(values, caps, tmp):
    plan = S.build_launch(values, paths_in(tmp), caps, public=False)
    return {k: v for k, v in plan['files']['iq'].items() if v is not None}, plan


class PresetTests(unittest.TestCase):
    def test_high_is_what_the_project_plays_today(self):
        with tempfile.TemporaryDirectory() as tmp:
            iq, plan = iq_tokens(S.effective_values(S.default_settings(), RTX), RTX, tmp)
        for key, want in {'upscale': 'dlss', 'aa': 'fxaa', 'sharpen': '0.5', 'dlss_quality': 'quality', 'dlss_preset': 'K',
                          'dlss_sharpen': '0.2', 'dlss_mask': '1', 'dlss_mask_m': '0.9', 'dlssnr': '0'}.items():
            self.assertEqual(iq.get(key), want, key)
        self.assertNotIn('res', iq)
        self.assertEqual(plan['env']['BB_XR_UPSCALE'], 'dlss')

    def test_high_without_dlss_is_the_old_fsr1_default(self):
        v = S.effective_values(S.default_settings(), NO_GPU)
        self.assertEqual((v['upscale'], v['sharpen'], v['eye_res'], v['aa']), ('fsr1', 0.5, None, 'fxaa'))

    def test_every_preset_builds_with_and_without_dlss_and_dlssnr_stays_off(self):
        with tempfile.TemporaryDirectory() as tmp:
            for caps in (RTX, NO_GPU):
                for q in S.QUALITY_ORDER:
                    if q in S.NR_PRESETS and caps is RTX:
                        continue  # needs DLSS-NR: see the Super High tests
                    s = S.set_quality(S.default_settings(), q)
                    iq, plan = iq_tokens(S.effective_values(s, caps), caps, tmp)
                    self.assertEqual((plan['env']['BB_XR_MODE'], plan['env']['BB_STEREO_FULL']), ('stereo', '1'), q)  # true stereo
                    self.assertEqual(plan['env']['BB_XR_DLSSNR'], '0', q)  # every preset but Super High keeps DLSS-NR off
                    self.assertEqual(iq['dlssnr'], '0')
                    self.assertEqual(plan['env']['BB_XR_UPSCALE'], 'dlss' if caps is RTX else 'fsr1', q)
                    self.assertEqual(plan['warnings'], [], q)  # no "DLSS not available" note: the preset already picked FSR 1

    def test_presets_climb_the_eye_image_and_keep_the_documented_dlss_modes(self):
        for caps in (RTX, NO_GPU):
            table = S.preset_table(caps)
            sizes = [S.eye_image(p['values'])[0] for p in table['presets']]
            self.assertEqual(sizes, sorted(sizes), caps)
            self.assertEqual(len(set(sizes)), 3)  # Super High has High's eye image (it adds DLSS-NR)
        self.assertEqual(S.QUALITY_ORDER, ['Low', 'Medium', 'High', 'Super High'])
        self.assertNotIn('Ultra', S.QUALITY_PRESETS)
        modes = [p['values']['dlss_quality'] for p in S.preset_table(RTX)['presets']]
        self.assertEqual(modes, ['dlaa', 'auto', 'quality', 'quality'])
        self.assertEqual(S.eye_image({'upscale': 'dlss', 'dlss_quality': 'quality'}), (2880, 1620))
        self.assertEqual(S.eye_image({'upscale': 'dlss', 'dlss_quality': 'performance'}), (3840, 2160))
        self.assertEqual(S.eye_image({'upscale': 'fsr1', 'eye_res': None}), (3840, 2160))
        self.assertEqual(S.eye_image({'upscale': 'fsr1', 'eye_res': '1920x1080'}), (1920, 1080))

    def test_preset_keys_are_real_options_and_in_the_game_vocabulary(self):
        for k in S.PRESET_KEYS:
            self.assertIn(k, S.BY_ID)
        for q, p in S.QUALITY_PRESETS.items():
            self.assertIn(p['dlss']['dlss_quality'], S.BY_ID['dlss_quality']['choices'])
            self.assertEqual(p['dlss']['upscale'], 'dlss')
            self.assertEqual(p['fsr1']['upscale'], 'fsr1')

    def test_choosing_a_preset_drops_the_edits_of_what_it_sets_and_keeps_the_rest(self):
        s = S.apply_changes(S.default_settings(), {'dlss_sharpen': 0.9, 'turn': 'snap', 'sharpen': 0.1}, RTX)
        self.assertEqual(S.effective_values(s, RTX)['dlss_sharpen'], 0.9)
        self.assertEqual(S.modified_preset_keys(s), ['dlss_sharpen', 'sharpen'])
        s = S.set_quality(s, 'Low')
        v = S.effective_values(s, RTX)
        self.assertEqual((v['dlss_quality'], v['dlss_sharpen']), ('dlaa', 0.3))
        self.assertEqual(v['turn'], 'snap')
        self.assertEqual(S.modified_preset_keys(s), [])

    def test_an_edit_back_to_the_reference_value_is_not_an_override(self):
        s = S.apply_changes(S.default_settings(), {'fps': 90}, NO_GPU)
        self.assertEqual(s['overrides'], {'fps': 90})
        s = S.apply_changes(s, {'fps': 72}, NO_GPU)
        self.assertEqual(s['overrides'], {})

    def test_the_older_profiles_still_work_over_a_preset(self):
        s = dict(S.default_settings(), profile='Performance')
        v = S.effective_values(s, NO_GPU)
        self.assertEqual((v['eye_res'], v['patch_lod1']), ('1.5', True))
        # picking a preset retires the two profiles it replaces
        self.assertEqual(S.set_quality(s, 'Super High')['profile'], 'Default')
        self.assertEqual(S.set_quality(dict(S.default_settings(), profile='Comfort'), 'Super High')['profile'], 'Comfort')


class SuperHighTests(unittest.TestCase):
    def test_super_high_is_high_plus_dlss_nr_with_the_keys_that_exist_on_vr(self):
        v = S.preset_values('Super High', True)
        high = S.preset_values('High', True)
        self.assertEqual({k: v[k] for k in v if v[k] != high.get(k)}, {'dlssnr': True, 'dlssnr_place': 'pre'})
        want = {'upscale': 'dlss', 'dlss_quality': 'quality', 'dlss_preset': 'K', 'dlss_sharpen': 0.2, 'aa': 'fxaa',
                'dlss_mask': True, 'dlss_mask_m': 0.9, 'dlssnr': True, 'dlssnr_passes': 1, 'dlssnr_place': 'pre'}
        self.assertEqual({k: v[k] for k in want}, want)
        self.assertEqual(high['dlssnr_place'], 'post')
        self.assertEqual(S.eye_image(v), (2880, 1620))
        for k in v:
            self.assertIn(k, S.BY_ID)
        # without DLSS (FSR 1) it carries no DLSS-NR at all
        self.assertFalse(S.preset_values('Super High', False)['dlssnr'])

    def test_with_the_dll_it_launches_dlss_nr_on_one_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            s = S.set_quality(S.default_settings(), 'Super High')
            iq, plan = iq_tokens(S.effective_values(s, RTX_NR), RTX_NR, tmp)
        self.assertEqual((plan['env']['BB_XR_DLSSNR'], plan['env']['BB_XR_DLSSNR_PASSES'], plan['warnings']), ('1', '1', []))
        self.assertEqual((plan['env']['BB_XR_DLSSNR_PLACE'], plan['env']['BB_XR_MODE'], plan['env']['BB_STEREO_FULL']), ('pre', 'stereo', '1'))
        self.assertEqual((iq['dlssnr'], iq['dlssnr_passes'], iq['dlssnr_place'], iq['dlss_quality']), ('1', '1', 'pre', 'quality'))

    def test_the_card_is_greyed_out_with_the_reason_when_the_dll_is_missing(self):
        table = {p['name']: p for p in S.preset_table(RTX_NO_NR)['presets']}
        self.assertFalse(table['Super High']['available'])
        self.assertIn('nvngx_dlssnr.dll', table['Super High']['reason'])
        self.assertTrue(all(table[n]['available'] and not table[n]['reason'] for n in ('Low', 'Medium', 'High')))
        self.assertTrue({p['name']: p for p in S.preset_table(RTX_NR)['presets']}['Super High']['available'])
        self.assertFalse({p['name']: p for p in S.preset_table(NO_GPU)['presets']}['Super High']['available'])
        self.assertIn('nvngx_dlssnr.dll', S.QUALITY_NOTES['Super High'])  # the player's own DLL, not shipped
        self.assertEqual(table['Super High']['rows'][-1], ['DLSS-NR', 'on, 1 pass, before upscaling'])
        self.assertEqual(table['High']['rows'][-1], ['DLSS-NR', 'off'])

    def test_measured_fps_is_on_the_cards(self):
        table = {p['name']: p for p in S.preset_table(RTX_NR)['presets']}
        self.assertEqual(table['Super High']['fps'], 'about 50-54 fps on an RTX 4090, full stereo')
        self.assertIn('50-54 fps on an RTX 4090', S.QUALITY_NOTES['Super High'])
        self.assertEqual((table['Low']['fps'], table['High']['fps']), ('', ''))

    def test_a_stale_click_on_the_greyed_card_does_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            menu = bbvr_menu.Menu(Path(tmp) / 'u' / 's.json', paths_in(tmp), RTX_NO_NR)
            menu.update({'quality': 'Super High'})
            self.assertEqual(menu.settings['quality'], 'High')
            self.assertIn('not available', menu.status)
            menu.caps = RTX_NR
            menu.update({'quality': 'Super High'})
            self.assertEqual(menu.settings['quality'], 'Super High')


class SettingsFileTests(unittest.TestCase):
    def test_an_older_file_loads_unchanged_and_the_new_keys_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / 'bbvr_settings.json'
            f.write_text(json.dumps({'version': 1, 'profile': 'Comfort', 'custom': {}, 'show_menu': False}), encoding='utf-8')
            s = S.load_settings(f)
            self.assertEqual((s['profile'], s['show_menu'], s['mode'], s['coop_view'], s['quality'], s['overrides']),
                             ('Comfort', False, 'vr', 'vr', 'High', {}))
            s = S.apply_changes(S.set_quality(dict(s, mode='coop', coop_view='flat'), 'Medium'), {'fps': 90}, NO_GPU)
            S.save_settings(f, s)
            back = S.load_settings(f)
            self.assertEqual((back['mode'], back['coop_view'], back['quality'], back['overrides']), ('coop', 'flat', 'Medium', {'fps': 90}))
            data = json.loads(f.read_text(encoding='utf-8'))
            for key in ('profile', 'custom', 'show_menu'):  # the older keys are still there for an older launcher
                self.assertIn(key, data)

    def test_a_version_1_custom_profile_gets_the_new_stereo_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / 's.json'
            old = dict(S.effective_values(S.default_settings(), NO_GPU), mode='alt', turn='snap')  # what the old dashboard saved
            f.write_text(json.dumps({'version': 1, 'profile': 'Custom', 'custom': old, 'show_menu': True}), encoding='utf-8')
            v = S.effective_values(S.load_settings(f), NO_GPU)
            self.assertEqual((v['mode'], v['turn']), ('stereo', 'snap'))
            f.write_text(json.dumps({'version': 2, 'profile': 'Custom', 'custom': old, 'show_menu': True}), encoding='utf-8')
            self.assertEqual(S.effective_values(S.load_settings(f), NO_GPU)['mode'], 'alt')  # a deliberate choice since then

    def test_garbage_values_fall_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / 's.json'
            f.write_text(json.dumps({'mode': 'x', 'coop_view': 3, 'quality': 'Insane', 'overrides': {'nope': 1, 'fps': 80}}),
                         encoding='utf-8')
            s = S.load_settings(f)
            self.assertEqual((s['mode'], s['coop_view'], s['quality'], s['overrides']), ('vr', 'vr', 'High', {'fps': 80}))


class RenderModeTests(unittest.TestCase):
    def test_stereo_is_the_default_with_full_stereo_on(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = S.build_launch(S.effective_values(S.default_settings(), NO_GPU), paths_in(tmp), NO_GPU, public=False)['env']
        self.assertEqual((env['BB_XR_MODE'], env['BB_STEREO_FULL']), ('stereo', '1'))

    def test_mono_and_the_hidden_alt(self):
        with tempfile.TemporaryDirectory() as tmp:
            for mode in ('mono', 'alt'):
                env = S.build_launch(dict(S.effective_values(S.default_settings(), NO_GPU), mode=mode), paths_in(tmp), NO_GPU,
                                     public=False)['env']
                self.assertEqual(env['BB_XR_MODE'], mode)
                self.assertNotIn('BB_STEREO_FULL', env)
        opt = [o for o in S.page_state(S.default_settings(), NO_GPU)['options'] if o['id'] == 'mode'][0]
        self.assertEqual(opt['choices'][:2], ['stereo', 'mono'])
        self.assertEqual(opt['adv_choices'], ['alt'])
        self.assertIn('eye strain', opt['help'])


class ModeTests(unittest.TestCase):
    def test_view_follows_the_mode(self):
        for mode, coop_view, view in (('vr', 'flat', 'vr'), ('flat', 'vr', 'flat'), ('coop', 'vr', 'vr'), ('coop', 'flat', 'flat')):
            self.assertEqual(S.play_view({'mode': mode, 'coop_view': coop_view}), view)

    def test_flat_plan_has_no_vr_variables_hooks_or_control_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            values = dict(S.effective_values(S.default_settings(), RTX), flat_fps='120')
            plan = S.build_launch(values, paths_in(tmp), RTX, public=False, view='flat')
            self.assertEqual(plan['view'], 'flat')
            self.assertEqual(plan['hooks'], [])
            self.assertEqual(plan['files'], {'fp': {}, 'stereo': {}, 'iq': {}})
            self.assertEqual(set(plan['env']) - {'BB_PROBE'}, {'BB_GAME_DIR', 'BB_FPS_LIMIT'})
            self.assertEqual(plan['env']['BB_FPS_LIMIT'], '120')
            self.assertFalse([k for k in plan['env'] if k.startswith(('BB_VR', 'BB_XR', 'BB_STEREO'))])
            paths = paths_in(tmp)
            self.assertEqual(S.apply_files(plan, paths), [])
            self.assertFalse(paths['iq'].exists())

    def test_menu_update_takes_mode_quality_and_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            menu = bbvr_menu.Menu(Path(tmp) / 'u' / 's.json', paths_in(tmp), NO_GPU)
            menu.update({'mode': 'coop', 'coop_view': 'flat', 'quality': 'Medium', 'set': {'fps': 90, 'nonsense': 1}})
            saved = S.load_settings(menu.settings_path)
            self.assertEqual((saved['mode'], saved['coop_view'], saved['quality'], saved['overrides']), ('coop', 'flat', 'Medium', {'fps': 90}))
            menu.update({'mode': 'bogus', 'reset': ['fps']})
            saved = S.load_settings(menu.settings_path)
            self.assertEqual((saved['mode'], saved['overrides']), ('coop', {}))
            st = menu.state()
            self.assertEqual((st['mode'], st['view'], st['quality']), ('coop', 'flat', 'Medium'))


class PageTests(unittest.TestCase):
    def test_every_option_is_on_exactly_one_page(self):
        placed = [oid for p in S.PAGES for _, ids in p.get('sections', []) for oid in ids]
        self.assertEqual(len(placed), len(set(placed)))
        self.assertEqual(set(placed), {o['id'] for o in S.SCHEMA})
        for oid in placed:
            self.assertIn(oid, S.BY_ID)

    def test_page_state_lists_pages_and_the_preset_table(self):
        st = S.page_state(S.default_settings(), NO_GPU, public=True)
        ids = [p['id'] for p in st['pages']]
        for want in ('play', 'graphics', 'display', 'comfort', 'online', 'advanced', 'log'):
            self.assertIn(want, ids)
        self.assertEqual([p['name'] for p in st['quality_table']['presets']], ['Low', 'Medium', 'High', 'Super High'])
        online_page = [p for p in st['pages'] if p['id'] == 'online'][0]
        shown = [i for s in online_page['sections'] for i in s['options']]
        self.assertEqual(shown, [])  # the bell options and the other online ones stay out of a public build
        self.assertNotIn('online_autobell', [o['id'] for o in st['options']])
        self.assertIn('online_autobell', [o['id'] for o in S.page_state(S.default_settings(), NO_GPU, public=False)['options']])
        for o in st['options']:
            self.assertIn('page', o)

    def test_new_options_exist_in_the_game(self):
        # the names the game reads: iq file (vk_xr_eye_iq.cpp, vr_game_ui.cpp) and fp file (runtime_vr_fp.c, runtime_vr_atkspeed.c)
        for oid, kind, key in (('fps_counter', 'iq', 'fps'), ('fps_pos', 'iq', 'fps_pos'), ('hud_center', 'iq', 'hud_center'),
                               ('hp_color', 'iq', 'hp_color'), ('stamina_color', 'iq', 'stamina_color'),
                               ('evade_scale', 'fp', 'evade_scale'), ('backstep_scale', 'fp', 'backstep_scale'),
                               ('backstep', 'fp', 'backstep'), ('backstep_angle', 'fp', 'backstep_angle'),
                               ('backstep_lock', 'fp', 'backstep_lock')):
            self.assertEqual(S.BY_ID[oid]['target'][:2], (kind, key))
        self.assertEqual(S.BY_ID['hud_center']['choices'], ['menus', 'subtitles', 'auto', 'on', 'off'])

    def test_new_fp_options_leave_todays_fp_file_alone(self):
        with tempfile.TemporaryDirectory() as tmp:
            plan = S.build_launch(S.effective_values(S.default_settings(), NO_GPU), paths_in(tmp), NO_GPU, public=False)
        for key in ('evade_scale', 'backstep', 'backstep_scale', 'backstep_angle', 'backstep_lock'):
            self.assertNotIn(key, plan['files']['fp'])
        self.assertEqual(plan['files']['iq']['fps'], '0')
        self.assertEqual(plan['files']['iq']['hud_center'], 'menus')
        self.assertIsNone(plan['files']['iq']['hp_color'])


class DriverTests(unittest.TestCase):
    def test_nvidia_driver_version_is_shown_the_way_nvidia_prints_it(self):
        self.assertEqual(S.friendly_driver('NVIDIA GeForce RTX 4090', '31.0.15.5186'), '551.86')
        self.assertEqual(S.friendly_driver('NVIDIA GeForce RTX 3080', '32.0.15.6094'), '560.94')
        self.assertEqual(S.friendly_driver('AMD Radeon RX 7900', '31.0.21024.1000'), '31.0.21024.1000')
        self.assertEqual(S.friendly_driver('x', ''), '')


class CliReadyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.port = Path(self.tmp.name)
        for p in (mock.patch.object(L, 'PORT', self.port), mock.patch.object(L, 'DATA', self.port),
                  mock.patch.dict('os.environ', {'BB_CRASH_DIR': str(self.port / 'crashes')}),
                  mock.patch.object(L, 'OUT', self.port / 'out'), mock.patch.object(L, 'CONFIG', self.port / 'bbvr_config.json')):
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(self.tmp.cleanup)

    def test_state_has_the_ready_check_and_pages(self):
        st = bbvr_cli.run(['state'])
        rows = {r['id']: r for r in st['ready']}
        self.assertEqual(rows['game']['status'], 'bad')  # no game folder yet
        self.assertIn(rows['headset']['status'], ('ok', 'warn', 'bad'))
        self.assertIn('vulkan', rows)
        self.assertEqual(st['app']['game']['banner'], '')
        self.assertEqual((st['mode'], st['quality'], st['view']), ('vr', 'High', 'vr'))

    def test_a_game_folder_gives_title_version_and_the_banner_of_the_players_own_dump(self):
        game = self.port / 'dump'
        (game / 'sce_sys').mkdir(parents=True)
        (game / 'eboot.bin').write_bytes(b'x')
        (game / 'sce_sys' / 'pic1.png').write_bytes(b'\x89PNG')
        st = bbvr_cli.run(['game-dir', str(game)])
        self.assertEqual(st['app']['game']['banner'], str(game / 'sce_sys' / 'pic1.png'))
        self.assertEqual({r['id']: r['status'] for r in st['ready']}['game'], 'ok')

    def test_flat_view_says_the_headset_is_not_needed(self):
        st = bbvr_cli.run(['save'], json.dumps({'mode': 'flat'}))
        rows = {r['id']: r for r in st['ready']}
        self.assertEqual(rows['headset']['status'], 'na')
        self.assertEqual(rows['dlss']['status'], 'na')
        self.assertNotIn('vrdata', rows)
        self.assertEqual(st['view'], 'flat')


class LauncherDryRunTests(unittest.TestCase):
    """bbvr_launcher.run_with_settings --dry-run for each mode: the environment it would start the game with."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.port = Path(self.tmp.name)
        self.game = self.port / 'dump'
        self.game.mkdir()
        for p in (mock.patch.object(L, 'PORT', self.port), mock.patch.object(L, 'DATA', self.port),
                  mock.patch.object(L, 'OUT', self.port / 'out'), mock.patch.object(L, 'CONFIG', self.port / 'bbvr_config.json'),
                  mock.patch.object(L, 'LOGS', self.port / 'logs'),
                  mock.patch.object(S, 'detect_all', lambda port, exe=None: dict(NO_GPU)),
                  mock.patch.dict('os.environ', {'BBVR_LOBBY_URL': ''})):
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(self.tmp.cleanup)

    def run_dry(self, **kw):
        args = argparse.Namespace(menu=False, no_menu=True, profile=None, reset_settings=False, console=True, no_browser=True,
                                  mono=False, solo=False, dry_run=True, mode=None, flat=False, quality=None)
        for k, v in kw.items():
            setattr(args, k, v)
        lines = []
        with mock.patch.object(L, 'say', lambda text='': lines.append(text)):
            code = L.run_with_settings(self.game, args)
        env = dict(l.split('=', 1) for l in lines if l.startswith('BB_') and '=' in l)
        return code, env, lines

    def test_vr_stereo_by_default(self):
        code, env, lines = self.run_dry()
        self.assertEqual(code, 0)
        self.assertEqual((env['BB_XR_MODE'], env['BB_VR']), ('stereo', '1'))
        self.assertNotIn('BB_ONLINE', env)
        self.assertTrue(any('Play solo' in l for l in lines))

    def test_mono_flag_and_quality_flag(self):
        code, env, lines = self.run_dry(mono=True, quality='low')
        self.assertEqual(env['BB_XR_MODE'], 'mono')
        self.assertEqual(env['BB_XR_UPSCALE'], 'fsr1')
        self.assertEqual(self.run_dry(quality='nope')[0], 5)
        iq = (self.port / 'out' / 'iq.txt').read_text(encoding='utf-8')
        self.assertIn('res=1.5', iq)  # Low without DLSS: FSR 1 at 1.5x

    def test_flat_has_no_vr_environment(self):
        code, env, lines = self.run_dry(mode='flat')
        self.assertEqual(code, 0)
        self.assertNotIn('BB_VR', env)
        self.assertNotIn('BB_XR_MODE', env)
        self.assertIn('BB_GAME_DIR', env)
        self.assertTrue(any('Flat screen' in l for l in lines))

    def test_coop_without_a_room_starts_solo_with_a_note(self):
        code, env, lines = self.run_dry(mode='coop')
        self.assertNotIn('BB_ONLINE', env)
        self.assertTrue(any('no room' in l for l in lines))

    def test_coop_in_a_room_is_online_in_vr_or_flat(self):
        user = self.port / 'user'
        user.mkdir()
        (user / 'online_room.json').write_text(json.dumps({'code': 'K7Q-M4X', 'role': 'host', 'token': 't', 'server': 'http://lobby.invalid:1'}))
        (user / 'online_identity.json').write_text(json.dumps({'npid': 'Hunter_AB23', 'password': 'pw'}))
        with mock.patch('bbvr_online.sync_host_overrides', lambda *a, **k: None):
            code, env, _ = self.run_dry(mode='coop')
            self.assertEqual((env['BB_ONLINE'], env['BB_ONLINE_ROOM'], env['BB_VR']), ('1', 'K7Q-M4X', '1'))
            code, env, _ = self.run_dry(mode='coop', flat=True)
            self.assertEqual((env['BB_ONLINE'], env['BB_ONLINE_ROOM']), ('1', 'K7Q-M4X'))
            self.assertNotIn('BB_VR', env)
            code, env, _ = self.run_dry(mode='vr')  # solo with a saved room: online stays off
            self.assertNotIn('BB_ONLINE', env)


class WebMenuTests(unittest.TestCase):
    """The web window's own endpoints: banner from the player's game folder, log tail, rooms, stop."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        tmp = Path(self.tmp.name)
        self.game = tmp / 'game'
        (self.game / 'sce_sys').mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'x')
        (self.game / 'sce_sys' / 'pic1.png').write_bytes(b'\x89PNG fake picture')
        self.menu = bbvr_menu.Menu(tmp / 'user' / 's.json', S.make_paths(tmp, self.game), RTX)
        self.rooms = []
        self.menu.online = lambda argv: self.rooms.append(argv) or {'ok': True, 'in_room': False}
        self.menu.start()
        self.base = f'http://127.0.0.1:{self.menu.port}'

    def tearDown(self):
        self.menu.stop()
        self.tmp.cleanup()

    def call(self, path, body=None, token=True):
        import urllib.error
        import urllib.request
        headers = {'X-BB-Token': self.menu.token} if token else {}
        if body is not None:
            headers['Content-Type'] = 'application/json'
        req = urllib.request.Request(self.base + path, data=None if body is None else json.dumps(body).encode(), headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, b''

    def test_state_carries_the_ready_check_and_the_game_folder(self):
        st = json.loads(self.call('/api/state')[1])
        self.assertTrue(st['app']['eboot'])
        self.assertEqual({r['id']: r['status'] for r in st['ready']}['game'], 'ok')  # no param.sfo here: the folder is taken as it is
        self.assertEqual(st['app']['game']['banner'], 'yes')
        self.assertEqual((st['mode'], st['view'], st['quality']), ('vr', 'vr', 'High'))

    def test_the_banner_is_the_players_own_file_and_needs_the_token(self):
        status, data = self.call('/api/banner')
        self.assertEqual((status, data), (200, b'\x89PNG fake picture'))
        self.assertEqual(self.call('/api/banner', token=False)[0], 403)
        (self.game / 'sce_sys' / 'pic1.png').unlink()
        self.assertEqual(self.call('/api/banner')[0], 404)
        self.assertEqual(json.loads(self.call('/api/state')[1])['app']['game']['banner'], '')

    def test_page_serves_the_new_window(self):
        status, data = self.call('/?t=' + self.menu.token, token=False)
        page = data.decode('utf-8')
        self.assertEqual(status, 200)
        for text in ('PLAY', 'Co-op', 'Flat', 'VR quality', 'Stereo', 'Mono', 'eye strain or performance problems', 'Ready check',
                     'Show advanced options', 'id="stop"', '/api/log', '/api/online'):
            self.assertIn(text, page)
        self.assertNotIn('__TOKEN__', page)
        self.assertNotIn('prefers-color-scheme', page)  # one dark Bloodborne look

    def test_log_tail_is_capped(self):
        log = Path(self.tmp.name) / 'logs'
        log.mkdir()
        (log / 'bbvr-20260101-000000.log').write_text('\n'.join(f'line {i}' for i in range(7000)), encoding='utf-8')
        self.menu.log_dir = log
        lines = json.loads(self.call('/api/log')[1])['lines']
        self.assertEqual((len(lines), lines[0], lines[-1]), (6000, 'line 1000', 'line 6999'))

    def test_rooms_go_through_the_bridge_and_only_known_commands(self):
        self.assertEqual(self.call('/api/online', {'argv': ['join', 'K7Q-M4X']})[0], 200)
        self.assertEqual(self.rooms, [['join', 'K7Q-M4X'], ['view']])
        self.assertEqual(self.call('/api/online', {'argv': ['rm', '-rf']})[0], 400)
        self.assertEqual(self.call('/api/online', {'argv': 'view'})[0], 400)
        self.menu.online = None
        self.assertEqual(self.call('/api/online', {'argv': ['view']})[0], 400)

    def test_mode_and_quality_through_save(self):
        st = json.loads(self.call('/api/save', {'mode': 'flat', 'quality': 'Low'})[1])
        self.assertEqual((st['mode'], st['view'], st['quality']), ('flat', 'flat', 'Low'))
        self.assertEqual({r['id']: r['status'] for r in st['ready']}['headset'], 'na')
        st = json.loads(self.call('/api/save', {'mode': 'coop', 'coop_view': 'flat', 'set': {'mode': 'mono'}})[1])
        self.assertEqual((st['mode'], st['view'], st['values']['mode']), ('coop', 'flat', 'mono'))
        self.assertEqual(S.load_settings(self.menu.settings_path)['overrides'], {'mode': 'mono'})

    def test_stop_without_a_game_does_nothing(self):
        self.assertEqual(json.loads(self.call('/api/stop', {})[1]), {'ok': False})


class ConsoleMenuTests(unittest.TestCase):
    def run_menu(self, answers):
        answers = iter(answers)
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / 's.json'
            out = bbvr_menu.console_menu(f, NO_GPU, say=lambda *a: None, ask=lambda prompt='': next(answers))
            return out, S.load_settings(f) if out else None

    def test_mode_quality_render_mode_then_start(self):
        out, saved = self.run_menu(['c', 't', '3', 'm', ''])
        self.assertEqual((out['mode'], out['coop_view'], out['quality']), ('coop', 'flat', 'High'))
        out, saved = self.run_menu(['f', '1', 'm', ''])
        self.assertEqual((saved['mode'], saved['quality'], saved['overrides']), ('flat', 'Low', {'mode': 'mono'}))

    def test_quit(self):
        self.assertIsNone(self.run_menu(['x'])[0])


if __name__ == '__main__':
    unittest.main()
