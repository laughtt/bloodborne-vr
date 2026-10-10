// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport VR: NVIDIA DLSS Super Resolution for the eye images (docs/vr/findings/37-dlss-vr.md).
// BB_XR_UPSCALE=dlss (default off; options in vk_xr_eye_iq.h). The port contains no NVIDIA code:
// it goes through vk_dlss (bbport_dlss.dll, MIT, loaded at run time with nvngx_dlss.dll).
//
// Where it runs: Presenter::PrepareFrame, on the game's finished frame (the display buffer:
// tonemapped LDR, post-processed, without the HUD, which is its own layer in VR) before it is
// copied to an eye; the output (the eye image size) replaces the FSR 1 upscale.
//
// Temporal state per eye (alternate-eye stereo, BB_XR_MODE=alt, L R L R): one DLSS feature per
// eye (bridge slots 1 and 2), each fed only its own eye's frames, so its history is that eye's
// previous frame, two game frames back. The scene draws are jittered (viewport shift, the same
// mechanism as the flat upscaler) with a per-eye Halton sequence: the eye of the next frame is
// predicted (the other one than the last eye frame) when its first scene draw is recorded; the
// jitter really used is passed with the frame, so a misprediction costs only sequence order.
// Motion vectors: camera reprojection of the scene depth from the current camera to that eye's
// previous camera (head and body motion over the two frames included); no object vectors.

#pragma once

#include <array>
#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class CameraMotion;

namespace XrEyeIq {
struct Settings;
}

class XrDlss {
public:
    // Rasterizer side (GPU thread).

    /// The jitter of this frame's scene draws in render pixels (viewport shift, x right, y down),
    /// zero while VR DLSS is not running. Chosen at the frame's first scene draw.
    static std::array<float, 2> SceneJitter();
    /// The display pass (frame boundary): keeps the finished frame's jitter for its flip.
    static void OnFrameBoundary();

    // Presenter side (GPU thread).

    XrDlss(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
           VideoCore::TextureCache& texture_cache);
    ~XrDlss();

    struct Input {
        const XrEyeIq::Settings* settings;
        VideoCore::ImageId display; ///< the frame's display buffer (null when not a guest image)
        vk::Format sample_format;   ///< the format the presenter samples the display buffer in
        const CameraMotion* motion;
        bool stereo; ///< alternate eyes (BB_XR_MODE=alt)
        int eye;     ///< 0/1 the eye the frame goes to; -1 held (shown nowhere); -2 mono/menu
        bool has_camera;
        bool hdr;
        bool flat_temporal; ///< the flat upscaler jitters the scene
        u32 out_width, out_height; ///< the frame (eye image) size
    };
    struct Result {
        vk::ImageView view;     ///< the DLSS output (ShaderReadOnlyOptimal), or null
        u32 width = 0, height = 0; ///< its size
        bool fallback_fsr1 = false; ///< DLSS was asked for but did not run: upscale with FSR 1
        bool fallback_fxaa = false; ///< ... and anti-alias with FXAA (DLSS cannot run at all)
    };
    /// Every PrepareFrame while VR DLSS is asked for (or was): records DLSS into `cmdbuf`.
    Result Run(vk::CommandBuffer cmdbuf, const Input& input);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
