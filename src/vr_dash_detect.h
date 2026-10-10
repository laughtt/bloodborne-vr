/* The comfort dash in the game (BB_VR_DASH=1, src/runtime_vr_dash.c, docs/vr/findings/19): the
 * pure parts, no I/O and no globals (tests/test_vr_dash.c):
 *  - which of the player's animations are dodges, quicksteps, backsteps and jumps, with their root
 *    motion (measured from the user's dump by scripts/vr_assets/dodge_extract.py: distance,
 *    direction in the character's frame, when the root stops; the same sub-IDs in every a00..a09
 *    group);
 *  - where such a move goes in the world for the heading the character faces;
 *  - the newest entry of the TimeAct module's animation ring (findings/13 3.4);
 *  - the input-driven fallback: a circle tap, a still circle tap, L3 while sprinting;
 *  - the settings tokens (BB_VR_DASH_SET, the BB_VR_DASH_CONTROL file).
 * The filter itself is gpu/xr_bridge/vr_dash.h, shared with the sandbox.
 *
 * Conventions (findings/08, 16): game world +Y up, a heading/yaw is radians about +Y with 0
 * looking along world +Z and positive turning right, forward = (sin h, 0, cos h). */
#ifndef VR_DASH_DETECT_H
#define VR_DASH_DETECT_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/xr_bridge/vr_dash.h"

enum { VR_DASH_MOVE_NONE = 0, VR_DASH_MOVE_ROLL, VR_DASH_MOVE_QUICKSTEP, VR_DASH_MOVE_BACKSTEP, VR_DASH_MOVE_JUMP };

typedef struct {
    int32_t sub;     /* animation sub-ID: aGGG_<sub>, the TimeAct ID's last 6 digits */
    int kind;        /* VR_DASH_MOVE_* */
    float distance;  /* m: the clip's horizontal root motion */
    float direction; /* degrees in the character's frame: 0 forward, +90 right, 180 back */
    float motion;    /* s: the root stops moving */
    float clip;      /* s: the clip's length */
} VrDashMove;

/* c0000_a00_md.anibnd (a000_*): every clip with root motion of the dodge family. Not in it: the
 * 8000-8040 drops (off ledges: the camera follows a fall) and the 5000/5200 dashes (no
 * invincibility, meaning unknown). */
