// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: VR Phase 4 prototype, renderer-side stereo (docs/vr/findings/23-phase4-renderer-stereo.md).
// Off unless BB_XR_MODE=stereo (or BB_STEREO=1); then perspective vertex shaders read a per-draw
// clip x offset (runtime_info.hw.vs.stereo_remap, ir/passes/lower_stereo_remap.cpp) and the
// rasterizer (Rasterizer::Stereo* in vk_stereo.cpp) chooses it per draw:
//   - 0 outside the stereo region (shadows, UI) and by default inside it;
//   - "offset=<m>" (BB_STEREO_CONTROL file): the whole region rendered from the center camera
//     moved <m> metres right, the oracle check against a CPU camera moved the same way (H1);
//   - "dup=<n>" (a new n): the next frame's G-buffer pass is drawn a second time, into twin
//     images with the other eye's offset, and both eyes' targets are dumped (BB_DUMP_DIR).
//     P4.3 (default, "replay=1"): each render pass of it is recorded once and executed twice
//     (Scheduler::BeginCapture), the second time into the twins with eye R's offset;
//     "replay=0" keeps the prototype's per-draw second draw. "same=1" puts eye R at eye L
//     (the replay must then reproduce eye L bit for bit).

#pragma once

#include <array>
#include <optional>
#include <utility>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::Stereo {

/// Perspective vertex shaders get the per-eye remap (BB_XR_MODE=stereo or BB_STEREO=1).
bool RemapShaders();

/// The StereoView buffer of the draw being recorded: clip x offset, 0, 0, 0.
std::array<float, 4> CurrentView();

/// BindBuffers: for a draw of a pass that is recorded once and executed for both eyes (P4.3),
/// the buffer and offset its StereoView binding reads (rewritten between the eyes); else none.
std::optional<std::pair<vk::Buffer, u64>> EyeSlot();

/// BindBuffers: the descriptor write index of this draw's StereoView binding.
void NoteViewWrite(u32 write_index);

/// BindBuffers: an 864-byte scene constant block (the main camera when far == 3000).
void OnSceneConstants(const float* data);

} // namespace Vulkan::Stereo
