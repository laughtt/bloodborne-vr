/* VR gun aim (BB_VR_GUN_AIM=1, src/runtime_vr_gun.c, docs/vr/findings/27-gun-aim.md): the player's
 * bullets leave along the controller's pointing ray instead of the character's facing. */
#ifndef VR_GUN_H
#define VR_GUN_H
#include <stdint.h>

/* H1 (first person only), once per rendered frame: the first-person camera (rows right, up,
 * forward, position; world, before the stereo eye offset), which shows the head of the controller
 * snapshot the shot is aimed with. */
void vr_gun_view(const float rows[4][4]);
/* Physical items (findings/39): the headset menus (BBXR_UI_THROW_ORIGIN / _DIR actions, the pad
 * thread) tell where and along what the hand let go, in the head's frame: millimeters and a unit
 * vector times 10000. The DIR call arms the throw mode of the launch hook for a few seconds: the
 * next listed projectile of the player (BB_VR_THROW_IDS) leaves from that hand position along that
 * direction. Both are turned into the world with the latest first-person camera. */
void vr_gun_throw_origin(const int32_t mm[3]);
void vr_gun_throw_dir(const int32_t dir_1e4[3]);
/* Reads the environment and registers the bullet launch hook's handler ("bb_hook_vr_gun_aim"). */
void vr_gun_register(void);

#endif
