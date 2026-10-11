/* Equivalence test for the red zone guard's relocated copies (src/runtime_redzone_guard.c).
 *
 * Maps the eboot text (out/eboot.elf, file offset 0x4000 = vaddr 0) into RWX memory, builds each
 * relocated blob with the production code (redzone_guard_build), then calls the ORIGINAL function
 * and the relocated copy as SysV functions on identical synthetic inputs and compares every
 * buffer byte for byte (output buffer, canaries around it, and the float arrays the functions
 * were handed). 0x28ceec0 has a known signature and must be exercised and match; the sibling
 * builders get a generic call and are reported "not exercised" if the generic call faults.
 * Skips (exit 0) when out/eboot.elf is absent. Windows only. */
#ifndef _WIN32
#include <stdio.h>
int main(void) { printf("SKIP: red zone guard test is Windows only\n"); return 0; }
#else
#include "../src/runtime_redzone_guard.c"

#define SYSV __attribute__((sysv_abi))
#define IMAGE_SIZE 0x5100000u
#define TEXT_SIZE 0x50d96dcu
#define NARR 20
#define ARR_FLOATS 4096
#define OUT_BYTES 65536
#define CANARY 256
#define CASES 2000

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return rng_state; }
static float rndf(void) { return (float)((double)(rnd() >> 11) / 9007199254740992.0 * 4.0 - 2.0); }
static unsigned rndu(unsigned lo, unsigned hi) { return lo + (unsigned)(rnd() % (hi - lo + 1)); }

typedef void (SYSV *KnownFn)(void *out, uint32_t n, float *a, float *b, float *c, float *d,
                             float x0, float x1, float x2, float x3, float x4, float x5, float x6, float x7,
                             void *s0, void *s1, void *s2, void *s3, void *s4, int32_t i5, float f6, float f7,
                             int32_t i8, int32_t i9, float f10, float f11);
typedef void (SYSV *GenericFn)(void *out, uint64_t n, void *a, void *b, void *c, void *d,
                               float x0, float x1, float x2, float x3, float x4, float x5, float x6, float x7,
                               uint64_t s0, uint64_t s1, uint64_t s2, uint64_t s3, uint64_t s4, uint64_t s5,
                               uint64_t s6, uint64_t s7, uint64_t s8, uint64_t s9, uint64_t s10, uint64_t s11);

/* A set of working buffers: out region with canaries, then NARR float arrays. */
typedef struct { unsigned char out[CANARY + OUT_BYTES + CANARY]; float arr[NARR][ARR_FLOATS]; } Buffers;

static Buffers base_buf, buf_a, buf_b;

static void fill_base(void) {
    for (size_t i = 0; i < sizeof base_buf.out; ++i) base_buf.out[i] = (unsigned char)rnd();
    for (int k = 0; k < NARR; ++k)
        for (int i = 0; i < ARR_FLOATS; ++i) base_buf.arr[k][i] = rndf();
}

typedef struct {
    int known;               /* 1: KnownFn, 0: GenericFn */
    uint32_t n;
    float x[8], f6, f7, f10, f11;
    int32_t i5, i8, i9;
    int s1_null;
    unsigned mask;           /* generic: bit k set = stack slot k is a pointer */
    uint64_t ints[12];
} Params;

static void make_params(Params *p, int known, unsigned mask) {
    memset(p, 0, sizeof *p);
    p->known = known; p->mask = mask;
    p->n = known ? rndu(1, 64) : rndu(1, 32);
    for (int i = 0; i < 8; ++i) p->x[i] = rndf();
    p->f6 = rndf(); p->f7 = rndf(); p->f10 = rndf(); p->f11 = rndf();
    p->i5 = (int32_t)rndu(1, 8); p->i8 = (int32_t)rndu(1, 8); p->i9 = (int32_t)rndu(1, 8);
    p->s1_null = known && (rnd() % 4 == 0);
    for (int k = 0; k < 12; ++k) p->ints[k] = rndu(1, 8);
}

static void *jb[5];
static LONG CALLBACK veh(PEXCEPTION_POINTERS info) {
    DWORD c = info->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        c == EXCEPTION_PRIV_INSTRUCTION || c == EXCEPTION_IN_PAGE_ERROR)
        __builtin_longjmp(jb, 1);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void *g_fn; static const Params *g_p; static Buffers *g_b;

static void invoke(void) {
    const Params *p = g_p; Buffers *b = g_b;
    float *a[NARR];
    for (int k = 0; k < NARR; ++k) a[k] = b->arr[k] + 256;
    void *out = b->out + CANARY;
    if (p->known) {
        ((KnownFn)g_fn)(out, p->n, a[0], a[1], a[2], a[3], p->x[0], p->x[1], p->x[2], p->x[3], p->x[4], p->x[5], p->x[6], p->x[7],
                        a[4], p->s1_null ? NULL : a[5], a[6], a[7], a[8], p->i5, p->f6, p->f7, p->i8, p->i9, p->f10, p->f11);
    } else {
        uint64_t s[12];
        for (int k = 0; k < 12; ++k) s[k] = (p->mask >> k & 1) ? (uint64_t)(uintptr_t)a[4 + k] : p->ints[k];
        ((GenericFn)g_fn)(out, p->n, a[0], a[1], a[2], a[3], p->x[0], p->x[1], p->x[2], p->x[3], p->x[4], p->x[5], p->x[6], p->x[7],
                          s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11]);
    }
}

