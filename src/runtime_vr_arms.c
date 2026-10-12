/* VR arms in the game (BB_VR_ARMS=1, docs/vr/findings/24-arms-ik.md): the hunter's arms posed by the
 * IK the VR sandbox uses (src/vr_arm_math.h, tests/test_vr_arms.c) after the animation and before
 * the copy the skinning and the weapon attachment read.
 *
 * The hook (findings/13 3.2, bytes re-checked in out/eboot.elf, findings/24 2): the two
 * "AnimationToModel" copies 0x1C375D0 and 0x1E19BC0 take a character's synced model-space pose
 * (hkaPose::getSyncedPoseModelSpace 0xAC4F00) and write every bone as a 3x4 matrix in FLVER order.
 * Right after that call (A1 0x1C37650, A2 0x1E19BFE, 19 bytes `48 89 45 80 44 8b 68 08 48 8b 45 88
 * c7 40 08 01 00 00 00`, replay-after) rax = &pose.modelPose (hkArray<hkQsTransform>: data, size).
 * The handler acts only when rax is the player's pose + 0x18:
 *   player = [[0x593E878] + 0x60] (PlayerIns), pose = [[[P + 0x3B0] + 0x30] + 0x28] + 0x38
 * and then rewrites the model transforms of both arm subtrees (UpperArm and everything below it:
 * Forearm, Hand, fingers, Weapon, the twist and elbow helpers), marks them local-dirty (boneFlags |=
 * 1) and clears localInSync (+0x39), exactly as hkaPose::accessSyncedPoseModelSpace (0xAC59A0) does
 * for the whole pose, so a later local-space read is rebuilt from the new model pose.
 * Which of the two copies runs for the player is logged (BB_VR_ARMS_LOG=1): it is not proven.
 *
 * Targets (arm_src=):
 *   rest  a "ready" pose without controllers: the wrists in front of the chest relative to the live
 *         shoulders (arm_rest=out,up,forward,pitch). The interim "arms higher" mode.
 *   fake  scripted grip poses relative to the head (arm_fake_r= / arm_fake_l=, OpenXR head frame:
 *         x right, y up, -z forward, meters; yaw, pitch, roll degrees): the controller path end to
 *         end (the head, the H1 camera, model space) without a headset.
 *   xr    the controllers (default): bbgpu_vr_hands = the grips and the head of one GetInput
 *         snapshot, placed through the H1 camera like the head (vam_grip_to_model); per side
 *         rest while that grip is invalid or the snapshot is stale, or without the H1 camera.
 * fake and xr need first person (the H1 camera, vr_arms_view); rest also works in third person with
 * arm_tp=1.
 *
 * Blend: weight 0..arm_weight, faded in over arm_in s and out over arm_out s; during an attack (the
 * TimeAct ring's newest animation has a sub-id 30000-49999, findings/13 5.4 [L]) the target weight
 * is arm_attack (0: the game's swing). Rotations are blended per bone (nlerp), positions rebuilt
 * down the chain from the animated locals, so bone lengths never change.
 *
 * Threads: the copies may run on animation workers; the handler is non-reentrant (a busy flag: a
 * second thread skips), reads the H1 snapshot through a two-slot sequence, and reads guest memory
 * only through its own readable-memory guard (per-thread region cache).
 *
 * Environment (off unless BB_VR_ARMS=1; needs BB_HOOKS="...;VR Arms IK (AnimationToModel)"):
 *   BB_VR_ARMS=1            enable
 *   BB_VR_ARMS_LOG=1        once per second: which copy fired, the weight, the targets and reach
 *   BB_VR_ARMS_SRC=rest|fake|xr   the start value of arm_src (xr)
 *   BB_VR_ARMS_CONTROL=<file>     live keys (default: BB_VR_FP_CONTROL's file, the same fp.txt):
 *     arm=0|1 arm_src=rest|fake|xr arm_weight=<0..1> arm_attack=<0..1> arm_in=<s> arm_out=<s>
 *     arm_tp=0|1 arm_rest=<out,up,forward[,pitch_deg]> arm_pole=<out,down,back> arm_twist=<0..1>
 *     arm_fake_r=<x,y,z[,yaw,pitch,roll]> arm_fake_l=... arm_offset=<x,y,z[,pitch_deg]> arm_log=0|1
 *     arm_rot_l=<yaw,pitch> arm_rot_r=...: per hand aim trim (deg, + = right / up, controller axes)
 *     arm_roll_detach=0|1 (1): while a roll / quickstep / backstep plays the shoulders and clavicles come from a
 *       camera-frame pin of the last frame before it, not from the tumbling animation (findings/24 section 8)
 *   BB_VR_ARMS_ROLL_DETACH=0|1 (1) the start value of arm_roll_detach; BB_VR_FP_LOG / BB_VR_EVADE_LOG=1: a line per detach/attach
 */
#define _GNU_SOURCE
#include "hooks.h"
#include "vr_arms.h"
#include "vr_arms_roll.h"
#include "vr_evade_scale.h"
#include "vr_arm_math.h"
#include "vr_fp.h"
#include "vr_fp_math.h"
#include "vr_hitbox.h"
#include "gpu/bbgpu.h"
#include "vr_mem_cache.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#include <unistd.h>
#endif

#define VA_BASE 0x400000u
#define VA_SITE_A1 0x1C37650u        /* in 0x1C375D0, after getSyncedPoseModelSpace */
#define VA_SITE_A2 0x1E19BFEu        /* in 0x1E19BC0 (pose = [rdi + 0x28] + 0x38 = the behaviour module's) */
#define VA_SITE_A3 0x1C377C6u        /* in 0x1C37770, the i16-remap copy (per part?), rax = &modelPose */
#define VA_SITE_SYNC 0xAC4F0Eu       /* hkaPose::getSyncedPoseModelSpace 0xAC4F00 after its sync: rbx = the
                                        pose, every caller (25 call sites); [rsp + 0x18] = the return address */
#define VA_SITE_PLAYER 0x1CF845Fu     /* the PlayerIns pose update 0x1CF83F0, right after its hkaPose::syncModelSpace
                                        (0x1CF845A): r14 = the pose ([[[chr + 0x3B0] + 0x30] + 0x28] + 0x38) */
#define ARM_SITES 5
#define ARM_CALLERS 24
#define VA_WORLD_CHR_MAN 0x593E878u
#define VA_PLAYER_INS_VTABLE 0x578F310u
#define ARM_MAX_BONES 256
#define ARM_VIEW_STALE_US 300000u

enum { SRC_REST = 0, SRC_FAKE = 1, SRC_XR = 2 };
enum { AT_PLAYER = 0, AT_COPY = 1, AT_SYNC = 2 };
static const char *const at_names[] = {"player", "copy", "sync"};
static int at_parse(const char *v) { return !strcmp(v, "copy") ? AT_COPY : !strcmp(v, "sync") ? AT_SYNC : AT_PLAYER; }
static const char *const src_names[] = {"rest", "fake", "xr"};
static const char *const site_names[ARM_SITES] = {"A1", "A2", "A3", "sync", "player"};

/* Bones by name, once per skeleton. */
typedef struct {
    int clavicle, upper, fore, hand, weapon, twist, twist1;
    VamArmBind bind;
    int ok;
} ArmBones;

