// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport VR DLSS (docs/vr/findings/37-dlss-vr.md): the pure logic, no Vulkan, NGX, threads or I/O,
// so it is unit tested headless (tests/test_xr_dlss.cpp, target xr-dlss-test).
//
//   - option parsing: quality mode, render preset
//   - the output (eye image) size for a render size
//   - backend selection with fallback (DLSS, else FSR 1 + FXAA) and the reason
//   - per-eye temporal bookkeeping for alternate-eye stereo: one DLSS feature (history) per eye,
//     each fed the previous frame of the SAME eye (two game frames back), its own Halton jitter
//     sequence, and the reset rules
//   - the camera motion vector math of host_shaders/camera_motion.comp (CPU copy for tests)

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace XrDlssLogic {

// ---------------------------------------------------------------------------------------------
// Options

/// DLSS quality modes, in the bridge's order (BbDlssFeature::quality).
enum class Quality : int { Dlaa = 0, Quality = 1, Balanced = 2, Performance = 3, Ultra = 4 };

inline bool ParseQuality(std::string_view text, Quality& out) {
    struct Name {
        std::string_view name;
        Quality value;
    };
    static constexpr Name names[] = {
        {"dlaa", Quality::Dlaa},
        {"quality", Quality::Quality},
        {"balanced", Quality::Balanced},
        {"performance", Quality::Performance},
        {"perf", Quality::Performance},
        {"ultra", Quality::Ultra},
        {"ultra_performance", Quality::Ultra},
        {"ultraperformance", Quality::Ultra},
    };
    for (const auto& n : names) {
        if (n.name == text) {
            out = n.value;
            return true;
        }
    }
    return false;
}

inline const char* QualityName(Quality q) {
    switch (q) {
    case Quality::Dlaa:
        return "dlaa";
    case Quality::Quality:
        return "quality";
    case Quality::Balanced:
        return "balanced";
    case Quality::Performance:
        return "performance";
    case Quality::Ultra:
        return "ultra";
    }
    return "?";
}

/// Output / render size per axis of NVIDIA's standard modes (render scale 1, 0.667, 0.58, 0.5,
/// 0.333).
inline float QualityScale(Quality q) {
    switch (q) {
    case Quality::Dlaa:
        return 1.0f;
    case Quality::Quality:
        return 1.5f;
    case Quality::Balanced:
        return 1.0f / 0.58f;
    case Quality::Performance:
        return 2.0f;
    case Quality::Ultra:
        return 3.0f;
    }
    return 1.0f;
}

/// The mode for an output/render ratio (the size came from BB_XR_EYE_RES): the same thresholds
/// as the flat game's Dlss::QualityForScale.
inline Quality QualityForScale(float scale) {
    return scale >= 2.9f    ? Quality::Ultra
           : scale >= 1.95f ? Quality::Performance
           : scale >= 1.65f ? Quality::Balanced
           : scale > 1.01f  ? Quality::Quality
                            : Quality::Dlaa;
}

/// NVSDK_NGX_DLSS_Hint_Render_Preset: "default"/"0" -> 0, a letter -> its value (A = 1 ... O = 15),
/// or a number 0..15. Only default, J, K, L, M (and the deprecated E, F) select a model in SDK
/// 310.9.1; the others make the driver use its default. `usable` tells which.
inline bool ParsePreset(std::string_view text, uint32_t& out, bool* usable = nullptr) {
    uint32_t value = 0;
    if (text.empty() || text == "default" || text == "0") {
        value = 0;
    } else if (text.size() == 1 && ((text[0] >= 'A' && text[0] <= 'O') ||
                                    (text[0] >= 'a' && text[0] <= 'o'))) {
        const char c = char(text[0] & ~0x20);
        value = uint32_t(c - 'A') + 1;
    } else {
        uint32_t n = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return false;
            }
            n = n * 10 + uint32_t(c - '0');
            if (n > 15) {
                return false;
            }
        }
        value = n;
    }
    out = value;
    if (usable) {
        *usable = value == 0 || value == 5 || value == 6 || (value >= 10 && value <= 13);
    }
    return true;
}

