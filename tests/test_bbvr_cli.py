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

    def test_alpha_notice_is_once_per_version(self):
        (self.port / 'VERSION.txt').write_text('0.1.0-alpha.2', encoding='utf-8')
        st = bbvr_cli.run(['state'])
        self.assertTrue(st['app']['alpha_notice'])
        self.assertIn('EARLY ALPHA', st['app']['alpha_text'])
        self.assertFalse(bbvr_cli.run(['alpha-ack'])['app']['alpha_notice'])
        self.assertEqual(L.load_config()['alpha_notice_seen'], '0.1.0-alpha.2')
        (self.port / 'VERSION.txt').write_text('0.1.0-alpha.3', encoding='utf-8')  # a new version shows it again
        self.assertTrue(bbvr_cli.run(['state'])['app']['alpha_notice'])

    def test_save_profile_and_values_share_the_web_menu_file(self):
        bbvr_cli.run(['save'], json.dumps({'profile': 'Comfort'}))
        self.assertEqual(S.load_settings(self.port / 'user' / 'bbvr_settings.json')['profile'], 'Comfort')
        st = bbvr_cli.run(['save'], json.dumps({'values': {'fps': 90}}))
        self.assertEqual(st['profile'], 'Custom')
        self.assertEqual(st['values']['fps'], 90)
        saved = S.load_settings(self.port / 'user' / 'bbvr_settings.json')
        self.assertEqual(saved['custom']['fps'], 90)

    def test_play_tab_profile_cards_and_view_toggle(self):
        st = bbvr_cli.run(['state'])
        for name in ('Default', 'Comfort', 'Performance', 'Quality (DLSS)'):
            self.assertIn(name, st['profiles'])  # the Play tab shows one card per profile, with its note
            self.assertTrue(st['profile_notes'][name])
        self.assertEqual(st['values']['mode'], 'alt')  # the View toggle: 3D (alt) by default
        self.assertIn('rtx', st['caps']['gpus'][0] if st['caps']['gpus'] else {'rtx': False})
        # Performance: a plain profile pick
        st = bbvr_cli.run(['save'], json.dumps({'profile': 'Performance'}))
        self.assertEqual(st['profile'], 'Performance')
        self.assertEqual(st['values']['eye_res'], '1.5')
        # Flat screen (mono): the dashboard sends the full values with mode changed; nothing else moves
        vals = dict(st['values'])
        vals['mode'] = 'mono'
        st2 = bbvr_cli.run(['save'], json.dumps({'values': vals}))
        self.assertEqual(st2['values']['mode'], 'mono')
        self.assertEqual(st2['profile'], 'Custom')
        self.assertEqual(st2['values']['eye_res'], '1.5')
        self.assertEqual(st2['values']['upscale'], 'fsr1')
        self.assertEqual(S.build_launch(S.effective_values(S.load_settings(self.port / 'user' / 'bbvr_settings.json')),
                                        bbvr_cli.make_menu()[0].paths, {})['env']['BB_XR_MODE'], 'mono')
        # back to 3D
        vals['mode'] = 'alt'
        self.assertEqual(bbvr_cli.run(['save'], json.dumps({'values': vals}))['values']['mode'], 'alt')

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


