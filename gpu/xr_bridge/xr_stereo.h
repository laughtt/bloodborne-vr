// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Alternate-eye stereo (BB_XR_MODE=alt, docs/vr/findings/22-phase3-alt-eye-stereo.md): pure
// logic, no OpenXR, Vulkan, threads or I/O. Tests: xr_stereo_test.cpp (bbport_xr_stereo_test).
//
//   Pairing (the port's presenter, vk_xr_stereo.cpp): which eye a finished game frame belongs
//   to. The camera hook H1 picks the eye when it sets up the camera and publishes an EyeView (seq,
//   eye, the camera's rows and position, the pose and frustum it renders with) into a ring. When
//   the frame is finished its camera (the scene constants' view, and the projection's x scale,
//   which carries the eye tag: the right eye's aspect is raised by a relative 4e-5) is matched
//   against the ring in order: the oldest view newer than the last frame given to an eye that
//   matches rotation, position and tag. So a lost frame never moves an eye's images to the other
//   eye, and a repeated frame is not given twice. A frame that matches nothing new (no camera, a
//   stale camera, a repeat) is held: both eyes keep their last image. After
//   Config::fallback_frames held frames in a row (menus, loading screens, a camera the hook did
//   not set) frames go to the mono path until an eye matches again.
//
//   AltEyes (the bridge): when the projection layer shows the two eye images, each with the view
//   it was rendered with, and the per-eye image ages of the pacing log.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace xrstereo {

/// What the camera hook published for one frame it set up (BbVrEyeView).
struct EyeView {
    uint64_t seq = 0;
    int eye = 0;             // 0 left, 1 right
    bool has_pose = false;   // the pose is the headset's (false: flat tests, no layer)
    int64_t display_time = 0;
    float pose_position[3] = {};
    float pose_orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float fov[4] = {};       // left, right, up, down (radians; left and down < 0)
    float rows[3][3] = {};   // camera right, up, forward (world)
    float position[3] = {};  // camera position (world), at the eye
    float center[3] = {};    // camera position before the eye offset
};

/// A finished frame's camera, from the scene constants (CameraMotion).
struct FrameCamera {
    bool has_view = false;
    float view[12] = {};   // world to view, 3x4 rows [R | t]: view = R world + t
    bool has_proj = false;
    float proj_x = 0.0f;   // the projection's x scale (1 / tan of the right half angle)
};

struct Config {
    float rotation_tolerance = 2e-3f; // per element of the rotation rows (as the mono pairing)
    float position_tolerance = 2e-3f; // meters
    float tag_tolerance = 1e-5f;      // relative: x scale times the view's right tangent, minus 1
    uint32_t fallback_frames = 30;    // held frames in a row before the mono path takes over
};

/// How far a frame's camera is from a published view.
struct Error {
    float rotation = 1e9f;
    float position = 1e9f;
    float tag = 0.0f;       // 0 when the frame has no projection or the view no frustum
    bool transposed = false; // the rotation matched as columns (only seen if the layout differs)
};

/// The camera position of a view [R | t] (rows of R = the camera axes, each up to sign):
/// p = -R^T t. With transposed = true the axes are R's columns.
inline void ViewPosition(const float* view, bool transposed, float out[3]) {
    for (int k = 0; k < 3; ++k) {
        float p = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float axis_k = transposed ? view[k * 4 + i] : view[i * 4 + k];
            p -= view[i * 4 + 3] * axis_k;
        }
        out[k] = p;
    }
}

inline Error Compare(const FrameCamera& frame, const EyeView& v) {
    Error e;
    if (!frame.has_view) {
        return e;
    }
    // Each row up to sign (a view may flip an axis), as rows or as columns (vk_xr.cpp RowsError).
    float direct = 0.0f, transposed = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float plus = 0.0f, minus = 0.0f, tplus = 0.0f, tminus = 0.0f;
        for (int j = 0; j < 3; ++j) {
            plus = std::max(plus, std::fabs(frame.view[i * 4 + j] - v.rows[i][j]));
            minus = std::max(minus, std::fabs(frame.view[i * 4 + j] + v.rows[i][j]));
            tplus = std::max(tplus, std::fabs(frame.view[j * 4 + i] - v.rows[i][j]));
            tminus = std::max(tminus, std::fabs(frame.view[j * 4 + i] + v.rows[i][j]));
        }
        direct = std::max(direct, std::min(plus, minus));
        transposed = std::max(transposed, std::min(tplus, tminus));
    }
    e.transposed = transposed < direct;
    e.rotation = std::min(direct, transposed);
    float p[3];
    ViewPosition(frame.view, e.transposed, p);
    const float dx = p[0] - v.position[0], dy = p[1] - v.position[1], dz = p[2] - v.position[2];
    e.position = std::sqrt(dx * dx + dy * dy + dz * dz);
    // The x scale is 1 / the frustum's half-width in tangents (also for an off-axis frustum).
    if (frame.has_proj && v.fov[1] > 0.0f && v.fov[1] < 1.55f && v.fov[0] < 0.0f && v.fov[0] > -1.55f) {
        const float half = 0.5f * (std::tan(v.fov[1]) - std::tan(v.fov[0]));
        e.tag = std::fabs(frame.proj_x * half - 1.0f);
    }
    return e;
}

