// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The consumables of the sandbox as real objects (docs/vr/findings/36-vr-consumables.md), pure CPU,
// unit tested by consumables_test.cpp:
//   - the inventory (counts from sandbox3d.json, the sample inventory otherwise);
//   - the wheel: holding the open button (R3) puts every item you have in front of you as a 3D
//     object, a little below eye level, in rows (an arc around you or a flat shelf); letting go
//     closes it after a short linger; empty items are not listed;
//   - taking an item: grip near an object; the item stays in that hand. What happens next depends
//     on the item's gesture:
//       throw  (throwables)    handed to Throwables::TakeFor: the hand-velocity throw of findings/35;
//                              the throw uses one up
//       rub    (weapon buffs)  picked with the hand that does not hold the weapon, rubbed along the
//                              weapon: the paper must stay within contact_radius of the blade and
//                              cover `coverage` of the blade length within window_s; then the weapon
//                              glows for the SpEffect's duration (60 s default) and the paper is used
//       head   (heal, cures, body buffs, bells)  brought within mouth_radius of the mouth (below the
//                              eyes) and held head_hold_s: the effect applies (heal bar, status)
//       crush  (marks, bone blade)  squeezed to crush_squeeze for crush_hold_s
//   - the player state the effects change (HP %, ailments, timed buffs, insight);
//   - every pick, put back, use, rub, buff and expiry goes to the log and a jsonl file.
// Settings: ConsumeConfig (consumables_config.h, the "consumables" object of sandbox3d.json).

#pragma once

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "consumables_config.h"
#include "math3d.h"
#include "throwables.h"

namespace sb {

struct ConsumeInput {
    double time = 0.0;
    bool head_valid = false;
    m3::Vec3 head_position;
    m3::Quat head_rotation;
    bool hand_valid[2] = {};
    m3::Vec3 grip_position[2];
    m3::Quat grip_rotation[2];
    float squeeze[2] = {}, trigger[2] = {};
    uint32_t buttons = 0;
    float floor_y = 0.0f;
    // The weapon each hand holds, as a segment grip -> far end (the blade), world space.
    bool weapon_valid[2] = {};
    m3::Vec3 weapon_base[2], weapon_tip[2];
    // The hand has a weapon equipped (even when it is put away while the wheel is open): the
    // target of an instant weapon buff.
    bool weapon_equipped[2] = {};
};

struct ConsumeEvents {
    std::string log;
    float haptic_amplitude[2] = {}, haptic_seconds[2] = {};
    std::string toast_title, toast_body; // what the app shows the player (empty: nothing)
};

/// A weapon buff on a hand's weapon (a rubbed paper).
struct WeaponBuff {
    bool active = false;
    int goods = 0;
    std::string name, element;
    float color[3] = {1, 1, 1};
    double t0 = 0.0, until = 0.0;
    float duration = 0.0f;
};

/// A timed body status (Beast Blood, Blue Elixir ...).
struct BodyStatus {
    int goods = 0;
    std::string name;
    float color[3] = {1, 1, 1};
    double t0 = 0.0, until = 0.0;
    float duration = 0.0f;
};

class Consumables {
public:
    struct Entry {
        ConsumableDef def;
        int count = 0;
    };
    struct Slot { // an object of the open wheel
        int entry = 0;
        m3::Vec3 pos;
        m3::Quat rot;
        float phase = 0.0f;
    };
    struct Held { // an item in a hand
        int entry = -1;                  // -1: nothing (a thrown item is held by Throwables)
        bool via_throwables = false;
        m3::Vec3 pos;
        m3::Quat rot;
        double since = 0.0;
        double head_since = -1.0, crush_since = -1.0;
        bool blocked = false;            // the use was refused (not enough bullets): leave to retry
        // Rub tracking: (time, position along the blade 0..1) samples while in contact.
        int target = -1;                 // rub: the weapon rubbed (the other hand's)
        float rub_lo = 1.0f, rub_hi = 0.0f;  // rub: share of the weapon covered so far
        double last_diag = -1e9;
        std::vector<std::pair<double, float>> rub;
        double last_contact = -1e9;
        double last_pulse = -1e9;
        float rub_cover = 0.0f;
    };

    /// `assets`: the folder holding consumables/ (meshes) and items/items.json (may not exist).
    void Load(const std::filesystem::path& assets, const ConsumeConfig& config,
              const std::vector<ThrowItemDef>& throw_defs, std::string& log);
    /// Rebuilds the inventory from the config (a config reload that changed it).
    void Reconfigure(const ConsumeConfig& config);
    void Step(const ConsumeInput& in, const ConsumeConfig& config, Throwables& throwables,
              const ThrowConfig& throw_config, const ThrowInput& throw_input,
              ThrowEvents& throw_events, ConsumeEvents& events);

