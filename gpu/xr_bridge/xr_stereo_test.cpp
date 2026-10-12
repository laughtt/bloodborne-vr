// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_stereo_test.exe: offline unit tests of alternate-eye stereo's pairing and layer logic
// (xr_stereo.h; docs/vr/findings/22-phase3-alt-eye-stereo.md). No headset, no game:
//   - every finished frame goes to the eye its camera was set up for, with frames presented late,
//     lost, repeated, in a still scene (L and R differ only by the eye offset) and while strafing
//     at exactly one IPD per frame (L of frame n = R of frame n+1);
//   - frames without a camera or with an unknown one are held, then go to the mono path;
//   - a frame whose first scene camera is another view (sewers) pairs by its other cameras;
//   - a cutscene reported by the host sends every frame to the mono path (cinema) at once;
//   - the bridge shows the eyes only when both have an image newer than the latest mono one;
//   - the age statistics of the pacing log;
//   - true stereo (BB_XR_MODE=stereo, findings/31): the mode words, both eyes' layer poses from
//     one snapshot, pair atomicity, the hold/flat decision, the release -> display latency and
//     the display time the host asks views for (GetViewsAt).
// Exit code 0 when every check passes.

#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

#include "xr_stereo.h"

using namespace xrstereo;

namespace {

int failures = 0, checks = 0;

void Check(bool ok, const char* what, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

constexpr float kHalfIpd = 0.03125f;
constexpr float kTy = 1.0f;        // 90 deg vertical
constexpr float kAspect = 1.4281f; // the Dream Air union frustum
constexpr float kTag = 4e-5f;

struct Cam {
    float rows[3][3];
    float pos[3];
};

Cam MakeCam(float heading, float pitch, float x, float y, float z) {
    const float fx = std::sin(heading) * std::cos(pitch), fy = std::sin(pitch),
                fz = std::cos(heading) * std::cos(pitch);
    const float ux = -std::sin(heading) * std::sin(pitch), uy = std::cos(pitch),
                uz = -std::cos(heading) * std::sin(pitch);
    // right = up x forward (the game's det +1 cameras)
    const float rx = uy * fz - uz * fy, ry = uz * fx - ux * fz, rz = ux * fy - uy * fx;
    return {{{rx, ry, rz}, {ux, uy, uz}, {fx, fy, fz}}, {x, y, z}};
}

/// What H1 publishes for frame `seq` rendering `eye` from the center camera `c`.
EyeView View(uint64_t seq, int eye, const Cam& c, float half_ipd = kHalfIpd, bool invert = false) {
    EyeView v;
    v.seq = seq;
    v.eye = eye;
    v.has_pose = true;
    const float side = ((eye == 1) != invert) ? half_ipd : -half_ipd;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            v.rows[i][j] = c.rows[i][j];
        }
        v.center[i] = c.pos[i];
        v.position[i] = c.pos[i] + side * c.rows[0][i];
    }
    const float tx = kTy * kAspect * (eye == 1 ? 1.0f + kTag : 1.0f);
    v.fov[0] = -std::atan(tx);
    v.fov[1] = std::atan(tx);
    v.fov[2] = std::atan(kTy);
    v.fov[3] = -std::atan(kTy);
    return v;
}

/// The finished frame's camera as the scene constants carry it: [R | t], t = -R p, and the
/// projection's x scale 1 / tx.
FrameCamera Frame(const EyeView& v, bool with_proj = true) {
    FrameCamera f;
    f.has_view = true;
    for (int i = 0; i < 3; ++i) {
        float t = 0.0f;
        for (int j = 0; j < 3; ++j) {
            f.view[i * 4 + j] = v.rows[i][j];
            t -= v.rows[i][j] * v.position[j];
        }
        f.view[i * 4 + 3] = t;
    }
    f.has_proj = with_proj;
    f.proj_x = 1.0f / std::tan(v.fov[1]);
    return f;
}

/// A moving camera: frame n's center camera (walking, turning, looking around).
Cam Moving(int n) {
    return MakeCam(0.3f + 0.01f * float(n), -0.1f + 0.002f * float(n % 50),
                   120.0f + 0.05f * float(n), -6.5f, -15.0f + 0.03f * float(n));
}

