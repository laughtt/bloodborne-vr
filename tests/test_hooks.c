/* Unit tests for the host-call hook trampoline (src/runtime_hooks.c, src/hooks.h).
 *
 * Each test hand-assembles a synthetic SysV guest function into executable memory (taken from the
 * hook allocator so the trampoline lands nearby and rel32/RIP-relative fixups stay in range),
 * installs a hook in the middle of it, calls it, and checks the observable results. The synthetic
 * code deliberately exercises the hard parts: the SysV red zone, a RIP-relative load and a VEX
 * vmovaps inside the displaced window (as in findings/08 H1), preserved flags and XMM, register
 * write-back from the handler, and the replay-before ordering used by H2.
 *
 * Build (standalone, no GPU/Vulkan): clang -std=c11 -I../src test_hooks.c ../src/runtime_hooks.c
 * Returns 0 and prints PASS lines on success; nonzero on the first failure. */
#define _GNU_SOURCE
#include "hooks.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void (ABI *SynFn)(void *);

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

/* ---- Test A: replay-after. Displaced window has a RIP-relative mov and a VEX vmovaps. ---- */
typedef struct {
    uint64_t seen_const;    /* +0x00: eax after the RIP-relative load (proves the fixup) */
    uint64_t r8_after;      /* +0x08: r8 after the handler changed ctx->r8 (write-back) */
    uint64_t flags_after;   /* +0x10: RFLAGS after the hook (CF must survive) */
    uint64_t redzone_after; /* +0x18: the value read back from the red zone */
    unsigned char xmm5_after[16]; /* +0x20: xmm5 after the hook (must survive the handler) */
} OutA;

static void *gA_expect_rdi;
static int gA_calls, gA_saw_rdi, gA_saw_cf, gA_saw_xmm5;
static unsigned char gA_xmmk[16];

static ABI void handlerA(BbHookContext *ctx) {
    gA_calls++;
    gA_saw_rdi = (ctx->rdi == (uint64_t)(uintptr_t)gA_expect_rdi);
    gA_saw_cf = (int)(ctx->rflags & 1);                 /* did the handler see guest CF=1? */
    gA_saw_xmm5 = (memcmp(ctx->ymm[5], gA_xmmk, 16) == 0); /* did it see the guest xmm5 (low 128)? */
    ctx->r8 = 0xA5A5A5A5A5A5A5A5ULL;                    /* write-back: change a register */
    /* Clobber the real xmm5 and CF so the test fails if the stub does not restore them. */
    __asm__ volatile("pxor %%xmm5,%%xmm5\n\tclc" ::: "xmm5", "cc");
}

