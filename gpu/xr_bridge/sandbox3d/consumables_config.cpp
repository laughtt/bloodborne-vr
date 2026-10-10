// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "consumables_config.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace sb {
namespace {

void Read(const json::Value& v, float& out) {
    if (v.IsNumber()) {
        out = v.Float();
    }
}
void Read(const json::Value& v, int& out) {
    if (v.IsNumber()) {
        out = v.Int();
    }
}
void Read(const json::Value& v, bool& out) {
    if (!v.IsNull()) {
        out = v.Bool(out);
    }
}
void Read(const json::Value& v, std::string& out) {
    if (v.IsString()) {
        out = v.Str();
    }
}
void Read(const json::Value& v, m3::Vec3& out) {
    if (v.IsArray() && v.Size() >= 3) {
        out = {v[0].Float(), v[1].Float(), v[2].Float()};
    }
}

ConsumableDef Def(int goods, const char* name, CGroup group, CGesture gesture, const char* shape,
                  float r, float g, float b, int sample) {
    ConsumableDef d;
    d.goods = goods;
    d.name = name;
    d.group = group;
    d.gesture = gesture;
    d.shape = shape;
    d.color[0] = r, d.color[1] = g, d.color[2] = b;
    d.sample_count = sample;
    return d;
}

} // namespace

const char* GroupName(CGroup g) {
    switch (g) {
    case CGroup::Throwable: return "throwable";
    case CGroup::WeaponBuff: return "weapon_buff";
    case CGroup::Heal: return "heal";
    case CGroup::Cure: return "cure";
    case CGroup::BodyBuff: return "body_buff";
    case CGroup::Tool: return "tool";
    }
    return "?";
}

const char* GestureName(CGesture g) {
    switch (g) {
    case CGesture::Throw: return "throw";
    case CGesture::Rub: return "rub";
    case CGesture::Head: return "head";
    case CGesture::Crush: return "crush";
    }
    return "?";
}

