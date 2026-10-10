/* Unit tests for src/vr_gun_math.h, the VR gun aim's direction math (src/runtime_vr_gun.c,
 * docs/vr/findings/27-gun-aim.md): the controller ray in the game world and the launch redirect.
 * Build: ninja -C out/gpu vr-gun-test (no GPU, no game). Prints PASS lines; exit 0. */
#include "vr_gun_math.h"
#include <stdio.h>

static int failures, checks;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

static int near_v(VrVec3 a, VrVec3 b, float eps) { return vam_len(vr_sub(a, b)) <= eps; }
static int near_f(float a, float b, float eps) { return fabsf(a - b) <= eps; }
static void set_rows(float rows[3][4], VrVec3 r, VrVec3 u, VrVec3 f) {
    const VrVec3 v[3] = {r, u, f};
    for (int i = 0; i < 3; ++i) {
        rows[i][0] = v[i].x;
        rows[i][1] = v[i].y;
        rows[i][2] = v[i].z;
        rows[i][3] = 0.0f;
    }
}
/* A launch matrix (rows) from axes and a position. */
static void set_launch(float m[3][4], VrVec3 x, VrVec3 y, VrVec3 z, VrVec3 p) {
    const VrVec3 c[4] = {x, y, z, p};
    for (int col = 0; col < 4; ++col) {
        m[0][col] = c[col].x;
        m[1][col] = c[col].y;
        m[2][col] = c[col].z;
    }
}

static void test_aim_in_world(void) {
    float rows[3][4];
    set_rows(rows, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1));
    const VrQuat id = vr_qidentity();
    /* pointing straight ahead of the head: the camera's forward */
    CHECK(near_v(vg_aim_in_world(id, id, (const float (*)[4])rows), vr_v3(0, 0, 1), 1e-5f), "ahead = camera forward");
    /* the camera turned to world +X: the same ray follows it */
    float turned[3][4];
    set_rows(turned, vr_v3(0, 0, -1), vr_v3(0, 1, 0), vr_v3(1, 0, 0));
    CHECK(near_v(vg_aim_in_world(id, id, (const float (*)[4])turned), vr_v3(1, 0, 0), 1e-5f), "turned camera");
    /* the head and the controller turned together (the camera shows that head): still ahead */
    const VrQuat yaw = vr_qaxis(vr_v3(0, 1, 0), 70.0f * VR_DEG);
    CHECK(near_v(vg_aim_in_world(yaw, yaw, (const float (*)[4])rows), vr_v3(0, 0, 1), 1e-5f), "relative to the head");
    /* the controller pitched 30 deg up (OpenXR +X rotation lifts -Z) */
    const VrQuat up = vr_qaxis(vr_v3(1, 0, 0), 30.0f * VR_DEG);
    float y, p;
    vg_yaw_pitch(vg_aim_in_world(id, up, (const float (*)[4])rows), &y, &p);
    CHECK(near_f(p, 30.0f, 1e-3f) && near_f(y, 0.0f, 1e-3f), "pitch up: yaw %f pitch %f", y, p);
    /* the controller turned 90 deg left (OpenXR yaw is left positive): the camera's left (-right) */
    const VrQuat left = vr_qaxis(vr_v3(0, 1, 0), 90.0f * VR_DEG);
    CHECK(near_v(vg_aim_in_world(id, left, (const float (*)[4])rows), vr_v3(-1, 0, 0), 1e-5f), "left");
    CHECK(near_v(vg_aim_in_world(id, left, (const float (*)[4])turned), vr_v3(0, 0, 1), 1e-5f), "left of a turned camera");
    /* a grip used as the pointer, pitched up 20 deg */
    vg_yaw_pitch(vg_aim_in_world(id, vg_pitched(id, 20.0f), (const float (*)[4])rows), &y, &p);
    CHECK(near_f(p, 20.0f, 1e-3f), "grip pitch %f", p);
    /* degenerate rows: zero vector */
    float zero[3][4] = {{0}};
    CHECK(vam_len(vg_aim_in_world(id, id, (const float (*)[4])zero)) < 1e-6f, "degenerate");
    if (!failures) printf("PASS: the controller ray in the game world\n");
}