static const VrDashMove vr_dash_moves[] = {
    {7000, VR_DASH_MOVE_BACKSTEP, 2.48f, 180.0f, 0.83f, 1.37f},
    {7050, VR_DASH_MOVE_BACKSTEP, 2.48f, 180.0f, 0.67f, 1.17f},
    {7100, VR_DASH_MOVE_QUICKSTEP, 4.00f, 0.0f, 0.93f, 1.50f},   /* i-frames 0-0.37 s */
    {7101, VR_DASH_MOVE_QUICKSTEP, 4.00f, 180.0f, 1.07f, 1.23f},
    {7102, VR_DASH_MOVE_QUICKSTEP, 4.00f, -90.0f, 0.83f, 1.27f},
    {7103, VR_DASH_MOVE_QUICKSTEP, 4.00f, 90.0f, 1.27f, 1.27f},
    {7104, VR_DASH_MOVE_QUICKSTEP, 4.00f, -25.0f, 0.93f, 1.50f},
    {7105, VR_DASH_MOVE_QUICKSTEP, 4.00f, 25.0f, 0.93f, 1.50f},
    {7106, VR_DASH_MOVE_QUICKSTEP, 4.00f, -155.0f, 1.07f, 1.23f},
    {7107, VR_DASH_MOVE_QUICKSTEP, 4.00f, 155.0f, 1.07f, 1.23f},
    {7150, VR_DASH_MOVE_QUICKSTEP, 3.70f, 0.0f, 0.90f, 1.07f},   /* i-frames 0-0.27 s */
    {7151, VR_DASH_MOVE_QUICKSTEP, 4.00f, 180.0f, 0.97f, 1.07f},
    {7152, VR_DASH_MOVE_QUICKSTEP, 4.00f, -90.0f, 1.07f, 1.07f},
    {7153, VR_DASH_MOVE_QUICKSTEP, 4.00f, 90.0f, 1.07f, 1.07f},
    {7154, VR_DASH_MOVE_QUICKSTEP, 3.70f, -25.0f, 0.93f, 1.07f},
    {7155, VR_DASH_MOVE_QUICKSTEP, 3.70f, 25.0f, 0.90f, 1.07f},
    {7156, VR_DASH_MOVE_QUICKSTEP, 4.00f, -155.0f, 0.97f, 1.07f},
    {7157, VR_DASH_MOVE_QUICKSTEP, 4.00f, 155.0f, 0.97f, 1.07f},
    {7200, VR_DASH_MOVE_ROLL, 4.00f, 0.0f, 0.93f, 1.67f},        /* the forward roll, i-frames 0-0.37 s */
    {7250, VR_DASH_MOVE_ROLL, 4.00f, 0.0f, 1.00f, 1.27f},
    {7300, VR_DASH_MOVE_ROLL, 2.50f, 180.0f, 1.53f, 1.53f},
    {7350, VR_DASH_MOVE_ROLL, 2.00f, 180.0f, 1.50f, 1.50f},
    {7400, VR_DASH_MOVE_ROLL, 4.00f, 0.0f, 1.27f, 1.50f},
    {7401, VR_DASH_MOVE_ROLL, 4.00f, 180.0f, 1.50f, 1.67f},
    {7402, VR_DASH_MOVE_ROLL, 4.00f, -90.0f, 1.00f, 1.67f},
    {7403, VR_DASH_MOVE_ROLL, 4.00f, 90.0f, 1.00f, 1.67f},
    {7404, VR_DASH_MOVE_ROLL, 4.00f, -24.8f, 1.27f, 1.50f},
    {7405, VR_DASH_MOVE_ROLL, 4.00f, 25.2f, 1.27f, 1.50f},
    {7406, VR_DASH_MOVE_ROLL, 4.00f, -156.6f, 1.50f, 1.67f},
    {7407, VR_DASH_MOVE_ROLL, 4.00f, 153.4f, 1.50f, 1.67f},
    {7450, VR_DASH_MOVE_ROLL, 4.00f, 0.0f, 1.50f, 1.73f},
    {7451, VR_DASH_MOVE_ROLL, 4.00f, 180.0f, 1.00f, 1.60f},
    {7452, VR_DASH_MOVE_ROLL, 4.00f, -90.0f, 1.17f, 1.60f},
    {7453, VR_DASH_MOVE_ROLL, 4.00f, 90.0f, 1.17f, 1.60f},
    {7454, VR_DASH_MOVE_ROLL, 4.00f, -25.2f, 1.53f, 1.73f},
    {7455, VR_DASH_MOVE_ROLL, 4.00f, 24.8f, 1.50f, 1.73f},
    {7456, VR_DASH_MOVE_ROLL, 4.00f, -154.8f, 1.57f, 1.60f},
    {7457, VR_DASH_MOVE_ROLL, 4.00f, 155.2f, 1.57f, 1.60f},
    {7800, VR_DASH_MOVE_JUMP, 6.84f, 0.0f, 2.07f, 2.07f},        /* the running jump: a leap, a landing roll */
    {8250, VR_DASH_MOVE_ROLL, 2.95f, 0.0f, 1.03f, 1.33f},
};

static inline const char *vr_dash_move_name(int kind) {
    return kind == VR_DASH_MOVE_ROLL ? "roll" : kind == VR_DASH_MOVE_QUICKSTEP ? "quickstep"
         : kind == VR_DASH_MOVE_BACKSTEP ? "backstep" : kind == VR_DASH_MOVE_JUMP ? "jump" : "none";
}

/* The move of a TimeAct animation ID (group * 1000000 + sub; groups a000..a009), or NULL. */
static inline const VrDashMove *vr_dash_move_lookup(int32_t anim) {
    if (anim < 0 || anim / 1000000 > 9) return NULL;
    const int32_t sub = anim % 1000000;
    for (size_t i = 0; i < sizeof(vr_dash_moves) / sizeof(*vr_dash_moves); ++i)
        if (vr_dash_moves[i].sub == sub) return &vr_dash_moves[i];
    return NULL;
}

/* The world direction of a move `degrees` from where the character faces: (sin(f + d), 0,
 * cos(f + d)) for the heading `facing` of its face (headings grow to the right, so a move to the
 * character's right is facing + 90 degrees). The physics yaw (+0x1D4) is the heading of the
 * character's back (findings/16 section 2): pass vr_facing_heading(yaw) of vr_fp_math.h. */
static inline VrDashV3 vr_dash_move_direction(float facing, float degrees) {
    const float a = facing + degrees * 0.017453293f;
    return vr_dash_v3(sinf(a), 0.0f, cosf(a));
}

/* The newest entry of the TimeAct module's ring: a copy of its first 0xC8 bytes; 10 entries
 * {i32 anim, f32, f32, f32} at +0x20, the write index at +0xC0, so the newest is (head + 9) % 10
 * (findings/13 3.4, the push function 0x1E27FB0). Returns 0 when the index is not 0..9. */
