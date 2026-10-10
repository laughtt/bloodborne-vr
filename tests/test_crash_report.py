"""Crash report scrubber, bundle and upload (against the server on localhost)."""
from paths import ROOT
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
import zipfile

sys.path.insert(0, str(ROOT / 'scripts'))
import crash_report  # noqa: E402

spec = importlib.util.spec_from_file_location('crash_server', ROOT / 'tools/crash_server/server.py')
crash_server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(crash_server)

CONTEXT = {'users': ['Alice'], 'hosts': ['ALICE-PC']}
UPLOAD, ADMIN = 'u' * 20, 'a' * 20


class ScrubTests(unittest.TestCase):
    def test_paths_names_ids_ips(self):
        text = (r'open C:\Users\Alice\Documents\x.txt and /c/Users/Alice/foo; user Alice on ALICE-PC' '\n'
                'onlineId=CoolHunter99 accountId: 0123456789abcdef npid="abc" Authorization: Bearer s3cret\n'
                'peer 192.168.1.20:3658 and 2001:db8::1 at 12:34:56, rip 0x00007ff612345678')
        out = crash_report.scrub_text(text, CONTEXT)
        for secret in ('Alice', 'ALICE-PC', 'CoolHunter99', '0123456789abcdef', '192.168.1.20', '2001:db8::1'):
            self.assertNotIn(secret, out)
        self.assertIn('%USER%', out)
        self.assertIn('12:34:56', out)               # times stay
        self.assertIn('0x00007ff612345678', out)     # addresses stay

    def test_bytes_same_length(self):
        data = b'xx C:\\Users\\Alice\\a ' + 'Alice'.encode('utf-16-le') + b'yy'
        out = crash_report.scrub_bytes(data, CONTEXT)
        self.assertEqual(len(out), len(data))
        self.assertNotIn(b'Alice', out)
        self.assertNotIn('Alice'.encode('utf-16-le'), out)

    def test_bundle_has_only_whitelisted_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            crash = Path(tmp) / '20260101-000000-fault'
            crash.mkdir()
            (crash / 'crash.json').write_text(json.dumps(
                {'build': 'abc123', 'fault_module': 'guest', 'fault_offset': '0x10', 'note': r'C:\Users\Alice\x'}))
            (crash / 'crash.dmp').write_bytes(b'MDMP' + b'Alice' * 3)
            (crash / 'extra.bin').write_bytes(b'game file')
            data, summary = crash_report.build_bundle([crash], ['ok', 'Fault: bad thing'] * 3, 3, context=CONTEXT)
        archive = zipfile.ZipFile(io.BytesIO(data))
        self.assertEqual(sorted(archive.namelist()), ['crash0/crash.dmp', 'crash0/crash.json', 'log_tail.txt', 'meta.json'])
        self.assertEqual(summary['fault_module'], 'guest')
        self.assertIn('Fault', summary['first_error'])
        self.assertNotIn(b'Alice', archive.read('crash0/crash.json'))
        self.assertNotIn(b'Alice', archive.read('crash0/crash.dmp'))


class UploadTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.server = crash_server.make_server('127.0.0.1', 0, self.tmp.name, UPLOAD, ADMIN, retention_days=30,
                                               max_bytes=1 << 20)
        self.url = f'http://127.0.0.1:{self.server.server_address[1]}'
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    def bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            crash = Path(tmp) / 'c'
            crash.mkdir()
            (crash / 'crash.json').write_text(json.dumps(
                {'build': 'b1', 'fault_module': 'bb-probe.exe', 'fault_offset': '0x1234'}))
            return crash_report.build_bundle([crash], ['Fault: boom'], 3, context=CONTEXT)

    def test_upload_and_list(self):
        data, summary = self.bundle()
        self.assertTrue(crash_report.upload(self.url, UPLOAD, data, summary, budget=5))
        entries = self.server.store.entries()
        self.assertEqual((entries[0]['build'], entries[0]['fault_module'], entries[0]['fault_offset']),
                         ('b1', 'bb-probe.exe', '0x1234'))
        self.assertIn('Fault: boom', entries[0]['first_error'])
        self.assertTrue(list(Path(self.tmp.name).glob('b1/*/*.zip')))
        request = urllib.request.Request(self.url + '/', headers={'Authorization': 'Bearer ' + ADMIN})
        page = urllib.request.urlopen(request, timeout=5).read().decode()
        self.assertIn('bb-probe.exe+0x1234', page)

    def test_bad_token_and_garbage_rejected(self):
        data, summary = self.bundle()
        self.assertFalse(crash_report.upload(self.url, 'wrong', data, summary, budget=3))
        self.assertFalse(crash_report.upload(self.url, UPLOAD, b'not a zip', summary, budget=3))
        self.assertEqual(self.server.store.entries(), [])

    def test_list_needs_admin_token(self):
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(self.url + '/', timeout=5)
        self.assertEqual(caught.exception.code, 401)

    def test_plain_http_only_for_localhost(self):
        self.assertFalse(crash_report.url_allowed('http://example.com'))
        self.assertTrue(crash_report.url_allowed('https://example.com'))

    def test_handle_exit_consent_flow(self):
        with tempfile.TemporaryDirectory() as user:
            env = {'BB_CRASH_SERVER_URL': self.url, 'BB_CRASH_TOKEN': UPLOAD}
            # Not a console and no answer: kept locally, not sent.
            saved = crash_report.handle_exit(user, set(), ['Fault: x'], 1, env=env, interactive=False)
            self.assertTrue(saved.is_file())
            self.assertEqual(self.server.store.entries(), [])
            # Answer yes: saved as "on", sent.
            crash_report.handle_exit(user, set(), ['Fault: x'], 1, env=env, input_fn=lambda _: 'y', interactive=True)
            self.assertEqual(crash_report.load_config(user, {})['crash_reports'], 'on')
            self.assertEqual(len(self.server.store.entries()), 1)
            # Now automatic, no prompt.
            crash_report.handle_exit(user, set(), ['Fault: y'], 2, env=env, input_fn=lambda _: 1 / 0, interactive=True)
            self.assertEqual(len(self.server.store.entries()), 2)
            # Off: nothing.
            crash_report.save_mode(user, 'off')
            self.assertIsNone(crash_report.handle_exit(user, set(), ['z'], 1, env=env))
            self.assertEqual(len(self.server.store.entries()), 2)
            # Clean exit: nothing.
            crash_report.save_mode(user, 'on')
            self.assertIsNone(crash_report.handle_exit(user, crash_report.crash_dirs_in(user), ['z'], 0, env=env))


if __name__ == '__main__':
    unittest.main()
