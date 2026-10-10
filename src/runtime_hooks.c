/* Host-call hook infrastructure (see src/hooks.h and docs/vr/PLAN.md Phase 2).
 *
 * This file is named runtime_hooks.c so the loader's existing source globs pick it up with no
 * build-file changes: gpu/CMakeLists.txt uses `file(GLOB BB_RUNTIME ../src/runtime*.c)` for
 * bb-probe and build.sh uses `runtime=(src/runtime*.c)`.
 *
 * What a hook does: it replaces a few bytes at a guest instruction with a jump to a generated
 * trampoline (a "stub"). The stub saves the complete guest CPU context, calls a host function
 * with a pointer to it, writes the (possibly modified) context back, runs the original displaced
 * instructions, and jumps back to the byte after them. The guest never notices the detour.
 *
 * ---------------------------------------------------------------------------------------------
 * ABI and machine assumptions (all guest code here is Bloodborne's decrypted PS4 eboot):
 *
 *  - Guest code is System V AMD64 (PS4). Host code built by clang for Windows is MS x64 by
 *    default, so the host hook function is declared sysv_abi (the ABI macro, like the rest of
 *    the port). The stub therefore passes the context pointer in rdi, needs no 32-byte shadow
 *    space, and relies on the SysV callee-saved set (rbx, rbp, r12-r15, rsp) surviving the call.
 *
 *  - Red zone: a SysV leaf function may keep live data in the 128 bytes below rsp. The hook site
 *    is mid-function, reached by a jump (no return address pushed), so rsp still points at the
 *    guest's red zone. The stub skips it with `lea rsp,[rsp-0x80]` (lea does NOT touch flags)
 *    before writing anything to the stack, and restores rsp exactly, so the red zone is intact.
 *
 *  - Flags: captured with pushfq while still pristine (only lea, which preserves flags, runs
 *    before it) and restored with popfq after every other restore (mov/movups/ldmxcsr do not
 *    change flags), so the guest's flags survive even though the host function clobbers them.
 *
 *  - Stack alignment: the stub 16-aligns rsp (`and rsp,-16`) immediately before the call, as
 *    SysV requires at a call site.
 *
 *  - SSE/AVX: the guest is AVX code (PS4 Jaguar) and the SysV ABI makes all of YMM0-15 (and
 *    XMM0-15) caller-saved, so the host hook function is free to clobber them. The trampoline
 *    therefore saves and restores the FULL 256-bit YMM0-15 (VEX vmovups, unaligned) plus MXCSR,
 *    not just the low 128 bits -- otherwise a live upper half held across the hook site (common in
 *    the camera/matrix code) would be destroyed and the guest would crash downstream. ZMM upper
 *    halves (AVX-512) are not preserved; Jaguar has no AVX-512, so this does not arise.
 *
 *  - fs/gs TLS: the loader rewrites guest `fs:[0]` TCB loads to a gs-based TEB slot; the stub
 *    touches neither segment and runs on the same (host) thread, so host and guest TLS are fine.
 *
 *  - Reachability: rel32 jumps reach +-2 GiB. The default (and recommended) site patch is the
 *    14-byte absolute `jmp [rip+0]; dq target` form, which has no range limit, used whenever the
 *    displaced window is >= 14 bytes. A 5..13 byte window falls back to a 5-byte `E9 rel32` and
 *    the installer fails loudly if the trampoline is out of range. The verified Phase 2 windows
 *    (findings/08: H1 18 B, H2 20 B, H3 32 B) all use the absolute form.
 */
#define _GNU_SOURCE
#include "hooks.h"
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* The trampoline writes/reads the context by byte offset; these must match hooks.h exactly. The
 * 16 GPR fields are in x86 encoding order, so a register's slot is simply (encoding number * 8). */
_Static_assert(offsetof(BbHookContext, rflags) == 0x80, "ctx rflags offset");
_Static_assert(offsetof(BbHookContext, rip) == 0x88, "ctx rip offset");
_Static_assert(offsetof(BbHookContext, mxcsr) == 0x90, "ctx mxcsr offset");
_Static_assert(offsetof(BbHookContext, ymm) == 0x98, "ctx ymm offset");
_Static_assert(sizeof(BbHookContext) == 0x298, "ctx size");

/* Stack frame the stub builds, below the skipped red zone and the pushed flags.
 * CTX is the context area size, 16-byte rounded (0x298 -> 0x2A0). The guest rsp at entry is
 * (current rsp) + CTX + 8 (the pushfq) + 0x80 (the red-zone skip). */
