/* First-person camera for VR (BB_VR_FP=1): the render camera at the hunter's eyes instead of the
 * third-person follow camera. Design: docs/vr/findings/11 (FP-1/FP-2, C-1); addresses, evidence
 * and limits: docs/vr/findings/16-first-person.md. Pure math: src/vr_fp_math.h and the turn model
 * the VR sandbox shares, src/vr_turn_math.h (tests/test_vr_head.c, tests/test_vr_turn.c). Called
 * from the H1/H2 camera hooks (src/runtime_vr_head.c).
 *
 * Where the data comes from (PS4 VAs; the image maps VA 0x400000 at the base H1's rip implies):
 *   WorldChrMan  [0x593E878]          +0x60 = the main player (PlayerIns, vtable 0x578F310): the
 *                                     character the in-game step hands the camera director
 *   bag          [P + 0x3B0]          module bag
 *   physics      [bag + 0x68]         +0x1E0 position (vec4, the feet), +0x1D4 yaw: the heading of
 *                                     model +Z; the hunter faces yaw + pi (measured)
 *   pose         [[bag + 0x30] + 0x28] + 0x38   hkaPose (Havok 2014): skeleton +0x00, local
 *                                     +0x08, model +0x18, bone flags +0x28 (hkArray); skeleton:
 *                                     parents +0x18, bones +0x28, reference pose +0x38
 *   SprjCamera   [0x593E8F8]          +0x10 the field (gameplay) camera: first person only while
 *                                     it is this frame's camera (else a cutscene or debug camera)
 *   ChrCam       [step + 0x128]       the game camera the director just built (in-game step
 *                                     0x1d3ac10, r13 = step): hook "bb_hook_vr_fp_cam" at 0x1d3be5a
 *   models       [P + 0x48] the base body (c0000); [P + 0x4D0] + 0x28 + 8 i, i < 24, the ChrAsm
 *                parts (armour, weapons, ...). Each model's display entity [model + 0x18]
 *                (SprjFD4ModelDispEntity, vtable 0x57A0820) is drawn while its flags +0x210 have
 *                bit 0: the PlayerIns visibility update (vtable +0x4D0, 0x1cf4830) sets it every
 *                frame, SprjFD4ModelDispEntity::IsVisible (0x2121c80) tests it for the draw lists
 *   LockTgtMan   [0x593E860]          +0x2820 the player is locked on (byte); the target list at
 *                                     +8 (next +0x80, flags +0x90: 0x20 the locked one, the
 *                                     target point +0x00)
 * The eye: the midpoint of dummy 58 "Dummy_eye" (bind pose model space (0, 1.654, -0.066),
 * findings/11 2.2) carried by the live Head bone (78): its offset in the Head bone's frame comes
 * from the skeleton's reference pose, the live eye is model(Head) . offset, then the physics
 * position and yaw take it to the world.
 *
 * Heading: the view's yaw is psi + the headset's yaw (vr_turn_math.h): psi turns only with the
 * right stick (snap or smooth turn) or a recenter, never with the character, its root motion, its
 * camera or lock-on. The game must move along the chosen heading (psi, or psi + the head's yaw):
 * the follow camera rebuilds its yaw from its own state every frame (writing +0x144 or its target
 * +0x270 does not stick, findings/16 3), so the "cam" hook turns its output instead: ChrCam's
 * matrix, about the vertical through the player, before the in-game step copies it to the render
 * and field cameras. Without the hook the left stick is rotated instead.
 *
 * Environment (all off unless BB_VR_FP=1; needs the H1 hook, see findings/16):
 *   BB_VR_FP=1             first person while the field camera is active
 *   BB_VR_FP_LOG=1         once per second: the player, the eye, the camera, the heading; also the
 *                          per-frame trace below (out/vr_fp_trace.csv unless BB_VR_FP_TRACE says)
 *   BB_VR_FP_ANCHOR=head|body|raw|stable   head: the eye, smoothed in the character's frame
 *                          (default); body: a fixed eye above the feet (no head bob); raw:
 *                          unsmoothed; stable: the head's height straight above the feet (no
 *                          lean, no swing when the character turns: with hide=body)
 *   BB_VR_FP_SMOOTH=<s>    smoothing time constant (default 0.12 s; kept within 8 cm of the eye)
 *   BB_VR_FP_FLOOR=<f>     the eye stays at least f x the standing eye height above the feet
 *                          (rolls, quicksteps, knockdowns; default 0.75, 0 = off)
 *   BB_VR_FP_NEAR=<m>      near plane in first person (default 0.2 m; 0 = the game's 0.05)
 *   BB_VR_FP_HIDE=<what>   the hunter's own model hidden in first person (findings/16 4): none
 *                          (default), head (face, hair, hat), body (all but the arms, hands and
 *                          weapons), all, or parts (fp_hide_groups, sN, +)
 *   BB_VR_FP_FORWARD=<m>   the camera this far in front of the eye along the view (default 0)
 *   BB_VR_FP_UP=<m>        the camera this far above the eye (default 0, -0.3..0.3)
 *   BB_VR_FP_DOWN=<s>      third person after the eye stayed below 0.6 m for this long (death,
 *                          knockdown; default 1.5, 0 = never)
 *   BB_VR_TURN=snap|smooth|off|game  the right stick in first person (default snap with
 *                          BB_VR_HEAD=1, else game = the game's own camera turning)
 *   BB_VR_SNAP_DEG=<deg>   snap angle (30; 0 = game), BB_VR_TURN_SPEED=<deg/s> smooth (90)
 *   BB_VR_FP_MOVE=body|head|controller  the left stick moves along the view's body heading
 *                          (default), where the head looks, or the controller (falls back to the
 *                          head: the game has no controller pose yet)
 *   BB_VR_FP_FACE=off|view|head|controller|attack  the body is glued to the view (default off): each H1 frame
 *                          the physics yaw (+0x1D4) is set to the view heading (psi + the headset's
 *                          yaw) - 180 deg, so the hunter faces where you look, attacks and rolls
 *                          included (written after the game's own turn logic of that step, before
 *                          the next step reads it). head = view (the view IS psi + headset yaw);
 *                          controller falls back to view (no controller pose in this code path).
 *                          attack: the facing is left to the game (the left stick walks and turns the
 *                          body as usual) and is set to the view only while an attack or gun shot clip is
 *                          the newest TimeAct animation (anim % 1000000 in 30000..51999), from the very
 *                          frame the clip starts (the ring is pushed in the step that starts it, H1 sees it
 *                          the same frame, the first write is not delayed: the next step swings along the
 *                          view). Dodges follow face_dodge (game: the stick; view/strafe as above). The
 *                          arms read the game's live yaw whenever we do not write.
 *                          Not applied while locked on (the game faces the target) unless
 *                          BB_VR_FP_FACE_LOCKON=1. Control file: face= face_lockon=0|1
 *                          While a dodge plays (rolls, quicksteps, backsteps: a000_7000-7799) the
 *                          facing is left to the game, so the roll goes where the left stick points
 *                          (backwards included); afterwards the body eases back to the view over
 *                          0.15 s. BB_VR_FP_FACE_DODGE=view|strafe / control file face_dodge=game|view|strafe
 *                          (default game; view keeps the dodge facing the view too, so the roll goes
 *                          where you look; strafe also faces the view but the roll travels along the
 *                          left stick, captured at the dodge start (neutral: straight back from the
 *                          view): the position delta the game's step made is rotated onto it in H1).
 *                          The yaw is written one frame ahead (the arms' IK is computed with the
 *                          yaw the next frame is rendered with, so the hands do not swing by the
 *                          head's per-frame turn: that was the double image with alt-eye stereo).
 *   BB_VR_FP_SYNC=0|1      turn the game camera to that heading (default 1; needs the hook
 *                          "VR FP Camera Heading (in-game step)")
 *   BB_VR_FP_REMAP=0|1     rotate the left stick by what the game camera still lacks (default: the
 *                          residual with the hook, all of it without; 0 = never; it rotates menu
 *                          navigation too)
 *   BB_VR_FP_CONTROL=<file>  settings re-read while the game runs (fp_poll_control)
 *   BB_VR_FP_TRACE=<file>  one CSV line per frame (findings/16 9.6; tools/vr_fp_test.py)
 */
#define _GNU_SOURCE
#include "runtime.h"
#include "hooks.h"
#include "vr_fp.h"
#include "vr_fp_math.h"
#include "vr_dash_game.h"
#include "vr_dash_detect.h"
#include "vr_turn_math.h"
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

#define VA_BASE 0x400000u
#define VA_H1_SITE 0x22de113u          /* findings/08 H1: the hook's rip gives the image base */
#define VA_WORLD_CHR_MAN 0x593E878u
#define VA_LOCK_TGT_MAN 0x593E860u     /* LockTgtMan (constructor 0x1c8d8f0, debug "IsLockMode:") */
#define VA_SPRJ_CAMERA 0x593E8F8u
#define VA_PLAYER_INS_VTABLE 0x578F310u /* findings/13 3.1 */
#define VA_DISP_ENTITY_VTABLE 0x57A0820u /* SprjFD4ModelDispEntity (constructor 0x2120bc4) */

#define FP_EYE_BIND_Y 1.654f   /* dummy 58 midpoint, model space bind pose (findings/11 2.2) */
#define FP_EYE_BIND_Z (-0.066f)
#define FP_MAX_DEV 0.08f       /* smoothing keeps within 8 cm of the raw eye (erfps2) */
#define FP_DOWN_HEIGHT 0.6f
#define FP_ASM_SLOTS 24        /* ChrAsm part slots at [P + 0x4D0] + 0x28 (findings/16 4) */
#define FP_HIDE_SLOTS 0x00FFFFFFu
#define FP_HIDE_BASE (1u << 24) /* the base body model [P + 0x48] */
#define FP_HIDE_SKIN_HEAD (1u << 25) /* the skin's (slot 1) head and neck: display mask groups 41 47 48 49 */
#define FP_HIDE_SKIN_BODY (1u << 26) /* the skin's torso, legs, feet: groups 34 42 45 46 50 61 */
#define FP_HIDE_ALL_BITS (FP_HIDE_SLOTS | FP_HIDE_BASE | FP_HIDE_SKIN_HEAD | FP_HIDE_SKIN_BODY)
#define FP_TRACE_MAX (100u << 20)

enum { FP_ANCHOR_HEAD = 0, FP_ANCHOR_BODY = 1, FP_ANCHOR_RAW = 2, FP_ANCHOR_STABLE = 3 };

