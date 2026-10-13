/* Unit tests for src/vr_arm_math.h, the arm IK the game (src/runtime_vr_arms.c) and the 3D sandbox
 * (gpu/xr_bridge/sandbox3d/rig.cpp) share (docs/vr/findings/24-arms-ik.md). Written in the common
 * subset of C and C++: tests/test_vr_arms_cpp.cpp compiles this same file as C++. Build: ninja -C
 * out/gpu vr-arms-test vr-arms-test-cpp (no GPU, no game). Prints PASS lines; exit 0. */
#include "vr_arm_math.h"
#include "vr_fp_math.h"
#include "vr_arms_roll.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

static int near_v(VrVec3 a, VrVec3 b, float eps) { return vam_len(vr_sub(a, b)) <= eps; }
static int near_f(float a, float b, float eps) { return fabsf(a - b) <= eps; }
static VrVec3 mirror_v(VrVec3 v) { return vr_v3(-v.x, v.y, v.z); }
static VrQuat mirror_q(VrQuat q) { return vr_q(q.x, -q.y, -q.z, q.w); }
/* Same rotation (q and -q). */
static int near_q(VrQuat a, VrQuat b, float eps) {
    const float d = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return d >= 1.0f - eps;
}

/* The game's right arm in the bind pose (findings/13 2.2: A-pose, arms 45 deg down, R at -X), each
 * bone's local +X along its segment, the face toward -Z. */
static VamArmBind right_bind(void) {
    VamArmBind b;
    memset(&b, 0, sizeof(b));
    b.upper = vr_v3(-0.171f, 1.440f, 0.033f);
    b.fore = vr_v3(-0.371f, 1.240f, 0.033f);
    b.hand = vr_v3(-0.564f, 1.047f, 0.033f);
    const VrVec3 d1 = vam_norm(vr_sub(b.fore, b.upper), vr_v3(1, 0, 0));
    const VrVec3 d2 = vam_norm(vr_sub(b.hand, b.fore), vr_v3(1, 0, 0));
    b.upper_rot = vam_from_to(vr_v3(1, 0, 0), d1);
    b.fore_rot = vam_from_to(vr_v3(1, 0, 0), d2);
    b.hand_rot = b.fore_rot;
    b.twist_rot = b.fore_rot;
    b.twist1_rot = b.fore_rot;
    b.has_twist = b.has_twist1 = 1;
    b.twist1_t = 0.5f;
    /* knuckles further along the forearm, the thumb toward the front (-Z) */
    vam_bind_hand_frame(&b, vr_add(b.hand, vr_scale(d2, 0.09f)), vr_add(b.hand, vr_v3(0.0f, 0.0f, -0.04f)));
    return b;
}
static VamArmParams right_params(void) {
    VamArmParams p;
    memset(&p, 0, sizeof(p));
    p.pole = vam_norm(vr_v3(-0.4f, -0.85f, 0.35f), vr_v3(0, -1, 0)); /* out (-X), down, back (+Z) */
    p.arm_scale = 1.0f;
    p.twist_share = 1.0f;
    return p;
}
/* Forward kinematics with the bind locals: where the game's model pose puts the elbow and the wrist
 * once the new rotations are written (child = parent . bind local). */
static void fk(const VamArmBind *b, const VamArmPose *o, VrVec3 *elbow, VrVec3 *wrist) {
    *elbow = vr_add(o->shoulder, vr_qrotate(vr_qmul(o->upper_rot, vr_qconj(b->upper_rot)), vr_sub(b->fore, b->upper)));
    *wrist = vr_add(*elbow, vr_qrotate(vr_qmul(o->fore_rot, vr_qconj(b->fore_rot)), vr_sub(b->hand, b->fore)));
}

static void test_math(void) {
    const VrQuat f = vam_from_to(vr_v3(1, 0, 0), vr_v3(0, 0, 1));
    CHECK(near_v(vr_qrotate(f, vr_v3(1, 0, 0)), vr_v3(0, 0, 1), 1e-5f), "from_to");
    const VrQuat g = vam_from_to(vr_v3(0, 1, 0), vr_v3(0, -1, 0));
    CHECK(near_v(vr_qrotate(g, vr_v3(0, 1, 0)), vr_v3(0, -1, 0), 1e-5f), "from_to opposite");
    const VrQuat qs[4] = {vr_xr_ypr(0.3f, -0.2f, 1.1f), vr_xr_ypr(3.0f, 0.1f, 0.0f), vr_xr_ypr(-1.5f, 1.4f, -2.9f),
                          vr_q(0.0f, 0.0f, 1.0f, 0.0f)};
    for (int i = 0; i < 4; ++i) {
        float m[3][3];
        vr_qmat3(qs[i], m);
        const VrQuat back = vam_quat_from_columns(vr_v3(m[0][0], m[1][0], m[2][0]), vr_v3(m[0][1], m[1][1], m[2][1]),
                                                  vr_v3(m[0][2], m[1][2], m[2][2]));
        CHECK(near_q(back, qs[i], 1e-5f), "matrix -> quaternion round trip %d", i);
    }
    const VrQuat fr = vam_frame(vr_v3(0, 0, -1), vr_v3(0, 1, 0));
    CHECK(near_v(vr_qrotate(fr, vr_v3(1, 0, 0)), vr_v3(0, 0, -1), 1e-5f) &&
          near_v(vr_qrotate(fr, vr_v3(0, 1, 0)), vr_v3(0, 1, 0), 1e-5f), "frame");
    CHECK(near_f(vam_signed_angle(vr_v3(1, 0, 0), vr_v3(0, 0, -1), vr_v3(0, 1, 0)), VR_PI / 2, 1e-5f), "signed angle");
    printf("PASS math\n");
}

static void test_two_bone(void) {
    const VrVec3 s = vr_v3(0, 0, 0);
    VrVec3 w;
    int clamped = 1;
    const VrVec3 e = vam_two_bone(s, vr_v3(0.4f, 0, 0), 0.3f, 0.3f, vr_v3(0, -1, 0), &w, &clamped);
    CHECK(!clamped && near_v(w, vr_v3(0.4f, 0, 0), 1e-5f), "reached");
    CHECK(near_f(vam_len(e), 0.3f, 1e-4f) && near_f(vam_len(vr_sub(w, e)), 0.3f, 1e-4f), "lengths");
    CHECK(e.y < 0.0f, "toward the pole");
    vam_two_bone(s, vr_v3(2.0f, 0, 0), 0.3f, 0.3f, vr_v3(0, -1, 0), &w, &clamped);
    CHECK(clamped && w.x > 0.59f && w.x < 0.6f, "out of reach: clamped along the ray (%f)", w.x);
    vam_two_bone(s, vr_v3(0.0f, 0, 0), 0.3f, 0.25f, vr_v3(0, -1, 0), &w, &clamped);
    CHECK(clamped && vam_len(w) >= 0.05f - 1e-3f, "at the shoulder: pushed out to |a-b|");
    /* the pole along the reach direction: still a valid elbow */
    const VrVec3 e2 = vam_two_bone(s, vr_v3(0.0f, -0.4f, 0), 0.3f, 0.3f, vr_v3(0, -1, 0), &w, &clamped);
    CHECK(near_f(vam_len(e2), 0.3f, 1e-4f) && isfinite(e2.x), "pole along the reach");
    const VrQuat r = vam_aim_with_twist(vr_v3(1, 0, 0), vr_v3(0, 0, -1), vr_v3(0, 1, 0), vr_v3(1, 0, 0));
    CHECK(near_v(vr_qrotate(r, vr_v3(1, 0, 0)), vr_v3(0, 0, -1), 1e-4f), "aim: the segment");
    CHECK(near_v(vr_qrotate(r, vr_v3(0, 1, 0)), vr_v3(1, 0, 0), 1e-4f), "aim: the roll");
    printf("PASS two-bone\n");
}

