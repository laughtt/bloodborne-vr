/* Custom character model override, host side (BB_VR_SKIN). The VR UI layer (bbport_xr.dll) draws a
 * user-supplied mesh over other player characters; this file finds them in guest memory and gives
 * it, every pad poll (src/runtime_vr_ui.c), their feet, facing and speed in the headset's space.
 *
 * A background finder (every 2 s) reads the players' character set: WorldChrMan+0x30 slot count,
 * +0x38 entries of 0x38 bytes, entry+0 PlayerIns* (slot 0 = the local player, co-op guests 1..;
 * src/vr_skin_math.h), each checked by the PlayerIns vtable (VA 0x578F310), and the online name
 * from the net info [PlayerIns+0x3D8]+0x36 (ASCII). Only when that set cannot be read does it
 * fall back to the old search: a WorldChrMan pointer walk, then a heap scan for the vtable.
 * Candidates are checked (bag/physics chain readable, finite position within BB_VR_SKIN_RANGE_M)
 * and published to the frame thread. Character set slots keep their index as the slot id
 * (BB_VR_SKIN_TARGET=1 is the first guest); fallback hits get ids from 16 up.
 * Only first person (the hitbox overlay's view) gives a headset-space pose; otherwise nothing is sent.
 * Environment: BB_VR_SKIN=1 (off otherwise), BB_VR_SKIN_TARGET=all|<index>|<name> (default all),
 * BB_VR_SKIN_RANGE_M (60), BB_VR_SKIN_HIDE=1 (hide the selected characters' real model),
 * BB_VR_SKIN_SELF=1 (include the local player, for solo tests). A name selector is compared
 * (case-insensitive) with the online name; for fallback hits it is looked for as UTF-16 in the
 * PlayerIns object and one level of pointers (BB_USER_NAME on the local player teaches the offset). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/xr_bridge/bbport_xr_bridge.h"
#include "vr_hitbox.h"
#include "vr_skin.h"
#include "vr_skin_math.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SK_VA_BASE 0x400000u
#define SK_VA_WORLD_CHR_MAN 0x593E878u
#define SK_VA_PLAYER_VTABLE 0x578F310u
#define SK_VA_DISP_VTABLE 0x57A0820u
#define SK_VA_NETINFO_VTABLE 0x56C5C00u
#define SK_FALLBACK_SLOT 16
#define SK_STALE_US 300000u
#define SK_MAX_CAND 32

static double sk_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static struct {
    int checked, on, hide, self;
    float range;
    char target[64];
    char own_name[64];
} sk;

static void sk_init(void) {
    if (sk.checked) return;
    sk.checked = 1;
    const char *e = getenv("BB_VR_SKIN");
    sk.on = e && *e && strcmp(e, "0") != 0;
    sk.range = 60.0f;
    if ((e = getenv("BB_VR_SKIN_RANGE_M")) && atof(e) > 0.0) sk.range = (float)atof(e);
    if ((e = getenv("BB_VR_SKIN_TARGET")) && *e) snprintf(sk.target, sizeof(sk.target), "%s", e);
    if ((e = getenv("BB_VR_SKIN_HIDE"))) sk.hide = *e && strcmp(e, "0") != 0;
    if ((e = getenv("BB_VR_SKIN_SELF"))) sk.self = *e && strcmp(e, "0") != 0;
    if ((e = getenv("BB_USER_NAME")) && *e) snprintf(sk.own_name, sizeof(sk.own_name), "%s", e);
}

#ifndef _WIN32
void vr_skin_fill(struct BbXrSkinState *s, unsigned char *image) {
    (void)image;
    memset(s, 0, sizeof(*s));
    sk_init();
    static int logged;
    if (sk.on && !logged) {
        logged = 1;
        fprintf(stderr, "VR skin: BB_VR_SKIN is set but the finder is Windows only; nothing is sent\n");
    }
}
#else
#include <windows.h>

/* ---- guest memory: a readable-region guard (one instance per thread) ---- */
typedef struct {
    uintptr_t lo[16], hi[16];
    int n;
    double t;
} SkGuard;

