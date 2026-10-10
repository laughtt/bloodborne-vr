/* Unit tests for src/vr_stereo_math.h (alternate-eye stereo, docs/vr/PLAN.md Phase 3,
 * docs/vr/findings/22-phase3-alt-eye-stereo.md): the eye schedule (with a simulated presenter
 * that loses, repeats and decimates frames), the per-eye camera offset, the layer pose and
 * frustum, the eye tag, the frustum coverage and the control file.
 * Build: ninja -C out/gpu vr-stereo-test (no GPU, no game). Prints PASS lines; exit 0. */
#include "vr_stereo_math.h"
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

/* A point's position in a camera's view (x right, y up, z forward). */
static VrVec3 to_view(const float m[4][4], VrVec3 p) {
    const VrVec3 d = vr_sub(p, vr_row(m, 3));
    return vr_v3(vr_dot(d, vr_row(m, 0)), vr_dot(d, vr_row(m, 1)), vr_dot(d, vr_row(m, 2)));
}

static void test_alternation(void) {
    VrsSchedule s;
    vrs_schedule_reset(&s);
    VrsFeedback fb;
    memset(&fb, 0, sizeof(fb));
    fb.last_eye = -1;
    int ok = 1;
    for (int i = 0; i < 1000; ++i) {
        const int eye = vrs_schedule_next(&s, i % 3 ? &fb : NULL);
        ok &= eye == (i & 1);
        /* The presenter keeps up: it saw the previous frame (run 1). */
        fb.last_eye = eye;
        fb.run = 1;
        ++fb.assigned[eye];
    }
    CHECK(ok, "strict L R L R alternation");
    CHECK(s.guard_flips == 0, "no guard shifts while the presenter alternates (%u)", s.guard_flips);
    CHECK(s.frames == 1000, "frames counted");
    printf("PASS alternation\n");
}

/* A simulated presenter: frames reach it `latency` H1 calls later; `keep(i)` says whether frame i
 * is presented at all. A presented frame always goes to the eye H1 chose for it (the eye travels
 * with its camera; xr_stereo_test.cpp checks the pairing). Returns the longest run of presented
 * frames one eye waited for an image (after both eyes had one). */
typedef int (*KeepFn)(int i);
static int keep_all(int i) { (void)i; return 1; }
static int keep_even(int i) { return (i & 1) == 0; }
static int keep_odd(int i) { return (i & 1) == 1; }
static unsigned rng_state = 12345u;
static int keep_random90(int i) { (void)i; rng_state = rng_state * 1103515245u + 12345u; return (rng_state >> 16) % 10 != 0; }
static int keep_burst(int i) { return (i / 50) % 4 != 3 || (i & 1) == 0; } /* odd frames lost in bursts */

static VrsSchedule last_schedule;

static int simulate_from(const VrsSchedule *start, KeepFn keep, int frames, int latency, uint64_t counts[2],
                         uint32_t *guard) {
    VrsSchedule s = *start;
    VrsFeedback fb;
    memset(&fb, 0, sizeof(fb));
    fb.last_eye = -1;
    static int chosen[8192];
    int gap[2] = {0, 0}, worst = 0;
    counts[0] = counts[1] = 0;
    for (int i = 0; i < frames + latency; ++i) {
        if (i < frames) chosen[i] = vrs_schedule_next(&s, &fb);
        const int f = i - latency; /* the frame the presenter sees now */
        if (f < 0 || !keep(f)) continue;
        const int eye = chosen[f];
        ++counts[eye];
        if (fb.last_eye == eye) ++fb.run;
        else { fb.last_eye = eye; fb.run = 1; }
        ++fb.assigned[eye];
        gap[eye] = 0;
        ++gap[1 - eye];
        if (counts[0] && counts[1] && gap[1 - eye] > worst) worst = gap[1 - eye];
    }
    *guard = s.guard_flips;
    last_schedule = s;
    return worst;
}

