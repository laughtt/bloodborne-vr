/* Instant items (docs/vr/findings/44-instant-items.md): a held item's gesture (findings/39) uses the item
 * at once, with no use animation; our own hands do the action, the game's body does nothing.
 *
 *   SpEffect goods (vials, pellets, papers, ...): the game's PlayerIns::UseGoods (vtable slot 0x408,
 *     0x1cf9720) with dummy poly -1, the call the use animation's TAE event makes (0x1e25b0c) and the
 *     game's debug menu makes (0x1ccf180): the SpEffect (slot 0x3F0, with the virtual weapon's scaling),
 *     the count drop (0x18d0be0), the menu flags. Nothing of ours in between.
 *   Cures (Antidote, Sedative, Lead Elixir: bullets with spEffectIDForShooter): the bullet request
 *     UseGoods builds (src/vr_instant_items.h vri_build_request) launched at the chest through the game's
 *     0x1cb6680, then one item off (the copy of UseGoods' consume, vi_consume).
 *   Throws: one item off at the release; our projectile (vri_flight_step: the BulletParam rules, the game's
 *     own ray cast 0x1bfe5a0 against the map, the lock-on candidates as character spheres, a floor plane as
 *     the fallback) flies from the hand; where it lands the burst bullet (HitBulletID, Molotov 101) is
 *     requested at that point, or for a bullet without a burst (knives, pebble) the bullet itself 0.5 m
 *     before the point. The headset draws the item in flight (BBXR_EV_INSTANT_THROW / _LAND).
 * Before each use: the game's own "can use this item now" predicate 0x197f200 (the quick-item key and the
 * item menu call it), a held count > 0, no use running (PlayerIns+0x490), no game menu page, and a cooldown
 * as long as the vanilla animation (vri_cooldown_s; BB_VR_INSTANT_COOLDOWN_S overrides).
 * Quicksilver tools that fly: Quicksilver paid (0x1cf9f00), their own bullet requested at the hand along the release.
 * Choir Bell / Shaman Bone Blade: their bullet at (or in front of) the chest. Rope molotovs: thrown like the Molotov.
 * The multiplayer bells (and a throwable used without a throw) are handed back to the usual path: the quick slot and
 * square, with the animation (findings/44 section 6 has the per-item table).
 *
 * Environment (default OFF until the headset test):
 *   BB_VR_INSTANT_ITEMS=1      on; or instant_items=1 in the control file (BB_VR_FP_CONTROL / BB_VR_ARMS_CONTROL,
 *                              out/fp.txt), polled 4 Hz; the key overrides the env value
 *   BB_VR_INSTANT_RAY=0        no game ray cast (the floor plane at the feet only)
 *   BB_VR_INSTANT_COOLDOWN_S=<s>  one cooldown for every item (default: per use animation)
 *   BB_VR_INSTANT_CHR_R=0.6    the radius of the character spheres (m)
 *   BB_VR_THROW_SPEED_REF / _MIN / _MAX  the hand speed mapping (shared with the throw redirect: 7 / 0.3 / 1.5)
 * Online (BB_ONLINE=1 or BB_ONLINE_ROLE set) it stays off: single player only. */
#define _GNU_SOURCE
#include "runtime.h"
#include "vr_mem_cache.h"
#include "vr_items.h"
#include "vr_instant_items.h"
#include "vr_ui_game.h"
#include "vr_gun.h"
#include "gpu/bbgpu.h"
#include "gpu/xr_bridge/bbport_xr_bridge.h"
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define VA_BASE 0x400000u
#define VA_WORLD_CHR_MAN 0x593E878u
#define VA_PLAYER_INS_VTABLE 0x578F310u
#define VA_CHR_DATA_VTABLE 0x5735810u
#define VA_MENU_MAN 0x5962878u
#define VA_GOODS_LOOKUP 0x231E3D0u   /* (out {s32 id; ...; +8 row*}, goods): EquipParamGoods row */
#define VA_BULLET_LOOKUP 0x2314860u  /* (out, bullet id): Bullet row, same out layout */
#define VA_CAN_USE_GOODS 0x197F200u  /* bool (goods, chr, [chr+0x1c8], [chr+0x78], wep(-1), wep(-2), menu>0) */
#define VA_REPLACE_OFFSET 0x1CC9390u /* int (chr, goods row +0x41): added to refId by UseGoods */
#define VA_BULLET_BEHAVIOR 0x1CB6680u /* bool (behavior request, chr+0x178, chr): the goods/TAE bullet request */
#define VA_QUICK_INDEX 0x18DEFF0u    /* int (pgd+0x3d0, goods): inventory index of a quick slot holding it */
#define VA_INV_INDEX 0x18D9F60u      /* int (pgd+0x328, const u32 *0x40000000|goods): inventory index or -1 */
#define VA_INV_ADD 0x18D0BE0u        /* int (pgd+0x1d0, index, delta, 0, 0, 0): the count change; returns the new count */
#define VA_RAY_CAST 0x1BFE5A0u       /* bool (unused, const vec4 *from, const vec4 *to, vec4 *pos, vec4 *normal) */
#define VA_HANDLE_TO_ID 0x1E89070u   /* ({u32 handle; u64 0; s32 id}): an equip handle to its item id (+0x10) */
#define VA_PAY_QUICKSILVER 0x1CF9F00u /* bool (player, n): Quicksilver (goods 900 + 901) paid, 0 = not enough, nothing taken */
#define VA_HAVOK_MAN 0x593D700u     /* [[x]+0x28]+8: the havok world the ray cast uses */
#define VA_FLAG_RAN_OUT 0x596020Au   /* bytes UseGoods writes after a use (0x1cf9862..0x1cf986f) */
#define VA_FLAG_USED 0x5960209u
#define VA_FLAG_USED2 0x5980C50u
#define PLAYER_VT_PGD 0x1C8u
#define PLAYER_VT_LOCAL1 0x1B8u
#define PLAYER_VT_LOCAL2 0x1C0u
#define PLAYER_VT_WEAPON 0x388u
#define PLAYER_VT_USE_GOODS 0x408u
#define PLAYER_EMIT_SLOT 0x178u
#define PLAYER_SPEFFECTS 0x1C8u
#define PLAYER_TEAM 0x78u
#define PLAYER_USE_STATE 0x490u
#define PGD_USE_REQUEST 0x458u
#define MENU_PAGE 0x3Cu
#define MENU_USE_FLAG 0x1C0u
#define VI_FLIGHTS 4
#define VI_SPAWN_TRIES 30

