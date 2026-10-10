// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The Vulkan side of the game's VR UI layer (vr_game_ui.h): records both eyes of the UI into the
// bridge's UI swapchain images (dynamic rendering, cleared to transparent, premultiplied alpha)
// and submits them on the queue the bridge shares with the game, under its queue lock. One frame
// in flight: Wait() before the next frame's geometry goes to the GPU.

#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <vulkan/vulkan.h>

#include "sandbox3d/math3d.h"
#include "sandbox3d/renderer.h"

namespace vrui {

class GameUi;

class UiLayerGpu {
public:
    bool Create(const sb::Renderer::Device& device, std::string& error);
    void Destroy();
    /// Waits for the previous submission (at most timeout_ns); false while it still runs.
    bool Wait(uint64_t timeout_ns);
    /// Records and submits the UI into color[eye] (COLOR_ATTACHMENT_OPTIMAL in and out). The
    /// submission is not waited for.
    bool Render(GameUi& ui, const VkImage color[2], VkFormat format, uint32_t width,
                uint32_t height, const m3::Mat4 view_proj[2]);
    const std::string& LastError() const { return last_error; }

private:
    VkImageView ViewFor(VkImage image, VkFormat format);

    sb::Renderer::Device dev;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool in_flight = false;
    std::map<VkImage, VkImageView> views;
    std::string last_error;
};

} // namespace vrui
