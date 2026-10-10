// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_sandbox.exe: fast-iteration VR test bed for bbport_xr.dll, no game needed. Creates a
// Vulkan device like xr_smoke, then shows a synthetic 1920x1080 "menu" (or --image shot.bmp) on
// the cinema screen, prints controller input when it changes, moves a menu highlight with the
// stick / d-pad, and draws a CPU laser from the right controller's aim ray onto the screen.
//
//   bbport_xr_sandbox.exe [--image file.bmp] [--seconds N] [--headlock] [--stick N]
//
// The bridge places the screen, hit-tests the right controller's aim ray against it in the
// screen's own space (LOCAL, STAGE or head-locked VIEW) and draws the beam and hit dot
// (BB_XR_LASER, on by default here); the sandbox reads the hit pixel from GetViews and marks
// it with a small crosshair in the image as a check of the pixel mapping. With --image the
// menu's outlines and highlights are drawn over the screenshot, so stick navigation works too.
// stdout is unbuffered.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include "bbport_xr_bridge.h"

namespace {

constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;
constexpr uint32_t kBlack = 0xFF000000u;

std::mutex queue_mutex;
std::atomic<bool> quit{false};

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

BOOL WINAPI CtrlHandler(DWORD) {
    quit = true;
    return TRUE;
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

// ---- CPU drawing (BGRA, 0xAARRGGBB, alpha always 255) ----------------------------------------

using Image = std::vector<uint32_t>;

constexpr uint32_t Rgb(uint32_t r, uint32_t g, uint32_t b) {
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void FillRect(Image& img, int x0, int y0, int w, int h, uint32_t color) {
    const int xa = std::max(x0, 0), xb = std::max(std::min(x0 + w, int(kWidth)), xa);
    const int ya = std::max(y0, 0), yb = std::min(y0 + h, int(kHeight));
    for (int y = ya; y < yb; ++y) {
        std::fill(img.begin() + y * kWidth + xa, img.begin() + y * kWidth + xb, color);
    }
}

void StrokeRect(Image& img, int x, int y, int w, int h, int t, uint32_t color) {
    FillRect(img, x, y, w, t, color);
    FillRect(img, x, y + h - t, w, t, color);
    FillRect(img, x, y, t, h, color);
    FillRect(img, x + w - t, y, t, h, color);
}

struct Glyph {
    char c;
    uint8_t col[5]; // columns, bit 0 on top
};
constexpr Glyph kFont[] = {
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}}, {'D', {0x7F, 0x41, 0x41, 0x41, 0x3E}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}}, {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}}, {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}}, {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}}, {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}}, {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}}, {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}}, {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}}, {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}}, {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}}, {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {',', {0x00, 0x50, 0x30, 0x00, 0x00}}, {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
};

void DrawText(Image& img, int x, int y, int scale, const std::string& text, uint32_t color) {
    for (char ch : text) {
        if (ch >= 'a' && ch <= 'z') {
            ch = char(ch - 'a' + 'A');
        }
        for (const Glyph& glyph : kFont) {
            if (glyph.c != ch) {
                continue;
            }
            for (int col = 0; col < 5; ++col) {
                for (int row = 0; row < 7; ++row) {
                    if (glyph.col[col] & (1 << row)) {
                        FillRect(img, x + col * scale, y + row * scale, scale, scale, color);
                    }
                }
            }
            break;
        }
        x += 6 * scale;
    }
}

// ---- Menu model -------------------------------------------------------------------------------

constexpr int kRows = 6, kCols = 4, kTileRows = 3;
const char* const kRowNames[kRows] = {"CONTINUE", "NEW GAME", "LOAD",
                                      "OPTIONS",  "NETWORK",  "QUIT"};

struct Item {
    int zone = -1; // 0 list row, 1 grid tile, -1 none
    int index = 0;
    bool operator==(const Item& o) const { return zone == o.zone && (zone < 0 || index == o.index); }
};

struct Rect {
    int x, y, w, h;
};

Rect ItemRect(const Item& item) {
    if (item.zone == 0) {
        return {160, 200 + item.index * 110, 600, 90};
    }
    const int col = item.index % kCols, row = item.index / kCols;
    return {1000 + col * 220, 200 + row * 220, 190, 190};
}

std::string ItemName(const Item& item) {
    if (item.zone == 0) {
        return "row " + std::to_string(item.index) + " " + kRowNames[item.index];
    }
    if (item.zone == 1) {
        return "tile " + std::to_string(item.index) + " (col " + std::to_string(item.index % kCols) +
               " row " + std::to_string(item.index / kCols) + ")";
    }
    return "none";
}

Item ItemAt(float u, float v) {
    for (int zone = 0; zone < 2; ++zone) {
        const int count = zone == 0 ? kRows : kCols * kTileRows;
        for (int i = 0; i < count; ++i) {
            const Rect r = ItemRect({zone, i});
            if (u >= float(r.x) && u < float(r.x + r.w) && v >= float(r.y) && v < float(r.y + r.h)) {
                return {zone, i};
            }
        }
    }
    return {};
}

// dir: 0 up, 1 right, 2 down, 3 left.
void MoveSelection(Item& sel, int dir) {
    if (sel.zone == 0) {
        if (dir == 0) {
            sel.index = std::max(sel.index - 1, 0);
        } else if (dir == 2) {
            sel.index = std::min(sel.index + 1, kRows - 1);
        } else if (dir == 1) {
            sel = {1, (sel.index * kTileRows / kRows) * kCols};
        }
        return;
    }
    int col = sel.index % kCols, row = sel.index / kCols;
    if (dir == 0) {
        row = std::max(row - 1, 0);
    } else if (dir == 2) {
        row = std::min(row + 1, kTileRows - 1);
    } else if (dir == 1) {
        col = std::min(col + 1, kCols - 1);
    } else if (col == 0) {
        sel = {0, row * kRows / kTileRows};
        return;
    } else {
        --col;
    }
    sel.index = row * kCols + col;
}

/// The test menu; `over_image` draws only the item outlines and highlights, so stick navigation
/// and the laser can be tuned over a real screenshot.
void DrawMenu(Image& img, const Item& selected, const Item& hover, bool over_image) {
    for (int zone = 0; zone < 2; ++zone) {
        const int count = zone == 0 ? kRows : kCols * kTileRows;
        for (int i = 0; i < count; ++i) {
            const Item item{zone, i};
            const Rect r = ItemRect(item);
            const bool is_selected = item == selected, is_hover = item == hover;
            if (!over_image) {
                FillRect(img, r.x, r.y, r.w, r.h,
                         is_selected ? Rgb(120, 40, 40)
                         : is_hover  ? Rgb(30, 70, 90)
                                     : Rgb(32, 30, 34));
            }
            if (!over_image || is_selected || is_hover) {
                StrokeRect(img, r.x, r.y, r.w, r.h, is_selected ? 6 : 2,
                           is_selected ? Rgb(230, 200, 120) : Rgb(90, 86, 92));
            } else {
                StrokeRect(img, r.x, r.y, r.w, r.h, 1, Rgb(120, 120, 120));
            }
            if (is_hover) {
                StrokeRect(img, r.x + 8, r.y + 8, r.w - 16, r.h - 16, 4, Rgb(80, 220, 240));
            }
            if (!over_image) {
                const std::string label = zone == 0 ? kRowNames[i] : "TILE " + std::to_string(i);
                DrawText(img, r.x + 24, r.y + (zone == 0 ? 28 : 16), zone == 0 ? 5 : 4, label,
                         is_selected ? Rgb(255, 240, 200) : Rgb(190, 184, 176));
            }
        }
    }
    if (!over_image) {
        DrawText(img, 160, 120, 6, "BLOODBORNE TEST MENU", Rgb(200, 60, 60));
    }
}

// Crosshair grid every 120 px with pixel coordinates at each crossing.
void DrawGrid(Image& img) {
    for (int x = 0; x < int(kWidth); x += 120) {
        FillRect(img, x, 0, 1, int(kHeight), Rgb(70, 90, 70));
    }
    for (int y = 0; y < int(kHeight); y += 120) {
        FillRect(img, 0, y, int(kWidth), 1, Rgb(70, 90, 70));
    }
    for (int x = 0; x < int(kWidth); x += 120) {
        for (int y = 0; y < int(kHeight); y += 120) {
            FillRect(img, x - 6, y, 13, 1, Rgb(160, 255, 160));
            FillRect(img, x, y - 6, 1, 13, Rgb(160, 255, 160));
            DrawText(img, x + 4, y + 3, 2, std::to_string(x) + "," + std::to_string(y),
                     Rgb(120, 170, 120));
        }
    }
}

void DrawLaserDot(Image& img, int x, int y, bool click) {
    const uint32_t color = click ? Rgb(255, 255, 255) : Rgb(255, 60, 60);
    StrokeRect(img, x - 14, y - 14, 29, 29, 3, color);
    FillRect(img, x - 3, y - 3, 7, 7, color);
    FillRect(img, x - 24, y, 49, 1, color);
    FillRect(img, x, y - 24, 1, 49, color);
}

// ---- Optional real screenshot (uncompressed 24/32-bit BMP, stretched to 1920x1080) -------------

bool LoadBmp(const char* path, Image& out) {
    FILE* file = std::fopen(path, "rb");
    if (!file) {
        std::printf("cannot open %s\n", path);
        return false;
    }
    std::vector<uint8_t> data;
    uint8_t chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        data.insert(data.end(), chunk, chunk + got);
    }
    std::fclose(file);
    auto u16 = [&](size_t o) { return uint32_t(data[o] | data[o + 1] << 8); };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    if (data.size() < 54 || data[0] != 'B' || data[1] != 'M') {
        std::printf("%s: not a BMP (only .bmp is supported, no stb_image in the repo)\n", path);
        return false;
    }
    const uint32_t offset = u32(10), header = u32(14);
    const int32_t width = int32_t(u32(18)), raw_height = int32_t(u32(22));
    const uint32_t bits = u16(28), compression = u32(30);
    const int height = raw_height < 0 ? -raw_height : raw_height;
    if (header < 40 || (bits != 24 && bits != 32) || (compression != 0 && compression != 3) ||
        width <= 0 || height <= 0) {
        std::printf("%s: unsupported BMP (need uncompressed 24/32-bit)\n", path);
        return false;
    }
    const size_t stride = (size_t(width) * bits / 8 + 3) & ~size_t(3);
    if (offset + stride * size_t(height) > data.size()) {
        std::printf("%s: truncated BMP\n", path);
        return false;
    }
    out.assign(size_t(kWidth) * kHeight, kBlack);
    for (uint32_t y = 0; y < kHeight; ++y) {
        const int sy = int(uint64_t(y) * uint64_t(height) / kHeight);
        const int row = raw_height < 0 ? sy : height - 1 - sy; // bottom-up unless negative height
        const uint8_t* line = data.data() + offset + stride * size_t(row);
        for (uint32_t x = 0; x < kWidth; ++x) {
            const int sx = int(uint64_t(x) * uint64_t(width) / kWidth);
            const uint8_t* p = line + sx * (bits / 8);
            out[size_t(y) * kWidth + x] = Rgb(p[2], p[1], p[0]); // B, G, R[, A]; alpha forced
        }
    }
    return true;
}

