// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport VR DLSS: see vk_xr_dlss.h and docs/vr/findings/37-dlss-vr.md.

#include "video_core/renderer_vulkan/vk_xr_dlss.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_xr.h"
#include "video_core/renderer_vulkan/vk_xr_eye_iq.h"
#include "video_core/renderer_vulkan/xr_dlss_logic.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

using XrDlssLogic::Quality;

/// Shared by the rasterizer (scene jitter) and the presenter (flip): both on the GPU thread, the
/// lock only guards against a future second caller.
struct JitterState {
    std::mutex mutex;
    std::atomic<bool> armed{false}; ///< the presenter runs DLSS: jitter the scene
    XrDlssLogic::Tracker tracker;
    bool stereo = false;
    float scale = 1.0f;
    bool chosen = false;            ///< this frame's jitter is chosen
    std::array<float, 2> jitter{};
    bool finished_fresh = false;    ///< a display pass finished a frame since the last flip
    std::array<float, 2> finished_jitter{};
};

JitterState& State() {
    static JitterState state;
    return state;
}

vk::Format UnormOf(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Srgb:
        return vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eB8G8R8A8Srgb:
        return vk::Format::eB8G8R8A8Unorm;
    default:
        return format;
    }
}

bool LdrColor(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
        return true;
    default:
        return false;
    }
}

bool SrgbView(vk::Format format) {
    return format == vk::Format::eR8G8B8A8Srgb || format == vk::Format::eB8G8R8A8Srgb;
}

double NowMs() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

} // namespace

std::array<float, 2> XrDlss::SceneJitter() {
    JitterState& s = State();
    if (!s.armed.load(std::memory_order_relaxed)) {
        return {};
    }
    std::scoped_lock lock{s.mutex};
    if (!s.chosen) {
        s.jitter = s.tracker.ChooseJitter(s.stereo, s.scale);
        s.chosen = true;
    }
    return s.jitter;
}

void XrDlss::OnFrameBoundary() {
    JitterState& s = State();
    std::scoped_lock lock{s.mutex};
    s.finished_jitter = s.chosen ? s.jitter : std::array<float, 2>{};
    s.finished_fresh = true;
    s.chosen = false;
}

