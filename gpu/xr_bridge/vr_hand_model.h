// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The custom VR hands (docs/vr/findings/40-custom-hands.md, stage 1): the WebXR Input Profiles
// generic hand (<assets>/hands/left.glb, right.glb; MIT, packaging/windows/licenses/
// webxr-input-profiles-generic-hand-LICENSE.txt) drawn at each controller's grip pose in place of the
// game's hands (fp.txt hide=nohands), posed from the controllers and holding whatever item is in
// the hand at one palm socket:
//   - a minimal glTF 2.0 binary reader, limited to what that asset uses: one skinned primitive
//     (POSITION, NORMAL, JOINTS_0, WEIGHTS_0, indices), one skin whose joints are named like the 25
//     XR_EXT_hand_tracking joints without the palm. The file's joints are flat (all children of
//     the armature, each in armature space, like hand tracking's joint poses), so posing builds a
//     virtual chain per finger from the bind pose;
//   - poses as per-joint flexion about the joint's own X axis (the hand-tracking joint frame: -Z
//     along the bone toward the tip, +Y the back of the hand; a negative angle bends toward the
//     palm). Open, point and fist are named poses; the controller blends them by grip and trigger
//     (bilinear: open, trigger-only (index curled), point, fist);
//   - CPU linear-blend skinning, then the flat-lit, depth-sorted triangle path of the held items
//     (vr_held_mesh.h TriBatch): the UI layer has no depth buffer (no occlusion by the world yet);
//   - placement: the model's axes already match the OpenXR grip frame for both hands (+X: into
//     the right palm / out of the left palm, i.e. toward the user's right; -Z toward the thumb along
//     the fist; +Y back toward the wrist), so the grip pose places the hand with no rotation, about
//     the fist centre computed from the bind pose (`anchor`), plus a per-hand offset table;
//   - holding: every item maps to a hold category (bottle, small, paper, bell, tool, weapon) with a
//     default palm socket (item pose in the grip frame) and hand pose; per-item overrides on top.
// Pure CPU, no I/O except LoadGlb: unit tested in vr_game_ui_test.cpp.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "bbport_xr_bridge.h"
#include "sandbox3d/json.h"
#include "sandbox3d/math3d.h"

namespace vrui {
namespace hand {

constexpr int kJoints = 25;
enum : int {
    kWrist = 0,
    kThumbMeta, kThumbProx, kThumbDist, kThumbTip,
    kIndexMeta, kIndexProx, kIndexInter, kIndexDist, kIndexTip,
    kMiddleMeta, kMiddleProx, kMiddleInter, kMiddleDist, kMiddleTip,
    kRingMeta, kRingProx, kRingInter, kRingDist, kRingTip,
    kPinkyMeta, kPinkyProx, kPinkyInter, kPinkyDist, kPinkyTip,
};
/// The joint names of the asset (XR_EXT_hand_tracking's order without the palm: XR index = ours + 1).
inline const char* const kJointNames[kJoints] = {
    "wrist",
    "thumb-metacarpal", "thumb-phalanx-proximal", "thumb-phalanx-distal", "thumb-tip",
    "index-finger-metacarpal", "index-finger-phalanx-proximal", "index-finger-phalanx-intermediate",
    "index-finger-phalanx-distal", "index-finger-tip",
    "middle-finger-metacarpal", "middle-finger-phalanx-proximal", "middle-finger-phalanx-intermediate",
    "middle-finger-phalanx-distal", "middle-finger-tip",
    "ring-finger-metacarpal", "ring-finger-phalanx-proximal", "ring-finger-phalanx-intermediate",
    "ring-finger-phalanx-distal", "ring-finger-tip",
    "pinky-finger-metacarpal", "pinky-finger-phalanx-proximal", "pinky-finger-phalanx-intermediate",
    "pinky-finger-phalanx-distal", "pinky-finger-tip",
};

/// The joint's parent in the virtual chains (metacarpals hang off the wrist); parents come first.
inline int Parent(int j) {
    if (j <= kWrist) {
        return -1;
    }
    if (j == kThumbMeta || j == kIndexMeta || j == kMiddleMeta || j == kRingMeta || j == kPinkyMeta) {
        return kWrist;
    }
    return j - 1;
}

struct HandMesh {
    bool loaded = false;
    std::vector<m3::Vec3> position, normal;
    std::vector<std::array<uint8_t, 4>> joint; // our joint indices
    std::vector<std::array<float, 4>> weight;  // normalised
    std::vector<uint32_t> indices;
    m3::Mat4 bind[kJoints];     // each joint's frame in model space (bind pose)
    m3::Mat4 inv_bind[kJoints]; // the file's inverse bind matrices
    m3::Mat4 local[kJoints];    // the bind frame relative to the chain parent
    m3::Vec3 anchor;            // the fist centre (the grip origin) in model space
    m3::Vec3 dorsal;            // the back of the hand (the wrist's +Y) in model space
};

namespace detail {

inline int ComponentSize(int type) {
    switch (type) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    default: return 0;
    }
}

inline int TypeCount(const std::string& t) {
    return t == "SCALAR" ? 1 : t == "VEC2" ? 2 : t == "VEC3" ? 3 : t == "VEC4" ? 4 : t == "MAT4" ? 16 : 0;
}

struct Accessor {
    const uint8_t* data = nullptr;
    size_t count = 0, stride = 0;
    int type = 0, components = 0;
    bool normalized = false;

