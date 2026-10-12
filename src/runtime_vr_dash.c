/* The comfort dash in the game (BB_VR_DASH=1, docs/vr/findings/19-dash-locomotion.md). In first
 * person a dodge, a backstep or a jump moves the camera with the hunter's animation: a roll takes
 * the eye down to the floor and turns it over, a backstep hops, the running jump arcs and lands in
 * a roll. With the dash the camera slides instead: straight from where it was to where the move
 * ends, along an ease-out curve, level (a jump: one gentle bump), then joins the eye again with
 * position and velocity continuous (gpu/xr_bridge/vr_dash.h, the code the VR sandbox runs too).
 * The head pose is composed afterwards, on top: the head stays the user's own.
 *
 * How a move is recognised (src/vr_dash_detect.h, tests/test_vr_dash.c):
 *  - anim: the newest entry of the player's TimeAct ring, [[P + 0x3B0] + 0x18] +0x20/+0xC0
 *    (findings/13 3.4), starts one of the dodge-family clips. Its table row gives the root motion's
 *    distance and direction in the character's frame; the heading the character faces turns that
 *    into the world (re-aimed while it still changes in the move's first 0.25 s, the character
 *    turning into the roll). The move lasts until another clip starts, or until the feet (the
 *    physics position [[P + 0x3B0] + 0x68] + 0x1E0) stop well short of the clip's distance: a
 *    roll into a wall settles back at once instead of waiting out the clip ahead of the eye.
 *  - input (fallback when the ring cannot be read; VR controllers only): the bridge's pad shows a
 *    circle tap (stick pushed: roll, still: backstep) or L3 while sprinting (jump), and the anchor
 *    then moves faster than 2 m/s within 0.3 s. The distance comes from the kind, the direction
 *    from the motion; the move ends when the anchor stands still for 0.2 s.
 *
 * The first-person camera calls vr_dash_apply once per frame (H1) with its eye position (the
 * integration, findings/19 section 6). Its pad filter hands every pad sample to vr_dash_pad_sample:
 * the dash never reads the bridge's pad itself (that read consumes the gesture events).
 *
 * Environment (all off unless BB_VR_DASH=1):
 *   BB_VR_DASH=1              the comfort dash in first person
 *   BB_VR_DASH_LOG=1          a line when a move starts and ends: the clip, the table's distance
 *                             and direction, what the anchor really did (checks the table in game)
 *   BB_VR_DASH_SET="k=v ..."  settings: follow=0|1 follow_omega=<rad/s> duration=<s> ease=linear|quad|cubic|quart|quint|expo|smooth
 *                             power=<p> settle=<s> settle_speed=<m/s> lead=<m> drop=<m> jump=<m>
 *                             jump_s=<s> tap=<s> vignette=<0..1> detect=auto|anim|input dash=0|1
 *                             log=0|1 (defaults: follow on at 14 rad/s, settle 0.2 s at 4 m/s,
 *                             lead 0 = off, drop 2 m, jump 0.3 m over 0.9 s; the 0.4 s quad
 *                             slide only with follow=0)
 *   BB_VR_DASH_CONTROL=<file> the same tokens, re-read when the file changes (tuning in the
 *                             headset while the game runs; dash=0 switches it off live)
 */
#define _GNU_SOURCE
#include "vr_mem_cache.h"
#include "vr_dash_game.h"
#include "vr_dash_detect.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static struct {
    int ready, enabled;
    VrDashSettings set;
    const char *control;
    double control_checked;
    long long control_mtime, control_size;
    VrDash dash;
    double last_call;
    /* the move in progress */
    int ids, move_id;              /* the id the filter sees (0: none) */
    const VrDashMove *move;        /* anim-detected: its table row (NULL: input-detected) */
    int move_kind;
    int32_t move_anim;
    double move_t0;
    float facing_aimed, lead;
    VrDashV3 move_from, move_dir;
    int retargets, frames;
    /* the character's root (feet): a move that stops short ends the dash at once */
    VrDashV3 root, root0, last_root;
    int have_root, had_root;
    VrDashStall stall;
    /* anim detection */
    int ring_seen;
    int32_t ring_head, ring_anim;
    float ring_f[3];
    int ring_varied; /* a ring float changed since the move began: the clip time is readable */
    /* input fallback */
    VrDashInput input;
    int armed_kind;
    double armed_until, still_since;
    VrDashV3 last_anchor;
    int have_anchor;
    /* follow mode: the camera trails the character's real position (feet + the eye's offset) */
    VrDashV3 eye_off, synth_last, corr;
    int have_off, synth_on;
} vd;
static _Atomic uint32_t vd_vignette_bits;
static _Atomic uint64_t vd_pad_event; /* (time in microseconds << 8) | VR_DASH_MOVE_*: the newest input move */

