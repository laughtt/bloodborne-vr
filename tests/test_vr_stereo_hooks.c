/* The alternate-eye stereo off-axis projection hooks through the real trampoline
 * (src/runtime_hooks.c; docs/vr/findings/22 section 6; patches/Bloodborne.xml "VR Stereo Off-Axis
 * (projections)"). Both windows are the projection builders' epilogues, real eboot bytes that END
 * IN THE BUILDER'S `ret`: replay-after, the handler runs first, then the displaced bytes pop the
 * frame and return to the caller straight from the trampoline. A synthetic builder with the same
 * frame (pushes, 0x68 locals) writes a projection into the caller's buffer; the handler must see
 * it through r15 / r14, shift its P20 when it is the eye camera's, and the caller must get the
 * shifted matrix, its return value and its callee-saved registers back.
 * Build: ninja -C out/gpu vr-stereo-hooks-test (no GPU, no game). */
#define _GNU_SOURCE
#include "hooks.h"
#include "vr_stereo_math.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void *(ABI *BuildFn)(float *out, void *camera);

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static const float kInvHw = 1.0f / 1.2318f, kInvTy = 1.0f, kShift = 0.15936f;
static const float kFov = 1.5708f, kAspect = 1.2318f; /* the eye camera (format B +0x50, +0x64) */
static int g_calls, g_shifted;

/* As src/runtime_vr_stereo.c: SetCamera's builder by the projection's scales, the passes' builder
 * by the camera's FOV and aspect bits, the shift scaled for a tile of the frame. */
static ABI void handler_setcamera(BbHookContext *ctx) {
    float *p = (float *)(uintptr_t)ctx->r15;
    ++g_calls;
    if (vrs_projection_is(p, kInvHw, kInvTy)) {
        p[8] += kShift;
        ++g_shifted;
    }
}
static ABI void handler_pass(BbHookContext *ctx) {
    float *p = (float *)(uintptr_t)ctx->r14;
    const unsigned char *cam = (const unsigned char *)(uintptr_t)ctx->rbx;
    float fov, aspect;
    uint32_t tiles;
    memcpy(&fov, cam + 0x50, 4);
    memcpy(&aspect, cam + 0x64, 4);
    memcpy(&tiles, cam + 0x70, 4);
    ++g_calls;
    if (fov == kFov && aspect == kAspect) {
        p[8] += kShift * (tiles > 1 ? p[0] / kInvHw : 1.0f);
        ++g_shifted;
    }
}

static int hex(const char *s, unsigned char *out) {
    int n = 0;
    for (; s[0] && s[1]; s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (unsigned char)v;
    }
    return n;
}

/* mov dword [reg+disp8], imm32 for the matrix: reg 7 = rdi. */
static int store(unsigned char *c, int n, int disp, float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    c[n++] = 0xC7; c[n++] = 0x47; c[n++] = (unsigned char)disp;
    memcpy(c + n, &bits, 4);
    return n + 4;
}

/* A builder: prologue pushes, sub rsp 0x68, the out pointer in r15 (setcamera) or r14 (pass), the
 * camera in rbx, writes sx, sy, A, 1, B into the output, then the real epilogue window. */
static int build(unsigned char *c, int pass, float sx, float sy, int *window_at, int *window_len) {
    int n = 0;
    c[n++] = 0x55;                                            /* push rbp */
    c[n++] = 0x48; c[n++] = 0x89; c[n++] = 0xE5;              /* mov rbp,rsp */
    c[n++] = 0x41; c[n++] = 0x57;                             /* push r15 */
    if (pass) {
        c[n++] = 0x41; c[n++] = 0x56;                         /* push r14 */
        c[n++] = 0x41; c[n++] = 0x55;                         /* push r13 */
        c[n++] = 0x41; c[n++] = 0x54;                         /* push r12 */
    } else {
        c[n++] = 0x41; c[n++] = 0x56;                         /* push r14 */
    }
    c[n++] = 0x53;                                            /* push rbx */
    c[n++] = 0x48; c[n++] = 0x83; c[n++] = 0xEC; c[n++] = 0x68; /* sub rsp,0x68 */
    if (pass) { c[n++] = 0x49; c[n++] = 0x89; c[n++] = 0xFE; } /* mov r14,rdi */
    else { c[n++] = 0x49; c[n++] = 0x89; c[n++] = 0xFF; }      /* mov r15,rdi */
    c[n++] = 0x48; c[n++] = 0x89; c[n++] = 0xF3;              /* mov rbx,rsi */
    /* clobber the callee-saved registers this frame saves (the epilogue restores them) */
    if (pass) {
        c[n++] = 0x41; c[n++] = 0xBC; c[n++] = 1; c[n++] = 0; c[n++] = 0; c[n++] = 0; /* mov r12d,1 */
        c[n++] = 0x41; c[n++] = 0xBD; c[n++] = 2; c[n++] = 0; c[n++] = 0; c[n++] = 0; /* mov r13d,2 */
    } else {
        c[n++] = 0x41; c[n++] = 0xBE; c[n++] = 3; c[n++] = 0; c[n++] = 0; c[n++] = 0; /* mov r14d,3 */
    }
    n = store(c, n, 0x00, sx);
    n = store(c, n, 0x14, sy);
    n = store(c, n, 0x28, 1.0001f);
    n = store(c, n, 0x2C, 1.0f);
    n = store(c, n, 0x38, -0.05f);
    *window_at = n;
    *window_len = hex(pass ? "4c89f04883c4685b415c415d415e415f5dc3" : "4c89f84883c4685b415e415f5dc3", c + n);
    return n + *window_len;
}

