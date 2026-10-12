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
        /// True stereo (BB_XR_MODE=stereo): eye L of a pair, `eye_m` metres along the camera's
        /// x axis (the hybrid warp's eye L offset; 0 without one). Its camera, motion vectors and
        /// history are eye L's; RunEyeR then evaluates eye R of the same frame.
        bool pair = false;
        float eye_m = 0.0f;
        /// Pre-SR DLSS-NR (vk_xr_dlssnr.h): the colour DLSS reads instead of the display buffer
        /// (its processed copy: the same size and bytes, eGeneral, mutable format), or null.
        vk::Image color_override{};
        /// Start every DLSS history over (DLSS's input changed kind: pre-SR DLSS-NR toggled).
        bool reset_history = false;
    };
    struct Result {
        vk::ImageView view;     ///< the DLSS output (ShaderReadOnlyOptimal), or null
        u32 width = 0, height = 0; ///< its size
        bool fallback_fsr1 = false; ///< DLSS was asked for but did not run: upscale with FSR 1
        bool fallback_fxaa = false; ///< ... and anti-alias with FXAA (DLSS cannot run at all)
    };
    /// Every PrepareFrame while VR DLSS is asked for (or was): records DLSS into `cmdbuf`.
    Result Run(vk::CommandBuffer cmdbuf, const Input& input);

    /// True stereo, eye R of the frame whose eye L Run just evaluated (docs/vr/findings/31
    /// section 9.8): its own feature (slot 2) and history, colour = the hybrid warp's eye R
    /// (render size, eGeneral, the display buffer's bytes), depth = eye R's G-buffer depth twin
    /// (eGeneral), camera = eye L's moved to `eye_m`, the same jitter as eye L. No view in the
    /// result: eye R keeps FSR 1 this frame (eye L not evaluated, inputs unusable, or eye R's
    /// feature could not be created: logged once, FSR 1 until DLSS is toggled).
    struct EyeRInput {
        vk::Image color;
        u32 width, height;
        vk::Image depth;
        vk::Format depth_format;
        u32 depth_width, depth_height;
        float eye_m; ///< eye R's offset along the camera's x axis (metres)
    };
    Result RunEyeR(vk::CommandBuffer cmdbuf, const EyeRInput& input);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
