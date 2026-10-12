#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bloodborne VR (alpha) first-run helper and launcher. Standard library only.

    BloodborneVR.exe                 check everything, then start the game in VR
    BloodborneVR.exe --check         only run the checks
    BloodborneVR.exe --mono          VR render mode Mono: one flat screen in the headset (the default is Stereo, true 3D)
    BloodborneVR.exe --mode vr|coop|flat   this start's mode: VR solo, Co-op (online, needs a room) or Flat (monitor, no headset)
    BloodborneVR.exe --flat          play on the monitor (with --mode coop: co-op played flat)
    BloodborneVR.exe --quality Low|Medium|High|Super High   this start's VR quality preset
    BloodborneVR.exe --game-dir DIR  use (and remember) another game folder
    BloodborneVR.exe --reset         forget the saved game folder and the extracted VR data
    BloodborneVR.exe --menu          show the settings menu (default until you tick "don't show next time")
    BloodborneVR.exe --no-menu       start with the saved settings, no menu
    BloodborneVR.exe --profile NAME  start with a profile (Default, Comfort, Performance, "Quality (DLSS)", Custom)
    BloodborneVR.exe --reset-settings  forget the saved profile and options (and the UI overrides)

What it does, in order:
  1. finds your own legally dumped game folder (CUSA03173) and remembers it in bbvr_config.json;
  2. checks the version (param.sfo) and the executable: its SHA-256 against the known 01.09 dump,
     else every byte the VR hooks expect is compared with the game's code (unknown builds stop here);
  3. checks that an OpenXR runtime is set up;
  4. builds the VR menu / hit-shape data from YOUR dump into out/vr_assets (stays on this PC);
  5. writes default fp.txt / stereo.txt if missing, shows the settings menu (bbvr_menu.py, a local web page; the
     native dashboard of BloodborneVR.exe is the same window) and starts run.py with the chosen settings
     (bbvr_settings.py): the saved mode (VR / Co-op / Flat), VR quality preset and edits.
No game file is ever copied into this folder except those derived data, and nothing is uploaded.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))  # the embeddable Python does not put the script's folder on sys.path
import bbvr_menu  # noqa: E402
import bbvr_settings as settings_mod  # noqa: E402

# Source tree (tools/release/zip): the port is the repo root and the game data lives in out/.
DEV = HERE.name.lower() == 'zip' and (HERE.parents[2] / 'run.py').is_file()
PORT = HERE.parents[2] if DEV else (HERE.parent if HERE.name.lower() == 'launcher' else HERE)
# The player's own files (out/, user/, logs/, bbvr_config.json): the port folder for the portable zip and the
# source tree, %LOCALAPPDATA%/BloodborneVR for a Setup.exe install (installed.flag), see bbvr_settings.data_root.
DATA = settings_mod.data_root(PORT)
OUT = DATA / 'out'
CONFIG = (OUT / 'bbvr_config.json') if DEV else (DATA / 'bbvr_config.json')
LOGS = DATA / 'logs'
TITLE_ID = settings_mod.GAME_TITLE_ID
APP_VERSION = settings_mod.GAME_APP_VERSION
# SHA-256 of eboot.bin of the 01.09 dump the patches were written against (a hash, not game content).
KNOWN_EBOOT_SHA256 = {
    '22ccba99c1182247124b9cfa4c95472d2f2eea442eb61586b5ddec5b4e7cbe02': 'CUSA03173 1.09',
}
EBOOT_BASE = 0x400000
DISCLAIMER = """\
Bloodborne VR is an unofficial fan project. It contains no game files: you must own Bloodborne
and use your own legally obtained dump. It is not affiliated with, endorsed or sponsored by Sony
Interactive Entertainment, FromSoftware or any trademark holder. PlayStation, PS4 and Bloodborne
are trademarks of their owners.
"""
ALPHA_NOTICE = ('EARLY ALPHA: many bugs, crashes are possible, please be patient. Crash reports stay on this PC '
                '(they are never uploaded); see README.txt to report a bug.')
HOOKS = ';'.join(settings_mod.HOOKS_BASE)
PATCHES = ';'.join(name for name, oid, *_ in settings_mod.PATCHES if settings_mod.BY_ID[oid]['default'])


def say(text=''):
    print(text, flush=True)