static inline int vr_dash_ring_newest(const unsigned char *module, int32_t *anim, int32_t *head, float f[3]) {
    int32_t h;
    memcpy(&h, module + 0xC0, 4);
    if (h < 0 || h > 9) return 0;
    memcpy(anim, module + 0x20 + 16 * ((h + 9) % 10), 4);
    if (f) memcpy(f, module + 0x20 + 16 * ((h + 9) % 10) + 4, 12);
    *head = h;
    return 1;
}

/* Whether the ring's newest entry starts a new move. The game pushes the playing clip's ID again
 * on every frame (the write index advances, the ID stays: 407 one-frame "moves" for a few rolls in
 * the headset log), so the index alone is no event. A different ID is one. The same ID is one when
 * a restart shows in the entry's floats (one of the three is the clip time, which jumps back on a
 * restart: any float dropping by more than 0.1 s; which one is [L]), so a roll chained 0.5 s into
 * the first is caught. `varied` says that a float has changed since the move began (the time is
 * readable); if not, the fallback is the clip's length having run. Never while no move is in
 * progress (it ended early, blocked: the clip is still the old one). `since` is the time since
 * the current move began. */
static inline int vr_dash_ring_new_move(int have_move, int32_t prev_anim, int32_t anim, const float prev_f[3],
                                        const float f[3], int varied, double since, const VrDashMove *m) {
    if (anim != prev_anim) return 1;
    if (!have_move || !m) return 0;
    for (int i = 0; i < 3; ++i)
        if (isfinite(prev_f[i]) && isfinite(f[i]) && f[i] < prev_f[i] - 0.1f) return 1;
    return !varied && since >= (double)m->clip;
}

/* ---- a move stopped short (a roll into a wall or an enemy) ---- */

typedef struct {
    double slow_since; /* the root has been slow since then (< 0: it is not) */
} VrDashStall;

static inline void vr_dash_stall_reset(VrDashStall *s) { s->slow_since = -1.0; }
/* 1 when the character's root (the feet, not the eye: the head pauses as it comes over in a roll)
 * stopped short: slower than 0.5 m/s for 0.12 s, 0.2 s or more into the move, before the clip's
 * root motion should end, more than 0.4 m short of its distance. `t` is the time since the move
 * started, `progress` the root's travel along the move, `speed` its horizontal speed. The dash
 * then ends at once and the camera settles onto the eye, instead of waiting out the clip up to
 * max_lead ahead (inside the wall). */
static inline int vr_dash_stalled(VrDashStall *s, const VrDashMove *m, double t, float progress, float speed) {
    if (!m || t < 0.2 || t > m->motion || progress > m->distance - 0.4f || speed >= 0.5f) {
        s->slow_since = -1.0;
        return 0;
    }
    if (s->slow_since < 0.0) s->slow_since = t;
    return t - s->slow_since >= 0.12;
}

/* ---- the input-driven fallback (the bridge's pad: circle = A, L3 = left stick click) ---- */

#define VR_DASH_BTN_L3 0x2u
#define VR_DASH_BTN_CIRCLE 0x2000u

typedef struct {
    double circle_since; /* circle went down then (< 0: up) */
    int moving_since;    /* the stick moved during that press */
    uint32_t last;
} VrDashInput;

static inline void vr_dash_input_reset(VrDashInput *s) {
    s->circle_since = -1.0;
    s->moving_since = 0;
    s->last = 0;
}
/* One pad sample: circle released within `tap` seconds of its press = a roll or quickstep (the
 * stick pushed) or a backstep (not); L3 pressed while circle is held and the stick pushed (a
 * sprint) = a jump. Returns that VR_DASH_MOVE_*, else 0. */
static inline int vr_dash_input_update(VrDashInput *s, double now, uint32_t buttons, int moving, double tap) {
    const uint32_t pressed = buttons & ~s->last, released = s->last & ~buttons;
    s->last = buttons;
    int kind = VR_DASH_MOVE_NONE;
    if (pressed & VR_DASH_BTN_CIRCLE) {
        s->circle_since = now;
        s->moving_since = moving;
    }
    if (s->circle_since >= 0.0 && moving) s->moving_since = 1;
    if ((released & VR_DASH_BTN_CIRCLE) && s->circle_since >= 0.0) {
        if (now - s->circle_since < tap) kind = s->moving_since || moving ? VR_DASH_MOVE_ROLL : VR_DASH_MOVE_BACKSTEP;
        s->circle_since = -1.0;
    }
    if ((pressed & VR_DASH_BTN_L3) && (buttons & VR_DASH_BTN_CIRCLE) && moving) kind = VR_DASH_MOVE_JUMP;
    return kind;
}

/* ---- settings ---- */

enum { VR_DASH_DETECT_AUTO = 0, VR_DASH_DETECT_ANIM = 1, VR_DASH_DETECT_INPUT = 2 };

