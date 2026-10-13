/* Unit tests for src/vr_hurt_speed.h. Build: ninja -C out/gpu vr-hurt-speed-test. */
#include "vr_hurt_speed.h"
#include <stdio.h>
static int failures, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

int main(void) {
    const float dt = 0.0166f;
    const VrHurtRange r = vr_hurt_default_range();
    CHECK(vr_hurt_is_reaction(3020100, r) && vr_hurt_is_reaction(3020133, r) && vr_hurt_is_reaction(3020900, r) &&
              vr_hurt_is_reaction(20999, r),
          "default range");
    CHECK(!vr_hurt_is_reaction(19999, r) && !vr_hurt_is_reaction(21000, r) && !vr_hurt_is_reaction(3011100, r) && !vr_hurt_is_reaction(-1, r), "just outside");
    CHECK(!vr_hurt_is_reaction(3005000, r) && !vr_hurt_is_reaction(3007200, r) && !vr_hurt_is_reaction(3019000, r), "idle, dodge, style change");
    CHECK(!vr_hurt_is_reaction(112040000, r) && !vr_hurt_is_reaction(102051000, r), "attack and gun shot");
    CHECK(!vr_hurt_is_reaction(100033000, r) && !vr_hurt_is_reaction(3101200, r) && !vr_hurt_is_reaction(3104999, r),
          "visceral / grab / gesture ids (100000+) are excluded");
    CHECK(vr_hurt_factor(3020133, dt, 2.0f, r) == 2.0f, "scaled");
    CHECK(vr_hurt_factor(3020133, 0.0f, 2.0f, r) == 1.0f, "hit stop (dt 0) untouched");
    CHECK(vr_hurt_factor(3020133, 0.25f, 2.0f, r) == 1.0f, "hitch untouched");
    CHECK(vr_hurt_factor(3020133, NAN, 2.0f, r) == 1.0f, "NaN");
    CHECK(vr_hurt_factor(3020133, dt, 0.0f, r) == 1.0f, "off");
    CHECK(vr_hurt_factor(3000000, dt, 2.0f, r) == 1.0f, "walking untouched");
    /* discovery log classes */
    CHECK(vr_hurt_is_unclassified(3013000, r) && vr_hurt_is_unclassified(3101200, r) && vr_hurt_is_unclassified(3019000, r), "unclassified");
    CHECK(!vr_hurt_is_unclassified(3020133, r) && !vr_hurt_is_unclassified(3040000, r) && !vr_hurt_is_unclassified(102051000, r) &&
              !vr_hurt_is_unclassified(3005000, r),
          "classified or quiet ids");
    /* range override */
    VrHurtRange o = r;
    CHECK(vr_hurt_parse_range("10000-11999", &o) && o.from == 10000 && o.to == 12000, "range parse");
    CHECK(!vr_hurt_parse_range("12000-10000", &o) && !vr_hurt_parse_range("abc", &o) && !vr_hurt_parse_range("", &o) &&
              !vr_hurt_parse_range(NULL, &o) && !vr_hurt_parse_range("0-5", &o) && o.from == 10000,
          "bad ranges leave it alone");
    CHECK(vr_hurt_factor(3010500, dt, 2.0f, o) == 2.0f && vr_hurt_factor(3009500, dt, 2.0f, o) == 1.0f, "override applies");
    /* clamp, parse, online */
    CHECK(vr_hurt_clamp(1.0f) == 0.0f && vr_hurt_clamp(NAN) == 0.0f && vr_hurt_clamp(-1.0f) == 0.0f, "off values");
    CHECK(vr_hurt_clamp(2.0f) == 2.0f && vr_hurt_clamp(1.01f) == 1.05f && vr_hurt_clamp(99.0f) == VR_HURT_SPEED_MAX, "clamp");
    CHECK(VR_HURT_SPEED_DEFAULT == 2.0f && vr_hurt_clamp(VR_HURT_SPEED_DEFAULT) == 2.0f, "default is on (2.0)");
    CHECK(vr_hurt_factor(3020133, dt, vr_hurt_clamp(VR_HURT_SPEED_DEFAULT), r) == 2.0f, "default speeds reactions up 2x");
    float x = 0;
    CHECK(vr_hurt_parse("gun_speed=2\nhurt_speed=3\n", &x) && x == 3.0f, "parse later line");
    CHECK(!vr_hurt_parse("xhurt_speed=3", &x) && !vr_hurt_parse("", &x), "token start / empty");
    CHECK(!vr_hurt_online(NULL, NULL) && !vr_hurt_online("0", "") && vr_hurt_online("1", NULL) && vr_hurt_online(NULL, "host"), "online flag");
    CHECK(!vr_hurt_online_off(NULL, NULL, "0") && !vr_hurt_online_off("1", NULL, NULL) && !vr_hurt_online_off(NULL, "host", NULL),
          "online: stays on by default; offline ignores the opt-out");
    CHECK(!vr_hurt_online_off("1", "host", "1") && vr_hurt_online_off("1", NULL, "0") && vr_hurt_online_off(NULL, "host", "0"), "opt-out =0");
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("All %d VR hurt speed checks passed\n", checks);
    return 0;
}
