/* Tests of the "misses are free" stamina tickets (src/vr_stamina_tickets.h), the rules
 * src/runtime_vr_stamina.c applies in the game's cost and hit hooks (docs/vr/findings/20 6.2 G3),
 * the same as the sandbox's swing::StaminaModel: a miss is free, the first hit of an attack pays
 * its cost once, the -50 floor, the debug flags, the player-only filter, refund mode.
 * Build: ninja -C out/gpu vr-stamina-test (no GPU, game or headset). Prints PASS lines, exits 0.
 * tests/test_vr_stamina_cpp.cpp builds the same tests as C++. */
#include <stdio.h>
#include <string.h>
#include "vr_stamina_tickets.h"

static int failures, checks;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

#define PLAYER 0x1000u
#define ENEMY 0x2000u
#define SAW_R1 20          /* Saw Cleaver R1: BehaviorParam_PC stamina (findings/20 1.4) */
#define ATK_R1 100000
#define ATK_R1B 100010
#define W VRST_DEFAULT_WINDOW

/* The cost hook as runtime_vr_stamina.c runs it: filter, then open. Returns the request's delta as
 * the game will apply it (and applies it, as the game's clamp would, unless suppressed). */
static int32_t attack(VrStTickets *k, int mode, int32_t atk, int32_t cost, int32_t *sp, int32_t max,
                      int suppressed, double t) {
    int32_t delta = -cost;
    const int32_t c = vrst_cost_of(PLAYER, PLAYER, delta, 1, 0);
    if (c) vrst_open(k, mode, atk, c, sp, max, suppressed, t, W, &delta, NULL);
    if (delta < 0 && !suppressed) *sp = vrst_clamp((int64_t)*sp + delta, max); /* the game's write */
    return delta;
}

static void test_miss_free(void) {
    VrStTickets k;
    vrst_reset(&k);
    int32_t sp = 91;
    const int32_t delta = attack(&k, VRST_DEFER, ATK_R1, SAW_R1, &sp, 91, 0, 0.0);
    CHECK(delta == 0);              /* the game adds nothing at the start */
    CHECK(sp == 91);
    CHECK(k.n == 1 && k.t[0].cost == SAW_R1 && k.t[0].atk_id == ATK_R1);
    CHECK(vrst_pending(&k) == SAW_R1);
    VrStEvent ev[4];
    CHECK(vrst_expire(&k, VRST_DEFER, 0.5, &sp, 91, ev, 4) == 0); /* still open */
    CHECK(vrst_expire(&k, VRST_DEFER, W + 0.01, &sp, 91, ev, 4) == 1);
    CHECK(ev[0].kind == VRST_EV_EXPIRED && ev[0].amount == 0);
    CHECK(sp == 91 && k.n == 0);
    /* 20 misses in a row: still full */
    for (int i = 0; i < 20; ++i) {
        attack(&k, VRST_DEFER, ATK_R1, SAW_R1, &sp, 91, 0, 10.0 + i);
        vrst_expire(&k, VRST_DEFER, 10.0 + i + 0.9, &sp, 91, NULL, 0);
    }
    CHECK(sp == 91 && k.n == 0);
    printf("PASS miss free: 21 misses, stamina %d of 91\n", sp);
}

