#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared QuestDB ILP (InfluxDB line protocol) helper for the day-one recorder.

format_line() builds one line; IlpSender batches lines over TCP (default 127.0.0.1:9009),
reconnects with a back-off, and appends to a spool file when QuestDB is unreachable so no row is
lost. replay_spool() pushes a spool file later. Stdlib only.
"""
import json
import os
import queue
import socket
import threading
import time
import urllib.parse
import urllib.request

HOST, PORT, HTTP_PORT = '127.0.0.1', 9009, 9000


def data_dir(sub, default):
    """$BBVR_STORE_DIR/<sub> when BBVR_STORE_DIR is set, otherwise the repo-relative <default>."""
    root = os.environ.get('BBVR_STORE_DIR')
    return os.path.join(root, sub) if root else default
BS = '\\'


def _esc(s, chars):
    s = str(s).replace(BS, BS + BS)
    for c in chars:
        s = s.replace(c, BS + c)
    return s.replace('\n', BS + 'n').replace('\r', BS + 'r')


def esc_name(s):      # measurement (table) name
    return _esc(s, ', ')


def esc_key(s):       # tag key, tag value, field key
    return _esc(s, ',= ')


def esc_str(s):       # string field value, written inside double quotes
    return _esc(s, '"')


def field_value(v):
    if isinstance(v, bool):
        return 't' if v else 'f'
    if isinstance(v, int):
        return f'{v}i'
    if isinstance(v, float):
        return repr(v) if v == v and v not in (float('inf'), float('-inf')) else '0.0'
    return '"' + esc_str(v) + '"'


def format_line(table, tags, fields, ts_ns):
    """tags: dict of str -> str (SYMBOLs); fields: dict (int -> 'i', float, bool, str);
    ts_ns: designated timestamp in nanoseconds. Fields set to None are omitted."""
    head = esc_name(table) + ''.join(f',{esc_key(k)}={esc_key(v)}' for k, v in tags.items())
    body = ','.join(f'{esc_key(k)}={field_value(v)}' for k, v in fields.items() if v is not None)
    return f'{head} {body} {int(ts_ns)}\n'


class IlpSender:
    def __init__(self, host=HOST, port=PORT, spool_path=None, dry=False, log=None):
        self.host, self.port, self.spool_path, self.dry = host, port, spool_path, dry
        self.log = log or (lambda m: print(m, flush=True))
        self.sock = None
        self.retry_at = 0.0
        self.lock = threading.Lock()
        self.q = queue.Queue()
        self.thread = None
        self.sent = self.spooled = 0
        self._warned = False

    def _connect(self):
        now = time.monotonic()
        if now < self.retry_at:
            return False
        try:
            s = socket.create_connection((self.host, self.port), timeout=0.5)
            s.settimeout(2.0)
            self.sock = s
            if self._warned:
                self.log(f'ilp: connected to {self.host}:{self.port}')
            self._warned = False
            return True
        except OSError as e:
            self.retry_at = now + 2.0
            if not self._warned:
                self.log(f'ilp: cannot reach {self.host}:{self.port} ({e}); spooling to {self.spool_path}')
                self._warned = True
            return False

    def _spool(self, data):
        if not self.spool_path:
            return
        os.makedirs(os.path.dirname(os.path.abspath(self.spool_path)), exist_ok=True)
        with open(self.spool_path, 'ab') as f:
            f.write(data)
        self.spooled += data.count(b'\n')

    def send(self, lines):
        """Queue lines (each ending in newline) for the writer thread. Never blocks on the network,
        never raises, never drops: anything that cannot be sent goes to the spool file."""
        if not lines:
            return True
        data = ''.join(lines).encode('utf-8')
        if self.dry:
            print(data.decode('utf-8'), end='', flush=True)
            return True
        self.q.put((data, len(lines)))
        if self.thread is None:
            with self.lock:
                if self.thread is None:
                    self.thread = threading.Thread(target=self._writer, daemon=True)
                    self.thread.start()
        return True

    def _writer(self):
        while True:
            item = self.q.get()
            if item is None:
                self.q.task_done()
                return
            data, n = item
            try:
                if self.sock is None and not self._connect():
                    self._spool(data)
                else:
                    try:
                        self.sock.sendall(data)
                        self.sent += n
                    except OSError:
                        self._drop_sock()
                        self._spool(data)   # a partial send may have landed; replay can duplicate
            finally:
                self.q.task_done()

    def _drop_sock(self):
        try:
            self.sock.close()
        except OSError:
            pass
        self.sock, self.retry_at = None, time.monotonic() + 1.0

    def close(self):
        """Flush the queue, then close."""
        if self.thread is not None:
            self.q.put(None)
            self.thread.join(timeout=10)
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None


def replay_spool(path, host=HOST, port=PORT, chunk=2000, log=print):
    """Push a spool file to QuestDB; the file is renamed to .sent on success."""
    n = 0
    with socket.create_connection((host, port), timeout=3) as s, open(path, 'rb') as f:
        buf = []
        for ln in f:
            buf.append(ln)
            if len(buf) >= chunk:
                s.sendall(b''.join(buf))
                n += len(buf)
                buf = []
        if buf:
            s.sendall(b''.join(buf))
            n += len(buf)
    os.replace(path, path + '.sent')
    log(f'replayed {n} lines from {path}')
    return n


def sql(query, host=HOST, port=HTTP_PORT, timeout=5):
    url = f'http://{host}:{port}/exec?' + urllib.parse.urlencode({'query': query})
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode('utf-8'))


def git_rev(repo=None):
    import subprocess
    try:
        out = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=repo, capture_output=True,
                             text=True, timeout=3)
        return out.stdout.strip() if out.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None