static struct {
    int ready, enabled, log;
    unsigned char *image;
    /* settings */
    int on, src, tp;
    float weight_max, attack_weight, in_s, out_s;
    float rest[4];          /* out, up, forward (m), pitch down (deg) */
    float pole[3];          /* out, down, back (unit-ish; right side mirrored by the side) */
    float twist_share;
    float soft, give, fore_roll; /* arm_soft (reach fraction easing starts), arm_give (m the shoulder may slide), arm_fore_roll */
    float offset[4];        /* grip -> wrist, right hand, OpenXR grip frame: x, y, z (m), pitch (deg) */
    float fake[2][6];       /* left, right: grip relative to the head (OpenXR): x y z, yaw pitch roll */
    float rot[2][2];        /* left, right: aim trim, yaw right / pitch up (deg), not mirrored */
    const char *control;
    uint64_t control_checked, control_mtime, control_size;
    /* per skeleton */
    uintptr_t skel;
    int n_bones;
    ArmBones arm[2];        /* 0 left, 1 right */
    int16_t parent[ARM_MAX_BONES];
    uint8_t side_of[ARM_MAX_BONES]; /* 1 left subtree, 2 right subtree (UpperArm and below), 0 other */
    /* player pose address (refreshed every 100 ms) */
    uintptr_t player, pose, bag;
    uint64_t pose_checked;
    /* per frame */
    float w;
    uint64_t last_us, start;
    int xr_warned, xr_far_warned;
    /* the virtual upper body (findings/24 5.4): the shoulders turned toward the head's yaw */
    int body_on;            /* arm_body=0|1 (1) */
    float body_dz, body_tau; /* arm_body_dz=<deg> (25), arm_body_tau=<s> (0.2; 0 = at once) */
    float body_off;         /* the shoulders' turn now (radians about model +Y) */
    VamShoulderYaw shoulder_yaw; /* their heading in the world (follows the head only, findings/24 5.6) */
    /* log counters */
    uint64_t next_log;
    uint32_t fired[ARM_SITES], player_fired[ARM_SITES], writes, skipped_busy, stale_view;
    /* diagnostics printed unconditionally (findings/24 5): the first call per site, the first player
     * match per site, the player pose when it changes, the callers of getSyncedPoseModelSpace for the
     * player's pose, the first edit, and the per-second line for the first 20 seconds */
    int first_call[ARM_SITES], first_match[ARM_SITES], first_edit, diag_lines;
    uintptr_t last_pose_logged;
    const char *pose_fail_logged;
    uint64_t callers[ARM_CALLERS];
    uint32_t caller_hits[ARM_CALLERS];
    int n_callers;
    int at;                 /* where the arms are written (arm_at=): AT_PLAYER (default), AT_COPY (A1-A3), AT_SYNC */
    float last_upper[2][4]; /* the upper arm rotation last written per side (a second call in one frame skips) */
    int anim, attack;
    int roll_detach, roll_log, roll_was; /* arm_roll_detach, the log switch, the last frame's state */
    VrArmsRoll roll;
    float reach[2];
    int clamped[2];
    float roll_prev[2];  /* last wrist roll per side (unwrapped continuity) */
    int has_roll[2];
    VamArmPose dbg[2];   /* last solve per side, for the per-second line */
    float dbg_upper[2], dbg_fore[2];
} arms;

static _Atomic int arms_busy;
/* The player's pose as last looked up (under the lock), for the lock-free fast path of the many calls
 * that are not the player's: they never take the lock while the lookup is fresh (100 ms). */
static _Atomic uintptr_t arms_pose_pub;
static _Atomic uint64_t arms_pose_pub_us;
static _Atomic uint32_t arms_fast_other[ARM_SITES]; /* calls that left on the fast path */
static _Atomic uint32_t arms_spins;                  /* edit-site calls that had to wait for the lock */

static void arms_lock_spin(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&arms_busy, &expected, 1)) return;
    atomic_fetch_add_explicit(&arms_spins, 1, memory_order_relaxed);
    for (unsigned n = 0;; ++n) {
        expected = 0;
        if (atomic_compare_exchange_weak(&arms_busy, &expected, 1)) return;
#ifdef _WIN32
        if (n > 64) SwitchToThread();
#else
        if (n > 64) sched_yield();
#endif
    }
}

/* ---- the H1 snapshot (two slots, a sequence number) ---- */
typedef struct {
    uint64_t time_us;
    VrVec3 cam_pos;   /* the camera in the character's model space */
    VrVec3 shift;     /* the comfort dash's camera offset in model space (the arms move with the view) */
    float yaw;        /* the character's model -> world yaw this snapshot was taken with */
    VrQuat cam_rot;
    int head_valid;
    VrVec3 head_pos;  /* OpenXR */
    VrQuat head_rot;
    VrVec3 body_pos;  /* the character's world position (vr_fp_body), for the hitbox overlay */
    VrVec3 rel_vel;   /* (camera - body) in model space per second, from the last two snapshots (vr_roll_rel_vel) */
} ArmView;
static ArmView arm_view[2];
static _Atomic uint32_t arm_view_seq;

static uint64_t arm_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

void vr_arms_view(const float rows[4][4], int head_valid, const float head_pos[3], const float head_rot[4]) {
    VrVec3 pos;
    float yaw;
    if (!vr_fp_body(&pos, &yaw)) return;
    const uint32_t seq = atomic_load_explicit(&arm_view_seq, memory_order_relaxed);
    ArmView *v = &arm_view[(seq + 1) & 1];
    const uint64_t t_us = arm_now_us();
    static VrVec3 prev_cam, prev_body;
    static uint64_t prev_us;
    const VrVec3 cam_w = vr_v3(rows[3][0], rows[3][1], rows[3][2]);
    v->rel_vel = prev_us ? vr_roll_rel_vel(prev_cam, prev_body, cam_w, pos, yaw, (float)(t_us - prev_us) * 1e-6f) : vr_v3(0, 0, 0);
    prev_cam = cam_w;
    prev_body = pos;
    prev_us = t_us;
    v->time_us = t_us;
    vam_camera_in_model(rows, pos, yaw, &v->cam_pos, &v->cam_rot);
    v->yaw = yaw;
    v->body_pos = pos;
    v->shift = vr_qrotate(vr_qaxis(vr_v3(0, 1, 0), -yaw), vr_fp_dash_offset()); /* world -> model (a vector) */
    if (!(vam_len(v->shift) < 3.0f)) v->shift = vr_v3(0, 0, 0);
    v->head_valid = head_valid && head_pos && head_rot;
    if (v->head_valid) {
        v->head_pos = vr_v3(head_pos[0], head_pos[1], head_pos[2]);
        v->head_rot = vr_qnormalize(vr_q(head_rot[0], head_rot[1], head_rot[2], head_rot[3]));
    }
    atomic_store_explicit(&arm_view_seq, seq + 1, memory_order_release);
}
static int arm_view_read(ArmView *out) {
    for (int tries = 0; tries < 3; ++tries) {
        const uint32_t a = atomic_load_explicit(&arm_view_seq, memory_order_acquire);
        if (!a) return 0;
        *out = arm_view[a & 1];
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&arm_view_seq, memory_order_relaxed) == a) return 1;
    }
    return 0;
}
/* The latest first-person frame for the hitbox overlay (src/runtime_vr_hitbox.c): 0 without one, or
 * when it is older than max_age_us or has no headset pose. */
int vr_arms_hitbox_view(VhbView *out, uint64_t max_age_us) {
    ArmView v;
    if (!arm_view_read(&v) || !v.head_valid || arm_now_us() - v.time_us > max_age_us) return 0;
    out->cam_pos = v.cam_pos;
    out->cam_rot = v.cam_rot;
    out->head_pos = v.head_pos;
    out->head_rot = v.head_rot;
    out->body_pos = v.body_pos;
    out->body_yaw = v.yaw;
    return 1;
}

