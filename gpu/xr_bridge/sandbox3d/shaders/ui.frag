// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// VR menu shading, one pipeline for every element (mode in shape.x):
//   0 flat color
//   1 icon: the icon atlas (sRGB, straight alpha) times the color
//   2 text: the font's signed distance field; shape.y widens the glyph (outlines), shape.z softens
//   3 rounded rectangle: uv = position from the center; shape.yz = half size, shape.w = radius
//   4 ring sector: uv = position from the ring center, rotated so the sector is centered on +X;
//     shape.y/z = inner/outer radius, shape.w = half angle (>= pi: full ring), shape2.x = gap
// Edges are antialiased over one pixel (fwidth). Output is premultiplied (blend ONE,
// ONE_MINUS_SRC_ALPHA on color; the alpha channel keeps the 1 the compositor needs).
#version 450

layout(set = 0, binding = 0) uniform sampler2D icons;
layout(set = 0, binding = 1) uniform sampler2D font;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) flat in vec4 in_shape;
layout(location = 3) flat in vec4 in_shape2;

layout(location = 0) out vec4 out_color;

float Coverage(float d) {
    // d: signed distance in the uv unit, > 0 outside.
    float w = max(fwidth(d), 1e-6);
    return clamp(0.5 - d / w, 0.0, 1.0);
}

void main() {
    int mode = int(in_shape.x + 0.5);
    vec4 c = in_color;
    if (mode == 1) {
        c *= texture(icons, in_uv);
    } else if (mode == 2) {
        float d = texture(font, in_uv).r;
        float w = max(fwidth(d) * 0.75, 1e-4) + in_shape.z;
        c.a *= smoothstep(0.5 - in_shape.y - w, 0.5 - in_shape.y + w, d);
    } else if (mode == 3) {
        vec2 q = abs(in_uv) - in_shape.yz + vec2(in_shape.w);
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - in_shape.w;
        c.a *= Coverage(d);
    } else if (mode == 4) {
        float r = length(in_uv);
        float d = max(in_shape.y - r, r - in_shape.z);
        if (in_shape.w < 3.14) {
            vec2 n1 = vec2(-sin(in_shape.w), cos(in_shape.w));
            vec2 n2 = vec2(-sin(in_shape.w), -cos(in_shape.w));
            float edge = max(dot(in_uv, n1), dot(in_uv, n2)) + 0.5 * in_shape2.x;
            if (in_shape.w < 1.5707) {
                d = max(d, edge);
            } else {
                d = max(d, min(dot(in_uv, n1), dot(in_uv, n2)) + 0.5 * in_shape2.x);
            }
        }
        c.a *= Coverage(d);
    }
    out_color = vec4(c.rgb * c.a, c.a);
}
