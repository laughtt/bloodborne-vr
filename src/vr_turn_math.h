/* VR turning and heading ("yaw") model, shared by the game's first person (src/runtime_vr_fp.c, C)
 * and the 3D sandbox (gpu/xr_bridge/sandbox3d, C++), so both turn and walk the same way. Pure
 * logic: static inline, no OpenXR, no globals, no I/O; compiles as C (C99/C11) and as C++.
 * Design: docs/vr/findings/16-first-person.md section 9. Tests: tests/test_vr_turn.c (built as C
 * and as C++: ninja -C out/gpu vr-turn-test vr-turn-test-cpp).
 *
 * The model (VR style: the view is decoupled from the character):
 *     view yaw = psi + head_yaw
 *     move yaw = psi                     (move=body)
 *              | psi + head_yaw          (move=head)
 *              | psi + controller_yaw    (move=controller; the head when the controller is lost)
 *  - psi, the "body yaw" (the play space's turn in the world), changes ONLY through the turn
 *    stick (smooth turn at a rate, or snap turn), a recenter (which keeps the view where it is)
 *    and the opt-in lock-on auto-turn. Nothing the game does (the character's rotation, root
 *    motion, its camera, lock-on) ever turns the view.
 *  - head_yaw: the headset's heading relative to the recenter reference; it turns the view freely.
 *  - The character walks along the move yaw (the game: its camera is turned to it, findings/16
 *    section 3; the sandbox: the left stick through vrt_move_vector). Its body turns as the game
 *    animates it; the view does not follow.
 *  - Lock-on (VRT_LOCK_*): the view stays free in every policy, the game turns the body to the
 *    target. VIEW (default): walking stays relative to the move yaw and the right stick goes to
 *    the game (target switching), stick turning pauses. GAME: as VIEW, and the game's camera is
 *    left to the lock-on (walking relative to the target, as in flat play). TURN: the right stick
 *    keeps turning psi (no target switching). lock_auto_rad > 0 adds an auto-turn of psi toward
 *    the target when it drifts that far out of the view.
 *
 * Angles: radians in the "heading" sense, 0 = the app's world forward, positive = a turn to the
 * right (clockwise seen from above). Everything returned is wrapped to [-pi, pi).
 *     game (vr_cam_heading: left-handed, 0 = +Z, +X right)   heading as is
 *     OpenXR yaw (vr_xr_yaw: left positive)                    heading = -yaw
 *     sandbox (right-handed, -Z forward, yaw about +Y)         heading = -yaw
 *
 * Use, once per frame:
 *     VrTurnConfig cfg; vrt_config_default(&cfg);   (then set fields, or vrt_config_set per key)
 *     VrTurnState st;   vrt_reset(&st, start_heading);
 *     VrTurnInput in;   vrt_input_clear(&in); in.dt = ...; in.stick_x = ...; in.head_yaw = ...;
 *     VrTurnOutput out; vrt_update(&st, &cfg, &in, &out);
 *   then draw the view at out.view_yaw (with the head's pitch and roll), walk along out.move_yaw,
 *   and (sandbox) turn the play space by out.turn_delta about the head.
 * Replaying a game trace (BB_VR_FP_TRACE, findings/16 section 9.6) through vrt_update with the
 * same config reproduces its psi, view_yaw and move_yaw.
 */
#ifndef VR_TURN_MATH_H
#define VR_TURN_MATH_H
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VRT_PI 3.14159265358979f
#define VRT_DEG (VRT_PI / 180.0f)

enum { VRT_TURN_SNAP = 0, VRT_TURN_SMOOTH = 1, VRT_TURN_OFF = 2 };
enum { VRT_MOVE_BODY = 0, VRT_MOVE_HEAD = 1, VRT_MOVE_CONTROLLER = 2 };
enum { VRT_LOCK_VIEW = 0, VRT_LOCK_GAME = 1, VRT_LOCK_TURN = 2 };