/* ---- guest memory (own guard: this may run on another thread than H1's) ---- */
#ifdef _WIN32
/* VirtualQuery of the guest mappings took ms on a cold lookup (run12e: Arms IK hitches of 7-11 ms): see vr_mem_cache.h */
static _Thread_local VmCache arm_cache;
static int arm_mem_ok(uintptr_t a, size_t n, int write) {
    (void)write; /* the arrays written are the game's live heap data: readable is writable */
    return vm_readable(&arm_cache, a, n);
}
#else
static int arm_mem_ok(uintptr_t a, size_t n, int for_write) {
    (void)for_write;
    int fds[2];
    if (a < 0x10000 || a + n < a || pipe(fds) != 0) return 0;
    const ssize_t w = write(fds[1], (const void *)a, n > 4096 ? 4096 : n);
    close(fds[0]);
    close(fds[1]);
    return w >= 0;
}
#endif

static int rd_ptr(uintptr_t a, uintptr_t *out) {
    uint64_t v;
    if (!arm_mem_ok(a, 8, 0)) return 0;
    memcpy(&v, (const void *)a, 8);
    if (v < 0x10000 || (v & 7)) return 0;
    *out = (uintptr_t)v;
    return 1;
}
static uintptr_t va(uint32_t v) { return (uintptr_t)(arms.image + (v - VA_BASE)); }
/* hkArray { T *data; int32 size; int32 capacityAndFlags; } */
static int rd_array(uintptr_t a, uintptr_t *data, int *size) {
    uint64_t d;
    int32_t n;
    if (!arm_mem_ok(a, 12, 0)) return 0;
    memcpy(&d, (const void *)a, 8);
    memcpy(&n, (const void *)(a + 8), 4);
    if (!d || (d & 3) || n <= 0 || n > ARM_MAX_BONES) return 0;
    *data = (uintptr_t)d;
    *size = n;
    return 1;
}

/* ---- settings ---- */

static int env_on(const char *name) {
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0") != 0;
}
static int src_parse(const char *v) {
    return !strcmp(v, "fake") ? SRC_FAKE : !strcmp(v, "xr") ? SRC_XR : SRC_REST;
}
static void arms_init(void) {
    if (arms.ready) return;
    arms.ready = 1;
    arms.enabled = env_on("BB_VR_ARMS");
    arms.log = env_on("BB_VR_ARMS_LOG");
    arms.on = 1;
    arms.roll_detach = !getenv("BB_VR_ARMS_ROLL_DETACH") || env_on("BB_VR_ARMS_ROLL_DETACH");
    arms.roll_log = env_on("BB_VR_FP_LOG") || env_on("BB_VR_EVADE_LOG");
    const char *e = getenv("BB_VR_ARMS_SRC");
    arms.src = e ? src_parse(e) : SRC_XR; /* xr falls back to rest while there are no valid hands */
    arms.weight_max = 1.0f;
    arms.attack_weight = 0.0f;
    arms.in_s = 0.15f;
    arms.out_s = 0.08f;
    const float rest[4] = {0.02f, -0.30f, 0.32f, 25.0f};
    memcpy(arms.rest, rest, sizeof(rest));
    const float pole[3] = {0.4f, 0.85f, 0.35f};
    memcpy(arms.pole, pole, sizeof(pole));
    arms.twist_share = 1.0f;
    arms.soft = 0.92f;
    arms.give = 0.04f;
    arms.fore_roll = 0.35f;
    const float offset[4] = {0.0f, -0.01f, 0.07f, -15.0f}; /* the sandbox's hand_offset */
    memcpy(arms.offset, offset, sizeof(offset));
    const float fake[2][6] = {{-0.18f, -0.38f, -0.32f, 0.0f, -20.0f, 0.0f}, {0.18f, -0.38f, -0.32f, 0.0f, -20.0f, 0.0f}};
    memcpy(arms.fake, fake, sizeof(fake));
    arms.body_on = 1;
    arms.body_dz = 25.0f;
    arms.body_tau = 0.2f;
    e = getenv("BB_VR_ARMS_AT");
    arms.at = e ? at_parse(e) : AT_PLAYER;
    arms.control = getenv("BB_VR_ARMS_CONTROL");
    if (!arms.control || !*arms.control) arms.control = getenv("BB_VR_FP_CONTROL");
    arms.start = arm_now_us();
    if (arms.enabled)
        fprintf(stderr, "VR arms: on (src %s, written at %s (arm_at=); player site 0x%x, A1 0x%x / A2 0x%x; control %s)\n",
                src_names[arms.src], at_names[arms.at], VA_SITE_PLAYER, VA_SITE_A1,
                VA_SITE_A2, arms.control ? arms.control : "none");
}

static void set_floats(const char *v, float *dst, int min_n, int max_n) {
    float tmp[8];
    const int n = vr_parse_floats(v, tmp, max_n);
    if (n >= min_n) memcpy(dst, tmp, (size_t)n * sizeof(float));
}
static void arms_apply_key(const char *k, const char *v) {
    char *end;
    const float x = strtof(v, &end);
    const int ok = end != v && isfinite(x);
    if (!strcmp(k, "arm")) arms.on = ok && x != 0.0f;
    else if (!strcmp(k, "arm_src")) arms.src = src_parse(v);
    else if (!strcmp(k, "arm_weight") && ok) arms.weight_max = vr_clampf(x, 0.0f, 1.0f);
    else if (!strcmp(k, "arm_attack") && ok) arms.attack_weight = vr_clampf(x, 0.0f, 1.0f);
    else if (!strcmp(k, "arm_in") && ok) arms.in_s = vr_clampf(x, 0.0f, 2.0f);
    else if (!strcmp(k, "arm_out") && ok) arms.out_s = vr_clampf(x, 0.0f, 2.0f);
    else if (!strcmp(k, "arm_tp")) arms.tp = ok && x != 0.0f;
    else if (!strcmp(k, "arm_log")) arms.log = ok && x != 0.0f;
    else if (!strcmp(k, "arm_at")) arms.at = at_parse(v);
    else if (!strcmp(k, "arm_roll_detach")) arms.roll_detach = ok && x != 0.0f;
    else if (!strcmp(k, "arm_body")) arms.body_on = ok && x != 0.0f;
    else if (!strcmp(k, "arm_body_dz") && ok) arms.body_dz = vr_clampf(x, 0.0f, 180.0f);
    else if (!strcmp(k, "arm_body_tau") && ok) arms.body_tau = vr_clampf(x, 0.0f, 5.0f);
    else if (!strcmp(k, "arm_rest")) set_floats(v, arms.rest, 3, 4);
    else if (!strcmp(k, "arm_pole")) set_floats(v, arms.pole, 3, 3);
    else if (!strcmp(k, "arm_twist") && ok) arms.twist_share = vr_clampf(x, 0.0f, 1.0f);
    else if (!strcmp(k, "arm_soft") && ok) arms.soft = x <= 0.0f ? 0.0f : vr_clampf(x, 0.5f, 0.99f);
    else if (!strcmp(k, "arm_give") && ok) arms.give = vr_clampf(x, 0.0f, 0.15f);
    else if (!strcmp(k, "arm_fore_roll") && ok) arms.fore_roll = vr_clampf(x, 0.0f, 1.0f);
    else if (!strcmp(k, "arm_offset")) set_floats(v, arms.offset, 3, 4);
    else if (!strcmp(k, "arm_fake_r")) set_floats(v, arms.fake[1], 3, 6);
    else if (!strcmp(k, "arm_fake_l")) set_floats(v, arms.fake[0], 3, 6);
    else if (!strcmp(k, "arm_rot_l")) set_floats(v, arms.rot[0], 2, 2);
    else if (!strcmp(k, "arm_rot_r")) set_floats(v, arms.rot[1], 2, 2);
}
/* The control file (the first-person one by default): only the arm* keys are ours. */
static void arms_poll_control(uint64_t now) {
    if (!arms.control || now - arms.control_checked < 100000) return;
    arms.control_checked = now;
    struct stat st;
    if (stat(arms.control, &st) != 0) return;
    if ((uint64_t)st.st_mtime == arms.control_mtime && (uint64_t)st.st_size == arms.control_size) return;
    arms.control_mtime = (uint64_t)st.st_mtime;
    arms.control_size = (uint64_t)st.st_size;
    FILE *f = fopen(arms.control, "rb");
    if (!f) return;
    char text[4096];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    for (char *save = NULL, *tok = strtok_r(text, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save)) {
        if (strncmp(tok, "arm", 3) != 0) continue;
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        arms_apply_key(tok, eq + 1);
    }
    fprintf(stderr, "VR arms: control: arm %d src %s weight %.2f attack %.2f in %.2f out %.2f tp %d rest (%.2f %.2f %.2f, %.0f deg) "
                    "pole (%.2f %.2f %.2f) twist %.2f soft %.2f give %.3f fore_roll %.2f offset (%.3f %.3f %.3f, %.0f deg) fake_r (%.2f %.2f %.2f) fake_l (%.2f %.2f %.2f)\n",
            arms.on, src_names[arms.src], arms.weight_max, arms.attack_weight, arms.in_s, arms.out_s, arms.tp, arms.rest[0],
            arms.rest[1], arms.rest[2], arms.rest[3], arms.pole[0], arms.pole[1], arms.pole[2], arms.twist_share, arms.soft, arms.give, arms.fore_roll,
            arms.offset[0], arms.offset[1], arms.offset[2], arms.offset[3], arms.fake[1][0], arms.fake[1][1],
            arms.fake[1][2], arms.fake[0][0], arms.fake[0][1], arms.fake[0][2]);
}

