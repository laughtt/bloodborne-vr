// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "rig.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace sb {

using namespace m3;

namespace {
VrVec3 ToVr(Vec3 v) { return {v.x, v.y, v.z}; }
VrQuat ToVr(Quat q) { return {q.x, q.y, q.z, q.w}; }
Vec3 FromVr(VrVec3 v) { return {v.x, v.y, v.z}; }
Quat FromVr(VrQuat q) { return {q.x, q.y, q.z, q.w}; }
} // namespace

// The arm IK is the game's: src/vr_arm_math.h (runtime_vr_arms.c), tests/test_vr_arms.c.
Vec3 SolveTwoBone(Vec3 s, Vec3 t, float a, float b, Vec3 pole, Vec3* wrist, bool* clamped) {
    VrVec3 w;
    int c = 0;
    const Vec3 e = FromVr(vam_two_bone(ToVr(s), ToVr(t), a, b, ToVr(pole), &w, &c));
    if (wrist) {
        *wrist = FromVr(w);
    }
    if (clamped) {
        *clamped = c != 0;
    }
    return e;
}

Quat AimWithTwist(Vec3 bind_dir, Vec3 new_dir, Vec3 bind_ref, Vec3 new_ref) {
    return FromVr(vam_aim_with_twist(ToVr(bind_dir), ToVr(new_dir), ToVr(bind_ref), ToVr(new_ref)));
}

namespace {

float WrapAngle(float a) {
    while (a > kPi) {
        a -= 2.0f * kPi;
    }
    while (a < -kPi) {
        a += 2.0f * kPi;
    }
    return a;
}

/// Rotation whose columns are the orthonormal x, y, x*y.
Quat FrameRotation(Vec3 x, Vec3 y) {
    const Vec3 z = Cross(x, y);
    Mat4 m;
    m.m[0] = x.x;
    m.m[1] = x.y;
    m.m[2] = x.z;
    m.m[4] = y.x;
    m.m[5] = y.y;
    m.m[6] = y.z;
    m.m[8] = z.x;
    m.m[9] = z.y;
    m.m[10] = z.z;
    return ToQuat(m);
}

} // namespace