static struct {
    int ready, enabled, log, anchor, sync, remap;
    int face, face_lockon; /* body glued to the view: 0 off, 1 view, 2 head, 3 controller (-> view), 4 attack */
    float face_last;
    /* face: pending = the yaw the next frame is rendered with (the arms' snapshot is computed with it) */
    int face_pending_ok;
    float face_pending;
    int face_dodge;        /* 0 game: the dodge keeps the game's own facing; 1 view: faced to the view too (the roll goes where
                            * you look); 2 strafe: faced to the view, the roll travels along the left stick */
    /* face_dodge=strafe: the travel direction captured at the dodge start, the root motion rotated onto it */
    int dodge_fresh;       /* the dodge started this frame: capture the direction */
    int dodge_roll;        /* the dodge is a roll (the only kind redirected) */
    float dodge_tdeg;      /* the table's direction of the clip (deg from the facing) */
    float dodge_travel;    /* captured travel heading (rad) */
    int strafe_on;         /* dodge_travel is valid for the running dodge */
    float strafe_base[2];  /* x,z the previous frame ended with (what we wrote) */
    float strafe_game[2];  /* x,z the game's step produced last frame (before our rewrite) */
    int strafe_have, strafe_frames, strafe_kept;
    float strafe_dash;     /* heading for the dash: travel - table direction (NAN: none) */
    /* dodge tracker (the TimeAct ring's newest clip, a000_7xxx) */
    int dodge_seen, dodge_anim, dodge_on, dodge_live;
    int attack_on;         /* face=attack: an attack/shot clip is the newest TimeAct animation */
    float dodge_f[3];
    uint64_t dodge_start_us;
    float dodge_len;
    float ease_off;        /* radians still to blend away after a dodge ends (game yaw - view yaw) */
    uint64_t next_flip_log;
    uintptr_t phys_now;    /* the physics module of this H1 (vr_fp_yaw_live) */
    float face_dev;        /* |game yaw - our last write| seen before the next write (does it stick?) */
    float face_dev_max;
    int face_writes;
    float smooth, floor_frac, near_plane, forward, up, down_s;
    unsigned char *image; /* guest VA 0x400000 */
    uint64_t start, last_h1, next_log;
    /* the player's skeleton (bones by name, once per skeleton) */
    uint64_t skel;
    int head_bone, neck_bone;
    VrVec3 eye_offset;     /* the eye in the Head bone's frame (bind pose) */
    int eye_offset_ok;
    float eye_height;      /* standing eye height, model space (bind pose) */
    VrSmooth smooth_state;
    /* anim_lock: the camera does not follow the character's animation (comfort, test option) */
    int anim_lock;         /* 0 off, 1 fixed eye on the root, 2 also eases root-motion bursts */
    float anim_smooth;     /* mode 2: smooth time (s) of the follow during a burst */
    float anim_speed;      /* mode 2: root speed (m/s) above which a burst is eased */
    struct { int valid; float x, z, vx, vz, px, pz, w; } anim_state;
    float down_time;
    int was_active;
    uint64_t inactive_since; /* first person off since (a re-entry later than 0.5 s restarts psi) */
    float psi;             /* first-person body heading (radians, vr_cam_heading's sense) */
    /* the turn model (src/vr_turn_math.h, shared with the VR sandbox) */
    VrTurnConfig turn;
    VrTurnState turn_state;
    VrTurnOutput turn_out;
    int turn_game;         /* turn=game: psi follows the game camera (its own right-stick turning) */
    float head_yaw, ref_yaw;
    int locked, target_valid;
    float target_yaw;
    /* the hunter's own model */
    uint32_t hide;         /* FP_HIDE_* / slot bits hidden while in first person */
    int hide_tp;           /* also in third person (diagnostics: see what a part is) */
    uint8_t hide_bits;     /* the display flags cleared (+0x210; 1 = drawn; diagnostics: hide_bits=) */
    int hidden;            /* display entities hidden this frame */
    uintptr_t parts_dumped;/* the parts holder last listed in the log */
    uint32_t dump_hide;    /* the hide mask when it was listed */
    const char *state;     /* why first person is off (logs) */
    int on;                /* the control file's fp=0|1 (default 1) */
    const char *control;   /* BB_VR_FP_CONTROL */
    uint64_t control_checked, control_mtime, control_size;
    FILE *trace;           /* BB_VR_FP_TRACE */
    uint64_t trace_bytes;
    int body_ok;           /* this H1 read the player: body_pos, body_yaw (vr_fp_body) */
    VrVec3 dash_offset;    /* the dash's camera minus the eye, this H1 (vr_fp_dash_offset) */
    VrVec3 body_pos;
    float body_yaw;
} fp;

/* Shared with the pad sampler and the in-game step's hook (other threads). */
static _Atomic uint64_t fp_active_until_us;   /* the pad filter acts until then */
static _Atomic uint32_t fp_pad_bits[3];       /* floats: right stick X, left stick X and Y (-1..1, Y up) */
static _Atomic uint64_t fp_pad_time_us;       /* when the pad sampler last ran */
static _Atomic int fp_stick_to_game = 1;      /* the right stick goes to the game (turn=game|off, lock-on) */
static _Atomic uint32_t fp_remap_bits;        /* float: the left stick's rotation (radians) */
static _Atomic uint32_t fp_target_bits;       /* float: the heading the game camera must have */
static _Atomic uint32_t fp_pivot_bits[2];     /* floats: the player's x, z (the turn's pivot) */
static _Atomic int fp_sync_on;                /* H1: turn the game camera this frame */
static _Atomic int fp_cam_fired;              /* the in-game step hook ran (it is installed) */
static _Atomic uint32_t fp_cam_own_bits;      /* float: the game camera's own heading (logs) */
static _Atomic uint32_t fp_cam_turns;         /* frames the hook turned the game camera */
static _Atomic int fp_frame_active;           /* H1 put this frame's camera in first person */
static _Atomic int fp_turn_follow = 1;        /* the hook turns the follow camera too (control deep=0: no) */

static uint64_t fp_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static float env_float(const char *name, float fallback, float lo, float hi) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    const float f = strtof(v, NULL);
    if (!isfinite(f) || f < lo || f > hi) {
        fprintf(stderr, "VR FP: %s=%s ignored (%g..%g)\n", name, v, lo, hi);
        return fallback;
    }
    return f;
}

#define FP_FACE_EASE_S 0.15f
static float face_ease_s = FP_FACE_EASE_S;

static int face_parse(const char *v) {
    if (!strcmp(v, "view")) return 1;
    if (!strcmp(v, "head")) return 2;
    if (!strcmp(v, "controller")) return 3;
    if (!strcmp(v, "attack")) return 4;
    return 0;
}
static const char *face_name(int m) {
    return m == 1 ? "view" : m == 2 ? "head" : m == 3 ? "controller (-> view)" : m == 4 ? "attack (view only while attacking)" : "off";
}
static const char *anim_lock_name(int m) {
    return m == 1 ? "1 (fixed eye on the root)" : m >= 2 ? "2 (fixed eye + eased root-motion bursts)" : "0 (off)";
}
/* anim_lock=2: the root position (x, z) of the camera, eased while the root moves faster than
 * anim_speed (a lunge, a knockback, a roll; walking and running stay below it and pass through
 * unchanged). Critically damped follow (Unity SmoothDamp form) whose time constant goes from
 * ~0 (tracking) to anim_smooth with the burst weight w; w decays over 0.4 s so the arrival is eased too. */
static void anim_ease_root(float dt, VrVec3 *pos) {
    __typeof__(fp.anim_state) *a = &fp.anim_state;
    if (!a->valid || !(dt > 0.0f) || dt > 0.5f ||
        (pos->x - a->px) * (pos->x - a->px) + (pos->z - a->pz) * (pos->z - a->pz) > 9.0f) { /* start, gap, teleport */
        a->valid = 1;
        a->x = pos->x; a->z = pos->z; a->vx = a->vz = 0.0f; a->w = 0.0f;
        a->px = pos->x; a->pz = pos->z;
        return;
    }
    const float rvx = (pos->x - a->px) / dt, rvz = (pos->z - a->pz) / dt;
    a->px = pos->x; a->pz = pos->z;
    const float speed = sqrtf(rvx * rvx + rvz * rvz);
    float w = (speed - fp.anim_speed) / 1.5f;
    w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    a->w = w > a->w - dt / 0.4f ? w : a->w - dt / 0.4f;
    if (a->w < 0.0f) a->w = 0.0f;
    const float st = 0.02f + (fp.anim_smooth - 0.02f) * a->w; /* smooth time */
    const float om = 2.0f / (st > 0.02f ? st : 0.02f), x = om * dt;
    const float ex = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float cx = a->x - pos->x, cz = a->z - pos->z;
    const float tx = (a->vx + om * cx) * dt, tz = (a->vz + om * cz) * dt;
    a->vx = (a->vx - om * tx) * ex; a->vz = (a->vz - om * tz) * ex;
    cx = (cx + tx) * ex; cz = (cz + tz) * ex;
    const float n = sqrtf(cx * cx + cz * cz);
    if (n > 1.0f) { cx /= n; cz /= n; } /* never more than 1 m behind the body */
    a->x = pos->x + cx; a->z = pos->z + cz;
    pos->x = a->x; pos->z = a->z;
}

static const char *anchor_name(int a) {
    return a == FP_ANCHOR_BODY ? "body" : a == FP_ANCHOR_RAW ? "raw" : a == FP_ANCHOR_STABLE ? "stable" : "head";
}
static int anchor_parse(const char *v) {
    return !strcmp(v, "body") ? FP_ANCHOR_BODY : !strcmp(v, "raw") ? FP_ANCHOR_RAW
         : !strcmp(v, "stable") ? FP_ANCHOR_STABLE : FP_ANCHOR_HEAD;
}

/* What hide= names (findings/16 4.2): ChrAsm slots, measured by hiding one slot at a time
 * (tools/vr_fp_test.py --steps parts) and by the part names the log lists:
 *   0 FG_A_0100 FaceGen head    1 FC_M_0000 the skin (bare hands and arms)   2 HD hat/hood
 *   3 BD body armour (coat)     4 AM arm armour (sleeves, gloves)             5 LG legs
 *   6 HR hair                   7-10, 11-14 the two hands' weapon models      15, 16 more weapons
 *   17-23 FG_A FaceGen parts (eyes, brows, beard, ...); base: the c0000 body [P + 0x48]. */
#define FP_SLOT(i) (1u << (i))
#define FP_HIDE_HEAD (FP_SLOT(0) | FP_SLOT(2) | FP_SLOT(6) | 0x00FE0000u | FP_HIDE_SKIN_HEAD)
#define FP_HIDE_BODY (FP_HIDE_HEAD | FP_SLOT(3) | FP_SLOT(5) | FP_HIDE_BASE | FP_HIDE_SKIN_BODY)
/* The skin part (FC_M_0000) is one model: head, neck, arms, hands, torso (findings/16 4.2). Its
 * meshes carry FLVER display-mask tags (#NN# material names, findings/11 2.4): the head and neck
 * go by clearing those groups in its display mask (+0x170), the arms (43) and hands (44) stay. */
#define FP_SKIN_HEAD_GROUPS ((1ull << 41) | (1ull << 47) | (1ull << 48) | (1ull << 49))
#define FP_SKIN_BODY_GROUPS ((1ull << 34) | (1ull << 42) | (1ull << 45) | (1ull << 46) | (1ull << 50) | (1ull << 61))
#define FP_HIDE_WEAPONS 0x0001FF80u
static const struct { const char *name; uint32_t bits; } fp_hide_groups[] = {
    {"none", 0},
    {"head", FP_HIDE_HEAD},          /* face, hair, hat: what the near plane cut */
    {"body", FP_HIDE_BODY},          /* all but the arms (skin, sleeves), hands and weapons */
    {"all", FP_HIDE_ALL_BITS},
    {"weapons", FP_HIDE_WEAPONS},
    {"skin", FP_SLOT(1)},
    {"arms", FP_SLOT(4)},
    {"base", FP_HIDE_BASE},
    {"skinhead", FP_HIDE_SKIN_HEAD},
    {"skinbody", FP_HIDE_SKIN_BODY},
};
/* hide=<token>[+<token>...]: a group name, sN (ChrAsm slot N), or a number (slot bits; bit 24 the
 * base body, 25/26 the skin's head/torso groups). Returns 0 (and leaves *out) when a token is not
 * understood. */
