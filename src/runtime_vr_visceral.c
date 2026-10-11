/* VR visceral (docs/vr/findings/41-parry.md section 7): in Bloodborne a visceral is R1 pressed in
 * front of an enemy in the riposte (parried) state; the game decides. The runtime cannot read the
 * enemy's state, so the fallback is a window: a player firearm shot that has a target (the lock-on
 * target, or else the lock-on candidate the controller ray points at) opens it (vr_visceral_on_shot,
 * from the bullet launch hook in src/runtime_vr_gun.c); inside it the player's R1 is pressed by the
 * pad thread (vr_visceral_pad, called from runtime_pad.c sample()) when
 *   auto    the head or the right hand is within reach of the target;
 *   thrust  the right hand thrusts at the target (src/vr_visceral_math.h);
 *   swing   never (the swing gesture of the bridge sends R1 by itself).
 * False positive: a normal R1 attack, when the shot did not parry.
 *
 * Environment:
 *   BB_VR_VISCERAL_MODE=auto|thrust|swing   (default auto)
 *   BB_VR_VISCERAL_CONFIRM=aim|hit          aim (default): the shot's target is enough; hit: a player
 *                                           character hit within 600 ms of the shot must follow
 *   BB_VR_VISCERAL_SPEED=1.5     thrust speed along the hand's aim, m/s (0.1..10)
 *   BB_VR_VISCERAL_ALIGN=0.7     cosine of the stroke to the target (-1..0.999)
 *   BB_VR_VISCERAL_REARM=0.5     m/s below which the thrust re-arms
 *   BB_VR_VISCERAL_REACH_M=2.0   horizontal reach to the target (0.1..5)
 *   BB_VR_VISCERAL_DEBOUNCE_MS=300  (0..3000)
 *   BB_VR_VISCERAL_WINDOW_MS=2500   the stagger window; after it nothing is tried (200..10000)
 *   BB_VR_VISCERAL_DELAY_MS=400     no R1 earlier than this after the shot (0..2000)
 *   BB_VR_VISCERAL_LOG=1         the "VR visceral:" lines (default on) */
#define _GNU_SOURCE
#include "vr_gun.h"
#include "vr_gun_math.h"
#include "vr_visceral.h"
#include "vr_fp_math.h"
#include "vr_visceral_math.h"
#include "vr_fp.h"
#include "gpu/xr_bridge/vr_held_fire.h"
#include "gpu/bbgpu.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VIS_BTN_R1 0x800u
#define VIS_HIST 24
#define VIS_STALE_US 300000u
#define VIS_TRACK_MS 50.0
#define VIS_VEL_MIN_AGE_NS 30000000
#define VIS_HIST_KEEP_NS 250000000
#define VIS_TURN_BLOCK_DEG 25.0f
#define VIS_JUMP_BLOCK_M 0.25f

typedef struct {
    int64_t t_ns;
    VrVec3 hand, head; /* OpenXR space */
} VisSample;

static struct {
    VvConfig cfg;
    VvState st;
    int log, active;
    /* pad thread only */
    VisSample hist[VIS_HIST];
    int hist_n;
    int64_t hands_ns;
    int have_prev;
    float prev_cam_yaw, prev_xr_yaw;
    VrVec3 prev_cam_pos;
    double next_track_ms, no_target_log_ms;
    int window_seen; /* the pad thread saw the window open (resets the history) */
} vis;
static atomic_flag vis_lock = ATOMIC_FLAG_INIT;
static void vis_acquire(void) { while (atomic_flag_test_and_set_explicit(&vis_lock, memory_order_acquire)) {} }
static void vis_release(void) { atomic_flag_clear_explicit(&vis_lock, memory_order_release); }
static _Atomic int vis_window_flag; /* mirrors st.open || a press in progress, read without the lock */

static double vis_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec * 1e-6;
}

static float vis_env(const char *name, float def, float lo, float hi) {
    const char *v = getenv(name);
    if (!v || !*v) return def;
    return vr_clampf((float)atof(v), lo, hi);
}

int vr_visceral_active(void) { return vis.active; }

