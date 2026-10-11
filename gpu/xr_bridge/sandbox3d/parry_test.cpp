// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_sandbox3d_parry_test.exe: the parry trainer (parry.h) with scripted hands, no headset
// and no GPU: the parry window timing (parry / early / late / idle / miss), the visceral, the thrust
// gate's integration (fire, speed, alignment, debounce rejections and their log lines), the aim
// assist and the config. Exit code 0 when every check passes.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "json.h"
#include "math3d.h"
#include "parry.h"

using namespace m3;
using namespace sb;

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

bool Has(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

constexpr double kDt = 1.0 / 90.0;

/// A scripted player: head at 1.65 m looking -Z, the left hand 0.3 m ahead, aiming -Z (yawed by
/// aim_yaw_deg), driven frame by frame into the trainer (the dummy is its own stand-in 2.2 m ahead).
struct Sim {
    ParryTrainer trainer;
    ParryConfig config;
    ParryInput in;
    ParryEvents total;
    double time = 1.0;
    float aim_yaw_deg = 0.0f;
    Vec3 hand{0.0f, 1.3f, -0.3f};
    float trigger = 0.0f;
    Vec3 rhand{0.0f, 1.3f, -0.3f}; // the right hand, aiming -Z
    std::string log;
    float haptic_left = 0.0f;

    Sim() {
        config = ParryConfig{};
        config.shoot_speed = 1.5f;
        config.shoot_align = 0.8f;
        config.shoot_rearm = 0.5f;
        config.shoot_debounce_ms = 250.0f;
        config.assist_strength = 0.8f;
        config.sound = false;
        in.head_valid = true;
        in.head_position = {0.0f, 1.65f, 0.0f};
        in.hand_valid = true;
        in.hand_free = true;
        in.rhand_valid = true;
    }
    void Tick() {
        time += kDt;
        in.time = time;
        in.hand_position = hand;
        in.aim_position = hand;
        in.aim_rotation = AxisAngle({0, 1, 0}, aim_yaw_deg * kDeg);
        in.trigger = trigger;
        in.rhand_position = rhand;
        in.rhand_rotation = Quat{0, 0, 0, 1};
        ParryEvents e;
        trainer.Step(in, config, e);
        log += e.log;
        haptic_left = std::max(haptic_left, e.haptic_amplitude[0]);
        if (!e.toast_title.empty()) {
            total.toast_title = e.toast_title;
            total.toast_body = e.toast_body;
        }
    }
    void Run(double seconds) {
        for (double end = time + seconds; time < end;) {
            Tick();
        }
    }
    /// Runs until the dummy enters `phase` (false if it takes more than 20 s).
    bool Until(ParryTrainer::Phase phase) {
        for (int i = 0; i < 90 * 20; ++i) {
            Tick();
            if (trainer.GetPhase() == phase) {
                return true;
            }
        }
        return false;
    }
    /// Pulls the trigger for one frame (trigger_fire).
    void Pull() {
        trigger = 1.0f;
        Tick();
        trigger = 0.0f;
    }
    /// Fires so the bullet lands about `at` seconds after the window opens (negative: before).
    /// The flight over the ~2 m to the dummy is about 33 ms at 60 m/s.
    void FireForHitAt(double at) {
        const double fire_time = trainer.WindowStart() + at - 0.034;
        while (time + kDt < fire_time) {
            Tick();
        }
        Pull();
    }
    /// A jab along the barrel: velocity vmax * sin(pi t / duration) over `duration`, then still.
    void Jab(float vmax, double duration, Vec3 axis = {0, 0, -1}, bool right = false) {
        Vec3& hand = right ? rhand : this->hand;
        const Vec3 start = hand;
        double travelled = 0.0;
        for (double t = 0.0; t < duration; t += kDt) {
            travelled += double(vmax) * std::sin(kPi * t / duration) * kDt;
            hand = start + axis * float(travelled);
            Tick();
        }
    }
};

void TestWindowTiming() {
    {
        Sim s;
        s.config.trigger_fire = true;
        CHECK(s.Until(ParryTrainer::kWindUp));
        CHECK(std::fabs(s.trainer.WindowStart() - (s.trainer.PhaseStart() + s.config.windup_s)) < 1e-6);
        CHECK(std::fabs((s.trainer.WindowEnd() - s.trainer.WindowStart()) - s.config.window_s) < 1e-6);
        s.FireForHitAt(0.05);
        s.Run(0.2);
        CHECK(s.trainer.Score().parries == 1 && s.trainer.Score().early == 0 && s.trainer.Score().late == 0);
        CHECK(s.trainer.GetPhase() == ParryTrainer::kStagger);
        CHECK(Has(s.log, "PARRY!"));
        CHECK(s.total.toast_title == "PARRY!");
        CHECK(s.haptic_left >= 1.0f); // the big pulse
        // The stagger lasts stagger_s, then the loop goes on.
        CHECK(std::fabs((s.trainer.PhaseEnd() - s.trainer.PhaseStart()) - s.config.stagger_s) < 1e-6);
        s.Run(s.config.stagger_s + 0.1);
        CHECK(s.trainer.GetPhase() != ParryTrainer::kStagger);
        CHECK(Has(s.log, "recovers"));
        CHECK(s.trainer.Score().visceral == 0);
    }
    { // too early: 300 ms before the window
        Sim s;
        s.config.trigger_fire = true;
        CHECK(s.Until(ParryTrainer::kWindUp));
        s.FireForHitAt(-0.30);
        s.Run(0.1);
        CHECK(s.trainer.Score().early == 1 && s.trainer.Score().parries == 0);
        CHECK(Has(s.log, "TOO EARLY by"));
        const size_t at = s.log.find("TOO EARLY by ");
        const double ms = std::atof(s.log.c_str() + at + 13);
        CHECK(ms > 250.0 && ms < 350.0); // within a frame or two of the 300 ms
        CHECK(s.total.toast_title == "TOO EARLY");
    }
    { // too late: 100 ms after the window closed
        Sim s;
        s.config.trigger_fire = true;
        CHECK(s.Until(ParryTrainer::kWindUp));
        s.FireForHitAt(s.config.window_s + 0.10);
        s.Run(0.1);
        CHECK(s.trainer.Score().late == 1 && s.trainer.Score().parries == 0);
        CHECK(Has(s.log, "TOO LATE by"));
        const size_t at = s.log.find("TOO LATE by ");
        const double ms = std::atof(s.log.c_str() + at + 12);
        CHECK(ms > 70.0 && ms < 130.0);
    }
    { // the window length is tunable: a 40 ms window rejects what 150 ms takes
        Sim s;
        s.config.trigger_fire = true;
        s.config.window_s = 0.04f;
        CHECK(s.Until(ParryTrainer::kWindUp));
        s.FireForHitAt(0.10);
        s.Run(0.1);
        CHECK(s.trainer.Score().late == 1 && s.trainer.Score().parries == 0);
    }
    { // a hit while idle is not graded
        Sim s;
        s.config.trigger_fire = true;
        s.config.attack_loop = false;
        s.Run(0.5);
        s.Pull();
        s.Run(0.3);
        CHECK(s.trainer.Score().idle_hits == 1);
        CHECK(s.trainer.Score().parries + s.trainer.Score().early + s.trainer.Score().late == 0);
    }
    { // a bullet that hits nothing is a miss
        Sim s;
        s.config.trigger_fire = true;
        s.config.assist = false;
        s.config.attack_loop = false;
        s.aim_yaw_deg = 40.0f;
        s.Run(0.3);
        s.Pull();
        s.Run(1.2);
        CHECK(s.trainer.Score().misses == 1 && s.trainer.Score().shots == 1);
        CHECK(Has(s.log, "MISS"));
    }
}

void TestVisceral() {
    Sim s;
    s.config.trigger_fire = true;
    CHECK(s.Until(ParryTrainer::kWindUp));
    // A melee hit before any parry is not a visceral.
    s.in.melee_hit_time = s.time;
    s.Tick();
    CHECK(s.trainer.Score().visceral == 0);
    CHECK(s.Until(ParryTrainer::kIdle));
    CHECK(s.Until(ParryTrainer::kWindUp));
    s.FireForHitAt(0.05);
    s.Run(0.3);
    CHECK(s.trainer.GetPhase() == ParryTrainer::kStagger);
    s.in.melee_hit_time = s.time; // the R1 swing lands during the stagger
    s.Tick();
    CHECK(s.trainer.Score().visceral == 1);
    CHECK(s.trainer.GetPhase() == ParryTrainer::kDown);
    CHECK(Has(s.log, "VISCERAL!") && s.total.toast_title == "VISCERAL!");
}

/// A sim whose dummy is staggered (a parry), the right hand 1.0 m in front of the chest's plane.
void Stagger(Sim& s, ParryConfig::VisceralMode mode) {
    s.config.trigger_fire = true;
    s.config.visceral_mode = mode;
    s.config.visceral_reach_m = 1.5f;
    s.config.stagger_s = 4.0f;
    CHECK(s.Until(ParryTrainer::kWindUp));
    s.FireForHitAt(0.05);
    s.Run(0.3);
    CHECK(s.trainer.GetPhase() == ParryTrainer::kStagger);
}

void TestVisceralThrust() {
    { // a thrust at the staggered dummy within reach is a visceral
        Sim s;
        s.rhand = {0.0f, 1.3f, -1.0f};
        Stagger(s, ParryConfig::kVisceralThrust);
        s.Run(0.2);
        s.Jab(3.0f, 0.25, {0, 0, -1}, true);
        s.Run(0.1);
        CHECK(s.trainer.Score().visceral == 1);
        CHECK(s.trainer.GetPhase() == ParryTrainer::kDown);
        CHECK(Has(s.log, "VISCERAL! (thrust)") && s.total.toast_title == "VISCERAL!");
    }
    { // out of reach: rejected with the reason, logged once
        Sim s;
        s.rhand = {0.0f, 1.3f, -0.3f};
        Stagger(s, ParryConfig::kVisceralThrust);
        s.config.visceral_reach_m = 0.5f;
        s.Run(0.2);
        s.Jab(3.0f, 0.25, {0, 0, -1}, true);
        s.Run(0.3);
        CHECK(s.trainer.Score().visceral == 0 && s.trainer.GetPhase() == ParryTrainer::kStagger);
        CHECK(Has(s.log, "visceral thrust rejected: reach"));
    }
    { // too slow: rejected for speed
        Sim s;
        s.rhand = {0.0f, 1.3f, -1.0f};
        Stagger(s, ParryConfig::kVisceralThrust);
        s.Run(0.2);
        s.Jab(1.2f, 0.3, {0, 0, -1}, true);
        s.Run(0.3);
        CHECK(s.trainer.Score().visceral == 0);
        CHECK(Has(s.log, "visceral thrust rejected: speed"));
    }
    { // a thrust away from the chest (aim straight, stroke sideways): rejected for alignment
        Sim s;
        s.rhand = {0.0f, 1.3f, -1.0f};
        Stagger(s, ParryConfig::kVisceralThrust);
        s.Run(0.2);
        s.Jab(3.0f, 0.25, {-0.7071f, 0, 0.7071f}, true);
        s.Run(0.3);
        CHECK(s.trainer.Score().visceral == 0);
    }
    { // outside the stagger a thrust does nothing, and logs nothing
        Sim s;
        s.config.visceral_reach_m = 1.5f;
        s.config.attack_loop = false;
        s.rhand = {0.0f, 1.3f, -1.0f};
        s.Run(0.5);
        s.Jab(3.0f, 0.25, {0, 0, -1}, true);
        s.Run(0.3);
        CHECK(s.trainer.Score().visceral == 0 && s.trainer.GetPhase() == ParryTrainer::kIdle);
        CHECK(!Has(s.log, "visceral thrust") && !Has(s.log, "VISCERAL"));
    }
    { // swing mode ignores thrusts, but the melee hit still finishes it
        Sim s;
        s.rhand = {0.0f, 1.3f, -1.0f};
        Stagger(s, ParryConfig::kVisceralSwing);
        s.Run(0.2);
        s.Jab(3.0f, 0.25, {0, 0, -1}, true);
        s.Run(0.2);
        CHECK(s.trainer.Score().visceral == 0 && s.trainer.GetPhase() == ParryTrainer::kStagger);
        CHECK(!Has(s.log, "visceral thrust"));
        s.in.melee_hit_time = s.time;
        s.Tick();
        CHECK(s.trainer.Score().visceral == 1 && Has(s.log, "VISCERAL! (swing)"));
    }
    { // thrust mode: the melee hit still works too
        Sim s;
        Stagger(s, ParryConfig::kVisceralThrust);
        s.in.melee_hit_time = s.time;
        s.Tick();
        CHECK(s.trainer.Score().visceral == 1 && Has(s.log, "VISCERAL! (swing)"));
    }
    { // auto: fires by itself once the hand is within reach, not before
        Sim s;
        Stagger(s, ParryConfig::kVisceralAuto);
        s.rhand = {0.0f, 1.3f, -0.3f};
        s.Run(0.3);
        CHECK(s.trainer.Score().visceral == 0);
        s.rhand = {0.0f, 1.3f, -1.4f};
        s.Tick();
        CHECK(s.trainer.Score().visceral == 1 && Has(s.log, "VISCERAL! (auto)"));
    }
    { // auto: the head within reach does it as well
        Sim s;
        Stagger(s, ParryConfig::kVisceralAuto);
        s.Run(0.2);
        CHECK(s.trainer.Score().visceral == 0);
        s.in.head_position.z = -1.2f;
        s.rhand = {0.0f, 1.3f, -0.3f};
        s.Tick();
        CHECK(s.trainer.Score().visceral == 1 && Has(s.log, "(head"));
    }
    { // the tunables are read from the config
        ParryConfig c;
        c.Load(json::Parse(R"({"visceral": {"mode": "auto", "speed_mps": 2.2, "align": 0.5,
            "reach_m": 0.9, "debounce_ms": 150}})"));
        CHECK(c.visceral_mode == ParryConfig::kVisceralAuto && c.visceral_speed == 2.2f &&
              c.visceral_align == 0.5f && c.visceral_reach_m == 0.9f && c.visceral_debounce_ms == 150.0f);
        ParryConfig d;
        d.Load(json::Parse(c.Summary("")));
        CHECK(d.visceral_mode == ParryConfig::kVisceralAuto && d.visceral_reach_m == 0.9f);
        CHECK(ParryConfig{}.visceral_mode == ParryConfig::kVisceralThrust);
    }
}

