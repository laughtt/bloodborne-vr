// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The VR menu prototypes' layout file, out/vr_assets/ui_layouts.json: style, inventory, the menus
// (radial, grid, wrist, body_slots) and the presets that group them. Written with the defaults on
// the first run and reloaded when saved (polled twice a second). Keys a menu leaves out keep that
// menu type's defaults; top-level keys left out keep the built-in default layout's. The format is
// documented in README.md ("VR menu prototypes").

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "json.h"
#include "math3d.h"

namespace sb {

/// Linear RGB, straight alpha (the layout file gives sRGB).
struct UiColor {
    float r = 1, g = 1, b = 1, a = 1;
    UiColor WithAlpha(float alpha) const { return {r, g, b, a * alpha}; }
};

/// One controller input of a binding.
enum class UiInputId : uint8_t {
    None,
    LeftGrip, RightGrip, LeftTrigger, RightTrigger,
    A, B, X, Y, L3, R3,
    DpadUp, DpadDown, DpadLeft, DpadRight, Options, Touchpad,
    Count
};

/// "right_grip+b": the last token is the key whose press acts, the others must already be held.
struct UiBinding {
    std::vector<UiInputId> modifiers;
    UiInputId key = UiInputId::None;
    std::string text;
    bool Empty() const { return key == UiInputId::None; }
};

struct UiPulse {
    float amplitude = 0.3f, seconds = 0.02f;
};

struct UiStyle {
    UiColor panel, panel_border, slot, slot_border, highlight, highlight_border, text, text_dim,
        accent, laser;
    float open_seconds = 0.12f, close_seconds = 0.08f;
    float text_outline = 0.10f; // SDF widening of the dark outline behind text
    UiPulse haptic_open{0.25f, 0.02f}, haptic_hover{0.15f, 0.012f}, haptic_select{0.6f, 0.05f},
        haptic_grab{0.8f, 0.06f}, haptic_empty{0.35f, 0.12f}, haptic_preset{0.4f, 0.04f};
};

struct UiBodySlot {
    std::string name, item;
    std::string action = "use"; // use | equip
    std::string hands = "any";  // any | left | right
    m3::Vec3 standing, seated;  // from the neck pivot, body yaw frame: +X right, +Y up, +Z back
    float radius = 0.0f;        // 0: the menu's grab_radius
};

/// A dashboard button: its label and what it does ("press:options", "press:touchpad",
/// "press:dpad_up", "recenter", "mode", "close", "assign_quick", ...).
struct UiDashButton {
    std::string label, action;
};

struct UiMenuSpec {
    std::string name, type; // type: radial | grid | wrist | body_slots | dashboard
    std::string label;
    int hand = 1;            // the hand it belongs to: 0 left, 1 right
    UiBinding open;          // radial/grid: opens it; wrist: must be held to show (show "hold")
    std::string mode = "hold"; // hold (open while held) | toggle
    std::string action = "use"; // use (count down) | equip (weapons into their hand)
    std::vector<std::string> items;
    std::vector<std::string> select; // stick, move, point, touch
    bool on_top = false;     // drawn over the scene (no depth test)
    bool hide_weapons = false; // the held weapons are hidden while it is open (they cover it)
    int pointer = -1;        // the pointing hand: -1 the other hand, 0 left, 1 right
    UiBinding click;         // pointing: the press that selects ("right_trigger")
    UiBinding confirm;       // radial: selects the hovered item, however it was picked (none)

    // radial
    float radius = 0.13f, inner_radius = 0.05f, item_size = 0.048f, gap = 0.004f;
    float forward = 0.07f, up = 0.03f; // from the hand: away from the head, and up
    float deadzone = 0.5f, move_threshold = 0.035f;
    bool use_on_release = true;

