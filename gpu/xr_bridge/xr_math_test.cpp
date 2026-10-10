// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_math_test.exe: offline unit tests of the bridge's pure logic (no headset):
//   - xr_screen_math.h: the screen placement state machine against the sample sequences the Pimax
//     runtime produces: junk head poses (|y| ~ 77 m, valid bits set) for seconds at startup,
//     after the headset is put on and around a manual recenter;
//   - xr_input_logic.h: the left stick click layer (d-pad, options/touchpad chords, recenter hold).
// Exit code 0 when every check passes.

#include <cmath>
#include <cstdio>
#include <random>

#include "xr_hud.h"
#include "xr_input_logic.h"
#include "xr_screen_math.h"

using namespace xrscreen;

namespace {

int failures = 0, checks = 0;

void Check(bool ok, const char* what, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

constexpr float kDistance = 2.6f;
constexpr double kDt = 1.0 / 90.0;
constexpr float kDeg = 57.29578f;

Quat YawQuat(float degrees) {
    const float half = degrees / kDeg * 0.5f;
    return {0.0f, std::sin(half), 0.0f, std::cos(half)};
}

HeadSample Sample(Vec3 position, float yaw_degrees) {
    HeadSample s;
    s.oriented = s.positioned = true;
    s.pose = {YawQuat(yaw_degrees), position};
    return s;
}

/// The junk Pimax sends: valid bits set, the head 77 m below the origin, some random yaw.
HeadSample Junk() {
    return Sample({3.1f, -77.38f, -12.0f}, -42.0f);
}

HeadSample Good(float yaw_degrees = 30.0f) {
    return Sample({0.1f, 0.02f, 0.2f}, yaw_degrees);
}

/// Feeds `sample` from t for `seconds` at 90 Hz while waiting; returns true when it placed.
bool Feed(Placer& placer, double& t, double seconds, const HeadSample& sample, Pose* screen) {
    const double end = t + seconds;
    for (; t < end; t += kDt) {
        if (placer.Update(t, sample, kDistance, screen)) {
            t += kDt;
            return true;
        }
    }
    return false;
}

/// The screen 2.6 m ahead of the head along yaw, at head height.
bool AheadOf(const Pose& screen, const HeadSample& head, float yaw_degrees) {
    const float yaw = yaw_degrees / kDeg;
    const Vec3 expect{head.pose.position.x - std::sin(yaw) * kDistance, head.pose.position.y,
                      head.pose.position.z - std::cos(yaw) * kDistance};
    return Length(Sub(screen.position, expect)) < 0.01f &&
           std::fabs(Yaw(screen.orientation) * kDeg - yaw_degrees) < 0.5f;
}

bool IsDefault(const Pose& screen) {
    return Length(Sub(screen.position, {0.0f, 0.0f, -kDistance})) < 1e-4f &&
           std::fabs(screen.orientation.w - 1.0f) < 1e-4f;
}

void TestPlausible() {
    CHECK(Plausible(Good()));
    CHECK(Plausible(Sample({0.0f, 1.7f, 0.5f}, 0.0f)));  // STAGE: head above the floor origin
    CHECK(!Plausible(Junk()));                            // |y| > 3 m
    CHECK(!Plausible(Sample({40.0f, 0.0f, 0.0f}, 0.0f))); // > 5 m from the origin
    CHECK(!Plausible(Sample({4.0f, 0.0f, 3.5f}, 0.0f)));  // 5.3 m from the origin
    CHECK(!Plausible(Sample({0.0f, NAN, 0.0f}, 0.0f)));
    HeadSample no_position = Good();
    no_position.positioned = false;
    CHECK(!Plausible(no_position));
    HeadSample no_pose = Good();
    no_pose.oriented = false;
    CHECK(!Plausible(no_pose));
}

/// The in-game failure: junk for 5 s at startup (past the 3 s fallback), then good tracking.
void TestJunkThenGood() {
    Placer placer;
    placer.accept_steady = false; // constant junk would be a steady head
    Pose screen = DefaultScreen(kDistance);
    double t = 0.0;
    placer.Arm(t, false);
    CHECK(!Feed(placer, t, 5.0, Junk(), &screen)); // never placed at the junk pose
    CHECK(placer.Waiting());
    CHECK(IsDefault(screen));
    CHECK(placer.out_of_range > 400);
    CHECK(placer.DefaultIn(t) > 4.0);
    const double good_from = t;
    CHECK(Feed(placer, t, 2.0, Good(30.0f), &screen));
    CHECK(placer.how == Placer::How::Tracked);
    // 60 plausible samples in the window: about two thirds of a second.
    CHECK(t - good_from > 0.6 && t - good_from < 0.75);
    CHECK(AheadOf(screen, Good(), 30.0f));
    CHECK(std::fabs(screen.position.y) <= kPlausibleY);
}

/// Junk for good: the default pose after 10 s, then placed again once tracking is plausible.
void TestJunkForever() {
    Placer placer;
    placer.accept_steady = false; // constant junk would be a steady head
    Pose screen = DefaultScreen(kDistance);
    double t = 0.0;
    placer.Arm(t, false);
    CHECK(!Feed(placer, t, Placer::kDefaultSeconds - 0.1, Junk(), &screen));
    CHECK(Feed(placer, t, 0.5, Junk(), &screen));
    CHECK(placer.how == Placer::How::Default);
    CHECK(IsDefault(screen));
    // Still junk: nothing to do.
    for (int i = 0; i < 200; ++i, t += kDt) {
        CHECK(placer.Watch(t, Junk(), screen) == Placer::Again::No);
    }
    // Plausible tracking for kWatchSeconds: place again (the caller re-arms).
    Placer::Again again = Placer::Again::No;
    const double from = t;
    for (; t < from + 1.0 && again == Placer::Again::No; t += kDt) {
        again = placer.Watch(t, Good(-60.0f), screen);
    }
    CHECK(again == Placer::Again::Tracking);
    CHECK(t - from >= Placer::kWatchSeconds && t - from < Placer::kWatchSeconds + 0.05);
    placer.Arm(t, false);
    CHECK(Feed(placer, t, 2.0, Good(-60.0f), &screen));
    CHECK(placer.how == Placer::How::Tracked);
    CHECK(AheadOf(screen, Good(), -60.0f));
}

/// Tracking that drops out every other frame never steadies: the fallback at 3 s uses the latest
/// plausible sample, never the junk one.
void TestIntermittent() {
    Placer placer;
    Pose screen = DefaultScreen(kDistance);
    double t = 0.0;
    placer.Arm(t, false);
    bool placed = false;
    int frame = 0;
    for (; t < 5.0 && !placed; t += kDt, ++frame) {
        placed = placer.Update(t, frame % 2 ? Good(10.0f) : Junk(), kDistance, &screen);
    }
    CHECK(placed);
    CHECK(placer.how == Placer::How::Fallback);
    CHECK(t >= Placer::kFallbackSeconds && t < Placer::kFallbackSeconds + 0.05);
    CHECK(AheadOf(screen, Good(), 10.0f));
    CHECK(placer.age < 0.05);
}

/// Manual recenter (immediate) while the runtime sends junk: the most recent plausible pose from
/// before the recenter, after kImmediateFallbackSeconds.
void TestRecenterDuringJunk() {
    Placer placer;
    placer.accept_steady = false; // constant junk would be a steady head
    Pose screen = DefaultScreen(kDistance);
    double t = 0.0;
    placer.Arm(t, false);
    CHECK(Feed(placer, t, 2.0, Good(0.0f), &screen));
    // Placed; the user looks at 45 deg for a while (Watch keeps the most recent plausible pose).
    for (int i = 0; i < 90; ++i, t += kDt) {
        CHECK(placer.Watch(t, Good(45.0f), screen) == Placer::Again::No);
    }
    // Junk starts, then the user holds the menu button.
    for (int i = 0; i < 90; ++i, t += kDt) {
        CHECK(placer.Watch(t, Junk(), screen) == Placer::Again::No);
    }
    placer.Arm(t, true);
    const double armed = t;
    CHECK(Feed(placer, t, 1.0, Junk(), &screen));
    CHECK(placer.how == Placer::How::Fallback);
    CHECK(t - armed >= Placer::kImmediateFallbackSeconds && t - armed < 0.55);
    CHECK(AheadOf(screen, Good(), 45.0f));
    CHECK(placer.age > 1.0);
}

/// Manual recenter with good tracking: at once, at the current head.
void TestRecenterTracked() {
    Placer placer;
    Pose screen = DefaultScreen(kDistance);
    double t = 0.0;
    placer.Arm(t, false);
    CHECK(Feed(placer, t, 2.0, Good(0.0f), &screen));
    placer.Arm(t, true);
    CHECK(placer.Update(t, Good(-90.0f), kDistance, &screen));
    CHECK(placer.how == Placer::How::Tracked);
    CHECK(AheadOf(screen, Good(), -90.0f));
}

/// Manual recenter with no plausible pose ever: the default pose, not the junk one.
void TestRecenterNothingPlausible() {
    Placer placer;
    placer.accept_steady = false; // constant junk would be a steady head
    Pose screen = DefaultScreen(kDistance);
    double t = 0.0;
    placer.Arm(t, true);
    CHECK(Feed(placer, t, 1.0, Junk(), &screen));
    CHECK(placer.how == Placer::How::Default);
    CHECK(IsDefault(screen));
}

/// Headset put on again (refocus) and junk for more than 10 s: the screen stays where it was.
void TestRefocusKeeps() {
    Placer placer;
    placer.accept_steady = false; // constant junk would be a steady head
    Pose screen = DefaultScreen(kDistance);
    double t = 0.0;
    placer.Arm(t, false);
    CHECK(Feed(placer, t, 2.0, Good(20.0f), &screen));
    const Pose before = screen;
    placer.Arm(t, false);
    CHECK(!Feed(placer, t, Placer::kDefaultSeconds - 0.1, Junk(), &screen));
    CHECK(Feed(placer, t, 0.5, Junk(), &screen));
    CHECK(placer.how == Placer::How::Kept);
    CHECK(Length(Sub(screen.position, before.position)) < 1e-6f);
    // Kept is provisional: plausible tracking places it again.
    Placer::Again again = Placer::Again::No;
    for (int i = 0; i < 90 && again == Placer::Again::No; ++i, t += kDt) {
        again = placer.Watch(t, Good(20.0f), screen);
    }
    CHECK(again == Placer::Again::Tracking);
}

/// A plausible head far from the screen (the space moved under it): placed again; near: not.
void TestFar() {
    Placer placer;
    Pose screen = DefaultScreen(kDistance);
    double t = 0.0;
    placer.Arm(t, false);
    CHECK(Feed(placer, t, 2.0, Sample({0.0f, 0.0f, 0.0f}, 0.0f), &screen));
    for (int i = 0; i < 200; ++i, t += kDt) {
        CHECK(placer.Watch(t, Sample({0.0f, 0.0f, 2.0f}, 0.0f), screen) == Placer::Again::No);
    }
    Placer::Again again = Placer::Again::No;
    const double from = t;
    for (int i = 0; i < 90 && again == Placer::Again::No; ++i, t += kDt) {
        // Junk frames in between are ignored, they neither trigger nor reset.
        again = placer.Watch(t, i % 3 ? Sample({0.0f, 0.0f, 4.9f}, 0.0f) : Junk(), screen);
    }
    CHECK(again == Placer::Again::Far);
    CHECK(placer.head_to_screen > Placer::kFarMeters);
    CHECK(t - from >= Placer::kWatchSeconds);
}

/// Random mixes of junk, missing and good samples, re-arms and recenters: the screen is never
/// placed at an implausible height or far from the origin.
/// Tracking that never steadies places at the fallback; the first steady plausible head redoes
/// it once (BB_XR_RECENTER_ON_START), not forever.
void TestFallbackRedone() {
    Placer placer;
    Pose screen = DefaultScreen(kDistance);
    double t = 0.0;
    placer.Arm(t, false);
    // Plausible only 1 of 3 frames: never 60 of 90, so the fallback fires 3 s after the first.
    bool placed = false;
    for (int i = 0; !placed && i < 1000; ++i, t += kDt) {
        placed = placer.Update(t, i % 3 == 0 ? Good(10.0f) : Junk(), kDistance, &screen);
    }
    CHECK(placed && placer.how == Placer::How::Fallback);
    Placer::Again again = Placer::Again::No;
    for (int i = 0; i < 90 && again == Placer::Again::No; ++i, t += kDt) {
        again = placer.Watch(t, Good(80.0f), screen);
    }
    CHECK(again == Placer::Again::Tracking);
    placer.Arm(t, false);
    placed = false;
    for (int i = 0; !placed && i < 1000; ++i, t += kDt) {
        placed = placer.Update(t, i % 3 == 0 ? Good(10.0f) : Junk(), kDistance, &screen);
    }
    CHECK(placed && placer.how == Placer::How::Fallback);
    again = Placer::Again::No;
    for (int i = 0; i < 180; ++i, t += kDt) {
        again = placer.Watch(t, Good(10.0f), screen);
        CHECK(again == Placer::Again::No); // only one redo
    }
    // Off: the fallback stays.
    Placer off;
    off.redo_fallback = false;
    t = 0.0;
    off.Arm(t, false);
    placed = false;
    for (int i = 0; !placed && i < 1000; ++i, t += kDt) {
        placed = off.Update(t, i % 3 == 0 ? Good(10.0f) : Junk(), kDistance, &screen);
    }
    CHECK(placed && off.how == Placer::How::Fallback);
    for (int i = 0; i < 180; ++i, t += kDt) {
        CHECK(off.Watch(t, Good(10.0f), screen) == Placer::Again::No);
    }
}

/// The flat screen back after the 3D view: follow when the head is > 60 deg away from it.
void TestFollowOnReappear() {
    CHECK(ParseFollow(nullptr) == Follow::Reappear);
    CHECK(ParseFollow("") == Follow::Reappear);
    CHECK(ParseFollow("off") == Follow::Off);
    CHECK(ParseFollow("always") == Follow::Always);
    CHECK(ParseFollow("reappear") == Follow::Reappear);
    const HeadSample facing = Good(0.0f);
    const Pose screen = PlaceScreen(facing.pose.position, facing.pose.orientation, kDistance);
    CHECK(HeadAngleToScreen(facing, screen) < 1.0f);
    CHECK(std::fabs(HeadAngleToScreen(Good(90.0f), screen) - 90.0f) < 1.0f);
    CHECK(std::fabs(HeadAngleToScreen(Good(180.0f), screen) - 180.0f) < 1.0f);
    CHECK(!ShouldFollowOnReappear(Follow::Reappear, Good(0.0f), screen));
    CHECK(!ShouldFollowOnReappear(Follow::Reappear, Good(50.0f), screen));
    CHECK(ShouldFollowOnReappear(Follow::Reappear, Good(70.0f), screen));
    CHECK(ShouldFollowOnReappear(Follow::Reappear, Good(-120.0f), screen));
    CHECK(!ShouldFollowOnReappear(Follow::Off, Good(150.0f), screen));
    CHECK(ShouldFollowOnReappear(Follow::Always, Good(0.0f), screen));
    CHECK(!ShouldFollowOnReappear(Follow::Always, Junk(), screen)); // junk head: no
    HeadSample none;
    CHECK(!ShouldFollowOnReappear(Follow::Reappear, none, screen));
}

void TestFuzz() {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    int placements = 0;
    for (int run = 0; run < 200; ++run) {
        Placer placer;
        Pose screen = DefaultScreen(kDistance);
        double t = 0.0;
        placer.Arm(t, false);
        const float junk_rate = unit(rng);
        for (int frame = 0; frame < 90 * 30; ++frame, t += kDt) {
            HeadSample s;
            const float r = unit(rng);
            if (r < junk_rate) {
                s = Sample({unit(rng) * 200.0f - 100.0f, unit(rng) * 200.0f - 100.0f,
                            unit(rng) * 200.0f - 100.0f},
                           unit(rng) * 360.0f);
            } else if (r < junk_rate + 0.05f) {
                s = HeadSample{}; // nothing
            } else {
                s = Sample({unit(rng) * 2.0f - 1.0f, unit(rng) * 0.4f - 0.2f,
                            unit(rng) * 2.0f - 1.0f},
                           unit(rng) * 360.0f);
            }
            if (placer.Waiting()) {
                if (placer.Update(t, s, kDistance, &screen)) {
                    ++placements;
                    Check(std::fabs(screen.position.y) <= kPlausibleY, "fuzz: plausible height",
                          __LINE__);
                    Check(Length(screen.position) <= kPlausibleRadius + kDistance + 1e-3f,
                          "fuzz: near the origin", __LINE__);
                }
            } else if (placer.Watch(t, s, screen) != Placer::Again::No) {
                placer.Arm(t, false);
            }
            if (unit(rng) < 0.002f) {
                placer.Arm(t, unit(rng) < 0.5f);
            }
        }
    }
    CHECK(placements > 200);
}

// ---- The left stick click layer ----------------------------------------------------------------

struct Pad {
    bool l3 = false, y = false, x = false;
    float look_x = 0.0f, look_y = 0.0f;
    bool moving = false;
};

/// Runs the layer from t_ms for `ms` at 90 Hz with `pad`; ORs what came out.
struct Run {
    uint32_t buttons = 0, pulses = 0;
    int recenters = 0;
    bool zero_look = false;
};
Run Hold(xrpad::L3Layer& layer, double& t_ms, double ms, const Pad& pad) {
    Run run;
    const double end = t_ms + ms;
    for (; t_ms < end; t_ms += 1000.0 / 90.0) {
        const auto out = layer.Update(t_ms, pad.l3, pad.y, pad.x, pad.look_x, pad.look_y, pad.moving);
        run.buttons |= out.buttons;
        run.pulses |= out.pulses;
        run.recenters += out.recenter ? 1 : 0;
        run.zero_look = run.zero_look || out.zero_look;
    }
    return run;
}

void TestL3Click() {
    xrpad::L3Layer layer;
    double t = 0.0;
    const Run down = Hold(layer, t, 200.0, {true});
    CHECK(down.pulses == 0 && down.recenters == 0 && down.zero_look);
    const Run up = Hold(layer, t, 50.0, {});
    CHECK(up.pulses == BBXR_BTN_L3); // a plain click: L3 on release
    CHECK(!up.zero_look);
}

/// The default map since findings/26 (chords off): B and A stay cross and circle while L3 is held,
/// a right stick flick stays the camera; the standing click and the recenter hold still work.
void TestL3NoChords() {
    xrpad::L3Layer layer;
    layer.chords = false;
    double t = 0.0;
    Pad chord;
    chord.l3 = chord.y = true;
    chord.look_x = 1.0f;
    const Run down = Hold(layer, t, 300.0, chord);
    CHECK(down.buttons == BBXR_BTN_CROSS); // no options, no d-pad
    CHECK(down.pulses == 0 && !down.zero_look);
    const Run up = Hold(layer, t, 50.0, {});
    CHECK(up.pulses == BBXR_BTN_L3); // still the click (turning the camera does not cancel it)
    Pad still;
    still.l3 = true;
    Hold(layer, t, 200.0, still);
    CHECK(Hold(layer, t, 50.0, {}).pulses == BBXR_BTN_L3);
    still.x = true;
    const Run a = Hold(layer, t, 200.0, still);
    CHECK(a.buttons == BBXR_BTN_CIRCLE);
    CHECK(Hold(layer, t, 1200.0, {true}).recenters == 1);
}

void TestMenuTap() {
    xrpad::MenuTap tap;
    CHECK(!tap.Update(0.0, true));
    CHECK(!tap.Update(300.0, true));
    CHECK(tap.Update(320.0, false)); // a short press: options on release
    CHECK(!tap.Update(400.0, true));
    CHECK(!tap.Update(1500.0, true));
    CHECK(!tap.Update(1520.0, false)); // held: the recenter, no options
}

void TestL3WhileMovingJumps() {
    xrpad::L3Layer layer;
    double t = 0.0;
    Pad click;
    click.l3 = click.moving = true;
    const Run down = Hold(layer, t, 1500.0, click); // long enough to recenter if it were the layer
    CHECK(down.buttons == BBXR_BTN_L3 && down.recenters == 0 && !down.zero_look);
    const Run up = Hold(layer, t, 50.0, {});
    CHECK(up.buttons == 0 && up.pulses == 0); // no second L3 on release
    // Standing still again: the layer is back.
    const Run still = Hold(layer, t, 1100.0, {true});
    CHECK(still.recenters == 1 && still.buttons == 0);
}

void TestL3HoldRecenters() {
    xrpad::L3Layer layer;
    double t = 0.0;
    const Run early = Hold(layer, t, 950.0, {true});
    CHECK(early.recenters == 0);
    const Run late = Hold(layer, t, 2000.0, {true});
    CHECK(late.recenters == 1); // once per hold
    const Run up = Hold(layer, t, 50.0, {});
    CHECK(up.pulses == 0); // the hold never reaches the game as L3
    // The next plain click works again.
    Hold(layer, t, 100.0, {true});
    CHECK(Hold(layer, t, 50.0, {}).pulses == BBXR_BTN_L3);
}

void TestL3Dpad() {
    xrpad::L3Layer layer;
    double t = 0.0;
    Hold(layer, t, 100.0, {true});
    Pad flick{true};
    flick.look_x = 0.9f;
    const Run right = Hold(layer, t, 100.0, flick);
    CHECK(right.buttons == BBXR_BTN_RIGHT && right.zero_look);
    flick.look_x = 0.0f;
    flick.look_y = -0.95f;
    CHECK(Hold(layer, t, 100.0, flick).buttons == BBXR_BTN_DOWN);
    // Held on past 1 s after the d-pad: no recenter, no L3 on release.
    CHECK(Hold(layer, t, 1500.0, {true}).recenters == 0);
    CHECK(Hold(layer, t, 50.0, {}).pulses == 0);
}

void TestL3StickCancelsHold() {
    xrpad::L3Layer layer;
    double t = 0.0;
    Pad nudge{true};
    nudge.look_y = 0.6f; // past the hold cancel, short of a d-pad flick
    const Run run = Hold(layer, t, 1500.0, nudge);
    CHECK(run.recenters == 0 && run.buttons == 0);
    CHECK(Hold(layer, t, 50.0, {}).pulses == BBXR_BTN_L3);
}

void TestL3Options() {
    xrpad::L3Layer layer;
    double t = 0.0;
    Hold(layer, t, 100.0, {true});
    Pad chord{true};
    chord.y = true;
    const Run options = Hold(layer, t, 1500.0, chord);
    CHECK(options.buttons == BBXR_BTN_OPTIONS); // never triangle
    CHECK(options.pulses == BBXR_BTN_OPTIONS);
    CHECK(options.recenters == 0);
    // L3 released first, Y still held: still options.
    Pad y_only;
    y_only.y = true;
    const Run tail = Hold(layer, t, 100.0, y_only);
    CHECK(tail.buttons == BBXR_BTN_OPTIONS && tail.pulses == 0); // and no L3 click
    // A new Y press without L3: triangle.
    Hold(layer, t, 50.0, {});
    CHECK(Hold(layer, t, 100.0, y_only).buttons == BBXR_BTN_CROSS);
}

void TestYBeforeL3StaysTriangle() {
    xrpad::L3Layer layer;
    double t = 0.0;
    Pad y_only;
    y_only.y = true;
    CHECK(Hold(layer, t, 100.0, y_only).buttons == BBXR_BTN_CROSS);
    Pad both{true};
    both.y = true;
    CHECK(Hold(layer, t, 200.0, both).buttons == BBXR_BTN_CROSS); // began before L3
    Pad l3_only{true};
    Hold(layer, t, 50.0, l3_only); // Y released, L3 held
    const Run options = Hold(layer, t, 100.0, both);
    CHECK(options.buttons == BBXR_BTN_OPTIONS);
}

void TestL3Touchpad() {
    xrpad::L3Layer layer;
    double t = 0.0;
    Hold(layer, t, 100.0, {true});
    Pad chord{true};
    chord.x = true;
    const Run touchpad = Hold(layer, t, 100.0, chord);
    CHECK(touchpad.buttons == BBXR_BTN_TOUCHPAD && touchpad.pulses == BBXR_BTN_TOUCHPAD);
    Hold(layer, t, 50.0, {true});
    CHECK(Hold(layer, t, 50.0, {}).pulses == 0);
    Pad x_only;
    x_only.x = true;
    CHECK(Hold(layer, t, 100.0, x_only).buttons == BBXR_BTN_CIRCLE);
}

} // namespace

/// Mono head tracking: both eyes take the render orientation and keep their offset from the head.
static void TestMonoEye() {
    // Located eyes: head at (0, 1.6, 0) turned 20 deg left, canted 10 deg outward, IPD 64 mm.
    const Quat head = YawQuat(20.0f);
    const Vec3 center{0.0f, 1.6f, 0.0f};
    const Pose left{Multiply(head, YawQuat(10.0f)), Add(center, Rotate(head, {-0.032f, 0.0f, 0.0f}))};
    const Pose right{Multiply(head, YawQuat(-10.0f)), Add(center, Rotate(head, {0.032f, 0.0f, 0.0f}))};
    // Rendered from a head 30 deg right at another spot (the pose the camera used).
    const Pose render{YawQuat(-30.0f), {1.0f, 1.5f, -2.0f}};
    const Pose l = MonoEye(render, left, right), r = MonoEye(render, right, left);
    const auto same = [](Quat a, Quat b) {
        return std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z) + std::fabs(a.w - b.w) < 1e-5f;
    };
    CHECK(same(l.orientation, render.orientation) && same(r.orientation, render.orientation));
    const Vec3 expect_l = Add(render.position, Rotate(render.orientation, {-0.032f, 0.0f, 0.0f}));
    const Vec3 expect_r = Add(render.position, Rotate(render.orientation, {0.032f, 0.0f, 0.0f}));
    CHECK(Length(Sub(l.position, expect_l)) < 1e-5f);
    CHECK(Length(Sub(r.position, expect_r)) < 1e-5f);
    CHECK(std::fabs(Length(Sub(l.position, r.position)) - 0.064f) < 1e-5f);
}

