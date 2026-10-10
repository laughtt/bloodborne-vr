/* The game side of the VR UI (BB_VR_UI=1, docs/vr/findings/26-game-ui.md): where the player's items,
 * quick slots and weapon slots live, read only, and the executor that carries out a choice made in
 * the headset menus with the game's own buttons. C11, header-only, no I/O: the reader takes a read
 * callback (the guarded guest reads of src/runtime_vr_ui.c, or a fake memory in tests/test_vr_ui.c).
 *
 * Memory (static analysis of the eboot, findings/26 section 2; PS4 VAs in patch-XML space):
 *   GameDataMan = [0x593B130]; PlayerGameData PGD = [GameDataMan + 0x8] (also PlayerIns vfunc +0x1C8).
 *   PGD+0x1D0 EquipGameData (vtable 0x576F130), PGD+0x328 EquipInventoryData (0x576F320),
 *   PGD+0x3D0 EquipItemData (0x576F360): the three vtables are checked before anything is trusted.
 *   Inventory (EquipInventoryData): key capacity u32 PGD+0x34C, key array ptr PGD+0x380, normal
 *     array ptr PGD+0x370, normal capacity u32 PGD+0x33C, tail index s32 PGD+0x3B0. Index i is
 *     key[i] when i < keyCap, else normal[i - keyCap]. Entry, 16 bytes: u32 handle (0 = empty),
 *     u32 item id (category bits: weapon 0, armor 0x10000000, accessory 0x20000000, goods
 *     0x40000000), s32 quantity, u32 sort key.
 *   Quick items (EquipItemData): 6 slots of {u32, s32 inventory index (-1 empty)} at PGD+0x3E8; the
 *     selected item is the s32 INVENTORY INDEX at PGD+0x418 (the slot is the one holding it). D-pad
 *     down (0x18df170) steps to the next non-empty slot, wrapping; square uses the selected one.
 *   Weapons (ChrAsm, PGD+0x240): ids u32[16] at PGD+0x2A4 (0 left 1, 1 right 1, 2 left 2, 3 right 2),
 *     active right slot s32 PGD+0x250, active left slot s32 PGD+0x24C (0/1). D-pad right/left swap.
 * Nothing is ever written. */
#ifndef VR_UI_GAME_H
#define VR_UI_GAME_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VRUI_VA_GAME_DATA_MAN 0x593B130u
#define VRUI_VT_EQUIP_GAME_DATA 0x576F130u
#define VRUI_VT_EQUIP_INVENTORY 0x576F320u
#define VRUI_VT_EQUIP_ITEM 0x576F360u
#define VRUI_PGD_EGD 0x1D0u
#define VRUI_PGD_EID 0x328u
#define VRUI_PGD_EIT 0x3D0u
#define VRUI_PGD_NORMAL_CAP 0x33Cu
#define VRUI_PGD_KEY_CAP 0x34Cu
#define VRUI_PGD_NORMAL_ARRAY 0x370u
#define VRUI_PGD_KEY_ARRAY 0x380u
#define VRUI_PGD_TAIL 0x3B0u
#define VRUI_PGD_QUICK 0x3E8u
#define VRUI_PGD_QUICK_SELECTED 0x418u
#define VRUI_PGD_ACTIVE_LEFT 0x24Cu
#define VRUI_PGD_ACTIVE_RIGHT 0x250u
#define VRUI_PGD_WEAPON_IDS 0x2A4u
#define VRUI_QUICK_SLOTS 6
#define VRUI_MAX_ITEMS 320
#define VRUI_NONE 0xFFFFFFFFu
#define VRUI_GOODS 0x40000000u

/* Reads n bytes of guest memory at addr into out; 0 when it is not readable. */
typedef int (*VrUiRead)(void *user, uint64_t addr, void *out, size_t n);

typedef struct {
    uint32_t id;
    int32_t count;
    int32_t index; /* inventory index */
} VrUiItem;

typedef struct {
    int ok;                     /* PGD found and its three vtables match */
    uint64_t pgd;
    int32_t selected_index;     /* PGD+0x418 */
    int quick_count;            /* slots read (6) */
    int32_t quick_index[VRUI_QUICK_SLOTS];
    uint32_t quick_id[VRUI_QUICK_SLOTS]; /* VRUI_NONE: empty (or its entry unreadable) */
    int32_t quick_count_of[VRUI_QUICK_SLOTS];
    int quick_active;           /* the slot whose index is the selected one, -1 none */
    uint32_t weapon[4];         /* right 1, right 2, left 1, left 2 */
    int32_t active_right, active_left;
    int items_read;             /* the item list below was read this time */
    int item_count;
    VrUiItem items[VRUI_MAX_ITEMS];
    int32_t key_cap, normal_cap, tail;
} VrUiGameData;

