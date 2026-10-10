// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <thread>
#include <mutex>
#include <string>
#include <string_view>

#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_xr.h"
#include "../../../xr_bridge/xr_hud.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../../../xr_bridge/bbport_xr_bridge.h"
#endif

namespace Vulkan {

#ifdef _WIN32

namespace {
constexpr wchar_t BridgeName[] = L"bbport_xr.dll";

std::filesystem::path ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

void BridgeLog(int level, const char* message) {
    std::printf("VR: %s%s\n", level == 2 ? "error: " : level == 1 ? "warning: " : "", message);
}

void QueueLock(void*, int lock) {
    if (lock) {
        Scheduler::submit_mutex.lock();
    } else {
        Scheduler::submit_mutex.unlock();
    }
}

std::vector<const char*> Names(uint32_t (*query)(const char**, uint32_t)) {
    std::vector<const char*> names(64);
    const uint32_t count = query(names.data(), uint32_t(names.size()));
    names.resize(std::min<size_t>(count, names.size()));
    return names;
}
} // namespace

/// How far a frame's scene camera (world to view, 3x4 rows) is from a render view's rotation
/// rows: the largest row difference, each row compared up to sign (the view may flip an axis),
/// the 3x3 read as rows or as columns.
static float RowsError(const float* view, const float (&rows)[3][3]) {
    float direct = 0.0f, transposed = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float plus = 0.0f, minus = 0.0f, tplus = 0.0f, tminus = 0.0f;
        for (int j = 0; j < 3; ++j) {
            plus = std::max(plus, std::fabs(view[i * 4 + j] - rows[i][j]));
            minus = std::max(minus, std::fabs(view[i * 4 + j] + rows[i][j]));
            tplus = std::max(tplus, std::fabs(view[j * 4 + i] - rows[i][j]));
            tminus = std::max(tminus, std::fabs(view[j * 4 + i] + rows[i][j]));
        }
        direct = std::max(direct, std::min(plus, minus));
        transposed = std::max(transposed, std::min(tplus, tminus));
    }
    return std::min(direct, transposed);
}

struct Xr::Impl {
    HMODULE module{};
    const BbXrApi* api{};
    std::vector<const char*> device_extensions;
    bool session{};
    // Mono head tracking: the camera hook's latest render views (SetRenderView, game thread),
    // paired with the presented frames in ReleaseFlat (present thread).
    std::mutex views_mutex;
    std::array<RenderView, 16> render_views{};
    u64 render_count{};
    u32 paired{}, unpaired{}, lag_sum{};
    std::chrono::steady_clock::time_point next_log{};
    // Alternate-eye stereo: the bridge said it shows no eye images (AcquireEye returned -1).
    bool eyes_refused{};
    // HUD layer: the bridge said it shows no HUD (AcquireHud returned -1).
    std::atomic<bool> hud_refused{false};
    std::atomic<bool> shut_down{false};
    // The grips of the latest Input() (Hands).
    std::mutex hands_mutex;
    std::optional<Hands> hands;
};

static std::atomic<Xr*> g_live{nullptr};

Xr* Xr::Get() {
    static Xr* const xr = []() -> Xr* {
        const char* setting = std::getenv("BB_VR");
        if (!setting || setting[0] != '1') {
            return nullptr;
        }
        if (!std::filesystem::is_regular_file(ExecutableDirectory() / BridgeName)) {
            std::printf("VR: BB_VR=1 but bbport_xr.dll is not next to the executable\n");
            return nullptr;
        }
        auto* created = new Xr;
        if (!created->impl->api) {
            delete created;
            return nullptr;
        }
        g_live = created;
        std::atexit([] {
            if (Xr* live = Get()) {
                live->Shutdown();
            }
        });
        return created;
    }();
    return xr;
}

