// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: VR Phase 4 prototype, renderer-side stereo (vk_stereo.h,
// docs/vr/findings/23-phase4-renderer-stereo.md). Everything here is inert unless
// BB_XR_MODE=stereo (or BB_STEREO=1).

#include "video_core/renderer_vulkan/vk_stereo.h"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "video_core/renderer_vulkan/vk_common.h"
#include <vk_mem_alloc.h>
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/ui_composition.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/tile_manager.h"

namespace Vulkan {

namespace Stereo {
namespace {

struct Config {
    bool enabled = false;
    const char* control = nullptr;
};

const Config& Cfg() {
    static const Config config = [] {
        Config c;
        const char* mode = std::getenv("BB_XR_MODE");
        const char* flag = std::getenv("BB_STEREO");
        c.enabled = (mode && std::strcmp(mode, "stereo") == 0) || (flag && flag[0] == '1');
        c.control = std::getenv("BB_STEREO_CONTROL");
        if (c.enabled) {
            std::printf("Stereo: prototype on (per-eye clip x offset in perspective vertex "
                        "shaders)%s%s\n",
                        c.control ? ", control file " : "", c.control ? c.control : "");
        }
        return c;
    }();
    return config;
}

/// Live settings from BB_STEREO_CONTROL ("key=value" tokens), read at frame starts.
struct Control {
    float offset_m = 0.0f; ///< oracle: the region from the center camera moved this far right
    float ipd_m = 0.063f;  ///< dup: eye L at -ipd/2, eye R at +ipd/2
    int dup = 0;           ///< a new value arms one duplicated G-buffer pass
    bool dup_all = false;  ///< double the G-buffer pass of every frame (no dumps): cost
    bool log = false;
    bool replay = true;    ///< P4.3: record once, execute twice (else the per-draw prototype)
    bool same = false;     ///< eye R at eye L's position: the replay must reproduce eye L
    bool guard_test = false; ///< the write guard's negative control: every draw "writes"
};
Control control;
std::filesystem::file_time_type control_time{};

// State of the thread recording draws (stage B, or the GPU thread when drained).
bool previous_display = false;
bool in_region = false;
bool region_done = false;
float sx = 0.0f;    ///< P00 of the main camera (scene constants float 52)
float eye_m = 0.0f; ///< this draw's eye: metres along the camera's right axis
u32 view_write = ~0u;
u64 frame = 0;
int dup_seen = 0;
enum class Dup { Off, Armed, Active };
Dup dup_state = Dup::Off;
u32 dup_draws = 0, dup_dispatches = 0;
bool dup_this_draw = false;              ///< the draw being recorded is doubled
VideoCore::ImageId pass_depth{};         ///< the doubled pass: its depth target
std::vector<VideoCore::ImageId> pass_colors; ///< and its color targets
u32 region_draws = 0, remapped_draws = 0, frames_logged = 0;

// P4.3, record once / execute twice (Scheduler::BeginCapture).
struct EyeSlotBuffer {
    VkBuffer buffer{};
    VmaAllocation allocation{};
};
EyeSlotBuffer eye_slot;
u32 replay_passes = 0;   ///< doubled pass: render passes executed for both eyes
u64 replay_commands = 0; ///< and the commands they held
u32 replay_aborted = 0;  ///< passes a direct recording cut short (eye R lacks them)
u32 replay_lost = 0;     ///< doubled draws recorded outside a capture (eye R lacks them)

// P4.4 write guard: eye R must never write memory the game can see. Its attachments are twins by
// construction (checked per pass); a captured draw whose shaders write a buffer or a storage
// image would write guest memory a second time, with eye R's values. Such a pass gets no eye R
// (counted; BB_STEREO_WRITE_AUDIT=1 names the shaders).
struct PassAudit {
    u32 writing_draws = 0;   ///< draws of the pass with a written buffer or storage image
    u64 first_writer = 0;    ///< the first one's vertex and pixel shader hashes, combined
    bool guest_attachment = false; ///< an eye-R attachment that is not a twin (never expected)
};
std::shared_ptr<PassAudit> pass_audit; ///< the capture being recorded
u32 replay_skipped_writes = 0;     ///< doubled render passes left without eye R: guest writes
u32 replay_skipped_attachments = 0; ///< ... : an attachment that is not a twin

// P4.4 slice 2: eye R's copies of what the copy shader 3d5ebf4e writes to memory that no image
// covers (an RGBA16F and an RGBA8 G-buffer target, 25 MB per frame): the source's twin, tiled into
// a twin buffer exactly as the guest copy gets the source image's memory (download, tile).
constexpr u64 CopyShaderHash = 0x3d5ebf4eull;
struct TwinBuffer {
    VkBuffer buffer{}, staging{};
    VmaAllocation allocation{}, staging_allocation{};
    u64 size = 0;
    u64 frame = ~0ull; ///< the frame whose eye R it holds
};
std::unordered_map<VAddr, TwinBuffer> twin_buffers; ///< by guest address of the copy's target
u32 buffer_copies = 0;   ///< per doubled pass: eye-R copies made
u32 buffer_unhandled = 0; ///< copy-shader dispatches in the pass whose eye R is not made

bool CreateDeviceBuffer(const Instance& instance, u64 size, VkBufferUsageFlags usage,
                        VkBuffer& buffer, VmaAllocation& allocation) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = size,
                                       .usage = usage};
    const VmaAllocationCreateInfo alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                           nullptr) == VK_SUCCESS;
}

