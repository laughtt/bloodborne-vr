# Usage: python -I tools/redzone_reloc.py <eboot.elf> <start hex> <end hex> <out.h>
# Appends one relocated (red-zone-free) copy of an eboot leaf function to <out.h>; see src/runtime_redzone_guard.c.
# Regenerates src/redzone_blobs.h; verify with tools/redzone_reloc_check.py.
# Relocate the eboot leaf function [START,END) so that it keeps no data below rsp:
# frame grows by SHIFT bytes and every rsp-based displacement moves up by SHIFT.
import sys, struct, capstone
from capstone import x86_const as X
ELF, START, END, SHIFT = sys.argv[1], int(sys.argv[2],16), int(sys.argv[3],16), 0x80
P = 'rz_%x' % START
d = open(ELF,'rb').read()
code = d[0x4000+START:0x4000+END]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
ins = list(md.disasm(code, START))
assert sum(i.size for i in ins) == END-START, "decode gap"
JCC = {'jo':0,'jno':1,'jb':2,'jae':3,'je':4,'jne':5,'jbe':6,'ja':7,'js':8,'jns':9,'jp':10,'jnp':11,'jl':12,'jge':13,'jle':14,'jg':15}
def enc(i):
    b = bytearray(i.bytes); rip = None
    m = i.mnemonic
    if m in JCC or m == 'jmp':
        assert i.operands[0].type == X.X86_OP_IMM, i
        t = i.operands[0].imm; assert START <= t < END, ("external branch", hex(i.address))
        return (bytes([0x0f,0x80+JCC[m]]) if m != 'jmp' else b'\xe9') + b'\0\0\0\0', ('br', t), None
    if m in ('call','syscall','int3') or m.startswith('loop') or m.startswith('j'):
        raise SystemExit("unsupported %s at %x" % (m, i.address))
    if i.size == 4 and bytes(i.bytes[:3]) in (b'\x48\x83\xec', b'\x48\x83\xc4'):
        return b'\x48\x81' + bytes(i.bytes[2:3]) + struct.pack('<i', i.bytes[3] + SHIFT), None, None
    if bytes(i.bytes) == b'\x50':  # push rax that only reserves an 8-byte slot (written before read)
        return b'\x48\x81\xec' + struct.pack('<i', 8 + SHIFT), None, None
    for op in i.operands:
        if op.type != X.X86_OP_MEM: continue
        if op.mem.base == X.X86_REG_RIP:
            assert i.disp_size == 4
            t = i.address + i.size + op.mem.disp
            if op.access & capstone.CS_AC_WRITE: raise SystemExit("rip store at %x" % i.address)
            rip = (i.disp_offset, t)
        elif op.mem.base == X.X86_REG_RSP or op.mem.index == X.X86_REG_RSP:
            assert op.mem.base == X.X86_REG_RSP
            nd = op.mem.disp + SHIFT
            mo = i.modrm_offset; mod = b[mo] >> 6
            if i.disp_size == 1 and -128 <= nd <= 127: b[i.disp_offset] = nd & 0xff
            elif i.disp_size == 4: b[i.disp_offset:i.disp_offset+4] = struct.pack('<i', nd)
            else:
                # mod 00 (no disp) or 01 (disp8) -> mod 10 with disp32
                at = i.disp_offset if i.disp_size == 1 else mo + 2  # rsp base always has SIB: disp follows SIB
                tail = b[at + i.disp_size:]
                b = b[:at] + struct.pack('<i', nd) + tail
                b[mo] = (b[mo] & 0x3f) | 0x80
                if rip: raise SystemExit("rip+rsp?")
    # implicit rsp users other than push/pop/ret/sub/add handled above
    regs_read, regs_write = i.regs_access()
    if X.X86_REG_RSP in regs_write and i.mnemonic not in ('push','pop','ret','sub','add'):
        raise SystemExit("rsp write %s at %x" % (i.mnemonic, i.address))
    if i.mnemonic in ('sub','add') and X.X86_REG_RSP in regs_write and i.bytes[:3] not in (b'\x48\x83\xec', b'\x48\x83\xc4'):
        raise SystemExit("odd rsp arith at %x" % i.address)
    return bytes(b), None, rip
items = [(i,)+enc(i) for i in ins]
# layout
off = {}; pos = 0
for i, b, br, rip in items: off[i.address] = pos; pos += len(b)
blob = bytearray(); fix = []
for i, b, br, rip in items:
    b = bytearray(b); here = len(blob)
    if br: b[-4:] = struct.pack('<i', off[br[1]] - (here + len(b)))
    if rip:
        # disp offset may move if earlier bytes changed size: recompute from end (disp is followed by imm bytes, unchanged)
        tail = i.size - (rip[0] + 4)
        fix.append((here + len(b) - tail - 4, rip[1], here + len(b)))
    blob += b
# verify: decode the blob, no rsp disp < 0, same mnemonic sequence
md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md2.detail = True
out = list(md2.disasm(bytes(blob), 0))
assert sum(x.size for x in out) == len(blob) and len(out) == len(ins)
for a, b2 in zip(ins, out):
    assert a.mnemonic == b2.mnemonic or (bytes(a.bytes) == bytes([0x50]) and b2.mnemonic == 'sub') or (a.mnemonic.startswith('j') and b2.mnemonic == a.mnemonic), (a, b2)
    for op in b2.operands:
        if op.type == X.X86_OP_MEM and op.mem.base == X.X86_REG_RSP:
            assert op.mem.disp >= 0, ("negative rsp disp", hex(a.address))
for o, t, e in fix: assert out[[x.address for x in out].index(max(x.address for x in out if x.address < e))].address + out[[x.address for x in out].index(max(x.address for x in out if x.address < e))].size == e
h = 0xcbf29ce484222325
for c in code: h = ((h ^ c) * 0x100000001b3) & 0xffffffffffffffff
print("orig %d bytes, blob %d bytes, %d rip fixups, fnv %#x" % (len(code), len(blob), len(fix), h), file=sys.stderr)
with open(sys.argv[4], 'a', newline='\n') as f:
    f.write("/* 0x%x-0x%x: %d bytes -> %d bytes, %d rip fixups */\n" % (START, END, len(code), len(blob), len(fix)))
    f.write("static const unsigned char %s_blob[%d] = {\n" % (P, len(blob)))
    for k in range(0, len(blob), 16): f.write("    " + ",".join("0x%02x" % c for c in blob[k:k+16]) + ",\n")
    f.write("};\nstatic const RzFixup %s_fix[%d] = {\n" % (P, len(fix)))
    for o, t, e in fix: f.write("    {0x%x, 0x%x, 0x%xull},\n" % (o, e, t))
    f.write("};\n#define %s_ENTRY {0x%xull, 0x%xu, 0x%016xull, %s_blob, sizeof %s_blob, %s_fix, sizeof %s_fix / sizeof %s_fix[0]}\n\n" % (P.upper(), START, END-START, h, P, P, P, P, P))