// ---- HUD layer (xr_hud.h) -------------------------------------------------------------------

static bool Near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) < eps;
}

static void TestTriggerEdge() {
    xrpad::TriggerEdge e;
    int fires = 0;
    for (float v : {0.0f, 0.3f, 0.7f, 1.0f, 0.9f, 0.5f, 0.4f}) { // one pull, held
        fires += e.Update(v);
    }
    CHECK(fires == 1);
    CHECK(!e.Update(0.5f)); // above the off threshold: still down
    CHECK(!e.Update(0.2f)); // released
    CHECK(!e.Update(0.5f)); // below the on threshold
    CHECK(e.Update(0.8f));  // second pull
    CHECK(!e.Update(0.8f));
}

void TestGripTap() {
    // Quick squeeze and release: one tap, on the release.
    xrpad::GripTap g;
    CHECK(!g.Update(0, 0.0f, false));
    CHECK(!g.Update(100, 0.9f, false));
    CHECK(!g.Update(200, 0.8f, false));
    CHECK(g.Update(300, 0.1f, false)); // 200 ms: swap the right weapon
    CHECK(!g.Update(400, 0.0f, false)); // once only
    // Held long without a swing: nothing, even after the release.
    CHECK(!g.Update(1000, 0.9f, false));
    CHECK(!g.Update(1500, 0.9f, false));
    CHECK(!g.Update(1600, 0.0f, false));
    // Grip held with a swing (heavy R2): no swap on the release.
    CHECK(!g.Update(2000, 0.9f, false));
    CHECK(!g.Update(2100, 0.9f, true));
    CHECK(!g.Update(2200, 0.0f, false));
    // A swing in the same frame as the press also cancels it.
    CHECK(!g.Update(3000, 0.9f, true));
    CHECK(!g.Update(3100, 0.0f, false));
    // A swing before the press does not: the next quick tap works.
    CHECK(!g.Update(3500, 0.0f, true));
    CHECK(!g.Update(3600, 0.9f, false));
    CHECK(g.Update(3700, 0.0f, false));
    // A hysteresis wobble between 0.4 and 0.6 is not a release.
    CHECK(!g.Update(4000, 0.9f, false));
    CHECK(!g.Update(4050, 0.5f, false));
    CHECK(!g.Update(4100, 0.9f, false));
    CHECK(g.Update(4150, 0.2f, false));
    // The limit is configurable.
    xrpad::GripTap h;
    h.max_ms = 150.0;
    CHECK(!h.Update(0, 0.9f, false));
    CHECK(!h.Update(300, 0.0f, false));
    // The default is 350 ms.
    CHECK(!g.Update(5000, 0.9f, false));
    CHECK(!g.Update(5400, 0.0f, false));
}