void vr_visceral_register(void) {
    vis.cfg = vv_defaults();
    vv_init(&vis.st);
    const char *v = getenv("BB_VR_VISCERAL_MODE");
    vis.cfg.mode = v && !strcmp(v, "thrust") ? VV_MODE_THRUST : v && !strcmp(v, "swing") ? VV_MODE_SWING : VV_MODE_AUTO;
    v = getenv("BB_VR_VISCERAL_CONFIRM");
    vis.cfg.confirm = v && !strcmp(v, "hit") ? VV_CONFIRM_HIT : VV_CONFIRM_AIM;
    vis.cfg.speed_mps = vis_env("BB_VR_VISCERAL_SPEED", vis.cfg.speed_mps, 0.1f, 10.0f);
    vis.cfg.align = vis_env("BB_VR_VISCERAL_ALIGN", vis.cfg.align, -1.0f, 0.999f);
    vis.cfg.rearm = vis_env("BB_VR_VISCERAL_REARM", vis.cfg.rearm, 0.0f, 10.0f);
    vis.cfg.reach_m = vis_env("BB_VR_VISCERAL_REACH_M", vis.cfg.reach_m, 0.1f, 5.0f);
    vis.cfg.debounce_ms = vis_env("BB_VR_VISCERAL_DEBOUNCE_MS", vis.cfg.debounce_ms, 0.0f, 3000.0f);
    vis.cfg.window_ms = vis_env("BB_VR_VISCERAL_WINDOW_MS", vis.cfg.window_ms, 200.0f, 10000.0f);
    vis.cfg.delay_ms = vis_env("BB_VR_VISCERAL_DELAY_MS", vis.cfg.delay_ms, 0.0f, 2000.0f);
    v = getenv("BB_VR_VISCERAL_LOG");
    vis.log = !(v && !strcmp(v, "0"));
    vis.active = vis.cfg.mode != VV_MODE_SWING;
    fprintf(stderr,
            "VR visceral: mode %s, confirm %s, window %.0f-%.0f ms after a parry-candidate shot, reach %.2f m, "
            "thrust %.2f m/s align %.2f rearm %.2f debounce %.0f ms%s\n",
            vv_mode_name(vis.cfg.mode), vis.cfg.confirm == VV_CONFIRM_HIT ? "hit" : "aim", vis.cfg.delay_ms, vis.cfg.window_ms,
            vis.cfg.reach_m, vis.cfg.speed_mps, vis.cfg.align, vis.cfg.rearm, vis.cfg.debounce_ms,
            vis.active ? "" : " (the swing gesture sends R1: nothing to do here)");
}

/* Logs the events a locked section produced (call after releasing the lock). */
static void vis_log_close(int reason) {
    if (!vis.log || reason == VV_CLOSE_NONE || reason == VV_CLOSE_FIRED) return;
    fprintf(stderr, "VR visceral: window closed (%s)\n", vv_close_name(reason));
}

void vr_visceral_on_shot(int how, const float target[3], float dist_m) {
    if (!vis.active) return;
    const double now = vis_now_ms();
    vis_acquire();
    const VrVec3 t = how ? vr_v3(target[0], target[1], target[2]) : vr_v3(0, 0, 0);
    const int r = vv_on_shot(&vis.st, &vis.cfg, now, how != 0, t);
    const int closed = vv_take_close(&vis.st);
    atomic_store_explicit(&vis_window_flag, vis.st.open, memory_order_release);
    vis_release();
    if (!vis.log) return;
    if (closed == VV_CLOSE_REPLACED || closed == VV_CLOSE_NO_TARGET) vis_log_close(closed);
    if (r == VV_SHOT_OPENED || r == VV_SHOT_REPLACED)
        fprintf(stderr, "VR visceral: window opened (%s), target %.1f m away, confirm: %s; live %.0f-%.0f ms after the shot\n",
                how == 1 ? "locked on" : "aimed at a lock-on candidate", dist_m,
                vis.cfg.confirm == VV_CONFIRM_HIT ? "a player hit within 600 ms" : "none (the shot's target is enough)",
                vis.cfg.delay_ms, vis.cfg.window_ms);
    else if (r == VV_SHOT_NO_TARGET && now - vis.no_target_log_ms > 3000.0) {
        vis.no_target_log_ms = now;
        fprintf(stderr, "VR visceral: shot without a target (not locked on, controller ray at no candidate): no window\n");
    }
}

void vr_visceral_note_hit(int atk_id, const float point[3]) {
    if (!vis.active || !atomic_load_explicit(&vis_window_flag, memory_order_acquire)) return;
    const double now = vis_now_ms();
    vis_acquire();
    const double after = now - vis.st.opened_ms;
    const int r = vv_on_hit(&vis.st, &vis.cfg, now, vr_v3(point[0], point[1], point[2]));
    vis_release();
    if (r && vis.log)
        fprintf(stderr, "VR visceral: confirmed by a player hit (atk %d) %.0f ms after the shot\n", atk_id, after);
}

