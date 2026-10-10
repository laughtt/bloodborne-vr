// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The stateful part of the controller mapping as pure logic (no OpenXR), so it can be unit
// tested (xr_math_test.cpp): the left stick click (L3) as a modifier. xr_input.cpp feeds it one
// frame at a time and documents the full mapping.

#pragma once

#include <cmath>
#include <cstdint>

#include "bbport_xr_bridge.h"

namespace xrpad {

/// The left stick click as a modifier. While L3 is held:
///   - a right stick flick past kDpadFlick is the d-pad, and the camera stops (zero_look);
///   - a B press (right controller, so the left thumb can keep L3 down) that begins now is
///     Options, an A press Touchpad, until released (a press that began before L3 stays cross or
///     circle);
///   - nothing else for kRecenterHoldMs (no layer use, the right stick never past kHoldCancel)
///     fires `recenter` once.
/// L3 itself reaches the game only on release, as a short click, and only when neither the layer
/// nor the recenter was used: the hold and the chords never leak an L3 press.
/// With `chords` false (the default map since findings/26's one-button-one-action change; the
/// chords stay for debugging, BB_XR_CHORDS=1) B and A stay cross and circle and the right stick
/// stays the camera while L3 is held: only the standing click on release and the recenter hold.
class L3Layer {
public:
    bool chords = true;

    static constexpr double kRecenterHoldMs = 1000.0;
    static constexpr float kDpadFlick = 0.7f;
    static constexpr float kHoldCancel = 0.5f;

    struct Out {
        uint32_t buttons = 0;   // held this frame: triangle/square or options/touchpad, d-pad
        uint32_t pulses = 0;    // to press briefly: L3 on release, a quick options/touchpad tap
        bool zero_look = false; // the right stick is the d-pad: no camera
        bool recenter = false;  // the hold fired this frame
        double held_ms = 0.0;   // how long L3 has been held
    };