#define HOOK_CTX_SIZE 0x2A0
#define HOOK_GUEST_RSP_DELTA (HOOK_CTX_SIZE + 8 + 0x80)

/* ---- executable-memory bump allocator (trampolines live for the whole process) ---- */
static unsigned char *slab;
static size_t slab_used, slab_cap;

void *bb_hook_alloc_exec(size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (!slab || slab_used + size > slab_cap) {
        size_t cap = size > 0x100000 ? ((size + 0xFFFF) & ~(size_t)0xFFFF) : 0x100000;
#ifdef _WIN32
        void *m = VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
        void *m = mmap(NULL, cap, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) m = NULL;
#endif
        if (!m) return NULL;
        slab = m; slab_used = 0; slab_cap = cap;
    }
    void *p = slab + slab_used;
    slab_used += size;
    return p;
}

/* A 14-byte absolute jump to `target` placed within rel32 reach of `site` (a "jump island"), for
 * 5..13 byte windows whose trampoline slab is out of range (the image can sit more than 2 GiB from
 * the slab). Islands share 64 KiB pages allocated as close to the site as the address space allows.
 * Returns NULL when no page within +-1.9 GiB is free. */
static unsigned char *island_page;
static size_t island_used;
static unsigned char *jump_island(uint64_t site, uint64_t target) {
    const int64_t reach = 0x70000000; /* 1.75 GiB: margin for the 5-byte jump itself */
    if (island_page && island_used + 16 <= 0x10000) {
        const int64_t d = (int64_t)((uint64_t)(uintptr_t)(island_page + island_used) - site);
        if (d > -reach && d < reach) goto place;
    }
    island_page = NULL;
#ifdef _WIN32
    for (uint64_t step = 0x10000; step < (uint64_t)reach && !island_page; step += 0x10000) {
        const uint64_t base = site & ~(uint64_t)0xFFFF;
        const uint64_t tries[2] = {base - step, base + step};
        for (int k = 0; k < 2 && !island_page; ++k) {
            if (tries[k] < 0x10000 || tries[k] > 0x7FFFFFFF0000ull) continue;
            island_page = VirtualAlloc((void *)(uintptr_t)tries[k], 0x10000, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_EXECUTE_READWRITE);
        }
    }
#endif
    if (!island_page) return NULL;
    island_used = 0;
place:;
    unsigned char *p = island_page + island_used;
    island_used += 16;
    p[0] = 0xFF; p[1] = 0x25; p[2] = p[3] = p[4] = p[5] = 0;
    memcpy(p + 6, &target, 8);
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), p, 14);
#endif
    return p;
}

/* ---- tiny x86-64 length decoder for the displaced-instruction subset ----
 * Enough to (a) confirm the displaced window ends on an instruction boundary, (b) locate a
 * RIP-relative disp32 so it can be rebased into the trampoline, and (c) reject anything we do not
 * understand -- relative branches, EVEX, three-byte legacy opcodes -- with a clear message, so a
 * displaced instruction is never copied blind. Covers legacy prefixes, REX, the one-byte map,
 * the common two-byte (0F) map, and VEX.128/.256 (2- and 3-byte), which the camera copy code uses
 * for vmovaps/vmovss (findings/08 H1/H3). RIP-relative is mod==00 && r/m==101 only; [rbp+disp32]
 * (mod==10, r/m==101) is position-independent and needs no fixup. */
typedef struct { int len; int rip_off; } BbInsn; /* rip_off: byte offset of disp32, or -1 */

static int op0f_has_imm8(unsigned op) {
    return op == 0x70 || op == 0x71 || op == 0x72 || op == 0x73 || op == 0xA4 || op == 0xAC ||
           op == 0xBA || op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6;
}

