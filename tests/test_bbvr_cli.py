"""Machine interface of the native dashboard (tools/release/zip/bbvr_cli.py): the same settings / environment as the
web menu, without a window. The launcher's paths are pointed at a temp folder, so nothing real is touched."""
from paths import ROOT
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
import bbvr_settings as S  # noqa: E402


class CliTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.port = Path(self.tmp.name)
        patches = [mock.patch.object(L, 'PORT', self.port), mock.patch.object(L, 'DATA', self.port),
                   mock.patch.dict('os.environ', {'BB_CRASH_DIR': str(self.port / 'crashes')}),
                   mock.patch.object(L, 'OUT', self.port / 'out'),
                   mock.patch.object(L, 'CONFIG', self.port / 'bbvr_config.json')]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(self.tmp.cleanup)

    def test_state_has_app_info_and_options(self):
        st = bbvr_cli.run(['state'])
        self.assertEqual(st['profile'], 'Default')
        self.assertFalse(st['app']['eboot'])
        self.assertFalse(st['app']['accepted'])
        self.assertEqual(set(st['app']['dlss_files']), {'nvngx_dlss.dll', 'bbport_dlss.dll'})
        self.assertIn('stereo_scale', {o['id'] for o in st['options']})
        self.assertEqual(st['crash']['count'], 0)
        self.assertEqual(st['crash']['dir'], str(self.port / 'crashes'))
        self.assertTrue((self.port / 'crashes').is_dir())  # created so "Open crash reports folder" works
        self.assertEqual(st['app']['data_dir'], str(self.port))
        self.assertFalse(st['app']['installed'])
        self.assertEqual(st['update']['status'], 'unchecked')

    def test_save_profile_and_values_share_the_web_menu_file(self):
        bbvr_cli.run(['save'], json.dumps({'profile': 'Comfort'}))
        self.assertEqual(S.load_settings(self.port / 'user' / 'bbvr_settings.json')['profile'], 'Comfort')
        st = bbvr_cli.run(['save'], json.dumps({'values': {'fps': 90}}))
        self.assertEqual(st['profile'], 'Custom')
        self.assertEqual(st['values']['fps'], 90)
        saved = S.load_settings(self.port / 'user' / 'bbvr_settings.json')
        self.assertEqual(saved['custom']['fps'], 90)

    def test_game_dir_needs_eboot_and_accept_is_remembered(self):
        game = self.port / 'dump'
        game.mkdir()
        st = bbvr_cli.run(['game-dir', str(game)])
        self.assertIn('error', st)
        self.assertFalse(st['app']['eboot'])
        (game / 'eboot.bin').write_bytes(b'x')
        st = bbvr_cli.run(['game-dir', str(game)])
        self.assertTrue(st['app']['eboot'])
        self.assertEqual(json.loads((self.port / 'bbvr_config.json').read_text())['game_dir'], str(game))
        self.assertTrue(bbvr_cli.run(['accept'])['app']['accepted'])

    def test_print_env_matches_the_launch_builder(self):
        out = bbvr_cli.run(['print-env', 'quality (dlss)'])
        self.assertEqual(out['profile'], 'Quality (DLSS)')
        paths = S.make_paths(self.port, self.port)
        plan = S.build_launch(S.effective_values({'profile': 'Quality (DLSS)'}), paths, S.detect_all(self.port))
        self.assertEqual(out['env'], plan['env'])
        self.assertIn('error', bbvr_cli.run(['print-env', 'nope']))

    def test_main_prints_one_json_line(self):
        with mock.patch.object(sys, 'argv', ['bbvr_cli.py', 'state']), mock.patch.object(sys, 'stdout') as out:
            out.buffer = io.BytesIO()
            self.assertEqual(bbvr_cli.main(), 0)
            self.assertEqual(json.loads(out.buffer.getvalue())['profile'], 'Default')

    def test_update_check_is_quiet_offline_and_can_be_switched_off(self):
        import bbvr_update
        with mock.patch.object(bbvr_update, 'fetch', side_effect=OSError('404')):
            st = bbvr_cli.run(['update-check'])
        self.assertEqual(st['update']['status'], 'unavailable')
        self.assertNotIn('error', st)
        newer = [{'tag_name': 'v9.0.0', 'html_url': 'https://github.com/x/y/releases/tag/v9.0.0', 'draft': False}]
        with mock.patch.object(bbvr_update, 'fetch', return_value=newer):
            st = bbvr_cli.run(['update-check'])
        self.assertEqual((st['update']['status'], st['update']['latest']), ('newer', '9.0.0'))
        st = bbvr_cli.run(['update-opt', 'off'])
        self.assertEqual(st['update']['status'], 'disabled')
        with mock.patch.object(bbvr_update, 'fetch', side_effect=AssertionError('must not be called')):
            bbvr_cli.run(['update-check'])
        self.assertEqual(bbvr_cli.run(['update-opt', 'on'])['update']['enabled'], True)

    def test_installed_flag_moves_the_data_dir(self):
        env = {'LOCALAPPDATA': str(self.port / 'lad')}
        self.assertEqual(S.data_root(self.port, env), self.port)
        (self.port / S.INSTALLED_FLAG).write_text('1')
        self.assertEqual(S.data_root(self.port, env), self.port / 'lad' / 'BloodborneVR')
        self.assertEqual(S.data_root(self.port, dict(env, BBVR_DATA_DIR=str(self.port / 'x'))), self.port / 'x')
        paths = S.make_paths(self.port, self.port, self.port / 'lad')
        self.assertEqual(paths['settings'], self.port / 'lad' / 'user' / 'bbvr_settings.json')
        self.assertEqual(paths['port'], self.port)

    def test_release_defaults_are_optional(self):
        self.assertEqual(S.release_defaults(self.port), {})
        (self.port / 'defaults').mkdir()
        (self.port / 'defaults' / 'release.json').write_text('{"update_repo": "a/b"}')
        self.assertEqual(S.release_defaults(self.port)['update_repo'], 'a/b')


if __name__ == '__main__':
    unittest.main()
