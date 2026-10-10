// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// VR melee as pure logic (docs/vr/findings/20-swing-and-stamina.md): no OpenXR, no I/O, no heap,
// so the bridge (xr_input.cpp), sandbox3d and the offline tests (swing_logic_test.cpp) can share
// it. Three parts:
//   SwingClassifier  the sword hand's pose history -> a light swing (R1), a heavy swing (R2 tap),
//                    or a charged heavy (the wind-up held still: R2 held, the swing releases it);
//   AttackScheduler  those events -> R1/R2 presses timed to the game's combo windows (a swing
//                    that comes too early waits for the window or is dropped), gated by stamina;
//   StaminaModel     Bloodborne's stamina with the VR rule: a swing costs only when it connects.
//                    The cost is held while the swing can still hit and dropped afterwards (the
//                    refund window); a dash costs at once; nothing starts at 0 or below; regen
//                    resumes after a pause.
// Units: meters, seconds, m/s; +Y up; any fixed right-handed world space (LOCAL or STAGE). Grip
// poses follow OpenXR: -Z runs from the little finger to the thumb (where a held blade points).
// The defaults are Bloodborne's own numbers where the game has one (doc 20 section 1):
//   regen 45 points/s (0x5528318), floor -50 (the clamp in every stamina writer, e.g. 0x1cb64ae),
//   no regen while an attack or dodge animation runs (TAE event 225 SetSPRegenRatePercent = 0
//   until about 0.7-0.9 s), the attack cost charged by the behavior (BehaviorParam.stamina).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace swing {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

inline Vec3 Add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 Sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 Scale(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(Vec3 a) {
    return std::sqrt(Dot(a, a));
}
/// v + 2w(u x v) + 2u x (u x v)
inline Vec3 Rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = Scale(Cross(u, v), 2.0f);
    return Add(Add(v, Scale(t, q.w)), Cross(u, t));
}
/// Heading (radians about +Y) of a pose's -Z axis: 0 looking down -Z, positive turning left.
inline float Yaw(Quat q) {
    const Vec3 f = Rotate(q, {0.0f, 0.0f, -1.0f});
    return std::atan2(-f.x, -f.z);
}

// ---- Swing classifier ---------------------------------------------------------------------------

enum class Attack : uint8_t {
    None,
    Light,         // R1
    Heavy,         // R2 tap (the quick heavy)
    ChargeStart,   // hold R2: the wind-up was held still
    ChargeRelease, // release R2: the swing out of the wind-up (or the hold ran out)
};

inline const char* AttackName(Attack a) {
    switch (a) {
    case Attack::Light:
        return "light";
    case Attack::Heavy:
        return "heavy";
    case Attack::ChargeStart:
        return "charge";
    case Attack::ChargeRelease:
        return "release";
    default:
        return "none";
    }
}

/// One frame of the sword hand (the right controller's grip pose) and, when tracked, the head.
/// Velocities are optional: without them the classifier differentiates the poses.
struct HandSample {
    double t = 0.0;          // seconds, monotonic
    bool valid = false;      // the hand is tracked
    Vec3 position;           // grip position
    Quat orientation;        // grip orientation
    bool has_velocity = false;
    Vec3 velocity;           // m/s, the grip (OpenXR XrSpaceVelocity linear)
    Vec3 angular_velocity;   // rad/s, world frame
    float squeeze = 0.0f;    // grip button 0..1 (only for charge_needs_grip)
    bool head_valid = false; // walking, crouching and turning compensation need the head
    Vec3 head_position;
    Quat head_orientation;
};

/// Weapon classes for the presets (doc 20 section 4.3): the moveset's pace, not the virtual blade's
/// length (the controller weighs the same whatever the game shows).
enum class WeaponClass : uint8_t {
    Quick,  // one-handed fast forms: Saw Cleaver A, Threaded Cane A, Blade of Mercy, Chikage A,
            // Reiterpallasch, Rakuyo, Beast Claw
    Medium, // Saw Spear, Hunter Axe A, Kirkhammer A, Burial Blade, Ludwig's A, Church Pick,
            // Whirligig A, Saw Cleaver B, Threaded Cane B (whip)
    Heavy,  // two-handed transformed or slow: Hunter Axe B, Kirkhammer B, Ludwig's B, Beast
            // Cutter, Boom Hammer, Holy Moonlight B, Amygdalan Arm, Logarius' Wheel
};