static int sk_readable(SkGuard *g, uintptr_t a, size_t n) {
    if (a < 0x10000 || a + n < a) return 0;
    const double now = sk_now();
    if (now - g->t > 1.0) { g->n = 0; g->t = now; }
    for (int i = 0; i < g->n; ++i)
        if (g->lo[i] <= a && a + n <= g->hi[i]) return 1;
    uintptr_t at = a;
    while (at < a + n) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void *)at, &mbi, sizeof(mbi))) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return 0;
        const uintptr_t lo = (uintptr_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
        if (g->n < 16) { g->lo[g->n] = lo; g->hi[g->n] = hi; ++g->n; }
        at = hi;
    }
    return 1;
}
/* Reads through ReadProcessMemory on ourselves: the finder thread walks objects the game may free
 * at any moment, and a read that loses that race fails instead of faulting. */
static int sk_read(SkGuard *g, uintptr_t a, void *out, size_t n) {
    (void)g;
    SIZE_T got = 0;
    if (a < 0x10000 || a + n < a) return 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)a, out, n, &got) && got == n;
}
static int sk_plausible(uint64_t v) { return v >= 0x10000 && v < 0x800000000000ull; }
static int sk_ptr(SkGuard *g, uintptr_t a, uintptr_t *out) {
    uint64_t v;
    if (!sk_read(g, a, &v, 8) || !sk_plausible(v) || (v & 7)) return 0;
    *out = (uintptr_t)v;
    return 1;
}

typedef struct { uintptr_t player; VrVec3 pos; float yaw; } SkPlayer;

/* The player at `p`: vtable, bag, physics, finite position and yaw. */
static int sk_player_read(SkGuard *g, uintptr_t base, uintptr_t p, SkPlayer *out) {
    uint64_t vt;
    uintptr_t bag, phys;
    float v[3], yaw;
    if (!sk_read(g, p, &vt, 8) || vt != (uint64_t)(base + SK_VA_PLAYER_VTABLE) || !sk_ptr(g, p + 0x3B0, &bag) ||
        !sk_ptr(g, bag + 0x68, &phys) || !sk_read(g, phys + 0x1E0, v, 12) || !sk_read(g, phys + 0x1D4, &yaw, 4))
        return 0;
    if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]) || !isfinite(yaw)) return 0;
    if (fabsf(v[0]) > 1e5f || fabsf(v[1]) > 1e5f || fabsf(v[2]) > 1e5f) return 0;
    out->player = p;
    out->pos = vr_v3(v[0], v[1], v[2]);
    out->yaw = yaw;
    return 1;
}
static int sk_local(SkGuard *g, uintptr_t base, SkPlayer *out) {
    uintptr_t wcm, p;
    return sk_ptr(g, base + SK_VA_WORLD_CHR_MAN, &wcm) && sk_ptr(g, wcm + 0x60, &p) && sk_player_read(g, base, p, out);
}

/* ---- shared candidate list (finder -> frame) ---- */
typedef struct {
    uintptr_t addr;
    int slot, name_match;
    char how[64];
    char name[VSK_NAME_MAX + 1];
} SkCand;

static SRWLOCK sk_lock = SRWLOCK_INIT;
static SkCand sk_pub[SK_MAX_CAND];
static int sk_pub_n;
static uintptr_t sk_slot_addr[64];
static int sk_slot_n;
static volatile uintptr_t sk_image;
static volatile LONG sk_started;

static int sk_slot_of(uintptr_t a) {
    for (int i = 0; i < sk_slot_n; ++i) if (sk_slot_addr[i] == a) return i;
    if (sk_slot_n < 64) { sk_slot_addr[sk_slot_n] = a; return sk_slot_n++; }
    return 63;
}

/* ---- name search: UTF-16 in the object and one pointer level ---- */
typedef struct { int level; long off1, off2; } SkNamePath; /* level -1: unknown; 0: [P+off2]; 1: [[P+off1]+off2] */
static SkNamePath sk_learned = {-1, 0, 0};