typedef struct {
    int turn;              /* VRT_TURN_*: snap (default), smooth, off */
    float snap_rad;        /* snap angle (30 deg) */
    float snap_on;         /* |x| that fires a snap (0.6) */
    float snap_off;        /* |x| below which the stick re-arms (0.3) */
    float snap_repeat_s;   /* held past snap_on: snaps again every this many seconds (0.3; <= 0: never) */
    float smooth_rad_s;    /* smooth turn rate at full deflection (90 deg/s) */
    float deadzone;        /* smooth: |x| up to this does nothing (0.15) */
    float expo;            /* smooth: rate = max * m^expo, m = 0..1 beyond the dead zone (1: linear) */
    float accel_s;         /* smooth: time from rest to the full rate and back (0.08 s; 0: instant) */
    int move;              /* VRT_MOVE_*: what "left stick up" walks along (body) */
    int recenter_keeps_view; /* 1 (default): a recenter turns psi so the view does not jump */
    int lock;              /* VRT_LOCK_*: the lock-on policy (view) */
    float lock_auto_rad;   /* > 0: while locked, psi turns toward the target once it is this far off
                              the view, until it is within a quarter of it (0: never) */
    float max_dt;          /* a longer frame counts as this long (0.1 s): no big turn after a hitch */
} VrTurnConfig;

typedef struct {
    float dt;              /* seconds since the last update */
    float stick_x;         /* the turn stick's X, -1 (left) .. 1 (right) */
    float head_yaw;        /* the headset's heading relative to the recenter reference (0: none) */
    float ref_yaw;         /* the reference's own heading in the tracking space; a change is a
                              recenter (keep it 0 when the app has no reference) */
    int controller_valid;  /* controller_yaw is valid this frame */
    float controller_yaw;  /* the movement controller's heading relative to the reference */
    int locked;            /* the game is locked on a target */
    int target_valid;      /* target_yaw is valid */
    float target_yaw;      /* world heading from the player to the lock-on target */
} VrTurnInput;

typedef struct {
    float psi;             /* the body yaw (world) */
    float view_yaw;        /* psi + head_yaw: where the view looks */
    float move_yaw;        /* the heading "left stick up" walks along */
    float turn_delta;      /* psi's change this frame (stick, auto-turn, recenter), wrapped */
    float turn_rate;       /* rad/s of the smooth turn now (vignette); snaps show in `snapped` */
    int snapped;           /* -1 / +1: a snap fired this frame (a blink could hide it), else 0 */
    int recentered;        /* the reference changed this frame */
    int sync_game;         /* 1: the game's camera is turned to move_yaw this frame */
    int stick_to_game;     /* 1: the turn stick goes to the game (lock-on target switching) */
} VrTurnOutput;

typedef struct {
    int valid;             /* vrt_reset was called */
    float psi;
    double t;              /* the model's own clock (seconds): snap repeats */
    int snap_armed, snap_dir;
    int snap_block;        /* the stick must return to the centre before the next snap */
    double snap_next;
    float rate;            /* the smooth turn's current rate (rad/s) */
    int have_ref;
    float ref_yaw;
    int auto_on;           /* the lock-on auto-turn is turning */
} VrTurnState;

static inline int vrt_finite(float x) { return fabsf(x) <= 3.0e38f; } /* false for NaN and inf */
static inline float vrt_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* a wrapped to [-pi, pi) (0 when it is not finite). */
static inline float vrt_wrap(float a) {
    if (!vrt_finite(a)) return 0.0f;
    if (a >= -VRT_PI && a < VRT_PI) return a;
    a = fmodf(a + VRT_PI, 2.0f * VRT_PI);
    if (a < 0.0f) a += 2.0f * VRT_PI;
    a -= VRT_PI;
    return a >= VRT_PI ? -VRT_PI : a;
}

