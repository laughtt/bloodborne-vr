// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The swing and stamina HUD of sandbox3d (combat.h), drawn with the VR menus' pipeline and the
// game's menu font (ui_render.h; its own canvas and GPU buffers, chained after the menus' draw
// hook):
//   - head-locked, below the view (hud.head): the stamina bar over the game's whole range, floor
//     -50 to max (green above 0, red below; a white ghost for the cost the open swings hold; grey
//     while regen waits; an orange flash where a cost was taken; a red blink when something was
//     refused), the last swing's label and result ("HEAVY  HIT -35", "LIGHT  MISS, free",
//     "NO STAMINA"), its peak speed and detection time, and the live speed on a scale with the
//     onset / light / heavy thresholds;
//   - on the left wrist, facing you (hud.wrist): the bar and "SP 71 / 91";
//   - where a swing connected: its cost, floating up.
// Also the --offline previews combat_*.png (a heavy slash into the enemy, swings at no stamina,
// a charge), through the sandbox's own scene, physics and combat code.

#pragma once

#include <filesystem>
#include <string>

#include "combat.h"
#include "config.h"
#include "renderer.h"
#include "scene.h"
#include "ui_render.h"

namespace sb {

/// The HUD's triangles for this frame into `canvas` (CPU only). `head`, `left_grip`: world poses
/// as rendered.
void DrawCombatHud(UiCanvas& canvas, const SwingReadout& readout, const CombatConfig& config,
                   const TrackedPose& head, const TrackedPose& left_grip);

class CombatView {
public:
    /// Loads the font (<assets>/ui/font.*), creates the GPU side and chains it after the
    /// renderer's current draw hook (the menus'). Without the font only the bars show.
    bool Create(const std::filesystem::path& assets, Renderer& renderer, std::string& log);
    /// Gives the renderer its previous hook back and frees the GPU side.
    void Destroy(Renderer& renderer);
    /// This frame's HUD (world poses, as rendered).
    void Build(const Combat& combat, const TrackedPose& head, const TrackedPose& left_grip);

private:
    struct Chain : DrawHook {
        DrawHook* first = nullptr;
        DrawHook* second = nullptr;
        void Draw(VkCommandBuffer cmd, const m3::Mat4& view_proj, VkFormat color,
                  VkFormat depth) override {
            if (first) {
                first->Draw(cmd, view_proj, color, depth);
            }
            if (second) {
                second->Draw(cmd, view_proj, color, depth);
            }
        }
    };

    UiFont font;
    UiRenderer gpu;
    UiCanvas canvas{nullptr, &font};
    Chain chain;
    bool ready = false;
};

/// The "swing" object of a sandbox3d.json, read only (the previews never write the user's file).
CombatConfig ReadCombatConfig(const std::filesystem::path& config_path);

/// Offline previews through the scene's physics and the combat logic: <out_dir>/combat_*.png
/// (first person with the HUD | outside), their log in combat_swing.jsonl. 0 when all were written.
int RenderCombatPreviews(const std::filesystem::path& assets,
                         const std::filesystem::path& config_path, Renderer& renderer, Scene& scene,
                         const Config& config, const std::filesystem::path& out_dir,
                         uint32_t size);

} // namespace sb
