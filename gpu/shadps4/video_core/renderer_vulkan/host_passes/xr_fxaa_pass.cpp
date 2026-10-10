// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR (BB_XR_AA=fxaa): see xr_fxaa_pass.h.

#include "common/assert.h"
#include "video_core/host_shaders/xr_fxaa_comp.h"
#include "video_core/renderer_vulkan/host_passes/xr_fxaa_pass.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

namespace {
struct Push {
    float inv_size[2];
    s32 size[2];
    float subpix;
    float edge_threshold;
    float edge_min;
    float pad;
};
} // namespace

void XrFxaaPass::Create(vk::Device device_, VmaAllocator allocator, u32 num_images) {
    device = device_;
    sampler = Check<"create xr fxaa sampler">(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxAnisotropy = 1.0f,
        .minLod = 0.0f,
        .maxLod = 0.0f,
    }));
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .pImmutableSamplers = &sampler.get(),
        },
    };
    set_layout = Check<"create xr fxaa set layout">(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Push),
    };
    pipeline_layout = Check<"create xr fxaa layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout.get(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto module = CompileSPV(XR_FXAA_COMP, device);
    ASSERT(module);
    pipeline = Check<"create xr fxaa pipeline">(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage{
                    .stage = vk::ShaderStageFlagBits::eCompute,
                    .module = module,
                    .pName = "main",
                },
                .layout = pipeline_layout.get(),
            }));
    device.destroyShaderModule(module);
    images.resize(num_images);
    for (auto& img : images) {
        img.image = VideoCore::UniqueImage(device, allocator);
    }
}

vk::ImageView XrFxaaPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D size,
                                 const Settings& settings) {
    auto& img = images[next];
    next = (next + 1) % u32(images.size());
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    if (img.size != size || !img.view) {
        img.view.reset();
        img.image.Destroy();
        img.image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = {size.width, size.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        SetObjectName(device, static_cast<vk::Image>(img.image), "XR FXAA image");
        img.view = Check<"create xr fxaa view">(device.createImageViewUnique({
            .image = img.image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = range,
        }));
        img.size = size;
    }
    const vk::ImageMemoryBarrier2 to_write{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = img.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
    const std::array infos{
        vk::DescriptorImageInfo{.imageView = input,
                                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{.imageView = img.view.get(),
                                .imageLayout = vk::ImageLayout::eGeneral},
        vk::DescriptorImageInfo{.sampler = sampler.get()},
    };
    const std::array writes{
        vk::WriteDescriptorSet{.dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eSampledImage,
                               .pImageInfo = &infos[0]},
        vk::WriteDescriptorSet{.dstBinding = 1,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eStorageImage,
                               .pImageInfo = &infos[1]},
        vk::WriteDescriptorSet{.dstBinding = 2,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eSampler,
                               .pImageInfo = &infos[2]},
    };
    const Push push{
        .inv_size = {1.0f / float(size.width), 1.0f / float(size.height)},
        .size = {s32(size.width), s32(size.height)},
        .subpix = settings.subpix,
        .edge_threshold = settings.edge_threshold,
        .edge_min = settings.edge_min,
        .pad = 0.0f,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.get());
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0, writes);
    cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push),
                         &push);
    cmdbuf.dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
    const vk::ImageMemoryBarrier2 to_read{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = img.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    return img.view.get();
}

} // namespace Vulkan::HostPasses