static void test_arm(void) {
    const VamArmBind b = right_bind();
    const VamArmParams p = right_params();
    const float a = vam_upper_len(&b), c = vam_fore_len(&b);
    CHECK(near_f(a, 0.283f, 2e-3f) && near_f(c, 0.273f, 2e-3f), "the game's lengths: %f %f", a, c);
    /* hand in front of the chest, below the shoulder */
    const VrVec3 shoulder = b.upper;
    const VrVec3 target = vr_add(shoulder, vr_v3(0.05f, -0.25f, -0.32f));
    VamArmPose o;
    vam_solve_arm(&b, shoulder, target, b.hand_rot, &p, &o);
    CHECK(!o.clamped && near_v(o.wrist, target, 1e-4f), "reached");
    VrVec3 fe, fw;
    fk(&b, &o, &fe, &fw);
    CHECK(near_v(fe, o.elbow, 1e-4f) && near_v(fw, target, 1e-3f), "the bind locals rebuild the solved chain (FK %f)", vam_len(vr_sub(fw, target)));
    CHECK(vr_dot(vr_sub(o.elbow, vam_lerp(shoulder, target, 0.5f)), p.pole) > 0.0f, "elbow toward the pole");
    CHECK(o.elbow.y < shoulder.y && o.elbow.x < shoulder.x, "elbow down and out (R side -X)");
    /* continuity: 1 mm target steps move the elbow smoothly */
    float worst = 0.0f;
    VrVec3 prev = o.elbow;
    for (int i = 0; i <= 600; ++i) {
        const float t = (float)i / 600.0f;
        const VrVec3 tt = vr_add(shoulder, vr_v3(0.25f * sinf(6.0f * t) - 0.1f, -0.45f + 0.6f * t, -0.2f - 0.15f * cosf(4.0f * t)));
        vam_solve_arm(&b, shoulder, tt, b.hand_rot, &p, &o);
        const float jump = vam_len(vr_sub(o.elbow, prev));
        if (i > 0 && jump > worst) worst = jump;
        prev = o.elbow;
        fk(&b, &o, &fe, &fw);
        if (!near_v(fw, o.wrist, 2e-3f)) { CHECK(0, "FK at step %d", i); break; }
    }
    CHECK(worst < 0.02f, "no elbow flips along a path (worst step %.4f m)", worst);
    /* out of reach */
    vam_solve_arm(&b, shoulder, vr_add(shoulder, vr_v3(0, 0, -2.0f)), b.hand_rot, &p, &o);
    CHECK(o.clamped && vam_len(vr_sub(o.wrist, shoulder)) <= a + c + 1e-4f && o.reach > 3.0f, "clamped");
    /* the twist bones: with the hand rolled 60 deg about the forearm, Twist takes it all, Twist1 half */
    vam_solve_arm(&b, shoulder, target, b.hand_rot, &p, &o);
    const VrVec3 fore_dir = vam_norm(vr_sub(o.wrist, o.elbow), vr_v3(1, 0, 0));
    const VrQuat rolled = vr_qmul(vam_axis_angle(fore_dir, 1.0472f), o.fore_rot);
    const VrQuat base_fore = o.fore_rot;
    vam_solve_arm(&b, shoulder, target, rolled, &p, &o);
    const VrVec3 t0 = vr_qrotate(vr_qmul(base_fore, vr_qconj(b.fore_rot)), b.thumb);
    const VrVec3 t1 = vr_qrotate(vr_qmul(o.twist_rot, vr_qconj(b.twist_rot)), b.thumb);
    const VrVec3 t2 = vr_qrotate(vr_qmul(o.twist1_rot, vr_qconj(b.twist1_rot)), b.thumb);
    const float r1 = vam_signed_angle(vam_norm(vam_perp(t0, fore_dir), t0), vam_norm(vam_perp(t1, fore_dir), t1), fore_dir);
    const float r2 = vam_signed_angle(vam_norm(vam_perp(t0, fore_dir), t0), vam_norm(vam_perp(t2, fore_dir), t2), fore_dir);
    CHECK(near_f(r1, 1.0472f, 0.02f) && near_f(r2, 0.5236f, 0.02f), "twist shares: %f %f", r1, r2);
    CHECK(near_v(o.twist1_pos, vam_lerp(o.elbow, o.wrist, 0.5f), 1e-5f), "twist1 mid forearm");
    printf("PASS arm\n");
}

/* The same solve in the mirrored frame (the sandbox's: x -> -x) gives the mirrored answer: the
 * game's left-handed model space and the sandbox's right-handed one share the code. */
static void test_mirror(void) {
    const VamArmBind b = right_bind();
    const VamArmParams p = right_params();
    VamArmBind m = b;
    m.upper = mirror_v(b.upper); m.fore = mirror_v(b.fore); m.hand = mirror_v(b.hand);
    m.upper_rot = mirror_q(b.upper_rot); m.fore_rot = mirror_q(b.fore_rot); m.hand_rot = mirror_q(b.hand_rot);
    m.twist_rot = mirror_q(b.twist_rot); m.twist1_rot = mirror_q(b.twist1_rot);
    m.fingers = mirror_v(b.fingers); m.thumb = mirror_v(b.thumb);
    VamArmParams mp = p;
    mp.pole = mirror_v(p.pole);
    const VrVec3 target = vr_add(b.upper, vr_v3(0.1f, -0.2f, -0.35f));
    const VrQuat hand = vr_qmul(vr_xr_ypr(0.4f, -0.3f, 0.7f), b.hand_rot);
    VamArmPose o, om;
    vam_solve_arm(&b, b.upper, target, hand, &p, &o);
    vam_solve_arm(&m, m.upper, mirror_v(target), mirror_q(hand), &mp, &om);
    CHECK(near_v(om.elbow, mirror_v(o.elbow), 1e-5f) && near_v(om.wrist, mirror_v(o.wrist), 1e-5f), "mirrored joints");
    CHECK(near_q(om.upper_rot, mirror_q(o.upper_rot), 1e-5f) && near_q(om.fore_rot, mirror_q(o.fore_rot), 1e-5f) &&
          near_q(om.twist_rot, mirror_q(o.twist_rot), 1e-4f), "mirrored rotations");
    /* the hand basis too */
    const VrQuat h = vam_hand_rotation(vr_qidentity(), vr_v3(0, 0, -1), vr_v3(0, 1, 0), b.fingers, b.thumb, b.hand_rot);
    const VrQuat hm = vam_hand_rotation(vr_qidentity(), vr_v3(0, 0, -1), vr_v3(0, 1, 0), m.fingers, m.thumb, m.hand_rot);
    CHECK(near_q(hm, mirror_q(h), 1e-5f), "mirrored hand rotation");
    /* the grip offset mirrors as the sandbox's (x, euler y, euler z negated) */
    VrVec3 op;
    VrQuat oq;
    const VrQuat off = vr_qmul(vr_qmul(vam_axis_angle(vr_v3(0, 0, 1), 0.2f), vam_axis_angle(vr_v3(0, 1, 0), 0.3f)),
                               vam_axis_angle(vr_v3(1, 0, 0), -0.26f));
    vam_side_offset(0, vr_v3(0.01f, -0.01f, 0.07f), off, &op, &oq);
    const VrQuat expect = vr_qmul(vr_qmul(vam_axis_angle(vr_v3(0, 0, 1), -0.2f), vam_axis_angle(vr_v3(0, 1, 0), -0.3f)),
                                  vam_axis_angle(vr_v3(1, 0, 0), -0.26f));
    CHECK(near_v(op, vr_v3(-0.01f, -0.01f, 0.07f), 1e-6f) && near_q(oq, expect, 1e-5f), "left offset");
    printf("PASS mirror\n");
}

static void test_hand(void) {
    const VamArmBind b = right_bind();
    /* a grip with no rotation (OpenXR/sandbox axes): fingers along -Z, thumb up */
    const VrQuat h = vam_hand_rotation(vr_qidentity(), vr_v3(0, 0, -1), vr_v3(0, 1, 0), b.fingers, b.thumb, b.hand_rot);
    const VrQuat delta = vr_qmul(h, vr_qconj(b.hand_rot));
    CHECK(near_v(vr_qrotate(delta, b.fingers), vr_v3(0, 0, -1), 1e-4f), "fingers along the grip");
    CHECK(near_v(vr_qrotate(delta, b.thumb), vr_v3(0, 1, 0), 1e-4f), "thumb up");
    /* a grip turned 90 deg to the left: the fingers follow */
    const VrQuat left = vam_axis_angle(vr_v3(0, 1, 0), VR_PI / 2);
    const VrQuat h2 = vam_hand_rotation(left, vr_v3(0, 0, -1), vr_v3(0, 1, 0), b.fingers, b.thumb, b.hand_rot);
    CHECK(near_v(vr_qrotate(vr_qmul(h2, vr_qconj(b.hand_rot)), b.fingers), vr_v3(-1, 0, 0), 1e-4f), "turned grip");
    /* the wrist sits behind the grip by the offset */
    VrVec3 w;
    VrQuat r;
    vam_grip_wrist(vr_v3(1, 1, 1), left, vr_v3(0, -0.01f, 0.07f), vr_qidentity(), &w, &r);
    CHECK(near_v(w, vr_v3(1.07f, 0.99f, 1.0f), 1e-5f), "wrist offset in the grip frame");
    /* the rest pose */
    VrVec3 fingers, thumb;
    const VrVec3 rest = vam_rest_target(b.upper, vr_v3(-1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, -1), 0.0f, -0.3f, 0.32f,
                                        20.0f * VR_DEG, &fingers, &thumb);
    CHECK(near_v(rest, vr_add(b.upper, vr_v3(0, -0.3f, -0.32f)), 1e-6f), "rest wrist");
    CHECK(near_f(vr_dot(fingers, thumb), 0.0f, 1e-5f) && fingers.y < 0.0f && fingers.z < 0.0f && thumb.y > 0.9f, "rest axes");
    printf("PASS hand\n");
}

