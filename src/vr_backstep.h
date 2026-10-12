/* Backward evade = the backstep (BB_VR_BACKSTEP / backstep= in out/fp.txt, src/runtime_vr_fp.c,
 * docs/vr/findings/19-dash-locomotion.md section 11): the pure part, no I/O, no globals.
 *
 * In the game, circle tapped with the stick pushed is a roll or quickstep along the stick; circle
 * tapped with the stick neutral is the backstep (decided when circle is released). So when the dodge
 * is pressed while the stick points BEHIND the character, the pad data the game reads gets a neutral
 * left stick from the press until a few frames after the release: the game sees a plain tap with no
 * stick and does its own backstep. Nothing else is touched (no position, no animation).
 * "Behind": the stick's world direction (the move heading + the stick angle: in VR the movement is
 * head-relative) differs from the player's VIEW heading by more than the threshold (default 120 deg). The
 * character's own facing is not used: out of lock-on it turns to its walk direction, so walking
 * backwards it already faces the movement and the angle would be ~0.
 * A circle held longer than the tap window is a sprint: the stick is given back at once. */
#ifndef VR_BACKSTEP_H
#define VR_BACKSTEP_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VR_BACKSTEP_BTN_CIRCLE 0x2000u
#define VR_BACKSTEP_ANGLE_DEFAULT 120.0f
#define VR_BACKSTEP_TAP_S 0.30     /* a circle held longer is a sprint, not a dodge */
#define VR_BACKSTEP_TAIL_S 0.10    /* neutral stick kept after the release (the game's dodge frame) */
#define VR_BACKSTEP_GRACE_S 0.08   /* the stick may arrive this long after the button */

typedef struct {
    int on;            /* backstep=1|0 */
    float angle_deg;   /* backstep_angle= */
    int in_lock;       /* backstep_lock=1: also while locked on; 0: only unlocked */
} VrBackstepCfg;

typedef struct {
    int armed;         /* the stick is being held neutral */
    int held;          /* circle is down */
    double t_press, t_release;
    uint32_t last;
} VrBackstep;

static inline void vr_backstep_cfg_default(VrBackstepCfg *c) { c->on = 1; c->angle_deg = VR_BACKSTEP_ANGLE_DEFAULT; c->in_lock = 1; }

/* An angle setting: finite, clamped to 30..180 deg (else the default). */
static inline float vr_backstep_clamp_angle(float a) {
    if (!isfinite(a)) return VR_BACKSTEP_ANGLE_DEFAULT;
    return a < 30.0f ? 30.0f : a > 180.0f ? 180.0f : a;
}

static inline float vr_backstep_wrap(float a) {
    const float pi = 3.14159265358979f;
    while (a > pi) a -= 2.0f * pi;
    while (a < -pi) a += 2.0f * pi;
    return a;
}

/* A raw PS4 pad byte pair (0..255, 128 = centre; Y: 0 = up/forward, 255 = down/back, as the keyboard path of
 * src/runtime_pad.c writes W = 0, S = 255) as sx (right +) and sy (up/forward +). */
static inline void vr_backstep_stick(uint8_t lx, uint8_t ly, float *sx, float *sy) {
    *sx = ((float)lx - 128.0f) / 127.0f;
    *sy = (128.0f - (float)ly) / 127.0f;
}

/* 1 when the stick (sx right, sy up, -1..1; deadzone 0.3) points behind `ref` (the view heading; NAN = judge the raw stick against
 * the move heading): its world heading
 * is move_yaw + atan2(sx, sy), the heading sense of the camera (radians). */
static inline int vr_backstep_is_back(float ref, float move_yaw, float sx, float sy, float angle_deg) {
    if (!isfinite(move_yaw) || !isfinite(sx) || !isfinite(sy)) return 0;
    if (sx * sx + sy * sy < 0.3f * 0.3f) return 0;
    const float world = move_yaw + atan2f(sx, sy);
    return fabsf(vr_backstep_wrap(world - (isfinite(ref) ? ref : move_yaw))) > angle_deg * 3.14159265358979f / 180.0f;
}

/* "backstep=<x>" / "backstep_angle=<x>" / "backstep_lock=<x>" at the start of a token; 1 when present. */
static inline int vr_backstep_parse_key(const char *text, const char *key, float *out) {
    const size_t n = strlen(key);
    const char *p = text;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == text || p[-1] == ' ' || p[-1] == '\n' || p[-1] == '\r' || p[-1] == '\t') && p[n] == '=') {
            const float x = strtof(p + n + 1, NULL);
            if (isfinite(x)) { *out = x; return 1; }
        }
        p += n;
    }
    return 0;
}

/* One pad sample (the pad thread). `back`: vr_backstep_is_back for this sample; `locked`: lock-on.
 * Returns 1 when the left stick must be zeroed in this sample; *started is set on the sample that arms. */
static inline int vr_backstep_update(VrBackstep *s, const VrBackstepCfg *c, double now, uint32_t buttons, int back,
                                     int locked, int *started) {
    if (started) *started = 0;
    const uint32_t pressed = buttons & ~s->last, released = s->last & ~buttons;
    s->last = buttons;
    if (!c->on || (locked && !c->in_lock)) { s->armed = s->held = 0; return 0; }
    if (pressed & VR_BACKSTEP_BTN_CIRCLE) { s->held = 1; s->t_press = now; s->armed = 0; }
    if (s->held && !(buttons & VR_BACKSTEP_BTN_CIRCLE)) s->held = 0;
    if (released & VR_BACKSTEP_BTN_CIRCLE) {
        if (s->armed) s->t_release = now;
    }
    if (s->held && !s->armed && back && now - s->t_press <= VR_BACKSTEP_GRACE_S) {
        s->armed = 1;
        s->t_release = 0.0;
        if (started) *started = 1;
    }
    if (!s->armed) return 0;
    if (s->held) {
        if (now - s->t_press > VR_BACKSTEP_TAP_S) { s->armed = 0; return 0; } /* a sprint */
        return 1;
    }
    if (now - s->t_release <= VR_BACKSTEP_TAIL_S) return 1;
    s->armed = 0;
    return 0;
}

#endif
