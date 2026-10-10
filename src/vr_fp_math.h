/* Pure math for the first-person camera (docs/vr/findings/16-first-person.md, 11 sections 3-4).
 * No I/O, no globals: src/runtime_vr_fp.c uses it from the camera hooks and the pad sampler,
 * tests/test_vr_head.c checks it. Builds on vr_head_math.h (types, quaternions, conventions).
 * Turning (snap, smooth, the move heading, recenter, lock-on) is src/vr_turn_math.h, shared with
 * the VR sandbox.
 *
 * Conventions (findings/08, 13, 15, 16):
 *  - Game world: +Y up, left-handed; a heading (yaw) is radians about +Y with 0 looking along
 *    world +Z and positive toward +X (vr_cam_heading), so forward = (sin h, 0, cos h).
 *  - Havok poses (hkaPose, hkQsTransform: translation xyzw, rotation xyzw, scale xyzw) are in the
 *    character's model space: +Y up, the face toward model -Z (the eye dummy 58 is at z -0.066),
 *    model +X the character's left; the same handedness as the world.
 *  - model -> world: world = position + R(yaw) . model, R(yaw) the rotation that turns world +Z
 *    to heading `yaw`, with the physics module's yaw (+0x1D4). Measured (findings/16 section 2):
 *    running straight, the movement heading is yaw + 180 deg, i.e. the yaw is the heading of
 *    model +Z (the back) and the face looks along yaw + pi.
 */
#ifndef VR_FP_MATH_H
#define VR_FP_MATH_H
#include "vr_head_math.h"
#include <stdint.h>

/* A rigid transform (Havok hkQsTransform without the scale, which is 1 for the player). */
typedef struct { VrVec3 t; VrQuat q; } VrXform;

static inline VrXform vr_xf(VrVec3 t, VrQuat q) { VrXform x = {t, q}; return x; }
static inline VrXform vr_xf_identity(void) { return vr_xf(vr_v3(0, 0, 0), vr_qidentity()); }
/* parent . local (Havok model[i] = model[parent] . local[i]). */
static inline VrXform vr_xf_mul(VrXform parent, VrXform local) {
    return vr_xf(vr_add(parent.t, vr_qrotate(parent.q, local.t)), vr_qnormalize(vr_qmul(parent.q, local.q)));
}
static inline VrVec3 vr_xf_apply(VrXform x, VrVec3 p) { return vr_add(x.t, vr_qrotate(x.q, p)); }
static inline VrXform vr_xf_inverse(VrXform x) {
    const VrQuat qi = vr_qconj(x.q);
    return vr_xf(vr_scale(vr_qrotate(qi, x.t), -1.0f), qi);
}
/* An hkQsTransform as 12 floats (t xyzw, q xyzw, s xyzw); 0 when it is not finite or the
 * rotation is not a unit quaternion (a torn or garbage read). */
static inline int vr_xf_from_qs(const float *qs, VrXform *out) {
    for (int i = 0; i < 8; ++i) if (!isfinite(qs[i])) return 0;
    const VrQuat q = vr_q(qs[4], qs[5], qs[6], qs[7]);
    if (!vr_qvalid(q)) return 0;
    if (fabsf(qs[0]) > 1e4f || fabsf(qs[1]) > 1e4f || fabsf(qs[2]) > 1e4f) return 0;
    *out = vr_xf(vr_v3(qs[0], qs[1], qs[2]), vr_qnormalize(q));
    return 1;
}

/* ---- character model space <-> world ---- */

/* The character's model-space point in the world: position + R(yaw) . p, where R(yaw) turns
 * model +Z to the world heading `yaw` (sin yaw, 0, cos yaw) and model +X to (cos yaw, 0, -sin yaw).
 * `yaw` is the physics module's (+0x1D4); the face (model -Z) then looks along yaw + pi. */
static inline VrVec3 vr_model_to_world(VrVec3 position, float yaw, VrVec3 p) {
    const float s = sinf(yaw), c = cosf(yaw);
    return vr_v3(position.x + c * p.x + s * p.z, position.y + p.y, position.z - s * p.x + c * p.z);
}
/* Inverse of vr_model_to_world. */
static inline VrVec3 vr_world_to_model(VrVec3 position, float yaw, VrVec3 w) {
    const float s = sinf(yaw), c = cosf(yaw);
    const VrVec3 d = vr_sub(w, position);
    return vr_v3(c * d.x - s * d.z, d.y, s * d.x + c * d.z);
}
/* The heading the character faces (its face, model -Z) for the physics yaw. */
static inline float vr_facing_heading(float yaw) {
    const float h = yaw + VR_PI;
    return h > VR_PI ? h - 2.0f * VR_PI : h;
}

/* ---- headings ---- */

