/* VR attack speed (BB_VR_ATK_SPEED=<mult> or atk_speed=<mult> in out/fp.txt, default off, docs/vr/findings/20
 * section 4 "never raise animation speed" is the reason it is opt-in): the player's attack and recovery
 * animations run <mult> times faster, so the body catches up with the real arm sooner. Enemies and the
 * player's other moves (walk, roll, items) are untouched.
 *
 * Mechanism (static analysis of out/eboot.elf + a read-only probe of a running game, see the commit):
 *   Every ChrIns has a per-frame time step at ChrIns+0xE0 (f32, ~0.0166 s at 60 fps). The ChrIns update
 *   0x1cbd6a0 (r13 = the ChrIns) computes it as the frame time passed through the character's hit-stop
 *   module ([[chr+0x3B0]+0x90]: +0x1C in, +0x18 out; 0x1e38430 ramps it to 0 for the stop and back) and
 *   stores it at 0x1cbd716, after copying the same value to [rbp-0x7c]. The later per-character stages
 *   (stamina regen 0x1cbdb42, the ChrCtrl / behaviour updates 0x1cbdfd8 0x1cbe0c5 0x1cbf482 0x1cbfb70
 *   0x1cbfb88 0x1cbfee0 0x1ce5f0c 0x1cf5015 0x1cf7fd2 0x1cfc656) read ChrIns+0xE0 as their dt. Hit stop
 *   already freezes the attacker's animation through this very value, so scaling it by k > 1 plays the
 *   character's time k times faster (clip time, root motion, TAE windows keep their relative timing).
 *   The debug menu also names a "Debug Play Speed" float at +0xE0 of a ChrCtrl-side object (0x1c38d7d),
 *   which is why the field is treated as a play speed.
 * The hook ("VR Attack Speed (chr dt)", 0x1cbd711, 14 bytes, replay-after) runs before the two stores:
 * for the player (r13 == [[WorldChrMan]+0x60]) while an attack is the newest TimeAct animation it scales
 * xmm0 (the dt), so both copies (+0xE0 and [rbp-0x7c]) carry the faster step.
 *
 * Attack state: the TimeAct ring's newest animation ([[chr+0x3B0]+0x18] + 0x20, 10 x 16 B, head +0xC0,
 * findings/13 3.4) has a sub-id (anim % 1000000) of 30000-49999 (the weapon moves; the same test as the
 * arms' attack blend). The ring keeps the attack's id for the whole recovery until the next move
 * starts, so "after I hit" is covered. Dodges (a00 7000s), walking, items and gun shots (51000+) are
 * not scaled. Anything else that happens to use a 30000-49999 sub-id counts as an attack.
 *
 * Environment (needs BB_HOOKS="...;VR Attack Speed (chr dt)"):
 *   BB_VR_ATK_SPEED=<mult>  start value (default 0 = off; 1 = off; 0.5-3)
 *   BB_VR_FP_LOG=1          log every value change and, while an attack plays, the measured clip rate
 *                           (ring clip time per wall second: ~1.0 normal, ~mult scaled) twice a second
 *   atk_speed=<mult>        in BB_VR_FP_CONTROL / BB_VR_ARMS_CONTROL (out/fp.txt), polled 4 Hz; 0/1 off
 * Risks: the character's whole dt is scaled in an attack, so stamina regen, poise recovery and SpEffect
 * timers also tick faster during it (the regen is blocked while attacking); hit tests sweep a longer
 * path per frame (fine up to ~2x); co-op peers see the faster animation through the net state but
 * their copy of the moveset times are unchanged. */
#define _GNU_SOURCE
#include "hooks.h"
#include "vr_throw_anim.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define VA_BASE 0x400000u
#define VA_SITE 0x1CBD711u
#define VA_WORLD_CHR_MAN 0x593E878u
#define VA_PLAYER_INS_VTABLE 0x578F310u
#define AS_MULT_MIN 0.5f
#define AS_MULT_MAX 3.0f
#define AS_THROW_MULT_MAX 4.0f /* the throw clip only, not attacks */
#define AS_ATTACK_FROM 30000
#define AS_ATTACK_TO 50000
#define AS_LOG_EVERY_S 0.5

static struct {
    int inited;
    float mult;                 /* 0 / 1 = off */
    int log;
    unsigned char *image;
    const char *control;
    double control_checked;
    long long control_mtime, control_size;
    /* the measured clip rate */
    int m_anim;
    float m_time;
    double m_wall;
    int scaled_frames;
    /* thrown items' animation (BB_VR_THROW_ANIM_SPEED, vr_throw_anim.h) */
    float throw_mult;           /* 0 / 1 = off */
    _Atomic uint32_t throw_req; /* bumped by the pad thread at a throw gesture */
    uint32_t throw_seen;
    VtaState throw_st;
} as;

