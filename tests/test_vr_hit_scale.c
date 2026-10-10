/* Unit tests for src/vr_hit_scale.h, the VR hit shape scaling shared by the game hook and the
 * sandbox (docs/vr/findings/32-vr-hitbox-scale.md). Build: ninja -C out/gpu vr-hit-scale-test
 * vr-hit-scale-test-cpp (no GPU, no game). Prints the failures; exit 0 when none. */
#include "vr_hit_scale.h"
#include <stdio.h>

static int failures, checks;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

static int near_f(float a, float b, float eps) { return fabsf(a - b) <= eps; }
static int near_v(VrVec3 a, VrVec3 b, float eps) { return near_f(a.x, b.x, eps) && near_f(a.y, b.y, eps) && near_f(a.z, b.z, eps); }

static void test_defaults(void) {
    const VhsParams d = vhs_defaults(), id = vhs_identity();
    CHECK(d.enabled && near_f(d.radius_scale, 1.5f, 1e-6f) && near_f(d.reach_m, 0.15f, 1e-6f), "VR defaults x1.5 +0.15");
    CHECK(!id.enabled && id.radius_scale == 1.0f && id.reach_m == 0.0f, "identity");
    const VrVec3 g = vr_v3(0, 0, 0), a = vr_v3(0, -0.3f, 0), b = vr_v3(0, -0.8f, 0);
    const VhsShape s = vhs_apply(&id, a, b, g, 0.1f, 0.8f, 1);
    CHECK(near_v(s.a, a, 1e-6f) && near_v(s.b, b, 1e-6f) && s.r == 0.1f, "identity leaves the shape");
    VhsParams p = d;
    p.enabled = 0;
    const VhsShape o = vhs_apply(&p, a, b, g, 0.1f, 0.8f, 1);
    CHECK(near_v(o.b, b, 1e-6f) && o.r == 0.1f, "disabled leaves the shape");
}

static void test_capsule(void) {
    const VhsParams d = vhs_defaults();
    const VrVec3 g = vr_v3(0, 0, 0), a = vr_v3(0, -0.3f, 0), b = vr_v3(0, -0.8f, 0);
    VhsShape s = vhs_apply(&d, a, b, g, 0.1f, 0.8f, 1);
    CHECK(near_f(s.r, 0.15f, 1e-6f), "radius x1.5, got %f", s.r);
    CHECK(near_v(s.a, a, 1e-6f) && near_v(s.b, vr_v3(0, -0.95f, 0), 1e-5f), "tip +0.15 m along the blade");
    /* swapped endpoints: the far one still grows */
    s = vhs_apply(&d, b, a, g, 0.1f, 0.8f, 1);
    CHECK(near_v(s.a, vr_v3(0, -0.95f, 0), 1e-5f) && near_v(s.b, a, 1e-6f), "swapped endpoints");
    /* a root shape (near the grip) is only thicker */
    s = vhs_apply(&d, vr_v3(0, 0, 0), vr_v3(0, -0.1f, 0), g, 0.1f, 0.8f, 1);
    CHECK(near_v(s.b, vr_v3(0, -0.1f, 0), 1e-6f) && near_f(s.r, 0.15f, 1e-6f), "root shape: radius only");
    /* reach_ok = 0 (a kick) */
    s = vhs_apply(&d, a, b, g, 0.1f, 0.8f, 0);
    CHECK(near_v(s.b, b, 1e-6f) && near_f(s.r, 0.15f, 1e-6f), "no reach for body shapes");
    /* an oblique blade extends along its own axis, not the grip direction */
    s = vhs_apply(&d, vr_v3(0.1f, 0, 0), vr_v3(0.1f, 0, -0.5f), vr_v3(0, 0, 0), 0.05f, 0.51f, 1);
    CHECK(near_v(s.b, vr_v3(0.1f, 0, -0.65f), 1e-5f), "along the capsule axis");
}

