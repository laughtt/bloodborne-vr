// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "vr_ui_layer.h"

#include "vr_game_ui.h"

namespace vrui {

bool UiLayerGpu::Create(const sb::Renderer::Device& device, std::string& error) {
    dev = device;
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
        error = "UI layer: command pool/buffer/fence";
        return false;
    }
    return true;
}

void UiLayerGpu::Destroy() {
    if (!dev.device) {
        return;
    }
    Wait(UINT64_MAX);
    for (auto& [image, view] : views) {
        vkDestroyImageView(dev.device, view, nullptr);
    }
    views.clear();
    if (fence) {
        vkDestroyFence(dev.device, fence, nullptr);
    }
    if (pool) {
        vkDestroyCommandPool(dev.device, pool, nullptr);
    }
    fence = VK_NULL_HANDLE;
    pool = VK_NULL_HANDLE;
    cmd = VK_NULL_HANDLE;
    dev = {};
}

bool UiLayerGpu::Wait(uint64_t timeout_ns) {
    if (!in_flight) {
        return true;
    }
    if (vkWaitForFences(dev.device, 1, &fence, VK_TRUE, timeout_ns) != VK_SUCCESS) {
        return false;
    }
    vkResetFences(dev.device, 1, &fence);
    in_flight = false;
    return true;
}

VkImageView UiLayerGpu::ViewFor(VkImage image, VkFormat format) {
    const auto it = views.find(image);
    if (it != views.end()) {
        return it->second;
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
    views[image] = view;
    return view;
}

bool UiLayerGpu::Render(GameUi& ui, const VkImage color[2], VkFormat format, uint32_t width,
                        uint32_t height, const m3::Mat4 view_proj[2]) {
    if (!cmd || in_flight) {
        last_error = "UI layer: the previous frame is still on the GPU";
        return false;
    }
    VkImageView targets[2] = {ViewFor(color[0], format), ViewFor(color[1], format)};
    if (!targets[0] || !targets[1]) {
        last_error = "UI layer: image view";
        return false;
    }
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    for (int eye = 0; eye < 2; ++eye) {
        // The acquired image's old contents are not needed: everything is cleared.
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = color[eye];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = targets[eye];
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {width, height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        vkCmdBeginRendering(cmd, &rendering);
        const VkViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {width, height}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        ui.Draw(cmd, view_proj[eye], format);
        vkCmdEndRendering(cmd);
        // For the compositor (it reads the image after the runtime's own sync on this queue).
        barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
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
        last_error = "UI layer: vkQueueSubmit " + std::to_string(int(result));
        return false;
    }
    in_flight = true;
    return true;
}

} // namespace vrui
