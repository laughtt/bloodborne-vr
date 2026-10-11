// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The custom character model override ("skin"): a user-supplied static mesh drawn over the other
// characters whose poses the host sends (BbXrSkinState in bbport_xr_bridge.h). Pure CPU:
//   - ParseSkinGlb: glTF 2.0 binary, static geometry only. Walks the scene's node tree applying the
//     node transforms; every triangle primitive becomes a SkinPart (POSITION, NORMAL or computed
//     smooth normals, indices or none, COLOR_0 per-vertex colours, the material's base colour
//     factor). Textures, skins and animations are ignored. <= 200k triangles, no non-finite values.
//   - LoadSkin: a file (.glb, or the bbmodel .json as one grey part) or a folder (skin.glb, else the
//     first *.glb, else the first *.json, sorted).
//   - Normalize: feet at Y = 0, centred on X/Z, uniformly scaled to a height in metres. The mesh
//     convention is glTF's: +Y up, the character faces +Z.
//   - SkinWorld: the rigid, right-handed frame that stands the mesh on a target: Y = up, the mesh's
//     +Z face = the target's forward, origin = the feet (+ a bob). SkinBob: a small walk hop.
//   - AppendSkin: world transform, back-face culling, flat lighting and one far-to-near sort for
//     all parts of a skin (the UI layer has no depth buffer), like vr_held_mesh.h TriBatch but with
//     per-vertex colours.
//   - Config from the environment: BB_VR_SKIN, BB_VR_SKIN_SCALE, BB_VR_SKIN_HEIGHT, BB_VR_SKIN_YAW,
//     BB_VR_SKIN_BOB.
// Unit tested in vr_game_ui_test.cpp.

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "bbport_xr_bridge.h"
#include "sandbox3d/json.h"
#include "sandbox3d/math3d.h"
#include "sandbox3d/model.h"
#include "sandbox3d/ui_render.h"
#include "vr_hand_model.h"

namespace vrui {
namespace skin {

constexpr size_t kMaxTriangles = 200000;

struct SkinPart {
    std::vector<m3::Vec3> pos, nrm;
    std::vector<uint32_t> idx;
    sb::UiColor color{0.78f, 0.78f, 0.78f, 1.0f};
    std::vector<sb::UiColor> vcolor; // optional, per vertex (multiplies `color`)
};

struct SkinMesh {
    std::vector<SkinPart> parts;
    m3::Vec3 lo{0, 0, 0}, hi{0, 0, 0};
    size_t tris = 0;
};

/// Recomputes lo/hi and tris from the parts.
inline void Measure(SkinMesh& mesh) {
    bool any = false;
    mesh.tris = 0;
    for (const SkinPart& p : mesh.parts) {
        mesh.tris += p.idx.size() / 3;
        for (const m3::Vec3& v : p.pos) {
            if (!any) {
                mesh.lo = mesh.hi = v;
                any = true;
            }
            mesh.lo = {std::min(mesh.lo.x, v.x), std::min(mesh.lo.y, v.y), std::min(mesh.lo.z, v.z)};
            mesh.hi = {std::max(mesh.hi.x, v.x), std::max(mesh.hi.y, v.y), std::max(mesh.hi.z, v.z)};
        }
    }
    if (!any) {
        mesh.lo = mesh.hi = {0, 0, 0};
    }
}

namespace detail {

inline bool Finite(float v) { return std::isfinite(v); }

inline float Det3(const m3::Mat4& m) {
    return m.m[0] * (m.m[5] * m.m[10] - m.m[9] * m.m[6]) - m.m[4] * (m.m[1] * m.m[10] - m.m[9] * m.m[2]) +
           m.m[8] * (m.m[1] * m.m[6] - m.m[5] * m.m[2]);
}

inline void SmoothNormals(SkinPart& p) {
    p.nrm.assign(p.pos.size(), m3::Vec3{0, 0, 0});
    for (size_t i = 0; i + 2 < p.idx.size(); i += 3) {
        const uint32_t a = p.idx[i], b = p.idx[i + 1], c = p.idx[i + 2];
        const m3::Vec3 n = m3::Cross(p.pos[b] - p.pos[a], p.pos[c] - p.pos[a]); // area weighted
        p.nrm[a] = p.nrm[a] + n;
        p.nrm[b] = p.nrm[b] + n;
        p.nrm[c] = p.nrm[c] + n;
    }
    for (m3::Vec3& n : p.nrm) {
        n = m3::Normalize(n, {0, 1, 0});
    }
}

struct Walker {
    const json::Value* doc = nullptr;
    const std::vector<uint8_t>* bin = nullptr;
    SkinMesh* out = nullptr;
    std::string* error = nullptr;
    size_t visited = 0;