Xr::Xr() : impl{std::make_unique<Impl>()} {
    impl->module = LoadLibraryW((ExecutableDirectory() / BridgeName).c_str());
    const auto get_api =
        impl->module ? reinterpret_cast<BbXrGetApiFn>(GetProcAddress(impl->module, "BbXrGetApi"))
                     : nullptr;
    const BbXrApi* api = get_api ? get_api() : nullptr;
    // The API struct only grows: any bridge with the functions used here (GetViews, ABI 3) works.
    constexpr uint32_t kNeededAbi = 3;
    if (!api || api->abi < kNeededAbi) {
        std::printf("VR: bbport_xr.dll is too old (ABI %u, need %u)\n", api ? api->abi : 0u,
                    kNeededAbi);
        return;
    }
    if (!api->Init(BridgeLog)) {
        std::printf("VR: no usable headset, playing flat\n");
        return;
    }
    impl->api = api;
}

Xr::~Xr() = default;

void Xr::AppendInstanceExtensions(std::vector<const char*>& enabled) {
    for (const char* name : Names(impl->api->InstanceExtensions)) {
        if (std::ranges::none_of(enabled, [&](const char* e) { return std::string_view{e} == name; })) {
            enabled.push_back(name);
        }
    }
}

std::optional<vk::PhysicalDevice> Xr::PhysicalDevice(vk::Instance instance) {
    const VkPhysicalDevice physical = impl->api->PhysicalDevice(
        instance, VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr);
    impl->device_extensions = Names(impl->api->DeviceExtensions);
    if (!physical) {
        return std::nullopt;
    }
    return vk::PhysicalDevice{physical};
}

const std::vector<const char*>& Xr::DeviceExtensions() const {
    return impl->device_extensions;
}

void Xr::StartSession(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device,
                      u32 queue_family_index) {
    const BbXrSessionInfo info{instance, physical,  device, queue_family_index,
                               0,        QueueLock, nullptr};
    impl->session = impl->api->StartSession(&info) != 0;
}

std::optional<Xr::FlatTarget> Xr::AcquireFlat(u32 width, u32 height, vk::Format source) {
    if (!impl->session) {
        return std::nullopt;
    }
    VkImage image{};
    VkFormat format{};
    if (!impl->api->AcquireFlat(width, height, VkFormat(source), &image, &format)) {
        return std::nullopt;
    }
    return FlatTarget{vk::Image{image}, vk::Format(format)};
}

void Xr::ReleaseFlat(const float* camera_view) {
    const BbXrApi* api = impl->api;
    if (api->abi < 5 || !api->ReleaseFlatView) {
        api->ReleaseFlat();
        return;
    }
    // The render view whose camera rows match the presented frame's scene camera (the frame was
    // rendered with it); the newest one when none matches (menus, loading, no camera seen).
    std::optional<RenderView> chosen;
    {
        std::scoped_lock lock{impl->views_mutex};
        const u64 size = impl->render_views.size();
        const u64 count = std::min<u64>(impl->render_count, size);
        float best = 1e9f;
        u64 best_back = 0;
        for (u64 back = 0; camera_view && back < count; ++back) {
            const float error =
                RowsError(camera_view, impl->render_views[(impl->render_count - 1 - back) % size].rows);
            if (error < best) {
                best = error;
                best_back = back;
            }
        }
        if (count && best < 2e-3f) {
            chosen = impl->render_views[(impl->render_count - 1 - best_back) % size];
            ++impl->paired;
            impl->lag_sum += u32(best_back);
        } else if (count) {
            chosen = impl->render_views[(impl->render_count - 1) % size];
            ++impl->unpaired;
        }
        const auto now = std::chrono::steady_clock::now();
        if (count && now >= impl->next_log) {
            if (impl->next_log != std::chrono::steady_clock::time_point{}) {
                std::printf("VR: mono images with their camera's view: %u paired (%.1f game frames "
                            "after the camera), %u with the newest view\n",
                            impl->paired,
                            impl->paired ? double(impl->lag_sum) / impl->paired : 0.0,
                            impl->unpaired);
                impl->paired = impl->unpaired = impl->lag_sum = 0;
            }
            impl->next_log = now + std::chrono::seconds(10);
        }
    }
    if (!chosen) {
        api->ReleaseFlatView(nullptr);
        return;
    }
    const BbXrFlatView flat{
        chosen->display_time,
        {{chosen->head.position[0], chosen->head.position[1], chosen->head.position[2]},
         {chosen->head.orientation[0], chosen->head.orientation[1], chosen->head.orientation[2],
          chosen->head.orientation[3]},
         1u},
        {chosen->fov_left, chosen->fov_right, chosen->fov_up, chosen->fov_down}};
    api->ReleaseFlatView(&flat);
}

