// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The held item as a 3D object (docs/vr/findings/39-physical-items-plan.md, phase 2a): the extracted
// consumable / throwable meshes (scripts/vr_assets/{consumables,throwables}_extract.py, under
// <assets>/consumables and <assets>/throwables), drawn at the grip pose into the game UI's canvas as
// lit, depth-sorted triangles (the UI pipeline has no depth buffer write and no mesh path: a held item
// is a few hundred triangles, sorted on the CPU each frame).
//
// The goods -> mesh table is the sandbox's own (sandbox3d/throw_config.cpp DefaultThrowItems and
// consumables_config.cpp DefaultConsumables: mesh name, scale, how it sits in the hand, colour), not a
// copy. Goods without a mesh file (pellets, elixirs, papers, syringes, Blacksky Eye...) fall back to
// a procedural stand-in per hold category (card, pellet, bottle, box: StandIn), so something item-shaped
// is always in the hand; the coloured disc of GameUi::DrawHeldItem stays as the last resort. Beyond those rows, <assets>/items/items.json
// (scripts/vr_assets/items_extract.py, --items) lists a mesh for every goods the game has any model for
// (items/<goods>.json) with its size in metres and a grip category: MeshFor(assets, goods) falls back to
// it with a default pose and a scale from the bounding box; the sandbox rows stay authoritative. Goods
// the game has no model for keep the disc (the index's "icon" says where their menu icon is, the card
// fallback). Pure CPU, no I/O except the mesh load: unit tested in
// vr_game_ui_test.cpp.

#pragma once

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "sandbox3d/consumables_config.h"
#include "sandbox3d/item_sheet.h"
#include "sandbox3d/json.h"
#include "sandbox3d/model.h"
#include "sandbox3d/throw_config.h"
#include "sandbox3d/ui_render.h"
#include "vr_hand_model.h"
#include "vr_held_items.h"

