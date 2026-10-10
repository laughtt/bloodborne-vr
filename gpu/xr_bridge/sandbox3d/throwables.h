// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Hand-thrown goods in the sandbox (docs/vr/findings/35-throwables.md), pure CPU, unit tested by
// throwables_test.cpp:
//   - a rack (a table in front of where you stood) with the 15 throwables of the game: Molotov
//     Cocktail, Delayed Molotov, Poison/Throwing Knife, Oil Urn, Pebble, Rope Molotovs (back
//     throws), Pungent Blood, Numbing Mist, Shining Coins, Tiny Tonitrus, Executioner's Gloves,
//     Blacksky Eye, Accursed Brew;
//   - grab with the grip (nearest within a radius, haptic), the item rides the hand with a per-item
//     grip offset; letting go throws: the hand's velocity over the last ~80 ms (mean and peak
//     blended) gives the direction and, scaled, the speed, mapped onto the game's Bullet values
//     (initial velocity, gravity inside/outside the range, acceleration, max velocity, life,
//     count and fan angle). Back-throw items and weak flicks just drop;
//   - flight is the Bullet integration (gravity switches at `dist` metres of travel, the speed is
//     capped at maxVellocity, the bullet ends at `life`), stopped by the floor, the table, the
//     post, the wall and the enemy's hurtboxes (penetrating bullets pass through the enemy), with
//     a per-type impact effect (fire, poison, oil, mist, blood, coin, lightning, slash, dust);
//   - mode "game": the game's own throw instead: a trigger press while holding throws along the
//     view with the exact initial velocity (the hand's speed is ignored).
// Every throw and impact is logged (item, release speed, range, what it hit).
// Settings: ThrowConfig (the "throwables" object of sandbox3d.json, hot reloaded).

#pragma once

#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "json.h"
#include "math3d.h"
#include "model.h"
#include "physics_weapon.h"
#include "throw_config.h"

namespace sb {

struct ThrowEvents {
    std::string log;
    float haptic_amplitude[2] = {}, haptic_seconds[2] = {};
};

struct ThrowInput {
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
};

/// A hand velocity estimator: positions over time -> a throw velocity.
class HandVelocity {
public:
    void Reset() { samples.clear(); }
    void Push(double t, m3::Vec3 p);
    /// The blend of the mean velocity over the last `window_s` and the fastest step in it;
    /// `end_before_s` ends the window that long before the newest sample.
    m3::Vec3 Estimate(float window_s, float peak_weight, float end_before_s = 0.0f) const;
    size_t Size() const { return samples.size(); }

private:
    struct Sample {
        double t;
        m3::Vec3 p;
    };
    std::deque<Sample> samples;
};

class Throwables {
public:
    enum State { kRack, kHeld, kFlying, kRest, kSpent };
    struct Item {
        int def = 0;
        State state = kRack;
        m3::Vec3 home;            // its place on the rack
        m3::Quat home_rot;
        m3::Vec3 pos;
        m3::Quat rot;
        m3::Vec3 vel;             // while it falls (a drop)
        int hand = -1;
        double since = 0.0;       // when the state began
        double drop_time = 0.0;
    };
    struct Bullet {
        int item = 0, def = 0, index = 0;
        bool alive = true;        // flying or resting (a persistent type or a fuse running)
        bool resting = false;
        m3::Vec3 pos, vel, origin;
        m3::Quat rot;
        double born = 0.0, rest_since = 0.0;
        float travelled = 0.0f;
        float hand_speed = 0.0f, release_speed = 0.0f;
        std::vector<int> hit_parts; // enemy parts already hit (penetrating bullets)
        std::string hit_what;
        bool ended = false;         // its Throw record is written
    };
    struct Effect {
        std::string kind;
        m3::Vec3 pos, normal;
        double t0 = 0.0;
        float duration = 1.0f, radius = 0.5f;
        float color[3] = {1, 1, 1};
        int seed = 0;
    };
    struct Throw { // the record of a finished bullet
        std::string item;
        int goods = 0;
        std::string mode;
        float hand_speed = 0.0f, release_speed = 0.0f, range_m = 0.0f, path_m = 0.0f;
        float flight_s = 0.0f;
        std::string hit;
        m3::Vec3 direction, origin, end;
        int bullet_index = 0;
    };

    /// `assets`: the folder holding throwables/ (may not exist); the extracted bullet table and
    /// the config's per-item overrides are applied here.
    void Load(const std::filesystem::path& assets, const ThrowConfig& config, std::string& log);
    /// Puts the rack in front of this head pose (where you stood and looked).
    void Place(m3::Vec3 head_position, m3::Quat head_rotation, float floor_y,
               const ThrowConfig& config);
    void Step(const ThrowInput& in, const ThrowConfig& config, const WeaponPhysics& world,
              ThrowEvents& events);
    /// Everything back on the rack, bullets and effects cleared.
    void Respawn();
    /// Re-applies the config's per-item overrides and rebuilds the rack (a config reload).
    void Reconfigure(const ThrowConfig& config);

