#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Multi-instance recorder: samples every running bb-probe process into QuestDB, one shared session.

    python tools/bbvr_rec_multi.py --session coop1                 all bb-probe*.exe, 10 Hz, until Ctrl+C
    python tools/bbvr_rec_multi.py --seconds 10 --dry             print rows instead of sending
    python tools/bbvr_rec_multi.py --pid 123 --pid 456 --label 123=Player1
    python tools/bbvr_rec_multi.py --stop-file F                  exit when F appears (record.sh)

Same read-only attach and sampling as tools/bbvr_rec.py (PROCESS_VM_READ only; the command line and
environment are read from the PEB with PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, nothing is
written). New processes are picked up every few seconds, exited ones are dropped. Tables:
bbvr_state   (same columns as bbvr_rec.py plus tags inst, pid, exe), bbvr_session (one row),
bbvr_instance (one row per process: label, npid, user_dir, cmdline, first/last seen).
The label is BB_SHADNET_NPID (else BB_USER_NAME, else the user dir name, else exe-pid) from the
process environment; --label PID=NAME overrides. Windows, Python 3.10+, stdlib only.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import fnmatch
import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bbvr_ilp  # noqa: E402
import bbvr_rec  # noqa: E402

SPOOL_DIR = bbvr_rec.SPOOL_DIR
FLUSH_NS = 500_000_000


def process_names():
    """{pid: exe name} for every process (toolhelp snapshot)."""
    import vr_stamina_probe as p
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    snap = k32.CreateToolhelp32Snapshot(p.TH32CS_SNAPPROCESS, 0)
    e = p.PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    out = {}
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        out[e.th32ProcessID] = e.szExeFile
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def read_peb_strings(pid):
    """(command line, {env}) of a 64-bit process, or ('', {}) when it cannot be read. Read only."""
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    nt = ctypes.WinDLL('ntdll')
    k32.OpenProcess.restype = wt.HANDLE
    k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t,
                                      ctypes.POINTER(ctypes.c_size_t)]
    h = k32.OpenProcess(0x0400 | 0x0010, False, pid)    # QUERY_INFORMATION | VM_READ
    if not h:
        return '', {}

    def rd(addr, n):
        buf, got = ctypes.create_string_buffer(n), ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(h, ctypes.c_uint64(addr), buf, n, ctypes.byref(got)) or not got.value:
            return b''
        return buf.raw[:got.value]

    try:
        pbi = (ctypes.c_uint64 * 6)()
        if nt.NtQueryInformationProcess(h, 0, ctypes.byref(pbi), ctypes.sizeof(pbi), None) != 0:
            return '', {}
        peb = pbi[1]
        pp = int.from_bytes(rd(peb + 0x20, 8) or b'\0' * 8, 'little')       # ProcessParameters
        if not pp:
            return '', {}
        head = rd(pp, 0x400)
        if len(head) < 0x3F8:
            return '', {}
        clen = int.from_bytes(head[0x70:0x72], 'little')
        cptr = int.from_bytes(head[0x78:0x80], 'little')
        eptr = int.from_bytes(head[0x80:0x88], 'little')
        esz = min(int.from_bytes(head[0x3F0:0x3F8], 'little'), 1 << 20)
        cmd = rd(cptr, clen).decode('utf-16-le', 'replace') if clen and cptr else ''
        env = {}
        if eptr and esz:
            for item in rd(eptr, esz).decode('utf-16-le', 'replace').split('\0'):
                k, sep, v = item.partition('=')
                if sep and k and not k.startswith('='):
                    env[k] = v
        return cmd, env
    finally:
        k32.CloseHandle(h)


class Inst:
    def __init__(self, pid, exe, label, npid, user_dir, cmd):
        self.pid, self.exe, self.label, self.npid, self.user_dir, self.cmd = pid, exe, label, npid, user_dir, cmd
        self.stop = threading.Event()
        self.thread = None
        self.n = 0
        self.valid = 0


def describe(pid, exe, overrides):
    cmd, env = read_peb_strings(pid)
    npid = env.get('BB_SHADNET_NPID', '')
    user_dir = env.get('BB_USER_DIR', '')
    label = (overrides.get(pid) or npid or env.get('BB_USER_NAME') or
             (os.path.basename(user_dir.rstrip('/\\')) if user_dir else '') or f'{exe.rsplit(".", 1)[0]}-{pid}')
    return Inst(pid, exe, label, npid, user_dir, cmd)


def instance_line(session, inst, first, last):
    return bbvr_ilp.format_line('bbvr_instance', {'session': session, 'inst': inst.label}, {
        'pid': inst.pid, 'exe': inst.exe, 'npid': inst.npid, 'user_dir': inst.user_dir,
        'cmdline': inst.cmd[:500], 'samples': inst.n, 'valid_samples': inst.valid,
        'first_seen_ns': first, 'last_seen_ns': last}, last)


