// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_sandbox3d.exe: the 3D VR sandbox for body interaction with the real Bloodborne assets
// (scripts/vr_assets/extract.py -> out/vr_assets): the hunter driven by the headset and the
// controllers (IK arms, finger curl), weapons in the hands, hit capsules, a mirror copy. The app
// renders both eyes itself and hands them to bbport_xr.dll (ABI 4 AcquireStereo/ReleaseStereo).
//
//   bbport_xr_sandbox3d.exe [--assets DIR] [--config FILE] [--seconds N]
//   bbport_xr_sandbox3d.exe --offline [--assets DIR] [--out DIR] [--size N]
//       renders scripted poses without a headset to DIR/sandbox3d_<pose>.png (left: first
//       person, right: third person)
//   Co-op pose sync (coop_net.h, docs/vr/findings/34-coop-pose-sync.md): two instances over UDP,
//       bbport_xr_sandbox3d.exe --net-port 7001 --net-peer 127.0.0.1:7002        (headset)
//       bbport_xr_sandbox3d.exe --offline-peer --net-port 7002 --net-peer 127.0.0.1:7001
//   --offline-peer is the no-XR, no-GPU partner: it plays back the scripted player
//   (coop_script.h), sends it and logs what it receives.
//
// Live tuning: <assets>/sandbox3d.json (written with the defaults on first run) is reloaded
// when saved. Controls are listed in scene.h. stdout is unbuffered.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <mmsystem.h>

#include "bbport_xr_bridge.h"
#include "collision_offline.h"
#include "combat.h"
#include "combat_view.h"
#include "config.h"
#include "dash_preview.h"
#include "image_out.h"
#include "locomotion.h"
#include "math3d.h"
#include "renderer.h"
#include "scene.h"
#include "ui.h"
#include "vignette.h"

namespace fs = std::filesystem;
using namespace m3;

namespace {

std::mutex queue_mutex;
volatile bool quit = false;

void BridgeLog(int level, const char* message) {
    std::printf("XR%s: %s\n", level == 2 ? " error" : level == 1 ? " warning" : "", message);
}

void QueueLock(void*, int lock) {
    if (lock) {
        queue_mutex.lock();
    } else {
        queue_mutex.unlock();
    }
}

BOOL WINAPI CtrlHandler(DWORD) {
    quit = true;
    return TRUE;
}

double Now() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void SetDefaultEnv(const char* name, const char* value) {
    if (!std::getenv(name)) {
        _putenv_s(name, value);
    }
}

fs::path ExeDir() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(std::wstring(path, n)).parent_path();
}

/// out/vr_assets of the repo the exe was built or copied from (walks up from the exe and the
/// working directory), or BB_VR_ASSETS.
fs::path DefaultAssets() {
    if (const char* env = std::getenv("BB_VR_ASSETS"); env && *env) {
        return env;
    }
    for (fs::path start : {ExeDir(), fs::current_path()}) {
        for (fs::path p = start; !p.empty(); p = p.parent_path()) {
            if (fs::exists(p / "out" / "vr_assets" / "assets.json")) {
                return p / "out" / "vr_assets";
            }
            if (fs::exists(p / "vr_assets" / "assets.json")) {
                return p / "vr_assets";
            }
            if (p == p.parent_path()) {
                break;
            }
        }
    }
    return "out/vr_assets";
}

