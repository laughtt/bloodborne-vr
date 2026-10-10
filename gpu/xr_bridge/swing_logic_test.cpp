// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_swing_test.exe: offline tests of swing_logic.h (no headset): synthetic swings
// (minimum-jerk arcs of the hand about the shoulder, sampled at 90 Hz with 2 mm of noise), walking,
// head and body turns, tracking glitches, the combo scheduler and the stamina model.
// Exit code 0 when every check passes.

#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

#include "swing_logic.h"
#include "trick_transform.h"

using namespace swing;

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

constexpr double kDt = 1.0 / 90.0;
constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

Quat YawQuat(float radians) {
    return {0.0f, std::sin(radians * 0.5f), 0.0f, std::cos(radians * 0.5f)};
}
Quat PitchQuat(float radians) { // about +X: -Z tilts up for positive angles
    return {std::sin(radians * 0.5f), 0.0f, 0.0f, std::cos(radians * 0.5f)};
}

/// Normalized linear blend of two rotations (fine for the small blends here).
Quat Nlerp(Quat a, Quat b, float k) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    Quat q{a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k, a.w + (b.w - a.w) * k};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

/// Minimum-jerk progress 0..1 (the shape of a natural reach or swing).
float MinJerk(double u) {
    const double x = std::clamp(u, 0.0, 1.0);
    return float(x * x * x * (10.0 - 15.0 * x + 6.0 * x * x));
}

/// A standing user: head at 1.65 m looking down -Z, the right shoulder 0.2 m to the side.
struct Body {
    Vec3 head{0.0f, 1.65f, 0.0f};
    Quat head_rot{};
    Vec3 shoulder{0.20f, 1.45f, 0.0f};
};

std::mt19937 rng(7);

Vec3 Noise(float sigma) {
    std::normal_distribution<float> n(0.0f, sigma);
    return {n(rng), n(rng), n(rng)};
}

HandSample Sample(double t, Vec3 hand, Quat grip, const Body& body, float noise = 0.002f) {
    HandSample s;
    s.t = t;
    s.valid = true;
    s.position = Add(hand, Noise(noise));
    s.orientation = grip;
    s.head_valid = true;
    s.head_position = Add(body.head, Noise(noise * 0.5f));
    s.head_orientation = body.head_rot;
    return s;
}

/// The hand at rest beside the body, blade forward and down.
HandSample Rest(double t, const Body& body, float noise = 0.002f) {
    return Sample(t, {0.25f, 1.10f, -0.30f}, PitchQuat(-30.0f * kDeg), body, noise);
}

/// A horizontal slash about the shoulder: angle 0 = ahead (-Z), positive = to the left.
HandSample Slash(double t, double t0, double duration, float from_deg, float to_deg, float radius,
                 const Body& body) {
    const float a = (from_deg + (to_deg - from_deg) * MinJerk((t - t0) / duration)) * kDeg;
    const Vec3 dir{-std::sin(a), 0.0f, -std::cos(a)};
    return Sample(t, Add(body.shoulder, Scale(dir, radius)), YawQuat(a), body);
}

/// A vertical chop about the shoulder: angle 0 = ahead, 90 = straight up.
HandSample Chop(double t, double t0, double duration, float from_deg, float to_deg, float radius,
                const Body& body) {
    const float a = (from_deg + (to_deg - from_deg) * MinJerk((t - t0) / duration)) * kDeg;
    const Vec3 dir{0.0f, std::sin(a), -std::cos(a)};
    return Sample(t, Add(body.shoulder, Scale(dir, radius)), PitchQuat(a), body);
}

/// Prints events (for a failing check).
void Dump(const char* what, const std::vector<SwingEvent>& events) {
    std::printf("  %s: %zu events\n", what, events.size());
    for (const SwingEvent& e : events) {
        std::printf("    %-7s t=%.3f onset=%.3f peak=%.2f m/s arc=%.2f m dir=(%.2f %.2f %.2f)\n",
                    AttackName(e.kind), e.t, e.onset_t, e.peak_speed, e.arc, e.direction.x,
                    e.direction.y, e.direction.z);
    }
}

/// Runs `make` from t0 to t1 at 90 Hz and collects the events.
std::vector<SwingEvent> Run(SwingClassifier& c, double& t, double t1,
                            const std::function<HandSample(double)>& make) {
    std::vector<SwingEvent> events;
    for (; t < t1; t += kDt) {
        const SwingEvent e = c.Update(make(t));
        if (e.kind != Attack::None) {
            events.push_back(e);
        }
    }
    return events;
}

/// Holds the hand still at the slash's start for 0.5 s (arms the classifier), then slashes.
std::vector<SwingEvent> OneSlash(SwingClassifier& c, double duration, float from_deg,
                                 float to_deg, float radius) {
    Body body;
    double t = 0.0;
    return Run(c, t, 0.5 + duration + 0.4,
               [&](double s) { return Slash(s, 0.5, duration, from_deg, to_deg, radius, body); });
}

// ---- Classifier ---------------------------------------------------------------------------------

/// A slow 80 degree slash (probe peak ~3 m/s): one light attack, decided within decide_s of onset.
void TestSlowSwingIsLight() {
    SwingClassifier c;
    const auto events = OneSlash(c, 0.60, -50.0f, 30.0f, 0.45f);
    CHECK(events.size() == 1);
    if (!events.empty()) {
        CHECK(events[0].kind == Attack::Light);
        CHECK(events[0].peak_speed > 2.2f && events[0].peak_speed < 4.6f);
        CHECK(events[0].t - events[0].onset_t <= c.Tuning().decide_s + 2.5 * kDt);
        CHECK(events[0].arc >= c.Tuning().min_arc);
        // Moving to the left (+angle): -X.
        CHECK(events[0].direction.x < -0.5f);
    }
}

