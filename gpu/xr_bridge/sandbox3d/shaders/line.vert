// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Debug lines in world space (grid, skeleton, axes, laser, hit capsules).
#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;

layout(push_constant) uniform Push {
    mat4 view_proj;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = in_color;
    gl_Position = pc.view_proj * vec4(in_position, 1.0);
}
