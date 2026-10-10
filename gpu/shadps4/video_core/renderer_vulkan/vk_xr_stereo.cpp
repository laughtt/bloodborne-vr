// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_xr_stereo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Vulkan {

namespace {

double Seconds() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

bool EnvOn(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] == '1';
}

float MaxDiff(const std::array<float, 64>& a, const float* b) {
    float worst = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    }
    return worst;
}

const char* KindName(xrstereo::Kind kind) {
    switch (kind) {
    case xrstereo::Kind::Eye:
        return "eye";
    case xrstereo::Kind::Mono:
        return "mono";
    default:
        return "hold";
    }
}

} // namespace

XrStereo& XrStereo::Get() {
    static XrStereo instance;
    return instance;
}

XrStereo::XrStereo() {
    const char* mode = std::getenv("BB_XR_MODE");
    enabled = mode && std::strcmp(mode, "alt") == 0;
    if (!enabled) {
        return;
    }
    log = EnvOn("BB_VR_STEREO_LOG") || EnvOn("BB_VR");
    if (const char* path = std::getenv("BB_VR_STEREO_TRACE"); path && *path) {
        trace = std::fopen(path, "w");
        if (trace) {
            std::fprintf(trace, "frame,time,kind,eye,seq,lag,side,reason,draws,s1,s2\n");
        }
    }
    std::printf("VR stereo: alternate eyes (BB_XR_MODE=alt): each finished frame goes to the eye "
                "its camera was set up for%s\n",
                trace ? " (trace on)" : "");
}

void XrStereo::Add(const xrstereo::EyeView& view) {
    std::scoped_lock lock{mutex};
    pairing.Add(view);
}

XrStereo::Status XrStereo::GetStatus() {
    std::scoped_lock lock{mutex};
    const auto& stats = pairing.stats();
    return Status{{stats.eye[0], stats.eye[1]}, pairing.LastEye(), pairing.Run()};
}

xrstereo::Decision XrStereo::OnFrame(const float* camera_view, float proj_x, const float* shadow,
                                     u64 draws_total) {
    std::scoped_lock lock{mutex};
    xrstereo::FrameCamera frame;
    if (camera_view) {
        frame.has_view = true;
        std::memcpy(frame.view, camera_view, sizeof(frame.view));
    }
    if (proj_x > 0.0f && std::isfinite(proj_x)) {
        frame.has_proj = true;
        frame.proj_x = proj_x;
    }
    const xrstereo::Decision d = pairing.OnFrame(frame);
    const u64 draws = last_draws ? draws_total - last_draws : 0;
    last_draws = draws_total;
    const double now = Seconds();
    const int eye = d.kind == xrstereo::Kind::Eye ? (d.eye & 1) : -1;

    // The shadow cascades of the scene constants against the previous two frames (trace s1, s2):
    // with alternating eyes s1 compares the two eyes, s2 the same eye two frames apart. Identical
    // eyes (s1 no larger than s2) mean the cascade fit does not follow the eye offset.
    float s1 = -1.0f, s2 = -1.0f;
    if (shadow) {
        if (shadow_eye[0] != -2) {
            s1 = MaxDiff(shadow_history[0], shadow);
        }
        if (shadow_eye[1] != -2) {
            s2 = MaxDiff(shadow_history[1], shadow);
        }
        if (eye >= 0 && shadow_eye[0] >= 0 && shadow_eye[0] != eye) {
            shadow_other_max = std::max(shadow_other_max, s1);
            ++shadow_other_n;
        }
        if (eye >= 0 && shadow_eye[1] == eye) {
            shadow_same_max = std::max(shadow_same_max, s2);
            ++shadow_same_n;
        }
        shadow_history[1] = shadow_history[0];
        shadow_eye[1] = shadow_eye[0];
        std::memcpy(shadow_history[0].data(), shadow, sizeof(float) * 64);
        shadow_eye[0] = eye;
    } else {
        shadow_eye[0] = shadow_eye[1] = -2; // no cascades: start over
    }

    if (eye >= 0) {
        eye_draw_sum[eye] += draws;
        ++eye_draw_frames[eye];
        // Draws of consecutive frames of the two eyes: culling or LOD that differs between the
        // eyes shows here (a still scene renders the same draws every frame).
        if (previous_kind_eye >= 0 && previous_kind_eye != eye) {
            lr_draw_diff_sum += draws > previous_frame_draws ? draws - previous_frame_draws
                                                             : previous_frame_draws - draws;
            ++lr_draw_pairs;
        }
        previous_kind_eye = eye;
        previous_frame_draws = draws;
    } else {
        previous_kind_eye = -1;
    }

    if (pairing.Mono() != was_mono) {
        was_mono = pairing.Mono();
        std::printf(was_mono ? "VR stereo: no eye frames for a while (%s): frames go to the mono "
                               "path\n"
                             : "VR stereo: eye frames again%s: alternate eyes\n",
                    was_mono ? d.reason : "");
    }
    if (trace) {
        std::fprintf(trace, "%llu,%.4f,%s,%d,%llu,%u,%d,%s,%llu,%.6g,%.6g\n",
                     static_cast<unsigned long long>(pairing.stats().frames), now,
                     KindName(d.kind), d.eye, static_cast<unsigned long long>(d.view.seq),
                     d.lag, d.side, d.reason, static_cast<unsigned long long>(draws), s1, s2);
        if (pairing.stats().frames % 120 == 0) {
            std::fflush(trace);
        }
    }
    if (now >= next_log) {
        if (next_log > 0.0 && log) {
            LogWindow();
        }
        next_log = now + 10.0;
    }
    return d;
}

