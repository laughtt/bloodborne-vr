"""Room heartbeat (tools/release/zip/bbvr_online.py) and the CLI's `online view` against a stand-in lobby."""
from paths import ROOT
import http.server
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock

ZIP = ROOT / 'tools' / 'release' / 'zip'
sys.path.insert(0, str(ZIP))
import bbvr_cli  # noqa: E402
import bbvr_launcher as L  # noqa: E402
import bbvr_online as O  # noqa: E402
import bbvr_settings as S  # noqa: E402


class StandInLobby(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.port = Path(self.tmp.name)
        self.user = self.port / 'user'
        self.user.mkdir()
        self.calls = []
        self.answer = {'ok': True, 'in_room': True, 'code': 'K7Q-M4X', 'role': 'guest', 'max': 3, 'closed': False,
                       'members': [{'name': 'Boss', 'role': 'host', 'npid': 'Boss_1111', 'idle': 1, 'sign': False, 'summoned': False},
                                   {'name': 'Me', 'role': 'guest', 'npid': 'Me_2222', 'idle': 0, 'sign': True, 'summoned': False}]}
        test = self

        class Lobby(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers.get('Content-Length') or 0)) or b'{}')
                action = self.path.rsplit('/', 1)[-1]
                test.calls.append((action, body))
                if action == 'status' and body.get('token') != 'tok':
                    out = {'ok': True, 'in_room': False}
                elif action in ('status', 'create', 'join'):
                    out = test.answer
                else:
                    out = {'ok': True}
                data = json.dumps(out).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.srv = http.server.HTTPServer(('127.0.0.1', 0), Lobby)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)
        self.url = 'http://127.0.0.1:%d' % self.srv.server_address[1]
        (self.user / 'online_identity.json').write_text(json.dumps({'npid': 'Me_2222'}))
        self.room_file = self.user / 'online_room.json'
        self.room_file.write_text(json.dumps({'code': 'K7Q-M4X', 'role': 'guest', 'token': 'tok', 'server': self.url}))


