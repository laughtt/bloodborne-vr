#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only live probe of the player's stamina (docs/vr/findings/20-swing-and-stamina.md).

Attaches to a running bb-probe.exe with PROCESS_VM_READ only (it never writes, suspends or
injects), finds the eboot image by code signatures, follows the player chain and prints the
stamina state whenever it changes:

    WorldChrMan [0x593E878] -> +0x60 PlayerIns -> +0x3B0 module bag
      bag+0x20 SprjChrDataModule: +0xF8 HP, +0xFC max HP, +0x134 stamina, +0x138 max stamina,
               +0x200 debug flags (0x80 "No Stamina Consume")
      bag+0x08 SprjChrActionFlagModule, bag+0x18 SprjChrTimeActModule (the animation ring)
      PlayerIns +0x39C regen points/s (stale while blocked), +0x168 regen remainder,
               +0x16C regen multiplier, +0xE0 frame time
      [[PlayerIns+0x58]+0x38] +0x34 regen percent (TAE event 225), +0x5C bit 1 regen blocked
               (the behaviour script's "stamina recovery disabled")
      [PlayerIns+0x4B8] equipment: +0x08 style (trick form), +0x10 right slot, +0x64 weapon ids

    python tools/vr_stamina_probe.py [--pid N] [--hz 60] [--csv out.csv] [--all]
    python tools/vr_stamina_probe.py --check-elf out/eboot.elf     (offline: the signatures)
    python tools/vr_stamina_probe.py --pad-file F --script         (scripted presses, below)

Every stamina change is printed with the animation playing and its time, so one R1 shows when
the cost is taken (at the attack's TAE window, ~0.37 s into a112_030000 for the Saw Cleaver, not
at the press), a dodge shows the script's dodge cost, and attacking while exhausted shows the
-50 floor. --script drives the pad through BB_PAD_FILE (the game must have been started with
BB_PAD_FILE=F): R1, R2, a dodge, dodges until exhausted, R1 while exhausted, idle (regen); the
procedure and what to look for are in doc 20 section 7. Windows only; stdlib only.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

VA_BASE = 0x400000
ELF_VA_DELTA = 0x3FC000            # out/eboot.elf: VA = file offset + 0x3FC000 (findings/13)
VA_WORLD_CHR_MAN = 0x593E878
VA_ALL_NO_STAMINA = 0x593E88A      # WorldChrManDbg "All No Stamina Consume" byte
VA_PLAYER_VTABLE = 0x578F310       # PlayerIns
VA_DATA_VTABLE = 0x5735810         # SprjChrDataModule
VA_FLAG_VTABLE = 0x57342C0         # SprjChrActionFlagModule
VA_TIMEACT_VTABLE = 0x5735190      # SprjChrTimeActModule

# Code that is unique in the eboot and never hooked (bytes = out/eboot.elf at VA - 0x3FC000).
SIGNATURES = [
    (0x1E57190, bytes.fromhex('554889e54156534989fe418b9e3401000001f385f6794441f686000200008075')),
    (0x1E56AE0, bytes.fromhex('554889e541565389f34989fe41399e340100007e4441f6860002000080755948')),
    (0x1CBDC44, bytes.fromhex('488b8bb00300004c8b7120458bae340100004101c585c079')),
]

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
EXECUTABLE = 0x10 | 0x20 | 0x40 | 0x80
TH32CS_SNAPPROCESS = 0x2


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_uint64), ('AllocationBase', ctypes.c_uint64),
                ('AllocationProtect', wt.DWORD), ('PartitionId', wt.WORD),
                ('RegionSize', ctypes.c_uint64), ('State', wt.DWORD), ('Protect', wt.DWORD),
                ('Type', wt.DWORD)]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD),
                ('cntThreads', wt.DWORD), ('th32ParentProcessID', wt.DWORD),
                ('pcPriClassBase', ctypes.c_long), ('dwFlags', wt.DWORD),
                ('szExeFile', ctypes.c_wchar * 260)]


