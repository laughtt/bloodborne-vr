/* Tests of the comfort dash in C: the filter the sandbox shares with the game
 * (gpu/xr_bridge/vr_dash.h, built here as C11 the way bb-probe builds it) and the game's move
 * detection (src/vr_dash_detect.h: the clip table, directions, the TimeAct ring, the input
 * fallback, the settings tokens). docs/vr/findings/19-dash-locomotion.md.
 * Build: ninja -C out/gpu vr-dash-test (no GPU, game or headset). Prints PASS lines, exits 0. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vr_dash_detect.h"
#include "vr_fp_math.h"

static int failures, checks;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* A roll as the first-person eye does it: 4 m forward with a quick start, the eye down to the
 * floor and surging ahead, sampled at the game's 30 fps; the filter at 72 Hz. */
static VrDashV3 roll_eye(double t) {
    const double tick = floor(t * 30.0 + 1e-6) / 30.0;
    float x = (float)(tick / 0.9);
    if (x > 1.0f) x = 1.0f;
    const float root = 4.0f * (1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x));
    const float s = sinf(3.14159265f * x);
    return vr_dash_v3(0.05f * s, 1.6f - 1.4f * s * s, root + 0.3f * s);
}

static void test_filter_in_c(void) {
    VrDashParams p;
    vr_dash_params_default(&p);
    p.duration = 0.4f;
    p.power = 2.0f;
    VrDash d;
    vr_dash_reset(&d);
    VrDashHint h;
    memset(&h, 0, sizeof(h));
    h.kind = VR_DASH_DODGE;
    h.direction = vr_dash_v3(0.0f, 0.0f, 1.0f);
    h.distance = 4.0f;
    h.height = -1.0f;
    float max_z = -1.0f, max_dy = 0.0f, max_x = 0.0f, max_step = 0.0f;
    VrDashV3 prev = vr_dash_v3(0.0f, 1.6f, 0.0f), last = prev, anchor = prev;
    for (int i = 0; i < 160; ++i) {
        const double t = i / 72.0;
        anchor = roll_eye(t);
        const VrDashV3 o = vr_dash_update(&d, &p, anchor, t < 1.2 ? 1 : 0, &h, t);
        if (o.z > max_z) max_z = o.z;
        if (d.mode == VR_DASH_MOVE && fabsf(o.y - 1.6f) > max_dy) max_dy = fabsf(o.y - 1.6f);
        if (fabsf(o.x) > max_x && d.mode == VR_DASH_MOVE) max_x = fabsf(o.x);
        const float step = vr_dash_len(vr_dash_sub(o, prev));
        if (step > max_step) max_step = step;
        prev = last = o;
    }
    CHECK(max_z <= 4.0f + 1e-3f);                       /* no overshoot (the eye surges 0.3 m past) */
    CHECK(max_dy < 1e-4f);                              /* level: the eye dips 1.4 m */
    CHECK(max_x < 1e-4f);                               /* straight: the eye sways 5 cm */
    CHECK(vr_dash_len(vr_dash_sub(last, anchor)) == 0.0f); /* on the eye again, no drift */
    CHECK(d.mode == VR_DASH_IDLE);
    CHECK(max_step < 0.3f);                             /* 20 m/s at the start: 0.28 m at 72 Hz */
    printf("PASS filter in C: end %.4f m (no overshoot), level within %.5f m, straight within %.5f m, max step %.3f m\n",
           max_z, max_dy, max_x, max_step);
}

