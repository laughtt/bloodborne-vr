/* VR hit scale in the game's real hit test (BB_VR_HIT_SCALE=1, docs/vr/findings/32-vr-hitbox-scale.md 4):
 * the player's weapon capsules get radius x hit_scale and +hit_reach along the blade, the same maths as
 * the sandbox and the overlay (src/vr_hit_scale.h).
 *
 * Where (static analysis of out/eboot.elf, findings/32 4):
 *   attack creation 0x1c63970 -> per hit 0..3: 0x23142d0 copies AtkParam Radius (+0x00..0x0C), DmyPoly1
 *   (+0x2C..) and DmyPoly2 (+0x34..) into the attack info (+0x4C, +0x134, +0x138) -> 0x1c63f00 ->
 *   0x1c5de90 makes the 0x300-byte attack entry (the info at entry+0x54: radius +0xA0, dummies +0x188 /
 *   +0x18C, owner handle +0x50, AtkParam id +0x94).
 *   Every frame the attack manager 0x1c63540 first updates every entry (0x1c5ed50), then tests every
 *   entry (0x1c5fbe0: an hkpWorld linear cast 0xf21340 of the entry's shape from the last position to
 *   the new one; contacts counted in entry+0x2E0). The update REBUILDS the capsule each frame: both dummy
 *   world matrices (0x19a7610 / the model's vf+0x58), p1 = DmyPoly1, p2 = DmyPoly2, an hkpCapsuleShape
 *   in place at [entry+0x30] (ctor 0xf86da0) with radius [entry+0xA0] stored at shape+0x14 and the local
 *   ends A = [rbp-0x210] = 0 and B = [rbp-0x220] = (0,0,|p2-p1|); the transform (+0x1E0..+0x210) has its
 *   origin at p1 and +Z toward p2.
 * The hook ("VR Hitbox Scale (attack shape)", 0x1c5fa55, 18 bytes, replay-after) runs after the radius
 * was stored and before the ctor reads it and the ends: rbx = the shape, r12 = the entry, rbp the update's
 * frame ([rbp-0x280] = p1, the DmyPoly2 matrices at [rbp-0x1d0 + 0x30 i], i = [entry+0x258]). For the
 * player's entries (entry+0x50 == [PlayerIns+8]) it scales shape+0x14 and moves A or B (the end
 * farther from the grip) outward. Since the capsule is rebuilt from [entry+0xA0] each frame, nothing
 * compounds and hit=0 is the game again on the next frame. Spheres (one dummy) are not rebuilt per frame
 * and are left alone; the game's debug draw (0x1c61a30) still reads [entry+0xA0] (unscaled).
 * The grip: the L_Weapon / R_Weapon bone nearer to the capsule (model pose -> world through the physics
 * position +0x1E0 and yaw +0x1D4); a capsule farther than 0.5 m from both (a kick) gets the radius only;
 * no pose -> radius only.
 *
 * Environment: BB_VR_HIT_SCALE=1 (off: the handler returns at once); the keys hit, hit_scale, hit_reach,
 * hit_scale_s/m/l, hit_reach_s/m/l of BB_VR_FP_CONTROL / BB_VR_ARMS_CONTROL (out/fp.txt), polled 4 Hz.
 * Log: "VR hit scale: attack <id> r a->b reach +c ..." per new attack (at most 10 lines a second) and,
 * when the attack ends, "VR hit scale: attack <id> done: ..." with the cast contacts the game's test
 * counted (entry+0x2E0) and the character hits the stamina hit hooks reported. */
#define _GNU_SOURCE
#include "hooks.h"
#include "vr_mem_cache.h"
#include "vr_fp_math.h"
#include "vr_hit_scale.h"
#include "vr_hitbox.h"
#include <math.h>
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
#define VA_SITE_SHAPE 0x1C5FA55u /* in the attack update 0x1c5ed50, after the radius store, before the ctor */
#define VA_WORLD_CHR_MAN 0x593E878u
#define VA_PLAYER_INS_VTABLE 0x578F310u
#define HS_GRIP_MAX_M 0.5f
#define HS_ATTACK_GAP_S 0.5
#define HS_LOG_PER_S 10
#define HS_MAX_BONES 256

static struct {
    int on;
    unsigned char *image;
    VhsParams p;
    const char *control;
    double control_checked;
    long long control_mtime, control_size;
    /* the skeleton's weapon bones */
    uintptr_t skel;
    int weapon[2];
    /* the current attack */
    int atk, atk_alt, active;
    double last_s;
    float r0, r1, reach_applied, weapon_reach;
    int cls, side, grip_ok, scaled_shapes;
    unsigned contact_frames, contacts_max, hits[3];
    VhsReachTrack track;
    /* log rate */
    long long log_second;
    int log_lines;
    unsigned mismatch;
} hs;