static int fp_parse_hide(const char *v, uint32_t *out) {
    uint32_t bits = 0;
    char token[32];
    while (*v) {
        size_t n = strcspn(v, "+,|");
        if (n == 0) { ++v; continue; }
        if (n >= sizeof(token)) return 0;
        memcpy(token, v, n);
        token[n] = 0;
        v += n;
        int found = 0;
        for (size_t i = 0; i < sizeof(fp_hide_groups) / sizeof(*fp_hide_groups); ++i)
            if (!strcmp(token, fp_hide_groups[i].name)) { bits |= fp_hide_groups[i].bits; found = 1; }
        if (!found && token[0] == 's' && token[1] >= '0' && token[1] <= '9') {
            const long slot = strtol(token + 1, NULL, 10);
            if (slot < 0 || slot >= FP_ASM_SLOTS) return 0;
            bits |= 1u << slot;
            found = 1;
        }
        if (!found) {
            char *end;
            const unsigned long x = strtoul(token, &end, 0);
            if (*end || end == token) return 0;
            bits |= (uint32_t)x & FP_HIDE_ALL_BITS;
        }
    }
    *out = bits;
    return 1;
}

static void fp_init(void) {
    if (fp.ready) return;
    fp.ready = 1;
    const char *e = getenv("BB_VR_FP");
    fp.enabled = e && e[0] == '1';
    e = getenv("BB_VR_FP_LOG");
    fp.log = e && e[0] == '1';
    e = getenv("BB_VR_FP_ANCHOR");
    fp.anchor = e ? anchor_parse(e) : FP_ANCHOR_HEAD;
    e = getenv("BB_VR_FP_FACE");
    if (e && *e) {
        fp.face = face_parse(e);
        if (!fp.face && strcmp(e, "off") && strcmp(e, "0")) fprintf(stderr, "VR FP: BB_VR_FP_FACE=%s ignored (off, view, head, controller, attack)\n", e);
    }
    e = getenv("BB_VR_FP_FACE_LOCKON");
    fp.face_lockon = e && e[0] == '1';
    e = getenv("BB_VR_FP_FACE_DODGE");
    fp.face_dodge = e && !strcmp(e, "view") ? 1 : e && !strcmp(e, "strafe") ? 2 : 0;
    fp.strafe_dash = NAN;
    e = getenv("BB_VR_FP_SYNC");
    fp.sync = !(e && e[0] == '0');
    e = getenv("BB_VR_FP_REMAP");
    fp.remap = e && e[0] == '0' ? 0 : 1; /* the residual with the hook, everything without it */
    fp.smooth = env_float("BB_VR_FP_SMOOTH", 0.12f, 0.0f, 2.0f);
    fp.floor_frac = env_float("BB_VR_FP_FLOOR", 0.75f, 0.0f, 1.0f);
    fp.near_plane = env_float("BB_VR_FP_NEAR", 0.2f, 0.0f, 1.0f);
    fp.forward = env_float("BB_VR_FP_FORWARD", 0.0f, -0.5f, 0.5f);
    fp.up = env_float("BB_VR_FP_UP", 0.0f, -0.3f, 0.3f);
    fp.down_s = env_float("BB_VR_FP_DOWN", 1.5f, 0.0f, 60.0f);
    fp.anim_lock = (int)env_float("BB_VR_FP_ANIM_LOCK", 0.0f, 0.0f, 2.0f);
    fp.anim_smooth = env_float("BB_VR_FP_ANIM_SMOOTH", 0.25f, 0.02f, 2.0f);
    fp.anim_speed = env_float("BB_VR_FP_ANIM_SPEED", 4.5f, 0.5f, 30.0f);
    /* The turn model: snap 30 in the headset, the game's own turning flat (no head). */
    vrt_config_default(&fp.turn);
    e = getenv("BB_VR_HEAD");
    const int head = e && e[0] == '1';
    const float snap_deg = env_float("BB_VR_SNAP_DEG", 30.0f, 0.0f, 180.0f);
    if (snap_deg >= 1.0f) fp.turn.snap_rad = snap_deg * VR_DEG;
    fp.turn_game = !head || snap_deg < 1.0f;
    fp.turn.smooth_rad_s = env_float("BB_VR_TURN_SPEED", 90.0f, 1.0f, 720.0f) * VR_DEG;
    e = getenv("BB_VR_TURN");
    if (e && !strcmp(e, "game")) fp.turn_game = 1;
    else if (e && *e) {
        if (vrt_config_set(&fp.turn, "turn", e)) fp.turn_game = 0;
        else fprintf(stderr, "VR FP: BB_VR_TURN=%s ignored (snap, smooth, off, game)\n", e);
    }
    e = getenv("BB_VR_FP_MOVE");
    if (e && *e && !vrt_config_set(&fp.turn, "move", e))
        fprintf(stderr, "VR FP: BB_VR_FP_MOVE=%s ignored (body, head, controller)\n", e);
    e = getenv("BB_VR_FP_HIDE");
    if (e && *e && !fp_parse_hide(e, &fp.hide)) fprintf(stderr, "VR FP: BB_VR_FP_HIDE=%s not understood\n", e);
    fp.start = fp_now_us();
    fp.head_bone = fp.neck_bone = -1;
    fp.eye_height = FP_EYE_BIND_Y;
    fp.state = "starting";
    fp.on = 1;
    fp.hide_bits = 1;
    e = getenv("BB_VR_FP_CONTROL");
    fp.control = e && *e ? e : NULL;
    e = getenv("BB_VR_FP_TRACE");
    const char *trace = e && *e ? e : fp.log ? "out/vr_fp_trace.csv" : NULL;
    if (fp.enabled && trace) {
        fp.trace = fopen(trace, "w");
        if (fp.trace)
            fputs("t,active,px,py,pz,yaw,eye_rx,eye_ry,eye_rz,eye_x,eye_y,eye_z,wx,wy,wz,cam_x,cam_y,cam_z,heading,game_own,psi,"
                  "dt,stick_x,lx,ly,head_yaw,ref_yaw,view_yaw,move_yaw,turn_delta,turn_rate,snapped,recentered,sync,"
                  "stick_to_game,locked,target_valid,target_yaw,turn,move,hidden\n", fp.trace);
        else
            fprintf(stderr, "VR FP: trace %s could not be opened\n", trace);
    }
    if (fp.enabled)
        fprintf(stderr, "VR FP: first person on (anim_lock %s, anchor %s, smooth %.2f s, floor %.2f, near %.2f m, turn %s (snap %.0f deg, smooth %.0f deg/s), move %s, hide 0x%x, camera heading sync %s, stick remap %s%s%s)\n",
                anim_lock_name(fp.anim_lock), anchor_name(fp.anchor), fp.smooth, fp.floor_frac, fp.near_plane,
                fp.turn_game ? "game" : vrt_turn_name(fp.turn.turn), fp.turn.snap_rad / VR_DEG,
                fp.turn.smooth_rad_s / VR_DEG, vrt_move_name(fp.turn.move), fp.hide, fp.sync ? "on" : "off",
                fp.remap ? "on (the residual with the hook)" : "off", fp.trace ? ", trace " : "",
                fp.trace ? trace : "");
}

int vr_fp_enabled(void) {
    fp_init();
    return fp.enabled;
}

int vr_fp_active(void) { return atomic_load_explicit(&fp_frame_active, memory_order_relaxed); }

VrVec3 vr_fp_dash_offset(void) { return fp.dash_offset; }

/* While a dodge plays under face_dodge=game the body is not turned by us: the arms must use the game's
 * live yaw (it can change in the step, before the arms run) instead of the H1 snapshot's. */
static int fp_readable(uintptr_t a, size_t n);
int vr_fp_yaw_live(float *yaw) {
    if (!fp.dodge_live || !fp.phys_now || !fp_readable(fp.phys_now + 0x1D4, 4)) return 0;
    const float y = *(volatile float *)(fp.phys_now + 0x1D4);
    if (!isfinite(y)) return 0;
    *yaw = y;
    return 1;
}

int vr_fp_body(VrVec3 *pos, float *yaw) {
    if (!fp.body_ok) return 0;
    *pos = fp.body_pos;
    *yaw = fp.body_yaw;
    return 1;
}

/* BB_VR_FP_CONTROL=<file>: whitespace-separated tokens, re-read (at most every 100 ms) when the
 * file changes, so a running game can be tuned (in the headset) or A/B-tested (tools/vr_fp_test.py):
 *   fp=0|1 near=<m> anchor=head|body|raw|stable smooth=<s> floor=<f> forward=<m> up=<m>
 *   anim_lock=0|1|2 (comfort: 1 = the eye fixed above the root, no animation sway, flinch or yaw;
 *   2 = also eases root-motion bursts) anim_smooth=<s> (mode 2, default 0.25) anim_speed=<m/s> (mode 2: burst threshold, 4.5)
 *   hide=none|head|body|all|... (fp_parse_hide) hide_tp=0|1 (also in third person: diagnostics)
 *   turn=snap|smooth|off|game snap=<deg> (0: game) turn_speed=<deg/s> deadzone= expo= turn_accel=<s>
 *   snap_on= snap_off= snap_repeat=<s> move=body|head|controller recenter=view|reset
 *   lock=view|game|turn lock_auto=<deg> (vr_turn_math.h, the same keys as the sandbox's)
 *   face=off|view|head|controller|attack face_lockon=0|1 (the body follows the view, see BB_VR_FP_FACE)
 *   sync=0|1 remap=0|1 (the left stick takes what the game camera lacks) deep=0|1 (deep=0: the hook turns ChrCam
 *   only, not the follow camera; for A/B tests).
 * A token that is absent keeps its value; others are ignored. */
