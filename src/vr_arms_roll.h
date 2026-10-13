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
#include "vr_arm_math.h"

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

/* ---- the shake (findings/24 section 8.2) ----
 * rel_vel above is one raw finite difference: the step of one H1 over that H1's wall-clock gap. The H1 and the
 * game's step do not run at the same point of every frame (frame time 17-33 ms in the headset logs, H1 and the
 * arms a few ms of phase apart), so step / gap swings from frame to frame (a gap 3 ms off on a 20 ms frame is 15 %
 * of the step, 2-3 cm at 10 m/s), and the camera position also carries the real head's own motion (true stereo:
 * extrapolated ~0.1 s ahead), which the hands follow through the grips anyway, so its noise was led a second time.
 * Now: the step is that of the point of the headset space where the head is (the camera minus the real head's own
 * step), and the velocity is the ratio of the low-passed step and the low-passed gap, updated once per new H1
 * snapshot; it is then led by the arms' own frame interval as before (vr_roll_lead). */

/* Where a camera (world position and rotation, vam_camera_in_model with no body) built from `head_p`/`head_q`
 * puts the point `p` of the headset's space (OpenXR): the anchor of that frame evaluated at p. */
static inline VrVec3 vr_roll_head_point(VrVec3 cam_w, VrQuat cam_q, VrVec3 head_p, VrQuat head_q, VrVec3 p) {
    VrVec3 w;
    VrQuat q;
    vam_grip_to_model(head_p, head_q, p, vr_qidentity(), vr_v3(0, 0, 0), vr_qidentity(), cam_w, cam_q, &w, &q);
    return w;
}

/* The model-space step (metres) of (anchor - body) between two H1 snapshots: `prev_point` is where the
 * previous camera put this frame's head point (vr_roll_head_point; the previous camera itself without a head),
 * `cam` this camera (world). Zero for an absurd step (a teleport, a cut). */
static inline VrVec3 vr_roll_rel_step(VrVec3 prev_point, VrVec3 prev_body, VrVec3 cam, VrVec3 body, float yaw) {
    const VrVec3 d = vr_sub(vr_sub(cam, body), vr_sub(prev_point, prev_body));
    const VrVec3 m = vr_qrotate(vr_qaxis(vr_v3(0, 1, 0), -yaw), d);
    if (!(sqrtf(vr_dot(m, m)) < 0.5f)) return vr_v3(0, 0, 0);
    return m;
}

#define VR_ROLL_STEP_MAX 0.2f /* metres per H1: 10 m/s at 50 fps; a larger step is cut to it */

/* The low-passed relative velocity: step and gap each low-passed (x += alpha (new - x), alpha in (0, 1], 1 = the
 * raw last snapshot) once per new H1 snapshot (`seq`), velocity = step / gap. A gap under 4 ms or over 0.1 s (a
 * stall, a load) starts it again at zero. */
typedef struct {
    VrVec3 step;
    float gap;
    uint32_t seq;
    int valid;
} VrRollLead;

static inline void vr_roll_lead_reset(VrRollLead *l) {
    l->step = vr_v3(0, 0, 0);
    l->gap = 0.0f;
    l->seq = 0;
    l->valid = 0;
}

static inline VrVec3 vr_roll_lead_vel(const VrRollLead *l) {
    return l->valid && l->gap > 0.0f ? vr_scale(l->step, 1.0f / l->gap) : vr_v3(0, 0, 0);
}

static inline VrVec3 vr_roll_lead_update(VrRollLead *l, uint32_t seq, VrVec3 step, float gap_s, float alpha) {
    if (l->valid && seq == l->seq) return vr_roll_lead_vel(l);
    if (!(gap_s >= 0.004f) || gap_s > 0.1f) {
        vr_roll_lead_reset(l);
        l->seq = seq;
        return vr_v3(0, 0, 0);
    }
    if (!(alpha > 0.0f) || alpha > 1.0f) alpha = 1.0f;
    const float n = sqrtf(vr_dot(step, step));
    if (!(n <= VR_ROLL_STEP_MAX)) step = n > 0.0f && isfinite(n) ? vr_scale(step, VR_ROLL_STEP_MAX / n) : vr_v3(0, 0, 0);
    if (!l->valid) {
        l->step = step;
        l->gap = gap_s;
    } else {
        l->step = vr_add(l->step, vr_scale(vr_sub(step, l->step), alpha));
        l->gap += alpha * (gap_s - l->gap);
    }
    l->seq = seq;
    l->valid = 1;
    return vr_roll_lead_vel(l);
}

/* ---- which head the controllers are measured against (arm_head=, findings/24 section 8.2) ----
 * The hand is placed as camera . head^-1 . grip. The camera is built from the head H1 used (in true stereo
 * predicted ~0.1 s ahead, extrapolated), the grip snapshot carries its own head from the pad poll. With the
 * snapshot's head every difference between those two heads (head rotation over the gap, the prediction's error)
 * moves the hands in the world while the scene stays put (the compositor reprojects it from the camera's head):
 * at 60 deg/s and 60 ms, 3.6 deg, ~3 cm at the hand. With the camera's own head the hands stay at the controllers.
 * Both heads must be in one space: the views are in the screen's space (LOCAL unless BB_XR_STAGE=1), the grips
 * in LOCAL; heads further apart than 0.5 m or 60 deg are taken as another space and the snapshot's head is used.
 * Returns 1 when the view's head was chosen. */
static inline int vr_arm_pick_head(int want_view, int view_ok, VrVec3 view_p, VrQuat view_q, VrVec3 xr_p, VrQuat xr_q,
                                   VrVec3 *head_p, VrQuat *head_q) {
    *head_p = xr_p;
    *head_q = xr_q;
    if (!want_view || !view_ok) return 0;
    const VrVec3 d = vr_sub(view_p, xr_p);
    if (!(sqrtf(vr_dot(d, d)) < 0.5f)) return 0;
    const VrQuat a = vr_qnormalize(view_q), b = vr_qnormalize(xr_q);
    const float c = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    if (!(c > 0.8660254f)) return 0; /* |cos(half angle)| > cos 30 deg: under 60 deg apart */
    *head_p = view_p;
    *head_q = view_q;
    return 1;
}
#endif
