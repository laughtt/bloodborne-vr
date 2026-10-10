// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Swing classification and stamina in sandbox3d: phases S1-S2 of
// docs/vr/findings/20-swing-and-stamina.md, on the pure logic the game will share
// (../swing_logic.h):
//   - the right controller's real swing (tracking space: locomotion, snap turns and the dash do not
//     move it; the head compensates walking, leaning and body turns) -> SwingClassifier: a slower
//     swing is a light attack (R1), a fast one a heavy attack (R2), a rising stroke is a wind-up
//     (never an attack), the wind-up held still overhead charges R2 and the swing out of it is the
//     charged attack;
//   - every attack holds its cost as a ticket (StaminaModel::BeginSwing) for hit_window_s. The
//     held weapon touching the enemy (the shown attack's hit capsules in its hurtboxes, as the
//     physics' own HIT) pays the cost once; touching the post or the wall pays it too with
//     charge_world (the game's clang). A swing that touches nothing costs nothing: misses are
//     free. Contacts made by a stroke before it is decided count for it (the blade can reach the
//     target before the classifier has seen enough of the swing);
//   - the costs are the game's (BehaviorParam_PC through hitbox/wp_<id>.json: Saw Cleaver R1 20,
//     R2 35, charged 50, form B R1 24); the dodge, backstep and jump pay at once through the
//     locomotion's move gate (TryDash; 20 each until the game's values are measured);
//   - at 0 or below nothing starts (the game's rule; costs may push it to the floor, -50): the swing
//     does not register, the move is refused. Regen 45/s, paused 0.75 s after any action;
//   - feedback: the readout for the HUD (combat_view.h: stamina bar, swing label, speed scale),
//     haptics (a tick when a swing registers, a hit pulse by attack kind, a double buzz when
//     tired, a double click when the weapon transforms), stdout and JSON lines (sandbox3d_swing.jsonl in the working directory: every swing,
//     dropped stroke, hit, miss, refusal, dash and stamina change; with the hand samples of each
//     stroke for offline replay).
//   - the trick-weapon transform (docs/vr/findings/33): a wrist snap of the weapon hand
//     (../trick_transform.h) or the L1 button (the left grip, as in the game) switches form A/B of
//     a weapon that has both (CombatEvents::set_form; the caller sets the scene's form, which swaps
//     the hit shapes). While the wrist snaps the swing classifier is not fed, so a snap is never
//     an attack. Settings: the "transform" object inside "swing".
// Settings: the "swing" object of sandbox3d.json, polled here apart from config.h and written with
// the defaults when missing.

#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "combat_hud.h"
#include "json.h"
#include "math3d.h"
#include "physics_weapon.h"
#include "swing_logic.h"
#include "trick_transform.h"

namespace sb {

struct SceneInput;

/// A haptic pulse: amplitude 0..1 and seconds ([amplitude, seconds] in the config).
struct Pulse {
    float amplitude = 0.0f, seconds = 0.0f;
};

struct CombatConfig {
    bool enabled = true;

    // The classifier (swing::SwingTuning). The class gives the thresholds that differ per moveset
    // (doc 20 4.3); a key below that is not 0 overrides its class value.
    std::string weapon_class = "auto"; // auto (by the right weapon and form) | quick | medium | heavy
    float light_speed = 0.0f, heavy_speed = 0.0f, min_arc = 0.0f;
    double decide_s = 0.0, max_swing_s = 0.0;
    swing::SwingTuning tuning; // every other classifier key (onset_speed, up_limit, charge, ...)