static void fp_poll_control(uint64_t now) {
    if (!fp.control || now - fp.control_checked < 100000) return;
    fp.control_checked = now;
    struct stat st;
    if (stat(fp.control, &st) != 0) return;
    if ((uint64_t)st.st_mtime == fp.control_mtime && (uint64_t)st.st_size == fp.control_size) return;
    fp.control_mtime = (uint64_t)st.st_mtime;
    fp.control_size = (uint64_t)st.st_size;
    FILE *f = fopen(fp.control, "r");
    if (!f) return;
    char token[96];
    while (fscanf(f, "%95s", token) == 1) {
        char *eq = strchr(token, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = token, *v = eq + 1;
        const float x = strtof(v, NULL);
        const int ok = isfinite(x);
        if (!strcmp(k, "fp")) fp.on = x != 0.0f;
        else if (!strcmp(k, "near") && ok && x >= 0.0f && x <= 1.0f) fp.near_plane = x;
        else if (!strcmp(k, "smooth") && ok && x >= 0.0f && x <= 2.0f) fp.smooth = x;
        else if (!strcmp(k, "floor") && ok && x >= 0.0f && x <= 1.0f) fp.floor_frac = x;
        else if (!strcmp(k, "forward") && ok && fabsf(x) <= 0.5f) fp.forward = x;
        else if (!strcmp(k, "up") && ok && fabsf(x) <= 0.3f) fp.up = x;
        else if (!strcmp(k, "anchor")) fp.anchor = anchor_parse(v);
        else if (!strcmp(k, "anim_lock") && ok && x >= 0.0f && x <= 2.0f) fp.anim_lock = (int)x;
        else if (!strcmp(k, "anim_smooth") && ok && x >= 0.02f && x <= 2.0f) fp.anim_smooth = x;
        else if (!strcmp(k, "anim_speed") && ok && x >= 0.5f && x <= 30.0f) fp.anim_speed = x;
        else if (!strcmp(k, "hide")) { if (!fp_parse_hide(v, &fp.hide)) fprintf(stderr, "VR FP: hide=%s not understood\n", v); }
        else if (!strcmp(k, "hide_tp")) fp.hide_tp = x != 0.0f;
        else if (!strcmp(k, "hide_bits") && ok && x >= 1.0f && x <= 3.0f) fp.hide_bits = (uint8_t)x; /* bits 2-5 are never restored */
        else if (!strcmp(k, "turn") && !strcmp(v, "game")) fp.turn_game = 1;
        else if (!strcmp(k, "turn") && vrt_config_set(&fp.turn, k, v)) fp.turn_game = 0;
        else if (!strcmp(k, "snap") && ok && x < 1.0f) fp.turn_game = 1; /* snap=0: the game's own turning */
        else if (!strcmp(k, "snap") && vrt_config_set(&fp.turn, k, v)) { if (fp.turn.turn == VRT_TURN_SNAP) fp.turn_game = 0; }
        else if (vrt_config_set(&fp.turn, k, v)) {}
        else if (!strcmp(k, "face")) { fp.face = face_parse(v); fprintf(stderr, "VR FP: face %s\n", face_name(fp.face)); }
        else if (!strcmp(k, "face_lockon")) fp.face_lockon = x != 0.0f;
        else if (!strcmp(k, "face_dodge")) { fp.face_dodge = !strcmp(v, "view") ? 1 : !strcmp(v, "strafe") ? 2 : 0; fp.strafe_on = 0; fprintf(stderr, "VR FP: face_dodge %s\n", fp.face_dodge == 2 ? "strafe" : fp.face_dodge ? "view" : "game"); }
        else if (!strcmp(k, "sync")) fp.sync = x != 0.0f;
        else if (!strcmp(k, "remap")) fp.remap = x != 0.0f;
        else if (!strcmp(k, "deep")) atomic_store_explicit(&fp_turn_follow, x != 0.0f, memory_order_relaxed);
    }
    fclose(f);
    fprintf(stderr, "VR FP: control file: fp %d near %.3f anchor %s smooth %.2f floor %.2f forward %.2f up %.2f hide 0x%x%s | turn %s snap %.0f speed %.0f accel %.2f move %s lock %s lock_auto %.0f recenter %s | sync %d remap %d | anim_lock %s smooth %.2f s speed %.1f m/s\n",
            fp.on, fp.near_plane, anchor_name(fp.anchor), fp.smooth, fp.floor_frac, fp.forward, fp.up, fp.hide,
            fp.hide_tp ? " (third person too)" : "", fp.turn_game ? "game" : vrt_turn_name(fp.turn.turn),
            fp.turn.snap_rad / VR_DEG, fp.turn.smooth_rad_s / VR_DEG, fp.turn.accel_s, vrt_move_name(fp.turn.move),
            vrt_lock_name(fp.turn.lock), fp.turn.lock_auto_rad / VR_DEG, fp.turn.recenter_keeps_view ? "view" : "reset",
            fp.sync, fp.remap, anim_lock_name(fp.anim_lock), fp.anim_smooth, fp.anim_speed);
}

/* ---- guest memory: read only what is mapped (the player chain dangles during loads) ---- */

#ifdef _WIN32
static struct { uintptr_t lo, hi; } fp_regions[16];
static int fp_region_count;
static uint64_t fp_regions_time;
static int fp_readable(uintptr_t a, size_t n) {
    if (a < 0x10000 || a + n < a) return 0;
    const uint64_t now = fp_now_us();
    if (now - fp_regions_time > 1000000) { fp_region_count = 0; fp_regions_time = now; }
    for (int i = 0; i < fp_region_count; ++i)
        if (fp_regions[i].lo <= a && a + n <= fp_regions[i].hi) return 1;
    uintptr_t at = a;
    while (at < a + n) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void *)at, &mbi, sizeof(mbi))) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return 0;
        const uintptr_t lo = (uintptr_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
        if (fp_region_count < (int)(sizeof(fp_regions) / sizeof(*fp_regions))) {
            fp_regions[fp_region_count].lo = lo;
            fp_regions[fp_region_count].hi = hi;
            ++fp_region_count;
        }
        at = hi;
    }
    return 1;
}
#else
static int fp_readable(uintptr_t a, size_t n) {
    static int fds[2] = {-1, -1};
    if (a < 0x10000 || a + n < a) return 0;
    if (fds[0] < 0 && pipe(fds) != 0) return 0;
    const ssize_t w = write(fds[1], (const void *)a, n > 4096 ? 4096 : n);
    if (w < 0) return 0;
    char sink[4096];
    if (read(fds[0], sink, (size_t)w) < 0) return 0;
    return 1;
}
#endif

static int rd64(uintptr_t a, uint64_t *out) {
    if (!fp_readable(a, 8)) return 0;
    memcpy(out, (const void *)a, 8);
    return 1;
}
static int rd_ptr(uintptr_t a, uintptr_t *out) {
    uint64_t v;
    if (!rd64(a, &v) || v < 0x10000 || (v & 7)) return 0;
    *out = (uintptr_t)v;
    return 1;
}
static uintptr_t va(uint32_t v) { return (uintptr_t)(fp.image + (v - VA_BASE)); }
static uint64_t to_va(uint64_t host) { return host - (uint64_t)(uintptr_t)fp.image + VA_BASE; }

/* hkArray { T *data; int32 size; int32 capacityAndFlags; } */
static int rd_array(uintptr_t a, uintptr_t *data, int *size) {
    uint64_t d;
    uint32_t n;
    if (!rd64(a, &d) || !fp_readable(a + 8, 4)) return 0;
    memcpy(&n, (const void *)(a + 8), 4);
    if (!d || (d & 3) || (int32_t)n <= 0 || n > 4096) return 0;
    *data = (uintptr_t)d;
    *size = (int)n;
    return 1;
}

typedef struct {
    uintptr_t player, phys, pose, skel;
    uintptr_t local, model, flags, parents, bones, ref;
    int n_local, n_model, n_flags, n_parents, n_bones, n_ref;
    VrVec3 pos;
    float yaw;
    int pose_ok;
} FpChain;

static int fp_bone_named(uintptr_t bones, int i, const char *name) {
    uint64_t s;
    if (!rd64(bones + (uintptr_t)i * 16, &s)) return 0;
    s &= ~(uint64_t)1; /* hkStringPtr: bit 0 is the "owned" flag */
    const size_t n = strlen(name) + 1;
    if (!s || !fp_readable((uintptr_t)s, n)) return 0;
    return !memcmp((const void *)(uintptr_t)s, name, n);
}

/* model[i] from the pose: the model transform when it is clean, else composed from the local
 * pose up the parents (Havok's boneFlags: 1 local dirty, 2 model dirty). */
static int fp_model_bone(const FpChain *c, int i, VrXform *out, int depth) {
    if (i < 0 || i >= c->n_model || depth > 64) return 0;
    uint32_t flags = 0;
    if (i < c->n_flags) memcpy(&flags, (const void *)(c->flags + (uintptr_t)i * 4), 4);
    if (!(flags & 2)) return vr_xf_from_qs((const float *)(c->model + (uintptr_t)i * 48), out);
    VrXform local;
    if (i >= c->n_local || !vr_xf_from_qs((const float *)(c->local + (uintptr_t)i * 48), &local)) return 0;
    int16_t parent = -1;
    if (i < c->n_parents) memcpy(&parent, (const void *)(c->parents + (uintptr_t)i * 2), 2);
    if (parent < 0) { *out = local; return 1; }
    VrXform p;
    if (!fp_model_bone(c, parent, &p, depth + 1)) return 0;
    *out = vr_xf_mul(p, local);
    return 1;
}
/* The bind (reference) pose's model transform of bone i. */
static int fp_bind_bone(const FpChain *c, int i, VrXform *out) {
    VrXform acc = vr_xf_identity();
    int chain[64], n = 0;
    for (int b = i; b >= 0 && n < 64;) {
        if (b >= c->n_ref || b >= c->n_parents) return 0;
        chain[n++] = b;
        int16_t parent;
        memcpy(&parent, (const void *)(c->parents + (uintptr_t)b * 2), 2);
        b = parent;
    }
    for (int k = n - 1; k >= 0; --k) {
        VrXform local;
        if (!vr_xf_from_qs((const float *)(c->ref + (uintptr_t)chain[k] * 48), &local)) return 0;
        acc = vr_xf_mul(acc, local);
    }
    *out = acc;
    return 1;
}

static int fp_read_chain(FpChain *c) {
    memset(c, 0, sizeof(*c));
    uintptr_t wcm, bag, beh, holder;
    if (!rd_ptr(va(VA_WORLD_CHR_MAN), &wcm) || !rd_ptr(wcm + 0x60, &c->player)) { fp.state = "no player"; return 0; }
    uint64_t vtable;
    if (!rd64(c->player, &vtable) || vtable != (uint64_t)va(VA_PLAYER_INS_VTABLE)) { fp.state = "player: not a PlayerIns"; c->player = 0; return 0; }
    if (!rd_ptr(c->player + 0x3B0, &bag) || !rd_ptr(bag + 0x68, &c->phys) || !fp_readable(c->phys + 0x1D0, 0x20)) {
        fp.state = "no physics module";
        return 0;
    }
    float p[4], rot[4];
    memcpy(p, (const void *)(c->phys + 0x1E0), 16);
    memcpy(rot, (const void *)(c->phys + 0x1D0), 16);
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || !isfinite(rot[1]) ||
        fabsf(p[0]) > 1e5f || fabsf(p[1]) > 1e5f || fabsf(p[2]) > 1e5f) {
        fp.state = "physics: no position";
        return 0;
    }
    c->pos = vr_v3(p[0], p[1], p[2]);
    c->yaw = rot[1];
    /* The pose: optional (the body anchor works without it). */
    if (!rd_ptr(bag + 0x30, &beh) || !rd_ptr(beh + 0x28, &holder)) return 1;
    c->pose = holder + 0x38;
    if (!rd_ptr(c->pose, &c->skel) ||
        !rd_array(c->pose + 0x08, &c->local, &c->n_local) || !rd_array(c->pose + 0x18, &c->model, &c->n_model) ||
        !rd_array(c->skel + 0x18, &c->parents, &c->n_parents) || !rd_array(c->skel + 0x28, &c->bones, &c->n_bones) ||
        !rd_array(c->skel + 0x38, &c->ref, &c->n_ref))
        return 1;
    if (!rd_array(c->pose + 0x28, &c->flags, &c->n_flags)) c->n_flags = 0;
    if (!fp_readable(c->local, (size_t)c->n_local * 48) || !fp_readable(c->model, (size_t)c->n_model * 48) ||
        !fp_readable(c->parents, (size_t)c->n_parents * 2) || !fp_readable(c->bones, (size_t)c->n_bones * 16) ||
        !fp_readable(c->ref, (size_t)c->n_ref * 48) || (c->n_flags && !fp_readable(c->flags, (size_t)c->n_flags * 4)))
        return 1;
    /* Bones by name, once per skeleton (Havok order: Head 78, Neck 77 for c0000, findings/13). */
    if (c->skel != fp.skel) {
        fp.skel = c->skel;
        fp.head_bone = fp.neck_bone = -1;
        fp.eye_offset_ok = 0;
        for (int i = 0; i < c->n_bones; ++i) {
            if (fp.head_bone < 0 && fp_bone_named(c->bones, i, "Head")) fp.head_bone = i;
            if (fp.neck_bone < 0 && fp_bone_named(c->bones, i, "Neck")) fp.neck_bone = i;
        }
        VrXform bind = vr_xf_identity();
        if (fp.head_bone >= 0 && fp_bind_bone(c, fp.head_bone, &bind)) {
            fp.eye_offset = vr_xf_apply(vr_xf_inverse(bind), vr_v3(0.0f, FP_EYE_BIND_Y, FP_EYE_BIND_Z));
            fp.eye_offset_ok = 1;
        }
        fprintf(stderr, "VR FP: skeleton 0x%llx: %d bones, Head %d, Neck %d; Head bind (%.3f %.3f %.3f) -> eye offset in the Head frame (%.3f %.3f %.3f)%s\n",
                (unsigned long long)c->skel, c->n_bones, fp.head_bone, fp.neck_bone, bind.t.x, bind.t.y, bind.t.z,
                fp.eye_offset.x, fp.eye_offset.y, fp.eye_offset.z, fp.eye_offset_ok ? "" : " (NOT FOUND)");
    }
    c->pose_ok = fp.head_bone >= 0 && fp.eye_offset_ok;
    return 1;
}