void TestInOrder() {
    Pairing p;
    std::vector<EyeView> views;
    int wrong = 0, eyes = 0;
    uint32_t lag = 0;
    for (int n = 0; n < 400; ++n) {
        views.push_back(View(uint64_t(n + 1), n & 1, Moving(n)));
        p.Add(views.back());
        if (n >= 3) { // presented three frames after its camera was set up
            const Decision d = p.OnFrame(Frame(views[size_t(n - 3)]));
            eyes += d.kind == Kind::Eye;
            wrong += d.kind != Kind::Eye || d.eye != views[size_t(n - 3)].eye ||
                     d.view.seq != views[size_t(n - 3)].seq;
            lag = d.lag;
        }
    }
    CHECK(eyes == 397 && wrong == 0);
    CHECK(lag == 3);
    CHECK(p.stats().side_failures == 0 && p.stats().same_eye_twice == 0 && p.stats().max_run == 1);
    CHECK(p.stats().eye[0] == 199 && p.stats().eye[1] == 198);
    std::printf("PASS in order, 3 frames late: every frame to its own eye\n");
}

void TestLostAndRepeated() {
    Pairing p;
    std::vector<EyeView> views;
    int wrong = 0, held = 0, given = 0, repeats = 0;
    for (int n = 0; n < 300; ++n) {
        views.push_back(View(uint64_t(n + 1), n & 1, Moving(n)));
        p.Add(views.back());
        if (n < 2) {
            continue;
        }
        const int f = n - 2;
        if (f % 7 == 3 || f % 11 == 5) {
            continue; // lost: never presented
        }
        Decision d = p.OnFrame(Frame(views[size_t(f)]));
        wrong += d.kind != Kind::Eye || d.eye != views[size_t(f)].eye;
        ++given;
        if (f % 13 == 0) { // presented again (a repeated flip of the same image)
            ++repeats;
            d = p.OnFrame(Frame(views[size_t(f)]));
            held += d.kind == Kind::Hold && std::string_view(d.reason) == "repeat";
        }
    }
    CHECK(wrong == 0);
    CHECK(repeats == 18 && held == repeats); // f = 0, 13, ..., 286, five of them lost
    CHECK(p.stats().held_repeat == uint64_t(repeats) && p.stats().side_failures == 0);
    CHECK(p.stats().same_eye_twice > 0); // a lost frame gives its neighbour's eye two in a row
    CHECK(p.stats().eye[0] + p.stats().eye[1] == uint64_t(given));
    std::printf("PASS lost and repeated frames: no swap, repeats held\n");
}

void TestStill() {
    // Head and game still: every L view has the same camera, every R view too.
    Pairing p;
    const Cam c = MakeCam(1.0f, 0.0f, 10.0f, 2.0f, 3.0f);
    std::vector<EyeView> views;
    int wrong = 0;
    for (int n = 0; n < 200; ++n) {
        views.push_back(View(uint64_t(n + 1), n & 1, c));
        p.Add(views.back());
        if (n >= 4) {
            const Decision d = p.OnFrame(Frame(views[size_t(n - 4)]));
            wrong += d.kind != Kind::Eye || d.eye != views[size_t(n - 4)].eye ||
                     d.view.seq != views[size_t(n - 4)].seq; // in order: the oldest new match
        }
    }
    CHECK(wrong == 0);
    CHECK(p.stats().side_failures == 0);
    std::printf("PASS still scene: identical cameras per eye, in order\n");
}

void TestStrafeCollision() {
    // Strafing left at exactly one IPD per frame without turning: R of frame n+1 has the camera of
    // L of frame n. With frame n lost, only the eye tag keeps frame n+1 (R) from going to L.
    const float step = 2.0f * kHalfIpd;
    std::vector<EyeView> views;
    for (int n = 0; n < 40; ++n) {
        Cam c = MakeCam(0.5f, 0.0f, 0.0f, 0.0f, 0.0f);
        for (int i = 0; i < 3; ++i) {
            c.pos[i] = -step * float(n) * c.rows[0][i];
        }
        views.push_back(View(uint64_t(n + 1), n & 1, c));
    }
    for (int n = 0; n + 1 < 40; ++n) {
        if (n % 2 == 0) {
            float d = 0.0f;
            for (int i = 0; i < 3; ++i) {
                d = std::max(d, std::fabs(views[size_t(n)].position[i] -
                                          views[size_t(n + 1)].position[i]));
            }
            if (d > 1e-5f) {
                CHECK(!"the test cameras do not collide");
                break;
            }
        }
    }
    // With the tag: lose every frame with n % 4 == 2 (an L frame), present the rest.
    {
        Pairing p;
        int wrong = 0;
        for (int n = 0; n < 16; ++n) { // the ring holds 16
            p.Add(views[size_t(n)]);
        }
        for (int n = 0; n < 16; ++n) {
            if (n % 4 == 2) {
                continue;
            }
            const Decision d = p.OnFrame(Frame(views[size_t(n)]));
            wrong += d.kind != Kind::Eye || d.eye != views[size_t(n)].eye;
        }
        CHECK(wrong == 0);
        CHECK(p.stats().tag_rejects > 0);
        CHECK(p.stats().side_failures == 0);
    }
    // Without a projection (no tag), frame n+1 after a lost frame n would go to the wrong eye:
    // this is what the tag is for (the pose alone cannot tell).
    {
        Pairing p;
        for (int n = 0; n < 16; ++n) {
            p.Add(views[size_t(n)]);
        }
        p.OnFrame(Frame(views[0], false));
        p.OnFrame(Frame(views[1], false));
        // frame 2 (L) lost; frame 3 (R) has frame 2's camera
        const Decision d = p.OnFrame(Frame(views[3], false));
        CHECK(d.kind == Kind::Eye && d.eye == 0); // documents the ambiguity without the tag
    }
    std::printf("PASS strafing at one IPD per frame: the eye tag keeps the eyes apart\n");
}

