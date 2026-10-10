// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Flat color with two-sided key light, a dimmer fill and a sky/ground ambient (linear output
// into an sRGB target; alpha 1 for the Pimax compositor, which blends by alpha).
#version 450

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec3 in_world;

layout(push_constant) uniform Push {
    mat4 view_proj;
    vec4 color;
    uint palette_base;
    uint flags;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 n = normalize(in_normal);
    const vec3 key = normalize(vec3(0.35, 0.9, 0.25));
    const vec3 fill = normalize(vec3(-0.6, 0.3, -0.5));
    float light = 0.28 + 0.12 * n.y + 0.62 * abs(dot(n, key)) + 0.18 * max(dot(n, fill), 0.0);
    if ((pc.flags & 1u) != 0u) {
        // Grid-like checker for the floor: every 0.5 m.
        vec2 cell = floor(in_world.xz * 2.0);
        light *= mod(cell.x + cell.y, 2.0) < 1.0 ? 1.0 : 0.82;
    }
    out_color = vec4(pc.color.rgb * light, 1.0);
}
