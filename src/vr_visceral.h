/* VR visceral (src/runtime_vr_visceral.c, docs/vr/findings/41-parry.md section 7): after a gun shot
 * that has a lock-on target (a parry candidate), R1 is pressed for the player when the right hand
 * thrusts at the enemy or stands close (BB_VR_VISCERAL_MODE=auto|thrust|swing), so the game's own
 * riposte/visceral runs. The logic is src/vr_visceral_math.h. */
#ifndef VR_VISCERAL_H
#define VR_VISCERAL_H
#include <stdint.h>

/* Reads BB_VR_VISCERAL_* and prints the config line (next to vr_gun_register). */
void vr_visceral_register(void);
/* 1 while the mode is auto or thrust (the launch hook must notify the shots). */
int vr_visceral_active(void);
/* Game thread, the launch hook: a player firearm shot. how 0: it had no target, 1: the lock-on
 * target (world point), 2: aimed at a lock-on candidate with the controller ray. dist_m: horizontal
 * distance from the muzzle to the target. */
void vr_visceral_on_shot(int how, const float target[3], float dist_m);
/* Game thread, the player's hit hook: a character was hit by the player at `point` (world). */
void vr_visceral_note_hit(int atk_id, const float point[3]);
/* Pad thread, once per pad sample: the per-frame step. Returns the buttons to OR into the pad
 * (BTN_R1 0x800 while the press lasts). */
uint32_t vr_visceral_pad(void);

#endif
