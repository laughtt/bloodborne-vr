#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Crash reports: collect, scrub, consent, upload. Standard library only; used by run.py.

Nothing leaves the PC unless the player agreed (crash_reports = "on", or "ask" answered with y)
and a crash_server_url is set. See docs/CRASH_REPORTS.md.
Config: <user dir>/crash_config.json {"crash_reports": "ask"|"on"|"off", "crash_server_url": "",
"upload_token": ""}; env BB_CRASH_REPORTS, BB_CRASH_SERVER_URL, BB_CRASH_TOKEN override it.
"""
import getpass
import io
import json
import os
from pathlib import Path
import re
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zipfile

LOG_LINES = 500
MAX_ZIP = 18 * 1024 * 1024          # the server accepts 20 MB
UPLOAD_TIMEOUT = 4                  # seconds per attempt
UPLOAD_BUDGET = 8                   # seconds for all attempts of one report
MODES = ('ask', 'on', 'off')
# The only files a bundle may hold; game files are never added.
BUNDLE_FILES = ('crash.json', 'crash.dmp')


# ---------------------------------------------------------------- scrubbing

_IPV4 = re.compile(r'(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])')
_IPV6 = re.compile(r'(?<![\w:])(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}(?![\w:])')
_USER_PATH = re.compile(r'(?i)(?P<root>(?:[A-Za-z]:|/[A-Za-z]|/mnt/[A-Za-z]|\\\\\?\\[A-Za-z]:)?[\\/]+Users[\\/]+)(?P<name>[^\\/\s"\'<>|:*?]+)')
# key=value / key: value for ids that identify a person or an account.
_ID_KEY = re.compile(
    r'(?i)(?P<key>"?(?:online[_ ]?id|account[_ ]?id|np[_ ]?id|npid|psn[\w ]*id|user[_ ]?id|user[_ ]?name|'
    r'ticket|access[_ ]?token|refresh[_ ]?token|auth[\w-]*|password|passwd|secret|api[_ ]?key|token)"?\s*[=:]\s*"?)'
    r'(?P<value>[^\s",;&}\]]+)')
_HEX_ACCOUNT = re.compile(r'(?i)\b[0-9a-f]{16}\b(?=.*(?:account|npid|psn))')


def scrub_context():
    """Names to remove besides the patterns: the Windows user, the computer and the home folder."""
    names = set()
    for source in (os.environ.get('USERNAME'), os.environ.get('USER'), os.environ.get('LOGNAME')):
        if source:
            names.add(source)
    try:
        names.add(getpass.getuser())
    except Exception:
        pass
    host = os.environ.get('COMPUTERNAME') or socket.gethostname()
    return {'users': sorted((n for n in names if len(n) >= 2), key=len, reverse=True),
            'hosts': [host] if host and len(host) >= 3 else []}


def scrub_text(text, context=None):
    """Removes user paths, the user name, the PC name, PSN/NP ids, tokens and IP addresses."""
    context = context or scrub_context()
    text = _USER_PATH.sub('%USER%', text)
    for name in context['users']:
        text = re.sub(rf'(?<![A-Za-z0-9]){re.escape(name)}(?![A-Za-z0-9])', '%USERNAME%', text)
    for host in context['hosts']:
        text = re.sub(rf'(?i)(?<![A-Za-z0-9]){re.escape(host)}(?![A-Za-z0-9])', '%HOST%', text)
    text = _ID_KEY.sub(lambda m: m.group('key') + '<redacted>', text)
    text = _HEX_ACCOUNT.sub('<redacted-id>', text)
    text = _IPV4.sub('<ip>', text)
    text = _IPV6.sub(lambda m: '<ip>' if ('::' in m.group(0) or m.group(0).count(':') >= 7) else m.group(0), text)
    return text


def scrub_bytes(data, context=None):
    """Same-length scrub of a binary dump: the user name (ASCII and UTF-16) is overwritten with x."""
    context = context or scrub_context()
    for name in context['users'] + context['hosts']:
        for encoding in ('ascii', 'utf-16-le'):
            try:
                raw = name.encode(encoding)
            except UnicodeEncodeError:
                continue
            for variant in {raw, name.lower().encode(encoding), name.upper().encode(encoding)}:
                if len(variant) >= 3 and variant in data:
                    data = data.replace(variant, b'x' * len(variant) if encoding == 'ascii'
                                        else b'x\0' * (len(variant) // 2))
    return data


# ---------------------------------------------------------------- config

def config_path(user_dir):
    return Path(user_dir) / 'crash_config.json'


def load_config(user_dir, env=None):
    env = os.environ if env is None else env
    config = {'crash_reports': 'ask', 'crash_server_url': '', 'upload_token': ''}
    try:
        data = json.loads(config_path(user_dir).read_text(encoding='utf-8'))
        if isinstance(data, dict):
            config.update({k: v for k, v in data.items() if k in config and isinstance(v, str)})
    except (OSError, ValueError):
        pass
    for key, variable in (('crash_reports', 'BB_CRASH_REPORTS'), ('crash_server_url', 'BB_CRASH_SERVER_URL'),
                          ('upload_token', 'BB_CRASH_TOKEN')):
        if env.get(variable):
            config[key] = env[variable]
    if config['crash_reports'] not in MODES:
        config['crash_reports'] = 'ask'
    return config


def save_mode(user_dir, mode):
    path = config_path(user_dir)
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(data, dict):
            data = {}
    except (OSError, ValueError):
        data = {}
    data.setdefault('crash_server_url', '')
    data.setdefault('upload_token', '')
    data['crash_reports'] = mode
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    except OSError:
        pass


def ask_consent(user_dir, input_fn=input, interactive=None):
    """First time only: asks on the console, saves on/off. Returns the mode ('ask' when unanswered)."""
    if interactive is None:
        interactive = bool(sys.stdin and sys.stdin.isatty())
    if not interactive:
        return 'ask'
    print()
    print('The game ended unexpectedly. Send an anonymous crash report to the developer?')
    print('It holds a small crash dump, the fault address, the build id and the last lines of the log,')
    print('with your user name, folders, IP addresses and PSN ids removed. No game files.')
    print('Your answer is remembered (change it in', config_path(user_dir), ').')
    try:
        answer = input_fn('Send crash reports automatically from now on? [y/N] ').strip().lower()
    except (EOFError, KeyboardInterrupt):
        return 'ask'
    mode = 'on' if answer in ('y', 'yes') else 'off'
    save_mode(user_dir, mode)
    return mode


# ---------------------------------------------------------------- bundle

def first_error_line(lines):
    for line in lines:
        if re.search(r'(?i)\b(fault|error|fatal|exception|assert|abort|panic|hang)\b', line):
            return line.strip()[:300]
    return ''


def build_bundle(crash_dirs, log_lines, exit_code, build_id='unknown', context=None):
    """Returns (zip bytes, summary dict). Everything text is scrubbed; dump names are overwritten."""
    context = context or scrub_context()
    log_tail = scrub_text('\n'.join(log_lines[-LOG_LINES:]), context)
    summary = {'build': build_id, 'exit_code': exit_code, 'os': sys.platform, 'time': time.strftime('%Y-%m-%dT%H:%M:%S')}
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
        for index, crash_dir in enumerate(crash_dirs[:3]):
            prefix = f'crash{index}/'
            info_path = Path(crash_dir) / 'crash.json'
            try:
                info = json.loads(info_path.read_text(encoding='utf-8'))
                summary['build'] = info.get('build', summary['build'])
                for key in ('kind', 'fault_module', 'fault_offset', 'exception_code'):
                    if key in info and index == 0:
                        summary[key] = info[key]
                archive.writestr(prefix + 'crash.json', scrub_text(json.dumps(info, indent=2), context))
            except (OSError, ValueError):
                pass
            dump = Path(crash_dir) / 'crash.dmp'
            try:
                if dump.is_file() and dump.stat().st_size < 12 * 1024 * 1024:
                    archive.writestr(prefix + 'crash.dmp', scrub_bytes(dump.read_bytes(), context))
            except OSError:
                pass
        summary['first_error'] = first_error_line(log_tail.splitlines())
        archive.writestr('log_tail.txt', log_tail + '\n')
        archive.writestr('meta.json', json.dumps(summary, indent=2) + '\n')
    data = buffer.getvalue()
    if len(data) > MAX_ZIP:  # too big: the log and the summary are enough
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('log_tail.txt', log_tail + '\n')
            archive.writestr('meta.json', json.dumps(summary, indent=2) + '\n')
        data = buffer.getvalue()
    return data, summary


# ---------------------------------------------------------------- upload

def multipart(fields, filename, payload):
    boundary = uuid.uuid4().hex
    body = io.BytesIO()
    for key, value in fields.items():
        body.write(f'--{boundary}\r\nContent-Disposition: form-data; name="{key}"\r\n\r\n{value}\r\n'.encode())
    body.write((f'--{boundary}\r\nContent-Disposition: form-data; name="report"; filename="{filename}"\r\n'
                'Content-Type: application/zip\r\n\r\n').encode())
    body.write(payload)
    body.write(f'\r\n--{boundary}--\r\n'.encode())
    return body.getvalue(), f'multipart/form-data; boundary={boundary}'


def url_allowed(url):
    """https only; plain http just for the local machine (tests, a server on the same PC)."""
    parts = urllib.parse.urlsplit(url)
    if parts.scheme == 'https' and parts.hostname:
        return True
    return parts.scheme == 'http' and parts.hostname in ('127.0.0.1', 'localhost', '::1')


def upload(url, token, payload, summary, timeout=UPLOAD_TIMEOUT, budget=UPLOAD_BUDGET, retries=2):
    """POST the zip. True on success. Never raises; gives up after `budget` seconds."""
    if not url or not url_allowed(url):
        return False
    target = url.rstrip('/') + '/upload'
    body, content_type = multipart({'build': str(summary.get('build', '')),
                                    'fault_module': str(summary.get('fault_module', '')),
                                    'fault_offset': str(summary.get('fault_offset', '')),
                                    'first_error': str(summary.get('first_error', ''))}, 'report.zip', payload)
    deadline = time.monotonic() + budget
    for attempt in range(retries + 1):
        left = deadline - time.monotonic()
        if left <= 0.5:
            break
        request = urllib.request.Request(target, data=body, method='POST', headers={
            'Content-Type': content_type, 'Authorization': f'Bearer {token}', 'User-Agent': 'bb-crash-report/1'})
        try:
            with urllib.request.urlopen(request, timeout=min(timeout, left)) as response:
                if 200 <= response.status < 300:
                    return True
        except urllib.error.HTTPError as error:
            if error.code in (400, 401, 403, 413):  # retrying cannot help
                return False
        except (OSError, ValueError):
            pass
        time.sleep(min(0.5 * (attempt + 1), max(0, deadline - time.monotonic())))
    return False


# ---------------------------------------------------------------- run.py glue

def crash_dirs_in(user_dir):
    root = Path(user_dir) / 'crashes'
    try:
        return {p for p in root.iterdir() if p.is_dir()}
    except OSError:
        return set()


def build_id_from_log(lines):
    for line in lines:
        match = re.match(r'BB_BUILD_ID=(\S+)', line)
        if match:
            return match.group(1)
    return 'unknown'


def handle_exit(user_dir, before, log_lines, exit_code, env=None, input_fn=input, interactive=None):
    """After the game ended: nonzero exit or a new crash folder -> bundle, consent, upload.
    Returns the saved bundle path or None. Never raises."""
    try:
        return _handle_exit(user_dir, before, log_lines, exit_code, env, input_fn, interactive)
    except Exception as error:  # a reporting problem must never disturb the player
        print(f'Crash report skipped: {error}', file=sys.stderr)
        return None


def _handle_exit(user_dir, before, log_lines, exit_code, env, input_fn, interactive):
    config = load_config(user_dir, env)
    if config['crash_reports'] == 'off':
        return None
    new = sorted(crash_dirs_in(user_dir) - set(before))
    if not new and exit_code in (0, None):
        return None
    if exit_code in (130,):  # Ctrl+C
        return None
    payload, summary = build_bundle(new, log_lines, exit_code, build_id_from_log(log_lines))
    folder = new[0] if new else Path(user_dir) / 'crashes' / (time.strftime('%Y%m%d-%H%M%S-exit-') + uuid.uuid4().hex[:4])
    folder.mkdir(parents=True, exist_ok=True)
    saved = folder / 'report.zip'
    saved.write_bytes(payload)
    print(f'Crash report saved: {saved}')
    mode = config['crash_reports']
    if mode == 'ask':
        mode = ask_consent(user_dir, input_fn, interactive)
    if mode != 'on':
        return saved
    if not config['crash_server_url']:
        print('No crash server is configured (crash_server_url); the report stays on this PC.')
        return saved
    if upload(config['crash_server_url'], config['upload_token'], payload, summary):
        (folder / 'report.sent').write_text(time.strftime('%Y-%m-%dT%H:%M:%S'), encoding='utf-8')
        print('Crash report sent. Thank you!')
    else:
        print('Crash report could not be sent now; it stays on this PC.')
    return saved