    const std::vector<ThrowItemDef>& Defs() const { return defs; }
    const std::vector<Item>& Items() const { return items; }
    const std::vector<Bullet>& Bullets() const { return bullets; }
    const std::vector<Effect>& Effects() const { return effects; }
    const std::vector<Throw>& Throws() const { return finished; }
    /// Hands a stocked item (by goods id) to an empty hand wherever it is (the consumables wheel):
    /// false when the hand is busy, tracking is lost or the goods are unknown. The throw is
    /// counted in ThrownGoods().
    bool TakeFor(int hand, int goods, const ThrowInput& in, const ThrowConfig& config,
                 ThrowEvents& events);
    /// The goods id of every throw so far, in order (a bullet fan counts once).
    const std::vector<int>& ThrownGoods() const { return thrown_goods; }
    int HeldItem(int hand) const { return held[hand]; }
    int HoverItem(int hand) const { return hover[hand]; }
    bool Placed() const { return placed; }
    bool GameMode() const { return game_mode; }
    double Now() const { return now; }
    const col::Box& Table() const { return table; }
    /// throwables/<mesh>.json under the assets, "" without a mesh.
    std::filesystem::path ModelPath(int def) const;
    bool HasMesh(int def) const { return def >= 0 && size_t(def) < has_mesh.size() && has_mesh[size_t(def)]; }
    /// The item's / the bullet's world matrix (mesh scale included for extracted meshes).
    m3::Mat4 ItemWorld(const Item& item) const;
    m3::Mat4 BulletWorld(const Bullet& b) const;
    /// Wireframes: the impact effects and the rack's table; line(a, b, rgb).
    using LineFn = std::function<void(m3::Vec3, m3::Vec3, const uint8_t*)>;
    void EffectLines(const LineFn& line, const ThrowConfig& config) const;
    void TableLines(const LineFn& line) const;

    ~Throwables();

private:
    void Release(int hand, const ThrowInput& in, const ThrowConfig& config,
                 const WeaponPhysics& world, ThrowEvents& events, bool game_throw);
    void Drop(Item& item, m3::Vec3 velocity, const ThrowConfig& config, ThrowEvents& events,
              const char* why);
    void SpawnBullets(Item& item, m3::Vec3 dir, float speed, float hand_speed, bool game,
                      ThrowEvents& events, const ThrowConfig& config);
    void StepBullet(Bullet& b, double dt, const ThrowConfig& config, const WeaponPhysics& world,
                    ThrowEvents& events);
    void StepItem(Item& item, double dt, const ThrowConfig& config, const WeaponPhysics& world,
                  ThrowEvents& events);
    bool Collide(m3::Vec3 p, float radius, const WeaponPhysics& world,
                 const std::vector<int>* skip_parts, std::string& what, int& part,
                 m3::Vec3& point, m3::Vec3& normal) const;
    void Burst(const ThrowItemDef& def, m3::Vec3 point, m3::Vec3 normal,
               const ThrowConfig& config);
    void End(Bullet& b, const std::string& hit, m3::Vec3 end, const ThrowConfig& config,
             ThrowEvents& events);
    void Emit(const ThrowConfig& config, ThrowEvents& events, const std::string& text,
              const std::string& json);
    void BuildRack(const ThrowConfig& config);
    void ReturnToRack(Item& item);
    void Grab(int hand, int item_index, const ThrowInput& in, const ThrowConfig& config,
              ThrowEvents& events);
    m3::Mat4 HandMatrix(const ThrowInput& in, int hand) const;

    std::filesystem::path assets_dir;
    std::vector<ThrowItemDef> defs;
    std::vector<bool> has_mesh;
    json::Value extracted;          // throwables/throwables.json, kept for reconfiguration
    std::vector<Item> items;
    std::vector<Bullet> bullets;
    std::vector<Effect> effects;
    std::vector<Throw> finished;
    std::vector<int> thrown_goods;
    HandVelocity velocity[2];
    int held[2] = {-1, -1};
    int hover[2] = {-1, -1};
    bool squeeze_was[2] = {false, false}, trigger_was[2] = {false, false};
    uint32_t last_buttons = 0;
    std::string mode_seen;
    bool game_mode = false;
    bool placed = false;
    col::Box table;
    m3::Vec3 table_legs[4];
    float table_top = 0.0f, floor_y = 0.0f;
    m3::Vec3 anchor;
    float anchor_yaw = 0.0f;
    double now = 0.0, last_time = 0.0;
    bool have_time = false;
    std::FILE* log_file = nullptr;
    std::string log_path;
};

/// A procedural stand-in mesh (single bone, centred on the origin) for a shape name: "bottle",
/// "urn", "knife", "vial", "disc", "stone", "sphere"; the consumables' shapes (consumables.h):
/// "paper", "pellet", "bone", "bell", "flask", "syringe", "packet", "tablet".
Model MakeThrowModel(const std::string& shape, float scale);

} // namespace sb