struct SwingTuning {
    Vec3 probe{0.0f, 0.0f, -0.25f}; // the measured point in the grip frame: a short lever along
                                    // the blade, so wrist turns count as well as arm travel
    float onset_speed = 1.3f;   // m/s at the probe: a swing begins
    float light_speed = 2.2f;   // peak for a light attack
    float heavy_speed = 4.6f;   // peak for a heavy attack (decided the moment it is reached)
    float heavy_accel = 0.0f;   // m/s^2: a peak acceleration this high (with light speed) is also
                                // heavy, earlier; 0 = off
    float min_arc = 0.20f;      // m the probe travels before a light attack (heavy: half)
    float up_limit = 0.7f;      // a stroke that rises more steeply than this (direction y, when
                                // it first reaches light speed) is a wind-up, not an attack:
                                // raising the arm to strike or to charge
    double decide_s = 0.09;     // a light swing is decided this long after its onset (or when its
                                // speed falls past its peak, if sooner)
    double max_swing_s = 0.45;  // a swing that has not qualified by then is not one
    float rearm_speed = 0.9f;   // the next swing needs the probe slower than this ...
    double rearm_s = 0.04;      // ... for this long,
    bool reverse_rearm = true;  // ... or a stroke back the other way (forehand then backhand)
    float reverse_dot = -0.3f;  // cosine between the strokes that counts as "back"
    bool reverse_follow = true; // "back" is against the follow-through's latest fast direction,
                                // not the direction when the attack was decided: a long arc
                                // turns smoothly (no second attack), a real turnaround flips
    bool head_relative = true;  // subtract the head's own velocity (walking, crouching, leaning)
    float turn_comp = 1.0f;     // body turns: ignore yaw_rate * (horizontal head-probe distance)
    float glitch_m = 0.30f;     // a probe jump this large in one sample is a tracking glitch,
    float max_speed = 15.0f;    // ... and so is any sample implying more than this (m/s)
    double max_gap_s = 0.10;    // samples further apart restart the measurement
    float smooth = 0.6f;        // weight of the newest speed sample (1 = raw)
    // The charged heavy: hold the wind-up still, raised and not in front of the face.
    bool charge = true;
    float charge_above_head = -0.10f; // the hand at least this high relative to the head (m)
    float charge_behind = 0.05f;      // and at most this far in front of the face plane (m)
    float charge_still_speed = 0.8f;
    double charge_hold_s = 0.35;      // still for this long: start charging (hold R2)
    double charge_max_s = 2.5;        // the game auto-releases a full charge; release by then
    double charge_drop_s = 0.25;      // lowered out of the zone this long without a swing: release
    bool charge_needs_grip = false;   // also require the grip button held (squeeze > 0.6)
};

/// Starting points per class; tune live (sandbox3d.json "swing").
inline SwingTuning TuningFor(WeaponClass c) {
    SwingTuning t;
    switch (c) {
    case WeaponClass::Quick:
        t.light_speed = 2.0f;
        t.heavy_speed = 4.3f;
        t.min_arc = 0.18f;
        t.decide_s = 0.08;
        break;
    case WeaponClass::Medium:
        break;
    case WeaponClass::Heavy:
        t.light_speed = 2.0f;
        t.heavy_speed = 4.2f;
        t.min_arc = 0.28f;
        t.decide_s = 0.11;
        t.max_swing_s = 0.55;
        break;
    }
    return t;
}

struct SwingEvent {
    Attack kind = Attack::None;
    double t = 0.0;       // decided at
    double onset_t = 0.0; // the swing began (detection latency = t - onset_t)
    float peak_speed = 0.0f;
    float arc = 0.0f;
    float peak_accel = 0.0f;
    Vec3 direction;       // unit: the probe's motion when decided
};

class SwingClassifier {
public:
    enum class Phase : uint8_t { Armed, Swing, Rearm, Charging };

