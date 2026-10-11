/* Unit tests for src/vr_throw_anim.h. Build: ninja -C out/gpu vr-throw-anim-test. */
#include "vr_throw_anim.h"
#include <stdio.h>
static int failures, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

int main(void) {
    VtaState s = {0};
    CHECK(vta_step(&s, 0, 5000, 0.5f, 1.8f) == 1.0f, "idle passes through");
    vta_begin(&s, 10.0);
    float t = 0.9f;
    CHECK(vta_step(&s, 10.0, 7000, t, 1.8f) == 1.0f, "first frame only records");
    CHECK(vta_step(&s, 10.02, 7000, t + 0.02f, 1.8f) == 1.0f, "same clip is not a new one");
    CHECK(vta_step(&s, 10.04, 12345, 0.0f, 1.8f) == 1.8f, "new clip latches");
    float c = 0.0f;
    int n = 0;
    for (int i = 0; i < 20; ++i) { c += 0.03f; n += vta_step(&s, 10.06 + i * 0.016, 12345, c, 1.8f) == 1.8f; }
    CHECK(n == 20, "scaled the whole clip (%d)", n);
    int ended = 0;
    for (int i = 0; i < 5 && !ended; ++i) ended = vta_step(&s, 11.0 + i * 0.016, 12345, c, 1.8f) == 1.0f;
    CHECK(ended && s.state == 0, "a stalled clip ends the scaling");

    /* a weapon move or a dodge never latches */
    vta_begin(&s, 20.0);
    vta_step(&s, 20.0, 5000, 0.5f, 1.8f);
    CHECK(vta_step(&s, 20.1, 7200, 0.0f, 1.8f) == 1.0f && s.state == 1, "dodge ignored");
    CHECK(vta_step(&s, 20.2, 30100, 0.0f, 1.8f) == 1.0f && s.state == 1, "attack ignored");
    CHECK(vta_step(&s, 20.3, 5111, 0.0f, 1.8f) == 1.8f, "then the throw clip latches");
    /* a different clip after it ends the scaling */
    CHECK(vta_step(&s, 20.4, 5222, 0.0f, 1.8f) == 1.0f && s.state == 0, "next clip ends");
    /* the same id restarting (second throw) is a new clip */
    vta_begin(&s, 30.0);
    vta_step(&s, 30.0, 5111, 1.2f, 1.8f);
    CHECK(vta_step(&s, 30.1, 5111, 0.0f, 1.8f) == 1.8f, "restart of the same id latches");
    /* the wait window expires */
    vta_begin(&s, 40.0);
    vta_step(&s, 40.0, 5000, 0.5f, 1.8f);
    CHECK(vta_step(&s, 45.0, 5111, 0.0f, 1.8f) == 1.0f && s.state == 0, "window expired");
    /* hard cap */
    vta_begin(&s, 50.0);
    vta_step(&s, 50.0, 5000, 0.5f, 1.8f);
    vta_step(&s, 50.1, 5111, 0.0f, 1.8f);
    CHECK(vta_step(&s, 57.0, 5111, 0.5f, 1.8f) == 1.0f && s.state == 0, "max duration");
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("All %d VR throw anim checks passed\n", checks);
    return 0;
}