/* ---- the hunter's own model (findings/16 section 4) ---- */

/* A model's display entity ([model + 0x18], a SprjFD4ModelDispEntity), or 0. */
static uintptr_t fp_disp_of(uintptr_t model) {
    uintptr_t disp;
    uint64_t vt;
    if (!model || !rd_ptr(model + 0x18, &disp) || !rd64(disp, &vt) || vt != (uint64_t)va(VA_DISP_ENTITY_VTABLE) ||
        !fp_readable(disp + 0x170, 0xD4))
        return 0;
    return disp;
}
/* The ChrAsm part in slot i (0 when the slot is empty or unreadable). */
static uintptr_t fp_part(uintptr_t holder, int i) {
    uint64_t m;
    if (!holder || !rd64(holder + 0x28 + (uintptr_t)i * 8, &m) || m < 0x10000 || (m & 7)) return 0;
    return (uintptr_t)m;
}
/* Clears the draw bit (+0x210 bit 0) of the display entities in `mask`: the PlayerIns visibility
 * update set it this frame (DrawUpdate, in the in-game step), the draw lists test it after H1.
 * Returns how many were hidden. */
static int fp_hide_apply(uintptr_t player, uint32_t mask) {
    const uint8_t keep = (uint8_t)~fp.hide_bits;
    int n = 0;
    uintptr_t holder = 0, base = 0, disp;
    if ((mask & FP_HIDE_SLOTS) && rd_ptr(player + 0x4D0, &holder))
        for (int i = 0; i < FP_ASM_SLOTS; ++i)
            if ((mask & (1u << i)) && (disp = fp_disp_of(fp_part(holder, i)))) {
                *(volatile uint8_t *)(disp + 0x210) &= keep;
                ++n;
            }
    if ((mask & FP_HIDE_BASE) && rd_ptr(player + 0x48, &base) && (disp = fp_disp_of(base))) {
        *(volatile uint8_t *)(disp + 0x210) &= keep;
        ++n;
    }
    return n;
}
/* The skin's display mask (slot 1, +0x170, 128 bits: the visible #NN# groups; the game computes
 * it from the protectors' invisibleFlags, 0x209cd20): `clear` groups cleared while hiding, the
 * game's value put back afterwards (only when ours is still there: an equipment change wins). */
static struct { uintptr_t disp; int have; uint64_t game[2], ours[2]; } fp_skin;
static void fp_skin_mask(uintptr_t player, uint64_t clear) {
    uintptr_t holder = 0, disp = 0;
    if (player && rd_ptr(player + 0x4D0, &holder)) disp = fp_disp_of(fp_part(holder, 1));
    if (disp != fp_skin.disp) { fp_skin.disp = disp; fp_skin.have = 0; } /* another model: nothing to restore */
    if (!disp) return;
    uint64_t cur[2];
    memcpy(cur, (const void *)(disp + 0x170), 16);
    if (!clear) {
        if (fp_skin.have && cur[0] == fp_skin.ours[0] && cur[1] == fp_skin.ours[1])
            memcpy((void *)(disp + 0x170), fp_skin.game, 16);
        fp_skin.have = 0;
        return;
    }
    if (!fp_skin.have || cur[0] != fp_skin.ours[0] || cur[1] != fp_skin.ours[1]) { /* the game (re)wrote it */
        memcpy(fp_skin.game, cur, 16);
        fp_skin.have = 1;
    }
    fp_skin.ours[0] = fp_skin.game[0] & ~clear;
    fp_skin.ours[1] = fp_skin.game[1];
    if (cur[0] != fp_skin.ours[0]) memcpy((void *)(disp + 0x170), fp_skin.ours, 16);
}
/* A part name ("HD_M_1100", "c0000", ...) as UTF-16 or ASCII text at p, within its first 64
 * characters: two letters, '_', A/F/M, '_', four digits; or c + four digits. */
static int fp_name_at(uintptr_t p, char *out, size_t size) {
    if (!fp_readable(p, 128)) return 0;
    char text[65];
    int n = 0;
    const unsigned char *b = (const unsigned char *)p;
    const int wide = b[1] == 0 && b[3] == 0;
    for (; n < 64; ++n) {
        const unsigned c = wide ? (unsigned)(b[2 * n] | b[2 * n + 1] << 8) : b[n];
        if (c < 0x20 || c > 0x7e) break;
        text[n] = (char)c;
    }
    text[n] = 0;
    for (int i = 0; i + 5 <= n; ++i) {
        const char *s = text + i;
        const int digits4 = i + 9 <= n && s[5] >= '0' && s[5] <= '9' && s[6] >= '0' && s[6] <= '9' &&
                            s[7] >= '0' && s[7] <= '9' && s[8] >= '0' && s[8] <= '9';
        if (digits4 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[2] == '_' && s[4] == '_' &&
            strchr("AFMafm", s[3])) {
            snprintf(out, size, "%.9s", s);
            return 1;
        }
        if ((s[0] == 'c' || s[0] == 'C') && s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9' &&
            s[3] >= '0' && s[3] <= '9' && s[4] >= '0' && s[4] <= '9' && (i + 5 == n || s[5] < '0' || s[5] > '9')) {
            snprintf(out, size, "%.5s", s);
            return 1;
        }
    }
    return 0;
}
/* A model's resource name, searched two pointers deep from the model and its display entity
 * (diagnostics only). */
static void fp_model_name(uintptr_t model, char *out, size_t size) {
    snprintf(out, size, "?");
    const uintptr_t roots[2] = {model, fp_disp_of(model)};
    for (int r = 0; r < 2; ++r) {
        if (!roots[r] || !fp_readable(roots[r], 0x300)) continue;
        for (uintptr_t o = 0; o < 0x300; o += 8) {
            uintptr_t p;
            if (!rd_ptr(roots[r] + o, &p)) continue;
            if (fp_name_at(p, out, size)) return;
            if (!fp_readable(p, 0x100)) continue;
            for (uintptr_t q = 0; q < 0x100; q += 8) {
                uintptr_t s;
                if (rd_ptr(p + q, &s) && fp_name_at(s, out, size)) return;
            }
        }
    }
}
/* The models in the log: once per parts holder, and again when hide= changes. */
static void fp_parts_dump(uintptr_t player) {
    uintptr_t holder = 0, base = 0;
    rd_ptr(player + 0x4D0, &holder);
    rd_ptr(player + 0x48, &base);
    if (holder == fp.parts_dumped && fp.hide == fp.dump_hide) return;
    fp.parts_dumped = holder;
    fp.dump_hide = fp.hide;
    char name[32];
    uintptr_t disp = fp_disp_of(base);
    fp_model_name(base, name, sizeof(name));
    uint32_t flags = disp ? *(const uint32_t *)(disp + 0x210) : 0;
    fprintf(stderr, "VR FP: models (hide 0x%x): base 0x%llx disp 0x%llx flags 0x%08x name %s; ChrAsm parts 0x%llx\n",
            fp.hide, (unsigned long long)base, (unsigned long long)disp, flags, name, (unsigned long long)holder);
    if (!holder) return;
    for (int i = 0; i < FP_ASM_SLOTS; ++i) {
        const uintptr_t m = fp_part(holder, i);
        if (!m) { fprintf(stderr, "VR FP:   part %2d empty\n", i); continue; }
        uint64_t vt = 0;
        rd64(m, &vt);
        disp = fp_disp_of(m);
        fp_model_name(m, name, sizeof(name));
        uint64_t mask[2] = {0, 0};
        flags = 0;
        if (disp) {
            memcpy(mask, (const void *)(disp + 0x170), 16);
            flags = *(const uint32_t *)(disp + 0x210);
        }
        fprintf(stderr, "VR FP:   part %2d model 0x%llx (vtable 0x%llx) disp 0x%llx flags 0x%08x mask %016llx%016llx name %s\n",
                i, (unsigned long long)m, (unsigned long long)to_va(vt), (unsigned long long)disp, flags,
                (unsigned long long)mask[1], (unsigned long long)mask[0], name);
    }
}

/* ---- lock-on: LockTgtMan [0x593E860] ---- */

/* 1 while the player is locked on; *heading (the camera heading's sense) toward the target when
 * the locked target's point reads plausibly. */
static int fp_lock_state(const FpChain *c, float *heading, int *valid) {
    *valid = 0;
    uintptr_t ltm, node;
    if (!rd_ptr(va(VA_LOCK_TGT_MAN), &ltm) || !fp_readable(ltm + 0x2820, 1)) return 0;
    if (!*(const volatile uint8_t *)(ltm + 0x2820)) return 0;
    if (!rd_ptr(ltm + 8, &node)) return 1;
    for (int guard = 0; node && guard < 64; ++guard) {
        if (!fp_readable(node, 0x98)) break;
        if (*(const uint8_t *)(node + 0x90) & 0x20) {
            float p[4];
            memcpy(p, (const void *)node, 16);
            const float dx = p[0] - c->pos.x, dz = p[2] - c->pos.z;
            if (isfinite(dx) && isfinite(dz) && dx * dx + dz * dz > 1e-4f && dx * dx + dz * dz < 1e4f) {
                *heading = atan2f(dx, dz);
                *valid = 1;
            }
            break;
        }
        uint64_t next;
        if (!rd64(node + 0x80, &next) || (next & 7)) break;
        node = (uintptr_t)next;
    }
    return 1;
}

/* ---- the pad (the turn stick, stick remap): the game's pad read, another thread ---- */

