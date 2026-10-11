// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Screen placement and laser-pointer math for the cinema screen, pure functions with no OpenXR or
// Vulkan dependency. The bridge's FrameLoop() places its screen and hit-tests the laser with these
// and reports both through BbXrApi::GetViews (xr_sandbox.cpp shows them): one implementation.
// The screen sits `distance` meters in front of the head, flattened to the floor plane (head yaw
// only), at head height, facing the head along its +Z. A curved screen is the arc of a cylinder
// whose axis runs through the head (CylinderCenter).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace xrscreen {

struct Vec3 {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};
struct Pose {
    Quat orientation;
    Vec3 position;
};

inline Vec3 Add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 Sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 Scale(Vec3 v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

inline Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/// Unit length, or zero for a (near) zero vector.
inline Vec3 Normalize(Vec3 v) {
    const float length = std::sqrt(Dot(v, v));
    return length > 1e-6f ? Scale(v, 1.0f / length) : Vec3{0.0f, 0.0f, 0.0f};
}

inline bool IsFinite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// Rotates v by the unit quaternion q.
inline Vec3 Rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 c = Cross(u, v);
    const Vec3 t{c.x + q.w * v.x, c.y + q.w * v.y, c.z + q.w * v.z};
    const Vec3 d = Cross(u, t);
    return {v.x + 2.0f * d.x, v.y + 2.0f * d.y, v.z + 2.0f * d.z};
}

/// The Hamilton product a * b: rotates by b first, then by a.
inline Quat Multiply(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Quat Conjugate(Quat q) {
    return {-q.x, -q.y, -q.z, q.w};
}

/// Halfway between two orientations (normalized sum on the same hemisphere).
inline Quat Nlerp(Quat a, Quat b) {
    const float sign = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f ? -1.0f : 1.0f;
    const Quat q{a.x + sign * b.x, a.y + sign * b.y, a.z + sign * b.z, a.w + sign * b.w};
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return length > 1e-6f ? Quat{q.x / length, q.y / length, q.z / length, q.w / length} : a;
}

/// The rotation whose local X, Y and Z axes are the given orthonormal (right-handed) vectors.
inline Quat FromBasis(Vec3 x, Vec3 y, Vec3 z) {
    const float trace = x.x + y.y + z.z;
    Quat q;
    if (trace > 0.0f) {
        const float s = 2.0f * std::sqrt(trace + 1.0f);
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (x.x > y.y && x.x > z.z) {
        const float s = 2.0f * std::sqrt(1.0f + x.x - y.y - z.z);
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        const float s = 2.0f * std::sqrt(1.0f + y.y - x.x - z.z);
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const float s = 2.0f * std::sqrt(1.0f + z.z - x.x - y.y);
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / length, q.y / length, q.z / length, q.w / length};
}

/// `child`, given relative to `parent`, in the parent's space.
inline Pose Compose(const Pose& parent, const Pose& child) {
    return {Multiply(parent.orientation, child.orientation),
            Add(parent.position, Rotate(parent.orientation, child.position))};
}

/// Rotation about +Y (radians) of a yaw-only quaternion such as PlaceScreen's.
inline float Yaw(Quat q) {
    return 2.0f * std::atan2(q.y, q.w);
}

/// Screen pose for a head pose (same space as the screen): in front of the head's flattened
/// forward direction, at head height, rotated by the head yaw.
inline Pose PlaceScreen(Vec3 head_position, Quat head, float distance) {
    float fx = -2.0f * (head.x * head.z + head.w * head.y);
    float fz = -(1.0f - 2.0f * (head.x * head.x + head.y * head.y));
    const float length = std::sqrt(fx * fx + fz * fz);
    if (length > 0.1f) {
        fx /= length;
        fz /= length;
    } else {
        fx = 0.0f;
        fz = -1.0f;
    }
    const float yaw = std::atan2(-fx, -fz);
    return {{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)},
            {head_position.x + fx * distance, head_position.y, head_position.z + fz * distance}};
}

/// Mono head tracking (ABI 5): eye `eye`'s projection-layer pose for an image rendered from
/// `render_head`. Both eyes take the render orientation (parallel, sharing the image's one
/// frustum; the runtime warps it to canted panels) and keep their offset from the head, taken
/// from the located eye views `eye` and `other` (the head is their midpoint).
inline Pose MonoEye(const Pose& render_head, const Pose& eye, const Pose& other) {
    const Quat middle = Nlerp(eye.orientation, other.orientation);
    const Vec3 center = Scale(Add(eye.position, other.position), 0.5f);
    const Vec3 offset = Rotate(Conjugate(middle), Sub(eye.position, center));
    return {render_head.orientation,
            Add(render_head.position, Rotate(render_head.orientation, offset))};
}

/// The pose the bridge uses before it has placed the screen, and while head-locked.
inline Pose DefaultScreen(float distance) {
    return {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -distance}};
}

