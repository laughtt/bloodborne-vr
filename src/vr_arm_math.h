/* Arm IK for VR hands, shared by the game (src/runtime_vr_arms.c, C) and the 3D sandbox
 * (gpu/xr_bridge/sandbox3d/rig.cpp, C++), so both pose the hunter's arms with the same code.
 * Pure logic: static inline, no OpenXR, no globals, no I/O; compiles as C (C11) and as C++.
 * Design: docs/vr/findings/24-arms-ik.md (13 section 5). Tests: tests/test_vr_arms.c (built as C
 * and as C++: ninja -C out/gpu vr-arms-test vr-arms-test-cpp).
 *
 * What it does, per arm, in the character's model space (any handedness: every function is
 * equivariant under a mirror, so the game's left-handed model space and the sandbox's mirrored
 * right-handed one use the same calls):
 *   1. the wrist target and the hand rotation from a controller grip pose (vam_grip_wrist,
 *      vam_hand_rotation), or a fixed "ready" pose (vam_rest_target);
 *   2. two-bone IK from the live shoulder (UpperArm joint) to the wrist (vam_two_bone): the elbow
 *      bends toward a pole direction, the target is pulled into the reachable range;
 *   3. the bones' new model rotations (vam_solve_arm): UpperArm and Forearm aimed along the
 *      solved segments with their roll set by the pole (vam_aim_with_twist), the hand at the
 *      controller's orientation, the forearm twist helpers taking part of the wrist roll.
 *
 * Bind data (VamArmBind) is the skeleton's reference pose in model space: the game composes it
 * from the hkaSkeleton reference pose, the sandbox from the FLVER bind matrices. The two
 * skeletons are the same 108 bones (findings/13 2.2).
 *
 * First person in the game (findings/24 section 3): a grip pose relative to the head (OpenXR) is
 * taken into the game camera's frame by the Z mirror of vr_head_math.h (vam_xr_in_head_game),
 * then into model space through the camera expressed in model space (vam_camera_in_model). The
 * grip frame there has the fingers along +Z (OpenXR -Z mirrored) and the thumb along +Y.
 */
#ifndef VR_ARM_MATH_H
#define VR_ARM_MATH_H
#include "vr_head_math.h"

/* ---- vectors and rotations (the sandbox's math3d.h names, C style) ---- */

