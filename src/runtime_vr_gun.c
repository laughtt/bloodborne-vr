/* VR gun aim (BB_VR_GUN_AIM=1, docs/vr/findings/27-gun-aim.md): the player's bullets leave along the
 * controller's pointing ray instead of the character's facing (or the lock-on target).
 *
 * Where the game decides the direction (findings/27 2, static analysis of out/eboot.elf):
 *   TAE bullet/gun event -> behavior 0x1cb61c0 -> 0x1cb6680 builds the bullet request (owner handle,
 *   BulletParam id, the dummy poly's and the character's 3x4 matrices) -> 0x1cb7250 ->
 *   SprjBulletManager [0x593E8D0] 0x1e03620 -> 0x1dedc20 builds the emitter descriptor: the target is
 *   the lock-on target, or else a point BulletParam.dist meters ahead of the character's facing
 *   (kind 4) -> the emitter's update 0x1deffa0 picks the launch matrix per bullet (dummy poly,
 *   character, spread) and calls the launch init 0x1df6ca0(rdi = SprjBulletIns, rsi = descriptor,
 *   rdx = target, rcx = launch matrix, r8d = index) at 0x1df0607. The init puts the bullet at the
 *   matrix's position and gives it velocity = BulletParam speed x the matrix's Z axis (0x1df76df).
 * The hook ("VR Gun Aim (bullet spawn)", 0x1df6ca0, 20 bytes, replay-after) runs at the init's entry
 * and, for a launch from the emitter (return address 0x1df060c) owned by the player (descriptor
 * +0x0C = [player + 8]) with a BulletParam id in the list, turns the launch matrix and the aim point
 * so the game's direction becomes the controller's ray (src/vr_gun_math.h vg_redirect). The muzzle
 * (the matrix position) is kept. With a lock-on target the game's aim is kept unless
 * BB_VR_GUN_AIM_LOCK=aim. Both are caller stack copies: only this bullet changes.
 *
 * The controller's ray: bbgpu_vr_hands (the aim pose, its -Z; or the grip pitched by
 * BB_VR_GUN_AIM_GRIP_PITCH when the runtime gives no aim pose), relative to the head of the same
 * snapshot, through the first-person camera H1 just showed (vr_gun_view). Needs first person.
 *
 * Environment (needs BB_HOOKS="...;VR Gun Aim (bullet spawn)"):
 *   BB_VR_GUN_AIM=1              on (default off: the handler returns at once)
 *   BB_VR_GUN_AIM_HAND=left|right   the hand that aims (left: the gun hand, L2 fires)
 *   BB_VR_GUN_AIM_LOCK=keep|aim  with a lock-on target: keep the game's aim (default) or use the ray
 *   BB_VR_GUN_AIM_IDS=500-999    BulletParam ids that follow the ray ("all"; the default is the
 *                                firearms, their child bullets and the other left-hand bullets;
 *                                thrown items are 100-370)
 *   BB_VR_GUN_AIM_REF=auto|target|forward  the game's direction the turn starts from (findings/27 3)
 *   BB_VR_GUN_AIM_POSE=aim|grip  the controller pose used as the ray (aim; grip when aim is invalid)
 *   BB_VR_GUN_AIM_GRIP_PITCH=<deg>  the grip's ray pitch when the grip is used (0)
 *   BB_VR_PARRY_ASSIST=0.8       parry aim assist strength 0..1 (0 off; findings/41): the ray is pulled toward the
 *                                lock-on candidate it nearly points at (cone atan(RADIUS/d) clamped to MIN..MAX deg)
 *   BB_VR_PARRY_RADIUS_M=0.6  BB_VR_PARRY_CONE_MIN=4  BB_VR_PARRY_CONE_MAX=18  BB_VR_PARRY_RANGE_M=12
 *   BB_VR_GUN_AIM_LOG=1          also log the player's shots that are left alone and other owners'
 * Each redirected shot is logged (old vs new yaw/pitch), at most 20 lines a second.
 *
 * Throw mode (physical items, findings/39; the headset menus' held items, vr_held_items.h): when the
 * hand lets go of a throwable (BBXR_UI_THROW_ORIGIN / _DIR actions, then the game's own use of the item
 * runs), the first projectile of the player that reaches this hook with a BulletParam id in the
 * thrown list leaves from the hand's position along the release direction instead of the character's
 * muzzle and facing (src/vr_gun_math.h vg_throw_launch). The speed stays the game's. Only flying
 * goods are listed (not Antidote 200, Sedative 230, Lead Elixir 320; the rope molotovs' speed-0 spawners
 * 110/114 are left alone and their flying child 111/115 is aimed). Works without BB_VR_GUN_AIM.
 * From the release until the bullet launches the body faces the aim's heading (vr_fp_throw_face): the game
 * throws along the body, and a body the stick had turned away from the view threw backwards (live log: game
 * launch yaw -127.6 = the body's facing -129.0 while the view looked along 50.5). Each redirected bullet's
 * velocity (+0x40) is read again one frame and 200 ms later and its heading logged against the aim.
 *   BB_VR_THROW_AIM=0        off (default on; it only acts after a throw gesture armed it)
 *   BB_VR_THROW_IDS=...      the redirected BulletParam ids (default BBXR_THROW_BULLET_IDS)
 *   BB_VR_THROW_FOLLOW_IDS=...  later bullets of one throw, turned with the first (default BBXR_THROW_FOLLOW_IDS)
 *   BB_VR_THROW_SHIFT_M=2    keep the game's muzzle when the hand is farther than this from it
 *   BB_VR_THROW_SPEED=1      speed from the hand (findings/39 1A ii, default on; [L] unverified live): the
 *                            headset sends the release speed with the use action (vr_gun_throw_speed);
 *                            after a redirect the BulletIns velocity (vec4 at +0x40, copied to +0x50
 *                            by the launch init at 0x1df77a7) is scaled by clamp(speed / REF, MIN, MAX)
 *                            at the next camera frame or launch hook (whichever comes first, within
 *                            300 ms) when it reads as a plausible velocity along the new direction.
 *                            One frame later the speed is read again and logged, which tells whether
 *                            the bullet mover integrates +0x40 at all.
 *   BB_VR_THROW_SPEED_REF=7  hand speed (m/s) that keeps the game's speed
 *   BB_VR_THROW_SPEED_MIN=0.3  BB_VR_THROW_SPEED_MAX=1.5   the factor's clamp
 *   BB_VR_THROW_LOG=0        silence the per-projectile lines (every id the player's projectiles
 *                            carry through this hook is logged, once at first sight and then rate
 *                            limited: run once with BB_VR_GUN_AIM_IDS=all to see them all)
 *
 * VR visceral (src/runtime_vr_visceral.c, findings/41 7): every player shot with a BulletParam id in
 * BB_VR_GUN_AIM_IDS tells the visceral module (also with BB_VR_GUN_AIM off, then nothing is
 * redirected) its target: the lock-on target's point, or else the lock-on candidate the controller
 * ray points at (vg_assist_dir's cone). */