    bool Primitive(const json::Value& prim, const m3::Mat4& world) {
        if (prim["mode"].Int(4) != 4) {
            return true; // lines, points, strips: skipped
        }
        const json::Value& at = prim["attributes"];
        hand::detail::Accessor pos, nrm, col, idx;
        if (!hand::detail::GetAccessor(*doc, *bin, at["POSITION"].Int(-1), 3, pos, *error)) {
            return false;
        }
        if (pos.count == 0 || pos.count > kMaxTriangles * 3) {
            *error = "bad vertex count";
            return false;
        }
        const bool has_n = at.Has("NORMAL");
        if (has_n && (!hand::detail::GetAccessor(*doc, *bin, at["NORMAL"].Int(-1), 3, nrm, *error) ||
                      nrm.count < pos.count)) {
            if (error->empty()) {
                *error = "NORMAL has fewer entries than POSITION";
            }
            return false;
        }
        int col_n = 0;
        bool has_c = false;
        if (at.Has("COLOR_0")) {
            for (int n = 3; n <= 4 && !has_c; ++n) {
                std::string ignore;
                if (hand::detail::GetAccessor(*doc, *bin, at["COLOR_0"].Int(-1), n, col, ignore) && col.count >= pos.count &&
                    col.type != 5125) {
                    has_c = true;
                    col_n = n;
                }
            }
        }
        const bool has_i = prim.Has("indices");
        if (has_i) {
            if (!hand::detail::GetAccessor(*doc, *bin, prim["indices"].Int(-1), 1, idx, *error)) {
                return false;
            }
            if (idx.type != 5121 && idx.type != 5123 && idx.type != 5125) {
                *error = "unsupported index type";
                return false;
            }
        }
        const size_t n_idx = has_i ? idx.count : pos.count;
        if (n_idx < 3) {
            return true;
        }
        if (out->tris + n_idx / 3 > kMaxTriangles) {
            *error = "more than 200000 triangles";
            return false;
        }
        SkinPart part;
        part.pos.resize(pos.count);
        for (size_t i = 0; i < pos.count; ++i) {
            const m3::Vec3 p{pos.Get(i, 0), pos.Get(i, 1), pos.Get(i, 2)};
            if (!Finite(p.x) || !Finite(p.y) || !Finite(p.z)) {
                *error = "non-finite vertex position";
                return false;
            }
            part.pos[i] = m3::TransformPoint(world, p);
        }
        const bool flip = Det3(world) < 0.0f;
        part.idx.reserve(n_idx / 3 * 3);
        for (size_t i = 0; i + 2 < n_idx; i += 3) {
            uint32_t t[3];
            for (size_t k = 0; k < 3; ++k) {
                t[k] = has_i ? idx.GetU(i + k, 0) : uint32_t(i + k);
                if (t[k] >= pos.count) {
                    *error = "index out of range";
                    return false;
                }
            }
            if (flip) {
                std::swap(t[1], t[2]);
            }
            part.idx.insert(part.idx.end(), t, t + 3);
        }
        if (has_n) {
            part.nrm.resize(pos.count);
            for (size_t i = 0; i < pos.count; ++i) {
                const m3::Vec3 n{nrm.Get(i, 0), nrm.Get(i, 1), nrm.Get(i, 2)};
                if (!Finite(n.x) || !Finite(n.y) || !Finite(n.z)) {
                    *error = "non-finite normal";
                    return false;
                }
                part.nrm[i] = m3::Normalize(m3::TransformDir(world, n), {0, 1, 0});
            }
        } else {
            SmoothNormals(part);
        }
        if (has_c) {
            part.vcolor.resize(pos.count);
            for (size_t i = 0; i < pos.count; ++i) {
                sb::UiColor c{col.Get(i, 0), col.Get(i, 1), col.Get(i, 2), col_n == 4 ? col.Get(i, 3) : 1.0f};
                c.a = 1.0f;
                if (!Finite(c.r) || !Finite(c.g) || !Finite(c.b)) {
                    *error = "non-finite vertex colour";
                    return false;
                }
                part.vcolor[i] = c;
            }
        }
        const int mat = prim["material"].Int(-1);
        if (mat >= 0) {
            const json::Value& f = (*doc)["materials"][size_t(mat)]["pbrMetallicRoughness"]["baseColorFactor"];
            if (f.Size() >= 3) {
                part.color = {f[0].Float(1), f[1].Float(1), f[2].Float(1), 1.0f};
            }
        }
        out->tris += part.idx.size() / 3;
        out->parts.push_back(std::move(part));
        return true;
    }

