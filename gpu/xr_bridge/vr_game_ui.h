// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The game's VR UI inside bbport_xr.dll (BB_VR_UI=1, ABI 8; docs/vr/findings/26-game-ui.md): the
// sandbox's menus (sandbox3d/ui.h: the quick ring, the inventory grid, wrist counters, body slots)
// and its swing/stamina HUD (sandbox3d/combat_hud.h), the same code, fed with the game's real
// state instead of the sandbox's scripted inventory:
//   - the host (src/runtime_vr_ui.c) sends the player's stamina, quick slots, items and weapon
//     slots each pad poll (SetGameState) and the "misses are free" stamina tickets as they open
//     and close (PushEvent, src/runtime_vr_stamina.c);
//   - every headset frame the UI thread feeds the menus the controllers (in the bridge's world
//     space, at the display time it renders for), builds their triangles and the HUD's, and the
//     bridge draws them into a transparent stereo layer over the game (bbport_xr_bridge.cpp);
//   - what the user picks becomes an action (TakeActions) the host carries out with the game's
//     own buttons; nothing here writes game memory;
//   - vr_ui_filter.h keeps the menus' presses from the game (R3 tap stays lock-on, R3 held opens
//     the inventory; a standing L3 click opens the quick ring; nothing reaches the game while
//     the menus have the hands).
// Swing labels: a swing gesture (xr_input.cpp's R1/R2 pulse) shows LIGHT or HEAVY with a weak
// tick; the stamina ticket that opens for it gives its cost ("-20 if it hits"), the ticket's end
// gives "HIT -20" (a pulse) or "MISS  free". At 0 stamina a swing shows NO STAMINA (two buzzes).
// CPU only except Load's GPU objects and Draw; unit tested without a GPU (vr_game_ui_test.cpp).

#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "bbport_xr_bridge.h"
#include "sandbox3d/combat_hud.h"
#include "sandbox3d/ui.h"
#include "vr_ui_filter.h"
#include "vr_game_hitbox.h"
#include "vr_held_items.h"

namespace vrui {

/// One headset frame's controllers, in the bridge's world space (LOCAL or STAGE).
struct FrameInput {
    double time = 0.0;             // seconds, steady
    bool stage = false;            // STAGE: the floor is y = 0
    sb::UiPose head, grip[2], aim[2];
    float trigger[2] = {}, squeeze[2] = {};
    BbXrInput pad{};               // xr_input's mapped state (buttons, sticks, hand speeds)
    double ms_since_swing = 1e9;   // since xr_input last fired a right-hand swing gesture
};

struct Haptic {
    int hand;
    float amplitude, seconds;
};

/// The item key the menus use for a game item id ("goods:1000", "weapon:7000000"), "" for none.
std::string ItemKey(uint32_t id);

/// The debug gun ray (the left controller's aim ray) for one frame.
struct GunRayLook {
    bool visible = false; // enabled in "game", in the world, the left aim pose valid
    bool flash = false;   // within kGunRayFlashS of a left-trigger pull: brighter and thicker
    float width_m = 0.0f, alpha = 0.0f;
};
constexpr double kGunRayFlashS = 0.3;
GunRayLook GunRayFor(const sb::UiGameLayout& game, bool in_world, bool aim_valid,
                     double since_pull_s);
/// A world direction as yaw (radians, 0 = -Z, left positive) and pitch (up positive), relative
/// to the head's own yaw/pitch when `head` is given.
void AimAngles(m3::Vec3 dir, const m3::Quat* head, float* yaw, float* pitch);
/// ... and back (0xFFFFFFFF when it is not one).
uint32_t ItemId(const std::string& key);

class GameUi {
public:
    GameUi();
    ~GameUi();

    /// <assets>/ui (icons, font), <assets>/ui_layouts.json (its "game" object, added with the
    /// defaults when the file has none). With a device the GPU side is created on it. False when
    /// nothing can be shown (the log says why).
    bool Load(const std::filesystem::path& assets, const sb::Renderer::Device* device,
              std::string& log);
    void Destroy();

    // Any thread.
    void SetGameState(const BbXrGameState& state, double now);
    void PushEvent(const BbXrGameEvent& event, double now);
    uint32_t TakeActions(BbXrUiAction* out, uint32_t capacity);
    InputFilter& Filter() { return filter; }
    /// What the dashboard's Recenter button calls (the bridge: XrInput::RequestRecenter).
    void SetRecenter(void (*fn)()) { recenter = fn; }

