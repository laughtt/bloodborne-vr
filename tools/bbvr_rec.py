#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Day-one recorder: samples the player state at --hz and streams it to QuestDB (ILP, TCP 9009).

    python tools/bbvr_rec.py --obs              state + start/watch OBS recording, one session id
    python tools/bbvr_rec.py --seconds 30 --dry print rows instead of sending
    python tools/bbvr_rec.py --replay-spool     push the spooled *.ilp (out/rec, or $BBVR_STORE_DIR/rec) while QuestDB was down

Read-only attach to bb-probe.exe (same as tools/vr_stamina_probe.py, which this imports). Tables:
bbvr_state (one row per sample), bbvr_session (one row per run), bbvr_video (bbvr_obs_ctl.py).
qpc_ns is time.perf_counter_ns() (QPC, shared with the OBS script); the designated timestamp is
time.time_ns(). Pointers are cached and re-resolved about every second or when a read fails.
Windows, Python 3.10+, stdlib only.
"""
import argparse
import ctypes
import glob
import math
import os
import struct
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bbvr_ilp  # noqa: E402

REPO = os.path.dirname(HERE)
SPOOL_DIR = bbvr_ilp.data_dir('rec', os.path.join(REPO, 'out', 'rec'))   # $BBVR_STORE_DIR/rec when set
FLUSH_NS = 250_000_000
RESOLVE_NS = 1_000_000_000

DATA_OFF, DATA_LEN = 0xF8, 0x44      # HP, max HP at +0, stamina +0x3C, max stamina +0x40
POS_OFF, POS_LEN = 0x1D4, 0x1C       # yaw +0, position xyz at +0xC (obj+0x1E0)
DATA_HEAD_LEN = 0x10                 # vtable + owner, checked on every sample


class Reader:
    """Cached-pointer reader on top of vr_stamina_probe's Process/Player."""

    def __init__(self, probe, proc, image):
        self.probe, self.proc = probe, proc
        self.pl = probe.Player(proc, image)
        self.image = image
        self.cache = None
        self.next_resolve = 0

    def resolve(self):
        pl, probe = self.pl, self.probe
        world = pl.ptr(pl.va(probe.VA_WORLD_CHR_MAN))
        player = pl.ptr(world + 0x60) if world else None
        if not player or pl.u64(player) != pl.va(probe.VA_PLAYER_VTABLE):
            return None
        bag = pl.ptr(player + 0x3B0)
        data = pl.ptr(bag + 0x20) if bag else None
        if not data or pl.u64(data) != pl.va(probe.VA_DATA_VTABLE) or pl.u64(data + 8) != player:
            return None
        pos = pl.ptr(bag + 0x68)
        if not pos:
            return None
        return (player, data, pos)

    def sample(self, now_ns):
        """-> tuple (hp, hp_max, sp, sp_max, px, py, pz, yaw) or None (loading / lost)."""
        if self.cache is None or now_ns >= self.next_resolve:
            self.cache = self.resolve()
            self.next_resolve = now_ns + RESOLVE_NS
            if self.cache is None:
                return None
        player, data, pos = self.cache
        rd = self.proc.read
        head = rd(data, DATA_HEAD_LEN)
        d = rd(data + DATA_OFF, DATA_LEN) if head else None
        p = rd(pos + POS_OFF, POS_LEN) if d else None
        ok = p is not None
        if ok:
            vt, owner = struct.unpack('<QQ', head)
            ok = vt == self.pl.va(self.probe.VA_DATA_VTABLE) and owner == player
        if not ok:
            self.cache = None
            self.next_resolve = 0
            return None
        hp, hpm = struct.unpack_from('<ii', d, 0)
        sp, spm = struct.unpack_from('<ii', d, 0x3C)
        yaw = struct.unpack_from('<f', p, 0)[0]
        px, py, pz = struct.unpack_from('<fff', p, 0xC)
        return (hp, hpm, sp, spm, px, py, pz, yaw)

    def alive(self):
        code = ctypes.c_uint32(0)
        ok = self.proc.k32.GetExitCodeProcess(self.proc.handle, ctypes.byref(code))
        return bool(ok) and code.value == 259


def attach(probe, name, pid, log):
    """Process + image, or None. Never raises."""
    try:
        pids = [pid] if pid else probe.find_pid(name)
        if len(pids) != 1:
            return None
        proc = probe.Process(pids[0])
        image = proc.find_image()
        if image is None:
            return None
        log(f'attached pid {pids[0]}, VA 0x400000 at 0x{image:x}')
        return Reader(probe, proc, image)
    except (SystemExit, OSError) as e:
        log(f'attach failed: {e}')
        return None


def fake_sample(t):
    return (int(900 - 100 * (0.5 + 0.5 * math.sin(t))), 900, int(91 * (0.5 + 0.5 * math.cos(t * 2))), 91,
            10.0 * math.cos(t * 0.3), 1.5, 10.0 * math.sin(t * 0.3), (t * 0.3) % (2 * math.pi) - math.pi)


def state_line(session, i, qpc, wall, s):
    valid = s[1] > 0     # max HP > 0: a real player, not a half-built one
    return bbvr_ilp.format_line('bbvr_state', {'session': session}, {
        'sample': i, 'qpc_ns': qpc, 'hp': s[0], 'hp_max': s[1], 'stamina': s[2], 'stamina_max': s[3],
        'px': s[4], 'py': s[5], 'pz': s[6], 'yaw': s[7], 'valid': valid}, wall)


