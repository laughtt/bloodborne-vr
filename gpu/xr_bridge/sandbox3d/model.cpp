// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "model.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace sb {

std::string ReadText(const std::filesystem::path& path, bool* ok) {
    std::ifstream file(path, std::ios::binary);
    if (ok) {
        *ok = bool(file);
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

namespace {

m3::Vec3 Vec(const json::Value& v, m3::Vec3 fallback = {}) {
    if (!v.IsArray() || v.Size() < 3) {
        return fallback;
    }
    return {v[0].Float(), v[1].Float(), v[2].Float()};
}

m3::Mat4 Matrix(const json::Value& v) {
    m3::Mat4 m;
    if (v.IsArray() && v.Size() == 16) {
        for (int i = 0; i < 16; ++i) {
            m.m[i] = v[size_t(i)].Float();
        }
    }
    return m;
}

} // namespace

int Model::FindBone(const std::string& bone_name) const {
    for (size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == bone_name) {
            return int(i);
        }
    }
    return -1;
}

std::vector<const Dummy*> Model::FindDummies(int ref) const {
    std::vector<const Dummy*> out;
    for (const Dummy& d : dummies) {
        if (d.ref == ref) {
            out.push_back(&d);
        }
    }
    return out;
}

bool LoadModel(const std::filesystem::path& json_path, Model& out, std::string& error) {
    bool ok = false;
    const std::string text = ReadText(json_path, &ok);
    if (!ok) {
        error = "cannot read " + json_path.string();
        return false;
    }
    std::string parse_error;
    json::Value doc = json::Parse(text, &parse_error);
    if (!parse_error.empty() || doc["format"].Str() != "bbmodel") {
        error = json_path.string() + ": not a bbmodel (" + parse_error + ")";
        return false;
    }
    out = {};
    out.path = json_path;
    out.name = doc["name"].Str();
    out.kind = doc["kind"].Str();
    for (const json::Value& b : doc["bones"].array) {
        Bone bone;
        bone.name = b["name"].Str();
        bone.parent = b["parent"].Int(-1);
        bone.bind_world = Matrix(b["bind_world"]);
        bone.inv_bind = m3::Inverse(bone.bind_world);
        bone.deform = b["deform"].Bool(true);
        out.bones.push_back(bone);
    }
    for (const json::Value& d : doc["dummies"].array) {
        Dummy dummy;
        dummy.ref = d["ref"].Int();
        dummy.parent = d["parent"].Int(-1);
        dummy.attach = d["attach"].Int(-1);
        dummy.position = Vec(d["position"]);
        dummy.forward = Vec(d["forward"], {0, 0, -1});
        dummy.upward = Vec(d["upward"], {0, 1, 0});
        out.dummies.push_back(dummy);
    }
    for (const json::Value& m : doc["meshes"].array) {
        MeshRange mesh;
        mesh.name = m["name"].Str();
        mesh.part = m["part"].Str();
        mesh.slot = m["slot"].Str();
        mesh.material = m["material"].Str();
        mesh.texture = m["texture"].Str();
        mesh.mask = m["mask"].Int(-1);
        mesh.hidden = m["hidden"].Bool(false);
        mesh.dominant_bone = m["dominant_bone"].Int(-1);
        mesh.dominant_bone_name = m["dominant_bone_name"].Str();
        mesh.first_index = uint32_t(m["first_index"].Number());
        mesh.index_count = uint32_t(m["index_count"].Number());
        mesh.first_vertex = uint32_t(m["first_vertex"].Number());
        mesh.vertex_count = uint32_t(m["vertex_count"].Number());
        out.meshes.push_back(mesh);
    }
    const auto bin_path = json_path.parent_path() / doc["bin"].Str();
    std::ifstream bin(bin_path, std::ios::binary);
    if (!bin) {
        error = "cannot read " + bin_path.string();
        return false;
    }
    const size_t vertex_count = size_t(doc["vertex_count"].Number());
    const size_t index_count = size_t(doc["index_count"].Number());
    const size_t stride = size_t(doc["vertex_layout"]["stride"].Number(56));
    if (stride != sizeof(Vertex)) {
        error = bin_path.string() + ": vertex stride " + std::to_string(stride);
        return false;
    }
    out.vertices.resize(vertex_count);
    out.indices.resize(index_count);
    bin.seekg(std::streamoff(doc["vertices_offset"].Number()));
    bin.read(reinterpret_cast<char*>(out.vertices.data()),
             std::streamsize(vertex_count * sizeof(Vertex)));
    bin.seekg(std::streamoff(doc["indices_offset"].Number()));
    bin.read(reinterpret_cast<char*>(out.indices.data()),
             std::streamsize(index_count * sizeof(uint32_t)));
    if (!bin) {
        error = bin_path.string() + ": truncated";
        return false;
    }
    for (const Vertex& v : out.vertices) {
        for (uint16_t b : v.bones) {
            if (b >= out.bones.size() && !out.bones.empty()) {
                error = json_path.string() + ": vertex bone index out of range";
                return false;
            }
        }
    }
    for (uint32_t i : out.indices) {
        if (i >= vertex_count) {
            error = json_path.string() + ": index out of range";
            return false;
        }
    }
    out.doc = std::move(doc);
    return true;
}

bool LoadAssetIndex(const std::filesystem::path& dir, AssetIndex& out, std::string& error) {
    bool ok = false;
    const std::string text = ReadText(dir / "assets.json", &ok);
    if (!ok) {
        error = "no " + (dir / "assets.json").string() +
                " (run scripts/vr_assets/extract.py first)";
        return false;
    }
    std::string parse_error;
    const json::Value doc = json::Parse(text, &parse_error);
    if (!parse_error.empty()) {
        error = "assets.json: " + parse_error;
        return false;
    }
    out = {};
    out.dir = dir;
    out.body_file = doc["body"].Str();
    for (const json::Value& w : doc["weapons"].array) {
        out.weapons.push_back({w["file"].Str(), w["name"].Str(), w["model"].Str(), w["id"].Int()});
    }
    return true;
}

namespace {

/// The bone of a bbhitbox point: a name (the extractor's), or an index in older files.
void ReadBone(const json::Value& bone, ShapePoint& out) {
    if (bone.IsString()) {
        out.bone_name = bone.Str();
    } else if (bone.IsNumber()) {
        out.bone = bone.Int(-1);
    }
}

/// A bbhitbox hit point: its position, whether it sits on the body, the bone it follows.
bool HitPoint(const json::Value& p, ShapePoint& out) {
    if (!p.IsObject() || p["unresolved"].Bool(false) || !p["pos"].IsArray()) {
        return false;
    }
    out.pos = Vec(p["pos"]);
    out.on_body = p["src"].Str() == "body";
    ReadBone(p["bone"], out);
    if (p["pos_local"].IsArray()) {
        out.local = Vec(p["pos_local"]);
        out.has_local = true;
    }
    return true;
}

} // namespace

void ResolveBone(const Model& body, ShapePoint& point) {
    if (!point.bone_name.empty()) {
        point.bone = body.FindBone(point.bone_name);
    } else if (point.bone >= int(body.bones.size())) {
        point.bone = -1;
    }
}

std::vector<Hurtbox> LoadHurtboxes(const std::filesystem::path& assets_dir, const std::string& chr,
                                   std::string& log) {
    std::vector<Hurtbox> out;
    bool ok = false;
    const auto path = assets_dir / "hitbox" / ("chr_" + chr + ".json");
    const std::string text = ReadText(path, &ok);
    if (!ok) {
        log = "no hurtboxes (" + path.string() + ")";
        return out;
    }
    const json::Value doc = json::Parse(text);
    for (const json::Value& h : doc["hurtboxes"].array) {
        if (!h["a"].IsArray()) {
            continue; // convex bodies without a/b: not drawn
        }
        Hurtbox box;
        box.name = h["body"].Str();
        box.radius = h["radius"].Float(0.05f);
        box.capsule = h["shape"].Str() == "capsule";
        box.part_group = h["part_group"].Int(0);
        ShapePoint* points[2] = {&box.a, &box.b};
        const char* keys[2] = {"a", "b"};
        for (int k = 0; k < 2; ++k) {
            ShapePoint& p = *points[k];
            const json::Value& pos = h[keys[k]].IsArray() ? h[keys[k]] : h["a"];
            p.pos = Vec(pos);
            p.on_body = true;
            ReadBone(h["bone"], p);
            const json::Value& local = h[std::string(keys[k]) + "_local"];
            if (local.IsArray()) {
                p.local = Vec(local);
                p.has_local = true;
            }
        }
        out.push_back(box);
    }
    log = "hurtboxes " + path.string() + ": " + std::to_string(out.size());
    return out;
}

Hitboxes LoadHitboxes(const std::filesystem::path& assets_dir, const Model& weapon, int weapon_id,
                      float placeholder_radius, std::string& log) {
    Hitboxes out;
    const auto index_path = assets_dir / "hitbox" / "index.json";
    bool ok = false;
    const std::string index_text = ReadText(index_path, &ok);
    if (ok) {
        const json::Value index = json::Parse(index_text);
        std::string file;
        for (const json::Value& w : index["weapons"].array) {
            if (w["model"].Str() == weapon.name && (file.empty() || w["id"].Int() == weapon_id)) {
                file = w["file"].Str();
            }
        }
        if (!file.empty()) {
            const json::Value doc = json::Parse(ReadText(assets_dir / "hitbox" / file));
            for (const json::Value& a : doc["attacks"].array) {
                Attack attack;
                attack.name = "judge " + std::to_string(a["judge"].Int()) + " " +
                              a["name_jp"].Str() + " (" + a["form"].Str() + ")";
                attack.form = a["form"].Str();
                attack.atk_id = a["ref_id"].Int(-1);
                for (const json::Value& h : a["hits"].array) {
                    HitShape shape;
                    shape.radius = h["radius"].Float(0.05f);
                    shape.hit_type = h["hit_type"].Str();
                    if (!HitPoint(h["p1"], shape.p[0])) {
                        continue;
                    }
                    shape.p[1] = shape.p[0];
                    ShapePoint second;
                    if (h["shape"].Str() == "capsule" && HitPoint(h["p2"], second)) {
                        shape.p[1] = second;
                        shape.capsule = true;
                    }
                    attack.hits.push_back(shape);
                }
                if (!attack.hits.empty()) {
                    out.attacks.push_back(std::move(attack));
                }
            }
            if (!out.attacks.empty()) {
                out.source = (assets_dir / "hitbox" / file).string();
                log = "hitboxes " + out.source + ": " + std::to_string(out.attacks.size()) +
                      " attacks";
                return out;
            }
        }
    }
    // Placeholder: a capsule along the blade dummies, 100 to the farthest 1xx dummy from it.
    const auto base = weapon.FindDummies(100);
    if (!base.empty()) {
        const m3::Vec3 a = base[0]->position;
        m3::Vec3 b = a;
        float best = 0.0f;
        for (const Dummy& d : weapon.dummies) {
            if (d.ref > 100 && d.ref < 200 && m3::Length(d.position - a) > best) {
                best = m3::Length(d.position - a);
                b = d.position;
            }
        }
        Attack attack;
        attack.name = "placeholder (dummies 100..1xx)";
        HitShape shape;
        shape.p[0].pos = a;
        shape.p[1].pos = b;
        shape.radius = placeholder_radius;
        shape.capsule = best > 0.0f;
        shape.hit_type = "placeholder";
        attack.hits.push_back(shape);
        out.attacks.push_back(attack);
    }
    out.source = "placeholder";
    log = "hitboxes: no " + index_path.string() + " entry for " + weapon.name +
          ", placeholder capsule along dummies 100+";
    return out;
}

} // namespace sb
