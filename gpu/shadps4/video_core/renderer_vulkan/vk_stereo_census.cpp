// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_stereo_census.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <string>
#include <vector>

#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/texture_cache/image_info.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Vulkan::StereoCensus {

namespace {

// Bytes kept per buffer: whole small buffers (constants), the head of large ones.
constexpr u64 FullBytes = 4096;
constexpr u64 HeadBytes = 256;

enum class State { Idle, Recording };
State state = State::Idle;
bool previous_display = false; ///< the last targets were a display buffer (same pass)
u32 frames_seen = 0;
std::string pending; ///< resources of the next draw or dispatch (JSON fragments)
std::string targets; ///< the current targets (JSON object)
std::vector<std::string> lines;
u32 draw_index = 0;

std::string Hex(const void* data, u64 size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    const auto* bytes = static_cast<const u8*>(data);
    for (u64 i = 0; i < size; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 15]);
    }
    return out;
}

std::string Describe(const VideoCore::ImageInfo& info) {
    return std::format("[\"{:#x}\",\"{}\",{},{},{}]", info.guest_address,
                       vk::to_string(info.pixel_format), info.size.width, info.size.height,
                       info.resources.levels);
}

void Write() {
    const char* dir = std::getenv("BB_STEREO_CENSUS_DIR");
    if (!dir) {
        dir = std::getenv("BB_CAPTURE_DIR");
    }
    const std::string path = std::format("{}/census_{}.jsonl", dir ? dir : ".",
                                         static_cast<long long>(std::time(nullptr)));
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::printf("Stereo census: cannot write %s\n", path.c_str());
        return;
    }
    for (const auto& line : lines) {
        std::fputs(line.c_str(), f);
        std::fputc('\n', f);
    }
    std::fclose(f);
    std::printf("Stereo census: %zu draws and dispatches written to %s\n", lines.size(),
                path.c_str());
}

void Emit(std::string record) {
    if (state != State::Recording) {
        pending.clear();
        return;
    }
    record += ",\"res\":[";
    if (!pending.empty()) {
        pending.pop_back(); // trailing comma
        record += pending;
    }
    record += "]}";
    lines.push_back(std::move(record));
    pending.clear();
}

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_STEREO_CENSUS");
        return value && value[0] == '1';
    }();
    return enabled;
}

#ifdef _WIN32
/// BB_STEREO_CENSUS_SCAN=1: the other copies of the main camera's scene constants in the
/// process (the game's CPU-side source of the per-pass constant copies), once per census.
void ScanForCopies(const void* data, VAddr bound_at) {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_STEREO_CENSUS_SCAN");
        return value && value[0] == '1';
    }();
    static u32 scanned_frame = ~0u;
    if (!enabled || scanned_frame == frames_seen) {
        return;
    }
    scanned_frame = frames_seen;
    // The camera part: far, 1/far, screen size and the view matrix (floats 0..19).
    const auto* needle = static_cast<const u8*>(data);
    constexpr size_t NeedleBytes = 80;
    std::string found;
    u32 count = 0;
    // Guest memory only (the eboot image is mapped at 0x800000000, its heaps above it).
    u64 first8 = 0;
    std::memcpy(&first8, needle, sizeof(first8));
    MEMORY_BASIC_INFORMATION mbi{};
    for (u8* at = reinterpret_cast<u8*>(0x800000000ull);
         reinterpret_cast<uintptr_t>(at) < 0x1400000000ull &&
         VirtualQuery(at, &mbi, sizeof(mbi)) == sizeof(mbi);
         at = static_cast<u8*>(mbi.BaseAddress) + mbi.RegionSize) {
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
        if (mbi.State != MEM_COMMIT || !(mbi.Protect & readable) || (mbi.Protect & PAGE_GUARD)) {
            continue;
        }
        const u8* begin = static_cast<const u8*>(mbi.BaseAddress);
        const u8* end = begin + mbi.RegionSize;
        for (const u8* p = begin; p + NeedleBytes <= end; p += 16) {
            u64 head;
            std::memcpy(&head, p, sizeof(head));
            if (head == first8 && std::memcmp(p, needle, NeedleBytes) == 0 && p != needle) {
                ++count;
                if (count <= 64) {
                    found += std::format("{}\"{:#x}\"", found.empty() ? "" : ",",
                                         reinterpret_cast<uintptr_t>(p));
                }
            }
        }
    }
    lines.push_back(std::format("{{\"t\":\"scan\",\"bound_at\":\"{:#x}\",\"copies\":{},"
                                "\"at\":[{}],\"res\":[]}}",
                                bound_at, count, found));
    std::printf("Stereo census: %u copies of the camera constants in memory\n", count);
}
#endif