// ---- Misc -------------------------------------------------------------------------------------

uint32_t FindMemoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return 0;
}

double Now() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Edge-triggered direction with auto repeat: fires on press, again after 350 ms, then every 120 ms.
struct Repeater {
    int last = -1;
    double next = 0.0;
    bool Fire(int dir, double t) {
        if (dir < 0) {
            last = -1;
            return false;
        }
        if (dir != last) {
            last = dir;
            next = t + 0.35;
            return true;
        }
        if (t >= next) {
            next = t + 0.12;
            return true;
        }
        return false;
    }
};

} // namespace

int main(int argc, char** argv) {
    // Unbuffered: the log must survive the process being killed.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // The bridge draws the laser beam and hit dot (off by default in the game).
    if (!std::getenv("BB_XR_LASER")) {
        _putenv_s("BB_XR_LASER", "1");
    }
    // Log every input path the runtime reports (what the controllers really send).
    if (!std::getenv("BB_XR_INPUT_DIAG")) {
        _putenv_s("BB_XR_INPUT_DIAG", "1");
    }
    const char* image_path = nullptr;
    double seconds = 0.0;
    int stick_threshold = 70; // distance from 128 that counts as a press
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--image" && i + 1 < argc) {
            image_path = argv[++i];
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (arg == "--stick" && i + 1 < argc) {
            stick_threshold = std::atoi(argv[++i]);
        } else if (arg == "--headlock") {
            _putenv_s("BB_XR_HEADLOCK", "1");
        } else {
            std::printf("usage: bbport_xr_sandbox [--image file.bmp] [--seconds N] [--headlock] "
                        "[--stick N]\n");
            return arg == "--help" ? 0 : 1;
        }
    }
    const bool headlock = std::getenv("BB_XR_HEADLOCK") && std::getenv("BB_XR_HEADLOCK")[0] == '1';
    Image loaded;
    if (image_path && !LoadBmp(image_path, loaded)) {
        return 1;
    }
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

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
    app.pApplicationName = "bbport_xr_sandbox";
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

    // Host-visible staging buffer, persistently mapped.
    const VkDeviceSize bytes = VkDeviceSize(kWidth) * kHeight * 4;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging;
    VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &staging));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, staging, &requirements);
    VkMemoryAllocateInfo memory_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory_info.allocationSize = requirements.size;
    memory_info.memoryTypeIndex =
        FindMemoryType(physical, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory staging_memory;
    VK_CHECK(vkAllocateMemory(device, &memory_info, nullptr, &staging_memory));
    VK_CHECK(vkBindBufferMemory(device, staging, staging_memory, 0));
    void* mapped;
    VK_CHECK(vkMapMemory(device, staging_memory, 0, bytes, 0, &mapped));

    bool placed = false;
    float printed[4] = {}; // screen x, y, z, radius last printed

    Item selected{0, 0};
    Repeater repeater;
    std::string event = image_path ? "IMAGE MODE" : "READY";
    uint32_t last_buttons = 0;
    uint8_t last_r2 = 0;
    double click_flash = -1.0, last_print = -1.0, last_shown = Now();
    std::string last_key;
    Image frame(size_t(kWidth) * kHeight);
    bool ever_shown = false;
    int frames = 0;

    std::printf("sandbox: %s, stick threshold %d%s. Ctrl+C quits.\n",
                image_path ? image_path : "synthetic menu", stick_threshold,
                headlock ? " (headlock)" : "");
    while (!quit) {
        const double t = Now();
        if (seconds > 0.0 && t > seconds) {
            break;
        }

        BbXrInput input;
        xr->GetInput(&input);

        // The screen and the laser hit as the bridge shows them: it places the screen (and
        // again on focus, recenter or left menu held 1 s), curves it, and hit-tests the right
        // controller's aim ray in the screen's own space.
        BbXrViews views;
        xr->GetViews(&views);
        const BbXrScreen& shown = views.screen;
        placed = shown.pose.valid != 0;
        if (placed && !shown.head_locked) {
            const float* p = shown.pose.position;
            const float now_printed[4] = {p[0], p[1], p[2], shown.radius};
            bool moved = false;
            for (int i = 0; i < 4; ++i) {
                moved |= std::fabs(now_printed[i] - printed[i]) > 0.05f;
            }
            if (moved) {
                std::copy(now_printed, now_printed + 4, printed);
                std::printf("screen placed at (%.2f %.2f %.2f), %.2f m wide, %s\n", p[0], p[1],
                            p[2], shown.width, shown.radius > 0.0f ? "curved" : "flat");
            }
        }
        const bool aim_valid = input.aim[1].valid != 0;
        const bool hit = views.pointer.hit != 0;
        const float u = views.pointer.u, v = views.pointer.v;
        const Item hover = hit ? ItemAt(u, v) : Item{};

        // Menu navigation: left stick or d-pad, edge-triggered with repeat.
        int dir = -1;
        if (input.active) {
            if ((input.buttons & BBXR_BTN_UP) || input.ly < 128 - stick_threshold) {
                dir = 0;
            } else if ((input.buttons & BBXR_BTN_DOWN) || input.ly > 128 + stick_threshold) {
                dir = 2;
            } else if ((input.buttons & BBXR_BTN_LEFT) || input.lx < 128 - stick_threshold) {
                dir = 3;
            } else if ((input.buttons & BBXR_BTN_RIGHT) || input.lx > 128 + stick_threshold) {
                dir = 1;
            }
        }
        if (repeater.Fire(dir, t)) {
            MoveSelection(selected, dir);
        }
        const uint32_t pressed = input.buttons & ~last_buttons;
        const bool r2_edge = input.r2 > 128 && last_r2 <= 128;
        if (pressed & BBXR_BTN_CROSS) {
            event = "select " + ItemName(selected);
            std::printf("%s\n", event.c_str());
        }
        if (pressed & BBXR_BTN_CIRCLE) {
            event = "back";
            std::printf("back\n");
        }
        if ((pressed & BBXR_BTN_R1) || r2_edge) {
            const std::string where = " at " + std::to_string(int(u)) + "," + std::to_string(int(v));
            event = hit ? "click " + (hover.zone >= 0 ? ItemName(hover) : std::string("empty")) + where
                        : std::string("click (no hit)");
            std::printf("%s\n", event.c_str());
            click_flash = t;
            if (hit) {
                selected = hover.zone >= 0 ? hover : selected;
                xr->Haptic(1, 0.3f, 0.05f);
            }
        }
        last_buttons = input.buttons;
        last_r2 = input.r2;

        // Compact input line, ~5 Hz and only when something changed.
        char line[256], key[256];
        std::snprintf(key, sizeof(key), "%d 0x%06x L%u,%u R%u,%u L2 %u R2 %u g%x aim%d hit%d %d:%d",
                      int(input.active), input.buttons, input.lx, input.ly, input.rx, input.ry,
                      input.l2, input.r2, input.gestures, int(aim_valid), int(hit), hover.zone,
                      hover.index);
        if (t - last_print >= 0.2 && key != last_key) {
            last_print = t;
            last_key = key;
            std::snprintf(line, sizeof(line),
                          "pad %s btn 0x%06x L(%3u,%3u) R(%3u,%3u) L2 %3u R2 %3u gest 0x%x "
                          "aim %s hit %s (%d,%d) sel %s",
                          input.active ? "on " : "off", input.buttons, input.lx, input.ly, input.rx,
                          input.ry, input.l2, input.r2, input.gestures, aim_valid ? "ok" : "--",
                          hit ? "yes" : "no ", hit ? int(u) : -1, hit ? int(v) : -1,
                          ItemName(selected).c_str());
            std::printf("%s\n", line);
        }

        // Render on the CPU, upload through the staging buffer.
        VkImage image;
        VkFormat format;
        if (!xr->AcquireFlat(kWidth, kHeight, VK_FORMAT_B8G8R8A8_UNORM, &image, &format)) {
            if (ever_shown && Now() - last_shown > 60.0) {
                std::printf("headset not showing for 60 s, assuming the session ended\n");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        ever_shown = true;
        last_shown = Now();
        if (image_path) {
            frame = loaded;
            DrawMenu(frame, selected, hover, true);
        } else {
            std::fill(frame.begin(), frame.end(), Rgb(14, 14, 18));
            DrawMenu(frame, selected, hover, false);
            DrawGrid(frame);
        }
        DrawText(frame, 160, 880, 4, "LAST: " + event, Rgb(255, 255, 255));
        DrawText(frame, 160, 930, 3, placed ? "SCREEN PLACED" : "HOLD STILL: PLACING SCREEN",
                 Rgb(160, 160, 160));
        if (hit) {
            DrawLaserDot(frame, int(u), int(v), t - click_flash < 0.2);
        }
        std::memcpy(mapped, frame.data(), size_t(bytes));

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        Barrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
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
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    std::printf("submitted %d flat frames in %.0f s\n", frames, Now());
    xr->Shutdown();
    vkDeviceWaitIdle(device);
    vkUnmapMemory(device, staging_memory);
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, staging_memory, nullptr);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