static double hs_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ---- guest memory (the game thread only) ---- */
#ifdef _WIN32
static _Thread_local VmCache hs_cache;
static int hs_readable(uintptr_t a, size_t n) { return vm_readable(&hs_cache, a, n); }
#else
static int hs_readable(uintptr_t a, size_t n) { return a >= 0x10000 && a + n >= a; }
#endif
static int hs_rd(uintptr_t a, void *out, size_t n) {
    if (!hs_readable(a, n)) return 0;
    memcpy(out, (const void *)a, n);
    return 1;
}
static int hs_ptr(uintptr_t a, uintptr_t *out) {
    uint64_t v;
    if (!hs_rd(a, &v, 8) || v < 0x10000 || (v & 7)) return 0;
    *out = (uintptr_t)v;
    return 1;
}
static uintptr_t hs_va(uint32_t v) { return (uintptr_t)(hs.image + (v - VA_BASE)); }

static void hs_poll_control(double now) {
    if (!hs.control || now - hs.control_checked < 0.25) return;
    hs.control_checked = now;
    struct stat st;
    if (stat(hs.control, &st) != 0) return;
    if ((long long)st.st_mtime == hs.control_mtime && (long long)st.st_size == hs.control_size) return;
    hs.control_mtime = (long long)st.st_mtime;
    hs.control_size = (long long)st.st_size;
    FILE *f = fopen(hs.control, "rb");
    if (!f) return;
    char text[4096];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    vhs_apply_text(&hs.p, text);
    fprintf(stderr, "VR hit scale: game hit test %s: radius x%.2f reach %+.3f m (short %.2f/%+.3f, medium %.2f/%+.3f, long %.2f/%+.3f; <0 = global)\n",
            hs.p.enabled ? "on" : "off (hit=0)", hs.p.radius_scale, hs.p.reach_m, hs.p.class_radius_scale[0], hs.p.class_reach_m[0],
            hs.p.class_radius_scale[1], hs.p.class_reach_m[1], hs.p.class_radius_scale[2], hs.p.class_reach_m[2]);
}

static int hs_log_ok(double now) {
    const long long s = (long long)now;
    if (s != hs.log_second) { hs.log_second = s; hs.log_lines = 0; }
    return hs.log_lines++ < HS_LOG_PER_S;
}

/* The player (PlayerIns) or 0. */
static uintptr_t hs_player(void) {
    uintptr_t wcm, player;
    uint64_t vt;
    if (!hs_ptr(hs_va(VA_WORLD_CHR_MAN), &wcm) || !hs_ptr(wcm + 0x60, &player) || !hs_rd(player, &vt, 8) ||
        vt != (uint64_t)hs_va(VA_PLAYER_INS_VTABLE))
        return 0;
    return player;
}

static int hs_bone_is(uintptr_t names, int i, const char *want) {
    uint64_t s;
    if (!hs_rd(names + (uintptr_t)i * 16, &s, 8)) return 0;
    s &= ~(uint64_t)1; /* hkStringPtr's owned flag */
    char buf[16];
    if (!s || !hs_rd((uintptr_t)s, buf, 9)) return 0;
    return !memcmp(buf, want, 9);
}