TwinBuffer* EnsureTwinBuffer(const Instance& instance, VAddr address, u64 size) {
    auto& twin = twin_buffers[address];
    if (twin.buffer && twin.size == size) {
        return &twin;
    }
    // (A resized target: the old buffers leak; the game's targets do not change size.)
    twin = {};
    constexpr VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!CreateDeviceBuffer(instance, size, usage, twin.buffer, twin.allocation) ||
        !CreateDeviceBuffer(instance, size, usage, twin.staging, twin.staging_allocation)) {
        twin = {};
        return nullptr;
    }
    twin.size = size;
    std::printf("Stereo: twin buffer %llx+%llx\n", static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(size));
    return &twin;
}

/// Records a copy of `size` bytes of `buffer` at `offset` into a file once the GPU is past it
/// (DumpImage's naming: <name>_Buffer_<size>x1.raw).
void DumpBuffer(const Instance& instance, Scheduler& scheduler, vk::Buffer buffer, u64 offset,
                u64 size, const std::string& name) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = size,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer host{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &host, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    scheduler.Record([buffer, offset, size, host](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        cmdbuf.copyBuffer(buffer, vk::Buffer(host), vk::BufferCopy{offset, 0, size});
    });
    const char* dir = std::getenv("BB_DUMP_DIR");
    const std::string path = std::string(dir && *dir ? dir : ".") + "/" + name + "_Buffer_" +
                             std::to_string(size) + "x1.raw";
    scheduler.DeferPriorityOperation(
        [allocator = instance.GetAllocator(), host, allocation, info, size, path] {
            vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(info.pMappedData, 1, size, f);
                std::fclose(f);
            }
            vmaDestroyBuffer(allocator, host, allocation);
        });
}

bool WriteAudit() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_STEREO_WRITE_AUDIT");
        return value && value[0] == '1';
    }();
    return enabled;
}

/// Whether a stage writes memory through a buffer or a storage image (not the host-side special
/// buffers: StereoView, clip planes, flat user data, the fault buffer, LDS emulation).
bool StageWritesMemory(const Shader::Info& info) {
    for (const auto& buffer : info.buffers) {
        if (buffer.is_written && (!buffer.IsSpecial() ||
                                  buffer.buffer_type == Shader::BufferType::GdsBuffer)) {
            return true;
        }
    }
    for (const auto& image : info.images) {
        if (image.is_written) {
            return true;
        }
    }
    return false;
}
constexpr u64 ReplayProfileKey = 0x57E2E05E7E0ull;
std::chrono::steady_clock::time_point last_log{};

struct Twin {
    VideoCore::UniqueImage image;
    vk::UniqueImageView view;
    vk::Format format{};
    u32 width = 0, height = 0;
    bool depth = false;
    vk::ImageAspectFlags aspect{};
    VideoCore::ImageId source{}; ///< the guest image it doubles (this frame)
    u64 frame = ~0ull;           ///< frame it was initialized from the guest image
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
};
std::unordered_map<u64, std::unique_ptr<Twin>> twins;

/// The G-buffer pass: 5+ color targets and a depth target (Rasterizer::gbuffer_draw needs the
/// upscaler's camera motion; the prototype runs without the upscaler).
bool GBufferDraw(const GraphicsPipeline* pipeline, VideoCore::ImageId depth) {
    return std::popcount(pipeline->GetGraphicsKey().mrt_mask) >= 5 && bool(depth);
}

u64 TwinKey(VAddr address, vk::Format format) {
    return address ^ (u64(format) << 48);
}

bool HasStencil(vk::Format format) {
    return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint ||
           format == vk::Format::eD16UnormS8Uint;
}

u32 BytesPerPixel(vk::Format format) {
    switch (format) {
    case vk::Format::eR8Unorm:
        return 1;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eB10G11R11UfloatPack32:
    case vk::Format::eA2B10G10R10UnormPack32:
    case vk::Format::eR32Sfloat:
    case vk::Format::eR16G16Sfloat:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD32SfloatS8Uint: // depth aspect only
        return 4;
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR32G32Sfloat:
        return 8;
    default:
        return 0;
    }
}

