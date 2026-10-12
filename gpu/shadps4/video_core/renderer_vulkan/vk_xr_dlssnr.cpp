// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport VR DLSS-NR: see vk_xr_dlssnr.h.

#include "video_core/renderer_vulkan/vk_xr_dlssnr.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_xr_eye_iq.h"
#include "video_core/renderer_vulkan/xr_dlssnr_logic.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

namespace {

double NowMs() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// GPU profiler labels (BB_GPU_PROFILE=1): one per stream and placement.
constexpr u64 ProfileKey = 0x5A1Full << 40;

} // namespace

bool XrDlssNr::FlatPairs() {
    static const bool on = [] {
        const char* env = std::getenv("BB_XR_DLSSNR_FLAT_PAIRS");
        return env && env[0] == '1';
    }();
    return on;
}

struct XrDlssNr::Impl {
    const Instance& instance;
    Scheduler& scheduler;

    // The scratch image the DLL writes (the processed image's size and UNORM twin).
    VideoCore::UniqueImage work;
    vk::UniqueImageView work_view;
    vk::Format work_format = vk::Format::eUndefined;
    u32 work_width = 0, work_height = 0;

    // Pre-SR: per stream, the copy of the render-size image that is processed and handed to the
    // upscaler (its bytes as the source's, mutable: read as the work format by the DLL and DLSS,
    // as the presenter's view format by its passes, as R8G8B8A8 UNORM by the hybrid warp).
    struct PreImage {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view; ///< view_format, alpha 1, sampled
        vk::Format format = vk::Format::eUndefined, view_format = vk::Format::eUndefined;
        u32 width = 0, height = 0;
    };
    std::array<PreImage, XrDlssNrLogic::Streams> pre;

    struct Slot {
        bool feature = false;
        u32 width = 0, height = 0;
        double last_ms = -1.0;     ///< last evaluation (a gap starts the history over)
        bool place_reset = false;  ///< the placement changed since its last evaluation
    };
    std::array<Slot, Dlss::NrSlots> slots{};

    XrDlssNrLogic::PlaceTracker place_tracker;
    XrDlssNrLogic::SrInputTracker sr_tracker;

    vk::Format checked_format = vk::Format::eUndefined;
    bool format_writable = false;
    bool failed = false; ///< a creation/evaluation failed (until switched off and on)
    std::string last_reason = "-";
    double next_log = 0.0;
    std::array<u64, XrDlssNrLogic::Streams> evaluated{};
    u64 passes_run = 0;

    Impl(const Instance& instance_, Scheduler& scheduler_)
        : instance{instance_}, scheduler{scheduler_} {}

    void Reason(const std::string& reason) {
        if (last_reason == reason) {
            return;
        }
        last_reason = reason;
        if (reason.empty()) {
            std::printf("VR DLSS-NR: on (%s)\n",
                        XrDlssNrLogic::PlaceName(XrEyeIq::Current().nr.place));
        } else {
            std::printf("VR DLSS-NR: not used: %s\n", reason.c_str());
        }
        std::fflush(stdout);
    }

    bool Writable(vk::Format format) {
        if (format != checked_format) {
            checked_format = format;
            const auto properties = instance.GetPhysicalDevice().getFormatProperties(format);
            format_writable = bool(properties.optimalTilingFeatures &
                                   vk::FormatFeatureFlagBits::eStorageImage);
        }
        return format_writable;
    }

    /// The bridge and the DLL, or null (the reason is logged once).
    Dlss* Ready(const XrDlssNrLogic::Settings& s) {
        if (!s.on) {
            if (last_reason != "switched off" && last_reason != "-") {
                Reason("switched off");
            }
            failed = false; // switching it off and on tries again
            return nullptr;
        }
        if (failed) {
            return nullptr;
        }
        Dlss* dlss = Dlss::Get();
        if (!dlss) {
            Reason("bbport_dlss.dll or nvngx_dlss.dll is not next to the game");
            return nullptr;
        }
        if (!dlss->Available()) {
            Reason("DLSS is not available: " + dlss->Problem());
            return nullptr;
        }
        if (!dlss->HasNr()) {
            Reason("this bbport_dlss.dll has no DLSS-NR (use the one from this build)");
            return nullptr;
        }
        if (!dlss->NrInitialize()) {
            Reason("nvngx_dlssnr.dll is not next to the game or did not initialise (see the "
                   "DLSS: lines above)");
            return nullptr;
        }
        return dlss;
    }

