// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR DLSS-NR logic (gpu/shadps4/video_core/renderer_vulkan/xr_dlssnr_logic.h): settings
// parsing, pass regions, streams and slots, reset rule. Headless: no Vulkan, no NGX, no game.
#include <cassert>
#include <cstdio>
#include <set>
#include <string>
#include "gpu/shadps4/video_core/renderer_vulkan/xr_dlssnr_logic.h"

using namespace XrDlssNrLogic;

namespace {

void TestDefaults() {
    const Settings s{};
    assert(!s.on); // off unless the player switches it on
    assert(s.passes == 1 && s.style == 0);
    assert(s.intensity == 2.0f && s.tone == 0.6f && s.structure == 1.0f && s.skin == 1.0f);
    assert(s.fovea == 0.66f && s.inner == 0.4f);
    assert(s.place == Place::Post); // the original placement unless asked for
}

void TestApply() {
    Settings s{};
    assert(Apply(s, "dlssnr", "1") && s.on);
    assert(Apply(s, "dlssnr", "0") && !s.on);
    assert(Apply(s, "dlssnr", "on") && s.on);
    assert(Apply(s, "dlssnr", "true") && s.on);
    assert(Apply(s, "dlssnr", "off") && !s.on);
    // Clamped ranges.
    assert(Apply(s, "dlssnr_passes", "2") && s.passes == 2);
    assert(Apply(s, "dlssnr_passes", "9") && s.passes == 3);
    assert(Apply(s, "dlssnr_passes", "0") && s.passes == 1);
    assert(Apply(s, "dlssnr_style", "2") && s.style == 2);
    assert(Apply(s, "dlssnr_style", "7") && s.style == 2);
    assert(Apply(s, "dlssnr_intensity", "1.5") && s.intensity == 1.5f);
    assert(Apply(s, "dlssnr_intensity", "5") && s.intensity == 2.0f);
    assert(Apply(s, "dlssnr_tone", "-1") && s.tone == 0.0f);
    assert(Apply(s, "dlssnr_structure", "0.3") && s.structure == 0.3f);
    assert(Apply(s, "dlssnr_skin", "0.3") && s.skin == 0.3f);
    assert(Apply(s, "dlssnr_skin", "2") && s.skin == 1.0f);
    assert(Apply(s, "dlssnr_fovea", "0.01") && s.fovea == 0.1f);
    assert(Apply(s, "dlssnr_fovea", "1") && s.fovea == 1.0f);
    assert(Apply(s, "dlssnr_inner", "0.5") && s.inner == 0.5f);
    // Not a number: the setting stays.
    assert(Apply(s, "dlssnr_intensity", "strong") && s.intensity == 2.0f);
    // Other keys are not DLSS-NR's.
    Settings before = s;
    assert(!Apply(s, "upscale", "dlss"));
    assert(!Apply(s, "dlss_sharpen", "0.5"));
    assert(s == before);
    // Every environment variable maps to a key Apply knows.
    std::set<std::string> keys;
    for (const auto& [env, key] : EnvKeys) {
        Settings t{};
        assert(Apply(t, key, "1"));
        keys.insert(key);
        assert(std::string(env).rfind("BB_XR_DLSSNR", 0) == 0);
    }
    assert(keys.size() == 10);
}

void TestPlace() {
    Settings s{};
    assert(Apply(s, "dlssnr_place", "pre") && s.place == Place::Pre);
    assert(Apply(s, "dlssnr_place", "post") && s.place == Place::Post);
    assert(Apply(s, "dlssnr_place", "before") && s.place == Place::Pre);
    assert(Apply(s, "dlssnr_place", "after") && s.place == Place::Post);
    assert(Apply(s, "dlssnr_place", "1") && s.place == Place::Pre);
    assert(Apply(s, "dlssnr_place", "0") && s.place == Place::Post);
    // Unknown text: the key is DLSS-NR's, the placement stays.
    assert(Apply(s, "dlssnr_place", "pre") && s.place == Place::Pre);
    assert(Apply(s, "dlssnr_place", "middle") && s.place == Place::Pre);
    assert(Apply(s, "dlssnr_place", "") && s.place == Place::Pre);
    // The placement is part of the settings' identity (a change is logged and noticed).
    Settings a{}, b{};
    b.place = Place::Pre;
    assert(!(a == b));
    assert(std::string(PlaceName(Place::Pre)) == "pre" &&
           std::string(PlaceName(Place::Post)) == "post");
    // Environment variable and key.
    bool found = false;
    for (const auto& [env, key] : EnvKeys) {
        found = found || (std::string(env) == "BB_XR_DLSSNR_PLACE" &&
                          std::string(key) == "dlssnr_place");
    }
    assert(found);
}

void TestPreStreams() {
    FrameInfo f{};
    f.has_camera = true;
    assert(PreStreamFor(f) == 0); // flat / mono
    f.hdr = true;
    assert(PreStreamFor(f) == -1);
    f.hdr = false;
    f.has_camera = false;
    assert(PreStreamFor(f) == -1); // title, menus, loading: never
    // True stereo: every 3D frame, held and flat-screen ones too (eye L's DLSS evaluates them, so
    // its input stays of one kind), unlike the post placement.
    f = {};
    f.has_camera = true;
    f.pair = true;
    f.pair_eyes = false;
    assert(PreStreamFor(f) == 0 && StreamFor(f) == -1);
    f.pair_eyes = true;
    assert(PreStreamFor(f) == 0 && StreamFor(f) == 0);
    f.has_camera = false;
    assert(PreStreamFor(f) == -1);
    // Alternate eyes: as post (one history per eye, held frames not processed: DLSS skips them).
    f = {};
    f.has_camera = true;
    f.alt = true;
    for (int eye : {0, 1, -1, -2}) {
        f.alt_eye = eye;
        assert(PreStreamFor(f) == StreamFor(f));
    }
    // Eye R: its own evaluation only when it is its own image (full stereo) and eye L's ran;
    // the hybrid's eye R is warped from eye L's processed image.
    assert(PreEyeR(true, true));
    assert(!PreEyeR(true, false)); // hybrid: inherited through the warp
    assert(!PreEyeR(false, true)); // eye L not processed (off, HDR, failed): neither eye
    assert(!PreEyeR(false, false));
    // Flat runs (BB_XR_DLSSNR_FLAT_PAIRS=1): 3D pair frames count as pairs; nothing else changes.
    f = {};
    f.has_camera = true;
    f.pair = true;
    assert(!WithFlatPairs(f, false).pair_eyes);
    assert(WithFlatPairs(f, true).pair_eyes && StreamFor(WithFlatPairs(f, true)) == 0);
    f.has_camera = false;
    assert(!WithFlatPairs(f, true).pair_eyes);
    f = {};
    f.has_camera = true; // not true stereo
    assert(!WithFlatPairs(f, true).pair_eyes);
}

void TestPlacementResets() {
    // The placement: the first frame is no change; every switch is one.
    PlaceTracker places;
    assert(!places.Changed(Place::Post));
    assert(!places.Changed(Place::Post));
    assert(places.Changed(Place::Pre));
    assert(!places.Changed(Place::Pre));
    assert(places.Changed(Place::Post));
    PlaceTracker first_pre;
    assert(!first_pre.Changed(Place::Pre)); // started with pre: nothing to reset
    // A placement change resets a history that would otherwise continue (same-sized feature,
    // evaluated a frame ago).
    assert(!NeedsReset(false, 100.0, 113.9, false));
    assert(NeedsReset(false, 100.0, 113.9, true));
    assert(NeedsReset(true, 100.0, 113.9, false)); // the other rules still hold
    assert(NeedsReset(false, 100.0, 400.0, false));

    // DLSS SR's input: processed only when DLSS-NR is on and placed before it.
    Settings s{};
    assert(!SrInputProcessed(s));
    s.on = true;
    assert(!SrInputProcessed(s)); // post: SR sees the game's image
    s.place = Place::Pre;
    assert(SrInputProcessed(s));
    s.on = false;
    assert(!SrInputProcessed(s));
    // SR's history resets whenever that changes: pre on/off, post <-> pre while on; not when
    // the placement changes while DLSS-NR is off, nor for the post placement's toggles.
    SrInputTracker sr;
    Settings t{};
    assert(!sr.Changed(SrInputProcessed(t))); // first frame
    t.on = true;
    assert(!sr.Changed(SrInputProcessed(t))); // post on: SR's input unchanged
    t.place = Place::Pre;
    assert(sr.Changed(SrInputProcessed(t))); // post -> pre while on
    assert(!sr.Changed(SrInputProcessed(t)));
    t.on = false;
    assert(sr.Changed(SrInputProcessed(t))); // pre switched off
    t.place = Place::Post;
    assert(!sr.Changed(SrInputProcessed(t))); // off: the placement alone changes nothing
    t.place = Place::Pre;
    assert(!sr.Changed(SrInputProcessed(t)));
    t.on = true;
    assert(sr.Changed(SrInputProcessed(t))); // pre switched on
    SrInputTracker sr_start;
    assert(!sr_start.Changed(true)); // started with pre on: DLSS starts fresh anyway
}

void TestRegions() {
    // VirtualDesktop-OpenXR's default: the centre 66 % of the eye, multiples of 4.
    const Rect r = CentreRegion(3840, 2160, 0.66f);
    assert(r.width == 2536 && r.height == 1428); // as the smoke test printed
    assert(r.x == (3840 - 2536) / 2 && r.y == (2160 - 1428) / 2);
    assert(r.width % 4 == 0 && r.height % 4 == 0);
    // All of it.
    assert((CentreRegion(3840, 2160, 1.0f) == Rect{0, 0, 3840, 2160}));
    // Never larger than the image, never smaller than MinRegion (when the image allows).
    assert((CentreRegion(1922, 1081, 1.0f) == Rect{0, 0, 1922, 1081}));
    assert(CentreRegion(1000, 1000, 0.01f).width == MinRegion);
    assert(CentreRegion(40, 40, 0.5f).width == 40);

    Settings s{};
    s.passes = 3;
    const Rect p0 = PassRegion(s, 3840, 2160, 0);
    const Rect p1 = PassRegion(s, 3840, 2160, 1);
    const Rect p2 = PassRegion(s, 3840, 2160, 2);
    assert(p0 == r);
    assert(p1 == p2);
    assert((p1 == CentreRegion(3840, 2160, 0.4f)));
    // Later passes lie inside the first (each reads only what the one before wrote).
    assert(p1.x >= p0.x && p1.y >= p0.y && p1.x + p1.width <= p0.x + p0.width &&
           p1.y + p1.height <= p0.y + p0.height);
    // inner above fovea: clipped to the first region.
    s.fovea = 0.5f;
    s.inner = 0.9f;
    assert(PassRegion(s, 3840, 2160, 1) == PassRegion(s, 3840, 2160, 0));
    assert(Passes(Settings{}) == 1);
}

void TestStreams() {
    FrameInfo f{};
    f.has_camera = true;
    assert(StreamFor(f) == 0); // mono / flat
    f.hdr = true;
    assert(StreamFor(f) == -1);
    f.hdr = false;
    f.has_camera = false;
    assert(StreamFor(f) == -1); // title, menus, loading
    f.has_camera = true;
    // Alternate eyes: each eye its own history; held frames not processed.
    f.alt = true;
    f.alt_eye = 0;
    assert(StreamFor(f) == 0);
    f.alt_eye = 1;
    assert(StreamFor(f) == 1);
    f.alt_eye = -1;
    assert(StreamFor(f) == -1);
    f.alt_eye = -2; // mono (flat screen with a scene camera): processed as stream 0
    assert(StreamFor(f) == 0);
    // True stereo: eye L of a pair (eye R runs separately as stream 1).
    f = {};
    f.has_camera = true;
    f.pair = true;
    f.pair_eyes = true;
    assert(StreamFor(f) == 0);
    f.pair_eyes = false;
    assert(StreamFor(f) == -1);
    // Slots: one per stream and pass, all distinct, within the bridge's 6.
    std::set<uint32_t> slots;
    for (int stream = 0; stream < Streams; ++stream) {
        for (int pass = 0; pass < MaxPasses; ++pass) {
            slots.insert(Slot(stream, pass));
        }
    }
    assert(slots.size() == 6 && *slots.rbegin() == 5);
}

void TestFormats() {
    assert(WorkFormat(kB8G8R8A8Srgb) == kB8G8R8A8Unorm);
    assert(WorkFormat(kB8G8R8A8Unorm) == kB8G8R8A8Unorm);
    assert(WorkFormat(kR8G8B8A8Srgb) == kR8G8B8A8Unorm);
    assert(WorkFormat(kR8G8B8A8Unorm) == kR8G8B8A8Unorm);
    assert(WorkFormat(64) == 0); // A2B10G10R10 (HDR swapchains): not given to it
    assert(WorkFormat(97) == 0); // R16G16B16A16_SFLOAT
}

void TestReset() {
    assert(NeedsReset(true, 100.0, 110.0));   // new feature
    assert(NeedsReset(false, -1.0, 110.0));   // never evaluated
    assert(!NeedsReset(false, 100.0, 127.8)); // alternate eyes: two frames at 72 Hz
    assert(NeedsReset(false, 100.0, 400.0));  // a menu or the option off in between
}

} // namespace

int main() {
    TestDefaults();
    TestApply();
    TestRegions();
    TestStreams();
    TestFormats();
    TestReset();
    TestPlace();
    TestPreStreams();
    TestPlacementResets();
    std::printf("xr-dlssnr-test: all passed\n");
    return 0;
}