namespace vrui {
namespace held {

/// How one goods looks in the hand (from the sandbox's tables).
struct MeshSpec {
    int goods = 0;
    std::string file;          // under the assets: "throwables/th_1200.json"
    float scale = 1.0f;        // mesh scale (the extracted meshes are in metres)
    m3::Vec3 pos;              // item centre relative to the grip pose (sandbox value)
    m3::Vec3 rot_deg;          // Euler degrees, X then Y then Z, relative to the grip pose
    sb::UiColor color;
};

/// The in-game adjustments on top of the sandbox's pose, per kind of item (the one place to tune
/// how an item sits in the hand): `pos` in the grip frame, `rot_deg` extra Euler degrees.
struct KindTweak {
    Kind kind;
    m3::Vec3 pos;
    m3::Vec3 rot_deg;
};
inline const KindTweak kKindTweaks[] = {
    {Kind::Mouth, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}, // vials: upright in the fist, like the sandbox
    {Kind::Blade, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}, // (papers have no mesh yet: the disc)
    {Kind::Throw, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}, // bottles upright, knives along the fist
};

inline const KindTweak& TweakFor(Kind kind) {
    for (const KindTweak& t : kKindTweaks) {
        if (t.kind == kind) {
            return t;
        }
    }
    return kKindTweaks[0];
}

/// Every goods the sandbox gives a mesh name (a file may still be missing from the install).
inline const std::vector<MeshSpec>& MeshTable() {
    static const std::vector<MeshSpec> table = [] {
        std::vector<MeshSpec> v;
        for (const sb::ThrowItemDef& d : sb::DefaultThrowItems()) {
            if (d.mesh.empty()) {
                continue;
            }
            MeshSpec s;
            s.goods = d.goods;
            s.file = "throwables/" + d.mesh + ".json";
            s.scale = d.scale;
            s.pos = d.grip.position;
            s.rot_deg = d.grip.rotation_deg;
            s.color = {d.color[0], d.color[1], d.color[2], 1.0f};
            v.push_back(s);
        }
        for (const sb::ConsumableDef& d : sb::DefaultConsumables()) {
            if (d.mesh.empty() || d.group == sb::CGroup::Throwable) {
                continue;
            }
            MeshSpec s;
            s.goods = d.goods;
            s.file = "consumables/" + d.mesh + ".json";
            s.scale = d.scale;
            s.pos = d.hold;
            s.rot_deg = d.hold_rot_deg;
            s.color = {d.color[0], d.color[1], d.color[2], 1.0f};
            v.push_back(s);
        }
        return v;
    }();
    return table;
}

/// The mesh of a goods row, null when the sandbox has none for it.
inline const MeshSpec* MeshFor(int goods) {
    for (const MeshSpec& s : MeshTable()) {
        if (s.goods == goods) {
            return &s;
        }
    }
    return nullptr;
}

// ---- items/items.json: the fallback for goods the sandbox tables do not cover ----

/// Default look per grip category of items.json: `cap` is the longest side (m) a hand holds
/// comfortably (the mesh is scaled down to it, and up to half of it), `color` the flat tint.
struct GripLook {
    const char* grip;
    float cap;
    sb::UiColor color;
};
inline const GripLook kGripLooks[] = {
    {"bottle", 0.22f, {0.75f, 0.35f, 0.3f, 1.0f}}, {"small", 0.10f, {0.8f, 0.78f, 0.7f, 1.0f}},
    {"paper", 0.18f, {0.9f, 0.85f, 0.7f, 1.0f}},   {"bell", 0.16f, {0.85f, 0.7f, 0.35f, 1.0f}},
    {"tool", 0.20f, {0.75f, 0.78f, 0.85f, 1.0f}},  {"weapon", 0.45f, {0.85f, 0.88f, 0.95f, 1.0f}},
};

inline const GripLook& GripLookFor(const std::string& grip) {
    for (const GripLook& g : kGripLooks) {
        if (grip == g.grip) {
            return g;
        }
    }
    return kGripLooks[1];
}

/// The spec of a goods row of items.json: mesh file, scale from the bounding box (size_m) into the
/// grip category's size range, upright (a long X axis is turned to point forward, like the knives).
inline bool SpecFromItemsRow(const json::Value& row, MeshSpec& out) {
    const std::string& mesh = row["mesh"].Str();
    if (mesh.empty()) {
        return false;
    }
    const GripLook& look = GripLookFor(row["grip"].Str());
    const float sx = row["size_m"][0].Float(0.1f), sy = row["size_m"][1].Float(0.1f),
                sz = row["size_m"][2].Float(0.1f);
    const float longest = std::max({sx, sy, sz, 1e-4f});
    float scale = 1.0f;
    if (longest > look.cap) {
        scale = look.cap / longest;
    } else if (longest < look.cap * 0.5f) {
        scale = look.cap * 0.5f / longest;
    }
    out = {};
    out.goods = row["id"].Int();
    out.file = "items/" + mesh;
    out.scale = scale;
    out.color = look.color;
    if (sx > 1.6f * std::max(sy, sz)) {
        out.rot_deg = {0.0f, 90.0f, 0.0f};
    }
    return out.goods != 0;
}

/// items/items.json of an assets folder, parsed once per folder (an empty table when it is missing).
class ItemsIndex {
public:
    const MeshSpec* Find(const std::filesystem::path& assets, int goods) {
        if (!loaded || assets != loaded_from) {
            Load(assets);
        }
        const auto it = specs.find(goods);
        return it == specs.end() ? nullptr : &it->second;
    }
    size_t Size() const { return specs.size(); }

private:
    bool loaded = false;
    std::filesystem::path loaded_from;
    std::map<int, MeshSpec> specs;

