#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Clean-PC check of a built release: installs Setup.exe (or unzips the zip) into a temp folder and runs the
package with a sanitized environment, the way a PC without MSYS2 / Python / developer tools would. Standard library
only. Never starts the game and never touches the player's real data (LOCALAPPDATA is redirected to a temp folder).

    python tools/release/verify_install.py --installer out/release/BloodborneVR-<v>-Setup.exe
    python tools/release/verify_install.py --zip out/release/BloodborneVR-<v>.zip
    [--work DIR] [--report FILE] [--keep]

Checks (each prints PASS / FAIL): the dashboard renders (--screenshot, two tabs); bbvr_cli.py state through the
bundled Python; the launcher's dry run (settings, control files, environment) against a fake game folder; every DLL
imported by every exe / dll resolves to a file in bin/, next to the exe or in System32 (PE import tables, no
dependency walker needed); the bundled Python imports all launcher / script modules; the DLSS DLLs and NVIDIA licence
text are in the package, no user data shipped; where the player's files went (installed: %LOCALAPPDATA%\\BloodborneVR; zip: the folder itself);
the public build checks of online_checks.py on the installed files (the exe has the online client and the room bells,
no server code / address / test account / dev tool anywhere in the package, dev options hidden in the settings);
the uninstaller exists, removes the program and keeps the player's files. Exit code 0 when everything passed.
The report (text) goes next to the package unless --report says otherwise.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
REPO = HERE.parents[1]
SYSTEM32 = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'
# Loaded from the GPU driver / the OS, never shipped.
DRIVER_DLLS = {'vulkan-1.dll'}
KEEP_ENV = ('SystemRoot', 'windir', 'ComSpec', 'PATHEXT', 'OS', 'PROCESSOR_ARCHITECTURE', 'NUMBER_OF_PROCESSORS',
            'SystemDrive', 'ProgramData', 'ProgramFiles', 'ProgramFiles(x86)', 'ProgramW6432', 'USERNAME',
            'USERDOMAIN', 'COMPUTERNAME', 'ALLUSERSPROFILE', 'CommonProgramFiles', 'DriverData')


class Report:
    def __init__(self):
        self.lines, self.failed = [], 0

    def add(self, ok, name, detail=''):
        self.failed += 0 if ok else 1
        line = f'{"PASS" if ok else "FAIL"}  {name}' + (f'  -- {detail}' if detail else '')
        self.lines.append(line)
        print(line, flush=True)
        return ok

    def note(self, text):
        self.lines.append(text)
        print(text, flush=True)


def sanitized_env(app, fake_local):
    """PATH = System32 (+ wbem) and the install dir only; no BB_* / PYTHON* / MSYS2 variables."""
    keep = {k.lower() for k in KEEP_ENV}  # os.environ upper-cases names on Windows
    env = {k: v for k, v in os.environ.items() if k.lower() in keep}
    temp = fake_local / 'Temp'
    temp.mkdir(parents=True, exist_ok=True)
    home = fake_local.parent
    env.update(PATH=os.pathsep.join([str(SYSTEM32), str(SYSTEM32 / 'wbem'), str(app)]), LOCALAPPDATA=str(fake_local),
               APPDATA=str(home / 'Roaming'), USERPROFILE=str(home), HOMEPATH=str(home), TEMP=str(temp), TMP=str(temp))
    return env