#define _GNU_SOURCE
#include "hooks.h"
#include "vr_gun.h"
#include "vr_fp.h"
#include "vr_gun_math.h"
#include "vr_visceral.h"
#include "gpu/bbgpu.h"
#include "gpu/xr_bridge/bbport_xr_bridge.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define VA_BASE 0x400000u
#define VA_SITE_LAUNCH 0x1DF6CA0u      /* SprjBulletIns launch init (rdi bullet, rsi desc, rdx target, rcx matrix) */
#define VA_RET_EMITTER 0x1DF060Cu      /* after the emitter's call at 0x1df0607 (0x1deffa0) */
#define VA_WORLD_CHR_MAN 0x593E878u
#define VA_PLAYER_INS_VTABLE 0x578F310u
#define VA_LOCK_TGT_MAN 0x593E860u     /* LockTgtMan: +8 the candidate list (next +0x80, point +0x00, flags +0x90) */
#define GUN_ASSIST_MAX 32
#define GUN_VIEW_STALE_US 300000u
#define GUN_HANDS_STALE_US 250000u
#define GUN_LOG_PER_S 20

static struct {
    int on, log, side, lock_aim, ref, use_grip;
    float grip_pitch, max_off;
    char ids[128];
    VgAssist assist;       /* parry aim assist (strength 0: off) */
    unsigned assist_n, assist_hits;
    unsigned char *image;
    /* hands freshness (tracked per frame in vr_gun_view) */
    int64_t hands_ns;
    uint64_t hands_changed_us;
    /* log budget */
    uint64_t log_second;
    int log_lines;
    uint32_t shots, redirected;
} gun;

typedef struct {
    uint64_t time_us;
    float rows[4][4]; /* right, up, forward, position */
} GunView;
static GunView gun_views[2];

/* The throw mode (written by the pad thread, taken by the game thread's hook). */
static struct {
    int on, log;
    char ids[128];
    char follow[64]; /* ids of later bullets of one throw (BB_VR_THROW_FOLLOW_IDS) */
    float max_shift;
    uint32_t seen[32]; /* ids < 1024 already announced */
    uint32_t projectiles, thrown;
} thr;
/* The throw whose first bullet was just redirected (game thread only): its later bullets (the rope
 * molotov's chain, the gloves' fan) get the same turn, so the spread between them is kept. */
#define THR_GROUP_S 1.0
static struct {
    int active;
    int32_t first_id;
    uint32_t count;
    uint64_t at_us;
    VgThrowGroup g;
} thr_grp;
static struct {
    int armed;
    uint64_t at_us;
    VrVec3 rel_origin; /* m, head frame (from the pending ORIGIN call) */
    int have_origin;
    VrVec3 origin, dir; /* world */
    float speed_mps;    /* the hand's release speed (vr_gun_throw_speed); 0 = unknown */
} thr_arm;
/* The speed rewrite (BB_VR_THROW_SPEED): stashed by thr_redirect, applied by thr_speed_poll once the
 * game's launch init has written the BulletIns velocity. */
static struct {
    int on;
    float ref, lo, hi;
    int pending, verify;
    uintptr_t bullet;
    int32_t id;
    VrVec3 dir;
    float factor, hand_mps, expect;
    uint64_t at_us, applied_us;
    char why[48];
} thr_spd;
/* The heading check of a redirected bullet (thr_speed_poll): its velocity one frame and 200 ms after the launch. */
static struct {
    int stage; /* 0 none, 1 waiting for one frame, 2 waiting for 200 ms */
    uintptr_t bullet;
    int32_t id;
    VrVec3 dir;
    uint64_t at_us;
} thr_chk;
static _Atomic int thr_spd_busy;
#define THR_SPD_WINDOW_US 300000u
static atomic_flag thr_lock = ATOMIC_FLAG_INIT;
static void thr_acquire(void) { while (atomic_flag_test_and_set_explicit(&thr_lock, memory_order_acquire)) {} }
static void thr_release(void) { atomic_flag_clear_explicit(&thr_lock, memory_order_release); }
#define THR_ARM_S 4.0
#define THR_FACE_US 2500000u      /* the body faces the aim at most this long after the release */
#define THR_FACE_AFTER_US 150000u /* ... and this long after the bullet launched (a rope molotov's later bullets) */
static _Atomic uint32_t gun_view_seq;

static uint64_t gun_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void thr_speed_poll(uint64_t now);
void vr_gun_view(const float rows[4][4]) {
    if (!gun.on && !thr.on && !vr_visceral_active()) return;
    const uint64_t now = gun_now_us();
    const uint32_t seq = atomic_load_explicit(&gun_view_seq, memory_order_relaxed);
    GunView *v = &gun_views[(seq + 1) & 1];
    v->time_us = now;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) v->rows[i][j] = rows[i][j];
    atomic_store_explicit(&gun_view_seq, seq + 1, memory_order_release);
    /* the controller snapshot is alive while its time moves (the pad poll runs every frame) */
    BbVrHands h;
    if (bbgpu_vr_hands(&h) && h.time_ns != gun.hands_ns) {
        gun.hands_ns = h.time_ns;
        gun.hands_changed_us = now;
    }
    thr_speed_poll(now);
}
static int gun_view_read(GunView *out) {
    for (int tries = 0; tries < 3; ++tries) {
        const uint32_t a = atomic_load_explicit(&gun_view_seq, memory_order_acquire);
        if (!a) return 0;
        *out = gun_views[a & 1];
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&gun_view_seq, memory_order_relaxed) == a) return 1;
    }
    return 0;
}