    void Load(const std::filesystem::path& assets) {
        loaded = true;
        loaded_from = assets;
        specs.clear();
        bool ok = false;
        const std::string text = sb::ReadText(assets / "items" / "items.json", &ok);
        if (!ok) {
            return;
        }
        const json::Value doc = json::Parse(text);
        const json::Value& items = doc["items"];
        for (size_t i = 0; i < items.Size(); ++i) {
            MeshSpec s;
            if (SpecFromItemsRow(items[i], s)) {
                specs[s.goods] = s;
            }
        }
    }
};

/// The mesh of a goods row: the sandbox's own table first (authoritative), else the items.json fallback
/// under `assets`; null when the game has no model for it (the caller draws the disc / icon card).
inline const MeshSpec* MeshFor(const std::filesystem::path& assets, int goods) {
    if (const MeshSpec* s = MeshFor(goods)) {
        return s;
    }
    static ItemsIndex index;
    return index.Find(assets, goods);
}

// ---- Stand-ins: a procedural model per hold category for goods the game has no model for ----

namespace standin_detail {
inline sb::Vertex MakeVertex(m3::Vec3 p, m3::Vec3 n) {
    sb::Vertex v{};
    v.position[0] = p.x, v.position[1] = p.y, v.position[2] = p.z;
    v.normal[0] = n.x, v.normal[1] = n.y, v.normal[2] = n.z;
    v.weights[0] = 1.0f;
    return v;
}
/// A flat convex polygon (a fan) with its own vertices and normal, wound outward from the model's
/// origin (every stand-in is convex around its centre).
inline void AddPoly(sb::Model& m, const std::vector<m3::Vec3>& pts) {
    using namespace m3;
    Vec3 c{};
    for (const Vec3& p : pts) {
        c = c + p;
    }
    c = c * (1.0f / float(pts.size()));
    Vec3 n = Normalize(Cross(pts[1] - pts[0], pts[2] - pts[0]), {0, 1, 0});
    const bool flip = Dot(n, c) < 0.0f;
    if (flip) {
        n = -n;
    }
    const uint32_t base = uint32_t(m.vertices.size());
    for (const Vec3& p : pts) {
        m.vertices.push_back(MakeVertex(p, n));
    }
    for (uint32_t i = 1; i + 1 < pts.size(); ++i) {
        m.indices.push_back(base);
        m.indices.push_back(base + (flip ? i + 1 : i));
        m.indices.push_back(base + (flip ? i : i + 1));
    }
}
/// A box centred at the origin.
inline void AddBox(sb::Model& m, float sx, float sy, float sz) {
    using namespace m3;
    const float x = sx * 0.5f, y = sy * 0.5f, z = sz * 0.5f;
    const Vec3 c[8] = {{-x, -y, -z}, {x, -y, -z}, {x, y, -z}, {-x, y, -z},
                       {-x, -y, z},  {x, -y, z},  {x, y, z},  {-x, y, z}};
    static const int f[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
    for (const auto& q : f) {
        AddPoly(m, {c[q[0]], c[q[1]], c[q[2]], c[q[3]]});
    }
}
/// A thin card: two textured faces (the icon's middle part: a square picture on a card 0.68 as wide as
/// tall; the icon's transparent background leaves just the picture) as the one mesh range ("card": MeshCache::Resolve gives it the goods' icon), the edges flat.
inline void AddCard(sb::Model& m, float w, float h, float t) {
    using namespace m3;
    const float x = w * 0.5f, y = h * 0.5f, z = t * 0.5f;
    const float u0 = 0.14f, u1 = 0.86f;
    sb::MeshRange face;
    face.name = "card";
    face.texture = "card";
    face.first_index = 0;
    for (int side = 0; side < 2; ++side) {
        const float sz = side ? -z : z;
        const Vec3 n{0.0f, 0.0f, side ? -1.0f : 1.0f};
        const Vec3 p[4] = {{-x, y, sz}, {x, y, sz}, {x, -y, sz}, {-x, -y, sz}};
        const float uu[4] = {side ? u1 : u0, side ? u0 : u1, side ? u0 : u1, side ? u1 : u0};
        const float vv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        const uint32_t base = uint32_t(m.vertices.size());
        for (int k = 0; k < 4; ++k) {
            sb::Vertex v = MakeVertex(p[k], n);
            v.uv[0] = uu[k], v.uv[1] = vv[k];
            m.vertices.push_back(v);
        }
        // counter-clockwise seen from the face's normal
        for (const uint32_t i : side ? std::initializer_list<uint32_t>{0u, 1u, 2u, 0u, 2u, 3u}
                                     : std::initializer_list<uint32_t>{0u, 2u, 1u, 0u, 3u, 2u}) {
            m.indices.push_back(base + i);
        }
    }
    face.index_count = uint32_t(m.indices.size());
    face.vertex_count = uint32_t(m.vertices.size());
    m.meshes.push_back(face);
}
/// An n-sided prism along Y from y0 to y1 (radius r); `bottom` / `top` add the caps.
inline void AddPrism(sb::Model& m, int n, float r, float y0, float y1, bool bottom, bool top) {
    using namespace m3;
    const float two_pi = 6.2831853f;
    std::vector<Vec3> lo(static_cast<size_t>(n)), hi(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float a = two_pi * (float(i) + 0.5f) / float(n);
        lo[size_t(i)] = {r * std::cos(a), y0, r * std::sin(a)};
        hi[size_t(i)] = {r * std::cos(a), y1, r * std::sin(a)};
    }
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        AddPoly(m, {lo[size_t(i)], lo[size_t(j)], hi[size_t(j)], hi[size_t(i)]});
    }
    if (bottom) {
        AddPoly(m, lo);
    }
    if (top) {
        AddPoly(m, hi);
    }
}
/// An n-sided bipyramid (a faceted pellet): ring radius r, apexes at +-h.
inline void AddBipyramid(sb::Model& m, int n, float r, float h) {
    using namespace m3;
    const float two_pi = 6.2831853f;
    for (int i = 0; i < n; ++i) {
        const float a = two_pi * float(i) / float(n), b = two_pi * float(i + 1) / float(n);
        const Vec3 p{r * std::cos(a), 0.0f, r * std::sin(a)}, q{r * std::cos(b), 0.0f, r * std::sin(b)};
        AddPoly(m, {p, q, {0, h, 0}});
        AddPoly(m, {q, p, {0, -h, 0}});
    }
}
} // namespace standin_detail

inline sb::Model BuildStandIn(hand::HoldCategory cat) {
    using namespace standin_detail;
    sb::Model m;
    m.kind = "standin";
    switch (cat) {
    case hand::HoldCategory::Paper:
        m.name = "standin_paper";
        AddCard(m, 0.075f, 0.11f, 0.003f); // a thin card, the long side along +Y; its faces carry the menu icon
        break;
    case hand::HoldCategory::Small:
        m.name = "standin_small";
        AddBipyramid(m, 8, 0.011f, 0.013f); // a faceted pellet, ~0.022 m
        break;
    case hand::HoldCategory::Bottle:
        m.name = "standin_bottle";
        AddPrism(m, 8, 0.0175f, -0.05f, 0.03f, true, true); // the body, 0.035 wide
        AddPrism(m, 8, 0.008f, 0.03f, 0.05f, false, true);  // the neck: +Y is the mouth, 0.10 tall in all
        break;
    case hand::HoldCategory::Bell:
        m.name = "standin_bell";
        AddBox(m, 0.05f, 0.09f, 0.05f);
        break;
    case hand::HoldCategory::Tool:
    case hand::HoldCategory::Weapon:
        m.name = "standin_tool";
        AddBox(m, 0.02f, 0.12f, 0.02f);
        break;
    }
    return m;
}

/// The stand-in of a hold category (built once).
inline const sb::Model& StandIn(hand::HoldCategory cat) {
    static const std::vector<sb::Model> models = [] {
        std::vector<sb::Model> v;
        for (int c = 0; c <= int(hand::HoldCategory::Weapon); ++c) {
            v.push_back(BuildStandIn(hand::HoldCategory(c)));
        }
        return v;
    }();
    return models[size_t(cat)];
}

/// The stand-in's tint: by what the item does (drinks red, weapon buffs orange: Fire Paper red-orange,
/// Bolt Paper pale blue; throwables blue), else the category's look (bells gold, marks and tools steel).
inline sb::UiColor StandInColor(int goods, hand::HoldCategory cat) {
    if (goods == 1300) {
        return {1.0f, 0.35f, 0.15f, 1.0f};
    }
    if (goods == 1320) {
        return {0.65f, 0.8f, 1.0f, 1.0f};
    }
    const ItemInfo info = Classify(BBXR_ITEM_GOODS | uint32_t(goods));
    if (info.kind == Kind::Mouth && !info.bell) {
        return {0.85f, 0.08f, 0.1f, 1.0f};
    }
    if (info.kind == Kind::Blade) {
        return {1.0f, 0.55f, 0.1f, 1.0f};
    }
    if (info.kind == Kind::Throw) {
        return {0.3f, 0.7f, 1.0f, 1.0f};
    }
    return GripLookFor(hand::CategoryName(cat)).color;
}

/// The in-game size of a held item: its longest side in metres (a real item's size, not the
/// sandbox's); 0 = no specific target (the category's cap applies).
inline float InGameTarget(int goods, bool standin, hand::HoldCategory cat) {
    static const struct {
        int lo, hi;
        float m;
    } kTargets[] = {
        {701, 706, 0.11f},   {1000, 1000, 0.11f}, {1100, 1101, 0.11f}, {1120, 1120, 0.12f},
        {2030, 2030, 0.09f}, {1200, 1201, 0.18f}, {1250, 1251, 0.18f}, {1260, 1270, 0.12f},
        {1220, 1220, 0.15f}, {1230, 1230, 0.055f}, {1210, 1210, 0.20f}, {1240, 1240, 0.20f},
        {1410, 1410, 0.035f}, {200, 226, 0.11f},  {2050, 2050, 0.11f}, {4312, 4312, 0.17f},
        {1300, 1300, 0.11f}, {1320, 1320, 0.11f}, {120, 120, 0.11f},   {4003, 4003, 0.11f},
        {4308, 4308, 0.11f},
    };
    for (const auto& t : kTargets) {
        if (goods >= t.lo && goods <= t.hi) {
            return t.m;
        }
    }
    if (standin && cat == hand::HoldCategory::Small) {
        return 0.03f; // the pellet
    }
    return 0.0f;
}

/// The category's longest-side cap (m) when nothing is specific.
inline float InGameCap(hand::HoldCategory cat) {
    switch (cat) {
    case hand::HoldCategory::Bottle: return 0.13f;
    case hand::HoldCategory::Small: return 0.06f;
    case hand::HoldCategory::Paper: return 0.11f;
    case hand::HoldCategory::Bell: return 0.13f;
    case hand::HoldCategory::Tool: return 0.20f;
    case hand::HoldCategory::Weapon: return 0.45f;
    }
    return 0.1f;
}

/// The mesh's own longest side at scale 1 (its bounding box).
inline float ModelLongest(const sb::Model& model) {
    if (model.vertices.empty()) {
        return 0.0f;
    }
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (const sb::Vertex& v : model.vertices) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], v.position[k]);
            hi[k] = std::max(hi[k], v.position[k]);
        }
    }
    return std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
}