/* ---- the skeleton ---- */

static int bone_named(uintptr_t bones, int i, const char *name) {
    uint64_t s;
    if (!arm_mem_ok(bones + (uintptr_t)i * 16, 8, 0)) return 0;
    memcpy(&s, (const void *)(bones + (uintptr_t)i * 16), 8);
    s &= ~(uint64_t)1; /* hkStringPtr: bit 0 is the "owned" flag */
    const size_t n = strlen(name) + 1;
    if (!s || !arm_mem_ok((uintptr_t)s, n, 0)) return 0;
    return !memcmp((const void *)(uintptr_t)s, name, n);
}
static int find_bone(uintptr_t bones, int n, const char *name) {
    for (int i = 0; i < n; ++i)
        if (bone_named(bones, i, name)) return i;
    return -1;
}
/* The reference pose's model transform of bone i (composed up the parents). */
static int bind_bone(uintptr_t ref, int i, VrXform *out) {
    VrXform acc = vr_xf_identity();
    int chain[64], n = 0;
    for (int b = i; b >= 0 && n < 64; b = arms.parent[b]) {
        if (b >= arms.n_bones) return 0;
        chain[n++] = b;
    }
    for (int k = n - 1; k >= 0; --k) {
        VrXform local;
        if (!vr_xf_from_qs((const float *)(ref + (uintptr_t)chain[k] * 48), &local)) return 0;
        acc = vr_xf_mul(acc, local);
    }
    *out = acc;
    return 1;
}

/* Bones, the bind arm data and the subtree map, once per skeleton. */
static int arms_skeleton(uintptr_t skel) {
    if (skel == arms.skel) return arms.arm[0].ok && arms.arm[1].ok;
    arms.skel = skel;
    arms.arm[0].ok = arms.arm[1].ok = 0;
    uintptr_t parents, bones, ref;
    int n_parents, n_bones, n_ref;
    if (!rd_array(skel + 0x18, &parents, &n_parents) || !rd_array(skel + 0x28, &bones, &n_bones) ||
        !rd_array(skel + 0x38, &ref, &n_ref) || n_parents != n_bones || n_ref != n_bones ||
        !arm_mem_ok(parents, (size_t)n_bones * 2, 0) || !arm_mem_ok(ref, (size_t)n_bones * 48, 0)) {
        fprintf(stderr, "VR arms: skeleton 0x%llx unreadable\n", (unsigned long long)skel);
        return 0;
    }
    arms.n_bones = n_bones;
    memcpy(arms.parent, (const void *)parents, (size_t)n_bones * 2);
    for (int i = 0; i < n_bones; ++i)
        if (arms.parent[i] >= i) { fprintf(stderr, "VR arms: skeleton not parent-first at %d\n", i); return 0; }
    memset(arms.side_of, 0, sizeof(arms.side_of));
    for (int side = 0; side < 2; ++side) {
        const char *p = side ? "R_" : "L_";
        char name[64];
        ArmBones *a = &arms.arm[side];
#define FIND(field, fmt) snprintf(name, sizeof(name), fmt, p), a->field = find_bone(bones, n_bones, name)
        FIND(clavicle, "%sClavicle");
        FIND(upper, "%sUpperArm");
        FIND(fore, "%sForearm");
        FIND(hand, "%sHand");
        FIND(weapon, "%sWeapon");
        FIND(twist, "Support_%sForeArmTwist");
        FIND(twist1, "Support_%sForeArmTwist1");
        int fingers[5];
        for (int f = 0; f < 5; ++f) {
            snprintf(name, sizeof(name), "%sFinger%d", p, f);
            fingers[f] = find_bone(bones, n_bones, name);
        }
#undef FIND
        if (a->upper < 0 || a->fore < 0 || a->hand < 0 || arms.parent[a->fore] != a->upper || arms.parent[a->hand] != a->fore) {
            fprintf(stderr, "VR arms: %sUpperArm/Forearm/Hand not found as a chain (%d %d %d)\n", p, a->upper, a->fore, a->hand);
            continue;
        }
        VrXform u, fo, h, t, t1;
        if (!bind_bone(ref, a->upper, &u) || !bind_bone(ref, a->fore, &fo) || !bind_bone(ref, a->hand, &h)) continue;
        VamArmBind *b = &a->bind;
        memset(b, 0, sizeof(*b));
        b->upper = u.t; b->fore = fo.t; b->hand = h.t;
        b->upper_rot = u.q; b->fore_rot = fo.q; b->hand_rot = h.q;
        b->has_twist = a->twist >= 0 && bind_bone(ref, a->twist, &t);
        b->has_twist1 = a->twist1 >= 0 && bind_bone(ref, a->twist1, &t1);
        b->twist_rot = b->has_twist ? t.q : fo.q;
        b->twist1_rot = b->has_twist1 ? t1.q : fo.q;
        b->twist1_t = 0.5f;
        if (b->has_twist1) {
            const VrVec3 seg = vr_sub(h.t, fo.t);
            b->twist1_t = vr_clampf(vr_dot(vr_sub(t1.t, fo.t), seg) / fmaxf(vr_dot(seg, seg), 1e-6f), 0.0f, 1.0f);
        }
        VrVec3 knuckles = vr_v3(0, 0, 0), thumb = vr_add(h.t, vr_v3(0, 0, -0.04f));
        int count = 0;
        for (int f = 0; f < 5; ++f) {
            VrXform x;
            if (fingers[f] < 0 || !bind_bone(ref, fingers[f], &x)) continue;
            if (f == 0) thumb = x.t;
            else { knuckles = vr_add(knuckles, x.t); ++count; }
        }
        if (!count) knuckles = vr_add(h.t, vr_scale(vam_norm(vr_sub(h.t, fo.t), vr_v3(0, -1, 0)), 0.09f));
        else knuckles = vr_scale(knuckles, 1.0f / (float)count);
        vam_bind_hand_frame(b, knuckles, thumb);
        for (int i = 0; i < n_bones; ++i) {
            int under = 0;
            for (int k = i; k >= 0 && !under; k = arms.parent[k]) under = k == a->upper;
            if (under) arms.side_of[i] = (uint8_t)(side + 1);
        }
        a->ok = 1;
        fprintf(stderr, "VR arms: %s arm: UpperArm %d Forearm %d Hand %d Weapon %d twist %d/%d; upper %.3f m, forearm %.3f m, "
                        "shoulder (%.3f %.3f %.3f)\n",
                side ? "right" : "left", a->upper, a->fore, a->hand, a->weapon, a->twist, a->twist1, vam_upper_len(b),
                vam_fore_len(b), b->upper.x, b->upper.y, b->upper.z);
    }
    return arms.arm[0].ok && arms.arm[1].ok;
}

