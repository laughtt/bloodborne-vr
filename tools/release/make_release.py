#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the tester zip from BUILT artifacts and the public export tree. Standard library only.

    python tools/release/make_release.py [--exe out/gpu-public/bb-probe.exe] [--bridge out/bbport_xr.dll]
        [--python-embed PATH_OR_URL] [--version 0.1.0-alpha1] [--out out/release]

Nothing is compiled here. Inputs: the game host exe (installed as bin/bb-probe.exe; it must be the public
variant built with -DBBPORT_ONLINE=OFF in out/gpu-public, checked below for online / co-op strings), bbport_xr.dll,
bb-gpu-capabilities.exe, the OpenXR loader, and the MSYS2 CLANG64 runtime DLLs they need (found by
walking the PE import tables; no ldd needed). Scripts, patches and run.py come from the export tree
(run tools/release/export_public.py first), so only cleared files are packaged.

Layout of BloodborneVR-<version>/ in the zip:
    BloodborneVR.exe (the native dashboard, built here: ImGui + SDL3 linked in, one file; `--web` = the web menu),
    BloodborneVR-console.bat (fallback), README.txt, VERSION.txt, LICENSE, THIRD_PARTY_NOTICES.md, SOURCE.txt
    launcher/{bbvr_launcher,bbvr_menu,bbvr_settings,bbvr_cli,bbvr_update}.py (settings, web menu, the dashboard's helper,
    update check), defaults/{fp,stereo}.txt + release.json (update-check address),
    python/ (embeddable Python, optional)
    run.py, scripts/, patches/, bin/ (exes + DLLs), licenses/
bin/ also has NVIDIA's nvngx_dlss.dll and the bbport_dlss.dll bridge (licenses/NVIDIA-DLSS-*.txt; see --dlss-dir).
No game file, extracted asset or FSR 4 runtime is included.