void PollControl() {
    const char* path = Cfg().control;
    if (!path) {
        return;
    }
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec || time == control_time) {
        return;
    }
    control_time = time;
    FILE* f = std::fopen(path, "r");
    if (!f) {
        return;
    }
    char text[512]{};
    const size_t n = std::fread(text, 1, sizeof(text) - 1, f);
    std::fclose(f);
    text[n] = 0;
    Control next = control;
    for (char* token = std::strtok(text, " \t\r\n"); token; token = std::strtok(nullptr, " \t\r\n")) {
        char* eq = std::strchr(token, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        const char* value = eq + 1;
        if (!std::strcmp(token, "offset")) {
            next.offset_m = std::strtof(value, nullptr);
        } else if (!std::strcmp(token, "ipd")) {
            next.ipd_m = std::strtof(value, nullptr);
        } else if (!std::strcmp(token, "dup")) {
            next.dup = std::atoi(value);
        } else if (!std::strcmp(token, "dupall")) {
            next.dup_all = std::atoi(value) != 0;
        } else if (!std::strcmp(token, "log")) {
            next.log = std::atoi(value) != 0;
        } else if (!std::strcmp(token, "replay")) {
            next.replay = std::atoi(value) != 0;
        } else if (!std::strcmp(token, "guardtest")) {
            next.guard_test = std::atoi(value) != 0;
        } else if (!std::strcmp(token, "same")) {
            next.same = std::atoi(value) != 0;
        }
    }
    control = next;
    std::printf("Stereo: control offset %.3f m, ipd %.3f m, dup %d, replay %d, same %d\n",
                control.offset_m, control.ipd_m, control.dup, int(control.replay),
                int(control.same));
    if (control.dup != dup_seen) {
        dup_seen = control.dup;
        dup_state = Dup::Armed;
    }
}

/// dupall=1: every frame's G-buffer pass is doubled (measuring the cost; no dumps).
void ArmEveryFrame() {
    if (control.dup_all && dup_state == Dup::Off) {
        dup_state = Dup::Armed;
    }
}

Twin& EnsureTwin(const Instance& instance, u64 key, vk::Format format, u32 width, u32 height,
                 bool depth) {
    auto& slot = twins[key];
    if (slot && slot->format == format && slot->width == width && slot->height == height) {
        return *slot;
    }
    slot = std::make_unique<Twin>();
    Twin& twin = *slot;
    const auto device = instance.GetDevice();
    twin.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    const auto attachment = depth ? vk::ImageUsageFlagBits::eDepthStencilAttachment
                                  : vk::ImageUsageFlagBits::eColorAttachment;
    twin.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = attachment | vk::ImageUsageFlagBits::eTransferSrc |
                 vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    twin.aspect = depth ? (HasStencil(format) ? vk::ImageAspectFlagBits::eDepth |
                                                    vk::ImageAspectFlagBits::eStencil
                                              : vk::ImageAspectFlagBits::eDepth)
                        : vk::ImageAspectFlagBits::eColor;
    twin.view = Check(device.createImageViewUnique({
        .image = vk::Image(twin.image),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {twin.aspect, 0, 1, 0, 1},
    }));
    twin.format = format;
    twin.width = width;
    twin.height = height;
    twin.depth = depth;
    std::printf("Stereo: twin %s %ux%u\n", vk::to_string(format).c_str(), width, height);
    return twin;
}

vk::ImageMemoryBarrier2 TwinBarrier(Twin& twin, vk::ImageLayout layout) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout = twin.layout,
        .newLayout = layout,
        .image = vk::Image(twin.image),
        .subresourceRange = {twin.aspect, 0, 1, 0, 1},
    };
    twin.layout = layout;
    return barrier;
}

vk::ImageLayout AttachmentLayout(const Twin& twin) {
    return twin.depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                      : vk::ImageLayout::eColorAttachmentOptimal;
}

/// Records a copy of `image` (layout TRANSFER_SRC) into a host buffer, written to a file
/// once the GPU is past it.
void DumpImage(const Instance& instance, Scheduler& scheduler, vk::Image image,
               vk::ImageAspectFlags aspect, vk::Format format, u32 width, u32 height,
               const std::string& name) {
    const u32 bpp = BytesPerPixel(format);
    if (!bpp) {
        std::printf("Stereo: no dump of %s (%s)\n", name.c_str(), vk::to_string(format).c_str());
        return;
    }
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(width) * height * bpp,
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
    // Depth/stencil: the depth aspect (4 bytes per pixel for D32).
    const vk::ImageAspectFlags copy_aspect =
        (aspect & vk::ImageAspectFlagBits::eDepth) ? vk::ImageAspectFlags(vk::ImageAspectFlagBits::eDepth)
                                                   : aspect;
    scheduler.Record([image, buffer, copy_aspect, width, height](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                                 vk::BufferImageCopy{.imageSubresource = {copy_aspect, 0, 0, 1},
                                                     .imageExtent = {width, height, 1}});
    });
    const char* dir = std::getenv("BB_DUMP_DIR");
    const std::string path = std::string(dir && *dir ? dir : ".") + "/" + name + "_" +
                             vk::to_string(format) + "_" + std::to_string(width) + "x" +
                             std::to_string(height) + ".raw";
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation,
                                      info, size = buffer_ci.size, path] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