struct XrDlss::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::TextureCache& texture_cache;

    VideoCore::UniqueImage motion_image;
    vk::UniqueImageView motion_view;
    u32 motion_width = 0, motion_height = 0;
    struct Eye {
        VideoCore::UniqueImage image; ///< RGBA8 UNORM, mutable format (sRGB sampling view)
        vk::UniqueImageView storage_view, sample_view;
        vk::Format sample_format = vk::Format::eUndefined;
        u32 width = 0, height = 0;
        bool feature = false;
        u64 evaluated = 0, resets = 0;
    };
    std::array<Eye, 2> eyes;

    bool requested = false; ///< upscale=dlss in the last frame
    bool failed = false;    ///< a feature creation/evaluation failed (until toggled off and on)
    bool warned_hud = false, warned_input = false;
    std::string last_reason = "-";
    u64 frame_no = 0;
    float jitter_sign = 1.0f;
    double next_log = 0.0;
    u64 fallback_frames = 0, held_frames = 0, other_frames = 0;

    Impl(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
         VideoCore::TextureCache& texture_cache_)
        : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
          texture_cache{texture_cache_} {
        // Debug: the opposite jitter sign convention (as the flat game's toggle 1 << 26).
        if (const char* sign = std::getenv("BB_XR_DLSS_JITTER_SIGN"); sign && sign[0] == '-') {
            jitter_sign = -1.0f;
        }
    }

    void Arm(bool on, bool stereo, float scale) {
        JitterState& s = State();
        std::scoped_lock lock{s.mutex};
        s.stereo = stereo;
        s.scale = scale;
        if (!on && s.armed.load()) {
            s.tracker.Reset();
        }
        s.armed.store(on);
    }

    void ReleaseFeatures(const char* why) {
        Dlss* dlss = Dlss::Get();
        if (!dlss || !(eyes[0].feature || eyes[1].feature)) {
            return;
        }
        // The features may still be used by submitted work; the open command buffer stays open.
        scheduler.WaitSubmitted();
        for (u32 e = 0; e < 2; ++e) {
            if (eyes[e].feature) {
                dlss->ReleaseFeature(Dlss::EyeSlot0 + e);
                eyes[e].feature = false;
            }
        }
        std::printf("VR DLSS: features released (%s)\n", why);
        std::fflush(stdout);
    }

    void Reason(const char* reason) {
        if (last_reason != reason) {
            last_reason = reason;
            if (*reason) {
                std::printf("VR DLSS: not used: %s; falling back to FSR 1 + FXAA\n", reason);
            } else {
                std::printf("VR DLSS: on (per-eye features, alternate-eye histories)\n");
            }
            std::fflush(stdout);
        }
    }

    void EnsureMotion(u32 w, u32 h) {
        if (motion_image && motion_width == w && motion_height == h) {
            return;
        }
        const auto device = instance.GetDevice();
        motion_view.reset();
        motion_image = VideoCore::UniqueImage(device, instance.GetAllocator());
        motion_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16Sfloat,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        motion_view = Check(device.createImageViewUnique({
            .image = vk::Image(motion_image),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        motion_width = w;
        motion_height = h;
    }

    void EnsureOutput(Eye& eye, u32 w, u32 h, vk::Format sample_format) {
        if (eye.image && eye.width == w && eye.height == h && eye.sample_format == sample_format) {
            return;
        }
        const auto device = instance.GetDevice();
        eye.storage_view.reset();
        eye.sample_view.reset();
        eye.image = VideoCore::UniqueImage(device, instance.GetAllocator());
        eye.image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        eye.storage_view = Check(device.createImageViewUnique({
            .image = vk::Image(eye.image),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        // DLSS writes the encoded values it read (an UNORM view of the sRGB-encoded frame); the
        // presenter samples them as it samples the display buffer: decoded when that is sRGB.
        eye.sample_view = Check(device.createImageViewUnique({
            .image = vk::Image(eye.image),
            .viewType = vk::ImageViewType::e2D,
            .format = SrgbView(sample_format) ? vk::Format::eR8G8B8A8Srgb
                                              : vk::Format::eR8G8B8A8Unorm,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        eye.width = w;
        eye.height = h;
        eye.sample_format = sample_format;
    }

    void Log(const XrEyeIq::Settings& s) {
        const double now = NowMs();
        if (now < next_log) {
            return;
        }
        if (next_log > 0.0) {
            JitterState& js = State();
            u64 resets[2];
            {
                std::scoped_lock lock{js.mutex};
                resets[0] = js.tracker.Resets(0);
                resets[1] = js.tracker.Resets(1);
            }
            std::printf("VR DLSS: 10 s: evaluated L %llu R %llu (history resets L %llu R %llu), "
                        "held %llu, menu/mono %llu, fallback %llu; %ux%u per eye, preset %c\n",
                        static_cast<unsigned long long>(eyes[0].evaluated),
                        static_cast<unsigned long long>(eyes[1].evaluated),
                        static_cast<unsigned long long>(resets[0]),
                        static_cast<unsigned long long>(resets[1]),
                        static_cast<unsigned long long>(held_frames),
                        static_cast<unsigned long long>(other_frames),
                        static_cast<unsigned long long>(fallback_frames), eyes[0].width,
                        eyes[0].height, XrDlssLogic::PresetLetter(s.dlss_preset));
            std::fflush(stdout);
            eyes[0].evaluated = eyes[1].evaluated = 0;
            held_frames = other_frames = fallback_frames = 0;
        }
        next_log = now + 10000.0;
    }
};

XrDlss::XrDlss(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
               VideoCore::TextureCache& texture_cache)
    : impl{std::make_unique<Impl>(instance, scheduler, runtime, texture_cache)} {}

XrDlss::~XrDlss() {
    impl->Arm(false, false, 1.0f);
    if (Dlss* dlss = Dlss::Get(); dlss && dlss->Available()) {
        impl->ReleaseFeatures("shutdown");
    }
}

XrDlss::Result XrDlss::Run(vk::CommandBuffer cmdbuf, const Input& in) {
    Result result{};
    const XrEyeIq::Settings& s = *in.settings;
    ++impl->frame_no;

    if (!s.dlss) {
        if (impl->requested) {
            impl->requested = false;
            impl->Arm(false, false, 1.0f);
            impl->ReleaseFeatures("switched off");
            impl->last_reason = "-";
            std::printf("VR DLSS: off\n");
            std::fflush(stdout);
        }
        return result;
    }
    if (!impl->requested) {
        impl->requested = true;
        impl->failed = false; // switched on again: retry after a failure
    }

    Dlss* dlss = Dlss::Get();
    const std::string problem = dlss ? dlss->Problem() : std::string{};
    const XrDlssLogic::System system{
        .dlls_present = dlss != nullptr,
        .bridge_slots = dlss && dlss->HasSlots(),
        .dlss_ready = dlss && dlss->Available(),
        .motion_vectors = in.motion && in.motion->HasMotionPass(),
        .flat_temporal = in.flat_temporal,
        .failed = impl->failed,
        .hdr_output = in.hdr,
        .problem = problem.c_str(),
    };
    const XrDlssLogic::Choice choice =
        XrDlssLogic::Select({.dlss = true, .fsr1 = s.fsr1, .fxaa = s.fxaa}, system);
    impl->Reason(choice.reason);
    if (!choice.dlss) {
        impl->Arm(false, false, 1.0f);
        if (dlss && dlss->Available()) {
            impl->ReleaseFeatures(choice.reason);
        }
        ++impl->fallback_frames;
        result.fallback_fsr1 = choice.fsr1;
        result.fallback_fxaa = choice.fxaa;
        return result;
    }
    if (!impl->warned_hud && !XrHud::Separating()) {
        impl->warned_hud = true;
        std::printf("VR DLSS: warning: the HUD is not a separate layer (BB_XR_HUD): it is in the "
                    "eye image and gets camera motion vectors, so it may ghost\n");
    }
    impl->Log(s);

    // The finished frame's jitter (its display pass came before this flip).
    JitterState& js = State();
    bool fresh;
    std::array<float, 2> jitter;
    {
        std::scoped_lock lock{js.mutex};
        fresh = js.finished_fresh;
        jitter = js.finished_jitter;
        js.finished_fresh = false;
    }

    // Frames DLSS does not take: FSR 1 fills the eye image size this once.
    result.fallback_fsr1 = true;
    if (!in.display) {
        return result; // the flat upscaler's own display image (not used in VR)
    }
    auto& color = impl->texture_cache.GetImage(in.display);
    const u32 w = color.info.size.width, h = color.info.size.height;
    const u32 ow = std::max(in.out_width, w), oh = std::max(in.out_height, h);
    impl->Arm(true, in.stereo, float(ow) / float(std::max(w, 1u)));

    if (in.eye == -1) {
        std::scoped_lock lock{js.mutex};
        js.tracker.OnOtherFrame(false); // shown nowhere: the histories stay
        ++impl->held_frames;
        return result;
    }
    XrDlssLogic::Camera camera{};
    const VideoCore::ImageId depth_id = in.motion->FinishedDepth();
    if (in.eye < 0 || !in.has_camera || !fresh || !depth_id ||
        !in.motion->CurrentCamera(camera.view, camera.inv_view, camera.proj)) {
        std::scoped_lock lock{js.mutex};
        js.tracker.OnOtherFrame(true); // menus, loading, mono fallback: histories go stale
        ++impl->other_frames;
        return result;
    }
    auto& depth = impl->texture_cache.GetImage(depth_id);
    const auto depth_format = depth.info.pixel_format;
    if (!LdrColor(color.info.pixel_format) ||
        (depth_format != vk::Format::eD32Sfloat && depth_format != vk::Format::eD32SfloatS8Uint) ||
        depth.info.size.width < w || depth.info.size.height < h ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eSampled)) {
        if (!impl->warned_input) {
            impl->warned_input = true;
            std::printf("VR DLSS: frame skipped: display %s %ux%u, depth %s %ux%u\n",
                        vk::to_string(color.info.pixel_format).c_str(), w, h,
                        vk::to_string(depth_format).c_str(), depth.info.size.width,
                        depth.info.size.height);
        }
        std::scoped_lock lock{js.mutex};
        js.tracker.OnOtherFrame(true);
        result.fallback_fxaa = true;
        return result;
    }

    XrDlssLogic::EyeFrame eye_frame;
    {
        std::scoped_lock lock{js.mutex};
        eye_frame = js.tracker.OnEyeFrame(in.stereo ? in.eye : 0, camera, jitter, impl->frame_no,
                                          NowMs());
    }
    const u32 slot = Dlss::EyeSlot0 + u32(eye_frame.slot);
    auto& eye = impl->eyes[eye_frame.slot];
    impl->EnsureMotion(w, h);
    impl->EnsureOutput(eye, ow, oh, in.sample_format);

    const Quality quality = XrDlssLogic::FeatureQuality(
        s.dlss_quality_set, Quality(s.dlss_quality), w, ow);
    const Dlss::FeatureDesc desc{w, h, ow, oh, int(quality), false, s.dlss_preset};
    if (!dlss->HasFeature(desc, slot)) {
        if (eye.feature) {
            impl->scheduler.WaitSubmitted();
            dlss->ReleaseFeature(slot);
            eye.feature = false;
        }
        if (!dlss->CreateFeature(cmdbuf, desc, slot)) {
            impl->failed = true;
            std::printf("VR DLSS: feature creation failed (eye %d)\n", eye_frame.slot);
            std::fflush(stdout);
            result.fallback_fxaa = true;
            return result;
        }
        eye.feature = true;
        eye_frame.reset = true;
        std::printf("VR DLSS: eye %c feature %ux%u -> %ux%u, %s, preset %c\n",
                    eye_frame.slot ? 'R' : 'L', w, h, ow, oh, XrDlssLogic::QualityName(quality),
                    XrDlssLogic::PresetLetter(s.dlss_preset));
        std::fflush(stdout);
    }

    // Per-frame views of the game's images (the texture cache owns them; destroyed after use).
    const auto device = impl->instance.GetDevice();
    const vk::ImageView color_view = Check(device.createImageView({
        .image = color.GetImage(),
        .viewType = vk::ImageViewType::e2D,
        .format = UnormOf(color.info.pixel_format),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const vk::ImageView depth_view = Check(device.createImageView({
        .image = depth.GetImage(),
        .viewType = vk::ImageViewType::e2D,
        .format = depth_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    impl->scheduler.DeferOperation([device, color_view, depth_view] {
        device.destroyImageView(color_view);
        device.destroyImageView(depth_view);
    });

    impl->runtime.Transit(&depth, vk::ImageLayout::eGeneral,
                          vk::PipelineStageFlagBits2::eComputeShader,
                          vk::AccessFlagBits2::eShaderRead);
    impl->runtime.Transit(&color, vk::ImageLayout::eGeneral,
                          vk::PipelineStageFlagBits2::eComputeShader,
                          vk::AccessFlagBits2::eShaderRead);
    impl->runtime.FlushBarriers();

    const auto barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                             vk::ImageLayout new_layout, vk::PipelineStageFlags2 dst_stage,
                             vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    // Previous contents are not needed (DLSS keeps its history inside the feature).
    barrier(vk::Image(impl->motion_image), vk::ImageLayout::eUndefined, all,
            vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    barrier(vk::Image(eye.image), vk::ImageLayout::eUndefined, all,
            vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eGeneral, all, rw);

    // Camera motion from this eye's previous frame (two game frames back with alternate eyes).
    const auto reproject = XrDlssLogic::Reproject(camera, eye_frame.previous);
    in.motion->RecordMotionWith(cmdbuf, depth_view, *impl->motion_view, w, h, reproject,
                                camera.proj, eye_frame.previous.proj, jitter);
    barrier(vk::Image(impl->motion_image), vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
            vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    float frame_ms = eye_frame.frame_ms;
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 27.8f; // two frames at 72 Hz
    }
    const bool ok = dlss->Evaluate(
        cmdbuf,
        {
            .color = {color.GetImage(), color_view, UnormOf(color.info.pixel_format),
                      vk::ImageAspectFlagBits::eColor, w, h},
            .depth = {depth.GetImage(), depth_view, depth_format, vk::ImageAspectFlagBits::eDepth,
                      depth.info.size.width, depth.info.size.height},
            .motion = {vk::Image(impl->motion_image), *impl->motion_view,
                       vk::Format::eR16G16Sfloat, vk::ImageAspectFlagBits::eColor, w, h},
            .output = {vk::Image(eye.image), *eye.storage_view, vk::Format::eR8G8B8A8Unorm,
                       vk::ImageAspectFlagBits::eColor, ow, oh},
            .jitter_x = impl->jitter_sign * jitter[0],
            .jitter_y = impl->jitter_sign * jitter[1],
            .reset = eye_frame.reset,
            .frame_ms = frame_ms,
            .sharpness = s.dlss_sharpen, // NGX sharpening: ignored by the DLSS 4 models
        },
        slot);

    // The display buffer goes back to the layout the presenter's passes sample it in.
    impl->runtime.Transit(&color, vk::ImageLayout::eShaderReadOnlyOptimal,
                          vk::PipelineStageFlagBits2::eFragmentShader |
                              vk::PipelineStageFlagBits2::eComputeShader,
                          vk::AccessFlagBits2::eShaderRead);
    impl->runtime.FlushBarriers();
    if (!ok) {
        impl->failed = true;
        std::printf("VR DLSS: evaluation failed (eye %d)\n", eye_frame.slot);
        std::fflush(stdout);
        result.fallback_fxaa = true;
        return result;
    }
    barrier(vk::Image(eye.image), vk::ImageLayout::eGeneral, all, rw,
            vk::ImageLayout::eShaderReadOnlyOptimal, all, vk::AccessFlagBits2::eShaderRead);
    ++eye.evaluated;
    eye.resets += eye_frame.reset ? 1 : 0;
    result.view = *eye.sample_view;
    result.width = ow;
    result.height = oh;
    result.fallback_fsr1 = false;
    return result;
}

} // namespace Vulkan
