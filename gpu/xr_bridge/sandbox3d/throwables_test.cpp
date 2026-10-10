// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_sandbox3d_throw_test.exe [out/vr_assets]: offline tests of the sandbox's hand-thrown
// goods (throwables.h, docs/vr/findings/35-throwables.md), no headset. A scripted hand (90 Hz,
// controller pose + squeeze + trigger) grabs items from the rack and lets go at chosen speeds:
//   - the built-in rows against the dump's Bullet values, the extracted-table override;
//   - the hand velocity estimator (constant, ramp-and-stop, noise);
//   - grab (radius, squeeze, nearest), the item riding the hand;
//   - throws against an independent reference integration of the Bullet rules: range, straight
//     knives, speed scaling and the max-velocity cap, the fan of three, the delayed Molotov and
//     its fuse, back throws and weak flicks (drops), the enemy (pierced or not), Tiny Tonitrus'
//     life, the game-style throw (trigger, along the view), the respawn and mode buttons, the log;
//   - the extracted meshes (when out/vr_assets/throwables exists) and the procedural stand-ins.
// Exit code 0 when every check passes. BB_THROW_TEST_VERBOSE=1 prints the throw logs.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "bbport_xr_bridge.h"
#include "model.h"
#include "physics_weapon.h"
#include "throwables.h"

using namespace m3;