/// A fast 110 degree slash (probe peak ~8 m/s): one heavy attack, decided early.
void TestFastSwingIsHeavy() {
    SwingClassifier c;
    const auto events = OneSlash(c, 0.30, -60.0f, 50.0f, 0.50f);
    CHECK(events.size() == 1);
    if (!events.empty()) {
        CHECK(events[0].kind == Attack::Heavy);
        CHECK(events[0].t - events[0].onset_t < 0.08);
    }
}

/// A medium-fast slash just over the heavy speed (~5 m/s) is heavy; one just under it is light.
void TestHeavyBoundary() {
    {
        SwingClassifier c;
        const auto events = OneSlash(c, 0.40, -55.0f, 55.0f, 0.45f); // peak ~6.3 m/s
        CHECK(events.size() == 1 && events[0].kind == Attack::Heavy);
    }
    {
        SwingClassifier c;
        const auto events = OneSlash(c, 0.55, -40.0f, 40.0f, 0.45f); // peak ~3.3 m/s
        CHECK(events.size() == 1 && events[0].kind == Attack::Light);
    }
}

/// A small, slow wave of the hand is not an attack.
void TestWeakMotionIgnored() {
    SwingClassifier c;
    const auto events = OneSlash(c, 0.70, -10.0f, 10.0f, 0.45f); // 0.24 m at ~0.7 m/s peak
    CHECK(events.empty());
}

/// The runtime's own velocities give the same answer as differentiated poses.
void TestRuntimeVelocity() {
    SwingClassifier c;
    Body body;
    double t = 0.0;
    const double t0 = 0.5, duration = 0.6;
    const float from = -50.0f * kDeg, to = 30.0f * kDeg, radius = 0.45f;
    const auto events = Run(c, t, t0 + 1.0, [&](double s) {
        const double u = (s - t0) / duration;
        const float a = from + (to - from) * MinJerk(u);
        const double x = std::clamp(u, 0.0, 1.0);
        const float rate = float((to - from) * (30.0 * x * x - 60.0 * x * x * x + 30.0 * x * x * x * x) / duration);
        const Vec3 dir{-std::sin(a), 0.0f, -std::cos(a)};
        HandSample h = Sample(s, Add(body.shoulder, Scale(dir, radius)), YawQuat(a), body, 0.0f);
        h.has_velocity = true;
        h.angular_velocity = {0.0f, rate, 0.0f};
        h.velocity = Cross(h.angular_velocity, Scale(dir, radius));
        return h;
    });
    CHECK(events.size() == 1 && events[0].kind == Attack::Light);
}

/// The stroke there and straight back as one continuous motion (a sine of the angle): the hand
/// turns without pausing.
HandSample ThereAndBack(double s, double t0, double half, const Body& body) {
    const double u = (s - t0) / (2.0 * half);
    const float a = (-50.0f + 90.0f * std::sin(kPi * float(std::clamp(u, 0.0, 1.0)))) * kDeg;
    const Vec3 dir{-std::sin(a), 0.0f, -std::cos(a)};
    return Sample(s, Add(body.shoulder, Scale(dir, 0.45f)), YawQuat(a), body);
}

/// Forehand then straight back: two attacks. A slow back-and-forth re-arms at the turn; a fast
/// one turns too quickly for that, and only the stroke-back rule keeps the second attack.
void TestForehandBackhand() {
    Body body;
    for (const double half : {0.40, 0.22}) {
        SwingClassifier c;
        double t = 0.0;
        const auto events = Run(c, t, 2.0, [&](double s) { return ThereAndBack(s, 0.5, half, body); });
        CHECK(events.size() == 2);
        if (events.size() == 2) {
            CHECK(events[0].direction.x < 0.0f && events[1].direction.x > 0.0f);
        } else {
            Dump("there and back", events);
        }
    }
    SwingTuning slow;
    slow.reverse_rearm = false;
    SwingClassifier d(slow);
    double t = 0.0;
    const auto once = Run(d, t, 2.0, [&](double s) { return ThereAndBack(s, 0.5, 0.22, body); });
    CHECK(once.size() == 1);
    if (once.size() != 1) {
        Dump("there and back, no reversal re-arm", once);
    }
}

/// Walking (1.6 m/s jog, head bob, arm sway, 3 mm noise) for 8 s: no attacks. A slash made while
/// walking still counts (the head compensation measures the hand against the body).
HandSample Walk(double t, double slash_t0) {
    Body body;
    const float bob = 0.03f * std::sin(2.0f * kPi * 1.9f * float(t));
    body.head = {0.0f, 1.65f + bob, -1.6f * float(t)};
    body.head_rot = YawQuat(3.0f * kDeg * std::sin(2.0f * kPi * 0.95f * float(t)));
    const float sway = 0.12f * std::sin(2.0f * kPi * 0.95f * float(t));
    const Vec3 rest = Add(body.head, {0.25f, -0.55f + 0.5f * bob, -0.30f + sway});
    if (slash_t0 >= 0.0 && t >= slash_t0 - 1.0 && t < slash_t0 + 0.9) {
        Body moving = body; // the slash about the moving shoulder
        moving.shoulder = Add(body.head, {0.20f, -0.20f, 0.0f});
        const HandSample slash = Slash(t, slash_t0, 0.55, -50.0f, 30.0f, 0.45f, moving);
        if (t >= slash_t0) {
            return slash;
        }
        // Bring the hand to the slash's start over 0.8 s (an unhurried reach, not an attack).
        HandSample h = slash;
        const float k = MinJerk((t - (slash_t0 - 1.0)) / 0.8);
        h.position = Add(rest, Scale(Sub(slash.position, rest), k));
        h.orientation = Nlerp(PitchQuat(-30.0f * kDeg), slash.orientation, k);
        return h;
    }
    return Sample(t, rest, PitchQuat(-30.0f * kDeg), body, 0.003f);
}

