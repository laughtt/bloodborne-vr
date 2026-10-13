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
/* The held weapon's bone (side 0: L_Weapon, 1: R_Weapon) as the IK last wrote it, relative to the H1
 * camera that placed it (camera frame <- bone frame, src/vr_gun_math.h vg_weapon_in_cam; x y z w) and
 * its age. 0 when the arms have not written that side (arms off, weight 0, no first person). The gun
 * aim's barrel source (findings/27 section 11). */
int vr_arms_weapon_cam(int side, float q_xyzw[4], unsigned long long *age_us);

#endif
