# SPDX-License-Identifier: GPL-2.0-or-later
"""The start-up settings menu of Bloodborne VR: a local web page served on 127.0.0.1 (random port, a
random token per run, nothing else can reach it), opened as an app window in Edge / Chrome or in the
default browser; a console menu when no browser can be opened. Standard library only.

The page edits profiles and options (bbvr_settings.SCHEMA). "Start game" ends the menu wait; the server
keeps running while the game does, and a change of a live option is written to the control files at once.
"""
import hmac
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import bbvr_settings as S


def crash_module(port):
    """scripts/crash_report.py of the port (None when it is missing)."""
    scripts = Path(port) / 'scripts'
    if str(scripts) not in sys.path:
        sys.path.append(str(scripts))
    try:
        import crash_report
        return crash_report
    except ImportError:
        return None


class Menu:
    def __init__(self, settings_path, paths, caps, settings=None):
        self.settings_path = Path(settings_path)
        self.paths = paths
        self.caps = caps
        self.settings = settings or S.load_settings(self.settings_path)
        self.token = secrets.token_urlsafe(18)
        self.game_running = False
        self.launch_event = threading.Event()
        self.quit_event = threading.Event()
        self.lock = threading.RLock()
        self.httpd = None
        self.thread = None
        self.status = ''
        self.crash = crash_module(paths['port'])
        # set by the launcher while it runs the game from this menu
        self.version = ''
        self.online = None      # callable(argv) -> dict: `bbvr_cli.online` (rooms); None = no co-op controls
        self.log_path = None    # the log file of the game being run (or the newest one: log_dir)
        self.log_dir = None
        self.child = None       # the running game process (Popen)

    # -- the game folder, as the player's own files
    def game_dir(self):
        return Path(self.paths['game_dir'])

    def banner_file(self):
        """sce_sys/pic1.png of the player's own game folder (None when it is not there). Read when asked, never copied."""
        pic = self.game_dir() / 'sce_sys' / 'pic1.png'
        return pic if pic.is_file() else None

    def log_tail(self, limit=6000):
        """The last `limit` lines of the running (or newest) launcher log."""
        path = self.log_path
        if not path and self.log_dir and Path(self.log_dir).is_dir():
            logs = sorted(Path(self.log_dir).glob('bbvr-2*.log'))
            path = logs[-1] if logs else None
        if not path or not Path(path).is_file():
            return []
        try:
            return Path(path).read_text(encoding='utf-8', errors='replace').splitlines()[-limit:]
        except OSError:
            return []

    def stop_game(self):
        """Ends the game the launcher started (its whole process tree)."""
        child = self.child
        if child is None or child.poll() is not None:
            return False
        try:
            subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=15)
        except (OSError, subprocess.SubprocessError):
            child.kill()
        self.status = 'Stopped'
        return True

    # -- the DLSS-NR model file: the player's own copy, copied next to the game exe (never downloaded)
    def install_nr_dll(self, path=None):
        # copies the player's own nvngx_dlssnr.dll (path, or picked in a file dialog) next to the game exe
        path = path or S.pick_file('Select your nvngx_dlssnr.dll', 'DLSS-NR model (nvngx_dlssnr.dll)|nvngx_dlssnr.dll')
        result = S.install_nr_dll(path, self.paths['port'])
        with self.lock:
            self.caps = S.detect_all(self.paths['port'])
        return result

    def open_nr_help(self):
        return S.open_nr_doc(self.paths['port'], self.paths.get('data'))

    # -- crash reports: always saved on this PC, never sent (docs/CRASH_REPORTS.md)
    def crash_state(self):
        if not self.crash:
            return {'available': False, 'dir': '', 'count': 0, 'latest': ''}
        reports = self.crash.list_reports()
        return {'available': True, 'dir': str(self.crash.store_dir()), 'count': len(reports),
                'latest': str(reports[-1]) if reports else ''}

    # -- state shared with the page
    def state(self):
        with self.lock:
            st = S.page_state(self.settings, self.caps)
            values = S.effective_values(self.settings, self.caps)
            plan = S.build_launch(values, self.paths, self.caps, view=st['view'])
            st['warnings'] = list(plan['warnings'])
            chosen = values.get('runtime') or ''
            w = S.runtime_warning(self.caps['runtimes'], chosen) if st['view'] == 'vr' else ''
            if w:
                st['warnings'].append(w)
            game = self.game_dir()
            info = S.game_info(game)
            eboot = (game / 'eboot.bin').is_file()
            app = {'version': self.version, 'game_dir': str(game), 'eboot': eboot, 'game': dict(info, banner='yes' if self.banner_file() else ''),
                   'online': self.online is not None, 'accepted': True}
            st['app'] = app
            built = (Path(self.paths['assets']) / 'ui' / 'icons.json').is_file()
            st['ready'] = S.ready_check(app, st['caps'], st['view'], info, built)
            st['game_running'] = self.game_running
            st['status'] = self.status
            st['crash'] = self.crash_state()
            st['dlss_native'] = bool(self.caps['dlss'].get('native'))
            return st

    def update(self, body):
        """body: {mode?, coop_view?, quality?, set?, reset?, profile?, values?, show_menu?}. Saves the settings; while the
        game runs, writes the live files. mode: vr | coop | flat; coop_view: vr | flat; quality: Low .. Super High (drops the
        edits of what the preset sets); set: {option: value} the player changed (kept as overrides); reset: [option] back
        to the preset's value. profile / values are the older pages' way (values makes the profile Custom)."""
        with self.lock:
            blocked = ''
            if body.get('mode') in S.MODES:
                self.settings['mode'] = body['mode']
            if body.get('coop_view') in S.COOP_VIEWS:
                self.settings['coop_view'] = body['coop_view']
            if body.get('quality') in S.QUALITY_PRESETS:
                ok, reason = S.nr_state(self.caps)
                if body['quality'] in S.NR_PRESETS and not ok:  # the card is greyed out; a stale click does nothing
                    blocked = body['quality'] + ' is not available: ' + reason
                else:
                    self.settings = S.set_quality(self.settings, body['quality'])
            if isinstance(body.get('reset'), list):
                self.settings['overrides'] = {k: v for k, v in self.settings.get('overrides', {}).items()
                                              if k not in body['reset']}
            if isinstance(body.get('set'), dict):
                self.settings = S.apply_changes(self.settings, body['set'], self.caps)
            profile = body.get('profile')
            if profile and profile != self.settings['profile'] and 'values' not in body:
                self.settings['profile'] = profile if profile in S.PROFILE_ORDER else self.settings['profile']
                if self.settings['profile'] == 'Custom' and not self.settings.get('custom'):
                    self.settings['custom'] = {k: v for k, v in S.effective_values(
                        dict(self.settings, profile='Default'), self.caps).items()}
            elif 'values' in body:
                self.settings = S.apply_edit(self.settings, body['values'], profile if profile else None, self.caps)
            if 'show_menu' in body:
                self.settings['show_menu'] = bool(body['show_menu'])
            S.save_settings(self.settings_path, self.settings)
            if self.game_running:
                plan = S.build_launch(S.effective_values(self.settings, self.caps), self.paths, self.caps)
                changed = S.apply_files(plan, self.paths)
                self.status = 'Applied live: ' + ', '.join(Path(c).name for c in changed) if changed else 'Saved'
            else:
                self.status = 'Saved'
            if blocked:
                self.status = blocked

    # -- server
    def start(self):
        menu = self

        class Handler(BaseHTTPRequestHandler):
            server_version = 'bbvr-menu'

            def log_message(self, *a):
                pass

            def _deny(self, code=403):
                self.send_response(code)
                self.send_header('Content-Length', '0')
                self.end_headers()

            def _ok(self):
                port = self.server.server_address[1]
                if self.headers.get('Host', '') not in (f'127.0.0.1:{port}', f'localhost:{port}'):
                    return False
                origin = self.headers.get('Origin')
                if origin and origin not in (f'http://127.0.0.1:{port}', f'http://localhost:{port}'):
                    return False
                token = self.headers.get('X-BB-Token') or parse_qs(urlparse(self.path).query).get('t', [''])[0]
                return hmac.compare_digest(token, menu.token)

            def _json(self, obj, code=200):
                data = json.dumps(obj).encode('utf-8')
                self.send_response(code)
                self.send_header('Content-Type', 'application/json; charset=utf-8')
                self.send_header('Content-Length', str(len(data)))
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                if not self._ok():
                    return self._deny()
                path = urlparse(self.path).path
                if path == '/':
                    data = PAGE.replace('__TOKEN__', menu.token).encode('utf-8')
                    self.send_response(200)
                    self.send_header('Content-Type', 'text/html; charset=utf-8')
                    self.send_header('Content-Length', str(len(data)))
                    self.send_header('Cache-Control', 'no-store')
                    self.send_header('X-Frame-Options', 'DENY')
                    self.end_headers()
                    self.wfile.write(data)
                elif path == '/api/state':
                    self._json(menu.state())
                elif path == '/api/log':
                    self._json({'lines': menu.log_tail(), 'running': menu.game_running})
                elif path == '/api/banner':
                    pic = menu.banner_file()
                    if not pic:
                        return self._deny(404)
                    try:
                        data = pic.read_bytes()
                    except OSError:
                        return self._deny(404)
                    self.send_response(200)
                    self.send_header('Content-Type', 'image/png')
                    self.send_header('Content-Length', str(len(data)))
                    self.send_header('Cache-Control', 'private, max-age=3600')
                    self.end_headers()
                    self.wfile.write(data)
                else:
                    self._deny(404)

            def do_POST(self):
                if not self._ok() or not (self.headers.get('Content-Type', '').startswith('application/json')):
                    return self._deny()
                try:
                    n = int(self.headers.get('Content-Length', '0'))
                    body = json.loads(self.rfile.read(min(n, 1 << 20)) or b'{}')
                    if not isinstance(body, dict):
                        raise ValueError
                except (ValueError, OSError):
                    return self._deny(400)
                path = urlparse(self.path).path
                if path == '/api/save':
                    menu.update(body)
                    self._json(menu.state())
                elif path == '/api/launch':
                    menu.update(body)
                    menu.launch_event.set()
                    self._json(menu.state())
                elif path == '/api/nr-dll':  # {"path"?}: copy the player's own nvngx_dlssnr.dll (no path: a file dialog)
                    p = body.get('path')
                    self._json({'result': menu.install_nr_dll(p if isinstance(p, str) else None), 'state': menu.state()})
                elif path == '/api/nr-help':
                    self._json(menu.open_nr_help())
                elif path == '/api/quit':
                    menu.quit_event.set()
                    menu.launch_event.set()
                    self._json({'ok': True})
                elif path == '/api/stop':
                    self._json({'ok': menu.stop_game()})
                elif path == '/api/online':  # rooms: {"argv": ["view"] | ["create"] | ["join", CODE] | ["leave"] | ["server", URL]}
                    argv = body.get('argv')
                    if menu.online is None or not isinstance(argv, list) or not argv or not all(isinstance(a, str) for a in argv) \
                            or argv[0] not in ('view', 'create', 'join', 'leave', 'server'):
                        return self._deny(400)
                    out = menu.online(argv)
                    if argv[0] != 'view':
                        out = {'result': out, 'view': menu.online(['view'])}
                    else:
                        out = {'result': out, 'view': out}
                    self._json(out)
                else:
                    self._deny(404)

        self.httpd = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        return self.url

    @property
    def url(self):
        return f'http://127.0.0.1:{self.port}/?t={self.token}'

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
            self.httpd = None


