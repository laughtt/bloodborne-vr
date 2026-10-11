/* Unit tests for src/vr_visceral_math.h, the VR visceral's window and thrust logic
 * (src/runtime_vr_visceral.c, docs/vr/findings/41-parry.md section 7).
 * Build: ninja -C out/gpu vr-visceral-test (no GPU, no game). Prints PASS lines; exit 0. */
#include "vr_visceral_math.h"
#include <stdio.h>

/* The checks below were written for a 250 ms delay (the default is 400 ms, checked in main). */
static VvConfig test_cfg(void) {
    VvConfig c = vv_defaults();
    c.delay_ms = 250.0f;
    return c;
}

static int failures, checks;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

static const VrVec3 TARGET = {0.0f, 1.0f, 2.0f};

/* Head at the origin, hand 0.5 m ahead, both tracked; the hand moves with velocity `rel` along +Z. */
static VvInput frame(double ms, float thrust, VrVec3 hand, int blocked) {
    VvInput in;
    memset(&in, 0, sizeof(in));
    in.now_ms = ms;
    in.head_valid = in.hand_valid = in.have_rel = 1;
    in.head = vr_v3(0, 1.6f, 0);
    in.hand = hand;
    in.rel_vel = vr_v3(0, 0, thrust);
    in.hand_fwd = vr_v3(0, 0, 1);
    in.blocked = blocked;
    return in;
}
static VvInput still(double ms, VrVec3 head, VrVec3 hand) {
    VvInput in = frame(ms, 0.0f, hand, 0);
    in.head = head;
    return in;
}
static VvState opened(const VvConfig *c, double ms) {
    VvState s;
    vv_init(&s);
    vv_on_shot(&s, c, ms, 1, TARGET);
    return s;
}

static void test_auto(void) {
    VvConfig c = test_cfg();
    VvState s = opened(&c, 1000.0);
    CHECK(s.open && s.have_target, "window open");
    const VrVec3 near_hand = vr_v3(0.0f, 1.2f, 1.2f), far = vr_v3(0, 1.6f, -3.0f);
    VvInput in = still(1100.0, far, near_hand);
    CHECK(vv_step(&s, &c, &in) == 0, "no fire before delay_ms");
    in = still(1260.0, far, far);
    CHECK(vv_step(&s, &c, &in) == 0, "no fire when both are away");
    in = still(1300.0, far, near_hand);
    CHECK(vv_step(&s, &c, &in) == 1, "fire within reach after the delay");
    CHECK(!s.open && s.fired_how == VV_MODE_AUTO && s.fired_by_hand, "closed by the fire, hand");
    CHECK(vv_take_close(&s) == VV_CLOSE_FIRED, "close reason fired");
    CHECK(vv_pressed(&s, 1350.0) && !vv_pressed(&s, 1400.0), "press held 100 ms");
    in = still(1320.0, far, near_hand);
    CHECK(vv_step(&s, &c, &in) == 0, "once per window");
    /* the head alone counts, horizontal only (height ignored) */
    s = opened(&c, 0.0);
    in = still(500.0, vr_v3(0.5f, 9.0f, 1.5f), far);
    CHECK(vv_step(&s, &c, &in) == 1 && !s.fired_by_hand, "head within reach, height ignored");
    /* never after the window */
    s = opened(&c, 0.0);
    in = still(2600.0, far, near_hand);
    CHECK(vv_step(&s, &c, &in) == 0 && !s.open, "no fire after window_ms");
    CHECK(vv_take_close(&s) == VV_CLOSE_EXPIRED, "expired");
    /* a closed window does nothing at all */
    in = still(100.0, far, near_hand);
    CHECK(vv_step(&s, &c, &in) == 0, "closed: nothing");
    if (!failures) printf("PASS: auto (delay, reach, horizontal, window, once)\n");
}

static void test_confirm_hit(void) {
    VvConfig c = test_cfg();
    c.confirm = VV_CONFIRM_HIT;
    const VrVec3 near_hand = vr_v3(0.0f, 1.2f, 1.2f), far = vr_v3(0, 1.6f, -3.0f);
    VvState s = opened(&c, 0.0);
    VvInput in = still(400.0, far, near_hand);
    CHECK(vv_step(&s, &c, &in) == 0, "waits for the hit");
    CHECK(vv_on_hit(&s, &c, 500.0, vr_v3(0, 1, 2.1f)) == 1 && s.confirmed, "hit confirms");
    CHECK(vv_on_hit(&s, &c, 520.0, TARGET) == 0, "second hit is not new");
    in = still(600.0, far, near_hand);
    CHECK(vv_step(&s, &c, &in) == 1, "fires once confirmed");
    s = opened(&c, 0.0);
    CHECK(vv_on_hit(&s, &c, 700.0, TARGET) == 0 && !s.confirmed, "hit after hit_ms does not confirm");
    in = still(800.0, far, near_hand);
    CHECK(vv_step(&s, &c, &in) == 0, "never confirmed: no fire");
    /* a hit near the target moves it, a far one does not */
    s = opened(&c, 0.0);
    vv_on_hit(&s, &c, 100.0, vr_v3(1.0f, 1.0f, 2.0f));
    CHECK(fabsf(s.target.x - 1.0f) < 1e-5f, "near hit moves the target");
    s = opened(&c, 0.0);
    vv_on_hit(&s, &c, 100.0, vr_v3(9.0f, 1.0f, 2.0f));
    CHECK(fabsf(s.target.x) < 1e-5f && s.confirmed, "far hit confirms but keeps the target");
    if (!failures) printf("PASS: confirm HIT (waits, time limit, target follows a near hit)\n");
}