/* First person in the game: a controller in front of / right of the headset ends in front of /
 * right of the character's face (R bones are at model -X, the face looks along -Z). */
static void test_game_mapping(void) {
    const float yaw = 0.7f;                  /* physics yaw: model +Z heads this way */
    const VrVec3 pos = vr_v3(10.0f, -3.0f, 5.0f);
    const VrVec3 eye_model = vr_v3(0.0f, 1.6f, -0.1f);
    const float heading = vr_facing_heading(yaw); /* the camera looks where the face does */
    float cam[4][4];
    vr_cam_from_heading(cam, heading, vr_model_to_world(pos, yaw, eye_model));
    VrVec3 cp;
    VrQuat cq;
    vam_camera_in_model((const float (*)[4])cam, pos, yaw, &cp, &cq);
    CHECK(near_v(cp, eye_model, 1e-4f), "camera position in model space");
    CHECK(near_v(vr_qrotate(cq, vr_v3(0, 0, 1)), vr_v3(0, 0, -1), 1e-4f), "camera forward = model -Z (the face)");
    CHECK(near_v(vr_qrotate(cq, vr_v3(1, 0, 0)), vr_v3(-1, 0, 0), 1e-4f), "camera right = model -X (the R side)");
    /* head turned and moved; the grip 0.3 m forward, 0.2 right, 0.25 down of the head in the head's frame */
    const VrQuat head = vr_xr_ypr(0.0f, 0.0f, 0.0f);
    const VrVec3 head_p = vr_v3(0.5f, 1.7f, -0.2f);
    const VrVec3 grip_p = vr_add(head_p, vr_v3(0.2f, -0.25f, -0.3f));
    VrVec3 gp;
    VrQuat gq;
    vam_xr_in_head_game(head_p, head, grip_p, vr_qidentity(), &gp, &gq);
    CHECK(near_v(gp, vr_v3(0.2f, -0.25f, 0.3f), 1e-5f), "grip in the camera frame (+Z forward)");
    const VrVec3 hand_model = vr_add(cp, vr_qrotate(cq, gp));
    CHECK(near_v(hand_model, vr_add(eye_model, vr_v3(-0.2f, -0.25f, -0.3f)), 1e-4f), "grip in model space: in front, R side");
    /* the grip's fingers: OpenXR -Z mirrored to +Z in the camera frame, then the face direction */
    CHECK(near_v(vr_qrotate(vr_qmul(cq, gq), vr_v3(0, 0, 1)), vr_v3(0, 0, -1), 1e-4f), "grip fingers forward");
    /* a head turned 30 deg left: the same grip relative to the head turns with it */
    const VrQuat head_l = vr_xr_ypr(30.0f * VR_DEG, 0.0f, 0.0f);
    const VrVec3 grip_l = vr_add(head_p, vr_qrotate(head_l, vr_v3(0.0f, 0.0f, -0.4f)));
    vam_xr_in_head_game(head_p, head_l, grip_l, head_l, &gp, &gq);
    CHECK(near_v(gp, vr_v3(0, 0, 0.4f), 1e-5f) && near_q(gq, vr_qidentity(), 1e-5f), "head-relative");
    /* the camera turned with the head (level H1 camera: R(heading) . head) keeps the grip where it is in the world */
    float cam_l[4][4];
    vr_cam_from_heading(cam_l, heading, vr_model_to_world(pos, yaw, eye_model));
    vr_cam_compose(cam_l, vr_head_rel_game(head_l, vr_qidentity()), vr_v3(0, 0, 0), 1);
    vam_camera_in_model((const float (*)[4])cam_l, pos, yaw, &cp, &cq);
    const VrVec3 hm = vr_add(cp, vr_qrotate(cq, gp));
    /* 30 deg to the user's left = toward the character's left = model +X, ahead = -Z */
    CHECK(near_v(hm, vr_add(eye_model, vr_v3(0.4f * sinf(30.0f * VR_DEG), 0.0f, -0.4f * cosf(30.0f * VR_DEG))), 1e-4f),
          "head turned left: the grip ahead-left (%f %f %f)", hm.x, hm.y, hm.z);
    /* vam_grip_to_model: the same result as the head-relative fake path, for any head in the space */
    {
        const VrVec3 hp = vr_v3(1.5f, 1.1f, -4.0f);
        const VrQuat hq = vr_xr_ypr(70.0f * VR_DEG, -10.0f * VR_DEG, 5.0f * VR_DEG);
        const VrVec3 rel_p = vr_v3(0.18f, -0.38f, -0.32f);
        const VrQuat rel_q = vr_xr_ypr(10.0f * VR_DEG, -20.0f * VR_DEG, 0.0f);
        VrVec3 op, w_fake, w_xr;
        VrQuat oq, r_fake, r_xr;
        vam_side_offset(1, vr_v3(0.0f, -0.01f, 0.07f), vam_axis_angle(vr_v3(1, 0, 0), -15.0f * VR_DEG), &op, &oq);
        vam_grip_to_model(vr_v3(0, 0, 0), vr_qidentity(), rel_p, rel_q, op, oq, cp, cq, &w_fake, &r_fake);
        /* the same hand, the head anywhere: world grip = head * relative */
        const VrVec3 world_p = vr_add(hp, vr_qrotate(hq, rel_p));
        const VrQuat world_q = vr_qnormalize(vr_qmul(hq, rel_q));
        vam_grip_to_model(hp, hq, world_p, world_q, op, oq, cp, cq, &w_xr, &r_xr);
        CHECK(near_v(w_fake, w_xr, 1e-4f), "grip_to_model: wrist independent of the head's place in the space");
        CHECK(near_q(r_fake, r_xr, 1e-4f), "grip_to_model: hand rotation independent too");
        /* a hand straight ahead of the face (no offset) lands ahead of it in model space (-Z) */
        VrVec3 w0;
        VrQuat r0;
        vam_grip_to_model(hp, hq, vr_add(hp, vr_qrotate(hq, vr_v3(0, 0, -0.5f))), hq, vr_v3(0, 0, 0), vr_qidentity(), cp,
                          cq, &w0, &r0);
        CHECK(near_v(w0, vr_add(cp, vr_qrotate(cq, vr_v3(0, 0, 0.5f))), 1e-4f), "grip_to_model: ahead of the camera");
    }
    printf("PASS game mapping\n");
}

static void test_weight(void) {
    float w = 0.0f;
    for (int i = 0; i < 9; ++i) w = vam_weight_step(w, 1.0f, 0.016f, 0.15f, 0.08f);
    CHECK(near_f(w, 0.96f, 1e-3f), "fade in: %f", w);
    w = vam_weight_step(w, 1.0f, 0.1f, 0.15f, 0.08f);
    CHECK(w == 1.0f, "capped");
    w = vam_weight_step(w, 0.0f, 0.04f, 0.15f, 0.08f);
    CHECK(near_f(w, 0.5f, 1e-5f), "fade out faster: %f", w);
    CHECK(vam_weight_step(0.3f, 1.0f, 0.0f, 0.1f, 0.1f) == 0.3f, "dt 0: unchanged");
    CHECK(vam_weight_step(0.3f, 1.0f, 0.01f, 0.0f, 0.0f) == 1.0f, "no fade");
    printf("PASS weight\n");
}

