/* Pure logic for the VR visceral (src/runtime_vr_visceral.c, docs/vr/findings/41-parry.md section 7):
 * after a parry-candidate gun shot, a time window in which a hand thrust (or just standing close)
 * presses R1, which in Bloodborne is the visceral attack when the enemy is in the riposte state. The
 * runtime cannot read the enemy's state, so the window is the fallback for "the enemy is staggered".
 * No I/O, no globals: tests/test_vr_visceral.c checks it. The shape follows the sandbox's
 * ParryTrainer::VisceralStep (gpu/xr_bridge/sandbox3d/parry.cpp).
 *
 * Distances are horizontal (x, z; the game's Y is up) because the lock-on point's height is not
 * known to be the chest. Times are milliseconds on any steady clock (double).
 *
 * Life of a window: vv_on_shot opens it (target = the shot's lock-on target); the stagger takes a
 * moment, so nothing fires before delay_ms; with confirm HIT a player character hit within hit_ms of
 * the shot must also have been seen (vv_on_hit); after window_ms the window closes (expired).
 * vv_track follows the enemy while it moves. vv_step fires (returns 1) at most once per window:
 *   auto    the head or the right hand is within reach_m of the target;
 *   thrust  the right hand's relative speed along its aim passes the gate (speed, alignment to the
 *           target, rising, debounce, armed, reach);
 *   swing   never (the swing gesture sends R1 from the bridge itself).
 * A fire holds R1 for press_ms (vv_pressed). */
#ifndef VR_VISCERAL_MATH_H
#define VR_VISCERAL_MATH_H
#include "vr_arm_math.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { VV_MODE_AUTO = 0, VV_MODE_THRUST = 1, VV_MODE_SWING = 2 };
enum { VV_CONFIRM_AIM = 0, VV_CONFIRM_HIT = 1 };
/* why a thrust frame is rejected (bits) */
enum { VV_REJ_SPEED = 1, VV_REJ_ALIGN = 2, VV_REJ_FALLING = 4, VV_REJ_DEBOUNCE = 8, VV_REJ_DISARMED = 16, VV_REJ_REACH = 32 };
/* why a window closed */
enum { VV_CLOSE_NONE = 0, VV_CLOSE_EXPIRED = 1, VV_CLOSE_FIRED = 2, VV_CLOSE_REPLACED = 3, VV_CLOSE_NO_TARGET = 4 };
/* vv_on_shot results */
enum { VV_SHOT_IGNORED = 0, VV_SHOT_OPENED = 1, VV_SHOT_REPLACED = 2, VV_SHOT_NO_TARGET = 3 };

typedef struct {
    int mode, confirm;
    float speed_mps, align, rearm, reach_m;
    float debounce_ms, window_ms, delay_ms, hit_ms, track_m, press_ms;
} VvConfig;

static inline VvConfig vv_defaults(void) {
    VvConfig c;
    c.mode = VV_MODE_AUTO;
    c.confirm = VV_CONFIRM_AIM;
    c.speed_mps = 1.5f;
    c.align = 0.7f;
    c.rearm = 0.5f;
    c.reach_m = 2.0f;
    c.debounce_ms = 300.0f;
    c.window_ms = 2500.0f;
    c.delay_ms = 400.0f;
    c.hit_ms = 600.0f;
    c.track_m = 1.5f;
    c.press_ms = 100.0f;
    return c;
}

typedef struct {
    int open, confirmed, have_target;
    double opened_ms;
    VrVec3 target;
    /* the thrust gate */
    int armed;
    double last_ms;
    float prev_t;
    double press_until_ms;
    /* one report per thrust attempt while live */
    int attempt, attempt_fired;
    unsigned best_bits;
    float best_t, best_a, best_d, peak;
    /* events for the logs (taken once) */
    int close_reason;
    int report_ready;
    unsigned report_bits;
    float report_t, report_a, report_d, report_peak;
    /* what fired last */
    int fired_how; /* VV_MODE_*, -1 none */
    float fired_dist, fired_t, fired_a;
    int fired_by_hand; /* auto: 1 when the hand was the nearer */
} VvState;

static inline void vv_init(VvState *s) {
    memset(s, 0, sizeof(*s));
    s->armed = 1;
    s->last_ms = -1e12;
    s->press_until_ms = -1e12;
    s->fired_how = -1;
}

static inline float vv_horiz(VrVec3 a, VrVec3 b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return sqrtf(dx * dx + dz * dz);
}
static inline int vv_popcount(unsigned v) {
    int n = 0;
    for (; v; v >>= 1) n += (int)(v & 1u);
    return n;
}

