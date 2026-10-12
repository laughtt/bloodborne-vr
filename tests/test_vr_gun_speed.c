/* Unit tests for src/vr_gun_speed.h. Build: ninja -C out/gpu vr-gun-speed-test. */
#include "vr_gun_speed.h"
#include <stdio.h>
static int failures, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

int main(void) {
    const float dt = 0.0166f;
    /* ranges: the observed shots, the documented bounds, the neighbours */
    CHECK(vr_gun_is_shot(102051000) && vr_gun_is_shot(102051003), "observed a102 shots");
    CHECK(vr_gun_is_shot(51000) && vr_gun_is_shot(52150) && vr_gun_is_shot(52550) && vr_gun_is_shot(52999), "documented shots");
    CHECK(!vr_gun_is_shot(50999) && !vr_gun_is_shot(53000) && !vr_gun_is_shot(-1), "just outside");
    CHECK(!vr_gun_is_shot(112040000) && !vr_gun_is_shot(100033000) && !vr_gun_is_shot(3007200) && !vr_gun_is_shot(3000000),
          "attacks, dodge and walk are not shots");
    /* factor and guards */
    CHECK(vr_gun_factor(102051000, dt, 2.0f) == 2.0f, "scaled");
    CHECK(vr_gun_factor(102051000, 0.0f, 2.0f) == 1.0f, "stop at 0 left alone");
    CHECK(vr_gun_factor(102051000, 0.25f, 2.0f) == 1.0f, "hitch left alone");
    CHECK(vr_gun_factor(102051000, 0.2f, 2.0f) == 2.0f, "0.2 is still scaled");
    CHECK(vr_gun_factor(102051000, NAN, 2.0f) == 1.0f, "NaN dt");
    CHECK(vr_gun_factor(112040000, dt, 2.0f) == 1.0f, "melee untouched");
    CHECK(vr_gun_factor(102051000, dt, 0.0f) == 1.0f, "off");
    /* clamp */
    CHECK(vr_gun_clamp(1.0f) == 0.0f && vr_gun_clamp(0.5f) == 0.0f && vr_gun_clamp(-3.0f) == 0.0f, "1 and below = off");
    CHECK(vr_gun_clamp(NAN) == 0.0f && vr_gun_clamp(INFINITY) == 0.0f, "not finite = off");
    CHECK(vr_gun_clamp(2.0f) == 2.0f && vr_gun_clamp(1.01f) == 1.05f && vr_gun_clamp(99.0f) == VR_GUN_SPEED_MAX, "clamp range");
    /* parse */
    float x = 0;
    CHECK(vr_gun_parse("atk_speed=1.5\ngun_speed=2.5\n", &x) && x == 2.5f, "parse on a later line");
    CHECK(vr_gun_parse("gun_speed=1", &x) && x == 1.0f, "parse 1");
    CHECK(!vr_gun_parse("atk_speed=2\nxgun_speed=3\n", &x), "needs a token start");
    CHECK(!vr_gun_parse("", &x), "empty");
    CHECK(vr_gun_parse("a=1 gun_speed=3.0", &x) && x == 3.0f, "after a space");
    /* online */
    CHECK(!vr_gun_online(NULL, NULL) && !vr_gun_online("0", "") && vr_gun_online("1", NULL) && vr_gun_online(NULL, "host"), "online flag");
    CHECK(!vr_gun_online_off(NULL, NULL, NULL) && !vr_gun_online_off("0", "", "0"), "offline: on even with the opt-out set");
    CHECK(!vr_gun_online_off("1", NULL, NULL) && !vr_gun_online_off(NULL, "host", NULL), "online: stays on by default");
    CHECK(!vr_gun_online_off("1", "host", "1") && !vr_gun_online_off("1", NULL, ""), "only 0 opts out");
    CHECK(vr_gun_online_off("1", NULL, "0") && vr_gun_online_off(NULL, "host", "0"), "opt-out =0 turns it off online");
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("All %d VR gun speed checks passed\n", checks);
    return 0;
}
