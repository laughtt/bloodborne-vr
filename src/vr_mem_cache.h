/* Guest-memory readability guard for the VR hook handlers (they read pointer chains that dangle during
 * loads, so every read is checked first).
 *
 * The old guards asked VirtualQuery for the region and cached it for a second. The headset profile
 * (run12e) shows what that costs: VirtualQuery on the emulator's multi-GB guest mappings has to find the
 * extent of the region, and every cold lookup took milliseconds (Stamina cost 3-5 ms per call, Arms IK and
 * Attack speed hitches of 5-11 ms). The probe here touches only the pages asked about (IsBadReadPtr:
 * a few microseconds), and a small cache of probed page ranges answers the repeat reads. A second's
 * validity matches the old guard's. A write is checked as a read: the arrays the handlers write (pose
 * arrays, stamina) are the game's own live heap data, which the game writes every frame. */
#ifndef BB_VR_MEM_CACHE_H
#define BB_VR_MEM_CACHE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VM_SLOTS 128
#define VM_PAGE 4096u
#define VM_TTL_US 1000000u

typedef int (*VmProbe)(uintptr_t addr, size_t len); /* 1: [addr, addr+len) can be read */

typedef struct {
    uintptr_t lo[VM_SLOTS], hi[VM_SLOTS];
    int count, next;
    uint64_t stamp_us;
} VmCache;

/* 1 when [a, a+n) is readable: from the cache, else probed (whole pages) and remembered. */
static inline int vm_check(VmCache *c, uint64_t now_us, uintptr_t a, size_t n, VmProbe probe) {
    if (a < 0x10000 || a + n < a) return 0;
    if (!n) return 1;
    if (now_us - c->stamp_us > VM_TTL_US) { c->count = c->next = 0; c->stamp_us = now_us; }
    for (int i = 0; i < c->count; ++i)
        if (c->lo[i] <= a && a + n <= c->hi[i]) return 1;
    const uintptr_t lo = a & ~(uintptr_t)(VM_PAGE - 1), hi = (a + n + VM_PAGE - 1) & ~(uintptr_t)(VM_PAGE - 1);
    if (hi < lo || !probe(lo, hi - lo)) return 0;
    const int slot = c->count < VM_SLOTS ? c->count++ : (c->next++ % VM_SLOTS);
    c->lo[slot] = lo;
    c->hi[slot] = hi;
    return 1;
}

#ifdef _WIN32
#include <windows.h>
static int vm_probe_os(uintptr_t a, size_t n) { return !IsBadReadPtr((const void *)a, n); }
static inline uint64_t vm_now_us(void) {
    static uint64_t freq;
    LARGE_INTEGER c;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = (uint64_t)f.QuadPart; }
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart / freq * 1000000u + (uint64_t)c.QuadPart % freq * 1000000u / freq;
}
#else
#include <time.h>
static int vm_probe_os(uintptr_t a, size_t n) { return a >= 0x10000 && a + n >= a; }
static inline uint64_t vm_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
#endif
/* The OS-probed check with the caller's (thread-local) cache. */
static inline int vm_readable(VmCache *c, uintptr_t a, size_t n) { return vm_check(c, vm_now_us(), a, n, vm_probe_os); }

#endif