def app_version():
    try:
        return (PORT / 'VERSION.txt').read_text(encoding='utf-8').strip()
    except OSError:
        return ''


def alpha_notice_due(config, version=None):
    """True until the player pressed OK on the early-alpha notice for this version (config key alpha_notice_seen)."""
    return config.get('alpha_notice_seen') != (app_version() if version is None else version)


def ask(prompt, default=None):
    try:
        answer = input(prompt).strip()
    except EOFError:
        answer = ''
    return answer or default


def yes(prompt, default=False):
    answer = ask(prompt + (' [Y/n] ' if default else ' [y/N] '), 'y' if default else 'n')
    return answer.lower().startswith('y')


def load_config():
    try:
        return json.loads(CONFIG.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def save_config(config):
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text(json.dumps(config, indent=2) + '\n', encoding='utf-8')


# --- game folder -------------------------------------------------------------------------------

def pick_folder():
    """A folder dialog through Windows PowerShell; None when it is unavailable or cancelled."""
    script = ("Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.FolderBrowserDialog; "
              "$d.Description = 'Select your Bloodborne dump folder (the one that contains eboot.bin)'; "
              "if ($d.ShowDialog() -eq 'OK') { $d.SelectedPath }")
    try:
        result = subprocess.run(['powershell', '-NoProfile', '-STA', '-Command', script], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, timeout=300)
        return result.stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def find_game_dir(config, requested):
    candidates = [requested, os.environ.get('BB_GAME_DIR'), config.get('game_dir')]
    for candidate in candidates:
        if candidate and (Path(candidate) / 'eboot.bin').is_file():
            return Path(candidate)
    say('Step 1: your game folder. Choose the folder of YOUR OWN dump of Bloodborne (CUSA03173) that')
    say('contains eboot.bin and the dvdroot_ps4 folder. No game files are copied.')
    for _ in range(3):
        chosen = pick_folder() or ask('Game folder path: ')
        if chosen and (Path(chosen) / 'eboot.bin').is_file():
            return Path(chosen)
        say(f'  No eboot.bin in "{chosen}". Pick the folder that directly contains eboot.bin.')
    return None


def read_sfo(path):
    """Minimal param.sfo reader: {key: value} of the string / integer entries (bbvr_settings.read_sfo)."""
    return settings_mod.read_sfo(path)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 22), b''):
            digest.update(block)
    return digest.hexdigest()


def elf_hook_mismatches(elf, xml_path, names):
    """Compares the bytes each named hook expects (patches/Bloodborne.xml, Expect=) with the game code."""
    import xml.etree.ElementTree as ET
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
    segments = []
    for i in range(phnum):
        kind, _, offset, vaddr, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + i * phentsize)
        if kind == 1:
            segments.append((vaddr, vaddr + memsz, offset))
    bad, checked = [], 0
    for meta in ET.parse(xml_path).getroot().iter('Metadata'):
        if meta.get('Name') not in names or meta.get('AppVer') != APP_VERSION:
            continue
        for line in meta.iter('Line'):
            expect = (line.get('Expect') or '').replace(' ', '')
            if not expect:
                continue
            address = int(line.get('Address'), 0) - EBOOT_BASE
            want = bytes.fromhex(expect)
            for start, end, offset in segments:
                if start <= address < end:
                    checked += 1
                    if elf[offset + address - start:offset + address - start + len(want)] != want:
                        bad.append(f'{meta.get("Name")} @ {line.get("Address")}')
                    break
            else:
                bad.append(f'{meta.get("Name")} @ {line.get("Address")} (outside the executable)')
    return bad, checked


