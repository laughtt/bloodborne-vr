"""tools/release/online_checks.py: the public-build checks (online client in, server and dev tools out), the export resolver
for BB_NO_ONLINE / BB_PUBLIC_ALPHA, and make_release's --check-only. The forbidden words are built from pieces so that
this file passes the export scan itself."""
from paths import ROOT
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(ROOT / 'tools' / 'release'))
import online_checks as C  # noqa: E402
import export_public as X  # noqa: E402

CLIENT = (b'shad' + b'net sceNpMatching2 sceHttp BB_SHADNET_SERVER BB_ONLINE_AUTOBELL BB_ONLINE_ROLE '
          b'BB_ONLINE_FREE_BELLS BB_ONLINE_BELL_GIVE 127.0.0.1 1.1.1.1 239.255.255.250 %s:%d')
ACCOUNT = b'Hunter' + b'One'
PASSWORD = b'change' + b'me'
HOST = b'thehunters' + b'dream.com'
PORT = b'3131' + b'3'
DEV = b'BB_DEBUG_' + b'GIVE'


def errors(data, **kw):
    return C.check_exe(data, **kw)[0]


class ExeChecks(unittest.TestCase):
    def test_clean_online_client_passes(self):
        e, w, p = C.check_exe(CLIENT)
        self.assertEqual(e, [])
        self.assertTrue(any('online client present' in x for x in p))
        self.assertTrue(any('room bells present' in x for x in p))

    def test_generic_client_strings_are_allowed(self):
        # loopback, DNS, SSDP multicast, format strings and the HTTP route names of the client are not server data
        extra = b' http://schemas.example.org/soap 0.0.0.0 255.255.255.255 https://%s:%d/summon_' + b'messenger/create'
        self.assertEqual(errors(CLIENT + extra), [])

    def test_missing_online_client_fails_for_online_variant(self):
        e = errors(b'single player only BB_ONLINE_ROLE')
        self.assertTrue(any(x.startswith('online client missing') for x in e))
        self.assertTrue(any(x.startswith('room bells missing') for x in e))

    def test_missing_room_bells_alone_fails(self):
        e = errors(CLIENT.replace(b'BB_ONLINE_AUTOBELL', b'X'))
        self.assertEqual([x.split(':')[0] for x in e], ['room bells missing'])

    def test_test_accounts_rejected(self):
        for word in (ACCOUNT, ACCOUNT.lower(), b'Hunter' + b'Two', b'Hunter' + b'Three', PASSWORD, PASSWORD.upper()):
            self.assertTrue(any(x.startswith('test account') for x in errors(CLIENT + b' ' + word)), word)

    def test_server_hosts_rejected(self):
        for word in (HOST, b'scej-' + b'network.jp', b'srv.shadps4.' + b'net'):
            self.assertTrue(any(x.startswith('server host') for x in errors(CLIENT + b' https://' + word + b'/x')), word)

    def test_hardcoded_ip_and_port_rejected(self):
        for word in (b'203.0.113.7', b'10.1.2.3', b'127.0.0.1:' + PORT, b'play.example.com:443'):
            e = errors(CLIENT + b' ' + word)
            self.assertTrue(any(x.startswith(('hard-coded IP', 'host:port')) for x in e), (word, e))
        self.assertTrue(any(x.startswith('server port') for x in errors(CLIENT + b' port ' + PORT)))
        # ... but a number that merely contains the digits is not a port
        self.assertEqual(errors(CLIENT + b' 1' + PORT + b'9'), [])

    def test_server_code_and_private_paths_rejected(self):
        for word in (b'shadnet-' + b'server', b'bb-coop-' + b'local/coopd', b'C:\\game\\shadnet-' + b'server\\src', b'/online/bb-x',
                     b'tokio::runtime'):
            self.assertTrue(any(x.startswith('server code') for x in errors(CLIENT + b' ' + word)), word)

    def test_dev_tools_rejected_in_public_build_only(self):
        for word in (DEV, b'BB_DEBUG_' + b'USE', b'BB_NET_' + b'TRACE', b'BB_NET_' + b'BREAK', b'speffect' + b':12'):
            self.assertTrue(any(x.startswith('dev tool') for x in errors(CLIENT + b' ' + word)), word)
        self.assertEqual(C.check_exe(CLIENT + b' ' + DEV, public=False)[0], [])

    def test_maintainer_path_is_an_error_in_a_public_build(self):
        e, w, _ = C.check_exe(CLIENT + b' C:/bloodborne_' + b'vr/src/x.c')
        self.assertTrue(any('maintainer path' in x for x in e))
        e, w, _ = C.check_exe(CLIENT + b' C:/bloodborne_' + b'vr/src/x.c', public=False)
        self.assertEqual(e, [])
        self.assertTrue(w)

    def test_offline_variant_must_not_hold_the_client(self):
        self.assertEqual(errors(b'single player', variant='offline'), [])
        self.assertTrue(errors(CLIENT, variant='offline'))


