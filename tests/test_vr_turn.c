/* Unit tests for src/vr_turn_math.h, the VR turn and heading model the game's first person and the
 * 3D sandbox share (docs/vr/findings/16-first-person.md section 9). Written in the common subset
 * of C and C++: tests/test_vr_turn_cpp.cpp compiles this same file as C++ (the sandbox's
 * language). Build: ninja -C out/gpu vr-turn-test vr-turn-test-cpp (no GPU, no game). Prints PASS
 * lines; exit 0. */
#include "vr_turn_math.h"
#include <stdio.h>

static int failures, checks;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

static int near_f(float a, float b, float eps) { return fabsf(a - b) <= eps; }
static float deg(float r) { return r / VRT_DEG; }
/* The difference of two headings, wrapped. */
static float dyaw(float a, float b) { return vrt_wrap(a - b); }

static VrTurnInput input(float dt, float stick_x, float head_yaw) {
    VrTurnInput in;
    vrt_input_clear(&in);
    in.dt = dt;
    in.stick_x = stick_x;
    in.head_yaw = head_yaw;
    return in;
}

static void test_wrap(void) {
    CHECK(near_f(vrt_wrap(190 * VRT_DEG), -170 * VRT_DEG, 1e-5f), "190 -> -170: %f", deg(vrt_wrap(190 * VRT_DEG)));
    CHECK(near_f(vrt_wrap(-190 * VRT_DEG), 170 * VRT_DEG, 1e-5f), "-190 -> 170");
    CHECK(near_f(vrt_wrap(721 * VRT_DEG), 1 * VRT_DEG, 1e-4f), "721 -> 1: %f", deg(vrt_wrap(721 * VRT_DEG)));
    CHECK(vrt_wrap(VRT_PI) == -VRT_PI, "pi -> -pi (half-open range)");
    CHECK(vrt_wrap(0.25f) == 0.25f, "in range: unchanged, bit for bit");
    volatile float big = 1e38f, minus_one = -1.0f;
    const float inf = big * big, not_a_number = sqrtf(minus_one);
    CHECK(vrt_wrap(not_a_number) == 0.0f && vrt_wrap(inf) == 0.0f && vrt_wrap(-inf) == 0.0f, "not finite -> 0");
    for (int i = -2000; i <= 2000; ++i) {
        const float w = vrt_wrap((float)i * 0.0137f * VRT_PI);
        if (!(w >= -VRT_PI && w < VRT_PI)) { CHECK(0, "out of range at %d: %f", i, w); break; }
    }
    printf("PASS wrap\n");
}

static void test_snap(void) {
    VrTurnConfig c;
    vrt_config_default(&c);
    VrTurnState s;
    vrt_reset(&s, 0.0f);
    VrTurnOutput o;
    VrTurnInput in = input(0.011f, 0.7f, 0.0f);
    vrt_update(&s, &c, &in, &o);
    CHECK(o.snapped == 1 && near_f(o.psi, 30 * VRT_DEG, 1e-6f) && near_f(o.turn_delta, 30 * VRT_DEG, 1e-6f), "flick right: +30 (%f)", deg(o.psi));
    in.stick_x = 1.0f;
    for (int i = 0; i < 20; ++i) vrt_update(&s, &c, &in, &o); /* 0.22 s held */
    CHECK(near_f(o.psi, 30 * VRT_DEG, 1e-6f), "held: no second snap before 0.3 s (%f)", deg(o.psi));
    for (int i = 0; i < 10; ++i) vrt_update(&s, &c, &in, &o); /* 0.33 s */
    CHECK(near_f(s.psi, 60 * VRT_DEG, 1e-6f), "held: repeats after 0.3 s (%f)", deg(s.psi));
    in.stick_x = 0.5f;
    vrt_update(&s, &c, &in, &o);
    CHECK(o.snapped == 0 && near_f(s.psi, 60 * VRT_DEG, 1e-6f), "between the zones: nothing");
    in.stick_x = 0.1f;
    vrt_update(&s, &c, &in, &o);
    CHECK(s.snap_armed, "the centre re-arms");
    in.stick_x = -0.9f;
    vrt_update(&s, &c, &in, &o);
    CHECK(o.snapped == -1 && near_f(s.psi, 30 * VRT_DEG, 1e-6f), "flick left: -30");
    in.stick_x = 0.9f;
    vrt_update(&s, &c, &in, &o);
    CHECK(o.snapped == 1 && near_f(s.psi, 60 * VRT_DEG, 1e-6f), "a reversal snaps at once");
    /* Wrap-around: 6 more snaps right from 60 = 240 = -120. */
    for (int k = 0; k < 6; ++k) {
        in.stick_x = 0.0f;
        vrt_update(&s, &c, &in, &o);
        in.stick_x = 1.0f;
        vrt_update(&s, &c, &in, &o);
        CHECK(near_f(o.turn_delta, 30 * VRT_DEG, 1e-5f), "every snap is +30 across +-180 (%f)", deg(o.turn_delta));
    }
    CHECK(near_f(s.psi, -120 * VRT_DEG, 1e-4f), "wrapped: %f", deg(s.psi));
    /* Snap mode never turns smoothly. */
    CHECK(o.turn_rate == 0.0f, "no smooth rate in snap mode");
    printf("PASS snap\n");
}