    /// One frame: the raw left stick click, the right controller's B (top) and A (bottom) buttons,
    /// the right stick (y up) and whether the left stick is moving the character. A click that
    /// starts while moving is plain L3, held as long as the click (Bloodborne's jump is L3 while
    /// sprinting): no layer, no recenter for that press.
    Out Update(double now_ms, bool l3, bool y, bool x, float look_x, float look_y,
               bool moving = false) {
        Out out;
        if (l3 && !l3_down) {
            since = now_ms;
            layer_used = stick_moved = hold_fired = false;
            passthrough = moving;
        }
        if (passthrough) {
            if (y) {
                out.buttons |= BBXR_BTN_CROSS;
            }
            if (x) {
                out.buttons |= BBXR_BTN_CIRCLE;
            }
            y_prev = y;
            x_prev = x;
            y_layer = x_layer = false;
            if (l3) {
                out.buttons |= BBXR_BTN_L3;
            } else {
                passthrough = false;
            }
            l3_down = l3;
            return out;
        }
        const bool layer = chords && l3;
        if (y && !y_prev) {
            y_layer = layer;
            if (layer) {
                out.pulses |= BBXR_BTN_OPTIONS;
            }
        }
        if (x && !x_prev) {
            x_layer = layer;
            if (layer) {
                out.pulses |= BBXR_BTN_TOUCHPAD;
            }
        }
        y_layer = y_layer && y;
        x_layer = x_layer && x;
        y_prev = y;
        x_prev = x;
        if (y) {
            out.buttons |= y_layer ? BBXR_BTN_OPTIONS : BBXR_BTN_CROSS;
        }
        if (x) {
            out.buttons |= x_layer ? BBXR_BTN_TOUCHPAD : BBXR_BTN_CIRCLE;
        }
        if (l3) {
            layer_used = layer_used || y_layer || x_layer;
            const float ax = std::fabs(look_x), ay = std::fabs(look_y);
            if (chords && (ax > kDpadFlick || ay > kDpadFlick)) {
                if (ax > ay) {
                    out.buttons |= look_x > 0.0f ? BBXR_BTN_RIGHT : BBXR_BTN_LEFT;
                } else {
                    out.buttons |= look_y > 0.0f ? BBXR_BTN_UP : BBXR_BTN_DOWN;
                }
                layer_used = true;
            }
            stick_moved = stick_moved || ax > kHoldCancel || ay > kHoldCancel;
            out.held_ms = now_ms - since;
            if (!hold_fired && !layer_used && !stick_moved && out.held_ms >= kRecenterHoldMs) {
                hold_fired = true;
                out.recenter = true;
            }
            out.zero_look = chords;
        } else if (l3_down && !layer_used && !hold_fired) {
            out.pulses |= BBXR_BTN_L3;
        }
        l3_down = l3;
        return out;
    }

private:
    bool l3_down = false, layer_used = false, stick_moved = false, hold_fired = false;
    bool passthrough = false; // this press began while moving: plain L3
    double since = 0.0;
    bool y_prev = false, x_prev = false, y_layer = false, x_layer = false;
};

/// The runtime's menu button: a press shorter than kHoldMs is Options, sent on release (a short
/// pulse); held kHoldMs it is the bridge's recenter instead (bbport_xr_bridge.cpp) and no Options.
struct MenuTap {
    static constexpr double kHoldMs = 1000.0;
    bool down = false;
    double since = 0.0;
    /// True on the frame Options should be pulsed.
    bool Update(double now_ms, bool pressed) {
        bool tap = false;
        if (pressed && !down) {
            since = now_ms;
        } else if (!pressed && down) {
            tap = now_ms - since < kHoldMs;
        }
        down = pressed;
        return tap;
    }
};

// ---- Gesture and trigger logic (pure) ----------------------------------------------------------

struct V3 {
    float x, y, z;
};

/// A trigger as a button with hysteresis: fires once per pull (on above kOn, rearmed below kOff).
struct TriggerEdge {
    static constexpr float kOn = 0.6f, kOff = 0.3f;
    bool down = false;
    bool Update(float value) {
        if (!down && value > kOn) {
            down = true;
            return true;
        }
        if (down && value < kOff) {
            down = false;
        }
        return false;
    }
};

/// Whether a swing velocity points mostly away from the head (forward, sideways or down), not
/// back toward the body: the return or turnaround of a swing must not fire a second attack.
inline bool SwingAwayFromHead(V3 velocity, V3 hand, V3 head) {
    const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y +
                                  velocity.z * velocity.z);
    if (speed < 1e-4f) {
        return false;
    }
    if (velocity.y < -0.5f * speed) {
        return true; // a downward chop
    }
    V3 to_head{head.x - hand.x, head.y - hand.y, head.z - hand.z};
    const float d = std::sqrt(to_head.x * to_head.x + to_head.y * to_head.y + to_head.z * to_head.z);
    if (d < 1e-4f) {
        return true;
    }
    const float toward =
        (velocity.x * to_head.x + velocity.y * to_head.y + velocity.z * to_head.z) / d;
    return toward < 0.4f * speed;
}

/// The right-hand swing: fires once above kSwing, rearmed below kRearm, only when the direction
/// is fine and L3 is not held.
struct SwingGate {
    static constexpr float kSwing = 2.0f, kRearm = 1.0f;
    bool armed = true;
    bool Update(float speed, bool direction_ok, bool l3) {
        if (speed < kRearm) {
            armed = true;
            return false;
        }
        if (armed && speed > kSwing && direction_ok && !l3) {
            armed = false;
            return true;
        }
        return false;
    }
};

/// A heavy swing followed by the right grip held keeps R2 held (a charged attack) until the grip
/// is released. A grip held without a heavy swing does nothing.
struct HeavyCharge {
    static constexpr double kWindowMs = 500.0;
    static constexpr float kOn = 0.6f, kOff = 0.4f;
    bool pending = false, active = false;
    double fired_ms = 0.0;
    void OnHeavy(double now_ms) {
        pending = true;
        active = false;
        fired_ms = now_ms;
    }
    /// True while R2 is to be held.
    bool Update(double now_ms, float grip) {
        if (pending) {
            if (grip > kOn) {
                active = true;
                pending = false;
            } else if (now_ms - fired_ms > kWindowMs) {
                pending = false;
            }
        } else if (active && grip < kOff) {
            active = false;
        }
        return active;
    }
};

