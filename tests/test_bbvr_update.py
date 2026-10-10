"""Update check of the dashboard (tools/release/zip/bbvr_update.py): version order, release picking, quiet failures."""
from paths import ROOT
import sys
import tempfile
import unittest
import urllib.error

sys.path.insert(0, str(ROOT / 'tools' / 'release' / 'zip'))
import bbvr_update as U  # noqa: E402


class VersionTests(unittest.TestCase):
    def test_order(self):
        newer = [('0.1.0-alpha5', '0.1.0-alpha4'), ('0.1.0', '0.1.0-alpha9'), ('v0.1.0-beta1', '0.1.0-alpha9'),
                 ('0.1.0-alpha5', '0.1.0-alpha5-pre'), ('0.2.0-alpha1', '0.1.9'), ('0.1.0-rc1', '0.1.0-beta3')]
        for a, b in newer:
            self.assertTrue(U.is_newer(a, b), (a, b))
            self.assertFalse(U.is_newer(b, a), (b, a))
        self.assertFalse(U.is_newer('0.1.0-alpha4', '0.1.0-alpha4'))

    def test_pick_latest_skips_drafts_and_junk(self):
        rel = [{'tag_name': 'v0.1.0-alpha4', 'html_url': 'u4'}, {'tag_name': 'v0.1.0-alpha6', 'draft': True},
               {'tag_name': 'v0.1.0-alpha5', 'html_url': 'u5'}, 'junk', {'html_url': 'x'}]
        self.assertEqual(U.pick_latest(rel), ('v0.1.0-alpha5', 'u5'))
        self.assertIsNone(U.pick_latest({'message': 'Not Found'}))


class CheckTests(unittest.TestCase):
    def test_private_repo_404_is_unavailable(self):
        def not_found(url):
            raise urllib.error.HTTPError(url, 404, 'Not Found', {}, None)
        out = U.check('0.1.0-alpha4', {}, fetcher=not_found)
        self.assertEqual(out['status'], 'unavailable')

    def test_newer_and_current(self):
        rel = [{'tag_name': 'v0.1.0-alpha5', 'html_url': 'https://example.org/r'}]
        out = U.check('0.1.0-alpha4', {}, fetcher=lambda u: rel)
        self.assertEqual((out['status'], out['latest'], out['url']), ('newer', '0.1.0-alpha5', 'https://example.org/r'))
        self.assertEqual(U.check('0.1.0-alpha5', {}, fetcher=lambda u: rel)['status'], 'current')

    def test_address_is_configurable_and_https_only(self):
        seen = []
        U.check('1', {'update_repo': 'me/mine'}, fetcher=lambda u: seen.append(u) or [], env={})
        self.assertEqual(seen, ['https://api.github.com/repos/me/mine/releases'])
        U.check('1', {'update_api_url': 'https://h.example/api'}, fetcher=lambda u: seen.append(u) or [], env={})
        self.assertEqual(seen[-1], 'https://h.example/api')
        called = []
        out = U.check('1', {'update_api_url': 'http://insecure.example/'}, fetcher=lambda u: called.append(u), env={})
        self.assertEqual((out['status'], called), ('unavailable', []))

    def test_summary_recompares_cached_answer_with_running_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            U.save_cache(tmp, {'status': 'newer', 'latest': '0.1.0-alpha5', 'url': 'u'})
            self.assertEqual(U.summary(True, '0.1.0-alpha4', {}, tmp)['status'], 'newer')
            self.assertEqual(U.summary(True, '0.1.0-alpha5', {}, tmp)['status'], 'current')
            self.assertEqual(U.summary(False, '0.1.0-alpha4', {}, tmp)['status'], 'disabled')


if __name__ == '__main__':
    unittest.main()