static void test_table(void) {
    const VrDashMove *roll = vr_dash_move_lookup(7200);
    CHECK(roll && roll->kind == VR_DASH_MOVE_ROLL && fabsf(roll->distance - 4.0f) < 1e-6f);
    CHECK(vr_dash_move_lookup(2007200) == roll);        /* a002_007200: the same sub-ID */
    CHECK(vr_dash_move_lookup(9007800) && vr_dash_move_lookup(9007800)->kind == VR_DASH_MOVE_JUMP);
    CHECK(vr_dash_move_lookup(10007200) == NULL);       /* a010: not the a00x groups */
    CHECK(vr_dash_move_lookup(7201) == NULL);
    CHECK(vr_dash_move_lookup(8000) == NULL);           /* a drop off a ledge: no dash */
    CHECK(vr_dash_move_lookup(-1) == NULL);
    CHECK(vr_dash_move_lookup(7000)->kind == VR_DASH_MOVE_BACKSTEP);
    CHECK(fabsf(vr_dash_move_lookup(7103)->direction - 90.0f) < 1e-6f); /* quickstep right */
    int sane = 1;
    for (size_t i = 0; i < sizeof(vr_dash_moves) / sizeof(*vr_dash_moves); ++i) {
        const VrDashMove *m = &vr_dash_moves[i];
        sane &= m->distance > 1.0f && m->distance < 8.0f && fabsf(m->direction) <= 180.0f &&
                m->motion > 0.0f && m->motion <= m->clip + 1e-6f && m->kind != VR_DASH_MOVE_NONE;
        for (size_t j = 0; j < i; ++j) sane &= vr_dash_moves[j].sub != m->sub;
    }
    CHECK(sane);
    printf("PASS table: %zu clips, a00x groups, roll 4.00 m, backstep, jump; drops excluded\n",
           sizeof(vr_dash_moves) / sizeof(*vr_dash_moves));
}

static void test_direction(void) {
    /* Headings grow to the right: facing +Z (0), a move to the right goes +X. */
    VrDashV3 d = vr_dash_move_direction(0.0f, 0.0f);
    CHECK(fabsf(d.x) < 1e-6f && fabsf(d.z - 1.0f) < 1e-6f);
    d = vr_dash_move_direction(0.0f, 90.0f);
    CHECK(fabsf(d.x - 1.0f) < 1e-6f && fabsf(d.z) < 1e-5f);
    d = vr_dash_move_direction(0.0f, 180.0f);                  /* backstep */
    CHECK(fabsf(d.z + 1.0f) < 1e-6f);
    d = vr_dash_move_direction(1.5707963f, -90.0f);             /* facing +X, stepping left: +Z */
    CHECK(fabsf(d.x) < 1e-5f && fabsf(d.z - 1.0f) < 1e-5f);
    printf("PASS direction: right = facing + 90 deg, back = + 180 deg\n");
}

static void test_ring_repush(void) {
    /* The game re-pushes the playing clip every frame (its time floats advance): one roll = one
     * move; the clip time jumping back = the same roll chained (here at 0.6 s). */
    const int32_t roll = 7200;
    const VrDashMove *m = vr_dash_move_lookup(roll);
    CHECK(m != NULL);
    int have = 0, moves = 0, varied = 0;
    int32_t prev = 0;
    float pf[3] = {0, 0, 0};
    double t0 = 0.0;
    for (int f = 0; f < 200; ++f) { /* 100 fps; the second roll starts at 0.6 s, clip time back to 0 */
        const double now = f * 0.01;
        const double ct = now < 0.6 ? now : now - 0.6;
        const float cur[3] = {(float)ct, 0.0f, 1.67f};
        if (f && vr_dash_ring_new_move(have, prev, roll, pf, cur, varied, now - t0, m)) {
            ++moves; have = 1; t0 = now; varied = 0;
        } else if (f && cur[0] != pf[0]) varied = 1;
        if (f == 1 && !moves) { moves = 1; have = 1; t0 = now; varied = 0; } /* the first sight */
        prev = roll;
        memcpy(pf, cur, sizeof(pf));
    }
    CHECK(moves == 2);
    /* the time never moves (unreadable): the clip length is the fallback, and a plain re-push is no move */
    const float still[3] = {0.0f, 0.0f, 1.67f};
    CHECK(!vr_dash_ring_new_move(1, roll, roll, still, still, 0, 0.6, m));
    CHECK(vr_dash_ring_new_move(1, roll, roll, still, still, 0, 1.7, m));
    CHECK(!vr_dash_ring_new_move(1, roll, roll, still, still, 1, 1.7, m));
    /* a different clip is a move at once; re-pushes with no move in progress are not */
    CHECK(vr_dash_ring_new_move(1, 7200, 7100, still, still, 1, 0.1, m));
    CHECK(!vr_dash_ring_new_move(0, 7200, 7200, still, still, 0, 5.0, m));
    printf("PASS ring repush: one clip = one move, the clip time jumping back = the next\n");
}