typedef unsigned char (ABI *ViUseGoodsFn)(void *player, int32_t goods, int32_t dummy);
typedef unsigned char (ABI *ViCanUseFn)(int32_t goods, void *chr, void *speffects, int32_t team, int32_t wep_r,
                                        int32_t wep_l, int32_t menu);
typedef int32_t (ABI *ViWeaponFn)(void *chr, int32_t which);
typedef float (ABI *ViScaleFn)(void *chr, int32_t wep);
typedef void *(ABI *ViPtrFn)(void *chr);
typedef unsigned char (ABI *ViBoolFn)(void *chr);
typedef void (ABI *ViLookupFn)(void *out, int32_t id);
typedef int32_t (ABI *ViReplaceFn)(void *chr, int32_t category);
typedef unsigned char (ABI *ViBehaviorFn)(void *br, void *slot, void *chr);
typedef int32_t (ABI *ViQuickIndexFn)(void *eit, int32_t goods);
typedef int32_t (ABI *ViInvIndexFn)(void *eid, const uint32_t *id);
typedef int32_t (ABI *ViInvAddFn)(void *egd, int32_t index, int32_t delta, int32_t c, int32_t d, int32_t e);
typedef unsigned char (ABI *ViPayQsFn)(void *player, int32_t n);
typedef unsigned char (ABI *ViRayFn)(void *unused, const float *from, const float *to, float *pos, float *normal);

enum { VI_REQ_IDLE = 0, VI_REQ_POSTED = 1, VI_REQ_DONE = 2, VI_REQ_RUNNING = 3 };
enum { VI_RES_DONE = 1, VI_RES_REFUSED = 2, VI_RES_FALLBACK = 3 };

static struct {
    int checked, env_on, file_on, on, online, ray_on, logged_state;
    float cooldown_env, chr_r, spd_ref, spd_lo, spd_hi;
    const char *control;
    double control_checked;
    long long control_mtime, control_size;
    unsigned char *image;
} it;
static atomic_int it_active;

/* The release of the last throw (pad thread writes, game thread takes). */
static struct {
    atomic_flag lock;
    int armed;
    double at;
    int have_origin;
    int32_t mm[3], dir_1e4[3];
    float origin[3], dir[3];
    int world_ok;
} rel = {ATOMIC_FLAG_INIT, 0, 0, 0, {0}, {0}, {0}, {0}, 0};

/* The one request in flight between the threads. */
static struct {
    atomic_int state;
    uint32_t kind, item;
    int32_t slot, speed_cms;
    double posted;
    /* the answer */
    int result, reason;
    char msg[240];
    /* the release that came with it (a fallback re-arms the game's throw redirect with it) */
    int had_release;
    int32_t mm[3], dir_1e4[3];
} rq;

/* Game thread only. */
typedef struct {
    int active;
    VriFlight f;
    VriBullet b;
    VriGoods g;
    int32_t spawn_id;
    int tries;
    VriV3 spawn_pos, spawn_dir;
    double started;
} ViFlight;
static ViFlight fl[VI_FLIGHTS];
static VriCooldown cool;
static double last_step;
static int ray_hits, ray_calls, ray_bad;

static double vi_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static void vi_lock(void) { while (atomic_flag_test_and_set_explicit(&rel.lock, memory_order_acquire)) {} }
static void vi_unlock(void) { atomic_flag_clear_explicit(&rel.lock, memory_order_release); }

/* ---- guest memory (the chains dangle during loads) ---- */
#ifdef _WIN32
static _Thread_local VmCache vi_cache;
static int vi_readable(uintptr_t a, size_t n) { return vm_readable(&vi_cache, a, n); }
#else
static int vi_readable(uintptr_t a, size_t n) { return a >= 0x10000 && a + n >= a; }
#endif
static int vi_rd(uint64_t a, void *out, size_t n) {
    if (!vi_readable((uintptr_t)a, n)) return 0;
    memcpy(out, (const void *)(uintptr_t)a, n);
    return 1;
}
static int vi_ui_read(void *user, uint64_t a, void *out, size_t n) {
    (void)user;
    return vi_rd(a, out, n);
}
static int vi_ptr(uint64_t a, uint64_t *out) {
    uint64_t v;
    if (!vi_rd(a, &v, 8) || v < 0x10000 || (v & 7)) return 0;
    *out = v;
    return 1;
}
static uint64_t vi_va(uint32_t v) { return (uint64_t)(uintptr_t)(it.image + (v - VA_BASE)); }
static void *vi_fn(uint32_t v) { return (void *)(uintptr_t)vi_va(v); }
static void *vi_slot(uint64_t obj, uint32_t slot) {
    uint64_t vt, fn;
    if (!vi_ptr(obj, &vt) || !vi_rd(vt + slot, &fn, 8) || fn < 0x10000) return NULL;
    return (void *)(uintptr_t)fn;
}
static int vi_wr(uint64_t a, const void *v, size_t n) {
    if (!vi_readable((uintptr_t)a, n)) return 0;
    memcpy((void *)(uintptr_t)a, v, n);
    return 1;
}