static int test_replay_after(void) {
    unsigned char code[256]; int n = 0;
    int df_xmmk, df_const, hook_off, off_xmmk, off_const;

    /* prologue (runs in place) */
    code[n++]=0x48; code[n++]=0x89; code[n++]=0x7C; code[n++]=0x24; code[n++]=0xF8; /* mov [rsp-8],rdi (red zone) */
    code[n++]=0x0F; code[n++]=0x10; code[n++]=0x2D; df_xmmk=n; n+=4;                 /* movups xmm5,[rip+XMMK] */
    code[n++]=0x41; code[n++]=0xB8; code[n++]=0x0D; code[n++]=0x60; code[n++]=0x00; code[n++]=0x00; /* mov r8d,0x600D */
    code[n++]=0xF9;                                                                  /* stc (CF=1) */
    hook_off = n;
    /* displaced window (14 bytes): RIP-relative load + VEX vmovaps + mov + nop; no flag/xmm writes */
    code[n++]=0x8B; code[n++]=0x05; df_const=n; n+=4;                                /* mov eax,[rip+CONST] */
    code[n++]=0xC5; code[n++]=0xF8; code[n++]=0x28; code[n++]=0xC1;                  /* vmovaps xmm0,xmm1 (VEX) */
    code[n++]=0x41; code[n++]=0x89; code[n++]=0xC1;                                  /* mov r9d,eax */
    code[n++]=0x90;                                                                  /* nop */
    int orig_len = n - hook_off;
    /* body. Read the red zone FIRST, into r10: the test's own pushfq below writes [rsp-8], so
     * capturing [rsp-8] before it isolates the question to whether the *hook* preserved it. */
    code[n++]=0x4C; code[n++]=0x8B; code[n++]=0x54; code[n++]=0x24; code[n++]=0xF8;  /* mov r10,[rsp-8] */
    code[n++]=0x9C; code[n++]=0x5E;                                                  /* pushfq; pop rsi */
    code[n++]=0x89; code[n++]=0x47; code[n++]=0x00;                                  /* mov [rdi+0],eax */
    code[n++]=0x4C; code[n++]=0x89; code[n++]=0x47; code[n++]=0x08;                  /* mov [rdi+8],r8 */
    code[n++]=0x48; code[n++]=0x89; code[n++]=0x77; code[n++]=0x10;                  /* mov [rdi+0x10],rsi */
    code[n++]=0x4C; code[n++]=0x89; code[n++]=0x57; code[n++]=0x18;                  /* mov [rdi+0x18],r10 */
    code[n++]=0x0F; code[n++]=0x11; code[n++]=0x6F; code[n++]=0x20;                  /* movups [rdi+0x20],xmm5 */
    code[n++]=0xC3;                                                                  /* ret */
    while (n % 16) code[n++]=0x90;
    off_xmmk = n;
    for (int i = 0; i < 16; i++) { gA_xmmk[i] = (unsigned char)(0x10 + i); code[n++] = gA_xmmk[i]; }
    off_const = n;
    uint32_t konst = 0x1234ABCD; memcpy(code + n, &konst, 4); n += 4;

    /* RIP-relative disps are offset-based, so independent of the final load address. */
    int32_t d1 = off_xmmk - (df_xmmk + 4); memcpy(code + df_xmmk, &d1, 4);
    int32_t d2 = off_const - (df_const + 4); memcpy(code + df_const, &d2, 4);

    unsigned char *e = bb_hook_alloc_exec((size_t)n);
    CHECK(e != NULL, "alloc exec A"); if (!e) return 1;
    memcpy(e, code, (size_t)n);

    const char *err = bb_hook_install(e, (uint64_t)hook_off, (uint64_t)orig_len, handlerA, 0, NULL);
    CHECK(err == NULL, err ? err : "install A");
    if (err) return 1;

    OutA out; memset(&out, 0, sizeof out);
    gA_expect_rdi = &out; gA_calls = 0;
    ((SynFn)(void *)e)(&out);

    CHECK(gA_calls == 1, "handler ran exactly once");
    CHECK(gA_saw_rdi, "handler saw rdi = &out");
    CHECK(gA_saw_cf, "handler saw guest CF = 1 (save)");
    CHECK(gA_saw_xmm5, "handler saw guest xmm5 (save)");
    CHECK(out.seen_const == 0x1234ABCD, "RIP-relative displaced load fixed up correctly");
    CHECK(out.r8_after == 0xA5A5A5A5A5A5A5A5ULL, "register write-back (ctx->r8)");
    CHECK((out.flags_after & 1) == 1, "CF preserved across the hook (restore)");
    CHECK(out.redzone_after == (uint64_t)(uintptr_t)&out, "red zone survived");
    CHECK(memcmp(out.xmm5_after, gA_xmmk, 16) == 0, "xmm5 preserved across the hook (restore)");
    if (!failures) printf("PASS: replay-after (red zone, flags, xmm, rip-rel + VEX displaced, write-back)\n");
    return 0;
}

/* ---- Test B: replay-before. Displaced window builds a pointer in rbx; handler writes it. ---- */
typedef struct {
    uint64_t cam0_after;    /* +0x00: *camera after the handler wrote it */
    uint64_t rbx_after;     /* +0x08: rbx after the hook (the produced pointer) */
    unsigned char pad[0x40 - 16];
    uint64_t cam[2];        /* +0x40: the "camera" the displaced lea points at */
} OutB;

