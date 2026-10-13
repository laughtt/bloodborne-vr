/* Instant items (BB_VR_INSTANT_ITEMS=1 or instant_items=1 in out/fp.txt; default off), src/runtime_vr_items.c,
 * docs/vr/findings/44-instant-items.md: a held item's gesture uses it at once, with no use animation; a thrown
 * item flies as our own projectile and the game's burst bullet is requested where it lands. Single player only. */
#ifndef VR_ITEMS_H
#define VR_ITEMS_H
#include <stdint.h>

/* The pad thread: whether the instant path is on now (the setting, not online). Polls the control file. */
int vr_items_on(void);
/* Any thread: the last value vr_items_on found (no file access). */
int vr_items_active(void);
/* The pad thread: the release of a thrown item (BBXR_UI_THROW_ORIGIN / _DIR, head frame: mm and a unit
 * vector x 10000), turned into the world with the latest first-person camera. */
void vr_items_throw_origin(const int32_t mm[3]);
void vr_items_throw_dir(const int32_t dir_1e4[3]);
/* The pad thread: a use from the headset (kind BBXR_UI_USE_QUICK with slot / item / hand speed cm/s, or
 * BBXR_UI_USE_ITEM with the item id in `item`). 1: taken (the game thread decides; the answer comes as
 * BBXR_EV_ACTION_DONE, or the action comes back through vr_items_pad_poll for the game's own path);
 * 0: not taken (off, or busy): the caller runs it the usual way. */
int vr_items_take(uint32_t kind, int32_t slot, uint32_t item, int32_t speed_cms);
/* The pad thread, every poll: 1 when a taken action must run the game's own way after all (the item is not
 * one the instant path handles); *kind / *slot / *item / *speed_cms are the action. Also logs the results. */
int vr_items_pad_poll(uint32_t *kind, int32_t *slot, uint32_t *item, int32_t *speed_cms);
/* The game thread (the in-game step hook, src/runtime_vr_ui.c vu_hook_menu): runs the taken uses, flies
 * the projectiles, requests the bursts. `image` is the host address of guest VA 0x400000. */
void vr_items_game_step(unsigned char *image);

#endif
