/* Gun-shot animation speed (BB_VR_GUN_SPEED / gun_speed= in out/fp.txt, src/runtime_vr_atkspeed.c,
 * docs/vr/findings/27-gun-aim.md section 8): the pure decision logic, no I/O, no globals.
 *
 * Headset feedback: "the gun feels a little bit delayed". The delay is the shot clip's wind-up before the
 * TAE bullet event and its recovery after it. While the newest TimeAct clip of the player is a gun shot
 * (sub-id = anim % 1000000 in 51000..52999, the "gun groups" of findings/13 section 7.4: bullet events at
 * 0.33 s for 51000, 0.37 s for 52150, 0.17 s for 52550; a102_051000 and a102_051003 were seen in real play
 * logs) the character's dt is multiplied, like the attack speed does. The bullet event is a window crossed
 * by the clip time, so it still fires exactly once at the faster rate, and the gun aim hook (bullet spawn)
 * is untouched. Same guards as the attacks: a stop (dt 0) and a hitch (dt > 0.2) are left alone.
 * Default 2.0; 1 (or less, or not finite) = off. Stays ON while online (co-op) by user decision; the opt-out is
 * BB_VR_GUN_SPEED_ONLINE=0 (findings/27 section 8 lists what a peer sees). C11, header only. */
#ifndef VR_GUN_SPEED_H
#define VR_GUN_SPEED_H
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VR_GUN_SPEED_DEFAULT 2.0f
#define VR_GUN_SPEED_MAX 4.0f
#define VR_GUN_ANIM_FROM 51000
#define VR_GUN_ANIM_TO 53000 /* exclusive */
#define VR_GUN_DT_MAX 0.2f

/* A multiplier from a number: <= 1 or not finite = off (0), else clamped to 1.05..VR_GUN_SPEED_MAX. */
static inline float vr_gun_clamp(float k) {
    if (!isfinite(k) || !(k > 1.0f)) return 0.0f;
    return k < 1.05f ? 1.05f : k > VR_GUN_SPEED_MAX ? VR_GUN_SPEED_MAX : k;
}

/* "gun_speed=<x>" at the start of a token in `text`; 1 and the raw value when present. */
static inline int vr_gun_parse(const char *text, float *out) {
    static const char key[] = "gun_speed=";
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
static inline int vr_gun_online(const char *online, const char *role) {
    return (online && online[0] == '1') || (role && role[0] != 0);
}

/* 1 when the multiplier must stay off: online AND the opt-out BB_VR_GUN_SPEED_ONLINE=0 is set (`optout` is its
 * text). Online play keeps the multiplier by default. */
static inline int vr_gun_online_off(const char *online, const char *role, const char *optout) {
    return vr_gun_online(online, role) && optout && optout[0] == '0';
}

/* Is `anim` (a TimeAct ring id, a group * 1000000 + sub-id) a gun-shot clip? */
static inline int vr_gun_is_shot(int anim) {
    if (anim < 0) return 0;
    const int sub = anim % 1000000;
    return sub >= VR_GUN_ANIM_FROM && sub < VR_GUN_ANIM_TO;
}

/* The factor to apply to this frame's dt (1 = leave it): k is the clamped multiplier (0 = off). */
static inline float vr_gun_factor(int anim, float dt, float k) {
    if (!(k > 1.0f) || !vr_gun_is_shot(anim)) return 1.0f;
    if (!(dt > 0.0f) || dt > VR_GUN_DT_MAX) return 1.0f; /* a stop (0), a hitch or NaN */
    return k;
}
#endif
