// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The consumables' data (consumables.h, docs/vr/findings/36-vr-consumables.md): every item of the
// wheel with its group, gesture, look and effect (ConsumableDef), the sample inventory, and the
// live-tunable settings (ConsumeConfig, the "consumables" object of out/vr_assets/sandbox3d.json,
// hot reloaded; missing keys keep these defaults).

#pragma once

#include <string>
#include <vector>

#include "json.h"
#include "math3d.h"
#include "throw_config.h"

namespace sb {

/// What the item is (the wheel's order and the effect applied).
enum class CGroup { Throwable, WeaponBuff, Heal, Cure, BodyBuff, Tool };
/// How it is used: thrown (Throwables), rubbed along the weapon, brought to the head, crushed.
enum class CGesture { Throw, Rub, Head, Crush };

const char* GroupName(CGroup g);
const char* GestureName(CGesture g);

struct ConsumableDef {
    int goods = 0;
    std::string name;
    CGroup group = CGroup::Heal;
    CGesture gesture = CGesture::Head;
    bool consumed = true;      // false: reusable (bells, the quicksilver tools), count shown as inf
    int bullet_cost = 0;       // quicksilver bullets (goods 900) one use takes
    int sample_count = 0;      // the sample inventory
    // Look: consumables/<mesh>.json under the assets when it exists, else the procedural shape.
    std::string mesh;
    std::string shape = "vial";
    float scale = 1.0f;        // mesh scale (extracted meshes are in metres already)
    float color[3] = {0.8f, 0.8f, 0.8f};
    m3::Vec3 hold{0.0f, 0.0f, -0.05f};  // item centre relative to the grip pose
    m3::Vec3 hold_rot_deg{0.0f, 0.0f, 0.0f};
    // Effect.
    float heal_pct = 0.0f;     // % of max HP (items.json: transfusion HP)
    std::string cure;          // ailment removed ("poison", "frenzy")
    std::string element;       // weapon buffs: fire bolt arcane gun
    int target_side = 1;       // weapon buffs: the weapon that is buffed (1 right, 0 left)
    float duration_s = 0.0f;   // buff / status length (items.json, else the config default)
    std::string status;        // the status line shown when used
    int insight = 0;           // Madman's Knowledge
    // Throwables: the Throwables definition this entry mirrors.
    int throw_def = -1;
};

/// The non-throwable items (the throwables are appended from Throwables::Defs()).
std::vector<ConsumableDef> DefaultConsumables();
/// The throwable entry of a Throwables definition (goods, name, look; counts from the sample).
ConsumableDef ThrowableEntry(const ThrowItemDef& def, int def_index);
/// The sample inventory (goods -> count) used when the config has no "inventory" object.
std::vector<std::pair<int, int>> SampleInventory();
/// Overrides names, durations, heal %, consumed flags and costs from out/vr_assets/items/items.json
/// (the dump's goods + SpEffect rows, scripts/vr_assets); returns how many rows matched.
int ApplyItemsTable(std::vector<ConsumableDef>& defs, const json::Value& items);

struct ConsumeConfig {
    bool enabled = true;
    std::string wheel = "objects";    // "objects": 3D items in front of you; "cards": the old UI wheel
    std::string open_button = "r3";   // opens the wheel
    std::string wheel_mode = "toggle"; // "toggle": it stays open until an item is taken or the button
                                       // is pressed again; "hold": open while held (+ linger_s)
    bool close_on_grab = true;        // toggle mode: taking an item closes the wheel
    float linger_s = 0.8f;            // hold mode: the wheel stays this long after the release
    // Placement (relative to the head's yaw, taken when the wheel opens).
    std::string layout = "arc";       // "arc" (items on a circle around you) or "shelf" (flat rows)
    float distance_m = 0.45f;         // from the head
    float down_deg = 30.0f;           // the first row's angle below eye level
    float spacing_m = 0.10f;          // between neighbours in a row
    float row_spacing_m = 0.11f;      // rows go down
    int per_row = 10;
    float item_scale = 1.0f;
    float bob_m = 0.004f;             // idle float amplitude
    bool labels = true;               // the small count under each item
    // Grab (the hand's grip + grab_point near an item, squeeze).
    float grab_radius_m = 0.065f;
    m3::Vec3 grab_point{0.0f, 0.0f, -0.05f};
    float grab_squeeze = 0.60f, release_squeeze = 0.35f;
    // Head gesture: within mouth_radius_m of the mouth point (head local), below the eyes, held.
    m3::Vec3 mouth{0.0f, -0.10f, -0.07f};
    float mouth_radius_m = 0.12f;
    float below_eyes_m = 0.03f;
    float head_hold_s = 0.3f;
    // Rub: the paper within contact_radius_m of the blade, covering `coverage` of the blade within
    // window_s; blade_start is the share of the weapon's length (from the grip) that is handle.
    float contact_radius_m = 0.10f;
    float coverage = 0.50f;
    float window_s = 2.5f;
    float blade_start = 0.25f;
    float contact_gap_s = 0.20f;      // a break in contact longer than this restarts the rub
    float buff_default_s = 60.0f;
    // Crush: squeeze at least crush_squeeze for crush_hold_s.
    float crush_squeeze = 0.97f;
    float crush_hold_s = 0.5f;
    // The throwables live on the table / rack only; the wheel holds consumables (heals, cures, body
    // buffs, tools, weapon buffs). throwables_on_wheel=true brings the throwables back onto it (and
    // hide_rack=true then hides the rack).
    bool throwables_on_wheel = false;
    bool hide_rack = false;
    // Weapon buffs: false = picking a paper / ash from the wheel buffs the weapon of the other hand
    // (else the right one) at once; true = the paper is held and rubbed along the blade.
    bool buff_rub = false;
    bool stow_weapons = true;         // weapons are put away while the wheel is open / a hand holds an item
    float hp_start_pct = 55.0f;
    bool hud = true;
    float diag_interval_s = 0.5f;     // rub / mouth diagnostics in the log (0: off)
    bool log = true;
    std::string log_file = "sandbox3d_consumables.jsonl";
    // Counts: {"1000": 20, ...} (goods id -> count); empty: the sample inventory.
    json::Value inventory;

    void Load(const json::Value& doc);
    std::string Summary(const std::string& indent = "  ") const;
    /// The objects wheel is in charge of the open button (and the R3 skeleton toggle).
    bool Objects() const { return enabled && wheel == "objects"; }
};

} // namespace sb