inline char PresetLetter(uint32_t preset) {
    return preset == 0 || preset > 15 ? '-' : char('A' + preset - 1);
}

// ---------------------------------------------------------------------------------------------
// Output size

/// The DLSS output (eye image) size for a render (display buffer) size: an explicit quality mode
/// scales the render size; without one the BB_XR_EYE_RES size (`res_w` x `res_h`, already
/// resolved, as for FSR 1) is used. Clamped to 8192 and never below the render size.
inline void OutputSize(uint32_t render_w, uint32_t render_h, bool quality_set, Quality quality,
                       uint32_t res_w, uint32_t res_h, uint32_t& out_w, uint32_t& out_h) {
    out_w = out_h = 0;
    if (!render_w || !render_h) {
        return;
    }
    if (quality_set || !res_w || !res_h) {
        const float s = QualityScale(quality_set ? quality : Quality::Performance);
        out_w = uint32_t(float(render_w) * s + 0.5f);
        out_h = uint32_t(float(render_h) * s + 0.5f);
    } else {
        out_w = res_w;
        out_h = res_h;
    }
    out_w = std::clamp(out_w, render_w, 8192u);
    out_h = std::clamp(out_h, render_h, 8192u);
}

/// The mode NGX is created with for a feature of these sizes (the explicit one, or from the ratio).
inline Quality FeatureQuality(bool quality_set, Quality quality, uint32_t render_w,
                              uint32_t out_w) {
    if (quality_set) {
        return quality;
    }
    return QualityForScale(render_w ? float(out_w) / float(render_w) : 1.0f);
}

// ---------------------------------------------------------------------------------------------
// Backend selection (graceful fallback)

struct Request {
    bool dlss = false; ///< upscale=dlss
    bool fsr1 = false; ///< upscale=fsr1
    bool fxaa = false; ///< aa=fxaa
};

struct System {
    bool dlls_present = false;   ///< bbport_dlss.dll + nvngx_dlss.dll next to the executable
    bool bridge_slots = false;   ///< the bridge has per-eye features (ABI >= 2)
    bool dlss_ready = false;     ///< NGX initialised, DLSS SR available on this GPU/driver
    bool motion_vectors = false; ///< camera motion is tracked (BB_UPSCALER is not "none")
    bool flat_temporal = false;  ///< the flat game's temporal upscaler/jitter is active
    bool failed = false;         ///< DLSS failed at run time (feature creation/evaluation)
    bool hdr_output = false;     ///< the frame is HDR (10-bit): DLSS runs on LDR only
    const char* problem = "";    ///< Dlss::Problem() when !dlss_ready
};

struct Choice {
    bool dlss = false;
    bool fsr1 = false; ///< upscale the frame with FSR 1 (EASU, + RCAS)
    bool fxaa = false;
    const char* reason = ""; ///< why DLSS is not used (empty when it is, or was not asked for)
};

inline Choice Select(const Request& r, const System& s) {
    Choice c{};
    if (!r.dlss) {
        c.fsr1 = r.fsr1;
        c.fxaa = r.fxaa;
        return c;
    }
    const char* reason = !s.dlls_present ? "bbport_dlss.dll / nvngx_dlss.dll not next to the exe"
                         : !s.dlss_ready  ? (s.problem && *s.problem ? s.problem
                                                                     : "DLSS is not available")
                         : !s.bridge_slots ? "bbport_dlss.dll is too old (no per-eye features)"
                         : !s.motion_vectors ? "BB_UPSCALER=none (no camera motion vectors)"
                         : s.flat_temporal   ? "the flat game's temporal upscaler is on"
                         : s.failed          ? "DLSS failed at run time"
                         : s.hdr_output      ? "HDR output"
                                             : nullptr;
    if (!reason) {
        c.dlss = true; // DLSS anti-aliases: no FXAA on top
        return c;
    }
    // Fallback: the same output size through FSR 1, with FXAA for the edges DLSS would have done.
    c.fsr1 = true;
    c.fxaa = true;
    c.reason = reason;
    return c;
}