bool Rig::Init(const Model& body, std::string& error) {
    const size_t n = body.bones.size();
    if (n == 0) {
        error = "the body has no bones";
        return false;
    }
    bind.resize(n);
    inv_bind.resize(n);
    local_bind.resize(n);
    bind_rot.resize(n);
    parent.resize(n);
    for (size_t b = 0; b < n; ++b) {
        bind[b] = body.bones[b].bind_world;
        inv_bind[b] = body.bones[b].inv_bind;
        bind_rot[b] = ToQuat(bind[b]);
        const int p = body.bones[b].parent;
        parent[b] = p >= 0 && size_t(p) < n && size_t(p) != b ? p : -1;
    }
    for (size_t b = 0; b < n; ++b) {
        local_bind[b] = parent[b] >= 0 ? Inverse(bind[size_t(parent[b])]) * bind[b] : bind[b];
    }
    // Parents before children.
    std::vector<std::vector<int>> children(n);
    std::deque<int> queue;
    for (size_t b = 0; b < n; ++b) {
        if (parent[b] < 0) {
            queue.push_back(int(b));
        } else {
            children[size_t(parent[b])].push_back(int(b));
        }
    }
    order.clear();
    while (!queue.empty()) {
        const int b = queue.front();
        queue.pop_front();
        order.push_back(b);
        for (int c : children[size_t(b)]) {
            queue.push_back(c);
        }
    }
    if (order.size() != n) {
        error = "the bone hierarchy has a cycle";
        return false;
    }
    local = local_bind;
    pose = bind;
    override_pose = bind;
    overridden.assign(n, 0);

    head_bone = body.FindBone("Head");
    for (int side = 0; side < 2; ++side) {
        const std::string p = side ? "R_" : "L_";
        Arm& arm = arms[side];
        arm.clavicle = body.FindBone(p + "Clavicle");
        arm.upper = body.FindBone(p + "UpperArm");
        arm.fore = body.FindBone(p + "Forearm");
        arm.hand = body.FindBone(p + "Hand");
        arm.weapon = body.FindBone(p + "Weapon");
        arm.twist = body.FindBone("Support_" + p + "ForeArmTwist");
        arm.twist1 = body.FindBone("Support_" + p + "ForeArmTwist1");
        hand_bone[side] = arm.hand;
        weapon_bone[side] = arm.weapon;
        if (arm.upper < 0 || arm.fore < 0 || arm.hand < 0) {
            error = "no " + p + "UpperArm/Forearm/Hand bones";
            return false;
        }
        arm.upper_len = Length(BindPos(arm.fore) - BindPos(arm.upper));
        arm.fore_len = Length(BindPos(arm.hand) - BindPos(arm.fore));
        if (arm.twist1 >= 0) {
            const Vec3 seg = BindPos(arm.hand) - BindPos(arm.fore);
            arm.twist1_t = std::clamp(
                Dot(BindPos(arm.twist1) - BindPos(arm.fore), seg) / std::max(Dot(seg, seg), 1e-6f),
                0.0f, 1.0f);
        }
        // The bind hand's frame: fingers from the wrist to the knuckles, thumb side.
        const Vec3 wrist = BindPos(arm.hand);
        Vec3 knuckles{};
        int count = 0;
        for (int f = 0; f < 5; ++f) {
            Arm::Finger& finger = arm.fingers[f];
            finger.thumb = f == 0;
            const std::string base = p + "Finger" + std::to_string(f);
            finger.bones[0] = body.FindBone(base);
            finger.bones[1] = body.FindBone(base + "1");
            finger.bones[2] = body.FindBone(base + "2");
            if (f > 0 && finger.bones[0] >= 0) {
                knuckles = knuckles + BindPos(finger.bones[0]);
                ++count;
            }
        }
        Vec3 fdir{0.0f, 0.0f, -1.0f}, tdir{0.0f, 1.0f, 0.0f};
        if (count > 0 && arm.fingers[0].bones[0] >= 0) {
            fdir = Normalize(knuckles * (1.0f / float(count)) - wrist, fdir);
            tdir = Normalize(Perp(BindPos(arm.fingers[0].bones[0]) - wrist, fdir), tdir);
        }
        arm.thumb_bind = tdir;
        // Grip pose: fingers along -Z, thumb up (+Y) (OpenXR grip: -Z is where the straightened
        // index finger points, +Y up).
        arm.basis = FromVr(vam_hand_basis(ToVr(fdir), ToVr(tdir), {0.0f, 0.0f, -1.0f},
                                          {0.0f, 1.0f, 0.0f}));
        // The shared IK's bind data (src/vr_arm_math.h; the game fills it from the HKX
        // reference pose).
        VamArmBind& ik = arm.ik;
        ik = {};
        ik.upper = ToVr(BindPos(arm.upper));
        ik.fore = ToVr(BindPos(arm.fore));
        ik.hand = ToVr(wrist);
        ik.upper_rot = ToVr(BindRot(arm.upper));
        ik.fore_rot = ToVr(BindRot(arm.fore));
        ik.hand_rot = ToVr(BindRot(arm.hand));
        ik.has_twist = arm.twist >= 0;
        ik.has_twist1 = arm.twist1 >= 0;
        ik.twist_rot = ToVr(arm.twist >= 0 ? BindRot(arm.twist) : BindRot(arm.fore));
        ik.twist1_rot = ToVr(arm.twist1 >= 0 ? BindRot(arm.twist1) : BindRot(arm.fore));
        ik.twist1_t = arm.twist1_t;
        ik.fingers = ToVr(fdir);
        ik.thumb = ToVr(tdir);
        const Vec3 palm_out = side ? Cross(tdir, fdir) : Cross(fdir, tdir);
        for (Arm::Finger& finger : arm.fingers) {
            for (int j = 0; j < 3; ++j) {
                const int bone = finger.bones[j];
                if (bone < 0) {
                    continue;
                }
                Vec3 dir = fdir;
                if (j < 2 && finger.bones[j + 1] >= 0) {
                    dir = Normalize(BindPos(finger.bones[j + 1]) - BindPos(bone), fdir);
                } else if (j > 0 && finger.bones[j - 1] >= 0) {
                    dir = Normalize(BindPos(bone) - BindPos(finger.bones[j - 1]), fdir);
                }
                const Vec3 axis = Normalize(Cross(dir, palm_out), {1.0f, 0.0f, 0.0f});
                finger.axis_local[j] = Rotate(Conjugate(BindRot(bone)), axis);
            }
        }
    }
    return true;
}

