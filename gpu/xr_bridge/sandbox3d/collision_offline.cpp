// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "collision_offline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "image_out.h"
#include "renderer.h"
#include "scene.h"

namespace sb {

using namespace m3;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 72.0; // the Dream Air's refresh

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

Mat4 ViewProj(Vec3 position, Vec3 look, float half_fov, const Config& config) {
    return ProjectionFov(-half_fov, half_fov, half_fov, -half_fov, config.near_z, config.far_z) *
           Inverse(FromPose(LookAt(position, look), position));
}

/// Horizontal direction of a yaw (radians, positive to the left; 0 is -Z).
Vec3 Heading(float yaw) {
    return {-std::sin(yaw), 0.0f, -std::cos(yaw)};
}

/// How a held weapon sits in the controller frame: the direction to its farthest point and its
/// model +X (for the Saw Cleaver: the side its folded blade is on).
struct Aim {
    Vec3 tip{0, 0, -1}, side{1, 0, 0};
};

/// The controller rotation that points the weapon's tip along `dir` and turns its side axis
/// toward `side` (as far as it can, perpendicular to `dir`).
Quat GripFor(const Aim& aim, Vec3 dir, Vec3 side) {
    dir = Normalize(dir);
    const Quat q = FromTo(Normalize(aim.tip), dir);
    const Vec3 have = Perp(Rotate(q, aim.side), dir), want = Perp(side, dir);
    if (Length(have) < 1e-3f || Length(want) < 1e-3f) {
        return q;
    }
    return AxisAngle(dir, SignedAngle(Normalize(have), Normalize(want), dir)) * q;
}

class Director {
public:
    Director(Scene& scene, Renderer& renderer, const Config& config, fs::path out, uint32_t size)
        : cfg(config), scene(scene), renderer(renderer), out(std::move(out)), size(size) {}

    /// A new scenario: the weapons, where the user stands and looks; fresh physics.
    void Begin(const char* scenario, const std::string& right, const std::string& left,
               Vec3 head, float yaw_deg) {
        name = scenario;
        cfg.weapon = right;
        cfg.left_weapon = left;
        cfg.show_mirror = false;
        cfg.body_yaw_deadzone_deg = 0.0f;
        input = SceneInput{};
        input.stage = true;
        head_yaw = yaw_deg * kDeg;
        input.head = {true, head, AxisAngle({0, 1, 0}, head_yaw)};
        for (int side = 0; side < 2; ++side) {
            input.grip[side] = input.aim[side] = Rest(side);
        }
        scene.Physics().Reset(); // no sweep from the last scenario's hands
        for (int settle = 0; settle < 3; ++settle) {
            Step(); // the weapons load, the body stands under the head
        }
        scene.Physics().Reset();
        hits_before = scene.Physics().HitCount();
        std::printf("collision preview \"%s\": right %s, left %s\n", scenario, right.c_str(),
                    left.c_str());
    }

    /// Where the user's frame puts a point (x right, y up, -z ahead of the head's yaw).
    Vec3 Local(Vec3 p) const {
        return input.head.position + Rotate(AxisAngle({0, 1, 0}, head_yaw), p);
    }
    Vec3 LocalDir(Vec3 d) const { return Rotate(AxisAngle({0, 1, 0}, head_yaw), d); }

    /// A relaxed hand low at the side, the controller pointing down (out of the way).
    TrackedPose Rest(int side) const {
        return {true, Local({side ? 0.3f : -0.3f, -0.75f, 0.0f}),
                AxisAngle({0, 1, 0}, head_yaw) * AxisAngle({1, 0, 0}, -90.0f * kDeg)};
    }

    /// How a hand's weapon sits in its controller frame (one frame with a level controller
    /// pointing back, away from whatever the scenario aims at).
    Aim AimOf(int side) {
        const Quat q = AxisAngle({0, 1, 0}, head_yaw + kPi);
        input.grip[side] = input.aim[side] = {true, Local({side ? 0.25f : -0.25f, -0.4f, 0.1f}), q};
        Step();
        const WeaponPhysics::Hand& hand = scene.Physics().GetHand(side);
        Aim aim;
        aim.tip = Rotate(Conjugate(q),
                         Normalize(col::Apply(hand.target, hand.shape.tip) - hand.target.x,
                                   {0, 0, -1}));
        aim.side = Rotate(Conjugate(q), Rotate(hand.target.q, {1, 0, 0}));
        input.grip[side] = input.aim[side] = Rest(side);
        scene.Physics().Reset();
        Step();
        scene.Physics().Reset();
        return aim;
    }