def check_game(game, config):
    """Returns True when the game folder is usable for the VR hooks."""
    sfo_path = game / 'sce_sys' / 'param.sfo'
    try:
        sfo = read_sfo(sfo_path)
    except (OSError, ValueError, struct.error):
        say(f'  Cannot read {sfo_path}. This does not look like a decrypted CUSA03173 dump.')
        return False
    title, version = sfo.get('TITLE_ID'), sfo.get('APP_VER')
    say(f'  Title {title}, version {version}')
    if title != TITLE_ID:
        say(f'  This is {title}, not {TITLE_ID} (the European / US disc release the patches target).')
        return False
    if version != APP_VERSION:
        say(f'  Version {version}: the VR patches need {APP_VERSION}. Update the game to 1.09 first.')
        return False
    digest = sha256(game / 'eboot.bin')
    if digest in KNOWN_EBOOT_SHA256:
        say(f'  eboot.bin matches the known dump ({KNOWN_EBOOT_SHA256[digest]}).')
        config['eboot_sha256_checked'] = digest
        return True
    say('  eboot.bin differs from the known 01.09 dump (another dump or decryption tool).')
    say('  Checking that every byte the VR hooks expect is where it should be ...')
    OUT.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([sys.executable, str(PORT / 'scripts' / 'prepare.py'), str(game), '--out', str(OUT)],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    elf = OUT / 'eboot.elf'
    if result.returncode or not elf.is_file():
        say('  Could not read the executable: ' + result.stderr.strip()[-300:])
        return False
    bad, checked = elf_hook_mismatches(elf.read_bytes(), PORT / 'patches' / 'Bloodborne.xml', HOOKS.split(';'))
    if bad or not checked:
        say(f'  {len(bad)} of {checked} hook checks failed (first: {bad[:1]}). This build is not supported.')
        return False
    say(f'  All {checked} expected byte runs match: this executable is compatible. (SHA-256 {digest[:16]}...)')
    config['eboot_sha256_checked'] = digest
    return True


# --- OpenXR / Vulkan ---------------------------------------------------------------------------

def check_openxr():
    runtime = os.environ.get('XR_RUNTIME_JSON')
    if runtime and Path(runtime).is_file():
        say(f'  OpenXR runtime from XR_RUNTIME_JSON: {runtime}')
        return True
    try:
        import winreg
        for view in (winreg.KEY_WOW64_64KEY, 0):
            try:
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\Khronos\OpenXR\1', 0, winreg.KEY_READ | view) as key:
                    path = winreg.QueryValueEx(key, 'ActiveRuntime')[0]
                    if Path(path).is_file():
                        say(f'  Active OpenXR runtime: {path}')
                        return True
            except OSError:
                continue
    except ImportError:
        pass
    say('  No active OpenXR runtime found. Install and start your headset software, then make it the')
    say('  active OpenXR runtime (SteamVR: Settings > OpenXR > Set SteamVR as OpenXR runtime; Oculus app:')
    say('  Settings > General > OpenXR Runtime; Pimax Play / Windows Mixed Reality have the same switch).')
    return False


def check_vulkan():
    try:
        ctypes.WinDLL('vulkan-1.dll')
        return True
    except OSError:
        say('  vulkan-1.dll not found: update your graphics driver (Vulkan 1.3 is required).')
        return False


# --- VR data from the user's own dump ------------------------------------------------------------

def extract_assets(game, config):
    marker = OUT / 'vr_assets' / 'ui' / 'icons.json'
    if marker.is_file() and config.get('assets_for') == str(game):
        return
    say('Step 4: building the VR menu and hit-shape data from your own game folder (a few minutes,')
    say('once). This stays on your PC in out\\vr_assets and is never uploaded or shared.')
    assets = OUT / 'vr_assets'
    assets.mkdir(parents=True, exist_ok=True)
    jobs = [('menu icons and item names', 'ui_extract.py', ['--game', str(game), '--out', str(assets), '--quiet']),
            ('hit shapes', 'hitbox_extract.py', ['--game', str(game), '--out', str(assets / 'hitbox')]),
            ('dodge data', 'dodge_extract.py', ['--game', str(game), '--out', str(assets / 'dash')])]
    failed = False
    for label, script, extra in jobs:
        say(f'  - {label} ...')
        started = time.time()
        result = subprocess.run([sys.executable, str(PORT / 'scripts' / 'vr_assets' / script), *extra],
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            failed = True
            say(f'    failed ({result.returncode}): {result.stdout.strip()[-200:]}')
        else:
            say(f'    done in {time.time() - started:.0f} s')
    if failed:
        say('  Some data could not be built; the game still runs, but menus or hit shapes may be limited.')
    else:
        config['assets_for'] = str(game)


# --- start ------------------------------------------------------------------------------------

def ensure_defaults():
    OUT.mkdir(parents=True, exist_ok=True)
    for name in ('fp.txt', 'stereo.txt'):
        target, source = OUT / name, (PORT / 'defaults' if (PORT / 'defaults').is_dir() else HERE / 'defaults') / name
        if not target.exists() and source.is_file():
            shutil.copyfile(source, target)


def launch(env_extra, menu=None):
    """Starts run.py with the plan's environment. With a menu, its server keeps running while the game does."""
    env = dict(os.environ)
    env.update({'PYTHONIOENCODING': 'utf-8'})
    env.update(env_extra)
    if DATA != PORT:  # installed build: generated files, saves and settings are outside the program folder
        env.setdefault('BB_DATA_DIR', str(DATA))
        env.setdefault('BB_PATCHES_DIR', str(PORT / 'patches'))  # run.py defaults these to <data>/patches
    LOGS.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    log = LOGS / f'bbvr-{stamp}.log'
    for old in sorted(LOGS.glob('bbvr-*.log'))[:-9]:
        old.unlink(missing_ok=True)
    say(f'Starting. Put the headset on. Log: {log}')
    say(ALPHA_NOTICE)
    if menu:
        menu.game_running = True
        menu.log_path = log
    heartbeat = None
    if env.get('BB_ONLINE_ROOM_FILE'):  # in a room: keep the room file (members, sign, summoned) fresh while the game runs
        try:
            import bbvr_online
            heartbeat = bbvr_online.start_thread(DATA / 'user', log=say, room_file=env['BB_ONLINE_ROOM_FILE'])[1]
        except Exception as err:  # the game must start without it
            say(f'  Online: room heartbeat not started ({err})')
    # The game's output goes to the console and to the log (a crash report is saved by run.py, docs/CRASH_REPORTS.md).
    with open(log, 'w', encoding='utf-8', errors='replace') as handle:
        handle.write(ALPHA_NOTICE + '\n')
        child = subprocess.Popen([sys.executable, str(PORT / 'run.py')], cwd=PORT, env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if menu:
            menu.child = child  # the menu's Stop button ends it
        while True:
            chunk = child.stdout.read1(65536)  # returns what is there, so a prompt without a newline shows
            if not chunk:
                break
            text = chunk.decode('utf-8', 'replace')
            handle.write(text)
            handle.flush()
            # The console may not be UTF-8 (cp1252): never let a character it can't show stop the game.
            enc = getattr(sys.stdout, 'encoding', None) or 'utf-8'
            sys.stdout.write(text.encode(enc, 'replace').decode(enc, 'replace'))
            sys.stdout.flush()
        code = child.wait()
    if heartbeat:
        heartbeat.set()
    if menu:
        menu.game_running = False
    shutil.copyfile(log, LOGS / 'bbvr-latest.log')
    if code:
        say(f'\nThe game ended with exit code {code}. Please attach logs\\bbvr-latest.log to a bug report.')
    return code


def settings_path():
    return DATA / 'user' / 'bbvr_settings.json'


def online_bridge(argv):
    """The room commands of bbvr_cli (host / join / leave / view / server) for the web menu. Imported on use: bbvr_cli
    itself imports this module."""
    import bbvr_cli
    return bbvr_cli.online(list(argv))


def wants_flat(args):
    """True when this start is Flat (--flat, --mode flat, or the saved mode) and shows no menu that could change it."""
    if args.flat or args.mode == 'flat':
        return True
    if args.menu or not (args.no_menu or args.profile):
        return False  # the menu may still pick VR
    return settings_mod.play_view(settings_mod.load_settings(settings_path())) == 'flat'


def run_with_settings(game, args):
    """The settings step: profile / menu, control files, environment; then the game."""
    caps = settings_mod.detect_all(PORT)
    spath = settings_path()
    paths = settings_mod.make_paths(PORT, game, DATA)
    if args.reset_settings:
        spath.unlink(missing_ok=True)
        settings_mod.apply_ui_overrides(paths['assets'], {})  # back to the pristine ui_layouts.json
        say('Saved profile and options removed.')
    saved = settings_mod.load_settings(spath)
    show = args.menu or (saved.get('show_menu', True) and not args.no_menu and not args.profile)
    menu = None
    if show:
        menu = bbvr_menu.Menu(spath, paths, caps, saved)
        menu.version = app_version()
        menu.log_dir = LOGS
        if settings_mod.ONLINE_PLAY:
            menu.online = online_bridge  # the Online co-op page: host / join / leave a room
        url = menu.start()
        opened = '' if (args.console or args.no_browser) else bbvr_menu.open_page(url)
        if opened:
            say(f'Settings menu opened ({opened}). Choose a profile or options, then "Start game".')
            say(f'If no window appeared, open: {url}')
            while not menu.launch_event.wait(0.5):
                pass
            if menu.quit_event.is_set():
                menu.stop()
                return 0
            saved = menu.settings
        elif args.no_browser:
            say(f'Settings menu: {url}')
            say('Choose in the page and press "Start game" (or press Enter here to start with the saved settings).')
            waiter = threading.Thread(target=lambda: (sys.stdin.readline(), menu.launch_event.set()), daemon=True)
            waiter.start()
            while not menu.launch_event.wait(0.5):
                pass
            if menu.quit_event.is_set():
                menu.stop()
                return 0
            saved = menu.settings
        else:
            if not args.console:
                say('No browser could be opened; using the text menu.')
            menu.stop()
            menu = None
            saved = bbvr_menu.console_menu(spath, caps, say)
            if saved is None:
                return 0
    if args.profile:
        wanted = next((p for p in settings_mod.PROFILE_ORDER if p.lower() == args.profile.lower()), None)
        if not wanted:
            say(f'Unknown profile "{args.profile}". Profiles: ' + ', '.join(settings_mod.PROFILE_ORDER))
            return 5
        saved = dict(saved, profile=wanted)
    if getattr(args, 'mode', None):
        saved = dict(saved, mode=args.mode)
    if getattr(args, 'quality', None):
        wanted = next((q for q in settings_mod.QUALITY_ORDER if q.lower() == args.quality.lower()), None)
        if not wanted:
            say(f'Unknown quality "{args.quality}". Presets: ' + ', '.join(settings_mod.QUALITY_ORDER))
            return 5
        saved = settings_mod.set_quality(saved, wanted)
    if getattr(args, 'flat', False):
        saved = dict(saved, mode='flat' if saved.get('mode') != 'coop' else 'coop', coop_view='flat')
    view = settings_mod.play_view(saved)
    values = settings_mod.effective_values(saved, caps)
    if args.mono:
        values['mode'] = 'mono'
    plan = settings_mod.build_launch(values, paths, caps, view=view)
    for warning in plan['warnings']:
        say('  Note: ' + warning)
    if view == 'vr':
        wr = settings_mod.runtime_warning(caps['runtimes'], values.get('runtime') or '')
        if wr:
            say('  Warning: ' + wr)
    OUT.mkdir(parents=True, exist_ok=True)
    # Only the Co-op mode (or --online-room flows of older callers: none) plays online; VR and Flat are solo.
    solo = getattr(args, 'solo', False) or saved.get('mode', 'vr') != 'coop'
    online = settings_mod.online_env(values, load_config(), DATA / 'user', solo=solo, port=PORT)
    if solo:
        say('  Play solo: online is off.')
    elif not online:
        say('  Co-op: no room to play in (host a room or join one first); starting solo.')
    if online:
        plan['env'].update(online)
        lobby = settings_mod.lobby_address(values, load_config(), DATA / 'user', PORT)
        if lobby and 'SHADPS4_HTTP_HOST_OVERRIDES_JSON' not in plan['env']:
            import bbvr_online
            hosts = bbvr_online.sync_host_overrides(lobby, DATA / 'user', say)
            if hosts:
                plan['env']['SHADPS4_HTTP_HOST_OVERRIDES_JSON'] = str(hosts)
        say('  Online: ' + (f'room {online["BB_ONLINE_ROOM"]} as {online["BB_ONLINE_ROLE"]}' if 'BB_ONLINE_ROOM' in online else 'on'))
    changed = settings_mod.apply_files(plan, paths)
    if changed:
        say('  Updated: ' + ', '.join(Path(c).name for c in changed))
    if view == 'flat':
        say(f'  Flat screen ({settings_mod.MODE_LABELS.get(saved.get("mode", "vr"))}): no headset, executable {plan["exe"] or "default"}')
    else:
        say(f'  {settings_mod.MODE_LABELS.get(saved.get("mode", "vr"))}, quality {saved.get("quality", settings_mod.QUALITY_DEFAULT)}, '
            f'profile {saved["profile"]}: upscaler {plan["upscale"]}, render mode {values.get("mode")}, '
            f'executable {plan["exe"] or "default"}')
    if args.dry_run:
        for k, v in sorted(plan['env'].items()):
            say(f'{k}={v}')
        if menu:
            menu.stop()
        return 0
    try:
        return launch(plan['env'], menu)
    finally:
        if menu:
            menu.stop()


def main():
    ap = argparse.ArgumentParser(description='Bloodborne VR alpha launcher')
    ap.add_argument('--check', action='store_true', help='run the checks only')
    ap.add_argument('--solo', action='store_true', help='play solo: online off even when a room is saved (the dashboard Play solo button)')
    ap.add_argument('--mono', action='store_true', help='VR render mode Mono: one flat screen in the headset (no stereo)')
    ap.add_argument('--mode', choices=settings_mod.MODES, help='play mode for this start: vr, coop (online) or flat (monitor, no headset)')
    ap.add_argument('--flat', action='store_true', help='play on the monitor, no headset (Co-op stays Co-op, played flat)')
    ap.add_argument('--quality', help='VR quality preset for this start: ' + ', '.join(settings_mod.QUALITY_ORDER))
    ap.add_argument('--game-dir', help='game folder (remembered)')
    ap.add_argument('--reset', action='store_true', help='forget the game folder and the extracted VR data')
    ap.add_argument('--menu', action='store_true', help='show the settings menu even if it was switched off')
    ap.add_argument('--no-menu', action='store_true', help='start with the saved settings, no menu')
    ap.add_argument('--profile', help='start with this profile (no menu): Default, Comfort, Performance, "Quality (DLSS)", Custom')
    ap.add_argument('--reset-settings', action='store_true', help='forget the saved profile and options')
    ap.add_argument('--console', action='store_true', help='text menu instead of the web page')
    ap.add_argument('--no-browser', action='store_true', help='do not open a browser; print the menu address')
    ap.add_argument('--skip-checks', action='store_true',
                    help='source tree: skip the game checks and the VR data build (the game folder comes from '
                         'BB_GAME_DIR or the saved one)')
    ap.add_argument('--dry-run', action='store_true', help='write the control files, print the environment, do not start')
    args = ap.parse_args()
    os.chdir(PORT)
    config = load_config()
    if args.reset:
        config = {}
        shutil.rmtree(OUT / 'vr_assets', ignore_errors=True)
        save_config(config)
        say('Saved settings and extracted VR data removed.')
    if args.skip_checks:
        game = find_game_dir(config, args.game_dir)
        if not game:
            say('No game folder (set BB_GAME_DIR or pass --game-dir).')
            return 1
        ensure_defaults()
        return run_with_settings(game, args)
    if not config.get('accepted'):
        say(DISCLAIMER)
        if not yes('I own a legal copy of Bloodborne and accept the above. Continue?'):
            return 1
        config['accepted'] = True
    game = find_game_dir(config, args.game_dir)
    if not game:
        say('No game folder. Run again and pick the folder that contains eboot.bin.')
        return 1
    config['game_dir'] = str(game)
    say(f'Step 2: checking the game in {game}')
    ok_game = check_game(game, config)
    save_config(config)
    say('Step 3: checking graphics and VR runtime')
    ok_vulkan = check_vulkan()
    ok_xr = check_openxr()
    if not ok_game:
        say('\nThe game folder is not usable. Fix it and run again.')
        return 2
    if not ok_vulkan:
        return 3
    if not ok_xr and not wants_flat(args) and not yes('Continue without an OpenXR runtime (the game will not show in a headset)?'):
        return 4
    if args.check:
        say('\nAll checks done.')
        return 0
    if wants_flat(args):
        say('Flat mode: the VR menu and hit-shape data are not needed (they are built when you start in VR).')
    else:
        extract_assets(game, config)
    save_config(config)
    ensure_defaults()
    return run_with_settings(game, args)


def finish(code):
    """BloodborneVR.exe gives the launcher its own console window, which closes with the process: when
    something went wrong, or only checks ran, keep it open until Enter so the text can be read."""
    if os.environ.get('BBVR_EXE') == '1' and (code or '--check' in sys.argv or '--dry-run' in sys.argv):
        try:
            input('\nPress Enter to close this window.')
        except (EOFError, OSError, KeyboardInterrupt):
            pass
    sys.exit(code)


if __name__ == '__main__':
    try:
        finish(main())
    except KeyboardInterrupt:
        sys.exit(130)