/// One frame's head pose for placement.
struct HeadSample {
    bool oriented = false;   // the orientation is valid
    bool positioned = false; // the position is valid and finite too
    Pose pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
};

inline float Length(Vec3 v) {
    return std::sqrt(Dot(v, v));
}

/// Head positions a runtime can really report: at most kPlausibleY above or below the space
/// origin and at most kPlausibleRadius from it. Pimax reports poses tens of meters off (head
/// height -77 m) for seconds at startup and before the headset is worn, with the valid bits set.
constexpr float kPlausibleY = 3.0f;
constexpr float kPlausibleRadius = 5.0f;

/// A sample the screen may be placed at: oriented, positioned and plausible.
inline bool Plausible(const HeadSample& head) {
    return head.oriented && head.positioned && IsFinite(head.pose.position) &&
           std::fabs(head.pose.position.y) <= kPlausibleY &&
           Length(head.pose.position) <= kPlausibleRadius;
}

/// When and where to place the screen. Only plausible samples (above) ever place it, never a junk
/// pose. The most recent plausible sample is kept across Arm (`Recent`).
///   - Tracked: once the head was plausibly tracked in most of the last second (single frames
///     drop out, so no streak is needed); a manual recenter (`immediate`) takes the next one.
///   - Fallback: tracking never steadied within kFallbackSeconds of the first plausible sample
///     since Arm: at the latest plausible sample. A manual recenter that gets no plausible sample
///     within kImmediateFallbackSeconds uses the most recent plausible one, from before Arm too.
///   - Kept / Default: no plausible sample since Arm (a runtime still sending junk) for
///     kDefaultSeconds, or for kImmediateFallbackSeconds on a manual recenter with no plausible
///     sample ever: the screen stays where it was if that was a plausible placement (Kept), else
///     it goes to DefaultScreen, in front of the space origin (Default). Both are provisional:
///     Watch asks for a new placement once plausible tracking arrives.
/// While placed, Watch also asks again when the plausibly tracked head stays more than
/// kFarMeters from the screen (a pose that slipped through, or the space moved under it).
/// Pure logic: the caller feeds one sample per frame (Update while Waiting, else Watch).
class Placer {
public:
    static constexpr int kWindow = 90; // samples looked back on (~1 s at 90 Hz)
    static constexpr int kNeeded = 60; // plausible samples among them
    static constexpr double kFallbackSeconds = 3.0;
    static constexpr double kImmediateFallbackSeconds = 0.5;
    static constexpr double kDefaultSeconds = 10.0;
    static constexpr float kFarMeters = 5.0f;
    static constexpr double kWatchSeconds = 0.5; // a Watch condition must hold this long
    // A pose out of range that stays put (within kSteadyMeters) this long is the real head in a
    // runtime whose space origin is far away (Pimax LOCAL, seen 9 to 25 m off for minutes), not
    // the jumping junk of a starting runtime: the screen is placed there (Steady).
    static constexpr double kSteadySeconds = 1.5;
    static constexpr double kImmediateSteadySeconds = 0.3; // manual recenter
    static constexpr float kSteadyMeters = 0.35f;

    enum class How { None, Tracked, Fallback, Kept, Default, Steady };
    enum class Again { No, Far, Tracking };

    /// Starts over (the most recent plausible sample and how the screen was placed are kept).
    /// `immediate` (manual recenter): the next plausible sample places the screen.
    void Arm(double now, bool at_once) {
        Placer fresh;
        fresh.recent = recent;
        fresh.recent_at = recent_at;
        fresh.have_recent = have_recent;
        fresh.how = how;
        fresh.redo_fallback = redo_fallback;
        fresh.redo_used = redo_used;
        fresh.accept_steady = accept_steady;
        *this = fresh;
        armed = true;
        armed_at = now;
        immediate = at_once;
    }

