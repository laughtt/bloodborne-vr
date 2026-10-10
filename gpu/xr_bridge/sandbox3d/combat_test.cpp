// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_sandbox3d_combat_test.exe [out/vr_assets]: offline tests of the sandbox's swing and
// stamina integration (combat.h, docs/vr/findings/20 S1-S2), no headset. Scripted swings
// (minimum-jerk arcs about the shoulder at 90 Hz, 1 mm tracking noise) go through
//   - Combat::Update with contacts placed at chosen moments (the rules: light/heavy/charged, hit
//     and miss, contacts before the decision, the world, the gate at 0 and the floor, dashes,
//     regen, menus, wind-ups, the logs, the config section);
//   - the sandbox's own path: the solid weapon (WeaponPhysics) held by the scripted hand against
//     an enemy torso or a wall, its contacts (DetectSwingContacts) and Combat::Frame.
// With out/vr_assets it also checks the real Saw Cleaver's costs (doc 20: 20 / 35 / 50).
// Exit code 0 when every check passes.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "bbport_xr_bridge.h"
#include "combat.h"
#include "locomotion.h"
#include "model.h"
#include "physics_weapon.h"
#include "scene.h"

using namespace m3;

namespace {

int failures = 0, checks = 0;

void Check(bool ok, const char* what, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL combat_test.cpp line %d: %s\n", line, what);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

constexpr double kDt = 1.0 / 90.0;
const Vec3 kHead{0.0f, 1.65f, 0.0f};
const Vec3 kShoulder{0.20f, 1.45f, 0.0f};

bool Verbose() {
    return std::getenv("BB_COMBAT_TEST_VERBOSE") != nullptr;
}

float MinJerk(double u) {
    const double x = std::clamp(u, 0.0, 1.0);
    return float(x * x * x * (10.0 - 15.0 * x + 6.0 * x * x));
}

struct Pose {
    Vec3 p;
    Quat q;
};

/// A level slash about the shoulder: angle 0 = ahead (-Z), positive = to the left; the
/// controller's -Z (the blade) points away from the shoulder.
Pose SlashPose(double t, double t0, double duration, float from_deg, float to_deg,
               float radius = 0.45f) {
    const float a = (from_deg + (to_deg - from_deg) * MinJerk((t - t0) / duration)) * kDeg;
    const Vec3 dir{-std::sin(a), 0.0f, -std::cos(a)};
    return {kShoulder + dir * radius, AxisAngle({0, 1, 0}, a)};
}

/// A vertical chop about the shoulder: angle 0 = ahead, 90 = straight up.
Pose ChopPose(double t, double t0, double duration, float from_deg, float to_deg,
              float radius = 0.5f) {
    const float a = (from_deg + (to_deg - from_deg) * MinJerk((t - t0) / duration)) * kDeg;
    const Vec3 dir{0.0f, std::sin(a), -std::cos(a)};
    return {kShoulder + dir * radius, AxisAngle({1, 0, 0}, a)};
}

std::mt19937 rng(11);

Vec3 Noise(float sigma) {
    std::normal_distribution<float> n(0.0f, sigma);
    return {n(rng), n(rng), n(rng)};
}

/// The defaults with the logs kept out of the working directory.
sb::CombatConfig Quiet() {
    sb::CombatConfig c;
    c.log_file = "";
    return c;
}

/// Combat::Update driven by a scripted hand; contacts where the caller says.
struct Driver {
    explicit Driver(const sb::CombatConfig& config) : combat(config, "") {}

    enum Touch { kNone, kEnemy, kWall, kFloor };

    void Frame(const Pose& hand, Touch touch = kNone, bool blocked = false) {
        swing::HandSample h;
        h.t = t;
        h.valid = true;
        const Vec3 p = hand.p + Noise(0.001f);
        h.position = {p.x, p.y, p.z};
        h.orientation = {hand.q.x, hand.q.y, hand.q.z, hand.q.w};
        h.head_valid = true;
        h.head_position = {kHead.x, kHead.y, kHead.z};
        sb::SwingContact c;
        if (touch == kEnemy) {
            c.enemy = true;
            c.part = 2;
            c.target = "dummy";
            c.part_name = "Spine1";
            c.enemy_point = hand.p;
        } else if (touch == kWall || touch == kFloor) {
            const int e = touch == kWall ? sb::kWall : sb::kFloor;
            c.world[e] = true;
            c.world_point[e] = hand.p;
        }
        sb::CombatEvents ev;
        combat.Update(h, c, weapon, form, blocked, ev);
        log += ev.log;
        if (!ev.set_form.empty()) {
            form = ev.set_form; // as main.cpp sets the scene's form
            ++form_changes;
        }
        for (const sb::CombatHaptic& k : ev.haptics) {
            haptics.push_back({t, k});
        }
        lowest = std::min(lowest, combat.Stamina().Current());
        if (Verbose() && !ev.log.empty()) {
            std::printf("  %7.3f %s", t, ev.log.c_str());
        }
        t += kDt;
    }
    /// Frames until t1: the hand from `make`, a contact while `touch` says so.
    void Run(double t1, const std::function<Pose(double)>& make,
             const std::function<Touch(double)>& touch = {}, bool blocked = false) {
        while (t < t1 - 1e-9) {
            Frame(make(t), touch ? touch(t) : kNone, blocked);
        }
    }
    float StrongestSince(double from) const {
        float a = 0.0f;
        for (const auto& [at, h] : haptics) {
            if (at >= from - 1e-9) {
                a = std::max(a, h.amplitude);
            }
        }
        return a;
    }

    sb::Combat combat;
    std::string weapon = "Saw Cleaver";
    std::string form = "A";
    int form_changes = 0;
    double t = 0.0;
    std::string log;
    std::vector<std::pair<double, sb::CombatHaptic>> haptics;
    float lowest = 1e9f;
};

// ---- the rules (Combat::Update) -----------------------------------------------------------------

/// A slower swing is LIGHT (R1, the Saw Cleaver's 20); it pays when it connects, once, however
/// long it stays in contact; a tick when it registers and a hit pulse when it connects.
void TestLightHit() {
    Driver d(Quiet());
    double decided = -1.0, hit_t = -1.0;
    float after_hit = 0.0f;
    d.Run(2.0, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); },
          [&](double s) {
              if (decided < 0.0 && d.combat.Count().light == 1) {
                  decided = s;
              }
              if (hit_t < 0.0 && d.combat.Count().hits == 1) {
                  hit_t = s;
                  after_hit = d.combat.Stamina().Current();
              }
              return decided >= 0.0 && s > decided + 0.1 && s < decided + 0.3 ? Driver::kEnemy
                                                                               : Driver::kNone;
          });
    const sb::Combat::Counts& n = d.combat.Count();
    CHECK(n.light == 1 && n.heavy == 0);
    CHECK(n.hits == 1 && n.misses == 0);
    CHECK(std::fabs(after_hit - 71.0f) < 1e-3f); // 91 - 20
    CHECK(d.combat.Readout().label == sb::SwingReadout::Label::Light);
    CHECK(d.combat.Readout().result == sb::SwingReadout::Result::Hit);
    CHECK(d.combat.Readout().label_paid == 20.0f);
    CHECK(d.StrongestSince(decided) >= d.combat.Config().hit_light.amplitude - 1e-4f);
    CHECK(d.combat.Weapon().weapon_class == "quick");
    CHECK(d.combat.Classifier().Tuning().light_speed == 2.0f); // the quick class preset
    CHECK(d.log.find("HIT dummy Spine1") != std::string::npos);
}

