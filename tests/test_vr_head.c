/* Unit tests for src/vr_head_math.h (VR head tracking math, docs/vr/PLAN.md Phase 2).
 *
 * The camera conventions under test (findings/08): the game camera is four float4 rows right,
 * up, forward, position; left-handed (+Z forward), world +Y up. OpenXR is right-handed, -Z
 * forward. Build: ninja -C out/gpu vr-head-test (no GPU, no game). Prints PASS lines; exit 0. */
#include "vr_head_math.h"
#include "vr_fp_math.h"
#include <stdio.h>

static int failures, checks;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }
static int near_v(VrVec3 a, VrVec3 b, float eps) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }

/* A game camera with heading h (radians, toward +X positive) and pitch p (up positive). */
static void game_cam(float m[4][4], float h, float p, VrVec3 pos) {
    const VrVec3 f = vr_v3(sinf(h) * cosf(p), sinf(p), cosf(h) * cosf(p));
    const VrVec3 u = vr_v3(-sinf(h) * sinf(p), cosf(p), -cosf(h) * sinf(p));
    const VrVec3 r = vr_cross(u, f);
    const VrVec3 rows[4] = {r, u, f, pos};
    for (int i = 0; i < 4; ++i) {
        m[i][0] = rows[i].x; m[i][1] = rows[i].y; m[i][2] = rows[i].z; m[i][3] = i == 3 ? 1.0f : 0.0f;
    }
}

static void test_identity(void) {
    float m[4][4], ref[4][4];
    game_cam(m, 0.7f, -0.2f, vr_v3(10, 2, -3));
    memcpy(ref, m, sizeof(m));
    vr_cam_compose(m, vr_head_rel_game(vr_qidentity(), vr_qidentity()), vr_v3(0, 0, 0), 0);
    int same = 1;
    for (int i = 0; i < 16; ++i) same &= near(m[i / 4][i % 4], ref[i / 4][i % 4], 1e-6f);
    CHECK(same, "identity head must leave the camera unchanged");
    printf("PASS identity\n");
}

static void test_directions(void) {
    float m[4][4];
    /* Camera looking along world +Z. Head turned 90 deg left -> view along -X (left). */
    game_cam(m, 0.0f, 0.0f, vr_v3(0, 0, 0));
    vr_cam_compose(m, vr_head_rel_game(vr_xr_ypr(90 * VR_DEG, 0, 0), vr_qidentity()), vr_v3(0, 0, 0), 0);
    CHECK(near_v(vr_row(m, 2), vr_v3(-1, 0, 0), 1e-5f), "yaw left: fwd (%f %f %f)", m[2][0], m[2][1], m[2][2]);
    CHECK(near_v(vr_row(m, 0), vr_v3(0, 0, 1), 1e-5f), "yaw left: right (%f %f %f)", m[0][0], m[0][1], m[0][2]);
    /* Head up 30 deg -> forward rises. */
    game_cam(m, 0.0f, 0.0f, vr_v3(0, 0, 0));
    vr_cam_compose(m, vr_head_rel_game(vr_xr_ypr(0, 30 * VR_DEG, 0), vr_qidentity()), vr_v3(0, 0, 0), 0);
    CHECK(near_v(vr_row(m, 2), vr_v3(0, 0.5f, 0.8660254f), 1e-5f), "pitch up: fwd (%f %f %f)", m[2][0], m[2][1], m[2][2]);
    /* Head tilted left 20 deg -> the right vector rises. */
    game_cam(m, 0.0f, 0.0f, vr_v3(0, 0, 0));
    vr_cam_compose(m, vr_head_rel_game(vr_xr_ypr(0, 0, 20 * VR_DEG), vr_qidentity()), vr_v3(0, 0, 0), 0);
    CHECK(near_v(vr_row(m, 0), vr_v3(cosf(20 * VR_DEG), sinf(20 * VR_DEG), 0), 1e-5f), "roll: right (%f %f %f)", m[0][0], m[0][1], m[0][2]);
    CHECK(near_v(vr_row(m, 2), vr_v3(0, 0, 1), 1e-5f), "roll keeps the view axis");
    /* Head turned 30 deg right on a camera already heading 90 deg right (+X): heading 120. */
    game_cam(m, 90 * VR_DEG, 0.0f, vr_v3(5, 1, 5));
    vr_cam_compose(m, vr_head_rel_game(vr_xr_ypr(-30 * VR_DEG, 0, 0), vr_qidentity()), vr_v3(0, 0, 0), 0);
    CHECK(near(vr_cam_heading(m), 120 * VR_DEG, 1e-4f), "headings add: %f", vr_cam_heading(m) / VR_DEG);
    CHECK(near_v(vr_row(m, 3), vr_v3(5, 1, 5), 1e-6f), "rotation keeps the position");
    printf("PASS directions\n");
}