static void fp_pad_filter(uint32_t *buttons, uint8_t *lx, uint8_t *ly, uint8_t *rx, uint8_t *ry) {
    (void)ry;
    vr_dash_pad_sample(*buttons, *lx, *ly); /* findings/19: the dash's input fallback, before any remap */
    const uint64_t now = fp_now_us();
    atomic_store_explicit(&fp_pad_bits[0], f2u(((float)*rx - 128.0f) / 127.0f), memory_order_relaxed);
    atomic_store_explicit(&fp_pad_bits[1], f2u(((float)*lx - 128.0f) / 127.0f), memory_order_relaxed);
    atomic_store_explicit(&fp_pad_bits[2], f2u((128.0f - (float)*ly) / 127.0f), memory_order_relaxed);
    atomic_store_explicit(&fp_pad_time_us, now, memory_order_release);
    if (now > atomic_load_explicit(&fp_active_until_us, memory_order_acquire)) return;
    if (!atomic_load_explicit(&fp_stick_to_game, memory_order_relaxed))
        *rx = 128; /* first person turns with it (vr_turn_math.h): the game camera must not */
    if (fp.remap) { /* H1 sets the angle: the residual with the hook, all of it without (remap=0: none) */
        const float angle = u2f(atomic_load_explicit(&fp_remap_bits, memory_order_relaxed));
        if (fabsf(angle) > 0.5f * VR_DEG && (abs((int)*lx - 128) > 8 || abs((int)*ly - 128) > 8))
            vr_stick_rotate(lx, ly, angle);
    }
}

/* ---- the game camera's heading: hook "bb_hook_vr_fp_cam" in the in-game step at 0x1d3be5a
 * (replay-after; r13 = the step), right after the director (0x18368b0) built ChrCam
 * ([r13+0x128]) from its follow camera ([ChrCam+0x60], ChrExFollowCam) and before the step copies
 * it into the holder's render camera (r15), from where SprjCamera's field camera and H1's source
 * take it (findings/08 3).
 * Two things are turned about the vertical through the player, to the first-person heading:
 *  - ChrCam's matrix: the camera everything downstream sees this frame;
 *  - the follow camera's own state (its matrix +0x10, its output copy +0x2E0, its position +0x100,
 *    its yaw +0x144 and target yaw +0x270): its next update starts from that heading, and the
 *    hunter's movement, which reads the follow camera's state rather than ChrCam, follows it.
 *    Turning ChrCam alone left the walk on the follow camera's heading (findings/16 3). ---- */
static void fp_turn_point(float *p, float d, VrVec3 pivot) {
    float m[4][4] = {{0}};
    m[3][0] = p[0];
    m[3][2] = p[2];
    vr_cam_rotate_heading(m, d, pivot);
    p[0] = m[3][0];
    p[2] = m[3][2];
}
static int fp_turn_matrix(float (*m)[4], float target, VrVec3 pivot) {
    float out[4][4];
    memcpy(out, m, sizeof(out));
    vr_cam_rotate_heading(out, vr_wrap_pi(target - vr_cam_heading((const float (*)[4])out)), pivot);
    for (int i = 0; i < 16; ++i) if (!isfinite(out[i / 4][i % 4])) return 0;
    memcpy(m, out, sizeof(out));
    return 1;
}
static ABI void fp_hook_cam(BbHookContext *ctx) {
    uint64_t chrcam;
    memcpy(&chrcam, (const void *)(uintptr_t)(ctx->r13 + 0x128), 8); /* the next instruction loads it too */
    if (!chrcam) return;
    atomic_store_explicit(&fp_cam_fired, 1, memory_order_relaxed);
    float (*m)[4] = (float (*)[4])(uintptr_t)(chrcam + 0x10); /* rows right, up, forward, position */
    const float own = vr_cam_heading((const float (*)[4])m);
    atomic_store_explicit(&fp_cam_own_bits, f2u(own), memory_order_relaxed);
    if (!atomic_load_explicit(&fp_sync_on, memory_order_relaxed)) return;
    const float target = u2f(atomic_load_explicit(&fp_target_bits, memory_order_relaxed));
    const VrVec3 pivot = vr_v3(u2f(atomic_load_explicit(&fp_pivot_bits[0], memory_order_relaxed)), 0.0f,
                               u2f(atomic_load_explicit(&fp_pivot_bits[1], memory_order_relaxed)));
    if (!isfinite(target) || !fp_turn_matrix(m, target, pivot)) return;
    atomic_fetch_add_explicit(&fp_cam_turns, 1, memory_order_relaxed);
    if (!atomic_load_explicit(&fp_turn_follow, memory_order_relaxed)) return;
    uint64_t fc;
    memcpy(&fc, (const void *)(uintptr_t)(chrcam + 0x60), 8);
    if (!fc || (fc & 15)) return;
    float (*fm)[4] = (float (*)[4])(uintptr_t)(fc + 0x10);
    const float fd = vr_wrap_pi(target - vr_cam_heading((const float (*)[4])fm));
    if (!fp_turn_matrix(fm, target, pivot)) return;
    fp_turn_matrix((float (*)[4])(uintptr_t)(fc + 0x2E0), target, pivot);
    fp_turn_point((float *)(uintptr_t)(fc + 0x100), fd, pivot);
    *(float *)(uintptr_t)(fc + 0x144) = target;
    *(float *)(uintptr_t)(fc + 0x270) = target;
}

/* ---- H1 ---- */

static void fp_log_frame(const FpChain *c, const VrVec3 *eye_raw, const VrVec3 *eye, const VrVec3 *world,
                         const float game[4][4], float heading, int active) {
    fprintf(stderr, "VR FP: %s | player 0x%llx pos (%.3f %.3f %.3f) yaw %.2f deg (faces %.2f deg)\n",
            active ? "first person" : fp.state, (unsigned long long)c->player, c->pos.x, c->pos.y, c->pos.z,
            c->yaw / VR_DEG, vr_facing_heading(c->yaw) / VR_DEG);
    if (eye_raw)
        fprintf(stderr, "VR FP:   eye model raw (%.3f %.3f %.3f) used (%.3f %.3f %.3f) world (%.3f %.3f %.3f), above the feet %.3f m\n",
                eye_raw->x, eye_raw->y, eye_raw->z, eye->x, eye->y, eye->z, world->x, world->y, world->z,
                world->y - c->pos.y);
    if (c->pose_ok) {
        VrXform head;
        if (fp_model_bone(c, fp.head_bone, &head, 0))
            fprintf(stderr, "VR FP:   Head bone model t (%.3f %.3f %.3f) q (%.3f %.3f %.3f %.3f)\n",
                    head.t.x, head.t.y, head.t.z, head.q.x, head.q.y, head.q.z, head.q.w);
    }
    fprintf(stderr, "VR FP:   game camera pos (%.3f %.3f %.3f) heading %.2f deg (its own %.2f deg, turned in %u frames) | psi %.2f deg\n",
            game[3][0], game[3][1], game[3][2], heading / VR_DEG,
            u2f(atomic_load_explicit(&fp_cam_own_bits, memory_order_relaxed)) / VR_DEG,
            atomic_exchange_explicit(&fp_cam_turns, 0, memory_order_relaxed), fp.psi / VR_DEG);
    if (active)
        fprintf(stderr, "VR FP:   turn %s: view %.1f deg = psi + head %.1f deg, move %s %.1f deg, reference %.1f deg, lock %d%s%.1f, hidden %d models\n",
                fp.turn_game ? "game" : vrt_turn_name(fp.turn.turn), fp.turn_out.view_yaw / VR_DEG, fp.head_yaw / VR_DEG,
                vrt_move_name(fp.turn.move), fp.turn_out.move_yaw / VR_DEG, fp.ref_yaw / VR_DEG, fp.locked,
                fp.target_valid ? " target " : " (no target point) ", fp.target_valid ? fp.target_yaw / VR_DEG : 0.0f,
                fp.hidden);
    if (fp.face && active)
        fprintf(stderr, "VR FP:   face %s: writes %d, the game moved the yaw by %.1f deg since our write (max %.1f)%s\n",
                face_name(fp.face), fp.face_writes, fp.face_dev / VR_DEG, fp.face_dev_max / VR_DEG,
                fp.locked && !fp.face_lockon ? " | locked on: not overriding" : "");
    if (fp.trace) fflush(fp.trace);
}

/* A dodge (roll, quickstep, backstep) is playing: the newest TimeAct clip of the player (the ring at
 * [[P + 0x3B0] + 0x18] + 0x20, 10 x 16 B, head +0xC0, findings/13 3.4) is one of the a000_7000-7799 moves
 * (vr_dash_detect.h table; the running jump 7800 is excluded) and its clip length has not run out. The
 * ring keeps pushing the playing clip's ID, so a new ID (or a clip time that dropped: a chained dodge)
 * is the start. The first read is only a baseline. */
static int fp_dodge_update(const FpChain *c, uint64_t now) {
    uintptr_t bag, tam;
    if (!rd_ptr(c->player + 0x3B0, &bag) || !rd_ptr(bag + 0x18, &tam) || !fp_readable(tam, 0xC8)) { fp.dodge_on = 0; return 0; }
    unsigned char module[0xC8];
    memcpy(module, (const void *)tam, sizeof(module));
    int32_t anim, head;
    float f[3];
    if (!vr_dash_ring_newest(module, &anim, &head, f)) { fp.dodge_on = 0; return 0; }
    const VrDashMove *m = vr_dash_move_lookup(anim);
    if (m && (m->kind == VR_DASH_MOVE_JUMP || m->sub >= 7800)) m = NULL;
    const int fresh = fp.dodge_seen && (anim != fp.dodge_anim ||
                      f[0] < fp.dodge_f[0] - 0.1f || f[1] < fp.dodge_f[1] - 0.1f || f[2] < fp.dodge_f[2] - 0.1f);
    if (m && fresh) {
        fp.dodge_on = 1;
        fp.dodge_start_us = now;
        fp.dodge_len = m->clip + 0.1f;
        fp.dodge_fresh = 1;
        fp.dodge_roll = m->kind == VR_DASH_MOVE_ROLL;
        fp.dodge_tdeg = m->direction;
        if (fp.log) fprintf(stderr, "VR FP: face: dodge a%03d_%06d (%s) starts: the game's facing for %.2f s\n",
                            anim / 1000000, anim % 1000000, vr_dash_move_name(m->kind), fp.dodge_len);
    } else if (!m && fp.dodge_on) {
        fp.dodge_on = 0;
        if (fp.log) fprintf(stderr, "VR FP: face: dodge over (anim %d), back to the view over %.2f s\n", anim, face_ease_s);
    }
    fp.dodge_seen = 1;
    fp.dodge_anim = anim;
    memcpy(fp.dodge_f, f, sizeof(f));
    if (fp.dodge_on && (float)(now - fp.dodge_start_us) * 1e-6f > fp.dodge_len) {
        fp.dodge_on = 0;
        if (fp.log) fprintf(stderr, "VR FP: face: dodge over (clip length), back to the view over %.2f s\n", face_ease_s);
    }
    return fp.dodge_on;
}