/* Runs a thrust profile: returns the number of fires; mid-run state in *s. */
static int run(VvState *s, const VvConfig *c, double t0, const float *thrusts, int n, VrVec3 hand, double dt) {
    int fires = 0;
    for (int i = 0; i < n; ++i) {
        VvInput in = frame(t0 + dt * i, thrusts[i], hand, 0);
        fires += vv_step(s, c, &in);
    }
    return fires;
}

static void test_thrust(void) {
    VvConfig c = test_cfg();
    c.mode = VV_MODE_THRUST;
    const VrVec3 hand = vr_v3(0, 1.0f, 1.0f); /* 1 m from the target, toward +Z */
    const float good[] = {0.0f, 0.4f, 1.0f, 2.0f, 2.5f, 1.0f, 0.2f};
    VvState s = opened(&c, 0.0);
    CHECK(run(&s, &c, 300.0, good, 7, hand, 16.0) == 1, "a good thrust after the delay fires once");
    CHECK(s.fired_how == VV_MODE_THRUST && s.fired_t >= c.speed_mps && s.fired_a > 0.99f, "thrust numbers");
    /* a thrust before the delay does nothing and does not use up the arming */
    s = opened(&c, 0.0);
    CHECK(run(&s, &c, 20.0, good, 7, hand, 16.0) == 0 && s.armed, "no fire before delay, still armed");
    CHECK(run(&s, &c, 400.0, good, 7, hand, 16.0) == 1, "then the next thrust fires");
    /* too slow */
    s = opened(&c, 0.0);
    const float slow[] = {0.0f, 0.4f, 0.8f, 1.2f, 0.9f, 0.1f};
    CHECK(run(&s, &c, 300.0, slow, 6, hand, 16.0) == 0, "too slow");
    CHECK(vv_take_report(&s) && (s.report_bits & VV_REJ_SPEED), "report names speed");
    /* misaligned: the stroke runs sideways of the target (hand aims +Z, target to the side) */
    s = opened(&c, 0.0);
    CHECK(run(&s, &c, 300.0, good, 7, vr_v3(3.0f, 1.0f, 0.0f), 16.0) == 0, "stroke not toward the target");
    char b[64];
    CHECK(vv_take_report(&s), "report ready");
    CHECK((s.report_bits & VV_REJ_ALIGN) && (s.report_bits & VV_REJ_REACH), "align and reach named: %s",
          vv_reject_names(s.report_bits, b, sizeof(b)));
    /* falling: only falling frames above the speed */
    s = opened(&c, 0.0);
    VvInput in = frame(300.0, 3.0f, hand, 0);
    s.prev_t = 4.0f;
    CHECK(vv_step(&s, &c, &in) == 0, "falling edge does not fire");
    in = frame(316.0, 3.5f, hand, 0);
    s.armed = 1;
    CHECK(vv_step(&s, &c, &in) == 1, "rising does");
    /* reach */
    s = opened(&c, 0.0);
    CHECK(run(&s, &c, 300.0, good, 7, vr_v3(0, 1.0f, -1.0f), 16.0) == 0, "hand 3 m from the target: out of reach");
    /* debounce: a second window right after the first fire is refused for 300 ms */
    c.debounce_ms = 1000.0f;
    s = opened(&c, 0.0);
    CHECK(run(&s, &c, 300.0, good, 7, hand, 16.0) == 1, "first");
    vv_on_shot(&s, &c, 400.0, 1, TARGET);
    CHECK(run(&s, &c, 650.0, good, 7, hand, 16.0) == 0, "debounced (fired at 348, 1000 ms)");
    c.debounce_ms = 300.0f;
    CHECK(vv_take_report(&s) && (s.report_bits & VV_REJ_DEBOUNCE), "debounce named");
    /* disarm on blocked, re-arm below rearm */
    s = opened(&c, 0.0);
    in = frame(300.0, 0.0f, hand, 1);
    vv_step(&s, &c, &in);
    CHECK(!s.armed, "blocked disarms");
    in = frame(316.0, 0.2f, hand, 0);
    vv_step(&s, &c, &in);
    CHECK(s.armed, "re-armed below rearm");
    s.armed = 0;
    in = frame(332.0, 2.5f, hand, 0);
    CHECK(vv_step(&s, &c, &in) == 0 && !s.armed, "disarmed: a fast frame cannot fire");
    /* blocked frame never fires even armed */
    s = opened(&c, 0.0);
    s.prev_t = 0.0f;
    in = frame(300.0, 3.0f, hand, 1);
    CHECK(vv_step(&s, &c, &in) == 0, "blocked frame");
    /* not live (HIT confirm unmet) uses no arming */
    c.confirm = VV_CONFIRM_HIT;
    s = opened(&c, 0.0);
    CHECK(run(&s, &c, 300.0, good, 7, hand, 16.0) == 0 && !vv_take_report(&s), "unconfirmed: no fire, no report");
    /* after the window: nothing */
    c.confirm = VV_CONFIRM_AIM;
    s = opened(&c, 0.0);
    CHECK(run(&s, &c, 2700.0, good, 7, hand, 16.0) == 0, "after the window");
    /* no target: aligned to the hand's fwd, but no window opens */
    vv_init(&s);
    CHECK(vv_on_shot(&s, &c, 0.0, 0, TARGET) == VV_SHOT_NO_TARGET && !s.open, "no target: no window");
    if (!failures) printf("PASS: thrust gate (speed, align, falling, debounce, reach, disarm, re-arm, delay)\n");
}

