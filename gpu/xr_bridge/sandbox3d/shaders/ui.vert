// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// VR menu geometry (ui_render.h): world-space triangles built on the CPU every frame.
#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;  // linear RGB, straight alpha
layout(location = 3) in vec4 in_shape;  // x: mode, yzw: mode parameters
layout(location = 4) in vec4 in_shape2; // more mode parameters

layout(push_constant) uniform Push {
    mat4 view_proj;
} pc;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) flat out vec4 out_shape;
layout(location = 3) flat out vec4 out_shape2;

void main() {
    out_uv = in_uv;
    out_color = in_color;
    out_shape = in_shape;
    out_shape2 = in_shape2;
    gl_Position = pc.view_proj * vec4(in_position, 1.0);
}