static void test_smooth(void) {
    VrTurnConfig c;
    vrt_config_default(&c);
    c.turn = VRT_TURN_SMOOTH;
    VrTurnState s;
    vrt_reset(&s, 0.0f);
    VrTurnOutput o;
    const float dt = 1.0f / 90.0f;
    /* Inside the dead zone: nothing, bit for bit, for a long time (no drift). */
    VrTurnInput in = input(dt, 0.149f, 0.3f);
    for (int i = 0; i < 100000; ++i) vrt_update(&s, &c, &in, &o);
    CHECK(s.psi == 0.0f && o.turn_rate == 0.0f, "dead zone: psi stays exactly 0 (%g)", s.psi);
    /* Full right for 1 s, then released: the ramps up and down cancel, 90 deg in all. */
    in.stick_x = 1.0f;
    float max_rate = 0.0f;
    for (int i = 0; i < 90; ++i) {
        vrt_update(&s, &c, &in, &o);
        max_rate = o.turn_rate > max_rate ? o.turn_rate : max_rate;
        if (i == 3) CHECK(o.turn_rate > 0.0f && o.turn_rate < 90 * VRT_DEG, "accelerating: %f deg/s", deg(o.turn_rate));
    }
    CHECK(near_f(max_rate, 90 * VRT_DEG, 1e-4f), "full deflection: 90 deg/s (%f)", deg(max_rate));
    in.stick_x = 0.0f;
    for (int i = 0; i < 30; ++i) vrt_update(&s, &c, &in, &o);
    CHECK(o.turn_rate == 0.0f, "released: the rate returns to exactly 0");
    CHECK(near_f(s.psi, 90 * VRT_DEG, 1.5f * VRT_DEG), "1 s at 90 deg/s: %f deg", deg(s.psi));
    const float rest = s.psi;
    for (int i = 0; i < 10000; ++i) vrt_update(&s, &c, &in, &o);
    CHECK(s.psi == rest, "at rest: no drift");
    /* Half deflection: half the rate beyond the dead zone (linear). */
    vrt_reset(&s, 0.0f);
    in.stick_x = -(0.15f + 0.5f * 0.85f);
    for (int i = 0; i < 45; ++i) vrt_update(&s, &c, &in, &o);
    CHECK(near_f(o.turn_rate, -45 * VRT_DEG, 1e-3f), "half deflection left: -45 deg/s (%f)", deg(o.turn_rate));
    /* A full circle at full rate comes back to the start, through +-180 smoothly. */
    vrt_reset(&s, 170 * VRT_DEG);
    c.accel_s = 0.0f;
    in.stick_x = 1.0f;
    float biggest = 0.0f;
    for (int i = 0; i < 360; ++i) {
        vrt_update(&s, &c, &in, &o);
        biggest = fabsf(o.turn_delta) > biggest ? fabsf(o.turn_delta) : biggest;
    }
    CHECK(near_f(dyaw(s.psi, 170 * VRT_DEG), 0.0f, 0.05f * VRT_DEG), "4 s at 90 deg/s: a full circle (%f)", deg(dyaw(s.psi, 170 * VRT_DEG)));
    CHECK(near_f(biggest, 1.0f * VRT_DEG, 1e-4f), "1 deg per frame, also across +-180 (%f)", deg(biggest));
    /* A hitch: a 5 s frame counts as max_dt. */
    vrt_reset(&s, 0.0f);
    in.dt = 5.0f;
    vrt_update(&s, &c, &in, &o);
    CHECK(near_f(s.psi, 9 * VRT_DEG, 1e-4f), "a 5 s frame turns 0.1 s worth: %f", deg(s.psi));
    /* expo: m^2 at half deflection = a quarter of the rate. */
    c.expo = 2.0f;
    CHECK(near_f(vrt_smooth_rate(&c, 0.15f + 0.5f * 0.85f), 22.5f * VRT_DEG, 1e-4f), "expo 2");
    printf("PASS smooth\n");
}