    float Get(size_t i, int k) const {
        const uint8_t* p = data + i * stride + size_t(k) * size_t(ComponentSize(type));
        switch (type) {
        case 5126: {
            float f;
            std::memcpy(&f, p, 4);
            return f;
        }
        case 5121: return normalized ? float(*p) / 255.0f : float(*p);
        case 5123: {
            uint16_t v;
            std::memcpy(&v, p, 2);
            return normalized ? float(v) / 65535.0f : float(v);
        }
        case 5125: {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return float(v);
        }
        default: return 0.0f;
        }
    }
    uint32_t GetU(size_t i, int k) const {
        const uint8_t* p = data + i * stride + size_t(k) * size_t(ComponentSize(type));
        switch (type) {
        case 5121: return *p;
        case 5123: {
            uint16_t v;
            std::memcpy(&v, p, 2);
            return v;
        }
        case 5125: {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        default: return 0;
        }
    }
};

inline bool GetAccessor(const json::Value& doc, const std::vector<uint8_t>& bin, int index, int want_components,
                        Accessor& a, std::string& error) {
    const json::Value& acc = doc["accessors"][size_t(std::max(index, 0))];
    if (index < 0 || !acc.IsObject() || !acc.Has("bufferView") || acc.Has("sparse")) {
        error = "accessor " + std::to_string(index) + " missing or sparse";
        return false;
    }
    const json::Value& view = doc["bufferViews"][size_t(std::max(acc["bufferView"].Int(-1), 0))];
    if (!view.IsObject() || view["buffer"].Int(0) != 0) {
        error = "accessor " + std::to_string(index) + ": bad buffer view";
        return false;
    }
    a.type = acc["componentType"].Int();
    a.components = TypeCount(acc["type"].Str());
    a.count = size_t(std::max(acc["count"].Int(), 0));
    a.normalized = acc["normalized"].Bool(false);
    const int csize = ComponentSize(a.type);
    if (!csize || a.components != want_components) {
        error = "accessor " + std::to_string(index) + ": unexpected type";
        return false;
    }
    const size_t elem = size_t(csize) * size_t(a.components);
    a.stride = view.Has("byteStride") ? size_t(view["byteStride"].Int()) : elem;
    const size_t offset = size_t(view["byteOffset"].Int(0)) + size_t(acc["byteOffset"].Int(0));
    const size_t view_end = size_t(view["byteOffset"].Int(0)) + size_t(view["byteLength"].Int(0));
    if (a.stride < elem || view_end > bin.size() ||
        (a.count && offset + a.stride * (a.count - 1) + elem > view_end)) {
        error = "accessor " + std::to_string(index) + ": out of the buffer";
        return false;
    }
    a.data = bin.data() + offset;
    return true;
}

inline m3::Mat4 NodeLocal(const json::Value& n) {
    m3::Mat4 m;
    if (n["matrix"].Size() == 16) {
        for (int i = 0; i < 16; ++i) {
            m.m[i] = n["matrix"][size_t(i)].Float();
        }
        return m;
    }
    m3::Vec3 t{n["translation"][0].Float(0), n["translation"][1].Float(0), n["translation"][2].Float(0)};
    m3::Quat r{n["rotation"][0].Float(0), n["rotation"][1].Float(0), n["rotation"][2].Float(0),
               n["rotation"][3].Float(1)};
    const json::Value& s = n["scale"];
    m3::Mat4 scale;
    if (s.Size() == 3) {
        scale.m[0] = s[0].Float(1);
        scale.m[5] = s[1].Float(1);
        scale.m[10] = s[2].Float(1);
    }
    return m3::FromPose(m3::Normalize(r), t) * scale;
}

/// A rigid copy of `m` (unit axes): the asset's joint scales are 1 within float noise.
inline m3::Mat4 Rigid(const m3::Mat4& m) { return m3::FromPose(m3::ToQuat(m), m3::Translation(m)); }

} // namespace detail

/// Parses a .glb image into `out` (the hand asset's subset of glTF 2.0; see the header comment).
inline bool ParseGlb(const std::vector<uint8_t>& file, HandMesh& out, std::string& error) {
    out = HandMesh{};
    auto u32 = [&](size_t at) {
        uint32_t v = 0;
        std::memcpy(&v, file.data() + at, 4);
        return v;
    };
    if (file.size() < 28 || u32(0) != 0x46546C67u || u32(4) != 2u) {
        error = "not a glTF 2.0 binary";
        return false;
    }
    const size_t json_len = u32(12);
    if (u32(16) != 0x4E4F534Au || 20 + json_len + 8 > file.size()) {
        error = "no JSON chunk";
        return false;
    }
    const size_t bin_at = 20 + json_len;
    const size_t bin_len = u32(bin_at);
    if (u32(bin_at + 4) != 0x004E4942u || bin_at + 8 + bin_len > file.size()) {
        error = "no BIN chunk";
        return false;
    }
    const std::vector<uint8_t> bin(file.begin() + std::ptrdiff_t(bin_at + 8),
                                   file.begin() + std::ptrdiff_t(bin_at + 8 + bin_len));
    std::string parse_error;
    const json::Value doc = json::Parse(std::string(file.begin() + 20, file.begin() + std::ptrdiff_t(20 + json_len)),
                                        &parse_error);
    if (!doc.IsObject()) {
        error = "JSON: " + parse_error;
        return false;
    }
    // The node whose mesh is skinned.
    const json::Value& nodes = doc["nodes"];
    int mesh_node = -1;
    for (size_t i = 0; i < nodes.Size(); ++i) {
        if (nodes[i].Has("mesh") && nodes[i].Has("skin")) {
            mesh_node = int(i);
            break;
        }
    }
    if (mesh_node < 0) {
        error = "no skinned mesh";
        return false;
    }
    const json::Value& skin = doc["skins"][size_t(nodes[size_t(mesh_node)]["skin"].Int())];
    const json::Value& prim = doc["meshes"][size_t(nodes[size_t(mesh_node)]["mesh"].Int())]["primitives"][0];
    if (!skin.IsObject() || !prim.IsObject() || prim["mode"].Int(4) != 4) {
        error = "no skin or no triangle primitive";
        return false;
    }
    // Node parents and world matrices (the armature may carry a transform).
    std::vector<int> parent(nodes.Size(), -1);
    for (size_t i = 0; i < nodes.Size(); ++i) {
        const json::Value& ch = nodes[i]["children"];
        for (size_t c = 0; c < ch.Size(); ++c) {
            const int k = ch[c].Int(-1);
            if (k >= 0 && size_t(k) < nodes.Size()) {
                parent[size_t(k)] = int(i);
            }
        }
    }
    auto world = [&](int n) {
        m3::Mat4 m;
        for (int depth = 0; n >= 0 && depth < 64; ++depth, n = parent[size_t(n)]) {
            m = detail::NodeLocal(nodes[size_t(n)]) * m;
        }
        return m;
    };
    // Skin joints -> ours, by name.
    const json::Value& sj = skin["joints"];
    std::vector<int> map(sj.Size(), -1);
    bool have[kJoints] = {};
    detail::Accessor ibm;
    if (!detail::GetAccessor(doc, bin, skin["inverseBindMatrices"].Int(-1), 16, ibm, error) || ibm.count != sj.Size()) {
        error = "inverse bind matrices: " + error;
        return false;
    }
    for (size_t k = 0; k < sj.Size(); ++k) {
        const int node = sj[k].Int(-1);
        if (node < 0 || size_t(node) >= nodes.Size()) {
            continue;
        }
        const std::string& name = nodes[size_t(node)]["name"].Str();
        for (int j = 0; j < kJoints; ++j) {
            if (name == kJointNames[j]) {
                map[k] = j;
                have[j] = true;
                out.bind[j] = detail::Rigid(world(node));
                for (int e = 0; e < 16; ++e) {
                    out.inv_bind[j].m[e] = ibm.Get(k, e);
                }
            }
        }
    }
    for (int j = 0; j < kJoints; ++j) {
        if (!have[j]) {
            error = std::string("no joint ") + kJointNames[j];
            return false;
        }
    }
    // Vertices.
    const json::Value& at = prim["attributes"];
    detail::Accessor pos, nrm, jnt, wgt, idx;
    if (!detail::GetAccessor(doc, bin, at["POSITION"].Int(-1), 3, pos, error) ||
        !detail::GetAccessor(doc, bin, at["NORMAL"].Int(-1), 3, nrm, error) ||
        !detail::GetAccessor(doc, bin, at["JOINTS_0"].Int(-1), 4, jnt, error) ||
        !detail::GetAccessor(doc, bin, at["WEIGHTS_0"].Int(-1), 4, wgt, error) ||
        !detail::GetAccessor(doc, bin, prim["indices"].Int(-1), 1, idx, error)) {
        return false;
    }
    const size_t n = pos.count;
    if (!n || nrm.count != n || jnt.count != n || wgt.count != n || idx.count < 3 || jnt.type == 5126 ||
        idx.type == 5126) {
        error = "inconsistent vertex attributes";
        return false;
    }
    out.position.resize(n);
    out.normal.resize(n);
    out.joint.resize(n);
    out.weight.resize(n);
    for (size_t i = 0; i < n; ++i) {
        out.position[i] = {pos.Get(i, 0), pos.Get(i, 1), pos.Get(i, 2)};
        out.normal[i] = m3::Normalize(m3::Vec3{nrm.Get(i, 0), nrm.Get(i, 1), nrm.Get(i, 2)}, {0, 1, 0});
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const uint32_t sk = jnt.GetU(i, k);
            float w = wgt.Get(i, k);
            int ours = sk < map.size() ? map[sk] : -1;
            if (ours < 0) { // a joint we do not drive: none in the asset
                ours = kWrist;
                w = 0.0f;
            }
            out.joint[i][size_t(k)] = uint8_t(ours);
            out.weight[i][size_t(k)] = std::max(w, 0.0f);
            sum += std::max(w, 0.0f);
        }
        if (sum <= 1e-6f) {
            out.weight[i] = {1.0f, 0.0f, 0.0f, 0.0f};
            out.joint[i] = {uint8_t(kWrist), 0, 0, 0};
        } else {
            for (float& w : out.weight[i]) {
                w /= sum;
            }
        }
    }
    out.indices.resize(idx.count - idx.count % 3);
    for (size_t i = 0; i < out.indices.size(); ++i) {
        out.indices[i] = idx.GetU(i, 0);
        if (out.indices[i] >= n) {
            error = "index out of range";
            return false;
        }
    }
    // The chains, the back of the hand and the fist centre.
    for (int j = 0; j < kJoints; ++j) {
        const int p = Parent(j);
        out.local[j] = p < 0 ? out.bind[j] : m3::Inverse(out.bind[p]) * out.bind[j];
    }
    out.dorsal = m3::Normalize(m3::TransformDir(out.bind[kWrist], {0, 1, 0}), {1, 0, 0});
    m3::Vec3 knuckles;
    for (int j : {kIndexProx, kMiddleProx, kRingProx, kPinkyProx}) {
        knuckles = knuckles + m3::Translation(out.bind[j]) * 0.25f;
    }
    const m3::Vec3 to_wrist = m3::Normalize(m3::Translation(out.bind[kWrist]) - knuckles, {0, 1, 0});
    // The fist's centre: in front of the palm under the knuckles (a 4 cm controller handle).
    out.anchor = knuckles - out.dorsal * 0.03f + to_wrist * 0.012f;
    out.loaded = true;
    return true;
}

inline bool LoadGlb(const std::filesystem::path& path, HandMesh& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path.string();
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!ParseGlb(bytes, out, error)) {
        error = path.string() + ": " + error;
        return false;
    }
    return true;
}