/// The doubled pass is over: both eyes' targets to files (BB_DUMP_DIR).
void DumpPass(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
              VideoCore::TextureCache& texture_cache) {
    dup_state = Dup::Off;
    scheduler.EndRendering(); // the last captured render pass: eye R replay (P4.3)
    if (control.dup_all) {
        return;
    }
    std::vector<Twin*> used;
    for (auto& [key, twin] : twins) {
        if (twin->frame == frame && twin->source) {
            used.push_back(twin.get());
        }
    }
    for (Twin* twin : used) {
        auto& image = texture_cache.GetImage(twin->source);
        runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    }
    runtime.FlushBarriers();
    u32 index = 0;
    for (Twin* twin : used) {
        auto& image = texture_cache.GetImage(twin->source);
        const auto barrier = TwinBarrier(*twin, vk::ImageLayout::eTransferSrcOptimal);
        scheduler.Record([barrier](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        });
        const std::string base = "stereo_f" + std::to_string(frame) + "_t" + std::to_string(index++);
        DumpImage(instance, scheduler, image.GetImage(), twin->aspect, twin->format, twin->width,
                  twin->height, base + "_L");
        DumpImage(instance, scheduler, vk::Image(twin->image), twin->aspect, twin->format,
                  twin->width, twin->height, base + "_R");
    }
    std::printf("Stereo: G-buffer pass of frame %llu drawn twice (%u draws, %u dispatches in "
                "it, not doubled), %zu targets dumped; %s: %u render passes executed twice "
                "(%llu commands), %u cut short, %u draws outside a capture; write guard: %u "
                "passes without eye R (guest writes), %u (attachment not a twin); eye-R buffer copies "
                "%u, not made %u\n",
                static_cast<unsigned long long>(frame), dup_draws, dup_dispatches, used.size(),
                control.replay ? "record once/execute twice" : "per-draw prototype",
                replay_passes, static_cast<unsigned long long>(replay_commands), replay_aborted,
                replay_lost, replay_skipped_writes, replay_skipped_attachments, buffer_copies,
                buffer_unhandled);
}

/// The StereoView value of the eye at `eye` metres along the camera's right axis.
std::array<float, 4> ViewAt(float eye) {
    return {-sx * eye, 0.0f, 0.0f, 0.0f};
}

bool EnsureEyeSlot(const Instance& instance) {
    if (eye_slot.buffer) {
        return true;
    }
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 256,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &eye_slot.buffer,
                        &eye_slot.allocation, nullptr) != VK_SUCCESS) {
        eye_slot = {};
        std::printf("Stereo: no eye slot buffer: the doubled pass uses per-draw replay\n");
        return false;
    }
    return true;
}

/// Writes the eye's StereoView value into the slot every draw of a doubled pass reads; the
/// previous eye's vertex stages are done with it first. Outside render passes.
void RecordSlotUpdate(Scheduler& scheduler, std::array<float, 4> view) {
    scheduler.Record([buffer = vk::Buffer(eye_slot.buffer), view](vk::CommandBuffer cmdbuf) {
        constexpr auto vertex_stages = vk::PipelineStageFlagBits2::eVertexShader |
                                       vk::PipelineStageFlagBits2::eTessellationEvaluationShader |
                                       vk::PipelineStageFlagBits2::eGeometryShader;
        const vk::MemoryBarrier2 before{
            .srcStageMask = vertex_stages,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        cmdbuf.updateBuffer(buffer, 0, sizeof(view), view.data());
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vertex_stages,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    });
}

} // namespace

std::optional<std::pair<vk::Buffer, u64>> EyeSlot() {
    if (dup_this_draw && dup_state == Dup::Active && control.replay && eye_slot.buffer) {
        return std::pair{vk::Buffer(eye_slot.buffer), u64{0}};
    }
    return std::nullopt;
}

bool RemapShaders() {
    return Cfg().enabled;
}

std::array<float, 4> CurrentView() {
    // The eye at x metres along the camera's right axis sees clip.x - P00 * x.
    return {-sx * eye_m, 0.0f, 0.0f, 0.0f};
}

void NoteViewWrite(u32 write_index) {
    view_write = write_index;
    if (in_region) {
        ++remapped_draws;
    }
}

void OnSceneConstants(const float* data) {
    // The main camera's block: far 3000 (as CameraMotion::OnConstants matches it, within 100: the
    // [0] value moves with the near plane, 3001.49 at first person's 0.2 m), 1/far, the render size.
    // The shadow cascades' blocks (far 103-140) do not match.
    if (std::abs(data[0] - 3000.0f) <= 100.0f && std::abs(data[1] * data[0] - 1.0f) < 1e-3f &&
        data[4] >= 64.0f && data[5] >= 64.0f && data[52] > 0.0f) {
        sx = data[52];
    }
}

} // namespace Stereo