void TestWalkingIgnored() {
    SwingClassifier c;
    double t = 0.0;
    CHECK(Run(c, t, 8.0, [](double s) { return Walk(s, -1.0); }).empty());
    SwingClassifier d;
    t = 0.0;
    const auto events = Run(d, t, 8.0, [](double s) { return Walk(s, 4.0); });
    CHECK(events.size() == 1 && events[0].kind == Attack::Light);
    if (events.size() != 1) {
        Dump("slash while walking", events);
    }
}

/// A minute of standing with 3 mm tracking noise, slow drifts and laser-pointer wrist turns
/// (+-20 degrees at 1.5 Hz, as when pointing at a menu): no attacks.
void TestIdleAndPointingIgnored() {
    SwingClassifier c;
    Body body;
    double t = 0.0;
    const auto events = Run(c, t, 60.0, [&](double s) {
        const float drift = 0.05f * std::sin(2.0f * kPi * 0.1f * float(s));
        const float aim = 20.0f * kDeg * std::sin(2.0f * kPi * 1.5f * float(s));
        const Vec3 hand{0.25f + drift, 1.20f, -0.40f};
        return Sample(s, hand, YawQuat(aim), body, 0.003f);
    });
    CHECK(events.empty());
    if (!events.empty()) {
        Dump("idle and pointing", events);
    }
}

/// Looking around (the head yaws +-70 degrees, the hands still): no attacks.
void TestHeadTurnIgnored() {
    SwingClassifier c;
    double t = 0.0;
    const auto events = Run(c, t, 6.0, [](double s) {
        Body body;
        const float yaw = 70.0f * kDeg * std::sin(2.0f * kPi * 0.7f * float(s));
        body.head_rot = YawQuat(yaw);
        body.head = Add({0.0f, 1.55f, 0.0f}, Rotate(body.head_rot, {0.0f, 0.10f, -0.08f}));
        return Rest(s, body);
    });
    CHECK(events.empty());
}

/// A quick 90 degree body turn carries the hand around at ~3 m/s at the probe: no attack (the
/// turn debit), although it would be a light attack without it.
void TestBodyTurnIgnored() {
    const auto turn = [](double s) {
        Body body;
        const float yaw = 90.0f * kDeg * MinJerk((s - 0.5) / 0.6);
        body.head_rot = YawQuat(yaw);
        const Quat r = YawQuat(yaw);
        const Vec3 hand = Rotate(r, {0.25f, 1.20f, -0.35f});
        const Quat grip = YawQuat(yaw); // blade level and forward, turning with the body
        return Sample(s, hand, grip, body);
    };
    SwingClassifier c;
    double t = 0.0;
    CHECK(Run(c, t, 2.0, turn).empty());
    SwingTuning raw = c.Tuning();
    raw.turn_comp = 0.0f;
    SwingClassifier d(raw);
    t = 0.0;
    CHECK(!Run(d, t, 2.0, turn).empty());
}

/// A tracking glitch (the hand jumps 0.6 m for one sample) and a tracking loss: no attacks.
void TestGlitchIgnored() {
    SwingClassifier c;
    Body body;
    double t = 0.0;
    std::vector<SwingEvent> events = Run(c, t, 3.0, [&](double s) {
        HandSample h = Rest(s, body);
        if (std::fabs(s - 1.0) < 0.5 * kDt) {
            h.position = Add(h.position, {0.6f, 0.0f, 0.0f});
        }
        if (s > 2.0 && s < 2.3) {
            h.valid = false;
        }
        return h;
    });
    CHECK(events.empty());
}

/// Hold the wind-up still overhead: charge (R2 held); the chop out of it releases. A hand raised
/// in front of the face (adjusting the headset) does not charge.
void TestChargedHeavy() {
    SwingClassifier c;
    Body body;
    double t = 0.0;
    // Hold the hand low in front, raise it over and behind the head (0.4 s: a fast wind-up, not
    // an attack), hold it still: charge.
    auto events = Run(c, t, 1.7, [&](double s) { return Chop(s, 0.5, 0.4, -30.0f, 100.0f, 0.5f, body); });
    CHECK(events.size() == 1 && events[0].kind == Attack::ChargeStart);
    const double t1 = t;
    auto chop = Run(c, t, t1 + 0.8, [&](double s) { return Chop(s, t1, 0.35, 100.0f, -30.0f, 0.5f, body); });
    CHECK(chop.size() == 1 && chop[0].kind == Attack::ChargeRelease);
    if (!chop.empty()) {
        CHECK(chop[0].t - t1 < 0.2);
    }
    // The same wind-up followed at once by the chop (no hold): one heavy attack, not two.
    SwingClassifier q;
    t = 0.0;
    const auto quick = Run(q, t, 2.0, [&](double s) {
        return s < 0.9 ? Chop(s, 0.5, 0.4, -30.0f, 100.0f, 0.5f, body)
                       : Chop(s, 0.9, 0.35, 100.0f, -30.0f, 0.5f, body);
    });
    CHECK(quick.size() == 1 && quick[0].kind == Attack::Heavy);

    // The hand held up in front of the face: no charge.
    SwingClassifier d;
    t = 0.0;
    const auto face = Run(d, t, 3.0, [&](double s) {
        return Sample(s, {0.10f, 1.68f, -0.15f}, PitchQuat(60.0f * kDeg), body);
    });
    CHECK(face.empty());

    // Charged and then lowered slowly without a swing: released after charge_drop_s.
    SwingClassifier e;
    t = 0.0;
    auto up = Run(e, t, 1.7, [&](double s) { return Chop(s, 0.5, 0.4, -30.0f, 100.0f, 0.5f, body); });
    const double u1 = t;
    auto down = Run(e, t, u1 + 3.0, [&](double s) { return Chop(s, u1, 2.5, 100.0f, -30.0f, 0.5f, body); });
    CHECK(up.size() == 1 && up[0].kind == Attack::ChargeStart);
    CHECK(down.size() == 1 && down[0].kind == Attack::ChargeRelease);
}