    /// The work format of an image read as `format`, or eUndefined (the reason is logged).
    vk::Format CheckFormat(vk::Format format, const char* what) {
        const vk::Format work_format = vk::Format(XrDlssNrLogic::WorkFormat(u32(format)));
        if (work_format == vk::Format::eUndefined) {
            Reason(std::string(what) + " is " + vk::to_string(format) + ", not 8-bit RGBA/BGRA");
            return vk::Format::eUndefined;
        }
        if (!Writable(work_format)) {
            Reason(vk::to_string(work_format) + " cannot be a storage image on this GPU");
            return vk::Format::eUndefined;
        }
        return work_format;
    }

    static XrDlssNrLogic::FrameInfo Info(const Frame& frame) {
        return XrDlssNrLogic::WithFlatPairs(
            {
                .has_camera = frame.has_camera_view,
                .hdr = frame.is_hdr,
                .alt = frame.stereo_on,
                .alt_eye = frame.stereo.kind == xrstereo::Kind::Eye    ? int(frame.stereo.eye & 1)
                           : frame.stereo.kind == xrstereo::Kind::Hold ? -1
                                                                       : -2,
                .pair = frame.pair_on,
                .pair_eyes = frame.pair_kind == xrstereo::Kind::Eye,
            },
            FlatPairs());
    }

