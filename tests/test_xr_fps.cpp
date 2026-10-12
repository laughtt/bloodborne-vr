// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR frame rate counter and bottleneck verdict (gpu/shadps4/video_core/renderer_vulkan/
// xr_fps_logic.h): colour thresholds, smoothing, per-frame averages, verdict, bitmap text.
// Headless: no Vulkan, no game.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "gpu/shadps4/video_core/renderer_vulkan/xr_fps_logic.h"

using namespace XrFpsLogic;

namespace {

bool Near(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

void TestColors() {
    // Display 72 Hz: green from 71, yellow from 57.6, red below.
    assert(Classify(72.0, 72.0) == Color::Green);
    assert(Classify(75.0, 72.0) == Color::Green);
    assert(Classify(71.0, 72.0) == Color::Green);
    assert(Classify(70.9, 72.0) == Color::Yellow);
    assert(Classify(62.0, 72.0) == Color::Yellow);
    assert(Classify(57.6, 72.0) == Color::Yellow);
    assert(Classify(57.5, 72.0) == Color::Red);
    assert(Classify(0.0, 72.0) == Color::Red);
    // 90 and 120 Hz headsets.
    assert(Classify(89.0, 90.0) == Color::Green);
    assert(Classify(72.0, 90.0) == Color::Yellow);
    assert(Classify(71.9, 90.0) == Color::Red);
    assert(Classify(119.0, 120.0) == Color::Green);
    // Unknown display rate.
    assert(Classify(60.0, 0.0) == Color::Unknown);
    assert(Classify(60.0, -1.0) == Color::Unknown);
    // The colours differ.
    const Rgb g = ColorRgb(Color::Green), y = ColorRgb(Color::Yellow), r = ColorRgb(Color::Red);
    assert(g.g > g.r && y.r > 0.9f && y.g > 0.7f && r.r > r.g);
}

void TestSmoothing() {
    RateMeter m;
    assert(!m.Valid());
    assert(!m.Push(10.0, 0));  // start
    assert(!m.Push(10.2, 12)); // under the interval: no sample
    assert(!m.Valid());
    assert(m.Push(10.5, 31)); // 31 frames in 0.5 s = 62
    assert(m.Valid() && Near(m.Value(), 62.0));
    // Same rate: stays.
    assert(m.Push(11.0, 62));
    assert(Near(m.Value(), 62.0));
    // A drop to 40 fps moves half way (alpha 0.5), then on.
    assert(m.Push(11.5, 82));
    assert(Near(m.Value(), 51.0));
    assert(m.Push(12.0, 102));
    assert(Near(m.Value(), 45.5));
    // Samples closer than the interval are ignored and do not lose frames.
    assert(!m.Push(12.2, 110));
    assert(m.Push(12.5, 122)); // 20 frames over 0.5 s since 12.0
    assert(Near(m.Value(), 42.75));
    // A long gap (loading screen) restarts the average.
    assert(m.Push(20.0, 122 + 720)); // 720 frames in 7.5 s = 96
    assert(Near(m.Value(), 96.0));
}

void TestVerdict() {
    const auto ms = [](double v) { return std::uint64_t(v * 1e6); };
    // 620 flips in 10 s: 16.13 ms a frame.
    Window w;
    w.seconds = 10.0;
    w.flips = 620;
    const auto set = [&](double guest, double gpu, double frame, double present = 0.0) {
        w.guest_wait_ns = ms(guest) * w.flips;
        w.gpu_wait_ns = ms(gpu) * w.flips;
        w.frame_wait_ns = ms(frame) * w.flips;
        w.present_ns = ms(present) * w.flips;
        w.xr_ns = 0;
        w.queue_ns = 0;
        w.gpu_ms_per_frame = -1.0;
    };
    // The command thread mostly works or waits for the guest: CPU.
    set(3.0, 0.5, 0.2);
    PerFrame p = Analyze(w, 72.0);
    assert(Near(p.fps, 62.0) && Near(p.frame_ms, 10000.0 / 620.0));
    assert(p.verdict == Verdict::Cpu);
    assert(Near(p.work_ms, p.frame_ms - 3.7, 1e-3));
    // It waits for the GPU for a third of the frame.
    set(1.0, 5.4, 0.5);
    assert(Analyze(w, 72.0).verdict == Verdict::Gpu);
    // It waits for a free presentation frame.
    set(1.0, 0.5, 6.0);
    assert(Analyze(w, 72.0).verdict == Verdict::Pacing);
    // Both wait: the larger wins.
    set(0.0, 4.0, 5.0);
    assert(Analyze(w, 72.0).verdict == Verdict::Pacing);
    set(0.0, 5.0, 4.0);
    assert(Analyze(w, 72.0).verdict == Verdict::Gpu);
    // The presenter itself fills the frame.
    set(1.0, 0.5, 0.5, 15.0);
    assert(Analyze(w, 72.0).verdict == Verdict::Pacing);
    // A measured GPU time that fills the frame.
    set(1.0, 0.5, 0.5);
    w.gpu_ms_per_frame = 15.0;
    assert(Analyze(w, 72.0).verdict == Verdict::Gpu);
    w.gpu_ms_per_frame = 9.0;
    assert(Analyze(w, 72.0).verdict == Verdict::Cpu);
    // At the display rate nothing else limits it, whatever the waits are.
    w.flips = 719;
    w.seconds = 10.0;
    set(0.0, 6.0, 0.0);
    assert(Analyze(w, 72.0).verdict == Verdict::Pacing);
    // Unknown display rate: the waits decide.
    assert(Analyze(w, 0.0).verdict == Verdict::Gpu);
    // No frames: no division by zero.
    Window none;
    assert(Analyze(none, 72.0).frame_ms == 0.0);
    assert(std::strcmp(VerdictName(Verdict::Gpu), "GPU-bound") == 0);
    assert(std::strcmp(VerdictName(Verdict::Cpu), "CPU-bound") == 0);
    assert(std::strcmp(VerdictName(Verdict::Pacing), "pacing-bound") == 0);
}

void TestText() {
    char buf[32];
    FormatRate(buf, sizeof(buf), 61.6, 72.0);
    assert(std::strcmp(buf, "62/72") == 0);
    FormatRate(buf, sizeof(buf), 61.6, 0.0);
    assert(std::strcmp(buf, "62") == 0);
    FormatRate(buf, sizeof(buf), -3.0, 90.0);
    assert(std::strcmp(buf, "0/90") == 0);
    FormatDetail(buf, sizeof(buf), 16.13, Verdict::Gpu);
    assert(std::strcmp(buf, "16.1MS GPU") == 0);
    // Every character the counter can print has a glyph of its own (not the unknown box).
    const std::uint8_t* box = Glyph('~');
    for (const char* c = "0123456789./- ACEFGHMPSUZ"; *c; ++c) {
        if (*c != ' ') {
            assert(std::memcmp(Glyph(*c), box, kGlyphH) != 0);
        }
    }
    assert(Glyph('g') == Glyph('G'));
    // Width: n characters of 5 pixels and n - 1 gaps.
    assert(TextWidth("", 3) == 0);
    assert(TextWidth("8", 1) == 5);
    assert(TextWidth("62/72", 2) == (5 * 6 - 1) * 2);
    // '8' is 7 rows; row 0 is one run of 3 pixels, row 1 two runs: 1 + 2 + 2 + 1 + 2 + 2 + 1 runs.
    Rect rects[64];
    std::size_t n = TextRects("8", 1, 10, 20, rects, 64);
    assert(n == 11);
    assert(rects[0].x == 11 && rects[0].y == 20 && rects[0].w == 3 && rects[0].h == 1);
    // Scaled and shifted: every rectangle inside the text box.
    n = TextRects("62/72 FPS", 3, 100, 50, rects, 64);
    const int width = TextWidth("62/72 FPS", 3);
    assert(n > 20 && n < 64);
    for (std::size_t i = 0; i < n; ++i) {
        assert(rects[i].x >= 100 && rects[i].x + rects[i].w <= 100 + width);
        assert(rects[i].y >= 50 && rects[i].y + rects[i].h <= 50 + kGlyphH * 3);
        assert(rects[i].w % 3 == 0 && rects[i].h == 3);
    }
    // A full buffer is not overrun.
    assert(TextRects("62/72 FPS", 3, 0, 0, rects, 5) == 5);
    // Corners and switches.
    assert(ParseCorner("tl", 1) == 0 && ParseCorner("TR", 0) == 1 && ParseCorner("bl", 1) == 2 &&
           ParseCorner("br", 1) == 3 && ParseCorner("x", 1) == 1 && ParseCorner(nullptr, 2) == 2);
    assert(ParseOn("1") && ParseOn("on") && ParseOn("true") && ParseOn("yes"));
    assert(!ParseOn("0") && !ParseOn("off") && !ParseOn("") && !ParseOn(nullptr) &&
           !ParseOn("false"));
}

} // namespace

int main() {
    TestColors();
    TestSmoothing();
    TestVerdict();
    TestText();
    std::printf("xr-fps-test: ok\n");
    return 0;
}
