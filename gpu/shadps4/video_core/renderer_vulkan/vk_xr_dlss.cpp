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

#include "video_core/host_shaders/xr_dlss_mask_comp.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_xr.h"
#include "video_core/renderer_vulkan/vk_xr_eye_iq.h"
#include "video_core/renderer_vulkan/xr_dlss_logic.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

using XrDlssLogic::Quality;

/// Shared by the rasterizer (scene jitter) and the presenter (flip): both on the GPU thread, the
/// lock only guards against a future second caller.
/// Push constants of host_shaders/xr_dlss_mask.comp.
struct MaskConstants {
    std::array<float, 4> proj;
    std::array<float, 2> size;
    std::array<float, 2> jitter;
    float threshold;
};

struct JitterState {
    std::mutex mutex;
    std::atomic<bool> armed{false}; ///< the presenter runs DLSS: jitter the scene
    std::atomic<bool> jitter_on{true}; ///< dlss_jitter=0 zeroes the jitter (A/B diagnostic)
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
        s.jitter = s.tracker.ChooseJitter(s.stereo, s.scale, s.jitter_on.load());
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
    // The arm/weapon bias-current-colour mask (findings/37 section 12), R8 at render size.
    VideoCore::UniqueImage mask_image;
    vk::UniqueImageView mask_view;
    u32 mask_width = 0, mask_height = 0;
    vk::UniqueDescriptorSetLayout mask_desc_layout;
    vk::UniquePipelineLayout mask_pipeline_layout;
    vk::UniquePipeline mask_pipeline;
    int mask_logged = -1; ///< last logged state: -1 none, 0 off, 1 on
    float mask_logged_m = -1.0f;
    bool warned_mask_abi = false;
    struct Eye {
        VideoCore::UniqueImage image; ///< RGBA8 UNORM, mutable format (sRGB sampling view)
        vk::UniqueImageView storage_view, sample_view;
        vk::Format sample_format = vk::Format::eUndefined;
        u32 width = 0, height = 0;
        bool feature = false;
        XrDlssLogic::FeatureKey key{};  ///< this eye's own feature (never shared with the other)
        bool released_by_reset = false; ///< released by a settings change / DLSS toggle
        u64 evaluated = 0, resets = 0;
    };
    XrDlssLogic::RecreateGuard recreate_guard; ///< >3 creates of one eye in 2 s: warns once
    int jitter_logged = -1;                    ///< dlss_jitter state last logged
    std::array<Eye, 2> eyes;

    // True stereo (Input::pair, findings/31 section 9.8): eye L's evaluated frame, for RunEyeR in
    // the same PrepareFrame, and eye R's own history.
    struct PairFrame {
        bool valid = false;
        u64 frame_no = 0;
        XrDlssLogic::Camera center{}; ///< the game camera (eye cameras are moved from it)
        std::array<float, 2> jitter{};
        bool reset = true;
        float frame_ms = 0.0f;
        u32 w = 0, h = 0, ow = 0, oh = 0;
        vk::Format sample_format{};
        bool use_mask = false;
        float eye_m = 0.0f; ///< eye L's offset along the camera's x axis
    } pair_l;
    XrDlssLogic::PairEyeR pair_r;
    bool pair_mode = false;       ///< the last evaluated frame was eye L of a pair
    bool pair_r_failed = false;   ///< eye R's feature failed: eye R on FSR 1 until a toggle
    bool warned_pair_input = false;
    u64 pair_r_fsr_frames = 0;    ///< 10 s: eye-L-evaluated pair frames whose eye R kept FSR 1

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
        pair_r.Invalidate();
        if (!dlss || !(eyes[0].feature || eyes[1].feature)) {
            return;
        }
        // The features may still be used by submitted work; the open command buffer stays open.
        scheduler.WaitSubmitted();
        for (u32 e = 0; e < 2; ++e) {
            if (eyes[e].feature) {
                dlss->ReleaseFeature(Dlss::EyeSlot0 + e);
                eyes[e].feature = false;
                eyes[e].released_by_reset = true;
                std::printf("VR DLSS: eye %c feature released (%s): %ux%u -> %ux%u\n",
                            e ? 'R' : 'L', why, eyes[e].key.render_w, eyes[e].key.render_h,
                            eyes[e].key.out_w, eyes[e].key.out_h);
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
        if (motion_image) {
            // The last frame's DLSS may still read it on the GPU.
            scheduler.WaitSubmitted();
            std::printf("VR DLSS: motion vectors %ux%u -> %ux%u\n", motion_width, motion_height,
                        w, h);
            std::fflush(stdout);
        }
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

    /// The compute pass of the mask, created on first use.
    void EnsureMaskPipeline() {
        if (mask_pipeline) {
            return;
        }
        const auto device = instance.GetDevice();
        const std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {{
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        mask_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(MaskConstants)};
        mask_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*mask_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto module = CompileSPV(XR_DLSS_MASK_COMP, device);
        mask_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *mask_pipeline_layout,
                }));
        device.destroyShaderModule(module);
    }