/// A swing that touches nothing is free: its window ends (MISS) and the bar is where it was (no
/// regen during the swing either: the attack's own pause).
void TestMissIsFree() {
    Driver d(Quiet());
    CHECK(d.combat.TryDash(0, 0.0)); // 91 -> 71, to see that nothing else changes it
    d.Run(1.35, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); });
    CHECK(d.combat.Count().light == 1);
    CHECK(d.combat.Count().misses == 1 && d.combat.Count().hits == 0);
    CHECK(std::fabs(d.combat.Stamina().Current() - 71.0f) < 1e-3f);
    CHECK(d.combat.Readout().result == sb::SwingReadout::Result::Miss);
    CHECK(d.log.find("MISS") != std::string::npos);
}

/// A fast swing is HEAVY (R2, 35) and pulses harder than a light one when it connects.
void TestHeavyHit() {
    Driver d(Quiet());
    double decided = -1.0;
    d.Run(1.5, [](double s) { return SlashPose(s, 0.5, 0.30, -60.0f, 50.0f); },
          [&](double s) {
              if (decided < 0.0 && d.combat.Count().heavy == 1) {
                  decided = s;
              }
              return decided >= 0.0 && s > decided + 0.05 && s < decided + 0.15 ? Driver::kEnemy
                                                                                : Driver::kNone;
          });
    CHECK(d.combat.Count().heavy == 1 && d.combat.Count().light == 0);
    CHECK(d.combat.Count().hits == 1);
    CHECK(d.combat.Readout().label_paid == 35.0f);
    const sb::CombatConfig& c = d.combat.Config();
    CHECK(c.hit_heavy.amplitude > c.hit_light.amplitude || c.hit_heavy.seconds > c.hit_light.seconds);
    CHECK(std::fabs(d.StrongestSince(decided + 0.04) - c.hit_heavy.amplitude) < 1e-4f);
}

/// The blade can reach the target before the swing is decided: that contact belongs to the
/// stroke and pays when it becomes an attack.
void TestContactBeforeDecision() {
    Driver d(Quiet());
    d.Run(2.0, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); },
          [&](double) {
              return d.combat.Classifier().State().stroke && d.combat.Count().light == 0
                         ? Driver::kEnemy
                         : Driver::kNone;
          });
    CHECK(d.combat.Count().light == 1 && d.combat.Count().hits == 1);
    CHECK(d.combat.Readout().label_paid == 20.0f);
    CHECK(d.log.find("before the swing was decided") != std::string::npos);
}

/// A stroke that is not an attack touching the enemy costs nothing (a push is not a swing).
void TestWeakStrokeContactIsFree() {
    Driver d(Quiet());
    d.Run(2.0, [](double s) { return SlashPose(s, 0.5, 0.60, -20.0f, 20.0f); },
          [](double s) { return s > 0.7 && s < 0.9 ? Driver::kEnemy : Driver::kNone; });
    CHECK(d.combat.Count().light == 0 && d.combat.Count().heavy == 0);
    CHECK(d.combat.Count().too_slow == 1);
    CHECK(d.combat.Count().hits == 0);
    CHECK(d.combat.Stamina().Current() == 91.0f);
}

/// The world: with charge_world the wall pays (CLANG), without it the swing is a miss; the floor
/// only with charge_floor.
void TestWorldContact() {
    for (const int mode : {0, 1, 2}) {
        sb::CombatConfig c = Quiet();
        c.charge_world = mode != 1;
        Driver d(c);
        double decided = -1.0;
        d.Run(1.5, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); },
              [&](double s) {
                  if (decided < 0.0 && d.combat.Count().light == 1) {
                      decided = s;
                  }
                  const bool on = decided >= 0.0 && s > decided + 0.1 && s < decided + 0.2;
                  return on ? (mode == 2 ? Driver::kFloor : Driver::kWall) : Driver::kNone;
              });
        if (mode == 0) {
            CHECK(d.combat.Count().clangs == 1 && d.combat.Count().misses == 0);
            CHECK(d.combat.Readout().result == sb::SwingReadout::Result::Clang);
            CHECK(d.combat.Readout().charged_from == 91.0f && d.combat.Readout().charged_to == 71.0f);
        } else {
            CHECK(d.combat.Count().clangs == 0 && d.combat.Count().misses == 1);
            CHECK(d.combat.Stamina().Current() == 91.0f);
        }
    }
}

