#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Minimal obs-websocket v5 client (hand-written WebSocket over a socket) for the day-one recorder.

    python tools/bbvr_obs_ctl.py start  [--session S]   StartRecord, stamp qpc/wall around it
    python tools/bbvr_obs_ctl.py stop   [--session S]
    python tools/bbvr_obs_ctl.py status
    python tools/bbvr_obs_ctl.py watch  [--session S]   log RecordStateChanged / RecordFileChanged

Default ws://127.0.0.1:4455, password from --password or env OBS_WS_PASSWORD. Events and requests
are written to QuestDB table bbvr_video (session, event, qpc_before, qpc_after, wall_ns,
output_path) through tools/bbvr_ilp.py. Stdlib only.
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bbvr_ilp  # noqa: E402

WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'


def auth_string(password, salt, challenge):
    secret = base64.b64encode(hashlib.sha256((password + salt).encode()).digest()).decode()
    return base64.b64encode(hashlib.sha256((secret + challenge).encode()).digest()).decode()


def encode_frame(opcode, payload=b'', mask=True, key=None):
    """One FIN frame. Client frames must be masked (RFC 6455)."""
    n = len(payload)
    b0 = 0x80 | opcode
    mbit = 0x80 if mask else 0
    if n < 126:
        head = struct.pack('!BB', b0, mbit | n)
    elif n < 65536:
        head = struct.pack('!BBH', b0, mbit | 126, n)
    else:
        head = struct.pack('!BBQ', b0, mbit | 127, n)
    if not mask:
        return head + payload
    key = key if key is not None else os.urandom(4)
    masked = bytes(b ^ key[i & 3] for i, b in enumerate(payload))
    return head + key + masked


def read_frame(recv_exact):
    """-> (fin, opcode, payload). recv_exact(n) must return exactly n bytes."""
    b0, b1 = recv_exact(2)
    fin, opcode = bool(b0 & 0x80), b0 & 0x0F
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack('!H', recv_exact(2))[0]
    elif n == 127:
        n = struct.unpack('!Q', recv_exact(8))[0]
    key = recv_exact(4) if b1 & 0x80 else None
    data = recv_exact(n) if n else b''
    if key:
        data = bytes(b ^ key[i & 3] for i, b in enumerate(data))
    return fin, opcode, data


class ObsError(Exception):
    pass


class ObsClient:
    def __init__(self, url='ws://127.0.0.1:4455', password='', timeout=5.0):
        u = urllib.parse.urlparse(url)
        self.host, self.port = u.hostname or '127.0.0.1', u.port or 4455
        self.password, self.timeout = password, timeout
        self.sock = None
        self.buf = b''
        self.rid = 0

    def _recv_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ObsError('connection closed')
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _handshake(self):
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f'GET / HTTP/1.1\r\nHost: {self.host}:{self.port}\r\nUpgrade: websocket\r\n'
               f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n'
               f'Sec-WebSocket-Protocol: obswebsocket.json\r\n\r\n')
        self.sock.sendall(req.encode())
        while b'\r\n\r\n' not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ObsError('closed during handshake')
            self.buf += chunk
        head, self.buf = self.buf.split(b'\r\n\r\n', 1)
        lines = head.decode('latin1').split('\r\n')
        if ' 101 ' not in lines[0]:
            raise ObsError(f'handshake refused: {lines[0]}')
        hdr = {k.strip().lower(): v.strip() for k, v in (ln.split(':', 1) for ln in lines[1:] if ':' in ln)}
        want = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        if hdr.get('sec-websocket-accept') != want:
            raise ObsError('bad Sec-WebSocket-Accept')

    def _send_text(self, obj):
        self.sock.sendall(encode_frame(0x1, json.dumps(obj).encode()))

    def recv_msg(self):
        """Next JSON message (handles ping/close/fragments); None on socket timeout."""
        parts = []
        while True:
            try:
                fin, op, data = read_frame(self._recv_exact)
            except socket.timeout:
                if parts:
                    continue
                return None
            if op == 0x9:
                self.sock.sendall(encode_frame(0xA, data))
            elif op == 0x8:
                raise ObsError('server closed the connection')
            elif op in (0x1, 0x2, 0x0):
                parts.append(data)
                if fin:
                    return json.loads(b''.join(parts).decode('utf-8'))

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.settimeout(self.timeout)
        self._handshake()
        hello = self.recv_msg()
        if not hello or hello.get('op') != 0:
            raise ObsError('no Hello from server')
        ident = {'rpcVersion': 1}
        a = hello['d'].get('authentication')
        if a:
            if not self.password:
                raise ObsError('server needs a password (--password or OBS_WS_PASSWORD)')
            ident['authentication'] = auth_string(self.password, a['salt'], a['challenge'])
        self._send_text({'op': 1, 'd': ident})
        while True:
            m = self.recv_msg()
            if m is None:
                raise ObsError('no Identified reply (wrong password?)')
            if m.get('op') == 2:
                return

    def close(self):
        if self.sock:
            try:
                self.sock.sendall(encode_frame(0x8, struct.pack('!H', 1000)))
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def request(self, rtype, data=None, events=None):
        """Send a request, return responseData. Events seen meanwhile go to events (a list)."""
        self.rid += 1
        rid = f'bbvr{self.rid}'
        d = {'requestType': rtype, 'requestId': rid}
        if data:
            d['requestData'] = data
        self._send_text({'op': 6, 'd': d})
        end = time.monotonic() + self.timeout
        while time.monotonic() < end:
            m = self.recv_msg()
            if m is None:
                continue
            if m.get('op') == 5 and events is not None:
                events.append(m['d'])
            if m.get('op') == 7 and m['d'].get('requestId') == rid:
                st = m['d'].get('requestStatus', {})
                if not st.get('result'):
                    raise ObsError(f'{rtype}: {st.get("code")} {st.get("comment")}')
                return m['d'].get('responseData') or {}
        raise ObsError(f'{rtype}: timeout')