static void test_body_follow(void) {
    /* the face looks along -Z (model); a view 40 deg toward -X is +40 deg about +Y? check by rotating */
    const VrVec3 facing = vr_v3(0, 0, -1);
    const VrVec3 view = vr_qrotate(vam_axis_angle(vr_v3(0, 1, 0), 0.7f), vr_v3(0, -0.5f, -1));
    CHECK(near_f(vam_yaw_between(facing, view), 0.7f, 1e-5f), "yaw between: %f", vam_yaw_between(facing, view));
    CHECK(vam_yaw_between(facing, vr_v3(0, -1, 0)) == 0.0f, "straight down: 0");
    const float dz = 25.0f * VR_DEG;
    CHECK(vam_body_follow(0.0f, 20.0f * VR_DEG, dz, 0.2f, 0.016f) == 0.0f, "inside the deadzone: no change");
    CHECK(near_f(vam_body_follow(0.0f, 60.0f * VR_DEG, dz, 0.0f, 0.016f), 35.0f * VR_DEG, 1e-5f), "tau 0: at the edge");
    float off = 0.0f;
    for (int i = 0; i < 300; ++i) off = vam_body_follow(off, -90.0f * VR_DEG, dz, 0.2f, 1.0f / 72.0f);
    CHECK(near_f(off, -65.0f * VR_DEG, 1e-3f), "converges to the edge: %f", off / VR_DEG);
    off = vam_body_follow(170.0f * VR_DEG, -170.0f * VR_DEG, 25.0f * VR_DEG, 0.0f, 0.016f);
    CHECK(near_f(off, 170.0f * VR_DEG, 1e-4f), "across the seam: 20 deg the short way, inside 25");
    off = vam_body_follow(170.0f * VR_DEG, -150.0f * VR_DEG, 10.0f * VR_DEG, 0.0f, 0.016f);
    CHECK(near_f(vam_wrap(off - (-160.0f * VR_DEG)), 0.0f, 1e-4f), "across the seam: %f", off / VR_DEG);
    const VrVec3 r = vam_turn_about(vr_v3(-0.171f, 1.44f, 0.033f), vr_v3(0, 0, 0.033f), VR_PI / 2);
    CHECK(near_v(r, vr_v3(0.0f, 1.44f, 0.033f + 0.171f), 1e-5f), "shoulder turned about the spine: %f %f %f", r.x, r.y, r.z);
    /* the shoulders' heading in the world: the character's own turns never move it */
    VamShoulderYaw sy;
    memset(&sy, 0, sizeof(sy));
    const float dt = 1.0f / 72.0f;
    float b = vam_shoulder_yaw(&sy, 0.3f, 0.0f, dz, 0.2f, dt);
    CHECK(near_f(b, 0.0f, 1e-6f), "start: shoulders under the head");
    b = vam_shoulder_yaw(&sy, 0.3f + VR_PI / 2, -VR_PI / 2, dz, 0.2f, dt); /* the body turned 90 right, head still */
    CHECK(near_f(b, -VR_PI / 2, 1e-5f), "body turned 90: the shoulders turn back -90 at once (%f)", b / VR_DEG);
    b = vam_shoulder_yaw(&sy, 0.3f + VR_PI, -VR_PI, dz, 0.2f, dt); /* walking backwards: the body faces back */
    CHECK(near_f(fabsf(b), VR_PI, 1e-4f), "body turned 180: the shoulders turn 180 at once (%f)", b / VR_DEG);
    /* the turned shoulders, the same in the world as at the start: the facing stays in front of the view */
    const VrVec3 fwd0 = vr_qrotate(vam_axis_angle(vr_v3(0, 1, 0), 0.3f), vr_v3(0, 0, -1));
    const VrVec3 fwd1 = vr_qrotate(vam_axis_angle(vr_v3(0, 1, 0), 0.3f + VR_PI + b), vr_v3(0, 0, -1));
    CHECK(near_v(fwd0, fwd1, 1e-4f), "180: the arms' front is unchanged in the world");
    /* wrap: the character's yaw crosses +-pi while the head stays */
    memset(&sy, 0, sizeof(sy));
    vam_shoulder_yaw(&sy, 3.1f, 0.0f, dz, 0.2f, dt);
    b = vam_shoulder_yaw(&sy, -3.1f, 6.2f - 2.0f * VR_PI, dz, 0.2f, dt);
    CHECK(near_f(vam_wrap(b - (6.2f - 2.0f * VR_PI)), 0.0f, 1e-4f), "wrap at +-pi: %f", b);
    /* the head itself: inside the deadzone nothing, beyond it the follow */
    memset(&sy, 0, sizeof(sy));
    vam_shoulder_yaw(&sy, 0.0f, 0.0f, dz, 0.2f, dt);
    b = vam_shoulder_yaw(&sy, 0.0f, 20.0f * VR_DEG, dz, 0.2f, dt);
    CHECK(b == 0.0f, "head 20 deg: inside the deadzone");
    for (int i = 0; i < 300; ++i) b = vam_shoulder_yaw(&sy, 0.0f, 60.0f * VR_DEG, dz, 0.2f, dt);
    CHECK(near_f(b, 35.0f * VR_DEG, 1e-3f), "head 60 deg: the shoulders follow to the edge (%f)", b / VR_DEG);
    printf("PASS body follow\n");
}

/* Spaghetti guards: bone lengths stay, a far target is eased not cut, the roll is continuous. */
static void test_spaghetti(void) {
    const VamArmBind b = right_bind();
    VamArmParams p = right_params();
    p.soft = 0.92f;
    p.give = 0.04f;
    p.fore_roll = 0.35f;
    const float a = vam_upper_len(&b), c = vam_fore_len(&b);
    const VrVec3 shoulder = b.upper;
    VamArmPose o;
    /* sweep a target out through and past the reach: the elbow angle and the wrist move continuously,
     * the FK (bind locals = fixed bone lengths) always rebuilds the solved chain */
    float worst = 0.0f, prev_elbow = -1.0f;
    for (int i = 0; i <= 400; ++i) {
        const float d = 0.30f + 0.5f * (float)i / 400.0f; /* 0.30 .. 0.80 m, chain is 0.556 */
        vam_solve_arm(&b, shoulder, vr_add(shoulder, vr_v3(-0.2f * d, -0.3f * d, -0.93f * d)), b.hand_rot, &p, &o);
        VrVec3 fe, fw;
        fk(&b, &o, &fe, &fw);
        CHECK(near_v(fw, o.wrist, 2e-3f) && near_v(fe, o.elbow, 2e-3f), "FK matches at d=%.3f", d);
        CHECK(near_f(vam_len(vr_sub(o.elbow, o.shoulder)), a, 1e-4f) && near_f(vam_len(vr_sub(o.wrist, o.elbow)), c, 1e-4f),
              "bone lengths kept at d=%.3f", d);
        if (prev_elbow >= 0.0f && fabsf(o.elbow_deg - prev_elbow) > worst) worst = fabsf(o.elbow_deg - prev_elbow);
        prev_elbow = o.elbow_deg;
    }
    CHECK(worst < 3.0f, "elbow angle changes smoothly through the reach limit (worst step %.2f deg)", worst);
    CHECK(o.clamped && o.give > 0.0f && o.give <= 0.04f + 1e-6f && o.elbow_deg < 179.9f && o.eased < 1.0f,
          "far target: shoulder gives %.3f, eased %.3f, elbow %.1f deg", o.give, o.eased, o.elbow_deg);
    CHECK(vam_len(vr_sub(o.wrist, o.shoulder)) <= a + c, "never beyond the chain");
    /* the roll is continuous across +-180 deg, and the Forearm bone takes its share */
    vam_solve_arm(&b, shoulder, vr_add(shoulder, vr_v3(0.05f, -0.25f, -0.32f)), b.hand_rot, &p, &o);
    const VrVec3 fd = vam_norm(vr_sub(o.wrist, o.elbow), vr_v3(1, 0, 0));
    const VrQuat base = o.fore_rot;
    p.has_roll_prev = 0;
    float prev = 0.0f;
    float step_worst = 0.0f, roll0 = 0.0f;
    for (int i = 0; i <= 120; ++i) {
        const float ang = (float)i * 3.0f * VR_DEG; /* 0 .. 360 deg in 3 deg steps */
        vam_solve_arm(&b, shoulder, vr_add(shoulder, vr_v3(0.05f, -0.25f, -0.32f)),
                      vr_qmul(vam_axis_angle(fd, ang), base), &p, &o);
        if (i > 0 && fabsf(o.roll - prev) > step_worst) step_worst = fabsf(o.roll - prev);
        if (i == 0) roll0 = o.roll;
        prev = o.roll;
        p.roll_prev = o.roll;
        p.has_roll_prev = 1;
        if (i == 20) CHECK(near_f(o.roll - roll0, 60.0f * VR_DEG, 0.02f), "roll measured %f", (o.roll - roll0) / VR_DEG);
    }
    CHECK(step_worst < 4.0f * VR_DEG, "roll unwrapped through 180 deg (worst step %.2f deg)", step_worst / VR_DEG);
    /* fore_roll 0.5: the forearm bone itself rolls half of 60 deg */
    p.has_roll_prev = 0;
    p.fore_roll = 0.5f;
    vam_solve_arm(&b, shoulder, vr_add(shoulder, vr_v3(0.05f, -0.25f, -0.32f)), vr_qmul(vam_axis_angle(fd, 1.0472f), base), &p, &o);
    const VrQuat dq = vr_qmul(o.fore_rot, vr_qconj(base));
    CHECK(near_f(2.0f * atan2f(vr_dot(vr_v3(dq.x, dq.y, dq.z), fd), dq.w < 0 ? -dq.w : dq.w), 0.5236f, 0.02f) || near_f(fabsf(dq.w), cosf(0.2618f), 0.02f),
          "forearm takes half the roll (w %f)", dq.w);
    /* the wrist bent across the forearm (thumb projection would vanish): the roll is still defined */
    p.fore_roll = 0.0f;
    vam_solve_arm(&b, shoulder, vr_add(shoulder, vr_v3(0.05f, -0.25f, -0.32f)),
                  vr_qmul(vam_axis_angle(vam_any_perp(fd), 1.5708f), base), &p, &o);
    CHECK(isfinite(o.roll) && vr_qvalid(o.twist_rot), "roll finite with a 90 deg wrist bend");
    /* a pole lined up with the reach never flips the elbow */
    float worst2 = 0.0f;
    VrVec3 pe = vr_v3(0, 0, 0);
    p.has_roll_prev = 0;
    for (int i = 0; i <= 200; ++i) {
        const VrVec3 n = vam_norm(vr_add(vr_scale(p.pole, 1.0f), vr_v3(0.004f * (float)(i - 100), 0.0f, 0.0f)), vr_v3(0, -1, 0));
        vam_solve_arm(&b, shoulder, vr_add(shoulder, vr_scale(n, 0.4f)), b.hand_rot, &p, &o);
        if (i > 0 && vam_len(vr_sub(o.elbow, pe)) > worst2) worst2 = vam_len(vr_sub(o.elbow, pe));
        pe = o.elbow;
    }
    CHECK(worst2 < 0.03f, "no elbow flip with the hand along the pole (worst step %.4f m)", worst2);
    printf("PASS spaghetti guards\n");
}