    void Grip(int side, Vec3 position, Quat rotation) {
        input.grip[side] = input.aim[side] = {true, position, rotation};
    }

    /// One 72 Hz frame; prints what the physics logged.
    void Step() {
        input.time = (time += 1.0 / kRate);
        SceneEvents events;
        scene.Update(input, cfg, nullptr, events, renderer);
        for (size_t at = 0; at < events.log.size();) {
            const size_t end = events.log.find('\n', at);
            const std::string line =
                events.log.substr(at, end == std::string::npos ? end : end - at);
            if (!line.empty()) {
                std::printf("  %7.3f s  %s\n", time - 1000.0, line.c_str());
            }
            if (end == std::string::npos) {
                break;
            }
            at = end + 1;
        }
        if (std::getenv("BB_COLLISION_DEBUG")) {
            for (int side = 0; side < 2; ++side) {
                const WeaponPhysics::Hand& h = scene.Physics().GetHand(side);
                const Vec3 tp = Rotate(h.pose.q, h.shape.tip), tt = Rotate(h.target.q, h.shape.tip);
                std::printf("    %s weapon (%.2f %.2f %.2f) target (%.2f %.2f %.2f) tip yaw %.0f / "
                            "%.0f deg, sep %.3f m %.0f deg%s%s\n",
                            side ? "R" : "L", h.pose.x.x, h.pose.x.y, h.pose.x.z, h.target.x.x,
                            h.target.x.y, h.target.x.z, std::atan2(-tp.x, -tp.z) / kDeg,
                            std::atan2(-tt.x, -tt.z) / kDeg, h.separation, h.separation_deg,
                            h.passthrough ? " passthrough" : "",
                            std::count(h.touching.begin(), h.touching.end(), 1) ? " touching" : "");
            }
        }
    }

    int NewHits() const { return scene.Physics().HitCount() - hits_before; }

    /// Left: first person looking at `look`; right: a camera at `camera` looking at `target`.
    bool Shot(Vec3 look, Vec3 camera, Vec3 target, float camera_half_fov = 0.5f) {
        const Vec3 eye = input.head.position;
        const Mat4 view_proj[2] = {ViewProj(eye, look, 0.8f, cfg),
                                   ViewProj(camera, target, camera_half_fov, cfg)};
        Config third = cfg;
        third.show_head = true;
        FrameData data;
        std::vector<uint8_t> first[2], outside[2];
        scene.Build(third, data);
        if (!renderer.RenderOffscreen(size, size, view_proj, data, outside)) {
            std::printf("render failed: %s\n", renderer.LastError().c_str());
            return false;
        }
        scene.Build(cfg, data);
        if (!renderer.RenderOffscreen(size, size, view_proj, data, first)) {
            std::printf("render failed: %s\n", renderer.LastError().c_str());
            return false;
        }
        std::vector<uint8_t> combined(size_t(size) * 2 * size * 4);
        for (uint32_t y = 0; y < size; ++y) {
            std::memcpy(&combined[size_t(y) * size * 2 * 4], &first[0][size_t(y) * size * 4],
                        size_t(size) * 4);
            std::memcpy(&combined[(size_t(y) * size * 2 + size) * 4],
                        &outside[1][size_t(y) * size * 4], size_t(size) * 4);
        }
        const fs::path png = out / ("collision_" + name + ".png");
        if (!WritePng(png.string(), size * 2, size, combined)) {
            std::printf("cannot write %s\n", png.string().c_str());
            return false;
        }
        const WeaponPhysics& physics = scene.Physics();
        if (std::getenv("BB_COLLISION_DEBUG")) {
            for (int side = 0; side < 2; ++side) {
                const WeaponPhysics::Hand& h = physics.GetHand(side);
                const Vec3 tp = Normalize(Rotate(h.pose.q, h.shape.tip));
                const Vec3 tt = Normalize(Rotate(h.target.q, h.shape.tip));
                const Vec3 sp = Rotate(h.pose.q, {1, 0, 0}), st = Rotate(h.target.q, {1, 0, 0});
                std::printf("    ghost %s: %zu segments, wrist (%.2f %.2f %.2f); tip turned %.0f "
                            "deg (pose %.2f %.2f %.2f, target %.2f %.2f %.2f), side axis %.0f deg\n",
                            side ? "R" : "L", h.ghost.size(), h.ghost_wrist.x, h.ghost_wrist.y,
                            h.ghost_wrist.z, std::acos(std::clamp(Dot(tp, tt), -1.0f, 1.0f)) / kDeg,
                            tp.x, tp.y, tp.z, tt.x, tt.y, tt.z,
                            std::acos(std::clamp(Dot(sp, st), -1.0f, 1.0f)) / kDeg);
            }
        }
        std::printf("  -> %s: weapon held back from the hand R %.3f m / %.0f deg, L %.3f m / %.0f "
                    "deg; hits %d\n",
                    png.string().c_str(), physics.GetHand(1).separation,
                    physics.GetHand(1).separation_deg, physics.GetHand(0).separation,
                    physics.GetHand(0).separation_deg, NewHits());
        ++written;
        return true;
    }

