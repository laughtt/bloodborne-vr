/* Unit tests for src/vr_online_bell.h. Build: ninja -C out/gpu vr-online-bell-test. */
#include "vr_online_bell.h"
#include <stdio.h>
static int failures, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

static VobIn world(double now) { VobIn i = {0}; i.now = now; i.in_world = 1; i.alive = 1; i.bell_count = 1; i.summoned = -1; i.sign = -1; i.signs = -1; i.pending = -1; i.net_quiet_s = -1.0; i.np_room = -1; return i; }

/* a fresh machine, primed at t=1, that rings its first ring at t=10 (idle, settled) and gets "accepted" at t=12: next_at is 32 */
static void first_ring(VobCfg *c, VobMachine *m, int role, int members, int summoned, int pending) {
    vob_cfg_default(c, role);
    vob_init(m, c);
    VobIn in = world(1.0); in.members = members; in.summoned = summoned; in.pending = pending;
    vob_step(m, c, &in);
    in = world(10.0); in.members = members; in.summoned = summoned; in.pending = pending;
    CHECK(vob_step(m, c, &in) == VOB_ACT_RING && m->rings == 1, "first ring at 10 s (role %d)", role);
    vob_ring_result(m, c, 12.0, 1);
}

int main(void) {
    VobCfg c;
    VobMachine m;
    vob_cfg_default(&c, VOB_HOST);
    CHECK(c.gap_s == 20.0 && c.retry_s == 1.0 && c.arrive_s == 20.0 && c.free_s == 10.0 && c.pending_max_s == 120.0 && c.settle_s == 8.0, "defaults");
    vob_init(&m, &c);
    VobIn in = {0};
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_WAIT_WORLD, "no world, no ring");
    in = world(100.0);
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "settling");
    in = world(104.0);
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "still settling at 4 s");
    in = world(104.5); in.menu_open = 1;
    vob_step(&m, &c, &in);
    in = world(109.0);
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "a menu restarts the settle");
    in = world(118.0);
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING && m.state == VOB_RINGING && m.rings == 1, "host rings once idle (member count unknown)");
    in = world(119.0);
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "no second ring while one runs");
    vob_ring_result(&m, &c, 124.0, 1);
    CHECK(m.next_at == 144.0, "next ring in 20 s (%.1f)", m.next_at - 124.0);
    in = world(143.0);
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "waits the gap");
    /* constant 20 s gap, however many rings */
    double t = 144.0;
    for (int k = 2; k <= 6; ++k) {
        in = world(t + 0.1);
        CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING && m.rings == k, "ring %d after the gap", k);
        vob_ring_result(&m, &c, t + 1.0, k % 2); /* accepted and not reached alternate: same gap */
        CHECK(m.next_at == t + 1.0 + 20.0, "ring %d: next in 20 s (%.1f)", k, m.next_at - t - 1.0);
        in = world(t + 20.0);
        CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "ring %d: not 1 s early", k);
        t = t + 21.0;
    }

    /* host dies: start over, settle again */
    in = world(1100.0); in.alive = 0;
    vob_step(&m, &c, &in);
    CHECK(m.state == VOB_WAIT_WORLD && m.resets == 1 && m.settled == 0, "death resets");
    in = world(1101.0);
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "settling after death");
    in = world(1110.0);
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "rings again after the settle");

    /* a ring that never reached the game: the next ring comes after the gap */
    vob_ring_result(&m, &c, 1111.0, 0);
    CHECK(m.fails == 1 && m.error && m.next_at == 1131.0, "press not reached: still the gap (%.1f)", m.next_at - 1111.0);
    in = world(1121.0);
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "no 10 s retry");
    in = world(1131.5);
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "rings");
    in = world(m.ring_at + 40.0);
    vob_step(&m, &c, &in);
    CHECK(m.state == VOB_READY && m.fails == 2 && m.next_at == m.ring_at + 40.0 + 20.0, "no result in 30 s counts as not reached (%d)", m.fails);

    /* refused by the game (busy character): retry after 1 s, not counted as a failure */
    first_ring(&c, &m, VOB_HOST, 3, 0, 0);
    in = world(32.0); in.members = 3; in.summoned = 0; in.pending = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING && m.rings == 2, "refused test: second ring");
    vob_ring_result(&m, &c, 32.2, -1);
    CHECK(m.refused == 1 && m.fails == 0 && !m.error && m.state == VOB_READY && m.next_at == 33.2, "refused: retry in 1 s (%.2f, fails %d)", m.next_at, m.fails);
    in = world(33.0); in.members = 3; in.summoned = 0; in.pending = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "not before the retry time");
    in = world(33.3); in.members = 3; in.summoned = 0; in.pending = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING && m.rings == 3, "retry after 1 s, no 20 s wait");
    vob_ring_result(&m, &c, 33.5, -1);
    CHECK(m.refused == 2 && m.next_at == 34.5, "refused again");
    in = world(34.6); in.members = 3; in.summoned = 0; in.pending = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "third try");
    vob_ring_result(&m, &c, 34.8, 1);
    CHECK(m.refused == 2 && m.next_at == 54.8, "accepted: back to the 20 s gap (%.1f)", m.next_at);

    /* missing bell */
    vob_init(&m, &c);
    in = world(10.0); vob_step(&m, &c, &in);
    in = world(20.0); in.bell_count = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_NEED_BELL && m.error, "missing bell: logged, nothing given");
    c.allow_give = 1;
    in = world(21.0); in.bell_count = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_GIVE, "dev flag gives it");
    in = world(22.0); in.bell_count = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "not again at once");
    in = world(23.0); in.bell_count = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "rings once the bell is there");
    c.allow_give = 0;
    CHECK(vob_bell_id(VOB_HOST) == 0x400000C8u && vob_bell_id(VOB_GUEST) == 0x400000CDu, "bell ids");

    /* host with the server's arrived count: full at 2 guests, a freed slot rings 10 s after */
    first_ring(&c, &m, VOB_HOST, 3, 0, 0);
    CHECK(m.need == 2 && m.arrived == 0, "need 2, arrived 0 (%d %d)", m.need, m.arrived);
    in = world(100.0); in.members = 3; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_READY, "one arrived: hold the bell for the arrival");
    in = world(119.0); in.members = 3; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "arrival hold is 20 s");
    in = world(120.5); in.members = 3; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "ring for the second guest");
    vob_ring_result(&m, &c, 122.0, 1);
    in = world(140.0); in.members = 3; in.summoned = 2;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_FULL, "two guests arrived: full");
    CHECK(!strcmp(vob_state_name(VOB_FULL), "room_full"), "room_full name");
    in = world(1000.0); in.members = 3; in.summoned = 2;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_FULL, "stays full");
    in = world(1010.0); in.members = 3; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_READY, "an arrived guest left");
    in = world(1019.0); in.members = 3; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "not before 10 s");
    in = world(1020.5); in.members = 3; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "rings again 10 s after the leave");
    vob_ring_result(&m, &c, 1022.0, 0);
    in = world(1030.0); in.members = 3; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "then the gap, no fast retry");
    in = world(1031.0); in.members = 2; in.summoned = 1; in.room_closed = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_CLOSED, "closed room stops the host");

    /* all_here: everyone who joined the room is in the host's world; no ring until another member joins */
    first_ring(&c, &m, VOB_HOST, 2, 0, 0);
    CHECK(m.need == 1, "members 2: need 1");
    in = world(32.0); in.members = 2; in.summoned = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "member joined the room, not here yet: rings");
    vob_ring_result(&m, &c, 33.0, 1);
    in = world(40.0); in.members = 2; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_ALL_IN, "2 members, 1 arrived: all here");
    CHECK(!strcmp(vob_state_name(VOB_ALL_IN), "all_here"), "all_here name");
    in = world(300.0); in.members = 2; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_ALL_IN, "all_here: no ring however long");
    in = world(301.0); in.members = 3; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING && m.state == VOB_RINGING, "a third member joined: rings again");
    vob_ring_result(&m, &c, 302.0, 1);
    in = world(330.0); in.members = 3; in.summoned = 2;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_FULL, "both here: room_full");
    in = world(340.0); in.members = 1; in.summoned = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_ALL_IN, "host alone in the room: all_here");
    /* the member count is not in the file: keep ringing up to the full room */
    in = world(400.0); in.members = 0; in.summoned = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "members unknown: rings (need %d)", m.need);
    /* older room file without the arrived count: full at members >= max */
    first_ring(&c, &m, VOB_HOST, 0, -1, -1);
    in = world(32.0); in.members = 2;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "no arrived count: a member counts as arrived, 20 s hold");
    in = world(53.0); in.members = 2;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "no arrived count, 2 members: rings");
    vob_ring_result(&m, &c, 54.0, 1);
    in = world(100.0); in.members = 3;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_FULL, "no arrived count, 3 members: full");

    /* a guest arrives (really in): 20 s hold, then the next ring */
    first_ring(&c, &m, VOB_HOST, 3, 0, 0);
    in = world(100.0); in.members = 3; in.summoned = 1; in.pending = 0;
    vob_step(&m, &c, &in);
    CHECK(m.next_at >= 120.0, "arrival: 20 s hold (%.1f)", m.next_at);
    in = world(119.0); in.members = 3; in.summoned = 1; in.pending = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "still in the arrival window");
    in = world(121.0); in.members = 3; in.summoned = 1; in.pending = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "ring after the arrival hold");
    vob_ring_result(&m, &c, 132.0, 1);
    CHECK(m.next_at == 152.0, "20 s apart (%.1f)", m.next_at - 132.0);

    /* summon in progress (pending): no ring, state summoning, even past next_at */
    first_ring(&c, &m, VOB_HOST, 3, 0, 0);
    in = world(100.0); in.members = 3; in.summoned = 0; in.pending = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SUMMONING && m.next_at >= 120.0, "pending: no ring after next_at, summoning");
    CHECK(!strcmp(vob_state_name(VOB_SUMMONING), "summoning") && !strcmp(vob_state_name(VOB_SUMMONED), "summoned"), "state names");
    in = world(200.0); in.members = 3; in.summoned = 0; in.pending = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SUMMONING && m.next_at >= 220.0, "pending 100 s: still held");
    in = world(215.0); in.members = 3; in.summoned = 0; in.pending = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SUMMONING, "pending 115 s: still held");
    in = world(221.0); in.members = 3; in.summoned = 0; in.pending = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_READY, "pending over 120 s: hold ends, next_at still ahead");
    in = world(236.0); in.members = 3; in.summoned = 0; in.pending = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "pending stuck: rings again");
    /* pending cleared: the arrival window */
    first_ring(&c, &m, VOB_HOST, 3, 0, 0);
    in = world(100.0); in.members = 3; in.summoned = 0; in.pending = 1; vob_step(&m, &c, &in);
    in = world(110.0); in.members = 3; in.summoned = 0; in.pending = 0; /* the summon failed, nobody in */
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_READY && m.next_at == 130.0, "pending cleared: 20 s hold (%.1f)", m.next_at);
    in = world(129.0); in.members = 3; in.summoned = 0; in.pending = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "hold not over at 19 s");
    in = world(131.0); in.members = 3; in.summoned = 0; in.pending = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "ring after the 20 s hold");
    /* pending guests do not count as arrived */
    vob_init(&m, &c);
    in = world(1.0); in.members = 3; in.summoned = 0; in.pending = 2; vob_step(&m, &c, &in);
    in = world(10.0); in.members = 3; in.summoned = 0; in.pending = 2;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SUMMONING, "two pending: not full, summoning");
    in = world(20.0); in.members = 3; in.summoned = 2; in.pending = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_FULL, "two arrived: full");

    /* NP signaling activity holds the ring (both roles) */
    vob_init(&m, &c);
    in = world(1.0); vob_step(&m, &c, &in);
    in = world(10.0); in.net_quiet_s = 5.0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_READY, "signaling 5 s ago: no ring");
    in = world(11.0); in.net_quiet_s = 0.0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "handshake running: no ring");
    in = world(12.0); in.net_quiet_s = 25.0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "signaling quiet 25 s: ring");
    vob_init(&m, &c);
    in = world(1.0); in.net_quiet_s = -1.0; vob_step(&m, &c, &in);
    in = world(10.0); in.net_quiet_s = -1.0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "signaling unknown: ring");
    {
        VobCfg gc; VobMachine gm;
        vob_cfg_default(&gc, VOB_GUEST); vob_init(&gm, &gc);
        in = world(1.0); vob_step(&gm, &gc, &in);
        in = world(10.0); in.net_quiet_s = 5.0;
        CHECK(vob_step(&gm, &gc, &in) == VOB_ACT_NONE, "guest: signaling 5 s ago: no ring");
        in = world(11.0); in.net_quiet_s = 25.0;
        CHECK(vob_step(&gm, &gc, &in) == VOB_ACT_RING, "guest: quiet: ring");
    }
    /* the player pressing buttons holds a pad-press ring (direct 0) but not a direct use; neither restarts the settle timer */
    vob_init(&m, &c);
    in = world(1.0); in.acting = 1; vob_step(&m, &c, &in);
    in = world(5.0); in.acting = 1; vob_step(&m, &c, &in);
    in = world(10.0); in.acting = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_READY, "acting, pad path: settled anyway, no ring");
    in = world(10.2); in.acting = 1; in.direct = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "acting, direct 0: still no ring");
    in = world(10.5); in.acting = 1; in.direct = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "acting, direct 1: rings while the player keeps moving");
    vob_init(&m, &c);
    in = world(1.0); in.acting = 1; in.direct = 1; vob_step(&m, &c, &in);
    in = world(10.0); in.acting = 1; in.direct = 1; in.net_quiet_s = 5.0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "direct: the NP signaling hold still applies");
    vob_init(&m, &c);
    in = world(1.0); vob_step(&m, &c, &in);
    in = world(10.0);
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "ring once the player stops pressing");

    /* sign counts from the room file */
    CHECK(vob_count_signs("{\"members\":[{\"role\":\"host\",\"sign\":true},{\"role\":\"guest\",\"sign\":false}]}") == 0, "host's own sign is not a guest's");
    CHECK(vob_count_signs("{\"members\":[{\"role\":\"host\",\"sign\":false},{\"role\":\"guest\",\"sign\":true},{\"role\":\"guest\",\"sign\":true}]}") == 2, "two guest signs");
    CHECK(vob_count_signs("{\"members\":[{\"role\":\"host\"},{\"role\":\"guest\"}]}") == -1, "no sign keys: unknown");
    CHECK(vob_count_signs("{\"members\":2}") == -1, "no array: unknown");
    {
        int s, i, p;
        vob_count_members("{\"members\":[{\"role\":\"host\",\"sign\":false,\"summoned\":true},{\"role\":\"guest\",\"sign\":true,\"summoned\":true},"
                          "{\"role\":\"guest\",\"sign\":false,\"summoned\":true},{\"role\":\"guest\",\"sign\":true,\"summoned\":false}]}", &s, &i, &p);
        CHECK(s == 2 && i == 1 && p == 1, "members: signs %d in %d pending %d", s, i, p);
        vob_count_members("{\"members\":[{\"role\":\"host\",\"sign\":false},{\"role\":\"guest\",\"sign\":true}]}", &s, &i, &p);
        CHECK(s == 1 && i == -1 && p == -1, "no summoned key: in/pending unknown (%d %d %d)", s, i, p);
        vob_count_members("{\"members\":3}", &s, &i, &p);
        CHECK(s == -1 && i == -1 && p == -1, "no array: all unknown");
        /* with "world": arrived = summoned && world host (world host alone also counts); pending = summoned && world not host */
        vob_count_members("{\"members\":[{\"role\":\"host\",\"sign\":false,\"summoned\":false,\"world\":\"own\"},"
                          "{\"role\":\"guest\",\"sign\":false,\"summoned\":true,\"world\":\"host\"},"
                          "{\"role\":\"guest\",\"sign\":true,\"summoned\":true,\"world\":\"own\"},"
                          "{\"role\":\"guest\",\"sign\":false,\"summoned\":false,\"world\":\"host\"},"
                          "{\"role\":\"guest\",\"sign\":false,\"summoned\":false,\"world\":\"own\"}]}", &s, &i, &p);
        CHECK(i == 2 && p == 1 && s == 1, "world keys: in %d pending %d signs %d", i, p, s);
        vob_count_members("{\"members\":[{\"role\":\"host\",\"world\":\"host\"},{\"role\":\"guest\",\"sign\":false,\"summoned\":true,\"world\":\"own\"}]}", &s, &i, &p);
        CHECK(i == 0 && p == 1, "summoned but still in its own world: pending, the old rule would say in (%d %d)", i, p);
        /* without "world": the old rule */
        vob_count_members("{\"members\":[{\"role\":\"host\"},{\"role\":\"guest\",\"sign\":false,\"summoned\":true},"
                          "{\"role\":\"guest\",\"sign\":true,\"summoned\":true},{\"role\":\"guest\",\"sign\":false,\"summoned\":false}]}", &s, &i, &p);
        CHECK(i == 1 && p == 1, "no world key: old rule (%d %d)", i, p);
        /* mixed: members without the key follow the old rule */
        vob_count_members("{\"members\":[{\"role\":\"host\"},{\"role\":\"guest\",\"sign\":false,\"summoned\":true,\"world\":\"own\"},"
                          "{\"role\":\"guest\",\"sign\":false,\"summoned\":true}]}", &s, &i, &p);
        CHECK(i == 1 && p == 1, "mixed (%d %d)", i, p);
        VobRoom rr;
        vob_parse_room_ex("{\"members\":[{\"role\":\"host\"},{\"role\":\"guest\",\"sign\":false,\"summoned\":true,\"world\":\"host\"}]}", VOB_HOST, &rr);
        CHECK(rr.in_guests == 1 && rr.pending == 0, "ex: world host is in (%d %d)", rr.in_guests, rr.pending);
    }

    /* guest: rings every 20 s while it is not in the host's world and has no sign up */
    first_ring(&c, &m, VOB_GUEST, 3, 0, -1);
    CHECK(m.next_at == 32.0, "guest: next ring in 20 s");
    {
        double gt = 32.0;
        for (int k = 2; k <= 4; ++k) {
            in = world(gt + 0.1); in.sign = 0; in.summoned = 0; in.np_room = k % 2; /* no room / own room */
            CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING && m.rings == k, "guest ring %d", k);
            vob_ring_result(&m, &c, gt + 1.0, 1);
            CHECK(m.next_at == gt + 21.0, "guest: constant gap (%.1f)", m.next_at - gt - 1.0);
            gt += 21.0;
        }
        in = world(gt + 0.1); in.sign = 0; in.summoned = 0; in.np_room = 2;
        CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SUMMONED, "guest: np_room 2 (in the host's world): no ring");
        in = world(gt + 500.0); in.sign = 0; in.summoned = 0; in.np_room = 2;
        CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SUMMONED, "guest: stays summoned");
        in = world(gt + 510.0); in.sign = 0; in.summoned = 0; in.np_room = 0; /* back home */
        CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_WAIT_WORLD, "guest: home again, settles");
        in = world(gt + 519.0); in.sign = 0; in.summoned = 0; in.np_room = 0;
        CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "guest: rings again after the settle");
        vob_ring_result(&m, &c, gt + 520.0, 1);
        in = world(gt + 530.0); in.sign = 1; in.summoned = 0; in.np_room = 1;
        CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SIGN_UP, "guest: sign up: no ring");
        in = world(gt + 900.0); in.sign = 1; in.summoned = 0; in.np_room = 1;
        CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SIGN_UP, "guest: sign up, however long");
        in = world(gt + 910.0); in.sign = 0; in.summoned = 0; in.np_room = 1;
        CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_READY, "guest: sign gone: ready");
        in = world(gt + 915.0); in.sign = 0; in.summoned = 0; in.np_room = 1;
        CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE, "guest: free_s wait");
        in = world(gt + 920.5); in.sign = 0; in.summoned = 0; in.np_room = 1;
        CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "guest: rings 10 s after the sign went");
    }
    /* guest: the room size does not matter, never FULL; a refusal is retried after 1 s */
    first_ring(&c, &m, VOB_GUEST, 3, -1, -1);
    in = world(32.0); in.members = 3;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "guest rings, room size does not matter");
    vob_ring_result(&m, &c, 32.5, -1);
    in = world(33.6); in.members = 3;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING && m.refused == 1, "guest: retry after a refusal");
    /* guest: sent home after the first ring, settles and rings again */
    vob_cfg_default(&c, VOB_GUEST);
    vob_init(&m, &c);
    in = world(1.0); in.members = 3; vob_step(&m, &c, &in);
    in = world(10.0); in.members = 3;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "guest rings");
    vob_ring_result(&m, &c, 14.0, 1);
    in = world(20.0); in.in_world = 0;
    vob_step(&m, &c, &in);
    in = world(30.0); vob_step(&m, &c, &in);
    in = world(39.0);
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING && m.resets == 1, "sent home: rings again");
    in = world(40.0); in.room_closed = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_CLOSED, "room closed stops it");

    /* guest: the server's sign decides */
    vob_cfg_default(&c, VOB_GUEST);
    vob_init(&m, &c);
    in = world(1.0); in.sign = 0; in.summoned = 0; vob_step(&m, &c, &in);
    in = world(10.0); in.sign = 0; in.summoned = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "guest rings");
    vob_ring_result(&m, &c, 14.0, 0); /* the local press check said "no": irrelevant */
    in = world(20.0); in.sign = 1; in.summoned = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SIGN_UP && !m.error, "sign is up: waits");
    in = world(500.0); in.sign = 1; in.summoned = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SIGN_UP, "keeps waiting however long");
    in = world(510.0); in.sign = 0; in.summoned = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_READY, "sign gone: ready, short wait");
    in = world(521.0); in.sign = 0; in.summoned = 0;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "rings again");
    vob_ring_result(&m, &c, 523.0, 1);
    in = world(530.0); in.sign = 1; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SUMMONED, "summoned: nothing to ring");
    in = world(535.0); in.in_world = 0; in.sign = 0; in.summoned = 1; /* loading into the host's world */
    vob_step(&m, &c, &in);
    CHECK(m.state == VOB_SUMMONED && m.summon_resets == 1, "arrival keeps the state");
    in = world(540.0); in.sign = 0; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SUMMONED, "in the host's world");
    in = world(700.0); in.in_world = 0; in.sign = 0; in.summoned = 1; /* sent home */
    vob_step(&m, &c, &in);
    CHECK(m.stale_summon == 1 && m.state == VOB_WAIT_WORLD, "way home: the summoned flag is stale");
    in = world(715.0); in.sign = 0; in.summoned = 1; vob_step(&m, &c, &in);
    in = world(724.0); in.sign = 0; in.summoned = 1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "home again: rings although the server still says summoned");
    vob_ring_result(&m, &c, 725.0, 1);
    in = world(730.0); in.sign = 1; in.summoned = 0; /* a new sign clears the server's flag */
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_NONE && m.state == VOB_SIGN_UP && m.stale_summon == 0, "flag cleared, sign up");
    /* the summon was seen only after the guest had already loaded into the host's world */
    in = world(800.0); in.sign = 1; in.summoned = 1; vob_step(&m, &c, &in);
    CHECK(m.state == VOB_SUMMONED, "summoned (late seen)");
    in = world(900.0); in.in_world = 0; in.summoned = 1; vob_step(&m, &c, &in);
    CHECK(m.stale_summon == 1, "a change long after the summon was seen is the way home");
    /* unknown (no fresh room file): the guest just rings on its gaps */
    vob_init(&m, &c);
    in = world(1.0); in.sign = -1; in.summoned = -1; vob_step(&m, &c, &in);
    in = world(10.0); in.sign = -1; in.summoned = -1;
    CHECK(vob_step(&m, &c, &in) == VOB_ACT_RING, "unknown state: rings");

    int mem, closed;
    CHECK(vob_parse_room("{\"code\":\"K7QM4X\",\"members\": 2}", &mem, &closed) && mem == 2 && !closed, "members number");
    CHECK(vob_parse_room("{\"members\":[{\"npid\":\"a\",\"role\":\"host\"},{\"npid\":\"b\",\"x\":[1,2]}],\"closed\": false}", &mem, &closed) && mem == 2 && !closed, "members array (%d)", mem);
    CHECK(!vob_parse_room("{\"code\":\"X\"}", &mem, &closed) && mem == 0, "no members key");
    vob_parse_room("{\"closed\": true}", &mem, &closed);
    CHECK(closed, "closed");
    vob_parse_room("{\"ok\":true,\"in_room\":false}", &mem, &closed);
    CHECK(closed, "not in a room");
    VobRoom r;
    const char *file = "{\"code\":\"K7Q-M4X\",\"members\":[{\"name\":\"a\",\"role\":\"host\",\"sign\":false,\"summoned\":false},"
                       "{\"name\":\"b\",\"role\":\"guest\",\"sign\":true,\"summoned\":false}],\"closed\":false,"
                       "\"summoned_guests\":1,\"self_sign\":true,\"self_summoned\":false,\"ts\":1700000000}";
    CHECK(vob_parse_room_ex(file, VOB_HOST, &r) && r.members == 2 && !r.closed && r.summoned == 1 && r.sign == 1 && r.ts == 1700000000.0, "ex host (%d %d %d)", r.members, r.summoned, r.sign);
    vob_parse_room_ex(file, VOB_GUEST, &r);
    CHECK(r.summoned == 0 && r.sign == 1, "ex guest: self_summoned false");
    vob_parse_room_ex(file, VOB_HOST, &r);
    CHECK(r.signs == 1, "ex host: one guest sign (%d)", r.signs);
    CHECK(r.in_guests == 0 && r.pending == 0, "ex host: nobody in, none pending (%d %d)", r.in_guests, r.pending);
    vob_parse_room_ex("{\"members\":[{\"role\":\"host\"},{\"role\":\"guest\",\"sign\":true,\"summoned\":true},{\"role\":\"guest\",\"sign\":false,\"summoned\":true}]}", VOB_HOST, &r);
    CHECK(r.in_guests == 1 && r.pending == 1 && r.signs == 1, "ex host: one in, one pending (%d %d %d)", r.in_guests, r.pending, r.signs);
    vob_parse_room_ex("{\"members\":[{\"role\":\"host\",\"sign\":false},{\"role\":\"guest\",\"sign\":true}]}", VOB_HOST, &r);
    CHECK(r.in_guests == -1 && r.pending == -1 && r.signs == 1, "ex host: no summoned key");
    vob_parse_room_ex("{\"members\":2}", VOB_GUEST, &r);
    CHECK(r.summoned == -1 && r.sign == -1 && r.signs == -1 && r.in_guests == -1 && r.pending == -1 && r.ts == 0.0, "ex: fields absent");

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