static const char *decode_one(const unsigned char *p, int max, BbInsn *out) {
    int i = 0, o16 = 0, rexw = 0;
    out->rip_off = -1;
    while (i < max) {
        unsigned char b = p[i];
        if (b == 0x66) { o16 = 1; i++; }
        else if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
                 b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) i++;
        else break;
    }
    if (i >= max) return "truncated instruction in displaced bytes";

    int has_modrm = 0, imm = 0, is_vex = 0, map = 0; /* map: 0=1-byte, 1=0F, 2=0F38, 3=0F3A */
    const int IMMZ = o16 ? 2 : 4;
    unsigned op = 0;

    if (p[i] == 0x62) return "EVEX instruction in displaced bytes (unsupported; pick a different hook offset)";
    if (p[i] == 0xC5) { if (i + 1 >= max) return "truncated 2-byte VEX"; i += 2; is_vex = 1; map = 1; }
    else if (p[i] == 0xC4) {
        if (i + 2 >= max) return "truncated 3-byte VEX";
        map = p[i + 1] & 0x1F; i += 3; is_vex = 1;
        if (map < 1 || map > 3) return "unsupported VEX opcode map";
    }

    if (is_vex) {
        if (i >= max) return "truncated VEX opcode";
        op = p[i++];
        if (map == 3) { has_modrm = 1; imm = 1; }              /* 0F3A: all have imm8 */
        else if (map == 2) has_modrm = 1;                      /* 0F38: ModRM, no imm */
        else if (op == 0x77) has_modrm = 0;                    /* vzeroupper / vzeroall */
        else { has_modrm = 1; if (op0f_has_imm8(op)) imm = 1; }
    } else {
        if ((p[i] & 0xF0) == 0x40) { if (p[i] & 8) rexw = 1; i++; if (i >= max) return "truncated after REX"; }
        op = p[i++];
        if (op == 0x0F) {
            map = 1;
            if (i >= max) return "truncated 0F instruction";
            unsigned o2 = p[i++];
            if (o2 == 0x38 || o2 == 0x3A) return "three-byte-opcode instruction in displaced bytes (unsupported)";
            if (o2 >= 0x80 && o2 <= 0x8F) return "relative branch (0F Jcc) in displaced bytes (unsupported)";
            switch (o2) {
            case 0x05: return "syscall in displaced bytes (unsupported)";
            case 0x0B: return "ud2 in displaced bytes (unsupported)";
            case 0x31: case 0xA2: case 0x77: break; /* rdtsc, cpuid, emms: no ModRM, no imm */
            default: has_modrm = 1; if (op0f_has_imm8(o2)) imm = 1; break;
            }
        } else {
            switch (op) {
            case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: case 0x0A: case 0x0B:
            case 0x10: case 0x11: case 0x12: case 0x13: case 0x18: case 0x19: case 0x1A: case 0x1B:
            case 0x20: case 0x21: case 0x22: case 0x23: case 0x28: case 0x29: case 0x2A: case 0x2B:
            case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39: case 0x3A: case 0x3B:
            case 0x63: case 0x84: case 0x85: case 0x86: case 0x87:
            case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
            case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xFE: case 0xFF:
                has_modrm = 1; break;
            case 0x69: has_modrm = 1; imm = IMMZ; break;
            case 0x6B: case 0x80: case 0x83: case 0xC0: case 0xC1: case 0xC6: has_modrm = 1; imm = 1; break;
            case 0x81: case 0xC7: has_modrm = 1; imm = IMMZ; break;
            case 0xF6: case 0xF7: has_modrm = 1; break; /* imm depends on reg field (below) */
            case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C:
            case 0xA8: case 0x6A: imm = 1; break;
            case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D:
            case 0xA9: case 0x68: imm = IMMZ; break;
            case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
                imm = 1; break;
            case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
                imm = rexw ? 8 : IMMZ; break;
            case 0xC2: imm = 2; break;
            case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
            case 0x98: case 0x99: case 0x9B: case 0x9C: case 0x9D: case 0xC3: case 0xC9:
            case 0xF5: case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD:
                break;
            case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
            case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE8: case 0xE9: case 0xEB:
                return "relative branch in displaced bytes (unsupported; pick a window with no jumps/calls)";
            default: return "unsupported opcode in displaced bytes";
            }
        }
    }

    if (has_modrm) {
        if (i >= max) return "truncated ModRM";
        unsigned m = p[i]; int mod = m >> 6, rm = m & 7; i++;
        int sib_base5 = 0;
        if (mod != 3 && rm == 4) { if (i >= max) return "truncated SIB"; sib_base5 = (p[i] & 7) == 5; i++; }
        int dispsz = 0, riprel = 0;
        if (mod == 0) {
            if (rm == 5) { dispsz = 4; riprel = 1; }       /* RIP-relative (64-bit mode) */
            else if (rm == 4 && sib_base5) dispsz = 4;      /* SIB, no base: absolute disp32 */
        } else if (mod == 1) dispsz = 1;
        else if (mod == 2) dispsz = 4;
        if (riprel) out->rip_off = i;                       /* disp32 starts here (no SIB when rm==5) */
        if (i + dispsz > max) return "truncated displacement";
        i += dispsz;
        if (map == 0 && op == 0xF6) { int reg = (m >> 3) & 7; if (reg == 0 || reg == 1) imm = 1; }
        if (map == 0 && op == 0xF7) { int reg = (m >> 3) & 7; if (reg == 0 || reg == 1) imm = IMMZ; }
    }
    if (i + imm > max) return "truncated immediate";
    i += imm;
    out->len = i;
    return NULL;
}

