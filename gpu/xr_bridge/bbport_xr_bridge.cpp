// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// OpenXR bridge: XR_KHR_vulkan_enable with the host's own VkInstance/VkDevice and a frame loop
// thread at the headset rate. Layers, back to front:
//   1. a projection layer: the host's own stereo images (AcquireStereo, ABI 4; with depth when
//      the runtime has XR_KHR_composition_layer_depth), else with BB_XR_MODE=alt each eye's
//      latest game image with the view it was rendered with (AcquireEye, ABI 6), else with
//      BB_XR_MODE=mono (or alt between eye frames: menus, loading) the game image itself, else a
//      solid background per eye (BB_XR_BG). Without one the Pimax compositor never clears, so
//      moving quads smear;
//   2. the cinema screen showing the game window: a cylinder when the runtime has
//      XR_KHR_composition_layer_cylinder (BB_XR_CURVE=0 for the flat quad), else a quad;
//   2b. the HUD quad (ABI 7) and the game's VR UI (ABI 8, BB_VR_UI=1: vr_game_ui.h), a
//      transparent projection layer the bridge renders itself on a thread of its own;
//   3. the laser pointer of the right controller (BB_XR_LASER=1): a beam and the hit dot;
//   4. the hand markers.
// The screen is placed in front of the user once the head is tracked, and again when the session
// is focused again, the runtime recenters or the left menu button is held.
// Locks: state_mutex (flat swapchain, the frame loop's layers) before the host queue lock;
// g_views_mutex is a leaf.

#include "bbport_xr_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "vr_game_ui.h"
#include "vr_mirror_ui.h"
#include "vr_ui_layer.h"
#include "xr_hud.h"
#include "xr_input.h"
#include "xr_screen_math.h"
#include "xr_stereo.h"

namespace {

BbXrLogFn g_log = nullptr;

void Log(int level, const char* format, ...) __attribute__((format(printf, 2, 3)));
void Log(int level, const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (g_log) {
        g_log(level, buffer);
    } else {
        std::fprintf(stderr, "XR: %s\n", buffer);
    }
}

/// A positive number from the environment, else the fallback.
float EnvFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    const float parsed = value && *value ? float(std::atof(value)) : 0.0f;
    return parsed > 0.0f ? parsed : fallback;
}

/// "1..." on, "0..." off, anything else (or unset) the fallback.
bool EnvFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (value && value[0] == '1') {
        return true;
    }
    if (value && value[0] == '0') {
        return false;
    }
    return fallback;
}

/// Failures of calls made every frame: the first few are logged, then one in 1000, so a stuck
/// runtime cannot flood the game log at 90 Hz.
struct FailureLog {
    uint32_t count = 0;
};

/// A swapchain and where its acquire/wait/release cycle stands.
struct Chain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkanKHR> images;
    uint32_t width = 0, height = 0;
    int64_t format = 0;
    uint32_t index = 0;
    bool waiting = false;  // acquired, xrWaitSwapchainImage has not succeeded yet
    bool writing = false;  // images[index] may be written until it is released
    bool released = false; // an image was released: layers may show the chain
    FailureLog failures;
};

struct Bridge {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE; // the screen's: LOCAL, STAGE, or VIEW when head-locked
    XrSpace local = XR_NULL_HANDLE; // hand markers (XrInput poses are LOCAL)
    XrSpace view = XR_NULL_HANDLE;  // the head
    XrSpace world = XR_NULL_HANDLE; // views and projection layers: `space`, LOCAL if head-locked
    bool head_locked = false;
    bool space_is_local = false; // `space` is LOCAL, the space of XrInput's poses
    std::atomic<bool> recentered{false}; // the runtime moved a reference space
    std::atomic<bool> refocused{false};  // the session became FOCUSED (again): headset put on
    XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;

    PFN_xrGetVulkanInstanceExtensionsKHR get_instance_extensions = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR get_device_extensions = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR get_graphics_device = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR get_graphics_requirements = nullptr;

    std::string instance_extension_storage, device_extension_storage;
    std::vector<const char*> instance_extensions, device_extensions;

    BbXrSessionInfo vk{};
    PFN_vkGetInstanceProcAddr gipa = nullptr;

    // Eye views of PRIMARY_STEREO (from Init): recommended sizes; poses are located per frame.
    bool stereo = false;
    XrViewConfigurationView eye_config[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                             {XR_TYPE_VIEW_CONFIGURATION_VIEW}};

    // Set up by Init/StartSession; afterwards only the frame loop touches the layer switches (it
    // turns one off when the runtime keeps rejecting frames with it).
    float screen_width = 4.16f, screen_distance = 3.38f;
    bool cylinder = false; // XR_KHR_composition_layer_cylinder enabled and wanted
    const char* flat_reason = "";
    bool mono = false;       // BB_XR_MODE=mono
    bool alt = false;        // BB_XR_MODE=alt: alternate-eye stereo (ABI 6)
    bool background = false; // the background chains are filled
    bool show_hands = true;
    bool laser = false;      // BB_XR_LASER=1: the right controller's beam and hit dot

    // Flat screen swapchain (written by the host's present thread) and the views current when
    // its latest image was finished, guarded by state_mutex (taken before the host queue lock).
    std::mutex state_mutex;
    Chain flat;
    XrView flat_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool flat_views_valid = false;
    // ABI 5: the view the latest flat image was rendered with (ReleaseFlatView), for mono.
    BbXrFlatView flat_view{};
    bool flat_view_valid = false;

    // App-rendered stereo (ABI 4), guarded by state_mutex like the flat chain.
    bool depth_layers = false;     // XR_KHR_composition_layer_depth enabled at Init
    Chain stereo_color[2], stereo_depth[2];
    bool stereo_writing = false;   // the host holds acquired images
    bool stereo_with_depth = false; // ... depth images among them
    XrView stereo_acquired[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; // the views handed out
    XrView stereo_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};    // of the latest release
    bool stereo_shown = false;     // released images to show
    bool stereo_depth_shown = false;
    float stereo_near = 0.05f, stereo_far = 100.0f;
    std::chrono::steady_clock::time_point stereo_released_at{};
    uint64_t stereo_frame = 0;     // the headset frame the latest AcquireStereo rendered for
    bool stereo_logged = false;

    // Alternate-eye stereo (ABI 6), guarded by state_mutex like the flat chain: each eye's chain
    // and the view its latest image was rendered with; the compositor keeps showing a chain's
    // last released image, so the eye not rendered this frame stays with its own view.
    Chain alt_chain[2];
    XrPosef alt_pose[2] = {};
    XrFovf alt_fov[2] = {};
    xrstereo::AltEyes alt_eyes;
    xrstereo::AgeStats alt_ages;
    uint64_t alt_seq[2] = {};
    uint64_t alt_display_frames = 0, alt_shown_frames = 0;
    bool alt_logged = false, alt_view_warned = false;
    std::chrono::steady_clock::time_point alt_next_log{};

    // The HUD layer (ABI 7, xr_hud.h), guarded by state_mutex like the flat chain: the game's UI
    // drawn into its own image, shown as a quad in front of the user while it is fresh.
    xrhud::Config hud_config{};
    bool hud = false;       // the layer is on (BB_XR_HUD) and the runtime has not refused it
    Chain hud_chain;
    bool hud_shown = false; // a released image to show (HideHud clears it)
    std::chrono::steady_clock::time_point hud_released_at{};
    bool hud_logged = false;
    xrhud::Alpha hud_alpha = xrhud::Alpha::Premul; // BB_XR_HUD_ALPHA, or the live file

    // The game's VR UI (ABI 8, BB_VR_UI=1, vr_game_ui.h): its logic, its GPU side, its thread,
    // and the transparent stereo layer it renders (chains and views guarded by state_mutex).
    vrui::GameUi* game_ui = nullptr;
    vrui::UiLayerGpu ui_gpu;
    std::thread ui_thread;
    float ui_scale = 0.5f;          // BB_VR_UI_SCALE: of the recommended eye size
    Chain ui_chain[2];
    XrView ui_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; // what the shown images were drawn for
    bool ui_shown = false;
    bool ui_layer = true;           // the runtime has not refused the layer
    bool ui_logged = false;
    std::chrono::steady_clock::time_point ui_released_at{};
    // The VR UI in the desktop window (ABI 9, BB_XR_MIRROR_UI, vr_mirror_ui.h): the UI thread
    // draws it once more from the view the flat image was rendered with into a ring of 3 images.
    // pending: submitted, not known finished; ready: finished (guarded by state_mutex).
    bool mirror_on = false;         // BB_XR_MIRROR_UI / bb_xr_mirror_ui.txt (UI thread only)
    bool mirror_logged = false;
    int mirror_next = 0;
    int mirror_pending = -1, mirror_ready = -1;
    BbXrMirrorUi mirror_pending_info{}, mirror_ready_info{};
    std::chrono::steady_clock::time_point mirror_ready_at{}, mirror_file_polled{};
    uint64_t mirror_seq = 0;
    std::chrono::steady_clock::time_point flat_view_at{}; // when flat_view was set

    // Filled once at StartSession: the compositor keeps showing a chain's last released image.
    Chain background_eyes[2];
    Chain markers[3]; // hand markers: idle, button held, gesture fired
    Chain lasers[3];  // laser beam missing the screen, beam on it, hit dot

    std::atomic<bool> running{false};
    std::atomic<bool> visible{false};
    std::atomic<bool> focused{false};
    bool session_running = false;
    std::thread loop;

    void QueueLock(bool lock) const {
        if (vk.queue_lock) {
            vk.queue_lock(vk.queue_lock_user, lock ? 1 : 0);
        }
    }
};

Bridge* g = nullptr;

/// GetViews' snapshot, written by the frame loop once per frame. Outside Bridge so readers on
/// game threads never race Shutdown's delete.
struct Published {
    BbXrViews api{};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; // the latest valid views, for mono
    bool views_valid = false;
    // For AcquireStereo: the views one display period ahead (when its images will be shown).
    XrView next_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    int64_t next_time = 0;
    uint32_t next_flags = 0;
    bool next_valid = false;
    BbXrPose next_grip[2] = {}, next_aim[2] = {}; // the controllers then, in the world space
    float trigger[2] = {}, squeeze[2] = {};             // the latest analog values
    uint64_t frame = 0; // counts the frame loop's frames (AcquireStereo paces on it)
};
std::mutex g_views_mutex;
std::condition_variable g_frame_cv; // with g_views_mutex: a new frame was published
Published g_published;

bool Check(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    char text[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (g && g->instance) {
        xrResultToString(g->instance, result, text);
    }
    Log(2, "%s failed: %s (%d)", what, text, int(result));
    return false;
}

/// Check() for calls made every frame.
bool CheckEvery(XrResult result, const char* what, FailureLog& failures) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    ++failures.count;
    if (failures.count <= 5 || failures.count % 1000 == 0) {
        char text[XR_MAX_RESULT_STRING_SIZE] = "?";
        if (g && g->instance) {
            xrResultToString(g->instance, result, text);
        }
        Log(2, "%s failed: %s (%d), %u times", what, text, int(result), failures.count);
    }
    return false;
}

void SplitExtensions(std::string& storage, std::vector<const char*>& out) {
    out.clear();
    for (char& c : storage) {
        if (c == ' ') {
            c = '\0';
        }
    }
    const char* begin = storage.data();
    const char* end = begin + storage.size();
    for (const char* p = begin; p < end; p += std::strlen(p) + 1) {
        if (*p) {
            out.push_back(p);
        }
    }
}

template <typename Fn>
bool LoadFunction(const char* name, Fn& fn) {
    return Check(xrGetInstanceProcAddr(g->instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn)),
                 name);
}

xrscreen::Pose FromXr(const XrPosef& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
            {p.position.x, p.position.y, p.position.z}};
}

XrPosef ToXr(const xrscreen::Pose& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
            {p.position.x, p.position.y, p.position.z}};
}

BbXrPose ToApi(const XrPosef& p, bool valid) {
    return {{p.position.x, p.position.y, p.position.z},
            {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
            valid ? 1u : 0u};
}

/// Runtimes return XR_ERROR_POSE_INVALID for layers with a non-unit quaternion.
bool IsUnit(const XrQuaternionf& q) {
    return std::fabs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1.0f) < 0.01f;
}

XrRect2Di FullRect(const Chain& chain) {
    return {{0, 0}, {int32_t(chain.width), int32_t(chain.height)}};
}