class OnlineHeartbeatTests(StandInLobby):
    def test_refresh_writes_members_sign_and_summoned(self):
        data = O.refresh(self.user, now=lambda: 1700000000)
        self.assertEqual(self.calls[-1], ('status', {'npid': 'Me_2222', 'token': 'tok'}))
        saved = json.loads(self.room_file.read_text())
        self.assertEqual(saved, data)
        self.assertEqual([(m['role'], m['sign'], m['summoned']) for m in saved['members']],
                         [('host', False, False), ('guest', True, False)])
        self.assertEqual((saved['self_sign'], saved['self_summoned'], saved['summoned_guests'], saved['closed']),
                         (True, False, 0, False))
        self.assertEqual((saved['token'], saved['server'], saved['ts']), ('tok', self.url, 1700000000))
        self.answer['members'][1].update(summoned=True)
        self.answer['members'].append({'name': 'Third', 'role': 'guest', 'npid': 'T_3', 'idle': 3, 'sign': False, 'summoned': True})
        saved = O.refresh(self.user)
        self.assertEqual((saved['self_summoned'], saved['summoned_guests']), (True, 2))

    def test_member_world_is_copied_when_the_server_reports_it(self):
        self.answer['members'][0]['world'] = 'host'
        self.answer['members'][1]['world'] = 'own'
        self.answer['members'].append({'name': 'X', 'role': 'guest', 'npid': 'X_4', 'idle': 0, 'sign': False, 'summoned': True, 'world': 'bogus'})
        self.answer['members'].append({'name': 'Y', 'role': 'guest', 'npid': 'Y_5', 'idle': 0, 'sign': False, 'summoned': True})
        saved = O.refresh(self.user)
        self.assertEqual([m.get('world') for m in saved['members']], ['host', 'own', None, None])
        self.assertNotIn('world', saved['members'][2])
        self.assertNotIn('world', saved['members'][3])

    def test_room_gone_marks_closed_and_drops_the_token(self):
        self.room_file.write_text(json.dumps({'code': 'K7Q-M4X', 'role': 'guest', 'token': 'old', 'server': self.url}))
        data = O.refresh(self.user)
        self.assertTrue(data['closed'])
        saved = json.loads(self.room_file.read_text())
        self.assertNotIn('token', saved)
        self.assertEqual(saved['members'], [])
        self.assertIsNone(O.refresh(self.user))                     # no token any more: nothing to ask
        self.assertEqual(S.online_env({}, {}, self.user, public=False), {})   # a closed room starts no auto-bell

    def test_unreachable_keeps_the_old_file(self):
        before = O.refresh(self.user, now=lambda: 5)
        self.room_file.write_text(json.dumps(dict(before, server='http://127.0.0.1:9')))
        text = self.room_file.read_text()
        self.assertIsNone(O.refresh(self.user))
        self.assertEqual(self.room_file.read_text(), text)

    def test_watch_beats_until_the_room_closes_and_honours_stop(self):
        lines = []
        stop = threading.Event()
        beats = []

        def stop_after_three(*a, **k):
            beats.append(1)
            if len(beats) == 3:
                self.answer = {'ok': True, 'in_room': False, 'closed': True, 'reason': 'host_left'}
            return real(*a, **k)

        real = O.refresh
        with mock.patch.object(O, 'refresh', side_effect=stop_after_three):
            last = O.watch(self.user, interval=0.01, stop=stop, log=lines.append)
        self.assertEqual(len(beats), 3)
        self.assertTrue(last['closed'])
        self.assertEqual(last['reason'], 'host_left')
        self.assertTrue(any('closed' in t for t in lines))
        # stop event
        self.room_file.write_text(json.dumps({'code': 'K7Q-M4X', 'role': 'guest', 'token': 'tok', 'server': self.url}))
        self.answer = {'ok': True, 'in_room': True, 'code': 'K7Q-M4X', 'role': 'guest', 'members': []}
        thread, ev = O.start_thread(self.user, interval=0.01)
        self.assertIsNotNone(thread)
        n = len(self.calls)
        ev.set()
        thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertGreaterEqual(len(self.calls), n)

    def test_no_room_no_thread(self):
        self.room_file.unlink()
        self.assertEqual(O.start_thread(self.user), (None, None))

    def test_pid_gone_stops_the_watch(self):
        with mock.patch.object(O, '_pid_alive', return_value=False):
            self.assertIsNone(O.watch(self.user, interval=0.01, pid=123456))
        self.assertEqual(self.calls, [])

    def test_env_lists_the_room_for_a_hand_started_game(self):
        O.refresh(self.user)
        with mock.patch('builtins.print') as pr:
            self.assertEqual(O.main(['env', '--user-dir', str(self.user)]), 0)
        text = '\n'.join(c.args[0] for c in pr.call_args_list)
        self.assertIn('BB_ONLINE_ROLE=guest', text)
        self.assertIn('BB_SHADNET_NPID=Me_2222', text)
        self.assertIn('BB_ONLINE_ROOM_FILE=', text)


class OnlineWorldFieldTests(StandInLobby):
    def status(self, **kw):
        (self.user / 'online_bell_status.json').write_text(json.dumps(kw))

    def world_sent(self):
        env = {k: v for k, v in __import__('os').environ.items() if k != 'BB_ONLINE_STATUS_FILE'}
        with mock.patch.dict('os.environ', env, clear=True):
            O.refresh(self.user, now=lambda: 1000)
        return self.calls[-1][1].get('world')

    def test_member_of_another_room_is_host_world(self):
        self.status(np_room=2, ts=995)
        self.assertEqual(self.world_sent(), 'host')

    def test_no_room_or_own_room_is_own_world(self):
        for role in (0, 1):
            self.status(np_room=role, ts=1000)
            self.assertEqual(self.world_sent(), 'own')

    def test_stale_missing_or_broken_status_sends_no_field(self):
        self.status(np_room=2, ts=900)                      # older than 20 s
        self.assertIsNone(self.world_sent())
        self.status(state='idle', ts=1000)                  # no np_room
        self.assertIsNone(self.world_sent())
        self.status(np_room=-1, ts=1000)                    # built without online
        self.assertIsNone(self.world_sent())
        (self.user / 'online_bell_status.json').write_text('{broken')
        self.assertIsNone(self.world_sent())
        (self.user / 'online_bell_status.json').unlink()    # missing file
        self.assertIsNone(self.world_sent())

    def test_env_status_file_path_and_lobby_call_gets_the_field(self):
        other = self.port / 'elsewhere.json'
        other.write_text(json.dumps({'np_room': 2, 'ts': 1000}))
        with mock.patch.dict('os.environ', {'BB_ONLINE_STATUS_FILE': str(other)}),                 mock.patch.object(O, 'lobby_call', return_value={'ok': False}) as lc:
            O.refresh(self.user, now=lambda: 1000)
        self.assertEqual(lc.call_args.args[2], {'npid': 'Me_2222', 'token': 'tok', 'world': 'host'})


