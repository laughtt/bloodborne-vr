// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "consumables.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "model.h"

namespace sb {

using namespace m3;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;
constexpr int kBulletsGoods = 900; // Quicksilver Bullets: the pool the tools draw from

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

void ElementColor(const std::string& element, float rgb[3], const float fallback[3]) {
    if (element == "fire") {
        rgb[0] = 1.0f, rgb[1] = 0.45f, rgb[2] = 0.08f;
    } else if (element == "bolt") {
        rgb[0] = 0.35f, rgb[1] = 0.6f, rgb[2] = 1.0f;
    } else if (element == "arcane") {
        rgb[0] = 0.75f, rgb[1] = 0.3f, rgb[2] = 0.95f;
    } else if (element == "gun") {
        rgb[0] = 0.95f, rgb[1] = 0.82f, rgb[2] = 0.4f;
    } else {
        rgb[0] = fallback[0], rgb[1] = fallback[1], rgb[2] = fallback[2];
    }
}

} // namespace

Consumables::~Consumables() {
    if (log_file) {
        std::fclose(log_file);
    }
}

// ---- Inventory ---------------------------------------------------------------------------------

void Consumables::Load(const std::filesystem::path& assets, const ConsumeConfig& config,
                       const std::vector<ThrowItemDef>& throw_defs, std::string& log) {
    assets_dir = assets;
    items_table = {};
    bool ok = false;
    const std::string text = ReadText(assets / "items" / "items.json", &ok);
    if (ok) {
        items_table = json::Parse(text);
    }
    entries.clear();
    for (const ConsumableDef& d : DefaultConsumables()) {
        entries.push_back({d, 0});
    }
    for (size_t i = 0; i < throw_defs.size(); ++i) {
        entries.push_back({ThrowableEntry(throw_defs[i], int(i)), 0});
    }
    // Names, durations, heal %, costs from the dump's table when it is there.
    std::vector<ConsumableDef> defs;
    for (const Entry& e : entries) {
        defs.push_back(e.def);
    }
    int matched = 0;
    if (items_table.IsObject()) {
        matched = ApplyItemsTable(defs, items_table);
        for (size_t i = 0; i < defs.size(); ++i) {
            entries[i].def = defs[i];
        }
    }
    has_mesh.assign(entries.size(), false);
    int meshes = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].def.group == CGroup::Throwable) {
            continue; // (Scene draws those with the throwables' own meshes)
        }
        const auto path = ModelPath(int(i));
        std::error_code ec;
        has_mesh[i] = !path.empty() && std::filesystem::exists(path, ec);
        meshes += has_mesh[i] ? 1 : 0;
    }
    BuildInventory(config);
    char t[256];
    std::snprintf(t, sizeof(t),
                  "consumables: %zu items, %d rows from items.json, %d extracted meshes, %zu procedural "
                  "stand-ins",
                  entries.size(), matched, meshes, entries.size() - size_t(meshes));
    log = t;
}

std::filesystem::path Consumables::ModelPath(int entry) const {
    if (entry < 0 || size_t(entry) >= entries.size() || entries[size_t(entry)].def.mesh.empty() ||
        entries[size_t(entry)].def.group == CGroup::Throwable) {
        return {};
    }
    return assets_dir / "consumables" / (entries[size_t(entry)].def.mesh + ".json");
}

bool Consumables::HasMesh(int entry) const {
    return entry >= 0 && size_t(entry) < has_mesh.size() && has_mesh[size_t(entry)];
}

void Consumables::BuildInventory(const ConsumeConfig& config) {
    inventory_signature = ThrowItemsSignature(config.inventory);
    for (Entry& e : entries) {
        e.count = 0;
    }
    if (config.inventory.IsObject()) {
        for (const auto& kv : config.inventory.object) {
            const int goods = std::atoi(kv.first.c_str());
            const int i = FindEntry(goods);
            if (i >= 0) {
                entries[size_t(i)].count = std::max(kv.second.Int(0), 0);
            } else if (goods == kBulletsGoods) {
                // (the bullet pool is an entry-less count: kept below)
            }
        }
    } else {
        for (const auto& kv : SampleInventory()) {
            const int i = FindEntry(kv.first);
            if (i >= 0) {
                entries[size_t(i)].count = kv.second;
            }
        }
    }
    // The bullet pool lives in a hidden entry.
    int pool = FindEntry(kBulletsGoods);
    if (pool < 0) {
        Entry e;
        e.def.goods = kBulletsGoods;
        e.def.name = "Quicksilver Bullets";
        e.def.group = CGroup::Tool;
        e.def.gesture = CGesture::Head;
        e.def.shape = "pellet";
        entries.push_back(e);
        has_mesh.push_back(false);
        pool = int(entries.size()) - 1;
    }
    int bullets = 30;
    if (config.inventory.IsObject()) {
        bullets = config.inventory[std::to_string(kBulletsGoods)].Int(0);
    }
    entries[size_t(pool)].count = bullets;
}

