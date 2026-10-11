/* The game side of the VR UI (BB_VR_UI=1, docs/vr/findings/26-game-ui.md). The headset menus and the
 * swing/stamina HUD live in bbport_xr.dll (gpu/xr_bridge/vr_game_ui.h, the sandbox's code); this file
 * feeds them the game and carries out what the user picks:
 *   - every pad read (runtime_pad.c, the game's input thread) it reads the player's stamina, quick
 *     slots, weapon slots and (four times a second) every inventory entry, read only, through the
 *     readable-memory guard (src/vr_ui_game.h has the layout), and hands them to the bridge;
 *   - it forwards the "misses are free" stamina tickets (src/runtime_vr_stamina.c) as swing events;
 *   - it takes the user's choices from the bridge and presses the game's own buttons for them
 *     (src/vr_ui_game.h's executor: d-pad down to the quick slot, then square; d-pad right/left to a
 *     weapon slot). The presses are added to the pad state the game reads; nothing is written to
 *     game memory.
 * Environment:
 *   BB_VR_UI=1         on (default off: nothing is read, nothing pressed); the bridge needs it too
 *   BB_VR_UI_PROBE=1   logs what is read: the PlayerGameData chain and its vtables, the quick slots
 *                      and the selection, the weapon slots, vials and bullets; every 2 s while it
 *                      changes, and every selection change (the findings/26 live check)
 *   BB_VR_UI_PRESS_S   how long each press is held and released (0.06 s)
 *   BB_DEBUG_GIVE      "goods:<id>[xN];...;insight:+N": gives those once, from the in-game step hook, after a
 *                      living character is loaded (co-op tests without playing for the bells); runs the
 *                      game's own add-item routine 0x1f5c8c0; works without BB_VR_UI
 *                      "weapon:<id>[xN]" gives a weapon (plain EquipParamWeapon id, category 0)
 *   BB_DEBUG_GIVE_FILE a file with one entry per line ("goods:..", "weapon:..", "insight:+N", # comments), no
 *                      length limit; one item is given per hook call; an empty value is ignored
 *   BB_DEBUG_USE       goods:<id>: uses that item once the character is in the world (the quick-slot
 *                      path of the dashboard: 0x1f63e00 into a quick slot, d-pad down, square)
 *   BB_DEBUG_USE_FILE  a control file polled twice a second: write "goods:205" (or just 205) into it to
 *                      use that item mid-game; the file is emptied when taken (default out/debug_use.txt)
 *   BB_ONLINE_BELL_DIRECT  0: the room auto-bell uses only the old quick-slot pad path (default: the direct item use, then
 *                      one square press, then the pad path; see vu_online_bell)
 * Both debug features need the patch "VR FP Camera Heading (in-game step)" in BB_HOOKS.
 * Works with BB_VR=0 too (flat tests: the probe and the pad presses, no headset). */
#define _GNU_SOURCE
#include "runtime.h"
#include "hooks.h"
#include "gpu/bbgpu.h"
#include "gpu/xr_bridge/bbport_xr_bridge.h"
#include "vr_ui_game.h"
#include "vr_hitbox.h"
#include "vr_skin.h"
#include "vr_gun.h"
#include "vr_fp.h"
#include "vr_free_bells.h"
/* Public builds: only the room features run here (the auto-bell's press, a missing bell given, free bells). The
 * general debug give / use tools stay out; the give plan is one bell at most. */
#define VRDG_MAX_INSIGHT 99999
typedef struct { uint32_t id; int32_t count; } VrDgItem;
typedef struct { int n; VrDgItem items[1]; int32_t insight, insight_min; int bad; } VrDgPlan;
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define VA_BASE 0x400000u
#define VA_WORLD_CHR_MAN 0x593E878u
#define VA_PLAYER_INS_VTABLE 0x578F310u
#define VA_CHR_DATA_VTABLE 0x5735810u
#define VA_MENU_MAN 0x5962878u           /* the menu manager (findings/26): +0x3c the pause menu page */
#define VA_EQUIP_FROM_MENU 0x1f63e00u    /* (edi = equip slot, rsi = record {+0x48 inv idx, +0x4c id}) */
#define VA_ADD_ITEM 0x1f5c8c0u           /* bool add(const u32 *itemId, int count): the pickup / messenger gift routine */
#define PLAYER_PENDING_USE 0x394u        /* PlayerIns +0x394: the pending use-item id (read at 0x18d0822) */
#define PLAYER_VT_APPLY_SPEFFECT 0x3F0u  /* PlayerIns vtable slot: apply SpEffect (the Co-op Quick-search 0x200d3b0 calls it) */
#define PLAYER_PGD_PTR 0x290u            /* PlayerIns +0x290: its PlayerGameData */
#define PGD_INSIGHT 0x84u                /* PlayerGameData +0x84: Insight (s32), of [[GameDataMan]+8] (findings/26): the shop price code
                                          * subtracts it (0x1f63c39), the Beckoning Bell predicate 0x1731d00 tests it <= 0, 0x1738670 returns it.
                                          * (+0x48 is a stat, +0x94 the Blood Echoes.) */
#define VA_CAN_USE_ITEM 0x1ae38d0u      /* bool f(void): the game's "an item can be used now" predicate (the item menu calls it first) */
#define PLAYER_USE_ITEM_ID 0x478u        /* PlayerIns +0x478: the use-item id GetUseItemId (0x18d1ef0) returned, stored at 0x1cf754c */
#define PLAYER_USE_STATE 0x490u          /* PlayerIns +0x490: item-use state (0 idle; 2/3/5 while a use runs) */
#define PGD_USE_REQUEST 0x458u           /* PGD +0x458: m_useRequestItemFromMenu (logged only, never written) */
#define PGD_STAT48 0x48u                 /* PlayerGameData +0x48: a stat; logged only, to show what [PlayerIns+0x290] is */

static struct {
    int checked, on, probe;
    unsigned char *image; /* guest VA 0x400000 */
    double press_s;
    double items_at;
    VrUiGameData game;
    VrUiExec exec;
    int exec_reported;
    int then_use; /* a quick slot to use once its assignment is done, else -1 */
    BbXrGameState state;
    /* probe */
    double probe_at;
    int32_t probe_selected, probe_right, probe_left;
    int probe_ok;
    /* debug give / use */
    int dbg;              /* BB_DEBUG_GIVE or BB_DEBUG_USE or BB_DEBUG_USE_FILE is set, or a room feature is on */
    VrDgPlan give;
    int give_state;       /* 0 off, 1 waiting for a loaded character, 2 done */
    int give_i;           /* next item to give (one per hook call) */
    int give_stable;      /* consecutive hook calls with a living player and a readable inventory */
    uint32_t use_pending; /* a use waiting for its item to be there */
    double use_since, use_gate_at;
    int32_t pend_before;  /* player+0x394 (the pending use-item id) at the debug press */
    double pend_until;    /* watch it until then */
    int pend_seen;
    uint32_t chk_id;      /* debug use result check: the item used, */
    double chk_at;        /* when to look, */
    int32_t chk_count, chk_insight; /* and what they were at the press */
} vu;
static int vu_direct_enabled = 1; /* BB_ONLINE_BELL_DIRECT != 0 */
static int vu_free_bells; /* Free co-op bells (src/vr_free_bells.h) */
static VfbState vu_fb;
/* A bell press is starting (any thread): arms the refund window. */
static void vu_fb_arm(double now) {
    if (vu_free_bells) vfb_arm(&vu_fb, now);
}

static double vu_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ---- guest memory: read only what is mapped (the chains dangle during loads); the guard of
 * src/runtime_vr_stamina.c / runtime_vr_fp.c (a copy, as each VR file keeps its own) ---- */
#ifdef _WIN32
static struct { uintptr_t lo, hi; } vu_regions[32];
static int vu_region_count;
static double vu_regions_time;
static int vu_readable(uintptr_t a, size_t n) {
    if (a < 0x10000 || a + n < a) return 0;
    const double now = vu_now();
    if (now - vu_regions_time > 1.0) { vu_region_count = 0; vu_regions_time = now; }
    for (int i = 0; i < vu_region_count; ++i)
        if (vu_regions[i].lo <= a && a + n <= vu_regions[i].hi) return 1;
    uintptr_t at = a;
    while (at < a + n) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void *)at, &mbi, sizeof(mbi))) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return 0;
        const uintptr_t lo = (uintptr_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
        if (vu_region_count < (int)(sizeof(vu_regions) / sizeof(*vu_regions))) {
            vu_regions[vu_region_count].lo = lo;
            vu_regions[vu_region_count].hi = hi;
            ++vu_region_count;
        }
        at = hi;
    }
    return 1;
}
#else
#include <unistd.h>
static int vu_readable(uintptr_t a, size_t n) {
    static int fds[2] = {-1, -1};
    if (a < 0x10000 || a + n < a) return 0;
    if (fds[0] < 0 && pipe(fds) != 0) return 0;
    const ssize_t w = write(fds[1], (const void *)a, n > 4096 ? 4096 : n);
    if (w < 0) return 0;
    char sink[4096];
    if (read(fds[0], sink, (size_t)w) < 0) return 0;
    return 1;
}
#endif

static int vu_read(void *user, uint64_t addr, void *out, size_t n) {
    (void)user;
    if (!vu_readable((uintptr_t)addr, n)) return 0;
    memcpy(out, (const void *)(uintptr_t)addr, n);
    return 1;
}
static int vu_ptr(uint64_t a, uint64_t *out) {
    uint64_t v;
    if (!vu_read(NULL, a, &v, 8) || v < 0x10000 || (v & 7)) return 0;
    *out = v;
    return 1;
}
static uint64_t vu_va(uint32_t v) { return (uint64_t)(uintptr_t)(vu.image + (v - VA_BASE)); }

