// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: VR Phase 4 research (docs/vr/findings/23-phase4-renderer-stereo.md). With
// BB_STEREO_CENSUS=1, the frame that BB_CAPTURE_TRIGGER records (vk_frame_capture.h) is also
// written draw by draw as JSON lines (BB_STEREO_CENSUS_DIR, else BB_CAPTURE_DIR): every draw
// and dispatch with its stage hashes, targets, sampled images and bound buffers (address, size,
// binding, constant-ring or V#, and the bytes of small buffers). It answers which passes read
// the camera, from which buffers, so per-eye stereo knows what to patch or replay.
// Called on the thread recording the draw, only while FrameCapture::Active().

#pragma once

#include "common/types.h"

namespace AmdGpu {
union Regs;
}
namespace Shader {
struct Info;
}
namespace VideoCore {
struct ImageInfo;
}

namespace Vulkan {
class GraphicsPipeline;
}

namespace Vulkan::StereoCensus {

/// BB_STEREO_CENSUS=1.
bool Enabled();

/// A buffer bound for the next draw or dispatch. `data` points to the bytes the GPU reads
/// (the constant-ring snapshot for ring bindings), or null.
void Buffer(const Shader::Info& stage, u32 slot, u32 binding, VAddr address, const void* data,
            u64 size, bool ring, bool written);

/// A sampled or storage image of the next draw or dispatch.
void Image(const VideoCore::ImageInfo& info, bool storage);

/// The render targets of the next draws; a pass into a display buffer starts and ends frames.
void Targets(const VideoCore::ImageInfo* const* colors, u32 num_colors,
             const VideoCore::ImageInfo* depth, bool display);

/// The draw just recorded: stage hashes, counts, clip state, viewport 0 and the targets.
/// `scene_depth`: its depth target is the scene depth (CameraMotion::Depth).
void Draw(const GraphicsPipeline& pipeline, const AmdGpu::Regs& regs, bool indexed,
          u32 num_indices, u32 num_instances, bool gbuffer, bool scene_depth);
void Dispatch(u64 cs_hash, u32 x, u32 y, u32 z);

} // namespace Vulkan::StereoCensus