class OnlineViewTests(StandInLobby):
    """bbvr_cli online view / create / status keep the same file."""

    def setUp(self):
        super().setUp()
        for p in [mock.patch.object(L, 'PORT', self.port), mock.patch.object(L, 'DATA', self.port),
                  mock.patch.object(L, 'OUT', self.port / 'out'), mock.patch.object(L, 'CONFIG', self.port / 'cfg.json'),
                  mock.patch.dict('os.environ', {'BBVR_LOBBY_URL': self.url})]:
            p.start()
            self.addCleanup(p.stop)

    def test_view_refreshes_a_stale_file_and_labels_the_members(self):
        out = bbvr_cli.run(['online', 'view'])
        self.assertTrue(out['in_room'])
        self.assertEqual((out['code'], out['role'], out['npid']), ('K7Q-M4X', 'guest', 'Me_2222'))
        self.assertEqual([m['state'] for m in out['members']], ['host', 'sign up'])
        self.assertNotIn('token', json.dumps(out))
        self.answer['members'][1].update(sign=False, summoned=True)
        self.room_file.write_text(json.dumps(dict(json.loads(self.room_file.read_text()), ts=0)))
        out = bbvr_cli.run(['online', 'view'])
        self.assertEqual(out['members'][1]['state'], 'in world')
        n = len(self.calls)
        bbvr_cli.run(['online', 'view'])                                 # fresh file: no new call
        self.assertEqual(len(self.calls), n)

    def test_view_without_a_room_and_with_a_closed_room(self):
        self.room_file.unlink()
        out = bbvr_cli.run(['online', 'view'])
        self.assertEqual((out['ok'], out['in_room'], out['members']), (True, False, []))
        self.room_file.write_text(json.dumps({'code': 'K7Q-M4X', 'role': 'guest', 'token': 'old', 'server': self.url}))
        out = bbvr_cli.run(['online', 'view'])
        self.assertEqual((out['in_room'], out['closed']), (False, True))

    def test_view_includes_a_fresh_bell_status_only(self):
        import time
        (self.user / 'online_bell_status.json').write_text(json.dumps({'state': 'sign_up', 'ts': time.time()}))
        self.assertEqual(bbvr_cli.run(['online', 'view'])['bell']['state'], 'sign_up')
        (self.user / 'online_bell_status.json').write_text(json.dumps({'state': 'sign_up', 'ts': 1}))
        self.assertEqual(bbvr_cli.run(['online', 'view'])['bell'], {})

    def test_create_writes_the_members_into_the_file(self):
        self.answer = {'ok': True, 'code': 'ABC-DEF', 'role': 'host', 'token': 'tok', 'max': 3,
                       'members': [{'name': 'Me', 'role': 'host', 'npid': 'Me_2222', 'idle': 0}]}
        self.room_file.unlink()
        out = bbvr_cli.run(['online', 'create'])
        self.assertTrue(out['ok'])
        saved = json.loads(self.room_file.read_text())
        self.assertEqual((saved['code'], saved['role'], saved['token'], saved['summoned_guests']), ('ABC-DEF', 'host', 'tok', 0))
        self.assertEqual(len(saved['members']), 1)