static void test_single_charge_multi_hit(void) {
    VrStTickets k;
    vrst_reset(&k);
    int32_t sp = 91;
    attack(&k, VRST_DEFER, ATK_R1, SAW_R1, &sp, 91, 0, 0.0);
    VrStEvent e = vrst_hit(&k, VRST_DEFER, ATK_R1, 1, 0.37, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_CHARGED && e.amount == SAW_R1 && e.ticket.by_id);
    CHECK(sp == 71);
    /* a sweep through two more enemies, and a wall: same attack, free */
    e = vrst_hit(&k, VRST_DEFER, ATK_R1, 1, 0.38, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_REPEAT && e.amount == 0);
    e = vrst_hit(&k, VRST_DEFER, ATK_R1, 1, 0.40, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_REPEAT);
    CHECK(sp == 71);
    CHECK(vrst_pending(&k) == 0);
    /* the window ends: the hit ticket closes, nothing given back */
    VrStEvent ev[2];
    CHECK(vrst_expire(&k, VRST_DEFER, 1.0, &sp, 91, ev, 2) == 1);
    CHECK(ev[0].kind == VRST_EV_KEPT && ev[0].amount == SAW_R1 && ev[0].ticket.hits == 3);
    CHECK(sp == 71);
    /* 20 hits, one per attack: exactly 20 x cost (clamped at the floor) */
    sp = 500;
    for (int i = 0; i < 20; ++i) {
        const double t = 10.0 + i;
        attack(&k, VRST_DEFER, ATK_R1, SAW_R1, &sp, 500, 0, t);
        vrst_hit(&k, VRST_DEFER, ATK_R1, 1, t + 0.4, &sp, 500, 0);
        vrst_hit(&k, VRST_DEFER, ATK_R1, 1, t + 0.45, &sp, 500, 0);
        vrst_expire(&k, VRST_DEFER, t + 0.9, &sp, 500, NULL, 0);
    }
    CHECK(sp == 500 - 20 * SAW_R1);
    /* a combo: R1 #2 opens while R1 #1's window is still open; each hit pays its own ticket */
    vrst_reset(&k);
    sp = 91;
    attack(&k, VRST_DEFER, ATK_R1, SAW_R1, &sp, 91, 0, 0.0);
    attack(&k, VRST_DEFER, ATK_R1B, 24, &sp, 91, 0, 0.5);
    e = vrst_hit(&k, VRST_DEFER, ATK_R1, 1, 0.55, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_CHARGED && e.ticket.atk_id == ATK_R1 && sp == 71);
    e = vrst_hit(&k, VRST_DEFER, ATK_R1B, 1, 0.9, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_CHARGED && e.ticket.atk_id == ATK_R1B && sp == 47);
    printf("PASS one charge per attack: sweep 3 hits -> -%d once; 20 attacks x 2 hits -> -%d\n", SAW_R1, 20 * SAW_R1);
}

static void test_matching(void) {
    VrStTickets k;
    vrst_reset(&k);
    int32_t sp = 91;
    attack(&k, VRST_DEFER, ATK_R1, SAW_R1, &sp, 91, 0, 0.0);
    /* an AtkParam id the ticket does not know: the id-only rule leaves it free... */
    VrStEvent e = vrst_hit(&k, VRST_DEFER, 555, 0, 0.4, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_UNMATCHED && sp == 91);
    /* ...the sandbox's rule takes the newest open ticket */
    e = vrst_hit(&k, VRST_DEFER, 555, 1, 0.4, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_CHARGED && !e.ticket.by_id && e.ticket.hit_atk_id == 555 && sp == 71);
    /* no ticket at all (a free attack, a hit after the window): unmatched, free */
    e = vrst_hit(&k, VRST_DEFER, ATK_R1, 1, 5.0, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_UNMATCHED && sp == 71);
    printf("PASS matching: by AtkParam id, newest-ticket fallback, unmatched free\n");
}

static void test_expiry(void) {
    VrStTickets k;
    vrst_reset(&k);
    int32_t sp = 91;
    attack(&k, VRST_DEFER, ATK_R1, SAW_R1, &sp, 91, 0, 0.0);
    /* a hit after the window does not match the ticket, even before the tick closes it */
    VrStEvent e = vrst_hit(&k, VRST_DEFER, ATK_R1, 1, W + 0.05, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_UNMATCHED && sp == 91);
    /* a hit exactly at the end still counts */
    vrst_reset(&k);
    attack(&k, VRST_DEFER, ATK_R1, SAW_R1, &sp, 91, 0, 0.0);
    e = vrst_hit(&k, VRST_DEFER, ATK_R1, 1, W, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_CHARGED && sp == 71);
    /* more attacks than tickets: the oldest closes (free) to make room */
    vrst_reset(&k);
    sp = 91;
    VrStEvent ev;
    for (int i = 0; i < VRST_MAX_TICKETS; ++i)
        vrst_open(&k, VRST_DEFER, 1000 + i, 10, &sp, 91, 0, 0.01 * i, W, NULL, &ev);
    CHECK(k.n == VRST_MAX_TICKETS && ev.kind == VRST_EV_NONE);
    vrst_open(&k, VRST_DEFER, 2000, 10, &sp, 91, 0, 0.1, W, NULL, &ev);
    CHECK(k.n == VRST_MAX_TICKETS && ev.kind == VRST_EV_EXPIRED && ev.ticket.atk_id == 1000);
    CHECK(k.t[0].atk_id == 1001 && k.t[VRST_MAX_TICKETS - 1].atk_id == 2000 && sp == 91);
    /* ids are unique and non-zero */
    CHECK(k.t[0].id != 0 && k.t[0].id != k.t[1].id);
    printf("PASS expiry: window %.2f s, late hit free, eviction free\n", W);
}

