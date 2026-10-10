// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_sandbox3d_dash_test.exe [out/vr_assets]: sandbox3d's locomotion (locomotion.h) driven
// by scripted controllers, no headset: walking where the head looks, snap turns about the head,
// a dodge, a backstep and a jump as comfort dashes (straight, level, landing exactly where the
// move ends), the same moves raw (the animation's dip and tumble), A held back from the scene
// while it dodges, the config section added to sandbox3d.json. Synthetic moves always; the
// recorded ones too when <assets>/dash/dodges.json exists. Exit code 0 when every check passes.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "locomotion.h"
#include "model.h"

using namespace m3;

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

constexpr double kDt = 1.0 / 72.0;

/// A fake headset: the head at (0, 1.65, 0) of the tracking space, turned by `head_yaw`, the
/// sticks and buttons of the bridge's pad mapping.
struct Rig {
    sb::Locomotion loco;
    double t = 0.0;
    float head_yaw = 0.0f;
    float lx = 0.0f, ly = 0.0f, rx = 0.0f; // -1..1, y up
    uint32_t buttons = 0;
    sb::SceneInput last;
    std::string log;
    explicit Rig(const std::filesystem::path& config) : loco(config) {}

    sb::SceneInput Step() {
        sb::SceneInput in;
        in.time = t;
        in.stage = true;
        in.head = {true, {0.0f, 1.65f, 0.0f}, AxisAngle({0, 1, 0}, head_yaw)};
        in.grip[1] = {true, {0.2f, 1.2f, -0.3f}, {}};
        in.buttons = buttons;
        BbXrInput pad{};
        pad.active = 1;
        pad.buttons = buttons;
        const auto axis = [](float v) { return uint8_t(std::lround(128.0f + 127.0f * std::fmax(-1.0f, std::fmin(1.0f, v)))); };
        pad.lx = axis(lx);
        pad.ly = axis(-ly);
        pad.rx = axis(rx);
        pad.ry = 128;
        log += loco.Update(in, pad);
        t += kDt;
        last = in;
        return in;
    }
    void Run(double seconds) {
        for (double end = t + seconds; t < end - 1e-9;) {
            Step();
        }
    }
    void Tap(uint32_t button) {
        buttons |= button;
        Step();
        Step();
        buttons &= ~button;
        Step();
    }
    Vec3 Head() const { return last.head.position; }
};

void WriteConfig(const std::filesystem::path& path, const std::string& locomotion) {
    std::ofstream f(path, std::ios::binary);
    f << "{\n  \"render_scale\": 0.6,\n  \"locomotion\": " << locomotion << "\n}\n";
}

float Horizontal(Vec3 v) {
    return std::sqrt(v.x * v.x + v.z * v.z);
}

void TestConfigSection(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "plain.json";
    {
        std::ofstream f(path, std::ios::binary);
        f << "{\n  \"render_scale\": 0.7,\n  \"show\": {\"grid\": false}\n}\n";
    }
    sb::LocoConfigFile file(path);
    sb::LocoConfig config;
    std::string log;
    CHECK(file.Poll(0.0, config, log));
    const std::string text = sb::ReadText(path);
    std::string error;
    const json::Value doc = json::Parse(text, &error);
    CHECK(error.empty());
    CHECK(doc["locomotion"].IsObject());
    CHECK(std::fabs(doc["render_scale"].Float() - 0.7f) < 1e-6f); // the rest is kept
    CHECK(!doc["show"]["grid"].Bool(true));
    CHECK(doc["locomotion"]["mode"].Str() == "dash");
}

/// Walking: where the head looks, at the configured speed; a snap turn about the head.
void TestWalkAndTurn(const std::filesystem::path& config) {
    Rig r(config);
    r.Run(0.1);
    const Vec3 start = r.Head();
    r.ly = 1.0f;
    r.Run(1.0);
    r.ly = 0.0f;
    r.Run(0.3);
    const Vec3 walked = r.Head() - start;
    CHECK(walked.z < -2.5f && walked.z > -3.1f); // 3 m/s for 1 s, minus the 0.08 s ramps
    CHECK(std::fabs(walked.x) < 1e-3f && std::fabs(walked.y) < 1e-4f);
    // Looking left (90 deg): the stick's forward walks left (-X).
    r.head_yaw = 0.5f * kPi;
    const Vec3 before = r.Head();
    r.ly = 1.0f;
    r.Run(0.5);
    r.ly = 0.0f;
    r.Run(0.2);
    const Vec3 left = r.Head() - before;
    CHECK(left.x < -1.0f && std::fabs(left.z) < 1e-3f);
    // Snap turn right: the head stays where it is, the view turns 30 deg right.
    r.head_yaw = 0.0f;
    r.Run(0.05);
    const Vec3 pivot = r.Head();
    const float yaw0 = YawOf(r.last.head.rotation);
    r.rx = 1.0f;
    r.Run(0.1);
    r.rx = 0.0f;
    r.Run(0.1);
    CHECK(Length(r.Head() - pivot) < 1e-4f);
    float turned = YawOf(r.last.head.rotation) - yaw0;
    turned = std::atan2(std::sin(turned), std::cos(turned));
    CHECK(std::fabs(turned + 30.0f * kDeg) < 1e-3f);
    std::printf("walk: %.2f m forward in 1 s; looking left walks left; snap turn %.1f deg about the head\n",
                -walked.z, turned / kDeg);
}

