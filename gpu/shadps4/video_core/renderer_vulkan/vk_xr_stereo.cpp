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
    const xrstereo::XrMode mode = xrstereo::ParseMode(std::getenv("BB_XR_MODE"));
    enabled = mode == xrstereo::XrMode::Alt;
    pair_enabled = mode == xrstereo::XrMode::Stereo;
    if (pair_enabled) {
        std::printf("VR stereo: true stereo (BB_XR_MODE=stereo): every finished frame goes to the "
                    "headset as one pair of eye images with its render view; frames without a "
                    "camera match are held, then shown on the flat screen\n");
        return;
    }
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

void XrStereo::SetCinema(bool on) {
    std::scoped_lock lock{mutex};
    if (pair_enabled) {
        if (pair_cinema != on) {
            pair_cinema = on;
            std::printf("VR stereo: %s\n", on ? "cutscene: frames go to the cinema screen"
                                              : "cutscene over: pairs again once frames match");
        }
        return;
    }
    if (pairing.Cinema() == on) {
        return;
    }
    pairing.SetCinema(on);
    std::printf("VR stereo: %s\n", on ? "cutscene: frames go to the cinema screen"
                                      : "cutscene over: eye frames again once the eyes match");
}

xrstereo::Kind XrStereo::OnPairFrame(bool matched) {
    std::scoped_lock lock{mutex};
    const bool was_mono = decider.Mono();
    const xrstereo::Kind kind = decider.OnFrame(pair_cinema, matched);
    if (decider.Mono() != was_mono) {
        std::printf(decider.Mono() ? "VR stereo: no paired frames for a while (or a cutscene): "
                                     "frames go to the flat screen\n"
                                   : "VR stereo: paired frames again\n");
    }
    const double now = Seconds();
    if (now >= pair_next_log) {
        if (pair_next_log > 0.0) {
            const u64 renderer = eye_r_renderer.load(), copy = eye_r_copy.load();
            std::printf("VR stereo: %llu frames: %llu pairs, %llu held, %llu cutscene; eye R from "
                        "the stereo renderer %llu, eye L's image again %llu\n",
                        static_cast<unsigned long long>(decider.frames() - pair_logged.frames()),
                        static_cast<unsigned long long>(decider.pairs() - pair_logged.pairs()),
                        static_cast<unsigned long long>(decider.held() - pair_logged.held()),
                        static_cast<unsigned long long>(decider.cinema() - pair_logged.cinema()),
                        static_cast<unsigned long long>(renderer - eye_r_renderer_logged),
                        static_cast<unsigned long long>(copy - eye_r_copy_logged));
            eye_r_renderer_logged = renderer;
            eye_r_copy_logged = copy;
        }
        pair_logged = decider;
        pair_next_log = now + 10.0;
    }
    return kind;
}

void XrStereo::NoteEyeR(bool from_renderer) {
    (from_renderer ? eye_r_renderer : eye_r_copy).fetch_add(1, std::memory_order_relaxed);
}

xrstereo::Decision XrStereo::OnFrame(const float* const* camera_views, const float* proj_x,
                                     size_t count, const float* shadow, u64 draws_total) {
    std::scoped_lock lock{mutex};
    constexpr size_t kMaxCameras = 4;
    std::array<xrstereo::FrameCamera, kMaxCameras> frames{};
    count = std::min(count, kMaxCameras);
    for (size_t i = 0; i < count; ++i) {
        if (camera_views && camera_views[i]) {
            frames[i].has_view = true;
            std::memcpy(frames[i].view, camera_views[i], sizeof(frames[i].view));
        }
        if (proj_x && proj_x[i] > 0.0f && std::isfinite(proj_x[i])) {
            frames[i].has_proj = true;
            frames[i].proj_x = proj_x[i];
        }
    }
    const xrstereo::Decision d = pairing.OnFrame(frames.data(), count);
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
        if (was_mono && std::strcmp(d.reason, "unmatched") == 0) {
            // Which camera the frames had: how far the nearest published eye view was.
            std::printf("VR stereo:   %zu scene camera(s) in the frame, the nearest eye view %.4f "
                        "off in rotation, %.3f m in position (tolerance %.4f, %.3f m)\n",
                        count, d.best_rotation, d.best_position, 2e-3, 2e-3);
        }
    }
    if (d.kind == xrstereo::Kind::Eye && d.camera != 0 && !other_camera_logged) {
        other_camera_logged = true;
        std::printf("VR stereo: a frame matched its eye by scene camera %u of %zu (the first "
                    "main-pass camera was another view)\n",
                    d.camera + 1, count);
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
    if (const u64 cinema = delta(s.cinema, logged.cinema),
        other = delta(s.other_camera, logged.other_camera);
        cinema || other) {
        std::printf("VR stereo:   cinema screen (cutscene) %llu frames, matched by another scene "
                    "camera than the first %llu\n",
                    static_cast<unsigned long long>(cinema), static_cast<unsigned long long>(other));
    }
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