static void test_refund(void) {
    VrStTickets k;
    vrst_reset(&k);
    int32_t sp = 91;
    /* the game pays at the start, as in vanilla */
    int32_t delta = attack(&k, VRST_REFUND, ATK_R1, SAW_R1, &sp, 91, 0, 0.0);
    CHECK(delta == -SAW_R1 && sp == 71 && k.t[0].paid == SAW_R1);
    VrStEvent ev[2];
    CHECK(vrst_expire(&k, VRST_REFUND, 1.0, &sp, 91, ev, 2) == 1);
    CHECK(ev[0].kind == VRST_EV_REFUNDED && ev[0].amount == SAW_R1 && sp == 91);
    /* a hit keeps it */
    attack(&k, VRST_REFUND, ATK_R1, SAW_R1, &sp, 91, 0, 2.0);
    VrStEvent e = vrst_hit(&k, VRST_REFUND, ATK_R1, 1, 2.4, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_PAID && e.amount == SAW_R1 && sp == 71);
    vrst_expire(&k, VRST_REFUND, 3.0, &sp, 91, NULL, 0);
    CHECK(sp == 71);
    /* the refund is what the clamp let the game take, not the whole cost: 10 - 75 -> -50 (60 taken) */
    sp = 10;
    attack(&k, VRST_REFUND, ATK_R1, 75, &sp, 91, 0, 4.0);
    CHECK(sp == -50 && k.t[0].paid == 60);
    vrst_expire(&k, VRST_REFUND, 5.0, &sp, 91, ev, 2);
    CHECK(ev[0].amount == 60 && sp == 10);
    /* refund is capped at the maximum (regen during the window is paused by the game, but not
     * always: a SpEffect) */
    attack(&k, VRST_REFUND, ATK_R1, SAW_R1, &sp, 91, 0, 6.0);
    sp = 85;
    vrst_expire(&k, VRST_REFUND, 7.0, &sp, 91, NULL, 0);
    CHECK(sp == 91);
    printf("PASS refund: miss given back, hit kept, clamped amount -50 -> back to 10\n");
}

static void test_floor(void) {
    CHECK(vrst_clamp(-49, 91) == -49);
    CHECK(vrst_clamp(-50, 91) == -50);
    CHECK(vrst_clamp(-51, 91) == -50);
    CHECK(vrst_clamp(-100000, 91) == -50);
    CHECK(vrst_clamp(92, 91) == 91);
    CHECK(vrst_clamp(91, 91) == 91);
    VrStTickets k;
    vrst_reset(&k);
    int32_t sp = 5;
    attack(&k, VRST_DEFER, ATK_R1, 75, &sp, 91, 0, 0.0);
    VrStEvent e = vrst_hit(&k, VRST_DEFER, ATK_R1, 1, 0.3, &sp, 91, 0);
    CHECK(sp == -50 && e.amount == 55 && e.ticket.cost == 75);
    /* already at the floor: a hit takes nothing more */
    attack(&k, VRST_DEFER, ATK_R1B, SAW_R1, &sp, 91, 0, 1.0);
    e = vrst_hit(&k, VRST_DEFER, ATK_R1B, 1, 1.3, &sp, 91, 0);
    CHECK(sp == -50 && e.kind == VRST_EV_CHARGED && e.amount == 0);
    printf("PASS floor: 5 - 75 -> -50 (55 taken), never below\n");
}

static void test_debug_flags(void) {
    CHECK(!vrst_suppressed(0, 0));
    CHECK(vrst_suppressed(0x80, 0));
    CHECK(vrst_suppressed(0x480, 0));
    CHECK(!vrst_suppressed(0x7F | 0x100, 0)); /* No MP Consume etc.: not stamina */
    CHECK(vrst_suppressed(0, 1));
    VrStTickets k;
    vrst_reset(&k);
    int32_t sp = 91;
    attack(&k, VRST_DEFER, ATK_R1, SAW_R1, &sp, 91, 1, 0.0);
    VrStEvent e = vrst_hit(&k, VRST_DEFER, ATK_R1, 1, 0.4, &sp, 91, vrst_suppressed(0x80, 0));
    CHECK(e.kind == VRST_EV_SUPPRESSED && e.amount == 0 && sp == 91 && e.ticket.suppressed);
    /* refund: the game skipped the cost, so nothing is given back */
    vrst_reset(&k);
    attack(&k, VRST_REFUND, ATK_R1, SAW_R1, &sp, 91, 1, 0.0);
    CHECK(sp == 91 && k.t[0].paid == 0 && k.t[0].suppressed);
    VrStEvent ev[1];
    vrst_expire(&k, VRST_REFUND, 1.0, &sp, 91, ev, 1);
    CHECK(ev[0].kind == VRST_EV_REFUNDED && ev[0].amount == 0 && sp == 91);
    printf("PASS debug flags: module bit 0x80 and the global byte suppress the charge\n");
}