void Xr::SetRenderView(const RenderView& view) {
    std::scoped_lock lock{impl->views_mutex};
    impl->render_views[impl->render_count % impl->render_views.size()] = view;
    ++impl->render_count;
}

bool Xr::HasEyes() const {
    return impl->session && !impl->eyes_refused && impl->api->abi >= 6 && impl->api->AcquireEye &&
           impl->api->ReleaseEye;
}

std::optional<Xr::FlatTarget> Xr::AcquireEye(int eye, u32 width, u32 height, vk::Format source) {
    if (!HasEyes() || eye < 0 || eye > 1) {
        return std::nullopt;
    }
    VkImage image{};
    VkFormat format{};
    const int acquired =
        impl->api->AcquireEye(uint32_t(eye), width, height, VkFormat(source), &image, &format);
    if (acquired < 0) {
        // The bridge shows no eye images (not in alt mode, or the runtime refused the layer):
        // every frame takes the flat path from now on.
        impl->eyes_refused = true;
        std::printf("VR: the bridge takes no eye images: mono from now on\n");
    }
    if (acquired <= 0) {
        return std::nullopt;
    }
    return FlatTarget{vk::Image{image}, vk::Format(format)};
}

void Xr::ReleaseEye(int eye, const xrstereo::EyeView& view) {
    if (!HasEyes()) {
        return;
    }
    BbXrEyeView out{};
    out.display_time = view.display_time;
    for (int i = 0; i < 3; ++i) {
        out.pose.position[i] = view.pose_position[i];
    }
    for (int i = 0; i < 4; ++i) {
        out.pose.orientation[i] = view.pose_orientation[i];
    }
    out.pose.valid = view.has_pose ? 1u : 0u;
    out.fov = {view.fov[0], view.fov[1], view.fov[2], view.fov[3]};
    out.seq = view.seq;
    impl->api->ReleaseEye(uint32_t(eye), view.has_pose ? &out : nullptr);
}

bool Xr::HasHud() const {
    return impl->session && !impl->hud_refused.load(std::memory_order_relaxed) &&
           impl->api->abi >= 7 && impl->api->AcquireHud && impl->api->ReleaseHud &&
           impl->api->HideHud;
}
std::optional<Xr::FlatTarget> Xr::AcquireHud(u32 width, u32 height, vk::Format source) {
    if (!HasHud()) {
        return std::nullopt;
    }
    VkImage image{};
    VkFormat format{};
    const int acquired = impl->api->AcquireHud(width, height, VkFormat(source), &image, &format);
    if (acquired < 0) {
        impl->hud_refused = true;
        XrHud::Disable("the bridge shows no HUD layer");
    }
    if (acquired <= 0) {
        return std::nullopt;
    }
    return FlatTarget{vk::Image{image}, vk::Format(format)};
}
void Xr::ReleaseHud() {
    if (HasHud()) {
        impl->api->ReleaseHud();
    }
}
void Xr::HideHud() {
    if (HasHud()) {
        impl->api->HideHud();
    }
}
void Xr::Shutdown() {
    // Once (atexit and the window-close path both come here); gates the game's submissions first.
    if (impl->api && !impl->shut_down.exchange(true)) {
        impl->session = false;
        std::printf("VR: shutdown: begin\n");
        impl->api->Shutdown();
        std::printf("VR: shutdown: bridge torn down\n");
    }
}

void Xr::ShutdownForExit(int timeout_ms) {
    Xr* live = g_live.load();
    if (!live) {
        return;
    }
    // On a helper thread so a stuck runtime cannot hang the exit.
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    std::thread([live, done] {
        live->Shutdown();
        done->set_value();
    }).detach();
    if (finished.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
        std::printf("VR: shutdown: timed out after %d ms, exiting anyway\n", timeout_ms);
    }
    std::fflush(stdout);
}

