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
#include <atomic>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "video_core/renderer_vulkan/vk_common.h"
#include <vk_mem_alloc.h>
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/host_shaders/stereo_warp_comp.h"
#include "video_core/renderer_vulkan/ui_composition.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_stereo_warp.h"
#include "video_core/renderer_vulkan/vk_stereo_region.h"
#include "video_core/renderer_vulkan/vk_stereo_substitute.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/tile_manager.h"

namespace Vulkan {

namespace Stereo {
namespace {

struct Config {
    bool enabled = false;
    const char* control = nullptr;
    bool hybrid = true;   ///< BB_STEREO_HYBRID (default 1): hybrid stereo every frame
    bool guided = true;   ///< BB_STEREO_GUIDED (default 1): G-buffer-guided hole fill
    bool full = true;     ///< BB_STEREO_FULL (default 1): eye R rendered for real (section 10)
    float ipd_m = 0.063f; ///< BB_STEREO_IPD (metres)
};

const Config& Cfg() {
    static const Config config = [] {
        Config c;
        const char* mode = std::getenv("BB_XR_MODE");
        const char* flag = std::getenv("BB_STEREO");
        c.enabled = (mode && std::strcmp(mode, "stereo") == 0) || (flag && flag[0] == '1');
        c.control = std::getenv("BB_STEREO_CONTROL");
        const char* hybrid = std::getenv("BB_STEREO_HYBRID");
        c.hybrid = !(hybrid && hybrid[0] == '0');
        const char* guided = std::getenv("BB_STEREO_GUIDED");
        c.guided = !(guided && guided[0] == '0');
        const char* full = std::getenv("BB_STEREO_FULL");
        c.full = !(full && full[0] == '0');
        if (const char* ipd = std::getenv("BB_STEREO_IPD")) {
            const float value = std::strtof(ipd, nullptr);
            if (value > 0.0f && value < 0.2f) {
                c.ipd_m = value;
            }
        }
        if (c.enabled) {
            std::printf("Stereo: prototype on (per-eye clip x offset in perspective vertex "
                        "shaders)%s%s; hybrid stereo %s (guided hole fill %s), full stereo %s "
                        "(eye R rendered to the display pass; the warp is its fallback), ipd "
                        "%.4f m\n",
                        c.control ? ", control file " : "", c.control ? c.control : "",
                        c.hybrid ? "on" : "off", c.guided ? "on" : "off", c.full ? "on" : "off",
                        c.ipd_m);
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
    bool hybrid = true;    ///< hybrid stereo (BB_STEREO_HYBRID); only while offset is 0
    bool ipd_set = false;  ///< "ipd=" given: overrides the runtime's eye distance
    int warp_dump = 0;     ///< a new value dumps the next region end (eye L, eye R's warp)
    bool guided = true;    ///< G-buffer-guided hole fill (BB_STEREO_GUIDED, "guided=")
    bool compute = true;   ///< P4.4 slice 3: dispatches per eye and eye-R descriptors
    bool full = true;      ///< full stereo (BB_STEREO_FULL, "full="): needs hybrid, replay, compute
    int audit = 0;         ///< a new value lists every pass, dispatch and copy of the next full region
};
Control control;
bool control_defaults = false;        ///< Config's defaults applied to `control`
std::atomic<float> runtime_ipd{0.0f}; ///< SetIpd
std::atomic<u64> public_frame{0};     ///< Frame(), for other threads
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
bool dump_pass = false;    ///< the armed/active doubled pass dumps its targets (dup=<n>)
float frame_ipd = 0.063f;  ///< this frame's eye distance (latched at the frame start)
bool hybrid_frame = false; ///< this frame runs hybrid stereo (whole region at eye L)
VideoCore::ImageId region_depth{};      ///< the 3D region's depth (the G-buffer pass's)
VideoCore::ImageId last_scene_color{};  ///< the region's last RGBA8 target of the depth's size
bool warp_pending = false; ///< hybrid: eye R's warp still to run at this region's end
int warp_dump_seen = 0;
int warp_dump_armed = 0;   ///< a warpdump id to dump at the next region end
int audit_seen = 0;
bool audit_armed = false;  ///< "audit=<n>": list the next full region
bool audit_region = false; ///< the full region being recorded is listed
u32 audit_pass = 0;        ///< its render passes, numbered
float sz_s = 0.0f, sz_o = 0.0f; ///< the main camera's depth = zs + zo / z (floats 62, 63)
u32 warps = 0, warp_fallbacks = 0, warp_skipped = 0; ///< per log window
u32 dup_draws = 0, dup_dispatches = 0;
bool dup_this_draw = false;              ///< the draw being recorded is doubled
VideoCore::ImageId pass_depth{};         ///< the doubled pass: its depth target
std::vector<VideoCore::ImageId> pass_colors; ///< and its color targets
u32 region_draws = 0, remapped_draws = 0, frames_logged = 0;

// Full stereo (findings/31 section 10): the region doubled from the G-buffer to the display pass.
FullRegion full_region;
u64 twin_frame = 0;      ///< the region frame twins are tagged with (frame advances at the
                         ///< display pass, which is still the region's for full stereo)
bool full_frame = false; ///< the doubled region of this frame is the full region
VideoCore::ImageId full_display{}; ///< the display buffer image the doubled display pass wrote
struct FullStats {
    u32 frames = 0;          ///< full regions finished (per 5 s window)
    u32 copies = 0;          ///< flips whose eye R is the rendered display twin
    u32 fallbacks = 0;       ///< flips that fell back to the warp
    u32 not_doubled = 0;     ///< region render passes eye R cannot render (mips, layers, MSAA)
    u32 image_copies = 0;    ///< compute image copies redone between twins
    u32 copies_stale = 0;    ///< image copies and clears that left eye R reading eye L's result
    u32 history = 0;         ///< eye-R reads of last region's twin (read before written)
    u32 leaks = 0;           ///< eye-R reads of an image eye L wrote in this region, no twin
    u32 aliases = 0;         ///< images with twins of two formats (views of one stay eye L's)
    u32 indirect = 0;        ///< indirect draws in the region (doubled like direct ones)
    u32 shared_reads = 0;    ///< eye-R reads of images eye L computed for both eyes by design
};
FullStats full_stats;  ///< per doubled region (its log line)
FullStats full_window; ///< per 5 s log window (copies, fallbacks, frames)
/// Images eye L wrote in this region (attachments, storage, copy targets): an eye-R read of one
/// without a twin reads eye L's pixels (counted as a leak, BB_STEREO_WRITE_AUDIT names them).
std::vector<VideoCore::ImageId> region_written;
std::vector<VideoCore::ImageId> region_shared; ///< eye L's for both eyes by design (exposure)
std::vector<VideoCore::ImageId> last_region_written; ///< the probe's chain (BB_STEREO_PROBE_CHAIN)
std::vector<std::pair<u32, VideoCore::ImageId>> pass_targets, last_pass_targets; ///< (pass, image)
u32 pass_counter = 0;
u64 pass_ps = 0;
std::vector<u64> pass_shaders, last_pass_shaders;

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
    vk::UniqueImageView depth_view; ///< depth aspect only, for sampling (hybrid warp)
    vk::Format format{};
    u32 width = 0, height = 0;
    bool depth = false;
    vk::ImageAspectFlags aspect{};
    VideoCore::ImageId source{}; ///< the guest image it doubles (this frame)
    u64 frame = ~0ull;           ///< frame it was initialized from the guest image
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    // P4.4 slice 3: eye R's descriptors read (and dispatches write) the twin through views
    // matching the guest's (TwinViewFor).
    bool storage = false; ///< created with storage use (its format supports it)
    struct View {
        vk::ImageAspectFlags aspect;
        vk::ImageViewType type;
        vk::ComponentMapping mapping;
        bool storage;
        vk::UniqueImageView view;
    };
    std::vector<View> views;
};
std::unordered_map<u64, std::unique_ptr<Twin>> twins;
std::vector<std::unique_ptr<Twin>> retired_twins; ///< replaced twins (in-flight commands use them)

/// The twins by source image (stereo-perf): the per-draw lookups (substitutes, TwinOf,
/// MarkImageStale) scanned every twin (about 100) per bound image. Rebuilt from `twins`, in its
/// iteration order (so the same twin wins where a scan took the first or last match), whenever
/// a twin is added or retired or its source changes (SetSource).
u64 twins_generation = 0;
struct SourceIndex {
    u64 generation = ~0ull;
    std::vector<boost::container::small_vector<Twin*, 2>> by_source; ///< by ImageId index
};
SourceIndex source_index;

void SetSource(Twin& twin, VideoCore::ImageId id) {
    if (twin.source != id) {
        twin.source = id;
        ++twins_generation;
    }
}

/// The twins whose source is `id` (any frame), in `twins`' iteration order.
std::span<Twin* const> TwinsOfSource(VideoCore::ImageId id) {
    if (source_index.generation != twins_generation) {
        source_index.generation = twins_generation;
        for (auto& list : source_index.by_source) {
            list.clear();
        }
        for (auto& [key, twin] : twins) {
            if (!twin->source) {
                continue;
            }
            const u32 index = twin->source.index;
            if (index >= source_index.by_source.size()) {
                source_index.by_source.resize(index + 1);
            }
            source_index.by_source[index].push_back(twin.get());
        }
    }
    if (!id || id.index >= source_index.by_source.size()) {
        return {};
    }
    return source_index.by_source[id.index];
}

/// The doubled G-buffer pass's color twins by MRT slot (the draws with 5+ targets), for the
/// guided hole fill (findings/31 section 9.10). The base colour (albedo) is the only
/// R8G8B8A8Srgb target; in the world's G-buffer (6 targets: 0 normals, 1 material ids, 2 albedo,
/// 3 specular, 4 B10G11R11, 5 RGBA16F) the normals are slot 0, R8G8B8A8Unorm, world-space
/// normal * 0.5 + 0.5 (identified on the warpdump slot dumps, out/stereo_ab/run7/warp). Other
/// layouts (menus: slot 0 RGBA16F) get no normals (the format check).
constexpr u32 NormalSlot = 0;
std::array<Twin*, 8> gbuffer_slots{};
u64 gbuffer_slots_frame = ~0ull;
bool gbuffer_slots_logged = false;

/// The G-buffer pass: 5+ color targets and a depth target (Rasterizer::gbuffer_draw needs the
/// upscaler's camera motion; the prototype runs without the upscaler).
bool GBufferDraw(const GraphicsPipeline* pipeline, VideoCore::ImageId depth) {
    return std::popcount(pipeline->GetGraphicsKey().mrt_mask) >= 5 && bool(depth);
}

/// Twins are keyed by the guest image's address, view format and size: render targets that
/// alias one address at different sizes (transient targets) get separate twins, so no twin is
/// replaced while recorded commands still use it.
u64 TwinKey(const VideoCore::Image& image, vk::Format format) {
    u64 key = image.info.guest_address ^ (u64(format) << 48);
    key ^= (u64(image.info.size.width) * 0x9E3779B97F4A7C15ull) ^
           (u64(image.info.size.height) * 0xC2B2AE3D27D4EB4Full);
    return key;
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
            next.ipd_set = true;
        } else if (!std::strcmp(token, "hybrid")) {
            next.hybrid = std::atoi(value) != 0;
        } else if (!std::strcmp(token, "guided")) {
            next.guided = std::atoi(value) != 0;
        } else if (!std::strcmp(token, "warpdump")) {
            next.warp_dump = std::atoi(value);
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
        } else if (!std::strcmp(token, "compute")) {
            next.compute = std::atoi(value) != 0;
        } else if (!std::strcmp(token, "full")) {
            next.full = std::atoi(value) != 0;
        } else if (!std::strcmp(token, "audit")) {
            next.audit = std::atoi(value);
        }
    }
    control = next;
    std::printf("Stereo: control offset %.3f m, ipd %.3f m, dup %d, replay %d, same %d, "
                "hybrid %d, guided %d, warpdump %d, compute %d, full %d\n",
                control.offset_m, control.ipd_m, control.dup, int(control.replay),
                int(control.same), int(control.hybrid), int(control.guided), control.warp_dump,
                int(control.compute), int(control.full));
    if (control.dup != dup_seen) {
        dup_seen = control.dup;
        dup_state = Dup::Armed;
        dump_pass = !control.dup_all;
    }
    if (control.audit != audit_seen) {
        audit_seen = control.audit;
        audit_armed = true;
    }
    if (control.warp_dump != warp_dump_seen) {
        warp_dump_seen = control.warp_dump;
        warp_dump_armed = control.warp_dump;
    }
}

/// The eye distance: the control file's "ipd=", else the runtime's, else BB_STEREO_IPD.
float Ipd() {
    if (control.ipd_set) {
        return control.ipd_m;
    }
    const float runtime = runtime_ipd.load(std::memory_order_relaxed);
    return runtime > 0.0f ? runtime : control.ipd_m;
}

/// dupall=1 or hybrid stereo: every frame's G-buffer pass is doubled (no dumps).
void ArmEveryFrame() {
    if ((control.dup_all || hybrid_frame) && dup_state == Dup::Off) {
        dup_state = Dup::Armed;
        dump_pass = false;
    }
}

Twin& EnsureTwin(const Instance& instance, u64 key, vk::Format format, u32 width, u32 height,
                 bool depth) {
    auto& slot = twins[key];
    if (slot && slot->format == format && slot->width == width && slot->height == height) {
        return *slot;
    }
    if (slot) {
        // Recorded commands (and captures' twin lists) may still use it: kept, never freed.
        retired_twins.push_back(std::move(slot));
        ++twins_generation;
        std::printf("Stereo: a twin replaced (key collision or resized target): kept retired\n");
    }
    slot = std::make_unique<Twin>();
    ++twins_generation;
    Twin& twin = *slot;
    const auto device = instance.GetDevice();
    twin.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    const auto attachment = depth ? vk::ImageUsageFlagBits::eDepthStencilAttachment
                                  : vk::ImageUsageFlagBits::eColorAttachment;
    // P4.4 slice 3: storage use where the format allows it (dispatches of the region write twins).
    twin.storage = !depth && bool(instance.GetPhysicalDevice().getFormatProperties(format)
                                      .optimalTilingFeatures &
                                  vk::FormatFeatureFlagBits::eStorageImage);
    twin.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = attachment | vk::ImageUsageFlagBits::eTransferSrc |
                 vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled |
                 (twin.storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlags{}),
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

/// Twins rest in GENERAL (section 10): eye R renders into, samples and stores to them in one
/// layout, also a twin a pass both renders into and samples (depth-tested light volumes reading
/// the depth, feedback loops), which the guest does in read-only or feedback layouts.
vk::ImageLayout AttachmentLayout(const Twin& twin) {
    (void)twin;
    return vk::ImageLayout::eGeneral;
}

// ---- P4.4 slice 3: compute per eye and eye-R descriptors (findings/31 section 5.3) ----
// A draw or dispatch recorded into an arena carries (guest view or buffer range -> twin) pairs
// (vk_stereo_substitute.h); the arena's eye-R execution pushes its descriptors with them. The
// twins eye R uses through descriptors are in GENERAL during that execution and back in their
// attachment layout after it (between operations every twin rests in its attachment layout).

/// The twins one capture's eye R uses through descriptors; for a render pass, its attachments
/// (never substituted: eye R renders into them).
struct CaptureTwins {
    std::vector<Twin*> used;
    std::vector<const Twin*> attachments;
    void Use(Twin* twin) {
        if (std::ranges::find(used, twin) == used.end()) {
            used.push_back(twin);
        }
    }
};
std::shared_ptr<CaptureTwins> capture_twins; ///< the render pass or dispatch being captured
std::vector<DescriptorSubstitute> pending_subs; ///< of the draw or dispatch being bound
std::vector<DescriptorSubstitute> taken_subs;   ///< handed to Pipeline::BindResources

/// Per doubled pass (its log line): what the region's dispatches did, and the substitutions.
struct ComputeStats {
    u32 both_eyes = 0;      ///< dispatches executed again for eye R
    u32 once = 0;           ///< read no twin: one execution serves both eyes
    u32 cut_short = 0;      ///< captures a direct recording ended (eye R lacks them)
    u32 twin_targets = 0;   ///< storage images of the region's size given a twin, twin buffers
    u32 shared_buffers = 0; ///< eye L only: buffer writes (exposure, CS_VFX_*, counters)
    u32 shared_gds = 0;     ///< eye L only: GDS
    u32 shared_images = 0;  ///< eye L only: storage images not of the region's size (LUT)
    u32 guard_test = 0;     ///< eye L only: guardtest=1
    u32 stale = 0;          ///< twins and twin buffers a single execution overwrote (invalidated)
    u32 views = 0;          ///< image descriptors substituted
    u32 buffers = 0;        ///< buffer descriptors substituted
    u32 missed = 0;         ///< views of twinned images kept (format, subrange, attachment)
};
ComputeStats compute_stats;
VideoCore::Extent3D region_extent{}; ///< the doubled pass's depth: storage targets this size get twins
constexpr u64 ComputeProfileKey = 0x57E2E0C0FFEull;
constexpr u64 TwinInitProfileKey = 0x57E2E0C0FFDull;
constexpr u64 ImageCopyProfileKey = 0x57E2E0C0FFCull;

/// The twin that doubles `id` in this region, if any. `history` (full stereo, reads): also last
/// region's twin of an image this region has not written yet (temporal history stays per eye).
Twin* TwinOf(VideoCore::ImageId id, bool history = false) {
    Twin* found = nullptr;
    for (Twin* twin : TwinsOfSource(id)) {
        const auto use = ClassifyTwin(twin->frame, twin_frame, history && full_frame);
        if (use == TwinUse::Current) {
            return twin;
        }
        if (use == TwinUse::History) {
            found = twin;
        }
    }
    return found;
}

/// `shared`: written by eye L for both eyes by design (a dispatch reading twins that writes shared
/// memory: exposure, particles); eye-R reads of it are shared reads, not leaks.
void NoteRegionWrite(VideoCore::ImageId id, bool shared = false) {
    if (!full_frame) {
        return;
    }
    auto& list = shared ? region_shared : region_written;
    if (std::ranges::find(list, id) == list.end()) {
        list.push_back(id);
    }
    if (!shared) {
        std::erase(region_shared, id);
    }
}

/// What a dispatch writes (images by id, buffers by guest range).
struct DispatchTargets {
    std::vector<VideoCore::ImageId> images;
    std::vector<std::pair<VAddr, u64>> buffers;
};

/// Eye L's execution alone wrote `id` for both eyes (or it got no eye R): its twins, current or
/// history, no longer hold eye R's version; readers fall back to the guest's (eye L's) contents.
u32 MarkImageStale(VideoCore::ImageId id) {
    u32 marked = 0;
    for (Twin* twin : TwinsOfSource(id)) {
        if (twin->frame != NoFrame) {
            twin->frame = NoFrame;
            ++marked;
        }
    }
    return marked;
}

/// A single execution (eye L's) wrote `written` for both eyes: the twins of it are stale, and
/// their readers fall back to the guest's (eye L's) contents.
void MarkStale(const DispatchTargets& written) {
    for (const auto id : written.images) {
        compute_stats.stale += MarkImageStale(id);
    }
    for (const auto& [address, size] : written.buffers) {
        for (auto& [base, twin] : twin_buffers) {
            if (twin.frame != NoFrame && address < base + twin.size && base < address + size) {
                twin.frame = NoFrame;
                ++compute_stats.stale;
            }
        }
    }
}

/// A view of `twin` that can stand for `guest` in eye R's descriptors; null when none can (other
/// format, a mip or layer subset, not 2D, a storage view of a twin without storage use).
vk::ImageView TwinViewFor(const Instance& instance, Twin& twin, const VideoCore::ImageView& guest) {
    const auto& range = guest.info.range;
    const bool storage = guest.info.is_storage;
    if (guest.vk_format != twin.format || range.base.level != 0 || range.base.layer != 0 ||
        range.extent.levels != 1 || range.extent.layers != 1 ||
        (guest.vk_type != vk::ImageViewType::e2D && guest.vk_type != vk::ImageViewType::e2DArray) ||
        (guest.vk_aspect & twin.aspect) != guest.vk_aspect || (storage && !twin.storage)) {
        return {};
    }
    for (const auto& view : twin.views) {
        if (view.aspect == guest.vk_aspect && view.type == guest.vk_type &&
            view.mapping == guest.info.mapping && view.storage == storage) {
            return *view.view;
        }
    }
    const vk::ImageViewUsageCreateInfo usage_ci{
        .usage = storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled};
    auto view = Check(instance.GetDevice().createImageViewUnique({
        .pNext = &usage_ci,
        .image = vk::Image(twin.image),
        .viewType = guest.vk_type,
        .format = twin.format,
        .components = guest.info.mapping,
        .subresourceRange = {guest.vk_aspect, 0, 1, 0, 1},
    }));
    const vk::ImageView handle = *view;
    twin.views.push_back(
        {guest.vk_aspect, guest.vk_type, guest.info.mapping, storage, std::move(view)});
    return handle;
}

/// Before an eye-R execution: eye R's earlier writes (attachments, storage, copies into twins)
/// are visible to it, and every twin it uses (`used`, `attachments`) is in GENERAL (a dump may
/// have left one in TRANSFER_SRC). The guest images' barriers recorded for eye L do not cover the
/// twins, so this is a global memory barrier.
void PrepareEyeR(Scheduler& scheduler, const std::vector<Twin*>& used,
                 const std::vector<const Twin*>& attachments) {
    std::vector<vk::ImageMemoryBarrier2> barriers;
    const auto fix = [&](Twin* twin) {
        if (twin->layout != vk::ImageLayout::eGeneral &&
            std::ranges::none_of(barriers, [&](const vk::ImageMemoryBarrier2& b) {
                return b.image == vk::Image(twin->image);
            })) {
            barriers.push_back(TwinBarrier(*twin, vk::ImageLayout::eGeneral));
        }
    };
    for (Twin* twin : used) {
        fix(twin);
    }
    for (const Twin* twin : attachments) {
        fix(const_cast<Twin*>(twin));
    }
    scheduler.Record([barriers = std::move(barriers)](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 memory{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,
                                 .pMemoryBarriers = &memory,
                                 .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                                 .pImageMemoryBarriers = barriers.data()});
    });
}

/// Records a copy of `image` (in `layout`, TRANSFER_SRC by default) into a host buffer,
/// written to a file once the GPU is past it.
void DumpImage(const Instance& instance, Scheduler& scheduler, vk::Image image,
               vk::ImageAspectFlags aspect, vk::Format format, u32 width, u32 height,
               const std::string& name,
               vk::ImageLayout layout = vk::ImageLayout::eTransferSrcOptimal) {
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
    scheduler.Record([image, buffer, copy_aspect, width, height,
                      layout](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(image, layout, buffer,
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

/// The doubled region's counters (the dumped G-buffer pass, or every 5 s the last full region).
void PrintRegionSummary(bool dumped, size_t targets_dumped = 0) {
    if (!dumped) {
        std::printf("Stereo: full region of frame %llu: %u draws, %u dispatches; full: %u "
                    "draws eye R cannot render (mip, layer, MSAA targets), %u image copies "
                    "redone between twins, %u copies or clears left to eye L, %u history "
                    "reads, %u leaks (eye R read an image eye L wrote, no twin), %u twin "
                    "format aliases, %u indirect draws, %u shared reads (eye L's by design)\n",
                    static_cast<unsigned long long>(twin_frame), dup_draws, dup_dispatches,
                    full_stats.not_doubled, full_stats.image_copies, full_stats.copies_stale,
                    full_stats.history, full_stats.leaks, full_stats.aliases,
                    full_stats.indirect, full_stats.shared_reads);
    }
    std::printf("Stereo: %s of frame %llu drawn twice (%u draws, %u dispatches in "
                "it), %zu targets dumped; %s: %u render passes executed twice "
                "(%llu commands), %u cut short, %u draws outside a capture; write guard: %u "
                "passes without eye R (guest writes), %u (attachment not a twin); eye-R buffer copies "
                "%u, not made %u; compute: %u dispatches executed twice, %u once (no twin read), "
                "%u cut short, %u twin targets; eye L only (shared): %u buffer writes, %u GDS, "
                "%u storage images not region size, %u guardtest; %u twins stale; eye-R "
                "descriptors: %u views, %u buffers substituted, %u views kept\n",
                dumped ? "G-buffer pass" : "full region",
                static_cast<unsigned long long>(twin_frame), dup_draws, dup_dispatches,
                targets_dumped,
                control.replay ? "record once/execute twice" : "per-draw prototype",
                replay_passes, static_cast<unsigned long long>(replay_commands), replay_aborted,
                replay_lost, replay_skipped_writes, replay_skipped_attachments, buffer_copies,
                buffer_unhandled, compute_stats.both_eyes, compute_stats.once,
                compute_stats.cut_short, compute_stats.twin_targets, compute_stats.shared_buffers,
                compute_stats.shared_gds, compute_stats.shared_images, compute_stats.guard_test,
                compute_stats.stale, compute_stats.views, compute_stats.buffers,
                compute_stats.missed);
}

/// The doubled pass is over: both eyes' targets to files (BB_DUMP_DIR).
void DumpPass(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
              VideoCore::TextureCache& texture_cache) {
    dup_state = Dup::Off;
    scheduler.EndRendering(); // the last captured render pass: eye R replay (P4.3)
    if (!dump_pass) {
        return;
    }
    std::vector<Twin*> used;
    for (auto& [key, twin] : twins) {
        if (twin->frame == twin_frame && twin->source) {
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
        const std::string base =
            "stereo_f" + std::to_string(twin_frame) + "_t" + std::to_string(index++);
        DumpImage(instance, scheduler, image.GetImage(), twin->aspect, twin->format, twin->width,
                  twin->height, base + "_L");
        DumpImage(instance, scheduler, vk::Image(twin->image), twin->aspect, twin->format,
                  twin->width, twin->height, base + "_R");
    }
    PrintRegionSummary(true, used.size());
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

// Hybrid stereo: eye R's final image, built at the end of the 3D region (vk_stereo.h).
constexpr u64 WarpProfileKey = 0x57E2E0A2Bull;
constexpr u64 FullCopyProfileKey = 0x57E2E0A2Dull;
constexpr u64 GuidedProfileKey = 0x57E2E0A2Cull;

struct WarpPipeline {
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    bool failed = false;
};
WarpPipeline warp_pipeline;

/// One slot of the eye-R ring (frame % 3).
struct EyeROutput {
    VideoCore::UniqueImage image;
    vk::UniqueImageView storage_view; ///< R8G8B8A8Unorm, the shader's target
    vk::UniqueImageView view;         ///< `format`, for consumers
    vk::Format format{};
    u32 width = 0, height = 0;
    u64 frame = ~0ull;
    VAddr display = 0;
    bool valid = false;
    bool rendered = false; ///< full stereo's own eye R (not warped from eye L)
    // The frame's eye offsets and eye R's depth twin (EyeRImage, for eye R's DLSS).
    float eye_l = 0.0f, eye_r = 0.0f;
    vk::Image depth_image;
    vk::ImageView depth_view;
    vk::Format depth_format{};
    u32 depth_width = 0, depth_height = 0;
};
std::array<EyeROutput, 3> eye_r_outputs;
std::mutex eye_r_mutex; ///< the slots' frame/display/valid (read by the presenter's thread)

bool IsSceneFormat(vk::Format format) {
    return format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb;
}

/// stereo_warp.comp's bindings: 3 the output, 6 the hole list, the rest sampled images.
vk::DescriptorType WarpDescriptorType(u32 binding) {
    return binding == 3   ? vk::DescriptorType::eStorageImage
           : binding == 6 ? vk::DescriptorType::eStorageBuffer
                          : vk::DescriptorType::eSampledImage;
}

/// The guided fill's hole list (stereo_warp.comp HoleList): a 16-byte header (count, capacity)
/// and one 16-byte entry per hole pixel, room for a quarter of the pixels (8.3 MB at 1080p;
/// holes past it keep the mirrored fill).
struct HoleList {
    VkBuffer buffer{};
    VmaAllocation allocation{};
    u32 capacity = 0;
};
HoleList hole_list;

bool EnsureHoleList(const Instance& instance, Scheduler& scheduler, u32 width, u32 height) {
    const u32 capacity = std::max(width * height / 4, 64u);
    if (hole_list.buffer && hole_list.capacity == capacity) {
        return true;
    }
    if (hole_list.buffer) {
        scheduler.DeferOperation([allocator = instance.GetAllocator(), old = hole_list] {
            vmaDestroyBuffer(allocator, old.buffer, old.allocation);
        });
        hole_list = {};
    }
    if (!CreateDeviceBuffer(instance, 16 + u64(capacity) * 16,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            hole_list.buffer, hole_list.allocation)) {
        hole_list = {};
        std::printf("Stereo: no hole list buffer: eye R's holes keep the mirrored fill\n");
        return false;
    }
    hole_list.capacity = capacity;
    return true;
}

/// The warp shader's views and buffers of one flip.
struct WarpBindings {
    std::array<vk::ImageView, 6> views; ///< bindings 0-5 (3: the output's storage view)
    vk::Buffer holes;
};

/// Binds the warp pipeline and pushes its descriptors.
void BindWarp(vk::CommandBuffer cmdbuf, const WarpBindings& b) {
    std::array<vk::DescriptorImageInfo, 6> infos{};
    for (u32 i = 0; i < infos.size(); ++i) {
        infos[i] = {.imageView = b.views[i], .imageLayout = vk::ImageLayout::eGeneral};
    }
    // Without the guided fill the shader never touches the hole list (still bound).
    const vk::DescriptorBufferInfo hole_info{.buffer = b.holes, .offset = 0,
                                             .range = VK_WHOLE_SIZE};
    std::array<vk::WriteDescriptorSet, 7> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {.dstBinding = i, .descriptorCount = 1, .descriptorType = WarpDescriptorType(i)};
        if (i == 6) {
            writes[i].pBufferInfo = &hole_info;
        } else {
            writes[i].pImageInfo = &infos[i];
        }
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *warp_pipeline.pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *warp_pipeline.layout, 0,
                                writes);
}

bool EnsureWarpPipeline(const Instance& instance) {
    if (warp_pipeline.pipeline || warp_pipeline.failed) {
        return bool(warp_pipeline.pipeline);
    }
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 7> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = WarpDescriptorType(i),
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    warp_pipeline.set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(StereoWarp::Params)};
    warp_pipeline.layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*warp_pipeline.set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto module = CompileSPV(STEREO_WARP_COMP, device);
    if (!module) {
        warp_pipeline.failed = true;
        std::printf("Stereo: the eye-R warp shader did not compile: no hybrid eye R\n");
        return false;
    }
    auto [result, pipeline] = device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *warp_pipeline.layout,
            });
    device.destroyShaderModule(module);
    if (result != vk::Result::eSuccess) {
        warp_pipeline.failed = true;
        std::printf("Stereo: no eye-R warp pipeline (%s): no hybrid eye R\n",
                    vk::to_string(result).c_str());
        return false;
    }
    warp_pipeline.pipeline = std::move(pipeline);
    return true;
}

/// The ring slot of frame `slot_frame`, (re)created for the display buffer's size and the
/// format the presenter samples it with. A replaced image
/// is destroyed once the GPU is past the commands recorded so far.
EyeROutput& EnsureEyeROutput(const Instance& instance, Scheduler& scheduler, u64 slot_frame,
                             u32 width, u32 height, vk::Format format) {
    auto& out = eye_r_outputs[slot_frame % eye_r_outputs.size()];
    if (out.image && out.width == width && out.height == height && out.format == format) {
        return out;
    }
    {
        std::scoped_lock lock{eye_r_mutex};
        out.valid = false;
        auto old = std::make_shared<std::tuple<VideoCore::UniqueImage, vk::UniqueImageView,
                                               vk::UniqueImageView>>(
            std::move(out.image), std::move(out.storage_view), std::move(out.view));
        scheduler.DeferOperation([old] {});
    }
    const auto device = instance.GetDevice();
    out.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    out.image.Create(vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage,
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    out.storage_view = Check(device.createImageViewUnique({
        .image = vk::Image(out.image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    // An sRGB view cannot be a storage view: this one is for sampling and copies only.
    const vk::ImageViewUsageCreateInfo view_usage{.usage = vk::ImageUsageFlagBits::eSampled};
    out.view = Check(device.createImageViewUnique({
        .pNext = &view_usage,
        .image = vk::Image(out.image),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        // As the presenter samples the display buffer: alpha is not the frame's.
        .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    out.width = width;
    out.height = height;
    out.format = format;
    std::printf("Stereo: eye-R image %s %ux%u (slot %llu)\n", vk::to_string(format).c_str(),
                width, height,
                static_cast<unsigned long long>(slot_frame % eye_r_outputs.size()));
    return out;
}

/// A view of a texture-cache image for this frame's warp, destroyed once the GPU is past it.
vk::ImageView TransientView(const Instance& instance, Scheduler& scheduler, vk::Image image,
                            vk::Format format, vk::ImageAspectFlags aspect) {
    const auto device = instance.GetDevice();
    const vk::ImageView view = Check(device.createImageView({
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    }));
    scheduler.DeferOperation([device, view] { device.destroyImageView(view); });
    return view;
}

/// A frame's warp inputs, from its region's end to its flip: eye R is warped from the display
/// buffer (the game's finished frame: its display pass applies the brightness curve and writes
/// the presenter's channel order, so eye R gets both exactly as eye L does; with the HUD layer
/// the display buffer has no UI).
struct PendingWarp {
    bool valid = false;
    u64 frame = 0;
    VAddr display = 0;           ///< set by the frame's display pass
    VideoCore::ImageId depth{};  ///< the region's depth (eye L)
    Twin* twin = nullptr;        ///< eye R's depth twin; null: dump only (no warp)
    Twin* albedo = nullptr;      ///< eye R's albedo twin (guided hole fill); null: mirrored fill
    Twin* normal = nullptr;      ///< eye R's normal twin (refines the guided fill)
    Twin* full = nullptr;        ///< full stereo: eye R's display buffer twin (its final image)
    float eye_l = 0.0f, eye_r = 0.0f, p00 = 0.0f, zs = 0.0f, zo = 0.0f;
    int dump = 0;                ///< warpdump id (0: none)
    bool full_region = false;    ///< the frame's region was a full region (else hybrid)
};
PendingWarp pending_warp;

bool IsDisplay8888(vk::Format format) {
    return format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb ||
           format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
}

/// The 3D region of a hybrid frame ends (the first Scaleform draw, or the display pass when
/// no UI was drawn): eye R's warp inputs (eye L's depth, eye R's depth twin, the camera) wait
/// for the frame's flip (WarpAtFlip). Also the "warpdump" dumps (with hybrid off: eye L only).
void RegionEnd(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
               VideoCore::TextureCache& texture_cache, VideoCore::ImageId color_id) {
    (void)scheduler, (void)runtime, (void)color_id;
    const bool warp = warp_pending;
    const int dump = warp_dump_armed;
    warp_pending = false;
    if (!warp && !dump) {
        return;
    }
    if (!region_depth) {
        warp_skipped += warp;
        return;
    }
    auto& depth = texture_cache.GetImage(region_depth);
    Twin* twin = nullptr;
    for (auto& [key, candidate] : twins) {
        if (candidate->depth && candidate->source == region_depth &&
            candidate->frame == twin_frame) {
            twin = candidate.get();
        }
    }
    const bool can_warp = warp && twin && sx > 0.0f && sz_o != 0.0f &&
                          twin->width == depth.info.size.width &&
                          twin->height == depth.info.size.height && EnsureWarpPipeline(instance);
    if (warp && !can_warp) {
        ++warp_skipped;
    }
    if (!can_warp && !dump) {
        return;
    }
    warp_dump_armed = 0;
    // The guided hole fill's G-buffer twins of this frame, at the depth twin's size.
    Twin* albedo = nullptr;
    Twin* normal = nullptr;
    if (can_warp && control.guided && gbuffer_slots_frame == twin_frame) {
        auto usable = [&](Twin* t, vk::Format format) {
            return t && t->frame == twin_frame && t->format == format && t->width == twin->width &&
                   t->height == twin->height;
        };
        for (Twin* t : gbuffer_slots) {
            if (usable(t, vk::Format::eR8G8B8A8Srgb)) {
                albedo = t;
            }
        }
        if (albedo && usable(gbuffer_slots[NormalSlot], vk::Format::eR8G8B8A8Unorm)) {
            normal = gbuffer_slots[NormalSlot];
        }
    }
    pending_warp = PendingWarp{
        .valid = true,
        .frame = frame,
        .depth = region_depth,
        .twin = can_warp ? twin : nullptr,
        .albedo = albedo,
        .normal = normal,
        .eye_l = hybrid_frame ? -frame_ipd / 2 : control.offset_m,
        .eye_r = control.same ? (hybrid_frame ? -frame_ipd / 2 : control.offset_m)
                              : frame_ipd / 2,
        .p00 = sx,
        .zs = sz_s,
        .zo = sz_o,
        .dump = dump,
        .full_region = full_frame,
    };
}

// ---- Probe (diagnostics for the headset "white" reports): every BB_STEREO_PROBE_SECONDS (10)
// the mean colour of eye L's display buffer and eye R's image (9 rows read back), the exposure
// image the game's cs 1f2b3c9e writes (first texels, NaN/Inf count) and the VRAM budget. ----
constexpr u64 ExposureShaderHash = 0x1f2b3c9eull;
VideoCore::ImageId exposure_image{}; ///< the storage image cs 1f2b3c9e wrote last
VideoCore::ImageId display_target{}; ///< the image the last display pass rendered into
VAddr display_target_address = 0;
VideoCore::ImageId display_input{}; ///< the image the last display pass sampled
std::vector<VideoCore::ImageId> watched_inputs; ///< BB_STEREO_PROBE_PS: that pixel shader's images
u64 WatchedPs() {
    static const u64 hash = [] {
        const char* env = std::getenv("BB_STEREO_PROBE_PS");
        return env ? std::strtoull(env, nullptr, 16) : 0ull;
    }();
    return hash;
}
u64 display_ps = 0;                  ///< and its pixel shader
std::chrono::steady_clock::time_point last_probe{};

double ProbeSeconds() {
    static const double seconds = [] {
        const char* env = std::getenv("BB_STEREO_PROBE_SECONDS");
        const double v = env ? std::strtod(env, nullptr) : 10.0;
        return v;
    }();
    return seconds;
}

struct ProbeResult {
    std::mutex mutex;
    std::string parts[4];
    u32 pending = 0;
    u64 frame = 0;
    std::string vram;
};

/// Copies `rows` rows of `image` (eGeneral) into a host buffer; `done` gets the bytes later.
void ProbeCopy(const Instance& instance, Scheduler& scheduler, vk::Image image,
               vk::ImageAspectFlags aspect, u32 width, u32 height, u32 bpp, u32 rows,
               std::function<void(const u8*, u32 row_bytes, u32 rows)> done) {
    rows = std::min(rows, height);
    const u64 row_bytes = u64(width) * bpp;
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = row_bytes * rows,
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
    std::vector<vk::BufferImageCopy> regions;
    for (u32 r = 0; r < rows; ++r) {
        const u32 y = rows == height ? r : (height * (2 * r + 1)) / (2 * rows);
        regions.push_back({.bufferOffset = r * row_bytes,
                           .imageSubresource = {aspect, 0, 0, 1},
                           .imageOffset = {0, s32(y), 0},
                           .imageExtent = {width, 1, 1}});
    }
    scheduler.Record([image, buffer, regions](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eGeneral, vk::Buffer(buffer), regions);
    });
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation,
                                      info, row_bytes, rows, done = std::move(done)] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        done(static_cast<const u8*>(info.pMappedData), u32(row_bytes), rows);
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

std::string MeanRgb(const u8* data, u32 row_bytes, u32 rows) {
    double sum[3]{};
    u64 n = 0;
    u32 white = 0;
    for (u32 r = 0; r < rows; ++r) {
        const u8* row = data + u64(r) * row_bytes;
        for (u32 x = 0; x + 4 <= row_bytes; x += 4, ++n) {
            sum[0] += row[x];
            sum[1] += row[x + 1];
            sum[2] += row[x + 2];
            white += row[x] > 240 && row[x + 1] > 240 && row[x + 2] > 240;
        }
    }
    char text[96];
    std::snprintf(text, sizeof(text), "%.1f/%.1f/%.1f (white %.1f%%)", n ? sum[0] / n : 0.0,
                  n ? sum[1] / n : 0.0, n ? sum[2] / n : 0.0, n ? 100.0 * white / n : 0.0);
    return text;
}

std::string FloatTexels(const u8* data, u32 bytes, vk::Format format) {
    // 32-bit float formats (R32, RG32, RGBA32) and 16-bit (R16, RGBA16): the first values, the
    // count of NaN and Inf over the row.
    const bool f32 = format == vk::Format::eR32Sfloat || format == vk::Format::eR32G32Sfloat ||
                     format == vk::Format::eR32G32B32A32Sfloat;
    const bool f16 = format == vk::Format::eR16Sfloat || format == vk::Format::eR16G16Sfloat ||
                     format == vk::Format::eR16G16B16A16Sfloat;
    const bool b10 = format == vk::Format::eB10G11R11UfloatPack32;
    const bool u8x4 = format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb ||
                      format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    std::string text = vk::to_string(format) + " [";
    u32 nan = 0, inf = 0, count = 0;
    double vmin = 1e30, vmax = -1e30, vsum = 0.0;
    const auto half = [](u16 h) {
        const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
        if (exp == 31) {
            return man ? std::numeric_limits<float>::quiet_NaN()
                       : (sign ? -1.0f : 1.0f) * std::numeric_limits<float>::infinity();
        }
        const float v = exp ? std::ldexp(1.0f + man / 1024.0f, int(exp) - 15)
                            : std::ldexp(man / 1024.0f, -14);
        return sign ? -v : v;
    };
    const auto uf = [](u32 bits, u32 man_bits) {
        const u32 exp = bits >> man_bits, man = bits & ((1u << man_bits) - 1);
        if (exp == 31) {
            return man ? std::numeric_limits<float>::quiet_NaN()
                       : std::numeric_limits<float>::infinity();
        }
        return exp ? std::ldexp(1.0f + float(man) / float(1u << man_bits), int(exp) - 15)
                   : std::ldexp(float(man) / float(1u << man_bits), -14);
    };
    for (u32 i = 0; f32 || f16 || b10 || u8x4; ++i) {
        float v;
        if (b10 || u8x4) {
            // Per channel: element i is channel i % 3 of texel i / 3 (alpha skipped).
            const u32 texel = i / 3, channel = i % 3;
            if ((texel + 1) * 4 > bytes) break;
            u32 word;
            std::memcpy(&word, data + texel * 4, 4);
            if (b10) {
                v = channel == 0 ? uf(word & 0x7ff, 6)
                    : channel == 1 ? uf((word >> 11) & 0x7ff, 6)
                                   : uf(word >> 22, 5);
            } else {
                v = float((word >> (8 * channel)) & 0xff);
            }
        } else if (f32) {
            if ((i + 1) * 4 > bytes) break;
            std::memcpy(&v, data + i * 4, 4);
        } else {
            if ((i + 1) * 2 > bytes) break;
            u16 h;
            std::memcpy(&h, data + i * 2, 2);
            v = half(h);
        }
        nan += std::isnan(v);
        inf += std::isinf(v);
        if (std::isfinite(v)) {
            vmin = std::min(vmin, double(v));
            vmax = std::max(vmax, double(v));
            vsum += v;
        }
        if (count++ < 8) {
            char t[24];
            std::snprintf(t, sizeof(t), "%s%g", count > 1 ? " " : "", v);
            text += t;
        }
    }
    if (!f32 && !f16 && !b10 && !u8x4) {
        char t[64];
        std::snprintf(t, sizeof(t), "raw %02x%02x%02x%02x", data[0], data[1], data[2], data[3]);
        text += t;
    }
    char t[160];
    std::snprintf(t, sizeof(t), "] min %g max %g mean %g NaN %u Inf %u of %u",
                  count ? vmin : 0.0, count ? vmax : 0.0, count ? vsum / count : 0.0, nan, inf,
                  count);
    return text + t;
}

std::string VramText(const Instance& instance) {
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(instance.GetAllocator(), budgets);
    const VkPhysicalDeviceMemoryProperties* props{};
    vmaGetMemoryProperties(instance.GetAllocator(), &props);
    u64 usage = 0, budget = 0, ours = 0;
    for (u32 h = 0; props && h < props->memoryHeapCount; ++h) {
        if (props->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            usage += budgets[h].usage;
            budget += budgets[h].budget;
            ours += budgets[h].statistics.blockBytes;
        }
    }
    char text[128];
    std::snprintf(text, sizeof(text), "VRAM usage %llu MB of budget %llu MB (this process's "
                  "allocator blocks %llu MB)",
                  static_cast<unsigned long long>(usage >> 20),
                  static_cast<unsigned long long>(budget >> 20),
                  static_cast<unsigned long long>(ours >> 20));
    return text;
}

/// Scratch memory for depth-to-color copies between twins (full stereo).
struct Scratch {
    VkBuffer buffer{};
    VmaAllocation allocation{};
    u64 size = 0;
};
Scratch copy_scratch;

/// Eye R's version of a guest image copy (full stereo): `src`'s twin into `dst`'s twin, as the
/// guest copy does (color to color bitwise; the depth aspect into a 32-bit color image through a
/// buffer, as Runtime::CopyImageWithBuffer). False when it cannot be redone.
bool CopyTwin(const Instance& instance, Scheduler& scheduler, Twin& src, Twin& dst) {
    if (src.width != dst.width || src.height != dst.height || dst.depth) {
        return false;
    }
    const bool depth_to_color = src.depth;
    if (depth_to_color ? BytesPerPixel(dst.format) != 4 ||
                             (src.format != vk::Format::eD32Sfloat &&
                              src.format != vk::Format::eD32SfloatS8Uint)
                       : BytesPerPixel(src.format) == 0 ||
                             BytesPerPixel(src.format) != BytesPerPixel(dst.format)) {
        return false;
    }
    const u64 bytes = u64(src.width) * src.height * 4;
    if (depth_to_color && copy_scratch.size < bytes) {
        if (copy_scratch.buffer) {
            scheduler.DeferOperation([allocator = instance.GetAllocator(), old = copy_scratch] {
                vmaDestroyBuffer(allocator, old.buffer, old.allocation);
            });
            copy_scratch = {};
        }
        if (!CreateDeviceBuffer(instance, bytes,
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                copy_scratch.buffer, copy_scratch.allocation)) {
            copy_scratch = {};
            return false;
        }
        copy_scratch.size = bytes;
    }
    const auto src_read = TwinBarrier(src, vk::ImageLayout::eTransferSrcOptimal);
    const auto dst_write = TwinBarrier(dst, vk::ImageLayout::eTransferDstOptimal);
    const auto src_back = TwinBarrier(src, vk::ImageLayout::eGeneral);
    const auto dst_back = TwinBarrier(dst, vk::ImageLayout::eGeneral);
    scheduler.Record([src_read, dst_write, src_back, dst_back, depth_to_color,
                      src_image = vk::Image(src.image), dst_image = vk::Image(dst.image),
                      w = src.width, h = src.height,
                      scratch = vk::Buffer(copy_scratch.buffer)](vk::CommandBuffer cmdbuf) {
        const std::array before{src_read, dst_write};
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(before.size()),
                                 .pImageMemoryBarriers = before.data()});
        if (depth_to_color) {
            const vk::BufferImageCopy depth{
                .imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
                .imageExtent = {w, h, 1}};
            const vk::BufferImageCopy color{
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {w, h, 1}};
            cmdbuf.copyImageToBuffer(src_image, vk::ImageLayout::eTransferSrcOptimal, scratch,
                                     depth);
            const vk::MemoryBarrier2 staged{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &staged});
            cmdbuf.copyBufferToImage(scratch, dst_image, vk::ImageLayout::eTransferDstOptimal,
                                     color);
        } else {
            const vk::ImageCopy region{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                       .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                       .extent = {w, h, 1}};
            cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                             vk::ImageLayout::eTransferDstOptimal, region);
        }
        const std::array after{src_back, dst_back};
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(after.size()),
                                 .pImageMemoryBarriers = after.data()});
    });
    return true;
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

bool ParseSceneConstants(const float* data, std::array<float, 3>& out) {
    // The main camera's block: far 3000 (as CameraMotion::OnConstants matches it, within 100: the
    // [0] value moves with the near plane, 3001.49 at first person's 0.2 m), 1/far, the render size.
    // The shadow cascades' blocks (far 103-140) do not match.
    if (std::abs(data[0] - 3000.0f) <= 100.0f && std::abs(data[1] * data[0] - 1.0f) < 1e-3f &&
        data[4] >= 64.0f && data[5] >= 64.0f && data[52] > 0.0f) {
        // depth = zs + zo / z (CameraMotion's proj z scale, z offset)
        out = {data[52], data[62], data[63]};
        return true;
    }
    return false;
}

void SetSceneConstants(const std::array<float, 3>& values) {
    sx = values[0];
    sz_s = values[1];
    sz_o = values[2];
}

void OnSceneConstants(const float* data) {
    std::array<float, 3> values;
    if (ParseSceneConstants(data, values)) {
        SetSceneConstants(values);
    }
}

u64 Frame() {
    return public_frame.load(std::memory_order_acquire);
}

namespace {
EyeRImage Describe(const EyeROutput& out) {
    return {.image = vk::Image(out.image),
            .view = *out.view,
            .format = out.format,
            .width = out.width,
            .height = out.height,
            .layout = vk::ImageLayout::eGeneral,
            .frame = out.frame,
            .display_buffer = out.display,
            .eye_l = out.eye_l,
            .eye_r = out.eye_r,
            .depth_image = out.depth_image,
            .depth_view = out.depth_view,
            .depth_format = out.depth_format,
            .depth_width = out.depth_width,
            .depth_height = out.depth_height,
            .rendered = out.rendered};
}
} // namespace

std::optional<EyeRImage> EyeRFinal(u64 frame_number) {
    std::scoped_lock lock{eye_r_mutex};
    for (const auto& out : eye_r_outputs) {
        if (out.valid && out.frame == frame_number) {
            return Describe(out);
        }
    }
    return std::nullopt;
}

std::optional<EyeRImage> EyeRForDisplay(VAddr display_buffer) {
    std::scoped_lock lock{eye_r_mutex};
    const EyeROutput* best = nullptr;
    for (const auto& out : eye_r_outputs) {
        if (out.valid && display_buffer && out.display == display_buffer &&
            (!best || out.frame > best->frame)) {
            best = &out;
        }
    }
    return best ? std::optional{Describe(*best)} : std::nullopt;
}

void SetIpd(float metres) {
    runtime_ipd.store(metres > 0.0f && metres < 0.2f ? metres : 0.0f,
                      std::memory_order_relaxed);
}

std::span<const DescriptorSubstitute> TakeSubstitutes() {
    taken_subs.swap(pending_subs);
    pending_subs.clear();
    return taken_subs;
}

} // namespace Stereo

void Rasterizer::StereoBeginDraw(const GraphicsPipeline* pipeline, bool indirect) {
    const BbStats::PerfScope perf{BbStats::PerfStereoDraw}; // the "Cmd:" line
    using namespace Stereo;
    if (!RemapShaders()) {
        return;
    }
    // (Regs() is inline in vk_rasterizer.cpp.)
    const auto& regs = stage_regs ? *stage_regs : liverpool->regs;
    const bool display =
        regs.color_buffers[0] && FrameCapture::IsDisplayBuffer(regs.color_buffers[0].Address());
    const bool boundary = display && !previous_display;
    const u64 vs = pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash;
    const bool gbuffer = GBufferDraw(pipeline, db_desc.first);
    const bool movie = UiComposition::MovieShader(vs);

    // Full stereo (section 10): the region doubled from the G-buffer to the display pass. A
    // region starts at a G-buffer draw of a hybrid frame (eye offsets, no oracle offset) with the
    // replay, the eye slot and per-eye dispatches available, and the scene rasterized at the
    // guest size (no reduced proxies: eye R's targets are twins of the guest images; VR runs the
    // upscaler "off", which only hosts the HUD layer). A display the game upscaler replaced is
    // caught at the flip (the warp then stands in).
    const bool native_raster =
        !upscaler->Enabled() || (!upscaler->RasterScaling() && upscaler->SceneMipBias() == 0.0f);
    const bool full_allowed = control.full && hybrid_frame && control.replay && control.compute &&
                              native_raster && !dump_pass && EnsureEyeSlot(instance);
    const auto step = full_region.OnDraw(boundary, display, gbuffer, movie, full_allowed);
    if (step.finish) {
        FinishFullRegion(); // the display pass is over: eye R's final image is complete
    }

    if (boundary) {
        if (dup_state == Dup::Active && !full_frame) {
            DumpPass(instance, scheduler, runtime, texture_cache); // the pass ended the frame
        }
        if (in_region && (warp_pending || warp_dump_armed)) {
            // A 3D frame without UI: its region ends here, the last scene target is the frame.
            warp_fallbacks += warp_pending;
            RegionEnd(instance, scheduler, runtime, texture_cache, last_scene_color);
        }
        warp_pending = false;
        {
            // This display buffer now holds a new frame: an older frame's eye R for it is stale
            // (EyeRForDisplay would pair it with this frame when this frame gets no warp, eye R
            // then lagging eye L by the buffer's turn: a moving scene drags in eye R only).
            const VAddr written = regs.color_buffers[0].Address();
            std::scoped_lock lock{eye_r_mutex};
            for (auto& out : eye_r_outputs) {
                if (out.display == written) {
                    out.valid = false;
                }
            }
        }
        display_target = cb_descs[0].first;
        display_target_address = regs.color_buffers[0].Address();
        if (pending_warp.valid && pending_warp.frame == frame) {
            // The display buffer this frame's display pass writes: what the presenter flips,
            // and eye R's warp source (Rasterizer::StereoWarpAtFlip).
            pending_warp.display = regs.color_buffers[0].Address();
        }
        ++frame;
        public_frame.store(frame, std::memory_order_release);
        ++frames_logged;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(5)) {
            if (frames_logged && last_log.time_since_epoch().count()) {
                std::printf("Stereo: %.1f region draws/frame, %.1f remapped (perspective), P00 "
                            "%.4f, offset %.3f m; hybrid %s, ipd %.4f m: %u eye-R warps in %u "
                            "frames (%u region ends at the display pass), %u skipped; full "
                            "stereo %s: %u regions, eye R rendered at %u flips, warp fallback "
                            "at %u\n",
                            double(region_draws) / frames_logged,
                            double(remapped_draws) / frames_logged, sx, control.offset_m,
                            hybrid_frame ? "on" : "off", frame_ipd, warps, frames_logged,
                            warp_fallbacks, warp_skipped, control.full ? "on" : "off",
                            full_window.frames, full_window.copies, full_window.fallbacks);
                if (full_window.frames) {
                    PrintRegionSummary(false);
                }
            }
            last_log = now;
            region_draws = remapped_draws = frames_logged = 0;
            warps = warp_fallbacks = warp_skipped = 0;
            full_window = {};
        }
        in_region = false;
        region_done = false;
        if (!control_defaults) {
            control_defaults = true;
            control.hybrid = Cfg().hybrid;
            control.guided = Cfg().guided;
            control.full = Cfg().full;
            control.ipd_m = Cfg().ipd_m;
        }
        PollControl();
        frame_ipd = Ipd();
        hybrid_frame = control.hybrid && control.offset_m == 0.0f;
        if (!full_frame) {
            ArmEveryFrame();
        }
    }
    previous_display = display;
    if (!in_region && !region_done && gbuffer) {
        in_region = true;
        region_depth = db_desc.first;
        last_scene_color = {};
        warp_pending = hybrid_frame;
    }
    if (in_region && movie) {
        in_region = false;
        region_done = true;
        // Hybrid stereo: the 3D region ends before this first Scaleform draw; its target (the
        // finished frame, ldr_target) holds the scene without the HUD.
        if (warp_pending || warp_dump_armed) {
            const auto color = cb_descs[0].first;
            const bool scene =
                color && IsSceneFormat(texture_cache.GetImage(color).info.pixel_format);
            RegionEnd(instance, scheduler, runtime, texture_cache,
                      scene ? color : last_scene_color);
        }
    }
    if (in_region && (warp_pending || warp_dump_armed) && cb_descs[0].first && region_depth) {
        // The region's last RGBA8 target of the scene's size: the frame when no UI follows.
        const auto& image = texture_cache.GetImage(cb_descs[0].first);
        const auto& depth = texture_cache.GetImage(region_depth);
        if (IsSceneFormat(image.info.pixel_format) &&
            image.info.size.width == depth.info.size.width &&
            image.info.size.height == depth.info.size.height) {
            last_scene_color = cb_descs[0].first;
        }
    }
    // This draw's targets: the color slots the pipeline writes, and the depth.
    const auto key_mask = pipeline->GetGraphicsKey().mrt_mask;
    struct Target {
        VideoCore::ImageId id;
        vk::Format format;
        bool depth;
        bool twinnable;
        bool cleared; ///< the pass clears it at its start (Rasterizer::BeginRendering's test)
    };
    boost::container::small_vector<Target, 9> targets; // (no allocation per draw)
    if (dup_state != Dup::Off || step.start || full_frame) {
        const auto twinnable = [&](VideoCore::ImageId id, const VideoCore::ImageViewInfo& view) {
            const auto& image = texture_cache.GetImage(id);
            return AttachmentTwinnable(view.range.base.level, view.range.base.layer,
                                       view.range.extent.layers, image.info.num_samples);
        };
        for (u32 cb = 0; cb < cb_descs.size(); ++cb) {
            if ((key_mask >> cb) & 1 && cb_descs[cb].first) {
                const auto& view = cb_descs[cb].second.view_info;
                const bool cleared = texture_cache.IsMetaCleared(
                    regs.color_buffers[cb].CmaskAddress(), view.range.base.layer);
                targets.push_back({cb_descs[cb].first, view.format, false,
                                   twinnable(cb_descs[cb].first, view), cleared});
            }
        }
        if (db_desc.first) {
            const auto& view = db_desc.second.view_info;
            const bool depth_clear =
                (regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
                 regs.depth_control.depth_write_enable) ||
                texture_cache.IsMetaCleared(regs.depth_htile_data_base.GetAddress(),
                                            view.range.base.layer);
            const bool stencil = texture_cache.GetImage(db_desc.first).info.props.has_stencil;
            const bool cleared = depth_clear &&
                                 (!stencil || regs.depth_render_control.stencil_clear_enable);
            targets.push_back({db_desc.first, view.format, true, twinnable(db_desc.first, view),
                               cleared});
        }
    }
    if (dup_state == Dup::Active && !full_frame) {
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
    const bool start_hybrid = dup_state == Dup::Armed && gbuffer && !step.start;
    if (step.start || start_hybrid) {
        if (step.start) {
            // A full region: it supersedes the hybrid's doubled G-buffer pass of this frame.
            full_frame = true;
            twin_frame = frame;
            full_display = {};
            full_stats = {};
            region_written.clear();
            region_shared.clear();
            audit_region = audit_armed;
            audit_armed = false;
            audit_pass = 0;
            if (audit_region) {
                std::printf("Stereo: audit: full region of frame %llu starts\n",
                            static_cast<unsigned long long>(frame));
            }
        } else {
            twin_frame = frame;
        }
        dup_state = Dup::Active;
        dup_draws = dup_dispatches = 0;
        replay_passes = replay_aborted = replay_lost = 0;
        replay_skipped_writes = replay_skipped_attachments = 0;
        buffer_copies = buffer_unhandled = 0;
        compute_stats = {};
        replay_commands = 0;
        pass_depth = db_desc.first;
        region_extent = texture_cache.GetImage(pass_depth).info.size; // P4.4 slice 3
        pass_colors.clear();
        for (const auto& target : targets) {
            if (!target.depth) {
                pass_colors.push_back(target.id);
            }
        }
        if (dump_pass) {
            std::printf("Stereo: duplicating the G-buffer pass of frame %llu (eyes at -/+ %.3f "
                        "m)\n",
                        static_cast<unsigned long long>(frame), frame_ipd / 2);
        }
    }
    bool doubled = dup_state == Dup::Active;
    if (full_frame) {
        doubled = step.doubled;
        if (doubled && std::ranges::any_of(targets, [](const Target& t) { return !t.twinnable; })) {
            // A target eye R cannot render into (a mip or layer, MSAA): eye L only, and eye R
            // reads eye L's result of it.
            doubled = false;
            ++full_stats.not_doubled;
            for (const auto& target : targets) {
                MarkImageStale(target.id);
            }
            static u32 printed = 0;
            if (audit_region || (WriteAudit() && printed++ < 16)) {
                std::string list;
                for (const auto& target : targets) {
                    const auto& image = texture_cache.GetImage(target.id);
                    list += " " + vk::to_string(target.format) + " " +
                            std::to_string(image.info.size.width) + "x" +
                            std::to_string(image.info.size.height) +
                            (target.twinnable ? "" : " (mip/layer/MSAA)");
                }
                std::printf("Stereo: audit: draw vs %016llx eye L only (eye R cannot render "
                            "into a target):%s\n",
                            static_cast<unsigned long long>(vs), list.c_str());
            }
        }
        if (doubled && display) {
            full_display = cb_descs[0].first;
        }
    }
    dup_this_draw = doubled;
    if (indirect && full_frame && full_region.Current() == FullRegion::Phase::Scene) {
        ++full_stats.indirect;
    }
    if (dup_this_draw && control.replay && !EnsureEyeSlot(instance)) {
        control.replay = false;
    }
    if (!dup_this_draw && full_frame && scheduler.IsCapturing()) {
        // The doubled pass before this draw ends here (eye R follows), before anything this draw
        // records directly (the HUD layer's UI start) could cut its capture short.
        scheduler.EndRendering();
    }
    if (dup_this_draw) {
        // Twins of this draw's targets start from the guest images' contents (the depth clear
        // ran in an earlier pass), copied before eye L draws into them.
        boost::container::small_vector<std::pair<Twin*, VideoCore::Image*>, 9> fresh;
        for (const auto& target : targets) {
            auto& image = texture_cache.GetImage(target.id);
            Twin& twin = EnsureTwin(instance, TwinKey(image, target.format),
                                    target.format, image.info.size.width, image.info.size.height,
                                    target.depth);
            if ((twin.source != target.id || twin.frame != twin_frame) &&
                !(full_frame && target.cleared)) {
                // (A target the pass clears needs no copy: eye R's pass clears its twin too.)
                fresh.emplace_back(&twin, &image);
            }
            SetSource(twin, target.id);
            twin.frame = twin_frame;
            NoteRegionWrite(target.id);
        }
        if (gbuffer) {
            gbuffer_slots.fill(nullptr);
            gbuffer_slots_frame = twin_frame;
            for (u32 cb = 0; cb < cb_descs.size() && cb < gbuffer_slots.size(); ++cb) {
                if ((key_mask >> cb) & 1 && cb_descs[cb].first) {
                    const auto it = twins.find(TwinKey(texture_cache.GetImage(cb_descs[cb].first),
                                                       cb_descs[cb].second.view_info.format));
                    gbuffer_slots[cb] = it != twins.end() ? it->second.get() : nullptr;
                }
            }
            if (!gbuffer_slots_logged) {
                gbuffer_slots_logged = true;
                std::string list;
                for (u32 cb = 0; cb < gbuffer_slots.size(); ++cb) {
                    if (gbuffer_slots[cb]) {
                        list += " " + std::to_string(cb) + ":" +
                                vk::to_string(gbuffer_slots[cb]->format);
                    }
                }
                std::printf("Stereo: G-buffer color slots%s (guided fill: albedo = the "
                            "R8G8B8A8Srgb slot, normals = slot %u)\n",
                            list.c_str(), NormalSlot);
            }
        }
        if (!fresh.empty()) {
            scheduler.EndRendering();
            auto* profiler = GpuProfiler::Get();
            const u64 resume = profiler ? profiler->Current() : 0;
            if (profiler && full_frame) {
                profiler->Mark(TwinInitProfileKey,
                               [] { return std::string{"stereo: eye R twin init copies"}; });
            }
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
            if (profiler && resume && full_frame) {
                profiler->Resume(resume);
            }
        }
    } else if (full_frame && full_region.Current() == FullRegion::Phase::Scene) {
        for (const auto& target : targets) {
            NoteRegionWrite(target.id); // eye L only: a leak if eye R reads it
        }
    }
    eye_m = !in_region                     ? 0.0f
            : dup_this_draw || hybrid_frame ? -frame_ipd / 2
                                            : control.offset_m;
    view_write = ~0u;
    if (in_region) {
        ++region_draws;
    }
}

void Rasterizer::FinishFullRegion() {
    using namespace Stereo;
    if (!full_frame) {
        full_region.Reset();
        return;
    }
    scheduler.EndRendering(); // the display pass's capture: eye R's display pass follows
    Twin* final_twin = full_display ? TwinOf(full_display) : nullptr;
    if (pending_warp.valid && pending_warp.frame == twin_frame && final_twin) {
        pending_warp.full = final_twin;
    }
    full_region.Reset();
    if (audit_region) {
        std::printf("Stereo: audit: full region of frame %llu ends: %u render passes listed; "
                    "eye R's final image %s\n",
                    static_cast<unsigned long long>(twin_frame), audit_pass,
                    final_twin ? "rendered (display twin)" : "missing");
        audit_region = false;
        PrintRegionSummary(false);
    }
    DumpPass(instance, scheduler, runtime, texture_cache); // dup_state Off
    full_frame = false;
    full_display = {};
    last_region_written = region_written;
    last_pass_targets = pass_targets;
    last_pass_shaders = pass_shaders;
    pass_targets.clear();
    pass_shaders.clear();
    pass_counter = 0;
    region_written.clear();
    region_shared.clear();
    ++full_window.frames;
    ArmEveryFrame();
}

void Rasterizer::StereoProbe(VideoCore::ImageId color_id, u32 width, u32 height) {
    const BbStats::PerfScope perf{BbStats::PerfStereoProbe}; // the "Cmd:" line
    using namespace Stereo;
    static const bool forced = std::getenv("BB_STEREO_PROBE_SECONDS") != nullptr;
    if (!RemapShaders() && !forced) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (ProbeSeconds() <= 0.0 ||
        now - last_probe < std::chrono::duration<double>(ProbeSeconds())) {
        return;
    }
    last_probe = now;
    scheduler.EndRendering();
    auto result = std::make_shared<ProbeResult>();
    result->frame = frame;
    result->vram = VramText(instance);
    {
        // Which image the presenter flips vs which one the display pass rendered into.
        char ids[512];
        const auto describe = [&](VideoCore::ImageId id, char* out, size_t n) {
            if (!id) {
                std::snprintf(out, n, "none");
                return;
            }
            const auto& im = texture_cache.GetImage(id);
            std::snprintf(out, n, "id %u %llx %s %ux%u%s", id.index,
                          static_cast<unsigned long long>(im.info.guest_address),
                          vk::to_string(im.info.pixel_format).c_str(), im.info.size.width,
                          im.info.size.height,
                          im.flags & VideoCore::ImageFlagBits::GpuModified ? " gpu" : "");
        };
        char a[112], b[112];
        describe(color_id, a, sizeof(a));
        describe(display_target, b, sizeof(b));
        char c[112];
        describe(display_input, c, sizeof(c));
        std::snprintf(ids, sizeof(ids), "flip %s, display pass target %s (%llx), input %s ps %llx",
                      a, b, static_cast<unsigned long long>(display_target_address), c,
                      static_cast<unsigned long long>(display_ps));
        result->vram += std::string("; ") + ids;
    }
    constexpr u32 Rows = 9;
    const auto finish = [result] {
        // (Called with the mutex held, when the last part arrived.)
        std::printf("Stereo: probe frame %llu: eye L display %s; eye R %s; exposure %s; display "
                    "input %s; %s\n",
                    static_cast<unsigned long long>(result->frame), result->parts[0].c_str(),
                    result->parts[1].c_str(), result->parts[2].c_str(), result->parts[3].c_str(),
                    result->vram.c_str());
        std::fflush(stdout);
    };
    std::vector<std::function<void()>> jobs;
    result->parts[0] = "-";
    result->parts[1] = "-";
    result->parts[2] = "-";
    result->parts[3] = "-";
    const auto part = [result, finish](int index, std::string text) {
        std::scoped_lock lock{result->mutex};
        result->parts[index] = std::move(text);
        if (--result->pending == 0) {
            finish();
        }
    };
    u32 pending = 0;
    VideoCore::Image* color = color_id ? &texture_cache.GetImage(color_id) : nullptr;
    const bool color_ok = color && BytesPerPixel(color->info.pixel_format) == 4 &&
                          !color->info.props.is_depth;
    const EyeROutput* eye_r = nullptr;
    {
        std::scoped_lock lock{eye_r_mutex};
        for (const auto& out : eye_r_outputs) {
            if (out.valid && out.image && (!eye_r || out.frame > eye_r->frame)) {
                eye_r = &out;
            }
        }
    }
    VideoCore::Image* exposure = exposure_image ? &texture_cache.GetImage(exposure_image) : nullptr;
    const u32 exposure_bpp = exposure ? BytesPerPixel(exposure->info.pixel_format) : 0;
    VideoCore::Image* input = display_input ? &texture_cache.GetImage(display_input) : nullptr;
    const bool input_ok = input && BytesPerPixel(input->info.pixel_format) == 4 &&
                          !input->info.props.is_depth && input->info.num_samples == 1;
    pending = (color_ok ? 1 : 0) + (eye_r ? 1 : 0) + (exposure && exposure_bpp ? 1 : 0) +
              (input_ok ? 1 : 0);
    if (!pending) {
        std::printf("Stereo: probe frame %llu: no image to read; %s\n",
                    static_cast<unsigned long long>(frame), result->vram.c_str());
        return;
    }
    result->pending = pending;
    if (color_ok) {
        runtime.Transit(color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                        vk::AccessFlagBits2::eTransferRead);
    }
    if (exposure && exposure_bpp) {
        runtime.Transit(exposure, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                        vk::AccessFlagBits2::eTransferRead);
    }
    if (input_ok) {
        runtime.Transit(input, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                        vk::AccessFlagBits2::eTransferRead);
    }
    runtime.FlushBarriers();
    if (input_ok) {
        const vk::Format format = input->info.pixel_format;
        ProbeCopy(instance, scheduler, input->GetImage(), vk::ImageAspectFlagBits::eColor,
                  input->info.size.width, input->info.size.height, 4, Rows,
                  [part, format](const u8* d, u32 rb, u32 r) {
                      part(3, format == vk::Format::eB10G11R11UfloatPack32
                                  ? FloatTexels(d, rb * r, format)
                                  : MeanRgb(d, rb, r));
                  });
    }
    if (color_ok) {
        ProbeCopy(instance, scheduler, color->GetImage(), vk::ImageAspectFlagBits::eColor,
                  color->info.size.width, color->info.size.height, 4, Rows,
                  [part](const u8* d, u32 rb, u32 r) { part(0, MeanRgb(d, rb, r)); });
    }
    if (eye_r) {
        ProbeCopy(instance, scheduler, vk::Image(eye_r->image), vk::ImageAspectFlagBits::eColor,
                  eye_r->width, eye_r->height, 4, Rows,
                  [part](const u8* d, u32 rb, u32 r) { part(1, MeanRgb(d, rb, r)); });
    }
    if (exposure && exposure_bpp) {
        const vk::Format format = exposure->info.pixel_format;
        const std::string size = std::to_string(exposure->info.size.width) + "x" +
                                 std::to_string(exposure->info.size.height) + " ";
        ProbeCopy(instance, scheduler, exposure->GetImage(), vk::ImageAspectFlagBits::eColor,
                  exposure->info.size.width, exposure->info.size.height, exposure_bpp,
                  exposure->info.size.height,
                  [part, format, size](const u8* d, u32 rb, u32 r) {
                      part(2, size + FloatTexels(d, rb * r, format));
                  });
    }
    (void)width, (void)height;
    static u32 watch_probes = 0;
    if (WatchedPs() && !watched_inputs.empty() && watch_probes++ % 3 == 0 && watch_probes < 30) {
        const u64 watch_frame = frame;
        for (const auto id : watched_inputs) {
            auto& image = texture_cache.GetImage(id);
            const vk::Format format = image.info.pixel_format;
            const u32 bpp = BytesPerPixel(format);
            if (!bpp || image.info.props.is_depth || image.info.num_samples != 1) {
                std::printf("Stereo: watch frame %llu: image id %u %s %ux%u not read\n",
                            static_cast<unsigned long long>(watch_frame), id.index,
                            vk::to_string(format).c_str(), image.info.size.width,
                            image.info.size.height);
                continue;
            }
            runtime.Transit(&image, vk::ImageLayout::eGeneral,
                            vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            char head[128];
            std::snprintf(head, sizeof(head), "id %u %llx %ux%u ", id.index,
                          static_cast<unsigned long long>(image.info.guest_address),
                          image.info.size.width, image.info.size.height);
            const std::string name = head;
            ProbeCopy(instance, scheduler, image.GetImage(), vk::ImageAspectFlagBits::eColor,
                      image.info.size.width, image.info.size.height, bpp,
                      std::min(image.info.size.height, 5u),
                      [name, format, watch_frame](const u8* d, u32 rb, u32 r) {
                          std::printf("Stereo: watch frame %llu: image %s%s\n",
                                      static_cast<unsigned long long>(watch_frame), name.c_str(),
                                      FloatTexels(d, rb * r, format).c_str());
                      });
        }
    }
    static const bool chain = [] {
        const char* env = std::getenv("BB_STEREO_PROBE_CHAIN");
        return env && env[0] == '1';
    }();
    static u32 chains = 0;
    static u32 chain_probes = 0;
    if (!chain || last_pass_targets.empty() || chains >= 6 || chain_probes++ % 3 != 0) {
        return;
    }
    ++chains;
    const u64 chain_frame = frame;
    std::vector<std::pair<u32, VideoCore::ImageId>> order;
    for (const auto& [pass, id] : last_pass_targets) {
        std::erase_if(order, [&](const auto& e) { return e.second == id; });
        order.emplace_back(pass, id);
    }
    for (u32 i = 0; i < order.size(); ++i) {
        const u32 pass = order[i].first;
        auto& image = texture_cache.GetImage(order[i].second);
        const vk::Format format = image.info.pixel_format;
        const u32 bpp = BytesPerPixel(format);
        if (!bpp || image.info.props.is_depth || image.info.num_samples != 1) {
            continue;
        }
        runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                        vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
        char head[160];
        std::snprintf(head, sizeof(head), "pass %u ps %llx id %u %llx %ux%u ", pass,
                      static_cast<unsigned long long>(
                          pass < last_pass_shaders.size() ? last_pass_shaders[pass] : 0),
                      order[i].second.index,
                      static_cast<unsigned long long>(image.info.guest_address),
                      image.info.size.width, image.info.size.height);
        const std::string name = head;
        ProbeCopy(instance, scheduler, image.GetImage(), vk::ImageAspectFlagBits::eColor,
                  image.info.size.width, image.info.size.height, bpp, 5,
                  [name, format, chain_frame](const u8* d, u32 rb, u32 r) {
                      std::printf("Stereo: chain probe frame %llu: eye L image %s%s\n",
                                  static_cast<unsigned long long>(chain_frame), name.c_str(),
                                  FloatTexels(d, rb * r, format).c_str());
                  });
    }
}

void Rasterizer::StereoAfterIndirectDraw(const GraphicsPipeline* pipeline) {
    using namespace Stereo;
    if (!RemapShaders() || !dup_this_draw || dup_state != Dup::Active || !control.replay) {
        return;
    }
    bool writes = false;
    for (const auto* stage : pipeline->GetStages()) {
        writes |= stage && StageWritesMemory(*stage);
    }
    const u64 vs = pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash;
    const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    static std::vector<u64> logged;
    if (std::ranges::find(logged, vs) == logged.end() && logged.size() < 64) {
        logged.push_back(vs);
        std::printf("Stereo: indirect draw in a doubled pass: vs %016llx ps %016llx, shaders %s "
                    "memory%s\n",
                    static_cast<unsigned long long>(vs),
                    static_cast<unsigned long long>(ps ? ps->pgm_hash : 0),
                    writes ? "write" : "do not write",
                    writes ? " (its pass gets no eye R)" : "");
    }
    ++dup_draws;
    if (!scheduler.IsCapturing()) {
        ++replay_lost;
    } else if (pass_audit && (writes || control.guard_test) &&
               pass_audit->writing_draws++ == 0) {
        pass_audit->first_writer = vs << 32 | (ps ? ps->pgm_hash & 0xffffffffull : 0);
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
                            ? twins.find(TwinKey(texture_cache.GetImage(cb_descs[cb].first),
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
            TwinKey(texture_cache.GetImage(db_desc.first), db_desc.second.view_info.format));
        if (it != twins.end()) {
            twin_state.depth_stencil_attachment.image_view = *it->second->view;
            twin_state.depth_stencil_attachment.image_layout = AttachmentLayout(*it->second);
        }
    }
    scheduler.EndRendering();
    scheduler.BeginRendering(twin_state);
    if (view_write != ~0u && view_write < set_writes.size()) {
        eye_m = control.same ? -frame_ipd / 2 : frame_ipd / 2;
        const auto view = CurrentView();
        auto& stream = buffer_cache.GetStreamBuffer();
        const u64 offset =
            stream.Copy(view.data(), sizeof(view), instance.StorageMinAlignment());
        const vk::DescriptorBufferInfo info{stream.Handle(), offset, sizeof(view)};
        const auto* saved = set_writes[view_write].pBufferInfo;
        set_writes[view_write].pBufferInfo = &info;
        pipeline->BindResources(set_writes, push_data);
        set_writes[view_write].pBufferInfo = saved;
        eye_m = -frame_ipd / 2;
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
    const BbStats::PerfScope perf{BbStats::PerfStereoEmit}; // the "Cmd:" line
    using namespace Stereo;
    if (!RemapShaders()) {
        return;
    }
    if (const u64 watched = WatchedPs()) {
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        static u64 watched_frame = ~0ull;
        if (ps && ps->pgm_hash == watched && watched_frame != frame) {
            watched_frame = frame; // the first draw of the frame with that shader
            watched_inputs.clear();
            for (const auto id : bound_images) {
                if (std::ranges::find(watched_inputs, id) == watched_inputs.end()) {
                    watched_inputs.push_back(id);
                }
            }
            static u32 dumps = 0;
            if (dumps++ % 30 == 0 && dumps < 3000) {
                // What the GPU reads: the constant ring copies (and other buffers) of this draw.
                std::string text;
                for (u32 w = 0; w < set_writes.size(); ++w) {
                    const auto* info = set_writes[w].pBufferInfo;
                    if (!info) {
                        continue;
                    }
                    char t[96];
                    std::snprintf(t, sizeof(t), " [w%u %s+%llx:", w,
                                  info->buffer == constant_ring->Handle() ? "ring" : "buf",
                                  static_cast<unsigned long long>(info->offset));
                    text += t;
                    if (info->buffer == constant_ring->Handle()) {
                        const float* f =
                            reinterpret_cast<const float*>(constant_ring->Data(info->offset));
                        const u32 n = std::min<u32>(u32(info->range / 4), 24);
                        for (u32 i = 0; i < n; ++i) {
                            std::snprintf(t, sizeof(t), " %g", f[i]);
                            text += t;
                        }
                    }
                    text += "]";
                }
                for (u32 i = 0; i < ps->buffers.size(); ++i) {
                    const auto sharp = ps->buffers[i].GetSharp(*ps);
                    char t[96];
                    std::snprintf(t, sizeof(t), " ps buffer %u guest %llx size %u", i,
                                  static_cast<unsigned long long>(sharp.base_address),
                                  sharp.GetSize());
                    text += t;
                }
                std::printf("Stereo: watch frame %llu: draw constants:%s\n",
                            static_cast<unsigned long long>(frame), text.c_str());
            }
        }
    }
    if (previous_display) {
        // (The probe: what the display pass samples.)
        for (const auto id : bound_images) {
            if (id != cb_descs[0].first) {
                display_input = id;
                const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
                display_ps = ps ? ps->pgm_hash : 0;
                break;
            }
        }
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
        if (control.compute) {
            StereoCollectSubstitutes(true); // P4.4 slice 3: eye R samples the twins
        }
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
    const float eye_l = -frame_ipd / 2;
    const float eye_r = control.same ? eye_l : frame_ipd / 2;
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
                            ? twins.find(TwinKey(texture_cache.GetImage(cb_descs[cb].first),
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
                            ? twins.find(TwinKey(texture_cache.GetImage(db_desc.first),
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
    // P4.4 slice 3: the twins eye R samples (filled per draw); its attachments may be among them
    // (all twins rest in GENERAL: a depth-tested pass that samples the same depth reads its twin).
    auto capture = std::make_shared<CaptureTwins>();
    for (const auto& [key, twin] : twins) {
        for (u32 cb = 0; cb < twin_state.num_color_attachments; ++cb) {
            if (twin_state.color_attachments[cb].image_view == *twin->view) {
                capture->attachments.push_back(twin.get());
            }
        }
        if (twin_state.depth_stencil_attachment.image_view == *twin->view) {
            capture->attachments.push_back(twin.get());
        }
    }
    capture_twins = capture;
    if (full_frame) {
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        pass_shaders.push_back(ps ? ps->pgm_hash : 0);
        for (u32 cb = 0; cb < cb_descs.size(); ++cb) {
            if ((key_mask >> cb) & 1 && cb_descs[cb].first) {
                pass_targets.emplace_back(pass_counter, cb_descs[cb].first);
            }
        }
        ++pass_counter;
    }
    if (audit_region) {
        std::string list;
        for (u32 cb = 0; cb < cb_descs.size(); ++cb) {
            if ((key_mask >> cb) & 1 && cb_descs[cb].first) {
                const auto& image = texture_cache.GetImage(cb_descs[cb].first);
                list += " " + vk::to_string(cb_descs[cb].second.view_info.format) + " " +
                        std::to_string(image.info.size.width) + "x" +
                        std::to_string(image.info.size.height) +
                        (twin_state.color_attachments[cb].image_view ? "" : " (no twin)");
            }
        }
        if (db_desc.first) {
            const auto& image = texture_cache.GetImage(db_desc.first);
            list += " depth " + vk::to_string(db_desc.second.view_info.format) + " " +
                    std::to_string(image.info.size.width) + "x" +
                    std::to_string(image.info.size.height) +
                    (twin_state.depth_stencil_attachment.image_view ? "" : " (no twin)");
        }
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        std::printf("Stereo: audit: pass %u vs %016llx ps %016llx both eyes, targets:%s\n",
                    audit_pass++,
                    static_cast<unsigned long long>(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash),
                    static_cast<unsigned long long>(ps ? ps->pgm_hash : 0), list.c_str());
    }
    const bool begun = scheduler.BeginCapture(
        std::make_shared<RecordArena>(), [this, twin_state, view_r, audit,
                                          capture](std::shared_ptr<RecordArena> arena,
                                                   bool complete) {
            const BbStats::PerfScope perf{BbStats::PerfStereoEyeR};
            if (pass_audit == audit) {
                pass_audit.reset();
            }
            if (capture_twins == capture) {
                capture_twins.reset();
            }
            // A pass eye R does not get: its targets' twins miss it, so eye R reads eye L's
            // result of them from here on (full stereo; the hybrid's G-buffer the same way).
            const auto without_eye_r = [&] {
                for (const Twin* twin : capture->attachments) {
                    MarkImageStale(twin->source);
                }
            };
            if (!complete) {
                ++replay_aborted;
                without_eye_r();
                return;
            }
            if (audit->guest_attachment || audit->writing_draws) {
                ++(audit->guest_attachment ? replay_skipped_attachments : replay_skipped_writes);
                without_eye_r();
                if (WriteAudit() || audit_region) {
                    static u32 printed = 0;
                    if (printed++ < 32 || audit_region) {
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
            PrepareEyeR(scheduler, capture->used, capture->attachments);
            scheduler.BeginRendering(twin_state);
            scheduler.Record([arena = std::move(arena)](vk::CommandBuffer cmdbuf) {
                arena->Execute(cmdbuf, true); // eye R: its descriptors (TakeSubstitutes)
            });
            scheduler.EndRendering();
            if (profiler && resume) {
                profiler->Resume(resume);
            }
        });
    if (!begun) {
        pass_audit.reset();
        capture_twins.reset();
        ++replay_lost; // not deferred recording: this pass has no eye R
    } else if (control.compute) {
        StereoCollectSubstitutes(true); // P4.4 slice 3: the pass's first draw
    }
}

void Rasterizer::StereoAfterDispatch(const Shader::Info& cs) {
    const BbStats::PerfScope perf{BbStats::PerfStereoDispatch}; // the "Cmd:" line
    using namespace Stereo;
    if (scheduler.IsCapturing()) {
        scheduler.EndCapture(); // P4.4 slice 3: the dispatch's commands, then eye R's execution
    }
    if (RemapShaders() && cs.pgm_hash == ExposureShaderHash && ProbeSeconds() > 0.0) {
        // The probe's exposure image: the storage image this dispatch wrote.
        for (u32 i = 0; i < cs.images.size(); ++i) {
            const u32 w = u32(cs.buffers.size()) + i;
            if (!cs.images[i].is_written || w >= set_writes.size() || !set_writes[w].pImageInfo) {
                continue;
            }
            const vk::ImageView handle = set_writes[w].pImageInfo[0].imageView;
            for (const auto id : bound_images) {
                for (const auto view_id : texture_cache.GetImage(id).backing->image_view_ids) {
                    if (*texture_cache.GetImageView(view_id).image_view == handle) {
                        exposure_image = id;
                    }
                }
            }
        }
    }
    if (!RemapShaders() || dup_state != Dup::Active || !control.replay ||
        cs.pgm_hash != CopyShaderHash || cs.buffers.size() != 2) {
        return;
    }
    const bool first_written = cs.buffers[0].is_written;
    const auto src = cs.buffers[first_written ? 1 : 0].GetSharp(cs);
    const auto dst = cs.buffers[first_written ? 0 : 1].GetSharp(cs);
    const auto src_id = texture_cache.FindImageFromRange(src.base_address, src.GetSize());
    if (const auto dst_id = texture_cache.FindImageFromRange(dst.base_address, dst.GetSize(), false)) {
        // An image target the dispatch wrote through memory (an image copy never gets here: it
        // skips the dispatch). Section 3's copy 3, the depth (and the stencil behind it) into an
        // R32F image: full stereo redoes it between the twins (the depth aspect into the R32F
        // twin); otherwise the image's twins are stale.
        if (src_id && full_frame) {
            StereoRedoCopy(src_id, dst_id);
        } else {
            MarkImageStale(dst_id);
        }
        return;
    }
    Twin* twin = nullptr;
    for (auto& [key, candidate] : twins) {
        if (src_id && candidate->source == src_id && candidate->frame == twin_frame) {
            twin = candidate.get();
        }
    }
    if (full_frame && src_id) {
        // Full stereo: a copy into the memory of an image it does not match exactly (section 3,
        // copy 3: the depth plane and the stencil behind it into an R32F image): eye R's copy
        // goes between the twins (the depth aspect into the R32F twin).
        VideoCore::ImageId dst_image{};
        texture_cache.ForEachImageInRegion(
            dst.base_address, dst.GetSize(), [&](VideoCore::ImageId id, VideoCore::Image& image) {
                if (!dst_image && image.info.guest_address == dst.base_address) {
                    dst_image = id;
                }
            });
        if (dst_image) {
            StereoRedoCopy(src_id, dst_image);
            return;
        }
    }
    static const bool twin_buffers = [] {
        const char* env = std::getenv("BB_STEREO_TWIN_BUFFERS");
        return !(env && env[0] == '0');
    }();
    if (!twin_buffers) {
        ++buffer_unhandled;
        return;
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
    target->frame = twin_frame;
    const u32 index = buffer_copies++;
    if (dump_pass) {
        // Both eyes' bytes: the guest copy's result (eye L) and the twin buffer (eye R).
        const auto [guest, offset] =
            buffer_cache.ObtainBuffer(dst.base_address, dst.GetSize(), false);
        const std::string base =
            "stereo_f" + std::to_string(twin_frame) + "_t" + std::to_string(100 + index);
        DumpBuffer(instance, scheduler, guest->Handle(), offset, dst.GetSize(), base + "_L");
        DumpBuffer(instance, scheduler, vk::Buffer(target->buffer), 0, dst.GetSize(),
                   base + "_R");
    }
}

void Rasterizer::StereoBeforeDispatch() {
    using namespace Stereo;
    // Dispatches inside the doubled pass (the HLE constant copies among them) are counted here;
    // those reading twins are executed again for eye R (StereoBeginDispatch, P4.4 slice 3).
    if (RemapShaders() && dup_state == Dup::Active) {
        ++dup_dispatches;
    }
}

bool Rasterizer::StereoRedoCopy(VideoCore::ImageId src_id, VideoCore::ImageId dst_id) {
    using namespace Stereo;
    const bool region = full_frame && dup_state == Dup::Active && control.compute &&
                        full_region.Current() == FullRegion::Phase::Scene;
    if (!region) {
        MarkImageStale(dst_id); // written once for both eyes: readers take the guest's
        return false;
    }
    NoteRegionWrite(dst_id);
    Twin* src = TwinOf(src_id);
    auto& dst_image = texture_cache.GetImage(dst_id);
    const vk::Format dst_format = dst_image.info.pixel_format;
    bool done = false;
    if (src && !dst_image.info.props.is_depth && dst_image.info.resources.levels == 1 &&
        dst_image.info.resources.layers == 1 && dst_image.info.num_samples == 1) {
        Twin& dst = EnsureTwin(instance, TwinKey(dst_image, dst_format),
                               dst_format, dst_image.info.size.width, dst_image.info.size.height,
                               false);
        scheduler.EndRendering();
        auto* profiler = GpuProfiler::Get();
        const u64 resume = profiler ? profiler->Current() : 0;
        if (profiler) {
            profiler->Mark(ImageCopyProfileKey,
                           [] { return std::string{"stereo: eye R image copies"}; });
        }
        done = CopyTwin(instance, scheduler, *src, dst);
        if (profiler && resume) {
            profiler->Resume(resume);
        }
        if (done) {
            SetSource(dst, dst_id);
            dst.frame = twin_frame;
            ++full_stats.image_copies;
            if (audit_region) {
                std::printf("Stereo: audit: image copy %s -> %s %ux%u redone between the twins\n",
                            vk::to_string(src->format).c_str(), vk::to_string(dst_format).c_str(),
                            dst_image.info.size.width, dst_image.info.size.height);
            }
        }
    }
    if (!done) {
        // Eye R reads eye L's copy (a leak when the source was per eye).
        MarkImageStale(dst_id);
        ++full_stats.copies_stale;
        static u32 printed = 0;
        if (audit_region || (WriteAudit() && printed++ < 16)) {
            {
                std::printf("Stereo: audit: image copy into %s %ux%u not redone for eye R (%s)\n",
                            vk::to_string(dst_format).c_str(), dst_image.info.size.width,
                            dst_image.info.size.height,
                            src ? "formats or sizes" : "the source has no eye-R twin");
            }
        }
    }
    return done;
}

void Rasterizer::StereoAfterImageCopy(VideoCore::ImageId src_id, VideoCore::ImageId dst_id) {
    if (Stereo::RemapShaders()) {
        StereoRedoCopy(src_id, dst_id); // bbport: compute image copies (IsComputeImageCopy)
    }
}

void Rasterizer::StereoAfterImageClear(VideoCore::ImageId id) {
    using namespace Stereo;
    if (RemapShaders()) {
        MarkImageStale(id); // the same clear for both eyes: eye R reads the guest's until written
    }
}

std::optional<Stereo::EyeRImage> Rasterizer::StereoWarpAtFlip(VAddr display_buffer,
                                                              VideoCore::ImageId color_id,
                                                              vk::Image raw_color, u32 width,
                                                              u32 height,
                                                              vk::Format view_format) {
    using namespace Stereo;
    if (full_region.Current() == FullRegion::Phase::Display) {
        // The flip follows the display pass: eye R's display pass (its capture) is recorded now.
        FinishFullRegion();
    }
    StereoProbe(color_id, width, height);
    PendingWarp& pw = pending_warp;
    if (!pw.valid || !display_buffer || pw.display != display_buffer) {
        return std::nullopt;
    }
    pw.valid = false;
    // The depth and the twin still hold the frame: no newer region has started since.
    const bool current = frame == pw.frame + 1 && !in_region && !region_done &&
                         (!pw.twin || pw.twin->frame == pw.frame);
    const bool format_ok = IsDisplay8888(view_format);
    if (!current || !format_ok) {
        warp_skipped += pw.twin != nullptr;
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("Stereo: no eye-R warp at a flip: %s\n",
                        !format_ok ? "the display buffer is not 8-bit RGBA"
                                   : "a newer frame's 3D region has started");
        }
        return std::nullopt;
    }
    auto& depth = texture_cache.GetImage(pw.depth);
    scheduler.EndRendering();
    constexpr auto read_stages =
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
    constexpr auto read_access =
        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead;
    // The upscaled display, or the display buffer's pre-SR DLSS-NR copy (eGeneral).
    vk::Image color_image = raw_color;
    if (color_id) {
        auto& color = texture_cache.GetImage(color_id);
        runtime.Transit(&color, vk::ImageLayout::eGeneral, read_stages, read_access);
        if (!raw_color) {
            color_image = color.GetImage();
        }
    }
    if (raw_color) {
        const vk::MemoryBarrier2 written{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                         .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                         .dstStageMask = read_stages,
                                         .dstAccessMask = read_access};
        scheduler.Record([written](vk::CommandBuffer cmdbuf) {
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
        });
    }
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, read_stages, read_access);
    runtime.FlushBarriers();

    StereoWarp::Params params{};
    params.width = float(width);
    params.height = float(height);
    params.depth_width = float(depth.info.size.width);
    params.depth_height = float(depth.info.size.height);
    params.shift = StereoWarp::Shift(pw.p00, pw.eye_r, pw.eye_l, float(width));
    params.zs = pw.zs;
    params.zo = pw.zo;
    params.tolerance_rel = StereoWarp::TolRel;
    params.tolerance_abs = StereoWarp::TolAbs;
    params.tolerance_px = StereoWarp::TolPx;
    params.search = StereoWarp::SearchRadius(params.shift);
    // The guided hole fill: eye R's albedo (and normal) twins still hold this frame (only the
    // next frame's G-buffer pass writes them; `current` says it has not started).
    // The shader binds the hole list even without the guided fill (allocated once per size).
    if (pw.twin && !EnsureHoleList(instance, scheduler, width, height)) {
        warp_skipped += 1;
        return std::nullopt;
    }
    Twin* albedo = pw.albedo && pw.albedo->frame == pw.frame ? pw.albedo : nullptr;
    Twin* normal = albedo && pw.normal && pw.normal->frame == pw.frame ? pw.normal : nullptr;
    if (albedo) {
        params.flags |= StereoWarp::FlagAlbedo;
        params.flags |= normal ? StereoWarp::FlagNormal : 0;
        params.flags |= view_format == vk::Format::eB8G8R8A8Srgb ||
                                view_format == vk::Format::eB8G8R8A8Unorm
                            ? StereoWarp::FlagSwapRB
                            : 0;
        params.flags |= view_format == vk::Format::eB8G8R8A8Srgb ||
                                view_format == vk::Format::eR8G8B8A8Srgb
                            ? StereoWarp::FlagSrgb
                            : 0;
    }
    static bool guided_logged = false;
    if (pw.twin && !guided_logged) {
        guided_logged = true;
        std::printf("Stereo: eye-R hole fill: %s\n",
                    albedo ? (normal ? "G-buffer guided (albedo x lighting ratio, normals)"
                                     : "G-buffer guided (albedo x lighting ratio, no normals)")
                           : "mirrored background (no albedo twin, or guided=0)");
    }

    EyeROutput* out = nullptr;
    Twin* twin = pw.twin;
    // Full stereo: eye R's final image was rendered (the display pass for eye R into the display
    // buffer's twin, which nothing writes until that buffer's next display pass); it replaces the
    // warp. Only for the guest display buffer itself (an upscaled display is not the twin's size).
    Twin* full = pw.full && pw.full->frame == pw.frame && color_id && pw.full->width == width &&
                         pw.full->height == height && BytesPerPixel(pw.full->format) == 4 &&
                         !pw.full->depth
                     ? pw.full
                     : nullptr;
    if (pw.full_region && !full) {
        ++full_window.fallbacks;
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("Stereo: full stereo: no rendered eye R at a flip (%s): the warp stands "
                        "in (counted as warp fallback)\n",
                        !pw.full            ? "the display pass got no eye R"
                        : !color_id         ? "the display buffer is upscaled"
                        : pw.full->frame != pw.frame ? "its twin holds another frame"
                                                     : "size or format");
        }
    }
    if (twin && full) {
        out = &EnsureEyeROutput(instance, scheduler, pw.frame, width, height, view_format);
        if (!twin->depth_view) {
            twin->depth_view = Check(instance.GetDevice().createImageViewUnique({
                .image = vk::Image(twin->image),
                .viewType = vk::ImageViewType::e2D,
                .format = twin->format,
                .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
            }));
        }
        auto* profiler = GpuProfiler::Get();
        const u64 resume = profiler ? profiler->Current() : 0;
        if (profiler) {
            profiler->Mark(FullCopyProfileKey, [] {
                return std::string{"stereo: eye R full stereo (display twin to the eye R image)"};
            });
        }
        const auto to_src = TwinBarrier(*full, vk::ImageLayout::eTransferSrcOptimal);
        const auto back = TwinBarrier(*full, vk::ImageLayout::eGeneral);
        const auto depth_general = TwinBarrier(*twin, vk::ImageLayout::eGeneral);
        const vk::Image out_image = vk::Image(out->image);
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .extent = {width, height, 1},
        };
        scheduler.Record([to_src, back, depth_general, out_image, region,
                          src = vk::Image(full->image)](vk::CommandBuffer cmdbuf) {
            const vk::ImageMemoryBarrier2 out_dst{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined, // every texel is rewritten
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .image = out_image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            const std::array before{to_src, out_dst, depth_general};
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(before.size()),
                                     .pImageMemoryBarriers = before.data()});
            // The twin's bytes as stored (an R8G8B8A8 sRGB twin into the UNORM image: the copy
            // is bitwise), as the warp copies the display buffer's bytes.
            cmdbuf.copyImage(src, vk::ImageLayout::eTransferSrcOptimal, out_image,
                             vk::ImageLayout::eTransferDstOptimal, region);
            vk::ImageMemoryBarrier2 out_general = out_dst;
            out_general.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
            out_general.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
            out_general.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
            out_general.dstAccessMask = vk::AccessFlagBits2::eMemoryRead;
            out_general.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            out_general.newLayout = vk::ImageLayout::eGeneral;
            const std::array after{out_general, back};
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(after.size()),
                                     .pImageMemoryBarriers = after.data()});
        });
        if (profiler && resume) {
            profiler->Resume(resume);
        }
        {
            std::scoped_lock lock{eye_r_mutex};
            out->frame = pw.frame;
            out->display = display_buffer;
            out->valid = true;
            out->eye_l = pw.eye_l;
            out->eye_r = pw.eye_r;
            out->depth_image = vk::Image(twin->image);
            out->depth_view = *twin->depth_view;
            out->depth_format = twin->format;
            out->depth_width = twin->width;
            out->depth_height = twin->height;
            out->rendered = true;
        }
        ++full_window.copies;
    } else if (twin) {
        // The warp copies the display buffer's bytes as stored (through UNORM views, in its
        // own channel order); the presenter samples eye R with eye L's view format.
        out = &EnsureEyeROutput(instance, scheduler, pw.frame, width, height, view_format);
        if (!twin->depth_view) {
            twin->depth_view = Check(instance.GetDevice().createImageViewUnique({
                .image = vk::Image(twin->image),
                .viewType = vk::ImageViewType::e2D,
                .format = twin->format,
                .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
            }));
        }
        const vk::ImageView color_view =
            TransientView(instance, scheduler, color_image, vk::Format::eR8G8B8A8Unorm,
                          vk::ImageAspectFlagBits::eColor);
        const vk::ImageView depth_view =
            TransientView(instance, scheduler, depth.GetImage(), depth.info.pixel_format,
                          vk::ImageAspectFlagBits::eDepth);
        auto* profiler = GpuProfiler::Get();
        const u64 resume = profiler ? profiler->Current() : 0;
        if (profiler) {
            profiler->Mark(WarpProfileKey,
                           [] { return std::string{"stereo: eye R warp (hybrid)"}; });
        }
        // Unused bindings (no albedo, no normals) get a valid view the flags tell it to ignore.
        const vk::ImageView albedo_view = albedo ? *albedo->view : color_view;
        const vk::ImageView normal_view = normal ? *normal->view : albedo_view;
        std::array<vk::ImageMemoryBarrier2, 4> barriers{};
        u32 barrier_count = 0;
        barriers[barrier_count++] = TwinBarrier(*twin, vk::ImageLayout::eGeneral);
        if (albedo) {
            barriers[barrier_count++] = TwinBarrier(*albedo, vk::ImageLayout::eGeneral);
        }
        if (normal) {
            barriers[barrier_count++] = TwinBarrier(*normal, vk::ImageLayout::eGeneral);
        }
        const vk::ImageMemoryBarrier2 out_barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined, // every texel is rewritten
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(out->image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        barriers[barrier_count++] = out_barrier;
        const WarpBindings bindings{
            .views = {color_view, depth_view, *twin->depth_view, *out->storage_view, albedo_view,
                      normal_view},
            .holes = vk::Buffer(hole_list.buffer)};
        const u32 hole_capacity = hole_list.capacity;
        const bool guided = (params.flags & StereoWarp::FlagAlbedo) != 0;
        scheduler.Record([barriers, barrier_count, params, bindings, hole_capacity, guided,
                          layout = *warp_pipeline.layout](vk::CommandBuffer cmdbuf) {
            if (guided) {
                // The hole list starts empty (count 0, capacity), after its last use.
                const vk::MemoryBarrier2 before{
                    .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .srcAccessMask =
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                };
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
                cmdbuf.fillBuffer(bindings.holes, 0, 4, 0);
                cmdbuf.fillBuffer(bindings.holes, 4, 4, hole_capacity);
                const vk::MemoryBarrier2 after{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                    .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask =
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                };
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
            }
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = barrier_count,
                                     .pImageMemoryBarriers = barriers.data()});
            BindWarp(cmdbuf, bindings);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                                 &params);
            cmdbuf.dispatch((u32(params.width) + 7) / 8, (u32(params.height) + 7) / 8, 1);
            if (guided) {
                const vk::MemoryBarrier2 listed{
                    .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask =
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                };
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &listed});
            }
        });
        if (guided) {
            // The listed hole pixels, one thread each (64 per group; groups past the count exit
            // at once): its own profiler segment.
            if (profiler) {
                profiler->Mark(GuidedProfileKey,
                               [] { return std::string{"stereo: eye R guided hole fill"}; });
            }
            scheduler.Record([params, bindings, hole_capacity,
                              layout = *warp_pipeline.layout](vk::CommandBuffer cmdbuf) {
                BindWarp(cmdbuf, bindings);
                StereoWarp::Params second = params;
                second.flags |= StereoWarp::FlagGuidedPass;
                cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(second),
                                     &second);
                cmdbuf.dispatch((hole_capacity + 63) / 64, 1, 1);
            });
        }
        scheduler.Record([](vk::CommandBuffer cmdbuf) {
            // Eye R is read by whatever comes later on the queue (the presenter's chain).
            const vk::MemoryBarrier2 done{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        });
        if (profiler && resume) {
            profiler->Resume(resume);
        }
        {
            std::scoped_lock lock{eye_r_mutex};
            out->frame = pw.frame;
            out->display = display_buffer;
            out->valid = true;
            out->eye_l = pw.eye_l;
            out->eye_r = pw.eye_r;
            out->depth_image = vk::Image(twin->image);
            out->depth_view = *twin->depth_view;
            out->depth_format = twin->format;
            out->depth_width = twin->width;
            out->depth_height = twin->height;
            out->rendered = false;
        }
        ++warps;
    }
    if (pw.dump) {
        // warpdump: eye L (the display buffer) and its depth, eye R's image and depth twin
        // (files warp<n>_f<frame>_{color,depth}_{L,R}_<format>_<w>x<h>.raw in BB_DUMP_DIR; the
        // color files hold the display buffer's bytes, read with <format>).
        const std::string base =
            "warp" + std::to_string(pw.dump) + "_f" + std::to_string(pw.frame);
        DumpImage(instance, scheduler, color_image, vk::ImageAspectFlagBits::eColor, view_format,
                  width, height, base + "_color_L", vk::ImageLayout::eGeneral);
        DumpImage(instance, scheduler, depth.GetImage(), vk::ImageAspectFlagBits::eDepth,
                  depth.info.pixel_format, depth.info.size.width, depth.info.size.height,
                  base + "_depth_L", vk::ImageLayout::eGeneral);
        if (out) {
            DumpImage(instance, scheduler, vk::Image(out->image), vk::ImageAspectFlagBits::eColor,
                      view_format, width, height, base + "_color_R", vk::ImageLayout::eGeneral);
            DumpImage(instance, scheduler, vk::Image(twin->image), vk::ImageAspectFlagBits::eDepth,
                      twin->format, twin->width, twin->height, base + "_depth_R",
                      vk::ImageLayout::eGeneral);
            // Eye R's G-buffer color twins by MRT slot (warp<n>_f<frame>_slot<i>_R_...): which
            // holds the albedo and the normals (section 9.10). BB_STEREO_DUMP_SLOTS=1 (about 65
            // MB per dump).
            static const bool dump_slots = [] {
                const char* env = std::getenv("BB_STEREO_DUMP_SLOTS");
                return env && env[0] == '1';
            }();
            if (dump_slots && gbuffer_slots_frame == pw.frame) {
                for (u32 cb = 0; cb < gbuffer_slots.size(); ++cb) {
                    Twin* slot = gbuffer_slots[cb];
                    if (!slot || slot->frame != pw.frame) {
                        continue;
                    }
                    const auto barrier = TwinBarrier(*slot, vk::ImageLayout::eGeneral);
                    scheduler.Record([barrier](vk::CommandBuffer cmd) {
                        cmd.pipelineBarrier2(
                            {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
                    });
                    DumpImage(instance, scheduler, vk::Image(slot->image), slot->aspect,
                              slot->format, slot->width, slot->height,
                              base + "_slot" + std::to_string(cb) + "_R",
                              vk::ImageLayout::eGeneral);
                }
            }
        }
        std::printf("Stereo: warp %d of frame %llu: %s, eye L %.4f m, eye R %.4f m, P00 %.5f, "
                    "shift %.4f px*m, zs %.8f, zo %.8f, search %d, %ux%u (from the display "
                    "buffer, %s)\n",
                    pw.dump, static_cast<unsigned long long>(pw.frame),
                    out ? (full ? "eye R rendered" : "eye R warped")
                        : "eye L only (no warp this frame)",
                    pw.eye_l, pw.eye_r,
                    pw.p00, params.shift, params.zs, params.zo, params.search, width, height,
                    vk::to_string(view_format).c_str());
    }
    if (!out) {
        return std::nullopt;
    }
    std::scoped_lock lock{eye_r_mutex};
    return Describe(*out);
}

// ---- P4.4 slice 3: compute per eye and eye-R descriptors (findings/31 section 5.3) ----

bool Rasterizer::StereoCollectSubstitutes(bool graphics) {
    const BbStats::PerfScope perf{BbStats::PerfStereoSubs}; // the "Cmd:" line
    using namespace Stereo;
    pending_subs.clear();
    CaptureTwins* const capture = capture_twins.get();
    if (!capture) {
        return false; // nothing would put the twins in GENERAL for eye R
    }
    const auto referenced = [&](vk::ImageView view) {
        for (const auto& write : set_writes) {
            if (!write.pImageInfo || write.descriptorType == vk::DescriptorType::eSampler) {
                continue;
            }
            for (u32 i = 0; i < write.descriptorCount; ++i) {
                if (write.pImageInfo[i].imageView == view) {
                    return true;
                }
            }
        }
        return false;
    };
    // Images: the bound images doubled by a twin in this region (or, full stereo, last region's
    // twin of an image this region has not written yet: its history), through the views this
    // binding uses. Twins rest in GENERAL, so a twin eye R renders into in this pass can also be
    // sampled (a depth-tested pass reading the same depth).
    (void)graphics;
    for (const auto image_id : bound_images) {
        const bool written_here =
            full_frame && std::ranges::find(region_written, image_id) != region_written.end();
        const auto& image = texture_cache.GetImage(image_id);
        std::array<Twin*, 4> candidates{};
        u32 count = 0;
        for (Twin* twin : TwinsOfSource(image_id)) {
            // (The size check: an image id the texture cache reused for another image.)
            if (count < candidates.size() &&
                twin->width == image.info.size.width && twin->height == image.info.size.height &&
                ClassifyTwin(twin->frame, twin_frame, full_frame && !written_here) !=
                    TwinUse::None) {
                candidates[count++] = twin;
            }
        }
        if (count > 1) {
            ++full_stats.aliases; // twins of two view formats: each view takes its own
        }
        for (const auto view_id : image.backing->image_view_ids) {
            const auto& view = texture_cache.GetImageView(view_id);
            const vk::ImageView handle = *view.image_view;
            if (!referenced(handle) ||
                std::ranges::any_of(pending_subs, [&](const DescriptorSubstitute& sub) {
                    return sub.guest_view == handle;
                })) {
                continue;
            }
            Twin* chosen = nullptr;
            vk::ImageView twin_view{};
            for (u32 i = 0; i < count && !twin_view && image.backing->num_samples == 1; ++i) {
                twin_view = TwinViewFor(instance, *candidates[i], view);
                chosen = candidates[i];
            }
            if (!twin_view) {
                if (count) {
                    ++compute_stats.missed;
                } else if (full_frame && std::ranges::find(region_shared, image_id) !=
                                             region_shared.end()) {
                    ++full_stats.shared_reads;
                } else if (written_here) {
                    // Eye L wrote this image in the region and eye R has no version of it.
                    ++full_stats.leaks;
                    static u32 printed = 0;
                    static std::vector<std::pair<u64, u32>> listed; // (region, image) once
                    const std::pair<u64, u32> entry{twin_frame, image_id.index};
                    const bool first = std::ranges::find(listed, entry) == listed.end();
                    if (first && (audit_region || (WriteAudit() && printed++ < 24))) {
                        listed.push_back(entry);
                        std::printf("Stereo: audit: leak: eye R reads eye L's %s %ux%u (no twin; "
                                    "view %s)\n",
                                    vk::to_string(image.info.pixel_format).c_str(),
                                    image.info.size.width, image.info.size.height,
                                    vk::to_string(view.vk_format).c_str());
                    }
                }
                continue;
            }
            pending_subs.push_back({.guest_view = handle,
                                    .twin_view = twin_view,
                                    .twin_layout = vk::ImageLayout::eGeneral});
            capture->Use(chosen);
            ++compute_stats.views;
            full_stats.history += chosen->frame != twin_frame ? 1 : 0;
        }
    }
    // Buffers: bindings inside a twin buffer of this frame (eye R's bytes of the copy shader's
    // targets, slice 2), at the same offset in the twin.
    const u64 alignment = instance.StorageMinAlignment();
    for (const auto& bound : bound_buffers) {
        const u64 offset_aligned = Common::AlignDown(bound.offset, alignment);
        const VAddr start = bound.buffer->CpuAddr() + offset_aligned;
        const VAddr end = bound.buffer->CpuAddr() + bound.offset + bound.size;
        for (const auto& [address, twin] : twin_buffers) {
            if (twin.frame != twin_frame || start < address || end > address + twin.size ||
                (start - address) % alignment != 0) {
                continue;
            }
            const vk::Buffer guest = bound.buffer->Handle();
            if (std::ranges::none_of(pending_subs, [&](const DescriptorSubstitute& sub) {
                    return sub.guest_buffer == guest && sub.guest_offset == offset_aligned;
                })) {
                pending_subs.push_back({.guest_buffer = guest,
                                        .guest_offset = offset_aligned,
                                        .twin_buffer = vk::Buffer(twin.buffer),
                                        .twin_offset = start - address});
                ++compute_stats.buffers;
            }
            break;
        }
    }
    return !pending_subs.empty();
}

void Rasterizer::StereoBeginDispatch(const ComputePipeline* pipeline) {
    const BbStats::PerfScope perf{BbStats::PerfStereoDispatch}; // the "Cmd:" line
    using namespace Stereo;
    if (!RemapShaders() || dup_state != Dup::Active || !control.replay || !control.compute ||
        scheduler.IsCapturing()) {
        return;
    }
    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    auto capture = std::make_shared<CaptureTwins>();
    capture_twins = capture;
    const bool reads = StereoCollectSubstitutes(false);
    const auto substituted_view = [&](vk::ImageView view) {
        return std::ranges::any_of(pending_subs, [&](const DescriptorSubstitute& sub) {
            return sub.guest_view == view;
        });
    };
    const auto substituted_buffer = [&](const vk::DescriptorBufferInfo& info) {
        return std::ranges::any_of(pending_subs, [&](const DescriptorSubstitute& sub) {
            return sub.guest_buffer == info.buffer && sub.guest_offset == info.offset;
        });
    };
    // The bound image whose view `handle` is.
    const auto owner = [&](vk::ImageView handle,
                           const VideoCore::ImageView*& found) -> VideoCore::ImageId {
        for (const auto image_id : bound_images) {
            for (const auto view_id : texture_cache.GetImage(image_id).backing->image_view_ids) {
                const auto& view = texture_cache.GetImageView(view_id);
                if (*view.image_view == handle) {
                    found = &view;
                    return image_id;
                }
            }
        }
        return {};
    };
    const auto storage_format = [&](vk::Format format) {
        const auto features =
            instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
        return bool(features & vk::FormatFeatureFlagBits::eStorageImage) &&
               bool(features & vk::FormatFeatureFlagBits::eColorAttachment);
    };

    // The dispatch's writes. A compute stage's descriptor writes are [buffers][images][samplers]:
    // write i is buffer i, write buffers.size() + i is image i.
    const bool indexed = set_writes.size() >= cs.buffers.size() + cs.images.size();
    DispatchWrites writes;
    struct Target {
        VideoCore::ImageId id;
        const VideoCore::ImageView* view;
    };
    std::vector<Target> targets; // storage images to give a twin
    DispatchTargets written;     // their twins go stale unless eye R writes them
    for (u32 i = 0; i < cs.buffers.size(); ++i) {
        const auto& desc = cs.buffers[i];
        if (!desc.is_written) {
            continue;
        }
        if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
            ++writes.shared_gds;
            continue;
        }
        if (desc.IsSpecial()) {
            continue; // host-side (LDS emulation, fault buffer): not guest memory
        }
        const auto* info = indexed ? set_writes[i].pBufferInfo : nullptr;
        for (const auto& bound : bound_buffers) {
            if (info && bound.buffer->Handle() == info->buffer &&
                Common::AlignDown(bound.offset, instance.StorageMinAlignment()) == info->offset) {
                written.buffers.emplace_back(bound.buffer->CpuAddr() + bound.offset, bound.size);
            }
        }
        if (info && substituted_buffer(*info)) {
            ++writes.twin_targets; // inside a twin buffer: eye R writes the twin
        } else {
            ++writes.shared_buffers;
        }
    }
    for (u32 i = 0; i < cs.images.size(); ++i) {
        if (!cs.images[i].is_written) {
            continue;
        }
        const auto* write = indexed ? &set_writes[cs.buffers.size() + i] : nullptr;
        if (!write || !write->pImageInfo) {
            ++writes.shared_images;
            continue;
        }
        for (u32 k = 0; k < write->descriptorCount; ++k) {
            const vk::ImageView handle = write->pImageInfo[k].imageView;
            if (!handle) {
                continue;
            }
            const VideoCore::ImageView* view = nullptr;
            const auto id = owner(handle, view);
            if (id) {
                written.images.push_back(id);
            }
            if (substituted_view(handle)) {
                ++writes.twin_targets; // already twinned this frame
                continue;
            }
            const auto* image = id ? &texture_cache.GetImage(id) : nullptr;
            // Hybrid: storage images of the region's size; full stereo: any plain 2D color image
            // of at least 64x64 (half-resolution AO, blur and bloom targets).
            const bool sized =
                image && (full_frame || (image->info.size.width == region_extent.width &&
                                         image->info.size.height == region_extent.height));
            const bool region =
                sized && view &&
                StorageTwinnable(image->info.size.width, image->info.size.height,
                                 view->info.range.extent.levels, view->info.range.extent.layers,
                                 view->info.range.base.level, view->info.range.base.layer,
                                 view->vk_type == vk::ImageViewType::e2D ||
                                     view->vk_type == vk::ImageViewType::e2DArray,
                                 !image->info.props.is_depth &&
                                     view->vk_aspect == vk::ImageAspectFlagBits::eColor,
                                 image->backing->num_samples == 1, storage_format(view->vk_format));
            if (region) {
                targets.push_back({id, view});
                ++writes.twin_targets;
            } else {
                ++writes.shared_images;
            }
        }
    }

    const auto audit = [&](DispatchPlan plan) {
        if (!WriteAudit() && !audit_region) {
            return;
        }
        static std::unordered_map<u64, u32> printed;
        if (audit_region || (printed.size() < 256 && printed[cs.pgm_hash]++ == 0)) {
            std::printf("Stereo: write audit: cs %016llx %s: writes %u twin targets, %u shared "
                        "buffers, %u GDS, %u shared storage images (reads twins: %s)\n",
                        static_cast<unsigned long long>(cs.pgm_hash),
                        plan == DispatchPlan::BothEyes   ? "both eyes"
                        : plan == DispatchPlan::EyeLOnly ? "eye L only (shared)"
                                                         : "once (reads no twin)",
                        writes.twin_targets, writes.shared_buffers, writes.shared_gds,
                        writes.shared_images, reads ? "yes" : "no");
        }
    };
    const auto run_once = [&](DispatchPlan plan) {
        audit(plan);
        // A dispatch that reads no twin computes the same for both eyes (its images are not
        // eye L's); one that reads twins and writes shared memory is eye L's by design
        // (exposure, particles): eye-R reads of its images count as shared reads, not leaks.
        for (const auto id : written.images) {
            if (plan == DispatchPlan::Once) {
                std::erase(region_written, id);
            } else {
                NoteRegionWrite(id, true);
            }
        }
        if (plan == DispatchPlan::Once) {
            ++compute_stats.once;
        } else {
            compute_stats.shared_buffers += writes.shared_buffers;
            compute_stats.shared_gds += writes.shared_gds;
            compute_stats.shared_images += writes.shared_images;
            compute_stats.guard_test += control.guard_test ? 1 : 0;
        }
        MarkStale(written);
        pending_subs.clear();
        capture_twins.reset();
    };
    const auto plan = PlanDispatch(reads, writes, control.guard_test);
    if (plan != DispatchPlan::BothEyes) {
        run_once(plan);
        return;
    }

    // Twin targets: storage images of the region's size, started from the guest's contents
    // (the dispatch may write part of them), as the attachments' twins are.
    for (const auto& target : targets) {
        auto& image = texture_cache.GetImage(target.id);
        const vk::Format format = target.view->vk_format;
        Twin& twin = EnsureTwin(instance, TwinKey(image, format), format,
                                image.info.size.width, image.info.size.height, false);
        SetSource(twin, target.id);
        const vk::ImageView twin_view = TwinViewFor(instance, twin, *target.view);
        if (!twin_view) {
            continue; // checked below: eye R may not write the guest's image
        }
        if (twin.source != target.id || twin.frame != twin_frame) {
            twin.frame = twin_frame;
            // The guest image is bound as storage: GENERAL (BindTextures).
            const auto to_dst = TwinBarrier(twin, vk::ImageLayout::eTransferDstOptimal);
            const auto back = TwinBarrier(twin, AttachmentLayout(twin));
            const vk::ImageCopy region{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .extent = {std::min(twin.width, image.info.size.width),
                           std::min(twin.height, image.info.size.height), 1},
            };
            scheduler.Record([to_dst, back, region, src = image.GetImage(),
                              dst = vk::Image(twin.image)](vk::CommandBuffer cmdbuf) {
                const vk::MemoryBarrier2 before{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                };
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,
                                         .pMemoryBarriers = &before,
                                         .imageMemoryBarrierCount = 1,
                                         .pImageMemoryBarriers = &to_dst});
                cmdbuf.copyImage(src, vk::ImageLayout::eGeneral, dst,
                                 vk::ImageLayout::eTransferDstOptimal, region);
                // The dispatch (eye L) then writes the guest image the copy read.
                const vk::MemoryBarrier2 after{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                    .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
                    .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .dstAccessMask =
                        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                };
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,
                                         .pMemoryBarriers = &after,
                                         .imageMemoryBarrierCount = 1,
                                         .pImageMemoryBarriers = &back});
            });
        }
        pending_subs.push_back({.guest_view = *target.view->image_view,
                                .twin_view = twin_view,
                                .twin_layout = vk::ImageLayout::eGeneral});
        capture->Use(&twin);
    }
    // The write guard for dispatches: eye R's execution writes only twins. Every written storage
    // image and buffer must be substituted, else the dispatch runs once (eye L only).
    bool guarded = true;
    for (u32 i = 0; indexed && i < cs.images.size(); ++i) {
        const auto& write = set_writes[cs.buffers.size() + i];
        for (u32 k = 0; cs.images[i].is_written && write.pImageInfo && k < write.descriptorCount;
             ++k) {
            const vk::ImageView handle = write.pImageInfo[k].imageView;
            guarded &= !handle || substituted_view(handle);
        }
    }
    for (u32 i = 0; indexed && i < cs.buffers.size(); ++i) {
        const auto& desc = cs.buffers[i];
        if (desc.is_written && !desc.IsSpecial()) {
            const auto* info = set_writes[i].pBufferInfo;
            guarded &= !info || !info->buffer || substituted_buffer(*info);
        }
    }
    if (!guarded || !indexed) {
        ++writes.shared_images; // (the twins just started for eye R go stale with the rest)
        run_once(DispatchPlan::EyeLOnly);
        return;
    }

    audit(DispatchPlan::BothEyes);
    for (const auto id : written.images) {
        NoteRegionWrite(id);
    }
    const u32 twin_targets = writes.twin_targets;
    const bool begun = scheduler.BeginComputeCapture(
        std::make_shared<RecordArena>(),
        [this, capture, twin_targets, written](std::shared_ptr<RecordArena> arena,
                                               bool complete) {
            const BbStats::PerfScope perf{BbStats::PerfStereoEyeR};
            if (capture_twins == capture) {
                capture_twins.reset();
            }
            if (!complete) {
                ++compute_stats.cut_short;
                MarkStale(written); // eye L's execution only
                return;
            }
            ++compute_stats.both_eyes;
            compute_stats.twin_targets += twin_targets;
            auto* profiler = GpuProfiler::Get();
            const u64 resume = profiler ? profiler->Current() : 0;
            if (profiler) {
                profiler->Mark(ComputeProfileKey, [] {
                    return std::string{"stereo: eye R of doubled dispatches"};
                });
            }
            PrepareEyeR(scheduler, capture->used, {});
            scheduler.Record([arena = std::move(arena)](vk::CommandBuffer cmdbuf) {
                // Eye R's writes (twins, twin buffers) after eye L's, and visible to what follows.
                const vk::MemoryBarrier2 barrier{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .dstAccessMask =
                        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                };
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
                arena->Execute(cmdbuf, true);
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
            });
            if (profiler && resume) {
                profiler->Resume(resume);
            }
        });
    if (!begun) {
        pending_subs.clear();
        capture_twins.reset();
        MarkStale(written);
        ++compute_stats.cut_short; // not deferred recording: eye R reads eye L's results
    }
}

} // namespace Vulkan
