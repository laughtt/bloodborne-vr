/* SPDX-FileCopyrightText: Copyright 2026 bbport contributors
 * SPDX-License-Identifier: MIT
 *
 * The comfort dash (docs/vr/findings/19-dash-locomotion.md). Pure logic: no I/O, no globals, no
 * OpenXR; C11 and C++, so the sandbox (gpu/xr_bridge/sandbox3d/locomotion.cpp) and the game
 * (src/runtime_vr_dash.c) run the same code. Tests: gpu/xr_bridge/vr_dash_test.cpp (C++) and
 * tests/test_vr_dash.c (C).
 *
 * In first person a dodge (roll or quickstep), a backstep or a jump moves the camera with the
 * character's animation: the head tumbles through the roll, bobs, arcs. The filter takes the
 * per-frame anchor (the eye, or the character's root plus an eye height; world meters, +Y up,
 * either handedness) and, while a move is active, replaces its motion with one straight slide from
 * where the camera was to where the move ends, along an ease-out curve, at the start height (a
 * jump: one smooth bump). When the move ends, whatever still separates the camera from the anchor
 * blends out with position and velocity continuous, so the camera lands exactly on the anchor: no
 * drift. A warp of the anchor (a lamp, a respawn, a cut) snaps. Only the position is filtered: the
 * caller composes the headset pose (rotation, 6DoF offset) on top, so the head stays the user's.
 *
 * Use:
 *     VrDash dash; vr_dash_reset(&dash);
 *     VrDashParams params; vr_dash_params_default(&params);
 *     every frame: camera = vr_dash_update(&dash, &params, anchor, move_id, &hint_or_null, seconds);
 * move_id: 0 = no move; any other value = a move in progress. A new value starts a new dash (pass
 * 1 for a plain "active" flag, a counter for back-to-back moves). The hint (read only when a dash
 * starts) gives what is known about the move: its direction, distance, duration, a jump's height.
 * Without a direction the dash takes it from the anchor's first few centimeters of travel; without
 * a distance it follows the anchor's own progress (smoothed). Time is any monotonic clock in
 * seconds; the update rate may differ from the anchor's (72 Hz camera, 30 fps game: the dash is a
 * function of time, so it moves smoothly between the anchor's samples).
 */
#ifndef VR_DASH_H
#define VR_DASH_H
#include <math.h>
#include <string.h>

typedef struct { float x, y, z; } VrDashV3;

enum { VR_DASH_IDLE = 0, VR_DASH_MOVE = 1, VR_DASH_SETTLE = 2 };  /* VrDash.mode */
enum { VR_DASH_DODGE = 0, VR_DASH_JUMP = 1 };                      /* VrDashHint.kind */
enum { VR_DASH_EASE_POWER = 0, VR_DASH_EASE_EXPO = 1, VR_DASH_EASE_SMOOTH = 2 };