class OnlineCliTests(CliTests):
    """online create|join|leave|status against a stand-in lobby (a tiny in-process REST server)."""

    def setUp(self):
        super().setUp()
        import http.server
        import threading
        calls = self.calls = []

        class Lobby(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers.get('Content-Length') or 0)) or b'{}')
                action = self.path.rsplit('/', 1)[-1]
                calls.append((action, body))
                if action == 'join' and body.get('code') != 'K7Q-M4X':
                    status, out = 404, {'ok': False, 'error': 'not_found', 'message': 'No such room'}
                elif action in ('create', 'join'):
                    status, out = 200, {'ok': True, 'code': 'K7Q-M4X', 'token': 'tok',
                                        'role': 'host' if action == 'create' else 'guest'}
                elif action == 'status':
                    status, out = 200, {'ok': True, 'in_room': body.get('token') == 'tok'}
                else:
                    status, out = 200, {'ok': True}
                data = json.dumps(out).encode()
                self.send_response(status)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.srv = http.server.HTTPServer(('127.0.0.1', 0), Lobby)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)
        self.url = 'http://127.0.0.1:%d' % self.srv.server_address[1]

    def test_no_server_is_a_clean_error(self):
        with mock.patch.dict('os.environ', {'BBVR_LOBBY_URL': ''}):
            self.assertEqual(bbvr_cli.run(['online', 'create'])['error'], 'no_server')

    def _write_default(self, url):
        (self.port / 'defaults').mkdir(exist_ok=True)
        (self.port / 'defaults' / 'release.json').write_text(json.dumps({'default_lobby': url}))

    def test_room_game_gets_a_password(self):
        # the game skips the shadNet login without BB_SHADNET_PASSWORD; an alpha7 identity (name only) is filled in
        self._write_default(self.url)
        user = self.port / 'user'
        user.mkdir(exist_ok=True)
        (user / 'online_identity.json').write_text(json.dumps({'npid': 'Old_2222'}))
        with mock.patch.dict('os.environ', {'BBVR_LOBBY_URL': ''}):
            self.assertTrue(bbvr_cli.run(['online', 'create'])['ok'])
            ident = json.loads((user / 'online_identity.json').read_text())
            self.assertEqual(ident['npid'], 'Old_2222')
            self.assertGreaterEqual(len(ident['password']), 12)
            env = S.online_env({}, {}, user, public=True, port=self.port)
        self.assertEqual(env['BB_SHADNET_NPID'], 'Old_2222')
        self.assertEqual(env['BB_SHADNET_PASSWORD'], ident['password'])

    def test_default_lobby_used_when_unset(self):
        self._write_default(self.url)
        with mock.patch.dict('os.environ', {'BBVR_LOBBY_URL': ''}):
            self.assertEqual(bbvr_cli.run(['online', 'server']), {'ok': True, 'server': self.url, 'is_default': True})
            self.assertEqual(bbvr_cli.run(['online', 'create'])['code'], 'K7Q-M4X')
            self.assertTrue(bbvr_cli.run(['online', 'view'])['is_default'])
            self.assertEqual(bbvr_cli.run(['state'])['app']['lobby'], {'server': self.url, 'is_default': True})
            self.assertEqual(S.lobby_address({}, L.load_config(), bbvr_cli._user_dir(), self.port), self.url.rstrip('/'))

    def test_player_setting_beats_default_and_default_returns(self):
        self._write_default('http://127.0.0.1:9')
        with mock.patch.dict('os.environ', {'BBVR_LOBBY_URL': ''}):
            out = bbvr_cli.run(['online', 'server', self.url])
            self.assertEqual((out['server'], out['is_default']), (self.url, False))
            self.assertEqual(S.lobby_address({}, L.load_config(), bbvr_cli._user_dir(), self.port), self.url)
            self.assertEqual(S.lobby_address({'online_server': 'http://127.0.0.1:7/'}, {}, bbvr_cli._user_dir(), self.port), 'http://127.0.0.1:7')
            back = bbvr_cli.run(['online', 'server', 'default'])
            self.assertEqual((back['server'], back['is_default']), ('http://127.0.0.1:9', True))

    def test_env_beats_setting_and_default(self):
        self._write_default('http://127.0.0.1:9')
        bbvr_cli.run(['online', 'server', 'http://127.0.0.1:7'])
        with mock.patch.dict('os.environ', {'BBVR_LOBBY_URL': self.url}):
            self.assertEqual(bbvr_cli.run(['online', 'server'])['server'], self.url)
            self.assertEqual(S.lobby_address({}, L.load_config(), bbvr_cli._user_dir(), self.port), self.url)

    def test_create_status_leave(self):
        bbvr_cli.run(['online', 'server', self.url])
        out = bbvr_cli.run(['online', 'create', 'Tester'])
        self.assertEqual((out['ok'], out['code'], out['role']), (True, 'K7Q-M4X', 'host'))
        npid = self.calls[-1][1]['npid']
        self.assertRegex(npid, r'^Tester_[2-9A-Z]{4}$')
        self.assertTrue(bbvr_cli.run(['online', 'status'])['in_room'])
        self.assertEqual(bbvr_cli.run(['online', 'leave']), {'ok': True, 'left': True})
        self.assertEqual(bbvr_cli.run(['online', 'status']), {'ok': True, 'in_room': False})
        self.assertEqual(bbvr_cli.run(['online', 'create'])['ok'], True)
        self.assertEqual(self.calls[-1][1]['npid'], npid)          # identity is stable

    def test_join_errors_and_unreachable(self):
        bbvr_cli.run(['online', 'server', self.url])
        self.assertEqual(bbvr_cli.run(['online', 'join', 'ZZZ-ZZZ'])['error'], 'not_found')
        self.assertEqual(bbvr_cli.run(['online', 'join', 'K7Q-M4X'])['role'], 'guest')
        bbvr_cli.run(['online', 'server', 'http://127.0.0.1:9'])
        self.assertEqual(bbvr_cli.run(['online', 'join', 'K7Q-M4X'])['error'], 'unreachable')

    def test_start_flow_host_join_solo(self):
        """The dashboard's Play online / Play solo: the room file and the launcher environment each leads to."""
        bbvr_cli.run(['online', 'server', self.url])
        user = bbvr_cli._user_dir()
        values = {'online_autobell': True}
        # Play solo: no room, no online vars; the choice is remembered
        self.assertEqual(bbvr_cli.run(['online', 'mode', 'solo'])['play_mode'], 'solo')
        self.assertEqual(S.online_env(values, L.load_config(), user, public=True), {})
        # Host a game: create, remember "host", the room file has the code, the env makes the game the host
        out = bbvr_cli.run(['online', 'create'])
        self.assertEqual(bbvr_cli.run(['online', 'mode', 'host']), {'ok': True, 'play_mode': 'online', 'online_step': 'host'})
        room = json.loads((user / 'online_room.json').read_text(encoding='utf-8'))
        self.assertEqual((room['code'], room['role']), (out['code'], 'host'))
        for public in (False, True):
            env = S.online_env(values, L.load_config(), user, public=public)
            self.assertEqual((env['BB_ONLINE_ROOM'], env['BB_ONLINE_ROLE']), ('K7Q-M4X', 'host'))
            self.assertTrue(env['BB_ONLINE_ROOM_FILE'].endswith('online_room.json'))
            # Play solo again with the room still saved: online stays off
            self.assertEqual(S.online_env(values, L.load_config(), user, public=public, solo=True), {})
        st = bbvr_cli.run(['state'])['app']
        self.assertEqual((st['play_mode'], st['online_step'], st['online_play']), ('online', 'host', True))
        # Join a friend: leave, join by code, guest env
        bbvr_cli.run(['online', 'leave'])
        self.assertFalse((user / 'online_room.json').exists())
        self.assertEqual(bbvr_cli.run(['online', 'join', 'K7Q-M4X'])['role'], 'guest')
        bbvr_cli.run(['online', 'mode', 'join'])
        env = S.online_env(values, L.load_config(), user, public=True)
        self.assertEqual((env['BB_ONLINE_ROOM'], env['BB_ONLINE_ROLE']), ('K7Q-M4X', 'guest'))
        self.assertEqual(bbvr_cli.run(['state'])['app']['online_step'], 'join')
        # the view the dashboard polls carries the code
        view = bbvr_cli.run(['online', 'view'])
        self.assertTrue(view['in_room'] and view['code'] == 'K7Q-M4X')


if __name__ == '__main__':
    unittest.main()
