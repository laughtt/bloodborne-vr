// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "vignette.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

// SPIR-V of the debug-line shaders (compiled at build time, see CMakeLists.txt).
#include "line_frag.h"
#include "line_vert.h"

namespace sb {

using namespace m3;

namespace {

constexpr int kGrid = 32; // cells per side of each eye's grid

} // namespace

float Vignette::Transmission(float angle, float strength, float inner, float outer) {
    const float x = std::clamp((angle - inner) / std::max(outer - inner, 1e-3f), 0.0f, 1.0f);
    return 1.0f - std::clamp(strength, 0.0f, 1.0f) * x * x * (3.0f - 2.0f * x);
}

bool Vignette::Create(const Renderer::Device& device, std::string& error) {
    dev = device;
    vkGetPhysicalDeviceMemoryProperties(dev.physical, &memory_properties);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = dev.family;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateCommandPool(dev.device, &pool_info, nullptr, &pool) != VK_SUCCESS ||
        (alloc.commandPool = pool, vkAllocateCommandBuffers(dev.device, &alloc, &cmd)) != VK_SUCCESS ||
        vkCreateFence(dev.device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
        error = "vignette: command pool/buffer/fence";
        return false;
    }
    const struct {
        const uint32_t* code;
        size_t size;
    } code[2] = {{line_vert_spv, sizeof(line_vert_spv)}, {line_frag_spv, sizeof(line_frag_spv)}};
    for (int i = 0; i < 2; ++i) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code[i].size;
        info.pCode = code[i].code;
        if (vkCreateShaderModule(dev.device, &info, nullptr, &shaders[i]) != VK_SUCCESS) {
            error = "vignette: shader module";
            return false;
        }
    }
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev.device, &layout_info, nullptr, &layout) != VK_SUCCESS) {
        error = "vignette: pipeline layout";
        return false;
    }
    return true;
}

void Vignette::Destroy() {
    if (!dev.device) {
        return;
    }
    vkDeviceWaitIdle(dev.device);
    for (auto& [format, pipeline] : pipelines) {
        vkDestroyPipeline(dev.device, pipeline, nullptr);
    }
    pipelines.clear();
    for (auto& [image, view] : views) {
        vkDestroyImageView(dev.device, view, nullptr);
    }
    views.clear();
    if (buffer) {
        vkDestroyBuffer(dev.device, buffer, nullptr);
    }
    if (memory) {
        vkFreeMemory(dev.device, memory, nullptr);
    }
    for (VkShaderModule& module : shaders) {
        if (module) {
            vkDestroyShaderModule(dev.device, module, nullptr);
            module = VK_NULL_HANDLE;
        }
    }
    if (layout) {
        vkDestroyPipelineLayout(dev.device, layout, nullptr);
    }
    if (fence) {
        vkDestroyFence(dev.device, fence, nullptr);
    }
    if (pool) {
        vkDestroyCommandPool(dev.device, pool, nullptr);
    }
    dev = {};
}

VkPipeline Vignette::PipelineFor(VkFormat color) {
    const auto it = pipelines.find(color);
    if (it != pipelines.end()) {
        return it->second;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shaders[0];
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shaders[1];
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0, uint32_t(sizeof(LineVertex)), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attrs[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex, position)},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(LineVertex, color)}};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = 2;
    input.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // result = scene * vertex color (the light that goes through); alpha stays.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return pipelines[color] = pipeline;
}

VkImageView Vignette::ViewFor(VkImage image, VkFormat format) {
    const auto it = views.find(image);
    if (it != views.end()) {
        return it->second;
    }
    if (views.size() > 64) { // the bridge recreated its swapchains: drop the stale views
        vkDeviceWaitIdle(dev.device);
        for (auto& [img, view] : views) {
            vkDestroyImageView(dev.device, view, nullptr);
        }
        views.clear();
    }
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &info, nullptr, &view) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return views[image] = view;
}

