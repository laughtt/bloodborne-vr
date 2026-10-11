/* Auto-bell for rooms: the driver around src/vr_online_bell.h. In a room (the launcher sets the variables) the game
 * rings its own bell: the Beckoning Bell (goods:200) for the host, the Small Resonant Bell (goods:205) for a guest.
 * The ring itself is the dashboard's quick-slot path in src/runtime_vr_ui.c (the same code BB_DEBUG_USE runs: find
 * the item's quick slot, d-pad down, square); this file decides when, keeps the retry/backoff state, reads the room
 * file, and writes a small status file for the launcher / dashboard. Log lines start with "Online bell:".
 * Rule: ring the bell every gap_s (20 s) while someone is still joining / not in the room (see src/vr_online_bell.h).
 * Environment:
 *   BB_ONLINE_ROLE         host | guest (unset: off)
 *   BB_ONLINE_ROOM         the room code, for the log and the status file
 *   BB_ONLINE_AUTOBELL     0 turns it off (default on when the role is set)
 *   BB_ONLINE_BELL_GIVE    1: add the bell when the inventory lacks it (the launcher sets it in a room; off: it is only logged)
 *   BB_ONLINE_ROOM_FILE    the room file (user/online_room.json): the CLI makes it, the heartbeat helper
 *                          (tools/release/zip/bbvr_online.py) refreshes it every ~5 s with the server's view: members,
 *                          per member sign / summoned / world, "closed". The game reads it every 5 s. Host: rings while
 *                          fewer guests are in its world than members joined (stops at "room_full" or "all_here"); no
 *                          ring while a summon is in progress (held at most 120 s), then a 20 s arrival hold. Guest: stops
 *                          while its sign is up or it is in the host's world (summoned, or the emulator's Matching2 room
 *                          role says member of another room). A closed room stops everything. A file older than 45 s
 *                          (helper gone) counts as "unknown": the machine then just rings on its gap. Both roles: no ring
 *                          within 20 s of NP signaling activity (bbnet_signaling_quiet_s); with the pad-press fallback
 *                          also not while the player pressed a button in the last 1.5 s.
 *   BB_ONLINE_STATUS_FILE  status JSON (default out/online_bell_status.json), rewritten on change and every 5 s
 *   BB_ONLINE_BELL_GAP_S   wait between rings (default 20, 10..3600), both roles
 * Needs the same in-game step hook as BB_DEBUG_USE. Built in public builds too: the press, a bell given and the
 * free-bells Insight guard in src/runtime_vr_ui.c are restricted to the bell goods there. */
#define _GNU_SOURCE
#include "runtime.h"
#include "vr_online_bell.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
extern double bbnet_signaling_quiet_s(void); /* np_signaling: seconds since the last signaling activity, 0 mid-handshake, -1 none */
extern int bbnet_matching2_room_role(void); /* np_matching2: 0 in no room, 1 owner of its own room, 2 member of another player's room */

static struct {
    int checked, armed;
    VobCfg cfg;
    VobMachine m;
    char room[32], status_path[512], room_path[512];
    double status_at, room_at;
    int members, closed, members_known, summoned, sign, signs, in_guests, pending;
    int last_state, last_rings;
    const char *last_error;
    int in_world, alive, bell_count;
    const char *ring_path; /* "direct" / "pad" of the last result, or NULL */
    double refuse_log_at;  /* last refusal log line */
    int refuse_logged;     /* refusals counted at that line */
    unsigned long long room_stamp;
} ob;

static double ob_wall(void) { return (double)time(NULL); }

static double ob_env_num(const char *name, double fallback, double lo, double hi) {
    const char *e = getenv(name);
    if (!e || !*e) return fallback;
    const double v = strtod(e, NULL);
    return isfinite(v) && v >= lo && v <= hi ? v : fallback;
}