static void test_camera_frame_vs_level(void) {
    float m[4][4];
    /* Game camera pitched 25 deg down; the head looks level and straight. */
    game_cam(m, 40 * VR_DEG, -25 * VR_DEG, vr_v3(0, 0, 0));
    float local[4][4], level[4][4];
    memcpy(local, m, sizeof(m)); memcpy(level, m, sizeof(m));
    const VrQuat rel = vr_head_rel_game(vr_qidentity(), vr_qidentity());
    vr_cam_compose(local, rel, vr_v3(0, 0, 0), 0);
    vr_cam_compose(level, rel, vr_v3(0, 0, 0), 1);
    CHECK(near(local[2][1], sinf(-25 * VR_DEG), 1e-5f), "camera frame keeps the game's pitch");
    CHECK(near(level[2][1], 0.0f, 1e-6f) && near(level[1][1], 1.0f, 1e-6f), "level: horizon level");
    CHECK(near(vr_cam_heading(level), 40 * VR_DEG, 1e-5f), "level keeps the heading");
    /* Level mode with the head pitched 10 deg up: exactly 10 deg up, whatever the game's pitch. */
    memcpy(level, m, sizeof(m));
    vr_cam_compose(level, vr_head_rel_game(vr_xr_ypr(0, 10 * VR_DEG, 0), vr_qidentity()), vr_v3(0, 0, 0), 1);
    CHECK(near(level[2][1], sinf(10 * VR_DEG), 1e-5f), "level: the head alone sets the pitch");
    /* Camera frame: the head's pitch adds to the game's: -25 + 10. */
    memcpy(local, m, sizeof(m));
    vr_cam_compose(local, vr_head_rel_game(vr_xr_ypr(0, 10 * VR_DEG, 0), vr_qidentity()), vr_v3(0, 0, 0), 0);
    CHECK(near(local[2][1], sinf(-15 * VR_DEG), 1e-5f), "camera frame: pitches add");
    printf("PASS camera frame vs level\n");
}

static void test_orthonormal(void) {
    unsigned seed = 12345;
    float worst = 0.0f, det_min = 2.0f;
    for (int k = 0; k < 2000; ++k) {
        float a[7];
        for (int i = 0; i < 7; ++i) { seed = seed * 1103515245u + 12345u; a[i] = ((seed >> 8) & 0xffff) / 65535.0f * 2.0f - 1.0f; }
        float m[4][4];
        game_cam(m, a[0] * VR_PI, a[1] * 1.4f, vr_v3(a[2] * 100, a[3] * 10, a[4] * 100));
        const VrQuat head = vr_qnormalize(vr_q(a[5], a[6], a[0] * a[1], 0.5f + fabsf(a[2])));
        vr_cam_compose(m, vr_head_rel_game(head, vr_xr_yaw_only(vr_xr_ypr(a[3], 0, 0))), vr_v3(0, 0, 0), k & 1);
        float det;
        const float e = vr_cam_ortho_error(m, &det);
        worst = e > worst ? e : worst;
        det_min = det < det_min ? det : det_min;
        CHECK(near(m[3][3], 1.0f, 0.0f) && m[0][3] == 0.0f, "w lanes kept");
    }
    CHECK(worst < 1e-5f, "orthonormal: worst %g", worst);
    CHECK(det_min > 0.9999f, "handedness kept: det %f", det_min);
    printf("PASS orthonormal (worst %.2g)\n", worst);
}