// ---------------------------------------------------------------------------------------------
// Jitter

inline float Halton(uint32_t index, uint32_t base) {
    float f = 1.0f, result = 0.0f;
    for (uint32_t i = index; i > 0; i /= base) {
        f /= float(base);
        result += f * float(i % base);
    }
    return result;
}

/// NVIDIA's recommendation: at least 8 * (output / render)^2 phases (per eye here).
inline uint32_t JitterPhases(float scale) {
    const double s = std::max(1.0, double(scale));
    return uint32_t(std::clamp(std::ceil(8.0 * s * s), 8.0, 128.0));
}

/// Phase `n` (1-based) of the sequence: a sub-pixel offset in render pixels, in [-0.5, 0.5).
inline std::array<float, 2> JitterAt(uint32_t n) {
    return {Halton(n, 2) - 0.5f, Halton(n, 3) - 0.5f};
}

// ---------------------------------------------------------------------------------------------
// Cameras and motion vectors (host_shaders/camera_motion.comp)

/// A frame's camera from Bloodborne's scene constants.
struct Camera {
    std::array<float, 12> view{};     ///< world to view, 3x4 rows [R | t]
    std::array<float, 12> inv_view{}; ///< view to world, 3x4 rows
    std::array<float, 4> proj{};      ///< x scale, y scale, z scale, z offset (D3D depth 0..1)
};

