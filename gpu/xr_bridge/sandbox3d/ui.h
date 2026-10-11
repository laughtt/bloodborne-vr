// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// VR menu prototypes for sandbox3d: pop-up menus for Bloodborne VR, data-driven from
// out/vr_assets/ui_layouts.json (ui_layout.h, hot-reloaded) and drawn with the game's own icons
// and font (ui_render.h). Menu types:
//   radial      a ring of items that pops up in front of a hand while a binding is held; pick by
//               stick direction, by moving that hand, by pointing the other hand (or touching);
//               release to use (or equip, for the weapon wheel)
//   grid        an inventory panel placed in front of you: pages of item cards; point and click,
//               or poke a card with the index fingertip; stick flicks turn pages
//   wrist       a strip on the forearm shown when you glance at it (and/or while a binding is
//               held): counts, touch or point to use
//   body_slots  items holstered on the body (hips, chest, back; standing and seated offsets):
//               reach and squeeze the grip to take or use
// A preset groups menus; the cycle binding switches presets (a toast shows its notes). Every
// interaction (open, hover, select with its method and timings, cancel, grab, glance) is logged
// to out/vr_assets/ui_log/ui_<time>.jsonl and summarized per preset.
//
// Frame order (main.cpp): FilterButtons (hides the keys the menus take from the scene),
// Scene::Update, Update, Scene::Build, Build (laser lines, triangles for the draw hook), render.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "math3d.h"
#include "renderer.h"
#include "ui_layout.h"
#include "ui_render.h"

namespace sb {

class Scene;
struct SceneInput;

/// The controllers and the head in the world space the scene renders (after locomotion).
struct UiInput {
    double time = 0.0;
    bool stage = false;
    float floor_y = 0.0f;
    UiPose head, grip[2], aim[2];
    float trigger[2] = {}, squeeze[2] = {};
    uint32_t buttons = 0;   // BBXR_BTN_* as the bridge reports them (before FilterButtons)
    float stick[2][2] = {}; // per hand: x right, y up, -1..1
};

/// The sticks of BbXrInput (0..255, 128 centered, y down) as -1..1, y up.
void UiSticks(uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, float stick[2][2]);

/// What the menus need from the body: its yaw (body slots turn with it) and the index fingertips
/// (touching panels).
struct UiBody {
    float yaw = 0.0f;
    m3::Vec3 tip[2];
    bool tip_valid[2] = {};
};

// ui_scene.cpp: the glue to the sandbox scene, and the offline previews.

/// The menus' input from the scene's (same world space) plus the sticks of BbXrInput (0..255,
/// y down) and the raw buttons (before any filtering).
UiInput UiInputOf(const SceneInput& input, uint32_t buttons, uint8_t lx, uint8_t ly, uint8_t rx,
                  uint8_t ry, float floor_y);
/// The body's yaw and the fingertips of the posed hunter (else of the aim poses).
UiBody UiBodyOf(const Scene& scene, const UiInput& input);

struct UiHaptic {
    int hand;
    float amplitude, seconds;
};

/// A choice the user made in a menu. The sandbox carries it out at once (counts, the held weapon);
/// the game gets it through the bridge and makes the game do it with its own buttons
/// (src/runtime_vr_ui.c).
struct UiAction {
    std::string menu;     // the menu's name
    std::string item;     // the item as listed (a name, "goods:1000", "weapon:7000000")
    std::string action;   // use | equip
    std::string source;   // the live list it came from ("quick", "inventory", "weapons") or ""
    int index = -1;       // its index in that list (the quick slot for "quick"), else in the menu
    int hand = 1;         // the hand that chose it
    int side = -1;        // equip: the hand the weapon goes to (0 left, 1 right)
};

struct UiEvents {
    std::vector<UiHaptic> haptics;
    std::vector<UiAction> actions;
    std::string log;
};

/// What the menus read from and change in the app around them: the sandbox's Config, or the
/// game's state (through the bridge).
struct UiWorld {
    float eye_height_m = 1.65f;
    std::string weapon[2];   // held now: left, right (names, models or "weapon:<id>")
    bool show_weapon = true; // the held weapons are drawn (hide_weapons menus clear it)
};

/// The game's live lists (the game mode): the menus' "@quick", "@inventory", "@weapons" items and
/// every count come from here, nothing is counted down by the menus themselves.
struct UiLiveList {
    std::vector<std::string> items; // atlas keys ("goods:1000"), "" for an empty slot
};
struct UiLiveData {
    std::vector<UiInventoryEntry> inventory;      // every item the player carries, with counts
    std::map<std::string, UiLiveList> lists;      // "quick", "weapons", ...
    std::map<std::string, int> selected;          // "quick": the current slot; "weapons_right",
                                                  // "weapons_left": the held slot (0..3)
    /// Dashboard actions the app cannot do (shown disabled with the reason): "use_inventory" (an
    /// item in no quick slot), "assign_quick" (put an item into a quick slot), "equip_inventory"
    /// (a weapon from the inventory into a hand slot), "clear_quick" (empty a quick slot), or a
    /// button's action.
    std::map<std::string, std::string> disabled;
    /// The co-op room line for the dashboard title ("Room K7Q-M4X  2/3"), "" in a solo game. Read only.
    std::string room;
};

class UiSystem {
public:
    UiSystem();
    ~UiSystem();