/// The game's first-person steering and turning (turn_adapter.h): "body" walks along the view's
/// heading whatever the head does; a snap repeats every 0.3 s while held; smooth turns at its rate.
void TestSteerModes(const std::filesystem::path& dir) {
    const std::filesystem::path body = dir / "body.json", smooth = dir / "smooth.json";
    WriteConfig(body, "{\"raw_source\": \"synthetic\", \"move\": \"body\", \"turn\": \"snap\"}");
    WriteConfig(smooth, "{\"raw_source\": \"synthetic\", \"turn\": \"smooth\", \"turn_speed\": 90, \"turn_accel\": 0}");
    Rig r(body);
    r.Run(0.1);
    r.head_yaw = 0.5f * kPi; // looking left
    const Vec3 before = r.Head();
    r.ly = 1.0f;
    r.Run(0.5);
    r.ly = 0.0f;
    r.Run(0.2);
    const Vec3 walked = r.Head() - before;
    CHECK(walked.z < -1.0f && std::fabs(walked.x) < 1e-3f); // forward, not where the head looks
    // Held right for 1 s: snaps at 0, 0.3, 0.6, 0.9 s.
    r.head_yaw = 0.0f;
    r.Run(0.05);
    const float yaw0 = YawOf(r.last.head.rotation);
    r.rx = 1.0f;
    r.Run(1.0);
    r.rx = 0.0f;
    r.Run(0.05);
    float held = YawOf(r.last.head.rotation) - yaw0;
    held = std::atan2(std::sin(held), std::cos(held));
    CHECK(std::fabs(held + 120.0f * kDeg) < 1e-3f);
    Rig s(smooth);
    s.Run(0.1);
    const float yaw1 = YawOf(s.last.head.rotation);
    s.rx = -1.0f;
    s.Run(0.5);
    s.rx = 0.0f;
    s.Run(0.05);
    float smoothly = YawOf(s.last.head.rotation) - yaw1;
    smoothly = std::atan2(std::sin(smoothly), std::cos(smoothly));
    CHECK(std::fabs(smoothly - 45.0f * kDeg) < 2.0f * kDeg); // left at 90 deg/s for 0.5 s
    std::printf("steer: body walks along the view, a held snap repeats (%.0f deg in 1 s), smooth %.1f deg in 0.5 s\n",
                held / kDeg, smoothly / kDeg);
}

/// A move as the comfort dash: level all the way (a jump: its bump only), straight, ending
/// exactly where the move ends; A is held back from the scene meanwhile.
struct MoveResult {
    Vec3 displacement;
    float max_rise = 0.0f, max_drop = 0.0f, max_pitch = 0.0f, max_across = 0.0f;
    bool a_masked = true;
};
MoveResult DoMove(const std::filesystem::path& config, int kind) {
    Rig r(config);
    r.Run(0.2);
    MoveResult m;
    Vec3 at; // the head when the move begins (a dodge on A's release, a jump on the click)
    if (kind == 1) { // backstep: A tapped standing still
        r.buttons |= BBXR_BTN_CIRCLE;
        r.Step();
        r.Step();
        at = r.Head();
        r.buttons = 0;
        r.Step();
    } else { // dodge (A) or jump (left stick click) while the stick pushes forward
        r.ly = 1.0f;
        r.Step();
        if (kind == 0) {
            r.buttons |= BBXR_BTN_CIRCLE;
            r.Step();
            m.a_masked = !(r.last.buttons & BBXR_BTN_CIRCLE);
            at = r.Head();
            r.buttons = 0;
            r.Step();
        } else {
            at = r.Head();
            r.buttons |= BBXR_BTN_L3;
            r.Step();
            r.buttons = 0;
            r.Step();
        }
        r.ly = 0.0f;
    }
    const Vec3 start = at;
    for (double end = r.t + 3.0; r.t < end;) {
        r.Step();
        const Vec3 h = r.Head();
        m.max_rise = std::fmax(m.max_rise, h.y - start.y);
        m.max_drop = std::fmax(m.max_drop, start.y - h.y);
        m.max_across = std::fmax(m.max_across, std::fabs(h.x - at.x));
        const Vec3 f = Rotate(r.last.head.rotation, {0, 0, -1});
        m.max_pitch = std::fmax(m.max_pitch, std::fabs(std::asin(std::fmax(-1.0f, std::fmin(1.0f, f.y)))));
    }
    m.displacement = r.Head() - start;
    return m;
}