void TestHoldAndMono() {
    Config config;
    config.fallback_frames = 30;
    Pairing p{config};
    // Loading screen: no camera.
    int mono = 0, held = 0;
    for (int n = 0; n < 40; ++n) {
        const Decision d = p.OnFrame(FrameCamera{});
        mono += d.kind == Kind::Mono;
        held += d.kind == Kind::Hold;
    }
    CHECK(held == 29 && mono == 11 && p.Mono());
    // An unknown camera (not set up by the hook: a cutscene camera without H1) also falls back.
    Pairing q{config};
    const EyeView stranger = View(1, 0, MakeCam(0.0f, 0.0f, 5.0f, 5.0f, 5.0f));
    q.Add(View(2, 1, MakeCam(1.0f, 0.0f, 5.0f, 5.0f, 5.0f)));
    int unmatched = 0;
    for (int n = 0; n < 35; ++n) {
        const Decision d = q.OnFrame(Frame(stranger));
        unmatched += std::string_view(d.reason) == "unmatched";
    }
    CHECK(unmatched == 35 && q.Mono());
    // Eye frames again: straight back to the eyes.
    p.Add(View(1, 0, Moving(0)));
    p.Add(View(2, 1, Moving(1)));
    Decision d = p.OnFrame(Frame(View(1, 0, Moving(0))));
    CHECK(d.kind == Kind::Eye && d.eye == 0 && !p.Mono());
    d = p.OnFrame(Frame(View(2, 1, Moving(1))));
    CHECK(d.kind == Kind::Eye && d.eye == 1);
    // A stale camera (the last scene camera kept through a loading screen) is a repeat.
    for (int n = 0; n < 31; ++n) {
        d = p.OnFrame(Frame(View(2, 1, Moving(1))));
    }
    CHECK(d.kind == Kind::Mono && p.stats().held_repeat == 31);
    std::printf("PASS no camera, unknown camera, stale camera: held, then mono, then eyes again\n");
}

void TestSideAndTransposed() {
    // Negative control: the image rendered from the other eye's position under this eye's label.
    Pairing p;
    const EyeView inverted = View(1, 0, Moving(5), kHalfIpd, true);
    p.Add(inverted);
    const Decision d = p.OnFrame(Frame(inverted));
    CHECK(d.kind == Kind::Eye && d.side == 1 && p.stats().side_failures == 1);
    // A view whose 3x3 is stored as columns still matches (as the mono pairing allows).
    Pairing q;
    const EyeView v = View(1, 1, Moving(9));
    q.Add(v);
    FrameCamera f = Frame(v);
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            std::swap(f.view[i * 4 + j], f.view[j * 4 + i]);
        }
    }
    // the translation of a transposed layout: t = -R^T-as-stored... rebuild it for the columns
    for (int i = 0; i < 3; ++i) {
        float t = 0.0f;
        for (int k = 0; k < 3; ++k) {
            t -= f.view[k * 4 + i] * v.position[k];
        }
        f.view[i * 4 + 3] = t;
    }
    const Error e = Compare(f, v);
    CHECK(e.transposed && e.rotation < 1e-5f);
    // Zero IPD: no side to check.
    Pairing r;
    const EyeView zero = View(1, 1, Moving(3), 0.0f);
    r.Add(zero);
    CHECK(r.OnFrame(Frame(zero)).side == 0 && r.stats().side_failures == 0);
    std::printf("PASS side check (negative control) and layouts\n");
}