static int simulate(KeepFn keep, int frames, int latency, uint64_t counts[2], uint32_t *guard) {
    VrsSchedule s;
    vrs_schedule_reset(&s);
    return simulate_from(&s, keep, frames, latency, counts, guard);
}

static void test_presenter_patterns(void) {
    uint64_t counts[2];
    uint32_t guard;
    int worst = simulate(keep_all, 3000, 3, counts, &guard);
    CHECK(worst == 1 && guard == 0, "all frames presented: strict alternation (worst gap %d, guard %u)", worst, guard);
    CHECK(counts[0] == counts[1], "all frames: equal counts (%llu %llu)", (unsigned long long)counts[0], (unsigned long long)counts[1]);
    worst = simulate(keep_random90, 3000, 3, counts, &guard);
    CHECK(worst <= 6, "10%% lost: worst gap %d (guard %u)", worst, guard);
    CHECK(counts[0] > 1200 && counts[1] > 1200, "10%% lost: both eyes fed (%llu %llu)", (unsigned long long)counts[0], (unsigned long long)counts[1]);
    /* A presenter that only ever sees every other frame would starve one eye forever without the
     * guard; with it both eyes keep getting frames (in bursts). */
    const KeepFn decimate[2] = {keep_even, keep_odd};
    for (int k = 0; k < 2; ++k) {
        worst = simulate(decimate[k], 3000, 3, counts, &guard);
        CHECK(counts[0] > 600 && counts[1] > 600, "2:1 decimation (%s): both eyes fed (%llu %llu)",
              k ? "odd" : "even", (unsigned long long)counts[0], (unsigned long long)counts[1]);
        CHECK(worst <= (int)(VRS_STARVE_RUN + VRS_GUARD_HOLD + 6), "2:1 decimation: worst gap %d presented frames", worst);
        CHECK(guard >= 2 && guard < 10 && last_schedule.pairs == 1, "2:1 decimation: pairs (guard %u, pairs %d)", guard,
              last_schedule.pairs);
    }
    /* Pair mode while the presenter sees every frame again: back to strict alternation. */
    VrsSchedule paired;
    vrs_schedule_reset(&paired);
    paired.pairs = 1;
    worst = simulate_from(&paired, keep_all, 3000, 3, counts, &guard);
    CHECK(last_schedule.pairs == 0 && counts[0] == counts[1], "pairs end when every frame is seen (%llu %llu)",
          (unsigned long long)counts[0], (unsigned long long)counts[1]);
    worst = simulate(keep_burst, 3000, 3, counts, &guard);
    CHECK(counts[1] > 800, "odd frames lost in bursts: the right eye still fed (%llu %llu), worst gap %d",
          (unsigned long long)counts[0], (unsigned long long)counts[1], worst);
    printf("PASS presenter patterns (lost, decimated, bursts)\n");
}

static void test_offsets(void) {
    /* Head turned 90 deg left and tilted; eyes 62.5 mm apart along the head's right axis. */
    const VrQuat head = vr_xr_ypr(90 * VR_DEG, 10 * VR_DEG, 5 * VR_DEG);
    const VrVec3 hp = vr_v3(0.3f, 1.6f, -0.2f);
    const VrVec3 right = vr_qrotate(head, vr_v3(1, 0, 0));
    const VrVec3 eye_l = vr_sub(hp, vr_scale(right, 0.03125f)), eye_r = vr_add(hp, vr_scale(right, 0.03125f));
    const VrVec3 ol = vrs_eye_in_head(head, hp, eye_l), orr = vrs_eye_in_head(head, hp, eye_r);
    CHECK(near_v(ol, vr_v3(-0.03125f, 0, 0), 1e-5f), "left eye offset (%f %f %f)", ol.x, ol.y, ol.z);
    CHECK(near_v(orr, vr_v3(0.03125f, 0, 0), 1e-5f), "right eye offset (%f %f %f)", orr.x, orr.y, orr.z);
    CHECK(vrs_offset_ok(ol, VRS_LEFT) && vrs_offset_ok(orr, VRS_RIGHT), "plausible offsets");
    CHECK(!vrs_offset_ok(ol, VRS_RIGHT), "a left offset is not a right eye's");
    CHECK(!vrs_offset_ok(vr_v3(-0.2f, 0, 0), VRS_LEFT), "20 cm is not half an IPD");
    CHECK(!vrs_offset_ok(vr_v3(-0.03f, 0.1f, 0), VRS_LEFT), "10 cm up is not an eye");
    CHECK(!vrs_offset_ok(vr_v3(NAN, 0, 0), VRS_LEFT), "NaN");
    /* The layer pose round-trips to the eye's real position. */
    VrQuat q;
    VrVec3 p;
    vrs_layer_pose(head, hp, ol, &q, &p);
    CHECK(near_v(p, eye_l, 1e-5f) && near(q.w, head.w, 1e-6f), "layer pose = the left eye");
    vrs_layer_pose(head, hp, orr, &q, &p);
    CHECK(near_v(p, eye_r, 1e-5f), "layer pose = the right eye");
    const VrVec3 d = vrs_default_offset(VRS_LEFT, 0.064f);
    CHECK(near_v(d, vr_v3(-0.032f, 0, 0), 1e-7f), "default offset");
    printf("PASS offsets and layer pose\n");
}

