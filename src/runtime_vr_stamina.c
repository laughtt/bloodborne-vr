/* "Misses are free" stamina in the game (BB_VR_STAMINA=defer|refund, docs/vr/findings/20 6.2 G3):
 * a swing that does not connect costs no stamina; the first hit of an attack pays its cost once.
 * The rules are src/vr_stamina_tickets.h (tests/test_vr_stamina.c), the same as the VR sandbox's
 * (gpu/xr_bridge/swing_logic.h StaminaModel, sandbox3d/combat.{h,cpp}).
 *
 * Hooks (patches/Bloodborne.xml "VR Stamina Cost (behavior)" and "VR Stamina Hits", disabled by
 * default, selected with BB_HOOKS; all replay-after, so the handler sees the registers at the site):
 *   bb_hook_vr_stamina_cost  0x1e297cb, in 0x1e293b0 (the TAE attack-behavior handler, called by the
 *                            TAE dispatcher 0x1e249d0 at 0x1e258df; it creates the attack through
 *                            0x1c63970) just after `r12d = (int)cost` (cost = vfunc +0x200, BehaviorParam
 *                            stamina x SpEffect rates) and before `stamina -= r12d` ... clamp ... `mov
 *                            [r15+0x134],eax` (0x1e29845; the trace saw every R1's drop there). r12d =
 *                            the cost (> 0), r13 = the handler ([r13+8] = the character), r14d = the
 *                            behavior id, [rbp-0x38] = the BehaviorParam row (refType u8 +0x09, refId
 *                            s32 +0x0C = the AtkParam id). For the player's attack rows with a cost a
 *                            ticket opens; defer sets r12 to 0 (the game then subtracts 0), refund
 *                            leaves it. (The first site, 0x1cb644d in 0x1cb61c0, never ran in the game.)
 *   bb_hook_vr_stamina_tick  0x1cbdb20, the stamina regen (once per frame per character, before the
 *                            regen): rdi = the character. Closes the tickets whose window ended
 *                            (refund: gives the cost back). Without it tickets close lazily in the
 *                            next cost or hit hook.
 *   bb_hook_vr_hit_chr/_obj  0x1cb79e0 / 0x1cb8430, the hit dispatcher's handlers for a character
 *                            attacker against a character / an object (entry): rdi = the hit record
 *                            (+0x3C AtkParam id, +0x40 isPc), rdx = the attacker.
 *   bb_hook_vr_hit_map       0x1cb87ff, inside the world handler 0x1cb8730 just after the attacker's
 *                            module (bag +0x98, vfunc +0xb0) decided whether the weapon bounces: r15b = 1
 *                            for a wall it bounced off, rbx = the hit record, r14 = the attacker. The
 *                            hitbox touches the floor from its first frame (every swing "hit" 0.00-0.07 s
 *                            in, swings into the air included), so only a bounce pays (vrst_contact). The player's first hit of a ticket pays it (defer). World and
 *                            object hits pay only by AtkParam id (the player's body contacts 304/307
 *                            fire ~20 times a second and are not attacks); character hits may also pay
 *                            the newest ticket (BB_VR_STAMINA_MATCH=any).
 * The writes copy the game's own tail: skipped while the module's "No Stamina Consume" (+0x200 bit
 * 0x80) or the global "All No Stamina Consume" byte is set, else clamp to [-50, max].
 *
 * Memory: WorldChrMan [0x593E878] +0x60 = the player (PlayerIns, vtable 0x578F310); its data module
 * [[P + 0x3B0] + 0x20] (SprjChrDataModule, vtable 0x5735810): stamina +0x134, max +0x138, flags
 * +0x200 (findings/20 1.1). Every read goes through the readable-memory guard (the one
 * src/runtime_vr_fp.c uses); writes also check the page is writable.
 *
 * Threads: the behavior executor, the hit dispatcher and the regen run in the character update (the
 * game thread, findings/20 8 [L]); the state below is used only from these hooks, unlocked.
 *
 * Environment:
 *   BB_VR_STAMINA=off|defer|refund   default off (the hooks then do nothing, read nothing)
 *   BB_VR_STAMINA_LOG=1              the mode; a line when a ticket opens and when it closes (hit or
 *                                    missed, amount, stamina); hits while a ticket is open; the first
 *                                    cost-hook calls with their registers and a 10 s counter summary of
 *                                    why requests were skipped; unmatched player hits as a 10 s count
 *   BB_VR_STAMINA_WINDOW=<s>         how long after the attack's start a hit still matches (0.75)
 *   BB_VR_STAMINA_MATCH=any|id       a hit whose AtkParam id matches no ticket pays the newest open
 *                                    one (any, the sandbox's rule; default) or nothing (id)
 *   BB_VR_STAMINA_TRACE=1            who writes the player's stamina: a hardware write watchpoint on it
 *                                    logs every drop with the writing instruction and its callers, and a
 *                                    writer table every 10 s (any mode; Windows; findings/20 6.4)
 */