bool CreateDevice(VkInstance instance, VkPhysicalDevice physical,
                  const std::vector<const char*>& extensions, VkDevice* device, uint32_t* family) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    VkPhysicalDeviceVulkan13Features have13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &have13;
    vkGetPhysicalDeviceFeatures2(physical, &have);
    std::printf("GPU: %s (Vulkan %u.%u)\n", properties.deviceName,
                VK_API_VERSION_MAJOR(properties.apiVersion),
                VK_API_VERSION_MINOR(properties.apiVersion));
    if (properties.apiVersion < VK_API_VERSION_1_3 || !have13.dynamicRendering) {
        std::printf("the GPU needs Vulkan 1.3 with dynamic rendering\n");
        return false;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    *family = 0;
    while (*family < count && !(families[*family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        ++*family;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = *family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features want13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    want13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &want13;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    const VkResult result = vkCreateDevice(physical, &info, nullptr, device);
    if (result != VK_SUCCESS) {
        std::printf("vkCreateDevice failed (%d)\n", int(result));
        return false;
    }
    (void)instance;
    return true;
}

Mat4 ViewProj(Vec3 position, Quat rotation, float left, float right, float up, float down,
              const sb::Config& config) {
    return ProjectionFov(left, right, up, down, config.near_z, config.far_z) *
           Inverse(FromPose(rotation, position));
}

Quat LookAt(Vec3 from, Vec3 to) {
    const Vec3 f = Normalize(to - from, {0, 0, -1});
    const Vec3 r = Normalize(Cross(f, {0, 1, 0}), {1, 0, 0});
    const Vec3 u = Cross(r, f);
    Mat4 m;
    m.m[0] = r.x, m.m[1] = r.y, m.m[2] = r.z;
    m.m[4] = u.x, m.m[5] = u.y, m.m[6] = u.z;
    m.m[8] = -f.x, m.m[9] = -f.y, m.m[10] = -f.z;
    return ToQuat(m);
}

sb::TrackedPose Pose(Vec3 p, Quat q) {
    return {true, p, q};
}

// ---- Co-op pose sync ----------------------------------------------------------------------------

struct NetArgs {
    bool enabled = false, offline_peer = false;
    uint16_t port = 0;
    std::string peer;
};


// ---- Offline ------------------------------------------------------------------------------------

struct Scripted {
    const char* name;
    sb::TrackedPose grip[2];
    float trigger[2], squeeze[2];
};

int RunOffline(const fs::path& assets, const fs::path& config_path, const fs::path& out_dir,
               uint32_t size) {
    { // The comfort dash's paths, raw against dash (no GPU needed): dash_*.png.
        std::string dash_log;
        const bool dash_ok = sb::WriteDashPreviews(out_dir, assets, config_path, dash_log);
        std::printf("%s", dash_log.c_str());
        if (!dash_ok) {
            return 1;
        }
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bbport_xr_sandbox3d (offline)";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        std::printf("vkCreateInstance failed\n");
        return 1;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(instance, &count, physicals.data());
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (VkPhysicalDevice p : physicals) { // prefer a discrete GPU
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(p, &props);
        if (!physical || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical = p;
        }
    }
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;
    if (!physical || !CreateDevice(instance, physical, {}, &device, &family)) {
        return 1;
    }
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    sb::Renderer renderer;
    std::string error;
    if (!renderer.Create({instance, physical, device, family, queue, nullptr, nullptr}, error)) {
        std::printf("renderer: %s\n", error.c_str());
        return 1;
    }
    sb::Scene scene;
    if (!scene.Load(assets, renderer, error)) {
        std::printf("assets: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s\n", error.c_str());
    sb::Config config;
    sb::ConfigFile file(config_path);
    std::string log;
    if (file.Poll(config, log)) {
        std::printf("%s\n", log.c_str());
    }
    fs::create_directories(out_dir);
    // These poses show the body alone; the collision scenarios (collision_offline.h) come after.
    const sb::PhysicsConfig physics_config = config.physics;
    config.physics.enabled = config.physics.props = config.physics.hits = false;

    // Head 1.65 m above the floor facing -Z (STAGE-like); grips in OpenXR's grip frame (-Z where
    // the index finger points, +Y up).
    const Vec3 head{0.0f, 1.65f, 0.0f};
    const Quat forward_down = AxisAngle({1, 0, 0}, -25.0f * kDeg);
    const Scripted poses[] = {
        {"hold", {Pose({-0.28f, 1.05f, -0.30f}, forward_down), Pose({0.26f, 1.15f, -0.38f}, forward_down)},
         {0.0f, 0.2f}, {0.0f, 1.0f}},
        {"tpose", {Pose({-0.82f, 1.42f, 0.02f}, AxisAngle({0, 1, 0}, 90.0f * kDeg)),
                   Pose({0.82f, 1.42f, 0.02f}, AxisAngle({0, 1, 0}, -90.0f * kDeg))},
         {0.0f, 0.0f}, {0.0f, 0.0f}},
        {"fist_up", {Pose({-0.05f, 1.52f, -0.16f}, AxisAngle({0, 1, 0}, -70.0f * kDeg) * AxisAngle({1, 0, 0}, 60.0f * kDeg)),
                     Pose({0.18f, 1.95f, -0.25f}, AxisAngle({1, 0, 0}, 80.0f * kDeg))},
         {0.0f, 1.0f}, {0.0f, 1.0f}},
        {"guard", {Pose({-0.20f, 1.20f, -0.42f}, AxisAngle({0, 1, 0}, 35.0f * kDeg) * forward_down),
                   Pose({0.12f, 1.30f, -0.40f}, AxisAngle({0, 1, 0}, 70.0f * kDeg) * AxisAngle({1, 0, 0}, 30.0f * kDeg))},
         {0.5f, 0.0f}, {0.5f, 1.0f}},
    };
    sb::FrameData data;
    std::vector<uint8_t> pixels[2];
    double time = 0.0;
    for (const Scripted& pose : poses) {
        sb::SceneInput input;
        input.stage = true;
        input.head = Pose(head, Quat{});
        for (int side = 0; side < 2; ++side) {
            input.grip[side] = pose.grip[side];
            input.aim[side] = pose.grip[side];
            input.trigger[side] = pose.trigger[side];
            input.squeeze[side] = pose.squeeze[side];
        }
        sb::SceneEvents events;
        for (int settle = 0; settle < 2; ++settle) {
            input.time = (time += 0.011);
            scene.Update(input, config, nullptr, events, renderer);
        }
        if (!events.log.empty()) {
            std::printf("%s", events.log.c_str());
        }
        // Left image: first person, looking down at the hands (no head). Right image: third
        // person, from in front of the hunter and to its right (with the head).
        const Quat look_down = AxisAngle({1, 0, 0}, -30.0f * kDeg);
        const Vec3 camera{1.2f, 1.55f, -2.1f};
        const Mat4 view_proj[2] = {
            ViewProj(head, look_down, -0.9f, 0.9f, 0.9f, -0.9f, config),
            ViewProj(camera, LookAt(camera, {0.0f, 1.15f, -0.1f}), -0.5f, 0.5f, 0.5f, -0.5f,
                     config)};
        sb::Config third_person = config;
        third_person.show_head = true;
        std::vector<uint8_t> outside[2];
        scene.Build(third_person, data);
        if (!renderer.RenderOffscreen(size, size, view_proj, data, outside)) {
            std::printf("render failed: %s\n", renderer.LastError().c_str());
            return 1;
        }
        scene.Build(config, data);
        if (!renderer.RenderOffscreen(size, size, view_proj, data, pixels)) {
            std::printf("render failed: %s\n", renderer.LastError().c_str());
            return 1;
        }
        pixels[1] = outside[1];
        std::vector<uint8_t> combined(size_t(size) * 2 * size * 4);
        for (uint32_t y = 0; y < size; ++y) {
            for (int eye = 0; eye < 2; ++eye) {
                std::memcpy(&combined[(size_t(y) * size * 2 + size_t(eye) * size) * 4],
                            &pixels[eye][size_t(y) * size * 4], size_t(size) * 4);
            }
        }
        const fs::path png = out_dir / (std::string("sandbox3d_") + pose.name + ".png");
        if (!sb::WritePng(png.string(), size * 2, size, combined)) {
            std::printf("cannot write %s\n", png.string().c_str());
            return 1;
        }
        const auto& arms = scene.GetRig().arm_state;
        std::printf("%s: %zu draws, %zu lines, arms reach L %.2f%s R %.2f%s -> %s\n", pose.name,
                    data.meshes.size(), (data.lines.size() + data.overlay.size()) / 2,
                    arms[0].reach, arms[0].clamped ? " (clamped)" : "", arms[1].reach,
                    arms[1].clamped ? " (clamped)" : "", png.string().c_str());
    }
    { // Scripted swings against the props: collision_*.png (the previews after keep physics off).
        sb::Config collision_config = config;
        collision_config.physics = physics_config;
        if (sb::RenderCollisionPreviews(scene, renderer, collision_config, out_dir, size) != 0) {
            return 1;
        }
    }
    // The VR menu prototypes, scripted through each preset: ui_*.png (ui.h).
    if (sb::RenderUiPreviews(assets, renderer, scene, config, out_dir, size) != 0) {
        return 1;
    }
    { // Swings and stamina through the scene's physics, with their HUD: combat_*.png (combat.h).
        sb::Config combat_config = config;
        combat_config.physics = physics_config;
        if (sb::RenderCombatPreviews(assets, config_path, renderer, scene, combat_config, out_dir,
                                     size) != 0) {
            return 1;
        }
    }
    renderer.Destroy();
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}

// ---- Headset ------------------------------------------------------------------------------------

int RunXr(const fs::path& assets, const fs::path& config_path, double seconds,
          const NetArgs& net_args) {
    // The sandbox draws its own hands and laser; STAGE puts the floor at y = 0; log every input
    // path the runtime reports.
    SetDefaultEnv("BB_XR_STAGE", "1");
    SetDefaultEnv("BB_XR_HANDS", "0");
    SetDefaultEnv("BB_XR_LASER", "0");
    SetDefaultEnv("BB_XR_INPUT_DIAG", "1");
    const HMODULE module = LoadLibraryW(L"bbport_xr.dll");
    const auto get_api =
        module ? reinterpret_cast<BbXrGetApiFn>(GetProcAddress(module, "BbXrGetApi")) : nullptr;
    const BbXrApi* xr = get_api ? get_api() : nullptr;
    if (!xr || xr->abi < 4u) {
        std::printf("bbport_xr.dll missing or older than ABI 4 (app-rendered stereo)\n");
        return 1;
    }
    if (!xr->Init(BridgeLog)) {
        std::printf("no usable headset\n");
        return 1;
    }
    const char* names[64];
    uint32_t count = xr->InstanceExtensions(names, 64);
    std::vector<const char*> instance_extensions(names, names + std::min(count, 64u));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bbport_xr_sandbox3d";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = uint32_t(instance_extensions.size());
    instance_info.ppEnabledExtensionNames = instance_extensions.data();
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        std::printf("vkCreateInstance failed\n");
        return 1;
    }
    VkPhysicalDevice physical = xr->PhysicalDevice(instance, vkGetInstanceProcAddr);
    if (!physical) {
        std::printf("the runtime named no GPU\n");
        return 1;
    }
    count = xr->DeviceExtensions(names, 64);
    std::vector<const char*> device_extensions(names, names + std::min(count, 64u));
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;
    if (!CreateDevice(instance, physical, device_extensions, &device, &family)) {
        return 1;
    }
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    const BbXrSessionInfo session{instance, physical, device, family, 0, QueueLock, nullptr};
    if (!xr->StartSession(&session)) {
        std::printf("session failed\n");
        return 1;
    }
    sb::Renderer renderer;
    std::string error;
    if (!renderer.Create({instance, physical, device, family, queue, QueueLock, nullptr}, error)) {
        std::printf("renderer: %s\n", error.c_str());
        return 1;
    }
    sb::Scene scene;
    if (!scene.Load(assets, renderer, error)) {
        std::printf("assets: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s\n", error.c_str());
    sb::Config config;
    sb::ConfigFile file(config_path);
    std::printf("controls: A held 1.5 s = T-pose calibration, B = next weapon, Y = next attack's "
                "hitboxes, X = mirror, right stick click = skeleton on top; edit %s to tune "
                "live. Ctrl+C quits.\n",
                config_path.string().c_str());
    // Locomotion and the comfort dash (locomotion.h): its own "locomotion" object in the config.
    sb::Locomotion locomotion(config_path);
    // Swing classification and stamina (combat.h, docs/vr/findings/20): the "swing" object of the
    // config. The dodge, backstep and jump pay stamina through the locomotion's move gate.
    sb::Combat combat(config_path);
    locomotion.move_gate = [&combat](int kind, double now) { return combat.TryDash(kind, now); };
    sb::Vignette vignette;
    if (!vignette.Create({instance, physical, device, family, queue, QueueLock, nullptr}, error)) {
        std::printf("vignette off: %s\n", error.c_str());
    }
    std::printf("locomotion: left stick = move where you look (A held = sprint), right stick = "
                "turn, A tapped = dodge (standing: backstep), left stick click while moving = jump; "
                "\"mode\" dash | raw | ab in the \"locomotion\" object of the config.\n");
    // The VR menu prototypes (ui.h): <assets>/ui_layouts.json, hot reloaded.
    sb::UiSystem ui;
    {
        std::string ui_log;
        ui.Load(assets, &renderer, ui_log);
        std::printf("%s%s", ui_log.c_str(), ui.Controls().c_str());
    }
    // The swing and stamina HUD (combat_view.h), drawn after the menus.
    sb::CombatView combat_view;
    {
        std::string combat_log;
        combat_view.Create(assets, renderer, combat_log);
        std::printf("%sswing: a slower swing of the right controller = LIGHT (R1), a fast one = "
                    "HEAVY (R2), the wind-up held still overhead charges R2; stamina is paid only "
                    "when the swing connects (the enemy; the post or wall too), dodges pay at once, "
                    "nothing starts at 0; the \"swing\" object of the config.\n",
                    combat_log.c_str());
    }


    sb::FrameData data;
    int frames = 0, frames_window = 0;
    double window_start = Now(), render_seconds = 0.0, last_shown = Now();
    bool ever_shown = false;
    while (!quit) {
        const double t = Now();
        if (seconds > 0.0 && t > seconds) {
            break;
        }
        std::string log;
        if (file.Poll(config, log) || !log.empty()) {
            std::printf("%s\n", log.c_str());
        }
        BbXrViews views{};
        xr->GetViews(&views);
        BbXrStereoRequest request{};
        request.width = uint32_t(float(views.eye[0].width ? views.eye[0].width : 1600) *
                                 config.render_scale);
        request.height = uint32_t(float(views.eye[0].height ? views.eye[0].height : 1600) *
                                  config.render_scale);
        request.color_format = VK_FORMAT_R8G8B8A8_UNORM;
        request.want_depth = config.want_depth ? 1u : 0u;
        request.timeout_ms = 100;
        BbXrStereoFrame frame{};
        if (!xr->AcquireStereo(&request, &frame)) {
            if (ever_shown && t - last_shown > 60.0) {
                std::printf("headset not showing for 60 s, assuming the session ended\n");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        ever_shown = true;
        last_shown = t;
        BbXrInput pad{};
        xr->GetInput(&pad);
        sb::SceneInput input;
        input.time = t;
        input.stage = frame.space == 1;
        const auto pose = [](const BbXrPose& p) {
            return sb::TrackedPose{p.valid != 0, {p.position[0], p.position[1], p.position[2]},
                                   Normalize(Quat{p.orientation[0], p.orientation[1],
                                                  p.orientation[2], p.orientation[3]})};
        };
        const sb::TrackedPose eyes[2] = {pose(frame.eye[0].pose), pose(frame.eye[1].pose)};
        input.head = {(frame.flags & BBXR_VIEW_ORIENTATION_VALID) != 0,
                      Lerp(eyes[0].position, eyes[1].position, 0.5f),
                      Slerp(eyes[0].rotation, eyes[1].rotation, 0.5f)};
        for (int side = 0; side < 2; ++side) {
            input.grip[side] = pose(frame.grip[side]);
            input.aim[side] = pose(frame.aim[side]);
            input.trigger[side] = frame.trigger[side];
            input.squeeze[side] = frame.squeeze[side];
        }
        input.buttons = pad.active ? pad.buttons : 0u;
        // The menus take their keys (and, while one is open, the sticks) before the locomotion
        // and the scene see them.
        // A left stick click while walking is the jump: the menus only see the click standing.
        const BbXrInput raw_pad = pad;
        const float walk_x = (pad.lx - 128) / 127.0f, walk_y = (pad.ly - 128) / 127.0f;
        const uint32_t jump_l3 =
            walk_x * walk_x + walk_y * walk_y > 0.3f * 0.3f ? BBXR_BTN_L3 : 0u;
        // The item wheel of objects takes R3 (hold): the card menus (quick wheel, inventory) must
        // not see it then.
        const uint32_t wheel_r3 = config.consumables.Objects() ? BBXR_BTN_R3 : 0u;
        const uint32_t raw_buttons = input.buttons & ~jump_l3;
        const uint32_t ui_buttons = raw_buttons & ~wheel_r3;
        input.buttons = pad.buttons =
            ui.FilterButtons(sb::UiInputOf(input, ui_buttons, pad.lx, pad.ly, pad.rx, pad.ry,
                                           0.0f)) |
            (input.buttons & jump_l3) | (raw_buttons & wheel_r3);
        if (ui.UsesSticks()) {
            pad.lx = pad.ly = pad.rx = pad.ry = 128;
        }
        const sb::TrackedPose head_tracking = input.head;
        const sb::SceneInput tracking = input; // the real hands and head, for the swing logic
        std::printf("%s", locomotion.Update(input, pad).c_str()); // input -> the world
        sb::SceneEvents events;
        scene.Update(input, config, &file, events, renderer);
        if (!events.log.empty()) {
            std::printf("%s", events.log.c_str());
        }
        if (!events.toast_title.empty()) {
            ui.ShowMessage(events.toast_title, events.toast_body, 3.0);
        }
        if (events.beep_hz > 0.0f) { // the parry cues
            std::thread([hz = DWORD(events.beep_hz), ms = DWORD(events.beep_ms)] { Beep(hz, ms); }).detach();
        }
        for (int side = 0; side < 2; ++side) {
            if (events.haptic[side] && events.haptic_amplitude[side] < 0.4f) {
                xr->Haptic(side, 0.4f, 0.05f); // a control's tick
            } else if (events.haptic_amplitude[side] > 0.0f) { // weapon contact, grinding, hit
                xr->Haptic(side, events.haptic_amplitude[side], events.haptic_seconds[side]);
            }
        }
        // Swings and stamina (combat.h): the real right hand and what its weapon touches; its
        // pulses (tick, hit by attack kind, tired) come after the physics' and replace them.
        sb::CombatEvents combat_events;
        combat.Frame(tracking, scene.Physics(), config.form, ui.Engaged(), combat_events);
        std::printf("%s", combat_events.log.c_str());
        if (!combat_events.set_form.empty()) {
            config.form = combat_events.set_form; // the trick weapon transformed (L1 or a snap)
        }
        for (const sb::CombatHaptic& h : combat_events.haptics) {
            xr->Haptic(h.hand, h.amplitude, h.seconds);
        }
        sb::UiEvents ui_events;
        const sb::UiInput ui_input =
            sb::UiInputOf(input, ui_buttons, raw_pad.lx, raw_pad.ly, raw_pad.rx, raw_pad.ry,
                          input.stage ? 0.0f : config.floor_y);
        ui.Update(ui_input, sb::UiBodyOf(scene, ui_input), config, ui_events);
        std::printf("%s", ui_events.log.c_str());
        for (const sb::UiHaptic& h : ui_events.haptics) {
            xr->Haptic(h.hand, h.amplitude, h.seconds);
        }
        scene.Build(config, data);
        locomotion.AppendWorld(data, input.stage ? 0.0f : config.floor_y);
        ui.Build(data);
        combat_view.Build(combat, input.head, input.grip[0]);
        sb::EyeTarget targets[2];
        for (int eye = 0; eye < 2; ++eye) {
            const BbXrEye& e = frame.eye[eye];
            const sb::TrackedPose view = locomotion.ToWorld(eyes[eye]);
            targets[eye].view_proj = ViewProj(view.position, view.rotation, e.fov.left,
                                              e.fov.right, e.fov.up, e.fov.down, config);
            targets[eye].color = frame.color[eye];
            targets[eye].depth = frame.depth[eye];
            targets[eye].width = frame.width;
            targets[eye].height = frame.height;
        }
        const double render_start = Now();
        const bool rendered =
            renderer.Render(targets, frame.color_format, frame.depth_format, data);
        if (rendered && locomotion.Vignette() > 0.005f) { // the comfort vignette, head-locked
            sb::Vignette::Eye v[2];
            for (int eye = 0; eye < 2; ++eye) {
                const BbXrFov& f = frame.eye[eye].fov;
                v[eye] = {frame.color[eye], frame.width, frame.height, {f.left, f.right, f.up, f.down},
                          Normalize(Conjugate(head_tracking.rotation) * eyes[eye].rotation)};
            }
            if (!vignette.Draw(v, frame.color_format, locomotion.Vignette(),
                               locomotion.Config().vignette_inner_deg,
                               locomotion.Config().vignette_outer_deg)) {
                std::printf("vignette: %s\n", vignette.LastError().c_str());
            }
        }
        render_seconds += Now() - render_start;
        const BbXrStereoSubmit submit{config.near_z, config.far_z};
        xr->ReleaseStereo(&submit); // always: every acquired image goes back
        if (!rendered) {
            std::printf("render failed: %s\n", renderer.LastError().c_str());
        }
        ++frames;
        ++frames_window;
        if (Now() - window_start >= 5.0) {
            const auto& arms = scene.GetRig().arm_state;
            std::printf("%.1f fps, render %.1f ms, %ux%u per eye%s, %zu draws; reach L %.2f R "
                        "%.2f, body scale %.3f\n",
                        frames_window / (Now() - window_start),
                        1000.0 * render_seconds / std::max(frames_window, 1), frame.width,
                        frame.height, frame.depth_to_compositor ? " + depth" : "",
                        data.meshes.size(), arms[0].reach, arms[1].reach,
                        scene.GetRig().BodyScale());
            window_start = Now();
            frames_window = 0;
            render_seconds = 0.0;
        }
    }
    std::printf("rendered %d stereo frames in %.0f s\n", frames, Now());
    xr->Shutdown();
    vignette.Destroy();
    combat_view.Destroy(renderer);
    ui.Destroy(&renderer);
    renderer.Destroy();
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    fs::path assets, config_path, out_dir;
    bool offline = false;
    NetArgs net_args;
    double seconds = 0.0;
    uint32_t size = 720;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--assets" && i + 1 < argc) {
            assets = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (arg == "--offline") {
            offline = true;
        }
        else if (arg == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--size" && i + 1 < argc) {
            size = uint32_t(std::max(64, std::atoi(argv[++i])));
        } else {
            std::printf("usage: bbport_xr_sandbox3d [--assets DIR] [--config FILE] [--seconds N]\n"
                        "       bbport_xr_sandbox3d --offline [--assets DIR] [--out DIR] [--size N]\n");
            return arg == "--help" ? 0 : 1;
        }
    }
    if (assets.empty()) {
        assets = DefaultAssets();
    }
    if (config_path.empty()) {
        config_path = assets / "sandbox3d.json";
    }
    if (!fs::exists(config_path) && sb::WriteDefaultConfig(config_path)) {
        std::printf("wrote the default config to %s\n", config_path.string().c_str());
    }
    if (offline) {
        return RunOffline(assets, config_path, out_dir.empty() ? assets / "preview" : out_dir,
                          size);
    }
    return RunXr(assets, config_path, seconds, net_args);
}
