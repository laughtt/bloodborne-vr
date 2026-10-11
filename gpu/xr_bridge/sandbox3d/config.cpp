// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "config.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "model.h"

namespace sb {
namespace {

double Seconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

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
void Read(const json::Value& v, Offset& out) {
    Read(v["position"], out.position);
    Read(v["rotation_deg"], out.rotation_deg);
}

std::string V(m3::Vec3 v) {
    char text[96];
    std::snprintf(text, sizeof(text), "[%.3g, %.3g, %.3g]", v.x, v.y, v.z);
    return text;
}

std::string O(const Offset& o) {
    return "{\"position\": " + V(o.position) + ", \"rotation_deg\": " + V(o.rotation_deg) + "}";
}

} // namespace

void Config::Load(const json::Value& d) {
    Read(d["render_scale"], render_scale);
    Read(d["near_z"], near_z);
    Read(d["far_z"], far_z);
    Read(d["background"], background);
    Read(d["want_depth"], want_depth);
    Read(d["eye_height_m"], eye_height_m);
    Read(d["arm_scale"], arm_scale);
    Read(d["floor_y"], floor_y);
    Read(d["follow_head_height"], follow_head_height);
    Read(d["eye_offset"], eye_offset);
    Read(d["body_yaw_deadzone_deg"], body_yaw_deadzone_deg);
    Read(d["body_back_m"], body_back_m);
    Read(d["elbow_pole"], elbow_pole);
    Read(d["forearm_twist"], forearm_twist);
    Read(d["hand_offset"], hand_offset);
    Read(d["trigger_curl_deg"], trigger_curl_deg);
    Read(d["grip_curl_deg"], grip_curl_deg);
    Read(d["thumb_curl_deg"], thumb_curl_deg);
    Read(d["rest_curl"], rest_curl);
    Read(d["weapon"], weapon);
    Read(d["left_weapon"], left_weapon);
    Read(d["weapon_grip"], weapon_grip);
    Read(d["left_weapon_grip"], left_weapon_grip);
    Read(d["attack"], attack);
    Read(d["form"], form);
    Read(d["placeholder_radius"], placeholder_radius);
    Read(d["weapon_align"], weapon_align);
    const json::Value& show = d["show"];
    Read(show["body"], show_body);
    Read(show["head"], show_head);
    Read(show["masked"], show_masked);
    Read(show["skeleton"], show_skeleton);
    Read(show["dummies"], show_dummies);
    Read(show["weapon"], show_weapon);
    Read(show["hitboxes"], show_hitboxes);
    Read(show["grid"], show_grid);
    Read(show["controllers"], show_controllers);
    Read(show["laser"], show_laser);
    Read(show["mirror"], show_mirror);
    Read(show["skeleton_on_top"], skeleton_on_top);
    Read(show["hurtboxes"], show_hurtboxes);
    Read(show["hurtboxes_self"], show_hurtboxes_self);
    Read(show["fp_hide"], fp_hide);
    Read(d["mirror_distance_m"], mirror_distance_m);
    physics.Load(d["physics"]);
    throwables.Load(d["throwables"]);
    consumables.Load(d["consumables"]);
    parry.Load(d["parry"]);
    hands.Load(d["hands"]);
    render_scale = std::clamp(render_scale, 0.1f, 2.0f);
    near_z = std::max(near_z, 0.001f);
}

std::string Config::Summary() const {
    std::ostringstream s;
    s << std::boolalpha;
    s << "{\n"
      << "  \"render_scale\": " << render_scale << ",\n"
      << "  \"near_z\": " << near_z << ", \"far_z\": " << far_z << ",\n"
      << "  \"background\": " << V(background) << ",\n"
      << "  \"want_depth\": " << want_depth << ",\n"
      << "  \"eye_height_m\": " << eye_height_m << ",\n"
      << "  \"arm_scale\": " << arm_scale << ",\n"
      << "  \"floor_y\": " << floor_y << ",\n"
      << "  \"follow_head_height\": " << follow_head_height << ",\n"
      << "  \"eye_offset\": " << V(eye_offset) << ",\n"
      << "  \"body_yaw_deadzone_deg\": " << body_yaw_deadzone_deg << ",\n"
      << "  \"body_back_m\": " << body_back_m << ",\n"
      << "  \"elbow_pole\": " << V(elbow_pole) << ",\n"
      << "  \"forearm_twist\": " << forearm_twist << ",\n"
      << "  \"hand_offset\": " << O(hand_offset) << ",\n"
      << "  \"trigger_curl_deg\": " << trigger_curl_deg << ",\n"
      << "  \"grip_curl_deg\": " << grip_curl_deg << ",\n"
      << "  \"thumb_curl_deg\": " << thumb_curl_deg << ",\n"
      << "  \"rest_curl\": " << rest_curl << ",\n"
      << "  \"weapon\": \"" << weapon << "\",\n"
      << "  \"left_weapon\": \"" << left_weapon << "\",\n"
      << "  \"weapon_grip\": " << O(weapon_grip) << ",\n"
      << "  \"left_weapon_grip\": " << O(left_weapon_grip) << ",\n"
      << "  \"form\": \"" << form << "\",\n"
      << "  \"attack\": " << attack << ",\n"
      << "  \"placeholder_radius\": " << placeholder_radius << ",\n"
      << "  \"weapon_align\": " << weapon_align << ",\n"
      << "  \"mirror_distance_m\": " << mirror_distance_m << ",\n"
      << "  \"physics\": " << physics.Summary("  ") << ",\n"
      << "  \"throwables\": " << throwables.Summary("  ") << ",\n"
      << "  \"consumables\": " << consumables.Summary("  ") << ",\n"
      << "  \"parry\": " << parry.Summary("  ") << ",\n"
      << "  \"hands\": " << hands.Summary() << ",\n"
      << "  \"show\": {\"body\": " << show_body << ", \"head\": " << show_head
      << ", \"masked\": " << show_masked << ", \"skeleton\": " << show_skeleton
      << ", \"dummies\": " << show_dummies << ", \"weapon\": " << show_weapon
      << ", \"hitboxes\": " << show_hitboxes << ", \"grid\": " << show_grid
      << ", \"controllers\": " << show_controllers << ", \"laser\": " << show_laser
      << ", \"mirror\": " << show_mirror << ", \"skeleton_on_top\": " << skeleton_on_top
      << ", \"hurtboxes\": " << show_hurtboxes
      << ", \"hurtboxes_self\": " << show_hurtboxes_self
      << ", \"fp_hide\": \"" << fp_hide << "\"}\n}\n";
    return s.str();
}

bool WriteDefaultConfig(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary);
    file << Config{}.Summary();
    return bool(file);
}