void TestOffAxis() {
    // Off-axis eyes (Dream Air: left -55..46 deg, right -46..55): the x scale is 1 / half-width.
    const float l = std::tan(-55.0f / 57.29578f), r = std::tan(46.0f / 57.29578f);
    const float half = 0.5f * (r - l);
    auto off = [&](uint64_t seq, int eye, const Cam& c) {
        EyeView v = View(seq, eye, c);
        const float hw = half * (eye ? 1.0f + kTag : 1.0f), centre = eye ? -0.5f * (r + l) : 0.5f * (r + l);
        v.fov[0] = std::atan(centre - hw);
        v.fov[1] = std::atan(centre + hw);
        return v;
    };
    Pairing p;
    std::vector<EyeView> views;
    int wrong = 0;
    for (int n = 0; n < 40; ++n) {
        views.push_back(off(uint64_t(n + 1), n & 1, Moving(n)));
        p.Add(views.back());
        if (n >= 2) {
            FrameCamera f = Frame(views[size_t(n - 2)]);
            const float hw = half * ((n - 2) & 1 ? 1.0f + kTag : 1.0f);
            f.proj_x = 1.0f / hw; // the game's x scale for the off-axis camera
            const Decision d = p.OnFrame(f);
            wrong += d.kind != Kind::Eye || d.eye != views[size_t(n - 2)].eye;
        }
    }
    CHECK(wrong == 0);
    std::printf("PASS off-axis eyes pair by their half-width\n");
}

// The Central Yharnam sewers: the frame's first G-buffer camera is another view (a mirrored one
// here), the eye's camera comes second. One camera: unmatched for the whole area, mono; all the
// frame's cameras: every frame pairs, and from the first matched frame on the eyes are back.
void TestOtherSceneCamera() {
    Config config;
    config.fallback_frames = 30;
    Pairing old_way{config}, p{config};
    int wrong = 0, mono_old = 0;
    for (int n = 0; n < 200; ++n) {
        const EyeView v = View(uint64_t(n + 1), n & 1, Moving(n));
        old_way.Add(v);
        p.Add(v);
        // The extra view: the eye camera mirrored about a water plane 2 m below.
        Cam mirror = Moving(n);
        for (int i = 0; i < 3; ++i) {
            mirror.rows[i][1] = -mirror.rows[i][1];
        }
        mirror.pos[1] = -6.5f - 2.0f - (mirror.pos[1] + 6.5f + 2.0f);
        const FrameCamera cams[2] = {Frame(View(uint64_t(n + 1), n & 1, mirror)), Frame(v)};
        mono_old += old_way.OnFrame(cams[0]).kind == Kind::Mono;
        const Decision d = p.OnFrame(cams, 2);
        wrong += d.kind != Kind::Eye || d.eye != (n & 1) || d.camera != 1;
    }
    CHECK(mono_old == 171 && old_way.stats().held_unmatched == 200);
    CHECK(wrong == 0 && !p.Mono() && p.stats().other_camera == 200);
    CHECK(p.stats().eye[0] == 100 && p.stats().eye[1] == 100 && p.stats().side_failures == 0);
    // The diagnostics of an unmatched frame: how far the nearest view was.
    Pairing q{config};
    q.Add(View(1, 0, Moving(0)));
    const Decision u = q.OnFrame(Frame(View(1, 0, MakeCam(1.0f, 0.0f, 120.0f, -6.5f, -15.0f))));
    CHECK(std::string_view(u.reason) == "unmatched" && u.best_rotation > 0.1f &&
          u.best_rotation < 2.0f && u.best_position < 0.1f);
    // A frame with no cameras at all is "no camera", as with one empty camera.
    CHECK(std::string_view(q.OnFrame(nullptr, 0).reason) == "no camera");
    std::printf("PASS another scene camera first: paired by the frame's other cameras\n");
}