/// The readout of strokes that are not attacks (Debug::stroke, windup, dropped): a slash between
/// the onset and the light speed is dropped as too slow; a fast rising stroke is dropped as a
/// wind-up; a swing in progress shows its onset and clears once decided. Readout only: the events
/// are the same as without it.
void TestDroppedStrokes() {
    {
        SwingClassifier c;
        const auto events = OneSlash(c, 0.60, -20.0f, 20.0f, 0.45f); // probe peak ~1.7 m/s
        CHECK(events.empty());
        const SwingClassifier::Stroke& d = c.State().dropped;
        CHECK(d.count == 1);
        CHECK(!d.windup);
        CHECK(d.peak > c.Tuning().onset_speed && d.peak < c.Tuning().light_speed);
        CHECK(d.t > d.onset_t && d.onset_t > 0.5);
        CHECK(!c.State().stroke);
    }
    {
        // The slow light slash: the stroke shows from its onset until the decision.
        SwingClassifier c;
        Body body;
        double t = 0.0, stroke_onset = -1.0;
        bool stroke_before = false, stroke_at_event = true;
        SwingEvent event;
        for (; t < 1.5; t += kDt) {
            const SwingEvent e = c.Update(Slash(t, 0.5, 0.60, -50.0f, 30.0f, 0.45f, body));
            if (e.kind != Attack::None) {
                event = e;
                stroke_at_event = c.State().stroke;
                break;
            }
            if (c.State().stroke) {
                stroke_before = true;
                stroke_onset = c.State().onset_t;
            }
        }
        CHECK(event.kind == Attack::Light);
        CHECK(stroke_before && !stroke_at_event);
        CHECK(std::fabs(stroke_onset - event.onset_t) < 1e-9);
        CHECK(c.State().dropped.count == 0);
    }
    {
        // The fast wind-up over the head, then held still: dropped as a wind-up, then the charge.
        SwingClassifier c;
        Body body;
        double t = 0.0;
        const auto events =
            Run(c, t, 1.7, [&](double s) { return Chop(s, 0.5, 0.4, -30.0f, 100.0f, 0.5f, body); });
        CHECK(events.size() == 1 && events[0].kind == Attack::ChargeStart);
        CHECK(c.State().dropped.count == 1 && c.State().dropped.windup);
        CHECK(c.State().dropped.peak >= c.Tuning().light_speed);
    }
}

/// One long one-way slash (90 to 150 degrees, fast to slow) is one attack: an attack decided
/// early in the arc must not re-arm on the rest of the arc as "a stroke back the other way" (its
/// direction turns past 107 degrees from the decision's). The stroke-back rule compares with the
/// follow-through's latest fast direction (reverse_follow); without that, about a third of these
/// slashes were two attacks. Forehand then backhand stays two (TestForehandBackhand).
void TestLongArcIsOneAttack() {
    int one[2] = {}, total = 0;
    for (const bool follow : {true, false}) {
        for (const float arc : {90.0f, 110.0f, 130.0f, 150.0f}) {
            for (const double duration : {0.22, 0.30, 0.40, 0.55}) {
                SwingTuning t = TuningFor(WeaponClass::Quick);
                t.reverse_follow = follow;
                SwingClassifier c(t);
                const auto events = OneSlash(c, duration, -0.5f * arc, 0.5f * arc, 0.45f);
                one[follow ? 0 : 1] += events.size() == 1;
                total += follow;
                if (follow && events.size() != 1) {
                    std::printf("  arc %.0f deg in %.2f s:\n", arc, duration);
                    Dump("long arc", events);
                }
            }
        }
    }
    CHECK(total == 16);
    CHECK(one[0] == 16);     // with the rule: one attack each
    CHECK(one[1] < one[0]);  // without it some arcs were two
}

// ---- Scheduler ----------------------------------------------------------------------------------

struct Presses {
    std::vector<double> r1, r2;  // rising edges
    double min_gap_r1 = 1e9;     // shortest release between two R1 presses
    double r2_held = 0.0;        // total R2 hold time
};