static void test_swing_and_shots(void) {
    VvConfig c = test_cfg();
    c.mode = VV_MODE_SWING;
    VvState s;
    vv_init(&s);
    CHECK(vv_on_shot(&s, &c, 0.0, 1, TARGET) == VV_SHOT_IGNORED && !s.open, "swing: no window");
    const float good[] = {0.0f, 0.4f, 1.0f, 2.0f, 2.5f, 1.0f, 0.2f};
    CHECK(run(&s, &c, 300.0, good, 7, vr_v3(0, 1.0f, 1.0f), 16.0) == 0, "swing never fires");
    c.mode = VV_MODE_AUTO;
    s = opened(&c, 0.0);
    CHECK(vv_on_shot(&s, &c, 100.0, 1, vr_v3(5, 1, 5)) == VV_SHOT_REPLACED && s.open, "a new shot replaces");
    CHECK(vv_take_close(&s) == VV_CLOSE_REPLACED && fabsf(s.target.x - 5.0f) < 1e-5f && s.opened_ms == 100.0, "replaced reason, new target");
    CHECK(vv_on_shot(&s, &c, 200.0, 0, TARGET) == VV_SHOT_NO_TARGET && s.open && vv_take_close(&s) == VV_CLOSE_NONE,
          "a shot without a target leaves the window open");
    char b[64];
    CHECK(!strcmp(vv_reject_names(0, b, sizeof(b)), "none") && !strcmp(vv_reject_names(3, b, sizeof(b)), "speed,align"), "names");
    if (!failures) printf("PASS: swing never fires, shots open/replace/close\n");
}

static void test_track(void) {
    VvConfig c = test_cfg();
    VvState s = opened(&c, 0.0);
    const VrVec3 moved[3] = {vr_v3(8, 1, 8), vr_v3(0.8f, 0.2f, 2.5f), vr_v3(0.0f, 1.0f, 3.2f)};
    CHECK(vv_track(&s, &c, moved, 3) == 1, "follows the nearest");
    CHECK(fabsf(s.target.x - 0.8f) < 1e-5f && fabsf(s.target.y - 0.2f) < 1e-5f, "target is the 3D point of the nearest horizontal");
    const VrVec3 far[1] = {vr_v3(8, 1, 8)};
    CHECK(vv_track(&s, &c, far, 1) == 0 && fabsf(s.target.x - 0.8f) < 1e-5f, "far candidates ignored");
    CHECK(vv_track(&s, &c, far, 0) == 0, "empty list");
    vv_close(&s, VV_CLOSE_EXPIRED);
    CHECK(vv_track(&s, &c, moved, 3) == 0, "closed window is not tracked");
    if (!failures) printf("PASS: track follows a moved candidate, ignores far ones\n");
}

static void test_press(void) {
    VvConfig c = test_cfg();
    c.press_ms = 150.0f;
    VvState s = opened(&c, 0.0);
    VvInput in = still(300.0, vr_v3(0, 1, 1.5f), vr_v3(0, 1, -5));
    CHECK(vv_step(&s, &c, &in) == 1, "fires");
    CHECK(vv_pressed(&s, 300.0) && vv_pressed(&s, 449.0) && !vv_pressed(&s, 450.0), "press_ms honored");
    if (!failures) printf("PASS: press duration\n");
}

int main(void) {
    CHECK(vv_defaults().delay_ms == 400.0f && vv_defaults().mode == VV_MODE_AUTO && vv_defaults().reach_m == 2.0f, "defaults: auto, 400 ms delay, 2.0 m reach");
    test_auto();
    test_confirm_hit();
    test_thrust();
    test_swing_and_shots();
    test_track();
    test_press();
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("All VR visceral tests passed (%d checks)\n", checks);
    return 0;
}