static void test_ring(void) {
    unsigned char m[0xC8];
    memset(m, 0, sizeof(m));
    for (int i = 0; i < 10; ++i) {
        const int32_t anim = 1000 + i;
        memcpy(m + 0x20 + 16 * i, &anim, 4);
    }
    int32_t head = 3, anim = 0, h = 0;
    memcpy(m + 0xC0, &head, 4);
    CHECK(vr_dash_ring_newest(m, &anim, &h, NULL) && anim == 1002 && h == 3); /* (3 + 9) % 10 = 2 */
    head = 0;
    memcpy(m + 0xC0, &head, 4);
    CHECK(vr_dash_ring_newest(m, &anim, &h, NULL) && anim == 1009);
    head = 10;
    memcpy(m + 0xC0, &head, 4);
    CHECK(!vr_dash_ring_newest(m, &anim, &h, NULL));
    printf("PASS ring: the newest entry is (head + 9) %% 10, a bad index is refused\n");
}

static void test_stall(void) {
    const VrDashMove *roll = vr_dash_move_lookup(7200); /* 4 m, root motion until 0.93 s */
    VrDashStall s;
    vr_dash_stall_reset(&s);
    /* The roll's own deceleration near its end: not a stall (already within 0.4 m). */
    int fired = 0;
    for (double t = 0.0; t < 1.0; t += 1.0 / 30.0) {
        const float x = (float)(t / 0.93 < 1.0 ? t / 0.93 : 1.0);
        const float progress = 4.0f * (1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x));
        const float speed = 4.0f * 3.0f * (1.0f - x) * (1.0f - x) / 0.93f;
        fired |= vr_dash_stalled(&s, roll, t, progress, speed);
    }
    CHECK(!fired);
    /* Into a wall after 1.1 m at 0.25 s: fires 0.12 s later. */
    vr_dash_stall_reset(&s);
    double when = -1.0;
    for (double t = 0.0; t < 0.9 && when < 0.0; t += 1.0 / 30.0) {
        const float progress = t < 0.25 ? (float)(4.4 * t) : 1.1f;
        const float speed = t < 0.25 ? 4.4f : 0.0f;
        if (vr_dash_stalled(&s, roll, t, progress, speed)) when = t;
    }
    CHECK(when > 0.36 && when < 0.42);
    /* A slow start (the quickstep's first frames) is not a stall: under 0.2 s. */
    vr_dash_stall_reset(&s);
    CHECK(!vr_dash_stalled(&s, roll, 0.05, 0.0f, 0.0f) && !vr_dash_stalled(&s, roll, 0.15, 0.05f, 0.2f));
    CHECK(!vr_dash_stalled(&s, NULL, 0.5, 0.0f, 0.0f));
    printf("PASS stall: a roll into a wall ends after %.2f s, a full roll never\n", when);
}

static void test_input(void) {
    VrDashInput s;
    vr_dash_input_reset(&s);
    const uint32_t C = VR_DASH_BTN_CIRCLE, L3 = VR_DASH_BTN_L3;
    CHECK(vr_dash_input_update(&s, 0.00, C, 1, 0.3) == 0);
    CHECK(vr_dash_input_update(&s, 0.10, 0, 1, 0.3) == VR_DASH_MOVE_ROLL);      /* tapped, moving */
    CHECK(vr_dash_input_update(&s, 1.00, C, 0, 0.3) == 0);
    CHECK(vr_dash_input_update(&s, 1.15, 0, 0, 0.3) == VR_DASH_MOVE_BACKSTEP);  /* tapped, still */
    CHECK(vr_dash_input_update(&s, 2.00, C, 1, 0.3) == 0);
    CHECK(vr_dash_input_update(&s, 2.80, C, 1, 0.3) == 0);                      /* held: sprint */
    CHECK(vr_dash_input_update(&s, 2.90, C | L3, 1, 0.3) == VR_DASH_MOVE_JUMP); /* L3 sprinting */
    CHECK(vr_dash_input_update(&s, 3.20, 0, 1, 0.3) == 0);                      /* long: no roll */
    CHECK(vr_dash_input_update(&s, 4.00, L3, 1, 0.3) == 0);                     /* L3 alone */
    printf("PASS input: circle tap = roll or backstep, held = sprint, L3 while sprinting = jump\n");
}

