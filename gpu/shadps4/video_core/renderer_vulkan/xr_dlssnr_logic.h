// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport VR DLSS-NR (docs/vr/findings/42-dlss-nr.md, vk_xr_dlssnr.h): the pure logic, no Vulkan,
// NGX, threads or I/O, so it is unit tested headless (tests/test_xr_dlssnr.cpp, target
// xr-dlssnr-test).
//
//   - the settings and their parsing (environment and BB_XR_IQ_CONTROL keys)
//   - the region of the eye image each pass works on (centred, nested)
//   - which frames are processed, in which history (stream), and the feature slot per pass

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace XrDlssNrLogic {

constexpr int MaxPasses = 3;
constexpr int Streams = 2; // eye L (or the only image), eye R
constexpr uint32_t MinRegion = 64;

/// Where DLSS-NR runs (dlssnr_place):
///   Post: on the finished eye image (output size, after the upscaler and the post pass), in place.
///   Pre:  on the render-size image the upscaler reads (the display buffer; true stereo's own eye
///         R image), before DLSS Super Resolution (or FSR 1 / FXAA): its output is the
///         upscaler's input. Hybrid stereo runs it once, on eye L before the warp, so eye R is
///         warped from the processed image (both eyes carry the same invented detail).
enum class Place : int { Post = 0, Pre = 1 };

struct Settings {
    bool on = false;      ///< dlssnr (off by default)
    Place place = Place::Post; ///< dlssnr_place: post (default) or pre
    int passes = 1;       ///< dlssnr_passes, 1..3: the result goes through it again
    int style = 0;        ///< dlssnr_style, 0..2
    // Strength defaults: what the first headset test chose (findings/42 section 3); the DLL's
    // own GUI defaults (1, 0.2, 0.7, 0.5) changed the image too little to notice in VR.
    float intensity = 2.0f; ///< dlssnr_intensity, 0..2 (the DLL's own GUI stops at 1)
    float tone = 0.6f;      ///< dlssnr_tone: local tone strength
    float structure = 1.0f; ///< dlssnr_structure: local structure strength
    float skin = 1.0f;      ///< dlssnr_skin: skin structure strength
    float fovea = 0.66f;    ///< dlssnr_fovea: share of the eye image the first pass covers
    float inner = 0.4f;     ///< dlssnr_inner: share passes 2 and 3 cover (at most fovea)
    bool operator==(const Settings&) const = default;
};

/// The environment variables, in the order they are applied, and their control-file keys.
struct EnvKey {
    const char* env;
    const char* key;
};
inline constexpr EnvKey EnvKeys[] = {
    {"BB_XR_DLSSNR", "dlssnr"},
    {"BB_XR_DLSSNR_PASSES", "dlssnr_passes"},
    {"BB_XR_DLSSNR_STYLE", "dlssnr_style"},
    {"BB_XR_DLSSNR_INTENSITY", "dlssnr_intensity"},
    {"BB_XR_DLSSNR_TONE", "dlssnr_tone"},
    {"BB_XR_DLSSNR_STRUCTURE", "dlssnr_structure"},
    {"BB_XR_DLSSNR_SKIN", "dlssnr_skin"},
    {"BB_XR_DLSSNR_FOVEA", "dlssnr_fovea"},
    {"BB_XR_DLSSNR_INNER", "dlssnr_inner"},
    {"BB_XR_DLSSNR_PLACE", "dlssnr_place"},
};

/// "post" / "pre" (also "after" / "before", 0 / 1); false: not a placement (the setting stays).
inline bool ParsePlace(std::string_view value, Place& out) {
    if (value == "post" || value == "after" || value == "0") {
        out = Place::Post;
        return true;
    }
    if (value == "pre" || value == "before" || value == "1") {
        out = Place::Pre;
        return true;
    }
    return false;
}

inline const char* PlaceName(Place p) {
    return p == Place::Pre ? "pre" : "post";
}

/// 1, on, true, yes = on; anything else off.
inline bool ParseOn(std::string_view value) {
    return value == "1" || value == "on" || value == "true" || value == "True" || value == "yes";
}

/// Applies one key; false when the key is not a DLSS-NR key. Numbers out of range are clamped,
/// text that is not a number leaves the setting as it was.
inline bool Apply(Settings& s, std::string_view key, std::string_view value) {
    const auto number = [&](float lo, float hi, float& out) {
        const std::string text(value);
        char* end = nullptr;
        const float f = std::strtof(text.c_str(), &end);
        if (end != text.c_str()) {
            out = std::clamp(f, lo, hi);
        }
    };
    if (key == "dlssnr") {
        s.on = ParseOn(value);
    } else if (key == "dlssnr_passes") {
        float f = float(s.passes);
        number(1.0f, float(MaxPasses), f);
        s.passes = int(f + 0.5f);
    } else if (key == "dlssnr_style") {
        float f = float(s.style);
        number(0.0f, 2.0f, f);
        s.style = int(f + 0.5f);
    } else if (key == "dlssnr_intensity") {
        number(0.0f, 2.0f, s.intensity);
    } else if (key == "dlssnr_tone") {
        number(0.0f, 1.0f, s.tone);
    } else if (key == "dlssnr_structure") {
        number(0.0f, 1.0f, s.structure);
    } else if (key == "dlssnr_skin") {
        number(0.0f, 1.0f, s.skin);
    } else if (key == "dlssnr_fovea") {
        number(0.1f, 1.0f, s.fovea);
    } else if (key == "dlssnr_inner") {
        number(0.1f, 1.0f, s.inner);
    } else if (key == "dlssnr_place") {
        ParsePlace(value, s.place);
    } else {
        return false;
    }
    return true;
}

struct Rect {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Rect&) const = default;
};

