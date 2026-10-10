// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr.dll: the OpenXR side of VR (Pimax, Quest through Link/Virtual Desktop/Steam Link,
// SteamVR). The port loads it at run time like bbport_dlss.dll and talks to it only through this
// C ABI, so the bridge has no game code and can serve other Vulkan hosts too.
//
// Call order:
//   GetApi -> Init -> InstanceExtensions -> (vkCreateInstance) -> PhysicalDevice ->
//   DeviceExtensions -> (vkCreateDevice) -> StartSession -> per frame AcquireFlat/ReleaseFlat,
//   AcquireStereo/ReleaseStereo and/or AcquireEye/ReleaseEye (+ AcquireHud/ReleaseHud or HideHud)
//   -> Shutdown. GetInput, Haptic and
//   GetViews work from any thread after StartSession.
// The bridge runs its own frame loop thread (xrWaitFrame/BeginFrame/EndFrame) at the headset
// rate; the host only hands it finished images.
// A host built against an older ABI can use a newer bridge (the struct only grows); check
// `abi >= ` the version a function appeared in.

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BBPORT_XR_BRIDGE_ABI 8u

/* Pad buttons, the PS4 pad bit layout (sceePadReadState). */
#define BBXR_BTN_L3 0x2u
#define BBXR_BTN_R3 0x4u
#define BBXR_BTN_OPTIONS 0x8u
#define BBXR_BTN_UP 0x10u
#define BBXR_BTN_RIGHT 0x20u
#define BBXR_BTN_DOWN 0x40u
#define BBXR_BTN_LEFT 0x80u
#define BBXR_BTN_L2 0x100u
#define BBXR_BTN_R2 0x200u
#define BBXR_BTN_L1 0x400u
#define BBXR_BTN_R1 0x800u
#define BBXR_BTN_TRIANGLE 0x1000u
#define BBXR_BTN_CIRCLE 0x2000u
#define BBXR_BTN_CROSS 0x4000u
#define BBXR_BTN_SQUARE 0x8000u
#define BBXR_BTN_TOUCHPAD 0x100000u

/* Gestures fired (BbXrInput.gestures, one bit each). */
#define BBXR_GESTURE_SWING 0x1u     /* right hand swing  -> R1 */
#define BBXR_GESTURE_HEAVY 0x2u     /* fast or gripped swing -> R2 */
#define BBXR_GESTURE_SHOOT 0x4u     /* left hand thrust  -> L2 */
#define BBXR_GESTURE_VIAL 0x8u      /* left grip at the mouth -> triangle */

#define BBXR_RAW_Y 0x1u

typedef struct BbXrPose {
    float position[3];    /* meters, LOCAL space unless stated otherwise */
    float orientation[4]; /* x, y, z, w */
    uint32_t valid;
} BbXrPose;

typedef struct BbXrInput {
    uint32_t active;   /* controllers are tracked and the session has focus */
    uint32_t buttons;  /* BBXR_BTN_*, gesture presses included */
    uint8_t lx, ly, rx, ry; /* sticks 0..255, 128 centered, y down */
    uint8_t l2, r2;    /* analog triggers 0..255 */
    uint8_t reserved[2]; /* [0]: BBXR_RAW_Y when the Y button itself is down (not a vial gesture) */
    uint32_t gestures; /* BBXR_GESTURE_* fired since the previous GetInput */
    float hand_speed[2]; /* m/s, 0 left 1 right */
    BbXrPose head, grip[2], aim[2];
} BbXrInput;

/* View state (BbXrViews.flags), the values of XrViewStateFlags. */
#define BBXR_VIEW_ORIENTATION_VALID 0x1u
#define BBXR_VIEW_POSITION_VALID 0x2u
#define BBXR_VIEW_ORIENTATION_TRACKED 0x4u
#define BBXR_VIEW_POSITION_TRACKED 0x8u

typedef struct BbXrFov {
    float left, right, up, down; /* radians from the view direction; left and down are < 0 */
} BbXrFov;

typedef struct BbXrEye {
    BbXrPose pose;
    BbXrFov fov;
    uint32_t width, height; /* the image size the runtime recommends for this eye */
} BbXrEye;

