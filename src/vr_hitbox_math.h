/* Pure math for the game's hitbox debug overlay (docs/vr/findings/29-game-hitboxes.md): where a
 * point of the player's model space (or of the game world) is in the headset's space, so the VR UI
 * layer (gpu/xr_bridge/vr_game_hitbox.*) can draw the sandbox's hit/hurt capsules over the game.
 * No I/O, no globals: src/runtime_vr_hitbox.c uses it at the pad poll, tests/test_vr_hitbox.c
 * checks it.
 *
 * The way there is the exact inverse of the arms' controller placement (vr_arm_math.h
 * vam_grip_to_model): the first-person camera in model space (vam_camera_in_model) and the head
 * pose it showed (OpenXR, the space of bbgpu_vr_views):
 *     xr = head_pos + head_rot . Z( conj(cam_rot) . (p_model - cam_pos) )
 * with Z the XR <-> game-camera mirror of vr_head_math.h (x, y, -z).
 *
 * The bbhitbox data (out/vr_assets/hitbox/ JSON files, scripts/vr_assets/hitbox_extract.py) is in
 * FromSoftware's model space with X mirrored (right-handed); a data point j is the game's X(j)
 * = (-x, y, z). vhb_affine folds that mirror, a bone's transform and the way to the headset into
 * one 3x4 matrix the bridge applies to the data's points as they are (the two mirrors make it a
 * proper rotation, det +1).
 */
#ifndef VR_HITBOX_MATH_H
#define VR_HITBOX_MATH_H
#include "vr_fp_math.h"
#include <stdint.h>

/* One first-person frame: the camera in the character's model space, the head pose it showed and
 * the character's world position and yaw (vr_model_to_world). */
typedef struct {
    VrVec3 cam_pos;
    VrQuat cam_rot;
    VrVec3 head_pos;
    VrQuat head_rot;
    VrVec3 body_pos;
    float body_yaw;
} VhbView;

/* FNV-1a 32 of a bone name (the bridge hashes the data's names the same way). */
static inline uint32_t vhb_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; s && *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static inline VrVec3 vhb_mirror_x(VrVec3 v) { return vr_v3(-v.x, v.y, v.z); }

/* A model-space point (the game's own axes) in the headset's space. */
static inline VrVec3 vhb_model_to_xr(const VhbView *v, VrVec3 p) {
    const VrVec3 c = vr_qrotate(vr_qconj(v->cam_rot), vr_sub(p, v->cam_pos));
    return vr_add(v->head_pos, vr_qrotate(v->head_rot, vr_xr_to_game_vec(c)));
}
/* A model-space direction (no translation). */
static inline VrVec3 vhb_model_dir_to_xr(const VhbView *v, VrVec3 d) {
    return vr_qrotate(v->head_rot, vr_xr_to_game_vec(vr_qrotate(vr_qconj(v->cam_rot), d)));
}
/* A game-world point in the headset's space (through the character's model space). */
static inline VrVec3 vhb_world_to_xr(const VhbView *v, VrVec3 w) {
    return vhb_model_to_xr(v, vr_world_to_model(v->body_pos, v->body_yaw, w));
}

/* The 3x4 matrix (column-major: x, y, z columns, then the origin) taking a bbhitbox data point j
 * (X-mirrored) through the model-space transform x into the headset's space:
 *     out(j) = vhb_model_to_xr(x.t + x.q . X(j)). */
static inline void vhb_affine(const VhbView *v, VrXform x, float out[12]) {
    const VrVec3 axes[3] = {vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1)};
    for (int i = 0; i < 3; ++i) {
        const VrVec3 c = vhb_model_dir_to_xr(v, vr_qrotate(x.q, vhb_mirror_x(axes[i])));
        out[i * 3 + 0] = c.x;
        out[i * 3 + 1] = c.y;
        out[i * 3 + 2] = c.z;
    }
    const VrVec3 o = vhb_model_to_xr(v, x.t);
    out[9] = o.x;
    out[10] = o.y;
    out[11] = o.z;
}
/* The skinning transform of a bone: its pose now times the inverse of its bind (reference) pose,
 * for points given in the bind pose's model space (the hurtboxes' a/b, the body hit points). */
static inline VrXform vhb_skin(VrXform now, VrXform bind) { return vr_xf_mul(now, vr_xf_inverse(bind)); }

/* Applies a vhb_affine matrix. */
static inline VrVec3 vhb_apply(const float m[12], VrVec3 p) {
    return vr_v3(m[0] * p.x + m[3] * p.y + m[6] * p.z + m[9], m[1] * p.x + m[4] * p.y + m[7] * p.z + m[10],
                 m[2] * p.x + m[5] * p.y + m[8] * p.z + m[11]);
}
static inline float vhb_det(const float m[12]) {
    return m[0] * (m[4] * m[8] - m[7] * m[5]) - m[3] * (m[1] * m[8] - m[7] * m[2]) + m[6] * (m[1] * m[5] - m[4] * m[2]);
}
static inline int vhb_finite(const float *f, int n) {
    for (int i = 0; i < n; ++i)
        if (!isfinite(f[i]) || fabsf(f[i]) > 1e5f) return 0;
    return 1;
}

#endif