class TreeChecks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'bin').mkdir()
        (self.root / 'bin' / 'bb-probe.exe').write_bytes(CLIENT)
        (self.root / 'run.py').write_text('print("hi")\n')

    def tearDown(self):
        self.tmp.cleanup()

    def test_clean_tree_passes(self):
        e, w, p = C.check_tree(self.root)
        self.assertEqual(e, [])

    def test_package_versions_in_the_sources_list_are_not_addresses(self):
        (self.root / "SOURCES-THIRD-PARTY.txt").write_text("libfoo 0.8.9.0-1 10.1.2.3:1234 end")
        e, _, _ = C.check_tree(self.root)
        self.assertEqual(len(e), 1)
        self.assertIn('hard-coded IP address', e[0])
        self.assertNotIn('0.8.9.0', e[0])

    def test_maintainer_path_in_a_helper_binary_fails(self):
        (self.root / 'bin' / 'bb-gpu-capabilities.exe').write_bytes(b'MZ C:/bloodborne_' + b'vr/tools/x.c')
        e, _, _ = C.check_tree(self.root)
        self.assertTrue(any('maintainer path' in x for x in e))

    def test_account_in_a_config_file_fails(self):
        (self.root / 'defaults.json').write_text('{"user": "' + ACCOUNT.decode() + '"}')
        self.assertTrue(any('test account' in x for x in C.check_tree(self.root)[0]))

    def test_server_address_in_a_script_fails(self):
        (self.root / 'run.py').write_text('SERVER = "' + HOST.decode() + ':' + PORT.decode() + '"\n')
        self.assertTrue(C.check_tree(self.root)[0])

    def test_server_folder_fails(self):
        (self.root / 'online').mkdir()
        (self.root / 'online' / 'x.txt').write_text('x')
        self.assertTrue(any('server file' in x for x in C.check_tree(self.root)[0]))

    def test_bundled_python_and_licences_are_not_scanned(self):
        (self.root / 'python').mkdir()
        (self.root / 'python' / 'x.py').write_text('a = "203.0.113.7"\n')
        self.assertEqual(C.check_tree(self.root)[0], [])

    def test_dll_version_numbers_are_not_addresses(self):
        (self.root / 'BloodborneVR.exe').write_bytes(b'version 6.7.8.5 ' + CLIENT)
        self.assertEqual(C.check_tree(self.root)[0], [])


class Cli(unittest.TestCase):
    def test_make_release_check_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            good, bad = Path(tmp) / 'good.exe', Path(tmp) / 'bad.exe'
            good.write_bytes(CLIENT)
            bad.write_bytes(CLIENT + b' ' + ACCOUNT)
            run = lambda exe: subprocess.run([sys.executable, str(ROOT / 'tools' / 'release' / 'make_release.py'), '--exe', str(exe),
                                              '--check-only'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            ok, ko = run(good), run(bad)
            self.assertEqual(ok.returncode, 0, ok.stdout)
            self.assertIn('CHECK PASSED', ok.stdout)
            self.assertEqual(ko.returncode, 1, ko.stdout)
            self.assertIn('FAIL', ko.stdout)

    def test_allow_online_flag_is_gone(self):
        r = subprocess.run([sys.executable, str(ROOT / 'tools' / 'release' / 'make_release.py'), '--allow-online'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.assertNotEqual(r.returncode, 0)


class ExportResolver(unittest.TestCase):
    def resolve(self, name, text):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
            X.resolve_build_options(Path(tmp))
            return path.read_text(encoding='utf-8')

    def test_dev_blocks_removed_online_blocks_kept(self):
        src = ('#ifndef BB_PUBLIC_ALPHA\ndev();\n#endif\n#ifndef BB_NO_ONLINE\nonline();\n#endif\n'
               '#ifdef BB_NO_ONLINE\noffline();\n#endif\n#ifdef BB_PUBLIC_ALPHA\npublic_only();\n#else\nprivate_only();\n#endif\n')
        out = self.resolve('src/a.c', src)
        self.assertIn('online();', out)
        self.assertIn('public_only();', out)
        for gone in ('dev();', 'offline();', 'private_only();', 'BB_NO_ONLINE', 'BB_PUBLIC_ALPHA'):
            self.assertNotIn(gone, out)

    def test_gpu_cmake_options_become_fixed_values(self):
        out = self.resolve('gpu/CMakeLists.txt', 'option(BBPORT_ONLINE "x" ON)\noption(BBPORT_PUBLIC "y" OFF)\nif (NOT BB_PUBLIC_BUILD)\nx()\nendif()\n')
        self.assertIn('set(BBPORT_ONLINE ON)', out)
        self.assertIn('set(BBPORT_PUBLIC ON)', out)
        self.assertIn('x()', out)  # cmake decides; the dev tests' sources are simply not evaluated

    def test_bridge_cmake_keeps_the_offline_resolution(self):
        out = self.resolve('gpu/xr_bridge/CMakeLists.txt',
                           'option(BBPORT_ONLINE "x" ON)\nif (BBPORT_ONLINE)\npose()\nendif()\nif (NOT BBPORT_ONLINE)\nadd()\nendif()\n')
        self.assertNotIn('pose()', out)
        self.assertIn('add()', out)

    def test_online_client_is_not_denied_but_the_server_and_dev_tools_are(self):
        denied = [pattern for pattern, _ in X.DENY]
        for client in ('gpu/bbnet/**', 'gpu/shadps4/shadnet/**', 'gpu/shadps4/core/libraries/np/**',
                       'gpu/shadps4/core/libraries/network/**'):
            self.assertNotIn(client, denied)
        for private in ('online/**', 'src/vr_debug_give.h', 'src/net_trace_fmt.h', 'src/runtime_vr_netstate.c', 'scripts/online_hosts.py'):
            self.assertIn(private, denied)

    def test_scan_flags_server_names_outside_the_client(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'docs').mkdir()
            (root / 'docs' / 'a.md').write_text('use ' + HOST.decode() + ' with ' + ACCOUNT.decode() + '\n')
            (root / 'gpu' / 'shadps4' / 'core' / 'libraries' / 'network').mkdir(parents=True)
            (root / 'gpu' / 'shadps4' / 'core' / 'libraries' / 'network' / 'http.cpp').write_text('"/summon_' + 'messenger/create"\n')
            errs, _ = X.scan(root, None)
            where = [e[0] for e in errs]
            self.assertTrue(any(w.startswith('docs/a.md') for w in where))
            self.assertFalse(any('http.cpp' in w for w in where))


if __name__ == '__main__':
    unittest.main()