/* The grip: the weapon bone (world) nearer to the segment p1..p2; *side 0 left, 1 right. */
static int hs_grip(uintptr_t player, VrVec3 p1, VrVec3 p2, VrVec3 *grip, int *side, float *dist) {
    uintptr_t bag, phys, beh, holder, skel, model;
    if (!hs_ptr(player + 0x3B0, &bag) || !hs_ptr(bag + 0x68, &phys) || !hs_ptr(bag + 0x30, &beh) || !hs_ptr(beh + 0x28, &holder))
        return 0;
    float pos[4], rot[4];
    if (!hs_rd(phys + 0x1E0, pos, 16) || !hs_rd(phys + 0x1D0, rot, 16) || !isfinite(pos[0]) || !isfinite(pos[1]) ||
        !isfinite(pos[2]) || !isfinite(rot[1]))
        return 0;
    const uintptr_t pose = holder + 0x38;
    int32_t n;
    if (!hs_ptr(pose, &skel) || !hs_ptr(pose + 0x18, &model) || !hs_rd(pose + 0x20, &n, 4) || n <= 0 || n > HS_MAX_BONES) return 0;
    if (skel != hs.skel) {
        hs.skel = skel;
        hs.weapon[0] = hs.weapon[1] = -1;
        uintptr_t names;
        int32_t nn;
        if (hs_ptr(skel + 0x28, &names) && hs_rd(skel + 0x30, &nn, 4) && nn > 0 && nn <= HS_MAX_BONES)
            for (int i = 0; i < nn; ++i) {
                if (hs_bone_is(names, i, "L_Weapon")) hs.weapon[0] = i;
                if (hs_bone_is(names, i, "R_Weapon")) hs.weapon[1] = i;
            }
        fprintf(stderr, "VR hit scale: player skeleton 0x%llx: L_Weapon %d, R_Weapon %d\n", (unsigned long long)skel, hs.weapon[0],
                hs.weapon[1]);
    }
    const VrVec3 at = vr_v3(pos[0], pos[1], pos[2]);
    int found = 0;
    for (int s = 0; s < 2; ++s) {
        const int b = hs.weapon[s];
        float t[4];
        if (b < 0 || b >= n || !hs_rd(model + (uintptr_t)b * 48, t, 16) || !isfinite(t[0]) || !isfinite(t[1]) || !isfinite(t[2])) continue;
        const VrVec3 w = vr_model_to_world(at, rot[1], vr_v3(t[0], t[1], t[2]));
        const float d = vhs_seg_dist(w, p1, p2);
        if (!found || d < *dist) {
            found = 1;
            *dist = d;
            *grip = w;
            *side = s;
        }
    }
    return found;
}

static const char *hs_class_name(int c) { return c == VHS_LONG ? "long" : c == VHS_MEDIUM ? "medium" : "short"; }

/* The current attack ended (a new one started, or none for HS_ATTACK_GAP_S): one line on what it hit. */
static void hs_flush(void) {
    if (!hs.active) return;
    hs.active = 0;
    fprintf(stderr, "VR hit scale: attack %d done: %s (cast contacts in %u frame%s, max %u; character hits %u, wall %u, object %u; %d shape%s scaled)\n",
            hs.atk, hs.contact_frames || hs.hits[0] || hs.hits[1] || hs.hits[2] ? "HIT" : "miss", hs.contact_frames,
            hs.contact_frames == 1 ? "" : "s", hs.contacts_max, hs.hits[0], hs.hits[1], hs.hits[2], hs.scaled_shapes,
            hs.scaled_shapes == 1 ? "" : "s");
}

void vr_hitscale_hit(int contact, int32_t atk_id) {
    if (!hs.on || !hs.active || (atk_id != hs.atk && atk_id != hs.atk_alt) || contact < 0 || contact > 2) return;
    ++hs.hits[contact];
}