/* The game's "a menu is up" bitmask 0x1b6b7e0, from the menu manager's own fields f = [0x5962878]
 * (+0..0x2FC; its global bytes: vu_menu_mask_globals). The game drops pad input when
 * mask & 0x7ffff77f (0x1705fc9). Live: closed 0; the Options menu sets +0x54 (2, 7 in a sub-page). */
static uint32_t vu_menu_mask(const uint32_t *f) {
    static const struct { uint16_t off; uint32_t bit; } bits[] = {
        {0x3c, 0x200}, {0x40, 0x1}, {0x7c, 0x4}, {0xb0, 0x800}, {0x100, 0x40000000}, {0xb8, 0x20},
        {0xb4, 0x10}, {0xa0, 0x400}, {0x54, 0x40}, {0x104, 0x100000}, {0x60, 0x1000}, {0x64, 0x2000},
        {0x68, 0x4000}, {0x134, 0x2}, {0x114, 0x2000000}, {0x138, 0x8000}, {0xc4, 0x10000},
        {0xd4, 0x40000}, {0x88, 0x400000}, {0x90, 0x200000}, {0x98, 0x800000}, {0x9c, 0x1000000},
        {0x2dc, 0x100}};
    uint32_t m = 0;
    for (size_t i = 0; i < sizeof bits / sizeof bits[0]; ++i)
        if (f[bits[i].off / 4]) m |= bits[i].bit;
    if (f[0x78 / 4]) m |= 0x8;
    if (f[0xdc / 4] || f[0x80 / 4] || f[0x120 / 4] || f[0x2f8 / 4]) m |= 0x8000000;
    if (f[0x58 / 4] == 4) m |= 0x80;
    else if (f[0x58 / 4] >= 1 && f[0x58 / 4] <= 3) m |= 0x80000;
    if (f[0xf0 / 4]) m |= 0x10000000;
    return m;
}

/* The bits 0x1b6b7e0 takes from globals beside the menu manager: the level-up / shop / dialog
 * screens set these rather than the manager's own fields (live: the Options menu sets 0x800008). */
static uint32_t vu_menu_mask_globals(const uint32_t *f) {
    uint8_t b = 0;
    uint32_t m = 0;
    if (vu_read(NULL, vu_va(0x593b1bau), &b, 1) && b) m |= 0x8;
    if (vu_read(NULL, vu_va(0x5960ff0u), &b, 1) && b) m |= 0x8000000;
    if (f[0x2f4 / 4] && vu_read(NULL, vu_va(0x5960e72u), &b, 1) && b) m |= 0x8000000;
    if (vu_read(NULL, vu_va(0x5961424u), &b, 1) && (b & 1)) m |= 0x10000000;
    uint64_t o, v = 0;
    if (vu_read(NULL, vu_va(0x5962882u), &b, 1) && b && vu_ptr(vu_va(0x5962870u), &o) &&
        vu_read(NULL, o + 0xa88, &v, 8) && v)
        m |= 0x800008;
    return m;
}

static void vu_init(void) {
    if (vu.checked) return;
    vu.checked = 1;
    const char *e = getenv("BB_VR_UI");
    vu.on = e && e[0] == '1';
    e = getenv("BB_VR_UI_PROBE");
    vu.probe = e && e[0] == '1';
    vu.press_s = 0.06; /* two frames at 30 fps: the game takes it, and a cycle of 6 slots stays short */
    e = getenv("BB_VR_UI_PRESS_S");
    if (e && *e) {
        const double v = strtod(e, NULL);
        if (isfinite(v) && v >= 0.04 && v <= 0.5) vu.press_s = v;
    }
    vu.probe_selected = vu.probe_right = vu.probe_left = -99;
    vu.then_use = -1;
    e = getenv("BB_ONLINE_BELL_DIRECT");
    vu_direct_enabled = !(e && e[0] == '0');
    if (runtime_online_bell_armed()) vu.dbg = 1; /* the auto-bell rings through the use path */
    {
        /* Free co-op bells: BB_ONLINE_FREE_BELLS=1, or on by default in a room (BB_ONLINE_ROLE set) unless it is 0. */
        const char *fb = getenv("BB_ONLINE_FREE_BELLS"), *role = getenv("BB_ONLINE_ROLE");
        vu_free_bells = fb && *fb ? fb[0] == '1' : (role && *role);
        if (vu_free_bells) {
            vu.dbg = 1;
            vu_fb.floor_always = role && *role; /* in a room: Insight 0 -> 1 even before a press is seen */
            fprintf(stderr, "Free bells: on (Insight kept at 1+, a bell's Insight cost given back; params carry no cost for rows 205/225)\n");
            fflush(stderr);
        }
    }
    if (vu.on || vu.probe) {
        fprintf(stderr, "VR UI: game side %s (BB_VR_UI=%d), probe %d, presses %.2f s\n", vu.on ? "on" : "off", vu.on,
                vu.probe, vu.press_s);
        fflush(stderr);
    }
}

/* ---- Equipping through the game's own command (hook "bb_hook_vr_ui_menu", findings/26) ----
 * The pad thread posts a request; the game thread runs it at the entry of the HUD input handler
 * 0x1a5b970, the frame phase where the game itself acts on menu input: it checks the player, the menu
 * manager and the inventory entry once more, calls the equip-from-menu command 0x1f63e00 exactly as the
 * game's equipment and item menus do (slot, a record with the inventory index and the item id), and
 * reads back what the slot holds. 0x1f63e00 clears the item from another slot it was in, writes the
 * slot through the game's setter, re-checks the quick item and tells the player (and an online
 * session) itself. Nothing else is written. */
typedef void (ABI *VuEquipFn)(uint32_t slot, void *record);
static struct {
    int state;            /* 0 idle, 1 requested (pad thread), 3 running, 2 done (game thread) */
    uint32_t slot, id;
    int32_t inv;
    int ok, reason;
    uint32_t before, after;
    double requested;
} vq;
static volatile uint64_t vu_hook_us; /* when the game thread last ran the hook (us), 0 never */

static int vu_hp(uint64_t player) {
    uint64_t bag, d, vt;
    int32_t hp = 0;
    if (vu_ptr(player + 0x3B0, &bag) && vu_ptr(bag + 0x20, &d) && vu_read(NULL, d, &vt, 8) &&
        vt == vu_va(VA_CHR_DATA_VTABLE))
        vu_read(NULL, d + 0xF8, &hp, 4);
    return hp;
}

/* ---- BB_DEBUG_GIVE (game thread; public builds: only a bell, from the auto-bell): the game's own add-item routine 0x1f5c8c0(const u32 *itemId, int count),
 * the one the pickup code (0x20187d4) and the messenger gifts use, then Insight at [[GameDataMan]+8]+0x84.
 * Waits until a living player, a valid PlayerGameData (three vtables) and no game menu have held for
 * about two seconds of gameplay (120 hook calls), then runs once. */
typedef unsigned char (ABI *VuAddItemFn)(const uint32_t *id, int32_t count);

static int32_t vu_count_of(const VrUiGameData *g, uint32_t id) {
    int32_t n = 0;
    for (int i = 0; i < g->item_count; ++i)
        if (g->items[i].id == id) n += g->items[i].count;
    return n;
}

static void vu_debug_give_step(void) {
    if (vu.give_state == 2) {
        /* one item per hook call (the game's own add-item may queue UI/notifications) */
        uint64_t gdm, pgd;
        VrUiGameData g;
        if (!vu_ptr(vu_va(VRUI_VA_GAME_DATA_MAN), &gdm) || !vu_ptr(gdm + 8, &pgd)) return;
        if (vu.give_i >= vu.give.n) {
            vu.give_state = 3;
            fprintf(stderr, "Debug give: %d item(s) done\n", vu.give.n);
            fflush(stderr);
            return;
        }
        const int i = vu.give_i++;
        const uint32_t id = vu.give.items[i].id;
        const int32_t want = vu.give.items[i].count;
        if (!vfb_is_bell(id) || want != 1) return; /* public builds give only a bell, once */
        const int32_t before = vrui_read(vu_read, NULL, pgd, (uint64_t)(uintptr_t)vu.image, 1, &g) ? vu_count_of(&g, id) : -1;
        _Alignas(16) uint32_t idv = id;
        const unsigned char r = ((VuAddItemFn)(void *)vu_va(VA_ADD_ITEM))(&idv, want);
        const int32_t after = vrui_read(vu_read, NULL, pgd, (uint64_t)(uintptr_t)vu.image, 1, &g) ? vu_count_of(&g, id) : -1;
        fprintf(stderr, "Debug give: [%d/%d] item %#x x%d: add-item returned %d; held %d -> %d%s\n", i + 1, vu.give.n, id, want,
                (int)r, before, after, after >= before + want ? "" : " (not all of it: the game refused, or the inventory is full)");
        fflush(stderr);
        return;
    }
    if (vu.give_state != 1) return;
    uint64_t wcm, player, vt, gdm, pgd, mm;
    VrUiGameData g;
    int32_t page = 0;
    if (!vu_ptr(vu_va(VA_WORLD_CHR_MAN), &wcm) || !vu_ptr(wcm + 0x60, &player) || !vu_read(NULL, player, &vt, 8) ||
        vt != vu_va(VA_PLAYER_INS_VTABLE) || vu_hp(player) <= 0 || !vu_ptr(vu_va(VRUI_VA_GAME_DATA_MAN), &gdm) ||
        !vu_ptr(gdm + 8, &pgd) || !vrui_read(vu_read, NULL, pgd, (uint64_t)(uintptr_t)vu.image, 1, &g) || g.tail < 0 ||
        !vu_ptr(vu_va(VA_MENU_MAN), &mm) || !vu_read(NULL, mm + 0x3C, &page, 4) || page != 0) {
        vu.give_stable = 0;
        return;
    }
    if (++vu.give_stable < 120) return;
    vu.give_state = 2;
    fprintf(stderr, "Debug give: character loaded (PlayerGameData %#llx, %d items); giving with the game's add-item 0x%x\n",
            (unsigned long long)pgd, g.item_count, VA_ADD_ITEM);
    {
        /* Insight lives in [[GameDataMan]+8] (= pgd, the object the game reads: shops, bells, HUD) at +0x84.
         * [PlayerIns+0x290] is logged for comparison only; it is not written. */
        uint64_t other = 0;
        int32_t cur = 0, o_ins = -1, o_48 = -1, g_48 = -1;
        const int have_other = vu_ptr(player + PLAYER_PGD_PTR, &other);
        vu_read(NULL, pgd + PGD_STAT48, &g_48, 4);
        if (have_other) {
            vu_read(NULL, other + PGD_INSIGHT, &o_ins, 4);
            vu_read(NULL, other + PGD_STAT48, &o_48, 4);
        }
        fprintf(stderr, "Debug give: before: [[GameDataMan]+8]=%#llx insight(+%#x)=%d stat(+%#x)=%d; [PlayerIns+%#x]=%#llx insight(+%#x)=%d stat(+%#x)=%d%s\n",
                (unsigned long long)pgd, PGD_INSIGHT, vu_read(NULL, pgd + PGD_INSIGHT, &cur, 4) ? (int)cur : -1, PGD_STAT48, (int)g_48,
                PLAYER_PGD_PTR, (unsigned long long)other, PGD_INSIGHT, (int)o_ins, PGD_STAT48, (int)o_48,
                have_other ? (other == pgd ? " (same object)" : " (a different object)") : " (not a pointer)");
    }
    fflush(stderr);
}