    /// A stroke that ended without an attack: too slow (peak below light_speed), too short (arc
    /// below min_arc), out of time (max_swing_s), or a wind-up (it rose). For readouts and logs;
    /// it changes nothing in the classification.
    struct Stroke {
        uint32_t count = 0; // bumped for every such stroke (compare it to see a new one)
        double t = 0.0;     // when it ended
        double onset_t = 0.0;
        float peak = 0.0f, arc = 0.0f;
        bool windup = false;
    };

    /// The last sample's measurements, for a readout (speed, label).
    struct Debug {
        Phase phase = Phase::Armed;
        float speed = 0.0f;      // the effective probe speed the thresholds see (m/s)
        float raw_speed = 0.0f;  // before the turn debit
        float turn_debit = 0.0f; // m/s ignored for a body turn
        float accel = 0.0f;
        float arc = 0.0f;        // of the swing in progress
        float peak = 0.0f;
        bool raised = false;     // the hand is in the charge zone
        bool stroke = false;     // a stroke is being measured (a swing, or one out of a charge)
        double onset_t = 0.0;    // ... since then
        bool windup = false;     // ... and it rose when it reached light speed: a wind-up
        SwingEvent last;         // the last event fired
        Stroke dropped;          // the last stroke that ended without an attack
    };

    explicit SwingClassifier(const SwingTuning& t = {}) : tuning(t) {}

    void SetTuning(const SwingTuning& t) {
        tuning = t;
    }
    const SwingTuning& Tuning() const {
        return tuning;
    }
    const Debug& State() const {
        return debug;
    }
    void Reset() {
        *this = SwingClassifier(tuning);
    }