/// The in-game scale: target / longest side at scale 1, else the category cap (down only).
inline float InGameScale(int goods, bool standin, hand::HoldCategory cat, float longest) {
    if (longest < 1e-5f) {
        return 1.0f;
    }
    const float target = InGameTarget(goods, standin, cat);
    return target > 0.0f ? target / longest : std::min(1.0f, InGameCap(cat) / longest);
}

/// The radius of the model's vertices around its origin (times `scale`).
inline float ModelRadius(const sb::Model& model, float scale = 1.0f) {
    float r2 = 0.0f;
    for (const sb::Vertex& v : model.vertices) {
        r2 = std::max(r2, v.position[0] * v.position[0] + v.position[1] * v.position[1] +
                              v.position[2] * v.position[2]);
    }
    return std::sqrt(r2) * scale;
}

/// The loaded meshes, by goods (loaded on first use; a missing or bad file is remembered, so the
/// caller draws its fallback without touching the disk again).
class MeshCache {
public:
    struct Entry {
        bool tried = false, ok = false;
        sb::Model model;
        float radius = 0.05f; // of the scaled mesh around its origin
        float longest = 0.0f; // the mesh's own longest side at scale 1 (spec.scale is the in-game scale)
        std::string error;
        MeshSpec spec;           // the file and look Resolve settled on
        bool from_table = false; // the sandbox's tuned entry (its grip pose is a socket)
        std::string source;      // "items/1000.json" or "stand-in paper" (for the log)
        std::string tried_path;  // the file path looked at (full), empty when no row names one
        std::string why;         // why a stand-in: "no mesh row for this goods" / "file missing" / "unreadable: ..."
        bool standin = false;    // the procedural stand-in of the goods' hold category
    };