/// The game's rule at 0: five slashes 0.9 s apart, 30 each, from 45 (doc 20's end-to-end case):
/// hit (15), miss (free), hit (-15 + a little regen), hit (still above 0 before it), then the fifth
/// does not register; a dash is refused too; never below the floor; regen brings it back.
void TestExhaustion() {
    sb::CombatConfig c = Quiet();
    c.stamina.max = 45.0f;
    c.light_cost = c.heavy_cost = 30.0f;
    Driver d(c);
    const double starts[] = {0.5, 1.4, 2.3, 3.2, 4.1};
    const bool connects[] = {true, false, true, true, true};
    int last = -1;
    double decided = -1.0;
    bool dash_refused = false;
    d.Run(5.2,
          [&](double s) {
              int k = 0;
              while (k < 4 && s >= starts[k + 1]) {
                  ++k;
              }
              return SlashPose(s, starts[k], 0.45, (k % 2) ? 40.0f : -50.0f,
                               (k % 2) ? -50.0f : 40.0f);
          },
          [&](double s) {
              const int fired = d.combat.Count().light + d.combat.Count().heavy;
              if (fired - 1 != last) {
                  last = fired - 1;
                  decided = s;
              }
              if (!dash_refused && d.combat.Count().tired == 1) {
                  dash_refused = !d.combat.TryDash(0, s);
              }
              return last >= 0 && connects[last] && s > decided + 0.15 && s < decided + 0.17
                         ? Driver::kEnemy
                         : Driver::kNone;
          });
    const sb::Combat::Counts& n = d.combat.Count();
    CHECK(n.light + n.heavy == 4);
    CHECK(n.tired == 1);
    CHECK(n.hits == 3 && n.misses == 1);
    CHECK(dash_refused && n.dashes_refused == 1);
    CHECK(d.combat.Readout().label == sb::SwingReadout::Label::DashTired ||
          d.combat.Readout().label == sb::SwingReadout::Label::Tired);
    CHECK(d.lowest >= c.stamina.floor);
    CHECK(d.lowest < 0.0f);
    // Two buzzes for each refusal (the swing and the dash).
    int tired = 0;
    for (const auto& [at, h] : d.haptics) {
        tired += std::fabs(h.amplitude - c.tired.amplitude) < 1e-4f &&
                 std::fabs(h.seconds - c.tired.seconds) < 1e-4f;
    }
    CHECK(tired == 4);
    // Regen: 45/s after the 0.75 s pause; back above 0, a dash is paid (and may go negative).
    double back = -1.0;
    d.Run(8.0, [](double s) { return SlashPose(s, 4.1, 0.45, -50.0f, 40.0f); },
          [&](double s) {
              if (back < 0.0 && d.combat.Stamina().CanAct()) {
                  back = s;
              }
              return Driver::kNone;
          });
    CHECK(back > 4.0 && back < 5.6);
    CHECK(d.combat.Stamina().Current() == 45.0f);
    CHECK(d.combat.TryDash(1, d.t));
    CHECK(std::fabs(d.combat.Stamina().Current() - 25.0f) < 1e-3f);
    CHECK(d.combat.Count().dashes == 1);
    CHECK(d.combat.TryDash(2, d.t) && d.combat.TryDash(0, d.t)); // 25 -> 5 (> 0) -> -15
    CHECK(std::fabs(d.combat.Stamina().Current() + 15.0f) < 1e-3f);
    CHECK(!d.combat.TryDash(0, d.t)); // -15: refused
}

/// A menu has the controls: the swing is dropped, nothing is held.
void TestBlocked() {
    Driver d(Quiet());
    d.Run(1.2, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); }, {}, true);
    CHECK(d.combat.Count().blocked == 1 && d.combat.Count().light == 0);
    CHECK(d.combat.Stamina().OpenSwings() == 0);
    CHECK(d.combat.Readout().label == sb::SwingReadout::Label::Blocked);
}

/// A rising stroke is a WINDUP (no attack); held still overhead it charges (CHARGING), and the
/// chop out of it is the CHARGED attack (50) after a full charge, a HEAVY (35) before one.
void TestWindupAndCharge() {
    for (const double hold : {1.0, 0.2}) {
        Driver d(Quiet());
        d.Run(0.98, [](double s) { return ChopPose(s, 0.5, 0.4, -30.0f, 100.0f); });
        CHECK(d.combat.Count().windups == 1);
        CHECK(d.combat.Readout().label == sb::SwingReadout::Label::Windup);
        CHECK(d.combat.Stamina().OpenSwings() == 0);
        double charge_t = -1.0;
        d.Run(3.0, [](double s) { return ChopPose(s, 0.5, 0.4, -30.0f, 100.0f); },
              [&](double s) {
                  if (charge_t < 0.0 && d.combat.Count().charge_starts == 1) {
                      charge_t = s;
                  }
                  return Driver::kNone;
              });
        CHECK(charge_t > 0.0);
        if (charge_t < 0.0) {
            return;
        }
        // Hold, then chop down through the target.
        Driver e(Quiet());
        double started = -1.0, released = -1.0;
        const double chop = charge_t + hold;
        e.Run(chop + 1.2,
              [&](double s) {
                  return s < chop ? ChopPose(s, 0.5, 0.4, -30.0f, 100.0f)
                                  : ChopPose(s, chop, 0.35, 100.0f, 0.0f);
              },
              [&](double s) {
                  if (started < 0.0 && e.combat.Readout().charging) {
                      started = s;
                  }
                  const int done = e.combat.Count().charged + e.combat.Count().heavy;
                  if (released < 0.0 && done == 1) {
                      released = s;
                  }
                  return released >= 0.0 && s > released + 0.03 && s < released + 0.1
                             ? Driver::kEnemy
                             : Driver::kNone;
              });
        CHECK(started > 0.0 && released > started);
        CHECK(e.combat.Count().hits == 1);
        if (hold >= e.combat.Config().charge_full_s) {
            CHECK(e.combat.Count().charged == 1 && e.combat.Count().heavy == 0);
            CHECK(e.combat.Readout().label_paid == 50.0f);
            CHECK(std::fabs(e.StrongestSince(released + 0.02) -
                            e.combat.Config().hit_charged.amplitude) < 1e-4f);
        } else {
            CHECK(e.combat.Count().heavy == 1 && e.combat.Count().charged == 0);
            CHECK(e.combat.Readout().label_paid == 35.0f);
        }
    }
    // Charging at 0: refused (the R2 hold never starts), and its release is nothing.
    sb::CombatConfig c = Quiet();
    c.stamina.max = 10.0f;
    c.stamina.regen_per_s = 0.0f; // stays below 0
    Driver d(c);
    CHECK(d.combat.TryDash(0, 0.0)); // 10 -> -10
    d.Run(3.4, [](double s) {
        return s < 2.5 ? ChopPose(s, 0.5, 0.4, -30.0f, 100.0f) : ChopPose(s, 2.5, 0.35, 100.0f, 0.0f);
    });
    CHECK(d.combat.Count().charge_starts == 0 && d.combat.Count().tired == 1);
    CHECK(d.combat.Count().charged == 0 && d.combat.Count().heavy == 0);
}