def find_app_browser():
    roots = [os.environ.get(k) for k in ('ProgramFiles(x86)', 'ProgramFiles', 'LOCALAPPDATA')]
    rels = [r'Microsoft\Edge\Application\msedge.exe', r'Google\Chrome\Application\chrome.exe']
    for rel in rels:
        for root in roots:
            if root and Path(root, rel).is_file():
                return str(Path(root, rel))
    return None


def open_page(url):
    """App window in Edge / Chrome (--app=), else the default browser. Returns how it was opened or ''."""
    browser = find_app_browser()
    if browser:
        try:
            profile = Path(os.environ.get('TEMP', '.')) / 'bbvr-menu-profile'
            subprocess.Popen([browser, f'--app={url}', '--window-size=1100,820', f'--user-data-dir={profile}',
                              '--no-first-run', '--no-default-browser-check'])
            return 'app window'
        except OSError:
            pass
    try:
        if webbrowser.open(url):
            return 'default browser'
    except webbrowser.Error:
        pass
    return ''


def console_menu(settings_path, caps, say=print, ask=input):
    """Text fallback (no browser): mode, VR quality, render mode and profile, then Enter. Returns the settings, or None
    to quit."""
    settings = S.load_settings(settings_path)
    while True:
        values = S.effective_values(settings, caps)
        say('\nBloodborne VR')
        say('  Mode:        ' + '   '.join(('[' if m == settings['mode'] else ' ') + S.MODE_LABELS[m] + (']' if m == settings['mode'] else ' ')
                                          for m in S.MODES) + '   (v = VR, c = Co-op, f = Flat)')
        if settings['mode'] == 'coop':
            say('  Co-op in:    ' + settings['coop_view'].upper() + '   (t = switch VR / Flat); host or join a room with: bbvr_cli.py online ...')
        if S.play_view(settings) == 'vr':
            say('  VR quality:  ' + '   '.join(('[' if q == settings['quality'] else ' ') + q + (']' if q == settings['quality'] else ' ')
                                              for q in S.QUALITY_ORDER) + '   (1 = Low .. ' + str(len(S.QUALITY_ORDER)) + ' = Super High)')
            w, h = S.eye_image(values)
            say(f'               {values["upscale"].upper()}, eye image {w}x{h}; ' + S.QUALITY_NOTES[settings['quality']])
            say('  Render mode: ' + ('Stereo' if values['mode'] == 'stereo' else values['mode'].capitalize()) +
                '   (s = Stereo, m = Mono: use if you get eye strain or performance problems)')
            d = caps['dlss']
            say('  DLSS:        ' + ('available' if d['ok'] else 'not available - ' + d['reason']))
        say('  Profile:     ' + settings['profile'] + '   (p = choose an older profile)')
        try:
            answer = ask('Enter to start, x to quit: ').strip().lower()
        except EOFError:
            answer = ''
        if answer == 'x':
            return None
        if not answer:
            S.save_settings(settings_path, settings)
            return settings
        if answer in ('v', 'c', 'f'):
            settings['mode'] = {'v': 'vr', 'c': 'coop', 'f': 'flat'}[answer]
        elif answer == 't':
            settings['coop_view'] = 'flat' if settings['coop_view'] == 'vr' else 'vr'
        elif answer.isdigit() and 1 <= int(answer) <= len(S.QUALITY_ORDER) and (S.QUALITY_ORDER[int(answer) - 1] not in S.NR_PRESETS or S.nr_state(caps)[0]):
            settings = S.set_quality(settings, S.QUALITY_ORDER[int(answer) - 1])
        elif answer in ('s', 'm'):
            settings = S.apply_changes(settings, {'mode': 'stereo' if answer == 's' else 'mono'}, caps)
        elif answer == 'p':
            for i, name in enumerate(S.PROFILE_ORDER, 1):
                say(f' {"*" if name == settings["profile"] else " "}{i}. {name} - {S.PROFILE_NOTES[name]}')
            try:
                pick = ask('Number of the profile: ').strip()
            except EOFError:
                pick = ''
            if pick.isdigit() and 1 <= int(pick) <= len(S.PROFILE_ORDER):
                settings['profile'] = S.PROFILE_ORDER[int(pick) - 1]


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bloodborne VR</title>
<style>
:root{--bg:#0e0c0b;--panel:#151210;--card:#1c1815;--card2:#26211c;--card3:#322b25;--line:#2e2722;--line2:#4f443a;--text:#e9e2d6;--dim:#aa9f90;--gold:#c8a96a;--blood:#7c1717;--blood2:#9e2222;--blood3:#5e1010;--ok:#7fb88a;--warn:#d9a441;--bad:#d36b5c;
 --serif:Georgia,"Times New Roman",serif;--sans:"Segoe UI Variable","Segoe UI",system-ui,-apple-system,sans-serif;--mono:ui-monospace,"Cascadia Mono",Consolas,monospace}