static inline int vrui_rd32(VrUiRead rd, void *u, uint64_t a, uint32_t *v) { return rd(u, a, v, 4); }
static inline int vrui_rd64(VrUiRead rd, void *u, uint64_t a, uint64_t *v) { return rd(u, a, v, 8); }

/* The 16-byte entry of inventory index i (0 when out of range or unreadable). */
static inline int vrui_entry(VrUiRead rd, void *u, const VrUiGameData *g, uint64_t key_array,
                             uint64_t normal_array, int32_t i, uint32_t entry[4]) {
    if (i < 0 || i > g->tail || i >= g->key_cap + g->normal_cap) return 0;
    const uint64_t at = i < g->key_cap ? key_array + 16u * (uint64_t)i
                                       : normal_array + 16u * (uint64_t)(i - g->key_cap);
    if (!(i < g->key_cap ? key_array : normal_array)) return 0;
    if (!rd(u, at, entry, 16)) return 0;
    return entry[0] != 0;
}

/* Reads the player's quick slots, weapon slots and (with_items) every inventory entry. `pgd` is
 * the PlayerGameData pointer, `image` the host address of guest VA 0x400000 (for the vtables). */
static inline int vrui_read(VrUiRead rd, void *u, uint64_t pgd, uint64_t image, int with_items,
                            VrUiGameData *g) {
    memset(g, 0, sizeof(*g));
    g->quick_active = -1;
    for (int i = 0; i < 4; ++i) g->weapon[i] = VRUI_NONE;
    for (int i = 0; i < VRUI_QUICK_SLOTS; ++i) g->quick_id[i] = VRUI_NONE, g->quick_index[i] = -1;
    g->pgd = pgd;
    uint64_t vt[3];
    if (!pgd || !vrui_rd64(rd, u, pgd + VRUI_PGD_EGD, &vt[0]) ||
        !vrui_rd64(rd, u, pgd + VRUI_PGD_EID, &vt[1]) || !vrui_rd64(rd, u, pgd + VRUI_PGD_EIT, &vt[2]))
        return 0;
    if (vt[0] != image + (VRUI_VT_EQUIP_GAME_DATA - 0x400000u) ||
        vt[1] != image + (VRUI_VT_EQUIP_INVENTORY - 0x400000u) ||
        vt[2] != image + (VRUI_VT_EQUIP_ITEM - 0x400000u))
        return 0;
    uint32_t v;
    uint64_t key_array = 0, normal_array = 0;
    if (!vrui_rd32(rd, u, pgd + VRUI_PGD_KEY_CAP, &v)) return 0;
    g->key_cap = (int32_t)v;
    if (!vrui_rd32(rd, u, pgd + VRUI_PGD_NORMAL_CAP, &v)) return 0;
    g->normal_cap = (int32_t)v;
    if (!vrui_rd32(rd, u, pgd + VRUI_PGD_TAIL, &v)) return 0;
    g->tail = (int32_t)v;
    if (!vrui_rd64(rd, u, pgd + VRUI_PGD_KEY_ARRAY, &key_array) ||
        !vrui_rd64(rd, u, pgd + VRUI_PGD_NORMAL_ARRAY, &normal_array))
        return 0;
    if (g->key_cap < 0 || g->key_cap > 0x1000 || g->normal_cap < 0 || g->normal_cap > 0x2000 ||
        g->tail < -1 || g->tail >= g->key_cap + g->normal_cap)
        return 0;
    if (!vrui_rd32(rd, u, pgd + VRUI_PGD_QUICK_SELECTED, &v)) return 0;
    g->selected_index = (int32_t)v;
    g->quick_count = VRUI_QUICK_SLOTS;
    for (int k = 0; k < VRUI_QUICK_SLOTS; ++k) {
        uint32_t slot[2];
        if (!rd(u, pgd + VRUI_PGD_QUICK + 8u * (uint64_t)k, slot, 8)) return 0;
        g->quick_index[k] = (int32_t)slot[1];
        uint32_t e[4];
        if (g->quick_index[k] >= 0 &&
            vrui_entry(rd, u, g, key_array, normal_array, g->quick_index[k], e)) {
            g->quick_id[k] = e[1];
            g->quick_count_of[k] = (int32_t)e[2];
            if (g->quick_index[k] == g->selected_index && g->quick_active < 0) g->quick_active = k;
        }
    }
    uint32_t ids[4];
    if (!rd(u, pgd + VRUI_PGD_WEAPON_IDS, ids, sizeof(ids))) return 0;
    g->weapon[0] = ids[1]; /* right 1 */
    g->weapon[1] = ids[3]; /* right 2 */
    g->weapon[2] = ids[0]; /* left 1 */
    g->weapon[3] = ids[2]; /* left 2 */
    if (!vrui_rd32(rd, u, pgd + VRUI_PGD_ACTIVE_RIGHT, &v)) return 0;
    g->active_right = (int32_t)v;
    if (!vrui_rd32(rd, u, pgd + VRUI_PGD_ACTIVE_LEFT, &v)) return 0;
    g->active_left = (int32_t)v;
    if (with_items) {
        g->items_read = 1;
        for (int32_t i = 0; i <= g->tail && g->item_count < VRUI_MAX_ITEMS; ++i) {
            uint32_t e[4];
            if (!vrui_entry(rd, u, g, key_array, normal_array, i, e)) continue;
            const uint32_t category = e[1] & 0xF0000000u;
            if (category != VRUI_GOODS && category != 0) continue; /* armor, accessories */
            g->items[g->item_count].id = e[1];
            g->items[g->item_count].count = (int32_t)e[2];
            g->items[g->item_count].index = i;
            ++g->item_count;
        }
    }
    g->ok = 1;
    return 1;
}

