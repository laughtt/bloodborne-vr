// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "physics_weapon.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <ctime>
#include <map>

#include "rig.h"

namespace sb {

using namespace m3;
using col::Capsule;
using col::Pose;

namespace {

constexpr const char* kEntityNames[kEntityCount] = {"floor", "post", "wall", "enemy"};

std::string Fmt(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

std::string JsonText(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string JsonPoint(Vec3 p) {
    return Fmt("[%.3f, %.3f, %.3f]", p.x, p.y, p.z);
}

const char* HandName(int mask) {
    return mask == 3 ? "LR" : mask == 2 ? "R" : "L";
}

float CapsuleDistance(Vec3 p, const Capsule& c) {
    return Length(p - col::ClosestOnSegment(c.a, c.b, p)) - c.r;
}

/// One bone's vertices -> capsules: slices along the part's long axis (PCA); a flat wide slice
/// becomes several capsules side by side, so the radius follows the thickness.
void SliceGroup(const std::vector<Vec3>& pts, const PhysicsConfig& cfg, WeaponShape& out) {
    Vec3 c{};
    for (const Vec3& p : pts) {
        c = c + p;
    }
    c = c * (1.0f / float(pts.size()));
    col::Mat3 cov = col::Diagonal(0.0f);
    for (const Vec3& p : pts) {
        cov = cov + col::Outer(p - c, p - c);
    }
    cov = cov * (1.0f / float(pts.size()));
    float values[3];
    Vec3 axes[3];
    col::SymmetricEigen(cov, values, axes);
    const Vec3 e0 = axes[0];
    float smin = 1e30f, smax = -1e30f;
    for (const Vec3& p : pts) {
        const float s = Dot(p - c, e0);
        smin = std::min(smin, s);
        smax = std::max(smax, s);
    }
    const float length = smax - smin;
    const int n = std::clamp(int(std::ceil(length / cfg.slice_m - 0.3f)), 1, 12);
    for (int k = 0; k < n; ++k) {
        const float lo = smin + length * float(k) / float(n);
        const float hi = smin + length * float(k + 1) / float(n);
        float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
        int count = 0;
        for (const Vec3& p : pts) {
            const Vec3 d = p - c;
            const float s = Dot(d, e0);
            if (s < lo - 1e-4f || s > hi + 1e-4f) {
                continue;
            }
            const float u = Dot(d, axes[1]), v = Dot(d, axes[2]);
            umin = std::min(umin, u);
            umax = std::max(umax, u);
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v);
            ++count;
        }
        if (count < 2) {
            continue;
        }
        Vec3 wide = axes[1], thin = axes[2];
        float w = umax - umin, t = vmax - vmin, wlo = umin, tc = 0.5f * (vmin + vmax);
        if (t > w) {
            std::swap(wide, thin);
            std::swap(w, t);
            wlo = vmin;
            tc = 0.5f * (umin + umax);
        }
        const int lanes = std::clamp(
            int(std::ceil(w / std::max(t, 2.0f * cfg.radius_min_m) - 0.05f)), 1, cfg.max_lanes);
        float r = std::max(0.5f * t, w / (2.0f * float(lanes))) * cfg.radius_scale;
        r = std::clamp(r, cfg.radius_min_m, cfg.radius_max_m);
        const float mid = 0.5f * (lo + hi);
        const float s0 = k == 0 ? std::min(lo + r, mid) : lo;
        const float s1 = k == n - 1 ? std::max(hi - r, mid) : hi;
        for (int j = 0; j < lanes; ++j) {
            float off = wlo + 0.5f * w;
            if (lanes > 1) {
                const float edge = std::min(r, 0.5f * w);
                off = wlo + edge + (w - 2.0f * edge) * float(j) / float(lanes - 1);
            }
            const Vec3 base = c + wide * off + thin * tc;
            out.capsules.push_back({base + e0 * s0, base + e0 * s1, r});
        }
    }
}

col::Mat3 ParallelAxis(float mass, Vec3 d) {
    return (col::Diagonal(Dot(d, d)) + col::Outer(d, d) * -1.0f) * mass;
}

std::string ShapeKey(const PhysicsConfig& c) {
    return Fmt("%g/%g/%g/%g/%d/%g/%g/%g", c.slice_m, c.radius_min_m, c.radius_max_m,
               c.radius_scale, c.max_lanes, c.hand_mass_kg, c.weapon_mass_kg, c.hand_gyration_m);
}

} // namespace

WeaponShape BuildWeaponShape(const Model& model, const PhysicsConfig& cfg) {
    WeaponShape out;
    std::map<int, std::vector<Vec3>> groups;
    for (const Vertex& v : model.vertices) {
        int k = 0;
        for (int j = 1; j < 4; ++j) {
            if (v.weights[j] > v.weights[k]) {
                k = j;
            }
        }
        groups[int(v.bones[k])].push_back({v.position[0], v.position[1], v.position[2]});
    }
    std::vector<const std::vector<Vec3>*> order;
    for (const auto& g : groups) {
        order.push_back(&g.second);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const auto* a, const auto* b) { return a->size() > b->size(); });
    const size_t small = std::max<size_t>(24, model.vertices.size() / 40);
    for (const auto* pts : order) {
        if (out.capsules.size() >= 64 || pts->size() < 4) {
            continue;
        }
        if (pts->size() < small && !out.capsules.empty()) {
            // A small part (trigger, lever): only when the big parts' capsules miss it.
            size_t covered = 0;
            for (const Vec3& p : *pts) {
                for (const Capsule& c : out.capsules) {
                    if (CapsuleDistance(p, c) <= 0.004f) {
                        ++covered;
                        break;
                    }
                }
            }
            if (covered * 10 >= pts->size() * 9) {
                continue;
            }
        }
        SliceGroup(*pts, cfg, out);
    }
    if (out.capsules.size() > 64) {
        out.capsules.resize(64);
    }
    if (out.capsules.empty()) {
        out.capsules.push_back({{0, 0, 0}, {0, 0, 0}, 0.03f}); // no mesh: a ball at the grip
    }
    col::Aabb box;
    std::vector<float> radii;
    for (const Capsule& c : out.capsules) {
        box.Add(c.a, c.r);
        box.Add(c.b, c.r);
        radii.push_back(c.r);
        for (const Vec3 e : {c.a, c.b}) {
            if (Length(e) + c.r > out.reach) {
                out.reach = Length(e) + c.r;
                out.tip = e;
            }
        }
    }
    out.center = (box.lo + box.hi) * 0.5f;
    for (const Capsule& c : out.capsules) {
        out.bound = std::max(out.bound, std::max(Length(c.a - out.center),
                                                 Length(c.b - out.center)) + c.r);
    }
    std::sort(radii.begin(), radii.end());
    out.min_radius = radii.front();
    out.median_radius = radii[radii.size() / 2];

