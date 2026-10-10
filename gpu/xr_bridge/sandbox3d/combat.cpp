// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "combat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <ctime>
#include <fstream>
#include <sstream>

#include "bbport_xr_bridge.h"
#include "model.h"
#include "scene.h"

namespace sb {

using namespace m3;
using Label = SwingReadout::Label;
using Result = SwingReadout::Result;

namespace {

/// printf into a string of any length (the swing records carry their hand samples).
std::string Fmt(const char* format, ...) {
    va_list args, again;
    va_start(args, format);
    va_copy(again, args);
    char small[512];
    const int n = std::vsnprintf(small, sizeof(small), format, args);
    va_end(args);
    std::string out;
    if (n >= 0 && size_t(n) < sizeof(small)) {
        out.assign(small, size_t(n));
    } else if (n >= 0) {
        out.resize(size_t(n) + 1);
        std::vsnprintf(out.data(), out.size(), format, again);
        out.resize(size_t(n));
    }
    va_end(again);
    return out;
}

void Read(const json::Value& v, float& out) {
    if (v.IsNumber()) {
        out = v.Float();
    }
}
void Read(const json::Value& v, double& out) {
    if (v.IsNumber()) {
        out = v.Number();
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
void Read(const json::Value& v, Vec3& out) {
    if (v.IsArray() && v.Size() >= 3) {
        out = {v[0].Float(), v[1].Float(), v[2].Float()};
    }
}
void Read(const json::Value& v, swing::Vec3& out) {
    if (v.IsArray() && v.Size() >= 3) {
        out = {v[0].Float(), v[1].Float(), v[2].Float()};
    }
}
void Read(const json::Value& v, Pulse& out) {
    if (v.IsArray() && v.Size() >= 2) {
        out = {v[0].Float(), v[1].Float()};
    }
}

std::string Lower(std::string s) {
    for (char& c : s) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// A JSON string literal.
std::string JsonText(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += Fmt("\\u%04x", unsigned(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string P(const Pulse& p) {
    std::ostringstream s;
    s << "[" << p.amplitude << ", " << p.seconds << "]";
    return s.str();
}

std::string V(Vec3 v) {
    std::ostringstream s;
    s << "[" << v.x << ", " << v.y << ", " << v.z << "]";
    return s.str();
}

std::string JsonPoint(Vec3 p) {
    return Fmt("[%.3f, %.3f, %.3f]", p.x, p.y, p.z);
}

swing::Vec3 S(Vec3 v) {
    return {v.x, v.y, v.z};
}
swing::Quat S(Quat q) {
    return {q.x, q.y, q.z, q.w};
}

const char* LabelName(Label l) {
    switch (l) {
    case Label::Light:
        return "LIGHT";
    case Label::Heavy:
        return "HEAVY";
    case Label::Charging:
        return "CHARGING";
    case Label::Charged:
        return "CHARGED";
    case Label::Windup:
        return "WINDUP";
    case Label::TooSlow:
        return "TOO SLOW";
    case Label::Tired:
        return "NO STAMINA";
    case Label::Blocked:
        return "BLOCKED";
    case Label::Dash:
        return "DASH";
    case Label::DashTired:
        return "DASH: NO STAMINA";
    default:
        return "";
    }
}

/// Labels that show what an attack or a move did; a weak one (a dropped stroke) does not cover
/// them while they are fresh.
bool Strong(Label l) {
    return l != Label::None && l != Label::Windup && l != Label::TooSlow;
}

constexpr const char* kMoveNames[3] = {"dodge", "backstep", "jump"};
constexpr const char* kEntity[kEntityCount] = {"floor", "post", "wall", "enemy"};

} // namespace

// ---- config -------------------------------------------------------------------------------------

void CombatConfig::Load(const json::Value& d) {
    Read(d["enabled"], enabled);
    Read(d["class"], weapon_class);
    weapon_class = Lower(weapon_class);
    Read(d["light_speed"], light_speed);
    Read(d["heavy_speed"], heavy_speed);
    Read(d["min_arc"], min_arc);
    Read(d["decide_s"], decide_s);
    Read(d["max_swing_s"], max_swing_s);
    swing::SwingTuning& t = tuning;
    Read(d["probe"], t.probe);
    Read(d["onset_speed"], t.onset_speed);
    Read(d["heavy_accel"], t.heavy_accel);
    Read(d["up_limit"], t.up_limit);
    Read(d["rearm_speed"], t.rearm_speed);
    Read(d["rearm_s"], t.rearm_s);
    Read(d["reverse_rearm"], t.reverse_rearm);
    Read(d["reverse_dot"], t.reverse_dot);
    Read(d["reverse_follow"], t.reverse_follow);
    Read(d["head_relative"], t.head_relative);
    Read(d["turn_comp"], t.turn_comp);
    Read(d["glitch_m"], t.glitch_m);
    Read(d["max_speed"], t.max_speed);
    Read(d["max_gap_s"], t.max_gap_s);
    Read(d["smooth"], t.smooth);
    Read(d["charge"], t.charge);
    Read(d["charge_above_head"], t.charge_above_head);
    Read(d["charge_behind"], t.charge_behind);
    Read(d["charge_still_speed"], t.charge_still_speed);
    Read(d["charge_hold_s"], t.charge_hold_s);
    Read(d["charge_max_s"], t.charge_max_s);
    Read(d["charge_drop_s"], t.charge_drop_s);
    Read(d["charge_needs_grip"], t.charge_needs_grip);
    Read(d["charge_full_s"], charge_full_s);
    const json::Value& x = d["transform"];
    swing::TransformTuning& tt = transform;
    Read(x["enabled"], tt.enabled);
    Read(x["gesture"], transform_gesture);
    Read(x["button"], transform_button);
    Read(x["min_angle_deg"], tt.min_angle_deg);
    Read(x["window_s"], tt.window_s);
    Read(x["max_travel_m"], tt.max_travel_m);
    Read(x["cooldown_s"], tt.cooldown_s);
    Read(x["rest_rate"], tt.rest_rate);
    Read(x["veto_rate"], tt.veto_rate);
    Read(x["veto_grip_speed"], tt.veto_grip_speed);
    Read(x["veto_hold_s"], tt.veto_hold_s);
    Read(x["lock_s"], tt.lock_s);
    Read(x["max_gap_s"], tt.max_gap_s);
    Read(x["glitch_m"], tt.glitch_m);
    Read(x["haptic"], transform_haptic);
    Read(x["haptic_lock"], transform_lock);
    Read(x["haptic_gap_s"], transform_gap_s);
    Read(x["haptic_none"], transform_none);
    const json::Value& s = d["stamina"];
    Read(s["max"], stamina.max);
    Read(s["floor"], stamina.floor);
    Read(s["regen_per_s"], stamina.regen_per_s);
    Read(s["regen_delay_s"], stamina.regen_delay_s);
    Read(s["hit_window_s"], stamina.hit_window_s);
    Read(s["gate_at_zero"], stamina.gate_at_zero);
    Read(s["infinite"], infinite);
    Read(s["light_cost"], light_cost);
    Read(s["heavy_cost"], heavy_cost);
    Read(s["charged_cost"], charged_cost);
    Read(s["dodge_cost"], dodge_cost);
    Read(s["backstep_cost"], backstep_cost);
    Read(s["jump_cost"], jump_cost);
    Read(s["charge_world"], charge_world);
    Read(s["charge_floor"], charge_floor);
    Read(s["contact_margin_m"], contact_margin_m);
    Read(s["pre_contact_s"], pre_contact_s);
    const json::Value& h = d["haptics"];
    Read(h["light"], tick_light);
    Read(h["heavy"], tick_heavy);
    Read(h["charge"], tick_charge);
    Read(h["hit_light"], hit_light);
    Read(h["hit_heavy"], hit_heavy);
    Read(h["hit_charged"], hit_charged);
    Read(h["tired"], tired);
    Read(h["tired_gap_s"], tired_gap_s);
    Read(h["soft_contact"], soft_contact);
    Read(h["replace_physics_hit"], replace_physics_hit);
    const json::Value& u = d["hud"];
    Read(u["head"], hud_head);
    Read(u["wrist"], hud_wrist);
    Read(u["distance"], hud_distance);
    Read(u["down_deg"], hud_down_deg);
    Read(u["scale"], hud_scale);
    Read(u["wrist_scale"], wrist_scale);
    Read(u["wrist_offset"], wrist_offset);
    Read(u["label_s"], label_s);
    Read(u["speed_max"], speed_max);
    Read(u["popups"], popups);
    Read(d["log"], log);
    Read(d["log_samples"], log_samples);
    Read(d["log_file"], log_file);
    // Keep the numbers usable whatever the file says.
    tt.min_angle_deg = std::clamp(tt.min_angle_deg, 20.0f, 360.0f);
    tt.window_s = std::clamp(tt.window_s, 0.03, 1.0);
    tt.max_travel_m = std::max(tt.max_travel_m, 0.0f);
    tt.cooldown_s = std::max(tt.cooldown_s, 0.0);
    tt.veto_hold_s = std::max(tt.veto_hold_s, 0.0);
    tt.lock_s = std::max(tt.lock_s, 0.0);
    stamina.max = std::max(stamina.max, 1.0f);
    stamina.floor = std::min(stamina.floor, 0.0f);
    stamina.regen_per_s = std::max(stamina.regen_per_s, 0.0f);
    stamina.regen_delay_s = std::max(stamina.regen_delay_s, 0.0);
    stamina.hit_window_s = std::clamp(stamina.hit_window_s, 0.02, 5.0);
    t.smooth = std::clamp(t.smooth, 0.05f, 1.0f);
    contact_margin_m = std::clamp(contact_margin_m, 0.0f, 0.1f);
    pre_contact_s = std::clamp(pre_contact_s, 0.0, 1.0);
    hud_distance = std::clamp(hud_distance, 0.2f, 5.0f);
    hud_scale = std::clamp(hud_scale, 0.2f, 5.0f);
    wrist_scale = std::clamp(wrist_scale, 0.2f, 5.0f);
    speed_max = std::max(speed_max, 1.0f);
}

std::string CombatConfig::Summary() const {
    const swing::SwingTuning& t = tuning;
    std::ostringstream s;
    s << std::boolalpha;
    s << "{\n"
      << "    \"enabled\": " << enabled << ",\n"
      << "    \"class\": " << JsonText(weapon_class) << ", \"light_speed\": " << light_speed
      << ", \"heavy_speed\": " << heavy_speed << ", \"min_arc\": " << min_arc
      << ", \"decide_s\": " << decide_s << ", \"max_swing_s\": " << max_swing_s << ",\n"
      << "    \"onset_speed\": " << t.onset_speed << ", \"heavy_accel\": " << t.heavy_accel
      << ", \"up_limit\": " << t.up_limit << ", \"rearm_speed\": " << t.rearm_speed
      << ", \"rearm_s\": " << t.rearm_s << ", \"reverse_rearm\": " << t.reverse_rearm
      << ", \"reverse_dot\": " << t.reverse_dot << ", \"reverse_follow\": " << t.reverse_follow
      << ",\n"
      << "    \"probe\": [" << t.probe.x << ", " << t.probe.y << ", " << t.probe.z
      << "], \"head_relative\": " << t.head_relative << ", \"turn_comp\": " << t.turn_comp
      << ", \"glitch_m\": " << t.glitch_m << ", \"max_speed\": " << t.max_speed
      << ", \"max_gap_s\": " << t.max_gap_s << ", \"smooth\": " << t.smooth << ",\n"
      << "    \"charge\": " << t.charge << ", \"charge_above_head\": " << t.charge_above_head
      << ", \"charge_behind\": " << t.charge_behind
      << ", \"charge_still_speed\": " << t.charge_still_speed
      << ", \"charge_hold_s\": " << t.charge_hold_s << ", \"charge_max_s\": " << t.charge_max_s
      << ", \"charge_drop_s\": " << t.charge_drop_s
      << ", \"charge_needs_grip\": " << t.charge_needs_grip
      << ", \"charge_full_s\": " << charge_full_s << ",\n"
      << "    \"transform\": {\"enabled\": " << transform.enabled
      << ", \"gesture\": " << transform_gesture << ", \"button\": " << transform_button
      << ",\n      \"min_angle_deg\": " << transform.min_angle_deg
      << ", \"window_s\": " << transform.window_s
      << ", \"max_travel_m\": " << transform.max_travel_m
      << ", \"cooldown_s\": " << transform.cooldown_s
      << ", \"rest_rate\": " << transform.rest_rate << ",\n      \"veto_rate\": "
      << transform.veto_rate << ", \"veto_grip_speed\": " << transform.veto_grip_speed
      << ", \"veto_hold_s\": " << transform.veto_hold_s << ", \"lock_s\": " << transform.lock_s
      << ", \"max_gap_s\": " << transform.max_gap_s << ", \"glitch_m\": " << transform.glitch_m
      << ",\n      \"haptic\": " << P(transform_haptic) << ", \"haptic_lock\": "
      << P(transform_lock) << ", \"haptic_gap_s\": " << transform_gap_s
      << ", \"haptic_none\": " << P(transform_none) << "},\n"
      << "    \"stamina\": {\"max\": " << stamina.max << ", \"floor\": " << stamina.floor
      << ", \"regen_per_s\": " << stamina.regen_per_s
      << ", \"regen_delay_s\": " << stamina.regen_delay_s
      << ", \"hit_window_s\": " << stamina.hit_window_s
      << ", \"gate_at_zero\": " << stamina.gate_at_zero << ", \"infinite\": " << infinite
      << ",\n      \"light_cost\": " << light_cost << ", \"heavy_cost\": " << heavy_cost
      << ", \"charged_cost\": " << charged_cost << ", \"dodge_cost\": " << dodge_cost
      << ", \"backstep_cost\": " << backstep_cost << ", \"jump_cost\": " << jump_cost
      << ",\n      \"charge_world\": " << charge_world << ", \"charge_floor\": " << charge_floor
      << ", \"contact_margin_m\": " << contact_margin_m
      << ", \"pre_contact_s\": " << pre_contact_s << "},\n"
      << "    \"haptics\": {\"light\": " << P(tick_light) << ", \"heavy\": " << P(tick_heavy)
      << ", \"charge\": " << P(tick_charge) << ", \"hit_light\": " << P(hit_light)
      << ", \"hit_heavy\": " << P(hit_heavy) << ", \"hit_charged\": " << P(hit_charged)
      << ",\n      \"tired\": " << P(tired) << ", \"tired_gap_s\": " << tired_gap_s
      << ", \"soft_contact\": " << P(soft_contact)
      << ", \"replace_physics_hit\": " << replace_physics_hit << "},\n"
      << "    \"hud\": {\"head\": " << hud_head << ", \"wrist\": " << hud_wrist
      << ", \"distance\": " << hud_distance << ", \"down_deg\": " << hud_down_deg
      << ", \"scale\": " << hud_scale << ", \"wrist_scale\": " << wrist_scale
      << ", \"wrist_offset\": " << V(wrist_offset) << ",\n      \"label_s\": " << label_s
      << ", \"speed_max\": " << speed_max << ", \"popups\": " << popups << "},\n"
      << "    \"log\": " << log << ", \"log_samples\": " << log_samples
      << ", \"log_file\": " << JsonText(log_file) << "\n"
      << "  }";
    return s.str();
}

bool CombatConfigFile::Poll(double now, CombatConfig& config, std::string& log) {
    if (loaded && now < next) {
        return false;
    }
    next = now + 0.5;
    std::error_code ec;
    const auto stamp_now = std::filesystem::last_write_time(path, ec);
    if (ec) {
        if (!loaded) {
            loaded = true;
            config = CombatConfig{};
            log += "swing: no " + path.string() + ", defaults:\n  \"swing\": " + config.Summary() +
                   "\n";
            return true;
        }
        return false;
    }
    if (loaded && stamp_now == stamp) {
        return false;
    }
    stamp = stamp_now;
    const std::string text = ReadText(path);
    std::string error;
    const json::Value doc = json::Parse(text, &error);
    if (!error.empty()) {
        log += "swing: " + path.string() + ": " + error + " (keeping the previous settings)\n";
        loaded = true;
        return false;
    }
    CombatConfig next_config;
    next_config.Load(doc["swing"]);
    if (!doc.Has("swing") && !inserted) {
        // Add the section with every default, keeping the rest of the file as it is.
        inserted = true;
        const size_t close = text.find_last_of('}');
        if (close != std::string::npos) {
            size_t body = close;
            while (body > 0 && std::isspace(static_cast<unsigned char>(text[body - 1]))) {
                --body;
            }
            const bool empty = body > 0 && text[body - 1] == '{';
            const std::string updated = text.substr(0, body) + (empty ? "\n" : ",\n") +
                                        "  \"swing\": " + next_config.Summary() + "\n" +
                                        text.substr(close);
            const std::filesystem::path temp = path.string() + ".swing.tmp";
            std::ofstream out(temp, std::ios::binary);
            out << updated;
            out.close();
            std::error_code rename_error;
            if (out) {
                std::filesystem::rename(temp, path, rename_error);
            }
            log += rename_error || !out
                       ? "swing: could not add the \"swing\" section to " + path.string() + "\n"
                       : "swing: added the \"swing\" section with the defaults to " +
                             path.string() + "\n";
            // Our own write must not count as the user's edit.
            std::error_code ec2;
            const auto written = std::filesystem::last_write_time(path, ec2);
            if (!ec2) {
                stamp = written;
            }
        }
    }
    config = next_config;
    log += std::string(loaded ? "swing config reloaded" : "swing config") + ":\n  \"swing\": " +
           config.Summary() + "\n";
    loaded = true;
    return true;
}

// ---- weapons ------------------------------------------------------------------------------------

std::string WeaponClassOf(const std::string& name, const std::string& form) {
    const std::string n = Lower(name);
    const bool b = form == "B" || form == "b";
    const auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
    if (has("beast cutter") || has("boom hammer") || has("amygdalan") || has("logarius")) {
        return "heavy";
    }
    if (b && (has("hunter axe") || has("kirkhammer") || has("ludwig") || has("holy moonlight"))) {
        return "heavy";
    }
    if (has("blade of mercy") || has("reiterpallasch") || has("rakuyo") || has("beast claw")) {
        return "quick";
    }
    if (!b && (has("saw cleaver") || has("threaded cane") || has("chikage"))) {
        return "quick";
    }
    return "medium";
}

bool KnownTrickWeapon(const std::string& name) {
    const std::string n = Lower(name);
    for (const char* k : {"saw cleaver", "hunter axe", "threaded cane", "kirkhammer", "ludwig",
                          "holy moonlight", "chikage", "rakuyo", "saw spear", "whirligig",
                          "beast cutter", "boom hammer", "logarius"}) {
        if (n.find(k) != std::string::npos) {
            return true;
        }
    }
    return false;
}

SwingWeapon LookUpSwingWeapon(const std::filesystem::path& assets, const std::string& name,
                              const std::string& form) {
    SwingWeapon w;
    w.name = name;
    w.form = form == "B" || form == "b" ? "B" : "A";
    w.weapon_class = WeaponClassOf(name, w.form);
    w.two_forms = KnownTrickWeapon(name);
    if (assets.empty() || name.empty()) {
        return w;
    }
    AssetIndex index;
    std::string error;
    if (!LoadAssetIndex(assets, index, error)) {
        return w;
    }
    for (const WeaponEntry& e : index.weapons) {
        if (Lower(e.name) == Lower(name) || Lower(e.model) == Lower(name)) {
            w.id = e.id;
            break;
        }
    }
    if (!w.id) {
        return w;
    }
    const std::filesystem::path file = assets / "hitbox" / ("wp_" + std::to_string(w.id) + ".json");
    bool ok = false;
    const std::string text = ReadText(file, &ok);
    if (!ok) {
        return w;
    }
    const json::Value doc = json::Parse(text, &error);
    if (!error.empty() || !doc["attacks"].IsArray()) {
        return w;
    }
    // The hitbox data knows the forms: a weapon with a form B attack transforms.
    for (const json::Value& a : doc["attacks"].array) {
        if (a["form"].Str() == "B") {
            w.two_forms = true;
            break;
        }
    }
    // BehaviorParam_PC judge ids (doc 20 1.4): R1 #1, R2 tap, charged R2; form B 400 / 550 / 570.
    const int judges[2][3] = {{0, 150, 170}, {400, 550, 570}};
    const int* want = judges[w.form == "B" ? 1 : 0];
    int found = 0;
    for (const json::Value& a : doc["attacks"].array) {
        const int judge = a["judge"].Int(-1);
        for (int k = 0; k < 3; ++k) {
            if (judge == want[k] && a["stamina"].IsNumber() && !(found & (1 << k))) {
                w.cost[k] = a["stamina"].Float();
                found |= 1 << k;
            }
        }
    }
    if (found) {
        w.source = "hitbox/" + file.filename().string() + (found == 7 ? "" : ", partly");
    }
    return w;
}

swing::SwingTuning SwingTuningFor(const CombatConfig& c, const std::string& weapon_class) {
    const swing::WeaponClass wc = weapon_class == "quick"   ? swing::WeaponClass::Quick
                                  : weapon_class == "heavy" ? swing::WeaponClass::Heavy
                                                            : swing::WeaponClass::Medium;
    const swing::SwingTuning preset = swing::TuningFor(wc);
    swing::SwingTuning t = c.tuning;
    t.light_speed = c.light_speed > 0.0f ? c.light_speed : preset.light_speed;
    t.heavy_speed = c.heavy_speed > 0.0f ? c.heavy_speed : preset.heavy_speed;
    t.min_arc = c.min_arc > 0.0f ? c.min_arc : preset.min_arc;
    t.decide_s = c.decide_s > 0.0 ? c.decide_s : preset.decide_s;
    t.max_swing_s = c.max_swing_s > 0.0 ? c.max_swing_s : preset.max_swing_s;
    return t;
}

// ---- contacts -----------------------------------------------------------------------------------

SwingContact DetectSwingContacts(const WeaponPhysics& physics, int side, float margin) {
    SwingContact out;
    const WeaponPhysics::Hand& hand = physics.GetHand(side);
    if (!hand.active || !hand.have_pose) {
        return out;
    }
    out.swinging = hand.swinging;
    const col::Pose& pose = hand.pose;
    // The attack shapes are tested where the hand puts the weapon (the physics' HIT does the
    // same: a solid blocker cannot hold them off), the weapon's own capsules where it is.
    const auto world = [&](const col::Capsule& local, float extra, bool unclipped) {
        const col::Pose& at = unclipped ? hand.target : pose;
        return col::Capsule{col::Apply(at, local.a), col::Apply(at, local.b),
                            local.r * at.s + extra};
    };
    // The enemy: the shown attack's hit capsules (the game's, scaled as the physics' HIT uses
    // them), else the weapon's own.
    const bool attack = !hand.attack.empty();
    const std::vector<col::Capsule>& shapes = attack ? hand.attack : hand.shape.capsules;
    float deepest = 0.0f;
    for (const col::Capsule& s : shapes) {
        const col::Capsule cap = world(s, attack ? 0.0f : margin, attack);
        const col::Aabb box = col::Bounds(cap);
        for (const Collider& k : physics.Colliders()) {
            if (k.entity != kEnemy || k.kind != Collider::kCapsule || !col::Overlap(box, k.bounds)) {
                continue;
            }
            const col::Contact c = col::CapsuleCapsule(cap, k.capsule);
            if (c.hit && c.depth > deepest) {
                deepest = c.depth;
                out.enemy = true;
                out.part = k.part;
                out.enemy_point = c.point_b;
            }
        }
    }
    // The world: the weapon's own capsules, a margin out (the solver keeps them at the surface).
    float world_depth[kEntityCount] = {};
    for (const col::Capsule& s : hand.shape.capsules) {
        const col::Capsule cap = world(s, margin, false);
        const col::Aabb box = col::Bounds(cap);
        for (const Collider& k : physics.Colliders()) {
            if (k.entity == kEnemy || !col::Overlap(box, k.bounds)) {
                continue;
            }
            const col::Contact c = k.kind == Collider::kCapsule ? col::CapsuleCapsule(cap, k.capsule)
                                   : k.kind == Collider::kBox   ? col::CapsuleBox(cap, k.box)
                                                                : col::CapsulePlane(cap, k.plane);
            if (c.hit && c.depth > world_depth[k.entity]) {
                world_depth[k.entity] = c.depth;
                out.world[k.entity] = true;
                out.world_point[k.entity] = c.point_b;
            }
        }
    }
    return out;
}

// ---- combat -------------------------------------------------------------------------------------

Combat::Combat(const std::filesystem::path& config_path)
    : assets(config_path.parent_path()), file(std::make_unique<CombatConfigFile>(config_path)) {}

Combat::Combat(const CombatConfig& fixed, const std::filesystem::path& assets_dir)
    : assets(assets_dir) {
    std::string log;
    Apply(fixed, 0.0, log);
}

Combat::~Combat() {
    if (log_file) {
        std::fclose(log_file);
    }
}

void Combat::SetConfig(const CombatConfig& next) {
    std::string log;
    Apply(next, now, log);
}

void Combat::Apply(const CombatConfig& next, double, std::string&) {
    const bool first = !configured;
    config = next;
    configured = true;
    if (first) {
        stamina = swing::StaminaModel(config.stamina);
    } else {
        stamina.SetTuning(config.stamina);
    }
    weapon_key = "\x01"; // the class, the overrides and the costs again
    readout.enabled = config.enabled;
}

void Combat::ChangeWeapon(const std::string& name, const std::string& form, std::string& log) {
    const SwingWeapon next = LookUpSwingWeapon(assets, name, form);
    const bool changed = next.name != weapon.name || next.form != weapon.form || side_changed;
    side_changed = false;
    weapon = next;
    if (changed) {
        ResetSwings(); // (a config reload alone keeps the swing in progress)
    }
    const std::string cls = config.weapon_class.empty() || config.weapon_class == "auto"
                                ? weapon.weapon_class
                                : config.weapon_class;
    readout.weapon = weapon.name;
    readout.form = weapon.form;
    readout.two_forms = weapon.two_forms;
    readout.weapon_class = cls;
    classifier.SetTuning(SwingTuningFor(config, cls));
    const swing::SwingTuning& t = classifier.Tuning();
    log += Fmt("swing: right hand %s (form %s): class %s%s: onset %.2f, light %.2f, heavy %.2f m/s, "
               "min arc %.2f m, decided %.0f ms after the onset; costs R1 %g, R2 %g, charged R2 %g "
               "(%s); dodge %g, backstep %g, jump %g%s\n",
               weapon.name.empty() ? "(empty)" : weapon.name.c_str(), weapon.form.c_str(),
               cls.c_str(), config.weapon_class == "auto" ? " (auto)" : "", t.onset_speed,
               t.light_speed, t.heavy_speed, t.min_arc, t.decide_s * 1000.0, Cost(0), Cost(1),
               Cost(2),
               config.light_cost > 0.0f || config.heavy_cost > 0.0f || config.charged_cost > 0.0f
                   ? "set in the config"
                   : weapon.source.c_str(),
               config.dodge_cost, config.backstep_cost, config.jump_cost,
               config.infinite ? "; infinite: nothing is paid" : "");
}

void Combat::ResetSwings() {
    classifier.Reset(); // keeps the tuning (set right after)
    seen_dropped = 0;
    charging = charge_refused = false;
    stamina.DropOpen();
    tickets.clear();
    contacts.clear();
    due.erase(std::remove_if(due.begin(), due.end(), [](const Due& d) { return !d.keep; }),
              due.end()); // (the transform's clicks go on)
    was_enemy = false;
    for (bool& w : was_world) {
        w = false;
    }
}

float Combat::Cost(int k) const {
    if (config.infinite) {
        return 0.0f;
    }
    const float set = k == 0 ? config.light_cost : k == 1 ? config.heavy_cost : config.charged_cost;
    return set > 0.0f ? set : weapon.cost[k];
}

void Combat::Frame(const SceneInput& in, const WeaponPhysics& physics, const std::string& form,
                   bool blocked, CombatEvents& out) {
    // The hand that holds the weapon: the right (the sword hand) when it has one, else the left.
    const int held_side = physics.GetHand(1).active ? 1 : physics.GetHand(0).active ? 0 : 1;
    if (held_side != side) {
        side = held_side;
        side_changed = true;
        weapon_key = "\x01"; // the weapon changes hands: a new swing state
        last_physics_hits = physics.HitCount();
    }
    const uint32_t pressed = in.buttons & ~last_buttons;
    last_buttons = in.buttons;
    if (pressed & BBXR_BTN_L1) {
        PressTransform(); // L1 as in the game (the left grip): the transform
    }
    swing::HandSample h;
    h.t = in.time;
    h.valid = in.grip[side].valid;
    h.position = S(in.grip[side].position);
    h.orientation = S(in.grip[side].rotation);
    h.squeeze = in.squeeze[side];
    h.head_valid = in.head.valid;
    h.head_position = S(in.head.position);
    h.head_orientation = S(in.head.rotation);
    SwingContact contact = DetectSwingContacts(physics, side, config.contact_margin_m);
    // A contact pays when it starts during a swing (the physics' HIT rule: an edge, with the
    // weapon driven at hit_speed or more); a weapon resting on the target pays nothing.
    {
        const bool touch_enemy = contact.enemy;
        bool touch_world[kEntityCount];
        for (int e = 0; e < kEntityCount; ++e) {
            touch_world[e] = contact.world[e];
            contact.world[e] = touch_world[e] && !was_world[e] && contact.swinging;
            was_world[e] = touch_world[e];
        }
        contact.enemy = touch_enemy && !was_enemy && contact.swinging;
        was_enemy = touch_enemy;
    }
    // The physics' own HIT this frame on this hand (its substeps catch what a frame misses).
    if (physics.HitCount() > last_physics_hits) {
        for (const WeaponPhysics::Marker& m : physics.Markers()) {
            if (m.side == side && m.time >= physics.Now() - 1e-9) {
                contact.physics_hit = true;
                if (!contact.enemy) {
                    contact.part = m.part;
                    contact.enemy_point = m.point;
                }
            }
        }
    }
    last_physics_hits = physics.HitCount();
    if (contact.part >= 0 && size_t(contact.part) < physics.GetEnemy().part_names.size()) {
        contact.part_name = physics.GetEnemy().part_names[size_t(contact.part)];
    }
    const WeaponPhysics::Hand& hand = physics.GetHand(side);
    contact.target = physics.GetEnemy().chr;
    Update(h, contact, hand.active ? hand.name : std::string(), form, blocked, out);
}

void Combat::Update(const swing::HandSample& hand, const SwingContact& contact,
                    const std::string& weapon_name, const std::string& form, bool blocked,
                    CombatEvents& out) {
    const double t = hand.t;
    now = t;
    std::string config_log;
    if (file) {
        CombatConfig next = config;
        if (file->Poll(t, next, config_log)) {
            Apply(next, t, config_log);
        }
    }
    if (!configured) {
        Apply(CombatConfig{}, t, config_log);
    }
    const std::string key = weapon_name + "|" + form + "|" + config.weapon_class;
    if (key != weapon_key) {
        weapon_key = key;
        ChangeWeapon(weapon_name, form, config_log);
    }
    out.log += config_log;
    haptic_priority = 0;
    haptic = {};
    if (!config.enabled) {
        readout.enabled = false;
        out.log += pending_log;
        pending_log.clear();
        pending_haptics.clear();
        return;
    }
    readout.enabled = true;

    // The hand samples of the last 1.5 s (the strokes' records in the log).
    if (hand.valid) {
        samples.push_back(hand);
    }
    while (!samples.empty() && samples.front().t < t - 1.5) {
        samples.pop_front();
    }
    // What the weapon touches this frame: the enemy first, then the world when it counts.
    bool fresh = false;
    {
        ContactSample c;
        c.t = t;
        if (contact.enemy || contact.physics_hit) {
            c.enemy = true;
            c.what = contact.part;
            c.name = contact.target + (contact.part_name.empty() ? "" : " " + contact.part_name);
            c.point = contact.enemy_point;
            fresh = true;
        } else if (config.charge_world) {
            for (const int e : {int(kPost), int(kWall), int(kFloor)}) {
                if (contact.world[e] && (e != kFloor || config.charge_floor)) {
                    c.enemy = false;
                    c.what = e;
                    c.name = kEntity[e];
                    c.point = contact.world_point[e];
                    fresh = true;
                    break;
                }
            }
        }
        if (fresh) {
            contacts.push_back(c);
        }
        while (!contacts.empty() && contacts.front().t < t - 1.0) {
            contacts.pop_front();
        }
    }

    // The trick-weapon transform: a wrist snap (or the L1 button) switches the form. While the
    // wrist snaps the swing classifier does not see the hand: a snap is never an attack (a hold
    // of R2 in progress keeps its samples).
    transformer.SetTuning(config.transform);
    swing::TransformEvent snap = transformer.Update(hand);
    if (button_transform) {
        button_transform = false;
        if (config.transform_button && !blocked) {
            Transform("button", {}, form, blocked, t, out);
        }
    }
    if (snap.fired && config.transform_gesture && !blocked) {
        Transform("snap", snap, form, blocked, t, out);
    }
    // The classifier.
    swing::SwingEvent e;
    if (!(config.transform_gesture && transformer.Veto() && !charging)) {
        e = classifier.Update(hand);
    }
    const swing::SwingClassifier::Debug& dbg = classifier.State();
    if (dbg.dropped.count != seen_dropped) {
        seen_dropped = dbg.dropped.count;
        Dropped(dbg.dropped, t);
    }
    switch (e.kind) {
    case swing::Attack::Light:
        Attack(e, Label::Light, Cost(0), blocked, t, -1.0);
        break;
    case swing::Attack::Heavy:
        Attack(e, Label::Heavy, Cost(1), blocked, t, -1.0);
        break;
    case swing::Attack::ChargeStart:
        ChargeStart(blocked, t);
        break;
    case swing::Attack::ChargeRelease:
        ChargeRelease(e, blocked, t);
        break;
    default:
        break;
    }
    // This frame's contact, when no stroke waits for its decision: the newest open swing.
    if (fresh && !contacts.back().used && !dbg.stroke) {
        Connect(0, contacts.back(), t);
    }
    // The physics registered a hit, but no attack: a soft bump instead of its hit pulse.
    if (contact.physics_hit && config.replace_physics_hit && !dbg.stroke &&
        stamina.OpenSwings() == 0 && !(fresh && contacts.back().used)) {
        Haptic(1, config.soft_contact);
    }

    // Windows that ended without a contact: misses, free.
    swing::StaminaEvent events[swing::StaminaModel::kMaxOpen];
    const int n = stamina.Update(t, events, swing::StaminaModel::kMaxOpen);
    for (int i = 0; i < std::min(n, int(swing::StaminaModel::kMaxOpen)); ++i) {
        if (events[i].kind == swing::StaminaEvent::Kind::Refunded) {
            Missed(events[i].swing, t);
        }
    }

    // Regen and the gate, for the log.
    {
        const float cur = stamina.Current();
        const bool full = cur >= stamina.Max() - 1e-3f;
        const bool can_act = stamina.CanAct();
        const bool regen = !full && t >= regen_at && config.stamina.regen_per_s > 0.0f;
        if (have_time) {
            if (!can_act && was_can_act) {
                Print(Fmt("stamina: %.1f, at or below 0: no swing registers and no move starts "
                          "until it regenerates above 0",
                          cur));
                Json(Fmt("{\"t\": %.4f, \"event\": \"stamina\", \"what\": \"exhausted\", \"sp\": "
                         "%.2f}",
                         t, cur));
            }
            if (regen && !was_regen) {
                Print(Fmt("stamina: regen from %.1f (%g/s)", cur, config.stamina.regen_per_s));
                Json(Fmt("{\"t\": %.4f, \"event\": \"stamina\", \"what\": \"regen\", \"sp\": %.2f}",
                         t, cur));
            }
            if (can_act && !was_can_act) {
                Print(Fmt("stamina: %.1f, above 0 again: swings and moves register", cur));
                Json(Fmt("{\"t\": %.4f, \"event\": \"stamina\", \"what\": \"can_act\", \"sp\": "
                         "%.2f}",
                         t, cur));
            }
            if (full && !was_full) {
                Print(Fmt("stamina: full (%g)", stamina.Max()));
                Json(Fmt("{\"t\": %.4f, \"event\": \"stamina\", \"what\": \"full\", \"sp\": %.2f}",
                         t, cur));
            }
        }
        was_full = full;
        was_can_act = can_act;
        was_regen = regen;
        have_time = true;
    }

    // Haptics: the queued ones that are due, then this frame's strongest.
    for (size_t i = 0; i < due.size();) {
        if (due[i].t <= t + 1e-9) {
            pending_haptics.push_back(due[i].haptic);
            due.erase(due.begin() + long(i));
        } else {
            ++i;
        }
    }
    if (haptic_priority > 0 && haptic.amplitude > 0.0f && haptic.seconds > 0.0f) {
        pending_haptics.push_back({1, haptic.amplitude, haptic.seconds});
    }

    // The readout.
    const swing::SwingTuning& tun = classifier.Tuning();
    readout.stamina = stamina.Current();
    readout.max = stamina.Max();
    readout.floor = config.stamina.floor;
    readout.pending = stamina.Pending();
    readout.can_act = stamina.CanAct();
    readout.regen_paused = t < regen_at && stamina.Current() < stamina.Max();
    readout.regen_at = regen_at;
    readout.phase = dbg.phase;
    readout.speed = dbg.speed;
    readout.stroke_peak = dbg.stroke ? dbg.peak : 0.0f;
    readout.onset = tun.onset_speed;
    readout.light = tun.light_speed;
    readout.heavy = tun.heavy_speed;
    readout.charging = charging;
    readout.charge_full_s = config.charge_full_s;
    readout.now = t;
    readout.popups.erase(std::remove_if(readout.popups.begin(), readout.popups.end(),
                                        [&](const SwingReadout::Popup& p) { return t - p.t > 1.2; }),
                         readout.popups.end());

    out.log += pending_log;
    pending_log.clear();
    out.haptics.insert(out.haptics.end(), pending_haptics.begin(), pending_haptics.end());
    pending_haptics.clear();
}

void Combat::Attack(const swing::SwingEvent& e, Label label, float cost, bool blocked, double t,
                    double held) {
    ++swing_no;
    const float latency = float((e.t - e.onset_t) * 1000.0);
    const char* name = LabelName(label);
    const char* button = label == Label::Light ? "R1" : label == Label::Heavy ? "R2" : "R2 held";
    const std::string charge_text =
        held >= 0.0 ? Fmt(", R2 released after %.2f s (%s)", held,
                          label == Label::Charged ? "charged" : "short of a full charge")
                    : std::string();
    const auto stroke_json = [&]() {
        return Fmt("\"onset\": %.4f, \"latency_ms\": %.1f, \"peak\": %.3f, \"arc\": %.3f, "
                   "\"accel\": %.1f, \"dir\": [%.3f, %.3f, %.3f], \"weapon\": %s, \"form\": %s, "
                   "\"class\": %s%s",
                   e.onset_t, latency, e.peak_speed, e.arc, e.peak_accel, e.direction.x,
                   e.direction.y, e.direction.z, JsonText(weapon.name).c_str(),
                   JsonText(weapon.form).c_str(), JsonText(readout.weapon_class).c_str(),
                   held >= 0.0 ? Fmt(", \"held_s\": %.3f", held).c_str() : "");
    };
    readout.swing_no = swing_no;
    readout.label_speed = e.peak_speed;
    readout.label_cost = cost;
    readout.label_paid = 0.0f;
    readout.latency_ms = latency;
    if (blocked) {
        ++counts.blocked;
        SetLabel(Label::Blocked, t);
        Print(Fmt("swing %u: %s peak %.2f m/s: dropped, a menu has the controls", swing_no, name,
                  e.peak_speed));
        Json(Fmt("{\"t\": %.4f, \"event\": \"swing\", \"no\": %u, \"label\": \"%s\", \"dropped\": "
                 "\"blocked\", %s%s}",
                 t, swing_no, name, stroke_json().c_str(), Samples(e.onset_t - 0.3).c_str()));
        return;
    }
    const float before = stamina.Current();
    const uint32_t id = stamina.BeginSwing(t, cost);
    if (!id) {
        ++counts.tired;
        SetLabel(Label::Tired, t);
        readout.refused_at = t;
        Tired(t);
        Print(Fmt("swing %u: %s (%s) peak %.2f m/s, %.0f ms after the onset%s: does not register, "
                  "no stamina (SP %.1f <= 0)",
                  swing_no, name, button, e.peak_speed, latency, charge_text.c_str(), before));
        Json(Fmt("{\"t\": %.4f, \"event\": \"swing\", \"no\": %u, \"label\": \"%s\", \"button\": "
                 "\"%s\", \"refused\": \"no stamina\", \"cost\": %g, \"sp\": %.2f, %s%s}",
                 t, swing_no, name, button, cost, before, stroke_json().c_str(),
                 Samples(e.onset_t - 0.3).c_str()));
        return;
    }
    regen_at = std::max(regen_at, t + config.stamina.regen_delay_s);
    if (label == Label::Light) {
        ++counts.light;
    } else if (label == Label::Heavy) {
        ++counts.heavy;
    } else {
        ++counts.charged;
    }
    Ticket k;
    k.id = id;
    k.no = swing_no;
    k.label = label;
    k.t = t;
    k.onset_t = e.onset_t;
    k.cost = cost;
    k.peak = e.peak_speed;
    tickets.push_back(k);
    if (tickets.size() > 32) {
        tickets.erase(tickets.begin());
    }
    SetLabel(label, t);
    readout.result = Result::Pending;
    Haptic(2, label == Label::Light ? config.tick_light : config.tick_heavy);
    Print(Fmt("swing %u: %s (%s) peak %.2f m/s, arc %.2f m, %.0f ms after the onset%s: cost %g "
              "held %.2f s for a hit (SP %.1f, pending %g)",
              swing_no, name, button, e.peak_speed, e.arc, latency, charge_text.c_str(), cost,
              config.stamina.hit_window_s, stamina.Current(), stamina.Pending()));
    Json(Fmt("{\"t\": %.4f, \"event\": \"swing\", \"no\": %u, \"label\": \"%s\", \"button\": "
             "\"%s\", \"ticket\": %u, \"cost\": %g, \"sp\": %.2f, \"pending\": %.2f, %s%s}",
             t, swing_no, name, button, id, cost, stamina.Current(), stamina.Pending(),
             stroke_json().c_str(), Samples(e.onset_t - 0.3).c_str()));
    // What the stroke touched before it was decided belongs to it (the enemy first). A charge
    // released without a swing (lowered, or held too long) has no stroke: only what comes next.
    const bool stroke = e.peak_speed > 0.0f || e.arc > 0.0f;
    const double from = stroke ? e.onset_t - config.pre_contact_s : t;
    for (const bool enemy : {true, false}) {
        for (ContactSample& c : contacts) {
            if (c.enemy == enemy && !c.used && c.t >= from - 1e-9 && c.t <= t + 1e-9 &&
                Connect(id, c, t)) {
                return;
            }
        }
    }
}

void Combat::ChargeStart(bool blocked, double t) {
    if (blocked) {
        charge_refused = true;
        ++counts.blocked;
        SetLabel(Label::Blocked, t);
        Print("charge: dropped, a menu has the controls");
        return;
    }
    if (!stamina.CanAct()) {
        charge_refused = true;
        ++counts.tired;
        SetLabel(Label::Tired, t);
        readout.refused_at = t;
        Tired(t);
        Print(Fmt("charge: the R2 hold does not register, no stamina (SP %.1f <= 0)",
                  stamina.Current()));
        Json(Fmt("{\"t\": %.4f, \"event\": \"charge\", \"refused\": \"no stamina\", \"sp\": %.2f}",
                 t, stamina.Current()));
        return;
    }
    charging = true;
    charge_refused = false;
    charge_t = t;
    ++counts.charge_starts;
    SetLabel(Label::Charging, t);
    readout.charge_since = t;
    Haptic(2, config.tick_charge);
    Print(Fmt("charge: the wind-up held still overhead: R2 held (a full charge after %.2f s)",
              config.charge_full_s));
    Json(Fmt("{\"t\": %.4f, \"event\": \"charge\", \"sp\": %.2f}", t, stamina.Current()));
}

void Combat::ChargeRelease(const swing::SwingEvent& e, bool blocked, double t) {
    if (charge_refused || !charging) {
        charge_refused = false;
        charging = false;
        return;
    }
    charging = false;
    const double held = t - charge_t;
    const bool full = held >= config.charge_full_s;
    Attack(e, full ? Label::Charged : Label::Heavy, full ? Cost(2) : Cost(1), blocked, t, held);
}

void Combat::Dropped(const swing::SwingClassifier::Stroke& d, double t) {
    const swing::SwingTuning& tun = classifier.Tuning();
    const char* reason = d.windup                    ? "windup"
                         : d.peak < tun.light_speed ? "too_slow"
                         : d.arc < tun.min_arc       ? "too_short"
                                                     : "timeout";
    // Near misses and wind-ups show; small motions (reaching, gesturing) only go to the file.
    const bool shown = d.windup || d.peak >= 0.75f * tun.light_speed;
    if (d.windup) {
        ++counts.windups;
    } else {
        ++counts.too_slow;
    }
    if (shown) {
        SetLabel(d.windup ? Label::Windup : Label::TooSlow, t, true);
        if (readout.label == (d.windup ? Label::Windup : Label::TooSlow) && readout.label_t == t) {
            readout.label_speed = d.peak;
            readout.label_cost = 0.0f;
        }
        const std::string why = reason;
        const char* text = d.windup              ? "WINDUP (it rose)"
                           : why == "too_slow"   ? "too slow"
                           : why == "too_short"  ? "too short"
                                                 : "too long without qualifying";
        Print(Fmt("stroke: %s, peak %.2f m/s (light %.2f, heavy %.2f), arc %.2f m (min %.2f): "
                  "not an attack",
                  text, d.peak, tun.light_speed, tun.heavy_speed, d.arc, tun.min_arc));
    }
    Json(Fmt("{\"t\": %.4f, \"event\": \"stroke\", \"reason\": \"%s\", \"onset\": %.4f, "
             "\"peak\": %.3f, \"arc\": %.3f%s}",
             t, reason, d.onset_t, d.peak, d.arc, Samples(d.onset_t - 0.3).c_str()));
    // What it touched goes to the swing still open before it (that attack can still hit).
    for (ContactSample& c : contacts) {
        if (!c.used && c.t >= d.onset_t - config.pre_contact_s && c.t <= t + 1e-9 &&
            Connect(0, c, t)) {
            break;
        }
    }
}

bool Combat::Connect(uint32_t id, ContactSample& c, double t) {
    if (stamina.OpenSwings() == 0) {
        return false;
    }
    const float before = stamina.Current();
    const int open_before = stamina.OpenSwings();
    const float paid = stamina.Hit(c.t, id);
    if (stamina.OpenSwings() >= open_before) {
        return false; // nothing open then (expired or unknown)
    }
    c.used = true;
    const uint32_t charged = stamina.LastEvent().swing;
    Ticket* k = Find(charged);
    const float after = stamina.Current();
    if (c.enemy) {
        ++counts.hits;
    } else {
        ++counts.clangs;
    }
    readout.charged_at = t;
    readout.charged_from = before;
    readout.charged_to = after;
    Label kind = Label::Light;
    uint32_t no = 0;
    double decided = t;
    if (k) {
        k->done = true;
        kind = k->label;
        no = k->no;
        decided = k->t;
        if (readout.swing_no == k->no) {
            readout.result = c.enemy ? Result::Hit : Result::Clang;
            readout.label_paid = paid;
            readout.label_t = t; // the label stays a while after the hit
        }
    }
    if (config.popups) {
        readout.popups.push_back({c.point, t, paid, !c.enemy});
    }
    Haptic(4, kind == Label::Light ? config.hit_light
              : kind == Label::Heavy ? config.hit_heavy
                                     : config.hit_charged);
    const bool early = c.t < decided - 1e-9;
    Print(Fmt("swing %u: %s %s: paid %g, SP %.1f -> %.1f (%s)", no,
              c.enemy ? "HIT" : "CLANG", c.name.c_str(), paid, before, after,
              early ? Fmt("it touched %.0f ms before the swing was decided", (decided - c.t) * 1e3)
                          .c_str()
                    : Fmt("%.0f ms after the decision", (c.t - decided) * 1e3).c_str()));
    Json(Fmt("{\"t\": %.4f, \"event\": \"%s\", \"no\": %u, \"ticket\": %u, \"label\": \"%s\", "
             "\"target\": %s, \"part\": %d, \"contact_t\": %.4f, \"after_decision_ms\": %.1f, "
             "\"paid\": %g, \"sp_before\": %.2f, \"sp\": %.2f, \"point\": %s}",
             t, c.enemy ? "hit" : "clang", no, charged, LabelName(kind), JsonText(c.name).c_str(),
             c.what, c.t, (c.t - decided) * 1e3, paid, before, after,
             JsonPoint(c.point).c_str()));
    return true;
}

void Combat::Missed(uint32_t id, double t) {
    Ticket* k = Find(id);
    if (!k || k->done) {
        return;
    }
    k->done = true;
    ++counts.misses;
    if (readout.swing_no == k->no && (readout.label == k->label)) {
        readout.result = Result::Miss;
        readout.label_t = t;
    }
    Print(Fmt("swing %u: %s MISS: it touched nothing in %.2f s, so it cost nothing (SP %.1f)",
              k->no, LabelName(k->label), config.stamina.hit_window_s, stamina.Current()));
    Json(Fmt("{\"t\": %.4f, \"event\": \"miss\", \"no\": %u, \"ticket\": %u, \"label\": \"%s\", "
             "\"cost\": %g, \"sp\": %.2f}",
             t, k->no, id, LabelName(k->label), k->cost, stamina.Current()));
}

Combat::Ticket* Combat::Find(uint32_t id) {
    for (Ticket& k : tickets) {
        if (k.id == id) {
            return &k;
        }
    }
    return nullptr;
}

bool Combat::TryDash(int kind, double t) {
    if (!configured || !config.enabled) {
        return true;
    }
    kind = std::clamp(kind, 0, 2);
    const float cost = config.infinite         ? 0.0f
                       : kind == 0             ? config.dodge_cost
                       : kind == 1             ? config.backstep_cost
                                               : config.jump_cost;
    const float before = stamina.Current();
    const bool ok = stamina.TryDash(t, cost);
    readout.move = kMoveNames[kind];
    readout.label_cost = cost;
    if (!ok) {
        ++counts.dashes_refused;
        readout.refused_at = t;
        SetLabel(Label::DashTired, t);
        readout.label_cost = cost;
        Tired(t);
        Print(Fmt("dash: %s refused: no stamina (SP %.1f <= 0)", kMoveNames[kind], before));
        Json(Fmt("{\"t\": %.4f, \"event\": \"dash\", \"move\": \"%s\", \"refused\": \"no "
                 "stamina\", \"cost\": %g, \"sp\": %.2f}",
                 t, kMoveNames[kind], cost, before));
        return false;
    }
    ++counts.dashes;
    regen_at = std::max(regen_at, t + config.stamina.regen_delay_s);
    SetLabel(Label::Dash, t, true);
    if (readout.label == Label::Dash) {
        readout.label_cost = cost;
    }
    readout.charged_at = t;
    readout.charged_from = before;
    readout.charged_to = stamina.Current();
    Print(Fmt("dash: %s paid %g: SP %.1f -> %.1f", kMoveNames[kind], cost, before,
              stamina.Current()));
    Json(Fmt("{\"t\": %.4f, \"event\": \"dash\", \"move\": \"%s\", \"cost\": %g, \"sp_before\": "
             "%.2f, \"sp\": %.2f}",
             t, kMoveNames[kind], cost, before, stamina.Current()));
    return true;
}

void Combat::SetLabel(Label label, double t, bool weak) {
    if (weak && Strong(readout.label) && t - readout.label_t < 0.8) {
        return; // a fresh attack, hit or move label stays
    }
    readout.label = label;
    readout.label_t = t;
    readout.result = Result::None;
    if (!weak) {
        readout.label_paid = 0.0f;
    }
}

void Combat::Haptic(int priority, const Pulse& p) {
    if (priority > haptic_priority) {
        haptic_priority = priority;
        haptic = p;
    }
}

void Combat::Transform(const char* by, const swing::TransformEvent& e, const std::string& form,
                       bool blocked, double t, CombatEvents& out) {
    (void)blocked;
    const bool snap = std::string(by) == "snap";
    if (!weapon.two_forms) {
        ++counts.transforms_none;
        Print(Fmt("transform (%s): %s has no other form", by,
                  weapon.name.empty() ? "(empty)" : weapon.name.c_str()));
        Json(Fmt("{\"t\": %.4f, \"event\": \"transform\", \"by\": \"%s\", \"result\": "
                 "\"no_form\"}",
                 t, by));
        due.push_back({t, {side, config.transform_none.amplitude, config.transform_none.seconds}, true});
        return;
    }
    const std::string next = form == "B" || form == "b" ? "A" : "B";
    out.set_form = next;
    ++counts.transforms;
    counts.transforms_button += snap ? 0 : 1;
    readout.form_t = t;
    readout.form = next;
    ResetSwings(); // the old form's open swings end with it
    Print(Fmt("transform (%s): %s form %s -> %s%s", by, weapon.name.c_str(), form.c_str(),
              next.c_str(),
              snap ? Fmt(" (%.0f deg in %.0f ms, hand moved %.0f mm)", e.angle_deg,
                         e.turn_s * 1000.0, e.travel_m * 1000.0)
                         .c_str()
                   : ""));
    Json(Fmt("{\"t\": %.4f, \"event\": \"transform\", \"by\": \"%s\", \"result\": \"%s\", "
             "\"weapon\": %s, \"from\": \"%s\", \"angle\": %.1f, \"turn_ms\": %.0f}",
             t, by, next.c_str(), JsonText(weapon.name).c_str(), form.c_str(), e.angle_deg,
             e.turn_s * 1000.0));
    due.push_back({t, {side, config.transform_haptic.amplitude, config.transform_haptic.seconds}, true});
    due.push_back({t + std::max(config.transform_gap_s, double(config.transform_haptic.seconds) + 0.01),
                   {side, config.transform_lock.amplitude, config.transform_lock.seconds},
                   true});
}

void Combat::Tired(double t) {
    // Two short buzzes: the swing or the move did not register.
    due.push_back({t, {1, config.tired.amplitude, config.tired.seconds}});
    due.push_back({t + std::max(config.tired_gap_s, double(config.tired.seconds) + 0.02),
                   {1, config.tired.amplitude, config.tired.seconds}});
}

void Combat::Print(const std::string& line) {
    if (config.log) {
        pending_log += line + "\n";
    }
}

void Combat::Json(const std::string& object) {
    if (config.log_file.empty()) {
        return;
    }
    if (!log_file || log_path != config.log_file) {
        if (log_file) {
            std::fclose(log_file);
        }
        log_path = config.log_file;
        log_file = std::fopen(log_path.c_str(), "ab");
        if (log_file) {
            char date[64];
            const std::time_t wall = std::time(nullptr);
            std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&wall));
            std::fprintf(log_file,
                         "{\"t\": %.4f, \"event\": \"session\", \"date\": \"%s\", \"weapon\": %s, "
                         "\"class\": %s, \"costs\": [%g, %g, %g], \"stamina_max\": %g}\n",
                         now, date, JsonText(weapon.name).c_str(),
                         JsonText(readout.weapon_class).c_str(), Cost(0), Cost(1), Cost(2),
                         config.stamina.max);
            pending_log += "swing: log " + log_path + "\n";
        } else {
            pending_log += "swing: cannot write the log " + log_path + "\n";
        }
    }
    if (log_file) {
        std::fputs((object + "\n").c_str(), log_file);
        std::fflush(log_file);
    }
}

std::string Combat::Samples(double from) const {
    if (!config.log_samples) {
        return "";
    }
    // [t, grip x y z, grip qx qy qz qw, head x y z, head qx qy qz qw] (tracking space)
    std::string out = ", \"samples\": [";
    bool first = true;
    for (const swing::HandSample& s : samples) {
        if (s.t < from) {
            continue;
        }
        out += Fmt("%s[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]",
                   first ? "" : ",", s.t, s.position.x, s.position.y, s.position.z,
                   s.orientation.x, s.orientation.y, s.orientation.z, s.orientation.w,
                   s.head_position.x, s.head_position.y, s.head_position.z,
                   s.head_orientation.x, s.head_orientation.y, s.head_orientation.z,
                   s.head_orientation.w);
        first = false;
    }
    return out + "]";
}

} // namespace sb