    /// One frame. Returns the attack decided this frame (kind None almost always).
    SwingEvent Update(const HandSample& s) {
        SwingEvent out;
        if (!s.valid) {
            Interrupt();
            have = false;
            return out;
        }
        const Vec3 probe = Add(s.position, Rotate(s.orientation, tuning.probe));
        const double dt_d = s.t - last_t;
        if (!have || dt_d <= 0.0 || dt_d > tuning.max_gap_s) {
            Interrupt();
            Remember(s, probe);
            return out;
        }
        const float dt = float(dt_d);
        const Vec3 move = Sub(probe, last_probe);
        const float step = Length(move);
        if (step > tuning.glitch_m || step > tuning.max_speed * dt) {
            // a tracking jump: forget the swing, wait for calm
            Interrupt();
            speed_s = 0.0f;
            Remember(s, probe);
            return out;
        }
        Vec3 v = Scale(move, 1.0f / dt);
        if (s.has_velocity) {
            v = Add(s.velocity, Cross(s.angular_velocity, Rotate(s.orientation, tuning.probe)));
        }
        float debit = 0.0f;
        if (s.head_valid && last_head_valid) {
            const Vec3 hv = Scale(Sub(s.head_position, last_head), 1.0f / dt);
            head_vel = Add(head_vel, Scale(Sub(hv, head_vel), 0.5f));
            float dyaw = Yaw(s.head_orientation) - last_head_yaw;
            if (dyaw > 3.14159265f) {
                dyaw -= 6.28318531f;
            } else if (dyaw < -3.14159265f) {
                dyaw += 6.28318531f;
            }
            yaw_rate += (dyaw / dt - yaw_rate) * 0.5f;
            if (tuning.head_relative) {
                v = Sub(v, head_vel);
            }
            const float hx = probe.x - s.head_position.x, hz = probe.z - s.head_position.z;
            debit = tuning.turn_comp * std::fabs(yaw_rate) * std::sqrt(hx * hx + hz * hz);
        }
        const float raw = Length(v);
        const float eff = std::max(0.0f, raw - debit);
        const float prev = speed_s;
        speed_s = primed ? speed_s + tuning.smooth * (eff - speed_s) : eff;
        primed = true;
        const float accel = (speed_s - prev) / dt;
        if (raw > 1e-4f) {
            dir = Scale(v, 1.0f / raw);
        }
        debug.speed = speed_s;
        debug.raw_speed = raw;
        debug.turn_debit = debit;
        debug.accel = accel;
        debug.raised = Raised(s);

        switch (phase) {
        case Phase::Armed:
            if (ChargeHold(s)) {
                phase = Phase::Charging;
                charge_t = s.t;
                lowered_since = -1.0;
                track = {};
                out = Event(Attack::ChargeStart, s.t);
                out.onset_t = s.t;
            } else if (speed_s >= tuning.onset_speed) {
                Begin(s.t);
                phase = Phase::Swing;
                Track(speed_s, accel, step);
            }
            break;
        case Phase::Swing: {
            Track(speed_s, accel, step);
            const Attack kind = Decide(s.t);
            if (kind != Attack::None) {
                out = Event(kind, s.t);
                Commit();
            } else if (Dead(s.t)) {
                GiveUp(s.t);
                phase = Phase::Rearm;
                rest_since = -1.0;
            }
            break;
        }
        case Phase::Rearm:
            if (speed_s < tuning.rearm_speed) {
                if (rest_since < 0.0) {
                    rest_since = s.t;
                }
                if (s.t - rest_since >= tuning.rearm_s) {
                    phase = Phase::Armed;
                    raised_since = -1.0;
                }
            } else {
                rest_since = -1.0;
                if (tuning.reverse_rearm && has_commit_dir && speed_s >= tuning.onset_speed &&
                    Dot(dir, commit_dir) < tuning.reverse_dot) {
                    Begin(s.t);
                    phase = Phase::Swing;
                    Track(speed_s, accel, step);
                } else if (tuning.reverse_follow && has_commit_dir &&
                           speed_s >= tuning.onset_speed) {
                    // The follow-through still sweeps on: it is the stroke to turn back from.
                    commit_dir = dir;
                }
            }
            break;
        case Phase::Charging:
            if (track.active) {
                Track(speed_s, accel, step);
                const Attack kind = Decide(s.t);
                if (kind != Attack::None) {
                    out = Event(Attack::ChargeRelease, s.t);
                    Commit();
                    break;
                }
                if (Dead(s.t)) {
                    GiveUp(s.t);
                }
            } else if (speed_s >= tuning.onset_speed) {
                Begin(s.t);
                Track(speed_s, accel, step);
            }
            if (phase != Phase::Charging) {
                break;
            }
            if (!debug.raised && !track.active) {
                if (lowered_since < 0.0) {
                    lowered_since = s.t;
                }
            } else {
                lowered_since = -1.0;
            }
            if (s.t - charge_t >= tuning.charge_max_s ||
                (lowered_since >= 0.0 && s.t - lowered_since >= tuning.charge_drop_s)) {
                out = Event(Attack::ChargeRelease, s.t);
                out.onset_t = charge_t;
                phase = Phase::Rearm;
                rest_since = -1.0;
                has_commit_dir = false;
                track = {};
            }
            break;
        }
        debug.phase = phase;
        debug.arc = track.arc;
        debug.peak = track.peak;
        debug.stroke = track.active;
        debug.onset_t = track.onset_t;
        debug.windup = track.windup;
        if (out.kind != Attack::None) {
            debug.last = out;
        }
        Remember(s, probe);
        return out;
    }

private:
    struct Tracked {
        bool active = false;
        double onset_t = 0.0;
        float arc = 0.0f, peak = 0.0f, peak_accel = 0.0f;
        Vec3 peak_dir;       // the stroke's direction at its fastest
        bool judged = false; // the direction was checked when it first reached light speed
        bool windup = false; // ... and it rose: a wind-up
    };

