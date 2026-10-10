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
/* The player's position (feet, world) and physics yaw H1 last read (findings/16 2); 0 when none.
 * For the arms (src/runtime_vr_arms.c): the H1 camera in the character's model space. */
int vr_fp_body(VrVec3 *pos, float *yaw);
/* 1 and the game's current physics yaw while a dodge keeps the body at the game's facing (face=view). */
int vr_fp_yaw_live(float *yaw);
/* How far this H1's comfort dash moved the camera away from the eye (world, meters; 0 outside a
 * dash, findings/19 follow mode): the arms move their shoulders by it (findings/24 5.5). */
VrVec3 vr_fp_dash_offset(void);
/* Registers the pad filter (snap turn, stick remap) and the follow-camera yaw hook handler. */
void vr_fp_register(void);

#endif
