/* Hit-reaction animation speed (BB_VR_HURT_SPEED / hurt_speed= in out/fp.txt, src/runtime_vr_atkspeed.c,
 * docs/vr/findings/27-gun-aim.md section 9): the pure decision logic, no I/O, no globals.
 *
 * Request: "reduce stagger time to half from all hits". While the newest TimeAct clip of the player is a
 * damage reaction (flinch, stagger, knockback, knockdown, get-up) the character's dt is multiplied, like the
 * attack speed does, so the reaction lasts 1/k as long. Same guards: dt 0 (the hit stop that follows a hit)
 * and hitches (dt > 0.2) are left alone.
 *
 * WHICH CLIPS (sub-id = anim % 1000000): DEFAULT_FROM..DEFAULT_TO = 9000..12999. Evidence, and its limit:
 * findings/13 section 7.4 lists 9000-12999 as "likely damage" (a TAE-side guess, tag [L]); the DS3 head-tracking
 * rules in findings/11 name 10000-15000 as hurt clips (a sister engine; its 5000-5500 is a plain loop in
 * Bloodborne, a000_005000, so that part is NOT used); the play logs show a003_011100 and a003_012000 as the
 * clip right after a dodge ended, a003_008000 once. No log names a hit reaction directly, so the range is NOT
 * confirmed. Run with BB_VR_FP_LOG=1: "VR hurt speed: new clip ..." lines list every unclassified clip id the
 * player plays (sub-id 8000 and up, outside attacks, gun shots and dodges), so the real ids can be read off
 * the log after getting hit, and BB_VR_HURT_RANGE=<from>-<to> sets the range without a rebuild.
 * Excluded on purpose: 100000..104999 (events, gestures, and the likely home of the victim side of grabs and
 * visceral hits, which are timing-locked to the enemy's clip and could desync when scaled), 13000+ (deaths,
 * unconfirmed), attacks (30000-49999), gun shots (51000-52999), dodges (7000s).
 * OFF BY DEFAULT (VR_HURT_SPEED_DEFAULT = 1.0): the feature is postponed because the anim range above is
 * unconfirmed; the code, the keys and BB_VR_HURT_SPEED stay (BB_VR_HURT_SPEED=2 or hurt_speed=2 turns it on).
 * 1 (or less, or not finite) = off. Stays ON while online (co-op) by user decision; the opt-out is
 * BB_VR_HURT_SPEED_ONLINE=0 (findings/27 section 8 lists what a peer sees). C11, header only. */
#ifndef VR_HURT_SPEED_H
#define VR_HURT_SPEED_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VR_HURT_SPEED_DEFAULT 1.0f /* OFF by default until the anim range is confirmed; set BB_VR_HURT_SPEED=2 to try it */
#define VR_HURT_SPEED_MAX 4.0f
#define VR_HURT_ANIM_FROM 9000
#define VR_HURT_ANIM_TO 13000 /* exclusive */
#define VR_HURT_DT_MAX 0.2f

typedef struct { int from, to; } VrHurtRange; /* sub-ids from..to-1 */

static inline VrHurtRange vr_hurt_default_range(void) {
    VrHurtRange r = {VR_HURT_ANIM_FROM, VR_HURT_ANIM_TO};
    return r;
}

/* "<from>-<to>" (inclusive, both sub-ids 1..99999, from <= to) -> 1 and the range; 0 and *r untouched otherwise. */
static inline int vr_hurt_parse_range(const char *text, VrHurtRange *r) {
    int a = 0, b = 0, n = 0;
    if (!text || sscanf(text, "%d-%d%n", &a, &b, &n) < 2) return 0;
    if (a < 1 || b > 99999 || a > b) return 0;
    r->from = a;
    r->to = b + 1;
    return 1;
}

/* A multiplier from a number: <= 1 or not finite = off (0), else clamped to 1.05..VR_HURT_SPEED_MAX. */
static inline float vr_hurt_clamp(float k) {
    if (!isfinite(k) || !(k > 1.0f)) return 0.0f;
    return k < 1.05f ? 1.05f : k > VR_HURT_SPEED_MAX ? VR_HURT_SPEED_MAX : k;
}

/* "hurt_speed=<x>" at the start of a token in `text`; 1 and the raw value when present. */
static inline int vr_hurt_parse(const char *text, float *out) {
    static const char key[] = "hurt_speed=";
    const size_t n = sizeof(key) - 1;
    const char *p = text;
    while ((p = strstr(p, key)) != NULL) {
        if (p == text || p[-1] == ' ' || p[-1] == '\n' || p[-1] == '\r' || p[-1] == '\t') {
            const float x = strtof(p + n, NULL);
            if (isfinite(x)) { *out = x; return 1; }
        }
        p += n;
    }
    return 0;
}

/* Online play: `online` is BB_ONLINE's text, `role` BB_ONLINE_ROLE's. 1 when the game is in online mode. */
static inline int vr_hurt_online(const char *online, const char *role) {
    return (online && online[0] == '1') || (role && role[0] != 0);
}

/* 1 when the multiplier must stay off: online AND the opt-out BB_VR_HURT_SPEED_ONLINE=0 is set (`optout` is its
 * text). Online play keeps the multiplier by default. */
static inline int vr_hurt_online_off(const char *online, const char *role, const char *optout) {
    return vr_hurt_online(online, role) && optout && optout[0] == '0';
}

/* Is `anim` (group * 1000000 + sub-id) a damage-reaction clip in range `r`? */
static inline int vr_hurt_is_reaction(int anim, VrHurtRange r) {
    if (anim < 0) return 0;
    const int sub = anim % 1000000;
    return sub >= r.from && sub < r.to;
}

/* A clip the other speed-ups do not know and the range does not cover (sub-id 8000 up, not an attack
 * 30000-49999, not a gun shot 51000-52999): the discovery log lists these. */
static inline int vr_hurt_is_unclassified(int anim, VrHurtRange r) {
    if (anim < 0) return 0;
    const int sub = anim % 1000000;
    return sub >= 8000 && !vr_hurt_is_reaction(anim, r) && !(sub >= 30000 && sub < 53000);
}

/* The factor to apply to this frame's dt (1 = leave it): k is the clamped multiplier (0 = off). */
static inline float vr_hurt_factor(int anim, float dt, float k, VrHurtRange r) {
    if (!(k > 1.0f) || !vr_hurt_is_reaction(anim, r)) return 1.0f;
    if (!(dt > 0.0f) || dt > VR_HURT_DT_MAX) return 1.0f; /* the hit stop (0), a hitch or NaN */
    return k;
}
#endif