static void test_reference(void) {
    /* Head with yaw 40, pitch 20, roll 10 deg. */
    const VrQuat head = vr_xr_ypr(40 * VR_DEG, 20 * VR_DEG, 10 * VR_DEG);
    float yaw, pitch, roll;
    vr_xr_angles(head, &yaw, &pitch, &roll);
    CHECK(near(yaw, 40 * VR_DEG, 1e-4f) && near(pitch, 20 * VR_DEG, 1e-4f) && near(roll, 10 * VR_DEG, 1e-4f),
          "angles %f %f %f", yaw / VR_DEG, pitch / VR_DEG, roll / VR_DEG);
    CHECK(near(vr_xr_yaw(head), 40 * VR_DEG, 1e-4f), "yaw %f", vr_xr_yaw(head) / VR_DEG);
    const VrQuat ref = vr_xr_yaw_only(head);
    /* Relative to its own yaw the head keeps pitch and roll but has no yaw. */
    vr_xr_angles(vr_qmul(vr_qconj(ref), head), &yaw, &pitch, &roll);
    CHECK(near(yaw, 0.0f, 1e-4f) && near(pitch, 20 * VR_DEG, 1e-4f) && near(roll, 10 * VR_DEG, 1e-4f), "recentered");
    /* Straight down: the heading comes from the up vector. */
    CHECK(near(vr_xr_yaw(vr_xr_ypr(70 * VR_DEG, -90 * VR_DEG, 0)), 70 * VR_DEG, 1e-3f), "yaw at the pole");
    /* A screen placed at yaw 30 deg (PlaceScreen's yaw-only quaternion) as the reference: the head
     * turned to 50 deg is 20 deg left of forward. */
    const VrQuat screen = vr_qaxis(vr_v3(0, 1, 0), 30 * VR_DEG);
    float m[4][4];
    game_cam(m, 0.0f, 0.0f, vr_v3(0, 0, 0));
    vr_cam_compose(m, vr_head_rel_game(vr_xr_ypr(50 * VR_DEG, 0, 0), vr_xr_yaw_only(screen)), vr_v3(0, 0, 0), 0);
    CHECK(near(vr_cam_heading(m), -20 * VR_DEG, 1e-4f), "screen reference: %f", vr_cam_heading(m) / VR_DEG);
    printf("PASS reference\n");
}

static void test_offset(void) {
    /* Reference facing yaw 90 (left, -X in XR); the head moved 0.1 m along XR -X = forward. */
    const VrQuat ref = vr_qaxis(vr_v3(0, 1, 0), 90 * VR_DEG);
    VrVec3 d = vr_head_offset_game(vr_v3(-0.1f, 0.05f, 0), vr_v3(0, 0, 0), ref, 1.0f, 0.25f, 0.15f);
    CHECK(near_v(d, vr_v3(0, 0.05f, 0.1f), 1e-6f), "offset forward/up: %f %f %f", d.x, d.y, d.z);
    d = vr_head_offset_game(vr_v3(0, 1.0f, 0.6f), vr_v3(0, 0, 0), vr_qidentity(), 1.0f, 0.25f, 0.15f);
    CHECK(near_v(d, vr_v3(0, 0.15f, -0.25f), 1e-6f), "clamped: %f %f %f", d.x, d.y, d.z);
    float m[4][4];
    game_cam(m, 90 * VR_DEG, -30 * VR_DEG, vr_v3(1, 2, 3));
    vr_cam_compose(m, vr_qidentity(), vr_v3(0.1f, 0.2f, 0.3f), 0);
    /* Along the level basis of heading +X: right = -Z, up = +Y, forward = +X. */
    CHECK(near_v(vr_row(m, 3), vr_v3(1.3f, 2.2f, 2.9f), 1e-5f), "offset in the level basis: %f %f %f", m[3][0], m[3][1], m[3][2]);
    printf("PASS offset\n");
}