static void test_camera_offset(void) {
    /* A game camera heading 30 deg right, pitched 10 deg down, with the head composed (yaw 20 deg
     * left, level): the eye moves along the composed camera's own right axis. */
    float m[4][4], base[4][4];
    game_cam(m, 30 * VR_DEG, -10 * VR_DEG, vr_v3(100.0f, 5.0f, -40.0f));
    vr_cam_compose(m, vr_head_rel_game(vr_xr_ypr(20 * VR_DEG, 0, 0), vr_qidentity()), vr_v3(0, 0, 0), 1);
    memcpy(base, m, sizeof(m));
    const VrVec3 center = vr_row(m, 3), right = vr_row(m, 0);
    float l[4][4], r[4][4];
    memcpy(l, base, sizeof(l));
    memcpy(r, base, sizeof(r));
    const VrVec3 dl = vrs_apply_offset(l, vrs_default_offset(VRS_LEFT, 0.0625f), 1.0f);
    const VrVec3 dr = vrs_apply_offset(r, vrs_default_offset(VRS_RIGHT, 0.0625f), 1.0f);
    CHECK(near_v(dl, vr_scale(right, -0.03125f), 1e-6f), "left: -IPD/2 along right (%f %f %f)", dl.x, dl.y, dl.z);
    CHECK(near_v(dr, vr_scale(right, 0.03125f), 1e-6f), "right: +IPD/2 along right");
    CHECK(near(sqrtf(vr_dot(vr_sub(vr_row(r, 3), vr_row(l, 3)), vr_sub(vr_row(r, 3), vr_row(l, 3)))), 0.0625f, 1e-5f),
          "eyes 62.5 mm apart");
    int rot_same = 1;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) rot_same &= l[i][j] == base[i][j] && r[i][j] == base[i][j];
    CHECK(rot_same, "the offset keeps the rotation rows");
    float rows[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rows[i][j] = base[i][j];
    const float c3[3] = {center.x, center.y, center.z};
    const float l3[3] = {l[3][0], l[3][1], l[3][2]}, r3[3] = {r[3][0], r[3][1], r[3][2]};
    CHECK(vrs_side((const float(*)[3])rows, c3, l3) == -1, "left side");
    CHECK(vrs_side((const float(*)[3])rows, c3, r3) == 1, "right side");
    CHECK(vrs_side((const float(*)[3])rows, c3, c3) == 0, "no offset");
    /* Parallax: a point 4 m ahead of the center camera is right of center in the left eye and
     * left of center in the right eye (screen right = view +x); a point 400 m ahead barely moves. */
    const VrVec3 fwd = vr_row(base, 2);
    const VrVec3 near_point = vr_add(center, vr_scale(fwd, 4.0f)), far_point = vr_add(center, vr_scale(fwd, 400.0f));
    const VrVec3 nl = to_view(l, near_point), nr = to_view(r, near_point);
    const VrVec3 fl = to_view(l, far_point), fr = to_view(r, far_point);
    CHECK(nl.x / nl.z > 0.0f && nr.x / nr.z < 0.0f, "near point: right in L, left in R (%f %f)", nl.x / nl.z, nr.x / nr.z);
    const float near_disp = nl.x / nl.z - nr.x / nr.z, far_disp = fl.x / fl.z - fr.x / fr.z;
    CHECK(near(near_disp, 0.0625f / 4.0f, 1e-5f), "near disparity = IPD/Z (%f)", near_disp);
    CHECK(near_disp > 90.0f * far_disp, "far disparity 100x smaller (%f vs %f)", far_disp, near_disp);
    /* World scale 2: the offset doubles (the world looks half size). */
    memcpy(l, base, sizeof(l));
    const VrVec3 d2 = vrs_apply_offset(l, vrs_default_offset(VRS_LEFT, 0.0625f), 2.0f);
    CHECK(near_v(d2, vr_scale(right, -0.0625f), 1e-6f), "scale 2");
    /* An offset with a forward part (some runtimes put the eye ahead of the head's center). */
    memcpy(l, base, sizeof(l));
    const VrVec3 d3 = vrs_apply_offset(l, vr_v3(-0.031f, 0.0f, -0.01f), 1.0f); /* XR -Z = forward */
    CHECK(near_v(d3, vr_add(vr_scale(right, -0.031f), vr_scale(fwd, 0.01f)), 1e-6f), "forward part goes along the view axis");
    printf("PASS camera offset and parallax\n");
}

