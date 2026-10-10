/* Tunable weapon hit shapes for VR, shared by the game (src/runtime_vr_hitbox.c, the
 * "VR Hitbox Scale" hook) and the 3D sandbox (gpu/xr_bridge/sandbox3d/hit_scale.h, the weapon
 * physics and the wireframe), docs/vr/findings/32-vr-hitbox-scale.md.
 * Pure logic: static inline, no I/O, no globals; compiles as C (C11) and as C++. Tests:
 * tests/test_vr_hit_scale.c (ninja -C out/gpu vr-hit-scale-test vr-hit-scale-test-cpp) and the
 * sandbox collision tests.
 *
 * Why: in VR you stand much closer to the enemy than the flat game's camera puts you, and a swing
 * that ends 5 cm short of the target is a miss that the animation never had. Two knobs make the
 * game's attack capsules more forgiving:
 *   radius_scale  multiplies every capsule/sphere radius (default 1.5);
 *   reach_m       extends the shapes that reach the weapon's tip by this much along the weapon axis,
 *                 away from the grip (default 0.15 m). A sphere (a hammer head) becomes a capsule
 *                 from its centre outward.
 * Per weapon class (by how far the weapon reaches from the grip: short / medium / long) either
 * can be overridden; a negative override means "use the global value".
 *
 * Fixed: a shape's far end is judged against the grip point (the weapon bone's origin); a shape
 * gets the reach only when its far end is at least 60% as far from the grip as the weapon's farthest one (the blade and
 * tip shapes, not a pommel/root shape); the final radius is capped at VHS_RADIUS_MAX_M.
 *
 * Keys of the game's control file (out/fp.txt, BB_VR_FP_CONTROL, polled like the arm* keys):
 *   hit_scale=1.5 hit_reach=0.15     global
 *   hit_scale_s/m/l=<x|-1>           per class (short < 0.7 m, medium < 1.1 m, long)
 *   hit_reach_s/m/l=<m|-2>           (below -0.3 = use the global one; for the scale any negative)
 *   hit=0                            off (identity: radius x1, reach 0)
 */
#ifndef VR_HIT_SCALE_H
#define VR_HIT_SCALE_H
#include "vr_head_math.h"
#include <stdlib.h>
#include <string.h>

#define VHS_CLASSES 3
#define VHS_SHORT 0
#define VHS_MEDIUM 1
#define VHS_LONG 2
#define VHS_MEDIUM_FROM_M 0.7f
#define VHS_LONG_FROM_M 1.1f
#define VHS_TIP_SHARE 0.6f /* a shape gets the reach when its far end >= this x the weapon's reach */
#define VHS_SCALE_MIN 0.25f
#define VHS_SCALE_MAX 4.0f
#define VHS_REACH_MIN_M -0.3f
#define VHS_REACH_MAX_M 1.0f
#define VHS_RADIUS_MAX_M 1.2f

typedef struct {
    int enabled;
    float radius_scale;                   /* global */
    float reach_m;                        /* global */
    float class_radius_scale[VHS_CLASSES]; /* < 0: the global one */
    float class_reach_m[VHS_CLASSES];      /* < VHS_REACH_MIN_M (-2): the global one (a reach may be negative) */
} VhsParams;

/* One shape after scaling: a capsule a..b of radius r (a == b: a sphere). */
typedef struct {
    VrVec3 a, b;
    float r;
} VhsShape;

static inline float vhs_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The VR defaults: radius x1.5, +0.15 m (see the header comment). */
static inline VhsParams vhs_defaults(void) {
    VhsParams p;
    p.enabled = 1;
    p.radius_scale = 1.5f;
    p.reach_m = 0.15f;
    for (int i = 0; i < VHS_CLASSES; ++i) {
        p.class_radius_scale[i] = -1.0f;
        p.class_reach_m[i] = -2.0f;
    }
    return p;
}
/* No change at all. */
static inline VhsParams vhs_identity(void) {
    VhsParams p = vhs_defaults();
    p.enabled = 0;
    p.radius_scale = 1.0f;
    p.reach_m = 0.0f;
    return p;
}
/* Pulls every value into its allowed range (call after loading). */
static inline void vhs_sanitize(VhsParams *p) {
    p->radius_scale = vhs_clamp(isfinite(p->radius_scale) ? p->radius_scale : 1.0f, VHS_SCALE_MIN, VHS_SCALE_MAX);
    p->reach_m = vhs_clamp(isfinite(p->reach_m) ? p->reach_m : 0.0f, VHS_REACH_MIN_M, VHS_REACH_MAX_M);
    for (int i = 0; i < VHS_CLASSES; ++i) {
        float s = p->class_radius_scale[i], r = p->class_reach_m[i];
        p->class_radius_scale[i] = !isfinite(s) || s < 0.0f ? -1.0f : vhs_clamp(s, VHS_SCALE_MIN, VHS_SCALE_MAX);
        p->class_reach_m[i] = !isfinite(r) || r < VHS_REACH_MIN_M ? -2.0f : vhs_clamp(r, VHS_REACH_MIN_M, VHS_REACH_MAX_M);
    }
}