Quat Rig::WeaponAlignment(int side, Vec3 axis, bool gun) const {
    const int bone = weapon_bone[side] >= 0 ? weapon_bone[side] : hand_bone[side];
    if (bone < 0 || Length(axis) < 1e-6f) {
        return {};
    }
    // The weapon's frame in the grip frame (the hand follows grip * basis * bind).
    const Quat in_grip = arms[side].basis * BindRot(bone);
    const Vec3 a = Rotate(in_grip, Normalize(axis));
    Quat turn;
    if (gun) {
        // Barrel forward, the weapon's up (perpendicular to the barrel) as close to +Y as can be.
        const Vec3 up = Normalize(Perp(Rotate(in_grip, {0.0f, 1.0f, 0.0f}), a), AnyPerp(a));
        turn = FrameRotation({0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}) *
               Conjugate(FrameRotation(a, up));
    } else {
        turn = FromTo(a, Normalize(Vec3{0.0f, a.y, a.z}, a));
    }
    return Conjugate(in_grip) * turn * in_grip;
}

float Rig::ModelEyeHeight(const Config& config) const {
    if (head_bone < 0) {
        return 1.6f;
    }
    return std::max(BindPos(head_bone).y + config.eye_offset.y, 0.5f);
}

float Rig::ModelShoulderWidth() const {
    return Length(BindPos(arms[1].upper) - BindPos(arms[0].upper));
}

void Rig::ComputePose() {
    for (int b : order) {
        const size_t i = size_t(b);
        if (overridden[i]) {
            pose[i] = override_pose[i];
        } else if (parent[i] >= 0) {
            pose[i] = pose[size_t(parent[i])] * local[i];
        } else {
            pose[i] = local[i];
        }
    }
}

