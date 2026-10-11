/* Unit tests for src/vr_gun_math.h, the VR gun aim's direction math (src/runtime_vr_gun.c,
 * docs/vr/findings/27-gun-aim.md): the controller ray in the game world and the launch redirect.
 * Build: ninja -C out/gpu vr-gun-test (no GPU, no game). Prints PASS lines; exit 0. */
#include "vr_gun_math.h"
#include "vr_fp_math.h"
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
#define THROW_IDS "100,105,111,115,120,121,150,160,170,180,300,350,410,460,470"
#define FOLLOW_IDS "111,112,115,116" /* = BBXR_THROW_BULLET_IDS (checked in vr_game_ui_test) */

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

static void test_throw_group(void) {
    /* a throw of three bullets fanned 20 deg either side of the game's forward (the gloves), the first one the middle */
    const VrVec3 muzzle = vr_v3(0, 1.4f, 0), hand = vr_v3(0.3f, 1.2f, 0.4f);
    const VrVec3 dir = vam_norm(vr_v3(1, 0.4f, 1), vr_v3(0, 0, 1));
    float mid[3][4], a[3][4], b[3][4];
    const VrQuat qa = vr_qaxis(vr_v3(0, 1, 0), 20.0f * VR_DEG), qb = vr_qaxis(vr_v3(0, 1, 0), -20.0f * VR_DEG);
    set_launch(mid, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), muzzle);
    set_launch(a, vr_qrotate(qa, vr_v3(1, 0, 0)), vr_v3(0, 1, 0), vr_qrotate(qa, vr_v3(0, 0, 1)), muzzle);
    set_launch(b, vr_qrotate(qb, vr_v3(1, 0, 0)), vr_v3(0, 1, 0), vr_qrotate(qb, vr_v3(0, 0, 1)), muzzle);
    VgThrowGroup g = vg_throw_group_make((const float (*)[4])mid, hand, dir, 2.0f);
    CHECK(g.moved == 1, "hand in reach");
    int moved = 0;
    VrVec3 od, nd;
    vg_throw_launch(mid, NULL, hand, dir, 2.0f, &moved, NULL, NULL);
    vg_throw_group_apply(a, NULL, &g, 0.5f, &moved, &od, &nd);
    CHECK(moved == 1 && near_v(vg_launch_origin((const float (*)[4])a), hand, 1e-5f), "member at the hand");
    vg_throw_group_apply(b, NULL, &g, 0.5f, &moved, NULL, NULL);
    const VrVec3 za = vg_launch_axis((const float (*)[4])a, 2), zb = vg_launch_axis((const float (*)[4])b, 2);
    CHECK(near_f(vg_angle_deg(za, dir), 20.0f, 0.05f) && near_f(vg_angle_deg(zb, dir), 20.0f, 0.05f), "spread 20 deg around the release");
    CHECK(near_f(vg_angle_deg(za, zb), 40.0f, 0.05f), "relative spread kept (40 deg)");
    CHECK(near_v(vg_launch_axis((const float (*)[4])mid, 2), dir, 1e-4f), "first bullet along the release");
    /* a member born far from the first bullet's muzzle (an impact child) is turned but not moved */
    float far_m[3][4];
    set_launch(far_m, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), vr_v3(5, 1, 5));
    vg_throw_group_apply(far_m, NULL, &g, 0.5f, &moved, NULL, NULL);
    CHECK(moved == 0 && near_v(vg_launch_origin((const float (*)[4])far_m), vr_v3(5, 1, 5), 1e-6f), "far member keeps its origin");
    CHECK(near_v(vg_launch_axis((const float (*)[4])far_m, 2), dir, 1e-4f), "far member still turned");
    /* hand out of reach: the group keeps every origin */
    set_launch(mid, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), muzzle);
    g = vg_throw_group_make((const float (*)[4])mid, vr_v3(3, 1.4f, 0), dir, 2.0f);
    set_launch(a, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), muzzle);
    vg_throw_group_apply(a, NULL, &g, 0.5f, &moved, NULL, NULL);
    CHECK(g.moved == 0 && moved == 0 && near_v(vg_launch_origin((const float (*)[4])a), muzzle, 1e-6f), "far hand: origin kept");
    /* the aim point is turned and moved with the member */
    float t[3] = {0, 1.4f, 6.0f};
    set_launch(a, vr_v3(1, 0, 0), vr_v3(0, 1, 0), vr_v3(0, 0, 1), muzzle);
    g = vg_throw_group_make((const float (*)[4])a, hand, dir, 2.0f);
    vg_throw_group_apply(a, t, &g, 0.5f, &moved, NULL, NULL);
    CHECK(near_v(vr_v3(t[0], t[1], t[2]), vr_add(hand, vr_scale(dir, 6.0f)), 1e-3f), "aim point along the release from the hand");
    if (!failures) printf("PASS: a multi-bullet throw keeps its spread\n");
}