void TestSwingDirection() {
    const xrpad::V3 head{0.0f, 1.6f, 0.0f}, hand{0.2f, 1.2f, -0.4f};
    CHECK(xrpad::SwingAwayFromHead({0.5f, 0.0f, -3.0f}, hand, head));  // forward
    CHECK(xrpad::SwingAwayFromHead({3.0f, 0.0f, 0.0f}, hand, head));   // sideways
    CHECK(xrpad::SwingAwayFromHead({0.0f, -3.0f, -0.5f}, hand, head)); // downward chop
    CHECK(!xrpad::SwingAwayFromHead({-0.5f, 1.0f, 3.0f}, hand, head)); // back toward the body
    CHECK(!xrpad::SwingAwayFromHead({0.0f, 0.0f, 0.0f}, hand, head));
}

void TestSwingGate() {
    xrpad::SwingGate g;
    CHECK(g.Update(3.0f, true, false));  // swing
    CHECK(!g.Update(3.0f, true, false)); // not rearmed
    CHECK(!g.Update(1.5f, true, false)); // between rearm and swing speed
    CHECK(!g.Update(2.5f, true, false)); // still not rearmed: no second R1
    CHECK(!g.Update(0.5f, true, false)); // rearm
    CHECK(!g.Update(3.0f, false, false)); // return stroke: wrong direction
    CHECK(g.armed);
    CHECK(!g.Update(3.0f, true, true)); // L3 held
    CHECK(g.Update(3.0f, true, false));
}