    // Stamina (swing::StaminaTuning: max, floor, regen_per_s, regen_delay_s, hit_window_s,
    // gate_at_zero; its dash_cost is not used, the moves have their own).
    swing::StaminaTuning stamina;
    float light_cost = 0.0f, heavy_cost = 0.0f, charged_cost = 0.0f; // 0: the weapon's
    float dodge_cost = 20.0f, backstep_cost = 20.0f, jump_cost = 20.0f;
    double charge_full_s = 0.8;     // R2 held this long: the release is the charged attack (else
                                    // the R2 tap's cost); a placeholder until measured
    bool charge_world = true;       // the post or the wall pays the swing (the game's clang)
    bool charge_floor = false;      // ... the floor too
    bool infinite = false;          // nothing costs (classifier tuning without the gate)
    float contact_margin_m = 0.01f; // the weapon's own capsules count as touching this close
    double pre_contact_s = 0.15;    // a contact this long before the swing's onset still counts
    bool replace_physics_hit = true; // the physics' hit pulse becomes soft_contact when no attack

    // Haptics (right hand).
    Pulse tick_light{0.2f, 0.025f}, tick_heavy{0.4f, 0.035f}, tick_charge{0.3f, 0.06f};
    Pulse hit_light{0.6f, 0.06f}, hit_heavy{1.0f, 0.12f}, hit_charged{1.0f, 0.2f};
    Pulse tired{0.5f, 0.05f};
    double tired_gap_s = 0.1; // the second buzz of "tired"
    Pulse soft_contact{0.2f, 0.03f};

    // The trick-weapon transform (swing.transform): swing::TransformTuning plus the feedback.
    swing::TransformTuning transform;
    bool transform_button = true;        // the L1 button (left grip) transforms too
    bool transform_gesture = true;       // the wrist snap
    Pulse transform_haptic{0.8f, 0.04f}; // the snap: a click ...
    Pulse transform_lock{0.5f, 0.08f};   // ... and the weapon locking into place
    double transform_gap_s = 0.09;
    Pulse transform_none{0.2f, 0.03f};   // the weapon has no other form

    // HUD (combat_view.h).
    bool hud_head = true;          // head-locked, below the view
    bool hud_wrist = true;         // above the left wrist, facing you
    float hud_distance = 0.9f, hud_down_deg = 24.0f, hud_scale = 1.0f;
    float wrist_scale = 0.7f;
    m3::Vec3 wrist_offset{0.0f, 0.08f, 0.0f}; // from the left grip, world axes
    double label_s = 1.6;          // a label fades out after this long
    float speed_max = 8.0f;        // the speed scale's end (m/s)
    bool popups = true;            // the cost floats up from where the swing connected

    // Logs.
    bool log = true;               // stdout
    bool log_samples = true;       // the hand samples of each stroke in the JSON lines
    std::string log_file = "sandbox3d_swing.jsonl"; // relative to the working directory; "" off