    void Remember(const HandSample& s, Vec3 probe) {
        have = true;
        last_t = s.t;
        last_probe = probe;
        last_head_valid = s.head_valid;
        if (s.head_valid) {
            last_head = s.head_position;
            last_head_yaw = Yaw(s.head_orientation);
        } else {
            head_vel = {};
            yaw_rate = 0.0f;
        }
    }
    /// Tracking was lost or jumped: no swing survives it (a charge keeps its hold).
    void Interrupt() {
        primed = false;
        if (phase == Phase::Swing || phase == Phase::Armed) {
            phase = Phase::Rearm;
            rest_since = -1.0;
        }
        track = {};
        raised_since = -1.0;
        debug.stroke = debug.windup = false;
    }
    bool Raised(const HandSample& s) const {
        if (!s.head_valid) {
            return false;
        }
        const Vec3 rel = Sub(s.position, s.head_position);
        const float yaw = Yaw(s.head_orientation);
        // The head's horizontal forward is (-sin yaw, 0, -cos yaw): how far the hand is in front.
        const float ahead = rel.x * -std::sin(yaw) + rel.z * -std::cos(yaw);
        return rel.y >= tuning.charge_above_head && ahead <= tuning.charge_behind;
    }
    bool ChargeHold(const HandSample& s) {
        const bool grip_ok = !tuning.charge_needs_grip || s.squeeze > 0.6f;
        if (!tuning.charge || !debug.raised || !grip_ok || speed_s >= tuning.charge_still_speed) {
            raised_since = -1.0;
            return false;
        }
        if (raised_since < 0.0) {
            raised_since = s.t;
        }
        return s.t - raised_since >= tuning.charge_hold_s;
    }
    void Begin(double t) {
        track = {};
        track.active = true;
        track.onset_t = t;
    }
    void Track(float speed, float accel, float step) {
        if (!track.judged && speed >= tuning.light_speed) {
            track.judged = true;
            track.windup = dir.y > tuning.up_limit;
        }
        track.arc += step;
        if (speed > track.peak) {
            track.peak = speed;
            track.peak_dir = dir;
        }
        track.peak_accel = std::max(track.peak_accel, accel);
    }
    /// A stroke ended without becoming an attack (too weak, or a wind-up): a stroke back the other
    /// way may still start a swing at once.
    void GiveUp(double t) {
        if (track.peak >= tuning.onset_speed) {
            commit_dir = track.peak_dir;
            has_commit_dir = true;
        }
        debug.dropped = {debug.dropped.count + 1, t, track.onset_t, track.peak, track.arc,
                         track.windup};
        track = {};
    }
    Attack Decide(double t) const {
        if (!track.active || track.windup) {
            return Attack::None;
        }
        const bool heavy = track.peak >= tuning.heavy_speed ||
                           (tuning.heavy_accel > 0.0f && track.peak_accel >= tuning.heavy_accel &&
                            track.peak >= tuning.light_speed);
        if (heavy && track.arc >= 0.5f * tuning.min_arc) {
            return Attack::Heavy;
        }
        const bool past_peak = speed_s < 0.7f * track.peak;
        if (track.peak >= tuning.light_speed && track.arc >= tuning.min_arc &&
            (t - track.onset_t >= tuning.decide_s || past_peak)) {
            return Attack::Light;
        }
        return Attack::None;
    }
    bool Dead(double t) const {
        return t - track.onset_t > tuning.max_swing_s || speed_s < tuning.rearm_speed;
    }
    SwingEvent Event(Attack kind, double t) const {
        SwingEvent e;
        e.kind = kind;
        e.t = t;
        e.onset_t = track.active ? track.onset_t : t;
        e.peak_speed = track.peak;
        e.arc = track.arc;
        e.peak_accel = track.peak_accel;
        e.direction = dir;
        return e;
    }
    void Commit() {
        phase = Phase::Rearm;
        rest_since = -1.0;
        commit_dir = dir;
        has_commit_dir = true;
        track = {};
    }

    SwingTuning tuning;
    Debug debug;
    Phase phase = Phase::Armed;
    Tracked track;
    bool have = false, primed = false, last_head_valid = false, has_commit_dir = false;
    double last_t = 0.0, rest_since = -1.0, raised_since = -1.0, charge_t = 0.0;
    double lowered_since = -1.0;
    Vec3 last_probe, last_head, head_vel, dir{0.0f, 0.0f, -1.0f}, commit_dir;
    float last_head_yaw = 0.0f, yaw_rate = 0.0f, speed_s = 0.0f;
};

// ---- Attack scheduler ---------------------------------------------------------------------------

/// The PS4 pad bits the scheduler holds (the same values as BBXR_BTN_R1/R2 in bbport_xr_bridge.h).
constexpr uint32_t kButtonR1 = 0x800u;
constexpr uint32_t kButtonR2 = 0x200u;

/// When the game takes the next attack, in seconds after the press that started the current one
/// (the attack animation's TAE; doc 20 section 4.4 has every weapon's first R1).
struct ComboTiming {
    double input_open = 0.33; // ChrActionFlag 87 opens: a press now is buffered for the chain
    double chain_open = 0.57; // flag 4 "Cancel - RH Attack": the next attack starts
    double recover = 0.73;    // flag 11 (move): a press after this starts a fresh attack
};