static void test_head_and_move(void) {
    VrTurnConfig c;
    vrt_config_default(&c);
    VrTurnState s;
    vrt_reset(&s, 100 * VRT_DEG);
    VrTurnOutput o;
    /* The head turns the view freely; psi and the body-relative move do not follow. */
    for (int i = -180; i <= 180; i += 15) {
        VrTurnInput in = input(0.011f, 0.0f, (float)i * VRT_DEG);
        vrt_update(&s, &c, &in, &o);
        CHECK(o.psi == vrt_wrap(100 * VRT_DEG) && o.turn_delta == 0.0f, "head %d: psi unchanged", i);
        CHECK(near_f(dyaw(o.view_yaw, (100.0f + (float)i) * VRT_DEG), 0.0f, 1e-5f), "head %d: view = psi + head", i);
        CHECK(o.move_yaw == o.psi, "move=body: the move yaw is psi");
    }
    c.move = VRT_MOVE_HEAD;
    VrTurnInput in = input(0.011f, 0.0f, -40 * VRT_DEG);
    vrt_update(&s, &c, &in, &o);
    CHECK(near_f(o.move_yaw, 60 * VRT_DEG, 1e-5f), "move=head: psi + head (%f)", deg(o.move_yaw));
    c.move = VRT_MOVE_CONTROLLER;
    in.controller_valid = 1;
    in.controller_yaw = 30 * VRT_DEG;
    vrt_update(&s, &c, &in, &o);
    CHECK(near_f(o.move_yaw, 130 * VRT_DEG, 1e-5f), "move=controller: psi + controller (%f)", deg(o.move_yaw));
    in.controller_valid = 0;
    vrt_update(&s, &c, &in, &o);
    CHECK(near_f(o.move_yaw, 60 * VRT_DEG, 1e-5f), "controller lost: the head");
    /* The stick as a world direction. */
    float r, f;
    vrt_move_vector(90 * VRT_DEG, 0.0f, 1.0f, &r, &f);
    CHECK(near_f(r, 1.0f, 1e-6f) && near_f(f, 0.0f, 1e-6f), "heading 90, stick up: walks right (%f %f)", r, f);
    vrt_move_vector(0.0f, 1.0f, 0.0f, &r, &f);
    CHECK(near_f(r, 1.0f, 1e-6f) && near_f(f, 0.0f, 1e-6f), "heading 0, stick right: right");
    vrt_move_vector(90 * VRT_DEG, 1.0f, 0.0f, &r, &f);
    CHECK(near_f(r, 0.0f, 1e-6f) && near_f(f, -1.0f, 1e-6f), "heading 90, stick right: back");
    vrt_move_vector(-30 * VRT_DEG, 0.6f, -0.8f, &r, &f);
    CHECK(near_f(r * r + f * f, 1.0f, 1e-5f), "magnitude kept");
    printf("PASS head and move\n");
}

