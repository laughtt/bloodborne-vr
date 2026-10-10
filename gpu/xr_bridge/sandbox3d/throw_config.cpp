// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "throw_config.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "bbport_xr_bridge.h"

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
void Read(const json::Value& v, float (&out)[2]) {
    if (v.IsArray() && v.Size() >= 2) {
        out[0] = v[0].Float();
        out[1] = v[1].Float();
    }
}
void Read(const json::Value& v, float (&out)[3]) {
    if (v.IsArray() && v.Size() >= 3) {
        for (size_t i = 0; i < 3; ++i) {
            out[i] = v[i].Float();
        }
    }
}
void Read(const json::Value& v, m3::Vec3& out) {
    if (v.IsArray() && v.Size() >= 3) {
        out = {v[0].Float(), v[1].Float(), v[2].Float()};
    }
}
void Read(const json::Value& v, ThrowOffset& out) {
    Read(v["position"], out.position);
    Read(v["rotation_deg"], out.rotation_deg);
}

struct Row {
    int goods;
    const char* name;
    float init, gin, gout, accel, maxv, life, range;
    int num;
    float angle;
    bool penetrate, back;
    float fuse;
    const char* effect;
    float color[3];
    const char* mesh;
    const char* shape;
    float scale, radius;
    m3::Vec3 align;
    float spin;
    ThrowOffset grip;
    int sfx;
    int hit_bullet;
};

// The Bullet columns are the dump's (param/gameparam, scripts/vr_assets/throwables_extract.py
// rewrites them from the files when out/vr_assets/throwables/throwables.json exists). Not from the
// game: fuse_s (the delayed items' burst delay is not in the Bullet row), the look, the grip.
// Tiny Tonitrus' gravityInRange of 500 is not a physical gravity (life 0.2 s, "nearly straight"):
// any in-range gravity of 100 or more is read as 0.
const Row kRows[] = {
    {1200, "Molotov Cocktail", 15, 8, 12, 0, 50, 4, 6, 1, 0, false, false, 0, "fire",
     {1.0f, 0.5f, 0.1f}, "th_1200", "bottle", 0.55f, 0.04f, {0, 0, 0}, 360, {{0, 0.03f, -0.03f}, {0, 0, 0}},
     20100, 101},
    {1201, "Delayed Molotov", 15, 40, 20, 0, 15, 4, 2, 1, 0, true, false, 1.5f, "fire",
     {1.0f, 0.75f, 0.25f}, "th_1201", "bottle", 0.55f, 0.04f, {0, 0, 0}, 360, {{0, 0.03f, -0.03f}, {0, 0, 0}},
     20120, 106},
    {1210, "Poison Knife", 20, 0, 9.8f, 10, 50, 2, 8, 1, 0, false, false, 0, "poison",
     {0.35f, 1.0f, 0.35f}, "th_1210", "knife", 1.1f, 0.02f, {0, 0, 1}, 0, {{0, 0.0f, -0.075f}, {0, 180, 0}},
     20105, -1},
    {1220, "Oil Urn", 15, 8, 12, 0, 50, 4, 6, 1, 0, false, false, 0, "oil",
     {0.9f, 0.6f, 0.1f}, "th_1220", "urn", 0.7f, 0.06f, {0, 0, 0}, 300, {{0, 0.03f, -0.04f}, {0, 0, 0}},
     20101, 151},
    {1230, "Pebble", 20, 3, 16, 0, 20, 4, 10, 1, 0, false, false, 0, "dust",
     {0.72f, 0.7f, 0.64f}, "th_1230", "stone", 0.5f, 0.03f, {0, 0, 0}, 540, {{0, 0.02f, -0.03f}, {0, 0, 0}},
     20103, -1},
    {1240, "Throwing Knife", 20, 0, 9.8f, 10, 50, 2, 8, 1, 0, false, false, 0, "dust",
     {0.85f, 0.88f, 0.95f}, "th_1240", "knife", 1.1f, 0.02f, {0, 0, 1}, 0, {{0, 0.0f, -0.075f}, {0, 180, 0}},
     20104, -1},
    {1250, "Rope Molotov Cocktail", 0, 0, 0, 0, 0, 0.03f, 0, 1, 0, false, true, 0, "fire",
     {1.0f, 0.45f, 0.1f}, "th_1250", "bottle", 0.55f, 0.04f, {0, 0, 0}, 0, {{0, 0.03f, -0.03f}, {0, 0, 0}},
     -1, 111},
    {1251, "Delayed Rope Molotov", 0, 0, 0, 0, 0, 0.03f, 0, 1, 0, false, true, 1.5f, "fire",
     {1.0f, 0.7f, 0.25f}, "th_1251", "bottle", 0.55f, 0.04f, {0, 0, 0}, 0, {{0, 0.03f, -0.03f}, {0, 0, 0}},
     -1, 115},
    {1260, "Pungent Blood Cocktail", 15, 0, 12, 0, 50, 4, 0, 1, 0, true, false, 0, "blood",
     {0.8f, 0.05f, 0.1f}, "th_1260", "vial", 1.0f, 0.035f, {0, 0, 0}, 360, {{0, 0.03f, -0.03f}, {0, 0, 0}},
     20107, 161},
    {1270, "Numbing Mist", 15, 8, 12, 0, 50, 4, 6, 1, 0, false, false, 0, "mist",
     {0.6f, 0.8f, 1.0f}, "th_1270", "vial", 1.0f, 0.035f, {0, 0, 0}, 360, {{0, 0.03f, -0.03f}, {0, 0, 0}},
     20108, 181},
    {1410, "Shining Coins", 2, 9.8f, 9.8f, 0, 20, 2.04f, 20, 1, 0, true, false, 0, "coin",
     {1.0f, 0.85f, 0.2f}, "th_1410", "disc", 1.5f, 0.02f, {0, 0, 0}, 720, {{0, 0.02f, -0.03f}, {0, 0, 0}},
     20133, 301},
    {2070, "Tiny Tonitrus", 33, 0, 60, 0, 33, 0.2f, 15, 1, 0, true, false, 0, "lightning",
     {0.6f, 0.8f, 1.0f}, "th_2070", "stone", 0.7f, 0.03f, {0, 0, 0}, 0, {{0, 0.02f, -0.04f}, {0, 0, 0}},
     202007, 411},
    {2080, "Executioner's Gloves", 4, 0, 0, 40, 4, 3, 20, 3, -20, false, false, 0, "slash",
     {0.9f, 0.2f, 0.2f}, "th_2080", "stone", 1.0f, 0.05f, {0, 0, 0}, 0, {{0, 0.02f, -0.03f}, {0, 0, 0}},
     202008, -1},
    {2120, "Blacksky Eye", 30, 0, 0, 0, 30, 0.54f, 16, 1, 0, false, false, 0, "eye",
     {0.7f, 0.4f, 1.0f}, "", "sphere", 1.0f, 0.03f, {0, 0, 0}, 0, {{0, 0.02f, -0.03f}, {0, 0, 0}},
     202050, 461},
    {2130, "Accursed Brew", 15, 3, 7, 10, 20, 0.5f, 7.6f, 1, 0, false, false, 0, "poison",
     {0.6f, 0.25f, 0.85f}, "th_2130", "vial", 1.2f, 0.04f, {0, 0, 0}, 360, {{0, 0.03f, -0.03f}, {0, 0, 0}},
     202051, 471},
};

} // namespace

