# SPDX-License-Identifier: GPL-2.0-or-later
"""Room heartbeat: keeps user/online_room.json in step with the room server while the game runs. Standard library only.

The game cannot ask the room server anything by itself. Its auto-bell (src/runtime_online_bell.c) reads the room
file instead, so this helper asks the server every few seconds how the room is doing (members, and for each member
whether a summon sign is up and whether it is summoned into the host's world) and writes the answer into that file.
Each call also counts as the player's activity, so the server does not drop a member who is playing.

The launcher runs `watch()` in a thread next to the game when the player is in a room. The same file can be run by hand:

    bbvr_online.py watch [--user-dir DIR] [--interval 5] [--pid N]   until the room closes, the game (pid) exits or Ctrl+C
    bbvr_online.py once  [--user-dir DIR]                            one refresh, prints the room file
    bbvr_online.py env   [--user-dir DIR]                            KEY=VALUE lines to start a game in the room by hand

The room file (user/online_room.json) holds what `bbvr_cli.py online create|join` saved (code, role, token, server) plus
what this helper adds: members [{name, role, sign, summoned, idle, you, world?}] (world: the server's "host" = a phantom in the host's world,
"own" = in its own world; left out when the server does not know), summoned_guests (host's view), self_sign,
self_summoned, closed (+ reason), ts (unix seconds of the last answer). When the room is gone the token is dropped
and closed is true.

Each status call also tells the server which world the player is in: `world` is "host" while the game is a phantom
in another player's world (np_room 2 in the game's online_bell_status.json, from the emulator's Matching2 room
role) and "own" otherwise; no field when that file is missing or older than 20 s.
"""
import json
import os
import re
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

INTERVAL = 5.0
CALL_TIMEOUT = 6


def read_json(path):
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def write_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(json.dumps(data, indent=2) + "\n", encoding='utf-8')
    os.replace(tmp, path)