    void Load(const json::Value& v);
    std::string Summary() const; // the "swing" object as JSON
};

/// Polls the "swing" object of sandbox3d.json (twice a second); adds it with the defaults when the
/// file has none.
class CombatConfigFile {
public:
    explicit CombatConfigFile(std::filesystem::path path) : path(std::move(path)) {}
    bool Poll(double now, CombatConfig& config, std::string& log);

private:
    std::filesystem::path path;
    std::filesystem::file_time_type stamp{};
    double next = 0.0;
    bool loaded = false, inserted = false;
};

/// The right weapon as the swing logic sees it: its class and the costs of its attacks.
struct SwingWeapon {
    std::string name, form, weapon_class = "medium";
    int id = 0;
    bool two_forms = false; // has a form B (the hitbox attacks name one; else a known trick weapon)
    float cost[3] = {20.0f, 35.0f, 50.0f}; // light (R1 #1), heavy (R2 tap), charged R2
    std::string source = "defaults";
};

/// Whether a weapon (by name) is a trick weapon with two forms, when no hitbox data says.
bool KnownTrickWeapon(const std::string& name);
/// The moveset class of a weapon form (doc 20 4.3); medium when unknown.
std::string WeaponClassOf(const std::string& name, const std::string& form);
/// The class and the costs (BehaviorParam_PC stamina of judge 0 / 150 / 170, form B 400 / 550 /
/// 570, from <assets>/hitbox/wp_<id>.json via assets.json; else 20 / 35 / 50).
SwingWeapon LookUpSwingWeapon(const std::filesystem::path& assets, const std::string& name,
                              const std::string& form);
/// The classifier's tuning for a weapon: its class preset, then the config's overrides.
swing::SwingTuning SwingTuningFor(const CombatConfig& config, const std::string& weapon_class);

/// What the held weapon touches now (after the physics step).
struct SwingContact {
    bool enemy = false; // the attack's hit capsules (else the weapon's own) in a hurtbox
    int part = -1;
    std::string target, part_name; // the enemy (c2632) and the hurtbox's name, for the logs
    m3::Vec3 enemy_point;
    bool world[kEntityCount] = {}; // the weapon's own capsules at the floor, post, wall
    m3::Vec3 world_point[kEntityCount];
    bool physics_hit = false; // the physics registered its own HIT this frame (its speed rule)
    bool swinging = false;    // the weapon is driven at hit_speed or more (the physics' rule)
};

/// The contacts of a hand's weapon from the physics' public state (no physics change).
SwingContact DetectSwingContacts(const WeaponPhysics& physics, int side, float margin);

struct CombatHaptic {
    int hand = 1;
    float amplitude = 0.0f, seconds = 0.0f;
};

struct CombatEvents {
    std::string log;
    std::string set_form; // "A" or "B": the weapon transformed, the caller sets the scene's form
    std::vector<CombatHaptic> haptics;
};

class Combat {
public:
    struct Counts {
        int light = 0, heavy = 0, charged = 0, charge_starts = 0;
        int windups = 0, too_slow = 0, tired = 0, blocked = 0;
        int transforms = 0, transforms_button = 0, transforms_none = 0;
        int hits = 0, clangs = 0, misses = 0, dashes = 0, dashes_refused = 0;
    };

    /// Polls the "swing" object of `config_path`; the weapons' costs come from the folder it is in.
    explicit Combat(const std::filesystem::path& config_path);
    /// A fixed config (tests, previews); `assets` for the weapons' costs ("" : 20/35/50).
    Combat(const CombatConfig& config, const std::filesystem::path& assets);
    ~Combat();
    Combat(const Combat&) = delete;
    Combat& operator=(const Combat&) = delete;

    /// One frame, after the scene's physics step. `tracking`: the scene's input before the
    /// locomotion moved it into the world (the real hands and head); `physics`: the held weapons
    /// and what they touch; `form`: the trick weapon form shown; `blocked`: a menu has the
    /// controls (swings are dropped).
    void Frame(const SceneInput& tracking, const WeaponPhysics& physics, const std::string& form,
               bool blocked, CombatEvents& out);
    /// The same from a prepared hand sample and contacts (tests).
    void Update(const swing::HandSample& hand, const SwingContact& contact,
                const std::string& weapon, const std::string& form, bool blocked,
                CombatEvents& out);
    /// The locomotion's move gate: a dodge (0), backstep (1) or jump (2) at `now` pays its cost;
    /// false at 0 or below (nothing moves). Its log and buzz come with the next Frame.
    bool TryDash(int kind, double now);
    /// The L1 button (the left grip) was pressed: the transform, applied by the next Update.
    void PressTransform() { button_transform = true; }
    /// Replaces the config (tests; the file-polling one reloads its own).
    void SetConfig(const CombatConfig& config);