static void run(int pass) {
    const char *what = pass ? "passes' builder 0x25aa69c (r14)" : "SetCamera's builder 0x25aa933 (r15)";
    /* the projection's scales, the camera's FOV and aspect, tiles, the expected P20 */
    struct {
        float sx, sy, fov, aspect;
        uint32_t tiles;
        float expect;
    } cases[4] = {{kInvHw, kInvTy, kFov, kAspect, 0, kShift},          /* the eye camera */
                  {0.7f, 1.0f, 0.75f, 1.7778f, 0, 0.0f},              /* another camera */
                  {kInvHw, kInvTy, kFov, kAspect, 1, kShift},          /* the main view's one tile */
                  {2.0f * kInvHw, 2.0f * kInvTy, kFov, kAspect, 2, 2.0f * kShift}}; /* a tile: x scale doubled */
    for (int k = 0; k < (pass ? 4 : 2); ++k) {
        unsigned char *code = bb_hook_alloc_exec(256);
        int at, len;
        const int n = build(code, pass, cases[k].sx, cases[k].sy, &at, &len);
        (void)n;
        const char *err = bb_hook_install(code, (uint64_t)at, (uint64_t)len, pass ? handler_pass : handler_setcamera, 0, NULL);
        CHECK(err == NULL, err ? err : "install");
        if (err) return;
        CHECK(code[at] == 0xFF && code[at + 1] == 0x25, "the window uses the absolute jump");
        float out[16];
        memset(out, 0, sizeof(out));
        unsigned char camera[0x80];
        memset(camera, 0, sizeof(camera));
        memcpy(camera + 0x50, &cases[k].fov, 4);
        memcpy(camera + 0x64, &cases[k].aspect, 4);
        memcpy(camera + 0x70, &cases[k].tiles, sizeof(cases[k].tiles));
        g_calls = g_shifted = 0;
        uint64_t rbx_before = 0x1111, r12_before = 0x2222, r13_before = 0x3333, r14_before = 0x4444,
                 r15_before = 0x5555, rbx_after, r12_after, r13_after, r14_after, r15_after;
        void *ret;
        float *out_ptr = out;
        unsigned char *cam_ptr = camera, *fn_ptr = code;
        /* Call the builder with known callee-saved registers and read them back after it returned
         * through the replayed epilogue (rsp stays as the compiler keeps it at a call site). */
        __asm__ volatile(
            "mov %[rbx0], %%rbx\n\tmov %[r120], %%r12\n\tmov %[r130], %%r13\n\t"
            "mov %[r140], %%r14\n\tmov %[r150], %%r15\n\t"
            "mov %[out], %%rdi\n\tmov %[cam], %%rsi\n\t"
            "call *%[fn]\n\t"
            "mov %%rax, %[ret]\n\tmov %%rbx, %[rbx1]\n\tmov %%r12, %[r121]\n\t"
            "mov %%r13, %[r131]\n\tmov %%r14, %[r141]\n\tmov %%r15, %[r151]\n\t"
            : [ret] "=m"(ret), [rbx1] "=m"(rbx_after), [r121] "=m"(r12_after), [r131] "=m"(r13_after),
              [r141] "=m"(r14_after), [r151] "=m"(r15_after)
            : [rbx0] "m"(rbx_before), [r120] "m"(r12_before), [r130] "m"(r13_before), [r140] "m"(r14_before),
              [r150] "m"(r15_before), [out] "m"(out_ptr), [cam] "m"(cam_ptr), [fn] "m"(fn_ptr)
            : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
              "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9",
              "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
        char msg[160];
        snprintf(msg, sizeof(msg), "%s case %d: the handler ran once (%d)", what, k, g_calls);
        CHECK(g_calls == 1, msg);
        snprintf(msg, sizeof(msg), "%s case %d: P20 %f (expected %f)", what, k, out[8], cases[k].expect);
        CHECK(fabsf(out[8] - cases[k].expect) < 1e-6f, msg);
        snprintf(msg, sizeof(msg), "%s case %d: the rest of the matrix kept", what, k);
        CHECK(out[0] == cases[k].sx && out[5] == cases[k].sy && out[11] == 1.0f && out[14] == -0.05f, msg);
        snprintf(msg, sizeof(msg), "%s case %d: returned the output (the replayed mov rax and ret)", what, k);
        CHECK(ret == (void *)out, msg);
        snprintf(msg, sizeof(msg), "%s case %d: callee-saved registers restored by the replayed pops", what, k);
        CHECK(rbx_after == rbx_before && r12_after == r12_before && r13_after == r13_before &&
                  r14_after == r14_before && r15_after == r15_before, msg);
    }
}

int main(void) {
    run(0);
    run(1);
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: off-axis projection windows (real eboot epilogues ending in ret, replay-after)\n");
    return 0;
}