// ---- Poses -----------------------------------------------------------------------------------

/// Flexion per joint in degrees about the joint's X axis (negative: toward the palm).
struct Pose {
    float flex[kJoints] = {};
};

/// A pose from per-finger flexions: thumb {metacarpal, proximal, distal}, fingers {metacarpal,
/// proximal, intermediate, distal}.
inline Pose MakePose(std::array<float, 3> thumb, std::array<float, 4> index, std::array<float, 4> middle,
                     std::array<float, 4> ring, std::array<float, 4> pinky) {
    Pose p;
    for (int k = 0; k < 3; ++k) {
        p.flex[kThumbMeta + k] = thumb[size_t(k)];
    }
    const std::array<float, 4>* f[4] = {&index, &middle, &ring, &pinky};
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 4; ++k) {
            p.flex[kIndexMeta + i * 5 + k] = (*f[i])[size_t(k)];
        }
    }
    return p;
}

/// The asset's bind pose is a relaxed open hand.
inline const Pose& OpenPose() {
    static const Pose p;
    return p;
}
/// A closed fist around a ~4 cm handle.
inline const Pose& FistPose() {
    static const Pose p = MakePose({-12, -30, -40}, {0, -72, -80, -45}, {0, -78, -85, -45},
                                   {-4, -82, -85, -45}, {-8, -85, -80, -45});
    return p;
}
/// The fist with the index straight.
inline const Pose& PointPose() {
    static const Pose p = [] {
        Pose q = FistPose();
        for (int j = kIndexMeta; j <= kIndexTip; ++j) {
            q.flex[j] = 0.0f;
        }
        return q;
    }();
    return p;
}
/// The open hand with the index curled (trigger pulled, grip open).
inline const Pose& TriggerPose() {
    static const Pose p = [] {
        Pose q;
        for (int j = kIndexMeta; j <= kIndexTip; ++j) {
            q.flex[j] = FistPose().flex[j];
        }
        return q;
    }();
    return p;
}