static int sk_name_at(SkGuard *g, uintptr_t p, const SkNamePath *np, const char *name) {
    uintptr_t at = p + (uintptr_t)np->off2;
    if (np->level == 1) {
        if (!sk_ptr(g, p + (uintptr_t)np->off1, &at)) return 0;
        at += (uintptr_t)np->off2;
    }
    unsigned char b[128];
    const size_t len = strlen(name) * 2;
    if (len > sizeof(b) || !sk_read(g, at, b, len)) return 0;
    return vsk_utf16_find(b, len, name) == 0;
}
/* Searches the name; fills `found` with the path. */
static int sk_name_search(SkGuard *g, uintptr_t p, const char *name, SkNamePath *found) {
    unsigned char buf[0x1000];
    if (!*name) return 0;
    if (sk_learned.level >= 0 && sk_name_at(g, p, &sk_learned, name)) {
        if (found) *found = sk_learned;
        return 1;
    }
    if (sk_read(g, p, buf, 0x1000)) {
        const long o = vsk_utf16_find(buf, 0x1000, name);
        if (o >= 0) {
            if (found) { found->level = 0; found->off1 = 0; found->off2 = o; }
            return 1;
        }
        for (int i = 0; i < 0x1000; i += 8) {
            uint64_t q;
            memcpy(&q, buf + i, 8);
            unsigned char blk[0x400];
            if (!sk_plausible(q) || (q & 7) || !sk_read(g, (uintptr_t)q, blk, sizeof(blk))) continue;
            const long o2 = vsk_utf16_find(blk, sizeof(blk), name);
            if (o2 >= 0) {
                if (found) { found->level = 1; found->off1 = i; found->off2 = o2; }
                return 1;
            }
        }
    }
    return 0;
}
static int sk_name_selector(void) {
    return sk.target[0] && !(sk.target[0] >= '0' && sk.target[0] <= '9') && strcmp(sk.target, "all") != 0;
}

/* ---- finder ---- */
/* set_slot: the character set index, or -1 for a fallback hit */
typedef struct { uintptr_t addr; int set_slot; char how[64]; char name[VSK_NAME_MAX + 1]; } SkHit;
typedef struct { SkHit h[SK_MAX_CAND]; int n; } SkHits;

static SkHit *sk_add_hit(SkHits *hs, uintptr_t a, const char *how) {
    for (int i = 0; i < hs->n; ++i) if (hs->h[i].addr == a) return NULL;
    if (hs->n >= SK_MAX_CAND) return NULL;
    SkHit *h = &hs->h[hs->n++];
    memset(h, 0, sizeof(*h));
    h->addr = a;
    h->set_slot = -1;
    snprintf(h->how, sizeof(h->how), "%s", how);
    return h;
}

static void sk_path_found(const char *path) {
    static char seen[64][64];
    static int n;
    for (int i = 0; i < n; ++i) if (!strcmp(seen[i], path)) return;
    if (n < 64) snprintf(seen[n++], 64, "%s", path);
    fprintf(stderr, "VR skin: PlayerIns via %s\n", path);
}

static int sk_is_vt(SkGuard *g, uintptr_t base, uintptr_t p) {
    uint64_t vt;
    return sk_read(g, p, &vt, 8) && vt == (uint64_t)(base + SK_VA_PLAYER_VTABLE);
}

/* The online name of the player at `p` (net info [p+0x3D8], vtable-checked), or "" when unknown. */
static void sk_net_name(SkGuard *g, uintptr_t base, uintptr_t p, char out[VSK_NAME_MAX + 1]) {
    uintptr_t info;
    uint64_t vt;
    unsigned char b[VSK_NAME_MAX + 2];
    out[0] = 0;
    if (!sk_ptr(g, p + VSK_PLAYER_NETINFO, &info) || !sk_read(g, info, &vt, 8) ||
        vt != (uint64_t)(base + SK_VA_NETINFO_VTABLE) || !sk_read(g, info + VSK_NETINFO_NAME, b, sizeof(b)))
        return;
    vsk_net_name(b, sizeof(b), out);
}

/* The players' character set: adds the other players to hs. Returns 1 when the set was read and holds
 * the local player (slot 0 as world host; another slot when visiting), 0 otherwise (the caller then
 * falls back to the search). */
