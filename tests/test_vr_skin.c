/* Unit tests for src/vr_skin_math.h (custom character model override). Build: ninja -C out/gpu
 * vr-skin-test. Prints PASS; exit 0. */
#include "vr_skin_math.h"
#include <stdio.h>

static int failures, checks;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

static float len3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

/* A simple view: camera at the model origin looking along model -Z...(the camera forward is +Z
 * of its own frame), the head at the XR origin looking along XR -Z. */
static VhbView simple_view(float yaw) {
    VhbView v;
    v.cam_pos = vr_v3(0, 0, 0);
    v.cam_rot = vr_qidentity();
    v.head_pos = vr_v3(0, 0, 0);
    v.head_rot = vr_qidentity();
    v.body_pos = vr_v3(10, 2, -5);
    v.body_yaw = yaw;
    return v;
}

static void test_target(void) {
    const VhbView v = simple_view(0.7f);
    /* the player's face looks along model -Z; a character 3 m in front of the player's face */
    const VrVec3 front = vr_model_to_world(v.body_pos, v.body_yaw, vr_v3(0, 0, -3));
    float p[3], f[3], u[3];
    vsk_target_from_world(&v, front, v.body_yaw, p, f, u);
    CHECK(fabsf(len3(p) - 3.0f) < 1e-3f, "distance %f", len3(p));
    /* the camera's forward is model +Z here, which is XR -Z; a point at model -Z is behind the camera (XR +Z) */
    CHECK(fabsf(p[0]) < 1e-3f && fabsf(p[1]) < 1e-3f, "lateral %f %f", p[0], p[1]);
    CHECK(fabsf(fabsf(p[2]) - 3.0f) < 1e-3f, "z %f", p[2]);
    CHECK(fabsf(len3(f) - 1.0f) < 1e-4f && fabsf(len3(u) - 1.0f) < 1e-4f, "unit vectors");
    /* same yaw as the player: it faces the same way as the player's face (model -Z) */
    const VrVec3 pf = vhb_model_dir_to_xr(&v, vr_v3(0, 0, -1));
    CHECK(fabsf(f[0] - pf.x) < 1e-4f && fabsf(f[1] - pf.y) < 1e-4f && fabsf(f[2] - pf.z) < 1e-4f,
          "forward %f %f %f vs %f %f %f", f[0], f[1], f[2], pf.x, pf.y, pf.z);
    CHECK(fabsf(u[1] - 1.0f) < 1e-4f, "up %f %f %f", u[0], u[1], u[2]);
    /* turned by 90 degrees: forward is perpendicular to the first */
    vsk_target_from_world(&v, front, v.body_yaw + VR_PI * 0.5f, p, f, u);
    CHECK(fabsf(f[0] * pf.x + f[1] * pf.y + f[2] * pf.z) < 1e-4f, "perpendicular");
    /* reflection: the mapped points keep distances */
    float p2[3], f2[3], u2[3];
    vsk_target_from_world(&v, vr_add(front, vr_v3(1, 0, 0)), v.body_yaw, p2, f2, u2);
    const float d[3] = {p2[0] - p[0], p2[1] - p[1], p2[2] - p[2]};
    CHECK(fabsf(len3(d) - 1.0f) < 1e-3f, "metric kept %f", len3(d));
    /* a tilted headset keeps up unit and feet at the right distance */
    VhbView t = simple_view(2.0f);
    t.head_rot = vr_xr_ypr(0.4f, -0.2f, 0.1f);
    t.head_pos = vr_v3(0.3f, 1.7f, -0.4f);
    vsk_target_from_world(&t, vr_add(t.body_pos, vr_v3(2, 0, 1)), 0.3f, p, f, u);
    CHECK(fabsf(len3(f) - 1.0f) < 1e-4f && fabsf(len3(u) - 1.0f) < 1e-4f && fabsf(f[0] * u[0] + f[1] * u[1] + f[2] * u[2]) < 1e-3f,
          "tilted frame");
}

static void test_speed(void) {
    VskSpeed s;
    memset(&s, 0, sizeof(s));
    float v = vsk_speed_update(&s, vr_v3(0, 0, 0), 0.0);
    CHECK(v == 0.0f, "first sample");
    for (int i = 1; i <= 100; ++i) v = vsk_speed_update(&s, vr_v3(0.05f * (float)i, 9.0f, 0), 0.01 * i); /* 5 m/s */
    CHECK(fabsf(v - 5.0f) < 0.1f, "converges %f", v);
    v = vsk_speed_update(&s, vr_v3(500, 0, 0), 1.02); /* teleport */
    CHECK(v == 0.0f, "teleport reset %f", v);
    v = vsk_speed_update(&s, vr_v3(500, 0, 0), 1.03);
    CHECK(v < 0.01f, "stands %f", v);
    const float k = vsk_speed_update(&s, vr_v3(500, 0, 0), 1.0301); /* tiny dt keeps the old value */
    CHECK(k == v, "tiny dt");
}