/// The centred region covering `fraction` of a width x height image in each direction, its size
/// a multiple of 4 (as VirtualDesktop-OpenXR sizes it) and at least MinRegion, never larger than
/// the image.
inline Rect CentreRegion(uint32_t width, uint32_t height, float fraction) {
    const auto side = [&](uint32_t full) {
        const float f = std::clamp(fraction, 0.0f, 1.0f);
        uint32_t s = (uint32_t(float(full) * f) + 3u) & ~3u;
        s = std::max(s, MinRegion);
        return std::min(s, full);
    };
    Rect r{};
    r.width = side(width);
    r.height = side(height);
    r.x = (width - r.width) / 2;
    r.y = (height - r.height) / 2;
    return r;
}

/// Pass `pass`'s region: the first pass covers `fovea`, the later ones `inner` (never more than
/// the first, so each pass reads only what the one before it wrote).
inline Rect PassRegion(const Settings& s, uint32_t width, uint32_t height, int pass) {
    const float fraction = pass == 0 ? s.fovea : std::min(s.inner, s.fovea);
    return CentreRegion(width, height, fraction);
}

/// The number of passes actually run.
inline int Passes(const Settings& s) {
    return std::clamp(s.passes, 1, MaxPasses);
}

/// The bridge feature slot (its own history) of a stream's pass.
inline uint32_t Slot(int stream, int pass) {
    return uint32_t(stream * MaxPasses + pass);
}

/// What a finished frame image is, for the decision below.
struct FrameInfo {
    bool has_camera = false; ///< a 3D frame (title, menus and loading screens have none)
    bool hdr = false;
    bool alt = false;        ///< alternate-eye stereo (BB_XR_MODE=alt)
    int alt_eye = -1;        ///< with alt: 0/1 the eye it goes to, -1 held, -2 menu/mono
    bool pair = false;       ///< true stereo (BB_XR_MODE=stereo)
    bool pair_eyes = false;  ///< with pair: it goes to both eyes as a pair
};

/// Tests (BB_XR_DLSSNR_FLAT_PAIRS=1): a true-stereo frame with a scene camera that no headset
/// view matched (flat runs) counts as a pair, so a flat run processes both eyes as the headset
/// does (eye R's chain runs every frame) and post and pre can be compared.
inline FrameInfo WithFlatPairs(FrameInfo f, bool flat_pairs) {
    if (flat_pairs && f.pair && f.has_camera) {
        f.pair_eyes = true;
    }
    return f;
}