/// Steps the scheduler at 90 Hz from t0 to t1, feeding events at their times.
Presses Drive(AttackScheduler& s, double t0, double t1, std::vector<SwingEvent> events,
              const std::function<bool(double)>& can_act, std::vector<AttackScheduler::Out>* outs = nullptr) {
    Presses p;
    uint32_t prev = 0;
    double r1_up = -1e9;
    size_t next = 0;
    for (double t = t0; t < t1; t += kDt) {
        SwingEvent e;
        if (next < events.size() && events[next].t <= t + 1e-9) {
            e = events[next++];
        }
        const AttackScheduler::Out o = s.Update(t, e, can_act(t));
        if (outs) {
            outs->push_back(o);
        }
        if ((o.buttons & kButtonR1) && !(prev & kButtonR1)) {
            if (!p.r1.empty()) {
                p.min_gap_r1 = std::min(p.min_gap_r1, t - r1_up);
            }
            p.r1.push_back(t);
        }
        if (!(o.buttons & kButtonR1) && (prev & kButtonR1)) {
            r1_up = t;
        }
        if ((o.buttons & kButtonR2) && !(prev & kButtonR2)) {
            p.r2.push_back(t);
        }
        if (o.buttons & kButtonR2) {
            p.r2_held += kDt;
        }
        prev = o.buttons;
    }
    return p;
}

SwingEvent At(double t, Attack kind) {
    SwingEvent e;
    e.kind = kind;
    e.t = t;
    e.onset_t = t - 0.08;
    return e;
}

bool Near(double a, double b, double tolerance = 1.5 * kDt) {
    return std::fabs(a - b) <= tolerance;
}

/// Swings faster than the moveset: the first fires, the next waits for the combo window (input
/// open = 0.33 s after the press), a newer one replaces a waiting one, presses never merge.
void TestComboTiming() {
    AttackScheduler s;
    std::vector<AttackScheduler::Out> outs;
    const Presses p = Drive(s, 0.0, 3.0,
                            {At(1.00, Attack::Light), At(1.10, Attack::Light), At(1.36, Attack::Light),
                             At(1.40, Attack::Light)},
                            [](double) { return true; }, &outs);
    CHECK(p.r1.size() == 3);
    if (p.r1.size() == 3) {
        CHECK(Near(p.r1[0], 1.00));
        CHECK(Near(p.r1[1], 1.33));
        CHECK(Near(p.r1[2], 1.66));
    }
    CHECK(p.min_gap_r1 >= 0.05 - 1e-6);
    int superseded = 0, queued = 0;
    for (const auto& o : outs) {
        superseded += o.dropped == Drop::Superseded;
        queued += o.queued;
    }
    CHECK(superseded == 1);
    CHECK(queued == 3);

    // A swing long before the window (queue 0.2 s) is dropped; one after recovery fires at once.
    SchedulerTuning tight;
    tight.queue_s = 0.2;
    AttackScheduler s2(tight);
    std::vector<AttackScheduler::Out> outs2;
    const Presses p2 = Drive(s2, 0.0, 3.0,
                             {At(1.00, Attack::Light), At(1.02, Attack::Light), At(2.00, Attack::Light)},
                             [](double) { return true; }, &outs2);
    CHECK(p2.r1.size() == 2);
    if (p2.r1.size() == 2) {
        CHECK(Near(p2.r1[1], 2.00));
    }
    int early = 0;
    for (const auto& o : outs2) {
        early += o.dropped == Drop::TooEarly;
    }
    CHECK(early == 1);

    // A heavy follow-up presses R2; the same button twice in a row keeps a release gap.
    AttackScheduler s3;
    const Presses p3 = Drive(s3, 0.0, 3.0, {At(1.00, Attack::Heavy), At(1.05, Attack::Heavy)},
                             [](double) { return true; });
    CHECK(p3.r2.size() == 2);
    if (p3.r2.size() == 2) {
        CHECK(Near(p3.r2[1], 1.00 + s3.Tuning().heavy.input_open));
    }

    // The game ended the attack early (a stagger): the next swing fires at once.
    AttackScheduler s4;
    s4.Update(1.0, At(1.0, Attack::Light), true);
    s4.AttackEnded();
    const AttackScheduler::Out o4 = s4.Update(1.1, At(1.1, Attack::Light), true);
    CHECK(o4.fired == Attack::Light);
}

/// Charge: R2 held from ChargeStart to ChargeRelease; a blocked (menu) swing never presses.
void TestChargeAndBlocked() {
    AttackScheduler s;
    const Presses p = Drive(s, 0.0, 3.0, {At(0.50, Attack::ChargeStart), At(1.50, Attack::ChargeRelease)},
                            [](double) { return true; });
    CHECK(p.r2.size() == 1);
    CHECK(std::fabs(p.r2_held - 1.0) < 2.5 * kDt);

    AttackScheduler b;
    const AttackScheduler::Out o = b.Update(1.0, At(1.0, Attack::Light), true, true);
    CHECK(o.dropped == Drop::Blocked && o.buttons == 0);
}

// ---- Stamina ------------------------------------------------------------------------------------

StaminaTuning Sandbox() {
    StaminaTuning t;
    t.max = 100.0f;
    t.regen_per_s = 45.0f;
    t.regen_delay_s = 0.75;
    t.hit_window_s = 0.6;
    t.dash_cost = 20.0f;
    return t;
}