/// `dir` holds the configs (and dash/dodges.json for the recorded moves).
void TestMoves(const std::filesystem::path& dir, const char* source) {
    const std::string common = std::string("\"raw_source\": \"") + source + "\", \"game_fps\": 30";
    // Distances of the moves played (the same paths for both modes).
    sb::LocoConfig config;
    config.raw_source = source;
    sb::MoveSet moves;
    moves.Load(dir, config);
    std::printf("%s\n", moves.log.c_str());
    const char* names[3] = {"dodge", "backstep", "jump"};
    for (int kind = 0; kind < 3; ++kind) {
        const std::filesystem::path dash = dir / "dash.json", raw = dir / "raw.json";
        WriteConfig(dash, "{\"mode\": \"dash\", " + common + "}");
        WriteConfig(raw, "{\"mode\": \"raw\", \"raw_rotation\": 1, " + common + "}");
        const MoveResult d = DoMove(dash, kind), w = DoMove(raw, kind);
        const float want = kind == 0 ? moves.roll.Distance() : kind == 1 ? moves.backstep.Distance() : moves.jump.Distance();
        // Lands where the move ends, in both modes (no drift), forward or back.
        CHECK(std::fabs(Horizontal(d.displacement) - want) < 0.02f);
        CHECK(Length(d.displacement - w.displacement) < 0.02f);
        CHECK(kind == 1 ? d.displacement.z > 0.0f : d.displacement.z < 0.0f);
        CHECK(std::fabs(d.displacement.y) < 1e-3f);
        // The dash: level (a jump: its 0.3 m bump), straight, no pitch from the animation.
        CHECK(d.max_drop < 1e-3f);
        CHECK(d.max_rise < (kind == 2 ? 0.301f : 1e-3f));
        CHECK(d.max_across < 1e-3f);
        CHECK(d.max_pitch < 1e-3f);
        CHECK(d.a_masked);
        // Raw: the animation's dip and the head's pitch reach the view.
        if (kind != 1) {
            CHECK(w.max_drop > 0.5f);
            CHECK(w.max_pitch > 30.0f * kDeg);
        }
        std::printf("%s (%s): %.2f m, dash dips %.4f m rises %.3f m pitch %.2f deg | raw dips %.2f m rises %.2f m "
                    "pitch %.0f deg\n",
                    names[kind], source, Horizontal(d.displacement), d.max_drop, d.max_rise, d.max_pitch / kDeg,
                    w.max_drop, w.max_rise, w.max_pitch / kDeg);
    }
}

/// "ab" alternates: dash, raw, dash, ...; A held still passes to the scene (calibration).
void TestAlternateAndCalibration(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "ab.json";
    WriteConfig(path, "{\"mode\": \"ab\", \"raw_source\": \"synthetic\"}");
    Rig r(path);
    r.Run(0.1);
    for (int i = 0; i < 4; ++i) {
        r.ly = 1.0f;
        r.Step();
        r.Tap(BBXR_BTN_CIRCLE);
        r.ly = 0.0f;
        r.Run(2.5);
    }
    const size_t first = r.log.find("move 1: dodge DASH"), second = r.log.find("move 2: dodge RAW");
    const size_t third = r.log.find("move 3: dodge DASH"), fourth = r.log.find("move 4: dodge RAW");
    CHECK(first != std::string::npos && second != std::string::npos && third != std::string::npos &&
          fourth != std::string::npos);
    // Standing still with A held: the scene sees it (its T-pose calibration).
    r.buttons = BBXR_BTN_CIRCLE;
    r.Run(0.5);
    CHECK((r.last.buttons & BBXR_BTN_CIRCLE) != 0);
    r.buttons = 0;
    r.Run(0.1);
    std::printf("ab: moves alternate dash/raw; a still A held reaches the scene\n");
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path assets = argc > 1 ? argv[1] : "out/vr_assets";
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bbport_dash_test" / "dash";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    TestConfigSection(dir);
    const std::filesystem::path walk = dir / "walk.json";
    WriteConfig(walk, "{\"raw_source\": \"synthetic\", \"turn\": \"snap\"}");
    TestWalkAndTurn(walk);
    TestSteerModes(dir);
    TestMoves(dir, "synthetic");
    if (std::filesystem::exists(assets / "dash" / "dodges.json")) {
        // The recorded moves: a copy of dodges.json next to the temporary configs.
        const std::filesystem::path rec = dir.parent_path() / "recorded";
        std::filesystem::create_directories(rec / "dash", ec);
        std::filesystem::copy_file(assets / "dash" / "dodges.json", rec / "dash" / "dodges.json",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        TestMoves(rec, "auto");
    } else {
        std::printf("skip recorded moves: no %s\n", (assets / "dash" / "dodges.json").string().c_str());
    }
    TestAlternateAndCalibration(dir);
    std::filesystem::remove_all(dir.parent_path(), ec);
    std::printf("locomotion tests: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