static void test_fov(void) {
    /* Mirror-asymmetric eyes (Quest-like), parallel: the union is the outer edges. */
    const VrQuat id[2] = {vr_qidentity(), vr_qidentity()};
    const VrFov quest[2] = {{-0.9f, 0.7f, 0.8f, -0.85f}, {-0.7f, 0.9f, 0.8f, -0.85f}};
    float tx, ty;
    CHECK(vr_union_tangents(id, quest, &tx, &ty), "union");
    CHECK(near(tx, tanf(0.9f), 1e-5f) && near(ty, tanf(0.85f), 1e-5f), "union tangents %f %f", tx, ty);
    float fov_y, aspect;
    vr_fov_from_tangents(tx, ty, &fov_y, &aspect);
    CHECK(near(fov_y, 1.7f, 1e-5f) && near(aspect, tanf(0.9f) / tanf(0.85f), 1e-5f), "fov %f aspect %f", fov_y, aspect);
    /* Canted 10 deg outward each (Pimax-like): the horizontal half-extent is the outer edge
     * rotated by the cant, the vertical one grows at the outer corners. */
    const float cant = 10 * VR_DEG;
    const VrQuat canted[2] = {vr_qaxis(vr_v3(0, 1, 0), cant), vr_qaxis(vr_v3(0, 1, 0), -cant)};
    const VrFov sym[2] = {{-0.8f, 0.8f, 0.8f, -0.8f}, {-0.8f, 0.8f, 0.8f, -0.8f}};
    {
        float ox = 1.0f, oy = 2.0f;
        vr_overscan_tangents(&ox, &oy, 0.1f, 0.15f);
        CHECK(near(ox, 1.1f, 1e-6f) && near(oy, 2.3f, 1e-6f), "overscan %f %f", ox, oy);
        vr_overscan_tangents(&ox, &oy, -1.0f, 5.0f);
        CHECK(near(ox, 1.1f, 1e-6f) && near(oy, 4.6f, 1e-6f), "overscan clamp %f %f", ox, oy);
    }
    CHECK(vr_union_tangents(canted, sym, &tx, &ty), "canted union");
    CHECK(near(tx, tanf(0.8f + cant), 1e-4f), "canted tan_x %f expected %f", tx, tanf(0.8f + cant));
    /* outer top corner: direction (-tan .8, tan .8, -1) rotated by +cant about Y */
    const VrVec3 c = vr_qrotate(canted[0], vr_v3(-tanf(0.8f), tanf(0.8f), -1));
    CHECK(near(ty, c.y / -c.z, 1e-4f) && ty > tanf(0.8f), "canted tan_y %f expected %f", ty, c.y / -c.z);
    printf("PASS fov\n");
}

static void test_fake(void) {
    VrFakeHead f;
    CHECK(vr_fake_parse("30,-5", &f) && f.kind == VR_FAKE_FIXED && f.yaw == 30 && f.pitch == -5 && f.roll == 0, "fixed");
    CHECK(vr_fake_parse("10 2 3 0.1 0.2 0.3", &f) && f.roll == 3 && near(f.z, 0.3f, 1e-6f), "six values");
    CHECK(vr_fake_parse("sweep", &f) && f.kind == VR_FAKE_SWEEP && f.amp_yaw == 30 && f.period == 8, "sweep default");
    CHECK(vr_fake_parse("sweep:20,5,4", &f) && f.amp_yaw == 20 && f.amp_pitch == 5 && f.period == 4, "sweep values");
    CHECK(vr_fake_parse("@C:/tmp/head.txt", &f) && f.kind == VR_FAKE_FILE && !strcmp(f.path, "C:/tmp/head.txt"), "file");
    CHECK(!vr_fake_parse("sideways", &f) && !vr_fake_parse("sweepy", &f) && !vr_fake_parse("", &f) && !vr_fake_parse("1,2,3,4,5,6,7", &f), "garbage rejected");
    vr_fake_parse("30,0", &f);
    VrVec3 p;
    float yaw, pitch, roll;
    vr_xr_angles(vr_fake_pose(&f, 0.0, &p), &yaw, &pitch, &roll);
    CHECK(near(yaw, 30 * VR_DEG, 1e-5f) && near(pitch, 0, 1e-6f), "fixed pose");
    vr_fake_parse("sweep:20,0,4", &f);
    vr_xr_angles(vr_fake_pose(&f, 1.0, &p), &yaw, &pitch, &roll);
    CHECK(near(yaw, 20 * VR_DEG, 1e-4f), "sweep peak at a quarter period: %f", yaw / VR_DEG);
    VrFakeFile ff;
    vr_fake_file_parse("yaw=30 apply=1 fov=100\n", &ff);
    CHECK(ff.v[0] == 30 && ff.apply == 1 && ff.fov_deg == 100 && ff.level == -1 && ff.aspect == 0 && ff.lod == 1, "file keys");
    vr_fake_file_parse("fov=100 lod=0", &ff);
    CHECK(ff.fov_deg == 100 && ff.lod == 0, "file lod=0");
    vr_fake_file_parse("15 -4 level=1 junk=? apply=2", &ff);
    CHECK(ff.v[0] == 15 && ff.v[1] == -4 && ff.level == 1 && ff.apply == 2, "file bare numbers");
    vr_fake_file_parse("", &ff);
    CHECK(ff.apply == 3 && ff.v[0] == 0 && ff.fov_deg == 0, "empty file: neutral, both hooks");
    printf("PASS fake head\n");
}

