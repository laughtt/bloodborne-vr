/* Unit tests for gpu/xr_bridge/vr_held_fire.h. Build: ninja -C out/gpu vr-held-fire-test. */
#include "gpu/xr_bridge/vr_held_fire.h"
#include <stdio.h>
static int failures, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)
#define NEVER (-1e9)

int main(void) {
    const uint32_t L = VHF_BTN_L1 | VHF_BTN_L2 | VHF_BTN_LEFT, R = VHF_BTN_R1 | VHF_BTN_R2;
    /* nothing held: nothing blocked, whichever hand was last */
    CHECK(vhf_block(1, 0, 0, 5.0, NEVER, VHF_TAIL_S, 0.8) == 0, "idle left");
    CHECK(vhf_block(1, 0, 1, 5.0, NEVER, VHF_TAIL_S, 0.8) == 0, "idle right");
    /* the left hand holds (the Fire Paper case): L1 + L2 off, the right hand fully free */
    uint32_t m = vhf_block(1, 1, 0, 5.0, NEVER, VHF_TAIL_S, 0.8);
    CHECK((m & L) == L, "left holds: L1 L2 and the left swap (X) blocked (%#x)", (unsigned)m);
    CHECK((m & (R | VHF_BTN_RIGHT)) == 0, "left holds: R1 R2 and the swap stay (%#x)", (unsigned)m);
    /* the right hand holds: R1 R2 and the grip tap's swap off, the left (the gun) free */
    m = vhf_block(1, 1, 1, 5.0, NEVER, VHF_TAIL_S, 0.8);
    CHECK((m & R) == R && (m & VHF_BTN_RIGHT), "right holds: R1 R2 swap blocked (%#x)", (unsigned)m);
    CHECK((m & L) == 0, "right holds: L1 L2 and the left swap free (%#x)", (unsigned)m);
    /* the grip squeeze on the item hand: left grip is L1, right grip is R2 (heavy / charge) and the tap */
    CHECK((vhf_block(1, 1, 0, 0, NEVER, VHF_TAIL_S, 0.8) & VHF_BTN_L1) != 0, "left grip squeeze (L1)");
    CHECK((vhf_block(1, 1, 1, 0, NEVER, VHF_TAIL_S, 0.8) & (VHF_BTN_R2 | VHF_BTN_RIGHT)) == (VHF_BTN_R2 | VHF_BTN_RIGHT),
          "right grip (R2, tap)");
    /* the tail after the item left: at least 300 ms, the layout's longer lockout wins */
    CHECK(vhf_block(1, 0, 0, 10.2, 10.0, VHF_TAIL_S, 0.1) == L, "0.2 s after: still blocked");
    CHECK(vhf_block(1, 0, 0, 10.35, 10.0, VHF_TAIL_S, 0.1) == 0, "0.35 s after, lockout 0.1: free");
    CHECK(vhf_block(1, 0, 0, 10.5, 10.0, VHF_TAIL_S, 0.8) == L, "lockout 0.8 keeps it off at 0.5 s");
    CHECK(vhf_block(1, 0, 1, 10.9, 10.0, VHF_TAIL_S, 0.8) == 0, "free after the lockout");
    /* switched off: the older mask (R1 R2 swap, + L1 for the left hand) and no L2 */
    m = vhf_block(0, 1, 0, 5.0, NEVER, VHF_TAIL_S, 0.8);
    CHECK((m & VHF_BTN_L1) && (m & R) == R && (m & VHF_BTN_RIGHT) && !(m & VHF_BTN_L2), "off: the older mask (%#x)", (unsigned)m);
    m = vhf_block(0, 1, 1, 5.0, NEVER, VHF_TAIL_S, 0.8);
    CHECK((m & R) == R && !(m & VHF_BTN_L1), "off, right hand (%#x)", (unsigned)m);
    /* a bad hand blocks nothing new */
    CHECK(vhf_block(1, 1, -1, 5.0, NEVER, VHF_TAIL_S, 0.8) == 0, "no hand");
    /* the visceral R1: dropped only while the RIGHT hand holds */
    CHECK(vhf_visceral_ok(1, -1) && vhf_visceral_ok(1, 0), "visceral free with nothing / the left hand holding");
    CHECK(!vhf_visceral_ok(1, 1), "visceral off while the right hand holds");
    CHECK(vhf_visceral_ok(0, 1), "visceral free when the block is switched off");
    CHECK(vhf_attack_buttons(0) == (VHF_BTN_L1 | VHF_BTN_L2) && vhf_attack_buttons(1) == R && vhf_attack_buttons(2) == 0, "attack buttons");
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