static void test_recenter(void) {
    VrTurnConfig c;
    vrt_config_default(&c);
    VrTurnState s;
    vrt_reset(&s, 10 * VRT_DEG);
    VrTurnOutput o;
    /* The user faces 40 deg right of the reference, then recenters there: the reference moves
     * +40 and the head's relative yaw drops to 0 in the same frame. */
    VrTurnInput in = input(0.011f, 0.0f, 40 * VRT_DEG);
    in.ref_yaw = 0.0f;
    vrt_update(&s, &c, &in, &o);
    const float view = o.view_yaw;
    CHECK(near_f(view, 50 * VRT_DEG, 1e-5f) && !o.recentered, "before: view 50");
    in.ref_yaw = 40 * VRT_DEG;
    in.head_yaw = 0.0f;
    vrt_update(&s, &c, &in, &o);
    CHECK(o.recentered && near_f(o.view_yaw, view, 1e-5f), "recenter keeps the view (%f)", deg(o.view_yaw));
    CHECK(near_f(o.psi, 50 * VRT_DEG, 1e-5f) && near_f(o.turn_delta, 40 * VRT_DEG, 1e-5f), "psi took the 40 deg (%f)", deg(o.psi));
    CHECK(o.move_yaw == o.psi, "body forward = where the user now faces");
    vrt_update(&s, &c, &in, &o);
    CHECK(!o.recentered && o.turn_delta == 0.0f, "once");
    /* Across +-180 with the reference wrapping. */
    in.ref_yaw = 170 * VRT_DEG;
    vrt_update(&s, &c, &in, &o);
    in.ref_yaw = -170 * VRT_DEG;
    const float before = s.psi;
    vrt_update(&s, &c, &in, &o);
    CHECK(near_f(dyaw(s.psi, before), 20 * VRT_DEG, 1e-4f), "the reference crossing 180: +20 (%f)", deg(dyaw(s.psi, before)));
    /* recenter=reset: psi stays, the view jumps to the body's forward. */
    vrt_config_set(&c, "recenter", "reset");
    vrt_reset(&s, 10 * VRT_DEG);
    in.ref_yaw = 0.0f;
    in.head_yaw = 40 * VRT_DEG;
    vrt_update(&s, &c, &in, &o);
    in.ref_yaw = 40 * VRT_DEG;
    in.head_yaw = 0.0f;
    vrt_update(&s, &c, &in, &o);
    CHECK(o.recentered && near_f(o.psi, 10 * VRT_DEG, 1e-6f) && near_f(o.view_yaw, 10 * VRT_DEG, 1e-6f), "reset: the view returns to psi");
    /* The first update takes the reference without calling it a recenter. */
    vrt_reset(&s, 0.0f);
    in.ref_yaw = 77 * VRT_DEG;
    vrt_update(&s, &c, &in, &o);
    CHECK(!o.recentered && o.psi == 0.0f, "first reference: no recenter");
    printf("PASS recenter\n");
}

static void test_lock(void) {
    VrTurnConfig c;
    vrt_config_default(&c);
    c.turn = VRT_TURN_SMOOTH;
    VrTurnState s;
    vrt_reset(&s, 0.0f);
    VrTurnOutput o;
    VrTurnInput in = input(0.011f, 1.0f, 0.0f);
    in.locked = 1;
    /* VIEW (default): the stick goes to the game, psi stays, the game camera still follows the
     * move yaw. */
    for (int i = 0; i < 50; ++i) vrt_update(&s, &c, &in, &o);
    CHECK(o.stick_to_game && o.sync_game && s.psi == 0.0f, "lock=view: stick to the game, psi kept");
    /* The lock ends with the stick still pushed: smooth turning resumes from rest. */
    in.locked = 0;
    vrt_update(&s, &c, &in, &o);
    CHECK(!o.stick_to_game && o.turn_rate > 0.0f && o.turn_rate < 90 * VRT_DEG, "after the lock: turning ramps up");
    /* Snap mode: after the lock, a held stick must come back to the centre first. */
    c.turn = VRT_TURN_SNAP;
    vrt_reset(&s, 0.0f);
    in.locked = 1;
    vrt_update(&s, &c, &in, &o);
    in.locked = 0;
    vrt_update(&s, &c, &in, &o);
    CHECK(o.snapped == 0 && s.psi == 0.0f, "no snap from a stick held through the lock");
    in.stick_x = 0.0f;
    vrt_update(&s, &c, &in, &o);
    in.stick_x = 1.0f;
    vrt_update(&s, &c, &in, &o);
    CHECK(o.snapped == 1, "re-armed in the centre");
    /* GAME: the game camera is released to the lock-on. TURN: the stick keeps turning. */
    in.locked = 1;
    c.lock = VRT_LOCK_GAME;
    vrt_update(&s, &c, &in, &o);
    CHECK(o.stick_to_game && !o.sync_game, "lock=game: camera released");
    c.lock = VRT_LOCK_TURN;
    in.stick_x = 0.0f;
    vrt_update(&s, &c, &in, &o);
    in.stick_x = 1.0f;
    vrt_update(&s, &c, &in, &o);
    CHECK(!o.stick_to_game && o.sync_game && o.snapped == 1, "lock=turn: the stick still snaps");
    /* Auto-turn (smooth): the target 100 deg right of the view, threshold 70: turns toward it at
     * the smooth rate, stops within a quarter of the threshold, never past it. */
    c.lock = VRT_LOCK_VIEW;
    c.turn = VRT_TURN_SMOOTH;
    c.lock_auto_rad = 70 * VRT_DEG;
    vrt_reset(&s, 0.0f);
    in = input(0.011f, 0.0f, 0.0f);
    in.locked = 1;
    in.target_valid = 1;
    in.target_yaw = 100 * VRT_DEG;
    float off = 0.0f;
    int frames = 0;
    for (; frames < 400; ++frames) {
        vrt_update(&s, &c, &in, &o);
        off = dyaw(in.target_yaw, o.view_yaw);
        if (!s.auto_on) break;
        CHECK(off >= -1e-5f, "never past the target (%f)", deg(off));
    }
    CHECK(!s.auto_on && off < 17.5f * VRT_DEG && off > 0.0f, "auto-turn stops within 17.5 deg (%f after %d frames)", deg(off), frames);
    /* Within the threshold nothing happens. */
    vrt_reset(&s, 0.0f);
    in.target_yaw = 60 * VRT_DEG;
    for (int i = 0; i < 100; ++i) vrt_update(&s, &c, &in, &o);
    CHECK(s.psi == 0.0f, "target inside the threshold: no turn");
    /* Snap auto-turn: steps of 30 every 0.3 s. */
    c.turn = VRT_TURN_SNAP;
    vrt_reset(&s, 0.0f);
    in.target_yaw = -100 * VRT_DEG;
    vrt_update(&s, &c, &in, &o);
    CHECK(o.snapped == -1 && near_f(s.psi, -30 * VRT_DEG, 1e-5f), "snap auto-turn: -30 (%f)", deg(s.psi));
    printf("PASS lock-on\n");
}