/* ---- the setting ---- */
static float vi_envf(const char *name, float def, float lo, float hi) {
    const char *e = getenv(name);
    if (!e || !*e) return def;
    const float v = strtof(e, NULL);
    return isfinite(v) && v >= lo && v <= hi ? v : def;
}
static void vi_init(void) {
    if (it.checked) return;
    it.checked = 1;
    const char *e = getenv("BB_VR_INSTANT_ITEMS");
    it.env_on = e && e[0] == '1';
    it.file_on = -1;
    it.online = vri_online(getenv("BB_ONLINE"), getenv("BB_ONLINE_ROLE"));
    e = getenv("BB_VR_INSTANT_RAY");
    it.ray_on = !(e && e[0] == '0');
    it.cooldown_env = vi_envf("BB_VR_INSTANT_COOLDOWN_S", -1.0f, 0.0f, 10.0f);
    it.chr_r = vi_envf("BB_VR_INSTANT_CHR_R", 0.6f, 0.0f, 3.0f);
    it.spd_ref = vi_envf("BB_VR_THROW_SPEED_REF", 7.0f, 0.5f, 50.0f);
    it.spd_lo = vi_envf("BB_VR_THROW_SPEED_MIN", 0.3f, 0.05f, 1.0f);
    it.spd_hi = vi_envf("BB_VR_THROW_SPEED_MAX", 1.5f, 1.0f, 4.0f);
    it.control = getenv("BB_VR_ARMS_CONTROL");
    if (!it.control || !*it.control) it.control = getenv("BB_VR_FP_CONTROL");
    if (it.control && !*it.control) it.control = NULL;
    it.logged_state = -1;
}
static void vi_poll_control(double now) {
    if (!it.control || now - it.control_checked < 0.25) return;
    it.control_checked = now;
    struct stat st;
    if (stat(it.control, &st) != 0) return;
    if ((long long)st.st_mtime == it.control_mtime && (long long)st.st_size == it.control_size) return;
    it.control_mtime = (long long)st.st_mtime;
    it.control_size = (long long)st.st_size;
    FILE *f = fopen(it.control, "rb");
    if (!f) return;
    char text[4096];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    it.file_on = vri_parse_on(text, "instant_items");
}
int vr_items_on(void) {
    vi_init();
    vi_poll_control(vi_now());
    const int want = it.file_on >= 0 ? it.file_on : it.env_on;
    it.on = want && !it.online;
    if (want != it.logged_state) {
        it.logged_state = want;
        fprintf(stderr, "VR instant items: %s (%s)%s; ray cast %s; cooldown %s\n", it.on ? "ON" : "off",
                    it.file_on >= 0 ? "instant_items= in the control file" : "BB_VR_INSTANT_ITEMS",
                    want && it.online ? ", but online: forced off (single player only)" : "", it.ray_on ? "on" : "off",
                    it.cooldown_env >= 0.0f ? "BB_VR_INSTANT_COOLDOWN_S" : "per use animation");
        fflush(stderr);
    }
    atomic_store(&it_active, it.on);
    return it.on;
}
int vr_items_active(void) { return atomic_load(&it_active); }

/* ---- pad thread ---- */
void vr_items_throw_origin(const int32_t mm[3]) {
    vi_lock();
    memcpy(rel.mm, mm, sizeof(rel.mm));
    rel.have_origin = 1;
    vi_unlock();
}
void vr_items_throw_dir(const int32_t dir_1e4[3]) {
    float o[3], d[3];
    vi_lock();
    const int have = rel.have_origin;
    int32_t mm[3];
    memcpy(mm, rel.mm, sizeof(mm));
    rel.have_origin = 0;
    vi_unlock();
    if (!have) return;
    const int ok = vr_gun_rel_to_world(mm, dir_1e4, o, d);
    vi_lock();
    rel.armed = 1;
    rel.at = vi_now();
    memcpy(rel.dir_1e4, dir_1e4, sizeof(rel.dir_1e4));
    memcpy(rel.mm, mm, sizeof(rel.mm));
    memcpy(rel.origin, o, sizeof(o));
    memcpy(rel.dir, d, sizeof(d));
    rel.world_ok = ok;
    vi_unlock();
    if (!ok) fprintf(stderr, "VR instant items: a release with no first-person camera: the throw goes the game's way\n");
}

int vr_items_take(uint32_t kind, int32_t slot, uint32_t item, int32_t speed_cms) {
    if (!vr_items_on()) return 0;
    if (kind != BBXR_UI_USE_QUICK && kind != BBXR_UI_USE_ITEM) return 0;
    const uint32_t id = kind == BBXR_UI_USE_ITEM ? (uint32_t)slot : item;
    if ((id & 0xF0000000u) != BBXR_ITEM_GOODS) return 0;
    if (atomic_load(&rq.state) != VI_REQ_IDLE) return 0;
    rq.kind = kind;
    rq.slot = slot;
    rq.item = id;
    rq.speed_cms = speed_cms;
    rq.posted = vi_now();
    rq.result = 0;
    rq.reason = 0;
    rq.msg[0] = 0;
    vi_lock();
    rq.had_release = rel.armed && rq.posted - rel.at < 2.0;
    memcpy(rq.mm, rel.mm, sizeof(rq.mm));
    memcpy(rq.dir_1e4, rel.dir_1e4, sizeof(rq.dir_1e4));
    vi_unlock();
    atomic_store(&rq.state, VI_REQ_POSTED);
    return 1;
}