/* The player's pose (holder + 0x38), refreshed every 100 ms. */
static uintptr_t arms_player_pose(uint64_t now) {
    if (now - arms.pose_checked < 100000) return arms.pose;
    arms.pose_checked = now;
    arms.pose = arms.player = arms.bag = 0;
    uintptr_t wcm, player = 0, bag = 0, beh, holder = 0;
    uint64_t vt;
    const char *fail = NULL;
    if (!rd_ptr(va(VA_WORLD_CHR_MAN), &wcm) || !rd_ptr(wcm + 0x60, &player) || !arm_mem_ok(player, 8, 0)) fail = "no player";
    else {
        memcpy(&vt, (const void *)player, 8);
        if (vt != (uint64_t)va(VA_PLAYER_INS_VTABLE)) fail = "player: not a PlayerIns";
        else if (!rd_ptr(player + 0x3B0, &bag) || !rd_ptr(bag + 0x30, &beh) || !rd_ptr(beh + 0x28, &holder))
            fail = "player: no behaviour module / Havok holder";
    }
    if (fail) {
        if (fail != arms.pose_fail_logged) {
            arms.pose_fail_logged = fail;
            fprintf(stderr, "VR arms: player pose: %s (image 0x%llx)\n", fail, (unsigned long long)(uintptr_t)arms.image);
        }
        return 0;
    }
    arms.pose_fail_logged = NULL;
    arms.player = player;
    arms.bag = bag;
    arms.pose = holder + 0x38;
    if (arms.pose != arms.last_pose_logged) {
        arms.last_pose_logged = arms.pose;
        uintptr_t data = 0;
        int n = 0;
        rd_array(arms.pose + 0x18, &data, &n);
        fprintf(stderr, "VR arms: player 0x%llx pose 0x%llx (modelPose array 0x%llx, data 0x%llx, %d bones)\n",
                (unsigned long long)player, (unsigned long long)arms.pose, (unsigned long long)(arms.pose + 0x18),
                (unsigned long long)data, n);
    }
    return arms.pose;
}

/* The newest animation in the TimeAct ring ([bag + 0x18] + 0x20, 10 x 16 B, head +0xC0; findings/13
 * 3.4); -1 when unreadable. */
static int arms_current_anim(void) {
    uintptr_t tam;
    if (!arms.bag || !rd_ptr(arms.bag + 0x18, &tam) || !arm_mem_ok(tam + 0x20, 0xA4, 0)) return -1;
    uint32_t head;
    memcpy(&head, (const void *)(tam + 0xC0), 4);
    if (head >= 10) return -1;
    int32_t anim;
    memcpy(&anim, (const void *)(tam + 0x20 + 16 * ((head + 9) % 10)), 4);
    return anim;
}

/* ---- the per-frame pose edit ---- */

/* The controllers (bbgpu_vr_hands), refreshed once per edit; arms_xr_ok: a snapshot that is still
 * being updated (the pad poll runs every frame; time_ns unchanged for 250 ms = stale). */
static BbVrHands xr_hands;
static int arms_xr_ok;
static int64_t xr_seen_ns;
static uint64_t xr_seen_us;
static void arms_xr_refresh(uint64_t now) {
    BbVrHands h;
    arms_xr_ok = 0;
    if (!bbgpu_vr_hands(&h)) return;
    xr_hands = h;
    if (h.time_ns != xr_seen_ns) {
        xr_seen_ns = h.time_ns;
        xr_seen_us = now;
    }
    arms_xr_ok = now - xr_seen_us < 250000u;
}

/* The wrist target and the hand rotation (model space) for one side; 0 when this source has none. */
static int arms_target(int side, const VamArmBind *b, VrVec3 shoulder, float body_yaw, const ArmView *view,
                       int have_view, VrVec3 *wrist, VrQuat *hand_rot) {
    int src = arms.src;
    int xr_near = 1; /* a grip more than 1.5 m from the head is not a hand (seen: 93 m, both arms stretched) */
    if (src == SRC_XR) {
        const float *gp = xr_hands.grip[side].position, *hp = xr_hands.head.position;
        const float dx = gp[0] - hp[0], dy = gp[1] - hp[1], dz = gp[2] - hp[2];
        xr_near = dx * dx + dy * dy + dz * dz < 1.5f * 1.5f;
        if (!xr_near && !arms.xr_far_warned) {
            arms.xr_far_warned = 1;
            fprintf(stderr, "VR arms: the %s grip is %.1f m from the head: ignored (rest pose) while that lasts\n",
                    side ? "right" : "left", sqrtf(dx * dx + dy * dy + dz * dz));
        }
    }
    if (src == SRC_XR && !(have_view && arms_xr_ok && xr_near && (xr_hands.valid & 4u) && (xr_hands.valid & (side ? 2u : 1u)))) {
        if (!arms.xr_warned) {
            arms.xr_warned = 1;
            fprintf(stderr, "VR arms: arm_src=xr: no valid controller pose%s: using the rest pose until there is one\n",
                    have_view ? "" : " (no first-person camera)");
        }
        src = SRC_REST;
    }
    if (src == SRC_REST) {
        /* model space: the face looks along -Z, the R side is at -X (findings/13 2.2) */
        const VrQuat turn = vam_axis_angle(vr_v3(0, 1, 0), body_yaw); /* with the virtual upper body */
        const VrVec3 outward = vr_qrotate(turn, side ? vr_v3(-1, 0, 0) : vr_v3(1, 0, 0));
        VrVec3 fingers, thumb;
        *wrist = vam_rest_target(shoulder, outward, vr_v3(0, 1, 0), vr_qrotate(turn, vr_v3(0, 0, -1)), arms.rest[0], arms.rest[1],
                                 arms.rest[2], arms.rest[3] * VR_DEG, &fingers, &thumb);
        *hand_rot = vam_hand_rotation(vr_qidentity(), fingers, thumb, b->fingers, b->thumb, b->hand_rot);
        return 1;
    }
    /* fake: a grip relative to the head (OpenXR head frame), through the H1 camera */
    if (!have_view) return 0;
    const float *g = arms.fake[side];
    VrVec3 head_p = vr_v3(0, 0, 0), grip_p = vr_v3(g[0], g[1], g[2]), off_p, p;
    VrQuat head_q = vr_qidentity(), grip_q = vr_xr_ypr(g[3] * VR_DEG, g[4] * VR_DEG, g[5] * VR_DEG), off_q, q;
    if (src == SRC_XR) {
        /* the controller: grip and head of the same GetInput snapshot (one space), the hand is
         * then placed through the H1 camera exactly like the head */
        const BbVrPose *gp = &xr_hands.grip[side], *hd = &xr_hands.head;
        head_p = vr_v3(hd->position[0], hd->position[1], hd->position[2]);
        head_q = vr_qnormalize(vr_q(hd->orientation[0], hd->orientation[1], hd->orientation[2], hd->orientation[3]));
        grip_p = vr_v3(gp->position[0], gp->position[1], gp->position[2]);
        grip_q = vr_qnormalize(vr_q(gp->orientation[0], gp->orientation[1], gp->orientation[2], gp->orientation[3]));
    }
    vam_side_offset(side, vr_v3(arms.offset[0], arms.offset[1], arms.offset[2]),
                    vam_axis_angle(vr_v3(1, 0, 0), arms.offset[3] * VR_DEG), &off_p, &off_q);
    /* aim trim in the controller frame (OpenXR: +Y up, -Z forward): yaw right is -Y, pitch up +X */
    off_q = vr_qnormalize(vr_qmul(vr_qmul(vam_axis_angle(vr_v3(0, 1, 0), -arms.rot[side][0] * VR_DEG),
                                          vam_axis_angle(vr_v3(1, 0, 0), arms.rot[side][1] * VR_DEG)), off_q));
    vam_grip_to_model(head_p, head_q, grip_p, grip_q, off_p, off_q, view->cam_pos, view->cam_rot, &p, &q);
    *wrist = p;
    /* the game's grip frame: OpenXR -Z (fingers) mirrored to +Z, the thumb +Y */
    *hand_rot = vam_hand_rotation(q, vr_v3(0, 0, 1), vr_v3(0, 1, 0), b->fingers, b->thumb, b->hand_rot);
    return 1;
}