static void test_turn(void) {
    const VrVec3 dirs[] = {vr_v3(0, 0, 1), vr_v3(1, 0, 0), vam_norm(vr_v3(1, 0.4f, -2), vr_v3(0, 0, 1)),
                           vam_norm(vr_v3(-0.3f, -0.5f, 0.8f), vr_v3(0, 0, 1)), vr_v3(0, 0, -1), vr_v3(-1, 0, 0),
                           vr_v3(0, 1, 0)};
    const int n = (int)(sizeof(dirs) / sizeof(*dirs));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const VrQuat q = vg_turn(dirs[i], dirs[j]);
            CHECK(near_v(vr_qrotate(q, dirs[i]), dirs[j], 1e-4f), "turn %d -> %d", i, j);
            /* no roll: a horizontal side vector of a non-vertical `from` stays horizontal */
            if (fabsf(dirs[i].y) < 0.9f && fabsf(dirs[j].y) < 0.9f) {
                const VrVec3 side = vam_norm(vr_cross(vr_v3(0, 1, 0), dirs[i]), vr_v3(1, 0, 0));
                CHECK(fabsf(vr_qrotate(q, side).y) < 1e-4f, "level %d -> %d", i, j);
            }
        }
    if (!failures) printf("PASS: the level turn between two directions\n");
}

static void test_redirect(void) {
    const VrVec3 o = vr_v3(10, 2, -3);
    float m[3][4];
    /* the game's shot: along world +Z, the aim point 8.5 m ahead */
    set_launch(m, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), o);
    float t[3] = {o.x, o.y, o.z + 8.5f};
    const VrVec3 aim = vam_norm(vr_v3(1, 0.3f, 0.2f), vr_v3(0, 0, 1));
    int used = -1;
    VrVec3 before, after;
    const float turn = vg_redirect(m, t, aim, VG_REF_AUTO, 30.0f, &used, &before, &after);
    CHECK(used == 1, "aim point used");
    CHECK(near_v(before, vr_v3(0, 0, 1), 1e-5f), "old direction");
    CHECK(near_v(after, aim, 1e-4f), "new direction = the ray");
    CHECK(near_v(vg_launch_origin((const float (*)[4])m), o, 1e-6f), "muzzle kept");
    CHECK(near_v(vr_v3(t[0], t[1], t[2]), vr_add(o, vr_scale(aim, 8.5f)), 1e-3f), "aim point moved onto the ray");
    CHECK(near_f(turn, vg_angle_deg(vr_v3(0, 0, 1), aim), 1e-3f), "turn angle");
    /* the axes stay orthonormal */
    const VrVec3 x = vg_launch_axis((const float (*)[4])m, 0), y = vg_launch_axis((const float (*)[4])m, 1),
                 z = vg_launch_axis((const float (*)[4])m, 2);
    CHECK(near_f(vr_dot(x, y), 0, 1e-5f) && near_f(vr_dot(y, z), 0, 1e-5f) && near_f(vam_len(x), 1, 1e-5f), "orthonormal");
    CHECK(near_v(vr_cross(x, y), z, 1e-4f), "same handedness");

    /* a pellet 10 deg off the aim point keeps its 10 deg around the new ray */
    const VrQuat spread = vr_qaxis(vr_v3(0, 1, 0), 10.0f * VR_DEG);
    set_launch(m, vr_qrotate(spread, vr_v3(1, 0, 0)), vr_v3(0, 1, 0), vr_qrotate(spread, vr_v3(0, 0, 1)), o);
    t[0] = o.x; t[1] = o.y; t[2] = o.z + 8.5f;
    vg_redirect(m, t, aim, VG_REF_AUTO, 30.0f, &used, NULL, &after);
    CHECK(used == 1 && near_f(vg_angle_deg(after, aim), 10.0f, 1e-2f), "spread kept: %f", vg_angle_deg(after, aim));

    /* auto: an aim point far off the bullet's own direction is not the reference */
    set_launch(m, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), o);
    t[0] = o.x + 8.5f; t[1] = o.y; t[2] = o.z;
    vg_redirect(m, t, aim, VG_REF_AUTO, 30.0f, &used, NULL, &after);
    CHECK(used == 0 && near_v(after, aim, 1e-4f), "auto falls back to Z");
    /* target mode trusts the aim point anyway (the bullet keeps its 90 deg offset) */
    set_launch(m, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), o);
    t[0] = o.x + 8.5f; t[1] = o.y; t[2] = o.z;
    vg_redirect(m, t, aim, VG_REF_TARGET, 30.0f, &used, NULL, &after);
    CHECK(used == 1 && near_f(vg_angle_deg(after, aim), 90.0f, 1e-2f), "target mode");
    /* no aim point (lock-on overridden): the launch's Z */
    set_launch(m, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), o);
    vg_redirect(m, NULL, aim, VG_REF_AUTO, 30.0f, &used, NULL, &after);
    CHECK(used == 0 && near_v(after, aim, 1e-4f), "no target");
    /* an aim point on the muzzle is ignored */
    set_launch(m, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), o);
    t[0] = o.x; t[1] = o.y; t[2] = o.z;
    vg_redirect(m, t, aim, VG_REF_TARGET, 30.0f, &used, NULL, &after);
    CHECK(used == 0 && near_v(after, aim, 1e-4f), "aim point at the muzzle");
    if (!failures) printf("PASS: the launch redirect (muzzle kept, aim point moved, spread kept)\n");
}

