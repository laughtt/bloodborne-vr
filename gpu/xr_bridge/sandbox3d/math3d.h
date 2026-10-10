// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Small 3D math for sandbox3d: vectors, unit quaternions (x, y, z, w) and 4x4 column-major
// matrices (column vectors, Vulkan/GLM order: translation in m[12..14]). Right-handed, +Y up,
// -Z forward, as OpenXR and the extracted models (scripts/vr_assets/bbmodel.py) use.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

namespace m3 {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
};
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return a * s; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a, Vec3 fallback = {0, 0, 0}) {
    const float l = Length(a);
    return l > 1e-8f ? a * (1.0f / l) : fallback;
}
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
/// The part of v perpendicular to the unit axis.
inline Vec3 Perp(Vec3 v, Vec3 axis) { return v - axis * Dot(v, axis); }
/// Any unit vector perpendicular to the unit v.
inline Vec3 AnyPerp(Vec3 v) {
    return Normalize(std::fabs(v.x) < 0.9f ? Cross(v, {1, 0, 0}) : Cross(v, {0, 1, 0}));
}

// ---- Quaternions ------------------------------------------------------------------------------

inline Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat Conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat Normalize(Quat q) {
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-12f ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}
inline Vec3 Rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = Cross(u, v) * 2.0f;
    return v + t * q.w + Cross(u, t);
}
inline Quat AxisAngle(Vec3 axis, float radians) {
    const Vec3 a = Normalize(axis, {0, 1, 0});
    const float s = std::sin(radians * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
}
/// The shortest rotation taking unit `from` to unit `to`.
inline Quat FromTo(Vec3 from, Vec3 to) {
    const float d = Dot(from, to);
    if (d < -0.999999f) {
        return AxisAngle(AnyPerp(from), kPi);
    }
    const Vec3 c = Cross(from, to);
    return Normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}
/// Signed angle (radians) from unit a to unit b about the unit axis (both perpendicular to it).
inline float SignedAngle(Vec3 a, Vec3 b, Vec3 axis) {
    return std::atan2(Dot(Cross(a, b), axis), Dot(a, b));
}
inline Quat Slerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f) {
        return Normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                              a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    }
    const float th = std::acos(std::clamp(d, -1.0f, 1.0f));
    const float s = std::sin(th);
    const float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}
/// Euler degrees (applied X, then Y, then Z, intrinsic like most tools: q = Z * Y * X).
inline Quat FromEulerDeg(Vec3 deg) {
    return AxisAngle({0, 0, 1}, deg.z * kDeg) * AxisAngle({0, 1, 0}, deg.y * kDeg) *
           AxisAngle({1, 0, 0}, deg.x * kDeg);
}
/// Rotation about +Y only, keeping the heading of q's -Z axis.
inline float YawOf(Quat q) {
    const Vec3 f = Rotate(q, {0, 0, -1});
    return std::atan2(-f.x, -f.z);
}

// ---- Matrices -----------------------------------------------------------------------------------

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}
inline Vec3 TransformPoint(const Mat4& a, Vec3 p) {
    return {a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12],
            a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13],
            a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14]};
}
inline Vec3 TransformDir(const Mat4& a, Vec3 d) {
    return {a.m[0] * d.x + a.m[4] * d.y + a.m[8] * d.z, a.m[1] * d.x + a.m[5] * d.y + a.m[9] * d.z,
            a.m[2] * d.x + a.m[6] * d.y + a.m[10] * d.z};
}
inline Vec3 Translation(const Mat4& a) { return {a.m[12], a.m[13], a.m[14]}; }
inline Mat4 Translate(Vec3 t) {
    Mat4 r;
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}
inline Mat4 Scale(float s) {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = s;
    return r;
}
inline Mat4 FromQuat(Quat q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    Mat4 r;
    r.m[0] = 1 - 2 * (y * y + z * z);
    r.m[1] = 2 * (x * y + z * w);
    r.m[2] = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w);
    r.m[5] = 1 - 2 * (x * x + z * z);
    r.m[6] = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w);
    r.m[9] = 2 * (y * z - x * w);
    r.m[10] = 1 - 2 * (x * x + y * y);
    return r;
}
/// Rigid transform: rotation q then translation t.
inline Mat4 FromPose(Quat q, Vec3 t) {
    Mat4 r = FromQuat(q);
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}
/// The rotation of a matrix with orthogonal (possibly uniformly scaled) axes.
inline Quat ToQuat(const Mat4& a) {
    Vec3 x = Normalize(Vec3{a.m[0], a.m[1], a.m[2]});
    Vec3 y = Normalize(Vec3{a.m[4], a.m[5], a.m[6]});
    Vec3 z = Normalize(Vec3{a.m[8], a.m[9], a.m[10]});
    const float trace = x.x + y.y + z.z;
    Quat q;
    if (trace > 0) {
        const float s = 2 * std::sqrt(trace + 1);
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (x.x > y.y && x.x > z.z) {
        const float s = 2 * std::sqrt(1 + x.x - y.y - z.z);
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        const float s = 2 * std::sqrt(1 + y.y - x.x - z.z);
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const float s = 2 * std::sqrt(1 + z.z - x.x - y.y);
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    return Normalize(q);
}
/// General inverse (Cramer); the identity when singular.
inline Mat4 Inverse(const Mat4& a) {
    const float* m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r;
    if (std::fabs(det) < 1e-20f) {
        return r;
    }
    for (int i = 0; i < 16; ++i) {
        r.m[i] = inv[i] / det;
    }
    return r;
}
/// Reflection through the plane with unit normal n passing through point p (det -1).
inline Mat4 Reflection(Vec3 n, Vec3 p) {
    Mat4 r;
    const float d = Dot(n, p);
    const float nn[3] = {n.x, n.y, n.z};
    for (int c = 0; c < 3; ++c) {
        for (int row = 0; row < 3; ++row) {
            r.m[c * 4 + row] = (c == row ? 1.0f : 0.0f) - 2.0f * nn[row] * nn[c];
        }
        r.m[12 + c] = 2.0f * d * nn[c];
    }
    return r;
}
/// OpenXR-style asymmetric projection for Vulkan clip space (y down, depth 0..1), FOV angles in
/// radians (left/down negative). far <= near means an infinite far plane.
inline Mat4 ProjectionFov(float left, float right, float up, float down, float n, float f) {
    const float tl = std::tan(left), tr = std::tan(right), tu = std::tan(up),
                td = std::tan(down);
    const float w = tr - tl, h = td - tu; // h < 0: Vulkan's y points down
    Mat4 r;
    r.m[0] = 2.0f / w;
    r.m[5] = 2.0f / h;
    r.m[8] = (tr + tl) / w;
    r.m[9] = (tu + td) / h;
    if (f <= n) {
        r.m[10] = -1.0f;
        r.m[14] = -n;
    } else {
        r.m[10] = -f / (f - n);
        r.m[14] = -(f * n) / (f - n);
    }
    r.m[11] = -1.0f;
    r.m[15] = 0.0f;
    return r;
}

} // namespace m3
