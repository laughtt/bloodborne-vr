// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "collision.h"

#include <algorithm>
#include <cmath>

namespace sb::col {

using namespace m3;

namespace {

Vec3 Min(Vec3 a, Vec3 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
Vec3 Max(Vec3 a, Vec3 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

/// A unit vector perpendicular to the unit `axis`, as close to `hint` as possible.
Vec3 PerpendicularTowards(Vec3 axis, Vec3 hint) {
    const Vec3 p = Perp(hint, axis);
    return Length(p) > 1e-6f ? Normalize(p) : AnyPerp(axis);
}

} // namespace

void Aabb::Add(Vec3 p, float r) {
    lo = Min(lo, p - Vec3{r, r, r});
    hi = Max(hi, p + Vec3{r, r, r});
}

bool Overlap(const Aabb& a, const Aabb& b, float margin) {
    return a.lo.x <= b.hi.x + margin && b.lo.x <= a.hi.x + margin && a.lo.y <= b.hi.y + margin &&
           b.lo.y <= a.hi.y + margin && a.lo.z <= b.hi.z + margin && b.lo.z <= a.hi.z + margin;
}

Aabb Bounds(const Capsule& c) {
    Aabb box;
    box.Add(c.a, c.r);
    box.Add(c.b, c.r);
    return box;
}

Aabb Bounds(const Box& b) {
    Aabb box;
    const Vec3 ax = Rotate(b.rotation, {b.half.x, 0, 0});
    const Vec3 ay = Rotate(b.rotation, {0, b.half.y, 0});
    const Vec3 az = Rotate(b.rotation, {0, 0, b.half.z});
    const Vec3 e{std::fabs(ax.x) + std::fabs(ay.x) + std::fabs(az.x),
                 std::fabs(ax.y) + std::fabs(ay.y) + std::fabs(az.y),
                 std::fabs(ax.z) + std::fabs(ay.z) + std::fabs(az.z)};
    box.lo = b.center - e;
    box.hi = b.center + e;
    return box;
}

Vec3 ClosestOnSegment(Vec3 a, Vec3 b, Vec3 p, float* t) {
    const Vec3 d = b - a;
    const float dd = Dot(d, d);
    const float u = dd > 1e-12f ? std::clamp(Dot(p - a, d) / dd, 0.0f, 1.0f) : 0.0f;
    if (t) {
        *t = u;
    }
    return a + d * u;
}

float ClosestSegmentSegment(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, float& s, float& t, Vec3& c1,
                            Vec3& c2) {
    constexpr float kEps = 1e-12f;
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
    if (a <= kEps && e <= kEps) {
        s = t = 0.0f;
        c1 = p1;
        c2 = p2;
        return Dot(c1 - c2, c1 - c2);
    }
    if (a <= kEps) {
        s = 0.0f;
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = Dot(d1, r);
        if (e <= kEps) {
            t = 0.0f;
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = Dot(d1, d2);
            const float denom = a * e - b * b;
            if (denom > 1e-6f * a * e) {
                s = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
            } else {
                // Parallel: the middle of the overlap of the projections (a fair contact point
                // for a blade lying along a bar), else the nearer end.
                const float s0 = Dot(p2 - p1, d1) / a, s1 = Dot(q2 - p1, d1) / a;
                const float lo = std::max(0.0f, std::min(s0, s1));
                const float hi = std::min(1.0f, std::max(s0, s1));
                s = lo <= hi ? 0.5f * (lo + hi) : std::clamp(0.5f * (s0 + s1), 0.0f, 1.0f);
            }
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
    return Dot(c1 - c2, c1 - c2);
}

Contact CapsuleCapsule(const Capsule& a, const Capsule& b, Vec3 hint) {
    float s = 0.0f, t = 0.0f;
    Vec3 c1, c2;
    const float d = std::sqrt(ClosestSegmentSegment(a.a, a.b, b.a, b.b, s, t, c1, c2));
    Contact out;
    if (d > 1e-6f) {
        out.normal = (c1 - c2) * (1.0f / d);
    } else {
        // The axes cross: separate perpendicular to both (or to the one that is a segment),
        // on the hint's side.
        const Vec3 da = a.b - a.a, db = b.b - b.a;
        Vec3 n = Cross(da, db);
        if (Length(n) < 1e-9f) {
            const Vec3 axis = Length(da) > 1e-6f ? Normalize(da) : Normalize(db, {0, 1, 0});
            n = PerpendicularTowards(axis, hint);
        }
        n = Normalize(n, {0, 1, 0});
        out.normal = Dot(n, hint) < 0.0f ? -n : n;
    }
    out.depth = a.r + b.r - d;
    out.hit = out.depth > 0.0f;
    out.point_a = c1 - out.normal * a.r;
    out.point_b = c2 + out.normal * b.r;
    return out;
}

float BoxSignedDistance(const Box& box, Vec3 p, Vec3* normal) {
    const Vec3 l = Rotate(Conjugate(box.rotation), p - box.center);
    const Vec3 q{std::fabs(l.x) - box.half.x, std::fabs(l.y) - box.half.y,
                 std::fabs(l.z) - box.half.z};
    const Vec3 outside{std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)};
    const float out_len = Length(outside);
    float sd;
    Vec3 n;
    if (out_len > 0.0f) {
        sd = out_len;
        const Vec3 clamped{std::clamp(l.x, -box.half.x, box.half.x),
                           std::clamp(l.y, -box.half.y, box.half.y),
                           std::clamp(l.z, -box.half.z, box.half.z)};
        n = Normalize(l - clamped, {0, 1, 0});
    } else {
        // Inside: the nearest face.
        if (q.x >= q.y && q.x >= q.z) {
            sd = q.x;
            n = {l.x >= 0.0f ? 1.0f : -1.0f, 0, 0};
        } else if (q.y >= q.z) {
            sd = q.y;
            n = {0, l.y >= 0.0f ? 1.0f : -1.0f, 0};
        } else {
            sd = q.z;
            n = {0, 0, l.z >= 0.0f ? 1.0f : -1.0f};
        }
    }
    if (normal) {
        *normal = Rotate(box.rotation, n);
    }
    return sd;
}

Contact CapsuleBox(const Capsule& a, const Box& b) {
    const Vec3 d = a.b - a.a;
    // The search runs in the box's frame (one rotation for the whole segment).
    const Quat inv = Conjugate(b.rotation);
    const Vec3 la = Rotate(inv, a.a - b.center), ld = Rotate(inv, d);
    const auto f = [&](float t) {
        const Vec3 l = la + ld * t;
        const float qx = std::fabs(l.x) - b.half.x, qy = std::fabs(l.y) - b.half.y,
                    qz = std::fabs(l.z) - b.half.z;
        const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f), oz = std::max(qz, 0.0f);
        return std::sqrt(ox * ox + oy * oy + oz * oz) + std::min(std::max(qx, std::max(qy, qz)), 0.0f);
    };
    float t_best = 0.0f;
    if (Dot(d, d) > 1e-12f) {
        // Golden-section search for the minimum of the convex f on [0, 1].
        constexpr float kPhi = 0.6180339887f;
        float lo = 0.0f, hi = 1.0f;
        float x1 = hi - kPhi * (hi - lo), x2 = lo + kPhi * (hi - lo);
        float f1 = f(x1), f2 = f(x2);
        for (int i = 0; i < 24; ++i) {
            if (f1 <= f2) {
                hi = x2;
                x2 = x1;
                f2 = f1;
                x1 = hi - kPhi * (hi - lo);
                f1 = f(x1);
            } else {
                lo = x1;
                x1 = x2;
                f1 = f2;
                x2 = lo + kPhi * (hi - lo);
                f2 = f(x2);
            }
        }
        t_best = 0.5f * (lo + hi);
        float f_best = f(t_best);
        // A segment lying along a face has a flat minimum: take the middle of it (the set where
        // a convex function is near its minimum is an interval), so the push does not twist.
        float t_lo = t_best, t_hi = t_best;
        float samples[5];
        for (int k = 0; k <= 4; ++k) {
            samples[k] = f(float(k) / 4.0f);
            if (samples[k] < f_best) {
                f_best = samples[k];
                t_best = t_lo = t_hi = float(k) / 4.0f;
            }
        }
        for (int k = 0; k <= 4; ++k) {
            if (samples[k] <= f_best + 0.0005f) {
                t_lo = std::min(t_lo, float(k) / 4.0f);
                t_hi = std::max(t_hi, float(k) / 4.0f);
            }
        }
        t_best = 0.5f * (t_lo + t_hi);
    }
    const Vec3 p = a.a + d * t_best;
    Vec3 n;
    const float sd = BoxSignedDistance(b, p, &n);
    Contact out;
    out.normal = n;
    out.depth = a.r - sd;
    out.hit = out.depth > 0.0f;
    out.point_a = p - n * a.r;
    out.point_b = p - n * sd;
    return out;
}

Contact CapsulePlane(const Capsule& a, const Plane& p) {
    const float sa = Dot(p.normal, a.a) - p.d, sb = Dot(p.normal, a.b) - p.d;
    Vec3 point;
    float sd;
    if (std::fabs(sa - sb) < 5e-4f) {
        point = (a.a + a.b) * 0.5f;
        sd = 0.5f * (sa + sb);
    } else {
        point = sa < sb ? a.a : a.b;
        sd = std::min(sa, sb);
    }
    Contact out;
    out.normal = p.normal;
    out.depth = a.r - sd;
    out.hit = out.depth > 0.0f;
    out.point_a = point - p.normal * a.r;
    out.point_b = point - p.normal * sd;
    return out;
}

// ---- 3x3 -----------------------------------------------------------------------------------

Mat3 operator*(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r(i, j) = a(i, 0) * b(0, j) + a(i, 1) * b(1, j) + a(i, 2) * b(2, j);
        }
    }
    return r;
}

Mat3 operator+(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 9; ++i) {
        r.m[i] = a.m[i] + b.m[i];
    }
    return r;
}