std::vector<ThrowItemDef> DefaultThrowItems() {
    std::vector<ThrowItemDef> out;
    for (const Row& r : kRows) {
        ThrowItemDef d;
        d.goods = r.goods;
        d.name = r.name;
        d.initial_velocity = r.init;
        d.gravity_in = r.gin;
        d.gravity_out = r.gout;
        d.accel_in = r.accel;
        d.max_velocity = r.maxv;
        d.life_s = r.life;
        d.range_m = r.range;
        d.num_shoot = r.num;
        d.shoot_angle_deg = r.angle;
        d.penetrate = r.penetrate;
        d.back_throw = r.back;
        d.fuse_s = r.fuse;
        d.effect = r.effect;
        for (int k = 0; k < 3; ++k) {
            d.color[k] = r.color[k];
        }
        d.mesh = r.mesh;
        d.shape = r.shape;
        d.scale = r.scale;
        d.radius_m = r.radius;
        d.align = r.align;
        d.spin_dps = r.spin;
        d.grip = r.grip;
        d.sfx_flight = r.sfx;
        d.hit_bullet = r.hit_bullet;
        out.push_back(d);
    }
    return out;
}

void ApplyItemOverride(ThrowItemDef& d, const json::Value& o) {
    if (!o.IsObject()) {
        return;
    }
    Read(o["initial_velocity"], d.initial_velocity);
    Read(o["gravity_in_range"], d.gravity_in);
    Read(o["gravity_out_range"], d.gravity_out);
    Read(o["accel_in_range"], d.accel_in);
    Read(o["max_velocity"], d.max_velocity);
    Read(o["life_s"], d.life_s);
    Read(o["range_m"], d.range_m);
    Read(o["num_shoot"], d.num_shoot);
    Read(o["shoot_angle_deg"], d.shoot_angle_deg);
    Read(o["penetrate"], d.penetrate);
    Read(o["back_throw"], d.back_throw);
    Read(o["fuse_s"], d.fuse_s);
    Read(o["effect"], d.effect);
    Read(o["color"], d.color);
    Read(o["mesh"], d.mesh);
    Read(o["shape"], d.shape);
    Read(o["scale"], d.scale);
    Read(o["radius_m"], d.radius_m);
    Read(o["align"], d.align);
    Read(o["spin_dps"], d.spin_dps);
    Read(o["grip"], d.grip);
    d.num_shoot = std::max(d.num_shoot, 1);
    d.scale = std::max(d.scale, 0.05f);
    d.radius_m = std::clamp(d.radius_m, 0.005f, 0.5f);
}