void Rig::Update(const Config& config, const RigInput& input, float floor_y) {
    // Body placement: size from the eye height, yaw following the head past the deadzone, the
    // model's eyes at the headset.
    scale = std::clamp(config.eye_height_m / ModelEyeHeight(config), 0.4f, 1.6f);
    if (input.head_valid) {
        const float head_yaw = YawOf(input.head_rotation);
        if (!yaw_valid) {
            body_yaw = head_yaw;
            yaw_valid = true;
        } else {
            const float diff = WrapAngle(head_yaw - body_yaw);
            const float deadzone = std::max(config.body_yaw_deadzone_deg, 0.0f) * kDeg;
            if (std::fabs(diff) > deadzone) {
                body_yaw = WrapAngle(body_yaw + diff - std::copysign(deadzone, diff));
            }
        }
    }
    const Quat body_rot = AxisAngle({0.0f, 1.0f, 0.0f}, body_yaw);
    if (input.head_valid && head_bone >= 0) {
        const Vec3 eye_model = BindPos(head_bone) + config.eye_offset;
        Vec3 origin = input.head_position - Rotate(body_rot, eye_model * scale) +
                      Rotate(body_rot, {0.0f, 0.0f, config.body_back_m});
        if (!config.follow_head_height) {
            origin.y = floor_y;
        }
        body_world = Translate(origin) * FromQuat(body_rot) * Scale(scale);
        placed = true;
    } else if (!placed) {
        body_world = Translate({0.0f, floor_y, 0.0f}) * Scale(scale);
    }

    // Pass 1: the bind pose with the head turned (the shoulders' positions).
    local = local_bind;
    std::fill(overridden.begin(), overridden.end(), 0);
    if (input.head_valid && head_bone >= 0) {
        const Quat relative = Conjugate(body_rot) * input.head_rotation;
        override_pose[size_t(head_bone)] =
            FromPose(relative * BindRot(head_bone), BindPos(head_bone));
        overridden[size_t(head_bone)] = 1;
    }
    ComputePose();

    // Arms: two-bone IK to the wrist the controller grip gives.
    const Mat4 inv_body = Inverse(body_world);
    for (int side = 0; side < 2; ++side) {
        ArmState& state = arm_state[side];
        state = {};
        const Arm& arm = arms[side];
        const HandInput& hand = input.hands[side];
        // Fingers curl whether or not the controller is tracked.
        const float rest = std::clamp(config.rest_curl, 0.0f, 1.0f);
        const float index = std::max(rest, hand.trigger) * config.trigger_curl_deg * kDeg;
        const float others = std::max(rest, hand.grip) * config.grip_curl_deg * kDeg;
        const float thumb =
            std::max(rest, std::max(hand.trigger, hand.grip)) * config.thumb_curl_deg * kDeg;
        constexpr float kFinger[3] = {0.74f, 1.0f, 0.63f}, kThumb[3] = {0.6f, 1.0f, 1.25f};
        for (int f = 0; f < 5; ++f) {
            const Arm::Finger& finger = arm.fingers[f];
            const float curl = f == 0 ? thumb : f == 1 ? index : others;
            const float* segment = f == 0 ? kThumb : kFinger;
            for (int j = 0; j < 3; ++j) {
                const int bone = finger.bones[j];
                if (bone >= 0) {
                    local[size_t(bone)] =
                        local_bind[size_t(bone)] *
                        FromQuat(AxisAngle(finger.axis_local[j], curl * segment[j]));
                }
            }
        }
        if (!hand.valid) {
            continue;
        }
        // The wrist target and the hand's rotation from the grip (the right hand's offset,
        // mirrored for the left), then the shared two-bone solve (src/vr_arm_math.h).
        VrVec3 off_pos, wrist_world;
        VrQuat off_rot, grip_rot;
        vam_side_offset(side, ToVr(config.hand_offset.position),
                        ToVr(FromEulerDeg(config.hand_offset.rotation_deg)), &off_pos, &off_rot);
        vam_grip_wrist(ToVr(hand.position), ToVr(hand.rotation), off_pos, off_rot, &wrist_world,
                       &grip_rot);
        const Vec3 target = TransformPoint(inv_body, FromVr(wrist_world));
        const VrQuat hand_rot =
            vam_hand_rotation(vr_qmul(ToVr(Conjugate(body_rot)), grip_rot), {0.0f, 0.0f, -1.0f},
                              {0.0f, 1.0f, 0.0f}, arm.ik.fingers, arm.ik.thumb, arm.ik.hand_rot);
        const Vec3 shoulder = Translation(pose[size_t(arm.upper)]);
        VamArmParams params{};
        params.pole = ToVr(config.elbow_pole);
        if (side == 0) {
            params.pole.x = -params.pole.x;
        }
        params.arm_scale = config.arm_scale;
        params.twist_share = config.forearm_twist;
        VamArmPose solved;
        vam_solve_arm(&arm.ik, ToVr(shoulder), ToVr(target), hand_rot, &params, &solved);
        const Vec3 elbow = FromVr(solved.elbow), wrist = FromVr(solved.wrist);
        const bool clamped = solved.clamped != 0;
        const float a = arm.upper_len * config.arm_scale, b = arm.fore_len * config.arm_scale;
        const auto put = [&](int bone, VrQuat rot, VrVec3 at) {
            override_pose[size_t(bone)] = FromPose(FromVr(rot), FromVr(at));
            overridden[size_t(bone)] = 1;
        };
        put(arm.upper, solved.upper_rot, solved.shoulder);
        put(arm.fore, solved.fore_rot, solved.elbow);
        put(arm.hand, solved.hand_rot, solved.wrist);
        // Forearm twist bones take part of the wrist roll (less candy-wrapping at the wrist).
        if (std::clamp(config.forearm_twist, 0.0f, 1.0f) > 0.0f && arm.twist >= 0) {
            put(arm.twist, solved.twist_rot, solved.twist_pos);
            if (arm.twist1 >= 0) {
                put(arm.twist1, solved.twist1_rot, solved.twist1_pos);
            }
        }
        state.solved = true;
        state.clamped = clamped;
        state.reach = Length(target - shoulder) / std::max(a + b, 1e-4f);
        state.shoulder = shoulder;
        state.elbow = elbow;
        state.wrist = wrist;
    }
    ComputePose();
}

void Rig::AppendPalette(const Mat4& world_from_model, std::vector<Mat4>& out) const {
    for (size_t b = 0; b < pose.size(); ++b) {
        out.push_back(world_from_model * pose[b] * inv_bind[b]);
    }
}

} // namespace sb
