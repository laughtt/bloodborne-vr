/* VR throw animation speed-up (BB_VR_THROW_ANIM_SPEED, src/runtime_vr_atkspeed.c): the pure state machine.
 * The headset's throw gesture arms it (vta_begin); it then watches the player's newest TimeAct clip
 * (anim id, clip time) and, once a NEW clip starts (the id changes or the clip time restarts) that is
 * not a weapon move (30000-49999) or a dodge (7000-7999), it returns the dt multiplier until that clip
 * ends (id changes, clip restarts, or the clip time stops advancing). Drinking and other actions never
 * arm it, so they keep the game's speed. Header only, no game access. */
#ifndef VR_THROW_ANIM_H
#define VR_THROW_ANIM_H
#include <stdint.h>

#define VTA_WAIT_S 4.0      /* how long after the gesture a new clip may still start */
#define VTA_MAX_S 6.0       /* a latched clip is never scaled longer than this */
#define VTA_STALL_FRAMES 3  /* frames of unchanged clip time that end the clip */

typedef struct {
    int state;       /* 0 idle, 1 waiting for the throw clip, 2 scaling it */
    double begin, latched;
    int have_prev, anim, prev_anim, stall;
    float prev_clip;
} VtaState;

static inline void vta_begin(VtaState *s, double now) {
    s->state = 1;
    s->begin = now;
    s->have_prev = 0;
    s->stall = 0;
}

static inline int vta_excluded(int anim) {
    const int sub = anim >= 0 ? anim % 1000000 : -1;
    return sub < 0 || (sub >= 30000 && sub < 50000) || (sub >= 7000 && sub < 8000);
}

/* One game frame with a valid dt: returns the multiplier to apply (1 = none). */
static inline float vta_step(VtaState *s, double now, int anim, float clip, float mult) {
    if (s->state == 0) return 1.0f;
    const int restarted = s->have_prev && (anim != s->prev_anim || clip < s->prev_clip - 1e-3f);
    if (s->state == 1) {
        if (now - s->begin > VTA_WAIT_S) { s->state = 0; return 1.0f; }
        const int latch = restarted && !vta_excluded(anim);
        s->prev_anim = anim;
        s->prev_clip = clip;
        s->have_prev = 1;
        if (!latch) return 1.0f;
        s->state = 2;
        s->anim = anim;
        s->latched = now;
        s->stall = 0;
        return mult;
    }
    /* scaling */
    if (anim != s->anim || restarted || vta_excluded(anim) || now - s->latched > VTA_MAX_S) { s->state = 0; return 1.0f; }
    s->stall = clip <= s->prev_clip + 1e-6f ? s->stall + 1 : 0;
    s->prev_clip = clip;
    if (s->stall >= VTA_STALL_FRAMES) { s->state = 0; return 1.0f; }
    return mult;
}
#endif