static void *gB_cam;
static int gB_calls, gB_saw_rbx;

static ABI void handlerB(BbHookContext *ctx) {
    gB_calls++;
    gB_saw_rbx = (ctx->rbx == (uint64_t)(uintptr_t)gB_cam);
    *(volatile uint64_t *)(uintptr_t)ctx->rbx = 0xC0FFEEULL; /* write guest memory via produced ptr */
}

static int test_replay_before(void) {
    unsigned char code[128]; int n = 0, hook_off;
    code[n++]=0x48; code[n++]=0x31; code[n++]=0xDB;                                  /* xor rbx,rbx (wrong on purpose) */
    hook_off = n;
    /* displaced window (14 bytes), replay-before: produces rbx = &camera */
    code[n++]=0x48; code[n++]=0x8D; code[n++]=0x5F; code[n++]=0x40;                  /* lea rbx,[rdi+0x40] */
    code[n++]=0x48; code[n++]=0x89; code[n++]=0xF8;                                  /* mov rax,rdi */
    code[n++]=0x48; code[n++]=0xC7; code[n++]=0xC1; code[n++]=0x11; code[n++]=0x11; code[n++]=0x00; code[n++]=0x00; /* mov rcx,0x1111 */
    int orig_len = n - hook_off;
    code[n++]=0x48; code[n++]=0x8B; code[n++]=0x03;                                  /* mov rax,[rbx] */
    code[n++]=0x48; code[n++]=0x89; code[n++]=0x07;                                  /* mov [rdi+0],rax */
    code[n++]=0x48; code[n++]=0x89; code[n++]=0x5F; code[n++]=0x08;                  /* mov [rdi+8],rbx */
    code[n++]=0xC3;                                                                  /* ret */

    unsigned char *e = bb_hook_alloc_exec((size_t)n);
    CHECK(e != NULL, "alloc exec B"); if (!e) return 1;
    memcpy(e, code, (size_t)n);

    const char *err = bb_hook_install(e, (uint64_t)hook_off, (uint64_t)orig_len, handlerB, 1, NULL);
    CHECK(err == NULL, err ? err : "install B");
    if (err) return 1;

    OutB out; memset(&out, 0, sizeof out);
    gB_cam = &out.cam[0]; gB_calls = 0;
    ((SynFn)(void *)e)(&out);

    CHECK(gB_calls == 1, "handler B ran exactly once");
    CHECK(gB_saw_rbx, "handler B saw rbx = produced camera pointer (replay-before)");
    CHECK(out.rbx_after == (uint64_t)(uintptr_t)&out.cam[0], "produced pointer survived to the body");
    CHECK(out.cam0_after == 0xC0FFEEULL, "handler's guest-memory write took effect");
    if (!failures) printf("PASS: replay-before (displaced builds state, handler writes guest memory)\n");
    return 0;
}