/// The right grip tapped: squeezed (above kOn) and released (below kOff) within kMaxMs, with no
/// swing fired during the press, is ONE tap (swap the right weapon). A grip held longer, or used
/// as the heavy modifier of a swing, is never a tap. Update returns true once, on the release.
struct GripTap {
    static constexpr float kOn = 0.6f, kOff = 0.4f;
    static constexpr double kDefaultMaxMs = 350.0;
    double max_ms = kDefaultMaxMs;
    bool down = false, swung = false;
    double pressed_ms = 0.0;
    bool Update(double now_ms, float grip, bool swing_fired) {
        if (!down) {
            if (grip > kOn) {
                down = true;
                swung = swing_fired;
                pressed_ms = now_ms;
            }
            return false;
        }
        if (swing_fired) {
            swung = true;
        }
        if (grip < kOff) {
            down = false;
            return !swung && now_ms - pressed_ms <= max_ms;
        }
        return false;
    }
};

/// The left hand held at the mouth with the left grip closed for kDwellMs fires once; it rearms
/// when the hand is more than twice the radius away.
struct MouthDwell {
    static constexpr float kRadius = 0.10f;
    static constexpr float kGrip = 0.6f;
    static constexpr double kDwellMs = 300.0;
    bool at_mouth = false, armed = true;
    double since = 0.0;
    bool Update(double now_ms, float distance, float grip) {
        bool fire = false;
        if (distance < kRadius && grip > kGrip) {
            if (!at_mouth) {
                at_mouth = true;
                since = now_ms;
            }
            if (armed && now_ms - since > kDwellMs) {
                armed = false;
                fire = true;
            }
        } else {
            at_mouth = false;
            if (distance > kRadius * 2.0f) {
                armed = true;
            }
        }
        return fire;
    }
};

/// Blocks the gun thrust for kBlockMs after a large head yaw change in one frame (a snap turn)
/// or a recenter, when the hand velocity is meaningless.
struct TurnGuard {
    static constexpr double kBlockMs = 300.0;
    static constexpr float kYawJump = 0.35f; // rad, about 20 degrees per frame
    bool have = false;
    float prev = 0.0f;
    double until = -1e9;
    static float Yaw(float qx, float qy, float qz, float qw) {
        // Yaw of the forward (-z) axis of the quaternion.
        const float fx = -(2.0f * (qx * qz + qw * qy));
        const float fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
        return std::atan2(-fx, -fz);
    }
    bool Blocked(double now_ms, float yaw, bool recenter) {
        if (have) {
            float d = yaw - prev;
            const float pi = 3.14159265f;
            while (d > pi) d -= 2.0f * pi;
            while (d < -pi) d += 2.0f * pi;
            if (std::fabs(d) > kYawJump) {
                until = now_ms + kBlockMs;
            }
        }
        if (recenter) {
            until = now_ms + kBlockMs;
        }
        have = true;
        prev = yaw;
        return now_ms < until;
    }
};

/// Right grip + right stick flick = d-pad, for cycling quick items while walking. Active only
/// when no swing is in progress and L3 is not held (the L3 layer wins). The camera stops while it
/// is used.
struct GripFlick {
    static constexpr float kGrip = 0.6f;
    struct Out {
        uint32_t buttons = 0;
        bool zero_look = false;
    };
    static Out Update(float grip, bool swing_in_progress, bool l3, float look_x, float look_y) {
        Out out;
        if (grip <= kGrip || swing_in_progress || l3) {
            return out;
        }
        const float ax = std::fabs(look_x), ay = std::fabs(look_y);
        if (ax > L3Layer::kDpadFlick || ay > L3Layer::kDpadFlick) {
            if (ax > ay) {
                out.buttons = look_x > 0.0f ? BBXR_BTN_RIGHT : BBXR_BTN_LEFT;
            } else {
                out.buttons = look_y > 0.0f ? BBXR_BTN_UP : BBXR_BTN_DOWN;
            }
        }
        out.zero_look = ax > L3Layer::kHoldCancel || ay > L3Layer::kHoldCancel;
        return out;
    }
};

/// One log line per kind per kIntervalMs.
struct RateLimit {
    static constexpr double kIntervalMs = 2000.0;
    double last[8];
    bool seen[8] = {};
    bool Allow(int kind, double now_ms) {
        if (kind < 0 || kind >= 8) {
            return false;
        }
        if (seen[kind] && now_ms - last[kind] < kIntervalMs) {
            return false;
        }
        seen[kind] = true;
        last[kind] = now_ms;
        return true;
    }
};

} // namespace xrpad