int ApplyExtractedTable(std::vector<ThrowItemDef>& defs, const json::Value& table) {
    const json::Value& rows = table["items"];
    int matched = 0;
    for (size_t i = 0; i < rows.Size(); ++i) {
        const json::Value& r = rows[i];
        for (ThrowItemDef& d : defs) {
            if (d.goods != r["goods"].Int(-1)) {
                continue;
            }
            ++matched;
            d.bullet_overrides = true;
            Read(r["initVellocity"], d.initial_velocity);
            Read(r["gravityInRange"], d.gravity_in);
            if (d.gravity_in >= 100.0f) {
                d.gravity_in = 0.0f; // Tiny Tonitrus' 500: not a gravity
            }
            Read(r["gravityOutRange"], d.gravity_out);
            Read(r["accelInRange"], d.accel_in);
            Read(r["maxVellocity"], d.max_velocity);
            Read(r["life"], d.life_s);
            Read(r["dist"], d.range_m);
            Read(r["numShoot"], d.num_shoot);
            Read(r["shootAngle"], d.shoot_angle_deg);
            if (r["isPenetrate"].IsNumber()) {
                d.penetrate = r["isPenetrate"].Int() != 0;
            }
            Read(r["sfxId_Bullet"], d.sfx_flight);
            if (r["HitBulletID"].IsNumber()) {
                d.hit_bullet = r["HitBulletID"].Int();
            }
            d.num_shoot = std::max(d.num_shoot, 1);
        }
    }
    return matched;
}

uint32_t ThrowButton(const std::string& name) {
    struct Entry {
        const char* name;
        uint32_t bit;
    };
    static const Entry kButtons[] = {
        {"l3", BBXR_BTN_L3}, {"r3", BBXR_BTN_R3}, {"options", BBXR_BTN_OPTIONS},
        {"dpad_up", BBXR_BTN_UP}, {"dpad_right", BBXR_BTN_RIGHT}, {"dpad_down", BBXR_BTN_DOWN},
        {"dpad_left", BBXR_BTN_LEFT}, {"l2", BBXR_BTN_L2}, {"r2", BBXR_BTN_R2},
        {"l1", BBXR_BTN_L1}, {"r1", BBXR_BTN_R1}, {"triangle", BBXR_BTN_TRIANGLE},
        {"circle", BBXR_BTN_CIRCLE}, {"cross", BBXR_BTN_CROSS}, {"square", BBXR_BTN_SQUARE},
        {"touchpad", BBXR_BTN_TOUCHPAD},
    };
    for (const Entry& e : kButtons) {
        if (name == e.name) {
            return e.bit;
        }
    }
    return 0;
}