    bool Waiting() const {
        return armed;
    }

    /// One frame. Returns true when it is time to place the screen: *screen is the new pose
    /// (unchanged for Kept) and `how` says which way.
    bool Update(double now, const HeadSample& head, float distance, Pose* screen) {
        if (!armed) {
            return false;
        }
        const bool good = Plausible(head);
        good_count += int(good) - int(ring[next]);
        ring[next] = good;
        next = (next + 1) % kWindow;
        samples = samples < kWindow ? samples + 1 : kWindow;
        no_pose += !head.oriented;
        no_position += head.oriented && !head.positioned;
        out_of_range += head.oriented && head.positioned && !good;
        if (head.positioned) {
            last_position = head.pose.position;
            have_position = true;
        }
        if (good) {
            recent = head.pose;
            recent_at = now;
            have_recent = true;
            if (!seen_since_arm) {
                seen_since_arm = true;
                first_good_at = now;
            }
        }
        // Steadiness of out-of-range (but oriented, positioned, finite) poses.
        const bool usable = head.oriented && head.positioned && IsFinite(head.pose.position);
        if (!usable || good) {
            steady_since = -1.0;
        } else if (steady_since < 0.0 ||
                   Length(Sub(head.pose.position, steady_anchor)) > kSteadyMeters) {
            steady_since = now;
            steady_anchor = head.pose.position;
        }
        const bool steady =
            accept_steady && !seen_since_arm && steady_since >= 0.0 &&
            now - steady_since >= (immediate ? kImmediateSteadySeconds : kSteadySeconds);
        const double waited = now - armed_at;
        const bool placed_plausibly =
            how == How::Tracked || how == How::Fallback || how == How::Kept;
        How next_how = How::None;
        if (good && (immediate || good_count >= kNeeded)) {
            next_how = How::Tracked;
        } else if (steady) {
            next_how = How::Steady;
        } else if (immediate && waited >= kImmediateFallbackSeconds) {
            next_how = have_recent ? How::Fallback : placed_plausibly ? How::Kept : How::Default;
        } else if (!immediate && seen_since_arm && now - first_good_at >= kFallbackSeconds) {
            next_how = How::Fallback;
        } else if (!immediate && !seen_since_arm && waited >= kDefaultSeconds) {
            next_how = placed_plausibly ? How::Kept : How::Default;
        }
        switch (next_how) {
        case How::None:
            return false;
        case How::Steady:
        case How::Tracked:
            age = 0.0;
            *screen = PlaceScreen(head.pose.position, head.pose.orientation, distance);
            break;
        case How::Fallback:
            age = now - recent_at;
            *screen = PlaceScreen(recent.position, recent.orientation, distance);
            break;
        case How::Kept:
            age = 0.0;
            break;
        case How::Default:
            age = 0.0;
            *screen = DefaultScreen(distance);
            break;
        }
        how = next_how;
        armed = false;
        watch_since = -1.0;
        return true;
    }

    /// While placed (not Waiting): whether to place the screen again, from this frame's sample.
    /// Implausible samples are ignored; a condition must hold over kWatchSeconds of plausible
    /// samples. The caller re-arms (not immediate) on Far or Tracking.
    Again Watch(double now, const HeadSample& head, const Pose& screen) {
        if (armed || !Plausible(head)) {
            return Again::No;
        }
        recent = head.pose;
        recent_at = now;
        have_recent = true;
        head_to_screen = Length(Sub(head.pose.position, screen.position));
        // A Fallback placement (tracking never steadied) is provisional too, once: it is redone
        // when the head is steadily plausible (redo_fallback; BB_XR_RECENTER_ON_START).
        const bool provisional = how == How::Kept || how == How::Default ||
                                 (how == How::Fallback && redo_fallback && !redo_used);
        const Again want = provisional                   ? Again::Tracking
                           : head_to_screen > kFarMeters ? Again::Far
                                                         : Again::No;
        if (want == Again::No) {
            watch_since = -1.0;
            return Again::No;
        }
        if (watch_since < 0.0) {
            watch_since = now;
        }
        if (now - watch_since < kWatchSeconds) {
            return Again::No;
        }
        if (want == Again::Tracking && how == How::Fallback) {
            redo_used = true;
        }
        return want;
    }