struct SchedulerTuning {
    double press_s = 0.10;   // a tap's length (several game frames at 30-60 fps)
    double gap_s = 0.05;     // released this long before the same button is pressed again
    double queue_s = 0.35;   // a swing up to this long before the window opens waits for it
    double charge_max_s = 3.0;
    ComboTiming light;                  // Saw Cleaver R1 #1 (a112_030000)
    ComboTiming heavy{0.30, 0.80, 1.0}; // R2 taps; take them from the weapon's TAE
    ComboTiming charged{0.35, 0.90, 1.2};
};

enum class Drop : uint8_t {
    None,
    TooEarly,   // the game cannot take it yet and the window is too far away to wait
    Tired,      // stamina is 0 or below: the swing does not register (the game's own rule)
    Blocked,    // a menu, a cutscene or the host said so
    Superseded, // a newer swing replaced the one waiting
};

inline const char* DropName(Drop d) {
    switch (d) {
    case Drop::TooEarly:
        return "too early";
    case Drop::Tired:
        return "no stamina";
    case Drop::Blocked:
        return "blocked";
    case Drop::Superseded:
        return "superseded";
    default:
        return "none";
    }
}

class AttackScheduler {
public:
    struct Out {
        uint32_t buttons = 0;       // hold these pad buttons this frame
        Attack fired = Attack::None; // a press (or a charge hold) was started this frame
        bool queued = false;        // a swing now waits for the combo window
        Drop dropped = Drop::None;  // a swing was dropped this frame (why)
    };

    explicit AttackScheduler(const SchedulerTuning& t = {}) : tuning(t) {}
    void SetTuning(const SchedulerTuning& t) {
        tuning = t;
    }
    const SchedulerTuning& Tuning() const {
        return tuning;
    }

    /// The game reports no attack in progress (staggered, rolled, the animation ended): the next
    /// swing fires at once.
    void AttackEnded() {
        have_attack = false;
        pending = {};
    }

    /// One frame. `can_act`: stamina above 0 (the game's value, or StaminaModel::CanAct);
    /// `blocked`: a menu or cutscene (drops everything).
    Out Update(double t, const SwingEvent& e, bool can_act, bool blocked = false) {
        Out out;
        switch (e.kind) {
        case Attack::Light:
        case Attack::Heavy:
        case Attack::ChargeStart:
            Request(t, e.kind, can_act, blocked, out);
            break;
        case Attack::ChargeRelease:
            if (charging) {
                charging = false;
                charge_release_t = t;
                attack_t = t; // the charged attack starts on the release
                attack_kind = Attack::ChargeRelease;
                have_attack = true;
            } else if (pending.kind == Attack::ChargeStart) {
                pending.kind = Attack::Heavy; // released before the hold began: a tap
            }
            break;
        default:
            break;
        }
        if (pending.kind != Attack::None) {
            if (blocked) {
                out.dropped = Drop::Blocked;
                pending = {};
            } else if (t >= pending.fire_at) {
                const Attack kind = pending.kind;
                pending = {};
                Fire(t, kind, can_act, out);
            }
        }
        if (charging && t - charge_t > tuning.charge_max_s) {
            charging = false;
            charge_release_t = t;
        }
        if (press_button && t >= press_from && t < press_until) {
            out.buttons |= press_button;
        }
        if (charging && t >= press_from) {
            out.buttons |= kButtonR2;
        }
        return out;
    }

private:
    struct Pending {
        Attack kind = Attack::None;
        double fire_at = 0.0;
    };