static VrQuat q_blend(VrQuat from, VrQuat to, float w) { return w >= 1.0f ? to : vr_qnlerp(from, to, w); }

/* The clavicle of `side` at its camera-frame pin (blended in by the detach amount) and everything that hangs
 * off it outside the arm subtree (shoulder pads, cloth helpers) moved by the same rigid step. Skipped when the
 * parent of the UpperArm is not the clavicle bone or is shared by both arms. */
static void arms_roll_clavicle(int side, const ArmView *view, VrXform *old, VrXform *neu, uint8_t *ok, uint8_t *dirty,
                               uintptr_t model, int n_model) {
    const ArmBones *a = &arms.arm[side];
    const int cp = arms.parent[a->upper];
    if (cp < 0 || cp != a->clavicle || !ok[cp] || cp == arms.parent[arms.arm[1 - side].upper]) return;
    const VrXform pin = vr_roll_from_cam(view->cam_pos, view->cam_rot, arms.roll.clav[side]);
    const VrXform now = vr_roll_blend(old[cp], pin, arms.roll.e);
    const VrXform delta = vr_xf_mul(now, vr_xf_inverse(old[cp]));
    static _Thread_local uint8_t under[ARM_MAX_BONES];
    neu[cp] = now;
    dirty[cp] = 1;
    for (int i = 0; i < n_model; ++i) {
        const int p = arms.parent[i];
        under[i] = i > cp && p >= 0 && (p == cp || under[p]);
        if (!under[i] || arms.side_of[i]) continue;
        if (!vr_xf_from_qs((const float *)(model + (uintptr_t)i * 48), &old[i])) continue;
        neu[i] = vr_xf_mul(delta, old[i]);
        dirty[i] = 1;
    }
}