typedef struct {
    int on;               /* dash=0|1 */
    int log;              /* log=0|1 */
    int detect;           /* detect=auto|anim|input */
    float jump_duration;  /* jump_s=<s>: a jump's slide and bump (the leap is ~0.85 s in the air) */
    float tap;            /* tap=<s>: the input fallback's longest circle tap */
    float vignette;       /* vignette=<0..1>: the strength handed on (vr_dash_vignette) */
    VrDashParams params;  /* duration= ease= power= settle= settle_speed= lead= drop= jump= */
} VrDashSettings;

static inline void vr_dash_settings_default(VrDashSettings *s) {
    memset(s, 0, sizeof(*s));
    s->on = 1;
    s->detect = VR_DASH_DETECT_AUTO;
    s->jump_duration = 0.9f;
    s->tap = 0.3f;
    s->vignette = 0.6f;
    vr_dash_params_default(&s->params);
    s->params.duration = 0.4f;
    s->params.power = 2.0f; /* quad: 20 m/s at the start of a 4 m roll */
    s->params.jump_duration = 0.9f;
    s->params.follow = 1; /* the camera follows the character's real position, never leads it */
    s->params.follow_omega = 14.0f;
}

/* Whitespace-separated key=value tokens; unknown or out-of-range ones are skipped. Returns how
 * many were applied. */
static inline int vr_dash_settings_parse(const char *text, VrDashSettings *s) {
    int applied = 0;
    while (text && *text) {
        while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r' || *text == ',') ++text;
        if (!*text) break;
        const char *end = text;
        while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r' && *end != ',') ++end;
        char token[64];
        const size_t n = (size_t)(end - text) < sizeof(token) - 1 ? (size_t)(end - text) : sizeof(token) - 1;
        memcpy(token, text, n);
        token[n] = 0;
        text = end;
        char *eq = strchr(token, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = token, *val = eq + 1;
        char *stop;
        const float x = strtof(val, &stop);
        const int num = stop != val && isfinite(x);
        VrDashParams *p = &s->params;
        int ok = 1;
        if (!strcmp(key, "dash") && num) s->on = x != 0.0f;
        else if (!strcmp(key, "log") && num) s->log = x != 0.0f;
        else if (!strcmp(key, "detect"))
            s->detect = !strcmp(val, "anim") ? VR_DASH_DETECT_ANIM : !strcmp(val, "input") ? VR_DASH_DETECT_INPUT : VR_DASH_DETECT_AUTO;
        else if (!strcmp(key, "duration") && num && x >= 0.02f && x <= 3.0f) p->duration = x;
        else if (!strcmp(key, "ease")) {
            p->curve = VR_DASH_EASE_POWER;
            if (!strcmp(val, "linear")) p->power = 1.0f;
            else if (!strcmp(val, "quad")) p->power = 2.0f;
            else if (!strcmp(val, "cubic")) p->power = 3.0f;
            else if (!strcmp(val, "quart")) p->power = 4.0f;
            else if (!strcmp(val, "quint")) p->power = 5.0f;
            else if (!strcmp(val, "expo")) { p->curve = VR_DASH_EASE_EXPO; p->power = 10.0f; }
            else if (!strcmp(val, "smooth")) p->curve = VR_DASH_EASE_SMOOTH;
            else ok = 0;
        }
        else if (!strcmp(key, "power") && num && x >= 1.0f && x <= 12.0f) p->power = x;
        else if (!strcmp(key, "settle") && num && x >= 0.0f && x <= 3.0f) p->settle = x;
        else if (!strcmp(key, "settle_speed") && num && x >= 0.0f && x <= 50.0f) p->settle_speed = x;
        else if (!strcmp(key, "follow") && num) p->follow = x != 0.0f;
        else if (!strcmp(key, "follow_omega") && num && x >= 1.0f && x <= 100.0f) p->follow_omega = x;
        else if (!strcmp(key, "lead") && num && x >= 0.0f && x <= 20.0f) p->max_lead = x;
        else if (!strcmp(key, "drop") && num && x >= 0.0f && x <= 20.0f) p->max_drop = x;
        else if (!strcmp(key, "jump") && num && x >= 0.0f && x <= 2.0f) p->jump_height = x;
        else if (!strcmp(key, "jump_s") && num && x >= 0.05f && x <= 3.0f) s->jump_duration = p->jump_duration = x;
        else if (!strcmp(key, "tap") && num && x >= 0.05f && x <= 2.0f) s->tap = x;
        else if (!strcmp(key, "vignette") && num && x >= 0.0f && x <= 1.0f) s->vignette = x;
        else ok = 0;
        applied += ok;
    }
    return applied;
}

#endif