Mat3 operator*(const Mat3& a, float s) {
    Mat3 r;
    for (int i = 0; i < 9; ++i) {
        r.m[i] = a.m[i] * s;
    }
    return r;
}

Vec3 operator*(const Mat3& a, Vec3 v) {
    return {a(0, 0) * v.x + a(0, 1) * v.y + a(0, 2) * v.z,
            a(1, 0) * v.x + a(1, 1) * v.y + a(1, 2) * v.z,
            a(2, 0) * v.x + a(2, 1) * v.y + a(2, 2) * v.z};
}

Mat3 Transpose(const Mat3& a) {
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r(i, j) = a(j, i);
        }
    }
    return r;
}

Mat3 Inverse(const Mat3& a) {
    Mat3 r;
    r(0, 0) = a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1);
    r(0, 1) = a(0, 2) * a(2, 1) - a(0, 1) * a(2, 2);
    r(0, 2) = a(0, 1) * a(1, 2) - a(0, 2) * a(1, 1);
    r(1, 0) = a(1, 2) * a(2, 0) - a(1, 0) * a(2, 2);
    r(1, 1) = a(0, 0) * a(2, 2) - a(0, 2) * a(2, 0);
    r(1, 2) = a(0, 2) * a(1, 0) - a(0, 0) * a(1, 2);
    r(2, 0) = a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0);
    r(2, 1) = a(0, 1) * a(2, 0) - a(0, 0) * a(2, 1);
    r(2, 2) = a(0, 0) * a(1, 1) - a(0, 1) * a(1, 0);
    const float det = a(0, 0) * r(0, 0) + a(0, 1) * r(1, 0) + a(0, 2) * r(2, 0);
    if (std::fabs(det) < 1e-20f) {
        return Mat3{};
    }
    return r * (1.0f / det);
}