#define _GNU_SOURCE
#include "runtime.h"
#include "hooks.h"
#include "vr_stamina_tickets.h"
#include "vr_hitbox.h"
#include "gpu/xr_bridge/bbport_xr_bridge.h" /* BBXR_EV_*: the tickets for the VR UI's swing labels */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define VA_BASE 0x400000u
#define VA_SITE_COST 0x1e297cbu  /* in 0x1e293b0, the attack behavior (findings/20 6.4) */
#define VA_SITE_HIT_CHR 0x1cb79e0u
#define VA_SITE_HIT_MAP 0x1cb87ffu  /* in the world handler 0x1cb8730, after the bounce decision */
#define VA_SITE_HIT_OBJ 0x1cb8430u
#define VA_SITE_TICK 0x1cbdb20u
#define VA_WORLD_CHR_MAN 0x593E878u
#define VA_ALL_NO_STAMINA 0x593E88Au     /* WorldChrManDbg "All No Stamina Consume" (u8) */
#define VA_PLAYER_INS_VTABLE 0x578F310u
#define VA_CHR_DATA_VTABLE 0x5735810u    /* SprjChrDataModule */

static struct {
    int ready, mode, log, match_any;
    double window;
    unsigned char *image; /* guest VA 0x400000 */
    uintptr_t player;     /* the tickets belong to this PlayerIns */
    VrStTickets tickets;
    unsigned opened, charged, missed, unmatched;
} vs;

static double vs_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void vt_init(void); /* BB_VR_STAMINA_TRACE, below */

static void vs_init(void) {
    if (vs.ready) return;
    vs.ready = 1;
    vt_init();
    vs.mode = vrst_mode_parse(getenv("BB_VR_STAMINA"));
    const char *e = getenv("BB_VR_STAMINA_LOG");
    vs.log = e && *e && strcmp(e, "0");
    vs.window = VRST_DEFAULT_WINDOW;
    e = getenv("BB_VR_STAMINA_WINDOW");
    if (e && *e) {
        const double w = strtod(e, NULL);
        if (isfinite(w) && w >= 0.05 && w <= 5.0) vs.window = w;
        else fprintf(stderr, "VR stamina: BB_VR_STAMINA_WINDOW=%s ignored (0.05..5)\n", e);
    }
    e = getenv("BB_VR_STAMINA_MATCH");
    vs.match_any = !(e && !strcmp(e, "id"));
    vrst_reset(&vs.tickets);
    if (vs.mode != VRST_OFF || vs.log) {
        fprintf(stderr, "VR stamina: mode %s, window %.2f s, match %s, log %d\n", vrst_mode_name(vs.mode),
                vs.window, vs.match_any ? "any" : "id", vs.log);
        fflush(stderr);
    }
}

/* ---- guest memory: read only what is mapped (the player chain dangles during loads) ----
 * The same guard as src/runtime_vr_fp.c (fp_readable); writes also need a writable page. */