/* Arms detached while the body evades (vr_arms_roll.h): the blend, the camera-frame pin. */
static void test_roll_detach(void) {
    VrArmsRoll r;
    vr_roll_reset(&r);
    CHECK(vr_roll_capturing(&r, 0) && !vr_roll_capturing(&r, 1), "pins are captured only outside an evade");
    /* 0 -> 1 over VR_ROLL_BLEND_S, no pop */
    float prev = 0.0f, worst = 0.0f;
    for (int i = 0; i < 20; ++i) {
        const float e = vr_roll_step(&r, 1, 0.01f, VR_ROLL_BLEND_S);
        if (e - prev > worst) worst = e - prev;
        prev = e;
    }
    CHECK(near_f(r.e, 1.0f, 1e-6f) && worst <= 0.1001f, "blend in over 0.1 s (step %.3f)", worst);
    CHECK(!vr_roll_capturing(&r, 0), "no capture while still fading out");
    vr_roll_step(&r, 0, 0.05f, VR_ROLL_BLEND_S);
    CHECK(near_f(r.e, 0.5f, 1e-5f), "blend out, half way %.3f", r.e);
    vr_roll_step(&r, 0, 0.2f, VR_ROLL_BLEND_S);
    CHECK(r.e == 0.0f && vr_roll_capturing(&r, 0), "attached again, capturing");
    CHECK(vr_roll_step(&r, 1, 0.0f, VR_ROLL_BLEND_S) == 0.0f, "dt 0 changes nothing");
    CHECK(vr_roll_step(&r, 1, 0.01f, 0.0f) == 1.0f, "blend 0 = at once");
    /* the pin stays put in the camera frame while the body (the model-space shoulder) tumbles */
    const VrVec3 cam_p = vr_v3(0.1f, 1.6f, -0.2f);
    const VrQuat cam_q = vr_qnormalize(vam_axis_angle(vr_v3(0, 1, 0), 0.4f));
    const VrXform sh = vr_xf(vr_v3(-0.17f, 1.44f, 0.03f), vr_qidentity());
    const VrXform rel = vr_roll_to_cam(cam_p, cam_q, sh);
    const VrXform same = vr_roll_from_cam(cam_p, cam_q, rel);
    CHECK(near_v(same.t, sh.t, 1e-5f), "pin round trip");
    const VrVec3 cam2_p = vr_v3(0.5f, 1.0f, -0.9f); /* the view moved on (a roll) */
    const VrQuat cam2_q = vr_qnormalize(vr_qmul(vam_axis_angle(vr_v3(0, 1, 0), 1.2f), vam_axis_angle(vr_v3(1, 0, 0), 0.5f)));
    const VrXform moved = vr_roll_from_cam(cam2_p, cam2_q, rel);
    const VrVec3 back = vr_qrotate(vr_qconj(cam2_q), vr_sub(moved.t, cam2_p));
    CHECK(near_v(back, rel.t, 1e-5f), "pinned shoulder keeps its camera-relative position");
    const VrXform b0 = vr_roll_blend(sh, moved, 0.0f), b1 = vr_roll_blend(sh, moved, 1.0f), bh = vr_roll_blend(sh, moved, 0.5f);
    CHECK(near_v(b0.t, sh.t, 1e-6f) && near_v(b1.t, moved.t, 1e-6f) &&
          near_v(bh.t, vr_scale(vr_add(sh.t, moved.t), 0.5f), 1e-5f), "blend ends and middle");
    printf("PASS roll detach\n");
}

/* A roll with a sliding camera and a body moving ahead: the hands must stay fixed relative to the eye. */
static void test_roll_lag(void) {
    const float dt = 1.0f / 90.0f;
    const float yaw = 0.7f;
    /* body 9 m/s along its heading; the camera slides at 4 m/s along it (the comfort dash, anim_smooth) */
    const VrVec3 dir = vr_qrotate(vr_qaxis(vr_v3(0, 1, 0), yaw), vr_v3(0, 0, 1));
    const VrVec3 hand_off = vr_v3(0.2f, -0.3f, -0.4f); /* the controller relative to the camera, model axes */
    float worst_plain = 0.0f, worst_lead = 0.0f;
    VrVec3 body = vr_v3(3, 0, -2), cam = vr_add(body, vr_v3(0, 1.6f, 0)), prev_cam = cam, prev_body = body;
    VrVec3 rel_vel = vr_v3(0, 0, 0), view_cam_model = vr_v3(0, 1.6f, 0);
    for (int i = 0; i < 60; ++i) {
        /* the render before: the snapshot the IK will use */
        view_cam_model = vr_qrotate(vr_qaxis(vr_v3(0, 1, 0), -yaw), vr_sub(cam, body));
        rel_vel = vr_roll_rel_vel(prev_cam, prev_body, cam, body, yaw, dt);
        prev_cam = cam;
        prev_body = body;
        /* the next render: the body and the camera moved on; the model is drawn at the body, the eye is at cam */
        body = vr_add(body, vr_scale(dir, 9.0f * dt));
        cam = vr_add(cam, vr_scale(dir, 4.0f * dt));
        const VrVec3 eye_hand = vr_add(cam, vr_qrotate(vr_qaxis(vr_v3(0, 1, 0), yaw), hand_off));
        for (int lead = 0; lead < 2; ++lead) {
            const VrVec3 c = lead ? vr_roll_lead(view_cam_model, rel_vel, dt, 1.0f) : view_cam_model;
            const VrVec3 drawn = vr_add(body, vr_qrotate(vr_qaxis(vr_v3(0, 1, 0), yaw), vr_add(c, hand_off)));
            const float err = vam_len(vr_sub(drawn, eye_hand));
            if (i > 3 && lead && err > worst_lead) worst_lead = err;
            if (i > 3 && !lead && err > worst_plain) worst_plain = err;
        }
    }
    CHECK(worst_plain > 0.04f, "without the lead the hands run ahead of the eye (%.3f m)", worst_plain);
    CHECK(worst_lead < 0.002f, "with the lead the hands stay on the eye (%.4f m)", worst_lead);
    CHECK(vam_len(vr_roll_rel_vel(prev_cam, prev_body, vr_v3(9, 9, 9), prev_body, yaw, dt)) == 0.0f, "a jump is no velocity");
    CHECK(vam_len(vr_roll_rel_vel(prev_cam, prev_body, prev_cam, prev_body, yaw, 0.5f)) == 0.0f, "a stale snapshot is no velocity");
    CHECK(near_v(vr_roll_lead(view_cam_model, rel_vel, dt, 0.0f), view_cam_model, 1e-7f), "no lead while attached");
    CHECK(vam_len(vr_roll_lead(vr_v3(0, 0, 0), vr_v3(100, 0, 0), 0.05f, 1.0f)) <= 0.2001f, "lead capped");
    printf("PASS roll lag\n");
}

