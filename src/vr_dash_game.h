/* The comfort dash in the game (BB_VR_DASH=1, src/runtime_vr_dash.c): the interface the
 * first-person camera (src/runtime_vr_fp.c) calls. See docs/vr/findings/19-dash-locomotion.md. */
#ifndef VR_DASH_GAME_H
#define VR_DASH_GAME_H
#include <stdint.h>

/* BB_VR_DASH=1 (read once). */
int vr_dash_enabled(void);
/* Once per rendered frame (H1, the game thread) while first person places the camera: `player`
 * is the PlayerIns (its TimeAct ring tells dodges, backsteps and jumps; 0: unknown, then the
 * input-driven fallback), `facing` the heading the character faces (radians, the camera heading's
 * sense; vr_facing_heading(physics yaw) of vr_fp_math.h; NAN: unknown), `anchor` the camera
 * position (world, meters): replaced by the dash's while a move plays, unchanged otherwise. The
 * head pose is composed afterwards, on top. */
void vr_dash_apply(uint64_t player, float facing, float anchor[3]);
/* The pad sampler's thread, every pad read (src/runtime_vr_fp.c's pad filter): the buttons and the
 * left stick, for the input fallback. The dash does not read the bridge's pad itself: that read
 * consumes the gesture events the game's pad needs. */
void vr_dash_pad_sample(uint32_t buttons, uint8_t lx, uint8_t ly);
/* The dash's comfort vignette 0..1 (any thread), for a vignette layer in the bridge. */
float vr_dash_vignette(void);

#endif
