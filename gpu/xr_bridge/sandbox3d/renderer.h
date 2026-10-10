// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// sandbox3d's Vulkan renderer: forward, per eye with dynamic rendering (Vulkan 1.3), GPU-skinned
// meshes from a palette storage buffer, flat-lit, plus depth-tested and always-on-top lines.
// Renders into the bridge's stereo swapchain images (or its own images offline), submits under
// the host queue lock and waits, one frame at a time.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "math3d.h"
#include "model.h"

namespace sb {

struct LineVertex {
    float position[3];
    uint8_t color[4];
};

struct MeshDraw {
    int model = 0; // UploadModel's handle
    uint32_t first_index = 0, index_count = 0;
    uint32_t palette_base = 0;
    float color[4] = {1, 1, 1, 1};
    uint32_t flags = 0; // 1: floor checker
};

struct FrameData {
    std::vector<m3::Mat4> palette;
    std::vector<MeshDraw> meshes;
    std::vector<LineVertex> lines;   // depth tested
    std::vector<LineVertex> overlay; // drawn on top
    m3::Vec3 clear{0.03f, 0.03f, 0.04f};
};

struct EyeTarget {
    m3::Mat4 view_proj;
    VkImage color = VK_NULL_HANDLE;
    VkImage depth = VK_NULL_HANDLE; // VK_NULL_HANDLE: the renderer's own depth image
    uint32_t width = 0, height = 0;
};

/// Extra drawing inside each eye's pass, after the meshes and lines (the VR menus, ui_render.h):
/// the attachments are bound and the viewport/scissor set.
class DrawHook {
public:
    virtual ~DrawHook() = default;
    virtual void Draw(VkCommandBuffer cmd, const m3::Mat4& view_proj, VkFormat color,
                      VkFormat depth) = 0;
};

class Renderer {
public:
    struct Device {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        uint32_t family = 0;
        VkQueue queue = VK_NULL_HANDLE;
        void (*lock)(void* user, int lock) = nullptr; // the queue lock shared with the bridge
        void* lock_user = nullptr;
    };

    bool Create(const Device& device, std::string& error);
    void Destroy();
    /// Static geometry; returns the model handle for MeshDraw (-1 on failure).
    int UploadModel(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
    /// Both eyes into the given images (color in COLOR_ATTACHMENT_OPTIMAL, depth in
    /// DEPTH_STENCIL_ATTACHMENT_OPTIMAL, both left so), submitted and waited for.
    bool Render(const EyeTarget eyes[2], VkFormat color_format, VkFormat depth_format,
                const FrameData& data);
    /// Offline: renders into the renderer's own sRGB images and reads them back (RGBA8, rows top
    /// down).
    bool RenderOffscreen(uint32_t width, uint32_t height, const m3::Mat4 view_proj[2],
                         const FrameData& data, std::vector<uint8_t> pixels[2]);
    const std::string& LastError() const { return last_error; }
    const Device& GetDevice() const { return dev; }
    /// Draws after the scene in every pass (nullptr: none).
    void SetDrawHook(DrawHook* hook) { draw_hook = hook; }
    DrawHook* GetDrawHook() const { return draw_hook; } // to chain another pass after it

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    struct Pipelines {
        VkPipeline mesh = VK_NULL_HANDLE, lines = VK_NULL_HANDLE, overlay = VK_NULL_HANDLE;
    };
    struct GpuModel {
        Buffer vertices, indices;
    };

    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, Buffer& out);
    void DestroyBuffer(Buffer& buffer);
    bool CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                     VkImageAspectFlags aspect, Image& out);
    void DestroyImage(Image& image);
    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
    bool EnsureHostBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage);
    VkImageView ViewFor(VkImage image, VkFormat format, VkImageAspectFlags aspect);
    const Pipelines* PipelinesFor(VkFormat color, VkFormat depth);
    VkPipeline CreatePipeline(bool mesh, bool depth_test, VkFormat color, VkFormat depth);
    bool OneShot(void (*record)(VkCommandBuffer, void*), void* user);
    bool Submit();
    void Record(VkCommandBuffer cmd, const EyeTarget eyes[2], VkFormat color_format,
                VkFormat depth_format, const FrameData& data, const VkImageView depth_views[2],
                const Pipelines& pipes);

    Device dev;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout mesh_layout = VK_NULL_HANDLE, line_layout = VK_NULL_HANDLE;
    VkShaderModule shaders[4] = {};
    std::map<uint64_t, Pipelines> pipelines; // key: color format << 32 | depth format
    std::map<VkImage, VkImageView> views;
    std::vector<GpuModel> models;
    Buffer palette, lines;
    Image own_depth[2];
    Image offscreen[2];
    Buffer readback;
    std::string last_error;
    DrawHook* draw_hook = nullptr;
};

} // namespace sb
