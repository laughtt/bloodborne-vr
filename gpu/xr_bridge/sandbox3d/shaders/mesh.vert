// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Skinned mesh: four bone weights per vertex into a palette of world * pose * inverse-bind
// matrices (rigid weapons use the same path, every bone the weapon's world matrix).
#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_bones;
layout(location = 4) in vec4 in_weights;

layout(std430, set = 0, binding = 0) readonly buffer Palette {
    mat4 bones[];
};

layout(push_constant) uniform Push {
    mat4 view_proj;
    vec4 color;
    uint palette_base;
    uint flags;
} pc;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec3 out_world;

void main() {
    mat4 skin = in_weights.x * bones[pc.palette_base + in_bones.x] +
                in_weights.y * bones[pc.palette_base + in_bones.y] +
                in_weights.z * bones[pc.palette_base + in_bones.z] +
                in_weights.w * bones[pc.palette_base + in_bones.w];
    vec4 world = skin * vec4(in_position, 1.0);
    out_world = world.xyz;
    out_normal = mat3(skin) * in_normal;
    gl_Position = pc.view_proj * world;
}
