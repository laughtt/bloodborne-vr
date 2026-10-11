/* Free co-op bells (BB_ONLINE_FREE_BELLS=1, src/runtime_vr_ui.c): the bells cost no Insight. C11, header only, no I/O.
 *
 * Why not a param edit: EquipParamGoods has consumeHeroPoint (+0x38) = 1 only on row 200 (Beckoning Bell); rows 205 and
 * 225 hold 0 and SpEffects 9000/9005/9025 have heroPointDamage 0, yet a live ring of 205 took Insight 100 -> 99. So the
 * cost is in code. The game thread watches the character instead:
 *   - a bell press ARMS a window (vfb_arm): our own presses (auto-bell / BB_DEBUG_USE), a dashboard use of a bell, the
 *     player's square press while a bell is the selected quick item; the pending use-item id (PlayerIns +0x394,
 *     which often never changes) is only an extra signal. A drop of Insight by exactly 1 inside the window is put back.
 *   - the bell is refused when Insight <= 0 (the predicate 0x1731d00), so while armed (or with floor_always, set in a
 *     room) a 0 is raised to 1.
 * vfb_step() is fed the Insight and the pending id of each game-thread call and returns the value to write (or -1). */
#ifndef VR_FREE_BELLS_H
#define VR_FREE_BELLS_H

#include <stdint.h>

#define VFB_WINDOW_S 8.0 /* after an armed bell press, a drop of Insight is refunded for this long */
#define VFB_DROP 1       /* exactly this drop is the bell; any other is not (a shop purchase, a death...) */

typedef struct {
    int have_prev;
    int32_t prev;      /* Insight at the last call, with refunds applied */
    double window_end; /* armed until this time (0: closed) */
    int floor_always;  /* raise Insight 0 to 1 even when no press is armed (rooms) */
    int refunds, floors;
} VfbState;

static inline int vfb_is_bell(uint32_t id) {
    return id == 0x400000C8u || id == 0x400000CDu || id == 0x400000E1u; /* goods 200, 205, 225 */
}

/* A bell press starts now (or is about to). */
static inline void vfb_arm(VfbState *s, double now) { s->window_end = now + VFB_WINDOW_S; }

/* insight < 0: not readable. Returns the Insight to write, or -1 for no change. */
static inline int32_t vfb_step(VfbState *s, double now, int32_t insight, uint32_t pending_id) {
    if (insight < 0) {
        s->have_prev = 0;
        return -1;
    }
    if (vfb_is_bell(pending_id)) vfb_arm(s, now);
    const int armed = now < s->window_end;
    int32_t out = -1;
    if (s->have_prev && armed && s->prev - insight == VFB_DROP) {
        out = s->prev; /* the bell's cost, given back */
        s->refunds++;
    } else if (insight == 0 && (armed || s->floor_always)) {
        out = 1; /* the bell test refuses at 0 */
        s->floors++;
    }
    s->prev = out >= 0 ? out : insight;
    s->have_prev = 1;
    return out;
}

#endif
