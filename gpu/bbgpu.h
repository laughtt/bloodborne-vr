/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Co-op: the game folder and param.sfo identity for the network and PSN libraries. */
void bbnet_configure(const char *app0, const char *serial, const char *title, const char *app_ver);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* VR (BB_VR=1): the headset controllers as the PS4 pad, motion gestures included.
 * Returns 1 while they are active; buttons use the pad bit layout, sticks 128 = centered. */
typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;
} BbVrPad;
int bbgpu_vr_pad(BbVrPad *out);
/* Game rumble to the controllers. */
void bbgpu_vr_rumble(uint8_t large, uint8_t small);
/* VR head tracking (Phase 2 groundwork): the head and eye poses for the headset's latest
 * predicted display time, in the world space of the headset screen (meters; orientation
 * x, y, z, w), each eye's FOV (radians; left and down negative) and the per-eye image size
 * the runtime recommends. flags: 1 orientation valid, 2 position valid, 4 orientation tracked,
 * 8 position tracked. Returns 1 while the head orientation is valid (else *out is zeroed). */
typedef struct {
    float position[3];
    float orientation[4];
} BbVrPose;
typedef struct {
    BbVrPose pose;
    float fov_left, fov_right, fov_up, fov_down;
    uint32_t width, height;
} BbVrEye;
typedef struct {
    int64_t display_time_ns;
    uint32_t flags;
    BbVrPose head;
    BbVrEye eye[2]; /* left, right; never assume parallel views (Pimax) */
    /* The cinema screen as placed: the user's "forward", placed again on a recenter (left stick
     * click held). screen_flags: 1 placed (pose valid), 2 head-locked (BB_XR_HEADLOCK=1). */
    BbVrPose screen;
    uint32_t screen_flags;
} BbVrViews;
int bbgpu_vr_views(BbVrViews *out);
/* VR controllers' grip poses of the latest pad poll (bbgpu_vr_pad), with the head of the same
 * snapshot: all in one bridge space (LOCAL), so hand-relative-to-head is exact. valid: bit 0 left
 * grip, 1 right grip, 2 head, 3 left aim, 4 right aim. time_ns: CLOCK_MONOTONIC-like steady clock
 * at capture. aim: the pointing ray (its -Z), as OpenXR's aim pose. Returns 1 when a snapshot
 * exists. */
typedef struct {
    BbVrPose head, grip[2];
    uint32_t valid;
    int64_t time_ns;
    BbVrPose aim[2];
} BbVrHands;
int bbgpu_vr_hands(BbVrHands *out);
/* VR head tracking: the head pose and frustum the game camera was just turned to (the H1 camera
 * hook, once per game frame), with the camera's rotation rows (right, up, forward in the world)
 * that pair it with the presented frame. BB_XR_MODE=mono then shows each image with the view it
 * was rendered with (bridge ABI 5). */
typedef struct {
    uint64_t seq;
    int64_t display_time;                        /* BbVrViews.display_time of the pose */
    BbVrPose head;                               /* world space of bbgpu_vr_views */
    float fov_left, fov_right, fov_up, fov_down; /* radians; left and down negative */
    float rows[3][3];
    /* BB_XR_MODE=stereo (bridge ABI 10): the eyes' offsets from the head, in the head's frame
     * (OpenXR axes), taken from the same snapshot as `head`, so the pair's two layer poses come
     * from one xrLocateViews. has_eye_offset 0: none (fake head). */
    uint32_t has_eye_offset;
    float eye_offset[2][3];
} BbVrRenderView;
void bbgpu_vr_set_render_view(const BbVrRenderView *view);
/* The views for another display time (bridge ABI 10 GetViewsAt, XrTime ns as
 * BbVrViews.display_time_ns): like bbgpu_vr_views, predicted for `display_time_ns`. Falls back to
 * the latest views (bbgpu_vr_views) when the bridge cannot or time <= 0. Returns 1 while the head
 * orientation is valid. */
int bbgpu_vr_views_at(int64_t display_time_ns, BbVrViews *out);
/* The release -> display latency the bridge measures (true stereo, ABI 10): ahead_ns is where H1
 * should ask views for, from now: the host's own camera -> release time (measured by the presenter,
 * smoothed) plus the bridge's release -> display time; display_time_ns is that moment as an XrTime
 * (0: the runtime cannot convert time, use bbgpu_vr_views). Returns 1 when measured. */
typedef struct {
    int64_t display_time_ns;   /* the XrTime the image of a camera set up now is displayed at */
    int64_t ahead_ns;          /* display_time_ns minus now */
    int64_t period_ns;         /* the display period */
    float release_to_display_periods;
    float camera_to_release_ms; /* the host's part */
    uint64_t pairs, half_pairs;
} BbVrDisplayAhead;
int bbgpu_vr_display_ahead(BbVrDisplayAhead *out);
/* VR alternate-eye stereo (BB_XR_MODE=alt, docs/vr/findings/22): the camera hook H1 chose `eye`
 * for the frame it just set up and moved the camera there. The presenter recognizes the finished
 * frame by this camera (rows, position, and the eye tag in the projection's x scale) and hands it
 * to that eye's headset swapchain with this pose and frustum. Works without a headset (flat
 * tests: the pairing and its log). */
typedef struct {
    uint64_t seq;
    uint32_t eye;                                /* 0 left, 1 right */
    uint32_t has_pose;                           /* 1: the pose is the headset's (0: fake head) */
    int64_t display_time;                        /* BbVrViews.display_time of the pose */
    BbVrPose pose;                               /* the eye: head orientation, eye position */
    float fov_left, fov_right, fov_up, fov_down; /* the camera's frustum, radians */
    float rows[3][3];                            /* camera right, up, forward (world) */
    float position[3];                           /* camera position (world), at the eye */
    float center[3];                             /* camera position before the eye offset */
} BbVrEyeView;
void bbgpu_vr_set_eye_view(const BbVrEyeView *view);
/* VR alternate-eye stereo: 1 while the game shows a real-time cutscene (Remo) or the debug camera.
 * The camera hooks then leave the game's camera alone and publish no eye view; the presenter sends
 * every finished frame to the flat cinema screen (no 30-frame wait for the mono fallback) until 0. */
void bbgpu_vr_set_cinema(int on);
/* What the presenter gave to each eye so far (the eye schedule's starvation guard). Returns 0
 * while alternate-eye stereo is off. */
typedef struct {
    uint64_t assigned[2];
    int32_t last_eye; /* -1: none yet */
    uint32_t run;     /* frames in a row given to last_eye */
} BbVrStereoStatus;
int bbgpu_vr_stereo_status(BbVrStereoStatus *out);
/* VR UI (BB_VR_UI=1, bridge ABI 8, docs/vr/findings/26-game-ui.md): the game's state for the
 * headset menus and HUD (a BbXrGameState of gpu/xr_bridge/bbport_xr_bridge.h, `size` bytes), the
 * game events (BBXR_EV_*), and the user's choices (BbXrUiAction[capacity]; returns how many).
 * No-ops (0) without VR or an older bridge. */
void bbgpu_vr_ui_state(const void *state, uint32_t size);
void bbgpu_vr_ui_event(uint32_t kind, int32_t a, int32_t b, int32_t c);
uint32_t bbgpu_vr_ui_actions(void *out, uint32_t capacity);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
