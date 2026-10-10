// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "renderer.h"

#include <cstdint>
#include <cstring>

// SPIR-V compiled at build time (glslangValidator --vn, see CMakeLists.txt).
#include "line_frag.h"
#include "line_vert.h"
#include "mesh_frag.h"
#include "mesh_vert.h"

namespace sb {
namespace {

struct MeshPush {
    float view_proj[16];
    float color[4];
    uint32_t palette_base;
    uint32_t flags;
};
static_assert(sizeof(MeshPush) == 88, "push constant layout of mesh.vert/mesh.frag");

constexpr VkImageLayout kDepthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

bool HasStencil(VkFormat format) {
    return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_D16_UNORM_S8_UINT;
}

VkImageAspectFlags DepthAspect(VkFormat format) {
    return VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
}

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

} // namespace

uint32_t Renderer::MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool Renderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, Buffer& out) {
    out = {};
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev.device, &info, nullptr, &out.buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev.device, out.buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = MemoryType(
        req.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(dev.device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindBufferMemory(dev.device, out.buffer, out.memory, 0) != VK_SUCCESS) {
        DestroyBuffer(out);
        return false;
    }
    if (host && vkMapMemory(dev.device, out.memory, 0, size, 0, &out.mapped) != VK_SUCCESS) {
        DestroyBuffer(out);
        return false;
    }
    out.size = size;
    return true;
}

void Renderer::DestroyBuffer(Buffer& buffer) {
    if (buffer.buffer) {
        vkDestroyBuffer(dev.device, buffer.buffer, nullptr);
    }
    if (buffer.memory) {
        vkFreeMemory(dev.device, buffer.memory, nullptr);
    }
    buffer = {};
}

bool Renderer::CreateImage(uint32_t width, uint32_t height, VkFormat format,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect, Image& out) {
    out = {};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev.device, &info, nullptr, &out.image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev.device, out.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(dev.device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindImageMemory(dev.device, out.image, out.memory, 0) != VK_SUCCESS ||
        (view.image = out.image, vkCreateImageView(dev.device, &view, nullptr, &out.view)) !=
            VK_SUCCESS) {
        DestroyImage(out);
        return false;
    }
    out.width = width;
    out.height = height;
    out.format = format;
    return true;
}

void Renderer::DestroyImage(Image& image) {
    if (image.view) {
        vkDestroyImageView(dev.device, image.view, nullptr);
    }
    if (image.image) {
        vkDestroyImage(dev.device, image.image, nullptr);
    }
    if (image.memory) {
        vkFreeMemory(dev.device, image.memory, nullptr);
    }
    image = {};
}

bool Renderer::Create(const Device& device, std::string& error) {
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
        (alloc.commandPool = pool, vkAllocateCommandBuffers(dev.device, &alloc, &cmd)) !=
            VK_SUCCESS ||
        vkCreateFence(dev.device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
        error = "command pool/buffer/fence";
        return false;
    }
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpool.maxSets = 1;
    dpool.poolSizeCount = 1;
    dpool.pPoolSizes = &pool_size;
    if (vkCreateDescriptorSetLayout(dev.device, &set_info, nullptr, &set_layout) != VK_SUCCESS ||
        vkCreateDescriptorPool(dev.device, &dpool, nullptr, &descriptor_pool) != VK_SUCCESS) {
        error = "descriptor layout/pool";
        return false;
    }
    VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_alloc.descriptorPool = descriptor_pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &set_layout;
    if (vkAllocateDescriptorSets(dev.device, &set_alloc, &descriptor_set) != VK_SUCCESS) {
        error = "descriptor set";
        return false;
    }
    VkPushConstantRange mesh_push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                  sizeof(MeshPush)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &mesh_push;
    VkPushConstantRange line_push{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
    VkPipelineLayoutCreateInfo line_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    line_info.pushConstantRangeCount = 1;
    line_info.pPushConstantRanges = &line_push;
    if (vkCreatePipelineLayout(dev.device, &layout, nullptr, &mesh_layout) != VK_SUCCESS ||
        vkCreatePipelineLayout(dev.device, &line_info, nullptr, &line_layout) != VK_SUCCESS) {
        error = "pipeline layouts";
        return false;
    }
    const struct {
        const uint32_t* code;
        size_t size;
    } code[4] = {{mesh_vert_spv, sizeof(mesh_vert_spv)},
                 {mesh_frag_spv, sizeof(mesh_frag_spv)},
                 {line_vert_spv, sizeof(line_vert_spv)},
                 {line_frag_spv, sizeof(line_frag_spv)}};
    for (int i = 0; i < 4; ++i) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code[i].size;
        info.pCode = code[i].code;
        if (vkCreateShaderModule(dev.device, &info, nullptr, &shaders[i]) != VK_SUCCESS) {
            error = "shader module";
            return false;
        }
    }
    // A palette from the start, so the descriptor set is always valid.
    if (!EnsureHostBuffer(palette, sizeof(m3::Mat4) * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        error = "palette buffer";
        return false;
    }
    return true;
}

void Renderer::Destroy() {
    if (!dev.device) {
        return;
    }
    vkDeviceWaitIdle(dev.device);
    for (auto& [key, p] : pipelines) {
        for (VkPipeline pipe : {p.mesh, p.lines, p.overlay}) {
            if (pipe) {
                vkDestroyPipeline(dev.device, pipe, nullptr);
            }
        }
    }
    pipelines.clear();
    for (auto& [image, view] : views) {
        vkDestroyImageView(dev.device, view, nullptr);
    }
    views.clear();
    for (GpuModel& m : models) {
        DestroyBuffer(m.vertices);
        DestroyBuffer(m.indices);
    }
    models.clear();
    DestroyBuffer(palette);
    DestroyBuffer(lines);
    DestroyBuffer(readback);
    for (int eye = 0; eye < 2; ++eye) {
        DestroyImage(own_depth[eye]);
        DestroyImage(offscreen[eye]);
    }
    for (VkShaderModule& module : shaders) {
        if (module) {
            vkDestroyShaderModule(dev.device, module, nullptr);
            module = VK_NULL_HANDLE;
        }
    }
    if (mesh_layout) {
        vkDestroyPipelineLayout(dev.device, mesh_layout, nullptr);
    }
    if (line_layout) {
        vkDestroyPipelineLayout(dev.device, line_layout, nullptr);
    }
    if (descriptor_pool) {
        vkDestroyDescriptorPool(dev.device, descriptor_pool, nullptr);
    }
    if (set_layout) {
        vkDestroyDescriptorSetLayout(dev.device, set_layout, nullptr);
    }
    if (fence) {
        vkDestroyFence(dev.device, fence, nullptr);
    }
    if (pool) {
        vkDestroyCommandPool(dev.device, pool, nullptr);
    }
    dev = {};
}

bool Renderer::EnsureHostBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage) {
    if (buffer.buffer && buffer.size >= size) {
        return true;
    }
    DestroyBuffer(buffer);
    VkDeviceSize capacity = 4096;
    while (capacity < size) {
        capacity *= 2;
    }
    if (!CreateBuffer(capacity, usage, true, buffer)) {
        return false;
    }
    if (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
        VkDescriptorBufferInfo info{buffer.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptor_set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &info;
        vkUpdateDescriptorSets(dev.device, 1, &write, 0, nullptr);
    }
    return true;
}

bool Renderer::OneShot(void (*record)(VkCommandBuffer, void*), void* user) {
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    record(cmd, user);
    vkEndCommandBuffer(cmd);
    return Submit();
}

bool Renderer::Submit() {
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
        last_error = "vkQueueSubmit failed (" + std::to_string(int(result)) + ")";
        return false;
    }
    vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev.device, 1, &fence);
    return true;
}

