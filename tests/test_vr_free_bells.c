/* vr-free-bells-test: src/vr_free_bells.h. */
#include "vr_free_bells.h"

#include <stdio.h>

static int checks, failures;
#define CHECK(x)                                        \
    do {                                                \
        ++checks;                                       \
        if (!(x)) {                                     \
            ++failures;                                 \
            printf("FAIL line %d: %s\n", __LINE__, #x); \
        }                                               \
    } while (0)

int main(void) {
    VfbState s = {0};
    CHECK(vfb_is_bell(0x400000C8u) && vfb_is_bell(0x400000CDu) && vfb_is_bell(0x400000E1u) && !vfb_is_bell(0x400003E8u));
    CHECK(vfb_step(&s, 0.0, 100, 0) == -1); /* idle */
    CHECK(vfb_step(&s, 1.0, 99, 0) == -1);  /* a drop with no armed press: left alone (a purchase) */
    vfb_arm(&s, 2.0);                       /* a press of ours, the pending id never changes */
    CHECK(vfb_step(&s, 2.5, 99, 0) == -1);
    CHECK(vfb_step(&s, 3.0, 98, 0) == 99 && s.refunds == 1); /* the bell's cost, back */
    CHECK(vfb_step(&s, 4.0, 99, 0) == -1);
    CHECK(vfb_step(&s, 5.0, 94, 0) == -1); /* a drop of 5 in the window: not the bell */
    CHECK(vfb_step(&s, 20.0, 93, 0) == -1); /* window over */
    vfb_arm(&s, 30.0);
    CHECK(vfb_step(&s, 39.0, 92, 0) == -1); /* 9 s after the press: over (window 8 s) */
    CHECK(vfb_step(&s, 40.0, 92, 0x400000CDu) == -1); /* the pending id is an extra signal */
    CHECK(vfb_step(&s, 41.0, 91, 0) == 92);
    /* floor: only while armed, or with floor_always */
    VfbState f = {0};
    CHECK(vfb_step(&f, 0.0, 0, 0) == -1 && f.floors == 0);
    vfb_arm(&f, 1.0);
    CHECK(vfb_step(&f, 1.1, 0, 0) == 1 && f.floors == 1);
    CHECK(vfb_step(&f, 1.2, 0, 0) == 1); /* the Beckoning Bell took the 1: refunded to 1 */
    CHECK(vfb_step(&f, 1.3, -1, 0) == -1 && !f.have_prev);
    VfbState r = {0};
    r.floor_always = 1;
    CHECK(vfb_step(&r, 0.0, 0, 0) == 1);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