/* The cinema screen as shown (laser pointers aim at this). The pose is the middle of the
 * screen, facing the user along its +Z. Curved (radius > 0): the arc of a cylinder whose axis
 * runs along the pose's +Y, `radius` behind the middle along +Z; width is the arc length. */
typedef struct BbXrScreen {
    BbXrPose pose;        /* valid once the screen is placed (and the head tracked if locked) */
    float width, height;  /* meters */
    float radius;         /* 0 for the flat quad */
    uint32_t head_locked; /* BB_XR_HEADLOCK=1: the pose follows the head */
} BbXrScreen;

/* Where the right controller's aim ray meets the screen (computed every frame; BB_XR_LASER=1
 * also draws the beam and the hit dot). */
typedef struct BbXrPointer {
    uint32_t hit;   /* the ray meets the screen */
    float u, v;     /* the hit in pixels of the image on the screen (v down) */
    float distance; /* meters from the controller */
} BbXrPointer;

/* ABI 3: the head and eyes for the latest predicted display time, in the world space of the
 * screen: LOCAL, or STAGE with BB_XR_STAGE=1 (space 1); BbXrInput poses stay in LOCAL. */
typedef struct BbXrViews {
    int64_t display_time; /* XrTime (ns) the poses are predicted for */
    uint32_t flags;       /* BBXR_VIEW_*, 0 until the views are located */
    uint32_t space;       /* 0 LOCAL, 1 STAGE */
    BbXrPose head;        /* between the eyes (the VIEW space origin) */
    BbXrEye eye[2];       /* 0 left, 1 right; Pimax views are canted: never assume parallel */
    BbXrScreen screen;
    BbXrPointer pointer;
} BbXrViews;

/* ABI 4: app-rendered stereo (sandbox3d now, the game's stereo renderer later).
 *
 * Protocol (one render thread of the host; the bridge's frame loop runs on its own thread):
 *   1. AcquireStereo waits up to request->timeout_ms for a headset frame newer than the one the
 *      last ReleaseStereo was rendered for (this paces the host at the headset rate), then
 *      acquires one color image per eye (and one depth image per eye when asked and the runtime
 *      can take them) and returns them with that frame's predicted views: render each eye with
 *      exactly eye[i].pose (in the world space `space`) and eye[i].fov.
 *   2. The host records and submits its rendering (on the queue it shares with the bridge,
 *      under the BbXrSessionInfo queue lock), then calls ReleaseStereo. Every acquired image
 *      must be released, even when nothing was drawn.
 *   3. From the next headset frame on, the frame loop shows the released images as the
 *      projection layer (behind the cinema screen; it replaces the background layer and
 *      BB_XR_MODE=mono), tagged with the views they were rendered with, so the runtime
 *      reprojects them to the head pose at display time (positionally too when the depth was
 *      handed to it). The last released images keep showing until the next release; after 1 s
 *      without one the background layer comes back.
 * Images: color in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, depth in
 * VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL when acquired; return them in those layouts.
 * The color format is an sRGB format of request->color_format's channel order (write linear
 * values, alpha 1: the Pimax compositor blends by alpha whatever the layer flags say).
 * Thread safety: AcquireStereo/ReleaseStereo from one thread at a time; they take the bridge's
 * state lock briefly (the frame loop holds it while it submits), never the host queue lock
 * except around the runtime's own swapchain calls (as AcquireFlat does). */
typedef struct BbXrStereoRequest {
    uint32_t width, height;  /* per-eye image size; 0 = the runtime's recommended size */
    VkFormat color_format;   /* channel order wanted (e.g. VK_FORMAT_R8G8B8A8_UNORM) */
    uint32_t want_depth;     /* 1: hand out depth images too (see BbXrStereoFrame.depth) */
    uint32_t timeout_ms;     /* how long to wait for the next headset frame; 0 = no wait */
} BbXrStereoRequest;

