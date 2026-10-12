// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <deque>

#include <condition_variable>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/host_passes/xr_fxaa_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_xr_stereo.h"
#include "video_core/renderer_vulkan/xr_resize_logic.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

namespace Stereo {
struct EyeRImage;
}

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};
    // bbport VR: the frame's scene camera (world to view, 3x4 rows) when it had one, taken when
    // the flip was prepared; pairs the image with the head pose it was rendered with (mono).
    std::array<float, 12> camera_view{};
    bool has_camera_view{false};
    // bbport VR (BB_XR_MODE=alt, vk_xr_stereo.h): the eye this frame was set up for, decided when
    // the flip was prepared; Present hands it to that eye (Hold: to no eye; Mono: the flat path).
    bool stereo_on{false};
    xrstereo::Decision stereo{};
    // bbport VR true stereo (BB_XR_MODE=stereo, vk_xr_stereo.h): what this frame goes to (Eye: the
    // pair of both eyes with pair_view; Hold: nothing; Mono: the flat screen), decided when the
    // flip was prepared. eye_r: the stereo renderer's final eye R image (vk_stereo_eye_r.h), else
    // eye L's image goes to both eyes.
    bool pair_on{false};
    xrstereo::Kind pair_kind{xrstereo::Kind::Hold};
    xrstereo::PairView pair_view{};
    s64 pair_camera_ns{};
    bool eye_r_valid{false};
    vk::Image eye_r_image{};
    vk::ImageLayout eye_r_layout{};
    // bbport VR (hybrid stereo): the frame's own eye R output, the hybrid warp's eye R run through
    // the same image-quality chain as eye L; width x height, swapchain format, eGeneral after.
    vk::Image eye_r_out{};
    vk::ImageView eye_r_out_view{};
    VmaAllocation eye_r_out_allocation{};
    u32 eye_r_out_width{}, eye_r_out_height{};
    vk::Format eye_r_out_format{};
    // bbport VR HUD layer (vk_xr.h XrHud): the frame's UI, drawn apart from its image (eGeneral),
    // copied when the flip was prepared; Present hands it to the bridge's HUD quad, or, when it
    // covers the screen (in-game loading screens, full-screen menus), to the flat cinema screen.
    vk::Image hud_image{};
    VmaAllocation hud_allocation{};
    vk::Format hud_format{};
    u32 hud_width{}, hud_height{};
    bool hud_valid{false};
    bool hud_fullscreen{false};
    u32 hud_activity{0}; ///< lit probes outside the always-on HUD zones (xr_hud_center.h)
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;
class XrDlss;
class XrDlssNr;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    Frame* PrepareBlankFrame(bool present_thread);

    /// BB_XR_MODE=stereo: the frame's render view and what it goes to (Frame::pair_*).
    void PreparePair(Frame* frame);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    Frame* PrepareLastFrame();

private:
    Frame* GetRenderFrame();

    void RecreateFrame(Frame* frame, u32 width, u32 height);
    bool EnsureEyeROut(Frame* frame);
    void RecordEyeRChain(Frame* frame, const Stereo::EyeRImage& hybrid, bool fxaa,
                         const HostPasses::FsrPass::Settings& fsr, bool dlss_on,
                         vk::ImageView dlss_view = {}, vk::Extent2D dlss_size = {});
    void DestroyEyeROut(Frame* frame);

    XrResize::Size FitGameSize(s32 width, s32 height) const;

private:
    // bbport: written by the GPU thread (PrepareFrame) and the present thread (Present), read
    // by the other: each one atomic and the frame size published whole (XrResize::Pack), so the
    // GPU thread never sees half of an update (docs/vr/findings/37, device-lost crashes).
    std::atomic<float> expected_ratio{1920.0f / 1080.0f};
    std::atomic<u64> expected_frame_size{(u64(1920) << 32) | 1080};
    /// bbport VR: present thread only. The frame size target is applied once it held for a few
    /// presents; the inputs it was made of last (for the resize log line).
    XrResize::SizeDebounce frame_size_debounce{3};
    XrResize::Inputs frame_size_inputs{};
    /// bbport VR: the sizes of the bridge swapchains this side wrote (eye L, eye R, flat, HUD):
    /// before one is recreated the GPU must be done with the old images.
    std::array<XrResize::ChainSize, 4> xr_chain_sizes{};
    void XrChainResize(int chain, u32 width, u32 height);
    /// bbport VR: the last frame size PrepareFrame drew at (GPU thread): host pass images and
    /// DLSS outputs are recreated after a change, once the GPU is done with them.
    XrResize::Size prepared_size{};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::XrFxaaPass xr_fxaa_pass; ///< bbport VR: BB_XR_AA=fxaa (vk_xr_eye_iq.h)
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    /// bbport VR (hybrid stereo): eye R's own chain instances (each pass keeps one intermediate
    /// ring, so eye L's instances cannot be run twice per frame).
    HostPasses::FsrPass fsr_pass_r;
    HostPasses::XrFxaaPass xr_fxaa_pass_r;
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    std::deque<u64> recent_frame_ticks; ///< bbport: BB_FRAMES_AHEAD bound (PrepareFrame)
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    std::unique_ptr<XrDlss> xr_dlss; ///< bbport VR: BB_XR_UPSCALE=dlss (vk_xr_dlss.h)
    std::unique_ptr<XrDlssNr> xr_dlssnr; ///< bbport VR: BB_XR_DLSSNR=1 (vk_xr_dlssnr.h)
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
};

} // namespace Vulkan