void Buffer(const Shader::Info& stage, u32 slot, u32 binding, VAddr address, const void* data,
            u64 size, bool ring, bool written) {
    if (!Enabled() || state != State::Recording) {
        return;
    }
#ifdef _WIN32
    if (data && size == 864 && std::abs(static_cast<const float*>(data)[0] - 3000.0f) <= 100.0f) {
        ScanForCopies(data, address);
    }
#endif
    const u64 kept = data ? (size <= FullBytes ? size : HeadBytes) : 0;
    pending += std::format("{{\"t\":\"buf\",\"stage\":{},\"hash\":\"{:016x}\",\"slot\":{},"
                           "\"binding\":{},\"addr\":\"{:#x}\",\"size\":{},\"ring\":{},"
                           "\"written\":{},\"data\":\"{}\"}},",
                           u32(stage.sw_stage), stage.pgm_hash, slot, binding, address, size,
                           ring ? 1 : 0, written ? 1 : 0, kept ? Hex(data, kept) : "");
}

void Image(const VideoCore::ImageInfo& info, bool storage) {
    if (!Enabled() || state != State::Recording) {
        return;
    }
    pending += std::format("{{\"t\":\"img\",\"storage\":{},\"img\":{}}},", storage ? 1 : 0,
                           Describe(info));
}

void Targets(const VideoCore::ImageInfo* const* colors, u32 num_colors,
             const VideoCore::ImageInfo* depth, bool display) {
    if (!Enabled()) {
        return;
    }
    // Called for every draw: only the first draw into a display buffer after other targets
    // opens a frame (FrameCapture uses the same boundary).
    const bool boundary = display && !previous_display;
    previous_display = display;
    if (boundary) {
        if (state == State::Recording) {
            Write();
            lines.clear();
            state = State::Idle;
            ++frames_seen;
        } else if (state == State::Idle) {
            state = State::Recording;
            draw_index = 0;
            lines.clear();
        }
    }
    targets = "{\"colors\":[";
    for (u32 i = 0; i < num_colors; ++i) {
        targets += colors[i] ? Describe(*colors[i]) : "null";
        if (i + 1 < num_colors) {
            targets += ",";
        }
    }
    targets += "],\"depth\":";
    targets += depth ? Describe(*depth) : "null";
    targets += std::format(",\"display\":{}}}", display ? 1 : 0);
}

void Draw(const GraphicsPipeline& pipeline, const AmdGpu::Regs& regs, bool indexed,
          u32 num_indices, u32 num_instances, bool gbuffer, bool scene_depth) {
    if (!Enabled()) {
        return;
    }
    if (state != State::Recording) {
        pending.clear();
        return;
    }
    // Shader::SwStage order: fragment, tess control, tess eval, vertex, geometry.
    u64 hashes[5]{};
    for (u32 s = 0; s < 5; ++s) {
        const auto* stage = pipeline.GetStages()[s];
        hashes[s] = stage ? stage->pgm_hash : 0;
    }
    const auto& vp = regs.viewports[0];
    Emit(std::format("{{\"t\":\"draw\",\"i\":{},\"fs\":\"{:016x}\",\"tcs\":\"{:016x}\","
                     "\"tes\":\"{:016x}\",\"vs\":\"{:016x}\",\"gs\":\"{:016x}\",\"indices\":{},"
                     "\"instances\":{},\"indexed\":{},\"clip_disable\":{},\"gbuffer\":{},"
                     "\"scene_depth\":{},\"prim\":{},\"viewport\":[{},{},{},{}],\"targets\":{}",
                     draw_index++, hashes[0], hashes[1], hashes[2], hashes[3], hashes[4],
                     num_indices, num_instances, indexed ? 1 : 0, regs.IsClipDisabled() ? 1 : 0,
                     gbuffer ? 1 : 0, scene_depth ? 1 : 0, u32(regs.primitive_type), vp.xscale,
                     vp.xoffset, vp.yscale, vp.yoffset, targets.empty() ? "null" : targets));
}

void Dispatch(u64 cs_hash, u32 x, u32 y, u32 z) {
    if (!Enabled()) {
        return;
    }
    if (state != State::Recording) {
        pending.clear();
        return;
    }
    Emit(std::format("{{\"t\":\"dispatch\",\"i\":{},\"cs\":\"{:016x}\",\"groups\":[{},{},{}]",
                     draw_index++, cs_hash, x, y, z));
}

} // namespace Vulkan::StereoCensus