def invalid_line(session, i, qpc, wall):
    return bbvr_ilp.format_line('bbvr_state', {'session': session}, {
        'sample': i, 'qpc_ns': qpc, 'hp': 0, 'hp_max': 0, 'stamina': 0, 'stamina_max': 0,
        'px': 0.0, 'py': 0.0, 'pz': 0.0, 'yaw': 0.0, 'valid': False}, wall)


def replay(arg, host, port):
    if arg == 'all':
        paths = sorted(glob.glob(os.path.join(SPOOL_DIR, '*.ilp')))
    else:
        paths = [arg if os.path.exists(arg) else os.path.join(SPOOL_DIR, arg + '.ilp')]
    if not paths:
        print('no spool files')
    for p in paths:
        try:
            bbvr_ilp.replay_spool(p, host, port)
        except OSError as e:
            print(f'{p}: {e}')
            return 1
    return 0


def run(a, session, sender, stop):
    probe = None
    if not a.fake:
        import vr_stamina_probe as probe
    period = int(1e9 / max(1.0, a.hz))
    if sys.platform == 'win32':
        try:
            ctypes.WinDLL('winmm').timeBeginPeriod(1)
        except OSError:
            pass
    q0, w0 = time.perf_counter_ns(), time.time_ns()
    sender.send([bbvr_ilp.format_line('bbvr_session', {'session': session}, {
        'start_wall_ns': w0, 'start_qpc_ns': q0, 'hz': float(a.hz), 'git_rev': bbvr_ilp.git_rev(REPO),
        'fake': bool(a.fake)}, w0)])
    reader, next_attach = None, 0
    batch, last_flush = [], q0
    t0, i, n_total, late = q0, 0, 0, 0
    deadline = t0 + int(a.seconds * 1e9) if a.seconds else None
    while not stop.is_set():
        i += 1
        target = t0 + i * period
        while True:                                     # sleep coarse, spin the last ~1.5 ms
            rem = target - time.perf_counter_ns()
            if rem <= 0:
                break
            if rem > 2_000_000:
                time.sleep((rem - 1_500_000) / 1e9)
        qpc = time.perf_counter_ns()
        if qpc - target > period:                       # fell behind: skip ticks, keep the grid
            i = (qpc - t0) // period + 1
            late += 1
        if deadline and qpc >= deadline:
            break
        wall = time.time_ns()
        if a.fake:
            batch.append(state_line(session, i, qpc, wall, fake_sample((qpc - t0) / 1e9)))
        else:
            if reader is None and qpc >= next_attach:
                reader = attach(probe, a.name, a.pid, print)
                next_attach = qpc + 2_000_000_000
            s = None
            if reader is not None:
                s = reader.sample(qpc)
                if s is None and not reader.alive():
                    print('game process gone; waiting to re-attach')
                    reader = None
            batch.append(state_line(session, i, qpc, wall, s) if s else invalid_line(session, i, qpc, wall))
        n_total += 1
        if qpc - last_flush >= FLUSH_NS:
            sender.send(batch)
            batch, last_flush = [], qpc
    sender.send(batch)
    print(f'session {session}: {n_total} samples, {late} late resyncs, sent {sender.sent}, '
          f'spooled {sender.spooled}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--hz', type=float, default=60.0)
    ap.add_argument('--seconds', type=float, default=0.0, help='exit after N seconds (0: Ctrl+C)')
    ap.add_argument('--session', default=time.strftime('s%Y%m%d_%H%M%S'))
    ap.add_argument('--name', default='bb-probe.exe', help='game process name')
    ap.add_argument('--pid', type=int)
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=9009, help='QuestDB ILP TCP port')
    ap.add_argument('--dry', action='store_true', help='print rows instead of sending')
    ap.add_argument('--obs', action='store_true', help='also start/watch the OBS recording')
    ap.add_argument('--obs-url', default='ws://127.0.0.1:4455')
    ap.add_argument('--obs-password', default=os.environ.get('OBS_WS_PASSWORD', ''))
    ap.add_argument('--replay-spool', nargs='?', const='all', metavar='FILE_OR_SESSION',
                    help='push spooled ILP (default: every out/rec/*.ilp) and exit')
    ap.add_argument('--fake', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.replay_spool:
        return replay(a.replay_spool, a.host, a.port)
    if sys.platform != 'win32' and not a.fake:
        raise SystemExit('Windows only (use --fake elsewhere)')
    sender = bbvr_ilp.IlpSender(a.host, a.port, os.path.join(SPOOL_DIR, a.session + '.ilp'), dry=a.dry)
    stop = threading.Event()
    obs = None
    if a.obs:
        import bbvr_obs_ctl
        obs = threading.Thread(target=bbvr_obs_ctl.run_obs_thread, daemon=True,
                               args=(a.session, sender, stop, a.obs_url, a.obs_password))
        obs.start()
    print(f'session {a.session} at {a.hz:g} Hz; Ctrl+C to stop')
    try:
        run(a, a.session, sender, stop)
    except KeyboardInterrupt:
        print('stopping')
    finally:
        stop.set()
        if obs:
            obs.join(timeout=8)
        sender.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