// A real-time cutscene (Remo): the host stops publishing eye views and reports it; every frame goes
// to the mono path (the cinema screen) at once, not after 30 held frames; after it the eyes come
// back with the first matched frame.
void TestCinema() {
    Config config;
    config.fallback_frames = 30;
    Pairing p{config};
    for (int n = 0; n < 10; ++n) {
        const EyeView v = View(uint64_t(n + 1), n & 1, Moving(n));
        p.Add(v);
        CHECK(p.OnFrame(Frame(v)).kind == Kind::Eye);
    }
    p.SetCinema(true);
    CHECK(p.Cinema());
    const FrameCamera cutscene = Frame(View(99, 0, MakeCam(2.0f, 0.2f, 0.0f, 0.0f, 0.0f)));
    int mono = 0;
    for (int n = 0; n < 50; ++n) {
        const Decision d = p.OnFrame(cutscene);
        mono += d.kind == Kind::Mono && std::string_view(d.reason) == "cinema";
    }
    CHECK(mono == 50 && p.Mono() && p.stats().cinema == 50 && p.stats().held_unmatched == 0);
    // Even a frame whose camera matches an old view goes to the cinema screen while it is on.
    CHECK(p.OnFrame(Frame(View(9, 0, Moving(8)))).kind == Kind::Mono);
    p.SetCinema(false);
    // The last cutscene frames after the end (the hook publishes again from the next frame): mono.
    CHECK(p.OnFrame(cutscene).kind == Kind::Mono);
    const EyeView back = View(11, 0, Moving(11));
    p.Add(back);
    const Decision d = p.OnFrame(Frame(back));
    CHECK(d.kind == Kind::Eye && d.eye == 0 && !p.Mono());
    std::printf("PASS cutscene: the cinema screen at once, the eyes again after it\n");
}

void TestRingOverflow() {
    Pairing p;
    std::vector<EyeView> views;
    for (int n = 0; n < 20; ++n) {
        views.push_back(View(uint64_t(n + 1), n & 1, Moving(n)));
        p.Add(views.back());
    }
    // 20 views published, frame 1 presented only now: it left the ring.
    const Decision d = p.OnFrame(Frame(views[0]));
    CHECK(d.kind == Kind::Hold && std::string_view(d.reason) == "unmatched");
    const Decision e = p.OnFrame(Frame(views[10]));
    CHECK(e.kind == Kind::Eye && e.eye == 0 && e.lag == 9);
    std::printf("PASS ring overflow: frames older than 16 views are held\n");
}

void TestAltEyes() {
    AltEyes a;
    CHECK(!a.Showing(0.0, 1.0));
    a.ReleaseEye(0, 0.010);
    CHECK(!a.Showing(0.011, 1.0)); // one eye only
    a.ReleaseEye(1, 0.027);
    CHECK(a.Showing(0.030, 1.0));
    CHECK(!a.Showing(1.5, 1.0)); // nothing for 1.5 s: background
    a.ReleaseFlat(0.040);          // a loading screen
    CHECK(!a.Showing(0.041, 1.0));
    a.ReleaseEye(0, 0.050);
    CHECK(!a.Showing(0.051, 1.0)); // the right eye's image is from before the loading screen
    a.ReleaseEye(1, 0.067);
    CHECK(a.Showing(0.070, 1.0));
    CHECK(a.released[0] == 2 && a.released[1] == 2);
    std::printf("PASS alt layer: both eyes newer than the mono image\n");
}

void TestAges() {
    // Game at 60 FPS, display at 72 Hz: per display frame (13.9 ms) count the eye images released.
    AgeStats s;
    uint64_t released[2] = {};
    double next_frame = 0.0;
    int eye = 0;
    for (int d = 0; d < 7200; ++d) {
        const double now = d / 72.0;
        while (next_frame <= now) {
            ++released[eye];
            eye ^= 1;
            next_frame += 1.0 / 60.0;
        }
        s.Frame(released);
        if (d == 71) {
            s.ResetWindow(); // the first display frame counts both eyes as new
        }
    }
    const double mean_l = s.MeanAge(0), mean_r = s.MeanAge(1);
    CHECK(mean_l > 0.6 && mean_l < 1.2 && mean_r > 0.6 && mean_r < 1.2);
    CHECK(s.window.max_age[0] <= 3 && s.window.max_age[1] <= 3);
    CHECK(s.window.both_fresh == 0); // one game frame = one eye
    std::printf("PASS ages at 60 FPS / 72 Hz: mean %.2f / %.2f display frames, max %u / %u, "
                "both stale %.0f%%\n",
                mean_l, mean_r, s.window.max_age[0], s.window.max_age[1],
                100.0 * double(s.window.both_stale) / double(s.window.frames));
    // At 72 FPS / 72 Hz each eye is exactly 0 or 1 frame old.
    AgeStats t;
    uint64_t r2[2] = {};
    for (int d = 0; d < 720; ++d) {
        ++r2[d & 1];
        t.Frame(r2);
    }
    CHECK(t.window.max_age[0] == 1 && t.window.max_age[1] == 1 && t.window.both_stale == 0);
    t.ResetWindow();
    CHECK(t.window.frames == 0 && t.started);
    std::printf("PASS ages at 72 FPS / 72 Hz: each eye 0 or 1 display frame old\n");
}

