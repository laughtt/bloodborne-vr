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
 * Both debug features need the patch "VR FP Camera Heading (in-game step)" in BB_HOOKS.
 * Works with BB_VR=0 too (flat tests: the probe and the pad presses, no headset). */
#define _GNU_SOURCE
#include "runtime.h"
#include "hooks.h"
#include "gpu/bbgpu.h"
#include "gpu/xr_bridge/bbport_xr_bridge.h"
#include "vr_ui_game.h"
#include "vr_hitbox.h"
#include "vr_gun.h"
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
    int dbg;              /* BB_DEBUG_GIVE or BB_DEBUG_USE or BB_DEBUG_USE_FILE is set */
} vu;

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

#define vu_debug_give_step() ((void)0)
#define vu_debug_speffect_step() ((void)0)

static ABI void vu_hook_menu(BbHookContext *ctx) {
    (void)ctx;
    if ((!vu.on && !vu.dbg) || !vu.image) return;
    __atomic_store_n(&vu_hook_us, (uint64_t)(vu_now() * 1e6) + 1u, __ATOMIC_RELEASE);
    vu_debug_give_step();
    vu_debug_speffect_step();
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
#define vu_debug_use(now, player) ((void)0)

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
    if (vrui_exec_busy(&vu.exec) || __atomic_load_n(&vq.state, __ATOMIC_ACQUIRE)) s->flags |= BBXR_GAME_BUSY;
    {
        const uint64_t seen_us = __atomic_load_n(&vu_hook_us, __ATOMIC_ACQUIRE);
        const double seen = seen_us ? (double)(seen_us - 1u) * 1e-6 : -1.0;
        if (seen >= 0.0 && now - seen < 2.0) s->flags |= BBXR_GAME_CAN_EQUIP;
    }
    if (vu.probe) vu_probe_log(now, player);
    vu_swap_log(now, buttons_in);
    if (!vu.on && !vu.dbg) return 0;
    if (vu.on) {
        vr_hitbox_fill(&s->debug, vu.image); /* the hitbox overlay (findings/29) */
        bbgpu_vr_ui_state(s, (uint32_t)sizeof(*s));
    }
    if (vu.dbg) vu_debug_use(now, player);

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
