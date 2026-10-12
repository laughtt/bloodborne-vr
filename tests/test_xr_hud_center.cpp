// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR HUD centre gate (gpu/shadps4/video_core/renderer_vulkan/xr_hud_center.h): the mode
// words, the message detector (lit probes outside the always-on HUD zones) and the show/hold
// decision. Headless.
#include <cassert>
#include <cstdio>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/xr_hud_center.h"

using namespace XrHudCenter;

namespace {

std::vector<uint8_t> Blank() {
    return std::vector<uint8_t>(size_t(kProbeW) * kProbeH * 4, 0);
}
void Light(std::vector<uint8_t>& img, uint32_t x, uint32_t y, uint8_t a = 255) {
    img[(size_t(y) * kProbeW + x) * 4 + 3] = a;
}
/// A horizontal run of lit probes at fraction row fy, from fraction fx0 to fx1 (a subtitle line).
void Line(std::vector<uint8_t>& img, float fy, float fx0, float fx1) {
    const uint32_t y = uint32_t(fy * float(kProbeH));
    for (uint32_t x = uint32_t(fx0 * float(kProbeW)); x < uint32_t(fx1 * float(kProbeW)); ++x) {
        Light(img, x, y);
    }
}

void TestModes() {
    assert(ParseMode("auto", Mode::Off) == Mode::Auto);
    assert(ParseMode(" MENUS \n", Mode::Auto) == Mode::Menus);
    assert(ParseMode("on", Mode::Off) == Mode::On);
    assert(ParseMode("1", Mode::Off) == Mode::On);
    assert(ParseMode("off", Mode::On) == Mode::Off);
    assert(ParseMode("0", Mode::On) == Mode::Off);
    assert(ParseMode("", Mode::Menus) == Mode::Menus);      // empty: fallback
    assert(ParseMode("bogus", Mode::On) == Mode::On);       // unknown: fallback
    assert(ParseMode("subtitles", Mode::Off) == Mode::Subtitles);
    assert(ParseMode("SUBS", Mode::Off) == Mode::Subtitles);
    assert(kDefaultMode == Mode::Menus); // the player's decision: menus only, unless asked
    assert(ModeName(Mode::Subtitles)[0] == 's');
    assert(ModeName(Mode::Auto)[0] == 'a' && ModeName(Mode::Off)[1] == 'f');
}

void TestZones() {
    // Top-left bars, bottom-left item cross, bottom-right souls: always on, never counted.
    assert(InAlwaysOnZone(5, 5, kProbeW, kProbeH));
    assert(InAlwaysOnZone(5, kProbeH - 3, kProbeW, kProbeH));
    assert(InAlwaysOnZone(kProbeW - 3, kProbeH - 3, kProbeW, kProbeH));
    // The middle, the bottom centre (subtitles), the top right and the right edge middle: counted.
    assert(!InAlwaysOnZone(kProbeW / 2, kProbeH / 2, kProbeW, kProbeH));
    assert(!InAlwaysOnZone(kProbeW / 2, kProbeH * 5 / 6, kProbeW, kProbeH));
    assert(!InAlwaysOnZone(kProbeW - 3, 3, kProbeW, kProbeH));
    assert(!InAlwaysOnZone(kProbeW - 3, kProbeH / 2, kProbeW, kProbeH));
}

void TestActivity() {
    auto img = Blank();
    assert(Activity(img.data(), kProbeW, kProbeH) == 0);
    assert(Activity(nullptr, kProbeW, kProbeH) == 0);
    assert(Activity(img.data(), 0, 0) == 0);
    // The always-on HUD alone: full bars top left, items bottom left, souls bottom right.
    Line(img, 0.05f, 0.02f, 0.30f);
    Line(img, 0.10f, 0.02f, 0.30f);
    Line(img, 0.90f, 0.02f, 0.22f);
    Line(img, 0.95f, 0.78f, 0.98f);
    assert(Activity(img.data(), kProbeW, kProbeH) == 0);
    // A lock-on reticle and a damage number: a few probes in the middle stay under the limit.
    Light(img, 60, 30);
    Light(img, 68, 30);
    Light(img, 64, 26);
    Light(img, 64, 34);
    Light(img, 80, 20);
    assert(Activity(img.data(), kProbeW, kProbeH) == 5);
    assert(Activity(img.data(), kProbeW, kProbeH) < kActivityMin);
    // Faint alpha (below kLitAlpha) is not a probe.
    auto faint = Blank();
    Line(faint, 0.8f, 0.2f, 0.8f);
    for (uint32_t x = 0; x < kProbeW; ++x) {
        faint[(size_t(uint32_t(0.8f * float(kProbeH))) * kProbeW + x) * 4 + 3] = kLitAlpha - 1;
    }
    assert(Activity(faint.data(), kProbeW, kProbeH) == 0);
    // A subtitle line (a run of probes along the bottom centre) is a message.
    auto sub = Blank();
    Line(sub, 0.84f, 0.30f, 0.70f);
    assert(Activity(sub.data(), kProbeW, kProbeH) >= kActivityMin);
    // The same run inside the bottom-left item zone is not.
    auto zone = Blank();
    Line(zone, 0.84f, 0.00f, 0.24f);
    assert(Activity(zone.data(), kProbeW, kProbeH) == 0);
}

/// Feeds a steady `count` for `seconds` at 72 Hz; returns whether the last frame showed the quad.
bool Feed(Gate& g, double& t, double seconds, uint32_t count, Mode m = Mode::Auto, bool menu = false,
          bool ignore = false) {
    bool shown = false;
    for (double end = t + seconds; t < end; t += 1.0 / 72.0) {
        shown = g.Update(t, m, menu, count, ignore);
    }
    return shown;
}

void TestGate() {
    double t = 10.0;
    Gate g;
    assert(!Feed(g, t, 1.0, 0));
    assert(g.reason == Reason::None);
    // A menu opens: shown at once, with the menu reason; held kHoldMenuS after it closes.
    assert(g.Update(t, Mode::Auto, true, 0));
    assert(g.reason == Reason::Menu);
    assert(g.Update(t + kHoldMenuS - 0.01, Mode::Auto, false, 0));
    t += 1.0;
    assert(!Feed(g, t, 1.0, 0));
    // A message on top of the baseline: shown, held kHoldMessageS after its last sign.
    assert(Feed(g, t, 0.5, kActivityMin + 2));
    assert(g.reason == Reason::Message);
    assert(Feed(g, t, kHoldMessageS - 0.2, 0));
    assert(!Feed(g, t, 0.5, 0));
    // Fewer probes than the limit never open it.
    assert(!Feed(g, t, 2.0, kActivityMin - 1));
    // menus: only the menu flag.
    Gate m;
    double u = 1.0;
    assert(!Feed(m, u, 1.0, 500, Mode::Menus));
    assert(m.Update(u, Mode::Menus, true, 0));
    u += 5.0;
    assert(!m.Update(u, Mode::Menus, false, 500));
    // on: always; off: never (even for menus).
    Gate on, off;
    assert(on.Update(1.0, Mode::On, false, 0) && on.reason == Reason::Forced);
    assert(!off.Update(1.0, Mode::Off, true, 500));
    // A live switch to off hides the quad at once even inside a hold, and back to on shows it.
    Gate s;
    assert(s.Update(1.0, Mode::Auto, true, 0));
    assert(!s.Update(1.1, Mode::Off, true, 0));
    assert(s.Update(1.2, Mode::On, false, 0));
}

/// The default (menus) never reacts to message content; subtitles does, but only the band.
void TestDefaultAndSubtitles() {
    Gate d;
    double t = 5.0;
    assert(!Feed(d, t, 1.0, 0, kDefaultMode));
    assert(!Feed(d, t, 3.0, 400, kDefaultMode)); // lots of lit probes: still hidden
    assert(!d.band_only);
    assert(Feed(d, t, 0.2, 0, kDefaultMode, true) && d.reason == Reason::Menu);

    Gate s;
    t = 5.0;
    assert(!Feed(s, t, 1.0, 0, Mode::Subtitles));
    assert(Feed(s, t, 0.5, 60, Mode::Subtitles));
    assert(s.reason == Reason::Message && s.band_only);       // a message: the band only
    assert(Feed(s, t, 0.2, 0, Mode::Subtitles, true));
    assert(s.reason == Reason::Menu && !s.band_only);         // a menu: the whole quad
    assert(!Feed(s, t, kHoldMessageS + 1.0, 0, Mode::Subtitles));
    assert(!s.band_only);
    Gate a;
    t = 5.0;
    Feed(a, t, 1.0, 0, Mode::Auto);
    assert(Feed(a, t, 0.5, 60, Mode::Auto) && !a.band_only);  // auto: the whole quad
    // The band is bottom centre and inside the image.
    assert(kMessageBand.x0 > 0.0f && kMessageBand.x1 < 1.0f && kMessageBand.y0 > 0.5f && kMessageBand.y1 <= 1.0f);
}

/// The headset log's stuck case: a loading screen fills the UI image (7279 of 9216 probes), then
/// play goes on with ~50 lit probes of always-on HUD outside the zones. The quad must hide, and a
/// real message on top of that baseline must still show it and then let go.
void TestStuckAfterLoading() {
    Gate g;
    double t = 100.0;
    // The loading screen is ignored when the presenter says so, never a baseline.
    Gate h;
    double v = 0.0;
    Feed(h, v, 2.0, 7279, Mode::Auto, false, true);
    assert(!h.Update(v, Mode::Auto, false, 7279, true));
    // Without the ignore: loading (7279) then plain play at 51: hides as soon as the count drops.
    Feed(g, t, 3.0, 7279);
    Feed(g, t, 0.05, 51);
    assert(!Feed(g, t, 4.0, 51)); // hidden for good
    assert(g.baseline <= 52.0);
    // 51 -> 51 + 40 (a subtitle line) shows, then hides again 2.5 s after it goes.
    assert(Feed(g, t, 1.0, 91));
    assert(g.reason == Reason::Message);
    assert(!Feed(g, t, kHoldMessageS + 0.5, 51));
    // A message that never goes away (a boss bar) becomes the new normal after a while.
    assert(Feed(g, t, 1.0, 51 + 30));
    assert(!Feed(g, t, 60.0, 51 + 30));
    // A wobble of the always-on HUD (51..60) never shows it.
    assert(!Feed(g, t, 2.0, 60));
    assert(!Feed(g, t, 2.0, 49));
    // A first sample in the middle of a message: it is the baseline until a lower one arrives.
    Gate k;
    double w = 0.0;
    Feed(k, w, 0.5, 200);
    assert(!Feed(k, w, 4.0, 40));
}

} // namespace

int main() {
    TestModes();
    TestZones();
    TestActivity();
    TestGate();
    TestStuckAfterLoading();
    TestDefaultAndSubtitles();
    std::printf("xr-hud-center-test: all checks passed\n");
    return 0;
}
