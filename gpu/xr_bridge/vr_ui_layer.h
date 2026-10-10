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

/// An extra target drawn in the same submission (the desktop mirror, vr_mirror_ui.h).
struct MirrorTarget {
    VkImage image;
    uint32_t width, height; // the used part of the image
    m3::Mat4 view_proj;
};

class UiLayerGpu {
public:
    bool Create(const sb::Renderer::Device& device, std::string& error);
    void Destroy();
    /// Waits for the previous submission (at most timeout_ns); false while it still runs.
    bool Wait(uint64_t timeout_ns);
    /// Records and submits the UI into color[eye] (COLOR_ATTACHMENT_OPTIMAL in and out). The
    /// submission is not waited for.
    bool Render(GameUi& ui, const VkImage color[2], VkFormat format, uint32_t width,
                uint32_t height, const m3::Mat4 view_proj[2],
                const MirrorTarget* mirror = nullptr);
    /// Creates the 3 mirror images of this size (idempotent); slot < 3 names one afterwards.
    bool EnsureMirror(VkFormat format, uint32_t width, uint32_t height);
    VkImage MirrorImage(int slot) const { return mirror_images[slot].image; }
    uint32_t MirrorWidth() const { return mirror_w; }
    uint32_t MirrorHeight() const { return mirror_h; }
    const std::string& LastError() const { return last_error; }

private:
    VkImageView ViewFor(VkImage image, VkFormat format);
    void DestroyMirror();
    struct MirrorImage_ {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    MirrorImage_ mirror_images[3];
    VkFormat mirror_format = VK_FORMAT_UNDEFINED;
    uint32_t mirror_w = 0, mirror_h = 0;

    sb::Renderer::Device dev;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool in_flight = false;
    std::map<VkImage, VkImageView> views;
    std::string last_error;
};

} // namespace vrui
