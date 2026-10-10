// SPDX-FileCopyrightText: Copyright 2026 IFreemz, bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport: NVIDIA DLSS Super Resolution through the optional bbport_dlss.dll bridge (gpu/
// dlss_bridge, MIT), loaded at run time from next to bb-probe.exe together with NVIDIA's
// nvngx_dlss.dll. The port itself contains no NVIDIA code; without the DLLs, on other GPUs or
// with BB_DLSS=0 nothing changes. Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR
// (vk_dlss_ngx).

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Dlss {
public:
    /// The process-wide instance, or null when the DLLs are absent or BB_DLSS=0.
    static Dlss* Get();
    ~Dlss();

    /// Instance and device creation: adds what NGX needs (or disables DLSS when it is missing).
    void AppendInstanceExtensions(std::vector<const char*>& enabled);
    void AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                std::vector<const char*>& enabled);
    /// After the device exists.
    void Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device);
    void Shutdown();

    [[nodiscard]] bool Available() const;
    /// Why DLSS cannot run on this system (empty when it can, or before device creation).
    [[nodiscard]] std::string Problem() const;

    struct Resource {
        vk::Image image;
        vk::ImageView view;
        vk::Format format;
        vk::ImageAspectFlags aspect;
        u32 width, height;
    };
    struct FeatureDesc {
        u32 input_width, input_height, output_width, output_height;
        int quality;
        bool hdr;
        u32 preset = 0; ///< NVSDK_NGX_DLSS_Hint_Render_Preset (0 = driver default)
        bool operator==(const FeatureDesc&) const = default;
    };
    /// bbport VR (vk_xr_dlss.h): independent features with their own history. Slot 0 is the flat
    /// game's (the functions below default to it), 1 and 2 the VR eyes.
    static constexpr u32 FlatSlot = 0, EyeSlot0 = 1, Slots = 4;
    /// The bridge supports feature slots other than 0 (bridge ABI 2).
    [[nodiscard]] bool HasSlots() const;
    /// The bridge takes a bias mask (bridge ABI 3); older bridges ignore it.
    [[nodiscard]] bool HasBiasMask() const;
    struct Frame {
        Resource color, depth, motion, output;
        float jitter_x, jitter_y;
        bool reset;
        float frame_ms;
        float sharpness;
        Resource bias_mask{}; ///< optional (null view = none), R8, bridge ABI 3: 1 = trust current colour
    };

    /// DLSS quality mode for an output/render size ratio (0 DLAA .. 4 Ultra Performance).
    static int QualityForScale(float scale);
    [[nodiscard]] bool HasFeature(const FeatureDesc& desc, u32 slot = FlatSlot) const;
    /// Records the feature creation. The GPU must be done with the previous one.
    bool CreateFeature(vk::CommandBuffer command, const FeatureDesc& desc, u32 slot = FlatSlot);
    bool Evaluate(vk::CommandBuffer command, const Frame& frame, u32 slot = FlatSlot);
    void ReleaseFeature(u32 slot = FlatSlot);

private:
    Dlss();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