static void ob_init(void) {
    if (ob.checked) return;
    ob.checked = 1;
    const char *role = getenv("BB_ONLINE_ROLE");
    const char *on = getenv("BB_ONLINE_AUTOBELL");
    if (!role || !*role) return;
    const int r = strcmp(role, "host") == 0 ? VOB_HOST : strcmp(role, "guest") == 0 ? VOB_GUEST : 0;
    if (!r) {
        fprintf(stderr, "Online bell: BB_ONLINE_ROLE=\"%s\" not understood (want host or guest); off\n", role);
        return;
    }
    if (on && on[0] == '0') {
        fprintf(stderr, "Online bell: BB_ONLINE_AUTOBELL=0, off\n");
        return;
    }
    vob_cfg_default(&ob.cfg, r);
    const char *g = getenv("BB_ONLINE_BELL_GIVE");
    ob.cfg.allow_give = g && g[0] == '1';
    ob.cfg.gap_s = ob_env_num("BB_ONLINE_BELL_GAP_S", ob.cfg.gap_s, 10.0, 3600.0);
    vob_init(&ob.m, &ob.cfg);
    const char *room = getenv("BB_ONLINE_ROOM"), *sp = getenv("BB_ONLINE_STATUS_FILE"), *rp = getenv("BB_ONLINE_ROOM_FILE");
    snprintf(ob.room, sizeof ob.room, "%s", room ? room : "");
    snprintf(ob.status_path, sizeof ob.status_path, "%s", sp && *sp ? sp : "out/online_bell_status.json");
    snprintf(ob.room_path, sizeof ob.room_path, "%s", rp ? rp : "");
    ob.last_state = -1;
    ob.summoned = ob.sign = ob.signs = ob.in_guests = ob.pending = -1;
    ob.armed = 1;
    fprintf(stderr, "Online bell: %s of room %s, bell goods:%u, gap %.0f s, give-if-missing %s, status %s\n",
            r == VOB_HOST ? "host" : "guest", ob.room[0] ? ob.room : "?", (unsigned)(vob_bell_id(r) & 0xFFFFu), ob.cfg.gap_s,
            ob.cfg.allow_give ? "on" : "off", ob.status_path);
    fflush(stderr);
}

int runtime_online_bell_armed(void) {
    ob_init();
    return ob.armed;
}

unsigned runtime_online_bell_item(void) { return ob.armed ? vob_bell_id(ob.cfg.role) : 0; }

