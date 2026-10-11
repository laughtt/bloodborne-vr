/* "Misses are free" stamina tickets (docs/vr/findings/20-swing-and-stamina.md 3.1, 6.2 G3): the pure
 * rules src/runtime_vr_stamina.c applies inside the game's hooks, the same rules as the VR sandbox's
 * swing::StaminaModel (gpu/xr_bridge/swing_logic.h) and sandbox3d/combat.{h,cpp}:
 *   - an attack of the player opens a ticket with its cost (BehaviorParam stamina x the SpEffect
 *     rates, the game's own request delta);
 *   - defer: the game adds 0 when the attack starts; the first hit of the attack pays the cost once
 *     (later hits of the same attack, e.g. a sweep through three enemies, are free); a ticket whose
 *     window ends without a hit costs nothing;
 *   - refund: the game pays at the start as in vanilla; a ticket whose window ends without a hit
 *     gives back what the game really took (after its clamp);
 *   - every change goes through the game's clamp, x < -50 ? -50 : min(x, max);
 *   - the debug flags (the module's "No Stamina Consume" bit 0x80 of +0x200, the global "All No
 *     Stamina Consume" byte) still suppress the cost, as in every writer of the game.
 * C11, header-only, no game memory: tests/test_vr_stamina.c builds it as C and as C++. */
#ifndef VR_STAMINA_TICKETS_H
#define VR_STAMINA_TICKETS_H
#include <stdint.h>
#include <string.h>

#define VRST_MAX_TICKETS 8
#define VRST_FLOOR (-50)                 /* the game's clamp (0x1cb64ae: cmp ebx,-0x32) */
#define VRST_FLAG_NO_STAMINA 0x80u       /* SprjChrDataModule +0x200: "No Stamina Consume" */
#define VRST_DEFAULT_WINDOW 0.75         /* s: the longest player window (the plunge, 0.63 s) + 0.1 */

enum { VRST_OFF = 0, VRST_DEFER = 1, VRST_REFUND = 2 };

/* What happened to a ticket (or a hit). */
enum {
    VRST_EV_NONE = 0,
    VRST_EV_CHARGED,    /* defer: the first hit paid the cost */
    VRST_EV_REPEAT,     /* a later hit of an attack that already connected: free */
    VRST_EV_SUPPRESSED, /* defer: the first hit, but a debug flag says no stamina is consumed */
    VRST_EV_PAID,       /* refund: the first hit; the game took the cost at the start, it stays */
    VRST_EV_UNMATCHED,  /* a hit with no open ticket (an attack without a cost, or expired) */
    VRST_EV_EXPIRED,    /* defer: the window ended without a hit: free */
    VRST_EV_REFUNDED,   /* refund: the window ended without a hit: the game's deduction given back */
    VRST_EV_KEPT,       /* a ticket that connected closed at its window's end (logs only) */
};

typedef struct {
    uint32_t id;
    int32_t atk_id;        /* the AtkParam id (BehaviorParam refId) */
    int32_t cost;          /* > 0: what the game asked for */
    int32_t paid;          /* what the stamina really lost for it (after the clamp, 0 when suppressed) */
    int32_t before, after; /* stamina around the charge (defer: the hit; refund: the start) */
    double opened, until;  /* the window: hits after `until` no longer match */
    double first_hit;      /* < 0 until it connects */
    int hits;              /* hit events matched (one per target) */
    int by_id;             /* the first hit matched by AtkParam id (0: the newest-ticket fallback) */
    int hit_atk_id;        /* the first hit's AtkParam id */
    int suppressed;        /* a debug flag said no stamina is consumed (nothing paid) */
    int hit_kind;          /* what the first hit touched (VRST_HIT_*, logs only; set by the caller) */
} VrStTicket;

typedef struct {
    VrStTicket t[VRST_MAX_TICKETS]; /* oldest first */
    int n;
    uint32_t next_id;
} VrStTickets;

typedef struct {
    int kind;          /* VRST_EV_* */
    int32_t amount;    /* stamina taken (CHARGED) or given back (REFUNDED); the cost otherwise */
    int32_t before, after;
    VrStTicket ticket; /* a copy, as it was when the event happened */
} VrStEvent;

static inline void vrst_reset(VrStTickets *k) {
    memset(k, 0, sizeof(*k));
}

/* The game's clamp on every stamina writer: x < -50 ? -50 : min(x, max). */
static inline int32_t vrst_clamp(int64_t x, int32_t max) {
    if (x < VRST_FLOOR) return VRST_FLOOR;
    return x > (int64_t)max ? max : (int32_t)x;
}