static void test_yaw_pitch(void) {
    float y, p;
    vg_yaw_pitch(vr_v3(0, 0, 1), &y, &p);
    CHECK(near_f(y, 0, 1e-4f) && near_f(p, 0, 1e-4f), "forward");
    vg_yaw_pitch(vr_v3(1, 0, 0), &y, &p);
    CHECK(near_f(y, 90, 1e-4f), "+X = yaw 90");
    vg_yaw_pitch(vr_v3(0, -1, 0), &y, &p);
    CHECK(near_f(p, -90, 1e-3f), "down");
    if (!failures) printf("PASS: yaw and pitch for the log\n");
}

static void test_ids(void) {
    CHECK(vg_id_in_list("500-999", 500) && vg_id_in_list("500-999", 999), "range ends");
    CHECK(!vg_id_in_list("500-999", 1000) && !vg_id_in_list("500-999", 120), "outside");
    CHECK(vg_id_in_list("all", 7), "all");
    CHECK(vg_id_in_list("100, 500-510,1100", 1100) && vg_id_in_list("100, 500-510,1100", 100), "list");
    CHECK(!vg_id_in_list("100, 500-510,1100", 511), "list miss");
    CHECK(!vg_id_in_list("", 500) && !vg_id_in_list("abc", 500) && !vg_id_in_list(NULL, 500), "empty or bad");
    if (!failures) printf("PASS: the bullet id list\n");
}

/* ---- Physical items: the throw mode (findings/39) ---- */
#define THROW_IDS "100,105,120,121,150,170,180,410,460,470" /* = BBXR_THROW_BULLET_IDS (checked in vr_game_ui_test) */

static void test_rel_to_world(void) {
    float rows[3][4];
    set_rows(rows, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1));
    /* a head-frame forward (-Z) is the camera's forward; right is right; up is up */
    CHECK(near_v(vg_rel_dir_to_world(vr_v3(0, 0, -1), (const float (*)[4])rows), vr_v3(0, 0, 1), 1e-5f), "forward");
    CHECK(near_v(vg_rel_dir_to_world(vr_v3(1, 0, 0), (const float (*)[4])rows), vr_v3(1, 0, 0), 1e-5f), "right");
    CHECK(near_v(vg_rel_dir_to_world(vr_v3(0, 1, 0), (const float (*)[4])rows), vr_v3(0, 1, 0), 1e-5f), "up");
    /* the same through a turned camera and the vg_aim_in_world path (identity head, a pose pointing 30 deg up) */
    float turned[3][4];
    set_rows(turned, vr_v3(0, 0, -1), vr_v3(0, 1, 0), vr_v3(1, 0, 0));
    const VrQuat id = vr_qidentity();
    const VrQuat up = vr_qaxis(vr_v3(1, 0, 0), 30.0f * VR_DEG);
    const VrVec3 pointing = vr_qrotate(up, vr_v3(0, 0, -1));
    CHECK(near_v(vg_rel_dir_to_world(pointing, (const float (*)[4])turned),
                 vg_aim_in_world(id, up, (const float (*)[4])turned), 1e-5f), "agrees with the controller ray");
    /* a position: the camera plus the offset (0.3 m right, 0.2 m below, 0.4 m ahead of the head) */
    const VrVec3 p = vg_rel_pos_to_world(vr_v3(0.3f, -0.2f, -0.4f), (const float (*)[4])rows, vr_v3(10, 2, -5));
    CHECK(near_v(p, vr_v3(10.3f, 1.8f, -4.6f), 1e-5f), "position %f %f %f", p.x, p.y, p.z);
    if (!failures) printf("PASS: head-frame vectors in the game world\n");
}

