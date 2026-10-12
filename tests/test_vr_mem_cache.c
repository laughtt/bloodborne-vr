/* Tests of the guest-memory guard cache (src/vr_mem_cache.h) with a fake probe.
 * Build: ninja -C out/gpu vr-mem-cache-test. */
#include <stdio.h>
#include "vr_mem_cache.h"

static int failures, checks, probes;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static uintptr_t good_lo = 0x100000, good_hi = 0x200000;
static int fake_probe(uintptr_t a, size_t n) { ++probes; return a >= good_lo && a + n <= good_hi; }

int main(void) {
    VmCache c;
    memset(&c, 0, sizeof(c));
    CHECK(!vm_check(&c, 10, 0x100, 8, fake_probe) && probes == 0);      /* null-ish: no probe */
    CHECK(vm_check(&c, 10, 0x100010, 8, fake_probe) && probes == 1);
    CHECK(vm_check(&c, 20, 0x100ff0, 8, fake_probe) && probes == 1);    /* same page: cached */
    CHECK(vm_check(&c, 30, 0x100ff8, 0x10, fake_probe) && probes == 2); /* crosses into page 2: probed (whole pages) */
    CHECK(vm_check(&c, 40, 0x101000, 0x20, fake_probe) && probes == 2); /* now cached */
    CHECK(!vm_check(&c, 50, 0x300000, 8, fake_probe) && probes == 3);   /* bad: not cached */
    CHECK(!vm_check(&c, 51, 0x300000, 8, fake_probe) && probes == 4);
    CHECK(vm_check(&c, 60, 0x100010, 8, fake_probe) && probes == 4);
    CHECK(vm_check(&c, 60 + VM_TTL_US + 1, 0x100010, 8, fake_probe) && probes == 5); /* expired: probed again */
    CHECK(!vm_check(&c, 70 + VM_TTL_US, ~(uintptr_t)0 - 3, 8, fake_probe));          /* wraps */
    CHECK(vm_check(&c, 80 + VM_TTL_US, 0x100000, 0, fake_probe));                    /* empty read */
    /* a full cache replaces its oldest slot and still answers */
    memset(&c, 0, sizeof(c));
    good_hi = 0x20000000;
    for (int i = 0; i < 200; ++i) CHECK(vm_check(&c, 100, 0x100000 + (uintptr_t)i * 0x10000, 8, fake_probe));
    CHECK(c.count == VM_SLOTS);
    probes = 0;
    CHECK(vm_check(&c, 101, 0x100000 + 199u * 0x10000, 8, fake_probe) && probes == 0);
    /* the OS probe: a live stack variable is readable, address 0x1000 is not */
    int local = 5;
    memset(&c, 0, sizeof(c));
    CHECK(vm_readable(&c, (uintptr_t)&local, sizeof(local)));
    CHECK(!vm_readable(&c, 0x1000, 4));
    if (failures) { printf("%d of %d FAILED\n", failures, checks); return 1; }
    printf("PASS: vr mem cache, %d checks\n", checks);
    return 0;
}
