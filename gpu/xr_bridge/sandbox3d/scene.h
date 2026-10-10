// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// What sandbox3d shows and how the controllers drive it, independent of OpenXR (the offline mode
// feeds it scripted poses):
//   - the hunter (body + outfit) posed by the rig, first person: the head is hidden;
//   - a mirror copy (a true reflection) a couple of meters ahead, head included;
//   - the right weapon at R_Weapon (+ weapon_grip), the left one at L_Weapon;
//   - hit capsules from the hitbox extractor's JSON (else a placeholder along dummies 100+);
//   - floor, grid, skeleton, dummy points, controller axes, laser, IK debug lines.
// Controls (pad mapping of the bridge): A held 1.5 s = T-pose calibration (eye height, arm
// length), B = next right weapon, Y = next attack's hitboxes, X = mirror on/off,
// right stick click = skeleton on top on/off.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "config.h"
#include "consumables.h"
#include "model.h"
#include "physics_view.h"
#include "physics_weapon.h"
#include "renderer.h"
#include "rig.h"
#include "throwables.h"

namespace sb {

struct TrackedPose {
    bool valid = false;
    m3::Vec3 position;
    m3::Quat rotation;
};

struct SceneInput {
    double time = 0.0; // seconds
    TrackedPose head;  // between the eyes
    TrackedPose grip[2], aim[2];
    float trigger[2] = {}, squeeze[2] = {};
    uint32_t buttons = 0; // BBXR_BTN_*
    bool stage = false;   // the world space is STAGE: the floor is y = 0
};

struct SceneEvents {
    bool haptic[2] = {};
    float haptic_amplitude[2] = {}, haptic_seconds[2] = {}; // weapon contacts (physics_weapon.h)
    std::string log;
    std::string toast_title, toast_body; // what the app shows the player (consumables)
};

class Scene {
public:
    bool Load(const std::filesystem::path& assets, Renderer& renderer, std::string& log);
    void Update(const SceneInput& input, Config& config, ConfigFile* file, SceneEvents& events,
                Renderer& renderer);
    void Build(const Config& config, FrameData& out) const;

    const Rig& GetRig() const { return rig; }
    const Model& Body() const { return body; }
    WeaponPhysics& Physics() { return physics; } // solid weapons, props, haptics
    Throwables& Throw() { return throwables; }   // the rack and the hand-thrown goods
    Consumables& Consume() { return consumables; } // the item wheel and the inventory

private:
    struct Weapon {
        WeaponEntry entry;
        Model model;
        int gpu = -1;
        Hitboxes hitboxes;
        float hit_reach = 0.0f; // the farthest the hit shapes reach from the grip (hit_scale.h)
        bool loaded = false, failed = false;
        float reach = 0.4f; // grip to the far end along the long axis (the blade rubbed by papers)
        m3::Vec3 long_axis{0.0f, -1.0f, 0.0f}; // grip to the far end, weapon model space
        bool gun = false; // the long axis is the model's Z (barrel along -Z): aim it forward
    };

    void EnsureThrowModels(Renderer& renderer, std::string& log);
    void EnsureConsumeModels(Renderer& renderer, std::string& log);
    bool RackVisible(const Config& config) const;
    int FindWeapon(const std::string& want) const;
    Weapon* Load(int index, const Config& config, Renderer& renderer, std::string& log);
    void PlaceMirror(const SceneInput& input, const Config& config);
    m3::Mat4 WeaponWorld(int side, const Config& config) const;
    bool IsHeadMesh(const MeshRange& mesh) const;
    bool HiddenInFirstPerson(const std::string& hide, const MeshRange& mesh) const;
    /// A hit/hurt point in the world: body points follow their bone on the posed body (`body`:
    /// the body's world transform, the mirrored one for the mirror copy), weapon points the
    /// weapon.
    m3::Vec3 PointWorld(const ShapePoint& p, const m3::Mat4& body,
                        const m3::Mat4& weapon) const;

    std::filesystem::path assets_dir;
    AssetIndex index;
    Model body, floor_model;
    int body_gpu = -1, floor_gpu = -1;
    Rig rig;
    std::vector<Weapon> weapons;
    std::vector<Hurtbox> hurtboxes; // the player's (chr_c0000.json)
    int weapon_slot[2] = {-1, -1};       // index into weapons (left, right)
    std::string weapon_config[2];        // the config names they were picked from
    int attack = -1;
    std::string attack_config_seen;
    std::string form_seen;               // the config form the attack index was picked in
    bool mirror_on = true, mirror_placed = false, skeleton_top = true;
    m3::Mat4 mirror;
    uint32_t last_buttons = 0;
    double hold_since = -1.0;
    bool calibrated_this_hold = false;
    float floor_y = 0.0f;
    SceneInput last_input;
    int last_config_attack = -2;
    bool last_config_mirror = true, last_config_top = true;
    WeaponPhysics physics;    // weapons stop at the props, the enemy and each other
    Throwables throwables;    // the rack and the hand-thrown goods (throwables.h)
    struct ThrowModel {
        Model model;
        int gpu = -1;
        bool tried = false;
    };
    std::vector<ThrowModel> throw_models; // per throwable definition
    std::string throw_signature;          // the config the throwables were last set up with
    Consumables consumables;              // the item wheel of 3D objects (consumables.h)
    std::vector<ThrowModel> consume_models; // per consumables entry (throwables use throw_models)
    PhysicsView physics_view; // ... and how they are drawn
};

} // namespace sb