void TestHeavyCharge() {
    xrpad::HeavyCharge c;
    CHECK(!c.Update(0.0, 1.0f)); // grip without a swing: nothing
    CHECK(!c.Update(1000.0, 1.0f));
    c.OnHeavy(2000.0);
    CHECK(c.Update(2010.0, 1.0f)); // grip held after the heavy swing: R2 held
    CHECK(c.Update(5000.0, 0.8f));
    CHECK(c.Update(5100.0, 0.5f)); // hysteresis
    CHECK(!c.Update(5200.0, 0.1f)); // released
    CHECK(!c.Update(5300.0, 1.0f)); // grip again without a swing: nothing
    c.OnHeavy(6000.0);
    CHECK(!c.Update(6100.0, 0.0f)); // no grip, window runs out
    CHECK(!c.Update(6600.0, 0.0f));
    CHECK(!c.Update(6700.0, 1.0f)); // late grip: nothing
}

void TestMouthDwell() {
    xrpad::MouthDwell m;
    bool fired = false;
    for (double t = 0.0; t < 1000.0; t += 11.0) { // near the mouth, grip open
        fired = fired || m.Update(t, 0.05f, 0.0f);
    }
    CHECK(!fired);
    for (double t = 1000.0; t < 1250.0; t += 11.0) { // grip closed, not long enough
        fired = fired || m.Update(t, 0.05f, 1.0f);
    }
    CHECK(!fired);
    for (double t = 1250.0; t < 1500.0; t += 11.0) {
        fired = fired || m.Update(t, 0.05f, 1.0f);
    }
    CHECK(fired);
    CHECK(!m.Update(1600.0, 0.05f, 1.0f)); // once
    CHECK(!m.Update(1700.0, 0.12f, 1.0f)); // outside 0.10 but not rearmed (< 0.2)
    CHECK(!m.Update(1800.0, 0.5f, 1.0f));  // rearmed
    CHECK(!m.Update(1900.0, 0.05f, 1.0f));
    CHECK(m.Update(2300.0, 0.05f, 1.0f));
    xrpad::MouthDwell far;
    for (double t = 0.0; t < 2000.0; t += 11.0) { // 0.12 m: outside the radius
        CHECK(!far.Update(t, 0.12f, 1.0f));
    }
}