/// The weighted sum of poses (weights normalised; all zero: the first pose).
inline Pose Blend(const Pose* const* poses, const float* weights, int n) {
    Pose out;
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        sum += std::max(weights[i], 0.0f);
    }
    if (n <= 0) {
        return out;
    }
    if (sum <= 1e-6f) {
        return *poses[0];
    }
    for (int i = 0; i < n; ++i) {
        const float w = std::max(weights[i], 0.0f) / sum;
        for (int j = 0; j < kJoints; ++j) {
            out.flex[j] += poses[i]->flex[j] * w;
        }
    }
    return out;
}

/// The blend weights of the controller pose: grip closes the middle, ring, pinky and thumb, the
/// trigger the index (bilinear over open / trigger-only / point / fist).
struct ControllerWeights {
    float open, trigger, point, fist;
};
inline ControllerWeights WeightsFor(float grip, float trigger) {
    const float g = std::clamp(grip, 0.0f, 1.0f), t = std::clamp(trigger, 0.0f, 1.0f);
    return {(1 - g) * (1 - t), (1 - g) * t, g * (1 - t), g * t};
}
inline Pose ControllerPose(float grip, float trigger) {
    const ControllerWeights w = WeightsFor(grip, trigger);
    const Pose* poses[4] = {&OpenPose(), &TriggerPose(), &PointPose(), &FistPose()};
    const float weights[4] = {w.open, w.trigger, w.point, w.fist};
    return Blend(poses, weights, 4);
}

