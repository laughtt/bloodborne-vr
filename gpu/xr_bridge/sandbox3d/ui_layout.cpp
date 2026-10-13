// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "ui_layout.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

#include "model.h"

namespace sb {
namespace {

// The default layout: what ui_layouts.json starts as. Presets follow doc 18 (Dungeons of Eternity
// read-across): A body slots first, B the wrist strip, C the radial (+ weapon wheel), then D the
// classic grid inventory.
const char* const kDefaultLayout = R"JSON({
  "_doc": "VR menu prototypes for sandbox3d, hot-reloaded when saved. See gpu/xr_bridge/sandbox3d/README.md. Colors: sRGB + alpha (0..1). Lengths in meters, angles in degrees. Bindings: tokens joined by '+'; the last token is the key that acts, the others must already be held. Tokens: left_grip right_grip left_trigger right_trigger a b x y l3 r3 dpad_up dpad_down dpad_left dpad_right options touchpad none. dpad_* come from the bridge's left-stick-click layer: hold the left stick click and flick the right stick.",
  "version": 1,
  "active_preset": "body_slots",
  "cycle_next": "dpad_right",
  "cycle_prev": "dpad_left",
  "help": "dpad_up",
  "reset": "dpad_down",
  "log": true,
  "infinite": false,
  "style": {
    "panel": [0.06, 0.06, 0.07, 0.90],
    "panel_border": [0.55, 0.51, 0.42, 0.95],
    "slot": [0.17, 0.17, 0.19, 0.92],
    "slot_border": [0.34, 0.33, 0.31, 0.95],
    "highlight": [0.26, 0.28, 0.33, 0.97],
    "highlight_border": [0.84, 0.82, 0.69, 1.0],
    "text": [0.93, 0.90, 0.81, 1.0],
    "text_dim": [0.64, 0.62, 0.57, 1.0],
    "accent": [0.78, 0.16, 0.13, 1.0],
    "laser": [0.55, 0.85, 1.0, 1.0],
    "open_seconds": 0.12,
    "close_seconds": 0.08,
    "text_outline": 0.10,
    "haptic_open": [0.25, 0.02],
    "haptic_hover": [0.15, 0.012],
    "haptic_select": [0.6, 0.05],
    "haptic_grab": [0.8, 0.06],
    "haptic_empty": [0.35, 0.12],
    "haptic_preset": [0.4, 0.04]
  },
  "inventory": [
    {"item": "Blood Vial", "count": 20},
    {"item": "Quicksilver Bullets", "count": 20},
    {"item": "Molotov Cocktail", "count": 10},
    {"item": "Pebble", "count": 20},
    {"item": "Throwing Knife", "count": 20},
    {"item": "Fire Paper", "count": 10},
    {"item": "Bolt Paper", "count": 10},
    {"item": "Antidote", "count": 5},
    {"item": "Sedative", "count": 5},
    {"item": "Beast Blood Pellet", "count": 3},
    {"item": "Blue Elixir", "count": 5},
    {"item": "Oil Urn", "count": 10},
    {"item": "Poison Knife", "count": 20},
    {"item": "Rope Molotov Cocktail", "count": 10},
    {"item": "Pungent Blood Cocktail", "count": 10},
    {"item": "Numbing Mist", "count": 5},
    {"item": "Bone Marrow Ash", "count": 5},
    {"item": "Lead Elixir", "count": 3},
    {"item": "Madman's Knowledge", "count": 3},
    {"item": "Coldblood Dew (1)", "count": 5},
    {"item": "Bold Hunter's Mark", "count": 2},
    {"item": "Hunter's Mark", "count": 1},
    {"item": "Shining Coins", "count": 20},
    {"item": "Monocular", "count": 1},
    {"item": "Notebook", "count": 1},
    {"item": "Beckoning Bell", "count": 1},
    {"item": "Delayed Molotov", "count": 10}
  ],
  "presets": [
    {"name": "body_slots", "label": "A  Body slots",
     "notes": "Reach and squeeze the grip: right hip = Saw Cleaver, over the right shoulder = Hunter Axe, chest = Blood Vial, left hip = Molotov, left hip front = Pebble, behind the left hip = Blunderbuss. Turn the left palm up for the counts.",
     "menus": ["body_slots", "wrist_counters"]},
    {"name": "wrist_strip", "label": "B  Wrist strip",
     "notes": "Turn the left palm up: vials, bullets, Molotov on the forearm. Touch one with the right index finger, or point the right hand and pull the trigger.",
     "menus": ["wrist_strip"]},
    {"name": "radial", "label": "C  Radial ring + weapon wheel",
     "notes": "Hold right grip + B: quick items around the right hand. Pick by right stick, by moving the hand, or by pointing the left hand; release B to use. Hold left grip + X: weapon wheel (left stick, left hand or right pointer).",
     "menus": ["quick_radial", "weapon_wheel"]},
    {"name": "grid", "label": "D  Grid inventory",
     "notes": "Right grip + A opens or closes the inventory within reach. Point the right hand and pull the trigger, or touch a card. Tabs filter by use; right stick left/right turns pages, up/down changes the tab. Left grip + X: weapon wheel.",
     "menus": ["inventory_grid", "wrist_counters", "weapon_wheel"]},
    {"name": "grid_far", "label": "E  Far grid (doc 17)",
     "notes": "Doc 17's fuller inventory: right grip + A opens a large panel 1.5 m ahead that catches up when you turn away. Laser only: point and pull the right trigger. Tabs filter by use; right stick up/down changes the tab, left/right the page.",
     "menus": ["inventory_far", "wrist_counters", "weapon_wheel"]}
  ],
  "menus": [
    {"name": "quick_radial", "type": "radial", "label": "Quick items", "hand": "right",
     "open": "right_grip+b", "mode": "hold", "action": "use",
     "items": ["Blood Vial", "Quicksilver Bullets", "Molotov Cocktail", "Pebble", "Throwing Knife",
               "Fire Paper", "Bolt Paper", "Antidote"],
     "select": ["stick", "move", "point"], "pointer": "other", "click": "left_trigger",
     "radius": 0.13, "inner_radius": 0.05, "item_size": 0.048, "gap": 0.004,
     "forward": 0.07, "up": 0.04, "deadzone": 0.5, "move_threshold": 0.035,
     "use_on_release": true, "on_top": true},
    {"name": "weapon_wheel", "type": "radial", "label": "Weapons", "hand": "left",
     "open": "left_grip+x", "mode": "hold", "action": "equip",
     "items": ["Saw Cleaver", "Hunter Axe", "Threaded Cane", "Kirkhammer", "Saw Spear",
               "Hunter Pistol", "Hunter Blunderbuss", "Hunter's Torch"],
     "select": ["stick", "move", "point"], "pointer": "other", "click": "right_trigger",
     "radius": 0.15, "inner_radius": 0.055, "item_size": 0.062, "gap": 0.004,
     "forward": 0.07, "up": 0.04, "deadzone": 0.5, "move_threshold": 0.035,
     "use_on_release": true, "on_top": true},
    {"name": "inventory_grid", "type": "grid", "label": "Inventory",
     "open": "right_grip+a", "mode": "toggle", "action": "use",
     "select": ["point", "touch"], "pointer": "right", "click": "right_trigger",
     "columns": 5, "rows": 3, "cell": 0.08, "distance": 0.6, "below": 0.15, "tilt": 20,
     "anchor": "world", "tabs": ["all", "heal", "throw", "buff", "tool", "ammo"],
     "page_stick": "right", "on_top": false, "hide_weapons": true},
    {"name": "inventory_far", "type": "grid", "label": "Inventory",
     "open": "right_grip+a", "mode": "toggle", "action": "use",
     "select": ["point"], "pointer": "right", "click": "right_trigger",
     "columns": 6, "rows": 3, "cell": 0.15, "distance": 1.5, "below": 0.2, "tilt": 8,
     "anchor": "lazy", "tabs": ["all", "heal", "throw", "buff", "tool", "ammo"],
     "page_stick": "right", "on_top": false, "hide_weapons": true},
    {"name": "wrist_counters", "type": "wrist", "label": "", "hand": "left", "show": "glance",
     "items": ["Blood Vial", "Quicksilver Bullets"], "select": [],
     "glance_deg": 50, "view_deg": 40, "offset": [0.035, 0.0, 0.09], "normal": "+x",
     "width": 0.10, "height": 0.05, "on_top": true},
    {"name": "wrist_strip", "type": "wrist", "label": "Quick", "hand": "left", "show": "glance",
     "items": ["Blood Vial", "Quicksilver Bullets", "Molotov Cocktail"], "action": "use",
     "select": ["touch", "point"], "pointer": "right", "click": "right_trigger",
     "glance_deg": 55, "view_deg": 45, "offset": [0.04, 0.0, 0.10], "normal": "+x",
     "width": 0.15, "height": 0.06, "on_top": true},
    {"name": "body_slots", "type": "body_slots", "posture": "auto", "seated_drop": 0.35,
     "markers": "always", "grab_radius": 0.10, "marker_size": 0.045, "neck": [0.0, -0.10, 0.08],
     "on_top": true,
     "slots": [
       {"name": "right_hip", "item": "Saw Cleaver", "action": "equip", "hands": "right",
        "standing": [0.17, -0.62, 0.03], "seated": [0.21, -0.47, -0.17]},
       {"name": "back", "item": "Hunter Axe", "action": "equip", "hands": "right",
        "standing": [0.15, 0.06, 0.17], "seated": [0.15, 0.06, 0.14]},
       {"name": "chest", "item": "Blood Vial", "action": "use", "hands": "any",
        "standing": [-0.07, -0.27, -0.14], "seated": [-0.07, -0.25, -0.14]},
       {"name": "left_hip", "item": "Molotov Cocktail", "action": "use", "hands": "any",
        "standing": [-0.17, -0.62, 0.03], "seated": [-0.21, -0.47, -0.17]},
       {"name": "left_hip_front", "item": "Pebble", "action": "use", "hands": "any",
        "standing": [-0.13, -0.66, -0.11], "seated": [-0.14, -0.45, -0.28]},
       {"name": "left_back", "item": "Hunter Blunderbuss", "action": "equip", "hands": "left",
        "standing": [-0.20, -0.55, 0.15], "seated": [-0.24, -0.42, 0.02]}
     ]}
  ],
  "game": {
    "_doc": "The game's VR UI (BB_VR_UI=1, bbport_xr.dll; docs/vr/findings/26-game-ui.md): its own menus, drawn over the game in the headset by the same code as the sandbox. The dashboard (the standing left stick click) is one panel: what each hand holds, the weapon slots, the quick slots, the inventory and buttons for what has no button of its own (options, gestures, the d-pad, recenter); point at it with either controller and pull that trigger, or use the sticks: either stick moves a highlight between the entries (nearest entry in the pushed direction, repeating while held), A or a trigger pulls the highlighted entry, B closes it (menu keys stick_nav and block_move, both default true; block_move keeps the character from walking, turning or swinging while the dashboard is open). Items: @quick = the game's quick slots, @inventory = every item the player carries, @weapons = the equipped weapon slots (right 1, right 2, left 1, left 2; @weapons/N one of them), goods:<id> / weapon:<id> / a name = one item with its live count. What you pick is done with the game's own buttons (d-pad to the slot, square to use). An older game object (version < 2) is replaced with these defaults.",
    "version": 2,
    "active": ["dashboard", "wrist_counters"],
    "r3_hold_s": 0.5,
    "press_s": 0.06,
    "debug_gun_ray": false, "gun_laser": "hold", "gun_laser_yaw_deg": 10, "gun_laser_pitch_deg": 0, "debug_gun_ray_length_m": 30, "debug_gun_ray_color": [1.0, 0.15, 0.1, 1.0],
    "debug_hitboxes": {"weapon": false, "attack": false, "enemies": false, "self": false, "hits": false,
                       "line_mrad": 1.5, "idle_alpha": 0.6},
    "hud": {"on": true, "head": false, "wrist": true, "bar": "always", "hp_color": "green", "stamina_color": "green", "distance": 0.9,
            "down_deg": 24, "scale": 1.0, "wrist_scale": 0.7, "wrist_offset": [0.0, 0.08, 0.0],
            "label_s": 1.6},
    "haptics": {"tick_light": [0.2, 0.025], "tick_heavy": [0.4, 0.035], "hit_light": [0.6, 0.06],
                "hit_heavy": [1.0, 0.12], "tired": [0.5, 0.05]},
    "held_items": true,
    "custom_hands": {"on": false, "color": [0.30, 0.22, 0.17, 1.0], "scale": 1.0, "weapon_grip": true},
    "held": {"grab_squeeze": 0.6, "release_squeeze": 0.35, "pickup_grace_s": 5.0,
             "hand_ring": false, "mouth": [0.0, -0.10, -0.07], "mouth_radius_m": 0.24, "below_eyes_m": 0.03, "mouth_hold_s": 0.3,
             "blade_radius_m": 0.10, "blade_start": 0.25, "blade_mid_radius_m": 0.42, "blade_mid_offset_m": 0.25, "blade_hold_s": 0.12, "blade_fallback_m": 0.18,
             "throw_min_speed": 1.5, "throw_window_s": 0.08, "throw_peak_weight": 0.5,
             "crush_squeeze": 0.92, "crush_hold_s": 0.4, "hide_weapon": true,
             "lockout_s": 0.8, "use_timeout_s": 4.0,
             "pick_haptic": [0.3, 0.03], "use_haptic": [0.5, 0.05], "touch_haptic": [0.2, 0.03],
             "cancel_haptic": [0.25, 0.04]},
    "collision_haptics": {"on": true, "hit": [0.55, 0.07], "hit_strong": [1.0, 0.16], "wall": [0.9, 0.18],
                          "object": [0.7, 0.12], "strength_ref": 40, "scale_by_strength": true},
    "menus": [
      {"name": "dashboard", "type": "dashboard", "label": "Dashboard", "open": "l3",
       "mode": "toggle", "columns": 10, "rows": 2, "cell": 0.055, "distance": 0.75,
       "below": 0.12, "tilt": 12, "anchor": "world", "stick_nav": true, "block_move": true,
       "tabs": ["all", "heal", "throw", "buff", "tool", "ammo", "weapons"],
       "buttons": [{"label": "Options", "action": "press:options"},
                   {"label": "Gestures", "action": "press:touchpad"},
                   {"label": "Tool / bullet", "action": "press:dpad_up"},
                   {"label": "Next quick", "action": "press:dpad_down"},
                   {"label": "Swap left", "action": "press:dpad_left"},
                   {"label": "Swap right", "action": "press:dpad_right"},
                   {"label": "Recenter", "action": "recenter"},
                   {"label": "Close", "action": "close"}]},
      {"name": "quick_ring", "type": "radial", "label": "Quick items", "hand": "right",
       "open": "l3", "mode": "toggle", "action": "use", "items": ["@quick"],
       "select": ["stick", "point"], "pointer": "left", "click": "left_trigger",
       "confirm": "right_trigger", "radius": 0.13, "inner_radius": 0.05, "item_size": 0.05,
       "gap": 0.004, "forward": 0.10, "up": 0.06, "deadzone": 0.5, "use_on_release": true,
       "on_top": true},
      {"name": "quick_wheel", "type": "radial", "label": "Quick wheel", "hand": "right",
       "open": "r3", "mode": "toggle", "action": "use", "items": ["@quick", "@weapons"],
       "select": ["touch", "point", "stick"], "pointer": "right", "click": "right_trigger",
       "confirm": "right_trigger", "anchor": "world", "radius": 0.15, "inner_radius": 0.055,
       "item_size": 0.05, "gap": 0.004, "forward": 0.0, "up": 0.0, "deadzone": 0.5,
       "use_on_release": true, "on_top": true},
      {"name": "inventory", "type": "grid", "label": "Inventory", "open": "r3", "mode": "toggle",
       "action": "use", "items": ["@inventory"], "select": ["point", "touch"], "pointer": "right",
       "click": "right_trigger", "columns": 5, "rows": 3, "cell": 0.08, "distance": 0.6,
       "below": 0.15, "tilt": 20, "anchor": "world",
       "tabs": ["all", "heal", "throw", "buff", "tool", "ammo"], "page_stick": "right",
       "on_top": true, "hide_weapons": false},
      {"name": "weapon_wheel", "type": "radial", "label": "Weapons", "hand": "right",
       "open": "right_grip+b", "mode": "hold", "action": "equip", "items": ["@weapons"],
       "select": ["stick", "point"], "pointer": "left", "click": "left_trigger",
       "radius": 0.15, "inner_radius": 0.055, "item_size": 0.062, "gap": 0.004,
       "forward": 0.07, "up": 0.04, "deadzone": 0.5, "use_on_release": true, "on_top": true},
      {"name": "wrist_counters", "type": "wrist", "label": "", "hand": "left", "show": "glance",
       "items": ["goods:1000", "goods:900"], "select": [], "glance_deg": 50, "view_deg": 40,
       "offset": [0.035, 0.0, 0.09], "normal": "+x", "width": 0.10, "height": 0.05,
       "on_top": true},
      {"name": "body_slots", "type": "body_slots", "posture": "auto", "seated_drop": 0.35,
       "markers": "near", "grab_radius": 0.10, "marker_size": 0.045, "neck": [0.0, -0.10, 0.08],
       "on_top": true,
       "slots": [
         {"name": "right_hip", "item": "@weapons/1", "action": "equip", "hands": "right",
          "standing": [0.17, -0.62, 0.03], "seated": [0.21, -0.47, -0.17]},
         {"name": "left_hip", "item": "@weapons/3", "action": "equip", "hands": "left",
          "standing": [-0.17, -0.62, 0.03], "seated": [-0.21, -0.47, -0.17]},
         {"name": "chest", "item": "goods:1000", "action": "use", "hands": "any",
          "standing": [-0.07, -0.27, -0.14], "seated": [-0.07, -0.25, -0.14]}
       ]}
    ]
  }
}
)JSON";

