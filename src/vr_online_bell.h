/* Auto-bell state machine for rooms (src/runtime_online_bell.c): decides when the game rings the Beckoning Bell
 * (host) or the Small Resonant Bell (guest) by itself. C11, header only, no I/O, no clock of its own: the
 * caller passes `now` (seconds) and what it sees in the game; the test is tests/test_vr_online_bell.c.
 *
 * The rule: ring the bell every gap_s (20 s) while someone is still joining / not in the room. One fixed gap for
 * both roles; after every ring result the next ring is now + gap_s. A ring the game refuses right now (the
 * character is attacking or rolling: vob_ring_result with reached -1) is tried again after retry_s (1 s) and counted
 * in `refused`, not in `fails`. A ring that reported nothing within result_s (30 s) counts as not reached.
 * The truth about what the rings achieved comes from the room server through the room file
 * (tools/release/zip/bbvr_online.py keeps it fresh), plus the emulator's Matching2 room role (np_room).
 *
 * Host: "arrived" = guests really in the host's world (VobIn.summoned), "summoning" = VobIn.pending (the host's game
 * sent a summon request, the guest has not arrived). need = min(members-1, max_members-1) (max_members-1 while the
 * member count is unknown). Ring while arrived < need. arrived >= max_members-1: VOB_FULL (room_full). arrived >=
 * need but the room is not full (everyone who joined is here): VOB_ALL_IN (all_here), no ring; a member joining later
 * goes back to ready. Without the arrived count (older file, -1) the room is full at members >= max_members.
 * An arrival restarts the wait (next ring >= arrive_s later), a guest leaving rings again after free_s. While
 * pending > 0 (at most pending_max_s since first seen) there is no ring (state summoning, next_at >= now + arrive_s):
 * a ring during a summon makes the game reject it; when the summon ends the arrival window starts.
 *
 * Guest: np_room == 2 (member of another player's room = in the host's world) or the server's self_summoned: state
 * summoned, no ring (a stale server flag left after the way home is ignored). Sign up on the server: state sign_up, no
 * ring; sign gone: ring again after free_s. Otherwise ring every gap_s.
 *
 * Both roles: wait settle_s in the world with no menu / busy before the first ring after a load; no ring while a menu
 * or the UI executor is busy; no ring within net_hold_s (20 s) of NP signaling activity (net_quiet_s). With the
 * pad-press fallback (direct 0) also no ring while the player pressed a button in the last 1.5 s (`acting`, checked at
 * the final ring test only); with the game's direct item-use call (direct 1) the player may keep walking, so
 * `acting` is ignored. Death / leaving the world resets the machine. */
#ifndef VR_ONLINE_BELL_H
#define VR_ONLINE_BELL_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VOB_GOODS_BECKONING 200u /* EquipParamGoods rows; the item id is 0x40000000 | row */
#define VOB_GOODS_RESONANT 205u
#define VOB_GOODS_FLAG 0x40000000u

enum { VOB_NONE_ROLE = 0, VOB_HOST = 1, VOB_GUEST = 2 };
enum { VOB_OFF = 0, VOB_WAIT_WORLD, VOB_NEED_BELL, VOB_READY, VOB_RINGING, VOB_FULL, VOB_CLOSED, VOB_SIGN_UP, VOB_SUMMONED, VOB_SUMMONING, VOB_ALL_IN };
enum { VOB_ACT_NONE = 0, VOB_ACT_RING, VOB_ACT_GIVE };

typedef struct {
    int role;            /* VOB_HOST / VOB_GUEST */
    int max_members;     /* room size including the host (3) */
    int allow_give;      /* dev flag: add a missing bell to the inventory */
    double settle_s;     /* in the world and idle this long before ringing (8) */
    double gap_s;        /* wait after a ring result before the next ring, both roles (20) */
    double retry_s;      /* wait after the game refused a ring (busy character) (1) */
    double arrive_s;     /* a guest was just added: leave the bell alone this long (20) */
    double result_s;     /* a ring that has not reported back after this counts as not reached (30) */
    double free_s;       /* a summoned guest left / a sign vanished: ring again after this (10) */
    double stale_s;      /* guest: a world change this long after the summon was seen is the way home (60) */
    double pending_max_s; /* host: a summon in progress (summoned && sign) holds the bell at most this long (120) */
    double net_hold_s;   /* no ring within this long of NP signaling activity (20) */
} VobCfg;