void TestTurnGuard() {
    xrpad::TurnGuard g;
    CHECK(!g.Blocked(0.0, 0.0f, false));
    CHECK(!g.Blocked(11.0, 0.05f, false));
    CHECK(g.Blocked(22.0, 0.05f + 0.8f, false)); // snap turn
    CHECK(g.Blocked(300.0, 0.85f, false));
    CHECK(!g.Blocked(330.0, 0.85f, false)); // 300 ms later
    CHECK(g.Blocked(400.0, 0.85f, true));   // recenter
    CHECK(!g.Blocked(750.0, 0.85f, false));
    // wrap-around: 3.0 -> -3.0 rad is a small turn
    xrpad::TurnGuard w;
    w.Blocked(0.0, 3.0f, false);
    CHECK(!w.Blocked(11.0, -3.0f, false));
    // yaw of a quaternion
    CHECK(Near(xrpad::TurnGuard::Yaw(0.0f, std::sin(0.5f), 0.0f, std::cos(0.5f)), 1.0f, 1e-4f));
}

void TestGripFlick() {
    using xrpad::GripFlick;
    GripFlick::Out o = GripFlick::Update(1.0f, false, false, 0.0f, 0.9f);
    CHECK(o.buttons == BBXR_BTN_UP && o.zero_look);
    CHECK(GripFlick::Update(1.0f, false, false, 0.0f, -0.9f).buttons == BBXR_BTN_DOWN);
    CHECK(GripFlick::Update(1.0f, false, false, 0.9f, 0.1f).buttons == BBXR_BTN_RIGHT);
    CHECK(GripFlick::Update(1.0f, false, false, -0.9f, 0.1f).buttons == BBXR_BTN_LEFT);
    o = GripFlick::Update(1.0f, false, false, 0.6f, 0.0f); // turning gently: below the flick
    CHECK(o.buttons == 0 && o.zero_look);
    o = GripFlick::Update(1.0f, false, false, 0.2f, 0.0f);
    CHECK(o.buttons == 0 && !o.zero_look);
    CHECK(GripFlick::Update(0.3f, false, false, 0.0f, 0.9f).buttons == 0); // no grip
    o = GripFlick::Update(1.0f, true, false, 0.0f, 0.9f); // swing in progress
    CHECK(o.buttons == 0 && !o.zero_look);
    CHECK(GripFlick::Update(1.0f, false, true, 0.0f, 0.9f).buttons == 0); // L3 layer wins
}

