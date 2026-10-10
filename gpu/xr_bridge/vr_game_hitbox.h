// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The game's hitbox debug overlay (docs/vr/findings/29-game-hitboxes.md): the VR sandbox's hitbox
// view (sandbox3d/scene.cpp "show": hitboxes, hurtboxes_self) drawn over the game in the VR UI
// layer, with the sandbox's data (out/vr_assets/hitbox, bbhitbox) and its wireframe and colours
// (sandbox3d/wire_shapes.h):
//   - the held weapons' hit capsules on the game's L_Weapon / R_Weapon bones (every attack's
//     shapes, each once, at idle_alpha), the current attack's (the stamina ticket's AtkParam id,
//     BBXR_EV_SWING_OPEN) brighter and thicker, white for a moment on its hit;
//   - the player's own hurtboxes (chr_c0000, skinned on the game's bones; off by default);
//   - markers where the player's hits landed (the stamina hit hooks' contact point, 3 s).
// The host (src/runtime_vr_hitbox.c) sends the pose as matrices in the headset's space
// (BbXrGameDebug in BbXrGameState); the lines are thin quads facing the viewer, on the UI
// layer's canvas. The "game" object's "debug_hitboxes" says what is drawn (hot reload).

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "bbport_xr_bridge.h"
#include "sandbox3d/math3d.h"
#include "sandbox3d/model.h"
#include "sandbox3d/ui_layout.h"
#include "sandbox3d/ui_render.h"

namespace vrui {

/// One line of the overlay (headset space).
struct HitboxLine {
    m3::Vec3 a, b;
    sb::UiColor color;
    float width_scale = 1.0f; // times the configured width
};

constexpr double kHitboxFlashS = 0.25;  // the attack's shapes stay white this long after a hit
constexpr double kHitboxAttackS = 2.0;  // an attack without its ticket's end is over after this

class GameHitboxes {
public:
    /// <assets>/hitbox/index.json and chr_c0000.json (the weapons load on first use).
    void Load(const std::filesystem::path& assets, std::string& log);
    /// The stamina tickets: an attack opens (its AtkParam id), hits, or misses.
    void OnEvent(const BbXrGameEvent& e, double now);
    /// This frame's lines (pure: no GPU). Empty without a pose or with everything off.
    void Lines(const sb::UiDebugHitboxes& cfg, const BbXrGameState& s, double now,
               std::vector<HitboxLine>& out, std::string& log);
    /// Lines() drawn as thin quads facing `eye` into the canvas.
    void Draw(sb::UiCanvas& canvas, const sb::UiDebugHitboxes& cfg, const BbXrGameState& s,
              double now, m3::Vec3 eye, std::string& log);

    /// The blade of the weapon held in `side` (0 left, 1 right) as a segment in the headset's space:
    /// the weapon bone's origin to the farthest hit-shape point of the weapon's hitboxes. `approx`
    /// is set (base == tip == the bone) when the weapon has no hitbox data. False without a pose.
    bool BladeSegment(const BbXrGameState& s, int side, m3::Vec3* base, m3::Vec3* tip, bool* approx,
                      std::string& log);

    /// The index entry for a game weapon id (upgrades and variants fall back to the base row);
    /// -1 when the data has none.
    int IndexIdFor(uint32_t game_id) const;
    int32_t AttackAtk() const { return attack_atk; }

private:
    struct Weapon {
        bool tried = false;
        sb::Hitboxes boxes;
        bool tip_tried = false, tip_ok = false;
        m3::Vec3 tip; // the farthest weapon-space point of any hit shape (the blade's end)
    };
    const sb::Hitboxes* WeaponBoxes(uint32_t game_id, std::string& log);

    std::filesystem::path assets;
    std::map<int, std::string> models; // index id -> weapon model name ("wp_a_0800")
    std::map<int, Weapon> weapons;     // index id -> its hitboxes
    std::vector<sb::Hurtbox> self_boxes;
    bool enemies_logged = false;
    // The attack in progress (a stamina ticket).
    int32_t attack_atk = -1, attack_ticket = 0;
    double attack_at = -1e9, flash_at = -1e9;
};

} // namespace vrui