static void test_throw_ids(void) {
    static const int listed[] = {100, 105, 111, 115, 120, 121, 150, 160, 170, 180, 300, 350, 410, 460, 470};
    for (size_t i = 0; i < sizeof listed / sizeof *listed; ++i) CHECK(vg_id_in_list(THROW_IDS, listed[i]), "bullet %d is listed", listed[i]);
    /* the self-targeted and non-flying ones are never redirected: Antidote 200, Sedative 230, Lead Elixir 320,
     * the rope molotovs' speed-0 spawners 110 / 114 (their flying child 111 / 115 is aimed), the
     * firearms 500-999 (the gun's own list) and the burst children */
    static const int excluded[] = {200, 230, 320, 101, 106, 110, 114, 112, 116, 151, 600, 601, 0};
    for (size_t i = 0; i < sizeof excluded / sizeof *excluded; ++i) CHECK(!vg_id_in_list(THROW_IDS, excluded[i]), "bullet %d is not listed", excluded[i]);
    if (!failures) printf("PASS: the thrown bullet id list\n");
}

static void test_assist(void) {
    VgAssist a = vg_assist_defaults();
    const VrVec3 mz = vr_v3(0, 1.5f, 0);
    /* cone: wide close, narrow far, clamped */
    CHECK(near_f(vg_assist_cone(&a, 0.5f), 18.0f, 1e-4f), "close = max cone");
    CHECK(near_f(vg_assist_cone(&a, 3.0f), atanf(0.2f) / VR_DEG, 1e-3f), "3 m cone %.2f", vg_assist_cone(&a, 3.0f));
    CHECK(near_f(vg_assist_cone(&a, 30.0f), 4.0f, 1e-4f), "far = min cone");
    /* an enemy 3 m ahead; a ray 3 degrees off is inside the inner part: pulled 80% of the way */
    const VrVec3 tg = vr_v3(0, 1.5f, 3.0f);
    const float r = 3.0f * VR_DEG;
    const VrVec3 ray = vr_v3(sinf(r), 0, cosf(r));
    int pick = -2;
    float ang = 0;
    VrVec3 d = vg_assist_dir(ray, mz, &tg, 1, &a, &pick, &ang);
    CHECK(pick == 0 && near_f(ang, 3.0f, 1e-3f), "picked");
    CHECK(near_f(vg_angle_deg(d, vr_v3(0, 0, 1)), 0.6f, 0.1f), "pulled to %.2f deg", vg_angle_deg(d, vr_v3(0, 0, 1)));
    /* outside the cone: untouched */
    const float r2 = 15.0f * VR_DEG;
    const VrVec3 far_ray = vr_v3(sinf(r2), 0, cosf(r2));
    d = vg_assist_dir(far_ray, mz, &tg, 1, &a, &pick, &ang);
    CHECK(pick == -1 && near_v(d, far_ray, 1e-5f), "outside the cone");
    /* strength 1 inside the inner cone snaps onto the point; strength 0 does nothing */
    a.strength = 1.0f;
    d = vg_assist_dir(ray, mz, &tg, 1, &a, &pick, &ang);
    CHECK(near_v(d, vr_v3(0, 0, 1), 1e-4f), "snap");
    a.strength = 0.0f;
    d = vg_assist_dir(ray, mz, &tg, 1, &a, &pick, &ang);
    CHECK(near_v(d, ray, 1e-5f), "off");
    /* the pull fades toward the cone's edge: the remaining miss never shrinks as the ray moves out */
    a.strength = 0.8f;
    float prev = 0.0f;
    for (float deg = 0.0f; deg < 11.0f; deg += 0.5f) {
        const float rr = deg * VR_DEG;
        const VrVec3 rv = vr_v3(sinf(rr), 0, cosf(rr));
        d = vg_assist_dir(rv, mz, &tg, 1, &a, &pick, &ang);
        const float miss = vg_angle_deg(d, vr_v3(0, 0, 1));
        CHECK(miss >= prev - 1e-3f, "monotone at %.1f deg: %.3f < %.3f", deg, miss, prev);
        prev = miss;
    }
    /* the nearest in angle wins; targets too close or too far are ignored */
    const VrVec3 two[3] = {vr_v3(0.5f, 1.5f, 3.0f), vr_v3(0.1f, 1.5f, 3.0f), vr_v3(0, 1.5f, 40.0f)};
    d = vg_assist_dir(vr_v3(0, 0, 1), mz, two, 3, &a, &pick, &ang);
    CHECK(pick == 1, "nearest in angle %d", pick);
    const VrVec3 close = vr_v3(0, 1.5f, 0.1f);
    vg_assist_dir(vr_v3(0, 0, 1), mz, &close, 1, &a, &pick, &ang);
    CHECK(pick == -1, "too close ignored");
    if (!failures) printf("PASS: the parry aim assist (cone by distance, smooth falloff, nearest in angle)\n");
}

