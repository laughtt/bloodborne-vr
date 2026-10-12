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
 * Gun shots (BB_VR_GUN_SPEED, default 2.0, src/vr_gun_speed.h, findings/27 section 8): while the newest
 * clip is a gun shot (sub-id 51000-52999) the same dt is scaled by the gun multiplier, with the same
 * stop / hitch guards, so the wind-up and the recovery around the bullet event are shorter. The bullet is
 * still spawned by the game's own TAE event (once) and redirected by the gun aim hook. On while online too.
 * Hit reactions (BB_VR_HURT_SPEED, OFF by default (1.0) until the anim range is confirmed, src/vr_hurt_speed.h, findings/27 section 9): while the newest
 * clip is a damage reaction (sub-id 9000-12999, UNCONFIRMED, see the header and BB_VR_HURT_RANGE) dt is scaled the
 * same way, so staggers last half as long. dt 0 (hit stop) and hitches are untouched; the victim side of grabs
 * and visceral hits (100000+) is excluded. On while online too. With BB_VR_FP_LOG=1 clip ids outside every known
 * class are logged ("new clip") so the real reaction ids can be read off after getting hit.
 * Attack state: the TimeAct ring's newest animation ([[chr+0x3B0]+0x18] + 0x20, 10 x 16 B, head +0xC0,
 * findings/13 3.4) has a sub-id (anim % 1000000) of 30000-49999 (the weapon moves; the same test as the
 * arms' attack blend). The ring keeps the attack's id for the whole recovery until the next move
 * starts, so "after I hit" is covered. Dodges (a00 7000s), walking, items and gun shots (51000+) are
 * not scaled by the attack speed (the gun multiplier below does that). Anything else that happens to use a 30000-49999 sub-id counts as an attack.
 *
 * The same hook also scales the evade distance (BB_VR_EVADE_SCALE, default 1.5, src/vr_evade_scale.h,
 * findings/19 section 10): the player's rolls and steps get a share of each frame's step added to the feet.
 *
 * Environment (needs BB_HOOKS="...;VR Attack Speed (chr dt)"):
 *   BB_VR_BACKSTEP_SCALE=<k> the backstep clips' own distance multiplier (default 2.0; 1 = off); backstep_scale=<k>
 *                           in the control file; its stop guard waits for the clip's root motion (src/vr_evade_scale.h)
 *   BB_VR_EVADE_SCALE=<k>   evade distance multiplier (default 1.5; 1 = off); evade_scale=<k> in the
 *                           control file (out/fp.txt, a missing key = the env value / 1.5); BB_VR_EVADE_LOG=1 logs;
 *                           off while online (BB_ONLINE=1 or BB_ONLINE_ROLE), BB_VR_EVADE_ONLINE=1 overrides
 *   BB_VR_GUN_SPEED=<k>     gun-shot clip speed multiplier (default 2.0; 1 = off; max 4); gun_speed=<k> in the
 *                           control file (a missing key = the env value / 2.0); logs the value at start and on
 *                           every change, and with BB_VR_FP_LOG=1 the measured clip rate of a shot;
 *                           stays on while online (co-op); BB_VR_GUN_SPEED_ONLINE=0 turns it off online
 *   BB_VR_HURT_SPEED=<k>    hit-reaction clip speed multiplier (default 1 = off until the anim range is confirmed; max 4); hurt_speed=<k> in the
 *                           control file; BB_VR_HURT_RANGE=<from>-<to> overrides the sub-id range (default 9000-12999);
 *                           stays on while online (co-op); BB_VR_HURT_SPEED_ONLINE=0 turns it off online
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
#include "vr_mem_cache.h"
#include "vr_evade_scale.h"
#include "vr_throw_anim.h"
#include "vr_gun_speed.h"
#include "vr_hurt_speed.h"
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
    /* evade distance (BB_VR_EVADE_SCALE / evade_scale=, vr_evade_scale.h) */
    float evade_k;              /* clamped; 0 = off */
    float backstep_k, backstep_start; /* the backstep clips' own scale (BB_VR_BACKSTEP_SCALE / backstep_scale=) */
    float evade_start;          /* the env / default value: what a missing control-file key falls back to */
    int evade_online;           /* online play: the scale stays off */
    int evade_log;
    VrEvade evade;
    int evade_logged_clip;
    /* gun-shot clips (BB_VR_GUN_SPEED / gun_speed=, vr_gun_speed.h) */
    float gun_k;                /* clamped; 0 = off */
    float gun_start;            /* the env / default value: what a missing control-file key falls back to */
    int gun_online;             /* online play: the speed stays off */
    int g_anim;                 /* the measured clip rate of the shot (log) */
    float g_time;
    double g_wall;
    int gun_frames;
    /* hit-reaction clips (BB_VR_HURT_SPEED / hurt_speed=, vr_hurt_speed.h) */
    float hurt_k;               /* clamped; 0 = off */
    float hurt_start;
    int hurt_online;
    VrHurtRange hurt_range;
    int h_anim;                 /* the measured clip rate of the reaction (log) */
    float h_time;
    double h_wall;
    int hurt_frames;
    int h_seen;                 /* last unclassified clip logged */
} as;

