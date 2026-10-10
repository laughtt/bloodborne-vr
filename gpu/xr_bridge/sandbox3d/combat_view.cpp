// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "combat_view.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "image_out.h"
#include "model.h"

namespace sb {

using namespace m3;

void DrawCombatHud(UiCanvas& cv, const SwingReadout& r, const CombatConfig& c,
                   const TrackedPose& head, const TrackedPose& left_grip) {
    SwingHudConfig hud;
    hud.head = c.hud_head;
    hud.wrist = c.hud_wrist;
    hud.distance = c.hud_distance;
    hud.down_deg = c.hud_down_deg;
    hud.scale = c.hud_scale;
    hud.wrist_scale = c.wrist_scale;
    hud.wrist_offset = c.wrist_offset;
    hud.label_s = c.label_s;
    hud.speed_max = c.speed_max;
    hud.popups = c.popups;
    DrawSwingHud(cv, r, hud, UiPose{head.valid, head.position, head.rotation},
                 UiPose{left_grip.valid, left_grip.position, left_grip.rotation});
}

bool CombatView::Create(const std::filesystem::path& assets, Renderer& renderer,
                        std::string& log) {
    std::string font_log;
    if (!font.Load(assets / "ui", font_log)) {
        log += "swing HUD: no font in " + (assets / "ui").string() +
               " (scripts/vr_assets/ui_extract.py): the bars show, the labels only in the "
               "console\n";
    }
    std::string error;
    if (!gpu.Create(renderer.GetDevice(), error)) {
        log += "swing HUD: " + error + "\n";
        return false;
    }
    if (font.Ready() && !gpu.UploadFont(font.width, font.height, font.pixels)) {
        log += "swing HUD: font upload failed: " + gpu.LastError() + "\n";
    }
    font.pixels = {};
    chain.first = renderer.GetDrawHook(); // the menus' (ui.h), drawn first
    chain.second = &gpu;
    renderer.SetDrawHook(&chain);
    ready = true;
    return true;
}

void CombatView::Destroy(Renderer& renderer) {
    if (!ready) {
        return;
    }
    if (renderer.GetDrawHook() == &chain) {
        renderer.SetDrawHook(chain.first);
    }
    gpu.Destroy();
    ready = false;
}

void CombatView::Build(const Combat& combat, const TrackedPose& head, const TrackedPose& left_grip) {
    if (!ready) {
        return;
    }
    canvas.Clear();
    DrawCombatHud(canvas, combat.Readout(), combat.Config(), head, left_grip);
    gpu.SetGeometry(canvas);
}

CombatConfig ReadCombatConfig(const std::filesystem::path& config_path) {
    CombatConfig c;
    bool ok = false;
    const std::string text = ReadText(config_path, &ok);
    if (ok) {
        std::string error;
        const json::Value doc = json::Parse(text, &error);
        if (error.empty()) {
            c.Load(doc["swing"]);
        }
    }
    return c;
}

// ---- offline previews ---------------------------------------------------------------------------

namespace {

constexpr double kRate = 72.0; // the Dream Air's refresh

float MinJerk(double u) {
    const double x = std::clamp(u, 0.0, 1.0);
    return float(x * x * x * (10.0 - 15.0 * x + 6.0 * x * x));
}

Quat LookAt(Vec3 from, Vec3 to) {
    const Vec3 f = Normalize(to - from, {0, 0, -1});
    const Vec3 r = Normalize(Cross(f, {0, 1, 0}), {1, 0, 0});
    const Vec3 u = Cross(r, f);
    Mat4 m;
    m.m[0] = r.x, m.m[1] = r.y, m.m[2] = r.z;
    m.m[4] = u.x, m.m[5] = u.y, m.m[6] = u.z;
    m.m[8] = -f.x, m.m[9] = -f.y, m.m[10] = -f.z;
    return ToQuat(m);
}

/// How the held weapon sits in the controller frame: its tip direction and side axis.
struct Aim {
    Vec3 tip{0, 0, -1}, side{1, 0, 0};
};

/// The controller rotation that points the weapon's tip along `dir`, its side axis toward `side`.
Quat GripFor(const Aim& aim, Vec3 dir, Vec3 side) {
    dir = Normalize(dir);
    const Quat q = FromTo(Normalize(aim.tip), dir);
    const Vec3 have = Perp(Rotate(q, aim.side), dir), want = Perp(side, dir);
    if (Length(have) < 1e-3f || Length(want) < 1e-3f) {
        return q;
    }
    return AxisAngle(dir, SignedAngle(Normalize(have), Normalize(want), dir)) * q;
}

/// Scripted frames through the sandbox's path: Scene::Update (rig, solid weapons) then
/// Combat::Frame, as the headset loop runs them.
class Preview {
public:
    Preview(Scene& scene, Renderer& renderer, const Config& config, Combat& combat,
            CombatView& view, std::filesystem::path out, uint32_t size)
        : cfg(config), scene(scene), renderer(renderer), combat(combat), view(view),
          out(std::move(out)), size(size) {}