std::string Lower(std::string s) {
    for (char& c : s) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(a, b - a);
}

float Srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
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
void Read(const json::Value& v, UiColor& out) {
    if (v.IsArray() && v.Size() >= 3) {
        out = SrgbColor(v[0].Float(), v[1].Float(), v[2].Float(), v.Size() > 3 ? v[3].Float() : 1);
    }
}
void Read(const json::Value& v, UiPulse& out) {
    if (v.IsArray() && v.Size() >= 2) {
        out = {v[0].Float(), v[1].Float()};
    }
}
void Read(const json::Value& v, std::vector<std::string>& out) {
    if (v.IsArray()) {
        out.clear();
        for (const json::Value& s : v.array) {
            if (s.IsString()) {
                out.push_back(s.Str());
            }
        }
    }
}
void ReadBinding(const json::Value& v, UiBinding& out, const std::string& where,
                 std::string& warnings) {
    if (!v.IsString()) {
        return;
    }
    std::string error;
    out = ParseBinding(v.Str(), &error);
    if (!error.empty()) {
        warnings += where + ": " + error + "\n";
    }
}
int Hand(const json::Value& v, int fallback) {
    const std::string s = Lower(v.Str());
    return s == "left" ? 0 : s == "right" ? 1 : s == "other" || s == "none" ? -1 : fallback;
}

