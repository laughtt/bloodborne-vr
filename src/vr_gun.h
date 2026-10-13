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
/* The pad thread, right after the DIR call, for a thrown item's use action: the hand's speed at the
 * release relative to the head, cm/s. The projectile's speed is scaled by it (BB_VR_THROW_SPEED). */
void vr_gun_throw_speed(int32_t cm_s);
/* The visceral module (src/runtime_vr_visceral.c, pad thread): the latest first-person camera rows
 * (right, up, forward, position) and its age; 0 when there is none or it is older than 300 ms. */
int vr_gun_view_latest(float rows[4][4], uint64_t *age_us);
/* The lock-on candidates' world points (the enemies the game would let the player lock onto); returns
 * how many were read (0 before the first launch hook call). */
int vr_gun_candidate_points(float (*out)[3], int max);
/* Instant items (src/runtime_vr_items.c, pad thread): a hand position and a direction in the head's frame
 * (mm, unit x 10000, as BBXR_UI_THROW_ORIGIN / _DIR) into the world with the latest first-person camera;
 * 0 when there is no camera of the last second or the direction is degenerate. */
int vr_gun_rel_to_world(const int32_t mm[3], const int32_t dir_1e4[3], float origin[3], float dir[3]);
/* Reads the environment and registers the bullet launch hook's handler ("bb_hook_vr_gun_aim"). */
void vr_gun_register(void);

#endif
