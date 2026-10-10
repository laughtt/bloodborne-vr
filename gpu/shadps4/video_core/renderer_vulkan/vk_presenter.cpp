// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "bbport_overlay.h"
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_xr.h"
#include "video_core/renderer_vulkan/vk_xr_eye_iq.h"
#include "video_core/renderer_vulkan/vk_xr_dlss.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/xr_opaque_frag.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <vk_mem_alloc.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // bbport: winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

namespace Vulkan {

// bbport VR image quality: the display buffer size (w << 32 | h), from PrepareFrame (GPU thread)
// to Present (the frame size with BB_XR_UPSCALE=fsr1).
static std::atomic<u64> xr_iq_source{(u64(1920) << 32) | 1080};

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// bbport: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    xr_fxaa_pass.Create(device, instance.GetAllocator(), num_images); // bbport VR (BB_XR_AA)
    xr_dlss = std::make_unique<XrDlss>(instance, draw_scheduler, runtime, texture_cache);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);
    BbOverlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);

}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        if (frame.hud_image) {
            vmaDestroyImage(instance.GetAllocator(), frame.hud_image, frame.hud_allocation);
        }
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// bbport: raw copy of an image for BB_FRAME_DUMP_TRIGGER, written once the GPU is done.
static void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* dir = std::getenv("BB_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  index, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

/// bbport VR HUD layer: the upscaler's UI snapshot into the frame's HUD image (below).
static void CopyHud(const Instance& instance, vk::CommandBuffer cmdbuf, Frame* frame,
                    const TemporalUpscaler::Hud& hud);

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // bbport: BB_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to BB_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    // BB_FRAME_DUMP_COUNT=<n> dumps n consecutive frames per trigger (stereo pairs).
    static const char* frame_dump_trigger = std::getenv("BB_FRAME_DUMP_TRIGGER");
    static const int frame_dump_count = [] {
        const char* env = std::getenv("BB_FRAME_DUMP_COUNT");
        return env ? std::clamp(std::atoi(env), 1, 64) : 1;
    }();
    static int frame_dump_index = 0;
    static int frame_dump_left = 0;
    if (frame_dump_trigger && std::remove(frame_dump_trigger) == 0) {
        frame_dump_left = frame_dump_count;
    }
    const bool frame_dump = frame_dump_left > 0;
    if (frame_dump) {
        --frame_dump_left;
    }
    // bbport: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();
    // bbport VR: a new frame size recreates the FSR / FXAA pass images and the DLSS eye outputs
    // in this frame's passes; earlier frames may still read them on the GPU (BB_FRAMES_AHEAD).
    if (const XrResize::Size size{frame->width, frame->height}; size != prepared_size) {
        if (prepared_size.Valid()) {
            draw_scheduler.WaitSubmitted();
            if (Xr::Get()) {
                std::printf("VR: frames now %ux%u (were %ux%u): GPU idle before the passes "
                            "resize\n",
                            size.width, size.height, prepared_size.width, prepared_size.height);
                std::fflush(stdout);
            }
        }
        prepared_size = size;
    }
    // bbport VR: this frame's scene camera (prepared on the GPU thread right after its draws).
    frame->has_camera_view = rasterizer->GetCameraMotion().CurrentView(frame->camera_view);
    // bbport VR (BB_XR_MODE=alt): the eye the frame's camera was set up for, matched here, in
    // flip order, so a lost or repeated flip never moves a frame to the other eye.
    frame->stereo_on = XrStereo::Get().Enabled();
    if (frame->stereo_on) {
        const auto& motion = rasterizer->GetCameraMotion();
        std::array<float, 4> proj{};
        std::array<float, 64> shadow{};
        const bool has_proj = frame->has_camera_view && motion.CurrentProjection(proj);
        const bool has_shadow = frame->has_camera_view && motion.CurrentShadow(shadow);
        frame->stereo = XrStereo::Get().OnFrame(
            frame->has_camera_view ? frame->camera_view.data() : nullptr, has_proj ? proj[0] : 0.0f,
            has_shadow ? shadow.data() : nullptr, BbStats::draws.load(std::memory_order_relaxed));
        if (frame_dump) {
            const auto& s = frame->stereo;
            std::printf("VR stereo: frame dump %d: %s, seq %llu, lag %u, side %d, frame %llu "
                        "(%s)\n",
                        frame_dump_index,
                        s.kind == xrstereo::Kind::Eye ? (s.eye ? "eye R" : "eye L")
                        : s.kind == xrstereo::Kind::Mono ? "mono"
                                                         : "held",
                        static_cast<unsigned long long>(s.view.seq), s.lag, s.side,
                        static_cast<unsigned long long>(s.frame), s.reason);
            // The frame's shadow cascade constants (do the eyes share their shadow maps?).
            static const bool log_shadow = std::getenv("BB_VR_STEREO_LOG") != nullptr;
            if (log_shadow && has_shadow) {
                std::string values;
                for (const float v : shadow) {
                    char number[24];
                    std::snprintf(number, sizeof(number), " %.7g", v);
                    values += number;
                }
                std::printf("VR stereo: frame dump %d shadow cascades:%s\n", frame_dump_index,
                            values.c_str());
            }
        }
    }

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });
    // bbport VR HUD layer: the UI of the frame in this display buffer (the upscaler's snapshot,
    // taken at the display pass that wrote it) into the frame's own HUD image.
    frame->hud_valid = false;
    frame->hud_fullscreen = false;
    if (TemporalUpscaler::Hud hud{}; XrHud::Separating() && rasterizer->GetUpscaler().LatestHud(hud)) {
        CopyHud(instance, cmdbuf, frame, hud);
        if (frame_dump) {
            DumpRaw(instance, draw_scheduler, cmdbuf, frame->hud_image, vk::ImageLayout::eGeneral,
                    frame->hud_width, frame->hud_height, "hud", frame_dump_index);
            std::printf("VR HUD: frame dump %d: hud image %ux%u%s\n", frame_dump_index,
                        frame->hud_width, frame->hud_height,
                        frame->hud_fullscreen ? " (covers the screen: cinema)" : "");
        }
    }

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                    vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                    image.info.size.height, "display", frame_dump_index);
        }
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio.store(static_cast<float>(image_size.width) / static_cast<float>(image_size.height),
                         std::memory_order_relaxed);

    // bbport VR image quality (vk_xr_eye_iq.h): spatial AA of 3D frames, FSR 1 to the eye size.
    // Both without history: alternate-eye stereo stays clean.
    const XrEyeIq::Settings iq = XrEyeIq::Current();
    xr_iq_source.store((u64(image_size.width) << 32) | image_size.height, std::memory_order_relaxed);
    auto* iq_profiler = GpuProfiler::Get();
    const bool iq_profile = iq_profiler && iq_profiler->Records(&draw_scheduler) &&
                            (iq.fxaa || iq.fsr1 || iq.dlss);
    const u64 iq_resume = iq_profile ? iq_profiler->Current() : 0;
    if (iq_profile) {
        const bool rcas = iq.fsr1 && iq.sharpen > 0.0f;
        iq_profiler->Mark(0x5A1Eull << 40 | (iq.fxaa ? 1 : 0) | (iq.fsr1 ? 2 : 0) | (rcas ? 4 : 0),
                          [&] {
                              return fmt::format("VR IQ:{}{}{}", iq.fxaa ? " fxaa" : "",
                                                 iq.fsr1 ? (rcas ? " fsr1 easu+rcas" : " fsr1 easu")
                                                         : "",
                                                 iq.dlss ? " dlss" : "");
                          });
    }
    // bbport VR DLSS (vk_xr_dlss.h, BB_XR_UPSCALE=dlss): per-eye DLSS from the display buffer to
    // the eye image size. Frames it does not take (menus, held frames, no RTX/DLL) get FSR 1 to
    // the same size, and FXAA when DLSS cannot run at all (the log says why).
    XrDlss::Result dlss{};
    {
        int eye = 0; // without alternate eyes: one stream
        if (frame->stereo_on) {
            const auto& d = frame->stereo;
            eye = d.kind == xrstereo::Kind::Eye    ? (d.eye & 1)
                  : d.kind == xrstereo::Kind::Hold ? -1
                                                   : -2;
        }
        dlss = xr_dlss->Run(cmdbuf, {
                                        .settings = &iq,
                                        .display = upscaled ? VideoCore::ImageId{} : image_id,
                                        .sample_format = view_info.format,
                                        .motion = &rasterizer->GetCameraMotion(),
                                        .stereo = frame->stereo_on,
                                        .eye = eye,
                                        .has_camera = frame->has_camera_view,
                                        .hdr = frame->is_hdr,
                                        .flat_temporal = rasterizer->GetUpscaler().FlatJitterOn(),
                                        .out_width = frame->width,
                                        .out_height = frame->height,
                                    });
    }
    if (dlss.view) {
        image_view = dlss.view;
        image_size = vk::Extent2D{dlss.width, dlss.height};
    } else if ((iq.fxaa || dlss.fallback_fxaa) && frame->has_camera_view && !frame->is_hdr) {
        image_view = xr_fxaa_pass.Render(cmdbuf, image_view, image_size, {});
    }
    HostPasses::FsrPass::Settings iq_fsr = fsr_settings;
    if (iq.fsr1 || dlss.fallback_fsr1) {
        iq_fsr.enable = true;
        iq_fsr.use_rcas = iq.sharpen > 0.0f;
        iq_fsr.rcas_attenuation = 2.0f * (1.0f - iq.sharpen); // stops: 0 = sharpest
    } else if (dlss.view && iq.dlss_sharpen > 0.0f) {
        // RCAS over the DLSS output at its own size (BB_XR_DLSS_SHARPEN).
        iq_fsr.enable = true;
        iq_fsr.use_rcas = true;
        iq_fsr.rcas_only = true;
        iq_fsr.rcas_attenuation = 2.0f * (1.0f - iq.dlss_sharpen);
    }
    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 iq_fsr, frame->is_hdr);
    if (iq_profile && iq_resume) {
        iq_profiler->Resume(iq_resume);
    }

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);
    if (frame_dump) {
        // The presented image as the swapchain blit reads it (General after the pass).
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "output", frame_dump_index);
        ++frame_dump_index;
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    // bbport: the GPU command thread runs at most BB_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("BB_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