static inline void vv_close(VvState *s, int reason) {
    if (!s->open) return;
    s->open = 0;
    s->close_reason = reason;
}

/* A parry-candidate shot. have_target: the shot had a lock-on target (locked or aimed at a
 * candidate). A shot without one leaves an open window alone (a stray shot does not undo a parry). */
static inline int vv_on_shot(VvState *s, const VvConfig *c, double now_ms, int have_target, VrVec3 target) {
    if (c->mode == VV_MODE_SWING) return VV_SHOT_IGNORED;
    if (!have_target) {
        return VV_SHOT_NO_TARGET;
    }
    const int replaced = s->open;
    if (replaced) vv_close(s, VV_CLOSE_REPLACED);
    s->open = 1;
    s->confirmed = 0;
    s->have_target = 1;
    s->opened_ms = now_ms;
    s->target = target;
    s->attempt = 0;
    return replaced ? VV_SHOT_REPLACED : VV_SHOT_OPENED;
}

/* A player hit on a character at `point`. Within hit_ms of the shot it confirms the window; a point
 * near the target (track_m, horizontal) becomes the target. Returns 1 when it newly confirmed. */
static inline int vv_on_hit(VvState *s, const VvConfig *c, double now_ms, VrVec3 point) {
    if (!s->open || now_ms - s->opened_ms > (double)c->hit_ms) return 0;
    if (s->have_target && vv_horiz(point, s->target) <= c->track_m) s->target = point;
    if (s->confirmed) return 0;
    s->confirmed = 1;
    return 1;
}

/* The enemies move and fall when staggered: the candidate nearest the target (within track_m)
 * becomes the target. Returns 1 when the target moved. */
static inline int vv_track(VvState *s, const VvConfig *c, const VrVec3 *cand, int n) {
    if (!s->open || !s->have_target) return 0;
    int best = -1;
    float bd = c->track_m;
    for (int i = 0; i < n; ++i) {
        const float d = vv_horiz(cand[i], s->target);
        if (d <= bd) { bd = d; best = i; }
    }
    if (best < 0) return 0;
    s->target = cand[best];
    return 1;
}

static inline int vv_live(const VvState *s, const VvConfig *c, double now_ms) {
    if (!s->open) return 0;
    if (c->confirm == VV_CONFIRM_HIT && !s->confirmed) return 0;
    const double a = now_ms - s->opened_ms;
    return a >= (double)c->delay_ms && a <= (double)c->window_ms;
}
static inline int vv_pressed(const VvState *s, double now_ms) { return now_ms < s->press_until_ms; }

typedef struct {
    double now_ms;
    int head_valid, hand_valid, have_rel;
    VrVec3 head, hand;
    VrVec3 rel_vel;  /* right hand velocity minus head velocity, world frame, m/s */
    VrVec3 hand_fwd; /* right aim -Z, world */
    int blocked;     /* snap turn / recenter / view jump */
} VvInput;

static inline void vv_fire(VvState *s, const VvConfig *c, const VvInput *in, int how, float dist, float t, float a, int by_hand) {
    s->press_until_ms = in->now_ms + (double)c->press_ms;
    s->fired_how = how;
    s->fired_dist = dist;
    s->fired_t = t;
    s->fired_a = a;
    s->fired_by_hand = by_hand;
    vv_close(s, VV_CLOSE_FIRED);
}

