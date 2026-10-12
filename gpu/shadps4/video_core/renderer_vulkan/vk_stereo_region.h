// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: VR Phase 4 true stereo, full stereo (docs/vr/findings/31-true-stereo.md section 10).
// The doubled region runs from the G-buffer to the display pass: every render pass and dispatch of
// the 3D region is recorded once and executed for eye L (guest images) and eye R (twins), the UI
// (Scaleform) runs once, and the display pass runs again for eye R into a twin of the display
// buffer: eye R's final image. Header-only and device-free (tests/test_stereo_substitute.cpp).

#pragma once

#include "common/types.h"

namespace Vulkan::Stereo {

/// Which draws of a frame are doubled in full stereo. Fed every draw in order.
///
///   Idle    --G-buffer draw (full allowed)-->  Scene   (doubled; the region starts)
///   Scene   --first Scaleform draw------->     Ui      (the UI: eye L only)
///   Scene/Ui --the display pass----------->    Display (its draws doubled: eye R's final image)
///   Display --any other draw, or the next display pass--> finish, then Idle
class FullRegion {
public:
    enum class Phase { Idle, Scene, Ui, Display };

    struct Step {
        bool finish = false;  ///< before this draw: the display pass is over (eye R final done)
        bool start = false;   ///< this draw starts a region (its frame's twins)
        bool doubled = false; ///< this draw runs for both eyes
    };

    /// `boundary`: the first draw of a display pass (a new frame); `display`: the draw writes a
    /// display buffer; `gbuffer`: a G-buffer draw; `movie`: a Scaleform draw; `allowed`: full
    /// stereo may start a region now (stereo on, hybrid frame, no upscaler, ...).
    Step OnDraw(bool boundary, bool display, bool gbuffer, bool movie, bool allowed) {
        Step step;
        if (phase == Phase::Display && (!display || boundary)) {
            step.finish = true;
            phase = Phase::Idle;
        }
        if (boundary && (phase == Phase::Scene || phase == Phase::Ui)) {
            phase = Phase::Display; // the region's frame ends with this display pass
        } else if (phase == Phase::Idle && gbuffer && allowed && !display) {
            phase = Phase::Scene;
            step.start = true;
        } else if (phase == Phase::Scene && movie) {
            phase = Phase::Ui;
        }
        step.doubled = phase == Phase::Scene || (phase == Phase::Display && display);
        return step;
    }

    /// Stereo or full stereo switched off mid-region: forget it (the caller ends its captures).
    void Reset() {
        phase = Phase::Idle;
    }

    [[nodiscard]] Phase Current() const {
        return phase;
    }
    [[nodiscard]] bool Active() const {
        return phase != Phase::Idle;
    }

private:
    Phase phase = Phase::Idle;
};

/// What eye R may read through a twin (its `tag`: the region frame whose eye R it holds).
enum class TwinUse {
    None,    ///< read the guest image (eye L's, or shared)
    Current, ///< eye R wrote it in this region
    History, ///< eye R wrote it in the previous region and nothing wrote the guest since: an
             ///< image read before it is written (temporal history) keeps eye R's own
};

constexpr u64 NoFrame = ~0ull;

constexpr TwinUse ClassifyTwin(u64 tag, u64 region_frame, bool full) {
    if (tag == NoFrame) {
        return TwinUse::None;
    }
    if (tag == region_frame) {
        return TwinUse::Current;
    }
    return full && region_frame > 0 && tag + 1 == region_frame ? TwinUse::History
                                                                : TwinUse::None;
}

/// A dispatch's storage image gets a twin (eye R writes the twin) when it is a plain 2D color
/// image of at least 64x64, one mip and layer viewed, in a format usable as storage and color
/// attachment. Smaller and other images (exposure 2048x2, the 3D LUT) are shared: eye L only.
constexpr bool StorageTwinnable(u32 width, u32 height, u32 view_levels, u32 view_layers,
                                u32 base_level, u32 base_layer, bool view_2d, bool color,
                                bool samples_1, bool format_ok) {
    return width >= 64 && height >= 64 && view_levels == 1 && view_layers == 1 &&
           base_level == 0 && base_layer == 0 && view_2d && color && samples_1 && format_ok;
}

/// A render target eye R can render into through its twin (the twin is the image's mip 0,
/// layer 0, single-sampled).
constexpr bool AttachmentTwinnable(u32 base_level, u32 base_layer, u32 layers, u32 samples) {
    return base_level == 0 && base_layer == 0 && layers == 1 && samples == 1;
}

} // namespace Vulkan::Stereo