*{box-sizing:border-box}html,body{height:100%}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 var(--sans)}
#app{display:grid;grid-template-columns:220px minmax(0,1fr);grid-template-rows:minmax(0,1fr) 76px;height:100vh}
aside{grid-row:1;background:var(--bg);border-right:1px solid var(--line);display:flex;flex-direction:column;min-height:0}
.brand{padding:18px 22px 14px}.brand b{font:700 21px var(--serif);color:var(--gold);letter-spacing:.02em;display:block}
.brand .tag{display:inline-block;margin-top:8px;background:var(--blood);color:#f4ece0;font-weight:700;font-size:13px;border-radius:5px;padding:1px 10px}
.brand .ver{color:var(--dim);font-size:13px;margin-left:8px}
nav{flex:1;overflow-y:auto;padding-top:6px}
nav button{display:block;width:100%;text-align:left;background:none;border:0;border-left:3px solid transparent;border-radius:0;color:#b9ae9d;font:16px var(--sans);padding:9px 21px;cursor:pointer}
nav button:hover{background:#17130f;color:#d8cfc0}nav button.on{background:var(--panel);border-left-color:var(--blood2);color:var(--gold);font-weight:600}
.advrow{padding:14px 18px;color:#b9ae9d;font-size:13.5px;display:flex;gap:8px;align-items:center;white-space:nowrap}
main{grid-row:1;background:var(--panel);overflow-y:auto;padding:14px 28px 28px;min-width:0}
header{display:flex;align-items:baseline;gap:16px;margin:0 0 14px}header h1{font:700 27px var(--serif);color:#f2ead9;margin:0}header span{color:var(--dim);font-size:13.5px}
footer{grid-column:1/-1;background:var(--bg);border-top:1px solid var(--line);display:flex;align-items:center;gap:14px;padding:0 24px}
#statusdot{width:9px;height:9px;border-radius:50%;background:var(--dim);box-shadow:0 0 0 4px rgba(170,159,144,.18);flex:none}
#statustxt{flex:1;min-width:0;color:#d2c9ba;font-size:14.5px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#stop{height:40px;width:110px}
#play{height:52px;width:230px;font:700 24px var(--serif);letter-spacing:.04em;color:#f4ece0;background:var(--blood);border:1px solid rgba(200,169,106,.35);border-radius:8px;cursor:pointer}
#play:hover{background:var(--blood2)}#play:active{background:var(--blood3)}#play.blocked{opacity:.55}#play:disabled{opacity:.55;cursor:default}
button{font:inherit;color:var(--text);background:var(--card2);border:1px solid var(--line);border-radius:7px;padding:7px 14px;cursor:pointer}
button:hover{background:var(--card3)}button:disabled{opacity:.45;cursor:default}button.pri{background:var(--blood);border-color:rgba(200,169,106,.35)}button.pri:hover{background:var(--blood2)}
button.out{background:var(--card);border-color:var(--line2)}
input,select,textarea{font:inherit;color:var(--text);background:var(--card2);border:1px solid var(--line);border-radius:7px;padding:7px 10px}
input[type=number],input[type=text]{width:12em}textarea{width:100%;min-height:70px;font-family:var(--mono);font-size:13px}
input[type=range]{width:min(200px,50%);vertical-align:middle;padding:0;accent-color:var(--gold)}
input[type=checkbox]{appearance:none;-webkit-appearance:none;width:40px;height:22px;border-radius:999px;padding:0;position:relative;cursor:pointer;background:#2d2620;border:1px solid #4a4036;vertical-align:middle}
input[type=checkbox]::after{content:"";position:absolute;top:3px;left:3px;width:14px;height:14px;border-radius:50%;background:#f4ece0;opacity:.75;transition:transform .15s}
input[type=checkbox]:checked{background:var(--blood2);border-color:rgba(200,169,106,.7)}input[type=checkbox]:checked::after{transform:translateX(18px);opacity:1}
button:focus-visible,input:focus-visible,select:focus-visible{outline:2px solid var(--gold);outline-offset:2px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin:0 0 14px}
.card.gold{border-color:var(--gold)}
h2.cap{font:600 12.5px var(--sans);letter-spacing:.1em;text-transform:uppercase;color:var(--gold);margin:0 0 8px}
.cols{display:grid;grid-template-columns:minmax(0,43fr) minmax(0,57fr);gap:16px;align-items:start}
@media (max-width:640px){.segs.q5{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media (max-width:860px){.cols{grid-template-columns:1fr}.cols>.r{order:-1}}
.banner{position:relative;height:clamp(110px,22vh,180px);border:1px solid var(--line);border-radius:10px;overflow:hidden;margin:0 0 14px;background:linear-gradient(135deg,#2a0f0d,#120d0b 60%,#0b0908)}
.banner img{width:100%;height:100%;object-fit:cover;object-position:50% 35%;display:block}
.banner::after{content:"";position:absolute;inset:35% 0 0 0;background:linear-gradient(transparent,rgba(14,12,11,.92))}
.banner .t{position:absolute;left:22px;bottom:12px;z-index:1;color:var(--gold);font-size:14px}
.banner .big{position:absolute;left:22px;bottom:36px;z-index:1;font:700 34px var(--serif);color:#f2ead9}.banner .big i{color:var(--gold);font-style:normal}
.ready{display:grid;grid-template-columns:auto minmax(0,1fr) auto;gap:2px 12px;align-items:center;padding:7px 0}
.dot{width:11px;height:11px;border-radius:50%;box-shadow:0 0 0 4px color-mix(in srgb,currentColor 18%,transparent);margin:0 3px}.dot.hollow{background:none!important;border:1.5px solid currentColor;box-shadow:none;width:11px;height:11px}
.ready b{font-weight:600}.ready .d{grid-column:2;color:var(--dim);font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.segs{display:grid;gap:8px}.segs.m3{grid-template-columns:repeat(3,minmax(0,1fr))}.segs.q5{grid-template-columns:repeat(5,minmax(0,1fr))}.segs.row{display:flex;flex-wrap:wrap}
.seg{text-align:left;background:var(--card2);border:1px solid var(--line);border-radius:9px;padding:9px 12px;color:var(--text);min-width:0}
.seg:hover{background:var(--card3);border-color:#5a4d40}.seg.on{background:rgba(124,23,23,.30);border:2px solid var(--gold);padding:8px 11px;color:#f4ece0}
.seg b{display:block;font:700 18px var(--serif)}.seg.m3 b{font-size:21px}.seg small{display:block;color:var(--dim);font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.seg.on small:first-of-type{color:var(--gold)}
.seg.slim{padding:8px 16px}.seg.slim.on{padding:7px 15px}.seg.slim b{font:600 15px var(--sans)}
.seg{display:flex;flex-direction:column;justify-content:flex-start;align-items:flex-start}.segs.q5 .seg{padding:8px 6px;overflow:hidden}.segs.q5 .seg.on{padding:7px 5px}.segs.q5 .seg b{font-size:12.5px;white-space:nowrap}.segs.q5 .seg small{font-size:12px}.seg small.fps{white-space:normal;color:var(--dim);margin-top:2px}.seg.na{opacity:.5;cursor:default}.seg.na:hover{background:var(--card2);border-color:var(--line)}
.note{color:var(--dim);font-size:13px;margin:8px 0 0}
.sub{margin:12px 0 4px;font-weight:600;font-size:15px}
.row{display:grid;grid-template-columns:minmax(120px,1fr) max-content;gap:6px 20px;padding:10px 0;border-bottom:1px solid color-mix(in srgb,var(--line) 80%,transparent);align-items:center}
.row:last-child{border-bottom:0}.row.off{opacity:.5}.row.compact{padding:6px 0;grid-template-columns:minmax(0,1fr) auto}.row.compact input[type=range]{width:90px}.row.compact input[type=number]{width:5em}.lab b{font-weight:600;font-size:15px}.hint{color:var(--dim);font-size:13px;margin-top:2px}
.ctl{justify-self:end;max-width:100%}.ctl select{max-width:min(300px,100%)}.ctl textarea{width:min(300px,100%)}.ctl input[type=text]{width:min(14em,100%)}
.badge{font-size:11px;font-weight:600;margin-left:8px;letter-spacing:.04em;vertical-align:middle}.badge.live{color:var(--gold)}.badge.restart{color:var(--dim)}.badge.edited{color:var(--warn)}
table.pt{width:100%;border-collapse:collapse;font-size:14px;margin-top:10px}table.pt th,table.pt td{text-align:left;padding:5px 8px;border-bottom:1px solid var(--line)}table.pt th{background:var(--card2);font-weight:600}table.pt td:first-child{color:var(--dim)}table.pt .cur{color:var(--gold)}
.msg{padding:10px 14px;border-radius:9px;border:1px solid var(--warn);background:#2a1d12;color:#e3b25a;margin:0 0 12px;font-size:14px}.msg.bad{border-color:var(--bad);background:#2a1114;color:var(--text)}
.code{font:600 38px var(--mono);letter-spacing:.04em}
.mem{display:flex;gap:12px;align-items:flex-start;padding:6px 0}.mem .dot{margin-top:5px}
#logbox{height:calc(100vh - 230px);min-height:200px;overflow:auto;background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px;font:13px/1.45 var(--mono);color:#cfc6b8;white-space:pre-wrap;word-break:break-word;margin:10px 0 0}
table.ctlmap{width:100%;border-collapse:collapse}table.ctlmap td,table.ctlmap th{text-align:left;padding:6px 10px;border-bottom:1px solid var(--line);vertical-align:top}table.ctlmap th{background:var(--card2)}table.ctlmap td:first-child{font-weight:600;width:42%}
[hidden]{display:none!important}
@media (max-width:720px){#app{grid-template-columns:1fr;grid-template-rows:auto minmax(0,1fr) 76px}aside{grid-row:1;border-right:0;border-bottom:1px solid var(--line);flex-direction:row;align-items:center;overflow-x:auto}
 main{grid-row:2;padding:12px 16px 24px}nav{display:flex;padding:0;overflow:visible}nav button{white-space:nowrap;border-left:0;border-bottom:3px solid transparent;padding:10px 12px}nav button.on{border-bottom-color:var(--blood2)}
 .brand,.advrow{display:none}#play{width:150px;font-size:20px}#stop{width:80px}footer{padding:0 14px}.row{grid-template-columns:1fr}.ctl{justify-self:start}}
@media (prefers-reduced-motion:reduce){*{transition:none!important}}
</style></head><body>
<div id="app">
<aside><div class="brand"><b>BLOODBORNE</b><span class="tag">VR</span><span class="ver" id="ver"></span></div><nav id="nav"></nav>
<label class="advrow"><input type="checkbox" id="adv"> Show advanced options</label></aside>
<main><header><h1 id="title">Play</h1><span id="sub"></span></header><div id="banners"></div><div id="page"></div></main>
<footer><i id="statusdot"></i><span id="statustxt">Loading...</span><button id="stop" class="out" disabled>Stop</button><button id="play">PLAY</button></footer>
</div>
<script>
const TOKEN="__TOKEN__";
let nrMsg='',nrBad=false,S=null,tab='play',saveTimer=null,pending={},online=null,onlineMsg='',lobbyEdit=false,logCleared=0,logTimer=null,busy=false;
try{tab=localStorage.getItem('bbvr_tab')||'play'}catch(e){}
{const qs=new URLSearchParams(location.search);if(qs.get('tab'))tab=qs.get('tab');if(qs.get('adv'))setTimeout(()=>{$('adv').checked=true;render()},0)}
const $=id=>document.getElementById(id);
const el=(t,c,txt)=>{const e=document.createElement(t);if(c)e.className=c;if(txt!==undefined)e.textContent=txt;return e};
async function api(path,body){const r=await fetch(path,{method:body?'POST':'GET',headers:{'X-BB-Token':TOKEN,'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined});if(!r.ok)throw new Error(r.status);return r.json()}
function view(){return S.view}
function dlssOK(){return S.caps.dlss.ok}
function opt(id){return S.options.find(o=>o.id===id)}
function edited(id){return (S.overrides||[]).indexOf(id)>=0}
function applyState(s){S=s;render()}
function flushBody(extra){const b=Object.assign({},extra||{});if(Object.keys(pending).length&&!('quality' in b))b.set=pending;pending={};clearTimeout(saveTimer);saveTimer=null;return b}
async function save(extra){try{applyState(await api('/api/save',flushBody(extra)))}catch(e){$('statustxt').textContent='Could not save'}}
function schedule(){clearTimeout(saveTimer);saveTimer=setTimeout(()=>save({}),400)}
function setVal(id,v){S.values[id]=v;pending[id]=v;schedule()}
// ---- option controls
function mk(o){
 const v=S.values[o.id];let c;
 if(o.kind==='bool'){c=el('input');c.type='checkbox';c.checked=!!v;c.onchange=()=>setVal(o.id,c.checked)}
 else if(o.kind==='choice'||o.kind==='runtime'){c=el('select');let list=[];
  if(o.kind==='runtime')list=[['', 'System active runtime'+(S.caps.active_runtime?' ('+S.caps.active_runtime+')':'')]].concat(S.caps.runtimes.map(r=>[r.path,r.name+' ['+r.kind+']'+(r.active?' (active)':'')]));
  else{list=o.choices.filter(x=>!(o.adv_choices&&o.adv_choices.indexOf(x)>=0&&!$('adv').checked&&String(v)!==x)).map(x=>[x,(o.labels&&o.labels[x])||(x==='default'?'default (driver)':x)]);if(v===null)list.unshift(['','(game default)'])}
  for(const [val,txt] of list){const op=el('option',null,txt);op.value=val;if(o.id==='upscale'&&val==='dlss'&&!dlssOK()){op.disabled=true;op.textContent='DLSS (not available)'}c.appendChild(op)}
  c.value=v===null?'':String(v);c.onchange=()=>setVal(o.id,c.value)}
 else if(o.kind==='multiline'){c=el('textarea');c.value=v||'';c.onchange=()=>setVal(o.id,c.value)}
 else if(o.kind==='int'||o.kind==='float'){c=el('span');const n=el('input');n.type='number';n.step=o.step||1;if(o.min!==undefined)n.min=o.min;if(o.max!==undefined)n.max=o.max;n.value=v===null?'':v;n.placeholder='game default';
  n.onchange=()=>setVal(o.id,n.value===''?null:Number(n.value));
  if(o.min!==undefined&&o.max!==undefined&&o.kind==='float'&&o.max-o.min<=10){const r=el('input');r.type='range';r.min=o.min;r.max=o.max;r.step=o.step||0.01;r.value=v===null?o.min:v;r.oninput=()=>{n.value=r.value};r.onchange=()=>{n.value=r.value;setVal(o.id,Number(r.value))};c.appendChild(r);c.appendChild(document.createTextNode(' '))}
  c.appendChild(n)}
 else{c=el('span');const t=el('input');t.type='text';t.value=v===null?'':v;t.placeholder='default';if(o.suggestions){t.setAttribute('list','dl_'+o.id);const d=el('datalist');d.id='dl_'+o.id;o.suggestions.forEach(x=>{const op=el('option');op.value=x;d.appendChild(op)});c.appendChild(d)}
  t.onchange=()=>setVal(o.id,t.value===''?null:t.value);c.appendChild(t)}
 return c}
function offReason(o){
 if(o.only_if)for(const k in o.only_if){if(S.values[k]!==o.only_if[k]){const w=o.only_if[k];return 'needs '+((opt(k)||{}).label||k)+' = '+(w===true?'on':w===false?'off':w)}}
 if(o.id.indexOf('dlssnr')===0&&S.caps.dlssnr&&!S.caps.dlssnr.ok)return 'DLSS-NR is not available';
 return ''}
function optRow(o,compact){
 if(o.id==='mode'){const box=el('div','modebox');box.style.cssText='padding:10px 0;border-bottom:1px solid var(--line)';box.appendChild(el('b',null,o.label));box.appendChild(el('span','badge restart','RESTART'));const m=renderModeControl();m.style.marginTop='8px';box.appendChild(m);return box}
 const off=offReason(o);const row=el('div','row'+(off?' off':'')+(compact?' compact':''));const lab=el('div','lab');lab.appendChild(el('b',null,o.label));
 lab.appendChild(el('span','badge '+(o.live?'live':'restart'),o.live?'LIVE':'RESTART'));if(edited(o.id))lab.appendChild(el('span','badge edited','EDITED'));
 const help=(compact?'':(o.help||''))+(off?(o.help&&!compact?' ':'')+'('+off+')':'');if(help)lab.appendChild(el('div','hint',help));
 row.appendChild(lab);const w=el('div','ctl');w.appendChild(o.id==='mode'?renderModeControl():mk(o));if(off)w.querySelectorAll('input,select').forEach(x=>x.disabled=true);row.appendChild(w);return row}
function renderModeControl(){
 const box=el('div');const cur=S.values.mode;const items=[['stereo','Stereo'],['mono','Mono']];if($('adv').checked||cur==='alt')items.push(['alt','Alternate-eye']);
 const g=el('div','segs row');items.forEach(([id,label])=>{const b=el('button','seg slim'+(cur===id?' on':''));b.appendChild(el('b',null,label));b.onclick=()=>{if(cur!==id){S.values.mode=id;pending.mode=id;save({})}};g.appendChild(b)});box.appendChild(g);
 box.appendChild(el('div','note',cur==='stereo'?'True per-eye 3D (both eyes rendered). Switch to Mono if you get eye strain or performance problems.':cur==='mono'?'One flat screen in the headset: use if you get eye strain or performance problems.':'The older alternate-eye method, kept for testing.'));return box}
function sectionsOf(page){const adv=$('adv').checked;const out=[];
 for(const sec of page.sections){const rows=sec.options.map(opt).filter(o=>o&&(adv||!o.advanced));if(!rows.length)continue;
  const card=el('div','card');card.appendChild(el('h2','cap',sec.title));rows.forEach(o=>card.appendChild(optRow(o,false)));out.push(card)}
 return out}
// ---- pieces
function dotEl(status){const d=el('i','dot'+(status==='na'||status==='info'?' hollow':''));d.style.color=({ok:'var(--ok)',warn:'var(--warn)',bad:'var(--bad)'})[status]||'var(--dim)';if(!(status==='na'||status==='info'))d.style.background='currentColor';return d}
function readyCard(){const c=el('div','card');c.appendChild(el('h2','cap','Ready check'));
 S.ready.forEach(r=>{const row=el('div','ready');row.appendChild(dotEl(r.status));row.appendChild(el('b',null,r.title));row.appendChild(el('span'));const d=el('div','d',r.detail);d.title=r.detail;row.appendChild(d);c.appendChild(row)});return c}
function modeCard(){const c=el('div','card');c.appendChild(el('h2','cap','Mode'));const g=el('div','segs m3');
 const tags={vr:'Solo, headset',coop:'Online',flat:'Solo, monitor'};
 S.modes.forEach(m=>{const b=el('button','seg m3'+(S.mode===m.id?' on':''));b.appendChild(el('b',null,m.label));b.appendChild(el('small',null,tags[m.id]));b.onclick=()=>{if(S.mode!==m.id)save({mode:m.id})};g.appendChild(b)});c.appendChild(g);
 c.appendChild(el('div','note',(S.modes.find(m=>m.id===S.mode)||{}).note||''));
 if(S.mode==='coop'){c.appendChild(el('div','sub','Play co-op in'));const r=el('div','segs row');[['vr','VR headset'],['flat','Flat screen']].forEach(([id,label])=>{const b=el('button','seg slim'+(S.coop_view===id?' on':''));b.appendChild(el('b',null,label));b.onclick=()=>{if(S.coop_view!==id)save({coop_view:id})};r.appendChild(b)});c.appendChild(r)}
 return c}
function presetCards(){const g=el('div','segs q5');const modified=S.quality_modified&&S.quality_modified.length;
 S.quality_table.presets.forEach(p=>{const sel=p.name===S.quality;const b=el('button','seg'+(sel?' on':'')+(p.available?'':' na'));b.appendChild(el('b',null,p.name));
  b.appendChild(el('small',null,sel&&modified?'custom (edited)':p.rows[2][1]));
  if(!p.available){b.disabled=true;b.title=p.reason;b.appendChild(el('small','fps','needs your nvngx_dlssnr.dll'))}else if(p.fps)b.appendChild(el('small','fps',p.fps));
  b.onclick=()=>{pending={};save({quality:p.name})};g.appendChild(b)});return g}
function nrBox(){const b=el('div','nrbox');
 b.appendChild(el('div','note','Super High needs the model file nvngx_dlssnr.dll. It is NVIDIA\'s, it is NOT included with this mod, and there is no download here: if you already have a copy on this PC, press Find file... and it is copied next to the game. Without it the game runs High.'));
 const r=el('div');r.style.marginTop='6px';
 const f=el('button','out','Find file...');f.onclick=async()=>{try{const x=await api('/api/nr-dll',{});nrMsg=x.result.message||'';nrBad=!x.result.ok||x.result.available===false;if(x.state)S=x.state}catch(e){nrMsg='That did not work.';nrBad=true}render()};r.appendChild(f);r.appendChild(document.createTextNode(' '));
 const h=el('button','out','What is this?');h.onclick=async()=>{try{const x=await api('/api/nr-help',{});if(!x.ok)window.open(x.url,'_blank','noopener')}catch(e){}};r.appendChild(h);b.appendChild(r);
 if(nrMsg){const m=el('div','note',nrMsg);m.style.color=nrBad?'var(--bad)':'var(--ok)';b.appendChild(m)}
 return b}
function displayLine(){const d=(S.display||{}).sizes;if(S.values.display_size==='game')return 'Display size: the game\'s own 1080p frame; the eye image is the preset / Eye resolution.';
 if(!d)return 'Display size: no headset known yet (start the game once, or pick it), so the preset\'s own sizes are used.';
 return 'Render '+d.render[0]+'x'+d.render[1]+(d.native?' (game\'s own)':'')+' -> eye '+d.eye[0]+'x'+d.eye[1]+' ('+d.upscale+'). The preset picks the upscaler, DLSS-NR and a starting Display resolution; Display size and Display resolution set the eye image for your headset and win over the preset\'s size.'}
function qualityCard(){const c=el('div','card');c.appendChild(el('h2','cap','VR quality'));c.appendChild(presetCards());
 const cur=S.quality_table.presets.find(p=>p.name===S.quality);if(cur)c.appendChild(el('div','note',cur.note));
 S.quality_table.presets.filter(p=>!p.available).forEach(p=>{const n=el('div','note',p.name+' is greyed out: '+p.reason);n.style.color='var(--warn)';c.appendChild(n)});
 if(S.quality_notice){const n=el('div','note',S.quality_notice);n.style.color='var(--warn)';c.appendChild(n)}
 c.appendChild(nrBox());const more=el('button','out','What each preset sets');more.style.marginTop='10px';more.onclick=()=>go('graphics');c.appendChild(more);
 c.appendChild(el('div','sub','VR render mode'));c.appendChild(renderModeControl());return c}
function memberColor(st){return st==='in world'?'var(--ok)':st==='sign up'?'var(--warn)':st==='host'?'var(--gold)':'var(--dim)'}
async function onlineCall(argv){if(busy)return;busy=true;try{const r=await api('/api/online',{argv});online=r.view;onlineMsg=(argv[0]!=='view'&&r.result&&r.result.ok===false)?(r.result.message||'That did not work.'):'';
  if(argv[0]!=='view'&&r.result&&r.result.ok&&(argv[0]==='create'||argv[0]==='join'))await save({mode:'coop'})}catch(e){onlineMsg='The room service did not answer.'}finally{busy=false;render()}}
function inRoom(){return !!(online&&online.in_room&&!online.closed)}
function noServer(){return !!(online&&online.error==='no_server')}
function roomPanel(){const box=el('div');
 if(!S.app.online){box.appendChild(el('div','note','Rooms are not part of this build.'));return box}
 if(!online){box.appendChild(el('div','note','Reading the room state...'));return box}
 if(inRoom()){box.appendChild(el('div','code',online.code));box.appendChild(el('div','note',online.role==='host'?'You host this room. Give the code to up to two friends, then press PLAY.':'You are a guest in this room. Press PLAY when the host is ready.'));
  if((online.members||[]).length)box.appendChild(el('div','note',online.members.length+' of '+(online.max||3)+' in the room'));
  const r=el('div');r.style.marginTop='10px';const cp=el('button','pri','Copy code');cp.onclick=()=>{navigator.clipboard&&navigator.clipboard.writeText(online.code);onlineMsg='Code copied.';render()};r.appendChild(cp);r.appendChild(document.createTextNode(' '));
  const lv=el('button','out',online.role==='host'?'Close room':'Leave');lv.onclick=()=>onlineCall(['leave']);r.appendChild(lv);box.appendChild(r)}
 else if(noServer()){box.appendChild(el('div','note','Lobby address of your own server (http://address:port).'));const i=el('input');i.type='text';i.placeholder='http://address:port';box.appendChild(i);box.appendChild(document.createTextNode(' '));const b=el('button','pri','Save');b.onclick=()=>{if(i.value.trim())onlineCall(['server',i.value.trim()])};box.appendChild(b)}
 else{const h=el('button','pri','Host a game');h.onclick=()=>onlineCall(['create']);box.appendChild(h);box.appendChild(document.createTextNode(' '));box.appendChild(el('span','note','Opens a room and shows its code.'));
  box.appendChild(el('div','sub','or join a friend'));const i=el('input');i.type='text';i.placeholder='K7Q-M4X';i.style.width='9em';i.style.font='20px var(--mono)';i.style.textTransform='uppercase';box.appendChild(i);box.appendChild(document.createTextNode(' '));
  const j=el('button','out','Join');j.onclick=()=>{if(i.value.trim())onlineCall(['join',i.value.trim().toUpperCase()])};box.appendChild(j);
  if(online.server){const l=el('div','note',online.is_default?'Lobby: default ':'Lobby: your own ');const ch=el('button','out',lobbyEdit?'Hide':'Change');ch.style.padding='2px 10px';ch.onclick=()=>{lobbyEdit=!lobbyEdit;render()};l.appendChild(ch);box.appendChild(l);
   if(lobbyEdit){const a=el('input');a.type='text';a.placeholder='http://address:port';box.appendChild(a);box.appendChild(document.createTextNode(' '));const u=el('button','pri','Use');u.onclick=()=>{if(a.value.trim())onlineCall(['server',a.value.trim()])};box.appendChild(u);
    if(!online.is_default){box.appendChild(document.createTextNode(' '));const d=el('button','out','Use the default lobby');d.onclick=()=>onlineCall(['server','default']);box.appendChild(d)}}}}
 if(onlineMsg){const m=el('div','note',onlineMsg);m.style.color=/copied/i.test(onlineMsg)?'var(--ok)':'var(--bad)';box.appendChild(m)}
 return box}
// ---- pages
function pagePlay(root){
 const ban=el('div','banner');if(S.app.game&&S.app.game.banner){const im=el('img');im.alt='';im.src='/api/banner?t='+encodeURIComponent(TOKEN);im.onerror=()=>im.remove();ban.appendChild(im)}
 else{ban.appendChild(el('div','big','Bloodborne '));ban.firstChild.appendChild(el('i',null,'VR'))}
 const g=S.app.game||{};ban.appendChild(el('div','t',g.title_id?g.title_id+'  -  game version '+g.version:'Choose your game folder (the launcher asks for it at start)'));root.appendChild(ban);
 const cols=el('div','cols'),l=el('div','l'),r=el('div','r');cols.appendChild(l);cols.appendChild(r);root.appendChild(cols);
 l.appendChild(readyCard());
 const q=el('div','card');q.appendChild(el('h2','cap','Quick settings'));
 (view()==='vr'?['display_size'].concat(S.values.display_size==='custom'?['display_custom']:[],['display_scale','fps','fps_counter','hud_center']):['flat_fps']).forEach(id=>{const o=opt(id);if(o)q.appendChild(optRow(o,true))});l.appendChild(q);
 r.appendChild(modeCard());
 if(view()==='vr')q.appendChild(el('div','note',displayLine()));
 if(S.mode==='coop'){const c=el('div','card'+(inRoom()?' gold':''));c.appendChild(el('h2','cap',inRoom()?(online.role==='host'?'Your room (host)':'Your room (guest)'):'Online room'));c.appendChild(roomPanel());r.appendChild(c)}
 if(view()==='vr')r.appendChild(qualityCard());
 else{const c=el('div','card');c.appendChild(el('h2','cap','Flat screen'));c.appendChild(el('div','note',"The game opens in its own window on this PC. No headset or VR runtime is needed. Graphics are set in the game's own options menu, not here; the quality presets are for VR."));r.appendChild(c)}}
function pageGraphics(root){
 if(!dlssOK())root.appendChild(el('div','msg','DLSS is not available: '+S.caps.dlss.reason+' FSR 1 is used instead.'));
 const c=el('div','card');c.appendChild(el('h2','cap','VR quality'));c.appendChild(presetCards());
 const t=el('table','pt'),hd=el('tr');hd.appendChild(el('th',null,'Setting'));S.quality_table.presets.forEach(p=>hd.appendChild(el('th',p.name===S.quality?'cur':'',p.name)));t.appendChild(hd);
 S.quality_table.presets[0].rows.forEach((r,i)=>{const tr=el('tr');tr.appendChild(el('td',null,r[0]));S.quality_table.presets.forEach(p=>tr.appendChild(el('td',p.name===S.quality?'cur':'',p.rows[i][1])));t.appendChild(tr)});c.appendChild(t);
 c.appendChild(el('div','note',(S.quality_table.dlss?'':'DLSS is not available on this PC, so the presets use FSR 1. ')+'The scene is always rendered at 1920x1080; the upscaler makes the eye image, so a bigger eye image looks sharper and costs more GPU time. The frame limit is not part of a preset (Display & FPS). DLSS-NR is off except in Super High.'));
 if(S.quality_modified&&S.quality_modified.length){const n=el('div','note','Edited below: '+S.quality_modified.map(i=>(opt(i)||{}).label||i).join(', ')+'.');n.style.color='var(--warn)';c.appendChild(n);const b=el('button','out','Reset to '+S.quality+' preset');b.style.marginTop='8px';b.onclick=()=>save({reset:S.quality_modified});c.appendChild(b)}
 c.appendChild(nrBox());root.appendChild(c);pageOptions(root,'graphics')}
function pageOptions(root,id){const page=S.pages.find(p=>p.id===id);if(page)sectionsOf(page).forEach(c=>root.appendChild(c));if(!$('adv').checked){const n=el('div','note','Switch on "Show advanced options" in the sidebar for more.');root.appendChild(n)}}
const CONTROLS=[['Left stick','Move'],['Right stick','Camera / turn (smooth or snap, see VR comfort)'],['Right-hand swing','Light attack (R1)'],['Swing with the right grip held, or a very hard swing','Heavy attack (R2); keep the grip held after the swing to charge'],['Quick right-grip tap (under 0.35 s, no swing)','Swap the right-hand weapon'],['Right trigger','Nothing in the world'],['Left trigger (hold)','Hold the gun (L2)'],['Left-hand thrust forward (gun held)','Shoot'],['Left grip (squeeze)','L1 (transform)'],['A (right)','Swap the right-hand weapon'],['B (right)','Roll / dodge, hold to sprint'],['X (left)','Swap the left-hand weapon'],['Y (left)','Interact (doors, pickups, ladders)'],['Heal','From the quick wheel, or bring a held Blood Vial to your mouth'],['Right stick click (tap)','Open the item wheel: pick with your right hand (touch or point) or the stick'],['Left stick click while standing','Open the VR dashboard (equipment, items, options)'],['Left stick click while moving','Jump'],['Left stick click, held 1 s','Recenter'],['Left menu button','Options (pause)'],['Swing at a staggered enemy','Visceral attack (stagger one with a gun parry, or a charged heavy from behind)']];
function pageControls(root){const c=el('div','card');const t=el('table','ctlmap');const h=el('tr');h.appendChild(el('th',null,'Controller input'));h.appendChild(el('th',null,'In game'));t.appendChild(h);CONTROLS.forEach(r=>{const tr=el('tr');tr.appendChild(el('td',null,r[0]));tr.appendChild(el('td',null,r[1]));t.appendChild(tr)});c.appendChild(t);
 c.appendChild(el('div','note',"Lock-on is off by default: a right stick click opens the item wheel instead. In the game's own menus (Options, pause): either stick moves the cursor (hold to repeat), A or either trigger confirms, B goes back, and the menu button closes the menu. Alpha: controls may change."));root.appendChild(c)}
function pageOnline(root){const cols=el('div','cols'),l=el('div','l'),r=el('div','r');cols.appendChild(l);cols.appendChild(r);root.appendChild(cols);
 const c=el('div','card'+(inRoom()?' gold':''));c.appendChild(el('h2','cap','Room'));c.appendChild(roomPanel());c.appendChild(el('div','note','Co-op is for up to three players. A room is opened on the lobby this release comes with; nothing to type. Pick Co-op on the Play page, then press PLAY.'));l.appendChild(c);
 const page=S.pages.find(p=>p.id==='online');if(page)sectionsOf(page).forEach(x=>l.appendChild(x));
 const m=el('div','card');m.appendChild(el('h2','cap','Members'));
 if(!inRoom())m.appendChild(el('div','note','Nobody yet. Host a game or join one to see who is in the room.'));
 else{(online.members||[]).forEach(x=>{const st=x.state||'waiting';const row=el('div','mem');const d=el('i','dot');d.style.color=memberColor(st);d.style.background='currentColor';row.appendChild(d);const t=el('div');t.appendChild(el('b',null,(x.name||'?')+(x.you?' (you)':'')));t.appendChild(el('div','hint',st==='host'?'host':st==='in world'?"in the host's world":st==='sign up'?'sign is up, waiting to be summoned':'waiting (no sign yet)'));row.appendChild(t);m.appendChild(row)});
  m.appendChild(el('h2','cap','Auto-bell'));const b=online.bell||{};m.appendChild(el('div','note',(!b||!b.state)?'Not running. It starts with the game, which rings your bell by itself.':'Auto-bell: '+String(b.state).replace(/_/g,' ')+'.  Rings so far: '+(b.rings||0)))}
 r.appendChild(m)}
async function pageLog(root){const bar=el('div');const cp=el('button',null,'Copy');const cl=el('button',null,'Clear');cl.style.marginLeft='8px';const cnt=el('span','note','');cnt.style.marginLeft='12px';bar.appendChild(cp);bar.appendChild(cl);bar.appendChild(cnt);root.appendChild(bar);
 const box=el('div');box.id='logbox';root.appendChild(box);let all=[];
 const draw=()=>{const lines=all.slice(logCleared);const stick=box.scrollTop+box.clientHeight>=box.scrollHeight-20;box.textContent=lines.length?lines.join('\n'):'The output of the game launcher appears here.';cnt.textContent=lines.length+' lines (the last 6000 are kept)';if(stick)box.scrollTop=box.scrollHeight};
 cp.onclick=()=>{navigator.clipboard&&navigator.clipboard.writeText(all.slice(logCleared).join('\n'))};cl.onclick=()=>{logCleared=all.length;draw()};
 const poll=async()=>{try{const r=await api('/api/log');if(r.lines.length<logCleared)logCleared=0;all=r.lines;draw()}catch(e){}};await poll();clearInterval(logTimer);logTimer=setInterval(()=>{if(tab!=='log'||!document.body.contains(box)){clearInterval(logTimer);return}poll()},1500)}
function pageAdvanced(root){const page=S.pages.find(p=>p.id==='advanced');if(page)sectionsOf(page).forEach(c=>root.appendChild(c));
 const c=el('div','card');c.appendChild(el('h2','cap','Application'));
 const r1=el('div','row');const l1=el('div','lab');l1.appendChild(el('b',null,'Show this window next time'));l1.appendChild(el('div','hint','Off: BloodborneVR starts the game with the saved settings (BloodborneVR.exe --menu brings it back).'));r1.appendChild(l1);const w1=el('div','ctl');const cb=el('input');cb.type='checkbox';cb.checked=S.show_menu;cb.onchange=()=>save({show_menu:cb.checked});w1.appendChild(cb);r1.appendChild(w1);c.appendChild(r1);
 const r2=el('div','row');const l2=el('div','lab');l2.appendChild(el('b',null,'Older profile'));l2.appendChild(el('div','hint','The profiles of earlier versions. Comfort sets snap turning and a steadier camera; the quality presets on the Play page replace Performance and Quality (DLSS).'));r2.appendChild(l2);const w2=el('div','ctl');const sel=el('select');S.profiles.filter(p=>p!=='Custom'||S.profile==='Custom').forEach(p=>{const o=el('option',null,p);o.value=p;sel.appendChild(o)});sel.value=S.profile;sel.onchange=()=>save({profile:sel.value});w2.appendChild(sel);r2.appendChild(w2);c.appendChild(r2);
 const cr=S.crash||{};const r3=el('div','row');const l3=el('div','lab');l3.appendChild(el('b',null,'Crash reports'));l3.appendChild(el('div','hint',cr.available?cr.count+' saved in '+cr.dir+' (never sent automatically)':'Not part of this build.'));r3.appendChild(l3);c.appendChild(r3);root.appendChild(c)}
function pageAbout(root){const c=el('div','card');c.appendChild(el('h2','cap','Bloodborne VR'));const v=el('div',null,'Version '+(S.app.version||'dev'));v.style.font='600 22px var(--sans)';c.appendChild(v);
 c.appendChild(el('div','note','An unofficial fan project that runs YOUR copy of Bloodborne (PS4, CUSA03173, version 1.09) natively on Windows with a Vulkan renderer and an OpenXR bridge for VR headsets. Early alpha, with co-op for up to three players.'));
 c.appendChild(el('h2','cap','Licence'));c.lastChild.style.marginTop='14px';c.appendChild(el('div','note','Free software under the GPL (see LICENSE and THIRD_PARTY_NOTICES.md next to this program). No game files are included and none are ever uploaded: you must own Bloodborne and use your own legally obtained dump. Not affiliated with, endorsed or sponsored by Sony Interactive Entertainment, FromSoftware or any trademark holder. PlayStation, PS4 and Bloodborne are trademarks of their owners.'));
 c.appendChild(el('h2','cap','Credits'));c.lastChild.style.marginTop='14px';c.appendChild(el('div','note','This program uses NVIDIA DLSS (nvngx_dlss.dll, NVIDIA RTX SDKs LICENSE: see licenses/NVIDIA-DLSS-LICENSE.txt). Not sponsored or endorsed by NVIDIA.'));
 c.appendChild(el('h2','cap','Settings'));c.lastChild.style.marginTop='14px';c.appendChild(el('div','note','This window and the dashboard edit the same files (user\\bbvr_settings.json, out\\fp.txt, stereo.txt, iq.txt), so you can use either. Changed control files keep a .bak of the original.'));root.appendChild(c)}
// ---- shell
function go(id){tab=id;try{localStorage.setItem('bbvr_tab',tab)}catch(e){};render()}
function status(){
 const s={text:'',color:'var(--dim)',can:false,go:''};
 if(S.game_running){s.text=view()==='flat'?'The game is running.':'The game is running. Put the headset on.';s.color='var(--gold)';return s}
 if(!S.app.eboot){s.text='No game folder: no eboot.bin in '+S.app.game_dir+'. Start BloodborneVR.exe again and choose it.';s.color='var(--bad)';return s}
 const g=S.ready.find(r=>r.id==='game');if(g&&g.status==='bad'){s.text=g.detail;s.color='var(--bad)';return s}
 if(S.mode==='coop'&&!inRoom()){s.text=noServer()?'Co-op needs a lobby address: enter it on the Online co-op page.':'Co-op: host a room or join one with a code first.';s.color='var(--warn)';s.go=noServer()?'online':'play';return s}
 s.can=true;let t=S.mode==='coop'?'Co-op ('+(view()==='flat'?'flat':'VR')+') in room '+(online?online.code:''):S.mode==='flat'?'Flat':'VR';
 if(view()==='vr')t+='  |  '+S.quality+' ('+(S.values.upscale==='dlss'&&dlssOK()?'DLSS':'FSR 1')+')  |  '+({stereo:'Stereo',mono:'Mono',alt:'Alternate-eye'})[S.values.mode];
 s.color='var(--dim)';if(S.warnings&&S.warnings.length){t+='  |  '+S.warnings[0];s.color='var(--warn)'}s.text='Ready: '+t;return s}
function render(){
 if(!S)return;
 $('ver').textContent=S.app.version||'';
 const nav=$('nav');nav.innerHTML='';const flat=view()==='flat';
 const pages=S.pages.filter(p=>!(p.vr&&flat)&&!(p.id==='online'&&!S.online_play));
 if(!pages.find(p=>p.id===tab))tab='play';
 pages.forEach(p=>{const b=el('button',p.id===tab?'on':'',p.label);b.onclick=()=>go(p.id);nav.appendChild(b)});
 const pg=S.pages.find(p=>p.id===tab);$('title').textContent=pg?pg.label:'Play';
 const subs={graphics:'VR quality presets and the upscaler',display:'Frame rate, counter and render mode',comfort:'Turning, camera, dodging and the HUD',combat:'Hit shapes, attacks, hands and body',controls:'VR controller map',online:'Rooms, members and bells',advanced:'Runtime, logs and the application',log:'What the launcher and the game print',about:'Version, licence and settings files'};$('sub').textContent=subs[tab]||'';
 const bn=$('banners');bn.innerHTML='';(S.warnings||[]).forEach(w=>bn.appendChild(el('div','msg',w)));
 const root=$('page');root.innerHTML='';
 if(tab==='play')pagePlay(root);else if(tab==='graphics')pageGraphics(root);else if(tab==='controls')pageControls(root);else if(tab==='online')pageOnline(root);else if(tab==='log')pageLog(root);else if(tab==='advanced')pageAdvanced(root);else if(tab==='about')pageAbout(root);else pageOptions(root,tab);
 const st=status();$('statustxt').textContent=st.text;$('statustxt').title=st.text;$('statusdot').style.background=st.color;$('statusdot').style.boxShadow='0 0 0 4px color-mix(in srgb,'+st.color+' 18%,transparent)';
 const play=$('play');play.textContent=S.game_running?'RUNNING':'PLAY';play.disabled=!!S.game_running;play.classList.toggle('blocked',!st.can&&!S.game_running);play.onclick=()=>{if(st.can)doStart();else if(st.go)go(st.go)};
 $('stop').disabled=!S.game_running}
async function doStart(){try{applyState(await api('/api/launch',flushBody({})));$('statustxt').textContent='Starting the game - put the headset on. You can keep changing live options here.'}catch(e){}}
$('stop').onclick=async()=>{try{await api('/api/stop',{})}catch(e){}};
$('adv').onchange=render;
api('/api/state').then(s=>{applyState(s);if(S.app.online)onlineCall(['view'])}).catch(()=>{document.body.textContent='This page needs the launcher running.'});
setInterval(async()=>{if(!S||saveTimer)return;try{const s=await api('/api/state');const changed=s.game_running!==S.game_running||s.status!==S.status;S.game_running=s.game_running;S.status=s.status;if(changed)render();if(S.app.online&&!busy&&(tab==='online'||S.mode==='coop')){const before=JSON.stringify(online);const r=await api('/api/online',{argv:['view']});online=r.view;if(JSON.stringify(online)!==before&&(tab==='online'||tab==='play')&&!document.activeElement.matches('input,textarea'))render()}}catch(e){}},3000);
</script></body></html>
"""