static const char *decode_region(const unsigned char *p, int len, int *rip_off, int *n_rip) {
    int at = 0;
    *n_rip = 0;
    while (at < len) {
        BbInsn in;
        const char *e = decode_one(p + at, len - at, &in);
        if (e) return e;
        if (in.rip_off >= 0) rip_off[(*n_rip)++] = at + in.rip_off;
        at += in.len;
    }
    if (at != len) return "displaced bytes do not end on an instruction boundary";
    return NULL;
}

/* ---- code emitters (hand-assembled, documented inline like probe.c's import thunks) ---- */
static void w8(unsigned char **q, unsigned char v) { *(*q)++ = v; }
static void w32(unsigned char **q, uint32_t v) { memcpy(*q, &v, 4); *q += 4; }
static void w64(unsigned char **q, uint64_t v) { memcpy(*q, &v, 8); *q += 8; }

/* `op` reg <-> [rsp+disp32]: 0x89 store (mov [rsp+d],reg), 0x8B load (mov reg,[rsp+d]),
 * 0x8D lea (lea reg,[rsp+d]). ModRM mod=10, r/m=100 (SIB), SIB base=rsp index=none. */
static void rsp_mem(unsigned char **q, unsigned char op, int reg, int32_t disp) {
    w8(q, (unsigned char)(0x48 | (reg >= 8 ? 4 : 0)));
    w8(q, op);
    w8(q, (unsigned char)(0x84 | ((reg & 7) << 3)));
    w8(q, 0x24);
    w32(q, (uint32_t)disp);
}
/* vmovups [rsp+d],ymmN (store=1) / vmovups ymmN,[rsp+d] (store=0): full 256-bit, 2-byte VEX
 * (C5), unaligned. The VEX R bit (inverted) selects ymm8-15; L=1 selects 256-bit; pp=00 (no
 * prefix) selects the "ps"/movups form. */
static void rsp_ymm(unsigned char **q, int store, int n, int32_t disp) {
    w8(q, 0xC5);
    w8(q, (unsigned char)((n >= 8 ? 0x00 : 0x80) | 0x7C)); /* R, vvvv=1111, L=1, pp=00 */
    w8(q, (unsigned char)(store ? 0x11 : 0x10));
    w8(q, (unsigned char)(0x84 | ((n & 7) << 3)));
    w8(q, 0x24);
    w32(q, (uint32_t)disp);
}

/* Save the full guest context, call fn(&ctx) in the SysV convention, then restore the context.
 * On entry rsp is the guest rsp with the red zone still below it; on exit rsp is the same guest
 * rsp (red zone preserved) and every register/flag/xmm holds its saved-then-possibly-modified
 * value. Flag-changing instructions appear only before the first pushfq and are finished before
 * the final popfq, so the guest's flags round-trip intact. */
