// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The trick-weapon transform gesture as pure logic (docs/vr/findings/33-trick-transform.md): no
// OpenXR, no I/O, no heap, shared by sandbox3d and the bridge like swing_logic.h.
//
// The gesture is a WRIST SNAP of the weapon hand: the grip turns by min_angle_deg or more (any
// axis) within window_s while the grip itself barely travels (max_travel_m). That is the flat
// game's L1 (the transform). What keeps it apart from a swing, which turns the controller too:
//   - a swing moves the hand along an arc (0.4-0.7 m in 150 ms), a snap turns it in place;
//   - the swing classifier's probe sits 0.25 m along the blade, so a wrist snap moves the probe
//     at 2-5 m/s and would read as a light or heavy attack. The detector therefore raises a VETO
//     while the wrist turns fast without the hand travelling (rate >= veto_rate and grip speed
//     <= veto_grip_speed): a few frames after the snap starts, well before the classifier
//     decides (about 90 ms after its onset), held veto_hold_s after the last such sample. The
//     caller skips SwingClassifier::Update while Veto() is true (the classifier restarts when
//     the samples resume). A fired transform holds the veto lock_s (the transform animation is
//     not interruptible by a swing anyway).
// Cooldown: one transform per cooldown_s, and the wrist must come to rest before the next (the
// snap back is not a second transform).
// Units: meters, seconds, radians; the same HandSample as swing_logic.h.

#pragma once

#include <cmath>

#include "swing_logic.h"

namespace swing {

struct TransformTuning {
    bool enabled = true;
    float min_angle_deg = 90.0f; // the grip's turn within the window
    double window_s = 0.15;
    float max_travel_m = 0.20f;  // the grip's travel within the window: more is a swing
    double cooldown_s = 0.8;
    float rest_rate = 3.0f;      // rad/s: below this the wrist is at rest and the next snap arms
    // The swing veto.
    float veto_rate = 9.0f;       // rad/s of the grip's turn ...
    float veto_grip_speed = 0.8f; // ... with the grip itself slower than this (m/s)
    double veto_hold_s = 0.25;    // the veto outlasts the last such sample
    double lock_s = 0.6;          // and a fired transform holds it this long
    double max_gap_s = 0.10;      // samples further apart restart the measurement
    float glitch_m = 0.30f;       // a jump this large in one sample is a tracking glitch
};

struct TransformEvent {
    bool fired = false;     // the transform gesture was completed this sample
    float angle_deg = 0.0f; // the turn that fired it
    float travel_m = 0.0f;  // the grip's travel in the window
    double turn_s = 0.0;    // how long that turn took (<= window_s)
};

class TrickTransformDetector {
public:
    explicit TrickTransformDetector(const TransformTuning& t = {}) : tuning(t) {}
    void SetTuning(const TransformTuning& t) {
        tuning = t;
    }
    const TransformTuning& Tuning() const {
        return tuning;
    }
    void Reset() {
        *this = TrickTransformDetector(tuning);
    }

    /// One frame of the weapon hand. The swing classifier must not see this sample while Veto().
    TransformEvent Update(const HandSample& s) {
        TransformEvent ev;
        if (!tuning.enabled || !s.valid) {
            count = 0;
            return ev;
        }
        if (count > 0) {
            const Entry& last = At(count - 1);
            const double dt = s.t - last.t;
            if (dt <= 0.0 || dt > tuning.max_gap_s ||
                Length(Sub(s.position, last.position)) > tuning.glitch_m) {
                count = 0;
            }
        }
        Push({s.t, s.position, s.orientation});
        now = s.t;

        // The current turn rate and grip speed over about 40 ms (steady against frame jitter).
        rate = 0.0f;
        grip_speed = 0.0f;
        {
            int pick = -1;
            for (int i = count - 2; i >= 0; --i) {
                pick = i;
                if (now - At(i).t >= 0.035) {
                    break;
                }
            }
            if (pick >= 0) {
                const double dt = now - At(pick).t;
                if (dt > 1e-6) {
                    rate = float(AngleBetween(At(pick).orientation, s.orientation) / dt);
                    grip_speed = float(Length(Sub(s.position, At(pick).position)) / dt);
                }
            }
        }
        if (rate >= tuning.veto_rate && grip_speed <= tuning.veto_grip_speed) {
            veto_until = std::fmax(veto_until, now + tuning.veto_hold_s);
        }
        if (rate < tuning.rest_rate && now >= cooldown_until) {
            armed = true;
        }

        // The snap: the largest turn and travel inside the window.
        float best_angle = 0.0f, travel = 0.0f;
        double best_age = 0.0;
        for (int i = 0; i < count - 1; ++i) {
            const double age = now - At(i).t;
            if (age > tuning.window_s) {
                continue;
            }
            const float a = AngleBetween(At(i).orientation, s.orientation);
            if (a > best_angle) {
                best_angle = a;
                best_age = age;
            }
            travel = std::fmax(travel, Length(Sub(s.position, At(i).position)));
        }
        const float need = tuning.min_angle_deg * kRad;
        if (armed && now >= cooldown_until && best_angle >= need &&
            travel <= tuning.max_travel_m) {
            ev.fired = true;
            ev.angle_deg = best_angle / kRad;
            ev.travel_m = travel;
            ev.turn_s = best_age;
            armed = false;
            cooldown_until = now + tuning.cooldown_s;
            veto_until = std::fmax(veto_until, now + tuning.lock_s);
        }
        return ev;
    }

    /// True while the swing classifier must not see the hand (a snap in progress or just done).
    bool Veto() const {
        return tuning.enabled && now < veto_until;
    }
    bool CoolingDown() const {
        return now < cooldown_until;
    }
    /// Turn rate (rad/s) and grip speed (m/s) at the last sample, for a readout.
    float Rate() const {
        return rate;
    }
    float GripSpeed() const {
        return grip_speed;
    }

private:
    static constexpr float kRad = 3.14159265f / 180.0f;
    static constexpr int kCap = 48;
    struct Entry {
        double t = 0.0;
        Vec3 position;
        Quat orientation;
    };

    /// The angle (radians, 0..pi) between two orientations.
    static float AngleBetween(Quat a, Quat b) {
        float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        const float na = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
        const float nb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w);
        if (na < 1e-6f || nb < 1e-6f) {
            return 0.0f;
        }
        d = std::fmin(d / (na * nb), 1.0f);
        return 2.0f * std::acos(d);
    }
    const Entry& At(int i) const {
        return ring[(head + i) % kCap];
    }
    void Push(const Entry& e) {
        if (count == kCap) {
            head = (head + 1) % kCap;
            --count;
        }
        ring[(head + count) % kCap] = e;
        ++count;
        while (count > 1 && e.t - At(0).t > tuning.window_s + 0.1) {
            head = (head + 1) % kCap;
            --count;
        }
    }

    TransformTuning tuning;
    Entry ring[kCap];
    int head = 0, count = 0;
    double now = 0.0, cooldown_until = -1e9, veto_until = -1e9;
    bool armed = true;
    float rate = 0.0f, grip_speed = 0.0f;
};

} // namespace swing
