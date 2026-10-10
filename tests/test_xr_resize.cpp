// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR resize bookkeeping (gpu/shadps4/video_core/renderer_vulkan/xr_resize_logic.h): the
// frame size debounce, the resize reason, the swapchain size tracking. Headless.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "gpu/shadps4/video_core/renderer_vulkan/xr_resize_logic.h"

using namespace XrResize;

namespace {

const Size kWindow{1920, 1080};
const Size kEye{2880, 1620};

void TestPack() {
    const Size s{3840, 2160};
    assert(Unpack(Pack(s)) == s);
    assert(Unpack(Pack({})) == Size{});
}

void TestTarget() {
    Inputs in{kWindow, {}, kWindow};
    assert(Target(in) == kWindow);
    in.iq = kEye;
    assert(Target(in) == kEye);
}

void TestFirstAppliesAtOnce() {
    SizeDebounce d{3};
    assert(!d.Update({})); // minimized window: nothing
    assert(!d.Applied().Valid());
    assert(d.Update(kEye));
    assert(d.Applied() == kEye);
}

void TestOneFrameFlipIgnored() {
    // The crash pattern: 2880x1620 per eye, one present computes 1920x1080, then 2880x1620.
    SizeDebounce d{3};
    d.Update(kEye);
    for (int i = 0; i < 100; ++i) {
        assert(!d.Update(kEye));
    }
    assert(!d.Update(kWindow));
    assert(d.Applied() == kEye);
    assert(d.Pending() == 1);
    assert(!d.Update(kEye));
    assert(d.Applied() == kEye);
    assert(d.Pending() == 0);
    // Two-frame flips too.
    assert(!d.Update(kWindow));
    assert(!d.Update(kWindow));
    assert(!d.Update(kEye));
    assert(d.Applied() == kEye);
}

void TestStableChangeApplies() {
    SizeDebounce d{3};
    d.Update(kEye);
    const Size uhd{3840, 2160};
    assert(!d.Update(uhd));
    assert(!d.Update(uhd));
    assert(d.Update(uhd)); // third present in a row
    assert(d.Applied() == uhd);
    assert(!d.Update(uhd));
}

void TestAlternatingNeverApplies() {
    SizeDebounce d{3};
    d.Update(kEye);
    const Size a{1000, 500}, b{1200, 600};
    for (int i = 0; i < 20; ++i) {
        assert(!d.Update(i & 1 ? a : b));
    }
    assert(d.Applied() == kEye);
}

void TestMinimizedKeepsSize() {
    SizeDebounce d{3};
    d.Update(kEye);
    for (int i = 0; i < 10; ++i) {
        assert(!d.Update({0, 0}));
    }
    assert(d.Applied() == kEye);
    // A minimized gap does not reset a pending change either way.
    assert(!d.Update(kWindow));
    assert(!d.Update({}));
    assert(!d.Update(kWindow));
    assert(d.Update(kWindow));
}

void TestStableOne() {
    SizeDebounce d{0}; // treated as 1: every change applies at once
    d.Update(kEye);
    assert(d.Update(kWindow));
    assert(d.Applied() == kWindow);
}

void TestReason() {
    const Inputs eye{kWindow, kEye, kWindow};
    Inputs off = eye;
    off.iq = {};
    assert(!std::strcmp(ChangeReason(eye, off), "eye IQ upscale switched off"));
    assert(!std::strcmp(ChangeReason(off, eye), "eye IQ upscale switched on"));
    Inputs src = eye;
    src.source = {1280, 720};
    src.iq = kWindow;
    assert(!std::strcmp(ChangeReason(eye, src), "display buffer size"));
    Inputs res = eye;
    res.iq = {3840, 2160};
    assert(!std::strcmp(ChangeReason(eye, res), "eye IQ settings (res / dlss_quality)"));
    Inputs win = off;
    win.window = {2560, 1440};
    assert(!std::strcmp(ChangeReason(off, win), "window size"));
}

void TestChainSize() {
    ChainSize c;
    assert(!c.NeedsWait(kEye)); // first swapchain: nothing written before
    c.Acquired(kEye);
    assert(!c.NeedsWait(kEye));
    assert(c.NeedsWait(kWindow));
    assert(!c.NeedsWait({})); // no size: no recreation
    c.Acquired(kWindow);
    assert(!c.NeedsWait(kWindow));
    assert(c.Last() == kWindow);
}

} // namespace

int main() {
    TestPack();
    TestTarget();
    TestFirstAppliesAtOnce();
    TestOneFrameFlipIgnored();
    TestStableChangeApplies();
    TestAlternatingNeverApplies();
    TestMinimizedKeepsSize();
    TestStableOne();
    TestReason();
    TestChainSize();
    std::printf("xr-resize-test: ok\n");
    return 0;
}