static void arms_edit(uintptr_t pose, int which, uint64_t now) {
    float dt = arms.last_us ? (float)(now - arms.last_us) * 1e-6f : 0.0f;
    arms.last_us = now;
    if (dt > 0.1f) dt = 0.1f;

    ArmView view;
    const int have_view = arm_view_read(&view) && now - view.time_us < ARM_VIEW_STALE_US;
    if (have_view) { /* a dodge under face=view: the body is at the game's live yaw, not the snapshot's */
        float live;
        if (vr_fp_yaw_live(&live) && fabsf(live - view.yaw) > 1e-4f) {
            const VrQuat d = vr_qaxis(vr_v3(0, 1, 0), view.yaw - live);
            view.cam_pos = vr_qrotate(d, view.cam_pos);
            view.cam_rot = vr_qnormalize(vr_qmul(d, view.cam_rot));
            view.shift = vr_qrotate(d, view.shift);
            view.rel_vel = vr_qrotate(d, view.rel_vel);
            view.yaw = live;
        }
    }
    const int fp_now = have_view && vr_fp_active();
    arms.anim = arms_current_anim();
    const int sub = arms.anim >= 0 ? arms.anim % 1000000 : -1;
    arms.attack = sub >= 30000 && sub < 50000;
    if (arms.src == SRC_XR) arms_xr_refresh(now);
    float target = arms.on && (fp_now || arms.tp) ? arms.weight_max : 0.0f;
    if (arms.attack) target = fminf(target, arms.attack_weight);
    if (arms.src == SRC_FAKE && !have_view) target = 0.0f;
    if (!fp_now) ++arms.stale_view;
    arms.w = vam_weight_step(arms.w, target, dt, arms.in_s, arms.out_s);
    if (!arms.first_edit) {
        arms.first_edit = 1;
        fprintf(stderr, "VR arms: first player pose at %s: src %s, arm %d, view %d, first person %d, target weight %.2f%s\n",
                site_names[which], src_names[arms.src], arms.on, have_view, fp_now, target,
                target <= 0.0f ? " (nothing written until it rises: arm=1 and first person, or arm_tp=1)" : "");
    }
    /* an evade clip: the arms leave the body (findings/24 section 8) */
    const int evading = arms.roll_detach && fp_now && vr_evade_move(arms.anim) != NULL;
    const int roll_cap = vr_roll_capturing(&arms.roll, evading);
    vr_roll_step(&arms.roll, evading, dt, VR_ROLL_BLEND_S);
    /* the view is one render old: in an evade the camera moves against the body, so advance it by that (vr_arms_roll.h) */
    if (have_view && arms.roll.e > 0.0f) view.cam_pos = vr_roll_lead(view.cam_pos, view.rel_vel, dt, arms.roll.e);
    if (evading != arms.roll_was) {
        arms.roll_was = evading;
        if (arms.roll_log)
            fprintf(stderr, "VR arms: %s the arms %s the body (anim a%03d_%06d, pins L %d R %d)\n", evading ? "detach" : "attach",
                    evading ? "from" : "to", arms.anim / 1000000, arms.anim % 1000000, arms.roll.has[0], arms.roll.has[1]);
    }
    if (arms.w <= 0.0f) return;

    uintptr_t skel;
    if (!rd_ptr(pose, &skel) || !arms_skeleton(skel)) return;
    uintptr_t model, flags = 0;
    int n_model, n_flags = 0;
    if (!rd_array(pose + 0x18, &model, &n_model) || n_model != arms.n_bones ||
        !arm_mem_ok(model, (size_t)n_model * 48, 1))
        return;
    if (!rd_array(pose + 0x28, &flags, &n_flags) || n_flags != n_model || !arm_mem_ok(flags, (size_t)n_flags * 4, 1))
        n_flags = 0;

    static _Thread_local VrXform old[ARM_MAX_BONES], neu[ARM_MAX_BONES];
    static _Thread_local uint8_t ok[ARM_MAX_BONES], dirty[ARM_MAX_BONES];
    for (int i = 0; i < n_model; ++i) {
        ok[i] = arms.side_of[i] ? (uint8_t)vr_xf_from_qs((const float *)(model + (uintptr_t)i * 48), &old[i]) : 0;
        dirty[i] = 0;
    }
    /* the parents of the arm roots are needed too (the clavicles) */
    for (int side = 0; side < 2; ++side) {
        const int p = arms.parent[arms.arm[side].upper];
        if (p >= 0) ok[p] = (uint8_t)vr_xf_from_qs((const float *)(model + (uintptr_t)p * 48), &old[p]);
    }

    /* The virtual upper body (findings/24 5.4): the shoulders turn about the vertical through their
     * midpoint toward the head's yaw (the H1 camera in model space), past a deadzone, smoothed. The
     * character itself (its heading, movement, the game's pose of the rest of the body) is untouched:
     * only the arm chains are rooted at the turned shoulders. */
    float body_yaw = 0.0f;
    VrVec3 pivot = vr_v3(0, 0, 0);
    /* A comfort dash (findings/19 follow mode) draws the view behind the real eye for ~0.3 s; the hands
     * are placed from that view, so the shoulders move by the same offset (findings/24 5.5): the arms
     * stay where they are relative to the view instead of reaching from the body ahead of it. */
    const VrVec3 shift = have_view ? view.shift : vr_v3(0, 0, 0);
    if (ok[arms.arm[0].upper] && ok[arms.arm[1].upper]) {
        pivot = vam_lerp(old[arms.arm[0].upper].t, old[arms.arm[1].upper].t, 0.5f);
        /* the shoulders' heading lives in the world and follows the head only: the character turning
         * to face its movement (walking backwards spins it 180 deg) does not take the arms with it */
        if (have_view && arms.body_on) {
            const float head = vam_yaw_between(vr_v3(0, 0, -1), vr_qrotate(view.cam_rot, vr_v3(0, 0, 1)));
            arms.body_off = vam_shoulder_yaw(&arms.shoulder_yaw, view.yaw, head, arms.body_dz * VR_DEG, arms.body_tau, dt);
        } else {
            arms.shoulder_yaw.valid = 0;
            arms.body_off = 0.0f;
        }
        body_yaw = arms.body_off;
    }
    for (int side = 0; side < 2; ++side) {
        const ArmBones *a = &arms.arm[side];
        if (!ok[a->upper] || !ok[a->fore] || !ok[a->hand]) continue;
        {   /* already ours (a second getSynced call in the same frame): leave it */
            const float *u = (const float *)(model + (uintptr_t)a->upper * 48 + 16);
            if (!memcmp(u, arms.last_upper[side], sizeof(arms.last_upper[side]))) continue;
        }
        VrVec3 shoulder = vr_add(body_yaw != 0.0f ? vam_turn_about(old[a->upper].t, pivot, body_yaw) : old[a->upper].t, shift);
        if (have_view && arms.roll_detach) {
            const int cp = arms.parent[a->upper];
            if (roll_cap) { /* the last frame before an evade: the shoulder and the clavicle in the camera frame */
                arms.roll.shoulder[side] = vr_roll_to_cam(view.cam_pos, view.cam_rot, vr_xf(shoulder, vr_qidentity()));
                arms.roll.has[side] = 1;
                if (cp >= 0 && ok[cp]) arms.roll.clav[side] = vr_roll_to_cam(view.cam_pos, view.cam_rot, old[cp]);
                else arms.roll.has[side] = 0;
            } else if (arms.roll.e > 0.0f && arms.roll.has[side]) {
                const VrXform pin = vr_roll_from_cam(view.cam_pos, view.cam_rot, arms.roll.shoulder[side]);
                shoulder = vam_lerp(shoulder, pin.t, arms.roll.e);
                /* the clavicle follows its pin, and what hangs off it (shoulder pads, cloth) moves with it */
                arms_roll_clavicle(side, &view, old, neu, ok, dirty, model, n_model);
            }
        }
        VrVec3 wrist;
        VrQuat hand_rot;
        if (!arms_target(side, &a->bind, shoulder, body_yaw, &view, have_view, &wrist, &hand_rot)) continue;
        VamArmParams params;
        memset(&params, 0, sizeof(params));
        params.pole = vr_v3(side ? -arms.pole[0] : arms.pole[0], -arms.pole[1], arms.pole[2]); /* out, down, back (+Z) */
        params.pole = vr_qrotate(vam_axis_angle(vr_v3(0, 1, 0), body_yaw), vam_norm(params.pole, vr_v3(0, -1, 0)));
        params.arm_scale = 1.0f;
        params.twist_share = arms.twist_share;
        params.soft = arms.soft;
        params.give = arms.give;
        params.fore_roll = arms.fore_roll;
        params.roll_prev = arms.roll_prev[side];
        params.has_roll_prev = arms.has_roll[side];
        VamArmPose ik;
        vam_solve_arm(&a->bind, shoulder, wrist, hand_rot, &params, &ik);
        arms.reach[side] = ik.reach;
        arms.roll_prev[side] = ik.roll;
        arms.has_roll[side] = 1;
        arms.dbg[side] = ik;
        arms.dbg_upper[side] = vam_upper_len(&a->bind);
        arms.dbg_fore[side] = vam_fore_len(&a->bind);
        arms.clamped[side] = ik.clamped;
        /* the chain's new rotations (blended with the animation), positions rebuilt from the
         * animated locals down the hierarchy (parents first) */
        const float w = arms.w;
        for (int i = a->upper; i < n_model; ++i) {
            if (arms.side_of[i] != side + 1 || !ok[i]) continue;
            const int p = arms.parent[i];
            if (i == a->upper) {
                neu[i] = vr_xf(vam_lerp(old[i].t, ik.shoulder, w), q_blend(old[i].q, ik.upper_rot, w));
            } else {
                if (p < 0 || !ok[p]) { ok[i] = 0; continue; }
                const VrXform local = vr_xf_mul(vr_xf_inverse(old[p]), old[i]);
                VrXform x = vr_xf_mul(neu[p], local);
                if (i == a->fore) x.q = q_blend(old[i].q, ik.fore_rot, w);
                else if (i == a->hand) x.q = q_blend(old[i].q, ik.hand_rot, w);
                else if (i == a->twist && a->bind.has_twist && arms.twist_share > 0.0f) x.q = q_blend(old[i].q, ik.twist_rot, w);
                else if (i == a->twist1 && a->bind.has_twist1 && arms.twist_share > 0.0f) x.q = q_blend(old[i].q, ik.twist1_rot, w);
                neu[i] = x;
            }
            dirty[i] = 1;
        }
    }

    int written = 0;
    for (int i = 0; i < n_model; ++i) {
        if (!dirty[i]) continue;
        const VrXform x = neu[i];
        if (!isfinite(x.t.x) || !isfinite(x.t.y) || !isfinite(x.t.z) || !vr_qvalid(x.q)) continue;
        float *qs = (float *)(model + (uintptr_t)i * 48);
        qs[0] = x.t.x; qs[1] = x.t.y; qs[2] = x.t.z;              /* t.w and the scale stay */
        qs[4] = x.q.x; qs[5] = x.q.y; qs[6] = x.q.z; qs[7] = x.q.w;
        if (n_flags) ((uint32_t *)flags)[i] |= 1u;                /* local dirty */
        ++written;
    }
    for (int side = 0; side < 2; ++side)
        if (dirty[arms.arm[side].upper])
            memcpy(arms.last_upper[side], (const float *)(model + (uintptr_t)arms.arm[side].upper * 48 + 16),
                   sizeof(arms.last_upper[side]));
    if (written) {
        if (arm_mem_ok(pose + 0x39, 1, 1)) *(volatile uint8_t *)(pose + 0x39) = 0; /* localInSync */
        ++arms.writes;
    }
}

