"""Runs the VR head tracking math test binary (tests/test_vr_head.c, src/vr_head_math.h).

Build it under the shared build lock with: ninja -C out/gpu vr-head-test
(it needs no GPU device, game or headset). The binary prints PASS lines and exits 0 on success."""
from paths import ROOT
import subprocess
import unittest


def _exe():
    for candidate in (ROOT / 'out/gpu/vr-head-test.exe', ROOT / 'out/gpu/vr-head-test'):
        if candidate.exists():
            return candidate
    return None


@unittest.skipUnless(_exe(), 'build with: ninja -C out/gpu vr-head-test')
class VrHeadMathTests(unittest.TestCase):
    def test_all_pass(self):
        r = subprocess.run([str(_exe().resolve())], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('All VR head tests passed', r.stdout)


if __name__ == '__main__':
    unittest.main()
