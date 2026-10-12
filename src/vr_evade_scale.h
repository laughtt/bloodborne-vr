/* Evade distance scale (BB_VR_EVADE_SCALE / evade_scale= in out/fp.txt, src/runtime_vr_atkspeed.c,
 * docs/vr/findings/19-dash-locomotion.md section 10): the pure part, no I/O, no globals.
 *
 * A roll, quickstep or backstep moves the character by the clip's ROOT MOTION (4 m for the
 * quickstep and roll, 2.5 m for the backstep: the table of src/vr_dash_detect.h). The game applies
 * it as a position change per frame; the clip time, the i-frame window and the stamina cost do not
 * depend on the distance. So the scale adds a share of each frame's real horizontal step while an
 * evade clip plays: extra = (k - 1) * step, written to the feet by the caller. The clip keeps its
 * length and its i-frames; walking and running are not clips of the table and get nothing.
 * Guards: never past (k - 1) times the table's distance in total; only while the clip's root
 * motion still plays; no extra after a step whose speed fell below half the one before (a wall, the end
 * of the motion: the physics stopped the character, so the extra stops too); none for a teleport
 * (a step over 1.5 m). C11, header only. */
#ifndef VR_EVADE_SCALE_H
#define VR_EVADE_SCALE_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "vr_dash_detect.h"

#define VR_EVADE_SCALE_DEFAULT 1.5f
#define VR_BACKSTEP_SCALE_DEFAULT 2.0f /* backsteps have their own scale: BB_VR_BACKSTEP_SCALE / backstep_scale= */
#define VR_EVADE_SCALE_MAX 4.0f
/* a backstep's root motion decays early, so the "speed halved = a wall" rule would cut it short: its guard
 * only stops on a step under this share of the one before (a wall gives ~0) */
#define VR_EVADE_BACKSTEP_STOP 0.15f
#define VR_EVADE_MAX_STEP 1.5f

typedef struct {
    int active;            /* an evade clip is being followed */
    int32_t anim;
    const VrDashMove *move;
    double elapsed;        /* game seconds since the clip began */
    float f[3];            /* the ring entry's floats (a drop = the clip restarted) */
    int have_pos;
    float px, pz;          /* the feet after the last call (with our extra) */
    float last_speed;      /* the last step's speed in m/s, 0 = none yet */
    float given;           /* the extra distance added so far */
    int stopped;           /* a wall or the end: no more extra in this clip */
} VrEvade;

/* A scale from a number: <= 1 or not finite = off (0), else clamped to 1.05..VR_EVADE_SCALE_MAX. */
static inline float vr_evade_clamp(float k) {
    if (!isfinite(k) || !(k > 1.0f)) return 0.0f;
    return k < 1.05f ? 1.05f : k > VR_EVADE_SCALE_MAX ? VR_EVADE_SCALE_MAX : k;
}

/* "evade_scale=<x>" at the start of a token in `text`; 1 and the raw value when present. */
static inline int vr_evade_parse_key(const char *text, const char *key, float *out) {
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
static inline int vr_evade_parse(const char *text, float *out) { return vr_evade_parse_key(text, "evade_scale", out); }
/* "backstep_scale=<x>" */
static inline int vr_evade_parse_backstep(const char *text, float *out) { return vr_evade_parse_key(text, "backstep_scale", out); }

/* Online play (the position replication is not verified, findings/19 section 10): `online` is
 * BB_ONLINE's text, `role` BB_ONLINE_ROLE's. 1 when either says the game is in online mode. */
static inline int vr_evade_online(const char *online, const char *role) {
    return (online && online[0] == '1') || (role && role[0] != 0);
}

/* The evade clips: rolls, quicksteps and backsteps of the dash table; the running jump is not one. */
static inline const VrDashMove *vr_evade_move(int32_t anim) {
    const VrDashMove *m = vr_dash_move_lookup(anim);
    return m && m->kind != VR_DASH_MOVE_JUMP ? m : NULL;
}

static inline void vr_evade_reset(VrEvade *e) { memset(e, 0, sizeof(*e)); }

/* One call per frame of the player's update. `anim` and `f` are the ring's newest entry, `x`,`z`
 * the feet now, `dt` the frame's game time, `k` the clamped scale for rolls and quicksteps, `kb` the one for backsteps (0 = off). Returns 1 and the
 * extra to add to the feet in `dx`,`dz`; the state then expects the feet at x + dx, z + dz on the
 * next call (vr_evade_unwrite if the write did not happen). */
static inline int vr_evade_step(VrEvade *e, int32_t anim, const float f[3], float x, float z, float dt, float k, float kb,
                                float *dx, float *dz) {
    *dx = *dz = 0.0f;
    const VrDashMove *m = vr_evade_move(anim);
    if (m) {
        k = m->kind == VR_DASH_MOVE_BACKSTEP ? kb : k;
        if (!(k > 1.0f)) m = NULL;
    }
    if (!m) {
        vr_evade_reset(e);
        return 0;
    }
    int restart = !e->active || anim != e->anim;
    for (int i = 0; i < 3 && !restart; ++i)
        if (isfinite(e->f[i]) && isfinite(f[i]) && f[i] < e->f[i] - 0.1f) restart = 1;
    if (restart) {
        vr_evade_reset(e);
        e->active = 1;
        e->anim = anim;
        e->move = m;
    }
    memcpy(e->f, f, sizeof(e->f));
    if (dt > 0.0f && dt <= 0.2f) e->elapsed += dt;
    const int had = e->have_pos;
    const float sx = x - e->px, sz = z - e->pz;
    e->px = x;
    e->pz = z;
    e->have_pos = 1;
    if (!had || e->stopped) return 0;
    const float step = sqrtf(sx * sx + sz * sz);
    if (step > VR_EVADE_MAX_STEP) { e->stopped = 1; return 0; }
    if (dt > 0.0f && dt <= 0.2f) {
        const float speed = step / dt;
        if (e->last_speed > 0.5f && speed < (e->move->kind == VR_DASH_MOVE_BACKSTEP ? VR_EVADE_BACKSTEP_STOP : 0.5f) * e->last_speed) { e->stopped = 1; return 0; }
        e->last_speed = speed;
    }
    if (e->elapsed > e->move->motion + 0.05) { e->stopped = 1; return 0; }
    if (step < 1e-4f) return 0;
    float extra = (k - 1.0f) * step;
    const float room = (k - 1.0f) * e->move->distance - e->given;
    if (extra > room) extra = room;
    if (!(extra > 1e-5f)) return 0;
    e->given += extra;
    *dx = sx / step * extra;
    *dz = sz / step * extra;
    e->px = x + *dx;
    e->pz = z + *dz;
    return 1;
}

/* The write did not happen: the next step is measured from the feet as read. */
static inline void vr_evade_unwrite(VrEvade *e, float x, float z) { e->px = x; e->pz = z; }

#endif