typedef struct {
    float duration;         /* s: the slide from the start to the end (default 0.3) */
    int curve;              /* VR_DASH_EASE_POWER: e(u) = 1 - (1 - u)^power (ease-out; 1 linear,
                               2 quadratic, 3 cubic); _EXPO: (1 - 2^(-power u)) / (1 - 2^-power);
                               _SMOOTH: smoothstep in-out (for comparison only) */
    float power;            /* the curve's exponent (default 3) */
    float settle;           /* s: the remaining offset to the anchor blends out over this (0.2) */
    float settle_speed;     /* m/s: a larger offset (a cancelled or blocked move: the camera has
                               to come back) takes longer, offset / settle_speed (0: always
                               `settle`; default 4) */
    float jump_height;      /* m: a jump's single bump, 0 = a flat dash (default 0.3) */
    float jump_duration;    /* s: the bump's length; 0 = the dash's duration (default 0.5) */
    float teleport;         /* m: the anchor moving more than this in one update snaps (2) */
    float max_lead;         /* m: the camera never runs further than this ahead of the anchor's
                               own progress along the dash (blocked moves, a wrong distance);
                               0 = no limit (default; a 0.4 s quadratic slide leads Bloodborne's
                               roll by up to 1.6 m when nothing blocks it; unused in FOLLOW) */
    float max_drop;         /* m: the anchor more than this below the start height is a fall: the
                               dash ends and the camera follows (0 = never; default 2.0: more
                               than an eye can dip, Bloodborne's rolls take it to the floor) */
    float min_axis;         /* m: anchor travel that sets the direction when no hint gives one */
    int follow_height;      /* 1: follow the anchor's height (a root anchor on slopes); 0: hold the
                               start height (an eye anchor that dips through a roll; default) */
    float vignette_speed;   /* m/s: the camera speed of a full vignette (default 4) */
    float vignette_release; /* s: the vignette's fade-out time constant (default 0.15) */
    int follow;             /* 1: FOLLOW mode (the game): the camera never runs ahead of the anchor.
                               It trails the anchor's actual position through a critically damped
                               spring (no hint, table or distance is used), so it stops where the
                               anchor stops (walls) and its height is the anchor's (hills, stairs)
                               plus a jump's bump. 0: the predicted slide above (default; the
                               sandbox, where nothing blocks the character) */
    float follow_omega;     /* rad/s: the spring's frequency in FOLLOW mode (default 14: the camera
                               lags the character by about 2/omega = 0.14 s of travel) */
} VrDashParams;

typedef struct {
    int kind;               /* VR_DASH_DODGE or VR_DASH_JUMP */
    VrDashV3 direction;     /* the move's horizontal direction (any length); zero: from the anchor */
    float distance;         /* m, horizontal, trusted (the settle fixes a wrong one at the end);
                               <= 0: follow the anchor's own progress */
    float duration;         /* s; <= 0: the params' */
    float height;           /* m, a jump's bump; < 0: the params' */
} VrDashHint;

typedef struct {
    int mode, kind, move, has_last, axis_known, axis_hinted;
    double t_last, t0, t_begin, ts0;
    VrDashV3 last_anchor, out, vel;  /* the last output and the camera's own (analytic) velocity */
    VrDashV3 start, anchor0, axis;   /* the dash: from `start`, the anchor at its start, unit axis */
    VrDashV3 net_s, net_v;           /* no direction hint: the anchor's travel, smoothed (axis) */
    float distance_plan;             /* m: the hint's distance (0: none) */
    float distance, distance_v;      /* m, m/s: the distance the path uses, smoothed */
    float progress_max;              /* m: the anchor's furthest progress along the axis */
    float progress_s, progress_v;    /* smoothed */
    float progress_guard;            /* the lead guard's: smoothed, lag-compensated, never back */
    float duration, height, bump;    /* s, m, s */
    VrDashV3 off0, voff0;            /* the settle: offset to the anchor and its rate at its start */
    float settle_time;
    float u, speed, vignette;        /* progress 0..1, camera speed, vignette 0..1 (outputs) */
    int snapped;                     /* the last update snapped to the anchor (a warp) */
} VrDash;