typedef struct BbXrStereoFrame {
    int64_t display_time; /* XrTime (ns) the views are predicted for */
    uint32_t flags;       /* BBXR_VIEW_* of the views */
    uint32_t space;       /* 0 LOCAL, 1 STAGE: the world space of the eye poses (as GetViews) */
    BbXrEye eye[2];       /* render with these (width/height: the images' size) */
    VkImage color[2];
    VkImage depth[2];     /* VK_NULL_HANDLE: the runtime takes no depth; use your own buffer */
    VkFormat color_format;
    VkFormat depth_format;
    uint32_t width, height;
    uint32_t depth_to_compositor; /* the depth goes to the runtime (XR_KHR_composition_layer_depth) */
    /* The controllers at display_time in `space` (like the eyes; BbXrInput's are LOCAL and a
     * frame older): grip = the hand, aim = the pointing ray along -Z. valid 0 when untracked. */
    BbXrPose grip[2], aim[2];
    float trigger[2], squeeze[2]; /* analog, 0..1 per hand (BbXrInput only has digital ones) */
} BbXrStereoFrame;

typedef struct BbXrStereoSubmit {
    float near_z, far_z; /* the projection's clip distances in meters (for the depth layer) */
} BbXrStereoSubmit;

/* ABI 5: the view a flat image was rendered with (BB_XR_MODE=mono with head tracking). The host
 * turns its camera by the head pose of a GetViews snapshot and gives it a symmetric frustum that
 * covers both eyes, then hands both back with the finished image (ReleaseFlatView). Mono mode
 * shows that image to both eyes with this one orientation and frustum (one image cannot match
 * two different eye frusta: per-eye frusta give double vision), each eye at its own offset from
 * the head, so the runtime reprojects from the pose the image was really rendered with. */
typedef struct BbXrFlatView {
    int64_t display_time; /* the GetViews display_time the pose came from (0 unknown) */
    BbXrPose head;        /* the head pose the camera used, in GetViews' world space; valid 1 */
    BbXrFov fov;          /* the camera's frustum (radians from the view axis; left, down < 0) */
} BbXrFlatView;

/* ABI 6: alternate-eye stereo (BB_XR_MODE=alt, docs/vr/findings/22). The host renders ONE eye per
 * game frame, alternating L, R, L, R, each from its own camera (the eye's position, the head's
 * orientation, a frustum that covers the eye), and hands each finished image to its eye
 * (AcquireEye/ReleaseEye with the view it was rendered with). The projection layer shows each
 * eye's latest image with that eye's own view, so the runtime reprojects the older eye from the
 * pose it was really rendered with. Images that belong to no eye (menus, loading screens) go
 * through AcquireFlat/ReleaseFlat(View) and show as in mono mode; the eyes come back once both
 * have an image newer than the latest flat one. */
typedef struct BbXrEyeView {
    int64_t display_time; /* the GetViews display_time the pose came from (0 unknown) */
    BbXrPose pose;        /* the camera pose the image was rendered from (the eye's position, the
                             camera's orientation) in GetViews' world space; valid 1 */
    BbXrFov fov;          /* the frustum it was rendered with (radians; left, down < 0) */
    uint64_t seq;         /* the host's frame number (logs) */
} BbXrEyeView;

/* ABI 7: the HUD layer (docs/vr/findings/25-hud-layer.md, xr_hud.h). The host draws the game's 2D
 * UI (HUD, subtitles, menus over the scene) into an image of its own, transparent where there is
 * no UI with the color premultiplied by alpha, keeps it out of the eye/flat images, and hands it
 * over per game frame with AcquireHud/ReleaseHud. The bridge shows the latest one as a quad in
 * front of the user (BB_XR_HUD_DIST, _WIDTH_DEG, _PITCH_DEG, _FOLLOW), the same image in both
 * eyes, above the scene and the cinema screen, while it is fresh (250 ms). HideHud: the latest
 * frame has its UI in the image itself (title, loading screens, menus without a scene): no HUD
 * layer until the next ReleaseHud. */

/* ABI 8: the game's VR UI (BB_VR_UI=1; docs/vr/findings/26-game-ui.md, vr_game_ui.h). The bridge
 * draws the menus of out/vr_assets/ui_layouts.json ("game" object: the quick ring, the inventory
 * grid, wrist counters, body slots) and the swing/stamina HUD with the sandbox's code into a
 * transparent stereo layer over the game, and filters the controls (vr_ui_filter.h). The host
 * feeds it the game's state each pad poll (SetGameState), the stamina tickets as they open and
 * close (GameEvent), and carries out what the user chose (TakeUiActions) with the game's own
 * buttons. Item ids: goods 0x40000000 | id, weapons the plain param id (7000000), as the game's
 * inventory stores them. */