/// A pose with each finger a share of the way from open to the fist.
inline Pose CurlPose(float thumb, float index, float others) {
    Pose p;
    const Pose& fist = FistPose();
    for (int j = 0; j < kJoints; ++j) {
        const float s = j >= kThumbMeta && j <= kThumbTip   ? thumb
                        : j >= kIndexMeta && j <= kIndexTip ? index
                        : j >= kMiddleMeta                  ? others
                                                            : 0.0f;
        p.flex[j] = fist.flex[j] * std::clamp(s, 0.0f, 1.0f);
    }
    return p;
}

/// The posed joint frames in model space.
inline void PoseJoints(const HandMesh& m, const Pose& p, m3::Mat4 out[kJoints]) {
    for (int j = 0; j < kJoints; ++j) {
        const int parent = Parent(j);
        const m3::Mat4 flex = m3::FromQuat(m3::AxisAngle({1, 0, 0}, p.flex[j] * m3::kDeg));
        out[j] = (parent < 0 ? m.bind[j] : out[parent] * m.local[j]) * flex;
    }
}

/// Linear-blend skinning of the whole mesh into `world` space.
inline void Skin(const HandMesh& m, const m3::Mat4 joints[kJoints], const m3::Mat4& world,
                 std::vector<m3::Vec3>& pos, std::vector<m3::Vec3>& nrm) {
    m3::Mat4 skin[kJoints];
    for (int j = 0; j < kJoints; ++j) {
        skin[j] = world * joints[j] * m.inv_bind[j];
    }
    pos.resize(m.position.size());
    nrm.resize(m.position.size());
    for (size_t i = 0; i < m.position.size(); ++i) {
        m3::Vec3 p, n;
        for (int k = 0; k < 4; ++k) {
            const float w = m.weight[i][size_t(k)];
            if (w <= 0.0f) {
                continue;
            }
            const m3::Mat4& s = skin[m.joint[i][size_t(k)]];
            p = p + m3::TransformPoint(s, m.position[i]) * w;
            n = n + m3::TransformDir(s, m.normal[i]) * w;
        }
        pos[i] = p;
        nrm[i] = m3::Normalize(n, {0, 1, 0});
    }
}

