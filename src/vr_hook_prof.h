/* Pure part of the VR hook profiler (src/runtime_hooks.c, BB_VR_HOOK_PROFILE): the per-hook totals
 * and the 10 s summary line. Header only, no platform calls, tested by tests/test_vr_hook_prof.c.
 *
 * Why: the headset reports frame drops in combat, with the gun and in visceral attacks; the hooks run
 * on the game thread (a slow hook is a slow game step), but the Pace line cannot say which one. Every
 * hook handler is timed (a clock read before and after, two atomic adds); the line names the culprit:
 *   VR hooks: 10 s: Attack Speed calls 612, total 0.84 ms, avg 1.4 us, max 0.05 ms, >1 ms 0; ...
 * "total" is the time inside the handler over the 10 s (ms), "max" the longest single call, ">1 ms"
 * how many calls took longer than a millisecond (a hitch is one such call). Hooks that did not run
 * in the window are left out. */
#ifndef BB_VR_HOOK_PROF_H
#define BB_VR_HOOK_PROF_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VHP_SLOTS 48
#define VHP_OVER_MS 1.0 /* a call longer than this counts in ">1 ms" */

typedef struct {
    const char *name; /* NULL: unused slot */
    uint64_t calls, total_ticks, max_ticks, over;
} VhpSnap;

/* One call of `ticks` into a snapshot-style accumulator (the runtime keeps atomics and builds the
 * same numbers; this is the reference the tests pin). */
static inline void vhp_note(VhpSnap *s, uint64_t ticks, uint64_t over_ticks) {
    ++s->calls;
    s->total_ticks += ticks;
    if (ticks > s->max_ticks) s->max_ticks = ticks;
    if (ticks > over_ticks) ++s->over;
}

/* The summary line (no newline) for `n` slots; ticks_per_ms converts. Returns the length written
 * (always NUL-terminated); slots without calls are skipped; the slots are listed busiest first
 * (by total time). When no slot ran, the line says so. */
static inline int vhp_format(char *out, size_t cap, const VhpSnap *s, int n, double ticks_per_ms, double seconds) {
    if (!cap) return 0;
    if (!(ticks_per_ms > 0.0)) ticks_per_ms = 1.0;
    int order[VHP_SLOTS], m = 0;
    for (int i = 0; i < n && i < VHP_SLOTS; ++i)
        if (s[i].name && s[i].calls) order[m++] = i;
    for (int i = 1; i < m; ++i) { /* insertion sort, busiest first */
        const int k = order[i];
        int j = i - 1;
        while (j >= 0 && s[order[j]].total_ticks < s[k].total_ticks) { order[j + 1] = order[j]; --j; }
        order[j + 1] = k;
    }
    int len = snprintf(out, cap, "VR hooks: %.0f s:", seconds);
    if (len < 0) { out[0] = 0; return 0; }
    if (!m && (size_t)len < cap) len += snprintf(out + len, cap - (size_t)len, " no hook ran");
    double sum = 0.0;
    for (int k = 0; k < m && (size_t)len < cap; ++k) {
        const VhpSnap *h = &s[order[k]];
        const double total = (double)h->total_ticks / ticks_per_ms;
        sum += total;
        len += snprintf(out + len, cap - (size_t)len, "%s %s calls %llu, total %.2f ms, avg %.1f us, max %.2f ms, >%.0f ms %llu",
                        k ? ";" : "", h->name, (unsigned long long)h->calls, total,
                        total * 1000.0 / (double)h->calls, (double)h->max_ticks / ticks_per_ms, VHP_OVER_MS,
                        (unsigned long long)h->over);
    }
    if (m > 1 && (size_t)len < cap) len += snprintf(out + len, cap - (size_t)len, "; all %.2f ms (%.3f ms/s)", sum, sum / (seconds > 0 ? seconds : 1.0));
    if ((size_t)len >= cap) { len = (int)cap - 1; out[len] = 0; }
    return len;
}

#endif
