# SPDX-License-Identifier: GPL-2.0-or-later
"""Machine interface of the native dashboard (BloodborneVR.exe). The dashboard draws the window; this script
owns all the logic and files, so the dashboard and the web menu (bbvr_menu.py) read and write the SAME
settings (user/bbvr_settings.json, out/fp.txt, stereo.txt, iq.txt, ui_layouts) and the same environment is
built for the game. Every command prints one JSON document on stdout. Standard library only.

    bbvr_cli.py state                       settings, options, hardware, game folder, crash consent
    bbvr_cli.py save                        stdin: {"profile"?, "values"?, "show_menu"?}  -> state
    bbvr_cli.py game-dir PATH               remember the game folder (must contain eboot.bin) -> state
    bbvr_cli.py alpha-ack                   remember that the early-alpha notice was read (for this version)
    bbvr_cli.py accept                      remember that the legal note was accepted -> state
    bbvr_cli.py update-check                ask GitHub for a newer release (quiet when unavailable) -> state
    bbvr_cli.py update-opt on|off           switch the start-up update check on / off -> state
    bbvr_cli.py print-env [PROFILE]         the game environment of a profile (no files written)
    bbvr_cli.py online server [URL|default] show / set the lobby address (or env BBVR_LOBBY_URL); default = the release's own lobby
    bbvr_cli.py online create [NAME]        open a room, prints its join code
    bbvr_cli.py online join CODE [NAME]     join a room by code
    bbvr_cli.py online leave                leave the room (a host leaving closes it)
    bbvr_cli.py online status               current room and members (refreshes user/online_room.json)
    bbvr_cli.py online mode solo|host|join  remember the start choice (dashboard: Play solo / Host a game / Join a friend)
    bbvr_cli.py online view                 room + members + auto-bell state in one answer (the dashboard's Online tab)
Starting the game is bbvr_launcher.py --no-menu (it reads the saved settings), not done here.
"""
import json
import os
import re
import secrets
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bbvr_launcher as L  # noqa: E402
import bbvr_online  # noqa: E402
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
        'alpha_notice': L.alpha_notice_due(config, version()), 'alpha_text': L.ALPHA_NOTICE,
        'dlss_files': dlss_files(L.PORT), 'public': S.is_public(),
        'online_play': S.ONLINE_PLAY, 'play_mode': play_mode(config), 'online_step': config.get('online_step', 'host'),
        'lobby': lobby_info(config), 'data_dir': str(L.DATA), 'installed': (L.PORT / S.INSTALLED_FLAG).is_file(),
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


# --- online rooms: a plain REST client against a configurable lobby address ----------------------------------------

def _user_dir():
    return Path(L.DATA) / 'user'


_read_json = bbvr_online.read_json
_write_json = bbvr_online.write_json
_lobby_call = bbvr_online.lobby_call


def lobby_url(config=None):
    url = os.environ.get('BBVR_LOBBY_URL') or (config if config is not None else L.load_config()).get('online_server', '')
    return str(url or S.default_lobby(L.PORT) or '').strip().rstrip('/')


def lobby_info(config=None):
    """{'server', 'is_default'}: the lobby in use and whether it is the one that comes with the release."""
    url = lobby_url(config)
    return {'server': url, 'is_default': bool(url) and url == S.default_lobby(L.PORT)}


def online_identity(name=None):
    """Account name for this install, created on first use: <Name>_<4 random>, kept in user/online_identity.json."""
    path = _user_dir() / 'online_identity.json'
    ident = _read_json(path)
    if not re.fullmatch(r'[A-Za-z0-9_-]{3,16}', str(ident.get('npid', ''))):
        base = re.sub(r'[^A-Za-z0-9]', '', name or os.environ.get('USERNAME') or os.environ.get('USER') or 'Hunter')[:10] or 'Hunter'
        base = base.ljust(3, 'x')
        ident = {'npid': base + '_' + ''.join(secrets.choice('23456789ABCDEFGHJKMNPQRSTUVWXYZ') for _ in range(4))}
        _write_json(path, ident)
    return ident


def _lobby_call(base, action, fields, timeout=8):
    url = f'{base}/bbvr/v1/room/{action}'
    req = urllib.request.Request(url, data=json.dumps(fields).encode(), method='POST',
                                 headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode('utf-8'))
    except urllib.error.HTTPError as err:
        try:
            return json.loads(err.read().decode('utf-8'))
        except ValueError:
            return {'ok': False, 'error': 'http_%d' % err.code, 'message': 'The lobby answered an error.'}
    except (OSError, ValueError) as err:
        return {'ok': False, 'error': 'unreachable', 'message': 'Cannot reach the lobby: %s' % (getattr(err, 'reason', err),)}