After the zip, the one-click installer BloodborneVR-<version>-Setup.exe is built from the same folder with Inno Setup 6
(packaging/windows/BloodborneVR.iss; ISCC.exe is searched in --iscc, $ISCC, PATH and the usual install folders;
--no-installer skips it). Both files go into SHA256SUMS.txt. Crash reports are local only (no server setting).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import urllib.request
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_dashboard  # noqa: E402
import build_stub  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
ZIPSRC = REPO / 'tools' / 'release' / 'zip'
PY_EMBED_URL = 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip'
SYSTEM_DIRS = [Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32']
# DLLs left to the system / the GPU driver (never shipped even when the MSYS2 tree has a copy).
SYSTEM_DLLS = {'vulkan-1.dll'}
# NVIDIA DLSS runtime + our bridge (NVIDIA RTX SDKs LICENSE allows redistribution in an application).
DLSS_DLLS = ('bbport_dlss.dll', 'nvngx_dlss.dll')
# Licence file folders in <msys2>/share/licenses for the DLLs that may be shipped: dll prefix -> folder.
LICENSE_DIRS = {
    'sdl3': 'SDL3', 'libopenxr_loader': 'openxr-sdk', 'libssl': 'openssl', 'libcrypto': 'openssl',
    'libc++': 'libc++', 'libunwind': 'libc++', 'libwinpthread': 'libwinpthread', 'libxxhash': 'xxhash',
    'libzydis': 'zydis', 'libprotobuf': 'protobuf', 'zlib1': 'zlib', 'libabsl': 'abseil-cpp',
    'avcodec': 'ffmpeg', 'avformat': 'ffmpeg', 'avutil': 'ffmpeg', 'swresample': 'ffmpeg', 'swscale': 'ffmpeg',
}


def pe_imports(path):
    """Names of the DLLs a PE file imports (import table and delay-import table)."""
    data = path.read_bytes()
    if data[:2] != b'MZ':
        return []
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        return []
    nsec, = struct.unpack_from('<H', data, pe + 6)
    optsize, = struct.unpack_from('<H', data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from('<H', data, opt)
    dirs = opt + (112 if magic == 0x20B else 96)
    sections = []
    for i in range(nsec):
        s = opt + optsize + i * 40
        vsize, va, rawsize, raw = struct.unpack_from('<IIII', data, s + 8)
        sections.append((va, max(vsize, rawsize), raw))

    def offset(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return raw + rva - va
        return None

    names = []
    for index, stride, name_at in ((1, 20, 12), (13, 32, 4)):  # import, delay import
        rva, size = struct.unpack_from('<II', data, dirs + index * 8)
        start = offset(rva) if rva else None
        if start is None:
            continue
        pos = start
        while pos + stride <= len(data):
            name_rva, = struct.unpack_from('<I', data, pos + name_at)
            if not name_rva:
                break
            at = offset(name_rva)
            if at is None:
                break
            names.append(data[at:data.index(b'\0', at)].decode('ascii', 'replace'))
            pos += stride
    return names


def dll_closure(roots, search_dirs):
    """(found {lowercase name: path}, missing set) for the import closure of `roots`."""
    found, missing, queue = {}, set(), list(roots)
    while queue:
        current = queue.pop()
        for name in pe_imports(current):
            low = name.lower()
            if low in found or low in missing:
                continue
            if low.startswith(('api-ms-win-', 'ext-ms-')) or low in SYSTEM_DLLS:
                continue
            if any((d / name).is_file() for d in SYSTEM_DIRS) and not any((d / name).is_file() for d in search_dirs):
                continue
            for directory in search_dirs:
                candidate = directory / name
                if candidate.is_file():
                    found[low] = candidate
                    queue.append(candidate)
                    break
            else:
                missing.add(low)
    return found, missing


def license_for(dll, share):
    stem = dll.lower().rsplit('.', 1)[0]
    for prefix, folder in LICENSE_DIRS.items():
        if stem.startswith(prefix):
            for candidate in (share / folder).glob('*') if (share / folder).is_dir() else []:
                return folder, candidate
    return None, None


# Packages whose licence is the GPL (the MSYS2 FFmpeg is configured --enable-gpl --enable-version3 and links these).
GPL_PACKAGES = {'ffmpeg': 'GPL-3.0-or-later (built with --enable-gpl --enable-version3)',
                'libx264': 'GPL-2.0-or-later', 'x265': 'GPL-2.0-only', 'xvidcore': 'GPL-2.0-or-later'}
OWN_FILES = {'bbport_xr.dll', 'bbport_dlss.dll', 'nvngx_dlss.dll', 'bb-probe.exe', 'bb-gpu-capabilities.exe'}


def read_desc(path):
    """{'%NAME%': [lines]} of a pacman desc file."""
    out, key = {}, None
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        if line.startswith('%') and line.endswith('%'):
            key = line
            out[key] = []
        elif line.strip() and key:
            out[key].append(line.strip())
    return out


def msys2_dll_packages(bin_dir, msys2_root):
    """({package: {name, version, base, url, licenses, dlls}}, [dlls with no MSYS2 package]) for the DLLs in bin_dir,
    read from the pacman database of the MSYS2 install (var/lib/pacman/local/*/{desc,files})."""
    db = msys2_root / 'var' / 'lib' / 'pacman' / 'local'
    owner = {}  # lower-case dll file name -> db folder
    for folder in sorted(db.glob('*')) if db.is_dir() else []:
        files = folder / 'files'
        if not files.is_file():
            continue
        for line in files.read_text(encoding='utf-8', errors='replace').splitlines():
            low = line.lower()
            if low.endswith('.dll') and '/bin/' in low and low.split('/')[0] in ('clang64', 'mingw64'):
                owner.setdefault(low.rsplit('/', 1)[-1], folder)
    packages, unknown = {}, []
    for dll in sorted(bin_dir.glob('*.dll')):
        if dll.name.lower() in OWN_FILES:
            continue
        folder = owner.get(dll.name.lower())
        if folder is None:
            unknown.append(dll.name)
            continue
        desc = read_desc(folder / 'desc')
        name = desc['%NAME%'][0]
        entry = packages.setdefault(name, {'name': name, 'version': desc['%VERSION%'][0],
                                           'base': (desc.get('%BASE%') or [name])[0], 'url': (desc.get('%URL%') or [''])[0],
                                           'licenses': desc.get('%LICENSE%') or ['see the package'], 'dlls': []})
        entry['dlls'].append(dll.name)
    return packages, unknown


def short_name(package):
    for prefix in ('mingw-w64-clang-x86_64-', 'mingw-w64-x86_64-', 'mingw-w64-'):
        if package.startswith(prefix):
            return package[len(prefix):]
    return package


def write_third_party_sources(stage, packages, unknown, repo_slug, version):
    """SOURCES-THIRD-PARTY.txt: exact package versions of the bundled MSYS2 DLLs, source links, written offer."""
    lines = [
        f'Third-party binaries in bin/ of Bloodborne VR {version}: exact versions and where their source is',
        '=' * 78, '',
        'Every DLL in bin/ that is not ours was copied unmodified from the MSYS2 CLANG64 environment',
        '(https://www.msys2.org). Its source is the MSYS2 source package of the same name and version:',
        '  * recipes (PKGBUILD + patches): https://github.com/msys2/MINGW-packages/tree/master/<source package>',
        '  * source archives:              https://repo.msys2.org/mingw/sources/<source package>-<version>.src.tar.zst',
        '',
        'The FFmpeg libraries (avcodec, avformat, avutil, swresample, swscale) of that build are configured with',
        '--enable-gpl --enable-version3 and link GPL codec libraries (libx264, x265, xvidcore). They are therefore',
        'used here under the GNU GPL version 3: the licence text is COPYING.GPL-3.0 (also licenses/GPL-3.0.txt).',
        'LGPL libraries keep their LGPL (licenses/LGPL-3.0.txt, and the texts the packages ship in licenses/).',
        '', 'WRITTEN OFFER OF SOURCE CODE',
        '---------------------------',
        'For three years from the date of this release, and for as long as we distribute this version, we will',
        'give anyone who asks the complete corresponding source code of every GPL / LGPL component listed below',
        "(the MSYS2 source archives named here, and this project's own source tagged v" + version + '), on a',
        'medium customarily used for software interchange, for no more than the cost of doing so. Ask by opening an',
        f'issue at https://github.com/{repo_slug}/issues (title "Source request") and we will answer with the files or links.',
        'The same archives are always available from https://repo.msys2.org/mingw/sources/ (the MSYS2 project keeps them).',
        '', 'PACKAGES', '--------']
    for name in sorted(packages):
        e = packages[name]
        gpl = GPL_PACKAGES.get(short_name(name))
        lic = f'  licence:         {gpl or ", ".join(e["licenses"])}' + (f'  [package metadata: {", ".join(e["licenses"])}]' if gpl else '')
        lines += ['', f'{name} {e["version"]}',
                  f'  provides:        {", ".join(e["dlls"])}', lic,
                  f'  upstream:        {e["url"]}',
                  f'  MSYS2 source:    {e["base"]} {e["version"]}',
                  f'                   https://github.com/msys2/MINGW-packages/tree/master/{e["base"]}',
                  f'                   https://repo.msys2.org/mingw/sources/{e["base"]}-{e["version"]}.src.tar.zst']
    if unknown:
        lines += ['', 'DLLs not found in the MSYS2 package database (built or supplied separately; ask via the issue tracker):',
                  '  ' + ', '.join(unknown)]
    lines += ['', 'Other components', '----------------',
              '* bbport_xr.dll, bb-probe.exe, bb-gpu-capabilities.exe: this project (SOURCE.txt, THIRD_PARTY_NOTICES.md).',
              '* bbport_dlss.dll: this project (MIT, gpu/dlss_bridge in the source repository).',
              '* nvngx_dlss.dll: NVIDIA DLSS runtime, not open source; see licenses/NVIDIA-DLSS-LICENSE.txt.',
              '* python/: the embeddable Python from python.org (PSF licence, python/LICENSE.txt).', '']
    (stage / 'SOURCES-THIRD-PARTY.txt').write_text('\n'.join(lines), encoding='utf-8')


def fetch_python(source, cache):
    if source and Path(source).is_file():
        return Path(source)
    target = cache / 'python-embed-amd64.zip'
    if not target.is_file():
        url = source or PY_EMBED_URL
        print(f'downloading {url}')
        cache.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(url, target)
    return target


def add_python(stage, embed_zip):
    dest = stage / 'python'
    with zipfile.ZipFile(embed_zip) as z:
        z.extractall(dest)
    pth = next(dest.glob('python*._pth'))
    stdlib = next(dest.glob('python*.zip')).name
    # '..' and '../scripts' so run.py's and the scripts' sibling imports work; no site-packages needed.
    pth.write_text(f'{stdlib}\n.\n..\n../scripts\n../scripts/vr_assets\n', encoding='ascii')


def find_iscc(explicit=None):
    """Path of Inno Setup's command-line compiler, or None."""
    candidates = [explicit, os.environ.get('ISCC'), shutil.which('ISCC.exe') or shutil.which('iscc')]
    for base in (os.environ.get('LOCALAPPDATA'), os.environ.get('ProgramFiles(x86)'), os.environ.get('ProgramFiles')):
        if base:
            candidates += [Path(base) / 'Programs' / 'Inno Setup 6' / 'ISCC.exe', Path(base) / 'Inno Setup 6' / 'ISCC.exe']
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None


def build_installer(iscc, stage, version, out):
    """Runs Inno Setup on the staged folder; returns the Setup.exe path. Raises RuntimeError on failure."""
    script = REPO / 'packaging' / 'windows' / 'BloodborneVR.iss'
    setup = out / f'BloodborneVR-{version}-Setup.exe'
    setup.unlink(missing_ok=True)
    result = subprocess.run([str(iscc), '/Q', f'/DMyVersion={version}', '/DMyNumVersion=' + '.'.join(map(str, build_stub.file_version(version)[0])), f'/DStageDir={stage}', f'/DOutDir={out}',
                             f'/DRepoDir={REPO}', str(script)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode or not setup.is_file():
        raise RuntimeError(f'Inno Setup failed ({result.returncode}):\n{result.stdout[-2000:]}')
    return setup


def commit_id():
    import subprocess
    try:
        return subprocess.run(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], stdout=subprocess.PIPE,
                              text=True).stdout.strip()
    except OSError:
        return 'unknown'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--exe', type=Path, default=REPO / 'out' / 'gpu-public' / 'bb-probe.exe')
    ap.add_argument('--bridge', type=Path, default=REPO / 'out' / 'bbport_xr.dll')
    ap.add_argument('--dlss-dir', type=Path, default=REPO / 'out', help='folder with bbport_dlss.dll and nvngx_dlss.dll '
                    '(packaging/windows/build_dlss.sh); both are shipped (NVIDIA RTX SDKs LICENSE, licenses/NVIDIA-DLSS-*.txt)')
    ap.add_argument('--no-dlss', action='store_true', help='build a package without the DLSS DLLs (not for releases)')
    ap.add_argument('--caps', type=Path, default=REPO / 'out' / 'bb-gpu-capabilities.exe')
    ap.add_argument('--openxr', type=Path, default=REPO / 'out' / 'libopenxr_loader.dll')
    ap.add_argument('--export', type=Path, default=REPO / 'out' / 'public_export')
    ap.add_argument('--msys2', type=Path, default=Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64')
    ap.add_argument('--python-embed', help='embeddable Python zip (path or URL); default: python.org 3.12.10, cached')
    ap.add_argument('--ffmpeg-dir', type=Path, help='folder with a slim LGPL FFmpeg DLL set (avcodec-63.dll ...); '
                    'searched before the MSYS2 tree (whose FFmpeg is GPLv3 and drags in ~100 codec DLLs)')
    ap.add_argument('--no-python', action='store_true', help='do not bundle Python (testers need Python 3.10+)')
    ap.add_argument('--allow-online', action='store_true', help='skip the online-strings check of the host exe (never for a public build)')
    ap.add_argument('--version', default='0.1.0-alpha5')
    ap.add_argument('--no-installer', action='store_true', help='do not build Setup.exe (Inno Setup)')
    ap.add_argument('--iscc', type=Path, help='path of Inno Setup 6 ISCC.exe (default: $ISCC, PATH, standard folders)')
    ap.add_argument('--update-repo', help='GitHub owner/repo the update check asks (default: laughtt/bloodborne-vr)')
    ap.add_argument('--out', type=Path, default=REPO / 'out' / 'release')
    ap.add_argument('--stub', type=Path, help='prebuilt BloodborneVR.exe (default: build the native dashboard, tools/release/dashboard, with clang; '
                    'falls back to the tiny console stub, tools/release/zip/exe)')
    ap.add_argument('--strip', default=None, help='llvm-strip to drop debug info (default: clang64/bin/llvm-strip.exe if present)')
    args = ap.parse_args()

    dlss_dlls = [args.dlss_dir / n for n in DLSS_DLLS]
    for path in (args.exe, args.bridge, args.openxr, args.export / 'run.py') + (() if args.no_dlss else tuple(dlss_dlls)):
        if not path.is_file():
            sys.exit(f'missing input: {path}' + (' (run tools/release/export_public.py first)' if 'export' in str(path) else ''))
    name = f'BloodborneVR-{args.version}'
    stage = args.out / name
    if stage.exists():
        if args.out.resolve() == REPO.resolve():
            sys.exit('refusing to wipe the repo root')
        shutil.rmtree(stage)
    (stage / 'bin').mkdir(parents=True)
    (stage / 'licenses').mkdir()

    # The public alpha is single player: refuse a host exe that has the online / co-op code in it.
    blob = args.exe.read_bytes().lower()
    needles = (b'shad' + b'net', b'thehunters' + b'dream', b'scej-' + b'network', b'summon_' + b'messenger',
               b'bb_' + b'online', b'bb_debug_' + b'give', b'bb_net_' + b'trace', b'mini' + b'upnp')
    leaked = [m.decode() for m in needles if m in blob]
    if leaked and not args.allow_online:
        shutil.rmtree(stage)
        sys.exit(f'{args.exe} contains online / co-op strings {leaked}: build the public variant '
                 '(cmake -DBBPORT_ONLINE=OFF, out/gpu-public) or pass --allow-online')

    # Executables.
    bin_dir = stage / 'bin'
    strip = Path(args.strip) if args.strip else args.msys2 / 'bin' / 'llvm-strip.exe'
    shutil.copyfile(args.exe, bin_dir / 'bb-probe.exe')
    if strip.is_file():
        subprocess.run([str(strip), '--strip-debug', str(bin_dir / 'bb-probe.exe')], check=False)
    shutil.copyfile(args.bridge, bin_dir / 'bbport_xr.dll')
    shutil.copyfile(args.openxr, bin_dir / 'libopenxr_loader.dll')
    if not args.no_dlss:  # the exe loads both from its own folder (vk_dlss.cpp ExecutableDirectory)
        for dll in dlss_dlls:
            shutil.copyfile(dll, bin_dir / dll.name)
    if args.caps.is_file():
        shutil.copyfile(args.caps, bin_dir / 'bb-gpu-capabilities.exe')

    # Runtime DLLs: import closure, searched in the build folder first, then the MSYS2 tree.
    search = ([args.ffmpeg_dir] if args.ffmpeg_dir else []) + [args.exe.parent, args.msys2 / 'bin',
                                                              Path('C:/msys64/mingw64/bin')]
    roots = [p for p in bin_dir.iterdir() if p.suffix.lower() in ('.exe', '.dll')]
    found, missing = dll_closure(roots, search)
    share = args.msys2 / 'share' / 'licenses'
    for low, path in sorted(found.items()):
        if not (bin_dir / path.name).exists():
            shutil.copyfile(path, bin_dir / path.name)
    for dll in sorted(bin_dir.iterdir()):
        folder, lic = license_for(dll.name, share)
        if lic and not (stage / 'licenses' / f'{folder}-{lic.name}').exists():
            shutil.copyfile(lic, stage / 'licenses' / f'{folder}-{lic.name}')

    # Sources of the package from the export tree.
    ex = args.export
    for item in ('run.py', 'LICENSE', 'COPYING.GPL-3.0', 'COPYING.LGPL-3.0', 'THIRD_PARTY_NOTICES.md'):
        if (ex / item).is_file():
            shutil.copyfile(ex / item, stage / item)
    # The GPLv3 (the binaries are GPLv3) and LGPLv3 texts, then the licence texts the MSYS2 packages ship.
    for item, target in (('COPYING.GPL-3.0', 'GPL-3.0.txt'), ('COPYING.LGPL-3.0', 'LGPL-3.0.txt')):
        if not (stage / item).is_file():
            shutil.rmtree(stage)
            sys.exit(f'missing {item} in the export tree (tools/release/public): the GPL / LGPL texts must ship with the binaries')
        shutil.copyfile(stage / item, stage / 'licenses' / target)
    packages, unknown_dlls = msys2_dll_packages(bin_dir, args.msys2.parent)
    if not packages:
        print(f'NOTE: no MSYS2 package database under {args.msys2.parent}: SOURCES-THIRD-PARTY.txt lists no package versions')
    for package in sorted(packages):
        folder = share / short_name(package)
        if folder.is_dir():
            for lic in sorted(p for p in folder.rglob('*') if p.is_file()):
                target = stage / 'licenses' / f'{short_name(package)}-{lic.relative_to(folder).as_posix().replace("/", "_")}'
                if not target.exists():
                    shutil.copyfile(lic, target)
    write_third_party_sources(stage, packages, unknown_dlls, args.update_repo or 'laughtt/bloodborne-vr', args.version)
    shutil.copytree(ex / 'patches', stage / 'patches')
    shutil.copytree(ex / 'scripts', stage / 'scripts',
                    ignore=shutil.ignore_patterns('*.sh', '__pycache__', 'test_*.py', 'vr_frames.py'))
    stub = args.stub
    if not stub:
        cache = REPO / 'out' / 'release_cache'
        try:
            if not build_dashboard.available(args.msys2):
                raise RuntimeError('clang++ / llvm-rc / SDL3 headers not found')
            stub = build_dashboard.build(cache / 'BloodborneVR.exe', args.version, args.msys2)
        except RuntimeError as error:
            print(f'NOTE: native dashboard not built ({error}); using the console stub (web menu only)')
            try:
                stub = build_stub.build(cache / 'BloodborneVR.exe', args.version, args.msys2)
            except RuntimeError as error2:
                shutil.rmtree(stage)
                sys.exit(f'cannot build BloodborneVR.exe: {error2}')
    shutil.copyfile(stub, stage / 'BloodborneVR.exe')
    for item in ('BloodborneVR-console.bat', 'README.txt'):
        text = (ZIPSRC / item).read_text(encoding='utf-8')
        (stage / item).write_bytes(text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8'))
    controls = REPO / 'docs' / 'images' / 'controls.png'
    if controls.is_file():  # the labelled controller map README.txt points to
        shutil.copyfile(controls, stage / 'controls.png')
    (stage / 'launcher').mkdir()
    for module in ('bbvr_launcher.py', 'bbvr_menu.py', 'bbvr_settings.py', 'bbvr_cli.py', 'bbvr_update.py'):  # the start-up menu is standard library only
        shutil.copyfile(ZIPSRC / module, stage / 'launcher' / module)
    shutil.copytree(ZIPSRC / 'defaults', stage / 'defaults')
    if args.update_repo:
        release_json = stage / 'defaults' / 'release.json'
        data = json.loads(release_json.read_text(encoding='utf-8'))
        data['update_repo'] = args.update_repo
        release_json.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    for lic in (ex / 'gpu' / 'xr_bridge' / 'LICENSE.txt',):
        if lic.is_file():
            shutil.copyfile(lic, stage / 'licenses' / 'bbport_xr-LICENSE.txt')
    for lic in sorted((ex / 'packaging' / 'windows' / 'licenses').glob('*.txt')):
        shutil.copyfile(lic, stage / 'licenses' / lic.name)
    if not args.no_dlss:  # the bridge source (MIT) is in the public export
        shutil.copyfile(ex / 'gpu' / 'dlss_bridge' / 'LICENSE.txt', stage / 'licenses' / 'bbport_dlss-LICENSE.txt')
        for need in ('NVIDIA-DLSS-LICENSE.txt', 'NVIDIA-DLSS-NOTICE.txt'):
            if not (stage / 'licenses' / need).is_file():
                shutil.rmtree(stage)
                sys.exit(f'missing licenses/{need} in the export tree: required to ship nvngx_dlss.dll')
    (stage / 'SOURCE.txt').write_text(
        'Source code (GPL-2.0-or-later; this binary release is distributed under GPLv3, COPYING.GPL-3.0) for this\n'
        f'build is the public repository this release was made from, tag v{args.version}; the commit is in COMMIT.txt.\n'
        'Build instructions: README.md, build.sh. The DLSS bridge (bbport_dlss.dll, MIT) is in gpu/dlss_bridge; it is\n'
        "built against NVIDIA's DLSS SDK, which is not part of the repository (see THIRD_PARTY_NOTICES.md); nvngx_dlss.dll\n"
        "is NVIDIA's own binary. Exact versions, source links and a written offer of source for the bundled MSYS2\n"
        'libraries (FFmpeg, x264, x265, xvidcore and the rest of bin/): SOURCES-THIRD-PARTY.txt.\n', encoding='utf-8')
    (stage / 'COMMIT.txt').write_text(commit_id() + '\n', encoding='utf-8')
    (stage / 'VERSION.txt').write_text(args.version + '\n', encoding='utf-8')

    if not args.no_python:
        add_python(stage, fetch_python(args.python_embed, REPO / 'out' / 'release_cache'))

    # Zip (stored paths use forward slashes; deflate).
    archive = args.out / f'{name}.zip'
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(stage.rglob('*')):
            if path.is_file():
                z.write(path, f'{name}/{path.relative_to(stage).as_posix()}')
    setup = None
    if not args.no_installer:
        iscc = find_iscc(args.iscc)
        if not iscc:
            print('NOTE: Inno Setup 6 (ISCC.exe) not found: no Setup.exe (install it from https://jrsoftware.org, or pass --no-installer)')
        else:
            try:
                setup = build_installer(iscc, stage, args.version, args.out)
                print(f'installer: {setup}  ({setup.stat().st_size / 1e6:.1f} MB, {iscc})')
            except RuntimeError as error:
                print(f'ERROR: {error}')
                return 1
    files = [(archive, archive.name)] + ([(setup, setup.name)] if setup else []) + [
        (stage / 'BloodborneVR.exe', 'BloodborneVR.exe'), (bin_dir / 'bb-probe.exe', 'bin/bb-probe.exe')]
    sums = [(hashlib.sha256(path.read_bytes()).hexdigest(), n) for path, n in files]
    (args.out / 'SHA256SUMS.txt').write_text(''.join(f'{h} *{n}\n' for h, n in sums), encoding='ascii')
    print(f'BloodborneVR.exe: {(stage / "BloodborneVR.exe").stat().st_size / 1024:.0f} KB; sums: {args.out / "SHA256SUMS.txt"}')
    total = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
    dlls = sorted(p.name for p in bin_dir.iterdir())
    print(f'release folder: {stage}')
    print(f'zip: {archive}  ({archive.stat().st_size / 1e6:.1f} MB; unpacked {total / 1e6:.1f} MB)')
    print(f'bin/ ({len(dlls)} files): ' + ', '.join(dlls))
    if missing:
        print('NOT FOUND (system / optional?): ' + ', '.join(sorted(missing)))
    big = sorted(((p.stat().st_size, p.name) for p in bin_dir.iterdir()), reverse=True)[:8]
    print('largest: ' + ', '.join(f'{n} {s / 1e6:.1f} MB' for s, n in big))
    gpl3 = [n for n in dlls if n.lower().startswith(('avcodec', 'avformat', 'avutil', 'swscale', 'swresample'))]
    if gpl3 and not args.ffmpeg_dir:
        print('NOTE: FFmpeg DLLs are from the MSYS2 build configured --enable-gpl --enable-version3 (GPLv3): '
              'licenses/GPL-3.0.txt and SOURCES-THIRD-PARTY.txt (source offer) are in the package.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
