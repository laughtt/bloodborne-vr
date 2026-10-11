// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "custom_hands.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace sb {

using namespace m3;

bool CustomHands::Load(const std::filesystem::path& dir, Renderer& renderer, std::string& log) {
    if (!models.Load(dir)) {
        log += "custom hands: " + (models.error.empty() ? std::string("not found") : models.error) +
               " (the hunter's own hands are shown)\n";
        return false;
    }
    for (int h = 0; h < 2; ++h) {
        const vrui::hand::HandMesh& m = models.hand[h];
        std::vector<Vertex> vertices(m.position.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            Vertex v{};
            v.position[0] = m.position[i].x, v.position[1] = m.position[i].y, v.position[2] = m.position[i].z;
            v.normal[0] = m.normal[i].x, v.normal[1] = m.normal[i].y, v.normal[2] = m.normal[i].z;
            for (int k = 0; k < 4; ++k) {
                v.bones[k] = m.joint[i][size_t(k)];
                v.weights[k] = m.weight[i][size_t(k)];
            }
            vertices[i] = v;
        }
        gpu_models[h] = renderer.UploadModel(vertices, m.indices);
        index_count[h] = uint32_t(m.indices.size());
        if (gpu_models[h] < 0) {
            log += "custom hands: upload failed\n";
            return false;
        }
    }
    gpu = 0;
    char text[160];
    std::snprintf(text, sizeof(text), "custom hands: %zu / %zu vertices\n", models.hand[0].position.size(),
                  models.hand[1].position.size());
    log += text;
    return true;
}

void CustomHands::Set(int side, bool valid, Vec3 position, Quat rotation,
                      const vrui::hand::Pose& target, double dt, const HandsConfig& config) {
    State& s = state[side];
    s.valid = valid;
    s.position = position;
    s.rotation = rotation;
    if (!s.have_pose) {
        s.pose = target;
        s.have_pose = true;
        return;
    }
    const float k = 1.0f - std::exp(-config.blend_rate * float(std::clamp(dt, 0.0, 0.1)));
    for (int j = 0; j < vrui::hand::kJoints; ++j) {
        s.pose.flex[j] += (target.flex[j] - s.pose.flex[j]) * k;
    }
}

void CustomHands::Append(const HandsConfig& config, const Mat4* mirror, FrameData& out) const {
    if (!Ready() || !config.Custom()) {
        return;
    }
    for (int h = 0; h < 2; ++h) {
        const State& s = state[h];
        if (!s.valid) {
            continue;
        }
        const vrui::hand::HandMesh& m = models.hand[h];
        // The extra offset in the grip frame; the left hand mirrors it (x, and Y/Z rotations).
        Vec3 pos = config.position, rot = config.rotation_deg;
        if (h == 0) {
            pos.x = -pos.x;
            rot.y = -rot.y;
            rot.z = -rot.z;
        }
        const Quat grip_rot = Normalize(s.rotation * FromEulerDeg(rot));
        const Vec3 grip_pos = s.position + Rotate(s.rotation, pos);
        const Mat4 world = vrui::hand::HandWorld(m, h, grip_pos, grip_rot, config.scale);
        Mat4 joints[vrui::hand::kJoints];
        vrui::hand::PoseJoints(m, s.pose, joints);
        for (int copy = 0; copy < (mirror ? 2 : 1); ++copy) {
            const uint32_t base = uint32_t(out.palette.size());
            for (int j = 0; j < vrui::hand::kJoints; ++j) {
                const Mat4 w = (copy ? *mirror * world : world) * joints[j] * m.inv_bind[j];
                out.palette.push_back(w);
            }
            MeshDraw draw;
            draw.model = gpu_models[h];
            draw.first_index = 0;
            draw.index_count = index_count[h];
            draw.palette_base = base;
            draw.color[0] = 0.80f, draw.color[1] = 0.62f, draw.color[2] = 0.52f;
            out.meshes.push_back(draw);
        }
    }
}

} // namespace sb
