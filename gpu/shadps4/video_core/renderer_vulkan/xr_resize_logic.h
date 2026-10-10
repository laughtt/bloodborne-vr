// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR: runtime resize bookkeeping of the presenter (frame size, eye / flat / HUD swapchains).
// Header only, no Vulkan, so tests/test_xr_resize.cpp checks it headless.
//
// Two crashes (device lost, 2026-10-10) came from the frame size flipping for one frame:
// the present thread wrote the window size and then the eye IQ size into the frame size the GPU
// thread reads, so the GPU thread now and then caught the window size in between. Each flip
// recreated the eye swapchain (xrDestroySwapchain) and the per-eye DLSS feature twice while the
// GPU still wrote into them. The presenter now computes the size once, publishes it whole, only
// after it held for a few presents (SizeDebounce), and waits for the GPU before a swapchain it
// wrote is recreated (ChainSize).

#pragma once

#include <cstdint>

namespace XrResize {

struct Size {
    uint32_t width = 0, height = 0;
    [[nodiscard]] bool Valid() const {
        return width != 0 && height != 0;
    }
    friend bool operator==(const Size& a, const Size& b) {
        return a.width == b.width && a.height == b.height;
    }
    friend bool operator!=(const Size& a, const Size& b) {
        return !(a == b);
    }
};

[[nodiscard]] inline uint64_t Pack(Size s) {
    return (uint64_t(s.width) << 32) | s.height;
}
[[nodiscard]] inline Size Unpack(uint64_t v) {
    return {uint32_t(v >> 32), uint32_t(v)};
}

/// What the frame size is made of (the present thread computes it per present).
struct Inputs {
    Size window; ///< the window (swapchain) extent, aspect-fitted to the game image
    Size iq;     ///< the eye IQ target (BB_XR_UPSCALE / BB_XR_EYE_RES / dlss_quality), 0 if none
    Size source; ///< the display buffer the eye IQ scales from
};

/// The frame size these inputs ask for: the eye IQ target when there is one, else the window.
[[nodiscard]] inline Size Target(const Inputs& in) {
    return in.iq.Valid() ? in.iq : in.window;
}

/// Why the target moved from `before` to `after` (for the resize log line).
[[nodiscard]] inline const char* ChangeReason(const Inputs& before, const Inputs& after) {
    const bool had_iq = before.iq.Valid(), has_iq = after.iq.Valid();
    if (had_iq != has_iq) {
        return has_iq ? "eye IQ upscale switched on" : "eye IQ upscale switched off";
    }
    if (has_iq) {
        if (before.source != after.source) {
            return "display buffer size";
        }
        return "eye IQ settings (res / dlss_quality)";
    }
    return "window size";
}

/// Debounces the frame size: a new target applies once it was the same for `stable` consecutive
/// updates (one-frame flips never reach the frame images and swapchains); the first valid target
/// applies at once. Invalid targets (minimized window) are ignored.
class SizeDebounce {
public:
    explicit SizeDebounce(uint32_t stable = 3) : stable_{stable ? stable : 1} {}

    /// True when this update changed the applied size.
    bool Update(Size target) {
        if (!target.Valid()) {
            return false;
        }
        if (!applied_.Valid()) {
            applied_ = target;
            pending_ = target;
            count_ = 0;
            return true;
        }
        if (target == applied_) {
            pending_ = applied_;
            count_ = 0;
            return false;
        }
        if (target != pending_) {
            pending_ = target;
            count_ = 1;
        } else {
            ++count_;
        }
        if (count_ >= stable_) {
            applied_ = target;
            count_ = 0;
            return true;
        }
        return false;
    }
    [[nodiscard]] Size Applied() const {
        return applied_;
    }
    /// Updates a different target was held back (seen, not applied yet).
    [[nodiscard]] uint32_t Pending() const {
        return count_;
    }

private:
    uint32_t stable_;
    Size applied_{}, pending_{};
    uint32_t count_ = 0;
};

/// The size of a swapchain the presenter writes into (eye L / R, flat, HUD). The bridge destroys
/// and recreates a swapchain whose size changes inside its Acquire: earlier copies into the old
/// images must be finished by then.
class ChainSize {
public:
    /// True when acquiring at `size` recreates a swapchain this side wrote before.
    [[nodiscard]] bool NeedsWait(Size size) const {
        return last_.Valid() && size.Valid() && size != last_;
    }
    /// After an acquire at `size` returned an image (the swapchain has that size now).
    void Acquired(Size size) {
        last_ = size;
    }
    [[nodiscard]] Size Last() const {
        return last_;
    }

private:
    Size last_{};
};

} // namespace XrResize