int vr_items_pad_poll(uint32_t *kind, int32_t *slot, uint32_t *item, int32_t *speed_cms) {
    vr_items_on(); /* polls the setting (and keeps the camera recorded for the releases) */
    const int st = atomic_load(&rq.state);
    const double now = vi_now();
    if (st == VI_REQ_POSTED && now - rq.posted > 2.0) {
        int expected = VI_REQ_POSTED;
        if (atomic_compare_exchange_strong(&rq.state, &expected, VI_REQ_DONE)) {
            rq.result = VI_RES_FALLBACK; /* the game-thread hook never ran: the usual path */
            snprintf(rq.msg, sizeof(rq.msg), "the game-thread hook did not run (\"VR FP Camera Heading (in-game step)\" in BB_HOOKS?)");
        }
    }
    if (atomic_load(&rq.state) != VI_REQ_DONE) return 0;
    const int res = rq.result;
    fprintf(stderr, "VR instant items: item %#x: %s%s%s\n", (unsigned)rq.item,
            res == VI_RES_DONE ? "used" : res == VI_RES_REFUSED ? "refused" : "the game's own path", rq.msg[0] ? ": " : "",
            rq.msg);
    fflush(stderr);
    int back = 0;
    if (res == VI_RES_FALLBACK) {
        *kind = rq.kind;
        *slot = rq.slot;
        *item = rq.kind == BBXR_UI_USE_ITEM ? 0 : rq.item;
        *speed_cms = rq.speed_cms;
        if (rq.had_release) { /* the game's throw redirect gets the release it would have had */
            vr_gun_throw_origin(rq.mm);
            vr_gun_throw_dir(rq.dir_1e4);
        }
        back = 1;
    } else {
        bbgpu_vr_ui_event(BBXR_EV_ACTION_DONE, (int32_t)rq.kind, res == VI_RES_DONE, res == VI_RES_DONE ? 0 : rq.reason);
    }
    atomic_store(&rq.state, VI_REQ_IDLE);
    return back;
}

/* ---- game thread ---- */
static int vi_player(uint64_t *player) {
    uint64_t wcm, p, vt, bag, d, dvt;
    int32_t hp = 0;
    if (!vi_ptr(vi_va(VA_WORLD_CHR_MAN), &wcm) || !vi_ptr(wcm + 0x60, &p) || !vi_rd(p, &vt, 8) ||
        vt != vi_va(VA_PLAYER_INS_VTABLE))
        return 0;
    if (!vi_ptr(p + 0x3B0, &bag) || !vi_ptr(bag + 0x20, &d) || !vi_rd(d, &dvt, 8) || dvt != vi_va(VA_CHR_DATA_VTABLE) ||
        !vi_rd(d + 0xF8, &hp, 4) || hp <= 0)
        return 0;
    *player = p;
    return 1;
}
/* The player's feet [[P + 0x3B0] + 0x68] + 0x1E0 (findings/16). */
static int vi_feet(uint64_t player, VriV3 *out) {
    uint64_t bag, m;
    float p[4];
    if (!vi_ptr(player + 0x3B0, &bag) || !vi_ptr(bag + 0x68, &m) || !vi_rd(m + 0x1E0, p, 16) || !isfinite(p[0]) ||
        !isfinite(p[1]) || !isfinite(p[2]))
        return 0;
    *out = vri_v3(p[0], p[1], p[2]);
    return 1;
}
/* A param row through the game's own lookup ({s32 id = -1; pad; row *} as the callers build it). */
static const uint8_t *vi_row(uint32_t lookup, int32_t id, size_t need) {
    _Alignas(16) struct {
        int32_t id, pad;
        uint64_t row;
    } out = {-1, 0, 0};
    ((ViLookupFn)vi_fn(lookup))(&out, id);
    if (!out.row || !vi_readable((uintptr_t)out.row, need)) return NULL;
    return (const uint8_t *)(uintptr_t)out.row;
}
static int32_t vi_count(uint64_t pgd, uint32_t id) {
    static VrUiGameData g; /* game thread only; 4 KB */
    if (!vrui_read(vi_ui_read, NULL, pgd, (uint64_t)(uintptr_t)it.image, 1, &g)) return -1;
    int32_t n = 0;
    for (int i = 0; i < g.item_count; ++i)
        if (g.items[i].id == id) n += g.items[i].count;
    return n;
}
/* PlayerIns::UseGoods' consume (0x1cf97cc..0x1cf9875), for the paths that do not call UseGoods. */
static int vi_consume(uint64_t player, uint64_t pgd, int32_t goods, int32_t *left) {
    const ViBoolFn l1 = (ViBoolFn)vi_slot(player, PLAYER_VT_LOCAL1), l2 = (ViBoolFn)vi_slot(player, PLAYER_VT_LOCAL2);
    if (!l1 || !l2 || (!l1((void *)(uintptr_t)player) && !l2((void *)(uintptr_t)player))) return 0;
    int32_t idx = ((ViQuickIndexFn)vi_fn(VA_QUICK_INDEX))((void *)(uintptr_t)(pgd + 0x3D0), goods);
    if (idx < 0) {
        _Alignas(16) uint32_t id = ((uint32_t)goods & 0x0FFFFFFFu) | 0x40000000u;
        idx = ((ViInvIndexFn)vi_fn(VA_INV_INDEX))((void *)(uintptr_t)(pgd + 0x328), &id);
    }
    if (idx == -1) return 0;
    uint64_t bag, d;
    uint16_t dbg = 0;
    if (vi_ptr(player + 0x3B0, &bag) && vi_ptr(bag + 0x20, &d)) vi_rd(d + 0x200, &dbg, 2);
    uint8_t ran_out = 0;
    *left = -1;
    if (!(dbg & 0x200)) { /* the debug "items are not used up" flag */
        *left = ((ViInvAddFn)vi_fn(VA_INV_ADD))((void *)(uintptr_t)(pgd + 0x1D0), idx, -1, 0, 0, 0);
        ran_out = *left <= 0;
    }
    const int32_t none = -1, zero = 0;
    const uint8_t one = 1;
    uint64_t mm;
    vi_wr(pgd + PGD_USE_REQUEST, &none, 4);
    if (vi_ptr(vi_va(VA_MENU_MAN), &mm)) vi_wr(mm + MENU_USE_FLAG, &zero, 4);
    vi_wr(vi_va(VA_FLAG_RAN_OUT), &ran_out, 1);
    vi_wr(vi_va(VA_FLAG_USED), &one, 1);
    vi_wr(vi_va(VA_FLAG_USED2), &one, 1);
    return 1;
}
/* The four scaling floats UseGoods puts in the request (+0x60..+0x6c) from refVirtualWepId. */
static void vi_scale(uint64_t player, int32_t wep, float out[4]) {
    static const uint32_t slots[4] = {0x298u, 0x2A0u, 0x2A8u, 0x2B0u};
    for (int i = 0; i < 4; ++i) {
        const ViScaleFn f = (ViScaleFn)vi_slot(player, slots[i]);
        out[i] = f ? f((void *)(uintptr_t)player, wep) : 1.0f;
        if (!isfinite(out[i])) out[i] = 1.0f;
    }
}
/* A Bullet row through the game's lookup (VriBulletFn for vri_resolve_flight). */
static int vi_bullet(void *user, int32_t id, VriBullet *out) {
    (void)user;
    const uint8_t *row = id > 0 ? vi_row(VA_BULLET_LOOKUP, id, VRI_BULLET_ROW_MIN) : NULL;
    if (!row) return 0;
    vri_bullet_from_row(id, row, out);
    return 1;
}
/* The active weapons as the quick-item key computes them (0x1a5d119..0x1a5d1d6): PGD+0x250 / +0x24c the active
 * right / left slot, the handles at PGD+0x264 + 4 * (2 slot + 1) (right) / 2 slot (left), each resolved by
 * 0x1e89070 ({u32 handle; u64 0; s32 id = -1} -> id at +0x10, & 0x0fffffff, 0x0fffffff = none); the left is -1
 * when [PGD+0x248] == 3. */
