// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Locomotion in sandbox3d, and the comfort dash test bed (docs/vr/findings/19-dash-locomotion.md):
//   - left stick: smooth locomotion relative to where the head looks; A held while moving: sprint;
//   - right stick X: snap turn (or smooth turn), about the head;
//   - A tapped while moving: dodge (a roll in the stick's direction); A tapped standing: backstep;
//     left stick click while moving: jump. The game's mapping (xr_input.cpp: A = circle, L3 as
//     the bridge passes it through while the left stick moves).
// Each move plays like the game: a "character" follows the animation's eye path (recorded from the
// user's dump by scripts/vr_assets/dodge_extract.py, out/vr_assets/dash/dodges.json, or synthetic:
// root motion, the head surging ahead, dipping, swaying, a jump's arc), sampled at the game's frame
// rate. The camera then either rides that path ("raw": the tumble and the bob) or the comfort dash
// (vr_dash.h, the code the game uses): a straight ease-out slide, no bob, a gentle jump bump, an
// optional vignette. "ab" alternates the two on every move.
// Everything is live-tunable in the "locomotion" object of sandbox3d.json (polled here, apart from
// config.h; written with the defaults when missing).
// The player's place in the world is a transform of the tracking space (the headset, the hands):
// world = WorldFromTracking() * tracking. The scene works in the world: the hunter follows.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "bbport_xr_bridge.h"
#include "math3d.h"
#include "renderer.h"
#include "scene.h"
#include "turn_adapter.h"
#include "vr_dash.h"

namespace sb {

struct LocoConfig {
    bool enabled = true;
    std::string mode = "dash"; // dash | raw (the animation's camera path) | ab (alternate)
    // Walking.
    float move_speed = 3.0f, sprint_speed = 5.5f; // m/s at full deflection
    float deadzone = 0.15f;
    float accel_s = 0.08f;     // to full speed and back to rest
    // Turning and steering: the game's first-person model (src/vr_turn_math.h through
    // turn_adapter.h), set by its own keys in this object: turn (snap | smooth | off), snap (deg),
    // snap_repeat (s), turn_speed (deg/s), deadzone, expo, turn_accel (s), snap_on, snap_off,
    // move (head | body | controller), recenter, lock, lock_auto. Default here: move = head.
    VrTurnConfig turning = DefaultTurning();
    static VrTurnConfig DefaultTurning() {
        VrTurnConfig c;
        vrt_config_default(&c);
        c.move = VRT_MOVE_HEAD;
        return c;
    }
    // The comfort dash (vr_dash.h).
    float dash_duration = 0.4f;      // s: the slide of a dodge or backstep
    std::string ease = "quad";       // linear | quad | cubic | quart | quint | expo | smooth (in-out)
    float ease_power = 0.0f;         // > 0 overrides the curve's exponent
    float settle_s = 0.2f;           // the camera joins the character after the move
    float settle_speed = 4.0f;       // m/s: a bigger gap (cancelled, blocked) takes gap / this
    float max_lead_m = 0.0f;         // never further ahead of the character (0: no limit)
    std::string hint = "exact";      // what the dash knows: exact (end), direction, none
    float dodge_distance = 0.0f;     // m; 0: the animation's own (the roll: 4.0)
    float backstep_distance = 0.0f;  // (2.48)
    float jump_distance = 0.0f;      // (6.84: the leap and the landing roll)
    float jump_height = 0.3f;        // m: the dash's one smooth bump (0: flat)
    float jump_duration = 0.9f;      // s: the jump's slide and bump (the leap is ~0.85 s airborne)
    bool jump_flat = false;          // a flat dash for jumps, whatever jump_height says
    // Comfort vignette: darkens the view beyond inner_deg from the head's forward axis, fully at
    // outer_deg; strength 0..1 (0 = off). During the dash only, unless vignette_walk > 0.
    float vignette = 0.6f, vignette_inner_deg = 35.0f, vignette_outer_deg = 70.0f;
    float vignette_walk = 0.0f;      // strength while walking or smooth-turning (findings/11 4.3)
    bool vignette_raw = false;       // also in raw mode
    // The moves ("the game").
    std::string raw_source = "auto"; // auto (recorded if dodges.json has them) | synthetic
    // The recorded animations (dodges.json; "" = by its kind_guess): the standing forward roll,
    // the backstep, the running jump (a leap and a landing roll).
    std::string roll_anim = "a000_007200", backstep_anim = "a000_007000", jump_anim = "a000_007800";
    float raw_rotation = 0.5f;       // raw mode: how much of the head's own pitch the view takes
                                     // (shown 2 asin(s sin(p/2)): 1 = all of it, upside down in a
                                     // roll; 0.5 = up to 60 degrees; 0 = position only)
    float game_fps = 30.0f;          // the animation's sample rate (0: every frame)
    float tap_s = 0.3f;              // A released before this: dodge or backstep; held: sprint
    bool show_paths = true, show_landmarks = true;

