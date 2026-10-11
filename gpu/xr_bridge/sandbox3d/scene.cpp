// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "bbport_xr_bridge.h"
#include "hit_scale.h"
#include "parry_view.h"
#include "wire_shapes.h"

namespace sb {

using namespace m3;

namespace {

struct Color {
    uint8_t r, g, b;
};

constexpr Color kGrid{70, 74, 82}, kGridAxisX{170, 60, 60}, kGridAxisZ{60, 90, 180};
constexpr Color kBoneArmL{60, 220, 240}, kBoneArmR{240, 80, 60}, kBoneFinger{240, 220, 60};
constexpr Color kBoneLeg{90, 220, 110}, kBoneSpine{230, 230, 230};
constexpr Color kDummyWeapon{255, 70, 255}, kDummyBody{60, 255, 120};
constexpr Color kLaser{60, 220, 255}, kTarget{255, 230, 60}, kIk{255, 160, 40};

void Line(std::vector<LineVertex>& out, Vec3 a, Vec3 b, Color c) {
    out.push_back({{a.x, a.y, a.z}, {c.r, c.g, c.b, 255}});
    out.push_back({{b.x, b.y, b.z}, {c.r, c.g, c.b, 255}});
}

void Cross(std::vector<LineVertex>& out, Vec3 p, float size, Color c) {
    Line(out, p - Vec3{size, 0, 0}, p + Vec3{size, 0, 0}, c);
    Line(out, p - Vec3{0, size, 0}, p + Vec3{0, size, 0}, c);
    Line(out, p - Vec3{0, 0, size}, p + Vec3{0, 0, size}, c);
}

void Axes(std::vector<LineVertex>& out, Vec3 p, Quat q, float length) {
    Line(out, p, p + Rotate(q, {length, 0, 0}), {255, 60, 60});
    Line(out, p, p + Rotate(q, {0, length, 0}), {60, 255, 60});
    Line(out, p, p + Rotate(q, {0, 0, length}), {70, 110, 255});
}

/// Wireframe capsule (a sphere when a == b): wire_shapes.h, shared with the game's overlay.
void Capsule(std::vector<LineVertex>& out, Vec3 a, Vec3 b, float r, Color c) {
    WireCapsule([&](Vec3 p, Vec3 q) { Line(out, p, q, c); }, a, b, r);
}

Color HitColor(const std::string& type) {
    const WireColor w = HitTypeColor(type);
    return {w.r, w.g, w.b};
}

void SlotColor(const std::string& slot, size_t index, float out[4]) {
    float base[3] = {0.5f, 0.5f, 0.52f};
    if (slot == "skin") {
        base[0] = 0.78f, base[1] = 0.60f, base[2] = 0.50f;
    } else if (slot == "head") {
        base[0] = 0.20f, base[1] = 0.17f, base[2] = 0.15f;
    } else if (slot == "body") {
        base[0] = 0.27f, base[1] = 0.24f, base[2] = 0.22f;
    } else if (slot == "arm") {
        base[0] = 0.36f, base[1] = 0.27f, base[2] = 0.19f;
    } else if (slot == "leg") {
        base[0] = 0.22f, base[1] = 0.21f, base[2] = 0.23f;
    } else if (slot == "weapon") {
        base[0] = 0.62f, base[1] = 0.63f, base[2] = 0.67f;
    }
    const float vary[3] = {1.0f, 1.18f, 0.86f};
    for (int k = 0; k < 3; ++k) {
        out[k] = std::min(base[k] * vary[index % 3], 1.0f);
    }
    out[3] = 1.0f;
}

bool SkeletonBone(const Model& body, size_t b) {
    const Bone& bone = body.bones[b];
    if (bone.parent < 0 || !bone.deform) {
        return false;
    }
    const std::string& n = bone.name;
    return n.rfind("Dummy", 0) != 0 && n.rfind("Ctrl", 0) != 0 && n.rfind("Point", 0) != 0 &&
           n.rfind("Support_", 0) != 0 && n.find("_Case_") == std::string::npos &&
           n.find("Target") == std::string::npos && n.rfind("Face", 0) != 0;
}

Color BoneColor(const std::string& n) {
    if (n.find("Finger") != std::string::npos) {
        return kBoneFinger;
    }
    const bool arm = n.find("Clavicle") != std::string::npos ||
                     n.find("Arm") != std::string::npos || n.find("Hand") != std::string::npos ||
                     n.find("Weapon") != std::string::npos;
    if (arm) {
        return n.rfind("L_", 0) == 0 ? kBoneArmL : kBoneArmR;
    }
    if (n.find("Thigh") != std::string::npos || n.find("Calf") != std::string::npos ||
        n.find("Foot") != std::string::npos || n.find("Toe") != std::string::npos) {
        return kBoneLeg;
    }
    return kBoneSpine;
}

/// The attacks of a trick weapon form ("A" one-handed, "B"): those of that form, of both, or of
/// no known form (doc 14: draw the current form's).
std::vector<size_t> FormAttacks(const Hitboxes& hitboxes, const std::string& form) {
    std::vector<size_t> out;
    for (size_t a = 0; a < hitboxes.attacks.size(); ++a) {
        const std::string& f = hitboxes.attacks[a].form;
        if (form.empty() || f.empty() || f == "A/B" || f == form) {
            out.push_back(a);
        }
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

} // namespace

bool Scene::Load(const std::filesystem::path& assets, Renderer& renderer, std::string& log) {
    assets_dir = assets;
    std::string error;
    if (!LoadAssetIndex(assets, index, error) ||
        !LoadModel(assets / index.body_file, body, error) || !rig.Init(body, error)) {
        log = error;
        return false;
    }
    body_gpu = renderer.UploadModel(body.vertices, body.indices);
    // Floor: 12 m square at y = 0 (moved to the floor by its palette entry).
    floor_model.name = "floor";
    const float h = 6.0f;
    const float corners[4][2] = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
    for (const auto& c : corners) {
        Vertex v{};
        v.position[0] = c[0];
        v.position[2] = c[1];
        v.normal[1] = 1.0f;
        v.weights[0] = 1.0f;
        floor_model.vertices.push_back(v);
    }
    floor_model.indices = {0, 2, 1, 0, 3, 2};
    floor_model.meshes.push_back({"floor", "floor", "floor", "", -1, false, 0, "", 0, 6, 0, 4});
    floor_gpu = renderer.UploadModel(floor_model.vertices, floor_model.indices);
    if (body_gpu < 0 || floor_gpu < 0) {
        log = "upload failed: " + renderer.LastError();
        return false;
    }
    for (const WeaponEntry& w : index.weapons) {
        Weapon weapon;
        weapon.entry = w;
        weapons.push_back(std::move(weapon));
    }
    std::string hurt_log;
    hurtboxes = LoadHurtboxes(assets, "c0000", hurt_log);
    int unresolved = 0;
    for (Hurtbox& box : hurtboxes) {
        ResolveBone(body, box.a);
        ResolveBone(body, box.b);
        unresolved += box.a.bone < 0 && !box.a.bone_name.empty();
    }
    char text[512];
    std::snprintf(text, sizeof(text),
                  "assets %s: body %s (%zu vertices, %zu bones, %zu meshes), %zu weapons; %s%s",
                  assets.string().c_str(), index.body_file.c_str(), body.vertices.size(),
                  body.bones.size(), body.meshes.size(), weapons.size(), hurt_log.c_str(),
                  unresolved ? (", " + std::to_string(unresolved) + " on unknown bones").c_str()
                             : "");
    log = text;
    // Solid weapons: the enemy target (the config's, once Update sees it) and the prop meshes.
    std::string physics_log;
    physics.Load(assets, Config{}.physics, physics_log);
    physics_view.Upload(physics, renderer, physics_log);
    log += "\n" + physics_log;
    std::string throw_log;
    throwables.Load(assets, Config{}.throwables, throw_log);
    log += "\n" + throw_log;
    std::string consume_log;
    consumables.Load(assets, Config{}.consumables, throwables.Defs(), consume_log);
    log += "\n" + consume_log;
    std::string hands_log;
    custom_hands.Load(assets / "hands", renderer, hands_log);
    log += "\n" + hands_log;
    return true;
}

int Scene::FindWeapon(const std::string& want) const {
    if (want.empty()) {
        return -1;
    }
    const std::string low = Lower(want);
    for (size_t i = 0; i < weapons.size(); ++i) {
        const WeaponEntry& e = weapons[i].entry;
        if (Lower(e.name) == low || Lower(e.model) == low || Lower(e.file) == low) {
            return int(i);
        }
    }
    for (size_t i = 0; i < weapons.size(); ++i) {
        if (Lower(weapons[i].entry.name).find(low) != std::string::npos) {
            return int(i);
        }
    }
    return -1;
}

Scene::Weapon* Scene::Load(int slot, const Config& config, Renderer& renderer, std::string& log) {
    if (slot < 0 || size_t(slot) >= weapons.size()) {
        return nullptr;
    }
    Weapon& w = weapons[size_t(slot)];
    if (!w.loaded && !w.failed) {
        std::string error;
        if (!LoadModel(index.dir / w.entry.file, w.model, error)) {
            w.failed = true;
            log += "weapon " + w.entry.name + ": " + error + "\n";
            return nullptr;
        }
        w.gpu = renderer.UploadModel(w.model.vertices, w.model.indices);
        // The long axis: the bounding box's longest side, toward its far end from the grip.
        Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (const Vertex& v : w.model.vertices) {
            lo = {std::min(lo.x, v.position[0]), std::min(lo.y, v.position[1]),
                  std::min(lo.z, v.position[2])};
            hi = {std::max(hi.x, v.position[0]), std::max(hi.y, v.position[1]),
                  std::max(hi.z, v.position[2])};
        }
        const float ext[3] = {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
        const float far_hi[3] = {hi.x, hi.y, hi.z}, far_lo[3] = {-lo.x, -lo.y, -lo.z};
        const int k = ext[0] > ext[1] && ext[0] > ext[2] ? 0 : ext[1] > ext[2] ? 1 : 2;
        float axis[3] = {0.0f, 0.0f, 0.0f};
        axis[k] = far_hi[k] >= far_lo[k] ? 1.0f : -1.0f;
        w.long_axis = {axis[0], axis[1], axis[2]};
        w.reach = std::max(far_hi[k], far_lo[k]);
        w.gun = k == 2;
        std::string hit_log;
        w.hitboxes = LoadHitboxes(index.dir, w.model, w.entry.id, config.placeholder_radius,
                                  hit_log);
        for (Attack& attack : w.hitboxes.attacks) {
            for (HitShape& hit : attack.hits) {
                for (ShapePoint& p : hit.p) {
                    if (p.on_body) {
                        ResolveBone(body, p);
                    }
                }
            }
        }
        w.hit_reach = HitWeaponReach(w.hitboxes);
        w.loaded = w.gpu >= 0;
        w.failed = !w.loaded;
        log += "weapon " + w.entry.name + " (" + w.entry.model + "): " +
               std::to_string(w.model.vertices.size()) + " vertices, " +
               std::to_string(w.model.dummies.size()) + " dummies; " + hit_log + "\n";
    }
    return w.loaded ? &w : nullptr;
}

void Scene::PlaceMirror(const SceneInput& input, const Config& config) {
    if (!input.head.valid) {
        return;
    }
    Vec3 forward = Rotate(input.head.rotation, {0, 0, -1});
    forward.y = 0.0f;
    forward = Normalize(forward, {0, 0, -1});
    const Vec3 point = input.head.position + forward * config.mirror_distance_m;
    mirror = Reflection(forward, point);
    mirror_placed = true;
}

// fp_hide (config.h): head = the head meshes; body = all but the arms and hands (the arm armour
// and the skin's display-mask groups 43 arms and 44 hands, as the game keeps them); all = everything.
bool Scene::HiddenInFirstPerson(const std::string& hide, const MeshRange& mesh) const {
    if (hide == "all") {
        return true;
    }
    if (hide == "body") {
        return mesh.slot != "arm" && !(mesh.slot == "skin" && (mesh.mask == 43 || mesh.mask == 44));
    }
    return hide == "head" && IsHeadMesh(mesh);
}

bool Scene::IsHeadMesh(const MeshRange& mesh) const {
    const std::string& b = mesh.dominant_bone_name;
    return mesh.slot == "head" || b == "Head" || b == "Neck" || b.rfind("Face", 0) == 0;
}

Vec3 Scene::PointWorld(const ShapePoint& p, const Mat4& body_world, const Mat4& weapon) const {
    if (!p.on_body) {
        return TransformPoint(weapon, p.pos);
    }
    if (p.bone < 0 || size_t(p.bone) >= body.bones.size()) {
        return TransformPoint(body_world, p.pos); // no bone: static in the body's model space
    }
    const Mat4 bone = body_world * rig.Pose()[size_t(p.bone)];
    return p.has_local ? TransformPoint(bone, p.local)
                       : TransformPoint(bone * body.bones[size_t(p.bone)].inv_bind, p.pos);
}

Mat4 Scene::WeaponWorld(int side, const Config& config) const {
    int bone = rig.weapon_bone[side];
    if (bone < 0) {
        bone = rig.hand_bone[side];
    }
    const Offset& grip = side ? config.weapon_grip : config.left_weapon_grip;
    Quat align;
    const int slot = weapon_slot[side];
    if (config.weapon_align && slot >= 0 && weapons[size_t(slot)].loaded) {
        align = rig.WeaponAlignment(side, weapons[size_t(slot)].long_axis,
                                    weapons[size_t(slot)].gun);
    }
    return rig.BoneWorld(bone) * FromQuat(align) *
           FromPose(FromEulerDeg(grip.rotation_deg), grip.position);
}

/// The throwables' meshes (extracted effect meshes where throwables/<mesh>.json exists, else the
/// procedural stand-in of the item's shape), uploaded once per definition.
void Scene::EnsureThrowModels(Renderer& renderer, std::string& log) {
    const std::vector<ThrowItemDef>& defs = throwables.Defs();
    if (throw_models.size() < defs.size()) {
        throw_models.resize(defs.size());
    }
    for (size_t i = 0; i < defs.size(); ++i) {
        ThrowModel& tm = throw_models[i];
        if (tm.tried) {
            continue;
        }
        tm.tried = true;
        std::string error;
        if (!(throwables.HasMesh(int(i)) &&
              LoadModel(throwables.ModelPath(int(i)), tm.model, error))) {
            if (!error.empty()) {
                log += "throwable " + defs[i].name + ": " + error + " (procedural stand-in)\n";
            }
            tm.model = MakeThrowModel(defs[i].shape, 1.0f);
        }
        tm.gpu = renderer.UploadModel(tm.model.vertices, tm.model.indices);
    }
}

/// The consumables' meshes (consumables/cs_<goods>.json where extracted, else the procedural
/// stand-in of the item's shape).
void Scene::EnsureConsumeModels(Renderer& renderer, std::string& log) {
    const auto& entries = consumables.Entries();
    if (consume_models.size() < entries.size()) {
        consume_models.resize(entries.size());
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        ThrowModel& cm = consume_models[i];
        if (cm.tried || entries[i].def.group == CGroup::Throwable) {
            continue;
        }
        cm.tried = true;
        std::string error;
        if (!(consumables.HasMesh(int(i)) &&
              LoadModel(consumables.ModelPath(int(i)), cm.model, error))) {
            if (!error.empty()) {
                log += "consumable " + entries[i].def.name + ": " + error + " (procedural stand-in)\n";
            }
            cm.model = MakeThrowModel(entries[i].def.shape, 1.0f);
        }
        cm.gpu = renderer.UploadModel(cm.model.vertices, cm.model.indices);
    }
}

/// The throwables' rack is shown and grabbable unless the consumables wheel hands the items out.
bool Scene::RackVisible(const Config& config) const {
    return config.throwables.rack_grab && !(config.consumables.Objects() && config.consumables.hide_rack);
}

/// The finger pose of a hand: what it holds (the sandbox's tuned grips first, then the hold
/// category), else the analog grip and trigger.
vrui::hand::Pose Scene::HandTarget(int side, const SceneInput& input, const Config& config) const {
    using namespace vrui::hand;
    const float trigger = input.trigger[side], squeeze = input.squeeze[side];
    uint32_t id = BBXR_ITEM_NONE;
    GripDef tuned;
    const int thrown = throwables.HeldItem(side);
    if (thrown >= 0) {
        const ThrowItemDef& d = throwables.Defs()[size_t(throwables.Items()[size_t(thrown)].def)];
        id = BBXR_ITEM_GOODS | uint32_t(d.goods);
        tuned.pos = d.grip.position;
        tuned.rot_deg = d.grip.rotation_deg;
    } else if (consumables.HeldBy(side).entry >= 0) {
        const ConsumableDef& d = consumables.DefOf(consumables.HeldBy(side).entry);
        id = BBXR_ITEM_GOODS | uint32_t(d.goods);
        tuned.pos = d.hold;
        tuned.rot_deg = d.hold_rot_deg;
    }
    if (id != BBXR_ITEM_NONE) {
        return HoldPose(GripFor(id, &tuned), trigger);
    }
    const int slot = weapon_slot[side];
    if (slot >= 0 && weapons[size_t(slot)].loaded && !consumables.WeaponStowed(side, config.consumables)) {
        GripDef g = CategoryGrip(HoldCategory::Weapon);
        if (weapons[size_t(slot)].gun) { // the index rests along the frame until the trigger
            return CurlPose(g.thumb, 0.3f + 0.7f * std::clamp(trigger, 0.0f, 1.0f), g.others);
        }
        return HoldPose(g, 0.0f);
    }
    return ControllerPose(squeeze, trigger);
}

void Scene::Update(const SceneInput& input, Config& config, ConfigFile* file,
                   SceneEvents& events, Renderer& renderer) {
    floor_y = input.stage ? 0.0f : config.floor_y;
    if (config.form != form_seen) { // a trick weapon transformed (or the config changed): the
        if (!form_seen.empty()) {   // attack index belonged to the other form's list
            attack = -1;
            events.log += "form " + config.form + ": hitboxes of that form, all attacks\n";
        }
        form_seen = config.form;
    }
    // The config picks the weapons, the attack and the toggles; buttons change them until the
    // config does again.
    const std::string* wanted[2] = {&config.left_weapon, &config.weapon};
    for (int side = 0; side < 2; ++side) {
        if (*wanted[side] != weapon_config[side]) {
            weapon_config[side] = *wanted[side];
            weapon_slot[side] = FindWeapon(*wanted[side]);
            if (!wanted[side]->empty() && weapon_slot[side] < 0) {
                events.log += "no weapon \"" + *wanted[side] + "\" in assets.json\n";
            }
        }
        Load(weapon_slot[side], config, renderer, events.log);
    }
    if (config.attack != last_config_attack) {
        last_config_attack = config.attack;
        attack = config.attack;
    }
    if (config.show_mirror != last_config_mirror) {
        last_config_mirror = mirror_on = config.show_mirror;
    }
    if (config.skeleton_on_top != last_config_top) {
        last_config_top = skeleton_top = config.skeleton_on_top;
    }

    RigInput rig_input;
    rig_input.head_valid = input.head.valid;
    rig_input.head_position = input.head.position;
    rig_input.head_rotation = input.head.rotation;
    for (int side = 0; side < 2; ++side) {
        HandInput& hand = rig_input.hands[side];
        hand.valid = input.grip[side].valid;
        hand.position = input.grip[side].position;
        hand.rotation = input.grip[side].rotation;
        hand.trigger = input.trigger[side];
        hand.grip = input.squeeze[side];
    }
    rig.Update(config, rig_input, floor_y);
    ThrowEvents throw_events;
    {
        // The rack and the hand-thrown goods (throwables.h): grab with the grip, throw on release.
        EnsureThrowModels(renderer, events.log);
        const std::string signature = config.throwables.Summary() +
                                      ThrowItemsSignature(config.throwables.items);
        if (signature != throw_signature) {
            throw_signature = signature;
            throwables.Reconfigure(config.throwables);
            EnsureThrowModels(renderer, events.log);
        }
        ThrowConfig tcfg = config.throwables;
        tcfg.rack_grab = RackVisible(config);
        if (!throwables.Placed() && input.head.valid) {
            throwables.Place(input.head.position, input.head.rotation, floor_y, config.throwables);
        }
        ThrowInput ti;
        ti.time = input.time;
        ti.head_valid = input.head.valid;
        ti.head_position = input.head.position;
        ti.head_rotation = input.head.rotation;
        for (int side = 0; side < 2; ++side) {
            ti.hand_valid[side] = input.grip[side].valid;
            ti.grip_position[side] = input.grip[side].position;
            ti.grip_rotation[side] = input.grip[side].rotation;
            ti.squeeze[side] = input.squeeze[side];
            ti.trigger[side] = input.trigger[side];
        }
        ti.buttons = input.buttons;
        ti.floor_y = floor_y;
        throwables.Step(ti, tcfg, physics, throw_events);
        events.log += throw_events.log;
    }
    {
        // Solid weapons (physics_weapon.h): each weapon stops at what it hits; the hand holding it
        // follows it (the rig runs again with the moved hands), a ghost shows the real one.
        WeaponPhysics::StepInput step;
        step.time = input.time;
        step.head_valid = input.head.valid;
        step.head_position = input.head.position;
        step.head_rotation = input.head.rotation;
        step.floor_y = floor_y;
        for (int side = 0; side < 2; ++side) {
            // The raw controllers: the teleport check must not see the IK-clamped hands.
            step.controller_valid[side] = input.grip[side].valid;
            step.controller_position[side] = input.grip[side].position;
            step.controller_rotation[side] = input.grip[side].rotation;
        }
        step.rig = &rig;
        step.body = &body;
        for (int side = 0; side < 2; ++side) {
            const int slot = weapon_slot[side];
            if (slot >= 0 && weapons[size_t(slot)].loaded && throwables.HeldItem(side) < 0 &&
                !consumables.WeaponStowed(side, config.consumables)) {
                const Weapon& w = weapons[size_t(slot)];
                HeldWeapon& held = step.held[side];
                held.model = &w.model;
                held.name = w.entry.name;
                held.target = WeaponWorld(side, config);
                // Body-sourced shapes (kicks) where the scene draws them: bone-relative.
                const Mat4 to_weapon = Inverse(held.target);
                held.attack = AttackShapesFor(
                    w.hitboxes, config.form, side == 1 ? attack : (attack < 0 ? -1 : 0),
                    &held.attack_name, [&](const ShapePoint& p) {
                        return TransformPoint(to_weapon, PointWorld(p, rig.BodyWorld(), Mat4{}));
                    });
                // The VR hit scale (hit_radius / hit_reach): the physics tests the same shapes
                // the wireframe draws.
                held.attack = ScaleHitShapes(config.physics.hit, held.attack, w.hit_reach);
            }
        }
        PhysicsEvents physics_events;
        // The parry dummy is the physics enemy, moved straight ahead of the calibration pose.
        PhysicsConfig physics_config = config.physics;
        if (config.parry.enabled && config.parry.dummy) {
            physics_config.enemy_pos[0] = config.parry.x_m;
            physics_config.enemy_pos[1] = -config.parry.distance_m;
            physics_config.enemy_face_user = true;
        }
        physics.Step(step, physics_config, physics_events);
        physics_view.Sync(physics, renderer);
        events.log += physics_events.log;
        for (int side = 0; side < 2; ++side) {
            events.haptic_amplitude[side] = physics_events.haptic_amplitude[side];
            events.haptic_seconds[side] = physics_events.haptic_seconds[side];
            if (throw_events.haptic_amplitude[side] > events.haptic_amplitude[side]) {
                events.haptic_amplitude[side] = throw_events.haptic_amplitude[side];
                events.haptic_seconds[side] = throw_events.haptic_seconds[side];
            }
        }
        if (physics.CorrectHands(rig_input)) {
            rig.Update(config, rig_input, floor_y);
        }
    }
    {
        // The item wheel and what is in the hands (consumables.h): grab, throw (Throwables), rub
        // along the weapon, bring to the head, crush. Weapons are put away while it is open.
        EnsureConsumeModels(renderer, events.log);
        ConsumeInput ci;
        ci.time = input.time;
        ci.head_valid = input.head.valid;
        ci.head_position = input.head.position;
        ci.head_rotation = input.head.rotation;
        ci.buttons = input.buttons;
        ci.floor_y = floor_y;
        ThrowInput ti;
        ti.time = input.time;
        ti.head_valid = ci.head_valid;
        ti.head_position = ci.head_position;
        ti.head_rotation = ci.head_rotation;
        ti.buttons = input.buttons;
        ti.floor_y = floor_y;
        for (int side = 0; side < 2; ++side) {
            ci.hand_valid[side] = ti.hand_valid[side] = input.grip[side].valid;
            ci.grip_position[side] = ti.grip_position[side] = input.grip[side].position;
            ci.grip_rotation[side] = ti.grip_rotation[side] = input.grip[side].rotation;
            ci.squeeze[side] = ti.squeeze[side] = input.squeeze[side];
            ci.trigger[side] = ti.trigger[side] = input.trigger[side];
            const int slot = weapon_slot[side];
            ci.weapon_equipped[side] = slot >= 0 && weapons[size_t(slot)].loaded;
            if (slot >= 0 && weapons[size_t(slot)].loaded && throwables.HeldItem(side) < 0 &&
                !consumables.WeaponStowed(side, config.consumables)) {
                const Weapon& w = weapons[size_t(slot)];
                const Mat4 world = physics.WeaponPose(side, WeaponWorld(side, config));
                ci.weapon_valid[side] = true;
                ci.weapon_base[side] = TransformPoint(world, {0, 0, 0});
                ci.weapon_tip[side] = TransformPoint(world, w.long_axis * w.reach);
            }
        }
        ThrowConfig tcfg = config.throwables;
        tcfg.rack_grab = RackVisible(config);
        ThrowEvents grab_events;
        ConsumeEvents consume_events;
        consumables.Step(ci, config.consumables, throwables, tcfg, ti, grab_events, consume_events);
        events.log += grab_events.log + consume_events.log;
        events.toast_title = consume_events.toast_title;
        events.toast_body = consume_events.toast_body;
        for (int side = 0; side < 2; ++side) {
            events.haptic_amplitude[side] = std::max({events.haptic_amplitude[side],
                                                      consume_events.haptic_amplitude[side],
                                                      grab_events.haptic_amplitude[side]});
            events.haptic_seconds[side] = std::max({events.haptic_seconds[side],
                                                    consume_events.haptic_seconds[side],
                                                    grab_events.haptic_seconds[side]});
        }
    }
    {
        // The left-hand gun thrust and the parry dummy (parry.h).
        ParryInput pi;
        pi.time = input.time;
        pi.head_valid = input.head.valid;
        pi.head_position = input.head.position;
        pi.head_rotation = input.head.rotation;
        pi.hand_valid = input.grip[0].valid && input.aim[0].valid;
        pi.hand_position = input.grip[0].position;
        pi.aim_position = input.aim[0].position;
        pi.aim_rotation = input.aim[0].rotation;
        pi.trigger = input.trigger[0];
        pi.hand_free = throwables.HeldItem(0) < 0 && !consumables.HandBusy(0);
        pi.rhand_valid = input.grip[1].valid && input.aim[1].valid;
        pi.rhand_position = input.grip[1].position;
        pi.rhand_rotation = input.aim[1].rotation;
        pi.floor_y = floor_y;
        const int gun_slot = weapon_slot[0];
        if (gun_slot >= 0 && weapons[size_t(gun_slot)].loaded && pi.hand_free &&
            !consumables.WeaponStowed(0, config.consumables)) {
            const Weapon& w = weapons[size_t(gun_slot)];
            const Mat4 world = physics.WeaponPose(0, WeaponWorld(0, config));
            pi.muzzle = TransformPoint(world, w.long_axis * w.reach);
            pi.muzzle_valid = true;
        }
        const WeaponPhysics::Enemy& enemy = physics.GetEnemy();
        if (enemy.placed) {
            for (const col::Capsule& c : enemy.parts) {
                pi.parts.push_back({TransformPoint(enemy.world, c.a), TransformPoint(enemy.world, c.b), c.r});
            }
        }
        for (const WeaponPhysics::Marker& m : physics.Markers()) {
            if (m.side == 1 && m.part >= 0) {
                pi.melee_hit_time = std::max(pi.melee_hit_time, m.time);
            }
        }
        ParryEvents pe;
        parry.Step(pi, config.parry, pe);
        events.log += pe.log;
        for (int side = 0; side < 2; ++side) {
            if (pe.haptic_amplitude[side] > events.haptic_amplitude[side]) {
                events.haptic_amplitude[side] = pe.haptic_amplitude[side];
                events.haptic_seconds[side] = pe.haptic_seconds[side];
            }
        }
        if (!pe.toast_title.empty()) {
            events.toast_title = pe.toast_title;
            events.toast_body = pe.toast_body;
        }
        events.beep_hz = pe.beep_hz;
        events.beep_ms = pe.beep_ms;
    }
    {
        // The custom hands: the controller poses as the rig ended up with them, the fingers from
        // grip and trigger or from what the hand holds.
        const double dt = hands_time > 0.0 ? input.time - hands_time : 0.0;
        hands_time = input.time;
        for (int side = 0; side < 2; ++side) {
            const HandInput& hand = rig_input.hands[side];
            custom_hands.Set(side, hand.valid, hand.position, hand.rotation,
                             HandTarget(side, input, config), dt, config.hands);
        }
    }
    if (!mirror_placed) {
        PlaceMirror(input, config);
    }

    const uint32_t pressed = input.buttons & ~last_buttons;
    if (pressed & BBXR_BTN_CROSS) {
        if (!weapons.empty()) {
            weapon_slot[1] = (weapon_slot[1] + 1) % int(weapons.size());
            Weapon* w = Load(weapon_slot[1], config, renderer, events.log);
            events.log += "right weapon: " + weapons[size_t(weapon_slot[1])].entry.name +
                          (w ? "" : " (failed to load)") + "\n";
            attack = -1;
            events.haptic[1] = true;
        }
    }
    if (pressed & BBXR_BTN_TRIANGLE) {
        const Weapon* w = weapon_slot[1] >= 0 ? &weapons[size_t(weapon_slot[1])] : nullptr;
        const std::vector<size_t> form =
            w && w->loaded ? FormAttacks(w->hitboxes, config.form) : std::vector<size_t>{};
        const int count = int(form.size());
        attack = count ? (attack + 2) % (count + 1) - 1 : -1;
        events.log += "hitboxes (form " + config.form + "): " +
                      (attack < 0 ? std::string("all attacks")
                                  : std::to_string(attack + 1) + "/" + std::to_string(count) +
                                        " " + w->hitboxes.attacks[form[size_t(attack)]].name) +
                      "\n";
        events.haptic[0] = true;
    }
    if (pressed & BBXR_BTN_SQUARE) {
        mirror_on = !mirror_on;
        if (mirror_on) {
            PlaceMirror(input, config);
        }
        events.log += std::string("mirror ") + (mirror_on ? "on" : "off") + "\n";
    }
    if ((pressed & BBXR_BTN_R3) && !config.consumables.Objects()) { // (R3 opens the item wheel)
        skeleton_top = !skeleton_top;
        events.log += std::string("skeleton ") + (skeleton_top ? "on top" : "depth tested") +
                      "\n";
    }
    // T-pose calibration: A held 1.5 s.
    if (input.buttons & BBXR_BTN_CIRCLE) {
        if (hold_since < 0.0) {
            hold_since = input.time;
            calibrated_this_hold = false;
        } else if (!calibrated_this_hold && input.time - hold_since >= 1.5 && input.head.valid) {
            calibrated_this_hold = true;
            const float eye = input.head.position.y - floor_y;
            char text[256];
            if (eye > 0.9f && eye < 2.3f) {
                config.eye_height_m = eye;
                if (input.grip[0].valid && input.grip[1].valid) {
                    const float span = Length(input.grip[0].position - input.grip[1].position);
                    const float s = eye / rig.ModelEyeHeight(config);
                    const float arm = (span - rig.ModelShoulderWidth() * s) * 0.5f - 0.08f;
                    config.arm_scale =
                        std::clamp(arm / std::max(rig.ModelArmLength() * s, 0.1f), 0.75f, 1.35f);
                }
                std::snprintf(text, sizeof(text),
                              "calibrated (T-pose): eye height %.3f m, arm scale %.3f\n", eye,
                              config.arm_scale);
                events.log += text;
                if (file) {
                    std::string saved;
                    file->SaveCalibration(config.eye_height_m, config.arm_scale, saved);
                    events.log += saved;
                }
                rig.ResetYaw();
                PlaceMirror(input, config);
                physics.PlaceProps(input.head.position, input.head.rotation); // props around you
                throwables.Place(input.head.position, input.head.rotation, floor_y,
                                 config.throwables); // ... and the rack
                events.haptic[0] = events.haptic[1] = true;
            } else {
                std::snprintf(text, sizeof(text),
                              "calibration skipped: eye height %.2f m above the floor is not "
                              "plausible (LOCAL space: set floor_y in the config)\n",
                              eye);
                events.log += text;
            }
        }
    } else {
        hold_since = -1.0;
    }
    last_buttons = input.buttons;
    last_input = input;
}

void Scene::Build(const Config& config, FrameData& out) const {
    out = {};
    out.clear = config.background;
    const bool show_mirror = mirror_on && mirror_placed;
    // Palette: body, mirrored body, weapons (one matrix per weapon bone), mirrored weapons, floor.
    rig.AppendPalette(rig.BodyWorld(), out.palette);
    uint32_t mirror_base = 0;
    if (show_mirror) {
        mirror_base = uint32_t(out.palette.size());
        rig.AppendPalette(mirror * rig.BodyWorld(), out.palette);
    }
    uint32_t weapon_base[2][2] = {};
    Mat4 weapon_world[2];
    const Weapon* held[2] = {};
    for (int side = 0; side < 2; ++side) {
        const int slot = weapon_slot[side];
        if (slot < 0 || !weapons[size_t(slot)].loaded || throwables.HeldItem(side) >= 0 ||
            consumables.WeaponStowed(side, config.consumables)) {
            continue; // (a hand holding a throwable or an item, or the wheel open: no weapon)
        }
        held[side] = &weapons[size_t(slot)];
        weapon_world[side] = physics.WeaponPose(side, WeaponWorld(side, config)); // as simulated
        for (int copy = 0; copy < (show_mirror ? 2 : 1); ++copy) {
            weapon_base[side][copy] = uint32_t(out.palette.size());
            const Mat4 w = copy ? mirror * weapon_world[side] : weapon_world[side];
            for (size_t b = 0; b < std::max<size_t>(held[side]->model.bones.size(), 1); ++b) {
                out.palette.push_back(w);
            }
        }
    }
    const uint32_t floor_base = uint32_t(out.palette.size());
    out.palette.push_back(Translate({0.0f, floor_y, 0.0f}));

    // Meshes.
    {
        MeshDraw floor;
        floor.model = floor_gpu;
        floor.index_count = 6;
        floor.palette_base = floor_base;
        floor.color[0] = 0.15f, floor.color[1] = 0.15f, floor.color[2] = 0.17f;
        floor.flags = 1;
        out.meshes.push_back(floor);
    }
    if (config.throwables.enabled && config.throwables.show && throwables.Placed()) {
        const auto add = [&](const Mat4& world, int def) {
            if (size_t(def) >= throw_models.size() || throw_models[size_t(def)].gpu < 0) {
                return;
            }
            const ThrowModel& tm = throw_models[size_t(def)];
            const uint32_t base = uint32_t(out.palette.size());
            for (size_t b = 0; b < std::max<size_t>(tm.model.bones.size(), 1); ++b) {
                out.palette.push_back(world);
            }
            const ThrowItemDef& d = throwables.Defs()[size_t(def)];
            for (const MeshRange& mesh : tm.model.meshes) {
                MeshDraw draw;
                draw.model = tm.gpu;
                draw.first_index = mesh.first_index;
                draw.index_count = mesh.index_count;
                draw.palette_base = base;
                for (int k = 0; k < 3; ++k) {
                    draw.color[k] = 0.3f + 0.7f * d.color[k];
                }
                out.meshes.push_back(draw);
            }
        };
        const bool rack = RackVisible(config);
        for (const Throwables::Item& item : throwables.Items()) {
            if (item.state != Throwables::kSpent && (rack || item.state != Throwables::kRack)) {
                add(throwables.ItemWorld(item), item.def);
            }
        }
        for (const Throwables::Bullet& b : throwables.Bullets()) {
            if (b.alive) {
                add(throwables.BulletWorld(b), b.def);
            }
        }
    }
    if (config.consumables.Objects()) {
        const auto draw_model = [&](const ThrowModel& tm, const Mat4& world, const float color[3],
                                    float boost) {
            if (tm.gpu < 0) {
                return;
            }
            const uint32_t base = uint32_t(out.palette.size());
            for (size_t b = 0; b < std::max<size_t>(tm.model.bones.size(), 1); ++b) {
                out.palette.push_back(world);
            }
            for (const MeshRange& mesh : tm.model.meshes) {
                MeshDraw draw;
                draw.model = tm.gpu;
                draw.first_index = mesh.first_index;
                draw.index_count = mesh.index_count;
                draw.palette_base = base;
                for (int k = 0; k < 3; ++k) {
                    draw.color[k] = std::min(1.0f, 0.3f + 0.7f * color[k] + boost);
                }
                out.meshes.push_back(draw);
            }
        };
        const auto entry_model = [&](int e, float& scale) -> const ThrowModel* {
            const ConsumableDef& d = consumables.DefOf(e);
            if (d.group == CGroup::Throwable) {
                if (d.throw_def < 0 || size_t(d.throw_def) >= throw_models.size()) {
                    return nullptr;
                }
                scale = throwables.HasMesh(d.throw_def) ? throwables.Defs()[size_t(d.throw_def)].scale : 1.0f;
                return &throw_models[size_t(d.throw_def)];
            }
            if (size_t(e) >= consume_models.size()) {
                return nullptr;
            }
            scale = d.scale;
            return &consume_models[size_t(e)];
        };
        for (size_t i = 0; i < consumables.Slots().size(); ++i) {
            if (!consumables.SlotVisible(int(i))) {
                continue;
            }
            const int e = consumables.Slots()[i].entry;
            float scale = 1.0f;
            const ThrowModel* tm = entry_model(e, scale);
            if (!tm) {
                continue;
            }
            const bool hover = consumables.Hover(0) == int(i) || consumables.Hover(1) == int(i);
            const Mat4 world = Translate(consumables.SlotPos(int(i), config.consumables.bob_m)) *
                               FromQuat(consumables.SlotRot(int(i))) *
                               Scale(scale * config.consumables.item_scale);
            draw_model(*tm, world, consumables.DefOf(e).color, hover ? 0.35f : 0.0f);
        }
        for (int side = 0; side < 2; ++side) {
            const Consumables::Held& h = consumables.HeldBy(side);
            if (h.entry < 0 || h.via_throwables) {
                continue; // (a thrown item is drawn with the throwables)
            }
            float scale = 1.0f;
            if (const ThrowModel* tm = entry_model(h.entry, scale)) {
                draw_model(*tm, Translate(h.pos) * FromQuat(h.rot) * Scale(scale),
                           consumables.DefOf(h.entry).color, 0.0f);
            }
        }
    }
    const bool custom_hands_on = config.hands.Custom() && custom_hands.Ready();
    if (custom_hands_on) {
        custom_hands.Append(config.hands, show_mirror ? &mirror : nullptr, out);
    }
    if (config.show_body) {
        for (int copy = 0; copy < (show_mirror ? 2 : 1); ++copy) {
            for (size_t i = 0; i < body.meshes.size(); ++i) {
                const MeshRange& mesh = body.meshes[i];
                if (mesh.hidden && !config.show_masked) {
                    continue;
                }
                if (custom_hands_on && mesh.slot == "skin" && mesh.mask == 44) {
                    continue; // the generic hands replace the hunter's own
                }
                if (copy == 0 && config.fp_hide.empty() && !config.show_head && IsHeadMesh(mesh)) {
                    continue; // first person: no head in front of the eyes
                }
                if (copy == 0 && HiddenInFirstPerson(config.fp_hide, mesh)) {
                    continue; // fp_hide: the parts the game hides (findings/16 4.2)
                }
                MeshDraw draw;
                draw.model = body_gpu;
                draw.first_index = mesh.first_index;
                draw.index_count = mesh.index_count;
                draw.palette_base = copy ? mirror_base : 0;
                SlotColor(mesh.slot, i, draw.color);
                out.meshes.push_back(draw);
            }
        }
    }
    if (config.show_weapon) {
        for (int side = 0; side < 2; ++side) {
            if (!held[side]) {
                continue;
            }
            for (int copy = 0; copy < (show_mirror ? 2 : 1); ++copy) {
                for (size_t i = 0; i < held[side]->model.meshes.size(); ++i) {
                    const MeshRange& mesh = held[side]->model.meshes[i];
                    MeshDraw draw;
                    draw.model = held[side]->gpu;
                    draw.first_index = mesh.first_index;
                    draw.index_count = mesh.index_count;
                    draw.palette_base = weapon_base[side][copy];
                    SlotColor("weapon", i, draw.color);
                    const WeaponBuff& buff = consumables.Buff(side);
                    if (buff.active) { // the buff's glow: a pulsing tint toward its colour
                        const float k = 0.55f + 0.2f * std::sin(float(consumables.Now()) * 9.0f);
                        for (int c = 0; c < 3; ++c) {
                            draw.color[c] = draw.color[c] * (1.0f - k) + (0.4f + 0.6f * buff.color[c]) * k;
                        }
                    }
                    out.meshes.push_back(draw);
                }
            }
        }
    }

    // Lines.
    if (config.show_grid) {
        const float y = floor_y + 0.003f;
        for (int k = -10; k <= 10; ++k) {
            const float v = float(k) * 0.5f;
            Line(out.lines, {v, y, -5.0f}, {v, y, 5.0f}, k == 0 ? kGridAxisZ : kGrid);
            Line(out.lines, {-5.0f, y, v}, {5.0f, y, v}, k == 0 ? kGridAxisX : kGrid);
        }
    }
    auto& skeleton_lines = skeleton_top ? out.overlay : out.lines;
    if (config.show_skeleton) {
        const size_t limit = std::min(body.bones.size(),
                                      size_t(body.doc["skeleton_bones"].Number(
                                          double(body.bones.size()))));
        for (size_t b = 0; b < limit; ++b) {
            if (!SkeletonBone(body, b)) {
                continue;
            }
            const Vec3 a = Translation(rig.BoneWorld(int(b)));
            const Vec3 p = Translation(rig.BoneWorld(body.bones[b].parent));
            const Color c = BoneColor(body.bones[b].name);
            Line(skeleton_lines, a, p, c);
            if (show_mirror) {
                Line(out.lines, TransformPoint(mirror, a), TransformPoint(mirror, p), c);
            }
        }
        // IK: shoulder -> elbow -> wrist and the wrist target.
        for (int side = 0; side < 2; ++side) {
            const Rig::ArmState& s = rig.arm_state[side];
            if (!s.solved) {
                continue;
            }
            const Mat4& w = rig.BodyWorld();
            Line(out.overlay, TransformPoint(w, s.shoulder), TransformPoint(w, s.elbow), kIk);
            Line(out.overlay, TransformPoint(w, s.elbow), TransformPoint(w, s.wrist), kIk);
            Cross(out.overlay, TransformPoint(w, s.wrist), s.clamped ? 0.03f : 0.015f,
                  s.clamped ? Color{255, 40, 40} : kTarget);
        }
    }
    if (config.show_dummies) {
        for (int side = 0; side < 2; ++side) {
            if (!held[side]) {
                continue;
            }
            for (const Dummy& d : held[side]->model.dummies) {
                Cross(out.overlay, TransformPoint(weapon_world[side], d.position), 0.01f,
                      kDummyWeapon);
            }
        }
        for (const Dummy& d : body.dummies) {
            const int bone = d.attach >= 0 ? d.attach : d.parent;
            if (bone < 0 || size_t(bone) >= body.bones.size() || !SkeletonBone(body, size_t(bone))) {
                continue;
            }
            const Mat4 m = rig.BodyWorld() * rig.Pose()[size_t(bone)] *
                           body.bones[size_t(bone)].inv_bind;
            Cross(out.lines, TransformPoint(m, d.position), 0.006f, kDummyBody);
        }
    }
    if (config.show_hitboxes) {
        for (int side = 0; side < 2; ++side) {
            if (!held[side]) {
                continue;
            }
            const Hitboxes& hb = held[side]->hitboxes;
            const float scale = rig.BodyScale();
            // The chosen attack (right hand; the left shows its first), or with attack -1
            // the weapon's own shapes of every attack, each shape once.
            std::vector<const HitShape*> shown;
            const std::vector<size_t> form = FormAttacks(hb, config.form);
            const int pick = side == 1 ? attack : (attack < 0 ? -1 : 0);
            for (size_t k = 0; k < form.size(); ++k) {
                if (pick >= 0 && size_t(pick) != k) {
                    continue;
                }
                for (const HitShape& hit : hb.attacks[form[k]].hits) {
                    if (pick < 0 && (hit.p[0].on_body || hit.p[1].on_body)) {
                        continue;
                    }
                    const bool seen = std::any_of(shown.begin(), shown.end(), [&](const HitShape* s) {
                        return Length(s->p[0].pos - hit.p[0].pos) < 1e-3f &&
                               Length(s->p[1].pos - hit.p[1].pos) < 1e-3f &&
                               std::fabs(s->radius - hit.radius) < 1e-3f;
                    });
                    if (!seen) {
                        shown.push_back(&hit);
                    }
                }
            }
            {
                for (const HitShape* shape : shown) {
                    const HitShape hit = ScaleHitShape(config.physics.hit, *shape,
                                                       held[side]->hit_reach);
                    const Vec3 p1 = PointWorld(hit.p[0], rig.BodyWorld(), weapon_world[side]);
                    const Vec3 p2 = PointWorld(hit.p[1], rig.BodyWorld(), weapon_world[side]);
                    Capsule(out.lines, p1, hit.capsule ? p2 : p1, hit.radius * scale,
                            HitColor(hit.hit_type));
                }
            }
        }
    }
    // The player's hurtboxes: on the mirror copy (and on yourself with hurtboxes_self).
    if (config.show_hurtboxes && !hurtboxes.empty()) {
        const float scale = rig.BodyScale();
        for (int copy = 0; copy < 2; ++copy) {
            if (copy == 0 ? !config.show_hurtboxes_self : !show_mirror) {
                continue;
            }
            const Mat4 body_world = copy ? mirror * rig.BodyWorld() : rig.BodyWorld();
            for (const Hurtbox& box : hurtboxes) {
                const WireColor w = HurtGroupColor(box.part_group);
                const Color c{w.r, w.g, w.b};
                const Vec3 a = PointWorld(box.a, body_world, Mat4{});
                const Vec3 b = PointWorld(box.b, body_world, Mat4{});
                Capsule(out.lines, a, box.capsule ? b : a, box.radius * scale, c);
            }
        }
    }
    if (config.show_controllers) {
        for (int side = 0; side < 2; ++side) {
            if (last_input.grip[side].valid) {
                Axes(out.overlay, last_input.grip[side].position, last_input.grip[side].rotation,
                     0.08f);
            }
        }
    }
    DrawParry(parry, config.parry, last_input.time, last_input.head.position, out);
    physics_view.Draw(physics, config.physics, out); // props, enemy, colliders, ghost hands, hits
    if (config.throwables.enabled && config.throwables.show && throwables.Placed()) {
        const auto line = [&](Vec3 a, Vec3 b, const uint8_t* rgb) {
            Line(out.lines, a, b, Color{rgb[0], rgb[1], rgb[2]});
        };
        if (RackVisible(config)) {
            throwables.TableLines(line);
        }
        throwables.EffectLines(line, config.throwables);
        for (int side = 0; side < 2; ++side) { // the item in reach of an empty hand
            const int hover = throwables.HeldItem(side) < 0 ? throwables.HoverItem(side) : -1;
            if (hover >= 0 && (RackVisible(config) || throwables.Items()[size_t(hover)].state != Throwables::kRack)) {
                Cross(out.overlay, throwables.Items()[size_t(hover)].pos, 0.03f, kTarget);
            }
        }
    }
    if (config.consumables.Objects()) {
        const double now = consumables.Now();
        const auto rgb = [](const float c[3], float k = 1.0f) {
            const auto u = [&](float v) { return uint8_t(std::clamp(v * k, 0.0f, 1.0f) * 255.0f); };
            return Color{u(c[0]), u(c[1]), u(c[2])};
        };
        const bool head_ok = last_input.head.valid;
        const Vec3 head = last_input.head.position;
        if (config.consumables.labels && head_ok) { // the small count under each object
            for (size_t i = 0; i < consumables.Slots().size(); ++i) {
                if (!consumables.SlotVisible(int(i))) {
                    continue;
                }
                const Vec3 p = consumables.SlotPos(int(i), config.consumables.bob_m);
                Vec3 to_head = Normalize(Vec3{head.x - p.x, 0.0f, head.z - p.z}, {0, 0, 1});
                const Vec3 right = Normalize(Cross({0, 1, 0}, to_head), {1, 0, 0});
                const std::string text = consumables.Label(consumables.Slots()[i].entry);
                const float h = 0.016f, width = (float(text.size()) * 0.8f - 0.25f) * h * 0.55f * 1.0f;
                const Vec3 origin = p - right * (width * 0.5f * 1.8f) - Vec3{0, 0.05f, 0};
                const bool hover = consumables.Hover(0) == int(i) || consumables.Hover(1) == int(i);
                const Color c = hover ? kTarget : Color{205, 205, 215};
                AppendGlyphs(text, origin, right, {0, 1, 0}, h,
                             [&](Vec3 a, Vec3 b) { Line(out.overlay, a, b, c); });
            }
        }
        if (config.consumables.hud && head_ok) { // HP, statuses and the weapon buffs: a corner of the view
            const Quat q = last_input.head.rotation;
            const Vec3 r = Rotate(q, {1, 0, 0}), u = Rotate(q, {0, 1, 0}), f = Rotate(q, {0, 0, -1});
            const Vec3 o = head + f * 0.7f - r * 0.22f - u * 0.27f;
            const bool any = consumables.WheelOpen() || consumables.HpPct() < 99.9f ||
                             !consumables.Statuses().empty() || consumables.Buff(0).active ||
                             consumables.Buff(1).active || now - consumables.LastHealTime() < 3.0;
            if (any) {
                const float w = 0.16f, h = 0.012f;
                const auto bar = [&](float row, float frac, Color c, Color frame) {
                    const Vec3 a = o - u * (row * 0.02f);
                    const Vec3 b = a + r * w, d = a + u * h, e = b + u * h;
                    Line(out.overlay, a, b, frame);
                    Line(out.overlay, d, e, frame);
                    Line(out.overlay, a, d, frame);
                    Line(out.overlay, b, e, frame);
                    for (int k = 1; k <= 5; ++k) {
                        const float y = h * float(k) / 6.0f;
                        Line(out.overlay, a + u * y, a + u * y + r * (w * std::clamp(frac, 0.0f, 1.0f)), c);
                    }
                };
                const float hp = consumables.HpPct() / 100.0f;
                const bool flash = now - consumables.LastHealTime() < 0.6;
                bar(0.0f, hp, flash ? Color{255, 255, 255} : Color{uint8_t(255.0f * (1.0f - hp) + 60.0f * hp), uint8_t(60.0f + 160.0f * hp), 70},
                    Color{200, 200, 200});
                float row = 1.0f;
                for (const BodyStatus& s : consumables.Statuses()) {
                    bar(row++, float((s.until - now) / double(std::max(s.duration, 0.01f))), rgb(s.color), rgb(s.color, 0.6f));
                }
                for (int side = 0; side < 2; ++side) {
                    const WeaponBuff& b = consumables.Buff(side);
                    if (b.active) {
                        bar(row++, float((b.until - now) / double(std::max(b.duration, 0.01f))), rgb(b.color), Color{230, 230, 230});
                    }
                }
            }
        }
        // The rub in progress: the blade zone to rub (dim) and what is covered so far (glowing).
        for (int hand = 0; hand < 2; ++hand) {
            const Consumables::Held& h = consumables.HeldBy(hand);
            if (h.entry < 0 || h.via_throwables || consumables.DefOf(h.entry).gesture != CGesture::Rub) {
                continue;
            }
            const int side = h.target >= 0 ? h.target : 1 - hand;
            const int slot = weapon_slot[side];
            if (slot < 0 || !weapons[size_t(slot)].loaded || consumables.WeaponStowed(side, config.consumables)) {
                continue;
            }
            const Weapon& w = weapons[size_t(slot)];
            const Mat4 world = physics.WeaponPose(side, WeaponWorld(side, config));
            const Vec3 base = TransformPoint(world, {0, 0, 0}), tip = TransformPoint(world, w.long_axis * w.reach);
            const float start = config.consumables.blade_start;
            const Color dim = rgb(consumables.DefOf(h.entry).color, 0.45f);
            const Color bright = rgb(consumables.DefOf(h.entry).color, 1.0f);
            const Vec3 axis = Normalize(tip - base, {0, 1, 0});
            const Vec3 side_a = Normalize(Cross(axis, {0, 1, 0}), {1, 0, 0});
            const Vec3 side_b = Normalize(Cross(axis, side_a), {0, 1, 0});
            const float r = config.consumables.contact_radius_m * 0.5f;
            for (const Vec3& off : {side_a * r, side_a * -r, side_b * r, side_b * -r}) {
                Line(out.overlay, Lerp(base, tip, start) + off, tip + off, dim);
            }
            if (h.rub_hi > h.rub_lo) {
                const float pulse = 0.7f + 0.3f * std::sin(float(now) * 20.0f);
                const Color glow = rgb(consumables.DefOf(h.entry).color, pulse);
                for (const Vec3& off : {Vec3{0, 0, 0}, side_a * (r * 0.5f), side_a * (-r * 0.5f), side_b * (r * 0.5f), side_b * (-r * 0.5f)}) {
                    Line(out.overlay, Lerp(base, tip, h.rub_lo) + off, Lerp(base, tip, h.rub_hi) + off, off.x == 0 && off.y == 0 && off.z == 0 ? bright : glow);
                }
            }
            // The progress towards the coverage needed: a gauge beside the blade.
            const float prog = std::clamp(consumables.RubProgress(hand) / std::max(config.consumables.coverage, 0.05f), 0.0f, 1.0f);
            const Vec3 g0 = base + side_a * (r * 3.0f), g1 = tip + side_a * (r * 3.0f);
            Line(out.overlay, g0, g1, dim);
            Line(out.overlay, g0, Lerp(g0, g1, prog), bright);
        }
        // Sparks along a buffed weapon.
        for (int side = 0; side < 2; ++side) {
            const WeaponBuff& b = consumables.Buff(side);
            const int slot = weapon_slot[side];
            if (!b.active || slot < 0 || !weapons[size_t(slot)].loaded ||
                consumables.WeaponStowed(side, config.consumables)) {
                continue;
            }
            const Weapon& w = weapons[size_t(slot)];
            const Mat4 world = physics.WeaponPose(side, WeaponWorld(side, config));
            const Vec3 base = TransformPoint(world, {0, 0, 0}), tip = TransformPoint(world, w.long_axis * w.reach);
            const Color c = rgb(b.color);
            const int frame = int(now * 24.0);
            for (int k = 0; k < 8; ++k) {
                uint32_t x = uint32_t(frame * 31 + k * 977 + side * 13) * 2654435761u;
                const auto rnd = [&](int n) { x = x * 1664525u + 1013904223u; return float((x >> 8) & 0xFFFF) / 65535.0f * (n ? 1.0f : 1.0f); };
                const float t = 0.2f + 0.8f * rnd(0);
                const Vec3 p = Lerp(base, tip, t);
                const Vec3 d{rnd(0) - 0.5f, rnd(0) - 0.2f, rnd(0) - 0.5f};
                const Vec3 e = p + d * (b.element == "bolt" ? 0.12f : 0.07f);
                Line(out.overlay, p, e, c);
                if (b.element == "bolt") { // a forked bolt
                    Line(out.overlay, e, e + Vec3{rnd(0) - 0.5f, rnd(0) - 0.5f, rnd(0) - 0.5f} * 0.08f, c);
                }
            }
        }
    }
    if (config.show_laser && last_input.aim[1].valid) {
        const Vec3 o = last_input.aim[1].position;
        const Vec3 d = Rotate(last_input.aim[1].rotation, {0, 0, -1});
        float length = 3.0f;
        if (d.y < -1e-3f) {
            length = std::min(length, (floor_y - o.y) / d.y); // stop on the floor
        }
        Line(out.lines, o, o + d * length, kLaser);
        Cross(out.lines, o + d * length, 0.02f, kLaser);
    }
}

} // namespace sb