/* face=attack: the newest TimeAct clip is a weapon move (anim % 1000000 in 30000..49999, the test of
 * runtime_vr_atkspeed.c) or a gun shot (50000..51999; the gun aim hook, when on, redirects the bullet
 * itself and is unaffected: it turns the launch matrix from whatever direction the game made). The ring
 * pushes the new id in the very step that starts the move, so H1 of that frame already sees it. */
static int fp_attack_update(const FpChain *c) {
    uintptr_t bag, tam;
    int on = 0;
    if (rd_ptr(c->player + 0x3B0, &bag) && rd_ptr(bag + 0x18, &tam) && fp_readable(tam, 0xC8)) {
        unsigned char module[0xC8];
        memcpy(module, (const void *)tam, sizeof(module));
        int32_t anim, head;
        if (vr_dash_ring_newest(module, &anim, &head, NULL) && anim >= 0) {
            const int sub = anim % 1000000;
            on = sub >= 30000 && sub < 52000 && sub != 33300 && sub != 33310; /* not the L1 trick transform */
            if (on != fp.attack_on && fp.log)
                fprintf(stderr, "VR FP: face=attack: %s (anim %d)\n", on ? "attack starts: facing the view" : "attack over: the game's facing", anim);
        }
    }
    fp.attack_on = on;
    return on;
}

/* face_dodge=strafe (H1, after the game's step, before the yaw write): the body faces the view during the dodge, so
 * the game's root motion went along the view; the horizontal position delta of this step is rotated about the
 * feet onto the travel heading captured at the start (the left stick against the move heading, neutral: straight
 * back; a backstep, quickstep or any non-roll keeps the game's own motion). The delta is |c.pos - the position
 * the previous frame ended with|, or against the game's own previous output, whichever is smaller (whether the
 * game integrates from the position we wrote or from its own). Writing +0x1E0 bypasses collision: a wall in
 * the new direction is not seen (the game's collision ran along the old direction). */
static void fp_strafe(FpChain *c, VrVec3 *world, VrVec3 eye, float root_dx, float root_dz, int dodge_on, uint64_t now) {
    (void)now;
    if (!dodge_on || !fp.dodge_roll) {
        if (fp.strafe_on && fp.log) fprintf(stderr, "VR FP: strafe: over after %d frames (%d kept the game's delta)\n", fp.strafe_frames, fp.strafe_kept);
        fp.strafe_on = 0;
        fp.dodge_fresh = 0;
        fp.strafe_dash = NAN;
        return;
    }
    const float mv = fp.turn_out.move_yaw;
    if (fp.dodge_fresh) {
        fp.dodge_fresh = 0;
        const int fresh_pad = fp_now_us() - atomic_load_explicit(&fp_pad_time_us, memory_order_acquire) < 200000;
        const float sx = fresh_pad ? u2f(atomic_load_explicit(&fp_pad_bits[1], memory_order_relaxed)) : 0.0f;
        const float sy = fresh_pad ? u2f(atomic_load_explicit(&fp_pad_bits[2], memory_order_relaxed)) : 0.0f;
        const float a = sx * sx + sy * sy > 0.3f * 0.3f ? atan2f(sx, sy) : VR_PI; /* neutral: back */
        fp.dodge_travel = vr_wrap_pi(mv + a);
        fp.strafe_on = 1;
        fp.strafe_have = 0;
        fp.strafe_frames = fp.strafe_kept = 0;
        fp.strafe_dash = vr_wrap_pi(fp.dodge_travel - fp.dodge_tdeg * VR_DEG);
        if (fp.log) fprintf(stderr, "VR FP: strafe: stick (%.2f %.2f) -> travel heading %.1f deg (move %.1f deg)\n", sx, sy, fp.dodge_travel / VR_DEG, mv / VR_DEG);
    }
    if (!fp.strafe_on || !c->phys || !fp_readable(c->phys + 0x1E0, 16)) return;
    const float gx = c->pos.x, gz = c->pos.z;
    float bx = fp.strafe_base[0], bz = fp.strafe_base[1];
    float dx = gx - bx, dz = gz - bz;
    if (fp.strafe_have) { /* against the game's own last output too */
        const float ex = gx - fp.strafe_game[0], ez = gz - fp.strafe_game[1];
        if (ex * ex + ez * ez < dx * dx + dz * dz) { dx = ex; dz = ez; bx = fp.strafe_game[0]; bz = fp.strafe_game[1]; }
    } else { bx = gx; bz = gz; dx = dz = 0.0f; }
    fp.strafe_game[0] = gx;
    fp.strafe_game[1] = gz;
    float nx = gx, nz = gz;
    const float len = sqrtf(dx * dx + dz * dz);
    if (fp.strafe_have && len > 1e-5f && len < 1.0f) {
        /* the game turns the body toward the stick during a roll (a back roll up to ~180 deg), so only the step's
         * length is trusted; its direction is set to the captured travel heading */
        float sx, sz;
        vr_strafe_step(dx, dz, fp.dodge_travel, &sx, &sz);
        nx = bx + sx;
        nz = bz + sz;
        volatile float *p = (volatile float *)(c->phys + 0x1E0);
        p[0] = nx;
        p[2] = nz;
        c->pos.x = nx;
        c->pos.z = nz;
        *world = vr_model_to_world(c->pos, c->yaw, eye);
        world->x += root_dx;
        world->z += root_dz;
        ++fp.strafe_frames;
    } else ++fp.strafe_kept;
    fp.strafe_have = 1;
    fp.strafe_base[0] = nx;
    fp.strafe_base[1] = nz;
}