void Rasterizer::StereoBeginDraw(const GraphicsPipeline* pipeline) {
    using namespace Stereo;
    if (!RemapShaders()) {
        return;
    }
    // (Regs() is inline in vk_rasterizer.cpp.)
    const auto& regs = stage_regs ? *stage_regs : liverpool->regs;
    const bool display =
        regs.color_buffers[0] && FrameCapture::IsDisplayBuffer(regs.color_buffers[0].Address());
    if (display && !previous_display) {
        if (dup_state == Dup::Active) {
            DumpPass(instance, scheduler, runtime, texture_cache); // the pass ended the frame
        }
        ++frame;
        ++frames_logged;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(5)) {
            if (frames_logged && last_log.time_since_epoch().count()) {
                std::printf("Stereo: %.1f region draws/frame, %.1f remapped (perspective), P00 "
                            "%.4f, offset %.3f m\n",
                            double(region_draws) / frames_logged,
                            double(remapped_draws) / frames_logged, sx, control.offset_m);
            }
            last_log = now;
            region_draws = remapped_draws = frames_logged = 0;
        }
        in_region = false;
        region_done = false;
        PollControl();
        ArmEveryFrame();
    }
    previous_display = display;
    const u64 vs = pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash;
    const bool gbuffer = GBufferDraw(pipeline, db_desc.first);
    if (!in_region && !region_done && gbuffer) {
        in_region = true;
    }
    if (in_region && UiComposition::MovieShader(vs)) {
        in_region = false;
        region_done = true;
    }
    // This draw's targets: the color slots the pipeline writes, and the depth.
    const auto key_mask = pipeline->GetGraphicsKey().mrt_mask;
    struct Target {
        VideoCore::ImageId id;
        vk::Format format;
        bool depth;
    };
    std::vector<Target> targets;
    if (dup_state != Dup::Off) {
        for (u32 cb = 0; cb < cb_descs.size(); ++cb) {
            if ((key_mask >> cb) & 1 && cb_descs[cb].first) {
                targets.push_back({cb_descs[cb].first, cb_descs[cb].second.view_info.format, false});
            }
        }
        if (db_desc.first) {
            targets.push_back({db_desc.first, db_desc.second.view_info.format, true});
        }
    }
    if (dup_state == Dup::Active) {
        // The G-buffer pass: draws into its depth whose colors (if any) include one of its
        // color targets (single-target decals and the extra-target draw stay in it; the lighting
        // pass, same depth but other colors, ends it).
        bool colors = false, shared = false;
        for (const auto& target : targets) {
            if (!target.depth) {
                colors = true;
                shared |= std::ranges::find(pass_colors, target.id) != pass_colors.end();
            }
        }
        if (db_desc.first != pass_depth || (colors && !shared)) {
            DumpPass(instance, scheduler, runtime, texture_cache); // the first draw after it
        } else {
            for (const auto& target : targets) {
                if (!target.depth && std::ranges::find(pass_colors, target.id) == pass_colors.end()) {
                    pass_colors.push_back(target.id);
                }
            }
        }
    }
    if (dup_state == Dup::Armed && gbuffer) {
        dup_state = Dup::Active;
        dup_draws = dup_dispatches = 0;
        replay_passes = replay_aborted = replay_lost = 0;
        replay_skipped_writes = replay_skipped_attachments = 0;
        buffer_copies = buffer_unhandled = 0;
        replay_commands = 0;
        pass_depth = db_desc.first;
        pass_colors.clear();
        for (const auto& target : targets) {
            if (!target.depth) {
                pass_colors.push_back(target.id);
            }
        }
        if (!control.dup_all) {
            std::printf("Stereo: duplicating the G-buffer pass of frame %llu (eyes at -/+ %.3f "
                        "m)\n",
                        static_cast<unsigned long long>(frame), control.ipd_m / 2);
        }
    }
    dup_this_draw = dup_state == Dup::Active;
    if (dup_this_draw && control.replay && !EnsureEyeSlot(instance)) {
        control.replay = false;
    }
    if (dup_this_draw) {
        // Twins of this draw's targets start from the guest images' contents (the depth clear
        // ran in an earlier pass), copied before eye L draws into them.
        std::vector<std::pair<Twin*, VideoCore::Image*>> fresh;
        for (const auto& target : targets) {
            auto& image = texture_cache.GetImage(target.id);
            Twin& twin = EnsureTwin(instance, TwinKey(image.info.guest_address, target.format),
                                    target.format, image.info.size.width, image.info.size.height,
                                    target.depth);
            twin.source = target.id;
            if (twin.frame != frame) {
                twin.frame = frame;
                fresh.emplace_back(&twin, &image);
            }
        }
        if (!fresh.empty()) {
            scheduler.EndRendering();
            for (auto& [twin, image] : fresh) {
                runtime.Transit(image, vk::ImageLayout::eTransferSrcOptimal,
                                vk::PipelineStageFlagBits2::eTransfer,
                                vk::AccessFlagBits2::eTransferRead);
            }
            runtime.FlushBarriers();
            for (auto& [twin, image] : fresh) {
                const auto to_dst = TwinBarrier(*twin, vk::ImageLayout::eTransferDstOptimal);
                const auto to_attachment = TwinBarrier(*twin, AttachmentLayout(*twin));
                const vk::ImageCopy region{
                    .srcSubresource = {twin->aspect, 0, 0, 1},
                    .dstSubresource = {twin->aspect, 0, 0, 1},
                    .extent = {std::min(twin->width, image->info.size.width),
                               std::min(twin->height, image->info.size.height), 1},
                };
                const vk::Image src = image->GetImage();
                const vk::Image dst = vk::Image(twin->image);
                scheduler.Record([to_dst, to_attachment, region, src, dst](vk::CommandBuffer cmd) {
                    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1,
                                          .pImageMemoryBarriers = &to_dst});
                    cmd.copyImage(src, vk::ImageLayout::eTransferSrcOptimal, dst,
                                  vk::ImageLayout::eTransferDstOptimal, region);
                    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1,
                                          .pImageMemoryBarriers = &to_attachment});
                });
            }
        }
    }
    eye_m = !in_region ? 0.0f : dup_this_draw ? -control.ipd_m / 2 : control.offset_m;
    view_write = ~0u;
    if (in_region) {
        ++region_draws;
    }
}