/* The visceral module (pad thread): the latest first-person camera; 0 when there is none or it is stale. */
int vr_gun_view_latest(float rows[4][4], uint64_t *age_us) {
    GunView v;
    const uint64_t now = gun_now_us();
    if (!gun_view_read(&v) || now - v.time_us > GUN_VIEW_STALE_US) return 0;
    memcpy(rows, v.rows, sizeof(v.rows));
    if (age_us) *age_us = now - v.time_us;
    return 1;
}

/* The pad thread: the hand's position at the release (head frame, mm). */
void vr_gun_throw_origin(const int32_t mm[3]) {
    if (!thr.on) return;
    thr_acquire();
    thr_arm.rel_origin = vr_v3((float)mm[0] * 0.001f, (float)mm[1] * 0.001f, (float)mm[2] * 0.001f);
    thr_arm.have_origin = 1;
    thr_release();
}
/* ... and its direction: arms the hook with both in the world (the camera of this moment). */
void vr_gun_throw_dir(const int32_t dir_1e4[3]) {
    if (!thr.on) return;
    GunView view;
    const uint64_t now = gun_now_us();
    thr_acquire();
    const int have = thr_arm.have_origin;
    const VrVec3 rel_o = thr_arm.rel_origin;
    thr_arm.have_origin = 0;
    thr_release();
    if (!have || !gun_view_read(&view) || now - view.time_us > 1000000u) {
        fprintf(stderr, "VR throw: not armed (%s): the game's own direction\n",
                !have ? "no hand position" : "no first-person camera");
        return;
    }
    const VrVec3 rel_d = vr_v3((float)dir_1e4[0] * 1e-4f, (float)dir_1e4[1] * 1e-4f, (float)dir_1e4[2] * 1e-4f);
    const VrVec3 dir = vam_norm(vg_rel_dir_to_world(rel_d, (const float (*)[4])view.rows), vr_v3(0, 0, 0));
    if (vam_len(dir) < 0.5f) return;
    const VrVec3 cam = vr_v3(view.rows[3][0], view.rows[3][1], view.rows[3][2]);
    const VrVec3 origin = vg_rel_pos_to_world(rel_o, (const float (*)[4])view.rows, cam);
    thr_acquire();
    thr_arm.armed = 1;
    thr_arm.at_us = now;
    thr_arm.origin = origin;
    thr_arm.dir = dir;
    thr_arm.speed_mps = 0.0f;
    thr_release();
    float yaw, pitch;
    vg_yaw_pitch(dir, &yaw, &pitch);
    vr_fp_throw_face(yaw * VR_DEG, now + THR_FACE_US);
    fprintf(stderr, "VR throw: armed for %.0f s: hand %.2f %.2f %.2f, release yaw %.1f pitch %.1f deg (the body faces yaw %.1f until the launch)\n",
            THR_ARM_S, origin.x, origin.y, origin.z, yaw, pitch, yaw);
}
/* The pad thread, right after the DIR call: the hand's speed at the release (cm/s, relative to the
 * head); the projectile's speed is scaled by it (thr_speed_poll). */
void vr_gun_throw_speed(int32_t cm_s) {
    if (!thr.on || cm_s <= 0) return;
    thr_acquire();
    if (thr_arm.armed) thr_arm.speed_mps = (float)cm_s * 0.01f;
    thr_release();
}
/* The game thread: takes the armed throw (once). */
static int thr_take(uint64_t now, VrVec3 *origin, VrVec3 *dir, float *speed) {
    int ok = 0;
    thr_acquire();
    if (thr_arm.armed) {
        if ((double)(now - thr_arm.at_us) * 1e-6 <= THR_ARM_S) {
            *origin = thr_arm.origin;
            *dir = thr_arm.dir;
            *speed = thr_arm.speed_mps;
            ok = 1;
        }
        thr_arm.armed = 0;
        thr_arm.speed_mps = 0.0f;
    }
    thr_release();
    return ok;
}

/* ---- guest memory: read only what is mapped (the player chain dangles during loads) ---- */
static int gun_mem_ok(uintptr_t a, size_t n, int write) {
    if (a < 0x10000 || a + n < a) return 0;
#ifdef _WIN32
    uintptr_t at = a;
    while (at < a + n) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void *)at, &mbi, sizeof(mbi))) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_EXECUTE_READ | rw))) return 0;
        if (write && !(mbi.Protect & rw)) return 0;
        at = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
#else
    (void)write;
#endif
    return 1;
}
static int gun_rd(uintptr_t a, void *out, size_t n) {
    if (!gun_mem_ok(a, n, 0)) return 0;
    memcpy(out, (const void *)a, n);
    return 1;
}
static uintptr_t gun_va(uint32_t v) { return (uintptr_t)(gun.image + (v - VA_BASE)); }

/* The player's handle ([PlayerIns + 8]); 0 when there is no player. */
static int gun_player_handle(uint32_t *handle) {
    uint64_t wcm = 0, player = 0, vtable = 0;
    if (!gun_rd(gun_va(VA_WORLD_CHR_MAN), &wcm, 8) || !gun_rd((uintptr_t)wcm + 0x60, &player, 8) ||
        !gun_rd((uintptr_t)player, &vtable, 8) || vtable != (uint64_t)gun_va(VA_PLAYER_INS_VTABLE))
        return 0;
    return gun_rd((uintptr_t)player + 8, handle, 4);
}

static int gun_log_ok(uint64_t now) {
    const uint64_t s = now / 1000000u;
    if (s != gun.log_second) { gun.log_second = s; gun.log_lines = 0; }
    return gun.log_lines++ < GUN_LOG_PER_S;
}