static double as_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ---- guest memory (the game thread; a region cache like the other VR handlers) ---- */
#ifdef _WIN32
static struct { uintptr_t lo, hi; } as_regions[16];
static int as_region_count;
static double as_regions_time;
static int as_readable(uintptr_t a, size_t n) {
    if (a < 0x10000 || a + n < a) return 0;
    const double now = as_now();
    if (now - as_regions_time > 1.0) { as_region_count = 0; as_regions_time = now; }
    for (int i = 0; i < as_region_count; ++i)
        if (as_regions[i].lo <= a && a + n <= as_regions[i].hi) return 1;
    uintptr_t at = a;
    while (at < a + n) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void *)at, &mbi, sizeof(mbi))) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return 0;
        const uintptr_t lo = (uintptr_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
        if (as_region_count < (int)(sizeof(as_regions) / sizeof(*as_regions))) {
            as_regions[as_region_count].lo = lo;
            as_regions[as_region_count].hi = hi;
            ++as_region_count;
        }
        at = hi;
    }
    return 1;
}
#else
static int as_readable(uintptr_t a, size_t n) { return a >= 0x10000 && a + n >= a; }
#endif
static int as_rd(uintptr_t a, void *out, size_t n) {
    if (!as_readable(a, n)) return 0;
    memcpy(out, (const void *)a, n);
    return 1;
}
static int as_ptr(uintptr_t a, uintptr_t *out) {
    uint64_t v;
    if (!as_rd(a, &v, 8) || v < 0x10000 || (v & 7)) return 0;
    *out = (uintptr_t)v;
    return 1;
}
static uintptr_t as_va(uint32_t v) { return (uintptr_t)(as.image + (v - VA_BASE)); }

/* "atk_speed=<x>" in the control text; returns 1 and the value when the key is there. */
static int as_parse(const char *text, float *out) {
    const char *p = text;
    while ((p = strstr(p, "atk_speed=")) != NULL) {
        if (p == text || p[-1] == ' ' || p[-1] == '\n' || p[-1] == '\r' || p[-1] == '\t') {
            const float x = strtof(p + 10, NULL);
            if (isfinite(x)) { *out = x; return 1; }
        }
        p += 10;
    }
    return 0;
}

static float as_clamp(float x) {
    if (!(x > 0.0f) || x == 1.0f) return 0.0f; /* off */
    return x < AS_MULT_MIN ? AS_MULT_MIN : x > AS_MULT_MAX ? AS_MULT_MAX : x;
}

static void as_set(float m, const char *why) {
    const float v = as_clamp(m);
    if (v == as.mult) return;
    as.mult = v;
    if (as.log)
        fprintf(stderr, "VR atk_speed: %s (%s)\n", v > 0.0f ? "on" : "off", why), fprintf(stderr, "VR atk_speed: multiplier %.2f\n", v > 0.0f ? v : 1.0f);
}

static void as_poll_control(double now) {
    if (!as.control || now - as.control_checked < 0.25) return;
    as.control_checked = now;
    struct stat st;
    if (stat(as.control, &st) != 0) return;
    if ((long long)st.st_mtime == as.control_mtime && (long long)st.st_size == as.control_size) return;
    as.control_mtime = (long long)st.st_mtime;
    as.control_size = (long long)st.st_size;
    FILE *f = fopen(as.control, "rb");
    if (!f) return;
    char text[4096];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    float x;
    if (as_parse(text, &x)) as_set(x, "control file");
    else as_set(0.0f, "key removed from the control file"); /* a missing key = off, like the others' defaults */
}

/* The newest TimeAct animation of the player: id and clip time; 0 when unreadable. */
static int as_ring(uintptr_t player, int *anim, float *time) {
    uintptr_t bag, tam;
    if (!as_ptr(player + 0x3B0, &bag) || !as_ptr(bag + 0x18, &tam)) return 0;
    uint8_t buf[0xA4];
    if (!as_rd(tam + 0x20, buf, sizeof(buf))) return 0;
    uint32_t head;
    memcpy(&head, buf + 0xA0, 4);
    if (head >= 10) return 0;
    const uint8_t *e = buf + 16 * ((head + 9) % 10);
    memcpy(anim, e, 4);
    memcpy(time, e + 4, 4);
    return 1;
}