    void EnsureWork(vk::Format format, u32 width, u32 height) {
        if (work && work_format == format && work_width == width && work_height == height) {
            return;
        }
        const auto device = instance.GetDevice();
        if (work) {
            // Commands recorded earlier (this frame's other eye, submitted frames) may still use
            // it: destroyed once they are done.
            scheduler.DeferOperation(
                [image = std::move(work), view = std::move(work_view)]() mutable {
                    view.reset();
                    image = VideoCore::UniqueImage{};
                });
            std::printf("VR DLSS-NR: scratch image %ux%u -> %ux%u\n", work_width, work_height,
                        width, height);
        }
        work = VideoCore::UniqueImage(device, instance.GetAllocator());
        work.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        work_view = Check(device.createImageViewUnique({
            .image = vk::Image(work),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        work_format = format;
        work_width = width;
        work_height = height;
    }

    void EnsurePre(PreImage& p, vk::Format format, vk::Format view_format, u32 width,
                   u32 height) {
        if (p.image && p.format == format && p.view_format == view_format && p.width == width &&
            p.height == height) {
            return;
        }
        const auto device = instance.GetDevice();
        if (p.image) {
            scheduler.DeferOperation(
                [image = std::move(p.image), view = std::move(p.view)]() mutable {
                    view.reset();
                    image = VideoCore::UniqueImage{};
                });
        }
        p.image = VideoCore::UniqueImage(device, instance.GetAllocator());
        // Extended usage: the sRGB view format is not a storage format (only sampled).
        p.image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat |
                     vk::ImageCreateFlagBits::eExtendedUsage,
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        const vk::ImageViewUsageCreateInfo sampled{.usage = vk::ImageUsageFlagBits::eSampled};
        p.view = Check(device.createImageViewUnique({
            .pNext = &sampled,
            .image = vk::Image(p.image),
            .viewType = vk::ImageViewType::e2D,
            .format = view_format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        std::printf("VR DLSS-NR: pre-SR image (%s) %ux%u %s\n", &p == &pre[0] ? "eye L" : "eye R",
                    width, height, vk::to_string(format).c_str());
        std::fflush(stdout);
        p.format = format;
        p.view_format = view_format;
        p.width = width;
        p.height = height;
    }

    void ReleaseFeatures(const char* why) {
        Dlss* dlss = Dlss::Get();
        bool any = false;
        for (const Slot& slot : slots) {
            any = any || slot.feature;
        }
        if (!dlss || !any) {
            return;
        }
        scheduler.WaitSubmitted();
        for (u32 i = 0; i < slots.size(); ++i) {
            if (slots[i].feature) {
                dlss->NrReleaseFeature(i);
                slots[i] = {};
            }
        }
        std::printf("VR DLSS-NR: features released (%s)\n", why);
        std::fflush(stdout);
    }

    void Log(const XrDlssNrLogic::Settings& s, u32 width, u32 height) {
        const double now = NowMs();
        if (now < next_log) {
            return;
        }
        if (next_log > 0.0) {
            std::printf("VR DLSS-NR: 10 s: eye images L %llu R %llu, %llu passes; %ux%u, %d "
                        "pass(es), %s\n",
                        static_cast<unsigned long long>(evaluated[0]),
                        static_cast<unsigned long long>(evaluated[1]),
                        static_cast<unsigned long long>(passes_run), width, height,
                        XrDlssNrLogic::Passes(s), XrDlssNrLogic::PlaceName(s.place));
            std::fflush(stdout);
            evaluated = {};
            passes_run = 0;
        }
        next_log = now + 10000.0;
    }

    /// Records the passes on `image` (eGeneral, readable as `fmt`, width x height), in place: the
    /// DLL reads each pass's centre region and writes the scratch image, which is copied back.
    /// Leaves the image in eGeneral. False: a creation or evaluation failed (off until toggled).
    bool Process(vk::CommandBuffer cmdbuf, Dlss* dlss, const XrDlssNrLogic::Settings& s,
                 int stream, vk::Image image, vk::Format fmt, u32 width, u32 height,
                 bool pre_sr) {
        const int passes = XrDlssNrLogic::Passes(s);
        // The DLL's output is the image's size, written at the region's place (a region-sized
        // output at its origin fails with InvalidParameter: smoke test, BB_NR_SMOKE=...,scratch).
        EnsureWork(fmt, width, height);

        // The image read as its UNORM twin (the frame images and the pre-SR images are created
        // with eMutableFormat): the encoded values, as VirtualDesktop-OpenXR hands the
        // swapchain's bytes to the DLL.
        const auto device = instance.GetDevice();
        const vk::ImageView color_view = Check(device.createImageView({
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = fmt,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        scheduler.DeferOperation([device, color_view] { device.destroyImageView(color_view); });

        auto* profiler = GpuProfiler::Get();
        const bool mark = profiler && profiler->Records(&scheduler);
        const u64 resume = mark ? profiler->Current() : 0;
        if (mark) {
            profiler->Mark(ProfileKey | u64(stream) | (pre_sr ? 2u : 0u) | (u64(passes) << 4),
                           [&] {
                               return std::string{"VR DLSS-NR: eye "} + (stream ? "R" : "L") +
                                      (pre_sr ? " pre-SR" : " post") + " (" +
                                      std::to_string(passes) + " pass" +
                                      (passes > 1 ? "es)" : ")");
                           });
        }

        const auto barrier = [&](vk::Image target, vk::ImageLayout old_layout,
                                 vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                 vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
            const vk::ImageMemoryBarrier2 b{
                .srcStageMask = src_stage,
                .srcAccessMask = src_access,
                .dstStageMask = dst_stage,
                .dstAccessMask = dst_access,
                .oldLayout = old_layout,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = target,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        };
        const auto all = vk::PipelineStageFlagBits2::eAllCommands;
        const auto memory = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        const auto transfer = vk::PipelineStageFlagBits2::eTransfer;

        // Written before (the post-process pass, eye R's chain, or the pre-SR copy) in eGeneral.
        barrier(image, vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eMemoryWrite, all,
                memory);
        bool ok = true;
        for (int pass = 0; pass < passes; ++pass) {
            const XrDlssNrLogic::Rect region = XrDlssNrLogic::PassRegion(s, width, height, pass);
            const u32 slot_index = XrDlssNrLogic::Slot(stream, pass);
            Slot& slot = slots[slot_index];
            bool created = false;
            if (!slot.feature || slot.width != region.width || slot.height != region.height) {
                if (slot.feature) {
                    // The feature may still be used by submitted work.
                    scheduler.WaitSubmitted();
                    dlss->NrReleaseFeature(slot_index);
                    slot = {};
                }
                if (!dlss->NrCreateFeature(cmdbuf, slot_index, region.width, region.height)) {
                    failed = true;
                    ok = false;
                    std::printf("VR DLSS-NR: feature creation failed (eye %c, pass %d, %ux%u); "
                                "off until it is switched off and on\n",
                                stream ? 'R' : 'L', pass + 1, region.width, region.height);
                    std::fflush(stdout);
                    break;
                }
                slot.feature = true;
                slot.width = region.width;
                slot.height = region.height;
                created = true;
                std::printf("VR DLSS-NR: eye %c pass %d feature (%s): %ux%u at %u,%u of %ux%u\n",
                            stream ? 'R' : 'L', pass + 1, pre_sr ? "pre-SR" : "post",
                            region.width, region.height, region.x, region.y, width, height);
                std::fflush(stdout);
            }
            const double now = NowMs();
            const bool reset =
                XrDlssNrLogic::NeedsReset(created, slot.last_ms, now, slot.place_reset);
            slot.last_ms = now;
            slot.place_reset = false;

            // The scratch image's old contents are not needed (the history is in the feature).
            barrier(vk::Image(work), vk::ImageLayout::eUndefined, all, memory, all, memory);
            const bool evaluated_ok = dlss->NrEvaluate(
                cmdbuf, slot_index,
                {
                    .color = {image, color_view, fmt, vk::ImageAspectFlagBits::eColor, width,
                              height},
                    .output = {vk::Image(work), *work_view, fmt, vk::ImageAspectFlagBits::eColor,
                               work_width, work_height},
                    .color_rect = {region.x, region.y, region.width, region.height},
                    .output_rect = {region.x, region.y, region.width, region.height},
                    .reset = reset,
                    .style = s.style,
                    .intensity = s.intensity,
                    .local_tone = s.tone,
                    .local_structure = s.structure,
                    .skin_structure = s.skin,
                });
            if (!evaluated_ok) {
                failed = true;
                ok = false;
                std::printf("VR DLSS-NR: evaluation failed (eye %c, pass %d); off until it is "
                            "switched off and on\n",
                            stream ? 'R' : 'L', pass + 1);
                std::fflush(stdout);
                break;
            }
            // The result over the region of the image.
            barrier(vk::Image(work), vk::ImageLayout::eGeneral, all,
                    vk::AccessFlagBits2::eMemoryWrite, transfer,
                    vk::AccessFlagBits2::eTransferRead);
            barrier(image, vk::ImageLayout::eGeneral, all, memory, transfer,
                    vk::AccessFlagBits2::eTransferWrite);
            const vk::ImageCopy copy{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .srcOffset = {s32(region.x), s32(region.y), 0},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstOffset = {s32(region.x), s32(region.y), 0},
                .extent = {region.width, region.height, 1},
            };
            cmdbuf.copyImage(vk::Image(work), vk::ImageLayout::eGeneral, image,
                             vk::ImageLayout::eGeneral, copy);
            barrier(image, vk::ImageLayout::eGeneral, transfer,
                    vk::AccessFlagBits2::eTransferWrite, all, memory);
            ++passes_run;
        }
        if (mark && resume) {
            profiler->Resume(resume);
        }
        if (ok) {
            ++evaluated[stream];
            Log(s, width, height);
        }
        return ok;
    }
};

XrDlssNr::XrDlssNr(const Instance& instance, Scheduler& scheduler)
    : impl{std::make_unique<Impl>(instance, scheduler)} {}

XrDlssNr::~XrDlssNr() {
    if (Dlss* dlss = Dlss::Get(); dlss && dlss->Available()) {
        impl->ReleaseFeatures("shutdown");
    }
}

bool XrDlssNr::SrInputChanged() {
    Impl& m = *impl;
    const XrDlssNrLogic::Settings s = XrEyeIq::Current().nr;
    if (m.place_tracker.Changed(s.place)) {
        // Every history starts over, also of features whose region size stays (DLAA).
        for (Impl::Slot& slot : m.slots) {
            slot.place_reset = true;
        }
        std::printf("VR DLSS-NR: placement -> %s: DLSS-NR histories start over\n",
                    XrDlssNrLogic::PlaceName(s.place));
        std::fflush(stdout);
    }
    const bool changed = m.sr_tracker.Changed(XrDlssNrLogic::SrInputProcessed(s));
    if (changed) {
        std::printf("VR DLSS-NR: DLSS SR's input is now %s: DLSS histories start over\n",
                    XrDlssNrLogic::SrInputProcessed(s) ? "DLSS-NR's output (pre-SR)"
                                                       : "the game's image");
        std::fflush(stdout);
    }
    return changed;
}

void XrDlssNr::Run(vk::CommandBuffer cmdbuf, const Frame& frame, vk::Format format, bool eye_r) {
    Impl& m = *impl;
    const XrDlssNrLogic::Settings s = XrEyeIq::Current().nr;
    if (s.on && s.place != XrDlssNrLogic::Place::Post) {
        return; // RunPre's
    }
    Dlss* dlss = m.Ready(s);
    if (!dlss) {
        return;
    }

    // The image and its history.
    const XrDlssNrLogic::FrameInfo info = Impl::Info(frame);
    int stream = -1;
    vk::Image image{};
    vk::Format image_format = format;
    u32 width = frame.width, height = frame.height;
    if (!eye_r) {
        stream = XrDlssNrLogic::StreamFor(info);
        image = frame.image;
    } else if (frame.pair_on && info.pair_eyes && frame.eye_r_valid && frame.eye_r_out &&
               frame.eye_r_image == frame.eye_r_out && frame.has_camera_view && !frame.is_hdr) {
        stream = 1;
        image = frame.eye_r_out;
        image_format = frame.eye_r_out_format;
        width = frame.eye_r_out_width;
        height = frame.eye_r_out_height;
    }
    if (stream < 0 || !image || width < XrDlssNrLogic::MinRegion ||
        height < XrDlssNrLogic::MinRegion) {
        return;
    }
    const vk::Format work_format = m.CheckFormat(image_format, "the eye image");
    if (work_format == vk::Format::eUndefined) {
        return;
    }
    m.Reason("");
    m.Process(cmdbuf, dlss, s, stream, image, work_format, width, height, false);
}

XrDlssNr::PreResult XrDlssNr::RunPre(vk::CommandBuffer cmdbuf, const Frame& frame,
                                     vk::Image source, vk::Format view_format, u32 width,
                                     u32 height, bool eye_r) {
    Impl& m = *impl;
    const XrDlssNrLogic::Settings s = XrEyeIq::Current().nr;
    if (!s.on || s.place != XrDlssNrLogic::Place::Pre) {
        if (!s.on) {
            m.Ready(s); // the "switched off" line, and a failure is retried after a toggle
        }
        return {};
    }
    Dlss* dlss = m.Ready(s);
    if (!dlss) {
        return {};
    }
    const int stream = eye_r ? 1 : XrDlssNrLogic::PreStreamFor(Impl::Info(frame));
    if (stream < 0 || !source || width < XrDlssNrLogic::MinRegion ||
        height < XrDlssNrLogic::MinRegion) {
        return {};
    }
    const vk::Format work_format = m.CheckFormat(view_format, "the display buffer");
    if (work_format == vk::Format::eUndefined) {
        return {};
    }
    m.Reason("");

    Impl::PreImage& pre = m.pre[stream];
    m.EnsurePre(pre, work_format, view_format, width, height);
    const vk::Image target = vk::Image(pre.image);

    // The source's bytes as stored (both 32-bit formats: the copy is bitwise), so the pre-SR
    // image reads exactly as the source in any of its views.
    const std::array before{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = source,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined, // every texel is rewritten
            .newLayout = vk::ImageLayout::eGeneral,
            .image = target,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
    };
    cmdbuf.pipelineBarrier2(
        {.imageMemoryBarrierCount = u32(before.size()), .pImageMemoryBarriers = before.data()});
    const vk::ImageCopy copy{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {width, height, 1},
    };
    cmdbuf.copyImage(source, vk::ImageLayout::eGeneral, target, vk::ImageLayout::eGeneral, copy);

    if (!m.Process(cmdbuf, dlss, s, stream, target, work_format, width, height, true)) {
        return {};
    }
    return {.image = target, .view = *pre.view, .width = width, .height = height};
}

} // namespace Vulkan