static inline void vrt_config_default(VrTurnConfig *c) {
    memset(c, 0, sizeof(*c));
    c->turn = VRT_TURN_SNAP;
    c->snap_rad = 30.0f * VRT_DEG;
    c->snap_on = 0.6f;
    c->snap_off = 0.3f;
    c->snap_repeat_s = 0.3f;
    c->smooth_rad_s = 90.0f * VRT_DEG;
    c->deadzone = 0.15f;
    c->expo = 1.0f;
    c->accel_s = 0.08f;
    c->move = VRT_MOVE_BODY;
    c->recenter_keeps_view = 1;
    c->lock = VRT_LOCK_VIEW;
    c->lock_auto_rad = 0.0f;
    c->max_dt = 0.1f;
}

static inline void vrt_input_clear(VrTurnInput *in) { memset(in, 0, sizeof(*in)); }

/* Starts (or restarts) at body yaw `psi`; the reference is taken from the next update. */
static inline void vrt_reset(VrTurnState *s, float psi) {
    memset(s, 0, sizeof(*s));
    s->valid = 1;
    s->psi = vrt_wrap(psi);
    s->snap_armed = 1;
}

/* The smooth turn's target rate (rad/s) for stick X `x`: 0 inside the dead zone, then
 * max * m^expo with m rising from 0 at the dead zone to 1 at full deflection. */
static inline float vrt_smooth_rate(const VrTurnConfig *c, float x) {
    const float a = fabsf(x);
    if (!vrt_finite(x) || a <= c->deadzone || !(c->smooth_rad_s > 0.0f)) return 0.0f;
    float m = c->deadzone < 1.0f ? (a - c->deadzone) / (1.0f - c->deadzone) : 1.0f;
    m = vrt_clamp(m, 0.0f, 1.0f);
    if (c->expo > 0.0f && c->expo != 1.0f) m = powf(m, c->expo);
    return (x > 0.0f ? 1.0f : -1.0f) * m * c->smooth_rad_s;
}

/* Snap detection: -1 (left), +1 (right) or 0. A snap fires when |x| crosses snap_on from the
 * re-arm zone (|x| < snap_off), or reverses, then repeats every snap_repeat_s while held. */
static inline int vrt_snap_step(VrTurnState *s, const VrTurnConfig *c, float x) {
    const float a = vrt_finite(x) ? fabsf(x) : 0.0f;
    if (a < c->snap_off) { s->snap_armed = 1; s->snap_dir = 0; s->snap_block = 0; return 0; }
    if (a < c->snap_on || s->snap_block) return 0;
    const int dir = x > 0.0f ? 1 : -1;
    if (s->snap_armed || dir != s->snap_dir) {
        s->snap_armed = 0;
        s->snap_dir = dir;
        s->snap_next = s->t + c->snap_repeat_s;
        return dir;
    }
    if (c->snap_repeat_s > 0.0f && s->t >= s->snap_next) {
        s->snap_next = s->t + c->snap_repeat_s;
        return dir;
    }
    return 0;
}