    const ComboTiming& TimingOf(Attack kind) const {
        return kind == Attack::Light   ? tuning.light
               : kind == Attack::Heavy ? tuning.heavy
                                       : tuning.charged;
    }
    void Request(double t, Attack kind, bool can_act, bool blocked, Out& out) {
        if (blocked) {
            out.dropped = Drop::Blocked;
            return;
        }
        if (charging) { // a swing while charging is the classifier's release, not a new attack
            return;
        }
        if (have_attack) {
            const double open = attack_t + TimingOf(attack_kind).input_open;
            if (t < open) {
                if (open - t <= tuning.queue_s) {
                    if (pending.kind != Attack::None) {
                        out.dropped = Drop::Superseded;
                    }
                    pending.kind = kind;
                    pending.fire_at = open;
                    out.queued = true;
                } else {
                    out.dropped = Drop::TooEarly;
                }
                return;
            }
        }
        Fire(t, kind, can_act, out);
    }
    void Fire(double t, Attack kind, bool can_act, Out& out) {
        if (!can_act) {
            out.dropped = Drop::Tired;
            return;
        }
        const uint32_t button = kind == Attack::Light ? kButtonR1 : kButtonR2;
        double start = t;
        const double released = button == kButtonR2 ? std::max(r2_released, charge_release_t)
                                                    : r1_released;
        if (start < released + tuning.gap_s) {
            start = released + tuning.gap_s;
        }
        press_button = button;
        press_from = start;
        press_until = kind == Attack::ChargeStart ? start : start + tuning.press_s;
        if (button == kButtonR1) {
            r1_released = press_until;
        } else {
            r2_released = press_until;
        }
        if (kind == Attack::ChargeStart) {
            charging = true;
            charge_t = t;
        }
        attack_t = start;
        attack_kind = kind;
        have_attack = true;
        out.fired = kind;
    }

    SchedulerTuning tuning;
    Pending pending;
    bool have_attack = false, charging = false;
    Attack attack_kind = Attack::None;
    double attack_t = 0.0, charge_t = 0.0;
    uint32_t press_button = 0;
    double press_from = 0.0, press_until = 0.0;
    double r1_released = -1e9, r2_released = -1e9, charge_release_t = -1e9;
};

// ---- Stamina ------------------------------------------------------------------------------------

struct StaminaTuning {
    float max = 91.0f;           // a fresh hunter (the game: SprjChrDataModule +0x138)
    float floor = -50.0f;        // the game clamps every change to [-50, max]
    float regen_per_s = 45.0f;   // the game's base (0 when mirroring the game, which regenerates)
    double regen_delay_s = 0.75; // no regen this long after an attack or a dash starts (the
                                 // game: TAE 225 holds regen at 0 % until ~0.7-0.9 s)
    double hit_window_s = 0.6;   // a swing can connect this long after it started; then it
                                 // expires free (the refund window)
    float dash_cost = 20.0f;     // a dodge (the game's own cost comes from its script; measure it)
    bool gate_at_zero = true;    // nothing starts at 0 or below (the game's rule)
};

struct StaminaEvent {
    enum class Kind : uint8_t {
        None,
        Charged,     // a swing connected: its cost was taken (once per swing)
        Refunded,    // a swing's window ended without a hit: it cost nothing
        DashCharged, // a dash was paid for
        Refused,     // a swing or dash was refused: stamina 0 or below
    };
    Kind kind = Kind::None;
    float amount = 0.0f;
    uint32_t swing = 0;
    double t = 0.0;
};

class StaminaModel {
public:
    static constexpr int kMaxOpen = 8;

    explicit StaminaModel(const StaminaTuning& t = {}) : tuning(t), current(t.max) {}

    void SetTuning(const StaminaTuning& t) {
        tuning = t;
        current = std::clamp(current, t.floor, t.max);
    }
    const StaminaTuning& Tuning() const {
        return tuning;
    }
    void Reset(double t) {
        current = tuning.max;
        open_count = 0;
        regen_block = t;
        last_t = t;
        started = true;
    }
    /// Forgets the open swings without charging or refunding events (a weapon change, a reset):
    /// their cost is simply never taken. Stamina and regen are untouched.
    void DropOpen() {
        open_count = 0;
    }
    /// Game mode: take the game's own value each frame (the game charges dodges and regenerates;
    /// set regen_per_s 0). The model then only tracks the open swings and the gate.
    void Mirror(float value, float max_value) {
        current = value;
        tuning.max = max_value;
    }