    bool Node(int index, const m3::Mat4& parent, int depth) {
        const json::Value& nodes = (*doc)["nodes"];
        if (index < 0 || size_t(index) >= nodes.Size() || depth > 64 || ++visited > 100000) {
            *error = "bad node tree";
            return false;
        }
        const json::Value& n = nodes[size_t(index)];
        const m3::Mat4 world = parent * hand::detail::NodeLocal(n);
        for (int i = 0; i < 16; ++i) {
            if (!Finite(world.m[i])) {
                *error = "non-finite node transform";
                return false;
            }
        }
        if (n.Has("mesh")) {
            const json::Value& prims = (*doc)["meshes"][size_t(n["mesh"].Int(-1) < 0 ? 0 : n["mesh"].Int())]["primitives"];
            for (size_t i = 0; i < prims.Size(); ++i) {
                if (!Primitive(prims[i], world)) {
                    return false;
                }
            }
        }
        const json::Value& ch = n["children"];
        for (size_t i = 0; i < ch.Size(); ++i) {
            if (!Node(ch[i].Int(-1), world, depth + 1)) {
                return false;
            }
        }
        return true;
    }
};

} // namespace detail

/// Parses a glTF 2.0 binary (see the header comment). On failure `err` says why and `out` is empty.
inline bool ParseSkinGlb(const std::vector<uint8_t>& file, SkinMesh& out, std::string& err) {
    out = SkinMesh{};
    err.clear();
    auto u32 = [&](size_t at) {
        uint32_t v = 0;
        std::memcpy(&v, file.data() + at, 4);
        return v;
    };
    if (file.size() < 28 || u32(0) != 0x46546C67u || u32(4) != 2u) {
        err = "not a glTF 2.0 binary";
        return false;
    }
    const size_t json_len = u32(12);
    if (u32(16) != 0x4E4F534Au || 20 + json_len + 8 > file.size()) {
        err = "no JSON chunk (truncated?)";
        return false;
    }
    const size_t bin_at = 20 + json_len;
    const size_t bin_len = u32(bin_at);
    if (u32(bin_at + 4) != 0x004E4942u || bin_at + 8 + bin_len > file.size()) {
        err = "no BIN chunk (truncated?)";
        return false;
    }
    const std::vector<uint8_t> bin(file.begin() + std::ptrdiff_t(bin_at + 8),
                                   file.begin() + std::ptrdiff_t(bin_at + 8 + bin_len));
    std::string perr;
    const json::Value doc = json::Parse(std::string(file.begin() + 20, file.begin() + std::ptrdiff_t(20 + json_len)), &perr);
    if (!doc.IsObject()) {
        err = "JSON: " + perr;
        return false;
    }
    detail::Walker w;
    w.doc = &doc;
    w.bin = &bin;
    w.out = &out;
    w.error = &err;
    const m3::Mat4 identity;
    bool ok = true;
    const json::Value& scenes = doc["scenes"];
    const json::Value& nodes = doc["nodes"];
    if (scenes.Size() > 0) {
        const json::Value& roots = scenes[size_t(std::max(doc["scene"].Int(0), 0))]["nodes"];
        for (size_t i = 0; i < roots.Size() && ok; ++i) {
            ok = w.Node(roots[i].Int(-1), identity, 0);
        }
    } else {
        std::vector<bool> child(nodes.Size(), false);
        for (size_t i = 0; i < nodes.Size(); ++i) {
            for (size_t k = 0; k < nodes[i]["children"].Size(); ++k) {
                const int c = nodes[i]["children"][k].Int(-1);
                if (c >= 0 && size_t(c) < child.size()) {
                    child[size_t(c)] = true;
                }
            }
        }
        for (size_t i = 0; i < nodes.Size() && ok; ++i) {
            if (!child[i]) {
                ok = w.Node(int(i), identity, 0);
            }
        }
    }
    if (!ok) {
        if (err.empty()) {
            err = "bad glTF";
        }
        out = SkinMesh{};
        return false;
    }
    Measure(out);
    if (out.tris == 0) {
        err = "no triangles";
        out = SkinMesh{};
        return false;
    }
    return true;
}

inline bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& data) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/// A file (.glb, or the bbmodel .json as one grey part) or a folder (skin.glb, else the first *.glb,
/// else the first *.json, sorted). `used` is the file read.
inline bool LoadSkin(const std::filesystem::path& path, SkinMesh& out, std::string& err,
                     std::filesystem::path* used = nullptr) {
    namespace fs = std::filesystem;
    out = SkinMesh{};
    std::error_code ec;
    fs::path file = path;
    if (fs::is_directory(path, ec)) {
        std::vector<fs::path> glbs, jsons;
        for (const fs::directory_entry& e : fs::directory_iterator(path, ec)) {
            if (!e.is_regular_file(ec)) {
                continue;
            }
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".glb") {
                glbs.push_back(e.path());
            } else if (ext == ".json") {
                jsons.push_back(e.path());
            }
        }
        std::sort(glbs.begin(), glbs.end());
        std::sort(jsons.begin(), jsons.end());
        file.clear();
        for (const fs::path& g : glbs) {
            if (g.filename() == "skin.glb") {
                file = g;
            }
        }
        if (file.empty()) {
            file = !glbs.empty() ? glbs.front() : !jsons.empty() ? jsons.front() : fs::path{};
        }
        if (file.empty()) {
            err = "no .glb or .json in " + path.string();
            return false;
        }
    }
    if (used) {
        *used = file;
    }
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".json") {
        sb::Model model;
        if (!sb::LoadModel(file, model, err)) {
            if (err.empty()) {
                err = "cannot load " + file.string();
            }
            return false;
        }
        SkinPart part;
        part.pos.reserve(model.vertices.size());
        for (const sb::Vertex& v : model.vertices) {
            const m3::Vec3 p{v.position[0], v.position[1], v.position[2]};
            if (!detail::Finite(p.x) || !detail::Finite(p.y) || !detail::Finite(p.z)) {
                err = "non-finite vertex position";
                return false;
            }
            part.pos.push_back(p);
            part.nrm.push_back(m3::Normalize({v.normal[0], v.normal[1], v.normal[2]}, {0, 1, 0}));
        }
        for (const uint32_t i : model.indices) {
            if (i >= part.pos.size()) {
                err = "index out of range";
                return false;
            }
        }
        part.idx = model.indices;
        part.idx.resize(part.idx.size() / 3 * 3);
        if (part.idx.empty() || part.idx.size() / 3 > kMaxTriangles) {
            err = part.idx.empty() ? "no triangles" : "more than 200000 triangles";
            return false;
        }
        out.parts.push_back(std::move(part));
        Measure(out);
        return true;
    }
    std::vector<uint8_t> data;
    if (!ReadFile(file, data)) {
        err = "cannot read " + file.string();
        return false;
    }
    return ParseSkinGlb(data, out, err);
}