static int sk_chrset(SkGuard *g, uintptr_t base, uintptr_t wcm, uintptr_t local, SkHits *hs) {
    uint32_t raw;
    uintptr_t slots;
    int has_local = 0;
    if (!sk_read(g, wcm + VSK_WCM_SLOT_COUNT, &raw, 4) || !sk_ptr(g, wcm + VSK_WCM_SLOTS, &slots)) return 0;
    const int count = vsk_slot_count(raw);
    for (int i = 0; i < count; ++i) {
        uint64_t p;
        if (!sk_read(g, slots + (uintptr_t)i * VSK_SLOT_STRIDE, &p, 8) || !sk_plausible(p) || (p & 7) ||
            !sk_is_vt(g, base, (uintptr_t)p))
            continue;
        if ((uintptr_t)p == local) {
            has_local = 1;
            continue;
        }
        char how[64];
        snprintf(how, sizeof(how), "WorldChrMan+0x%X[%d]", VSK_WCM_SLOTS, i);
        SkHit *h = sk_add_hit(hs, (uintptr_t)p, how);
        if (!h) continue;
        h->set_slot = i;
        sk_net_name(g, base, (uintptr_t)p, h->name);
    }
    if (!has_local) hs->n = 0; /* not the players' set (or not built yet): drop what it gave */
    return has_local;
}

static void sk_walk(SkGuard *g, uintptr_t base, uintptr_t wcm, SkHits *hs) {
    uint64_t w[0x80];
    int reads = 0;
    if (!sk_read(g, wcm, w, sizeof(w))) return;
    for (int i = 0; i < 0x80; ++i) {
        const uint64_t a = w[i];
        if (!sk_plausible(a) || (a & 7)) continue;
        char path[64];
        ++reads;
        if (sk_is_vt(g, base, (uintptr_t)a)) {
            snprintf(path, sizeof(path), "WorldChrMan+0x%X", i * 8);
            sk_path_found(path);
            sk_add_hit(hs, (uintptr_t)a, path);
            continue;
        }
        uint64_t b1[0x40];
        if (!sk_read(g, (uintptr_t)a, b1, sizeof(b1))) continue;
        for (int j = 0; j < 0x40; ++j) {
            const uint64_t b = b1[j];
            if (!sk_plausible(b) || (b & 7)) continue;
            ++reads;
            if (sk_is_vt(g, base, (uintptr_t)b)) {
                snprintf(path, sizeof(path), "WorldChrMan+0x%X ->[+0x%X]", i * 8, j * 8);
                sk_path_found(path);
                sk_add_hit(hs, (uintptr_t)b, path);
                continue;
            }
            if (reads > 20000) continue;
            uint64_t b2[0x20];
            if (!sk_read(g, (uintptr_t)b, b2, sizeof(b2))) continue;
            ++reads;
            for (int k = 0; k < 0x20; ++k) {
                const uint64_t c = b2[k];
                if (!sk_plausible(c) || (c & 7) || reads > 20000) continue;
                ++reads;
                if (sk_is_vt(g, base, (uintptr_t)c)) {
                    snprintf(path, sizeof(path), "WorldChrMan+0x%X ->[+0x%X] ->[+0x%X]", i * 8, j * 8, k * 8);
                    sk_path_found(path);
                    sk_add_hit(hs, (uintptr_t)c, path);
                }
            }
        }
    }
}

/* Heap scan for the vtable value near `center`. Reads through ReadProcessMemory (never faults). */
static void sk_heap_scan(uintptr_t base, uintptr_t center, SkHits *hs) {
    const uint64_t want = (uint64_t)(base + SK_VA_PLAYER_VTABLE);
    const uintptr_t span = (uintptr_t)768 << 20;
    const uintptr_t lo = center > span ? center - span : 0x10000, hi = center + span;
    const double t0 = sk_now();
    const size_t cap = (size_t)1536 << 20;
    size_t scanned = 0;
    int hits = 0;
    static unsigned char buf[1 << 20];
    HANDLE self = GetCurrentProcess();
    uintptr_t at = lo;
    while (at < hi && scanned < cap) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void *)at, &mbi, sizeof(mbi))) break;
        const uintptr_t rlo = (uintptr_t)mbi.BaseAddress, rhi = rlo + mbi.RegionSize;
        const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mbi.Protect & rw) &&
            mbi.Type != MEM_IMAGE) {
            uintptr_t s = rlo < lo ? lo : rlo;
            const uintptr_t e = rhi > hi ? hi : rhi;
            s = (s + 7) & ~(uintptr_t)7;
            while (s < e && scanned < cap) {
                size_t n = e - s;
                if (n > sizeof(buf)) n = sizeof(buf);
                SIZE_T got = 0;
                if (ReadProcessMemory(self, (const void *)s, buf, n, &got) && got >= 8) {
                    for (size_t o = 0; o + 8 <= got; o += 8) {
                        uint64_t q;
                        memcpy(&q, buf + o, 8);
                        if (q == want) {
                            sk_add_hit(hs, s + o, "heap scan");
                            ++hits;
                        }
                    }
                }
                scanned += n;
                s += n;
            }
            Sleep(1);
        }
        at = rhi > at ? rhi : at + 0x1000;
    }
    fprintf(stderr, "VR skin: heap scan %.0f MB in %.0f ms, %d vtable hits\n", (double)scanned / 1048576.0,
            (sk_now() - t0) * 1000.0, hits);
}