    // grid
    int columns = 5, rows = 3;
    float cell = 0.08f, distance = 0.6f, below = 0.15f, tilt = 20.0f;
    int page_stick = 1; // the stick that turns pages (x) and tabs (y) (-1 none)
    std::string anchor = "world";  // world (placed when opened) | head (follows) | lazy (catches up); radial: world | hand (follows the hand)
    std::vector<std::string> tabs; // filters by an item's use: all, heal, ammo, throw, buff, tool

    // wrist
    std::string show = "glance"; // glance | hold | always | glance+hold
    float glance_deg = 50.0f, view_deg = 40.0f;
    m3::Vec3 offset{0.035f, 0.0f, 0.09f}; // grip space: the panel center (+X: off the palm side)
    std::string normal = "+x";            // grip axis the panel faces (left hand: +x palm side)
    float width = 0.14f, height = 0.055f;

    // body_slots
    std::string posture = "auto"; // auto | standing | seated
    float seated_drop = 0.35f;    // head this far below the calibrated eye height: seated
    std::string markers = "always"; // always | near | never
    float grab_radius = 0.10f, marker_size = 0.045f;
    m3::Vec3 neck{0.0f, -0.10f, 0.08f}; // the neck pivot in head space (slots hang from it)
    std::vector<UiBodySlot> slots;