/* One frame of the model. */
static inline void vrt_update(VrTurnState *s, const VrTurnConfig *c, const VrTurnInput *in, VrTurnOutput *out) {
    if (!s->valid) vrt_reset(s, 0.0f);
    float dt = vrt_finite(in->dt) ? in->dt : 0.0f;
    dt = vrt_clamp(dt, 0.0f, c->max_dt > 0.0f ? c->max_dt : 0.1f);
    s->t += dt;
    const float psi0 = s->psi;
    memset(out, 0, sizeof(*out));
    /* Recenter: the reference moved by d; the head's yaw relative to it jumped by -d, so psi
     * takes +d and the view stays. */
    const float ref = vrt_finite(in->ref_yaw) ? in->ref_yaw : 0.0f;
    if (s->have_ref) {
        const float d = vrt_wrap(ref - s->ref_yaw);
        if (fabsf(d) > 1e-4f) {
            out->recentered = 1;
            if (c->recenter_keeps_view) s->psi = vrt_wrap(s->psi + d);
        }
    }
    s->have_ref = 1;
    s->ref_yaw = ref;
    /* Lock-on policy. */
    const int locked = in->locked != 0;
    out->stick_to_game = locked && c->lock != VRT_LOCK_TURN;
    out->sync_game = !(locked && c->lock == VRT_LOCK_GAME);
    /* The turn stick. */
    const float x = vrt_finite(in->stick_x) ? vrt_clamp(in->stick_x, -1.0f, 1.0f) : 0.0f;
    float target_rate = 0.0f;
    if (c->turn != VRT_TURN_SNAP || out->stick_to_game) {
        /* not snapping: a stick still pushed when snapping resumes must return to the centre first */
        s->snap_block = fabsf(x) >= c->snap_off || out->stick_to_game;
        s->snap_armed = !s->snap_block;
        s->snap_dir = 0;
    }
    if (out->stick_to_game || c->turn == VRT_TURN_OFF) {
        /* no stick turning: a smooth turn in progress eases out */
    } else if (c->turn == VRT_TURN_SMOOTH) {
        target_rate = vrt_smooth_rate(c, x);
    } else {
        const int step = vrt_snap_step(s, c, x);
        if (step) {
            s->psi = vrt_wrap(s->psi + (float)step * c->snap_rad);
            out->snapped = step;
        }
    }
    /* Lock-on auto-turn: toward the target while it is far out of the view. */
    const float head = vrt_finite(in->head_yaw) ? in->head_yaw : 0.0f;
    if (locked && c->lock_auto_rad > 0.0f && in->target_valid && vrt_finite(in->target_yaw)) {
        const float off = vrt_wrap(in->target_yaw - (s->psi + head));
        if (fabsf(off) > c->lock_auto_rad) s->auto_on = 1;
        else if (fabsf(off) < 0.25f * c->lock_auto_rad) s->auto_on = 0;
        if (s->auto_on && c->turn == VRT_TURN_SNAP && !out->snapped && s->t >= s->snap_next) {
            const float step = fabsf(off) < c->snap_rad ? off : (off > 0.0f ? c->snap_rad : -c->snap_rad);
            s->psi = vrt_wrap(s->psi + step);
            s->snap_next = s->t + (c->snap_repeat_s > 0.0f ? c->snap_repeat_s : 0.3f);
            out->snapped = off > 0.0f ? 1 : -1;
        } else if (s->auto_on && c->turn != VRT_TURN_SNAP) {
            target_rate = off > 0.0f ? c->smooth_rad_s : -c->smooth_rad_s;
        }
    } else {
        s->auto_on = 0;
    }
    if (target_rate != 0.0f || s->rate != 0.0f) {
        if (c->accel_s > 0.0f && c->smooth_rad_s > 0.0f) {
            const float step = c->smooth_rad_s / c->accel_s * dt;
            const float d = target_rate - s->rate;
            s->rate = fabsf(d) <= step ? target_rate : s->rate + (d > 0.0f ? step : -step);
        } else {
            s->rate = target_rate;
        }
        float turn = s->rate * dt;
        if (s->auto_on && in->target_valid) { /* never past the target */
            const float off = vrt_wrap(in->target_yaw - (s->psi + head));
            if ((turn > 0.0f && turn > off && off > 0.0f) || (turn < 0.0f && turn < off && off < 0.0f)) turn = off;
        }
        s->psi = vrt_wrap(s->psi + turn);
    }
    /* Outputs. */
    out->psi = s->psi;
    out->turn_delta = vrt_wrap(s->psi - psi0);
    out->turn_rate = s->rate;
    out->view_yaw = vrt_wrap(s->psi + head);
    float rel = 0.0f;
    if (c->move == VRT_MOVE_HEAD) rel = head;
    else if (c->move == VRT_MOVE_CONTROLLER)
        rel = in->controller_valid && vrt_finite(in->controller_yaw) ? in->controller_yaw : head;
    out->move_yaw = vrt_wrap(s->psi + rel);
}

/* The left stick (x right, y up = forward, -1..1) as a walking direction for heading h: the
 * components along the world's right axis and forward axis (those of heading 0), magnitude kept.
 * World vectors: game (x, z) = (right, forward); sandbox (x, z) = (right, -forward). */
