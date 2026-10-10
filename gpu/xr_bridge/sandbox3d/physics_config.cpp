// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "physics_config.h"

#include <algorithm>
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
void Read(const json::Value& v, float (&out)[2]) {
    if (v.IsArray() && v.Size() >= 2) {
        out[0] = v[0].Float();
        out[1] = v[1].Float();
    }
}
void Read(const json::Value& v, m3::Vec3& out) {
    if (v.IsArray() && v.Size() >= 3) {
        out = {v[0].Float(), v[1].Float(), v[2].Float()};
    }
}

/// {"radius_scale": x|null, "reach_m": y|null} of a weapon class (null: the global value).
std::string HitClassJson(const VhsParams& h, int c) {
    std::ostringstream o;
    o << "{\"radius_scale\": ";
    if (h.class_radius_scale[c] >= 0.0f) {
        o << h.class_radius_scale[c];
    } else {
        o << "null";
    }
    o << ", \"reach_m\": ";
    if (h.class_reach_m[c] >= VHS_REACH_MIN_M) {
        o << h.class_reach_m[c];
    } else {
        o << "null";
    }
    return o.str() + "}";
}

} // namespace

void PhysicsConfig::Load(const json::Value& d) {
    if (!d.IsObject()) {
        return;
    }
    Read(d["enabled"], enabled);
    Read(d["weapons_collide"], weapons_collide);
    Read(d["floor"], floor);
    Read(d["enemy_solid"], enemy_solid);
    Read(d["show_colliders"], show_colliders);
    Read(d["show_ghost"], show_ghost);
    Read(d["ghost_min_m"], ghost_min_m);
    Read(d["stiffness"], stiffness);
    Read(d["iterations"], iterations);
    Read(d["max_substeps"], max_substeps);
    Read(d["max_step_m"], max_step_m);
    Read(d["max_separation_m"], max_separation_m);
    Read(d["max_separation_arc_m"], max_separation_arc_m);
    Read(d["teleport_m"], teleport_m);
    Read(d["teleport_deg"], teleport_deg);
    Read(d["swing_through_enemy"], swing_through_enemy);
    Read(d["hand_mass_kg"], hand_mass_kg);
    Read(d["weapon_mass_kg"], weapon_mass_kg);
    Read(d["hand_gyration_m"], hand_gyration_m);
    const json::Value& shape = d["shape"];
    Read(shape["slice_m"], slice_m);
    Read(shape["radius_min_m"], radius_min_m);
    Read(shape["radius_max_m"], radius_max_m);
    Read(shape["radius_scale"], radius_scale);
    Read(shape["max_lanes"], max_lanes);
    const json::Value& h = d["haptics"];
    Read(h["impact_min_speed"], impact_min_speed);
    Read(h["impact_amplitude_min"], impact_amplitude_min);
    Read(h["impact_amplitude_per_mps"], impact_amplitude_per_mps);
    Read(h["impact_amplitude_max"], impact_amplitude_max);
    Read(h["impact_seconds"], impact_seconds);
    Read(h["impact_seconds_per_mps"], impact_seconds_per_mps);
    Read(h["impact_seconds_max"], impact_seconds_max);
    Read(h["slide_min_speed"], slide_min_speed);
    Read(h["slide_amplitude_min"], slide_amplitude_min);
    Read(h["slide_amplitude_per_mps"], slide_amplitude_per_mps);
    Read(h["slide_amplitude_max"], slide_amplitude_max);
    Read(h["buzz_interval_s"], buzz_interval_s);
    Read(h["buzz_seconds"], buzz_seconds);
    Read(h["release_s"], release_s);
    const json::Value& m = d["melee"];
    Read(m["hits"], hits);
    Read(m["hit_speed"], hit_speed);
    Read(m["rearm_speed"], rearm_speed);
    Read(m["rearm_reverse_deg"], rearm_reverse_deg);
    Read(m["attack_radius_scale"], attack_radius_scale);
    Read(m["hit_amplitude"], hit_amplitude);
    Read(m["hit_seconds"], hit_seconds);
    Read(m["marker_s"], marker_s);
    const json::Value& ht = d["hit"];
    if (ht.IsObject()) {
        bool on = hit.enabled != 0;
        Read(ht["enabled"], on);
        hit.enabled = on;
        Read(ht["radius_scale"], hit.radius_scale);
        Read(ht["reach_m"], hit.reach_m);
        static const char* const kClass[VHS_CLASSES] = {"short", "medium", "long"};
        for (int c = 0; c < VHS_CLASSES; ++c) {
            const json::Value& o = ht[kClass[c]];
            Read(o["radius_scale"], hit.class_radius_scale[c]);
            Read(o["reach_m"], hit.class_reach_m[c]);
        }
        vhs_sanitize(&hit);
    }
    const json::Value& p = d["props"];
    Read(p["show"], props);
    Read(p["post"], post);
    Read(p["post_radius"], post_radius);
    Read(p["post_height"], post_height);
    Read(p["crossbar_height"], crossbar_height);
    Read(p["crossbar_length"], crossbar_length);
    Read(p["crossbar_radius"], crossbar_radius);
    Read(p["wall"], wall);
    Read(p["wall_size"], wall_size);
    Read(p["wall_yaw_deg"], wall_yaw_deg);
    Read(p["enemy"], enemy);
    Read(p["enemy_pos"], enemy_pos);
    Read(p["enemy_face_user"], enemy_face_user);
    Read(p["enemy_yaw_deg"], enemy_yaw_deg);
    Read(d["log_contacts"], log_contacts);
    Read(d["contact_log"], contact_log);
    // Keep the solver sane whatever the file says.
    stiffness = std::clamp(stiffness, 0.01f, 1.0f);
    iterations = std::clamp(iterations, 1, 16);
    max_substeps = std::clamp(max_substeps, 1, 256);
    max_step_m = std::max(max_step_m, 0.0f);
    hand_mass_kg = std::max(hand_mass_kg, 0.01f);
    weapon_mass_kg = std::max(weapon_mass_kg, 0.01f);
    hand_gyration_m = std::max(hand_gyration_m, 0.01f);
    slice_m = std::max(slice_m, 0.02f);
    radius_min_m = std::max(radius_min_m, 0.002f);
    radius_max_m = std::max(radius_max_m, radius_min_m);
    max_lanes = std::clamp(max_lanes, 1, 8);
}