std::optional<Xr::Pad> Xr::Input() {
    BbXrInput in{};
    if (!impl->session || !impl->api->GetInput(&in)) {
        return std::nullopt;
    }
    Hands hands{};
    const auto put = [](float* to, const BbXrPose& p) {
        for (int i = 0; i < 3; ++i) to[i] = p.position[i];
        for (int i = 0; i < 4; ++i) to[3 + i] = p.orientation[i];
    };
    put(hands.head, in.head);
    put(hands.grip[0], in.grip[0]);
    put(hands.grip[1], in.grip[1]);
    put(hands.aim[0], in.aim[0]);
    put(hands.aim[1], in.aim[1]);
    hands.valid = (in.grip[0].valid ? 1u : 0u) | (in.grip[1].valid ? 2u : 0u) | (in.head.valid ? 4u : 0u) |
                  (in.aim[0].valid ? 8u : 0u) | (in.aim[1].valid ? 16u : 0u);
    hands.time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
    {
        std::lock_guard lock{impl->hands_mutex};
        impl->hands = hands;
    }
    return Pad{in.buttons, in.lx, in.ly, in.rx, in.ry, in.l2, in.r2};
}

std::optional<Xr::Hands> Xr::LatestHands() {
    std::lock_guard lock{impl->hands_mutex};
    return impl->hands;
}

void Xr::Rumble(u8 large, u8 small) {
    if (!impl->session) {
        return;
    }
    const float amplitude = float(std::max(large, small)) / 255.0f;
    impl->api->Haptic(2, amplitude, amplitude > 0.0f ? 0.1f : 0.0f);
}

void Xr::SetGameState(const void* state, u32 size) {
    if (!impl->session || impl->api->abi < 8 || !impl->api->SetGameState ||
        size < sizeof(uint32_t)) {
        return;
    }
    // The bridge reads `size` from the struct itself: copy into a full one (a shorter host struct
    // leaves the rest zero).
    BbXrGameState full{};
    std::memcpy(&full, state, std::min<size_t>(size, sizeof(full)));
    full.size = u32(std::min<size_t>(size, sizeof(full)));
    impl->api->SetGameState(&full);
}

void Xr::GameEvent(u32 kind, s32 a, s32 b, s32 c) {
    if (!impl->session || impl->api->abi < 8 || !impl->api->GameEvent) {
        return;
    }
    const BbXrGameEvent event{kind, a, b, c};
    impl->api->GameEvent(&event);
}

u32 Xr::TakeUiActions(void* out, u32 capacity) {
    if (!impl->session || impl->api->abi < 8 || !impl->api->TakeUiActions) {
        return 0;
    }
    return impl->api->TakeUiActions(static_cast<BbXrUiAction*>(out), capacity);
}

std::optional<Xr::Views> Xr::LatestViews() {
    BbXrViews in{};
    if (!impl->session || !impl->api->GetViews(&in)) {
        return std::nullopt;
    }
    const auto pose = [](const BbXrPose& p) {
        return Pose{{p.position[0], p.position[1], p.position[2]},
                    {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]}};
    };
    Views views{in.display_time, in.flags, pose(in.head), {}, pose(in.screen.pose),
                (in.screen.pose.valid ? 1u : 0u) | (in.screen.head_locked ? 2u : 0u)};
    for (int i = 0; i < 2; ++i) {
        const BbXrEye& eye = in.eye[i];
        views.eyes[i] = {pose(eye.pose), eye.fov.left, eye.fov.right, eye.fov.up,
                         eye.fov.down,   eye.width,    eye.height};
    }
    return views;
}

#else

struct Xr::Impl {};