    void EnsureMask(u32 w, u32 h) {
        if (mask_image && mask_width == w && mask_height == h) {
            return;
        }
        const auto device = instance.GetDevice();
        if (mask_image) {
            // The last frame's DLSS may still read it on the GPU.
            scheduler.WaitSubmitted();
        }
        mask_view.reset();
        mask_image = VideoCore::UniqueImage(device, instance.GetAllocator());
        mask_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8Unorm,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        mask_view = Check(device.createImageViewUnique({
            .image = vk::Image(mask_image),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR8Unorm,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        mask_width = w;
        mask_height = h;
    }

    /// Logs the mask state once and whenever it changes (on/off, distance).
    void LogMask(bool on, float metres, const Dlss* dlss) {
        if (on && !dlss->HasBiasMask()) {
            if (!warned_mask_abi) {
                warned_mask_abi = true;
                std::printf("VR DLSS: arm mask requested but the bridge has no bias mask (needs "
                            "bbport_dlss.dll ABI 3): off\n");
                std::fflush(stdout);
            }
            return;
        }
        const int state = on ? 1 : 0;
        if (state == mask_logged && (!on || metres == mask_logged_m)) {
            return;
        }
        mask_logged = state;
        mask_logged_m = metres;
        if (on) {
            std::printf("VR DLSS: arm/weapon bias mask on (surfaces nearer than %.2f m use the "
                        "current colour)\n",
                        metres);
        } else {
            std::printf("VR DLSS: arm/weapon bias mask off\n");
        }
        std::fflush(stdout);
    }

    void EnsureOutput(Eye& eye, u32 w, u32 h, vk::Format sample_format) {
        if (eye.image && eye.width == w && eye.height == h && eye.sample_format == sample_format) {
            return;
        }
        const auto device = instance.GetDevice();
        if (eye.image) {
            // Earlier frames' DLSS writes and presenter passes may still use it on the GPU.
            scheduler.WaitSubmitted();
            std::printf("VR DLSS: eye output %ux%u -> %ux%u\n", eye.width, eye.height, w, h);
            std::fflush(stdout);
        }
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
            if (pair_mode) {
                std::printf("VR DLSS: 10 s: stereo pairs: eye R own feature %llu frames (history "
                            "resets %llu), eye R on FSR 1 %llu%s\n",
                            static_cast<unsigned long long>(eyes[1].evaluated),
                            static_cast<unsigned long long>(eyes[1].resets),
                            static_cast<unsigned long long>(pair_r_fsr_frames),
                            pair_r_failed ? " (eye R feature failed)" : "");
            }
            std::fflush(stdout);
            eyes[0].evaluated = eyes[1].evaluated = 0;
            eyes[0].resets = eyes[1].resets = 0;
            held_frames = other_frames = fallback_frames = 0;
            pair_r_fsr_frames = 0;
        }
        next_log = now + 10000.0;
    }

    /// Device-local memory in use by this process (VK_EXT_memory_budget; NGX's allocations
    /// included), 0 when the driver cannot tell.
    u64 DeviceLocalUsage() const {
        if (!instance.CanReportMemoryUsage()) {
            return 0;
        }
        vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
        vk::PhysicalDeviceMemoryProperties2 props{.pNext = &budget};
        instance.GetPhysicalDevice().getMemoryProperties2(&props);
        u64 used = 0;
        for (u32 i = 0; i < props.memoryProperties.memoryHeapCount; ++i) {
            if (props.memoryProperties.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
                used += budget.heapUsage[i];
            }
        }
        return used;
    }

    /// One eye's DLSS evaluation: its output image and feature (created when missing or
    /// changed), the motion vector and mask passes on its depth, the evaluate. The colour and
    /// depth images are in eGeneral (the caller's job).
    struct EyeJob {
        u32 eye = 0;                     ///< eyes[eye], bridge slot Dlss::EyeSlot0 + eye
        vk::Image color;
        vk::Format color_format{};       ///< the UNORM view DLSS reads (DlssColorFormat)
        bool color_sampled_only = false; ///< view usage restricted to sampling (the warp image)
        vk::Image depth;
        vk::Format depth_format{};
        u32 depth_w = 0, depth_h = 0;
        XrDlssLogic::Camera camera{}, previous{};
        std::array<float, 2> jitter{};
        bool reset = true;
        float frame_ms = 0.0f;
        u32 w = 0, h = 0, ow = 0, oh = 0;
        vk::Format sample_format{};
        bool use_mask = false;
        bool profile = false; ///< its own GPU profiler segment ("VR DLSS: eye L/R")
    };
    enum class Outcome { Ok, CreateFailed, EvalFailed };

    Outcome Evaluate(vk::CommandBuffer cmdbuf, Dlss* dlss, const XrEyeIq::Settings& s,
                     EyeJob& job) {
        const u32 w = job.w, h = job.h, ow = job.ow, oh = job.oh;
        const u32 slot = Dlss::EyeSlot0 + job.eye;
        const char letter = job.eye ? 'R' : 'L';
        auto& eye = eyes[job.eye];
        EnsureMotion(w, h);
        if (job.use_mask) {
            EnsureMaskPipeline();
            EnsureMask(w, h);
        }
        EnsureOutput(eye, ow, oh, job.sample_format);

        const Quality quality = XrDlssLogic::FeatureQuality(
            s.dlss_quality_set, Quality(s.dlss_quality), w, ow);
        const Dlss::FeatureDesc desc{w, h, ow, oh, int(quality), false, s.dlss_preset};
        // Per eye: compared with this eye's own slot/key, not a cache shared with the other eye.
        const XrDlssLogic::FeatureKey want{w, h, ow, oh, int(quality), s.dlss_preset, false};
        if (!dlss->HasFeature(desc, slot)) {
            const XrDlssLogic::CreateReason why = XrDlssLogic::CreateReasonFor(
                eye.feature ? &eye.key : nullptr, want, eye.released_by_reset);
            if (eye.feature) {
                scheduler.WaitSubmitted();
                dlss->ReleaseFeature(slot);
                eye.feature = false;
                std::printf("VR DLSS: eye %c feature released (%s)\n", letter,
                            XrDlssLogic::CreateReasonName(why));
            }
            const u64 used_before = DeviceLocalUsage();
            if (!dlss->CreateFeature(cmdbuf, desc, slot)) {
                std::printf("VR DLSS: feature creation failed (eye %u)\n", job.eye);
                std::fflush(stdout);
                return Outcome::CreateFailed;
            }
            const u64 used_after = DeviceLocalUsage();
            eye.feature = true;
            eye.key = want;
            eye.released_by_reset = false;
            job.reset = true;
            char vram[48] = "";
            if (used_before && used_after) {
                std::snprintf(vram, sizeof(vram), ", device memory %+.0f MB",
                              (double(used_after) - double(used_before)) / (1024.0 * 1024.0));
            }
            std::printf("VR DLSS: eye %c feature created (%s): render %ux%u -> output %ux%u, %s, "
                        "preset %c%s\n",
                        letter, XrDlssLogic::CreateReasonName(why), w, h, ow, oh,
                        XrDlssLogic::QualityName(quality), XrDlssLogic::PresetLetter(s.dlss_preset),
                        vram);
            if (recreate_guard.Note(int(job.eye), NowMs())) {
                std::printf("VR DLSS: WARNING: eye %c feature created %u times within 2 s (last "
                            "reason: %s): a recreate loop drops the history every time\n",
                            letter, recreate_guard.InWindow(int(job.eye)),
                            XrDlssLogic::CreateReasonName(why));
            }
            std::fflush(stdout);
        }

        // Per-frame views of the inputs (destroyed once the GPU is past them).
        const auto device = instance.GetDevice();
        const vk::ImageViewUsageCreateInfo sampled_only{.usage = vk::ImageUsageFlagBits::eSampled};
        const vk::ImageView color_view = Check(device.createImageView({
            .pNext = job.color_sampled_only ? &sampled_only : nullptr,
            .image = job.color,
            .viewType = vk::ImageViewType::e2D,
            .format = job.color_format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        const vk::ImageView depth_view = Check(device.createImageView({
            .image = job.depth,
            .viewType = vk::ImageViewType::e2D,
            .format = job.depth_format,
            .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
        }));
        scheduler.DeferOperation([device, color_view, depth_view] {
            device.destroyImageView(color_view);
            device.destroyImageView(depth_view);
        });

        auto* profiler = GpuProfiler::Get();
        const bool mark = job.profile && profiler && profiler->Records(&scheduler);
        const u64 resume = mark ? profiler->Current() : 0;
        if (mark) {
            profiler->Mark(0x5A1Dull << 40 | job.eye, [&] {
                return std::string{job.eye ? "VR DLSS: eye R (motion, mask, evaluate)"
                                           : "VR DLSS: eye L (motion, mask, evaluate)"};
            });
        }

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
        // Previous contents are not needed (DLSS keeps its history inside the feature). The
        // motion and mask images are shared by the eyes: each eye rewrites them after the
        // previous evaluate read them (these barriers order that).
        barrier(vk::Image(motion_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
        barrier(vk::Image(eye.image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eGeneral, all, rw);
        if (job.use_mask) {
            barrier(vk::Image(mask_image), vk::ImageLayout::eUndefined, all,
                    vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
        }

        // Camera motion from this eye's previous frame (two game frames back with alternate
        // eyes, one with true stereo).
        const auto reproject = XrDlssLogic::Reproject(job.camera, job.previous);
        // (CameraMotion::RecordMotionWith is stateless apart from its pipeline.)
        motion_pass->RecordMotionWith(cmdbuf, depth_view, *motion_view, w, h, reproject,
                                      job.camera.proj, job.previous.proj, job.jitter);
        barrier(vk::Image(motion_image), vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

        // Near surfaces (the arms and weapon follow the hands, the motion vectors only the
        // camera): DLSS is told to trust the current colour there.
        if (job.use_mask) {
            const MaskConstants push{
                .proj = job.camera.proj,
                .size = {float(w), float(h)},
                .jitter = job.jitter,
                .threshold = s.dlss_mask_m,
            };
            const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                                     .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo mask_info{.imageView = *mask_view,
                                                    .imageLayout = vk::ImageLayout::eGeneral};
            const std::array<vk::WriteDescriptorSet, 2> writes = {{
                {.dstBinding = 0,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage,
                 .pImageInfo = &depth_info},
                {.dstBinding = 1,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage,
                 .pImageInfo = &mask_info},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *mask_pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *mask_pipeline_layout,
                                        0, writes);
            cmdbuf.pushConstants(*mask_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(push), &push);
            cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
            barrier(vk::Image(mask_image), vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                    vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);
        }

        const bool ok = dlss->Evaluate(
            cmdbuf,
            {
                .color = {job.color, color_view, job.color_format,
                          vk::ImageAspectFlagBits::eColor, w, h},
                .depth = {job.depth, depth_view, job.depth_format,
                          vk::ImageAspectFlagBits::eDepth, job.depth_w, job.depth_h},
                .motion = {vk::Image(motion_image), *motion_view, vk::Format::eR16G16Sfloat,
                           vk::ImageAspectFlagBits::eColor, w, h},
                .output = {vk::Image(eye.image), *eye.storage_view, vk::Format::eR8G8B8A8Unorm,
                           vk::ImageAspectFlagBits::eColor, ow, oh},
                .jitter_x = jitter_sign * job.jitter[0],
                .jitter_y = jitter_sign * job.jitter[1],
                .reset = job.reset,
                .frame_ms = job.frame_ms,
                .sharpness = s.dlss_sharpen, // NGX sharpening: ignored by the DLSS 4 models
                .bias_mask = job.use_mask
                                 ? Dlss::Resource{vk::Image(mask_image), *mask_view,
                                                  vk::Format::eR8Unorm,
                                                  vk::ImageAspectFlagBits::eColor, w, h}
                                 : Dlss::Resource{},
            },
            slot);
        if (mark && resume) {
            profiler->Resume(resume);
        }
        if (!ok) {
            std::printf("VR DLSS: evaluation failed (eye %u)\n", job.eye);
            std::fflush(stdout);
            return Outcome::EvalFailed;
        }
        barrier(vk::Image(eye.image), vk::ImageLayout::eGeneral, all, rw,
                vk::ImageLayout::eShaderReadOnlyOptimal, all, vk::AccessFlagBits2::eShaderRead);
        ++eye.evaluated;
        eye.resets += job.reset ? 1 : 0;
        return Outcome::Ok;
    }

    const CameraMotion* motion_pass = nullptr; ///< the frame's Input::motion
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
        impl->pair_r_failed = false;
        impl->warned_pair_input = false;
    }
    impl->pair_l.valid = false;
    impl->motion_pass = in.motion;

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

    // dlss_jitter=0|1 (control file): zero jitter for A/B comparisons, in both the projection
    // offset (SceneJitter) and the value passed to DLSS (below); histories restart on a change.
    {
        const int want_jitter = s.dlss_jitter ? 1 : 0;
        if (impl->jitter_logged != want_jitter) {
            const bool first = impl->jitter_logged < 0;
            impl->jitter_logged = want_jitter;
            JitterState& jit = State();
            std::scoped_lock lock{jit.mutex};
            jit.jitter_on.store(s.dlss_jitter);
            if (!first || !s.dlss_jitter) {
                jit.tracker.Reset();
            }
            std::printf("VR DLSS: jitter %s%s\n",
                        s.dlss_jitter ? "on"
                                      : "OFF (dlss_jitter=0: projection and DLSS jitter are zero)",
                        first ? " (initial)" : "");
            std::fflush(stdout);
        }
    }

    if (in.reset_history) {
        JitterState& jit = State();
        std::scoped_lock lock{jit.mutex};
        jit.tracker.Reset();
        std::printf("VR DLSS: histories start over (the input changed: pre-SR DLSS-NR)\n");
        std::fflush(stdout);
    }

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
    if (!s.dlss_jitter) {
        jitter = {}; // a frame drawn before the switch still carries its old offset
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
    if (!LdrColor(color.info.pixel_format) || !LdrColor(in.sample_format) ||
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

    // True stereo: eye L's own camera (the game camera moved to eye L), so its motion vectors
    // carry the eye's lever arm under head rotation; RunEyeR moves it to eye R.
    const XrDlssLogic::Camera center = camera;
    if (in.pair) {
        camera = XrDlssLogic::EyeCamera(camera, in.eye_m);
    }
    XrDlssLogic::EyeFrame eye_frame;
    {
        std::scoped_lock lock{js.mutex};
        eye_frame = js.tracker.OnEyeFrame(in.stereo ? in.eye : 0, camera, jitter, impl->frame_no,
                                          NowMs());
    }
    const bool mask_on = s.dlss_mask && s.dlss_mask_m > 0.0f;
    impl->LogMask(mask_on, s.dlss_mask_m, dlss);
    const bool use_mask = mask_on && dlss->HasBiasMask();

    float frame_ms = eye_frame.frame_ms;
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 27.8f; // two frames at 72 Hz
    }
    // The presenter reads the display buffer in the video-out format (in.sample_format), whose
    // channel order can differ from the image's own: read it the same way (swapped red/blue
    // otherwise). Same 8888 class, so the image's mutable format covers it.
    Impl::EyeJob job{
        .eye = u32(eye_frame.slot),
        .color = in.color_override ? in.color_override : color.GetImage(),
        .color_format = vk::Format(XrDlssLogic::DlssColorFormat(u32(in.sample_format))),
        .depth = depth.GetImage(),
        .depth_format = depth_format,
        .depth_w = depth.info.size.width,
        .depth_h = depth.info.size.height,
        .camera = camera,
        .previous = eye_frame.previous,
        .jitter = jitter,
        .reset = eye_frame.reset,
        .frame_ms = frame_ms,
        .w = w,
        .h = h,
        .ow = ow,
        .oh = oh,
        .sample_format = in.sample_format,
        .use_mask = use_mask,
        .profile = in.pair,
    };

    impl->runtime.Transit(&depth, vk::ImageLayout::eGeneral,
                          vk::PipelineStageFlagBits2::eComputeShader,
                          vk::AccessFlagBits2::eShaderRead);
    impl->runtime.Transit(&color, vk::ImageLayout::eGeneral,
                          vk::PipelineStageFlagBits2::eComputeShader,
                          vk::AccessFlagBits2::eShaderRead);
    impl->runtime.FlushBarriers();
    const Impl::Outcome outcome = impl->Evaluate(cmdbuf, dlss, s, job);
    // The display buffer goes back to the layout the presenter's passes sample it in.
    impl->runtime.Transit(&color, vk::ImageLayout::eShaderReadOnlyOptimal,
                          vk::PipelineStageFlagBits2::eFragmentShader |
                              vk::PipelineStageFlagBits2::eComputeShader,
                          vk::AccessFlagBits2::eShaderRead);
    impl->runtime.FlushBarriers();
    if (outcome != Impl::Outcome::Ok) {
        impl->failed = true;
        result.fallback_fxaa = true;
        return result;
    }
    impl->pair_mode = in.pair;
    if (in.pair) {
        impl->pair_l = {
            .valid = true,
            .frame_no = impl->frame_no,
            .center = center,
            .jitter = jitter,
            .reset = job.reset,
            .frame_ms = frame_ms,
            .w = w,
            .h = h,
            .ow = ow,
            .oh = oh,
            .sample_format = in.sample_format,
            .use_mask = use_mask,
            .eye_m = in.eye_m,
        };
        ++impl->pair_r_fsr_frames; // undone when RunEyeR evaluates eye R
    }
    auto& eye = impl->eyes[eye_frame.slot];
    result.view = *eye.sample_view;
    result.width = ow;
    result.height = oh;
    result.fallback_fsr1 = false;
    return result;
}

XrDlss::Result XrDlss::RunEyeR(vk::CommandBuffer cmdbuf, const EyeRInput& in) {
    Result result{};
    auto& p = impl->pair_l;
    if (!p.valid || p.frame_no != impl->frame_no) {
        return result; // eye L was not evaluated this frame: eye R keeps FSR 1 too
    }
    p.valid = false;
    Dlss* dlss = Dlss::Get();
    if (impl->pair_r_failed || !dlss || !dlss->Available()) {
        return result;
    }
    const bool depth_ok = in.depth && (in.depth_format == vk::Format::eD32Sfloat ||
                                       in.depth_format == vk::Format::eD32SfloatS8Uint);
    if (!in.color || in.width != p.w || in.height != p.h || !depth_ok ||
        in.depth_width < p.w || in.depth_height < p.h) {
        if (!impl->warned_pair_input) {
            impl->warned_pair_input = true;
            std::printf("VR DLSS: eye R frame skipped (FSR 1): warp image %ux%u, depth twin %s "
                        "%ux%u, eye L render %ux%u\n",
                        in.width, in.height, vk::to_string(in.depth_format).c_str(),
                        in.depth_width, in.depth_height, p.w, p.h);
            std::fflush(stdout);
        }
        impl->pair_r.Invalidate();
        return result;
    }
    const XrDlssLogic::Camera camera = XrDlssLogic::EyeCamera(p.center, in.eye_m);
    const XrDlssLogic::PairEyeR::Step step =
        impl->pair_r.OnFrame(camera, p.reset, impl->frame_no);
    const XrEyeIq::Settings s = XrEyeIq::Current();
    Impl::EyeJob job{
        .eye = 1,
        .color = in.color,
        .color_format = vk::Format(XrDlssLogic::DlssColorFormat(u32(p.sample_format))),
        .color_sampled_only = true,
        .depth = in.depth,
        .depth_format = in.depth_format,
        .depth_w = in.depth_width,
        .depth_h = in.depth_height,
        .camera = camera,
        .previous = step.previous,
        .jitter = p.jitter, // one jittered render for both eyes (findings/31 section 9.8)
        .reset = step.reset,
        .frame_ms = p.frame_ms,
        .w = p.w,
        .h = p.h,
        .ow = p.ow,
        .oh = p.oh,
        .sample_format = p.sample_format,
        .use_mask = p.use_mask,
        .profile = true,
    };
    // Both inputs are in eGeneral already (the warp's output and the depth twin it read).
    const Impl::Outcome outcome = impl->Evaluate(cmdbuf, dlss, s, job);
    if (outcome != Impl::Outcome::Ok) {
        // Eye L keeps DLSS; eye R goes back to FSR 1 (likely out of video memory).
        impl->pair_r_failed = true;
        impl->pair_r.Invalidate();
        std::printf("VR DLSS: eye R's own DLSS feature %s: eye R stays on FSR 1 (eye L keeps "
                    "DLSS) until DLSS is switched off and on\n",
                    outcome == Impl::Outcome::CreateFailed ? "could not be created (video memory?)"
                                                           : "failed to evaluate");
        std::fflush(stdout);
        return result;
    }
    if (impl->pair_r_fsr_frames) {
        --impl->pair_r_fsr_frames;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::printf("VR DLSS: stereo pairs: eye R has its own feature and history (slot 2): "
                    "warped colour, its depth twin, camera at %+.4f m (eye L at %+.4f m) along "
                    "the camera's x axis, the same jitter\n",
                    in.eye_m, p.eye_m);
        std::fflush(stdout);
    }
    auto& eye = impl->eyes[1];
    result.view = *eye.sample_view;
    result.width = p.ow;
    result.height = p.oh;
    return result;
}

} // namespace Vulkan