/* A negative change is skipped when the module's flag or the global byte is set (0x1cb646a). */
static inline int vrst_suppressed(uint32_t module_flags, uint32_t all_no_stamina) {
    return (module_flags & VRST_FLAG_NO_STAMINA) != 0 || all_no_stamina != 0;
}

/* Why the cost hook leaves a request alone (VRST_WHY_OK: it opens a ticket). The diagnostics count and
 * log these, so a session that opens no ticket says which test failed. `chr` is the character the
 * request runs on (r12), `player` WorldChrMan's main player, `delta` the request's stamina change
 * (+0x78, negative for a cost), `have_row`/`ref_type` the BehaviorParam row (refType 0 = attack;
 * bullets and SpEffects stay vanilla). */
enum { VRST_WHY_OK = 0, VRST_WHY_NOT_PLAYER, VRST_WHY_NO_COST, VRST_WHY_NO_ROW, VRST_WHY_NOT_ATTACK };

static inline int vrst_cost_why(uint64_t chr, uint64_t player, int32_t delta, int have_row, uint32_t ref_type) {
    if (!chr || !player || chr != player) return VRST_WHY_NOT_PLAYER;
    if (delta >= 0 || delta == INT32_MIN) return VRST_WHY_NO_COST;
    if (!have_row) return VRST_WHY_NO_ROW;
    if (ref_type != 0) return VRST_WHY_NOT_ATTACK;
    return VRST_WHY_OK;
}

static inline const char *vrst_why_name(int why) {
    switch (why) {
    case VRST_WHY_OK: return "ok";
    case VRST_WHY_NOT_PLAYER: return "not the player";
    case VRST_WHY_NO_COST: return "no cost (delta >= 0)";
    case VRST_WHY_NO_ROW: return "no BehaviorParam row";
    case VRST_WHY_NOT_ATTACK: return "row is not an attack (refType != 0)";
    default: return "?";
    }
}

/* The cost hook's filter: returns the cost to ticket (> 0), or 0 to leave the request alone. */
static inline int32_t vrst_cost_of(uint64_t chr, uint64_t player, int32_t delta, int have_row, uint32_t ref_type) {
    if (vrst_cost_why(chr, player, delta, have_row, ref_type) != VRST_WHY_OK) return 0;
    return -delta;
}

/* A diagnostics rate limiter: true at most once per `period` seconds (*next is the earliest next time;
 * start it at 0). */
static inline int vrst_rate_ok(double *next, double now, double period) {
    if (now < *next) return 0;
    *next = now + period;
    return 1;
}

static inline void vrst_remove(VrStTickets *k, int i) {
    for (int j = i; j + 1 < k->n; ++j) k->t[j] = k->t[j + 1];
    --k->n;
}

/* Closes ticket i (its window ended, or it was evicted): defer drops an unhit ticket free; refund
 * gives back what the game took. *stamina is updated in place. */
static inline VrStEvent vrst_close(VrStTickets *k, int i, int mode, int32_t *stamina, int32_t max) {
    VrStEvent e;
    memset(&e, 0, sizeof(e));
    e.ticket = k->t[i];
    e.before = e.after = *stamina;
    if (k->t[i].hits > 0) {
        e.kind = VRST_EV_KEPT;
        e.amount = k->t[i].paid;
    } else if (mode == VRST_REFUND) {
        e.kind = VRST_EV_REFUNDED;
        e.amount = k->t[i].paid;
        if (e.amount > 0) *stamina = vrst_clamp((int64_t)*stamina + e.amount, max);
        e.after = *stamina;
    } else {
        e.kind = VRST_EV_EXPIRED;
        e.amount = 0;
    }
    vrst_remove(k, i);
    return e;
}

/* Every window that ended before t closes (vrst_close). Writes up to `cap` events; returns how many
 * tickets closed. */
static inline int vrst_expire(VrStTickets *k, int mode, double t, int32_t *stamina, int32_t max,
                              VrStEvent *events, int cap) {
    int n = 0;
    for (int i = 0; i < k->n;) {
        if (t > k->t[i].until) {
            const VrStEvent e = vrst_close(k, i, mode, stamina, max);
            if (events && n < cap) events[n] = e;
            ++n;
        } else {
            ++i;
        }
    }
    return n;
}