static int32_t vi_weapon_id(uint64_t pgd, int32_t index) {
    if (index < 0 || index > 7) return -1;
    uint32_t handle = 0;
    if (!vi_rd(pgd + 0x264 + 4u * (uint32_t)index, &handle, 4)) return -1;
    _Alignas(16) struct {
        uint32_t handle, pad;
        uint64_t zero;
        int32_t id, pad2;
    } q = {handle, 0, 0, -1, 0};
    ((ViLookupFn)vi_fn(VA_HANDLE_TO_ID))(&q, 0);
    const int32_t id = q.id & 0x0FFFFFFF;
    return id == 0x0FFFFFFF ? -1 : id;
}
static void vi_weapons(uint64_t pgd, int32_t *right, int32_t *left) {
    int32_t ar = 0, al = 0, mode = 0;
    vi_rd(pgd + 0x250, &ar, 4);
    vi_rd(pgd + 0x24C, &al, 4);
    vi_rd(pgd + 0x248, &mode, 4);
    *right = vi_weapon_id(pgd, 2 * ar + 1);
    *left = mode == 3 ? -1 : vi_weapon_id(pgd, 2 * al);
}
/* What a CURE / SPOT / DIRECT use costs: Quicksilver through the game's 0x1cf9f00 (it takes nothing and says 0
 * when goods 900 + 901 do not cover it), then the item through the copy of UseGoods' consume. */
static int vi_pay(uint64_t player, uint64_t pgd, const VriGoods *g, int32_t *left) {
    if (g->bullet_consume > 0 &&
        !((ViPayQsFn)vi_fn(VA_PAY_QUICKSILVER))((void *)(uintptr_t)player, (int32_t)g->bullet_consume))
        return 0;
    if (g->is_consume) return vi_consume(player, pgd, g->goods, left);
    return 1;
}

/* The game's bullet request at `pos` along `dir` (0x1cb6680 with dummy poly -1: our matrix is used). */
static int vi_request(uint64_t player, const VriGoods *g, int32_t bullet, VriV3 pos, VriV3 dir) {
    _Alignas(16) uint8_t br[VRI_BR_SIZE];
    float rows[3][4], scale[4];
    vri_launch_rows(pos, dir, rows);
    vi_scale(player, g->virtual_wep, scale);
    vri_build_request(br, g, bullet, (const float (*)[4])rows, scale);
    return ((ViBehaviorFn)vi_fn(VA_BULLET_BEHAVIOR))(br, (void *)(uintptr_t)(player + PLAYER_EMIT_SLOT),
                                                      (void *)(uintptr_t)player) != 0;
}
/* The game's ray cast (the foot-IK raycast interface, 0x1bfe5a0; filter 0x2f): segment a..b against the map. */
static int vi_ray(void *user, VriV3 a, VriV3 b, VriV3 *hit) {
    (void)user;
    uint64_t h, w0, w, q;
    if (!vi_ptr(vi_va(VA_HAVOK_MAN), &h) || !vi_ptr(h + 0x28, &w0) || !vi_ptr(w0 + 8, &w) || !vi_ptr(w + 0x150, &q) ||
        !vi_readable((uintptr_t)(q + 0x5E8), 8))
        return 0;
    _Alignas(16) float from[4] = {a.x, a.y, a.z, 1.0f}, to[4] = {b.x, b.y, b.z, 1.0f}, pos[4] = {0}, nrm[4] = {0};
    ++ray_calls;
    if (!((ViRayFn)vi_fn(VA_RAY_CAST))(NULL, from, to, pos, nrm)) return 0;
    const VriV3 p = vri_v3(pos[0], pos[1], pos[2]);
    /* trust a hit only on (about) the segment */
    const VriV3 d = vri_sub(b, a);
    const float dd = vri_dot(d, d);
    const float t = dd > 1e-12f ? vri_dot(vri_sub(p, a), d) / dd : 0.0f;
    const VriV3 on = vri_add(a, vri_mul(d, t < 0 ? 0 : t > 1 ? 1 : t));
    if (!vri_finite(p) || vri_len(vri_sub(p, on)) > 0.25f) {
        if (++ray_bad <= 3)
            fprintf(stderr, "VR instant items: ray cast hit (%.2f %.2f %.2f) is not on the segment (%.2f %.2f %.2f)-(%.2f %.2f %.2f): ignored\n",
                    (double)p.x, (double)p.y, (double)p.z, (double)a.x, (double)a.y, (double)a.z, (double)b.x, (double)b.y,
                    (double)b.z);
        return 0;
    }
    if (++ray_hits == 1)
        fprintf(stderr, "VR instant items: the game's ray cast 0x1bfe5a0 hit the map at (%.2f %.2f %.2f), normal (%.2f %.2f %.2f) (%d calls so far)\n",
                (double)p.x, (double)p.y, (double)p.z, (double)nrm[0], (double)nrm[1], (double)nrm[2], ray_calls);
    *hit = p;
    return 1;
}