static double vd_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void vd_store_vignette(float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    atomic_store_explicit(&vd_vignette_bits, bits, memory_order_relaxed);
}

float vr_dash_vignette(void) {
    const uint32_t bits = atomic_load_explicit(&vd_vignette_bits, memory_order_relaxed);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

/* The pad sampler's thread, every pad read (after all sources merged, before the filters change it):
 * the input fallback's tap and sprint-jump detection runs here, at the game's own poll rate, so no
 * press falls between two frames. vd.input belongs to this thread. */
void vr_dash_pad_sample(uint32_t buttons, uint8_t lx, uint8_t ly) {
    if (!vd.ready || !vd.enabled || !vd.set.on || vd.set.detect == VR_DASH_DETECT_ANIM) return;
    const double now = vd_now();
    const int moving = abs((int)lx - 128) > 40 || abs((int)ly - 128) > 40;
    const int kind = vr_dash_input_update(&vd.input, now, buttons, moving, vd.set.tap);
    if (kind) atomic_store_explicit(&vd_pad_event, ((uint64_t)(now * 1e6) << 8) | (uint64_t)kind, memory_order_release);
}

static void vd_log_settings(const char *why) {
    const VrDashParams *p = &vd.set.params;
    fprintf(stderr, "VR dash: %s: %s, slide %.2f s (ease %s %.1f), settle %.2f s (%.1f m/s), max lead %.2f m, "
                    "jump %.2f m over %.2f s, vignette %.2f, detect %s%s%s\n",
            why, vd.set.on ? "on" : "off (dash=0)", p->duration,
            p->curve == VR_DASH_EASE_EXPO ? "expo" : p->curve == VR_DASH_EASE_SMOOTH ? "smooth" : "power", p->power,
            p->settle, p->settle_speed, p->max_lead, p->jump_height, vd.set.jump_duration, vd.set.vignette,
            vd.set.detect == VR_DASH_DETECT_ANIM ? "anim" : vd.set.detect == VR_DASH_DETECT_INPUT ? "input" : "auto",
            vd.set.log ? ", log" : "", p->follow ? ", follow the character" : ", predicted slide");
}

static void vd_init(void) {
    if (vd.ready) return;
    vd.ready = 1;
    const char *e = getenv("BB_VR_DASH");
    vd.enabled = e && e[0] == '1';
    vr_dash_settings_default(&vd.set);
    e = getenv("BB_VR_DASH_LOG");
    if (e && e[0] == '1') vd.set.log = 1;
    e = getenv("BB_VR_DASH_SET");
    if (e && *e && !vr_dash_settings_parse(e, &vd.set))
        fprintf(stderr, "VR dash: BB_VR_DASH_SET=%s: no setting understood\n", e);
    e = getenv("BB_VR_DASH_CONTROL");
    vd.control = e && *e ? e : NULL;
    vd.control_mtime = vd.control_size = -1;
    vr_dash_reset(&vd.dash);
    vr_dash_input_reset(&vd.input);
    vd.still_since = -1.0;
    if (vd.enabled) vd_log_settings("first person moves as a comfort dash");
}

int vr_dash_enabled(void) {
    vd_init();
    return vd.enabled;
}

/* BB_VR_DASH_CONTROL: re-read (at most every 100 ms) when its time stamp or size changes. */
static void vd_poll_control(double now) {
    if (!vd.control || now - vd.control_checked < 0.1) return;
    vd.control_checked = now;
    struct stat st;
    if (stat(vd.control, &st) != 0) return;
    if ((long long)st.st_mtime == vd.control_mtime && (long long)st.st_size == vd.control_size) return;
    vd.control_mtime = (long long)st.st_mtime;
    vd.control_size = (long long)st.st_size;
    FILE *f = fopen(vd.control, "rb");
    if (!f) return;
    char text[2048];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    vr_dash_settings_parse(text, &vd.set); /* tokens left out keep their values */
    vd_log_settings("control file");
}

/* ---- guest memory: read only what is mapped (the player chain dangles during loads) ---- */

static int vd_readable(uintptr_t a, size_t n) {
    if (a < 0x10000 || a + n < a) return 0;
#ifdef _WIN32
    /* page-probed reads with a per-thread cache (vr_mem_cache.h); this guard had no cache at all: a VirtualQuery per read */
    static _Thread_local VmCache cache;
    return vm_readable(&cache, a, n);
#else
    static int fds[2] = {-1, -1};
    if (fds[0] < 0 && pipe(fds) != 0) return 0;
    const ssize_t w = write(fds[1], (const void *)a, n > 4096 ? 4096 : n);
    if (w < 0) return 0;
    char sink[4096];
    return read(fds[0], sink, (size_t)w) >= 0;
#endif
}

static int vd_ptr(uintptr_t a, uintptr_t *out) {
    uint64_t v;
    if (!vd_readable(a, 8)) return 0;
    memcpy(&v, (const void *)a, 8);
    if (v < 0x10000 || (v & 7)) return 0;
    *out = (uintptr_t)v;
    return 1;
}

/* The player's newest TimeAct clip: [[P + 0x3B0] + 0x18] is the TimeAct module (findings/13). */
static int vd_ring(uint64_t player, int32_t *anim, int32_t *head, float f[3]) {
    uintptr_t bag, tam;
    if (!player || !vd_ptr((uintptr_t)player + 0x3B0, &bag) || !vd_ptr(bag + 0x18, &tam) || !vd_readable(tam, 0xC8))
        return 0;
    unsigned char module[0xC8];
    memcpy(module, (const void *)tam, sizeof(module));
    return vr_dash_ring_newest(module, anim, head, f);
}

/* The player's root (the feet): [[P + 0x3B0] + 0x68] + 0x1E0, the physics module's position
 * (findings/16, read the same way by src/runtime_vr_fp.c). */
static int vd_read_root(uint64_t player, VrDashV3 *out) {
    uintptr_t bag, phys;
    if (!player || !vd_ptr((uintptr_t)player + 0x3B0, &bag) || !vd_ptr(bag + 0x68, &phys) || !vd_readable(phys + 0x1E0, 16))
        return 0;
    float p[4];
    memcpy(p, (const void *)(phys + 0x1E0), sizeof(p));
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || fabsf(p[0]) > 1e5f || fabsf(p[1]) > 1e5f ||
        fabsf(p[2]) > 1e5f)
        return 0;
    *out = vr_dash_v3(p[0], p[1], p[2]);
    return 1;
}

