// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: camera motion vectors for temporal upscaling (docs/upscaler.md). The scene constants
// give the camera of each frame; the previous one is kept here. BB_DEBUG_MOTION=1 blends the
// motion vectors as colors into the frame before it is copied to the display.

#pragma once

#include <array>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class ObjectMotion;

class CameraMotion {
public:
    CameraMotion(const Instance& instance, Scheduler& scheduler,
                 VideoCore::TextureCache& texture_cache, Runtime& runtime);
    ~CameraMotion();
    void InvalidateHistory() { current.valid = previous.valid = false; }
    void SetObjectMotion(ObjectMotion* motion) noexcept {
        object_motion = motion;
    }
    /// This frame's object motion image (after RecordMotion), for the debug view; or null.
    vk::ImageView ObjectMotionView() const noexcept;
    vk::Image ObjectMotionImage(u32 width, u32 height) const noexcept;

    void SetJitter(std::array<float, 2> value) noexcept {
        previous_jitter = jitter;
        jitter = value;
    }

    [[nodiscard]] bool Enabled() const noexcept {
        return debug_overlay || for_upscaler;
    }

    /// Both cameras and the scene depth of the current frame are known.
    [[nodiscard]] bool Ready() const noexcept {
        return current.valid && previous.valid && depth_id;
    }
    [[nodiscard]] VideoCore::ImageId Depth() const noexcept {
        return depth_id;
    }
    /// The render size in the scene constants (the game's viewport; its targets may be
    /// allocated larger, e.g. 1916x1080 for a 1916x1078 scene), or zero before the first camera.
    [[nodiscard]] std::array<u32, 2> RenderSize() const noexcept {
        return render_size;
    }
    /// The latest scene camera (world to view, 3x4 rows), if one was seen. GPU thread (a flip
    /// prepared there gets its frame's camera: the next frame's draws come after it).
    [[nodiscard]] bool CurrentView(std::array<float, 12>& view) const noexcept {
        if (!current.valid) {
            return false;
        }
        view = current.view;
        return true;
    }
    /// The latest scene camera's projection (x scale, y scale, z scale, z offset) and its shadow
    /// cascade block (scene constants 112-175), if a camera was seen (VR alternate-eye stereo:
    /// the x scale carries the eye tag; the cascades show whether the eyes share shadows).
    [[nodiscard]] bool CurrentProjection(std::array<float, 4>& proj) const noexcept {
        if (!current.valid) {
            return false;
        }
        proj = current.proj;
        return true;
    }
    [[nodiscard]] bool CurrentShadow(std::array<float, 64>& shadow) const noexcept {
        if (!current.valid) {
            return false;
        }
        shadow = current.shadow;
        return true;
    }
    /// Vertical field of view and near/far planes of the current camera.
    [[nodiscard]] float VerticalFov() const noexcept;
    [[nodiscard]] float Near() const noexcept;
    // Current projection, previous projection, and row Z of previous-view * inverse-view.
    [[nodiscard]] std::array<std::array<float, 4>, 3> TaaDepthParameters() const noexcept;

    /// Records the motion vector pass: `depth_view` (depth aspect, General layout) into
    /// `motion_view` (RG16F storage, General), pixels, previous minus current.
    void RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth_view, vk::ImageView motion_view,
                      u32 width, u32 height);

    /// bbport VR DLSS (vk_xr_dlss.h): the latest scene camera in full (world to view, view to
    /// world, projection), if one was seen. GPU thread, as CurrentView.
    [[nodiscard]] bool CurrentCamera(std::array<float, 12>& view, std::array<float, 12>& inv_view,
                                     std::array<float, 4>& proj) const noexcept {
        if (!current.valid) {
            return false;
        }
        view = current.view;
        inv_view = current.inv_view;
        proj = current.proj;
        return true;
    }
    /// bbport VR DLSS: the scene depth of the frame the latest display pass finished (Depth() is
    /// cleared there; the flip of that frame is prepared after it, before the next scene).
    [[nodiscard]] VideoCore::ImageId FinishedDepth() const noexcept {
        return finished_depth;
    }
    /// The motion vector pipeline exists (BB_UPSCALER is not "none").
    [[nodiscard]] bool HasMotionPass() const noexcept {
        return bool(motion_pipeline);
    }
    /// bbport VR DLSS: the motion pass with explicit cameras (camera motion only, no object
    /// vectors: those are relative to the previous game frame, the other eye's with alternate
    /// eyes). `reproject` = that eye's previous view * the current inverse view.
    void RecordMotionWith(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                          vk::ImageView motion_view, u32 width, u32 height,
                          const std::array<float, 12>& reproject, const std::array<float, 4>& proj,
                          const std::array<float, 4>& prev_proj,
                          std::array<float, 2> frame_jitter) const;

    /// A bound constant buffer of 864 bytes: checks the scene constant signature.
    void OnConstants(const float* data);

    /// The G-buffer pass (5+ color targets): its depth is the scene depth.
    void OnGBufferPass(VideoCore::ImageId depth);

    /// The pass copying the finished frame (`frame`, the last target drawn) to the display: the
    /// frame's depth and camera are complete. Records the debug overlay, starts a new frame.
    void OnDisplayPass(VideoCore::ImageId frame);

private:
    struct Camera {
        std::array<float, 12> view{};     ///< world to view, 3x4 rows
        std::array<float, 12> inv_view{}; ///< view to world, 3x4 rows
        std::array<float, 4> proj{};      ///< x scale, y scale, z scale, z offset
        std::array<float, 64> shadow{};   ///< shadow cascade matrices (scene constants 112-175)
        bool valid = false;
    };

    void Overlay(VideoCore::ImageId frame);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    bool debug_overlay = false;
    bool for_upscaler = false;
    vk::UniqueDescriptorSetLayout motion_desc_layout;
    vk::UniquePipelineLayout motion_pipeline_layout;
    vk::UniquePipeline motion_pipeline;

    ObjectMotion* object_motion = nullptr;
    Camera current, previous;
    bool frame_has_camera = false;
    std::array<float, 2> jitter{}, previous_jitter{};
    std::array<u32, 2> render_size{};
    VideoCore::ImageId depth_id{};
    VideoCore::ImageId finished_depth{}; ///< bbport VR DLSS: depth_id at the last display pass

    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline overlay_pipeline;
    /// Debug: the last two finished frames (RGBA8 packed), for the reprojection check.
    std::unique_ptr<VideoCore::Buffer> frames[2];
    u32 frame_index = 0;
};

} // namespace Vulkan