def online_view(base, saved, fresh_s=12):
    """Everything the dashboard's Online tab shows, from the files; one status call first when the room file is
    older than `fresh_s` seconds (no game is running a heartbeat)."""
    import time
    room_file = _user_dir() / 'online_room.json'
    if saved.get('token') and time.time() - float(saved.get('ts') or 0) > fresh_s:
        bbvr_online.refresh(_user_dir(), room_file)
        saved = _read_json(room_file)
    bell = _read_json(_user_dir() / 'online_bell_status.json')
    if bell and time.time() - float(bell.get('ts') or 0) > 60:
        bell = {}                                   # a leftover of an earlier game
    ident = _read_json(_user_dir() / 'online_identity.json')
    in_room = bool(saved.get('code') and saved.get('role') and not saved.get('closed'))
    out = {'ok': True, 'server': base, 'is_default': base == S.default_lobby(L.PORT), 'npid': ident.get('npid', ''), 'in_room': in_room,
           'code': saved.get('code', ''), 'role': saved.get('role', ''), 'closed': bool(saved.get('closed')),
           'reason': saved.get('reason', ''), 'members': saved.get('members') or [], 'max': saved.get('max', 3),
           'age_s': int(time.time() - float(saved.get('ts') or 0)) if saved.get('ts') else -1, 'bell': bell}
    if in_room and out['members']:
        for m in out['members']:
            if m.get('role') == 'host':
                m['state'] = 'host'
            else:
                m['state'] = 'in world' if m.get('summoned') else 'sign up' if m.get('sign') else 'waiting'
    return out


def play_mode(config):
    return 'online' if config.get('play_mode') == 'online' else 'solo'


def online(argv):
    """online create|join|leave|status|server -> one JSON-able dict ({ok, ...}). The saved room (code, role,
    token) is user/online_room.json; the lobby address is online_server in bbvr_config.json or BBVR_LOBBY_URL."""
    sub = argv[0] if argv else 'status'
    config = L.load_config()
    if sub == 'server':
        if len(argv) > 1:
            if argv[1].strip().lower() == 'default':   # back to the lobby that comes with the release
                config.pop('online_server', None)
            else:
                config['online_server'] = argv[1].strip().rstrip('/')
            L.save_config(config)
        return dict(ok=True, **lobby_info(config))
    if sub == 'mode':  # remember the start choice: mode solo | host | join
        what = argv[1] if len(argv) > 1 else ''
        if what in ('solo', 'host', 'join'):
            config['play_mode'] = 'solo' if what == 'solo' else 'online'
            if what != 'solo':
                config['online_step'] = what
            L.save_config(config)
        return {'ok': True, 'play_mode': play_mode(config), 'online_step': config.get('online_step', 'host')}
    base = lobby_url(config)
    if not base:
        return {'ok': False, 'error': 'no_server', 'message': 'No lobby set. Run: online server URL'}
    room_file = _user_dir() / 'online_room.json'
    saved = _read_json(room_file)
    if sub in ('create', 'join'):
        rest = argv[1:]
        if sub == 'join':
            if not rest:
                return {'ok': False, 'error': 'bad_code', 'message': 'Usage: online join CODE [NAME]'}
            code, rest = rest[0], rest[1:]
        name = ' '.join(rest).strip() or None
        ident = online_identity(name)
        fields = {'npid': ident['npid'], 'name': name or ident['npid']}
        if sub == 'join':
            fields['code'] = code
        out = _lobby_call(base, sub, fields)
        if out.get('ok'):
            saved = {'code': out['code'], 'role': out['role'], 'token': out['token'], 'server': base}
            bbvr_online.apply(room_file, saved, out, ident['npid'])
        return out
    if sub == 'view':
        return online_view(base, saved)
    if sub in ('leave', 'status'):
        ident = online_identity()
        if not saved.get('token') or saved.get('server') != base:
            room_file.unlink(missing_ok=True)       # nothing to leave; a closed-room leftover goes too
            return {'ok': True, 'in_room': False}
        out = _lobby_call(base, sub, {'npid': ident['npid'], 'token': saved['token']})
        if sub == 'leave' and (out.get('ok') or out.get('error') == 'not_in_room'):
            room_file.unlink(missing_ok=True)
            return {'ok': True, 'left': True}
        if sub == 'status' and out.get('ok'):
            if out.get('in_room'):
                bbvr_online.apply(room_file, saved, out, ident['npid'])      # keep the game's view fresh too
            else:
                room_file.unlink(missing_ok=True)       # room closed or we were dropped: forget it
        return out
    return {'ok': False, 'error': 'unknown_command', 'message': 'online create|join|leave|status|server'}


def run(argv, stdin_text=''):
    cmd = argv[0] if argv else 'state'
    if cmd == 'online':
        return online(argv[1:])
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
    elif cmd == 'alpha-ack':  # OK on the early-alpha notice: not shown again until the version changes
        config['alpha_notice_seen'] = version()
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