/* The cost hook, after vrst_cost_of said yes: opens a ticket. Defer sets *delta to 0 (the game then
 * adds nothing); refund leaves it and records what the game is about to take (its clamp, nothing
 * when suppressed). When all tickets are open the oldest closes first (*evicted, and *stamina for
 * a refund). Returns the new ticket's id. */
static inline uint32_t vrst_open(VrStTickets *k, int mode, int32_t atk_id, int32_t cost, int32_t *stamina,
                                 int32_t max, int suppressed, double t, double window, int32_t *delta,
                                 VrStEvent *evicted) {
    if (evicted) memset(evicted, 0, sizeof(*evicted));
    if (k->n == VRST_MAX_TICKETS) {
        const VrStEvent e = vrst_close(k, 0, mode, stamina, max);
        if (evicted) *evicted = e;
    }
    VrStTicket *o = &k->t[k->n++];
    memset(o, 0, sizeof(*o));
    o->id = ++k->next_id;
    if (!o->id) o->id = ++k->next_id;
    o->atk_id = atk_id;
    o->cost = cost;
    o->opened = t;
    o->until = t + window;
    o->first_hit = -1.0;
    o->hit_atk_id = -1;
    o->before = o->after = *stamina;
    if (mode == VRST_DEFER) {
        if (delta) *delta = 0;
    } else if (mode == VRST_REFUND && suppressed) {
        o->suppressed = 1;
    } else if (mode == VRST_REFUND) {
        o->after = vrst_clamp((int64_t)*stamina - cost, max);
        o->paid = o->before - o->after;
        if (o->paid < 0) o->paid = 0; /* stamina above max: the clamp raised it; nothing to give back */
    }
    return o->id;
}

/* A hit by the player (character, world or object). The ticket: the newest open one with the hit's
 * AtkParam id, else (`allow_any`) the newest open one at all (the sandbox's rule: any contact while
 * a ticket is open pays it). The first hit of a ticket pays (defer) or keeps the cost (refund);
 * later ones are free. *stamina is updated in place. */
static inline VrStEvent vrst_hit(VrStTickets *k, int mode, int32_t atk_id, int allow_any, double t,
                                 int32_t *stamina, int32_t max, int suppressed) {
    VrStEvent e;
    memset(&e, 0, sizeof(e));
    e.before = e.after = *stamina;
    int at = -1, by_id = 0;
    for (int i = k->n - 1; i >= 0; --i)
        if (k->t[i].atk_id == atk_id && t <= k->t[i].until) { at = i; by_id = 1; break; }
    if (at < 0 && allow_any)
        for (int i = k->n - 1; i >= 0; --i)
            if (t <= k->t[i].until) { at = i; break; }
    if (at < 0) {
        e.kind = VRST_EV_UNMATCHED;
        e.ticket.atk_id = atk_id;
        e.ticket.hit_atk_id = atk_id;
        return e;
    }
    VrStTicket *o = &k->t[at];
    if (o->hits++ > 0) {
        e.kind = VRST_EV_REPEAT;
        e.ticket = *o;
        return e;
    }
    o->first_hit = t;
    o->by_id = by_id;
    o->hit_atk_id = atk_id;
    if (mode == VRST_REFUND) {
        e.kind = VRST_EV_PAID;
        e.amount = o->paid;
    } else if (suppressed) {
        e.kind = VRST_EV_SUPPRESSED;
        o->suppressed = 1;
        o->before = o->after = *stamina;
    } else {
        o->before = *stamina;
        o->after = vrst_clamp((int64_t)*stamina - o->cost, max);
        o->paid = o->before - o->after;
        *stamina = o->after;
        e.kind = VRST_EV_CHARGED;
        e.amount = o->paid;
        e.after = *stamina;
    }
    e.ticket = *o;
    return e;
}

/* Sum of the open, not yet connected costs (a HUD's pending segment). */
static inline int32_t vrst_pending(const VrStTickets *k) {
    int32_t sum = 0;
    for (int i = 0; i < k->n; ++i)
        if (!k->t[i].hits) sum += k->t[i].cost;
    return sum;
}

static inline const char *vrst_kind_name(int kind) {
    switch (kind) {
    case VRST_EV_CHARGED: return "hit-charged";
    case VRST_EV_REPEAT: return "hit-repeat";
    case VRST_EV_SUPPRESSED: return "hit-suppressed";
    case VRST_EV_PAID: return "hit-kept";
    case VRST_EV_UNMATCHED: return "hit-unmatched";
    case VRST_EV_EXPIRED: return "miss-free";
    case VRST_EV_REFUNDED: return "miss-refunded";
    case VRST_EV_KEPT: return "closed";
    default: return "none";
    }
}