def check_elf(path):
    data = open(path, 'rb').read()
    ok = True
    for va, sig in SIGNATURES:
        at = va - ELF_VA_DELTA
        here = data[at:at + len(sig)] == sig
        count = data.count(sig)
        print(f'0x{va:07x}: {"matches" if here else "DIFFERS"} at file offset 0x{at:x}, '
              f'{count} occurrence(s)')
        ok = ok and here and count == 1
    print('signatures OK' if ok else 'signatures do NOT fit this eboot')
    return 0 if ok else 1


class Process:
    def __init__(self, pid):
        self.k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        self.k32.OpenProcess.restype = wt.HANDLE
        self.k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
        self.k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_void_p,
                                               ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        self.k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_uint64,
                                            ctypes.POINTER(MEMORY_BASIC_INFORMATION), ctypes.c_size_t]
        self.k32.VirtualQueryEx.restype = ctypes.c_size_t
        self.handle = self.k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION,
                                           False, pid)
        if not self.handle:
            raise SystemExit(f'cannot open process {pid} for reading (error {ctypes.get_last_error()})')

    def read(self, address, size):
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t(0)
        if not self.k32.ReadProcessMemory(self.handle, ctypes.c_uint64(address), buf, size,
                                          ctypes.byref(got)) or got.value != size:
            return None
        return buf.raw

    def regions(self):
        address = 0x10000
        info = MEMORY_BASIC_INFORMATION()
        while address < 0x7FFFFFFF0000:
            if not self.k32.VirtualQueryEx(self.handle, ctypes.c_uint64(address), ctypes.byref(info),
                                           ctypes.sizeof(info)):
                break
            yield info.BaseAddress, info.RegionSize, info.State, info.Protect
            address = info.BaseAddress + info.RegionSize

    def find_image(self):
        """The address VA 0x400000 maps to, from the first signature found in executable memory
        (checked against the others)."""
        first_va, first_sig = SIGNATURES[0]
        for base, size, state, protect in self.regions():
            if state != MEM_COMMIT or protect & (PAGE_GUARD | PAGE_NOACCESS) or not protect & EXECUTABLE:
                continue
            for at in range(base, base + size, 1 << 22):
                chunk = self.read(at, min(1 << 22, base + size - at) + 64)
                if chunk is None:
                    chunk = self.read(at, min(1 << 22, base + size - at))
                if chunk is None:
                    continue
                k = chunk.find(first_sig)
                if k < 0:
                    continue
                image = at + k - (first_va - VA_BASE)
                if all(self.read(image + va - VA_BASE, len(sig)) == sig for va, sig in SIGNATURES[1:]):
                    return image
        return None