void TestShootGate() {
    { // a fast jab along the barrel fires once
        Sim s;
        s.config.attack_loop = false;
        s.Run(0.3);
        s.Jab(3.0f, 0.25);
        s.Run(0.3);
        CHECK(s.trainer.Score().shots == 1);
        CHECK(Has(s.log, "SHOT #1 (thrust)"));
        CHECK(s.haptic_left >= 0.8f);
        CHECK(!s.trainer.Bullets().empty());
    }
    { // too slow: rejected for speed, logged once with the numbers
        Sim s;
        s.config.attack_loop = false;
        s.Run(0.3);
        s.Jab(1.0f, 0.3);
        s.Run(0.5);
        CHECK(s.trainer.Score().shots == 0);
        CHECK(Has(s.log, "jab rejected: speed"));
    }
    { // a sideways swing: rejected for alignment
        Sim s;
        s.config.attack_loop = false;
        s.Run(0.3);
        s.Jab(3.0f, 0.25, {0.7071f, 0, -0.7071f}); // 45 degrees off the barrel
        s.Run(0.5);
        CHECK(s.trainer.Score().shots == 0);
        CHECK(Has(s.log, "alignment"));
    }
    { // a second jab inside the debounce is rejected, one after it fires
        Sim s;
        s.config.attack_loop = false;
        s.config.shoot_debounce_ms = 600.0f;
        s.Run(0.3);
        s.Jab(3.0f, 0.2);
        s.Run(0.03);
        s.hand = {0.0f, 1.3f, -0.3f}; // pulled back at once
        s.Run(0.02);
        s.Jab(3.0f, 0.2);
        s.Run(0.5);
        CHECK(s.trainer.Score().shots == 1);
        CHECK(Has(s.log, "debounce"));
        s.Run(1.0);
        s.hand = {0.0f, 1.3f, -0.3f};
        s.Run(0.1);
        s.Jab(3.0f, 0.2);
        s.Run(0.3);
        CHECK(s.trainer.Score().shots == 2);
    }
    { // the head moving with the hand is not a thrust (walking forward)
        Sim s;
        s.config.attack_loop = false;
        s.Run(0.3);
        for (int i = 0; i < 40; ++i) {
            s.in.head_position.z -= 3.0f * float(kDt);
            s.hand.z -= 3.0f * float(kDt);
            s.Tick();
        }
        CHECK(s.trainer.Score().shots == 0);
    }
    { // the thresholds are tunable: a 1.2 m/s peak fires with shoot_speed 0.8
        Sim s;
        s.config.attack_loop = false;
        s.config.shoot_speed = 0.8f;
        s.Run(0.3);
        s.Jab(1.2f, 0.3);
        s.Run(0.3);
        CHECK(s.trainer.Score().shots == 1);
    }
}