    bool CanAct() const {
        return !tuning.gate_at_zero || current > 0.0f;
    }
    float Current() const {
        return current;
    }
    float Max() const {
        return tuning.max;
    }
    /// The cost of the swings that may still connect (a ghost segment on the bar).
    float Pending() const {
        float sum = 0.0f;
        for (int i = 0; i < open_count; ++i) {
            sum += open[i].cost;
        }
        return sum;
    }
    int OpenSwings() const {
        return open_count;
    }
    const StaminaEvent& LastEvent() const {
        return last_event;
    }

    /// A swing became an attack: its cost is held until Hit or until its window ends. Returns
    /// the swing's id (> 0), or 0 when refused (stamina 0 or below).
    uint32_t BeginSwing(double t, float cost) {
        Start(t);
        if (!CanAct()) {
            Note({StaminaEvent::Kind::Refused, cost, 0, t});
            return 0;
        }
        if (open_count == kMaxOpen) { // the oldest can no longer matter: it expires
            Note({StaminaEvent::Kind::Refunded, open[0].cost, open[0].id, t});
            Remove(0);
        }
        const uint32_t id = ++next_id;
        open[open_count++] = {id, cost, t + tuning.hit_window_s};
        regen_block = std::max(regen_block, t + tuning.regen_delay_s);
        return id;
    }

    /// The swing connected (a character, or the world when the caller counts clangs). The first
    /// hit takes the cost; later hits of the same swing are free. id 0: the newest open swing.
    /// Returns the amount taken (0 when the swing already paid, expired or is unknown).
    float Hit(double t, uint32_t id = 0) {
        Start(t);
        for (int i = open_count - 1; i >= 0; --i) {
            if ((id == 0 || open[i].id == id) && t <= open[i].until) {
                const Open o = open[i];
                Remove(i);
                current = std::max(tuning.floor, current - o.cost);
                Note({StaminaEvent::Kind::Charged, o.cost, o.id, t});
                return o.cost;
            }
        }
        return 0.0f;
    }

    /// A dash (dodge, quickstep, backstep): paid at once. False, and nothing paid, at 0 or below.
    bool TryDash(double t) {
        return TryDash(t, tuning.dash_cost);
    }
    /// The same with the move's own cost (the game's dodge, backstep and jump may differ).
    bool TryDash(double t, float cost) {
        Start(t);
        if (!CanAct()) {
            Note({StaminaEvent::Kind::Refused, cost, 0, t});
            return false;
        }
        current = std::max(tuning.floor, current - cost);
        regen_block = std::max(regen_block, t + tuning.regen_delay_s);
        Note({StaminaEvent::Kind::DashCharged, cost, 0, t});
        return true;
    }

    /// Every frame: regen, and the swings whose window ended (Refunded). Writes up to `capacity`
    /// events (null: none); returns how many happened.
    int Update(double t, StaminaEvent* events = nullptr, int capacity = 0) {
        Start(t);
        int n = 0;
        for (int i = 0; i < open_count;) {
            if (t > open[i].until) {
                const StaminaEvent e{StaminaEvent::Kind::Refunded, open[i].cost, open[i].id, t};
                Note(e);
                if (events && n < capacity) {
                    events[n] = e;
                }
                ++n;
                Remove(i);
            } else {
                ++i;
            }
        }
        const double from = std::max(last_t, regen_block);
        if (t > from && tuning.regen_per_s > 0.0f) {
            const double dt = std::min(t - from, 0.25); // a long hitch does not refill the bar
            current = std::min(tuning.max, current + float(dt) * tuning.regen_per_s);
        }
        last_t = std::max(last_t, t);
        return n;
    }

private:
    struct Open {
        uint32_t id = 0;
        float cost = 0.0f;
        double until = 0.0;
    };

    void Start(double t) {
        if (!started) {
            started = true;
            last_t = t;
            regen_block = t;
        }
    }
    void Remove(int i) {
        for (int k = i; k + 1 < open_count; ++k) {
            open[k] = open[k + 1];
        }
        --open_count;
    }
    void Note(const StaminaEvent& e) {
        last_event = e;
    }

    StaminaTuning tuning;
    float current;
    Open open[kMaxOpen]{};
    int open_count = 0;
    uint32_t next_id = 0;
    double regen_block = 0.0, last_t = 0.0;
    bool started = false;
    StaminaEvent last_event;
};

} // namespace swing