typedef struct {
    double now;
    int in_world;        /* a player character exists */
    int alive;
    int menu_open;       /* a game menu or panel is up */
    int busy;            /* the UI executor or an equip request is running */
    int bell_count;      /* how many of this role's bell the inventory holds; -1: inventory not read yet */
    int members;         /* room members incl. the host from the room file; 0: unknown */
    int summoned;        /* host: guests really in (summoned && !sign); guest: 1 when this player is summoned; -1: unknown */
    int sign;            /* guest: the server holds this player's summon sign (1/0); -1: unknown */
    int signs;           /* host: how many guests of the room have a summon sign up; -1: unknown */
    int pending;         /* host: guests with summoned && sign (summon in progress); -1: unknown */
    double net_quiet_s;  /* seconds since the last NP signaling activity (0 during a handshake); < 0: unknown */
    int acting;          /* the player pressed a button in the last 1.5 s (ignored when direct) */
    int direct;          /* 1: the ring uses the game's direct item-use call (no pad presses) */
    int np_room;         /* the emulator's Matching2 room role: 0 none, 1 owner of own room, 2 member of another's room; -1 unknown */
    int room_closed;     /* the room file says the room is gone */
} VobIn;

typedef struct {
    int state;
    int had_world;
    double world_since;  /* 0: not settling */
    double next_at;      /* earliest next ring */
    double ring_at;      /* when the last ring was requested */
    double give_at;
    double last_ring_at; /* 0 never */
    int gave;
    int rings, fails, refused, resets, last_members;
    int settled;         /* the settle time passed since the last (re)start */
    int need, arrived;   /* host: the last step's numbers (need -1: unknown) */
    double pending_since; /* host: when pending was first seen > 0 (0: not pending) */
    double summon_since; /* guest: when the summoned flag was first seen */
    int summon_resets;   /* guest: world changes seen while summoned (arrival, then the way home) */
    int stale_summon;    /* guest: the summoned flag is left over from a finished visit; ignored until it clears */
    int reached;         /* the last result */
    const char *error;   /* last problem, or NULL */
} VobMachine;

static inline void vob_cfg_default(VobCfg *c, int role) {
    memset(c, 0, sizeof(*c));
    c->role = role;
    c->max_members = 3;
    c->settle_s = 8.0;
    c->gap_s = 20.0;
    c->retry_s = 1.0;
    c->arrive_s = 20.0;
    c->result_s = 30.0;
    c->free_s = 10.0;
    c->stale_s = 60.0;
    c->pending_max_s = 120.0;
    c->net_hold_s = 20.0;
}

static inline uint32_t vob_bell_id(int role) {
    return VOB_GOODS_FLAG | (role == VOB_HOST ? VOB_GOODS_BECKONING : VOB_GOODS_RESONANT);
}

static inline void vob_init(VobMachine *m, const VobCfg *c) {
    memset(m, 0, sizeof(*m));
    m->state = (c->role == VOB_HOST || c->role == VOB_GUEST) ? VOB_WAIT_WORLD : VOB_OFF;
    m->need = -1;
}

static inline const char *vob_state_name(int s) {
    static const char *const n[] = {"off", "waiting_for_world", "bell_missing", "ready", "ringing", "room_full", "room_closed", "sign_up", "summoned", "summoning", "all_here"};
    return s >= 0 && s <= VOB_ALL_IN ? n[s] : "?";
}

/* The world was left (death, sent home, a load): start over. */
static inline void vob_reset(VobMachine *m, const VobCfg *c) {
    (void)c;
    m->state = VOB_WAIT_WORLD;
    m->world_since = 0.0;
    m->next_at = 0.0;
    m->settled = 0;
    m->fails = 0;
    m->error = NULL;
    m->resets++;
}

/* The use of the bell finished: reached 1 = the game took it, 0 = it did not reach the game (next ring after gap_s), -1 =
 * the game refused it right now (busy character): try again after retry_s, counted in `refused`, not in `fails`. */
static inline void vob_ring_result(VobMachine *m, const VobCfg *c, double now, int reached) {
    if (m->state != VOB_RINGING) return;
    m->reached = reached;
    m->state = VOB_READY;
    if (reached < 0) {
        m->refused++;
        m->next_at = now + c->retry_s;
        return;
    }
    if (reached) {
        m->fails = 0;
        m->error = NULL;
    } else {
        m->fails++;
        m->error = "the bell press did not reach the game";
    }
    m->next_at = now + c->gap_s;
}

