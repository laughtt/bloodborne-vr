"""BloodborneVR.exe (tools/release/zip/exe): argument forwarding, exit code, and the crash-report consent flow.

The stub is built with the MSYS2 CLANG64 toolchain (skipped when clang / llvm-rc or Windows is missing) and
run with the running Python as the stand-in for python\\python.exe and a dummy launcher script."""
from paths import ROOT
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import urllib.request

sys.path.insert(0, str(ROOT / 'tools' / 'release'))
sys.path.insert(0, str(ROOT / 'tools' / 'release' / 'zip'))
import build_stub  # noqa: E402
import bbvr_menu  # noqa: E402
import crash_report  # noqa: E402

DUMMY = '''import json, os, sys
with open(os.environ["BBVR_TEST_OUT"], "w", encoding="utf-8") as f:
    json.dump({"argv": sys.argv[1:], "cwd": os.getcwd(), "exe_env": os.environ.get("BBVR_EXE")}, f)
sys.exit(int(os.environ.get("BBVR_TEST_EXIT", "0")))
'''
TRICKY = ['plain', 'two words', 'quote"inside', 'back\\slash', 'trail slash\\', 'double\\\\', '', 'ünï côdé', '--flag=1 2',
          'C:\\Program Files\\x y\\']


@unittest.skipUnless(os.name == 'nt' and build_stub.available(), 'needs Windows and the MSYS2 CLANG64 clang / llvm-rc')
class StubTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe_dir = Path(cls.tmp.name) / 'Blood borne VR ünï'
        cls.exe_dir.mkdir()
        cls.exe = build_stub.build(cls.exe_dir / 'BloodborneVR.exe', '0.1.0-alpha3')
        cls.script = Path(cls.tmp.name) / 'dummy launcher.py'
        cls.script.write_text(DUMMY, encoding='utf-8')
        cls.out = Path(cls.tmp.name) / 'out.json'

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_stub(self, args, exit_code=0):
        env = dict(os.environ, BBVR_LAUNCHER_PYTHON=sys.executable, BBVR_LAUNCHER_SCRIPT=str(self.script),
                   BBVR_TEST_OUT=str(self.out), BBVR_TEST_EXIT=str(exit_code))
        self.out.unlink(missing_ok=True)
        result = subprocess.run([str(self.exe), *args], env=env, timeout=60, cwd=self.tmp.name)
        return result.returncode, json.loads(self.out.read_text(encoding='utf-8')) if self.out.is_file() else None

    def test_small_gui_exe(self):
        data = self.exe.read_bytes()
        self.assertLess(len(data), 200 * 1024)
        import struct
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        self.assertEqual(struct.unpack_from('<H', data, pe + 24 + 68)[0], 2)  # IMAGE_SUBSYSTEM_WINDOWS_GUI
        self.assertIn(b'asInvoker', data)

    def test_arguments_survive(self):
        code, seen = self.run_stub(TRICKY)
        self.assertEqual(code, 0)
        self.assertEqual(seen['argv'], TRICKY)
        self.assertEqual(seen['exe_env'], '1')
        self.assertEqual(Path(seen['cwd']).resolve(), self.exe_dir.resolve())  # runs from the exe's own folder

    def test_no_arguments(self):
        code, seen = self.run_stub([])
        self.assertEqual((code, seen['argv']), (0, []))

    def test_exit_code_is_returned(self):
        code, seen = self.run_stub(['--check'], exit_code=7)
        self.assertEqual(code, 7)
        self.assertEqual(seen['argv'], ['--check'])


class ConsentTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self._env = os.environ.pop('BB_CRASH_REPORTS', None)
        from bbvr_settings import make_paths
        paths = make_paths(Path(self.tmp.name), Path(self.tmp.name) / 'game')
        self.user = Path(self.tmp.name) / 'user'
        self.menu = bbvr_menu.Menu(self.user / 's.json', paths,
                                   {'gpus': [], 'dlss': {'ok': False, 'reason': 'x', 'exe': None, 'native': False, 'dir': None},
                                    'runtimes': {'active': None, 'runtimes': [], 'warnings': []}})
        self.menu.start()

    def tearDown(self):
        self.menu.stop()
        if self._env is not None:
            os.environ['BB_CRASH_REPORTS'] = self._env
        self.tmp.cleanup()

    def call(self, path, body=None):
        req = urllib.request.Request(f'http://127.0.0.1:{self.menu.port}{path}', headers={
            'X-BB-Token': self.menu.token, **({'Content-Type': 'application/json'} if body is not None else {})},
            data=None if body is None else json.dumps(body).encode())
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read())

    def test_first_launch_asks_then_remembers(self):
        self.assertFalse(crash_report.consent_stored(self.user, {}))
        state = self.call('/api/state')
        self.assertEqual(state['crash'], {'available': True, 'asked': False, 'mode': 'ask'})
        self.assertIn('id="consent"', bbvr_menu.PAGE)
        state = self.call('/api/crash', {'mode': 'on'})
        self.assertEqual(state['crash'], {'available': True, 'asked': True, 'mode': 'on'})
        self.assertEqual(json.loads((self.user / 'crash_config.json').read_text())['crash_reports'], 'on')
        self.assertTrue(crash_report.consent_stored(self.user, {}))
        self.assertEqual(self.call('/api/crash', {'mode': 'off'})['crash']['mode'], 'off')

    def test_bad_mode_is_ignored(self):
        self.assertFalse(self.call('/api/crash', {'mode': 'maybe'})['crash']['asked'])
        self.assertFalse((self.user / 'crash_config.json').exists())

    def test_stored_answer_means_no_console_prompt(self):
        crash_report.save_mode(self.user, 'off')
        asked = []
        mode = crash_report.load_config(self.user, {})['crash_reports']
        self.assertEqual(mode, 'off')
        # handle_exit asks only in mode 'ask'; with a stored answer input() is never called
        crash_report.handle_exit(self.user, set(), ['x'], 1, env={}, input_fn=lambda p: asked.append(p) or 'y', interactive=True)
        crash_report.save_mode(self.user, 'on')
        crash_report.handle_exit(self.user, set(), ['x'], 1, env={}, input_fn=lambda p: asked.append(p) or 'y', interactive=True)
        self.assertEqual(asked, [])

    def test_console_fallback_still_asks(self):
        asked = []
        crash_report.handle_exit(self.user, set(), ['x'], 1, env={}, input_fn=lambda p: asked.append(p) or 'n', interactive=True)
        self.assertEqual(len(asked), 1)
        self.assertTrue(crash_report.consent_stored(self.user, {}))


if __name__ == '__main__':
    unittest.main()