Mat3 RotationOf(Quat q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    Mat3 r;
    r(0, 0) = 1 - 2 * (y * y + z * z);
    r(0, 1) = 2 * (x * y - z * w);
    r(0, 2) = 2 * (x * z + y * w);
    r(1, 0) = 2 * (x * y + z * w);
    r(1, 1) = 1 - 2 * (x * x + z * z);
    r(1, 2) = 2 * (y * z - x * w);
    r(2, 0) = 2 * (x * z - y * w);
    r(2, 1) = 2 * (y * z + x * w);
    r(2, 2) = 1 - 2 * (x * x + y * y);
    return r;
}

Mat3 Diagonal(float s) {
    Mat3 r;
    r(0, 0) = r(1, 1) = r(2, 2) = s;
    return r;
}

Mat3 Outer(Vec3 a, Vec3 b) {
    Mat3 r;
    const float av[3] = {a.x, a.y, a.z}, bv[3] = {b.x, b.y, b.z};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r(i, j) = av[i] * bv[j];
        }
    }
    return r;
}

void SymmetricEigen(const Mat3& input, float values[3], Vec3 vectors[3]) {
    Mat3 a = input, v;
    const auto rotate = [&](int p, int q) {
        if (std::fabs(a(p, q)) < 1e-20f) {
            return;
        }
        const float theta = (a(q, q) - a(p, p)) / (2.0f * a(p, q));
        const float t =
            (theta >= 0.0f ? 1.0f : -1.0f) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0f));
        const float c = 1.0f / std::sqrt(t * t + 1.0f), s = t * c;
        for (int k = 0; k < 3; ++k) {
            const float akp = a(k, p), akq = a(k, q);
            a(k, p) = c * akp - s * akq;
            a(k, q) = s * akp + c * akq;
        }
        for (int k = 0; k < 3; ++k) {
            const float apk = a(p, k), aqk = a(q, k);
            a(p, k) = c * apk - s * aqk;
            a(q, k) = s * apk + c * aqk;
        }
        for (int k = 0; k < 3; ++k) {
            const float vkp = v(k, p), vkq = v(k, q);
            v(k, p) = c * vkp - s * vkq;
            v(k, q) = s * vkp + c * vkq;
        }
    };
    for (int sweep = 0; sweep < 32; ++sweep) {
        const float off = a(0, 1) * a(0, 1) + a(0, 2) * a(0, 2) + a(1, 2) * a(1, 2);
        const float scale = a(0, 0) * a(0, 0) + a(1, 1) * a(1, 1) + a(2, 2) * a(2, 2);
        if (off <= 1e-14f * std::max(scale, 1e-30f)) {
            break;
        }
        rotate(0, 1);
        rotate(0, 2);
        rotate(1, 2);
    }
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int i, int j) { return a(i, i) > a(j, j); });
    for (int k = 0; k < 3; ++k) {
        values[k] = a(order[k], order[k]);
        vectors[k] = Normalize(Vec3{v(0, order[k]), v(1, order[k]), v(2, order[k])},
                               k == 0 ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
    }
    vectors[2] = Normalize(Cross(vectors[0], vectors[1]), vectors[2]);
}

// ---- Poses ---------------------------------------------------------------------------------

Pose PoseOf(const Mat4& m) {
    Pose p;
    p.x = Translation(m);
    p.q = ToQuat(m);
    p.s = Length(Vec3{m.m[0], m.m[1], m.m[2]});
    if (p.s < 1e-6f) {
        p.s = 1.0f;
    }
    return p;
}

Mat4 MatrixOf(const Pose& p) {
    return Translate(p.x) * FromQuat(p.q) * Scale(p.s);
}

float AngleBetween(Quat a, Quat b) {
    const float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0f * std::acos(std::min(d, 1.0f));
}

Pose Interpolate(const Pose& a, const Pose& b, float t) {
    Pose r;
    r.x = Lerp(a.x, b.x, t);
    r.q = Slerp(a.q, b.q, t);
    r.s = b.s;
    return r;
}

} // namespace sb::col
