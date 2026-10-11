// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The parry trainer of sandbox3d (docs/vr/findings/41-parry.md): the game's left-hand gun thrust
// and aim assist, and a training dummy that telegraphs an attack on a loop, so the parry can be
// tuned in the headset without the game. Pure CPU (parry_test.cpp), drawn by parry_view.cpp.
//   - the shot: ShootGate (../xr_input_logic.h, the thresholds of BB_XR_SHOOT_*) fires on a jab of
//     the left hand along its barrel, from the hand's velocity relative to the head; every shot
//     decision (fired, or rejected for speed / alignment / falling / debounce / disarmed) is logged
//     once per jab with the numbers, so the thresholds can be tuned from the log;
//   - the aim assist: vg_assist_dir (../../../src/vr_gun_math.h, BB_VR_PARRY_*) pulls the barrel's
//     ray toward the dummy's chest inside a distance-scaled cone;
//   - the bullet: a fast projectile, a tracer and a muzzle flash, a haptic recoil;
//   - the dummy: the sandbox's enemy (its hurtbox capsules) in front of the player. Loop: idle ->
//     wind-up (amber, the weapon rises, a ring closes on the head) -> PARRY WINDOW (window_s, the
//     ring turns green) -> swing (red) -> recover. A bullet in the window is a PARRY: the dummy
//     staggers stagger_s, during which a right-hand melee hit is a VISCERAL; before the window the
//     shot was early, after it late (by N ms); a bullet that hits nothing is a miss.
// The "parry" object of sandbox3d.json (hot reloaded with the rest, config.h).

#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "../xr_input_logic.h"
#include "collision.h"
#include "json.h"
#include "math3d.h"

namespace sb {

struct ParryConfig {
    bool enabled = true;
    bool trigger_fire = false; // the left trigger also fires (for aim practice without the thrust)
    bool sound = true;         // a beep at the wind-up and at the window
    // The thrust gate (the game's defaults; BB_XR_SHOOT_SPEED / _ALIGN / _REARM / _DEBOUNCE_MS
    // set the defaults here too).
    float shoot_speed = 1.5f, shoot_align = 0.8f, shoot_rearm = 0.5f;
    float shoot_debounce_ms = 250.0f;
    // The aim assist (BB_VR_PARRY_*).
    bool assist = true;
    float assist_strength = 0.8f, assist_radius_m = 0.6f;
    float assist_cone_min_deg = 4.0f, assist_cone_max_deg = 18.0f, assist_range_m = 12.0f;
    // The bullet.
    float bullet_speed = 60.0f, bullet_range_m = 40.0f;
    // The dummy: straight ahead of the head at calibration (the physics enemy is moved here).
    bool dummy = true;
    bool attack_loop = true; // false: it stands still (aim practice)
    float distance_m = 2.2f, x_m = 0.0f;
    float idle_s = 1.2f, windup_s = 0.7f, window_s = 0.15f, swing_s = 0.35f, recover_s = 0.4f;
    float stagger_s = 2.0f, down_s = 1.0f;
    // The visceral ("parry.visceral"): how the staggered dummy is finished. mode "thrust" (default):
    // a forward thrust of the right hand at the chest (or a melee hit); "swing": only the melee
    // hit; "auto": by itself once the right hand or the head is within reach_m.
    enum VisceralMode { kVisceralThrust, kVisceralSwing, kVisceralAuto };
    VisceralMode visceral_mode = kVisceralThrust;
    float visceral_speed = 1.5f, visceral_align = 0.7f, visceral_reach_m = 1.2f;
    float visceral_debounce_ms = 300.0f;
    static const char* ModeName(VisceralMode m);

    ParryConfig();
    void Load(const json::Value& d);
    std::string Summary(const std::string& indent) const;
};

inline bool ParryEnvNumber(const char* name, double& out) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return false;
    }
    out = std::atof(v);
    return true;
}