static double as_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ---- guest memory (the game thread; a region cache like the other VR handlers) ---- */
#ifdef _WIN32
static _Thread_local VmCache as_cache;
static int as_readable(uintptr_t a, size_t n) { return vm_readable(&as_cache, a, n); }
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

static void as_evade_set(float k, const char *why) {
    const float v = as.evade_online ? 0.0f : vr_evade_clamp(k);
    if (v == as.evade_k) return;
    as.evade_k = v;
    {
        fprintf(stderr, "VR evade: scale %.2f (%s)%s\n", v > 0.0f ? v : 1.0f, why, as.evade_online ? ", online: forced off" : "");
    }
}

static void as_backstep_set(float k, const char *why) {
    const float v = as.evade_online ? 0.0f : vr_evade_clamp(k);
    if (v == as.backstep_k) return;
    as.backstep_k = v;
    fprintf(stderr, "VR evade: backstep scale %.2f (%s)%s\n", v > 0.0f ? v : 1.0f, why, as.evade_online ? ", online: forced off" : "");
}

static void as_gun_set(float k, const char *why) {
    const float v = as.gun_online ? 0.0f : vr_gun_clamp(k);
    if (v == as.gun_k) return;
    as.gun_k = v;
    fprintf(stderr, "VR gun speed: x%.2f (%s)%s\n", v > 0.0f ? v : 1.0f, why, as.gun_online ? ", online: forced off" : "");
}