static inline void vrt_move_vector(float h, float sx, float sy, float *right, float *forward) {
    const float s = sinf(h), c = cosf(h);
    *right = sx * c + sy * s;
    *forward = -sx * s + sy * c;
}

/* Settings by name, for the game's control file (fp.txt) and the sandbox's JSON: key=value pairs
 * with the same names and units in both. turn=snap|smooth|off, snap=<deg>, turn_speed=<deg/s>,
 * deadzone=<0..1>, expo=<p>, turn_accel=<s>, snap_on=<0..1>, snap_off=<0..1>, snap_repeat=<s>,
 * move=body|head|controller, recenter=view|reset, lock=view|game|turn, lock_auto=<deg>.
 * Returns 1 when the key is one of them and the value was taken, 0 otherwise (unknown key or a
 * value out of range: nothing changes). */
static inline int vrt_config_set(VrTurnConfig *c, const char *key, const char *value) {
    const float x = (float)atof(value);
    const int ok = vrt_finite(x);
    if (!strcmp(key, "turn")) {
        if (!strcmp(value, "snap")) c->turn = VRT_TURN_SNAP;
        else if (!strcmp(value, "smooth")) c->turn = VRT_TURN_SMOOTH;
        else if (!strcmp(value, "off")) c->turn = VRT_TURN_OFF;
        else return 0;
        return 1;
    }
    if (!strcmp(key, "move")) {
        if (!strcmp(value, "body")) c->move = VRT_MOVE_BODY;
        else if (!strcmp(value, "head")) c->move = VRT_MOVE_HEAD;
        else if (!strcmp(value, "controller")) c->move = VRT_MOVE_CONTROLLER;
        else return 0;
        return 1;
    }
    if (!strcmp(key, "lock")) {
        if (!strcmp(value, "view")) c->lock = VRT_LOCK_VIEW;
        else if (!strcmp(value, "game")) c->lock = VRT_LOCK_GAME;
        else if (!strcmp(value, "turn")) c->lock = VRT_LOCK_TURN;
        else return 0;
        return 1;
    }
    if (!strcmp(key, "recenter")) {
        if (!strcmp(value, "view")) c->recenter_keeps_view = 1;
        else if (!strcmp(value, "reset")) c->recenter_keeps_view = 0;
        else return 0;
        return 1;
    }
    if (!ok) return 0;
    if (!strcmp(key, "snap") && x >= 1.0f && x <= 180.0f) c->snap_rad = x * VRT_DEG;
    else if (!strcmp(key, "turn_speed") && x >= 1.0f && x <= 720.0f) c->smooth_rad_s = x * VRT_DEG;
    else if (!strcmp(key, "deadzone") && x >= 0.0f && x < 0.95f) c->deadzone = x;
    else if (!strcmp(key, "expo") && x >= 0.2f && x <= 5.0f) c->expo = x;
    else if (!strcmp(key, "turn_accel") && x >= 0.0f && x <= 2.0f) c->accel_s = x;
    else if (!strcmp(key, "snap_on") && x > 0.05f && x <= 1.0f) c->snap_on = x;
    else if (!strcmp(key, "snap_off") && x >= 0.0f && x < 1.0f) c->snap_off = x;
    else if (!strcmp(key, "snap_repeat") && x >= 0.0f && x <= 5.0f) c->snap_repeat_s = x;
    else if (!strcmp(key, "lock_auto") && x >= 0.0f && x <= 180.0f) c->lock_auto_rad = x * VRT_DEG;
    else return 0;
    return 1;
}

static inline const char *vrt_turn_name(int t) {
    return t == VRT_TURN_SMOOTH ? "smooth" : t == VRT_TURN_OFF ? "off" : "snap";
}
static inline const char *vrt_move_name(int m) {
    return m == VRT_MOVE_HEAD ? "head" : m == VRT_MOVE_CONTROLLER ? "controller" : "body";
}
static inline const char *vrt_lock_name(int l) {
    return l == VRT_LOCK_GAME ? "game" : l == VRT_LOCK_TURN ? "turn" : "view";
}

#endif
