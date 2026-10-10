// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_stereo_test.exe: offline unit tests of alternate-eye stereo's pairing and layer logic
// (xr_stereo.h; docs/vr/findings/22-phase3-alt-eye-stereo.md). No headset, no game:
//   - every finished frame goes to the eye its camera was set up for, with frames presented late,
//     lost, repeated, in a still scene (L and R differ only by the eye offset) and while strafing
//     at exactly one IPD per frame (L of frame n = R of frame n+1);
//   - frames without a camera or with an unknown one are held, then go to the mono path;
//   - the bridge shows the eyes only when both have an image newer than the latest mono one;
//   - the age statistics of the pacing log.
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
    TestAltEyes();
    TestAges();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
