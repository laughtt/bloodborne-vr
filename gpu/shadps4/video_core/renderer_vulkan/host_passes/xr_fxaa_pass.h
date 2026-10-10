// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR (BB_XR_AA=fxaa): spatial anti-aliasing of the display image (host_shaders/xr_fxaa.comp),
// before FSR1 and the post pass. No history, so alternate-eye stereo can use it.

#pragma once

#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::HostPasses {

class XrFxaaPass {
public:
    struct Settings {
        float subpix{0.75f};
        float edge_threshold{0.125f};
        float edge_min{0.0312f};
    };

    void Create(vk::Device device, VmaAllocator allocator, u32 num_images);

    /// `input` (eShaderReadOnlyOptimal, linear values) -> an RGBA16F image of the same size, left in
    /// eShaderReadOnlyOptimal; returns its view.
    vk::ImageView Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D size,
                         const Settings& settings);

private:
    struct Img {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::Extent2D size{};
    };

    vk::Device device{};
    vk::UniqueSampler sampler{};
    vk::UniqueDescriptorSetLayout set_layout{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniquePipeline pipeline{};
    std::vector<Img> images;
    u32 next{};
};

} // namespace Vulkan::HostPasses