void Rasterizer::StereoAfterDraw(const GraphicsPipeline* pipeline, const RenderState& state,
                                 bool is_indexed, u32 num_indices, u32 num_instances,
                                 u32 first_vertex, u32 first_instance) {
    using namespace Stereo;
    if (!RemapShaders() || !dup_this_draw || dup_state != Dup::Active) {
        return;
    }
    if (control.replay) {
        // P4.3: eye R runs when the captured pass ends (StereoBeforeEmit).
        ++dup_draws;
        if (!scheduler.IsCapturing()) {
            ++replay_lost;
        } else if (pass_audit) {
            bool writes = false;
            for (const auto* stage : pipeline->GetStages()) {
                writes |= stage && StageWritesMemory(*stage);
            }
            if (control.guard_test) {
                writes = true; // negative control: the guard must leave every pass without eye R
            }
            if (writes && pass_audit->writing_draws++ == 0) {
                const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
                pass_audit->first_writer =
                    pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash << 32 |
                    (ps ? ps->pgm_hash & 0xffffffffull : 0);
            }
        }
        return;
    }
    // Eye R: the same recorded draw into the twins, with the other eye's StereoView.
    RenderState twin_state = state;
    const auto key_mask = pipeline->GetGraphicsKey().mrt_mask;
    for (u32 cb = 0; cb < twin_state.num_color_attachments; ++cb) {
        auto& attachment = twin_state.color_attachments[cb];
        if (!attachment.image_view) {
            continue;
        }
        const auto it = ((key_mask >> cb) & 1) && cb_descs[cb].first
                            ? twins.find(TwinKey(texture_cache.GetImage(cb_descs[cb].first)
                                                     .info.guest_address,
                                                 cb_descs[cb].second.view_info.format))
                            : twins.end();
        if (it == twins.end()) {
            attachment = {}; // not doubled (object motion and other host attachments)
            continue;
        }
        attachment.image_view = *it->second->view;
        attachment.image_layout = AttachmentLayout(*it->second);
    }
    if (twin_state.depth_stencil_attachment.image_view && db_desc.first) {
        const auto it = twins.find(
            TwinKey(texture_cache.GetImage(db_desc.first).info.guest_address,
                    db_desc.second.view_info.format));
        if (it != twins.end()) {
            twin_state.depth_stencil_attachment.image_view = *it->second->view;
            twin_state.depth_stencil_attachment.image_layout = AttachmentLayout(*it->second);
        }
    }
    scheduler.EndRendering();
    scheduler.BeginRendering(twin_state);
    if (view_write != ~0u && view_write < set_writes.size()) {
        eye_m = control.same ? -control.ipd_m / 2 : control.ipd_m / 2;
        const auto view = CurrentView();
        auto& stream = buffer_cache.GetStreamBuffer();
        const u64 offset =
            stream.Copy(view.data(), sizeof(view), instance.StorageMinAlignment());
        const vk::DescriptorBufferInfo info{stream.Handle(), offset, sizeof(view)};
        const auto* saved = set_writes[view_write].pBufferInfo;
        set_writes[view_write].pBufferInfo = &info;
        pipeline->BindResources(set_writes, push_data);
        set_writes[view_write].pBufferInfo = saved;
        eye_m = -control.ipd_m / 2;
    } else {
        pipeline->BindResources(set_writes, push_data);
    }
    const vk::Pipeline handle = pipeline->Handle();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, handle);
        if (is_indexed) {
            cmdbuf.drawIndexed(num_indices, num_instances, 0, s32(first_vertex), first_instance);
        } else {
            cmdbuf.draw(num_indices, num_instances, first_vertex, first_instance);
        }
    });
    scheduler.EndRendering();
    ++dup_draws;
}

