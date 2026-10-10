/* VR arms in the game (BB_VR_ARMS=1, src/runtime_vr_arms.c): the hunter's arm bones posed by the
 * shared IK (src/vr_arm_math.h) after the animation, before the skinning copy. See
 * docs/vr/findings/24-arms-ik.md. */
#ifndef VR_ARMS_H
#define VR_ARMS_H

/* H1 (first person only), once per rendered frame: the final first-person camera (rows right, up,
 * forward, position; world, before the stereo eye offset) and the head pose it shows (OpenXR, the
 * space of bbgpu_vr_views, or the fake head; head_valid 0: none). The character's position and yaw
 * come from vr_fp_body (the same H1). */
void vr_arms_view(const float rows[4][4], int head_valid, const float head_pos[3], const float head_rot[4]);
/* Registers the AnimationToModel hooks' handlers ("bb_hook_vr_arms_a1"/"_a2"). */
void vr_arms_register(void);

#endif
