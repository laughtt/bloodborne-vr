// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport VR true stereo (BB_XR_MODE=stereo, docs/vr/findings/31-true-stereo.md "P4.6a hand-off"):
// the hand-off of eye R's final image from the stereo renderer (vk_stereo.cpp) to the presenter.
//
// The presenter copies eye L's finished frame image and eye R's image into the two eye
// swapchains of the headset bridge and releases them as one pair. Until the renderer produces a
// complete eye R it registers no provider, or returns nothing for a frame, and the presenter shows
// eye L's image in both eyes (logged, counted in the 10 s "VR stereo:" line).
//
// Contract of a provider (called on the GPU thread inside Presenter::PrepareFrame, after the
// frame's image was finished, so its commands are on the same draw scheduler):
//   - The returned image is the FINAL eye R picture, at the same stage as Frame::image: scene,
//     upscaled/anti-aliased as eye L, HUD-free (the HUD is its own layer), width x height equal to
//     the request's, and in the request's format (or the same channel order: the presenter blits).
//   - Its commands are recorded into the draw scheduler before the call returns, so the frame's
//     ready semaphore (what Present waits on) covers them.
//   - It stays valid and unchanged in `layout` until the frame's present is done: Request::slot is
//     the presenter frame (0 .. a few) to ring-buffer by; the next call with the same slot comes
//     only after that frame's present_done fence signaled.
// Returning nothing for a frame (renderer window missed, not ready) is allowed at any time.

#pragma once

#include <atomic>
#include <optional>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::StereoEyeR {

struct Request {
    u8 slot;           // the presenter frame (Frame::id)
    u32 width, height; // the frame image's size
    vk::Format format; // the frame image's format
    vk::Image eye_l;   // eye L's final image (Frame::image), e.g. for an identity copy
    u64 view_seq;      // the render view of this frame (BbVrRenderView.seq), 0 unknown
    u64 display_buffer; // guest address of the display buffer being flipped (B: EyeRForDisplay)
};

struct Image {
    vk::Image image;
    vk::ImageLayout layout; // the layout it is in; the presenter restores it after the copy
};

/// The renderer's side: a function the presenter calls once per paired frame.
using Provider = std::optional<Image> (*)(const Request& request);

inline std::atomic<Provider>& ProviderSlot() {
    static std::atomic<Provider> slot{nullptr};
    return slot;
}

/// vk_stereo.cpp registers its provider once (any thread); null unregisters.
inline void SetProvider(Provider provider) {
    ProviderSlot().store(provider, std::memory_order_release);
}

/// The presenter's side: eye R's image for this frame, or nothing (eye L's image is used).
inline std::optional<Image> Take(const Request& request) {
    const Provider provider = ProviderSlot().load(std::memory_order_acquire);
    return provider ? provider(request) : std::nullopt;
}

} // namespace Vulkan::StereoEyeR