static void CopyHud(const Instance& instance, vk::CommandBuffer cmdbuf, Frame* frame,
                    const TemporalUpscaler::Hud& hud) {
    if (!frame->hud_image || frame->hud_width != hud.width || frame->hud_height != hud.height ||
        frame->hud_format != hud.format) {
        if (frame->hud_image) {
            // The frame is free (GetRenderFrame waited for its present), so nothing reads it.
            vmaDestroyImage(instance.GetAllocator(), frame->hud_image, frame->hud_allocation);
            frame->hud_image = vk::Image{};
        }
        const VkImageCreateInfo image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VkFormat(hud.format),
            .extent = {hud.width, hud.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc_info{
            .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        VkImage image{};
        if (vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info, &image,
                           &frame->hud_allocation, nullptr) != VK_SUCCESS) {
            return;
        }
        frame->hud_image = vk::Image{image};
        frame->hud_width = hud.width;
        frame->hud_height = hud.height;
        frame->hud_format = hud.format;
        SetObjectName(instance.GetDevice(), frame->hud_image, "Frame HUD image #{}", frame->id);
    }
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageSubresourceLayers layers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    const std::array before{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = hud.image,
            .subresourceRange = range,
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = frame->hud_image,
            .subresourceRange = range,
        },
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(before.size()),
                             .pImageMemoryBarriers = before.data()});
    cmdbuf.copyImage(hud.image, vk::ImageLayout::eGeneral, frame->hud_image,
                     vk::ImageLayout::eGeneral,
                     vk::ImageCopy{.srcSubresource = layers,
                                   .dstSubresource = layers,
                                   .extent = {hud.width, hud.height, 1}});
    const vk::ImageMemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->hud_image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &after});
    frame->hud_valid = true;
    frame->hud_fullscreen = hud.fullscreen;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    frame->has_camera_view = false;
    frame->hud_valid = false;
    frame->hud_fullscreen = false;
    frame->stereo_on = XrStereo::Get().Enabled();
    if (frame->stereo_on) {
        frame->stereo = XrStereo::Get().OnFrame(nullptr, 0.0f, nullptr,
                                                BbStats::draws.load(std::memory_order_relaxed));
    }

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

