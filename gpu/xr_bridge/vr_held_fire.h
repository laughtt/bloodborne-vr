/* VR held-item fire block (BB_VR_HELD_BLOCK_FIRE, default on): while a hand holds an item (a vial, a molotov, a
 * paper, a bell...: from the held-item grip until it is used, thrown or put back, plus a tail) that hand's attack
 * inputs do nothing, so squeezing the grip on it or moving it toward the target is never a shot or a swing.
 *   left hand holds   L1 (transform) and L2 (the gun, the thrust shoot and the left trigger) and the left
 *                     weapon swap (d-pad left: the X button)
 *   right hand holds  R1 and R2 (the swing gestures, the heavy modifier and the charged R2) and the right
 *                     grip tap's and the A button's weapon swap (d-pad right)
 * The other hand keeps all of its inputs. The auto-visceral's R1 (src/runtime_vr_visceral.c) is not an input of
 * that hand but a send of the host: it is dropped only while the RIGHT hand holds an item (vhf_visceral_ok).
 * Header only, plain C (the bridge, src/ and tests/test_vr_held_fire.c share it). The bit values are the
 * bridge's BBXR_BTN_* (checked by a static_assert in gpu/xr_bridge/vr_held_items.h). */
#ifndef VR_HELD_FIRE_H
#define VR_HELD_FIRE_H
#include <stdint.h>

#define VHF_BTN_RIGHT 0x20u /* d-pad right: the right-hand weapon swap (right grip tap, the A button) */
#define VHF_BTN_LEFT 0x80u  /* d-pad left: the left-hand weapon swap (left trigger in switch mode, the X button) */
#define VHF_BTN_L2 0x100u
#define VHF_BTN_R2 0x200u
#define VHF_BTN_L1 0x400u
#define VHF_BTN_R1 0x800u

#define VHF_TAIL_S 0.3 /* the attack inputs stay off this long after the item left the hand (at least) */

/* The attack buttons of a hand (0 left, 1 right); 0 for any other value. */
static inline uint32_t vhf_attack_buttons(int hand) {
    return hand == 0 ? (VHF_BTN_L1 | VHF_BTN_L2) : hand == 1 ? (VHF_BTN_R1 | VHF_BTN_R2) : 0u;
}

/* The pad buttons hidden from the game right now.
 *   enabled      BB_VR_HELD_BLOCK_FIRE (0: the older behaviour, R1 R2 d-pad-right and, for the left hand, L1)
 *   holding      an item is in a hand (Pending or Held)
 *   hand         the item's hand
 *   released_at  when the item left the hand (any time in the past when it never did)
 *   tail_s       VHF_TAIL_S; the layout's longer lockout_s for the swing and swap buttons wins */
static inline uint32_t vhf_block(int enabled, int holding, int hand, double now, double released_at, double tail_s,
                                 double lockout_s) {
    const double window = tail_s > lockout_s ? tail_s : lockout_s;
    if (!holding && !(now - released_at < window)) return 0u;
    if (!enabled) return VHF_BTN_R1 | VHF_BTN_R2 | VHF_BTN_RIGHT | (hand == 0 ? (VHF_BTN_L1 | VHF_BTN_LEFT) : 0u);
    return vhf_attack_buttons(hand) | (hand == 1 ? VHF_BTN_RIGHT : hand == 0 ? VHF_BTN_LEFT : 0u);
}

/* The host's auto-visceral R1 may be sent: not while the right hand holds an item (or within the tail). */
static inline int vhf_visceral_ok(int enabled, int held_hand_now) {
    return !(enabled && held_hand_now == 1);
}
#endif