    /// Stand at `head` looking along `yaw` (degrees, +left) and `pitch` (degrees, +up).
    void Begin(const char* scenario, Vec3 head, float yaw_deg, float pitch_deg) {
        name = scenario;
        yaw = yaw_deg * kDeg;
        input = SceneInput{};
        input.stage = true;
        input.head = {true, head, AxisAngle({0, 1, 0}, yaw) * AxisAngle({1, 0, 0}, pitch_deg * kDeg)};
        for (int side = 0; side < 2; ++side) {
            input.grip[side] = input.aim[side] = Rest(side);
        }
        scene.Physics().Reset();
        // The weapons the swing logic's numbers are about (an earlier preview's buttons may have
        // cycled the scene's own choice): none for a frame, then these.
        cfg.weapon = cfg.left_weapon = "";
        Step();
        cfg.weapon = "Saw Cleaver";
        cfg.left_weapon = "Hunter Pistol";
        for (int settle = 0; settle < 3; ++settle) {
            Step();
        }
        scene.Physics().Reset();
        std::printf("combat preview \"%s\"\n", scenario);
    }
    Vec3 Local(Vec3 p) const {
        return input.head.position + Rotate(AxisAngle({0, 1, 0}, yaw), p);
    }
    Vec3 LocalDir(Vec3 d) const {
        return Rotate(AxisAngle({0, 1, 0}, yaw), d);
    }
    TrackedPose Rest(int side) const {
        return {true, Local({side ? 0.3f : -0.3f, -0.75f, 0.0f}),
                AxisAngle({0, 1, 0}, yaw) * AxisAngle({1, 0, 0}, -90.0f * kDeg)};
    }
    void Grip(int side, Vec3 p, Quat q) {
        input.grip[side] = input.aim[side] = {true, p, q};
    }
    /// The right weapon's tip direction in its controller frame (one frame pointing back).
    Aim AimOf() {
        const Quat q = AxisAngle({0, 1, 0}, yaw + kPi);
        Grip(1, Local({0.25f, -0.4f, 0.1f}), q);
        Step();
        const WeaponPhysics::Hand& hand = scene.Physics().GetHand(1);
        Aim aim;
        aim.tip = Rotate(Conjugate(q), Normalize(col::Apply(hand.target, hand.shape.tip) - hand.target.x,
                                                 {0, 0, -1}));
        aim.side = Rotate(Conjugate(q), Rotate(hand.target.q, {1, 0, 0}));
        input.grip[1] = input.aim[1] = Rest(1);
        scene.Physics().Reset();
        Step();
        scene.Physics().Reset();
        return aim;
    }
    void Step() {
        input.time = (time += 1.0 / kRate);
        SceneEvents events;
        scene.Update(input, cfg, nullptr, events, renderer);
        CombatEvents combat_events;
        combat.Frame(input, scene.Physics(), cfg.form, false, combat_events);
        if (!combat_events.set_form.empty()) {
            cfg.form = combat_events.set_form;
        }
        for (size_t at = 0; at < combat_events.log.size();) {
            const size_t end = combat_events.log.find('\n', at);
            const std::string line = combat_events.log.substr(at, end == std::string::npos ? end : end - at);
            if (!line.empty()) {
                std::printf("  %7.3f s  %s\n", time - 3000.0, line.c_str());
            }
            if (end == std::string::npos) {
                break;
            }
            at = end + 1;
        }
    }
    /// A level slash of the right hand about `pivot` (user frame): yaw from `from` to `to`
    /// (degrees, +left, 0 ahead) in `seconds` with a natural (minimum-jerk) speed profile, after
    /// holding the start pose for `lead` seconds (no jump into the swing).
    void Slash(const Aim& aim, Vec3 pivot, float radius, float from, float to, double seconds,
               double lead) {
        const double t0 = time + lead;
        while (time < t0 + seconds) {
            const float k = MinJerk((time + 1.0 / kRate - t0) / seconds);
            const float a = (from + (to - from) * k) * kDeg;
            const Vec3 dir = LocalDir({-std::sin(a), 0.0f, -std::cos(a)});
            const Vec3 lead = LocalDir(Vec3{-std::cos(a), 0.0f, std::sin(a)} * (to >= from ? 1.0f : -1.0f));
            Grip(1, Local(pivot) + dir * radius, GripFor(aim, dir, lead));
            Step();
        }
    }
    void Hold(double seconds) {
        const double t0 = time;
        while (time < t0 + seconds) {
            Step();
        }
    }
    /// Left: first person along the head (the HUD as in the headset); right: from `camera`.
    bool Shot(Vec3 camera, Vec3 target, float camera_half_fov) {
        const TrackedPose& head = input.head;
        const Mat4 first = ProjectionFov(-0.6f, 0.6f, 0.6f, -0.6f, cfg.near_z, cfg.far_z) *
                           Inverse(FromPose(head.rotation, head.position));
        const Mat4 outside =
            ProjectionFov(-camera_half_fov, camera_half_fov, camera_half_fov, -camera_half_fov,
                          cfg.near_z, cfg.far_z) *
            Inverse(FromPose(LookAt(camera, target), camera));
        const Mat4 view_proj[2] = {first, outside};
        view.Build(combat, head, input.grip[0]);
        Config third = cfg;
        third.show_head = true;
        FrameData data;
        std::vector<uint8_t> a[2], b[2];
        scene.Build(third, data);
        if (!renderer.RenderOffscreen(size, size, view_proj, data, b)) {
            std::printf("render failed: %s\n", renderer.LastError().c_str());
            return false;
        }
        scene.Build(cfg, data);
        if (!renderer.RenderOffscreen(size, size, view_proj, data, a)) {
            std::printf("render failed: %s\n", renderer.LastError().c_str());
            return false;
        }
        std::vector<uint8_t> combined(size_t(size) * 2 * size * 4);
        for (uint32_t y = 0; y < size; ++y) {
            std::memcpy(&combined[size_t(y) * size * 2 * 4], &a[0][size_t(y) * size * 4], size_t(size) * 4);
            std::memcpy(&combined[(size_t(y) * size * 2 + size) * 4], &b[1][size_t(y) * size * 4],
                        size_t(size) * 4);
        }
        const std::filesystem::path png = out / ("combat_" + name + ".png");
        if (!WritePng(png.string(), size * 2, size, combined)) {
            std::printf("cannot write %s\n", png.string().c_str());
            return false;
        }
        const SwingReadout& r = combat.Readout();
        std::printf("  -> %s: SP %.1f / %g, label %d result %d\n", png.string().c_str(), r.stamina,
                    r.max, int(r.label), int(r.result));
        ++written;
        return true;
    }