void TestRateLimit() {
    xrpad::RateLimit r;
    CHECK(r.Allow(0, 10.0));
    CHECK(!r.Allow(0, 1000.0));
    CHECK(r.Allow(1, 1000.0)); // other kind
    CHECK(!r.Allow(0, 2009.0));
    CHECK(r.Allow(0, 2011.0));
    CHECK(!r.Allow(9, 0.0)); // out of range
}

void TestHudParse() {
    using xrhud::Mode;
    using xrhud::ParseMode;
    CHECK(xrhud::ParseAlpha(nullptr) == xrhud::Alpha::Premul);
    CHECK(xrhud::ParseAlpha("straight\r\n") == xrhud::Alpha::Straight);
    CHECK(xrhud::ParseAlpha(" OPAQUE") == xrhud::Alpha::Opaque);
    CHECK(xrhud::ParseAlpha("junk") == xrhud::Alpha::Premul);
    CHECK(ParseMode(nullptr, nullptr) == Mode::Off);   // cinema: the HUD stays in the image
    CHECK(ParseMode(nullptr, "alt") == Mode::Layer);   // head-tracked modes: layer by default
    CHECK(ParseMode("", "mono") == Mode::Layer);
    CHECK(ParseMode("off", "alt") == Mode::Off);
    CHECK(ParseMode("layer", nullptr) == Mode::Layer); // explicit, also over the cinema screen
    CHECK(ParseMode("force", nullptr) == Mode::Force);
    const xrhud::Config d = xrhud::Parse(nullptr, "alt", nullptr, nullptr, nullptr, nullptr);
    CHECK(Near(d.distance, 2.5f) && Near(d.width_deg, 55.0f) && Near(d.pitch_deg, -5.0f));
    CHECK(d.follow == xrhud::Follow::Lock);
    const xrhud::Config c = xrhud::Parse("layer", nullptr, "3", "60", "-10", "lazy");
    CHECK(Near(c.distance, 3.0f) && Near(c.width_deg, 60.0f) && Near(c.pitch_deg, -10.0f));
    CHECK(c.follow == xrhud::Follow::Lazy);
    const xrhud::Config bad = xrhud::Parse("layer", nullptr, "abc", "1000", "-90", "x");
    CHECK(Near(bad.distance, 2.5f) && Near(bad.width_deg, 140.0f) && Near(bad.pitch_deg, -60.0f));
    CHECK(bad.follow == xrhud::Follow::Lock);
}

