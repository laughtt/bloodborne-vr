"""Crash report scrubber, bundle and the local-only save (nothing is sent)."""
from paths import ROOT
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(ROOT / 'scripts'))
import crash_report  # noqa: E402

CONTEXT = {'users': ['Alice'], 'hosts': ['ALICE-PC']}


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


class LocalReportTests(unittest.TestCase):
    def test_crash_is_saved_locally_scrubbed_and_never_sent(self):
        import socket
        import unittest.mock as mock
        with tempfile.TemporaryDirectory() as tmp:
            user, store = Path(tmp) / 'user', Path(tmp) / 'store'
            raw = user / 'crashes' / '20260101-000000-fault'
            raw.mkdir(parents=True)
            (raw / 'crash.json').write_text(json.dumps({'build': 'b1', 'note': r'C:\Users\Alice\x'}))
            (raw / 'crash.dmp').write_bytes(b'MDMP' + b'Alice' * 3)
            env = {'BB_CRASH_DIR': str(store)}
            with mock.patch.object(socket.socket, 'connect', side_effect=AssertionError('network used')), \
                    mock.patch.object(crash_report, 'scrub_context', return_value=CONTEXT):
                saved = crash_report.handle_exit(user, set(), ['Fault: x'], 1, env=env)
            self.assertEqual(saved.parent.parent, store)
            self.assertEqual(crash_report.list_reports(env), [saved])
            self.assertNotIn(b'Alice', zipfile.ZipFile(saved).read('crash0/crash.dmp'))
            self.assertFalse(raw.exists())  # the raw dump does not stay behind

    def test_exit_without_crash_saves_nothing_and_ctrl_c_too(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = {'BB_CRASH_DIR': str(Path(tmp) / 's')}
            self.assertIsNone(crash_report.handle_exit(tmp, set(), ['z'], 0, env=env))
            self.assertIsNone(crash_report.handle_exit(tmp, set(), ['z'], 130, env=env))
            self.assertEqual(crash_report.list_reports(env), [])

    def test_store_dir_is_per_user_appdata(self):
        self.assertEqual(crash_report.store_dir({'LOCALAPPDATA': r'C:\u\AppData\Local'}),
                         Path(r'C:\u\AppData\Local') / 'BloodborneVR' / 'crashes')

    def test_no_network_or_consent_code_left(self):
        source = (ROOT / 'scripts' / 'crash_report.py').read_text(encoding='utf-8')
        for word in ('urllib', 'upload', 'consent', 'crash_server_url', 'http'):
            self.assertNotIn(word, source.replace('import socket', ''))


if __name__ == '__main__':
    unittest.main()