    /// Loads <assets>/ui (icons, font) and <assets>/ui_layouts.json (written with the defaults
    /// when missing), creates the GPU side and hooks it into the renderer (none: logic only).
    bool Load(const std::filesystem::path& assets, Renderer* renderer, std::string& log,
              const std::string& log_prefix = "ui");
    /// The game's VR UI (the bridge): the same, with the "game" menus of the layout file and the
    /// live data (SetLiveData). With a device the GPU side is created on it, drawing into a
    /// transparent layer (coverage alpha); the caller records it through Gpu().
    bool LoadGame(const std::filesystem::path& assets, const Renderer::Device* device,
                  std::string& log);
    /// The GPU side to call Draw on inside a pass (null without one).
    DrawHook* Gpu();
    /// Writes the per-preset summary to the log, unhooks and frees the GPU side.
    void Destroy(Renderer* renderer);

    /// The buttons the scene may see this frame: a binding's key the menus take (its press came
    /// with the modifiers held, or a menu is using it) stays hidden until released.
    uint32_t FilterButtons(const UiInput& input);
    void Update(const UiInput& input, const UiBody& body, Config& config, UiEvents& events);
    void Update(const UiInput& input, const UiBody& body, UiWorld& world, UiEvents& events);
    /// Laser lines into `data`; the menus' triangles to the GPU (drawn by the hook).
    void Build(FrameData& data);
    /// The same without a line pipeline: the lasers become thin quads (the game's layer).
    void Build();
    /// The triangles of the last Build (CPU).
    const UiCanvas& Canvas() const;
    /// The last Build drew anything.
    bool Visible() const;

    /// The game mode: the live lists and counts (replaces them; any thread that also calls
    /// Update, never concurrently).
    void SetLiveData(const UiLiveData& data);
    /// The menus have the hands: a radial or grid is open, a hand is on a body slot, a pointer or a
    /// fingertip is on a panel. Swings must not attack and the game must not get the buttons.
    bool Engaged() const;
    /// An open menu uses this hand's stick (0 left, 1 right): keep it from the camera/movement.
    bool UsesStick(int hand) const;
    /// Closes every open radial and grid (a cancel).
    void CloseAll();
    /// A head-locked message for `seconds` (the toast): what the app did or why it could not.
    void ShowMessage(const std::string& title, const std::string& body, double seconds = 4.0);

    /// The dashboard's element ids (tests): weapon slot k (0 R1, 1 R2, 2 L1, 3 L2), quick slot k,
    /// button i, inventory card i (of the shown list).
    static int DashWeapon(int k) { return 1000 + k; }
    static int DashQuick(int k) { return 1100 + k; }
    static int DashButton(int i) { return 1300 + i; }
    static int DashItem(int i) { return 2000 + i; }
    /// The title's "Remove from wheel" toggle: the next quick item you point at is taken out.
    static int DashRemove() { return 1404; }
    /// Where element `id` of a panel menu was drawn by the last Build (world), false if not.
    bool ElementCenter(const std::string& menu, int id, m3::Vec3& world) const;
    /// The dashboard's drag and drop (ui_drag.h): an item is being dragged, and which (item key), with what hand.
    bool Dragging() const;
    std::string DragItem() const;
    int DragHand() const;

    /// One line per preset and its bindings, for the console.
    std::string Controls() const;
    bool SetPreset(const std::string& name);
    std::string ActivePreset() const;
    const UiLayout& Layout() const;
    /// An open radial or grid uses the sticks (selection, pages): keep them from locomotion.
    bool UsesSticks() const;
    /// The count left of an inventory item (-1: not counted).
    int Count(const std::string& item) const;
    /// The menu's hovered element (-1 none) and whether it is open (tests, previews).
    int Hovered(const std::string& menu) const;
    bool IsOpen(const std::string& menu) const;

    struct Impl;

private:
    void BuildInto(FrameData* data);
    std::unique_ptr<Impl> impl;
};

/// Where a grid menu opens for this head pose (in front, below the eyes, tilted toward them).
UiPanel UiGridPanel(const UiMenuSpec& spec, const UiPose& head);
/// A grid panel's size, and the center of card k of a page, in panel coordinates (as drawn).
void UiGridSize(const UiMenuSpec& spec, float& width, float& height);
void UiGridCard(const UiMenuSpec& spec, int k, float& u, float& v);
/// The center of the grid's tab i (false without tabs).
bool UiGridTab(const UiMenuSpec& spec, int i, float& u, float& v);

/// Offline renders of the menu presets: scripted input through each preset,
/// <out_dir>/ui_<scenario>.png (first person | outside). Returns 0 when all were written.
int RenderUiPreviews(const std::filesystem::path& assets, Renderer& renderer, Scene& scene,
                     const Config& config, const std::filesystem::path& out_dir, uint32_t size);

} // namespace sb