// ---- true stereo (BB_XR_MODE=stereo) -----------------------------------------------------------

/// A unit quaternion for a rotation of `angle` radians about the unit axis (x, y, z).
void AxisAngle(float x, float y, float z, float angle, float q[4]) {
    const float s = std::sin(0.5f * angle);
    q[0] = x * s;
    q[1] = y * s;
    q[2] = z * s;
    q[3] = std::cos(0.5f * angle);
}

float Dist(const float a[3], const float b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                     (a[2] - b[2]) * (a[2] - b[2]));
}

void TestModeParse() {
    CHECK(ParseMode(nullptr) == XrMode::Cinema);
    CHECK(ParseMode("") == XrMode::Cinema);
    CHECK(ParseMode("mono") == XrMode::Mono);
    CHECK(ParseMode("alt") == XrMode::Alt);
    CHECK(ParseMode("stereo") == XrMode::Stereo);
    CHECK(ParseMode("Stereo") == XrMode::Cinema); // exact words, as the bridge always compared
    CHECK(ParseMode("stereo2") == XrMode::Cinema);
    CHECK(ParseMode("cinema") == XrMode::Cinema);
    std::printf("PASS mode parsing: mono, alt, stereo; anything else the cinema screen\n");
}

void TestPairPoses() {
    // One snapshot: the head at (1, 1.6, -2) turned 90 deg to the left (about +Y), the eyes 64 mm
    // apart in the head's frame, the right eye 2 mm forward (a canted display).
    PairView v;
    v.has_eye_offset = true;
    v.head_position[0] = 1.0f;
    v.head_position[1] = 1.6f;
    v.head_position[2] = -2.0f;
    AxisAngle(0, 1, 0, 1.5707963f, v.head_orientation);
    v.eye_offset[0][0] = -0.032f;
    v.eye_offset[1][0] = 0.032f;
    v.eye_offset[1][2] = -0.002f;
    float pos[2][3], q[4];
    CHECK(PairEyePoses(v, pos, q));
    // Turned 90 deg left, the head's +X (right) points to world -Z: the right eye is 32 mm toward
    // -Z of the head and 2 mm toward its forward (-Z rotated to -X).
    CHECK(std::fabs(pos[1][2] - (-2.0f - 0.032f)) < 1e-5f);
    CHECK(std::fabs(pos[1][0] - (1.0f - 0.002f)) < 1e-5f);
    CHECK(std::fabs(pos[0][2] - (-2.0f + 0.032f)) < 1e-5f);
    CHECK(std::fabs(pos[0][1] - 1.6f) < 1e-6f && std::fabs(pos[1][1] - 1.6f) < 1e-6f);
    // Both eyes carry the head's orientation (the image was rendered along the head's axes).
    for (int i = 0; i < 4; ++i) {
        CHECK(q[i] == v.head_orientation[i]);
    }
    // The baseline is the snapshot's own, whatever the head does.
    const float baseline = Dist(pos[0], pos[1]);
    CHECK(std::fabs(baseline - std::sqrt(0.064f * 0.064f + 0.002f * 0.002f)) < 1e-5f);
    for (int k = 0; k < 8; ++k) {
        const float n = std::sqrt(14.0f); // the axis (1, 2, 3), unit
        AxisAngle(1.0f / n, 2.0f / n, 3.0f / n, 0.7f * float(k), v.head_orientation);
        float p2[2][3], q2[4];
        CHECK(PairEyePoses(v, p2, q2));
        CHECK(std::fabs(Dist(p2[0], p2[1]) - baseline) < 1e-5f);
        // The midpoint of the eyes is a fixed distance from the head (the 1 mm of z).
        float mid[3];
        for (int i = 0; i < 3; ++i) {
            mid[i] = 0.5f * (p2[0][i] + p2[1][i]);
        }
        CHECK(std::fabs(Dist(mid, v.head_position) - 0.001f) < 1e-5f);
    }
    // No offsets (flat tests, fake head): no pose; the bridge uses the views current at release.
    PairView fake;
    CHECK(!PairEyePoses(fake, pos, q));
    std::printf("PASS pair poses: head orientation, positions = head + rotated offsets, one "
                "baseline of %.1f mm\n",
                baseline * 1000.0f);
}