    // dashboard (the game's one panel: hands, weapon slots, quick slots, inventory, buttons; uses
    // columns, rows, cell, distance, below, tilt, tabs; pointed at with either hand's ray, that
    // hand's trigger clicks)
    std::vector<UiDashButton> buttons;
    // The sticks drive the dashboard: either stick moves a focus between its entries (initial
    // delay then repeat), A or a trigger activates it, B closes. block_move: while it is open the
    // game gets no stick (no walking, no turning) and no swings or weapon swaps.
    bool stick_nav = true, block_move = true;
};

struct UiPreset {
    std::string name, label, notes;
    std::vector<std::string> menus;
};

struct UiInventoryEntry {
    std::string item;
    int count = 1;
};

/// The game's VR UI (the "game" object of ui_layouts.json; the bridge, BB_VR_UI=1): its own
/// menus, so the sandbox presets and the game never step on each other, plus the swing and
/// stamina HUD and the haptics of the game's swings.
/// The game's hitbox debug overlay ("game" object, "debug_hitboxes"; vr_game_hitbox.h).
struct UiDebugHitboxes {
    bool weapon = false; // the held weapons' hit capsules (every attack's, each shape once)
    bool attack = false; // the current attack's shapes highlighted, white flash on a hit
    bool enemies = false; // enemy hurtboxes (not drawn yet: findings/29 section 5)
    bool self = false;   // the player's own hurtboxes (chr_c0000)
    bool hits = false;   // markers where the player's hits landed (3 s)
    float line_mrad = 1.5f;  // line width as an angle (milliradians: ~1.5 px at 1 mrad/px)
    float idle_alpha = 0.6f; // the weapon's shapes outside an attack
};

/// Items in the hand (the "game" object's "held_items" / "held", vr_held_items.h): a consumable
/// picked on the wheel is held in a hand and used by a gesture instead of at once.
struct UiHeldItems {
    float grab_squeeze = 0.60f, release_squeeze = 0.35f; // the grip that holds / lets go
    float pickup_grace_s = 5.0f;      // the grip must close this soon after the pick
    bool hand_ring = false; // the progress ring at the hand and the mouth ring (off: the hint text only)
    m3::Vec3 hold{0.0f, 0.0f, -0.05f}; // the item's point relative to the grip pose
    // Heals, cures, body buffs: the item within mouth_radius_m of the mouth (head local) and below
    // the eyes for mouth_hold_s (the sandbox's consumables: mouth, mouth_radius_m, below_eyes_m).
    m3::Vec3 mouth{0.0f, -0.10f, -0.07f};
    float mouth_radius_m = 0.24f, below_eyes_m = 0.03f, mouth_hold_s = 0.3f;
    // Weapon buffs: the item within blade_radius_m of the blade (blade_start: the share of the weapon's
    // length that is handle) for blade_hold_s: a touch, not a rub.
    float blade_radius_m = 0.10f, blade_start = 0.25f, blade_hold_s = 0.12f;
    // The extra mid zone: a sphere of blade_mid_radius_m (was 0.14, now 3x) in the middle of the blade part,
    // moved blade_mid_offset_m out of the blade toward the player's head.
    float blade_mid_radius_m = 0.42f, blade_mid_offset_m = 0.25f;
    float blade_fallback_m = 0.18f;   // no hitbox data for the weapon: this far around its bone
    // Throwables: letting go of the grip faster than this throws (m/s); slower puts the item back.
    float throw_min_speed = 1.5f;
    float throw_window_s = 0.08f, throw_peak_weight = 0.5f; // the release velocity estimate
    // Aimed throw (default): hold the grip to aim, let go to throw. "flick" is the old release-velocity throw.
    std::string throw_mode = "aim";
    // launch = bias + gain * (grip elevation - level): a neutral hand (grip -Z about +20 deg) throws nearly flat
    float throw_aim_pitch_bias_deg = 3.0f, throw_aim_level_deg = 20.0f, throw_aim_gain = 1.2f;
    float throw_aim_min_pitch_deg = -30.0f, throw_aim_max_pitch_deg = 60.0f;
    float throw_eye_height_m = 1.65f; // the floor is this far under the headset
    // Marks (Hunter's Mark...): squeezing the grip to at least crush_squeeze for crush_hold_s crushes it.
    float crush_squeeze = 0.92f, crush_hold_s = 0.4f;
    // Tells the host (BBXR_UI_HELD) which item is in a hand so it hides that hand's weapon.
    bool hide_weapon = true;
    float lockout_s = 0.8f;           // swings and the swap tap stay off this long after the item left the hand
    float use_timeout_s = 4.0f;       // a gesture's action the game never answered
    UiPulse pick{0.3f, 0.03f}, use{0.5f, 0.05f}, touch{0.2f, 0.03f}, cancel{0.25f, 0.04f};
};

/// The weapon hand's haptics when the game's weapon collides ("game" object, "collision_haptics").
struct UiCollisionHaptics {
    bool on = true;
    UiPulse hit{0.55f, 0.07f};        // a hit on a character, weak
    UiPulse hit_strong{1.0f, 0.16f};  // ... at strength_ref stamina paid or more
    UiPulse wall{0.9f, 0.18f};        // the attack bounced off a wall
    UiPulse object{0.7f, 0.12f};      // ... or an object
    float strength_ref = 40.0f;       // the stamina an attack pays at full strength
    bool scale_by_strength = true;    // hit / wall / object blend towards hit_strong by strength
};

/// The custom VR hands ("game" object, "custom_hands"; vr_hand_model.h, docs/vr/findings/40):
/// the generic hand model at each controller in place of the game's hands (fp.txt hide=nohands).
/// Off by default; BB_XR_CUSTOM_HANDS=1/0 overrides `on`.
struct UiCustomHands {
    bool on = false;
    UiColor color{0.30f, 0.22f, 0.17f, 1.0f}; // a dark leather glove (linear)
    float scale = 1.0f;                      // the model's size (the asset is an adult hand)
    bool weapon_grip = true;                 // a hand holding a game weapon keeps the weapon grip
};

struct UiGameLayout {
    int version = 0;                 // the "game" object's version (older ones are replaced)
    std::vector<std::string> active; // the game menus in use (names in `menus`)
    std::vector<UiMenuSpec> menus;
    bool lock_on = true;             // false: R3 never reaches the game (VR aims with head and hands)
    float r3_hold_s = 0.5f;          // R3 held this long opens the "r3" menus; shorter is lock-on
    // The swing and stamina HUD (combat_hud.h).
    bool hud = true;
    bool hud_head = false, hud_wrist = true; // the user asked for the wrist bar only
    std::string bar = "always";      // always | action (shown for 3 s after a swing or a cost)
    // The wrist bars' colours (hud_bars.h ParseBarColor: a word, "#rrggbb" or "r,g,b"); both green
    // by default. BB_VR_HP_COLOR / BB_VR_STAMINA_COLOR and the control file's hp_color= /
    // stamina_color= override them.
    std::string hp_color = "green", stamina_color = "green";
    float hud_distance = 0.9f, hud_down_deg = 24.0f, hud_scale = 1.0f, wrist_scale = 0.7f;
    m3::Vec3 wrist_offset{0.0f, 0.08f, 0.0f};
    double label_s = 1.6;
    // Haptics of the game's swings (right hand): a weak tick when a swing registers, a pulse
    // when it hits, two short buzzes when a swing finds no stamina.
    UiPulse tick_light{0.2f, 0.025f}, tick_heavy{0.4f, 0.035f};
    UiPulse hit_light{0.6f, 0.06f}, hit_heavy{1.0f, 0.12f}, tired{0.5f, 0.05f};
    // Physical items: a consumable picked on the wheel is held, then used by a gesture (mouth,
    // blade, throw); false: picking it uses it at once, as before.
    bool held_items = true;
    UiHeldItems held;
    UiCustomHands custom_hands;
    UiCollisionHaptics collision_haptics;
    // Item use through the game's own buttons (src/runtime_vr_ui.c).
    float press_s = 0.06f;           // how long each d-pad / square press is held
    // DEBUG: the left controller's aim ray (where the gun is pointed), brighter 0.3 s after a pull.
    bool debug_gun_ray = false;
    float debug_gun_ray_length_m = 30.0f;
    UiColor debug_gun_ray_color{1.0f, 0.02f, 0.01f, 1.0f}; // linear = sRGB (1, 0.15, 0.1)
    // The gun laser: "hold" (while the left trigger is held aiming, and the flash on the shot),
    // "always" (what debug_gun_ray: true means) or "off". The trim turns the drawn ray like the
    // host's gun_yaw / gun_pitch keys turn the shots: + = right / up, degrees.
    std::string gun_laser = "hold";
    float gun_laser_yaw_deg = 10.0f, gun_laser_pitch_deg = 0.0f;
    // DEBUG: the sandbox's hitboxes over the game (vr_game_hitbox.h, findings/29).
    UiDebugHitboxes debug_hitboxes;
};

struct UiLayout {
    std::string active_preset;
    UiBinding cycle_next, cycle_prev, help, reset;
    bool log = true;
    bool infinite = false; // items never run out
    UiStyle style;
    std::vector<UiInventoryEntry> inventory;
    std::vector<UiPreset> presets;
    std::vector<UiMenuSpec> menus;
    UiGameLayout game;