/* The controller's ray in the world; 0 (and why) when there is none. */
static int gun_aim(uint64_t now, VrVec3 *aim, const char **why, const char **pose_name) {
    GunView view;
    if (!gun_view_read(&view) || now - view.time_us > GUN_VIEW_STALE_US) { *why = "no first-person camera"; return 0; }
    BbVrHands h;
    if (!bbgpu_vr_hands(&h) || now - gun.hands_changed_us > GUN_HANDS_STALE_US) { *why = "no controller snapshot"; return 0; }
    if (!(h.valid & 4u)) { *why = "head not tracked"; return 0; }
    const int s = gun.side;
    const BbVrPose *p = NULL;
    float pitch = 0.0f;
    if (!gun.use_grip && (h.valid & (8u << s))) { p = &h.aim[s]; *pose_name = "aim"; }
    else if (h.valid & (1u << s)) { p = &h.grip[s]; pitch = gun.grip_pitch; *pose_name = "grip"; }
    if (!p) { *why = s ? "right controller not tracked" : "left controller not tracked"; return 0; }
    const VrQuat head = vr_q(h.head.orientation[0], h.head.orientation[1], h.head.orientation[2], h.head.orientation[3]);
    VrQuat q = vr_q(p->orientation[0], p->orientation[1], p->orientation[2], p->orientation[3]);
    if (!vr_qvalid(head) || !vr_qvalid(q)) { *why = "invalid pose"; return 0; }
    if (pitch != 0.0f) q = vg_pitched(q, pitch);
    *aim = vg_aim_in_world(head, q, (const float (*)[4])view.rows);
    if (vam_len(*aim) < 0.5f) { *why = "degenerate ray"; return 0; }
    return 1;
}

/* The lock-on candidates' points (the enemies the game would let the player lock onto, findings/41);
 * the list is read-only here. Returns how many plausible points were read. */
static int gun_candidates(VrVec3 *out, int max) {
    uint64_t ltm = 0, node = 0;
    int n = 0;
    if (!gun_rd(gun_va(VA_LOCK_TGT_MAN), &ltm, 8) || !gun_rd((uintptr_t)ltm + 8, &node, 8)) return 0;
    for (int guard = 0; node && guard < 64 && n < max; ++guard) {
        float p[4];
        uint64_t next = 0;
        if (!gun_rd((uintptr_t)node, p, 16) || !gun_rd((uintptr_t)node + 0x80, &next, 8)) break;
        if (isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]) && fabsf(p[0]) < 1e5f && fabsf(p[2]) < 1e5f)
            out[n++] = vr_v3(p[0], p[1], p[2]);
        if (next & 7) break;
        node = next;
    }
    return n;
}

/* The pad thread (visceral tracking): the candidates' points as plain floats. */
int vr_gun_candidate_points(float (*out)[3], int max) {
    if (!gun.image) return 0;
    VrVec3 p[GUN_ASSIST_MAX];
    const int n = gun_candidates(p, max < GUN_ASSIST_MAX ? max : GUN_ASSIST_MAX);
    for (int i = 0; i < n; ++i) {
        out[i][0] = p[i].x;
        out[i][1] = p[i].y;
        out[i][2] = p[i].z;
    }
    return n;
}

/* The locked-on node's point (flags & 0x20; the walk of fp_lock_state in src/runtime_vr_fp.c). */
static int gun_locked_point(VrVec3 *out) {
    uint64_t ltm = 0, node = 0;
    if (!gun_rd(gun_va(VA_LOCK_TGT_MAN), &ltm, 8) || !gun_rd((uintptr_t)ltm + 8, &node, 8)) return 0;
    for (int guard = 0; node && guard < 64; ++guard) {
        uint8_t flags = 0;
        uint64_t next = 0;
        if (!gun_rd((uintptr_t)node + 0x90, &flags, 1)) break;
        if (flags & 0x20) {
            float p[4];
            if (!gun_rd((uintptr_t)node, p, 16)) return 0;
            if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || fabsf(p[0]) > 1e5f || fabsf(p[2]) > 1e5f) return 0;
            *out = vr_v3(p[0], p[1], p[2]);
            return 1;
        }
        if (!gun_rd((uintptr_t)node + 0x80, &next, 8) || (next & 7)) break;
        node = next;
    }
    return 0;
}

/* The visceral module's view of a player firearm shot: its target, or else the lock-on candidate the
 * controller ray points at. Reads only; nothing is redirected. */
static void gun_visceral_shot(uint64_t now, int locked, const float m[3][4]) {
    VrVec3 tp = vr_v3(0, 0, 0);
    int how = 0;
    const VrVec3 muzzle = vg_launch_origin((const float (*)[4])m);
    if (locked && gun_locked_point(&tp)) how = 1;
    if (!how) {
        VrVec3 aim;
        const char *why = "", *pose = "";
        if (gun_aim(now, &aim, &why, &pose)) {
            VrVec3 pts[GUN_ASSIST_MAX];
            const int n = gun_candidates(pts, GUN_ASSIST_MAX);
            VgAssist a = gun.assist;
            a.strength = 1.0f; /* the pick only: whether the ray is pulled does not matter here */
            int idx = -1;
            vg_assist_dir(aim, muzzle, pts, n, &a, &idx, NULL);
            if (idx >= 0) {
                tp = pts[idx];
                how = 2;
            }
        }
    }
    const float t3[3] = {tp.x, tp.y, tp.z};
    const float dx = tp.x - muzzle.x, dz = tp.z - muzzle.z;
    vr_visceral_on_shot(how, t3, how ? sqrtf(dx * dx + dz * dz) : 0.0f);
}

/* The speed rewrite. The launch init (0x1df6ca0) writes velocity = BulletParam speed x normalize(launch
 * Z) as a vec4 to BulletIns+0x40 and copies it to +0x50 (static RE, 0x1df77a7/0x1df77b3), after this
 * hook has returned: thr_redirect stashes the job, the next camera frame or launch hook (this poll)
 * scales it. Whether the bullet mover integrates +0x40 is not verified [L]: one frame after the
 * rewrite the velocity is read again and logged, to see it in a headset test. */