/* ---- Test C: the shipped registry handler bb_hook_test_counter end to end. ---- */
static int test_registry(void) {
    CHECK(bb_hook_lookup("bb_hook_test_counter", 20) != NULL, "lookup known hook");
    CHECK(bb_hook_lookup("bb_hook_camera_inject", 21) != NULL, "lookup camera hook");
    CHECK(bb_hook_lookup("bb_hook_vr_lod", 14) != NULL, "lookup VR LOD hook");
    CHECK(bb_hook_lookup("bb_hook_vr_stamina_cost", 23) != NULL, "lookup VR stamina cost hook");
    CHECK(bb_hook_lookup("bb_hook_vr_hit_chr", 18) != NULL, "lookup VR hit (character) hook");
    CHECK(bb_hook_lookup("bb_hook_vr_hit_map", 18) != NULL, "lookup VR hit (world) hook");
    CHECK(bb_hook_lookup("bb_hook_vr_hit_obj", 18) != NULL, "lookup VR hit (object) hook");
    CHECK(bb_hook_lookup("bb_hook_vr_stamina_tick", 23) != NULL, "lookup VR stamina tick hook");
    CHECK(bb_hook_lookup("nope", 4) == NULL, "lookup unknown hook is NULL");

    BbHookFn fn = bb_hook_lookup("bb_hook_test_counter", 20);
    if (!fn) return 1;

    unsigned char code[64]; int n = 0, hook_off;
    code[n++]=0x48; code[n++]=0xC7; code[n++]=0xC6; code[n++]=0x00; code[n++]=0x00; code[n++]=0x00; code[n++]=0x00; /* mov rsi,0 */
    hook_off = n;
    code[n++]=0x48; code[n++]=0x89; code[n++]=0xF8;                                  /* mov rax,rdi */
    code[n++]=0x48; code[n++]=0x89; code[n++]=0xF9;                                  /* mov rcx,rdi */
    code[n++]=0x48; code[n++]=0x89; code[n++]=0xFA;                                  /* mov rdx,rdi */
    code[n++]=0x90; code[n++]=0x90; code[n++]=0x90; code[n++]=0x90; code[n++]=0x90;  /* 5x nop -> window = 14 */
    int orig_len = n - hook_off;
    code[n++]=0x48; code[n++]=0x89; code[n++]=0x37;                                  /* mov [rdi],rsi */
    code[n++]=0xC3;                                                                  /* ret */

    unsigned char *e = bb_hook_alloc_exec((size_t)n);
    if (!e) { CHECK(0, "alloc exec C"); return 1; }
    memcpy(e, code, (size_t)n);
    uint64_t before = bb_hook_test_counter_value();
    const char *err = bb_hook_install(e, (uint64_t)hook_off, (uint64_t)orig_len, fn, 0, NULL);
    CHECK(err == NULL, err ? err : "install C");
    if (err) return 1;

    uint64_t out = 0;
    ((SynFn)(void *)e)(&out);
    CHECK(bb_hook_test_counter_value() == before + 1, "bb_hook_test_counter incremented");
    CHECK(out == (uint64_t)(uintptr_t)&out, "bb_hook_test_counter wrote ctx->rsi = ctx->rdi");
    if (!failures) printf("PASS: registry handler bb_hook_test_counter\n");
    return 0;
}

/* ---- Test D: the decoder rejects what it cannot safely relocate. ---- */
static int test_rejects(void) {
    /* A relative jmp (E9 rel32) in the window must be rejected, not copied blind. */
    unsigned char br[16] = { 0xE9,0,0,0,0, 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90 };
    unsigned char *e = bb_hook_alloc_exec(sizeof br);
    if (!e) { CHECK(0, "alloc exec D"); return 1; }
    memcpy(e, br, sizeof br);
    CHECK(bb_hook_install(e, 0, 14, handlerA, 0, NULL) != NULL, "relative branch in window is rejected");
    /* Too-short window is rejected. */
    CHECK(bb_hook_install(e, 0, 4, handlerA, 0, NULL) != NULL, "sub-5-byte window is rejected");
    if (!failures) printf("PASS: decoder rejects relative branches and short windows\n");
    return 0;
}

/* ---- Test E: regression for the in-game crash. The guest holds a live 256-bit YMM value
 * across the hook site; the handler clobbers all YMM (as fprintf etc. do, since the SysV ABI
 * makes YMM0-15 caller-saved). The full register, including its upper 128 bits, must survive.
 * This fails if the trampoline saves only XMM (the original bug that corrupted the camera code). */
static int gE_calls;
static ABI void handlerE(BbHookContext *ctx) {
    gE_calls++;
    (void)ctx;
    /* Destroy every vector register's full width, like a real host callee would. */
    __asm__ volatile("vzeroall" ::: "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
                     "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15","memory");
}