/* Free co-op bells (src/vr_free_bells.h): game thread, every hook call. */
static void vu_free_bells_step(void) {
    if (!vu_free_bells) return;
    uint64_t wcm, player, gdm, pgd;
    int32_t pend = 0, ins = -1;
    if (!vu_ptr(vu_va(VA_WORLD_CHR_MAN), &wcm) || !vu_ptr(wcm + 0x60, &player) || !vu_ptr(vu_va(VRUI_VA_GAME_DATA_MAN), &gdm) ||
        !vu_ptr(gdm + 8, &pgd)) {
        vfb_step(&vu_fb, vu_now(), -1, 0);
        return;
    }
    vu_read(NULL, player + PLAYER_PENDING_USE, &pend, 4);
    if (!vu_read(NULL, pgd + PGD_INSIGHT, &ins, 4) || ins > VRDG_MAX_INSIGHT) ins = -1;
    const int refunds = vu_fb.refunds, floors = vu_fb.floors;
    const int32_t w = vfb_step(&vu_fb, vu_now(), ins, (uint32_t)pend);
    if (w >= 0) {
        memcpy((void *)(uintptr_t)(pgd + PGD_INSIGHT), &w, 4);
        fprintf(stderr, "Free bells: Insight %d -> %d (%s, pending use %#x, %d refund(s), %d floor(s))\n", (int)ins, (int)w,
                vu_fb.refunds != refunds ? "bell cost given back" : vu_fb.floors != floors ? "kept at 1 for the bell test" : "?",
                (unsigned)pend, vu_fb.refunds, vu_fb.floors);
        fflush(stderr);
    }
}

/* ---- The room auto-bell's direct item use ("selection swap", game thread, from the hook; docs in vu_online_bell) ----
 * GetUseItemId (0x18d1ef0) returns the inventory entry at the quick selection PGD+0x418 (no quick slot needed) and the
 * player's per-frame code (0x1cf74f2..0x1cf7553) calls it when the byte input+0x20 (input = [[PlayerIns+0x3B0]+0x80]) has
 * bit 0x80 (the "use item" action input), storing the id in PlayerIns+0x478. So: check the game's own predicate 0x1ae38d0,
 * point PGD+0x418 at the bell's inventory index, set that input bit, watch PlayerIns+0x478 / +0x490, restore the selection.
 * The pad thread posts vd.req = 1; the game thread answers through vd.ans (VD_ANS_*). PGD+0x458 and MenuMan+0x1e8 (the item
 * menu's own request) are not used. */
typedef unsigned char (ABI *VuCanUseFn)(void);
enum { VD_ANS_NONE = 0, VD_ANS_TAKEN = 1, VD_ANS_REFUSED = 2, VD_ANS_UNAVAIL = 3, VD_ANS_MISS = 4 };
static struct {
    int bytes_ok;                /* 0 unchecked, 1 the code is as expected, -1 differs (game thread) */
    int unavail;                 /* the bytes differ: the pad thread stops asking */
    int req;                     /* 0 idle, 1 requested (pad thread), 3 running (game thread) */
    uint32_t id;
    int32_t inv;
    int setbit;                  /* set the input bit (else only swap the selection) */
    double requested;
    int ans;                     /* VD_ANS_*, set by the game thread, cleared by the pad thread */
    int want_ans;                /* game thread: waiting to publish */
    const char *why;             /* reason of a refusal / unavailable */
    int32_t d478, d490, d458;    /* diagnostics at the answer */
    uint32_t d818;
    double refuse_log_at;
    /* the swap in progress (game thread) */
    int active, taken, miss_sent, setbit_s, taken_by_insight;
    int32_t old_sel, inv_s, before478, ins0; /* ins0: Insight at the swap (a bell's use takes 1) */
    int refunds0;                /* free-bells refunds at the swap (a refund = a bell's Insight cost was seen) */
    uint32_t id_s;
    double t0, taken_at;
} vd;

static int vd_check_bytes(void) {
    static const struct { uint32_t va; uint8_t n; uint8_t b[24]; } chk[] = {
        {0x18d1f09u, 10, {0x8b, 0x87, 0x88, 0x02, 0x00, 0x00, 0x85, 0xc0, 0x78, 0x27}},
        {0x18d1f3au, 6, {0x8b, 0x87, 0x48, 0x02, 0x00, 0x00}},
        {0x1cf7504u, 24, {0xf6, 0x81, 0x18, 0x08, 0x00, 0x00, 0x10, 0x74, 0x09, 0xf6, 0x80, 0xc0, 0x00, 0x00, 0x00, 0x80, 0x74, 0x4b,
                          0xf6, 0x40, 0x20, 0x80, 0x74, 0x45}},
        {0x1cf754cu, 7, {0x41, 0x89, 0x85, 0x78, 0x04, 0x00, 0x00}},
        {VA_CAN_USE_ITEM, 17, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x8d, 0x05, 0x97, 0xaf, 0xe5, 0x03}},
    };
    for (size_t i = 0; i < sizeof chk / sizeof chk[0]; ++i) {
        uint8_t got[24];
        if (!vu_read(NULL, vu_va(chk[i].va), got, chk[i].n) || memcmp(got, chk[i].b, chk[i].n) != 0) {
            fprintf(stderr, "Online bell: direct use unavailable: the code at %#x is not what was analysed\n", chk[i].va);
            fflush(stderr);
            return -1;
        }
    }
    return 1;
}

/* The living player, its PlayerGameData and the menu manager; 0 when any is missing. */
static int vd_world(uint64_t *player, uint64_t *pgd, uint64_t *mm) {
    uint64_t wcm, vt, gdm;
    return vu_ptr(vu_va(VA_WORLD_CHR_MAN), &wcm) && vu_ptr(wcm + 0x60, player) && vu_read(NULL, *player, &vt, 8) &&
           vt == vu_va(VA_PLAYER_INS_VTABLE) && vu_hp(*player) > 0 && vu_ptr(vu_va(VRUI_VA_GAME_DATA_MAN), &gdm) &&
           vu_ptr(gdm + 8, pgd) && vu_ptr(vu_va(VA_MENU_MAN), mm);
}

/* ChrCtrl = [player+0x3B0]; input = [ChrCtrl+0x80]: the address of the byte input+0x20 (0 if a pointer is bad).
 * flags818 (optional): [[ChrCtrl+8]+0x818], for the log. */
static uint64_t vd_input_byte(uint64_t player, uint32_t *flags818) {
    uint64_t chr, input, x;
    if (!vu_ptr(player + 0x3B0, &chr)) return 0;
    if (flags818 && vu_ptr(chr + 8, &x)) vu_read(NULL, x + 0x818, flags818, 4);
    if (!vu_ptr(chr + 0x80, &input) || !vu_readable(input + 0x20, 1)) return 0;
    return input + 0x20;
}

static void vd_set_bit(uint64_t player) {
    const uint64_t at = vd_input_byte(player, NULL);
    if (at) *(volatile uint8_t *)(uintptr_t)at |= 0x80;
}

static void vd_diag(uint64_t player, uint64_t pgd) {
    vd.d478 = vd.d490 = vd.d458 = -1;
    vd.d818 = 0xFFFFFFFFu;
    vu_read(NULL, player + PLAYER_USE_ITEM_ID, &vd.d478, 4);
    vu_read(NULL, player + PLAYER_USE_STATE, &vd.d490, 4);
    vu_read(NULL, pgd + PGD_USE_REQUEST, &vd.d458, 4);
    vd_input_byte(player, &vd.d818);
}

static void vd_restore(uint64_t pgd, const char *how) {
    int32_t sel = -2;
    vu_read(NULL, pgd + VRUI_PGD_QUICK_SELECTED, &sel, 4);
    if (sel == vd.inv_s) {
        memcpy((void *)(uintptr_t)(pgd + VRUI_PGD_QUICK_SELECTED), &vd.old_sel, 4);
        fprintf(stderr, "Online bell: selection swap ended (%s): quick selection %d restored\n", how, (int)vd.old_sel);
    } else {
        fprintf(stderr, "Online bell: selection swap ended (%s): the selection is %d now, left alone\n", how, (int)sel);
    }
    fflush(stderr);
    vd.active = 0;
}

static void vd_publish(void) {
    if (vd.want_ans && __atomic_load_n(&vd.ans, __ATOMIC_ACQUIRE) == VD_ANS_NONE) {
        __atomic_store_n(&vd.ans, vd.want_ans, __ATOMIC_RELEASE);
        vd.want_ans = 0;
    }
}