static void thr_speed_poll(uint64_t now) {
    if (!atomic_load_explicit(&thr_spd_busy, memory_order_relaxed)) return;
    char msg[240];
    msg[0] = 0;
    thr_acquire();
    if (thr_spd.pending) {
        float v[8];
        const uintptr_t b = thr_spd.bullet;
        int ok = 0;
        float sp = 0.0f, ang = 0.0f;
        if (!gun_rd(b + 0x40, v, sizeof(v))) {
            snprintf(thr_spd.why, sizeof(thr_spd.why), "bullet memory not readable");
        } else if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) {
            snprintf(thr_spd.why, sizeof(thr_spd.why), "velocity not finite");
        } else {
            const VrVec3 vel = vr_v3(v[0], v[1], v[2]);
            sp = vam_len(vel);
            ang = sp > 1e-6f ? vg_angle_deg(vel, thr_spd.dir) : 180.0f;
            if (!(sp >= 0.5f && sp <= 100.0f))
                snprintf(thr_spd.why, sizeof(thr_spd.why), "|v| %.2f outside 0.5..100 m/s", sp);
            else if (ang > 15.0f)
                snprintf(thr_spd.why, sizeof(thr_spd.why), "velocity %.0f deg off the release direction", ang);
            else if (!gun_mem_ok(b + 0x40, 0x20, 1))
                snprintf(thr_spd.why, sizeof(thr_spd.why), "bullet memory not writable");
            else
                ok = 1;
        }
        if (ok) {
            for (int i = 0; i < 3; ++i) {
                v[i] *= thr_spd.factor;
                v[4 + i] *= thr_spd.factor;
            }
            memcpy((void *)(b + 0x40), v, sizeof(v));
            snprintf(msg, sizeof(msg),
                     "VR throw: speed rewrite bullet %d: hand %.1f m/s (ref %.1f) -> factor %.2f: |v| %.1f -> %.1f m/s at +0x40/+0x50 "
                     "(%.1f deg off the release direction)",
                     thr_spd.id, thr_spd.hand_mps, thr_spd.ref, thr_spd.factor, sp, sp * thr_spd.factor, ang);
            thr_spd.pending = 0;
            thr_spd.verify = 1;
            thr_spd.expect = sp * thr_spd.factor;
            thr_spd.applied_us = now;
        } else if (now - thr_spd.at_us > THR_SPD_WINDOW_US) {
            snprintf(msg, sizeof(msg), "VR throw: speed rewrite bullet %d dropped: %s (not valid within 300 ms)", thr_spd.id,
                     thr_spd.why[0] ? thr_spd.why : "no frame came");
            thr_spd.pending = 0;
        }
    } else if (thr_spd.verify && now - thr_spd.applied_us >= 15000u) {
        float v[4];
        if (gun_rd(thr_spd.bullet + 0x40, v, sizeof(v)))
            snprintf(msg, sizeof(msg), "VR throw: speed rewrite bullet %d: one frame later |v| = %.1f (expected about %.1f)",
                     thr_spd.id, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), thr_spd.expect);
        else
            snprintf(msg, sizeof(msg), "VR throw: speed rewrite bullet %d: one frame later the bullet is gone", thr_spd.id);
        thr_spd.verify = 0;
    }
    char chk[240];
    chk[0] = 0;
    if (thr_chk.stage && now - thr_chk.at_us >= (thr_chk.stage == 1 ? 15000u : 200000u)) {
        float v[4];
        const char *when = thr_chk.stage == 1 ? "one frame" : "200 ms";
        float ay, ap;
        vg_yaw_pitch(thr_chk.dir, &ay, &ap);
        if (gun_rd(thr_chk.bullet + 0x40, v, sizeof(v)) && isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2])) {
            const VrVec3 vel = vr_v3(v[0], v[1], v[2]);
            const float sp = vam_len(vel);
            float vy = 0.0f, vp = 0.0f;
            if (sp > 1e-4f) vg_yaw_pitch(vel, &vy, &vp);
            const float off = sp > 1e-4f ? vg_heading_diff_deg(vy, ay) : 0.0f;
            snprintf(chk, sizeof(chk),
                     "VR throw: bullet %d %s after the launch: velocity |v| %.1f m/s yaw %.1f pitch %.1f vs the aim yaw %.1f pitch %.1f "
                     "(heading off by %.1f deg%s)",
                     thr_chk.id, when, sp, vy, vp, ay, ap, off,
                     sp <= 1e-4f ? ", not moving" : off > 90.0f ? ": the game re-aimed it" : "");
        } else {
            snprintf(chk, sizeof(chk), "VR throw: bullet %d %s after the launch: gone (velocity not readable)", thr_chk.id, when);
        }
        thr_chk.stage = thr_chk.stage == 1 ? 2 : 0;
    }
    atomic_store_explicit(&thr_spd_busy, thr_spd.pending || thr_spd.verify || thr_chk.stage, memory_order_relaxed);
    thr_release();
    if (msg[0]) fprintf(stderr, "%s\n", msg);
    if (chk[0]) fprintf(stderr, "%s\n", chk);
}

/* The target as a plain point (kind 4) at p, the form the game uses when nothing is locked on. */
static void gun_set_point_target(float t[13], VrVec3 p) {
    const int32_t none = -1, point = 4;
    memcpy(&t[0], &none, 4);
    memset(&t[2], 0, 8);
    t[4] = t[5] = t[6] = 0.0f;
    t[7] = 1.0f;
    t[8] = p.x;
    t[9] = p.y;
    t[10] = p.z;
    t[11] = 1.0f;
    memcpy(&t[12], &point, 4);
}