static void emit_save_call_restore(unsigned char **q, uint64_t site_abs, BbHookFn fn) {
    rsp_mem(q, 0x8D, 4, -0x80);                 /* lea rsp,[rsp-0x80]  (skip red zone; no flags) */
    w8(q, 0x9C);                                /* pushfq              (flags still pristine) */
    w8(q, 0x48); w8(q, 0x81); w8(q, 0xEC); w32(q, HOOK_CTX_SIZE); /* sub rsp, CTX */
    for (int r = 0; r < 16; r++) if (r != 4) rsp_mem(q, 0x89, r, r * 8); /* save GPRs (not rsp) */
    rsp_mem(q, 0x8D, 0, HOOK_CTX_SIZE + 8 + 0x80); /* lea rax,[rsp+guest_rsp] */
    rsp_mem(q, 0x89, 0, 0x20);                  /*   mov [rsp+OFF_RSP],rax */
    rsp_mem(q, 0x8B, 0, HOOK_CTX_SIZE);         /* mov rax,[rsp+CTX]  (the pushfq'd flags) */
    rsp_mem(q, 0x89, 0, 0x80);                  /*   mov [rsp+OFF_RFLAGS],rax */
    for (int n = 0; n < 16; n++) rsp_ymm(q, 1, n, 0x98 + n * 32); /* save YMM0..15 (full 256-bit) */
    w8(q, 0x0F); w8(q, 0xAE); w8(q, 0x9C); w8(q, 0x24); w32(q, 0x90); /* stmxcsr [rsp+MXCSR] */
    w8(q, 0x48); w8(q, 0xB8); w64(q, site_abs); rsp_mem(q, 0x89, 0, 0x88); /* movabs rax,site; mov [rsp+RIP],rax */

    w8(q, 0x48); w8(q, 0x89); w8(q, 0xE7);      /* mov rdi,rsp  (arg0 = &ctx) */
    w8(q, 0x48); w8(q, 0x89); w8(q, 0xE3);      /* mov rbx,rsp  (ctx ptr; rbx is callee-saved) */
    w8(q, 0x48); w8(q, 0x83); w8(q, 0xE4); w8(q, 0xF0); /* and rsp,-16 */
    w8(q, 0x48); w8(q, 0xB8); w64(q, (uint64_t)(uintptr_t)fn); /* movabs rax,fn */
    w8(q, 0xFF); w8(q, 0xD0);                   /* call rax */
    w8(q, 0x48); w8(q, 0x89); w8(q, 0xDC);      /* mov rsp,rbx  (restore &ctx) */

    w8(q, 0x0F); w8(q, 0xAE); w8(q, 0x94); w8(q, 0x24); w32(q, 0x90); /* ldmxcsr [rsp+MXCSR] */
    for (int n = 0; n < 16; n++) rsp_ymm(q, 0, n, 0x98 + n * 32); /* load YMM0..15 (full 256-bit) */
    for (int r = 1; r < 16; r++) if (r != 4) rsp_mem(q, 0x8B, r, r * 8); /* load GPRs (not rax/rsp) */
    rsp_mem(q, 0x8B, 0, 0x80);                  /* mov rax,[rsp+OFF_RFLAGS] */
    w8(q, 0x50); w8(q, 0x9D);                   /* push rax; popfq  (flags restored; no flag ops after) */
    rsp_mem(q, 0x8B, 0, 0x00);                  /* mov rax,[rsp+OFF_RAX] */
    rsp_mem(q, 0x8D, 4, HOOK_GUEST_RSP_DELTA);  /* lea rsp,[rsp+guest_rsp]  (no flag change) */
}

/* Copy the displaced bytes into the stub, rebasing any RIP-relative disp32 so it still points at
 * the same absolute target from the trampoline's location. Returns an error string or NULL. */
static const char *emit_displaced(unsigned char **q, const unsigned char *site, uint64_t site_abs,
                                  uint64_t orig_len, const int *rip_off, int n_rip) {
    unsigned char *disp_area = *q;
    memcpy(*q, site, orig_len); *q += orig_len;
    int64_t delta = (int64_t)(site_abs - (uint64_t)(uintptr_t)disp_area);
    for (int k = 0; k < n_rip; k++) {
        int32_t old; memcpy(&old, disp_area + rip_off[k], 4);
        int64_t nd = (int64_t)old + delta; /* same absolute target from the new location */
        if (nd < INT32_MIN || nd > INT32_MAX)
            return "RIP-relative displaced instruction is out of range from the trampoline";
        int32_t ndd = (int32_t)nd; memcpy(disp_area + rip_off[k], &ndd, 4);
    }
    return NULL;
}