void XrStereo::LogWindow() {
    const xrstereo::Stats& s = pairing.stats();
    const auto delta = [](u64 now, u64 before) { return now - before; };
    const u64 frames = delta(s.frames, logged.frames);
    const u64 left = delta(s.eye[0], logged.eye[0]), right = delta(s.eye[1], logged.eye[1]);
    const u64 given = left + right;
    std::printf("VR stereo: %llu frames: L %llu R %llu, held %llu (no camera %llu, repeat %llu, "
                "unmatched %llu), mono %llu; same eye twice %llu (longest run %u), eye tag "
                "kept apart %llu, side check failures %llu, %.1f game frames from camera to "
                "flip%s\n",
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(left),
                static_cast<unsigned long long>(right),
                static_cast<unsigned long long>(
                    delta(s.held_no_camera, logged.held_no_camera) +
                    delta(s.held_repeat, logged.held_repeat) +
                    delta(s.held_unmatched, logged.held_unmatched)),
                static_cast<unsigned long long>(delta(s.held_no_camera, logged.held_no_camera)),
                static_cast<unsigned long long>(delta(s.held_repeat, logged.held_repeat)),
                static_cast<unsigned long long>(delta(s.held_unmatched, logged.held_unmatched)),
                static_cast<unsigned long long>(delta(s.mono, logged.mono)),
                static_cast<unsigned long long>(delta(s.same_eye_twice, logged.same_eye_twice)),
                s.max_run,
                static_cast<unsigned long long>(delta(s.tag_rejects, logged.tag_rejects)),
                static_cast<unsigned long long>(delta(s.side_failures, logged.side_failures)),
                given ? double(delta(s.lag_sum, logged.lag_sum)) / double(given) : 0.0,
                delta(s.transposed, logged.transposed) ? " (rotation matched as columns)" : "");
    if (eye_draw_frames[0] && eye_draw_frames[1]) {
        std::printf("VR stereo:   draws/frame L %.1f R %.1f, consecutive L/R frames differ by "
                    "%.2f draws on average%s\n",
                    double(eye_draw_sum[0]) / double(eye_draw_frames[0]),
                    double(eye_draw_sum[1]) / double(eye_draw_frames[1]),
                    lr_draw_pairs ? double(lr_draw_diff_sum) / double(lr_draw_pairs) : 0.0,
                    shadow_other_n ? "" : "; no shadow cascades seen");
    }
    if (shadow_other_n) {
        std::printf("VR stereo:   shadow cascades (scene constants 112-175): largest change L<->R "
                    "%.3g (%llu pairs), same eye two frames apart %.3g (%llu pairs)\n",
                    shadow_other_max, static_cast<unsigned long long>(shadow_other_n),
                    shadow_same_max, static_cast<unsigned long long>(shadow_same_n));
    }
    logged = s;
    eye_draw_sum[0] = eye_draw_sum[1] = eye_draw_frames[0] = eye_draw_frames[1] = 0;
    lr_draw_diff_sum = lr_draw_pairs = 0;
    shadow_other_max = shadow_same_max = 0.0f;
    shadow_other_n = shadow_same_n = 0;
}

} // namespace Vulkan
