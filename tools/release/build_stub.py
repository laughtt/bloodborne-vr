#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds BloodborneVR.exe (tools/release/zip/exe) with the MSYS2 CLANG64 toolchain.

    python tools/release/build_stub.py [--version 0.1.0-alpha5] [--out out/release_cache/BloodborneVR.exe]
Exits non-zero with a message when clang or llvm-rc is missing or the build fails.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / 'tools' / 'release' / 'zip' / 'exe'


def find_tool(name, msys2):
    for ext in ('.exe', ''):
        if (msys2 / 'bin' / (name + ext)).is_file():
            return str(msys2 / 'bin' / (name + ext))
    return shutil.which(name)


def file_version(version):
    """'0.1.0-alpha3' -> ((0, 1, 0, 3), '0.1.0.3')."""
    nums = [int(n) for n in re.findall(r'\d+', version)][:4]
    nums += [0] * (4 - len(nums))
    return tuple(nums), '.'.join(map(str, nums))


def default_msys2():
    return Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64'


def available(msys2=None):
    msys2 = msys2 or default_msys2()
    return bool(find_tool('clang', msys2) and find_tool('llvm-rc', msys2))


def build(out, version='0.1.0', msys2=None):
    """Compiles the stub into `out`. Raises RuntimeError with the reason on failure."""
    msys2 = msys2 or default_msys2()
    clang, rc = find_tool('clang', msys2), find_tool('llvm-rc', msys2)
    if not clang or not rc:
        raise RuntimeError('clang / llvm-rc not found (MSYS2 CLANG64: pacman -S mingw-w64-clang-x86_64-clang)')
    nums, text = file_version(version)
    out = Path(out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        res = Path(tmp) / 'bbvr.res'
        env = dict(os.environ, PATH=str(msys2 / 'bin') + os.pathsep + os.environ.get('PATH', ''))
        steps = [
            [rc, '/I', str(msys2 / 'include'), f'-DBBVR_FILEVER={",".join(map(str, nums))}', f'-DBBVR_FILEVER_STR="{text}"',
             '/FO', str(res), str(SRC / 'bbvr.rc')],
            [clang, '--target=x86_64-w64-windows-gnu', '-municode', '-mwindows', '-Os', '-s', '-Wall',
             '-fno-ident', '-o', str(out), str(SRC / 'bbvr_stub.c'), str(res), '-lshell32', '-luser32'],
        ]
        for cmd in steps:
            result = subprocess.run(cmd, cwd=SRC, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if result.returncode:
                raise RuntimeError(f'{Path(cmd[0]).name} failed:\n{result.stdout}')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--version', default='0.1.0')
    ap.add_argument('--out', type=Path, default=REPO / 'out' / 'release_cache' / 'BloodborneVR.exe')
    ap.add_argument('--msys2', type=Path)
    args = ap.parse_args()
    try:
        path = build(args.out, args.version, args.msys2)
    except RuntimeError as error:
        sys.exit(str(error))
    print(f'{path} ({path.stat().st_size / 1024:.0f} KB)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