ConfigFile::ConfigFile(std::filesystem::path config_path)
    : path(std::move(config_path)),
      calibration(path.parent_path() / (path.stem().string() + "_calibration.json")) {}

bool ConfigFile::Poll(Config& config, std::string& log) {
    const double now = Seconds();
    if (loaded && now < next_poll) {
        return false;
    }
    next_poll = now + 0.5;
    std::error_code ec;
    const auto stamp_now = std::filesystem::last_write_time(path, ec);
    std::error_code ec2;
    const auto calibration_now = std::filesystem::last_write_time(calibration, ec2);
    if (loaded && !ec && stamp_now == stamp &&
        (ec2 ? true : calibration_now == calibration_stamp)) {
        return false;
    }
    Config next;
    if (!ec) {
        std::string error;
        const json::Value doc = json::Parse(ReadText(path), &error);
        if (!error.empty()) {
            log = path.string() + ": " + error + " (keeping the previous settings)";
            stamp = stamp_now;
            loaded = true;
            return false;
        }
        next.Load(doc);
        stamp = stamp_now;
    }
    if (!ec2) {
        const json::Value cal = json::Parse(ReadText(calibration));
        Read(cal["eye_height_m"], next.eye_height_m);
        Read(cal["arm_scale"], next.arm_scale);
        calibration_stamp = calibration_now;
    }
    config = next;
    log = std::string(loaded ? "config reloaded from " : "config loaded from ") + path.string() +
          (ec2 ? "" : " (+ " + calibration.filename().string() + ")") + ":\n" + config.Summary();
    loaded = true;
    return true;
}

void ConfigFile::SaveCalibration(float eye_height_m, float arm_scale, std::string& log) {
    std::ofstream file(calibration, std::ios::binary);
    char text[160];
    std::snprintf(text, sizeof(text), "{\"eye_height_m\": %.3f, \"arm_scale\": %.3f}\n",
                  eye_height_m, arm_scale);
    file << text;
    log = "calibration saved to " + calibration.string() + ": " + text;
}

} // namespace sb