int Renderer::UploadModel(const std::vector<Vertex>& vertices,
                          const std::vector<uint32_t>& indices) {
    GpuModel model;
    const VkDeviceSize vbytes = vertices.size() * sizeof(Vertex);
    const VkDeviceSize ibytes = indices.size() * sizeof(uint32_t);
    Buffer staging;
    if (vbytes == 0 || ibytes == 0 ||
        !CreateBuffer(vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      false, model.vertices) ||
        !CreateBuffer(ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      false, model.indices) ||
        !CreateBuffer(vbytes + ibytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging)) {
        DestroyBuffer(model.vertices);
        DestroyBuffer(model.indices);
        last_error = "model buffers";
        return -1;
    }
    std::memcpy(staging.mapped, vertices.data(), size_t(vbytes));
    std::memcpy(static_cast<uint8_t*>(staging.mapped) + vbytes, indices.data(), size_t(ibytes));
    struct Copy {
        VkBuffer src, vdst, idst;
        VkDeviceSize vbytes, ibytes;
    } copy{staging.buffer, model.vertices.buffer, model.indices.buffer, vbytes, ibytes};
    const bool ok = OneShot(
        [](VkCommandBuffer c, void* user) {
            const Copy& k = *static_cast<Copy*>(user);
            VkBufferCopy v{0, 0, k.vbytes};
            vkCmdCopyBuffer(c, k.src, k.vdst, 1, &v);
            VkBufferCopy i{k.vbytes, 0, k.ibytes};
            vkCmdCopyBuffer(c, k.src, k.idst, 1, &i);
        },
        &copy);
    DestroyBuffer(staging);
    if (!ok) {
        DestroyBuffer(model.vertices);
        DestroyBuffer(model.indices);
        return -1;
    }
    models.push_back(model);
    return int(models.size()) - 1;
}