/// Feet (min Y) at 0, X/Z centred on 0 (bounding box), uniformly scaled so the box is `height_m`
/// tall. False (mesh untouched) when it has no height.
inline bool Normalize(SkinMesh& mesh, float height_m) {
    Measure(mesh);
    const float h = mesh.hi.y - mesh.lo.y;
    if (!(h > 1e-6f) || !(height_m > 0.0f) || !std::isfinite(h)) {
        return false;
    }
    const float s = height_m / h;
    const float cx = 0.5f * (mesh.lo.x + mesh.hi.x), cz = 0.5f * (mesh.lo.z + mesh.hi.z);
    for (SkinPart& p : mesh.parts) {
        for (m3::Vec3& v : p.pos) {
            v = {(v.x - cx) * s, (v.y - mesh.lo.y) * s, (v.z - cz) * s};
        }
    }
    Measure(mesh);
    return true;
}

/// The rigid right-handed frame that stands the mesh on a target: Y = `up`, the mesh's +Z =
/// `forward` made orthogonal to `up`, X = Y x Z, origin = `pos` + up * bob_m. The mesh is first
/// rolled about its own Z (`roll_rad`), then yawed about its own Y (`yaw_offset_rad`). det = +1.
inline m3::Mat4 SkinWorld(m3::Vec3 pos, m3::Vec3 forward, m3::Vec3 up, float yaw_offset_rad, float bob_m,
                          float roll_rad = 0.0f) {
    using namespace m3;
    const Vec3 y = Normalize(up, {0, 1, 0});
    Vec3 z = Normalize(Perp(forward, y), {0, 0, 0});
    if (Dot(z, z) < 0.5f) { // forward is (nearly) along up: any heading
        z = Normalize(Perp(std::fabs(y.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0}, y), {0, 0, 1});
    }
    const Vec3 x = Cross(y, z);
    Mat4 frame;
    frame.m[0] = x.x, frame.m[1] = x.y, frame.m[2] = x.z;
    frame.m[4] = y.x, frame.m[5] = y.y, frame.m[6] = y.z;
    frame.m[8] = z.x, frame.m[9] = z.y, frame.m[10] = z.z;
    const Vec3 o = pos + y * bob_m;
    frame.m[12] = o.x, frame.m[13] = o.y, frame.m[14] = o.z;
    const float cy = std::cos(yaw_offset_rad), sy = std::sin(yaw_offset_rad);
    Mat4 ry; // about +Y: +Z -> (sin, 0, cos)
    ry.m[0] = cy, ry.m[2] = -sy, ry.m[8] = sy, ry.m[10] = cy;
    const float cr = std::cos(roll_rad), sr = std::sin(roll_rad);
    Mat4 rz; // about +Z: +X -> (cos, sin, 0)
    rz.m[0] = cr, rz.m[1] = sr, rz.m[4] = -sr, rz.m[5] = cr;
    return frame * ry * rz;
}