#define BBXR_GAME_IN_WORLD 0x1u   /* a player character exists (not title, not loading) */
#define BBXR_GAME_STAMINA 0x2u    /* stamina/stamina_max are valid */
#define BBXR_GAME_QUICK 0x4u      /* quick[] and quick_active are valid */
#define BBXR_GAME_ITEMS 0x8u      /* items[] is valid */
#define BBXR_GAME_WEAPONS 0x10u   /* weapons[] and the active indexes are valid */
#define BBXR_GAME_BUSY 0x20u      /* the host is pressing buttons for an action (TakeUiActions) */
#define BBXR_GAME_CAN_EQUIP 0x40u /* the equip hook runs: EQUIP_ITEM and ASSIGN_QUICK are possible */
#define BBXR_GAME_MENU_OPEN 0x80u /* the game's own menu (pause / Options, inventory...) is open: the filter turns the sticks into d-pad navigation */
#define BBXR_GAME_MAX_QUICK 8u
#define BBXR_GAME_MAX_ITEMS 320u
#define BBXR_ITEM_GOODS 0x40000000u
#define BBXR_ITEM_NONE 0xFFFFFFFFu

typedef struct BbXrGameItem {
    uint32_t id;    /* category bits | param id */
    int32_t count;
} BbXrGameItem;

/* The hitbox debug overlay (docs/vr/findings/29-game-hitboxes.md; the "game" object's
 * "debug_hitboxes"): the player's pose in the headset's space, as 3x4 matrices (column-major: x, y,
 * z columns, then the origin) that take the bbhitbox data's points (out/vr_assets/hitbox, X
 * mirrored) as they are. Filled by src/runtime_vr_hitbox.c; zero from an older host. */
#define BBXR_DEBUG_POSE 0x1u      /* weapon[]/bones[] valid (first person, the camera fresh) */
#define BBXR_DEBUG_MAX_BONES 128u
#define BBXR_DEBUG_MAX_HITS 8u

typedef struct BbXrDebugBone {
    uint32_t name_hash; /* FNV-1a 32 of the bone's name */
    float skin[12];     /* pose . inverse(bind): for points in the body's bind-pose model space */
} BbXrDebugBone;

typedef struct BbXrDebugHit {
    float pos[3];       /* where it landed, headset space */
    float age_s;        /* seconds since */
    int32_t atk_id;     /* AtkParam id */
    uint32_t contact;   /* 0 character, 1 wall, 2 object */
} BbXrDebugHit;

/* The VR hit scale (src/vr_hit_scale.h, docs/vr/findings/32-vr-hitbox-scale.md): radius multiplier
 * and extra reach the overlay draws the weapon shapes with. valid 0 (an older host): the shapes
 * are drawn as the game has them. Per-class values < 0 (reach < -1) mean the global one. */
typedef struct BbXrHitScale {
    uint32_t valid;     /* 1 when the host filled this */
    uint32_t enabled;   /* 0: draw the game's own shapes */
    float radius_scale;
    float reach_m;
    float class_radius_scale[3];  /* short, medium, long */
    float class_reach_m[3];
} BbXrHitScale;

typedef struct BbXrGameDebug {
    uint32_t flags;          /* BBXR_DEBUG_* */
    uint32_t weapon_valid;   /* bit 0 left, bit 1 right */
    float weapon[2][12];     /* L_Weapon / R_Weapon frames: for points in the weapon's model space */
    uint32_t bone_count;
    uint32_t hit_count;
    BbXrDebugHit hits[BBXR_DEBUG_MAX_HITS];   /* the player's recent hits, newest first */
    BbXrDebugBone bones[BBXR_DEBUG_MAX_BONES];
    BbXrHitScale hit_scale;  /* appended after the bones: an older host sends none (valid 0) */
} BbXrGameDebug;

typedef struct BbXrGameState {
    uint32_t size;     /* sizeof(BbXrGameState): the bridge reads only what it knows */
    uint32_t flags;    /* BBXR_GAME_* */
    int32_t stamina, stamina_max;
    int32_t hp, hp_max;
    uint32_t quick_count;           /* quick slots (Bloodborne: 5) */
    uint32_t quick_active;          /* the slot square uses */
    uint32_t quick[BBXR_GAME_MAX_QUICK]; /* item ids, BBXR_ITEM_NONE when empty */
    uint32_t weapons[4];            /* right 1, right 2, left 1, left 2 (BBXR_ITEM_NONE empty) */
    uint32_t weapon_active[2];      /* right slot 0/1, left slot 0/1 */
    uint32_t item_count;
    BbXrGameItem items[BBXR_GAME_MAX_ITEMS];
    BbXrGameDebug debug;            /* the hitbox overlay (appended; an older host sends none) */
} BbXrGameState;

