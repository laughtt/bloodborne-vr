// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_smoke.exe: checks bbport_xr.dll against the active OpenXR runtime without the game.
// Creates a Vulkan device the way the port does, starts the session and fills the flat screen
// with a slowly cycling color (white every fourth block of frames) for BB_XR_SMOKE_SECONDS
// (default 20). Prints the pad state, gestures and, once, the located eye views.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>
#include "bbport_xr_bridge.h"

namespace {

std::mutex queue_mutex;

void Log(int level, const char* message) {
    std::printf("XR%s: %s\n", level == 2 ? " error" : level == 1 ? " warning" : "", message);
}

void QueueLock(void*, int lock) {
    if (lock) {
        queue_mutex.lock();
    } else {
        queue_mutex.unlock();
    }
}

#define VK_CHECK(call)                                                                             \
    do {                                                                                           \
        const VkResult r_ = (call);                                                                \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::printf("%s failed: %d\n", #call, int(r_));                                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

int main() {
    const HMODULE module = LoadLibraryW(L"bbport_xr.dll");
    const auto get_api =
        module ? reinterpret_cast<BbXrGetApiFn>(GetProcAddress(module, "BbXrGetApi")) : nullptr;
    const BbXrApi* xr = get_api ? get_api() : nullptr;
    if (!xr || xr->abi != BBPORT_XR_BRIDGE_ABI) {
        std::printf("bbport_xr.dll missing or wrong ABI\n");
        return 1;
    }
    if (!xr->Init(Log)) {
        std::printf("no usable headset\n");
        return 1;
    }

    const char* names[64];
    uint32_t count = xr->InstanceExtensions(names, 64);
    std::vector<const char*> instance_extensions(names, names + count);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bbport_xr_smoke";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = uint32_t(instance_extensions.size());
    instance_info.ppEnabledExtensionNames = instance_extensions.data();
    VkInstance instance;
    VK_CHECK(vkCreateInstance(&instance_info, nullptr, &instance));

    VkPhysicalDevice physical = xr->PhysicalDevice(instance, vkGetInstanceProcAddr);
    if (!physical) {
        std::printf("runtime gave no physical device\n");
        return 1;
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::printf("GPU: %s\n", properties.deviceName);
    count = xr->DeviceExtensions(names, 64);
    std::vector<const char*> device_extensions(names, names + count);
    for (const char* name : device_extensions) {
        std::printf("  device extension %s\n", name);
    }

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t family = 0;
    while (family < family_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        ++family;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = uint32_t(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    VkDevice device;
    VK_CHECK(vkCreateDevice(physical, &device_info, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    const BbXrSessionInfo session{instance, physical, device, family, 0, QueueLock, nullptr};
    if (!xr->StartSession(&session)) {
        std::printf("session failed\n");
        return 1;
    }

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool;
    VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &alloc, &cmd));
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &fence));

    const char* seconds_env = std::getenv("BB_XR_SMOKE_SECONDS");
    const double seconds = seconds_env ? std::atof(seconds_env) : 20.0;
    const auto start = std::chrono::steady_clock::now();
    int frames = 0;
    for (;;) {
        const double t =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (t > seconds) {
            break;
        }
        // Controllers: print the pad state twice a second and every gesture as it fires.
        BbXrInput input;
        xr->GetInput(&input);
        static const char* const gesture_names[] = {"SWING -> R1", "HEAVY -> R2", "SHOOT -> L2",
                                                    "VIAL -> triangle"};
        for (int bit = 0; bit < 4; ++bit) {
            if (input.gestures & (1u << bit)) {
                std::printf("  gesture %s (hand speed L %.1f R %.1f m/s)\n", gesture_names[bit],
                            input.hand_speed[0], input.hand_speed[1]);
            }
        }
        static double last_print = -1.0;
        if (t - last_print > 0.5) {
            last_print = t;
            std::printf("pad %s buttons 0x%06x L(%3u,%3u) R(%3u,%3u) L2 %3u  hands %s%s\n",
                        input.active ? "on " : "off", input.buttons, input.lx, input.ly, input.rx,
                        input.ry, input.l2, input.grip[0].valid ? "L" : "-",
                        input.grip[1].valid ? "R" : "-");
        }
        // Head tracking (ABI 3): the eye views once, when first located.
        static bool views_printed = false;
        BbXrViews views;
        if (!views_printed && xr->GetViews(&views)) {
            views_printed = true;
            for (int eye = 0; eye < 2; ++eye) {
                const BbXrEye& e = views.eye[eye];
                constexpr float deg = 57.29578f;
                std::printf("  %s eye %ux%u, FOV left/right/up/down %.0f/%.0f/%.0f/%.0f deg, "
                            "at (%.3f %.3f %.3f)\n",
                            eye ? "right" : "left", e.width, e.height, e.fov.left * deg,
                            e.fov.right * deg, e.fov.up * deg, e.fov.down * deg,
                            e.pose.position[0], e.pose.position[1], e.pose.position[2]);
            }
        }

        VkImage image;
        VkFormat format;
        if (!xr->AcquireFlat(1920, 1080, VK_FORMAT_B8G8R8A8_UNORM, &image, &format)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        Barrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue white{{1.0f, 1.0f, 1.0f, 1.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
        // Mostly a cycling color, white for one frame block in four.
        const float h = float(t * 0.3);
        const VkClearColorValue color{{0.5f + 0.5f * std::sin(h), 0.5f + 0.5f * std::sin(h + 2.1f),
                                       0.5f + 0.5f * std::sin(h + 4.2f), 1.0f}};
        if ((frames / 45) % 4 != 0) {
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        }
        Barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        {
            std::scoped_lock lock{queue_mutex};
            vkQueueSubmit(queue, 1, &submit, fence);
        }
        xr->ReleaseFlat();
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &fence);
        ++frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(11));
    }
    std::printf("submitted %d flat frames in %.0f s\n", frames, seconds);
    xr->Shutdown();
    vkDeviceWaitIdle(device);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
