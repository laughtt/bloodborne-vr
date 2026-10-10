/* The game's hitbox debug overlay, host side (src/runtime_vr_hitbox.c, docs/vr/findings/29-game-hitboxes.md):
 * the player's pose and hits in the headset's space for the VR UI layer (BB_VR_UI=1). */
#ifndef VR_HITBOX_H
#define VR_HITBOX_H
#include "vr_hitbox_math.h"
#include <stdint.h>

struct BbXrGameDebug;

/* src/runtime_vr_arms.c: the latest first-person frame (camera in model space, the head it showed,
 * the character's position and yaw); 0 without a fresh one with a headset pose. */
int vr_arms_hitbox_view(VhbView *out, uint64_t max_age_us);
/* src/runtime_vr_stamina.c's hit hooks (the game thread): the player's attack touched something
 * (contact 0 character, 1 wall, 2 object); `record` is the hit record (+0x180 the contact point). */
void vr_hitbox_hit(int contact, int32_t atk_id, uintptr_t record);
/* src/runtime_vr_ui.c, every pad poll before the state goes to the bridge: fills (or zeroes) the
 * overlay block. `image` = the guest image (VA 0x400000). */
void vr_hitbox_fill(struct BbXrGameDebug *out, unsigned char *image);
/* src/runtime_vr_hitscale.c (BB_VR_HIT_SCALE=1, the hook "VR Hitbox Scale (attack shape)"): registers
 * the handler; vr_hitscale_hit counts the hit hooks' hits (contact as above) for its per-attack log. */
void vr_hitscale_register(void);
void vr_hitscale_hit(int contact, int32_t atk_id);
/* src/runtime_vr_atkspeed.c (BB_VR_ATK_SPEED / atk_speed=, the hook "VR Attack Speed (chr dt)"): registers the handler. */
void vr_atkspeed_register(void);

#endif