/* The throw mode: the projectile leaves the hand along the release direction. 1 when it did. */
static int thr_redirect(uintptr_t bullet, uintptr_t tgt, uintptr_t mat, int32_t bullet_id, uint64_t now, uint32_t index) {
    VrVec3 origin, dir;
    float hand_mps = 0.0f;
    if (!thr_take(now, &origin, &dir, &hand_mps)) return 0;
    float m[3][4], t[13];
    if (!gun_mem_ok(mat, sizeof(m), 1) || !gun_mem_ok(tgt, 0x34, 1)) return 0;
    memcpy(m, (const void *)mat, sizeof(m));
    memcpy(t, (const void *)tgt, 0x34);
    const VgThrowGroup grp = vg_throw_group_make((const float (*)[4])m, origin, dir, thr.max_shift);
    int32_t handle, kind;
    memcpy(&handle, &t[0], 4);
    memcpy(&kind, &t[12], 4);
    const int locked = !(handle == -1 && kind == 4);
    const VrVec3 muzzle = vg_launch_origin((const float (*)[4])m);
    VrVec3 old_dir, new_dir;
    int moved = 0;
    const float turn = vg_throw_launch(m, locked ? NULL : &t[8], origin, dir, thr.max_shift, &moved, &old_dir, &new_dir);
    if (locked) gun_set_point_target(t, vr_add(vg_launch_origin((const float (*)[4])m), vr_scale(dir, 10.0f)));
    int finite = 1;
    for (int i = 0; i < 12; ++i) finite &= isfinite(m[i / 4][i % 4]) != 0;
    for (int i = 4; i < 12; ++i) finite &= isfinite(t[i]) != 0;
    if (!finite) {
        fprintf(stderr, "VR throw: bullet %d: non-finite result: the game's launch kept\n", bullet_id);
        return 0;
    }
    memcpy((void *)mat, m, sizeof(m));
    memcpy((void *)tgt, t, 0x34);
    ++thr.thrown;
    thr_grp.active = 1;
    thr_grp.first_id = bullet_id;
    thr_grp.count = 1;
    thr_grp.at_us = now;
    thr_grp.g = grp;
    vr_fp_throw_face_end(now + THR_FACE_AFTER_US);
    if (bullet) {
        thr_acquire();
        thr_chk.stage = 1;
        thr_chk.bullet = bullet;
        thr_chk.id = bullet_id;
        thr_chk.dir = new_dir;
        thr_chk.at_us = now;
        atomic_store_explicit(&thr_spd_busy, 1, memory_order_relaxed);
        thr_release();
    }
    float oy, op, ny, np;
    vg_yaw_pitch(old_dir, &oy, &op);
    vg_yaw_pitch(new_dir, &ny, &np);
    const VrVec3 o = vg_launch_origin((const float (*)[4])m);
    fprintf(stderr,
            "VR throw: bullet %d #%u thrown: yaw %.1f pitch %.1f -> yaw %.1f pitch %.1f (turn %.1f deg), muzzle %.2f %.2f %.2f -> %.2f %.2f %.2f%s%s\n",
            bullet_id, index, oy, op, ny, np, turn, muzzle.x, muzzle.y, muzzle.z, o.x, o.y, o.z,
            moved ? "" : " (hand too far from the game's muzzle: the game's origin kept)", locked ? ", lock-on overridden" : "");
    if (thr_spd.on && hand_mps > 0.0f && bullet) {
        const float factor = vg_throw_speed_factor(hand_mps, thr_spd.ref, thr_spd.lo, thr_spd.hi);
        if (fabsf(factor - 1.0f) < 0.02f) {
            fprintf(stderr, "VR throw: bullet %d: hand %.1f m/s (ref %.1f): the game's speed kept\n", bullet_id, hand_mps, thr_spd.ref);
        } else {
            thr_acquire();
            thr_spd.pending = 1;
            thr_spd.verify = 0;
            thr_spd.bullet = bullet;
            thr_spd.id = bullet_id;
            thr_spd.dir = new_dir;
            thr_spd.factor = factor;
            thr_spd.hand_mps = hand_mps;
            thr_spd.at_us = now;
            thr_spd.why[0] = 0;
            atomic_store_explicit(&thr_spd_busy, 1, memory_order_relaxed);
            thr_release();
        }
    }
    return 1;
}

/* A later bullet of the throw just redirected (same id as the first, or in the follow list, within
 * THR_GROUP_S): the first bullet's turn and origin shift, so the bullets keep their spread. 1 when it did. */
static int thr_follow(uintptr_t tgt, uintptr_t mat, int32_t bullet_id, uint64_t now) {
    if (!thr_grp.active) return 0;
    if ((double)(now - thr_grp.at_us) * 1e-6 > THR_GROUP_S) {
        thr_grp.active = 0;
        return 0;
    }
    if (bullet_id != thr_grp.first_id && !vg_id_in_list(thr.follow, bullet_id)) return 0;
    float m[3][4], t[13];
    if (!gun_mem_ok(mat, sizeof(m), 1) || !gun_mem_ok(tgt, 0x34, 1)) return 0;
    memcpy(m, (const void *)mat, sizeof(m));
    memcpy(t, (const void *)tgt, 0x34);
    int32_t handle, kind;
    memcpy(&handle, &t[0], 4);
    memcpy(&kind, &t[12], 4);
    const int locked = !(handle == -1 && kind == 4);
    const VrVec3 before = vg_launch_origin((const float (*)[4])m);
    VrVec3 old_dir, new_dir;
    int shifted = 0;
    const float turn = vg_throw_group_apply(m, locked ? NULL : &t[8], &thr_grp.g, 0.5f, &shifted, &old_dir, &new_dir);
    if (locked) gun_set_point_target(t, vr_add(vg_launch_origin((const float (*)[4])m), vr_scale(new_dir, 10.0f)));
    int finite = 1;
    for (int i = 0; i < 12; ++i) finite &= isfinite(m[i / 4][i % 4]) != 0;
    for (int i = 4; i < 12; ++i) finite &= isfinite(t[i]) != 0;
    if (!finite) {
        fprintf(stderr, "VR throw: bullet %d (follows bullet %d): non-finite result: the game's launch kept\n", bullet_id,
                thr_grp.first_id);
        return 0;
    }
    memcpy((void *)mat, m, sizeof(m));
    memcpy((void *)tgt, t, 0x34);
    float oy, op, ny, np;
    vg_yaw_pitch(old_dir, &oy, &op);
    vg_yaw_pitch(new_dir, &ny, &np);
    const VrVec3 o = vg_launch_origin((const float (*)[4])m);
    fprintf(stderr,
            "VR throw: bullet %d #%u thrown with bullet %d (member %u of the throw): yaw %.1f pitch %.1f -> yaw %.1f pitch %.1f (turn %.1f deg), "
            "muzzle %.2f %.2f %.2f -> %.2f %.2f %.2f%s\n",
            bullet_id, thr_grp.count, thr_grp.first_id, thr_grp.count + 1, oy, op, ny, np, turn, before.x, before.y, before.z, o.x, o.y, o.z,
            shifted ? "" : " (origin kept: not at the first bullet's muzzle or the hand too far)");
    ++thr_grp.count;
    return 1;
}