def video_line(session, event, qb, qa, wall_before, wall_after, path=None):
    return bbvr_ilp.format_line('bbvr_video', {'session': session, 'event': event},
                                {'qpc_before': qb, 'qpc_after': qa, 'wall_ns': wall_before,
                                 'wall_after_ns': wall_after, 'output_path': path}, wall_before)


def do_start(cli, session, sender, log=print):
    events = []
    qb, wb = time.perf_counter_ns(), time.time_ns()
    cli.request('StartRecord', events=events)
    qa, wa = time.perf_counter_ns(), time.time_ns()
    path = None
    end = time.monotonic() + 3.0
    while path is None and time.monotonic() < end:     # the output path arrives as an event
        for e in events:
            if e.get('eventType') == 'RecordStateChanged' and e['eventData'].get('outputActive'):
                path = e['eventData'].get('outputPath')
        if path is None:
            m = cli.recv_msg()
            if m and m.get('op') == 5:
                events.append(m['d'])
    sender.send([video_line(session, 'start_request', qb, qa, wb, wa, path)])
    log(f'obs: StartRecord qpc {qb}..{qa} ({(qa - qb) / 1e6:.1f} ms) path={path}')
    return path


def do_stop(cli, session, sender, log=print):
    qb, wb = time.perf_counter_ns(), time.time_ns()
    r = cli.request('StopRecord')
    qa, wa = time.perf_counter_ns(), time.time_ns()
    path = r.get('outputPath')
    sender.send([video_line(session, 'stop_request', qb, qa, wb, wa, path)])
    log(f'obs: StopRecord qpc {qb}..{qa} path={path}')
    return path


def handle_event(e, session, sender, log=print):
    q, w = time.perf_counter_ns(), time.time_ns()
    t, d = e.get('eventType'), e.get('eventData') or {}
    if t == 'RecordStateChanged':
        sender.send([video_line(session, 'state:' + str(d.get('outputState')), q, q, w, w,
                                d.get('outputPath'))])
        log(f'obs: RecordStateChanged {d.get("outputState")} {d.get("outputPath")}')
    elif t == 'RecordFileChanged':
        sender.send([video_line(session, 'file_changed', q, q, w, w, d.get('newOutputPath'))])
        log(f'obs: RecordFileChanged {d.get("newOutputPath")}')


def watch(cli, session, sender, stop=None, log=print):
    while not (stop and stop.is_set()):
        m = cli.recv_msg()
        if m and m.get('op') == 5:
            handle_event(m['d'], session, sender, log)


def run_obs_thread(session, sender, stop, url, password, log=print):
    """Used by bbvr_rec.py --obs: connect, start recording, watch until stop, then stop recording."""
    cli = ObsClient(url, password)
    try:
        cli.connect()
        do_start(cli, session, sender, log)
        cli.sock.settimeout(1.0)
        watch(cli, session, sender, stop, log)
        cli.sock.settimeout(5.0)
        do_stop(cli, session, sender, log)
    except (ObsError, OSError) as e:
        log(f'obs: {e}')
    finally:
        cli.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['start', 'stop', 'status', 'watch'])
    ap.add_argument('--url', default='ws://127.0.0.1:4455')
    ap.add_argument('--password', default=os.environ.get('OBS_WS_PASSWORD', ''))
    ap.add_argument('--session', default=time.strftime('s%Y%m%d_%H%M%S'))
    ap.add_argument('--ilp-host', default='127.0.0.1')
    ap.add_argument('--ilp-port', type=int, default=9009)
    ap.add_argument('--dry', action='store_true', help='print rows instead of sending')
    a = ap.parse_args()
    sender = bbvr_ilp.IlpSender(a.ilp_host, a.ilp_port, os.path.join(bbvr_ilp.data_dir('rec', os.path.join('out', 'rec')), f'{a.session}_video.ilp'), dry=a.dry)
    cli = ObsClient(a.url, a.password)
    try:
        cli.connect()
        if a.cmd == 'start':
            do_start(cli, a.session, sender)
        elif a.cmd == 'stop':
            do_stop(cli, a.session, sender)
        elif a.cmd == 'status':
            print(json.dumps(cli.request('GetRecordStatus'), indent=1))
        else:
            cli.sock.settimeout(1.0)
            print(f'watching OBS (session {a.session}); Ctrl+C to stop')
            try:
                watch(cli, a.session, sender)
            except KeyboardInterrupt:
                pass
    except (ObsError, OSError) as e:
        print(f'error: {e}', file=sys.stderr)
        return 1
    finally:
        cli.close()
        sender.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
