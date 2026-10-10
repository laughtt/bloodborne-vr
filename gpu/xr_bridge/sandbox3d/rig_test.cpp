// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_sandbox3d_test.exe [out/vr_assets]: offline tests of sandbox3d's math and rig (no
// headset, no GPU). The rig tests use the extracted hunter (scripts/vr_assets/extract.py) and are
// skipped without it. Exit code 0 when every check passes.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "json.h"
#include "math3d.h"
#include "model.h"
#include "rig.h"

using namespace m3;

namespace {

int failures = 0, checks = 0;

void Check(bool ok, const char* what, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

bool Near(Vec3 a, Vec3 b, float eps) {
    return Length(a - b) <= eps;
}

void TestMath() {
    const Quat q = AxisAngle({0, 1, 0}, kPi / 2);
    CHECK(Near(Rotate(q, {0, 0, -1}), {-1, 0, 0}, 1e-5f)); // yaw left 90 deg: forward -> -X
    CHECK(std::fabs(YawOf(q) - kPi / 2) < 1e-5f);
    const Quat f = FromTo({1, 0, 0}, {0, 0, 1});
    CHECK(Near(Rotate(f, {1, 0, 0}), {0, 0, 1}, 1e-5f));
    const Mat4 pose = FromPose(q, {1, 2, 3});
    CHECK(Near(TransformPoint(Inverse(pose) * pose, {0.3f, -2, 5}), {0.3f, -2, 5}, 1e-4f));
    const Quat back = ToQuat(FromQuat(q));
    CHECK(Near(Rotate(back, {0.2f, 0.5f, -1}), Rotate(q, {0.2f, 0.5f, -1}), 1e-5f));
    // Mirror plane z = -2 facing +Z: (0, 1, 0) -> (0, 1, -4).
    const Mat4 mirror = Reflection({0, 0, 1}, {0, 0, -2});
    CHECK(Near(TransformPoint(mirror, {0, 1, 0}), {0, 1, -4}, 1e-5f));
    // Symmetric 45 deg FOV: a point on the right edge projects to x = +1, the top edge to y = -1
    // (Vulkan: y down), depth 0 at near and 1 at far.
    const Mat4 p = ProjectionFov(-kPi / 4, kPi / 4, kPi / 4, -kPi / 4, 0.1f, 10.0f);
    auto clip = [&](Vec3 v) {
        const float x = p.m[0] * v.x + p.m[4] * v.y + p.m[8] * v.z + p.m[12];
        const float y = p.m[1] * v.x + p.m[5] * v.y + p.m[9] * v.z + p.m[13];
        const float z = p.m[2] * v.x + p.m[6] * v.y + p.m[10] * v.z + p.m[14];
        const float w = p.m[3] * v.x + p.m[7] * v.y + p.m[11] * v.z + p.m[15];
        return Vec3{x / w, y / w, z / w};
    };
    CHECK(std::fabs(clip({1, 0, -1}).x - 1.0f) < 1e-4f);
    CHECK(std::fabs(clip({0, 1, -1}).y + 1.0f) < 1e-4f);
    CHECK(std::fabs(clip({0, 0, -0.1f}).z) < 1e-4f);
    CHECK(std::fabs(clip({0, 0, -10.0f}).z - 1.0f) < 1e-4f);
}

void TestTwoBone() {
    const Vec3 s{0, 0, 0};
    Vec3 wrist;
    bool clamped = true;
    const Vec3 e = sb::SolveTwoBone(s, {0.4f, 0, 0}, 0.3f, 0.3f, {0, -1, 0}, &wrist, &clamped);
    CHECK(!clamped);
    CHECK(Near(wrist, {0.4f, 0, 0}, 1e-5f));
    CHECK(std::fabs(Length(e - s) - 0.3f) < 1e-4f);
    CHECK(std::fabs(Length(wrist - e) - 0.3f) < 1e-4f);
    CHECK(e.y < 0.0f); // toward the pole
    sb::SolveTwoBone(s, {2.0f, 0, 0}, 0.3f, 0.3f, {0, -1, 0}, &wrist, &clamped);
    CHECK(clamped);
    CHECK(wrist.x > 0.59f && wrist.x < 0.6f);
    // AimWithTwist: the segment turns onto the new direction, the reference onto the new one.
    const Quat r = sb::AimWithTwist({1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {1, 0, 0});
    CHECK(Near(Rotate(r, {1, 0, 0}), {0, 0, -1}, 1e-4f));
    CHECK(Near(Rotate(r, {0, 1, 0}), {1, 0, 0}, 1e-4f));
}

void TestJson() {
    std::string error;
    const json::Value v = json::Parse(
        "{\"a\": [1, 2.5, -3e1], \"b\": {\"c\": true, \"d\": \"x\\u00e9\"}, // note\n \"e\": null}",
        &error);
    CHECK(error.empty());
    CHECK(v["a"].Size() == 3 && std::fabs(v["a"][2].Float() + 30.0f) < 1e-5f);
    CHECK(v["b"]["c"].Bool() && v["b"]["d"].Str() == "x\xc3\xa9");
    CHECK(v["missing"]["deeper"].Float(7.0f) == 7.0f);
    json::Parse("{\"a\": [1, }", &error);
    CHECK(!error.empty());
}

void TestRig(const std::filesystem::path& assets) {
    sb::Model body;
    std::string error;
    if (!sb::LoadModel(assets / "hunter.json", body, error)) {
        std::printf("skip rig tests: %s\n", error.c_str());
        return;
    }
    sb::Rig rig;
    CHECK(rig.Init(body, error));
    sb::Config config;
    config.body_yaw_deadzone_deg = 0.0f;
    sb::RigInput input;
    input.head_valid = true;
    input.head_position = {0.5f, 1.65f, -1.0f};
    input.head_rotation = AxisAngle({0, 1, 0}, 0.3f); // looking a bit to the left
    rig.Update(config, input, 0.0f);
    // The model's eyes land on the headset.
    const Vec3 eye_model = Translation(body.bones[size_t(body.FindBone("Head"))].bind_world) +
                           config.eye_offset;
    CHECK(Near(TransformPoint(rig.BodyWorld(), eye_model), input.head_position, 1e-3f));
    CHECK(std::fabs(rig.BodyYaw() - 0.3f) < 1e-4f);
    CHECK(std::fabs(rig.BodyScale() * rig.ModelEyeHeight(config) - config.eye_height_m) < 1e-3f);

    // Right hand 0.45 m ahead of the right shoulder, palm down: the hand bone reaches the wrist
    // target, the arm keeps its lengths.
    const int r_upper = body.FindBone("R_UpperArm"), r_fore = body.FindBone("R_Forearm");
    const int r_hand = body.FindBone("R_Hand");
    const Vec3 shoulder = Translation(rig.BoneWorld(r_upper));
    const Quat yaw = AxisAngle({0, 1, 0}, 0.3f);
    sb::HandInput& right = input.hands[1];
    right.valid = true;
    right.position = shoulder + Rotate(yaw, Vec3{0.0f, -0.15f, -0.45f});
    right.rotation = yaw;
    rig.Update(config, input, 0.0f);
    const Vec3 wrist_target =
        right.position + Rotate(right.rotation, config.hand_offset.position);
    CHECK(rig.arm_state[1].solved && !rig.arm_state[1].clamped);
    CHECK(Near(Translation(rig.BoneWorld(r_hand)), wrist_target, 2e-3f));
    const float s = rig.BodyScale();
    const float upper = Length(Translation(rig.BoneWorld(r_fore)) - shoulder);
    const float fore =
        Length(Translation(rig.BoneWorld(r_hand)) - Translation(rig.BoneWorld(r_fore)));
    const auto bind = [&](int b) { return Translation(body.bones[size_t(b)].bind_world); };
    CHECK(std::fabs(upper - Length(bind(r_fore) - bind(r_upper)) * s) < 2e-3f);
    CHECK(std::fabs(fore - Length(bind(r_hand) - bind(r_fore)) * s) < 2e-3f);
    // The elbow bends down/outward/back (the default pole), not up.
    CHECK(Translation(rig.BoneWorld(r_fore)).y < shoulder.y);
    // Fingers along the controller's -Z (forward), the thumb up.
    const int r_index = body.FindBone("R_Finger1"), r_thumb = body.FindBone("R_Finger0");
    const Vec3 knuckle = Translation(rig.BoneWorld(r_index));
    const Vec3 forward = Rotate(right.rotation * FromEulerDeg(config.hand_offset.rotation_deg),
                                {0, 0, -1});
    CHECK(Dot(Normalize(knuckle - Translation(rig.BoneWorld(r_hand))), forward) > 0.8f);
    CHECK(Translation(rig.BoneWorld(r_thumb)).y > Translation(rig.BoneWorld(r_hand)).y - 0.02f);

    // Trigger curls the index finger toward the palm.
    const int r_tip = body.FindBone("R_Finger12");
    const auto tip_to_wrist = [&] {
        return Length(Translation(rig.BoneWorld(r_tip)) - Translation(rig.BoneWorld(r_hand)));
    };
    right.trigger = 0.0f;
    config.rest_curl = 0.0f;
    rig.Update(config, input, 0.0f);
    const float open = tip_to_wrist();
    right.trigger = 1.0f;
    rig.Update(config, input, 0.0f);
    const float closed = tip_to_wrist();
    CHECK(closed < open - 0.02f);
    std::printf("rig: scale %.3f, right arm reach %.2f, index tip %.3f m open -> %.3f m curled\n",
                rig.BodyScale(), rig.arm_state[1].reach, open, closed);

    // Held weapons: a blade's long axis ends in the grip's up/forward plane, a gun's barrel
    // points where the index finger does, upright.
    {
        const int r_weapon = body.FindBone("R_Weapon");
        const Quat grip = right.rotation * FromEulerDeg(config.hand_offset.rotation_deg);
        const Quat weapon = ToQuat(rig.BoneWorld(r_weapon));
        const Vec3 blade = Rotate(weapon * rig.WeaponAlignment(1, {0, -1, 0}, false), {0, -1, 0});
        CHECK(std::fabs(Dot(blade, Rotate(grip, {1, 0, 0}))) < 1e-3f);
        const Quat gun = weapon * rig.WeaponAlignment(1, {0, 0, -1}, true);
        CHECK(Dot(Rotate(gun, {0, 0, -1}), Rotate(grip, {0, 0, -1})) > 0.999f);
        CHECK(Dot(Rotate(gun, {0, 1, 0}), Rotate(grip, {0, 1, 0})) > 0.999f);
    }

    // Out of reach: the hand stops at the arm's length along the same direction.
    right.position = shoulder + Rotate(yaw, Vec3{0.0f, 0.0f, -2.0f});
    rig.Update(config, input, 0.0f);
    CHECK(rig.arm_state[1].clamped);
    CHECK(Length(Translation(rig.BoneWorld(r_hand)) - shoulder) <
          (Length(bind(r_fore) - bind(r_upper)) + Length(bind(r_hand) - bind(r_fore))) * s +
              1e-3f);
}

} // namespace

int RunCollisionTests(const std::filesystem::path& assets, int& checks); // collision_test.cpp

int main(int argc, char** argv) {
    TestMath();
    TestTwoBone();
    TestJson();
    std::filesystem::path assets = argc > 1 ? argv[1] : "out/vr_assets";
    TestRig(assets);
    failures += RunCollisionTests(assets, checks);
    std::printf("sandbox3d tests: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