void TestAssist() {
    for (int assist = 0; assist < 2; ++assist) {
        Sim s;
        s.config.trigger_fire = true;
        s.config.attack_loop = false;
        s.config.assist = assist != 0;
        s.aim_yaw_deg = 9.0f;
        s.Run(0.3);
        s.Pull();
        s.Run(0.3);
        if (assist) { // the cone pulls a 9 degree miss onto the dummy
            CHECK(s.trainer.Score().idle_hits == 1 && s.trainer.Score().misses == 0);
            CHECK(Has(s.log, "assist pulled"));
        } else {
            CHECK(s.trainer.Score().idle_hits == 0);
        }
    }
    { // far off the cone (40 degrees): no pull
        Sim s;
        s.config.trigger_fire = true;
        s.config.attack_loop = false;
        s.aim_yaw_deg = 40.0f;
        s.Run(0.3);
        s.Pull();
        s.Run(0.2);
        CHECK(Has(s.log, "assist: no pull"));
    }
}

void TestConfig() {
    const json::Value doc = json::Parse(R"({"shoot": {"speed_mps": 2.5, "debounce_ms": 400},
        "assist": {"enabled": false}, "dummy": {"window_s": 0.3, "stagger_s": 3}, "trigger_fire": true})");
    ParryConfig c;
    c.Load(doc);
    CHECK(c.shoot_speed == 2.5f && c.shoot_debounce_ms == 400.0f && !c.assist);
    CHECK(c.window_s == 0.3f && c.stagger_s == 3.0f && c.trigger_fire);
    ParryConfig d;
    d.Load(json::Parse(c.Summary("")));
    CHECK(d.shoot_speed == 2.5f && d.window_s == 0.3f && !d.assist && d.trigger_fire);
    CHECK(RejectNames(kRejectSpeed | kRejectDebounce) == "speed+debounce");
}

} // namespace

int main() {
    TestWindowTiming();
    TestVisceral();
    TestVisceralThrust();
    TestShootGate();
    TestAssist();
    TestConfig();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