inline ParryConfig::ParryConfig() {
    // The same environment the game reads (xr_input.cpp, runtime_vr_gun.c): the sandbox starts
    // from the values the game would use.
    double x = 0.0;
    if (ParryEnvNumber("BB_XR_SHOOT_SPEED", x) && x >= 0.5 && x <= 6.0) shoot_speed = float(x);
    if (ParryEnvNumber("BB_XR_SHOOT_ALIGN", x) && x >= -1.0 && x <= 0.99) shoot_align = float(x);
    if (ParryEnvNumber("BB_XR_SHOOT_REARM", x) && x >= 0.0 && x <= 3.0) shoot_rearm = float(x);
    if (ParryEnvNumber("BB_XR_SHOOT_DEBOUNCE_MS", x) && x >= 0.0 && x <= 2000.0) shoot_debounce_ms = float(x);
    if (ParryEnvNumber("BB_VR_PARRY_ASSIST", x)) assist_strength = std::clamp(float(x), 0.0f, 1.0f);
    if (ParryEnvNumber("BB_VR_PARRY_RADIUS_M", x) && x > 0.0) assist_radius_m = float(x);
    if (ParryEnvNumber("BB_VR_PARRY_CONE_MIN", x) && x >= 0.0) assist_cone_min_deg = float(x);
    if (ParryEnvNumber("BB_VR_PARRY_CONE_MAX", x) && x >= 0.0) assist_cone_max_deg = float(x);
    if (ParryEnvNumber("BB_VR_PARRY_RANGE_M", x) && x > 0.0) assist_range_m = float(x);
}

struct ParryInput {
    double time = 0.0; // seconds, the scene's clock
    bool head_valid = false;
    m3::Vec3 head_position;
    m3::Quat head_rotation;
    bool hand_valid = false; // the left controller
    m3::Vec3 hand_position;  // grip
    m3::Vec3 aim_position;
    m3::Quat aim_rotation;
    float trigger = 0.0f;
    bool hand_free = true;   // the left hand holds no item
    bool muzzle_valid = false;
    m3::Vec3 muzzle;         // the gun's barrel end (else the aim pose + 12 cm)
    float floor_y = 0.0f;
    // The dummy's hurtbox capsules in the world (the physics enemy's); empty: the trainer's own.
    std::vector<col::Capsule> parts;
    bool rhand_valid = false; // the right controller (the visceral thrust)
    m3::Vec3 rhand_position;  // grip
    m3::Quat rhand_rotation;  // aim
    double melee_hit_time = -1.0; // the last right-hand melee hit on the dummy (scene clock), < 0 none
};

struct ParryEvents {
    std::string log;
    float haptic_amplitude[2] = {}, haptic_seconds[2] = {};
    std::string toast_title, toast_body;
    float beep_hz = 0.0f, beep_ms = 0.0f; // > 0: play it
};

struct ParryScore {
    int shots = 0, parries = 0, early = 0, late = 0, misses = 0, visceral = 0, idle_hits = 0;
};

/// Why a jab did not fire (bits; a frame's set of failed conditions).
enum ShotReject : unsigned {
    kRejectSpeed = 1,     // thrust along the barrel below the threshold
    kRejectAlign = 2,     // the stroke is not along the barrel
    kRejectFalling = 4,   // the thrust is not rising (past the onset)
    kRejectDebounce = 8,  // too soon after the last shot
    kRejectDisarmed = 16, // not re-armed (the thrust did not fall below rearm since the last shot)
    kRejectReach = 32,    // (visceral thrust) the hand is farther than reach_m from the chest
};
std::string RejectNames(unsigned bits);

class ParryTrainer {
public:
    enum Phase { kIdle, kWindUp, kWindow, kSwing, kRecover, kStagger, kDown };
    struct Bullet {
        m3::Vec3 pos, dir;
        double born = 0.0;
        float travelled = 0.0f;
        float closest_m = 1e9f; // the nearest it passed to the dummy's chest
        bool alive = true;
    };

    /// One frame: the thrust gate, bullets, the dummy's clock, the score.
    void Step(const ParryInput& in, const ParryConfig& config, ParryEvents& events);
    void Reset();