void Consumables::Reconfigure(const ConsumeConfig& config) {
    if (ThrowItemsSignature(config.inventory) != inventory_signature) {
        BuildInventory(config);
    }
}

int Consumables::FindEntry(int goods) const {
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].def.goods == goods) {
            return int(i);
        }
    }
    return -1;
}

int Consumables::Count(int goods) const {
    const int i = FindEntry(goods);
    return i < 0 ? 0 : entries[size_t(i)].count;
}

void Consumables::SetCount(int goods, int count) {
    const int i = FindEntry(goods);
    if (i >= 0) {
        entries[size_t(i)].count = std::max(count, 0);
    }
}

void Consumables::AddAilment(const std::string& name) {
    if (std::find(ailments.begin(), ailments.end(), name) == ailments.end()) {
        ailments.push_back(name);
    }
}

std::string Consumables::Label(int entry) const {
    const Entry& e = entries[size_t(entry)];
    if (!e.def.consumed) {
        return "~";
    }
    return "x" + std::to_string(e.count);
}

int Consumables::ShownCount() const {
    int n = 0;
    for (const Entry& e : entries) {
        n += (e.count > 0 && e.def.goods != kBulletsGoods) ? 1 : 0;
    }
    return n;
}

bool Consumables::SlotVisible(int slot) const {
    if (slot < 0 || size_t(slot) >= slots.size()) {
        return false;
    }
    const int e = slots[size_t(slot)].entry;
    int taken = 0;
    for (const Held& h : held) {
        taken += h.entry == e ? 1 : 0;
    }
    const Entry& entry = entries[size_t(e)];
    // A stack shows while one is left that no hand holds.
    return entry.count - taken > 0;
}

Quat Consumables::SlotRot(int slot) const {
    return slots[size_t(slot)].rot;
}

Vec3 Consumables::SlotPos(int slot, float bob_m) const {
    const Slot& s = slots[size_t(slot)];
    return s.pos + Vec3{0.0f, bob_m * std::sin(float(now) * 1.6f + s.phase), 0.0f};
}

// ---- The wheel ---------------------------------------------------------------------------------

void Consumables::LayoutWheel(const ConsumeInput& in, const ConsumeConfig& config) {
    slots.clear();
    std::vector<int> shown;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].count > 0 && entries[i].def.goods != kBulletsGoods &&
            (config.throwables_on_wheel || entries[i].def.group != CGroup::Throwable)) {
            shown.push_back(int(i));
        }
    }
    // Order: throwables, weapon buffs, healing, cures, body buffs, tools (by the enum), then
    // by goods id.
    std::stable_sort(shown.begin(), shown.end(), [&](int a, int b) {
        const ConsumableDef &da = entries[size_t(a)].def, &db = entries[size_t(b)].def;
        if (da.group != db.group) {
            return int(da.group) < int(db.group);
        }
        return da.goods < db.goods;
    });
    const int n = int(shown.size());
    const float yaw0 = YawOf(in.head_rotation);
    const float down = config.down_deg * kDeg;
    const float reach = config.distance_m * std::cos(down); // horizontal distance from the head
    const float drop = config.distance_m * std::sin(down);
    const int rows = (n + config.per_row - 1) / std::max(config.per_row, 1);
    int index = 0;
    for (int r = 0; r < rows; ++r) {
        const int in_row = std::min(config.per_row, n - r * config.per_row);
        for (int c = 0; c < in_row; ++c, ++index) {
            const float u = float(c) - 0.5f * float(in_row - 1);
            Slot s;
            s.entry = shown[size_t(index)];
            s.phase = float(index) * 0.7f;
            float yaw = yaw0;
            Vec3 p;
            if (config.layout == "shelf") {
                const Vec3 right = Rotate(AxisAngle({0, 1, 0}, yaw0), Vec3{1, 0, 0});
                const Vec3 fwd = Rotate(AxisAngle({0, 1, 0}, yaw0), Vec3{0, 0, -1});
                p = in.head_position + fwd * reach + right * (u * config.spacing_m);
            } else {
                const float angle = -u * config.spacing_m / std::max(reach, 0.05f);
                yaw = yaw0 + angle;
                p = in.head_position + Rotate(AxisAngle({0, 1, 0}, yaw), Vec3{0, 0, -1}) * reach;
            }
            p.y = in.head_position.y - drop - float(r) * config.row_spacing_m;
            if (in.floor_y > -1e8f) {
                p.y = std::max(p.y, in.floor_y + 0.25f);
            }
            s.pos = p;
            const ConsumableDef& d = entries[size_t(s.entry)].def;
            Vec3 tilt{0.0f, 0.0f, 0.0f};
            if (d.shape == "paper") {
                tilt = {75.0f, 0.0f, 0.0f}; // flat sheets face the player
            } else if (d.shape == "knife") {
                tilt = {0.0f, 0.0f, 90.0f};
            }
            s.rot = AxisAngle({0, 1, 0}, yaw) * FromEulerDeg(tilt);
            slots.push_back(s);
        }
    }
}

