// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The HUD layer (docs/vr/findings/25-hud-layer.md): the game draws its 2D UI (HUD, subtitles,
// menus over the scene) into an image of its own instead of the eye image, and the bridge shows
// that image as a quad layer in front of the user, the same image in both eyes. Pure functions,
// no OpenXR or Vulkan: the bridge (bbport_xr_bridge.cpp) and the host (vk_xr.cpp) share the
// settings, bbport_xr_math_test checks them.
//
// Environment:
//   BB_XR_HUD=layer|off|force  layer (default with BB_XR_MODE=alt or mono): the HUD layer; off:
//                              the HUD stays in the game image; force: separate the UI even
//                              without a headset (flat tests: the frame dumps get a "hud" image).
//   BB_XR_HUD_DIST=<m>         distance of the quad (default 2.5)
//   BB_XR_HUD_WIDTH_DEG=<deg>  horizontal angle it covers (default 55)
//   BB_XR_HUD_PITCH_DEG=<deg>  its center above (+) or below (-) the view axis (default -5)
//   BB_XR_HUD_FOLLOW=lock|lazy lock (default): fixed to the head; lazy: stays put in the world
//                              until the head turns more than 20 deg away, then eases back.
//   BB_XR_HUD_ALPHA=premul|straight|opaque  how the runtime blends the quad (default premul: the
//                              image holds color*alpha, layer flags = BLEND_TEXTURE_SOURCE_ALPHA;
//                              straight adds UNPREMULTIPLIED_ALPHA; opaque sets no blend flag).
//                              Live: a file bb_xr_hud_alpha.txt in the working directory holding
//                              one of the words overrides the variable while the game runs.
//   BB_XR_HUD_DEBUG=1          log the alpha of a few HUD pixels (corners, center) now and then.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstring>

#include "xr_screen_math.h"