static inline const char *vrst_mode_name(int mode) {
    return mode == VRST_DEFER ? "defer" : mode == VRST_REFUND ? "refund" : "off";
}

/* BB_VR_STAMINA's value: off (default, also for anything unknown), defer, refund. */
static inline int vrst_mode_parse(const char *v) {
    if (!v) return VRST_OFF;
    if (!strcmp(v, "defer") || !strcmp(v, "1")) return VRST_DEFER;
    if (!strcmp(v, "refund")) return VRST_REFUND;
    return VRST_OFF;
}

/* ---- What a contact is (findings/20 6.4) ----
 * The hit dispatcher reports the player's weapon against a character, the world (map) or an object. A
 * real contact pays (the sandbox's rule: an enemy's hurtbox, a wall or a post), the floor does not: the
 * world handler 0x1cb8730 reports every map contact of the hitbox from its first frame (the first headset
 * run with the cost at 0x1e297cb paid nearly every swing 0.00-0.07 s in, swings into the air included),
 * but only a wall the weapon bounces off makes the attacker's module (bag +0x98, vfunc +0xb0) return true
 * (the hit processor then stops the attack, 0x1c61420 -> 0x1c614b0). So a world contact pays only when
 * the game bounced the weapon. */
enum { VRST_HIT_NONE = 0, VRST_HIT_CHR, VRST_HIT_WALL, VRST_HIT_OBJ, VRST_HIT_FLOOR };
static inline int vrst_contact(int kind /* 0 character, 1 world, 2 object */, int bounced) {
    return kind == 0 ? VRST_HIT_CHR : kind == 2 ? VRST_HIT_OBJ : bounced ? VRST_HIT_WALL : VRST_HIT_FLOOR;
}
static inline int vrst_contact_pays(int contact) { return contact != VRST_HIT_FLOOR && contact != VRST_HIT_NONE; }
static inline const char *vrst_contact_name(int contact) {
    switch (contact) {
    case VRST_HIT_CHR: return "character";
    case VRST_HIT_WALL: return "wall (bounced)";
    case VRST_HIT_OBJ: return "object";
    case VRST_HIT_FLOOR: return "world, no bounce (floor)";
    default: return "none";
    }
}

/* ---- BB_VR_STAMINA_TRACE: who writes the player's stamina (findings/20 6.4) ----
 * A debug-register watchpoint on the stamina field (data module +0x134) reports every instruction
 * that writes it; these helpers are the pure parts (DR7 bits, the per-writer counters). */

/* DR7 with slot 0 watching 4-byte data writes, locally enabled (L0; RW0 = 01; LEN0 = 11); the other
 * slots' bits are kept. */
static inline uint64_t vrst_dr7_watch0(uint64_t dr7) {
    dr7 &= ~(((uint64_t)0xF << 16) | 3u);
    return dr7 | 1u | ((uint64_t)1 << 16) | ((uint64_t)3 << 18);
}
static inline uint64_t vrst_dr7_clear0(uint64_t dr7) { return dr7 & ~(((uint64_t)0xF << 16) | 3u); }

#define VRST_MAX_SITES 24
typedef struct {
    uint64_t site;           /* the instruction after the write (a data watchpoint traps after it) */
    uint32_t writes, drops, rises;
    int64_t dropped;         /* the sum of the drops (positive) */
    int32_t last_before, last_after;
} VrStSite;
typedef struct {
    VrStSite s[VRST_MAX_SITES];
    int n;
    uint32_t overflow;       /* writes from sites beyond the table */
} VrStSites;

/* Counts one write of the stamina from `site` (before -> after); NULL when the table is full. */
static inline VrStSite *vrst_site_note(VrStSites *t, uint64_t site, int32_t before, int32_t after) {
    int i = 0;
    while (i < t->n && t->s[i].site != site) ++i;
    if (i == t->n) {
        if (t->n == VRST_MAX_SITES) { ++t->overflow; return NULL; }
        memset(&t->s[i], 0, sizeof(t->s[i]));
        t->s[i].site = site;
        ++t->n;
    }
    VrStSite *s = &t->s[i];
    ++s->writes;
    if (after < before) { ++s->drops; s->dropped += (int64_t)before - after; }
    else if (after > before) ++s->rises;
    s->last_before = before;
    s->last_after = after;
    return s;
}

#endif