static void test_select(void) {
    VskCand c[6] = {{0, 30.0f, 0}, {1, 5.0f, 1}, {2, 12.0f, 0}, {3, 8.0f, 1}, {4, 2.0f, 0}, {5, 20.0f, 0}};
    int out[VSK_MAX];
    int n = vsk_select(c, 6, "all", out);
    CHECK(n == 4 && out[0] == 4 && out[1] == 1 && out[2] == 3 && out[3] == 2, "all nearest first cap 4: %d (%d %d %d %d)", n,
          out[0], out[1], out[2], out[3]);
    n = vsk_select(c, 6, NULL, out);
    CHECK(n == 4 && out[0] == 4, "null = all");
    n = vsk_select(c, 6, "2", out);
    CHECK(n == 1 && out[0] == 2, "index 2");
    n = vsk_select(c, 6, "9", out);
    CHECK(n == 0, "index absent");
    n = vsk_select(c, 6, "Hunter", out);
    CHECK(n == 2 && out[0] == 1 && out[1] == 3, "name: %d", n);
    n = vsk_select(c, 0, "all", out);
    CHECK(n == 0, "empty");
}

static void test_utf16(void) {
    const uint16_t w[] = {'x', 'A', 'b', 'c', 'y', 0};
    CHECK(vsk_utf16_match(w, 5, "Abc"), "match");
    CHECK(!vsk_utf16_match(w, 5, "Abd"), "no match");
    CHECK(!vsk_utf16_match(w, 2, "Abc"), "too short");
    unsigned char b[16] = {0};
    const char *nm = "Zed";
    for (int i = 0; i < 3; ++i) b[6 + i * 2] = (unsigned char)nm[i];
    CHECK(vsk_utf16_find(b, 16, "Zed") == 6, "find offset");
    CHECK(vsk_utf16_find(b, 16, "Zeda") < 0, "absent");
    CHECK(vsk_utf16_find(b, 16, "") < 0, "empty name");
}

static void test_net_name(void) {
    /* net info +0x30.. of a guest as probed live: IPv4/port, then the name at +0x36 */
    const unsigned char live[] = {0x01, 0x00, 0x00, 0x7f, 0x7c, 0xce, 'F', 'r', 'i', 'e', 'n', 'd', 'T', 'h',
                                  'r', 'e', 'e', 0, 0, 0};
    char out[VSK_NAME_MAX + 1];
    CHECK(vsk_net_name(live + 6, sizeof(live) - 6, out) == 11 && !strcmp(out, "FriendThree"), "live name '%s'", out);
    CHECK(vsk_name_eq(out, "friendthree") && vsk_name_eq("FriendThree", out), "case-insensitive");
    CHECK(!vsk_name_eq(out, "FriendThre") && !vsk_name_eq(out, "FriendThreeX") && !vsk_name_eq("", ""), "exact");
    const unsigned char empty[4] = {0};
    CHECK(vsk_net_name(empty, 4, out) == 0 && out[0] == 0, "empty");
    const unsigned char junk[] = {'A', 0x01, 'B', 0};
    CHECK(vsk_net_name(junk, 4, out) == 0, "control char");
    const unsigned char unterminated[] = {'A', 'B', 'C'};
    CHECK(vsk_net_name(unterminated, 3, out) == 0, "unterminated");
    unsigned char lng[20];
    memset(lng, 'x', sizeof(lng));
    lng[17] = 0;
    CHECK(vsk_net_name(lng, sizeof(lng), out) == 0, "17 characters is too long");
    lng[16] = 0;
    CHECK(vsk_net_name(lng, sizeof(lng), out) == 16, "16 characters");
    CHECK(vsk_slot_count(5) == 5 && vsk_slot_count(0) == 0 && vsk_slot_count(12) == VSK_SLOT_MAX &&
          vsk_slot_count(0x80000000u) == 0, "slot count");
}

int main(void) {
    test_target();
    test_speed();
    test_select();
    test_utf16();
    test_net_name();
    if (failures) { printf("FAILED %d of %d checks\n", failures, checks); return 1; }
    printf("PASS (%d checks)\n", checks);
    return 0;
}