/* ---- The executor: a choice from the headset as the game's own button presses ----
 * USE_QUICK slot k: d-pad down (one press, then wait until the selected slot changes) until slot k
 * is the selected one, then square. SELECT_QUICK: the same without the square. PRESS: one press of
 * the given buttons (the dashboard's Options, Gestures, d-pad), whatever the game's state. EQUIP weapon slot s: d-pad right (s 0/1) or left (s 2/3) until
 * that hand's active slot is s's. Each press is held press_s and followed by press_s released; a
 * press the game does not answer within 0.8 s, more presses than slots, the item leaving the slot or
 * the player leaving the world gives up. */
enum { VRUI_EXEC_IDLE = 0, VRUI_EXEC_PRESS, VRUI_EXEC_RELEASE, VRUI_EXEC_USE, VRUI_EXEC_USE_RELEASE };
enum { VRUI_ACT_USE_QUICK = 1, VRUI_ACT_USE_ITEM = 2, VRUI_ACT_EQUIP = 3, VRUI_ACT_SELECT_QUICK = 4,
       VRUI_ACT_PRESS = 5 /* one press of the button bits in `item` */ };
enum { VRUI_FAIL_NONE = 0, VRUI_FAIL_NOT_FOUND = 1, VRUI_FAIL_TIMEOUT = 2, VRUI_FAIL_NOT_ALLOWED = 3 };
#define VRUI_BTN_RIGHT 0x20u
#define VRUI_BTN_DOWN 0x40u
#define VRUI_BTN_LEFT 0x80u
#define VRUI_BTN_SQUARE 0x8000u
#define VRUI_ANSWER_S 0.8

typedef struct {
    int kind;        /* VRUI_ACT_*, 0 idle */
    int slot;
    uint32_t item;
    int phase;       /* VRUI_EXEC_* */
    double since;    /* the phase's start */
    int presses;
    int32_t before;  /* the selection when the last press started */
    int finished;    /* 1 done, -1 gave up (reason), 0 still running; set once, read by the caller */
    int reason;
} VrUiExec;

static inline void vrui_exec_start(VrUiExec *x, int kind, int slot, uint32_t item, double now) {
    memset(x, 0, sizeof(*x));
    x->kind = kind;
    x->slot = slot;
    x->item = item;
    x->phase = VRUI_EXEC_IDLE;
    x->since = now;
}

static inline int vrui_exec_busy(const VrUiExec *x) { return x->kind != 0 && x->finished == 0; }

static inline void vrui_exec_finish(VrUiExec *x, int ok, int reason) {
    x->finished = ok ? 1 : -1;
    x->reason = reason;
    x->phase = VRUI_EXEC_IDLE;
}

