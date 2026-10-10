// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The VR menus (ui.h) with the sandbox scene: their input and body from the scene's, and the offline
// renders (bbport_xr_sandbox3d.exe --offline): scripted poses, grips, sticks and button presses
// drive the menus frame by frame exactly as the headset would (FilterButtons, Scene::Update,
// UiSystem::Update at 72 Hz), then a first-person view (left) and an outside view (right) go to
// <out>/ui_<scenario>.png. The log goes to <assets>/ui_log/offline_<time>.jsonl.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

#include "bbport_xr_bridge.h"
#include "image_out.h"
#include "scene.h"
#include "ui.h"

namespace sb {

using namespace m3;

UiInput UiInputOf(const SceneInput& input, uint32_t buttons, uint8_t lx, uint8_t ly, uint8_t rx,
                  uint8_t ry, float floor_y) {
    const auto pose = [](const TrackedPose& p) { return UiPose{p.valid, p.position, p.rotation}; };
    UiInput out;
    out.time = input.time;
    out.stage = input.stage;
    out.floor_y = floor_y;
    out.head = pose(input.head);
    for (int h = 0; h < 2; ++h) {
        out.grip[h] = pose(input.grip[h]);
        out.aim[h] = pose(input.aim[h]);
        out.trigger[h] = input.trigger[h];
        out.squeeze[h] = input.squeeze[h];
    }
    out.buttons = buttons;
    UiSticks(lx, ly, rx, ry, out.stick);
    return out;
}

UiBody UiBodyOf(const Scene& scene, const UiInput& input) {
    UiBody out;
    const Rig& rig = scene.GetRig();
    const Model& body = scene.Body();
    out.yaw = rig.BodyYaw();
    static const char* const kTips[2][2] = {{"L_Finger11", "L_Finger12"},
                                            {"R_Finger11", "R_Finger12"}};
    for (int h = 0; h < 2; ++h) {
        const int b11 = body.FindBone(kTips[h][0]), b12 = body.FindBone(kTips[h][1]);
        if (input.grip[h].valid && b11 >= 0 && b12 >= 0 && size_t(b11) < rig.Pose().size() &&
            size_t(b12) < rig.Pose().size()) {
            // The distal phalanx continues past its joint about as long as the middle one.
            const Vec3 p11 = Translation(rig.BoneWorld(b11));
            const Vec3 p12 = Translation(rig.BoneWorld(b12));
            out.tip[h] = p12 + (p12 - p11) * 0.9f;
            out.tip_valid[h] = true;
        } else if (input.aim[h].valid) {
            out.tip[h] = input.aim[h].position + Rotate(input.aim[h].rotation, {0, 0, -0.06f});
            out.tip_valid[h] = true;
        }
    }
    return out;
}

namespace {

Quat LookRotation(Vec3 from, Vec3 to, Vec3 up = {0, 1, 0}) {
    const Vec3 f = Normalize(to - from, {0, 0, -1});
    const Vec3 r = Normalize(Cross(f, up), {1, 0, 0});
    const Vec3 u = Cross(r, f);
    Mat4 m;
    m.m[0] = r.x, m.m[1] = r.y, m.m[2] = r.z;
    m.m[4] = u.x, m.m[5] = u.y, m.m[6] = u.z;
    m.m[8] = -f.x, m.m[9] = -f.y, m.m[10] = -f.z;
    return ToQuat(m);
}

/// A rotation whose +X is `x` (the left grip's palm normal) and +Y as close to `up` as possible.
Quat Basis(Vec3 x, Vec3 up) {
    x = Normalize(x, {1, 0, 0});
    const Vec3 z = Normalize(Cross(x, up), {0, 0, 1});
    const Vec3 y = Cross(z, x);
    Mat4 m;
    m.m[0] = x.x, m.m[1] = x.y, m.m[2] = x.z;
    m.m[4] = y.x, m.m[5] = y.y, m.m[6] = y.z;
    m.m[8] = z.x, m.m[9] = z.y, m.m[10] = z.z;
    return ToQuat(m);
}

Mat4 ViewProj(Vec3 eye, Vec3 target, float tan_half, const Config& config) {
    const float a = std::atan(tan_half);
    return ProjectionFov(-a, a, a, -a, config.near_z, config.far_z) *
           Inverse(FromPose(LookRotation(eye, target), eye));
}

UiPose Pose(Vec3 p, Quat q) {
    return {true, p, q};
}

struct Scenario {
    const char* name;
    const char* preset;
    int frames;
    std::function<void(UiInput&, int)> script;
    std::function<Vec3()> look_at;        // the head looks here (the first-person view)
    Vec3 camera, target;                  // outside view
    float tan_half = 0.62f;
    bool aim_head = true;                 // false: keep the script's head rotation
};

} // namespace

int RenderUiPreviews(const std::filesystem::path& assets, Renderer& renderer, Scene& scene,
                     const Config& base_config, const std::filesystem::path& out_dir,
                     uint32_t size) {
    UiSystem ui;
    std::string log;
    if (!ui.Load(assets, &renderer, log, "offline")) {
        std::printf("ui: %s", log.c_str());
        return 1;
    }
    std::printf("%s", log.c_str());
    Config config = base_config;
    config.show_mirror = false;
    config.show_skeleton = false;
    config.show_hitboxes = false;
    config.show_controllers = false;
    config.show_laser = false;
    config.physics.props = false;          // the training post, wall and enemy (physics_config.h)
    config.physics.show_colliders = false;
    config.physics.show_ghost = false;
    config.physics.log_contacts = false;   // no contact log in the working directory

    const Vec3 head{0.0f, 1.65f, 0.0f};
    const Quat look_down = AxisAngle({1, 0, 0}, -20.0f * kDeg);
    const Quat forward_down = AxisAngle({1, 0, 0}, -25.0f * kDeg);
    const UiLayout& layout = ui.Layout();
    const auto menu = [&](const char* name) { return layout.FindMenu(name); };

    // Hands at rest: low and forward.
    const auto rest = [&](UiInput& in) {
        in.stage = true;
        in.head = Pose(head, look_down);
        in.grip[0] = Pose({-0.24f, 1.05f, -0.30f}, forward_down);
        in.grip[1] = Pose({0.24f, 1.05f, -0.30f}, forward_down);
        in.aim[0] = in.grip[0];
        in.aim[1] = in.grip[1];
    };
    // Where a radial opens for a hand (UiSystem's rule) and the point of item k on it.
    const auto radial_point = [&](const UiMenuSpec* m, Vec3 grip, int k, int n) {
        const Vec3 away = Normalize(grip - head, {0, 0, -1});
        const Vec3 center = grip + away * m->forward + Vec3{0.0f, m->up, 0.0f};
        const UiPanel p = UiPanel::Facing(center, head, {0, 1, 0});
        const float a = 0.5f * kPi - float(k) * 2.0f * kPi / float(n);
        const float r = 0.5f * (m->inner_radius + m->radius);
        return p.At(r * std::cos(a), r * std::sin(a));
    };
    const Vec3 right_hand_up{0.17f, 1.30f, -0.36f}, left_hand_up{-0.19f, 1.28f, -0.36f};
    // The wrist strip: the left forearm across the body, palm toward the eyes.
    const Vec3 wrist_grip{-0.05f, 1.32f, -0.30f};
    const Quat wrist_rot = Basis(head - wrist_grip, {-1, 0.2f, 0});
    const auto wrist_center = [&]() {
        const UiMenuSpec* m = menu("wrist_strip");
        return wrist_grip + Rotate(wrist_rot, m ? m->offset : Vec3{0, 0, 0.08f});
    };
    // Body slots hang from the neck pivot (body yaw 0: facing -Z). Looking down at them turns the
    // head about that pivot, so the eyes move forward and down while the pivot stays.
    const Quat chest_look = AxisAngle({1, 0, 0}, -75.0f * kDeg);
    const UiMenuSpec* slots_menu = menu("body_slots");
    const Vec3 neck_pivot = head + (slots_menu ? slots_menu->neck : Vec3{0.0f, -0.10f, 0.08f});
    const Vec3 chest_eyes = neck_pivot - Rotate(chest_look, slots_menu ? slots_menu->neck
                                                                       : Vec3{0.0f, -0.10f, 0.08f});
    const auto slot_at = [&](const char* name) {
        const UiMenuSpec* m = menu("body_slots");
        if (!m) {
            return head;
        }
        const Vec3 neck = neck_pivot;
        for (const UiBodySlot& s : m->slots) {
            if (s.name == name) {
                return neck + s.standing;
            }
        }
        return head;
    };

    std::vector<Scenario> scenarios;
    scenarios.push_back(
        {"radial", "radial", 40,
         [&](UiInput& in, int f) {
             rest(in);
             in.grip[1] = in.aim[1] = Pose(right_hand_up, forward_down);
             in.squeeze[1] = 1.0f;
             if (f >= 2) {
                 in.buttons |= BBXR_BTN_CROSS; // hold right grip + B
             }
             if (f >= 12) {
                 in.stick[1][0] = 0.72f, in.stick[1][1] = 0.72f; // up-right: item 1
             }
         },
         [&]() { return right_hand_up + Vec3{0.0f, 0.04f, -0.07f}; },
         {0.75f, 1.55f, -1.05f}, {0.1f, 1.3f, -0.4f}});
    scenarios.push_back(
        {"radial_select", "radial", 52,
         [&](UiInput& in, int f) {
             rest(in);
             in.grip[1] = in.aim[1] = Pose(right_hand_up, forward_down);
             in.squeeze[1] = 1.0f;
             if (f >= 2 && f < 30) {
                 in.buttons |= BBXR_BTN_CROSS;
             }
             if (f >= 12 && f < 24) { // move the hand down-left: item 5 (Fire Paper)
                 in.grip[1] = in.aim[1] = Pose(right_hand_up + Vec3{-0.04f, -0.04f, 0.0f},
                                              forward_down);
             }
         },
         [&]() { return right_hand_up + Vec3{0.0f, -0.02f, -0.07f}; },
         {0.75f, 1.55f, -1.05f}, {0.1f, 1.3f, -0.4f}});
    scenarios.push_back(
        {"weapon_wheel", "radial", 40,
         [&](UiInput& in, int f) {
             rest(in);
             in.grip[0] = in.aim[0] = Pose(left_hand_up, forward_down);
             in.squeeze[0] = 1.0f;
             if (f >= 2) {
                 in.buttons |= BBXR_BTN_SQUARE; // hold left grip + X
             }
             const UiMenuSpec* m = menu("weapon_wheel");
             if (m && f >= 12) { // point the right hand at item 2
                 const Vec3 from{0.20f, 1.18f, -0.22f};
                 const Vec3 to = radial_point(m, left_hand_up, 2, int(m->items.size()));
                 in.aim[1] = Pose(from, LookRotation(from, to));
                 in.grip[1] = in.aim[1];
             }
         },
         [&]() { return left_hand_up + Vec3{0.0f, 0.04f, -0.07f}; },
         {-0.8f, 1.6f, -1.05f}, {-0.1f, 1.3f, -0.4f}});
    scenarios.push_back(
        {"grid", "grid", 40,
         [&](UiInput& in, int f) {
             rest(in);
             in.head = Pose(head, Quat{});
             if (f < 8) {
                 in.squeeze[1] = 1.0f;
             }
             if (f >= 2 && f < 5) {
                 in.buttons |= BBXR_BTN_CIRCLE; // right grip + A toggles it
             }
             const UiMenuSpec* m = menu("inventory_grid");
             if (m && f >= 12) { // point at the second row, second card
                 float u, v;
                 UiGridCard(*m, m->columns + 1, u, v);
                 const Vec3 target = UiGridPanel(*m, Pose(head, Quat{})).At(u, v);
                 const Vec3 from{0.22f, 1.22f, -0.18f};
                 in.aim[1] = Pose(from, LookRotation(from, target));
                 in.grip[1] = in.aim[1];
             }
         },
         [&]() { return head + Vec3{0.0f, -0.17f, -0.6f}; },
         {0.9f, 1.75f, -1.3f}, {0.0f, 1.4f, -0.5f}, 0.55f});
    scenarios.push_back(
        {"grid_far", "grid_far", 48,
         [&](UiInput& in, int f) {
             rest(in);
             in.head = Pose(head, Quat{});
             if (f < 8) {
                 in.squeeze[1] = 1.0f;
             }
             if (f >= 2 && f < 5) {
                 in.buttons |= BBXR_BTN_CIRCLE; // right grip + A
             }
             const UiMenuSpec* m = menu("inventory_far");
             if (!m || f < 10) {
                 return;
             }
             float u, v;
             if (f < 24) { // the "Throw" tab, clicked
                 UiGridTab(*m, 2, u, v);
                 in.trigger[1] = f >= 16 && f < 19 ? 1.0f : 0.0f;
             } else { // then the second card of what it shows
                 UiGridCard(*m, 1, u, v);
             }
             const Vec3 target = UiGridPanel(*m, Pose(head, Quat{})).At(u, v);
             const Vec3 from{0.22f, 1.22f, -0.18f};
             in.aim[1] = Pose(from, LookRotation(from, target));
             in.grip[1] = in.aim[1];
         },
         [&]() { return head + Vec3{0.0f, -0.2f, -1.5f}; },
         {1.6f, 1.9f, -2.6f}, {0.0f, 1.4f, -0.9f}, 0.62f});
    scenarios.push_back(
        {"wrist", "wrist_strip", 30,
         [&](UiInput& in, int f) {
             rest(in);
             in.head = Pose(head, LookRotation(head, wrist_grip));
             in.grip[0] = in.aim[0] = Pose(wrist_grip, wrist_rot);
             if (f >= 10) { // point the right hand at the third slot
                 const UiMenuSpec* m = menu("wrist_strip");
                 const Vec3 c = wrist_center();
                 const Vec3 n = Rotate(wrist_rot, {1, 0, 0});
                 const Vec3 head_up = Rotate(LookRotation(head, wrist_grip), {0, 1, 0});
                 const Vec3 up = Normalize(head_up - n * Dot(head_up, n));
                 const Vec3 right = Cross(up, n); // the panel's +X (UiSystem's wrist rule)
                 const Vec3 target = c + right * ((m ? m->width : 0.15f) * 0.32f);
                 const Vec3 from{0.16f, 1.48f, -0.10f}; // above the palm-up panel: its front
                 in.aim[1] = Pose(from, LookRotation(from, target));
                 in.grip[1] = in.aim[1];
             }
         },
         [&]() { return wrist_center(); },
         {0.55f, 1.6f, -0.95f}, {-0.05f, 1.35f, -0.3f}, 0.45f});
    scenarios.push_back(
        {"body_slots", "body_slots", 50,
         [&](UiInput& in, int f) {
             rest(in);
             in.head = Pose(chest_eyes, chest_look);
             const Vec3 chest = slot_at("chest");
             const Vec3 hip = slot_at("left_hip");
             in.grip[1] = in.aim[1] = Pose(chest + Vec3{0.02f, 0.0f, -0.03f}, forward_down);
             in.grip[0] = in.aim[0] = Pose(hip + Vec3{-0.02f, 0.03f, -0.02f}, forward_down);
             if (f >= 25) {
                 in.squeeze[1] = 1.0f; // grab a Blood Vial
             }
         },
         [&]() { return slot_at("chest") + Vec3{0.0f, -0.18f, 0.0f}; },
         {0.95f, 1.45f, -1.25f}, {0.0f, 1.05f, 0.0f}, 0.95f, false});
    scenarios.push_back(
        {"preset_toast", "grid", 20,
         [&](UiInput& in, int f) {
             rest(in);
             in.head = Pose(head, Quat{});
             if (f == 5) {
                 in.buttons |= BBXR_BTN_RIGHT; // L3 layer: right stick flick right = next
             }
         },
         [&]() { return head + Vec3{0.0f, -0.17f, -0.85f}; },
         {0.0f, 1.7f, 0.9f}, {0.0f, 1.4f, -0.8f}, 0.5f});

    FrameData data;
    double time = 100.0;
    int failures = 0;
    for (const Scenario& sc : scenarios) {
        if (!ui.SetPreset(sc.preset)) {
            std::printf("ui preview %s: no preset \"%s\" in the layout\n", sc.name, sc.preset);
            ++failures;
            continue;
        }
        std::string events_log;
        UiInput in;
        for (int f = 0; f < sc.frames; ++f) {
            in = UiInput{};
            sc.script(in, f);
            // The head looks at the scenario's subject: the first-person view is the head, as in
            // the headset (screen-aligned labels and the toast follow the head).
            if (sc.aim_head) {
                in.head.rotation = LookRotation(in.head.position, sc.look_at());
            }
            in.time = (time += 1.0 / 72.0);
            const auto tracked = [](const UiPose& p) {
                return TrackedPose{p.valid, p.position, p.rotation};
            };
            SceneInput si;
            si.time = in.time;
            si.stage = true;
            si.head = tracked(in.head);
            for (int h = 0; h < 2; ++h) {
                si.grip[h] = tracked(in.grip[h]);
                si.aim[h] = tracked(in.aim[h]);
                si.trigger[h] = in.trigger[h];
                si.squeeze[h] = in.squeeze[h];
            }
            si.buttons = ui.FilterButtons(in);
            SceneEvents events;
            scene.Update(si, config, nullptr, events, renderer);
            UiEvents ui_events;
            ui.Update(in, UiBodyOf(scene, in), config, ui_events);
            events_log += events.log + ui_events.log;
            FrameData scratch; // as every headset frame: the panels' hit areas come from Build
            ui.Build(scratch);
        }
        scene.Build(config, data);
        ui.Build(data);
        const float a = std::atan(sc.tan_half);
        const Mat4 view_proj[2] = {
            ProjectionFov(-a, a, a, -a, config.near_z, config.far_z) *
                Inverse(FromPose(in.head.rotation, in.head.position)),
            ViewProj(sc.camera, sc.target, 0.5f, config)};
        std::vector<uint8_t> pixels[2];
        if (!renderer.RenderOffscreen(size, size, view_proj, data, pixels)) {
            std::printf("ui preview %s: render failed: %s\n", sc.name, renderer.LastError().c_str());
            ++failures;
            continue;
        }
        std::vector<uint8_t> combined(size_t(size) * 2 * size * 4);
        for (uint32_t y = 0; y < size; ++y) {
            for (int eye = 0; eye < 2; ++eye) {
                std::memcpy(&combined[(size_t(y) * size * 2 + size_t(eye) * size) * 4],
                            &pixels[eye][size_t(y) * size * 4], size_t(size) * 4);
            }
        }
        const std::filesystem::path png = out_dir / (std::string("ui_") + sc.name + ".png");
        if (!WritePng(png.string(), size * 2, size, combined)) {
            std::printf("cannot write %s\n", png.string().c_str());
            ++failures;
            continue;
        }
        std::printf("%s%s (preset %s) -> %s\n", events_log.c_str(), sc.name,
                    ui.ActivePreset().c_str(), png.string().c_str());
    }
    // The game's dashboard (the bridge's game mode, ../vr_game_ui.h) with made-up live data, the
    // right hand pointing at a quick item: ui_game_dashboard.png (first person | a closer look).
    {
        // A scratch copy of the art with the built-in layout: the user's game section may be older.
        const std::filesystem::path scratch =
            std::filesystem::temp_directory_path() / "bbport_sandbox3d_dashboard";
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
        std::filesystem::create_directories(scratch / "ui", ec);
        for (const char* f : {"icons.json", "icons.png", "font.json", "font.png"}) {
            std::filesystem::copy_file(assets / "ui" / f, scratch / "ui" / f, ec);
        }
        UiSystem game;
        std::string game_log;
        if (!game.LoadGame(scratch, &renderer.GetDevice(), game_log)) {
            std::printf("ui preview game_dashboard: %s", game_log.c_str());
            ++failures;
        } else {
            UiLiveData live;
            live.lists["weapons"].items = {"Saw Cleaver", "Hunter Axe", "Hunter Pistol", ""};
            live.lists["quick"].items = {"Blood Vial", "Molotov Cocktail", "", "Pebble",
                                         "Shining Coins", ""};
            live.lists["owned_weapons"].items = {"Saw Cleaver", "Hunter Axe", "Hunter Pistol",
                                                 "Threaded Cane"};
            live.selected["quick"] = 0;
            live.selected["weapons_right"] = 0;
            live.selected["weapons_left"] = 2;
            live.disabled["use_inventory"] = "only items in a quick slot can be used from here";
            live.inventory = {{"Blood Vial", 12}, {"Quicksilver Bullets", 17},
                              {"Molotov Cocktail", 3}, {"Pebble", 9}, {"Shining Coins", 20},
                              {"Antidote", 2}, {"Fire Paper", 4}, {"Notebook", 1},
                              {"Monocular", 1}};
            game.SetLiveData(live);
            UiWorld world;
            world.weapon[1] = "Saw Cleaver";
            world.weapon[0] = "Hunter Pistol";
            UiInput in;
            const Quat look = AxisAngle({1, 0, 0}, -12.0f * kDeg);
            for (int f = 0; f < 30; ++f) {
                in = UiInput{};
                in.stage = true;
                in.head = Pose(head, look);
                in.grip[0] = Pose({-0.22f, 1.15f, -0.30f}, forward_down);
                in.grip[1] = Pose({0.22f, 1.15f, -0.30f}, forward_down);
                in.aim[0] = in.grip[0];
                in.aim[1] = in.grip[1];
                in.buttons = f == 1 ? BBXR_BTN_L3 : 0;
                Vec3 target;
                if (f > 3 && game.ElementCenter("dashboard", UiSystem::DashQuick(1), target)) {
                    in.aim[1].rotation = LookRotation(in.aim[1].position, target);
                }
                in.time = (time += 1.0 / 72.0);
                UiEvents ev;
                game.Update(in, UiBody{}, world, ev);
                game.Build();
            }
            DrawHook* hook = renderer.GetDrawHook();
            renderer.SetDrawHook(game.Gpu());
            FrameData empty;
            const Mat4 view_proj[2] = {
                ProjectionFov(-0.75f, 0.75f, 0.65f, -0.65f, config.near_z, config.far_z) *
                    Inverse(FromPose(in.head.rotation, in.head.position)),
                ProjectionFov(-0.4f, 0.4f, 0.4f, -0.4f, config.near_z, config.far_z) *
                    Inverse(FromPose(in.head.rotation, in.head.position))};
            std::vector<uint8_t> pixels[2];
            if (!renderer.RenderOffscreen(size, size, view_proj, empty, pixels)) {
                std::printf("ui preview game_dashboard: render failed\n");
                ++failures;
            } else {
                std::vector<uint8_t> combined(size_t(size) * 2 * size * 4);
                for (uint32_t y = 0; y < size; ++y) {
                    for (int eye = 0; eye < 2; ++eye) {
                        std::memcpy(&combined[(size_t(y) * size * 2 + size_t(eye) * size) * 4],
                                    &pixels[eye][size_t(y) * size * 4], size_t(size) * 4);
                    }
                }
                for (size_t i = 3; i < combined.size(); i += 4) {
                    combined[i] = 255; // the layer's coverage alpha: opaque in the picture
                }
                const std::filesystem::path png = out_dir / "ui_game_dashboard.png";
                if (WritePng(png.string(), size * 2, size, combined)) {
                    std::printf("game_dashboard -> %s\n", png.string().c_str());
                } else {
                    ++failures;
                }
            }
            renderer.SetDrawHook(hook);
            game.Destroy(nullptr);
        }
    }
    std::printf("ui preview: Blood Vial %d, Quicksilver Bullets %d left; right weapon \"%s\"\n",
                ui.Count("Blood Vial"), ui.Count("Quicksilver Bullets"), config.weapon.c_str());
    ui.Destroy(&renderer);
    return failures ? 1 : 0;
}

} // namespace sb
