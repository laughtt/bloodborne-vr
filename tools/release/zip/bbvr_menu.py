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
        self.user_dir = self.settings_path.parent  # where crash_config.json lives (same as run.py's user dir)
        self.crash = crash_module(paths['port'])

    # -- crash-report consent (first launch asks in the page; run.py then never needs the console)
    def crash_state(self):
        if not self.crash:
            return {'available': False, 'asked': True, 'mode': 'off'}
        mode = self.crash.load_config(self.user_dir)['crash_reports']
        return {'available': True, 'asked': self.crash.consent_stored(self.user_dir), 'mode': mode}

    def set_crash_mode(self, mode):
        if self.crash and mode in ('on', 'off'):
            self.crash.save_mode(self.user_dir, mode)

    # -- state shared with the page
    def state(self):
        with self.lock:
            st = S.page_state(self.settings, self.caps)
            values = S.effective_values(self.settings)
            plan = S.build_launch(values, self.paths, self.caps)
            st['warnings'] = list(plan['warnings'])
            chosen = values.get('runtime') or ''
            w = S.runtime_warning(self.caps['runtimes'], chosen)
            if w:
                st['warnings'].append(w)
            st['game_running'] = self.game_running
            st['status'] = self.status
            st['crash'] = self.crash_state()
            st['dlss_native'] = bool(self.caps['dlss'].get('native'))
            return st

    def update(self, body):
        """body: {profile?, values?, show_menu?}. Saves the settings; while the game runs, writes the live files."""
        with self.lock:
            profile = body.get('profile')
            if profile and profile != self.settings['profile'] and 'values' not in body:
                self.settings['profile'] = profile if profile in S.PROFILE_ORDER else self.settings['profile']
                if self.settings['profile'] == 'Custom' and not self.settings.get('custom'):
                    self.settings['custom'] = {k: v for k, v in S.effective_values(
                        dict(self.settings, profile='Default')).items()}
            elif 'values' in body:
                self.settings = S.apply_edit(self.settings, body['values'], profile if profile else None)
            if 'show_menu' in body:
                self.settings['show_menu'] = bool(body['show_menu'])
            S.save_settings(self.settings_path, self.settings)
            if self.game_running:
                plan = S.build_launch(S.effective_values(self.settings), self.paths, self.caps)
                changed = S.apply_files(plan, self.paths)
                self.status = 'Applied live: ' + ', '.join(Path(c).name for c in changed) if changed else 'Saved'
            else:
                self.status = 'Saved'

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
                elif path == '/api/crash':
                    menu.set_crash_mode(body.get('mode'))
                    self._json(menu.state())
                elif path == '/api/launch':
                    menu.update(body)
                    menu.launch_event.set()
                    self._json(menu.state())
                elif path == '/api/quit':
                    menu.quit_event.set()
                    menu.launch_event.set()
                    self._json({'ok': True})
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
    """Text fallback: pick a profile, then Enter. Returns the settings, or None to quit."""
    settings = S.load_settings(settings_path)
    while True:
        say('\nProfiles:')
        for i, name in enumerate(S.PROFILE_ORDER, 1):
            mark = '*' if name == settings['profile'] else ' '
            say(f' {mark}{i}. {name} - {S.PROFILE_NOTES[name]}')
        d = caps['dlss']
        say('DLSS: ' + ('available' if d['ok'] else 'not available - ' + d['reason']))
        try:
            answer = ask('Number to choose a profile, Enter to start, q to quit: ').strip().lower()
        except EOFError:
            answer = ''
        if answer == 'q':
            return None
        if not answer:
            S.save_settings(settings_path, settings)
            return settings
        if answer.isdigit() and 1 <= int(answer) <= len(S.PROFILE_ORDER):
            settings['profile'] = S.PROFILE_ORDER[int(answer) - 1]


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bloodborne VR settings</title>
<style>
:root{--bg:#15161a;--panel:#1e2026;--line:#33363f;--text:#e8e4d8;--dim:#9a978c;--accent:#c8453a;--ok:#5fae6b;--warn:#d6a642;--live:#3d8f9a;--field:#0f1013}
@media (prefers-color-scheme: light){:root{--bg:#f3f1ec;--panel:#fff;--line:#d5d1c6;--text:#23221f;--dim:#6b685f;--field:#faf9f6}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 system-ui,Segoe UI,sans-serif}
header{position:sticky;top:0;z-index:5;background:var(--panel);border-bottom:1px solid var(--line);padding:10px 16px;display:flex;flex-wrap:wrap;gap:10px;align-items:center}
h1{font-size:17px;margin:0 8px 0 0;font-weight:600}
select,input,textarea,button{font:inherit;color:var(--text);background:var(--field);border:1px solid var(--line);border-radius:6px;padding:6px 8px}
button{cursor:pointer}button.primary{background:var(--accent);border-color:var(--accent);color:#fff;font-weight:600;padding:7px 16px}
nav{display:flex;flex-wrap:wrap;gap:4px;padding:10px 16px 0}nav button{border-radius:6px 6px 0 0;border-bottom:none;background:transparent;color:var(--dim)}
nav button.on{background:var(--panel);color:var(--text);border-color:var(--line)}
main{padding:0 16px 40px;max-width:980px}.card{background:var(--panel);border:1px solid var(--line);border-radius:0 8px 8px 8px;padding:6px 16px 12px}
.row{display:grid;grid-template-columns:minmax(150px,260px) 1fr;gap:6px 16px;padding:10px 0;border-bottom:1px solid var(--line);align-items:start}
.row:last-child{border-bottom:none}.row.off{opacity:.5}.lab b{font-weight:600}.hint{color:var(--dim);font-size:13px;margin-top:3px}
.badge{font-size:11px;border-radius:9px;padding:1px 7px;margin-left:6px;border:1px solid;vertical-align:middle}.live{color:var(--live);border-color:var(--live)}.restart{color:var(--warn);border-color:var(--warn)}
.banner{margin:10px 0;padding:10px 12px;border-radius:6px;border:1px solid var(--warn);color:var(--text);background:color-mix(in srgb,var(--warn) 14%,transparent)}
.banner.info{border-color:var(--live);background:color-mix(in srgb,var(--live) 12%,transparent)}.banner.bad{border-color:var(--accent);background:color-mix(in srgb,var(--accent) 14%,transparent)}
input[type=number],input[type=text]{width:12em}textarea{width:100%;min-height:70px}input[type=range]{width:min(260px,60%);vertical-align:middle;padding:0}
.status{color:var(--dim);font-size:13px;margin-left:auto}.small{font-size:13px;color:var(--dim)}label.chk{display:inline-flex;gap:6px;align-items:center}
[hidden]{display:none!important}.consent{position:fixed;inset:0;z-index:20;background:rgba(0,0,0,.6);display:flex;align-items:center;justify-content:center;padding:16px}
.consentbox{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:8px 20px 14px;max-width:560px}
@media (max-width:640px){.row{grid-template-columns:1fr}}
</style></head><body>
<header><h1>Bloodborne VR</h1>
<label>Profile <select id="profile"></select></label><span id="pnote" class="small"></span>
<span class="status" id="status"></span>
<label class="chk"><input type="checkbox" id="adv"> advanced</label>
<label class="chk"><input type="checkbox" id="showmenu" checked> show this menu next time</label>
<label class="chk" id="crashwrap" hidden><input type="checkbox" id="crashbox"> send crash reports</label>
<button class="primary" id="start">Start game</button><button id="quit">Quit</button></header>
<div id="consent" class="consent" hidden><div class="consentbox"><b>Help fix crashes?</b>
<p>If the game crashes, it can send an anonymous crash report to the developer: a small crash dump, the fault address, the build id and the last lines of the log, with your user name, folders, IP addresses and PSN ids removed. No game files, saves or settings. You can look at the report on your PC first, and change this later with the "send crash reports" box at the top of this page.</p>
<p><button class="primary" id="crashyes">Yes, send crash reports</button> <button id="crashno">No, keep them on my PC</button></p></div></div>
<nav id="tabs"></nav><main><div id="banners"></div><div class="card" id="card"></div></main>
<script>
const TOKEN="__TOKEN__";let S=null,tab=localStorage.getItem('bbvr_tab')||'display',saveTimer=null,values={};
const $=id=>document.getElementById(id);
const el=(t,c,txt)=>{const e=document.createElement(t);if(c)e.className=c;if(txt!==undefined)e.textContent=txt;return e};
async function api(path,body){const r=await fetch(path,{method:body?'POST':'GET',headers:{'X-BB-Token':TOKEN,'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined});if(!r.ok)throw new Error(r.status);return r.json()}
function applyState(s){S=s;values=Object.assign({},s.values);render()}
function renderCrash(){const c=S.crash||{available:false,asked:true};$('consent').hidden=!(c.available&&!c.asked);$('crashwrap').hidden=!(c.available&&c.asked);$('crashbox').checked=c.mode==='on'}
async function crashMode(m){try{applyState(await api('/api/crash',{mode:m}))}catch(e){$('status').textContent='save failed'}}
function dlssOK(){return S.caps.dlss.ok}
function schedule(){clearTimeout(saveTimer);saveTimer=setTimeout(async()=>{try{applyState(await api('/api/save',{values:values}))}catch(e){$('status').textContent='save failed'}},400)}
function mk(o){
 const v=values[o.id];let c;
 if(o.kind==='bool'){c=el('input');c.type='checkbox';c.checked=!!v;c.onchange=()=>{values[o.id]=c.checked;schedule()}}
 else if(o.kind==='choice'||o.kind==='runtime'){c=el('select');let list=[];
  if(o.kind==='runtime'){list=[['', 'System active runtime'+(S.caps.active_runtime?' ('+S.caps.active_runtime+')':'')]].concat(S.caps.runtimes.map(r=>[r.path,r.name+' ['+r.kind+']'+(r.active?' (active)':'')]))}
  else list=o.choices.map(x=>[x,(o.labels&&o.labels[x])||(x==='default'?'default (driver)':x)]);
  if(o.kind==='choice'&&v===null)list.unshift(['','(game default)']);
  for(const [val,txt] of list){const op=el('option',null,txt);op.value=val;if(o.id==='upscale'&&val==='dlss'&&!dlssOK()){op.disabled=true;op.textContent='DLSS (not available)'}c.appendChild(op)}
  c.value=v===null?'':String(v);c.onchange=()=>{values[o.id]=c.value;schedule()}}
 else if(o.kind==='multiline'){c=el('textarea');c.value=v||'';c.onchange=()=>{values[o.id]=c.value;schedule()}}
 else if(o.kind==='int'||o.kind==='float'){c=el('span');const n=el('input');n.type='number';n.step=o.step||1;if(o.min!==undefined)n.min=o.min;if(o.max!==undefined)n.max=o.max;n.value=v===null?'':v;n.placeholder='game default';
  n.onchange=()=>{values[o.id]=n.value===''?null:Number(n.value);schedule()};
  if(o.min!==undefined&&o.max!==undefined&&o.kind==='float'&&o.max-o.min<=10){const r=el('input');r.type='range';r.min=o.min;r.max=o.max;r.step=o.step||0.01;r.value=v===null?o.min:v;r.oninput=()=>{n.value=r.value};r.onchange=()=>{values[o.id]=Number(r.value);n.value=r.value;schedule()};c.appendChild(r);c.appendChild(document.createTextNode(' '))}
  c.appendChild(n)}
 else{c=el('span');const t=el('input');t.type='text';t.value=v===null?'':v;t.placeholder='default';if(o.suggestions){t.setAttribute('list','dl_'+o.id);const d=el('datalist');d.id='dl_'+o.id;o.suggestions.forEach(x=>{const op=el('option');op.value=x;d.appendChild(op)});c.appendChild(d)}
  t.onchange=()=>{values[o.id]=t.value===''?null:t.value;schedule()};c.appendChild(t)}
 return c}
function dependsOff(o){if(o.only_if){for(const k in o.only_if){const cur=values[k];if(cur!==o.only_if[k])return 'only with '+k+' = '+o.only_if[k]}}return ''}
function render(){
 $('profile').innerHTML='';S.profiles.forEach(p=>{const op=el('option',null,p);op.value=p;$('profile').appendChild(op)});$('profile').value=S.profile;
 renderCrash();$('pnote').textContent=S.profile_notes[S.profile]||'';$('showmenu').checked=S.show_menu;$('status').textContent=S.status||'';
 const tabs=$('tabs');tabs.innerHTML='';S.tabs.forEach(t=>{const b=el('button',t.id===tab?'on':'',t.label);b.onclick=()=>{tab=t.id;try{localStorage.setItem('bbvr_tab',tab)}catch(e){};render()};tabs.appendChild(b)});
 const bn=$('banners');bn.innerHTML='';(S.warnings||[]).forEach(w=>bn.appendChild(el('div','banner',w)));
 if(S.game_running)bn.appendChild(el('div','banner info','The game is running. Live options (blue badge) apply within a second; the others need a restart.'));
 const card=$('card');card.innerHTML='';
 if(tab==='dlss'){
  if(!dlssOK())card.appendChild(el('div','banner bad','DLSS is greyed out: '+S.caps.dlss.reason));
  else card.appendChild(el('div','banner info','DLSS is available'+(S.dlss_native?'.':' (using the DLSS build in out/dlss-vr until the main executable is rebuilt).')));
  card.appendChild(el('div','banner info',S.dlss_note));
  if(S.game_running&&!S.dlss_native)card.appendChild(el('div','hint','To switch to DLSS while playing, the game must have been started with Upscaler = DLSS (it then uses the DLSS-capable executable).'));
 }
 const adv=$('adv').checked;
 S.options.filter(o=>o.tab===tab&&(adv||!o.advanced)).forEach(o=>{
  const off=dependsOff(o)||((o.id==='upscale'||o.only_if)&&!dlssOK()&&o.only_if&&o.only_if.upscale==='dlss'?'DLSS not available':'');
  const row=el('div','row'+(off?' off':''));const lab=el('div','lab');const b=el('b',null,o.label);lab.appendChild(b);
  lab.appendChild(el('span','badge '+(o.live?'live':'restart'),o.live?'live':'restart'));
  if(o.help)lab.appendChild(el('div','hint',o.help));if(off)lab.appendChild(el('div','hint',off));
  row.appendChild(lab);const c=mk(o);if(o.kind==='runtime'){const w=el('div');w.appendChild(c);row.appendChild(w)}else row.appendChild(c);card.appendChild(row)});
 if(!card.querySelector('.row'))card.appendChild(el('div','hint','Nothing here (tick "advanced" for more).'));
}
$('adv').onchange=render;
$('crashyes').onclick=()=>crashMode('on');$('crashno').onclick=()=>crashMode('off');
$('crashbox').onchange=()=>crashMode($('crashbox').checked?'on':'off');
$('profile').onchange=async()=>{applyState(await api('/api/save',{profile:$('profile').value}))};
$('showmenu').onchange=async()=>{applyState(await api('/api/save',{show_menu:$('showmenu').checked}))};
$('start').onclick=async()=>{$('start').disabled=true;try{applyState(await api('/api/launch',{values:values}));$('status').textContent='Starting the game - put the headset on. You can keep changing live options here.'}catch(e){$('start').disabled=false}};
$('quit').onclick=async()=>{try{await api('/api/quit',{})}catch(e){};document.body.textContent='You can close this window.'};
api('/api/state').then(applyState).catch(()=>{document.body.textContent='This page needs the launcher running.'});
setInterval(async()=>{if(!S||saveTimer)return;try{const s=await api('/api/state');S.game_running=s.game_running;if(s.status!==S.status){S.status=s.status;$('status').textContent=s.status}}catch(e){}},3000);
</script></body></html>
"""
