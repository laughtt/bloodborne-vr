"""Super High / DLSS-NR follow-ups: the player's own nvngx_dlssnr.dll ("Find file..."), the help page ("What is this?"),
the removed Ultra preset, true stereo for every preset, Display size next to the presets
(tools/release/zip/bbvr_settings.py, bbvr_menu.py, bbvr_cli.py, tools/release/dashboard/main.cpp)."""
from paths import ROOT
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
import urllib.request
from unittest import mock

ZIP = ROOT / 'tools' / 'release' / 'zip'
sys.path.insert(0, str(ZIP))
import bbvr_cli  # noqa: E402
import bbvr_launcher as L  # noqa: E402
import bbvr_menu  # noqa: E402
import bbvr_settings as S  # noqa: E402

DOC = ROOT / 'docs' / 'vr' / 'dlss-nr.md'
GOOD_DLL = b'MZ' + bytes(100 * 1024)


def make_port(tmp):
    """A fake install: bin/bb-probe.exe with the DLSS strings, so the DLL belongs next to it."""
    port = Path(tmp)
    (port / 'bin').mkdir(parents=True, exist_ok=True)
    (port / 'bin' / 'bb-probe.exe').write_bytes(b'MZ BB_XR_DLSS_QUALITY dlss_sharpen BB_XR_DLSSNR' + bytes(64))
    (port / 'bin' / 'bbport_dlss.dll').write_bytes(b'MZ BbDlssGetNrApi' + bytes(16))
    (port / 'bin' / 'nvngx_dlss.dll').write_bytes(b'x')
    return port


class UltraAndStereoTests(unittest.TestCase):
    def test_a_saved_ultra_loads_as_high(self):
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / 's.json'
            for saved in ('Ultra', 'ultra', 'ULTRA'):
                f.write_text(json.dumps({'version': 3, 'profile': 'Default', 'quality': saved}), encoding='utf-8')
                self.assertEqual(S.load_settings(f)['quality'], 'High', saved)
        self.assertEqual(S.QUALITY_ORDER, ['Low', 'Medium', 'High', 'Super High'])
        self.assertNotIn('Ultra', S.QUALITY_PRESETS)
        self.assertNotIn('Ultra', S.QUALITY_NOTES)

    def test_every_preset_is_true_stereo_and_nothing_sets_hybrid(self):
        gpu = {'gpus': [{'name': 'NVIDIA GeForce RTX 4090', 'rtx': True}], 'runtimes': {'active': None, 'runtimes': [], 'warnings': []},
               'dlss': {'ok': False, 'reason': 'x', 'exe': None, 'native': False, 'dir': None}}
        with tempfile.TemporaryDirectory() as tmp:
            paths = S.make_paths(Path(tmp), Path(tmp) / 'game')
            for q in S.QUALITY_ORDER:
                self.assertNotIn('mode', S.preset_values(q, True), q)  # presets never touch the render mode
                self.assertNotIn('mode', S.preset_values(q, False), q)
                plan = S.build_launch(S.effective_values(S.set_quality(S.default_settings(), q), gpu), paths, gpu, public=False)
                self.assertEqual((plan['env']['BB_XR_MODE'], plan['env']['BB_STEREO_FULL']), ('stereo', '1'), q)
        for oid in ('mode',):
            o = S.BY_ID[oid]
            self.assertEqual(o['default'], 'stereo')
            self.assertEqual(o['adv_choices'], ['alt'])  # alternate-eye only under advanced options
        self.assertEqual(S.BY_ID['mode']['choices'], ['stereo', 'mono', 'alt'])
        # Mono stays as the manual fallback
        with tempfile.TemporaryDirectory() as tmp:
            paths = S.make_paths(Path(tmp), Path(tmp) / 'game')
            plan = S.build_launch(S.effective_values(dict(S.default_settings(), overrides={'mode': 'mono'}), gpu), paths, gpu, public=False)
            self.assertEqual(plan['env']['BB_XR_MODE'], 'mono')
            self.assertNotIn('BB_STEREO_FULL', plan['env'])
        text = (ZIP / 'bbvr_settings.py').read_text(encoding='utf-8')
        self.assertNotRegex(text, r"(?i)'hybrid'|BB_XR_MODE['\"]?\s*,\s*['\"]hybrid")


