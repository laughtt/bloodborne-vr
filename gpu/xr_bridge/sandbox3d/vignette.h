// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The comfort vignette (docs/vr/findings/19-dash-locomotion.md, 11 section 4.3): darkens the edge
// of the view while the camera dashes. A pass of its own after the scene, over each eye image the
// bridge hands out: a grid over the eye's whole frustum whose vertices carry how much light goes
// through (1 inside `inner` degrees from the head's forward axis, 1 - strength beyond `outer`),
// blended by multiplication. Both eyes measure the angle from the same head axis, so the dark
// edge sits at the same place in the world for both (canted Pimax views included). It reuses the
// debug-line shaders (shaders/line.*), so it needs no shader of its own.

#pragma once

#include <map>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "math3d.h"
#include "renderer.h"

namespace sb {

class Vignette {
public:
    struct Eye {
        VkImage color = VK_NULL_HANDLE; // COLOR_ATTACHMENT_OPTIMAL, left so
        uint32_t width = 0, height = 0;
        float fov[4] = {};    // left, right, up, down: radians from the eye's axis (left, down < 0)
        m3::Quat eye_in_head; // the eye's orientation relative to the head (tracking space)
    };
    bool Create(const Renderer::Device& device, std::string& error);
    void Destroy();
    /// Draws into both eye images and waits. Nothing is drawn below a strength of 0.005.
    bool Draw(const Eye eyes[2], VkFormat color_format, float strength, float inner_deg, float outer_deg);
    const std::string& LastError() const { return last_error; }

    /// How much light goes through in a direction `angle` radians off the head's axis (also for
    /// the offline previews).
    static float Transmission(float angle, float strength, float inner, float outer);

private:
    VkPipeline PipelineFor(VkFormat color);
    VkImageView ViewFor(VkImage image, VkFormat format);

    Renderer::Device dev;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule shaders[2] = {};
    std::map<VkFormat, VkPipeline> pipelines;
    std::map<VkImage, VkImageView> views;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    std::string last_error;
};

} // namespace sb
