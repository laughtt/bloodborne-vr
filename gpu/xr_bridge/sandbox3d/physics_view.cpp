// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "physics_view.h"

#include <algorithm>
#include <cmath>

namespace sb {

using namespace m3;

namespace {

struct Rgb {
    uint8_t r, g, b;
};

constexpr Rgb kWeaponFree{90, 230, 120}, kWeaponTouch{255, 150, 30};
constexpr Rgb kPropLine{200, 160, 110}, kWallLine{150, 150, 160}, kHurt{70, 200, 230};
constexpr Rgb kGhost{125, 205, 240}, kTether{170, 120, 70};

void Line(std::vector<LineVertex>& out, Vec3 a, Vec3 b, Rgb c) {
    out.push_back({{a.x, a.y, a.z}, {c.r, c.g, c.b, 255}});
    out.push_back({{b.x, b.y, b.z}, {c.r, c.g, c.b, 255}});
}

Rgb Mix(Rgb a, Rgb b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {uint8_t(float(a.r) + (float(b.r) - float(a.r)) * t),
            uint8_t(float(a.g) + (float(b.g) - float(a.g)) * t),
            uint8_t(float(a.b) + (float(b.b) - float(a.b)) * t)};
}

void Ring(std::vector<LineVertex>& out, Vec3 center, Vec3 u, Vec3 v, float r, Rgb c,
          float from = 0.0f, float to = 2.0f * kPi, int segments = 12) {
    Vec3 prev = center + u * (r * std::cos(from)) + v * (r * std::sin(from));
    for (int i = 1; i <= segments; ++i) {
        const float a = from + (to - from) * float(i) / float(segments);
        const Vec3 p = center + u * (r * std::cos(a)) + v * (r * std::sin(a));
        Line(out, prev, p, c);
        prev = p;
    }
}

void CapsuleLines(std::vector<LineVertex>& out, const col::Capsule& cap, Rgb c) {
    Vec3 axis = cap.b - cap.a;
    const float length = Length(axis);
    axis = length > 1e-5f ? axis * (1.0f / length) : Vec3{0, 1, 0};
    const Vec3 u = AnyPerp(axis);
    const Vec3 v = Cross(axis, u);
    if (length <= 1e-5f) {
        Ring(out, cap.a, u, v, cap.r, c);
        Ring(out, cap.a, u, axis, cap.r, c);
        Ring(out, cap.a, v, axis, cap.r, c);
        return;
    }
    Ring(out, cap.a, u, v, cap.r, c);
    Ring(out, cap.b, u, v, cap.r, c);
    for (int k = 0; k < 4; ++k) {
        const float ang = float(k) * kPi * 0.5f;
        const Vec3 off = u * (cap.r * std::cos(ang)) + v * (cap.r * std::sin(ang));
        Line(out, cap.a + off, cap.b + off, c);
    }
    Ring(out, cap.b, u, axis, cap.r, c, 0.0f, kPi, 6);
    Ring(out, cap.b, v, axis, cap.r, c, 0.0f, kPi, 6);
    Ring(out, cap.a, u, -axis, cap.r, c, 0.0f, kPi, 6);
    Ring(out, cap.a, v, -axis, cap.r, c, 0.0f, kPi, 6);
}

void BoxLines(std::vector<LineVertex>& out, const col::Box& b, Rgb c) {
    Vec3 corner[8];
    for (int i = 0; i < 8; ++i) {
        const Vec3 l{(i & 1) ? b.half.x : -b.half.x, (i & 2) ? b.half.y : -b.half.y,
                     (i & 4) ? b.half.z : -b.half.z};
        corner[i] = b.center + Rotate(b.rotation, l);
    }
    for (int i = 0; i < 8; ++i) {
        for (int bit = 1; bit < 8; bit <<= 1) {
            if (!(i & bit)) {
                Line(out, corner[i], corner[i | bit], c);
            }
        }
    }
}

Vertex MakeVertex(Vec3 p, Vec3 n) {
    Vertex v{};
    v.position[0] = p.x;
    v.position[1] = p.y;
    v.position[2] = p.z;
    v.normal[0] = n.x;
    v.normal[1] = n.y;
    v.normal[2] = n.z;
    v.weights[0] = 1.0f;
    return v;
}

/// Radius 1, y from -0.5 to 0.5, with caps.
void MakeCylinder(std::vector<Vertex>& v, std::vector<uint32_t>& idx, int segments) {
    for (int i = 0; i <= segments; ++i) {
        const float a = 2.0f * kPi * float(i) / float(segments);
        const Vec3 n{std::cos(a), 0.0f, std::sin(a)};
        v.push_back(MakeVertex({n.x, -0.5f, n.z}, n));
        v.push_back(MakeVertex({n.x, 0.5f, n.z}, n));
    }
    for (int i = 0; i < segments; ++i) {
        const uint32_t b = uint32_t(i * 2);
        idx.insert(idx.end(), {b, b + 1, b + 2, b + 1, b + 3, b + 2});
    }
    for (int cap = 0; cap < 2; ++cap) {
        const float y = cap ? 0.5f : -0.5f;
        const Vec3 n{0.0f, cap ? 1.0f : -1.0f, 0.0f};
        const uint32_t center = uint32_t(v.size());
        v.push_back(MakeVertex({0.0f, y, 0.0f}, n));
        for (int i = 0; i <= segments; ++i) {
            const float a = 2.0f * kPi * float(i) / float(segments);
            v.push_back(MakeVertex({std::cos(a), y, std::sin(a)}, n));
        }
        for (int i = 0; i < segments; ++i) {
            idx.insert(idx.end(), {center, center + 1 + uint32_t(i), center + 2 + uint32_t(i)});
        }
    }
}

/// The unit cube centred on the origin.
void MakeBox(std::vector<Vertex>& v, std::vector<uint32_t>& idx) {
    const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3& n : normals) {
        const Vec3 u = AnyPerp(n), w = Cross(n, u);
        const uint32_t base = uint32_t(v.size());
        const float s[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& c : s) {
            v.push_back(MakeVertex((n + u * c[0] + w * c[1]) * 0.5f, n));
        }
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

/// Columns x, y, z (scaled axes) and the translation t.
Mat4 Frame(Vec3 x, Vec3 y, Vec3 z, Vec3 t) {
    Mat4 m;
    m.m[0] = x.x, m.m[1] = x.y, m.m[2] = x.z;
    m.m[4] = y.x, m.m[5] = y.y, m.m[6] = y.z;
    m.m[8] = z.x, m.m[9] = z.y, m.m[10] = z.z;
    m.m[12] = t.x, m.m[13] = t.y, m.m[14] = t.z;
    return m;
}

void DrawProp(FrameData& out, int model, uint32_t index_count, const Mat4& world, float r, float g,
          float b) {
    MeshDraw draw;
    draw.model = model;
    draw.index_count = index_count;
    draw.palette_base = uint32_t(out.palette.size());
    draw.color[0] = r, draw.color[1] = g, draw.color[2] = b;
    out.palette.push_back(world);
    out.meshes.push_back(draw);
}

} // namespace

bool PhysicsView::Upload(const WeaponPhysics& physics, Renderer& renderer, std::string& log) {
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;
    MakeCylinder(v, idx, 24);
    cylinder = renderer.UploadModel(v, idx);
    cylinder_indices = uint32_t(idx.size());
    v.clear();
    idx.clear();
    MakeBox(v, idx);
    box = renderer.UploadModel(v, idx);
    box_indices = uint32_t(idx.size());
    Sync(physics, renderer);
    if (cylinder < 0 || box < 0) {
        log += "physics: prop upload failed: " + renderer.LastError() + "\n";
        return false;
    }
    return true;
}

void PhysicsView::Sync(const WeaponPhysics& physics, Renderer& renderer) {
    const WeaponPhysics::Enemy& enemy = physics.GetEnemy();
    if (enemy.generation == enemy_generation) {
        return;
    }
    enemy_generation = enemy.generation;
    enemy_gpu = enemy.has_model ? renderer.UploadModel(enemy.model.vertices, enemy.model.indices)
                                : -1;
}

void PhysicsView::Draw(const WeaponPhysics& physics, const PhysicsConfig& cfg,
                       FrameData& out) const {
    const WeaponPhysics::Props& props = physics.GetProps();
    const WeaponPhysics::Enemy& enemy = physics.GetEnemy();
    const double now = physics.Now();
    const float marker_s = std::max(cfg.marker_s, 0.05f);
    std::vector<float> flash(enemy.parts.size(), 0.0f);
    for (const WeaponPhysics::Marker& m : physics.Markers()) {
        if (m.part >= 0 && size_t(m.part) < flash.size()) {
            flash[size_t(m.part)] =
                std::max(flash[size_t(m.part)], 1.0f - float(now - m.time) / marker_s);
        }
    }

    // Props.
    if (cfg.props && props.valid && cylinder >= 0 && box >= 0) {
        const col::Capsule& post = props.post;
        const float r = post.r, height = post.b.y - post.a.y + 2.0f * r;
        const Vec3 base = post.a - Vec3{0, r, 0};
        DrawProp(out, cylinder, cylinder_indices,
             Frame({r, 0, 0}, {0, height, 0}, {0, 0, r}, base + Vec3{0, 0.5f * height, 0}), 0.40f,
             0.29f, 0.18f);
        const col::Capsule& bar = props.crossbar;
        const float length = Length(bar.b - bar.a);
        if (length > 1e-3f && bar.r > 0.0f) {
            const Vec3 y = (bar.b - bar.a) * (1.0f / length);
            const Vec3 x = AnyPerp(y), z = Cross(x, y);
            // The rounded ends of the capsule collider, drawn as a slightly longer bar.
            DrawProp(out, cylinder, cylinder_indices,
                 Frame(x * bar.r, y * (length + bar.r), z * bar.r, (bar.a + bar.b) * 0.5f),
                 0.46f, 0.34f, 0.21f);
        }
        const col::Box& wall = props.wall;
        DrawProp(out, box, box_indices,
             Frame(Rotate(wall.rotation, {2.0f * wall.half.x, 0, 0}),
                   Rotate(wall.rotation, {0, 2.0f * wall.half.y, 0}),
                   Rotate(wall.rotation, {0, 0, 2.0f * wall.half.z}), wall.center),
             0.42f, 0.42f, 0.45f);
    }
    // The enemy at its bind pose: every palette entry is its world matrix.
    if (enemy.placed && enemy.has_model && enemy_gpu >= 0) {
        const uint32_t base = uint32_t(out.palette.size());
        for (size_t b = 0; b < std::max<size_t>(enemy.model.bones.size(), 1); ++b) {
            out.palette.push_back(enemy.world);
        }
        const float tones[4][3] = {
            {0.33f, 0.29f, 0.25f}, {0.26f, 0.23f, 0.21f}, {0.42f, 0.35f, 0.28f}, {0.30f, 0.30f, 0.32f}};
        for (size_t i = 0; i < enemy.model.meshes.size(); ++i) {
            const MeshRange& mesh = enemy.model.meshes[i];
            if (mesh.hidden) {
                continue;
            }
            MeshDraw draw;
            draw.model = enemy_gpu;
            draw.first_index = mesh.first_index;
            draw.index_count = mesh.index_count;
            draw.palette_base = base;
            const bool weapon = mesh.dominant_bone_name.find("Weapon") != std::string::npos;
            const float* tone = weapon ? tones[3] : tones[i % 3];
            draw.color[0] = tone[0], draw.color[1] = tone[1], draw.color[2] = tone[2];
            out.meshes.push_back(draw);
        }
    }

    // Collider wireframes.
    if (cfg.show_colliders && cfg.enabled) {
        for (int side = 0; side < 2; ++side) {
            const WeaponPhysics::Hand& hand = physics.GetHand(side);
            if (!hand.active || !hand.have_pose) {
                continue;
            }
            for (size_t i = 0; i < hand.shape.capsules.size(); ++i) {
                const col::Capsule& c = hand.shape.capsules[i];
                const col::Capsule w{col::Apply(hand.pose, c.a), col::Apply(hand.pose, c.b),
                                     c.r * hand.pose.s};
                const bool touch = i < hand.touching.size() && hand.touching[i];
                CapsuleLines(out.lines, w, touch ? kWeaponTouch : kWeaponFree);
            }
        }
    }
    if (cfg.show_colliders && cfg.props && props.valid) {
        CapsuleLines(out.lines, props.post, kPropLine);
        if (props.crossbar.r > 0.0f) {
            CapsuleLines(out.lines, props.crossbar, kPropLine);
        }
        BoxLines(out.lines, props.wall, kWallLine);
    }
    if (enemy.placed) {
        for (size_t p = 0; p < enemy.parts.size(); ++p) {
            const col::Capsule& c = enemy.parts[p];
            const col::Capsule w{TransformPoint(enemy.world, c.a), TransformPoint(enemy.world, c.b),
                                 c.r};
            if (flash[p] > 0.0f) {
                CapsuleLines(out.overlay, w, Mix({60, 20, 20}, {255, 50, 40}, flash[p]));
            } else if (cfg.show_colliders) {
                CapsuleLines(out.lines, w, kHurt);
            }
        }
    }

    // The real controller hand where a blocked weapon holds the hand back.
    if (cfg.show_ghost && cfg.enabled) {
        for (int side = 0; side < 2; ++side) {
            const WeaponPhysics::Hand& hand = physics.GetHand(side);
            if (!hand.active || !hand.have_pose || hand.ghost.empty()) {
                continue;
            }
            const Quat turn = Normalize(hand.pose.q * Conjugate(hand.target.q));
            const auto held_at = [&](Vec3 p) {
                return hand.pose.x + Rotate(turn, p - hand.target.x);
            };
            const Vec3 held = held_at(hand.ghost_wrist);
            // Shown when any joint of the hand is that far from the controller's (a weapon
            // turned about the grip moves the fingers, not the wrist).
            float moved = Length(held - hand.ghost_wrist);
            for (const auto& segment : hand.ghost) {
                moved = std::max(moved, Length(held_at(segment.second) - segment.second));
            }
            if (moved < cfg.ghost_min_m) {
                continue;
            }
            Vec3 knuckles{};
            int count = 0;
            for (const auto& [a, b] : hand.ghost) {
                // Thin wire tubes: a hand outline that reads in the headset, still see-through.
                const bool palm = Length(a - hand.ghost_wrist) < 1e-6f;
                CapsuleLines(out.overlay, {a, b, palm ? 0.012f : 0.007f}, kGhost);
                if (palm) {
                    knuckles = knuckles + b;
                    ++count;
                }
            }
            // A ring around the wrist, across the forearm (wrist minus the knuckles' middle).
            const Vec3 forearm =
                count ? Normalize(hand.ghost_wrist - knuckles * (1.0f / float(count)), {0, 1, 0})
                      : Vec3{0, 1, 0};
            Ring(out.overlay, hand.ghost_wrist, AnyPerp(forearm), Cross(forearm, AnyPerp(forearm)),
                 0.035f, kGhost);
            Line(out.overlay, hand.ghost_wrist, held, kTether);
        }
    }

    // Hit markers: a burst at the hit point that grows and fades.
    for (const WeaponPhysics::Marker& m : cfg.hits ? physics.Markers() : std::vector<WeaponPhysics::Marker>{}) {
        const float age = std::clamp(float(now - m.time) / marker_s, 0.0f, 1.0f);
        const Rgb c = Mix({255, 90, 30}, {50, 20, 15}, age);
        const float size = 0.05f + 0.12f * age;
        const Vec3 dirs[7] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.577f, 0.577f, 0.577f},
                              {-0.577f, 0.577f, 0.577f}, {0.577f, -0.577f, 0.577f},
                              {0.577f, 0.577f, -0.577f}};
        for (const Vec3& d : dirs) {
            Line(out.overlay, m.point - d * size, m.point + d * size, c);
        }
        Ring(out.overlay, m.point, {1, 0, 0}, {0, 1, 0}, size * 0.8f, c);
        Ring(out.overlay, m.point, {0, 0, 1}, {0, 1, 0}, size * 0.8f, c);
    }
}

} // namespace sb