void Rasterizer::StereoBeforeEmit(const GraphicsPipeline* pipeline, const RenderState& state) {
    using namespace Stereo;
    if (!RemapShaders()) {
        return;
    }
    const bool doubled = dup_this_draw && dup_state == Dup::Active && control.replay;
    if (scheduler.IsCapturing() && (!doubled || scheduler.WillBeginRendering(state))) {
        scheduler.EndRendering(); // the captured pass ends here: eye R follows
    }
    if (!doubled) {
        return;
    }
    // Barriers this draw needs end the pass before its commands (not halfway through them);
    // DrawRecord flushes them only when needs_barrier, so do the same (flushing barriers mono
    // would defer would end every pass after one draw).
    if (needs_barrier) {
        runtime.FlushBarriers();
    }
    if (scheduler.IsCapturing()) {
        return; // the next draw of the captured pass
    }
    // A new render pass of the doubled region, recorded once and executed for both eyes:
    //   eye L: slot <- L, BeginRendering(guest targets), the commands, EndRendering
    //   eye R: slot <- R, BeginRendering(twins), the same commands, EndRendering
    // The first draw sets all of its dynamic state (vertex/index buffers, pipeline, push
    // constants and descriptors are set by every draw anyway), so the commands do not depend on
    // what was bound before the pass.
    scheduler.EndRendering();
    MarkPass(pipeline, state);
    const float eye_l = -control.ipd_m / 2;
    const float eye_r = control.same ? eye_l : control.ipd_m / 2;
    scheduler.KickRecording(); // nobody holds the raw command buffer here: deferred again
    RecordSlotUpdate(scheduler, ViewAt(eye_l));
    scheduler.GetDynamicState().Invalidate();
    scheduler.BeginRendering(state);

    // Eye R's targets: the twins of the guest targets (other attachments, e.g. object
    // motion, are dropped as in the prototype).
    RenderState twin_state = state;
    const auto key_mask = pipeline->GetGraphicsKey().mrt_mask;
    for (u32 cb = 0; cb < twin_state.num_color_attachments; ++cb) {
        auto& attachment = twin_state.color_attachments[cb];
        if (!attachment.image_view) {
            continue;
        }
        const auto it = ((key_mask >> cb) & 1) && cb_descs[cb].first
                            ? twins.find(TwinKey(texture_cache.GetImage(cb_descs[cb].first)
                                                     .info.guest_address,
                                                 cb_descs[cb].second.view_info.format))
                            : twins.end();
        if (it == twins.end()) {
            attachment = {};
            continue;
        }
        attachment.image_view = *it->second->view;
        attachment.image_layout = AttachmentLayout(*it->second);
    }
    if (twin_state.depth_stencil_attachment.image_view) {
        const auto it = db_desc.first
                            ? twins.find(TwinKey(
                                  texture_cache.GetImage(db_desc.first).info.guest_address,
                                  db_desc.second.view_info.format))
                            : twins.end();
        if (it != twins.end()) {
            twin_state.depth_stencil_attachment.image_view = *it->second->view;
            twin_state.depth_stencil_attachment.image_layout = AttachmentLayout(*it->second);
        } else {
            twin_state.depth_stencil_attachment = {}; // never eye L's own depth
        }
    }
    const auto view_r = ViewAt(eye_r);
    // Write guard: no eye-R attachment may be one of eye L's (guest) attachments.
    auto audit = std::make_shared<PassAudit>();
    for (u32 cb = 0; cb < twin_state.num_color_attachments; ++cb) {
        const auto view = twin_state.color_attachments[cb].image_view;
        for (u32 other = 0; other < state.num_color_attachments; ++other) {
            audit->guest_attachment |= view && view == state.color_attachments[other].image_view;
        }
        audit->guest_attachment |= view && view == state.depth_stencil_attachment.image_view;
    }
    if (const auto view = twin_state.depth_stencil_attachment.image_view) {
        audit->guest_attachment |= view == state.depth_stencil_attachment.image_view;
        for (u32 other = 0; other < state.num_color_attachments; ++other) {
            audit->guest_attachment |= view == state.color_attachments[other].image_view;
        }
    }
    pass_audit = audit;
    const bool begun = scheduler.BeginCapture(
        std::make_shared<RecordArena>(),
        [this, twin_state, view_r, audit](std::shared_ptr<RecordArena> arena, bool complete) {
            if (pass_audit == audit) {
                pass_audit.reset();
            }
            if (!complete) {
                ++replay_aborted;
                return;
            }
            if (audit->guest_attachment || audit->writing_draws) {
                ++(audit->guest_attachment ? replay_skipped_attachments : replay_skipped_writes);
                if (WriteAudit()) {
                    static u32 printed = 0;
                    if (printed++ < 32) {
                        std::printf("Stereo: write guard: a doubled render pass gets no eye R: "
                                    "%s (%u draws write memory, first vs/ps %016llx)\n",
                                    audit->guest_attachment ? "an attachment is not a twin"
                                                            : "its shaders write memory",
                                    audit->writing_draws,
                                    static_cast<unsigned long long>(audit->first_writer));
                    }
                }
                return; // the arena was executed for eye L only
            }
            ++replay_passes;
            replay_commands += arena->Commands();
            auto* profiler = GpuProfiler::Get();
            const u64 resume = profiler ? profiler->Current() : 0;
            if (profiler) {
                profiler->Mark(ReplayProfileKey, [] {
                    return std::string{"stereo: eye R of doubled passes (record once, execute "
                                       "twice)"};
                });
            }
            RecordSlotUpdate(scheduler, view_r);
            scheduler.BeginRendering(twin_state);
            scheduler.Record([arena = std::move(arena)](vk::CommandBuffer cmdbuf) {
                arena->Execute(cmdbuf);
            });
            scheduler.EndRendering();
            if (profiler && resume) {
                profiler->Resume(resume);
            }
        });
    if (!begun) {
        pass_audit.reset();
        ++replay_lost; // not deferred recording: this pass has no eye R
    }
}