    float LastY() const {
        return have_position ? last_position.y : 0.0f;
    }

    /// Seconds left before a Kept/Default placement (0 once a plausible sample came).
    double DefaultIn(double now) const {
        if (seen_since_arm) {
            return 0.0;
        }
        const double limit = immediate ? kImmediateFallbackSeconds : kDefaultSeconds;
        return std::max(0.0, limit - (now - armed_at));
    }

    /// The most recent plausible sample (Update and Watch keep it, Arm does not clear it).
    bool Recent(Pose* pose, double* at) const {
        if (have_recent) {
            *pose = recent;
            *at = recent_at;
        }
        return have_recent;
    }

    // Diagnostics: why the placement waits, and how the last one went.
    int good_count = 0, samples = 0;                   // plausible samples in the window, its fill
    int no_pose = 0, no_position = 0, out_of_range = 0; // rejected samples since Arm
    How how = How::None; // the last placement
    double age = 0.0;    // Fallback: seconds between the sample used and the placement
    float head_to_screen = 0.0f; // Watch: the latest plausible head's distance to the screen
    bool redo_fallback = true;   // a Fallback placement is redone once tracking is steady
    bool accept_steady = true;   // place at a steady out-of-range head (How::Steady)

private:
    bool armed = false, immediate = false, seen_since_arm = false;
    bool redo_used = false; // the one redo of a Fallback placement has been asked for
    double armed_at = 0.0, first_good_at = 0.0;
    bool ring[kWindow] = {};
    int next = 0;
    Vec3 last_position{0.0f, 0.0f, 0.0f};
    bool have_position = false;
    Pose recent{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}; // most recent plausible sample
    double recent_at = 0.0;
    bool have_recent = false;
    double watch_since = -1.0;
    double steady_since = -1.0;
    Vec3 steady_anchor{0.0f, 0.0f, 0.0f};
};

/// A name for logs.
inline const char* HowName(Placer::How how) {
    switch (how) {
    case Placer::How::Tracked:
        return "tracked";
    case Placer::How::Fallback:
        return "fallback";
    case Placer::How::Steady:
        return "steady";
    case Placer::How::Kept:
        return "kept";
    case Placer::How::Default:
        return "default";
    default:
        return "none";
    }
}

/// BB_XR_SCREEN_FOLLOW: whether the flat screen is put in front of the user again when it becomes
/// visible after the 3D view (lobby, menus, loading).
enum class Follow { Off, Reappear, Always };

inline Follow ParseFollow(const char* text) {
    if (!text || !*text) {
        return Follow::Reappear;
    }
    const std::string_view v{text};
    if (v == "off" || v == "0" || v == "no") {
        return Follow::Off;
    }
    if (v == "always") {
        return Follow::Always;
    }
    return Follow::Reappear;
}

/// Angle in degrees between where the head faces (flattened to the floor) and the direction to
/// the screen; 0 for a degenerate input.
inline float HeadAngleToScreen(const HeadSample& head, const Pose& screen) {
    const Quat q = head.pose.orientation;
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float dx = screen.position.x - head.pose.position.x;
    const float dz = screen.position.z - head.pose.position.z;
    const float a = std::sqrt(fx * fx + fz * fz), b = std::sqrt(dx * dx + dz * dz);
    if (a < 0.1f || b < 1e-3f) {
        return 0.0f;
    }
    const float c = std::clamp((fx * dx + fz * dz) / (a * b), -1.0f, 1.0f);
    return std::acos(c) * 57.29578f;
}

constexpr float kFollowDegrees = 60.0f;

/// A head the cinema screen may follow: oriented, positioned and finite, however far the runtime's
/// space origin is (Plausible() also rejects Pimax's real head 9 to 25 m from its LOCAL origin, so
/// a screen placed there by Placer (Steady) was never put in front of the user again).
inline bool Usable(const HeadSample& head) {
    return head.oriented && head.positioned && IsFinite(head.pose.position);
}

/// Normalized lerp from a to b (t in 0..1), on the same hemisphere.
inline Quat NlerpT(Quat a, Quat b, float t) {
    const float sign = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f ? -1.0f : 1.0f;
    const Quat q{a.x + t * (sign * b.x - a.x), a.y + t * (sign * b.y - a.y),
                 a.z + t * (sign * b.z - a.z), a.w + t * (sign * b.w - a.w)};
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return length > 1e-6f ? Quat{q.x / length, q.y / length, q.z / length, q.w / length} : a;
}