def tagged_state(session, inst, i, qpc, wall, s):
    valid = s is not None and s[1] > 0
    s = s or (0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0)
    return bbvr_ilp.format_line('bbvr_state', {'session': session, 'inst': inst.label, 'pid': str(inst.pid),
                                               'exe': inst.exe}, {
        'sample': i, 'qpc_ns': qpc, 'hp': s[0], 'hp_max': s[1], 'stamina': s[2], 'stamina_max': s[3],
        'px': s[4], 'py': s[5], 'pz': s[6], 'yaw': s[7], 'valid': valid}, wall)


def worker(a, session, sender, inst, fake):
    """One instance: attach (slow image scan, off the discovery thread), then sample on a grid."""
    probe = None
    if not fake:
        import vr_stamina_probe as probe
    period = int(1e9 / max(1.0, a.hz))
    reader, next_attach = None, 0
    t0 = time.perf_counter_ns()
    i, batch, last_flush = 0, [], t0
    while not inst.stop.is_set():
        i += 1
        target = t0 + i * period
        rem = target - time.perf_counter_ns()
        if rem > 0:
            inst.stop.wait(rem / 1e9)
        qpc = time.perf_counter_ns()
        if qpc - target > period:
            i = (qpc - t0) // period + 1
        wall = time.time_ns()
        if fake:
            s = bbvr_rec.fake_sample((qpc - t0) / 1e9)
        else:
            if reader is None and qpc >= next_attach:
                reader = bbvr_rec.attach(probe, inst.exe, inst.pid,
                                         lambda m: print(f'[{inst.label}] {m}', flush=True))
                next_attach = qpc + 3_000_000_000
            s = reader.sample(qpc) if reader else None
            if reader is not None and s is None and not reader.alive():
                reader = None
        inst.n += 1
        inst.valid += 1 if (s and s[1] > 0) else 0
        batch.append(tagged_state(session, inst, i, qpc, wall, s))
        if qpc - last_flush >= FLUSH_NS:
            sender.send(batch)
            batch, last_flush = [], qpc
    sender.send(batch)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--hz', type=float, default=10.0, help='samples per second per instance')
    ap.add_argument('--seconds', type=float, default=0.0)
    ap.add_argument('--session', default=time.strftime('c%Y%m%d_%H%M%S'))
    ap.add_argument('--name', default='bb-probe*.exe', help='process name glob')
    ap.add_argument('--pid', type=int, action='append', default=[], help='only these PIDs')
    ap.add_argument('--label', action='append', default=[], metavar='PID=NAME')
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=9009)
    ap.add_argument('--dry', action='store_true')
    ap.add_argument('--stop-file', help='exit when this file exists')
    ap.add_argument('--fake', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if sys.platform != 'win32':
        raise SystemExit('Windows only')
    overrides = {int(k): v for k, _, v in (x.partition('=') for x in a.label)}
    sender = bbvr_ilp.IlpSender(a.host, a.port, os.path.join(SPOOL_DIR, a.session + '.ilp'), dry=a.dry)
    q0, w0 = time.perf_counter_ns(), time.time_ns()
    sender.send([bbvr_ilp.format_line('bbvr_session', {'session': a.session}, {
        'start_wall_ns': w0, 'start_qpc_ns': q0, 'hz': float(a.hz), 'git_rev': bbvr_ilp.git_rev(bbvr_rec.REPO),
        'fake': bool(a.fake), 'multi': True}, w0)])
    insts, seen = {}, {}
    deadline = time.monotonic() + a.seconds if a.seconds else None
    print(f'session {a.session}: all {a.name} at {a.hz:g} Hz per instance; Ctrl+C to stop', flush=True)
    try:
        while True:
            if deadline and time.monotonic() >= deadline:
                break
            if a.stop_file and os.path.exists(a.stop_file):
                print('stop file found')
                break
            names = process_names()
            for pid, exe in names.items():
                if pid in insts or not fnmatch.fnmatch(exe.lower(), a.name.lower()):
                    continue
                if a.pid and pid not in a.pid:
                    continue
                inst = describe(pid, exe, overrides)
                insts[pid] = inst
                seen[pid] = time.time_ns()
                print(f'+ pid {pid} {exe} label={inst.label} npid={inst.npid or "-"} '
                      f'user_dir={inst.user_dir or "-"}', flush=True)
                inst.thread = threading.Thread(target=worker, args=(a, a.session, sender, inst, a.fake),
                                               daemon=True)
                inst.thread.start()
            for pid in [p for p in insts if p not in names]:
                inst = insts[pid]
                inst.stop.set()
                inst.thread.join(timeout=3)
                sender.send([instance_line(a.session, inst, seen[pid], time.time_ns())])
                print(f'- pid {pid} {inst.label} gone after {inst.n} samples', flush=True)
                del insts[pid]
            time.sleep(0.5 if deadline else 3.0)
    except KeyboardInterrupt:
        print('stopping')
    finally:
        for inst in insts.values():
            inst.stop.set()
        for pid, inst in insts.items():
            inst.thread.join(timeout=3)
            sender.send([instance_line(a.session, inst, seen[pid], time.time_ns())])
            print(f'{inst.label} (pid {pid}): {inst.n} samples, {inst.valid} valid')
        sender.close()
        print(f'sent {sender.sent}, spooled {sender.spooled}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