/* Game thread, every hook call: publishes a waiting answer, follows the swap in progress, takes a new request. */
static void vd_hook(double now) {
    vd_publish();
    uint64_t player, pgd, mm;
    if (vd.active) {
        if (!vd_world(&player, &pgd, &mm)) {
            if (now - vd.t0 > 3.0) { /* no living player (death, a load): PlayerGameData lives on, put the selection back */
                uint64_t gdm2 = 0, pgd2 = 0;
                if (vu_ptr(vu_va(VRUI_VA_GAME_DATA_MAN), &gdm2) && vu_ptr(gdm2 + 8, &pgd2)) vd_restore(pgd2, "no player");
                else vd.active = 0;
            }
        } else {
            int32_t p478 = 0, p490 = 0;
            vu_read(NULL, player + PLAYER_USE_ITEM_ID, &p478, 4);
            vu_read(NULL, player + PLAYER_USE_STATE, &p490, 4);
            if (!vd.taken) {
                /* +0x478 holds the used item's row (0x4b0 for goods 1200, no category bits); a bell's use also takes 1
                 * Insight (given back by the free bells: their refund count), the surest sign seen live */
                const uint32_t row = vd.id_s & 0x0FFFFFFFu;
                int32_t ins = vd.ins0;
                vu_read(NULL, pgd + PGD_INSIGHT, &ins, 4);
                const int took = ((uint32_t)p478 & 0x0FFFFFFFu) == row && (((uint32_t)vd.before478 & 0x0FFFFFFFu) != row || p490 != 0);
                const int paid = vu_fb.refunds != vd.refunds0 || (vd.ins0 > 0 && ins < vd.ins0);
                if (paid) vd.taken_by_insight = 1;
                if (took || paid || p490 != 0) {
                    vd.taken = 1;
                    vd.taken_at = now;
                    vd.want_ans = VD_ANS_TAKEN;
                    vd_diag(player, pgd);
                } else if (now - vd.t0 < 0.25 && vd.setbit_s) {
                    vd_set_bit(player);
                } else if (!vd.miss_sent && now - vd.t0 >= 0.5) {
                    vd.miss_sent = 1;
                    vd.want_ans = VD_ANS_MISS;
                    vd_diag(player, pgd);
                }
            }
            /* the player's quick selection is never left on the bell: back after the use, or after the square press window */
            if ((vd.taken && p490 == 0 && now - vd.taken_at >= 1.5) || (!vd.taken && now - vd.t0 >= 3.0) || now - vd.t0 >= 4.0)
                vd_restore(pgd, vd.taken ? "use finished" : "not taken");
            vd_publish();
        }
    }
    int expected = 1;
    if (!__atomic_compare_exchange_n(&vd.req, &expected, 3, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    int code = 0;
    const char *why = NULL;
    VrUiGameData g;
    uint32_t entry[4] = {0};
    player = pgd = mm = 0;
    if (vd.bytes_ok == 0) vd.bytes_ok = vd_check_bytes();
    if (vd.bytes_ok < 0) {
        code = VD_ANS_UNAVAIL;
        why = "code bytes differ";
        __atomic_store_n(&vd.unavail, 1, __ATOMIC_RELEASE);
    } else if (!vfb_is_bell(vd.id) || !vd_world(&player, &pgd, &mm) ||
               !vrui_read(vu_read, NULL, pgd, (uint64_t)(uintptr_t)vu.image, 0, &g)) {
        code = VD_ANS_REFUSED;
        why = "no living player / inventory unreadable";
    } else {
        int32_t page = 0, p490 = 0;
        uint64_t key = 0, normal = 0;
        vu_read(NULL, mm + 0x3C, &page, 4);
        vu_read(NULL, pgd + VRUI_PGD_KEY_ARRAY, &key, 8);
        vu_read(NULL, pgd + VRUI_PGD_NORMAL_ARRAY, &normal, 8);
        vu_read(NULL, player + PLAYER_USE_STATE, &p490, 4);
        if (page != 0) { code = VD_ANS_REFUSED; why = "a game menu is open"; }
        else if (!vrui_entry(vu_read, NULL, &g, key, normal, vd.inv, entry) || entry[1] != vd.id) { code = VD_ANS_REFUSED; why = "the item is not at that index"; }
        else if (p490 != 0) { code = VD_ANS_REFUSED; why = "an item use is running"; }
        else if (!(((VuCanUseFn)(void *)vu_va(VA_CAN_USE_ITEM))() & 0xFF)) { code = VD_ANS_REFUSED; why = "busy character"; }
        else if (!vd_input_byte(player, NULL)) { code = VD_ANS_UNAVAIL; why = "input pointers"; }
    }
    if (pgd && player) vd_diag(player, pgd);
    if (!code) {
        if (vd.active) vd_restore(pgd, "a new ring"); /* puts the old selection back first, so the next read is the real one */
        int32_t old = -1;
        vu_read(NULL, pgd + VRUI_PGD_QUICK_SELECTED, &old, 4);
        vd.old_sel = old;
        vd.inv_s = vd.inv;
        vd.id_s = vd.id;
        vd.before478 = vd.d478;
        vd.ins0 = -1;
        vu_read(NULL, pgd + PGD_INSIGHT, &vd.ins0, 4);
        vd.refunds0 = vu_fb.refunds;
        vd.taken_by_insight = 0;
        vd.setbit_s = vd.setbit;
        vd.taken = vd.miss_sent = 0;
        vd.t0 = now;
        vd.active = 1;
        memcpy((void *)(uintptr_t)(pgd + VRUI_PGD_QUICK_SELECTED), &vd.inv, 4);
        if (vd.setbit) vd_set_bit(player);
        else { vd.miss_sent = 1; vd.want_ans = VD_ANS_MISS; } /* selection swapped; the pad thread presses square */
    }
    vd.why = why;
    if (code) vd.want_ans = code;
    __atomic_store_n(&vd.req, 0, __ATOMIC_RELEASE);
    vd_publish();
}

#define vu_debug_speffect_step() ((void)0)

static ABI void vu_hook_menu(BbHookContext *ctx) {
    (void)ctx;
    if ((!vu.on && !vu.dbg) || !vu.image) return;
    __atomic_store_n(&vu_hook_us, (uint64_t)(vu_now() * 1e6) + 1u, __ATOMIC_RELEASE);
    vu_debug_give_step();
    vu_debug_speffect_step();
    vu_free_bells_step();
    if (vu_direct_enabled) vd_hook(vu_now());
    /* Taken once (1 -> 3), whichever of the two sites runs it first. */
    int expected = 1;
    if (!__atomic_compare_exchange_n(&vq.state, &expected, 3, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    uint64_t wcm, player, vt, gdm, pgd, mm;
    int reason = 0;
    VrUiGameData g;
    if (!vu_ptr(vu_va(VA_WORLD_CHR_MAN), &wcm) || !vu_ptr(wcm + 0x60, &player) || !vu_read(NULL, player, &vt, 8) ||
        vt != vu_va(VA_PLAYER_INS_VTABLE) || vu_hp(player) <= 0)
        reason = 3; /* no living player */
    else if (!vu_ptr(vu_va(VA_MENU_MAN), &mm))
        reason = 3;
    else if (!vu_ptr(vu_va(VRUI_VA_GAME_DATA_MAN), &gdm) || !vu_ptr(gdm + 8, &pgd) ||
             !vrui_read(vu_read, NULL, pgd, (uint64_t)(uintptr_t)vu.image, 0, &g))
        reason = 3;
    else {
        int32_t page = 0;
        vu_read(NULL, mm + 0x3C, &page, 4);
        if (page != 0) reason = 6; /* the game's own menu is open: let it do it */
    }
    uint32_t entry[4] = {0};
    if (!reason) {
        uint64_t key = 0, normal = 0;
        vu_read(NULL, pgd + VRUI_PGD_KEY_ARRAY, &key, 8);
        vu_read(NULL, pgd + VRUI_PGD_NORMAL_ARRAY, &normal, 8);
        if (!vrui_entry(vu_read, NULL, &g, key, normal, vq.inv, entry) || entry[1] != vq.id) reason = 1;
    }
    const uint32_t slot = vq.slot;
    uint32_t before = 0xFFFFFFFFu, after = 0xFFFFFFFFu;
    if (!reason) {
        if (slot >= 0x20) { /* Remove from wheel: the slot's inventory index back to -1 (its empty value) */
            const uint64_t at = pgd + VRUI_PGD_QUICK + 8u * (slot - 0x20) + 4;
            vu_read(NULL, at, &before, 4);
            if ((int32_t)before != vq.inv) reason = 1; /* the slot changed since the dashboard looked */
            else {
                const int32_t none = -1;
                memcpy((void *)(uintptr_t)at, &none, 4);
                int32_t sel = -1;
                vu_read(NULL, pgd + VRUI_PGD_QUICK_SELECTED, &sel, 4);
                if (sel == vq.inv) { /* it was the current quick item: the next filled slot becomes current */
                    int32_t next = -1;
                    for (uint32_t k = 1; k <= 6 && next < 0; ++k) {
                        int32_t idx = -1;
                        vu_read(NULL, pgd + VRUI_PGD_QUICK + 8u * ((slot - 0x20 + k) % 6) + 4, &idx, 4);
                        if (idx >= 0) next = idx;
                    }
                    memcpy((void *)(uintptr_t)(pgd + VRUI_PGD_QUICK_SELECTED), &next, 4);
                }
                vu_read(NULL, at, &after, 4);
                if ((int32_t)after != -1) reason = 5;
            }
        } else {
        if (slot < 16) vu_read(NULL, pgd + VRUI_PGD_WEAPON_IDS + 4u * slot, &before, 4);
        else vu_read(NULL, pgd + VRUI_PGD_QUICK + 8u * (slot - 0x10) + 4, &before, 4);
        /* The record the menus pass: only +0x48 (inventory index) and +0x4c (item id) are read. */
        _Alignas(16) unsigned char record[0x60];
        memset(record, 0, sizeof(record));
        memcpy(record + 0x48, &vq.inv, 4);
        memcpy(record + 0x4C, &vq.id, 4);
        ((VuEquipFn)(void *)vu_va(VA_EQUIP_FROM_MENU))(slot, record);
        if (slot < 16) vu_read(NULL, pgd + VRUI_PGD_WEAPON_IDS + 4u * slot, &after, 4);
        else vu_read(NULL, pgd + VRUI_PGD_QUICK + 8u * (slot - 0x10) + 4, &after, 4);
        const uint32_t want = slot < 16 ? (vq.id & 0x0FFFFFFFu) : (uint32_t)vq.inv;
        if ((after & (slot < 16 ? 0x0FFFFFFFu : 0xFFFFFFFFu)) != want) reason = 5;
        }
    }
    vq.before = before;
    vq.after = after;
    vq.ok = !reason;
    vq.reason = reason;
    __atomic_store_n(&vq.state, 2, __ATOMIC_RELEASE);
}

void runtime_vr_ui_register(unsigned char *image) {
    vu.image = image;
    vu_init();
    bb_hook_set_ui_impl(vu_hook_menu);   /* "VR UI Equip (HUD input)": only while a game menu is up */
    bb_hook_set_step_extra(vu_hook_menu); /* "VR FP Camera Heading (in-game step)": every gameplay frame */
}

/* The stamina tickets as swing events (src/runtime_vr_stamina.c; any thread). */
void runtime_vr_ui_event(uint32_t kind, int32_t a, int32_t b, int32_t c) {
    vu_init();
    if (!vu.on) return;
    bbgpu_vr_ui_event(kind, a, b, c);
}

/* The player's stamina (0: no player: title, loading). */
static int vu_player(int32_t *stamina, int32_t *max, int32_t *hp, int32_t *hp_max) {
    uint64_t wcm, p, vt, bag, d;
    if (!vu.image || !vu_ptr(vu_va(VA_WORLD_CHR_MAN), &wcm) || !vu_ptr(wcm + 0x60, &p) || !vu_read(NULL, p, &vt, 8) ||
        vt != vu_va(VA_PLAYER_INS_VTABLE))
        return 0;
    *stamina = *max = *hp = *hp_max = 0;
    if (vu_ptr(p + 0x3B0, &bag) && vu_ptr(bag + 0x20, &d) && vu_read(NULL, d, &vt, 8) && vt == vu_va(VA_CHR_DATA_VTABLE)) {
        vu_read(NULL, d + 0x134, stamina, 4);
        vu_read(NULL, d + 0x138, max, 4);
        vu_read(NULL, d + 0xF8, hp, 4);
        vu_read(NULL, d + 0xFC, hp_max, 4);
        return 2;
    }
    return 1;
}

static void vu_probe_log(double now, int player) {
    const VrUiGameData *g = &vu.game;
    const int changed = g->ok != vu.probe_ok || g->selected_index != vu.probe_selected ||
                        g->active_right != vu.probe_right || g->active_left != vu.probe_left;
    if (!changed && now - vu.probe_at < 2.0) return;
    vu.probe_at = now;
    vu.probe_ok = g->ok;
    vu.probe_selected = g->selected_index;
    vu.probe_right = g->active_right;
    vu.probe_left = g->active_left;
    uint64_t gdm = 0, pgd = 0;
    vu_ptr(vu_va(VRUI_VA_GAME_DATA_MAN), &gdm);
    if (gdm) vu_ptr(gdm + 8, &pgd);
    if (!g->ok) {
        fprintf(stderr, "VR UI probe: player %d, GameDataMan %#llx, PlayerGameData %#llx: vtables or layout not as expected\n",
                player, (unsigned long long)gdm, (unsigned long long)pgd);
        if (pgd) {
            uint64_t vt[3] = {0};
            vu_read(NULL, pgd + VRUI_PGD_EGD, &vt[0], 8);
            vu_read(NULL, pgd + VRUI_PGD_EID, &vt[1], 8);
            vu_read(NULL, pgd + VRUI_PGD_EIT, &vt[2], 8);
            const uint64_t base = (uint64_t)(uintptr_t)vu.image - VA_BASE;
            fprintf(stderr, "VR UI probe:   vtables at +0x1D0 %#llx, +0x328 %#llx, +0x3D0 %#llx (VA; want 0x576f130 0x576f320 0x576f360)\n",
                    (unsigned long long)(vt[0] - base), (unsigned long long)(vt[1] - base), (unsigned long long)(vt[2] - base));
        }
        fflush(stderr);
        return;
    }
    char quick[512] = "";
    size_t at = 0;
    for (int k = 0; k < g->quick_count && at < sizeof(quick) - 64; ++k)
        at += (size_t)snprintf(quick + at, sizeof(quick) - at, " [%d%s idx %d id %#x x%d]", k, k == g->quick_active ? "*" : "",
                               (int)g->quick_index[k], g->quick_id[k], (int)g->quick_count_of[k]);
    int32_t vials = -1, bullets = -1;
    for (int i = 0; i < g->item_count; ++i) {
        if (g->items[i].id == (VRUI_GOODS | 1000u)) vials = g->items[i].count;
        if (g->items[i].id == (VRUI_GOODS | 900u)) bullets = g->items[i].count;
    }
    fprintf(stderr,
            "VR UI probe: PGD %#llx keyCap %d normalCap %d tail %d, %d items (vials %d, bullets %d); selected idx %d, quick%s;"
            " weapons R %u/%u (active %d) L %u/%u (active %d)\n",
            (unsigned long long)g->pgd, (int)g->key_cap, (int)g->normal_cap, (int)g->tail, vu.state.item_count, (int)vials,
            (int)bullets, (int)g->selected_index, quick, g->weapon[0], g->weapon[1], (int)g->active_right, g->weapon[2],
            g->weapon[3], (int)g->active_left);
    fflush(stderr);
}

/* The weapon swap check (findings/26: "the trigger swap does not work"): every d-pad right/left press
 * that reaches the game is logged with that hand's slots, and 0.6 s later whether the held slot
 * changed. An empty second slot (or "Bare Fists" 110000) is why a swap shows nothing. */
static struct { uint32_t prev; double check_at; int hand; int32_t before; } vs_swap;

static void vu_swap_log(double now, uint32_t buttons) {
    const VrUiGameData *g = &vu.game;
    const uint32_t edge = buttons & ~vs_swap.prev;
    vs_swap.prev = buttons;
    if (g->ok && (edge & (VRUI_BTN_RIGHT | VRUI_BTN_LEFT))) {
        const int right = (edge & VRUI_BTN_RIGHT) != 0;
        vs_swap.hand = right;
        vs_swap.before = right ? g->active_right : g->active_left;
        vs_swap.check_at = now + 0.6;
        const uint32_t a = g->weapon[right ? 0 : 2], b = g->weapon[right ? 1 : 3];
        fprintf(stderr, "VR UI: d-pad %s reached the game: %s slots %u / %u (110000 = bare fists), held slot %d\n",
                right ? "right" : "left", right ? "right" : "left", a, b, (int)vs_swap.before);
        fflush(stderr);
    }
    if (vs_swap.check_at > 0.0 && now >= vs_swap.check_at) {
        vs_swap.check_at = 0.0;
        const int32_t after = g->ok ? (vs_swap.hand ? g->active_right : g->active_left) : -1;
        fprintf(stderr, "VR UI: ... 0.6 s later the %s hand holds slot %d (%s)\n", vs_swap.hand ? "right" : "left",
                (int)after, after != vs_swap.before ? "swapped" : "NOT swapped: the game ignored it (busy, or the other slot empty)");
        fflush(stderr);
    }
}

/* A dashboard request to put an item into an equip or quick slot: checked here, carried out by the
 * game thread (vu_hook_menu). */
static void vu_request_equip(const BbXrUiAction *a, double now) {
    const VrUiGameData *g = &vu.game;
    int32_t inv = -1;
    const int clear = a->kind == BBXR_UI_CLEAR_QUICK;
    if (clear) { /* the slot's own inventory index: emptied only while it still holds what the dashboard saw */
        if (a->a >= 0 && a->a < g->quick_count && g->quick_id[a->a] == (uint32_t)a->b) inv = g->quick_index[a->a];
    } else {
        for (int i = 0; i < g->item_count && inv < 0; ++i)
            if (g->items[i].id == (uint32_t)a->b) inv = g->items[i].index;
    }
    const uint32_t slot = a->kind == BBXR_UI_EQUIP_ITEM ? (uint32_t)a->a : (clear ? 0x20u : 0x10u) + (uint32_t)a->a;
    fprintf(stderr, "VR UI: action %u from the headset: item %#x (inventory index %d) into slot %#x%s\n", a->kind,
            (unsigned)a->b, (int)inv, slot, a->kind == BBXR_UI_ASSIGN_QUICK && a->c ? ", then use it" : "");
    const uint64_t seen_us = __atomic_load_n(&vu_hook_us, __ATOMIC_ACQUIRE);
        const double seen = seen_us ? (double)(seen_us - 1u) * 1e-6 : -1.0;
    if (inv < 0 || !g->ok || (a->kind == BBXR_UI_EQUIP_ITEM && (a->a < 0 || a->a > 3)) ||
        ((a->kind == BBXR_UI_ASSIGN_QUICK || clear) && (a->a < 0 || a->a > 5))) {
        bbgpu_vr_ui_event(BBXR_EV_ACTION_DONE, (int32_t)a->kind, 0, inv < 0 ? 1 : 3);
    } else if (seen < 0.0 || now - seen > 2.0) {
        fprintf(stderr, "VR UI: no game-thread hook has run in 2 s: select the patch \"VR FP Camera Heading (in-game step)\" in BB_HOOKS\n");
        bbgpu_vr_ui_event(BBXR_EV_ACTION_DONE, (int32_t)a->kind, 0, 4);
    } else {
        vq.slot = slot;
        vq.id = (uint32_t)a->b;
        vq.inv = inv;
        vq.requested = now;
        vq.before = vq.after = 0xFFFFFFFFu;
        vu.then_use = a->kind == BBXR_UI_ASSIGN_QUICK && a->c ? a->a : -1;
        __atomic_store_n(&vq.state, 1, __ATOMIC_RELEASE);
    }
    fflush(stderr);
}

/* Every pad read: the state to the bridge, the actions from it; returns the buttons to press.
 * `buttons`: what the game gets so far this read (the swap check). */
/* BB_DEBUG_USE / the control file (pad thread; public builds: no file, only the auto-bell's request): uses a goods item through the dashboard's own path.
 * In a quick slot already: d-pad down to it, square. Else the equip command puts it into the first
 * empty quick slot (the last, 5, when all are taken) and then uses it. Waits (up to 15 s) for the item
 * to be in the inventory, for the character to have been in the world 3 s, and for BB_DEBUG_GIVE. */
static void vu_debug_use(double now, int player) {
    const VrUiGameData *g = &vu.game;
    if (!player || !g->ok) {
        vu.use_gate_at = 0.0;
        return;
    }
    if (vu.use_gate_at == 0.0) vu.use_gate_at = now + 3.0;
    if (vu.pend_until > now) { /* the pending use-item id shows for a moment when the game takes the press */
        uint64_t wcm, p;
        int32_t pend = 0;
        if (vu_ptr(vu_va(VA_WORLD_CHR_MAN), &wcm) && vu_ptr(wcm + 0x60, &p) && vu_read(NULL, p + PLAYER_PENDING_USE, &pend, 4) &&
            pend != vu.pend_before && !vu.pend_seen) {
            vu.pend_seen = 1;
            fprintf(stderr, "Debug use: player+0x394 (pending use-item id) changed %#x -> %#x: the press reached the game\n",
                    (unsigned)vu.pend_before, (unsigned)pend);
            fflush(stderr);
        }
    } else if (vu.pend_until > 0.0) {
        vu.pend_until = 0.0;
        if (!vu.pend_seen) {
            fprintf(stderr, "Debug use: player+0x394 never changed from %#x during the press (sampled each pad poll)\n",
                    (unsigned)vu.pend_before);
            fflush(stderr);
        }
    }
    if (vu.chk_id && now >= vu.chk_at) {
        /* The result of a debug use: did the press start the item? Its count drops for a consumed item (the
         * bells are not consumed: look for the summon request in the net log), and 0x1731d00 is the game's
         * "Insight <= 0" test. */
        uint64_t gdm = 0, pgd = 0;
        int32_t ins = -1;
        if (vu_ptr(vu_va(VRUI_VA_GAME_DATA_MAN), &gdm) && vu_ptr(gdm + 8, &pgd)) vu_read(NULL, pgd + PGD_INSIGHT, &ins, 4);
        const int32_t cnt = vu_count_of(g, vu.chk_id);
        fprintf(stderr, "Debug use: result for %#x: count %d -> %d, insight %d -> %d (game's no-insight test 0x1731d00: %s); exec %s\n",
                vu.chk_id, (int)vu.chk_count, (int)cnt, (int)vu.chk_insight, (int)ins, ins <= 0 ? "TRUE, bell refused" : "false",
                vu.exec.kind ? (vu.exec.finished > 0 ? "done" : vu.exec.finished < 0 ? "gave up" : "still running") : "finished");
        fflush(stderr);
        vu.chk_id = 0;
    }
    if (!vu.use_pending || now < vu.use_gate_at || vu.give_state == 1) return;
    if (vu.use_since == 0.0) vu.use_since = now;
    if (vrui_exec_busy(&vu.exec) || __atomic_load_n(&vq.state, __ATOMIC_ACQUIRE) || !g->items_read) return;
    const uint32_t id = vu.use_pending;
    if (!vfb_is_bell(id)) { /* public builds press only the bells */
        vu.use_pending = 0;
        return;
    }
    int32_t inv = -1;
    for (int i = 0; i < g->item_count && inv < 0; ++i)
        if (g->items[i].id == id) inv = g->items[i].index;
    if (inv < 0) {
        if (now - vu.use_since > 15.0) {
            fprintf(stderr, "Debug use: item %#x is not in the inventory (it must be given first); dropped\n", id);
            fflush(stderr);
            vu.use_pending = 0;
        }
        return;
    }
    vu.use_pending = 0;
    vu.use_since = 0.0;
    {
        uint64_t gdm = 0, pgd = 0;
        int32_t ins = -1;
        if (vu_ptr(vu_va(VRUI_VA_GAME_DATA_MAN), &gdm) && vu_ptr(gdm + 8, &pgd)) vu_read(NULL, pgd + PGD_INSIGHT, &ins, 4);
        vu.chk_id = id;
        if (vfb_is_bell(id)) vu_fb_arm(now); /* our own press of a bell (auto-bell / BB_DEBUG_USE) */
        vu.chk_at = now + 6.0;
        vu.chk_count = vu_count_of(g, id);
        vu.chk_insight = ins;
        {
            uint64_t wcm, p;
            int32_t pend = 0;
            if (vu_ptr(vu_va(VA_WORLD_CHR_MAN), &wcm) && vu_ptr(wcm + 0x60, &p)) vu_read(NULL, p + PLAYER_PENDING_USE, &pend, 4);
            vu.pend_before = pend;
            vu.pend_seen = 0;
            vu.pend_until = now + 5.0;
        }
        fprintf(stderr, "Debug use: pressing %#x (count %d, insight %d); result in 6 s\n", id, (int)vu.chk_count, (int)ins);
    }
    int slot = -1;
    for (int k = 0; k < g->quick_count && slot < 0; ++k)
        if (g->quick_id[k] == id) slot = k;
    if (slot >= 0) {
        fprintf(stderr, "Debug use: item %#x is in quick slot %d: d-pad down to it, square\n", id, slot);
        fflush(stderr);
        vrui_exec_start(&vu.exec, VRUI_ACT_USE_QUICK, slot, id, now);
        vu.exec_reported = 0;
        return;
    }
    int target = VRUI_QUICK_SLOTS - 1;
    for (int k = 0; k < g->quick_count; ++k)
        if (g->quick_index[k] < 0) {
            target = k;
            break;
        }
    BbXrUiAction a = {BBXR_UI_ASSIGN_QUICK, target, (int32_t)id, 1};
    fprintf(stderr, "Debug use: item %#x into quick slot %d, then use it\n", id, target);
    vu_request_equip(&a, now);
}

/* The auto-bell (src/runtime_online_bell.c): the machine decides (when, the retry state), this rings. Three paths, in order:
 *  1. direct (default; BB_ONLINE_BELL_DIRECT=0 turns it off): the game thread (vd_hook) swaps the quick selection PGD+0x418 to
 *     the bell's inventory index, checks the game's own "an item can be used now" predicate 0x1ae38d0 and sets the "use item"
 *     input bit (byte input+0x20 |= 0x80, input = [[PlayerIns+0x3B0]+0x80]) for 0.25 s; the game's per-frame code then calls
 *     GetUseItemId and stores the bell in PlayerIns+0x478. No pad press, no quick slot; the selection is put back later.
 *     Answers: taken -> reached; the predicate false (attacking, rolling...) -> "refused", the machine retries in ~1 s; the
 *     code bytes differ / pointers bad -> "unavailable", this ring goes the old way (3).
 *  2. tap: the bit was not taken within 0.5 s (3 misses in a row: the bit is no longer set, swap + tap at once): the selection
 *     stays swapped and ONE square press (VRUI_ACT_PRESS) is sent through the pad; GetUseItemId now returns the bell. Watched
 *     for 1.5 s: taken -> reached, else not reached. "Taken" = PlayerIns+0x478 shows the bell's row (it holds rows, no category
 *     bits), the use state +0x490 starts, or the bell's Insight cost was seen (Insight dropped / a free-bells refund): live, the
 *     cost was the only sign of a working ring. 2 rings in a row with neither direct nor the press taken: path 3 for the rest
 *     of the session. The quick selection goes back after the use, or 3 s after an untaken swap.
 *  3. pad: the old quick-slot path (BB_DEBUG_USE's): the bell into a quick slot, d-pad down to it, square. A ring is "done"
 *     once the use was taken, its executor finished and the 6 s result check ran; it "reached" the game when player+0x394
 *     (the pending use-item id) changed during the press (vu.pend_seen) or the press sequence ran to its end. */
static struct {
    int ringing;      /* a pad-path (3) ring is running */
    double since;
    int phase;        /* direct / tap: 0 none, 1 waiting for the game thread, 2 the square press is out */
    double tap_until;
    int misses;       /* consecutive direct rings that took no effect */
    int tap_fails;    /* consecutive rings where direct + the square press both took no effect */
    int off;          /* direct given up for this session (tap_fails reached 2): the pad path */
    int bit_off_logged;
    uint32_t id;
    int32_t inv;
} vb;

/* The pad path for this ring (the old way). */
static void vu_online_bell_pad(double now, uint32_t id) {
    vu.use_pending = id;
    vu.use_since = now;
    vu.pend_seen = 0;
    vb.ringing = 1;
    vb.since = now;
}

static void vu_online_bell_answer(double now, int a) {
    const uint32_t id = vb.id;
    if (a == VD_ANS_TAKEN) {
        fprintf(stderr, "Online bell: direct use of %#x (inventory index %d): taken%s, seen by %s (PlayerIns+0x478 %#x, +0x490 %d, [[ChrCtrl+8]+0x818] %#x)\n",
                id, (int)vb.inv, vb.phase == 2 ? " by the square press" : "", vd.taken_by_insight ? "the Insight cost" : "the use id / state",
                (unsigned)vd.d478, (int)vd.d490, (unsigned)vd.d818);
        if (vb.phase == 1) vb.misses = 0;
        vb.tap_fails = 0;
        runtime_online_bell_result(now, 1, vb.phase == 2 ? "tap" : "direct");
        vb.phase = 0;
    } else if (a == VD_ANS_REFUSED) {
        if (now - vd.refuse_log_at >= 5.0) {
            vd.refuse_log_at = now;
            fprintf(stderr, "Online bell: direct use of %#x (inventory index %d): refused (busy character): %s (PlayerIns+0x478 %#x, +0x490 %d, [[ChrCtrl+8]+0x818] %#x, PGD+0x458 %d)\n",
                    id, (int)vb.inv, vd.why ? vd.why : "?", (unsigned)vd.d478, (int)vd.d490, (unsigned)vd.d818, (int)vd.d458);
        }
        runtime_online_bell_result(now, -1, "direct");
        vb.phase = 0;
    } else if (a == VD_ANS_UNAVAIL) {
        fprintf(stderr, "Online bell: direct use of %#x (inventory index %d): unavailable (%s); using the pad path for this ring\n", id,
                (int)vb.inv, vd.why ? vd.why : "?");
        vb.phase = 0;
        vu_online_bell_pad(now, id);
    } else if (a == VD_ANS_MISS && vb.phase == 1) {
        ++vb.misses;
        fprintf(stderr, "Online bell: direct use of %#x (inventory index %d): not taken in 0.5 s, trying one square press (PlayerIns+0x478 %#x, +0x490 %d, [[ChrCtrl+8]+0x818] %#x, %d miss(es) in a row)\n",
                id, (int)vb.inv, (unsigned)vd.d478, (int)vd.d490, (unsigned)vd.d818, vb.misses);
        if (vb.misses >= 3 && !vb.bit_off_logged) {
            vb.bit_off_logged = 1;
            fprintf(stderr, "Online bell: direct input bit never taken; using selection swap + one square press\n");
        }
        fflush(stderr);
        if (vrui_exec_busy(&vu.exec)) { /* the player's own action is running: not now */
            runtime_online_bell_result(now, -1, "direct");
            vb.phase = 0;
        } else {
            vrui_exec_start(&vu.exec, VRUI_ACT_PRESS, 0, VRUI_BTN_SQUARE, now);
            vu.exec_reported = 0;
            vb.phase = 2;
            vb.tap_until = now + 1.5;
        }
    }
    fflush(stderr);
}

static void vu_online_bell(double now, int player, int hp, uint32_t buttons_in) {
    if (!runtime_online_bell_armed()) return;
    const VrUiGameData *g = &vu.game;
    const uint32_t id = runtime_online_bell_item();
    if (vb.phase) { /* direct / tap in progress */
        const int a = __atomic_load_n(&vd.ans, __ATOMIC_ACQUIRE);
        if (a) {
            __atomic_store_n(&vd.ans, VD_ANS_NONE, __ATOMIC_RELEASE);
            vu_online_bell_answer(now, a);
        } else if (vb.phase == 1 && __atomic_load_n(&vd.req, __ATOMIC_ACQUIRE) == 1 && now - vd.requested > 2.0) {
            int expected = 1; /* the game thread never took it: the pad path */
            if (__atomic_compare_exchange_n(&vd.req, &expected, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                fprintf(stderr, "Online bell: direct use of %#x: unavailable (the game-thread hook did not run in 2 s); using the pad path for this ring\n", id);
                fflush(stderr);
                vb.phase = 0;
                vu_online_bell_pad(now, id);
            }
        }
        if (vb.phase == 2 && now >= vb.tap_until) {
            fprintf(stderr, "Online bell: direct use of %#x (inventory index %d): one square press, not taken in 1.5 s (PlayerIns+0x478 %#x)\n", id,
                    (int)vb.inv, (unsigned)vd.d478);
            fflush(stderr);
            runtime_online_bell_result(now, 0, "tap");
            vb.phase = 0;
            if (++vb.tap_fails >= 2 && !vb.off) {
                vb.off = 1;
                fprintf(stderr, "Online bell: direct use and the square press took no effect %d rings in a row; the old quick-slot path (pad) for the rest of the session\n",
                        vb.tap_fails);
                fflush(stderr);
            }
        }
    }
    const int busy = vrui_exec_busy(&vu.exec) || __atomic_load_n(&vq.state, __ATOMIC_ACQUIRE) || vu.use_pending ||
                     vu.give_state == 1 || vu.give_state == 2 || vb.phase || __atomic_load_n(&vd.req, __ATOMIC_ACQUIRE) ||
                     __atomic_load_n(&vd.ans, __ATOMIC_ACQUIRE) || __atomic_load_n(&vd.active, __ATOMIC_ACQUIRE); /* a swap still on */
    if (vb.ringing && !busy && !vu.chk_id && now - vb.since > 1.0) {
        vb.ringing = 0;
        /* "Reached" = the pending use-item id (player+0x394) changed, or the whole press sequence ran to its end (the
         * bell was selected and square was pressed). The id often stays at the bell's own id after an earlier ring,
         * so a later, working ring never shows a change: the id alone made those read as "did not reach the game". */
        const int ran = vu.exec.kind == VRUI_ACT_USE_QUICK && vu.exec.item == id && vu.exec.finished > 0;
        const int reached = vu.pend_seen || ran;
        if (!reached) {
            const int flags = (int)vu.state.flags;
            fprintf(stderr, "Online bell: press not seen: exec kind %d %s (reason %d, %d d-pad press(es)), quick_active %d (bell slot %d), game menu %d, busy %d, pending id before %#x%s\n",
                    vu.exec.kind, vu.exec.finished > 0 ? "done" : vu.exec.finished < 0 ? "gave up" : "not finished", vu.exec.reason,
                    vu.exec.presses, (int)vu.game.quick_active, vu.exec.slot, (flags & BBXR_GAME_MENU_OPEN) != 0,
                    (flags & BBXR_GAME_BUSY) != 0, (unsigned)vu.pend_before, vu.exec.kind ? "" : " (no press was started)");
            fflush(stderr);
        }
        runtime_online_bell_result(now, reached, "pad");
    }
    const int count = g->ok && g->items_read ? (int)vu_count_of(g, id) : -1;
    const int menu = (vu.state.flags & BBXR_GAME_MENU_OPEN) != 0;
    /* acting: the player's own press in the last 1.5 s (buttons_in is the pad before this module's presses are added) */
    static uint32_t prev_buttons;
    static double last_press = -100.0;
    if (buttons_in & ~prev_buttons) last_press = now;
    prev_buttons = buttons_in;
    const int acting = now - last_press < 1.5;
    const int direct_ok = vu_direct_enabled && !vb.off && !__atomic_load_n(&vd.unavail, __ATOMIC_ACQUIRE);
    const int act = runtime_online_bell_tick(now, player != 0, player == 2 && hp > 0, menu, busy, acting, count, direct_ok);
    if (act == 1) {
        vu_fb_arm(now); /* the refund window opens at each attempt */
        int32_t inv = -1;
        for (int i = 0; i < g->item_count && inv < 0; ++i)
            if (g->items[i].id == id) inv = g->items[i].index;
        if (direct_ok && inv >= 0 && vfb_is_bell(id)) {
            vb.id = id;
            vb.inv = inv;
            vb.phase = 1;
            vd.id = id;
            vd.inv = inv;
            vd.setbit = vb.misses < 3;
            vd.why = NULL;
            vd.requested = now;
            __atomic_store_n(&vd.req, 1, __ATOMIC_RELEASE);
        } else {
            vu_online_bell_pad(now, id);
        }
    } else if (act == 2 && vu.give_state != 1 && vu.give_state != 2) {
        memset(&vu.give, 0, sizeof(vu.give));
        vu.give.items[0].id = id;
        vu.give.items[0].count = 1;
        vu.give.n = 1;
        vu.give_i = 0;
        vu.give_stable = 0;
        vu.give_state = 1;
    }
}

static double vu_no_player_since; /* when vu_player() first returned 0 (0: there is a player) */

uint32_t runtime_vr_ui_pad(uint32_t buttons_in) {
    vu_init();
    if (!vu.on && !vu.probe && !vu.dbg) return 0;
    const double now = vu_now();
    BbXrGameState *s = &vu.state;
    const int with_items = now - vu.items_at > 0.25;
    int32_t stamina = 0, max = 0, hp = 0, hp_max = 0;
    const int player = vu_player(&stamina, &max, &hp, &hp_max);
    uint64_t gdm = 0, pgd = 0;
    VrUiGameData *g = &vu.game;
    const int keep_items = !with_items && g->items_read;
    VrUiItem *saved = NULL;
    int saved_count = 0;
    static VrUiItem saved_items[VRUI_MAX_ITEMS];
    if (keep_items) {
        saved_count = g->item_count;
        memcpy(saved_items, g->items, sizeof(VrUiItem) * (size_t)saved_count);
        saved = saved_items;
    }
    if (player && vu_ptr(vu_va(VRUI_VA_GAME_DATA_MAN), &gdm) && vu_ptr(gdm + 8, &pgd))
        vrui_read(vu_read, NULL, pgd, (uint64_t)(uintptr_t)vu.image, with_items, g);
    else
        memset(g, 0, sizeof(*g));
    if (with_items) vu.items_at = now;
    if (saved && g->ok) {
        memcpy(g->items, saved, sizeof(VrUiItem) * (size_t)saved_count);
        g->item_count = saved_count;
        g->items_read = 1;
    }
    memset(s, 0, sizeof(*s));
    s->size = sizeof(*s);
    if (player) {
        s->flags |= BBXR_GAME_IN_WORLD;
        vu_no_player_since = 0.0;
    } else {
        /* No PlayerIns under WorldChrMan: title, character creation, loading, system dialogs. Held
         * for 0.4 s first so a one-poll blip never takes the world's controls; the bridge turns the
         * sticks into the d-pad. The player appearing clears it at once. */
        if (vu_no_player_since == 0.0) vu_no_player_since = now;
        if (now - vu_no_player_since >= 0.4) s->flags |= BBXR_GAME_PRE_WORLD;
    }
    if (player == 2 && max > 0) {
        s->flags |= BBXR_GAME_STAMINA;
        s->stamina = stamina;
        s->stamina_max = max;
        s->hp = hp;
        s->hp_max = hp_max;
    }
    if (g->ok) {
        s->flags |= BBXR_GAME_QUICK | BBXR_GAME_WEAPONS;
        s->quick_count = (uint32_t)g->quick_count;
        s->quick_active = g->quick_active >= 0 ? (uint32_t)g->quick_active : 0xFFFFFFFFu;
        for (int k = 0; k < g->quick_count && k < (int)BBXR_GAME_MAX_QUICK; ++k) s->quick[k] = g->quick_id[k];
        for (int i = 0; i < 4; ++i) s->weapons[i] = g->weapon[i];
        s->weapon_active[0] = (uint32_t)g->active_right;
        s->weapon_active[1] = (uint32_t)g->active_left;
        if (g->items_read) {
            s->flags |= BBXR_GAME_ITEMS;
            s->item_count = (uint32_t)(g->item_count < (int)BBXR_GAME_MAX_ITEMS ? g->item_count : (int)BBXR_GAME_MAX_ITEMS);
            for (uint32_t i = 0; i < s->item_count; ++i) {
                s->items[i].id = g->items[i].id;
                s->items[i].count = g->items[i].count;
            }
        }
    }
    {
        uint64_t mm;
        uint32_t f[0x300 / 4];
        /* The game's pad gate (0x7ffff77f), plus 0x80 (+0x58 == 4): live, the talk/choice panels
         * (the Doll's level-up choices) set only that, and need the sticks as much. */
        if (vu_ptr(vu_va(VA_MENU_MAN), &mm) && vu_read(NULL, mm, f, sizeof f) &&
            ((vu_menu_mask(f) | vu_menu_mask_globals(f)) & (0x7ffff77fu | 0x80u)))
            s->flags |= BBXR_GAME_MENU_OPEN;
    }
    if (vrui_exec_busy(&vu.exec) || __atomic_load_n(&vq.state, __ATOMIC_ACQUIRE) || vb.phase) s->flags |= BBXR_GAME_BUSY;
    {
        const uint64_t seen_us = __atomic_load_n(&vu_hook_us, __ATOMIC_ACQUIRE);
        const double seen = seen_us ? (double)(seen_us - 1u) * 1e-6 : -1.0;
        if (seen >= 0.0 && now - seen < 2.0) s->flags |= BBXR_GAME_CAN_EQUIP;
    }
    if (vu.probe) vu_probe_log(now, player);
    vu_swap_log(now, buttons_in);
    {
        /* the player's own ring: square (use item) while a bell is the selected quick item */
        static uint32_t fb_prev;
        const uint32_t edge = buttons_in & ~fb_prev;
        fb_prev = buttons_in;
        if ((edge & VRUI_BTN_SQUARE) && g->ok && g->quick_active >= 0 && g->quick_active < VRUI_QUICK_SLOTS &&
            vfb_is_bell(g->quick_id[g->quick_active]))
            vu_fb_arm(now);
    }
    if (!vu.on && !vu.dbg) return 0;
    if (vu.on) {
        vr_hitbox_fill(&s->debug, vu.image); /* the hitbox overlay (findings/29) */
        vr_skin_fill(&s->skin, vu.image); /* custom character model override */
        bbgpu_vr_ui_state(s, (uint32_t)sizeof(*s));
    }
    if (vu.dbg) vu_debug_use(now, player);
    if (vu.dbg) vu_online_bell(now, player, hp, buttons_in);

    /* An equip request the game thread finished (or never picked up). */
    const int qstate = __atomic_load_n(&vq.state, __ATOMIC_ACQUIRE);
    if (qstate == 2 || (qstate == 1 && now - vq.requested > 2.0)) {
        if (qstate == 1) { /* never picked up: take it back so a late run cannot start it */
            int expected = 1;
            if (!__atomic_compare_exchange_n(&vq.state, &expected, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return 0; /* the game thread took it just now: report next poll */
        }
        const int ok = qstate == 2 && vq.ok;
        const int reason = qstate == 2 ? vq.reason : 4;
        __atomic_store_n(&vq.state, 0, __ATOMIC_RELEASE);
        fprintf(stderr, "VR UI: equip: 0x1f63e00(slot %#x, inventory index %d, id %#x): %s; the slot held %#x, now %#x\n",
                vq.slot, (int)vq.inv, vq.id,
                ok            ? "done"
                : reason == 4 ? "the game-thread hook did not run (the in-game step: patch \"VR FP Camera Heading (in-game step)\" in BB_HOOKS?)"
                : reason == 6 ? "not called: a game menu is open"
                : reason == 1 ? "not called: the item is no longer at that inventory index"
                : reason == 5 ? "called, but the slot does not hold it"
                              : "not called: no living player",
                vq.before, vq.after);
        fflush(stderr);
        bbgpu_vr_ui_event(BBXR_EV_ACTION_DONE,
                          vq.slot >= 0x20 ? (int32_t)BBXR_UI_CLEAR_QUICK
                          : vq.slot >= 0x10 ? (int32_t)BBXR_UI_ASSIGN_QUICK : (int32_t)BBXR_UI_EQUIP_ITEM, ok, reason);
        if (ok && vu.then_use >= 0) { /* assigned, now used: d-pad down to it and square */
            vrui_exec_start(&vu.exec, VRUI_ACT_USE_QUICK, vu.then_use, vq.id, now);
            vu.exec_reported = 0;
        }
        vu.then_use = -1;
    }
    /* The user's choices: one at a time (a new one waits while one runs). */
    if (vu.on && !vrui_exec_busy(&vu.exec) && __atomic_load_n(&vq.state, __ATOMIC_ACQUIRE) == 0) {
        BbXrUiAction a;
        while (bbgpu_vr_ui_actions(&a, 1) == 1) {
            if (a.kind == BBXR_UI_THROW_ORIGIN || a.kind == BBXR_UI_THROW_DIR) {
                /* a thrown item's release (physical items): arms the launch hook, then the use follows */
                const int32_t v[3] = {a.a, a.b, a.c};
                if (a.kind == BBXR_UI_THROW_ORIGIN) vr_gun_throw_origin(v);
                else vr_gun_throw_dir(v);
                continue;
            }
            if (a.kind == BBXR_UI_HELD) {
                /* the bridge holds an item in a hand (a = hand or -1, b = item): handled at once, never the executor */
                static int held_hand = -1;
                static uint32_t held_item;
                if (a.a != held_hand || (a.a >= 0 && (uint32_t)a.b != held_item)) {
                    fprintf(stderr, "VR UI: held item %s (hand %d, item %#x)\n", a.a >= 0 ? "in hand" : "released", (int)a.a, (unsigned)a.b);
                    fflush(stderr);
                    held_hand = (int)a.a;
                    held_item = (uint32_t)a.b;
                }
                vr_fp_held_item((int)a.a, (uint32_t)a.b);
                continue;
            }
            if (a.kind == BBXR_UI_USE_QUICK && a.c > 0) {
                vr_gun_throw_speed(a.c); /* the thrown hand speed, cm/s (findings/39 1A ii) */
                vr_atkspeed_throw_begin(); /* ... and its animation runs faster (BB_VR_THROW_ANIM_SPEED) */
            }
            if ((a.kind == BBXR_UI_USE_QUICK && a.a >= 0 && a.a < vu.game.quick_count && a.a < VRUI_QUICK_SLOTS &&
                 vfb_is_bell(vu.game.quick_id[a.a])) ||
                ((a.kind == BBXR_UI_USE_ITEM || a.kind == BBXR_UI_PRESS) && vfb_is_bell((uint32_t)a.a)) ||
                (a.kind == BBXR_UI_ASSIGN_QUICK && a.c && vfb_is_bell((uint32_t)a.b)))
                vu_fb_arm(now); /* the dashboard uses a bell */
            if (a.kind == BBXR_UI_EQUIP_ITEM || a.kind == BBXR_UI_ASSIGN_QUICK || a.kind == BBXR_UI_CLEAR_QUICK)
                vu_request_equip(&a, now);
            else {
                vrui_exec_start(&vu.exec, (int)a.kind, (int)a.a, (uint32_t)a.b, now);
                if (a.kind == BBXR_UI_USE_ITEM || a.kind == BBXR_UI_PRESS) vu.exec.item = (uint32_t)a.a;
                vu.exec_reported = 0;
                fprintf(stderr, "VR UI: action %u (slot %d, item %#x) from the headset%s\n", a.kind, (int)a.a, (unsigned)a.b,
                        a.kind == BBXR_UI_USE_QUICK && a.c > 0 ? " (a thrown item)" : "");
                fflush(stderr);
            }
            break;
        }
    }
    const uint32_t buttons = vrui_exec_step(&vu.exec, now, g, vu.press_s);
    if (vu.exec.kind && vu.exec.finished && !vu.exec_reported) {
        vu.exec_reported = 1;
        bbgpu_vr_ui_event(BBXR_EV_ACTION_DONE, vu.exec.kind, vu.exec.finished > 0, vu.exec.reason);
        fprintf(stderr, "VR UI: action %d %s after %d press(es)%s\n", vu.exec.kind, vu.exec.finished > 0 ? "done" : "given up",
                vu.exec.presses,
                vu.exec.reason == VRUI_FAIL_NOT_FOUND   ? ": the item is no longer there"
                : vu.exec.reason == VRUI_FAIL_TIMEOUT   ? ": the game did not answer"
                : vu.exec.reason == VRUI_FAIL_NOT_ALLOWED ? ": not possible now (or an item in no quick slot)"
                                                        : "");
        fflush(stderr);
        memset(&vu.exec, 0, sizeof(vu.exec));
    }
    return buttons;
}