    // The UI thread.
    /// One frame: the menus and the HUD state; haptics and log lines out.
    void Update(const FrameInput& in, std::vector<Haptic>& haptics, std::string& log);
    /// This frame's triangles (to the GPU when there is one). False when nothing is to be drawn.
    /// The GPU must be done with the previous frame's.
    bool Build();
    /// Records the menus and the HUD into the current pass (dynamic rendering, no depth).
    void Draw(VkCommandBuffer cmd, const m3::Mat4& view_proj, VkFormat color);

    // Tests and logs.
    const held::HeldItem& HeldItemState() const { return held_item; }
    const sb::SwingReadout& Readout() const { return readout; }
    sb::UiSystem& Menus() { return menus; }
    bool Active() const { return active; }
    bool HudVisible() const { return hud_visible; }
    size_t HudVertices() const { return hud_canvas.world.size() + hud_canvas.top.size(); }

private:
    struct Ticket {
        int32_t id = 0, cost = 0;
        double t = 0.0;
    };

    void ApplyState(const BbXrGameState& s, std::string& log);
    /// A weapon-contact pulse unless the hand had one in the last 80 ms; false when skipped.
    bool PulseWeapon(int hand, int contact, int paid, double now, std::vector<Haptic>& haptics, std::string& log);
    void Swings(const FrameInput& in, const BbXrGameState& s, std::vector<Haptic>& haptics);
    void Events(std::vector<Haptic>& haptics, std::string& log);
    void Act(const sb::UiAction& a, std::string& log);
    /// Something the user asked for that the game will not get: said in the headset and logged.
    void Refuse(const std::string& what, const std::string& why, std::string& log);
    void DrawGunRay(const sb::UiGameLayout& g);
    void DrawHeldItem(const sb::UiGameLayout& g);
    /// Physical items (vr_held_items.h): a picked consumable goes into a hand instead of being used.
    bool StartHeld(const sb::UiAction& a, int slot, uint32_t id, std::string& log);
    void StepHeld(const FrameInput& in, std::vector<Haptic>& haptics, std::string& log);
    void FireHeld(const held::Result& r, const FrameInput& in, std::string& log);
    void AddTired(std::vector<Haptic>& haptics);

    double last_contact_pulse[2] = {-1.0e9, -1.0e9}; // per hand, the last weapon-contact pulse
    std::filesystem::path assets;
    sb::UiSystem menus;
    bool loaded = false;

    // The HUD (combat_hud.h) with its own font and GPU side.
    sb::UiFont hud_font;
    sb::UiRenderer hud_gpu;
    bool hud_gpu_ready = false;
    sb::UiCanvas hud_canvas{nullptr, &hud_font};
    sb::SwingReadout readout;
    bool hud_visible = false;
    double last_activity = -1e9;

    InputFilter filter;
    GameHitboxes hitboxes;  // the hitbox debug overlay (vr_game_hitbox.h)
    std::string hitbox_log; // its lines from Build(), logged by the next Update()

    // Shared with the host's threads.
    std::mutex mutex;
    BbXrGameState state{};
    uint64_t state_seq = 0;
    double state_at = -1e9;
    std::deque<BbXrGameEvent> events;
    std::vector<BbXrUiAction> actions;

    // The UI thread's copy.
    BbXrGameState current{};
    uint64_t applied_seq = 0;
    bool active = false;       // in the world, with fresh state
    sb::UiWorld world;
    double body_yaw = 0.0;
    bool body_yaw_valid = false;
    uint32_t prev_buttons = 0;
    bool l3_consumed = false;
    double speed_window[24] = {}; // recent right-hand speeds (peak of a swing)
    double speed_times[24] = {};
    int speed_next = 0;
    std::deque<Ticket> tickets;
    double tired_second = -1.0; // the second buzz of "tired"
    int tired_hand = 1;
    double now = 0.0;
    sb::UiPose head_pose, left_grip; // this frame's, for the HUD
    sb::UiPose left_aim;             // the gun ray's origin
    double last_pull = -1e9;         // the last left-trigger (L2) pull
    double last_pull_log = -1e9;
    bool ray_in_world = false;    void (*recenter)() = nullptr;
    // The item in a hand (physical items).
    held::HeldItem held_item;
    double held_menu_ignore_until = -1.0; // the menu that took the pick is closing
    double held_fired_at = -1e9;
    // The action the host is carrying out (shown until it is done, gave up, or 8 s passed).
    std::string pending;
    double pending_at = -1.0;
};

} // namespace vrui