static void TestHudQuad() {
    xrhud::Config c;
    c.mode = xrhud::Mode::Layer;
    const xrhud::Quad q = xrhud::HeadQuad(c, 16.0f / 9.0f);
    // 55 deg wide at 2.5 m: 2 * 2.5 * tan(27.5 deg) = 2.603 m; 16:9.
    CHECK(Near(q.width, 2.603f, 2e-3f));
    CHECK(Near(q.height, 2.603f * 9.0f / 16.0f, 2e-3f));
    // 2.5 m out, 5 deg below the view axis.
    CHECK(Near(Length(q.pose.position), 2.5f));
    CHECK(Near(std::atan2(q.pose.position.y, -q.pose.position.z) * kDeg, -5.0f, 1e-2f));
    // It faces the eyes: its +Z points back at the head.
    const Vec3 normal = Rotate(q.pose.orientation, {0.0f, 0.0f, 1.0f});
    CHECK(Near(Dot(normal, Normalize(q.pose.position)), -1.0f));
    // The angle it covers is the configured one.
    CHECK(Near(2.0f * std::atan2(q.width * 0.5f, 2.5f) * kDeg, 55.0f, 0.05f));
}

static void TestHudLazyFollow() {
    xrhud::Config c;
    xrhud::LazyFollow f;
    f.Update(YawQuat(0.0f), 0.0f);
    CHECK(f.valid && Near(f.yaw, 0.0f));
    // A glance 15 deg to the side: the HUD stays put.
    for (int i = 0; i < 90; ++i) {
        f.Update(YawQuat(15.0f), 1.0f / 90.0f);
    }
    CHECK(Near(f.yaw, 0.0f) && !f.moving);
    // Turning 40 deg: it eases after the head and settles within the stop angle.
    for (int i = 0; i < 180; ++i) {
        f.Update(YawQuat(40.0f), 1.0f / 90.0f);
    }
    CHECK(std::fabs(f.yaw * kDeg - 40.0f) < 2.5f && !f.moving);
    // Placed at the head position, 2.5 m along its direction.
    const xrhud::Quad q = f.Place(c, 16.0f / 9.0f, {1.0f, 1.6f, 0.0f});
    const Vec3 rel = Sub(q.pose.position, {1.0f, 1.6f, 0.0f});
    CHECK(Near(Length(rel), 2.5f));
    CHECK(Near(std::atan2(-rel.x, -rel.z) * kDeg, f.yaw * kDeg, 0.05f));
    // Wrap-around: from 170 to -150 deg is a 40 deg turn the short way.
    xrhud::LazyFollow w;
    w.Update(YawQuat(170.0f), 0.0f);
    for (int i = 0; i < 270; ++i) {
        w.Update(YawQuat(-150.0f), 1.0f / 90.0f);
    }
    CHECK(std::fabs(xrhud::WrapPi(w.yaw - (-150.0f / kDeg)) * kDeg) < 2.5f);
    f.Reset();
    CHECK(!f.valid);
}