def find_pid(name):
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    found = []
    ok = k32.Process32FirstW(snap, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower() == name.lower():
            found.append(entry.th32ProcessID)
        ok = k32.Process32NextW(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return found


class Player:
    """One read of the chain; fields None when the chain is not there (loading, title)."""

    def __init__(self, proc, image):
        self.p, self.image = proc, image

    def va(self, v):
        return self.image + (v - VA_BASE)

    def u64(self, a):
        b = self.p.read(a, 8)
        return struct.unpack('<Q', b)[0] if b else None

    def ptr(self, a):
        v = self.u64(a) if a else None
        return v if v and v >= 0x10000 and not v & 7 else None

    def snapshot(self):
        s = {}
        world = self.ptr(self.va(VA_WORLD_CHR_MAN))
        player = self.ptr(world + 0x60) if world else None
        if not player or self.u64(player) != self.va(VA_PLAYER_VTABLE):
            return None
        bag = self.ptr(player + 0x3B0)
        data = self.ptr(bag + 0x20) if bag else None
        if not data or self.u64(data) != self.va(VA_DATA_VTABLE) or self.u64(data + 8) != player:
            return None
        d = self.p.read(data + 0xF8, 0x10C)
        if not d:
            return None
        s['player'], s['data'] = player, data
        s['hp'], s['hp_max'] = struct.unpack_from('<ii', d, 0)
        s['sp'], s['sp_max'] = struct.unpack_from('<ii', d, 0x134 - 0xF8)
        s['dbg'] = struct.unpack_from('<H', d, 0x200 - 0xF8)[0]
        all_flag = self.p.read(self.va(VA_ALL_NO_STAMINA), 1)
        s['all_no_sp'] = all_flag[0] if all_flag else None
        pl = self.p.read(player + 0xE0, 0x2C0)
        if pl:
            s['dt'] = struct.unpack_from('<f', pl, 0)[0]
            s['regen_rem'], s['regen_mul'] = struct.unpack_from('<ff', pl, 0x168 - 0xE0)
            s['regen_pts'] = struct.unpack_from('<i', pl, 0x39C - 0xE0)[0]
        ctrl = self.ptr(player + 0x58)
        flags = self.ptr(ctrl + 0x38) if ctrl else None
        fb = self.p.read(flags + 0x34, 0x2C) if flags else None
        if fb:
            s['regen_pct'] = fb[0]
            s['regen_blocked'] = bool(struct.unpack_from('<H', fb, 0x5C - 0x34)[0] & 2)
        tam = self.ptr(bag + 0x18)
        if tam and self.u64(tam) == self.va(VA_TIMEACT_VTABLE):
            ring = self.p.read(tam + 0x20, 0xA8)
            if ring:
                head = struct.unpack_from('<i', ring, 0xC0 - 0x20)[0]
                newest = (head + 9) % 10 if 0 <= head < 10 else 0
                anim, f0, f1, f2 = struct.unpack_from('<ifff', ring, 16 * newest)
                s['anim'] = f'a{anim // 1000000:03d}_{anim % 1000000:06d}' if anim >= 0 else '-'
                s['anim_t'] = (f0, f1, f2)
        equip = self.ptr(player + 0x4B8)
        eb = self.p.read(equip + 0x08, 0x80) if equip else None
        if eb:
            style, _slot_l, slot_r = struct.unpack_from('<iii', eb, 0)
            ids = struct.unpack_from('<6i', eb, 0x64 - 0x08)
            s['style'] = style
            s['right'] = ids[2 * slot_r + 1] if 0 <= slot_r <= 2 else None
        return s


def line(t, s, prev):
    sp = s['sp']
    delta = '' if prev is None or prev.get('sp') == sp else f' ({sp - prev["sp"]:+d})'
    anim = s.get('anim', '?')
    at = s.get('anim_t')
    at_s = f' t={at[0]:.3f}/{at[1]:.3f}/{at[2]:.3f}' if at else ''
    regen = (f" regen {s.get('regen_pts', '?')}/s pct {s.get('regen_pct', '?')}"
             f"{' BLOCKED' if s.get('regen_blocked') else ''}")
    dbg = ' NoSPconsume' if s['dbg'] & 0x80 or s.get('all_no_sp') else ''
    return (f'{t:8.3f}  SP {sp:4d}/{s["sp_max"]:<4d}{delta:7s} HP {s["hp"]}/{s["hp_max"]}  '
            f'{anim}{at_s}{regen}  wpn {s.get("right")} style {s.get("style")}{dbg}')


SCRIPT = [  # (seconds from start, pad file content, what it shows)
    (0.0, '', 'idle: stamina full, regen pct 100'),
    (2.0, 'r1', 'R1 (light): the cost at the TAE window, not at the press'),
    (2.15, '', ''),
    (5.0, 'r2', 'R2 tap (heavy): BehaviorParam judge 150 cost'),
    (5.15, '', ''),
    (8.0, 'circle', 'circle, stick centred (a backstep): the script cost (HKS act 36)'),
    (8.15, '', ''),
    (9.5, 'circle ly=0', 'circle with the stick forward (a roll): its cost'),
    (9.65, '', ''),
]
for i in range(8):  # dodges until exhausted
    SCRIPT += [(11.0 + 0.9 * i, 'circle', 'dodge spam' if i == 0 else ''), (11.15 + 0.9 * i, '', '')]
SCRIPT += [(18.5, 'r1', 'R1 while exhausted: refused at <= 0? the floor -50?'), (18.65, '', ''),
           (19.0, 'r1', ''), (19.15, '', ''), (22.0, '', 'idle: regen after the pause')]


FAKE_TARGET = r'''
import ctypes, struct, sys, time
k = ctypes.WinDLL("kernel32")
k.VirtualAlloc.restype = ctypes.c_uint64
k.VirtualAlloc.argtypes = [ctypes.c_uint64, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
image = k.VirtualAlloc(0, 0x5A00000, 0x3000, 0x40)
heap = k.VirtualAlloc(0, 0x10000, 0x3000, 0x04)
def put(addr, data): ctypes.memmove(addr, data, len(data))
def q(addr, v): put(addr, struct.pack("<Q", v))
va = lambda v: image + v - 0x400000
for sva, sig in SIGS: put(va(sva), sig)
W, P, BAG, DATA, TAM, CTRL, FLAGS, EQUIP = (heap + 0x800 * i for i in range(8))
q(va(0x593E878), W); q(W + 0x60, P); q(P, va(0x578F310)); q(P + 0x3B0, BAG)
q(BAG + 0x20, DATA); q(DATA, va(0x5735810)); q(DATA + 8, P)
put(DATA + 0xF8, struct.pack("<ii", 812, 900)); put(DATA + 0x134, struct.pack("<ii", 77, 91))
q(BAG + 0x18, TAM); q(TAM, va(0x5735190))
put(TAM + 0x20 + 16 * 3, struct.pack("<ifff", 112030000, 0.25, 0.5, 2.6)); put(TAM + 0xC0, struct.pack("<i", 4))
q(P + 0x58, CTRL); q(CTRL + 0x38, FLAGS); put(FLAGS + 0x34, bytes([100])); put(FLAGS + 0x5C, struct.pack("<H", 2))
put(P + 0x39C, struct.pack("<i", 45)); put(P + 0x168, struct.pack("<ff", 0.25, 1.0))
q(P + 0x4B8, EQUIP); put(EQUIP + 0x08, struct.pack("<iii", 1, 0, 0))
put(EQUIP + 0x64, struct.pack("<6i", 14000000, 7000000, -1, -1, -1, -1))
print(hex(image), flush=True)
time.sleep(60)
'''


def self_test():
    """Runs the reader against a fake target process (a Python child holding the signatures and
    a made-up player chain), so the probe can be checked without the game."""
    import subprocess
    code = 'SIGS = ' + repr([(va, sig) for va, sig in SIGNATURES]) + '\n' + FAKE_TARGET
    child = subprocess.Popen([sys.executable, '-I', '-c', code], stdout=subprocess.PIPE, text=True)
    try:
        expect = int(child.stdout.readline().strip(), 16)
        proc = Process(child.pid)
        image = proc.find_image()
        s = Player(proc, image).snapshot() if image else None
        checks = [
            ('image found', image == expect),
            ('chain read', s is not None),
            ('stamina 77/91', s and (s['sp'], s['sp_max']) == (77, 91)),
            ('hp 812/900', s and (s['hp'], s['hp_max']) == (812, 900)),
            ('anim a112_030000', s and s.get('anim') == 'a112_030000'),
            ('regen 45/s, 100 %, blocked', s and (s.get('regen_pts'), s.get('regen_pct'),
                                                   s.get('regen_blocked')) == (45, 100, True)),
            ('right weapon 7000000', s and s.get('right') == 7000000),
        ]
        for what, ok in checks:
            print(f'{"ok  " if ok else "FAIL"} {what}')
        return 0 if all(ok for _, ok in checks) else 1
    finally:
        child.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--pid', type=int)
    ap.add_argument('--name', default='bb-probe.exe')
    ap.add_argument('--hz', type=float, default=60.0)
    ap.add_argument('--csv', help='also write every sample here')
    ap.add_argument('--all', action='store_true', help='print every sample, not only changes')
    ap.add_argument('--seconds', type=float, default=0.0, help='stop after this long (0: Ctrl+C)')
    ap.add_argument('--check-elf', metavar='EBOOT', help='offline: check the signatures and exit')
    ap.add_argument('--self-test', action='store_true',
                    help='read a fake target process (no game needed) and exit')
    ap.add_argument('--pad-file', help="the game's BB_PAD_FILE, for --script")
    ap.add_argument('--script', action='store_true', help='drive the test presses (needs --pad-file)')
    a = ap.parse_args()
    if a.check_elf:
        return check_elf(a.check_elf)
    if sys.platform != 'win32':
        raise SystemExit('Windows only')
    if a.self_test:
        return self_test()
    if a.script and not a.pad_file:
        raise SystemExit('--script needs --pad-file (the BB_PAD_FILE the game was started with)')
    pid = a.pid
    if not pid:
        pids = find_pid(a.name)
        if len(pids) != 1:
            raise SystemExit(f'{a.name}: {len(pids)} processes; pass --pid')
        pid = pids[0]
    proc = Process(pid)
    image = proc.find_image()
    if image is None:
        raise SystemExit('eboot image not found (not Bloodborne 1.09, or not started yet)')
    print(f'pid {pid}: VA 0x400000 is mapped at 0x{image:x}; reading only')
    reader = Player(proc, image)
    csv = open(a.csv, 'w', encoding='utf-8') if a.csv else None
    if csv:
        csv.write('t,sp,sp_max,hp,hp_max,anim,anim_t0,anim_t1,anim_t2,regen_pts,regen_pct,'
                  'regen_blocked,regen_rem,dt,right,style,dbg\n')
    start = time.perf_counter()
    period = 1.0 / max(1.0, a.hz)
    prev, missing, step = None, False, 0
    try:
        while True:
            t = time.perf_counter() - start
            if a.seconds and t > a.seconds:
                break
            if a.script:
                while step < len(SCRIPT) and t >= SCRIPT[step][0]:
                    _, pad, what = SCRIPT[step]
                    with open(a.pad_file, 'w', encoding='ascii') as f:
                        f.write(pad + '\n')
                    if what:
                        print(f'{t:8.3f}  -- {what} (pad: {pad or "released"})')
                    step += 1
                if step >= len(SCRIPT) and t > SCRIPT[-1][0] + 4.0:
                    break
            s = reader.snapshot()
            if s is None:
                if not missing:
                    print(f'{t:8.3f}  player not available (title, loading, or the chain moved)')
                missing, prev = True, None
            else:
                missing = False
                key = (s['sp'], s['sp_max'], s.get('anim'), s.get('regen_blocked'), s.get('regen_pct'))
                pkey = None if prev is None else (prev['sp'], prev['sp_max'], prev.get('anim'),
                                                  prev.get('regen_blocked'), prev.get('regen_pct'))
                if a.all or key != pkey:
                    print(line(t, s, prev))
                if csv:
                    at = s.get('anim_t') or (0.0, 0.0, 0.0)
                    csv.write(f"{t:.4f},{s['sp']},{s['sp_max']},{s['hp']},{s['hp_max']},"
                              f"{s.get('anim', '')},{at[0]:.4f},{at[1]:.4f},{at[2]:.4f},"
                              f"{s.get('regen_pts', '')},{s.get('regen_pct', '')},"
                              f"{int(bool(s.get('regen_blocked')))},{s.get('regen_rem', 0.0):.4f},"
                              f"{s.get('dt', 0.0):.5f},{s.get('right', '')},{s.get('style', '')},"
                              f"{s['dbg']}\n")
                prev = s
            time.sleep(max(0.0, period - (time.perf_counter() - start - t)))
    except KeyboardInterrupt:
        pass
    finally:
        if a.script and a.pad_file:
            with open(a.pad_file, 'w', encoding='ascii') as f:
                f.write('\n')
        if csv:
            csv.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
