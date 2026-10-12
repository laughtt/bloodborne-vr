// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: VR Phase 4 prototype, renderer-side stereo (docs/vr/findings/23-phase4-renderer-stereo.md).
// Off unless BB_XR_MODE=stereo (or BB_STEREO=1); then perspective vertex shaders read a per-draw
// clip x offset (runtime_info.hw.vs.stereo_remap, ir/passes/lower_stereo_remap.cpp) and the
// rasterizer (Rasterizer::Stereo* in vk_stereo.cpp) chooses it per draw:
//   - 0 outside the stereo region (shadows, UI), and inside it with hybrid stereo off;
//   - "offset=<m>" (BB_STEREO_CONTROL file): the whole region rendered from the center camera
//     moved <m> metres right, the oracle check against a CPU camera moved the same way (H1);
//   - "dup=<n>" (a new n): the next frame's G-buffer pass is drawn a second time, into twin
//     images with the other eye's offset, and both eyes' targets are dumped (BB_DUMP_DIR).
//     P4.3 (default, "replay=1"): each render pass of it is recorded once and executed twice
//     (Scheduler::BeginCapture), the second time into the twins with eye R's offset;
//     "replay=0" keeps the prototype's per-draw second draw. "same=1" puts eye R at eye L
//     (the replay must then reproduce eye L bit for bit).
//   - hybrid stereo (the default in stereo mode, below): every frame's G-buffer doubled, the
//     whole region at eye L, eye R's final image warped from the display buffer at the flip.

#pragma once

#include <array>
#include <optional>
#include <span>
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

/// The values OnSceneConstants takes from a block (sx, zs, zo); false when the block is not the
/// main camera's. Stage A parses the ring's 864-byte blocks while their bytes are in its cache
/// (stereo-perf: stage B reading them back from the write-combined ring cost ~30% of its time),
/// stage B applies them in draw order (SetSceneConstants).
bool ParseSceneConstants(const float* data, std::array<float, 3>& out);
void SetSceneConstants(const std::array<float, 3>& values);

// Hybrid stereo (docs/vr/findings/31-true-stereo.md, "Hybrid stereo warp"), on by default in
// stereo mode (BB_STEREO_HYBRID=0 or "hybrid=0" in the control file turn it off): every frame's
// G-buffer is drawn for both eyes (eye L at -ipd/2 into the guest targets, eye R at +ipd/2
// into twins), the rest of the 3D region runs once for eye L (at -ipd/2), and at the frame's
// flip (Rasterizer::StereoWarpAtFlip) a compute pass builds eye R's final image from the display
// buffer (eye L's finished frame, HUD-free with the HUD layer), eye L's depth and eye R's own
// depth (host_shaders/stereo_warp.comp); disoccluded pixels get eye R's own albedo times the
// background's lighting (G-buffer-guided hole fill, BB_STEREO_GUIDED=0 / "guided=0" turn it off).

/// Eye R's final image of one frame.
struct EyeRImage {
    vk::Image image;
    vk::ImageView view;    ///< `format`, for sampling
    vk::Format format{};   ///< eye L's view format of the display buffer (the bytes are its own)
    u32 width = 0, height = 0;
    vk::ImageLayout layout = vk::ImageLayout::eGeneral; ///< always; a consumer leaves it so
    u64 frame = 0;         ///< the renderer frame (Frame()) it belongs to
    VAddr display_buffer = 0; ///< the display buffer that frame's display pass wrote (0: not yet)
    // For eye R's own DLSS (findings/31 section 9.8): the eyes' offsets along the camera's x axis
    // (metres; the remap draws clip.x - P00 * m) and eye R's G-buffer depth twin, in eGeneral
    // (a consumer leaves it so). The twin holds `frame` only until the next frame's G-buffer
    // pass: use it while Frame() == frame + 1.
    float eye_l = 0.0f, eye_r = 0.0f;
    vk::Image depth_image;
    vk::ImageView depth_view; ///< depth aspect
    vk::Format depth_format{};
    u32 depth_width = 0, depth_height = 0;
    /// Full stereo rendered it (its own image); false: warped from eye L's (the hybrid).
    bool rendered = false;
};

/// The renderer's frame counter: incremented at each display pass (the frame's last pass).
u64 Frame();

/// Eye R's final image of renderer frame `frame`, or none when that frame produced none (stereo
/// or hybrid off, no 3D scene, no eye-R depth). Written by a compute shader on the graphics
/// queue at the frame's flip and made visible to all later commands of the queue
/// (a consumer recorded or submitted after that frame needs no further barrier than its own
/// layout-preserving read). The image is reused three frames later (a ring of 3).
std::optional<EyeRImage> EyeRFinal(u64 frame);

/// The same for the newest frame whose display pass wrote `display_buffer` (the guest address
/// of the buffer being flipped), so a presenter can pair a flip with its eye R.
std::optional<EyeRImage> EyeRForDisplay(VAddr display_buffer);

/// Eye distance from the runtime (metres; <= 0: back to BB_STEREO_IPD, default 0.063). An
/// "ipd=" in the control file overrides both (tests).
void SetIpd(float metres);

// P4.4 slice 3 (vk_stereo_substitute.h): eye R's descriptors in captured commands.
struct DescriptorSubstitute;

/// Pipeline::BindResources, while the scheduler captures: the (guest -> twin) pairs of the draw or
/// dispatch being bound (empty when it reads no twin). Valid until the next call; consumed once.
std::span<const DescriptorSubstitute> TakeSubstitutes();

} // namespace Vulkan::Stereo