/* findings/24 8.2: the shake. A roll at 9 m/s against a camera at 4 m/s (plus the slide's ease: +-3 m/s at 3 rad/s),
 * a game loop with a variable frame time (20 ms +-2 ms, 5 % hitches of 33 ms: the sim of frame k steps by the time
 * since the last loop start), H1 and the arms each at their own point in the frame with +-3 / +-2 ms of jitter, and
 * the real head bobbing (it moves the camera; the hands follow it through the grips anyway). The IK of frame k+1
 * uses H1(k) plus the lead; the hand is off by lead - (the anchor's true step over frame k+1); the shake is how much
 * that error changes from one frame to the next. */
static unsigned lcg_state = 12345u;
static float lcg01(void) {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (float)((lcg_state >> 8) & 0xFFFFFFu) / 16777216.0f;
}
static float lcg_pm(float a) { return a * (2.0f * lcg01() - 1.0f); }
static void test_lead_jitter(void) {
    const float yaw = 0.7f;
    const VrQuat yq = vr_qaxis(vr_v3(0, 1, 0), yaw);
    const VrVec3 dir = vr_qrotate(yq, vr_v3(0, 0, 1));
    const VrQuat cam_q = yq; /* the camera faces along the roll; the head's rotation is identity */
    enum { N = 600 };
    static float ft[N + 2];
    for (int k = 0; k < N + 2; ++k) ft[k] = lcg01() < 0.05f ? 1.0f / 30.0f : 0.020f + lcg_pm(0.002f);
    VrVec3 body = vr_v3(3, 0, -2), anchor = vr_add(body, vr_v3(0, 1.6f, 0));
    VrVec3 head = vr_v3(0, 0, 0), prev_head = head, prev_cam = vr_v3(0, 0, 0), prev_body = body;
    float loop = 0.0f, t_h1 = 0.0f, prev_t_h1 = 0.0f, t_arm = 0.0f, prev_t_arm = 0.0f;
    VrRollLead filt;
    vr_roll_lead_reset(&filt);
    float off[3] = {0, 0, 0}, shake[3] = {0, 0, 0}, worst[3] = {0, 0, 0};
    VrVec3 e_prev[3];
    int counted = 0;
    for (int k = 1; k <= N; ++k) {
        loop += ft[k - 1]; /* loop start of frame k */
        const float sim_dt = ft[k - 1];
        const float cam_speed = 4.0f + 3.0f * sinf(3.0f * loop);
        body = vr_add(body, vr_scale(dir, 9.0f * sim_dt));
        anchor = vr_add(anchor, vr_scale(dir, cam_speed * sim_dt));
        prev_head = head;
        head = vr_v3(0.02f * sinf(7.0f * loop) + lcg_pm(0.002f), 0.01f * sinf(4.0f * loop) + lcg_pm(0.002f), lcg_pm(0.002f));
        const VrVec3 cam = vr_add(anchor, vr_qrotate(cam_q, vr_xr_to_game_vec(head)));
        prev_t_h1 = t_h1;
        t_h1 = loop + 0.008f + lcg_pm(0.003f);
        /* the arms of frame k+1 run at their point of the next loop */
        prev_t_arm = t_arm;
        t_arm = loop + ft[k] + 0.004f + lcg_pm(0.002f);
        float dt_arm = t_arm - prev_t_arm;
        if (dt_arm > 0.1f) dt_arm = 0.1f;
        /* 8.1: the raw finite difference of the camera over the H1 gap, led by the arms' interval */
        const VrVec3 lead_old = vr_roll_lead(vr_v3(0, 0, 0), vr_roll_rel_vel(prev_cam, prev_body, cam, body, yaw, t_h1 - prev_t_h1), dt_arm, 1.0f);
        /* 8.2: the anchor's step, low-passed with its gap, once per snapshot */
        const VrVec3 point = vr_roll_head_point(prev_cam, cam_q, prev_head, vr_qidentity(), head);
        const VrVec3 vel = vr_roll_lead_update(&filt, (uint32_t)k, vr_roll_rel_step(point, prev_body, cam, body, yaw),
                                               k > 1 ? t_h1 - prev_t_h1 : 0.0f, 0.5f);
        const VrVec3 lead_new = vr_roll_lead(vr_v3(0, 0, 0), vel, dt_arm, 1.0f);
        /* the truth: the anchor's step relative to the body over frame k+1 (it steps by ft[k]) */
        const float cam_next = 4.0f + 3.0f * sinf(3.0f * (loop + ft[k]));
        const VrVec3 truth = vr_qrotate(vr_qconj(yq), vr_scale(dir, (cam_next - 9.0f) * ft[k]));
        const VrVec3 e[3] = {vr_scale(truth, -1.0f), vr_sub(lead_old, truth), vr_sub(lead_new, truth)};
        if (k > 10) {
            for (int m = 0; m < 3; ++m) {
                const float s = vam_len(vr_sub(e[m], e_prev[m]));
                off[m] += vam_len(e[m]);
                shake[m] += s * s;
                if (s > worst[m]) worst[m] = s;
            }
            ++counted;
        }
        for (int m = 0; m < 3; ++m) e_prev[m] = e[m];
        prev_cam = cam;
        prev_body = body;
    }
    for (int m = 0; m < 3; ++m) {
        off[m] /= (float)counted;
        shake[m] = sqrtf(shake[m] / (float)counted);
    }
    printf("     lead jitter (mm; mean hand offset, frame-to-frame change rms / worst): no lead %.1f, %.1f / %.1f; "
           "8.1 raw %.1f, %.1f / %.1f; 8.2 filtered %.1f, %.1f / %.1f\n",
           off[0] * 1e3f, shake[0] * 1e3f, worst[0] * 1e3f, off[1] * 1e3f, shake[1] * 1e3f, worst[1] * 1e3f,
           off[2] * 1e3f, shake[2] * 1e3f, worst[2] * 1e3f);
    /* (the hitches stay: a 33 ms frame after 20 ms ones cannot be known one frame ahead; that floor is the "no lead" shake) */
    CHECK(shake[2] < 0.75f * shake[1], "the filtered lead shakes less than the raw one (rms %.4f vs %.4f m)", shake[2], shake[1]);
    CHECK(shake[2] < shake[0], "and less than no lead at all (rms %.4f vs %.4f m)", shake[2], shake[0]);
    CHECK(worst[2] <= worst[1], "its worst frame is no worse (%.4f vs %.4f m)", worst[2], worst[1]);
    CHECK(off[2] < 0.15f * off[0], "and it still removes the lag (offset %.4f vs %.4f m without a lead)", off[2], off[0]);

    /* steady frames: the filtered lead is exact (the 8.1 result is kept) */
    {
        VrRollLead f;
        vr_roll_lead_reset(&f);
        const VrVec3 s = vr_v3(0.0f, 0.0f, -0.055f);
        VrVec3 v = vr_v3(0, 0, 0);
        for (int i = 0; i < 30; ++i) v = vr_roll_lead_update(&f, (uint32_t)(i + 1), s, 0.011f, 0.5f);
        CHECK(near_v(v, vr_scale(s, 1.0f / 0.011f), 1e-3f), "a constant step is a constant velocity");
        CHECK(near_v(vr_roll_lead_update(&f, 30u, vr_v3(1, 1, 1), 0.011f, 0.5f), v, 1e-7f), "the same snapshot does not step the filter twice");
        CHECK(vam_len(vr_roll_lead_update(&f, 31u, s, 0.3f, 0.5f)) == 0.0f && !f.valid, "a stall starts it again");
    }
    /* bounded: junk steps (alternating, up to twice the cap) move the lead by at most half of what the raw one does */
    {
        VrRollLead f;
        vr_roll_lead_reset(&f);
        VrVec3 prev_new = vr_v3(0, 0, 0), prev_raw = vr_v3(0, 0, 0);
        float worst_new = 0.0f, worst_raw = 0.0f;
        for (int i = 0; i < 200; ++i) {
            const float sgn = (i & 1) ? 1.0f : -1.0f;
            const VrVec3 junk = vr_v3(sgn * 0.4f * lcg01(), lcg_pm(0.1f), -sgn * 0.15f);
            const float gap = 0.02f + lcg_pm(0.006f);
            const VrVec3 l_new = vr_roll_lead(vr_v3(0, 0, 0), vr_roll_lead_update(&f, (uint32_t)(i + 1), junk, gap, 0.5f), 0.02f, 1.0f);
            const VrVec3 l_raw = vr_roll_lead(vr_v3(0, 0, 0), vr_scale(junk, 1.0f / gap), 0.02f, 1.0f);
            CHECK(vam_len(l_new) <= 0.2001f, "lead within its cap");
            if (i > 0) {
                if (vam_len(vr_sub(l_new, prev_new)) > worst_new) worst_new = vam_len(vr_sub(l_new, prev_new));
                if (vam_len(vr_sub(l_raw, prev_raw)) > worst_raw) worst_raw = vam_len(vr_sub(l_raw, prev_raw));
            }
            prev_new = l_new;
            prev_raw = l_raw;
        }
        CHECK(worst_new < 0.5f * worst_raw && worst_new < 0.2f, "junk input: per-frame change %.3f m (raw %.3f m)", worst_new, worst_raw);
    }
    /* the head's own step is not a lead: the camera moving with the head over a still body gives no step */
    {
        const VrVec3 c0 = vr_v3(1, 1.6f, 2), h0 = vr_v3(0.1f, 0.0f, -0.05f), h1 = vr_v3(0.13f, 0.02f, -0.09f);
        const VrQuat cq = vr_qaxis(vr_v3(0, 1, 0), 0.3f);
        const VrVec3 c1 = vr_add(c0, vr_qrotate(cq, vr_xr_to_game_vec(vr_sub(h1, h0))));
        const VrVec3 pt = vr_roll_head_point(c0, cq, h0, vr_qidentity(), h1);
        CHECK(vam_len(vr_roll_rel_step(pt, vr_v3(0, 0, 0), c1, vr_v3(0, 0, 0), 0.3f)) < 1e-5f, "head motion is no step");
        CHECK(vam_len(vr_roll_rel_step(c0, vr_v3(0, 0, 0), vr_v3(9, 9, 9), vr_v3(0, 0, 0), 0.3f)) == 0.0f, "a jump is no step");
    }
    printf("PASS lead jitter\n");
}

