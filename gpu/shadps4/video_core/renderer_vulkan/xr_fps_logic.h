// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR frame rate counter and bottleneck line (docs/vr/findings/31-true-stereo.md section
// 10.9): pure logic, no Vulkan, tested in tests/test_xr_fps.cpp.
//
//   BB_XR_FPS=1 (or "fps=1" in the BB_XR_IQ_CONTROL file)  a small counter in the HUD layer of
//   the headset: game frames per second / headset Hz, and the frame time with the verdict.
//   Green at >= the display rate - 1, yellow at >= 80 %, red below.
//
//   Every 10 s the log gets a "Pace:" line: the per-frame averages of what the GPU command
//   thread did (waiting for the guest, working, waiting for the GPU, waiting for a free
//   presentation frame) and a one-word verdict.

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace XrFpsLogic {

// ---- colour -------------------------------------------------------------------------------

enum class Color { Unknown, Green, Yellow, Red };

/// Green at >= hz - 1, yellow at >= 80 % of hz, red below; Unknown when the display rate is not
/// known (hz <= 0).
inline Color Classify(double fps, double hz) {
    if (!(hz > 0.0)) {
        return Color::Unknown;
    }
    if (fps >= hz - 1.0) {
        return Color::Green;
    }
    if (fps >= 0.8 * hz) {
        return Color::Yellow;
    }
    return Color::Red;
}

struct Rgb {
    float r, g, b;
};

/// The text colours as they should look on screen (the caller converts for an sRGB target).
inline Rgb ColorRgb(Color c) {
    switch (c) {
    case Color::Green:
        return {0.25f, 0.95f, 0.35f};
    case Color::Yellow:
        return {1.0f, 0.85f, 0.15f};
    case Color::Red:
        return {1.0f, 0.25f, 0.2f};
    default:
        return {0.85f, 0.85f, 0.85f};
    }
}

// ---- smoothed rate ---------------------------------------------------------------------------

/// Frames per second from a running frame count: a sample every `interval` seconds (about 2 per
/// second), smoothed exponentially. A gap of several intervals (a loading screen, a pause) starts
/// the average again instead of dragging the old rate along.
class RateMeter {
public:
    static constexpr double kInterval = 0.5;  ///< s between samples
    static constexpr double kAlpha = 0.5;     ///< weight of a new sample
    static constexpr double kRestartGap = 2.0; ///< s without a sample: forget the average

    /// Call as often as you like with the clock (s) and the total frames so far. Returns true when
    /// the smoothed value changed (a new sample was taken).
    bool Push(double now_s, std::uint64_t total_frames) {
        if (!started_) {
            started_ = true;
            last_time_ = now_s;
            last_count_ = total_frames;
            return false;
        }
        const double dt = now_s - last_time_;
        if (dt < kInterval) {
            return false;
        }
        const double sample = double(total_frames - last_count_) / dt;
        if (!valid_ || dt >= kRestartGap) {
            value_ = sample;
        } else {
            value_ += kAlpha * (sample - value_);
        }
        valid_ = true;
        last_time_ = now_s;
        last_count_ = total_frames;
        return true;
    }
    bool Valid() const {
        return valid_;
    }
    double Value() const {
        return value_;
    }

private:
    bool started_ = false, valid_ = false;
    double last_time_ = 0.0, value_ = 0.0;
    std::uint64_t last_count_ = 0;
};

// ---- bottleneck --------------------------------------------------------------------------------

enum class Verdict { Cpu, Gpu, Pacing };

inline const char* VerdictName(Verdict v) {
    switch (v) {
    case Verdict::Gpu:
        return "GPU-bound";
    case Verdict::Pacing:
        return "pacing-bound";
    default:
        return "CPU-bound";
    }
}
inline const char* VerdictShort(Verdict v) {
    switch (v) {
    case Verdict::Gpu:
        return "GPU";
    case Verdict::Pacing:
        return "PACE";
    default:
        return "CPU";
    }
}

/// Totals over a window (nanoseconds of wall time, all frames of the window).
struct Window {
    double seconds = 0.0;
    std::uint64_t flips = 0;           ///< game frames delivered
    std::uint64_t guest_wait_ns = 0;   ///< command thread idle: waiting for the guest's commands
    std::uint64_t gpu_wait_ns = 0;     ///< command thread waiting for the GPU (BB_FRAMES_AHEAD)
    std::uint64_t frame_wait_ns = 0;   ///< command thread waiting for a free presentation frame
    std::uint64_t queue_ns = 0;        ///< flips waiting for their slot (frame limiter / vblank)
    std::uint64_t present_ns = 0;      ///< the presenter's work per frame (swap thread)
    std::uint64_t xr_ns = 0;           ///< ... of it in the bridge and swapchain calls
    double gpu_ms_per_frame = -1.0;    ///< GPU time of a frame (BB_GPU_PROFILE=1), < 0 unknown
};

struct PerFrame {
    double fps = 0.0;
    double frame_ms = 0.0;
    double guest_wait_ms = 0.0, work_ms = 0.0, gpu_wait_ms = 0.0, frame_wait_ms = 0.0;
    double queue_ms = 0.0, present_ms = 0.0, xr_ms = 0.0;
    double gpu_ms = -1.0;
    Verdict verdict = Verdict::Cpu;
};

/// Per-frame averages and the verdict. `hz` is the display rate (0 unknown).
///  - at the display rate (>= hz - 1): pacing-bound, nothing else limits it;
///  - the command thread waits for the GPU for >= 20 % of the frame (more than it waits for a
///    free frame), or the measured GPU time fills >= 90 % of the frame: GPU-bound;
///  - it waits for a free presentation frame for >= 20 % of the frame, or the presenter is busy
///    for >= 90 % of it: pacing-bound (limiter, headset, swapchain);
///  - otherwise the time goes to the guest and the command thread: CPU-bound.
inline PerFrame Analyze(const Window& w, double hz) {
    PerFrame p;
    if (w.flips == 0 || !(w.seconds > 0.0)) {
        return p;
    }
    const double n = double(w.flips);
    const auto ms = [&](std::uint64_t ns) { return double(ns) / 1e6 / n; };
    p.fps = n / w.seconds;
    p.frame_ms = w.seconds * 1000.0 / n;
    p.guest_wait_ms = ms(w.guest_wait_ns);
    p.gpu_wait_ms = ms(w.gpu_wait_ns);
    p.frame_wait_ms = ms(w.frame_wait_ns);
    p.queue_ms = ms(w.queue_ns);
    p.present_ms = ms(w.present_ns);
    p.xr_ms = ms(w.xr_ns);
    p.gpu_ms = w.gpu_ms_per_frame;
    p.work_ms = std::max(0.0, p.frame_ms - p.guest_wait_ms - p.gpu_wait_ms - p.frame_wait_ms);
    const double gpu_share = p.gpu_wait_ms / p.frame_ms;
    const double pace_share = p.frame_wait_ms / p.frame_ms;
    const bool gpu_full = p.gpu_ms >= 0.0 && p.gpu_ms >= 0.9 * p.frame_ms;
    const bool present_full = p.present_ms >= 0.9 * p.frame_ms;
    if (hz > 0.0 && p.fps >= hz - 1.0) {
        p.verdict = Verdict::Pacing;
    } else if ((gpu_share >= 0.2 && gpu_share >= pace_share) || gpu_full) {
        p.verdict = Verdict::Gpu;
    } else if (pace_share >= 0.2 || present_full) {
        p.verdict = Verdict::Pacing;
    } else {
        p.verdict = Verdict::Cpu;
    }
    return p;
}

// ---- bitmap text -------------------------------------------------------------------------------

constexpr int kGlyphW = 5, kGlyphH = 7;

/// Seven rows of five bits (bit 4 is the left pixel) for the characters the counter prints;
/// lower case is upper case. Unknown characters are a box.
inline const std::uint8_t* Glyph(char ch) {
    struct Entry {
        char c;
        std::uint8_t rows[kGlyphH];
    };
    static const Entry table[] = {
        {' ', {0, 0, 0, 0, 0, 0, 0}},
        {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
        {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
        {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
        {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
        {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
        {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
        {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
        {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
        {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
        {'.', {0, 0, 0, 0, 0, 0x0C, 0x0C}},
        {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
        {'-', {0, 0, 0, 0x1F, 0, 0, 0}},
        {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
        {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
        {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
        {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
        {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
        {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
        {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
        {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
        {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
        {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
        {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    };
    static const Entry unknown{'?', {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F}};
    const char upper = char(std::toupper(static_cast<unsigned char>(ch)));
    for (const Entry& e : table) {
        if (e.c == upper) {
            return e.rows;
        }
    }
    return unknown.rows;
}

struct Rect {
    int x, y, w, h;
};

/// Width in pixels of `text` at `scale` (one pixel column between characters).
inline int TextWidth(const char* text, int scale) {
    int n = 0;
    while (text && text[n]) {
        ++n;
    }
    return n ? (n * (kGlyphW + 1) - 1) * scale : 0;
}

/// The lit pixels of `text` with its top left at (x, y), as rectangles (horizontal runs of a row,
/// scale x scale pixels per font pixel). Returns the rectangles written (at most `capacity`).
inline std::size_t TextRects(const char* text, int scale, int x, int y, Rect* out,
                             std::size_t capacity) {
    std::size_t count = 0;
    int pen = x;
    for (; text && *text; ++text, pen += (kGlyphW + 1) * scale) {
        const std::uint8_t* rows = Glyph(*text);
        for (int row = 0; row < kGlyphH; ++row) {
            int col = 0;
            while (col < kGlyphW) {
                if (!(rows[row] & (0x10 >> col))) {
                    ++col;
                    continue;
                }
                int end = col;
                while (end < kGlyphW && (rows[row] & (0x10 >> end))) {
                    ++end;
                }
                if (count < capacity) {
                    out[count] = {pen + col * scale, y + row * scale, (end - col) * scale, scale};
                }
                ++count;
                col = end;
            }
        }
    }
    return std::min(count, capacity);
}

// ---- text of the counter ---------------------------------------------------------------------

/// "62/72" (rate / display rate), or "62" without a known display rate.
inline void FormatRate(char* out, std::size_t size, double fps, double hz) {
    const int f = int(std::lround(std::max(0.0, std::min(fps, 999.0))));
    if (hz > 0.0) {
        std::snprintf(out, size, "%d/%d", f, int(std::lround(std::min(hz, 999.0))));
    } else {
        std::snprintf(out, size, "%d", f);
    }
}

/// "16.1MS CPU": the frame time and the verdict word.
inline void FormatDetail(char* out, std::size_t size, double frame_ms, Verdict v) {
    std::snprintf(out, size, "%.1fMS %s", std::max(0.0, std::min(frame_ms, 999.9)),
                  VerdictShort(v));
}

/// Where the counter sits: 0 top left, 1 top right, 2 bottom left, 3 bottom right.
inline int ParseCorner(const char* value, int fallback) {
    if (!value) {
        return fallback;
    }
    const char c0 = char(std::tolower(static_cast<unsigned char>(value[0])));
    const char c1 = c0 ? char(std::tolower(static_cast<unsigned char>(value[1]))) : 0;
    if (c0 == 't' && c1 == 'l') return 0;
    if (c0 == 't' && c1 == 'r') return 1;
    if (c0 == 'b' && c1 == 'l') return 2;
    if (c0 == 'b' && c1 == 'r') return 3;
    return fallback;
}

/// "1", "on", "true", "yes" turn it on; anything else off.
inline bool ParseOn(const char* value) {
    if (!value) {
        return false;
    }
    const char c = char(std::tolower(static_cast<unsigned char>(value[0])));
    const bool on_word = c == 'o' && std::tolower(static_cast<unsigned char>(value[1])) == 'n';
    return c == '1' || on_word || c == 't' || c == 'y';
}

} // namespace XrFpsLogic