/// -1 left of the view's center camera, +1 right, 0 within 1 mm (no offset): the side the frame's
/// camera really is on, a check independent of the eye label.
inline int Side(const FrameCamera& frame, const EyeView& v, bool transposed) {
    float p[3];
    ViewPosition(frame.view, transposed, p);
    const float s = (p[0] - v.center[0]) * v.rows[0][0] + (p[1] - v.center[1]) * v.rows[0][1] +
                    (p[2] - v.center[2]) * v.rows[0][2];
    return s < -1e-3f ? -1 : s > 1e-3f ? 1 : 0;
}

enum class Kind { Hold, Eye, Mono };

struct Decision {
    Kind kind = Kind::Hold;
    int eye = -1;          // Kind::Eye
    EyeView view{};        // Kind::Eye: the matched view
    uint32_t lag = 0;      // Kind::Eye: views published after the matched one (game frames)
    int side = 0;          // Kind::Eye: Side() of the frame's camera (-1 left, +1 right, 0 none)
    uint64_t frame = 0;    // the finished frame's number (Stats::frames after it)
    const char* reason = "";
};

struct Stats {
    uint64_t frames = 0;
    uint64_t eye[2] = {};
    uint64_t held_no_camera = 0, held_repeat = 0, held_unmatched = 0;
    uint64_t mono = 0;
    uint64_t tag_rejects = 0;   // pose matched but the eye tag did not: a swap the tag prevented
    uint64_t side_failures = 0; // given to an eye whose side the camera is not on: a real swap
    uint64_t transposed = 0;    // matched with the rotation as columns
    uint64_t same_eye_twice = 0; // an eye got two frames in a row (the other eye's was lost)
    uint32_t max_run = 0;       // the longest run of frames given to one eye
    uint64_t lag_sum = 0;
};

class Pairing {
public:
    static constexpr size_t kRing = 16;

    explicit Pairing(Config config = {}) : config_{config} {}

    /// The camera hook set up a frame (any order of seq is kept as given; seq grows).
    void Add(const EyeView& view) {
        ring_[count_ % kRing] = view;
        ++count_;
    }

    Decision OnFrame(const FrameCamera& frame) {
        ++stats_.frames;
        Decision d;
        d.frame = stats_.frames;
        const uint64_t size = std::min<uint64_t>(count_, kRing);
        int match = -1;
        bool repeat = false;
        int tag_rejected_eyes = 0; // bit e: a newer view of eye e had the pose but not the tag
        Error match_error;
        if (frame.has_view) {
            // Oldest first: frames are presented in the order they were set up.
            for (uint64_t back = size; back-- > 0;) {
                const EyeView& v = ring_[(count_ - 1 - back) % kRing];
                const Error e = Compare(frame, v);
                if (e.rotation > config_.rotation_tolerance ||
                    e.position > config_.position_tolerance) {
                    continue;
                }
                if (any_ && v.seq <= last_seq_) {
                    repeat = true; // this frame (or its camera) was already given to an eye
                    continue;
                }
                if (e.tag > config_.tag_tolerance) {
                    // Another projection: the other eye's tag (a camera that coincides with the
                    // other eye's, e.g. strafing at one IPD per frame), or the same eye before a
                    // FOV change.
                    tag_rejected_eyes |= 1 << (v.eye & 1);
                    continue;
                }
                match = int((count_ - 1 - back) % kRing);
                match_error = e;
                d.lag = uint32_t(back);
                break;
            }
        }
        if (match >= 0) {
            const EyeView& v = ring_[match];
            d.kind = Kind::Eye;
            d.eye = v.eye;
            d.view = v;
            d.reason = "matched";
            last_seq_ = v.seq;
            any_ = true;
            streak_ = 0;
            mono_ = false;
            ++stats_.eye[v.eye & 1];
            stats_.lag_sum += d.lag;
            stats_.transposed += match_error.transposed;
            // Only the other eye's view counts: without the tag this frame could have gone there.
            stats_.tag_rejects += (tag_rejected_eyes >> (1 - (v.eye & 1))) & 1;
            d.side = Side(frame, v, match_error.transposed);
            if (d.side != 0 && d.side != (v.eye ? 1 : -1)) {
                ++stats_.side_failures;
            }
            if (last_eye_ == v.eye) {
                ++run_;
                ++stats_.same_eye_twice;
            } else {
                last_eye_ = v.eye;
                run_ = 1;
            }
            stats_.max_run = std::max(stats_.max_run, run_);
            return d;
        }
        d.reason = !frame.has_view ? "no camera" : repeat ? "repeat" : "unmatched";
        if (!frame.has_view) {
            ++stats_.held_no_camera;
        } else if (repeat) {
            ++stats_.held_repeat;
        } else {
            ++stats_.held_unmatched;
        }
        ++streak_;
        if (mono_ || streak_ >= config_.fallback_frames) {
            mono_ = true;
            d.kind = Kind::Mono;
            ++stats_.mono;
        }
        return d;
    }

