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
    BloodborneVR.exe (tiny launcher stub, built here), BloodborneVR-console.bat (fallback), README.txt, LICENSE, THIRD_PARTY_NOTICES.md, SOURCE.txt
    launcher/{bbvr_launcher,bbvr_menu,bbvr_settings}.py (start-up settings menu), defaults/{fp,stereo}.txt,
    python/ (embeddable Python, optional)
    run.py, scripts/, patches/, bin/ (exes + DLLs), licenses/
No game file, extracted asset or DLSS / FSR 4 runtime is included.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import urllib.request
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_stub  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
ZIPSRC = REPO / 'tools' / 'release' / 'zip'
PY_EMBED_URL = 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip'
SYSTEM_DIRS = [Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32']
# DLLs left to the system / the GPU driver (never shipped even when the MSYS2 tree has a copy).
SYSTEM_DLLS = {'vulkan-1.dll'}
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
    ap.add_argument('--caps', type=Path, default=REPO / 'out' / 'bb-gpu-capabilities.exe')
    ap.add_argument('--openxr', type=Path, default=REPO / 'out' / 'libopenxr_loader.dll')
    ap.add_argument('--export', type=Path, default=REPO / 'out' / 'public_export')
    ap.add_argument('--msys2', type=Path, default=Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64')
    ap.add_argument('--python-embed', help='embeddable Python zip (path or URL); default: python.org 3.12.10, cached')
    ap.add_argument('--ffmpeg-dir', type=Path, help='folder with a slim LGPL FFmpeg DLL set (avcodec-63.dll ...); '
                    'searched before the MSYS2 tree (whose FFmpeg is GPLv3 and drags in ~100 codec DLLs)')
    ap.add_argument('--no-python', action='store_true', help='do not bundle Python (testers need Python 3.10+)')
    ap.add_argument('--allow-online', action='store_true', help='skip the online-strings check of the host exe (never for a public build)')
    ap.add_argument('--version', default='0.1.0-alpha1')
    ap.add_argument('--out', type=Path, default=REPO / 'out' / 'release')
    ap.add_argument('--stub', type=Path, help='prebuilt BloodborneVR.exe (default: build tools/release/zip/exe with clang)')
    ap.add_argument('--strip', default=None, help='llvm-strip to drop debug info (default: clang64/bin/llvm-strip.exe if present)')
    args = ap.parse_args()

    for path in (args.exe, args.bridge, args.openxr, args.export / 'run.py'):
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
        import subprocess
        subprocess.run([str(strip), '--strip-debug', str(bin_dir / 'bb-probe.exe')], check=False)
    shutil.copyfile(args.bridge, bin_dir / 'bbport_xr.dll')
    shutil.copyfile(args.openxr, bin_dir / 'libopenxr_loader.dll')
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
    for item in ('run.py', 'LICENSE', 'COPYING.GPL-3.0', 'THIRD_PARTY_NOTICES.md'):
        if (ex / item).is_file():
            shutil.copyfile(ex / item, stage / item)
    shutil.copytree(ex / 'patches', stage / 'patches')
    shutil.copytree(ex / 'scripts', stage / 'scripts',
                    ignore=shutil.ignore_patterns('*.sh', '__pycache__', 'test_*.py', 'vr_frames.py'))
    stub = args.stub
    if not stub:
        try:
            stub = build_stub.build(REPO / 'out' / 'release_cache' / 'BloodborneVR.exe', args.version, args.msys2)
        except RuntimeError as error:
            shutil.rmtree(stage)
            sys.exit(f'cannot build BloodborneVR.exe: {error}')
    shutil.copyfile(stub, stage / 'BloodborneVR.exe')
    for item in ('BloodborneVR-console.bat', 'README.txt'):
        text = (ZIPSRC / item).read_text(encoding='utf-8')
        (stage / item).write_bytes(text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8'))
    (stage / 'launcher').mkdir()
    for module in ('bbvr_launcher.py', 'bbvr_menu.py', 'bbvr_settings.py'):  # the start-up menu is standard library only
        shutil.copyfile(ZIPSRC / module, stage / 'launcher' / module)
    shutil.copytree(ZIPSRC / 'defaults', stage / 'defaults')
    for lic in (ex / 'gpu' / 'xr_bridge' / 'LICENSE.txt',):
        if lic.is_file():
            shutil.copyfile(lic, stage / 'licenses' / 'bbport_xr-LICENSE.txt')
    for lic in sorted((ex / 'packaging' / 'windows' / 'licenses').glob('*.txt')):
        shutil.copyfile(lic, stage / 'licenses' / lic.name)
    (stage / 'SOURCE.txt').write_text(
        'Source code (GPL-2.0-or-later) for this build is the public repository this release was made\n'
        f'from, tag v{args.version}; the commit is in COMMIT.txt. Build instructions: README.md, build.sh.\n'
        'Sources of the bundled third-party DLLs: see THIRD_PARTY_NOTICES.md.\n', encoding='utf-8')
    (stage / 'COMMIT.txt').write_text(commit_id() + '\n', encoding='utf-8')

    if not args.no_python:
        add_python(stage, fetch_python(args.python_embed, REPO / 'out' / 'release_cache'))

    # Zip (stored paths use forward slashes; deflate).
    archive = args.out / f'{name}.zip'
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(stage.rglob('*')):
            if path.is_file():
                z.write(path, f'{name}/{path.relative_to(stage).as_posix()}')
    sums = [(hashlib.sha256(path.read_bytes()).hexdigest(), n) for path, n in
            ((archive, archive.name), (stage / 'BloodborneVR.exe', 'BloodborneVR.exe'), (bin_dir / 'bb-probe.exe', 'bin/bb-probe.exe'))]
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
        print('NOTE: FFmpeg DLLs are from the MSYS2 build configured --enable-gpl --enable-version3 (GPLv3).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