/* Game events (GameEvent). */
#define BBXR_EV_SWING_OPEN 1u   /* a costed attack started its window: a cost, b AtkParam id,
                                   c ticket id */
#define BBXR_EV_SWING_HIT 2u    /* ... it connected and paid: a paid, b contact (0 character, 1
                                   wall, 2 object), c ticket id */
#define BBXR_EV_SWING_MISS 3u   /* ... its window closed without a hit: a the cost saved, c the
                                   ticket id */
#define BBXR_EV_ACTION_DONE 4u  /* an action was carried out: a the action kind, b 1 done / 0
                                   gave up (c the reason: 1 not found, 2 timeout, 3 not allowed,
                                   4 the equip hook is not installed, 5 the game did not take it,
                                   6 a game menu is open) */

typedef struct BbXrGameEvent {
    uint32_t kind;
    int32_t a, b, c;
} BbXrGameEvent;

/* What the user chose (TakeUiActions). */
#define BBXR_UI_USE_QUICK 1u    /* use quick slot a (item b): d-pad down to it, then square */
#define BBXR_UI_USE_ITEM 2u     /* use item a (not in a quick slot) */
#define BBXR_UI_EQUIP 3u        /* make weapon slot a (0 right 1, 1 right 2, 2 left 1, 3 left 2;
                                   item b) the active one: d-pad right/left */
#define BBXR_UI_SELECT_QUICK 4u /* make quick slot a (item b) the current one: d-pad down only */
#define BBXR_UI_PRESS 5u        /* press pad button a (BBXR_BTN_*) once (the dashboard's buttons) */
#define BBXR_UI_EQUIP_ITEM 6u   /* put weapon (or armour) b from the inventory into the game's equip
                                   slot a (0 left 1, 1 right 1, 2 left 2, 3 right 2) with the game's
                                   own equip-from-menu command (the "VR UI Equip" hook) */
#define BBXR_UI_ASSIGN_QUICK 7u /* put goods b into quick slot a (0..5) the same way; c = 1: then use
                                   it (d-pad to it, square) */

#define BBXR_UI_CLEAR_QUICK 8u  /* empty quick slot a (0..5; item b is what the dashboard saw there): the
                                   slot's inventory index is set to -1 on the game thread */

typedef struct BbXrUiAction {
    uint32_t kind;
    int32_t a, b, c;
} BbXrUiAction;

/// level: 0 info, 1 warning, 2 error.
typedef void (*BbXrLogFn)(int level, const char* message);
/// The host's queue lock (the runtime may submit on the shared queue): lock != 0 locks.
typedef void (*BbXrQueueLockFn)(void* user, int lock);

typedef struct BbXrSessionInfo {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t queue_family_index;
    uint32_t queue_index;
    BbXrQueueLockFn queue_lock;
    void* queue_lock_user;
} BbXrSessionInfo;

