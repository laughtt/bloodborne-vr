// SPDX-License-Identifier: GPL-2.0-or-later
// VR true stereo, P4.4 slice 3 (docs/vr/findings/31-true-stereo.md section 5.3): eye R's
// descriptor substitution, the arena's eye flag and the dispatch write policy. No Vulkan device:
// handles are plain values and the command buffer is never called.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_stereo_region.h"
#include "video_core/renderer_vulkan/vk_stereo_substitute.h"

namespace {

using namespace Vulkan;
using namespace Vulkan::Stereo;

template <typename Handle>
Handle H(std::uintptr_t value) {
    return Handle(reinterpret_cast<typename Handle::CType>(value));
}

void Substitution() {
    const auto guest_a = H<vk::ImageView>(0x100), twin_a = H<vk::ImageView>(0x200);
    const auto guest_s = H<vk::ImageView>(0x110), twin_s = H<vk::ImageView>(0x210);
    const auto other = H<vk::ImageView>(0x120);
    const auto guest_buf = H<vk::Buffer>(0x300), twin_buf = H<vk::Buffer>(0x400);
    const auto sampler = H<vk::Sampler>(0x500);

    const std::vector<vk::DescriptorBufferInfo> buffers{
        {guest_buf, 256, 64},  // inside the twin buffer: substituted
        {guest_buf, 512, 64},  // same buffer, other offset: kept
        {H<vk::Buffer>(0x310), 256, 64},
    };
    const std::vector<vk::DescriptorImageInfo> sampled{
        {{}, guest_a, vk::ImageLayout::eShaderReadOnlyOptimal},
        {{}, other, vk::ImageLayout::eShaderReadOnlyOptimal},
    };
    const std::vector<vk::DescriptorImageInfo> storage{
        {{}, guest_s, vk::ImageLayout::eGeneral},
    };
    const std::vector<vk::DescriptorImageInfo> samplers{
        {sampler, {}, vk::ImageLayout::eGeneral},
    };
    std::vector<vk::WriteDescriptorSet> writes(5);
    writes[0] = {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &buffers[0]};
    writes[1] = {.dstBinding = 1, .descriptorCount = 2,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &buffers[1]};
    // A descriptor array (mip fallback): only its first element is a twinned view.
    writes[2] = {.dstBinding = 2, .descriptorCount = 2,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = sampled.data()};
    writes[3] = {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = storage.data()};
    writes[4] = {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampler, .pImageInfo = samplers.data()};

    const std::vector<DescriptorSubstitute> subs{
        {.guest_view = guest_a, .twin_view = twin_a, .twin_layout = vk::ImageLayout::eGeneral},
        {.guest_view = guest_s, .twin_view = twin_s, .twin_layout = vk::ImageLayout::eGeneral},
        {.guest_buffer = guest_buf, .guest_offset = 256, .twin_buffer = twin_buf,
         .twin_offset = 0},
    };
    SubstitutedWrites eye_r;
    const u32 replaced = eye_r.Build(writes, subs);
    assert(replaced == 3);
    const auto out = eye_r.Writes();
    assert(out.size() == writes.size());
    for (size_t i = 0; i < out.size(); ++i) {
        assert(out[i].dstBinding == writes[i].dstBinding);
        assert(out[i].descriptorCount == writes[i].descriptorCount);
        assert(out[i].descriptorType == writes[i].descriptorType);
    }
    // Buffers: the twin's range at the twin offset; the rest unchanged.
    assert(out[0].pBufferInfo[0].buffer == twin_buf && out[0].pBufferInfo[0].offset == 0 &&
           out[0].pBufferInfo[0].range == 64);
    assert(out[1].pBufferInfo[0].buffer == guest_buf && out[1].pBufferInfo[0].offset == 512);
    assert(out[1].pBufferInfo[1].buffer == H<vk::Buffer>(0x310));
    // Sampled and storage images: the twin's view in GENERAL; other views and samplers kept.
    assert(out[2].pImageInfo[0].imageView == twin_a &&
           out[2].pImageInfo[0].imageLayout == vk::ImageLayout::eGeneral);
    assert(out[2].pImageInfo[1].imageView == other &&
           out[2].pImageInfo[1].imageLayout == vk::ImageLayout::eShaderReadOnlyOptimal);
    assert(out[3].pImageInfo[0].imageView == twin_s);
    assert(out[4].pImageInfo[0].sampler == sampler && !out[4].pImageInfo[0].imageView);
    // The originals (eye L's) are untouched.
    assert(writes[2].pImageInfo[0].imageView == guest_a && buffers[0].buffer == guest_buf);
    // The copy owns its infos: rebuilding does not move what an earlier build pointed at.
    SubstitutedWrites none;
    assert(none.Build(writes, {}) == 0);
    assert(none.Writes()[2].pImageInfo[0].imageView == guest_a);
    assert(none.Writes()[2].pImageInfo != writes[2].pImageInfo);
    std::puts("substitution: sampled, storage, buffer, arrays, samplers: OK");
}

void ArenaEyeFlag() {
    RecordArena arena;
    std::vector<int> seen;
    arena.Push([&seen](vk::CommandBuffer) { seen.push_back(RecordArena::ExecutingEyeR()); });
    arena.Push([&seen](vk::CommandBuffer) { seen.push_back(10 + RecordArena::ExecutingEyeR()); });
    assert(!RecordArena::ExecutingEyeR());
    arena.Execute(vk::CommandBuffer{});       // eye L, in place
    arena.Execute(vk::CommandBuffer{}, true); // eye R, the same commands
    assert(!RecordArena::ExecutingEyeR());    // restored after the execution
    assert((seen == std::vector<int>{0, 10, 1, 11}));
    assert(arena.Commands() == 2);
    // Nested: an arena executed by a command of an eye-R execution keeps the outer flag after.
    RecordArena inner;
    bool inner_seen = true;
    inner.Push([&inner_seen](vk::CommandBuffer) { inner_seen = RecordArena::ExecutingEyeR(); });
    RecordArena outer;
    bool after = false;
    outer.Push([&inner](vk::CommandBuffer c) { inner.Execute(c); });
    outer.Push([&after](vk::CommandBuffer) { after = RecordArena::ExecutingEyeR(); });
    outer.Execute(vk::CommandBuffer{}, true);
    assert(!inner_seen && after);
    std::puts("arena: eye flag per execution: OK");
}

void WritePolicy() {
    constexpr DispatchWrites none{};
    constexpr DispatchWrites targets{.twin_targets = 2};
    static_assert(PlanDispatch(false, none, false) == DispatchPlan::Once);
    static_assert(PlanDispatch(false, DispatchWrites{.shared_buffers = 1}, false) ==
                  DispatchPlan::Once);
    static_assert(PlanDispatch(true, none, false) == DispatchPlan::BothEyes);
    static_assert(PlanDispatch(true, targets, false) == DispatchPlan::BothEyes);
    // Any guest-visible write besides twins: eye L only (exposure, CS_VFX_*, counters, LUT, GDS).
    static_assert(PlanDispatch(true, DispatchWrites{.twin_targets = 1, .shared_buffers = 1},
                               false) == DispatchPlan::EyeLOnly);
    static_assert(PlanDispatch(true, DispatchWrites{.shared_gds = 1}, false) ==
                  DispatchPlan::EyeLOnly);
    static_assert(PlanDispatch(true, DispatchWrites{.shared_images = 1}, false) ==
                  DispatchPlan::EyeLOnly);
    // The negative control: nothing would run twice.
    static_assert(PlanDispatch(true, targets, true) == DispatchPlan::EyeLOnly);
    static_assert(PlanDispatch(false, none, true) == DispatchPlan::Once);
    std::puts("write policy: OK");
}

// Full stereo (findings/31 section 10): which draws of a frame run for both eyes.
struct Draw {
    bool boundary, display, gbuffer, movie;
};
constexpr Draw Shadow{false, false, false, false};
constexpr Draw GBuf{false, false, true, false};
constexpr Draw Post{false, false, false, false};
constexpr Draw Ui{false, false, false, true};
constexpr Draw DisplayFirst{true, true, false, false};
constexpr Draw DisplayMore{false, true, false, false};

std::vector<FullRegion::Step> Run(FullRegion& region, const std::vector<Draw>& draws,
                                  bool allowed = true) {
    std::vector<FullRegion::Step> steps;
    for (const auto& d : draws) {
        steps.push_back(region.OnDraw(d.boundary, d.display, d.gbuffer, d.movie, allowed));
    }
    return steps;
}

void FullRegionSequence() {
    using Phase = FullRegion::Phase;
    FullRegion region;
    // A frame: shadows (once), G-buffer .. post (both eyes), UI (once), display pass (both eyes),
    // then the next frame's shadows finish it.
    const auto steps = Run(region, {Shadow, GBuf, GBuf, Post, Post, Ui, Ui, Post, DisplayFirst,
                                    DisplayMore, Shadow, GBuf});
    const std::vector<bool> doubled{false, true, true, true, true, false, false, false, true,
                                    true, false, true};
    for (size_t i = 0; i < steps.size(); ++i) {
        assert(steps[i].doubled == doubled[i]);
    }
    assert(steps[1].start && !steps[2].start);       // the region starts at the first G-buffer draw
    assert(!steps[9].finish && steps[10].finish);    // finished by the first non-display draw
    assert(steps[11].start);                          // the next frame's region
    assert(region.Current() == Phase::Scene);
    // A frame without UI: the display pass still ends the region and is doubled.
    FullRegion no_ui;
    const auto s2 = Run(no_ui, {GBuf, Post, DisplayFirst, Shadow});
    assert(s2[2].doubled && s2[3].finish && !s2[3].doubled);
    // Back-to-back display passes (a repeated frame): the second one is not the region's.
    FullRegion repeat;
    const auto s3 = Run(repeat, {GBuf, DisplayFirst, DisplayFirst, DisplayMore});
    assert(s3[1].doubled && s3[2].finish && !s3[2].doubled && !s3[3].doubled);
    assert(repeat.Current() == Phase::Idle);
    // Not allowed (stereo off, oracle offset, reduced raster): nothing is doubled.
    FullRegion off;
    for (const auto& s : Run(off, {GBuf, Post, Ui, DisplayFirst}, false)) {
        assert(!s.doubled && !s.start && !s.finish);
    }
    // A later G-buffer-like draw in the UI phase does not restart the region; a menu frame (UI,
    // display, no G-buffer) stays idle.
    FullRegion ui_gbuf;
    const auto s4 = Run(ui_gbuf, {GBuf, Ui, GBuf, DisplayFirst, Ui, DisplayFirst});
    assert(!s4[2].start && !s4[2].doubled && s4[3].doubled);
    assert(s4[4].finish && !s4[4].doubled && !s4[5].doubled && ui_gbuf.Current() == Phase::Idle);
    // Reset (a flip finishes the display pass first): the next frame starts clean.
    FullRegion flip;
    Run(flip, {GBuf, Post, DisplayFirst});
    assert(flip.Current() == Phase::Display);
    flip.Reset();
    const auto s5 = Run(flip, {Shadow, GBuf});
    assert(!s5[0].finish && s5[1].start);
    std::puts("full region: draw sequence: OK");
}

void TwinRules() {
    // Current: written by eye R in this region; history: last region's, full stereo only.
    static_assert(ClassifyTwin(7, 7, false) == TwinUse::Current);
    static_assert(ClassifyTwin(7, 7, true) == TwinUse::Current);
    static_assert(ClassifyTwin(6, 7, true) == TwinUse::History);
    static_assert(ClassifyTwin(6, 7, false) == TwinUse::None);  // hybrid: no history
    static_assert(ClassifyTwin(5, 7, true) == TwinUse::None);   // two regions old
    static_assert(ClassifyTwin(8, 7, true) == TwinUse::None);   // a newer region (never expected)
    static_assert(ClassifyTwin(NoFrame, 7, true) == TwinUse::None); // stale (eye L wrote it alone)
    static_assert(ClassifyTwin(NoFrame, 0, true) == TwinUse::None);
    static_assert(ClassifyTwin(NoFrame - 1, NoFrame, true) == TwinUse::History);
    // Storage targets of dispatches: plain 2D color images from 64x64 get twins.
    static_assert(StorageTwinnable(1920, 1080, 1, 1, 0, 0, true, true, true, true));
    static_assert(StorageTwinnable(960, 540, 1, 1, 0, 0, true, true, true, true));
    static_assert(StorageTwinnable(64, 64, 1, 1, 0, 0, true, true, true, true));
    static_assert(!StorageTwinnable(2048, 2, 1, 1, 0, 0, true, true, true, true)); // exposure
    static_assert(!StorageTwinnable(32, 32, 1, 1, 0, 0, true, true, true, true));
    static_assert(!StorageTwinnable(256, 256, 1, 1, 0, 0, false, true, true, true)); // 3D LUT
    static_assert(!StorageTwinnable(960, 540, 1, 1, 1, 0, true, true, true, true));  // a mip
    static_assert(!StorageTwinnable(960, 540, 2, 1, 0, 0, true, true, true, true));
    static_assert(!StorageTwinnable(960, 540, 1, 1, 0, 0, true, false, true, true)); // depth
    static_assert(!StorageTwinnable(960, 540, 1, 1, 0, 0, true, true, false, true)); // MSAA
    static_assert(!StorageTwinnable(960, 540, 1, 1, 0, 0, true, true, true, false)); // format
    // Render targets: eye R renders into a twin of mip 0, layer 0, single-sampled.
    static_assert(AttachmentTwinnable(0, 0, 1, 1));
    static_assert(!AttachmentTwinnable(1, 0, 1, 1));
    static_assert(!AttachmentTwinnable(0, 1, 1, 1));
    static_assert(!AttachmentTwinnable(0, 0, 6, 1));
    static_assert(!AttachmentTwinnable(0, 0, 1, 4));
    std::puts("full stereo: twin rules: OK");
}

} // namespace

int main() {
    Substitution();
    ArenaEyeFlag();
    WritePolicy();
    FullRegionSequence();
    TwinRules();
    std::puts("stereo-substitute-test: all passed");
    return 0;
}