VkImageView Renderer::ViewFor(VkImage image, VkFormat format, VkImageAspectFlags aspect) {
    const auto it = views.find(image);
    if (it != views.end()) {
        return it->second;
    }
    if (views.size() > 64) { // swapchains were recreated: drop the stale views
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
    info.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &info, nullptr, &view) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    views[image] = view;
    return view;
}

VkPipeline Renderer::CreatePipeline(bool mesh, bool depth_test, VkFormat color, VkFormat depth) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shaders[mesh ? 0 : 2];
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shaders[mesh ? 1 : 3];
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, mesh ? uint32_t(sizeof(Vertex))
                                                    : uint32_t(sizeof(LineVertex)),
                                            VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription mesh_attrs[5] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {3, 0, VK_FORMAT_R16G16B16A16_UINT, offsetof(Vertex, bones)},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, weights)}};
    const VkVertexInputAttributeDescription line_attrs[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex, position)},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(LineVertex, color)}};
    VkPipelineVertexInputStateCreateInfo input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = mesh ? 5 : 2;
    input.pVertexAttributeDescriptions = mesh ? mesh_attrs : line_attrs;

    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = mesh ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE; // mirrored copies flip the winding
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_state{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_state.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    depth_state.depthWriteEnable = mesh ? VK_TRUE : VK_FALSE;
    depth_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color;
    rendering.depthAttachmentFormat = depth;
    rendering.stencilAttachmentFormat = HasStencil(depth) ? depth : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_state;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = mesh ? mesh_layout : line_layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

const Renderer::Pipelines* Renderer::PipelinesFor(VkFormat color, VkFormat depth) {
    const uint64_t key = (uint64_t(color) << 32) | uint64_t(depth);
    const auto it = pipelines.find(key);
    if (it != pipelines.end()) {
        return &it->second;
    }
    Pipelines p;
    p.mesh = CreatePipeline(true, true, color, depth);
    p.lines = CreatePipeline(false, true, color, depth);
    p.overlay = CreatePipeline(false, false, color, depth);
    if (!p.mesh || !p.lines || !p.overlay) {
        last_error = "pipeline creation failed";
        return nullptr;
    }
    return &(pipelines[key] = p);
}

void Renderer::Record(VkCommandBuffer c, const EyeTarget eyes[2], VkFormat color_format,
                      VkFormat depth_format, const FrameData& data,
                      const VkImageView depth_views[2], const Pipelines& pipes) {
    const uint32_t line_count = uint32_t(data.lines.size());
    const uint32_t overlay_count = uint32_t(data.overlay.size());
    for (int eye = 0; eye < 2; ++eye) {
        const EyeTarget& target = eyes[eye];
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = ViewFor(target.color, color_format, VK_IMAGE_ASPECT_COLOR_BIT);
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{data.clear.x, data.clear.y, data.clear.z, 1.0f}};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depth_views[eye];
        depth.imageLayout = kDepthLayout;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // the compositor may read it
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {target.width, target.height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        rendering.pDepthAttachment = &depth;
        VkRenderingAttachmentInfo stencil = depth;
        if (HasStencil(depth_format)) {
            rendering.pStencilAttachment = &stencil;
        }
        vkCmdBeginRendering(c, &rendering);
        const VkViewport viewport{0.0f, 0.0f, float(target.width), float(target.height),
                                  0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {target.width, target.height}};
        vkCmdSetViewport(c, 0, 1, &viewport);
        vkCmdSetScissor(c, 0, 1, &scissor);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes.mesh);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout, 0, 1,
                                &descriptor_set, 0, nullptr);
        int bound = -1;
        for (const MeshDraw& draw : data.meshes) {
            if (draw.model < 0 || size_t(draw.model) >= models.size() || !draw.index_count) {
                continue;
            }
            if (draw.model != bound) {
                bound = draw.model;
                const VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(c, 0, 1, &models[size_t(bound)].vertices.buffer, &zero);
                vkCmdBindIndexBuffer(c, models[size_t(bound)].indices.buffer, 0,
                                     VK_INDEX_TYPE_UINT32);
            }
            MeshPush push{};
            std::memcpy(push.view_proj, target.view_proj.m, sizeof(push.view_proj));
            std::memcpy(push.color, draw.color, sizeof(push.color));
            push.palette_base = draw.palette_base;
            push.flags = draw.flags;
            vkCmdPushConstants(c, mesh_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            vkCmdDrawIndexed(c, draw.index_count, 1, draw.first_index, 0, 0);
        }
        if (line_count + overlay_count) {
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(c, 0, 1, &lines.buffer, &zero);
            if (line_count) {
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes.lines);
                vkCmdPushConstants(c, line_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64,
                                   target.view_proj.m);
                vkCmdDraw(c, line_count, 1, 0, 0);
            }
            if (overlay_count) {
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes.overlay);
                vkCmdPushConstants(c, line_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64,
                                   target.view_proj.m);
                vkCmdDraw(c, overlay_count, 1, line_count, 0);
            }
        }
        if (draw_hook) {
            draw_hook->Draw(c, target.view_proj, color_format, depth_format);
        }
        vkCmdEndRendering(c);
    }
}