/* The weapon class from how far its shapes reach from the grip. */
static inline int vhs_class_from_reach(float weapon_reach_m) {
    return weapon_reach_m >= VHS_LONG_FROM_M ? VHS_LONG : weapon_reach_m >= VHS_MEDIUM_FROM_M ? VHS_MEDIUM : VHS_SHORT;
}
static inline float vhs_radius_scale_for(const VhsParams *p, int cls) {
    if (!p->enabled) return 1.0f;
    const float c = cls >= 0 && cls < VHS_CLASSES ? p->class_radius_scale[cls] : -1.0f;
    return c >= 0.0f ? c : p->radius_scale;
}
static inline float vhs_reach_for(const VhsParams *p, int cls) {
    if (!p->enabled) return 0.0f;
    const float c = cls >= 0 && cls < VHS_CLASSES ? p->class_reach_m[cls] : -2.0f;
    return c >= VHS_REACH_MIN_M ? c : p->reach_m;
}

/* The distance from the grip to the shape's far end (the farther of a and b, plus nothing: the
 * centre line, not the radius). Used to find the weapon's reach: the maximum over its shapes. */
static inline float vhs_far_end(VrVec3 a, VrVec3 b, VrVec3 grip) {
    const VrVec3 da = vr_sub(a, grip), db = vr_sub(b, grip);
    const float la = sqrtf(vr_dot(da, da)), lb = sqrtf(vr_dot(db, db));
    return la > lb ? la : lb;
}

/* Scales one shape. `weapon_reach_m` is the maximum vhs_far_end over the weapon's shapes (so every
 * shape of a weapon gets the same class); `reach_ok` = 0 skips the reach (a kick on a body bone). */
static inline VhsShape vhs_apply(const VhsParams *p, VrVec3 a, VrVec3 b, VrVec3 grip, float r, float weapon_reach_m,
                                 int reach_ok) {
    VhsShape out;
    out.a = a;
    out.b = b;
    out.r = r;
    if (!p->enabled) return out;
    const int cls = vhs_class_from_reach(weapon_reach_m);
    out.r = vhs_clamp(r * vhs_radius_scale_for(p, cls), 0.0f, VHS_RADIUS_MAX_M);
    const float reach = vhs_reach_for(p, cls);
    if (!reach_ok || reach == 0.0f) return out;
    const float far_end = vhs_far_end(a, b, grip);
    if (weapon_reach_m > 1e-4f && far_end < weapon_reach_m * VHS_TIP_SHARE) return out; /* a root shape */
    const VrVec3 ab = vr_sub(b, a);
    const float len = sqrtf(vr_dot(ab, ab));
    const VrVec3 da = vr_sub(a, grip), db = vr_sub(b, grip);
    const int b_is_far = vr_dot(db, db) >= vr_dot(da, da);
    VrVec3 tip = b_is_far ? b : a, other = b_is_far ? a : b, dir;
    if (len > 1e-5f) {
        dir = vr_scale(b_is_far ? ab : vr_scale(ab, -1.0f), 1.0f / len); /* from the near end to the far end */
    } else { /* a sphere: outward from the grip */
        const VrVec3 d = vr_sub(a, grip);
        const float dl = sqrtf(vr_dot(d, d));
        if (dl < 1e-5f) return out;
        dir = vr_scale(d, 1.0f / dl);
        other = a;
    }
    tip = vr_add(tip, vr_scale(dir, reach));
    if (len > 1e-5f) {
        /* a negative reach must not turn the capsule inside out: keep at least a sphere */
        const VrVec3 rel = vr_sub(tip, other);
        if (vr_dot(rel, dir) < 0.0f) tip = other;
    } else if (reach < 0.0f) {
        tip = other;
    }
    if (b_is_far) {
        out.a = other;
        out.b = tip;
    } else {
        out.a = tip;
        out.b = other;
    }
    return out;
}

/* ---- the game's own capsule (the hook "VR Hitbox Scale (attack shape)", findings/32 4) ----
 * The attack update 0x1c5ed50 rebuilds each capsule every frame in a local frame whose origin is the
 * DmyPoly1 point p1 and whose +Z runs to the DmyPoly2 point p2: ends A = (0,0,a_z), B = (0,0,b_z) with
 * a_z = 0, b_z = |p2 - p1|. vhs_apply_axis scales it in the world (vhs_apply) and maps both ends back
 * onto that axis; vhs_apply only moves a capsule's ends along its own axis, so this is exact. */