const char *bb_hook_install(unsigned char *image, uint64_t site_offset, uint64_t orig_len,
                            BbHookFn fn, int replay_before, void **trampoline) {
    if (trampoline) *trampoline = NULL;
    if (!fn) return "null hook function";
    if (orig_len < 5) return "hook needs at least 5 displaced bytes (jump size)";
    if (orig_len > 32) return "hook displaced length too large (max 32)";
    unsigned char *site = image + site_offset;
    int rip_off[32], n_rip = 0;
    const char *e = decode_region(site, (int)orig_len, rip_off, &n_rip);
    if (e) return e;

    unsigned char *stub = bb_hook_alloc_exec(768 + orig_len);
    if (!stub) return "cannot allocate trampoline memory";
    unsigned char *q = stub;
    uint64_t site_abs = (uint64_t)(uintptr_t)site;

    if (replay_before) {
        /* The displaced bytes run first, with the live guest registers, and build the state the
         * handler reads (H2: a freshly constructed camera in rbx). They may include a balanced
         * `call`, which uses the red zone exactly as the guest would here, so it replays as-is. */
        e = emit_displaced(&q, site, site_abs, orig_len, rip_off, n_rip);
        if (e) return e;
        emit_save_call_restore(&q, site_abs, fn);
    } else {
        emit_save_call_restore(&q, site_abs, fn);
        e = emit_displaced(&q, site, site_abs, orig_len, rip_off, n_rip);
        if (e) return e;
    }
    w8(&q, 0xFF); w8(&q, 0x25); w32(&q, 0); w64(&q, site_abs + orig_len); /* jmp [rip+0]; dq resume */

    /* --- patch the hook site --- */
    if (orig_len >= 14) {
        /* placement-independent absolute jump */
        site[0] = 0xFF; site[1] = 0x25; site[2] = site[3] = site[4] = site[5] = 0;
        uint64_t sp = (uint64_t)(uintptr_t)stub; memcpy(site + 6, &sp, 8);
        for (uint64_t j = 14; j < orig_len; j++) site[j] = 0x90;
    } else {
        int64_t rel = (int64_t)((uint64_t)(uintptr_t)stub - (site_abs + 5));
        if (rel < INT32_MIN || rel > INT32_MAX) {
            /* the slab is too far: jump through an island near the site */
            unsigned char *island = jump_island(site_abs, (uint64_t)(uintptr_t)stub);
            if (!island) return "trampoline out of rel32 range and no jump island near the site; use a 14+ byte hook window";
            rel = (int64_t)((uint64_t)(uintptr_t)island - (site_abs + 5));
            if (rel < INT32_MIN || rel > INT32_MAX) return "jump island out of rel32 range";
        }
        site[0] = 0xE9; int32_t r = (int32_t)rel; memcpy(site + 1, &r, 4);
        for (uint64_t j = 5; j < orig_len; j++) site[j] = 0x90;
    }
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), stub, (SIZE_T)(q - stub));
    FlushInstructionCache(GetCurrentProcess(), site, (SIZE_T)orig_len);
#endif
    if (trampoline) *trampoline = stub;
    return NULL;
}

/* ---- registry ---- */
static uint64_t test_counter;
uint64_t bb_hook_test_counter_value(void) { return test_counter; }

/* Built-in test hook: proves the plumbing. Increments a counter and demonstrates register
 * write-back by copying the first integer argument (rdi) into rsi. */
static ABI void hook_test_counter(BbHookContext *ctx) {
    test_counter++;
    ctx->rsi = ctx->rdi;
}

/* Logs the first time it fires at each distinct hook site, then stays quiet (so a hook in a
 * per-frame draw step does not flood the log). For the Phase 2 runtime check: install it at H1
 * and H2 via the disabled XML entries and confirm each trampoline runs with a sane camera pointer
 * in rbx. It does not modify the guest state. The seen-site list is a best-effort filter; a race
 * only costs a duplicate line. */
static ABI void hook_log_once(BbHookContext *ctx) {
    static uint64_t seen[16];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == ctx->rip) return;
    if (nseen < (int)(sizeof(seen) / sizeof(*seen))) seen[nseen++] = ctx->rip;
    fprintf(stderr, "bb_hook_log_once: fired at guest rip=0x%llx rbx=0x%llx r12=0x%llx rdi=0x%llx\n",
            (unsigned long long)ctx->rip, (unsigned long long)ctx->rbx,
            (unsigned long long)ctx->r12, (unsigned long long)ctx->rdi);
    fflush(stderr);
}

/* The Phase 2 camera seam (docs/vr/PLAN.md Phase 2; findings/08 H1 at 0x22de113 replay-after,
 * H2 at 0x22de386 replay-before). The handlers live in src/runtime_vr_head.c (head tracking,
 * BB_VR_HEAD=1), which registers them at startup through bb_hook_set_camera_impl; until then (and
 * in tests/test_hooks.c, which links this file alone) the camera hooks are no-ops. They read the
 * render-camera pointer the draw step just built (rbx) and write the composed camera back into
 * guest memory; nothing is restored because the draw step copies the camera before using it
 * (findings/08 section 7). The LOD hook (SetCamera 0x2692049, replay-before) keeps the game's LOD
 * distances when the injected camera has a wider FOV. */
static BbHookFn camera_impl[3];
void bb_hook_set_camera_impl(BbHookFn h1, BbHookFn h2, BbHookFn lod) {
    camera_impl[0] = h1;
    camera_impl[1] = h2;
    camera_impl[2] = lod;
}
static ABI void hook_camera_inject(BbHookContext *ctx) {
    if (camera_impl[0]) camera_impl[0](ctx); /* rbx = the draw step's private PersCam */
}
static ABI void hook_camera_inject_fmtb(BbHookContext *ctx) {
    if (camera_impl[1]) camera_impl[1](ctx); /* rbx = the main context's format-B camera */
}
static ABI void hook_vr_lod(BbHookContext *ctx) {
    if (camera_impl[2]) camera_impl[2](ctx); /* rbx = the draw context, xmm0 = the FOV's LOD term */
}

