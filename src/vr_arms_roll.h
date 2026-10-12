/* Arms detached from the body while it evades (BB_VR_ARMS_ROLL_DETACH, arm_roll_detach=, src/runtime_vr_arms.c,
 * docs/vr/findings/24-arms-ik.md section 8): the pure part, no I/O, no globals (tests/test_vr_arms.c).
 *
 * A roll, quickstep or backstep tumbles the animated spine and shoulders. The hands are placed from
 * the controllers relative to the camera, but the arm chain is rooted at the animated shoulder, so
 * the target and the root part: the arm stretches, clamps and the sleeves and the weapon jump. While
 * an evade clip plays the shoulder (and the clavicle with what hangs off it) is therefore taken from
 * a pose captured in the CAMERA frame on the last frame before the evade, so the whole arm chain is
 * solved in camera space like the hands. In and out over a short blend. C11, header only. */
#ifndef VR_ARMS_ROLL_H
#define VR_ARMS_ROLL_H
#include <math.h>
#include "vr_fp_math.h"
#include "vr_head_math.h"

#define VR_ROLL_BLEND_S 0.1f

typedef struct {
    float e;            /* detach amount 0..1 */
    int has[2];         /* a pin is captured for this side */
    VrXform shoulder[2]; /* the shoulder (turned, shifted), in the camera frame */
    VrXform clav[2];     /* the clavicle, in the camera frame */
} VrArmsRoll;

static inline void vr_roll_reset(VrArmsRoll *r) {
    r->e = 0.0f;
    r->has[0] = r->has[1] = 0;
}

/* One step: `evading` = an evade clip plays (and the setting is on); `blend_s` the time for 0 -> 1
 * (0 = at once). Returns e. */
static inline float vr_roll_step(VrArmsRoll *r, int evading, float dt, float blend_s) {
    const float target = evading ? 1.0f : 0.0f;
    if (!(blend_s > 0.0f)) r->e = target;
    else if (dt > 0.0f) {
        const float d = dt / blend_s;
        r->e = target > r->e ? (r->e + d < target ? r->e + d : target) : (r->e - d > target ? r->e - d : target);
    }
    if (r->e < 0.0f) r->e = 0.0f;
    if (r->e > 1.0f) r->e = 1.0f;
    return r->e;
}

/* Whether pins may be (re)captured now: not evading and fully attached. */
static inline int vr_roll_capturing(const VrArmsRoll *r, int evading) { return !evading && r->e <= 0.0f; }

/* A model-space transform into the camera frame and back. */
static inline VrXform vr_roll_to_cam(VrVec3 cam_pos, VrQuat cam_rot, VrXform x) {
    return vr_xf_mul(vr_xf_inverse(vr_xf(cam_pos, cam_rot)), x);
}
static inline VrXform vr_roll_from_cam(VrVec3 cam_pos, VrQuat cam_rot, VrXform rel) {
    return vr_xf_mul(vr_xf(cam_pos, cam_rot), rel);
}

/* a -> b by e (position lerp, rotation nlerp) */
static inline VrXform vr_roll_blend(VrXform a, VrXform b, float e) {
    if (e <= 0.0f) return a;
    if (e >= 1.0f) return b;
    return vr_xf(vr_add(a.t, vr_scale(vr_sub(b.t, a.t), e)), vr_qnlerp(a.q, b.q, e));
}

/* ---- the one-frame lag of the view ----
 * The hands are placed from the camera of the PREVIOUS render (H1), in the body's model space:
 * cam_model = R(-yaw) (cam_world - body_pos). The model is then drawn at the body's position of the NEXT
 * render, where the eye is at the next camera. So the hands are right only if (camera - body) did not change
 * in between. In an evade it does, by up to the speed difference of the body (root motion, the evade scale's
 * extra) and the camera (the comfort dash's slide, anim_smooth) times a frame: the hands lead the eye in the roll
 * direction and settle when the speeds meet again. The remedy: advance the view's model-space camera by its own
 * last relative step (a one-frame linear extrapolation), scaled by the detach amount. */

/* The model-space step of (camera - body) between two snapshots, per second; zero when the snapshots are
 * further apart than 0.1 s or the step is absurd (a teleport, a cut). */
static inline VrVec3 vr_roll_rel_vel(VrVec3 prev_cam, VrVec3 prev_body, VrVec3 cam, VrVec3 body, float yaw, float gap_s) {
    if (!(gap_s > 1e-4f) || gap_s > 0.1f) return vr_v3(0, 0, 0);
    const VrVec3 d = vr_sub(vr_sub(cam, body), vr_sub(prev_cam, prev_body));
    const VrVec3 m = vr_qrotate(vr_qaxis(vr_v3(0, 1, 0), -yaw), d);
    const float l = sqrtf(vr_dot(m, m));
    if (!(l < 0.5f)) return vr_v3(0, 0, 0);
    return vr_scale(m, 1.0f / gap_s);
}

/* The camera advanced by one game frame of that relative velocity, by `e` (the detach amount); at most 0.2 m. */
static inline VrVec3 vr_roll_lead(VrVec3 cam_model, VrVec3 rel_vel, float dt, float e) {
    if (!(dt > 0.0f) || !(e > 0.0f)) return cam_model;
    if (dt > 0.05f) dt = 0.05f;
    VrVec3 s = vr_scale(rel_vel, dt * (e > 1.0f ? 1.0f : e));
    const float l = sqrtf(vr_dot(s, s));
    if (l > 0.2f) s = vr_scale(s, 0.2f / l);
    return vr_add(cam_model, s);
}
#endif