Xr* Xr::Get() {
    return nullptr;
}
Xr::Xr() = default;
Xr::~Xr() = default;
void Xr::AppendInstanceExtensions(std::vector<const char*>&) {}
std::optional<vk::PhysicalDevice> Xr::PhysicalDevice(vk::Instance) {
    return std::nullopt;
}
const std::vector<const char*>& Xr::DeviceExtensions() const {
    static const std::vector<const char*> none;
    return none;
}
void Xr::StartSession(vk::Instance, vk::PhysicalDevice, vk::Device, u32) {}
std::optional<Xr::FlatTarget> Xr::AcquireFlat(u32, u32, vk::Format) {
    return std::nullopt;
}
void Xr::ReleaseFlat(const float*) {}
bool Xr::HasEyes() const {
    return false;
}
std::optional<Xr::FlatTarget> Xr::AcquireEye(int, u32, u32, vk::Format) {
    return std::nullopt;
}
void Xr::ReleaseEye(int, const xrstereo::EyeView&) {}
bool Xr::HasHud() const {
    return false;
}
std::optional<Xr::FlatTarget> Xr::AcquireHud(u32, u32, vk::Format) {
    return std::nullopt;
}
void Xr::ReleaseHud() {}
void Xr::HideHud() {}
void Xr::SetRenderView(const RenderView&) {}
void Xr::SetGameState(const void*, u32) {}
void Xr::GameEvent(u32, s32, s32, s32) {}
u32 Xr::TakeUiActions(void*, u32) {
    return 0;
}
void Xr::Shutdown() {}
void Xr::ShutdownForExit(int) {}
std::optional<Xr::Pad> Xr::Input() {
    return std::nullopt;
}
std::optional<Xr::Hands> Xr::LatestHands() {
    return std::nullopt;
}
void Xr::Rumble(u8, u8) {}
std::optional<Xr::Views> Xr::LatestViews() {
    return std::nullopt;
}

#endif

namespace XrHud {
namespace {
// -1 not decided yet, 0 off, 1 separating.
std::atomic<int> state{-1};

int Decide() {
    const xrhud::Config config = xrhud::FromEnvironment();
    if (config.mode == xrhud::Mode::Force) {
        std::printf("VR HUD: BB_XR_HUD=force: the UI is drawn into its own image (flat test; "
                    "frame dumps get a hud image, the window shows no HUD)\n");
        return 1;
    }
    if (config.mode == xrhud::Mode::Off) {
        std::printf("VR HUD: off: the HUD stays in the game image (BB_XR_HUD=%s, BB_XR_MODE=%s)\n",
                    std::getenv("BB_XR_HUD") ? std::getenv("BB_XR_HUD") : "",
                    std::getenv("BB_XR_MODE") ? std::getenv("BB_XR_MODE") : "");
        return 0;
    }
    Xr* xr = Xr::Get();
    if (!xr) {
        return 0; // no VR: nothing changes
    }
    if (!xr->HasHud()) {
        // Asked at the first draws, after the session started with the device.
        std::printf("VR HUD: the bridge has no HUD layer (ABI < 7 or no session): the HUD stays "
                    "in the game image\n");
        return 0;
    }
    std::printf("VR HUD: layer: the game's UI (HUD, subtitles, menus over the scene) is drawn "
                "into its own image and shown as a quad %.1f m away, %.0f deg wide, center %+.0f "
                "deg, %s; the eye images carry no HUD (BB_XR_HUD=off to undo)\n",
                config.distance, config.width_deg, config.pitch_deg,
                config.follow == xrhud::Follow::Lazy ? "lazy follow" : "head-locked");
    return 1;
}
} // namespace

bool Separating() {
    int s = state.load(std::memory_order_relaxed);
    if (s < 0) {
        static std::mutex mutex;
        std::scoped_lock lock{mutex};
        s = state.load(std::memory_order_relaxed);
        if (s < 0) {
            s = Decide();
            state.store(s, std::memory_order_relaxed);
        }
    }
    return s > 0;
}

void Disable(const char* why) {
    if (state.exchange(0) != 0) {
        std::printf("VR HUD: off from now on (%s): the HUD goes back into the game image\n", why);
    }
}
} // namespace XrHud

} // namespace Vulkan