static void as_hurt_set(float k, const char *why) {
    const float v = as.hurt_online ? 0.0f : vr_hurt_clamp(k);
    if (v == as.hurt_k) return;
    as.hurt_k = v;
    fprintf(stderr, "VR hurt speed: x%.2f (%s)%s\n", v > 0.0f ? v : 1.0f, why, as.hurt_online ? ", online: forced off" : "");
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
    if (vr_evade_parse(text, &x)) as_evade_set(x, "control file");
    else as_evade_set(as.evade_start, "no evade_scale= in the control file: the env / default value");
    if (vr_evade_parse_backstep(text, &x)) as_backstep_set(x, "control file");
    else as_backstep_set(as.backstep_start, "no backstep_scale= in the control file: the env / default value");
    if (vr_gun_parse(text, &x)) as_gun_set(x, "control file");
    else as_gun_set(as.gun_start, "no gun_speed= in the control file: the env / default value");
    if (vr_hurt_parse(text, &x)) as_hurt_set(x, "control file");
    else as_hurt_set(as.hurt_start, "no hurt_speed= in the control file: the env / default value");
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

/* The player's feet [[P + 0x3B0] + 0x68] + 0x1E0 (findings/16): where the root motion lands. */
static int as_feet(uintptr_t player, uintptr_t *at) {
    uintptr_t bag, phys;
    if (!as_ptr(player + 0x3B0, &bag) || !as_ptr(bag + 0x68, &phys) || !as_readable(phys + 0x1E0, 16)) return 0;
    *at = phys + 0x1E0;
    return 1;
}

/* Evade distance: while a roll, quickstep or backstep plays (the newest ring clip), a share
 * (k - 1) of every frame's real horizontal step is added to the feet. Called once per player update
 * before the dt is used: the feet then hold the previous frame's motion. */
static void as_evade(uintptr_t player, float dt) {
    if (as.evade_k <= 0.0f && as.backstep_k <= 0.0f) {
        if (as.evade.active) vr_evade_reset(&as.evade);
        return;
    }
    uintptr_t bag, tam, at;
    if (!as_ptr(player + 0x3B0, &bag) || !as_ptr(bag + 0x18, &tam) || !as_feet(player, &at)) return;
    uint8_t buf[0xA4];
    if (!as_rd(tam + 0x20, buf, sizeof(buf))) return;
    uint32_t head;
    memcpy(&head, buf + 0xA0, 4);
    if (head >= 10) return;
    const uint8_t *e = buf + 16 * ((head + 9) % 10);
    int32_t anim;
    float f[3], p[4];
    memcpy(&anim, e, 4);
    memcpy(f, e + 4, 12);
    memcpy(p, (const void *)at, sizeof(p));
    if (!isfinite(p[0]) || !isfinite(p[2]) || fabsf(p[0]) > 1e5f || fabsf(p[2]) > 1e5f) return;
    float dx, dz;
    const int was = as.evade.active;
    const float given = as.evade.given;
    if (vr_evade_step(&as.evade, anim, f, p[0], p[2], dt, as.evade_k, as.backstep_k, &dx, &dz)) {
        p[0] += dx;
        p[2] += dz;
        memcpy((void *)at, p, 8);                 /* x */
        memcpy((char *)(void *)at + 8, p + 2, 4); /* z; y (up) and w stay */
    }
    if (as.evade_log && ((!was && as.evade.active) || (was && !as.evade.active && given > 0.0f)))
        fprintf(stderr, "VR evade: %s a%03d_%06d, extra so far %.2f m (x%.2f)\n", as.evade.active ? "start" : "end", anim / 1000000,
                anim % 1000000, as.evade.active ? 0.0f : given,
                vr_evade_move(anim) && vr_evade_move(anim)->kind == VR_DASH_MOVE_BACKSTEP ? as.backstep_k : as.evade_k);
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
    as_evade(player, *(const float *)&ctx->ymm[0][0]);
    const uint32_t req = atomic_load_explicit(&as.throw_req, memory_order_relaxed);
    if (req != as.throw_seen) {
        as.throw_seen = req;
        if (as.throw_mult > 0.0f) vta_begin(&as.throw_st, now);
    }
    if (as.mult <= 0.0f && as.gun_k <= 0.0f && as.hurt_k <= 0.0f && as.throw_st.state == 0) return;
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
    if (as.mult <= 0.0f && as.gun_k <= 0.0f && as.hurt_k <= 0.0f) return;
    if (as.gun_k > 0.0f && vr_gun_is_shot(anim)) {
        /* a gun shot (the newest clip, recovery included): same guards as the attacks (a stop or a hitch is left
         * alone). The bullet event is a window the clip time crosses, so it fires once at the faster rate. */
        float *gdt = (float *)&ctx->ymm[0][0];
        const float before = *gdt;
        const float k = vr_gun_factor(anim, before, as.gun_k);
        if (k == 1.0f) return;
        *gdt = before * k;
        ++as.gun_frames;
        if (as.log) {
            if (as.g_anim != anim || clip < as.g_time) {
                as.g_anim = anim;
                as.g_time = clip;
                as.g_wall = now;
            } else if (now - as.g_wall >= AS_LOG_EVERY_S) {
                fprintf(stderr, "VR gun speed: anim %d x%.2f dt %.4f->%.4f clip %.3f s, clip rate %.2f /s (%d frames scaled)\n", anim, k,
                        before, *gdt, clip, (clip - as.g_time) / (now - as.g_wall), as.gun_frames);
                as.g_time = clip;
                as.g_wall = now;
            }
        }
        return;
    }
    as.g_anim = -1;
    if (as.hurt_k > 0.0f && vr_hurt_is_reaction(anim, as.hurt_range)) {
        /* a damage reaction (flinch, stagger, knockback, knockdown, get-up): same guards as the attacks. The hit
         * stop that follows a hit ramps dt to 0 and is left alone, so the freeze keeps its length. */
        float *hdt = (float *)&ctx->ymm[0][0];
        const float before = *hdt;
        const float k = vr_hurt_factor(anim, before, as.hurt_k, as.hurt_range);
        if (k == 1.0f) return;
        *hdt = before * k;
        ++as.hurt_frames;
        if (as.log) {
            if (as.h_anim != anim || clip < as.h_time) {
                as.h_anim = anim;
                as.h_time = clip;
                as.h_wall = now;
            } else if (now - as.h_wall >= AS_LOG_EVERY_S) {
                fprintf(stderr, "VR hurt speed: anim %d x%.2f dt %.4f->%.4f clip %.3f s, clip rate %.2f /s (%d frames scaled)\n", anim, k,
                        before, *hdt, clip, (clip - as.h_time) / (now - as.h_wall), as.hurt_frames);
                as.h_time = clip;
                as.h_wall = now;
            }
        }
        return;
    }
    as.h_anim = -1;
    if (as.log && as.h_seen != anim && vr_hurt_is_unclassified(anim, as.hurt_range)) {
        /* discovery: a clip id nobody classified (the hit reactions' real ids show up here after getting hit) */
        as.h_seen = anim;
        fprintf(stderr, "VR hurt speed: new clip %d (sub-id %d) is not an attack, a shot or in the reaction range %d-%d, clip time %.3f s\n", anim,
                anim % 1000000, as.hurt_range.from, as.hurt_range.to - 1, clip);
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
    {
        float kb = VR_BACKSTEP_SCALE_DEFAULT;
        const char *be = getenv("BB_VR_BACKSTEP_SCALE");
        if (be && *be) kb = strtof(be, NULL);
        as.backstep_start = kb;
        as_backstep_set(kb, be && *be ? "BB_VR_BACKSTEP_SCALE" : "default");
    }
    as.control = getenv("BB_VR_ARMS_CONTROL");
    if (!as.control || !*as.control) as.control = getenv("BB_VR_FP_CONTROL");
    as.evade_log = (e = getenv("BB_VR_EVADE_LOG")) && e[0] == '1';
    as.evade_online = vr_evade_online(getenv("BB_ONLINE"), getenv("BB_ONLINE_ROLE")) && !((e = getenv("BB_VR_EVADE_ONLINE")) && e[0] == '1');
    {
        float k = VR_EVADE_SCALE_DEFAULT;
        e = getenv("BB_VR_EVADE_SCALE");
        if (e && *e) k = strtof(e, NULL);
        as.evade_start = k;
        as_evade_set(k, e && *e ? "BB_VR_EVADE_SCALE" : "default");
        fprintf(stderr, "VR evade: distance x%.2f (BB_VR_EVADE_SCALE, evade_scale= in %s, 1 = off)%s; needs the hook \"VR Attack Speed (chr dt)\"\n",
                as.evade_k > 0.0f ? as.evade_k : 1.0f, as.control ? as.control : "no control file", as.evade_online ? "; online play: off" : "");
    }
    as.m_anim = -1;
    as.g_anim = -1;
    as.h_anim = -1;
    as.h_seen = -1;
    as.hurt_range = vr_hurt_default_range();
    {
        const char *re = getenv("BB_VR_HURT_RANGE");
        if (re && *re && !vr_hurt_parse_range(re, &as.hurt_range))
            fprintf(stderr, "VR hurt speed: BB_VR_HURT_RANGE=%s is not <from>-<to>; using %d-%d\n", re, as.hurt_range.from, as.hurt_range.to - 1);
    }
    as.hurt_online = vr_hurt_online_off(getenv("BB_ONLINE"), getenv("BB_ONLINE_ROLE"), getenv("BB_VR_HURT_SPEED_ONLINE"));
    {
        float k = VR_HURT_SPEED_DEFAULT;
        e = getenv("BB_VR_HURT_SPEED");
        if (e && *e) k = strtof(e, NULL);
        as.hurt_start = k;
        as_hurt_set(k, e && *e ? "BB_VR_HURT_SPEED" : "default");
        fprintf(stderr, "VR hurt speed: hit-reaction animation x%.2f for sub-ids %d-%d (BB_VR_HURT_SPEED, hurt_speed= in %s, BB_VR_HURT_RANGE, 1 = off)%s; needs the hook \"VR Attack Speed (chr dt)\"\n",
                as.hurt_k > 0.0f ? as.hurt_k : 1.0f, as.hurt_range.from, as.hurt_range.to - 1, as.control ? as.control : "no control file",
                as.hurt_online ? "; online play: off (BB_VR_HURT_SPEED_ONLINE=0)" : "");
    }
    as.gun_online = vr_gun_online_off(getenv("BB_ONLINE"), getenv("BB_ONLINE_ROLE"), getenv("BB_VR_GUN_SPEED_ONLINE"));
    const int online_now = vr_gun_online(getenv("BB_ONLINE"), getenv("BB_ONLINE_ROLE"));
    {
        float k = VR_GUN_SPEED_DEFAULT;
        e = getenv("BB_VR_GUN_SPEED");
        if (e && *e) k = strtof(e, NULL);
        as.gun_start = k;
        as_gun_set(k, e && *e ? "BB_VR_GUN_SPEED" : "default");
        fprintf(stderr, "VR gun speed: shot animation x%.2f (BB_VR_GUN_SPEED, gun_speed= in %s, 1 = off)%s; needs the hook \"VR Attack Speed (chr dt)\"\n",
                as.gun_k > 0.0f ? as.gun_k : 1.0f, as.control ? as.control : "no control file", as.gun_online ? "; online play: off (BB_VR_GUN_SPEED_ONLINE=0)" : "");
    }
    if (online_now)
        fprintf(stderr, "VR online: gun_speed x%.2f and hurt_speed x%.2f are %s while online (opt out with BB_VR_GUN_SPEED_ONLINE=0 / BB_VR_HURT_SPEED_ONLINE=0)%s\n",
                as.gun_k > 0.0f ? as.gun_k : 1.0f, as.hurt_k > 0.0f ? as.hurt_k : 1.0f,
                as.gun_online || as.hurt_online ? "partly turned off by the opt-out" : "active", "");
    bb_hook_set_atkspeed_impl(as_hook);
    if (as.log)
        fprintf(stderr, "VR atk_speed: ready (multiplier %.2f, key atk_speed= in %s); needs the hook \"VR Attack Speed (chr dt)\"\n",
                as.mult > 0.0f ? as.mult : 1.0f, as.control ? as.control : "no control file");
    fprintf(stderr, "VR throw anim: thrown items' animation x%.2f (BB_VR_THROW_ANIM_SPEED, 1 = off); needs the hook \"VR Attack Speed (chr dt)\"\n",
            as.throw_mult > 0.0f ? as.throw_mult : 1.0f);
}