bool Renderer::Render(const EyeTarget eyes[2], VkFormat color_format, VkFormat depth_format,
                      const FrameData& data) {
    // Depth: the bridge's images, else our own (D32), created at the eye size.
    VkFormat used_depth = depth_format;
    VkImageView depth_views[2] = {};
    bool fresh_depth[2] = {};
    for (int eye = 0; eye < 2; ++eye) {
        if (eyes[eye].depth) {
            depth_views[eye] = ViewFor(eyes[eye].depth, depth_format, DepthAspect(depth_format));
            continue;
        }
        used_depth = VK_FORMAT_D32_SFLOAT;
        Image& own = own_depth[eye];
        if (own.width != eyes[eye].width || own.height != eyes[eye].height) {
            vkDeviceWaitIdle(dev.device);
            DestroyImage(own);
            if (!CreateImage(eyes[eye].width, eyes[eye].height, VK_FORMAT_D32_SFLOAT,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                             VK_IMAGE_ASPECT_DEPTH_BIT, own)) {
                last_error = "depth image";
                return false;
            }
            fresh_depth[eye] = true;
        }
        depth_views[eye] = own.view;
    }
    if (!depth_views[0] || !depth_views[1]) {
        last_error = "depth views";
        return false;
    }
    const Pipelines* pipes = PipelinesFor(color_format, used_depth);
    if (!pipes) {
        return false;
    }
    // Per-frame data: the palette and the lines (host visible; the previous frame finished).
    if (!EnsureHostBuffer(palette, data.palette.size() * sizeof(m3::Mat4) + sizeof(m3::Mat4),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !EnsureHostBuffer(lines, (data.lines.size() + data.overlay.size() + 1) *
                                     sizeof(LineVertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
        last_error = "frame buffers";
        return false;
    }
    if (!data.palette.empty()) {
        std::memcpy(palette.mapped, data.palette.data(), data.palette.size() * sizeof(m3::Mat4));
    }
    auto* line_out = static_cast<LineVertex*>(lines.mapped);
    if (!data.lines.empty()) {
        std::memcpy(line_out, data.lines.data(), data.lines.size() * sizeof(LineVertex));
    }
    if (!data.overlay.empty()) {
        std::memcpy(line_out + data.lines.size(), data.overlay.data(),
                    data.overlay.size() * sizeof(LineVertex));
    }

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    for (int eye = 0; eye < 2; ++eye) {
        if (fresh_depth[eye]) {
            ImageBarrier(cmd, own_depth[eye].image, VK_IMAGE_ASPECT_DEPTH_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, kDepthLayout);
        }
    }
    Record(cmd, eyes, color_format, used_depth, data, depth_views, *pipes);
    vkEndCommandBuffer(cmd);
    return Submit();
}

bool Renderer::RenderOffscreen(uint32_t width, uint32_t height, const m3::Mat4 view_proj[2],
                               const FrameData& data, std::vector<uint8_t> pixels[2]) {
    constexpr VkFormat kColor = VK_FORMAT_R8G8B8A8_SRGB;
    bool fresh = false;
    for (int eye = 0; eye < 2; ++eye) {
        if (offscreen[eye].width != width || offscreen[eye].height != height) {
            vkDeviceWaitIdle(dev.device);
            DestroyImage(offscreen[eye]);
            if (!CreateImage(width, height, kColor,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, offscreen[eye])) {
                last_error = "offscreen image";
                return false;
            }
            fresh = true;
        }
    }
    const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
    if (!EnsureHostBuffer(readback, bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        last_error = "readback buffer";
        return false;
    }
    if (fresh) {
        struct Ctx {
            VkImage images[2];
        } ctx{{offscreen[0].image, offscreen[1].image}};
        if (!OneShot(
                [](VkCommandBuffer c, void* user) {
                    for (VkImage image : static_cast<Ctx*>(user)->images) {
                        ImageBarrier(c, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                     VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                    }
                },
                &ctx)) {
            return false;
        }
    }
    EyeTarget eyes[2];
    for (int eye = 0; eye < 2; ++eye) {
        eyes[eye].view_proj = view_proj[eye];
        eyes[eye].color = offscreen[eye].image;
        eyes[eye].width = width;
        eyes[eye].height = height;
    }
    if (!Render(eyes, kColor, VK_FORMAT_UNDEFINED, data)) {
        return false;
    }
    struct Ctx {
        VkImage images[2];
        VkBuffer buffer;
        uint32_t width, height;
    } ctx{{offscreen[0].image, offscreen[1].image}, readback.buffer, width, height};
    if (!OneShot(
            [](VkCommandBuffer c, void* user) {
                const Ctx& k = *static_cast<Ctx*>(user);
                for (int eye = 0; eye < 2; ++eye) {
                    ImageBarrier(c, k.images[eye], VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    VkBufferImageCopy region{};
                    region.bufferOffset = VkDeviceSize(eye) * k.width * k.height * 4;
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.imageExtent = {k.width, k.height, 1};
                    vkCmdCopyImageToBuffer(c, k.images[eye], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           k.buffer, 1, &region);
                    ImageBarrier(c, k.images[eye], VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                }
            },
            &ctx)) {
        return false;
    }
    for (int eye = 0; eye < 2; ++eye) {
        const auto* src = static_cast<const uint8_t*>(readback.mapped) + size_t(eye) * bytes;
        pixels[eye].assign(src, src + bytes);
    }
    return true;
}

} // namespace sb