    Config cfg;
    int written = 0;

private:
    Scene& scene;
    Renderer& renderer;
    fs::path out;
    uint32_t size;
    std::string name;
    SceneInput input;
    float head_yaw = 0.0f;
    double time = 1000.0;
    int hits_before = 0;
};

/// A level sweep of one hand around `pivot` (user frame) at `radius`, the weapon pointing
/// outward, from yaw `from` to `to` (degrees, +left, 0 ahead) in `frames`, then `hold` frames;
/// the weapon's side axis turns toward `blade_side` (user frame; zero: the way it swings, so the
/// Saw Cleaver's teeth lead). With `stop_after_hit` >= 0 it ends that many frames after a hit.
void SweepLevel(Director& d, int side, const Aim& aim, Vec3 pivot, float radius, float from,
                float to, int frames, int hold, Vec3 blade_side, int stop_after_hit = -1) {
    int since_hit = -1;
    for (int f = 0; f <= frames + hold; ++f) {
        const float t = std::min(float(f) / float(frames), 1.0f);
        const float yaw = (from + (to - from) * t) * kDeg;
        const Vec3 dir = d.LocalDir(Heading(yaw));
        const Vec3 lead = Vec3{-std::cos(yaw), 0.0f, std::sin(yaw)} * (to >= from ? 1.0f : -1.0f);
        const Vec3 facing = d.LocalDir(Length(blade_side) > 0.0f ? blade_side : lead);
        d.Grip(side, d.Local(pivot) + dir * radius, GripFor(aim, dir, facing));
        d.Step();
        if (stop_after_hit >= 0) {
            if (since_hit < 0 && d.NewHits() > 0) {
                since_hit = 0;
            } else if (since_hit >= 0 && ++since_hit >= stop_after_hit) {
                return;
            }
        }
    }
}

} // namespace