static void test_fov_and_tag(void) {
    const float fov = 1.5550f, aspect = 1.4281f;
    const VrFov f = vrs_fov_from_camera(fov, aspect);
    CHECK(near(tanf(f.up), tanf(0.5f * fov), 1e-6f) && near(f.down, -f.up, 1e-7f), "vertical tangent");
    CHECK(near(tanf(f.right), tanf(0.5f * fov) * aspect, 1e-5f) && near(f.left, -f.right, 1e-7f), "horizontal tangent");
    float fy, a;
    vr_fov_from_tangents(tanf(f.right), tanf(f.up), &fy, &a);
    CHECK(near(fy, fov, 1e-5f) && near(a, aspect, 1e-5f), "round trip with vr_fov_from_tangents");
    const float al = vrs_tag_aspect(aspect, VRS_LEFT, 1), ar = vrs_tag_aspect(aspect, VRS_RIGHT, 1);
    CHECK(al == aspect && near(ar / aspect - 1.0f, VRS_TAG, 1e-6f), "right eye tagged (%g)", ar / aspect - 1.0f);
    CHECK(vrs_tag_aspect(aspect, VRS_RIGHT, 0) == aspect, "tag off");
    /* The presenter's check (xr_stereo.h Compare): projection x scale times the view's right
     * tangent is 1 for the eye's own view and 4e-5 off for the other eye's. */
    const float ty = tanf(0.5f * fov);
    const float p00_l = 1.0f / (ty * al), p00_r = 1.0f / (ty * ar);
    const float tx_l = tanf(vrs_fov_from_camera(fov, al).right), tx_r = tanf(vrs_fov_from_camera(fov, ar).right);
    CHECK(fabsf(p00_l * tx_l - 1.0f) < 2e-6f && fabsf(p00_r * tx_r - 1.0f) < 2e-6f, "own tag matches");
    CHECK(fabsf(p00_r * tx_l - 1.0f) > 3e-5f && fabsf(p00_l * tx_r - 1.0f) > 3e-5f, "other eye's tag is 4e-5 off");
    /* Image scale change of the tag: 4e-5 of 1000 px = 0.04 px. */
    CHECK(1000.0f * VRS_TAG < 0.05f, "tag invisible");
    printf("PASS frustum and eye tag\n");
}