static ABI void as_hook(BbHookContext *ctx) {
    if (!as.image) as.image = (unsigned char *)(uintptr_t)(ctx->rip - (VA_SITE - VA_BASE));
    const uintptr_t chr = (uintptr_t)ctx->r13;
    uintptr_t wcm, player;
    uint64_t vt;
    if (!as_ptr(as_va(VA_WORLD_CHR_MAN), &wcm) || !as_ptr(wcm + 0x60, &player) || player != chr || !as_rd(player, &vt, 8) ||
        vt != (uint64_t)as_va(VA_PLAYER_INS_VTABLE))
        return; /* every character passes here; only the player goes on */
    const double now = as_now();
    as_poll_control(now);
    const uint32_t req = atomic_load_explicit(&as.throw_req, memory_order_relaxed);
    if (req != as.throw_seen) {
        as.throw_seen = req;
        if (as.throw_mult > 0.0f) vta_begin(&as.throw_st, now);
    }
    if (as.mult <= 0.0f && as.throw_st.state == 0) return;
    int anim;
    float clip;
    if (!as_ring(player, &anim, &clip)) return;
    if (as.throw_st.state != 0) {
        float *tdt = (float *)&ctx->ymm[0][0];
        if (*tdt > 0.0f && *tdt <= 0.2f) {
            const int was = as.throw_st.state;
            const float k = vta_step(&as.throw_st, now, anim, clip, as.throw_mult);
            if (k != 1.0f) *tdt *= k;
            if (as.log && (was == 1 && as.throw_st.state == 2))
                fprintf(stderr, "VR throw anim: clip %d latched, x%.2f\n", anim, as.throw_mult);
            if (as.log && was == 2 && as.throw_st.state == 0)
                fprintf(stderr, "VR throw anim: clip %d over (clip time %.3f s)\n", as.throw_st.anim, clip);
            if (k != 1.0f) return;
        }
        if (as.throw_st.state != 0) return;
    }
    if (as.mult <= 0.0f) return;
    const int sub = anim >= 0 ? anim % 1000000 : -1;
    const int attack = sub >= AS_ATTACK_FROM && sub < AS_ATTACK_TO;
    float *dt = (float *)&ctx->ymm[0][0];
    if (!attack) {
        as.m_anim = -1;
        return;
    }
    if (!(*dt > 0.0f) || *dt > 0.2f) return; /* a stop (0) or a hitch: leave it */
    const float before = *dt;
    *dt = before * as.mult;
    ++as.scaled_frames;
    if (as.log) {
        /* the clip's own progress per wall second over a 0.5 s window of one animation */
        if (as.m_anim != anim || clip < as.m_time) {
            as.m_anim = anim;
            as.m_time = clip;
            as.m_wall = now;
        } else if (now - as.m_wall >= AS_LOG_EVERY_S) {
            fprintf(stderr, "VR atk_speed: anim %d x%.2f dt %.4f->%.4f clip %.3f s, clip rate %.2f /s (%d frames scaled)\n", anim, as.mult,
                    before, *dt, clip, (clip - as.m_time) / (now - as.m_wall), as.scaled_frames);
            as.m_time = clip;
            as.m_wall = now;
        }
    }
}

/* The pad thread: a thrown item's use was just sent (findings/39): its animation is sped up. */
void vr_atkspeed_throw_begin(void) { atomic_fetch_add_explicit(&as.throw_req, 1, memory_order_relaxed); }

void vr_atkspeed_register(void) {
    as.throw_mult = 3.6f;
    const char *te = getenv("BB_VR_THROW_ANIM_SPEED");
    if (te && *te) {
        const float x = strtof(te, NULL);
        as.throw_mult = isfinite(x) ? x : 1.0f;
    }
    as.throw_mult = as.throw_mult > AS_THROW_MULT_MAX ? AS_THROW_MULT_MAX : as.throw_mult;
    if (!(as.throw_mult > 1.0f)) as.throw_mult = 0.0f; /* 1 (or less) = off */
    const char *e = getenv("BB_VR_FP_LOG");
    as.log = e && e[0] == '1';
    e = getenv("BB_VR_ATK_SPEED");
    as.mult = 0.0f;
    if (e && *e) as_set(strtof(e, NULL), "BB_VR_ATK_SPEED");
    as.control = getenv("BB_VR_ARMS_CONTROL");
    if (!as.control || !*as.control) as.control = getenv("BB_VR_FP_CONTROL");
    as.m_anim = -1;
    bb_hook_set_atkspeed_impl(as_hook);
    if (as.log)
        fprintf(stderr, "VR atk_speed: ready (multiplier %.2f, key atk_speed= in %s); needs the hook \"VR Attack Speed (chr dt)\"\n",
                as.mult > 0.0f ? as.mult : 1.0f, as.control ? as.control : "no control file");
    fprintf(stderr, "VR throw anim: thrown items' animation x%.2f (BB_VR_THROW_ANIM_SPEED, 1 = off); needs the hook \"VR Attack Speed (chr dt)\"\n",
            as.throw_mult > 0.0f ? as.throw_mult : 1.0f);
}