/// A charge released without a swing (held until the game's auto-release) has no stroke: a
/// contact during the hold does not pay for it; only one after the release would.
void TestChargeTimeoutIgnoresHoldContacts() {
    Driver d(Quiet());
    double charge_t = -1.0;
    d.Run(5.0, [](double s) { return ChopPose(s, 0.5, 0.4, -30.0f, 100.0f); },
          [&](double s) {
              if (charge_t < 0.0 && d.combat.Count().charge_starts == 1) {
                  charge_t = s;
              }
              const bool hold_contact = charge_t > 0.0 && s > charge_t + 2.0 && s < charge_t + 2.4;
              return hold_contact ? Driver::kEnemy : Driver::kNone;
          });
    CHECK(charge_t > 0.0);
    CHECK(d.combat.Count().charged == 1); // released by the 2.5 s limit: a full charge
    CHECK(d.combat.Count().hits == 0 && d.combat.Count().misses == 1);
    CHECK(d.combat.Stamina().Current() == 91.0f);
}

/// infinite: nothing is paid and nothing is refused.
void TestInfinite() {
    sb::CombatConfig c = Quiet();
    c.infinite = true;
    Driver d(c);
    for (int k = 0; k < 6; ++k) {
        CHECK(d.combat.TryDash(0, 0.01 * k));
    }
    d.Run(1.5, [](double s) { return SlashPose(s, 0.5, 0.30, -60.0f, 50.0f); },
          [](double s) { return s > 0.7 && s < 0.8 ? Driver::kEnemy : Driver::kNone; });
    CHECK(d.combat.Count().heavy == 1 && d.combat.Count().hits == 1);
    CHECK(d.combat.Stamina().Current() == 91.0f);
}

/// The JSON-lines log: a session line, then one valid JSON object per event, the strokes with
/// their hand samples.
void TestLog() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bbport_combat_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "swing.jsonl";
    std::error_code ec;
    std::filesystem::remove(file, ec);
    {
        sb::CombatConfig c = Quiet();
        c.log_file = file.string();
        Driver d(c);
        d.Run(2.0, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); },
              [&](double s) {
                  return d.combat.Count().light == 1 && s < 0.95 ? Driver::kEnemy : Driver::kNone;
              });
        d.Run(4.0, [](double s) { return SlashPose(s, 2.0, 0.60, 30.0f, -50.0f); });
        CHECK(d.combat.TryDash(1, d.t));
        d.Run(4.1, [](double s) { return SlashPose(s, 2.0, 0.60, 30.0f, -50.0f); });
    }
    std::ifstream in(file);
    std::string line;
    int lines = 0, bad = 0, swings = 0, hits = 0, misses = 0, dashes = 0, sessions = 0, samples = 0;
    while (std::getline(in, line)) {
        ++lines;
        std::string error;
        const json::Value v = json::Parse(line, &error);
        if (!error.empty()) {
            ++bad;
            continue;
        }
        const std::string event = v["event"].Str();
        sessions += event == "session";
        swings += event == "swing";
        hits += event == "hit";
        misses += event == "miss";
        dashes += event == "dash";
        if (event == "swing" && v["samples"].Size() >= 10 && v["samples"][0].Size() == 15) {
            ++samples;
        }
    }
    CHECK(lines > 0 && bad == 0);
    CHECK(sessions == 1);
    CHECK(swings == 2 && hits == 1 && misses == 1 && dashes == 1);
    CHECK(samples == 2);
}

/// The "swing" section: added to a config file that has none (the rest kept), reloaded when
/// saved; the class presets and overrides; the summary reads back.
void TestConfig() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bbport_combat_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "sandbox3d.json";
    {
        std::ofstream out(file, std::ios::binary);
        out << "{\n  \"render_scale\": 0.5\n}\n";
    }
    sb::CombatConfigFile poll(file);
    sb::CombatConfig c;
    std::string log;
    CHECK(poll.Poll(0.0, c, log));
    std::string error;
    const json::Value doc = json::Parse(sb::ReadText(file), &error);
    CHECK(error.empty());
    CHECK(doc["render_scale"].Float() == 0.5f);
    CHECK(doc["swing"]["stamina"]["max"].Float() == 91.0f);
    CHECK(doc["swing"]["stamina"]["regen_per_s"].Float() == 45.0f);
    CHECK(doc["swing"]["class"].Str() == "auto");
    CHECK(doc["swing"]["haptics"]["hit_heavy"].Size() == 2);
    CHECK(doc["swing"]["transform"]["min_angle_deg"].Float() == 90.0f);
    CHECK(doc["swing"]["transform"]["window_s"].Float() == 0.15f);
    CHECK(doc["swing"]["transform"]["haptic"].Size() == 2);
    CHECK(doc["swing"]["transform"]["gesture"].Bool(false) &&
          doc["swing"]["transform"]["button"].Bool(false));
    log.clear();
    CHECK(!poll.Poll(0.2, c, log)); // polled at most twice a second, and unchanged
    {
        std::ofstream out(file, std::ios::binary);
        out << "{\"render_scale\": 0.5, \"swing\": {\"class\": \"heavy\", \"light_speed\": 3, "
               "\"stamina\": {\"max\": 120, \"light_cost\": 22}}}\n";
    }
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) +
                                               std::chrono::seconds(2));
    CHECK(poll.Poll(1.0, c, log));
    CHECK(c.weapon_class == "heavy" && c.stamina.max == 120.0f && c.light_cost == 22.0f);
    const swing::SwingTuning t = sb::SwingTuningFor(c, c.weapon_class);
    CHECK(t.light_speed == 3.0f && t.heavy_speed == 4.2f && std::fabs(t.min_arc - 0.28f) < 1e-6f);
    // Summary() is the section's JSON and reads back the same.
    sb::CombatConfig back;
    const json::Value summary = json::Parse(c.Summary(), &error);
    CHECK(error.empty());
    back.Load(summary);
    CHECK(back.weapon_class == "heavy" && back.stamina.max == 120.0f && back.light_cost == 22.0f &&
          back.light_speed == 3.0f && back.hit_charged.seconds == c.hit_charged.seconds);
    // The classes (doc 20 4.3).
    CHECK(sb::WeaponClassOf("Saw Cleaver", "A") == "quick");
    CHECK(sb::WeaponClassOf("Saw Cleaver", "B") == "medium");
    CHECK(sb::WeaponClassOf("Hunter Axe", "A") == "medium");
    CHECK(sb::WeaponClassOf("Hunter Axe", "B") == "heavy");
    CHECK(sb::WeaponClassOf("Beast Cutter", "A") == "heavy");
    CHECK(sb::WeaponClassOf("Threaded Cane", "A") == "quick");
    CHECK(sb::WeaponClassOf("Kirkhammer", "A") == "medium");
    const swing::SwingTuning quick = sb::SwingTuningFor(sb::CombatConfig{}, "quick");
    CHECK(quick.light_speed == 2.0f && quick.heavy_speed == 4.3f);
}