/* The right hand and the head from the controller snapshot, in the world through the first-person
 * camera; fills `in` (the velocity from the history). */
static int vis_gather(VvInput *in, double now_ms, int *fresh) {
    *fresh = 0;
    float rows[4][4];
    uint64_t age = 0;
    BbVrHands h;
    if (!vr_gun_view_latest(rows, &age) || !bbgpu_vr_hands(&h)) return 0;
    if (h.time_ns == vis.hands_ns) return 0;
    if (!(h.valid & 4u) || !(h.valid & 2u)) return 0;
    vis.hands_ns = h.time_ns;
    *fresh = 1;
    const VrQuat head_q = vr_q(h.head.orientation[0], h.head.orientation[1], h.head.orientation[2], h.head.orientation[3]);
    const BbVrPose *ap = (h.valid & 16u) ? &h.aim[1] : &h.grip[1];
    const VrQuat aim_q = vr_q(ap->orientation[0], ap->orientation[1], ap->orientation[2], ap->orientation[3]);
    if (!vr_qvalid(head_q) || !vr_qvalid(aim_q)) return 0;
    const VrVec3 head_xr = vr_v3(h.head.position[0], h.head.position[1], h.head.position[2]);
    const VrVec3 hand_xr = vr_v3(h.grip[1].position[0], h.grip[1].position[1], h.grip[1].position[2]);
    const VrQuat hconj = vr_qconj(vr_qnormalize(head_q));
    const VrVec3 cam = vr_v3(rows[3][0], rows[3][1], rows[3][2]);
    in->now_ms = now_ms;
    in->head_valid = in->hand_valid = 1;
    in->head = cam;
    in->hand = vg_rel_pos_to_world(vr_qrotate(hconj, vr_sub(hand_xr, head_xr)), (const float (*)[4])rows, cam);
    in->hand_fwd = vg_aim_in_world(head_q, aim_q, (const float (*)[4])rows);
    /* velocity: the newest sample at least 30 ms old; the hand minus the head, in the head's frame */
    if (vis.hist_n >= VIS_HIST) {
        memmove(vis.hist, vis.hist + 1, sizeof(vis.hist[0]) * (VIS_HIST - 1));
        vis.hist_n = VIS_HIST - 1;
    }
    vis.hist[vis.hist_n++] = (VisSample){h.time_ns, hand_xr, head_xr};
    while (vis.hist_n > 2 && h.time_ns - vis.hist[1].t_ns > VIS_HIST_KEEP_NS) {
        memmove(vis.hist, vis.hist + 1, sizeof(vis.hist[0]) * (size_t)(vis.hist_n - 1));
        --vis.hist_n;
    }
    const VisSample *ref = NULL;
    for (int i = 0; i < vis.hist_n - 1; ++i)
        if (h.time_ns - vis.hist[i].t_ns >= VIS_VEL_MIN_AGE_NS) ref = &vis.hist[i];
    if (ref) {
        const float dt = (float)(h.time_ns - ref->t_ns) * 1e-9f;
        const VrVec3 rel_xr = vr_scale(vr_sub(vr_sub(hand_xr, ref->hand), vr_sub(head_xr, ref->head)), 1.0f / dt);
        in->rel_vel = vg_rel_dir_to_world(vr_qrotate(hconj, rel_xr), (const float (*)[4])rows);
        in->have_rel = 1;
    }
    /* blocked: a snap turn / recenter / view jump between two steps */
    const float cam_yaw = vr_cam_heading((const float (*)[4])rows), xr_yaw = vr_xr_yaw(head_q);
    if (vis.have_prev) {
        const float dc = fabsf(vr_wrap_pi(cam_yaw - vis.prev_cam_yaw)), dx = fabsf(vr_wrap_pi(xr_yaw - vis.prev_xr_yaw));
        in->blocked = dc > VIS_TURN_BLOCK_DEG * VR_DEG || dx > VIS_TURN_BLOCK_DEG * VR_DEG ||
                      vam_len(vr_sub(cam, vis.prev_cam_pos)) > VIS_JUMP_BLOCK_M;
    }
    vis.prev_cam_yaw = cam_yaw;
    vis.prev_xr_yaw = xr_yaw;
    vis.prev_cam_pos = cam;
    vis.have_prev = 1;
    return 1;
}