static void vi_answer(int result, int reason, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void vi_answer(int result, int reason, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rq.msg, sizeof(rq.msg), fmt, ap);
    va_end(ap);
    rq.result = result;
    rq.reason = reason;
    atomic_store(&rq.state, VI_REQ_DONE);
}

static void vi_run_request(double now) {
    int expected = VI_REQ_POSTED;
    if (!atomic_compare_exchange_strong(&rq.state, &expected, VI_REQ_RUNNING)) return;
    const int32_t goods = (int32_t)(rq.item & 0x0FFFFFFFu);
    uint64_t player = 0, pgd = 0, gdm = 0, pgd2 = 0, mm = 0;
    int32_t page = 0, use_state = 0;
    if (!vi_player(&player)) { vi_answer(VI_RES_REFUSED, 3, "no living player"); return; }
    vi_rd(player + PLAYER_USE_STATE, &use_state, 4);
    if (use_state != 0) { vi_answer(VI_RES_REFUSED, 3, "an item use is running (PlayerIns+0x490 = %d)", (int)use_state); return; }
    if (!vi_ptr(vi_va(VA_MENU_MAN), &mm) || !vi_rd(mm + MENU_PAGE, &page, 4) || page != 0) {
        vi_answer(VI_RES_REFUSED, 6, "a game menu is open");
        return;
    }
    const ViPtrFn pgd_fn = (ViPtrFn)vi_slot(player, PLAYER_VT_PGD);
    pgd = pgd_fn ? (uint64_t)(uintptr_t)pgd_fn((void *)(uintptr_t)player) : 0;
    if (!pgd || !vi_ptr(vi_va(VRUI_VA_GAME_DATA_MAN), &gdm) || !vi_ptr(gdm + 8, &pgd2) || pgd != pgd2) {
        vi_answer(VI_RES_FALLBACK, 0, "PlayerGameData mismatch (slot 0x1c8 %#llx, GameDataMan %#llx)", (unsigned long long)pgd,
                  (unsigned long long)pgd2);
        return;
    }
    const uint8_t *grow = vi_row(VA_GOODS_LOOKUP, goods, VRI_GOODS_ROW_MIN);
    if (!grow) { vi_answer(VI_RES_FALLBACK, 0, "no EquipParamGoods row %d", (int)goods); return; }
    VriGoods g;
    vri_goods_from_row(goods, grow, &g);
    VriBullet b, own;
    const VriBullet *bp = NULL;
    int via_spawner = 0;
    if (g.ref_category == 1) {
        const int32_t off = ((ViReplaceFn)vi_fn(VA_REPLACE_OFFSET))((void *)(uintptr_t)player, g.replace_category);
        const int32_t bid = g.ref_id + (off > 0 && off < 16 ? off : 0);
        if (vi_bullet(NULL, bid, &own)) {
            b = vri_resolve_flight(&own, vi_bullet, NULL, &via_spawner); /* 110 -> 111: the bullet that flies */
            if (via_spawner) vri_spawner_flight(&b);
            bp = &b;
        }
    }
    /* the release that came with this use */
    float o[3] = {0}, d[3] = {0};
    vi_lock();
    const int thrown = rel.armed && rel.world_ok && now - rel.at < 2.0;
    memcpy(o, rel.origin, sizeof(o));
    memcpy(d, rel.dir, sizeof(d));
    rel.armed = 0;
    vi_unlock();
    const char *why = "";
    const int kind = vri_classify(&g, bp, thrown, &why);
    if (kind == VRI_KIND_NONE) { vi_answer(VI_RES_FALLBACK, 0, "goods %d: %s", (int)goods, why); return; }
    const float cd = it.cooldown_env >= 0.0f ? it.cooldown_env : vri_cooldown_s(g.use_anim, kind);
    if (!vri_cooldown_ready(&cool, now)) {
        vi_answer(VI_RES_REFUSED, 3, "goods %d: cooldown, %.2f s left (vanilla animation length, BB_VR_INSTANT_COOLDOWN_S)",
                  (int)goods, cool.until - now);
        return;
    }
    /* the game's own "can this item be used now", with exactly the arguments the quick-item key gives it (0x1a5d241,
     * the square press the old path makes): (goods, player, [PGD+0x670], [PGD+0xa4], the active right weapon, the
     * active left weapon or -1 when [PGD+0x248] == 3, 0). The weapons matter: the papers check the buffed weapon. */
    {
        uint64_t sp = 0;
        int32_t team = 0, wr = -1, wl = -1;
        vi_rd(pgd + 0x670, &sp, 8);
        vi_rd(pgd + 0xA4, &team, 4);
        vi_weapons(pgd, &wr, &wl);
        if (!((ViCanUseFn)vi_fn(VA_CAN_USE_GOODS))(goods, (void *)(uintptr_t)player, (void *)(uintptr_t)sp, team, wr, wl, 0)) {
            vi_answer(VI_RES_REFUSED, 3, "goods %d: the game says it cannot be used now (0x197f200; weapons %#x / %#x)",
                      (int)goods, (unsigned)wr, (unsigned)wl);
            return;
        }
    }
    const uint32_t gid = BBXR_ITEM_GOODS | (uint32_t)goods;
    const int32_t before = vi_count(pgd, gid);
    if (before <= 0) { vi_answer(VI_RES_REFUSED, 1, "goods %d: none held (%d)", (int)goods, (int)before); return; }
    if (kind == VRI_KIND_CURE || kind == VRI_KIND_SPOT || kind == VRI_KIND_DIRECT) {
        int32_t emit = -1;
        vi_rd(player + PLAYER_EMIT_SLOT + 8, &emit, 4);
        if (emit != -1) { /* 0x1cb7250 would refuse the request: nothing is paid */
            vi_answer(VI_RES_REFUSED, 3, "goods %d: the bullet emit slot is busy (a weapon bullet, handle %#x)", (int)goods,
                      (unsigned)emit);
            return;
        }
    }
    const double t0 = vi_now();
    if (kind == VRI_KIND_USE) {
        const ViUseGoodsFn use = (ViUseGoodsFn)vi_slot(player, PLAYER_VT_USE_GOODS);
        if (!use) { vi_answer(VI_RES_FALLBACK, 0, "no UseGoods slot"); return; }
        const int r = use((void *)(uintptr_t)player, goods, -1);
        const int32_t after = vi_count(pgd, gid);
        vri_cooldown_arm(&cool, now, cd);
        vi_answer(r ? VI_RES_DONE : VI_RES_REFUSED, 5,
                  "goods %d (SpEffect %d): UseGoods (slot 0x408, dummy -1) returned %d in %.2f ms; count %d -> %d; cooldown %.1f s",
                  (int)goods, (int)g.ref_id, r, (vi_now() - t0) * 1e3, (int)before, (int)after, (double)cd);
        return;
    }
    if (kind == VRI_KIND_CURE || kind == VRI_KIND_SPOT || kind == VRI_KIND_DIRECT) {
        VriV3 at, dir = vri_v3(0, 0, 1);
        const char *where = "at the chest";
        if (kind == VRI_KIND_DIRECT) { /* the tool's own bullet from the hand along the release: the game flies it */
            at = vri_v3(o[0], o[1], o[2]);
            dir = vri_v3(d[0], d[1], d[2]);
            where = "from the hand";
        } else {
            VriV3 feet;
            if (!vi_feet(player, &feet)) { vi_answer(VI_RES_REFUSED, 3, "goods %d: no player position", (int)goods); return; }
            at = vri_add(feet, vri_v3(0, 1.2f, 0));
            float rows[4][4];
            if (kind == VRI_KIND_SPOT && bp->atk_id > 0 && vr_gun_view_latest(rows, NULL)) {
                /* an attack made where it is (Shaman Bone Blade): 0.7 m in front of the chest, along the view */
                const VriV3 fwd = vri_norm(vri_v3(rows[2][0], 0.0f, rows[2][2]), vri_v3(0, 0, 1));
                at = vri_add(at, vri_mul(fwd, 0.7f));
                dir = fwd;
                where = "in front of the chest";
            }
        }
        /* pay first (Quicksilver checks that there is enough and takes nothing when there is not) */
        int32_t left = -1;
        if (!vi_pay(player, pgd, &g, &left)) {
            vi_answer(VI_RES_REFUSED, 3, "goods %d: not enough Quicksilver (%d needed) or not in the inventory", (int)goods,
                      (int)g.bullet_consume);
            return;
        }
        const int ok = vi_request(player, &g, bp->id, at, dir);
        const int32_t after = vi_count(pgd, gid);
        vri_cooldown_arm(&cool, now, cd);
        vi_answer(ok ? VI_RES_DONE : VI_RES_REFUSED, 5,
                  "goods %d: %s bullet %d (atk %d, shooter SpEffect %d) %s (%.2f %.2f %.2f) along (%.2f %.2f %.2f): request %s; paid %s%d; count %d -> %d",
                  (int)goods, kind == VRI_KIND_CURE ? "cure" : kind == VRI_KIND_SPOT ? "spot" : "direct", (int)bp->id,
                  (int)bp->atk_id, (int)bp->sp_shooter, where, (double)at.x, (double)at.y, (double)at.z, (double)dir.x,
                  (double)dir.y, (double)dir.z, ok ? "accepted" : "REFUSED by 0x1cb6680 (the cost is spent)",
                  g.bullet_consume ? "Quicksilver x" : "the item, left ", g.bullet_consume ? (int)g.bullet_consume : (int)left,
                  (int)before, (int)after);
        return;
    }
    /* VRI_KIND_THROW */
    int slot = -1;
    for (int i = 0; i < VI_FLIGHTS && slot < 0; ++i)
        if (!fl[i].active) slot = i;
    if (slot < 0) { vi_answer(VI_RES_REFUSED, 3, "goods %d: %d items already in the air", (int)goods, VI_FLIGHTS); return; }
    int32_t left = -1;
    if (!vi_consume(player, pgd, goods, &left)) {
        vi_answer(VI_RES_REFUSED, 1, "goods %d: not found in the inventory for the consume", (int)goods);
        return;
    }
    const float hand = rq.speed_cms > 0 ? (float)rq.speed_cms * 0.01f : 0.0f;
    const float speed = vri_throw_speed(bp, hand, it.spd_ref, it.spd_lo, it.spd_hi);
    ViFlight *f = &fl[slot];
    memset(f, 0, sizeof(*f));
    f->active = 1;
    f->b = *bp;
    f->g = g;
    f->started = now;
    vri_flight_start(&f->f, vri_v3(o[0], o[1], o[2]), vri_v3(d[0], d[1], d[2]), speed);
    const int32_t after = vi_count(pgd, gid);
    vri_cooldown_arm(&cool, now, cd);
    bbgpu_vr_ui_event(BBXR_EV_INSTANT_THROW, goods, (int32_t)lroundf(speed * 100.0f), 1);
    vi_answer(VI_RES_DONE, 0,
              "goods %d: thrown, bullet %d%s (burst %d) at %.1f m/s (hand %.1f m/s, initVellocity %.1f) from (%.2f %.2f %.2f) along (%.2f %.2f %.2f); count %d -> %d",
              (int)goods, (int)bp->id, via_spawner ? " (the flyer of its spawner, the Molotov's flight)" : "", (int)bp->hit_bullet, (double)speed, (double)hand, (double)bp->init_v, (double)o[0],
              (double)o[1], (double)o[2], (double)d[0], (double)d[1], (double)d[2], (int)before, (int)after);
}

