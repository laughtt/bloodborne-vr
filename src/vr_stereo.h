/* Alternate-eye stereo (BB_XR_MODE=alt, src/runtime_vr_stereo.c,
 * docs/vr/findings/22-phase3-alt-eye-stereo.md): the interface the H1/H2 camera hooks
 * (src/runtime_vr_head.c) use. */
#ifndef VR_STEREO_H
#define VR_STEREO_H
#include "vr_head_math.h"
#include <stdint.h>

/* BB_XR_MODE=alt (read once). */
int vr_stereo_enabled(void);

/* The head sample the H1 camera was composed with. */
typedef struct {
    int valid;            /* a head pose was composed into the camera this frame */
    int from_headset;     /* 1: bbgpu_vr_views (else the fake head: flat tests) */
    VrQuat head;          /* OpenXR orientation used */
    VrVec3 head_pos;      /* OpenXR position, the views' world space */
    int64_t display_time; /* of the views the sample came from (0: fake) */
    uint64_t frame;       /* the H1 call's frame number (H2 of the same frame passes it again) */
} VrStereoHead;

/* H1, last step of the camera (after the head rotation, the first-person eye, the head position
 * and the FOV/aspect were written): picks this frame's eye, moves the camera to it and publishes
 * the eye's view for the presenter. `cam` is PersCam +0x10: the 16 matrix floats, then FOV,
 * aspect, near, far. Returns the eye (0 left, 1 right), or -1 when stereo is off or does not
 * apply this frame (the camera is unchanged). */
int vr_stereo_h1(float *cam, const VrStereoHead *head);

/* H2 after its camera was composed (third person) or copied from H1's (first person, H1's camera
 * before the eye offset): with BB_VR_STEREO_H2=eye the same eye offset and aspect for the main
 * context's format-B camera (+0x00 matrix, +0x64 aspect), when H1 of the same frame (`frame` =
 * VrStereoHead.frame) moved its camera; by default the main context stays at the center (the LOD
 * selection reads its camera position: both eyes then get the same LOD). */
void vr_stereo_h2(float *cam_fmtb, uint64_t frame);

/* Registers the off-axis projection hooks' handlers (bb_hook_set_stereo_impl). */
void vr_stereo_register(void);

#endif