static void test_coverage(void) {
    /* Pimax Dream Air (not canted): L -55/46/45/-45, R -46/55/45/-45 degrees. */
    const VrFov eye[2] = {{-55 * VR_DEG, 46 * VR_DEG, 45 * VR_DEG, -45 * VR_DEG},
                          {-46 * VR_DEG, 55 * VR_DEG, 45 * VR_DEG, -45 * VR_DEG}};
    const VrQuat flat[2] = {vr_qidentity(), vr_qidentity()};
    float tx, ty;
    CHECK(vr_union_tangents(flat, eye, &tx, &ty), "union");
    CHECK(near(tx, tanf(55 * VR_DEG), 1e-4f) && near(ty, 1.0f, 1e-4f), "union tangents (%f %f)", tx, ty);
    for (int e = 0; e < 2; ++e) {
        const float c = vrs_coverage(vr_qidentity(), eye[e], tx, ty);
        CHECK(c >= 0.999f, "the union covers eye %d (%f)", e, c);
    }
    /* Each eye wastes the inner side: 2 tan 55 against tan 55 + tan 46 horizontally. */
    const float used = (tanf(55 * VR_DEG) + tanf(46 * VR_DEG)) / (2.0f * tanf(55 * VR_DEG));
    CHECK(used > 0.85f && used < 0.88f, "symmetric frustum: %.0f%% of its width is the eye's", used * 100.0f);
    /* The game's 43 deg camera covers far less. */
    const float c43 = vrs_coverage(vr_qidentity(), eye[0], tanf(21.5f * VR_DEG) * 1.7778f, tanf(21.5f * VR_DEG));
    CHECK(c43 < 0.6f, "43 deg does not cover (%f)", c43);
    /* Canted 10 deg outward (Pimax 8KX style): the union around the head axis still covers. */
    const VrQuat cant[2] = {vr_qaxis(vr_v3(0, 1, 0), 10 * VR_DEG), vr_qaxis(vr_v3(0, 1, 0), -10 * VR_DEG)};
    CHECK(vr_union_tangents(cant, eye, &tx, &ty), "canted union");
    CHECK(vrs_coverage(cant[0], eye[0], tx, ty) >= 0.999f && vrs_coverage(cant[1], eye[1], tx, ty) >= 0.999f,
          "canted eyes covered");
    printf("PASS frustum coverage (Dream Air, canted)\n");
}