void TestPairAtomic() {
    // The projection layer shows a pair only when both eyes have an image newer than the latest
    // flat one, and a pair release moves both eyes' counters together: a display frame never sees
    // one new eye next to an old one (the frame loop reads under the same lock as ReleasePair).
    AltEyes e;
    CHECK(!e.Showing(1.0, 1.0));
    e.ReleasePair(1.0);
    CHECK(e.valid[0] && e.valid[1] && e.released[0] == 1 && e.released[1] == 1);
    CHECK(e.released_at[0] == e.released_at[1]);
    CHECK(e.Showing(1.1, 1.0));
    AgeStats ages;
    for (int n = 0; n < 200; ++n) {
        e.ReleasePair(2.0 + n * 0.014);
        CHECK(e.released[0] == e.released[1]);
        ages.Frame(e.released);
    }
    CHECK(ages.window.one_fresh == 0 && ages.window.both_stale == 0);
    CHECK(ages.window.both_fresh == 200);
    // A flat frame (menu, loading) invalidates the pair until the next pair.
    e.ReleaseFlat(10.0);
    CHECK(!e.Showing(10.1, 1.0));
    e.ReleasePair(10.2);
    CHECK(e.Showing(10.3, 1.0));
    // Stale pairs stop showing (the host went away).
    CHECK(!e.Showing(12.0, 1.0));
    // A half release (an acquire failed) is a single ReleaseEye: after a flat frame it shows only
    // once the other eye has an image too.
    e.ReleaseFlat(20.0);
    e.ReleaseEye(0, 20.1);
    CHECK(!e.Showing(20.2, 1.0));
    e.ReleaseEye(1, 20.3);
    CHECK(e.Showing(20.4, 1.0));
    std::printf("PASS pair atomicity: counters move together (200 pairs, 0 one-eye frames), flat "
                "frames invalidate the pair\n");
}

void TestPairDecider() {
    PairDecider d(30);
    CHECK(d.OnFrame(false, true) == Kind::Eye);
    // Frames that match no render view are held, 29 of them, then the flat screen.
    for (int i = 0; i < 29; ++i) {
        CHECK(d.OnFrame(false, false) == Kind::Hold);
    }
    CHECK(d.OnFrame(false, false) == Kind::Mono);
    CHECK(d.OnFrame(false, false) == Kind::Mono); // stays flat while nothing matches
    CHECK(d.Mono());
    CHECK(d.OnFrame(false, true) == Kind::Eye);   // a match brings the pair back at once
    CHECK(!d.Mono());
    CHECK(d.OnFrame(false, false) == Kind::Hold); // the hold counter restarted
    // A cutscene goes flat at once, even for frames that match.
    CHECK(d.OnFrame(true, true) == Kind::Mono);
    CHECK(d.OnFrame(true, false) == Kind::Mono);
    CHECK(d.OnFrame(false, false) == Kind::Mono); // no match after the cutscene: still flat
    CHECK(d.OnFrame(false, true) == Kind::Eye);
    CHECK(d.cinema() == 2 && d.pairs() == 3);
    CHECK(d.frames() == 38 && d.held() == 33);
    std::printf("PASS pair decider: held 29 frames, then flat; a match or the end of a cutscene "
                "brings the pair back\n");
}