static ABI void gun_hook(BbHookContext *ctx) {
    if (!gun.on && !thr.on && !vr_visceral_active()) return;
    if (!gun.image) gun.image = (unsigned char *)(uintptr_t)(ctx->rip - (VA_SITE_LAUNCH - VA_BASE));
    thr_speed_poll(gun_now_us());
    uint64_t ret = 0;
    if (!gun_rd((uintptr_t)ctx->rsp, &ret, 8) || ret != (uint64_t)gun_va(VA_RET_EMITTER)) return;
    const uintptr_t desc = (uintptr_t)ctx->rsi, tgt = (uintptr_t)ctx->rdx, mat = (uintptr_t)ctx->rcx;
    int32_t bullet_id = 0;
    uint32_t owner = 0, player = 0;
    if (!gun_rd(desc + 4, &bullet_id, 4) || !gun_rd(desc + 0xC, &owner, 4)) return;
    const uint64_t now = gun_now_us();
    if (!gun_player_handle(&player) || owner != player) {
        if (gun.log && gun_log_ok(now))
            fprintf(stderr, "VR gun aim: bullet %d of owner 0x%x (player 0x%x): not the player's\n", bullet_id, owner, player);
        return;
    }
    ++gun.shots;
    if (thr.on) {
        ++thr.projectiles;
        const int listed = vg_id_in_list(thr.ids, bullet_id);
        const int fresh = bullet_id >= 0 && bullet_id < 1024 && !(thr.seen[bullet_id >> 5] & (1u << (bullet_id & 31)));
        if (fresh) thr.seen[bullet_id >> 5] |= 1u << (bullet_id & 31);
        if (thr.log && (fresh || gun_log_ok(now))) {
            thr_acquire();
            const int armed = thr_arm.armed;
            thr_release();
            fprintf(stderr, "VR throw: player projectile bullet %d reached the launch hook%s (throw list: %s, armed: %d)\n", bullet_id,
                    fresh ? " (first time)" : "", listed ? "listed" : "not listed", armed);
        }
        if (listed && thr_redirect((uintptr_t)ctx->rdi, tgt, mat, bullet_id, now, (uint32_t)ctx->r8)) return;
        if (thr_follow(tgt, mat, bullet_id, now)) return;
        if (listed && thr.log)
            fprintf(stderr, "VR throw: bullet %d is listed but not aimed: no throw armed (the hand did not release it): the game's own direction\n",
                    bullet_id);
    }
    if (vr_visceral_active() && vg_id_in_list(gun.ids, bullet_id)) { /* findings/41 7: before the lock-on early return */
        float vm[3][4], vt[13];
        if (gun_rd(mat, vm, sizeof(vm)) && gun_rd(tgt, vt, 0x34)) {
            int32_t vh, vk;
            memcpy(&vh, &vt[0], 4);
            memcpy(&vk, &vt[12], 4);
            gun_visceral_shot(now, !(vh == -1 && vk == 4), vm);
        }
    }
    if (!gun.on) return;
    if (!vg_id_in_list(gun.ids, bullet_id)) {
        if (gun.log && gun_log_ok(now))
            fprintf(stderr, "VR gun aim: player bullet %d not in BB_VR_GUN_AIM_IDS=%s: left alone\n", bullet_id, gun.ids);
        return;
    }
    float m[3][4], t[13];
    if (!gun_mem_ok(mat, sizeof(m), 1) || !gun_mem_ok(tgt, 0x34, 1)) return;
    memcpy(m, (const void *)mat, sizeof(m));
    memcpy(t, (const void *)tgt, 0x34);
    int32_t handle, kind;
    memcpy(&handle, &t[0], 4);
    memcpy(&kind, &t[12], 4);
    const int locked = !(handle == -1 && kind == 4);
    VrVec3 aim;
    const char *why = NULL, *pose_name = "";
    if (locked && !gun.lock_aim) {
        if (gun_log_ok(now))
            fprintf(stderr, "VR gun aim: shot %u bullet %d: lock-on (target handle 0x%x kind %d): the game's aim kept\n",
                    gun.shots, bullet_id, (unsigned)handle, kind);
        return;
    }
    if (!gun_aim(now, &aim, &why, &pose_name)) {
        if (gun_log_ok(now)) fprintf(stderr, "VR gun aim: shot %u bullet %d: %s: the game's aim kept\n", gun.shots, bullet_id, why);
        return;
    }
    float assist_deg = 0.0f;
    int assist_idx = -1, assist_n = 0;
    if (gun.assist.strength > 0.0f) { /* findings/41: pull the ray toward the enemy it nearly points at */
        VrVec3 pts[GUN_ASSIST_MAX];
        assist_n = gun_candidates(pts, GUN_ASSIST_MAX);
        const VrVec3 muzzle = vg_launch_origin((const float (*)[4])m);
        const VrVec3 pulled = vg_assist_dir(aim, muzzle, pts, assist_n, &gun.assist, &assist_idx, &assist_deg);
        if (assist_idx >= 0) { aim = pulled; ++gun.assist_hits; }
        ++gun.assist_n;
    }
    int used_target = 0;
    VrVec3 old_dir, new_dir;
    float *aim_point = locked ? NULL : &t[8];
    const VrVec3 z = vam_norm(vg_launch_axis((const float (*)[4])m, 2), vr_v3(0, 0, 1));
    const float off = aim_point ? vg_angle_deg(z, vr_sub(vr_v3(t[8], t[9], t[10]), vg_launch_origin((const float (*)[4])m))) : 0.0f;
    const float turn = vg_redirect(m, aim_point, aim, locked ? VG_REF_FORWARD : gun.ref, gun.max_off, &used_target, &old_dir, &new_dir);
    if (locked) /* BB_VR_GUN_AIM_LOCK=aim: the lock-on target becomes a point along the ray */
        gun_set_point_target(t, vr_add(vg_launch_origin((const float (*)[4])m), vr_scale(aim, 10.0f)));
    int finite = 1;
    for (int i = 0; i < 12; ++i) finite &= isfinite(m[i / 4][i % 4]) != 0;
    for (int i = 4; i < 12; ++i) finite &= isfinite(t[i]) != 0;
    if (!finite) {
        if (gun_log_ok(now)) fprintf(stderr, "VR gun aim: shot %u bullet %d: non-finite result: the game's aim kept\n", gun.shots, bullet_id);
        return;
    }
    memcpy((void *)mat, m, sizeof(m));
    memcpy((void *)tgt, t, 0x34);
    ++gun.redirected;
    if (gun_log_ok(now)) {
        float oy, op, ny, np;
        vg_yaw_pitch(old_dir, &oy, &op);
        vg_yaw_pitch(new_dir, &ny, &np);
        const VrVec3 o = vg_launch_origin((const float (*)[4])m);
        fprintf(stderr,
                "VR gun aim: shot %u bullet %d #%d: yaw %.1f pitch %.1f -> yaw %.1f pitch %.1f (turn %.1f deg from the %s, "
                "%s ray%s; launch Z vs aim point %.1f deg) muzzle %.2f %.2f %.2f\n",
                gun.shots, bullet_id, (int)(uint32_t)ctx->r8, oy, op, ny, np, turn,
                used_target ? "aim point" : "launch Z", pose_name, locked ? ", lock-on overridden" : "",
                off, o.x, o.y, o.z);
        if (gun.assist.strength > 0.0f)
            fprintf(stderr, "VR gun aim: assist %s (%d candidates, %u of %u shots pulled)\n",
                    assist_idx >= 0 ? "pulled" : "no target in the cone", assist_n, gun.assist_hits, gun.assist_n);
        if (assist_idx >= 0)
            fprintf(stderr, "VR gun aim: assist target #%d, ray %.1f deg off it\n", assist_idx, assist_deg);
    }
}

