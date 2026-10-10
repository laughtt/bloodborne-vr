// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Wireframe hit/hurt shapes as line segments, shared by the sandbox's scene (scene.cpp, GL-style
// lines) and the game's hitbox overlay (vr_game_hitbox.cpp, thin quads in the VR UI layer): the
// same capsule wireframe and the same colours in both.

#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include "math3d.h"

namespace sb {

struct WireColor {
    uint8_t r, g, b;
};

/// A circle of `segments` lines (an arc from..to) around `center` in the plane of unit u, v.
template <class LineFn>
void WireRing(LineFn&& line, m3::Vec3 center, m3::Vec3 u, m3::Vec3 v, float r, float from = 0.0f,
              float to = 2.0f * m3::kPi, int segments = 16) {
    m3::Vec3 prev = center + u * (r * std::cos(from)) + v * (r * std::sin(from));
    for (int i = 1; i <= segments; ++i) {
        const float a = from + (to - from) * float(i) / float(segments);
        const m3::Vec3 p = center + u * (r * std::cos(a)) + v * (r * std::sin(a));
        line(prev, p);
        prev = p;
    }
}

/// Wireframe capsule a..b of radius r (a sphere when a == b): line(p, q) per segment.
template <class LineFn>
void WireCapsule(LineFn&& line, m3::Vec3 a, m3::Vec3 b, float r) {
    using m3::Vec3;
    Vec3 axis = b - a;
    const float length = m3::Length(axis);
    axis = length > 1e-5f ? axis * (1.0f / length) : Vec3{0, 1, 0};
    const Vec3 u = m3::AnyPerp(axis);
    const Vec3 v = m3::Cross(axis, u);
    if (length <= 1e-5f) {
        WireRing(line, a, u, v, r);
        WireRing(line, a, u, axis, r);
        WireRing(line, a, v, axis, r);
        return;
    }
    WireRing(line, a, u, v, r);
    WireRing(line, b, u, v, r);
    for (int k = 0; k < 4; ++k) {
        const float ang = float(k) * m3::kPi * 0.5f;
        const Vec3 off = u * (r * std::cos(ang)) + v * (r * std::sin(ang));
        line(a + off, b + off);
    }
    // End caps: half circles bulging outward along the axis.
    WireRing(line, b, u, axis, r, 0.0f, m3::kPi, 8);
    WireRing(line, b, v, axis, r, 0.0f, m3::kPi, 8);
    WireRing(line, a, u, -axis, r, 0.0f, m3::kPi, 8);
    WireRing(line, a, v, -axis, r, 0.0f, m3::kPi, 8);
}

/// A weapon hit shape's colour by its bbhitbox hit_type.
inline WireColor HitTypeColor(const std::string& type) {
    if (type == "tip") {
        return {255, 60, 60};
    }
    if (type == "middle") {
        return {255, 150, 40};
    }
    if (type == "root") {
        return {255, 230, 60};
    }
    return {255, 90, 200};
}

/// A hurtbox's colour by its part group (player: 1 torso, 2 arms, 3 legs, 31 head).
inline WireColor HurtGroupColor(int part_group) {
    return part_group == 1    ? WireColor{90, 200, 255}
           : part_group == 2  ? WireColor{120, 255, 160}
           : part_group == 3  ? WireColor{255, 210, 90}
           : part_group == 31 ? WireColor{255, 120, 220}
                              : WireColor{200, 200, 200};
}

} // namespace sb