/* ---- moves ---- */

static float vd_wrap(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

static void vd_begin(int kind, const VrDashMove *move, int32_t anim, float facing, VrDashV3 at, double now,
                     VrDashHint *hint) {
    vd.move_id = ++vd.ids ? vd.ids : ++vd.ids;
    vd.move = move;
    vd.move_kind = kind;
    vd.move_anim = anim;
    vd.move_t0 = now;
    vd.move_from = at;
    vd.facing_aimed = facing;
    vd.lead = 0.0f;
    vd.retargets = vd.frames = 0;
    vd.still_since = -1.0;
    vd.root0 = vd.root;
    vr_dash_stall_reset(&vd.stall);
    memset(hint, 0, sizeof(*hint));
    hint->kind = kind == VR_DASH_MOVE_JUMP ? VR_DASH_JUMP : VR_DASH_DODGE;
    hint->height = -1.0f;
    hint->duration = kind == VR_DASH_MOVE_JUMP ? vd.set.jump_duration : 0.0f;
    vd.move_dir = vr_dash_v3(0.0f, 0.0f, 0.0f);
    if (move) {
        hint->distance = move->distance;
        if (isfinite(facing)) hint->direction = vd.move_dir = vr_dash_move_direction(facing, move->direction);
    } else {
        hint->distance = kind == VR_DASH_MOVE_JUMP ? 6.84f : kind == VR_DASH_MOVE_BACKSTEP ? 2.48f : 4.0f;
    }
    if (vd.set.log) {
        if (move)
            fprintf(stderr, "VR dash: move %d: a%03d_%06d %s, table %.2f m at %+.0f deg, facing %.1f deg -> slide %.2f s\n",
                    vd.move_id, anim / 1000000, anim % 1000000, vr_dash_move_name(kind), move->distance,
                    move->direction, facing / 0.017453293f,
                    kind == VR_DASH_MOVE_JUMP ? vd.set.jump_duration : vd.set.params.duration);
        else
            fprintf(stderr, "VR dash: move %d: %s from the controller input, %.2f m, direction from the motion\n",
                    vd.move_id, vr_dash_move_name(kind), hint->distance);
    }
}

static void vd_end(VrDashV3 at, double now, const char *why) {
    if (vd.set.log && vd.move_id) {
        VrDashV3 d = vr_dash_sub(at, vd.move_from);
        d.y = 0.0f;
        const float moved = vr_dash_len_h(d);
        float off = 0.0f;
        if (vr_dash_len_h(vd.move_dir) > 0.5f && moved > 0.05f)
            off = atan2f(d.x * vd.move_dir.z - d.z * vd.move_dir.x, d.x * vd.move_dir.x + d.z * vd.move_dir.z) / 0.017453293f;
        fprintf(stderr, "VR dash: move %d ended after %.2f s (%s, %d frames): the eye moved %.2f m%s%+.1f deg off "
                        "the table's direction%s; the camera led it by up to %.2f m; %d re-aims\n",
                vd.move_id, now - vd.move_t0, why, vd.frames, moved, vd.move ? " (table " : "",
                vd.move ? off : 0.0f, vd.move ? ")" : "", vd.lead, vd.retargets);
        if (vd.move) fprintf(stderr, "VR dash:   table distance %.2f m\n", vd.move->distance);
    }
    vd.move_id = 0;
    vd.move = NULL;
    vd.armed_kind = 0;
}

static void vd_forget(void) {
    vr_dash_reset(&vd.dash);
    vd.move_id = 0;
    vd.move = NULL;
    vd.ring_seen = 0; /* the next clip read is only a baseline: no dash from the middle of a move */
    vd.armed_kind = 0;
    vd.have_anchor = vd.have_root = vd.had_root = 0;
    vd.have_off = vd.synth_on = 0;
    vd.corr = vr_dash_v3(0.0f, 0.0f, 0.0f);
    atomic_store_explicit(&vd_pad_event, 0, memory_order_relaxed);
    vr_dash_stall_reset(&vd.stall);
    vd_store_vignette(0.0f);
}

void vr_dash_apply(uint64_t player, float facing, float anchor[3]) {
    vd_init();
    if (!vd.enabled) return;
    const double now = vd_now();
    vd_poll_control(now);
    if (vd.last_call > 0.0 && now - vd.last_call > 0.25) vd_forget(); /* first person was off */
    const double dt = vd.last_call > 0.0 ? now - vd.last_call : 0.0;
    vd.last_call = now;
    const VrDashV3 a = vr_dash_v3(anchor[0], anchor[1], anchor[2]);
    if (!isfinite(a.x) || !isfinite(a.y) || !isfinite(a.z) || !vd.set.on) {
        vd_forget();
        return;
    }
    float speed = 0.0f;
    if (vd.have_anchor && dt > 1e-4) speed = vr_dash_len_h(vr_dash_sub(a, vd.last_anchor)) / (float)dt;
    vd.last_anchor = a;
    vd.have_anchor = 1;
    float root_speed = -1.0f; /* unknown */
    vd.had_root = vd.have_root;
    vd.last_root = vd.root;
    vd.have_root = vd_read_root(player, &vd.root);
    if (vd.have_root && vd.had_root && dt > 1e-4)
        root_speed = vr_dash_len_h(vr_dash_sub(vd.root, vd.last_root)) / (float)dt;

    VrDashHint hint;
    int started = 0;
    int32_t anim = 0, head = 0;
    float rf[3] = {0, 0, 0};
    const int ring = vd.set.detect != VR_DASH_DETECT_INPUT && vd_ring(player, &anim, &head, rf);
    if (ring) {
        if (!vd.ring_seen) {
            vd.ring_seen = 1; /* a baseline */
        } else if ((head != vd.ring_head || anim != vd.ring_anim) &&
                   vr_dash_ring_new_move(vd.move_id != 0, vd.ring_anim, anim, vd.ring_f, rf, vd.ring_varied,
                                     now - vd.move_t0, vd.move)) {
            const VrDashMove *m = vr_dash_move_lookup(anim);
            if (m) {
                if (vd.move_id) vd_end(a, now, "the next move started");
                vd_begin(m->kind, m, anim, facing, a, now, &hint);
                vd.ring_varied = 0;
                started = 1;
            } else if (vd.move_id) {
                vd_end(a, now, "the next clip started");
            }
        }
        if (vd.move_id && anim == vd.ring_anim && (rf[0] != vd.ring_f[0] || rf[1] != vd.ring_f[1] || rf[2] != vd.ring_f[2]))
            vd.ring_varied = 1;
        vd.ring_head = head;
        vd.ring_anim = anim;
        memcpy(vd.ring_f, rf, sizeof(rf));
    } else if (vd.set.detect != VR_DASH_DETECT_ANIM) {
        const uint64_t ev = atomic_exchange_explicit(&vd_pad_event, 0, memory_order_acquire);
        if (ev) {
            const double t = (double)(ev >> 8) * 1e-6;
            if (now - t < 0.3) { /* the sampler's moment, not this frame's */
                vd.armed_kind = (int)(ev & 0xFF);
                vd.armed_until = t + 0.3;
            }
        }
        if (vd.armed_kind && now > vd.armed_until) vd.armed_kind = 0;
        if (vd.armed_kind && speed > 2.0f) {
            const int kind = vd.armed_kind;
            if (vd.move_id) vd_end(a, now, "the next move started");
            vd_begin(kind, NULL, 0, facing, a, now, &hint);
            started = 1;
        } else if (vd.move_id && !vd.move) {
            /* An input-started move ends when the anchor stands still (or after the clip). */
            vd.still_since = speed < 0.3f ? (vd.still_since < 0.0 ? now : vd.still_since) : -1.0;
            const double longest = vd.move_kind == VR_DASH_MOVE_JUMP ? 2.1 : 1.7;
            if ((vd.still_since >= 0.0 && now - vd.still_since > 0.2) || now - vd.move_t0 > longest)
                vd_end(a, now, "the anchor stopped");
        }
    }

    /* A move whose feet stopped short (a wall, an enemy): the dash ends now, the camera settles
     * onto the eye instead of waiting out the clip ahead of it. */
    if (vd.move_id && vd.move && !started && vd.have_root && root_speed >= 0.0f) {
        const VrDashV3 d = vr_dash_sub(vd.root, vd.root0);
        const float along = vr_dash_len_h(vd.move_dir) > 0.5f ? d.x * vd.move_dir.x + d.z * vd.move_dir.z
                                                              : vr_dash_len_h(d);
        if (vr_dash_stalled(&vd.stall, vd.move, now - vd.move_t0, along, root_speed))
            vd_end(a, now, "blocked: the feet stopped short");
    }
    /* Follow mode: the dash filters the character's REAL position (the feet from physics plus the
     * eye's offset from them, taken while no dash runs), so the eye never goes where the character
     * cannot (walls) and rides hills and stairs. When it hands back to the eye, a decaying
     * correction hides the small difference. */
    VrDashV3 in = a;
    const int use_synth = vd.set.params.follow && vd.have_root && vd.have_off &&
                          (vd.move_id || started || vd.dash.mode != VR_DASH_IDLE);
    if (vd.set.params.follow && vd.have_root && !use_synth && !vd.move_id) {
        vd.eye_off = vr_dash_sub(a, vd.root);
        vd.have_off = 1;
    }
    if (use_synth) in = vr_dash_add(vd.root, vd.eye_off);
    VrDashV3 out = vr_dash_update(&vd.dash, &vd.set.params, in, vd.move_id, started ? &hint : NULL, now);
    if (vd.set.params.follow) {
        if (vd.synth_on && !use_synth) vd.corr = vr_dash_sub(vd.synth_last, a);
        vd.synth_on = use_synth;
        vd.synth_last = in;
        if (!use_synth && vr_dash_len(vd.corr) > 1e-3f) {
            vd.corr = vr_dash_scale(vd.corr, expf(-(float)dt / 0.15f));
            out = vr_dash_add(out, vd.corr);
        } else if (use_synth) {
            vd.corr = vr_dash_v3(0.0f, 0.0f, 0.0f);
        }
    }
    /* The character may still turn into the move for a frame or two: follow its facing. */
    if (vd.move_id && vd.move && isfinite(facing) && now - vd.move_t0 < 0.25 &&
        fabsf(vd_wrap(facing - vd.facing_aimed)) > 5.0f * 0.017453293f) {
        vd.move_dir = vr_dash_move_direction(facing, vd.move->direction);
        vr_dash_retarget(&vd.dash, vd.move_dir, vd.move->distance, now);
        vd.facing_aimed = facing;
        ++vd.retargets;
    }
    if (vd.move_id) {
        ++vd.frames;
        const VrDashV3 lead = vr_dash_sub(out, in);
        const float along = vr_dash_len_h(vd.move_dir) > 0.5f ? lead.x * vd.move_dir.x + lead.z * vd.move_dir.z
                                                              : vr_dash_len_h(lead);
        if (along > vd.lead) vd.lead = along;
    }
    vd_store_vignette(vd.set.vignette * vd.dash.vignette);
    anchor[0] = out.x;
    anchor[1] = out.y;
    anchor[2] = out.z;
}