/* findings/24 8.2: the head the grips are measured against. The camera is built from a head that differs from the
 * grip snapshot's head (true stereo: predicted ahead and extrapolated); a controller held still in the room must
 * stay still in the world when the camera's head turns. */
static void test_head_pick(void) {
    const float yaw = 0.7f;
    const VrVec3 pos = vr_v3(10.0f, -3.0f, 5.0f);
    const VrVec3 eye_model = vr_v3(0.0f, 1.6f, -0.1f);
    const float heading = vr_facing_heading(yaw);
    const VrVec3 head_p = vr_v3(0.05f, 0.0f, 0.02f);
    const VrVec3 grip_p = vr_v3(0.25f, -0.35f, -0.4f); /* still in the room */
    const VrQuat grip_q = vr_xr_ypr(10.0f * VR_DEG, -30.0f * VR_DEG, 0.0f);
    float worst_view = 0.0f, worst_xr = 0.0f, jump_view = 0.0f, jump_xr = 0.0f;
    VrVec3 prev_view = vr_v3(0, 0, 0), prev_xr = vr_v3(0, 0, 0);
    VrVec3 first = vr_v3(0, 0, 0);
    for (int i = 0; i < 100; ++i) {
        /* game frames at 50 fps; the head turns at 60 deg/s with a wobble. The camera's head is predicted 111 ms
         * ahead (the stereo log's "views for 111.1 ms ahead") with up to 1.5 deg of prediction error; the grip
         * snapshot's head is that of the bridge's latest 90 Hz frame (its display time, ~20 ms ahead, 0-11 ms old) */
        const float t = (float)i / 50.0f;
        const float age = 0.0111f * lcg01();
#define HEAD_YAW(s) (60.0f * VR_DEG * (s) + 8.0f * VR_DEG * sinf(9.0f * (s)))
        const float yaw_xr = HEAD_YAW(t + 0.02f - age);
        const float yaw_cam = HEAD_YAW(t + 0.111f) + 1.5f * VR_DEG * sinf(31.0f * t);
#undef HEAD_YAW
        const VrQuat hq_xr = vr_xr_ypr(yaw_xr, 0.0f, 0.0f), hq_cam = vr_xr_ypr(yaw_cam, 5.0f * VR_DEG * sinf(5.0f * t), 0.0f);
        float cam[4][4];
        vr_cam_from_heading(cam, heading, vr_model_to_world(pos, yaw, eye_model));
        vr_cam_compose(cam, vr_head_rel_game(hq_cam, vr_qidentity()), vr_v3(0, 0, 0), 1);
        VrVec3 cp, w_view, w_xr, hp;
        VrQuat cq, r, hq;
        vam_camera_in_model((const float (*)[4])cam, pos, yaw, &cp, &cq);
        CHECK(vr_arm_pick_head(1, 1, head_p, hq_cam, head_p, hq_xr, &hp, &hq) == 1, "same space: the camera's head");
        vam_grip_to_model(hp, hq, grip_p, grip_q, vr_v3(0, 0, 0), vr_qidentity(), cp, cq, &w_view, &r);
        vr_arm_pick_head(0, 1, head_p, hq_cam, head_p, hq_xr, &hp, &hq);
        vam_grip_to_model(hp, hq, grip_p, grip_q, vr_v3(0, 0, 0), vr_qidentity(), cp, cq, &w_xr, &r);
        if (i == 0) first = w_view;
        if (vam_len(vr_sub(w_view, first)) > worst_view) worst_view = vam_len(vr_sub(w_view, first));
        if (vam_len(vr_sub(w_xr, first)) > worst_xr) worst_xr = vam_len(vr_sub(w_xr, first));
        if (i > 0) {
            if (vam_len(vr_sub(w_view, prev_view)) > jump_view) jump_view = vam_len(vr_sub(w_view, prev_view));
            if (vam_len(vr_sub(w_xr, prev_xr)) > jump_xr) jump_xr = vam_len(vr_sub(w_xr, prev_xr));
        }
        prev_view = w_view;
        prev_xr = w_xr;
    }
    printf("     head pick: a still controller moves %.1f mm (worst frame %.1f mm) with the camera's head, %.1f mm (%.1f mm) with the snapshot's\n",
           worst_view * 1000.0f, jump_view * 1000.0f, worst_xr * 1000.0f, jump_xr * 1000.0f);
    CHECK(worst_view < 0.001f, "with the camera's head a still controller stays still (%.4f m)", worst_view);
    CHECK(worst_xr > 0.03f && jump_xr > 0.01f, "with the snapshot's head it swims (%.4f m) and shakes (%.4f m a frame)", worst_xr, jump_xr);
    /* the fallbacks */
    VrVec3 hp;
    VrQuat hq;
    const VrQuat q0 = vr_qidentity(), q70 = vr_xr_ypr(70.0f * VR_DEG, 0.0f, 0.0f);
    CHECK(!vr_arm_pick_head(1, 1, vr_v3(0, 1.6f, 0), q0, vr_v3(0, 0, 0), q0, &hp, &hq) && near_v(hp, vr_v3(0, 0, 0), 1e-7f),
          "1.6 m apart (STAGE vs LOCAL): the snapshot's head");
    CHECK(!vr_arm_pick_head(1, 1, vr_v3(0, 0, 0), q70, vr_v3(0, 0, 0), q0, &hp, &hq), "70 deg apart: the snapshot's head");
    CHECK(!vr_arm_pick_head(1, 0, vr_v3(0, 0, 0), q0, vr_v3(0, 0, 0), q0, &hp, &hq), "no camera head: the snapshot's head");
    CHECK(!vr_arm_pick_head(0, 1, vr_v3(0, 0, 0), q0, vr_v3(0, 0, 0), q0, &hp, &hq), "arm_head=xr: the snapshot's head");
    printf("PASS head pick\n");
}

/* The wrist cut (findings/24 section 9): the Forearm and its twist helpers scaled about the wrist; the Hand, the
 * fingers and the Weapon bone (the held weapon) bit-identical; skin on the forearm pulled into the wrist by k. */