    const CombatConfig& Config() const { return config; }
    const SwingReadout& Readout() const { return readout; }
    const swing::StaminaModel& Stamina() const { return stamina; }
    const swing::SwingClassifier& Classifier() const { return classifier; }
    const SwingWeapon& Weapon() const { return weapon; }
    const Counts& Count() const { return counts; }
    const swing::TrickTransformDetector& Transformer() const { return transformer; }

private:
    struct Ticket {
        uint32_t id = 0, no = 0;
        SwingReadout::Label label = SwingReadout::Label::None;
        double t = 0.0, onset_t = 0.0;
        float cost = 0.0f, peak = 0.0f;
        bool done = false;
    };
    struct ContactSample {
        double t = 0.0;
        bool enemy = false; // else the world
        int what = -1;      // the hurtbox, or the entity
        std::string name;   // for the logs
        m3::Vec3 point;
        bool used = false;  // it paid for a swing
    };
    struct Due {
        double t = 0.0;
        CombatHaptic haptic;
        bool keep = false; // survives ResetSwings (the transform's clicks)
    };

    void Apply(const CombatConfig& next, double now, std::string& log);
    void ChangeWeapon(const std::string& name, const std::string& form, std::string& log);
    /// A new weapon (or hand) starts with no swing: the classifier, a charge, the open tickets
    /// and the remembered contacts are dropped.
    void ResetSwings();
    /// The cost of a light (0), heavy (1) or charged (2) attack of the held weapon.
    float Cost(int k) const;
    /// An attack was decided: refused at 0 or below, else its ticket opens (and takes what the
    /// stroke already touched). `held`: seconds R2 was held (a charge), else < 0.
    void Attack(const swing::SwingEvent& e, SwingReadout::Label label, float cost, bool blocked,
                double t, double held);
    void ChargeStart(bool blocked, double t);
    void ChargeRelease(const swing::SwingEvent& e, bool blocked, double t);
    /// A stroke ended without an attack (its contacts go to the swing still open before it).
    void Dropped(const swing::SwingClassifier::Stroke& d, double t);
    /// Charges ticket `id` (0: the newest open) for contact sample `c` at its time; true if paid.
    bool Connect(uint32_t id, ContactSample& c, double now);
    /// A ticket's window ended without a contact.
    void Missed(uint32_t id, double t);
    Ticket* Find(uint32_t id);
    void SetLabel(SwingReadout::Label label, double t, bool weak = false);
    void Haptic(int priority, const Pulse& p);
    void Tired(double t);
    /// The form switch (a snap or the button): `by` names what asked.
    void Transform(const char* by, const swing::TransformEvent& e, const std::string& form,
                   bool blocked, double t, CombatEvents& out);
    void Print(const std::string& line);
    void Json(const std::string& object);
    std::string Samples(double from) const;

    std::filesystem::path assets;
    std::unique_ptr<CombatConfigFile> file;
    CombatConfig config;
    bool configured = false;
    swing::SwingClassifier classifier;
    swing::StaminaModel stamina;
    swing::TrickTransformDetector transformer;
    bool button_transform = false;
    uint32_t last_buttons = 0;
    SwingWeapon weapon;
    std::string weapon_key = "\x01";
    SwingReadout readout;
    Counts counts;
    std::vector<Ticket> tickets;
    std::deque<ContactSample> contacts;
    std::deque<swing::HandSample> samples;
    std::vector<Due> due;
    uint32_t swing_no = 0, seen_dropped = 0;
    int last_physics_hits = 0;
    bool charging = false, charge_refused = false;
    // Contact edges and the hand that holds the weapon (Frame): a contact pays when it starts
    // during a swing, as the physics' HIT does; a resting weapon pays nothing.
    int side = 1;
    bool side_changed = false;
    bool was_enemy = false, was_world[kEntityCount] = {};
    double charge_t = 0.0, regen_at = -1e9, now = 0.0;
    bool was_full = true, was_can_act = true, was_regen = false, have_time = false;
    // This frame's haptic (the strongest wins), then the queue of pending log lines.
    int haptic_priority = 0;
    Pulse haptic;
    std::string pending_log;
    std::vector<CombatHaptic> pending_haptics;
    std::FILE* log_file = nullptr;
    std::string log_path;
};

} // namespace sb
