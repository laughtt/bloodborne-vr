// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The custom VR hands in the sandbox (../vr_hand_model.h, docs/vr/findings/40-custom-hands.md): the
// WebXR generic hand (<assets>/hands/{left,right}.glb) skinned on the GPU through the renderer's
// palette, at each controller's grip pose, with the fingers following grip and trigger and, while
// the hand holds something, the pose of that item (the sandbox's tuned grips first, then the
// category defaults). The "hands" object of sandbox3d.json: {"model": "custom" | "current", ...}.

#pragma once

#include <filesystem>
#include <string>

#include "../vr_hand_model.h"
#include "hands_config.h"
#include "math3d.h"
#include "renderer.h"

namespace sb {

class CustomHands {
public:
    /// Loads <hands_dir>/{left,right}.glb and uploads them once.
    bool Load(const std::filesystem::path& hands_dir, Renderer& renderer, std::string& log);
    bool Ready() const { return gpu >= 0; }
    /// The hand's pose this frame: the grip pose and the finger pose it moves toward.
    void Set(int side, bool valid, m3::Vec3 position, m3::Quat rotation, const vrui::hand::Pose& target,
             double dt, const HandsConfig& config);
    /// Adds the hands to the frame (and mirrored by `mirror`, when given).
    void Append(const HandsConfig& config, const m3::Mat4* mirror, FrameData& out) const;

private:
    struct State {
        bool valid = false;
        m3::Vec3 position;
        m3::Quat rotation;
        vrui::hand::Pose pose;
        bool have_pose = false;
    };
    vrui::hand::HandModels models;
    int gpu_models[2] = {-1, -1};
    int gpu = -1;
    uint32_t index_count[2] = {};
    State state[2];
};

} // namespace sb
