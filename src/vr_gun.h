/* VR gun aim (BB_VR_GUN_AIM=1, src/runtime_vr_gun.c, docs/vr/findings/27-gun-aim.md): the player's
 * bullets leave along the controller's pointing ray instead of the character's facing. */
#ifndef VR_GUN_H
#define VR_GUN_H

/* H1 (first person only), once per rendered frame: the first-person camera (rows right, up,
 * forward, position; world, before the stereo eye offset), which shows the head of the controller
 * snapshot the shot is aimed with. */
void vr_gun_view(const float rows[4][4]);
/* Reads the environment and registers the bullet launch hook's handler ("bb_hook_vr_gun_aim"). */
void vr_gun_register(void);

#endif