/// Seconds on the steady clock (alternate-eye bookkeeping).
double Steady(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

/// Stereo eye views: the image sizes the runtime recommends (published from now on).
void LoadEyes() {
    uint32_t count = 0;
    const XrResult counted = xrEnumerateViewConfigurationViews(
        g->instance, g->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
    if (XR_FAILED(counted) || count != 2) {
        Log(1, "no two-view stereo configuration (%d, %u views): no background layer, no head "
               "views",
            int(counted), count);
        return;
    }
    if (!Check(xrEnumerateViewConfigurationViews(g->instance, g->system,
                                                 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                 &count, g->eye_config),
               "xrEnumerateViewConfigurationViews")) {
        return;
    }
    g->stereo = true;
    {
        std::scoped_lock lock{g_views_mutex};
        for (int eye = 0; eye < 2; ++eye) {
            g_published.api.eye[eye].width = g->eye_config[eye].recommendedImageRectWidth;
            g_published.api.eye[eye].height = g->eye_config[eye].recommendedImageRectHeight;
        }
    }
    Log(0, "eye images: recommended %ux%u and %ux%u, max %ux%u",
        unsigned(g->eye_config[0].recommendedImageRectWidth),
        unsigned(g->eye_config[0].recommendedImageRectHeight),
        unsigned(g->eye_config[1].recommendedImageRectWidth),
        unsigned(g->eye_config[1].recommendedImageRectHeight),
        unsigned(g->eye_config[0].maxImageRectWidth),
        unsigned(g->eye_config[0].maxImageRectHeight));
}

int Init(BbXrLogFn log) {
    g_log = log;
    if (g) {
        return g->system != XR_NULL_SYSTEM_ID;
    }
    g = new Bridge;
    g->head_locked = EnvFlag("BB_XR_HEADLOCK", false);

    uint32_t count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data());
    bool has_vulkan = false, has_cylinder = false, has_depth = false;
    for (const auto& extension : available) {
        has_vulkan |= std::strcmp(extension.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
        has_cylinder |= std::strcmp(extension.extensionName,
                                    XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) == 0;
        has_depth |= std::strcmp(extension.extensionName,
                                 XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) == 0;
    }
    if (!has_vulkan) {
        Log(2, "the active OpenXR runtime has no %s", XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
        return 0;
    }
    // Curved screen by default when the runtime can draw cylinders; BB_XR_CURVE=0 keeps the
    // quad (and leaves the extension off).
    const bool want_curve = EnvFlag("BB_XR_CURVE", true);
    g->cylinder = want_curve && has_cylinder;
    g->flat_reason = want_curve ? "the runtime has no XR_KHR_composition_layer_cylinder"
                                : "BB_XR_CURVE=0";

    // Depth for app-rendered stereo (positional reprojection); BB_XR_DEPTH=0 leaves it off.
    g->depth_layers = has_depth && EnvFlag("BB_XR_DEPTH", true);

    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(create.applicationInfo.applicationName, "Bloodborne (bbport)",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    create.applicationInfo.applicationVersion = 1;
    std::strncpy(create.applicationInfo.engineName, "bbport", XR_MAX_ENGINE_NAME_SIZE - 1);
    create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    const auto create_instance = [&]() {
        std::vector<const char*> extensions{XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
        if (g->cylinder) {
            extensions.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
        }
        if (g->depth_layers) {
            extensions.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
        }
        create.enabledExtensionCount = uint32_t(extensions.size());
        create.enabledExtensionNames = extensions.data();
        g->instance = XR_NULL_HANDLE;
        return xrCreateInstance(&create, &g->instance);
    };
    XrResult created = create_instance();
    if (XR_FAILED(created) && g->depth_layers) {
        // Listed but refused: stereo images go without depth.
        Log(1, "xrCreateInstance with the depth layer extension failed (%d), retrying without",
            int(created));
        g->depth_layers = false;
        created = create_instance();
    }
    if (XR_FAILED(created) && g->cylinder) {
        // Listed but refused: the screen can stay flat.
        Log(1, "xrCreateInstance with the cylinder extension failed (%d), retrying without",
            int(created));
        g->cylinder = false;
        g->flat_reason = "the runtime refused XR_KHR_composition_layer_cylinder";
        created = create_instance();
    }
    if (!Check(created, "xrCreateInstance")) {
        g->instance = XR_NULL_HANDLE;
        return 0;
    }
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g->instance, &properties);
    Log(0, "runtime %s %u.%u.%u", properties.runtimeName,
        unsigned(XR_VERSION_MAJOR(properties.runtimeVersion)),
        unsigned(XR_VERSION_MINOR(properties.runtimeVersion)),
        unsigned(XR_VERSION_PATCH(properties.runtimeVersion)));

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(g->instance, &system_info, &g->system), "xrGetSystem (is the headset on?)")) {
        g->system = XR_NULL_SYSTEM_ID;
        return 0;
    }
    XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g->instance, g->system, &system);
    Log(0, "headset %s, max swapchain %ux%u", system.systemName,
        unsigned(system.graphicsProperties.maxSwapchainImageWidth),
        unsigned(system.graphicsProperties.maxSwapchainImageHeight));
    LoadEyes();

    if (!LoadFunction("xrGetVulkanInstanceExtensionsKHR", g->get_instance_extensions) ||
        !LoadFunction("xrGetVulkanDeviceExtensionsKHR", g->get_device_extensions) ||
        !LoadFunction("xrGetVulkanGraphicsDeviceKHR", g->get_graphics_device) ||
        !LoadFunction("xrGetVulkanGraphicsRequirementsKHR", g->get_graphics_requirements)) {
        g->system = XR_NULL_SYSTEM_ID;
        return 0;
    }

    uint32_t length = 0;
    g->get_instance_extensions(g->instance, g->system, 0, &length, nullptr);
    g->instance_extension_storage.assign(length, '\0');
    g->get_instance_extensions(g->instance, g->system, length, &length,
                               g->instance_extension_storage.data());
    g->instance_extension_storage.resize(std::strlen(g->instance_extension_storage.c_str()));
    SplitExtensions(g->instance_extension_storage, g->instance_extensions);
    return 1;
}

uint32_t CopyNames(const std::vector<const char*>& names, const char** out, uint32_t capacity) {
    for (uint32_t i = 0; i < names.size() && i < capacity; ++i) {
        out[i] = names[i];
    }
    return uint32_t(names.size());
}

uint32_t InstanceExtensions(const char** names, uint32_t capacity) {
    return g ? CopyNames(g->instance_extensions, names, capacity) : 0;
}

VkPhysicalDevice PhysicalDevice(VkInstance instance, PFN_vkGetInstanceProcAddr gipa) {
    if (!g || g->system == XR_NULL_SYSTEM_ID) {
        return VK_NULL_HANDLE;
    }
    g->gipa = gipa;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    if (!Check(g->get_graphics_device(g->instance, g->system, instance, &physical),
               "xrGetVulkanGraphicsDeviceKHR")) {
        return VK_NULL_HANDLE;
    }
    uint32_t length = 0;
    g->get_device_extensions(g->instance, g->system, 0, &length, nullptr);
    g->device_extension_storage.assign(length, '\0');
    g->get_device_extensions(g->instance, g->system, length, &length,
                             g->device_extension_storage.data());
    g->device_extension_storage.resize(std::strlen(g->device_extension_storage.c_str()));
    SplitExtensions(g->device_extension_storage, g->device_extensions);
    return physical;
}

uint32_t DeviceExtensions(const char** names, uint32_t capacity) {
    return g ? CopyNames(g->device_extensions, names, capacity) : 0;
}

// ---- Swapchains ------------------------------------------------------------------------------

int64_t PickFormat(VkFormat source) {
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(g->session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(g->session, count, &count, formats.data());
    // The game image holds sRGB-encoded values in a UNORM format: an sRGB swapchain of the same
    // channel order takes a raw copy and the compositor decodes it once.
    VkFormat wanted[2] = {source, source};
    switch (source) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        wanted[0] = VK_FORMAT_B8G8R8A8_SRGB;
        wanted[1] = VK_FORMAT_R8G8B8A8_SRGB;
        break;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        wanted[0] = VK_FORMAT_R8G8B8A8_SRGB;
        wanted[1] = VK_FORMAT_B8G8R8A8_SRGB;
        break;
    default:
        break;
    }
    for (VkFormat want : wanted) {
        for (int64_t format : formats) {
            if (format == want) {
                return format;
            }
        }
    }
    return formats.empty() ? 0 : formats[0];
}

bool CreateChain(Chain& chain, uint32_t width, uint32_t height, int64_t format, const char* what,
                 XrSwapchainUsageFlags usage = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                                               XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) {
    chain = {};
    if (!format) {
        return false;
    }
    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create.usageFlags = usage;
    create.format = format;
    create.sampleCount = 1;
    create.width = width;
    create.height = height;
    create.faceCount = create.arraySize = create.mipCount = 1;
    if (!Check(xrCreateSwapchain(g->session, &create, &chain.handle), what)) {
        chain.handle = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(chain.handle, 0, &count, nullptr);
    chain.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(chain.handle, count, &count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(chain.images.data()));
    chain.width = width;
    chain.height = height;
    chain.format = format;
    return !chain.images.empty();
}

void DestroyChain(Chain& chain) {
    if (chain.handle) {
        xrDestroySwapchain(chain.handle);
    }
    chain = {};
}

/// Acquires the next image (unless an earlier acquire is still waiting) and waits for it. True
/// when images[index] may be written. On XR_TIMEOUT_EXPIRED the image stays acquired and the
/// next call waits for it again: the spec forbids releasing an image that was not waited for.
bool BeginWrite(Chain& chain, XrDuration timeout) {
    if (!chain.handle || chain.writing) {
        return false;
    }
    if (!chain.waiting) {
        g->QueueLock(true); // the runtime may submit on the shared queue here
        const XrResult acquired = xrAcquireSwapchainImage(chain.handle, nullptr, &chain.index);
        g->QueueLock(false);
        if (!CheckEvery(acquired, "xrAcquireSwapchainImage", chain.failures)) {
            return false;
        }
        chain.waiting = true;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = timeout;
    const XrResult waited = xrWaitSwapchainImage(chain.handle, &wait);
    if (waited == XR_TIMEOUT_EXPIRED) {
        if (++chain.failures.count <= 5 || chain.failures.count % 1000 == 0) {
            Log(1, "swapchain image still in use by the compositor, waiting again next time");
        }
        return false;
    }
    if (!CheckEvery(waited, "xrWaitSwapchainImage", chain.failures)) {
        return false;
    }
    chain.waiting = false;
    chain.writing = true;
    return true;
}

void EndWrite(Chain& chain) {
    if (!chain.writing) {
        return;
    }
    g->QueueLock(true);
    const XrResult released = xrReleaseSwapchainImage(chain.handle, nullptr);
    g->QueueLock(false);
    chain.writing = false;
    if (CheckEvery(released, "xrReleaseSwapchainImage", chain.failures)) {
        chain.released = true;
    }
}

// The device functions the one-time fills use.
#define BB_FILL_FUNCTIONS(X)                                                                       \
    X(vkGetDeviceQueue)                                                                            \
    X(vkCreateCommandPool)                                                                         \
    X(vkDestroyCommandPool)                                                                        \
    X(vkAllocateCommandBuffers)                                                                    \
    X(vkBeginCommandBuffer)                                                                        \
    X(vkEndCommandBuffer)                                                                          \
    X(vkCmdPipelineBarrier)                                                                        \
    X(vkCmdClearColorImage)                                                                        \
    X(vkQueueSubmit)                                                                               \
    X(vkCreateFence)                                                                               \
    X(vkDestroyFence)                                                                              \
    X(vkWaitForFences)                                                                             \
    X(vkResetFences)

/// Fills swapchain images with one color on the host's queue, at session start (hand markers,
/// background). Needs PhysicalDevice() to have handed over vkGetInstanceProcAddr.
class Filler {
public:
    Filler() {
        const auto gdpa = g->gipa ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                                        g->gipa(g->vk.instance, "vkGetDeviceProcAddr"))
                                  : nullptr;
        if (!gdpa) {
            return;
        }
        device = g->vk.device;
        bool loaded = true;
#define BB_LOAD(name)                                                                              \
    name = reinterpret_cast<PFN_##name>(gdpa(device, #name));                                      \
    loaded = loaded && name;
        BB_FILL_FUNCTIONS(BB_LOAD)
#undef BB_LOAD
        if (!loaded) {
            return;
        }
        vkGetDeviceQueue(device, g->vk.queue_family_index, g->vk.queue_index, &queue);
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = g->vk.queue_family_index;
        if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
            pool = VK_NULL_HANDLE;
            return;
        }
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(device, &alloc, &cmd) != VK_SUCCESS ||
            vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
            cmd = VK_NULL_HANDLE;
        }
    }

    ~Filler() {
        if (fence) {
            vkDestroyFence(device, fence, nullptr);
        }
        if (pool) {
            vkDestroyCommandPool(device, pool, nullptr);
        }
    }

    Filler(const Filler&) = delete;
    Filler& operator=(const Filler&) = delete;

    bool Ready() const {
        return cmd && fence;
    }

    /// One image of the chain, acquired, cleared and released; the others are never shown.
    bool Fill(Chain& chain, const VkClearColorValue& color) {
        if (!Ready() || !BeginWrite(chain, 1'000'000'000)) { // 1 s
            return false;
        }
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = chain.images[chain.index].image;
        barrier.subresourceRange = range;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        // Acquired images are in COLOR_ATTACHMENT_OPTIMAL and go back in it; the clear covers
        // everything, so the old contents are discarded.
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkCmdClearColorImage(cmd, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                             &range);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        g->QueueLock(true);
        const VkResult submitted = vkQueueSubmit(queue, 1, &submit, fence);
        g->QueueLock(false);
        if (submitted == VK_SUCCESS) {
            vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(device, 1, &fence);
        }
        EndWrite(chain);
        return submitted == VK_SUCCESS && chain.released;
    }

private:
#define BB_MEMBER(name) PFN_##name name = nullptr;
    BB_FILL_FUNCTIONS(BB_MEMBER)
#undef BB_MEMBER
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};
#undef BB_FILL_FUNCTIONS

/// Solid-color 32x32 chains for small quads (colors are linear: the swapchain is sRGB). All or
/// nothing.
bool CreateColorChains(Filler& filler, Chain (&chains)[3], const VkClearColorValue (&colors)[3],
                       const char* what) {
    const int64_t format = PickFormat(VK_FORMAT_R8G8B8A8_UNORM);
    bool ok = filler.Ready();
    for (int i = 0; i < 3 && ok; ++i) {
        ok = CreateChain(chains[i], 32, 32, format, what) && filler.Fill(chains[i], colors[i]);
    }
    if (!ok) {
        for (Chain& chain : chains) {
            DestroyChain(chain);
        }
    }
    return ok;
}

/// Hand markers: small billboards at the controllers, grey idle, orange held, green gesture.
bool CreateMarkers(Filler& filler) {
    const VkClearColorValue colors[3] = {{{0.35f, 0.35f, 0.38f, 1.0f}},
                                         {{1.0f, 0.35f, 0.02f, 1.0f}},
                                         {{0.05f, 1.0f, 0.15f, 1.0f}}};
    return CreateColorChains(filler, g->markers, colors, "marker swapchain");
}

/// Laser pointer: a red beam while it misses the screen, cyan on it, and a white hit dot.
bool CreateLaser(Filler& filler) {
    const VkClearColorValue colors[3] = {{{0.9f, 0.04f, 0.03f, 1.0f}},
                                         {{0.05f, 0.75f, 1.0f, 1.0f}},
                                         {{1.0f, 1.0f, 0.9f, 1.0f}}};
    return CreateColorChains(filler, g->lasers, colors, "laser swapchain");
}

float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// The background projection layer, behind everything: without a projection layer the Pimax
/// compositor never clears its output and moving quads leave trails. One small chain per eye
/// holds a solid color (BB_XR_BG="r,g,b", display values 0..1, one value for grey, "off" for
/// none); the views are located every frame. The color is constant, so the images are filled
/// once: acquiring them every frame would only add waits on the queue lock the game's present
/// also holds.
bool CreateBackground(Filler& filler) {
    float rgb[3] = {0.02f, 0.02f, 0.02f};
    if (const char* value = std::getenv("BB_XR_BG"); value && *value) {
        if (std::strcmp(value, "off") == 0) {
            Log(0, "background layer off (BB_XR_BG=off): moving quads may smear on Pimax");
            return false;
        }
        float parsed[3] = {};
        const int fields = std::sscanf(value, "%f,%f,%f", &parsed[0], &parsed[1], &parsed[2]);
        if (fields == 1 || fields == 3) {
            for (int i = 0; i < 3; ++i) {
                rgb[i] = std::clamp(fields == 1 ? parsed[0] : parsed[i], 0.0f, 1.0f);
            }
        } else {
            Log(1, "BB_XR_BG=\"%s\" is not \"r,g,b\", using the default", value);
        }
    }
    if (!g->stereo || !filler.Ready()) {
        Log(1, "no background layer (%s): moving quads may smear on Pimax",
            g->stereo ? "no Vulkan access" : "no stereo views");
        return false;
    }
    // The compositor stretches each image over its eye's FOV: one color needs few pixels.
    const VkClearColorValue color{
        {SrgbToLinear(rgb[0]), SrgbToLinear(rgb[1]), SrgbToLinear(rgb[2]), 1.0f}};
    const int64_t format = PickFormat(VK_FORMAT_R8G8B8A8_UNORM);
    bool ok = true;
    for (int eye = 0; eye < 2 && ok; ++eye) {
        const uint32_t width = std::max(64u, g->eye_config[eye].recommendedImageRectWidth / 8);
        const uint32_t height = std::max(64u, g->eye_config[eye].recommendedImageRectHeight / 8);
        ok = CreateChain(g->background_eyes[eye], width, height, format, "background swapchain") &&
             filler.Fill(g->background_eyes[eye], color);
    }
    if (!ok) {
        for (Chain& eye : g->background_eyes) {
            DestroyChain(eye);
        }
        Log(1, "background layer unavailable: moving quads may smear on Pimax");
        return false;
    }
    Log(0, "background projection layer %ux%u per eye, color %.3g,%.3g,%.3g (BB_XR_BG)",
        g->background_eyes[0].width, g->background_eyes[0].height, rgb[0], rgb[1], rgb[2]);
    return true;
}

// ---- Frame loop ------------------------------------------------------------------------------

void HandleEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g->instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            Log(0, "session state %d", int(changed.state));
            g->focused = changed.state == XR_SESSION_STATE_FOCUSED;
            // Back to FOCUSED (the headset put on, a system menu closed): the user may face
            // anywhere now, so the screen is placed again.
            if (changed.state == XR_SESSION_STATE_FOCUSED &&
                g->session_state != XR_SESSION_STATE_FOCUSED) {
                g->refocused = true;
            }
            g->session_state = changed.state;
            switch (changed.state) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g->session_running = Check(xrBeginSession(g->session, &begin), "xrBeginSession");
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                g->session_running = false;
                g->visible = false;
                Check(xrEndSession(g->session), "xrEndSession");
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                Log(1, "session %s: the headset stops showing the game until it restarts",
                    changed.state == XR_SESSION_STATE_EXITING ? "exiting" : "lost");
                g->session_running = false;
                g->visible = false;
                g->running = false;
                break;
            default:
                break;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const auto& change =
                reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);
            static int events = 0;
            ++events;
            if (events <= 5 || events % 20 == 0) {
                Log(0, "reference space change pending #%d (space type %d, pose valid %d, "
                       "time %lld)",
                    events, int(change.referenceSpaceType), int(change.poseValid),
                    static_cast<long long>(change.changeTime));
            }
            g->recentered = true;
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            Log(1, "OpenXR instance lost: the headset stops showing the game");
            g->visible = false;
            g->running = false;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