/// The stream (feature history) of the frame's main image, or -1: not processed. Alternate eyes
/// keep one history per eye (each fed only its own eye's frames); a true-stereo pair's eye L and
/// the flat/mono image (also alternate eyes' mono frames) use stream 0.
inline int StreamFor(const FrameInfo& f) {
    if (!f.has_camera || f.hdr) {
        return -1;
    }
    if (f.alt) {
        // A mono frame (the game camera without eye views: the headset's flat screen) is
        // processed like a flat frame, on stream 0; held frames are not.
        return f.alt_eye == 0 || f.alt_eye == 1 ? f.alt_eye : f.alt_eye == -2 ? 0 : -1;
    }
    if (f.pair) {
        return f.pair_eyes ? 0 : -1;
    }
    return 0;
}

/// Pre-SR: the stream of the frame's render-size image (the display buffer), or -1. As StreamFor,
/// except that true stereo processes every 3D frame, held and flat-screen ones included: eye L's
/// DLSS evaluates those too (to keep its history in step), so its input must not switch between
/// processed and unprocessed frames.
inline int PreStreamFor(const FrameInfo& f) {
    if (!f.has_camera || f.hdr) {
        return -1;
    }
    if (f.pair) {
        return 0;
    }
    return StreamFor(f);
}

/// Pre-SR, true stereo: whether eye R's render-size image gets its own evaluation (stream 1).
/// Full stereo renders eye R (its own image): yes, when eye L's ran. The hybrid warps eye R from
/// eye L's processed image: it carries eye L's result already, so no.
inline bool PreEyeR(bool eye_l_processed, bool eye_r_rendered) {
    return eye_l_processed && eye_r_rendered;
}

/// The placement in use last frame; a change starts every DLSS-NR history over (the features
/// keep a history of differently sized and differently placed images).
class PlaceTracker {
public:
    /// True when `p` differs from the placement of the previous call (not on the first call).
    bool Changed(Place p) {
        const bool changed = seen_ && p != last_;
        seen_ = true;
        last_ = p;
        return changed;
    }

private:
    bool seen_ = false;
    Place last_ = Place::Post;
};

/// Whether DLSS Super Resolution's input is DLSS-NR's output (pre-SR, on). When this changes,
/// SR's history (built from the other kind of input) is reset too.
inline bool SrInputProcessed(const Settings& s) {
    return s.on && s.place == Place::Pre;
}
class SrInputTracker {
public:
    /// True when the input kind differs from the previous call's (not on the first call).
    bool Changed(bool processed) {
        const bool changed = seen_ && processed != last_;
        seen_ = true;
        last_ = processed;
        return changed;
    }

private:
    bool seen_ = false;
    bool last_ = false;
};

/// Vulkan format numbers of the 8-bit colour formats DLSS-NR is given (the swapchain's).
constexpr uint32_t kR8G8B8A8Unorm = 37, kR8G8B8A8Srgb = 43, kB8G8R8A8Unorm = 44,
                   kB8G8R8A8Srgb = 50;

/// The UNORM twin the image is read and written as (the encoded values, as VirtualDesktop-OpenXR
/// hands the swapchain's bytes to it), or 0: not a format it is given.
inline uint32_t WorkFormat(uint32_t format) {
    switch (format) {
    case kR8G8B8A8Unorm:
    case kR8G8B8A8Srgb:
        return kR8G8B8A8Unorm;
    case kB8G8R8A8Unorm:
    case kB8G8R8A8Srgb:
        return kB8G8R8A8Unorm;
    default:
        return 0;
    }
}

/// The history of a stream starts over after a gap (menus, the option switched off, held frames
/// for a while): older frames would be blended into a different picture.
constexpr double ResetGapMs = 250.0;
inline bool NeedsReset(bool feature_new, double last_ms, double now_ms) {
    return feature_new || last_ms < 0.0 || now_ms - last_ms > ResetGapMs;
}
/// The same, and when the placement changed (PlaceTracker): same-sized regions (DLAA, or a
/// region that happens to match) keep their feature, but not their history.
inline bool NeedsReset(bool feature_new, double last_ms, double now_ms, bool place_changed) {
    return place_changed || NeedsReset(feature_new, last_ms, now_ms);
}

} // namespace XrDlssNrLogic
