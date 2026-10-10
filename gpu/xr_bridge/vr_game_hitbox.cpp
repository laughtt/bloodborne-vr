// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "vr_game_hitbox.h"

#include "sandbox3d/hit_scale.h"

#include <algorithm>
#include <cmath>

#include "sandbox3d/json.h"
#include "sandbox3d/wire_shapes.h"

namespace vrui {

using namespace m3;

namespace {

/// FNV-1a 32 (src/vr_hitbox_math.h vhb_hash: the host hashes the game's bone names the same way).
uint32_t BoneHash(const std::string& s) {
    uint32_t h = 2166136261u;
    for (const char c : s) {
        h = (h ^ uint8_t(c)) * 16777619u;
    }
    return h;
}

Vec3 Apply(const float m[12], Vec3 p) {
    return {m[0] * p.x + m[3] * p.y + m[6] * p.z + m[9], m[1] * p.x + m[4] * p.y + m[7] * p.z + m[10],
            m[2] * p.x + m[5] * p.y + m[8] * p.z + m[11]};
}

/// The sandbox's 8-bit colours are sRGB; the UI canvas takes linear ones.
sb::UiColor Linear(sb::WireColor c, float alpha) {
    auto lin = [](uint8_t v) { return std::pow(float(v) / 255.0f, 2.2f); };
    return {lin(c.r), lin(c.g), lin(c.b), alpha};
}

/// The bone matrices of this frame by name hash.
struct Bones {
    const BbXrGameDebug& d;
    const float* Find(const std::string& name) const {
        if (name.empty()) {
            return nullptr;
        }
        const uint32_t h = BoneHash(name);
        const uint32_t n = std::min<uint32_t>(d.bone_count, BBXR_DEBUG_MAX_BONES);
        for (uint32_t i = 0; i < n; ++i) {
            if (d.bones[i].name_hash == h) {
                return d.bones[i].skin;
            }
        }
        return nullptr;
    }
};

bool SameShape(const sb::HitShape& a, const sb::HitShape& b) {
    return Length(a.p[0].pos - b.p[0].pos) < 1e-3f && Length(a.p[1].pos - b.p[1].pos) < 1e-3f &&
           std::fabs(a.radius - b.radius) < 1e-3f;
}

} // namespace

void GameHitboxes::Load(const std::filesystem::path& assets_dir, std::string& log) {
    assets = assets_dir;
    models.clear();
    weapons.clear();
    bool ok = false;
    const std::string text = sb::ReadText(assets / "hitbox" / "index.json", &ok);
    if (ok) {
        const json::Value index = json::Parse(text);
        for (const json::Value& w : index["weapons"].array) {
            if (w["id"].Int(-1) >= 0 && !w["model"].Str().empty()) {
                models[w["id"].Int()] = w["model"].Str();
            }
        }
    }
    std::string hurt_log;
    self_boxes = sb::LoadHurtboxes(assets, "c0000", hurt_log);
    log += "game hitboxes: " + std::to_string(models.size()) + " weapons in " +
           (assets / "hitbox" / "index.json").string() + (ok ? "" : " (missing)") + "; " +
           hurt_log + "\n";
}

int GameHitboxes::IndexIdFor(uint32_t game_id) const {
    if (game_id == BBXR_ITEM_NONE || (game_id & 0xF0000000u)) {
        return -1;
    }
    const int id = int(game_id);
    for (const int base : {id, id / 10000 * 10000, id / 100000 * 100000}) {
        if (models.count(base)) {
            return base;
        }
    }
    return -1;
}

const sb::Hitboxes* GameHitboxes::WeaponBoxes(uint32_t game_id, std::string& log) {
    const int id = IndexIdFor(game_id);
    if (id < 0) {
        return nullptr;
    }
    Weapon& w = weapons[id];
    if (!w.tried) {
        w.tried = true;
        sb::Model model;
        model.name = models[id];
        std::string load_log;
        w.boxes = sb::LoadHitboxes(assets, model, id, 0.03f, load_log);
        if (w.boxes.source == "placeholder") {
            w.boxes.attacks.clear(); // no model, no dummies: nothing to draw
        }
        log += "game hitboxes: weapon " + std::to_string(game_id) + " -> " + model.name + ": " +
               load_log + "\n";
    }
    return w.boxes.attacks.empty() ? nullptr : &w.boxes;
}

void GameHitboxes::OnEvent(const BbXrGameEvent& e, double now) {
    if (e.kind == BBXR_EV_SWING_OPEN) {
        attack_atk = e.b;
        attack_ticket = e.c;
        attack_at = now;
    } else if (e.kind == BBXR_EV_SWING_HIT && (e.c == attack_ticket || attack_atk >= 0)) {
        flash_at = now;
    } else if (e.kind == BBXR_EV_SWING_MISS && e.c == attack_ticket) {
        attack_atk = -1;
    }
}

void GameHitboxes::Lines(const sb::UiDebugHitboxes& cfg, const BbXrGameState& s, double now,
                         std::vector<HitboxLine>& out, std::string& log) {
    const BbXrGameDebug& d = s.debug;
    if (cfg.enemies && !enemies_logged && (d.flags & BBXR_DEBUG_POSE)) {
        enemies_logged = true;
        log += "game hitboxes: enemy hurtboxes are not drawn yet (no enemy list; findings/29)\n";
    }
    auto capsule = [&](Vec3 a, Vec3 b, float r, sb::UiColor c, float width) {
        sb::WireCapsule([&](Vec3 p, Vec3 q) { out.push_back({p, q, c, width}); }, a, b, r);
    };
    const bool in_attack = attack_atk >= 0 && now - attack_at < kHitboxAttackS;
    const bool flash = now - flash_at < kHitboxFlashS;
    if ((d.flags & BBXR_DEBUG_POSE) && (cfg.weapon || cfg.attack) && (s.flags & BBXR_GAME_WEAPONS)) {
        const Bones bones{d};
        for (int side = 0; side < 2; ++side) { // 0 left (L_Weapon), 1 right (R_Weapon)
            if (!(d.weapon_valid & (1u << side))) {
                continue;
            }
            const uint32_t slot = side == 1 ? std::min<uint32_t>(s.weapon_active[0], 1)
                                            : 2 + std::min<uint32_t>(s.weapon_active[1], 1);
            const sb::Hitboxes* hb = WeaponBoxes(s.weapons[slot], log);
            if (!hb) {
                continue;
            }
            // The current attack's shapes (this weapon has its AtkParam id), else every attack's.
            const sb::Attack* current = nullptr;
            if (in_attack && cfg.attack) {
                for (const sb::Attack& a : hb->attacks) {
                    if (a.atk_id == attack_atk) {
                        current = &a;
                        break;
                    }
                }
            }
            std::vector<const sb::HitShape*> shown;
            for (const sb::Attack& a : hb->attacks) {
                if (current && &a != current) {
                    continue;
                }
                for (const sb::HitShape& hit : a.hits) {
                    if (!current && (hit.p[0].on_body || hit.p[1].on_body)) {
                        continue; // kicks: only while that attack runs
                    }
                    if (std::none_of(shown.begin(), shown.end(),
                                     [&](const sb::HitShape* o) { return SameShape(*o, hit); })) {
                        shown.push_back(&hit);
                    }
                }
            }
            // The VR hit scale (findings/32): the shapes the hook gives the game's hit test.
            const BbXrHitScale& hs = d.hit_scale;
            VhsParams scale = vhs_identity();
            if (hs.valid && hs.enabled) {
                scale = vhs_defaults();
                scale.radius_scale = hs.radius_scale;
                scale.reach_m = hs.reach_m;
                for (int c = 0; c < VHS_CLASSES; ++c) {
                    scale.class_radius_scale[c] = hs.class_radius_scale[c];
                    scale.class_reach_m[c] = hs.class_reach_m[c];
                }
                vhs_sanitize(&scale);
            }
            const float weapon_reach = sb::HitWeaponReach(*hb);
            const bool lit = cfg.attack && in_attack && (current || side == 1);
            if (!lit && !cfg.weapon) {
                continue;
            }
            for (const sb::HitShape* shape : shown) {
                const sb::HitShape scaled = sb::ScaleHitShape(scale, *shape, weapon_reach);
                const sb::HitShape* hit = &scaled;
                Vec3 p[2];
                bool ok = true;
                for (int k = 0; k < 2; ++k) {
                    const sb::ShapePoint& sp = hit->p[k];
                    const float* m = sp.on_body ? bones.Find(sp.bone_name) : d.weapon[side];
                    ok = ok && m;
                    if (m) {
                        p[k] = Apply(m, sp.pos);
                    }
                }
                if (!ok) {
                    continue;
                }
                sb::UiColor c = Linear(sb::HitTypeColor(hit->hit_type),
                                       lit ? 1.0f : cfg.idle_alpha);
                float width = lit ? 2.0f : 1.0f;
                if (lit && flash) {
                    c = {1.0f, 1.0f, 1.0f, 1.0f};
                    width = 3.0f;
                }
                capsule(p[0], hit->capsule ? p[1] : p[0], hit->radius, c, width);
            }
        }
    }
    if ((d.flags & BBXR_DEBUG_POSE) && cfg.self) {
        const Bones bones{d};
        for (const sb::Hurtbox& box : self_boxes) {
            const float* ma = bones.Find(box.a.bone_name);
            const float* mb = bones.Find(box.b.bone_name);
            if (!ma || !mb) {
                continue;
            }
            const Vec3 a = Apply(ma, box.a.pos), b = Apply(mb, box.b.pos);
            capsule(a, box.capsule ? b : a, box.radius,
                    Linear(sb::HurtGroupColor(box.part_group), 0.8f), 1.0f);
        }
    }
    if (cfg.hits) {
        const uint32_t n = std::min<uint32_t>(d.hit_count, BBXR_DEBUG_MAX_HITS);
        for (uint32_t i = 0; i < n; ++i) {
            const BbXrDebugHit& h = d.hits[i];
            const Vec3 p{h.pos[0], h.pos[1], h.pos[2]};
            const float fade = std::clamp(1.0f - h.age_s / 3.0f, 0.0f, 1.0f);
            if (fade <= 0.0f) {
                continue;
            }
            const sb::WireColor wc = h.contact == 0   ? sb::WireColor{255, 60, 60}
                                     : h.contact == 1 ? sb::WireColor{230, 230, 230}
                                                      : sb::WireColor{255, 230, 60};
            const sb::UiColor c = Linear(wc, fade);
            const float r = 0.04f + 0.06f * std::clamp(h.age_s / 0.3f, 0.0f, 1.0f); // a quick pop
            capsule(p, p, r, c, 1.5f);
            const float x = 0.12f;
            out.push_back({p - Vec3{x, 0, 0}, p + Vec3{x, 0, 0}, c, 2.0f});
            out.push_back({p - Vec3{0, x, 0}, p + Vec3{0, x, 0}, c, 2.0f});
            out.push_back({p - Vec3{0, 0, x}, p + Vec3{0, 0, x}, c, 2.0f});
        }
    }
}

void GameHitboxes::Draw(sb::UiCanvas& canvas, const sb::UiDebugHitboxes& cfg,
                        const BbXrGameState& s, double now, Vec3 eye, std::string& log) {
    std::vector<HitboxLine> lines;
    Lines(cfg, s, now, lines, log);
    const float rad = cfg.line_mrad * 1e-3f;
    for (const HitboxLine& l : lines) {
        Vec3 dir = l.b - l.a;
        const float len = Length(dir);
        if (len < 1e-5f) {
            continue;
        }
        dir = dir * (1.0f / len);
        const Vec3 mid = (l.a + l.b) * 0.5f;
        const Vec3 to_eye = eye - mid;
        const float dist = std::max(Length(to_eye), 0.05f);
        Vec3 y = Cross(to_eye * (1.0f / dist), dir);
        const float yl = Length(y);
        y = yl > 1e-4f ? y * (1.0f / yl) : AnyPerp(dir);
        sb::UiPanel panel;
        panel.origin = mid;
        panel.x = dir;
        panel.y = y;
        panel.z = Cross(dir, y);
        const float width = std::max(dist * rad, 0.0008f) * l.width_scale;
        canvas.Box(panel, 0.0f, 0.0f, len + width, width, l.color);
    }
}

} // namespace vrui
