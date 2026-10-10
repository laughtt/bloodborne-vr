#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds BloodborneVR.exe, the native dashboard (tools/release/dashboard/main.cpp: Dear ImGui + SDL3 renderer,
gpu/third_party/imgui) with the MSYS2 CLANG64 toolchain. SDL3 and the C++ runtime are linked
statically: one self-contained exe (--copy-dlls also copies SDL3.dll, only needed for a dynamic build).

    python tools/release/build_dashboard.py [--version 0.1.0-alpha3] [--out out/launcher/BloodborneVR.exe] [--copy-dlls]
Object files are cached in out/launcher/obj (only changed sources are rebuilt). C++ runtime is linked statically.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_stub  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / 'tools' / 'release' / 'dashboard'
IMGUI = REPO / 'gpu' / 'third_party' / 'imgui'
RC_DIR = REPO / 'tools' / 'release' / 'zip' / 'exe'
SOURCES = [SRC / 'main.cpp', IMGUI / 'imgui.cpp', IMGUI / 'imgui_draw.cpp', IMGUI / 'imgui_tables.cpp',
           IMGUI / 'imgui_widgets.cpp', IMGUI / 'backends' / 'imgui_impl_sdl3.cpp',
           IMGUI / 'backends' / 'imgui_impl_sdlrenderer3.cpp']


def available(msys2=None):
    msys2 = msys2 or build_stub.default_msys2()
    return bool(build_stub.find_tool('clang++', msys2) and build_stub.find_tool('llvm-rc', msys2)
                and (IMGUI / 'imgui.cpp').is_file() and (msys2 / 'include' / 'SDL3').is_dir())


def build(out, version='0.1.0', msys2=None, copy_dlls=False):
    msys2 = msys2 or build_stub.default_msys2()
    cxx, rc = build_stub.find_tool('clang++', msys2), build_stub.find_tool('llvm-rc', msys2)
    if not cxx or not rc:
        raise RuntimeError('clang++ / llvm-rc not found (MSYS2 CLANG64: pacman -S mingw-w64-clang-x86_64-clang)')
    out = Path(out).resolve()
    obj = out.parent / 'obj'
    obj.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PATH=str(msys2 / 'bin') + os.pathsep + os.environ.get('PATH', ''))
    nums, text = build_stub.file_version(version)
    flags = ['--target=x86_64-w64-windows-gnu', '-std=c++17', '-O2', '-DNDEBUG', '-Wall', '-Wno-unused-function',
             f'-DBBVR_VERSION="{version}"', f'-I{SRC}', f'-I{IMGUI}', f'-I{IMGUI / "backends"}', f'-I{msys2 / "include"}',
             '-DIMGUI_DISABLE_DEBUG_TOOLS', '-DIMGUI_DISABLE_DEMO_WINDOWS' if False else '-DIMGUI_USE_WCHAR32=0']
    objects = []
    for src in SOURCES:
        o = obj / (src.stem + '.o')
        objects.append(o)
        if o.is_file() and o.stat().st_mtime > max(src.stat().st_mtime, (IMGUI / 'imgui.h').stat().st_mtime) \
                and (src != SRC / 'main.cpp' or True) and o.stat().st_mtime > Path(__file__).stat().st_mtime:
            continue
        r = subprocess.run([cxx, *flags, '-c', str(src), '-o', str(o)], env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        if r.returncode:
            raise RuntimeError(f'{src.name} failed:\n{r.stdout}')
    res = obj / 'bbvr.res'
    r = subprocess.run([rc, '/I', str(msys2 / 'include'), f'-DBBVR_FILEVER={",".join(map(str, nums))}',
                        f'-DBBVR_FILEVER_STR="{text}"', '/FO', str(res), str(RC_DIR / 'bbvr.rc')], cwd=RC_DIR, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise RuntimeError(f'llvm-rc failed:\n{r.stdout}')
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.stem + '.new.exe')
    r = subprocess.run([cxx, '--target=x86_64-w64-windows-gnu', '-mwindows', '-municode', '-s', '-static', '-o', str(tmp),
                        *map(str, objects), str(res), f'-L{msys2 / "lib"}', '-lSDL3', '-lm', '-liconv', '-lkernel32',
                        '-luser32', '-lgdi32', '-lwinmm', '-limm32', '-lole32', '-loleaut32', '-lversion', '-luuid',
                        '-ladvapi32', '-lsetupapi', '-lcfgmgr32', '-lshell32', '-ldinput8'], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise RuntimeError(f'link failed:\n{r.stdout}')
    os.replace(tmp, out)
    if copy_dlls:
        shutil.copyfile(msys2 / 'bin' / 'SDL3.dll', out.parent / 'SDL3.dll')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--version', default='0.1.0')
    ap.add_argument('--out', type=Path, default=REPO / 'out' / 'launcher' / 'BloodborneVR.exe')
    ap.add_argument('--msys2', type=Path)
    ap.add_argument('--copy-dlls', action='store_true')
    args = ap.parse_args()
    try:
        path = build(args.out, args.version, args.msys2, args.copy_dlls)
    except RuntimeError as error:
        sys.exit(str(error))
    print(f'{path} ({path.stat().st_size / 1024:.0f} KB)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