class HostRedirectTests(unittest.TestCase):
    """The lobby serves the game-server redirect (GET /bbvr/v1/hosts); the client validates it and saves it."""

    GOOD = {'https://game.example:20443': 'http://127.0.0.1:31415', 'api.example': 'http://10.0.0.5:31315', '*': 'https://x.example'}

    def serve(self, status, body):
        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                data = body if isinstance(body, bytes) else json.dumps(body).encode()
                self.send_response(status)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
        srv = http.server.HTTPServer(('127.0.0.1', 0), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        return 'http://127.0.0.1:%d' % srv.server_address[1]

    def user(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        return Path(tmp.name)

    def test_validate_accepts_mappings(self):
        self.assertEqual(O.validate_hosts(self.GOOD), self.GOOD)
        self.assertEqual(O.validate_hosts({'ok': True, 'hosts': self.GOOD}), self.GOOD)

    def test_validate_rejects_anything_else(self):
        for bad in (None, [], {}, {'hosts': {}}, {'a.example': 5}, {'a.example': 'ftp://b.example'},
                    {'a.example': 'b.example:1'}, {'a.example': 'http://u:p' + chr(64) + 'b.example'},
                    {'a.example': 'http://b.example/x?y=1'}, {'a.example': 'http://b.example:99999'},
                    {'a b': 'http://b.example'}, {'a.example': 'http://b.example/with space'},
                    {'a.example': 'http://b.example/p'}, {'a.example': 'http://b.example' + chr(92) + 'x'},
                    {'k%d.example' % i: 'http://b.example' for i in range(40)}):
            self.assertIsNone(O.validate_hosts(bad), bad)

    def test_sync_saves_the_file_and_returns_it(self):
        user, logs = self.user(), []
        path = O.sync_host_overrides(self.serve(200, {'ok': True, 'hosts': self.GOOD}), user, logs.append)
        self.assertEqual(path, user / 'host_overrides.lobby.json')
        self.assertEqual(json.loads(path.read_text()), self.GOOD)
        self.assertTrue(any('redirect from the lobby' in x for x in logs))

    def test_lobby_without_the_endpoint_keeps_the_old_behaviour(self):
        user, logs = self.user(), []
        (user / 'host_overrides.lobby.json').write_text('{"a.example": "http://b.example"}')   # stale
        self.assertIsNone(O.sync_host_overrides(self.serve(404, {'ok': False}), user, logs.append))
        self.assertFalse((user / 'host_overrides.lobby.json').exists())
        self.assertTrue(any('no game-server redirect' in x for x in logs))

    def test_invalid_oversized_and_unreachable_answers_are_ignored(self):
        user = self.user()
        for base in (self.serve(200, b'not json'), self.serve(200, {'hosts': {'a.example': 'ftp://x'}}),
                     self.serve(200, b'{"hosts": {"a": "http://b"}}' + b' ' * 9000), 'http://127.0.0.1:9'):
            self.assertIsNone(O.sync_host_overrides(base, user), base)
            self.assertFalse((user / 'host_overrides.lobby.json').exists())

    def test_players_custom_file_wins_and_is_not_touched(self):
        user = self.user()
        custom = user / 'host_overrides.custom.json'
        custom.write_text('{"mine.example": "http://127.0.0.1:1"}')
        self.assertEqual(O.sync_host_overrides(self.serve(200, self.GOOD), user), custom)
        self.assertEqual(json.loads(custom.read_text()), {'mine.example': 'http://127.0.0.1:1'})
        self.assertFalse((user / 'host_overrides.lobby.json').exists())

    def test_lobby_address_order(self):
        user = self.user()
        (user / 'online_room.json').write_text('{"server": "http://room.example:1/"}')
        self.assertEqual(S.lobby_address({'online_server': 'http://set.example:2/'}, {'online_server': 'c'}, user), 'http://set.example:2')
        self.assertEqual(S.lobby_address({}, {'online_server': 'c'}, user), 'http://room.example:1')
        self.assertEqual(S.lobby_address({}, {'online_server': 'http://cfg.example:3'}, self.user()), 'http://cfg.example:3')


if __name__ == '__main__':
    unittest.main()