static uint32_t vis_pad_raw(void) {
    if (!vis.active) return 0;
    const double now = vis_now_ms();
    vis_acquire();
    const int pressed = vv_pressed(&vis.st, now);
    const int open = vis.st.open;
    vis_release();
    if (!open) {
        if (vis.window_seen) { vis.window_seen = 0; vis.hist_n = 0; vis.have_prev = 0; }
        return pressed ? VIS_BTN_R1 : 0;
    }
    if (!vis.window_seen) { vis.window_seen = 1; vis.hist_n = 0; vis.have_prev = 0; vis.hands_ns = 0; vis.next_track_ms = 0.0; }
    /* the enemy moves: follow it */
    if (now >= vis.next_track_ms) {
        vis.next_track_ms = now + VIS_TRACK_MS;
        float pts[32][3];
        const int n = vr_gun_candidate_points(pts, 32);
        if (n > 0) {
            VrVec3 c[32];
            for (int i = 0; i < n; ++i) c[i] = vr_v3(pts[i][0], pts[i][1], pts[i][2]);
            vis_acquire();
            vv_track(&vis.st, &vis.cfg, c, n);
            vis_release();
        }
    }
    VvInput in;
    memset(&in, 0, sizeof(in));
    in.now_ms = now;
    int fresh = 0;
    const int ok = vis_gather(&in, now, &fresh);
    if (!ok) {
        /* no new data: only the expiry needs to run (stale or missing data cannot fire) */
        if (!fresh) {
            vis_acquire();
            const int expired = vis.st.open && now - vis.st.opened_ms > (double)vis.cfg.window_ms;
            vis_release();
            if (!expired) return pressed ? VIS_BTN_R1 : 0;
        }
        memset(&in, 0, sizeof(in));
        in.now_ms = now;
    }
    vis_acquire();
    const int fire = vv_step(&vis.st, &vis.cfg, &in);
    const int closed = vv_take_close(&vis.st);
    const int report = vv_take_report(&vis.st);
    const VvState snap = vis.st;
    atomic_store_explicit(&vis_window_flag, vis.st.open || vv_pressed(&vis.st, now), memory_order_release);
    vis_release();
    if (vis.log) {
        if (fire) {
            if (snap.fired_how == VV_MODE_AUTO)
                fprintf(stderr, "VR visceral: R1 sent (auto: the %s %.2f m from the target <= reach %.2f m)\n",
                        snap.fired_by_hand ? "hand" : "head", snap.fired_dist, vis.cfg.reach_m);
            else
                fprintf(stderr,
                        "VR visceral: R1 sent (thrust %.2f m/s >= %.2f, align %.2f >= %.2f, hand %.2f m from the target <= reach %.2f m)\n",
                        snap.fired_t, vis.cfg.speed_mps, snap.fired_a, vis.cfg.align, snap.fired_dist, vis.cfg.reach_m);
        }
        if (report) {
            char b[64];
            fprintf(stderr,
                    "VR visceral: thrust rejected: %s (best frame: thrust %.2f m/s vs %.2f, align %.2f vs %.2f, %.2f m vs reach %.2f; peak thrust %.2f m/s)\n",
                    vv_reject_names(snap.report_bits, b, sizeof(b)), snap.report_t, vis.cfg.speed_mps, snap.report_a, vis.cfg.align,
                    snap.report_d, vis.cfg.reach_m, snap.report_peak);
        }
        vis_log_close(closed);
    }
    return (fire || vv_pressed(&snap, now)) ? VIS_BTN_R1 : 0;
}

/* The auto-visceral R1 is a send of the host, not an input of the right hand: it is dropped only while the RIGHT hand
 * holds a bridge item (a thrown vial must not riposte), plus the 300 ms tail (BB_VR_HELD_BLOCK_FIRE=0: never dropped). */
uint32_t vr_visceral_pad(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("BB_VR_HELD_BLOCK_FIRE");
        enabled = !(e && *e && atoi(e) == 0);
    }
    const uint32_t r = vis_pad_raw();
    if (!r || !enabled) return r;
    if (vhf_visceral_ok(enabled, vr_fp_held_fire_hand((unsigned)(VHF_TAIL_S * 1000.0)))) return r;
    static double last_log;
    const double now = vis_now_ms();
    if (now - last_log > 2000.0) {
        last_log = now;
        fprintf(stderr, "VR visceral: R1 not sent: the right hand holds an item (BB_VR_HELD_BLOCK_FIRE)\n");
    }
    return 0;
}