    const Stats& stats() const {
        return stats_;
    }
    /// The eye the latest frame went to (-1 none yet) and how many in a row went to it.
    int LastEye() const {
        return last_eye_;
    }
    uint32_t Run() const {
        return run_;
    }
    bool Mono() const {
        return mono_;
    }
    uint64_t Published() const {
        return count_;
    }

private:
    Config config_;
    std::array<EyeView, kRing> ring_{};
    uint64_t count_ = 0;
    uint64_t last_seq_ = 0;
    bool any_ = false;
    uint32_t streak_ = 0;
    bool mono_ = false;
    int last_eye_ = -1;
    uint32_t run_ = 0;
    Stats stats_{};
};

// ---- the bridge: the alt projection layer ------------------------------------------------------

/// Which images the bridge's projection layer shows in alt mode. Each eye keeps its last released
/// image (and the view it was rendered with); the layer shows them once both eyes have one that is
/// newer than the latest mono (flat) image, so coming back from a loading screen never pairs a
/// fresh eye with an image from before it.
struct AltEyes {
    bool valid[2] = {};          // an image released since the latest flat image
    uint64_t released[2] = {};   // images released per eye
    double released_at[2] = {-1.0, -1.0};
    double flat_at = -1.0;

    void ReleaseEye(int eye, double now) {
        valid[eye & 1] = true;
        ++released[eye & 1];
        released_at[eye & 1] = now;
    }
    void ReleaseFlat(double now) {
        flat_at = now;
        valid[0] = valid[1] = false;
    }
    /// Both eyes have an image newer than the latest flat one, and the newest is at most max_age
    /// seconds old (after that the host stopped sending eyes: the mono or background layer).
    bool Showing(double now, double max_age) const {
        if (!valid[0] || !valid[1]) {
            return false;
        }
        const double newest = std::max(released_at[0], released_at[1]);
        return newest > flat_at && now - newest <= max_age;
    }
};

/// Pacing statistics of the alt layer, per display frame: how many display frames old each eye's
/// image is (0 = released since the previous display frame). The counters cover a log window
/// (ResetWindow); the ages carry over.
struct AgeStats {
    static constexpr uint32_t kBuckets = 6; // 0, 1, 2, 3, 4, 5+
    struct Window {
        uint64_t frames = 0;
        uint64_t hist[2][kBuckets] = {};
        uint64_t both_fresh = 0, both_stale = 0, one_fresh = 0;
        uint32_t max_age[2] = {};
    } window;
    uint64_t seen[2] = {};
    uint32_t age[2] = {};
    bool started = false;

    /// One display frame showing the alt layer; `released` = AltEyes::released at that time.
    void Frame(const uint64_t released[2]) {
        for (int e = 0; e < 2; ++e) {
            if (!started || released[e] != seen[e]) {
                age[e] = 0;
                seen[e] = released[e];
            } else {
                ++age[e];
            }
            ++window.hist[e][std::min<uint32_t>(age[e], kBuckets - 1)];
            window.max_age[e] = std::max(window.max_age[e], age[e]);
        }
        started = true;
        window.both_fresh += age[0] == 0 && age[1] == 0;
        window.both_stale += age[0] > 0 && age[1] > 0;
        window.one_fresh += (age[0] == 0) != (age[1] == 0);
        ++window.frames;
    }
    double MeanAge(int eye) const {
        uint64_t n = 0, sum = 0;
        for (uint32_t b = 0; b < kBuckets; ++b) {
            n += window.hist[eye][b];
            sum += window.hist[eye][b] * b;
        }
        return n ? double(sum) / double(n) : 0.0;
    }
    void ResetWindow() {
        window = Window{};
    }
};

} // namespace xrstereo