    // Mass: the hand at the grip (turning like a mass hand_gyration_m out: the forearm), the
    // weapon spread over the capsules by volume.
    const float mh = cfg.hand_mass_kg, mw = cfg.weapon_mass_kg;
    std::vector<float> volume;
    float total = 0.0f;
    for (const Capsule& c : out.capsules) {
        const float v = kPi * c.r * c.r * Length(c.b - c.a) + 4.0f / 3.0f * kPi * c.r * c.r * c.r;
        volume.push_back(v);
        total += v;
    }
    out.mass = mh + mw;
    Vec3 moment{};
    for (size_t k = 0; k < out.capsules.size(); ++k) {
        const Capsule& c = out.capsules[k];
        moment = moment + (c.a + c.b) * (0.5f * mw * volume[k] / total);
    }
    out.com = moment * (1.0f / out.mass);
    const float gyration = cfg.hand_gyration_m;
    col::Mat3 inertia =
        col::Diagonal(mh * gyration * gyration) + ParallelAxis(mh, -out.com);
    for (size_t k = 0; k < out.capsules.size(); ++k) {
        const Capsule& c = out.capsules[k];
        const float m = mw * volume[k] / total;
        const float len = Length(c.b - c.a);
        const Vec3 u = Normalize(c.b - c.a, {0, 1, 0});
        const float axial = 0.5f * m * c.r * c.r;
        const float across = m * (3.0f * c.r * c.r + (len + 2.0f * c.r) * (len + 2.0f * c.r)) / 12.0f;
        inertia = inertia + col::Diagonal(across) + col::Outer(u, u) * (axial - across) +
                  ParallelAxis(m, (c.a + c.b) * 0.5f - out.com);
    }
    out.inv_inertia = col::Inverse(inertia);
    return out;
}

std::vector<HitShape> AttackShapesFor(const Hitboxes& hitboxes, const std::string& form, int pick,
                                      std::string* name,
                                      const std::function<Vec3(const ShapePoint&)>& body_point) {
    std::vector<size_t> attacks;
    for (size_t a = 0; a < hitboxes.attacks.size(); ++a) {
        const std::string& f = hitboxes.attacks[a].form;
        if (form.empty() || f.empty() || f == "A/B" || f == form) {
            attacks.push_back(a);
        }
    }
    std::vector<HitShape> out;
    for (size_t k = 0; k < attacks.size(); ++k) {
        if (pick >= 0 && size_t(pick) != k) {
            continue;
        }
        for (const HitShape& source : hitboxes.attacks[attacks[k]].hits) {
            HitShape hit = source;
            if (hit.p[0].on_body || hit.p[1].on_body) {
                if (!body_point) {
                    continue; // body-sourced (kicks): no body sweep here
                }
                // Where the scene draws it: the bone's pose now, in the weapon's space.
                for (ShapePoint& p : hit.p) {
                    if (p.on_body) {
                        p.pos = body_point(p);
                        p.on_body = false;
                    }
                }
            }
            const bool seen = std::any_of(out.begin(), out.end(), [&](const HitShape& s) {
                return Length(s.p[0].pos - hit.p[0].pos) < 1e-3f &&
                       Length(s.p[1].pos - hit.p[1].pos) < 1e-3f &&
                       std::fabs(s.radius - hit.radius) < 1e-3f;
            });
            if (!seen) {
                out.push_back(hit);
            }
        }
    }
    if (name) {
        *name = pick >= 0 && size_t(pick) < attacks.size() ? hitboxes.attacks[attacks[size_t(pick)]].name
                                                           : std::string("all attacks");
    }
    return out;
}

// ---- WeaponPhysics ----------------------------------------------------------------------------

Vec3 WeaponPhysics::SwingProbe(int side, float dt, Vec3& velocity) const {
    const Hand& h = hands[side];
    Vec3 best = h.shape.tip;
    velocity = {};
    float fastest = -1.0f;
    const auto consider = [&](Vec3 p) {
        const Vec3 v = (col::Apply(h.target, p) - col::Apply(h.target_prev, p)) * (1.0f / dt);
        const float speed = Length(v);
        if (speed > fastest) {
            fastest = speed;
            best = p;
            velocity = v;
        }
    };
    consider(h.shape.tip);
    for (const Capsule& a : h.attack) {
        consider((a.a + a.b) * 0.5f);
    }
    return best;
}

void WeaponPhysics::ResetSwing(int side) {
    Hand& h = hands[side];
    h.hit_this_swing = h.overlap = h.swinging = h.have_swing_dir = false;
    h.slow_time = 0.0f;
}

WeaponPhysics::~WeaponPhysics() {
    if (log_file) {
        std::fclose(log_file);
    }
}

void WeaponPhysics::Load(const std::filesystem::path& assets, const PhysicsConfig& config,
                         std::string& log) {
    assets_dir = assets;
    LoadEnemy(config.enemy, log);
}

void WeaponPhysics::LoadEnemy(const std::string& chr, std::string& log) {
    const int generation = enemy.generation + 1;
    enemy = Enemy{};
    enemy.generation = generation;
    enemy.chr = chr;
    enemy_fixed = false;
    if (chr.empty()) {
        log += "physics: no enemy\n";
        return;
    }
    std::string error;
    const auto path = assets_dir / ("enemy_" + chr + ".json");
    enemy.has_model = LoadModel(path, enemy.model, error);
    std::string hurt_log;
    const std::vector<Hurtbox> boxes = LoadHurtboxes(assets_dir, chr, hurt_log);
    for (Hurtbox box : boxes) {
        Vec3 a = box.a.pos, b = box.capsule ? box.b.pos : box.a.pos;
        if (enemy.has_model) {
            // On the bone at its bind pose (for c2632 the ragdoll and the FLVER bind agree).
            ResolveBone(enemy.model, box.a);
            ResolveBone(enemy.model, box.b);
            if (box.a.bone >= 0 && box.a.has_local) {
                a = TransformPoint(enemy.model.bones[size_t(box.a.bone)].bind_world, box.a.local);
            }
            if (box.capsule && box.b.bone >= 0 && box.b.has_local) {
                b = TransformPoint(enemy.model.bones[size_t(box.b.bone)].bind_world, box.b.local);
            }
        }
        enemy.parts.push_back({a, b, box.radius});
        enemy.part_names.push_back(box.a.bone_name.empty() ? box.name : box.a.bone_name);
        enemy.part_groups.push_back(box.part_group);
    }
    log += Fmt("physics: enemy %s: %s; %s\n", chr.c_str(),
               enemy.has_model
                   ? Fmt("%s, %zu vertices", path.filename().string().c_str(),
                         enemy.model.vertices.size())
                         .c_str()
                   : (error + " (run scripts/vr_assets/extract_enemy.py)").c_str(),
               hurt_log.c_str());
}

void WeaponPhysics::SetEnemyParts(const std::vector<Capsule>& parts, const Mat4& world) {
    const int generation = enemy.generation + 1;
    enemy = Enemy{};
    enemy.generation = generation;
    enemy.chr = "test";
    enemy.parts = parts;
    for (size_t i = 0; i < parts.size(); ++i) {
        enemy.part_names.push_back("part " + std::to_string(i));
        enemy.part_groups.push_back(0);
    }
    enemy.world = world;
    enemy_fixed = true;
}

void WeaponPhysics::PlaceProps(Vec3 head_position, Quat head_rotation) {
    anchor = {head_position.x, 0.0f, head_position.z};
    anchor_yaw = YawOf(head_rotation);
    anchored = true;
}

Vec3 WeaponPhysics::AnchorPoint(float x, float z) const {
    Vec3 p = anchor + Rotate(AxisAngle({0, 1, 0}, anchor_yaw), Vec3{x, 0.0f, z});
    p.y = floor_y;
    return p;
}

void WeaponPhysics::BuildWorld(const PhysicsConfig& cfg) {
    colliders.clear();
    props = Props{};
    if (cfg.floor) {
        Collider c;
        c.kind = Collider::kPlane;
        c.entity = kFloor;
        c.plane = {{0, 1, 0}, floor_y};
        c.bounds.lo = {-1e4f, -1e4f, -1e4f};
        c.bounds.hi = {1e4f, floor_y, 1e4f};
        colliders.push_back(c);
    }
    const auto add_capsule = [&](const Capsule& cap, int entity, int part) {
        Collider c;
        c.kind = Collider::kCapsule;
        c.entity = entity;
        c.part = part;
        c.capsule = cap;
        c.bounds = col::Bounds(cap);
        colliders.push_back(c);
    };
    if (cfg.props && anchored) {
        // The training post: a vertical pole and a crossbar facing where you stood.
        const Vec3 base = AnchorPoint(cfg.post[0], cfg.post[1]);
        const float r = std::max(cfg.post_radius, 0.01f);
        const float height = std::max(cfg.post_height, 2.0f * r);
        props.post = {base + Vec3{0, r, 0}, base + Vec3{0, height - r, 0}, r};
        add_capsule(props.post, kPost, 0);
        const Vec3 out = Normalize(Vec3{base.x - anchor.x, 0.0f, base.z - anchor.z},
                                   Rotate(AxisAngle({0, 1, 0}, anchor_yaw), {0, 0, -1}));
        const Vec3 across = Normalize(Cross({0, 1, 0}, out), {1, 0, 0});
        const Vec3 bar = base + Vec3{0, cfg.crossbar_height, 0};
        props.crossbar = {bar - across * (0.5f * cfg.crossbar_length),
                          bar + across * (0.5f * cfg.crossbar_length), cfg.crossbar_radius};
        if (cfg.crossbar_length > 0.0f && cfg.crossbar_radius > 0.0f) {
            add_capsule(props.crossbar, kPost, 1);
        }
        // The wall: its face (toward you) at `wall`, standing on the floor.
        const Quat wq = AxisAngle({0, 1, 0}, anchor_yaw + cfg.wall_yaw_deg * kDeg);
        const Vec3 normal = Rotate(wq, {-1, 0, 0});
        props.wall.rotation = wq;
        props.wall.half = {std::max(cfg.wall_size.x, 0.02f) * 0.5f,
                           std::max(cfg.wall_size.y, 0.02f) * 0.5f,
                           std::max(cfg.wall_size.z, 0.02f) * 0.5f};
        props.wall.center = AnchorPoint(cfg.wall[0], cfg.wall[1]) - normal * props.wall.half.x +
                            Vec3{0, props.wall.half.y, 0};
        Collider wall;
        wall.kind = Collider::kBox;
        wall.entity = kWall;
        wall.box = props.wall;
        wall.bounds = col::Bounds(props.wall);
        colliders.push_back(wall);
        props.valid = true;
        if (!enemy_fixed) {
            const Vec3 pos = AnchorPoint(cfg.enemy_pos[0], cfg.enemy_pos[1]);
            const float yaw = anchor_yaw + (cfg.enemy_face_user
                                                ? std::atan2(cfg.enemy_pos[0], cfg.enemy_pos[1])
                                                : cfg.enemy_yaw_deg * kDeg);
            enemy.world = Translate(pos) * FromQuat(AxisAngle({0, 1, 0}, yaw));
        }
    }
    enemy.placed = !enemy.parts.empty() && (enemy_fixed || (cfg.props && anchored));
    enemy_world_parts.clear();
    enemy_bounds = col::Aabb{};
    if (enemy.placed) {
        for (size_t i = 0; i < enemy.parts.size(); ++i) {
            const Capsule& p = enemy.parts[i];
            const Capsule w{TransformPoint(enemy.world, p.a), TransformPoint(enemy.world, p.b), p.r};
            enemy_world_parts.push_back(w);
            enemy_bounds.Add(w.a, w.r);
            enemy_bounds.Add(w.b, w.r);
            add_capsule(w, kEnemy, int(i));
        }
    }
}

void WeaponPhysics::CaptureGhost(const Rig& rig, const Model& body) {
    if (ghost_body != &body) {
        ghost_body = &body;
        for (int side = 0; side < 2; ++side) {
            const std::string p = side ? "R_" : "L_";
            int* b = ghost_bones[side];
            b[0] = rig.hand_bone[side];
            for (int f = 0; f < 5; ++f) {
                for (int j = 0; j < 3; ++j) {
                    b[1 + f * 3 + j] = body.FindBone(p + "Finger" + std::to_string(f) +
                                                     (j == 0 ? "" : std::to_string(j)));
                }
            }
        }
    }
    for (int side = 0; side < 2; ++side) {
        Hand& h = hands[side];
        h.ghost.clear();
        const int* b = ghost_bones[side];
        if (b[0] < 0) {
            continue;
        }
        const Vec3 wrist = Translation(rig.BoneWorld(b[0]));
        h.ghost_wrist = wrist;
        Vec3 knuckle[5];
        bool have[5] = {};
        for (int f = 0; f < 5; ++f) {
            Vec3 before = wrist, prev = wrist;
            int joints = 0;
            for (int j = 0; j < 3; ++j) {
                const int bone = b[1 + f * 3 + j];
                if (bone < 0) {
                    break;
                }
                const Vec3 p = Translation(rig.BoneWorld(bone));
                h.ghost.push_back({prev, p});
                if (j == 0) {
                    knuckle[f] = p;
                    have[f] = true;
                }
                before = prev;
                prev = p;
                ++joints;
            }
            if (joints >= 2) {
                h.ghost.push_back({prev, prev + (prev - before) * 0.7f}); // the fingertip
            }
        }
        for (int f = 1; f < 4; ++f) {
            if (have[f] && have[f + 1]) {
                h.ghost.push_back({knuckle[f], knuckle[f + 1]});
            }
        }
    }
}

Capsule WeaponPhysics::CapsuleWorld(int side, const Capsule& local) const {
    const Pose& p = hands[side].pose;
    return {col::Apply(p, local.a), col::Apply(p, local.b), local.r * p.s};
}

Capsule WeaponPhysics::CapsuleWorld(int side, size_t index) const {
    return CapsuleWorld(side, hands[side].shape.capsules[index]);
}

float WeaponPhysics::TurnLever(int side, Quat turn) const {
    const Hand& h = hands[side];
    const Vec3 axis = Normalize(Vec3{turn.x, turn.y, turn.z}, {0, 1, 0});
    float lever = 0.0f;
    for (const Capsule& c : h.shape.capsules) {
        for (const Vec3 e : {c.a, c.b}) {
            const Vec3 r = Rotate(h.pose.q, e * h.pose.s);
            lever = std::max(lever, Length(Cross(axis, r)) + c.r * h.pose.s);
        }
    }
    return std::max(lever, 0.01f);
}

float WeaponPhysics::InverseMassAt(int side, Vec3 point, Vec3 normal) const {
    const Hand& h = hands[side];
    const col::Mat3 r = col::RotationOf(h.pose.q);
    const col::Mat3 inv = r * h.shape.inv_inertia * col::Transpose(r) *
                          (1.0f / (h.pose.s * h.pose.s));
    const Vec3 arm = Cross(point - col::Apply(h.pose, h.shape.com), normal);
    return 1.0f / h.shape.mass + Dot(arm, inv * arm);
}

void WeaponPhysics::ApplyImpulse(int side, Vec3 point, Vec3 impulse) {
    Hand& h = hands[side];
    const float s = h.pose.s;
    const col::Mat3 r = col::RotationOf(h.pose.q);
    const col::Mat3 inv = r * h.shape.inv_inertia * col::Transpose(r) * (1.0f / (s * s));
    Vec3 c = col::Apply(h.pose, h.shape.com);
    const Vec3 arm = point - c;
    c = c + impulse * (1.0f / h.shape.mass);
    Vec3 w = inv * Cross(arm, impulse);
    const float angle = Length(w);
    if (angle > 0.5f) {
        w = w * (0.5f / angle);
    }
    const Quat q = h.pose.q;
    const Quat spin = Quat{w.x, w.y, w.z, 0.0f} * q;
    h.pose.q = Normalize(Quat{q.x + 0.5f * spin.x, q.y + 0.5f * spin.y, q.z + 0.5f * spin.z,
                              q.w + 0.5f * spin.w});
    h.pose.x = c - Rotate(h.pose.q, h.shape.com * s);
}

void WeaponPhysics::Solve(const PhysicsConfig& cfg, const Pose prev[2], const Pose from[2],
                          const Pose to[2], float h, float step) {
    candidates.clear();
    weapon_candidates.clear();
    bool live[2];
    col::Aabb sphere[2];
    for (int side = 0; side < 2; ++side) {
        const Hand& hand = hands[side];
        live[side] = hand.active && !hand.passthrough;
        if (!live[side]) {
            continue;
        }
        sphere[side].Add(col::Apply(hand.pose, hand.shape.center),
                         hand.shape.bound * hand.pose.s + step);
        for (size_t c = 0; c < colliders.size(); ++c) {
            const Collider& collider = colliders[c];
            if ((collider.entity == kEnemy && !EnemySolid(cfg, side)) ||
                !col::Overlap(sphere[side], collider.bounds)) {
                continue;
            }
            for (size_t i = 0; i < hand.shape.capsules.size(); ++i) {
                if (col::Overlap(col::Bounds(CapsuleWorld(side, i)), collider.bounds, step)) {
                    candidates.push_back({side, int(i), int(c)});
                }
            }
        }
    }
    if (cfg.weapons_collide && live[0] && live[1] && col::Overlap(sphere[0], sphere[1])) {
        for (size_t i = 0; i < hands[0].shape.capsules.size(); ++i) {
            const col::Aabb a = col::Bounds(CapsuleWorld(0, i));
            for (size_t j = 0; j < hands[1].shape.capsules.size(); ++j) {
                if (col::Overlap(a, col::Bounds(CapsuleWorld(1, j)), step)) {
                    weapon_candidates.push_back({int(i), int(j)});
                }
            }
        }
    }
    if (candidates.empty() && weapon_candidates.empty()) {
        return;
    }
    for (SubTouch& s : sub) {
        s = SubTouch{};
    }
    const auto record = [&](int pair, int side_a, int side_b, const col::Contact& c, int part,
                            int cap_a, int cap_b) {
        // How fast the hands drive these points together (the hand's swing, not the solver's
        // catch-up after a weapon comes free, nor its push while pressed).
        const Vec3 la = col::Unapply(hands[side_a].pose, c.point_a);
        Vec3 v = (col::Apply(to[side_a], la) - col::Apply(from[side_a], la)) * (1.0f / h);
        Vec3 lb{};
        if (side_b >= 0) {
            lb = col::Unapply(hands[side_b].pose, c.point_b);
            v = v - (col::Apply(to[side_b], lb) - col::Apply(from[side_b], lb)) * (1.0f / h);
        }
        Touch& t = touches[pair];
        if (!t.any) {
            t.part = part;
            t.point = c.point_a;
        }
        t.any = true;
        t.approach = std::max(t.approach, -Dot(v, c.normal));
        t.depth = std::max(t.depth, c.depth);
        SubTouch& s = sub[pair];
        s.any = true;
        s.local_a = la;
        s.local_b = lb;
        s.normal = c.normal;
        s.side_a = side_a;
        s.side_b = side_b;
        hands[side_a].touching[size_t(cap_a)] = 1;
        if (side_b >= 0) {
            hands[side_b].touching[size_t(cap_b)] = 1;
        }
    };
    const auto test = [&](const Collider& collider, const Capsule& cap) {
        switch (collider.kind) {
        case Collider::kCapsule:
            return col::CapsuleCapsule(cap, collider.capsule,
                                       (cap.a + cap.b - collider.capsule.a - collider.capsule.b) *
                                           0.5f);
        case Collider::kBox:
            return col::CapsuleBox(cap, collider.box);
        default:
            return col::CapsulePlane(cap, collider.plane);
        }
    };
    const auto weapons_hint = [&] {
        return col::Apply(hands[0].pose, hands[0].shape.center) -
               col::Apply(hands[1].pose, hands[1].shape.center);
    };
    for (int it = 0; it < cfg.iterations; ++it) {
        bool moved = false;
        for (const Candidate& cand : candidates) {
            const Collider& collider = colliders[size_t(cand.collider)];
            const col::Contact c = test(collider, CapsuleWorld(cand.side, size_t(cand.capsule)));
            if (c.depth <= 1e-5f) {
                continue;
            }
            const float w = InverseMassAt(cand.side, c.point_a, c.normal);
            record(cand.side * kEntityCount + collider.entity, cand.side, -1, c, collider.part,
                   cand.capsule, -1);
            ApplyImpulse(cand.side, c.point_a, c.normal * (std::min(c.depth, 0.1f) / w));
            moved = true;
        }
        for (const auto& [i, j] : weapon_candidates) {
            const col::Contact c = col::CapsuleCapsule(CapsuleWorld(0, size_t(i)),
                                                       CapsuleWorld(1, size_t(j)), weapons_hint());
            if (c.depth <= 1e-5f) {
                continue;
            }
            const float wa = InverseMassAt(0, c.point_a, c.normal);
            const float wb = InverseMassAt(1, c.point_b, -c.normal);
            const float lambda = std::min(c.depth, 0.1f) / (wa + wb);
            record(kPairWeapons, 0, 1, c, -1, i, j);
            ApplyImpulse(0, c.point_a, c.normal * lambda);
            ApplyImpulse(1, c.point_b, c.normal * -lambda);
            moved = true;
        }
        if (!moved) {
            break;
        }
    }
    // What is left (tests), and how fast the contact points slid this substep.
    for (const Candidate& cand : candidates) {
        const col::Contact c =
            test(colliders[size_t(cand.collider)], CapsuleWorld(cand.side, size_t(cand.capsule)));
        residual_depth = std::max(residual_depth, c.depth);
    }
    for (const auto& [i, j] : weapon_candidates) {
        const col::Contact c = col::CapsuleCapsule(CapsuleWorld(0, size_t(i)),
                                                   CapsuleWorld(1, size_t(j)), weapons_hint());
        residual_depth = std::max(residual_depth, c.depth);
    }
    for (int pair = 0; pair < kPairCount; ++pair) {
        const SubTouch& s = sub[pair];
        if (!s.any) {
            continue;
        }
        Vec3 v = (col::Apply(hands[s.side_a].pose, s.local_a) -
                  col::Apply(prev[s.side_a], s.local_a)) *
                 (1.0f / h);
        if (s.side_b >= 0) {
            v = v - (col::Apply(hands[s.side_b].pose, s.local_b) -
                     col::Apply(prev[s.side_b], s.local_b)) *
                        (1.0f / h);
        }
        touches[pair].slide_sum += Length(Perp(v, s.normal)) * h;
        touches[pair].slide_time += h;
    }
}

bool WeaponPhysics::EnemySolid(const PhysicsConfig& cfg, int side) const {
    return cfg.enemy_solid && !(cfg.swing_through_enemy && hands[side].swinging);
}

bool WeaponPhysics::Overlaps(int side, const PhysicsConfig& cfg) const {
    const Hand& h = hands[side];
    const Hand& other = hands[1 - side];
    for (size_t i = 0; i < h.shape.capsules.size(); ++i) {
        const Capsule cap = CapsuleWorld(side, i);
        const col::Aabb box = col::Bounds(cap);
        for (const Collider& c : colliders) {
            if ((c.entity == kEnemy && !EnemySolid(cfg, side)) || !col::Overlap(box, c.bounds)) {
                continue;
            }
            const col::Contact contact = c.kind == Collider::kCapsule ? col::CapsuleCapsule(cap, c.capsule)
                                         : c.kind == Collider::kBox   ? col::CapsuleBox(cap, c.box)
                                                                      : col::CapsulePlane(cap, c.plane);
            if (contact.depth > 0.001f) {
                return true;
            }
        }
        if (cfg.weapons_collide && other.active && !other.passthrough) {
            for (size_t j = 0; j < other.shape.capsules.size(); ++j) {
                if (col::CapsuleCapsule(cap, CapsuleWorld(1 - side, j)).depth > 0.001f) {
                    return true;
                }
            }
        }
    }
    return false;
}

void WeaponPhysics::TestHits(const PhysicsConfig& cfg, const Pose from[2], const Pose to[2],
                             float h, PhysicsEvents& events) {
    if (!cfg.hits || !enemy.placed || enemy_world_parts.empty()) {
        return;
    }
    for (int side = 0; side < 2; ++side) {
        Hand& hand = hands[side];
        if (!hand.active || hand.attack.empty()) {
            hand.overlap = false;
            continue;
        }
        bool overlap = false;
        int part = -1, shape = -1;
        float best = -1e9f;
        Vec3 point;
        // Against where the hand puts the weapon, not where a solid blocker holds it: a blocked
        // weapon must not be able to prevent the attack overlap.
        const Pose& at = to[side];
        for (size_t a = 0; a < hand.attack.size(); ++a) {
            const Capsule& local = hand.attack[a];
            const Capsule cap{col::Apply(at, local.a), col::Apply(at, local.b), local.r * at.s};
            if (!col::Overlap(col::Bounds(cap), enemy_bounds)) {
                continue;
            }
            for (size_t p = 0; p < enemy_world_parts.size(); ++p) {
                const col::Contact c = col::CapsuleCapsule(cap, enemy_world_parts[p]);
                if (c.hit && c.depth > best) {
                    best = c.depth;
                    overlap = true;
                    part = int(p);
                    shape = int(a);
                    // The surface point of the hurtbox facing the blade.
                    point = c.point_b;
                }
            }
        }
        if (overlap && !hand.overlap && !hand.hit_this_swing) {
            const Capsule& s = hand.attack[size_t(shape)];
            const Vec3 center = (s.a + s.b) * 0.5f;
            const float speed =
                Length(col::Apply(to[side], center) - col::Apply(from[side], center)) / h;
            if (speed >= cfg.hit_speed) {
                hand.hit_this_swing = true;
                ++hit_count;
                ++stats_hits;
                markers.push_back({point, now, part, side});
                Request(side, 3, cfg.hit_amplitude, cfg.hit_seconds);
                const std::string part_name = enemy.part_names[size_t(part)];
                Emit(cfg, events,
                     Fmt("HIT %s %s (%s, r %.2f m) -> enemy %s %s at %.2f m/s; haptic %s %.2f x "
                         "%.3f s",
                         side ? "R" : "L", hand.name.c_str(), hand.attack_name.c_str(),
                         s.r * at.s, enemy.chr.c_str(), part_name.c_str(), speed,
                         side ? "R" : "L", cfg.hit_amplitude, cfg.hit_seconds),
                     Fmt("{\"t\": %.3f, \"event\": \"hit\", \"hand\": \"%s\", \"weapon\": %s, "
                         "\"attack\": %s, \"enemy\": %s, \"part\": %s, \"speed\": %.3f, "
                         "\"amp\": %.3f, \"dur\": %.3f, \"point\": %s}",
                         now, side ? "R" : "L", JsonText(hand.name).c_str(),
                         JsonText(hand.attack_name).c_str(), JsonText(enemy.chr).c_str(),
                         JsonText(part_name).c_str(), speed, cfg.hit_amplitude, cfg.hit_seconds,
                         JsonPoint(point).c_str()));
            }
        }
        hand.overlap = overlap;
    }
}

void WeaponPhysics::Request(int side, int priority, float amplitude, float seconds) {
    HapticRequest& r = requests[side];
    if (priority > r.priority || (priority == r.priority && amplitude > r.amplitude)) {
        r = {priority, amplitude, seconds};
    }
}

std::string WeaponPhysics::PairName(int pair) const {
    if (pair == kPairWeapons) {
        return "L " + hands[0].name + " | R " + hands[1].name;
    }
    const int side = pair / kEntityCount, entity = pair % kEntityCount;
    std::string other = kEntityNames[entity];
    if (entity == kEnemy) {
        other += " " + enemy.chr;
        const int part = pairs[pair].part;
        if (part >= 0 && size_t(part) < enemy.part_names.size()) {
            other += " " + enemy.part_names[size_t(part)];
        }
    } else if (entity == kPost && pairs[pair].part == 1) {
        other = "crossbar";
    }
    return std::string(side ? "R " : "L ") + hands[side].name + " | " + other;
}

void WeaponPhysics::Emit(const PhysicsConfig& cfg, PhysicsEvents& events, const std::string& line,
                         const std::string& json) {
    if (cfg.log_contacts) {
        events.log += line + "\n";
    }
    if (cfg.contact_log.empty()) {
        return;
    }
    if (!log_file || log_path != cfg.contact_log) {
        if (log_file) {
            std::fclose(log_file);
        }
        log_path = cfg.contact_log;
        log_file = std::fopen(log_path.c_str(), "ab");
        if (log_file) {
            char date[64];
            const std::time_t t = std::time(nullptr);
            std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            std::fprintf(log_file, "{\"t\": %.3f, \"event\": \"session\", \"date\": \"%s\"}\n",
                         now, date);
            events.log += "physics: contact log " + log_path + "\n";
        } else {
            events.log += "physics: cannot write the contact log " + log_path + "\n";
        }
    }
    if (log_file) {
        std::fputs((json + "\n").c_str(), log_file);
        std::fflush(log_file);
    }
}

void WeaponPhysics::EndContacts(int side, const PhysicsConfig& cfg, PhysicsEvents& events,
                                bool quiet) {
    for (int pair = 0; pair < kPairCount; ++pair) {
        const bool mine = pair == kPairWeapons || pair / kEntityCount == side;
        if (!mine || !pairs[pair].active) {
            continue;
        }
        if (!quiet) {
            Emit(cfg, events,
                 Fmt("contact end %s (hand let go)", PairName(pair).c_str()),
                 Fmt("{\"t\": %.3f, \"event\": \"contact_end\", \"pair\": %s, \"duration\": %.3f, "
                     "\"reason\": \"released\"}",
                     now, JsonText(PairName(pair)).c_str(), now - pairs[pair].start));
        }
        pairs[pair].active = false;
    }
}

void WeaponPhysics::UpdateContacts(const PhysicsConfig& cfg, PhysicsEvents& events) {
    for (int i = 0; i < kPairCount; ++i) {
        const Touch& t = touches[i];
        Pair& p = pairs[i];
        const int mask = i == kPairWeapons ? 3 : 1 << (i / kEntityCount);
        if (t.any) {
            if (!p.active) {
                p = Pair{};
                p.active = true;
                p.start = now;
                p.part = t.part;
                p.point = t.point;
                p.max_speed = t.approach;
                ++stats_contacts;
                float amplitude = 0.0f, seconds = 0.0f;
                if (t.approach >= cfg.impact_min_speed) {
                    amplitude = std::clamp(cfg.impact_amplitude_min +
                                               cfg.impact_amplitude_per_mps * t.approach,
                                           cfg.impact_amplitude_min, cfg.impact_amplitude_max);
                    seconds = std::clamp(cfg.impact_seconds + cfg.impact_seconds_per_mps * t.approach,
                                         cfg.impact_seconds,
                                         std::max(cfg.impact_seconds_max, cfg.impact_seconds));
                    for (int side = 0; side < 2; ++side) {
                        if (mask & (1 << side)) {
                            Request(side, 2, amplitude, seconds);
                        }
                    }
                }
                Emit(cfg, events,
                     Fmt("contact %s: %.2f m/s, depth %.1f mm; haptic %s %.2f x %.3f s",
                         PairName(i).c_str(), t.approach, t.depth * 1000.0f, HandName(mask),
                         amplitude, seconds),
                     Fmt("{\"t\": %.3f, \"event\": \"contact\", \"pair\": %s, \"hands\": \"%s\", "
                         "\"speed\": %.3f, \"depth\": %.4f, \"amp\": %.3f, \"dur\": %.3f, "
                         "\"point\": %s}",
                         now, JsonText(PairName(i)).c_str(), HandName(mask), t.approach, t.depth,
                         amplitude, seconds, JsonPoint(t.point).c_str()));
            }
            p.last = now;
            p.max_depth = std::max(p.max_depth, t.depth);
            const float slide = t.slide_time > 0.0f ? t.slide_sum / t.slide_time : 0.0f;
            p.max_slide = std::max(p.max_slide, slide);
            if (slide >= cfg.slide_min_speed) {
                const float amplitude =
                    std::clamp(cfg.slide_amplitude_min + cfg.slide_amplitude_per_mps * slide,
                               cfg.slide_amplitude_min, cfg.slide_amplitude_max);
                p.max_buzz = std::max(p.max_buzz, amplitude);
                for (int side = 0; side < 2; ++side) {
                    if (mask & (1 << side)) {
                        Request(side, 1, amplitude, cfg.buzz_seconds);
                    }
                }
            }
        } else if (p.active && now - p.last > cfg.release_s) {
            p.active = false;
            Emit(cfg, events,
                 Fmt("contact end %s: %.2f s, slide up to %.2f m/s (buzz %.2f), depth up to %.1f "
                     "mm",
                     PairName(i).c_str(), p.last - p.start, p.max_slide, p.max_buzz,
                     p.max_depth * 1000.0f),
                 Fmt("{\"t\": %.3f, \"event\": \"contact_end\", \"pair\": %s, \"duration\": %.3f, "
                     "\"max_slide\": %.3f, \"max_buzz\": %.3f, \"max_depth\": %.4f}",
                     now, JsonText(PairName(i)).c_str(), p.last - p.start, p.max_slide,
                     p.max_buzz, p.max_depth));
        }
    }
}

void WeaponPhysics::Step(const StepInput& in, const PhysicsConfig& cfg, PhysicsEvents& events) {
    const auto clock_start = std::chrono::steady_clock::now();
    if (have_time && in.time < last_time - 1e-6) {
        Reset(); // the clock went back (another run of scripted frames): start over
    }
    if (!cfg.enabled) {
        for (Pair& p : pairs) {
            p = Pair{}; // switched off: no contacts linger
        }
    }
    float dt = 1.0f / 72.0f;
    if (have_time && in.time > last_time) {
        dt = std::clamp(float(in.time - last_time), 1.0f / 1000.0f, 1.0f / 15.0f);
    }
    now = in.time;
    last_time = in.time;
    have_time = true;
    floor_y = in.floor_y;
    simulating = cfg.enabled;
    for (HapticRequest& r : requests) {
        r = HapticRequest{};
    }
    for (Touch& t : touches) {
        t = Touch{};
    }
    if (!anchored && in.head_valid) {
        PlaceProps(in.head_position, in.head_rotation);
    }
    if (!enemy_fixed && cfg.enemy != enemy.chr && !assets_dir.empty()) {
        LoadEnemy(cfg.enemy, events.log);
    }
    BuildWorld(cfg);
    if (in.rig && in.body) {
        CaptureGhost(*in.rig, *in.body);
    }
    markers.erase(std::remove_if(markers.begin(), markers.end(),
                                 [&](const Marker& m) { return now - m.time > cfg.marker_s; }),
                  markers.end());

    // The hands: weapon changes, targets, teleports; how far the weapons must travel.
    const std::string key = ShapeKey(cfg);
    float min_radius = 1e9f, motion = 0.0f, gap = 0.0f;
    float reach[2] = {};
    for (int side = 0; side < 2; ++side) {
        Hand& h = hands[side];
        const HeldWeapon& w = in.held[side];
        if (!w.model) {
            if (h.active) {
                EndContacts(side, cfg, events, false);
            }
            h.active = false;
            h.model = nullptr;
            h.have_pose = h.passthrough = h.have_raw = false;
            ResetSwing(side);
            h.separation = h.separation_deg = h.separation_arc = 0.0f;
            continue;
        }
        if (w.model != h.model || key != h.shape_key) {
            if (h.active) {
                EndContacts(side, cfg, events, true);
            }
            h.shape = BuildWeaponShape(*w.model, cfg);
            h.shape_key = key;
            h.model = w.model;
            h.name = w.name;
            h.have_pose = h.passthrough = h.have_raw = false;
            ResetSwing(side); // a new weapon starts with no swing, hit or overlap
            events.log += Fmt("physics: %s %s: %zu collision capsules (radius %.1f..%.1f cm, "
                              "median %.1f), reach %.2f m (model units)\n",
                              side ? "R" : "L", w.name.c_str(), h.shape.capsules.size(),
                              h.shape.min_radius * 100.0f,
                              std::max_element(h.shape.capsules.begin(), h.shape.capsules.end(),
                                               [](const Capsule& a, const Capsule& b) {
                                                   return a.r < b.r;
                                               })->r * 100.0f,
                              h.shape.median_radius * 100.0f, h.shape.reach);
        }
        h.active = true;
        h.touching.assign(h.shape.capsules.size(), 0);
        h.attack.clear();
        for (const HitShape& s : w.attack) {
            h.attack.push_back({s.p[0].pos, s.capsule ? s.p[1].pos : s.p[0].pos,
                                s.radius * cfg.attack_radius_scale});
        }
        h.attack_name = w.attack_name;
        h.target = col::PoseOf(w.target);
        if (!h.have_pose) {
            h.pose = h.target_prev = h.target;
            h.have_pose = true;
        } else {
            // A tracking glitch is judged on the raw controller (the IK-clamped hand can hide
            // it); without one, on the target. The distance matches swing_logic.h's glitch_m.
            float jump = Length(h.target.x - h.target_prev.x);
            float turned = col::AngleBetween(h.target_prev.q, h.target.q) / kDeg;
            if (in.controller_valid[side] && h.have_raw) {
                jump = Length(in.controller_position[side] - h.raw_position);
                turned = col::AngleBetween(h.raw_rotation, in.controller_rotation[side]) / kDeg;
            }
            if (jump > cfg.teleport_m || turned > cfg.teleport_deg) {
                EndContacts(side, cfg, events, true);
                events.log += Fmt("physics: %s hand jumped %.2f m / %.0f deg (tracking): weapon "
                                  "reset\n",
                                  side ? "R" : "L", jump, turned);
                h.pose = h.target_prev = h.target;
                h.passthrough = false;
                ResetSwing(side);
            }
        }
        h.have_raw = in.controller_valid[side];
        h.raw_position = in.controller_position[side];
        h.raw_rotation = in.controller_rotation[side];
        h.pose.s = h.target_prev.s = h.target.s;
        {
            // The swing probe: the fastest attack shape (else the tip) as the hand drives it.
            Vec3 velocity;
            SwingProbe(side, dt, velocity);
            h.swing_speed = Length(velocity);
            if (h.swing_speed >= cfg.hit_speed) {
                h.swinging = true;
            } else if (h.swing_speed < cfg.rearm_speed) {
                h.swinging = false;
            }
            if (h.swing_speed >= cfg.rearm_speed) {
                const Vec3 dir = velocity * (1.0f / h.swing_speed);
                if (cfg.rearm_reverse_deg > 0.0f && h.have_swing_dir &&
                    Dot(dir, h.swing_dir) < std::cos(cfg.rearm_reverse_deg * kDeg)) {
                    // The stroke turned back (forehand, then backhand): a new swing, at once.
                    h.hit_this_swing = false;
                    h.overlap = false;
                    h.slow_time = 0.0f;
                }
                h.swing_dir = dir;
                h.have_swing_dir = true;
            }
        }
        float r = h.shape.reach;
        for (const Capsule& a : h.attack) {
            r = std::max(r, std::max(Length(a.a), Length(a.b)) + a.r);
        }
        reach[side] = r * h.target.s;
        min_radius = std::min(min_radius, h.shape.min_radius * h.target.s);
        // The grip's way and the farthest point's way from turning, each spent separately: how
        // far the hand moved this frame, and how far the weapon is from the hand (held back).
        motion = std::max({motion, Length(h.target.x - h.target_prev.x),
                           col::AngleBetween(h.target_prev.q, h.target.q) * reach[side]});
        gap = std::max({gap, Length(h.target.x - h.pose.x),
                        col::AngleBetween(h.pose.q, h.target.q) * reach[side]});
    }

    // Substeps: no weapon point moves more than `step` in one (half by moving the grip, half by
    // turning), so nothing thinner than the capsules can be skipped. The two budgets are
    // separate: a blade held back by a post still lets the grip follow the hand.
    const float step = cfg.max_step_m > 0.0f ? cfg.max_step_m : std::max(0.8f * min_radius, 0.004f);
    const float half = 0.5f * step;
    // Enough substeps for the hand's own motion, plus up to 16 to close a gap (a weapon pressed
    // into a wall needs no more; one that comes free catches up at ~5 m/s, not in one frame).
    const float travel = std::max(motion, std::min(gap, motion + 16.0f * half));
    const int substeps = std::clamp(int(std::ceil(travel / half)), 1, cfg.max_substeps);
    last_substeps = substeps;
    const float h = dt / float(substeps);
    const float follow =
        cfg.stiffness >= 1.0f ? 1.0f : 1.0f - std::pow(1.0f - cfg.stiffness, h * 90.0f);
    residual_depth = 0.0f;
    for (int k = 0; k < substeps; ++k) {
        const float frac = float(k + 1) / float(substeps);
        Pose prev[2], from[2], to[2];
        for (int side = 0; side < 2; ++side) {
            Hand& hand = hands[side];
            prev[side] = from[side] = to[side] = hand.pose;
            if (!hand.active) {
                continue;
            }
            const Pose goal = col::Interpolate(hand.target_prev, hand.target, frac);
            from[side] = col::Interpolate(hand.target_prev, hand.target, float(k) / float(substeps));
            to[side] = goal;
            if (!cfg.enabled || hand.passthrough) {
                hand.pose = goal;
            } else {
                float fx = follow, fq = follow;
                const float move = fx * Length(goal.x - hand.pose.x);
                if (move > half) {
                    fx *= half / move;
                }
                // The turn's budget uses the lever of this very turn (a roll about the handle
                // moves the blade less than its reach).
                const float turn = fq * col::AngleBetween(hand.pose.q, goal.q) *
                                   TurnLever(side, goal.q * Conjugate(hand.pose.q));
                if (turn > half) {
                    fq *= half / turn;
                }
                hand.pose.x = Lerp(hand.pose.x, goal.x, fx);
                hand.pose.q = Slerp(hand.pose.q, goal.q, fq);
                hand.pose.s = goal.s;
            }
        }
        if (cfg.enabled) {
            Solve(cfg, prev, from, to, h, step);
        }
        TestHits(cfg, from, to, h, events);
    }

    // Let go when the hand is too far from its weapon; swings; contacts; haptics.
    for (int side = 0; side < 2; ++side) {
        Hand& hand = hands[side];
        if (!hand.active) {
            continue;
        }
        hand.separation = Length(hand.pose.x - hand.target.x);
        hand.separation_deg = col::AngleBetween(hand.pose.q, hand.target.q) / kDeg;
        // The turn as an arc: the angle times the weapon's reach (a long blade lets go sooner).
        hand.separation_arc = hand.separation_deg * kDeg * reach[side];
        if (cfg.enabled && !hand.passthrough &&
            (hand.separation > cfg.max_separation_m ||
             hand.separation_arc > cfg.max_separation_arc_m)) {
            Emit(cfg, events,
                 Fmt("%s %s: the hand is %.2f m / %.0f deg (%.2f m of arc) from the blocked "
                     "weapon: it snaps back into the hand and passes through until free",
                     side ? "R" : "L", hand.name.c_str(), hand.separation, hand.separation_deg,
                     hand.separation_arc),
                 Fmt("{\"t\": %.3f, \"event\": \"snap\", \"hand\": \"%s\", \"weapon\": %s, "
                     "\"separation\": %.3f, \"angle_deg\": %.1f, \"arc\": %.3f}",
                     now, side ? "R" : "L", JsonText(hand.name).c_str(), hand.separation,
                     hand.separation_deg, hand.separation_arc));
            hand.pose = hand.target;
            hand.passthrough = true;
            hand.separation = hand.separation_deg = hand.separation_arc = 0.0f;
        }
        if (hand.passthrough && !Overlaps(side, cfg)) {
            hand.passthrough = false;
        }
        if (hand.swing_speed < cfg.rearm_speed) {
            hand.slow_time += dt;
            if (hand.slow_time >= 0.1f) {
                hand.hit_this_swing = false;
            }
        } else {
            hand.slow_time = 0.0f;
        }
        hand.target_prev = hand.target;
    }
    UpdateContacts(cfg, events);
    for (int side = 0; side < 2; ++side) {
        const HapticRequest& r = requests[side];
        Hand& hand = hands[side];
        if (r.priority >= 2) {
            events.haptic_amplitude[side] = r.amplitude;
            events.haptic_seconds[side] = r.seconds;
            hand.pulse_until = hand.next_buzz = now + r.seconds;
        } else if (r.priority == 1 && now >= hand.pulse_until && now >= hand.next_buzz) {
            events.haptic_amplitude[side] = r.amplitude;
            events.haptic_seconds[side] = r.seconds;
            hand.next_buzz = now + cfg.buzz_interval_s;
        }
    }

    // Cost and activity, every 5 s while a weapon is held.
    last_step_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                            clock_start)
                       .count();
    if (hands[0].active || hands[1].active) {
        if (stats_start < 0.0 || now < stats_start || now - stats_last > 1.0) {
            stats_start = now; // (re)start the window after a pause or a jump in time
            stats_ms = stats_ms_max = 0.0;
            stats_frames = stats_substeps = stats_contacts = stats_hits = 0;
        }
        stats_last = now;
        ++stats_frames;
        stats_ms += last_step_ms;
        stats_ms_max = std::max(stats_ms_max, double(last_step_ms));
        stats_substeps = std::max(stats_substeps, substeps);
        if (now - stats_start >= 5.0 && cfg.log_contacts) {
            events.log += Fmt("physics: %.1f s: step %.3f ms avg, %.3f max, up to %d substeps; %d "
                              "contacts, %d hits\n",
                              now - stats_start, stats_ms / std::max(stats_frames, 1),
                              stats_ms_max, stats_substeps, stats_contacts, stats_hits);
            stats_start = now;
            stats_ms = stats_ms_max = 0.0;
            stats_frames = stats_substeps = stats_contacts = stats_hits = 0;
        }
    }
}

