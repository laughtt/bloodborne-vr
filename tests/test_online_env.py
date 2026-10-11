"""bbvr_settings.online_env: the environment a game started from a room gets (user/online_room.json + online_identity.json)."""
from paths import ROOT
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'tools' / 'release' / 'zip'))
import bbvr_settings as S  # noqa: E402


class OnlineEnvTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.user = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, data):
        (self.user / name).write_text(json.dumps(data), encoding='utf-8')

    def test_no_room_no_online(self):
        self.assertEqual(S.online_env({}, {}, self.user, public=False), {})

    def test_public_build_dev_option_never_switches_online_on(self):
        self.assertEqual(S.online_env({'online': True}, {}, self.user, public=True), {})

    def test_public_build_room_is_online(self):
        # the start flow (Play online) works in public builds: only a saved room switches it on
        self.write('online_room.json', {'code': 'K7QM4X', 'role': 'host'})
        env = S.online_env({'online': True}, {}, self.user, public=True)
        self.assertEqual(env['BB_ONLINE_ROOM'], 'K7QM4X')
        self.assertEqual(env['BB_ONLINE_ROLE'], 'host')
        with mock.patch.object(S, 'ONLINE_PLAY', False):
            self.assertEqual(S.online_env({}, {}, self.user, public=True), {})

    def test_play_solo_has_no_online_vars(self):
        self.write('online_room.json', {'code': 'K7QM4X', 'role': 'host'})
        self.write('online_identity.json', {'npid': 'Hunter_AB23'})
        for public in (False, True):
            self.assertEqual(S.online_env({'online': True}, {}, self.user, public=public, solo=True), {})

    def test_host_in_room(self):
        self.write('online_room.json', {'code': 'K7QM4X', 'role': 'host', 'server': 'http://lobby.example:2000'})
        self.write('online_identity.json', {'npid': 'Hunter_AB23'})
        env = S.online_env({'online_autobell': True}, {}, self.user, public=False)
        self.assertEqual(env['BB_ONLINE'], '1')
        self.assertEqual(env['BB_SHADNET_NPID'], 'Hunter_AB23')
        self.assertEqual(env['BB_USER_NAME'], 'Hunter_AB23')
        self.assertEqual(env['BB_ONLINE_ROLE'], 'host')
        self.assertEqual(env['BB_ONLINE_ROOM'], 'K7QM4X')
        self.assertEqual(env['BB_ONLINE_SERVER'], 'lobby.example:1998')
        self.assertEqual(env['BB_ONLINE_AUTOBELL'], '1')
        self.assertEqual(env['BB_ONLINE_BELL_GIVE'], '1')  # on by default in a room
        self.assertTrue(env['BB_ONLINE_ROOM_FILE'].endswith('online_room.json'))

    def test_guest_options(self):
        self.write('online_room.json', {'code': 'AAAAAA', 'role': 'guest'})
        env = S.online_env({'online_autobell': False, 'online_give_bells': True, 'online_game_server': 'h:1',
                            'online_server': 'http://x:9'}, {}, self.user, public=False)
        self.assertEqual(env['BB_ONLINE_ROLE'], 'guest')
        self.assertEqual(env['BB_ONLINE_AUTOBELL'], '0')
        self.assertEqual(env['BB_ONLINE_BELL_GIVE'], '1')
        self.assertEqual(env['BB_ONLINE_SERVER'], 'h:1')

    def test_online_option_without_room(self):
        env = S.online_env({'online': True}, {}, self.user, public=False)
        self.assertEqual(env, {'BB_ONLINE': '1'})


if __name__ == '__main__':
    unittest.main()