bool Vignette::Draw(const Eye eyes[2], VkFormat color_format, float strength, float inner_deg, float outer_deg) {
    if (!(strength >= 0.005f) || !dev.device) {
        return true;
    }
    const float inner = inner_deg * kDeg, outer = std::max(outer_deg, inner_deg + 1.0f) * kDeg;
    // Each eye: a grid over its frustum (a little past its edges), in eye space at z = -1.
    std::vector<LineVertex> vertices;
    vertices.reserve(size_t(2 * kGrid * kGrid * 6));
    uint32_t first[2] = {}, count[2] = {};
    for (int e = 0; e < 2; ++e) {
        first[e] = uint32_t(vertices.size());
        const float l = std::tan(eyes[e].fov[0]) * 1.02f, r = std::tan(eyes[e].fov[1]) * 1.02f;
        const float u = std::tan(eyes[e].fov[2]) * 1.02f, d = std::tan(eyes[e].fov[3]) * 1.02f;
        const auto vertex = [&](int i, int j) {
            const float x = l + (r - l) * float(i) / float(kGrid), y = d + (u - d) * float(j) / float(kGrid);
            const Vec3 in_head = Rotate(eyes[e].eye_in_head, Normalize(Vec3{x, y, -1.0f}));
            const float angle = std::acos(std::clamp(-in_head.z, -1.0f, 1.0f));
            const uint8_t g = uint8_t(std::lround(255.0f * Transmission(angle, strength, inner, outer)));
            return LineVertex{{x, y, -1.0f}, {g, g, g, 255}};
        };
        for (int j = 0; j < kGrid; ++j) {
            for (int i = 0; i < kGrid; ++i) {
                const LineVertex a = vertex(i, j), b = vertex(i + 1, j), c = vertex(i + 1, j + 1),
                                 dd = vertex(i, j + 1);
                if (a.color[0] == 255 && b.color[0] == 255 && c.color[0] == 255 && dd.color[0] == 255) {
                    continue; // all light goes through: nothing to draw
                }
                vertices.insert(vertices.end(), {a, b, c, a, c, dd});
            }
        }
        count[e] = uint32_t(vertices.size()) - first[e];
    }
    if (vertices.empty()) {
        return true;
    }
    const VkDeviceSize bytes = vertices.size() * sizeof(LineVertex);
    if (bytes > capacity) {
        vkDeviceWaitIdle(dev.device);
        if (buffer) {
            vkDestroyBuffer(dev.device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
        }
        if (memory) {
            vkFreeMemory(dev.device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
        capacity = 0;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes * 2;
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (vkCreateBuffer(dev.device, &info, nullptr, &buffer) != VK_SUCCESS) {
            last_error = "vignette: vertex buffer";
            return false;
        }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev.device, buffer, &req);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & want) == want) {
                type = i;
                break;
            }
        }
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        if (type == UINT32_MAX || vkAllocateMemory(dev.device, &alloc, nullptr, &memory) != VK_SUCCESS ||
            vkBindBufferMemory(dev.device, buffer, memory, 0) != VK_SUCCESS ||
            vkMapMemory(dev.device, memory, 0, info.size, 0, &mapped) != VK_SUCCESS) {
            last_error = "vignette: vertex memory";
            return false;
        }
        capacity = info.size;
    }
    std::memcpy(mapped, vertices.data(), size_t(bytes));
    const VkPipeline pipeline = PipelineFor(color_format);
    if (!pipeline) {
        last_error = "vignette: pipeline";
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    // The scene pass wrote these images (its submission finished): make the writes visible.
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    for (int e = 0; e < 2; ++e) {
        if (!count[e]) {
            continue;
        }
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = ViewFor(eyes[e].color, color_format);
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (!color.imageView) {
            continue;
        }
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {eyes[e].width, eyes[e].height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &rendering);
        const VkViewport viewport{0.0f, 0.0f, float(eyes[e].width), float(eyes[e].height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {eyes[e].width, eyes[e].height}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const Mat4 proj = ProjectionFov(eyes[e].fov[0], eyes[e].fov[1], eyes[e].fov[2], eyes[e].fov[3], 0.05f, 100.0f);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, proj.m);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
        vkCmdDraw(cmd, count[e], 1, first[e], 0);
        vkCmdEndRendering(cmd);
    }
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (dev.lock) {
        dev.lock(dev.lock_user, 1);
    }
    const VkResult result = vkQueueSubmit(dev.queue, 1, &submit, fence);
    if (dev.lock) {
        dev.lock(dev.lock_user, 0);
    }
    if (result != VK_SUCCESS) {
        last_error = "vignette: vkQueueSubmit failed (" + std::to_string(int(result)) + ")";
        return false;
    }
    vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev.device, 1, &fence);
    return true;
}

} // namespace sb
