// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The VR UI in the desktop window (BB_XR_MIRROR_UI, docs/vr/findings/26-game-ui.md). The headset
// shows the menus as a projection layer only the runtime composites, so a screen recording of the
// window misses them. The bridge draws the same UI geometry once more, from the view the flat
// (mono) desktop image was rendered with, into an image of its own; the host blends that image
// over the game image in the window. This header is the math of that third view and of where it
// lands in the window; pure, no Vulkan, tested in vr_game_ui_test.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "sandbox3d/math3d.h"

namespace vrmirror {

struct Fov {
    float left, right, up, down; // radians from the view axis; left and down are negative
};

/// The desktop image's camera: the head pose and frustum it was rendered with, as the UI's
/// view-projection (Vulkan clip space, the same convention the eye layers use).
inline m3::Mat4 ViewProj(const m3::Quat& orientation, const m3::Vec3& position, const Fov& fov) {
    return m3::ProjectionFov(fov.left, fov.right, fov.up, fov.down, 0.05f, 0.0f) *
           m3::Inverse(m3::FromPose(orientation, position));
}

/// The pixel size of the mirror image for a frustum, square pixels, inside max_w x max_h.
inline void ImageSize(const Fov& fov, uint32_t max_w, uint32_t max_h, uint32_t* w, uint32_t* h) {
    const float aspect = (std::tan(fov.right) - std::tan(fov.left)) /
                         (std::tan(fov.up) - std::tan(fov.down));
    float width = float(max_h) * aspect, height = float(max_h);
    if (width > float(max_w)) {
        width = float(max_w);
        height = float(max_w) / aspect;
    }
    *w = std::max(2u, uint32_t(width + 0.5f));
    *h = std::max(2u, uint32_t(height + 0.5f));
}

/// A world point in the mirror image's pixels (y down, origin top left); false when it is
/// behind the camera. The quad corners of a menu go through this: the same pixels the mirror
/// pass rasterizes, and what a recording shows.
inline bool ProjectToPixel(const m3::Mat4& view_proj, const m3::Vec3& p, uint32_t w, uint32_t h,
                           float* x, float* y) {
    const float* m = view_proj.m;
    const float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
    const float cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    const float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (cw <= 1e-5f) {
        return false;
    }
    *x = (cx / cw * 0.5f + 0.5f) * float(w);
    *y = (cy / cw * 0.5f + 0.5f) * float(h);
    return true;
}

/// Where a frame of fw x fh lands in a window of ww x wh (centered, aspect kept): the rectangle
/// the mirror image is drawn into.
struct Rect {
    int32_t x, y;
    uint32_t w, h;
};
inline Rect FitRect(uint32_t fw, uint32_t fh, uint32_t ww, uint32_t wh) {
    const float frame_aspect = float(fw) / float(fh), window_aspect = float(ww) / float(wh);
    uint32_t w = ww, h = wh;
    if (frame_aspect > window_aspect) {
        h = uint32_t(float(ww) / frame_aspect);
    } else {
        w = uint32_t(float(wh) * frame_aspect);
    }
    return {int32_t(ww - w) / 2, int32_t(wh - h) / 2, w, h};
}

} // namespace vrmirror