static void test_sphere_cap_negative(void) {
    const VhsParams d = vhs_defaults();
    const VrVec3 g = vr_v3(0, 0, 0), c = vr_v3(0, -0.7f, 0);
    VhsShape s = vhs_apply(&d, c, c, g, 0.5f, 0.7f, 1);
    CHECK(near_f(s.r, 0.75f, 1e-6f) && near_v(s.a, c, 1e-6f) && near_v(s.b, vr_v3(0, -0.85f, 0), 1e-5f), "sphere -> capsule outward");
    s = vhs_apply(&d, c, c, g, 1.0f, 0.7f, 1);
    CHECK(s.r <= VHS_RADIUS_MAX_M + 1e-6f, "radius cap");
    VhsParams n = d;
    n.reach_m = -0.3f;
    s = vhs_apply(&n, vr_v3(0, -0.5f, 0), vr_v3(0, -0.6f, 0), g, 0.1f, 0.6f, 1);
    CHECK(s.b.y <= s.a.y + 1e-6f && s.b.y >= -0.6f - 1e-6f && s.b.y <= -0.5f + 1e-6f, "a negative reach shortens, never inverts");
    /* a sphere on the grip has no direction: unchanged position */
    s = vhs_apply(&d, g, g, g, 0.1f, 0.7f, 1);
    CHECK(near_v(s.a, g, 1e-6f) && near_v(s.b, g, 1e-6f), "sphere at the grip");
}

static void test_classes_keys(void) {
    CHECK(vhs_class_from_reach(0.5f) == VHS_SHORT && vhs_class_from_reach(0.9f) == VHS_MEDIUM && vhs_class_from_reach(1.21f) == VHS_LONG, "classes");
    VhsParams p = vhs_defaults();
    p.class_radius_scale[VHS_LONG] = 2.0f;
    p.class_reach_m[VHS_LONG] = 0.0f;
    CHECK(vhs_radius_scale_for(&p, VHS_LONG) == 2.0f && vhs_radius_scale_for(&p, VHS_SHORT) == 1.5f, "per-class radius");
    CHECK(vhs_reach_for(&p, VHS_LONG) == 0.0f && near_f(vhs_reach_for(&p, VHS_MEDIUM), 0.15f, 1e-6f), "per-class reach (0 is an override)");
    VhsParams k = vhs_identity();
    CHECK(vhs_apply_key(&k, "hit", "1") && k.enabled, "hit=1");
    CHECK(vhs_apply_key(&k, "hit_scale", "2") && k.radius_scale == 2.0f, "hit_scale");
    CHECK(vhs_apply_key(&k, "hit_reach", "0.25") && near_f(k.reach_m, 0.25f, 1e-6f), "hit_reach");
    CHECK(vhs_apply_key(&k, "hit_scale_m", "1.2") && near_f(k.class_radius_scale[VHS_MEDIUM], 1.2f, 1e-6f), "hit_scale_m");
    CHECK(vhs_apply_key(&k, "hit_reach_l", "0.05") && near_f(k.class_reach_m[VHS_LONG], 0.05f, 1e-6f), "hit_reach_l");
    CHECK(vhs_apply_key(&k, "hit_scale", "100") && k.radius_scale == VHS_SCALE_MAX, "clamped");
    CHECK(vhs_apply_key(&k, "hit_scale", "nan") && k.radius_scale == VHS_SCALE_MAX, "garbage ignored");
    CHECK(vhs_apply_key(&k, "hit_scale_m", "-1") && k.class_radius_scale[VHS_MEDIUM] < 0.0f, "-1 inherits");
    CHECK(vhs_apply_key(&k, "hit_reach_l", "-2") && vhs_reach_for(&k, VHS_LONG) == k.reach_m, "-2 inherits the reach");
    CHECK(!vhs_apply_key(&k, "arm_weight", "1") && !vhs_apply_key(&k, "hit_bogus", "1") && !vhs_apply_key(&k, "hit_scale_x", "1"), "other keys ignored");
}