    /// Any goods (the custom hands, vr_hand_model.h): the spec of MeshFor(assets, goods) (the
    /// sandbox's tuned row, else items/items.json), loaded once. Null when there is none (the disc).
    const Entry* Resolve(const std::filesystem::path& assets, int goods) {
        const auto it = entries.find(goods);
        if (it != entries.end() && it->second.tried) {
            return it->second.ok ? &it->second : nullptr;
        }
        const MeshSpec* spec = MeshFor(assets, goods);
        const Entry* got = spec ? Get(assets, *spec) : nullptr;
        Entry& e = entries[goods];
        e.tried = true;
        if (spec) {
            e.tried_path = (assets / spec->file).string();
        }
        if (got) {
            e.spec = *spec;
            e.from_table = MeshFor(goods) != nullptr;
            e.source = spec->file;
            // The in-game size (a real item's), whatever the sandbox's or items.json's scale was.
            e.longest = ModelLongest(e.model);
            e.spec.scale = InGameScale(goods, false, hand::CategoryFor(BBXR_ITEM_GOODS | uint32_t(goods)), e.longest);
            e.radius = ModelRadius(e.model, e.spec.scale);
            return &e;
        }
        if (!spec) {
            e.error = "no mesh for goods " + std::to_string(goods);
            e.why = "no mesh row for this goods (sandbox table and items.json)";
        } else {
            e.why = (e.error.rfind("no ", 0) == 0 ? "file missing: " : "unusable: ") + e.error;
        }
        // No model (or its file is missing): the stand-in of the goods' hold category.
        const hand::HoldCategory cat = hand::CategoryFor(BBXR_ITEM_GOODS | uint32_t(goods));
        e.model = StandIn(cat);
        for (sb::MeshRange& r : e.model.meshes) {
            if (r.texture == "card") {
                r.texture = "goods:" + std::to_string(goods); // the paper's menu icon on the card
            }
        }
        e.ok = true;
        e.standin = true;
        e.from_table = false;
        e.spec = MeshSpec{};
        e.spec.goods = goods;
        e.spec.color = StandInColor(goods, cat);
        e.source = std::string("stand-in ") + hand::CategoryName(cat);
        e.longest = ModelLongest(e.model);
        e.spec.scale = InGameScale(goods, true, cat, e.longest);
        e.radius = ModelRadius(e.model, e.spec.scale);
        return &e;
    }
    /// The model of `spec`, null when its file is missing or unreadable.
    const Entry* Get(const std::filesystem::path& assets, const MeshSpec& spec) {
        Entry& e = entries[spec.goods];
        if (!e.tried) {
            e.tried = true;
            std::error_code ec;
            const std::filesystem::path path = assets / spec.file;
            if (!std::filesystem::exists(path, ec)) {
                e.error = "no " + path.string();
            } else if (sb::LoadModel(path, e.model, e.error) && !e.model.vertices.empty() &&
                       !e.model.indices.empty()) {
                e.ok = true;
                e.radius = ModelRadius(e.model, spec.scale);
            } else if (e.error.empty()) {
                e.error = "empty mesh " + path.string();
            }
        }
        return e.ok ? &e : nullptr;
    }
    /// The item texture sheet and icon cells of `assets` (loaded once per folder); null when there are
    /// neither (the flat look). The same layout UiAtlas::Load appends to the icon atlas.
    const sb::ItemSheet* Sheet(const std::filesystem::path& assets) {
        if (!sheet_loaded || sheet_from != assets) {
            sheet_loaded = true;
            sheet_from = assets;
            sheet_ok = sheet.Load(assets) || !sheet.icon_xy.empty();
        }
        return sheet_ok ? &sheet : nullptr;
    }
    const Entry* Peek(int goods) const {
        const auto it = entries.find(goods);
        return it == entries.end() ? nullptr : &it->second;
    }

private:
    std::map<int, Entry> entries;
    sb::ItemSheet sheet;
    std::filesystem::path sheet_from;
    bool sheet_loaded = false, sheet_ok = false;
};