static inline VrDashV3 vr_dash_v3(float x, float y, float z) { VrDashV3 v = {x, y, z}; return v; }
static inline VrDashV3 vr_dash_add(VrDashV3 a, VrDashV3 b) { return vr_dash_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline VrDashV3 vr_dash_sub(VrDashV3 a, VrDashV3 b) { return vr_dash_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline VrDashV3 vr_dash_scale(VrDashV3 a, float s) { return vr_dash_v3(a.x * s, a.y * s, a.z * s); }
static inline float vr_dash_len(VrDashV3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static inline float vr_dash_len_h(VrDashV3 a) { return sqrtf(a.x * a.x + a.z * a.z); }
static inline float vr_dash_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline void vr_dash_params_default(VrDashParams *p) {
    memset(p, 0, sizeof(*p));
    p->duration = 0.3f;
    p->curve = VR_DASH_EASE_POWER;
    p->power = 3.0f;
    p->settle = 0.2f;
    p->settle_speed = 4.0f;
    p->jump_height = 0.3f;
    p->jump_duration = 0.5f;
    p->teleport = 2.0f;
    p->max_lead = 0.0f;
    p->max_drop = 2.0f;
    p->min_axis = 0.05f;
    p->follow_height = 0;
    p->vignette_speed = 4.0f;
    p->vignette_release = 0.15f;
    p->follow = 0;
    p->follow_omega = 14.0f;
}

/* The ease curve e(u), 0..1 -> 0..1 (e(0) = 0, e(1) = 1, never past 1: no overshoot). */
static inline float vr_dash_ease(int curve, float power, float u) {
    u = vr_dash_clamp(u, 0.0f, 1.0f);
    if (curve == VR_DASH_EASE_SMOOTH) return u * u * (3.0f - 2.0f * u);
    if (curve == VR_DASH_EASE_EXPO) {
        const float k = power > 0.1f ? power : 0.1f;
        return (1.0f - exp2f(-k * u)) / (1.0f - exp2f(-k));
    }
    const float p = vr_dash_clamp(power, 1.0f, 12.0f);
    return 1.0f - powf(1.0f - u, p);
}
/* de/du. */
static inline float vr_dash_ease_rate(int curve, float power, float u) {
    u = vr_dash_clamp(u, 0.0f, 1.0f);
    if (curve == VR_DASH_EASE_SMOOTH) return 6.0f * u * (1.0f - u);
    if (curve == VR_DASH_EASE_EXPO) {
        const float k = power > 0.1f ? power : 0.1f;
        return k * 0.69314718f * exp2f(-k * u) / (1.0f - exp2f(-k));
    }
    const float p = vr_dash_clamp(power, 1.0f, 12.0f);
    return u >= 1.0f ? (p <= 1.0f ? 1.0f : 0.0f) : p * powf(1.0f - u, p - 1.0f);
}
/* A jump's height bump: sin^2(pi u), 0 -> 1 -> 0 with zero slope at both ends. */
static inline float vr_dash_bump(float u) {
    if (!(u > 0.0f) || u >= 1.0f) return 0.0f;
    const float s = sinf(3.14159265f * u);
    return s * s;
}
static inline float vr_dash_bump_rate(float u) {
    if (!(u > 0.0f) || u >= 1.0f) return 0.0f;
    return 3.14159265f * sinf(2.0f * 3.14159265f * u);
}

/* Forgets everything: the next update passes the anchor through. */
static inline void vr_dash_reset(VrDash *d) { memset(d, 0, sizeof(*d)); }

/* Moves every stored position by `delta` (a world shift, or the pivot of a turn applied to both
 * the anchor and the camera), so a dash in progress continues seamlessly. */
static inline void vr_dash_translate(VrDash *d, VrDashV3 delta) {
    d->last_anchor = vr_dash_add(d->last_anchor, delta);
    d->out = vr_dash_add(d->out, delta);
    d->start = vr_dash_add(d->start, delta);
    d->anchor0 = vr_dash_add(d->anchor0, delta);
}
/* Rotates the stored state about +Y around `pivot` by `c` = cos, `s` = sin of the angle, as
 * (x, z) -> (pivot.x + c dx + s dz, pivot.z - s dx + c dz), the same map the caller applies to
 * its world (a snap turn that moves the anchor and the camera together). */
static inline VrDashV3 vr_dash_rotate_y(VrDashV3 v, VrDashV3 pivot, float c, float s, int is_point) {
    const float dx = is_point ? v.x - pivot.x : v.x, dz = is_point ? v.z - pivot.z : v.z;
    return vr_dash_v3((is_point ? pivot.x : 0.0f) + c * dx + s * dz, v.y,
                      (is_point ? pivot.z : 0.0f) - s * dx + c * dz);
}
static inline void vr_dash_rotate(VrDash *d, VrDashV3 pivot, float c, float s) {
    d->last_anchor = vr_dash_rotate_y(d->last_anchor, pivot, c, s, 1);
    d->out = vr_dash_rotate_y(d->out, pivot, c, s, 1);
    d->start = vr_dash_rotate_y(d->start, pivot, c, s, 1);
    d->anchor0 = vr_dash_rotate_y(d->anchor0, pivot, c, s, 1);
    d->axis = vr_dash_rotate_y(d->axis, pivot, c, s, 0);
    d->net_s = vr_dash_rotate_y(d->net_s, pivot, c, s, 0);
    d->net_v = vr_dash_rotate_y(d->net_v, pivot, c, s, 0);
    d->vel = vr_dash_rotate_y(d->vel, pivot, c, s, 0);
    d->off0 = vr_dash_rotate_y(d->off0, pivot, c, s, 0);
    d->voff0 = vr_dash_rotate_y(d->voff0, pivot, c, s, 0);
}

/* The settle's offset (out - anchor) at time tau after it began: a cubic Hermite from (off0,
 * voff0) to (0, 0) over `length` seconds. *rate gets its derivative. */
static inline VrDashV3 vr_dash_settle_offset(const VrDash *d, float tau, VrDashV3 *rate) {
    const float T = d->settle_time > 1e-4f ? d->settle_time : 1e-4f;
    const float s = vr_dash_clamp(tau / T, 0.0f, 1.0f);
    const float h00 = (2.0f * s - 3.0f) * s * s + 1.0f, h10 = ((s - 2.0f) * s + 1.0f) * s;
    const float d00 = (6.0f * s - 6.0f) * s / T, d10 = (3.0f * s - 4.0f) * s + 1.0f;
    if (rate) *rate = vr_dash_add(vr_dash_scale(d->off0, d00), vr_dash_scale(d->voff0, d10));
    return vr_dash_add(vr_dash_scale(d->off0, h00), vr_dash_scale(d->voff0, h10 * T));
}

/* The settle starts with the camera's own velocity (a cancelled dash keeps moving and brakes):
 * not the anchor's, which stops or turns as the move ends and would push the camera past it. */
static inline void vr_dash_begin_settle(VrDash *d, const VrDashParams *p, VrDashV3 out, VrDashV3 vel,
                                        VrDashV3 anchor, double t) {
    d->off0 = vr_dash_sub(out, anchor);
    d->voff0 = vel;
    d->settle_time = p->settle > 0.0f ? p->settle : 0.0f;
    if (d->settle_time > 0.0f && p->settle_speed > 0.0f && vr_dash_len(d->off0) / p->settle_speed > d->settle_time)
        d->settle_time = vr_dash_len(d->off0) / p->settle_speed;
    d->ts0 = t;
    d->mode = d->settle_time > 0.0f && (vr_dash_len(d->off0) > 1e-5f || vr_dash_len(d->voff0) > 1e-4f)
                  ? VR_DASH_SETTLE : VR_DASH_IDLE;
}

/* A critically damped spring: x (rate *v) follows `target` with natural frequency `omega` over
 * dt. The exact solution: stable for any dt, and toward a target that only steps one way (the
 * anchor's samples) it moves monotonically, without overshoot. */
static inline void vr_dash_spring(float *x, float *v, float target, float omega, float dt) {
    if (!(dt > 0.0f)) return;
    const float r = *x - target, e = expf(-omega * dt), k = (*v + omega * r) * dt;
    *x = target + (r + k) * e;
    *v = (*v - omega * k) * e;
}
/* What is derived from the anchor's samples is smoothed at this frequency, so it moves between
 * the samples of a game slower than the headset (30 fps under 72 Hz) without their steps. */
#define VR_DASH_SMOOTH_OMEGA 31.4159f /* 5 Hz */

/* The dash's position at time t (mode VR_DASH_MOVE); also updates the axis, the distance, the
 * camera velocity and ends the move on a fall. */
static inline VrDashV3 vr_dash_eval_move(VrDash *d, const VrDashParams *p, VrDashV3 anchor, double t, float dt) {
    if (p->follow) {
        /* FOLLOW: trail the anchor's real position; never ahead, stops with it, height is its. */
        VrDashV3 out = d->out, vel = d->vel;
        const float T = d->duration > 0.02f ? d->duration : 0.02f;
        if (dt > 0.0f) {
            const float om = p->follow_omega > 1.0f ? p->follow_omega : 1.0f;
            vr_dash_spring(&out.x, &vel.x, anchor.x, om, dt);
            vr_dash_spring(&out.z, &vel.z, anchor.z, om, dt);
        } else {
            out = d->start;
            vel = vr_dash_v3(0.0f, 0.0f, 0.0f);
        }
        const float tb = (float)(t - d->t_begin);
        const float B = d->bump > 0.02f ? d->bump : 0.02f;
        out.y = anchor.y + d->height * vr_dash_bump(tb / B);
        vel.y = d->height * vr_dash_bump_rate(tb / B) / B;
        d->u = vr_dash_clamp((float)(t - d->t0) / T, 0.0f, 1.0f);
        d->vel = vel;
        return out;
    }
    VrDashV3 rel = vr_dash_sub(anchor, d->anchor0);
    rel.y = 0.0f;
    if (!d->axis_hinted) {
        /* No direction given: the anchor's (smoothed) travel sets it and keeps refining it. */
        vr_dash_spring(&d->net_s.x, &d->net_v.x, rel.x, VR_DASH_SMOOTH_OMEGA, dt);
        vr_dash_spring(&d->net_s.z, &d->net_v.z, rel.z, VR_DASH_SMOOTH_OMEGA, dt);
        const float n = vr_dash_len_h(d->net_s);
        if (n >= (p->min_axis > 0.0f ? p->min_axis : 0.05f)) {
            if (!d->axis_known) {
                /* The slide starts again from where the camera is (so far it only eased toward
                 * the anchor's start), with the time that is left (at least 60%). */
                const float T0 = d->duration > 0.02f ? d->duration : 0.02f;
                const float spent = (float)(t - d->t0);
                const float e0 = vr_dash_ease(p->curve, p->power, spent / T0);
                d->start.x += (d->anchor0.x - d->start.x) * e0;
                d->start.z += (d->anchor0.z - d->start.z) * e0;
                d->duration = T0 - spent > 0.6f * T0 ? T0 - spent : 0.6f * T0;
                d->t0 = t;
                d->axis_known = 1;
            }
            d->axis = vr_dash_v3(d->net_s.x / n, 0.0f, d->net_s.z / n);
        }
    }
    const float progress = d->axis_known ? rel.x * d->axis.x + rel.z * d->axis.z : 0.0f;
    if (progress > d->progress_max) d->progress_max = progress;
    vr_dash_spring(&d->progress_s, &d->progress_v, d->progress_max, VR_DASH_SMOOTH_OMEGA, dt);
    /* The smoothing lags a moving anchor by 2 v / omega (0.5 m in an 8 m/s roll); adding that
     * back gives its true progress, overshooting by at most v / (e omega) (9 cm) when it stops. */
    const float predicted = d->progress_s + (d->progress_v > 0.0f ? d->progress_v : 0.0f) * (2.0f / VR_DASH_SMOOTH_OMEGA);
    if (predicted > d->progress_guard) d->progress_guard = predicted;
    /* A given distance is trusted (an eye anchor surges past the end and comes back: following
     * it would overshoot); without one the dash follows the anchor's furthest progress. A wrong
     * distance is corrected by the settle when the move ends. */
    const float target = d->distance_plan > 0.0f ? d->distance_plan : d->progress_max;
    vr_dash_spring(&d->distance, &d->distance_v, target, VR_DASH_SMOOTH_OMEGA, dt);
    const float T = d->duration > 0.02f ? d->duration : 0.02f;
    const float u = vr_dash_clamp((float)(t - d->t0) / T, 0.0f, 1.0f);
    const float e = vr_dash_ease(p->curve, p->power, u), de = vr_dash_ease_rate(p->curve, p->power, u) / T;
    const VrDashV3 end = vr_dash_add(d->anchor0, vr_dash_scale(d->axis, d->distance));
    VrDashV3 path = vr_dash_sub(end, d->start);
    path.y = 0.0f;
    VrDashV3 out = vr_dash_add(d->start, vr_dash_scale(path, e));
    VrDashV3 vel = vr_dash_add(vr_dash_scale(path, u < 1.0f ? de : 0.0f), vr_dash_scale(d->axis, d->distance_v * e));
    if (p->max_lead > 0.0f && d->axis_known) {
        const float cam = (out.x - d->anchor0.x) * d->axis.x + (out.z - d->anchor0.z) * d->axis.z;
        const float limit = d->progress_guard + p->max_lead;
        if (cam > limit) {
            out = vr_dash_sub(out, vr_dash_scale(d->axis, cam - limit));
            vel = vr_dash_v3(0.0f, 0.0f, 0.0f);
        }
    }
    const float tb = (float)(t - d->t_begin);
    const float B = d->bump > 0.02f ? d->bump : 0.02f;
    out.y = d->start.y + (p->follow_height ? anchor.y - d->anchor0.y : 0.0f) + d->height * vr_dash_bump(tb / B);
    vel.y = d->height * vr_dash_bump_rate(tb / B) / B;
    d->u = u;
    d->vel = vel;
    if (!p->follow_height && p->max_drop > 0.0f && anchor.y < d->start.y - p->max_drop) {
        /* A fall: the dash ends here and the camera follows (the move id stays consumed). */
        vr_dash_begin_settle(d, p, out, vel, anchor, t);
        d->u = 1.0f;
    }
    return out;
}

/* Re-aims the dash in progress (mode VR_DASH_MOVE) once its direction is known better (the
 * character finished turning into the move a frame or two after it started): the slide goes on
 * from where the camera is (call it after the update of this frame) toward the anchor's start +
 * direction * distance (distance <= 0: keep the planned one), over the time that is left (at
 * least 60% of the dash). */
static inline void vr_dash_retarget(VrDash *d, VrDashV3 direction, float distance, double t) {
    const float n = vr_dash_len_h(direction);
    if (d->mode != VR_DASH_MOVE || !(n > 1e-6f)) return;
    const float T0 = d->duration > 0.02f ? d->duration : 0.02f;
    const float spent = (float)(t - d->t0);
    d->start.x = d->out.x;
    d->start.z = d->out.z;
    d->duration = T0 - spent > 0.6f * T0 ? T0 - spent : 0.6f * T0;
    d->t0 = t;
    d->axis = vr_dash_v3(direction.x / n, 0.0f, direction.z / n);
    d->axis_known = d->axis_hinted = 1;
    if (distance > 0.0f) {
        d->distance_plan = d->distance = distance;
        d->distance_v = 0.0f;
    }
    const float p = (d->last_anchor.x - d->anchor0.x) * d->axis.x + (d->last_anchor.z - d->anchor0.z) * d->axis.z;
    d->progress_max = d->progress_s = d->progress_guard = p > 0.0f ? p : 0.0f;
    d->progress_v = 0.0f;
}

/* One update; returns the camera position (see the top of this file). */
static inline VrDashV3 vr_dash_update(VrDash *d, const VrDashParams *p, VrDashV3 anchor, int move,
                                      const VrDashHint *hint, double t) {
    if (!d->has_last) {
        d->has_last = 1;
        d->mode = VR_DASH_IDLE;
        d->out = d->last_anchor = anchor;
        d->t_last = t;
        d->move = 0;
    }
    float dt = (float)(t - d->t_last);
    if (!(dt > 0.0f)) dt = 0.0f;
    d->snapped = 0;
    const VrDashV3 step = vr_dash_sub(anchor, d->last_anchor);
    /* A warp (or a long stall): snap to the anchor; a move in progress is over. */
    if ((p->teleport > 0.0f && vr_dash_len(step) > p->teleport) || dt > 1.0f || !isfinite(anchor.x) ||
        !isfinite(anchor.y) || !isfinite(anchor.z)) {
        const int keep = d->move == move ? 0 : move; /* a move that began in this very update still starts */
        const VrDashV3 a = isfinite(anchor.x) && isfinite(anchor.y) && isfinite(anchor.z) ? anchor : d->last_anchor;
        d->mode = VR_DASH_IDLE;
        d->out = d->last_anchor = a;
        d->vel = vr_dash_v3(0.0f, 0.0f, 0.0f);
        d->vignette = d->speed = 0.0f;
        d->snapped = 1;
        d->t_last = t;
        d->move = move;
        if (!keep) return a;
        d->move = 0; /* start it below */
        dt = 0.0f;
    }
    /* Where the current mode puts the camera now. */
    VrDashV3 out = anchor;
    if (d->mode == VR_DASH_MOVE) {
        out = vr_dash_eval_move(d, p, anchor, t, dt);
    } else if (d->mode == VR_DASH_SETTLE) {
        VrDashV3 rate;
        const float tau = (float)(t - d->ts0);
        out = vr_dash_add(anchor, vr_dash_settle_offset(d, tau, &rate));
        d->vel = rate;
        if (tau >= d->settle_time) {
            d->mode = VR_DASH_IDLE;
            out = anchor;
            d->vel = vr_dash_v3(0.0f, 0.0f, 0.0f);
        }
    } else {
        d->vel = vr_dash_v3(0.0f, 0.0f, 0.0f);
    }
    if (move && move != d->move) {
        /* A new move: slide from where the camera is now to where it will end. */
        d->mode = VR_DASH_MOVE;
        d->kind = hint ? hint->kind : VR_DASH_DODGE;
        d->start = out;
        d->anchor0 = anchor;
        d->t0 = d->t_begin = t;
        d->duration = hint && hint->duration > 0.0f ? hint->duration : p->duration;
        d->duration = vr_dash_clamp(d->duration, 0.02f, 5.0f);
        d->height = d->kind == VR_DASH_JUMP ? (hint && hint->height >= 0.0f ? hint->height : p->jump_height) : 0.0f;
        d->bump = p->jump_duration > 0.0f ? p->jump_duration : d->duration;
        const float dir = hint ? vr_dash_len_h(hint->direction) : 0.0f;
        d->axis_known = d->axis_hinted = dir > 1e-6f;
        d->axis = d->axis_known ? vr_dash_v3(hint->direction.x / dir, 0.0f, hint->direction.z / dir) : vr_dash_v3(0.0f, 0.0f, 0.0f);
        d->net_s = d->net_v = vr_dash_v3(0.0f, 0.0f, 0.0f);
        d->distance_plan = hint && hint->distance > 0.0f ? hint->distance : 0.0f;
        d->distance = d->distance_plan;
        d->distance_v = 0.0f;
        d->progress_max = d->progress_s = d->progress_v = d->progress_guard = 0.0f;
        out = vr_dash_eval_move(d, p, anchor, t, 0.0f);
    } else if (!move && d->move && d->mode == VR_DASH_MOVE) {
        /* The move ended (or was cancelled): blend what is left out. */
        vr_dash_begin_settle(d, p, out, d->vel, anchor, t);
        if (d->mode == VR_DASH_IDLE) out = anchor;
    }
    d->move = move;
    d->speed = vr_dash_len(d->vel);
    const float want = p->vignette_speed > 0.0f ? vr_dash_clamp(d->speed / p->vignette_speed, 0.0f, 1.0f) : 0.0f;
    if (want >= d->vignette) d->vignette = want;
    else if (dt > 0.0f) d->vignette = want + (d->vignette - want) * expf(-dt / (p->vignette_release > 1e-3f ? p->vignette_release : 1e-3f));
    if (d->mode == VR_DASH_IDLE) d->u = 0.0f;
    d->out = out;
    d->last_anchor = anchor;
    d->t_last = t;
    return out;
}

#endif
