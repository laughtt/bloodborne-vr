// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The live-tunable settings of sandbox3d's solid weapons (physics_weapon.h): the "physics" object
// of out/vr_assets/sandbox3d.json, hot reloaded with the rest of the config (config.h). Missing
// keys keep these defaults.

#pragma once

#include <string>

#include "../../../src/vr_hit_scale.h"
#include "json.h"
#include "math3d.h"

namespace sb {

struct PhysicsConfig {
    bool enabled = true;         // weapons are solid: they stop at colliders and push each other
    bool weapons_collide = true; // the left weapon against the right one
    bool floor = true;           // the floor stops weapons
    bool enemy_solid = true;     // the enemy's hurtboxes stop weapons (hits register either way)
    bool show_colliders = true;  // collision capsules, boxes and the enemy's hurtboxes as lines
    bool show_ghost = true;      // the real controller hand when a blocked weapon holds it back
    float ghost_min_m = 0.03f;   // ... from this far

    // Solver: each frame the held weapon moves toward where the hand puts it in substeps no
    // longer than max_step, and is pushed out of what it overlaps (position-based rigid body:
    // the hand's mass at the grip and the weapon's along its capsules decide how much it turns
    // and how much it slides). The rig then puts the hand on the weapon.
    float stiffness = 1.0f;         // share of the weapon->hand gap closed per 1/90 s (1: rigid)
    int iterations = 4;             // push-out passes per substep
    int max_substeps = 96;
    float max_step_m = 0.0f;        // per-substep travel of any weapon point; 0: 0.8 x the
                                    // thinnest capsule radius (no tunnelling)
    float max_separation_m = 0.45f; // the hand this far from its blocked weapon lets it go
                                    // (it snaps back into the hand and passes through)
    float max_separation_arc_m = 1.5f; // ... or the weapon turned so far from the hand that its
                                       // farthest point (reach) is this far along the arc
                                       // (angle x reach: a long blade lets go at a smaller angle)
    // The raw controller (not the IK-clamped hand) jumping this far in one frame, or turning
    // this much: tracking glitch, reset. Same distance as swing_logic.h's glitch_m.
    float teleport_m = 0.3f, teleport_deg = 90.0f;
    bool swing_through_enemy = true; // a swing in progress (hit_speed met) passes through the
                                     // enemy's hurtboxes (its hits still register); the post and
                                     // the wall stay solid
    float hand_mass_kg = 2.5f, weapon_mass_kg = 1.5f;
    float hand_gyration_m = 0.25f; // the hand resists turning like a mass this far out (the
                                   // forearm): a knock on the blade's side cannot spin the grip

    // Collision shapes from the weapon mesh: each bone's vertices sliced along their long axis;
    // a slice is one capsule, or up to max_lanes side by side when it is flat (a blade).
    float slice_m = 0.10f;
    float radius_min_m = 0.012f, radius_max_m = 0.05f;
    float radius_scale = 1.0f;
    int max_lanes = 4;

    // Haptics (amplitude 0..1, seconds).
    float impact_min_speed = 0.03f;          // m/s along the normal for a contact pulse
    float impact_amplitude_min = 0.12f;      // + per m/s, up to max
    float impact_amplitude_per_mps = 0.30f;
    float impact_amplitude_max = 1.0f;
    float impact_seconds = 0.03f, impact_seconds_per_mps = 0.01f, impact_seconds_max = 0.08f;
    float slide_min_speed = 0.08f;           // m/s along the surface for the grinding buzz
    float slide_amplitude_min = 0.06f, slide_amplitude_per_mps = 0.15f;
    float slide_amplitude_max = 0.35f;
    float buzz_interval_s = 0.05f, buzz_seconds = 0.07f;
    float release_s = 0.08f; // a contact ends after this long apart (no flicker)

    // Melee hits: the shown attack's hit capsules (doc 14) against the enemy's hurtboxes.
    bool hits = true;
    float hit_speed = 2.0f;            // m/s of the hit shape (xr_input's R1 swing threshold)
    float rearm_speed = 1.0f;          // the swing ends below this; the next one can hit again
    float rearm_reverse_deg = 120.0f;  // ... or the swing direction turns by more than this
                                       // (forehand then backhand through the enemy): 0 = off
    float attack_radius_scale = 0.6f;  // doc 14 4: 0.4 m radii are too big for a tracked blade
    float hit_amplitude = 1.0f, hit_seconds = 0.10f;
    // The VR hit scale (src/vr_hit_scale.h, findings/32): the game's attack capsules, thicker and
    // longer, drawn and tested the same; the game's hook applies the same math from out/fp.txt.
    VhsParams hit = vhs_defaults();
    float marker_s = 0.8f;             // the hit marker fades over this long

    // Props, placed relative to where you stood and looked at the start (and at each T-pose
    // calibration): [x, z] in meters, x to the right, -z ahead.
    bool props = true;                 // post, wall and enemy: drawn and solid
    float post[2] = {0.8f, -0.8f};
    float post_radius = 0.07f, post_height = 1.8f;
    float crossbar_height = 1.3f, crossbar_length = 0.8f, crossbar_radius = 0.035f;
    float wall[2] = {1.75f, -0.7f};    // the middle of the face toward you
    m3::Vec3 wall_size{0.25f, 2.4f, 2.6f}; // thickness, height, length
    float wall_yaw_deg = 0.0f;         // 0: the face looks at -x (toward you)
    std::string enemy = "c2632";       // enemy_<chr>.json (extract_enemy.py) + its hurtboxes
    float enemy_pos[2] = {-1.1f, -1.5f};
    bool enemy_face_user = true;       // else enemy_yaw_deg (0: faces -z)
    float enemy_yaw_deg = 0.0f;

    // Logs: every contact start/end, hit and snap, printed and appended as JSON lines.
    bool log_contacts = true;
    std::string contact_log = "sandbox3d_contacts.jsonl"; // relative to the working directory

    void Load(const json::Value& doc);
    /// The settings as a JSON object (lines indented by `indent`).
    std::string Summary(const std::string& indent = "  ") const;
};

} // namespace sb