/// Where the mesh sits: its centre and orientation from the grip pose and the item point the game
/// logic uses (`item_pos` = grip + grip_rot * hold, ItemPos()). The sandbox's centre offset is kept
/// relative to the grip, so a hold that differs from the sandbox's only shifts the mesh by the
/// difference and the gesture tests stay on ItemPos.
inline m3::Mat4 MeshWorld(const MeshSpec& spec, Kind kind, m3::Vec3 item_pos, m3::Quat grip_rot,
                          m3::Vec3 hold) {
    const KindTweak& t = TweakFor(kind);
    const m3::Vec3 centre = item_pos + m3::Rotate(grip_rot, spec.pos - hold + t.pos);
    const m3::Quat rot = grip_rot * m3::FromEulerDeg(spec.rot_deg + t.rot_deg);
    return m3::FromPose(rot, centre) * m3::Scale(spec.scale);
}

/// Where the item's hand frame is: the game's hand (its R_Weapon / L_Weapon bone, which the arms IK
/// poses, in the headset's space) or, without it, the controller's grip pose.
struct HandPlacement {
    m3::Vec3 pos;
    m3::Quat rot;
    bool from_bone = false;
    float gap_m = 0.0f; // controller grip to the game hand
};

/// The bone matrix (3x4, column-major: x, y, z columns then the origin, BbXrGameDebug::weapon) as a
/// pose. The matrix is the bone frame with the data's X mirror folded in (src/vr_hitbox_math.h, det +1),
/// which is the sandbox's grip frame for a weapon (a zero weapon_grip): so it is a grip pose, the one
/// the sandbox's item sockets are tuned for. False when it is not a finite rotation.
inline bool BoneGripPose(const float m[12], m3::Vec3* pos, m3::Quat* rot) {
    for (int i = 0; i < 12; ++i) {
        if (!std::isfinite(m[i])) {
            return false;
        }
    }
    const m3::Vec3 x{m[0], m[1], m[2]}, y{m[3], m[4], m[5]}, z{m[6], m[7], m[8]};
    const float det = m3::Dot(x, m3::Cross(y, z));
    if (det < 0.5f || det > 2.0f) {
        return false;
    }
    m3::Mat4 r;
    r.m[0] = x.x, r.m[1] = x.y, r.m[2] = x.z;
    r.m[4] = y.x, r.m[5] = y.y, r.m[6] = y.z;
    r.m[8] = z.x, r.m[9] = z.y, r.m[10] = z.z;
    *rot = m3::ToQuat(r);
    *pos = {m[9], m[10], m[11]};
    return true;
}

/// The hand frame the item is placed in: `bone` (null when the host sent none) unless it is not a pose
/// or lies more than `max_gap_m` from the controller (a stale or wrong bone), else the controller.
inline HandPlacement PlaceInHand(const float* bone, m3::Vec3 ctrl_pos, m3::Quat ctrl_rot, float max_gap_m = 0.6f) {
    HandPlacement p;
    p.pos = ctrl_pos;
    p.rot = ctrl_rot;
    m3::Vec3 bp;
    m3::Quat br;
    if (bone && BoneGripPose(bone, &bp, &br)) {
        p.gap_m = m3::Length(bp - ctrl_pos);
        if (p.gap_m <= max_gap_m) {
            p.pos = bp;
            p.rot = br;
            p.from_bone = true;
        }
    }
    return p;
}