/* What the hand selection is now: the quick slot, or the active weapon slot of the target hand. */
static inline int32_t vrui_exec_current(const VrUiExec *x, const VrUiGameData *g) {
    if (x->kind == VRUI_ACT_USE_QUICK || x->kind == VRUI_ACT_SELECT_QUICK) return g->quick_active;
    return x->slot < 2 ? g->active_right : g->active_left;
}

/* One pad poll: the buttons to press now (0 when idle or done). */
static inline uint32_t vrui_exec_step(VrUiExec *x, double now, const VrUiGameData *g, double press_s) {
    if (!vrui_exec_busy(x)) return 0;
    if (x->kind == VRUI_ACT_PRESS) { /* a plain press: held press_s, then released press_s */
        if (x->phase == VRUI_EXEC_IDLE) {
            x->phase = VRUI_EXEC_PRESS;
            x->since = now;
            x->presses = 1;
        }
        if (x->phase == VRUI_EXEC_PRESS) {
            if (now - x->since < press_s) return x->item;
            x->phase = VRUI_EXEC_RELEASE;
            x->since = now;
        }
        if (now - x->since >= press_s) vrui_exec_finish(x, 1, VRUI_FAIL_NONE);
        return 0;
    }
    if (!g->ok) {
        vrui_exec_finish(x, 0, VRUI_FAIL_NOT_ALLOWED);
        return 0;
    }
    if (x->kind == VRUI_ACT_USE_ITEM) { /* an item in no quick slot: no button does that safely */
        vrui_exec_finish(x, 0, VRUI_FAIL_NOT_ALLOWED);
        return 0;
    }
    const int quick = x->kind == VRUI_ACT_USE_QUICK || x->kind == VRUI_ACT_SELECT_QUICK;
    if (quick ? x->slot < 0 || x->slot >= VRUI_QUICK_SLOTS || g->quick_id[x->slot] != x->item
              : x->slot < 0 || x->slot > 3 || g->weapon[x->slot] != x->item) {
        vrui_exec_finish(x, 0, VRUI_FAIL_NOT_FOUND);
        return 0;
    }
    const int32_t want = quick ? x->slot : (x->slot & 1);
    const int32_t current = vrui_exec_current(x, g);
    const uint32_t step = quick ? VRUI_BTN_DOWN : x->slot < 2 ? VRUI_BTN_RIGHT : VRUI_BTN_LEFT;
    const int max_presses = quick ? VRUI_QUICK_SLOTS : 3;
    switch (x->phase) {
    case VRUI_EXEC_IDLE:
        if (current == want) {
            if (x->kind != VRUI_ACT_USE_QUICK) { /* equip, select: there is nothing more to press */
                vrui_exec_finish(x, 1, VRUI_FAIL_NONE);
                return 0;
            }
            x->phase = VRUI_EXEC_USE;
            x->since = now;
            return VRUI_BTN_SQUARE;
        }
        if (x->presses >= max_presses) {
            vrui_exec_finish(x, 0, VRUI_FAIL_TIMEOUT);
            return 0;
        }
        ++x->presses;
        x->before = current;
        x->phase = VRUI_EXEC_PRESS;
        x->since = now;
        return step;
    case VRUI_EXEC_PRESS:
        if (now - x->since < press_s) return step;
        x->phase = VRUI_EXEC_RELEASE;
        x->since = now;
        return 0;
    case VRUI_EXEC_RELEASE:
        /* Released at least press_s, and the game answered (the selection moved). */
        if (now - x->since >= press_s && current != x->before) {
            x->phase = VRUI_EXEC_IDLE;
            return vrui_exec_step(x, now, g, press_s);
        }
        if (now - x->since > VRUI_ANSWER_S) vrui_exec_finish(x, 0, VRUI_FAIL_TIMEOUT);
        return 0;
    case VRUI_EXEC_USE:
        if (now - x->since < press_s) return VRUI_BTN_SQUARE;
        x->phase = VRUI_EXEC_USE_RELEASE;
        x->since = now;
        return 0;
    case VRUI_EXEC_USE_RELEASE:
        if (now - x->since >= press_s) vrui_exec_finish(x, 1, VRUI_FAIL_NONE);
        return 0;
    default:
        vrui_exec_finish(x, 0, VRUI_FAIL_NOT_ALLOWED);
        return 0;
    }
}

#endif