enum { CUT_UPPER, CUT_FORE, CUT_TWIST1, CUT_TWIST, CUT_HAND, CUT_FINGER, CUT_WEAPON, CUT_N };
static void cut_rig(VrXform *bind, VrXform *model) {
    const VamArmBind b = right_bind();
    const VrVec3 seg = vr_sub(b.hand, b.fore);
    bind[CUT_UPPER] = vr_xf(b.upper, b.upper_rot);
    bind[CUT_FORE] = vr_xf(b.fore, b.fore_rot);
    bind[CUT_TWIST1] = vr_xf(vr_add(b.fore, vr_scale(seg, 0.5f)), b.fore_rot);
    bind[CUT_TWIST] = vr_xf(b.hand, b.fore_rot);
    bind[CUT_HAND] = vr_xf(b.hand, b.hand_rot);
    bind[CUT_FINGER] = vr_xf(vr_add(b.hand, vr_qrotate(b.hand_rot, vr_v3(0.09f, 0.01f, -0.015f))), b.hand_rot);
    bind[CUT_WEAPON] = vr_xf(vr_add(b.hand, vr_qrotate(b.hand_rot, vr_v3(0.076f, 0.005f, 0.017f))), b.hand_rot);
    /* a posed arm: the whole chain moved and turned, the forearm bent by 40 deg at the elbow and the twist
     * helpers rolled (rigid per bone, so the bind locals hold) */
    const VrXform body = vr_xf(vr_v3(0.3f, -0.1f, 0.5f), vr_qaxis(vam_norm(vr_v3(0.2f, 1.0f, -0.3f), vr_v3(0, 1, 0)), 0.7f));
    const VrXform elbow_bend = vr_xf_mul(vr_xf(b.fore, vr_qaxis(vr_v3(0, 0, 1), 40.0f * VR_DEG)), vr_xf(vr_scale(b.fore, -1.0f), vr_qidentity()));
    for (int i = 0; i < CUT_N; ++i) {
        VrXform x = i == CUT_UPPER ? bind[i] : vr_xf_mul(elbow_bend, bind[i]);
        if (i == CUT_TWIST || i == CUT_TWIST1)
            x.q = vr_qnormalize(vr_qmul(x.q, vr_qaxis(vr_v3(1, 0, 0), (i == CUT_TWIST ? 30.0f : 15.0f) * VR_DEG)));
        model[i] = vr_xf_mul(body, x);
    }
}
static void test_cut(void) {
    VrXform bind[CUT_N], model[CUT_N], cut[CUT_N];
    float scale[CUT_N];
    const unsigned char mask[CUT_N] = {0, 1, 1, 1, 0, 0, 0};
    cut_rig(bind, model);
    /* the setting */
    CHECK(vam_cut_scale(0.0f) == 1.0f && vam_cut_scale(-0.5f) == 1.0f && vam_cut_scale(NAN) == 1.0f, "cut <= 0: scale 1");
    CHECK(near_f(vam_cut_scale(0.8f), 0.2f, 1e-6f) && near_f(vam_cut_scale(0.25f), 0.75f, 1e-6f), "cut 0.8 -> 0.2, 0.25 -> 0.75");
    CHECK(vam_cut_scale(1.0f) == VAM_CUT_MIN_SCALE && vam_cut_scale(7.0f) == VAM_CUT_MIN_SCALE && VAM_CUT_MIN_SCALE > 0.0f,
          "cut 1 (and above): the floor %.2f, never 0", VAM_CUT_MIN_SCALE);
    /* cut 0 changes nothing */
    memcpy(cut, model, sizeof(cut));
    for (int i = 0; i < CUT_N; ++i) scale[i] = 1.0f;
    vam_cut_bones(cut, scale, mask, CUT_N, model[CUT_HAND].t, vam_cut_scale(0.0f));
    int same = !memcmp(cut, model, sizeof(cut));
    for (int i = 0; i < CUT_N; ++i) same = same && scale[i] == 1.0f;
    CHECK(same, "cut 0: every bone and scale bit-identical");
    /* cut 0.8 */
    const VrVec3 wrist = model[CUT_HAND].t;
    for (int c = 0; c < 3; ++c) {
        const float amount = c == 0 ? 0.8f : c == 1 ? 0.5f : 1.0f, k = vam_cut_scale(amount);
        memcpy(cut, model, sizeof(cut));
        for (int i = 0; i < CUT_N; ++i) scale[i] = 1.0f;
        vam_cut_bones(cut, scale, mask, CUT_N, wrist, k);
        CHECK(!memcmp(&cut[CUT_HAND], &model[CUT_HAND], sizeof(VrXform)) && !memcmp(&cut[CUT_FINGER], &model[CUT_FINGER], sizeof(VrXform)) &&
              !memcmp(&cut[CUT_WEAPON], &model[CUT_WEAPON], sizeof(VrXform)) && !memcmp(&cut[CUT_UPPER], &model[CUT_UPPER], sizeof(VrXform)),
              "cut %.1f: Hand, finger, Weapon and UpperArm bit-identical", amount);
        CHECK(scale[CUT_HAND] == 1.0f && scale[CUT_FINGER] == 1.0f && scale[CUT_WEAPON] == 1.0f && scale[CUT_UPPER] == 1.0f,
              "cut %.1f: their scales stay 1", amount);
        CHECK(scale[CUT_FORE] == k && scale[CUT_TWIST] == k && scale[CUT_TWIST1] == k, "cut %.1f: the forearm bones scaled by %.2f", amount, k);
        for (int i = CUT_FORE; i <= CUT_TWIST; ++i)
            CHECK(near_q(cut[i].q, model[i].q, 1e-7f) && near_v(cut[i].t, vam_cut_point(model[i].t, wrist, k), 1e-6f) &&
                  near_v(vam_cut_undo(cut[i].t, wrist, k), model[i].t, 2e-5f),
                  "cut %.1f: bone %d moved toward the wrist (rotation kept, undo restores it)", amount, i);
        /* skin: a forearm vertex (60% down the forearm, 3 cm off the axis) weighted to each forearm bone */
        const VamArmBind b = right_bind();
        const VrVec3 v = vr_add(vam_lerp(b.fore, b.hand, 0.6f), vr_v3(0.0f, 0.0f, 0.03f));
        for (int i = CUT_FORE; i <= CUT_TWIST; ++i) {
            const VrVec3 p0 = vam_skin_point(model[i], 1.0f, bind[i], v), p1 = vam_skin_point(cut[i], scale[i], bind[i], v);
            CHECK(near_v(p1, vam_lerp(wrist, p0, k), 1e-5f), "cut %.1f: a vertex on bone %d at wrist + %.2f (v - wrist) (%.4f m off)",
                  amount, i, k, vam_len(vr_sub(p1, vam_lerp(wrist, p0, k))));
            CHECK(near_f(vam_len(vr_sub(p1, wrist)), k * vam_len(vr_sub(p0, wrist)), 1e-5f), "cut %.1f: its distance to the wrist x %.2f", amount, k);
        }
        /* a wrist vertex half on the Hand, half on the Forearm: the hand half stays, the forearm half is pulled */
        const VrVec3 vw = vr_add(vam_lerp(b.fore, b.hand, 0.95f), vr_v3(0.0f, 0.02f, 0.0f));
        const VrVec3 h0 = vam_skin_point(model[CUT_HAND], 1.0f, bind[CUT_HAND], vw), h1 = vam_skin_point(cut[CUT_HAND], scale[CUT_HAND], bind[CUT_HAND], vw);
        const VrVec3 f0 = vam_skin_point(model[CUT_FORE], 1.0f, bind[CUT_FORE], vw), f1 = vam_skin_point(cut[CUT_FORE], scale[CUT_FORE], bind[CUT_FORE], vw);
        const VrVec3 blend0 = vam_lerp(h0, f0, 0.5f), blend1 = vam_lerp(h1, f1, 0.5f);
        CHECK(near_v(h1, h0, 1e-7f) && near_v(blend1, vam_lerp(h0, vam_lerp(wrist, f0, k), 0.5f), 1e-5f),
              "cut %.1f: a 50/50 wrist vertex keeps its hand half, its forearm half pulled", amount);
        if (c == 0)
            printf("     cut 0.8: a forearm vertex %.1f cm from the wrist -> %.1f cm; a 50/50 wrist vertex moves %.1f mm\n",
                   vam_len(vr_sub(vam_skin_point(model[CUT_FORE], 1.0f, bind[CUT_FORE], v), wrist)) * 100.0f,
                   vam_len(vr_sub(vam_skin_point(cut[CUT_FORE], scale[CUT_FORE], bind[CUT_FORE], v), wrist)) * 100.0f,
                   vam_len(vr_sub(blend1, blend0)) * 1000.0f);
    }
    printf("PASS wrist cut\n");
}

int main(void) {
    test_math();
    test_two_bone();
    test_arm();
    test_spaghetti();
    test_mirror();
    test_hand();
    test_game_mapping();
    test_weight();
    test_body_follow();
    test_roll_detach();
    test_roll_lag();
    test_lead_jitter();
    test_head_pick();
    test_cut();
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("All VR arm tests passed (%d checks)\n", checks);
    return 0;
}