def lobby_call(base, action, fields, timeout=8):
    """POST one room action; always returns a dict ({ok: False, error: ...} when the lobby cannot be reached)."""
    req = urllib.request.Request(f'{base}/bbvr/v1/room/{action}', data=json.dumps(fields).encode(), method='POST',
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


def room_file_path(user_dir=None):
    return Path(os.environ.get('BB_ONLINE_ROOM_FILE') or (Path(user_dir) / 'online_room.json'))


def summarise(view, my_npid):
    """The server's status answer -> the fields the game reads."""
    members = []
    for m in view.get('members') or []:
        item = {'name': str(m.get('name') or m.get('npid') or '?'), 'role': m.get('role'),
                'sign': bool(m.get('sign')), 'summoned': bool(m.get('summoned')),
                'idle': int(m.get('idle') or 0), 'you': m.get('npid') == my_npid}
        if m.get('world') in ('host', 'own'):
            item['world'] = m['world']
        members.append(item)
    me = next((m for m in members if m['you']), None)
    return {'members': members,
            'summoned_guests': sum(1 for m in members if m['role'] != 'host' and m['summoned']),
            'self_sign': bool(me and me['sign']), 'self_summoned': bool(me and me['summoned']),
            'max': view.get('max', 3), 'closed': False}


def apply(room_file, saved, out, npid, now=time.time):
    """Write the room file from a status-like answer (`out`: status, or create / join). `saved` has the token and the
    server. Returns the dict written."""
    keep = {k: saved[k] for k in ('code', 'role', 'server') if k in saved}
    if not out.get('in_room', True):
        data = dict(keep, closed=True, reason=(out.get('reason') or (out.get('notice') or {}).get('reason') or 'left'),
                    members=[], ts=int(now()))
    else:
        data = dict(keep, token=saved['token'], ts=int(now()), **summarise(out, npid))
        data['code'] = out.get('code', data.get('code'))
        data['role'] = out.get('role') or data.get('role')
    write_json(room_file, data)
    return data


STATUS_MAX_AGE = 20.0


def world_field(user_dir, now=time.time):
    """'host' when the game says it is a member of another player's Matching2 room (np_room 2), 'own' for np_room 0/1,
    None when the game's status file (BB_ONLINE_STATUS_FILE, else <user_dir>/online_bell_status.json) is missing,
    broken, older than 20 s or has no np_room >= 0. Never raises."""
    try:
        path = os.environ.get('BB_ONLINE_STATUS_FILE') or str(Path(user_dir) / 'online_bell_status.json')
        st = read_json(path)
        room = st.get('np_room')
        if isinstance(room, bool) or not isinstance(room, int) or room < 0:
            return None
        ts = st.get('ts')
        if isinstance(ts, bool) or not isinstance(ts, (int, float)) or abs(now() - ts) > STATUS_MAX_AGE:
            return None
        return 'host' if room == 2 else 'own'
    except Exception:
        return None


def refresh(user_dir, room_file=None, now=time.time):
    """Ask the server once and rewrite the room file. Returns the dict written, or None when nothing was written
    (no room, no server, lobby unreachable: the old file stays and ages out)."""
    room_file = Path(room_file) if room_file else room_file_path(user_dir)
    saved = read_json(room_file)
    if not saved.get('token') or not saved.get('server'):
        return None
    npid = read_json(Path(user_dir) / 'online_identity.json').get('npid')
    if not npid:
        return None
    fields = {'npid': npid, 'token': saved['token']}
    world = world_field(user_dir, now)
    if world:
        fields['world'] = world
    out = lobby_call(saved['server'], 'status', fields, timeout=CALL_TIMEOUT)
    if not out.get('ok'):
        return None
    return apply(room_file, saved, out, npid, now)


# ---- game-server redirects from the lobby ------------------------------------------------------------------------

HOSTS_MAX_BYTES = 8192
HOSTS_MAX_ENTRIES = 32
HOSTS_FILE = 'host_overrides.lobby.json'
_HOSTPORT = re.compile(r'(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*(?::\d{1,5})?')


def _valid_endpoint(text, scheme_required, allow_path):
    """[scheme://]host[:port][/path]: letters, digits, dots, dashes (or an IPv4/IPv6 free host name), no credentials,
    no query. `scheme_required`: the text must start with http:// or https://."""
    if not isinstance(text, str) or not text or len(text) > 200 or not re.fullmatch('[!-~]+', text) or any(c in text for c in '@?#' + chr(92)):
        return False
    rest = text
    if '://' in text:
        scheme, rest = text.split('://', 1)
        if scheme not in ('http', 'https'):
            return False
    elif scheme_required:
        return False
    hostport, slash, path = rest.partition('/')
    if slash and (not allow_path or not re.fullmatch(r'[A-Za-z0-9_./-]*', path)):
        return False
    if not _HOSTPORT.fullmatch(hostport):
        return False
    port = hostport.rpartition(':')[2] if ':' in hostport else ''
    return not port or 0 < int(port) < 65536


def validate_hosts(data):
    """The lobby's /bbvr/v1/hosts answer -> a clean {from: to} dict, or None. Accepts {"hosts": {...}} or the bare
    mapping. Keys: "*", a host[:port] or scheme://host[:port][/path]; values: http(s)://host[:port] only. Anything
    else (other types, credentials, queries, too many or too long entries) rejects the whole answer."""
    if isinstance(data, dict) and isinstance(data.get('hosts'), dict):
        data = data['hosts']
    if not isinstance(data, dict) or not data or len(data) > HOSTS_MAX_ENTRIES:
        return None
    out = {}
    for key, value in data.items():
        if key != '*' and not _valid_endpoint(key, False, True):
            return None
        if not _valid_endpoint(value, True, False):
            return None
        out[key] = value
    return out


def fetch_hosts(base, timeout=6):
    """GET <lobby>/bbvr/v1/hosts -> (mapping or None, note). The note says why there is no mapping."""
    try:
        with urllib.request.urlopen(urllib.request.Request(f'{base}/bbvr/v1/hosts'), timeout=timeout) as resp:
            raw = resp.read(HOSTS_MAX_BYTES + 1)
    except urllib.error.HTTPError as err:
        return None, ('the lobby has no host list (http %d)' % err.code)
    except (OSError, ValueError) as err:
        return None, 'cannot reach the lobby: %s' % (getattr(err, 'reason', err),)
    if len(raw) > HOSTS_MAX_BYTES:
        return None, 'the lobby host list is too large'
    try:
        mapping = validate_hosts(json.loads(raw.decode('utf-8-sig')))
    except ValueError:
        mapping = None
    return (mapping, '') if mapping else (None, 'the lobby host list is not valid')


def sync_host_overrides(base, user_dir, log=None, timeout=6):
    """Before an online launch: ask the lobby where the game's own HTTP calls (summon signs, bloodstains, messages)
    should go and save the answer as user/host_overrides.lobby.json. Returns the file path to hand the game
    (SHADPS4_HTTP_HOST_OVERRIDES_JSON), or None. The player's own user/host_overrides.custom.json always wins (it is
    returned as it is and never touched); a lobby without the endpoint keeps the old behaviour (nothing
    redirected) with a log line. A stale lobby file is removed when the lobby no longer answers."""
    log = log or (lambda text: None)
    user_dir = Path(user_dir)
    custom = user_dir / 'host_overrides.custom.json'
    if custom.exists():
        log('Online: using your host_overrides.custom.json; the lobby host list is not asked')
        return custom
    target = user_dir / HOSTS_FILE
    mapping, note = fetch_hosts(base, timeout)
    if mapping is None:
        try:
            target.unlink()
        except OSError:
            pass
        log('Online: no game-server redirect from the lobby (%s); summon signs and bloodstains use the default servers' % note)
        return None
    write_json(target, mapping)
    log('Online: game-server redirect from the lobby (%d entr%s)' % (len(mapping), 'y' if len(mapping) == 1 else 'ies'))
    return target


def _pid_alive(pid):
    if sys.platform == 'win32':
        import ctypes
        k32 = ctypes.windll.kernel32
        handle = k32.OpenProcess(0x00100000, False, int(pid))   # SYNCHRONIZE
        if not handle:
            return False
        try:
            return k32.WaitForSingleObject(handle, 0) == 0x102     # WAIT_TIMEOUT: still running
        finally:
            k32.CloseHandle(handle)
    try:
        os.kill(int(pid), 0)
        return True
    except OSError:
        return False


def watch(user_dir, interval=INTERVAL, stop=None, pid=None, log=None, room_file=None):
    """Heartbeat loop. Stops when `stop` (a threading.Event) is set, when the game process `pid` is gone, or when the
    room is closed. Returns the last data written (or None)."""
    stop = stop or threading.Event()
    last = None
    errors = 0
    while not stop.is_set():
        if pid and not _pid_alive(pid):
            break
        try:
            data = refresh(user_dir, room_file)
        except Exception as err:   # never take the game or the launcher down
            data = None
            if log:
                log('Online: heartbeat error: %s' % err)
        if data is None:
            errors += 1
            if errors == 3 and log:
                log('Online: the room server did not answer; the game keeps its last view')
        else:
            errors = 0
            last = data
            if data.get('closed'):
                if log:
                    log('Online: the room is closed (%s)' % data.get('reason'))
                break
        stop.wait(interval)
    return last


def start_thread(user_dir, interval=INTERVAL, log=None, room_file=None):
    """-> (thread, stop_event) or (None, None) when there is no room file with a token."""
    saved = read_json(room_file or room_file_path(user_dir))
    if not saved.get('token') or not saved.get('server'):
        return None, None
    stop = threading.Event()
    thread = threading.Thread(target=watch, args=(user_dir, interval, stop, None, log, room_file), daemon=True,
                              name='bbvr-online-heartbeat')
    thread.start()
    return thread, stop


def _arg(argv, name, default=None):
    if name in argv:
        i = argv.index(name)
        if i + 1 < len(argv):
            return argv[i + 1]
    return default


def default_user_dir():
    data = os.environ.get('BBVR_DATA_DIR')
    return Path(data) / 'user' if data else Path(__file__).resolve().parents[3] / 'user'


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    cmd = argv[0] if argv else 'once'
    user_dir = Path(_arg(argv, '--user-dir') or default_user_dir())
    if cmd == 'watch':
        pid = _arg(argv, '--pid')
        interval = float(_arg(argv, '--interval', INTERVAL))
        try:
            watch(user_dir, interval, pid=int(pid) if pid else None, log=lambda t: print(t, flush=True))
        except KeyboardInterrupt:
            pass
        return 0
    if cmd == 'once':
        data = refresh(user_dir)
        print(json.dumps(data if data is not None else read_json(room_file_path(user_dir)), indent=2))
        return 0 if data is not None else 1
    if cmd == 'env':
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import bbvr_settings
        env = bbvr_settings.online_env({}, {}, user_dir, public=False)
        for key in sorted(env):
            print('%s=%s' % (key, env[key]))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