class DisplaySizeWithPresetsTests(unittest.TestCase):
    def test_presets_start_the_resolution_step_and_the_headset_choice_is_sticky(self):
        low = S.effective_values(S.set_quality(S.default_settings(), 'Low'), {})
        med = S.effective_values(S.set_quality(S.default_settings(), 'Medium'), {})
        high = S.effective_values(S.default_settings(), {})
        self.assertEqual((low['display_scale'], med['display_scale'], high['display_scale']), ('70', '85', '100'))
        # a hardware choice is kept in settings['hw'] and survives a preset change
        s = S.apply_changes(S.default_settings(), {'display_size': 'quest3', 'display_scale': '125'}, {})
        self.assertEqual(s['hw'], {'display_size': 'quest3'})
        self.assertEqual(s['overrides'], {'display_scale': '125'})
        s = S.set_quality(s, 'Low')
        v = S.effective_values(s, {})
        self.assertEqual((v['display_size'], v['display_scale']), ('quest3', '70'))  # a preset drops the edited % it sets; the headset stays
        v = S.effective_values(S.apply_changes(s, {'display_scale': '125'}, {}), {})
        self.assertEqual(v['display_scale'], '125')  # the player's % over the preset's 70
        self.assertEqual(S.effective_values(S.set_quality(S.apply_changes(S.default_settings(), {'display_size': 'quest3'}, {}), 'Low'), {})['display_size'], 'quest3')
        self.assertEqual(S.apply_changes(s, {'display_size': 'auto'}, {})['hw'], {})

    def test_the_table_and_the_page_list_it(self):
        rows = {p['name']: dict(p['rows']) for p in S.preset_table({})['presets']}
        self.assertEqual(rows['Low']['Display resolution'], '70% of the headset size')
        self.assertEqual(rows['High']['Display resolution'], 'headset size (100%)')
        graphics = next(p for p in S.PAGES if p['id'] == 'graphics')
        self.assertIn('display_size', [i for _, ids in graphics['sections'] for i in ids])


