// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "throwables.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

#include "bbport_xr_bridge.h"

namespace sb {

using namespace m3;

namespace {

constexpr float kMaxSubstep = 0.004f; // s
constexpr float kMaxStepM = 0.02f;    // m of travel per collision test
constexpr float kLingerS = 3.0f;      // a stuck knife / pebble / coin stays this long
constexpr float kHoldTableSkipM = 0.15f;

bool Persistent(const ThrowItemDef& d) {
    return d.effect == "dust" || d.effect == "coin" || d.effect == "slash";
}

float RestHalfHeight(const ThrowItemDef& d) {
    if (d.shape == "bottle") return 0.11f;
    if (d.shape == "urn") return 0.10f;
    if (d.shape == "vial") return 0.07f;
    if (d.shape == "knife") return 0.012f;
    if (d.shape == "disc") return 0.012f;
    return 0.035f;
}

float Rand01(int seed, int k) {
    uint32_t x = uint32_t(seed) * 747796405u + uint32_t(k) * 2891336453u + 12345u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    x = (x >> 22u) ^ x;
    return float(x & 0xFFFFFFu) / float(0x1000000);
}

std::string F(float v) {
    char t[32];
    std::snprintf(t, sizeof(t), "%.3f", double(v));
    return t;
}
std::string V(Vec3 v) {
    return "[" + F(v.x) + ", " + F(v.y) + ", " + F(v.z) + "]";
}

std::string Escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

struct EffectStyle {
    float duration, radius;
};
EffectStyle StyleOf(const std::string& kind) {
    if (kind == "fire") return {1.6f, 0.50f};
    if (kind == "poison") return {1.5f, 0.55f};
    if (kind == "oil") return {3.0f, 0.55f};
    if (kind == "mist") return {2.5f, 1.00f};
    if (kind == "blood") return {1.2f, 0.45f};
    if (kind == "coin") return {1.2f, 0.30f};
    if (kind == "lightning") return {0.6f, 0.50f};
    if (kind == "slash") return {0.7f, 0.40f};
    if (kind == "eye") return {0.8f, 0.50f};
    return {0.5f, 0.12f};
}

} // namespace

// ---- HandVelocity ------------------------------------------------------------------------------

void HandVelocity::Push(double t, Vec3 p) {
    if (!samples.empty() && t <= samples.back().t) {
        return;
    }
    samples.push_back({t, p});
    while (samples.size() > 2 && t - samples.front().t > 0.5) {
        samples.pop_front();
    }
}

Vec3 HandVelocity::Estimate(float window_s, float peak_weight, float end_before_s) const {
    if (samples.size() < 2) {
        return {};
    }
    const double t_end = samples.back().t - double(end_before_s);
    const double t_start = t_end - double(window_s);
    std::vector<const Sample*> in;
    for (const Sample& s : samples) {
        if (s.t >= t_start - 1e-9 && s.t <= t_end + 1e-9) {
            in.push_back(&s);
        }
    }
    if (in.size() < 2) { // sparse frames: the two newest samples at or before the window's end
        in.clear();
        for (auto it = samples.rbegin(); it != samples.rend() && in.size() < 2; ++it) {
            if (it->t <= t_end + 1e-9) {
                in.insert(in.begin(), &*it);
            }
        }
        if (in.size() < 2) {
            return {};
        }
    }
    const double span = in.back()->t - in.front()->t;
    if (span < 1e-4) {
        return {};
    }
    const Vec3 mean = (in.back()->p - in.front()->p) * float(1.0 / span);
    Vec3 peak = mean;
    float best = -1.0f;
    for (size_t i = 0; i + 1 < in.size(); ++i) {
        const double dt = in[i + 1]->t - in[i]->t;
        if (dt < 1e-4) {
            continue;
        }
        const Vec3 v = (in[i + 1]->p - in[i]->p) * float(1.0 / dt);
        const float len = Length(v);
        if (len > best) {
            best = len;
            peak = v;
        }
    }
    return Lerp(mean, peak, std::clamp(peak_weight, 0.0f, 1.0f));
}

// ---- Throwables --------------------------------------------------------------------------------

Throwables::~Throwables() {
    if (log_file) {
        std::fclose(log_file);
    }
}

std::filesystem::path Throwables::ModelPath(int def) const {
    if (def < 0 || size_t(def) >= defs.size() || defs[size_t(def)].mesh.empty()) {
        return {};
    }
    return assets_dir / "throwables" / (defs[size_t(def)].mesh + ".json");
}

void Throwables::Reconfigure(const ThrowConfig& config) {
    // Built-ins, then the extracted bullet table, then the config's per-item overrides.
    std::vector<ThrowItemDef> next = DefaultThrowItems();
    if (extracted.IsObject()) {
        ApplyExtractedTable(next, extracted);
    }
    for (ThrowItemDef& d : next) {
        ApplyItemOverride(d, config.items[std::to_string(d.goods)]);
        ApplyItemOverride(d, config.items[d.name]);
    }
    defs = next;
    has_mesh.assign(defs.size(), false);
    for (size_t i = 0; i < defs.size(); ++i) {
        const auto path = ModelPath(int(i));
        std::error_code ec;
        has_mesh[i] = !path.empty() && std::filesystem::exists(path, ec);
    }
    if (items.size() != defs.size()) {
        items.assign(defs.size(), Item{});
        for (size_t i = 0; i < items.size(); ++i) {
            items[i].def = int(i);
        }
    }
    if (placed) {
        BuildRack(config);
    }
}

void Throwables::Load(const std::filesystem::path& assets, const ThrowConfig& config,
                      std::string& log) {
    assets_dir = assets;
    extracted = {};
    bool ok = false;
    const std::string text = ReadText(assets / "throwables" / "throwables.json", &ok);
    int matched = 0;
    if (ok) {
        extracted = json::Parse(text);
    }
    Reconfigure(config);
    if (ok && extracted.IsObject()) {
        std::vector<ThrowItemDef> probe = DefaultThrowItems();
        matched = ApplyExtractedTable(probe, extracted);
    }
    int meshes = 0;
    for (bool m : has_mesh) {
        meshes += m;
    }
    char t[256];
    std::snprintf(t, sizeof(t),
                  "throwables: %zu items, bullet rows from %s (%d matched), %d extracted meshes, "
                  "%zu procedural stand-ins",
                  defs.size(), ok ? "throwables.json" : "the built-in table", matched, meshes,
                  defs.size() - size_t(meshes));
    log = t;
}

void Throwables::BuildRack(const ThrowConfig& config) {
    const int n = int(defs.size());
    const int cols = std::min(std::max(config.rack_columns, 1), std::max(n, 1));
    const int rows = (n + cols - 1) / cols;
    const float sp = config.rack_spacing_m;
    const Quat yaw = AxisAngle({0, 1, 0}, anchor_yaw);
    const Vec3 centre = anchor + Rotate(yaw, Vec3{config.rack_pos[0], 0.0f, config.rack_pos[1]});
    table_top = floor_y + config.rack_height_m;
    table.rotation = yaw;
    table.half = {0.5f * float(cols) * sp + 0.05f, 0.02f, 0.5f * float(rows) * sp + 0.05f};
    table.center = {centre.x, table_top - table.half.y, centre.z};
    for (int k = 0; k < 4; ++k) {
        const Vec3 local{(k & 1) ? table.half.x - 0.04f : -table.half.x + 0.04f, 0.0f,
                         (k & 2) ? table.half.z - 0.04f : -table.half.z + 0.04f};
        table_legs[k] = table.center + Rotate(yaw, local);
    }
    for (int i = 0; i < n; ++i) {
        Item& it = items[size_t(i)];
        const int col = i % cols, row = i / cols;
        const Vec3 local{(float(col) - 0.5f * float(cols - 1)) * sp, 0.0f,
                         (float(row) - 0.5f * float(rows - 1)) * sp};
        const ThrowItemDef& d = defs[size_t(i)];
        it.home = centre + Rotate(yaw, local);
        it.home.y = table_top + RestHalfHeight(d) + 0.004f;
        it.home_rot = yaw * (d.shape == "knife" ? AxisAngle({0, 0, 1}, 0.5f * kPi) : Quat{0, 0, 0, 1});
        if (it.state == kRack) {
            it.pos = it.home;
            it.rot = it.home_rot;
        }
    }
}

void Throwables::Place(Vec3 head_position, Quat head_rotation, float floor, const ThrowConfig& config) {
    anchor = {head_position.x, floor, head_position.z};
    anchor_yaw = YawOf(head_rotation);
    floor_y = floor;
    placed = true;
    BuildRack(config);
    Respawn();
}

void Throwables::ReturnToRack(Item& item) {
    item.state = kRack;
    item.hand = -1;
    item.pos = item.home;
    item.rot = item.home_rot;
    item.vel = {};
}

void Throwables::Respawn() {
    bullets.clear();
    effects.clear();
    for (int h = 0; h < 2; ++h) {
        held[h] = -1;
        velocity[h].Reset();
    }
    for (Item& it : items) {
        ReturnToRack(it);
    }
}

Mat4 Throwables::HandMatrix(const ThrowInput& in, int hand) const {
    return FromPose(in.grip_rotation[hand], in.grip_position[hand]);
}

Mat4 Throwables::ItemWorld(const Item& item) const {
    const ThrowItemDef& d = defs[size_t(item.def)];
    return Translate(item.pos) * FromQuat(item.rot) * Scale(HasMesh(item.def) ? d.scale : 1.0f);
}

Mat4 Throwables::BulletWorld(const Bullet& b) const {
    const ThrowItemDef& d = defs[size_t(b.def)];
    return Translate(b.pos) * FromQuat(b.rot) * Scale(HasMesh(b.def) ? d.scale : 1.0f);
}

void Throwables::Emit(const ThrowConfig& config, ThrowEvents& events, const std::string& text,
                      const std::string& json) {
    events.log += "throw: " + text + "\n";
    if (!config.log || config.log_file.empty()) {
        return;
    }
    if (!log_file || log_path != config.log_file) {
        if (log_file) {
            std::fclose(log_file);
        }
        log_path = config.log_file;
        log_file = std::fopen(log_path.c_str(), "ab");
    }
    if (log_file) {
        std::fprintf(log_file, "{\"t\": %.3f, %s}\n", now, json.c_str());
        std::fflush(log_file);
    }
}

bool Throwables::Collide(Vec3 p, float radius, const WeaponPhysics& world,
                         const std::vector<int>* skip_parts, std::string& what, int& part,
                         Vec3& point, Vec3& normal) const {
    const col::Capsule sphere{p, p, radius};
    float deepest = -1e9f;
    bool any = false;
    const auto consider = [&](const col::Contact& c, const std::string& name, int part_index) {
        if (c.hit && c.depth > deepest) {
            deepest = c.depth;
            any = true;
            what = name;
            part = part_index;
            point = c.point_b;
            normal = c.normal;
        }
    };
    for (const Collider& c : world.Colliders()) {
        if (c.entity == kEnemy && skip_parts &&
            std::find(skip_parts->begin(), skip_parts->end(), c.part) != skip_parts->end()) {
            continue;
        }
        if (!col::Overlap(col::Aabb{p - Vec3{radius, radius, radius}, p + Vec3{radius, radius, radius}},
                          c.bounds)) {
            continue;
        }
        col::Contact contact;
        switch (c.kind) {
        case Collider::kCapsule: contact = col::CapsuleCapsule(sphere, c.capsule); break;
        case Collider::kBox: contact = col::CapsuleBox(sphere, c.box); break;
        case Collider::kPlane: contact = col::CapsulePlane(sphere, c.plane); break;
        }
        std::string name = c.entity == kFloor ? "floor" : c.entity == kPost ? "post" : c.entity == kWall ? "wall" : "enemy";
        if (c.entity == kEnemy) {
            const auto& names = world.GetEnemy().part_names;
            if (c.part >= 0 && size_t(c.part) < names.size()) {
                name += ":" + names[size_t(c.part)];
            }
        }
        consider(contact, name, c.entity == kEnemy ? c.part : -1);
    }
    return any;
}

void Throwables::Burst(const ThrowItemDef& d, Vec3 point, Vec3 normal, const ThrowConfig& config) {
    Effect e;
    e.kind = d.effect;
    e.pos = point;
    e.normal = Normalize(normal, {0, 1, 0});
    e.t0 = now;
    const EffectStyle style = StyleOf(d.effect);
    e.duration = style.duration;
    e.radius = style.radius * config.effect_scale;
    for (int k = 0; k < 3; ++k) {
        e.color[k] = d.color[k];
    }
    e.seed = int(effects.size() * 7919u + uint32_t(d.goods) + uint32_t(now * 100.0));
    effects.push_back(e);
    if (effects.size() > 64) {
        effects.erase(effects.begin());
    }
}

void Throwables::End(Bullet& b, const std::string& hit, Vec3 end, const ThrowConfig& config,
                     ThrowEvents& events) {
    if (b.ended) {
        return;
    }
    b.ended = true;
    const ThrowItemDef& d = defs[size_t(b.def)];
    Throw t;
    t.item = d.name;
    t.goods = d.goods;
    t.mode = game_mode ? "game" : "hand";
    t.hand_speed = b.hand_speed;
    t.release_speed = b.release_speed;
    t.range_m = std::hypot(end.x - b.origin.x, end.z - b.origin.z);
    t.path_m = b.travelled;
    t.flight_s = float(now - b.born);
    t.hit = hit;
    t.origin = b.origin;
    t.end = end;
    t.bullet_index = b.index;
    const Vec3 dir0 = b.vel;
    t.direction = Normalize(dir0, {0, 0, -1});
    b.hit_what = hit;
    finished.push_back(t);
    if (finished.size() > 256) {
        finished.erase(finished.begin());
    }
    std::string through;
    for (int p : b.hit_parts) {
        through += (through.empty() ? "" : ",") + std::to_string(p);
    }
    char text[256];
    std::snprintf(text, sizeof(text), "%s #%d ends on %s after %.2f m (%.1f m ahead, %.2f s)%s%s",
                  d.name.c_str(), b.index, hit.c_str(), double(t.path_m), double(t.range_m),
                  double(t.flight_s), through.empty() ? "" : ", through enemy parts ",
                  through.c_str());
    Emit(config, events, text,
         "\"event\": \"end\", \"item\": \"" + Escape(d.name) + "\", \"goods\": " +
             std::to_string(d.goods) + ", \"bullet\": " + std::to_string(b.index) +
             ", \"mode\": \"" + t.mode + "\", \"hand_speed\": " + F(t.hand_speed) +
             ", \"release_speed\": " + F(t.release_speed) + ", \"hit\": \"" + Escape(hit) +
             "\", \"range_m\": " + F(t.range_m) + ", \"path_m\": " + F(t.path_m) +
             ", \"flight_s\": " + F(t.flight_s) + ", \"end\": " + V(end) +
             ", \"enemy_parts\": [" + through + "]");
}

void Throwables::Drop(Item& item, Vec3 velocity, const ThrowConfig& config, ThrowEvents& events,
                      const char* why) {
    const ThrowItemDef& d = defs[size_t(item.def)];
    item.state = kFlying;
    item.vel = velocity;
    item.since = item.drop_time = now;
    item.hand = -1;
    Emit(config, events,
         std::string(d.name) + " dropped (" + why + ") at " + F(Length(velocity)) + " m/s",
         "\"event\": \"drop\", \"item\": \"" + Escape(d.name) + "\", \"goods\": " +
             std::to_string(d.goods) + ", \"reason\": \"" + why + "\", \"speed\": " +
             F(Length(velocity)) + ", \"pos\": " + V(item.pos));
}

void Throwables::SpawnBullets(Item& item, Vec3 dir, float speed, float hand_speed, bool game,
                              ThrowEvents& events, const ThrowConfig& config) {
    const ThrowItemDef& d = defs[size_t(item.def)];
    const int n = std::max(d.num_shoot, 1);
    for (int i = 0; i < n; ++i) {
        Bullet b;
        b.item = int(&item - items.data());
        b.def = item.def;
        b.index = i;
        const float yaw = (float(i) - 0.5f * float(n - 1)) * std::fabs(d.shoot_angle_deg) * kDeg;
        b.vel = Rotate(AxisAngle({0, 1, 0}, yaw), dir) * speed;
        b.pos = b.origin = item.pos;
        b.rot = item.rot;
        b.born = now;
        b.hand_speed = hand_speed;
        b.release_speed = speed;
        bullets.push_back(b);
    }
    item.state = kSpent;
    item.since = now;
    thrown_goods.push_back(d.goods);
    char text[256];
    std::snprintf(text, sizeof(text),
                  "%s (%s): hand %.2f m/s -> bullet %.2f m/s (initial %.1f, max %.1f), %d bullet%s",
                  d.name.c_str(), game ? "game throw" : "hand throw", double(hand_speed),
                  double(speed), double(d.initial_velocity), double(d.max_velocity), n,
                  n > 1 ? "s" : "");
    Emit(config, events, text,
         "\"event\": \"throw\", \"item\": \"" + Escape(d.name) + "\", \"goods\": " +
             std::to_string(d.goods) + ", \"mode\": \"" + (game ? "game" : "hand") +
             "\", \"hand_speed\": " + F(hand_speed) + ", \"release_speed\": " + F(speed) +
             ", \"dir\": " + V(dir) + ", \"origin\": " + V(item.pos) +
             ", \"bullets\": " + std::to_string(n));
}

void Throwables::Release(int hand, const ThrowInput& in, const ThrowConfig& config,
                         const WeaponPhysics&, ThrowEvents& events, bool game_throw) {
    Item& item = items[size_t(held[hand])];
    const ThrowItemDef& d = defs[size_t(item.def)];
    held[hand] = -1;
    item.hand = -1;
    Vec3 v = velocity[hand].Estimate(config.smooth_ms * 0.001f, config.peak_weight,
                                     config.release_lag_ms * 0.001f);
    float hs = Length(v);
    events.haptic_amplitude[hand] = std::max(events.haptic_amplitude[hand], 0.3f);
    events.haptic_seconds[hand] = std::max(events.haptic_seconds[hand], 0.04f);
    if (d.back_throw || (!game_throw && hs < config.min_throw_speed_mps) ||
        (game_throw && d.initial_velocity <= 0.0f)) {
        // Back throws land at the hand; weak flicks just let go with the hand's velocity.
        Drop(item, d.back_throw ? Vec3{} : (game_throw ? Vec3{} : v), config, events,
             d.back_throw ? "back throw" : game_throw ? "no flight" : "too slow to throw");
        return;
    }
    Vec3 dir;
    float speed;
    if (game_throw) {
        // The game's throw: along the view, the Bullet's own initial velocity.
        Vec3 fwd = in.head_valid ? Rotate(in.head_rotation, Vec3{0, 0, -1}) : Vec3{0, 0, -1};
        const Vec3 right = Normalize(Cross(fwd, {0, 1, 0}), {1, 0, 0});
        dir = Normalize(Rotate(AxisAngle(right, config.game_pitch_deg * kDeg), fwd), fwd);
        speed = d.initial_velocity;
        hs = Length(v);
    } else {
        dir = v * (1.0f / hs);
        const float scale = std::clamp(hs / config.ref_speed_mps, config.min_scale, config.max_scale);
        speed = d.initial_velocity * scale;
    }
    if (d.max_velocity > 0.0f) {
        speed = std::min(speed, d.max_velocity);
    }
    SpawnBullets(item, dir, speed, hs, game_throw, events, config);
}

void Throwables::Grab(int hand, int item_index, const ThrowInput& in, const ThrowConfig&,
                      ThrowEvents& events) {
    Item& item = items[size_t(item_index)];
    item.state = kHeld;
    item.hand = hand;
    item.since = now;
    held[hand] = item_index;
    velocity[hand].Reset();
    const ThrowItemDef& d = defs[size_t(item.def)];
    const Mat4 g = HandMatrix(in, hand);
    item.pos = TransformPoint(g, d.grip.position);
    item.rot = in.grip_rotation[hand] * FromEulerDeg(d.grip.rotation_deg);
    velocity[hand].Push(now, item.pos);
    events.haptic_amplitude[hand] = std::max(events.haptic_amplitude[hand], 0.5f);
    events.haptic_seconds[hand] = std::max(events.haptic_seconds[hand], 0.05f);
    events.log += std::string("throw: grabbed ") + d.name + " with the " +
                  (hand ? "right" : "left") + " hand\n";
}

bool Throwables::TakeFor(int hand, int goods, const ThrowInput& in, const ThrowConfig& config,
                         ThrowEvents& events) {
    now = in.time;
    if (hand < 0 || hand > 1 || held[hand] >= 0 || !in.hand_valid[hand] || !placed) {
        return false;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        if (defs[size_t(items[i].def)].goods != goods) {
            continue;
        }
        if (items[i].state == kHeld) {
            return false; // (the other hand has it)
        }
        Grab(hand, int(i), in, config, events);
        return true;
    }
    return false;
}

void Throwables::StepItem(Item& item, double dt, const ThrowConfig& config,
                          const WeaponPhysics& world, ThrowEvents& events) {
    const ThrowItemDef& d = defs[size_t(item.def)];
    if (item.state == kFlying) {
        const float travel = Length(item.vel) * float(dt);
        const int n = std::max({1, int(std::ceil(travel / kMaxStepM)),
                                int(std::ceil(dt / double(kMaxSubstep)))});
        const float h = float(dt) / float(n);
        for (int i = 0; i < n && item.state == kFlying; ++i) {
            item.vel.y -= config.grav * h;
            const Vec3 next = item.pos + item.vel * h;
            std::string what;
            int part = -1;
            Vec3 point, normal;
            const float r = std::max(d.radius_m, 0.02f);
            bool hit = Collide(next, r, world, nullptr, what, part, point, normal);
            if (!hit) {
                // The table is a box in the rack's own space.
                const col::Contact c = col::CapsuleBox({next, next, r}, table);
                if (placed && c.hit) {
                    hit = true;
                    what = "table";
                    point = c.point_b;
                    normal = c.normal;
                }
            }
            if (hit) {
                item.pos = point + normal * r;
                item.vel = {};
                if (d.back_throw) {
                    Burst(d, point, normal, config);
                    item.state = kSpent;
                    item.since = now;
                    Emit(config, events,
                         d.name + " breaks on " + what + " (" + F(float(now - item.drop_time)) + " s)",
                         "\"event\": \"drop_burst\", \"item\": \"" + Escape(d.name) +
                             "\", \"goods\": " + std::to_string(d.goods) + ", \"hit\": \"" +
                             Escape(what) + "\", \"pos\": " + V(point));
                } else {
                    item.state = kRest;
                    item.since = now;
                }
            } else {
                item.pos = next;
            }
        }
        if (item.state == kFlying && !d.align.x && !d.align.y && !d.align.z) {
            item.rot = AxisAngle(Normalize(Vec3{0.3f, 0.2f, 1.0f}), d.spin_dps * kDeg * float(now - item.drop_time)) *
                       item.rot;
        }
        if (item.pos.y < floor_y - 2.0f) { // fell out of the world (floor collider off)
            ReturnToRack(item);
        }
    } else if (item.state == kRest && now - item.since > double(config.rest_return_s)) {
        ReturnToRack(item);
    } else if (item.state == kSpent) {
        const bool burning = std::any_of(bullets.begin(), bullets.end(), [&](const Bullet& b) {
            return b.alive && b.item == int(&item - items.data());
        });
        if (!burning && now - item.since > double(config.respawn_s)) {
            ReturnToRack(item);
        }
    }
}

void Throwables::StepBullet(Bullet& b, double dt, const ThrowConfig& config,
                            const WeaponPhysics& world, ThrowEvents& events) {
    const ThrowItemDef& d = defs[size_t(b.def)];
    if (b.resting) {
        if (d.fuse_s > 0.0f && !b.ended) {
            if (now - b.rest_since >= double(d.fuse_s)) {
                Burst(d, b.pos, {0, 1, 0}, config);
                End(b, b.hit_what.empty() ? "fuse" : b.hit_what + " (fuse)", b.pos, config, events);
                b.alive = false;
            }
        } else if (now - b.rest_since >= double(kLingerS)) {
            b.alive = false;
        }
        return;
    }
    const float travel = Length(b.vel) * float(dt);
    const int n = std::max({1, int(std::ceil(travel / kMaxStepM)),
                            int(std::ceil(dt / double(kMaxSubstep)))});
    const float h = float(dt) / float(n);
    const float r = d.radius_m;
    for (int i = 0; i < n && b.alive && !b.resting; ++i) {
        const bool in_range = d.range_m > 0.0f && b.travelled < d.range_m;
        float speed = Length(b.vel);
        const Vec3 dir = speed > 1e-4f ? b.vel * (1.0f / speed) : Vec3{0, -1, 0};
        if (in_range && d.accel_in > 0.0f) {
            speed += d.accel_in * h;
        }
        b.vel = dir * speed;
        b.vel.y -= (in_range ? d.gravity_in : d.gravity_out) * h;
        speed = Length(b.vel);
        if (d.max_velocity > 0.0f && speed > d.max_velocity) {
            b.vel = b.vel * (d.max_velocity / speed);
        }
        const Vec3 next = b.pos + b.vel * h;
        std::string what;
        int part = -1;
        Vec3 point, normal;
        bool hit = Collide(next, r, world, d.penetrate ? &b.hit_parts : nullptr, what, part, point, normal);
        if (!hit && placed && Length(next - b.origin) > kHoldTableSkipM) {
            const col::Contact c = col::CapsuleBox({next, next, r}, table);
            if (c.hit) {
                hit = true;
                what = "table";
                point = c.point_b;
                normal = c.normal;
            }
        }
        if (hit && part >= 0 && what.rfind("enemy", 0) == 0 && d.penetrate) {
            // A penetrating bullet hits each part once and flies on.
            b.hit_parts.push_back(part);
            Burst(d, point, normal, config);
            char text[160];
            std::snprintf(text, sizeof(text), "%s #%d passes through %s at %.2f m", d.name.c_str(),
                          b.index, what.c_str(), double(b.travelled));
            Emit(config, events, text,
                 "\"event\": \"pierce\", \"item\": \"" + Escape(d.name) + "\", \"bullet\": " +
                     std::to_string(b.index) + ", \"hit\": \"" + Escape(what) +
                     "\", \"path_m\": " + F(b.travelled));
            hit = false;
        }
        b.travelled += Length(b.vel) * h;
        b.pos = next;
        if (hit) {
            b.pos = point + normal * r;
            b.vel = {};
            b.hit_what = what;
            if (d.fuse_s > 0.0f) {
                b.resting = true;
                b.rest_since = now;
                char text[160];
                std::snprintf(text, sizeof(text), "%s lands on %s, bursts in %.1f s", d.name.c_str(),
                              what.c_str(), double(d.fuse_s));
                Emit(config, events, text,
                     "\"event\": \"land\", \"item\": \"" + Escape(d.name) + "\", \"hit\": \"" +
                         Escape(what) + "\", \"fuse_s\": " + F(d.fuse_s));
            } else {
                Burst(d, point, normal, config);
                End(b, what, point, config, events);
                if (Persistent(d)) {
                    b.resting = true;
                    b.rest_since = now;
                } else {
                    b.alive = false;
                }
            }
            return;
        }
        if (d.life_s > 0.0f && now - b.born + double(h) * double(i + 1 - n) >= double(d.life_s)) {
            Burst(d, b.pos, {0, 1, 0}, config);
            End(b, "expired", b.pos, config, events);
            if (Persistent(d)) {
                b.resting = true;
                b.rest_since = now;
            } else {
                b.alive = false;
            }
            return;
        }
    }
    // Orientation: knives follow the velocity, the rest tumbles.
    if (b.alive && !b.resting) {
        const Vec3 v = b.vel;
        if (d.align.x || d.align.y || d.align.z) {
            if (Length(v) > 1e-3f) {
                b.rot = FromTo(Normalize(d.align), Normalize(v));
            }
        } else {
            const Vec3 axis = Normalize(Cross({0, 1, 0}, Normalize(v, {0, 0, -1})), {1, 0, 0});
            b.rot = AxisAngle(axis, d.spin_dps * kDeg * float(now - b.born));
        }
    }
}

void Throwables::Step(const ThrowInput& in, const ThrowConfig& config, const WeaponPhysics& world,
                      ThrowEvents& events) {
    now = in.time;
    const double dt = have_time ? std::clamp(in.time - last_time, 0.0, 0.1) : 0.0;
    have_time = true;
    last_time = in.time;
    if (!config.enabled || !placed) {
        return;
    }
    if (config.mode != mode_seen) {
        mode_seen = config.mode;
        game_mode = config.mode == "game";
    }
    const uint32_t pressed = in.buttons & ~last_buttons;
    last_buttons = in.buttons;
    if (const uint32_t b = ThrowButton(config.mode_button); b && (pressed & b)) {
        game_mode = !game_mode;
        events.log += std::string("throw: mode ") + (game_mode ? "game (trigger throws along the view)" : "hand (release throws with the hand's velocity)") + "\n";
        events.haptic_amplitude[1] = std::max(events.haptic_amplitude[1], 0.3f);
        events.haptic_seconds[1] = std::max(events.haptic_seconds[1], 0.04f);
    }
    if (const uint32_t b = ThrowButton(config.respawn_button); b && (pressed & b)) {
        Respawn();
        events.log += "throw: items back on the rack\n";
    }
    // Hands: grab, carry, release.
    for (int hand = 0; hand < 2; ++hand) {
        const bool valid = in.hand_valid[hand];
        const bool squeezing = valid && in.squeeze[hand] >= (squeeze_was[hand] ? config.release_squeeze : config.grab_squeeze);
        if (held[hand] >= 0) {
            Item& item = items[size_t(held[hand])];
            const ThrowItemDef& d = defs[size_t(item.def)];
            if (valid) {
                item.pos = TransformPoint(HandMatrix(in, hand), d.grip.position);
                item.rot = in.grip_rotation[hand] * FromEulerDeg(d.grip.rotation_deg);
                velocity[hand].Push(now, item.pos);
            }
            const bool trigger_down = valid && in.trigger[hand] >= config.game_trigger;
            if (game_mode && trigger_down && !trigger_was[hand] && valid) {
                Release(hand, in, config, world, events, true);
            } else if (!squeezing) {
                if (game_mode && valid) {
                    // The game has no hand throw: letting go drops it.
                    held[hand] = -1;
                    item.hand = -1;
                    Drop(item, {}, config, events, "released without the throw button");
                } else if (valid) {
                    Release(hand, in, config, world, events, false);
                } else {
                    held[hand] = -1;
                    Drop(item, {}, config, events, "tracking lost");
                }
            }
        } else if (valid) {
            // Nearest grabbable item within the radius (hover tick when one comes in reach).
            const Vec3 point = TransformPoint(HandMatrix(in, hand), config.grab_point);
            int best = -1;
            float best_d = config.grab_radius_m;
            for (size_t i = 0; i < items.size(); ++i) {
                if ((items[i].state != kRack || !config.rack_grab) && items[i].state != kRest) {
                    continue;
                }
                const float dist = Length(items[i].pos - point);
                if (dist <= best_d) {
                    best_d = dist;
                    best = int(i);
                }
            }
            if (best != hover[hand] && best >= 0) {
                events.haptic_amplitude[hand] = std::max(events.haptic_amplitude[hand], 0.15f);
                events.haptic_seconds[hand] = std::max(events.haptic_seconds[hand], 0.02f);
            }
            hover[hand] = best;
            if (best >= 0 && squeezing && (in.squeeze[hand] >= config.grab_squeeze)) {
                Grab(hand, best, in, config, events);
            }
        }
        squeeze_was[hand] = squeezing;
        trigger_was[hand] = valid && in.trigger[hand] >= config.game_trigger;
    }
    // Falling items, flying bullets, effects.
    for (Item& item : items) {
        StepItem(item, dt, config, world, events);
    }
    for (Bullet& b : bullets) {
        if (b.alive && b.born < now) { // (a bullet thrown this frame starts flying next frame)
            StepBullet(b, dt, config, world, events);
        }
    }
    bullets.erase(std::remove_if(bullets.begin(), bullets.end(),
                                 [](const Bullet& b) { return !b.alive && b.ended; }),
                  bullets.end());
    effects.erase(std::remove_if(effects.begin(), effects.end(),
                                 [&](const Effect& e) { return now - e.t0 > double(e.duration); }),
                  effects.end());
}

// ---- Wireframes --------------------------------------------------------------------------------

void Throwables::TableLines(const LineFn& line) const {
    if (!placed) {
        return;
    }
    static const uint8_t kWood[3] = {150, 110, 70};
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        const Vec3 l{(i & 1) ? table.half.x : -table.half.x, (i & 2) ? table.half.y : -table.half.y,
                     (i & 4) ? table.half.z : -table.half.z};
        c[i] = table.center + Rotate(table.rotation, l);
    }
    for (int i = 0; i < 8; ++i) {
        for (int bit = 1; bit < 8; bit <<= 1) {
            if (!(i & bit)) {
                line(c[i], c[i | bit], kWood);
            }
        }
    }
    for (const Vec3& leg : table_legs) {
        line(leg - Vec3{0, table.half.y, 0}, {leg.x, floor_y, leg.z}, kWood);
    }
}

