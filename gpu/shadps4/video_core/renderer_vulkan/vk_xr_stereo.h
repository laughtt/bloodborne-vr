// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport VR: alternate-eye stereo (BB_XR_MODE=alt, docs/vr/findings/22-phase3-alt-eye-stereo.md).
// The camera hook H1 picks each frame's eye and publishes its view (bbgpu_vr_set_eye_view, game
// thread); when the flip of a finished frame is prepared (Presenter::PrepareFrame, GPU thread)
// the frame's camera is paired with it (xrstereo::Pairing, gpu/xr_bridge/xr_stereo.h), and the
// present thread hands the frame to that eye's headset swapchain with the pose and frustum it was
// rendered with (Presenter::Present, Xr::ReleaseEye). Works without a headset too: the pairing,
// its log, the trace and the frame-dump tags are what the flat test (tools/vr_stereo_test.py)
// checks.
//
// Environment: BB_XR_MODE=alt turns it on; BB_VR_STEREO_LOG=1 a 10 s summary (always on with alt
// while a headset is used); BB_VR_STEREO_TRACE=<csv> one line per finished frame.

#pragma once

#include <array>
#include <cstdio>
#include <mutex>

#include "common/types.h"
#include "../../../xr_bridge/xr_stereo.h"

namespace Vulkan {

class XrStereo {
public:
    static XrStereo& Get();

    /// BB_XR_MODE=alt.
    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    /// The camera hook set up a frame for an eye (game thread).
    void Add(const xrstereo::EyeView& view);

    /// A finished frame at its flip (GPU thread): its scene camera (world to view 3x4 rows, null
    /// when the frame had none), its projection x scale (0 unknown), the shadow cascade block of
    /// its scene constants (null unknown) and the running draw counter.
    xrstereo::Decision OnFrame(const float* camera_view, float proj_x, const float* shadow,
                               u64 draws_total);

    /// What the frames went to so far, for the camera hook's eye schedule (any thread).
    struct Status {
        u64 assigned[2];
        int last_eye;
        u32 run;
    };
    Status GetStatus();

private:
    XrStereo();
    void LogWindow();

    bool enabled = false;
    bool log = false;
    std::mutex mutex;
    xrstereo::Pairing pairing;
    std::FILE* trace = nullptr;
    // The 10 s summary (GPU thread only, under mutex).
    double next_log = 0.0;
    xrstereo::Stats logged{};
    bool was_mono = false;
    u64 last_draws = 0;
    u64 eye_draw_sum[2] = {}, eye_draw_frames[2] = {};
    u64 lr_draw_diff_sum = 0, lr_draw_pairs = 0;
    int previous_kind_eye = -1;
    u64 previous_frame_draws = 0;
    // Shadow cascades of the previous two frames, [0] the latest, and their eyes (-1 a frame of
    // no eye, -2 no cascades).
    std::array<float, 64> shadow_history[2]{};
    int shadow_eye[2] = {-2, -2};
    float shadow_other_max = 0.0f, shadow_same_max = 0.0f;
    u64 shadow_other_n = 0, shadow_same_n = 0;
};

} // namespace Vulkan