/// The costs come from the game's BehaviorParam_PC rows (judge 0 / 150 / 170, form B 400 / 550 /
/// 570) through the hitbox extractor's wp_<id>.json.
void TestWeaponCosts(const std::filesystem::path& real_assets) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "bbport_combat_test" / "assets";
    std::filesystem::create_directories(dir / "hitbox");
    {
        std::ofstream a(dir / "assets.json", std::ios::binary);
        a << "{\"body\": \"hunter.json\", \"weapons\": [{\"name\": \"Saw Cleaver\", \"id\": "
             "7000000, \"model\": \"wp_a_0800\", \"file\": \"wp_a_0800.json\"}]}\n";
        std::ofstream h(dir / "hitbox" / "wp_7000000.json", std::ios::binary);
        h << "{\"attacks\": [{\"judge\": 0, \"stamina\": 21}, {\"judge\": 10, \"stamina\": 99}, "
             "{\"judge\": 150, \"stamina\": 36}, {\"judge\": 170, \"stamina\": 51}, {\"judge\": "
             "400, \"stamina\": 24}, {\"judge\": 550, \"stamina\": 40}, {\"judge\": 570, "
             "\"stamina\": 55}]}\n";
    }
    const sb::SwingWeapon a = sb::LookUpSwingWeapon(dir, "Saw Cleaver", "A");
    CHECK(a.id == 7000000 && a.cost[0] == 21.0f && a.cost[1] == 36.0f && a.cost[2] == 51.0f);
    CHECK(a.source == "hitbox/wp_7000000.json" && a.weapon_class == "quick");
    const sb::SwingWeapon b = sb::LookUpSwingWeapon(dir, "saw cleaver", "B");
    CHECK(b.cost[0] == 24.0f && b.cost[1] == 40.0f && b.cost[2] == 55.0f && b.weapon_class == "medium");
    const sb::SwingWeapon none = sb::LookUpSwingWeapon(dir, "Torch", "A");
    CHECK(none.id == 0 && none.cost[0] == 20.0f && none.source == "defaults");
    // The weapon's costs in the swings; a config cost overrides.
    sb::CombatConfig c = Quiet();
    sb::Combat combat(c, dir);
    swing::HandSample h;
    h.valid = true;
    sb::CombatEvents ev;
    combat.Update(h, {}, "Saw Cleaver", "A", false, ev);
    CHECK(combat.Weapon().cost[0] == 21.0f);
    // The user's own dump, when extracted: the Saw Cleaver's real numbers (doc 20 1.4).
    if (std::filesystem::exists(real_assets / "hitbox" / "wp_7000000.json")) {
        const sb::SwingWeapon real = sb::LookUpSwingWeapon(real_assets, "Saw Cleaver", "A");
        CHECK(real.cost[0] == 20.0f && real.cost[1] == 35.0f && real.cost[2] == 50.0f);
        const sb::SwingWeapon real_b = sb::LookUpSwingWeapon(real_assets, "Saw Cleaver", "B");
        CHECK(real_b.cost[0] == 24.0f);
        std::printf("combat tests: the dump's Saw Cleaver costs R1 %g, R2 %g, charged %g, form B R1 "
                    "%g (%s)\n",
                    real.cost[0], real.cost[1], real.cost[2], real_b.cost[0], real.source.c_str());
    }
}

/// The comfort dash's moves (locomotion.h) through its move gate, as main.cpp wires it: the
/// backstep, the dodge and the jump each pay 20; at 0 or below the move is refused and nothing
/// moves.
void TestLocomotionGate() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bbport_combat_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path config = dir / "loco.json";
    {
        std::ofstream out(config, std::ios::binary);
        out << "{}\n";
    }
    sb::CombatConfig c = Quiet();
    c.stamina.regen_per_s = 0.0f; // only the moves change it
    sb::Combat combat(c, "");
    sb::Locomotion loco(config);
    loco.move_gate = [&](int kind, double now) { return combat.TryDash(kind, now); };
    double t = 0.0;
    uint32_t buttons = 0;
    float ly = 0.0f;
    std::string log;
    const auto axis = [](float v) {
        return uint8_t(std::lround(128.0f + 127.0f * std::clamp(v, -1.0f, 1.0f)));
    };
    const auto step = [&]() {
        sb::SceneInput in;
        in.time = t;
        in.stage = true;
        in.head = {true, kHead, Quat{}};
        in.grip[1] = {true, {0.2f, 1.2f, -0.3f}, Quat{}};
        in.buttons = buttons;
        BbXrInput pad{};
        pad.active = 1;
        pad.buttons = buttons;
        pad.lx = 128;
        pad.ly = axis(-ly);
        pad.rx = pad.ry = 128;
        log += loco.Update(in, pad);
        swing::HandSample h;
        h.t = t;
        h.valid = true;
        h.position = {0.2f, 1.2f, -0.3f};
        h.head_valid = true;
        h.head_position = {kHead.x, kHead.y, kHead.z};
        sb::CombatEvents ev;
        combat.Update(h, {}, "Saw Cleaver", "A", false, ev);
        log += ev.log;
        t += 1.0 / 72.0;
    };
    const auto run = [&](double seconds) {
        for (const double end = t + seconds; t < end - 1e-9;) {
            step();
        }
    };
    const auto tap = [&](uint32_t button) {
        buttons |= button;
        step();
        step();
        buttons &= ~button;
        step();
    };
    run(0.5);
    Vec3 before = Translation(loco.WorldFromTracking());
    tap(BBXR_BTN_CIRCLE); // standing: the backstep
    run(1.6);
    CHECK(combat.Count().dashes == 1);
    CHECK(std::fabs(combat.Stamina().Current() - 71.0f) < 1e-3f);
    CHECK(Length(Translation(loco.WorldFromTracking()) - before) > 1.0f);
    ly = 1.0f; // walking: the dodge
    tap(BBXR_BTN_CIRCLE);
    ly = 0.0f;
    run(1.6);
    CHECK(combat.Count().dashes == 2 && std::fabs(combat.Stamina().Current() - 51.0f) < 1e-3f);
    ly = 1.0f; // the jump: L3 while walking
    run(0.2);
    tap(BBXR_BTN_L3);
    ly = 0.0f;
    run(2.4);
    CHECK(combat.Count().dashes == 3 && std::fabs(combat.Stamina().Current() - 31.0f) < 1e-3f);
    tap(BBXR_BTN_CIRCLE);
    run(1.6);
    tap(BBXR_BTN_CIRCLE);
    run(1.6);
    CHECK(combat.Count().dashes == 5 && std::fabs(combat.Stamina().Current() + 9.0f) < 1e-3f);
    // -9: refused, nothing moves.
    before = Translation(loco.WorldFromTracking());
    tap(BBXR_BTN_CIRCLE);
    run(1.6);
    CHECK(combat.Count().dashes == 5 && combat.Count().dashes_refused == 1);
    CHECK(Length(Translation(loco.WorldFromTracking()) - before) < 1e-3f);
    CHECK(log.find("move refused: backstep") != std::string::npos);
    CHECK(combat.Readout().label == sb::SwingReadout::Label::DashTired);
}