void Throwables::EffectLines(const LineFn& line, const ThrowConfig& config) const {
    if (!config.show_markers) {
        return;
    }
    for (const Effect& e : effects) {
        const float f = std::clamp(float((now - e.t0) / double(e.duration)), 0.0f, 1.0f);
        const float fade = 1.0f - f * f;
        uint8_t rgb[3];
        for (int k = 0; k < 3; ++k) {
            rgb[k] = uint8_t(std::clamp(e.color[k] * fade, 0.0f, 1.0f) * 255.0f);
        }
        const Vec3 n = e.normal;
        const Vec3 u = AnyPerp(n), w = Cross(n, u);
        const auto ring = [&](Vec3 centre, float r, int seg = 16) {
            Vec3 prev = centre + u * r;
            for (int i = 1; i <= seg; ++i) {
                const float a = 2.0f * kPi * float(i) / float(seg);
                const Vec3 p = centre + u * (r * std::cos(a)) + w * (r * std::sin(a));
                line(prev, p, rgb);
                prev = p;
            }
        };
        const float R = e.radius;
        const float grow = std::min(1.0f, f * 4.0f);
        if (e.kind == "fire") {
            ring(e.pos, R * grow);
            for (int i = 0; i < 10; ++i) {
                const float a = 2.0f * kPi * Rand01(e.seed, i);
                const float rr = R * grow * Rand01(e.seed, i + 20);
                const Vec3 base = e.pos + u * (rr * std::cos(a)) + w * (rr * std::sin(a));
                const float flick = 0.6f + 0.4f * std::sin(float(now) * 18.0f + float(i) * 2.0f);
                line(base, base + n * (R * 1.1f * flick * (1.0f - f)), rgb);
            }
        } else if (e.kind == "poison") {
            ring(e.pos, R * grow);
            for (int i = 0; i < 6; ++i) {
                const float a = 2.0f * kPi * Rand01(e.seed, i);
                const float rr = R * 0.7f * Rand01(e.seed, i + 20);
                const Vec3 c = e.pos + u * (rr * std::cos(a)) + w * (rr * std::sin(a)) + n * (f * (0.15f + 0.4f * Rand01(e.seed, i + 40)));
                ring(c, 0.03f, 6);
            }
        } else if (e.kind == "oil") {
            ring(e.pos, R * grow);
            ring(e.pos, R * grow * 0.65f);
            ring(e.pos, R * grow * 0.3f, 10);
        } else if (e.kind == "mist") {
            for (int k = 0; k < 3; ++k) {
                ring(e.pos + n * (0.12f + 0.22f * float(k)), R * (0.45f + 0.55f * f + 0.1f * float(k)), 20);
            }
        } else if (e.kind == "blood") {
            ring(e.pos, R * 0.5f * grow, 10);
            for (int i = 0; i < 9; ++i) {
                const float a = 2.0f * kPi * Rand01(e.seed, i);
                const Vec3 d = u * std::cos(a) + w * std::sin(a);
                const float len = R * (0.4f + 0.8f * Rand01(e.seed, i + 20)) * grow;
                line(e.pos + d * (len * 0.6f) + n * 0.02f, e.pos + d * len + n * (0.05f * (1.0f - f)), rgb);
            }
        } else if (e.kind == "coin") {
            for (int i = 0; i < 7; ++i) {
                const float a = 2.0f * kPi * Rand01(e.seed, i);
                const float rr = R * Rand01(e.seed, i + 20);
                const Vec3 c = e.pos + u * (rr * std::cos(a)) + w * (rr * std::sin(a)) + n * (0.05f + 0.25f * std::sin(f * kPi) * Rand01(e.seed, i + 40));
                line(c - u * 0.02f, c + u * 0.02f, rgb);
                line(c - w * 0.02f, c + w * 0.02f, rgb);
                line(c - n * 0.02f, c + n * 0.02f, rgb);
            }
        } else if (e.kind == "lightning") {
            for (int bolt = 0; bolt < 3; ++bolt) {
                Vec3 p = e.pos + n * 0.02f;
                for (int s = 0; s < 8; ++s) {
                    const float j = (Rand01(e.seed + int(now * 40.0), bolt * 10 + s) - 0.5f) * 0.28f;
                    const float j2 = (Rand01(e.seed + int(now * 40.0), bolt * 10 + s + 5) - 0.5f) * 0.28f;
                    const Vec3 q = e.pos + n * (0.2f * float(s + 1)) + u * j + w * j2;
                    line(p, q, rgb);
                    p = q;
                }
            }
            ring(e.pos, R * grow, 12);
        } else if (e.kind == "slash") {
            const float s = R * grow;
            line(e.pos - u * s - w * s, e.pos + u * s + w * s, rgb);
            line(e.pos - u * s + w * s, e.pos + u * s - w * s, rgb);
            line(e.pos - n * s * 0.5f, e.pos + n * s * 0.5f, rgb);
        } else if (e.kind == "eye") {
            ring(e.pos, R * grow, 18);
            ring(e.pos, R * grow * 0.4f, 10);
            line(e.pos - n * R * 0.5f, e.pos + n * R * 0.5f, rgb);
        } else { // dust
            ring(e.pos, R * grow, 10);
            line(e.pos - u * 0.04f, e.pos + u * 0.04f, rgb);
            line(e.pos - w * 0.04f, e.pos + w * 0.04f, rgb);
            line(e.pos, e.pos + n * 0.08f, rgb);
        }
    }
}