static void ob_write_status(double now) {
    char tmp[560];
    snprintf(tmp, sizeof tmp, "%s.tmp", ob.status_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    const double wall = ob_wall();
    const int np_room = bbnet_matching2_room_role();
    fprintf(f,
            "{\"state\":\"%s\",\"role\":\"%s\",\"room\":\"%s\",\"rings\":%d,\"not_reached\":%d,\"refused\":%d,\"ring_path\":%s%s%s,\"resets\":%d,\"gap_s\":%.0f,"
            "\"last_ring_ts\":%.0f,\"last_ring_ago_s\":%.0f,\"next_ring_in_s\":%.0f,\"bell_count\":%d,\"members\":%d,"
            "\"summoned\":%d,\"pending\":%d,\"sign\":%d,\"gave\":%d,\"in_world\":%d,\"np_room\":%d,\"last_error\":%s%s%s,\"ts\":%.0f}\n",
            vob_state_name(ob.m.state), ob.cfg.role == VOB_HOST ? "host" : "guest", ob.room, ob.m.rings, ob.m.fails, ob.m.refused,
            ob.ring_path ? "\"" : "", ob.ring_path ? ob.ring_path : "null", ob.ring_path ? "\"" : "", ob.m.resets,
            ob.cfg.gap_s, ob.m.last_ring_at > 0.0 ? wall - (now - ob.m.last_ring_at) : 0.0,
            ob.m.last_ring_at > 0.0 ? now - ob.m.last_ring_at : -1.0, ob.m.next_at > now ? ob.m.next_at - now : 0.0, ob.bell_count,
            ob.members_known ? ob.members : 0, ob.summoned, ob.pending, ob.sign, ob.m.gave, ob.in_world && ob.alive, np_room, ob.m.error ? "\"" : "", ob.m.error ? ob.m.error : "null",
            ob.m.error ? "\"" : "", wall);
    fclose(f);
#ifdef _WIN32
    MoveFileExA(tmp, ob.status_path, MOVEFILE_REPLACE_EXISTING);
#else
    rename(tmp, ob.status_path);
#endif
}

static void ob_read_room(double now) {
    if (!ob.room_path[0] || now - ob.room_at < 5.0) return;
    ob.room_at = now;
    FILE *f = fopen(ob.room_path, "rb");
    if (!f) { /* the CLI removes the file when the room is closed or left */
        ob.closed = 1;
        return;
    }
    char buf[8192];
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    VobRoom r;
    ob.members_known = vob_parse_room_ex(buf, ob.cfg.role, &r);
    ob.members = r.members;
    ob.summoned = r.summoned;
    ob.sign = r.sign;
    ob.signs = r.signs;
    ob.in_guests = r.in_guests;
    ob.pending = r.pending;
    if (r.ts > 0.0 && ob_wall() - r.ts > 45.0) { /* the helper stopped: do not trust a frozen view */
        ob.summoned = -1;
        ob.sign = -1;
        ob.signs = -1;
        ob.in_guests = -1;
        ob.pending = -1;
    }
    ob.closed = r.closed || n == 0;
}

/* Called from the pad thread every read. Returns VOB_ACT_NONE / VOB_ACT_RING / VOB_ACT_GIVE; the item for both is
 * runtime_online_bell_item(). bell_count < 0: the inventory is not read yet. acting: the player pressed a button
 * in the last 1.5 s (ignored when direct: the ring uses the game's item-use call, no pad presses). */
int runtime_online_bell_tick(double now, int in_world, int alive, int menu_open, int busy, int acting, int bell_count, int direct) {
    if (!ob.armed) return VOB_ACT_NONE;
    ob_read_room(now);
    ob.in_world = in_world;
    ob.alive = alive;
    ob.bell_count = bell_count;
    VobIn in = {0};
    in.now = now;
    in.in_world = in_world;
    in.alive = alive;
    in.menu_open = menu_open;
    in.busy = busy;
    in.bell_count = bell_count;
    in.members = ob.members_known ? ob.members : 0;
    /* host: the guests really in (summoned && !sign), else the older file's summoned_guests; guest: self_summoned */
    in.summoned = !ob.room_path[0] ? -1 : ob.cfg.role == VOB_HOST && ob.in_guests >= 0 ? ob.in_guests : ob.summoned;
    in.pending = ob.room_path[0] ? ob.pending : -1;
    in.acting = acting;
    in.direct = direct;
    in.net_quiet_s = bbnet_signaling_quiet_s();
    in.np_room = bbnet_matching2_room_role();
    in.sign = ob.room_path[0] ? ob.sign : -1;
    in.signs = ob.room_path[0] ? ob.signs : -1;
    in.room_closed = ob.closed;
    const int act = vob_step(&ob.m, &ob.cfg, &in);
    if (ob.m.state != ob.last_state || ob.m.error != ob.last_error || ob.m.rings != ob.last_rings || act != VOB_ACT_NONE) {
        fprintf(stderr, "Online bell: %s%s%s (rings %d, not reached %d, resets %d, members %d, summoned %d, pending %d, sign %d, np_room %d, arrived %d/need %d, net quiet %.0f s, acting %d, direct %d)%s%s\n", vob_state_name(ob.m.state),
                act == VOB_ACT_RING ? ", ringing now" : act == VOB_ACT_GIVE ? ", giving the missing bell" : "", ob.m.error ? ": " : "",
                ob.m.rings, ob.m.fails, ob.m.resets, in.members, in.summoned, in.pending, in.sign, in.np_room,
                ob.m.arrived, ob.m.need, in.net_quiet_s, in.acting, in.direct, ob.m.error ? " last error: " : "", ob.m.error ? ob.m.error : "");
        fflush(stderr);
        ob.last_state = ob.m.state;
        ob.last_error = ob.m.error;
        ob.last_rings = ob.m.rings;
        ob.status_at = 0.0;
    }
    if (now - ob.status_at >= 5.0) {
        ob.status_at = now;
        ob_write_status(now);
    }
    return act;
}

/* The ring finished: reached 1 = the game took the use, 0 = it did not reach the game, -1 = the game refused it right now (the
 * character is busy: attacking, rolling...). path: "direct" (the game's item-use call) or "pad" (quick slot + button presses),
 * may be NULL. Whether the ring worked is what the room server and the emulator report (sign / summoned / np_room). */
void runtime_online_bell_result(double now, int reached, const char *path) {
    if (!ob.armed) return;
    const int was_ringing = ob.m.state == VOB_RINGING;
    vob_ring_result(&ob.m, &ob.cfg, now, reached);
    if (path) ob.ring_path = strcmp(path, "pad") == 0 ? "pad" : "direct";
    const char *via = path && strcmp(path, "pad") == 0 ? "via pad presses" : "via direct use";
    if (reached < 0) {
        if (now - ob.refuse_log_at >= 5.0 || ob.refuse_log_at == 0.0) {
            fprintf(stderr, "Online bell: ring %d %s: refused by the game (busy character), retry in %.0f s (%d refused so far)\n", ob.m.rings,
                    via, ob.cfg.retry_s, ob.m.refused);
            ob.refuse_log_at = now;
        }
    } else {
        fprintf(stderr, "Online bell: ring %d %s: %s; next ring in %.0f s%s\n", ob.m.rings, via,
                reached ? "accepted by the game" : "did not reach the game", was_ringing ? ob.m.next_at - now : 0.0,
                was_ringing ? "" : " (late result, ignored)");
    }
    fflush(stderr);
    ob.status_at = 0.0;
}
