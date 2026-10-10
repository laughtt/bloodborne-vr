"""BloodborneVR.exe (tools/release/zip/exe): argument forwarding and exit code.

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

sys.path.insert(0, str(ROOT / 'tools' / 'release'))
sys.path.insert(0, str(ROOT / 'tools' / 'release' / 'zip'))
import build_stub  # noqa: E402

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


if __name__ == '__main__':
    unittest.main()
