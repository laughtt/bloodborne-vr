// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Tests of the weapon collision (collision.h, physics_weapon.h), run by
// bbport_xr_sandbox3d_test.exe (rig_test.cpp's main): the contact math, the weapon shapes (also
// on the real Saw Cleaver when out/vr_assets has it), push-out convergence, no tunnelling of fast
// swings at 72 and 90 Hz (against a thin post and against the other hand's blade), melee hits
// and the haptic rules.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "collision.h"
#include "hit_scale.h"
#include "model.h"
#include "physics_weapon.h"
#include "swing_logic.h"

using namespace m3;
using sb::col::Capsule;

namespace {

int failures = 0, checks = 0;

void Check(bool ok, const char* what, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL collision_test.cpp line %d: %s\n", line, what);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

bool Near(Vec3 a, Vec3 b, float eps) {
    return Length(a - b) <= eps;
}

/// A flat bar weapon: the grip at the origin, `length` along -Y, `width` along X, `thick` along Z.
sb::Model BarModel(float length, float width, float thick) {
    sb::Model m;
    m.name = "bar";
    m.kind = "weapon";
    m.bones.push_back(sb::Bone{"root", -1, Mat4{}, Mat4{}, true});
    for (int i = 0; i <= 32; ++i) {
        const float y = -length * float(i) / 32.0f;
        for (int c = 0; c < 4; ++c) {
            sb::Vertex v{};
            v.position[0] = (c & 1) ? width * 0.5f : -width * 0.5f;
            v.position[1] = y;
            v.position[2] = (c & 2) ? thick * 0.5f : -thick * 0.5f;
            v.weights[0] = 1.0f;
            m.vertices.push_back(v);
        }
    }
    return m;
}

/// A weapon pose: grip at `grip`, its -Y (the blade) along `dir`.
Mat4 BarPose(Vec3 grip, Vec3 dir) {
    return Translate(grip) * FromQuat(FromTo({0, -1, 0}, Normalize(dir)));
}

/// A level blade at `heading` (radians, from +X toward +Z): one yaw from the same blade
/// pointing +X, so a swing between two headings is a pure turn about the vertical (a FromTo per
/// heading would also roll the blade, and the shortest turn between those can lift it over a
/// post: legitimate sliding, but not what these tests measure).
Mat4 LevelPose(Vec3 grip, float heading) {
    return Translate(grip) *
           FromQuat(AxisAngle({0, 1, 0}, -heading) * FromTo({0, -1, 0}, {1, 0, 0}));
}

/// The blade's heading in the XZ plane (radians).
float Heading(const sb::col::Pose& p) {
    const Vec3 d = Rotate(p.q, {0, -1, 0});
    return std::atan2(d.z, d.x);
}

/// Props far away; no enemy, no logs.
sb::PhysicsConfig QuietConfig() {
    sb::PhysicsConfig c;
    c.enemy = "";
    c.contact_log = "";
    c.log_contacts = false;
    c.post[0] = c.post[1] = 50.0f;
    c.wall[0] = c.wall[1] = 60.0f;
    return c;
}

sb::WeaponPhysics::StepInput Input(double time) {
    sb::WeaponPhysics::StepInput in;
    in.time = time;
    in.head_valid = true;
    in.head_position = {0.0f, 1.65f, 0.0f};
    in.floor_y = 0.0f;
    return in;
}

void TestContacts() {
    float s = 0, t = 0;
    Vec3 c1, c2;
    // Crossing segments one unit apart.
    float d2 = sb::col::ClosestSegmentSegment({-1, 0, 0}, {1, 0, 0}, {0, -1, 1}, {0, 1, 1}, s, t,
                                              c1, c2);
    CHECK(std::fabs(d2 - 1.0f) < 1e-5f && Near(c1, {0, 0, 0}, 1e-5f) && Near(c2, {0, 0, 1}, 1e-5f));
    // Parallel and overlapping: the middle of the overlap.
    d2 = sb::col::ClosestSegmentSegment({0, 0, 0}, {2, 0, 0}, {1, 1, 0}, {3, 1, 0}, s, t, c1, c2);
    CHECK(std::fabs(d2 - 1.0f) < 1e-5f && Near(c1, {1.5f, 0, 0}, 1e-4f));
    // End to end.
    d2 = sb::col::ClosestSegmentSegment({0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}, s, t, c1, c2);
    CHECK(std::fabs(d2 - 1.0f) < 1e-5f && Near(c1, {1, 0, 0}, 1e-5f));

    // Capsules: A below B overlapping 5 cm; the normal points from B to A (down).
    const Capsule a{{-1, 0, 0}, {1, 0, 0}, 0.1f};
    sb::col::Contact c = sb::col::CapsuleCapsule(a, {{0, 0.15f, -1}, {0, 0.15f, 1}, 0.1f});
    CHECK(c.hit && std::fabs(c.depth - 0.05f) < 1e-5f && Near(c.normal, {0, -1, 0}, 1e-5f));
    CHECK(Near(c.point_a, {0, 0.1f, 0}, 1e-5f) && Near(c.point_b, {0, 0.05f, 0}, 1e-5f));
    c = sb::col::CapsuleCapsule(a, {{0, 0.3f, -1}, {0, 0.3f, 1}, 0.1f});
    CHECK(!c.hit && std::fabs(c.depth + 0.1f) < 1e-5f);
    // Axes crossing exactly: separate along the hint.
    c = sb::col::CapsuleCapsule(a, {{0, 0, -1}, {0, 0, 1}, 0.1f}, {0, 1, 0});
    CHECK(c.hit && std::fabs(c.depth - 0.2f) < 1e-5f && Near(c.normal, {0, 1, 0}, 1e-5f));
    // Spheres.
    c = sb::col::CapsuleCapsule({{0, 0, 0}, {0, 0, 0}, 0.5f}, {{0.8f, 0, 0}, {0.8f, 0, 0}, 0.5f});
    CHECK(c.hit && std::fabs(c.depth - 0.2f) < 1e-5f && Near(c.normal, {-1, 0, 0}, 1e-5f));

    // Box 2 x 1 x 2 at the origin.
    const sb::col::Box box{{0, 0, 0}, Quat{}, {1.0f, 0.5f, 1.0f}};
    Vec3 n;
    CHECK(std::fabs(sb::col::BoxSignedDistance(box, {0, 0.8f, 0}, &n) - 0.3f) < 1e-5f &&
          Near(n, {0, 1, 0}, 1e-5f));
    CHECK(std::fabs(sb::col::BoxSignedDistance(box, {0.9f, 0, 0}, &n) + 0.1f) < 1e-5f &&
          Near(n, {1, 0, 0}, 1e-5f));
    // Lying flat 5 cm into the top: the push is straight up from the middle (no twist).
    c = sb::col::CapsuleBox({{-0.5f, 0.55f, 0}, {0.5f, 0.55f, 0}, 0.1f}, box);
    CHECK(c.hit && std::fabs(c.depth - 0.05f) < 1e-4f && Near(c.normal, {0, 1, 0}, 1e-4f));
    CHECK(std::fabs(c.point_a.x) < 0.07f);
    // Near the edge (1, 0.5): the normal points out diagonally.
    c = sb::col::CapsuleBox({{1.2f, 0.7f, 0}, {2.0f, 1.5f, 0}, 0.3f}, box);
    CHECK(c.hit && std::fabs(c.depth - (0.3f - std::sqrt(0.08f))) < 2e-3f &&
          Near(c.normal, Normalize(Vec3{1, 1, 0}), 1e-2f));
    // Inside: out through the nearest face.
    c = sb::col::CapsuleBox({{-0.2f, 0.3f, 0}, {0.2f, 0.3f, 0}, 0.05f}, box);
    CHECK(c.hit && std::fabs(c.depth - 0.25f) < 1e-3f && Near(c.normal, {0, 1, 0}, 1e-4f));
    // A box turned 45 degrees about Z: its corner points up at y = sqrt(2) / 2.
    const sb::col::Box turned{{0, 0, 0}, AxisAngle({0, 0, 1}, kPi / 4), {0.5f, 0.5f, 0.5f}};
    CHECK(std::fabs(sb::col::BoxSignedDistance(turned, {0, 1, 0}) - (1.0f - std::sqrt(0.5f))) <
          1e-4f);
    // Separated.
    c = sb::col::CapsuleBox({{-0.5f, 0.9f, 0}, {0.5f, 0.9f, 0}, 0.1f}, box);
    CHECK(!c.hit);

    // Plane y = 0.
    c = sb::col::CapsulePlane({{0, 0.05f, 0}, {1, 0.2f, 0}, 0.1f}, sb::col::Plane{});
    CHECK(c.hit && std::fabs(c.depth - 0.05f) < 1e-5f && Near(c.point_a, {0, -0.05f, 0}, 1e-5f));

    // Eigen: points along (1, 1, 0) spread more than across.
    sb::col::Mat3 cov = sb::col::Outer(Normalize(Vec3{1, 1, 0}), Normalize(Vec3{1, 1, 0})) * 4.0f +
                        sb::col::Outer({0, 0, 1}, {0, 0, 1}) * 0.5f +
                        sb::col::Outer(Normalize(Vec3{1, -1, 0}), Normalize(Vec3{1, -1, 0})) * 0.1f;
    float values[3];
    Vec3 axes[3];
    sb::col::SymmetricEigen(cov, values, axes);
    CHECK(std::fabs(values[0] - 4.0f) < 1e-4f && std::fabs(values[1] - 0.5f) < 1e-4f &&
          std::fabs(values[2] - 0.1f) < 1e-4f);
    CHECK(std::fabs(std::fabs(Dot(axes[0], Normalize(Vec3{1, 1, 0}))) - 1.0f) < 1e-4f);
    // Inverse.
    const sb::col::Mat3 inv = sb::col::Inverse(cov) * cov;
    CHECK(std::fabs(inv(0, 0) - 1) < 1e-4f && std::fabs(inv(1, 1) - 1) < 1e-4f &&
          std::fabs(inv(0, 1)) < 1e-4f);
}

/// Every vertex within `slack` of a capsule; the share that is.
float Coverage(const sb::Model& model, const sb::WeaponShape& shape, float slack) {
    size_t inside = 0;
    for (const sb::Vertex& v : model.vertices) {
        const Vec3 p{v.position[0], v.position[1], v.position[2]};
        for (const Capsule& c : shape.capsules) {
            if (Length(p - sb::col::ClosestOnSegment(c.a, c.b, p)) <= c.r + slack) {
                ++inside;
                break;
            }
        }
    }
    return model.vertices.empty() ? 1.0f : float(inside) / float(model.vertices.size());
}

void TestShapes(const std::filesystem::path& assets) {
    const sb::PhysicsConfig cfg;
    const sb::Model bar = BarModel(0.8f, 0.06f, 0.02f);
    const sb::WeaponShape shape = sb::BuildWeaponShape(bar, cfg);
    CHECK(!shape.capsules.empty() && shape.capsules.size() <= 64);
    CHECK(Coverage(bar, shape, 0.008f) > 0.999f); // a square end's corners: ~8 mm
    CHECK(shape.median_radius <= 0.02f); // lanes: thin like the bar, not half its width
    CHECK(std::fabs(shape.reach - 0.8f) < 0.03f);
    CHECK(shape.com.y < 0.0f && shape.com.y > -0.4f); // between the hand and the bar's middle

    sb::Model cleaver;
    std::string error;
    if (!sb::LoadModel(assets / "wp_a_0800.json", cleaver, error)) {
        std::printf("skip the Saw Cleaver shape test: %s\n", error.c_str());
        return;
    }
    const sb::WeaponShape real = sb::BuildWeaponShape(cleaver, cfg);
    const float covered = Coverage(cleaver, real, 0.005f);
    std::printf("collision: Saw Cleaver %zu capsules, radius %.1f..%.1f cm (median %.1f; the R1 "
                "attack capsule is 40 cm), reach %.2f m, %.1f%% of the vertices within 5 mm\n",
                real.capsules.size(), real.min_radius * 100.0f,
                std::max_element(real.capsules.begin(), real.capsules.end(),
                                 [](const Capsule& a, const Capsule& b) { return a.r < b.r; })
                        ->r * 100.0f,
                real.median_radius * 100.0f, real.reach, covered * 100.0f);
    CHECK(covered > 0.95f);
    CHECK(real.median_radius < 0.05f);
    CHECK(real.capsules.size() >= 8);
}

void TestPushOut() {
    sb::PhysicsConfig cfg = QuietConfig();
    cfg.wall[0] = 1.0f; // face at x = 1, thickness along +x
    cfg.wall[1] = 0.0f;
    const sb::Model bar = BarModel(0.8f, 0.06f, 0.02f);
    sb::WeaponPhysics physics;
    sb::PhysicsEvents events;
    // The bar points at the wall with its tip 3 cm in.
    sb::WeaponPhysics::StepInput in = Input(0.0);
    in.held[1].model = &bar;
    in.held[1].name = "bar";
    in.held[1].target = BarPose({0.23f, 1.2f, 0.0f}, {1, 0, 0});
    physics.Step(in, cfg, events);
    const sb::WeaponPhysics::Hand& hand = physics.GetHand(1);
    CHECK(physics.ResidualDepth() < 0.001f);
    const Vec3 tip = sb::col::Apply(hand.pose, {0, -0.8f, 0});
    CHECK(tip.x < 1.0f + 0.001f && tip.x > 0.95f); // pushed back along the wall's normal
    CHECK(std::fabs(tip.y - 1.2f) < 0.01f);
    // Pressed in at 45 degrees: out again within a few frames, mostly by turning.
    in.held[1].target = BarPose({0.5f, 1.2f, -0.2f}, {1, 0, 1});
    float worst = 0.0f;
    for (int frame = 1; frame <= 10; ++frame) {
        in.time = frame / 72.0;
        physics.Step(in, cfg, events);
        if (frame >= 3) {
            worst = std::max(worst, physics.ResidualDepth());
        }
    }
    CHECK(worst < 0.002f);
    const Vec3 tip2 = sb::col::Apply(physics.GetHand(1).pose, {0, -0.8f, 0});
    CHECK(tip2.x < 1.0f + 0.002f);
}

/// A swing about the grip at `rate` Hz with the tip at `tip_speed` m/s from heading `from` to
/// `to` (radians, XZ plane, y = 1.2 m), then `hold` frames at the end. Returns the largest
/// heading the right blade reached.
struct SwingResult {
    float max_heading = -10.0f, worst_depth = 0.0f;
    int substeps = 0;
};
SwingResult Swing(sb::WeaponPhysics& physics, const sb::PhysicsConfig& cfg, const sb::Model& bar,
                  float rate, float tip_speed, float from, float to, int hold,
                  const sb::Model* left = nullptr, Mat4 left_pose = {}) {
    SwingResult r;
    const float omega = tip_speed / 0.8f;
    const int frames = int(std::ceil(std::fabs(to - from) / omega * rate));
    sb::PhysicsEvents events;
    for (int frame = 0; frame <= frames + hold; ++frame) {
        const float t = std::min(float(frame) / float(std::max(frames, 1)), 1.0f);
        sb::WeaponPhysics::StepInput in = Input(frame / double(rate));
        in.held[1].model = &bar;
        in.held[1].name = "bar";
        in.held[1].target = LevelPose({0, 1.2f, 0}, from + (to - from) * t);
        if (left) {
            in.held[0].model = left;
            in.held[0].name = "left";
            in.held[0].target = left_pose;
        }
        physics.Step(in, cfg, events);
        r.max_heading = std::max(r.max_heading, Heading(physics.GetHand(1).pose));
        r.worst_depth = std::max(r.worst_depth, physics.ResidualDepth());
        r.substeps = std::max(r.substeps, physics.LastSubsteps());
    }
    return r;
}

void TestNoTunnelling() {
    const sb::Model bar = BarModel(0.8f, 0.03f, 0.02f);
    for (const float rate : {72.0f, 90.0f}) {
        // A thin pole (3.5 cm) 0.5 m from the grip at heading 0; the tip moves at 20 m/s
        // (0.25-0.31 m per frame, far more than the pole and the blade are thick).
        sb::PhysicsConfig cfg = QuietConfig();
        cfg.post[0] = 0.5f;
        cfg.post[1] = 0.0f;
        cfg.post_radius = 0.035f;
        cfg.post_height = 2.0f;
        cfg.crossbar_length = 0.0f;
        sb::WeaponPhysics physics;
        const SwingResult hit = Swing(physics, cfg, bar, rate, 20.0f, -1.2f, 1.2f, 12);
        CHECK(hit.max_heading < 0.0f); // never on the far side of the pole
        CHECK(hit.worst_depth < 0.003f);
        CHECK(hit.substeps > 1);
        // The same swing without collision passes through (the test can fail).
        cfg.enabled = false;
        sb::WeaponPhysics ghost;
        CHECK(Swing(ghost, cfg, bar, rate, 20.0f, -1.2f, 1.2f, 2).max_heading > 1.0f);
        std::printf("collision: %.0f Hz, tip 20 m/s at a 3.5 cm pole: stopped at heading %.3f rad, "
                    "deepest %.1f mm, up to %d substeps\n",
                    rate, hit.max_heading, hit.worst_depth * 1000.0f, hit.substeps);

        // Blade against blade: the left hand holds a bar upright 0.5 m ahead of the right grip.
        cfg = QuietConfig();
        sb::WeaponPhysics duel;
        const Mat4 upright = BarPose({0.5f, 0.85f, 0.0f}, {0, 1, 0});
        const SwingResult cross = Swing(duel, cfg, bar, rate, 20.0f, -1.2f, 1.2f, 12, &bar, upright);
        const sb::col::Pose& left = duel.GetHand(0).pose;
        const float left_heading = std::atan2(left.x.z, left.x.x);
        CHECK(cross.max_heading < left_heading + 0.02f);
        CHECK(cross.worst_depth < 0.003f);
        CHECK(duel.GetHand(0).separation > 0.0f); // the left blade was pushed, too
        std::printf("collision: %.0f Hz, blade vs blade at 20 m/s: right stopped at %.3f rad, "
                    "left pushed %.1f cm, deepest %.1f mm\n",
                    rate, cross.max_heading, duel.GetHand(0).separation * 100.0f,
                    cross.worst_depth * 1000.0f);
    }
}

void TestSnap() {
    // Pushing far through the pole: the weapon lets go (snaps back into the hand) instead of
    // dragging the hand forever.
    sb::PhysicsConfig cfg = QuietConfig();
    cfg.post[0] = 0.5f;
    cfg.post[1] = 0.0f;
    cfg.crossbar_length = 0.0f;
    // The separation is an arc (angle x reach): 0.8 m of arc is ~46 degrees of this 0.8 m bar.
    cfg.max_separation_arc_m = 0.8f;
    const sb::Model bar = BarModel(0.8f, 0.03f, 0.02f);
    sb::WeaponPhysics physics;
    Swing(physics, cfg, bar, 72.0f, 3.0f, -1.0f, 1.2f, 20);
    CHECK(Heading(physics.GetHand(1).pose) > 1.0f); // in the hand on the far side
    // The same push with generous limits never lets go: the blade stays in front of the pole.
    cfg.max_separation_arc_m = 50.0f;
    cfg.max_separation_m = 50.0f;
    sb::WeaponPhysics held;
    Swing(held, cfg, bar, 72.0f, 3.0f, -1.0f, 1.2f, 20);
    CHECK(Heading(held.GetHand(1).pose) < 0.5f);
}

void TestHits() {
    sb::PhysicsConfig cfg = QuietConfig();
    const sb::Model bar = BarModel(0.8f, 0.03f, 0.02f);
    std::vector<sb::HitShape> attack(1);
    attack[0].p[0].pos = {0, -0.3f, 0};
    attack[0].p[1].pos = {0, -0.8f, 0};
    attack[0].radius = 0.1f;
    attack[0].capsule = true;
    // A torso 0.55 m from the grip at heading 0.
    const std::vector<Capsule> torso = {{{0.55f, 0.9f, 0}, {0.55f, 1.5f, 0}, 0.15f}};
    for (const float speed : {8.0f, 0.6f}) {
        sb::WeaponPhysics physics;
        physics.SetEnemyParts(torso, Mat4{});
        sb::PhysicsEvents events;
        int pulses = 0;
        float strongest = 0.0f;
        const float omega = speed / 0.8f;
        const int frames = int(std::ceil(1.2f / omega * 72.0f));
        for (int frame = 0; frame <= frames + 10; ++frame) {
            const float t = std::min(float(frame) / float(frames), 1.0f);
            sb::WeaponPhysics::StepInput in = Input(frame / 72.0);
            in.held[1].model = &bar;
            in.held[1].name = "bar";
            in.held[1].attack = attack;
            in.held[1].target = LevelPose({0, 1.2f, 0}, -0.9f + 1.2f * t);
            events = {};
            physics.Step(in, cfg, events);
            if (events.haptic_amplitude[1] > 0.0f) {
                ++pulses;
                strongest = std::max(strongest, events.haptic_amplitude[1]);
            }
        }
        if (speed > 2.0f) {
            CHECK(physics.HitCount() == 1); // once per swing
            CHECK(std::fabs(strongest - cfg.hit_amplitude) < 1e-4f);
            CHECK(!physics.Markers().empty());
        } else {
            CHECK(physics.HitCount() == 0); // too slow: only the contact pulse
            CHECK(pulses >= 1 && strongest < cfg.hit_amplitude);
        }
        // A slow blade stops at the body; a swing in progress passes through it (and hits).
        CHECK((speed > 2.0f) == (Heading(physics.GetHand(1).pose) > 0.25f));
    }
}

/// The VR hit scale (src/vr_hit_scale.h through hit_scale.h): the shape maths, then the same swing
/// at a torso 0.15 m too far for the game's capsule: a miss unscaled, a hit with the VR defaults.
void TestHitScale() {
    const VhsParams id = vhs_identity();
    const VhsParams vr = vhs_defaults();
    sb::HitShape blade;
    blade.p[0].pos = {0, -0.3f, 0};
    blade.p[1].pos = {0, -0.8f, 0};
    blade.radius = 0.1f;
    blade.capsule = true;
    sb::HitShape root = blade;
    root.p[0].pos = {0, 0, 0};
    root.p[1].pos = {0, -0.1f, 0};
    sb::Hitboxes boxes;
    boxes.attacks.resize(1);
    boxes.attacks[0].hits = {blade, root};
    const float reach = sb::HitWeaponReach(boxes);
    CHECK(std::fabs(reach - 0.8f) < 1e-5f);
    // Identity changes nothing; the defaults grow the radius and lengthen the tip shape only.
    const sb::HitShape same = sb::ScaleHitShape(id, blade, reach);
    CHECK(same.radius == blade.radius && Near(same.p[1].pos, blade.p[1].pos, 1e-6f));
    const sb::HitShape big = sb::ScaleHitShape(vr, blade, reach);
    CHECK(std::fabs(big.radius - 0.15f) < 1e-5f);
    CHECK(Near(big.p[1].pos, {0, -0.95f, 0}, 1e-5f) && Near(big.p[0].pos, blade.p[0].pos, 1e-6f));
    const sb::HitShape big_root = sb::ScaleHitShape(vr, root, reach);
    CHECK(std::fabs(big_root.radius - 0.15f) < 1e-5f && Near(big_root.p[1].pos, root.p[1].pos, 1e-6f));
    // Order of the endpoints does not matter: the far end is the one that grows.
    sb::HitShape flipped = blade;
    std::swap(flipped.p[0], flipped.p[1]);
    CHECK(Near(sb::ScaleHitShape(vr, flipped, reach).p[0].pos, {0, -0.95f, 0}, 1e-5f));
    // A sphere at the weapon head becomes a capsule going outward.
    sb::HitShape head;
    head.p[0].pos = head.p[1].pos = {0, -0.7f, 0};
    head.radius = 0.5f;
    const sb::HitShape head_big = sb::ScaleHitShape(vr, head, 0.7f);
    CHECK(head_big.capsule && std::fabs(head_big.radius - 0.75f) < 1e-5f &&
          Near(head_big.p[1].pos, {0, -0.85f, 0}, 1e-5f));
    // A kick (body-sourced) only gets the radius; the cap holds.
    sb::HitShape kick = blade;
    kick.p[0].on_body = true;
    CHECK(Near(sb::ScaleHitShape(vr, kick, reach).p[1].pos, kick.p[1].pos, 1e-6f));
    sb::HitShape huge = blade;
    huge.radius = 1.0f;
    CHECK(sb::ScaleHitShape(vr, huge, reach).radius <= VHS_RADIUS_MAX_M + 1e-6f);
    // Per class: a long weapon (reach >= 1.1 m) can differ from the short ones.
    VhsParams cls = vr;
    cls.class_radius_scale[VHS_LONG] = 2.0f;
    cls.class_reach_m[VHS_LONG] = 0.0f;
    CHECK(std::fabs(sb::ScaleHitShape(cls, blade, 1.2f).radius - 0.2f) < 1e-5f);
    CHECK(Near(sb::ScaleHitShape(cls, blade, 1.2f).p[1].pos, blade.p[1].pos, 1e-6f));
    CHECK(std::fabs(sb::ScaleHitShape(cls, blade, 0.8f).radius - 0.15f) < 1e-5f);
    // A negative reach never turns a capsule inside out.
    VhsParams neg = vr;
    neg.reach_m = -0.3f;
    CHECK(Dot(sb::ScaleHitShape(neg, blade, reach).p[1].pos - blade.p[0].pos, {0, -1, 0}) >= 0.0f);
    // The control-file keys.
    VhsParams keys = vhs_identity();
    CHECK(vhs_apply_key(&keys, "hit", "1") && keys.enabled);
    CHECK(vhs_apply_key(&keys, "hit_scale", "2.5") && keys.radius_scale == 2.5f);
    CHECK(vhs_apply_key(&keys, "hit_reach", "0.2") && std::fabs(keys.reach_m - 0.2f) < 1e-6f);
    CHECK(vhs_apply_key(&keys, "hit_scale_l", "3") && keys.class_radius_scale[VHS_LONG] == 3.0f);
    CHECK(vhs_apply_key(&keys, "hit_reach_s", "-2") && keys.class_reach_m[VHS_SHORT] < VHS_REACH_MIN_M);
    CHECK(vhs_apply_key(&keys, "hit_scale", "99") && keys.radius_scale == VHS_SCALE_MAX);
    CHECK(!vhs_apply_key(&keys, "arm_weight", "1") && !vhs_apply_key(&keys, "hit_nope", "1"));
    // The config file: "physics": {"hit": {...}}.
    {
        sb::PhysicsConfig c;
        json::Value doc = json::Parse(
            "{\"hit\": {\"radius_scale\": 2, \"reach_m\": 0.3, \"long\": {\"radius_scale\": 1}}}");
        c.Load(doc);
        CHECK(c.hit.enabled && c.hit.radius_scale == 2.0f && std::fabs(c.hit.reach_m - 0.3f) < 1e-6f);
        CHECK(c.hit.class_radius_scale[VHS_LONG] == 1.0f && c.hit.class_radius_scale[VHS_SHORT] < 0.0f);
        sb::PhysicsConfig defaults;
        CHECK(defaults.hit.radius_scale == 1.5f && std::fabs(defaults.hit.reach_m - 0.15f) < 1e-6f);
        CHECK(defaults.Summary().find("\"hit\"") != std::string::npos);
    }

    // The swing: torso surface at x = 0.85 m, the unscaled tip shape (r 0.05 x 0.6 = 3 cm) ends at
    // 0.83 m. Scaled (r 0.075 x 0.6 = 4.5 cm, tip at 0.95 m) it reaches it.
    sb::PhysicsConfig cfg = QuietConfig();
    const sb::Model bar = BarModel(0.8f, 0.03f, 0.02f);
    sb::HitShape tip = blade;
    tip.radius = 0.05f;
    const std::vector<Capsule> torso = {{{1.0f, 0.9f, 0}, {1.0f, 1.5f, 0}, 0.15f}};
    int hits[2] = {};
    for (int scaled = 0; scaled < 2; ++scaled) {
        sb::WeaponPhysics physics;
        physics.SetEnemyParts(torso, Mat4{});
        sb::PhysicsEvents events;
        const float omega = 10.0f;
        const int frames = int(std::ceil(1.2f / omega * 72.0f));
        for (int frame = 0; frame <= frames + 10; ++frame) {
            const float t = std::min(float(frame) / float(frames), 1.0f);
            sb::WeaponPhysics::StepInput in = Input(frame / 72.0);
            in.held[1].model = &bar;
            in.held[1].name = "bar";
            in.held[1].attack = {scaled ? sb::ScaleHitShape(vr, tip, 0.8f) : tip};
            in.held[1].target = LevelPose({0, 1.2f, 0}, -0.9f + 1.2f * t);
            events = {};
            physics.Step(in, cfg, events);
        }
        hits[scaled] = physics.HitCount();
    }
    CHECK(hits[0] == 0);
    CHECK(hits[1] == 1);
}

/// A swing of `bar` (grip y = 1.2 m) from heading `from` to `to` (and back to `back`, when given)
/// at `omega` rad/s through the torso at heading 0, 0.55 m out.
struct TorsoRun {
    int hits = 0;
    float heading = 0.0f;
    bool snapped = false, held_back = false;
};
TorsoRun SwingTorso(sb::WeaponPhysics& physics, const sb::PhysicsConfig& cfg, const sb::Model& bar,
                    float omega, float from, float to, float back = 99.0f, int hold = 6) {
    std::vector<sb::HitShape> attack(1);
    attack[0].p[0].pos = {0, -0.3f, 0};
    attack[0].p[1].pos = {0, -0.8f, 0};
    attack[0].radius = 0.1f;
    attack[0].capsule = true;
    const std::vector<Capsule> torso = {{{0.55f, 0.9f, 0}, {0.55f, 1.5f, 0}, 0.15f}};
    physics.SetEnemyParts(torso, Mat4{});
    TorsoRun run;
    const float step = omega / 72.0f;
    float heading = from;
    float dir = to > from ? 1.0f : -1.0f;
    bool returning = false;
    int frame = 0, still = 0;
    while (still < hold && frame < 2000) {
        sb::WeaponPhysics::StepInput in = Input(frame / 72.0);
        in.held[1].model = &bar;
        in.held[1].name = "bar";
        in.held[1].attack = attack;
        in.held[1].target = LevelPose({0, 1.2f, 0}, heading);
        sb::PhysicsEvents events;
        physics.Step(in, cfg, events);
        run.snapped = run.snapped || events.log.find("snaps back") != std::string::npos;
        run.held_back = run.held_back || physics.GetHand(1).separation_deg > 1.0f;
        const float goal = returning ? back : to;
        if (std::fabs(goal - heading) < 0.5f * step) {
            if (!returning && back < 90.0f) {
                returning = true;
                dir = -dir;
                heading += dir * step; // the apex is on a frame: a full reversal
            } else {
                ++still;
            }
        } else {
            heading += dir * std::min(step, std::fabs(goal - heading));
        }
        ++frame;
    }
    run.hits = physics.HitCount();
    run.heading = Heading(physics.GetHand(1).pose);
    return run;
}

void TestSwingThrough() {
    const sb::Model bar = BarModel(0.8f, 0.03f, 0.02f);
    // A fast swing through the enemy is not blocked and not snapped, and still hits (once).
    sb::PhysicsConfig cfg = QuietConfig();
    sb::WeaponPhysics fast;
    TorsoRun run = SwingTorso(fast, cfg, bar, 10.0f, -0.9f, 0.9f);
    CHECK(run.hits == 1);
    CHECK(run.heading > 0.8f); // it arrived at the end of the path
    CHECK(!run.snapped && !fast.GetHand(1).passthrough);
    // A slow push is stopped by the enemy as before (and hits nothing).
    sb::WeaponPhysics slow;
    run = SwingTorso(slow, cfg, bar, 1.0f, -0.9f, 0.9f, 99.0f, 20);
    CHECK(run.hits == 0 && Heading(slow.GetHand(1).pose) < 0.25f);
    // Switched off, the fast swing is held back by the torso -- and the hit still registers: the
    // attack capsules are tested where the hand puts the weapon, not where the blocker holds it.
    cfg.swing_through_enemy = false;
    sb::WeaponPhysics blocked;
    run = SwingTorso(blocked, cfg, bar, 10.0f, -0.9f, 0.9f);
    CHECK(run.hits == 1);
    CHECK(run.held_back);
    // The post stays solid for a fast swing.
    cfg = QuietConfig();
    cfg.post[0] = 0.5f;
    cfg.post[1] = 0.0f;
    cfg.crossbar_length = 0.0f;
    cfg.max_separation_m = 50.0f;
    cfg.max_separation_arc_m = 50.0f;
    sb::WeaponPhysics post;
    const SwingResult stopped = Swing(post, cfg, bar, 72.0f, 8.0f, -1.0f, 1.2f, 10);
    CHECK(stopped.max_heading < 0.3f);
}

void TestResets() {
    const sb::Model bar = BarModel(0.8f, 0.03f, 0.02f);
    const sb::Model other = BarModel(0.6f, 0.03f, 0.02f);
    const sb::PhysicsConfig cfg = QuietConfig();
    std::vector<sb::HitShape> attack(1);
    attack[0].p[0].pos = {0, -0.3f, 0};
    attack[0].p[1].pos = {0, -0.8f, 0};
    attack[0].radius = 0.1f;
    attack[0].capsule = true;
    const std::vector<Capsule> torso = {{{0.55f, 0.9f, 0}, {0.55f, 1.5f, 0}, 0.15f}};
    // After a hit (the swing is still on): a raw controller jump of 0.35 m, or a turn of 100
    // degrees, resets the swing -- even while the (IK-clamped) target hardly moved; 0.25 m does
    // not; a new weapon resets it too.
    for (const int variant : {0, 1, 2, 3, 4}) {
        sb::WeaponPhysics physics;
        physics.SetEnemyParts(torso, Mat4{});
        sb::PhysicsEvents events;
        const Vec3 raw{0.2f, 1.2f, 0.0f};
        const Quat raw_q;
        float heading = -0.9f;
        int frame = 0;
        const auto step = [&](double t, Vec3 pos, Quat q, bool swap) {
            sb::WeaponPhysics::StepInput in = Input(t);
            in.held[1].model = swap ? &other : &bar;
            in.held[1].name = "bar";
            in.held[1].attack = attack;
            in.held[1].target = LevelPose({0, 1.2f, 0}, heading);
            in.controller_valid[1] = true;
            in.controller_position[1] = pos;
            in.controller_rotation[1] = q;
            events = {};
            physics.Step(in, cfg, events);
        };
        while (physics.HitCount() == 0 && frame < 100) {
            step(frame / 72.0, raw, raw_q, false);
            heading += 10.0f / 72.0f;
            ++frame;
        }
        CHECK(physics.HitCount() == 1);
        CHECK(physics.GetHand(1).hit_this_swing && physics.GetHand(1).overlap);
        const Vec3 jumped = variant == 1 ? raw + Vec3{0.35f, 0, 0}
                            : variant == 2 ? raw + Vec3{0.25f, 0, 0} : raw;
        const Quat turned = variant == 3 ? AxisAngle({0, 1, 0}, 100.0f * kDeg) : raw_q;
        step(frame / 72.0, jumped, turned, variant == 4);
        const sb::WeaponPhysics::Hand& hand = physics.GetHand(1);
        if (variant == 1 || variant == 3) {
            CHECK(!hand.hit_this_swing && !hand.swinging && hand.slow_time < 0.05f);
            CHECK(events.log.find("weapon reset") != std::string::npos);
        } else if (variant == 4) {
            CHECK(!hand.hit_this_swing); // a new weapon starts clean
        } else {
            CHECK(hand.hit_this_swing); // 0.25 m is below the glitch distance
        }
    }
    // One distance for physics and swing logic.
    const sb::PhysicsConfig defaults;
    CHECK(std::fabs(defaults.teleport_m - swing::SwingTuning{}.glitch_m) < 1e-6f);
    CHECK(defaults.teleport_deg == 90.0f);
}

void TestRearm() {
    // Forehand then backhand through the enemy with no pause: the reversal rearms the swing and
    // the backhand hits as well; with the rule off it does not.
    const sb::Model bar = BarModel(0.8f, 0.03f, 0.02f);
    sb::PhysicsConfig cfg = QuietConfig();
    sb::WeaponPhysics two;
    TorsoRun run = SwingTorso(two, cfg, bar, 10.0f, -0.9f, 0.3f, -0.9f);
    CHECK(run.hits == 2);
    cfg.rearm_reverse_deg = 0.0f;
    sb::WeaponPhysics one;
    run = SwingTorso(one, cfg, bar, 10.0f, -0.9f, 0.3f, -0.9f);
    CHECK(run.hits == 1);
    // Continuing the same direction (a long arc) is one swing.
    cfg = QuietConfig();
    sb::WeaponPhysics arc;
    run = SwingTorso(arc, cfg, bar, 10.0f, -0.9f, 1.5f);
    CHECK(run.hits == 1);
}

void TestProbeAndShapes() {
    // The swing probe is the fastest attack shape or the tip: an attack capsule near the grip
    // does not make a fast swing look slow.
    const sb::Model bar = BarModel(0.8f, 0.03f, 0.02f);
    const sb::PhysicsConfig cfg = QuietConfig();
    sb::WeaponPhysics physics;
    std::vector<sb::HitShape> near_grip(1);
    near_grip[0].p[0].pos = {0, -0.05f, 0};
    near_grip[0].p[1].pos = {0, -0.15f, 0};
    near_grip[0].radius = 0.05f;
    near_grip[0].capsule = true;
    float heading = 0.0f;
    for (int frame = 0; frame < 4; ++frame) {
        sb::WeaponPhysics::StepInput in = Input(frame / 72.0);
        in.held[1].model = &bar;
        in.held[1].name = "bar";
        in.held[1].attack = near_grip;
        in.held[1].target = LevelPose({0, 1.2f, 0}, heading);
        sb::PhysicsEvents events;
        physics.Step(in, cfg, events);
        heading += 10.0f / 72.0f;
    }
    const float speed = physics.GetHand(1).swing_speed;
    CHECK(speed > 0.9f * 10.0f * 0.7f && speed < 1.1f * 10.0f * 0.85f); // the tip, not 1 m/s

    // Default shape tuning: tighter blocking capsules.
    const sb::PhysicsConfig defaults;
    CHECK(defaults.radius_max_m <= 0.05f + 1e-6f);
    const sb::WeaponShape fat = sb::BuildWeaponShape(BarModel(0.8f, 0.4f, 0.4f), defaults);
    for (const Capsule& c : fat.capsules) {
        CHECK(c.r <= 0.05f + 1e-6f);
    }

    // Body-sourced hit shapes (kicks) are skipped without a resolver, and resolved bone-relative
    // (here: moved into the weapon's space by the resolver) with one.
    sb::Hitboxes boxes;
    sb::Attack kick;
    kick.name = "kick";
    sb::HitShape shape;
    shape.p[0].on_body = shape.p[1].on_body = true;
    shape.p[0].pos = {1, 2, 3};
    shape.p[1].pos = {1, 3, 3};
    shape.radius = 0.1f;
    shape.capsule = true;
    kick.hits.push_back(shape);
    sb::HitShape blade;
    blade.p[0].pos = {0, -0.3f, 0};
    blade.p[1].pos = {0, -0.8f, 0};
    blade.capsule = true;
    kick.hits.push_back(blade);
    boxes.attacks.push_back(kick);
    CHECK(sb::AttackShapesFor(boxes, "", -1).size() == 1);
    const auto resolved = sb::AttackShapesFor(
        boxes, "", -1, nullptr, [](const sb::ShapePoint& p) { return p.pos + Vec3{0, 10, 0}; });
    CHECK(resolved.size() == 2);
    CHECK(resolved.size() == 2 && Near(resolved[0].p[0].pos, {1, 12, 3}, 1e-5f) &&
          !resolved[0].p[0].on_body);
}

void TestHaptics() {
    // Into the wall at two speeds: the faster impact pulses harder.
    float amplitude[2] = {};
    for (int k = 0; k < 2; ++k) {
        sb::PhysicsConfig cfg = QuietConfig();
        cfg.wall[0] = 1.0f;
        cfg.wall[1] = 0.0f;
        const sb::Model bar = BarModel(0.8f, 0.03f, 0.02f);
        sb::WeaponPhysics physics;
        const float speed = k ? 3.0f : 0.5f;
        for (int frame = 0; frame < 72; ++frame) {
            sb::WeaponPhysics::StepInput in = Input(frame / 72.0);
            in.held[1].model = &bar;
            in.held[1].name = "bar";
            in.held[1].target = BarPose({0.0f + speed * float(frame) / 72.0f, 1.2f, 0.0f}, {1, 0, 0});
            sb::PhysicsEvents events;
            physics.Step(in, cfg, events);
            if (events.haptic_amplitude[1] > 0.0f && amplitude[k] == 0.0f) {
                amplitude[k] = events.haptic_amplitude[1];
            }
        }
    }
    CHECK(amplitude[0] > 0.0f && amplitude[1] > amplitude[0]);

    // Pressed against the wall and dragged sideways: a light buzz, repeated.
    sb::PhysicsConfig cfg = QuietConfig();
    cfg.wall[0] = 1.0f;
    cfg.wall[1] = 0.0f;
    const sb::Model bar = BarModel(0.8f, 0.03f, 0.02f);
    sb::WeaponPhysics physics;
    int buzzes = 0;
    float loudest = 0.0f;
    for (int frame = 0; frame < 72; ++frame) {
        sb::WeaponPhysics::StepInput in = Input(frame / 72.0);
        in.held[1].model = &bar;
        in.held[1].name = "bar";
        // 5 cm into the wall, sliding along z at 1 m/s after the first 10 frames.
        const float z = frame < 10 ? 0.0f : float(frame - 10) / 72.0f;
        in.held[1].target = BarPose({0.25f, 1.2f, z}, {1, 0, 0});
        sb::PhysicsEvents events;
        physics.Step(in, cfg, events);
        if (frame > 12 && events.haptic_amplitude[1] > 0.0f) {
            ++buzzes;
            loudest = std::max(loudest, events.haptic_amplitude[1]);
        }
    }
    CHECK(buzzes >= 10); // every buzz_interval while it grinds
    CHECK(loudest > 0.0f && loudest <= cfg.slide_amplitude_max + 1e-4f);
}

} // namespace

/// Called by rig_test.cpp's main. Returns the failures; adds the checks run.
int RunCollisionTests(const std::filesystem::path& assets, int& total_checks) {
    failures = checks = 0;
    TestContacts();
    TestShapes(assets);
    TestPushOut();
    TestNoTunnelling();
    TestSnap();
    TestHits();
    TestHitScale();
    TestSwingThrough();
    TestResets();
    TestRearm();
    TestProbeAndShapes();
    TestHaptics();
    std::printf("collision tests: %d checks, %d failed\n", checks, failures);
    total_checks += checks;
    return failures;
}
