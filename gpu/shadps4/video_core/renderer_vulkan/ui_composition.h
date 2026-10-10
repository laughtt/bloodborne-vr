// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace Vulkan::UiComposition {

enum class Background { None, Copy, Temporal };

// UI composition must not require scene depth/camera or an active FSR context.
constexpr Background Choose(bool scaled, bool ui_draw, bool scene_ready, bool fsr_active) {
    if (!scaled || !ui_draw) {
        return Background::None;
    }
    return scene_ready && fsr_active ? Background::Temporal : Background::Copy;
}

inline bool NativeViewport(float width, float height) {
    return std::abs(std::abs(width) - 1920.0f) < 0.5f &&
           std::abs(std::abs(height) - 1080.0f) < 0.5f;
}

// Scaleform draws, including the first stencil/movie pass. A native-size viewport
// alone also matches every fullscreen post pass when guest targets stay at 1080p.
constexpr bool MovieShader(uint64_t hash) {
    return hash == 0x34e8a281 || hash == 0x81d336ce || hash == 0x09957251 ||
           hash == 0x24042a9b || hash == 0xa400228b;
}

inline std::array<float, 2> Scale(uint32_t guest_width, uint32_t guest_height,
                                uint32_t output_width, uint32_t output_height,
                                bool native_coordinates) {
    return {float(output_width) / float(native_coordinates ? 1920 : guest_width),
            float(output_height) / float(native_coordinates ? 1080 : guest_height)};
}

// bbport VR HUD layer (vk_xr.h XrHud, docs/vr/findings/25): the UI is drawn over transparent
// black. Scaleform blends alpha with the color factors (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), which
// leaves A = As^2 over an empty target; the HUD needs coverage, A = 1 - prod(1 - As). Factor and
// function values are AmdGpu::BlendControl's (Zero 0, One 1, OneMinusSrcAlpha 5; Add 0).
struct AlphaBlend {
    uint32_t src, func, dst;
};
constexpr uint32_t kZero = 0, kOne = 1, kOneMinusSrcAlpha = 5, kAdd = 0;

/// The alpha blend a UI draw gets in HUD mode, from its color blend: "over" (color dst
/// ONE_MINUS_SRC_ALPHA) accumulates coverage, additive (color dst ONE) leaves alpha alone; other
/// blends keep their own (false).
constexpr bool HudAlpha(uint32_t color_dst, uint32_t color_func, AlphaBlend* out) {
    if (color_func != kAdd) {
        return false;
    }
    if (color_dst == kOneMinusSrcAlpha) {
        *out = {kOne, kAdd, kOneMinusSrcAlpha};
        return true;
    }
    if (color_dst == kOne) {
        *out = {kZero, kAdd, kOne};
        return true;
    }
    return false;
}

/// A separated UI that covers (nearly) the whole screen: in-game loading screens and full-screen
/// menus, which go to the flat cinema screen instead of the HUD quad. `rgba`: probe texels (RGBA8)
/// of the UI image; covered when at least 90% of them have alpha >= 0.9.
inline bool FullScreenUi(const uint8_t* rgba, uint32_t texels) {
    if (!texels) {
        return false;
    }
    uint32_t covered = 0;
    for (uint32_t i = 0; i < texels; ++i) {
        covered += rgba[i * 4 + 3] >= 230 ? 1u : 0u;
    }
    return covered * 10 >= texels * 9;
}

/// Hysteresis for FullScreenUi: on after 2 covered measurements in a row, off after 4 uncovered
/// (a Scaleform movie may skip a frame; fades in and out of loading screens are partial).
struct FullScreenLatch {
    bool on = false;
    int run = 0;
    bool Update(bool covered) {
        if (covered == on) {
            run = 0;
        } else if (++run >= (on ? 4 : 2)) {
            on = covered;
            run = 0;
        }
        return on;
    }
};

} // namespace Vulkan::UiComposition