    Config cfg;
    int written = 0;
    SceneInput input;

private:
    Scene& scene;
    Renderer& renderer;
    Combat& combat;
    CombatView& view;
    std::filesystem::path out;
    uint32_t size;
    std::string name;
    float yaw = 0.0f;
    double time = 3000.0;
};

} // namespace

int RenderCombatPreviews(const std::filesystem::path& assets,
                         const std::filesystem::path& config_path, Renderer& renderer, Scene& scene,
                         const Config& config, const std::filesystem::path& out_dir,
                         uint32_t size) {
    CombatView view;
    std::string log;
    view.Create(assets, renderer, log);
    std::printf("%s", log.c_str());
    CombatConfig cc = ReadCombatConfig(config_path); // the user's tuning, never written here
    cc.log_file = (out_dir / "combat_swing.jsonl").string();
    std::error_code ec;
    std::filesystem::remove(cc.log_file, ec);
    Config base = config;
    base.show_mirror = false;
    base.show_skeleton = false;
    base.show_controllers = false;
    base.show_laser = false;
    base.body_yaw_deadzone_deg = 0.0f;
    base.physics.contact_log = "";
    base.physics.log_contacts = false;
    int failed = 0, written = 0;
    const PhysicsConfig& p = base.physics;
    if (!p.enemy.empty() && p.props) {
        // 1. A heavy slash into the enemy (c2632): the swing decides HEAVY, the blade connects,
        // 35 is paid (the bar flashes, the cost floats up), a strong pulse.
        {
            Combat combat(cc, assets);
            Preview d(scene, renderer, base, combat, view, out_dir, size);
            const Vec3 enemy{p.enemy_pos[0], 0.0f, p.enemy_pos[1]};
            const Vec3 facing = Normalize(Vec3{-enemy.x, 0.0f, -enemy.z}, {0, 0, 1});
            // 1.15 m away: the blade starts clear of the A-posed arms and sweeps into the body.
            const Vec3 head = enemy + facing * 1.15f + Vec3{0, 1.65f, 0};
            const float yaw = std::atan2(facing.x, facing.z) / kDeg;
            d.Begin("hit", head, yaw, -12.0f);
            const Aim aim = d.AimOf();
            d.Slash(aim, {0.15f, -0.12f, -0.05f}, 0.35f, -110.0f, 20.0f, 0.24, 0.35);
            d.Hold(0.12);
            failed += !d.Shot(d.Local({1.3f, 0.25f, -0.2f}), enemy + Vec3{0, 1.3f, 0}, 0.6f);
            written += d.written;
        }
        // 2. No stamina: five dodges drain it below 0 (91 -> -9), then a slower slash: it does
        // not register (two buzzes); the wrist shows the bar with the left hand raised.
        {
            Combat combat(cc, assets);
            Preview d(scene, renderer, base, combat, view, out_dir, size);
            const Vec3 enemy{p.enemy_pos[0], 0.0f, p.enemy_pos[1]};
            const Vec3 facing = Normalize(Vec3{-enemy.x, 0.0f, -enemy.z}, {0, 0, 1});
            const Vec3 head = enemy + facing * 1.6f + Vec3{0, 1.65f, 0};
            const float yaw = std::atan2(facing.x, facing.z) / kDeg;
            d.Begin("tired", head, yaw, -14.0f);
            const Aim aim = d.AimOf();
            for (int k = 0; k < 5 && combat.TryDash(0, d.input.time); ++k) {
            }
            d.Grip(0, d.Local({-0.16f, -0.32f, -0.36f}),
                   AxisAngle({0, 1, 0}, yaw * kDeg) * AxisAngle({0, 0, 1}, 70.0f * kDeg) *
                       AxisAngle({1, 0, 0}, 20.0f * kDeg));
            d.Slash(aim, {0.15f, -0.12f, -0.05f}, 0.35f, -90.0f, 0.0f, 0.5, 0.3);
            d.Hold(0.05);
            failed += !d.Shot(d.Local({0.9f, 0.3f, 0.6f}), d.Local({-0.1f, -0.25f, -0.4f}), 0.6f);
            written += d.written;
        }
    } else {
        std::printf("combat previews: no enemy or props in the physics config: skipping hit/tired\n");
    }
    // 3. The charged heavy: the wind-up held still overhead (CHARGING, filling up).
    {
        Combat combat(cc, assets);
        Preview d(scene, renderer, base, combat, view, out_dir, size);
        d.Begin("charge", {0.0f, 1.65f, 2.5f}, 0.0f, -5.0f);
        const Vec3 overhead = d.Local({0.18f, 0.32f, 0.06f});
        const Quat up = AxisAngle({1, 0, 0}, 80.0f * kDeg);
        // Raise it (a rising stroke: a wind-up, not an attack), then hold still.
        const TrackedPose from = d.input.grip[1];
        const double t0 = d.input.time;
        for (int f = 0; f < int(0.4 * kRate); ++f) {
            const float k = MinJerk((d.input.time - t0) / 0.4);
            d.Grip(1, Lerp(from.position, overhead, k), Slerp(from.rotation, up, k));
            d.Step();
        }
        d.Hold(0.9);
        failed += !d.Shot(d.Local({1.1f, 0.4f, -1.2f}), d.Local({0.0f, 0.0f, 0.0f}), 0.55f);
        written += d.written;
    }
    std::printf("combat previews: %d written to %s, log in %s\n", written,
                out_dir.string().c_str(), cc.log_file.c_str());
    view.Destroy(renderer);
    return failed ? 1 : 0;
}

} // namespace sb
