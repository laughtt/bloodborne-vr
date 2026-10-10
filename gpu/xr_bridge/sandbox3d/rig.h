// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The hunter driven by the headset and controllers, pure CPU logic (unit tested by rig_test):
//   - the body stands under the head: yaw follows the head past a deadzone, the eyes (Head joint +
//     eye_offset) sit at the headset, size from the calibrated eye height;
//   - each arm is a two-bone IK chain (UpperArm, Forearm) from the shoulder to the wrist target
//     the controller grip pose gives, the elbow bending toward a pole direction; the forearm twist
//     bones take part of the wrist roll; the hand takes the controller's orientation. The solve
//     is the game's own: src/vr_arm_math.h (src/runtime_vr_arms.c, docs/vr/findings/24);
//   - fingers curl with the trigger (index), the grip (middle, ring, little) and both (thumb);
//   - the head bone follows the headset orientation.
// Poses are computed in model space (the bind pose frame) and mapped to the world by BodyWorld().

#pragma once

#include <string>
#include <vector>

#include "../../../src/vr_arm_math.h"
#include "config.h"
#include "math3d.h"
#include "model.h"

namespace sb {

struct HandInput {
    bool valid = false;
    m3::Vec3 position; // controller grip pose, world
    m3::Quat rotation;
    float trigger = 0.0f, grip = 0.0f; // 0..1
};

struct RigInput {
    bool head_valid = false;
    m3::Vec3 head_position; // between the eyes, world
    m3::Quat head_rotation;
    HandInput hands[2]; // 0 left, 1 right
};

/// Two-bone IK: the elbow position for a chain of lengths a (upper) and b (lower) from the
/// shoulder s toward the target t, bending toward `pole`. Returns the wrist actually reached
/// (t pulled in or pushed out to the reachable range) in *wrist; *clamped when it moved.
m3::Vec3 SolveTwoBone(m3::Vec3 s, m3::Vec3 t, float a, float b, m3::Vec3 pole, m3::Vec3* wrist,
                      bool* clamped);

/// The rotation turning a bind bone segment `bind_dir` to `new_dir`, rolled about `new_dir` so
/// the bind reference `bind_ref` (perpendicular to bind_dir) lands on `new_ref`.
m3::Quat AimWithTwist(m3::Vec3 bind_dir, m3::Vec3 new_dir, m3::Vec3 bind_ref, m3::Vec3 new_ref);

class Rig {
public:
    bool Init(const Model& body, std::string& error);
    void Update(const Config& config, const RigInput& input, float floor_y);
    /// Starts the body yaw at the head's again (next Update).
    void ResetYaw() { yaw_valid = false; }

    const m3::Mat4& BodyWorld() const { return body_world; }
    float BodyScale() const { return scale; }
    float BodyYaw() const { return body_yaw; }
    const std::vector<m3::Mat4>& Pose() const { return pose; }
    /// world * pose * inverse bind per bone, appended to `out` (the skinning palette).
    void AppendPalette(const m3::Mat4& world_from_model, std::vector<m3::Mat4>& out) const;
    m3::Mat4 BoneWorld(int bone) const { return body_world * pose[size_t(bone)]; }

    /// The turn, in the weapon bone's local frame, applied to a held weapon whose long axis is
    /// `axis` (weapon model space, from the grip to the far end): the bind hand holds weapons
    /// leaning sideways once the hand follows a controller. Blades: the minimal turn into the
    /// grip pose's YZ plane (up and forward, pitch kept). Guns (`gun`): the barrel along the
    /// grip's -Z (where the index finger points) with the weapon's +Y up.
    m3::Quat WeaponAlignment(int side, m3::Vec3 axis, bool gun) const;

    /// Eye height of the unscaled model above its feet (bind pose, with eye_offset).
    float ModelEyeHeight(const Config& config) const;
    /// Shoulder to wrist of the unscaled model, and the distance between the shoulders.
    float ModelArmLength() const { return arms[0].upper_len + arms[0].fore_len; }
    float ModelShoulderWidth() const;

    struct ArmState {
        bool solved = false, clamped = false;
        float reach = 0.0f; // shoulder-wrist distance over the arm length
        m3::Vec3 shoulder, elbow, wrist; // model space
    };
    ArmState arm_state[2];
    int hand_bone[2] = {-1, -1}, weapon_bone[2] = {-1, -1}, head_bone = -1;

private:
    struct Arm {
        int clavicle = -1, upper = -1, fore = -1, hand = -1, weapon = -1;
        int twist = -1, twist1 = -1; // Support_X_ForeArmTwist (wrist), ...Twist1 (mid forearm)
        float upper_len = 0.3f, fore_len = 0.3f;
        float twist1_t = 0.5f;       // where ForeArmTwist1 sits along the forearm
        m3::Quat basis;              // bind hand frame -> grip frame (fingers -Z, thumb +Y)
        m3::Vec3 thumb_bind;         // bind thumb direction (model space)
        VamArmBind ik{};             // the shared solver's bind data (src/vr_arm_math.h)
        struct Finger {
            int bones[3] = {-1, -1, -1};
            m3::Vec3 axis_local[3];  // curl axis in each bone's local frame
            bool thumb = false;
        } fingers[5];
    };

    void ComputePose();
    m3::Quat BindRot(int bone) const { return bind_rot[size_t(bone)]; }
    m3::Vec3 BindPos(int bone) const { return m3::Translation(bind[size_t(bone)]); }

    std::vector<m3::Mat4> bind, inv_bind, local_bind;
    std::vector<m3::Quat> bind_rot;
    std::vector<int> parent, order;
    std::vector<m3::Mat4> local, pose, override_pose;
    std::vector<char> overridden;
    Arm arms[2];
    m3::Mat4 body_world;
    float scale = 1.0f;
    float body_yaw = 0.0f;
    bool yaw_valid = false;
    bool placed = false; // the body was placed under a tracked head at least once
};

} // namespace sb
