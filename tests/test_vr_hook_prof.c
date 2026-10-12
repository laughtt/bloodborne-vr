/* Tests of the hook profiler's pure part (src/vr_hook_prof.h): the accumulator and the 10 s line.
 * Build: ninja -C out/gpu vr-hook-prof-test. Prints PASS lines, exits 0. */
#include <stdio.h>
#include <string.h>
#include "vr_hook_prof.h"

static int failures, checks;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static void test_note(void) {
    VhpSnap s = {"x", 0, 0, 0, 0};
    vhp_note(&s, 10, 1000);
    vhp_note(&s, 5000, 1000);
    vhp_note(&s, 20, 1000);
    CHECK(s.calls == 3 && s.total_ticks == 5030 && s.max_ticks == 5000 && s.over == 1);
    vhp_note(&s, 1000, 1000); /* exactly the limit is not over */
    CHECK(s.over == 1);
}

static void test_format(void) {
    char out[1024];
    VhpSnap s[4] = {{"A", 4, 2000, 1500, 1}, {"B", 100, 9000, 200, 0}, {"idle", 0, 0, 0, 0}, {NULL, 5, 5, 5, 5}};
    const int n = vhp_format(out, sizeof(out), s, 4, 1000.0, 10.0);
    CHECK(n == (int)strlen(out));
    CHECK(strncmp(out, "VR hooks: 10 s:", 15) == 0);
    CHECK(strstr(out, " B calls 100, total 9.00 ms, avg 90.0 us, max 0.20 ms, >1 ms 0") != NULL);
    CHECK(strstr(out, "; A calls 4, total 2.00 ms, avg 500.0 us, max 1.50 ms, >1 ms 1") != NULL);
    CHECK(strstr(out, "idle") == NULL); /* no calls: left out */
    CHECK(strstr(out, "all 11.00 ms (1.100 ms/s)") != NULL);
    CHECK(strstr(out, " B ") < strstr(out, " A ")); /* busiest first */
}

static void test_edge(void) {
    char out[1024];
    VhpSnap none[2] = {{"a", 0, 0, 0, 0}, {"b", 0, 0, 0, 0}};
    vhp_format(out, sizeof(out), none, 2, 1000.0, 10.0);
    CHECK(strstr(out, "no hook ran") != NULL);
    VhpSnap one[1] = {{"solo", 1, 500, 500, 0}};
    vhp_format(out, sizeof(out), one, 1, 0.0, 0.0); /* bad scale and span do not divide by zero */
    CHECK(strstr(out, "solo calls 1") != NULL && strstr(out, "all ") == NULL);
    char small[40];
    VhpSnap many[2] = {{"first hook with a long name", 3, 30, 20, 0}, {"second", 1, 1, 1, 0}};
    const int n = vhp_format(small, sizeof(small), many, 2, 1.0, 10.0);
    CHECK(n < (int)sizeof(small) && (int)strlen(small) == n); /* truncated, terminated */
    CHECK(vhp_format(small, 0, many, 2, 1.0, 10.0) == 0);
}

int main(void) {
    test_note();
    test_format();
    test_edge();
    if (failures) { printf("%d of %d checks FAILED\n", failures, checks); return 1; }
    printf("PASS: vr hook profiler, %d checks\n", checks);
    return 0;
}