/// A swing that never connects costs nothing; its window ends with a Refunded event.
void TestMissIsFree() {
    StaminaModel m(Sandbox());
    m.Reset(0.0);
    m.Mirror(50.0f, 100.0f);
    const uint32_t id = m.BeginSwing(0.0, 20.0f);
    CHECK(id != 0);
    CHECK(m.Pending() == 20.0f);
    int refunds = 0;
    for (double t = 0.0; t < 0.74; t += kDt) {
        StaminaEvent ev[4];
        const int n = m.Update(t, ev, 4);
        for (int i = 0; i < n; ++i) {
            refunds += ev[i].kind == StaminaEvent::Kind::Refunded && ev[i].swing == id;
        }
    }
    CHECK(refunds == 1);
    CHECK(m.Current() == 50.0f); // no charge, and no regen yet (the swing's animation pauses it)
    CHECK(m.Pending() == 0.0f);
}

/// A swing that connects pays once, however many targets it hits; a hit after the window is free.
void TestHitPaysOnce() {
    StaminaModel m(Sandbox());
    m.Reset(0.0);
    const uint32_t a = m.BeginSwing(0.0, 20.0f);
    m.Update(0.2);
    CHECK(m.Hit(0.30) == 20.0f);
    CHECK(m.Hit(0.32) == 0.0f);
    CHECK(m.Current() == 80.0f);
    CHECK(m.LastEvent().kind == StaminaEvent::Kind::Charged && m.LastEvent().swing == a);
    const uint32_t b = m.BeginSwing(1.0, 35.0f);
    CHECK(b != 0);
    m.Update(1.7);
    CHECK(m.Hit(1.7, b) == 0.0f); // its window ended at 1.6
    CHECK(m.Current() <= 80.0f + 1e-3f);
}

/// At 0 or below nothing starts: no swing, no dash (and the scheduler drops the press).
void TestGateAtZero() {
    StaminaModel m(Sandbox());
    m.Reset(0.0);
    m.Mirror(0.0f, 100.0f);
    CHECK(!m.CanAct());
    CHECK(m.BeginSwing(0.0, 20.0f) == 0);
    CHECK(m.LastEvent().kind == StaminaEvent::Kind::Refused);
    CHECK(!m.TryDash(0.0));
    CHECK(m.Current() == 0.0f);

    // Above 0 by a hair, an action is allowed and may go negative (the game's rule).
    m.Mirror(0.5f, 100.0f);
    CHECK(m.TryDash(0.1));
    CHECK(std::fabs(m.Current() - (-19.5f)) < 1e-4f);
    CHECK(!m.CanAct());

    AttackScheduler s;
    const AttackScheduler::Out o = s.Update(0.2, At(0.2, Attack::Light), m.CanAct());
    CHECK(o.dropped == Drop::Tired && o.buttons == 0);

    // The cost is clamped at the floor.
    m.Mirror(5.0f, 100.0f);
    m.BeginSwing(5.0, 80.0f);
    CHECK(m.Hit(5.2) == 80.0f);
    CHECK(m.Current() == -50.0f);
}

/// Regen: nothing during the pause after an action, then 45/s up to the maximum.
void TestRegen() {
    StaminaModel m(Sandbox());
    m.Reset(0.0);
    m.Mirror(40.0f, 100.0f);
    CHECK(m.TryDash(0.0)); // 20 -> pause until 0.75
    CHECK(m.LastEvent().kind == StaminaEvent::Kind::DashCharged);
    double t = 0.0;
    for (; t < 0.70; t += kDt) {
        m.Update(t);
    }
    CHECK(m.Current() == 20.0f);
    for (; t < 1.75; t += kDt) {
        m.Update(t);
    }
    CHECK(std::fabs(m.Current() - (20.0f + 45.0f * float(1.75 - kDt - 0.75))) < 1.0f);
    for (; t < 4.0; t += kDt) {
        m.Update(t);
    }
    CHECK(m.Current() == 100.0f);

    // Back above 0 after exhaustion: -19.5 needs the 0.75 s pause plus 0.43 s of regen.
    StaminaModel n(Sandbox());
    n.Reset(0.0);
    n.Mirror(0.5f, 100.0f);
    n.TryDash(0.0);
    double back = -1.0;
    for (double u = 0.0; u < 3.0; u += kDt) {
        n.Update(u);
        if (back < 0.0 && n.CanAct()) {
            back = u;
        }
    }
    CHECK(back > 1.10 && back < 1.30);

    // Game mode: regen 0, the game's value mirrored; the model only holds the open swings.
    StaminaTuning game = Sandbox();
    game.regen_per_s = 0.0f;
    StaminaModel g(game);
    g.Reset(0.0);
    g.Mirror(30.0f, 91.0f);
    for (double u = 0.0; u < 2.0; u += kDt) {
        g.Update(u);
    }
    CHECK(g.Current() == 30.0f && g.Max() == 91.0f);
}

/// A dash with its own cost (dodge, backstep, jump): paid whole while above 0, refused at or below.
void TestDashCost() {
    StaminaModel m(Sandbox());
    m.Reset(0.0);
    m.Mirror(50.0f, 100.0f);
    CHECK(m.TryDash(0.0, 35.0f));
    CHECK(m.Current() == 15.0f);
    CHECK(m.LastEvent().kind == StaminaEvent::Kind::DashCharged && m.LastEvent().amount == 35.0f);
    CHECK(m.TryDash(0.1, 30.0f)); // 15 > 0: allowed, goes negative
    CHECK(m.Current() == -15.0f);
    CHECK(!m.TryDash(0.2, 10.0f));
    CHECK(m.LastEvent().kind == StaminaEvent::Kind::Refused && m.LastEvent().amount == 10.0f);
    CHECK(m.Current() == -15.0f);
    m.Mirror(5.0f, 100.0f);
    CHECK(m.TryDash(0.3, 80.0f) && m.Current() == -50.0f); // the floor
}