static inline float vam_len(VrVec3 v) { return sqrtf(vr_dot(v, v)); }
static inline VrVec3 vam_norm(VrVec3 v, VrVec3 fallback) {
    const float l = vam_len(v);
    return l > 1e-8f ? vr_scale(v, 1.0f / l) : fallback;
}
static inline VrVec3 vam_lerp(VrVec3 a, VrVec3 b, float t) { return vr_add(a, vr_scale(vr_sub(b, a), t)); }
/* The part of v perpendicular to the unit axis. */
static inline VrVec3 vam_perp(VrVec3 v, VrVec3 axis) { return vr_sub(v, vr_scale(axis, vr_dot(v, axis))); }
/* Any unit vector perpendicular to the unit v. */
static inline VrVec3 vam_any_perp(VrVec3 v) {
    return vam_norm(fabsf(v.x) < 0.9f ? vr_cross(v, vr_v3(1, 0, 0)) : vr_cross(v, vr_v3(0, 1, 0)), vr_v3(0, 0, 0));
}
static inline VrQuat vam_axis_angle(VrVec3 axis, float radians) {
    return vr_qaxis(vam_norm(axis, vr_v3(0, 1, 0)), radians);
}
/* The shortest rotation taking unit `from` to unit `to`. */
static inline VrQuat vam_from_to(VrVec3 from, VrVec3 to) {
    const float d = vr_dot(from, to);
    if (d < -0.999999f) return vam_axis_angle(vam_any_perp(from), VR_PI);
    const VrVec3 c = vr_cross(from, to);
    return vr_qnormalize(vr_q(c.x, c.y, c.z, 1.0f + d));
}
/* Signed angle (radians) from unit a to unit b about the unit axis (both perpendicular to it). */
static inline float vam_signed_angle(VrVec3 a, VrVec3 b, VrVec3 axis) {
    return atan2f(vr_dot(vr_cross(a, b), axis), vr_dot(a, b));
}
/* The rotation whose matrix columns are x, y, z (orthonormal, det +1 algebraically). */
static inline VrQuat vam_quat_from_columns(VrVec3 x, VrVec3 y, VrVec3 z) {
    const float m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22;
    VrQuat q;
    if (tr > 0.0f) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        q = vr_q((y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s);
    } else if (m00 > m11 && m00 > m22) {
        const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q = vr_q(0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s);
    } else if (m11 > m22) {
        const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q = vr_q((y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s);
    } else {
        const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q = vr_q((z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s);
    }
    return vr_qnormalize(q);
}
/* The rotation taking +X to x and +Y to y (x, y orthonormal; +Z goes to x * y). */
static inline VrQuat vam_frame(VrVec3 x, VrVec3 y) { return vam_quat_from_columns(x, y, vr_cross(x, y)); }

/* ---- two-bone IK ---- */

/* The elbow for a chain of lengths a (upper) and b (lower) from the shoulder s toward the target
 * t, bending toward `pole`. *wrist gets the wrist actually reached (t pulled in or pushed out to
 * the reachable range |a-b|..0.9995 (a+b)), *clamped 1 when it moved. Either may be NULL. */
static inline VrVec3 vam_two_bone(VrVec3 s, VrVec3 t, float a, float b, VrVec3 pole, VrVec3 *wrist, int *clamped) {
    const VrVec3 d = vr_sub(t, s);
    const float len = vam_len(d);
    const VrVec3 n = len > 1e-6f ? vr_scale(d, 1.0f / len) : vr_v3(0.0f, -1.0f, 0.0f);
    const float lo = fabsf(a - b) + 1e-4f, hi = (a + b) * 0.9995f;
    const float dc = vr_clampf(len, lo, hi);
    if (clamped) *clamped = fabsf(dc - len) > 1e-4f;
    const VrVec3 pp = vam_norm(vam_perp(pole, n), vam_any_perp(n));
    const float x = (a * a - b * b + dc * dc) / (2.0f * dc);
    const float h = sqrtf(a * a - x * x > 0.0f ? a * a - x * x : 0.0f);
    if (wrist) *wrist = vr_add(s, vr_scale(n, dc));
    return vr_add(vr_add(s, vr_scale(n, x)), vr_scale(pp, h));
}

/* The rotation turning a bind bone segment `bind_dir` to `new_dir`, rolled about `new_dir` so the
 * bind reference `bind_ref` (perpendicular to bind_dir) lands on `new_ref`. */
static inline VrQuat vam_aim_with_twist(VrVec3 bind_dir, VrVec3 new_dir, VrVec3 bind_ref, VrVec3 new_ref) {
    const VrVec3 axis = vam_norm(new_dir, vr_v3(1, 0, 0));
    const VrQuat swing = vam_from_to(vam_norm(bind_dir, vr_v3(1, 0, 0)), axis);
    const VrVec3 r1 = vam_norm(vam_perp(vr_qrotate(swing, bind_ref), axis), vr_v3(0, 0, 0));
    const VrVec3 r2 = vam_norm(vam_perp(new_ref, axis), vr_v3(0, 0, 0));
    if (vam_len(r1) < 0.5f || vam_len(r2) < 0.5f) return swing;
    return vr_qnormalize(vr_qmul(vam_axis_angle(axis, vam_signed_angle(r1, r2, axis)), swing));
}

/* ---- the hand ---- */

/* The bind hand frame -> a grip frame: the bind finger direction (wrist to knuckles) onto
 * `fingers`, the bind thumb direction (perpendicular to it) onto `thumb`. */
static inline VrQuat vam_hand_basis(VrVec3 fingers_bind, VrVec3 thumb_bind, VrVec3 fingers, VrVec3 thumb) {
    return vr_qnormalize(vr_qmul(vam_frame(fingers, thumb), vr_qconj(vam_frame(fingers_bind, thumb_bind))));
}
/* The hand bone's model rotation for a grip frame `grip` (its rotation; the grip's own finger and
 * thumb axes `grip_fingers`, `grip_thumb`: OpenXR / the sandbox -Z and +Y, the game's camera frame
 * +Z and +Y). */
static inline VrQuat vam_hand_rotation(VrQuat grip, VrVec3 grip_fingers, VrVec3 grip_thumb, VrVec3 fingers_bind,
                                       VrVec3 thumb_bind, VrQuat bind_hand_rot) {
    return vr_qnormalize(vr_qmul(vr_qmul(grip, vam_hand_basis(fingers_bind, thumb_bind, grip_fingers, grip_thumb)),
                                 bind_hand_rot));
}
/* The grip -> wrist offset of one side: the right hand's (in its grip frame) as given, mirrored
 * for the left hand (x, and the rotation's y and z, in the frame's own axes). */
static inline void vam_side_offset(int right, VrVec3 off_pos, VrQuat off_rot, VrVec3 *pos, VrQuat *rot) {
    *pos = right ? off_pos : vr_v3(-off_pos.x, off_pos.y, off_pos.z);
    *rot = right ? off_rot : vr_q(off_rot.x, -off_rot.y, -off_rot.z, off_rot.w);
}
/* The wrist (hand bone) target and the offset grip rotation from a grip pose. */
static inline void vam_grip_wrist(VrVec3 grip_pos, VrQuat grip_rot, VrVec3 off_pos, VrQuat off_rot, VrVec3 *wrist,
                                  VrQuat *rot) {
    *wrist = vr_add(grip_pos, vr_qrotate(grip_rot, off_pos));
    *rot = vr_qnormalize(vr_qmul(grip_rot, off_rot));
}

/* ---- the arm ---- */

typedef struct {
    VrVec3 upper, fore, hand;                 /* bind joints: shoulder, elbow, wrist (model space) */
    VrQuat upper_rot, fore_rot, hand_rot;     /* bind model rotations */
    VrQuat twist_rot, twist1_rot;             /* Support_X_ForeArmTwist (wrist), ...Twist1 (mid forearm) */
    int has_twist, has_twist1;
    float twist1_t;                           /* where Twist1 sits along the forearm (0 elbow .. 1 wrist) */
    VrVec3 fingers, thumb;                    /* bind hand frame: wrist -> knuckles, thumb side (unit) */
} VamArmBind;

typedef struct {
    VrVec3 pole;       /* elbow direction for this side (model space; mirror x for the other side) */
    float arm_scale;   /* chain length scale (1: the skeleton's) */
    float twist_share; /* share of the wrist roll the forearm twist bone takes (Twist1: half of it) */
} VamArmParams;

typedef struct {
    int clamped;
    float reach;                              /* shoulder-target distance over the chain length */
    VrVec3 shoulder, elbow, wrist, twist_pos, twist1_pos;
    VrQuat upper_rot, fore_rot, hand_rot, twist_rot, twist1_rot; /* model rotations */
} VamArmPose;

static inline float vam_upper_len(const VamArmBind *b) { return vam_len(vr_sub(b->fore, b->upper)); }
static inline float vam_fore_len(const VamArmBind *b) { return vam_len(vr_sub(b->hand, b->fore)); }

/* Fills the bind hand frame from the wrist, the knuckles' mean and the thumb's base (bind model
 * positions). */
static inline void vam_bind_hand_frame(VamArmBind *b, VrVec3 knuckles, VrVec3 thumb_base) {
    b->fingers = vam_norm(vr_sub(knuckles, b->hand), vr_v3(0, 0, -1));
    b->thumb = vam_norm(vam_perp(vr_sub(thumb_base, b->hand), b->fingers), vr_v3(0, 1, 0));
}

/* One arm: the chain from the live `shoulder` to the wrist `target`, the hand at `hand_rot`
 * (model rotation of the hand bone, vam_hand_rotation). */
static inline void vam_solve_arm(const VamArmBind *b, VrVec3 shoulder, VrVec3 target, VrQuat hand_rot,
                                 const VamArmParams *p, VamArmPose *out) {
    const float scale = p->arm_scale > 0.0f ? p->arm_scale : 1.0f;
    const float a = vam_upper_len(b) * scale, c = vam_fore_len(b) * scale;
    VrVec3 wrist;
    int clamped = 0;
    const VrVec3 elbow = vam_two_bone(shoulder, target, a, c, p->pole, &wrist, &clamped);
    const VrVec3 reach_dir = vam_norm(vr_sub(wrist, shoulder), vr_v3(0, -1, 0));
    const VrVec3 pole_dir = vam_norm(vam_perp(p->pole, reach_dir), vam_any_perp(reach_dir));

    const VrVec3 bind_upper = vam_norm(vr_sub(b->fore, b->upper), vr_v3(1, 0, 0));
    const VrVec3 new_upper = vam_norm(vr_sub(elbow, shoulder), bind_upper);
    const VrQuat upper = vam_aim_with_twist(bind_upper, new_upper, vam_perp(p->pole, bind_upper), pole_dir);
    const VrVec3 bind_fore = vam_norm(vr_sub(b->hand, b->fore), vr_v3(1, 0, 0));
    const VrVec3 new_fore = vam_norm(vr_sub(wrist, elbow), bind_fore);
    const VrQuat fore = vam_aim_with_twist(bind_fore, new_fore, vam_perp(p->pole, bind_fore), pole_dir);

    out->clamped = clamped;
    out->reach = vam_len(vr_sub(target, shoulder)) / (a + c > 1e-4f ? a + c : 1e-4f);
    out->shoulder = shoulder;
    out->elbow = elbow;
    out->wrist = wrist;
    out->upper_rot = vr_qnormalize(vr_qmul(upper, b->upper_rot));
    out->fore_rot = vr_qnormalize(vr_qmul(fore, b->fore_rot));
    out->hand_rot = vr_qnormalize(hand_rot);

    /* The forearm twist bones take part of the wrist roll (less candy-wrapping at the wrist). */
    const float share = vr_clampf(p->twist_share, 0.0f, 1.0f);
    float roll = 0.0f;
    const VrVec3 thumb_hand = vam_norm(vam_perp(vr_qrotate(vr_qmul(out->hand_rot, vr_qconj(b->hand_rot)), b->thumb), new_fore), vr_v3(0, 0, 0));
    const VrVec3 thumb_fore = vam_norm(vam_perp(vr_qrotate(fore, b->thumb), new_fore), vr_v3(0, 0, 0));
    if (vam_len(thumb_hand) > 0.5f && vam_len(thumb_fore) > 0.5f) roll = vam_signed_angle(thumb_fore, thumb_hand, new_fore);
    out->twist_pos = wrist;
    out->twist1_pos = vam_lerp(elbow, wrist, b->twist1_t);
    out->twist_rot = vr_qnormalize(vr_qmul(vr_qmul(vam_axis_angle(new_fore, roll * share), fore), b->twist_rot));
    out->twist1_rot = vr_qnormalize(vr_qmul(vr_qmul(vam_axis_angle(new_fore, roll * share * 0.5f), fore), b->twist1_rot));
}

/* ---- targets ---- */

/* A "ready" pose without controllers (the game's interim arms): the wrist relative to the live
 * shoulder along the body's axes (`outward` for this side, `up`, `forward`; unit, model space),
 * the grip's fingers along forward pitched down by `pitch_down` radians, the thumb up. Returns the
 * wrist; *fingers and *thumb get the grip axes in model space (for vam_hand_basis). */
static inline VrVec3 vam_rest_target(VrVec3 shoulder, VrVec3 outward, VrVec3 up, VrVec3 forward, float out_m,
                                     float up_m, float forward_m, float pitch_down, VrVec3 *fingers, VrVec3 *thumb) {
    const float c = cosf(pitch_down), s = sinf(pitch_down);
    *fingers = vam_norm(vr_sub(vr_scale(forward, c), vr_scale(up, s)), forward);
    *thumb = vam_norm(vr_add(vr_scale(up, c), vr_scale(forward, s)), up);
    return vr_add(shoulder, vr_add(vr_add(vr_scale(outward, out_m), vr_scale(up, up_m)), vr_scale(forward, forward_m)));
}

/* A grip pose (OpenXR) relative to the head pose (same space), in the game camera's frame (+X
 * right, +Y up, +Z forward; the Z mirror of vr_head_math.h). */
static inline void vam_xr_in_head_game(VrVec3 head_pos, VrQuat head_rot, VrVec3 grip_pos, VrQuat grip_rot, VrVec3 *pos,
                                       VrQuat *rot) {
    const VrQuat hi = vr_qconj(vr_qnormalize(head_rot));
    *pos = vr_xr_to_game_vec(vr_qrotate(hi, vr_sub(grip_pos, head_pos)));
    *rot = vr_xr_to_game_rot(vr_qnormalize(vr_qmul(hi, grip_rot)));
}
/* The game camera (rows right, up, forward, position; world, det +1) in the character's model
 * space (world = position + R(yaw) . model, vr_model_to_world). */
static inline void vam_camera_in_model(const float rows[4][4], VrVec3 position, float yaw, VrVec3 *pos, VrQuat *rot) {
    const VrQuat world = vam_quat_from_columns(vr_v3(rows[0][0], rows[0][1], rows[0][2]),
                                               vr_v3(rows[1][0], rows[1][1], rows[1][2]),
                                               vr_v3(rows[2][0], rows[2][1], rows[2][2]));
    const VrQuat inv_yaw = vr_qaxis(vr_v3(0, 1, 0), -yaw);
    *rot = vr_qnormalize(vr_qmul(inv_yaw, world));
    *pos = vr_qrotate(inv_yaw, vr_sub(vr_v3(rows[3][0], rows[3][1], rows[3][2]), position));
}

/* The wrist target and the hand rotation in model space from a controller grip and the head it is
 * measured against (OpenXR, one space: the same GetInput snapshot, or head = identity for a
 * head-relative grip), through the first-person camera in model space (vam_camera_in_model: the
 * camera is the head after the body heading / recenter, so this is the same conversion the head
 * gets). off_pos/off_rot: vam_side_offset. hand_rot has the game's grip frame (fingers +Z,
 * thumb +Y) before vam_hand_rotation. */
static inline void vam_grip_to_model(VrVec3 head_pos, VrQuat head_rot, VrVec3 grip_pos, VrQuat grip_rot,
                                     VrVec3 off_pos, VrQuat off_rot, VrVec3 cam_pos, VrQuat cam_rot,
                                     VrVec3 *wrist, VrQuat *hand_rot) {
    VrVec3 w_xr, p;
    VrQuat r_xr, q;
    vam_grip_wrist(grip_pos, grip_rot, off_pos, off_rot, &w_xr, &r_xr);
    vam_xr_in_head_game(head_pos, head_rot, w_xr, r_xr, &p, &q);
    *wrist = vr_add(cam_pos, vr_qrotate(cam_rot, p));
    *hand_rot = vr_qmul(cam_rot, q);
}

/* ---- the virtual upper body: shoulders under the head (findings/24 5.4) ---- */

static inline float vam_wrap(float a) {
    if (!isfinite(a)) return 0.0f;
    a = fmodf(a + VR_PI, 2.0f * VR_PI);
    if (a < 0.0f) a += 2.0f * VR_PI;
    return a - VR_PI;
}
/* The angle about +Y (vam_axis_angle's sense) turning the body's facing onto the view's forward,
 * both flattened onto the floor; 0 when either is vertical. */
static inline float vam_yaw_between(VrVec3 facing, VrVec3 view_fwd) {
    const VrVec3 a = vam_norm(vr_v3(facing.x, 0.0f, facing.z), vr_v3(0, 0, 0));
    const VrVec3 b = vam_norm(vr_v3(view_fwd.x, 0.0f, view_fwd.z), vr_v3(0, 0, 0));
    if (vam_len(a) < 0.5f || vam_len(b) < 0.5f) return 0.0f;
    return vam_signed_angle(a, b, vr_v3(0, 1, 0));
}
/* Soft follow of the shoulders' yaw `off` toward the head's `target` (radians): nothing inside
 * +-deadzone; beyond it the excess is taken with time constant tau (0: at once). Wrapped. */
static inline float vam_body_follow(float off, float target, float deadzone, float tau, float dt) {
    const float d = vam_wrap(target - off);
    if (fabsf(d) <= deadzone) return vam_wrap(off);
    const float excess = d - (d > 0.0f ? deadzone : -deadzone);
    const float k = tau > 0.0f ? (dt > 0.0f ? 1.0f - expf(-dt / tau) : 0.0f) : 1.0f;
    return vam_wrap(off + excess * k);
}
/* The shoulders' heading kept in the WORLD (findings/24 5.6): it follows only the head (deadzone,
 * smoothing), never the character's own turning, so a character that spins to face its movement
 * (walking backwards: 180 deg) leaves the arms in front of the view at once. `char_yaw` is the
 * character's model -> world yaw (vr_model_to_world), `head_model` the head's yaw in model space
 * (vam_yaw_between), both about +Y. Returns the shoulders' turn in model space (vam_turn_about). */
typedef struct { int valid; float world; } VamShoulderYaw;
static inline float vam_shoulder_yaw(VamShoulderYaw *s, float char_yaw, float head_model, float deadzone, float tau,
                                     float dt) {
    const float head_world = vam_wrap(char_yaw + head_model);
    if (!s->valid) { s->world = head_world; s->valid = 1; }
    else s->world = vam_body_follow(s->world, head_world, deadzone, tau, dt);
    return vam_wrap(s->world - char_yaw);
}
/* A model-space point turned by `yaw` about the vertical through `pivot`. */
static inline VrVec3 vam_turn_about(VrVec3 p, VrVec3 pivot, float yaw) {
    const VrVec3 d = vr_qrotate(vam_axis_angle(vr_v3(0, 1, 0), yaw), vr_v3(p.x - pivot.x, 0.0f, p.z - pivot.z));
    return vr_v3(pivot.x + d.x, p.y, pivot.z + d.z);
}

/* ---- blending (the game's animation vs the IK, per bone) ---- */

/* Moves `w` toward `target` (0..1) at 1/in_s per second going up, 1/out_s going down. */
static inline float vam_weight_step(float w, float target, float dt, float in_s, float out_s) {
    if (!(dt > 0.0f)) return w;
    if (target > w) w = in_s > 0.0f ? (w + dt / in_s < target ? w + dt / in_s : target) : target;
    else if (target < w) w = out_s > 0.0f ? (w - dt / out_s > target ? w - dt / out_s : target) : target;
    return vr_clampf(w, 0.0f, 1.0f);
}

#endif