bool WeaponPhysics::CorrectHands(RigInput& input) const {
    bool moved = false;
    for (int side = 0; side < 2; ++side) {
        const Hand& h = hands[side];
        HandInput& hand = input.hands[side];
        if (!h.active || !h.have_pose || !hand.valid) {
            continue;
        }
        if (Length(h.pose.x - h.target.x) < 1e-4f && col::AngleBetween(h.pose.q, h.target.q) < 1e-4f) {
            continue;
        }
        const Quat turn = Normalize(h.pose.q * Conjugate(h.target.q));
        hand.position = h.pose.x + Rotate(turn, hand.position - h.target.x);
        hand.rotation = Normalize(turn * hand.rotation);
        moved = true;
    }
    return moved;
}

Mat4 WeaponPhysics::WeaponPose(int side, const Mat4& fallback) const {
    const Hand& h = hands[side];
    return simulating && h.active && h.have_pose ? col::MatrixOf(h.pose) : fallback;
}

void WeaponPhysics::Reset() {
    for (Hand& h : hands) {
        h.have_pose = h.passthrough = h.overlap = h.hit_this_swing = false;
        h.swinging = h.have_swing_dir = h.have_raw = false;
        h.slow_time = h.swing_speed = h.separation = h.separation_deg = h.separation_arc = 0.0f;
        h.pulse_until = h.next_buzz = -1.0;
        h.ghost.clear();
    }
    for (Pair& p : pairs) {
        p = Pair{};
    }
    for (Touch& t : touches) {
        t = Touch{};
    }
    markers.clear();
    have_time = false;
}

} // namespace sb