std::vector<ConsumableDef> DefaultConsumables() {
    using G = CGroup;
    using K = CGesture;
    std::vector<ConsumableDef> v;
    // ---- Weapon buffs: the paper is picked with the hand that does not hold the weapon and
    // rubbed along it. The element and the +80 come from SpEffectParam 2200/2210/2220/2230.
    {
        ConsumableDef d = Def(1300, "Fire Paper", G::WeaponBuff, K::Rub, "paper", 1.0f, 0.45f, 0.1f, 10);
        d.element = "fire";
        d.status = "Fire +80";
        d.duration_s = 60.0f;
        v.push_back(d);
        d = Def(1320, "Bolt Paper", G::WeaponBuff, K::Rub, "paper", 0.35f, 0.6f, 1.0f, 6);
        d.element = "bolt";
        d.status = "Lightning +80";
        d.duration_s = 60.0f;
        v.push_back(d);
        d = Def(1310, "Empty Phantasm Shell", G::WeaponBuff, K::Rub, "pellet", 0.7f, 0.3f, 0.95f, 1);
        d.element = "arcane";
        d.status = "Arcane +80";
        d.duration_s = 60.0f;
        d.consumed = false;
        d.bullet_cost = 3;
        d.scale = 2.2f;
        v.push_back(d);
        // Bone Marrow Ash is the *gun* buff (SpEffect 2230 "weapon buff [gun]", wepParamChange 2):
        // the right hand takes it and dusts the left weapon.
        d = Def(1330, "Bone Marrow Ash", G::WeaponBuff, K::Rub, "packet", 0.9f, 0.78f, 0.45f, 4);
        d.element = "gun";
        d.target_side = 0;
        d.status = "Gun buff";
        d.duration_s = 60.0f;
        v.push_back(d);
    }
    // ---- Healing.
    {
        ConsumableDef d = Def(1000, "Blood Vial", G::Heal, K::Head, "vial", 0.75f, 0.04f, 0.07f, 20);
        d.mesh = "cs_1000";
        d.heal_pct = 40.0f;
        d.status = "Blood Vial";
        v.push_back(d);
        d = Def(703, "Iosefka's Blood Vial", G::Heal, K::Head, "vial", 0.8f, 0.2f, 0.25f, 1);
        d.mesh = "cs_703";
        d.heal_pct = 70.0f;
        v.push_back(d);
        d = Def(706, "Blood of Adeline", G::Heal, K::Head, "vial", 0.65f, 0.05f, 0.12f, 1);
        d.mesh = "cs_706";
        d.heal_pct = 50.0f;
        v.push_back(d);
    }
    // ---- Cures.
    {
        ConsumableDef d = Def(1100, "Antidote", G::Cure, K::Head, "syringe", 0.4f, 0.85f, 0.35f, 4);
        d.cure = "poison";
        d.status = "Poison cured";
        v.push_back(d);
        d = Def(1101, "Sedative", G::Cure, K::Head, "syringe", 0.55f, 0.7f, 0.95f, 3);
        d.cure = "frenzy";
        d.status = "Frenzy calmed";
        v.push_back(d);
    }
    // ---- Body buffs.
    {
        ConsumableDef d = Def(1110, "Beast Blood Pellet", G::BodyBuff, K::Head, "pellet", 0.55f, 0.05f, 0.08f, 3);
        d.duration_s = 60.0f;
        d.status = "Beast blood: bloodlust";
        v.push_back(d);
        d = Def(1120, "Blue Elixir", G::BodyBuff, K::Head, "flask", 0.25f, 0.5f, 1.0f, 2);
        d.duration_s = 30.0f;
        d.status = "Blue Elixir: quiet steps";
        v.push_back(d);
        d = Def(2030, "Lead Elixir", G::BodyBuff, K::Head, "flask", 0.55f, 0.58f, 0.62f, 2);
        d.mesh = "cs_2030";
        d.duration_s = 60.0f;
        d.status = "Lead Elixir: heavy";
        v.push_back(d);
        d = Def(1500, "Madman's Knowledge", G::BodyBuff, K::Head, "packet", 0.7f, 0.4f, 0.85f, 2);
        d.insight = 1;
        d.status = "Insight +1";
        v.push_back(d);
        d = Def(2060, "Old Hunter Bone", G::BodyBuff, K::Head, "bone", 0.88f, 0.84f, 0.72f, 1);
        d.consumed = false;
        d.bullet_cost = 4;
        d.duration_s = 16.0f;
        d.status = "Old Hunter Bone: water veil";
        v.push_back(d);
    }
    // ---- Tools that are not ingested. Bells ring next to the ear (the head gesture); the
    // marks and the bone blade are broken in the fist (crush).
    {
        ConsumableDef d = Def(200, "Beckoning Bell", G::Tool, K::Head, "bell", 0.85f, 0.7f, 0.3f, 1);
        d.mesh = "cs_200";
        d.consumed = false;
        d.status = "Beckoning Bell rung";
        v.push_back(d);
        d = Def(205, "Small Resonant Bell", G::Tool, K::Head, "bell", 0.8f, 0.8f, 0.85f, 1);
        d.mesh = "cs_205";
        d.consumed = false;
        d.status = "Small Resonant Bell rung";
        v.push_back(d);
        d = Def(225, "Sinister Resonant Bell", G::Tool, K::Head, "bell", 0.55f, 0.35f, 0.7f, 1);
        d.mesh = "cs_225";
        d.consumed = false;
        d.status = "Sinister Resonant Bell rung";
        v.push_back(d);
        d = Def(2050, "Choir Bell", G::Tool, K::Head, "bell", 0.95f, 0.85f, 0.45f, 1);
        d.mesh = "cs_2050";
        d.consumed = false;
        d.bullet_cost = 7;
        d.status = "Choir Bell rung";
        v.push_back(d);
        d = Def(100, "Hunter's Mark", G::Tool, K::Crush, "tablet", 0.55f, 0.5f, 0.5f, 1);
        d.consumed = false;
        d.status = "Hunter's Mark: back to the Dream (sandbox: no-op)";
        v.push_back(d);
        d = Def(1400, "Bold Hunter's Mark", G::Tool, K::Crush, "tablet", 0.8f, 0.35f, 0.3f, 3);
        d.status = "Bold Hunter's Mark: back to the Dream (sandbox: no-op)";
        v.push_back(d);
        d = Def(2090, "Shaman Bone Blade", G::Tool, K::Crush, "bone", 0.78f, 0.74f, 0.62f, 2);
        d.scale = 1.4f;
        d.status = "Shaman Bone Blade shattered";
        v.push_back(d);
    }
    return v;
}

ConsumableDef ThrowableEntry(const ThrowItemDef& t, int def_index) {
    ConsumableDef d;
    d.goods = t.goods;
    d.name = t.name;
    d.group = CGroup::Throwable;
    d.gesture = CGesture::Throw;
    d.shape = t.shape;
    d.color[0] = t.color[0], d.color[1] = t.color[1], d.color[2] = t.color[2];
    d.scale = t.scale;
    d.throw_def = def_index;
    // The four quicksilver tools of the throw list are not used up (bulletConsumeNum of the dump).
    switch (t.goods) {
    case 2070: d.consumed = false; d.bullet_cost = 6; break;
    case 2080: d.consumed = false; d.bullet_cost = 3; break;
    case 2120: d.consumed = false; d.bullet_cost = 1; break;
    case 2130: d.consumed = false; d.bullet_cost = 2; break;
    default: break;
    }
    return d;
}