    // ---- State for the renderer and the tests
    const std::vector<Entry>& Entries() const { return entries; }
    const std::vector<Slot>& Slots() const { return slots; }
    const Held& HeldBy(int hand) const { return held[hand]; }
    /// The weapon of `side` is put away (config stow_weapons): the wheel is open and the hand is
    /// free to grab, or the hand holds an item; except the weapon a held paper is meant to be
    /// rubbed along, which stays out. It is back once the item is used / thrown / dropped or the
    /// wheel has closed with nothing held.
    bool WeaponStowed(int side, const ConsumeConfig& config) const;
    /// A hand has an item of ours (or a throwable from the wheel): it holds no weapon.
    bool HandBusy(int hand) const { return held[hand].entry >= 0; }
    bool WheelOpen() const { return open; }
    /// Number of items on the wheel right now (those with a count).
    int ShownCount() const;
    int FindEntry(int goods) const;
    int Count(int goods) const;
    void SetCount(int goods, int count);
    /// Slot i of the open wheel is drawn (an item a hand holds the last of is not).
    bool SlotVisible(int slot) const;
    m3::Vec3 SlotPos(int slot, float bob_m) const;
    m3::Quat SlotRot(int slot) const;
    /// The slot a hand's grab point is in reach of (-1 none).
    int Hover(int hand) const { return hover[hand]; }
    bool CanUse(const ConsumableDef& d) const;
    /// The wheel's text under an item: "x12", "~" (not used up).
    std::string Label(int entry) const;
    float HpPct() const { return hp; }
    void SetHp(float pct) { hp = pct; }
    double LastHealTime() const { return last_heal; }
    float LastHeal() const { return last_heal_amount; }
    int Insight() const { return insight; }
    const std::vector<std::string>& Ailments() const { return ailments; }
    void AddAilment(const std::string& name);
    const std::vector<BodyStatus>& Statuses() const { return statuses; }
    /// The buff on the weapon of `side` (0 left, 1 right).
    const WeaponBuff& Buff(int side) const { return buffs[side]; }
    /// The rub in progress on this hand's paper: share of the blade covered so far (0..1).
    float RubProgress(int hand) const { return held[hand].rub_cover; }
    const ConsumableDef& DefOf(int entry) const { return entries[size_t(entry)].def; }
    double Now() const { return now; }
    /// throwables/ or consumables/ mesh path of an entry ("" for procedural).
    std::filesystem::path ModelPath(int entry) const;
    bool HasMesh(int entry) const;
    /// Log lines of everything that happened (the tests read it).
    const std::vector<std::string>& EventLog() const { return event_log; }

    ~Consumables();

private:
    void BuildInventory(const ConsumeConfig& config);
    void LayoutWheel(const ConsumeInput& in, const ConsumeConfig& config);
    /// Returns true when the pick was used up at once (an instant weapon buff).
    bool Take(int hand, int slot, const ConsumeInput& in, const ConsumeConfig& config,
              Throwables& throwables, const ThrowConfig& tc, const ThrowInput& ti,
              ThrowEvents& te, ConsumeEvents& events);
    void CloseWheel(const ConsumeConfig& config, ConsumeEvents& events, const char* why);
    void PutBack(int hand, const char* why, const ConsumeConfig& config, ConsumeEvents& events);
    void Use(int hand, const ConsumeConfig& config, ConsumeEvents& events, const char* how);
    bool PayFor(const ConsumableDef& d);
    void StepHeld(int hand, const ConsumeInput& in, const ConsumeConfig& config,
                  ConsumeEvents& events);
    void StepRub(int hand, const ConsumeInput& in, const ConsumeConfig& config,
                 ConsumeEvents& events);
    void ApplyBuff(int hand, const ConsumeInput& in, const ConsumeConfig& config,
                   ConsumeEvents& events);
    void Emit(const ConsumeConfig& config, ConsumeEvents& events, const std::string& text,
              const std::string& json);
    void Pulse(ConsumeEvents& events, int hand, float amplitude, float seconds);

    std::filesystem::path assets_dir;
    std::vector<Entry> entries;
    std::vector<bool> has_mesh;
    json::Value items_table;
    std::string inventory_signature;
    std::vector<Slot> slots;
    Held held[2];
    int throw_taken[2] = {-1, -1}; // entry index handed to Throwables, per hand
    size_t thrown_seen = 0;
    bool open = false, button_was = false;
    bool pending_open = false; // a press came while the head was not tracked: open once it is
    double pending_since = 0.0;
    double closed_at = -1e9;
    float hp = 55.0f;
    double last_heal = -1e9;
    float last_heal_amount = 0.0f;
    int insight = 0;
    std::vector<std::string> ailments;
    std::vector<BodyStatus> statuses;
    WeaponBuff buffs[2];
    float squeeze_prev[2] = {0.0f, 0.0f};
    int hover[2] = {-1, -1}, last_hover[2] = {-1, -1};
    double now = 0.0;
    bool hp_init = false;
    std::vector<std::string> event_log;
    std::FILE* log_file = nullptr;
    std::string log_path;
};

/// 7-segment style glyphs as line segments for the wheel's labels: digits, 'x' and '~' (inf).
/// `origin` is the left-bottom of the first glyph, `right`/`up` unit vectors, `height` in metres.
using GlyphLineFn = std::function<void(m3::Vec3, m3::Vec3)>;
void AppendGlyphs(const std::string& text, m3::Vec3 origin, m3::Vec3 right, m3::Vec3 up,
                  float height, const GlyphLineFn& line);

} // namespace sb