/* First person (src/runtime_vr_fp.c, findings/16): in the in-game step at 0x1d3be5a (replay-after,
 * r13 = the step), right after the camera director built ChrCam ([r13+0x128]) and before the step
 * copies it on, the handler turns ChrCam to the first-person heading so the game moves along it. */
static BbHookFn fp_impl, step_extra;
void bb_hook_set_fp_impl(BbHookFn cam) { fp_impl = cam; }
/* The same site also serves the VR UI's game-thread work (src/runtime_vr_ui.c, findings/26): it runs
 * every gameplay frame on the game thread, after first person's handler. */
void bb_hook_set_step_extra(BbHookFn step) { step_extra = step; }
static ABI void hook_vr_fp_cam(BbHookContext *ctx) {
    if (fp_impl) fp_impl(ctx);
    if (step_extra) step_extra(ctx);
}

/* "Misses are free" stamina (src/runtime_vr_stamina.c, findings/20 1.5 and 6.2 G3), all replay-after:
 * the attack cost (0x1e297cb in 0x1e293b0, r12d = the cost, [r13+8] = the character), the three hit
 * handlers of the hit dispatcher 0x1c5b200 (rdi = hit record, rdx = the attacker) and the stamina
 * regen (0x1cbdb20, rdi = the character): the expiry tick. */
static BbHookFn stamina_impl[5];
void bb_hook_set_stamina_impl(BbHookFn cost, BbHookFn hit_chr, BbHookFn hit_map, BbHookFn hit_obj,
                              BbHookFn tick) {
    stamina_impl[0] = cost;
    stamina_impl[1] = hit_chr;
    stamina_impl[2] = hit_map;
    stamina_impl[3] = hit_obj;
    stamina_impl[4] = tick;
}
static ABI void hook_vr_stamina_cost(BbHookContext *ctx) {
    if (stamina_impl[0]) stamina_impl[0](ctx);
}
static ABI void hook_vr_hit_chr(BbHookContext *ctx) {
    if (stamina_impl[1]) stamina_impl[1](ctx);
}
static ABI void hook_vr_hit_map(BbHookContext *ctx) {
    if (stamina_impl[2]) stamina_impl[2](ctx);
}
static ABI void hook_vr_hit_obj(BbHookContext *ctx) {
    if (stamina_impl[3]) stamina_impl[3](ctx);
}
static ABI void hook_vr_stamina_tick(BbHookContext *ctx) {
    if (stamina_impl[4]) stamina_impl[4](ctx);
}

/* Alternate-eye stereo, off-axis frusta (src/runtime_vr_stereo.c, findings/22 section 6), both
 * replay-after at the projection builders' epilogues (the displaced bytes end in the builder's
 * `ret`, which the trampoline replays): SetCamera's builder 0x25aa6c0 at 0x25aa933 (out = r15) and
 * the passes' builder 0x25aa290 at 0x25aa69c (out = r14, camera = rbx). */
static BbHookFn stereo_impl[2];
void bb_hook_set_stereo_impl(BbHookFn proj_setcamera, BbHookFn proj_pass) {
    stereo_impl[0] = proj_setcamera;
    stereo_impl[1] = proj_pass;
}
static ABI void hook_vr_proj_setcamera(BbHookContext *ctx) {
    if (stereo_impl[0]) stereo_impl[0](ctx);
}
static ABI void hook_vr_proj_pass(BbHookContext *ctx) {
    if (stereo_impl[1]) stereo_impl[1](ctx);
}

/* VR arms (src/runtime_vr_arms.c, findings/24), replay-after at both AnimationToModel copies right
 * after getSyncedPoseModelSpace (0x1c37650 in 0x1c375d0, 0x1e19bfe in 0x1e19bc0): rax = the pose's
 * modelPose hkArray; the handler rewrites the player's arm bones before the copy reads them. */
static BbHookFn arms_impl[5];
void bb_hook_set_arms_impl(BbHookFn a1, BbHookFn a2, BbHookFn a3, BbHookFn sync, BbHookFn player) {
    arms_impl[0] = a1;
    arms_impl[1] = a2;
    arms_impl[2] = a3;
    arms_impl[3] = sync;
    arms_impl[4] = player;
}
static ABI void hook_vr_arms_player(BbHookContext *ctx) {
    if (arms_impl[4]) arms_impl[4](ctx);
}
static ABI void hook_vr_arms_a3(BbHookContext *ctx) {
    if (arms_impl[2]) arms_impl[2](ctx);
}
static ABI void hook_vr_arms_sync(BbHookContext *ctx) {
    if (arms_impl[3]) arms_impl[3](ctx);
}
static ABI void hook_vr_arms_a1(BbHookContext *ctx) {
    if (arms_impl[0]) arms_impl[0](ctx);
}
static ABI void hook_vr_arms_a2(BbHookContext *ctx) {
    if (arms_impl[1]) arms_impl[1](ctx);
}