/* First person (src/vr_fp_math.h, findings/16). */
static void test_fp(void) {
    /* Havok transforms: compose and invert. */
    const VrXform parent = vr_xf(vr_v3(0, 1, 0), vr_qaxis(vr_v3(0, 1, 0), 90 * VR_DEG));
    const VrXform local = vr_xf(vr_v3(0, 0, 1), vr_qidentity());
    const VrXform model = vr_xf_mul(parent, local);
    CHECK(near_v(model.t, vr_v3(1, 1, 0), 1e-5f), "compose: %f %f %f", model.t.x, model.t.y, model.t.z);
    const VrVec3 back = vr_xf_apply(vr_xf_inverse(model), model.t);
    CHECK(near_v(back, vr_v3(0, 0, 0), 1e-5f), "inverse");
    const float qs[12] = {1, 2, 3, 0, 0, 0, 0, 1, 1, 1, 1, 0};
    VrXform x;
    CHECK(vr_xf_from_qs(qs, &x) && near_v(x.t, vr_v3(1, 2, 3), 0), "hkQsTransform");
    const float bad[12] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0};
    CHECK(!vr_xf_from_qs(bad, &x), "zero rotation rejected");
    /* Model -> world (measured, findings/16): the physics yaw is the heading of model +Z, the face
     * (model -Z) looks along yaw + 180 deg; model +X is the character's left. */
    const float yaw = 30 * VR_DEG;
    const VrVec3 pos = vr_v3(10, 2, -4);
    const float facing = vr_facing_heading(yaw);
    CHECK(near(facing, -150 * VR_DEG, 1e-5f), "facing heading %f", facing / VR_DEG);
    const VrVec3 face = vr_sub(vr_model_to_world(pos, yaw, vr_v3(0, 0, -1)), pos);
    CHECK(near_v(face, vr_v3(sinf(facing), 0, cosf(facing)), 1e-5f), "face along the facing heading");
    const VrVec3 left = vr_sub(vr_model_to_world(pos, yaw, vr_v3(1, 0, 0)), pos);
    CHECK(near_v(left, vr_v3(-cosf(facing), 0, sinf(facing)), 1e-5f), "model +X = left of the facing heading");
    /* The run1 numbers: yaw -27.9 deg while running along 152.1 deg. */
    CHECK(near(vr_facing_heading(-27.9f * VR_DEG), 152.1f * VR_DEG, 1e-4f), "measured facing");
    const VrVec3 p = vr_v3(0.3f, 1.6f, -0.2f);
    CHECK(near_v(vr_world_to_model(pos, yaw, vr_model_to_world(pos, yaw, p)), p, 1e-5f), "world <-> model");
    /* A heading's camera has that heading and the game cameras' handedness. */
    float m[4][4];
    vr_cam_from_heading(m, -70 * VR_DEG, vr_v3(1, 2, 3));
    float det;
    CHECK(vr_cam_ortho_error((const float (*)[4])m, &det) < 1e-6f && det > 0.999f, "heading camera orthonormal");
    CHECK(near(vr_cam_heading((const float (*)[4])m), -70 * VR_DEG, 1e-5f), "heading camera heading");
    CHECK(near(vr_wrap_pi(190 * VR_DEG), -170 * VR_DEG, 1e-4f) && near(vr_wrap_pi(-190 * VR_DEG), 170 * VR_DEG, 1e-4f), "wrap");
    /* Turning the game camera about the player: heading +d, pitch kept, orbit at the same distance. */
    float g[4][4];
    game_cam(g, 150 * VR_DEG, -20 * VR_DEG, vr_v3(-13.3f, -5.1f, -12.4f));
    const VrVec3 player = vr_v3(-11.6f, -6.6f, -15.7f);
    const float r0 = hypotf(g[3][0] - player.x, g[3][2] - player.z);
    vr_cam_rotate_heading(g, 30 * VR_DEG, player);
    CHECK(near(vr_cam_heading((const float (*)[4])g), -180 * VR_DEG, 1e-4f) || near(vr_cam_heading((const float (*)[4])g), 180 * VR_DEG, 1e-4f),
          "rotated heading %f", vr_cam_heading((const float (*)[4])g) / VR_DEG);
    CHECK(near(g[2][1], sinf(-20 * VR_DEG), 1e-5f), "pitch kept");
    CHECK(near(hypotf(g[3][0] - player.x, g[3][2] - player.z), r0, 1e-4f) && near(g[3][1], -5.1f, 0), "orbits the player");
    CHECK(vr_cam_ortho_error((const float (*)[4])g, &det) < 1e-5f && det > 0.999f, "still orthonormal");
    /* The head's yaw in the heading's sense: an OpenXR head turned left is a negative heading. */
    const VrQuat rel = vr_head_rel_game(vr_xr_ypr(25 * VR_DEG, -40 * VR_DEG, 5 * VR_DEG), vr_qidentity());
    CHECK(near(vr_rel_heading(rel), -25 * VR_DEG, 1e-3f), "head heading %f", vr_rel_heading(rel) / VR_DEG);
    /* It matches where the composition points a level camera's view axis. */
    vr_cam_from_heading(m, 0.0f, vr_v3(0, 0, 0));
    vr_cam_compose(m, rel, vr_v3(0, 0, 0), 1);
    CHECK(near(atan2f(m[2][0], m[2][2]), vr_rel_heading(rel), 1e-4f), "head heading = composed view axis");
    /* Looking straight down: the up vector gives it. */
    const VrQuat down = vr_head_rel_game(vr_xr_ypr(-60 * VR_DEG, -90 * VR_DEG, 0), vr_qidentity());
    CHECK(near(vr_rel_heading(down), 60 * VR_DEG, 1e-3f), "straight down: %f", vr_rel_heading(down) / VR_DEG);
    /* Smoothing: converges, stays within max_dev of the raw value. */
    VrSmooth s = {0, {0, 0, 0}};
    VrVec3 v = vr_smooth_step(&s, vr_v3(0, 1.6f, 0), 0.016f, 0.12f, 0.08f);
    CHECK(near_v(v, vr_v3(0, 1.6f, 0), 0), "first sample passes");
    v = vr_smooth_step(&s, vr_v3(0, 0.9f, 0), 0.016f, 0.12f, 0.08f);
    CHECK(near(v.y, 0.98f, 1e-5f), "a 70 cm drop is followed within 8 cm: %f", v.y);
    for (int i = 0; i < 200; ++i) v = vr_smooth_step(&s, vr_v3(0, 0.9f, 0), 0.016f, 0.12f, 0.08f);
    CHECK(near(v.y, 0.9f, 1e-4f), "converges: %f", v.y);
    v = vr_smooth_step(&s, vr_v3(0, 0.92f, 0), 0.016f, 0.12f, 0.08f);
    CHECK(v.y > 0.9f && v.y < 0.92f, "small moves are smoothed: %f", v.y);
    /* Snap and smooth turn: tests/test_vr_turn.c (src/vr_turn_math.h). */
    /* Stick remap: "up" turned 90 degrees right is "right"; the magnitude is kept. */
    uint8_t sx = 128, sy = 0;
    vr_stick_rotate(&sx, &sy, 90 * VR_DEG);
    CHECK(sx == 255 && sy == 128, "up -> right: %u %u", sx, sy);
    sx = 128; sy = 0;
    vr_stick_rotate(&sx, &sy, -90 * VR_DEG);
    CHECK(sx == 0 && sy == 128, "up -> left: %u %u", sx, sy);
    sx = 128; sy = 128;
    vr_stick_rotate(&sx, &sy, 45 * VR_DEG);
    CHECK(sx == 128 && sy == 128, "centre stays");
    printf("PASS first person\n");
}