/* One line per guest when it appears (or comes back at a new address). */
static void sk_guest_log(const SkHit *h, const SkPlayer *p, float dist, int name_match) {
    static uintptr_t seen[SK_MAX_CAND];
    static int seen_n, next;
    for (int i = 0; i < seen_n; ++i) if (seen[i] == h->addr) return;
    if (seen_n < SK_MAX_CAND) seen[seen_n++] = h->addr;
    else { seen[next] = h->addr; next = (next + 1) % SK_MAX_CAND; }
    fprintf(stderr, "VR skin: guest %s found at %s (PlayerIns %p) pos (%.1f %.1f %.1f) dist %.1f m name_match=%d\n",
            h->name[0] ? h->name : "(no name)", h->how, (void *)h->addr, (double)p->pos.x, (double)p->pos.y,
            (double)p->pos.z, (double)dist, name_match);
}

static void sk_publish(SkGuard *g, uintptr_t base, const SkPlayer *loc, const SkHits *hs) {
    SkCand c[SK_MAX_CAND];
    int n = 0;
    for (int i = 0; i < hs->n && n < SK_MAX_CAND; ++i) {
        SkPlayer p;
        const uintptr_t a = hs->h[i].addr;
        if (a == loc->player || !sk_player_read(g, base, a, &p)) continue;
        const float dx = p.pos.x - loc->pos.x, dy = p.pos.y - loc->pos.y, dz = p.pos.z - loc->pos.z;
        if (sqrtf(dx * dx + dy * dy + dz * dz) > sk.range) continue;
        const SkHit *h = &hs->h[i];
        c[n].addr = a;
        c[n].slot = h->set_slot;
        snprintf(c[n].how, sizeof(c[n].how), "%s", h->how);
        snprintf(c[n].name, sizeof(c[n].name), "%s", h->name);
        if (!sk_name_selector()) c[n].name_match = 0;
        else if (h->name[0]) c[n].name_match = vsk_name_eq(h->name, sk.target);
        else c[n].name_match = sk_name_search(g, a, sk.target, NULL);
        sk_guest_log(h, &p, sqrtf(dx * dx + dy * dy + dz * dz), c[n].name_match);
        ++n;
    }
    if (sk.self && n < SK_MAX_CAND) {
        c[n].addr = loc->player;
        c[n].slot = 0;
        snprintf(c[n].how, sizeof(c[n].how), "local player (BB_VR_SKIN_SELF)");
        sk_net_name(g, base, loc->player, c[n].name);
        if (!sk_name_selector()) c[n].name_match = 0;
        else if (c[n].name[0]) c[n].name_match = vsk_name_eq(c[n].name, sk.target);
        else c[n].name_match = sk_name_search(g, loc->player, sk.target, NULL);
        ++n;
    }
    AcquireSRWLockExclusive(&sk_lock);
    for (int i = 0; i < n; ++i)
        if (c[i].slot < 0) c[i].slot = SK_FALLBACK_SLOT + sk_slot_of(c[i].addr);
    memcpy(sk_pub, c, sizeof(SkCand) * (size_t)n);
    sk_pub_n = n;
    ReleaseSRWLockExclusive(&sk_lock);
}