/* One frame. Returns 1 on the frame an R1 press starts (then vv_pressed holds it for press_ms). */
static inline int vv_step(VvState *s, const VvConfig *c, const VvInput *in) {
    if (s->open && in->now_ms - s->opened_ms > (double)c->window_ms) vv_close(s, VV_CLOSE_EXPIRED);
    if (c->mode == VV_MODE_SWING) return 0;
    const int live = vv_live(s, c, in->now_ms);
    if (c->mode == VV_MODE_AUTO) {
        if (!live || !s->have_target) return 0;
        const float dh = in->hand_valid ? vv_horiz(in->hand, s->target) : 1e9f;
        const float dhead = in->head_valid ? vv_horiz(in->head, s->target) : 1e9f;
        const float d = dh < dhead ? dh : dhead;
        if (d > c->reach_m) return 0;
        vv_fire(s, c, in, VV_MODE_AUTO, d, 0.0f, 0.0f, dh <= dhead);
        return 1;
    }
    /* thrust */
    if (!in->hand_valid || !in->head_valid || !in->have_rel) {
        s->prev_t = 0.0f;
        s->attempt = 0;
        return 0;
    }
    const VrVec3 fwd = vam_norm(in->hand_fwd, vr_v3(0, 0, 1));
    const float t = vr_dot(in->rel_vel, fwd);
    const float sp = vam_len(in->rel_vel);
    const VrVec3 to_t = s->have_target ? vam_norm(vr_sub(s->target, in->hand), fwd) : fwd;
    const float a = sp > 1e-3f ? vr_dot(in->rel_vel, to_t) / sp : 0.0f;
    const float dist = s->have_target ? vv_horiz(in->hand, s->target) : 1e9f;
    const float before = s->prev_t;
    s->prev_t = t;
    unsigned bits = 0;
    if (t < c->speed_mps) bits |= VV_REJ_SPEED;
    if (a < c->align) bits |= VV_REJ_ALIGN;
    if (!(t > before)) bits |= VV_REJ_FALLING;
    if (in->now_ms - s->last_ms < (double)c->debounce_ms) bits |= VV_REJ_DEBOUNCE;
    if (!s->armed || in->blocked) bits |= VV_REJ_DISARMED;
    if (dist > c->reach_m) bits |= VV_REJ_REACH;
    int fired = 0;
    if (in->blocked) {
        s->armed = 0;
    } else if (bits == 0 && live) {
        fired = 1;
        s->armed = 0;
        s->last_ms = in->now_ms;
    } else if (t < c->rearm) {
        s->armed = 1;
    }
    /* one report per thrust, only while live */
    if (live && !s->attempt && t >= 0.5f * c->speed_mps) {
        s->attempt = 1;
        s->attempt_fired = 0;
        s->best_bits = 0xFFu;
        s->peak = 0.0f;
        s->best_t = s->best_a = 0.0f;
        s->best_d = 1e9f;
    }
    if (s->attempt) {
        if (t > s->peak) s->peak = t;
        const int pb = vv_popcount(bits), pbest = vv_popcount(s->best_bits);
        if (pb < pbest || (pb == pbest && t > s->best_t)) {
            s->best_bits = bits;
            s->best_t = t;
            s->best_a = a;
            s->best_d = dist;
        }
    }
    if (fired) {
        s->attempt_fired = 1;
        vv_fire(s, c, in, VV_MODE_THRUST, dist, t, a, 1);
    }
    if (s->attempt && (t < c->rearm || !live)) {
        if (!s->attempt_fired && live) {
            s->report_ready = 1;
            s->report_bits = s->best_bits;
            s->report_t = s->best_t;
            s->report_a = s->best_a;
            s->report_d = s->best_d;
            s->report_peak = s->peak;
        }
        s->attempt = 0;
    }
    return fired;
}

static inline int vv_take_close(VvState *s) {
    const int r = s->close_reason;
    s->close_reason = VV_CLOSE_NONE;
    return r;
}
static inline int vv_take_report(VvState *s) {
    const int r = s->report_ready;
    s->report_ready = 0;
    return r;
}

static inline const char *vv_mode_name(int mode) {
    return mode == VV_MODE_THRUST ? "thrust" : mode == VV_MODE_SWING ? "swing" : "auto";
}
static inline const char *vv_close_name(int reason) {
    switch (reason) {
    case VV_CLOSE_EXPIRED: return "expired";
    case VV_CLOSE_FIRED: return "fired";
    case VV_CLOSE_REPLACED: return "replaced by a new shot";
    case VV_CLOSE_NO_TARGET: return "next shot had no target";
    default: return "none";
    }
}
/* "speed,align" etc. into buf; "none" for 0. */
static inline const char *vv_reject_names(unsigned bits, char *buf, size_t n) {
    static const struct { unsigned bit; const char *name; } names[] = {
        {VV_REJ_SPEED, "speed"},       {VV_REJ_ALIGN, "align"},       {VV_REJ_FALLING, "falling"},
        {VV_REJ_DEBOUNCE, "debounce"}, {VV_REJ_DISARMED, "disarmed"}, {VV_REJ_REACH, "reach"}};
    size_t len = 0;
    if (!n) return buf;
    buf[0] = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!(bits & names[i].bit)) continue;
        const int w = snprintf(buf + len, n - len, "%s%s", len ? "," : "", names[i].name);
        if (w > 0) len += (size_t)w < n - len ? (size_t)w : n - len - 1;
    }
    if (!len) snprintf(buf, n, "none");
    return buf;
}

#endif
