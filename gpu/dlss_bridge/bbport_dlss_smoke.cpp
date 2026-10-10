// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Headless smoke test of bbport_dlss.dll (no game, no window): loads the bridge from the
// directory given as argv[1] (with nvngx_dlss.dll next to it), creates a Vulkan device with the
// extensions NGX asks for, creates the two VR eye features (slots 1 and 2: independent
// histories) and evaluates each a few times on synthetic inputs, then checks the output was
// written. Exit 0 = pass, 77 = skipped (no NVIDIA RTX / DLSS not available), other = failure.
// Contains no NVIDIA code: it uses only the bridge's C interface (bbport_dlss_bridge.h).
//
//   ninja -C out/gpu-dlss dlss-bridge-smoke && out/gpu-dlss/dlss-bridge-smoke.exe <dll dir>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "bbport_dlss_bridge.h"

namespace {

#define CHECK_VK(call)                                                                             \
    do {                                                                                           \
        const VkResult result_ = (call);                                                           \
        if (result_ != VK_SUCCESS) {                                                               \
            std::printf("FAIL: %s = %d\n", #call, int(result_));                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

void Log(int warning, const char* message) {
    std::printf("  bridge: %s%s\n", warning ? "warning: " : "", message);
}

struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkFormat format{};
    VkImageAspectFlags aspect{};
    uint32_t width{}, height{};
};

uint32_t MemoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    std::printf("FAIL: no memory type\n");
    std::exit(1);
}

Image MakeImage(VkDevice device, VkPhysicalDevice physical, VkFormat format, uint32_t w,
                uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Image img{};
    img.format = format;
    img.aspect = aspect;
    img.width = w;
    img.height = h;
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CHECK_VK(vkCreateImage(device, &info, nullptr, &img.image));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, img.image, &req);
    const VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex =
            MemoryType(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    CHECK_VK(vkAllocateMemory(device, &alloc, nullptr, &img.memory));
    CHECK_VK(vkBindImageMemory(device, img.image, img.memory, 0));
    const VkImageViewCreateInfo view{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    CHECK_VK(vkCreateImageView(device, &view, nullptr, &img.view));
    return img;
}

void Destroy(VkDevice device, Image& img) {
    vkDestroyImageView(device, img.view, nullptr);
    vkDestroyImage(device, img.image, nullptr);
    vkFreeMemory(device, img.memory, nullptr);
}

void ToGeneral(VkCommandBuffer cmd, const Image& img) {
    const VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img.image,
        .subresourceRange = {img.aspect, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void FullBarrier(VkCommandBuffer cmd) {
    const VkMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}

BbDlssImage Describe(const Image& img) {
    return BbDlssImage{img.image, img.view, {img.aspect, 0, 1, 0, 1}, img.format, img.width,
                       img.height};
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    std::wstring wdir(dir.begin(), dir.end());
    SetDllDirectoryW(wdir.c_str());
    HMODULE module = LoadLibraryW((wdir + L"\\bbport_dlss.dll").c_str());
    if (!module) {
        std::printf("SKIP: bbport_dlss.dll not found in %s\n", dir.c_str());
        return 77;
    }
    const auto get_api =
        reinterpret_cast<BbDlssGetApiFn>(GetProcAddress(module, "BbDlssGetApi"));
    const BbDlssApi* api = get_api ? get_api() : nullptr;
    if (!api || api->abi < 2 || !api->CreateFeatureSlot || !api->EvaluateSlot ||
        !api->ReleaseFeatureSlot) {
        std::printf("FAIL: bridge ABI %u (need 2 with feature slots)\n", api ? api->abi : 0u);
        return 1;
    }
    std::printf("bridge ABI %u\n", api->abi);
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    std::wstring data = std::wstring(temp) + L"bbport_dlss_smoke";
    CreateDirectoryW(data.c_str(), nullptr);
    if (!api->Configure(wdir.c_str(), data.c_str(), Log)) {
        std::printf("FAIL: Configure\n");
        return 1;
    }

    // Instance with NGX's extensions.
    uint32_t ext_count = 0;
    const VkExtensionProperties* exts = nullptr;
    if (!api->InstanceExtensions(&ext_count, &exts)) {
        std::printf("SKIP: NGX instance extension query failed\n");
        return 77;
    }
    std::vector<const char*> instance_exts;
    for (uint32_t i = 0; i < ext_count; ++i) {
        instance_exts.push_back(exts[i].extensionName);
    }
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .pApplicationName = "bbport_dlss_smoke",
                                .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = uint32_t(instance_exts.size()),
        .ppEnabledExtensionNames = instance_exts.data(),
    };
    VkInstance instance{};
    CHECK_VK(vkCreateInstance(&instance_info, nullptr, &instance));

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    VkPhysicalDevice physical{};
    for (auto d : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.vendorID == 0x10de) {
            physical = d;
            std::printf("GPU: %s\n", props.deviceName);
            break;
        }
    }
    if (!physical) {
        std::printf("SKIP: no NVIDIA GPU\n");
        return 77;
    }
    if (!api->DeviceExtensions(instance, physical, &ext_count, &exts)) {
        std::printf("SKIP: DLSS not supported on this GPU/driver\n");
        return 77;
    }
    std::vector<const char*> device_exts;
    for (uint32_t i = 0; i < ext_count; ++i) {
        device_exts.push_back(exts[i].extensionName);
    }
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t family = 0;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family = i;
            break;
        }
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                             .queueFamilyIndex = family,
                                             .queueCount = 1,
                                             .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .bufferDeviceAddress = VK_TRUE,
    };
    const VkDeviceCreateInfo device_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features12,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = uint32_t(device_exts.size()),
        .ppEnabledExtensionNames = device_exts.data(),
    };
    VkDevice device{};
    CHECK_VK(vkCreateDevice(physical, &device_info, nullptr, &device));
    VkQueue queue{};
    vkGetDeviceQueue(device, family, 0, &queue);
    if (!api->Initialize(instance, physical, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr)) {
        std::printf("SKIP: NGX initialization / DLSS availability failed\n");
        return 77;
    }