UiMenuSpec MenuDefaults(const std::string& type) {
    UiMenuSpec m;
    m.type = type;
    if (type == "radial") {
        m.select = {"stick", "move", "point"};
        m.on_top = true;
        m.click = ParseBinding("left_trigger");
    } else if (type == "grid") {
        m.mode = "toggle";
        m.hide_weapons = true;
        m.select = {"point", "touch"};
        m.pointer = 1;
        m.click = ParseBinding("right_trigger");
    } else if (type == "wrist") {
        m.hand = 0;
        m.on_top = true; // the held firearm would cut through it
        m.select = {"touch", "point"};
        m.pointer = 1;
        m.click = ParseBinding("right_trigger");
    } else if (type == "body_slots") {
        m.on_top = true;
    } else if (type == "dashboard") {
        m.mode = "toggle";
        m.on_top = true;
        m.select = {"point"};
        m.columns = 10;
        m.rows = 2;
        m.cell = 0.055f;
        m.distance = 0.75f;
        m.below = 0.12f;
        m.tilt = 12.0f;
        m.page_stick = -1;
    }
    return m;
}

void ReadMenu(const json::Value& d, UiMenuSpec& m, std::string& warnings) {
    const std::string where = "menu " + m.name;
    Read(d["label"], m.label);
    if (d.Has("hand") && Hand(d["hand"], -1) >= 0) {
        m.hand = Hand(d["hand"], m.hand);
    }
    ReadBinding(d["open"], m.open, where + " open", warnings);
    Read(d["mode"], m.mode);
    Read(d["action"], m.action);
    Read(d["items"], m.items);
    Read(d["select"], m.select);
    Read(d["on_top"], m.on_top);
    Read(d["hide_weapons"], m.hide_weapons);
    if (d.Has("pointer")) {
        m.pointer = Hand(d["pointer"], m.pointer);
    }
    ReadBinding(d["click"], m.click, where + " click", warnings);
    ReadBinding(d["confirm"], m.confirm, where + " confirm", warnings);
    Read(d["radius"], m.radius);
    Read(d["inner_radius"], m.inner_radius);
    Read(d["item_size"], m.item_size);
    Read(d["gap"], m.gap);
    Read(d["forward"], m.forward);
    Read(d["up"], m.up);
    Read(d["deadzone"], m.deadzone);
    Read(d["move_threshold"], m.move_threshold);
    Read(d["use_on_release"], m.use_on_release);
    Read(d["columns"], m.columns);
    Read(d["rows"], m.rows);
    Read(d["cell"], m.cell);
    Read(d["distance"], m.distance);
    Read(d["below"], m.below);
    Read(d["tilt"], m.tilt);
    Read(d["anchor"], m.anchor);
    Read(d["tabs"], m.tabs);
    if (d.Has("page_stick")) {
        m.page_stick = Hand(d["page_stick"], m.page_stick);
    }
    Read(d["stick_nav"], m.stick_nav);
    Read(d["block_move"], m.block_move);
    Read(d["show"], m.show);
    ReadBinding(d["hold"], m.open, where + " hold", warnings);
    Read(d["glance_deg"], m.glance_deg);
    Read(d["view_deg"], m.view_deg);
    Read(d["offset"], m.offset);
    Read(d["normal"], m.normal);
    Read(d["width"], m.width);
    Read(d["height"], m.height);
    Read(d["posture"], m.posture);
    Read(d["seated_drop"], m.seated_drop);
    Read(d["markers"], m.markers);
    Read(d["grab_radius"], m.grab_radius);
    Read(d["marker_size"], m.marker_size);
    Read(d["neck"], m.neck);
    if (d["buttons"].IsArray()) {
        m.buttons.clear();
        for (const json::Value& b : d["buttons"].array) {
            UiDashButton button;
            Read(b["label"], button.label);
            Read(b["action"], button.action);
            if (!button.action.empty()) {
                m.buttons.push_back(button);
            }
        }
    }
    if (d["slots"].IsArray()) {
        m.slots.clear();
        for (const json::Value& s : d["slots"].array) {
            UiBodySlot slot;
            Read(s["name"], slot.name);
            Read(s["item"], slot.item);
            Read(s["action"], slot.action);
            Read(s["hands"], slot.hands);
            Read(s["standing"], slot.standing);
            slot.seated = slot.standing;
            Read(s["seated"], slot.seated);
            Read(s["radius"], slot.radius);
            m.slots.push_back(slot);
        }
    }
    m.columns = std::clamp(m.columns, 1, 12);
    m.rows = std::clamp(m.rows, 1, 8);
    m.radius = std::max(m.radius, m.inner_radius + 0.01f);
}