namespace {
// bbport VR: same channel order and texel size -> raw copy into the sRGB headset image (the
// window image already holds sRGB-encoded values); anything else is converted by a blit.
bool SameChannelOrder(vk::Format a, vk::Format b) {
    const auto bgra = [](vk::Format f) {
        return f == vk::Format::eB8G8R8A8Unorm || f == vk::Format::eB8G8R8A8Srgb;
    };
    const auto rgba = [](vk::Format f) {
        return f == vk::Format::eR8G8B8A8Unorm || f == vk::Format::eR8G8B8A8Srgb;
    };
    return (bgra(a) && bgra(b)) || (rgba(a) && rgba(b));
}

/// Sets alpha to 1 over the headset screen image (eColorAttachmentOptimal): an alpha-only write.
void MakeFlatOpaque(vk::Device device, vk::CommandBuffer cmdbuf, vk::Extent2D extent,
                    const Xr::FlatTarget& flat) {
    // Plain handles kept for the process lifetime: static Unique* would outlive the device.
    static vk::PipelineLayout layout;
    static vk::Pipeline pipeline;
    static vk::Format pipeline_format = vk::Format::eUndefined;
    static std::unordered_map<VkImage, vk::ImageView> views;
    if (!pipeline || pipeline_format != flat.format) {
        const vk::UniqueShaderModule vs{CompileSPV(FS_TRI_VERT, device), device};
        const vk::UniqueShaderModule fs{CompileSPV(XR_OPAQUE_FRAG, device), device};
        const std::array stages{
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex, .module = *vs, .pName = "main"},
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment, .module = *fs, .pName = "main"},
        };
        if (!layout) {
            layout = Check<"create xr opaque layout">(
                device.createPipelineLayout(vk::PipelineLayoutCreateInfo{}));
        }
        const vk::PipelineRenderingCreateInfo rendering{
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &flat.format,
        };
        const vk::PipelineVertexInputStateCreateInfo vertex_input{};
        const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
            .topology = vk::PrimitiveTopology::eTriangleList};
        const vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
        const vk::PipelineRasterizationStateCreateInfo raster{
            .polygonMode = vk::PolygonMode::eFill,
            .cullMode = vk::CullModeFlagBits::eNone,
            .lineWidth = 1.0f,
        };
        const vk::PipelineMultisampleStateCreateInfo multisample{
            .rasterizationSamples = vk::SampleCountFlagBits::e1};
        const vk::PipelineColorBlendAttachmentState alpha_only{
            .blendEnable = false,
            .colorWriteMask = vk::ColorComponentFlagBits::eA,
        };
        const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 1,
                                                          .pAttachments = &alpha_only};
        const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        const vk::PipelineDynamicStateCreateInfo dynamic{
            .dynamicStateCount = u32(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data(),
        };
        const vk::GraphicsPipelineCreateInfo create{
            .pNext = &rendering,
            .stageCount = u32(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport,
            .pRasterizationState = &raster,
            .pMultisampleState = &multisample,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamic,
            .layout = layout,
        };
        auto [result, created] = device.createGraphicsPipeline({}, create);
        ASSERT_MSG(result == vk::Result::eSuccess, "xr opaque pipeline: {}", vk::to_string(result));
        pipeline = created;
        pipeline_format = flat.format;
    }
    auto& view = views[VkImage(flat.image)];
    if (!view) {
        view = Check<"create xr flat view">(device.createImageView(vk::ImageViewCreateInfo{
            .image = flat.image,
            .viewType = vk::ImageViewType::e2D,
            .format = flat.format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    }
    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::Rect2D area{{0, 0}, extent};
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = area,
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    cmdbuf.setViewport(0, vk::Viewport{0.0f, 0.0f, float(extent.width), float(extent.height),
                                       0.0f, 1.0f});
    cmdbuf.setScissor(0, area);
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();
}

/// An image of the window's format (the window image in ePresentSrcKHR, or a frame image in
/// eGeneral for an eye, BB_XR_MODE=alt) into a headset swapchain image; both end where they began.
void CopyToFlat(vk::Device device, vk::CommandBuffer cmdbuf, vk::Image swapchain_image,
                vk::Format swapchain_format, vk::Extent2D extent, const Xr::FlatTarget& flat,
                vk::ImageLayout source_layout = vk::ImageLayout::ePresentSrcKHR,
                bool opaque = true) {
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const auto barrier = [&](vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                             vk::AccessFlags src, vk::AccessFlags dst) {
        return vk::ImageMemoryBarrier{
            .srcAccessMask = src,
            .dstAccessMask = dst,
            .oldLayout = from,
            .newLayout = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
    };
    const std::array before{
        barrier(swapchain_image, source_layout, vk::ImageLayout::eTransferSrcOptimal,
                vk::AccessFlagBits::eMemoryWrite, vk::AccessFlagBits::eTransferRead),
        barrier(flat.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                vk::AccessFlagBits::eNone, vk::AccessFlagBits::eTransferWrite),
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                           vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, before);
    const vk::ImageSubresourceLayers layers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    // BB_XR_TEST_FILL=1: solid magenta instead of the game, to tell the layer from its contents.
    static const bool test_fill = std::getenv("BB_XR_TEST_FILL") && *std::getenv("BB_XR_TEST_FILL") == '1';
    if (test_fill) {
        const vk::ClearColorValue magenta{std::array<float, 4>{1.0f, 0.0f, 1.0f, 1.0f}};
        cmdbuf.clearColorImage(flat.image, vk::ImageLayout::eTransferDstOptimal, magenta, range);
    } else if (SameChannelOrder(swapchain_format, flat.format)) {
        const vk::ImageCopy copy{
            .srcSubresource = layers,
            .dstSubresource = layers,
            .extent = {extent.width, extent.height, 1},
        };
        cmdbuf.copyImage(swapchain_image, vk::ImageLayout::eTransferSrcOptimal, flat.image,
                         vk::ImageLayout::eTransferDstOptimal, copy);
    } else {
        const std::array<vk::Offset3D, 2> corners{
            vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(extent.width), s32(extent.height), 1}};
        const vk::ImageBlit blit{
            .srcSubresource = layers,
            .srcOffsets = corners,
            .dstSubresource = layers,
            .dstOffsets = corners,
        };
        cmdbuf.blitImage(swapchain_image, vk::ImageLayout::eTransferSrcOptimal, flat.image,
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
    }
    const std::array after{
        barrier(swapchain_image, vk::ImageLayout::eTransferSrcOptimal, source_layout,
                vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eNone),
        barrier(flat.image, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eColorAttachmentOptimal, vk::AccessFlagBits::eTransferWrite,
                vk::AccessFlagBits::eColorAttachmentRead |
                    vk::AccessFlagBits::eColorAttachmentWrite),
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, after);
    if (opaque) {
        MakeFlatOpaque(device, cmdbuf, extent, flat);
    }
}
} // namespace

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // bbport: a minimized window has no pixels on Windows (0x0); keep the swapchain and skip
    // presenting until the window is restored.
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }

    // Recreate the swapchain if the window was resized.
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // bbport: the game frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    std::optional<Xr::FlatTarget> xr_flat, xr_eye, xr_hud;
    bool hide_hud = false;
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        // bbport VR image quality (BB_XR_UPSCALE=fsr1|dlss): the frame, which an eye swapchain
        // copies 1:1, at its own size instead of the window's; the window gets it scaled down.
        // The size is computed here in full and published once (the GPU thread reads it in
        // GetRenderFrame): writing the window size first and the eye size after it let the GPU
        // thread catch the window size for one frame, which recreated an eye swapchain and its
        // DLSS feature twice under in-flight work (device lost, 2026-10-10 crashes). A new size
        // also has to hold for a few presents (XrResize::SizeDebounce): a one-frame target (a
        // frame with another display buffer size, say) never reaches the frames and swapchains.
        {
            XrResize::Inputs in{};
            in.window = FitGameSize(s32(extent.width), s32(extent.height));
            in.source = XrResize::Unpack(xr_iq_source.load(std::memory_order_relaxed));
            XrEyeIq::TargetSize(XrEyeIq::Current(), in.source.width, in.source.height,
                                in.iq.width, in.iq.height);
            const XrResize::Size before = frame_size_debounce.Applied();
            const XrResize::Size target = XrResize::Target(in);
            if (frame_size_debounce.Update(target)) {
                const XrResize::Size after = frame_size_debounce.Applied();
                expected_frame_size.store(XrResize::Pack(after), std::memory_order_release);
                if (before.Valid() && Xr::Get()) {
                    std::printf("VR: frame size %ux%u -> %ux%u (%s; window %ux%u, display buffer "
                                "%ux%u, eye IQ %ux%u)\n",
                                before.width, before.height, after.width, after.height,
                                XrResize::ChangeReason(frame_size_inputs, in), in.window.width,
                                in.window.height, in.source.width, in.source.height,
                                in.iq.width, in.iq.height);
                    std::fflush(stdout);
                }
                frame_size_inputs = in;
            } else if (target.Valid() && target != before && frame_size_debounce.Pending() == 1 &&
                       Xr::Get()) {
                // A different size for the first time: held back. Logged (at most once a second)
                // with what it was made of, so a one-frame flip shows its cause.
                static auto next_log = std::chrono::steady_clock::time_point{};
                static u32 held_back = 0;
                ++held_back;
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_log) {
                    next_log = now + std::chrono::seconds(1);
                    std::printf("VR: frame size: holding back %ux%u (stays %ux%u until it holds "
                                "3 presents; %s; window %ux%u, display buffer %ux%u, eye IQ "
                                "%ux%u; %u so far)\n",
                                target.width, target.height, before.width, before.height,
                                XrResize::ChangeReason(frame_size_inputs, in), in.window.width,
                                in.window.height, in.source.width, in.source.height,
                                in.iq.width, in.iq.height, held_back);
                    std::fflush(stdout);
                }
            }
        }
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                         vk::Filter::eLinear);
        // bbport: the settings menu / FPS counter over the frame, at display resolution.
        const bool overlay = BbOverlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            BbOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
        // bbport VR (cinema): the finished window image, menu included, onto the headset screen.
        // TODO(VR overlay quad): fine on the cinema screen, but once the game image is a
        // projection layer (BB_XR_MODE=mono, Phase 2) the menu must leave it: copy to the headset
        // before BbOverlay::Render; a bridge overlay swapchain (new ABI AcquireOverlay/
        // ReleaseOverlay, XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT so ImGui's UNORM pipeline draws
        // through a UNORM view of the sRGB image) cleared to transparent; BbOverlay::Render into
        // it; a quad ~1.2 m out above the screen layer, premultiplied alpha (what ImGui leaves on
        // transparent black). ImGui draws once per frame, so the window then needs that image
        // blended back in (or no menu on the desktop while in VR).
        if (Xr* xr = Xr::Get()) {
            // bbport VR (BB_XR_MODE=alt, vk_xr_stereo.h): a frame set up for an eye goes to that
            // eye's swapchain, the game image itself (no window letterbox, no menu overlay); a
            // held frame goes nowhere (both eyes keep their last image); the others (menus,
            // loading screens) take the mono path below.
            const bool alt = frame->stereo_on && xr->HasEyes();
            // bbport VR HUD layer: a separated UI that covers the screen (in-game loading
            // screens, full-screen menus) goes to the flat cinema screen like the menus without
            // a scene, instead of being stretched over the eye frustum.
            const bool hud = frame->hud_valid && XrHud::Separating();
            const bool hud_cinema = hud && frame->hud_fullscreen;
            if (hud_cinema) {
                if (!is_reusing_frame) {
                    XrChainResize(2, frame->hud_width, frame->hud_height);
                    xr_flat = xr->AcquireFlat(frame->hud_width, frame->hud_height,
                                              frame->hud_format);
                    if (xr_flat) {
                        xr_chain_sizes[2].Acquired({frame->hud_width, frame->hud_height});
                        CopyToFlat(instance.GetDevice(), cmdbuf, frame->hud_image,
                                   frame->hud_format, {frame->hud_width, frame->hud_height},
                                   *xr_flat, vk::ImageLayout::eGeneral);
                    }
                }
            } else if (alt && frame->stereo.kind == xrstereo::Kind::Eye) {
                if (!is_reusing_frame) {
                    const int eye_chain = frame->stereo.eye & 1;
                    XrChainResize(eye_chain, frame->width, frame->height);
                    xr_eye = xr->AcquireEye(frame->stereo.eye, frame->width, frame->height,
                                            swapchain.GetSurfaceFormat().format);
                    if (xr_eye) {
                        xr_chain_sizes[eye_chain].Acquired({frame->width, frame->height});
                        CopyToFlat(instance.GetDevice(), cmdbuf, frame->image,
                                   swapchain.GetSurfaceFormat().format,
                                   {frame->width, frame->height}, *xr_eye,
                                   vk::ImageLayout::eGeneral);
                    }
                }
            } else if (!alt || frame->stereo.kind == xrstereo::Kind::Mono) {
                XrChainResize(2, extent.width, extent.height);
                xr_flat = xr->AcquireFlat(extent.width, extent.height,
                                          swapchain.GetSurfaceFormat().format);
                if (xr_flat) {
                    xr_chain_sizes[2].Acquired({extent.width, extent.height});
                    CopyToFlat(instance.GetDevice(), cmdbuf, swapchain_image,
                               swapchain.GetSurfaceFormat().format, extent, *xr_flat);
                }
            }
            // The HUD quad: this frame's UI (premultiplied alpha kept), or hidden when the frame
            // carries its UI itself or went to the cinema screen.
            if (!is_reusing_frame && XrHud::Separating()) {
                if (hud && !hud_cinema) {
                    XrChainResize(3, frame->hud_width, frame->hud_height);
                    xr_hud = xr->AcquireHud(frame->hud_width, frame->hud_height,
                                            frame->hud_format);
                    if (xr_hud) {
                        xr_chain_sizes[3].Acquired({frame->hud_width, frame->hud_height});
                        CopyToFlat(instance.GetDevice(), cmdbuf, frame->hud_image,
                                   frame->hud_format, {frame->hud_width, frame->hud_height},
                                   *xr_hud, vk::ImageLayout::eGeneral, false);
                    }
                } else {
                    hide_hud = true;
                }
            }
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);
    if (xr_flat) {
        const bool hud_cinema = frame->hud_valid && frame->hud_fullscreen;
        Xr::Get()->ReleaseFlat(frame->has_camera_view && !hud_cinema ? frame->camera_view.data()
                                                                     : nullptr);
        xr_flat.reset();
    }
    if (xr_hud) {
        Xr::Get()->ReleaseHud();
        xr_hud.reset();
    } else if (hide_hud) {
        Xr::Get()->HideHud();
    }
    if (xr_eye) {
        Xr::Get()->ReleaseEye(frame->stereo.eye, frame->stereo.view);
        xr_eye.reset();
    }

    // Present to swapchain.
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        if (!swapchain.Present() && window.GetWidth() != 0 && window.GetHeight() != 0) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }

    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    const XrResize::Size expected =
        XrResize::Unpack(expected_frame_size.load(std::memory_order_acquire));
    if (frame->width != expected.width || frame->height != expected.height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected.width, expected.height);
    }

    return frame;
}