int RenderCollisionPreviews(Scene& scene, Renderer& renderer, const Config& config,
                            const fs::path& out_dir, uint32_t size) {
    Config base = config;
    base.physics.contact_log = (out_dir / "collision_contacts.jsonl").string();
    std::error_code ec;
    fs::remove(base.physics.contact_log, ec);
    Director d(scene, renderer, base, out_dir, size);
    const PhysicsConfig& p = base.physics;
    // The props sit around the origin facing -Z (the first head pose of the offline run).
    const Vec3 post{p.post[0], 0.0f, p.post[1]};

    // 1. A forehand into the training post: the blade stops at the pole while the controller
    // goes on ~25 degrees; the hand stays on the weapon, the ghost shows where the controller is.
    {
        d.Begin("post", "Saw Cleaver", "Hunter Pistol", post + Vec3{-0.65f, 1.65f, 0.65f}, 0.0f);
        const Aim aim = d.AimOf(1);
        SweepLevel(d, 1, aim, {0.15f, -0.68f, -0.05f}, 0.35f, -110.0f, -14.0f, 36, 10, {});
        d.Shot(post + Vec3{0.0f, 0.95f, 0.0f}, post + Vec3{0.6f, 2.2f, 1.1f},
               post + Vec3{-0.2f, 0.95f, 0.25f}, 0.55f);
    }
    // 2. Blade on blade: a level slash with the right sword into the left hand's upright cane;
    // the blades cross and stop.
    {
        d.Begin("blades", "Kirkhammer", "Threaded Cane", {-0.3f, 1.65f, 0.4f}, 0.0f);
        const Aim left = d.AimOf(0), right = d.AimOf(1);
        d.Grip(0, d.Local({-0.05f, -0.45f, -0.65f}), GripFor(left, {0, 1, 0}, {1, 0, 0}));
        SweepLevel(d, 1, right, {0.15f, -0.15f, -0.12f}, 0.3f, -90.0f, 45.0f, 34, 10, {0, 1, 0});
        d.Shot(d.Local({0.0f, -0.2f, -0.65f}), d.Local({0.3f, 0.3f, -1.95f}),
               d.Local({0.0f, -0.2f, -0.6f}), 0.5f);
    }
    // 3. Left gun against right blade: the pistol held upright before the chest as a guard, the
    // Saw Cleaver slashed level into its barrel from the right; the blade stops on the barrel,
    // the pistol gives a little, both hands pulse.
    {
        d.Begin("gun", "Saw Cleaver", "Hunter Pistol", {0.0f, 1.65f, 0.3f}, 0.0f);
        const Aim left = d.AimOf(0), right = d.AimOf(1);
        d.Grip(0, d.Local({0.0f, -0.5f, -0.5f}), GripFor(left, {0, 1, 0}, {0, 0, 1}));
        SweepLevel(d, 1, right, {0.15f, -0.4f, -0.05f}, 0.3f, -100.0f, 40.0f, 34, 10, {});
        d.Shot(d.Local({0.0f, -0.35f, -0.5f}), d.Local({0.75f, 0.25f, -1.35f}),
               d.Local({0.0f, -0.4f, -0.45f}), 0.45f);
    }
    // 4. A fast slash into the enemy (c2632) after stepping up to it: the hit capsule enters a
    // hurtbox above the hit speed (strong pulse, marker, log), then the blade stops on the body.
    if (!p.enemy.empty()) {
        const Vec3 enemy{p.enemy_pos[0], 0.0f, p.enemy_pos[1]};
        const Vec3 facing = Normalize(Vec3{-enemy.x, 0.0f, -enemy.z}, {0, 0, 1});
        const Vec3 head = enemy + facing * 0.85f + Vec3{0, 1.65f, 0};
        const float yaw = std::atan2(facing.x, facing.z) / kDeg; // looking at the enemy
        d.Begin("enemy", "Saw Cleaver", "Hunter Pistol", head, yaw);
        const Aim aim = d.AimOf(1);
        SweepLevel(d, 1, aim, {0.15f, -0.12f, -0.05f}, 0.35f, -100.0f, 10.0f, 22, 12, {}, 3);
        const Vec3 chest = enemy + Vec3{0, 1.55f, 0};
        d.Shot(chest, d.Local({1.25f, 0.2f, -0.35f}), chest - Vec3{0, 0.1f, 0}, 0.55f);
    }
    // 5. The blade pushed into the wall and dragged along it: it stays on the surface, grinding.
    {
        const Vec3 head{p.wall[0] - 0.75f, 1.65f, p.wall[1] + 1.0f};
        d.Begin("wall", "Saw Cleaver", "Hunter Pistol", head, -90.0f); // facing +x, the wall
        const Aim aim = d.AimOf(1);
        const Vec3 dir = d.LocalDir(Normalize(Vec3{0.35f, -0.15f, -1.0f}));
        for (int f = 0; f <= 60; ++f) {
            const float in = std::min(float(f) / 20.0f, 1.0f);              // toward the wall
            const float slide = f > 20 ? float(f - 20) / 40.0f * 0.6f : 0.0f; // then 1.1 m/s along
            d.Grip(1, d.Local({0.2f - slide, -0.4f, -0.1f - 0.35f * in}),
                   GripFor(aim, dir, d.LocalDir({0, 1, 0})));
            d.Step();
        }
        d.Shot(d.Local({-0.2f, -0.4f, -0.75f}), d.Local({0.55f, 0.55f, 0.55f}),
               d.Local({-0.15f, -0.4f, -0.75f}), 0.55f);
    }
    std::printf("collision previews: %d written to %s, contacts in %s\n", d.written,
                out_dir.string().c_str(), base.physics.contact_log.c_str());
    return d.written == (p.enemy.empty() ? 4 : 5) ? 0 : 1;
}

} // namespace sb