/// A small walk hop: 0 below 0.3 m/s. Otherwise a |sin| hop at a step rate that grows with the
/// speed (about 1.8 steps/s walking to 3.2 at a run), amplitude 2.2 % of the height (4 cm for a
/// 1.8 m mesh), and a side roll of up to 0.05 rad (swaying with the steps). `roll` may be null.
inline float SkinBob(float time_s, float speed_mps, float* roll, float height_m = 1.8f) {
    if (roll) {
        *roll = 0.0f;
    }
    if (!(speed_mps >= 0.3f) || !std::isfinite(time_s)) {
        return 0.0f;
    }
    const float rate = 1.4f + 0.45f * std::min(speed_mps, 4.0f); // steps per second
    const float phase = 3.14159265f * rate * time_s;
    if (roll) {
        *roll = 0.05f * std::sin(phase) * std::min(speed_mps, 2.0f) * 0.5f;
    }
    return 0.022f * height_m * std::fabs(std::sin(phase));
}

/// Flat-lit triangles of one skin (per-vertex colours), back faces from `eye` dropped (the winding
/// follows the vertex normals), emitted far to near in one sort over all parts. Returns the count.
inline size_t AppendSkin(std::vector<sb::UiVertex>& out, const SkinMesh& mesh, const m3::Mat4& world, m3::Vec3 eye) {
    using namespace m3;
    struct Tri {
        float depth;
        Vec3 p[3];
        sb::UiColor c[3];
    };
    std::vector<Tri> tris;
    const Vec3 light = Normalize(Vec3{0.35f, 0.8f, 0.5f});
    std::vector<Vec3> pos, nrm;
    for (const SkinPart& part : mesh.parts) {
        pos.resize(part.pos.size());
        nrm.resize(part.pos.size());
        for (size_t i = 0; i < part.pos.size(); ++i) {
            pos[i] = TransformPoint(world, part.pos[i]);
            nrm[i] = i < part.nrm.size() ? Normalize(TransformDir(world, part.nrm[i]), {0, 1, 0}) : Vec3{0, 1, 0};
        }
        for (size_t i = 0; i + 2 < part.idx.size(); i += 3) {
            const uint32_t id[3] = {part.idx[i], part.idx[i + 1], part.idx[i + 2]};
            if (id[0] >= pos.size() || id[1] >= pos.size() || id[2] >= pos.size()) {
                continue;
            }
            Vec3 n = Cross(pos[id[1]] - pos[id[0]], pos[id[2]] - pos[id[0]]);
            if (Dot(n, nrm[id[0]] + nrm[id[1]] + nrm[id[2]]) < 0.0f) {
                n = -n;
            }
            const Vec3 centre = (pos[id[0]] + pos[id[1]] + pos[id[2]]) * (1.0f / 3.0f);
            if (Dot(n, eye - centre) <= 0.0f) {
                continue;
            }
            Tri t;
            t.depth = Length(centre - eye);
            for (int k = 0; k < 3; ++k) {
                t.p[k] = pos[id[k]];
                const float lam = std::max(0.0f, Dot(nrm[id[k]], light));
                const float rim =
                    std::pow(1.0f - std::clamp(Dot(nrm[id[k]], Normalize(eye - pos[id[k]], {0, 0, 1})), 0.0f, 1.0f), 2.0f);
                const float s = std::clamp(0.38f + 0.62f * lam + 0.18f * rim, 0.0f, 1.2f);
                sb::UiColor c = part.color;
                if (id[k] < part.vcolor.size()) {
                    c.r *= part.vcolor[id[k]].r, c.g *= part.vcolor[id[k]].g, c.b *= part.vcolor[id[k]].b;
                }
                t.c[k] = {std::min(c.r * s, 1.0f), std::min(c.g * s, 1.0f), std::min(c.b * s, 1.0f), c.a};
            }
            tris.push_back(t);
        }
    }
    std::stable_sort(tris.begin(), tris.end(), [](const Tri& l, const Tri& r) { return l.depth > r.depth; });
    out.reserve(out.size() + tris.size() * 3);
    for (const Tri& t : tris) {
        for (int k = 0; k < 3; ++k) {
            out.push_back({{t.p[k].x, t.p[k].y, t.p[k].z},
                           {0.0f, 0.0f},
                           {t.c[k].r, t.c[k].g, t.c[k].b, t.c[k].a},
                           {0.0f, 0.0f, 0.0f, 0.0f},
                           {0.0f, 0.0f, 0.0f, 0.0f}});
        }
    }
    return tris.size();
}