static int test_ymm_preserved(void) {
    unsigned char code[128]; int n = 0, hook_off, df_ymmk, off_ymmk;
    /* prologue: load ymm3 with a known 256-bit value (distinct low and high halves) */
    code[n++]=0xC5; code[n++]=0xFC; code[n++]=0x10; code[n++]=0x1D; df_ymmk=n; n+=4; /* vmovups ymm3,[rip+YMMK] */
    hook_off = n;
    /* 14-byte window that touches neither YMM nor flags */
    code[n++]=0x48; code[n++]=0x89; code[n++]=0xF8;                                  /* mov rax,rdi */
    code[n++]=0x48; code[n++]=0x89; code[n++]=0xF9;                                  /* mov rcx,rdi */
    code[n++]=0x48; code[n++]=0x89; code[n++]=0xFA;                                  /* mov rdx,rdi */
    code[n++]=0x90; code[n++]=0x90; code[n++]=0x90; code[n++]=0x90; code[n++]=0x90;  /* 5x nop -> 14 */
    int orig_len = n - hook_off;
    code[n++]=0xC5; code[n++]=0xFC; code[n++]=0x11; code[n++]=0x1F;                  /* vmovups [rdi],ymm3 */
    code[n++]=0xC3;                                                                  /* ret */
    while (n % 32) code[n++]=0x90;
    off_ymmk = n;
    unsigned char ymmk[32];
    for (int i = 0; i < 32; i++) { ymmk[i] = (unsigned char)(0x41 + i); code[n++] = ymmk[i]; }
    int32_t d = off_ymmk - (df_ymmk + 4); memcpy(code + df_ymmk, &d, 4);

    unsigned char *e = bb_hook_alloc_exec((size_t)n);
    if (!e) { CHECK(0, "alloc exec E"); return 1; }
    memcpy(e, code, (size_t)n);
    const char *err = bb_hook_install(e, (uint64_t)hook_off, (uint64_t)orig_len, handlerE, 0, NULL);
    CHECK(err == NULL, err ? err : "install E");
    if (err) return 1;

    unsigned char out[32]; memset(out, 0, sizeof out);
    gE_calls = 0;
    ((SynFn)(void *)e)(out);
    CHECK(gE_calls == 1, "handler E ran");
    CHECK(memcmp(out, ymmk, 16) == 0, "YMM3 low 128 preserved");
    CHECK(memcmp(out + 16, ymmk + 16, 16) == 0, "YMM3 upper 128 preserved (the in-game crash)");
    if (!failures) printf("PASS: full YMM preserved across the hook\n");
    return 0;
}

/* ---- Test F: the VR LOD window (SetCamera 0x2692049, 15 bytes, replay-before), real bytes:
 * vdivss xmm0,xmm1,xmm0 / mov eax,[rbx+0x114] / vcvtsi2ss xmm1,xmm0,rax. The handler must see
 * the quotient in xmm0 and the converted height in xmm1, and its change to xmm0 must reach the
 * code after the window (patches/Bloodborne.xml "VR LOD Fix (SetCamera)"). ---- */
typedef struct {
    float in0, in1, out0, out1; /* +0x00: xmm0 / xmm1 before, xmm0 / xmm1 after the window */
    unsigned char pad[0x114 - 16];
    int32_t height;             /* +0x114: SetCamera's context height */
} OutF;

static float gF_seen0, gF_seen1;
static void *gF_ctx;
static int gF_saw_rbx;

static ABI void handlerF(BbHookContext *ctx) {
    memcpy(&gF_seen0, ctx->ymm[0], 4);
    memcpy(&gF_seen1, ctx->ymm[1], 4);
    gF_saw_rbx = (ctx->rbx == (uint64_t)(uintptr_t)gF_ctx);
    const float scaled = gF_seen0 * 0.5f;
    memcpy(ctx->ymm[0], &scaled, 4);
}