void vr_gun_register(void) {
    const char *v = getenv("BB_VR_GUN_AIM");
    gun.on = v && !strcmp(v, "1");
    v = getenv("BB_VR_GUN_AIM_LOG");
    gun.log = v && !strcmp(v, "1");
    v = getenv("BB_VR_GUN_AIM_HAND");
    gun.side = v && !strcmp(v, "right") ? 1 : 0;
    v = getenv("BB_VR_GUN_AIM_LOCK");
    gun.lock_aim = v && !strcmp(v, "aim");
    v = getenv("BB_VR_GUN_AIM_REF");
    gun.ref = v && !strcmp(v, "target") ? VG_REF_TARGET : v && !strcmp(v, "forward") ? VG_REF_FORWARD : VG_REF_AUTO;
    v = getenv("BB_VR_GUN_AIM_POSE");
    gun.use_grip = v && !strcmp(v, "grip");
    v = getenv("BB_VR_GUN_AIM_GRIP_PITCH");
    gun.grip_pitch = v ? (float)atof(v) : 0.0f;
    gun.max_off = 30.0f;
    v = getenv("BB_VR_GUN_AIM_IDS");
    snprintf(gun.ids, sizeof(gun.ids), "%s", v && *v ? v : "500-999");
    gun.assist = vg_assist_defaults();
    v = getenv("BB_VR_PARRY_ASSIST");
    if (v) gun.assist.strength = vr_clampf((float)atof(v), 0.0f, 1.0f);
    v = getenv("BB_VR_PARRY_RADIUS_M");
    if (v && atof(v) > 0.0) gun.assist.radius = (float)atof(v);
    v = getenv("BB_VR_PARRY_CONE_MIN");
    if (v && atof(v) >= 0.0) gun.assist.cone_min = (float)atof(v);
    v = getenv("BB_VR_PARRY_CONE_MAX");
    if (v && atof(v) >= 0.0) gun.assist.cone_max = (float)atof(v);
    v = getenv("BB_VR_PARRY_RANGE_M");
    if (v && atof(v) > 0.0) gun.assist.range = (float)atof(v);
    v = getenv("BB_VR_THROW_AIM");
    thr.on = !(v && !strcmp(v, "0"));
    v = getenv("BB_VR_THROW_LOG");
    thr.log = !(v && !strcmp(v, "0"));
    v = getenv("BB_VR_THROW_IDS");
    snprintf(thr.ids, sizeof(thr.ids), "%s", v && *v ? v : BBXR_THROW_BULLET_IDS);
    v = getenv("BB_VR_THROW_FOLLOW_IDS");
    snprintf(thr.follow, sizeof(thr.follow), "%s", v && *v ? v : BBXR_THROW_FOLLOW_IDS);
    v = getenv("BB_VR_THROW_SHIFT_M");
    thr.max_shift = v && atof(v) > 0.0 ? (float)atof(v) : 2.0f;
    v = getenv("BB_VR_THROW_SPEED");
    thr_spd.on = !(v && !strcmp(v, "0"));
    v = getenv("BB_VR_THROW_SPEED_REF");
    thr_spd.ref = v && atof(v) > 0.0 ? (float)atof(v) : 7.0f;
    v = getenv("BB_VR_THROW_SPEED_MIN");
    thr_spd.lo = v && atof(v) > 0.0 ? (float)atof(v) : 0.3f;
    v = getenv("BB_VR_THROW_SPEED_MAX");
    thr_spd.hi = v && atof(v) > 0.0 ? (float)atof(v) : 1.5f;
    if (thr_spd.hi < thr_spd.lo) thr_spd.hi = thr_spd.lo;
    bb_hook_set_gun_impl(gun_hook);
    if (thr.on)
        fprintf(stderr, "VR throw: aim on (ids %s, follow ids %s, hand shift limit %.1f m%s); needs the hook \"VR Gun Aim (bullet spawn)\" and first person\n",
                thr.ids, thr.follow, thr.max_shift, thr.log ? ", logs every projectile id" : "");
    if (thr.on && thr_spd.on)
        fprintf(stderr, "VR throw: speed from the hand on (ref %.1f m/s, factor %.2f..%.2f; [L] unverified live)\n", thr_spd.ref,
                thr_spd.lo, thr_spd.hi);
    if (gun.on)
        fprintf(stderr, "VR gun aim: on (hand %s, lock-on %s, ids %s, ref %s, pose %s%s); needs the hook \"VR Gun Aim (bullet spawn)\" "
                        "and first person\n",
                gun.side ? "right" : "left", gun.lock_aim ? "aim" : "keep", gun.ids,
                gun.ref == VG_REF_TARGET ? "target" : gun.ref == VG_REF_FORWARD ? "forward" : "auto",
                gun.use_grip ? "grip" : "aim", gun.log ? ", log" : "");
    if (gun.on)
        fprintf(stderr, "VR gun aim: parry assist strength %.2f (cone %.1f-%.1f deg, radius %.2f m, range %.0f m; BB_VR_PARRY_ASSIST=0 off)\n",
                gun.assist.strength, gun.assist.cone_min, gun.assist.cone_max, gun.assist.radius, gun.assist.range);
}