    // ---- State for the view and the tests
    Phase GetPhase() const { return phase; }
    static const char* PhaseName(Phase p);
    double PhaseStart() const { return phase_start; }
    double PhaseEnd() const { return phase_end; }
    double WindowStart() const { return window_start; } // of the current / last attack
    double WindowEnd() const { return window_start + window_len; }
    const ParryScore& Score() const { return score; }
    std::string ScoreText() const;
    const std::vector<Bullet>& Bullets() const { return bullets; }
    const std::vector<col::Capsule>& Parts() const { return parts; }
    /// The aim point (the chest) and the base on the floor / facing of the dummy.
    m3::Vec3 Target() const { return target; }
    m3::Vec3 Base() const { return base; }
    m3::Vec3 Facing() const { return facing; }
    float Height() const { return height; }
    bool HaveDummy() const { return have_dummy; }
    // The last shot, for the flash.
    double LastShotTime() const { return last_shot; }
    m3::Vec3 LastMuzzle() const { return last_muzzle; }
    m3::Vec3 LastDir() const { return last_dir; }
    /// The gate's numbers of this frame (thrust t along the barrel m/s, alignment).
    float Thrust() const { return thrust; }
    float Alignment() const { return align; }
    /// How long the window shows green after it opened / how far the telegraph ring is closed.
    float WindUpFraction(double now) const;

private:
    struct Sample {
        double t;
        m3::Vec3 hand, head;
    };
    struct Hit {
        size_t bullet;
        double time;
        float dist;
    };
    void AdvanceTo(double t, const ParryConfig& c, ParryEvents& e);
    void EnterPhase(Phase p, double t, double length);
    void Judge(double t, const ParryConfig& c, ParryEvents& e);
    void Fire(const ParryInput& in, const ParryConfig& c, ParryEvents& e, const char* how);
    void UpdateShape(const ParryInput& in, const ParryConfig& c);
    void Say(ParryEvents& e, const std::string& text);
    void Pulse(ParryEvents& e, int hand, float amplitude, float seconds);
    void Visceral(double t, const ParryConfig& c, ParryEvents& e, const char* how, const std::string& detail);
    void VisceralStep(const ParryInput& in, const ParryConfig& c, ParryEvents& e);

    Phase phase = kIdle;
    double phase_start = 0.0, phase_end = 0.0;
    double window_start = 0.0, window_len = 0.0;
    bool started = false;
    double last_time = 0.0;
    ParryScore score;
    std::vector<Bullet> bullets;
    std::vector<col::Capsule> parts, own_parts;
    bool own_placed = false;
    m3::Vec3 target, base, facing{0, 0, 1};
    float height = 1.8f;
    bool have_dummy = false;
    double last_melee_seen = -1.0;
    // The gate (the game's own, ../xr_input_logic.h).
    xrpad::ShootGate gate;
    bool gate_init = false;
    std::vector<Sample> history;
    bool have_head_prev = false;
    m3::Vec3 head_prev;
    float head_yaw_prev = 0.0f;
    float thrust = 0.0f, align = 0.0f;
    // The jab being judged: the best frame (fewest failed conditions, then the highest thrust).
    bool attempt = false, attempt_fired = false;
    unsigned attempt_bits = 0;
    float attempt_thrust = 0.0f, attempt_align = 0.0f, attempt_peak = 0.0f;
    float trigger_prev = 0.0f;
    // The visceral thrust gate of the right hand (the shape of xrpad::ShootGate, aimed at the chest).
    std::vector<Sample> rhistory;
    bool vis_have_head = false, vis_armed = true, vis_attempt = false, vis_fired = false;
    m3::Vec3 vis_head_prev;
    float vis_yaw_prev = 0.0f, vis_prev = 0.0f;
    double vis_last_ms = -1e9;
    unsigned vis_bits = 0;
    float vis_best_t = 0.0f, vis_best_a = 0.0f, vis_best_d = 0.0f, vis_peak = 0.0f;
    double last_shot = -1e9;
    m3::Vec3 last_muzzle, last_dir;
};

} // namespace sb