std::string PhysicsConfig::Summary(const std::string& in) const {
    const auto pair = [](const float v[2]) {
        std::ostringstream s;
        s << "[" << v[0] << ", " << v[1] << "]";
        return s.str();
    };
    std::ostringstream s;
    s << std::boolalpha;
    s << "{\n"
      << in << "  \"enabled\": " << enabled << ", \"weapons_collide\": " << weapons_collide
      << ", \"floor\": " << floor << ", \"enemy_solid\": " << enemy_solid << ",\n"
      << in << "  \"show_colliders\": " << show_colliders << ", \"show_ghost\": " << show_ghost
      << ", \"ghost_min_m\": " << ghost_min_m << ",\n"
      << in << "  \"stiffness\": " << stiffness << ", \"iterations\": " << iterations
      << ", \"max_substeps\": " << max_substeps << ", \"max_step_m\": " << max_step_m << ",\n"
      << in << "  \"max_separation_m\": " << max_separation_m
      << ", \"max_separation_arc_m\": " << max_separation_arc_m << ", \"teleport_m\": "
      << teleport_m << ", \"teleport_deg\": " << teleport_deg << ", \"swing_through_enemy\": "
      << swing_through_enemy << ",\n"
      << in << "  \"hand_mass_kg\": " << hand_mass_kg << ", \"weapon_mass_kg\": "
      << weapon_mass_kg << ", \"hand_gyration_m\": " << hand_gyration_m << ",\n"
      << in << "  \"shape\": {\"slice_m\": " << slice_m << ", \"radius_min_m\": " << radius_min_m
      << ", \"radius_max_m\": " << radius_max_m << ", \"radius_scale\": " << radius_scale
      << ", \"max_lanes\": " << max_lanes << "},\n"
      << in << "  \"haptics\": {\"impact_min_speed\": " << impact_min_speed
      << ", \"impact_amplitude_min\": " << impact_amplitude_min
      << ", \"impact_amplitude_per_mps\": " << impact_amplitude_per_mps
      << ", \"impact_amplitude_max\": " << impact_amplitude_max << ",\n"
      << in << "    \"impact_seconds\": " << impact_seconds
      << ", \"impact_seconds_per_mps\": " << impact_seconds_per_mps
      << ", \"impact_seconds_max\": " << impact_seconds_max << ",\n"
      << in << "    \"slide_min_speed\": " << slide_min_speed
      << ", \"slide_amplitude_min\": " << slide_amplitude_min
      << ", \"slide_amplitude_per_mps\": " << slide_amplitude_per_mps
      << ", \"slide_amplitude_max\": " << slide_amplitude_max << ",\n"
      << in << "    \"buzz_interval_s\": " << buzz_interval_s << ", \"buzz_seconds\": "
      << buzz_seconds << ", \"release_s\": " << release_s << "},\n"
      << in << "  \"melee\": {\"hits\": " << hits << ", \"hit_speed\": " << hit_speed
      << ", \"rearm_speed\": " << rearm_speed << ", \"rearm_reverse_deg\": "
      << rearm_reverse_deg << ", \"attack_radius_scale\": "
      << attack_radius_scale << ",\n"
      << in << "    \"hit_amplitude\": " << hit_amplitude << ", \"hit_seconds\": " << hit_seconds
      << ", \"marker_s\": " << marker_s << "},\n"
      << in << "  \"hit\": {\"enabled\": " << (hit.enabled ? "true" : "false")
      << ", \"radius_scale\": " << hit.radius_scale << ", \"reach_m\": " << hit.reach_m << ",\n"
      << in << "    \"short\": " << HitClassJson(hit, 0) << ", \"medium\": " << HitClassJson(hit, 1)
      << ", \"long\": " << HitClassJson(hit, 2) << "},\n"
      << in << "  \"props\": {\"show\": " << props << ", \"post\": " << pair(post)
      << ", \"post_radius\": " << post_radius << ", \"post_height\": " << post_height << ",\n"
      << in << "    \"crossbar_height\": " << crossbar_height << ", \"crossbar_length\": "
      << crossbar_length << ", \"crossbar_radius\": " << crossbar_radius << ",\n"
      << in << "    \"wall\": " << pair(wall) << ", \"wall_size\": [" << wall_size.x << ", "
      << wall_size.y << ", " << wall_size.z << "], \"wall_yaw_deg\": " << wall_yaw_deg << ",\n"
      << in << "    \"enemy\": \"" << enemy << "\", \"enemy_pos\": " << pair(enemy_pos)
      << ", \"enemy_face_user\": " << enemy_face_user << ", \"enemy_yaw_deg\": "
      << enemy_yaw_deg << "},\n"
      << in << "  \"log_contacts\": " << log_contacts << ", \"contact_log\": \"" << contact_log
      << "\"\n"
      << in << "}";
    return s.str();
}

} // namespace sb