    const UiMenuSpec* FindMenu(const std::string& name) const;
    const UiMenuSpec* FindGameMenu(const std::string& name) const;
    int FindPreset(const std::string& name) const; // -1 when missing
};

/// The built-in default layout (what ui_layouts.json is written with).
const char* DefaultLayoutText();

/// Parses `text` over the built-in defaults. False (and `error`) on a JSON error; unknown menus
/// in presets and bad bindings are reported in `warnings`.
bool ParseLayout(const std::string& text, UiLayout& out, std::string& error,
                 std::string* warnings = nullptr);

UiBinding ParseBinding(const std::string& text, std::string* error = nullptr);
UiColor SrgbColor(float r, float g, float b, float a = 1.0f);

/// Watches the layout file: writes the defaults when it is missing, reloads it when saved.
class UiLayoutFile {
public:
    explicit UiLayoutFile(std::filesystem::path path) : path(std::move(path)) {}
    /// True when `layout` was (re)loaded; `log` says what happened (also on errors).
    bool Poll(double now, UiLayout& layout, std::string& log);
    const std::filesystem::path& Path() const { return path; }

private:
    std::filesystem::path path;
    std::filesystem::file_time_type stamp{};
    double next_poll = 0.0;
    bool loaded = false;
};

} // namespace sb
