/* Unit tests for src/vr_hitbox_math.h, the placement of the game's hitbox debug overlay
 * (docs/vr/findings/29-game-hitboxes.md). Build: ninja -C out/gpu vr-hitbox-test (no GPU, no game).
 * Prints PASS; exit 0. */
#include "vr_arm_math.h"
#include "vr_hitbox_math.h"
#include <stdio.h>

static int failures, checks;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

static int near_v(VrVec3 a, VrVec3 b, float eps) { return vam_len(vr_sub(a, b)) <= eps; }

static VhbView some_view(void) {
    VhbView v;
    v.cam_pos = vr_v3(0.05f, 1.62f, -0.12f);
    v.cam_rot = vr_qnormalize(vr_qmul(vr_qaxis(vr_v3(0, 1, 0), 2.6f), vr_qaxis(vr_v3(1, 0, 0), -0.3f)));
    v.head_pos = vr_v3(0.3f, 1.7f, -0.4f);
    v.head_rot = vr_xr_ypr(0.4f, -0.2f, 0.1f);
    v.body_pos = vr_v3(-120.5f, 4.25f, 33.0f);
    v.body_yaw = 1.1f;
    return v;
}

/* The arms' way (controller -> model space) undone: a model point sent to the headset and brought
 * back through vam_xr_in_head_game + the camera lands where it started. */
static void test_inverse_of_arms(void) {
    const VhbView v = some_view();
    const VrVec3 pts[3] = {vr_v3(0, 0, 0), vr_v3(-0.6f, 1.0f, -0.3f), vr_v3(0.2f, 1.9f, 0.5f)};
    for (int i = 0; i < 3; ++i) {
        const VrVec3 x = vhb_model_to_xr(&v, pts[i]);
        VrVec3 c;
        VrQuat q;
        vam_xr_in_head_game(v.head_pos, v.head_rot, x, vr_qidentity(), &c, &q);
        const VrVec3 back = vr_add(v.cam_pos, vr_qrotate(v.cam_rot, c));
        CHECK(near_v(back, pts[i], 1e-4f), "point %d: back (%f %f %f)", i, back.x, back.y, back.z);
    }
    /* the camera itself is at the head */
    CHECK(near_v(vhb_model_to_xr(&v, v.cam_pos), v.head_pos, 1e-5f), "camera -> head");
    /* the camera's forward (+Z) is the head's forward (OpenXR -Z) */
    const VrVec3 fwd = vhb_model_dir_to_xr(&v, vr_qrotate(v.cam_rot, vr_v3(0, 0, 1)));
    CHECK(near_v(fwd, vr_qrotate(v.head_rot, vr_v3(0, 0, -1)), 1e-5f), "forward");
    const VrVec3 up = vhb_model_dir_to_xr(&v, vr_qrotate(v.cam_rot, vr_v3(0, 1, 0)));
    CHECK(near_v(up, vr_qrotate(v.head_rot, vr_v3(0, 1, 0)), 1e-5f), "up");
}

/* The affine matrix: a data point (X mirrored) through a bone equals the point-by-point way. */
static void test_affine(void) {
    const VhbView v = some_view();
    const VrXform bone = vr_xf(vr_v3(-0.58f, 1.0f, 0.02f), vr_qnormalize(vr_q(0.2f, -0.5f, 0.3f, 0.8f)));
    float m[12];
    vhb_affine(&v, bone, m);
    CHECK(fabsf(vhb_det(m) - 1.0f) < 1e-4f, "det %f (the two mirrors cancel)", vhb_det(m));
    const VrVec3 data[3] = {vr_v3(0.1496f, 0.0782f, -0.0041f), vr_v3(0.1494f, -0.4187f, -0.0038f), vr_v3(0, 0, 0)};
    for (int i = 0; i < 3; ++i) {
        const VrVec3 want = vhb_model_to_xr(&v, vr_xf_apply(bone, vhb_mirror_x(data[i])));
        CHECK(near_v(vhb_apply(m, data[i]), want, 1e-5f), "data point %d", i);
    }
    /* lengths are kept (rigid) */
    const float len = vam_len(vr_sub(vhb_apply(m, data[0]), vhb_apply(m, data[1])));
    CHECK(fabsf(len - vam_len(vr_sub(data[0], data[1]))) < 1e-5f, "length %f", len);
}

/* Skinning: at the bind pose a bind-space point stays where the data says; a bone moved by T moves
 * the point by T. */
static void test_skin(void) {
    const VhbView v = some_view();
    const VrXform bind = vr_xf(vr_v3(0.11f, 0.66f, -0.01f), vr_qnormalize(vr_q(0.1f, 0.7f, -0.1f, 0.7f)));
    const VrVec3 a_data = vr_v3(-0.1121f, 0.6568f, -0.0065f); /* X mirrored */
    float m[12];
    vhb_affine(&v, vhb_skin(bind, bind), m);
    CHECK(near_v(vhb_apply(m, a_data), vhb_model_to_xr(&v, vhb_mirror_x(a_data)), 1e-5f), "bind pose");
    const VrXform turn = vr_xf(vr_v3(0.0f, 0.1f, -0.2f), vr_qaxis(vr_v3(0, 1, 0), 0.5f));
    const VrXform now = vr_xf_mul(turn, bind);
    vhb_affine(&v, vhb_skin(now, bind), m);
    const VrVec3 want = vhb_model_to_xr(&v, vr_xf_apply(turn, vhb_mirror_x(a_data)));
    CHECK(near_v(vhb_apply(m, a_data), want, 1e-5f), "moved bone");
}

/* World points go through the character's model space. */
static void test_world(void) {
    const VhbView v = some_view();
    const VrVec3 p = vr_v3(0.3f, 1.2f, -0.8f);
    const VrVec3 w = vr_model_to_world(v.body_pos, v.body_yaw, p);
    CHECK(near_v(vhb_world_to_xr(&v, w), vhb_model_to_xr(&v, p), 1e-4f), "world == model");
}

static void test_hash(void) {
    CHECK(vhb_hash("") == 2166136261u, "empty");
    CHECK(vhb_hash("a") == 0xe40c292cu, "a: %08x", vhb_hash("a"));
    CHECK(vhb_hash("R_Weapon") != vhb_hash("L_Weapon"), "distinct");
}

static void test_finite(void) {
    float ok[3] = {1, 2, 3}, bad[3] = {1, NAN, 3}, huge[3] = {1, 2e6f, 3};
    CHECK(vhb_finite(ok, 3) && !vhb_finite(bad, 3) && !vhb_finite(huge, 3), "finite");
}

int main(void) {
    test_inverse_of_arms();
    test_affine();
    test_skin();
    test_world();
    test_hash();
    test_finite();
    if (failures) {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("PASS vr_hitbox_math (%d checks)\n", checks);
    return 0;
}