std::vector<std::pair<int, int>> SampleInventory() {
    std::vector<std::pair<int, int>> v;
    for (const ConsumableDef& d : DefaultConsumables()) {
        v.emplace_back(d.goods, d.sample_count);
    }
    static const std::pair<int, int> kThrow[] = {{1200, 6},  {1201, 3}, {1210, 8}, {1220, 3},
                                                 {1230, 12}, {1240, 10}, {1250, 2}, {1251, 2},
                                                 {1260, 3},  {1270, 2}, {1410, 10}, {2070, 1},
                                                 {2080, 1},  {2120, 1}, {2130, 1}};
    for (const auto& t : kThrow) {
        v.push_back(t);
    }
    v.emplace_back(900, 30); // Quicksilver Bullets: the pool the tools draw from
    return v;
}

int ApplyItemsTable(std::vector<ConsumableDef>& defs, const json::Value& items) {
    int matched = 0;
    const json::Value& rows = items["items"];
    for (size_t i = 0; i < rows.Size(); ++i) {
        const json::Value& row = rows[i];
        const int id = row["id"].Int(-1);
        for (ConsumableDef& d : defs) {
            if (d.goods != id) {
                continue;
            }
            ++matched;
            if (row["name"].IsString() && !row["name"].Str().empty()) {
                d.name = row["name"].Str();
            }
            if (row["isConsume"].type == json::Value::Type::Bool && d.group != CGroup::Throwable) {
                d.consumed = row["isConsume"].Bool(d.consumed);
            }
            if (row["bulletConsumeNum"].IsNumber()) {
                d.bullet_cost = row["bulletConsumeNum"].Int(d.bullet_cost);
            }
            const json::Value& sp = row["spEffect"];
            const float dur = sp["duration_s"].Float(-1.0f);
            if (dur > 0.0f && (d.group == CGroup::WeaponBuff || d.group == CGroup::BodyBuff)) {
                d.duration_s = dur;
            }
            const float heal = -sp["nondefault"]["transfusionHpRate"].Float(0.0f);
            if (heal > 0.0f && d.group == CGroup::Heal) {
                d.heal_pct = heal;
            }
        }
    }
    return matched;
}

void ConsumeConfig::Load(const json::Value& d) {
    Read(d["enabled"], enabled);
    Read(d["wheel"], wheel);
    Read(d["open_button"], open_button);
    Read(d["linger_s"], linger_s);
    Read(d["wheel_mode"], wheel_mode);
    Read(d["close_on_grab"], close_on_grab);
    Read(d["diag_interval_s"], diag_interval_s);
    Read(d["layout"], layout);
    Read(d["distance_m"], distance_m);
    Read(d["down_deg"], down_deg);
    Read(d["spacing_m"], spacing_m);
    Read(d["row_spacing_m"], row_spacing_m);
    Read(d["per_row"], per_row);
    Read(d["item_scale"], item_scale);
    Read(d["bob_m"], bob_m);
    Read(d["labels"], labels);
    Read(d["grab_radius_m"], grab_radius_m);
    Read(d["grab_point"], grab_point);
    Read(d["grab_squeeze"], grab_squeeze);
    Read(d["release_squeeze"], release_squeeze);
    Read(d["mouth"], mouth);
    Read(d["mouth_radius_m"], mouth_radius_m);
    Read(d["below_eyes_m"], below_eyes_m);
    Read(d["head_hold_s"], head_hold_s);
    Read(d["contact_radius_m"], contact_radius_m);
    Read(d["coverage"], coverage);
    Read(d["window_s"], window_s);
    Read(d["blade_start"], blade_start);
    Read(d["contact_gap_s"], contact_gap_s);
    Read(d["buff_default_s"], buff_default_s);
    Read(d["crush_squeeze"], crush_squeeze);
    Read(d["crush_hold_s"], crush_hold_s);
    Read(d["hide_rack"], hide_rack);
    Read(d["throwables_on_wheel"], throwables_on_wheel);
    Read(d["buff_rub"], buff_rub);
    Read(d["stow_weapons"], stow_weapons);
    Read(d["hp_start_pct"], hp_start_pct);
    Read(d["hud"], hud);
    Read(d["log"], log);
    Read(d["log_file"], log_file);
    if (d["inventory"].IsObject()) {
        inventory = d["inventory"];
    }
    if (wheel != "cards") {
        wheel = "objects";
    }
    if (wheel_mode != "hold") {
        wheel_mode = "toggle";
    }
    if (layout != "shelf") {
        layout = "arc";
    }
    linger_s = std::clamp(linger_s, 0.0f, 10.0f);
    distance_m = std::clamp(distance_m, 0.2f, 1.2f);
    down_deg = std::clamp(down_deg, -10.0f, 70.0f);
    spacing_m = std::clamp(spacing_m, 0.04f, 0.4f);
    row_spacing_m = std::clamp(row_spacing_m, 0.04f, 0.4f);
    per_row = std::clamp(per_row, 1, 30);
    item_scale = std::clamp(item_scale, 0.3f, 3.0f);
    grab_radius_m = std::clamp(grab_radius_m, 0.02f, 0.3f);
    release_squeeze = std::min(release_squeeze, grab_squeeze);
    mouth_radius_m = std::clamp(mouth_radius_m, 0.04f, 0.4f);
    head_hold_s = std::clamp(head_hold_s, 0.0f, 5.0f);
    contact_radius_m = std::clamp(contact_radius_m, 0.01f, 0.3f);
    coverage = std::clamp(coverage, 0.05f, 1.0f);
    window_s = std::clamp(window_s, 0.2f, 10.0f);
    blade_start = std::clamp(blade_start, 0.0f, 0.9f);
    buff_default_s = std::max(buff_default_s, 1.0f);
    crush_squeeze = std::clamp(crush_squeeze, 0.5f, 1.0f);
    crush_hold_s = std::clamp(crush_hold_s, 0.0f, 5.0f);
}