// ---- Placement and holding ---------------------------------------------------------------------

/// The hand model relative to the grip pose, per hand (0 left, 1 right): `pos` in the grip frame
/// (metres), `rot_deg` extra Euler degrees. The one place to tune where the hands sit.
struct Placement {
    m3::Vec3 pos, rot_deg;
};
inline const Placement kHandPlacement[2] = {
    {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}, // left
    {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}, // right
};

/// Model space -> world for hand `h` at its grip pose (the anchor at the grip origin).
inline m3::Mat4 HandWorld(const HandMesh& m, int h, m3::Vec3 grip_pos, m3::Quat grip_rot, float scale = 1.0f) {
    const Placement& pl = kHandPlacement[h ? 1 : 0];
    return m3::FromPose(grip_rot, grip_pos) * m3::FromPose(m3::FromEulerDeg(pl.rot_deg), pl.pos) *
           m3::Scale(scale) * m3::Translate(-m.anchor);
}

/// How an item is held: every item falls in one of these.
enum class HoldCategory { Bottle, Small, Paper, Bell, Tool, Weapon };

inline const char* CategoryName(HoldCategory c) {
    switch (c) {
    case HoldCategory::Bottle: return "bottle";
    case HoldCategory::Small: return "small";
    case HoldCategory::Paper: return "paper";
    case HoldCategory::Bell: return "bell";
    case HoldCategory::Tool: return "tool";
    case HoldCategory::Weapon: return "weapon";
    }
    return "?";
}

/// A palm socket and the hand's pose around it. Socket: the item's centre and orientation in the
/// RIGHT hand's grip frame (the left hand mirrors it across the grip's X); curls: the share of
/// the fist per finger (thumb, index, the other three).
struct GripDef {
    m3::Vec3 pos, rot_deg;
    float thumb = 0.8f, index = 0.8f, others = 0.8f;
};