int vr_fp_frame(const BbHookContext *ctx, const float game[4][4], int head_valid, VrQuat head_rel, float ref_yaw,
                VrFpView *out) {
    fp_init();
    atomic_store_explicit(&fp_frame_active, 0, memory_order_relaxed);
    if (!fp.enabled) return 0;
    fp.image = (unsigned char *)(uintptr_t)(ctx->rip - (VA_H1_SITE - VA_BASE));
    const uint64_t now = fp_now_us();
    const float dt = fp.last_h1 ? (float)(now - fp.last_h1) * 1e-6f : 0.0f;
    fp.last_h1 = now;
    const int logging = fp.log && now >= fp.next_log;
    if (logging) fp.next_log = now + 1000000;
    fp_poll_control(now);
    /* This frame's game camera heading; the hook turned it last frame when it is on. */
    const float heading = vr_cam_heading(game);
    const int was_synced = atomic_load_explicit(&fp_sync_on, memory_order_relaxed);
    int active = 0;
    FpChain c;
    memset(&c, 0, sizeof(c));
    VrVec3 eye_raw = vr_v3(0, 0, 0), eye = eye_raw, world = eye_raw;
    int have_eye = 0;
    float root_dx = 0.0f, root_dz = 0.0f;
    fp.body_ok = 0;
    fp.dash_offset = vr_v3(0, 0, 0);
    do {
        /* Only the field camera: a cutscene (Remo, SprjCamera+0x18) or the debug camera (+0x20)
         * keeps the game's third person. SprjCameraStep (0x1e08e00) copies the selected camera
         * into the active one (+8), which H1's PersCam copies, and clears its selection flags
         * (+0x30) afterwards, so compare the data: the field camera's matrix is this frame's
         * camera only while the field camera is selected (findings/16). */
        uintptr_t sc, cam_field;
        if (!rd_ptr(va(VA_SPRJ_CAMERA), &sc) || !rd_ptr(sc + 0x10, &cam_field) || !fp_readable(cam_field + 0x10, 64)) {
            fp.state = "no SprjCamera";
            break;
        }
        int field = 1;
        const float *fm = (const float *)(cam_field + 0x10);
        for (int i = 0; i < 16; ++i) field &= fabsf(fm[i] - game[i / 4][i % 4]) <= 1e-4f;
        if (!fp_read_chain(&c)) break;
        fp.body_ok = 1;
        fp.body_pos = c.pos;
        fp.body_yaw = c.yaw;
        /* The eye in model space (relative to the feet, the character's frame). */
        VrXform head;
        if (c.pose_ok && fp_model_bone(&c, fp.head_bone, &head, 0)) {
            eye_raw = vr_xf_apply(head, fp.eye_offset);
            have_eye = isfinite(eye_raw.x) && isfinite(eye_raw.y) && isfinite(eye_raw.z) &&
                       fabsf(eye_raw.x) < 3.0f && eye_raw.y > -1.0f && eye_raw.y < 4.0f && fabsf(eye_raw.z) < 3.0f;
        }
        const VrVec3 body = vr_v3(0.0f, fp.eye_height, FP_EYE_BIND_Z); /* standing, no bob */
        if (!have_eye) eye_raw = body;
        if (fp.anchor == FP_ANCHOR_BODY) eye = body;
        else if (fp.anchor == FP_ANCHOR_RAW) eye = eye_raw;
        else {
            eye = vr_smooth_step(&fp.smooth_state, eye_raw, dt, fp.smooth, FP_MAX_DEV);
            if (fp.anchor == FP_ANCHOR_STABLE) eye = vr_v3(0.0f, eye.y, 0.0f); /* above the feet: no lean, no swing */
        }
        /* anim_lock: the eye fixed above the root (no head bone: no bob, flinch, lean; x = z = 0 so the
         * root's animated yaw cannot swing it either). The root position itself is eased below. */
        if (fp.anim_lock >= 1) eye = vr_v3(0.0f, fp.eye_height, 0.0f);
        if (fp.floor_frac > 0.0f && eye.y < fp.floor_frac * fp.eye_height) eye.y = fp.floor_frac * fp.eye_height;
        world = vr_model_to_world(c.pos, c.yaw, eye);
        if (fp.anim_lock >= 2) {
            VrVec3 root = c.pos;
            anim_ease_root(dt, &root);
            root_dx = root.x - c.pos.x;
            root_dz = root.z - c.pos.z;
            world.x += root_dx;
            world.z += root_dz;
        } else fp.anim_state.valid = 0;
        if (!field) { fp.state = "cutscene or debug camera"; break; }
        /* Downed (death, knockdown): the raw eye low for a while -> third person. */
        fp.down_time = have_eye && eye_raw.y < FP_DOWN_HEIGHT ? fp.down_time + dt : 0.0f;
        if (fp.down_s > 0.0f && fp.down_time > fp.down_s) { fp.state = "downed"; break; }
        if (!fp.on) { fp.state = "off (control file)"; break; }
        active = 1;
    } while (0);

    /* The hunter's own model, hidden while the view is at its eyes. */
    fp.hidden = 0;
    if (c.player) {
        if (fp.log) fp_parts_dump(c.player);
        const int hiding = fp.hide && (active || fp.hide_tp);
        if (hiding) fp.hidden = fp_hide_apply(c.player, fp.hide);
        fp_skin_mask(c.player, !hiding ? 0
                     : ((fp.hide & FP_HIDE_SKIN_HEAD) ? FP_SKIN_HEAD_GROUPS : 0) |
                       ((fp.hide & FP_HIDE_SKIN_BODY) ? FP_SKIN_BODY_GROUPS : 0));
    }
    const int was_locked = fp.locked;
    fp.locked = c.player ? fp_lock_state(&c, &fp.target_yaw, &fp.target_valid) : 0;
    if (fp.locked != was_locked && fp.log)
        fprintf(stderr, "VR FP: lock-on %s%s\n", fp.locked ? "on" : "off",
                fp.locked && !fp.target_valid ? " (no target point)" : "");

    if (active) {
        /* Heading psi (vr_turn_math.h): our own, from where the game camera looked on entering
         * first person (a short drop-out keeps it); with turn=game the game camera's own. */
        const int hooked = atomic_load_explicit(&fp_cam_fired, memory_order_relaxed);
        const float own = was_synced && hooked ? u2f(atomic_load_explicit(&fp_cam_own_bits, memory_order_relaxed)) : heading;
        if (!fp.was_active && (!fp.turn_state.valid || !fp.inactive_since || now - fp.inactive_since > 500000))
            vrt_reset(&fp.turn_state, heading);
        VrTurnConfig cfg = fp.turn;
        if (fp.turn_game) {
            fp.turn_state.psi = vrt_wrap(own);
            cfg.turn = VRT_TURN_OFF;
        }
        fp.head_yaw = head_valid ? vr_rel_heading(head_rel) : 0.0f;
        fp.ref_yaw = head_valid ? ref_yaw : fp.turn_state.ref_yaw; /* no head: no recenter */
        VrTurnInput in;
        vrt_input_clear(&in);
        in.dt = dt;
        const int pad_fresh = now - atomic_load_explicit(&fp_pad_time_us, memory_order_acquire) < 200000;
        in.stick_x = pad_fresh ? u2f(atomic_load_explicit(&fp_pad_bits[0], memory_order_relaxed)) : 0.0f;
        in.head_yaw = fp.head_yaw;
        in.ref_yaw = fp.ref_yaw;
        in.locked = fp.locked;
        in.target_valid = fp.target_valid;
        in.target_yaw = fp.target_yaw;
        vrt_update(&fp.turn_state, &cfg, &in, &fp.turn_out);
        fp.psi = fp.turn_out.psi;
        if (fp.log && fp.turn_out.snapped) fprintf(stderr, "VR FP: snap turn %+d -> heading %.1f deg\n", fp.turn_out.snapped, fp.psi / VR_DEG);
        if (fp.log && fp.turn_out.recentered)
            fprintf(stderr, "VR FP: recentered: body heading %.1f deg (%s)\n", fp.psi / VR_DEG,
                    cfg.recenter_keeps_view ? "the view stays" : "the view returns to the body heading");
        fp.was_active = 1;
        const float target = fp.turn_out.move_yaw;
        const int sync = fp.sync && fp.turn_out.sync_game && (!fp.turn_game || cfg.move != VRT_MOVE_BODY);
        atomic_store_explicit(&fp_target_bits, f2u(target), memory_order_relaxed);
        atomic_store_explicit(&fp_pivot_bits[0], f2u(c.pos.x), memory_order_relaxed);
        atomic_store_explicit(&fp_pivot_bits[1], f2u(c.pos.z), memory_order_relaxed);
        /* The left stick: the game walks along the follow camera's own heading (the director's
         * output before the hook turns it), which lags the target after turning (6 deg after a
         * smooth turn, findings/16 9.3): the stick takes the rest. Without the hook, all of it. */
        const float game_walk = sync && hooked ? u2f(atomic_load_explicit(&fp_cam_own_bits, memory_order_relaxed)) : heading;
        atomic_store_explicit(&fp_remap_bits, f2u(fp.remap ? vr_wrap_pi(target - game_walk) : 0.0f), memory_order_relaxed);
        atomic_store_explicit(&fp_sync_on, sync, memory_order_relaxed);
        atomic_store_explicit(&fp_stick_to_game, fp.turn_game || cfg.turn == VRT_TURN_OFF || fp.turn_out.stick_to_game,
                              memory_order_relaxed);
        atomic_store_explicit(&fp_active_until_us, now + 250000, memory_order_release);
        atomic_store_explicit(&fp_frame_active, 1, memory_order_relaxed);
        /* face: the body glued to the view. This runs in H1, after the game's step of this frame (its
         * turn-toward-stick, attack tracking, root-motion rotation have all run), so ours is the last
         * word the render sees and the first the next step reads. The camera uses psi, never the body
         * yaw, so there is no feedback; the eye (anchor head) is recomputed with the new yaw. */
        fp.phys_now = c.phys;
        fp.dodge_live = 0;
        if (fp.face && c.phys && (!fp.locked || fp.face_lockon)) {
            const float want0 = vr_wrap_pi(fp.turn_out.view_yaw - VR_PI); /* the hunter faces yaw + pi */
            const float dev = fabsf(vr_wrap_pi(c.yaw - fp.face_last));
            const int attack_mode = fp.face == 4;
            const int dodge_on = (fp.face_dodge != 1 || attack_mode) && fp_dodge_update(&c, now);
            const int attack_on = attack_mode && fp_attack_update(&c);
            /* face=attack: the view only during an attack (and a dodge when face_dodge asks for it); otherwise the
             * game's own facing (locomotion turns the body toward the stick: overriding it made the stick dead) */
            const int idle = attack_mode && !attack_on && !(dodge_on && fp.face_dodge != 0);
            const int dodging = (fp.face_dodge == 0 && dodge_on) || idle;
            if (fp.face_dodge == 2) fp_strafe(&c, &world, eye, root_dx, root_dz, dodge_on, now);
            if (fp.face_writes && fp.face_pending_ok && !dodging && dev > 1.4f && fp.log && now >= fp.next_flip_log) {
                fp.next_flip_log = now + 1000000;
                fprintf(stderr, "VR FP: face: the game turned the body by %.0f deg since our write (anim %d, dodge %d); the render keeps ours\n",
                        dev / VR_DEG, fp.dodge_anim, fp.dodge_on);
            }
            if (fp.face_writes) { /* what the game did to our last write */
                fp.face_dev = dev;
                if (dev > fp.face_dev_max) fp.face_dev_max = dev;
            }
            if (dodging) {
                /* the game's facing: rolls, quicksteps and backsteps go along the stick; the arms use the live yaw */
                fp.dodge_live = 1;
                fp.face_pending_ok = 0;
                fp.face_writes++;
                fp.face_last = c.yaw;
                /* where the body is now, relative to the view (attack mode: snap, no ease, at the next attack) */
                fp.ease_off = attack_mode ? 0.0f : vr_wrap_pi(c.yaw - want0);
            } else if (isfinite(want0) && fp_readable(c.phys + 0x1D4, 4)) {
                float w = want0;
                if (fp.ease_off != 0.0f) { /* back to the view after a dodge */
                    fp.ease_off -= fp.ease_off * fminf(dt / face_ease_s, 1.0f);
                    if (dt <= 0.0f || fabsf(fp.ease_off) < 0.01f) fp.ease_off = 0.0f;
                    w = vr_wrap_pi(want0 + fp.ease_off);
                }
                /* written one frame late: this frame renders what the arms' snapshot of the last H1 assumed */
                const float x = fp.face_pending_ok ? fp.face_pending : w;
                *(volatile float *)(c.phys + 0x1D4) = x;
                if (fp.log && !fp.face_writes) fprintf(stderr, "VR FP: face %s: yaw %.1f -> %.1f deg\n", face_name(fp.face), c.yaw / VR_DEG, x / VR_DEG);
                fp.face_pending = w;
                fp.face_pending_ok = 1;
                fp.face_last = x;
                fp.face_writes++;
                c.yaw = x;
                world = vr_model_to_world(c.pos, c.yaw, eye);
                world.x += root_dx;
                world.z += root_dz;
                fp.body_yaw = w; /* the arms' snapshot: the yaw of the next render (vr_arms_view) */
            }
        } else { fp.attack_on = 0; fp.face_writes = 0; fp.face_pending_ok = 0; fp.dodge_on = 0; fp.dodge_seen = 0; fp.ease_off = 0.0f; fp.strafe_on = 0; }
    } else {
        if (fp.was_active) fp.inactive_since = now;
        fp.was_active = 0;
        fp.smooth_state.valid = 0;
        fp.anim_state.valid = 0;
        atomic_store_explicit(&fp_sync_on, 0, memory_order_relaxed);
        atomic_store_explicit(&fp_stick_to_game, 1, memory_order_relaxed);
        memset(&fp.turn_out, 0, sizeof(fp.turn_out));
        fp.turn_out.psi = fp.psi;
    }

    if (fp.trace && fp.trace_bytes < FP_TRACE_MAX) {
        const VrTurnOutput *o = &fp.turn_out;
        const int n = fprintf(fp.trace, "%.4f,%d,%.4f,%.4f,%.4f,%.5f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.6f,%.6f,%.6f,"
                              "%.7f,%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%.6f,%.6f,%.5f,%d,%d,%d,%d,%d,%d,%.6f,%s,%s,%d\n",
                              (double)(now - fp.start) * 1e-6, active, c.pos.x, c.pos.y, c.pos.z, c.yaw, eye_raw.x, eye_raw.y,
                              eye_raw.z, eye.x, eye.y, eye.z, world.x, world.y, world.z, game[3][0], game[3][1], game[3][2], heading,
                              u2f(atomic_load_explicit(&fp_cam_own_bits, memory_order_relaxed)), fp.psi,
                              dt, u2f(atomic_load_explicit(&fp_pad_bits[0], memory_order_relaxed)),
                              u2f(atomic_load_explicit(&fp_pad_bits[1], memory_order_relaxed)),
                              u2f(atomic_load_explicit(&fp_pad_bits[2], memory_order_relaxed)), active ? fp.head_yaw : 0.0f,
                              active ? fp.ref_yaw : 0.0f, o->view_yaw, o->move_yaw, o->turn_delta, o->turn_rate, o->snapped,
                              o->recentered, active && atomic_load_explicit(&fp_sync_on, memory_order_relaxed),
                              atomic_load_explicit(&fp_stick_to_game, memory_order_relaxed), fp.locked, fp.target_valid,
                              fp.target_valid ? fp.target_yaw : 0.0f, fp.turn_game ? "game" : vrt_turn_name(fp.turn.turn),
                              vrt_move_name(fp.turn.move), fp.hidden);
        if (n > 0) fp.trace_bytes += (uint64_t)n;
    }
    if (!active) {
        if (logging) fp_log_frame(&c, have_eye ? &eye_raw : NULL, &eye, &world, game, heading, 0);
        return 0;
    }

    memset(out, 0, sizeof(*out));
    VrVec3 cam = world;
    if (vr_dash_enabled()) { /* findings/19: a dodge, backstep or jump slides instead of tumbling */
        float p[3] = {cam.x, cam.y, cam.z};
        vr_dash_apply((uint64_t)c.player, fp.strafe_on && isfinite(fp.strafe_dash) ? fp.strafe_dash : vr_facing_heading(c.yaw), p);
        cam = vr_v3(p[0], p[1], p[2]);
        fp.dash_offset = vr_sub(cam, world);
    }
    if (fp.forward != 0.0f || fp.up != 0.0f)
        cam = vr_add(cam, vr_v3(sinf(fp.psi) * fp.forward, fp.up, cosf(fp.psi) * fp.forward));
    vr_cam_from_heading(out->rows, fp.psi, cam);
    out->near_plane = fp.near_plane;
    out->keep_game_rotation = !head_valid && fp.turn_game;
    if (logging) fp_log_frame(&c, &eye_raw, &eye, &world, game, heading, 1);
    return 1;
}

void vr_fp_register(void) {
    bb_hook_set_fp_impl(fp_hook_cam);
    runtime_pad_set_filter(fp_pad_filter);
}
