/* The custom character model override, host side (src/runtime_vr_skin.c, BB_VR_SKIN). */
#ifndef VR_SKIN_H
#define VR_SKIN_H
struct BbXrSkinState;
/* src/runtime_vr_ui.c, every pad poll: fills (or zeroes) the other characters' poses in the
 * headset's space. `image` = the guest image (VA 0x400000). */
void vr_skin_fill(struct BbXrSkinState *s, unsigned char *image);
#endif
