"""Runs the comfort dash test binary (tests/test_vr_dash.c: gpu/xr_bridge/vr_dash.h built as C,
src/vr_dash_detect.h).

Build it under the shared build lock with: ninja -C out/gpu vr-dash-test
(it needs no GPU device, game or headset). The binary prints PASS lines and exits 0 on success."""
from paths import ROOT
import subprocess
import unittest


def _exe():
    for candidate in (ROOT / 'out/gpu/vr-dash-test.exe', ROOT / 'out/gpu/vr-dash-test'):
        if candidate.exists():
            return candidate
    return None


@unittest.skipUnless(_exe(), 'build with: ninja -C out/gpu vr-dash-test')
class VrDashTests(unittest.TestCase):
    def test_all_pass(self):
        r = subprocess.run([str(_exe().resolve())], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('All VR dash tests passed', r.stdout)


if __name__ == '__main__':
    unittest.main()
