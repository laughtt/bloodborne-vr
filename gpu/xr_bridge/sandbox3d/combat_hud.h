// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The swing and stamina HUD as a pure canvas builder, shared by sandbox3d (combat_view.h, fed by
// combat.h) and the game's VR UI in the bridge (../vr_game_ui.h, fed by the game's real stamina
// and the "misses are free" tickets of src/runtime_vr_stamina.c):
//   - head-locked, below the view: the stamina bar over the game's whole range, floor -50 to max
//     (green above 0, red below; a white ghost for the cost the open swings hold; grey while regen
//     waits; an orange flash where a cost was taken; a red blink when something was refused), the
//     last swing's label and result ("HEAVY  HIT -35", "LIGHT  MISS  free", "NO STAMINA"), a
//     detail line and (optionally) the live speed on a scale with the classifier's thresholds;
//   - above the left wrist, facing you: the bar and "SP 71 / 91";
//   - where a swing connected: its cost, floating up.
// Only UiCanvas (ui_render.h) and the readout: no scene, no physics, no GPU.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"
#include "swing_logic.h"
#include "ui_render.h"

namespace sb {

/// What the HUD shows.
struct SwingReadout {
    enum class Label : uint8_t {
        None,
        Light,    // R1
        Heavy,    // R2 tap (also a charge released early)
        Charging, // R2 held
        Charged,  // the charged R2
        Windup,   // a rising stroke: not an attack
        TooSlow,  // a stroke that did not reach light speed (or was too short)
        Tired,    // a swing that did not register: no stamina
        Blocked,  // a menu has the controls
        Dash,     // a dodge, backstep or jump paid
        DashTired // ... refused: no stamina
    };
    enum class Result : uint8_t { None, Pending, Hit, Clang, Miss };

    bool enabled = true;
    // Stamina.
    float stamina = 0.0f, max = 1.0f, floor = -50.0f, pending = 0.0f;
    bool can_act = true, regen_paused = false;
    double regen_at = 0.0;
    double charged_at = -1e9;    // the last cost taken (the bar flashes)
    float charged_from = 0.0f, charged_to = 0.0f;
    double refused_at = -1e9;    // the last refusal (the bar blinks)
    // The classifier, live.
    swing::SwingClassifier::Phase phase = swing::SwingClassifier::Phase::Armed;
    float speed = 0.0f, stroke_peak = 0.0f;
    float onset = 1.3f, light = 2.2f, heavy = 4.6f; // the thresholds in use
    bool charging = false;
    double charge_since = 0.0, charge_full_s = 0.8;
    // The last label.
    Label label = Label::None;
    Result result = Result::None;
    double label_t = -1e9;
    uint32_t swing_no = 0;
    float label_speed = 0.0f;    // the stroke's peak (m/s)
    float label_cost = 0.0f;     // the attack's or the move's cost (0: not known yet)
    float label_paid = 0.0f;     // what the hit took
    float latency_ms = 0.0f;     // decided after the onset (< 0: not known, not shown)
    std::string move;            // Dash labels: dodge, backstep, jump
    std::string detail;          // replaces the label's detail line when set (the game's)
    // The costs, floating up from where swings connected.
    struct Popup {
        m3::Vec3 point;
        double t = 0.0;
        float amount = 0.0f;
        bool world = false;
    };
    std::vector<Popup> popups;
    double now = 0.0;
    std::string weapon, weapon_class;
    std::string form = "A"; // the trick weapon form shown
    bool two_forms = false; // the weapon has a form B
    double form_t = -1e9;   // the last transform (the HUD flashes the form)
};

/// Where and how the HUD is drawn (the sandbox fills it from its "swing" config, the game from the
/// "game" object of ui_layouts.json).
struct SwingHudConfig {
    bool head = true;          // head-locked, below the view
    bool wrist = true;         // above the left wrist, facing you
    float distance = 0.9f, down_deg = 24.0f, scale = 1.0f;
    float wrist_scale = 0.7f;
    m3::Vec3 wrist_offset{0.0f, 0.08f, 0.0f}; // from the left grip, world axes
    double label_s = 1.6;      // a label fades out after this long
    float speed_max = 8.0f;    // the speed scale's end (m/s)
    bool speed = true;         // the live speed scale under the bar
    bool popups = true;        // the cost floats up from where the swing connected
};

/// The HUD's triangles for this frame into `canvas` (CPU only). `head`, `left_grip`: world poses
/// as rendered.
void DrawSwingHud(UiCanvas& canvas, const SwingReadout& readout, const SwingHudConfig& config,
                  const UiPose& head, const UiPose& left_grip);

/// The label's words, for logs and tests: "HEAVY", "HIT  -35".
std::string SwingLabelText(const SwingReadout& readout);

} // namespace sb
