"""Runs the host-call hook trampoline test binary (tests/test_hooks.c).

Build it under the shared build lock with: ninja -C out/gpu hooks-test
(it needs no GPU device or Vulkan). The binary prints PASS lines and exits 0 on success."""
from paths import ROOT
import subprocess
import unittest


def _exe():
    for candidate in (ROOT / 'out/gpu/hooks-test.exe', ROOT / 'out/gpu/hooks-test',
                      ROOT / 'out/hooks-test.exe', ROOT / 'out/hooks-test'):
        if candidate.exists():
            return candidate
    return None


@unittest.skipUnless(_exe(), 'build with: ninja -C out/gpu hooks-test')
class HookTrampolineTests(unittest.TestCase):
    def test_all_pass(self):
        r = subprocess.run([str(_exe().resolve())], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('All hook tests passed', r.stdout)


if __name__ == '__main__':
    unittest.main()