/// a * b for 3x4 affine matrices (rows [R | t]).
inline std::array<float, 12> Multiply(const std::array<float, 12>& a,
                                      const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

/// The reprojection the motion pass takes: previous view * inverse current view.
inline std::array<float, 12> Reproject(const Camera& current, const Camera& previous) {
    return Multiply(previous.view, current.inv_view);
}

/// The motion vector of one pixel in render pixels, previous minus current position (DLSS:
/// MVScale 1, low-res MVs, no jitter in them), exactly as camera_motion.comp computes it.
inline std::array<float, 2> MotionVector(float px, float py, float depth, float width, float height,
                                         std::array<float, 2> jitter,
                                         const std::array<float, 12>& reproject,
                                         const std::array<float, 4>& proj,
                                         const std::array<float, 4>& prev_proj) {
    const float u = (px + 0.5f) / width - jitter[0] / width;
    const float v = (py + 0.5f) / height - jitter[1] / height;
    const float ndc_x = u * 2.0f - 1.0f, ndc_y = 1.0f - v * 2.0f;
    const bool sky = depth >= 1.0f;
    const float denom = std::min(depth - proj[2], -1e-8f);
    const float z = sky ? 1.0f : proj[3] / denom;
    const float view[4] = {ndc_x * z / proj[0], ndc_y * z / proj[1], z, sky ? 0.0f : 1.0f};
    float prev[3];
    for (int r = 0; r < 3; ++r) {
        prev[r] = reproject[r * 4 + 0] * view[0] + reproject[r * 4 + 1] * view[1] +
                  reproject[r * 4 + 2] * view[2] + reproject[r * 4 + 3] * view[3];
    }
    const float w = std::max(prev[2], 1e-4f);
    const float pu = prev[0] * prev_proj[0] / w * 0.5f + 0.5f;
    const float pv = 0.5f - prev[1] * prev_proj[1] / w * 0.5f;
    return {(pu - u) * width, (pv - v) * height};
}

/// Bias-current-colour mask (the player's arms and weapon, host_shaders/xr_dlss_mask.comp):
/// view-space z of a hardware depth value (D3D projection: depth = zs + zo / z).
inline float LinearDepth(float depth, const std::array<float, 4>& proj) {
    return proj[3] / std::min(depth - proj[2], -1e-8f);
}

/// Distance from the camera of the surface at a pixel of the (unjittered) scene uv.
inline float ViewDistance(float depth, float scene_u, float scene_v,
                          const std::array<float, 4>& proj) {
    const float z = LinearDepth(depth, proj);
    const float x = (scene_u * 2.0f - 1.0f) * z / proj[0];
    const float y = (1.0f - scene_v * 2.0f) * z / proj[1];
    return std::sqrt(x * x + y * y + z * z);
}

/// 1 = trust the current colour: a surface closer than `threshold_m` (0 = mask off). Sky and
/// cleared depth (>= 1) never.
inline bool NearMask(float depth, float scene_u, float scene_v, const std::array<float, 4>& proj,
                     float threshold_m) {
    if (threshold_m <= 0.0f || depth >= 1.0f) {
        return false;
    }
    return LinearDepth(depth, proj) > 0.0f &&
           ViewDistance(depth, scene_u, scene_v, proj) < threshold_m;
}

/// dlss_mask_m: metres, 0 = off; a bad value keeps the default; capped at 5 m.
inline float ParseMaskMeters(std::string_view text, float fallback) {
    std::string copy{text};
    char* end = nullptr;
    const float v = std::strtof(copy.c_str(), &end);
    if (copy.empty() || end == copy.c_str() || !std::isfinite(v) || v < 0.0f) {
        return fallback;
    }
    return std::min(v, 5.0f);
}

/// Camera position (world) of a view [R | t]: -R^T t.
inline std::array<float, 3> Position(const std::array<float, 12>& view) {
    std::array<float, 3> p{};
    for (int k = 0; k < 3; ++k) {
        for (int i = 0; i < 3; ++i) {
            p[k] -= view[i * 4 + 3] * view[i * 4 + k];
        }
    }
    return p;
}

/// The cosine of the largest rotation between two views' axes (1 = same orientation).
inline float AxisCosine(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    float worst = 1.0f;
    for (int i = 0; i < 3; ++i) {
        float d = 0.0f, la = 0.0f, lb = 0.0f;
        for (int j = 0; j < 3; ++j) {
            d += a[i * 4 + j] * b[i * 4 + j];
            la += a[i * 4 + j] * a[i * 4 + j];
            lb += b[i * 4 + j] * b[i * 4 + j];
        }
        const float l = std::sqrt(la * lb);
        worst = std::min(worst, l > 0.0f ? d / l : -1.0f);
    }
    return worst;
}

// ---------------------------------------------------------------------------------------------
// Per-eye temporal bookkeeping

struct Config {
    uint32_t max_frame_gap = 4;     ///< game frames since that eye's last frame (2 with alt eyes)
    float max_move = 1.5f;          ///< meters between that eye's frames: more is a cut
    float min_axis_cosine = 0.70f;  ///< ~45 deg between that eye's frames: more is a cut
};

/// What a frame's DLSS evaluation gets.
struct EyeFrame {
    int slot = 0;          ///< 0 left (or the only eye), 1 right
    bool reset = true;     ///< drop that eye's history
    Camera previous{};     ///< that eye's previous camera (the current one when reset)
    std::array<float, 2> jitter{};
    float frame_ms = 0.0f; ///< since that eye's previous frame (0 when unknown)
    const char* reason = ""; ///< why it was reset
};

/// Two eye histories. GPU thread only (one caller); no locking here.
class Tracker {
public:
    explicit Tracker(Config config = {}) : config_{config} {}

    /// The jitter of the next frame's scene draws, chosen once per frame at its first jittered
    /// draw (after the previous frame's eye is known). `stereo`: alternate eyes (the predicted
    /// eye is the other one than the last eye frame), else one stream (eye 0).
    std::array<float, 2> ChooseJitter(bool stereo, float scale) {
        const int eye = stereo ? (last_eye_ < 0 ? 0 : 1 - last_eye_) : 0;
        const uint32_t phases = JitterPhases(scale);
        pending_eye_ = eye;
        pending_ = JitterAt(eyes_[eye].jitter_seq % phases + 1);
        return pending_;
    }

    /// A finished frame went to `eye` (Kind::Eye, or eye 0 without stereo) at game frame
    /// `frame`, rendered with `camera`, `jitter` (what its draws really used) at `time_ms`.
    EyeFrame OnEyeFrame(int eye, const Camera& camera, std::array<float, 2> jitter, uint64_t frame,
                        double time_ms) {
        eye &= 1;
        EyeState& e = eyes_[eye];
        EyeFrame out{};
        out.slot = eye;
        out.jitter = jitter;
        out.previous = camera;
        out.reset = true;
        if (e.force_reset) {
            out.reason = "settings changed";
        } else if (!e.valid) {
            out.reason = "first frame of this eye";
        } else if (frame - e.frame > config_.max_frame_gap) {
            out.reason = "frame gap";
        } else {
            const auto a = Position(camera.view), b = Position(e.camera.view);
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > config_.max_move) {
                out.reason = "camera cut (moved)";
            } else if (AxisCosine(camera.view, e.camera.view) < config_.min_axis_cosine) {
                out.reason = "camera cut (turned)";
            } else {
                out.reset = false;
                out.previous = e.camera;
                out.frame_ms = float(time_ms - e.time_ms);
            }
        }
        if (pending_eye_ == eye) {
            ++e.jitter_seq; // this eye's sequence moves on only with its own frames
        }
        pending_eye_ = -1;
        e.valid = true;
        e.force_reset = false;
        e.camera = camera;
        e.frame = frame;
        e.time_ms = time_ms;
        last_eye_ = eye;
        ++eye_frames_[eye];
        resets_[eye] += out.reset ? 1 : 0;
        return out;
    }

    /// A frame that went to no eye: Hold (both eyes keep their image: histories untouched) or
    /// Mono / no camera (menus, loading: the histories are stale afterwards).
    void OnOtherFrame(bool stale) {
        pending_eye_ = -1;
        if (stale) {
            Reset();
            last_eye_ = -1;
        }
    }

    /// Drop both histories (settings or size changed, features recreated, DLSS toggled).
    void Reset() {
        for (auto& e : eyes_) {
            e.force_reset = true;
        }
    }

    [[nodiscard]] int LastEye() const noexcept { return last_eye_; }
    [[nodiscard]] int PendingEye() const noexcept { return pending_eye_; }
    [[nodiscard]] std::array<float, 2> PendingJitter() const noexcept { return pending_; }
    [[nodiscard]] uint32_t JitterSeq(int eye) const noexcept { return eyes_[eye & 1].jitter_seq; }
    [[nodiscard]] uint64_t EyeFrames(int eye) const noexcept { return eye_frames_[eye & 1]; }
    [[nodiscard]] uint64_t Resets(int eye) const noexcept { return resets_[eye & 1]; }

private:
    struct EyeState {
        bool valid = false;
        bool force_reset = false;
        Camera camera{};
        uint64_t frame = 0;
        double time_ms = 0.0;
        uint32_t jitter_seq = 0;
    };
    Config config_;
    std::array<EyeState, 2> eyes_{};
    int last_eye_ = -1;
    int pending_eye_ = -1;
    std::array<float, 2> pending_{};
    std::array<uint64_t, 2> eye_frames_{};
    std::array<uint64_t, 2> resets_{};
};

// ---------------------------------------------------------------------------------------------
// Colour channel order

/// The VkFormat (UNORM, no sRGB decode: DLSS works on the encoded values) DLSS reads the display
/// buffer through. It must be the format the presenter samples the buffer in (the video-out pixel
/// format decides it), not the image's own format: the two can differ in channel order, and then
/// reading the image natively swaps red and blue (blue skin and leather). Numbers are VkFormat.
inline uint32_t DlssColorFormat(uint32_t presenter_sample_format) {
    constexpr uint32_t kR8G8B8A8Unorm = 37, kR8G8B8A8Srgb = 43, kB8G8R8A8Unorm = 44,
                       kB8G8R8A8Srgb = 50;
    if (presenter_sample_format == kR8G8B8A8Srgb)
        return kR8G8B8A8Unorm;
    if (presenter_sample_format == kB8G8R8A8Srgb)
        return kB8G8R8A8Unorm;
    return presenter_sample_format;
}

} // namespace XrDlssLogic