static void test_player_only(void) {
    CHECK(vrst_cost_of(PLAYER, PLAYER, -20, 1, 0) == 20);
    CHECK(vrst_cost_of(ENEMY, PLAYER, -20, 1, 0) == 0);    /* an enemy's attack */
    CHECK(vrst_cost_of(PLAYER, 0, -20, 1, 0) == 0);        /* no player (loading) */
    CHECK(vrst_cost_of(0, 0, -20, 1, 0) == 0);
    CHECK(vrst_cost_of(PLAYER, PLAYER, -20, 1, 1) == 0);   /* a bullet behavior: vanilla */
    CHECK(vrst_cost_of(PLAYER, PLAYER, -20, 1, 2) == 0);   /* a SpEffect behavior */
    CHECK(vrst_cost_of(PLAYER, PLAYER, -20, 0, 0) == 0);   /* no BehaviorParam row */
    CHECK(vrst_cost_of(PLAYER, PLAYER, 0, 1, 0) == 0);     /* no cost */
    CHECK(vrst_cost_of(PLAYER, PLAYER, 15, 1, 0) == 0);    /* a gain */
    CHECK(vrst_cost_of(PLAYER, PLAYER, INT32_MIN, 1, 0) == 0);
    /* an enemy's request: no ticket, the game's own delta untouched */
    VrStTickets k;
    vrst_reset(&k);
    int32_t delta = -30, sp = 91;
    const int32_t c = vrst_cost_of(ENEMY, PLAYER, delta, 1, 0);
    if (c) vrst_open(&k, VRST_DEFER, ATK_R1, c, &sp, 91, 0, 0.0, W, &delta, NULL);
    CHECK(k.n == 0 && delta == -30);
    printf("PASS player only: enemies, bullets, SpEffects, gains untouched\n");
}

static void test_modes(void) {
    CHECK(vrst_mode_parse(NULL) == VRST_OFF);
    CHECK(vrst_mode_parse("") == VRST_OFF);
    CHECK(vrst_mode_parse("off") == VRST_OFF);
    CHECK(vrst_mode_parse("defer") == VRST_DEFER);
    CHECK(vrst_mode_parse("1") == VRST_DEFER);
    CHECK(vrst_mode_parse("refund") == VRST_REFUND);
    CHECK(vrst_mode_parse("bogus") == VRST_OFF);
    CHECK(!strcmp(vrst_mode_name(VRST_REFUND), "refund"));
    CHECK(!strcmp(vrst_kind_name(VRST_EV_EXPIRED), "miss-free"));
    printf("PASS modes: off (default) | defer | refund\n");
}

static void test_why_and_limiter(void) {
    CHECK(vrst_cost_why(PLAYER, PLAYER, -20, 1, 0) == VRST_WHY_OK);
    CHECK(vrst_cost_why(ENEMY, PLAYER, -20, 1, 0) == VRST_WHY_NOT_PLAYER);
    CHECK(vrst_cost_why(PLAYER, 0, -20, 1, 0) == VRST_WHY_NOT_PLAYER);
    CHECK(vrst_cost_why(PLAYER, PLAYER, 0, 1, 0) == VRST_WHY_NO_COST);
    CHECK(vrst_cost_why(PLAYER, PLAYER, INT32_MIN, 1, 0) == VRST_WHY_NO_COST);
    CHECK(vrst_cost_why(PLAYER, PLAYER, -20, 0, 0xFF) == VRST_WHY_NO_ROW);
    CHECK(vrst_cost_why(PLAYER, PLAYER, -20, 1, 1) == VRST_WHY_NOT_ATTACK);
    CHECK(!strcmp(vrst_why_name(VRST_WHY_NOT_ATTACK), "row is not an attack (refType != 0)"));
    double next = 0.0;
    CHECK(vrst_rate_ok(&next, 5.0, 10.0));
    CHECK(!vrst_rate_ok(&next, 14.9, 10.0));
    CHECK(vrst_rate_ok(&next, 15.0, 10.0));
    /* a world hit (by id only): constant non-weapon contacts (ids 304/307) never pay an open ticket */
    VrStTickets k;
    vrst_reset(&k);
    int32_t sp = 91, delta = -20;
    vrst_open(&k, VRST_DEFER, ATK_R1, 20, &sp, 91, 0, 0.0, W, &delta, NULL);
    VrStEvent e = vrst_hit(&k, VRST_DEFER, 304, 0, 0.2, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_UNMATCHED && sp == 91 && k.t[0].hits == 0);
    e = vrst_hit(&k, VRST_DEFER, ATK_R1, 0, 0.3, &sp, 91, 0);
    CHECK(e.kind == VRST_EV_CHARGED && sp == 71);
    printf("PASS cost filter reasons, diagnostics rate limiter, id-only world hits\n");
}