class FindFileTests(unittest.TestCase):
    def test_copies_the_players_file_next_to_the_exe(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as src_dir:
            port = make_port(tmp)
            src = Path(src_dir) / 'nvngx_dlssnr.dll'
            src.write_bytes(GOOD_DLL)
            self.assertFalse(S.detect_dlssnr(port, None, [{'name': 'RTX 4090', 'rtx': True}])['ok'])
            r = S.install_nr_dll(src, port)
            self.assertTrue(r['ok'], r)
            self.assertEqual(Path(r['dest']), port / 'bin' / 'nvngx_dlssnr.dll')
            self.assertEqual((port / 'bin' / 'nvngx_dlssnr.dll').read_bytes(), GOOD_DLL)
            self.assertTrue(S.detect_dlssnr(port, None, [{'name': 'RTX 4090', 'rtx': True}])['ok'])  # Super High is available now
            self.assertTrue(S.install_nr_dll(port / 'bin' / 'nvngx_dlssnr.dll', port)['ok'])  # picking the copy itself is fine
            self.assertFalse(list((port / 'bin').glob('*.tmp')))

    def test_refuses_the_wrong_file(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as src_dir:
            port = make_port(tmp)
            d = Path(src_dir)
            (d / 'other.dll').write_bytes(GOOD_DLL)
            (d / 'nvngx_dlssnr.dll').write_bytes(b'not a dll' * 20000)
            small = d / 'small'
            small.mkdir()
            (small / 'nvngx_dlssnr.dll').write_bytes(b'MZ')
            for path, text in ((d / 'other.dll', 'named nvngx_dlssnr.dll'), (d / 'nvngx_dlssnr.dll', 'not a usable'),
                               (small / 'nvngx_dlssnr.dll', 'not a usable'), (d / 'missing' / 'nvngx_dlssnr.dll', 'does not exist'),
                               ('', 'No file chosen')):
                r = S.install_nr_dll(path, port)
                self.assertFalse(r['ok'])
                self.assertIn(text, r['message'])
            self.assertFalse((port / 'bin' / 'nvngx_dlssnr.dll').exists())

    def test_no_exe_no_copy(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as src_dir:
            src = Path(src_dir) / 'nvngx_dlssnr.dll'
            src.write_bytes(GOOD_DLL)
            r = S.install_nr_dll(src, Path(tmp))
            self.assertFalse(r['ok'])
            self.assertIn('bb-probe.exe', r['message'])

    def test_the_menu_and_its_endpoint(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as src_dir:
            port = make_port(tmp)
            src = Path(src_dir) / 'nvngx_dlssnr.dll'
            src.write_bytes(GOOD_DLL)
            paths = S.make_paths(port, port / 'game')
            menu = bbvr_menu.Menu(port / 'u' / 's.json', paths, S.detect_all(port))
            with mock.patch.object(S, 'detect_gpus', return_value=[{'name': 'NVIDIA GeForce RTX 4090', 'rtx': True}]):
                menu.caps = S.detect_all(port)
                url = menu.start()
                try:
                    base, token = url.split('/?t=')
                    req = urllib.request.Request(base + '/api/nr-dll', data=json.dumps({'path': str(src)}).encode(),
                                                 headers={'Content-Type': 'application/json', 'X-BB-Token': token})
                    out = json.load(urllib.request.urlopen(req))
                finally:
                    menu.stop()
                self.assertTrue(out['result']['ok'], out)
                self.assertTrue(out['state']['caps']['dlssnr']['ok'])
                self.assertTrue(next(p for p in out['state']['quality_table']['presets'] if p['name'] == 'Super High')['available'])

    def test_cli_nr_dll_command(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as src_dir, tempfile.TemporaryDirectory() as data:
            port = make_port(tmp)
            src = Path(src_dir) / 'nvngx_dlssnr.dll'
            src.write_bytes(GOOD_DLL)
            with mock.patch.object(L, 'PORT', port), mock.patch.object(L, 'DATA', Path(data)), \
                    mock.patch.object(L, 'LOGS', Path(data) / 'logs'), mock.patch.object(L, 'CONFIG', Path(data) / 'c.json'):
                st = bbvr_cli.run(['nr-dll', str(src)])
            self.assertTrue(st['nr_dll']['ok'], st['nr_dll'])
            self.assertTrue((port / 'bin' / 'nvngx_dlssnr.dll').is_file())


class HelpPageTests(unittest.TestCase):
    def test_the_page_says_what_it_must_and_has_no_download_links(self):
        text = DOC.read_text(encoding='utf-8')
        for want in ('DLSS-NR', 'nvngx_dlssnr.dll', 'not part of the public DLSS SDK', 'Find file...', 'runs **High**',
                     'official SDK', '50-54 fps', 'RTX 4090'):
            self.assertIn(want, text)
        urls = re.findall(r'https?://\S+', text)
        self.assertEqual(urls, ['https://developer.nvidia.com/rtx/dlss'])  # the only external link: "more about DLSS"
        for bad in ('drive.google', 'mirror', 'forum', 'mega.nz', 'dropbox', 'github.com/', 'download'):
            self.assertNotIn(bad, text.lower() if bad != 'download' else text.lower().replace('does not link to a download for it', ''))

    def test_no_download_link_in_the_launcher_texts_or_docs(self):
        files = [ZIP / 'bbvr_settings.py', ZIP / 'bbvr_menu.py', ZIP / 'bbvr_cli.py', ZIP / 'README.txt',
                 ROOT / 'tools' / 'release' / 'dashboard' / 'main.cpp', ROOT / 'tools' / 'release' / 'public' / 'README.md']
        for f in files:
            for line in f.read_text(encoding='utf-8').splitlines():
                if ('dlssnr' in line.lower() or 'dlss-nr' in line.lower()) and 'NR_DOC_GITHUB' not in line:  # the repo's own help page
                    self.assertNotRegex(line, r'(?i)drive\.google|mega\.nz|dropbox|forums?\.|nexusmods|https?://\S*(dlssnr|dlss-nr)', f.name)

    def test_local_copy_else_github(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as data:
            port = Path(tmp)
            gh = S.nr_doc_url(port, data)
            self.assertEqual(gh['where'], 'github')
            self.assertEqual(gh['url'], 'https://github.com/laughtt/bloodborne-vr/blob/HEAD/docs/vr/dlss-nr.md')
            (port / 'docs' / 'vr').mkdir(parents=True)
            (port / 'docs' / 'vr' / 'dlss-nr.md').write_text(DOC.read_text(encoding='utf-8'), encoding='utf-8')
            local = S.nr_doc_url(port, data)
            self.assertEqual(local['where'], 'local')
            self.assertTrue(local['url'].startswith('file:///'))
            html = (Path(data) / 'dlss-nr-help.html').read_text(encoding='utf-8')
            self.assertIn('<h1>Super High and DLSS-NR</h1>', html)
            self.assertIn('nvngx_dlssnr.dll', html)
            (port / 'docs' / 'dlss-nr.md').write_text('# release copy\n', encoding='utf-8')  # the zip layout is found too
            self.assertEqual(S.nr_doc_source(port), port / 'docs' / 'dlss-nr.md')

    def test_open_uses_the_browser_and_the_endpoint_and_cli(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as data:
            port = make_port(tmp)
            with mock.patch('webbrowser.open', return_value=True) as opened:
                r = S.open_nr_doc(port, data)
                self.assertTrue(r['ok'])
                opened.assert_called_once_with(r['url'])
                paths = S.make_paths(port, port / 'game')
                menu = bbvr_menu.Menu(port / 'u' / 's.json', paths, S.detect_all(port))
                self.assertEqual(menu.open_nr_help()['where'], 'github')
                with mock.patch.object(L, 'PORT', port), mock.patch.object(L, 'DATA', Path(data)), \
                        mock.patch.object(L, 'LOGS', Path(data) / 'logs'), mock.patch.object(L, 'CONFIG', Path(data) / 'c.json'):
                    st = bbvr_cli.run(['nr-help'])
                self.assertEqual(st['nr_help']['where'], 'github')

    def test_the_buttons_exist_in_the_web_menu_and_the_dashboard(self):
        self.assertIn('Find file...', bbvr_menu.PAGE)
        self.assertIn('What is this?', bbvr_menu.PAGE)
        self.assertIn('/api/nr-dll', bbvr_menu.PAGE)
        self.assertIn('/api/nr-help', bbvr_menu.PAGE)
        cpp = (ROOT / 'tools' / 'release' / 'dashboard' / 'main.cpp').read_text(encoding='utf-8')
        for want in ('"Find file..."', '"What is this?"', '{"nr-dll"', '{"nr-help"', 'nvngx_dlssnr.dll'):
            self.assertIn(want, cpp)

    def test_the_help_text_and_cards_say_it_is_not_included(self):
        for text in (S.QUALITY_NOTES['Super High'], S.BY_ID['dlssnr']['help'], S.NR_DLL_MISSING):
            self.assertRegex(text, r'(?i)not included')
        self.assertIn('Find file', S.NR_DLL_MISSING)

    def test_release_ships_the_page_but_never_the_dll(self):
        text = (ROOT / 'tools' / 'release' / 'make_release.py').read_text(encoding='utf-8')
        self.assertIn("'dlss-nr.md'", text)
        self.assertIn("NEVER_SHIP = {'nvngx_dlssnr.dll'}", text)
        self.assertNotIn('for name in NEVER_SHIP', text)  # it clobbered the release name (the zip was called nvngx_dlssnr.dll.zip)


RTX = [{'name': 'NVIDIA GeForce RTX 4090', 'rtx': True}]


class InstalledLayoutTests(unittest.TestCase):
    """Setup.exe layout: program folder (installed.flag, bin/) apart from the data folder (%LOCALAPPDATA%/BloodborneVR).
    The model file goes next to the game exe in bin/, which is where the bridge loads it from; the choice is saved in the data folder."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.port = make_port(root / 'Programs' / 'BloodborneVR')
        (self.port / S.INSTALLED_FLAG).write_text('', encoding='utf-8')
        self.data = S.data_root(self.port, {'LOCALAPPDATA': str(root / 'Local')})
        self.assertNotEqual(self.data, self.port)
        self.src = root / 'Downloads' / 'nvngx_dlssnr.dll'
        self.src.parent.mkdir()
        self.src.write_bytes(GOOD_DLL)
        self.settings_file = self.data / 'user' / 'bbvr_settings.json'
        patcher = mock.patch.object(S, 'detect_gpus', return_value=RTX)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(self._tmp.cleanup)

    def test_copy_then_select_save_reload_and_launch(self):
        caps = S.detect_all(self.port, data=self.data)
        self.assertFalse(caps['dlssnr']['ok'])
        self.assertFalse(caps['dlssnr']['dll'])
        menu = bbvr_menu.Menu(self.settings_file, S.make_paths(self.port, self.port / 'game', self.data), caps)
        menu.update({'quality': 'Super High'})  # greyed out: a stale click does nothing
        self.assertEqual(menu.settings['quality'], 'High')
        result = menu.install_nr_dll(str(self.src))
        self.assertTrue(result['ok'] and result['available'], result)
        self.assertTrue((self.port / 'bin' / 'nvngx_dlssnr.dll').is_file())  # next to bb-probe.exe, not in the data folder
        self.assertFalse((self.data / 'nvngx_dlssnr.dll').exists())
        self.assertTrue(menu.state()['caps']['dlssnr']['ok'])  # refreshed at once, no restart
        menu.update({'quality': 'Super High'})
        self.assertEqual(menu.settings['quality'], 'Super High')
        self.assertEqual(json.loads(self.settings_file.read_text(encoding='utf-8'))['quality'], 'Super High')
        # reopened: a new Menu loads the file and checks again
        again = bbvr_menu.Menu(self.settings_file, S.make_paths(self.port, self.port / 'game', self.data), S.detect_all(self.port, data=self.data))
        st = again.state()
        self.assertEqual((st['quality'], st['quality_notice']), ('Super High', ''))
        values = S.effective_values(again.settings, again.caps)
        plan = S.build_launch(values, S.make_paths(self.port, self.port / 'game', self.data), again.caps, public=False)
        self.assertEqual((plan['env']['BB_XR_DLSSNR'], plan['env']['BB_XR_DLSSNR_PLACE']), ('1', 'pre'))
        self.assertEqual(plan['warnings'], [])

    def test_a_missing_file_later_gives_a_notice_not_a_silent_switch(self):
        S.install_nr_dll(self.src, self.port)
        S.save_settings(self.settings_file, S.set_quality(S.default_settings(), 'Super High'))
        (self.port / 'bin' / 'nvngx_dlssnr.dll').unlink()
        caps = S.detect_all(self.port, data=self.data)
        settings = S.load_settings(self.settings_file)
        self.assertEqual(settings['quality'], 'Super High')  # kept
        notice = S.page_state(settings, caps, public=False)['quality_notice']
        self.assertIn('Super High is selected', notice)
        self.assertIn('nvngx_dlssnr.dll', notice)
        plan = S.build_launch(S.effective_values(settings, caps), S.make_paths(self.port, self.port / 'game', self.data), caps, public=False)
        self.assertEqual(plan['env']['BB_XR_DLSSNR'], '0')  # NR settings only with the file
        self.assertNotIn('BB_XR_DLSSNR_PLACE', plan['env'])
        self.assertTrue(any('DLSS-NR' in w for w in plan['warnings']))

    def test_a_bridge_from_before_dlss_nr_is_reported_not_blamed_on_the_file(self):
        (self.port / 'bin' / 'bbport_dlss.dll').write_bytes(b'MZ old bridge' + bytes(16))  # what alpha11 shipped
        r = S.install_nr_dll(self.src, self.port)
        self.assertTrue(r['ok'])
        self.assertFalse(r['available'])
        self.assertIn('too old for DLSS-NR', r['message'])
        nr = S.detect_dlssnr(self.port)
        self.assertFalse(nr['ok'])
        self.assertTrue(nr['dll'])
        self.assertNotIn('Find file', nr['reason'])

    def test_a_failed_copy_says_why(self):
        with mock.patch.object(S.shutil, 'copyfile', side_effect=PermissionError(13, 'Access is denied')):
            r = S.install_nr_dll(self.src, self.port)
        self.assertFalse(r['ok'])
        self.assertIn('No permission to write', r['message'])
        self.assertFalse(list((self.port / 'bin').glob('nvngx_dlssnr*')))

    def test_make_release_refuses_a_stale_bridge(self):
        text = (ROOT / 'tools' / 'release' / 'make_release.py').read_text(encoding='utf-8')
        self.assertIn("b'BbDlssGetNrApi' not in", text)

    def test_the_web_menu_and_dashboard_show_the_notice(self):
        self.assertIn('S.quality_notice', bbvr_menu.PAGE)
        self.assertIn('quality_notice', (ROOT / 'tools' / 'release' / 'dashboard' / 'main.cpp').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