typedef struct BbXrApi {
    uint32_t abi;
    /// Creates the XrInstance and finds the headset. Returns 1 when a headset is usable.
    int (*Init)(BbXrLogFn log);
    /// Vulkan instance extensions the runtime needs. The strings live until Shutdown.
    uint32_t (*InstanceExtensions)(const char** names, uint32_t capacity);
    /// The GPU the headset is connected to (VK_NULL_HANDLE when unknown).
    VkPhysicalDevice (*PhysicalDevice)(VkInstance instance, PFN_vkGetInstanceProcAddr gipa);
    /// Vulkan device extensions the runtime needs. The strings live until Shutdown.
    uint32_t (*DeviceExtensions)(const char** names, uint32_t capacity);
    /// Creates the session and starts the frame loop thread. Returns 1 on success.
    int (*StartSession)(const BbXrSessionInfo* info);
    /// Cinema mode: hands out the next flat-screen swapchain image (width x height, a format
    /// matching src_format's channel order, sRGB when the source is 8-bit UNORM). The image is
    /// in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL and must be returned in that layout. Returns
    /// 0 when the headset is not showing (then skip ReleaseFlat).
    int (*AcquireFlat)(uint32_t width, uint32_t height, VkFormat src_format, VkImage* image,
                       VkFormat* format);
    /// After the commands that write the acquired image were submitted.
    void (*ReleaseFlat)(void);
    void (*Shutdown)(void);
    /* ABI 2: controllers. */
    /// Latest controller state mapped to the PS4 pad (gestures included). Returns out->active.
    int (*GetInput)(BbXrInput* out);
    /// Vibration: hand 0 left, 1 right, 2 both; amplitude 0..1.
    void (*Haptic)(int hand, float amplitude, float seconds);
    /* ABI 3: head tracking (Phase 2). */
    /// Fills `out` with the latest views (eye image sizes are known from Init on). Returns 1
    /// while the head orientation is valid.
    int (*GetViews)(BbXrViews* out);
    /* ABI 4: app-rendered stereo (protocol above). */
    /// Images and views for the next stereo frame. Returns 1 with `frame` filled, 0 when the
    /// headset is not showing, no newer headset frame came within the timeout, or an image is
    /// not available yet (then skip ReleaseStereo and try again).
    int (*AcquireStereo)(const BbXrStereoRequest* request, BbXrStereoFrame* frame);
    /// After the commands that render the acquired images were submitted.
    void (*ReleaseStereo)(const BbXrStereoSubmit* submit);
    /* ABI 5: mono head tracking. */
    /// ReleaseFlat with the view the released image was rendered with (see BbXrFlatView); null
    /// or an invalid pose: as ReleaseFlat (mono then uses the views current at release).
    void (*ReleaseFlatView)(const BbXrFlatView* view);
    /* ABI 6: alternate-eye stereo (BbXrEyeView). */
    /// As AcquireFlat, for eye 0 (left) or 1 (right): that eye's swapchain image to copy a
    /// finished frame into (COLOR_ATTACHMENT_OPTIMAL in and out). Returns 1 with the image, 0
    /// when the headset is not showing or the image is not free yet (skip ReleaseEye; the eye
    /// keeps its image), -1 when the bridge shows no eye images (not BB_XR_MODE=alt, or the
    /// runtime refused the layer): send every image through AcquireFlat from then on.
    int (*AcquireEye)(uint32_t eye, uint32_t width, uint32_t height, VkFormat src_format,
                      VkImage* image, VkFormat* format);
    /// After the commands writing the acquired image were submitted, with the view the image was
    /// rendered with (null or invalid: the views current at release, logged once).
    void (*ReleaseEye)(uint32_t eye, const BbXrEyeView* view);
    /* ABI 7: the HUD layer. */
    /// As AcquireFlat, for the HUD image (alpha kept: premultiplied). Returns 1 with the image, 0
    /// when the headset is not showing or the image is not free yet (skip ReleaseHud), -1 when the
    /// bridge shows no HUD layer (BB_XR_HUD=off, cinema mode, or the runtime refused the layer):
    /// keep the UI in the game image from then on.
    int (*AcquireHud)(uint32_t width, uint32_t height, VkFormat src_format, VkImage* image,
                      VkFormat* format);
    /// After the commands writing the acquired image were submitted: show it.
    void (*ReleaseHud)(void);
    /// The latest game frame has no separate HUD (its UI is in its image): hide the HUD layer.
    void (*HideHud)(void);
    /* ABI 8: the game's VR UI (BB_VR_UI=1). Any thread; cheap (a copy under a lock). */
    /// The game's state now (a copy is taken; state->size tells the version).
    void (*SetGameState)(const BbXrGameState* state);
    /// Something happened in the game (stamina tickets, actions done).
    void (*GameEvent)(const BbXrGameEvent* event);
    /// The user's choices since the last call, oldest first; returns how many were written.
    uint32_t (*TakeUiActions)(BbXrUiAction* out, uint32_t capacity);
} BbXrApi;

typedef const BbXrApi* (*BbXrGetApiFn)(void);

#ifdef _WIN32
__declspec(dllexport)
#endif
const BbXrApi* BbXrGetApi(void);

#ifdef __cplusplus
}
#endif