void ReadMenus(const json::Value& list, std::vector<UiMenuSpec>& out, std::string& warnings) {
    if (!list.IsArray()) {
        return;
    }
    out.clear();
    for (const json::Value& m : list.array) {
        UiMenuSpec menu = MenuDefaults(Lower(m["type"].Str()));
        Read(m["name"], menu.name);
        if (menu.name.empty()) {
            warnings += "a menu without a name is ignored\n";
            continue;
        }
        if (menu.type != "radial" && menu.type != "grid" && menu.type != "wrist" &&
            menu.type != "body_slots" && menu.type != "dashboard") {
            warnings += "menu " + menu.name + ": unknown type \"" + menu.type +
                        "\" (radial, grid, wrist, body_slots, dashboard)\n";
            continue;
        }
        ReadMenu(m, menu, warnings);
        out.push_back(menu);
    }
}

void Apply(const json::Value& d, UiLayout& out, std::string& warnings) {
    Read(d["active_preset"], out.active_preset);
    ReadBinding(d["cycle_next"], out.cycle_next, "cycle_next", warnings);
    ReadBinding(d["cycle_prev"], out.cycle_prev, "cycle_prev", warnings);
    ReadBinding(d["help"], out.help, "help", warnings);
    ReadBinding(d["reset"], out.reset, "reset", warnings);
    Read(d["log"], out.log);
    Read(d["infinite"], out.infinite);
    const json::Value& s = d["style"];
    UiStyle& st = out.style;
    Read(s["panel"], st.panel);
    Read(s["panel_border"], st.panel_border);
    Read(s["slot"], st.slot);
    Read(s["slot_border"], st.slot_border);
    Read(s["highlight"], st.highlight);
    Read(s["highlight_border"], st.highlight_border);
    Read(s["text"], st.text);
    Read(s["text_dim"], st.text_dim);
    Read(s["accent"], st.accent);
    Read(s["laser"], st.laser);
    Read(s["open_seconds"], st.open_seconds);
    Read(s["close_seconds"], st.close_seconds);
    Read(s["text_outline"], st.text_outline);
    Read(s["haptic_open"], st.haptic_open);
    Read(s["haptic_hover"], st.haptic_hover);
    Read(s["haptic_select"], st.haptic_select);
    Read(s["haptic_grab"], st.haptic_grab);
    Read(s["haptic_empty"], st.haptic_empty);
    Read(s["haptic_preset"], st.haptic_preset);
    st.open_seconds = std::max(st.open_seconds, 0.001f);
    st.close_seconds = std::max(st.close_seconds, 0.001f);
    if (d["inventory"].IsArray()) {
        out.inventory.clear();
        for (const json::Value& e : d["inventory"].array) {
            UiInventoryEntry entry;
            if (e.IsString()) {
                entry.item = e.Str();
            } else {
                Read(e["item"], entry.item);
                Read(e["count"], entry.count);
            }
            if (!entry.item.empty()) {
                out.inventory.push_back(entry);
            }
        }
    }
    if (d["presets"].IsArray()) {
        out.presets.clear();
        for (const json::Value& p : d["presets"].array) {
            UiPreset preset;
            Read(p["name"], preset.name);
            Read(p["label"], preset.label);
            Read(p["notes"], preset.notes);
            Read(p["menus"], preset.menus);
            if (preset.label.empty()) {
                preset.label = preset.name;
            }
            if (!preset.name.empty()) {
                out.presets.push_back(preset);
            }
        }
    }
    ReadMenus(d["menus"], out.menus, warnings);
    const json::Value& g = d["game"];
    if (g.IsObject()) {
        UiGameLayout& game = out.game;
        Read(g["version"], game.version);
        Read(g["active"], game.active);
        ReadMenus(g["menus"], game.menus, warnings);
        Read(g["lock_on"], game.lock_on);
        Read(g["r3_hold_s"], game.r3_hold_s);
        game.r3_hold_s = std::clamp(game.r3_hold_s, 0.15f, 3.0f);
        Read(g["press_s"], game.press_s);
        game.press_s = std::clamp(game.press_s, 0.04f, 0.5f);
        Read(g["debug_gun_ray"], game.debug_gun_ray);
        Read(g["gun_laser"], game.gun_laser);
        if (game.gun_laser != "always" && game.gun_laser != "off") game.gun_laser = "hold";
        Read(g["gun_laser_yaw_deg"], game.gun_laser_yaw_deg);
        Read(g["gun_laser_pitch_deg"], game.gun_laser_pitch_deg);
        game.gun_laser_yaw_deg = std::clamp(game.gun_laser_yaw_deg, -180.0f, 180.0f);
        game.gun_laser_pitch_deg = std::clamp(game.gun_laser_pitch_deg, -89.0f, 89.0f);
        Read(g["debug_gun_ray_length_m"], game.debug_gun_ray_length_m);
        game.debug_gun_ray_length_m = std::clamp(game.debug_gun_ray_length_m, 0.5f, 200.0f);
        Read(g["debug_gun_ray_color"], game.debug_gun_ray_color);
        {
            const json::Value& dh = g["debug_hitboxes"];
            UiDebugHitboxes& hb = game.debug_hitboxes;
            Read(dh["weapon"], hb.weapon);
            Read(dh["attack"], hb.attack);
            Read(dh["enemies"], hb.enemies);
            Read(dh["self"], hb.self);
            Read(dh["hits"], hb.hits);
            Read(dh["line_mrad"], hb.line_mrad);
            hb.line_mrad = std::clamp(hb.line_mrad, 0.2f, 20.0f);
            Read(dh["idle_alpha"], hb.idle_alpha);
            hb.idle_alpha = std::clamp(hb.idle_alpha, 0.0f, 1.0f);
        }
        const json::Value& h = g["hud"];
        Read(h["on"], game.hud);
        Read(h["head"], game.hud_head);
        Read(h["wrist"], game.hud_wrist);
        Read(h["bar"], game.bar);
        Read(h["hp_color"], game.hp_color);
        Read(h["stamina_color"], game.stamina_color);
        Read(h["distance"], game.hud_distance);
        Read(h["down_deg"], game.hud_down_deg);
        Read(h["scale"], game.hud_scale);
        Read(h["wrist_scale"], game.wrist_scale);
        Read(h["wrist_offset"], game.wrist_offset);
        float label_s = float(game.label_s);
        Read(h["label_s"], label_s);
        game.label_s = label_s;
        const json::Value& p = g["haptics"];
        Read(p["tick_light"], game.tick_light);
        Read(p["tick_heavy"], game.tick_heavy);
        Read(p["hit_light"], game.hit_light);
        Read(p["hit_heavy"], game.hit_heavy);
        Read(p["tired"], game.tired);
        Read(g["held_items"], game.held_items);
        {
            const json::Value& ch = g["custom_hands"];
            UiCustomHands& c = game.custom_hands;
            if (ch.type == json::Value::Type::Bool) {
                c.on = ch.boolean;
            } else {
                Read(ch["on"], c.on);
                Read(ch["color"], c.color);
                Read(ch["scale"], c.scale);
                Read(ch["weapon_grip"], c.weapon_grip);
            }
            c.scale = std::clamp(c.scale, 0.6f, 1.5f);
        }
        {
            const json::Value& hv = g["held"];
            UiHeldItems& hi = game.held;
            Read(hv["grab_squeeze"], hi.grab_squeeze);
            Read(hv["release_squeeze"], hi.release_squeeze);
            Read(hv["pickup_grace_s"], hi.pickup_grace_s);
            Read(hv["hold"], hi.hold);
            Read(hv["mouth"], hi.mouth);
            Read(hv["mouth_radius_m"], hi.mouth_radius_m);
            Read(hv["hand_ring"], hi.hand_ring);
            Read(hv["below_eyes_m"], hi.below_eyes_m);
            Read(hv["mouth_hold_s"], hi.mouth_hold_s);
            Read(hv["blade_radius_m"], hi.blade_radius_m);
            Read(hv["blade_start"], hi.blade_start);
            Read(hv["blade_mid_radius_m"], hi.blade_mid_radius_m);
            Read(hv["blade_mid_offset_m"], hi.blade_mid_offset_m);
            Read(hv["blade_hold_s"], hi.blade_hold_s);
            Read(hv["blade_fallback_m"], hi.blade_fallback_m);
            Read(hv["throw_min_speed"], hi.throw_min_speed);
            Read(hv["throw_window_s"], hi.throw_window_s);
            Read(hv["throw_peak_weight"], hi.throw_peak_weight);
            Read(hv["throw_mode"], hi.throw_mode);
            Read(hv["throw_aim_pitch_bias_deg"], hi.throw_aim_pitch_bias_deg);
            Read(hv["throw_aim_level_deg"], hi.throw_aim_level_deg);
            Read(hv["throw_aim_gain"], hi.throw_aim_gain);
            Read(hv["throw_aim_min_pitch_deg"], hi.throw_aim_min_pitch_deg);
            Read(hv["throw_aim_max_pitch_deg"], hi.throw_aim_max_pitch_deg);
            Read(hv["throw_eye_height_m"], hi.throw_eye_height_m);
            Read(hv["crush_squeeze"], hi.crush_squeeze);
            Read(hv["crush_hold_s"], hi.crush_hold_s);
            Read(hv["hide_weapon"], hi.hide_weapon);
            Read(hv["lockout_s"], hi.lockout_s);
            Read(hv["use_timeout_s"], hi.use_timeout_s);
            Read(hv["pick_haptic"], hi.pick);
            Read(hv["use_haptic"], hi.use);
            Read(hv["touch_haptic"], hi.touch);
            Read(hv["cancel_haptic"], hi.cancel);
            hi.grab_squeeze = std::clamp(hi.grab_squeeze, 0.1f, 1.0f);
            hi.release_squeeze = std::clamp(hi.release_squeeze, 0.0f, hi.grab_squeeze);
            hi.pickup_grace_s = std::clamp(hi.pickup_grace_s, 0.5f, 30.0f);
            hi.mouth_radius_m = std::clamp(hi.mouth_radius_m, 0.03f, 0.5f);
            hi.mouth_hold_s = std::clamp(hi.mouth_hold_s, 0.0f, 3.0f);
            hi.blade_radius_m = std::clamp(hi.blade_radius_m, 0.02f, 0.5f);
            hi.blade_start = std::clamp(hi.blade_start, 0.0f, 0.9f);
            hi.blade_mid_radius_m = std::clamp(hi.blade_mid_radius_m, 0.02f, 0.5f);
            hi.blade_mid_offset_m = std::clamp(hi.blade_mid_offset_m, 0.0f, 0.6f);
            hi.blade_hold_s = std::clamp(hi.blade_hold_s, 0.0f, 3.0f);
            hi.throw_min_speed = std::clamp(hi.throw_min_speed, 0.3f, 15.0f);
            hi.throw_window_s = std::clamp(hi.throw_window_s, 0.02f, 0.3f);
            hi.throw_peak_weight = std::clamp(hi.throw_peak_weight, 0.0f, 1.0f);
            if (hi.throw_mode != "aim" && hi.throw_mode != "flick") {
                hi.throw_mode = "aim";
            }
            hi.throw_aim_pitch_bias_deg = std::clamp(hi.throw_aim_pitch_bias_deg, -45.0f, 45.0f);
            hi.throw_aim_level_deg = std::clamp(hi.throw_aim_level_deg, -60.0f, 60.0f);
            hi.throw_aim_gain = std::clamp(hi.throw_aim_gain, 0.2f, 3.0f);
            hi.throw_aim_min_pitch_deg = std::clamp(hi.throw_aim_min_pitch_deg, -80.0f, 80.0f);
            hi.throw_aim_max_pitch_deg = std::clamp(hi.throw_aim_max_pitch_deg, hi.throw_aim_min_pitch_deg, 85.0f);
            hi.throw_eye_height_m = std::clamp(hi.throw_eye_height_m, 0.5f, 2.5f);
            hi.crush_squeeze = std::clamp(hi.crush_squeeze, 0.3f, 1.0f);
            hi.crush_hold_s = std::clamp(hi.crush_hold_s, 0.0f, 3.0f);
            hi.lockout_s = std::clamp(hi.lockout_s, 0.0f, 5.0f);
            hi.use_timeout_s = std::clamp(hi.use_timeout_s, 1.0f, 20.0f);
        }
        {
            const json::Value& cv = g["collision_haptics"];
            UiCollisionHaptics& ch = game.collision_haptics;
            Read(cv["on"], ch.on);
            Read(cv["hit"], ch.hit);
            Read(cv["hit_strong"], ch.hit_strong);
            Read(cv["wall"], ch.wall);
            Read(cv["object"], ch.object);
            Read(cv["strength_ref"], ch.strength_ref);
            Read(cv["scale_by_strength"], ch.scale_by_strength);
            ch.strength_ref = std::clamp(ch.strength_ref, 1.0f, 500.0f);
        }
    }
}

} // namespace

