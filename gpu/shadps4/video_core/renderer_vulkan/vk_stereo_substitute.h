// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: VR Phase 4 true stereo, P4.4 slice 3 (docs/vr/findings/31-true-stereo.md section 5.3).
// Eye R's descriptors for commands recorded once and executed twice (Scheduler::BeginCapture):
// the push-descriptor command of a capture carries a small table of (guest image view or buffer
// range -> twin) pairs, and its second execution (RecordArena::ExecutingEyeR) pushes a copy of its
// writes with those replaced. Header-only and device-free (tests/test_stereo_substitute.cpp).

#pragma once

#include <span>

#include <boost/container/small_vector.hpp>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::Stereo {

/// One eye-R replacement. An image entry (guest_view set) swaps a sampled or storage image view
/// and its layout; a buffer entry (guest_buffer set) swaps the buffer of a descriptor with exactly
/// that buffer and offset, keeping its range.
struct DescriptorSubstitute {
    vk::ImageView guest_view{};
    vk::ImageView twin_view{};
    vk::ImageLayout twin_layout{};
    vk::Buffer guest_buffer{};
    vk::DeviceSize guest_offset{};
    vk::Buffer twin_buffer{};
    vk::DeviceSize twin_offset{};
};

/// A copy of descriptor writes with the substitutes applied (the originals are not touched).
class SubstitutedWrites {
public:
    /// Returns how many descriptors were replaced.
    u32 Build(std::span<const vk::WriteDescriptorSet> source,
              std::span<const DescriptorSubstitute> subs) {
        size_t image_count = 0, buffer_count = 0;
        for (const auto& write : source) {
            image_count += write.pImageInfo ? write.descriptorCount : 0;
            buffer_count += write.pBufferInfo ? write.descriptorCount : 0;
        }
        writes.assign(source.begin(), source.end());
        images.clear();
        buffers.clear();
        images.reserve(image_count); // no reallocation below: the writes point into these
        buffers.reserve(buffer_count);
        u32 replaced = 0;
        for (auto& write : writes) {
            if (write.pImageInfo) {
                const size_t first = images.size();
                for (u32 i = 0; i < write.descriptorCount; ++i) {
                    auto info = write.pImageInfo[i];
                    if (info.imageView && (write.descriptorType == vk::DescriptorType::eSampledImage ||
                                           write.descriptorType == vk::DescriptorType::eStorageImage ||
                                           write.descriptorType ==
                                               vk::DescriptorType::eCombinedImageSampler)) {
                        for (const auto& sub : subs) {
                            if (sub.guest_view && sub.guest_view == info.imageView) {
                                info.imageView = sub.twin_view;
                                info.imageLayout = sub.twin_layout;
                                ++replaced;
                                break;
                            }
                        }
                    }
                    images.push_back(info);
                }
                write.pImageInfo = images.data() + first;
            }
            if (write.pBufferInfo) {
                const size_t first = buffers.size();
                for (u32 i = 0; i < write.descriptorCount; ++i) {
                    auto info = write.pBufferInfo[i];
                    if (info.buffer) {
                        for (const auto& sub : subs) {
                            if (sub.guest_buffer && sub.guest_buffer == info.buffer &&
                                sub.guest_offset == info.offset) {
                                info.buffer = sub.twin_buffer;
                                info.offset = sub.twin_offset;
                                ++replaced;
                                break;
                            }
                        }
                    }
                    buffers.push_back(info);
                }
                write.pBufferInfo = buffers.data() + first;
            }
        }
        return replaced;
    }

    [[nodiscard]] std::span<vk::WriteDescriptorSet> Writes() noexcept {
        return writes;
    }

private:
    boost::container::small_vector<vk::WriteDescriptorSet, 32> writes;
    boost::container::small_vector<vk::DescriptorImageInfo, 32> images;
    boost::container::small_vector<vk::DescriptorBufferInfo, 32> buffers;
};

/// The guest-visible writes of a dispatch inside the doubled region, by kind.
struct DispatchWrites {
    u32 twin_targets = 0;   ///< storage images of the region's size (twinned), twin buffer ranges
    u32 shared_buffers = 0; ///< other buffer writes (exposure, CS_VFX_* particles, counters)
    u32 shared_gds = 0;     ///< GDS (counters, append/consume)
    u32 shared_images = 0;  ///< storage images not of the region's size or not twinnable (LUT)
};

enum class DispatchPlan {
    Once,     ///< reads no twin: one execution serves both eyes (written twins go stale)
    EyeLOnly, ///< reads twins but writes shared memory: eye L only, both eyes see its result
    BothEyes, ///< captured and executed again for eye R with its descriptors
};

/// The write policy (P4.4 slice 3). `guard_test` is the write guard's negative control: every
/// dispatch that would be executed twice counts as writing shared memory.
constexpr DispatchPlan PlanDispatch(bool reads_twins, const DispatchWrites& writes,
                                    bool guard_test) {
    if (!reads_twins) {
        return DispatchPlan::Once;
    }
    if (guard_test || writes.shared_buffers || writes.shared_gds || writes.shared_images) {
        return DispatchPlan::EyeLOnly;
    }
    return DispatchPlan::BothEyes;
}

} // namespace Vulkan::Stereo