/// The cinema screen while the flat image is what the user sees (menus, loading, summon, bell):
///   - Show: it just became the visible layer after the 3D view. With recenter_on_show it is put
///     in front of the head at once (head yaw only, level, same distance), whatever it was before.
///   - Tick (every frame while it shows): world-locked (the pose does not change) until the head
///     has faced more than follow_deg away from it for follow_seconds; then it glides to the front
///     of the head over glide_seconds (lazy follow). follow_deg <= 0: never.
///   - Hide: the 3D view is back; forgets the timers.
/// Pure logic: the caller feeds the head each frame and takes `*screen` back.
class CinemaFollow {
public:
    struct Config {
        bool recenter_on_show = true;
        float follow_deg = kFollowDegrees;
        double follow_seconds = 1.0;
        double glide_seconds = 0.6;
    };

    /// Returns true when the screen was moved (snapped in front of the head).
    bool Show(const Config& cfg, const HeadSample& head, float distance, Pose* screen) {
        away_since_ = -1.0;
        gliding_ = false;
        if (!cfg.recenter_on_show || !Usable(head)) {
            return false;
        }
        *screen = PlaceScreen(head.pose.position, head.pose.orientation, distance);
        return true;
    }

    void Hide() {
        away_since_ = -1.0;
        gliding_ = false;
    }

    /// Returns true when a glide starts this call (for a log line).
    bool Tick(double now, const Config& cfg, const HeadSample& head, float distance,
              Pose* screen) {
        if (gliding_) {
            const double u = std::clamp((now - glide_at_) / cfg.glide_seconds, 0.0, 1.0);
            const float s = float(u * u * (3.0 - 2.0 * u));
            screen->orientation = NlerpT(from_.orientation, to_.orientation, s);
            screen->position = Add(from_.position,
                                   Scale(Sub(to_.position, from_.position), s));
            if (u >= 1.0) {
                gliding_ = false;
                away_since_ = -1.0;
            }
            return false;
        }
        if (cfg.follow_deg <= 0.0f || !Usable(head) ||
            HeadAngleToScreen(head, *screen) <= cfg.follow_deg) {
            away_since_ = -1.0;
            return false;
        }
        if (away_since_ < 0.0) {
            away_since_ = now;
        }
        if (now - away_since_ < cfg.follow_seconds) {
            return false;
        }
        from_ = *screen;
        to_ = PlaceScreen(head.pose.position, head.pose.orientation, distance);
        glide_at_ = now;
        gliding_ = true;
        return true;
    }

    bool Gliding() const {
        return gliding_;
    }

private:
    double away_since_ = -1.0, glide_at_ = 0.0;
    bool gliding_ = false;
    Pose from_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}, to_ = from_;
};

/// The flat screen just reappeared after the 3D view: place it again in front of the user? Only
/// with a plausible head, when following is on and (always, or the head faces more than
/// kFollowDegrees away from the screen).
inline bool ShouldFollowOnReappear(Follow mode, const HeadSample& head, const Pose& screen,
                                   float limit_degrees = kFollowDegrees) {
    if (mode == Follow::Off || !Plausible(head)) {
        return false;
    }
    return mode == Follow::Always || HeadAngleToScreen(head, screen) > limit_degrees;
}

// Curved screen: the cylinder's center (on its axis, where the viewer belongs) for the screen
/// pose, `radius` behind it along +Z with the same orientation. OpenXR cylinder layers take this
/// pose and show the arc around its -Z axis.
inline Pose CylinderCenter(const Pose& screen, float radius) {
    return Compose(screen, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, radius}});
}

/// The arc angle (radians) of a curved screen `width_m` long on a cylinder of `radius`, kept
/// below a full turn.
inline float ArcAngle(float width_m, float radius) {
    const float angle = width_m / radius;
    constexpr float kMax = 6.0f; // < 2 pi
    return angle < kMax ? angle : kMax;
}