/* VR UI (src/runtime_vr_ui.c, findings/26): replay-after at the entry of the HUD input handler
 * 0x1a5b970 (the game thread, the phase where the game acts on menu input); the handler may call the
 * game's own equip-from-menu command 0x1f63e00 for a request from the headset dashboard. */
static BbHookFn ui_impl;
void bb_hook_set_ui_impl(BbHookFn menu) { ui_impl = menu; }
static ABI void hook_vr_ui_menu(BbHookContext *ctx) {
    if (ui_impl) ui_impl(ctx);
}

/* VR gun aim (src/runtime_vr_gun.c, findings/27): replay-after at the entry of SprjBulletIns's launch
 * init 0x1df6ca0 (rsi = descriptor, rdx = target, rcx = launch matrix; both caller stack copies). */
static BbHookFn gun_impl;
void bb_hook_set_gun_impl(BbHookFn launch) { gun_impl = launch; }
static ABI void hook_vr_gun_aim(BbHookContext *ctx) {
    if (gun_impl) gun_impl(ctx);
}

/* VR hit scale (src/runtime_vr_hitscale.c, findings/32): replay-after at 0x1c5fa55 in the attack update
 * 0x1c5ed50 (rbx = the capsule shape being rebuilt, r12 = the attack entry, rbp = the update's frame). */
static BbHookFn hitscale_impl;
void bb_hook_set_hitscale_impl(BbHookFn shape) { hitscale_impl = shape; }
static ABI void hook_vr_hit_scale(BbHookContext *ctx) {
    if (hitscale_impl) hitscale_impl(ctx);
}

/* VR attack speed (src/runtime_vr_atkspeed.c): replay-after at 0x1cbd711 in the ChrIns update 0x1cbd6a0
 * (r13 = the ChrIns, xmm0 = the time step about to be stored at ChrIns+0xE0). */
static BbHookFn atkspeed_impl;
void bb_hook_set_atkspeed_impl(BbHookFn dt) { atkspeed_impl = dt; }
static ABI void hook_vr_atk_speed(BbHookContext *ctx) {
    if (atkspeed_impl) atkspeed_impl(ctx);
}

static const struct { const char *name; BbHookFn fn; } registry[] = {
    { "bb_hook_test_counter", hook_test_counter },
    { "bb_hook_log_once", hook_log_once },
    { "bb_hook_camera_inject", hook_camera_inject },
    { "bb_hook_camera_inject_fmtb", hook_camera_inject_fmtb },
    { "bb_hook_vr_lod", hook_vr_lod },
    { "bb_hook_vr_fp_cam", hook_vr_fp_cam },
    { "bb_hook_vr_stamina_cost", hook_vr_stamina_cost },
    { "bb_hook_vr_hit_chr", hook_vr_hit_chr },
    { "bb_hook_vr_hit_map", hook_vr_hit_map },
    { "bb_hook_vr_hit_obj", hook_vr_hit_obj },
    { "bb_hook_vr_stamina_tick", hook_vr_stamina_tick },
    { "bb_hook_vr_proj_setcamera", hook_vr_proj_setcamera },
    { "bb_hook_vr_proj_pass", hook_vr_proj_pass },
    { "bb_hook_vr_arms_a1", hook_vr_arms_a1 },
    { "bb_hook_vr_arms_a2", hook_vr_arms_a2 },
    { "bb_hook_vr_arms_a3", hook_vr_arms_a3 },
    { "bb_hook_vr_arms_sync", hook_vr_arms_sync },
    { "bb_hook_vr_arms_player", hook_vr_arms_player },
    { "bb_hook_vr_ui_menu", hook_vr_ui_menu },
    { "bb_hook_vr_gun_aim", hook_vr_gun_aim },
    { "bb_hook_vr_hit_scale", hook_vr_hit_scale },
    { "bb_hook_vr_atk_speed", hook_vr_atk_speed },
};

BbHookFn bb_hook_lookup(const char *name, size_t len) {
    for (size_t i = 0; i < sizeof(registry) / sizeof(*registry); i++)
        if (strlen(registry[i].name) == len && !memcmp(registry[i].name, name, len))
            return registry[i].fn;
    return NULL;
}