/* Real-time cutscenes (src/vr_fp_math.h vr_cutscene_raw / vr_cutscene_update). */
static void test_cutscene(void) {
    float a[4][4], b[4][4];
    vr_cam_from_heading(a, 30 * VR_DEG, vr_v3(10, 2, -5));
    memcpy(b, a, sizeof(b));
    CHECK(vr_cam_same_view(a, b, 0.02f, 0.5f), "same matrix: same view");
    b[3][0] += 0.4f;
    CHECK(vr_cam_same_view(a, b, 0.02f, 0.5f), "0.4 m apart within 0.5 m");
    b[3][0] += 0.2f;
    CHECK(!vr_cam_same_view(a, b, 0.02f, 0.5f), "0.6 m apart: another view");
    vr_cam_from_heading(b, 33 * VR_DEG, vr_v3(10, 2, -5));
    CHECK(!vr_cam_same_view(a, b, 0.02f, 0.5f), "3 deg of heading: another view");
    /* raw: the field camera is not the game's (debug camera, Remo slot), or a Remo plays with its camera */
    CHECK(vr_cutscene_raw(0, 0, 0), "not the field camera: cutscene");
    CHECK(!vr_cutscene_raw(1, 0, 0), "field camera, no Remo: gameplay");
    CHECK(vr_cutscene_raw(1, 2, 1) && vr_cutscene_raw(1, 1, 1), "Remo playing with the game's camera");
    CHECK(!vr_cutscene_raw(1, 3, 0), "a Remo flag but the gameplay camera: no cutscene");
    CHECK(!vr_cutscene_raw(1, 0x10, 1), "other Remo flags do not count");
    /* hysteresis: on after 3 frames, off after 20, single frames ignored */
    VrCutsceneLatch l = {0, 0};
    int on_at = -1;
    for (int i = 0; i < 10 && on_at < 0; ++i)
        if (vr_cutscene_update(&l, 1)) on_at = i;
    CHECK(on_at == (int)VR_CUTSCENE_ON - 1, "on after %u frames (at %d)", VR_CUTSCENE_ON, on_at);
    CHECK(vr_cutscene_update(&l, 0) && vr_cutscene_update(&l, 1), "one gameplay frame keeps the cinema screen");
    int off_at = -1;
    for (int i = 0; i < 40 && off_at < 0; ++i)
        if (!vr_cutscene_update(&l, 0)) off_at = i;
    CHECK(off_at == (int)VR_CUTSCENE_OFF - 1, "off after %u frames (at %d)", VR_CUTSCENE_OFF, off_at);
    CHECK(!vr_cutscene_update(&l, 1) && !vr_cutscene_update(&l, 0) && !vr_cutscene_update(&l, 1) &&
          !vr_cutscene_update(&l, 1), "flicker does not enter");
}

int main(void) {
    test_cutscene();
    test_fp();
    test_identity();
    test_directions();
    test_camera_frame_vs_level();
    test_orthonormal();
    test_reference();
    test_offset();
    test_fov();
    test_fake();
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("All VR head tests passed (%d checks)\n", checks);
    return 0;
}