/// Once, when the eyes are first located: FOVs, IPD and how far apart the displays point.
void LogViews(const XrView (&views)[2]) {
    constexpr float kDeg = 57.29578f;
    const XrVector3f& a = views[0].pose.position;
    const XrVector3f& b = views[1].pose.position;
    const float ipd = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                                (a.z - b.z) * (a.z - b.z));
    const auto forward = [](const XrView& view) {
        return xrscreen::Rotate(FromXr(view.pose).orientation, {0.0f, 0.0f, -1.0f});
    };
    const float cant =
        std::acos(std::clamp(xrscreen::Dot(forward(views[0]), forward(views[1])), -1.0f, 1.0f));
    const XrFovf& l = views[0].fov;
    const XrFovf& r = views[1].fov;
    Log(0, "eye views located: IPD %.1f mm, displays %.1f deg apart, FOV left/right/up/down "
           "%.0f/%.0f/%.0f/%.0f and %.0f/%.0f/%.0f/%.0f deg",
        ipd * 1000.0f, cant * kDeg, l.angleLeft * kDeg, l.angleRight * kDeg, l.angleUp * kDeg,
        l.angleDown * kDeg, r.angleLeft * kDeg, r.angleRight * kDeg, r.angleUp * kDeg,
        r.angleDown * kDeg);
}

/// The optional layers a frame used, newest first: the one to drop when the runtime keeps
/// rejecting frames.
struct OptionalLayers {
    bool stereo_depth = false, alt = false, mono = false, cylinder = false, background = false;
    bool hud = false, ui = false;
};

bool IsLayerError(XrResult result) {
    switch (result) {
    case XR_ERROR_SESSION_LOST:
    case XR_ERROR_SESSION_NOT_RUNNING:
    case XR_ERROR_INSTANCE_LOST:
    case XR_ERROR_CALL_ORDER_INVALID:
        return false;
    default:
        return true;
    }
}

/// No head views while no frames run (session stopped or lost): GetViews returns 0 instead of a
/// frozen pose.
void Unpublish() {
    std::scoped_lock lock{g_views_mutex};
    g_published.api.flags = 0;
    g_published.api.head.valid = 0;
}

void DropOptionalLayer(const OptionalLayers& used) {
    if (used.ui) {
        g->ui_layer = false;
        g->ui_shown = false;
        Log(1, "the runtime rejects the VR UI layer: the game's VR menus are not shown");
    } else if (used.hud) {
        // The newest layer goes first; the host puts the UI back into the game image when its
        // next AcquireHud returns -1.
        g->hud = false;
        g->hud_shown = false;
        Log(1, "the runtime rejects the HUD layer: the HUD goes back into the game image");
    } else if (used.stereo_depth) {
        g->depth_layers = false;
        Log(1, "the runtime rejects the stereo depth layer: stereo images without depth");
    } else if (used.alt) {
        g->alt = false;
        g->mono = true;
        Log(1, "the runtime rejects the alternate-eye projection layer: mono from now on");
    } else if (used.mono) {
        g->mono = false;
        Log(1, "the runtime rejects the mono projection layer: back to the cinema screen");
    } else if (used.cylinder) {
        g->cylinder = false;
        Log(1, "the runtime rejects the curved screen: flat quad from now on");
    } else if (used.background) {
        g->background = false;
        Log(1, "the runtime rejects the background layer: dropped (moving quads may smear)");
    }
}

/// This frame's head from each source, for screen placement and its logs: the VIEW space located
/// in the world space, the middle of the located eyes, XrInput's head (LOCAL, so only when the
/// screen is in LOCAL).
struct HeadSources {
    static constexpr const char* kNames[3] = {"view", "eyes", "input"};
    xrscreen::HeadSample sample[3];
    uint32_t flags[3] = {}; // XrSpaceLocationFlags, XrViewStateFlags, BbXrPose::valid
    bool present[3] = {};   // the source returned anything this frame
    int used = -1;          // the source placement uses (Picked)

    /// The first source that has the head plausibly (Pimax sends junk poses with the valid bits
    /// set, see xr_screen_math.h), else positioned, else oriented.
    const xrscreen::HeadSample& Picked() const {
        static const xrscreen::HeadSample none{};
        return used >= 0 ? sample[used] : none;
    }

    /// For the log: "view (3.10 -77.38 -12.00) flags 0xf IMPLAUSIBLE, eyes ..., input ...;
    /// using view".
    std::string Describe() const {
        std::string text;
        char part[160];
        for (int i = 0; i < 3; ++i) {
            const xrscreen::HeadSample& s = sample[i];
            if (!present[i]) {
                std::snprintf(part, sizeof(part), "%s%s none", i ? ", " : "", kNames[i]);
            } else {
                const xrscreen::Vec3& p = s.pose.position;
                std::snprintf(part, sizeof(part), "%s%s (%.2f %.2f %.2f) flags 0x%x %s",
                              i ? ", " : "", kNames[i], p.x, p.y, p.z, unsigned(flags[i]),
                              xrscreen::Plausible(s) ? "plausible"
                              : s.positioned         ? "IMPLAUSIBLE"
                              : s.oriented           ? "no position"
                                                     : "no pose");
            }
            text += part;
        }
        text += used >= 0 ? std::string("; using ") + kNames[used] : std::string("; no head");
        return text;
    }
};