void Rasterizer::StereoAfterDispatch(const Shader::Info& cs) {
    using namespace Stereo;
    if (!RemapShaders() || dup_state != Dup::Active || !control.replay ||
        cs.pgm_hash != CopyShaderHash || cs.buffers.size() != 2) {
        return;
    }
    const bool first_written = cs.buffers[0].is_written;
    const auto src = cs.buffers[first_written ? 1 : 0].GetSharp(cs);
    const auto dst = cs.buffers[first_written ? 0 : 1].GetSharp(cs);
    if (texture_cache.FindImageFromRange(dst.base_address, dst.GetSize(), false)) {
        return; // an image copy (IsComputeImageCopy) or an image target: P4.4 twins of images
    }
    const auto src_id = texture_cache.FindImageFromRange(src.base_address, src.GetSize());
    Twin* twin = nullptr;
    for (auto& [key, candidate] : twins) {
        if (src_id && candidate->source == src_id && candidate->frame == frame) {
            twin = candidate.get();
        }
    }
    const auto& info = src_id ? texture_cache.GetImage(src_id).info : VideoCore::ImageInfo{};
    TwinBuffer* target = twin && info.guest_size == dst.GetSize() && info.resources.levels == 1
                             ? EnsureTwinBuffer(instance, dst.base_address, dst.GetSize())
                             : nullptr;
    if (!target) {
        ++buffer_unhandled; // eye R would read eye L's bytes there
        return;
    }
    // The twin's texels, in rows of the guest pitch, then tiled like TileImage does.
    scheduler.EndRendering();
    const auto to_src = TwinBarrier(*twin, vk::ImageLayout::eTransferSrcOptimal);
    const auto back = TwinBarrier(*twin, AttachmentLayout(*twin));
    const vk::BufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = info.mips_layout[0].pitch,
        .bufferImageHeight = info.mips_layout[0].height,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {twin->width, twin->height, 1},
    };
    const vk::Buffer linear = info.props.is_tiled ? vk::Buffer(target->staging)
                                                  : vk::Buffer(target->buffer);
    scheduler.Record([to_src, back, region, image = vk::Image(twin->image),
                      linear](vk::CommandBuffer cmdbuf) {
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_src});
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, linear, region);
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader |
                            vk::PipelineStageFlagBits2::eAllTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &back});
    });
    if (info.props.is_tiled) {
        texture_cache.GetTileManager().TileLinear(info, {linear, 0, dst.GetSize()},
                                                  {vk::Buffer(target->buffer), 0, dst.GetSize()});
        scheduler.Record([](vk::CommandBuffer cmdbuf) {
            const vk::MemoryBarrier2 done{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        });
    }
    target->frame = frame;
    const u32 index = buffer_copies++;
    if (!control.dup_all) {
        // Both eyes' bytes: the guest copy's result (eye L) and the twin buffer (eye R).
        const auto [guest, offset] =
            buffer_cache.ObtainBuffer(dst.base_address, dst.GetSize(), false);
        const std::string base =
            "stereo_f" + std::to_string(frame) + "_t" + std::to_string(100 + index);
        DumpBuffer(instance, scheduler, guest->Handle(), offset, dst.GetSize(), base + "_L");
        DumpBuffer(instance, scheduler, vk::Buffer(target->buffer), 0, dst.GetSize(),
                   base + "_R");
    }
}

void Rasterizer::StereoBeforeDispatch() {
    using namespace Stereo;
    // Dispatches inside the doubled pass (the HLE constant copies among them) are counted only:
    // compute replays for eye R are not part of the prototype.
    if (RemapShaders() && dup_state == Dup::Active) {
        ++dup_dispatches;
    }
}

} // namespace Vulkan