/// The category defaults (the extracted meshes are recentred on their bounding box; long items
/// along their model +Y, which the sockets turn along the fist toward the thumb, grip -Z).
inline const GripDef& CategoryGrip(HoldCategory c) {
    static const GripDef kBottle{{0.0f, 0.0f, -0.015f}, {-90.0f, 0.0f, 0.0f}, 0.65f, 0.70f, 0.70f};
    static const GripDef kSmall{{-0.015f, -0.015f, -0.035f}, {0.0f, 0.0f, 0.0f}, 0.75f, 0.85f, 0.90f};
    static const GripDef kPaper{{-0.015f, -0.035f, -0.055f}, {0.0f, 0.0f, 0.0f}, 0.45f, 0.45f, 0.95f};
    static const GripDef kBell{{0.0f, 0.0f, 0.045f}, {-90.0f, 0.0f, 0.0f}, 0.80f, 0.85f, 0.85f};
    static const GripDef kTool{{0.0f, 0.0f, -0.02f}, {-90.0f, 0.0f, 0.0f}, 0.80f, 0.80f, 0.85f};
    static const GripDef kWeapon{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.85f, 0.85f, 0.90f};
    switch (c) {
    case HoldCategory::Bottle: return kBottle;
    case HoldCategory::Small: return kSmall;
    case HoldCategory::Paper: return kPaper;
    case HoldCategory::Bell: return kBell;
    case HoldCategory::Tool: return kTool;
    case HoldCategory::Weapon: return kWeapon;
    }
    return kSmall;
}

/// The category of a game item id (goods 0x40000000 | row; anything else a weapon). Goods rows
/// by range (out/vr_assets/items/items.json names); anything unlisted is a small object.
inline HoldCategory CategoryFor(uint32_t game_id) {
    if (game_id == BBXR_ITEM_NONE) {
        return HoldCategory::Small;
    }
    if ((game_id & 0xF0000000u) != BBXR_ITEM_GOODS) {
        return HoldCategory::Weapon;
    }
    const int g = int(game_id & 0x0FFFFFFFu);
    static const struct {
        int lo, hi;
        HoldCategory c;
    } kRanges[] = {
        {700, 700, HoldCategory::Small},     // Tiny Music Box
        {701, 706, HoldCategory::Bottle},    // the blood vials of Arianna, Adella, Iosefka, Adeline, Queenly Blood
        {1000, 1000, HoldCategory::Bottle},  // Blood Vial
        {1100, 1101, HoldCategory::Bottle},  // Antidote, Sedative
        {1120, 1120, HoldCategory::Bottle},  // Blue Elixir
        {1200, 1201, HoldCategory::Bottle},  // Molotov, Delayed Molotov
        {1210, 1210, HoldCategory::Tool},    // Poison Knife
        {1220, 1220, HoldCategory::Bottle},  // Oil Urn
        {1240, 1240, HoldCategory::Tool},    // Throwing Knife
        {1250, 1270, HoldCategory::Bottle},  // Rope Molotovs, Pungent Blood Cocktail, Numbing Mist
        {1300, 1300, HoldCategory::Paper},   // Fire Paper
        {1320, 1320, HoldCategory::Paper},   // Bolt Paper
        {1411, 1412, HoldCategory::Tool},    // Monocular, Hand Lantern
        {120, 120, HoldCategory::Paper},     // Notebook
        {131, 131, HoldCategory::Tool},      // League Staff
        {200, 225, HoldCategory::Bell},      // Beckoning, Small and Sinister Resonant Bells
        {2030, 2030, HoldCategory::Bottle},  // Lead Elixir
        {2050, 2050, HoldCategory::Bell},    // Choir Bell
        {2060, 2060, HoldCategory::Tool},    // Old Hunter Bone
        {2070, 2070, HoldCategory::Tool},    // Tiny Tonitrus
        {2090, 2090, HoldCategory::Tool},    // Shaman Bone Blade
        {2130, 2130, HoldCategory::Bottle},  // Accursed Brew
        {4003, 4003, HoldCategory::Paper},   // Cainhurst Summons
        {4102, 4104, HoldCategory::Tool},    // workshop tools
        {4105, 4105, HoldCategory::Bottle},  // Short Ritual Root Chalice
        {4308, 4308, HoldCategory::Paper},   // Unopened Summons
        {4312, 4312, HoldCategory::Bell},    // Old Hunter Bell
        {4340, 4342, HoldCategory::Bottle},  // Brain Fluid
        {6100, 6599, HoldCategory::Bottle},  // the chalices (cups)
    };
    for (const auto& r : kRanges) {
        if (g >= r.lo && g <= r.hi) {
            return r.c;
        }
    }
    return HoldCategory::Small; // pellets, shards, keys, badges, runes, gems, coldblood, materials
}