static DWORD WINAPI sk_finder(LPVOID arg) {
    (void)arg;
    SkGuard g;
    memset(&g, 0, sizeof(g));
    static SkHits known;
    double last_scan = -100.0;
    int own_logged = 0;
    int set_ok = -1;
    for (;; Sleep(2000)) {
        const uintptr_t image = sk_image;
        if (!image) continue;
        const uintptr_t base = image - SK_VA_BASE;
        g.n = 0;
        SkPlayer loc;
        if (!sk_local(&g, base, &loc)) continue;
        if (!own_logged && sk.own_name[0]) {
            SkNamePath np;
            if (sk_name_search(&g, loc.player, sk.own_name, &np)) {
                own_logged = 1;
                sk_learned = np;
                if (np.level == 1)
                    fprintf(stderr, "VR skin: own name at [PlayerIns+0x%lX]+0x%lX\n", np.off1, np.off2);
                else
                    fprintf(stderr, "VR skin: own name at PlayerIns+0x%lX\n", np.off2);
            }
        }
        static SkHits hs;
        memset(&hs, 0, sizeof(hs));
        uintptr_t wcm;
        if (!sk_ptr(&g, base + SK_VA_WORLD_CHR_MAN, &wcm)) continue;
        const int ok = sk_chrset(&g, base, wcm, loc.player, &hs);
        if (ok != set_ok) {
            set_ok = ok;
            fprintf(stderr, ok ? "VR skin: reading the players' character set (WorldChrMan+0x%X)\n"
                               : "VR skin: players' character set unreadable (WorldChrMan+0x%X); searching\n",
                    VSK_WCM_SLOTS);
        }
        if (ok) {
            sk_publish(&g, base, &loc, &hs);
            continue;
        }
        sk_walk(&g, base, wcm, &hs);
        /* heap-scan hits found before stay candidates while they validate */
        for (int i = 0; i < known.n; ++i) sk_add_hit(&hs, known.h[i].addr, known.h[i].how);
        int others = 0;
        for (int i = 0; i < hs.n; ++i) {
            SkPlayer p;
            if (hs.h[i].addr != loc.player && sk_player_read(&g, base, hs.h[i].addr, &p)) ++others;
        }
        if (!others && sk_now() - last_scan > 12.0) {
            last_scan = sk_now();
            sk_heap_scan(base, loc.player, &hs);
            for (int i = 0; i < hs.n; ++i) sk_add_hit(&known, hs.h[i].addr, hs.h[i].how);
        }
        sk_publish(&g, base, &loc, &hs);
    }
    return 0;
}

/* ---- frame side ---- */
static int sk_hide(SkGuard *g, uintptr_t base, uintptr_t player) {
    int n = 0;
    uintptr_t holder, bm, disp;
    uintptr_t models[1 + 24];
    int nm = 0;
    if (sk_ptr(g, player + 0x48, &bm)) models[nm++] = bm;
    if (sk_ptr(g, player + 0x4D0, &holder))
        for (int i = 0; i < 24; ++i) {
            uint64_t m;
            if (sk_read(g, holder + 0x28 + 8u * (uintptr_t)i, &m, 8) && sk_plausible(m) && !(m & 7))
                models[nm++] = (uintptr_t)m;
        }
    for (int i = 0; i < nm; ++i) {
        uint64_t vt;
        if (!sk_ptr(g, models[i] + 0x18, &disp) || !sk_read(g, disp, &vt, 8) ||
            vt != (uint64_t)(base + SK_VA_DISP_VTABLE) || !sk_readable(g, disp + 0x210, 1))
            continue;
        *(volatile uint8_t *)(disp + 0x210) &= (uint8_t)~1u;
        ++n;
    }
    return n;
}

/* Per-second draw counts: polls, polls without a first-person view, candidates, and how many polls
 * drew each selected character. A line is printed each second while there are candidates or draws. */
static struct {
    double t0;
    int polls, noview, cand_max, drawn;
    int slot_polls[64];
    char names[64][VSK_NAME_MAX + 1];
    char unmatched[96];
} sk_st;

static void sk_stat_flush(double now) {
    if (sk_st.t0 == 0.0) sk_st.t0 = now;
    if (now - sk_st.t0 < 1.0) return;
    if (sk_st.cand_max > 0 || sk_st.drawn > 0) {
        char drew[256];
        int len = 0;
        drew[0] = 0;
        for (int i = 0; i < 64 && len < (int)sizeof(drew) - 40; ++i)
            if (sk_st.slot_polls[i])
                len += snprintf(drew + len, sizeof(drew) - (size_t)len, " %s(slot %d) x%d",
                                sk_st.names[i][0] ? sk_st.names[i] : "?", i, sk_st.slot_polls[i]);
        fprintf(stderr, "VR skin: %.1f s: %d polls (%d without view), %d candidate(s), drew%s%s%s\n",
                now - sk_st.t0, sk_st.polls, sk_st.noview, sk_st.cand_max, len ? drew : " nothing",
                sk_st.unmatched[0] ? "; target matches none of:" : "", sk_st.unmatched);
    }
    memset(&sk_st, 0, sizeof(sk_st));
    sk_st.t0 = now;
}