    void Load(const json::Value& v);
    std::string Summary() const; // as JSON (the "locomotion" object)
};

/// A move's eye path in its own frame (start at the origin, forward -Z, +X right, +Y up), relative
/// to the standing eye it starts from (the game blends from standing into the animation's first
/// frame, which already leans into the move).
struct MovePath {
    std::string name, source; // the animation, "recorded" or "synthetic"
    float length = 0.7f;      // s: the whole clip (the dash filter's "active")
    float motion_end = 0.5f;  // s: the root stops moving (the stick may cancel the rest)
    std::vector<float> t;
    std::vector<m3::Vec3> eye;  // relative to the standing eye at the start
    std::vector<m3::Quat> head; // the head's rotation relative to standing (world frame of the move)
    m3::Vec3 Eye(float tau) const;
    m3::Quat Head(float tau) const;
    m3::Vec3 End() const { return eye.empty() ? m3::Vec3{} : eye.back(); }
    float Distance() const;   // horizontal, m
};

/// The moves the sandbox plays: recorded from dodges.json when it has them, else synthetic.
struct MoveSet {
    MovePath roll, backstep, jump;
    std::string log;
    void Load(const std::filesystem::path& assets, const LocoConfig& config);
};

/// The synthetic moves (also the previews' and the tests' stand-ins for the game).
MovePath SyntheticRoll(float distance, float length);
MovePath SyntheticBackstep(float distance, float length);
MovePath SyntheticJump(float distance, float length);

/// Polls the "locomotion" object of sandbox3d.json (twice a second); adds it with the defaults
/// when the file has none.
class LocoConfigFile {
public:
    explicit LocoConfigFile(std::filesystem::path path) : path(std::move(path)) {}
    bool Poll(double now, LocoConfig& config, std::string& log);

private:
    std::filesystem::path path;
    std::filesystem::file_time_type stamp{};
    double next = 0.0;
    bool loaded = false, inserted = false;
};

/// The vr_dash.h parameters for a config.
VrDashParams DashParams(const LocoConfig& c);

class Locomotion {
public:
    explicit Locomotion(const std::filesystem::path& config_path);

    /// One frame: reads the sticks and buttons of `pad`, moves the player, then turns `input`
    /// (head, hands; tracking space) into the world and masks A from the scene while it moves or
    /// dodges (the scene's A-hold calibration then only sees a still A). `suppress`: a menu has
    /// the controls (no moving, turning or moves). Returns lines to print.
    std::string Update(SceneInput& input, const BbXrInput& pad, bool suppress = false);
    /// The tracking space's pose in the world (apply to everything the runtime tracks).
    const m3::Mat4& WorldFromTracking() const { return world; }
    TrackedPose ToWorld(const TrackedPose& p) const;
    /// The vignette's strength now (0..1), already scaled by the config.
    float Vignette() const { return vignette; }
    const LocoConfig& Config() const { return config; }
    /// Asked before a dodge (kind 0), backstep (1) or jump (2) starts; false refuses it (nothing
    /// moves). The swing and stamina logic (combat.h) pays the move's stamina here and refuses
    /// it at 0 or below, as the game does. Unset: every move starts.
    std::function<bool(int kind, double now)> move_gate;
    /// Landmarks (so motion shows beyond the scene's floor) and the last move's two paths.
    void AppendWorld(FrameData& out, float floor_y) const;

private:
    struct Move {
        bool on = false, raw = false;
        int id = 0, kind = 0; // 0 roll, 1 backstep, 2 jump
        double t0 = 0.0;
        m3::Vec3 ground0;     // the character's ground position (standing eye height) at the start
        float yaw = 0.0f;     // the move's direction (radians about +Y, 0 = -Z)
        float scale = 1.0f;   // horizontal distance scale (configured distance / the path's)
        const MovePath* path = nullptr;
    };

    void Turn(float radians, m3::Vec3 pivot);
    void StartMove(int kind, float yaw, double now, std::string& log);
    m3::Vec3 MoveOffset(const Move& m, float tau) const;
    float Sampled(float tau) const; // the game's frame-rate sampling of a move's time

    std::filesystem::path assets;
    LocoConfigFile file;
    LocoConfig config;
    VrDashParams params{};
    MoveSet moves;
    VrDash dash{};
    Move move;
    int move_count = 0;
    bool have_time = false;
    double last_time = 0.0;
    m3::Vec3 ground;         // where the character stands (walking, a move's root motion)
    float lift = 0.0f;       // the eye's height change from the animation (+ recovering after it)
    float recover = 0.0f;    // after a move: the height still to recover
    double recover_t0 = 0.0;
    m3::Vec3 character;      // raw: ground + lift, where the "game" puts the tracking origin
    m3::Vec3 camera;         // what the view uses (dash filter or raw)
    float yaw = 0.0f;        // the tracking space's turn in the world
    m3::Vec3 velocity;       // walking (m/s, world)
    m3::Quat tumble;         // raw mode: the head's rotation from the animation (world)
    m3::Mat4 world;
    TrackedPose head;        // tracking space, this frame
    uint32_t last_buttons = 0;
    double a_down_since = -1.0;
    bool a_began_moving = false, sprinting = false;
    VrTurnState turn_state{}; // the shared turning model's (turn_adapter.h); psi = -yaw
    float vignette = 0.0f;
    // The last move's paths (world, the head) for show_paths.
    std::vector<m3::Vec3> path_raw, path_camera;
};

} // namespace sb