def run(cmd, env, cwd, timeout=120, stdin=None):
    try:
        done = subprocess.run([str(c) for c in cmd], env=env, cwd=str(cwd), stdin=subprocess.DEVNULL if stdin is None else None,
                              input=stdin, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        return done.returncode, done.stdout.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired as error:
        return -9, f'timeout after {timeout}s\n' + (error.stdout or b'').decode('utf-8', 'replace')
    except OSError as error:
        return -1, str(error)


# ---------------------------------------------------------------------------------------------- PE imports

def pe_imports(path):
    """DLL names a PE file imports (import + delay-import tables); [] for non-PE. Pure Python."""
    import struct
    data = Path(path).read_bytes()
    if data[:2] != b'MZ' or len(data) < 0x40:
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
    for index, stride, name_at in ((1, 20, 12), (13, 32, 4)):
        rva, _ = struct.unpack_from('<II', data, dirs + index * 8)
        pos = offset(rva) if rva else None
        while pos is not None and pos + stride <= len(data):
            name_rva, = struct.unpack_from('<I', data, pos + name_at)
            at = offset(name_rva) if name_rva else None
            if at is None:
                break
            names.append(data[at:data.index(b'\0', at)].decode('ascii', 'replace'))
            pos += stride
    return names


def check_imports(app, rep):
    present = {p.name.lower() for p in (app / 'bin').iterdir()} | {p.name.lower() for p in app.glob('*.dll')}
    unresolved, driver, checked = {}, set(), 0
    pes = [p for folder in (app, app / 'bin') for p in folder.iterdir() if p.suffix.lower() in ('.exe', '.dll')]
    for pe in sorted(pes):
        checked += 1
        for name in pe_imports(pe):
            low = name.lower()
            if low.startswith(('api-ms-win-', 'ext-ms-')) or low in present or (SYSTEM32 / name).is_file():
                continue
            if low in DRIVER_DLLS:
                driver.add(low)
                continue
            unresolved.setdefault(pe.name, set()).add(name)
    detail = '; '.join(f'{k}: {", ".join(sorted(v))}' for k, v in unresolved.items())
    rep.add(not unresolved, f'DLL imports resolve ({checked} files)', detail)
    if driver:
        rep.note(f'      (driver-provided, not shipped: {", ".join(sorted(driver))})')


# ---------------------------------------------------------------------------------------------- checks

def check_online_client(app, rep, state):
    """The public package ships the online client and the room bells, and nothing of the server or the dev tools."""
    import online_checks
    exe = app / 'bin' / 'bb-probe.exe'
    if not exe.is_file():
        rep.add(False, 'online client check: bin\\bb-probe.exe missing')
        return
    errors, warnings, passes = online_checks.check_exe(exe.read_bytes(), 'online', True)
    wanted = {'online client': 'host exe has the online client (shadNet, Matching2, HTTP)',
              'room bells': 'host exe has the room bells',
              'dev tools': 'host exe has no dev tools (BB_DEBUG_GIVE / BB_DEBUG_USE / BB_NET_TRACE / BB_NET_BREAK)'}
    for key, label in wanted.items():
        key_errors = [e for e in errors if (e.startswith('online client missing') if key == 'online client' else
                                            e.startswith('room bells missing') if key == 'room bells' else e.startswith('dev tool'))]
        rep.add(not key_errors, label, '; '.join(key_errors))
    rest = [e for e in errors if not e.startswith(('online client missing', 'room bells missing', 'dev tool'))]
    rep.add(not rest, 'host exe holds no server code, server address, test account or hard-coded IP / port', '; '.join(rest))
    for text in warnings:
        rep.note(f'      (note: {text})')
    errors, warnings, passes = online_checks.check_tree(app, 'online', True)
    rep.add(not errors, 'package files: no server files, addresses or test accounts', '; '.join(errors[:4]))
    if state is not None:
        rep.add(state.get('public') is True, 'dev options hidden in the settings (public build)')


def check_png(path):
    try:
        data = Path(path).read_bytes()
    except OSError:
        return False
    return data[:8] == b'\x89PNG\r\n\x1a\n' and len(data) > 8000


def check_package(app, mode, env, work, fake_local, rep):
    exe = app / 'BloodborneVR.exe'
    python = app / 'python' / 'python.exe'
    rep.add(exe.is_file(), 'BloodborneVR.exe present')
    rep.add(python.is_file(), 'bundled python\\python.exe present')
    rep.add((app / 'bin' / 'bb-probe.exe').is_file() and (app / 'bin' / 'bbport_xr.dll').is_file(), 'bin\\bb-probe.exe and bbport_xr.dll present')
    dlss = [n for n in ('nvngx_dlss.dll', 'bbport_dlss.dll') if (app / 'bin' / n).is_file()]
    rep.add(len(dlss) == 2, 'bin\\nvngx_dlss.dll and bbport_dlss.dll present (DLSS out of the box)', ', '.join(dlss))
    rep.add((app / 'licenses' / 'NVIDIA-DLSS-LICENSE.txt').is_file() and (app / 'licenses' / 'NVIDIA-DLSS-NOTICE.txt').is_file(),
            'NVIDIA DLSS licence + notice in licenses\\')
    rep.add(not any((app / d).exists() for d in ('out', 'user', 'logs')), 'no out\\ user\\ logs\\ in the package yet')
    flag = (app / 'installed.flag').is_file()
    rep.add(flag == (mode == 'installer'), f'installed.flag {"present" if flag else "absent"} ({mode})')

    check_imports(app, rep)

    # Dashboard renders (hidden window, one frame, PNG written).
    for tab in ('play', 'about'):
        shot = work / f'dashboard-{tab}.png'
        shot.unlink(missing_ok=True)
        code, out = run([exe, '--screenshot', shot, '--tab', tab], env, app, timeout=90)
        rep.add(code == 0 and check_png(shot), f'dashboard --screenshot ({tab})',
                f'exit {code}, {shot.stat().st_size if shot.is_file() else 0} bytes' + ('' if code == 0 else ' ' + out[-300:]))

    # Settings helper through the bundled Python.
    code, out = run([python, app / 'launcher' / 'bbvr_cli.py', 'state'], env, app, timeout=60)
    state = None
    try:
        state = json.loads(out[out.index('{'):])
    except ValueError:
        pass
    rep.add(code == 0 and bool(state) and 'options' in state, 'bbvr_cli.py state (bundled python)', '' if state else out[-300:])
    if state:
        info = state.get('app', {})
        want = fake_local / 'BloodborneVR' if mode == 'installer' else app
        rep.add(info.get('installed') == (mode == 'installer') and Path(info.get('data_dir', '?')) == want,
                'player data folder', f'data_dir={info.get("data_dir")} (expected {want})')
        rep.add(info.get('version', '') != '', 'version known', info.get('version', ''))
        rep.add(state.get('update', {}).get('status') in ('unchecked', 'disabled'), 'update check state present')
        crash = state.get('crash', {})
        rep.add(Path(crash.get('dir', '?')) == fake_local / 'BloodborneVR' / 'crashes' and crash.get('count') == 0,
                'crash reports folder is %LOCALAPPDATA%\\BloodborneVR\\crashes', crash.get('dir', ''))
    check_online_client(app, rep, state)
    # The update check must be quiet whatever the network says (repo private / offline).
    code, out = run([python, app / 'launcher' / 'bbvr_cli.py', 'update-check'], env, app, timeout=60)
    try:
        status = json.loads(out[out.index('{'):]).get('update', {}).get('status')
    except ValueError:
        status = None
    rep.add(code == 0 and status in ('newer', 'current', 'unavailable'), 'update-check does not fail', f'status={status}')

    # Launcher dry run against a fake game folder: settings, control files and the game environment, no game.
    game = work / 'fake-game'
    game.mkdir(exist_ok=True)
    (game / 'eboot.bin').write_bytes(b'\0')
    code, out = run([python, app / 'launcher' / 'bbvr_launcher.py', '--dry-run', '--skip-checks', '--no-menu', '--game-dir', game],
                    env, app, timeout=90)
    rep.add(code == 0 and 'BB_GAME_DIR=' in out and 'BB_PROBE=' in out, 'launcher dry run (no game started)', f'exit {code}' + ('' if code == 0 else ' ' + out[-300:]))
    # The start flow: Play solo has no online variables; the helper answers the remembered start choice (no network).
    code, out = run([python, app / 'launcher' / 'bbvr_launcher.py', '--dry-run', '--skip-checks', '--no-menu', '--solo', '--game-dir', game],
                    env, app, timeout=90)
    rep.add(code == 0 and 'BB_ONLINE' not in out, 'Play solo: launcher dry run has no online variables', f'exit {code}' + ('' if code == 0 else ' ' + out[-300:]))
    code, out = run([python, app / 'launcher' / 'bbvr_cli.py', 'online', 'mode', 'solo'], env, app, timeout=60)
    rep.add(code == 0 and '"play_mode": "solo"' in out, 'start flow helper (online mode) answers', out[-200:] if code else '')
    where = (fake_local / 'BloodborneVR') if mode == 'installer' else app
    rep.add((where / 'out' / 'fp.txt').is_file(), f'control files written under {where}')
    if mode == 'installer':
        rep.add(not (app / 'out').exists() and not (app / 'user').exists(), 'nothing written into the install folder')

    # The bundled interpreter imports the launcher and the scripts it runs.
    code_src = ("import sys; sys.path[:0] = ['launcher', 'scripts']; "
                "import bbvr_launcher, bbvr_menu, bbvr_settings, bbvr_cli, bbvr_update, bbvr_online, crash_report, patches, mods, prepare; "
                "import run; print('imports ok')")
    code, out = run([python, '-c', code_src], env, app, timeout=60)
    rep.add(code == 0 and 'imports ok' in out, 'bundled python imports launcher + scripts', out[-300:] if code else '')
    code, out = run([python, '-c', 'import ssl, sqlite3, ctypes, json, urllib.request; print("stdlib ok")'], env, app, timeout=60)
    rep.add(code == 0 and 'stdlib ok' in out, 'bundled python stdlib (ssl, ctypes, urllib)', out[-200:] if code else '')


# ---------------------------------------------------------------------------------------------- install / unzip

def install_setup(setup, target, work, rep):
    log = work / 'setup.log'
    code, out = run([setup, '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', '/NOICONS', f'/DIR={target}', f'/LOG={log}'],
                    dict(os.environ), work, timeout=600)
    ok = code == 0 and (target / 'BloodborneVR.exe').is_file()
    rep.add(ok, 'Setup.exe /VERYSILENT installs', f'exit {code}, {setup.stat().st_size / 1e6:.1f} MB' + ('' if ok else ' ' + out[-300:]))
    return ok


def uninstall(app, fake_local, work, rep):
    unins = sorted(app.glob('unins*.exe'))
    rep.add(bool(unins), 'uninstaller (unins000.exe) exists')
    if not unins:
        return
    data = fake_local / 'BloodborneVR'
    data.mkdir(parents=True, exist_ok=True)
    marker = data / 'user' / 'save-marker.txt'
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text('keep me', encoding='utf-8')
    run([unins[0], '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'], dict(os.environ), work, timeout=300)
    deadline = time.time() + 120
    while time.time() < deadline and (app / 'BloodborneVR.exe').exists():
        time.sleep(1)
    time.sleep(2)
    rep.add(not (app / 'BloodborneVR.exe').exists() and not (app / 'bin').exists(), 'uninstall removes the program files')
    rep.add(marker.is_file() and (data / 'out' / 'fp.txt').is_file(), 'uninstall keeps saves, settings and generated data')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--installer', type=Path)
    ap.add_argument('--zip', type=Path)
    ap.add_argument('--work', type=Path, help='temp folder to use (default: a new one)')
    ap.add_argument('--report', type=Path)
    ap.add_argument('--keep', action='store_true', help='keep the work folder')
    args = ap.parse_args()
    if bool(args.installer) == bool(args.zip):
        ap.error('give exactly one of --installer / --zip')
    source = (args.installer or args.zip).resolve()
    if not source.is_file():
        sys.exit(f'missing: {source}')
    mode = 'installer' if args.installer else 'zip'
    work = (args.work or Path(tempfile.mkdtemp(prefix='bbvr-verify-'))).resolve()
    shutil.rmtree(work, ignore_errors=True) if args.work else None
    work.mkdir(parents=True, exist_ok=True)
    fake_local = work / 'home' / 'AppData' / 'Local'
    fake_local.mkdir(parents=True)
    rep = Report()
    rep.note(f'verify_install: {mode} {source.name} ({source.stat().st_size / 1e6:.1f} MB), work folder {work}')
    rep.note(f'sanitized PATH: System32 + install dir; LOCALAPPDATA={fake_local}')
    app = work / 'app'
    try:
        if mode == 'installer':
            ok = install_setup(source, app, work, rep)
        else:
            with zipfile.ZipFile(source) as z:
                z.extractall(work / 'unzipped')
            roots = [p for p in (work / 'unzipped').iterdir() if p.is_dir()]
            app = roots[0] if len(roots) == 1 else work / 'unzipped'
            ok = (app / 'BloodborneVR.exe').is_file()
            rep.add(ok, 'zip unpacks to one folder with BloodborneVR.exe')
        if ok:
            check_package(app, mode, sanitized_env(app, fake_local), work, fake_local, rep)
            if mode == 'installer':
                uninstall(app, fake_local, work, rep)
    finally:
        rep.note(f'RESULT: {"ALL PASSED" if not rep.failed else str(rep.failed) + " FAILED"}')
        report = args.report or (source.parent / f'verify-{source.stem}.txt')
        report.write_text('\n'.join(rep.lines) + '\n', encoding='utf-8')
        print(f'report: {report}')
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    return 1 if rep.failed else 0


if __name__ == '__main__':
    sys.exit(main())