/* One hook call: `model_array` is the pose's modelPose hkArray (pose + 0x18). */
static void arms_hook(BbHookContext *ctx, int which, uint32_t site, uintptr_t model_array) {
    arms_init();
    if (!arms.enabled) return;
    /* Fast path: not the player's pose (a fresh lookup says so): no lock, nothing shared touched. */
    const uint64_t t0 = arm_now_us();
    const uintptr_t pub = atomic_load_explicit(&arms_pose_pub, memory_order_acquire);
    if (arms.first_call[which] && pub && model_array != pub + 0x18 &&
        t0 - atomic_load_explicit(&arms_pose_pub_us, memory_order_relaxed) < 100000) {
        atomic_fetch_add_explicit(&arms_fast_other[which], 1, memory_order_relaxed);
        return;
    }
    /* The write site waits for the lock (a skipped write shows the game's arms for a whole frame:
     * the "double arms" of alternate-eye stereo, findings/24 5.3); the diagnostic sites skip. */
    const int at = arms.at;
    const int edit_site = at == AT_PLAYER ? which == 4 : at == AT_SYNC ? which == 3 : which <= 2;
    if (edit_site) {
        const uint64_t prof_t0 = bb_vrprof_t0();
        arms_lock_spin();
        bb_vrprof_end(BB_PROF_ARMS_LOCK, prof_t0);
    }
    else {
        int expected = 0;
        if (!atomic_compare_exchange_strong(&arms_busy, &expected, 1)) { ++arms.skipped_busy; return; }
    }
    if (!arms.image) arms.image = (unsigned char *)(uintptr_t)(ctx->rip - (site - VA_BASE));
    ++arms.fired[which];
    const uint64_t now = arm_now_us();
    arms_poll_control(now); /* also without a player: arm_log=1 and the rest work at once */
    const uintptr_t pose = arms_player_pose(now);
    if (now - arms.pose_checked < 1000) { /* looked up just now: publish for the fast path */
        atomic_store_explicit(&arms_pose_pub_us, now, memory_order_relaxed);
        atomic_store_explicit(&arms_pose_pub, pose, memory_order_release);
    }
    const int match = pose && model_array == pose + 0x18;
    if (!arms.first_call[which]) {
        arms.first_call[which] = 1;
        fprintf(stderr, "VR arms: first call at %s (0x%x): modelPose 0x%llx, player pose 0x%llx -> %s\n", site_names[which],
                site, (unsigned long long)model_array, (unsigned long long)pose, match ? "the player" : "another pose");
    }
    if (match) {
        ++arms.player_fired[which];
        const int edit_here = arms.at == AT_PLAYER ? which == 4 : arms.at == AT_SYNC ? which == 3 : which <= 2;
        if (!arms.first_match[which]) {
            arms.first_match[which] = 1;
            fprintf(stderr, "VR arms: the player's pose reaches %s (0x%x)%s\n", site_names[which], site,
                    edit_here ? ": arms are written here" : " (not the edit site: arm_at=)");
        }
        if (which == 3) { /* the caller of getSyncedPoseModelSpace */
            uint64_t ret = 0;
            if (arm_mem_ok((uintptr_t)ctx->rsp + 0x18, 8, 0)) memcpy(&ret, (const void *)((uintptr_t)ctx->rsp + 0x18), 8);
            const uint64_t ret_va = ret - (uint64_t)(uintptr_t)arms.image + VA_BASE;
            int k = 0;
            while (k < arms.n_callers && arms.callers[k] != ret_va) ++k;
            if (k == arms.n_callers && k < ARM_CALLERS) {
                arms.callers[arms.n_callers++] = ret_va;
                fprintf(stderr, "VR arms: the player's pose is synced from 0x%llx (call at 0x%llx)\n",
                        (unsigned long long)ret_va, (unsigned long long)(ret_va - 5));
            }
            if (k < ARM_CALLERS) ++arms.caller_hits[k];
        }
        if (edit_here) {
            const uint64_t prof_t0 = bb_vrprof_t0();
            arms_edit(pose, which, now);
            bb_vrprof_end(BB_PROF_ARMS_EDIT, prof_t0);
        }
    }
    if ((arms.log || arms.diag_lines < 20) && now >= arms.next_log) {
        if (!arms.log) ++arms.diag_lines;
        arms.next_log = now + 1000000;
        uint32_t f[ARM_SITES];
        for (int i = 0; i < ARM_SITES; ++i) f[i] = arms.fired[i] + atomic_exchange_explicit(&arms_fast_other[i], 0, memory_order_relaxed);
        fprintf(stderr, "VR arms: A1 %u (player %u) A2 %u (%u) A3 %u (%u) sync %u (%u) player-site %u (%u) | at %s | w %.2f src %s "
                        "anim %d%s | reach L %.2f%s R %.2f%s | writes %u busy-skips %u (diagnostic sites only) waits %u no-fp %u\n",
                f[0], arms.player_fired[0], f[1], arms.player_fired[1], f[2], arms.player_fired[2],
                f[3], arms.player_fired[3], f[4], arms.player_fired[4], at_names[arms.at], arms.w,
                src_names[arms.src], arms.anim,
                arms.attack ? " (attack)" : "", arms.reach[0], arms.clamped[0] ? " clamped" : "", arms.reach[1],
                arms.clamped[1] ? " clamped" : "", arms.writes, arms.skipped_busy,
                atomic_exchange_explicit(&arms_spins, 0, memory_order_relaxed), arms.stale_view);
        for (int side = 0; side < 2; ++side) {
            const VamArmPose *d = &arms.dbg[side];
            fprintf(stderr, "VR arms: ik %s: upper %.3f fore %.3f m | target %.3f m (reach %.2f) -> ik %.3f (eased %.0f%%, shoulder give %.3f m%s) | "
                            "elbow %.0f deg | roll %.0f deg (fore %.0f%% twist %.0f%%, twist1 half)\n",
                    side ? "R" : "L", arms.dbg_upper[side], arms.dbg_fore[side], d->dist, d->reach, d->dist * d->eased, d->eased * 100.0f,
                    d->give, d->clamped ? ", clamped" : "", d->elbow_deg, d->roll * (180.0f / VR_PI), arms.fore_roll * 100.0f,
                    (1.0f - arms.fore_roll) * arms.twist_share * 100.0f);
        }
        if (arms.n_callers) {
            fprintf(stderr, "VR arms:   player syncs per caller:");
            for (int k = 0; k < arms.n_callers; ++k)
                fprintf(stderr, " 0x%llx:%u", (unsigned long long)arms.callers[k], arms.caller_hits[k]);
            fprintf(stderr, "\n");
        }
        for (int i = 0; i < ARM_SITES; ++i) arms.fired[i] = arms.player_fired[i] = 0;
        for (int k = 0; k < arms.n_callers; ++k) arms.caller_hits[k] = 0;
        arms.writes = arms.skipped_busy = arms.stale_view = 0;
    }
    atomic_store_explicit(&arms_busy, 0, memory_order_release);
}

static ABI void arms_hook_a1(BbHookContext *ctx) { arms_hook(ctx, 0, VA_SITE_A1, (uintptr_t)ctx->rax); }
static ABI void arms_hook_a2(BbHookContext *ctx) { arms_hook(ctx, 1, VA_SITE_A2, (uintptr_t)ctx->rax); }
static ABI void arms_hook_a3(BbHookContext *ctx) { arms_hook(ctx, 2, VA_SITE_A3, (uintptr_t)ctx->rax); }
static ABI void arms_hook_sync(BbHookContext *ctx) { arms_hook(ctx, 3, VA_SITE_SYNC, (uintptr_t)ctx->rbx + 0x18); }
/* r14 is the pose only on the path through the sync (the two skips to this site leave the caller's
 * r14): it is only compared with the player's pose. */
static ABI void arms_hook_player(BbHookContext *ctx) { arms_hook(ctx, 4, VA_SITE_PLAYER, (uintptr_t)ctx->r14 + 0x18); }

void vr_arms_register(void) {
    arms_init();
    bb_hook_set_arms_impl(arms_hook_a1, arms_hook_a2, arms_hook_a3, arms_hook_sync, arms_hook_player);
}