static inline int vob_step(VobMachine *m, const VobCfg *c, const VobIn *in) {
    if (m->state == VOB_OFF) return VOB_ACT_NONE;
    if (in->room_closed) {
        m->state = VOB_CLOSED;
        return VOB_ACT_NONE;
    }
    if (m->state == VOB_CLOSED) m->state = VOB_WAIT_WORLD; /* a new room appeared */
    const int world = in->in_world && in->alive;
    if (!world) {
        if (m->had_world) {
            if (c->role == VOB_GUEST && m->state == VOB_SUMMONED) {
                /* the first change while summoned is the arrival in the host's world, a later one is the way home */
                m->summon_resets++;
                if (m->summon_resets >= 2 || (m->summon_resets >= 1 && in->now - m->summon_since > c->stale_s)) {
                    m->stale_summon = 1;
                    vob_reset(m, c);
                }
            } else {
                vob_reset(m, c);
            }
        }
        m->had_world = 0;
        return VOB_ACT_NONE;
    }
    m->had_world = 1;
    if (c->role == VOB_GUEST) {
        if (in->summoned == 0) {
            m->stale_summon = 0;
            m->summon_resets = 0;
        }
        /* np_room 2: the emulator says this player is a member of another player's room, i.e. in the host's world */
        if (in->np_room == 2 || (in->summoned == 1 && !m->stale_summon)) { /* in (or on the way to) the host's world: nothing to ring */
            if (m->state != VOB_SUMMONED) {
                m->state = VOB_SUMMONED;
                m->summon_since = in->now;
                m->summon_resets = 0;
                m->error = NULL;
            }
            return VOB_ACT_NONE;
        }
        if (m->state == VOB_SUMMONED) { /* back in its own world: start over */
            m->state = VOB_WAIT_WORLD;
            m->settled = 0;
            m->world_since = 0.0;
            m->next_at = 0.0;
        }
        if (in->sign == 1) { /* the ring worked: the sign is up, wait for the host */
            m->state = VOB_SIGN_UP;
            m->error = NULL;
            return VOB_ACT_NONE;
        }
        if (m->state == VOB_SIGN_UP) { /* the sign is gone (expired or taken back): ring again after a short wait */
            m->state = VOB_READY;
            if (m->next_at < in->now + c->free_s) m->next_at = in->now + c->free_s;
        }
    }
    if (c->role == VOB_HOST && (in->summoned >= 0 || in->members > 0)) {
        /* arrived = guests really in the host's world; without that count (older room file) the member count stands in */
        const int known = in->summoned >= 0;
        const int cnt = known ? in->summoned : in->members;
        const int cap = known ? c->max_members - 1 : c->max_members;
        int need = cap;
        if (known && in->members > 0 && in->members - 1 < cap) need = in->members - 1;
        m->arrived = cnt;
        m->need = need;
        if (cnt > m->last_members) { /* a guest arrived: leave the bell alone a moment */
            if (m->next_at < in->now + c->arrive_s) m->next_at = in->now + c->arrive_s;
        } else if (cnt < m->last_members && cnt < cap) {
            m->next_at = in->now + c->free_s; /* one left: fill the slot soon */
        }
        m->last_members = cnt;
        if (cnt >= cap) {
            m->state = VOB_FULL;
            return VOB_ACT_NONE;
        }
        if (cnt >= need) { /* everyone who joined the room is here already */
            m->state = VOB_ALL_IN;
            return VOB_ACT_NONE;
        }
        if (m->state == VOB_FULL || m->state == VOB_ALL_IN) m->state = m->settled ? VOB_READY : VOB_WAIT_WORLD;
    } else if (c->role == VOB_HOST) {
        m->need = -1;
    }
    if (c->role == VOB_HOST) { /* summon-in-progress bookkeeping (the hold itself is below, after the settle) */
        if (in->pending > 0) {
            if (m->pending_since == 0.0) m->pending_since = in->now;
        } else if (m->pending_since != 0.0) { /* the summon ended (guest in, or the sign came back): arrival window */
            m->pending_since = 0.0;
            if (m->next_at < in->now + c->arrive_s) m->next_at = in->now + c->arrive_s;
        }
    }
    if (m->state == VOB_RINGING) {
        if (in->now - m->ring_at > c->result_s) vob_ring_result(m, c, in->now, 0);
        return VOB_ACT_NONE;
    }
    if (m->state == VOB_WAIT_WORLD) {
        if (in->menu_open || in->busy) {
            m->world_since = 0.0;
            return VOB_ACT_NONE;
        }
        if (m->world_since == 0.0) m->world_since = in->now;
        if (in->now - m->world_since < c->settle_s) return VOB_ACT_NONE;
        m->state = VOB_READY;
        m->settled = 1;
        if (m->next_at == 0.0) m->next_at = in->now;
    }
    if (c->role == VOB_HOST && (m->state == VOB_READY || m->state == VOB_SUMMONING)) {
        if (in->pending > 0 && in->now - m->pending_since <= c->pending_max_s) { /* a summon is running: hands off */
            m->state = VOB_SUMMONING;
            if (m->next_at < in->now + c->arrive_s) m->next_at = in->now + c->arrive_s;
            return VOB_ACT_NONE;
        }
        if (m->state == VOB_SUMMONING) m->state = VOB_READY;
    }
    if (in->bell_count < 0) return VOB_ACT_NONE; /* inventory not read yet */
    if (in->bell_count == 0) {
        m->state = VOB_NEED_BELL;
        m->error = "the bell is not in the inventory";
        if (c->allow_give && !in->menu_open && !in->busy && (m->gave == 0 || in->now - m->give_at >= 30.0)) {
            m->give_at = in->now;
            m->gave++;
            return VOB_ACT_GIVE;
        }
        return VOB_ACT_NONE;
    }
    if (m->state == VOB_NEED_BELL) {
        m->state = VOB_READY;
        m->error = NULL;
        m->next_at = in->now;
    }
    if (m->state != VOB_READY || in->now < m->next_at || in->menu_open || in->busy || (in->acting && !in->direct)) return VOB_ACT_NONE;
    if (in->net_quiet_s >= 0.0 && in->net_quiet_s < c->net_hold_s) return VOB_ACT_NONE; /* NP signaling is busy */
    m->state = VOB_RINGING;
    m->ring_at = in->now;
    m->last_ring_at = in->now;
    m->rings++;
    return VOB_ACT_RING;
}

