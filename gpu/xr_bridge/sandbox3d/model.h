// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The extracted models (scripts/vr_assets/bbmodel.py: JSON + .bin under out/vr_assets), the asset
// index (assets.json) and weapon hitboxes (scripts/vr_assets/hitbox_extract.py, "bbhitbox").

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "json.h"
#include "math3d.h"

namespace sb {

struct Bone {
    std::string name;
    int parent = -1;
    m3::Mat4 bind_world; // model space, right-handed
    m3::Mat4 inv_bind;
    bool deform = true;
};

struct Dummy {
    int ref = 0;
    int parent = -1; // the bone the position was given relative to (already applied)
    int attach = -1; // the bone the dummy follows
    m3::Vec3 position, forward, upward; // bind pose, model space
};

struct MeshRange {
    std::string name, part, slot, material;
    int mask = -1;
    bool hidden = false;  // masked by the outfit (EquipParamProtector invisibleFlag)
    int dominant_bone = -1;
    std::string dominant_bone_name;
    uint32_t first_index = 0, index_count = 0, first_vertex = 0, vertex_count = 0;
};

/// Vertex as stored (bbmodel.VERTEX, 56 bytes).
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    uint16_t bones[4];
    float weights[4];
};
static_assert(sizeof(Vertex) == 56, "bbmodel vertex layout");

struct Model {
    std::string name, kind; // kind "body" or "weapon"
    std::filesystem::path path;
    std::vector<Bone> bones;
    std::vector<Dummy> dummies;
    std::vector<MeshRange> meshes;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    json::Value doc; // the whole description (weapon names, outfit, ...)

    int FindBone(const std::string& bone_name) const;
    /// The dummies with a reference id (weapons often have one per id; bodies several).
    std::vector<const Dummy*> FindDummies(int ref) const;
};

bool LoadModel(const std::filesystem::path& json_path, Model& out, std::string& error);

struct WeaponEntry {
    std::string file, name, model;
    int id = 0;
};

struct AssetIndex {
    std::filesystem::path dir;
    std::string body_file;
    std::vector<WeaponEntry> weapons;
};

bool LoadAssetIndex(const std::filesystem::path& dir, AssetIndex& out, std::string& error);

/// A point of a hit or hurt shape (bbhitbox): model space at bind pose of the weapon or of the
/// body; body points follow a bone, named in the JSON (resolved against the body by
/// ResolveBone), with their position in that bone's bind space when the extractor gave it.
struct ShapePoint {
    m3::Vec3 pos;
    m3::Vec3 local;
    bool has_local = false;
    bool on_body = false;
    std::string bone_name; // "" = the model root (static in model space)
    int bone = -1;
};

/// One hit shape of an attack: a capsule p[0]..p[1] or a sphere at p[0] (p[1] == p[0]).
struct HitShape {
    ShapePoint p[2];
    float radius = 0.05f;
    bool capsule = false;
    std::string hit_type;  // tip / middle / root
};

/// A ragdoll hurtbox of a character (bbhitbox chr file): capsule a..b or sphere at a.
struct Hurtbox {
    std::string name;
    ShapePoint a, b;
    float radius = 0.05f;
    bool capsule = true;
    int part_group = 0; // player: 1 torso, 2 arms, 3 legs, 31 head
};

struct Attack {
    std::string name;      // judge id and Japanese row name
    std::string form;      // trick weapon form: "A", "B", "A/B" or "" (unknown)
    int atk_id = -1;       // the AtkParam id (BehaviorParam refId; the game's stamina tickets)
    std::vector<HitShape> hits;
};

struct Hitboxes {
    std::string source;    // file, or "placeholder"
    std::vector<Attack> attacks;
};

/// The hit shapes of a weapon: <assets>/hitbox/index.json (bbhitbox, the hitbox agent's
/// extractor) when it has the weapon, else a placeholder capsule along the blade dummies
/// (100 to the farthest 1xx dummy).
Hitboxes LoadHitboxes(const std::filesystem::path& assets_dir, const Model& weapon, int weapon_id,
                      float placeholder_radius, std::string& log);

/// The hurtboxes of a character (<assets>/hitbox/chr_<chr>.json), bones by name.
std::vector<Hurtbox> LoadHurtboxes(const std::filesystem::path& assets_dir, const std::string& chr,
                                   std::string& log);

/// Looks a body point's bone name up in the body (sets `bone`; -1 when unnamed or unknown).
void ResolveBone(const Model& body, ShapePoint& point);

std::string ReadText(const std::filesystem::path& path, bool* ok = nullptr);

} // namespace sb