UiColor SrgbColor(float r, float g, float b, float a) {
    return {Srgb(r), Srgb(g), Srgb(b), std::clamp(a, 0.0f, 1.0f)};
}

UiBinding ParseBinding(const std::string& text, std::string* error) {
    static const struct {
        const char* name;
        UiInputId id;
    } kNames[] = {{"left_grip", UiInputId::LeftGrip},     {"right_grip", UiInputId::RightGrip},
                  {"left_trigger", UiInputId::LeftTrigger},
                  {"right_trigger", UiInputId::RightTrigger},
                  {"a", UiInputId::A},                     {"b", UiInputId::B},
                  {"x", UiInputId::X},                     {"y", UiInputId::Y},
                  {"l3", UiInputId::L3},                   {"left_stick_click", UiInputId::L3},
                  {"r3", UiInputId::R3},                   {"right_stick_click", UiInputId::R3},
                  {"dpad_up", UiInputId::DpadUp},          {"dpad_down", UiInputId::DpadDown},
                  {"dpad_left", UiInputId::DpadLeft},      {"dpad_right", UiInputId::DpadRight},
                  {"options", UiInputId::Options},         {"touchpad", UiInputId::Touchpad}};
    UiBinding binding;
    binding.text = Trim(text);
    std::string rest = Lower(binding.text);
    if (rest.empty() || rest == "none") {
        return binding;
    }
    std::vector<UiInputId> tokens;
    size_t start = 0;
    while (start <= rest.size()) {
        size_t plus = rest.find('+', start);
        if (plus == std::string::npos) {
            plus = rest.size();
        }
        const std::string token = Trim(rest.substr(start, plus - start));
        UiInputId id = UiInputId::None;
        for (const auto& n : kNames) {
            if (token == n.name) {
                id = n.id;
            }
        }
        if (id == UiInputId::None) {
            if (error) {
                *error = "unknown input \"" + token + "\" in \"" + binding.text + "\"";
            }
            return UiBinding{{}, UiInputId::None, binding.text};
        }
        tokens.push_back(id);
        start = plus + 1;
    }
    binding.key = tokens.back();
    tokens.pop_back();
    binding.modifiers = tokens;
    return binding;
}