/* Returns 1 if the call faulted. */
static __attribute__((noinline)) int guarded(void *fn, const Params *p, Buffers *b) {
    g_fn = fn; g_p = p; g_b = b;
    if (__builtin_setjmp(jb) == 0) { invoke(); return 0; }
    return 1;
}

typedef struct { unsigned long long start; const char *name; int known; } Target;

int main(void) {
    FILE *f = fopen("out/eboot.elf", "rb");
    if (!f) f = fopen("../out/eboot.elf", "rb");
    if (!f) f = fopen("../../out/eboot.elf", "rb");
    if (!f) { printf("SKIP: out/eboot.elf not found\n"); return 0; }
    unsigned char *image = VirtualAlloc(NULL, IMAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!image) { printf("FAIL: cannot allocate the image\n"); return 1; }
    fseek(f, 0x4000, SEEK_SET);
    size_t got = fread(image, 1, TEXT_SIZE, f);
    fclose(f);
    if (got != TEXT_SIZE) { printf("FAIL: eboot too short (%zu bytes of text)\n", got); return 1; }
    AddVectoredExceptionHandler(1, veh);

    int failures = 0;
    for (size_t t = 0; t < sizeof rz_entries / sizeof rz_entries[0]; ++t) {
        const RzEntry *e = &rz_entries[t];
        int known = e->start == 0x28ceec0ull;
        const char *error = NULL;
        unsigned char *blob = redzone_guard_build(image, e, &error);
        if (!blob) { printf("FAIL: 0x%llx build: %s\n", e->start, error); failures++; continue; }
        void *orig = image + e->start;
        static const unsigned masks[] = {0xFFF, 0x000, 0xAAA, 0x555, 0xFF0, 0x00F, 0x0FF, 0xF00, 0x3F0, 0xFC0};
        unsigned mask = 0; int usable = known;
        Params p;
        if (!known) {
            for (size_t m = 0; m < sizeof masks / sizeof masks[0] + 400 && !usable; ++m) {
                int ok = 1;
                unsigned cand = m < sizeof masks / sizeof masks[0] ? masks[m] : (unsigned)(rnd() & 0xFFF);
                for (int k = 0; k < 20 && ok; ++k) {
                    fill_base(); memcpy(&buf_a, &base_buf, sizeof buf_a);
                    make_params(&p, 0, cand);
                    ok = !guarded(orig, &p, &buf_a);
                }
                if (ok) { usable = 1; mask = cand; }
            }
            if (!usable) {
                printf("NOT EXERCISED 0x%llx: the generic call faults on the original (%u bytes relocated copy built ok)\n",
                       e->start, e->blob_size);
                continue;
            }
        }
        int mismatches = 0, faults = 0, run = 0, changed = 0;
        for (int c = 0; c < CASES; ++c) {
            fill_base();
            make_params(&p, known, mask);
            memcpy(&buf_a, &base_buf, sizeof buf_a);
            int fa = guarded(orig, &p, &buf_a);
            memcpy(&buf_b, &buf_a, sizeof buf_b); /* result of the original */
            memcpy(&buf_a, &base_buf, sizeof buf_a); /* same buffers (same addresses) for the copy */
            int fb = guarded(blob, &p, &buf_a);
            if (fa) { faults++; if (!fb) mismatches++; continue; } /* original faults but the copy does not */
            ++run;
            if (memcmp(buf_b.out, base_buf.out, sizeof buf_b.out)) ++changed;
            if (fb || memcmp(&buf_a, &buf_b, sizeof buf_a)) {
                if (mismatches < 3) {
                    size_t at = 0; while (at < sizeof buf_a && ((unsigned char *)&buf_a)[at] == ((unsigned char *)&buf_b)[at]) ++at;
                    printf("  mismatch 0x%llx case %d n=%u mask=%03x%s first diff at byte %zu\n", e->start, c, p.n, mask, fb ? " (copy faulted)" : "", at);
                    { size_t q = at & ~(size_t)3; float u, v; memcpy(&u, (unsigned char *)&buf_b + q, 4); memcpy(&v, (unsigned char *)&buf_a + q, 4); printf("    orig %g copy %g (word at %zu)\n", u, v, q); }
                }
                mismatches++;
            }
        }
        printf("%s 0x%llx: %d cases compared, %d mismatches, %d original-fault cases, output written in %d%s\n",
               mismatches ? "FAIL" : "PASS", e->start, run, mismatches, faults, changed, known ? " (known signature)" : " (generic call)");
        if (mismatches || (known && run < CASES / 2)) failures++;
    }
    if (failures) { printf("%d red zone guard test failure(s)\n", failures); return 1; }
    printf("All red zone guard tests passed\n");
    return 0;
}
#endif