static inline float vr_wrap_pi(float a) {
    if (!isfinite(a)) return 0.0f;
    a = fmodf(a + VR_PI, 2.0f * VR_PI);
    if (a < 0.0f) a += 2.0f * VR_PI;
    return a - VR_PI;
}
/* face_dodge=strafe: one root-motion step (dx, dz) re-aimed along heading `travel` (direction (sin h, cos h)).
 * Only the step's length is kept: the game turns the body toward the stick during a roll, so the step's own
 * direction (a back roll: ~180 deg from our yaw) says nothing reliable about where it should go. */
static inline void vr_strafe_step(float dx, float dz, float travel, float *ox, float *oz) {
    const float len = sqrtf(dx * dx + dz * dz);
    *ox = len * sinf(travel);
    *oz = len * cosf(travel);
}
/* A camera matrix (rows right, up, forward, position) for heading h, level, at position p, with
 * the game cameras' handedness (det(right, up, forward) = +1). */
static inline void vr_cam_from_heading(float m[4][4], float h, VrVec3 p) {
    const float s = sinf(h), c = cosf(h);
    const float rows[4][4] = {{c, 0.0f, -s, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {s, 0.0f, c, 0.0f},
                              {p.x, p.y, p.z, 1.0f}};
    memcpy(m, rows, sizeof(rows));
}
/* Turns a camera matrix (rows right, up, forward, position) about the vertical through `pivot` by
 * `d` radians of heading (positive to the right): its heading grows by d, its pitch and roll stay,
 * its position orbits the pivot at the same distance and height. */
static inline void vr_cam_rotate_heading(float m[4][4], float d, VrVec3 pivot) {
    const float s = sinf(d), c = cosf(d);
    for (int i = 0; i < 3; ++i) {
        const float x = m[i][0], z = m[i][2];
        m[i][0] = x * c + z * s;
        m[i][2] = -x * s + z * c;
    }
    const float x = m[3][0] - pivot.x, z = m[3][2] - pivot.z;
    m[3][0] = pivot.x + x * c + z * s;
    m[3][2] = pivot.z - x * s + z * c;
}
/* The heading of where a head rotation `rel` (vr_head_rel_game, the game camera's local frame)
 * looks: its view axis flattened onto the floor, positive to the right; looking straight down or
 * up, the up vector gives it. Roll does not change it. */
static inline float vr_rel_heading(VrQuat rel) {
    const VrVec3 f = vr_qrotate(rel, vr_v3(0, 0, 1));
    if (f.x * f.x + f.z * f.z > 1e-6f) return atan2f(f.x, f.z);
    const VrVec3 u = vr_qrotate(rel, vr_v3(0, 1, 0));
    return f.y < 0.0f ? atan2f(u.x, u.z) : atan2f(-u.x, -u.z);
}

/* ---- the eye anchor (findings/11 section 3.2, A1 "stabilized head") ---- */

typedef struct {
    int valid;
    VrVec3 value;   /* model space, relative to the character's root */
} VrSmooth;
/* Exponential smoothing with time constant tau (seconds) of a model-space point, then the result
 * is kept within max_dev meters of the raw value. tau <= 0 or the first sample: no smoothing. */
static inline VrVec3 vr_smooth_step(VrSmooth *s, VrVec3 raw, float dt, float tau, float max_dev) {
    if (!s->valid || !(tau > 0.0f) || !(dt > 0.0f) || dt > 0.5f) {
        s->valid = 1;
        s->value = raw;
        return raw;
    }
    const float a = 1.0f - expf(-dt / tau);
    VrVec3 v = vr_add(s->value, vr_scale(vr_sub(raw, s->value), a));
    const VrVec3 d = vr_sub(v, raw);
    const float n = sqrtf(vr_dot(d, d));
    if (n > max_dev && n > 0.0f) v = vr_add(raw, vr_scale(d, max_dev / n));
    s->value = v;
    return v;
}

/* ---- the left stick remapped to another heading (findings/11 section 3.3 fallback) ---- */

/* PS4 stick bytes (128 = centre, Y down positive) rotated so that "stick up", which the game moves
 * along its camera's heading, moves along a heading `angle` radians to the right of it. */
static inline void vr_stick_rotate(uint8_t *sx, uint8_t *sy, float angle) {
    const float x = (float)*sx - 128.0f, y = 128.0f - (float)*sy; /* up positive */
    const float c = cosf(angle), s = sinf(angle);
    const float rx = x * c + y * s, ry = -x * s + y * c;
    const float ox = vr_clampf(128.0f + rx, 0.0f, 255.0f), oy = vr_clampf(128.0f - ry, 0.0f, 255.0f);
    *sx = (uint8_t)(ox + 0.5f);
    *sy = (uint8_t)(oy + 0.5f);
}

#endif