const char* DefaultLayoutText() {
    return kDefaultLayout;
}

const UiMenuSpec* UiLayout::FindMenu(const std::string& name) const {
    for (const UiMenuSpec& m : menus) {
        if (m.name == name) {
            return &m;
        }
    }
    return nullptr;
}

const UiMenuSpec* UiLayout::FindGameMenu(const std::string& name) const {
    for (const UiMenuSpec& m : game.menus) {
        if (m.name == name) {
            return &m;
        }
    }
    return nullptr;
}

int UiLayout::FindPreset(const std::string& name) const {
    for (size_t i = 0; i < presets.size(); ++i) {
        if (presets[i].name == name) {
            return int(i);
        }
    }
    return -1;
}

bool ParseLayout(const std::string& text, UiLayout& out, std::string& error,
                 std::string* warnings) {
    std::string notes;
    UiLayout layout;
    {
        std::string base_error;
        const json::Value base = json::Parse(kDefaultLayout, &base_error);
        if (!base_error.empty()) {
            error = "built-in layout: " + base_error;
            return false;
        }
        std::string ignored;
        Apply(base, layout, ignored);
    }
    const json::Value doc = json::Parse(text, &error);
    if (!error.empty()) {
        return false;
    }
    if (!doc.IsObject()) {
        error = "the layout is not a JSON object";
        return false;
    }
    Apply(doc, layout, notes);
    for (const UiPreset& p : layout.presets) {
        for (const std::string& m : p.menus) {
            if (!layout.FindMenu(m)) {
                notes += "preset " + p.name + ": no menu named \"" + m + "\"\n";
            }
        }
    }
    for (const std::string& m : layout.game.active) {
        if (!layout.FindGameMenu(m)) {
            notes += "game: no game menu named \"" + m + "\"\n";
        }
    }
    if (!layout.presets.empty() && layout.FindPreset(layout.active_preset) < 0) {
        notes += "active_preset \"" + layout.active_preset + "\" not found, using " +
                 layout.presets[0].name + "\n";
        layout.active_preset = layout.presets[0].name;
    }
    if (warnings) {
        *warnings = notes;
    }
    out = std::move(layout);
    return true;
}

