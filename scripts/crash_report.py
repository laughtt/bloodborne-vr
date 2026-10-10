#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Crash reports: collect, scrub, save locally. Standard library only; used by run.py.

Nothing is ever sent anywhere: after a crash the report is written to a private per-user folder
(%LOCALAPPDATA%/BloodborneVR/crashes, or BB_CRASH_DIR) and the player is told where it is, so they can send
the file to the developer if they want to. See docs/CRASH_REPORTS.md.
"""
import getpass
import io
import json
import os
from pathlib import Path
import re
import shutil
import socket
import sys
import time
import uuid
import zipfile

LOG_LINES = 500
MAX_ZIP = 18 * 1024 * 1024          # a report stays small enough to send by e-mail / chat
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


# ---------------------------------------------------------------- where reports live

def store_dir(env=None):
    """The private folder of the reports: %LOCALAPPDATA%/BloodborneVR/crashes (installed and portable builds)."""
    env = os.environ if env is None else env
    if env.get('BB_CRASH_DIR'):
        return Path(env['BB_CRASH_DIR'])
    base = env.get('LOCALAPPDATA') or env.get('XDG_DATA_HOME') or str(Path.home() / 'AppData' / 'Local')
    return Path(base) / 'BloodborneVR' / 'crashes'


def list_reports(env=None):
    """Saved report.zip files, oldest first."""
    try:
        return sorted(p for p in store_dir(env).glob('*/report.zip') if p.is_file())
    except OSError:
        return []


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


def handle_exit(user_dir, before, log_lines, exit_code, env=None):
    """After the game ended: nonzero exit or a new crash folder -> scrubbed report.zip in store_dir(), a plain
    message for the player. The raw (unscrubbed) dump folders the game wrote are removed afterwards.
    Returns the saved path or None. Never raises, never touches the network."""
    try:
        return _handle_exit(user_dir, before, log_lines, exit_code, env)
    except Exception as error:  # a reporting problem must never disturb the player
        print(f'Crash report skipped: {error}', file=sys.stderr)
        return None


def _handle_exit(user_dir, before, log_lines, exit_code, env):
    new = sorted(crash_dirs_in(user_dir) - set(before))
    if not new and exit_code in (0, None):
        return None
    if exit_code in (130,):  # Ctrl+C
        return None
    payload, summary = build_bundle(new, log_lines, exit_code, build_id_from_log(log_lines))
    folder = store_dir(env) / (time.strftime('%Y%m%d-%H%M%S-') + uuid.uuid4().hex[:4])
    folder.mkdir(parents=True, exist_ok=True)
    saved = folder / 'report.zip'
    saved.write_bytes(payload)
    for raw in new:  # the scrubbed copy is in the report; do not leave the raw dump lying around
        shutil.rmtree(raw, ignore_errors=True)
    print(f'The game crashed. A crash report was saved to {saved}. '
          'If you want to help, send that file to the developer.')
    return saved