// ---- the sandbox path: WeaponPhysics -> DetectSwingContacts -> Combat::Frame ----------------------

/// A flat bar weapon: the grip at the origin, `length` along -Y.
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

struct World {
    explicit World(const sb::CombatConfig& config, bool enemy = true) : combat(config, "") {
        cfg.enemy = "";
        cfg.contact_log = "";
        cfg.log_contacts = false;
        cfg.post[0] = cfg.post[1] = 50.0f;
        cfg.wall[0] = cfg.wall[1] = 60.0f;
        bar = BarModel(0.8f, 0.03f, 0.02f);
        attack.resize(1);
        attack[0].p[0].pos = {0, -0.3f, 0};
        attack[0].p[1].pos = {0, -0.8f, 0};
        attack[0].radius = 0.1f;
        attack[0].capsule = true;
        if (enemy) {
            // A torso straight ahead, 1 m from the shoulder: the blade's middle sweeps through it.
            physics.SetEnemyParts({{{0.2f, 1.0f, -1.0f}, {0.2f, 1.7f, -1.0f}, 0.15f}}, Mat4{});
        }
    }
    void Frame(const Pose& grip) {
        sb::WeaponPhysics::StepInput in;
        in.time = t;
        in.head_valid = true;
        in.head_position = kHead;
        in.floor_y = 0.0f;
        in.held[side].model = &bar;
        in.held[side].name = "Saw Cleaver";
        in.held[side].attack = attack;
        in.held[side].attack_name = "test";
        // The bar's -Y along the controller's -Z (where the classifier's probe is).
        in.held[side].target =
            Translate(grip.p) * FromQuat(grip.q * FromTo({0, -1, 0}, {0, 0, -1}));
        sb::PhysicsEvents pe;
        physics.Step(in, cfg, pe);
        sb::SceneInput si;
        si.time = t;
        si.head = {true, kHead, Quat{}};
        si.grip[side] = {true, grip.p + Noise(0.001f), grip.q};
        sb::CombatEvents ce;
        combat.Frame(si, physics, "A", false, ce);
        log += ce.log;
        if (Verbose() && !ce.log.empty()) {
            std::printf("  %7.3f %s", t, ce.log.c_str());
        }
        t += kDt;
    }
    void Run(double t1, const std::function<Pose(double)>& make) {
        while (t < t1 - 1e-9) {
            Frame(make(t));
        }
    }

    sb::PhysicsConfig cfg;
    sb::WeaponPhysics physics;
    sb::Model bar;
    std::vector<sb::HitShape> attack;
    sb::Combat combat;
    int side = 1; // the hand that holds the weapon
    double t = 0.0;
    std::string log;
};

/// Slow and fast slashes through the torso: LIGHT pays 20, HEAVY 35, once each (the physics' own
/// HIT and the frame's overlap are the same contact); a slash beside it is free.
void TestPhysicsPath() {
    {
        World w(Quiet());
        w.Run(2.0, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); });
        CHECK(w.combat.Count().light == 1 && w.combat.Count().hits == 1);
        CHECK(w.combat.Count().misses == 0);
        CHECK(w.log.find("HIT") != std::string::npos);
        CHECK(w.combat.Readout().label_paid == 20.0f);
    }
    {
        World w(Quiet());
        w.Run(1.6, [](double s) { return SlashPose(s, 0.5, 0.30, -60.0f, 50.0f); });
        CHECK(w.combat.Count().heavy == 1 && w.combat.Count().hits == 1);
        CHECK(w.physics.HitCount() >= 1); // the physics saw it as well (red flash, marker)
        CHECK(w.combat.Readout().label_paid == 35.0f);
        CHECK(w.combat.Readout().charged_from == 91.0f && w.combat.Readout().charged_to == 56.0f);
    }
    {
        World w(Quiet());
        w.Run(2.0, [](double s) { return SlashPose(s, 0.5, 0.45, 25.0f, 110.0f); });
        CHECK(w.combat.Count().light + w.combat.Count().heavy == 1);
        CHECK(w.combat.Count().hits == 0 && w.combat.Count().misses == 1);
        CHECK(w.combat.Stamina().Current() == 91.0f);
    }
}