static ABI void hs_hook(BbHookContext *ctx) {
    if (!hs.on) return;
    if (!hs.image) hs.image = (unsigned char *)(uintptr_t)(ctx->rip - (VA_SITE_SHAPE - VA_BASE));
    const double now = hs_now();
    hs_poll_control(now);
    if (hs.active && now - hs.last_s > HS_ATTACK_GAP_S) hs_flush();
    const uintptr_t entry = (uintptr_t)ctx->r12, shape = (uintptr_t)ctx->rbx, fp = (uintptr_t)ctx->rbp;
    uint32_t owner = 0, handle = 0;
    const uintptr_t player = hs_player();
    if (!player || !hs_rd(player + 8, &handle, 4) || !hs_rd(entry + 0x50, &owner, 4) || owner != handle) return;
    int32_t atk = -1, alt = -1, idx = -1;
    uint32_t contacts = 0;
    hs_rd(entry + 0x94, &atk, 4);
    hs_rd(entry + 0x3C, &alt, 4);
    hs_rd(entry + 0x2E0, &contacts, 4); /* the game's test of the last frame */
    if (!hs.active || atk != hs.atk) {
        hs_flush();
        hs.active = 1;
        hs.atk = atk;
        hs.atk_alt = alt;
        hs.contact_frames = hs.contacts_max = 0;
        hs.hits[0] = hs.hits[1] = hs.hits[2] = 0;
        hs.scaled_shapes = 0;
        hs.r0 = -1.0f;
    }
    hs.last_s = now;
    if (contacts) {
        ++hs.contact_frames;
        if (contacts > hs.contacts_max) hs.contacts_max = contacts;
    }
    if (!hs.p.enabled) return;
    float r, *a = (float *)(fp - 0x210), *b = (float *)(fp - 0x220), p1v[4], p2v[3];
    if (!hs_rd(shape + 0x14, &r, 4) || !isfinite(r) || r <= 0.0f || r > 10.0f || !hs_rd(fp - 0x280, p1v, 16) ||
        !hs_rd(entry + 0x258, &idx, 4) || idx < 0 || idx >= 8)
        return;
    const uintptr_t m2 = fp - 0x1d0 + (uintptr_t)idx * 0x30;
    if (!hs_rd(m2 + 0x0c, &p2v[0], 4) || !hs_rd(m2 + 0x1c, &p2v[1], 4) || !hs_rd(m2 + 0x2c, &p2v[2], 4)) return;
    const VrVec3 p1 = vr_v3(p1v[0], p1v[1], p1v[2]), p2 = vr_v3(p2v[0], p2v[1], p2v[2]);
    const float len = sqrtf(vr_dot(vr_sub(p2, p1), vr_sub(p2, p1)));
    /* the layout check: A = 0, B = (0,0,|p2-p1|) as the update builds them; otherwise radius only */
    int layout_ok = isfinite(len) && a[0] == 0.0f && a[1] == 0.0f && a[2] == 0.0f && b[0] == 0.0f && b[1] == 0.0f &&
                    fabsf(b[2] - (len > 1e-5f ? len : 1e-5f)) <= 1e-3f + 1e-3f * len;
    if (!layout_ok && hs.mismatch++ < 5)
        fprintf(stderr, "VR hit scale: attack %d: capsule layout unexpected (A %.3f %.3f %.3f, B %.3f %.3f %.3f, |p2-p1| %.3f): radius only\n", atk,
                a[0], a[1], a[2], b[0], b[1], b[2], len);
    VrVec3 grip = p1;
    int side = -1;
    float gd = 0.0f;
    const int grip_ok = layout_ok && hs_grip(player, p1, p2, &grip, &side, &gd);
    const int reach_ok = grip_ok && gd <= HS_GRIP_MAX_M;
    float weapon_reach = 0.0f;
    if (grip_ok) weapon_reach = vhs_track_reach(&hs.track, atk, now, HS_ATTACK_GAP_S, vhs_far_end(p1, p2, grip));
    const VhsAxisShape s =
        vhs_apply_axis(&hs.p, p1, p2, layout_ok ? a[2] : 0.0f, layout_ok ? b[2] : 0.0f, r, grip, weapon_reach, reach_ok);
    /* the capsule's growth in length (the reach actually given) */
    const float applied = layout_ok && s.axis_ok ? (s.b_z - b[2]) + (a[2] - s.a_z) : 0.0f;
    *(float *)(shape + 0x14) = s.r;
    if (layout_ok && s.axis_ok) {
        a[2] = s.a_z;
        b[2] = s.b_z;
    }
    ++hs.scaled_shapes;
    if (hs.r0 < 0.0f) {
        hs.r0 = r;
        hs.r1 = s.r;
        hs.reach_applied = applied;
        hs.weapon_reach = weapon_reach;
        hs.cls = vhs_class_from_reach(weapon_reach);
        hs.side = side;
        hs.grip_ok = grip_ok;
        if (hs_log_ok(now))
            fprintf(stderr, "VR hit scale: attack %d r %.3f->%.3f reach +%.3f (len %.3f, %s hand, grip %.2f m, weapon reach %.2f m %s%s; +0x3c %d)\n",
                    atk, r, s.r, applied, len, side == 1 ? "right" : side == 0 ? "left" : "no", gd, weapon_reach,
                    hs_class_name(hs.cls), !layout_ok ? ", layout: radius only" : !grip_ok ? ", no pose: radius only" : !reach_ok ? ", body shape: radius only" : "",
                    alt);
    }
}

void vr_hitscale_register(void) {
    const char *v = getenv("BB_VR_HIT_SCALE");
    hs.on = v && v[0] == '1';
    hs.p = vhs_defaults();
    hs.weapon[0] = hs.weapon[1] = -1;
    hs.control = getenv("BB_VR_ARMS_CONTROL");
    if (!hs.control || !*hs.control) hs.control = getenv("BB_VR_FP_CONTROL");
    bb_hook_set_hitscale_impl(hs_hook);
    if (hs.on)
        fprintf(stderr, "VR hit scale: on for the game's hit test (radius x%.2f reach %+.3f m until %s says otherwise); needs the hook "
                        "\"VR Hitbox Scale (attack shape)\"\n",
                hs.p.radius_scale, hs.p.reach_m, hs.control ? hs.control : "a control file");
}