/// Classifier -> scheduler -> stamina, the sandbox loop. Five slashes 0.9 s apart, 30 stamina
/// each, from 45: the 1st hits (15), the 2nd misses (free), the 3rd hits (-1.5 after a little
/// regen), the 4th is still allowed (5.25 > 0) and hits (-24.75), the 5th comes while exhausted
/// and does not register. Between swings only ~0.15 s of regen happens (the 0.75 s pause).
void TestEndToEnd() {
    SwingClassifier c;
    AttackScheduler sched;
    StaminaTuning st = Sandbox();
    st.max = 45.0f;
    StaminaModel m(st);
    m.Reset(0.0);
    Body body;
    const double starts[] = {0.5, 1.4, 2.3, 3.2, 4.1};
    const bool connects[] = {true, false, true, true, true};
    int fired = 0, tired = 0, charged = 0, refunded = 0, tired_swing = -1;
    float after_first = 0.0f;
    int swing = -1;
    double press_t = -1.0;
    for (double t = 0.0; t < 6.0; t += kDt) {
        // Alternate forehand and backhand; between swings the hand holds where the last ended.
        int k = 0;
        while (k < 4 && t >= starts[k + 1]) {
            ++k;
        }
        const HandSample h = Slash(t, starts[k], 0.45, (k % 2) ? 40.0f : -50.0f,
                                   (k % 2) ? -50.0f : 40.0f, 0.45f, body);
        const SwingEvent e = c.Update(h);
        const AttackScheduler::Out o = sched.Update(t, e, m.CanAct());
        if (o.dropped == Drop::Tired) {
            ++tired;
            tired_swing = k;
        }
        if (o.fired == Attack::Light || o.fired == Attack::Heavy) {
            ++fired;
            CHECK(m.BeginSwing(t, 30.0f) != 0);
            swing = k;
            press_t = t;
        }
        // The weapon meets the target 0.15 s after the press, on the swings that connect.
        if (press_t >= 0.0 && t >= press_t + 0.15 && t < press_t + 0.15 + kDt && connects[swing]) {
            charged += m.Hit(t) > 0.0f;
            if (swing == 0) {
                after_first = m.Current();
            }
        }
        StaminaEvent ev[4];
        const int n = m.Update(t, ev, 4);
        for (int i = 0; i < n; ++i) {
            refunded += ev[i].kind == StaminaEvent::Kind::Refunded;
        }
    }
    CHECK(fired == 4);
    CHECK(tired == 1 && tired_swing == 4);
    CHECK(charged == 3);
    CHECK(refunded == 1);
    CHECK(std::fabs(after_first - 15.0f) < 0.5f);
    CHECK(m.Current() == 45.0f); // 1.9 s of regen after the last swing refills it
}

} // namespace

// ---- Trick-weapon transform (trick_transform.h, doc 33) ----------------------------------------

Quat QMul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

/// A wrist snap in place: the grip turns `deg` about the vertical (axis 0, sweeps the probe
/// sideways), the blade (axis 1, roll) or the hand's X (axis 2) from t0 over `duration`; the hand
/// stays where it is (`travel` m of drift along -Z over the snap).
HandSample Snap(double t, double t0, double duration, float deg, int axis, const Body& body,
                float travel = 0.0f) {
    const float u = MinJerk((t - t0) / duration);
    const float a = deg * u * kDeg;
    Quat q = YawQuat(0.0f);
    if (axis == 0) {
        q = YawQuat(a);
    } else if (axis == 1) {
        q = {0.0f, 0.0f, std::sin(a * 0.5f), std::cos(a * 0.5f)};
    } else {
        q = PitchQuat(a);
    }
    return Sample(t, {0.25f, 1.20f, -0.30f - travel * u}, q, body, 0.001f);
}

struct SnapRun {
    int fired = 0;
    double fire_t = -1.0;      // seconds after the snap started
    float angle = 0.0f;
    int attacks = 0;           // the swing classifier's, fed only while there is no veto
    int attacks_raw = 0;       // ... and fed everything
    double veto_at = -1.0;     // first veto, seconds after the snap started
    double attack_at = -1.0;   // first attack of the raw classifier
};

/// Settles 0.5 s, snaps once (and again at `again` s after its start when > 0), runs to `end`.
SnapRun RunSnap(double duration, float deg, int axis, float travel = 0.0f, double again = 0.0,
                double end = 2.5) {
    Body body;
    SnapRun r;
    TrickTransformDetector det;
    SwingClassifier gated, raw;
    auto pose = [&](double t) {
        if (again > 0.0 && t >= 0.5 + again) {
            // the second snap goes back to where the first one started
            HandSample h = Snap(t, 0.5 + again, duration, -deg, axis, body, 0.0f);
            Quat q = h.orientation;
            const HandSample first = Snap(5.0, 0.5, duration, deg, axis, body, travel);
            h.orientation = QMul(first.orientation, q);
            h.position = first.position;
            return h;
        }
        return Snap(t, 0.5, duration, deg, axis, body, travel);
    };
    for (double t = 0.0; t < end; t += kDt) {
        const HandSample h = pose(t);
        const TransformEvent e = det.Update(h);
        if (e.fired) {
            ++r.fired;
            if (r.fire_t < 0.0) {
                r.fire_t = t - 0.5;
                r.angle = e.angle_deg;
            }
        }
        if (det.Veto() && r.veto_at < 0.0) {
            r.veto_at = t - 0.5;
        }
        if (!det.Veto() && gated.Update(h).kind != Attack::None) {
            ++r.attacks;
        }
        if (raw.Update(h).kind != Attack::None) {
            if (r.attack_at < 0.0) {
                r.attack_at = t - 0.5;
            }
            ++r.attacks_raw;
        }
    }
    return r;
}