    const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .flags =
                                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                            .queueFamilyIndex = family};
    VkCommandPool pool{};
    CHECK_VK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    const VkCommandBufferAllocateInfo cmd_info{.sType =
                                                   VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                               .commandPool = pool,
                                               .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               .commandBufferCount = 1};
    VkCommandBuffer cmd{};
    CHECK_VK(vkAllocateCommandBuffers(device, &cmd_info, &cmd));
    const auto begin = [&] {
        const VkCommandBufferBeginInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags =
                                                VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK_VK(vkBeginCommandBuffer(cmd, &info));
    };
    const auto submit = [&] {
        CHECK_VK(vkEndCommandBuffer(cmd));
        const VkSubmitInfo info{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                .commandBufferCount = 1,
                                .pCommandBuffers = &cmd};
        CHECK_VK(vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE));
        CHECK_VK(vkQueueWaitIdle(queue));
    };

    // Two eye features: 960x540 -> 1920x1080, Performance, preset default.
    const uint32_t w = 960, h = 540, ow = 1920, oh = 1080;
    const BbDlssFeature feature{w, h, ow, oh, 3, 0, 0, 0};
    begin();
    const int created_l = api->CreateFeatureSlot(1, cmd, &feature);
    const int created_r = api->CreateFeatureSlot(2, cmd, &feature);
    submit();
    if (!created_l || !created_r) {
        std::printf("FAIL: feature creation (L %d, R %d)\n", created_l, created_r);
        return 1;
    }

    const VkImageUsageFlags in_usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    Image color = MakeImage(device, physical, VK_FORMAT_R8G8B8A8_UNORM, w, h, in_usage,
                            VK_IMAGE_ASPECT_COLOR_BIT);
    Image depth = MakeImage(device, physical, VK_FORMAT_D32_SFLOAT, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT);
    Image motion = MakeImage(device, physical, VK_FORMAT_R16G16_SFLOAT, w, h, in_usage,
                             VK_IMAGE_ASPECT_COLOR_BIT);
    // ABI 3: the right eye also gets a bias mask (all 1: trust the current colour).
    Image bias = MakeImage(device, physical, VK_FORMAT_R8_UNORM, w, h, in_usage,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    Image outputs[2] = {
        MakeImage(device, physical, VK_FORMAT_R8G8B8A8_UNORM, ow, oh,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT),
        MakeImage(device, physical, VK_FORMAT_R8G8B8A8_UNORM, ow, oh,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT)};

    // Inputs: left eye grey 0.5, right eye red-ish; depth 0.5; no motion.
    for (int frame = 0; frame < 6; ++frame) {
        const int eye = frame & 1;
        begin();
        ToGeneral(cmd, color);
        ToGeneral(cmd, depth);
        ToGeneral(cmd, motion);
        ToGeneral(cmd, bias);
        ToGeneral(cmd, outputs[eye]);
        const VkClearColorValue grey{{0.5f, 0.5f, 0.5f, 1.0f}};
        const VkClearColorValue red{{0.8f, 0.2f, 0.1f, 1.0f}};
        const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkImageSubresourceRange depth_range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, color.image, VK_IMAGE_LAYOUT_GENERAL, eye ? &red : &grey, 1,
                             &color_range);
        vkCmdClearColorImage(cmd, motion.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &color_range);
        const VkClearColorValue one{{1.0f, 1.0f, 1.0f, 1.0f}};
        vkCmdClearColorImage(cmd, bias.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &color_range);
        vkCmdClearColorImage(cmd, outputs[eye].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                             &color_range);
        const VkClearDepthStencilValue half{0.5f, 0};
        vkCmdClearDepthStencilImage(cmd, depth.image, VK_IMAGE_LAYOUT_GENERAL, &half, 1,
                                    &depth_range);
        FullBarrier(cmd);
        const BbDlssEvaluate eval{Describe(color), Describe(depth), Describe(motion),
                                  Describe(outputs[eye]),
                                  frame < 2 ? 0.0f : 0.25f, frame < 2 ? 0.0f : -0.25f,
                                  frame < 2 ? 1 : 0, 27.8f, 0.0f,
                                  eye && api->abi >= 3 ? Describe(bias) : BbDlssImage{}};
        const int ok = api->EvaluateSlot(1 + eye, cmd, &eval);
        submit();
        if (!ok) {
            std::printf("FAIL: evaluation, frame %d eye %d\n", frame, eye);
            return 1;
        }
    }

    // Read back the centre of both outputs.
    const VkBufferCreateInfo buffer_info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size = 8,
                                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer{};
    CHECK_VK(vkCreateBuffer(device, &buffer_info, nullptr, &buffer));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    const VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = MemoryType(physical, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory memory{};
    CHECK_VK(vkAllocateMemory(device, &alloc, nullptr, &memory));
    CHECK_VK(vkBindBufferMemory(device, buffer, memory, 0));
    begin();
    FullBarrier(cmd);
    for (int eye = 0; eye < 2; ++eye) {
        const VkBufferImageCopy copy{.bufferOffset = VkDeviceSize(eye * 4),
                                     .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .imageOffset = {int32_t(ow / 2), int32_t(oh / 2), 0},
                                     .imageExtent = {1, 1, 1}};
        vkCmdCopyImageToBuffer(cmd, outputs[eye].image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
    }
    submit();
    unsigned char* pixels = nullptr;
    CHECK_VK(vkMapMemory(device, memory, 0, 8, 0, reinterpret_cast<void**>(&pixels)));
    std::printf("output centre: L %u %u %u, R %u %u %u\n", pixels[0], pixels[1], pixels[2],
                pixels[4], pixels[5], pixels[6]);
    const bool left_ok = pixels[0] > 90 && pixels[0] < 170 && pixels[1] > 90 && pixels[1] < 170;
    const bool right_ok = pixels[4] > 150 && pixels[5] < 110;
    vkUnmapMemory(device, memory);

    api->ReleaseFeatureSlot(1);
    api->ReleaseFeatureSlot(2);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    Destroy(device, color);
    Destroy(device, depth);
    Destroy(device, motion);
    Destroy(device, bias);
    Destroy(device, outputs[0]);
    Destroy(device, outputs[1]);
    vkDestroyCommandPool(device, pool, nullptr);
    api->Shutdown();
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    if (!left_ok || !right_ok) {
        std::printf("FAIL: the eye outputs do not match their own inputs\n");
        return 1;
    }
    std::printf("PASS: two eye features (slots 1, 2) created, evaluated, released\n");
    return 0;
}
