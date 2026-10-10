// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport VR: OpenXR through the optional bbport_xr.dll bridge (gpu/xr_bridge, MIT), loaded at
// run time from next to bb-probe.exe when BB_VR=1. Phase 1 (cinema): the finished window image
// is copied to a flat screen in the headset. Without the DLL or headset nothing changes.

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "../../../xr_bridge/xr_stereo.h"

namespace Vulkan {

class Xr {
public:
    /// The process-wide instance, or null when VR is off, the DLL is absent or no headset.
    static Xr* Get();
    ~Xr();

    /// Instance creation: adds what the OpenXR runtime needs.
    void AppendInstanceExtensions(std::vector<const char*>& enabled);
    /// The GPU the headset is connected to, if the runtime names one.
    std::optional<vk::PhysicalDevice> PhysicalDevice(vk::Instance instance);
    /// Device creation (after PhysicalDevice).
    [[nodiscard]] const std::vector<const char*>& DeviceExtensions() const;
    /// After the device exists: creates the session and starts the headset frame loop.
    void StartSession(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device,
                      u32 queue_family_index);

    /// Present: the next flat-screen image to copy the window image into (layout
    /// eColorAttachmentOptimal in and out), or nothing when the headset is not showing.
    struct FlatTarget {
        vk::Image image;
        vk::Format format;
    };
    std::optional<FlatTarget> AcquireFlat(u32 width, u32 height, vk::Format source);
    /// After the commands writing the acquired image were submitted. `camera_view`: the presented
    /// frame's scene camera (world to view, 3x4 rows, CameraMotion) or null; it pairs the image
    /// with the RenderView its camera was rendered with (mono head tracking, bridge ABI 5).
    void ReleaseFlat(const float* camera_view = nullptr);
    /// Alternate-eye stereo (BB_XR_MODE=alt, bridge ABI 6, vk_xr_stereo.h): the bridge takes
    /// images per eye.
    [[nodiscard]] bool HasEyes() const;
    /// As AcquireFlat, for eye 0 (left) or 1 (right).
    std::optional<FlatTarget> AcquireEye(int eye, u32 width, u32 height, vk::Format source);
    /// After the commands writing the acquired eye image were submitted: with the view (pose,
    /// frustum) the frame was rendered with.
    void ReleaseEye(int eye, const xrstereo::EyeView& view);
    /// The HUD layer (bridge ABI 7, docs/vr/findings/25): the bridge takes the game's UI image
    /// (premultiplied alpha) and shows it as a quad in front of the user.
    [[nodiscard]] bool HasHud() const;
    /// As AcquireFlat, for the HUD image. A bridge that shows no HUD layer turns the separation
    /// off (XrHud::Disable): the UI goes back into the game image.
    std::optional<FlatTarget> AcquireHud(u32 width, u32 height, vk::Format source);
    void ReleaseHud();
    /// This frame carries its UI in its image (no separate HUD): hide the HUD layer.
    void HideHud();
    void Shutdown();
    /// Window closed: orderly OpenXR teardown (once), at most `timeout_ms`; no-op without VR.
    static void ShutdownForExit(int timeout_ms);

    /// VR controllers as the PS4 pad (buttons in the pad's bit layout, gestures included).
    struct Pad {
        u32 buttons;
        u8 lx, ly, rx, ry, l2, r2;
    };
    std::optional<Pad> Input();
    /// The grips of the latest Input() call (same GetInput snapshot: head and grips in the
    /// bridge's LOCAL space, valid bit 0 left, 1 right). Nothing before the first Input().
    struct Hands {
        float head[7], grip[2][7]; // x y z, qx qy qz qw
        u32 valid;                 // bit 0 left grip, bit 1 right grip, bit 2 head, 3/4 left/right aim
        s64 time_ns;               // steady clock when captured
        float aim[2][7];           // the pointing rays (-Z), same layout as grip
    };
    std::optional<Hands> LatestHands();
    /// Game rumble (0..255 motors) to both controllers.
    void Rumble(u8 large, u8 small);
    /// The game's VR UI (bridge ABI 8, BB_VR_UI=1): the game's state (a BbXrGameState of `size`
    /// bytes), a game event, the user's choices (BbXrUiAction[capacity]). No-ops on older bridges.
    void SetGameState(const void* state, u32 size);
    void GameEvent(u32 kind, s32 a, s32 b, s32 c);
    u32 TakeUiActions(void* out, u32 capacity);
    /// The VR UI for the desktop window (bridge ABI 9, BB_XR_MIRROR_UI): the latest finished
    /// image (premultiplied alpha, SHADER_READ_ONLY_OPTIMAL) or nothing. Present thread.
    struct MirrorUi {
        vk::Image image;
        vk::Format view_format; // a UNORM view of the image
        float uv_w, uv_h;       // the used top-left part of the image
    };
    std::optional<MirrorUi> LatestMirrorUi();

    /// Head tracking (Phase 2 groundwork): the head and eyes for the headset's latest predicted
    /// display time, in the world space of the headset screen (meters; orientation x, y, z, w).
    struct Pose {
        float position[3];
        float orientation[4];
    };
    struct Eye {
        Pose pose;
        float fov_left, fov_right, fov_up, fov_down; // radians; left and down are negative
        u32 width, height;                           // recommended image size
    };
    struct Views {
        s64 display_time; // OpenXR time (ns) the poses are predicted for
        u32 flags;        // 1 orientation valid, 2 position valid, 4/8 orientation/position tracked
        Pose head;        // between the eyes
        Eye eyes[2];      // left, right; Pimax views are canted, never assume parallel
        Pose screen;      // the cinema screen as placed (placed again on a recenter)
        u32 screen_flags; // 1 placed (pose valid), 2 head-locked
    };
    /// Nothing while the head orientation is not tracked. Any thread.
    std::optional<Views> LatestViews();

    /// Mono head tracking: the head pose and frustum the game camera was turned to (the H1
    /// hook, once per game frame), and the camera's rotation rows (right, up, forward in the
    /// world) that recognize the frame at present time. Any thread.
    struct RenderView {
        u64 seq;
        s64 display_time;
        Pose head;
        float fov_left, fov_right, fov_up, fov_down; // radians; left and down are negative
        float rows[3][3];
    };
    void SetRenderView(const RenderView& view);

private:
    Xr();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// The HUD layer (BB_XR_HUD, gpu/xr_bridge/xr_hud.h, docs/vr/findings/25-hud-layer.md): while on,
/// the upscaler draws the game's 2D UI (the Scaleform pass at the end of a frame with a scene)
/// into an image of its own instead of the game image, the pipeline cache gives those draws
/// coverage alpha, the presenter hands the image to the bridge.
namespace XrHud {
/// The UI is separated this session (any thread, cheap): BB_XR_HUD=layer (default with
/// BB_XR_MODE=alt/mono) and a bridge with the HUD layer, or BB_XR_HUD=force (flat tests).
[[nodiscard]] bool Separating();
/// Puts the UI back into the game image from the next frame on (logged once).
void Disable(const char* why);
} // namespace XrHud

} // namespace Vulkan
