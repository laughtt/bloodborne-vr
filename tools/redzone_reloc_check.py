# Usage: python -I tools/redzone_reloc_check.py <eboot.elf> <header>
# Strict per-instruction check of src/redzone_blobs.h against the original eboot functions; must report 0 mismatches.
# Part of the red zone guard (src/runtime_redzone_guard.c).
# Strict static check: every relocated instruction equals the original except rsp disp +0x80, frame size, branch/rip targets.
import sys, re, struct, capstone
from capstone import x86_const as X
elf, hdr = sys.argv[1], open(sys.argv[2]).read()
d = open(elf,'rb').read()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
FAKE_IMG = 0x800000000; FAKE_BLOB = 0x7f000000  # blob placed at FAKE_IMG - offset for rip check
for m in re.finditer(r"static const unsigned char rz_([0-9a-f]+)_blob\[\d+\] = \{(.*?)\};\nstatic const RzFixup rz_\1_fix\[\d+\] = \{(.*?)\};", hdr, re.S):
    start = int(m.group(1),16)
    blob = bytearray(int(x,16) for x in re.findall(r"0x([0-9a-f]{2})\b", m.group(2)))
    fix = [tuple(int(v,16) for v in t) for t in re.findall(r"\{0x([0-9a-f]+), 0x([0-9a-f]+), 0x([0-9a-f]+)ull\}", m.group(3))]
    base = FAKE_IMG + 0x6000000
    for o, e, t in fix: blob[o:o+4] = struct.pack('<i', (FAKE_IMG + t) - (base + e))
    ent = re.search(r"RZ_%s_ENTRY \{0x[0-9a-f]+ull, 0x([0-9a-f]+)u" % m.group(1).upper(), hdr); size = int(ent.group(1),16)
    orig = list(md.disasm(d[0x4000+start:0x4000+start+size], FAKE_IMG + start))
    new = list(md.disasm(bytes(blob), base))
    assert len(orig) == len(new)
    omap = {}; nmap = {}
    for k,(a,b) in enumerate(zip(orig,new)): omap[a.address]=k; nmap[b.address]=k
    bad = 0
    for a, b in zip(orig, new):
        if bytes(a.bytes) == bytes([0x50]) and bytes(b.bytes) == bytes([0x48,0x81,0xec,0x88,0,0,0]): continue
        if a.mnemonic != b.mnemonic: bad += 1; print("mn", a, b); continue
        if a.mnemonic.startswith('j'):
            if omap[a.operands[0].imm] != nmap[b.operands[0].imm]: bad += 1; print("br", a, b)
            continue
        if len(a.operands) != len(b.operands): bad += 1; print("nops", a, b); continue
        for oa, ob in zip(a.operands, b.operands):
            if oa.type != ob.type: bad += 1; print("type", a, b); continue
            if oa.type == X.X86_OP_REG and oa.reg != ob.reg: bad += 1; print("reg", a, b)
            if oa.type == X.X86_OP_IMM:
                exp = oa.imm + (0x80 if a.mnemonic in ('sub','add') and a.operands[0].reg == X.X86_REG_RSP else 0)
                if ob.imm != exp: bad += 1; print("imm", a, b)
            if oa.type == X.X86_OP_MEM:
                ma, mb = oa.mem, ob.mem
                if (ma.base, ma.index, ma.scale, ma.segment, oa.size) != (mb.base, mb.index, mb.scale, mb.segment, ob.size): bad += 1; print("mem", a, b); continue
                if ma.base == X.X86_REG_RIP:
                    if a.address + a.size + ma.disp != b.address + b.size + mb.disp: bad += 1; print("rip", a, b)
                elif ma.base == X.X86_REG_RSP:
                    if mb.disp != ma.disp + 0x80: bad += 1; print("rsp", a, b)
                elif ma.disp != mb.disp: bad += 1; print("disp", a, b)
    print("%x: %d instructions, %d mismatches" % (start, len(orig), bad))