static void test_throw_speed(void) {
    CHECK(near_f(vg_throw_speed_factor(7.0f, 7.0f, 0.3f, 1.5f), 1.0f, 1e-6f), "the reference speed is the game's speed");
    CHECK(near_f(vg_throw_speed_factor(3.5f, 7.0f, 0.3f, 1.5f), 0.5f, 1e-6f), "half the reference: half");
    CHECK(near_f(vg_throw_speed_factor(30.0f, 7.0f, 0.3f, 1.5f), 1.5f, 1e-6f), "a fast throw is capped");
    CHECK(near_f(vg_throw_speed_factor(0.5f, 7.0f, 0.3f, 1.5f), 0.3f, 1e-6f), "a slow throw is floored");
    CHECK(near_f(vg_throw_speed_factor(0.0f, 7.0f, 0.3f, 1.5f), 1.0f, 1e-6f), "no speed: 1");
    CHECK(near_f(vg_throw_speed_factor(-2.0f, 7.0f, 0.3f, 1.5f), 1.0f, 1e-6f), "negative speed: 1");
    CHECK(near_f(vg_throw_speed_factor(5.0f, 0.0f, 0.3f, 1.5f), 1.0f, 1e-6f), "bad reference: 1");
    if (!failures) printf("PASS: throw speed factor (clamp(speed / ref, min, max), no speed = 1)\n");
}

/* The live throw that went backwards (out/vr_all_alt.log, v75, goods 1200 molotov, bullet 100):
 *   bridge: "direction (0.75 0.59 -0.28) [raw velocity (2.59 2.03 -0.97), forward 2.76 m/s ...]" (stage space)
 *   FP:     "player ... yaw 51.01 deg (faces -128.99 deg)", "turn smooth: view 50.5 deg = psi + head 38.3 deg"
 *   host:   "bullet 100 #0 thrown: yaw -127.6 pitch 29.2 -> yaw 48.6 pitch 36.4 (turn 114.3 deg)"
 * The hand went forward relative to the head (forward 2.76 of 3.4 m/s: the head looked along stage
 * (0.936, 0, -0.350)) and up 36 deg. Head frame -> world (vg_rel_dir_to_world) gives the view's heading,
 * so the conversion is right; the game's own launch went along the BODY, which faced -129.0, 180 deg from
 * the view: the stick had turned the body around (face=attack leaves the game's facing outside attacks).
 * The fix turns the body to the aim from the release (vr_fp_throw_face, vg_body_yaw_for_heading). */
