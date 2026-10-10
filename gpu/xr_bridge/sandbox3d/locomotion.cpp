// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "locomotion.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "model.h"

namespace sb {

using namespace m3;

namespace {

void Read(const json::Value& v, float& out) {
    if (v.IsNumber()) {
        out = v.Float();
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

std::string Quote(const std::string& s) {
    return "\"" + s + "\"";
}

/// The pitch (up positive) of where a rotation turns the forward axis -Z, radians.
float PitchOf(Quat q) {
    const Vec3 f = Rotate(q, {0, 0, -1});
    return std::atan2(f.y, std::sqrt(f.x * f.x + f.z * f.z) * (f.z <= 0.0f ? 1.0f : -1.0f));
}

float WrapPi(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

/// A smooth 0 -> 1 -> 0 bump over [0, 1].
float Bump(float x) {
    return x > 0.0f && x < 1.0f ? std::sin(kPi * x) * std::sin(kPi * x) : 0.0f;
}

Vec3 V3(const json::Value& v) {
    return {v[0].Float(), v[1].Float(), v[2].Float()};
}
Quat Q4(const json::Value& v) {
    return Normalize(Quat{v[0].Float(), v[1].Float(), v[2].Float(), v[3].Float(1.0f)});
}

constexpr uint32_t kA = BBXR_BTN_CIRCLE; // the bridge maps A to circle (the game's dodge)
constexpr float kBlendIn = 0.15f;        // s: the game blends from standing into a move's clip
constexpr float kRecover = 0.3f;         // s: the eye's height back to standing after a cancel

} // namespace

// ---- config -------------------------------------------------------------------------------------

void LocoConfig::Load(const json::Value& d) {
    Read(d["enabled"], enabled);
    Read(d["mode"], mode);
    Read(d["move_speed"], move_speed);
    Read(d["sprint_speed"], sprint_speed);
    Read(d["deadzone"], deadzone);
    Read(d["accel_s"], accel_s);
    turn::Configure(turning, d); // the shared model's keys (turn, snap, turn_speed, move, ...)
    Read(d["dash_duration"], dash_duration);
    Read(d["ease"], ease);
    Read(d["ease_power"], ease_power);
    Read(d["settle_s"], settle_s);
    Read(d["settle_speed"], settle_speed);
    Read(d["max_lead_m"], max_lead_m);
    Read(d["hint"], hint);
    Read(d["dodge_distance"], dodge_distance);
    Read(d["backstep_distance"], backstep_distance);
    Read(d["jump_distance"], jump_distance);
    Read(d["jump_height"], jump_height);
    Read(d["jump_duration"], jump_duration);
    Read(d["jump_flat"], jump_flat);
    Read(d["vignette"], vignette);
    Read(d["vignette_inner_deg"], vignette_inner_deg);
    Read(d["vignette_outer_deg"], vignette_outer_deg);
    Read(d["vignette_walk"], vignette_walk);
    Read(d["vignette_raw"], vignette_raw);
    Read(d["raw_source"], raw_source);
    Read(d["roll_anim"], roll_anim);
    Read(d["backstep_anim"], backstep_anim);
    Read(d["jump_anim"], jump_anim);
    Read(d["raw_rotation"], raw_rotation);
    Read(d["game_fps"], game_fps);
    Read(d["tap_s"], tap_s);
    Read(d["show_paths"], show_paths);
    Read(d["show_landmarks"], show_landmarks);
    dash_duration = std::clamp(dash_duration, 0.02f, 3.0f);
    settle_s = std::clamp(settle_s, 0.0f, 3.0f);
    jump_duration = std::clamp(jump_duration, 0.05f, 3.0f);
    vignette = std::clamp(vignette, 0.0f, 1.0f);
    vignette_walk = std::clamp(vignette_walk, 0.0f, 1.0f);
    raw_rotation = std::clamp(raw_rotation, 0.0f, 1.0f);
    deadzone = std::clamp(deadzone, 0.0f, 0.9f);
    game_fps = std::max(game_fps, 0.0f);
    tap_s = std::clamp(tap_s, 0.05f, 2.0f);
}

std::string LocoConfig::Summary() const {
    std::ostringstream s;
    s << std::boolalpha;
    s << "{\n"
      << "    \"enabled\": " << enabled << ", \"mode\": " << Quote(mode) << ",\n"
      << "    \"move_speed\": " << move_speed << ", \"sprint_speed\": " << sprint_speed
      << ", \"accel_s\": " << accel_s << ",\n"
      << "    " << turn::Summary(turning) << ",\n"
      << "    \"dash_duration\": " << dash_duration << ", \"ease\": " << Quote(ease)
      << ", \"ease_power\": " << ease_power << ", \"settle_s\": " << settle_s
      << ", \"settle_speed\": " << settle_speed << ", \"max_lead_m\": " << max_lead_m
      << ", \"hint\": " << Quote(hint) << ",\n"
      << "    \"dodge_distance\": " << dodge_distance << ", \"backstep_distance\": " << backstep_distance
      << ", \"jump_distance\": " << jump_distance << ",\n"
      << "    \"jump_height\": " << jump_height << ", \"jump_duration\": " << jump_duration
      << ", \"jump_flat\": " << jump_flat << ",\n"
      << "    \"vignette\": " << vignette << ", \"vignette_inner_deg\": " << vignette_inner_deg
      << ", \"vignette_outer_deg\": " << vignette_outer_deg << ", \"vignette_walk\": " << vignette_walk
      << ", \"vignette_raw\": " << vignette_raw << ",\n"
      << "    \"raw_source\": " << Quote(raw_source) << ", \"roll_anim\": " << Quote(roll_anim)
      << ", \"backstep_anim\": " << Quote(backstep_anim) << ", \"jump_anim\": " << Quote(jump_anim) << ",\n"
      << "    \"raw_rotation\": " << raw_rotation << ", \"game_fps\": " << game_fps << ", \"tap_s\": " << tap_s
      << ",\n"
      << "    \"show_paths\": " << show_paths << ", \"show_landmarks\": " << show_landmarks << "\n"
      << "  }";
    return s.str();
}

VrDashParams DashParams(const LocoConfig& c) {
    VrDashParams p;
    vr_dash_params_default(&p);
    p.duration = c.dash_duration;
    p.curve = VR_DASH_EASE_POWER;
    p.power = 2.0f;
    if (c.ease == "linear") p.power = 1.0f;
    else if (c.ease == "cubic") p.power = 3.0f;
    else if (c.ease == "quart") p.power = 4.0f;
    else if (c.ease == "quint") p.power = 5.0f;
    else if (c.ease == "expo") p.curve = VR_DASH_EASE_EXPO, p.power = 10.0f;
    else if (c.ease == "smooth") p.curve = VR_DASH_EASE_SMOOTH;
    if (c.ease_power > 0.0f) {
        p.power = c.ease_power;
    }
    p.settle = c.settle_s;
    p.settle_speed = std::max(c.settle_speed, 0.0f);
    p.max_lead = c.max_lead_m;
    p.jump_height = c.jump_flat ? 0.0f : c.jump_height;
    p.jump_duration = c.jump_duration;
    return p;
}

bool LocoConfigFile::Poll(double now, LocoConfig& config, std::string& log) {
    if (loaded && now < next) {
        return false;
    }
    next = now + 0.5;
    std::error_code ec;
    const auto stamp_now = std::filesystem::last_write_time(path, ec);
    if (ec) {
        if (!loaded) {
            loaded = true;
            log = "locomotion: no " + path.string() + ", defaults:\n  \"locomotion\": " + config.Summary() + "\n";
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
        log = "locomotion: " + path.string() + ": " + error + " (keeping the previous settings)\n";
        loaded = true;
        return false;
    }
    LocoConfig next_config;
    next_config.Load(doc["locomotion"]);
    if (!doc.Has("locomotion") && !inserted) {
        // Add the section with every default, keeping the rest of the file as it is.
        inserted = true;
        const size_t close = text.find_last_of('}');
        if (close != std::string::npos) {
            size_t body = close;
            while (body > 0 && std::isspace(static_cast<unsigned char>(text[body - 1]))) {
                --body;
            }
            const bool empty = body > 0 && text[body - 1] == '{';
            const std::string updated = text.substr(0, body) + (empty ? "\n" : ",\n") + "  \"locomotion\": " +
                                        next_config.Summary() + "\n" + text.substr(close);
            const std::filesystem::path temp = path.string() + ".locomotion.tmp";
            std::ofstream out(temp, std::ios::binary);
            out << updated;
            out.close();
            std::error_code rename_error;
            if (out) {
                std::filesystem::rename(temp, path, rename_error);
            }
            log += rename_error || !out ? "locomotion: could not add the \"locomotion\" section to " + path.string() + "\n"
                                        : "locomotion: added the \"locomotion\" section with the defaults to " +
                                              path.string() + "\n";
        }
    }
    config = next_config;
    log += std::string(loaded ? "locomotion config reloaded" : "locomotion config") + ":\n  \"locomotion\": " +
           config.Summary() + "\n";
    loaded = true;
    return true;
}

// ---- moves --------------------------------------------------------------------------------------

Vec3 MovePath::Eye(float tau) const {
    if (eye.empty()) {
        return {};
    }
    if (tau <= t.front()) {
        return eye.front();
    }
    if (tau >= t.back()) {
        return eye.back();
    }
    const size_t i = size_t(std::upper_bound(t.begin(), t.end(), tau) - t.begin());
    const float f = (tau - t[i - 1]) / std::max(t[i] - t[i - 1], 1e-6f);
    return Lerp(eye[i - 1], eye[i], f);
}

Quat MovePath::Head(float tau) const {
    if (head.empty()) {
        return {};
    }
    if (tau <= t.front()) {
        return head.front();
    }
    if (tau >= t.back()) {
        return head.back();
    }
    const size_t i = size_t(std::upper_bound(t.begin(), t.end(), tau) - t.begin());
    const float f = (tau - t[i - 1]) / std::max(t[i] - t[i - 1], 1e-6f);
    return Slerp(head[i - 1], head[i], f);
}

float MovePath::Distance() const {
    const Vec3 e = End();
    return std::sqrt(e.x * e.x + e.z * e.z);
}

namespace {

/// Samples a synthetic move at 120 Hz: eye(x), pitch(x) over x = 0..1 of `length` seconds.
template <class Eye, class Pitch>
MovePath Synthesize(const char* name, float length, float motion_end, Eye eye, Pitch pitch) {
    MovePath m;
    m.name = name;
    m.source = "synthetic";
    m.length = length;
    m.motion_end = motion_end;
    const int n = std::max(2, int(length * 120.0f));
    for (int i = 0; i <= n; ++i) {
        const float x = float(i) / float(n);
        m.t.push_back(x * length);
        m.eye.push_back(eye(x));
        m.head.push_back(AxisAngle({1, 0, 0}, pitch(x)));
    }
    return m;
}

} // namespace

// The synthetic moves follow the recorded ones' shapes (scripts/vr_assets/dodge_extract.py on
// a000_007200, 007000, 007800): root motion front-loaded, the eye diving and surging, the head
// turning over.
MovePath SyntheticRoll(float distance, float length) {
    const float m = 0.6f; // the root moves over the first 60% of the clip
    return Synthesize(
        "synthetic roll", length, m * length,
        [&](float x) {
            const float r = std::min(x / m, 1.0f);
            const float root = distance * (1.0f - (1.0f - r) * (1.0f - r) * (1.0f - r));
            const float dip = Bump(std::min(x / 0.75f, 1.0f));
            return Vec3{0.12f * std::sin(kPi * r), -1.4f * dip * (1.0f - 0.3f * x),
                        -(root + 0.35f * std::sin(kPi * std::min(x / 0.8f, 1.0f)))};
        },
        [&](float x) { return -2.0f * kPi * Smooth(x / 0.7f); });
}

MovePath SyntheticBackstep(float distance, float length) {
    const float m = 0.7f;
    return Synthesize(
        "synthetic backstep", length, m * length,
        [&](float x) {
            const float r = std::min(x / m, 1.0f);
            const float root = distance * (1.0f - (1.0f - r) * (1.0f - r));
            return Vec3{0.0f, 0.12f * Bump(x / 0.45f) - 0.25f * Bump((x - 0.3f) / 0.6f), root - 0.2f * Bump(x)};
        },
        [](float x) { return 0.35f * Bump(x / 0.8f); });
}

MovePath SyntheticJump(float distance, float length) {
    // The leap (75% of the distance in 40% of the clip, the eye 0.45 m up), then a landing roll.
    return Synthesize(
        "synthetic jump", length, 0.95f * length,
        [&](float x) {
            const float leap = std::min(x / 0.4f, 1.0f), roll = std::clamp((x - 0.4f) / 0.55f, 0.0f, 1.0f);
            const float root = distance * (0.75f * leap + 0.25f * (1.0f - (1.0f - roll) * (1.0f - roll)));
            const float y = 0.45f * Bump(leap) - 1.4f * Bump((x - 0.38f) / 0.5f);
            return Vec3{0.0f, y, -root};
        },
        [](float x) { return -0.3f * Bump(x / 0.4f) - 2.0f * kPi * Smooth((x - 0.4f) / 0.35f); });
}

void MoveSet::Load(const std::filesystem::path& assets, const LocoConfig& config) {
    roll = SyntheticRoll(4.0f, 1.3f);
    backstep = SyntheticBackstep(2.5f, 1.1f);
    jump = SyntheticJump(6.8f, 2.0f);
    log.clear();
    if (config.raw_source == "synthetic") {
        log = "moves: synthetic";
        return;
    }
    const std::filesystem::path file = assets / "dash" / "dodges.json";
    bool ok = false;
    const std::string text = ReadText(file, &ok);
    if (!ok) {
        log = "moves: synthetic (no " + file.string() + "; run scripts/vr_assets/dodge_extract.py)";
        return;
    }
    std::string error;
    const json::Value doc = json::Parse(text, &error);
    const json::Value& anims = doc["anims"];
    if (!error.empty() || !anims.IsArray()) {
        log = "moves: synthetic (" + file.string() + ": " + (error.empty() ? "no anims" : error) + ")";
        return;
    }
    // The animation for a kind: the configured name, else the kind's anim closest to straight
    // (forward for rolls and jumps, back for backsteps).
    const auto pick = [&](const std::string& want, const char* kind, float direction) -> const json::Value* {
        for (const json::Value& a : anims.array) {
            if (!want.empty() && a["name"].Str() == want && a["samples"].Size() >= 3) {
                return &a;
            }
        }
        const json::Value* best = nullptr;
        float best_off = 1e9f;
        for (const json::Value& a : anims.array) {
            if (a["kind_guess"].Str() != kind || a["samples"].Size() < 3) {
                continue;
            }
            const float off = std::fabs(WrapPi((a["summary"]["direction_deg"].Float() - direction) * kDeg));
            if (off < best_off) {
                best_off = off;
                best = &a;
            }
        }
        return best;
    };
    // The clip starts already leaning into the move (frame 0's eye is low and ahead); the game
    // blends from the standing pose into it. Standing = the clip's last frame (it ends standing):
    // the eye at `rest` above and ahead of the root, the head at `cam_rest`.
    const auto convert = [&](const json::Value& a, MovePath& out) {
        const json::Value& samples = a["samples"];
        const size_t n = samples.Size();
        const json::Value& last = samples[n - 1];
        const bool has_cam = last["cam_q"].IsArray();
        const Vec3 rest = V3(last["eye"]) - V3(last["root"]);
        const Quat cam_rest = Q4(has_cam ? last["cam_q"] : last["head_q"]);
        MovePath m;
        m.name = a["name"].Str();
        m.source = "recorded";
        for (size_t i = 0; i < n; ++i) {
            const json::Value& s = samples[i];
            const float t = s["t"].Float();
            const float w = Smooth(t / kBlendIn);
            const Vec3 root = V3(s["root"]);
            const Vec3 eye = Lerp(root + rest, V3(s["eye"]), w);
            // The head's turn from standing, in the move's (world) frame: cam * conj(cam_rest).
            const Quat cam = Q4(has_cam ? s["cam_q"] : s["head_q"]);
            m.t.push_back(t);
            m.eye.push_back(eye - rest);
            m.head.push_back(Slerp(Quat{}, Normalize(cam * Conjugate(cam_rest)), w));
        }
        m.length = std::max(a["duration"].Float(m.t.back()), 0.2f);
        m.motion_end = std::clamp(a["summary"]["t_motion_end"].Float(m.length), 0.1f, m.length);
        out = m;
    };
    struct Want {
        const std::string* name;
        const char* kind;
        float direction;
        MovePath* out;
    } wants[3] = {{&config.roll_anim, "roll", 0.0f, &roll},
                  {&config.backstep_anim, "backstep", 180.0f, &backstep},
                  {&config.jump_anim, "jump", 0.0f, &jump}};
    log = "moves:";
    for (const Want& w : wants) {
        const json::Value* a = pick(*w.name, w.kind, w.direction);
        if (a) {
            convert(*a, *w.out);
        }
        char line[200];
        std::snprintf(line, sizeof(line), " %s %s%s (%.2f m, clip %.2f s, root motion %.2f s)", w.kind,
                      a ? w.out->name.c_str() : "synthetic",
                      a && !w.name->empty() && w.out->name != *w.name ? " [configured one not found]" : "",
                      w.out->Distance(), w.out->length, w.out->motion_end);
        log += line;
    }
}

// ---- locomotion ---------------------------------------------------------------------------------

Locomotion::Locomotion(const std::filesystem::path& config_path)
    : assets(config_path.parent_path()), file(config_path) {
    params = DashParams(config);
    moves.Load(assets, config);
    vr_dash_reset(&dash);
}

TrackedPose Locomotion::ToWorld(const TrackedPose& p) const {
    return {p.valid, TransformPoint(world, p.position), Normalize(ToQuat(world) * p.rotation)};
}

float Locomotion::Sampled(float tau) const {
    return config.game_fps > 0.0f ? std::floor(tau * config.game_fps + 1e-4f) / config.game_fps : tau;
}

void Locomotion::Turn(float radians, Vec3 pivot) {
    const Quat q = AxisAngle({0, 1, 0}, radians);
    const auto point = [&](Vec3 p) { return pivot + Rotate(q, p - pivot); };
    ground = point(ground);
    character = point(character);
    camera = point(camera);
    velocity = Rotate(q, velocity);
    move.ground0 = point(move.ground0);
    move.yaw += radians;
    yaw += radians;
    vr_dash_rotate(&dash, {pivot.x, pivot.y, pivot.z}, std::cos(radians), std::sin(radians));
    for (Vec3& p : path_raw) p = point(p);
    for (Vec3& p : path_camera) p = point(p);
}

Vec3 Locomotion::MoveOffset(const Move& m, float tau) const {
    Vec3 e = m.path->Eye(tau);
    e.x *= m.scale;
    e.z *= m.scale;
    return Rotate(AxisAngle({0, 1, 0}, m.yaw), e);
}

void Locomotion::StartMove(int kind, float direction, double now, std::string& log) {
    if (move.on && (now - move.t0) < std::min(0.6 * move.path->motion_end, 0.5)) {
        return; // not before the current move is well under way (the game's recovery)
    }
    static const char* const kNames[3] = {"dodge", "backstep", "jump"};
    if (move_gate && !move_gate(kind, now)) {
        log += std::string("move refused: ") + kNames[kind] + " (no stamina)\n";
        return;
    }
    if (move.on) { // a chained move starts where this one is now
        const Vec3 off = MoveOffset(move, Sampled(float(now - move.t0)));
        ground = move.ground0 + Vec3{off.x, 0.0f, off.z};
        recover = lift;
        recover_t0 = now;
    }
    move.on = true;
    move.id = ++move_count;
    move.kind = kind;
    move.raw = config.mode == "raw" || (config.mode == "ab" && (move.id % 2) == 0);
    move.t0 = now;
    move.ground0 = ground;
    move.yaw = direction;
    move.path = kind == 0 ? &moves.roll : kind == 1 ? &moves.backstep : &moves.jump;
    const float want = kind == 0 ? config.dodge_distance : kind == 1 ? config.backstep_distance : config.jump_distance;
    move.scale = want > 0.0f && move.path->Distance() > 0.05f ? want / move.path->Distance() : 1.0f;
    velocity = {};
    path_raw.clear();
    path_camera.clear();
    char text[256];
    std::snprintf(text, sizeof(text), "move %d: %s %s (%s %s, %.2f m, clip %.2f s) heading %.0f deg -> %s\n",
                  move.id, kNames[kind], move.raw ? "RAW" : "DASH", move.path->source.c_str(),
                  move.path->name.c_str(), move.path->Distance() * move.scale, move.path->length, direction / kDeg,
                  move.raw ? "the camera rides the animation" : "comfort dash");
    log += text;
}

std::string Locomotion::Update(SceneInput& input, const BbXrInput& pad, bool suppress) {
    std::string log;
    const double now = input.time;
    std::string config_log;
    if (file.Poll(now, config, config_log)) {
        params = DashParams(config);
        moves.Load(assets, config);
        if (move.on) { // the paths may have changed under the move in progress: end it here
            move.on = false;
            recover = lift;
            recover_t0 = now;
        }
        log += config_log + moves.log + "\n";
    }
    const float dt = have_time ? float(std::clamp(now - last_time, 0.0, 0.1)) : 0.0f;
    have_time = true;
    last_time = now;
    head = input.head;

    const bool live = config.enabled && pad.active && !suppress;
    float mx = live ? (float(pad.lx) - 128.0f) / 127.0f : 0.0f;
    float my = live ? (128.0f - float(pad.ly)) / 127.0f : 0.0f;
    const float tx = live ? (float(pad.rx) - 128.0f) / 127.0f : 0.0f;
    const float mag = std::min(std::sqrt(mx * mx + my * my), 1.0f);
    if (mag <= config.deadzone) {
        mx = my = 0.0f;
    } else {
        const float k = (mag - config.deadzone) / (1.0f - config.deadzone) / std::max(mag, 1e-6f);
        mx *= k;
        my *= k;
    }
    const bool moving = mx != 0.0f || my != 0.0f;
    const uint32_t buttons = live ? pad.buttons : 0u;
    const uint32_t pressed = buttons & ~last_buttons, released = ~buttons & last_buttons;
    last_buttons = buttons;
    // Turning and steering: the game's first-person model (turn_adapter.h, src/vr_turn_math.h).
    // The play space turns about the head; the left stick walks along its move yaw (head, body
    // or the left controller).
    const Vec3 head_world_before = TransformPoint(world, head.position);
    const turn::Frame steer = turn::Step(turn_state, config.turning, yaw, tx, dt, head.rotation, head.valid,
                                         input.aim[0].rotation, input.aim[0].valid);
    if (steer.turn != 0.0f) {
        Turn(steer.turn, head_world_before);
    }
    const float turn_rate = steer.turn_rate;
    const float head_yaw = steer.move_yaw; // where a backstep backs away from
    const Vec3 stick_dir = steer.move_right * mx + steer.move_forward * my;
    const float stick_yaw = std::atan2(-stick_dir.x, -stick_dir.z);

    // A: dodge or backstep when tapped, sprint when held (the game's circle).
    if (pressed & kA) {
        a_down_since = now;
        a_began_moving = moving;
    }
    if ((released & kA) && a_down_since >= 0.0 && now - a_down_since < config.tap_s) {
        if (moving) {
            StartMove(0, stick_yaw, now, log);
        } else {
            StartMove(1, head_yaw, now, log);
        }
    }
    if (!(buttons & kA)) {
        a_down_since = -1.0;
        a_began_moving = false;
    }
    sprinting = (buttons & kA) && a_down_since >= 0.0 && now - a_down_since >= config.tap_s && moving;
    // Left stick click while moving: jump (the bridge passes L3 through at once while moving).
    if ((pressed & BBXR_BTN_L3) && moving) {
        StartMove(2, stick_yaw, now, log);
    }

    // The move: the character follows its clip, sampled at the game's frame rate. Once the root
    // stopped, the stick cancels the rest (the game's recovery frames).
    float tau = 0.0f;
    lift = 0.0f;
    if (move.on) {
        tau = float(now - move.t0);
        const bool cancel = moving && tau > move.path->motion_end + 0.1f;
        const float at = Sampled(std::min(tau, move.path->length));
        const Vec3 off = MoveOffset(move, at);
        ground = move.ground0 + Vec3{off.x, 0.0f, off.z};
        lift = off.y;
        if (tau >= move.path->length || cancel) {
            move.on = false;
            recover = off.y; // whatever height is left blends back to standing
            recover_t0 = now;
            lift = 0.0f;
        }
    }
    if (!move.on && recover != 0.0f) {
        const float k = float(now - recover_t0) / kRecover;
        lift = recover * (1.0f - Smooth(k));
        if (k >= 1.0f) {
            recover = 0.0f;
        }
    }
    // Walking (not while a move plays): toward the stick's speed, reached in accel_s.
    const float top = sprinting ? config.sprint_speed : config.move_speed;
    const Vec3 want_velocity = move.on ? Vec3{} : stick_dir * top;
    const float max_change = top / std::max(config.accel_s, 1e-3f) * dt;
    const Vec3 change = want_velocity - velocity;
    const float change_len = Length(change);
    velocity = change_len > max_change ? velocity + change * (max_change / change_len) : want_velocity;
    ground = ground + velocity * dt;
    character = ground + Vec3{0.0f, lift, 0.0f};

    // The camera: the comfort dash (or, for a raw move, the character itself).
    VrDashHint hint{};
    int dash_move = 0;
    if (move.on && !move.raw) {
        dash_move = move.id;
        const Vec3 end = MoveOffset(move, move.path->length);
        hint.kind = move.kind == 2 ? VR_DASH_JUMP : VR_DASH_DODGE;
        if (config.hint != "none") {
            hint.direction = {end.x, 0.0f, end.z};
        }
        if (config.hint == "exact") {
            hint.distance = std::sqrt(end.x * end.x + end.z * end.z);
        }
        hint.duration = move.kind == 2 ? config.jump_duration : config.dash_duration;
        hint.height = -1.0f;
    }
    const VrDashV3 out = vr_dash_update(&dash, &params, {character.x, character.y, character.z}, dash_move,
                                        dash_move ? &hint : nullptr, now);
    camera = {out.x, out.y, out.z};
    if (dash.snapped) {
        log += "locomotion: the character jumped more than the teleport distance: the camera snapped\n";
    }

    // The world transform; a raw move also turns the view with the clip's head.
    const Mat4 base = Translate(camera) * FromQuat(AxisAngle({0, 1, 0}, yaw));
    world = base;
    tumble = {};
    if (move.on && move.raw && config.raw_rotation > 0.0f) {
        // The head's pitch through the move (unwrapped: a roll turns a full circle), shown as
        // 2 asin(s sin(pitch / 2)): exact for s = 1 up to upside down, level again at the end.
        const float at = Sampled(tau);
        float pitch = 0.0f, prev = 0.0f;
        for (float x = 0.0f;; x += 1.0f / 120.0f) {
            const float p = PitchOf(move.path->Head(std::min(x, at)));
            pitch += WrapPi(p - prev);
            prev = p;
            if (x >= at) {
                break;
            }
        }
        const float shown = 2.0f * std::asin(std::clamp(config.raw_rotation * std::sin(0.5f * pitch), -1.0f, 1.0f));
        const Vec3 lateral = Rotate(AxisAngle({0, 1, 0}, move.yaw), {1, 0, 0});
        tumble = AxisAngle(lateral, shown);
        const Vec3 h = TransformPoint(base, head.position);
        world = Translate(h) * FromQuat(tumble) * Translate(-h) * base;
    }
    // The paths of this move (and the settle after it) for show_paths.
    const bool recent = move.path && now - move.t0 < move.path->length + params.settle + 0.2;
    if ((move.on || recent) && head.valid && path_raw.size() < 900) {
        const Mat4 raw_base = Translate(character) * FromQuat(AxisAngle({0, 1, 0}, yaw));
        path_raw.push_back(TransformPoint(raw_base, head.position));
        path_camera.push_back(TransformPoint(base, head.position));
    }

    // The scene sees the world.
    input.head = ToWorld(input.head);
    for (int side = 0; side < 2; ++side) {
        input.grip[side] = ToWorld(input.grip[side]);
        input.aim[side] = ToWorld(input.aim[side]);
    }
    if (moving || move.on || a_began_moving || sprinting) {
        input.buttons &= ~kA; // the scene's A-hold calibration only for a still A
    }

    // The vignette: the dash's (speed based), and walking or turning if asked.
    float v = (!move.raw || config.vignette_raw) ? config.vignette * dash.vignette : 0.0f;
    if (config.vignette_walk > 0.0f) {
        const float walk = std::clamp(Length(velocity) / 5.0f, 0.0f, 1.0f);
        const float spin = std::clamp(std::fabs(turn_rate) / (120.0f * kDeg), 0.0f, 1.0f);
        v = std::max(v, config.vignette_walk * std::max(walk, spin));
    }
    vignette = v;
    return log;
}

void Locomotion::AppendWorld(FrameData& out, float floor_y) const {
    const auto line = [](std::vector<LineVertex>& to, Vec3 a, Vec3 b, uint8_t r, uint8_t g, uint8_t bl) {
        to.push_back({{a.x, a.y, a.z}, {r, g, bl, 255}});
        to.push_back({{b.x, b.y, b.z}, {r, g, bl, 255}});
    };
    if (config.show_landmarks) {
        // A 60 m floor grid (2 m) beyond the scene's own, and posts every 4 m: something to move
        // past, for the eyes' sense of speed (optic flow) and of straight lines.
        const float y = floor_y + 0.002f;
        for (int k = -15; k <= 15; ++k) {
            const float v = float(k) * 2.0f;
            line(out.lines, {v, y, -30.0f}, {v, y, 30.0f}, 52, 56, 62);
            line(out.lines, {-30.0f, y, v}, {30.0f, y, v}, 52, 56, 62);
        }
        for (int i = -7; i <= 7; ++i) {
            for (int j = -7; j <= 7; ++j) {
                if (std::abs(i) <= 1 && std::abs(j) <= 1) {
                    continue; // keep the middle (the scene's floor, the mirror) clear
                }
                const Vec3 base{float(i) * 4.0f, floor_y, float(j) * 4.0f};
                const float h = 1.2f + 0.8f * float((i * 7 + j * 3) & 3) * 0.33f;
                const uint8_t shade = uint8_t(110 + 30 * ((i + j) & 1));
                const float s = 0.08f;
                const Vec3 corner[4] = {{-s, 0, -s}, {s, 0, -s}, {s, 0, s}, {-s, 0, s}};
                for (int c = 0; c < 4; ++c) {
                    const Vec3 a = base + corner[c], b = base + corner[(c + 1) % 4];
                    line(out.lines, a, a + Vec3{0, h, 0}, shade, shade, uint8_t(shade + 20));
                    line(out.lines, a + Vec3{0, h, 0}, b + Vec3{0, h, 0}, 200, 170, 90);
                }
            }
        }
    }
    if (config.show_paths && path_raw.size() > 1) {
        // The last move: the animation's eye path (orange) and the camera's (green), at their
        // heights and as shadows on the floor.
        for (size_t i = 1; i < path_raw.size(); ++i) {
            line(out.lines, path_raw[i - 1], path_raw[i], 255, 140, 40);
            line(out.lines, path_camera[i - 1], path_camera[i], 60, 230, 110);
            const auto floor = [&](Vec3 p) { return Vec3{p.x, floor_y + 0.005f, p.z}; };
            line(out.lines, floor(path_raw[i - 1]), floor(path_raw[i]), 150, 90, 40);
            line(out.lines, floor(path_camera[i - 1]), floor(path_camera[i]), 40, 140, 70);
        }
    }
}

} // namespace sb