#ifdef _WIN32
static struct { uintptr_t lo, hi; } vs_regions[16];
static int vs_region_count;
static double vs_regions_time;
static int vs_readable(uintptr_t a, size_t n) {
    if (a < 0x10000 || a + n < a) return 0;
    const double now = vs_now();
    if (now - vs_regions_time > 1.0) { vs_region_count = 0; vs_regions_time = now; }
    for (int i = 0; i < vs_region_count; ++i)
        if (vs_regions[i].lo <= a && a + n <= vs_regions[i].hi) return 1;
    uintptr_t at = a;
    while (at < a + n) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void *)at, &mbi, sizeof(mbi))) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return 0;
        const uintptr_t lo = (uintptr_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
        if (vs_region_count < (int)(sizeof(vs_regions) / sizeof(*vs_regions))) {
            vs_regions[vs_region_count].lo = lo;
            vs_regions[vs_region_count].hi = hi;
            ++vs_region_count;
        }
        at = hi;
    }
    return 1;
}
static int vs_writable(uintptr_t a, size_t n) {
    if (!vs_readable(a, n)) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((const void *)a, &mbi, sizeof(mbi))) return 0;
    if ((uintptr_t)mbi.BaseAddress + mbi.RegionSize < a + n) return 0;
    return (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
#else
static int vs_readable(uintptr_t a, size_t n) {
    static int fds[2] = {-1, -1};
    if (a < 0x10000 || a + n < a) return 0;
    if (fds[0] < 0 && pipe(fds) != 0) return 0;
    const ssize_t w = write(fds[1], (const void *)a, n > 4096 ? 4096 : n);
    if (w < 0) return 0;
    char sink[4096];
    if (read(fds[0], sink, (size_t)w) < 0) return 0;
    return 1;
}
static int vs_writable(uintptr_t a, size_t n) { return vs_readable(a, n); }
#endif

static int rd32(uintptr_t a, int32_t *out) {
    if (!vs_readable(a, 4)) return 0;
    memcpy(out, (const void *)a, 4);
    return 1;
}
static int rd64(uintptr_t a, uint64_t *out) {
    if (!vs_readable(a, 8)) return 0;
    memcpy(out, (const void *)a, 8);
    return 1;
}
static int rd_ptr(uintptr_t a, uintptr_t *out) {
    uint64_t v;
    if (!rd64(a, &v) || v < 0x10000 || (v & 7)) return 0;
    *out = (uintptr_t)v;
    return 1;
}
static uintptr_t va(uint32_t v) { return (uintptr_t)(vs.image + (v - VA_BASE)); }

/* The image base from the hook's own site (no dependency on the camera hooks). */
static void vs_image_from(const BbHookContext *ctx, uint32_t site) {
    if (!vs.image) vs.image = (unsigned char *)(uintptr_t)(ctx->rip - (site - VA_BASE));
}

/* The main player (0: none, loading, not a PlayerIns). A new player drops the old tickets. */
static uintptr_t vs_player(void) {
    uintptr_t wcm, p;
    uint64_t vt;
    if (!rd_ptr(va(VA_WORLD_CHR_MAN), &wcm) || !rd_ptr(wcm + 0x60, &p) || !rd64(p, &vt) ||
        vt != (uint64_t)va(VA_PLAYER_INS_VTABLE))
        return 0;
    if (p != vs.player) {
        if (vs.tickets.n && vs.log)
            fprintf(stderr, "VR stamina: player changed, %d open ticket(s) dropped\n", vs.tickets.n);
        vrst_reset(&vs.tickets);
        vs.player = p;
    }
    return p;
}

/* The player's SprjChrDataModule: stamina, max, flags. */
typedef struct { uintptr_t d; int32_t stamina, max; uint32_t flags; int suppressed; } VsData;
static int vs_data(uintptr_t player, VsData *o) {
    uintptr_t bag;
    uint64_t vt;
    memset(o, 0, sizeof(*o));
    if (!rd_ptr(player + 0x3B0, &bag) || !rd_ptr(bag + 0x20, &o->d) || !rd64(o->d, &vt) ||
        vt != (uint64_t)va(VA_CHR_DATA_VTABLE) || !vs_readable(o->d + 0x134, 8) || !vs_readable(o->d + 0x200, 2))
        return 0;
    memcpy(&o->stamina, (const void *)(o->d + 0x134), 4);
    memcpy(&o->max, (const void *)(o->d + 0x138), 4);
    uint16_t f;
    memcpy(&f, (const void *)(o->d + 0x200), 2);
    o->flags = f;
    uint32_t all = 0;
    if (vs_readable(va(VA_ALL_NO_STAMINA), 1)) all = *(const uint8_t *)va(VA_ALL_NO_STAMINA);
    o->suppressed = vrst_suppressed(o->flags, all);
    return 1;
}

static int vs_write_stamina(const VsData *d, int32_t value) {
    if (value == d->stamina) return 1;
    if (!vs_writable(d->d + 0x134, 4)) return 0;
    memcpy((void *)(d->d + 0x134), &value, 4);
    return 1;
}

static void vs_log_close(const VrStEvent *e, double now) {
    if (e->kind == VRST_EV_NONE) return;
    if (e->kind == VRST_EV_EXPIRED || e->kind == VRST_EV_REFUNDED) {
        ++vs.missed;
        runtime_vr_ui_event(BBXR_EV_SWING_MISS, e->ticket.cost, 0, (int32_t)e->ticket.id); /* "MISS  free" */
    }
    if (!vs.log) return;
    const VrStTicket *t = &e->ticket;
    if (t->hits > 0)
        fprintf(stderr, "VR stamina: ticket %u atk %d cost %d: HIT %s at +%.2f s (%d hit%s, %s%s, atk %d), paid %d (SP %d -> %d) [%s]\n",
                t->id, t->atk_id, t->cost, vrst_contact_name(t->hit_kind), t->first_hit - t->opened, t->hits, t->hits == 1 ? "" : "s",
                t->by_id ? "by id" : "newest ticket", t->suppressed ? ", debug flag: free" : "", t->hit_atk_id,
                t->paid, t->before, t->after, vrst_mode_name(vs.mode));
    else
        fprintf(stderr, "VR stamina: ticket %u atk %d cost %d: MISS after %.2f s, %s %d (SP %d -> %d) [%s]\n",
                t->id, t->atk_id, t->cost, now - t->opened, e->kind == VRST_EV_REFUNDED ? "refunded" : "free, saved",
                e->kind == VRST_EV_REFUNDED ? e->amount : t->cost, e->before, e->after, vrst_mode_name(vs.mode));
    fflush(stderr);
}

/* Closes the windows that ended (refund: gives the cost back). */
static void vs_expire(VsData *d, double now) {
    if (!vs.tickets.n) return;
    VrStEvent ev[VRST_MAX_TICKETS];
    int32_t sp = d->stamina;
    const int n = vrst_expire(&vs.tickets, vs.mode, now, &sp, d->max, ev, VRST_MAX_TICKETS);
    if (sp != d->stamina) {
        if (vs_write_stamina(d, sp)) d->stamina = sp;
        else if (vs.log) fprintf(stderr, "VR stamina: refund not written (module not writable)\n");
    }
    for (int i = 0; i < n && i < VRST_MAX_TICKETS; ++i) vs_log_close(&ev[i], now);
}

/* Cost hook diagnostics (BB_VR_STAMINA_LOG=1): the first VS_TRACE_ALL calls and the first
 * VS_TRACE_FAIL failing player-like calls (delta < 0) print their registers, and a counter summary
 * prints every 10 s while the counters move; so a session that opens no ticket says which test failed. */
#define VS_TRACE_ALL 24
#define VS_TRACE_FAIL 16
static struct {
    unsigned calls, why[5], no_data, not_writable, traced_all, traced_fail, last_sum_calls;
    double next_sum;
} vc;

static uintptr_t vs_rd_chr(const BbHookContext *ctx) { /* the cost site's character, [r13+8] */
    uintptr_t chr = 0;
    rd_ptr(ctx->r13 + 8, &chr);
    return chr;
}

static void vs_cost_trace(const BbHookContext *ctx, uintptr_t player, int have_delta, int32_t delta, uint64_t row,
                          int have_row, uint32_t ref_type, int32_t ref_id, int why) {
    fprintf(stderr,
            "VR stamina: cost call #%u chr %#llx player %#llx behavior %d delta %s%d row %#llx(%s) refType %#x refId %d"
            " -> %s\n",
            vc.calls, (unsigned long long)vs_rd_chr(ctx), (unsigned long long)player, (int)(int32_t)ctx->r14,
            have_delta ? "" : "(unreadable) ", delta, (unsigned long long)row, have_row ? "ok" : "none", ref_type,
            ref_id, vrst_why_name(why));
    fflush(stderr);
}

static void vs_cost_summary(double now, int force) {
    if (!vs.log || !vc.calls) return;
    if (!force && (vc.calls == vc.last_sum_calls || !vrst_rate_ok(&vc.next_sum, now, 10.0))) return;
    vc.last_sum_calls = vc.calls;
    fprintf(stderr,
            "VR stamina: cost hook %u calls: %u not the player, %u no cost, %u no row, %u not an attack, %u no data module,"
            " %u request not writable, %u tickets opened\n",
            vc.calls, vc.why[VRST_WHY_NOT_PLAYER], vc.why[VRST_WHY_NO_COST], vc.why[VRST_WHY_NO_ROW],
            vc.why[VRST_WHY_NOT_ATTACK], vc.no_data, vc.not_writable, vs.opened);
    fflush(stderr);
}

static ABI void vs_hook_cost(BbHookContext *ctx) {
    vs_init();
    if (vs.mode == VRST_OFF) return;
    vs_image_from(ctx, VA_SITE_COST);
    const uintptr_t player = vs_player();
    uintptr_t chr = 0;
    rd_ptr(ctx->r13 + 8, &chr);
    const int mine = player && chr == player;
    if (!mine && !vs.log) return; /* without the log nothing else is read for other characters */
    ++vc.calls;
    int32_t ref_id = 0;
    const int32_t cost_in = (int32_t)(uint32_t)ctx->r12; /* `vcvttss2si r12d, xmm0` */
    const int have_delta = 1;
    const int32_t delta = cost_in > 0 ? -cost_in : 0; /* as a change of the stamina */
    uint64_t row = 0;
    const int have_row = rd64(ctx->rbp - 0x38, &row) && row >= 0x10000 && vs_readable(row, 0x10);
    const uint32_t ref_type = have_row ? *(const uint8_t *)(row + 0x09) : 0xFFu;
    if (have_row) memcpy(&ref_id, (const void *)(row + 0x0C), 4);
    const int why = vrst_cost_why(chr, player, delta, have_row, ref_type);
    ++vc.why[why];
    const double now = vs_now();
    if (vs.log) {
        if (vc.traced_all < VS_TRACE_ALL) {
            ++vc.traced_all;
            vs_cost_trace(ctx, player, have_delta, delta, row, have_row, ref_type, ref_id, why);
        } else if (why != VRST_WHY_OK && have_delta && delta < 0 && vc.traced_fail < VS_TRACE_FAIL) {
            ++vc.traced_fail;
            vs_cost_trace(ctx, player, have_delta, delta, row, have_row, ref_type, ref_id, why);
        }
        vs_cost_summary(now, 0);
    }
    if (why != VRST_WHY_OK) return;
    const int32_t cost = -delta;
    VsData d;
    if (!vs_data(player, &d)) {
        ++vc.no_data;
        return;
    }
    vs_expire(&d, now);
    int32_t new_delta = delta, sp = d.stamina;
    VrStEvent evicted;
    vrst_open(&vs.tickets, vs.mode, ref_id, cost, &sp, d.max, d.suppressed, now, vs.window, &new_delta, &evicted);
    if (evicted.kind != VRST_EV_NONE) {
        if (sp != d.stamina && vs_write_stamina(&d, sp)) d.stamina = sp;
        vs_log_close(&evicted, now);
    }
    if (new_delta != delta) ctx->r12 = (uint64_t)(uint32_t)(-new_delta); /* defer: 0, the game subtracts nothing */
    ++vs.opened;
    runtime_vr_ui_event(BBXR_EV_SWING_OPEN, cost, ref_id, (int32_t)vs.tickets.t[vs.tickets.n - 1].id); /* "-20 if it hits" */
    if (vs.log) {
        fprintf(stderr, "VR stamina: ticket %u opened: atk %d cost %d, SP %d/%d (%s)\n", vs.tickets.t[vs.tickets.n - 1].id,
                ref_id, cost, d.stamina, d.max, vrst_mode_name(vs.mode));
        fflush(stderr);
    }
}

/* Hit diagnostics: the unmatched hits are counted per kind and summed up every 10 s (the world hook
 * fires ~20 times a second for the player's non-weapon contacts, AtkParam 304/307); only a hit while a
 * ticket is open prints itself. */
static struct { unsigned unmatched[3], last_total, floor, floor_shown; double next_sum; } vh;

/* Haptics for every weapon contact the game reports (BBXR_EV_WEAPON_CONTACT), ticket or not, any mode.
 * A repeat of the same contact (same kind and AtkParam id) within VS_CONTACT_REPEAT_S is one contact;
 * the player's body contacts (AtkParam 304/307) and the floor never pulse. */
#define VS_CONTACT_REPEAT_S 0.5
static struct { int contact, atk; double t; } vk = { 0, -1, -1.0e9 };
static int vs_contact_new(int contact, int atk_id, double now) { /* 1: not a repeat; notes it */
    const int fresh = contact != vk.contact || atk_id != vk.atk || now - vk.t > VS_CONTACT_REPEAT_S;
    vk.contact = contact; vk.atk = atk_id; vk.t = now;
    return fresh;
}
static void vs_contact_pulse(int contact, int atk_id, int32_t cost, double now) {
    if (!vrst_contact_pays(contact) || atk_id < 0 || atk_id == 304 || atk_id == 307) return;
    if (!vs_contact_new(contact, atk_id, now)) return;
    runtime_vr_ui_event(BBXR_EV_WEAPON_CONTACT, cost, contact == VRST_HIT_WALL ? 1 : contact == VRST_HIT_OBJ ? 2 : 0,
                        1 /* the right hand: the contact record does not say which weapon */);
}

static void vs_hit(BbHookContext *ctx, uint32_t site, int kind, uint64_t rec, uint64_t attacker, int bounced) {
    vs_init();
    vs_image_from(ctx, site);
    const uintptr_t player = vs_player();
    if (!player || attacker != player) return;
    const int contact = vrst_contact(kind, bounced);
    int32_t atk_id = -1, is_pc = -1;
    rd32(rec + 0x3C, &atk_id);
    rd32(rec + 0x40, &is_pc);
    if (vrst_contact_pays(contact)) /* the hitbox overlay's hit markers (findings/29) */
        vr_hitbox_hit(contact == VRST_HIT_WALL ? 1 : contact == VRST_HIT_OBJ ? 2 : 0, atk_id, (uintptr_t)rec);
    const double now = vs_now();
    int32_t open_cost = 0; /* the cost of the open ticket of this attack: the pulse's strength */
    for (int i = 0; i < vs.tickets.n; ++i)
        if (vs.tickets.t[i].atk_id == atk_id) open_cost = vs.tickets.t[i].cost;
    if (vs.mode == VRST_OFF) { /* no stamina feature: the contact still pulses */
        vs_contact_pulse(contact, atk_id, open_cost, now);
        return;
    }
    if (!vs.tickets.n) { /* nothing to pay: count, no further reads */
        vs_contact_pulse(contact, atk_id, 0, now);
        ++vh.unmatched[kind];
        ++vs.unmatched;
        if (vs.log) {
            const unsigned total = vh.unmatched[0] + vh.unmatched[1] + vh.unmatched[2];
            if (total != vh.last_total && vrst_rate_ok(&vh.next_sum, now, 10.0)) {
                vh.last_total = total;
                fprintf(stderr, "VR stamina: player hits with no open ticket so far: %u character, %u world, %u object;"
                        " %u floor contacts while a ticket was open (free)\n",
                        vh.unmatched[0], vh.unmatched[1], vh.unmatched[2], vh.floor);
                fflush(stderr);
            }
        }
        return;
    }
    if (!vrst_contact_pays(contact)) { /* the floor: never pays, never pulses; the first few print (the hit's point) */
        ++vh.floor;
        if (vs.log && vh.floor_shown < 12) {
            ++vh.floor_shown;
            float pt[8] = {0};
            const int have = vs_readable(rec + 0x180, sizeof(pt));
            if (have) memcpy(pt, (const void *)(uintptr_t)(rec + 0x180), sizeof(pt));
            fprintf(stderr, "VR stamina: %s atk %d: free; point (%.2f %.2f %.2f) +0x190 (%.2f %.2f %.2f)%s\n",
                    vrst_contact_name(contact), atk_id, pt[0], pt[1], pt[2], pt[4], pt[5], pt[6], have ? "" : " (unreadable)");
            fflush(stderr);
        }
        return;
    }
    VsData d;
    if (!vs_data(player, &d)) return;
    vs_expire(&d, now);
    int32_t sp = d.stamina;
    /* The walls and the objects pay only by AtkParam id: the player's body contacts (304/307) must not pay
     * an attack that did not touch anything; a character hit may also pay the newest ticket (match any). */
    const int any = vs.match_any && kind == 0;
    const VrStEvent e = vrst_hit(&vs.tickets, vs.mode, atk_id, any, now, &sp, d.max, d.suppressed);
    if (sp != d.stamina && !vs_write_stamina(&d, sp) && vs.log)
        fprintf(stderr, "VR stamina: charge not written (module not writable)\n");
    if (e.kind == VRST_EV_CHARGED || e.kind == VRST_EV_PAID || e.kind == VRST_EV_SUPPRESSED)
        for (int i = 0; i < vs.tickets.n; ++i)
            if (vs.tickets.t[i].id == e.ticket.id && !vs.tickets.t[i].hit_kind) vs.tickets.t[i].hit_kind = contact;
    if (e.kind == VRST_EV_CHARGED) ++vs.charged;
    if (e.kind == VRST_EV_CHARGED || e.kind == VRST_EV_PAID || e.kind == VRST_EV_SUPPRESSED) { /* "HIT -20" */
        runtime_vr_ui_event(BBXR_EV_SWING_HIT, e.kind == VRST_EV_CHARGED ? e.amount : e.ticket.paid,
                            contact == VRST_HIT_WALL ? 1 : contact == VRST_HIT_OBJ ? 2 : 0, (int32_t)e.ticket.id);
        vs_contact_new(contact, atk_id, now); /* it pulsed already: its repeats stay quiet */
    } else {
        vs_contact_pulse(contact, atk_id, open_cost, now);
    }
    if (e.kind == VRST_EV_UNMATCHED) {
        ++vs.unmatched;
        ++vh.unmatched[kind];
    } else if (vs.log && e.kind != VRST_EV_REPEAT) {
        fprintf(stderr, "VR stamina: %s hit atk %d isPc %d: %s (ticket atk %d, %d open)\n", vrst_contact_name(contact),
                atk_id, is_pc, vrst_kind_name(e.kind), e.ticket.atk_id, vs.tickets.n);
        fflush(stderr);
    }
}

/* ---- BB_VR_STAMINA_TRACE=1: who writes the player's stamina (findings/20 6.4) ----
 * The first headset sessions never reached the cost hook (0x1cb644d), not even for the enemies, while the
 * player attacked; static analysis finds 0x1cb61c0 reachable from the TAE attack event and 20 other
 * inlined stamina writers. This trace settles it live: a hardware data watchpoint (DR0, 4-byte writes) on
 * the player's stamina field, armed on the game thread from the per-frame hook (0x1cbdb20), records every
 * write: the instruction after the store (a data watchpoint traps after it), the old and new value and
 * three return addresses up the guest's rbp chain. The exception handler only fills a ring and counters;
 * the tick prints: each drop (rate-limited), a writer table every 10 s, and changes that no watchpoint
 * saw (another thread). Off by default; it works with BB_VR_STAMINA=off (the game unchanged). The cost
 * and hit hooks also count their calls for the player while it is on. Windows only. */
#ifdef _WIN32
#define VS_ARM_CODE 0xE0B5A701u /* a private software exception: the handler loads DR0/DR7 */
#define VS_RING 64
typedef struct { uint64_t va, ret[3]; int32_t before, after; } VsTraceEv;
static struct {
    int on;
    volatile int ever;               /* a thread was armed once */
    PVOID veh;
    volatile uintptr_t target;       /* the watched address (the data module's +0x134), 0 none */
    volatile LONG last, have_last;   /* the value after the last trapped write */
    DWORD thread[8];                 /* the threads armed for `armed_for` */
    int threads;
    uintptr_t armed_for;
    VsTraceEv ring[VS_RING];
    volatile unsigned head, tail, lost;
    VrStSites sites;
    unsigned untrapped, cost_calls, hits[3], printed, suppressed, last_writes;
    double window_start, next_sum;
} vt;

static uint64_t vt_va(uint64_t a) { /* a guest image address as a patch-space VA, else the host address */
    const uint64_t base = (uint64_t)(uintptr_t)vs.image;
    return vs.image && a - base < 0x20000000u ? a - base + VA_BASE : a;
}
static int vt_rd64(uint64_t a, uint64_t *out) { /* no cache: also called from the exception handler */
    MEMORY_BASIC_INFORMATION mbi;
    if (a < 0x10000 || (a & 7) || !VirtualQuery((const void *)(uintptr_t)a, &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        (uintptr_t)mbi.BaseAddress + mbi.RegionSize < (uintptr_t)a + 8)
        return 0;
    memcpy(out, (const void *)(uintptr_t)a, 8);
    return 1;
}

static LONG CALLBACK vt_veh(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    if (er->ExceptionCode == VS_ARM_CODE && er->NumberParameters == 1) {
        const uintptr_t a = (uintptr_t)er->ExceptionInformation[0];
        c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        c->Dr0 = a;
        c->Dr7 = a ? vrst_dr7_watch0(c->Dr7) : vrst_dr7_clear0(c->Dr7);
        c->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    /* ours: DR0 hit (B0), or DR0 armed by this trace (in case the context's DR6 is not filled in) */
    if (er->ExceptionCode != EXCEPTION_SINGLE_STEP ||
        !((c->Dr6 & 1) || (vt.ever && (c->Dr7 & 1) && c->Dr0)))
        return EXCEPTION_CONTINUE_SEARCH;
    c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    c->Dr6 = 0;
    const uintptr_t target = vt.target;
    if (!target || (uintptr_t)c->Dr0 != target) return EXCEPTION_CONTINUE_EXECUTION;
    int32_t after;
    memcpy(&after, (const void *)target, 4);
    const int32_t before = vt.have_last ? (int32_t)vt.last : after;
    vt.last = after;
    vt.have_last = 1;
    const uint64_t va = vt_va((uint64_t)c->Rip);
    vrst_site_note(&vt.sites, va, before, after);
    if (after < before) { /* a drop: where from (the guest keeps rbp frames) */
        const unsigned h = vt.head;
        if (h - vt.tail >= VS_RING) { ++vt.lost; return EXCEPTION_CONTINUE_EXECUTION; }
        VsTraceEv *e = &vt.ring[h % VS_RING];
        memset(e, 0, sizeof(*e));
        e->va = va;
        e->before = before;
        e->after = after;
        uint64_t fp = (uint64_t)c->Rbp, r;
        for (int i = 0; i < 3 && vt_rd64(fp + 8, &r) && vt_rd64(fp, &fp); ++i) e->ret[i] = vt_va(r);
        vt.head = h + 1;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void vt_init(void) {
    const char *e = getenv("BB_VR_STAMINA_TRACE");
    if (!(e && *e && strcmp(e, "0"))) return;
    vt.veh = AddVectoredExceptionHandler(1, vt_veh);
    vt.on = vt.veh != NULL;
    fprintf(stderr, "VR stamina trace: %s (a write watchpoint on the player's stamina; drops print with their writer)\n",
            vt.on ? "on" : "failed (no exception handler)");
    fflush(stderr);
}

/* Arms (a != 0) or clears the watchpoint on the calling thread: the handler edits the debug registers in
 * the context the exception resumes with. */
static void vt_arm_here(uintptr_t a) {
    ULONG_PTR arg = (ULONG_PTR)a;
    RaiseException(VS_ARM_CODE, 0, 1, &arg);
}

/* The per-frame part (the tick hook, any character): keeps the watchpoint on the player's stamina on this
 * thread, prints what the handler saw. */
static void vt_tick(const BbHookContext *ctx) {
    vs_image_from(ctx, VA_SITE_TICK);
    const uintptr_t player = vs_player();
    VsData d;
    const int have = player && ctx->rdi == player && vs_data(player, &d);
    if (!player) {
        if (vt.target) { /* loading: the module goes away; stop watching its memory */
            vt.target = 0;
            vt.armed_for = 0;
            vt.threads = 0;
            vt_arm_here(0);
        }
        return;
    }
    if (!have) return;
    const uintptr_t target = d.d + 0x134;
    const DWORD me = GetCurrentThreadId();
    if (target != vt.armed_for) {
        vt.armed_for = target;
        vt.threads = 0;
        vt.have_last = 0;
    }
    int armed = 0;
    for (int i = 0; i < vt.threads; ++i) armed |= vt.thread[i] == me;
    if (!armed && vt.threads < (int)(sizeof(vt.thread) / sizeof(*vt.thread))) {
        vt.target = target;
        vt.ever = 1;
        vt_arm_here(target);
        vt.thread[vt.threads++] = me;
        CONTEXT c;
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        const int ok = GetThreadContext(GetCurrentThread(), &c) && (uintptr_t)c.Dr0 == target && (c.Dr7 & 1);
        fprintf(stderr, "VR stamina trace: watching SP %d/%d at %p on thread %lu: %s (Dr7 %#llx)\n", d.stamina, d.max,
                (void *)target, (unsigned long)me, ok ? "armed" : "NOT ARMED", (unsigned long long)c.Dr7);
        fflush(stderr);
    }
    /* a change no watchpoint saw: written from a thread that is not armed */
    if (vt.have_last && d.stamina != (int32_t)vt.last) {
        if (++vt.untrapped <= 20)
            fprintf(stderr, "VR stamina trace: SP %d -> %d changed with no trapped write (another thread?)\n",
                    (int)vt.last, d.stamina);
        vt.last = d.stamina;
    } else if (!vt.have_last) {
        vt.last = d.stamina;
        vt.have_last = 1;
    }
    const double now = vs_now();
    if (now - vt.window_start > 10.0) {
        if (vt.suppressed)
            fprintf(stderr, "VR stamina trace: %u drop line(s) not printed (rate limit)\n", vt.suppressed);
        vt.window_start = now;
        vt.printed = vt.suppressed = 0;
    }
    while (vt.tail != vt.head) {
        const VsTraceEv *e = &vt.ring[vt.tail % VS_RING];
        if (vt.printed < 40) {
            ++vt.printed;
            fprintf(stderr, "VR stamina trace: SP %d -> %d (%+d) written before %#llx, return %#llx <- %#llx <- %#llx\n",
                    e->before, e->after, e->after - e->before, (unsigned long long)e->va, (unsigned long long)e->ret[0],
                    (unsigned long long)e->ret[1], (unsigned long long)e->ret[2]);
        } else {
            ++vt.suppressed;
        }
        ++vt.tail;
    }
    unsigned writes = vt.sites.overflow;
    for (int i = 0; i < vt.sites.n; ++i) writes += vt.sites.s[i].writes;
    if (writes != vt.last_writes && vrst_rate_ok(&vt.next_sum, now, 10.0)) {
        vt.last_writes = writes;
        fprintf(stderr, "VR stamina trace: %u writes; cost hook %u player calls; player hits %u character, %u world, %u object;"
                " %u untrapped, %u lost\n", writes, vt.cost_calls, vt.hits[0], vt.hits[1], vt.hits[2], vt.untrapped, vt.lost);
        for (int i = 0; i < vt.sites.n; ++i) {
            const VrStSite *s = &vt.sites.s[i];
            fprintf(stderr, "VR stamina trace:   writer before %#llx: %u writes, %u drops (-%lld total), %u rises, last %d -> %d\n",
                    (unsigned long long)s->site, s->writes, s->drops, (long long)s->dropped, s->rises, s->last_before,
                    s->last_after);
        }
    }
    fflush(stderr);
}
#else
static struct { int on; unsigned cost_calls, hits[3]; } vt;
static void vt_init(void) {
    const char *e = getenv("BB_VR_STAMINA_TRACE");
    if (e && *e && strcmp(e, "0")) fprintf(stderr, "VR stamina trace: Windows only\n");
}
static void vt_tick(const BbHookContext *ctx) { (void)ctx; }
#endif

/* With the trace on, the cost and hit hooks count their calls (any mode): the first cost-hook calls print,
 * whoever the character, so a session says whether 0x1cb644d is reached at all. */
static void vt_count(const BbHookContext *ctx, uint32_t site, int kind) {
    vs_init();
    if (!vt.on) return;
    vs_image_from(ctx, site);
    const uintptr_t player = vs_player();
    if (kind < 0) {
        static unsigned shown;
        const uintptr_t chr = vs_rd_chr(ctx);
        if (player && chr == player) ++vt.cost_calls;
        if (shown < 8) {
            ++shown;
            fprintf(stderr, "VR stamina trace: cost hook reached (chr %#llx, player %#llx, cost %d)\n",
                    (unsigned long long)chr, (unsigned long long)player, (int)(int32_t)ctx->r12);
            fflush(stderr);
        }
    } else if (player && (kind == 1 ? ctx->r14 : ctx->rdx) == player) {
        ++vt.hits[kind];
    }
}
/* The character updates run on several job threads (the trace armed up to 7 per session), so every hook
 * body that reads or changes the state above runs under one spinlock (held for microseconds; the
 * watchpoint's exception handler never takes it). With BB_VR_STAMINA=off and no trace the hooks return
 * before it. */
static volatile int vs_lock_word;
static void vs_lock(void) {
    while (__atomic_exchange_n(&vs_lock_word, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&vs_lock_word, __ATOMIC_RELAXED)) __builtin_ia32_pause();
}
static void vs_unlock(void) { __atomic_store_n(&vs_lock_word, 0, __ATOMIC_RELEASE); }
static void vs_ensure_init(void) { /* vs_init once, under the lock */
    static volatile int inited;
    if (!__atomic_load_n(&inited, __ATOMIC_ACQUIRE)) {
        vs_lock();
        vs_init();
        vs_unlock();
        __atomic_store_n(&inited, 1, __ATOMIC_RELEASE);
    }
}
static int vs_active(void) { /* is anything on */
    vs_ensure_init();
    return vs.mode != VRST_OFF || vt.on;
}
static int vs_active_hits(void) { /* the hit hooks always run: weapon contacts pulse in every mode */
    vs_ensure_init();
    return 1;
}

static ABI void vs_hook_cost_entry(BbHookContext *ctx) {
    if (!vs_active()) return;
    vs_lock();
    vt_count(ctx, VA_SITE_COST, -1);
    vs_hook_cost(ctx);
    vs_unlock();
}
static ABI void vs_hook_hit_chr(BbHookContext *ctx) {
    if (!vs_active_hits()) return;
    vs_lock();
    vt_count(ctx, VA_SITE_HIT_CHR, 0);
    vs_hit(ctx, VA_SITE_HIT_CHR, 0, ctx->rdi, ctx->rdx, 0);
    vs_unlock();
}
static ABI void vs_hook_hit_map(BbHookContext *ctx) {
    if (!vs_active_hits()) return;
    vs_lock();
    vt_count(ctx, VA_SITE_HIT_MAP, 1);
    vs_hit(ctx, VA_SITE_HIT_MAP, 1, ctx->rbx, ctx->r14, (int)(ctx->r15 & 0xFF));
    vs_unlock();
}
static ABI void vs_hook_hit_obj(BbHookContext *ctx) {
    if (!vs_active_hits()) return;
    vs_lock();
    vt_count(ctx, VA_SITE_HIT_OBJ, 2);
    vs_hit(ctx, VA_SITE_HIT_OBJ, 2, ctx->rdi, ctx->rdx, 0);
    vs_unlock();
}

static void vs_tick_locked(BbHookContext *ctx) {
    if (vt.on) vt_tick(ctx);
    if (vs.mode == VRST_OFF) return;
    if (!vs.tickets.n) return;
    vs_image_from(ctx, VA_SITE_TICK);
    const uintptr_t player = vs_player();
    if (!player || ctx->rdi != player || !vs.tickets.n) return;
    VsData d;
    if (!vs_data(player, &d)) return;
    vs_expire(&d, vs_now());
}
static ABI void vs_hook_tick(BbHookContext *ctx) {
    if (!vs_active()) return;
    /* nothing open and no trace: no lock, no reads (it runs for every character every frame) */
    if (!vt.on && !__atomic_load_n(&vs.tickets.n, __ATOMIC_RELAXED)) return;
    vs_lock();
    vs_tick_locked(ctx);
    vs_unlock();
}

void runtime_vr_stamina_register(void) {
    bb_hook_set_stamina_impl(vs_hook_cost_entry, vs_hook_hit_chr, vs_hook_hit_map, vs_hook_hit_obj, vs_hook_tick);
}