/* What the room file says (tools/release/zip/bbvr_online.py writes it; the CLI writes the first part). */
typedef struct {
    int members;      /* members incl. the host; 0: unknown */
    int closed;
    int summoned;     /* host: guests summoned; guest: 1/0 for this player; -1: not in the file */
    int sign;         /* this player has a summon sign on the server (1/0); -1: not in the file */
    int signs;        /* guests (members that are not the host) with a sign up; -1: not in the file */
    int in_guests;    /* guests with summoned && !sign (really in); -1: no member has a "summoned" key */
    int pending;      /* guests with summoned && sign (summon in progress); -1: as in_guests */
    double ts;        /* when the helper wrote it (unix seconds); 0: no stamp */
} VobRoom;

/* the number after `"key":` or -1; true/false read as 1/0 */
static inline double vob_json_num(const char *text, const char *key) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (!p) return -1.0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (!strncmp(p, "true", 4)) return 1.0;
    if (!strncmp(p, "false", 5)) return 0.0;
    if ((*p >= '0' && *p <= '9') || *p == '-') return strtod(p, NULL);
    return -1.0;
}

/* Room file text -> member count and closed flag. Understands "members": N, "members": [ {...}, ... ]
 * (one object per member) and "closed": true / "in_room": false. Returns 1 when it found a member count. */
static inline int vob_parse_room(const char *text, int *members, int *closed) {
    *members = 0;
    *closed = 0;
    if (!text) return 0;
    const char *p = strstr(text, "\"closed\"");
    if (p) {
        const char *t = strstr(p, "true"), *f = strstr(p, "false");
        if (t && t < p + 16 && !(f && f < t)) *closed = 1;
    }
    p = strstr(text, "\"in_room\"");
    if (p) {
        const char *f = strstr(p, "false");
        if (f && f < p + 17) *closed = 1;
    }
    p = strstr(text, "\"members\"");
    if (!p) return 0;
    p += 9;
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p >= '0' && *p <= '9') {
        *members = atoi(p);
        return 1;
    }
    if (*p == '[') {
        int depth = 0, n = 0, in_str = 0;
        for (const char *q = p; *q; ++q) {
            if (in_str) {
                if (*q == '\\' && q[1]) ++q;
                else if (*q == '"') in_str = 0;
                continue;
            }
            if (*q == '"') in_str = 1;
            else if (*q == '[' || *q == '{') {
                if (*q == '{' && depth == 1) ++n; /* one object per member */
                ++depth;
            } else if (*q == ']' || *q == '}') {
                if (--depth == 0) break;
            }
        }
        *members = n;
        return 1;
    }
    return 0;
}