void ThrowConfig::Load(const json::Value& d) {
    Read(d["enabled"], enabled);
    Read(d["mode"], mode);
    Read(d["grab_radius_m"], grab_radius_m);
    Read(d["grab_point"], grab_point);
    Read(d["grab_squeeze"], grab_squeeze);
    Read(d["release_squeeze"], release_squeeze);
    Read(d["smooth_ms"], smooth_ms);
    Read(d["peak_weight"], peak_weight);
    Read(d["release_lag_ms"], release_lag_ms);
    Read(d["ref_speed_mps"], ref_speed_mps);
    Read(d["min_scale"], min_scale);
    Read(d["max_scale"], max_scale);
    Read(d["min_throw_speed_mps"], min_throw_speed_mps);
    Read(d["game_pitch_deg"], game_pitch_deg);
    Read(d["game_trigger"], game_trigger);
    Read(d["respawn_s"], respawn_s);
    Read(d["rest_return_s"], rest_return_s);
    Read(d["effect_scale"], effect_scale);
    Read(d["gravity"], grav);
    Read(d["rack_pos"], rack_pos);
    Read(d["rack_height_m"], rack_height_m);
    Read(d["rack_spacing_m"], rack_spacing_m);
    Read(d["rack_columns"], rack_columns);
    Read(d["respawn_button"], respawn_button);
    Read(d["mode_button"], mode_button);
    Read(d["show"], show);
    Read(d["show_markers"], show_markers);
    Read(d["rack_grab"], rack_grab);
    Read(d["log"], log);
    Read(d["log_file"], log_file);
    if (d["items"].IsObject()) {
        items = d["items"];
    }
    grab_radius_m = std::clamp(grab_radius_m, 0.03f, 0.6f);
    release_squeeze = std::min(release_squeeze, grab_squeeze);
    smooth_ms = std::clamp(smooth_ms, 10.0f, 400.0f);
    peak_weight = std::clamp(peak_weight, 0.0f, 1.0f);
    ref_speed_mps = std::max(ref_speed_mps, 0.5f);
    min_scale = std::clamp(min_scale, 0.0f, 5.0f);
    max_scale = std::clamp(max_scale, min_scale, 5.0f);
    rack_columns = std::clamp(rack_columns, 1, 15);
    rack_spacing_m = std::clamp(rack_spacing_m, 0.08f, 0.6f);
    if (mode != "game") {
        mode = "hand";
    }
}

std::string ThrowConfig::Summary(const std::string& in) const {
    std::ostringstream s;
    s << std::boolalpha;
    s << "{\n"
      << in << "  \"enabled\": " << enabled << ", \"mode\": \"" << mode << "\", \"show\": " << show
      << ", \"show_markers\": " << show_markers << ", \"rack_grab\": " << rack_grab << ",\n"
      << in << "  \"grab_radius_m\": " << grab_radius_m << ", \"grab_point\": [" << grab_point.x
      << ", " << grab_point.y << ", " << grab_point.z << "], \"grab_squeeze\": " << grab_squeeze
      << ", \"release_squeeze\": " << release_squeeze << ",\n"
      << in << "  \"smooth_ms\": " << smooth_ms << ", \"peak_weight\": " << peak_weight
      << ", \"release_lag_ms\": " << release_lag_ms << ", \"ref_speed_mps\": " << ref_speed_mps
      << ", \"min_scale\": " << min_scale << ", \"max_scale\": " << max_scale
      << ", \"min_throw_speed_mps\": " << min_throw_speed_mps << ",\n"
      << in << "  \"game_pitch_deg\": " << game_pitch_deg << ", \"game_trigger\": " << game_trigger
      << ", \"respawn_s\": " << respawn_s << ", \"rest_return_s\": " << rest_return_s
      << ", \"effect_scale\": " << effect_scale << ", \"gravity\": " << grav << ",\n"
      << in << "  \"rack_pos\": [" << rack_pos[0] << ", " << rack_pos[1]
      << "], \"rack_height_m\": " << rack_height_m << ", \"rack_spacing_m\": " << rack_spacing_m
      << ", \"rack_columns\": " << rack_columns << ",\n"
      << in << "  \"respawn_button\": \"" << respawn_button << "\", \"mode_button\": \""
      << mode_button << "\",\n"
      << in << "  \"log\": " << log << ", \"log_file\": \"" << log_file << "\",\n"
      << in << "  \"items\": {}\n"
      << in << "}";
    return s.str();
}

std::string ThrowItemsSignature(const json::Value& v) {
    std::ostringstream o;
    switch (v.type) {
    case json::Value::Type::Number: o << v.number; break;
    case json::Value::Type::Bool: o << v.boolean; break;
    case json::Value::Type::String: o << '"' << v.string << '"'; break;
    case json::Value::Type::Array:
        o << '[';
        for (const json::Value& e : v.array) {
            o << ThrowItemsSignature(e) << ',';
        }
        o << ']';
        break;
    case json::Value::Type::Object:
        o << '{';
        for (const auto& kv : v.object) {
            o << kv.first << ':' << ThrowItemsSignature(kv.second) << ',';
        }
        o << '}';
        break;
    default: o << "null"; break;
    }
    return o.str();
}

} // namespace sb