// ---- Procedural stand-ins ------------------------------------------------------------------------

namespace {

struct Builder {
    Model m;
    void Vert(Vec3 p, Vec3 n) {
        Vertex v{};
        v.position[0] = p.x, v.position[1] = p.y, v.position[2] = p.z;
        v.normal[0] = n.x, v.normal[1] = n.y, v.normal[2] = n.z;
        v.weights[0] = 1.0f;
        m.vertices.push_back(v);
    }
    uint32_t Base() const { return uint32_t(m.vertices.size()); }
    void Box(Vec3 c, Vec3 half) {
        const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const Vec3& n : normals) {
            const Vec3 u = AnyPerp(n), w = Cross(n, u);
            const uint32_t b = Base();
            const float s[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (const auto& q : s) {
                const Vec3 corner = n + u * q[0] + w * q[1];
                Vert(c + Vec3{corner.x * half.x, corner.y * half.y, corner.z * half.z}, n);
            }
            m.indices.insert(m.indices.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
        }
    }
    /// Cylinder along Y from y0 to y1; radii r0 at y0 and r1 at y1.
    void Cylinder(Vec3 c, float y0, float y1, float r0, float r1, int seg = 14) {
        const uint32_t b = Base();
        for (int i = 0; i <= seg; ++i) {
            const float a = 2.0f * kPi * float(i) / float(seg);
            const Vec3 n{std::cos(a), 0.0f, std::sin(a)};
            Vert(c + Vec3{n.x * r0, y0, n.z * r0}, n);
            Vert(c + Vec3{n.x * r1, y1, n.z * r1}, n);
        }
        for (int i = 0; i < seg; ++i) {
            const uint32_t k = b + uint32_t(i * 2);
            m.indices.insert(m.indices.end(), {k, k + 1, k + 2, k + 1, k + 3, k + 2});
        }
        for (int cap = 0; cap < 2; ++cap) {
            const float y = cap ? y1 : y0, r = cap ? r1 : r0;
            const Vec3 n{0.0f, cap ? 1.0f : -1.0f, 0.0f};
            const uint32_t centre = Base();
            Vert(c + Vec3{0, y, 0}, n);
            for (int i = 0; i <= seg; ++i) {
                const float a = 2.0f * kPi * float(i) / float(seg);
                Vert(c + Vec3{std::cos(a) * r, y, std::sin(a) * r}, n);
            }
            for (int i = 0; i < seg; ++i) {
                m.indices.insert(m.indices.end(), {centre, centre + 1 + uint32_t(i), centre + 2 + uint32_t(i)});
            }
        }
    }
    void Sphere(Vec3 c, Vec3 radii, int lat = 8, int lon = 12) {
        const uint32_t b = Base();
        for (int i = 0; i <= lat; ++i) {
            const float t = kPi * float(i) / float(lat);
            for (int j = 0; j <= lon; ++j) {
                const float a = 2.0f * kPi * float(j) / float(lon);
                const Vec3 n{std::sin(t) * std::cos(a), std::cos(t), std::sin(t) * std::sin(a)};
                Vert(c + Vec3{n.x * radii.x, n.y * radii.y, n.z * radii.z}, n);
            }
        }
        for (int i = 0; i < lat; ++i) {
            for (int j = 0; j < lon; ++j) {
                const uint32_t k = b + uint32_t(i * (lon + 1) + j);
                m.indices.insert(m.indices.end(), {k, k + 1, k + uint32_t(lon + 1), k + 1, k + uint32_t(lon + 2), k + uint32_t(lon + 1)});
            }
        }
    }
};

} // namespace

Model MakeThrowModel(const std::string& shape, float scale) {
    Builder b;
    const float s = scale;
    if (shape == "bottle") {
        b.Cylinder({0, 0, 0}, -0.11f * s, 0.02f * s, 0.035f * s, 0.035f * s);
        b.Cylinder({0, 0, 0}, 0.02f * s, 0.07f * s, 0.035f * s, 0.012f * s);
        b.Cylinder({0, 0, 0}, 0.07f * s, 0.11f * s, 0.012f * s, 0.014f * s, 10);
    } else if (shape == "urn") {
        b.Sphere({0, -0.01f * s, 0}, {0.075f * s, 0.085f * s, 0.075f * s});
        b.Cylinder({0, 0, 0}, 0.06f * s, 0.10f * s, 0.035f * s, 0.045f * s, 12);
    } else if (shape == "vial") {
        b.Cylinder({0, 0, 0}, -0.06f * s, 0.02f * s, 0.028f * s, 0.028f * s);
        b.Cylinder({0, 0, 0}, 0.02f * s, 0.06f * s, 0.028f * s, 0.01f * s, 10);
        b.Cylinder({0, 0, 0}, 0.06f * s, 0.07f * s, 0.012f * s, 0.012f * s, 8);
    } else if (shape == "knife") { // blade tip toward +Z, flat in the YZ plane
        b.Box({0, 0, 0.05f * s}, {0.003f * s, 0.014f * s, 0.06f * s});
        b.Box({0, 0, -0.06f * s}, {0.008f * s, 0.01f * s, 0.045f * s});
    } else if (shape == "disc") {
        b.Cylinder({0, 0, 0}, -0.003f * s, 0.003f * s, 0.04f * s, 0.04f * s, 16);
    } else if (shape == "paper") { // a sheet with a rolled edge (weapon-buff papers)
        b.Box({0, 0, 0}, {0.045f * s, 0.0025f * s, 0.065f * s});
        b.Cylinder({0, 0, 0}, -0.003f * s, 0.003f * s, 0.006f * s, 0.006f * s, 8);
        b.Box({0.0f, 0.004f * s, 0.0f}, {0.03f * s, 0.0015f * s, 0.045f * s});
    } else if (shape == "pellet") {
        b.Sphere({0, 0, 0}, {0.016f * s, 0.016f * s, 0.016f * s}, 6, 9);
    } else if (shape == "bone") { // a knuckled shaft along Y
        b.Cylinder({0, 0, 0}, -0.05f * s, 0.05f * s, 0.011f * s, 0.011f * s, 8);
        b.Sphere({0.006f * s, -0.052f * s, 0}, {0.017f * s, 0.014f * s, 0.014f * s}, 5, 8);
        b.Sphere({-0.006f * s, 0.052f * s, 0}, {0.017f * s, 0.014f * s, 0.014f * s}, 5, 8);
    } else if (shape == "bell") {
        b.Cylinder({0, 0, 0}, -0.035f * s, 0.03f * s, 0.045f * s, 0.016f * s, 14);
        b.Sphere({0, -0.037f * s, 0}, {0.012f * s, 0.012f * s, 0.012f * s}, 5, 8);
        b.Cylinder({0, 0, 0}, 0.03f * s, 0.05f * s, 0.006f * s, 0.006f * s, 6);
    } else if (shape == "flask") { // a round flask with a short neck (elixirs)
        b.Sphere({0, -0.02f * s, 0}, {0.035f * s, 0.035f * s, 0.035f * s});
        b.Cylinder({0, 0, 0}, 0.01f * s, 0.06f * s, 0.013f * s, 0.011f * s, 10);
        b.Cylinder({0, 0, 0}, 0.06f * s, 0.07f * s, 0.014f * s, 0.014f * s, 10);
    } else if (shape == "syringe") { // antidote / sedative: a slim vial with a needle cap
        b.Cylinder({0, 0, 0}, -0.06f * s, 0.04f * s, 0.013f * s, 0.013f * s, 10);
        b.Cylinder({0, 0, 0}, 0.04f * s, 0.075f * s, 0.013f * s, 0.003f * s, 8);
        b.Cylinder({0, 0, 0}, -0.07f * s, -0.06f * s, 0.019f * s, 0.019f * s, 10);
    } else if (shape == "packet") { // a folded packet (ash, dew)
        b.Box({0, 0, 0}, {0.032f * s, 0.045f * s, 0.008f * s});
        b.Box({0, 0.03f * s, 0.006f * s}, {0.032f * s, 0.016f * s, 0.004f * s});
    } else if (shape == "tablet") { // Hunter's Mark: a small stone tablet
        b.Box({0, 0, 0}, {0.03f * s, 0.045f * s, 0.008f * s});
        b.Cylinder({0, 0, 0}, 0.045f * s, 0.058f * s, 0.03f * s, 0.012f * s, 4);
    } else if (shape == "stone") {
        b.Sphere({0, 0, 0}, {0.04f * s, 0.03f * s, 0.035f * s}, 6, 9);
    } else {
        b.Sphere({0, 0, 0}, {0.03f * s, 0.03f * s, 0.03f * s});
    }
    Model& m = b.m;
    m.name = "throw_" + shape;
    m.kind = "throwable";
    MeshRange range;
    range.name = range.part = m.name;
    range.slot = "weapon";
    range.index_count = uint32_t(m.indices.size());
    range.vertex_count = uint32_t(m.vertices.size());
    m.meshes.push_back(range);
    return std::move(b.m);
}

} // namespace sb