namespace xrhud {

enum class Mode { Off, Layer, Force };
enum class Follow { Lock, Lazy };

struct Config {
    Mode mode = Mode::Off;
    float distance = 2.5f;   // meters
    float width_deg = 55.0f; // horizontal angle of the quad
    float pitch_deg = -5.0f; // center below the view axis
    Follow follow = Follow::Lock;
};

inline bool Is(const char* value, const char* word) {
    return value && std::strcmp(value, word) == 0;
}

/// The HUD mode for BB_XR_HUD (`hud`, may be null) and BB_XR_MODE (`xr_mode`, may be null):
/// unset or empty turns the layer on in the head-tracked modes (alt, mono), where the HUD in the
/// eye image sits at the edges of the lenses; the cinema screen keeps it in the image.
inline Mode ParseMode(const char* hud, const char* xr_mode) {
    if (Is(hud, "off") || Is(hud, "0")) {
        return Mode::Off;
    }
    if (Is(hud, "force")) {
        return Mode::Force;
    }
    if (Is(hud, "layer") || Is(hud, "1")) {
        return Mode::Layer;
    }
    return Is(xr_mode, "alt") || Is(xr_mode, "mono") ? Mode::Layer : Mode::Off;
}

enum class Alpha { Premul, Straight, Opaque };

/// BB_XR_HUD_ALPHA / the live file's word; anything else is the default (premul). Whitespace around
/// the word and a case difference are ignored.
inline bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline Alpha ParseAlpha(const char* value) {
    if (!value) {
        return Alpha::Premul;
    }
    while (IsSpace(*value)) {
        ++value;
    }
    char word[16] = {};
    size_t n = 0;
    while (value[n] && n < sizeof(word) - 1 && !IsSpace(value[n])) {
        word[n] = char(value[n] >= 'A' && value[n] <= 'Z' ? value[n] + 32 : value[n]);
        ++n;
    }
    if (Is(word, "straight") || Is(word, "unpremul")) {
        return Alpha::Straight;
    }
    if (Is(word, "opaque") || Is(word, "none")) {
        return Alpha::Opaque;
    }
    return Alpha::Premul;
}

inline const char* AlphaName(Alpha a) {
    return a == Alpha::Straight ? "straight" : a == Alpha::Opaque ? "opaque" : "premul";
}

inline float ParseFloat(const char* value, float fallback, float low, float high) {
    if (!value || !*value) {
        return fallback;
    }
    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    if (end == value || !std::isfinite(parsed)) {
        return fallback;
    }
    return std::clamp(parsed, low, high);
}

/// The settings from the environment values (each may be null).
inline Config Parse(const char* hud, const char* xr_mode, const char* dist, const char* width,
                    const char* pitch, const char* follow) {
    Config c;
    c.mode = ParseMode(hud, xr_mode);
    c.distance = ParseFloat(dist, c.distance, 0.5f, 20.0f);
    c.width_deg = ParseFloat(width, c.width_deg, 15.0f, 140.0f);
    c.pitch_deg = ParseFloat(pitch, c.pitch_deg, -60.0f, 60.0f);
    c.follow = Is(follow, "lazy") ? Follow::Lazy : Follow::Lock;
    return c;
}

inline Config FromEnvironment() {
    return Parse(std::getenv("BB_XR_HUD"), std::getenv("BB_XR_MODE"),
                 std::getenv("BB_XR_HUD_DIST"), std::getenv("BB_XR_HUD_WIDTH_DEG"),
                 std::getenv("BB_XR_HUD_PITCH_DEG"), std::getenv("BB_XR_HUD_FOLLOW"));
}

constexpr float kDegToRad = 0.01745329252f;

/// A rotation of `radians` about +X (positive pitches the -Z view axis up).
inline xrscreen::Quat PitchQuat(float radians) {
    return {std::sin(radians * 0.5f), 0.0f, 0.0f, std::cos(radians * 0.5f)};
}

/// A rotation of `radians` about +Y (positive turns the -Z view axis to the left).
inline xrscreen::Quat YawQuat(float radians) {
    return {0.0f, std::sin(radians * 0.5f), 0.0f, std::cos(radians * 0.5f)};
}

struct Quad {
    xrscreen::Pose pose; // the middle of the quad, facing the viewer along its +Z
    float width, height; // meters
};

/// The quad relative to the head (VIEW space; head-locked): `distance` out along the view axis
/// pitched by pitch_deg, turned to face the eyes, `width_deg` wide, `aspect` = width / height.
inline Quad HeadQuad(const Config& c, float aspect) {
    const float pitch = c.pitch_deg * kDegToRad;
    Quad q;
    q.pose.orientation = PitchQuat(pitch);
    q.pose.position = xrscreen::Rotate(q.pose.orientation, {0.0f, 0.0f, -c.distance});
    q.width = 2.0f * c.distance * std::tan(c.width_deg * kDegToRad * 0.5f);
    q.height = q.width / (aspect > 0.1f ? aspect : 16.0f / 9.0f);
    return q;
}

/// The view axis (-Z) of an orientation as yaw (about +Y, 0 looking down -Z, positive to the
/// left) and pitch (positive up), radians. Roll is dropped: the lazy HUD stays level.
inline void YawPitch(xrscreen::Quat q, float* yaw, float* pitch) {
    const xrscreen::Vec3 f = xrscreen::Rotate(q, {0.0f, 0.0f, -1.0f});
    *yaw = std::atan2(-f.x, -f.z);
    *pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
}

inline float WrapPi(float a) {
    constexpr float kPi = 3.14159265f;
    while (a > kPi) {
        a -= 2.0f * kPi;
    }
    while (a < -kPi) {
        a += 2.0f * kPi;
    }
    return a;
}

/// Lazy follow: the HUD keeps its direction in the world while the head looks around it; once
/// the head points more than `start_deg` away it eases toward the head direction (time constant
/// 1/rate s) until it is within `stop_deg`. Its position follows the head at once (no parallax).
struct LazyFollow {
    float start_deg = 20.0f, stop_deg = 2.0f, rate = 4.0f;
    bool valid = false, moving = false;
    float yaw = 0.0f, pitch = 0.0f; // the HUD direction (radians)

    void Reset() {
        valid = moving = false;
    }

    /// One frame, `dt` seconds after the previous one, with the head orientation in the world.
    void Update(xrscreen::Quat head, float dt) {
        float head_yaw = 0.0f, head_pitch = 0.0f;
        YawPitch(head, &head_yaw, &head_pitch);
        if (!valid) {
            yaw = head_yaw;
            pitch = head_pitch;
            valid = true;
            moving = false;
            return;
        }
        const float dy = WrapPi(head_yaw - yaw);
        const float dp = head_pitch - pitch;
        const float error = std::sqrt(dy * dy + dp * dp);
        if (error > start_deg * kDegToRad) {
            moving = true;
        }
        if (moving) {
            const float k = 1.0f - std::exp(-std::max(dt, 0.0f) * rate);
            yaw = WrapPi(yaw + dy * k);
            pitch += dp * k;
            if (error * (1.0f - k) < stop_deg * kDegToRad) {
                moving = false;
            }
        }
    }

    /// The quad in the world: at the head position, in the HUD direction (Update first).
    Quad Place(const Config& c, float aspect, xrscreen::Vec3 head_position) const {
        Quad q = HeadQuad(c, aspect);
        const xrscreen::Pose frame{
            xrscreen::Multiply(YawQuat(yaw), PitchQuat(pitch)), head_position};
        q.pose = xrscreen::Compose(frame, q.pose);
        return q;
    }
};

inline const char* ModeName(Mode m) {
    return m == Mode::Layer ? "layer" : m == Mode::Force ? "force" : "off";
}

} // namespace xrhud