static int test_lod_window(void) {
    static const unsigned char window[15] = {0xC5, 0xF2, 0x5E, 0xC0, 0x8B, 0x83, 0x14, 0x01, 0x00,
                                             0x00, 0xC4, 0xE1, 0xFA, 0x2A, 0xC8};
    unsigned char code[64]; int n = 0, hook_off;
    code[n++]=0x53;                                                                  /* push rbx */
    code[n++]=0x48; code[n++]=0x89; code[n++]=0xFB;                                  /* mov rbx,rdi */
    code[n++]=0xF3; code[n++]=0x0F; code[n++]=0x10; code[n++]=0x07;                  /* movss xmm0,[rdi] */
    code[n++]=0xF3; code[n++]=0x0F; code[n++]=0x10; code[n++]=0x4F; code[n++]=0x04;  /* movss xmm1,[rdi+4] */
    hook_off = n;
    memcpy(code + n, window, sizeof window); n += (int)sizeof window;
    code[n++]=0xF3; code[n++]=0x0F; code[n++]=0x11; code[n++]=0x47; code[n++]=0x08;  /* movss [rdi+8],xmm0 */
    code[n++]=0xF3; code[n++]=0x0F; code[n++]=0x11; code[n++]=0x4F; code[n++]=0x0C;  /* movss [rdi+12],xmm1 */
    code[n++]=0x5B;                                                                  /* pop rbx */
    code[n++]=0xC3;                                                                  /* ret */

    unsigned char *e = bb_hook_alloc_exec((size_t)n);
    if (!e) { CHECK(0, "alloc exec F"); return 1; }
    memcpy(e, code, (size_t)n);
    const char *err = bb_hook_install(e, (uint64_t)hook_off, sizeof window, handlerF, 1, NULL);
    CHECK(err == NULL, err ? err : "install F");
    if (err) return 1;

    OutF out; memset(&out, 0, sizeof out);
    out.in0 = 2.0f; out.in1 = 6.0f; out.height = 1080;
    gF_ctx = &out;
    ((SynFn)(void *)e)(&out);
    CHECK(gF_saw_rbx, "LOD handler saw rbx = the context");
    CHECK(gF_seen0 == 3.0f, "LOD handler saw xmm0 = xmm1 / xmm0 (displaced vdivss ran first)");
    CHECK(gF_seen1 == 1080.0f, "LOD handler saw xmm1 = the converted height");
    CHECK(out.out0 == 1.5f, "the handler's xmm0 reached the code after the window");
    CHECK(out.out1 == 1080.0f, "xmm1 kept after the window");
    if (!failures) printf("PASS: VR LOD window (real SetCamera bytes, replay-before, xmm0 write-back)\n");
    return 0;
}

/* ---- The stamina hooks' windows (findings/20 1.5, patches/Bloodborne.xml "VR Stamina ..."): the
 * real displaced bytes from the eboot decode and install (absolute jump, no relocation). They are
 * not run: the prologues need their callers' frames. ---- */
static int test_stamina_windows(void) {
    static const struct { const char *name; const char *hex; } w[] = {
        { "cost 0x1e297cb", "498b4508488b80b00300004c8b7820" },
        { "hit chr 0x1cb79e0", "554889e54157415641554154534881ec58040000" },
        { "hit map 0x1cb87ff", "488b108b83f001000089c6c1ee10" }, /* no RIP-relative: 0x1cb87f5 failed in the game */
        { "hit obj 0x1cb8430", "554889e54157415641554154534883ec58" },
        { "tick 0x1cbdb20", "554889e54157415641554154534883ec68" },
    };
    BbHookFn fn = bb_hook_lookup("bb_hook_vr_stamina_cost", 23);
    int ok = 1;
    for (size_t i = 0; i < sizeof(w) / sizeof(*w); ++i) {
        unsigned char *code = bb_hook_alloc_exec(64);
        size_t n = strlen(w[i].hex) / 2;
        for (size_t j = 0; j < n; ++j) {
            unsigned v; sscanf(w[i].hex + 2 * j, "%2x", &v); code[j] = (unsigned char)v;
        }
        code[n] = 0xC3;
        const char *err = bb_hook_install(code, 0, n, fn, 0, NULL);
        if (err) printf("  %s: %s\n", w[i].name, err);
        CHECK(!err, "stamina window installs");
        CHECK(n >= 14 && code[0] == 0xFF && code[1] == 0x25, "stamina window uses the absolute jump");
        ok = ok && !err;
    }
    if (ok && !failures) printf("PASS: VR stamina windows (5 real eboot windows decode and install)\n");
    return 0;
}

