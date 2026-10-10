/* vr-ui-test: src/vr_ui_game.h (the VR UI's game side, docs/vr/findings/26-game-ui.md) on a fake
 * PlayerGameData: the vtable checks, the inventory index rule (key list, then normal list), the quick
 * slots and the selected one (an inventory index), the weapon slots, the item list; then the
 * executor against a scripted game that answers d-pad down / right / left like Bloodborne (the next
 * non-empty quick slot; the other weapon slot), slow and fast, and the ways it gives up. */
#include "vr_ui_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(x)                                                                  \
    do {                                                                          \
        ++checks;                                                                 \
        if (!(x)) {                                                               \
            ++failures;                                                           \
            printf("FAIL line %d: %s\n", __LINE__, #x);                           \
        }                                                                         \
    } while (0)

/* A fake guest: one buffer at a fake address range. */
#define MEM_BASE 0x10000000ull
#define MEM_SIZE 0x20000u
#define IMAGE 0x7f0000000000ull /* host address of guest VA 0x400000 */
static unsigned char mem[MEM_SIZE];
static int fail_reads;

static int fake_read(void *user, uint64_t addr, void *out, size_t n) {
    (void)user;
    if (fail_reads || addr < MEM_BASE || addr + n > MEM_BASE + MEM_SIZE) return 0;
    memcpy(out, mem + (addr - MEM_BASE), n);
    return 1;
}
static void put32(uint64_t a, uint32_t v) { memcpy(mem + (a - MEM_BASE), &v, 4); }
static void put64(uint64_t a, uint64_t v) { memcpy(mem + (a - MEM_BASE), &v, 8); }

#define PGD (MEM_BASE + 0x1000)
#define KEY_ARRAY (MEM_BASE + 0x4000)
#define NORMAL_ARRAY (MEM_BASE + 0x8000)

static void entry(int index, uint32_t id, int32_t count, int key_cap) {
    const uint64_t at = index < key_cap ? KEY_ARRAY + 16u * (unsigned)index : NORMAL_ARRAY + 16u * (unsigned)(index - key_cap);
    put32(at, 0x800000u + (uint32_t)index); /* handle */
    put32(at + 4, id);
    put32(at + 8, (uint32_t)count);
    put32(at + 12, 0);
}

static void build(void) {
    memset(mem, 0, sizeof(mem));
    put64(PGD + VRUI_PGD_EGD, IMAGE + (VRUI_VT_EQUIP_GAME_DATA - 0x400000u));
    put64(PGD + VRUI_PGD_EID, IMAGE + (VRUI_VT_EQUIP_INVENTORY - 0x400000u));
    put64(PGD + VRUI_PGD_EIT, IMAGE + (VRUI_VT_EQUIP_ITEM - 0x400000u));
    put32(PGD + VRUI_PGD_KEY_CAP, 0x40);
    put32(PGD + VRUI_PGD_NORMAL_CAP, 0x7C0);
    put64(PGD + VRUI_PGD_KEY_ARRAY, KEY_ARRAY);
    put64(PGD + VRUI_PGD_NORMAL_ARRAY, NORMAL_ARRAY);
    entry(2, VRUI_GOODS | 4312u, 1, 0x40);   /* a key item (key list) */
    entry(0x40, VRUI_GOODS | 1000u, 12, 0x40); /* Blood Vial (normal list, first) */
    entry(0x41, VRUI_GOODS | 900u, 17, 0x40);  /* Quicksilver Bullets */
    entry(0x42, VRUI_GOODS | 1200u, 3, 0x40);  /* Molotov */
    entry(0x43, 7000000u, 1, 0x40);            /* Saw Cleaver */
    entry(0x44, 0x10000000u | 220000u, 1, 0x40); /* armor: not listed */
    entry(0x45, VRUI_GOODS | 1230u, 9, 0x40);  /* Pebble */
    put32(PGD + VRUI_PGD_TAIL, 0x45);
    /* Quick slots: vial, molotov, -, pebble, -, - */
    const int32_t quick[6] = {0x40, 0x42, -1, 0x45, -1, -1};
    for (int k = 0; k < 6; ++k) {
        put32(PGD + VRUI_PGD_QUICK + 8u * (unsigned)k, quick[k] >= 0 ? 0x900000u + (unsigned)k : 0xFFFFFFFFu);
        put32(PGD + VRUI_PGD_QUICK + 8u * (unsigned)k + 4, (uint32_t)quick[k]);
    }
    put32(PGD + VRUI_PGD_QUICK_SELECTED, 0x42); /* the Molotov */
    const uint32_t ids[4] = {14000000u, 7000000u, 110000u, 7010000u}; /* L1, R1, L2, R2 */
    for (int i = 0; i < 4; ++i) put32(PGD + VRUI_PGD_WEAPON_IDS + 4u * (unsigned)i, ids[i]);
    put32(PGD + VRUI_PGD_ACTIVE_RIGHT, 0);
    put32(PGD + VRUI_PGD_ACTIVE_LEFT, 0);
}

static void test_read(void) {
    VrUiGameData g;
    build();
    CHECK(vrui_read(fake_read, NULL, PGD, IMAGE, 1, &g));
    CHECK(g.ok);
    CHECK(g.quick_id[0] == (VRUI_GOODS | 1000u) && g.quick_count_of[0] == 12);
    CHECK(g.quick_id[1] == (VRUI_GOODS | 1200u));
    CHECK(g.quick_id[2] == VRUI_NONE);
    CHECK(g.quick_id[3] == (VRUI_GOODS | 1230u));
    CHECK(g.quick_active == 1);
    CHECK(g.weapon[0] == 7000000u && g.weapon[1] == 7010000u);
    CHECK(g.weapon[2] == 14000000u && g.weapon[3] == 110000u);
    CHECK(g.active_right == 0 && g.active_left == 0);
    CHECK(g.item_count == 6); /* key item, vial, bullets, molotov, cleaver, pebble; no armor */
    int vials = -1;
    for (int i = 0; i < g.item_count; ++i)
        if (g.items[i].id == (VRUI_GOODS | 1000u)) vials = g.items[i].count;
    CHECK(vials == 12);
    CHECK(vrui_read(fake_read, NULL, PGD, IMAGE, 0, &g) && g.item_count == 0 && !g.items_read);
    /* A wrong vtable (another object, a stale pointer): nothing is trusted. */
    put64(PGD + VRUI_PGD_EIT, IMAGE + 0x1234);
    CHECK(!vrui_read(fake_read, NULL, PGD, IMAGE, 1, &g) && !g.ok);
    build();
    put32(PGD + VRUI_PGD_TAIL, 0x900); /* past the capacity: garbage */
    CHECK(!vrui_read(fake_read, NULL, PGD, IMAGE, 1, &g));
    build();
    fail_reads = 1;
    CHECK(!vrui_read(fake_read, NULL, PGD, IMAGE, 1, &g));
    fail_reads = 0;
    CHECK(!vrui_read(fake_read, NULL, 0, IMAGE, 1, &g));
}

/* The scripted game: the d-pad acts on its press edge after `lag` seconds. */
typedef struct {
    double lag;
    int deaf;            /* never answers */
    uint32_t prev;
    double pending_at;
    uint32_t pending;
    int used;            /* square pressed with this inventory index selected */
    int32_t used_index;
} Game;

static void game_poll(Game *gm, double now, uint32_t buttons) {
    const uint32_t edge = buttons & ~gm->prev;
    gm->prev = buttons;
    if (edge & (VRUI_BTN_DOWN | VRUI_BTN_RIGHT | VRUI_BTN_LEFT)) {
        gm->pending = edge;
        gm->pending_at = now + gm->lag;
    }
    if (edge & VRUI_BTN_SQUARE) {
        ++gm->used;
        uint32_t v;
        memcpy(&v, mem + (PGD + VRUI_PGD_QUICK_SELECTED - MEM_BASE), 4);
        gm->used_index = (int32_t)v;
    }
    if (gm->pending && now >= gm->pending_at && !gm->deaf) {
        if (gm->pending & VRUI_BTN_DOWN) { /* next non-empty slot */
            uint32_t sel;
            memcpy(&sel, mem + (PGD + VRUI_PGD_QUICK_SELECTED - MEM_BASE), 4);
            int k = 0;
            for (int i = 0; i < 6; ++i) {
                uint32_t idx;
                memcpy(&idx, mem + (PGD + VRUI_PGD_QUICK + 8u * (unsigned)i + 4 - MEM_BASE), 4);
                if (idx == sel) k = i;
            }
            for (int step = 1; step <= 6; ++step) {
                uint32_t idx;
                memcpy(&idx, mem + (PGD + VRUI_PGD_QUICK + 8u * (unsigned)((k + step) % 6) + 4 - MEM_BASE), 4);
                if (idx != 0xFFFFFFFFu) {
                    put32(PGD + VRUI_PGD_QUICK_SELECTED, idx);
                    break;
                }
            }
        }
        if (gm->pending & VRUI_BTN_RIGHT) {
            uint32_t a;
            memcpy(&a, mem + (PGD + VRUI_PGD_ACTIVE_RIGHT - MEM_BASE), 4);
            put32(PGD + VRUI_PGD_ACTIVE_RIGHT, a ^ 1u);
        }
        if (gm->pending & VRUI_BTN_LEFT) {
            uint32_t a;
            memcpy(&a, mem + (PGD + VRUI_PGD_ACTIVE_LEFT - MEM_BASE), 4);
            put32(PGD + VRUI_PGD_ACTIVE_LEFT, a ^ 1u);
        }
        gm->pending = 0;
    }
}

/* Runs an action at a pad rate (s per poll) until it finishes; returns the presses of `button`. */
static int run(VrUiExec *x, Game *gm, double rate, uint32_t watch, double *took) {
    double now = 0.0;
    int presses = 0;
    uint32_t prev = 0;
    VrUiGameData g;
    for (int i = 0; i < 2000 && vrui_exec_busy(x); ++i) {
        vrui_read(fake_read, NULL, PGD, IMAGE, 0, &g);
        const uint32_t b = vrui_exec_step(x, now, &g, 0.1);
        presses += (b & watch) && !(prev & watch);
        prev = b;
        game_poll(gm, now, b);
        now += rate;
    }
    *took = now;
    return presses;
}

static void test_exec(void) {
    double took;
    for (int pass = 0; pass < 2; ++pass) {
        const double rate = pass ? 1.0 / 30.0 : 1.0 / 60.0;
        /* Molotov selected (slot 1): the pebble (slot 3) is one press away (slot 2 is empty). */
        build();
        Game gm = {0};
        gm.lag = 0.05;
        VrUiExec x;
        vrui_exec_start(&x, VRUI_ACT_USE_QUICK, 3, VRUI_GOODS | 1230u, 0.0);
        CHECK(run(&x, &gm, rate, VRUI_BTN_DOWN, &took) == 1);
        CHECK(x.finished == 1);
        CHECK(gm.used == 1 && gm.used_index == 0x45);
        /* The vial (slot 0): wraps around (molotov -> pebble -> vial), two presses. */
        build();
        memset(&gm, 0, sizeof(gm));
        gm.lag = 0.03;
        vrui_exec_start(&x, VRUI_ACT_USE_QUICK, 0, VRUI_GOODS | 1000u, 0.0);
        CHECK(run(&x, &gm, rate, VRUI_BTN_DOWN, &took) == 2);
        CHECK(x.finished == 1 && gm.used == 1 && gm.used_index == 0x40);
        CHECK(took < 1.5);
        /* Already selected: square at once. */
        build();
        memset(&gm, 0, sizeof(gm));
        vrui_exec_start(&x, VRUI_ACT_USE_QUICK, 1, VRUI_GOODS | 1200u, 0.0);
        CHECK(run(&x, &gm, rate, VRUI_BTN_DOWN, &took) == 0);
        CHECK(x.finished == 1 && gm.used == 1 && gm.used_index == 0x42);
    }
    VrUiExec x;
    Game gm;
    /* The item left the slot (used up, moved in the game's menu): gives up, nothing pressed. */
    build();
    memset(&gm, 0, sizeof(gm));
    vrui_exec_start(&x, VRUI_ACT_USE_QUICK, 3, VRUI_GOODS | 1000u, 0.0);
    CHECK(run(&x, &gm, 1.0 / 60.0, VRUI_BTN_DOWN | VRUI_BTN_SQUARE, &took) == 0);
    CHECK(x.finished == -1 && x.reason == VRUI_FAIL_NOT_FOUND && gm.used == 0);
    /* The game does not answer (a menu open, a cutscene): one press, then gives up; no square. */
    build();
    memset(&gm, 0, sizeof(gm));
    gm.deaf = 1;
    vrui_exec_start(&x, VRUI_ACT_USE_QUICK, 3, VRUI_GOODS | 1230u, 0.0);
    CHECK(run(&x, &gm, 1.0 / 60.0, VRUI_BTN_DOWN, &took) == 1);
    CHECK(x.finished == -1 && x.reason == VRUI_FAIL_TIMEOUT && gm.used == 0);
    CHECK(took < 1.2);
    /* Equip the right hand's second weapon: one d-pad right. */
    build();
    memset(&gm, 0, sizeof(gm));
    gm.lag = 0.2;
    vrui_exec_start(&x, VRUI_ACT_EQUIP, 1, 7010000u, 0.0);
    CHECK(run(&x, &gm, 1.0 / 30.0, VRUI_BTN_RIGHT, &took) == 1);
    CHECK(x.finished == 1);
    uint32_t a;
    memcpy(&a, mem + (PGD + VRUI_PGD_ACTIVE_RIGHT - MEM_BASE), 4);
    CHECK(a == 1);
    /* The left hand's first (already active): nothing to press. */
    vrui_exec_start(&x, VRUI_ACT_EQUIP, 2, 14000000u, 0.0);
    CHECK(run(&x, &gm, 1.0 / 30.0, VRUI_BTN_LEFT, &took) == 0 && x.finished == 1);
    /* An item in no quick slot: not done (no safe button for it). */
    vrui_exec_start(&x, VRUI_ACT_USE_ITEM, 0, VRUI_GOODS | 900u, 0.0);
    CHECK(run(&x, &gm, 1.0 / 30.0, 0xFFFFFFFFu, &took) == 0);
    CHECK(x.finished == -1 && x.reason == VRUI_FAIL_NOT_ALLOWED);
    /* Select only: the d-pad to the slot, no square. */
    build();
    memset(&gm, 0, sizeof(gm));
    vrui_exec_start(&x, VRUI_ACT_SELECT_QUICK, 0, VRUI_GOODS | 1000u, 0.0);
    CHECK(run(&x, &gm, 1.0 / 30.0, VRUI_BTN_DOWN, &took) == 2);
    CHECK(x.finished == 1 && gm.used == 0);
    /* A plain press (the dashboard's Options): held 0.1 s, whatever the game's state. */
    VrUiGameData nothing;
    memset(&nothing, 0, sizeof(nothing));
    vrui_exec_start(&x, VRUI_ACT_PRESS, 0, 0x8u, 0.0);
    CHECK(vrui_exec_step(&x, 0.0, &nothing, 0.1) == 0x8u);
    CHECK(vrui_exec_step(&x, 0.05, &nothing, 0.1) == 0x8u);
    CHECK(vrui_exec_step(&x, 0.11, &nothing, 0.1) == 0);
    CHECK(vrui_exec_busy(&x));
    CHECK(vrui_exec_step(&x, 0.25, &nothing, 0.1) == 0 && x.finished == 1);
    /* The player gone (loading): gives up. */
    VrUiGameData none;
    memset(&none, 0, sizeof(none));
    vrui_exec_start(&x, VRUI_ACT_USE_QUICK, 0, VRUI_GOODS | 1000u, 0.0);
    CHECK(vrui_exec_step(&x, 0.0, &none, 0.1) == 0 && x.finished == -1);
}

int main(void) {
    test_read();
    test_exec();
    printf("vr-ui-test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
