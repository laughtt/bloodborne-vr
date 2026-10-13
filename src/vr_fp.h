/* First-person camera (BB_VR_FP=1, src/runtime_vr_fp.c): the interface the H1/H2 camera hooks
 * (src/runtime_vr_head.c) use. See docs/vr/findings/16-first-person.md. */
#ifndef VR_FP_H
#define VR_FP_H
#include "hooks.h"
#include "vr_head_math.h"

typedef struct {
    float rows[4][4];  /* the base camera: right, up, forward (level, heading psi), position = eye */
    float near_plane;  /* > 0: the near plane to write (hides the hunter's own head) */
    int keep_game_rotation; /* 1: no head pose is composed (flat first person): keep the game's
                               rotation rows and only move the camera to the eye */
} VrFpView;

/* BB_VR_FP=1 (read once). */
int vr_fp_enabled(void);
/* H1, once per rendered frame: `game` is the draw step's PersCam matrix (rows) before any change,
 * `head_valid`/`head_rel` the head-tracking sample (the head relative to its reference, game
 * camera frame; ignored when head_valid is 0), `ref_yaw` that reference's own heading (radians,
 * the camera heading's sense: right positive; it changes on a recenter, vr_turn_math.h). Returns 1
 * when first person applies this frame and fills *out; 0 keeps the third-person camera (cutscene,
 * debug camera, no player, downed). */
int vr_fp_frame(const BbHookContext *ctx, const float game[4][4], int head_valid, VrQuat head_rel,
                float ref_yaw, VrFpView *out);
/* H2: 1 while H1 put this frame's camera into first person (H2 then copies H1's final camera). */
int vr_fp_active(void);
/* 1 while the game shows a real-time cutscene (Remo) or the debug camera, as of H1's last
 * vr_fp_frame (with hysteresis, vr_cutscene_update): the hooks leave the game's camera alone and
 * BB_XR_MODE=alt shows the frames on the cinema screen. */
int vr_fp_cutscene(void);
/* The bridge's held item (BBXR_UI_HELD): hand 0 left, 1 right, -1 none. That hand's weapon models are hidden
 * in first person while held (lapses 5 s after the last refresh); fp.txt held_hide=0 / BB_VR_FP_HELD_HIDE=0 off. */
void vr_fp_held_item(int hand, uint32_t item);
/* The hand whose attack inputs are blocked now (gpu/xr_bridge/vr_held_fire.h): the one holding a bridge item, or the
 * one that let go less than tail_ms ago; -1 none. Any thread. */
int vr_fp_held_fire_hand(unsigned tail_ms);
/* A throw (src/runtime_vr_gun.c, any thread): the body faces `heading` (radians, world, 0 along +Z, +pi/2
 * along +X: vg_yaw_pitch's) from the next first-person frame until `until_us` (CLOCK_MONOTONIC us), whatever
 * the face mode, lock-on or stick, so the game's own throw (along the body) already leaves along the aim.
 * vr_fp_throw_face_end brings the end forward to `until_us` (the bullet launched). */
void vr_fp_throw_face(float heading, uint64_t until_us);
void vr_fp_throw_face_end(uint64_t until_us);
/* The player's position (feet, world) and physics yaw H1 last read (findings/16 2); 0 when none.
 * For the arms (src/runtime_vr_arms.c): the H1 camera in the character's model space. */
int vr_fp_body(VrVec3 *pos, float *yaw);
/* 1 and the game's current physics yaw while a dodge keeps the body at the game's facing (face=view). */
int vr_fp_yaw_live(float *yaw);
/* How far this H1's comfort dash moved the camera away from the eye (world, meters; 0 outside a
 * dash, findings/19 follow mode): the arms move their shoulders by it (findings/24 5.5). */
VrVec3 vr_fp_dash_offset(void);
/* 1 while first person hides the skin's arms (hide= hands, hands_skin, nohands or skinarms), as of H1's last
 * frame: the arms default their arm_cut to it (findings/24 section 9). Any thread. */
int vr_fp_hides_arms(void);
/* Registers the pad filter (snap turn, stick remap) and the follow-camera yaw hook handler. */
void vr_fp_register(void);

#endif
