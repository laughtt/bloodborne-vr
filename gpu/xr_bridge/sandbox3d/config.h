// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// sandbox3d's live-tunable parameters: out/vr_assets/sandbox3d.json, polled twice a second and
// applied without a restart (written with every default the first time). Missing keys keep their
// defaults, so the file only needs what is being tuned. The T-pose calibration (hold A) goes to
// sandbox3d_calibration.json next to it and overrides eye_height_m / arm_scale.

#pragma once

#include <filesystem>
#include <string>

#include "json.h"
#include "math3d.h"
#include "physics_config.h"
#include "consumables_config.h"
#include "throw_config.h"

namespace sb {

struct Offset {
    m3::Vec3 position;     // meters
    m3::Vec3 rotation_deg; // Euler degrees, X then Y then Z
};

struct Config {
    // Rendering.
    float render_scale = 0.6f; // of the runtime's recommended eye image size
    float near_z = 0.03f, far_z = 200.0f;
    m3::Vec3 background{0.03f, 0.03f, 0.04f};
    bool want_depth = true; // hand the depth to the runtime when it can take it

    // Body placement and size.
    float eye_height_m = 1.65f; // the user's eye height above the floor (calibration)
    float arm_scale = 1.0f;     // arm length relative to the scaled model (calibration)
    float floor_y = 0.0f;       // in LOCAL space (no STAGE): the floor relative to the origin
    bool follow_head_height = true; // crouching lowers the body (else feet stay on the floor)
    m3::Vec3 eye_offset{0.0f, 0.10f, -0.09f}; // eyes relative to the Head joint, model space
    float body_yaw_deadzone_deg = 30.0f;      // head turns this far before the body follows
    float body_back_m = 0.0f;                 // body pushed back from the eyes (comfort)

    // Arms.
    m3::Vec3 elbow_pole{0.4f, -0.85f, 0.35f}; // right elbow direction, body space (+X right =
                                              // out, +Y up, +Z back; 13 �5.3); mirrored left
    float forearm_twist = 1.0f; // share of the wrist roll ForeArmTwist takes (ForeArmTwist1: half)
    Offset hand_offset{{0.0f, -0.01f, 0.07f}, {-15.0f, 0.0f, 0.0f}}; // grip -> hand, right hand
                                                                     // (mirrored for the left)
    // Fingers (trigger: index, grip: middle/ring/little, thumb: max of both). The angle is the
    // middle joint's at full curl; the base and tip joints get 0.74 and 0.63 of it (13 �6.2:
    // MCP 70, PIP 95, DIP 60), the thumb 0.6 / 1.0 / 1.25 (CMC 25, MCP 40, IP 50).
    float trigger_curl_deg = 95.0f, grip_curl_deg = 95.0f, thumb_curl_deg = 40.0f;
    float rest_curl = 0.15f; // curl with nothing pressed (relaxed hand)

    // Weapons.
    std::string weapon = "Saw Cleaver";  // right hand: name, model (wp_a_0800) or "" for none
    std::string left_weapon = "Hunter Pistol";
    Offset weapon_grip;      // the weapon relative to the R_Weapon bone
    Offset left_weapon_grip; // ... L_Weapon
    std::string form = "A";  // trick weapon form whose attacks are shown (A one-handed, B)
    int attack = 0;          // that form's attack whose hit shapes are shown (Y cycles); -1:
                             // the weapon's own shapes of all of them (no body ones)
    float placeholder_radius = 0.06f;
    bool weapon_align = true; // turn the weapon's long axis into the grip's up/forward plane
                              // (minimal turn; the raw R_Weapon attachment leans sideways)

    // What to draw.
    bool show_body = true, show_head = false, show_masked = false;
    // Your own body in first person, as the game hides it (BB_VR_FP hide=, findings/16 4.2):
    // "head" the head meshes, "body" all but the arms (and the weapons), "all" the whole body and
    // the held weapons, "none" nothing; "" keeps show.head's choice. The mirror copy shows all.
    std::string fp_hide;
    bool show_skeleton = true, show_dummies = false, show_weapon = true, show_hitboxes = true;
    bool show_grid = true, show_controllers = true, show_laser = true, show_mirror = true;
    bool skeleton_on_top = true;
    bool show_hurtboxes = true;       // the player's hurtboxes (chr_c0000.json) on the mirror copy
    bool show_hurtboxes_self = false; // ... on your own body too
    float mirror_distance_m = 2.0f;

    PhysicsConfig physics; // solid weapons, props, haptics: the "physics" object (physics_config.h)
    ThrowConfig throwables; // hand-thrown goods: the rack, grab, throw (throw_config.h)
    ConsumeConfig consumables; // the item wheel of 3D objects and its gestures (consumables_config.h)

    void Load(const json::Value& doc);
    std::string Summary() const;
};

/// Watches the config file and the calibration file next to it.
class ConfigFile {
public:
    explicit ConfigFile(std::filesystem::path path);
    /// Reloads when the file changed (polls at most every 0.5 s). True when `config` changed.
    bool Poll(Config& config, std::string& log);
    /// Writes the calibration overrides (eye height, arm scale).
    void SaveCalibration(float eye_height_m, float arm_scale, std::string& log);
    const std::filesystem::path& Path() const { return path; }

private:
    std::filesystem::path path, calibration;
    std::filesystem::file_time_type stamp{}, calibration_stamp{};
    double next_poll = 0.0;
    bool loaded = false;
};

/// Writes the default config (every key) to `path`.
bool WriteDefaultConfig(const std::filesystem::path& path);

} // namespace sb
