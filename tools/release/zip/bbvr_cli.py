# SPDX-License-Identifier: GPL-2.0-or-later
"""Machine interface of the native dashboard (BloodborneVR.exe). The dashboard draws the window; this script
owns all the logic and files, so the dashboard and the web menu (bbvr_menu.py) read and write the SAME
settings (user/bbvr_settings.json, out/fp.txt, stereo.txt, iq.txt, ui_layouts) and the same environment is
built for the game. Every command prints one JSON document on stdout. Standard library only.

    bbvr_cli.py state                       settings, options, hardware, game folder, crash consent
    bbvr_cli.py save                        stdin: {"profile"?, "values"?, "show_menu"?}  -> state
    bbvr_cli.py game-dir PATH               remember the game folder (must contain eboot.bin) -> state
    bbvr_cli.py accept                      remember that the legal note was accepted -> state
    bbvr_cli.py update-check                ask GitHub for a newer release (quiet when unavailable) -> state
    bbvr_cli.py update-opt on|off           switch the start-up update check on / off -> state
    bbvr_cli.py print-env [PROFILE]         the game environment of a profile (no files written)
Starting the game is bbvr_launcher.py --no-menu (it reads the saved settings), not done here.
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bbvr_launcher as L  # noqa: E402
import bbvr_menu  # noqa: E402
import bbvr_settings as S  # noqa: E402
import bbvr_update  # noqa: E402

DLSS_FILES = ('nvngx_dlss.dll', 'bbport_dlss.dll')


def version():
    try:
        return (L.PORT / 'VERSION.txt').read_text(encoding='utf-8').strip()
    except OSError:
        return ''


def dlss_files(port):
    """{'nvngx_dlss.dll': bool, ...}: present in bin/ (release) or out/ (source tree)."""
    folders = [port / 'bin', port / 'out']
    return {n: any((f / n).is_file() for f in folders) for n in DLSS_FILES}


def make_menu(caps=None):
    caps = caps or S.detect_all(L.PORT)
    config = L.load_config()
    game = config.get('game_dir') or ''
    paths = S.make_paths(L.PORT, Path(game) if game else L.PORT, L.DATA)
    return bbvr_menu.Menu(L.settings_path(), paths, caps), config


def full_state(menu, config):
    st = menu.state()
    game = config.get('game_dir') or ''
    st['app'] = {
        'version': version(), 'port': str(L.PORT), 'game_dir': game,
        'eboot': bool(game) and (Path(game) / 'eboot.bin').is_file(),
        'accepted': bool(config.get('accepted')),
        'dlss_files': dlss_files(L.PORT), 'public': S.is_public(),
        'data_dir': str(L.DATA), 'installed': (L.PORT / S.INSTALLED_FLAG).is_file(),
    }
    defaults = S.release_defaults(L.PORT)
    st['update'] = bbvr_update.summary(config.get('update_check', True), version(), defaults, L.DATA)
    crash = st.get('crash') or {}
    if crash.get('dir'):
        try:  # so "Open crash reports folder" always has something to open
            Path(crash['dir']).mkdir(parents=True, exist_ok=True)
        except OSError:
            pass
    return st


def profile_env(profile, caps=None):
    """The environment build_launch makes for `profile` with the saved custom values (no files written)."""
    menu, config = make_menu(caps)
    settings = dict(menu.settings, profile=profile)
    values = S.effective_values(settings, menu.caps)
    plan = S.build_launch(values, menu.paths, menu.caps)
    return {'profile': profile, 'env': plan['env'], 'exe': str(plan['exe'] or ''), 'upscale': plan['upscale'],
            'warnings': plan['warnings']}


def run(argv, stdin_text=''):
    cmd = argv[0] if argv else 'state'
    if cmd == 'print-env':
        name = argv[1] if len(argv) > 1 else 'Default'
        wanted = next((p for p in S.PROFILE_ORDER if p.lower() == name.lower()), None)
        if not wanted:
            return {'error': 'unknown profile', 'profiles': S.PROFILE_ORDER}
        return profile_env(wanted)
    menu, config = make_menu()
    if cmd == 'save':
        body = json.loads(stdin_text or '{}')
        menu.update(body)
    elif cmd == 'game-dir':
        path = ' '.join(argv[1:]).strip().strip('"')
        if not path or not (Path(path) / 'eboot.bin').is_file():
            st = full_state(menu, config)
            st['error'] = 'No eboot.bin in that folder. Pick the folder that directly contains eboot.bin.'
            return st
        config['game_dir'] = str(Path(path))
        L.save_config(config)
    elif cmd == 'accept':
        config['accepted'] = True
        L.save_config(config)
    elif cmd == 'update-check':
        if config.get('update_check', True):
            bbvr_update.save_cache(L.DATA, bbvr_update.check(version(), S.release_defaults(L.PORT)))
    elif cmd == 'update-opt':
        config['update_check'] = (argv[1] if len(argv) > 1 else 'on') != 'off'
        L.save_config(config)
    elif cmd != 'state':
        return {'error': 'unknown command ' + cmd}
    return full_state(menu, config)


def main():
    stdin_text = sys.stdin.read() if (len(sys.argv) > 1 and sys.argv[1] == 'save') else ''
    out = run(sys.argv[1:], stdin_text)
    sys.stdout.buffer.write(json.dumps(out, default=str).encode('utf-8') + b'\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