/// The one item placement of both hand modes (the game's hands and the custom ones): the mesh's world
/// matrix for the holding hand `h`'s grip pose. The item's hold category socket (hand::GripFor, with
/// its per-item overrides; the sandbox's own pose is NOT used in game), then, for an items.json mesh,
/// its own upright turn (rot_deg); then the in-game scale (spec.scale, set by MeshCache::Resolve).
inline m3::Mat4 SocketMeshWorld(const MeshSpec& spec, bool from_table, Kind /*kind*/, uint32_t game_id, int h,
                                m3::Vec3 grip_pos, m3::Quat grip_rot) {
    const hand::GripDef sock = hand::GripFor(game_id);
    m3::Mat4 world = hand::SocketWorld(sock, h, grip_pos, grip_rot);
    if (!from_table) {
        world = world * m3::FromQuat(m3::FromEulerDeg(spec.rot_deg));
    }
    return world * m3::Scale(spec.scale);
}

/// Flat-lit triangles from several meshes, sorted together far to near (the UI layer has no depth
/// buffer: a hand and the item in it must interleave per triangle, not per mesh). Back faces
/// (from `eye`) are dropped; the winding follows the vertex normals, whichever the exporter used.
/// Where a mesh range takes its colour from the item texture sheet (the UI pipeline's icon mode):
/// the model's vertices (uv) and the sheet key. The triangles then carry atlas uvs and a white tint.
struct TexBinding {
    const sb::Vertex* vertices = nullptr;
    const sb::ItemSheet* sheet = nullptr;
    const std::string* key = nullptr;
    float tint_mix = 0.0f; // 0: white (the texture as is), 1: the item's flat colour (tinted glass: vials)
};

class TriBatch {
public:
    explicit TriBatch(m3::Vec3 eye_pos) : eye(eye_pos) {}

    /// World-space positions and normals, a triangle list, one colour. Returns the triangles kept.
    size_t Add(const std::vector<m3::Vec3>& pos, const std::vector<m3::Vec3>& nrm, const uint32_t* idx,
               size_t count, sb::UiColor color, const TexBinding* tex = nullptr) {
        using namespace m3;
        const Vec3 light = Normalize(Vec3{0.35f, 0.8f, 0.5f});
        size_t kept = 0;
        for (size_t i = 0; i + 2 < count; i += 3) {
            const uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
            if (a >= pos.size() || b >= pos.size() || c >= pos.size()) {
                continue;
            }
            Vec3 n = Cross(pos[b] - pos[a], pos[c] - pos[a]);
            if (Dot(n, nrm[a] + nrm[b] + nrm[c]) < 0.0f) {
                n = -n; // the winding follows the vertex normals, whichever the exporter used
            }
            const Vec3 centre = (pos[a] + pos[b] + pos[c]) * (1.0f / 3.0f);
            if (Dot(n, eye - centre) <= 0.0f) {
                continue;
            }
            Tri t;
            t.depth = Length(centre - eye);
            t.color = color;
            t.p[0] = pos[a], t.p[1] = pos[b], t.p[2] = pos[c];
            const uint32_t id[3] = {a, b, c};
            if (tex && tex->vertices && tex->sheet && tex->key) {
                // The triangle's uvs, wrapped by its own centroid (a repeating texture), into the sheet.
                float raw[3][2];
                float cu = 0.0f, cv = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    raw[k][0] = tex->vertices[id[k]].uv[0];
                    raw[k][1] = tex->vertices[id[k]].uv[1];
                    cu += raw[k][0] / 3.0f, cv += raw[k][1] / 3.0f;
                }
                const float ou = std::floor(cu), ov = std::floor(cv);
                bool ok = true;
                for (int k = 0; k < 3 && ok; ++k) {
                    ok = tex->sheet->Uv(*tex->key, raw[k][0] - ou, raw[k][1] - ov, t.uv[k]);
                }
                if (ok) {
                    t.textured = true;
                    const float m = tex->tint_mix;
                    // A tinted texture (vials) is dark (grey glass times a deep red): lifted so it shows.
                    const float gain = 1.0f + 0.9f * m;
                    t.color = {(1.0f + (color.r - 1.0f) * m) * gain, (1.0f + (color.g - 1.0f) * m) * gain,
                               (1.0f + (color.b - 1.0f) * m) * gain, color.a};
                }
            }
            for (int k = 0; k < 3; ++k) {
                const Vec3 nn = nrm[id[k]];
                const float lam = std::max(0.0f, Dot(nn, light));
                const float rim = std::pow(1.0f - std::clamp(Dot(nn, Normalize(eye - pos[id[k]], {0, 0, 1})), 0.0f, 1.0f), 2.0f);
                t.shade[k] = std::clamp(0.38f + 0.62f * lam + 0.18f * rim, 0.0f, 1.2f);
            }
            tris.push_back(t);
            ++kept;
        }
        return kept;
    }

    size_t Size() const { return tris.size(); }

    /// Appends every triangle, far to near, to `out` and empties the batch. Returns the count.
    size_t Emit(std::vector<sb::UiVertex>& out) {
        std::stable_sort(tris.begin(), tris.end(), [](const Tri& l, const Tri& r) { return l.depth > r.depth; });
        out.reserve(out.size() + tris.size() * 3);
        for (const Tri& t : tris) {
            for (int k = 0; k < 3; ++k) {
                const float s = t.shade[k];
                // A textured triangle: the icon mode of ui.frag (shape.y = 1 marks it as an item for the
                // offline render); the shade is the lighting, multiplied into the texture.
                out.push_back({{t.p[k].x, t.p[k].y, t.p[k].z},
                               {t.textured ? t.uv[k][0] : 0.0f, t.textured ? t.uv[k][1] : 0.0f},
                               {std::min(t.color.r * s, 1.0f), std::min(t.color.g * s, 1.0f),
                                std::min(t.color.b * s, 1.0f), t.color.a},
                               {t.textured ? 1.0f : 0.0f, t.textured ? 1.0f : 0.0f, 0.0f, 0.0f},
                               {0.0f, 0.0f, 0.0f, 0.0f}});
            }
        }
        const size_t n = tris.size();
        tris.clear();
        return n;
    }

