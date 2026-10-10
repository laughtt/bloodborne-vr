// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Turning and steering for sandbox3d's locomotion (locomotion.cpp): the game's own model,
// src/vr_turn_math.h (first person, findings/16 section 9), so the sandbox and the game turn and
// walk with the same code: snap turn (30 deg past 0.6, armed again under 0.3, repeated every 0.3 s
// while held) or smooth turn (a rate, an expo, an acceleration), the view yaw psi + head, the left
// stick along move = body | head | controller, recenter and the lock-on policy.
// This header only converts between the two conventions:
//   - the shared model counts headings positive to the right (the game's sense);
//   - the sandbox (OpenXR, right-handed, -Z forward) counts yaw about +Y positive to the left
//     (YawOf of math3d.h): heading = -yaw.
// The settings use the shared model's names (vrt_config_set): the same keys as the game's
// first-person control file.

#pragma once

#include <cmath>
#include <cstdio>
#include <string>

#include "../../../src/vr_turn_math.h"
#include "json.h"
#include "math3d.h"

namespace sb::turn {

/// The shared model's settings from a JSON object (its keys: turn, snap, turn_speed, deadzone,
/// expo, turn_accel, snap_on, snap_off, snap_repeat, move, recenter, lock, lock_auto); keys it
/// does not know are ignored. Returns how many it took.
inline int Configure(VrTurnConfig& c, const json::Value& object) {
    int taken = 0;
    if (!object.IsObject()) {
        return 0;
    }
    for (const auto& [key, value] : object.object) {
        std::string text;
        if (value.IsString()) {
            text = value.Str();
        } else if (value.IsNumber()) {
            char number[32];
            std::snprintf(number, sizeof(number), "%.9g", value.number);
            text = number;
        } else {
            continue;
        }
        taken += vrt_config_set(&c, key.c_str(), text.c_str());
    }
    return taken;
}

/// The settings back as JSON pairs, in vrt_config_set's units.
inline std::string Summary(const VrTurnConfig& c) {
    char text[400];
    std::snprintf(text, sizeof(text),
                  "\"turn\": \"%s\", \"snap\": %g, \"snap_repeat\": %g, \"turn_speed\": %g, \"deadzone\": %g, "
                  "\"expo\": %g, \"turn_accel\": %g, \"move\": \"%s\"",
                  vrt_turn_name(c.turn), c.snap_rad / VRT_DEG, c.snap_repeat_s, c.smooth_rad_s / VRT_DEG, c.deadzone,
                  c.expo, c.accel_s, vrt_move_name(c.move));
    return text;
}

struct Frame {
    float turn = 0.0f;       // the play space turns by this this frame (sandbox yaw: + left)
    float turn_rate = 0.0f;  // rad/s of a smooth turn (vignette)
    m3::Vec3 move_forward{0.0f, 0.0f, -1.0f}; // "left stick up" in the world (unit, horizontal)
    m3::Vec3 move_right{1.0f, 0.0f, 0.0f};
    float move_yaw = 0.0f;   // its yaw (sandbox sense)
};

/// One frame of the shared model: `view_yaw` the play space's turn in the world (sandbox sense,
/// kept equal to -psi), the right stick's x, the head and the left controller (tracking space).
inline Frame Step(VrTurnState& s, const VrTurnConfig& c, float view_yaw, float stick_x, float dt,
                  const m3::Quat& head, bool head_valid, const m3::Quat& left_aim, bool left_valid) {
    if (!s.valid) {
        vrt_reset(&s, -view_yaw);
    }
    VrTurnInput in;
    vrt_input_clear(&in);
    in.dt = dt;
    in.stick_x = stick_x;
    in.head_yaw = head_valid ? -m3::YawOf(head) : 0.0f;
    in.controller_valid = left_valid;
    in.controller_yaw = left_valid ? -m3::YawOf(left_aim) : 0.0f;
    VrTurnOutput out;
    vrt_update(&s, &c, &in, &out);
    Frame f;
    f.turn = -out.turn_delta;
    f.turn_rate = std::fabs(out.turn_rate);
    float right = 0.0f, forward = 0.0f;
    vrt_move_vector(out.move_yaw, 0.0f, 1.0f, &right, &forward);
    f.move_forward = {right, 0.0f, -forward}; // sandbox world (x, z) = (right, -forward)
    vrt_move_vector(out.move_yaw, 1.0f, 0.0f, &right, &forward);
    f.move_right = {right, 0.0f, -forward};
    f.move_yaw = -out.move_yaw;
    return f;
}

} // namespace sb::turn