static void test_settings(void) {
    VrDashSettings s;
    vr_dash_settings_default(&s);
    CHECK(s.on && fabsf(s.params.duration - 0.4f) < 1e-6f && fabsf(s.params.power - 2.0f) < 1e-6f);
    CHECK(fabsf(s.params.max_drop - 2.0f) < 1e-6f);
    const int n = vr_dash_settings_parse("duration=0.35 ease=cubic lead=1 jump=0 detect=input dash=0 bogus=1 power=99", &s);
    CHECK(n == 6);
    CHECK(fabsf(s.params.duration - 0.35f) < 1e-6f && fabsf(s.params.power - 3.0f) < 1e-6f);
    CHECK(fabsf(s.params.max_lead - 1.0f) < 1e-6f && s.params.jump_height == 0.0f);
    CHECK(s.detect == VR_DASH_DETECT_INPUT && !s.on);
    CHECK(vr_dash_settings_parse("ease=expo jump_s=0.7", &s) == 2 && s.params.curve == VR_DASH_EASE_EXPO &&
          fabsf(s.params.jump_duration - 0.7f) < 1e-6f && fabsf(s.jump_duration - 0.7f) < 1e-6f);
    printf("PASS settings: tokens applied, unknown and out-of-range ones skipped\n");
}

static void test_follow(void) {
    VrDashParams p;
    vr_dash_params_default(&p);
    p.follow = 1;
    CHECK(p.max_lead == 0.0f);
    VrDash d;
    vr_dash_reset(&d);
    float ahead = -1e9f, below = 0.0f, past_wall = 0.0f;
    VrDashV3 a = vr_dash_v3(0.0f, 1.6f, 0.0f), out = a;
    /* a wall after 1 m, on a rising slope (0.5 m per m), 4 m planned */
    for (int i = 0; i < 90; ++i) {
        const double t = i / 30.0;
        const float x = fminf((float)t / 0.25f, 1.0f), run = 1.0f - (1.0f - x) * (1.0f - x);
        a = vr_dash_v3(0.0f, 1.6f + 0.5f * run, run);
        VrDashHint h;
        memset(&h, 0, sizeof(h));
        h.direction = vr_dash_v3(0.0f, 0.0f, 1.0f);
        h.distance = 4.0f;
        h.height = -1.0f;
        out = vr_dash_update(&d, &p, a, t < 1.0 ? 1 : 0, i == 0 ? &h : NULL, t);
        if (out.z - a.z > ahead) ahead = out.z - a.z;
        if (a.y - out.y > below) below = a.y - out.y;
        if (out.z - 1.0f > past_wall) past_wall = out.z - 1.0f;
    }
    CHECK(ahead <= 1e-4f && below < 1e-4f && past_wall < 1e-3f);
    CHECK(fabsf(out.z - a.z) < 1e-3f && fabsf(out.y - a.y) < 1e-3f);
    printf("PASS follow: blocked at a wall and up a slope, the camera never leads and keeps the height\n");
}

/* face_dodge=strafe: the step keeps its length whatever direction the game moved it (a back roll), aims at travel. */
static void test_strafe_step(void) {
    float ox, oz;
    vr_strafe_step(0.0f, -0.5f, 0.0f, &ox, &oz); /* game stepped back, travel forward */
    CHECK(fabsf(ox) < 1e-5f && fabsf(oz - 0.5f) < 1e-5f);
    vr_strafe_step(0.3f, 0.4f, -1.5707963f, &ox, &oz); /* travel left */
    CHECK(fabsf(ox + 0.5f) < 1e-5f && fabsf(oz) < 1e-4f);
    vr_strafe_step(0.0f, 0.0f, 1.0f, &ox, &oz);
    CHECK(ox == 0.0f && oz == 0.0f);
}

int main(void) {
    test_filter_in_c();
    test_table();
    test_direction();
    test_ring();
    test_ring_repush();
    test_stall();
    test_input();
    test_settings();
    test_follow();
    test_strafe_step();
    if (failures) {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("All VR dash tests passed (%d checks)\n", checks);
    return 0;
}