/* ---- The arms hooks' window (findings/24 2, patches/Bloodborne.xml "VR Arms IK (AnimationToModel)"):
 * the same 19 bytes at both copies (0x1c37650, 0x1e19bfe) decode and install with the absolute jump,
 * and both names resolve. Not run: the bytes need the copy's frame. ---- */
static int test_arms_window(void) {
    static const char hex[] = "48894580448b6808488b4588c7400801000000";
    BbHookFn a1 = bb_hook_lookup("bb_hook_vr_arms_a1", 18), a2 = bb_hook_lookup("bb_hook_vr_arms_a2", 18);
    CHECK(a1 && a2, "arms hook names resolve");
    unsigned char *code = bb_hook_alloc_exec(64);
    const size_t n = strlen(hex) / 2;
    for (size_t j = 0; j < n; ++j) {
        unsigned v; sscanf(hex + 2 * j, "%2x", &v); code[j] = (unsigned char)v;
    }
    code[n] = 0xC3;
    const char *err = a1 ? bb_hook_install(code, 0, n, a1, 0, NULL) : "no handler";
    if (err) printf("  arms window: %s\n", err);
    CHECK(!err, "arms window installs");
    CHECK(n == 19 && code[0] == 0xFF && code[1] == 0x25, "arms window uses the absolute jump");
    /* A3 (15 bytes, absolute jump) and the sync window (11 bytes ending in ret: rel32 jump) */
    static const struct { const char *name, *hex; size_t len; int abs_jump; } more[] = {
        { "bb_hook_vr_arms_a3", "48894580488b4588448b600c4585e4", 15, 1 },
        { "bb_hook_vr_arms_sync", "488d43184883c4085b5dc3", 11, 0 },
        { "bb_hook_vr_arms_player", "4c8bb3c80400004d85f6", 10, 0 },
    };
    for (size_t i = 0; i < sizeof(more) / sizeof(*more); ++i) {
        BbHookFn fn = bb_hook_lookup(more[i].name, strlen(more[i].name));
        CHECK(fn != NULL, "arms hook name resolves");
        unsigned char *c = bb_hook_alloc_exec(64);
        const size_t m = strlen(more[i].hex) / 2;
        for (size_t j = 0; j < m; ++j) {
            unsigned v; sscanf(more[i].hex + 2 * j, "%2x", &v); c[j] = (unsigned char)v;
        }
        c[m] = 0xC3;
        const char *e2 = fn ? bb_hook_install(c, 0, m, fn, 0, NULL) : "no handler";
        if (e2) printf("  %s window: %s\n", more[i].name, e2);
        CHECK(!e2 && m == more[i].len, "arms window installs");
        CHECK(more[i].abs_jump ? (c[0] == 0xFF && c[1] == 0x25) : c[0] == 0xE9, "arms window jump form");
        err = err ? err : e2;
    }
    if (!err && !failures) printf("PASS: VR arms windows (A1/A2 19, A3 15, sync 11 real eboot bytes decode and install)\n");
    return 0;
}

/* ---- The VR UI's window (findings/26, patches/Bloodborne.xml "VR UI Equip (HUD input)"): the 20 bytes
 * at the entry of the HUD input handler 0x1a5b970 (push rbp ... and rsp,-32) decode and install with the
 * absolute jump, and the name resolves. Not run: the prologue needs its caller's frame. ---- */