void vr_skin_fill(struct BbXrSkinState *s, unsigned char *image) {
    static SkGuard g;
    static VskSpeed speed[64];
    static uint64_t logged_mask;
    memset(s, 0, sizeof(*s));
    sk_init();
    if (!sk.on || !image) return;
    if (!sk_started) {
        sk_started = 1;
        sk_image = (uintptr_t)image;
        fprintf(stderr, "VR skin: on (target=%s range=%.0f m hide=%d self=%d)\n", sk.target[0] ? sk.target : "all",
                (double)sk.range, sk.hide, sk.self);
        HANDLE h = CreateThread(NULL, 0, sk_finder, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    VhbView view;
    const double now = sk_now();
    ++sk_st.polls;
    if (!vr_arms_hitbox_view(&view, SK_STALE_US)) {
        ++sk_st.noview;
        sk_stat_flush(now);
        return;
    }
    const uintptr_t base = (uintptr_t)image - SK_VA_BASE;
    SkCand cl[SK_MAX_CAND];
    int n;
    AcquireSRWLockShared(&sk_lock);
    n = sk_pub_n;
    memcpy(cl, sk_pub, sizeof(SkCand) * (size_t)n);
    ReleaseSRWLockShared(&sk_lock);
    SkPlayer pl[SK_MAX_CAND];
    VskCand vc[SK_MAX_CAND];
    int m = 0, src[SK_MAX_CAND];
    g.n = 0;
    for (int i = 0; i < n; ++i) {
        if (!sk_player_read(&g, base, cl[i].addr, &pl[m])) continue;
        const float dx = pl[m].pos.x - view.body_pos.x, dy = pl[m].pos.y - view.body_pos.y,
                    dz = pl[m].pos.z - view.body_pos.z;
        vc[m].slot = cl[i].slot;
        vc[m].dist = sqrtf(dx * dx + dy * dy + dz * dz);
        vc[m].name_match = cl[i].name_match;
        if (vc[m].dist > sk.range) continue;
        src[m] = i;
        ++m;
    }
    int sel[VSK_MAX];
    const int ns = vsk_select(vc, m, sk.target, sel);
    if (m > sk_st.cand_max) sk_st.cand_max = m;
    if (ns == 0 && m > 0 && sk_name_selector() && !sk_st.unmatched[0]) {
        int len = 0;
        for (int i = 0; i < m && len < (int)sizeof(sk_st.unmatched) - 24; ++i)
            len += snprintf(sk_st.unmatched + len, sizeof(sk_st.unmatched) - (size_t)len, " %s",
                            cl[src[i]].name[0] ? cl[src[i]].name : "(no name)");
    }
    if (ns > 0) ++sk_st.drawn;
    for (int k = 0; k < ns; ++k) {
        const int ci = sel[k], si = src[ci], slot = cl[si].slot;
        BbXrSkinTarget *t = &s->targets[s->count++];
        vsk_target_from_world(&view, pl[ci].pos, pl[ci].yaw, t->pos, t->forward, t->up);
        t->speed_mps = vsk_speed_update(&speed[slot & 63], pl[ci].pos, now);
        t->id = (uint32_t)slot;
        ++sk_st.slot_polls[slot & 63];
        snprintf(sk_st.names[slot & 63], sizeof(sk_st.names[0]), "%s", cl[si].name);
        if (sk.hide) sk_hide(&g, base, pl[ci].player);
        if (slot < 64 && !(logged_mask & (1ull << slot))) {
            logged_mask |= 1ull << slot;
            fprintf(stderr, "VR skin: drawing %s slot %d at %p pos (%.1f %.1f %.1f) dist %.1f m via %s name_match=%d\n",
                    cl[si].name[0] ? cl[si].name : "(no name)", slot, (void *)cl[si].addr, (double)pl[ci].pos.x, (double)pl[ci].pos.y, (double)pl[ci].pos.z,
                    (double)vc[ci].dist, cl[si].how, cl[si].name_match);
        }
    }
    sk_stat_flush(now);
}
#endif
