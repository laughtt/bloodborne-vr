// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR DLSS logic (gpu/shadps4/video_core/renderer_vulkan/xr_dlss_logic.h): option parsing,
// output size, fallback selection, per-eye jitter and history bookkeeping, motion vector math.
// Headless: no Vulkan, no NGX, no game.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>
#include "gpu/shadps4/video_core/renderer_vulkan/xr_dlss_logic.h"

using namespace XrDlssLogic;

namespace {

bool Near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

/// A camera at `pos` looking down +z (view = R world + t with R = identity, t = -pos), and the
/// game's projection for a vertical FOV and aspect (D3D depth: zs = f/(f-n), zo = -n f/(f-n)).
Camera MakeCamera(float x, float y, float z, float yaw = 0.0f, float fov_y = 1.0f,
                  float aspect = 16.0f / 9.0f) {
    Camera c{};
    const float cs = std::cos(yaw), sn = std::sin(yaw);
    // rows: right, up, forward (rotation about y)
    const float r[3][3] = {{cs, 0.0f, -sn}, {0.0f, 1.0f, 0.0f}, {sn, 0.0f, cs}};
    const float p[3] = {x, y, z};
    for (int i = 0; i < 3; ++i) {
        float t = 0.0f;
        for (int j = 0; j < 3; ++j) {
            c.view[i * 4 + j] = r[i][j];
            c.inv_view[j * 4 + i] = r[i][j]; // R^T
            t -= r[i][j] * p[j];
        }
        c.view[i * 4 + 3] = t;
        c.inv_view[i * 4 + 3] = p[i];
    }
    const float n = 0.2f, f = 3000.0f;
    const float ys = 1.0f / std::tan(fov_y * 0.5f);
    c.proj = {ys / aspect, ys, f / (f - n), -n * f / (f - n)};
    return c;
}

/// Hardware depth of a point at view distance z.
float DepthAt(const Camera& c, float z) {
    return c.proj[2] + c.proj[3] / z;
}

void TestOptions() {
    Quality q{};
    assert(ParseQuality("dlaa", q) && q == Quality::Dlaa);
    assert(ParseQuality("quality", q) && q == Quality::Quality);
    assert(ParseQuality("balanced", q) && q == Quality::Balanced);
    assert(ParseQuality("performance", q) && q == Quality::Performance);
    assert(ParseQuality("ultra", q) && q == Quality::Ultra);
    assert(!ParseQuality("best", q));
    assert(Near(QualityScale(Quality::Quality), 1.5f));
    assert(Near(QualityScale(Quality::Performance), 2.0f));
    assert(QualityForScale(1.0f) == Quality::Dlaa);
    assert(QualityForScale(1.5f) == Quality::Quality);
    assert(QualityForScale(1.72f) == Quality::Balanced);
    assert(QualityForScale(2.0f) == Quality::Performance);
    assert(QualityForScale(3.0f) == Quality::Ultra);

    uint32_t p = 99;
    bool usable = false;
    assert(ParsePreset("default", p, &usable) && p == 0 && usable);
    assert(ParsePreset("K", p, &usable) && p == 11 && usable);
    assert(ParsePreset("m", p, &usable) && p == 13 && usable);
    assert(ParsePreset("L", p, &usable) && p == 12 && usable);
    assert(ParsePreset("J", p, &usable) && p == 10 && usable);
    assert(ParsePreset("A", p, &usable) && p == 1 && !usable); // removed from the SDK
    assert(ParsePreset("12", p, &usable) && p == 12 && usable);
    assert(!ParsePreset("16", p) && !ParsePreset("Z", p) && !ParsePreset("k2", p));
    assert(PresetLetter(11) == 'K' && PresetLetter(0) == '-');
}

void TestOutputSize() {
    uint32_t w = 0, h = 0;
    // No explicit mode: the BB_XR_EYE_RES size (default 2x: 3840x2160).
    OutputSize(1920, 1080, false, Quality::Quality, 3840, 2160, w, h);
    assert(w == 3840 && h == 2160);
    assert(FeatureQuality(false, Quality::Quality, 1920, w) == Quality::Performance);
    // Explicit mode: render size times the mode's scale.
    OutputSize(1920, 1080, true, Quality::Quality, 3840, 2160, w, h);
    assert(w == 2880 && h == 1620);
    assert(FeatureQuality(true, Quality::Quality, 1920, w) == Quality::Quality);
    OutputSize(1920, 1080, true, Quality::Dlaa, 3840, 2160, w, h);
    assert(w == 1920 && h == 1080);
    OutputSize(1920, 1080, true, Quality::Ultra, 0, 0, w, h);
    assert(w == 5760 && h == 3240);
    OutputSize(2560, 1440, true, Quality::Ultra, 0, 0, w, h);
    assert(w == 7680 && h == 4320);
    OutputSize(4000, 2000, true, Quality::Ultra, 0, 0, w, h);
    assert(w == 8192 && h == 6000); // clamped
    // Never below the render size (an explicit res smaller than the frame).
    OutputSize(1920, 1080, false, Quality::Quality, 1280, 720, w, h);
    assert(w == 1920 && h == 1080);
    OutputSize(0, 0, true, Quality::Quality, 0, 0, w, h);
    assert(w == 0 && h == 0);
}

void TestSelect() {
    Request r{.dlss = true, .fsr1 = false, .fxaa = false};
    System ok{.dlls_present = true, .bridge_slots = true, .dlss_ready = true,
              .motion_vectors = true};
    Choice c = Select(r, ok);
    assert(c.dlss && !c.fsr1 && !c.fxaa && !*c.reason);

    System s = ok;
    s.dlls_present = false;
    c = Select(r, s);
    assert(!c.dlss && c.fsr1 && c.fxaa && std::string_view(c.reason).find("nvngx") != std::string_view::npos);

    s = ok;
    s.dlss_ready = false;
    s.problem = "not an NVIDIA GPU";
    c = Select(r, s);
    assert(!c.dlss && c.fsr1 && c.fxaa && std::string_view(c.reason) == "not an NVIDIA GPU");

    s = ok;
    s.bridge_slots = false;
    assert(!Select(r, s).dlss);
    s = ok;
    s.motion_vectors = false;
    assert(!Select(r, s).dlss);
    s = ok;
    s.flat_temporal = true;
    assert(!Select(r, s).dlss);
    s = ok;
    s.failed = true;
    assert(!Select(r, s).dlss);
    s = ok;
    s.hdr_output = true;
    assert(!Select(r, s).dlss);

    // Not asked for: the request passes through.
    c = Select({.dlss = false, .fsr1 = true, .fxaa = true}, ok);
    assert(!c.dlss && c.fsr1 && c.fxaa && !*c.reason);
    c = Select({.dlss = false, .fsr1 = false, .fxaa = false}, ok);
    assert(!c.dlss && !c.fsr1 && !c.fxaa);
}

void TestJitter() {
    assert(JitterPhases(1.0f) == 8);
    assert(JitterPhases(1.5f) == 18);
    assert(JitterPhases(2.0f) == 32);
    assert(JitterPhases(3.0f) == 72);
    // Halton(2,3) in [-0.5, 0.5), distinct phases.
    std::set<std::pair<int, int>> seen;
    for (uint32_t n = 1; n <= 32; ++n) {
        const auto j = JitterAt(n);
        assert(j[0] >= -0.5f && j[0] < 0.5f && j[1] >= -0.5f && j[1] < 0.5f);
        seen.insert({int(std::lround(j[0] * 1e4f)), int(std::lround(j[1] * 1e4f))});
    }
    assert(seen.size() == 32);
    assert(Near(JitterAt(1)[0], 0.0f) && Near(JitterAt(1)[1], 1.0f / 3.0f - 0.5f));

    // Why per eye: one global sequence over L R L R gives each eye every other index, and base 2
    // at odd indices always has its first digit set: the left eye's x jitter stays in [0, 0.5).
    for (uint32_t n = 1; n < 64; n += 2) {
        assert(JitterAt(n)[0] >= 0.0f);
    }

    // Per-eye sequences: alternate eyes, each eye walks its own Halton sequence 1, 2, 3, ...
    Tracker t;
    const Camera cam = MakeCamera(0, 0, 0);
    double ms = 0.0;
    float left_min = 1.0f, left_max = -1.0f;
    for (uint64_t frame = 1; frame <= 16; ++frame) {
        const auto j = t.ChooseJitter(true, 1.0f);
        const int eye = int((frame - 1) & 1); // L R L R
        assert(t.PendingEye() == eye);
        const uint32_t n = t.JitterSeq(eye) % 8 + 1;
        assert(j == JitterAt(n));
        if (eye == 0) {
            left_min = std::min(left_min, j[0]);
            left_max = std::max(left_max, j[0]);
        }
        t.OnEyeFrame(eye, cam, j, frame, ms += 13.9);
    }
    assert(left_min < -0.2f && left_max > 0.2f); // the left eye covers the whole pixel
    assert(t.JitterSeq(0) == 8 && t.JitterSeq(1) == 8);

    // A misprediction (the same eye twice): the frame still gets its real jitter, the other
    // eye's sequence does not move on.
    const auto j = t.ChooseJitter(true, 1.0f); // predicts eye 0 (last was 1)
    assert(t.PendingEye() == 0);
    const uint32_t seq0 = t.JitterSeq(0), seq1 = t.JitterSeq(1);
    const EyeFrame f = t.OnEyeFrame(1, cam, j, 17, ms += 13.9);
    assert(f.slot == 1 && f.jitter == j);
    assert(t.JitterSeq(0) == seq0 && t.JitterSeq(1) == seq1);

    // Without stereo: one stream, eye 0.
    Tracker mono;
    for (uint64_t frame = 1; frame <= 4; ++frame) {
        const auto jm = mono.ChooseJitter(false, 2.0f);
        assert(mono.PendingEye() == 0 && jm == JitterAt(uint32_t(frame)));
        mono.OnEyeFrame(0, cam, jm, frame, double(frame) * 13.9);
    }
}

void TestHistory() {
    Tracker t;
    const Camera l0 = MakeCamera(-0.032f, 1.6f, 0.0f), r0 = MakeCamera(0.032f, 1.6f, 0.0f);
    const Camera l1 = MakeCamera(-0.022f, 1.6f, 0.0f), r1 = MakeCamera(0.042f, 1.6f, 0.0f);
    // First frame of each eye: reset.
    EyeFrame f = t.OnEyeFrame(0, l0, {}, 1, 0.0);
    assert(f.slot == 0 && f.reset);
    f = t.OnEyeFrame(1, r0, {}, 2, 13.9);
    assert(f.slot == 1 && f.reset);
    // Next left frame: its previous camera is the LEFT one two frames back, not the right one.
    f = t.OnEyeFrame(0, l1, {}, 3, 27.8);
    assert(!f.reset && f.previous.view == l0.view);
    assert(Near(f.frame_ms, 27.8f));
    f = t.OnEyeFrame(1, r1, {}, 4, 41.7);
    assert(!f.reset && f.previous.view == r0.view);

    // Hold frames (not shown) leave the histories alone.
    t.OnOtherFrame(false);
    f = t.OnEyeFrame(0, l1, {}, 5, 55.6);
    assert(!f.reset && f.previous.view == l1.view);
    // A frame gap (lost frames) resets that eye.
    f = t.OnEyeFrame(1, r1, {}, 12, 160.0);
    assert(f.reset && std::string_view(f.reason) == "frame gap");
    // Mono / menu frames make both histories stale.
    t.OnOtherFrame(true);
    assert(t.LastEye() == -1);
    f = t.OnEyeFrame(0, l1, {}, 13, 170.0);
    assert(f.reset);
    f = t.OnEyeFrame(1, r1, {}, 14, 180.0);
    assert(f.reset);
    // Camera cuts.
    f = t.OnEyeFrame(0, MakeCamera(5.0f, 1.6f, 0.0f), {}, 15, 190.0);
    assert(f.reset && std::string_view(f.reason) == "camera cut (moved)");
    f = t.OnEyeFrame(1, MakeCamera(0.032f, 1.6f, 0.0f, 1.2f), {}, 16, 200.0);
    assert(f.reset && std::string_view(f.reason) == "camera cut (turned)");
    // A normal head turn between the eye's frames (10 deg) keeps the history.
    f = t.OnEyeFrame(1, MakeCamera(0.032f, 1.6f, 0.0f, 1.2f + 0.17f), {}, 18, 227.0);
    assert(!f.reset);
    // Settings change: both reset once.
    t.Reset();
    assert(t.OnEyeFrame(0, l1, {}, 19, 240.0).reset);
    assert(t.OnEyeFrame(1, r1, {}, 20, 250.0).reset);
    assert(!t.OnEyeFrame(0, l1, {}, 21, 260.0).reset);
    assert(t.EyeFrames(0) == 7 && t.EyeFrames(1) == 7);
}

void TestMotionVectors() {
    const float W = 1920, H = 1080;
    const Camera c = MakeCamera(0, 0, 0);
    // A still camera: zero motion everywhere, sky too.
    {
        const auto rp = Reproject(c, c);
        for (float x : {0.0f, 960.0f, 1919.0f}) {
            const auto mv = MotionVector(x, 500, DepthAt(c, 10.0f), W, H, {}, rp, c.proj, c.proj);
            assert(Near(mv[0], 0) && Near(mv[1], 0));
        }
        const auto sky = MotionVector(100, 100, 1.0f, W, H, {}, rp, c.proj, c.proj);
        assert(Near(sky[0], 0) && Near(sky[1], 0));
    }
    // The jitter is not in the vectors: a jittered still frame still has zero motion.
    {
        const auto rp = Reproject(c, c);
        const auto mv = MotionVector(700, 300, DepthAt(c, 4.0f), W, H, {0.37f, -0.21f}, rp, c.proj,
                                     c.proj);
        assert(Near(mv[0], 0) && Near(mv[1], 0));
    }
    // The camera moved 0.1 m right since the previous frame: a point at 10 m straight ahead was
    // 0.1 m further right in the previous frame's view... i.e. the previous camera was 0.1 m
    // left, so the point appeared shifted right there: previous - current x = +fx * 0.1 / 10.
    {
        const Camera prev = MakeCamera(-0.1f, 0, 0);
        const auto rp = Reproject(c, prev);
        const auto mv = MotionVector(959.5f, 539.5f, DepthAt(c, 10.0f), W, H, {}, rp, c.proj,
                                     prev.proj);
        const float expect = c.proj[0] * 0.1f / 10.0f * 0.5f * W;
        assert(Near(mv[0], expect, 0.05f) && Near(mv[1], 0, 0.05f));
        // Sky (infinitely far) does not move with translation.
        const auto sky = MotionVector(959.5f, 539.5f, 1.0f, W, H, {}, rp, c.proj, prev.proj);
        assert(Near(sky[0], 0) && Near(sky[1], 0));
    }
    // Head turn between the same eye's frames: the sky moves too (rotation only).
    {
        const Camera prev = MakeCamera(0, 0, 0, 0.05f);
        const auto rp = Reproject(c, prev);
        const auto sky = MotionVector(959.5f, 539.5f, 1.0f, W, H, {}, rp, c.proj, prev.proj);
        assert(std::fabs(sky[0]) > 20.0f && Near(sky[1], 0, 0.05f));
    }
    // Why the previous frame of the SAME eye: against the other eye's camera (64 mm IPD) a still
    // scene gets a disparity of tens of pixels at 1 m, which DLSS would smear as motion.
    {
        const Camera left = MakeCamera(-0.032f, 0, 0), right = MakeCamera(0.032f, 0, 0);
        const auto same = MotionVector(959.5f, 539.5f, DepthAt(left, 1.0f), W, H, {},
                                       Reproject(left, left), left.proj, left.proj);
        const auto other = MotionVector(959.5f, 539.5f, DepthAt(left, 1.0f), W, H, {},
                                        Reproject(left, right), left.proj, right.proj);
        assert(Near(same[0], 0) && std::fabs(other[0]) > 30.0f);
    }
}

// The arm/weapon bias mask: depth -> view distance -> near flag (host_shaders/xr_dlss_mask.comp).
void TestMask() {
    const Camera c = MakeCamera(0, 0, 0);
    const float W = 1920.0f, H = 1080.0f;
    // Linearisation round trip on the game's projection (near 0.2, far 3000).
    for (float z : {0.2f, 0.5f, 0.9f, 1.0f, 5.0f, 100.0f}) {
        assert(Near(LinearDepth(DepthAt(c, z), c.proj), z, z * 2e-3f));
    }
    // Screen centre: distance == z, the threshold is a strict "<".
    const float u = 0.5f, v = 0.5f;
    assert(NearMask(DepthAt(c, 0.5f), u, v, c.proj, 0.9f));
    assert(NearMask(DepthAt(c, 0.85f), u, v, c.proj, 0.9f));
    assert(!NearMask(DepthAt(c, 0.95f), u, v, c.proj, 0.9f));
    assert(!NearMask(DepthAt(c, 5.0f), u, v, c.proj, 0.9f));
    // The distance is radial: at the screen corner the same z is farther away.
    const float centre = ViewDistance(DepthAt(c, 0.8f), u, v, c.proj);
    const float corner = ViewDistance(DepthAt(c, 0.8f), 0.0f, 0.0f, c.proj);
    assert(Near(centre, 0.8f, 2e-3f) && corner > 0.8f * 1.2f);
    assert(NearMask(DepthAt(c, 0.8f), u, v, c.proj, 0.9f));
    assert(!NearMask(DepthAt(c, 0.8f), 0.0f, 0.0f, c.proj, 0.9f));
    // A pixel's own uv: the left-edge pixel at z 0.7 and the matching centre pixel.
    assert(NearMask(DepthAt(c, 0.7f), 0.5f / W, 0.5f / H, c.proj, 1.5f));
    // Off (0) and sky / cleared depth never mask; a bad threshold leaves the default.
    assert(!NearMask(DepthAt(c, 0.5f), u, v, c.proj, 0.0f));
    assert(!NearMask(1.0f, u, v, c.proj, 100.0f));
    assert(Near(ParseMaskMeters("0.6", 0.9f), 0.6f) && Near(ParseMaskMeters("0", 0.9f), 0.0f));
    assert(Near(ParseMaskMeters("abc", 0.9f), 0.9f) && Near(ParseMaskMeters("-1", 0.9f), 0.9f));
    assert(Near(ParseMaskMeters("", 0.9f), 0.9f) && Near(ParseMaskMeters("99", 0.9f), 5.0f));
}

void TestColorOrder() {
    // BGRA stays BGRA, RGBA stays RGBA (no red/blue swap), sRGB becomes UNORM.
    assert(DlssColorFormat(50) == 44 && DlssColorFormat(44) == 44);
    assert(DlssColorFormat(43) == 37 && DlssColorFormat(37) == 37);
}

void TestFeatureRecreate() {
    const FeatureKey a{1000, 1100, 2000, 2200, 3, 11, false};
    FeatureKey b = a;
    // Per eye: each eye has its own key, the same wish never recreates a feature it already has.
    FeatureKey eye_l = a, eye_r = a;
    assert(CreateReasonFor(&eye_l, a) == CreateReason::None);
    assert(CreateReasonFor(&eye_r, a) == CreateReason::None);
    assert(CreateReasonFor(nullptr, a) == CreateReason::FirstUse);
    assert(CreateReasonFor(nullptr, a, true) == CreateReason::Reset);
    b.out_w = 2400;
    assert(CreateReasonFor(&eye_l, b) == CreateReason::Resize);
    assert(CreateReasonFor(&eye_r, a) == CreateReason::None); // the other eye is unaffected
    b = a;
    b.render_h = 900;
    assert(CreateReasonFor(&a, b) == CreateReason::Resize);
    b = a;
    b.quality = 1;
    assert(CreateReasonFor(&a, b) == CreateReason::Quality);
    b = a;
    b.preset = 12;
    assert(CreateReasonFor(&a, b) == CreateReason::Preset);
    b = a;
    b.hdr = true;
    assert(CreateReasonFor(&a, b) == CreateReason::Reset);

    // Recreate guard: more than 3 creates of one eye within 2 s warns, once; the other eye and a
    // slow cadence do not.
    RecreateGuard g;
    assert(!g.Note(0, 0.0) && !g.Note(0, 100.0) && !g.Note(0, 200.0));
    assert(g.Note(0, 300.0));    // the 4th within 2 s
    assert(!g.Note(0, 400.0));   // once only
    assert(!g.Note(1, 400.0));   // eye 1 counted separately
    RecreateGuard slow;
    for (int i = 0; i < 20; ++i) {
        assert(!slow.Note(0, i * 1000.0)); // one per second: never more than 3 in 2 s
    }
    RecreateGuard every_frame; // a loop at 36 Hz per eye
    bool warned = false;
    for (int i = 0; i < 100; ++i) {
        warned |= every_frame.Note(1, i * 27.8);
    }
    assert(warned);
}

void TestJitterPerEyeAndOff() {
    // Alternate eyes: each eye walks its own Halton sequence 1,2,3... and they do not interleave.
    Tracker t;
    const Camera c = MakeCamera(0, 0, 0);
    uint32_t n[2] = {0, 0};
    for (uint64_t f = 1; f <= 12; ++f) {
        const int eye = int((f - 1) & 1);
        const auto j = t.ChooseJitter(true, 1.5f);
        assert(t.PendingEye() == eye);
        const auto want = JitterAt(++n[eye]);
        assert(Near(j[0], want[0], 1e-6f) && Near(j[1], want[1], 1e-6f));
        t.OnEyeFrame(eye, c, j, f, double(f) * 13.9);
    }
    assert(t.JitterSeq(0) == 6 && t.JitterSeq(1) == 6);
    // dlss_jitter=0: zero for both eyes, and the sequences still stay per eye.
    assert(ParseJitterOn("1") && ParseJitterOn("on") && !ParseJitterOn("0") &&
           !ParseJitterOn("off") && !ParseJitterOn("false"));
    Tracker z;
    for (uint64_t f = 1; f <= 6; ++f) {
        const auto j = z.ChooseJitter(true, 1.5f, false);
        assert(j[0] == 0.0f && j[1] == 0.0f);
        z.OnEyeFrame(int((f - 1) & 1), c, j, f, double(f));
    }
}

} // namespace

int main() {
    TestFeatureRecreate();
    TestJitterPerEyeAndOff();
    TestColorOrder();
    TestOptions();
    TestOutputSize();
    TestSelect();
    TestJitter();
    TestHistory();
    TestMotionVectors();
    TestMask();
    std::printf("xr-dlss-test: all passed\n");
    return 0;
}