static void test_config(void) {
    VrTurnConfig c;
    vrt_config_default(&c);
    CHECK(vrt_config_set(&c, "turn", "smooth") && c.turn == VRT_TURN_SMOOTH, "turn=smooth");
    CHECK(vrt_config_set(&c, "turn_speed", "120") && near_f(c.smooth_rad_s, 120 * VRT_DEG, 1e-6f), "turn_speed");
    CHECK(vrt_config_set(&c, "snap", "45") && near_f(c.snap_rad, 45 * VRT_DEG, 1e-6f), "snap");
    CHECK(vrt_config_set(&c, "move", "controller") && c.move == VRT_MOVE_CONTROLLER, "move");
    CHECK(vrt_config_set(&c, "lock", "game") && c.lock == VRT_LOCK_GAME, "lock");
    CHECK(vrt_config_set(&c, "lock_auto", "70") && near_f(c.lock_auto_rad, 70 * VRT_DEG, 1e-6f), "lock_auto");
    CHECK(vrt_config_set(&c, "turn_accel", "0") && c.accel_s == 0.0f, "turn_accel");
    CHECK(!vrt_config_set(&c, "turn", "sideways") && c.turn == VRT_TURN_SMOOTH, "bad value kept the old one");
    CHECK(!vrt_config_set(&c, "turn_speed", "9999") && near_f(c.smooth_rad_s, 120 * VRT_DEG, 1e-6f), "out of range");
    CHECK(!vrt_config_set(&c, "near", "0.2"), "not a turn key");
    CHECK(!strcmp(vrt_turn_name(c.turn), "smooth") && !strcmp(vrt_move_name(c.move), "controller") &&
          !strcmp(vrt_lock_name(c.lock), "game"), "names");
    printf("PASS config\n");
}

/* The same inputs give the same outputs, bit for bit (a game trace replays exactly). */
static void test_replay(void) {
    VrTurnConfig c;
    vrt_config_default(&c);
    c.turn = VRT_TURN_SMOOTH;
    c.move = VRT_MOVE_HEAD;
    float a[3][600];
    for (int run = 0; run < 2; ++run) {
        VrTurnState s;
        vrt_reset(&s, -2.0f);
        unsigned seed = 99;
        for (int i = 0; i < 600; ++i) {
            seed = seed * 1103515245u + 12345u;
            const float r = (float)((seed >> 8) & 0xffff) / 65535.0f;
            VrTurnInput in = input(0.008f + 0.01f * r, sinf((float)i * 0.05f), 1.2f * sinf((float)i * 0.013f));
            in.ref_yaw = i < 300 ? 0.0f : 0.5f;
            in.locked = i > 400 && i < 450;
            VrTurnOutput o;
            vrt_update(&s, &c, &in, &o);
            if (run == 0) { a[0][i] = o.psi; a[1][i] = o.view_yaw; a[2][i] = o.move_yaw; }
            else if (a[0][i] != o.psi || a[1][i] != o.view_yaw || a[2][i] != o.move_yaw) {
                CHECK(0, "replay differs at frame %d", i);
                break;
            }
        }
    }
    printf("PASS replay\n");
}

int main(void) {
    test_wrap();
    test_snap();
    test_smooth();
    test_head_and_move();
    test_recenter();
    test_lock();
    test_config();
    test_replay();
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("All VR turn tests passed (%d checks)\n", checks);
    return 0;
}