/// A built-in procedural stand-in (a coloured body box, a head box and two arm boxes), unit height.
inline SkinMesh PlaceholderMesh() {
    SkinMesh mesh;
    auto box = [&](m3::Vec3 lo, m3::Vec3 hi, sb::UiColor color) {
        SkinPart p;
        p.color = color;
        struct Face { m3::Vec3 n, u, v; };
        const Face faces[6] = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                               {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
                               {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}};
        const m3::Vec3 c = (lo + hi) * 0.5f, h = (hi - lo) * 0.5f;
        for (const Face& f : faces) {
            const uint32_t base = uint32_t(p.pos.size());
            const m3::Vec3 fc{c.x + f.n.x * h.x, c.y + f.n.y * h.y, c.z + f.n.z * h.z};
            const m3::Vec3 uu{f.u.x * h.x, f.u.y * h.y, f.u.z * h.z}, vv{f.v.x * h.x, f.v.y * h.y, f.v.z * h.z};
            p.pos.push_back(fc - uu - vv);
            p.pos.push_back(fc + uu - vv);
            p.pos.push_back(fc + uu + vv);
            p.pos.push_back(fc - uu + vv);
            for (int k = 0; k < 4; ++k) {
                p.nrm.push_back(f.n);
            }
            for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
                p.idx.push_back(base + i);
            }
        }
        mesh.parts.push_back(std::move(p));
    };
    box({-0.18f, 0.0f, -0.10f}, {0.18f, 0.60f, 0.10f}, {0.85f, 0.25f, 0.2f, 1});   // body
    box({-0.12f, 0.64f, -0.12f}, {0.12f, 0.90f, 0.12f}, {0.95f, 0.85f, 0.6f, 1});  // head
    box({-0.30f, 0.20f, -0.06f}, {-0.20f, 0.58f, 0.06f}, {0.25f, 0.4f, 0.85f, 1}); // arms
    box({0.20f, 0.20f, -0.06f}, {0.30f, 0.58f, 0.06f}, {0.95f, 0.6f, 0.15f, 1}); // the +X arm is orange: shows a mirror
    box({-0.10f, 0.78f, 0.12f}, {0.10f, 0.84f, 0.18f}, {0.1f, 0.1f, 0.1f, 1});     // a nose block: the +Z (front)
    Measure(mesh);
    return mesh;
}