void TestLatencyTracker() {
    constexpr int64_t kPeriod = 13'888'889; // 72 Hz
    LatencyTracker t;
    CHECK(!t.Valid() && t.Periods() == 0.0);
    // The pipeline: a pair released 2.0 periods before the display time of the frame showing it.
    for (int i = 0; i < 7; ++i) {
        t.Shown(1'000'000'000 + i * kPeriod, 1'000'000'000 + i * kPeriod + 2 * kPeriod, kPeriod);
    }
    CHECK(!t.Valid());
    t.Shown(2'000'000'000, 2'000'000'000 + 2 * kPeriod, kPeriod);
    CHECK(t.Valid() && std::fabs(t.Periods() - 2.0) < 1e-9 && t.PeriodNs() == kPeriod);
    // Smoothed: one 3-period sample moves the mean by a tenth of the difference.
    t.Shown(3'000'000'000, 3'000'000'000 + 3 * kPeriod, kPeriod);
    CHECK(std::fabs(t.Periods() - 2.1) < 1e-9);
    // Converges to a new level.
    for (int i = 0; i < 100; ++i) {
        t.Shown(4'000'000'000, 4'000'000'000 + int64_t(1.5 * double(kPeriod)), kPeriod);
    }
    CHECK(std::fabs(t.Periods() - 1.5) < 1e-3);
    // Stalls and bad data are not samples: a display time before the release, more than 8 periods,
    // no release time, no period.
    const uint64_t n = t.samples();
    t.Shown(5'000'000'000, 5'000'000'000 - 1, kPeriod);
    t.Shown(5'000'000'000, 5'000'000'000 + 9 * kPeriod, kPeriod);
    t.Shown(0, 5'000'000'000, kPeriod);
    t.Shown(5'000'000'000, 5'000'000'000 + kPeriod, 0);
    CHECK(t.samples() == n && t.rejected() == 2);
    CHECK(std::fabs(t.Periods() - 1.5) < 1e-3);
    std::printf("PASS latency tracker: %.3f periods after 100 samples, stalls rejected\n",
                t.Periods());
}

void TestPredictDisplayTime() {
    constexpr int64_t kPeriod = 13'888'889;
    const int64_t now = 5'000'000'000;
    // now + the host's 8 ms (camera -> release) + 2 display periods (release -> display).
    CHECK(PredictDisplayTime(now, 8'000'000, 2.0, kPeriod) == now + 8'000'000 + 2 * kPeriod);
    CHECK(PredictDisplayTime(now, 0, 1.5, kPeriod) == now + int64_t(1.5 * double(kPeriod)));
    // A host time below zero (a clock hiccup) counts as zero.
    CHECK(PredictDisplayTime(now, -5, 2.0, kPeriod) == now + 2 * kPeriod);
    // Never more than 8 periods ahead, never in the past.
    CHECK(PredictDisplayTime(now, 0, 50.0, kPeriod) == now + 8 * kPeriod);
    CHECK(PredictDisplayTime(now, 2'000'000'000, 2.0, kPeriod) == now + 8 * kPeriod);
    CHECK(PredictDisplayTime(now, 0, 0.0, kPeriod) == now);
    // No XrTime now (the runtime cannot convert), no period or a nonsense latency: 0, the host
    // keeps the views of GetViews.
    CHECK(PredictDisplayTime(0, 8'000'000, 2.0, kPeriod) == 0);
    CHECK(PredictDisplayTime(now, 8'000'000, 2.0, 0) == 0);
    CHECK(PredictDisplayTime(now, 8'000'000, -1.0, kPeriod) == 0);
    CHECK(PredictDisplayTime(now, 8'000'000, std::nan(""), kPeriod) == 0);
    // End to end with a simulated frame loop (72 Hz, the runtime predicts 2 periods ahead of the
    // wait): the host releases 6 ms after the frame loop's wait returned, the loop shows the pair
    // in its next frame; the tracker measures it, and a request made at the time a camera is set
    // up lands where the image of that camera will be displayed.
    LatencyTracker t;
    const int64_t lead = 2 * kPeriod;         // display time - XrTime at the wait
    const int64_t release_offset = 6'000'000; // the host releases this long after the wait
    for (int frame = 0; frame < 50; ++frame) {
        const int64_t wait_xr = 1'000'000'000 + frame * kPeriod;
        t.Shown(wait_xr + release_offset, wait_xr + kPeriod + lead, kPeriod);
    }
    CHECK(t.Valid());
    const double expected = double(kPeriod + lead - release_offset) / double(kPeriod);
    CHECK(std::fabs(t.Periods() - expected) < 1e-9);
    // The camera is set up 5 ms before its release, which is 6 ms after a wait: the image is shown
    // at the next frame's display time, wait + 1 period + lead.
    const int64_t wait_xr = 1'000'000'000 + 20 * kPeriod;
    const int64_t camera_xr = wait_xr + release_offset - 5'000'000;
    const int64_t asked = PredictDisplayTime(camera_xr, 5'000'000, t.Periods(), kPeriod);
    CHECK(std::llabs(asked - (wait_xr + kPeriod + lead)) < 1000);
    std::printf("PASS display-time prediction: %.3f periods release to display, asks %.2f ms "
                "ahead of the camera\n",
                t.Periods(), double(asked - camera_xr) * 1e-6);
}

} // namespace

int main() {
    TestInOrder();
    TestLostAndRepeated();
    TestStill();
    TestStrafeCollision();
    TestHoldAndMono();
    TestSideAndTransposed();
    TestOffAxis();
    TestRingOverflow();
    TestOtherSceneCamera();
    TestCinema();
    TestAltEyes();
    TestAges();
    TestModeParse();
    TestPairPoses();
    TestPairAtomic();
    TestPairDecider();
    TestLatencyTracker();
    TestPredictDisplayTime();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