/// A 100 degree snap in 120 ms fires once, on every axis, as it passes 90 degrees; the swing
/// classifier alone would call the sideways one an attack, the veto stops that before it decides.
void TestTransformSnapFires() {
    for (int axis = 0; axis < 3; ++axis) {
        const SnapRun r = RunSnap(0.12, 100.0f, axis);
        CHECK(r.fired == 1);
        CHECK(r.fire_t > 0.0 && r.fire_t < 0.16);
        CHECK(r.angle >= 90.0f);
        CHECK(r.veto_at >= 0.0 && r.veto_at < 0.10);
        CHECK(r.attacks == 0);
    }
    const SnapRun side = RunSnap(0.12, 100.0f, 0);
    CHECK(side.attacks_raw >= 1); // the problem the veto solves
    CHECK(side.veto_at < side.attack_at);
    // Backwards (the other direction) fires as well.
    CHECK(RunSnap(0.12, -100.0f, 1).fired == 1);
}

/// Too small, too slow, or with the hand travelling: not a transform.
void TestTransformRejects() {
    CHECK(RunSnap(0.12, 60.0f, 1).fired == 0);   // not far enough
    CHECK(RunSnap(0.40, 100.0f, 1).fired == 0);  // a casual turn of the wrist
    CHECK(RunSnap(0.12, 100.0f, 1, 0.35f).fired == 0); // the hand travels: a swing, not a snap
    CHECK(RunSnap(0.40, 100.0f, 1).veto_at < 0.0);     // ... and a casual turn never vetoes
}

/// One transform per cooldown: the snap back does not transform again.
void TestTransformCooldown() {
    CHECK(RunSnap(0.12, 100.0f, 1, 0.0f, 0.4).fired == 1);
    CHECK(RunSnap(0.12, 100.0f, 1, 0.0f, 1.2).fired == 2);
}

/// Real slashes (light and heavy, forehand) never fire it or veto the classifier, and are still
/// attacks with the gate in place.
void TestTransformSlashUnaffected() {
    Body body;
    const struct {
        double duration;
        float from, to, radius;
    } slashes[] = {{0.30, -60.0f, 50.0f, 0.50f}, {0.55, -40.0f, 40.0f, 0.45f},
                   {0.18, -70.0f, 40.0f, 0.55f}};
    for (const auto& s : slashes) {
        TrickTransformDetector det;
        SwingClassifier c;
        int attacks = 0, fired = 0, vetoed = 0;
        for (double t = 0.0; t < 1.5 + s.duration; t += kDt) {
            const HandSample h = Slash(t, 0.5, s.duration, s.from, s.to, s.radius, body);
            fired += det.Update(h).fired;
            vetoed += det.Veto();
            if (!det.Veto() && c.Update(h).kind != Attack::None) {
                ++attacks;
            }
        }
        CHECK(fired == 0);
        CHECK(vetoed == 0);
        CHECK(attacks == 1);
    }
}

/// A tracking glitch, lost tracking and a disabled detector never fire.
void TestTransformGlitchAndOff() {
    Body body;
    {
        TrickTransformDetector det;
        int fired = 0;
        for (double t = 0.0; t < 1.0; t += kDt) {
            HandSample h = Snap(t, 0.5, 0.12, 0.0f, 1, body);
            if (t > 0.5 && t < 0.52) {
                h.position.x += 0.6f; // a jump
                h.orientation = YawQuat(2.5f);
            }
            fired += det.Update(h).fired;
        }
        CHECK(fired == 0);
    }
    {
        TransformTuning off;
        off.enabled = false;
        TrickTransformDetector det(off);
        int fired = 0;
        for (double t = 0.0; t < 1.5; t += kDt) {
            fired += det.Update(Snap(t, 0.5, 0.12, 100.0f, 1, body)).fired;
        }
        CHECK(fired == 0 && !det.Veto());
    }
    {
        TrickTransformDetector det;
        int fired = 0;
        for (double t = 0.0; t < 1.5; t += kDt) {
            HandSample h = Snap(t, 0.5, 0.12, 100.0f, 1, body);
            h.valid = (t < 0.52 || t > 0.62);
            fired += det.Update(h).fired;
        }
        CHECK(fired == 0); // the snap's start and end are in different measurements
    }
}

int main() {
    TestSlowSwingIsLight();
    TestFastSwingIsHeavy();
    TestHeavyBoundary();
    TestWeakMotionIgnored();
    TestRuntimeVelocity();
    TestForehandBackhand();
    TestWalkingIgnored();
    TestIdleAndPointingIgnored();
    TestHeadTurnIgnored();
    TestBodyTurnIgnored();
    TestGlitchIgnored();
    TestChargedHeavy();
    TestDroppedStrokes();
    TestLongArcIsOneAttack();
    TestComboTiming();
    TestChargeAndBlocked();
    TestMissIsFree();
    TestHitPaysOnce();
    TestGateAtZero();
    TestRegen();
    TestDashCost();
    TestEndToEnd();
    TestTransformSnapFires();
    TestTransformRejects();
    TestTransformCooldown();
    TestTransformSlashUnaffected();
    TestTransformGlitchAndOff();
    std::printf("swing_logic_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