typedef struct {
    float a_z, b_z, r;
    int axis_ok; /* 0: degenerate (p1 == p2): only the radius changed, a_z/b_z as given */
} VhsAxisShape;

static inline VhsAxisShape vhs_apply_axis(const VhsParams *p, VrVec3 p1, VrVec3 p2, float a_z, float b_z, float r,
                                          VrVec3 grip, float weapon_reach_m, int reach_ok) {
    VhsAxisShape out;
    const VrVec3 d = vr_sub(p2, p1);
    const float len = sqrtf(vr_dot(d, d));
    out.a_z = a_z;
    out.b_z = b_z;
    out.axis_ok = len > 1e-4f;
    if (!out.axis_ok) reach_ok = 0; /* a sphere's local axis is not the grip's: keep its ends */
    const VhsShape s = vhs_apply(p, p1, p2, grip, r, weapon_reach_m, reach_ok);
    out.r = s.r;
    if (out.axis_ok) {
        const VrVec3 dir = vr_scale(d, 1.0f / len);
        out.a_z = a_z + vr_dot(vr_sub(s.a, p1), dir);
        out.b_z = b_z + (vr_dot(vr_sub(s.b, p1), dir) - len);
    }
    return out;
}

/* The distance from point q to the segment a..b. */
static inline float vhs_seg_dist(VrVec3 q, VrVec3 a, VrVec3 b) {
    const VrVec3 ab = vr_sub(b, a), aq = vr_sub(q, a);
    const float l2 = vr_dot(ab, ab);
    float t = l2 > 1e-12f ? vr_dot(aq, ab) / l2 : 0.0f;
    t = vhs_clamp(t, 0.0f, 1.0f);
    const VrVec3 c = vr_sub(aq, vr_scale(ab, t));
    return sqrtf(vr_dot(c, c));
}

/* The game sees one attack's shapes one at a time, so the weapon's reach (the class and the 60% tip
 * rule) is the running maximum of vhs_far_end over the shapes of the current attack (`key`, the
 * AtkParam id); a new key or `gap_s` without a shape starts over. Returns the reach to use. */
typedef struct {
    int key;
    double last_s;
    float reach_m;
} VhsReachTrack;
static inline float vhs_track_reach(VhsReachTrack *t, int key, double now_s, double gap_s, float far_end) {
    if (t->key != key || now_s - t->last_s > gap_s || now_s < t->last_s) {
        t->key = key;
        t->reach_m = 0.0f;
    }
    t->last_s = now_s;
    if (far_end > t->reach_m) t->reach_m = far_end;
    return t->reach_m;
}

/* ---- control-file keys (the game's out/fp.txt; the sandbox reads JSON instead) ---- */

/* Applies one "key=value" pair; returns 1 when the key was a hit_* one (valid or not). */
static inline int vhs_apply_key(VhsParams *p, const char *k, const char *v) {
    if (strncmp(k, "hit", 3) != 0) return 0;
    char *end;
    const float x = strtof(v, &end);
    const int ok = end != v && isfinite(x);
    int cls = -1;
    const size_t n = strlen(k);
    if (n > 2 && k[n - 2] == '_') cls = k[n - 1] == 's' ? VHS_SHORT : k[n - 1] == 'm' ? VHS_MEDIUM : k[n - 1] == 'l' ? VHS_LONG : -1;
    if (!strcmp(k, "hit")) {
        if (ok) p->enabled = x != 0.0f;
    } else if (!strcmp(k, "hit_scale")) {
        if (ok) p->radius_scale = x;
    } else if (!strcmp(k, "hit_reach")) {
        if (ok) p->reach_m = x;
    } else if (cls >= 0 && !strncmp(k, "hit_scale_", 10)) {
        if (ok) p->class_radius_scale[cls] = x;
    } else if (cls >= 0 && !strncmp(k, "hit_reach_", 10)) {
        if (ok) p->class_reach_m[cls] = x;
    } else {
        return 0;
    }
    vhs_sanitize(p);
    return 1;
}

/* Applies every "key=value" token (space/tab/newline separated) of a control file's text; other keys
 * are ignored. Returns the number of hit* keys seen. */
static inline int vhs_apply_text(VhsParams *p, const char *text) {
    int n = 0;
    const char *s = text;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
        const char *e = s;
        while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') ++e;
        const char *eq = s;
        while (eq < e && *eq != '=') ++eq;
        if (eq < e && eq > s && (size_t)(eq - s) < 32 && (size_t)(e - eq) < 32) {
            char k[32], v[32];
            memcpy(k, s, (size_t)(eq - s));
            k[eq - s] = 0;
            memcpy(v, eq + 1, (size_t)(e - eq - 1));
            v[e - eq - 1] = 0;
            n += vhs_apply_key(p, k, v);
        }
        s = e;
    }
    return n;
}

#endif
