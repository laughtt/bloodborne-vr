// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Collision math for sandbox3d's solid weapons (physics_weapon.h), pure CPU and unit tested
// (collision_test.cpp): capsules (a segment swept by a radius; a sphere when both ends meet),
// oriented boxes and planes, their contacts (normal, depth, the deepest points), a 3x3 matrix
// with the symmetric eigen decomposition the weapon shapes use, and rigid poses.

#pragma once

#include "math3d.h"

namespace sb::col {

struct Capsule {
    m3::Vec3 a, b;
    float r = 0.0f;
};

/// Oriented box: centre, rotation, half extents along its local axes.
struct Box {
    m3::Vec3 center;
    m3::Quat rotation;
    m3::Vec3 half{0.5f, 0.5f, 0.5f};
};

/// The half space below dot(normal, p) = d is solid.
struct Plane {
    m3::Vec3 normal{0.0f, 1.0f, 0.0f};
    float d = 0.0f;
};

/// How A touches B. `normal` points from B toward A: moving A along it by `depth` separates
/// them. depth <= 0 means apart by -depth (only the capsule tests fill it in then).
struct Contact {
    bool hit = false;
    m3::Vec3 normal{0.0f, 1.0f, 0.0f};
    float depth = 0.0f;
    m3::Vec3 point_a; // A's surface point deepest inside B
    m3::Vec3 point_b; // B's surface point deepest inside A
};

struct Aabb {
    m3::Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    void Add(m3::Vec3 p, float r = 0.0f);
    bool Valid() const { return lo.x <= hi.x; }
};
bool Overlap(const Aabb& a, const Aabb& b, float margin = 0.0f);
Aabb Bounds(const Capsule& c);
Aabb Bounds(const Box& b);

/// The point of segment a..b closest to p (and its parameter t in 0..1).
m3::Vec3 ClosestOnSegment(m3::Vec3 a, m3::Vec3 b, m3::Vec3 p, float* t = nullptr);
/// Closest points c1 on p1..q1 and c2 on p2..q2 (Ericson, Real-Time Collision Detection 5.1.9);
/// returns their squared distance.
float ClosestSegmentSegment(m3::Vec3 p1, m3::Vec3 q1, m3::Vec3 p2, m3::Vec3 q2, float& s,
                            float& t, m3::Vec3& c1, m3::Vec3& c2);

/// Capsule A against capsule B. `hint` is the separating direction used when the axes cross
/// (distance 0), e.g. the direction from B's centre to A's.
Contact CapsuleCapsule(const Capsule& a, const Capsule& b, m3::Vec3 hint = {0.0f, 1.0f, 0.0f});
/// Signed distance from p to the box (negative inside) and the outward normal there.
float BoxSignedDistance(const Box& box, m3::Vec3 p, m3::Vec3* normal = nullptr);
/// Capsule A against box B: the axis point with the smallest signed distance (the box's signed
/// distance is convex, so a golden-section search along the segment finds it).
Contact CapsuleBox(const Capsule& a, const Box& b);
Contact CapsulePlane(const Capsule& a, const Plane& p);

// ---- 3x3 matrices --------------------------------------------------------------------------

struct Mat3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // row-major
    float& operator()(int r, int c) { return m[r * 3 + c]; }
    float operator()(int r, int c) const { return m[r * 3 + c]; }
};
Mat3 operator*(const Mat3& a, const Mat3& b);
Mat3 operator+(const Mat3& a, const Mat3& b);
Mat3 operator*(const Mat3& a, float s);
m3::Vec3 operator*(const Mat3& a, m3::Vec3 v);
Mat3 Transpose(const Mat3& a);
Mat3 Inverse(const Mat3& a); // identity when singular
Mat3 RotationOf(m3::Quat q);
/// s * I.
Mat3 Diagonal(float s);
/// v v^T.
Mat3 Outer(m3::Vec3 a, m3::Vec3 b);
/// Eigenvalues (descending) and unit eigenvectors (columns of `vectors`, right-handed) of a
/// symmetric matrix (Jacobi rotations).
void SymmetricEigen(const Mat3& a, float values[3], m3::Vec3 vectors[3]);

// ---- Rigid poses with a uniform scale --------------------------------------------------------

struct Pose {
    m3::Vec3 x;
    m3::Quat q;
    float s = 1.0f;
};
Pose PoseOf(const m3::Mat4& m);
m3::Mat4 MatrixOf(const Pose& p);
inline m3::Vec3 Apply(const Pose& p, m3::Vec3 local) {
    return p.x + m3::Rotate(p.q, local * p.s);
}
inline m3::Vec3 Unapply(const Pose& p, m3::Vec3 world) {
    return m3::Rotate(m3::Conjugate(p.q), world - p.x) * (1.0f / p.s);
}
/// Angle between two rotations (radians, 0..pi).
float AngleBetween(m3::Quat a, m3::Quat b);
/// Interpolates position and rotation (the scale is b's).
Pose Interpolate(const Pose& a, const Pose& b, float t);

} // namespace sb::col