static void test_throw_launch(void) {
    float m[3][4], t[3];
    const VrVec3 muzzle = vr_v3(0, 1.4f, 0), hand = vr_v3(0.3f, 1.2f, 0.4f);
    set_launch(m, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), muzzle);
    t[0] = 0; t[1] = 1.4f; t[2] = 6.0f; /* the game's aim point: 6 m ahead */
    const VrVec3 dir = vam_norm(vr_v3(1, 0.4f, 1), vr_v3(0, 0, 1));
    int moved = 0;
    VrVec3 before, after;
    const float turn = vg_throw_launch(m, t, hand, dir, 2.0f, &moved, &before, &after);
    CHECK(moved == 1, "hand within reach of the muzzle");
    CHECK(near_v(vg_launch_origin((const float (*)[4])m), hand, 1e-5f), "origin at the hand");
    CHECK(near_v(vam_norm(vg_launch_axis((const float (*)[4])m, 2), vr_v3(0, 0, 1)), dir, 1e-4f), "Z along the release");
    CHECK(near_v(after, dir, 1e-4f) && near_v(before, vr_v3(0, 0, 1), 1e-5f), "old and new directions");
    CHECK(near_f(turn, vg_angle_deg(vr_v3(0, 0, 1), dir), 1e-3f), "turn angle");
    CHECK(near_v(vr_v3(t[0], t[1], t[2]), vr_add(hand, vr_scale(dir, 6.0f)), 1e-4f), "aim point 6 m along the release from the hand");
    /* no roll: X stays horizontal, handedness kept */
    const VrVec3 x = vg_launch_axis((const float (*)[4])m, 0), y = vg_launch_axis((const float (*)[4])m, 1), z = vg_launch_axis((const float (*)[4])m, 2);
    CHECK(near_f(x.y, 0.0f, 1e-4f) && near_v(vr_cross(x, y), z, 1e-4f), "level, right-handed");
    /* a hand 3 m from the muzzle (a wrong world assumption): the game's origin is kept, the direction still turns */
    set_launch(m, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), muzzle);
    vg_throw_launch(m, NULL, vr_v3(3, 1.4f, 0), dir, 2.0f, &moved, NULL, NULL);
    CHECK(moved == 0 && near_v(vg_launch_origin((const float (*)[4])m), muzzle, 1e-6f), "far hand: origin kept");
    CHECK(near_v(vam_norm(vg_launch_axis((const float (*)[4])m, 2), vr_v3(0, 0, 1)), dir, 1e-4f), "far hand: direction still turned");
    /* a lock-on (no aim point): 10 m default distance is only used for a point target, none given here */
    set_launch(m, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), muzzle);
    vg_throw_launch(m, NULL, hand, vr_v3(0, 0, 1), 2.0f, &moved, NULL, NULL);
    CHECK(moved == 1 && near_v(vg_launch_origin((const float (*)[4])m), hand, 1e-6f), "no target");
    /* straight up (vertical release): the fallback rotation, still a unit direction */
    set_launch(m, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), muzzle);
    vg_throw_launch(m, NULL, hand, vr_v3(0, 1, 0), 2.0f, &moved, NULL, NULL);
    CHECK(near_v(vg_launch_axis((const float (*)[4])m, 2), vr_v3(0, 1, 0), 1e-4f), "vertical");
    if (!failures) printf("PASS: the throw launch (origin at the hand, Z along the release, aim point moved)\n");
}

static void test_throw_ids(void) {
    static const int listed[] = {100, 105, 120, 121, 150, 170, 180, 410, 460, 470};
    for (size_t i = 0; i < sizeof listed / sizeof *listed; ++i) CHECK(vg_id_in_list(THROW_IDS, listed[i]), "bullet %d is listed", listed[i]);
    /* the self-targeted and non-flying ones are never redirected: Antidote 200, Sedative 230, Lead Elixir 320,
     * the rope molotovs 110 / 114, Pungent Blood 160, Shining Coins 300, Executioner's Gloves 350, the
     * firearms 500-999 (the gun's own list) and the burst children */
    static const int excluded[] = {200, 230, 320, 110, 114, 160, 300, 350, 101, 106, 151, 600, 601, 0};
    for (size_t i = 0; i < sizeof excluded / sizeof *excluded; ++i) CHECK(!vg_id_in_list(THROW_IDS, excluded[i]), "bullet %d is not listed", excluded[i]);
    if (!failures) printf("PASS: the thrown bullet id list\n");
}

int main(void) {
    test_rel_to_world();
    test_throw_launch();
    test_throw_ids();
    test_aim_in_world();
    test_turn();
    test_redirect();
    test_yaw_pitch();
    test_ids();
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("All %d VR gun aim checks passed\n", checks);
    return 0;
}