// ---- Config ---------------------------------------------------------------------------------------

struct Config {
    std::string path;      // BB_VR_SKIN: a file or folder; empty = off
    float scale = 1.0f;    // BB_VR_SKIN_SCALE
    float height_m = 1.8f; // BB_VR_SKIN_HEIGHT
    float yaw_rad = 0.0f;  // BB_VR_SKIN_YAW (degrees in the environment)
    bool bob = true;       // BB_VR_SKIN_BOB
    bool Enabled() const { return !path.empty(); }
};

inline Config ReadConfig() {
    Config c;
    auto env = [](const char* name) -> std::string {
        const char* e = std::getenv(name);
        return e ? e : "";
    };
    c.path = env("BB_VR_SKIN");
    if (c.path == "0") {
        c.path.clear();
    }
    auto number = [&](const char* name, float fallback) {
        const std::string s = env(name);
        if (s.empty()) {
            return fallback;
        }
        char* end = nullptr;
        const float v = std::strtof(s.c_str(), &end);
        return end != s.c_str() && std::isfinite(v) ? v : fallback;
    };
    c.scale = number("BB_VR_SKIN_SCALE", 1.0f);
    if (!(c.scale > 0.01f && c.scale < 100.0f)) {
        c.scale = 1.0f;
    }
    c.height_m = number("BB_VR_SKIN_HEIGHT", 1.8f);
    if (!(c.height_m > 0.05f && c.height_m < 50.0f)) {
        c.height_m = 1.8f;
    }
    c.yaw_rad = number("BB_VR_SKIN_YAW", 0.0f) * m3::kDeg;
    c.bob = number("BB_VR_SKIN_BOB", 1.0f) != 0.0f;
    return c;
}

/// Loads and normalizes the configured skin. The path is tried as given, then relative to the
/// parent of `assets` (BB_VR_ASSETS/.., e.g. out/). `msg` is the one log line (loaded or why not).
inline bool LoadConfigured(const Config& cfg, const std::filesystem::path& assets, SkinMesh& mesh, std::string& msg) {
    namespace fs = std::filesystem;
    mesh = SkinMesh{};
    std::error_code ec;
    fs::path path = cfg.path;
    if (!fs::exists(path, ec)) {
        const fs::path alt = assets.parent_path() / cfg.path;
        if (!assets.empty() && fs::exists(alt, ec)) {
            path = alt;
        }
    }
    std::string err;
    fs::path used;
    if (!fs::exists(path, ec)) {
        msg = "VR skin: cannot load " + cfg.path + ": file or folder not found";
        return false;
    }
    if (!LoadSkin(path, mesh, err, &used)) {
        msg = "VR skin: cannot load " + path.string() + ": " + err;
        mesh = SkinMesh{};
        return false;
    }
    const float height = cfg.height_m * cfg.scale;
    if (!Normalize(mesh, height)) {
        msg = "VR skin: cannot load " + used.string() + ": the mesh has no height";
        mesh = SkinMesh{};
        return false;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%zu parts, %zu triangles, height %.2f m", mesh.parts.size(), mesh.tris,
                  double(height));
    msg = "VR skin: loaded " + used.string() + ": " + buf;
    return true;
}

} // namespace skin
} // namespace vrui