// ---- Events --------------------------------------------------------------------------------------

void Consumables::Pulse(ConsumeEvents& events, int hand, float amplitude, float seconds) {
    events.haptic_amplitude[hand] = std::max(events.haptic_amplitude[hand], amplitude);
    events.haptic_seconds[hand] = std::max(events.haptic_seconds[hand], seconds);
}

void Consumables::Emit(const ConsumeConfig& config, ConsumeEvents& events, const std::string& text,
                       const std::string& json) {
    events.log += "consumable: " + text + "\n";
    event_log.push_back(text);
    if (event_log.size() > 400) {
        event_log.erase(event_log.begin(), event_log.begin() + 200);
    }
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

namespace {
std::string Ident(const ConsumableDef& d, int hand) {
    return "\"item\": \"" + Escape(d.name) + "\", \"goods\": " + std::to_string(d.goods) +
           ", \"group\": \"" + GroupName(d.group) + "\", \"hand\": " + std::to_string(hand);
}
} // namespace

bool Consumables::WeaponStowed(int side, const ConsumeConfig& config) const {
    if (!config.Objects() || !config.stow_weapons) {
        return false;
    }
    // The weapon a held paper is rubbed along stays out.
    const Held& other = held[1 - side];
    if (other.entry >= 0 && !other.via_throwables && entries[size_t(other.entry)].def.gesture == CGesture::Rub &&
        other.target == side) {
        return false;
    }
    return open || held[side].entry >= 0;
}

bool Consumables::CanUse(const ConsumableDef& d) const {
    const int i = FindEntry(d.goods);
    if (i < 0 || entries[size_t(i)].count <= 0) {
        return false;
    }
    return d.bullet_cost <= 0 || Count(kBulletsGoods) >= d.bullet_cost;
}

bool Consumables::PayFor(const ConsumableDef& d) {
    if (!CanUse(d)) {
        return false;
    }
    const int i = FindEntry(d.goods);
    if (d.consumed) {
        entries[size_t(i)].count -= 1;
    }
    if (d.bullet_cost > 0) {
        SetCount(kBulletsGoods, Count(kBulletsGoods) - d.bullet_cost);
    }
    return true;
}

// ---- Taking, putting back, using ---------------------------------------------------------------

bool Consumables::Take(int hand, int slot, const ConsumeInput& in, const ConsumeConfig& config,
                       Throwables& throwables, const ThrowConfig& tc, const ThrowInput& ti,
                       ThrowEvents& te, ConsumeEvents& events) {
    const int e = slots[size_t(slot)].entry;
    const ConsumableDef& d = entries[size_t(e)].def;
    if (!CanUse(d)) {
        Pulse(events, hand, 0.5f, 0.12f);
        events.toast_title = d.name;
        events.toast_body = "Not enough quicksilver bullets (" + std::to_string(d.bullet_cost) + ")";
        Emit(config, events, d.name + ": not enough quicksilver bullets",
             "\"event\": \"refused\", " + Ident(d, hand) + ", \"why\": \"bullets\"");
        return false;
    }
    Held& h = held[hand];
    if (d.gesture == CGesture::Throw) {
        ThrowEvents local;
        if (!throwables.TakeFor(hand, d.goods, ti, tc, local)) {
            Pulse(events, hand, 0.4f, 0.08f);
            return false;
        }
        te.haptic_amplitude[hand] = std::max(te.haptic_amplitude[hand], local.haptic_amplitude[hand]);
        te.haptic_seconds[hand] = std::max(te.haptic_seconds[hand], local.haptic_seconds[hand]);
        te.log += local.log;
        h = Held{};
        h.entry = e;
        h.via_throwables = true;
        h.since = in.time;
        h.pos = ti.grip_position[hand];
        h.rot = ti.grip_rotation[hand];
    } else {
        h = Held{};
        h.entry = e;
        h.since = in.time;
        h.target = 1 - hand; // a paper is rubbed onto the weapon of the other hand
        h.pos = TransformPoint(FromPose(in.grip_rotation[hand], in.grip_position[hand]), d.hold);
        h.rot = in.grip_rotation[hand] * FromEulerDeg(d.hold_rot_deg);
        Pulse(events, hand, 0.5f, 0.05f);
    }
    if (d.gesture == CGesture::Rub && !config.buff_rub) {
        // Instant: the weapon in the other hand (the right one when that hand has none) gets it now.
        const auto equipped = [&](int s) { return in.weapon_equipped[s] || in.weapon_valid[s]; };
        h.target = equipped(1 - hand) ? 1 - hand : (equipped(1) ? 1 : 1 - hand);
        h.rub_cover = 1.0f;
        Emit(config, events,
             std::string("picked ") + d.name + " with the " + (hand ? "right" : "left") + " hand (instant buff)",
             "\"event\": \"pick\", " + Ident(d, hand) + ", \"gesture\": \"instant\", \"count\": " +
                 std::to_string(entries[size_t(e)].count));
        ApplyBuff(hand, in, config, events);
        h = Held{};
        return true;
    }
    if (d.gesture == CGesture::Rub) {
        const int side = 1 - hand;
        char blade[160];
        std::snprintf(blade, sizeof(blade), "%s weapon %s, blade %.2f m", side ? "right" : "left",
                      in.weapon_valid[side] ? "present" : "MISSING",
                      in.weapon_valid[side] ? double(Length(in.weapon_tip[side] - in.weapon_base[side])) : 0.0);
        events.toast_title = d.name;
        events.toast_body = std::string("Rub it along the ") + (side ? "right" : "left") + " weapon";
        Emit(config, events, std::string("rub target: ") + blade,
             "\"event\": \"rub_target\", " + Ident(d, hand) + ", \"side\": " + std::to_string(side) +
                 ", \"weapon\": " + (in.weapon_valid[side] ? "true" : "false"));
    }
    Emit(config, events,
         std::string("picked ") + d.name + " with the " + (hand ? "right" : "left") + " hand (" +
             GestureName(d.gesture) + ")",
         "\"event\": \"pick\", " + Ident(d, hand) + ", \"gesture\": \"" + GestureName(d.gesture) +
             "\", \"count\": " + std::to_string(entries[size_t(e)].count));
    return false;
}

void Consumables::PutBack(int hand, const char* why, const ConsumeConfig& config,
                          ConsumeEvents& events) {
    Held& h = held[hand];
    if (h.entry < 0) {
        return;
    }
    const ConsumableDef& d = entries[size_t(h.entry)].def;
    Emit(config, events, std::string("put back ") + d.name + " (" + why + ")",
         "\"event\": \"put_back\", " + Ident(d, hand) + ", \"why\": \"" + why + "\"");
    h = Held{};
}

void Consumables::Use(int hand, const ConsumeConfig& config, ConsumeEvents& events,
                      const char* how) {
    Held& h = held[hand];
    const int e = h.entry;
    const ConsumableDef d = entries[size_t(e)].def;
    if (!PayFor(d)) { // (bullets spent in the meantime)
        h.blocked = true;
        Pulse(events, hand, 0.5f, 0.12f);
        events.toast_title = d.name;
        events.toast_body = "Not enough quicksilver bullets";
        return;
    }
    std::string text = d.status.empty() ? d.name : d.status;
    std::string detail;
    switch (d.group) {
    case CGroup::Heal: {
        const float before = hp;
        hp = std::min(100.0f, hp + d.heal_pct);
        last_heal = now;
        last_heal_amount = hp - before;
        text = d.name + ": HP " + F(before) + " -> " + F(hp) + " %";
        detail = ", \"hp_before\": " + F(before) + ", \"hp_after\": " + F(hp);
        break;
    }
    case CGroup::Cure: {
        const auto it = std::find(ailments.begin(), ailments.end(), d.cure);
        if (it != ailments.end()) {
            ailments.erase(it);
            text = d.name + ": " + d.cure + " cured";
        } else {
            text = d.name + ": no " + d.cure + " to cure";
        }
        detail = ", \"cure\": \"" + d.cure + "\"";
        break;
    }
    case CGroup::BodyBuff: {
        if (d.insight > 0) {
            insight += d.insight;
            text = d.name + ": insight +" + std::to_string(d.insight) + " (" + std::to_string(insight) + ")";
        }
        if (d.duration_s > 0.0f) {
            BodyStatus s;
            s.goods = d.goods;
            s.name = d.name;
            for (int k = 0; k < 3; ++k) {
                s.color[k] = d.color[k];
            }
            s.t0 = now;
            s.duration = d.duration_s;
            s.until = now + double(d.duration_s);
            statuses.erase(std::remove_if(statuses.begin(), statuses.end(),
                                          [&](const BodyStatus& o) { return o.goods == d.goods; }),
                           statuses.end());
            statuses.push_back(s);
            text = d.status.empty() ? d.name : d.status;
            text += " (" + F(d.duration_s) + " s)";
            detail += ", \"duration_s\": " + F(d.duration_s);
        }
        break;
    }
    case CGroup::Tool:
        if (d.gesture == CGesture::Crush) {
            text = d.status;
        }
        break;
    default:
        break;
    }
    Emit(config, events, std::string("used ") + d.name + " (" + how + "): " + text,
         "\"event\": \"use\", " + Ident(d, hand) + ", \"how\": \"" + how + "\", \"count\": " +
             std::to_string(entries[size_t(e)].count) + detail);
    events.toast_title = d.name;
    events.toast_body = text;
    if (d.group == CGroup::Heal) {
        Pulse(events, hand, 0.8f, 0.15f);
    } else if (d.gesture == CGesture::Crush) {
        Pulse(events, hand, 1.0f, 0.2f);
    } else if (d.shape == "bell") {
        Pulse(events, hand, 0.9f, 0.25f);
    } else {
        Pulse(events, hand, 0.6f, 0.1f);
    }
    h = Held{};
}

// ---- Held items --------------------------------------------------------------------------------

void Consumables::StepRub(int hand, const ConsumeInput& in, const ConsumeConfig& config,
                          ConsumeEvents& events) {
    Held& h = held[hand];
    const ConsumableDef& d = entries[size_t(h.entry)].def;
    const int side = h.target >= 0 ? h.target : 1 - hand;
    const bool diag = config.diag_interval_s > 0.0f && now - h.last_diag >= double(config.diag_interval_s);
    const auto Diag = [&](const std::string& why, float dist, float t, float length) {
        if (!diag) {
            return;
        }
        h.last_diag = now;
        char text[256];
        std::snprintf(text, sizeof(text),
                      "rub %s: %s (dist %.1f cm, along %.0f %%, blade %.2f m, covered %.0f %% of %.0f %%, "
                      "weapon side %d)",
                      d.name.c_str(), why.c_str(), double(dist * 100.0f), double(t * 100.0f),
                      double(length), double(h.rub_cover * 100.0f), double(config.coverage * 100.0f), side);
        Emit(config, events, text,
             "\"event\": \"rub_diag\", " + Ident(d, hand) + ", \"why\": \"" + Escape(why) + "\", \"dist\": " +
                 F(dist) + ", \"along\": " + F(t) + ", \"blade_m\": " + F(length) + ", \"cover\": " +
                 F(h.rub_cover) + ", \"side\": " + std::to_string(side));
    };
    if (!in.weapon_valid[side]) {
        h.rub.clear();
        h.rub_cover = 0.0f;
        h.rub_lo = 1.0f, h.rub_hi = 0.0f;
        Diag(std::string("no weapon in the ") + (side ? "right" : "left") + " hand to rub", -1.0f, 0.0f, 0.0f);
        return;
    }
    const Vec3 base = in.weapon_base[side], tip = in.weapon_tip[side];
    const Vec3 seg = tip - base;
    const float length = Length(seg);
    if (length < 0.05f) {
        Diag("weapon segment too short", 0.0f, 0.0f, length);
        return;
    }
    const Vec3 axis = seg * (1.0f / length);
    // The paper's contact point is the item, or the hand itself (whichever is closer to the blade).
    const Vec3 hand_point = in.grip_position[hand];
    float best_dist = 1e9f, best_raw = 0.0f;
    for (const Vec3& p : {h.pos, hand_point}) {
        const float along = Dot(p - base, axis);
        const Vec3 closest = base + axis * std::clamp(along, 0.0f, length);
        const float dist = Length(p - closest);
        if (dist < best_dist) {
            best_dist = dist;
            best_raw = along / length;
        }
    }
    const float raw = best_raw, dist = best_dist;
    const bool in_range = raw >= config.blade_start - 0.05f && raw <= 1.1f;
    const bool contact = dist <= config.contact_radius_m && in_range;
    if (contact) {
        if (now - h.last_contact > double(config.contact_gap_s)) {
            h.rub.clear();
        }
        const float t = std::clamp(raw, config.blade_start, 1.0f);
        float speed = 0.0f;
        if (!h.rub.empty() && now > h.rub.back().first) {
            speed = std::fabs(t - h.rub.back().second) * length / float(now - h.rub.back().first);
        }
        h.rub.emplace_back(now, t);
        h.last_contact = now;
        while (!h.rub.empty() && now - h.rub.front().first > double(config.window_s)) {
            h.rub.erase(h.rub.begin());
        }
        float lo = 1.0f, hi = 0.0f;
        for (const auto& s : h.rub) {
            lo = std::min(lo, s.second);
            hi = std::max(hi, s.second);
        }
        h.rub_lo = lo, h.rub_hi = hi;
        h.rub_cover = h.rub.size() >= 2 ? std::clamp((hi - lo) / std::max(1.0f - config.blade_start, 0.05f), 0.0f, 1.0f) : 0.0f;
        if (speed >= 0.05f && now - h.last_pulse >= 0.05) {
            h.last_pulse = now;
            Pulse(events, hand, 0.2f + 0.4f * h.rub_cover, 0.03f);
            Pulse(events, side, 0.12f + 0.25f * h.rub_cover, 0.03f);
        }
        Diag(speed < 0.05f ? "in contact, move along the blade" : "rubbing", dist, raw, length);
        if (h.rub_cover >= config.coverage) {
            ApplyBuff(hand, in, config, events);
        }
    } else {
        if (now - h.last_contact > double(config.contact_gap_s)) {
            h.rub.clear();
            h.rub_cover = 0.0f;
            h.rub_lo = 1.0f, h.rub_hi = 0.0f;
        }
        Diag(dist > config.contact_radius_m ? "too far from the blade" : "beside the handle / past the tip",
             dist, raw, length);
    }
}

void Consumables::ApplyBuff(int hand, const ConsumeInput& in, const ConsumeConfig& config,
                            ConsumeEvents& events) {
    (void)in;
    Held& h = held[hand];
    const ConsumableDef d = entries[size_t(h.entry)].def;
    const int side = h.target >= 0 ? h.target : 1 - hand;
    const float cover = h.rub_cover;
    if (!PayFor(d)) {
        h.blocked = true;
        h.rub.clear();
        return;
    }
    WeaponBuff& b = buffs[side];
    b.active = true;
    b.goods = d.goods;
    b.name = d.name;
    b.element = d.element;
    ElementColor(d.element, b.color, d.color);
    b.t0 = now;
    b.duration = d.duration_s > 0.0f ? d.duration_s : config.buff_default_s;
    b.until = now + double(b.duration);
    const std::string text = d.name + ": " + d.status + " on the " + (side ? "right" : "left") +
                             " weapon for " + F(b.duration) + " s";
    Emit(config, events, "buff applied: " + text,
         "\"event\": \"buff\", " + Ident(d, hand) + ", \"side\": " + std::to_string(side) +
             ", \"element\": \"" + d.element + "\", \"duration_s\": " + F(b.duration) +
             ", \"coverage\": " + F(cover) + ", \"count\": " +
             std::to_string(entries[size_t(h.entry)].count));
    events.toast_title = d.name;
    events.toast_body = d.status + " for " + F(b.duration) + " s";
    Pulse(events, hand, 0.8f, 0.15f);
    Pulse(events, side, 0.8f, 0.2f);
    h = Held{};
}

void Consumables::StepHeld(int hand, const ConsumeInput& in, const ConsumeConfig& config,
                           ConsumeEvents& events) {
    Held& h = held[hand];
    const ConsumableDef& d = entries[size_t(h.entry)].def;
    const bool valid = in.hand_valid[hand];
    if (!valid) {
        PutBack(hand, "tracking lost", config, events);
        return;
    }
    h.pos = TransformPoint(FromPose(in.grip_rotation[hand], in.grip_position[hand]), d.hold);
    h.rot = in.grip_rotation[hand] * FromEulerDeg(d.hold_rot_deg);
    if (in.squeeze[hand] < config.release_squeeze) {
        PutBack(hand, "released", config, events);
        return;
    }
    switch (d.gesture) {
    case CGesture::Head: {
        if (!in.head_valid) {
            break;
        }
        const Vec3 mouth = in.head_position + Rotate(in.head_rotation, config.mouth);
        const float to_mouth = Length(h.pos - mouth);
        const bool below = h.pos.y <= in.head_position.y - config.below_eyes_m;
        const bool near = to_mouth <= config.mouth_radius_m && below;
        if (config.diag_interval_s > 0.0f && to_mouth < 0.45f && now - h.last_diag >= double(config.diag_interval_s)) {
            h.last_diag = now;
            char text[200];
            std::snprintf(text, sizeof(text), "head %s: %.1f cm from the mouth (limit %.0f), %s the eyes, held %.2f s",
                          d.name.c_str(), double(to_mouth * 100.0f), double(config.mouth_radius_m * 100.0f),
                          below ? "below" : "ABOVE", h.head_since >= 0.0 ? now - h.head_since : 0.0);
            Emit(config, events, text,
                 "\"event\": \"head_diag\", " + Ident(d, hand) + ", \"mouth_dist\": " + F(to_mouth) +
                     ", \"below_eyes\": " + (below ? "true" : "false") + ", \"head\": " + V(in.head_position));
        }
        if (!near) {
            h.head_since = -1.0;
            h.blocked = false;
        } else if (!h.blocked) {
            if (h.head_since < 0.0) {
                h.head_since = now;
                Pulse(events, hand, 0.2f, 0.03f);
            }
            if (now - h.head_since >= double(config.head_hold_s)) {
                Use(hand, config, events, "head");
            }
        }
        break;
    }
    case CGesture::Crush: {
        if (in.squeeze[hand] >= config.crush_squeeze) {
            if (h.crush_since < 0.0) {
                h.crush_since = now;
            }
            const float t = float((now - h.crush_since) / std::max(double(config.crush_hold_s), 1e-3));
            Pulse(events, hand, 0.1f + 0.5f * std::clamp(t, 0.0f, 1.0f), 0.03f);
            if (now - h.crush_since >= double(config.crush_hold_s)) {
                Use(hand, config, events, "crush");
            }
        } else {
            h.crush_since = -1.0;
        }
        break;
    }
    case CGesture::Rub:
        StepRub(hand, in, config, events);
        break;
    case CGesture::Throw:
        break;
    }
}

void Consumables::CloseWheel(const ConsumeConfig& config, ConsumeEvents& events, const char* why) {
    if (!open) {
        return;
    }
    open = false;
    slots.clear();
    Emit(config, events, std::string("wheel closed (") + why + ")",
         std::string("\"event\": \"wheel_close\", \"why\": \"") + why + "\"");
}

// ---- Step --------------------------------------------------------------------------------------

void Consumables::Step(const ConsumeInput& in, const ConsumeConfig& config, Throwables& throwables,
                       const ThrowConfig& tc, const ThrowInput& ti, ThrowEvents& te,
                       ConsumeEvents& events) {
    now = in.time;
    if (!hp_init) {
        hp_init = true;
        hp = config.hp_start_pct;
    }
    if (!config.Objects()) {
        if (open || held[0].entry >= 0 || held[1].entry >= 0) {
            open = false;
            slots.clear();
            held[0] = held[1] = Held{};
        }
        return;
    }
    Reconfigure(config);
    // Throws: the ones Throwables made since the last frame use their item up.
    const std::vector<int>& thrown = throwables.ThrownGoods();
    for (; thrown_seen < thrown.size(); ++thrown_seen) {
        const int i = FindEntry(thrown[thrown_seen]);
        if (i < 0 || !config.throwables_on_wheel) { // (the table's throwables are not counted)
            continue;
        }
        const ConsumableDef d = entries[size_t(i)].def;
        PayFor(d);
        Emit(config, events, "thrown " + d.name + ", " + std::to_string(entries[size_t(i)].count) + " left",
             "\"event\": \"throw\", " + Ident(d, -1) + ", \"count\": " + std::to_string(entries[size_t(i)].count));
    }
    // Timed things run out.
    for (int side = 0; side < 2; ++side) {
        if (buffs[side].active && now >= buffs[side].until) {
            Emit(config, events, buffs[side].name + " wore off the " + (side ? "right" : "left") + " weapon",
                 "\"event\": \"buff_end\", \"item\": \"" + Escape(buffs[side].name) + "\", \"side\": " +
                     std::to_string(side));
            buffs[side] = WeaponBuff{};
        }
    }
    for (size_t i = 0; i < statuses.size();) {
        if (now >= statuses[i].until) {
            Emit(config, events, statuses[i].name + " wore off",
                 "\"event\": \"status_end\", \"item\": \"" + Escape(statuses[i].name) + "\"");
            statuses.erase(statuses.begin() + long(i));
        } else {
            ++i;
        }
    }
    // The wheel: toggle mode opens on a press and stays until an item is taken or the button is
    // pressed again; hold mode is open while the button is held (plus the linger).
    const uint32_t button = ThrowButton(config.open_button);
    const bool down = button != 0 && (in.buttons & button) != 0;
    const bool pressed = down && !button_was;
    const bool toggle = config.wheel_mode == "toggle";
    const auto open_wheel = [&]() {
        if (!open && !in.head_valid) { // (a press while the head is not tracked is kept for a second)
            pending_open = true;
            pending_since = now;
            Emit(config, events, "R3 pressed but the head is not tracked: the wheel opens when it is",
                 "\"event\": \"wheel_wait\"");
        }
        if (!open && in.head_valid) {
            pending_open = false;
            LayoutWheel(in, config);
            open = true;
            Emit(config, events, "wheel opened: " + std::to_string(slots.size()) + " items",
                 "\"event\": \"wheel_open\", \"items\": " + std::to_string(slots.size()));
        }
    };
    if (pressed) {
        Emit(config, events, std::string("R3 pressed, wheel ") + (open ? "open" : "shut"),
             std::string("\"event\": \"button\", \"wheel_open\": ") + (open ? "true" : "false"));
    }
    if (pending_open && (open || now - pending_since > 1.0)) {
        pending_open = false;
    } else if (pending_open && in.head_valid) {
        open_wheel();
    }
    if (toggle) {
        if (pressed) {
            if (open) {
                CloseWheel(config, events, "button");
            } else {
                open_wheel();
            }
        }
    } else {
        if (pressed) {
            open_wheel();
        }
        if (!down && button_was) {
            closed_at = now;
        }
        if (open && !down && now - closed_at >= double(config.linger_s)) {
            CloseWheel(config, events, "released");
        }
    }
    button_was = down;
    // Hands.
    for (int hand = 0; hand < 2; ++hand) {
        Held& h = held[hand];
        if (h.entry >= 0 && h.via_throwables) {
            if (throwables.HeldItem(hand) < 0) { // thrown or dropped: Throwables has it now
                h = Held{};
            } else {
                h.pos = throwables.Items()[size_t(throwables.HeldItem(hand))].pos;
            }
        } else if (h.entry >= 0) {
            StepHeld(hand, in, config, events);
        }
        const bool valid = in.hand_valid[hand];
        const bool edge = valid && in.squeeze[hand] >= config.grab_squeeze &&
                          squeeze_prev[hand] < config.grab_squeeze;
        hover[hand] = -1;
        if (held[hand].entry < 0 && throwables.HeldItem(hand) < 0 && valid && open) {
            const Vec3 point = TransformPoint(FromPose(in.grip_rotation[hand], in.grip_position[hand]), config.grab_point);
            int best = -1;
            float best_d = config.grab_radius_m;
            for (size_t s = 0; s < slots.size(); ++s) {
                if (!SlotVisible(int(s))) {
                    continue;
                }
                const float dist = Length(SlotPos(int(s), 0.0f) - point);
                if (dist <= best_d) {
                    best_d = dist;
                    best = int(s);
                }
            }
            if (best >= 0 && best != last_hover[hand]) {
                Pulse(events, hand, 0.15f, 0.02f);
            }
            hover[hand] = best;
            if (best >= 0 && edge) { // (closing the hand on an object; not just entering it closed)
                const bool used = Take(hand, best, in, config, throwables, tc, ti, te, events);
                if ((held[hand].entry >= 0 || used) && toggle && config.close_on_grab) {
                    CloseWheel(config, events, "item taken");
                }
            }
        }
        last_hover[hand] = hover[hand];
        squeeze_prev[hand] = valid ? in.squeeze[hand] : 0.0f;
    }
}

// ---- Glyphs ------------------------------------------------------------------------------------

void AppendGlyphs(const std::string& text, Vec3 origin, Vec3 right, Vec3 up, float height,
                  const GlyphLineFn& line) {
    // Segments of a 7-segment cell: a top, b top right, c bottom right, d bottom, e bottom left,
    // f top left, g middle.
    static const int kDigits[10] = {0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
                                    0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111};
    const float w = height * 0.55f, step = height * 0.8f;
    Vec3 o = origin;
    for (char ch : text) {
        const auto P = [&](float x, float y) { return o + right * (x * w) + up * (y * height); };
        if (ch >= '0' && ch <= '9') {
            const int m = kDigits[ch - '0'];
            if (m & 1) line(P(0, 1), P(1, 1));
            if (m & 2) line(P(1, 1), P(1, 0.5f));
            if (m & 4) line(P(1, 0.5f), P(1, 0));
            if (m & 8) line(P(0, 0), P(1, 0));
            if (m & 16) line(P(0, 0.5f), P(0, 0));
            if (m & 32) line(P(0, 1), P(0, 0.5f));
            if (m & 64) line(P(0, 0.5f), P(1, 0.5f));
        } else if (ch == 'x') {
            line(P(0, 0.6f), P(1, 0.1f));
            line(P(0, 0.1f), P(1, 0.6f));
        } else if (ch == '~') { // infinity: two diamonds
            line(P(0.0f, 0.5f), P(0.25f, 0.8f));
            line(P(0.25f, 0.8f), P(0.5f, 0.5f));
            line(P(0.5f, 0.5f), P(0.25f, 0.2f));
            line(P(0.25f, 0.2f), P(0.0f, 0.5f));
            line(P(0.5f, 0.5f), P(0.75f, 0.8f));
            line(P(0.75f, 0.8f), P(1.0f, 0.5f));
            line(P(1.0f, 0.5f), P(0.75f, 0.2f));
            line(P(0.75f, 0.2f), P(0.5f, 0.5f));
        }
        o = o + right * step;
    }
}

} // namespace sb