static const char *vi_state_name(int s) {
    return s == VRI_HIT_WORLD ? "hit the map" : s == VRI_HIT_CHR ? "hit a character" : s == VRI_HIT_FLOOR ? "reached the floor plane"
           : s == VRI_EXPIRED ? "expired in the air" : "flying";
}

static void vi_step_flights(double now, float dt) {
    uint64_t player = 0;
    const int have_player = vi_player(&player);
    VriV3 feet = vri_v3(0, -1e9f, 0);
    if (have_player) vi_feet(player, &feet);
    float pts[16][3];
    const int n = have_player ? vr_gun_candidate_points(pts, 16) : 0;
    VriV3 spheres[16];
    for (int i = 0; i < n; ++i) spheres[i] = vri_v3(pts[i][0], pts[i][1], pts[i][2]);
    /* the floor plane: at the feet until the ray cast has proved it hits the map, then far below */
    const float floor_y = feet.y - (ray_hits > 0 ? 30.0f : 0.05f);
    for (int i = 0; i < VI_FLIGHTS; ++i) {
        ViFlight *f = &fl[i];
        if (!f->active) continue;
        if (!have_player) { f->active = 0; continue; } /* a load or a death: drop it */
        if (f->f.state == VRI_FLYING) {
            const int st = vri_flight_step(&f->f, &f->b, dt, it.ray_on ? vi_ray : NULL, NULL, spheres, n, it.chr_r, floor_y);
            if (st == VRI_FLYING) continue;
            f->spawn_id = vri_impact_plan(&f->f, &f->b, 0.05f, 0.5f, &f->spawn_pos, &f->spawn_dir);
            const float flight_ms = f->f.t * 1000.0f;
            bbgpu_vr_ui_event(BBXR_EV_INSTANT_LAND, f->g.goods, (int32_t)lroundf(flight_ms), st);
            fprintf(stderr, "VR instant items: goods %d bullet %d %s after %.2f s at (%.2f %.2f %.2f), %.1f m from the hand%s\n",
                    (int)f->g.goods, (int)f->b.id, vi_state_name(st), (double)f->f.t, (double)f->f.impact.x,
                    (double)f->f.impact.y, (double)f->f.impact.z, (double)vri_len(vri_sub(f->f.impact, f->f.origin)),
                    f->spawn_id ? "" : ": nothing requested (the game's bullet would expire the same way)");
            if (!f->spawn_id) { f->active = 0; continue; }
        }
        if (vi_request(player, &f->g, f->spawn_id, f->spawn_pos, f->spawn_dir)) {
            fprintf(stderr, "VR instant items: goods %d: bullet %d requested at (%.2f %.2f %.2f) along (%.2f %.2f %.2f)%s\n",
                    (int)f->g.goods, (int)f->spawn_id, (double)f->spawn_pos.x, (double)f->spawn_pos.y, (double)f->spawn_pos.z,
                    (double)f->spawn_dir.x, (double)f->spawn_dir.y, (double)f->spawn_dir.z,
                    f->tries ? " (after the emit slot freed)" : "");
            f->active = 0;
        } else if (++f->tries >= VI_SPAWN_TRIES) {
            fprintf(stderr, "VR instant items: goods %d: the bullet request 0x1cb6680 refused bullet %d %d times: given up (the item is spent)\n",
                    (int)f->g.goods, (int)f->spawn_id, f->tries);
            f->active = 0;
        }
    }
    (void)now;
}

void vr_items_game_step(unsigned char *image) {
    if (!image) return;
    it.image = image;
    const double now = vi_now();
    float dt = last_step > 0.0 ? (float)(now - last_step) : 0.0f;
    last_step = now;
    if (!(dt >= 0.0f)) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    if (atomic_load(&rq.state) == VI_REQ_POSTED) vi_run_request(now);
    int any = 0;
    for (int i = 0; i < VI_FLIGHTS; ++i) any |= fl[i].active;
    if (any) vi_step_flights(now, dt);
    fflush(stderr);
}