/* BB_VR_STAMINA_TRACE: the watchpoint's DR7 bits and the per-writer counters. */
static void test_trace_helpers(void) {
    CHECK(vrst_dr7_watch0(0) == 0xD0001u);                   /* L0, RW0 = write, LEN0 = 4 bytes */
    CHECK(vrst_dr7_watch0(0xF0004u) == 0xD0005u);            /* slot 1's L1 kept, slot 0 rewritten */
    CHECK(vrst_dr7_watch0(0x400u | 0xF00000u) == (0x400u | 0xF00000u | 0xD0001u)); /* other slots kept */
    CHECK(vrst_dr7_clear0(vrst_dr7_watch0(0x4u)) == 0x4u);
    VrStSites t;
    memset(&t, 0, sizeof(t));
    VrStSite *s = vrst_site_note(&t, 0x1cbdc9bu, 80, 82); /* regen */
    CHECK(s && s->writes == 1 && s->rises == 1 && s->drops == 0);
    vrst_site_note(&t, 0x1cbdc9bu, 82, 82);
    s = vrst_site_note(&t, 0x1cb64cdu, 82, 62);            /* an attack's cost */
    CHECK(s && s->drops == 1 && s->dropped == 20 && s->last_before == 82 && s->last_after == 62);
    CHECK(t.n == 2 && t.s[0].writes == 2 && t.s[0].rises == 1);
    for (unsigned i = 0; i < 2 * VRST_MAX_SITES; ++i) vrst_site_note(&t, 0x100u + i, 10, 5);
    CHECK(t.n == VRST_MAX_SITES && t.overflow == VRST_MAX_SITES + 2);
    CHECK(vrst_site_note(&t, 0x1cb64cdu, 62, 40) != NULL && t.s[1].drops == 2 && t.s[1].dropped == 42);
    printf("PASS trace: watchpoint DR7 bits, per-writer counters\n");
}

/* Only real contacts pay: an enemy, a wall the weapon bounced off, an object; never the floor. */
static void test_contacts(void) {
    CHECK(vrst_contact(0, 0) == VRST_HIT_CHR && vrst_contact_pays(VRST_HIT_CHR));
    CHECK(vrst_contact(1, 1) == VRST_HIT_WALL && vrst_contact_pays(VRST_HIT_WALL));
    CHECK(vrst_contact(1, 0) == VRST_HIT_FLOOR && !vrst_contact_pays(VRST_HIT_FLOOR));
    CHECK(vrst_contact(2, 0) == VRST_HIT_OBJ && vrst_contact_pays(VRST_HIT_OBJ));
    CHECK(!vrst_contact_pays(VRST_HIT_NONE));
    CHECK(!strcmp(vrst_contact_name(VRST_HIT_FLOOR), "world, no bounce (floor)"));
    /* a swing into the air: its hitbox grazes the floor from the first frame; nothing pays, the window ends free */
    VrStTickets k;
    vrst_reset(&k);
    int32_t sp = 91, delta = -20;
    vrst_open(&k, VRST_DEFER, ATK_R1, 20, &sp, 91, 0, 0.0, W, &delta, NULL);
    for (int i = 0; i < 10; ++i)
        if (vrst_contact_pays(vrst_contact(1, 0))) vrst_hit(&k, VRST_DEFER, ATK_R1, 0, 0.01 * i, &sp, 91, 0);
    VrStEvent ev[2];
    CHECK(vrst_expire(&k, VRST_DEFER, W + 0.01, &sp, 91, ev, 2) == 1 && ev[0].kind == VRST_EV_EXPIRED && sp == 91);
    printf("PASS contacts: character, bounced wall and object pay, the floor does not\n");
}

int main(void) {
    test_miss_free();
    test_single_charge_multi_hit();
    test_matching();
    test_expiry();
    test_refund();
    test_floor();
    test_debug_flags();
    test_player_only();
    test_modes();
    test_why_and_limiter();
    test_trace_helpers();
    test_contacts();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
