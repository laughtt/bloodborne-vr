// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The throwables' data (throwables.h): the 15 goods with the game's Bullet values and how they
// look and sit in the hand (ThrowItemDef), and the live-tunable settings (ThrowConfig, the
// "throwables" object of out/vr_assets/sandbox3d.json, hot reloaded; missing keys keep these
// defaults).

#pragma once

#include <string>
#include <vector>

#include "json.h"
#include "math3d.h"

namespace sb {

struct ThrowOffset {
    m3::Vec3 position;     // meters
    m3::Vec3 rotation_deg; // Euler degrees, X then Y then Z
};

/// One throwable: the game's Bullet row (EquipParamGoods.refId -> Bullet) plus how it looks and
/// how it sits in the hand.
struct ThrowItemDef {
    int goods = 0;
    std::string name;
    // Bullet (param/gameparam). dist: metres of travel with the "in range" gravity/acceleration.
    float initial_velocity = 0.0f, gravity_in = 0.0f, gravity_out = 0.0f, accel_in = 0.0f;
    float max_velocity = 0.0f, life_s = 0.0f, range_m = 0.0f;
    int num_shoot = 1;
    float shoot_angle_deg = 0.0f; // fan: num_shoot bullets spread this many degrees apart
    bool penetrate = false;       // passes through the enemy (stops at the world)
    bool back_throw = false;      // no flight: dropped at the hand (rope molotovs)
    float fuse_s = 0.0f;          // delayed items burst this long after their first impact
    int hit_bullet = 0;           // HitBulletID (the burst), informational
    int sfx_flight = -1;          // Bullet.sfxId_Bullet (informational)
    // Look and feel (not from the game).
    std::string effect = "dust";  // fire poison oil mist blood coin lightning slash eye dust
    float color[3] = {0.8f, 0.8f, 0.8f};
    std::string mesh;             // throwables/<mesh>.json under the assets; "" = procedural
    std::string shape = "sphere"; // procedural stand-in: bottle urn knife sphere disc vial stone
    float scale = 1.0f;           // mesh scale (the effect meshes are drawn smaller in game)
    float radius_m = 0.03f;       // collision sphere of the flying item
    m3::Vec3 align{0, 0, 0};      // model axis that follows the velocity (knives); 0: tumbles
    float spin_dps = 540.0f;      // tumble rate
    ThrowOffset grip;                  // item centre relative to the controller grip pose
    bool bullet_overrides = false; // true when a throwables.json row replaced the bullet fields
};

/// The 15 built-in rows (the dump's values; see throwables.cpp for the two that are adjusted).
std::vector<ThrowItemDef> DefaultThrowItems();

struct ThrowConfig {
    bool enabled = true;
    std::string mode = "hand";        // "hand": the hand's velocity throws; "game": trigger = the
                                      // game's fixed throw along the view
    float grab_radius_m = 0.16f;      // item centre to the hand (grip + grab_point)
    m3::Vec3 grab_point{0.0f, 0.0f, -0.05f};
    float grab_squeeze = 0.60f, release_squeeze = 0.35f;
    // Hand velocity: samples of the last window, blended mean/peak.
    float smooth_ms = 80.0f;
    float peak_weight = 0.5f;         // 0: mean velocity over the window, 1: the fastest step
    float release_lag_ms = 0.0f;      // the window ends this long before the release
    // Hand speed -> bullet speed: initial_velocity x clamp(speed / ref, min_scale, max_scale),
    // then capped by the bullet's max velocity.
    float ref_speed_mps = 7.0f;
    float min_scale = 0.30f, max_scale = 1.50f;
    float min_throw_speed_mps = 1.2f; // slower: the item is just dropped with the hand's velocity
    float game_pitch_deg = 5.0f;      // mode "game": the arc leaves this far above the view
    float game_trigger = 0.7f;
    float respawn_s = 3.0f;           // a spent item is back on the rack after this
    float rest_return_s = 25.0f;      // a dropped/landed item returns after this
    float effect_scale = 1.0f;
    float grav = 9.8f;                // drops
    // The rack: [x, z] of the table's centre relative to where you stood/looked (x right, -z
    // ahead), the top's height above the floor, rows x columns of items.
    float rack_pos[2] = {0.0f, -0.62f};
    float rack_height_m = 0.88f;
    float rack_spacing_m = 0.24f;
    int rack_columns = 5;
    std::string respawn_button = "dpad_down"; // all items back on the rack
    std::string mode_button = "dpad_up";      // toggles hand/game
    bool show = true, show_markers = true;
    bool rack_grab = true;            // false: the rack is hidden and its items cannot be grabbed
                                      // (the consumables wheel hands them out, consumables.h)
    bool log = true;
    std::string log_file = "sandbox3d_throws.jsonl";
    // Per-item overrides: {"1200": {"scale": 0.6, "grip": {...}, "fuse_s": 2, ...}} (goods id or
    // name); applied over the built-ins and the extracted table.
    json::Value items;

    void Load(const json::Value& doc);
    std::string Summary(const std::string& indent = "  ") const;
};

/// Applies a config object's per-item overrides ({"initial_velocity": .., "grip": ..}).
void ApplyItemOverride(ThrowItemDef& def, const json::Value& o);
/// Replaces the bullet fields of the defaults with an extracted out/vr_assets/throwables/
/// throwables.json (scripts/vr_assets/throwables_extract.py); returns how many rows matched.
int ApplyExtractedTable(std::vector<ThrowItemDef>& defs, const json::Value& table);

uint32_t ThrowButton(const std::string& name);
/// A text form of a JSON value (to notice a changed "items" object after a config reload).
std::string ThrowItemsSignature(const json::Value& v);

} // namespace sb