static void test_offaxis(void) {
    /* Dream Air left eye: -55/46/45/-45 deg, parallel displays. */
    const VrFov left = {-55 * VR_DEG, 46 * VR_DEG, 45 * VR_DEG, -45 * VR_DEG};
    VrsBounds b;
    CHECK(vrs_eye_bounds(vr_qidentity(), left, &b), "bounds");
    CHECK(near(b.l, tanf(-55 * VR_DEG), 1e-5f) && near(b.r, tanf(46 * VR_DEG), 1e-5f) && near(b.u, 1.0f, 1e-5f) &&
              near(b.d, -1.0f, 1e-5f), "parallel display: the eye's own tangents (%f %f %f %f)", b.l, b.r, b.d, b.u);
    const float ty = 1.0f, fov_y = 2.0f * atanf(ty);
    float aspect, shift;
    vrs_offaxis(b.l, b.r, ty, &aspect, &shift);
    CHECK(near(aspect, (b.r - b.l) / 2.0f, 1e-6f), "aspect = half-width / ty");
    CHECK(near(shift, 0.15936f, 1e-4f), "left eye shift %f (to the right in NDC)", shift);
    /* The shifted projection maps the eye's edges to NDC -1 and +1. */
    const float sx = 1.0f / (ty * aspect);
    CHECK(near(b.r * sx + shift, 1.0f, 1e-5f) && near(b.l * sx + shift, -1.0f, 1e-5f), "edges to -1 and +1");
    const VrFov f = vrs_fov_shifted(fov_y, aspect, shift);
    CHECK(near(f.left, -55 * VR_DEG, 1e-5f) && near(f.right, 46 * VR_DEG, 1e-5f) && near(f.up, 45 * VR_DEG, 1e-5f),
          "the layer gets the eye's own frustum (%f %f)", f.left / VR_DEG, f.right / VR_DEG);
    /* Mirrored right eye: the same aspect, the opposite shift. */
    const VrFov right = {-46 * VR_DEG, 55 * VR_DEG, 45 * VR_DEG, -45 * VR_DEG};
    VrsBounds br;
    float aspect_r, shift_r;
    CHECK(vrs_eye_bounds(vr_qidentity(), right, &br), "right bounds");
    vrs_offaxis(br.l, br.r, ty, &aspect_r, &shift_r);
    CHECK(near(aspect_r, aspect, 1e-6f) && near(shift_r, -shift, 1e-6f), "mirror");
    /* Pixels: the symmetric frustum needs 2 tan 55 of width, the off-axis one tan 55 + tan 46. */
    CHECK(near((b.r - b.l) / (2.0f * tanf(55 * VR_DEG)), 0.8627f, 1e-3f), "13.7%% fewer pixels for the same density");
    /* Recognizing the built projection, with the tag. */
    const float p_l[16] = {sx, 0, 0, 0, 0, 1.0f / ty, 0, 0, 0, 0, 1.0f, 1, 0, 0, -0.05f, 0};
    const float a_r = vrs_tag_aspect(aspect, VRS_RIGHT, 1);
    CHECK(vrs_projection_is(p_l, 1.0f / (ty * aspect), 1.0f / ty), "the left eye's projection");
    CHECK(!vrs_projection_is(p_l, 1.0f / (ty * a_r), 1.0f / ty), "not the right eye's (tag)");
    CHECK(!vrs_projection_is(p_l, 1.0f / (ty * aspect), 1.0f / 0.9f), "not another vertical FOV");
    /* Canted 10 deg outward: the bounds around the head axis widen outward and stay a frustum. */
    VrsBounds bc;
    CHECK(vrs_eye_bounds(vr_qaxis(vr_v3(0, 1, 0), 10 * VR_DEG), left, &bc), "canted bounds");
    CHECK(bc.l < b.l && bc.r < b.r && bc.u > 1.0f, "canted: shifted left, taller at the corners (%f %f %f)", bc.l, bc.r, bc.u);
    printf("PASS off-axis frustum\n");
}

static void test_control(void) {
    VrsControl c;
    vrs_control_parse("", &c);
    CHECK(c.alt == 1 && c.ipd == 0.0f && c.scale == 1.0f && c.invert == 0 && c.tag == 1, "defaults");
    vrs_control_parse("alt=0 ipd=0.3, scale=2 invert=1 tag=0 step=7 junk\n", &c);
    CHECK(c.alt == 0 && near(c.ipd, 0.3f, 1e-6f) && c.scale == 2.0f && c.invert == 1 && c.tag == 0, "all keys");
    vrs_control_parse("ipd=-1 scale=100 alt=x", &c);
    CHECK(c.ipd == 0.0f && c.scale == 1.0f && c.alt == 1, "out of range values fall back");
    vrs_control_parse("altitude=0 ipdx=0.5", &c);
    CHECK(c.alt == 1 && c.ipd == 0.0f, "keys must match exactly");
    CHECK(c.offaxis == -1 && c.h2 == -1 && c.eye_l_deg == 0.0f, "offaxis/h2 default to the environment");
    vrs_control_parse("offaxis=1 eyel=-54.6 eyer=46 h2=0", &c);
    CHECK(c.offaxis == 1 && c.h2 == 0 && near(c.eye_l_deg, -54.6f, 1e-4f) && near(c.eye_r_deg, 46.0f, 1e-4f), "off-axis keys");
    vrs_control_parse("eyel=10 eyer=-5", &c);
    CHECK(c.eye_l_deg == 0.0f && c.eye_r_deg == 0.0f, "edges on the wrong side are ignored");
    printf("PASS control file\n");
}

int main(void) {
    test_alternation();
    test_presenter_patterns();
    test_offsets();
    test_camera_offset();
    test_fov_and_tag();
    test_coverage();
    test_offaxis();
    test_control();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