/* The game's capsule: local ends (0,0,a_z)..(0,0,b_z) on the DmyPoly1 -> DmyPoly2 axis (findings/32 4). */
static void test_axis(void) {
    VhsParams p = vhs_defaults();
    p.reach_m = 0.5f; /* the user's game setting: x1.5, +0.50 m */
    vhs_sanitize(&p);
    CHECK(near_f(p.reach_m, 0.5f, 1e-6f), "+0.50 m reach is allowed");
    const VrVec3 grip = vr_v3(0, 1, 0), p1 = vr_v3(0, 1, 0.3f), p2 = vr_v3(0, 1, 1.0f); /* p2 the tip */
    VhsAxisShape s = vhs_apply_axis(&p, p1, p2, 0.0f, 0.7f, 0.2f, grip, 1.0f, 1);
    CHECK(s.axis_ok && near_f(s.r, 0.3f, 1e-6f), "axis: radius x1.5, got %f", s.r);
    CHECK(near_f(s.a_z, 0.0f, 1e-5f) && near_f(s.b_z, 1.2f, 1e-5f), "axis: tip end B +0.5, got %f..%f", s.a_z, s.b_z);
    /* DmyPoly1 is the tip: A moves backwards along local Z */
    s = vhs_apply_axis(&p, p2, p1, 0.0f, 0.7f, 0.2f, grip, 1.0f, 1);
    CHECK(near_f(s.a_z, -0.5f, 1e-5f) && near_f(s.b_z, 0.7f, 1e-5f), "axis: tip end A -0.5, got %f..%f", s.a_z, s.b_z);
    /* body shape (kick): radius only */
    s = vhs_apply_axis(&p, p1, p2, 0.0f, 0.7f, 0.2f, grip, 1.0f, 0);
    CHECK(near_f(s.a_z, 0.0f, 1e-6f) && near_f(s.b_z, 0.7f, 1e-6f) && near_f(s.r, 0.3f, 1e-6f), "axis: reach_ok 0");
    /* degenerate (p1 == p2, the game keeps B = 1e-5 on Z): radius only, ends untouched */
    s = vhs_apply_axis(&p, p1, p1, 0.0f, 1e-5f, 0.2f, grip, 1.0f, 1);
    CHECK(!s.axis_ok && s.a_z == 0.0f && s.b_z == 1e-5f && near_f(s.r, 0.3f, 1e-6f), "axis: degenerate");
    /* off: identity */
    VhsParams off = p;
    off.enabled = 0;
    s = vhs_apply_axis(&off, p1, p2, 0.0f, 0.7f, 0.2f, grip, 1.0f, 1);
    CHECK(s.a_z == 0.0f && near_f(s.b_z, 0.7f, 1e-6f) && s.r == 0.2f, "axis: off");
    /* reach capped at 1.0 m */
    CHECK(vhs_apply_key(&p, "hit_reach", "3") && p.reach_m == VHS_REACH_MAX_M, "reach clamp 1.0");
}

static void test_track_text(void) {
    VhsReachTrack t = {0, 0.0, 0.0f};
    CHECK(near_f(vhs_track_reach(&t, 700000, 1.0, 1.0, 0.5f), 0.5f, 1e-6f), "track: first");
    CHECK(near_f(vhs_track_reach(&t, 700000, 1.1, 1.0, 1.2f), 1.2f, 1e-6f), "track: grows");
    CHECK(near_f(vhs_track_reach(&t, 700000, 1.2, 1.0, 0.3f), 1.2f, 1e-6f), "track: keeps the max");
    CHECK(near_f(vhs_track_reach(&t, 700001, 1.3, 1.0, 0.4f), 0.4f, 1e-6f), "track: new attack restarts");
    CHECK(near_f(vhs_track_reach(&t, 700001, 3.0, 1.0, 0.2f), 0.2f, 1e-6f), "track: gap restarts");
    CHECK(near_f(vhs_seg_dist(vr_v3(0, 1, 0), vr_v3(-1, 0, 0), vr_v3(1, 0, 0)), 1.0f, 1e-6f) &&
              near_f(vhs_seg_dist(vr_v3(3, 0, 0), vr_v3(-1, 0, 0), vr_v3(1, 0, 0)), 2.0f, 1e-6f),
          "segment distance");
    VhsParams k = vhs_identity();
    const int n = vhs_apply_text(&k, "arm_weight=1 hit=1\nhit_scale=1.5\thit_reach=0.50 hit_scale_l=1.2 junk = hit_reach_s=\r\n");
    CHECK(n == 5 && k.enabled && near_f(k.radius_scale, 1.5f, 1e-6f) && near_f(k.reach_m, 0.5f, 1e-6f) &&
              near_f(k.class_radius_scale[VHS_LONG], 1.2f, 1e-6f) && k.class_reach_m[VHS_SHORT] < VHS_REACH_MIN_M,
          "control text, got %d keys", n);
}

int main(void) {
    test_defaults();
    test_capsule();
    test_sphere_cap_negative();
    test_classes_keys();
    test_axis();
    test_track_text();
    printf("vr_hit_scale: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