/// Per-item exceptions to the category default (goods row -> socket and pose). The sandbox's
/// tuned meshes (vr_held_mesh.h MeshTable) come next: their grip pose already is a socket.
struct ItemGrip {
    int goods;
    GripDef grip;
};
inline const std::vector<ItemGrip>& ItemGrips() {
    static const std::vector<ItemGrip> v = {
        // {goods, {{pos}, {rot_deg}, thumb, index, others}}. Mesh +Y is -Z (thumb side) with rot X -90.
        // Knives: the mesh's long axis is Z, so no turn: blade out of the thumb side, flat vertical.
        {1210, {{0.0f, 0.0f, -0.045f}, {0.0f, 0.0f, 0.0f}, 0.80f, 0.80f, 0.85f}},
        {1240, {{0.0f, 0.0f, -0.045f}, {0.0f, 0.0f, 0.0f}, 0.80f, 0.80f, 0.85f}},
        // Molotovs: held low on the neck, the body out of the little-finger side (a throw grip).
        {1200, {{0.0f, 0.0f, 0.03f}, {-90.0f, 0.0f, 0.0f}, 0.65f, 0.70f, 0.70f}},
        {1201, {{0.0f, 0.0f, 0.03f}, {-90.0f, 0.0f, 0.0f}, 0.65f, 0.70f, 0.70f}},
        {1250, {{0.0f, 0.0f, 0.03f}, {-90.0f, 0.0f, 0.0f}, 0.65f, 0.70f, 0.70f}},
        {1251, {{0.0f, 0.0f, 0.03f}, {-90.0f, 0.0f, 0.0f}, 0.65f, 0.70f, 0.70f}},
        // The coin pinched at the front of the fist.
        {1410, {{0.0f, -0.01f, -0.045f}, {0.0f, 0.0f, 0.0f}, 0.75f, 0.85f, 0.90f}},
    };
    return v;
}

/// The socket and pose for `game_id`: the per-item table, else `sandbox` (a tuned mesh pose in
/// the grip frame, when the caller has one) with the category's curls, else the category.
inline GripDef GripFor(uint32_t game_id, const GripDef* sandbox = nullptr) {
    const int goods = (game_id & 0xF0000000u) == BBXR_ITEM_GOODS ? int(game_id & 0x0FFFFFFFu) : -1;
    for (const ItemGrip& g : ItemGrips()) {
        if (g.goods == goods) {
            return g.grip;
        }
    }
    GripDef d = CategoryGrip(CategoryFor(game_id));
    if (sandbox) {
        d.pos = sandbox->pos;
        d.rot_deg = sandbox->rot_deg;
    }
    return d;
}

/// The socket's frame in world space for hand `h` (the left hand mirrors the right's socket
/// across the grip's X: x and the Y/Z rotations change sign).
inline m3::Mat4 SocketWorld(const GripDef& g, int h, m3::Vec3 grip_pos, m3::Quat grip_rot) {
    m3::Vec3 pos = g.pos, rot = g.rot_deg;
    if (!h) {
        pos.x = -pos.x;
        rot.y = -rot.y;
        rot.z = -rot.z;
    }
    return m3::FromPose(grip_rot, grip_pos) * m3::FromPose(m3::FromEulerDeg(rot), pos);
}

/// The hand's pose while it holds an item of grip `g`; the trigger can still close the index
/// further (a gun's trigger, a weapon held with the index on it).
inline Pose HoldPose(const GripDef& g, float trigger = 0.0f) {
    return CurlPose(g.thumb, std::max(g.index, std::clamp(trigger, 0.0f, 1.0f)), g.others);
}

/// The two hand models, loaded once from <assets>/hands.
struct HandModels {
    bool tried = false;
    HandMesh hand[2];
    std::string error;

    bool Ready() const { return hand[0].loaded && hand[1].loaded; }
    bool Load(const std::filesystem::path& dir) {
        tried = true;
        std::string e0, e1;
        const bool l = LoadGlb(dir / "left.glb", hand[0], e0);
        const bool r = LoadGlb(dir / "right.glb", hand[1], e1);
        error = l && r ? "" : (e0.empty() ? e1 : e1.empty() ? e0 : e0 + "; " + e1);
        return l && r;
    }
};

} // namespace hand
} // namespace vrui