/* Per non-host member of the "members" array: sign -> *signs. With a "world" key (the server's presence): world "host" ->
 * *in (really in, summoned or not), else summoned -> *pending. Without it: summoned && !sign -> *in, summoned && sign ->
 * *pending (summon in progress). *signs is -1 when there is no array or no member has a "sign" key; *in and *pending are
 * -1 when there is no array or no member has a "summoned" or "world" key. */
static inline void vob_count_members(const char *text, int *signs, int *in, int *pending) {
    *signs = *in = *pending = -1;
    const char *p = text ? strstr(text, "\"members\"") : NULL;
    if (!p) return;
    p += 9;
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != '[') return;
    int depth = 0, in_str = 0, n = 0, ni = 0, np = 0, any = 0, any_sm = 0;
    for (const char *q = p; *q; ++q) {
        if (in_str) {
            if (*q == '\\' && q[1]) ++q;
            else if (*q == '"') in_str = 0;
            continue;
        }
        if (*q == '"') {
            in_str = 1;
        } else if (*q == '[' || *q == '{') {
            if (*q == '{' && depth == 1) { /* one member: copy it to look at its own keys */
                int d = 0, s = 0;
                const char *e = q;
                for (; *e; ++e) {
                    if (s) {
                        if (*e == '\\' && e[1]) ++e;
                        else if (*e == '"') s = 0;
                        continue;
                    }
                    if (*e == '"') s = 1;
                    else if (*e == '{' || *e == '[') ++d;
                    else if ((*e == '}' || *e == ']') && --d == 0) break;
                }
                char obj[512];
                size_t len = (size_t)(e - q) + 1;
                if (len >= sizeof obj) len = sizeof obj - 1;
                memcpy(obj, q, len);
                obj[len] = 0;
                const char *r = strstr(obj, "\"role\"");
                int host = 0;
                if (r) {
                    r += 6;
                    while (*r == ' ' || *r == ':' || *r == '\t') ++r;
                    host = strncmp(r, "\"host\"", 6) == 0;
                }
                const double sg = vob_json_num(obj, "sign");
                const double sm = vob_json_num(obj, "summoned");
                if (sg >= 0.0) any = 1;
                const char *wk = strstr(obj, "\"world\"");
                int has_world = 0, world_host = 0;
                if (wk) {
                    wk += 7;
                    while (*wk == ' ' || *wk == ':' || *wk == '\t') ++wk;
                    has_world = *wk == '"';
                    world_host = strncmp(wk, "\"host\"", 6) == 0;
                }
                if (sm >= 0.0 || has_world) any_sm = 1;
                if (!host && sg > 0.5) ++n;
                if (!host && has_world) {
                    if (world_host) ++ni; /* the server's presence says it is in the host's world */
                    else if (sm > 0.5) ++np;
                } else if (!host && sm > 0.5) {
                    if (sg > 0.5) ++np;
                    else ++ni;
                }
            }
            ++depth;
        } else if (*q == ']' || *q == '}') {
            if (--depth == 0) break;
        }
    }
    if (any) *signs = n;
    if (any_sm) {
        *in = ni;
        *pending = np;
    }
}

/* Guests with "sign": true in the "members" array; -1 when there is no array or no member has a "sign" key. */
static inline int vob_count_signs(const char *text) {
    int s, i, p;
    vob_count_members(text, &s, &i, &p);
    return s;
}

/* The whole file: vob_parse_room plus "self_sign", "self_summoned" (this player), "summoned_guests" (host view) and
 * "ts". Returns 1 when it found a member count. */
static inline int vob_parse_room_ex(const char *text, int role, VobRoom *r) {
    memset(r, 0, sizeof(*r));
    r->summoned = -1;
    r->sign = -1;
    r->signs = -1;
    r->in_guests = -1;
    r->pending = -1;
    const int found = vob_parse_room(text, &r->members, &r->closed);
    if (!text) return 0;
    const double ts = vob_json_num(text, "ts");
    r->ts = ts > 0.0 ? ts : 0.0;
    const double sg = vob_json_num(text, "self_sign");
    r->sign = sg < 0.0 ? -1 : sg > 0.5;
    const double sm = vob_json_num(text, role == VOB_HOST ? "summoned_guests" : "self_summoned");
    r->summoned = sm < 0.0 ? -1 : (int)sm;
    vob_count_members(text, &r->signs, &r->in_guests, &r->pending);
    return found;
}

#endif