namespace {

int failures = 0, checks = 0;

void Check(bool ok, const char* what, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL throwables_test.cpp line %d: %s\n", line, what);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

bool Near(float a, float b, float tol) {
    return std::fabs(a - b) <= tol;
}
#define CHECK_NEAR(a, b, tol)                                                              \
    do {                                                                                   \
        const float va = float(a), vb = float(b);                                          \
        Check(Near(va, vb, float(tol)), #a " ~ " #b, __LINE__);                            \
        if (!Near(va, vb, float(tol))) {                                                   \
            std::printf("    %s = %g, %s = %g (tol %g)\n", #a, double(va), #b, double(vb), \
                        double(tol));                                                      \
        }                                                                                  \
    } while (0)

bool Verbose() {
    return std::getenv("BB_THROW_TEST_VERBOSE") != nullptr;
}

constexpr double kDt = 1.0 / 90.0;
const Vec3 kHead{0.0f, 1.65f, 0.0f};

int DefIndex(const sb::Throwables& t, int goods) {
    for (size_t i = 0; i < t.Defs().size(); ++i) {
        if (t.Defs()[i].goods == goods) {
            return int(i);
        }
    }
    return -1;
}

/// The Bullet rules, integrated finely and independently of the sandbox's stepping: gravity
/// switches at `dist` metres of travel, the speed is capped, the bullet ends on the floor
/// (centre at the sphere radius), at `life`, or never when neither happens within `limit`.
struct Ref {
    float range = 0.0f, path = 0.0f, time = 0.0f;
    Vec3 end;
    bool floor = false;
};
Ref Reference(const sb::ThrowItemDef& d, Vec3 origin, Vec3 dir, float speed) {
    Vec3 p = origin, v = dir * speed;
    float travelled = 0.0f, t = 0.0f;
    const float h = 0.0002f;
    Ref r;
    while (t < d.life_s + 1e-6f) {
        const bool in_range = d.range_m > 0.0f && travelled < d.range_m;
        float s = Length(v);
        const Vec3 u = s > 1e-6f ? v * (1.0f / s) : Vec3{0, -1, 0};
        if (in_range && d.accel_in > 0.0f) {
            s += d.accel_in * h;
        }
        v = u * s;
        v.y -= (in_range ? d.gravity_in : d.gravity_out) * h;
        s = Length(v);
        if (d.max_velocity > 0.0f && s > d.max_velocity) {
            v = v * (d.max_velocity / s);
        }
        p = p + v * h;
        travelled += Length(v) * h;
        t += h;
        if (p.y <= d.radius_m) {
            r.floor = true;
            break;
        }
    }
    r.end = p;
    r.path = travelled;
    r.time = t;
    r.range = std::hypot(p.x - origin.x, p.z - origin.z);
    return r;
}

/// The rack, the weapon physics' world (floor only unless asked) and a scripted right hand.
struct Sim {
    sb::Throwables t;
    sb::ThrowConfig cfg;
    sb::WeaponPhysics phys;
    sb::PhysicsConfig pc;
    sb::ThrowInput in;
    double time = 0.0;
    std::string log;
    sb::ThrowEvents last;

    explicit Sim(bool props = false, const char* mode = "hand") {
        cfg.log = false;
        cfg.log_file = "";
        cfg.mode = mode;
        pc.props = props;
        pc.enemy = "";
        pc.contact_log = "";
        pc.log_contacts = false;
        std::string l;
        t.Load("no-such-assets", cfg, l);
        t.Place(kHead, Quat{}, 0.0f, cfg);
        in.head_valid = true;
        in.head_position = kHead;
        in.head_rotation = Quat{};
        for (int s = 0; s < 2; ++s) {
            in.hand_valid[s] = true;
            in.grip_rotation[s] = Quat{};
            in.grip_position[s] = {s ? 0.3f : -0.3f, 1.0f, -0.2f};
        }
        phys.PlaceProps(kHead, Quat{});
        Physics();
    }
    void Physics() {
        sb::WeaponPhysics::StepInput si;
        si.time = time;
        si.head_valid = true;
        si.head_position = kHead;
        si.floor_y = 0.0f;
        sb::PhysicsEvents pe;
        phys.Step(si, pc, pe);
    }
    void Tick() {
        time += kDt;
        in.time = time;
        last = {};
        t.Step(in, cfg, phys, last);
        log += last.log;
        if (Verbose() && !last.log.empty()) {
            std::printf("  %7.3f %s", time, last.log.c_str());
        }
    }
    void Run(double seconds) {
        for (double end = time + seconds; time < end - 1e-9;) {
            Tick();
        }
    }
    sb::Throwables::Item& ItemOf(int goods) { return const_cast<sb::Throwables::Item&>(t.Items()[size_t(DefIndex(t, goods))]); }
    /// Puts the right hand so that the item's centre is in the hand, squeeze `s`.
    void HandAt(int hand, Vec3 item_pos, int goods, float squeeze) {
        in.grip_position[hand] = item_pos - t.Defs()[size_t(DefIndex(t, goods))].grip.position;
        in.squeeze[hand] = squeeze;
    }
    bool Grab(int goods, int hand = 1) {
        const Vec3 home = ItemOf(goods).home;
        HandAt(hand, home, goods, 0.0f);
        Tick();
        in.squeeze[hand] = 0.9f;
        Tick();
        return t.HeldItem(hand) == DefIndex(t, goods);
    }
    /// Grabs `goods`, carries it to a throwing spot, and lets go at `speed` m/s along `dir`
    /// (the item's own point moves at that speed over `move_s`); returns the release position.
    Vec3 Throw(int goods, Vec3 dir, float speed, int hand = 1, double move_s = 0.25) {
        if (!Grab(goods, hand)) {
            return {};
        }
        const sb::ThrowItemDef& d = t.Defs()[size_t(DefIndex(t, goods))];
        // Carry it (slowly) to the start of the throw.
        const Vec3 start{hand ? 0.2f : -0.2f, 1.3f, -0.1f};
        const Vec3 from = in.grip_position[hand];
        for (int i = 1; i <= 54; ++i) {
            in.grip_position[hand] = Lerp(from, start - d.grip.position, float(i) / 54.0f);
            Tick();
        }
        Run(0.1);
        const Vec3 u = Normalize(dir);
        const int n = int(std::lround(move_s / kDt));
        for (int i = 0; i < n; ++i) {
            in.grip_position[hand] = in.grip_position[hand] + u * (speed * float(kDt));
            Tick();
        }
        in.grip_position[hand] = in.grip_position[hand] + u * (speed * float(kDt));
        in.squeeze[hand] = 0.0f;
        Tick();
        return in.grip_position[hand] + d.grip.position;
    }
    void RunUntilDone(double limit = 8.0) {
        const double end = time + limit;
        while (time < end && std::any_of(t.Bullets().begin(), t.Bullets().end(),
                                         [](const sb::Throwables::Bullet& b) { return !b.ended; })) {
            Tick();
        }
        Run(0.05);
    }
};


// ---- The built-in table, the extracted table, the config ---------------------------------------

void TestTable(const std::filesystem::path& assets) {
    const std::vector<sb::ThrowItemDef> defs = sb::DefaultThrowItems();
    CHECK(defs.size() == 15);
    const auto find = [&](int goods) -> const sb::ThrowItemDef& {
        for (const sb::ThrowItemDef& d : defs) {
            if (d.goods == goods) {
                return d;
            }
        }
        return defs[0];
    };
    // The dump's Bullet rows (inventory throwables.json).
    const sb::ThrowItemDef& molotov = find(1200);
    CHECK(molotov.initial_velocity == 15 && molotov.gravity_in == 8 && molotov.gravity_out == 12);
    CHECK(molotov.max_velocity == 50 && molotov.life_s == 4 && molotov.range_m == 6);
    const sb::ThrowItemDef& knife = find(1240);
    CHECK(knife.initial_velocity == 20 && knife.gravity_in == 0 && knife.accel_in == 10);
    CHECK(Near(knife.gravity_out, 9.8f, 1e-4f) && knife.range_m == 8 && knife.life_s == 2);
    CHECK(find(1230).max_velocity == 20 && find(1230).range_m == 10);
    CHECK(find(1410).initial_velocity == 2 && Near(find(1410).life_s, 2.04f, 1e-4f));
    CHECK(find(2080).num_shoot == 3 && find(2080).shoot_angle_deg == -20);
    CHECK(find(2070).gravity_in == 0); // the dump's 500 is not a gravity
    CHECK(find(1201).fuse_s > 0 && find(1201).penetrate && find(1201).gravity_in == 40);
    CHECK(find(1251).fuse_s > 0 && find(1251).back_throw && find(1250).back_throw);
    CHECK(!find(1200).back_throw && find(1260).penetrate && find(1260).range_m == 0);
    CHECK(find(2130).accel_in == 10 && Near(find(2130).range_m, 7.6f, 1e-4f));
    CHECK(find(2120).mesh.empty()); // no flight mesh in the bundle: a procedural eye
    // Every row has a look.
    for (const sb::ThrowItemDef& d : defs) {
        CHECK(!d.name.empty() && !d.effect.empty() && !d.shape.empty() && d.scale > 0.0f);
    }

    // The extracted table replaces the bullet fields, and reads a gravity of 100+ as none.
    std::vector<sb::ThrowItemDef> next = defs;
    const json::Value table = json::Parse(
        "{\"items\": [{\"goods\": 2070, \"initVellocity\": 40, \"gravityInRange\": 500, "
        "\"gravityOutRange\": 61, \"accelInRange\": 0, \"maxVellocity\": 41, \"life\": 0.25, "
        "\"dist\": 15, \"numShoot\": 1, \"shootAngle\": 0, \"isPenetrate\": 1, "
        "\"sfxId_Bullet\": 202007, \"HitBulletID\": 411}, {\"goods\": 1}]}");
    CHECK(sb::ApplyExtractedTable(next, table) == 1);
    CHECK(next[11].goods == 2070 && next[11].initial_velocity == 40 && next[11].gravity_in == 0);
    CHECK(next[11].gravity_out == 61 && next[11].max_velocity == 41 && next[11].bullet_overrides);

    // The config: per-item overrides by name and id, clamps, the mode.
    sb::ThrowConfig cfg;
    cfg.Load(json::Parse("{\"mode\": \"banana\", \"smooth_ms\": 5000, \"grab_radius_m\": 9, "
                         "\"items\": {\"Pebble\": {\"scale\": 2, \"initial_velocity\": 10}, "
                         "\"1200\": {\"fuse_s\": 2, \"grip\": {\"position\": [1, 2, 3]}}}}"));
    CHECK(cfg.mode == "hand" && cfg.smooth_ms == 400.0f && cfg.grab_radius_m == 0.6f);
    sb::Throwables t;
    std::string log;
    t.Load("no-such-assets", cfg, log);
    CHECK(t.Defs()[size_t(DefIndex(t, 1230))].scale == 2.0f);
    CHECK(t.Defs()[size_t(DefIndex(t, 1230))].initial_velocity == 10.0f);
    CHECK(t.Defs()[size_t(DefIndex(t, 1200))].fuse_s == 2.0f);
    CHECK(t.Defs()[size_t(DefIndex(t, 1200))].grip.position.z == 3.0f);
    CHECK(sb::ThrowButton("dpad_down") == BBXR_BTN_DOWN && sb::ThrowButton("nope") == 0);

    // With the extracted files: the table matches the built-ins (they are the dump's values),
    // 14 items have a real mesh (the Blacksky Eye has none in the bundle).
    std::error_code ec;
    if (std::filesystem::exists(assets / "throwables" / "throwables.json", ec)) {
        sb::Throwables ex;
        std::string l;
        ex.Load(assets, sb::ThrowConfig{}, l);
        if (Verbose()) {
            std::printf("  %s\n", l.c_str());
        }
        int meshes = 0, same = 0;
        for (size_t i = 0; i < ex.Defs().size(); ++i) {
            meshes += ex.HasMesh(int(i));
            const sb::ThrowItemDef& a = ex.Defs()[i];
            const sb::ThrowItemDef& b = find(a.goods);
            same += a.bullet_overrides && Near(a.initial_velocity, b.initial_velocity, 1e-3f) &&
                    Near(a.gravity_out, b.gravity_out, 1e-3f) && Near(a.life_s, b.life_s, 1e-3f) &&
                    Near(a.range_m, b.range_m, 1e-3f) && a.num_shoot == b.num_shoot &&
                    a.penetrate == b.penetrate;
        }
        CHECK(meshes == 14);
        CHECK(same == 15);
        // The meshes load and are item-sized.
        int loaded = 0;
        for (size_t i = 0; i < ex.Defs().size(); ++i) {
            if (!ex.HasMesh(int(i))) {
                continue;
            }
            sb::Model m;
            std::string error;
            if (sb::LoadModel(ex.ModelPath(int(i)), m, error) && !m.vertices.empty() &&
                !m.meshes.empty()) {
                Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
                for (const sb::Vertex& v : m.vertices) {
                    lo = {std::min(lo.x, v.position[0]), std::min(lo.y, v.position[1]),
                          std::min(lo.z, v.position[2])};
                    hi = {std::max(hi.x, v.position[0]), std::max(hi.y, v.position[1]),
                          std::max(hi.z, v.position[2])};
                }
                const float size = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
                // Centred, 5 cm .. 0.5 m.
                if (size > 0.05f && size < 0.5f && std::fabs(lo.x + hi.x) < 0.01f &&
                    std::fabs(lo.y + hi.y) < 0.01f && std::fabs(lo.z + hi.z) < 0.01f) {
                    ++loaded;
                }
            } else {
                std::printf("  %s: %s\n", ex.Defs()[i].name.c_str(), error.c_str());
            }
        }
        CHECK(loaded == 14);
        std::printf("throwables: %d extracted meshes load, bullet table matches the built-ins\n",
                    loaded);
    } else {
        std::printf("throwables: no %s (run extract.py --throwables): mesh checks skipped\n",
                    (assets / "throwables").string().c_str());
    }

    // The procedural stand-ins are valid meshes.
    for (const char* shape : {"bottle", "urn", "vial", "knife", "disc", "stone", "sphere", "x"}) {
        const sb::Model m = sb::MakeThrowModel(shape, 1.0f);
        bool valid = !m.vertices.empty() && m.indices.size() % 3 == 0 && m.meshes.size() == 1 &&
                     m.meshes[0].index_count == m.indices.size();
        for (uint32_t i : m.indices) {
            valid = valid && i < m.vertices.size();
        }
        CHECK(valid);
    }
}

// ---- The hand velocity estimator ---------------------------------------------------------------

void TestHandVelocity() {
    {
        sb::HandVelocity h;
        const Vec3 v{3.0f, 4.0f, 0.0f};
        for (int i = 0; i < 30; ++i) {
            h.Push(double(i) * kDt, v * float(double(i) * kDt));
        }
        const Vec3 e = h.Estimate(0.08f, 0.5f);
        CHECK_NEAR(Length(e - v), 0.0f, 0.01f);
    }
    {
        // Speeds up to 10 m/s over 100 ms, then the hand stops dead (the release flick).
        sb::HandVelocity h;
        std::vector<Vec3> pos;
        for (int i = 0; i <= 13; ++i) {
            const double t = double(i) * kDt;
            const double x = t < 0.1 ? 50.0 * t * t : 0.5;
            h.Push(t, {float(x), 0.0f, 0.0f});
        }
        const float mean = h.Estimate(0.08f, 0.0f).x, mid = h.Estimate(0.08f, 0.5f).x,
                    peak = h.Estimate(0.08f, 1.0f).x;
        CHECK(mean > 3.0f && mean < 5.0f);
        CHECK(peak > 8.5f && peak < 10.5f);
        CHECK(mean < mid && mid < peak);
        CHECK_NEAR(mid, 0.5f * (mean + peak), 0.01f);
        // The window can end before the release.
        const float lagged = h.Estimate(0.08f, 0.0f, 0.04f).x;
        CHECK(lagged > mean);
    }
    {
        sb::HandVelocity h;
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> n(-0.001f, 0.001f);
        for (int i = 0; i < 30; ++i) {
            const double t = double(i) * kDt;
            h.Push(t, {6.0f * float(t) + n(rng), n(rng), n(rng)});
        }
        const Vec3 e = h.Estimate(0.08f, 0.5f);
        CHECK_NEAR(e.x, 6.0f, 0.4f);
        CHECK(std::fabs(e.y) < 0.4f && std::fabs(e.z) < 0.4f);
        sb::HandVelocity none;
        CHECK(Length(none.Estimate(0.08f, 0.5f)) == 0.0f);
    }
}

// ---- Grab ----------------------------------------------------------------------------------------

void TestGrab() {
    Sim s;
    // Nothing in reach.
    s.in.grip_position[1] = {1.5f, 1.5f, 0.5f};
    s.in.squeeze[1] = 0.9f;
    s.Tick();
    CHECK(s.t.HeldItem(1) < 0);
    // Close but not squeezing, then squeezing.
    const Vec3 home = s.ItemOf(1230).home;
    s.HandAt(1, home, 1230, 0.3f);
    s.Run(0.1);
    CHECK(s.t.HeldItem(1) < 0 && s.t.HoverItem(1) == DefIndex(s.t, 1230));
    s.in.squeeze[1] = 0.9f;
    s.Tick();
    CHECK(s.t.HeldItem(1) == DefIndex(s.t, 1230));
    CHECK(s.t.Items()[size_t(DefIndex(s.t, 1230))].state == sb::Throwables::kHeld);
    CHECK(s.last.haptic_amplitude[1] > 0.3f && s.last.haptic_seconds[1] > 0.0f); // the grab tick
    // The item rides the hand, with the grip offset in the hand's frame.
    const sb::ThrowItemDef& d = s.t.Defs()[size_t(DefIndex(s.t, 1230))];
    s.in.grip_position[1] = {0.1f, 1.4f, -0.3f};
    s.in.grip_rotation[1] = AxisAngle({0, 1, 0}, 0.5f * kPi);
    s.Tick();
    const Vec3 want = s.in.grip_position[1] + Rotate(s.in.grip_rotation[1], d.grip.position);
    CHECK_NEAR(Length(s.t.Items()[size_t(s.t.HeldItem(1))].pos - want), 0.0f, 1e-4f);
    // The other hand cannot take it; the weapon-free left hand takes another item.
    s.in.grip_position[0] = want;
    s.in.squeeze[0] = 0.9f;
    s.Tick();
    CHECK(s.t.HeldItem(0) < 0);
    s.in.squeeze[0] = 0.0f;
    // A weak squeeze (between the thresholds) keeps holding; below the release it lets go.
    s.in.squeeze[1] = 0.45f;
    s.Tick();
    CHECK(s.t.HeldItem(1) >= 0);
    s.in.squeeze[1] = 0.2f;
    s.Tick();
    CHECK(s.t.HeldItem(1) < 0);

    // The nearest of two in reach.
    Sim n;
    const Vec3 a = n.ItemOf(1200).home, b = n.ItemOf(1201).home; // 0.24 m apart
    const Vec3 between = Lerp(a, b, 0.4f);
    n.in.grip_position[1] = between - n.cfg.grab_point;
    n.in.squeeze[1] = 0.0f;
    n.Tick();
    n.in.squeeze[1] = 0.9f;
    n.Tick();
    CHECK(Length(a - between) < n.cfg.grab_radius_m && Length(b - between) < n.cfg.grab_radius_m);
    CHECK(n.t.HeldItem(1) == DefIndex(n.t, 1200));
}

// ---- Throws ----------------------------------------------------------------------------------------

/// A flat forward throw (-Z) of `goods` at `speed` m/s of hand: the record, compared with the
/// reference integration of the same Bullet rules.
struct Flight {
    Vec3 origin;
    sb::Throwables::Throw record;
    Ref ref;
    bool ok = false;
};
Flight FlatThrow(int goods, float hand_speed, Sim* use = nullptr) {
    Sim local;
    Sim& s = use ? *use : local;
    Flight f;
    const sb::ThrowItemDef d = s.t.Defs()[size_t(DefIndex(s.t, goods))];
    f.origin = s.Throw(goods, {0, 0, -1}, hand_speed);
    s.RunUntilDone();
    if (s.t.Throws().empty()) {
        return f;
    }
    f.record = s.t.Throws().front();
    const float scale = std::clamp(hand_speed / s.cfg.ref_speed_mps, s.cfg.min_scale, s.cfg.max_scale);
    float speed = d.initial_velocity * scale;
    if (d.max_velocity > 0.0f) {
        speed = std::min(speed, d.max_velocity);
    }
    f.ref = Reference(d, f.origin, {0, 0, -1}, speed);
    f.ok = true;
    return f;
}

void TestRanges() {
    // Pebble at the reference speed: the whole flight against the reference integration.
    {
        const Flight f = FlatThrow(1230, 7.0f);
        CHECK(f.ok);
        CHECK(f.record.hit == "floor");
        CHECK_NEAR(f.record.hand_speed, 7.0f, 0.1f);
        CHECK_NEAR(f.record.release_speed, 20.0f, 0.2f);
        CHECK_NEAR(f.record.range_m, f.ref.range, 0.03f * f.ref.range + 0.1f);
        CHECK_NEAR(f.record.flight_s, f.ref.time, 0.05f);
        CHECK(f.ref.range > 12.0f); // gravity 3 for the first 10 m: a long flat throw
        std::printf("  pebble: range %.2f m (reference %.2f), %.2f s\n", double(f.record.range_m),
                    double(f.ref.range), double(f.record.flight_s));
    }
    // Every flying item against its reference (flat throw at 7 m/s of hand).
    for (int goods : {1200, 1220, 1260, 1270, 1410, 1240, 1210, 2130, 1201}) {
        const Flight f = FlatThrow(goods, 7.0f);
        CHECK(f.ok);
        if (f.ok) {
            CHECK_NEAR(f.record.range_m, f.ref.range, 0.03f * f.ref.range + 0.15f);
            std::printf("  goods %d: range %.2f m (reference %.2f), hit %s\n", goods,
                        double(f.record.range_m), double(f.ref.range), f.record.hit.c_str());
        }
    }
    // The delayed Molotov drops much shorter than the Molotov (its speed cap turns it down).
    {
        const Flight m = FlatThrow(1200, 7.0f), d = FlatThrow(1201, 7.0f);
        CHECK(m.ok && d.ok);
        CHECK(d.record.range_m < 0.75f * m.record.range_m);
        CHECK(d.record.range_m > 1.0f);
    }
    // Speed scaling and the max-velocity cap.
    {
        const Flight slow = FlatThrow(1230, 3.5f);
        CHECK_NEAR(slow.record.hand_speed, 3.5f, 0.1f);
        CHECK_NEAR(slow.record.release_speed, 10.0f, 0.2f); // 20 x 3.5 / 7
        CHECK_NEAR(slow.record.range_m, slow.ref.range, 0.03f * slow.ref.range + 0.1f);
        const Flight weak = FlatThrow(1230, 1.5f);
        CHECK_NEAR(weak.record.release_speed, 6.0f, 0.2f);  // min_scale 0.3
        const Flight fast = FlatThrow(1230, 20.0f);
        CHECK_NEAR(fast.record.release_speed, 20.0f, 0.01f); // 1.5 x 20 = 30, capped at 20
        const Flight fast_knife = FlatThrow(1240, 20.0f);
        CHECK_NEAR(fast_knife.record.release_speed, 30.0f, 0.2f); // 20 x 1.5 (max 50)
    }
    // Knives fly straight through their range, then drop.
    {
        Sim s;
        const Vec3 origin = s.Throw(1240, {0, 0, -1}, 7.0f);
        float y_at_range = -1.0f;
        for (int i = 0; i < 500 && y_at_range < 0.0f; ++i) {
            s.Tick();
            for (const sb::Throwables::Bullet& b : s.t.Bullets()) {
                if (b.travelled >= 8.0f) {
                    y_at_range = b.pos.y;
                }
            }
        }
        CHECK(y_at_range > 0.0f);
        CHECK_NEAR(y_at_range, origin.y, 0.02f);
        s.RunUntilDone();
        CHECK(!s.t.Throws().empty() && s.t.Throws()[0].hit == "floor");
        CHECK(s.t.Throws()[0].range_m > 12.0f); // past its 8 m, falling at 9.8
    }
    // Tiny Tonitrus: nearly straight, ends at its 0.2 s life with a bolt.
    {
        Sim s;
        s.Throw(2070, {0, 0, -1}, 7.0f);
        s.RunUntilDone();
        CHECK(s.t.Throws().size() == 1);
        const auto& r = s.t.Throws()[0];
        CHECK(r.hit == "expired");
        CHECK_NEAR(r.flight_s, 0.2f, 0.02f);
        CHECK_NEAR(r.path_m, 33.0f * 0.2f, 0.5f);
        CHECK(std::any_of(s.t.Effects().begin(), s.t.Effects().end(),
                          [](const auto& e) { return e.kind == "lightning"; }));
    }
    // The coin toss: 2 m/s, falls in about half a second.
    {
        const Flight f = FlatThrow(1410, 7.0f);
        CHECK_NEAR(f.record.release_speed, 2.0f, 0.05f);
        CHECK_NEAR(f.record.range_m, f.ref.range, 0.15f);
        CHECK(f.record.range_m < 2.0f);
    }
}

void TestFan() {
    Sim s;
    s.Throw(2080, {0, 0, -1}, 7.0f);
    CHECK(s.t.Bullets().size() == 3);
    std::vector<float> yaw;
    for (const sb::Throwables::Bullet& b : s.t.Bullets()) {
        yaw.push_back(std::atan2(-b.vel.x, -b.vel.z) / kDeg);
        CHECK_NEAR(Length(b.vel), 4.0f, 0.01f); // initial 4 = max 4
        CHECK_NEAR(b.vel.y, 0.0f, 0.01f);
    }
    std::sort(yaw.begin(), yaw.end());
    if (yaw.size() == 3) {
        CHECK_NEAR(yaw[0], -20.0f, 0.5f);
        CHECK_NEAR(yaw[1], 0.0f, 0.5f);
        CHECK_NEAR(yaw[2], 20.0f, 0.5f);
    }
    s.RunUntilDone();
    CHECK(s.t.Throws().size() == 3);
    for (const sb::Throwables::Throw& r : s.t.Throws()) {
        CHECK(r.hit == "expired");
        CHECK_NEAR(r.path_m, 12.0f, 0.4f); // 4 m/s for 3 s
        const float a = std::atan2(-(r.end.x - r.origin.x), -(r.end.z - r.origin.z)) / kDeg;
        CHECK(std::fabs(a) < 0.5f || std::fabs(std::fabs(a) - 20.0f) < 0.5f);
    }
}

void TestDelayedAndDrops() {
    {
        // Delayed Molotov: lands, sits for its fuse, then bursts.
        Sim s;
        s.Throw(1201, {0, 0, -1}, 7.0f);
        for (int i = 0; i < 400 && !(s.t.Bullets().size() == 1 && s.t.Bullets()[0].resting); ++i) {
            s.Tick();
        }
        CHECK(s.t.Bullets().size() == 1 && s.t.Bullets()[0].resting);
        CHECK(s.t.Throws().empty() && s.t.Effects().empty());
        const double landed = s.time;
        s.Run(1.2);
        CHECK(s.t.Effects().empty() && s.t.Throws().empty());
        s.Run(0.5);
        CHECK(s.time - landed >= 1.5);
        CHECK(s.t.Effects().size() == 1 && s.t.Effects()[0].kind == "fire");
        CHECK(s.t.Throws().size() == 1 && s.t.Throws()[0].hit == "floor (fuse)");
        CHECK(s.t.Bullets().empty());
    }
    {
        // Rope Molotov: a back throw drops where the hand lets go, whatever the hand's speed.
        Sim s;
        const Vec3 at = s.Throw(1250, {0, 0, -1}, 5.0f);
        CHECK(s.t.Bullets().empty());
        s.Run(1.5);
        CHECK(s.t.Effects().size() == 1 && s.t.Effects()[0].kind == "fire");
        if (!s.t.Effects().empty()) {
            const Vec3 p = s.t.Effects()[0].pos;
            CHECK_NEAR(p.x, at.x, 0.03f);
            CHECK_NEAR(p.z, at.z, 0.03f);
            CHECK(p.y < 0.05f);
        }
        CHECK(s.ItemOf(1250).state == sb::Throwables::kSpent);
        s.Run(3.5); // ... and it is on the rack again
        CHECK(s.ItemOf(1250).state == sb::Throwables::kRack);
    }
    {
        // A weak flick just drops it (no bullet, no effect), and it can be picked up again.
        Sim s;
        s.Throw(1230, {0, 0, -1}, 0.6f);
        s.Run(1.5);
        CHECK(s.t.Bullets().empty() && s.t.Effects().empty() && s.t.Throws().empty());
        CHECK(s.ItemOf(1230).state == sb::Throwables::kRest);
        CHECK(s.log.find("dropped") != std::string::npos);
        CHECK(s.ItemOf(1230).pos.y < 1.0f);
        s.HandAt(1, s.ItemOf(1230).pos, 1230, 0.0f);
        s.Tick();
        s.in.squeeze[1] = 0.9f;
        s.Tick();
        CHECK(s.t.HeldItem(1) == DefIndex(s.t, 1230));
    }
    {
        // A dropped item left alone goes back to the rack.
        Sim s;
        s.Throw(1230, {0, 0, -1}, 0.6f);
        s.Run(s.cfg.rest_return_s + 2.0f);
        CHECK(s.ItemOf(1230).state == sb::Throwables::kRack);
    }
}

void TestEnemyAndWorld() {
    const auto enemy_sim = [](Sim& s) {
        s.phys.SetEnemyParts({{{0.0f, 0.2f, -6.0f}, {0.0f, 1.8f, -6.0f}, 0.3f}}, Mat4{});
        s.Physics();
    };
    {
        Sim s;
        enemy_sim(s);
        CHECK(s.phys.Colliders().size() >= 2);
        s.Throw(1200, {0, 0, -1}, 7.0f);
        s.RunUntilDone();
        CHECK(s.t.Throws().size() == 1);
        CHECK(s.t.Throws()[0].hit.rfind("enemy", 0) == 0);
        CHECK(s.t.Throws()[0].end.z < -5.4f && s.t.Throws()[0].end.z > -6.4f);
        CHECK(!s.t.Effects().empty() && s.t.Effects()[0].kind == "fire");
    }
    {
        // Pungent Blood Cocktail penetrates: it passes through the enemy and ends on the floor.
        Sim s;
        enemy_sim(s);
        s.Throw(1260, {0, 0, -1}, 7.0f);
        s.RunUntilDone();
        CHECK(s.t.Throws().size() == 1);
        CHECK(s.t.Throws()[0].hit == "floor");
        CHECK(s.t.Throws()[0].range_m > 6.0f);
        CHECK(s.log.find("passes through enemy") != std::string::npos);
    }
    {
        // The training props: the post and the wall stop bullets.
        Sim s(true);
        s.phys.SetEnemyParts({{{50, 0, 50}, {50, 1, 50}, 0.1f}}, Mat4{});
        s.Physics();
        // The wall's face is at x = 1.75 (right of where you stood), 2.6 m long around z = -0.7.
        s.Throw(1230, {1, 0, 0.3f}, 7.0f);
        s.RunUntilDone();
        CHECK(!s.t.Throws().empty() && s.t.Throws()[0].hit == "wall");
        CHECK(!s.t.Effects().empty() && s.t.Effects()[0].kind == "dust");
    }
    {
        // Thrown onto the table the item lands on it.
        Sim s;
        s.Throw(1230, {0, 0, -1}, 0.5f);
        CHECK(s.ItemOf(1230).state == sb::Throwables::kFlying);
        s.Run(1.0);
        const auto& it = s.ItemOf(1230);
        CHECK(it.state == sb::Throwables::kRest);
        CHECK(std::fabs(it.pos.y - (s.t.Table().center.y + s.t.Table().half.y)) < 0.06f);
    }
}

void TestGameMode() {
    {
        Sim s(false, "game");
        CHECK(s.t.GameMode() == false); // seeded on the first Step
        CHECK(s.Grab(1230));
        CHECK(s.t.GameMode());
        // The hand stands still: only the trigger throws, along the view, at the Bullet's speed.
        s.in.trigger[1] = 0.9f;
        s.Tick();
        CHECK(s.t.HeldItem(1) < 0 && s.t.Bullets().size() == 1);
        const auto& b = s.t.Bullets()[0];
        CHECK_NEAR(Length(b.vel), 20.0f, 0.01f);
        CHECK_NEAR(b.vel.y / Length(b.vel), std::sin(5.0f * kDeg), 0.002f);
        CHECK_NEAR(b.vel.x, 0.0f, 0.01f);
        s.RunUntilDone();
        CHECK(!s.t.Throws().empty() && s.t.Throws()[0].mode == "game");
        const sb::ThrowItemDef d = s.t.Defs()[size_t(DefIndex(s.t, 1230))];
        const Ref r = Reference(d, s.t.Throws()[0].origin, Normalize(Vec3{0, std::sin(5.0f * kDeg), -std::cos(5.0f * kDeg)}), 20.0f);
        CHECK_NEAR(s.t.Throws()[0].range_m, r.range, 0.03f * r.range + 0.15f);
    }
    {
        // The head turns: the throw follows the view; a fast hand does not matter.
        Sim s(false, "game");
        s.in.head_rotation = AxisAngle({0, 1, 0}, 0.5f * kPi); // looking along -X
        CHECK(s.Grab(1240));
        for (int i = 0; i < 8; ++i) {
            s.in.grip_position[1].z += 0.1f; // a fast hand, the wrong way
            s.Tick();
        }
        s.in.trigger[1] = 0.9f;
        s.Tick();
        CHECK(s.t.Bullets().size() == 1);
        if (!s.t.Bullets().empty()) {
            const auto& b = s.t.Bullets()[0];
            CHECK_NEAR(Length(b.vel), 20.0f, 0.01f); // the knife's initial velocity, unscaled
            CHECK(b.vel.x < -19.0f && std::fabs(b.vel.z) < 0.5f);
        }
    }
    {
        // Letting go of the grip without the throw button drops it.
        Sim s(false, "game");
        CHECK(s.Grab(1200));
        s.in.squeeze[1] = 0.0f;
        s.Tick();
        CHECK(s.t.Bullets().empty() && s.ItemOf(1200).state == sb::Throwables::kFlying);
        CHECK(s.log.find("released without the throw button") != std::string::npos);
    }
}

void TestButtons() {
    Sim s;
    s.Throw(1230, {0, 0, -1}, 7.0f);
    s.Run(0.2);
    CHECK(!s.t.Bullets().empty() && s.ItemOf(1230).state == sb::Throwables::kSpent);
    // dpad_up toggles the mode; dpad_down puts everything back.
    s.in.buttons = BBXR_BTN_UP;
    s.Tick();
    CHECK(s.t.GameMode());
    s.Tick();
    CHECK(s.t.GameMode()); // held: one toggle per press
    s.in.buttons = 0;
    s.Tick();
    s.in.buttons = BBXR_BTN_UP;
    s.Tick();
    CHECK(!s.t.GameMode());
    s.in.buttons = BBXR_BTN_DOWN;
    s.Tick();
    CHECK(s.t.Bullets().empty() && s.t.Effects().empty());
    for (const sb::Throwables::Item& it : s.t.Items()) {
        CHECK(it.state == sb::Throwables::kRack && Length(it.pos - it.home) < 1e-5f);
    }
    // Spent items come back by themselves.
    s.in.buttons = 0;
    s.Throw(1240, {0, 0, -1}, 7.0f);
    s.RunUntilDone();
    s.Run(s.cfg.respawn_s + 3.5f);
    CHECK(s.ItemOf(1240).state == sb::Throwables::kRack);
}

void TestRack() {
    Sim s;
    // The rack: 15 distinct places on one table, in reach, above the floor.
    std::vector<Vec3> homes;
    for (const sb::Throwables::Item& it : s.t.Items()) {
        homes.push_back(it.home);
        CHECK(it.home.y > 0.8f && it.home.y < 1.1f);
        CHECK(Length(Vec3{it.home.x, 0.0f, it.home.z}) < 1.3f);
        CHECK(it.home.z < -0.2f); // ahead of the player
    }
    for (size_t i = 0; i < homes.size(); ++i) {
        for (size_t j = i + 1; j < homes.size(); ++j) {
            CHECK(Length(homes[i] - homes[j]) > 0.2f);
        }
    }
    // Placed where the head looked: turning 90 degrees moves the rack with it.
    sb::Throwables t2;
    std::string l;
    t2.Load("no-such-assets", s.cfg, l);
    t2.Place(kHead, AxisAngle({0, 1, 0}, 0.5f * kPi), 0.0f, s.cfg);
    CHECK(t2.Items()[0].home.x < -0.2f); // looking along -X now
}

void TestLog() {
    const char* path = "throw_test_log.jsonl";
    std::filesystem::remove(path);
    {
        Sim s;
        s.cfg.log = true;
        s.cfg.log_file = path;
        s.Throw(1220, {0, 0, -1}, 7.0f);
        s.RunUntilDone();
        CHECK(s.log.find("Oil Urn") != std::string::npos);
    }
    std::ifstream f(path);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"event\": \"throw\"") != std::string::npos);
    CHECK(text.find("\"event\": \"end\"") != std::string::npos);
    CHECK(text.find("\"item\": \"Oil Urn\"") != std::string::npos);
    CHECK(text.find("\"release_speed\"") != std::string::npos && text.find("\"range_m\"") != std::string::npos);
    f.close();
    std::filesystem::remove(path);
}

void TestEffects() {
    // Every effect kind draws lines and fades out.
    for (const sb::ThrowItemDef& d : sb::DefaultThrowItems()) {
        Sim s;
        s.Throw(d.goods, {0, 0, -1}, 7.0f);
        s.RunUntilDone();
        size_t lines = 0;
        s.t.EffectLines([&](Vec3, Vec3, const uint8_t*) { ++lines; }, s.cfg);
        // Items that vanish before landing (none here) or whose effect has not spawned yet
        // (delayed ones are done by RunUntilDone) all have a marker now.
        CHECK(lines > 0 || s.t.Effects().empty());
        CHECK(!s.t.Effects().empty() || d.back_throw);
        s.Run(4.0);
        CHECK(s.t.Effects().empty());
    }
    Sim s;
    size_t table_lines = 0;
    s.t.TableLines([&](Vec3, Vec3, const uint8_t*) { ++table_lines; });
    CHECK(table_lines >= 12);
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path assets = argc > 1 ? argv[1] : "out/vr_assets";
    TestTable(assets);
    TestHandVelocity();
    TestRack();
    TestGrab();
    TestRanges();
    TestFan();
    TestDelayedAndDrops();
    TestEnemyAndWorld();
    TestGameMode();
    TestButtons();
    TestLog();
    TestEffects();
    std::printf("throwables tests: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