private:
    struct Tri {
        float depth;
        m3::Vec3 p[3];
        float shade[3];
        float uv[3][2] = {};
        bool textured = false;
        sb::UiColor color;
    };
    m3::Vec3 eye;
    std::vector<Tri> tris;
};

/// The model's vertices in world space (for TriBatch::Add).
inline void TransformModel(const sb::Model& model, const m3::Mat4& world, std::vector<m3::Vec3>& pos,
                           std::vector<m3::Vec3>& nrm) {
    pos.resize(model.vertices.size());
    nrm.resize(model.vertices.size());
    for (size_t i = 0; i < model.vertices.size(); ++i) {
        const sb::Vertex& v = model.vertices[i];
        pos[i] = m3::TransformPoint(world, {v.position[0], v.position[1], v.position[2]});
        nrm[i] = m3::Normalize(m3::TransformDir(world, {v.normal[0], v.normal[1], v.normal[2]}), {0, 1, 0});
    }
}

/// Adds a model's triangles to a batch: each mesh range with a texture the sheet knows is drawn
/// textured (white tint), every other range with the flat `color`. Returns the triangles kept.
inline size_t AddModel(TriBatch& batch, const sb::Model& model, const std::vector<m3::Vec3>& pos,
                       const std::vector<m3::Vec3>& nrm, sb::UiColor color, const sb::ItemSheet* sheet,
                       float tex_tint = 0.0f) {
    size_t kept = 0;
    bool any = false;
    for (const sb::MeshRange& r : model.meshes) {
        if (r.index_count == 0 || size_t(r.first_index) + r.index_count > model.indices.size()) {
            continue;
        }
        any = true;
        TexBinding tb;
        if (sheet && !r.texture.empty()) {
            tb.vertices = model.vertices.data();
            tb.sheet = sheet;
            tb.key = &r.texture;
            tb.tint_mix = tex_tint;
        }
        kept += batch.Add(pos, nrm, model.indices.data() + r.first_index, r.index_count, color, &tb);
    }
    if (!any) {
        kept = batch.Add(pos, nrm, model.indices.data(), model.indices.size(), color);
    }
    return kept;
}

/// Appends the model's triangles (flat colour, a key light and a rim, back faces dropped, far to
/// near so the translucent look blends right) to `out`. `eye`: the head position. Returns the count.
inline size_t AppendMesh(std::vector<sb::UiVertex>& out, const sb::Model& model, const m3::Mat4& world,
                         sb::UiColor color, m3::Vec3 eye, const sb::ItemSheet* sheet = nullptr,
                         float tex_tint = 0.0f) {
    std::vector<m3::Vec3> pos, nrm;
    TransformModel(model, world, pos, nrm);
    TriBatch batch(eye);
    AddModel(batch, model, pos, nrm, color, sheet, tex_tint);
    return batch.Emit(out);
}

} // namespace held
} // namespace vrui