bool UiLayoutFile::Poll(double now, UiLayout& layout, std::string& log) {
    if (loaded && now < next_poll) {
        return false;
    }
    next_poll = now + 0.5;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file(path, std::ios::binary);
        file << kDefaultLayout;
        log += "wrote the default VR menu layout to " + path.string() + "\n";
    }
    const auto stamp_now = std::filesystem::last_write_time(path, ec);
    if (ec) {
        if (!loaded) {
            std::string error, warnings;
            ParseLayout("{}", layout, error, &warnings);
            loaded = true;
            log += "cannot read " + path.string() + ": using the built-in layout\n";
            return true;
        }
        return false;
    }
    if (loaded && stamp_now == stamp) {
        return false;
    }
    stamp = stamp_now;
    std::string error, warnings;
    UiLayout next;
    if (!ParseLayout(ReadText(path), next, error, &warnings)) {
        log += path.string() + ": " + error + " (keeping the previous layout)\n";
        if (!loaded) {
            ParseLayout("{}", layout, error, &warnings);
            loaded = true;
            return true;
        }
        return false;
    }
    log += std::string(loaded ? "VR menu layout reloaded from " : "VR menu layout loaded from ") +
           path.string() + ": " + std::to_string(next.presets.size()) + " presets, " +
           std::to_string(next.menus.size()) + " menus, active \"" + next.active_preset + "\"\n" +
           warnings;
    layout = std::move(next);
    loaded = true;
    return true;
}

} // namespace sb