static int test_ui_window(void) {
    static const char hex[] = "554889e54157415641554154534881e4e0ffffff";
    BbHookFn fn = bb_hook_lookup("bb_hook_vr_ui_menu", 18);
    CHECK(fn != NULL, "VR UI hook name resolves");
    unsigned char *code = bb_hook_alloc_exec(64);
    const size_t n = strlen(hex) / 2;
    for (size_t j = 0; j < n; ++j) {
        unsigned v; sscanf(hex + 2 * j, "%2x", &v); code[j] = (unsigned char)v;
    }
    code[n] = 0xC3;
    const char *err = fn ? bb_hook_install(code, 0, n, fn, 0, NULL) : "no handler";
    if (err) printf("  ui window: %s\n", err);
    CHECK(!err, "VR UI window installs");
    CHECK(n == 20 && code[0] == 0xFF && code[1] == 0x25, "VR UI window uses the absolute jump");
    if (!err && !failures) printf("PASS: VR UI window (20 real eboot bytes at 0x1a5b970 decode and install)\n");
    return 0;
}

/* ---- The VR gun aim's window (findings/27, patches/Bloodborne.xml "VR Gun Aim (bullet spawn)"): the 20
 * bytes at the entry of SprjBulletIns's launch init 0x1df6ca0 (push rbp ... sub rsp,0x148) decode and
 * install with the absolute jump, and the name resolves. Not run: the prologue needs its caller's
 * frame. ---- */
static int test_gun_window(void) {
    static const char hex[] = "554889e54157415641554154534881ec48010000";
    BbHookFn fn = bb_hook_lookup("bb_hook_vr_gun_aim", 18);
    CHECK(fn != NULL, "VR gun aim hook name resolves");
    unsigned char *code = bb_hook_alloc_exec(64);
    const size_t n = strlen(hex) / 2;
    for (size_t j = 0; j < n; ++j) {
        unsigned v; sscanf(hex + 2 * j, "%2x", &v); code[j] = (unsigned char)v;
    }
    code[n] = 0xC3;
    const char *err = fn ? bb_hook_install(code, 0, n, fn, 0, NULL) : "no handler";
    if (err) printf("  gun window: %s\n", err);
    CHECK(!err, "VR gun aim window installs");
    CHECK(n == 20 && code[0] == 0xFF && code[1] == 0x25, "VR gun aim window uses the absolute jump");
    if (!err && !failures) printf("PASS: VR gun aim window (20 real eboot bytes at 0x1df6ca0 decode and install)\n");
    return 0;
}

/* ---- The VR hit scale's window (findings/32, patches/Bloodborne.xml "VR Hitbox Scale (attack shape)"): the
 * 18 bytes at 0x1c5fa55 in the attack update 0x1c5ed50 (three `mov word [rbx+..], imm16`, no RIP-relative
 * operand) decode, install with the absolute jump and replay: the handler runs, then the three stores. ---- */
static int test_hitscale_window(void) {
    static const char hex[] = "66c7432a380066c74328080066c743100301";
    BbHookFn fn = bb_hook_lookup("bb_hook_vr_hit_scale", 20);
    CHECK(fn != NULL, "VR hit scale hook name resolves");
    unsigned char *code = bb_hook_alloc_exec(64);
    const size_t n = strlen(hex) / 2;
    for (size_t j = 0; j < n; ++j) {
        unsigned v; sscanf(hex + 2 * j, "%2x", &v); code[j] = (unsigned char)v;
    }
    code[n] = 0xC3;
    const char *err = fn ? bb_hook_install(code, 0, n, fn, 0, NULL) : "no handler";
    if (err) printf("  hit scale window: %s\n", err);
    CHECK(!err, "VR hit scale window installs");
    CHECK(n == 18 && code[0] == 0xFF && code[1] == 0x25, "VR hit scale window uses the absolute jump");
    if (!err && !failures) printf("PASS: VR hit scale window (18 real eboot bytes at 0x1c5fa55 decode and install)\n");
    return 0;
}

int main(void) {
    test_replay_after();
    test_replay_before();
    test_registry();
    test_rejects();
    test_ymm_preserved();
    test_lod_window();
    test_stamina_windows();
    test_arms_window();
    test_ui_window();
    test_gun_window();
    test_hitscale_window();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("All hook tests passed\n");
    return 0;
}