XrResize::Size Presenter::FitGameSize(s32 width, s32 height) const {
    if (width <= 0 || height <= 0) {
        return {}; // no surface (minimized window): the debounce keeps the last frame size
    }
    const float ratio = (float)width / (float)height;
    const float game_ratio = expected_ratio.load(std::memory_order_relaxed);
    XrResize::Size size{u32(width), u32(height)};
    if (ratio > game_ratio) {
        size.width = static_cast<u32>(height * game_ratio);
    } else {
        size.height = static_cast<u32>(width / game_ratio);
    }
    return size;
}

void Presenter::XrChainResize(int chain, u32 width, u32 height) {
    // The bridge destroys and recreates a swapchain whose size changes inside its Acquire
    // (xrDestroySwapchain). Copies into the old images from earlier presents, and the runtime's
    // own submissions on the shared queue, may still run: wait for the queue first (resizes are
    // rare: debounced frame size, cinema <-> mono switches).
    const XrResize::Size size{width, height};
    if (!xr_chain_sizes[chain].NeedsWait(size)) {
        return;
    }
    static constexpr const char* names[] = {"eye L", "eye R", "flat", "HUD"};
    const XrResize::Size last = xr_chain_sizes[chain].Last();
    const auto start = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock{Scheduler::submit_mutex};
        (void)instance.GetGraphicsQueue().waitIdle();
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("VR: %s swapchain %ux%u -> %ux%u: waited %.1f ms for the GPU before the bridge "
                "recreates it\n",
                names[chain], last.width, last.height, width, height, ms);
    std::fflush(stdout);
    // Recreated now (or on the next acquire that gets an image): no second wait for this size.
    xr_chain_sizes[chain].Acquired(size);
}

} // namespace Vulkan