std::string ConsumeConfig::Summary(const std::string& in) const {
    std::ostringstream s;
    s << std::boolalpha;
    const auto V = [](m3::Vec3 v) {
        std::ostringstream o;
        o << "[" << v.x << ", " << v.y << ", " << v.z << "]";
        return o.str();
    };
    s << "{\n"
      << in << "  \"enabled\": " << enabled << ", \"wheel\": \"" << wheel << "\", \"open_button\": \""
      << open_button << "\", \"wheel_mode\": \"" << wheel_mode << "\", \"close_on_grab\": " << close_on_grab << ", \"linger_s\": " << linger_s << ",\n"
      << in << "  \"layout\": \"" << layout << "\", \"distance_m\": " << distance_m
      << ", \"down_deg\": " << down_deg << ", \"spacing_m\": " << spacing_m
      << ", \"row_spacing_m\": " << row_spacing_m << ", \"per_row\": " << per_row
      << ", \"item_scale\": " << item_scale << ", \"bob_m\": " << bob_m << ", \"labels\": "
      << labels << ",\n"
      << in << "  \"grab_radius_m\": " << grab_radius_m << ", \"grab_point\": " << V(grab_point)
      << ", \"grab_squeeze\": " << grab_squeeze << ", \"release_squeeze\": " << release_squeeze
      << ",\n"
      << in << "  \"mouth\": " << V(mouth) << ", \"mouth_radius_m\": " << mouth_radius_m
      << ", \"below_eyes_m\": " << below_eyes_m << ", \"head_hold_s\": " << head_hold_s << ",\n"
      << in << "  \"contact_radius_m\": " << contact_radius_m << ", \"coverage\": " << coverage
      << ", \"window_s\": " << window_s << ", \"blade_start\": " << blade_start
      << ", \"contact_gap_s\": " << contact_gap_s << ", \"buff_default_s\": " << buff_default_s
      << ",\n"
      << in << "  \"crush_squeeze\": " << crush_squeeze << ", \"crush_hold_s\": " << crush_hold_s
      << ", \"hide_rack\": " << hide_rack << ", \"throwables_on_wheel\": " << throwables_on_wheel
      << ", \"buff_rub\": " << buff_rub << ", \"stow_weapons\": " << stow_weapons << ", \"hp_start_pct\": " << hp_start_pct
      << ", \"hud\": " << hud << ", \"diag_interval_s\": " << diag_interval_s << ",\n"
      << in << "  \"log\": " << log << ", \"log_file\": \"" << log_file << "\",\n"
      << in << "  \"inventory\": {";
    const auto sample = SampleInventory();
    for (size_t i = 0; i < sample.size(); ++i) {
        s << (i ? ", " : "") << "\"" << sample[i].first << "\": " << sample[i].second;
    }
    s << "}\n" << in << "}";
    return s.str();
}

} // namespace sb