HeadSources GatherHead(const XrSpaceLocation& head, bool views_now, const XrViewState& view_state,
                       const XrView (&views)[2], const BbXrPose& input_head) {
    HeadSources out;
    out.present[0] = true;
    out.flags[0] = uint32_t(head.locationFlags);
    if ((head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
        IsUnit(head.pose.orientation)) {
        out.sample[0].oriented = true;
        out.sample[0].pose = FromXr(head.pose);
        out.sample[0].positioned = (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                                   xrscreen::IsFinite(out.sample[0].pose.position);
    }
    if (views_now) {
        const xrscreen::Pose left = FromXr(views[0].pose), right = FromXr(views[1].pose);
        out.present[1] = true;
        out.flags[1] = uint32_t(view_state.viewStateFlags);
        out.sample[1].oriented = true;
        out.sample[1].pose = {xrscreen::Nlerp(left.orientation, right.orientation),
                              xrscreen::Scale(xrscreen::Add(left.position, right.position), 0.5f)};
        out.sample[1].positioned = (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                                   xrscreen::IsFinite(out.sample[1].pose.position);
    }
    if (g->space_is_local) {
        out.present[2] = true;
        out.flags[2] = input_head.valid;
        if (input_head.valid) {
            const float* p = input_head.position;
            const float* q = input_head.orientation;
            out.sample[2].oriented = true;
            out.sample[2].pose = {{q[0], q[1], q[2], q[3]}, {p[0], p[1], p[2]}};
            out.sample[2].positioned = xrscreen::IsFinite(out.sample[2].pose.position);
        }
    }
    for (int i = 0; i < 3 && out.used < 0; ++i) {
        out.used = xrscreen::Plausible(out.sample[i]) ? i : -1;
    }
    for (int i = 0; i < 3 && out.used < 0; ++i) {
        out.used = out.sample[i].positioned ? i : -1;
    }
    for (int i = 0; i < 3 && out.used < 0; ++i) {
        out.used = out.sample[i].oriented ? i : -1;
    }
    return out;
}

void FrameLoop() {
    const float distance = g->screen_distance;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    const auto seconds = [&](Clock::time_point t) {
        return std::chrono::duration<double>(t - start).count();
    };
    // World screen: in front of where the head faces once it is tracked (Pimax's LOCAL forward
    // is not the user's, and its first tracked poses can be junk, |y| ~ 77 m with the valid bits
    // set: only plausible poses place it, xrscreen::Placer); placed again when the session is
    // focused again, on a runtime recenter, when the user holds the left menu button, when it
    // went to the default pose and tracking is plausible now, and when the head is far from it.
    // Head-locked: fixed in VIEW space.
    xrscreen::Pose screen = xrscreen::DefaultScreen(distance);
    xrscreen::Placer placer;
    bool ever_placed = g->head_locked;
    double next_wait_log = xrscreen::Placer::kFallbackSeconds;
    // BB_XR_RECENTER_ON_START (default 1): a placement made without steady tracking (the
    // fallback, the default pose) is redone as soon as the head is plausible and steady.
    placer.redo_fallback = EnvFlag("BB_XR_RECENTER_ON_START", true);
    // BB_XR_SCREEN_FOLLOW=off|reappear (default)|always: when the flat screen shows again after the
    // 3D view (lobby, menus, loading), place it in front of the user if the head faces > 60 deg
    // away from it (always: every time).
    const xrscreen::Follow follow = xrscreen::ParseFollow(std::getenv("BB_XR_SCREEN_FOLLOW"));
    bool saw_3d = false;
    double flat_since = 0.0;
    // The cinema screen while the flat image shows (menus, loading, summon, bell): put in front of
    // the head's current yaw each time it appears after the 3D view (BB_XR_CINEMA_RECENTER, default
    // 1), world-locked while it stays, and glided to the front when the head faces more than
    // BB_XR_CINEMA_FOLLOW_DEG (default 60, 0 = never) away for 1 s. Head-locked: unchanged.
    xrscreen::CinemaFollow cinema;
    xrscreen::CinemaFollow::Config cinema_cfg;
    cinema_cfg.recenter_on_show = EnvFlag("BB_XR_CINEMA_RECENTER", true);
    cinema_cfg.follow_deg = EnvFloat("BB_XR_CINEMA_FOLLOW_DEG", xrscreen::kFollowDegrees);
    bool cinema_active = false;
    bool no_layer_logged = false; // one log line per black episode
    if (!g->head_locked) {
        placer.Arm(0.0, false);
    }
    Clock::time_point last_event_place, menu_since;
    bool event_honored = false, menu_down = false, menu_fired = false;
    const char* recentering = nullptr; // a manual recenter is pending: what asked for it
    // The latest valid eye views, kept through tracking hiccups for the background layer.
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool have_views = false, views_logged = false;
    FailureLog wait_failures, begin_failures, end_failures, locate_failures;
    int rejected_frames = 0;
    // BB_XR_HUD_FOLLOW=lazy: where the HUD points, eased toward the head (xr_hud.h).
    xrhud::LazyFollow hud_follow;
    Clock::time_point hud_alpha_polled = start;
    Clock::time_point hud_follow_at = start;
    while (g->running) {
        HandleEvents();
        if (!g->session_running) {
            Unpublish();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        XrFrameState state{XR_TYPE_FRAME_STATE};
        if (!CheckEvery(xrWaitFrame(g->session, nullptr, &state), "xrWaitFrame", wait_failures)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // Like xrEndFrame, xrBeginFrame may submit on the shared queue (XR_KHR_vulkan_enable).
        g->QueueLock(true);
        const XrResult begun = xrBeginFrame(g->session, nullptr);
        g->QueueLock(false);
        if (!CheckEvery(begun, "xrBeginFrame", begin_failures)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const XrTime time = state.predictedDisplayTime;
        g->visible = state.shouldRender == XR_TRUE;
        XrInput::Update(time, g->focused);
        const auto now = Clock::now();
        const BbXrInput input = XrInput::Peek();

        // Head and eyes at display time, in the world space.
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags need =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        const bool head_valid = XR_SUCCEEDED(xrLocateSpace(g->view, g->world, time, &head)) &&
                                (head.locationFlags & need) == need;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        XrView located[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        bool views_now = false;
        if (g->stereo) {
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime = time;
            locate.space = g->world;
            uint32_t count = 0;
            views_now = CheckEvery(xrLocateViews(g->session, &locate, &view_state, 2, &count,
                                                 located),
                                   "xrLocateViews", locate_failures) &&
                        count == 2 &&
                        (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
                        IsUnit(located[0].pose.orientation) && IsUnit(located[1].pose.orientation);
        }
        if (views_now) {
            views[0] = located[0];
            views[1] = located[1];
            have_views = true;
            if (!views_logged) {
                views_logged = true;
                LogViews(views);
            }
        }
        // App-rendered stereo shows its images one frame later: hand it the views (and the
        // controllers) predicted for then, in the world space (AcquireStereo).
        BbXrPose ahead_grip[2] = {}, ahead_aim[2] = {};
        if (g->stereo && g->focused) {
            XrInput::LocateHands(time + state.predictedDisplayPeriod, g->world, ahead_grip,
                                 ahead_aim);
        }
        XrView ahead[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        XrViewState ahead_state{XR_TYPE_VIEW_STATE};
        bool ahead_valid = false;
        if (g->stereo && views_now) {
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime = time + state.predictedDisplayPeriod;
            locate.space = g->world;
            uint32_t count = 0;
            ahead_valid = XR_SUCCEEDED(xrLocateViews(g->session, &locate, &ahead_state, 2, &count,
                                                     ahead)) &&
                          count == 2 &&
                          (ahead_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
                          IsUnit(ahead[0].pose.orientation) && IsUnit(ahead[1].pose.orientation);
        }

        const double t = seconds(now);
        const HeadSources sources = GatherHead(head, views_now, view_state, located, input.head);
        const xrscreen::HeadSample& sample = sources.Picked();
        const auto arm = [&](bool immediate, const char* why, const char* manual = nullptr) {
            placer.Arm(t, immediate);
            recentering = manual;
            next_wait_log = t + xrscreen::Placer::kFallbackSeconds;
            Log(0, "placing the screen again: %s; head %s", why, sources.Describe().c_str());
        };
        if (g->refocused.exchange(false) && !g->head_locked) {
            arm(false, "the session is focused again (headset on)");
        }
        if (g->recentered.exchange(false) && !g->head_locked) {
            // The runtime may send these over and over: only the first, then one per 10 s.
            if (!event_honored || now - last_event_place > std::chrono::seconds(10)) {
                event_honored = true;
                last_event_place = now;
                arm(false, "the runtime moved the reference space");
            }
        }
        // Manual recenter: the left stick click held 1 s alone (XrInput: that click never
        // reaches the game), or the menu button itself held ~1 s on runtimes that report it (a
        // short press still reaches the game as Options; Pimax keeps that button). Buttons are
        // only set while focused; tracked controllers are not required.
        if (XrInput::TakeRecenter() && !g->head_locked) {
            arm(true, "left stick click held or the dashboard's Recenter (recenter)", "L3 hold");
        }
        if (!g->head_locked) {
            if (XrInput::MenuButtonDown()) {
                if (!menu_down) {
                    menu_down = true;
                    menu_since = now;
                    Log(0, "left menu pressed (hold 1 s to place the screen again; controllers "
                           "%s)",
                        input.active ? "tracked" : "not tracked");
                } else if (!menu_fired && now - menu_since >= std::chrono::milliseconds(1000)) {
                    menu_fired = true;
                    arm(true, "left menu held (recenter)", "menu hold");
                }
            } else {
                menu_down = menu_fired = false;
            }
        }
        if (placer.Waiting()) {
            if (placer.Update(t, sample, distance, &screen)) {
                ever_placed = true;
                if (recentering) {
                    Log(0, "screen recentered (%s): %s", recentering,
                        xrscreen::HowName(placer.how));
                    recentering = nullptr;
                }
                switch (placer.how) {
                case xrscreen::Placer::How::Tracked:
                    Log(0, "screen placed %.2f m ahead, at head height %.2f m, yaw %.0f deg",
                        distance, screen.position.y, xrscreen::Yaw(screen.orientation) * 57.29578f);
                    break;
                case xrscreen::Placer::How::Steady:
                    Log(0, "screen placed %.2f m ahead of the steady head, at head height %.2f m, "
                           "yaw %.0f deg (the runtime's space origin is far from the head: "
                           "last y %.2f)",
                        distance, screen.position.y, xrscreen::Yaw(screen.orientation) * 57.29578f,
                        placer.LastY());
                    break;
                case xrscreen::Placer::How::Fallback:
                    Log(0, "screen placed %.2f m ahead, at head height %.2f m, yaw %.0f deg (head "
                           "tracking never steadied: at the latest plausible head pose, %.1f s "
                           "old)",
                        distance, screen.position.y, xrscreen::Yaw(screen.orientation) * 57.29578f,
                        placer.age);
                    break;
                case xrscreen::Placer::How::Kept:
                    Log(1, "screen kept where it was: no plausible head pose since placement was "
                           "asked (implausible %d, no position %d, no pose %d); placed again once "
                           "tracking is plausible; head %s",
                        placer.out_of_range, placer.no_position, placer.no_pose,
                        sources.Describe().c_str());
                    break;
                default:
                    Log(1, "screen at the default pose %.2f m in front of the space origin: no "
                           "plausible head pose (implausible %d, no position %d, no pose %d, last "
                           "y %.2f); placed again once tracking is plausible; head %s",
                        distance, placer.out_of_range, placer.no_position, placer.no_pose,
                        placer.LastY(), sources.Describe().c_str());
                    break;
                }
            } else if (t >= next_wait_log) {
                next_wait_log = t + 3.0;
                Log(0, "screen placement waiting for a plausible head pose: %d/%d plausible "
                       "samples (since armed: no pose %d, no position %d, implausible (|y| > %.0f "
                       "m or > %.0f m from the origin) %d; default pose in %.1f s); head %s",
                    placer.good_count, placer.samples, placer.no_pose, placer.no_position,
                    xrscreen::kPlausibleY, xrscreen::kPlausibleRadius, placer.out_of_range,
                    placer.DefaultIn(t), sources.Describe().c_str());
            }
        } else if (ever_placed && !g->head_locked) {
            // Placed at the default pose (junk tracking) or far from the plausibly tracked head
            // (a pose that slipped through, or the space moved): place it again.
            switch (placer.Watch(t, sample, screen)) {
            case xrscreen::Placer::Again::Far: {
                char why[96];
                std::snprintf(why, sizeof(why), "the head is %.1f m from the screen",
                              placer.head_to_screen);
                arm(false, why);
                break;
            }
            case xrscreen::Placer::Again::Tracking:
                arm(false, "head tracking is plausible now");
                break;
            default:
                break;
            }
        }

        // The right controller's aim ray and the head in the screen's space: XrInput's poses are
        // LOCAL, the screen may be in STAGE or (head-locked) VIEW.
        XrSpaceLocation local_in_space{XR_TYPE_SPACE_LOCATION};
        local_in_space.pose.orientation.w = 1.0f; // identity when the screen is in LOCAL
        const bool aiming =
            input.aim[1].valid && input.head.valid &&
            (g->space_is_local ||
             (XR_SUCCEEDED(xrLocateSpace(g->local, g->space, time, &local_in_space)) &&
              (local_in_space.locationFlags & need) == need));
        const xrscreen::Pose to_space = FromXr(local_in_space.pose);
        const auto in_space = [&](const BbXrPose& p) {
            return xrscreen::Compose(to_space, {{p.orientation[0], p.orientation[1],
                                                 p.orientation[2], p.orientation[3]},
                                                {p.position[0], p.position[1], p.position[2]}});
        };
        const xrscreen::Pose aim = in_space(input.aim[1]);
        const xrscreen::Pose viewer = in_space(input.head);

        std::scoped_lock lock{g->state_mutex};
        const bool game = state.shouldRender && g->flat.handle && g->flat.released;
        const float width = g->screen_width;
        const float aspect = g->flat.width && g->flat.height
                                 ? float(g->flat.width) / float(g->flat.height)
                                 : 16.0f / 9.0f;
        XrCompositionLayerProjectionView eyes[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                    {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerDepthInfoKHR depth_info[2] = {{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                                                        {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad beam{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad dot{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad hands[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD},
                                           {XR_TYPE_COMPOSITION_LAYER_QUAD}};
        XrCompositionLayerQuad hud_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerProjectionView ui_eyes[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                       {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection ui_projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[10] = {};
        uint32_t layer_count = 0;
        const auto add = [&](const auto& layer) {
            layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
        };
        OptionalLayers used;

        // 1. Projection, behind everything: the host's stereo images (ABI 4) at the views they
        // were rendered with, so the runtime reprojects them to the head pose at display time.
        const bool stereo_fresh =
            g->stereo_shown && now - g->stereo_released_at < std::chrono::seconds(1);
        bool stereo_used = false;
        if (state.shouldRender && stereo_fresh) {
            stereo_used = true;
            const bool with_depth = g->depth_layers && g->stereo_depth_shown;
            for (int eye = 0; eye < 2; ++eye) {
                eyes[eye].pose = g->stereo_views[eye].pose;
                eyes[eye].fov = g->stereo_views[eye].fov;
                eyes[eye].subImage.swapchain = g->stereo_color[eye].handle;
                eyes[eye].subImage.imageRect = FullRect(g->stereo_color[eye]);
                if (with_depth) {
                    depth_info[eye].subImage.swapchain = g->stereo_depth[eye].handle;
                    depth_info[eye].subImage.imageRect = FullRect(g->stereo_depth[eye]);
                    depth_info[eye].minDepth = 0.0f;
                    depth_info[eye].maxDepth = 1.0f;
                    depth_info[eye].nearZ = g->stereo_near;
                    depth_info[eye].farZ = g->stereo_far;
                    eyes[eye].next = &depth_info[eye];
                }
            }
            used.stereo_depth = with_depth;
            if (!g->stereo_logged) {
                g->stereo_logged = true;
                Log(0, "first app-rendered stereo frame on the headset (%ux%u per eye%s)",
                    g->stereo_color[0].width, g->stereo_color[0].height,
                    with_depth ? ", with depth" : "");
            }
        } else if (g->alt && state.shouldRender && g->alt_eyes.Showing(Steady(now), 1.0)) {
            // Alternate-eye stereo (BB_XR_MODE=alt, ABI 6): each eye shows its latest image with
            // the view (eye pose, frustum) it was rendered with; the eye the game did not render
            // this frame is reprojected by the runtime from its own older pose.
            for (int eye = 0; eye < 2; ++eye) {
                eyes[eye].pose = g->alt_pose[eye];
                eyes[eye].fov = g->alt_fov[eye];
                eyes[eye].subImage.swapchain = g->alt_chain[eye].handle;
                eyes[eye].subImage.imageRect = FullRect(g->alt_chain[eye]);
            }
            used.alt = true;
            g->alt_ages.Frame(g->alt_eyes.released);
            if (!g->alt_logged) {
                g->alt_logged = true;
                Log(0, "first alternate-eye frame on the headset (%ux%u and %ux%u per eye)",
                    g->alt_chain[0].width, g->alt_chain[0].height, g->alt_chain[1].width,
                    g->alt_chain[1].height);
            }
        } else if (g->mono && game && g->flat_views_valid) {
            // Mono (BB_XR_MODE=mono only; alt shows its menus, loading and title frames on the
            // cinema screen below: one flat image, identical in both eyes, no reprojection of a
            // 110 deg window image, so nothing looks doubled): the game image to both eyes. With head tracking (ABI 5
            // ReleaseFlatView) both eyes get the view the game camera was rendered with: its head
            // orientation and its one symmetric frustum (per-eye frusta put the one image in two
            // directions: double vision), each eye at its offset from the head, so the runtime
            // reprojects from the right pose. Without it: the views current at release, each
            // eye's own FOV (looks wrong: stretched, swims). Still to do: a depth layer
            // (XR_KHR_composition_layer_depth) for positional reprojection.
            const xrscreen::Pose render_head = {
                {g->flat_view.head.orientation[0], g->flat_view.head.orientation[1],
                 g->flat_view.head.orientation[2], g->flat_view.head.orientation[3]},
                {g->flat_view.head.position[0], g->flat_view.head.position[1],
                 g->flat_view.head.position[2]}};
            for (int eye = 0; eye < 2; ++eye) {
                if (g->flat_view_valid) {
                    eyes[eye].pose = ToXr(xrscreen::MonoEye(render_head,
                                                            FromXr(g->flat_views[eye].pose),
                                                            FromXr(g->flat_views[1 - eye].pose)));
                    eyes[eye].fov = {g->flat_view.fov.left, g->flat_view.fov.right,
                                     g->flat_view.fov.up, g->flat_view.fov.down};
                } else {
                    eyes[eye].pose = g->flat_views[eye].pose;
                    eyes[eye].fov = g->flat_views[eye].fov;
                }
                eyes[eye].subImage.swapchain = g->flat.handle;
                eyes[eye].subImage.imageRect = FullRect(g->flat);
            }
            used.mono = true;
        } else if (state.shouldRender && g->background && have_views) {
            for (int eye = 0; eye < 2; ++eye) {
                eyes[eye].pose = views[eye].pose;
                eyes[eye].fov = views[eye].fov;
                eyes[eye].subImage.swapchain = g->background_eyes[eye].handle;
                eyes[eye].subImage.imageRect = FullRect(g->background_eyes[eye]);
            }
            used.background = true;
        }
        if (stereo_used || used.alt || used.mono) {
            saw_3d = true;
        }
        if (stereo_used || used.alt || used.mono || used.background) {
            projection.space = g->world;
            projection.viewCount = 2;
            projection.views = eyes;
            add(projection);
        }

        // 2. The cinema screen. Opaque: the presenter sets the game's alpha (left at 0) to 1,
        // as the Pimax runtime blends layers by alpha whatever their flags say.
        // Debounced: the flat screen must have been showing for 0.5 s (alt flickers to it between
        // eye frames) after 3D frames.
        const bool flat_shown = game && !used.mono && !used.alt && !stereo_used;
        if (!flat_shown) {
            flat_since = t;
            if (cinema_active) {
                cinema_active = false;
                cinema.Hide();
            }
        } else if (saw_3d && t - flat_since >= 0.5) {
            saw_3d = false;
            cinema_active = true;
            if (!g->head_locked && !placer.Waiting() && cinema.Show(cinema_cfg, sample, distance,
                                                                    &screen)) {
                ever_placed = true;
                Log(0, "cinema screen in front of the head: yaw %.0f deg, head %s",
                    xrscreen::Yaw(screen.orientation) * 57.29578f, sources.Describe().c_str());
            } else if (!g->head_locked && !placer.Waiting() && !cinema_cfg.recenter_on_show &&
                xrscreen::ShouldFollowOnReappear(follow, sample, screen)) {
                char why[112];
                std::snprintf(why, sizeof(why),
                              "the flat screen is back after the 3D view, the head faces %.0f deg "
                              "away from it",
                              xrscreen::HeadAngleToScreen(sample, screen));
                arm(true, why, "flat screen back");
            }
        }
        if (flat_shown && t - flat_since >= 0.5 && !g->head_locked && !placer.Waiting() &&
            ever_placed) {
            cinema_active = true;
            if (cinema.Tick(t, cinema_cfg, sample, distance, &screen)) {
                Log(0, "cinema screen: the head faced > %.0f deg away for 1 s, gliding it back "
                       "in front; head %s",
                    cinema_cfg.follow_deg, sources.Describe().c_str());
            }
        }
        if (game && !used.mono && !used.alt) {
            if (g->cylinder) {
                cylinder.space = g->space;
                cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                cylinder.subImage.swapchain = g->flat.handle;
                cylinder.subImage.imageRect = FullRect(g->flat);
                // Centered on the head the screen was placed for; the arc is `width` long.
                cylinder.pose = ToXr(xrscreen::CylinderCenter(screen, distance));
                cylinder.radius = distance;
                cylinder.centralAngle = xrscreen::ArcAngle(width, distance);
                cylinder.aspectRatio = aspect;
                add(cylinder);
                used.cylinder = true;
            } else {
                quad.space = g->space;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = g->flat.handle;
                quad.subImage.imageRect = FullRect(g->flat);
                quad.pose = ToXr(screen);
                quad.size = {width, width / aspect};
                add(quad);
            }
        }

        // 2a. Black guard: no 3D image fresh enough, no cinema frame, no background: the
        // compositor would show black. Keep the last eye images (alt or stereo) instead.
        if (state.shouldRender &&
            !(stereo_used || used.alt || used.mono || used.background || game)) {
            const char* stale = nullptr;
            if (g->alt && g->alt_eyes.released[0] && g->alt_eyes.released[1] &&
                g->alt_chain[0].handle && g->alt_chain[1].handle) {
                for (int eye = 0; eye < 2; ++eye) {
                    eyes[eye].pose = g->alt_pose[eye];
                    eyes[eye].fov = g->alt_fov[eye];
                    eyes[eye].subImage.swapchain = g->alt_chain[eye].handle;
                    eyes[eye].subImage.imageRect = FullRect(g->alt_chain[eye]);
                }
                stale = "last alt eye images";
            } else if (g->stereo_shown && g->stereo_color[0].handle && g->stereo_color[1].handle) {
                for (int eye = 0; eye < 2; ++eye) {
                    eyes[eye].pose = g->stereo_views[eye].pose;
                    eyes[eye].fov = g->stereo_views[eye].fov;
                    eyes[eye].subImage.swapchain = g->stereo_color[eye].handle;
                    eyes[eye].subImage.imageRect = FullRect(g->stereo_color[eye]);
                }
                stale = "last stereo eye images";
            }
            if (stale) {
                projection.space = g->world;
                projection.viewCount = 2;
                projection.views = eyes;
                add(projection);
            }
            if (!no_layer_logged) {
                no_layer_logged = true;
                Log(1, "VR: no layer to show (no fresh eye image: stereo %d, alt showing %d "
                       "(eyes released %llu/%llu); no flat image: handle %d released %d; "
                       "background %d): %s",
                    int(stereo_fresh), int(g->alt && g->alt_eyes.Showing(Steady(now), 1.0)),
                    static_cast<unsigned long long>(g->alt_eyes.released[0]),
                    static_cast<unsigned long long>(g->alt_eyes.released[1]),
                    int(g->flat.handle != nullptr), int(g->flat.released),
                    int(g->background && have_views),
                    stale ? stale : "nothing older to keep either");
            }
        } else {
            no_layer_logged = false;
        }

        // 2b. The HUD layer (ABI 7): the game's UI image, the same in both eyes, in front of the
        // scene and the cinema screen; head-locked (VIEW space) or lazily following the head.
        // Hidden while the host's frames carry their UI themselves (HideHud) and when the host
        // stops sending (250 ms: paused, loading).
        const bool hud_fresh = g->hud && g->hud_shown && g->hud_chain.handle &&
                               g->hud_chain.released &&
                               now - g->hud_released_at < std::chrono::milliseconds(250);
        if (state.shouldRender && hud_fresh) {
            const float hud_aspect =
                g->hud_chain.height ? float(g->hud_chain.width) / float(g->hud_chain.height)
                                    : 16.0f / 9.0f;
            xrhud::Quad placed{};
            bool placed_ok = false;
            if (g->hud_config.follow == xrhud::Follow::Lazy) {
                if (head_valid) {
                    const xrscreen::Pose head_pose = FromXr(head.pose);
                    hud_follow.Update(
                        head_pose.orientation,
                        float(std::chrono::duration<double>(now - hud_follow_at).count()));
                    placed = hud_follow.Place(g->hud_config, hud_aspect, head_pose.position);
                    hud_quad.space = g->world;
                    placed_ok = true;
                }
                hud_follow_at = now;
            } else {
                placed = xrhud::HeadQuad(g->hud_config, hud_aspect);
                hud_quad.space = g->view;
                placed_ok = true;
            }
            if (placed_ok) {
                // Live knob: bb_xr_hud_alpha.txt (premul|straight|opaque), polled once a second.
                if (now - hud_alpha_polled > std::chrono::seconds(1)) {
                    hud_alpha_polled = now;
                    if (std::FILE* f = std::fopen("bb_xr_hud_alpha.txt", "rb")) {
                        char word[32] = {};
                        std::fread(word, 1, sizeof(word) - 1, f);
                        std::fclose(f);
                        const xrhud::Alpha mode = xrhud::ParseAlpha(word);
                        if (mode != g->hud_alpha) {
                            g->hud_alpha = mode;
                            Log(0, "HUD blend: %s (bb_xr_hud_alpha.txt)",
                                xrhud::AlphaName(mode));
                        }
                    }
                }
                hud_quad.layerFlags =
                    g->hud_alpha == xrhud::Alpha::Opaque
                        ? XrCompositionLayerFlags(0)
                        : XrCompositionLayerFlags(
                              XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                              (g->hud_alpha == xrhud::Alpha::Straight
                                   ? XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
                                   : 0));
                hud_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                hud_quad.subImage.swapchain = g->hud_chain.handle;
                hud_quad.subImage.imageRect = FullRect(g->hud_chain);
                hud_quad.pose = ToXr(placed.pose);
                hud_quad.size = {placed.width, placed.height};
                add(hud_quad);
                used.hud = true;
                if (!g->hud_logged) {
                    g->hud_logged = true;
                    Log(0, "HUD layer: first image on the headset (%ux%u, %.2f x %.2f m at "
                           "%.2f m)",
                        g->hud_chain.width, g->hud_chain.height, placed.width, placed.height,
                        g->hud_config.distance);
                }
            }
        } else if (!hud_fresh) {
            hud_follow.Reset(); // the next HUD starts straight ahead
        }

        // 2c. The game's VR UI (ABI 8): menus at the hands and in front, the swing/stamina HUD;
        // drawn by the UI thread for the views it shows them with (premultiplied alpha).
        const bool ui_fresh = g->ui_layer && g->ui_shown && g->ui_chain[0].released &&
                              g->ui_chain[1].released &&
                              now - g->ui_released_at < std::chrono::milliseconds(250);
        if (state.shouldRender && ui_fresh) {
            for (int eye = 0; eye < 2; ++eye) {
                ui_eyes[eye].pose = g->ui_views[eye].pose;
                ui_eyes[eye].fov = g->ui_views[eye].fov;
                ui_eyes[eye].subImage.swapchain = g->ui_chain[eye].handle;
                ui_eyes[eye].subImage.imageRect = FullRect(g->ui_chain[eye]);
            }
            ui_projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            ui_projection.space = g->world;
            ui_projection.viewCount = 2;
            ui_projection.views = ui_eyes;
            add(ui_projection);
            used.ui = true;
            if (!g->ui_logged) {
                g->ui_logged = true;
                Log(0, "VR UI: first layer on the headset (%ux%u per eye)", g->ui_chain[0].width,
                    g->ui_chain[0].height);
            }
        }

        // 3. Laser pointer: where the right controller's aim ray meets the screen as shown, in
        // pixels of the image on it (published every frame; drawn with BB_XR_LASER=1).
        BbXrPointer pointer{};
        const uint32_t pixels_x = g->flat.width ? g->flat.width : 1920;
        const uint32_t pixels_y = g->flat.height ? g->flat.height : 1080;
        if (aiming && game && !used.mono && !used.alt) {
            pointer.hit =
                g->cylinder
                    ? xrscreen::RayToCylinderPixel(aim.position, aim.orientation, screen, distance,
                                                   width, pixels_x, pixels_y, true, &pointer.u,
                                                   &pointer.v, &pointer.distance)
                    : xrscreen::RayToPixel(aim.position, aim.orientation, screen, width, pixels_x,
                                           pixels_y, true, &pointer.u, &pointer.v,
                                           &pointer.distance);
        }
        if (aiming && g->laser && state.shouldRender) {
            // A 2.5 mm beam turned toward the eyes, ending on the screen or 3 m out; the dot is a
            // 2 cm billboard at the hit, drawn over the screen (layers draw in order).
            const xrscreen::Vec3 dir = xrscreen::Rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
            const float length = pointer.hit ? pointer.distance : 3.0f;
            beam.space = g->space;
            beam.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            beam.subImage.swapchain = g->lasers[pointer.hit ? 1 : 0].handle;
            beam.subImage.imageRect = FullRect(g->lasers[0]);
            beam.pose = ToXr(xrscreen::BeamPose(aim.position, dir, length, viewer.position));
            beam.size = {0.0025f, length};
            add(beam);
            if (pointer.hit) {
                dot.space = g->space;
                dot.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                dot.subImage.swapchain = g->lasers[2].handle;
                dot.subImage.imageRect = FullRect(g->lasers[2]);
                dot.pose = ToXr({viewer.orientation,
                                 xrscreen::Add(aim.position, xrscreen::Scale(dir, length))});
                dot.size = {0.02f, 0.02f};
                add(dot);
            }
        }

        // 4. Hand markers on top.
        if (state.shouldRender && g->show_hands) {
            constexpr uint32_t held[2] = {BBXR_BTN_L1 | BBXR_BTN_L2, BBXR_BTN_R1 | BBXR_BTN_R2};
            for (int hand = 0; hand < 2; ++hand) {
                const BbXrPose& grip = input.grip[hand];
                if (!grip.valid || !input.head.valid) {
                    continue;
                }
                // The 5 cm marker is drawn after the UI layer, on the grip: it would hide the held item's mesh.
                if (g->game_ui && g->game_ui->HeldHandForMarker() == hand) {
                    continue;
                }
                const int color = XrInput::MsSinceGesture(hand) < 300.0 ? 2
                                  : (input.buttons & held[hand])     ? 1
                                                                     : 0;
                auto& marker = hands[hand];
                marker.space = g->local;
                marker.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                marker.subImage.swapchain = g->markers[color].handle;
                marker.subImage.imageRect = FullRect(g->markers[color]);
                // Billboard: faces the head.
                marker.pose.orientation = {input.head.orientation[0], input.head.orientation[1],
                                           input.head.orientation[2], input.head.orientation[3]};
                marker.pose.position = {grip.position[0], grip.position[1], grip.position[2]};
                marker.size = {0.05f, 0.05f};
                add(marker);
            }
        }

        // Alternate-eye pacing, every 10 s: how old each eye's image is when the display shows it.
        if (g->alt) {
            ++g->alt_display_frames;
            g->alt_shown_frames += used.alt ? 1 : 0;
            if (now >= g->alt_next_log) {
                const auto& w = g->alt_ages.window;
                if (g->alt_next_log != Clock::time_point{} && g->alt_display_frames) {
                    const double n = w.frames ? double(w.frames) : 1.0;
                    Log(0, "alt: %llu of %llu display frames showed the eyes; eye images L %llu R "
                           "%llu (total); age in display frames L mean %.2f max %u, R mean %.2f "
                           "max %u; one eye new %.0f%%, both new %.0f%%, both old %.0f%%",
                        static_cast<unsigned long long>(g->alt_shown_frames),
                        static_cast<unsigned long long>(g->alt_display_frames),
                        static_cast<unsigned long long>(g->alt_eyes.released[0]),
                        static_cast<unsigned long long>(g->alt_eyes.released[1]),
                        g->alt_ages.MeanAge(0), w.max_age[0], g->alt_ages.MeanAge(1), w.max_age[1],
                        100.0 * double(w.one_fresh) / n, 100.0 * double(w.both_fresh) / n,
                        100.0 * double(w.both_stale) / n);
                }
                g->alt_ages.ResetWindow();
                g->alt_display_frames = g->alt_shown_frames = 0;
                g->alt_next_log = now + std::chrono::seconds(10);
            }
        }

        // What GetViews reports until the next frame.
        {
            std::scoped_lock views_lock{g_views_mutex};
            BbXrViews& out = g_published.api;
            out.display_time = time;
            out.flags = views_now ? uint32_t(view_state.viewStateFlags) : 0u;
            out.head.valid = head_valid ? 1u : 0u;
            if (head_valid) {
                out.head = ToApi(head.pose, true);
            }
            for (int eye = 0; eye < 2 && views_now; ++eye) {
                out.eye[eye].pose = ToApi(views[eye].pose, true);
                out.eye[eye].fov = {views[eye].fov.angleLeft, views[eye].fov.angleRight,
                                    views[eye].fov.angleUp, views[eye].fov.angleDown};
            }
            const xrscreen::Pose shown =
                g->head_locked ? xrscreen::Compose(FromXr(head.pose), screen) : screen;
            out.screen.pose = ToApi(ToXr(shown), ever_placed && (!g->head_locked || head_valid));
            out.screen.width = width;
            out.screen.height = width / aspect;
            out.screen.radius = g->cylinder ? distance : 0.0f;
            out.screen.head_locked = g->head_locked ? 1u : 0u;
            out.pointer = pointer;
            g_published.views[0] = views[0];
            g_published.views[1] = views[1];
            g_published.views_valid = have_views;
            if (ahead_valid) {
                g_published.next_views[0] = ahead[0];
                g_published.next_views[1] = ahead[1];
                g_published.next_time = time + state.predictedDisplayPeriod;
                g_published.next_flags = uint32_t(ahead_state.viewStateFlags);
                g_published.next_valid = true;
            } else if (views_now) {
                g_published.next_views[0] = views[0];
                g_published.next_views[1] = views[1];
                g_published.next_time = time;
                g_published.next_flags = uint32_t(view_state.viewStateFlags);
                g_published.next_valid = true;
            }
            for (int hand = 0; hand < 2; ++hand) {
                g_published.next_grip[hand] = ahead_grip[hand];
                g_published.next_aim[hand] = ahead_aim[hand];
            }
            XrInput::Analog(g_published.trigger, g_published.squeeze);
            ++g_published.frame;
        }
        g_frame_cv.notify_all();

        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = time;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = layer_count;
        end.layers = layers;
        g->QueueLock(true);
        const XrResult ended = xrEndFrame(g->session, &end);
        g->QueueLock(false);
        if (CheckEvery(ended, "xrEndFrame", end_failures)) {
            rejected_frames = 0;
        } else if (IsLayerError(ended) && ++rejected_frames >= 3) {
            rejected_frames = 0;
            DropOptionalLayer(used);
        }
    }
    Unpublish();
}

// ---- The game's VR UI (ABI 8, BB_VR_UI=1) ------------------------------------------------------

/// out/vr_assets: BB_VR_ASSETS, else next to the executable or one folder up, else ./out.
std::string FindAssets() {
    if (const char* env = std::getenv("BB_VR_ASSETS"); env && *env) {
        return env;
    }
    std::error_code ec;
    std::filesystem::path exe;
#ifdef _WIN32
    wchar_t path[1024] = {};
    if (GetModuleFileNameW(nullptr, path, 1024)) {
        exe = std::filesystem::path(path).parent_path();
    }
#endif
    for (const std::filesystem::path& dir :
         {exe / "vr_assets", exe.parent_path() / "vr_assets", std::filesystem::path("out/vr_assets"),
          std::filesystem::path("vr_assets")}) {
        if (!dir.empty() && std::filesystem::exists(dir / "ui" / "icons.json", ec)) {
            return dir.string();
        }
    }
    return (exe / "vr_assets").string();
}

double SteadyNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The UI chains for this eye size (state_mutex held).
bool PrepareUiChains(uint32_t width, uint32_t height) {
    if (g->ui_chain[0].handle && g->ui_chain[0].width == width && g->ui_chain[0].height == height) {
        return true;
    }
    for (Chain& chain : g->ui_chain) {
        DestroyChain(chain);
    }
    const int64_t format = PickFormat(VK_FORMAT_R8G8B8A8_UNORM);
    for (Chain& chain : g->ui_chain) {
        if (!CreateChain(chain, width, height, format, "VR UI swapchain",
                         XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) {
            for (Chain& c : g->ui_chain) {
                DestroyChain(c);
            }
            return false;
        }
    }
    Log(0, "VR UI: layer %ux%u per eye, format %lld", width, height, (long long)format);
    return true;
}

/// The UNORM format of an sRGB one (the desktop mirror is sampled as stored).
VkFormat UnormOf(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
    default:
        return format;
    }
}

/// The UI thread: one update per headset frame, rendered for the views one display period ahead
/// (as AcquireStereo), so the runtime shows it with the head and hands it was drawn for.
void UiLoop() {
    uint64_t seen = 0;
    std::vector<vrui::Haptic> haptics;
    std::string log;
    uint32_t failures = 0;
    while (g->running) {
        Published views;
        {
            std::unique_lock lock{g_views_mutex};
            if (!g_frame_cv.wait_for(lock, std::chrono::milliseconds(100),
                                     [&] { return g_published.frame != seen || !g->running; })) {
                continue;
            }
            if (!g->running) {
                break;
            }
            seen = g_published.frame;
            views = g_published;
        }
        vrui::FrameInput in;
        in.time = SteadyNow();
        in.stage = views.api.space == 1;
        in.pad = XrInput::Peek();
        in.ms_since_swing = XrInput::MsSinceGesture(1);
        const auto pose = [](const BbXrPose& p) {
            return sb::UiPose{p.valid != 0, {p.position[0], p.position[1], p.position[2]},
                              {p.orientation[0], p.orientation[1], p.orientation[2],
                               p.orientation[3]}};
        };
        if (views.next_valid) {
            const XrPosef& l = views.next_views[0].pose;
            const XrPosef& r = views.next_views[1].pose;
            const xrscreen::Pose head = {
                xrscreen::Nlerp(FromXr(l).orientation, FromXr(r).orientation),
                xrscreen::Scale(xrscreen::Add(FromXr(l).position, FromXr(r).position), 0.5f)};
            in.head = {true, {head.position.x, head.position.y, head.position.z},
                       {head.orientation.x, head.orientation.y, head.orientation.z,
                        head.orientation.w}};
        }
        for (int h = 0; h < 2; ++h) {
            in.grip[h] = pose(views.next_grip[h]);
            in.aim[h] = pose(views.next_aim[h]);
            in.trigger[h] = views.trigger[h];
            in.squeeze[h] = views.squeeze[h];
        }
        haptics.clear();
        log.clear();
        g->game_ui->Update(in, haptics, log);
        for (const vrui::Haptic& h : haptics) {
            XrInput::Haptic(h.hand, h.amplitude, h.seconds);
        }
        if (!log.empty()) {
            size_t start = 0;
            while (start < log.size()) {
                size_t end = log.find('\n', start);
                if (end == std::string::npos) {
                    end = log.size();
                }
                if (end > start) {
                    Log(0, "%s", log.substr(start, end - start).c_str());
                }
                start = end + 1;
            }
        }
        // The GPU must be done with the previous frame's geometry before Build rewrites it; while
        // it is busy (a heavy game frame on the shared queue) the last images keep showing.
        if (!g->ui_gpu.Wait(30'000'000)) {
            continue;
        }
        {
            // The previous mirror image is finished now.
            std::scoped_lock lock{g->state_mutex};
            if (g->mirror_pending >= 0) {
                g->mirror_ready = g->mirror_pending;
                g->mirror_ready_info = g->mirror_pending_info;
                g->mirror_ready_at = std::chrono::steady_clock::now();
                g->mirror_pending = -1;
            }
        }
        // Live knob: bb_xr_mirror_ui.txt (0|1|on|off), polled once a second.
        if (const auto polled = std::chrono::steady_clock::now();
            polled - g->mirror_file_polled > std::chrono::seconds(1)) {
            g->mirror_file_polled = polled;
            if (std::FILE* f = std::fopen("bb_xr_mirror_ui.txt", "rb")) {
                char word[16] = {};
                std::fread(word, 1, sizeof(word) - 1, f);
                std::fclose(f);
                const bool on = word[0] == '1' || word[0] == 'o' && word[1] == 'n' ||
                                word[0] == 't' || word[0] == 'y';
                const bool off = word[0] == '0' || word[0] == 'o' && word[1] == 'f' ||
                                 word[0] == 'f' || word[0] == 'n';
                if ((on || off) && on != g->mirror_on) {
                    g->mirror_on = on;
                    Log(0, "VR UI mirror: %s (bb_xr_mirror_ui.txt)", on ? "on" : "off");
                }
            }
        }
        const bool draw = g->game_ui->Build();
        if (!draw || !g->visible || !views.next_valid || !g->ui_layer) {
            std::scoped_lock lock{g->state_mutex};
            g->ui_shown = false;
            g->mirror_ready = -1;
            continue;
        }
        const uint32_t width =
            std::max(256u, uint32_t(float(g->eye_config[0].recommendedImageRectWidth) * g->ui_scale));
        const uint32_t height = std::max(
            256u, uint32_t(float(g->eye_config[0].recommendedImageRectHeight) * g->ui_scale));
        VkImage images[2] = {};
        VkFormat format = VK_FORMAT_UNDEFINED;
        {
            std::scoped_lock lock{g->state_mutex};
            if (!PrepareUiChains(width, height)) {
                g->ui_layer = false;
                Log(1, "VR UI: no swapchain: the menus are not shown");
                continue;
            }
            bool ready = true;
            for (Chain& chain : g->ui_chain) {
                if (!chain.writing) {
                    ready = BeginWrite(chain, 20'000'000) && ready; // 20 ms
                }
            }
            if (!ready) {
                continue; // an acquired image stays acquired: the next frame picks it up
            }
            for (int eye = 0; eye < 2; ++eye) {
                images[eye] = g->ui_chain[eye].images[g->ui_chain[eye].index].image;
            }
            format = VkFormat(g->ui_chain[0].format);
        }
        m3::Mat4 view_proj[2];
        for (int eye = 0; eye < 2; ++eye) {
            const XrView& v = views.next_views[eye];
            view_proj[eye] =
                m3::ProjectionFov(v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp,
                                  v.fov.angleDown, 0.05f, 0.0f) *
                m3::Inverse(m3::FromPose({v.pose.orientation.x, v.pose.orientation.y,
                                          v.pose.orientation.z, v.pose.orientation.w},
                                         {v.pose.position.x, v.pose.position.y,
                                          v.pose.position.z}));
        }
        // The desktop mirror: the same UI from the view the latest flat image was rendered with
        // (mono only: alternate-eye frames have no single view).
        vrui::MirrorTarget mirror_target{};
        int mirror_slot = -1;
        BbXrMirrorUi mirror_info{};
        {
            BbXrFlatView fv{};
            bool fv_ok = false;
            {
                std::scoped_lock lock{g->state_mutex};
                fv_ok = g->mirror_on && g->mono && g->flat_view_valid && g->flat.released &&
                        std::chrono::steady_clock::now() - g->flat_view_at <
                            std::chrono::milliseconds(500);
                fv = g->flat_view;
            }
            if (fv_ok) {
                const vrmirror::Fov fov{fv.fov.left, fv.fov.right, fv.fov.up, fv.fov.down};
                constexpr uint32_t kMirrorMaxW = 1920, kMirrorMaxH = 1080;
                uint32_t mw = 0, mh = 0;
                vrmirror::ImageSize(fov, kMirrorMaxW, kMirrorMaxH, &mw, &mh);
                if (g->ui_gpu.EnsureMirror(format, kMirrorMaxW, kMirrorMaxH)) {
                    mirror_slot = g->mirror_next;
                    g->mirror_next = (g->mirror_next + 1) % 3;
                    mirror_target.image = g->ui_gpu.MirrorImage(mirror_slot);
                    mirror_target.width = mw;
                    mirror_target.height = mh;
                    mirror_target.view_proj = vrmirror::ViewProj(
                        {fv.head.orientation[0], fv.head.orientation[1], fv.head.orientation[2],
                         fv.head.orientation[3]},
                        {fv.head.position[0], fv.head.position[1], fv.head.position[2]}, fov);
                    mirror_info.image = mirror_target.image;
                    mirror_info.view_format = UnormOf(format);
                    mirror_info.width = kMirrorMaxW;
                    mirror_info.height = kMirrorMaxH;
                    mirror_info.uv_w = float(mw) / float(kMirrorMaxW);
                    mirror_info.uv_h = float(mh) / float(kMirrorMaxH);
                } else {
                    Log(1, "VR UI mirror: %s: off", g->ui_gpu.LastError().c_str());
                    g->mirror_on = false;
                }
            }
            if (mirror_slot < 0) {
                std::scoped_lock lock{g->state_mutex};
                g->mirror_ready = -1;
            }
        }
        const bool rendered =
            g->ui_gpu.Render(*g->game_ui, images, format, width, height, view_proj,
                             mirror_slot >= 0 ? &mirror_target : nullptr);
        if (!rendered && (++failures <= 5 || failures % 1000 == 0)) {
            Log(2, "VR UI: %s (%u times)", g->ui_gpu.LastError().c_str(), failures);
        }
        std::scoped_lock lock{g->state_mutex};
        bool released = true;
        for (Chain& chain : g->ui_chain) {
            EndWrite(chain); // released even when nothing was drawn (the spec wants it)
            released = released && chain.released;
        }
        if (rendered && mirror_slot >= 0) {
            mirror_info.seq = ++g->mirror_seq;
            g->mirror_pending = mirror_slot;
            g->mirror_pending_info = mirror_info;
            if (!g->mirror_logged) {
                g->mirror_logged = true;
                Log(0, "VR UI mirror: first image for the desktop window (%.0f x %.0f px)",
                    mirror_info.uv_w * float(mirror_info.width),
                    mirror_info.uv_h * float(mirror_info.height));
            }
        }
        if (rendered && released) {
            g->ui_views[0] = views.next_views[0];
            g->ui_views[1] = views.next_views[1];
            g->ui_shown = true;
            g->ui_released_at = std::chrono::steady_clock::now();
        }
    }
}

/// BB_VR_UI=1: the game's VR UI, its GPU side on the game's device, and its thread.
void StartGameUi() {
    if (!EnvFlag("BB_VR_UI", false)) {
        return;
    }
    if (!g->stereo) {
        Log(1, "VR UI (BB_VR_UI=1): needs the stereo views: off");
        return;
    }
    g->ui_scale = std::clamp(EnvFloat("BB_VR_UI_SCALE", 0.5f), 0.2f, 1.0f);
    // The menus in the desktop window, for recordings: on by default in mono mode.
    g->mirror_on = EnvFlag("BB_XR_MIRROR_UI", g->mono);
    sb::Renderer::Device device;
    device.instance = g->vk.instance;
    device.physical = g->vk.physical_device;
    device.device = g->vk.device;
    device.family = g->vk.queue_family_index;
    vkGetDeviceQueue(device.device, device.family, g->vk.queue_index, &device.queue);
    device.lock = g->vk.queue_lock;
    device.lock_user = g->vk.queue_lock_user;
    const std::string assets = FindAssets();
    std::string log, error;
    auto* ui = new vrui::GameUi;
    const bool ok = ui->Load(assets, &device, log) && g->ui_gpu.Create(device, error);
    for (size_t start = 0; start < log.size();) {
        size_t end = log.find('\n', start);
        end = end == std::string::npos ? log.size() : end;
        if (end > start) {
            Log(0, "VR UI: %s", log.substr(start, end - start).c_str());
        }
        start = end + 1;
    }
    if (!ok) {
        Log(1, "VR UI (BB_VR_UI=1): could not start (%s, assets %s): off", error.c_str(),
            assets.c_str());
        g->ui_gpu.Destroy();
        delete ui;
        return;
    }
    ui->SetRecenter(XrInput::RequestRecenter);
    g->game_ui = ui;
    g->ui_thread = std::thread(UiLoop);
    Log(0, "VR UI on (BB_VR_UI=1): menus and swing HUD from %s/ui_layouts.json (\"game\"), "
           "layer at %.0f%% of the eye size (BB_VR_UI_SCALE)",
        assets.c_str(), g->ui_scale * 100.0f);
}

int StartSession(const BbXrSessionInfo* info) {
    if (!g || g->system == XR_NULL_SYSTEM_ID || g->session) {
        return 0;
    }
    g->vk = *info;
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (!Check(g->get_graphics_requirements(g->instance, g->system, &requirements),
               "xrGetVulkanGraphicsRequirementsKHR")) {
        return 0;
    }
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = info->instance;
    binding.physicalDevice = info->physical_device;
    binding.device = info->device;
    binding.queueFamilyIndex = info->queue_family_index;
    binding.queueIndex = info->queue_index;
    XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
    create.next = &binding;
    create.systemId = g->system;
    if (!Check(xrCreateSession(g->instance, &create, &g->session), "xrCreateSession")) {
        return 0;
    }
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    uint32_t space_count = 0;
    xrEnumerateReferenceSpaces(g->session, 0, &space_count, nullptr);
    std::vector<XrReferenceSpaceType> space_types(space_count);
    xrEnumerateReferenceSpaces(g->session, space_count, &space_count, space_types.data());
    bool has_stage = false;
    for (XrReferenceSpaceType type : space_types) {
        Log(0, "reference space type %d supported", int(type));
        has_stage |= type == XR_REFERENCE_SPACE_TYPE_STAGE;
    }
    // BB_XR_STAGE=1 anchors the world screen in STAGE instead of LOCAL (if LOCAL drifts).
    const bool use_stage = EnvFlag("BB_XR_STAGE", false) && has_stage && !g->head_locked;
    space.referenceSpaceType = g->head_locked ? XR_REFERENCE_SPACE_TYPE_VIEW
                               : use_stage    ? XR_REFERENCE_SPACE_TYPE_STAGE
                                              : XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(xrCreateReferenceSpace(g->session, &space, &g->space), "xrCreateReferenceSpace")) {
        return 0;
    }
    if (use_stage) {
        Log(0, "screen anchored in STAGE space");
    }
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    Check(xrCreateReferenceSpace(g->session, &space, &g->local), "xrCreateReferenceSpace");
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    Check(xrCreateReferenceSpace(g->session, &space, &g->view), "xrCreateReferenceSpace");
    g->world = g->head_locked ? g->local : g->space;
    g->space_is_local = !g->head_locked && !use_stage;
    {
        std::scoped_lock lock{g_views_mutex};
        g_published.api.space = use_stage ? 1u : 0u;
    }
    XrInput::Create(g->instance, g->session, Log);

    g->screen_width = EnvFloat("BB_XR_SCREEN_M", 4.16f);
    g->screen_distance = EnvFloat("BB_XR_SCREEN_DIST", 3.38f);
    if (g->cylinder) {
        Log(0, "screen curved: cylinder radius %.2f m, arc %.0f deg, %.2f m wide (BB_XR_CURVE=0 "
               "for flat)",
            g->screen_distance,
            xrscreen::ArcAngle(g->screen_width, g->screen_distance) * 57.29578f,
            g->screen_width);
    } else {
        Log(0, "screen flat: quad %.2f m wide at %.2f m (%s)", g->screen_width,
            g->screen_distance, g->flat_reason);
    }
    {
        Filler filler;
        if (!filler.Ready()) {
            Log(1, "no Vulkan access for the bridge: no hand markers, no background layer");
        }
        g->show_hands = EnvFlag("BB_XR_HANDS", true) && CreateMarkers(filler);
        g->background = CreateBackground(filler);
        if (EnvFlag("BB_XR_LASER", false)) {
            g->laser = CreateLaser(filler);
            Log(g->laser ? 0 : 1, "laser pointer %s (BB_XR_LASER=1): right controller beam and "
                                  "hit dot",
                g->laser ? "on" : "unavailable");
        }
    }
    const char* mode = std::getenv("BB_XR_MODE");
    g->mono = mode && std::strcmp(mode, "mono") == 0;
    g->alt = mode && std::strcmp(mode, "alt") == 0;
    if ((g->mono || g->alt) && !g->stereo) {
        Log(1, "BB_XR_MODE=%s needs the stereo views: cinema screen instead", mode);
        g->mono = g->alt = false;
    } else if (g->mono) {
        Log(0, "mode mono (BB_XR_MODE=mono, experimental): the game image is both eyes' "
               "projection layer; it swims with the head until the game camera follows it");
    } else if (g->alt) {
        Log(0, "mode alt (BB_XR_MODE=alt, experimental): alternate-eye stereo, one eye per game "
               "frame (AcquireEye/ReleaseEye), each eye shown with the view it was rendered with; "
               "menus, title and loading screens on the flat cinema screen (same image in both eyes)");
    }
    g->hud_config = xrhud::FromEnvironment();
    g->hud = g->hud_config.mode != xrhud::Mode::Off;
    g->hud_alpha = xrhud::ParseAlpha(std::getenv("BB_XR_HUD_ALPHA"));
    if (g->hud) {
        const xrhud::Quad quad = xrhud::HeadQuad(g->hud_config, 16.0f / 9.0f);
        Log(0, "HUD layer on (BB_XR_HUD=%s): the game's UI as a quad %.2f x %.2f m at %.2f m, "
               "%.0f deg wide, center %+.0f deg, %s (BB_XR_HUD_DIST, _WIDTH_DEG, _PITCH_DEG, "
               "_FOLLOW=lock|lazy; BB_XR_HUD=off keeps it in the game image)",
            xrhud::ModeName(g->hud_config.mode), quad.width, quad.height,
            g->hud_config.distance, g->hud_config.width_deg, g->hud_config.pitch_deg,
            g->hud_config.follow == xrhud::Follow::Lazy ? "lazy follow" : "head-locked");
    } else {
        Log(0, "HUD layer off (BB_XR_HUD=off, or cinema mode): the HUD stays in the game image");
    }
    g->running = true;
    g->loop = std::thread(FrameLoop);
    StartGameUi();
    Log(0, "session started (%s screen, %s mode)", g->head_locked ? "head-locked" : "world",
        g->alt ? "alt" : g->mono ? "mono" : "cinema");
    return 1;
}

bool CreateFlat(uint32_t width, uint32_t height, VkFormat source) {
    DestroyChain(g->flat);
    g->flat_views_valid = false;
    g->flat_view_valid = false;
    if (!CreateChain(g->flat, width, height, PickFormat(source), "xrCreateSwapchain")) {
        DestroyChain(g->flat);
        return false;
    }
    Log(0, "flat screen %ux%u format %lld (source %d)", width, height, (long long)g->flat.format,
        int(source));
    return true;
}

int AcquireFlat(uint32_t width, uint32_t height, VkFormat source, VkImage* image,
                VkFormat* format) {
    if (!g || !g->session || !g->visible || width == 0 || height == 0) {
        return 0;
    }
    std::scoped_lock lock{g->state_mutex};
    if (g->flat.writing) {
        return 0;
    }
    if (!g->flat.handle || g->flat.width != width || g->flat.height != height) {
        if (!CreateFlat(width, height, source)) {
            return 0;
        }
    }
    if (!BeginWrite(g->flat, 100'000'000)) { // 100 ms
        return 0;
    }
    *image = g->flat.images[g->flat.index].image;
    *format = VkFormat(g->flat.format);
    return 1;
}

/// ReleaseFlat, with the view the image was rendered with (ABI 5) or null.
void ReleaseFlatWith(const BbXrFlatView* view) {
    if (!g) {
        return;
    }
    std::scoped_lock lock{g->state_mutex};
    if (!g->flat.writing) {
        return;
    }
    const bool first = !g->flat.released;
    EndWrite(g->flat);
    if (first && g->flat.released) {
        Log(0, "first game frame on the headset screen");
    }
    if (g->alt && g->flat.released) {
        // Alternate eyes: a mono image (menu, loading screen) is newer than both eyes' images.
        g->alt_eyes.ReleaseFlat(Steady(std::chrono::steady_clock::now()));
    }
    // Mono mode shows the image with the view it was rendered with (ReleaseFlatView), else at
    // the views current now. The eye offsets come from the views current now either way.
    const bool rendered_view =
        view && view->head.valid &&
        IsUnit({view->head.orientation[0], view->head.orientation[1], view->head.orientation[2],
                view->head.orientation[3]}) &&
        view->fov.left < 0.0f && view->fov.right > 0.0f && view->fov.up > 0.0f &&
        view->fov.down < 0.0f && view->fov.right < 1.5f && view->fov.up < 1.5f &&
        view->fov.left > -1.5f && view->fov.down > -1.5f;
    if (rendered_view && !g->flat_view_valid) {
        Log(0, "mono: images come with the view they were rendered with (fov %.1f x %.1f deg)",
            (view->fov.right - view->fov.left) * 57.29578f,
            (view->fov.up - view->fov.down) * 57.29578f);
    }
    g->flat_view_valid = rendered_view;
    if (rendered_view) {
        g->flat_view = *view;
        g->flat_view_at = std::chrono::steady_clock::now();
    }
    std::scoped_lock views_lock{g_views_mutex};
    g->flat_views[0] = g_published.views[0];
    g->flat_views[1] = g_published.views[1];
    g->flat_views_valid = g_published.views_valid;
}

void ReleaseFlat() {
    ReleaseFlatWith(nullptr);
}

void ReleaseFlatView(const BbXrFlatView* view) {
    ReleaseFlatWith(view);
}

// ---- App-rendered stereo (ABI 4) --------------------------------------------------------------

int64_t PickDepthFormat() {
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(g->session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(g->session, count, &count, formats.data());
    for (const VkFormat want : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
                                VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM}) {
        for (int64_t format : formats) {
            if (format == want) {
                return format;
            }
        }
    }
    return 0;
}

void DestroyStereo() {
    for (int eye = 0; eye < 2; ++eye) {
        DestroyChain(g->stereo_color[eye]);
        DestroyChain(g->stereo_depth[eye]);
    }
    g->stereo_shown = g->stereo_depth_shown = g->stereo_writing = false;
}

/// The per-eye chains for a request (recreated when the size or format changes). state_mutex held.
bool PrepareStereo(uint32_t width, uint32_t height, VkFormat source, bool want_depth) {
    const int64_t color_format = PickFormat(source);
    const bool depth = want_depth && g->depth_layers;
    const bool same = g->stereo_color[0].handle && g->stereo_color[0].width == width &&
                      g->stereo_color[0].height == height &&
                      g->stereo_color[0].format == color_format &&
                      (g->stereo_depth[0].handle != XR_NULL_HANDLE) == depth;
    if (same) {
        return true;
    }
    DestroyStereo();
    bool ok = true;
    for (int eye = 0; eye < 2 && ok; ++eye) {
        ok = CreateChain(g->stereo_color[eye], width, height, color_format, "stereo swapchain",
                         XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                             XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                             XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
    }
    int64_t depth_format = 0;
    if (ok && depth) {
        depth_format = PickDepthFormat();
        for (int eye = 0; eye < 2 && depth_format; ++eye) {
            if (!CreateChain(g->stereo_depth[eye], width, height, depth_format,
                             "stereo depth swapchain",
                             XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
                depth_format = 0;
            }
        }
        if (!depth_format) {
            for (Chain& chain : g->stereo_depth) {
                DestroyChain(chain);
            }
            Log(1, "no depth swapchain: stereo images without depth (the host uses its own)");
        }
    }
    if (!ok) {
        DestroyStereo();
        return false;
    }
    Log(0, "stereo swapchains %ux%u per eye, color format %lld, depth %s", width, height,
        (long long)color_format,
        depth_format ? std::to_string(depth_format).c_str()
                     : (depth ? "none (format)" : "off (runtime or request)"));
    return true;
}

int AcquireStereo(const BbXrStereoRequest* request, BbXrStereoFrame* frame) {
    if (!g || !g->session || !g->stereo || !request || !frame) {
        return 0;
    }
    // Pace on the frame loop: wait for a headset frame newer than the last one rendered for.
    Published views;
    {
        std::unique_lock lock{g_views_mutex};
        const auto newer = [] { return g_published.frame != g->stereo_frame; };
        if (!newer()) {
            if (request->timeout_ms == 0 ||
                !g_frame_cv.wait_for(lock, std::chrono::milliseconds(request->timeout_ms), newer)) {
                return 0;
            }
        }
        views = g_published;
    }
    if (!g->visible || !views.next_valid) {
        return 0;
    }
    const uint32_t width = request->width ? request->width
                                          : g->eye_config[0].recommendedImageRectWidth;
    const uint32_t height = request->height ? request->height
                                            : g->eye_config[0].recommendedImageRectHeight;
    std::scoped_lock lock{g->state_mutex};
    if (!g->stereo_writing) {
        if (!PrepareStereo(width, height, request->color_format, request->want_depth != 0)) {
            return 0;
        }
    }
    // Every chain must be writable; an image that is ready stays acquired until the others are
    // (BeginWrite keeps a timed-out image waiting), so a later call picks up where this left.
    bool ready = true;
    for (int eye = 0; eye < 2; ++eye) {
        Chain* chains[2] = {&g->stereo_color[eye], &g->stereo_depth[eye]};
        for (Chain* chain : chains) {
            if (chain->handle && !chain->writing) {
                ready = BeginWrite(*chain, 50'000'000) && ready; // 50 ms
            }
        }
    }
    if (!ready) {
        return 0;
    }
    g->stereo_writing = true;
    g->stereo_with_depth = g->stereo_depth[0].handle != XR_NULL_HANDLE;
    g->stereo_frame = views.frame;
    *frame = {};
    frame->display_time = views.next_time;
    frame->flags = views.next_flags;
    frame->space = views.api.space;
    for (int eye = 0; eye < 2; ++eye) {
        g->stereo_acquired[eye] = views.next_views[eye];
        const XrView& v = views.next_views[eye];
        frame->eye[eye].pose = ToApi(v.pose, true);
        frame->eye[eye].fov = {v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown};
        frame->eye[eye].width = width;
        frame->eye[eye].height = height;
        frame->grip[eye] = views.next_grip[eye]; // eye index = hand index (0 left, 1 right)
        frame->aim[eye] = views.next_aim[eye];
        frame->trigger[eye] = views.trigger[eye];
        frame->squeeze[eye] = views.squeeze[eye];
        const Chain& color = g->stereo_color[eye];
        frame->color[eye] = color.images[color.index].image;
        const Chain& depth = g->stereo_depth[eye];
        frame->depth[eye] = depth.handle ? depth.images[depth.index].image : VK_NULL_HANDLE;
    }
    frame->color_format = VkFormat(g->stereo_color[0].format);
    frame->depth_format = g->stereo_with_depth ? VkFormat(g->stereo_depth[0].format)
                                               : VK_FORMAT_UNDEFINED;
    frame->width = width;
    frame->height = height;
    frame->depth_to_compositor = g->stereo_with_depth ? 1u : 0u;
    return 1;
}

void ReleaseStereo(const BbXrStereoSubmit* submit) {
    if (!g) {
        return;
    }
    std::scoped_lock lock{g->state_mutex};
    if (!g->stereo_writing) {
        return;
    }
    bool released = true;
    for (int eye = 0; eye < 2; ++eye) {
        EndWrite(g->stereo_color[eye]);
        released = released && g->stereo_color[eye].released;
        if (g->stereo_depth[eye].handle) {
            EndWrite(g->stereo_depth[eye]);
        }
    }
    g->stereo_writing = false;
    g->stereo_views[0] = g->stereo_acquired[0];
    g->stereo_views[1] = g->stereo_acquired[1];
    if (submit && submit->near_z > 0.0f) {
        g->stereo_near = submit->near_z;
        g->stereo_far = submit->far_z > submit->near_z ? submit->far_z : submit->near_z * 1e4f;
    }
    g->stereo_shown = released;
    g->stereo_depth_shown = released && g->stereo_with_depth;
    g->stereo_released_at = std::chrono::steady_clock::now();
}

// ---- Alternate-eye stereo (ABI 6) -------------------------------------------------------------

int AcquireEye(uint32_t eye, uint32_t width, uint32_t height, VkFormat source, VkImage* image,
               VkFormat* format) {
    if (!g || !g->session) {
        return 0;
    }
    if (!g->alt) {
        return -1; // not BB_XR_MODE=alt, or the runtime refused the layer: send flat images
    }
    if (!g->visible || eye > 1 || width == 0 || height == 0) {
        return 0;
    }
    std::scoped_lock lock{g->state_mutex};
    Chain& chain = g->alt_chain[eye];
    if (chain.writing) {
        return 0;
    }
    if (!chain.handle || chain.width != width || chain.height != height) {
        DestroyChain(chain);
        if (!CreateChain(chain, width, height, PickFormat(source), "eye swapchain")) {
            DestroyChain(chain);
            return 0;
        }
        g->alt_eyes.valid[eye] = false; // a new chain has no image to show yet
        Log(0, "%s eye images %ux%u format %lld (source %d)", eye ? "right" : "left", width,
            height, (long long)chain.format, int(source));
    }
    if (!BeginWrite(chain, 50'000'000)) { // 50 ms
        return 0;
    }
    *image = chain.images[chain.index].image;
    *format = VkFormat(chain.format);
    return 1;
}

void ReleaseEye(uint32_t eye, const BbXrEyeView* view) {
    if (!g || eye > 1) {
        return;
    }
    std::scoped_lock lock{g->state_mutex};
    Chain& chain = g->alt_chain[eye];
    if (!chain.writing) {
        return;
    }
    EndWrite(chain);
    if (!chain.released) {
        return;
    }
    const bool rendered_view =
        view && view->pose.valid &&
        IsUnit({view->pose.orientation[0], view->pose.orientation[1], view->pose.orientation[2],
                view->pose.orientation[3]}) &&
        std::isfinite(view->pose.position[0]) && std::isfinite(view->pose.position[1]) &&
        std::isfinite(view->pose.position[2]) && view->fov.left < 0.0f && view->fov.right > 0.0f &&
        view->fov.up > 0.0f && view->fov.down < 0.0f && view->fov.right < 1.5f &&
        view->fov.up < 1.5f && view->fov.left > -1.5f && view->fov.down > -1.5f;
    if (rendered_view) {
        g->alt_pose[eye] = {{view->pose.orientation[0], view->pose.orientation[1],
                             view->pose.orientation[2], view->pose.orientation[3]},
                            {view->pose.position[0], view->pose.position[1],
                             view->pose.position[2]}};
        g->alt_fov[eye] = {view->fov.left, view->fov.right, view->fov.up, view->fov.down};
        g->alt_seq[eye] = view->seq;
    } else {
        // No view: the eye's views current now (the image swims, but stays in its eye).
        std::scoped_lock views_lock{g_views_mutex};
        if (!g_published.views_valid) {
            return;
        }
        g->alt_pose[eye] = g_published.views[eye].pose;
        g->alt_fov[eye] = g_published.views[eye].fov;
        if (!g->alt_view_warned) {
            g->alt_view_warned = true;
            Log(1, "alt: an eye image came without the view it was rendered with: shown with the "
                   "views current at release");
        }
    }
    if (g->alt_eyes.released[0] + g->alt_eyes.released[1] == 0) {
        Log(0, "alt: first eye image (%s, seq %llu, fov %.1f x %.1f deg)", eye ? "right" : "left",
            static_cast<unsigned long long>(g->alt_seq[eye]),
            (g->alt_fov[eye].angleRight - g->alt_fov[eye].angleLeft) * 57.29578f,
            (g->alt_fov[eye].angleUp - g->alt_fov[eye].angleDown) * 57.29578f);
    }
    g->alt_eyes.ReleaseEye(int(eye), Steady(std::chrono::steady_clock::now()));
}

// ---- HUD layer (ABI 7) -----------------------------------------------------------------------

int AcquireHud(uint32_t width, uint32_t height, VkFormat source, VkImage* image,
               VkFormat* format) {
    if (!g || !g->session) {
        return 0;
    }
    std::scoped_lock lock{g->state_mutex};
    if (!g->hud) {
        return -1; // BB_XR_HUD=off, cinema mode, or the runtime refused the layer
    }
    if (!g->visible || width == 0 || height == 0 || g->hud_chain.writing) {
        return 0;
    }
    if (!g->hud_chain.handle || g->hud_chain.width != width || g->hud_chain.height != height) {
        DestroyChain(g->hud_chain);
        if (!CreateChain(g->hud_chain, width, height, PickFormat(source), "HUD swapchain")) {
            DestroyChain(g->hud_chain);
            return 0;
        }
        Log(0, "HUD images %ux%u format %lld (source %d), blend %s", width, height,
            (long long)g->hud_chain.format, int(source), xrhud::AlphaName(g->hud_alpha));
        if (std::getenv("BB_XR_HUD_DEBUG")) {
            uint32_t count = 0;
            xrEnumerateSwapchainFormats(g->session, 0, &count, nullptr);
            std::vector<int64_t> formats(count);
            xrEnumerateSwapchainFormats(g->session, count, &count, formats.data());
            std::string list;
            for (int64_t f : formats) {
                list += std::to_string((long long)f) + " ";
            }
            Log(0, "HUD debug: runtime swapchain formats: %s", list.c_str());
        }
    }
    if (!BeginWrite(g->hud_chain, 20'000'000)) { // 20 ms: a skipped HUD frame is harmless
        return 0;
    }
    *image = g->hud_chain.images[g->hud_chain.index].image;
    *format = VkFormat(g->hud_chain.format);
    return 1;
}

void ReleaseHud() {
    if (!g) {
        return;
    }
    std::scoped_lock lock{g->state_mutex};
    if (!g->hud_chain.writing) {
        return;
    }
    EndWrite(g->hud_chain);
    if (g->hud_chain.released) {
        g->hud_shown = true;
        g->hud_released_at = std::chrono::steady_clock::now();
    }
}

void HideHud() {
    if (!g) {
        return;
    }
    std::scoped_lock lock{g->state_mutex};
    g->hud_shown = false;
}

void Shutdown() {
    if (!g) {
        return;
    }
    Log(0, "shutdown: stopping the frame loop");
    g->running = false;
    g_frame_cv.notify_all();
    if (g->loop.joinable()) {
        g->loop.join();
    }
    if (g->ui_thread.joinable()) {
        g->ui_thread.join();
    }
    // The GPU must be done with every swapchain image before the runtime frees them.
    if (g->gipa && g->vk.device) {
        const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            g->gipa(g->vk.instance, "vkGetDeviceProcAddr"));
        const auto wait_idle = gdpa ? reinterpret_cast<PFN_vkDeviceWaitIdle>(
                                          gdpa(g->vk.device, "vkDeviceWaitIdle"))
                                    : nullptr;
        if (wait_idle) {
            Log(0, "shutdown: waiting for the GPU to go idle");
            wait_idle(g->vk.device);
        }
    }
    if (g->session && g->session_running) {
        // Orderly end: ask the runtime to stop, then take the STOPPING event (xrEndSession).
        Log(0, "shutdown: xrRequestExitSession");
        Check(xrRequestExitSession(g->session), "xrRequestExitSession");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        while (g->session_running && std::chrono::steady_clock::now() < deadline) {
            HandleEvents();
            if (g->session_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (g->session_running) {
            Log(1, "shutdown: no STOPPING event in time, xrEndSession anyway");
            g->session_running = false;
            Check(xrEndSession(g->session), "xrEndSession");
        } else {
            Log(0, "shutdown: session ended");
        }
    }
    if (g->game_ui) {
        g->ui_gpu.Destroy(); // waits for its last submission
        g->game_ui->Destroy();
        delete g->game_ui;
        g->game_ui = nullptr;
    }
    Log(0, "shutdown: destroying swapchains");
    {
        DestroyChain(g->ui_chain[0]);
        DestroyChain(g->ui_chain[1]);
        std::scoped_lock lock{g->state_mutex};
        DestroyChain(g->flat);
        DestroyStereo();
        DestroyChain(g->alt_chain[0]);
        DestroyChain(g->alt_chain[1]);
        DestroyChain(g->hud_chain);
    }
    for (Chain& chain : g->background_eyes) {
        DestroyChain(chain);
    }
    for (Chain& chain : g->markers) {
        DestroyChain(chain);
    }
    for (Chain& chain : g->lasers) {
        DestroyChain(chain);
    }
    XrInput::Destroy();
    for (XrSpace space : {g->space, g->local, g->view}) {
        if (space) {
            xrDestroySpace(space);
        }
    }
    if (g->session) {
        Log(0, "shutdown: xrDestroySession");
        xrDestroySession(g->session);
    }
    if (g->instance) {
        Log(0, "shutdown: xrDestroyInstance");
        xrDestroyInstance(g->instance);
    }
    Log(0, "shutdown: done");
    {
        std::scoped_lock lock{g_views_mutex};
        g_published = {};
    }
    delete g;
    g = nullptr;
}

int GetInput(BbXrInput* out) {
    const int active = XrInput::Get(out);
    if (g && g->game_ui) {
        float trigger[2]{}, grip[2]{};
        XrInput::Analog(trigger, grip);
        g->game_ui->Filter().Apply(SteadyNow(), *out, trigger[0] > 0.6f || trigger[1] > 0.6f);
    }
    return active;
}

void SetGameState(const BbXrGameState* state) {
    if (g && g->game_ui && state) {
        g->game_ui->SetGameState(*state, SteadyNow());
    }
}

void GameEvent(const BbXrGameEvent* event) {
    if (g && g->game_ui && event) {
        g->game_ui->PushEvent(*event, SteadyNow());
    }
}

uint32_t TakeUiActions(BbXrUiAction* out, uint32_t capacity) {
    return g && g->game_ui && out ? g->game_ui->TakeActions(out, capacity) : 0;
}

int GetMirrorUi(BbXrMirrorUi* out) {
    if (!g || !out) {
        return 0;
    }
    std::scoped_lock lock{g->state_mutex};
    if (g->mirror_ready < 0 || !g->mirror_ready_info.image ||
        std::chrono::steady_clock::now() - g->mirror_ready_at > std::chrono::milliseconds(250)) {
        return 0;
    }
    *out = g->mirror_ready_info;
    return 1;
}

int GetViews(BbXrViews* out) {
    std::scoped_lock lock{g_views_mutex};
    *out = g_published.api;
    return (out->flags & BBXR_VIEW_ORIENTATION_VALID) ? 1 : 0;
}

const BbXrApi kApi = {
    BBPORT_XR_BRIDGE_ABI, Init,          InstanceExtensions, PhysicalDevice, DeviceExtensions,
    StartSession,         AcquireFlat,   ReleaseFlat,        Shutdown,       GetInput,
    XrInput::Haptic,      GetViews,      AcquireStereo,      ReleaseStereo,
    ReleaseFlatView,      AcquireEye,    ReleaseEye,         AcquireHud,     ReleaseHud,
    HideHud,              SetGameState,  GameEvent,          TakeUiActions, GetMirrorUi,
};

} // namespace

extern "C" const BbXrApi* BbXrGetApi(void) {
    return &kApi;
}