/// A slash into a wall ahead: the solid blade stops at it and the swing pays (charge_world).
void TestPhysicsWall() {
    World w(Quiet(), false);
    w.cfg.wall[0] = 0.2f;
    w.cfg.wall[1] = -1.0f;
    w.cfg.wall_yaw_deg = 90.0f; // its face toward the user
    w.Run(2.0, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); });
    CHECK(w.combat.Count().light == 1);
    CHECK(w.combat.Count().clangs == 1 && w.combat.Count().hits == 0);
    CHECK(w.log.find("CLANG wall") != std::string::npos);
    CHECK(w.combat.Readout().charged_from == 91.0f && w.combat.Readout().charged_to == 71.0f);
}

/// Through the physics: swings into the torso until stamina is gone; the next one does not
/// register; nothing goes below the floor.
void TestPhysicsExhaustion() {
    sb::CombatConfig c = Quiet();
    c.stamina.max = 45.0f;
    c.light_cost = c.heavy_cost = 30.0f;
    World w(c);
    const double starts[] = {0.5, 1.4, 2.3};
    float lowest = 1e9f;
    w.Run(3.4, [&](double s) {
        lowest = std::min(lowest, w.combat.Stamina().Current());
        int k = 0;
        while (k < 2 && s >= starts[k + 1]) {
            ++k;
        }
        return SlashPose(s, starts[k], 0.45, (k % 2) ? 40.0f : -50.0f, (k % 2) ? -50.0f : 40.0f);
    });
    CHECK(w.combat.Count().hits == 2);
    CHECK(w.combat.Count().tired == 1);
    CHECK(lowest < 0.0f && lowest >= c.stamina.floor);
}

/// The weapon in the left hand: the swing is read from the left grip and the left weapon's hit.
void TestLeftHand() {
    World w(Quiet());
    w.side = 0;
    w.Run(2.0, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); });
    CHECK(w.combat.Count().light == 1 && w.combat.Count().hits == 1);
    CHECK(w.combat.Readout().label_paid == 20.0f);
    CHECK(w.physics.GetHand(0).active && !w.physics.GetHand(1).active);
}

/// A weapon resting in the target pays nothing: no contact edge during a swing, no physics HIT.
/// Leaving the target and coming back with a swing does.
void TestRestingWeaponPaysNothing() {
    World w(Quiet());
    // The blade lies across the torso (straight ahead) from the first frame, then swings away.
    w.Run(2.2, [](double s) { return SlashPose(s, 1.0, 0.60, 0.0f, 80.0f); });
    CHECK(w.combat.Count().light + w.combat.Count().heavy == 1);
    CHECK(w.combat.Count().hits == 0 && w.combat.Count().misses == 1);
    CHECK(w.physics.HitCount() == 0);
    CHECK(w.combat.Stamina().Current() == 91.0f);
    // The same swing from beside the torso into it pays.
    World v(Quiet());
    v.Run(2.0, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); });
    CHECK(v.combat.Count().hits == 1 && v.physics.HitCount() == 1);
}

/// A new weapon starts with no swing: the open ticket, the charge and the classifier are gone.
void TestWeaponChangeResets() {
    {
        Driver d(Quiet());
        d.Run(0.98, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); });
        CHECK(d.combat.Count().light == 1 && d.combat.Stamina().OpenSwings() == 1);
        d.weapon = "Threaded Cane";
        d.Frame(SlashPose(d.t, 0.5, 0.60, -50.0f, 30.0f));
        CHECK(d.combat.Stamina().OpenSwings() == 0);
        d.Frame(SlashPose(d.t, 0.5, 0.60, -50.0f, 30.0f), Driver::kEnemy);
        CHECK(d.combat.Count().hits == 0);
        CHECK(d.combat.Stamina().Current() == 91.0f); // the old swing never charges
        d.Run(2.0, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); });
        CHECK(d.combat.Count().misses == 0); // and does not expire as a miss either
    }
    {
        Driver d(Quiet());
        d.Run(3.0, [](double s) { return ChopPose(s, 0.5, 0.4, -30.0f, 100.0f); },
              [&](double) { return Driver::kNone; });
        CHECK(d.combat.Count().charge_starts == 1 && d.combat.Readout().charging);
        d.weapon = "Hunter Axe";
        d.Frame(ChopPose(d.t, 0.5, 0.4, -30.0f, 100.0f));
        CHECK(!d.combat.Readout().charging);
    }
}


// ---- the trick-weapon transform (docs/vr/findings/33) -------------------------------------------

/// A wrist snap in place: the grip turns `deg` about `axis` (world) from t0 over `duration`.
Pose SnapPose(double t, double t0, double duration, float deg, Vec3 axis, float travel = 0.0f) {
    const float u = MinJerk((t - t0) / duration);
    return {Vec3{0.25f, 1.20f, -0.30f - travel * u}, AxisAngle(axis, deg * u * kDeg)};
}

/// Settles, snaps (sideways, which sweeps the probe like a swing would), runs on.
void TestTransformSnap() {
    const Vec3 up{0, 1, 0}, roll{0, 0, 1};
    for (const Vec3 axis : {up, roll}) {
        Driver d(Quiet());
        d.Run(2.5, [&](double s) { return SnapPose(s, 0.5, 0.12, 100.0f, axis); });
        CHECK(d.form == "B" && d.form_changes == 1);
        CHECK(d.combat.Count().transforms == 1);
        // Never an attack, never paid, nothing open.
        CHECK(d.combat.Count().light + d.combat.Count().heavy + d.combat.Count().charge_starts == 0);
        CHECK(d.combat.Stamina().Current() == 91.0f);
        CHECK(d.combat.Readout().form == "B");
        CHECK(d.combat.Weapon().form == "B" && d.combat.Weapon().weapon_class == "medium");
        // Two clicks on the weapon hand: the snap's and the lock's, kept across the form change.
        int clicks = 0;
        for (const auto& [at, h] : d.haptics) {
            clicks += h.hand == 1 && h.amplitude >= 0.5f && at > 0.5 && at < 1.2;
        }
        CHECK(clicks == 2);
    }
    {
        // The same sideways snap with the gesture off is what the veto prevents: an attack.
        sb::CombatConfig c = Quiet();
        c.transform_gesture = false;
        Driver d(c);
        d.Run(2.5, [&](double s) { return SnapPose(s, 0.5, 0.12, 100.0f, up); });
        CHECK(d.form == "A" && d.combat.Count().transforms == 0);
        CHECK(d.combat.Count().light + d.combat.Count().heavy >= 1);
    }
    {
        // Back: a second snap after the cooldown, in the other direction, returns to A.
        Driver d(Quiet());
        d.Run(1.8, [&](double s) { return SnapPose(s, 0.5, 0.12, 100.0f, roll); });
        CHECK(d.form == "B");
        d.Run(2.4, [&](double) { return SnapPose(1.0, 0.5, 0.12, 100.0f, roll); });
        d.Run(4.0, [&](double s) { return SnapPose(s, 2.5, 0.12, -100.0f, roll); });
        CHECK(d.form == "A" && d.combat.Count().transforms == 2);
    }
}