static void test_throw_logged_case(void) {
    const float view = 50.49f * VR_DEG; /* the first-person camera's heading at the throw */
    float rows[3][4];
    set_rows(rows, vr_v3(cosf(view), 0, -sinf(view)), vr_v3(0, 1, 0), vr_v3(sinf(view), 0, cosf(view)));
    /* the head's yaw in the stage: its -Z along (0.936, 0, -0.350) */
    const float head_yaw = atan2f(-0.936f, 0.350f);
    const VrQuat head = vr_qaxis(vr_v3(0, 1, 0), head_yaw);
    const VrVec3 fwd = vr_qrotate(head, vr_v3(0, 0, -1));
    CHECK(near_v(fwd, vr_v3(0.936f, 0, -0.350f), 0.01f), "head forward %.3f %.3f %.3f", fwd.x, fwd.y, fwd.z);
    /* what the bridge sends: conj(head) * the stage direction */
    const VrVec3 stage_dir = vam_norm(vr_v3(0.75f, 0.59f, -0.28f), vr_v3(0, 0, 0));
    const VrVec3 rel = vr_qrotate(vr_qconj(head), stage_dir);
    CHECK(rel.z < -0.7f && fabsf(rel.x) < 0.1f && rel.y > 0.5f, "forward and up in the head (%.2f %.2f %.2f)", rel.x, rel.y, rel.z);
    const VrVec3 world = vam_norm(vg_rel_dir_to_world(rel, (const float (*)[4])rows), vr_v3(0, 0, 0));
    float yaw, pitch;
    vg_yaw_pitch(world, &yaw, &pitch);
    /* the world direction the player meant: the view's heading, pitched up as the hand went */
    CHECK(vg_heading_diff_deg(yaw, 50.49f) < 10.0f, "release heading %.1f within 10 deg of the view 50.5", yaw);
    CHECK(vg_heading_diff_deg(yaw, 48.6f) < 3.0f && near_f(pitch, 36.4f, 2.0f), "matches the host's logged 48.6 / 36.4 (%.1f / %.1f)", yaw, pitch);
    /* the game's own launch (yaw -127.6) is the body's facing, opposite the view */
    const float body_yaw_logged = 51.01f * VR_DEG;
    const float facing = vr_facing_heading(body_yaw_logged) / VR_DEG;
    CHECK(vg_heading_diff_deg(facing, -127.6f) < 3.0f, "game launch along the body's facing (%.1f)", facing);
    CHECK(vg_heading_diff_deg(facing, 50.49f) > 170.0f, "the body faced away from the view (%.1f vs 50.5)", facing);
    /* the fix: the body turned to the release heading faces it; a launch along the body is then the aim's heading */
    const float body = vg_body_yaw_for_heading(yaw * VR_DEG);
    CHECK(vg_heading_diff_deg(vr_facing_heading(body) / VR_DEG, yaw) < 0.1f, "body yaw %.1f faces the aim %.1f", body / VR_DEG, yaw);
    /* and the redirect of the game's launch (Z along the body's old facing, pitch 29.2) onto the release ends on it */
    const float gp = 29.2f * VR_DEG, gy = -127.6f * VR_DEG;
    const VrVec3 z = vr_v3(cosf(gp) * sinf(gy), sinf(gp), cosf(gp) * cosf(gy));
    const VrVec3 y = vam_norm(vr_sub(vr_v3(0, 1, 0), vr_scale(z, z.y)), vr_v3(0, 1, 0));
    float m[3][4];
    set_launch(m, vr_cross(y, z), y, z, vr_v3(-194.12f, -27.38f, 67.57f));
    float t[3] = {-194.12f + 6.0f * z.x, -27.38f + 6.0f * z.y, 67.57f + 6.0f * z.z};
    int moved = 0;
    VrVec3 od, nd;
    const float turn = vg_throw_launch(m, t, vr_v3(-193.87f, -26.97f, 67.59f), world, 2.0f, &moved, &od, &nd);
    float ny, np;
    vg_yaw_pitch(nd, &ny, &np);
    CHECK(near_f(turn, 114.3f, 2.0f), "turn %.1f (logged 114.3)", turn);
    CHECK(vg_heading_diff_deg(ny, yaw) < 0.5f && near_f(np, pitch, 0.5f) && moved, "launch Z yaw %.1f pitch %.1f", ny, np);
    if (!failures) printf("PASS: the logged backwards throw (conversion right, the body faced away; body turned to the aim)\n");
}

int main(void) {
    test_throw_logged_case();
    test_throw_speed();
    test_assist();
    test_rel_to_world();
    test_throw_launch();
    test_throw_ids();
    test_throw_group();
    test_aim_in_world();
    test_turn();
    test_redirect();
    test_yaw_pitch();
    test_ids();
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("All %d VR gun aim checks passed\n", checks);
    return 0;
}
