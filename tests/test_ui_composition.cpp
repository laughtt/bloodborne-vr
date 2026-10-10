// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/ui_composition.h"

int main() {
    using namespace Vulkan::UiComposition;
    // Title/loading menus have no depth/camera. UI must still go to the native target.
    assert(Choose(true, true, false, true) == Background::Copy);
    assert(Choose(true, true, false, false) == Background::Copy);
    // In-game HUD: first reconstruct the scene, then draw the native UI over it.
    assert(Choose(true, true, true, true) == Background::Temporal);
    // Runtime FSR disable/failure must not change UI resolution.
    assert(Choose(true, true, true, false) == Background::Copy);
    assert(Choose(true, false, true, true) == Background::None);
    assert(Choose(false, true, true, true) == Background::None);
    assert(NativeViewport(1920, -1080));
    assert(!NativeViewport(960, -540));
    assert(MovieShader(0x34e8a281) && MovieShader(0x24042a9b));
    assert(!MovieShader(0x0b0acf50)); // fullscreen post/tonemap also has a native viewport
    assert(!MovieShader(0x6c62a79f)); // shadow geometry
    const auto display = Scale(960, 540, 1920, 1080, false);
    assert(display[0] == 2 && display[1] == 2);
    const auto ui = Scale(960, 540, 1920, 1080, true);
    assert(ui[0] == 1 && ui[1] == 1); // don't double an already native UI viewport
    const auto larger = Scale(960, 540, 3840, 2160, true);
    assert(larger[0] == 2 && larger[1] == 2);
    // VR HUD layer: coverage alpha for "over" and additive UI blends, nothing else touched.
    AlphaBlend blend{};
    assert(HudAlpha(kOneMinusSrcAlpha, kAdd, &blend) && blend.src == kOne &&
           blend.dst == kOneMinusSrcAlpha && blend.func == kAdd);
    assert(HudAlpha(kOne, kAdd, &blend) && blend.src == kZero && blend.dst == kOne);
    assert(!HudAlpha(kZero, kAdd, &blend));          // replace
    assert(!HudAlpha(kOneMinusSrcAlpha, 4, &blend)); // reverse subtract
    // Coverage of the separated UI: a HUD in the corners is not full-screen, a loading screen is.
    uint8_t probes[100 * 4] = {};
    for (int i = 0; i < 100; ++i) {
        probes[i * 4 + 3] = i < 8 ? 255 : 0; // 8% HUD
    }
    assert(!FullScreenUi(probes, 100));
    for (int i = 0; i < 100; ++i) {
        probes[i * 4 + 3] = i < 95 ? 250 : 100; // opaque loading screen, a few soft texels
    }
    assert(FullScreenUi(probes, 100));
    assert(!FullScreenUi(probes, 0));
    FullScreenLatch latch;
    assert(!latch.Update(true) && latch.Update(true));             // on after 2
    assert(latch.Update(false) && latch.Update(true));             // one gap does not leave
    assert(latch.Update(false) && latch.Update(false) && latch.Update(false));
    assert(!latch.Update(false));                                  // off after 4
    std::puts("UI composition: PASS (menu without camera, HUD, FSR off, viewport scaling, "
              "VR HUD alpha, full-screen UI)");
}