/// Pimax LOCAL with the origin far from the head: steady, out of range, never plausible. The
/// screen goes in front of the head there, not at the default pose.
void TestSteadyOutOfRange() {
    const HeadSample far = Sample({-20.4f, -6.3f, -15.3f}, 50.0f);
    Placer placer;
    Pose screen = DefaultScreen(kDistance);
    double t = 0.0;
    placer.Arm(t, false);
    CHECK(Feed(placer, t, 3.0, far, &screen));
    CHECK(placer.how == Placer::How::Steady);
    CHECK(t > Placer::kSteadySeconds && t < Placer::kSteadySeconds + 0.1);
    CHECK(AheadOf(screen, far, 50.0f));
    // Manual recenter: faster, at the new yaw.
    placer.Arm(t, true);
    const HeadSample turned = Sample({-20.35f, -6.3f, -15.3f}, -80.0f);
    const double from = t;
    CHECK(Feed(placer, t, 1.0, turned, &screen));
    CHECK(placer.how == Placer::How::Steady);
    CHECK(t - from < 0.4);
    CHECK(AheadOf(screen, turned, -80.0f));
}

/// Jumping junk is not steady: still the default pose, and a plausible head wins over steady.
void TestJumpingJunkNotSteady() {
    Placer placer;
    Pose screen = DefaultScreen(kDistance);
    double t = 0.0;
    placer.Arm(t, false);
    bool placed = false;
    for (int i = 0; i < 900 && !placed; ++i, t += kDt) {
        const float k = float(i / 30); // a new place every third of a second
        placed = placer.Update(t, Sample({k, -77.0f + k, -12.0f}, 0.0f), kDistance, &screen);
    }
    CHECK(!placed);
    CHECK(Feed(placer, t, 2.0, Junk(), &screen));
    CHECK(placer.how == Placer::How::Steady || placer.how == Placer::How::Default);
    Placer p2;
    p2.Arm(0.0, false);
    t = 0.0;
    CHECK(Feed(p2, t, 2.0, Good(10.0f), &screen));
    CHECK(p2.how == Placer::How::Tracked);
}

/// A Steady placement is corrected once the head is plausibly tracked far from it.
void TestSteadyThenPlausible() {
    Placer placer;
    Pose screen = DefaultScreen(kDistance);
    double t = 0.0;
    placer.Arm(t, false);
    CHECK(Feed(placer, t, 3.0, Junk(), &screen));
    CHECK(placer.how == Placer::How::Steady);
    Placer::Again again = Placer::Again::No;
    for (int i = 0; i < 90 && again == Placer::Again::No; ++i, t += kDt) {
        again = placer.Watch(t, Good(), screen);
    }
    CHECK(again == Placer::Again::Far);
}

int main() {
    TestMonoEye();
    TestPlausible();
    TestJunkThenGood();
    TestJunkForever();
    TestIntermittent();
    TestRecenterDuringJunk();
    TestSteadyOutOfRange();
    TestJumpingJunkNotSteady();
    TestSteadyThenPlausible();
    TestRecenterTracked();
    TestRecenterNothingPlausible();
    TestRefocusKeeps();
    TestFar();
    TestFallbackRedone();
    TestFollowOnReappear();
    TestFuzz();
    TestL3Click();
    TestL3WhileMovingJumps();
    TestL3NoChords();
    TestMenuTap();
    TestL3HoldRecenters();
    TestL3Dpad();
    TestL3StickCancelsHold();
    TestL3Options();
    TestYBeforeL3StaysTriangle();
    TestL3Touchpad();
    TestTriggerEdge();
    TestGripTap();
    TestSwingDirection();
    TestSwingGate();
    TestHeavyCharge();
    TestMouthDwell();
    TestTurnGuard();
    TestGripFlick();
    TestRateLimit();
    TestHudParse();
    TestHudQuad();
    TestHudLazyFollow();
    std::printf("xr_math_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
