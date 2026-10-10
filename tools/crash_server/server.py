#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Crash report collector for Bloodborne VR testers. One file, Python standard library only
(no framework: it has two endpoints, and a stdlib server has nothing to patch or install).

  POST /upload   multipart/form-data, field "report" = zip; Authorization: Bearer <upload token>
  GET  /         HTML list grouped by build and fault; admin token (Bearer header, ?token=, or HTTP
                 Basic with the token as password, so a browser can log in)
  GET  /report/<id>.zip   download one report (admin token)

Settings (environment or flags): BB_CRASH_UPLOAD_TOKEN, BB_CRASH_ADMIN_TOKEN, BB_CRASH_DATA (data),
BB_CRASH_RETENTION_DAYS (60), BB_CRASH_MAX_BYTES (20 MiB), BB_CRASH_RATE (uploads per IP per hour, 30).
Run behind a TLS reverse proxy (see README.md); the proxy's X-Forwarded-For is used for rate limiting
only with --trust-proxy.
"""
import argparse
import base64
import collections
import email
import email.policy
import hmac
import html
import io
import json
import os
from pathlib import Path
import re
import shutil
import threading
import time
import uuid
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

SAFE = re.compile(r'[^A-Za-z0-9._-]')


def safe(text, default='unknown'):
    return SAFE.sub('_', str(text))[:64] or default


class Store:
    def __init__(self, root, retention_days, max_bytes, rate_per_hour):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.index = self.root / 'index.jsonl'
        self.retention_days = retention_days
        self.max_bytes = max_bytes
        self.rate = rate_per_hour
        self.lock = threading.Lock()
        self.hits = collections.defaultdict(list)

    def allow(self, client):
        now = time.time()
        with self.lock:
            hits = [t for t in self.hits[client] if now - t < 3600]
            self.hits[client] = hits
            if len(hits) >= self.rate:
                return False
            hits.append(now)
            return True

    def save(self, payload, fields):
        """Validates the zip, stores it under <build>/<date>/<id>.zip and appends to the index."""
        try:
            with zipfile.ZipFile(io.BytesIO(payload)) as archive:
                if archive.testzip() is not None:
                    raise ValueError('corrupt zip')
                meta = json.loads(archive.read('meta.json'))
                log = archive.read('log_tail.txt').decode('utf-8', 'replace') if 'log_tail.txt' in archive.namelist() else ''
        except (zipfile.BadZipFile, KeyError, ValueError):
            raise ValueError('not a crash report zip')
        build = safe(meta.get('build') or fields.get('build'))
        module = str(meta.get('fault_module') or fields.get('fault_module') or '')
        offset = str(meta.get('fault_offset') or fields.get('fault_offset') or '')
        first_error = str(meta.get('first_error') or '')
        if not first_error:
            for line in log.splitlines():
                if re.search(r'(?i)\b(fault|error|fatal|exception|assert|abort|panic)\b', line):
                    first_error = line.strip()
                    break
        report_id = uuid.uuid4().hex[:16]
        day = time.strftime('%Y-%m-%d')
        folder = self.root / build / day
        folder.mkdir(parents=True, exist_ok=True)
        (folder / f'{report_id}.zip').write_bytes(payload)
        entry = {'id': report_id, 'time': time.strftime('%Y-%m-%dT%H:%M:%S'), 'build': build, 'day': day,
                 'kind': str(meta.get('kind', 'exit')), 'exit_code': meta.get('exit_code'),
                 'fault_module': module[:120], 'fault_offset': offset[:40], 'first_error': first_error[:300],
                 'size': len(payload)}
        with self.lock, self.index.open('a', encoding='utf-8') as out:
            out.write(json.dumps(entry) + '\n')
        return entry

    def entries(self):
        try:
            lines = self.index.read_text(encoding='utf-8').splitlines()
        except OSError:
            return []
        result = []
        for line in lines:
            try:
                result.append(json.loads(line))
            except ValueError:
                pass
        return result

    def path_of(self, report_id):
        for entry in self.entries():
            if entry['id'] == report_id:
                path = self.root / entry['build'] / entry['day'] / f'{report_id}.zip'
                return path if path.is_file() else None
        return None

    def cleanup(self):
        """Removes reports older than the retention and rewrites the index without them."""
        if self.retention_days <= 0:
            return 0
        cutoff = time.strftime('%Y-%m-%d', time.localtime(time.time() - self.retention_days * 86400))
        removed = 0
        with self.lock:
            keep = []
            for entry in self.entries():
                if entry['day'] < cutoff:
                    path = self.root / entry['build'] / entry['day'] / f"{entry['id']}.zip"
                    try:
                        path.unlink()
                    except OSError:
                        pass
                    removed += 1
                else:
                    keep.append(entry)
            if removed:
                self.index.write_text(''.join(json.dumps(e) + '\n' for e in keep), encoding='utf-8')
            for day_dir in self.root.glob('*/*'):
                if day_dir.is_dir() and not any(day_dir.iterdir()):
                    shutil.rmtree(day_dir, ignore_errors=True)
        return removed


def render_index(entries):
    groups = collections.defaultdict(lambda: collections.defaultdict(list))
    for entry in entries:
        signature = f"{entry.get('fault_module') or '-'}+{entry.get('fault_offset') or '-'}" \
            if entry.get('fault_module') else f"exit {entry.get('exit_code')}: {entry.get('first_error', '')[:80]}"
        groups[entry['build']][signature].append(entry)
    parts = ['<!doctype html><html lang="en"><head><meta charset="utf-8">'
             '<meta name="viewport" content="width=device-width, initial-scale=1"><title>Crash reports</title><style>'
             ':root{color-scheme:light dark;--bg:#fff;--fg:#1d1d1f;--line:#ccc;--code:#eee}'
             '@media(prefers-color-scheme:dark){:root{--bg:#111214;--fg:#e8e8ea;--line:#33343a;--code:#24252b}}'
             'body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif}'
             'h1{font-size:1.4rem}h2{font-size:1.1rem;margin-top:1.6em}h3{font-size:.95rem;margin:1em 0 .3em}'
             'code{background:var(--code);padding:1px 4px;border-radius:4px}'
             'table{border-collapse:collapse;width:100%}th,td{border:1px solid var(--line);padding:4px 8px;text-align:left}'
             '.w{overflow-x:auto}</style></head><body><h1>Crash reports</h1>'
             f'<p>{len(entries)} reports.</p>']
    for build in sorted(groups, reverse=True):
        count = sum(len(v) for v in groups[build].values())
        parts.append(f'<h2>Build <code>{html.escape(build)}</code> ({count})</h2>')
        for signature, items in sorted(groups[build].items(), key=lambda kv: -len(kv[1])):
            parts.append(f'<h3><code>{html.escape(signature)}</code> x{len(items)}</h3><div class="w"><table>'
                         '<tr><th>Time</th><th>Kind</th><th>First error</th><th>Size</th><th></th></tr>')
            for e in sorted(items, key=lambda e: e['time'], reverse=True):
                parts.append(f"<tr><td>{html.escape(e['time'])}</td><td>{html.escape(e['kind'])}</td>"
                             f"<td>{html.escape(e.get('first_error', ''))}</td><td>{e['size'] // 1024} KB</td>"
                             f"<td><a href=\"/report/{e['id']}.zip\">zip</a></td></tr>")
            parts.append('</table></div>')
    parts.append('</body></html>')
    return ''.join(parts).encode('utf-8')


def make_handler(store, upload_token, admin_token, trust_proxy):
    class Handler(BaseHTTPRequestHandler):
        server_version = 'bb-crash/1'

        def log_message(self, fmt, *args):  # no query strings (tokens) in the log
            print('%s %s' % (self.address_string(), fmt % args if args else fmt), flush=True)

        def reply(self, code, body=b'', content_type='text/plain; charset=utf-8', extra=()):
            if isinstance(body, str):
                body = body.encode()
            self.send_response(code)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            for key, value in extra:
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)

        def token_given(self):
            header = self.headers.get('Authorization', '')
            if header.startswith('Bearer '):
                return header[7:].strip()
            if header.startswith('Basic '):
                try:
                    return base64.b64decode(header[6:]).decode().partition(':')[2]
                except ValueError:
                    return ''
            return parse_qs(urlsplit(self.path).query).get('token', [''])[0]

        def authorized(self, expected):
            return bool(expected) and hmac.compare_digest(self.token_given().encode(), expected.encode())

        def client(self):
            if trust_proxy and self.headers.get('X-Forwarded-For'):
                return self.headers['X-Forwarded-For'].split(',')[-1].strip()
            return self.client_address[0]

        def do_POST(self):
            if urlsplit(self.path).path != '/upload':
                return self.reply(404, 'not found')
            if not self.authorized(upload_token):
                return self.reply(401, 'bad token')
            if not store.allow(self.client()):
                return self.reply(429, 'too many uploads')
            length = int(self.headers.get('Content-Length') or 0)
            if length <= 0 or length > store.max_bytes:
                return self.reply(413, 'too large')
            ctype = self.headers.get('Content-Type', '')
            if not ctype.startswith('multipart/form-data'):
                return self.reply(400, 'multipart expected')
            body = self.rfile.read(length)
            message = email.message_from_bytes(b'Content-Type: ' + ctype.encode('latin-1') + b'\r\n\r\n' + body,
                                               policy=email.policy.default)
            fields, payload = {}, None
            for part in message.iter_parts():
                name = part.get_param('name', header='content-disposition')
                if name == 'report':
                    payload = part.get_payload(decode=True)
                elif name:
                    fields[name] = part.get_content() if part.get_content_maintype() == 'text' else ''
            if not payload:
                return self.reply(400, 'report missing')
            try:
                entry = store.save(payload, fields)
            except ValueError as error:
                return self.reply(400, str(error))
            self.reply(200, json.dumps({'id': entry['id']}), 'application/json')

        def do_GET(self):
            path = urlsplit(self.path).path
            if path == '/health':
                return self.reply(200, 'ok')
            if not self.authorized(admin_token):
                return self.reply(401, 'admin token required', extra=[('WWW-Authenticate', 'Basic realm="crash reports"')])
            if path == '/':
                return self.reply(200, render_index(store.entries()), 'text/html; charset=utf-8')
            match = re.fullmatch(r'/report/([0-9a-f]{16})\.zip', path)
            if match:
                file = store.path_of(match.group(1))
                if file:
                    return self.reply(200, file.read_bytes(), 'application/zip',
                                      [('Content-Disposition', f'attachment; filename="{match.group(1)}.zip"')])
            self.reply(404, 'not found')

    return Handler


def make_server(host, port, data, upload_token, admin_token, retention_days=60, max_bytes=20 * 1024 * 1024,
                rate=30, trust_proxy=False):
    store = Store(data, retention_days, max_bytes, rate)
    server = ThreadingHTTPServer((host, port), make_handler(store, upload_token, admin_token, trust_proxy))
    server.store = store
    return server


def main():
    env = os.environ.get
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--host', default=env('BB_CRASH_HOST', '127.0.0.1'))
    parser.add_argument('--port', type=int, default=int(env('PORT', '8787')))
    parser.add_argument('--data', default=env('BB_CRASH_DATA', 'data'))
    parser.add_argument('--retention-days', type=int, default=int(env('BB_CRASH_RETENTION_DAYS', '60')))
    parser.add_argument('--max-bytes', type=int, default=int(env('BB_CRASH_MAX_BYTES', str(20 * 1024 * 1024))))
    parser.add_argument('--rate', type=int, default=int(env('BB_CRASH_RATE', '30')))
    parser.add_argument('--trust-proxy', action='store_true')
    args = parser.parse_args()
    upload_token, admin_token = env('BB_CRASH_UPLOAD_TOKEN', ''), env('BB_CRASH_ADMIN_TOKEN', '')
    if len(upload_token) < 16 or len(admin_token) < 16:
        raise SystemExit('Set BB_CRASH_UPLOAD_TOKEN and BB_CRASH_ADMIN_TOKEN (16+ characters each).')
    server = make_server(args.host, args.port, args.data, upload_token, admin_token, args.retention_days,
                         args.max_bytes, args.rate, args.trust_proxy)

    def sweep():
        while True:
            server.store.cleanup()
            time.sleep(6 * 3600)
    threading.Thread(target=sweep, daemon=True).start()
    print(f'Crash server on http://{args.host}:{args.port}, data {Path(args.data).resolve()}, '
          f'retention {args.retention_days} days', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