/// Slashes, even fast ones, and a slow turn of the wrist leave the form alone and stay attacks.
void TestTransformIgnoresSwings() {
    {
        Driver d(Quiet());
        d.Run(1.6, [](double s) { return SlashPose(s, 0.5, 0.25, -60.0f, 50.0f); });
        CHECK(d.form == "A" && d.combat.Count().transforms == 0);
        CHECK(d.combat.Count().heavy == 1);
    }
    {
        Driver d(Quiet());
        d.Run(1.6, [](double s) { return SlashPose(s, 0.5, 0.55, -40.0f, 40.0f); });
        CHECK(d.form == "A" && d.combat.Count().light == 1);
    }
    {
        Driver d(Quiet());
        d.Run(2.0, [&](double s) { return SnapPose(s, 0.5, 0.5, 100.0f, Vec3{0, 0, 1}); });
        CHECK(d.form == "A" && d.combat.Count().transforms == 0);
    }
    {
        // The hand travels while it turns: a swing, not a snap.
        Driver d(Quiet());
        d.Run(2.0, [&](double s) { return SnapPose(s, 0.5, 0.12, 100.0f, Vec3{0, 0, 1}, 0.4f); });
        CHECK(d.form == "A" && d.combat.Count().transforms == 0);
    }
}

/// The L1 button (Combat::Frame sets it from the left grip) does the same; a menu, a weapon with
/// one form and the disabled config do not.
void TestTransformButton() {
    const auto still = [](double) { return SnapPose(0.0, 0.5, 0.12, 0.0f, Vec3{0, 0, 1}); };
    {
        Driver d(Quiet());
        d.Run(0.5, still);
        d.combat.PressTransform();
        d.Run(0.7, still);
        CHECK(d.form == "B" && d.combat.Count().transforms_button == 1);
        d.combat.PressTransform();
        d.Run(0.9, still);
        CHECK(d.form == "A"); // the button has no cooldown of its own
    }
    {
        Driver d(Quiet());
        d.weapon = "Hunter Pistol";
        d.Run(0.5, still);
        d.combat.PressTransform();
        d.Run(0.7, still);
        CHECK(d.form == "A" && d.combat.Count().transforms_none == 1);
        CHECK(d.StrongestSince(0.4) > 0.0f && d.StrongestSince(0.4) < 0.5f); // only a soft tick
    }
    {
        Driver d(Quiet());
        d.Run(0.5, still);
        d.combat.PressTransform();
        d.Run(0.7, still, {}, true); // blocked by a menu
        CHECK(d.form == "A" && d.combat.Count().transforms == 0);
        d.Run(2.5, [&](double s) { return SnapPose(s, 1.0, 0.12, 100.0f, Vec3{0, 0, 1}); }, {},
              true);
        CHECK(d.form == "A"); // ... and so is a snap
    }
    {
        sb::CombatConfig c = Quiet();
        c.transform_button = false;
        Driver d(c);
        d.Run(0.5, still);
        d.combat.PressTransform();
        d.Run(0.7, still);
        CHECK(d.form == "A");
    }
}

/// An open swing ends with the form it was made in.
void TestTransformDropsOpenSwing() {
    Driver d(Quiet());
    d.Run(0.98, [](double s) { return SlashPose(s, 0.5, 0.60, -50.0f, 30.0f); });
    CHECK(d.combat.Count().light == 1 && d.combat.Stamina().OpenSwings() == 1);
    d.combat.PressTransform();
    d.Frame(SlashPose(d.t, 0.5, 0.60, -50.0f, 30.0f));
    d.Frame(SlashPose(d.t, 0.5, 0.60, -50.0f, 30.0f), Driver::kEnemy);
    CHECK(d.form == "B" && d.combat.Stamina().OpenSwings() == 0);
    CHECK(d.combat.Stamina().Current() == 91.0f);
}

/// With the dump the Saw Cleaver has a form B attack set; the Hunter Pistol has no other form.
void TestTransformForms(const std::filesystem::path& real_assets) {
    CHECK(sb::KnownTrickWeapon("Saw Cleaver") && sb::KnownTrickWeapon("Hunter Axe"));
    CHECK(sb::KnownTrickWeapon("Threaded Cane") && !sb::KnownTrickWeapon("Hunter Pistol"));
    if (!std::filesystem::exists(real_assets / "assets.json")) {
        std::printf("combat tests: no %s/assets.json, the transform's form data not checked\n",
                    real_assets.string().c_str());
        return;
    }
    CHECK(sb::LookUpSwingWeapon(real_assets, "Saw Cleaver", "A").two_forms);
    CHECK(!sb::LookUpSwingWeapon(real_assets, "Hunter Pistol", "A").two_forms);
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path assets = argc > 1 ? argv[1] : "out/vr_assets";
    TestLightHit();
    TestMissIsFree();
    TestHeavyHit();
    TestContactBeforeDecision();
    TestWeakStrokeContactIsFree();
    TestWorldContact();
    TestExhaustion();
    TestBlocked();
    TestWindupAndCharge();
    TestChargeTimeoutIgnoresHoldContacts();
    TestInfinite();
    TestLog();
    TestConfig();
    TestWeaponCosts(assets);
    TestLocomotionGate();
    TestPhysicsPath();
    TestPhysicsWall();
    TestPhysicsExhaustion();
    TestLeftHand();
    TestRestingWeaponPaysNothing();
    TestWeaponChangeResets();
    TestTransformSnap();
    TestTransformIgnoresSwings();
    TestTransformButton();
    TestTransformDropsOpenSwing();
    TestTransformForms(assets);
    std::printf("combat tests: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