/// Intersects the ray from `origin` along the pose's -Z with the screen quad (width_m wide,
/// height following the pixel aspect). On a hit returns true and the pixel in (u, v), v down,
/// and the distance along the ray in *t when given; u/v may lie outside [0, width) x
/// [0, height) when `inside_only` is false.
inline bool RayToPixel(Vec3 origin, Quat aim, const Pose& screen, float width_m, uint32_t width,
                       uint32_t height, bool inside_only, float* u, float* v,
                       float* t_out = nullptr) {
    const Vec3 dir = Rotate(aim, {0.0f, 0.0f, -1.0f});
    const Vec3 normal = Rotate(screen.orientation, {0.0f, 0.0f, 1.0f});
    const Vec3 right = Rotate(screen.orientation, {1.0f, 0.0f, 0.0f});
    const Vec3 up = Rotate(screen.orientation, {0.0f, 1.0f, 0.0f});
    const float denominator = Dot(dir, normal);
    if (std::fabs(denominator) < 1e-5f) {
        return false;
    }
    const float t = Dot(Sub(screen.position, origin), normal) / denominator;
    if (t <= 0.0f) {
        return false;
    }
    const Vec3 hit = Sub(Add(origin, Scale(dir, t)), screen.position);
    const float height_m = width_m * float(height) / float(width);
    const float px = (Dot(hit, right) / width_m + 0.5f) * float(width);
    const float py = (0.5f - Dot(hit, up) / height_m) * float(height);
    if (inside_only && (px < 0.0f || py < 0.0f || px >= float(width) || py >= float(height))) {
        return false;
    }
    *u = px;
    *v = py;
    if (t_out) {
        *t_out = t;
    }
    return true;
}

/// RayToPixel for the curved screen: the screen pose is the middle of the arc, the cylinder of
/// `radius` stands around CylinderCenter(screen, radius) and the arc is `width_m` long. The hit
/// is the cylinder wall ahead of an origin inside it (the viewer's side of the screen).
inline bool RayToCylinderPixel(Vec3 origin, Quat aim, const Pose& screen, float radius,
                               float width_m, uint32_t width, uint32_t height, bool inside_only,
                               float* u, float* v, float* t_out = nullptr) {
    const Pose center = CylinderCenter(screen, radius);
    const Quat to_local = Conjugate(center.orientation);
    const Vec3 o = Rotate(to_local, Sub(origin, center.position));
    const Vec3 d = Rotate(to_local, Rotate(aim, {0.0f, 0.0f, -1.0f}));
    // |(o + t d).xz| = radius; the larger root is the wall ahead when starting inside.
    const float a = d.x * d.x + d.z * d.z;
    const float b = 2.0f * (o.x * d.x + o.z * d.z);
    const float c = o.x * o.x + o.z * o.z - radius * radius;
    const float discriminant = b * b - 4.0f * a * c;
    if (a < 1e-6f || discriminant < 0.0f) {
        return false;
    }
    const float t = (-b + std::sqrt(discriminant)) / (2.0f * a);
    if (t <= 0.0f) {
        return false;
    }
    const Vec3 hit = Add(o, Scale(d, t));
    const float angle = std::atan2(hit.x, -hit.z); // 0 at the arc's middle, + to the right
    const float height_m = width_m * float(height) / float(width);
    const float px = (angle / ArcAngle(width_m, radius) + 0.5f) * float(width);
    const float py = (0.5f - hit.y / height_m) * float(height);
    if (inside_only && (px < 0.0f || py < 0.0f || px >= float(width) || py >= float(height))) {
        return false;
    }
    *u = px;
    *v = py;
    if (t_out) {
        *t_out = t;
    }
    return true;
}

/// A laser beam as a thin quad: from `origin` along the unit `dir` for `length`, the quad's +Y
/// along the beam and its face turned toward `eye` (quads are flat, so it is rotated about the
/// beam to stay visible). Size the quad (beam width, length).
inline Pose BeamPose(Vec3 origin, Vec3 dir, float length, Vec3 eye) {
    const Vec3 center = Add(origin, Scale(dir, length * 0.5f));
    Vec3 x = Normalize(Cross(dir, Sub(eye, center)));
    if (Dot(x, x) == 0.0f) { // looking straight down the beam: any side will do
        x = Normalize(Cross(dir, std::fabs(dir.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f}
                                                         : Vec3{1.0f, 0.0f, 0.0f}));
    }
    return {FromBasis(x, dir, Cross(x, dir)), center};
}

} // namespace xrscreen
