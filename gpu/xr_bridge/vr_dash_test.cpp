// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_dash_test.exe: offline tests of the comfort dash filter (vr_dash.h), the code the
// sandbox and the game share (docs/vr/findings/19-dash-locomotion.md). A synthetic "game" produces
// the anchor the way Bloodborne's first-person eye moves through a roll (forward with the root
// motion, the head surging ahead, dipping 0.9 m, swaying), sampled at the game's frame rate and
// held between samples; the filter runs at the headset's 72 Hz. Exit code 0 when every check
// passes.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "vr_dash.h"

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

constexpr double kHz = 72.0;
constexpr float kPi = 3.14159265f;

VrDashV3 V(float x, float y, float z) {
    return vr_dash_v3(x, y, z);
}
float Dist(VrDashV3 a, VrDashV3 b) {
    return vr_dash_len(vr_dash_sub(a, b));
}
float Along(VrDashV3 p, VrDashV3 origin, VrDashV3 axis) {
    return (p.x - origin.x) * axis.x + (p.z - origin.z) * axis.z;
}
float Across(VrDashV3 p, VrDashV3 origin, VrDashV3 axis) {
    return -(p.x - origin.x) * axis.z + (p.z - origin.z) * axis.x;
}

/// One move of the synthetic game: the eye through a roll from `from` along unit `dir` (world,
/// horizontal), `distance` meters in `length` seconds (root motion: quick start, slow end), the
/// head surging ahead by up to `surge`, dipping by `dip`, swaying sideways by `sway`; `stop_at`
/// (0..1) freezes it there (a cancelled or blocked move).
struct Roll {
    VrDashV3 from{}, dir{0, 0, -1};
    float distance = 3.0f, length = 0.7f, surge = 0.25f, dip = 0.9f, sway = 0.08f, stop_at = 1.0f;
    float lift = 0.0f; // a jump: the eye rises this high in the middle
    VrDashV3 At(float tau) const {
        float x = std::clamp(tau / length, 0.0f, 1.0f);
        x = std::min(x, stop_at);
        const float root = distance * (1.0f - (1.0f - x) * (1.0f - x) * (1.0f + 0.5f * x));
        const float s = std::sin(kPi * x);
        const float ahead = root + surge * s * std::cos(0.5f * kPi * x);
        const float side = sway * std::sin(2.0f * kPi * x);
        const float y = -dip * s * s + lift * s;
        return V(from.x + dir.x * ahead - dir.z * side, from.y + y, from.z + dir.z * ahead + dir.x * side);
    }
    VrDashV3 End() const {
        return At(length);
    }
};

/// The game's anchor at time t when it only changes at `fps` (sample and hold); fps <= 0: every
/// update.
float Tick(double t, double fps) {
    return fps > 0.0 ? float(std::floor(t * fps + 1e-6) / fps) : float(t);
}

struct Sample {
    double t;
    VrDashV3 anchor, out;
    int mode;
    float vignette;
};

/// Runs the filter at 72 Hz over [t0, t1): `anchor_at(t)` gives the anchor, `move_at(t)` the move id.
template <class A, class M>
std::vector<Sample> Run(VrDash& d, const VrDashParams& p, double t0, double t1, A anchor_at, M move_at,
                        const VrDashHint* hint) {
    std::vector<Sample> out;
    for (double t = t0; t < t1 - 1e-9; t += 1.0 / kHz) {
        const VrDashV3 a = anchor_at(t);
        const VrDashV3 o = vr_dash_update(&d, &p, a, move_at(t), hint, t);
        out.push_back({t, a, o, d.mode, d.vignette});
    }
    return out;
}

VrDashHint Hint(VrDashV3 dir, float distance, int kind = VR_DASH_DODGE) {
    VrDashHint h{};
    h.kind = kind;
    h.direction = dir;
    h.distance = distance;
    h.height = -1.0f;
    return h;
}

/// Largest jump between consecutive outputs.
float MaxStep(const std::vector<Sample>& s) {
    float m = 0.0f;
    for (size_t i = 1; i < s.size(); ++i) {
        m = std::max(m, Dist(s[i].out, s[i - 1].out));
    }
    return m;
}

// ---- tests ----------------------------------------------------------------------------------

void TestEase() {
    for (int curve : {VR_DASH_EASE_POWER, VR_DASH_EASE_EXPO, VR_DASH_EASE_SMOOTH}) {
        float prev = 0.0f;
        bool monotonic = true;
        for (int i = 0; i <= 100; ++i) {
            const float e = vr_dash_ease(curve, 3.0f, float(i) / 100.0f);
            monotonic = monotonic && e >= prev - 1e-6f && e <= 1.0f + 1e-6f;
            prev = e;
        }
        CHECK(monotonic);
        CHECK(std::fabs(vr_dash_ease(curve, 3.0f, 0.0f)) < 1e-6f);
        CHECK(std::fabs(vr_dash_ease(curve, 3.0f, 1.0f) - 1.0f) < 1e-5f);
        // The rate is the derivative.
        const float u = 0.37f, h = 1e-3f;
        const float numeric = (vr_dash_ease(curve, 3.0f, u + h) - vr_dash_ease(curve, 3.0f, u - h)) / (2 * h);
        CHECK(std::fabs(numeric - vr_dash_ease_rate(curve, 3.0f, u)) < 2e-2f);
    }
    // Ease-out: the fastest at the start, at rest at the end.
    CHECK(vr_dash_ease_rate(VR_DASH_EASE_POWER, 3.0f, 0.0f) == 3.0f);
    CHECK(vr_dash_ease_rate(VR_DASH_EASE_POWER, 3.0f, 1.0f) == 0.0f);
    CHECK(vr_dash_bump(0.5f) > 0.999f && vr_dash_bump(0.0f) == 0.0f && vr_dash_bump(1.0f) == 0.0f);
}

/// The dash reaches the move's end on a straight line, at the start height, with an ease-out
/// profile and no overshoot, then sits exactly on the anchor.
void TestReach() {
    VrDashParams p;
    vr_dash_params_default(&p);
    p.max_lead = 0.0f; // the hint is right: no guard needed
    Roll roll;
    roll.from = V(1.0f, 1.65f, 2.0f);
    roll.dir = V(0.6f, 0.0f, -0.8f);
    const VrDashHint hint = Hint(roll.dir, roll.distance);
    VrDash d;
    vr_dash_reset(&d);
    const double t0 = 1.0;
    auto anchor = [&](double t) { return t < t0 ? roll.from : roll.At(Tick(t - t0, 30.0)); };
    auto move = [&](double t) { return t >= t0 && t < t0 + roll.length ? 1 : 0; };
    const auto s = Run(d, p, 0.0, 3.0, anchor, move, &hint);
    const VrDashV3 end = roll.End();
    float max_along = -1e9f, max_across = 0.0f, max_dy = 0.0f, raw_dy = 0.0f;
    bool monotonic = true;
    float prev = -1e9f;
    for (const Sample& x : s) {
        if (x.t < t0) {
            continue;
        }
        const float along = Along(x.out, roll.from, roll.dir);
        max_along = std::max(max_along, along);
        if (x.t < t0 + p.duration) {
            monotonic = monotonic && along >= prev - 1e-5f;
            prev = along;
            max_across = std::max(max_across, std::fabs(Across(x.out, roll.from, roll.dir)));
        }
        if (x.mode == VR_DASH_MOVE) {
            max_dy = std::max(max_dy, std::fabs(x.out.y - roll.from.y));
        }
        raw_dy = std::max(raw_dy, std::fabs(x.anchor.y - roll.from.y));
    }
    const float end_along = Along(end, roll.from, roll.dir);
    CHECK(max_along <= end_along + 1e-3f);      // no overshoot
    CHECK(monotonic);                           // always forward
    CHECK(max_across < 1e-3f);                  // straight (the anchor sways 8 cm)
    CHECK(max_dy < 1e-4f && raw_dy > 0.8f);     // no dip (the anchor dips 0.9 m)
    // At the end of the slide the camera is at the end of the move.
    for (const Sample& x : s) {
        if (x.t >= t0 + p.duration && x.t < t0 + p.duration + 1.5 / kHz) {
            CHECK(std::fabs(Along(x.out, roll.from, roll.dir) - end_along) < 0.02f);
        }
    }
    // Settled exactly onto the anchor, IDLE.
    CHECK(Dist(s.back().out, s.back().anchor) == 0.0f);
    CHECK(s.back().mode == VR_DASH_IDLE);
    // Ease-out: half of the distance is covered in the first fifth of the slide.
    for (const Sample& x : s) {
        if (x.t >= t0 + 0.2 * p.duration && x.t < t0 + 0.2 * p.duration + 1.0 / kHz) {
            CHECK(Along(x.out, roll.from, roll.dir) > 0.45f * end_along);
        }
    }
    std::printf("reach: end %.3f m, max along %.4f m, across %.5f m, camera dy %.5f m vs anchor %.2f m\n",
                end_along, max_along, max_across, max_dy, raw_dy);
}

/// The eye surges 0.4 m past the move's end and comes back (the head ahead of the root as the
/// character comes out of the roll): with the end known, the camera does not follow it out.
void TestSurgePastEnd() {
    VrDashParams p;
    vr_dash_params_default(&p);
    const VrDashV3 dir = V(0, 0, -1), from = V(0, 1.65f, 0);
    const float D = 3.0f, L = 0.8f;
    auto anchor = [&](double t) {
        const float x = std::clamp(float(Tick(t, 30.0)) / L, 0.0f, 1.0f);
        const float r = std::min(x / 0.7f, 1.0f); // the root arrives at 70%
        const float s = std::sin(kPi * x), late = x > 0.5f ? std::sin(kPi * (x - 0.5f) / 0.5f) : 0.0f;
        const float along = D * r * r * (3.0f - 2.0f * r) + 0.4f * late * late;
        return V(from.x, from.y - 0.8f * s * s, from.z - along);
    };
    auto move = [&](double t) { return t < L ? 1 : 0; };
    const VrDashHint hint = Hint(dir, D);
    VrDash d;
    vr_dash_reset(&d);
    const auto s = Run(d, p, 0.0, 1.6, anchor, move, &hint);
    float raw_max = 0.0f, cam_max = 0.0f;
    for (const Sample& x : s) {
        raw_max = std::max(raw_max, Along(x.anchor, from, dir));
        cam_max = std::max(cam_max, Along(x.out, from, dir));
    }
    CHECK(raw_max > D + 0.1f);
    CHECK(cam_max <= D + 1e-3f);
    CHECK(Dist(s.back().out, s.back().anchor) == 0.0f);
    std::printf("surge past the end: anchor %.3f m, camera %.4f m (end %.1f m)\n", raw_max, cam_max, D);
}

/// The anchor changes at 30 fps, the filter runs at 72 Hz: the camera still moves every frame,
/// with a smooth (monotonically falling) speed, while the anchor itself stalls and jumps.
void TestNoJitter() {
    for (int with_distance = 1; with_distance >= 0; --with_distance) {
        VrDashParams p;
        vr_dash_params_default(&p);
        p.max_lead = 0.0f;
        p.duration = 0.35f;
        Roll roll;
        roll.from = V(0, 1.65f, 0);
        roll.dir = V(1, 0, 0);
        const VrDashHint hint = Hint(roll.dir, with_distance ? roll.distance : 0.0f);
        VrDash d;
        vr_dash_reset(&d);
        auto anchor = [&](double t) { return roll.At(Tick(t, 30.0)); };
        auto move = [&](double t) { return t < roll.length ? 1 : 0; };
        const auto s = Run(d, p, 0.0, 1.5, anchor, move, &hint);
        int stalls = 0, raw_stalls = 0, reversals = 0;
        float max_rise = 0.0f, peak = 0.0f;
        bool started = false; // estimated: nothing to go on before the game's first moved sample
        std::vector<float> speed;
        for (size_t i = 1; i < s.size(); ++i) {
            const float v = Dist(s[i].out, s[i - 1].out) * float(kHz);
            const float raw = Dist(s[i].anchor, s[i - 1].anchor);
            started = started || v >= 1e-3f;
            if (s[i].t < p.duration * 0.9) {
                stalls += started && v < 1e-3f;
                raw_stalls += raw < 1e-6f;
            }
            speed.push_back(v);
            peak = std::max(peak, v);
        }
        // With the distance known the speed only falls (ease-out); estimated, it follows the
        // anchor's progress but never jumps back and forth with its 30 fps samples.
        const size_t dash_frames = size_t(p.duration * kHz);
        for (size_t i = 1; i < std::min(dash_frames, speed.size()); ++i) {
            const float rise = speed[i] - speed[i - 1];
            max_rise = std::max(max_rise, rise);
            if (i >= 2) {
                const float a = speed[i - 1] - speed[i - 2], b = speed[i] - speed[i - 1];
                reversals += a * b < 0.0f && std::fabs(a) > 0.05f * peak && std::fabs(b) > 0.05f * peak;
            }
        }
        CHECK(stalls == 0);
        CHECK(raw_stalls > 5); // the anchor does stall between its 30 fps samples
        if (with_distance) {
            CHECK(max_rise <= 0.02f * peak);
        }
        CHECK(reversals == 0);
        CHECK(Dist(s.back().out, s.back().anchor) < 1e-6f);
        std::printf("72 Hz / 30 fps (%s distance): peak %.1f m/s, stalls %d (anchor %d), largest speed rise %.2f m/s, "
                    "reversals %d, max step %.3f m\n",
                    with_distance ? "known" : "estimated", peak, stalls, raw_stalls, max_rise, reversals, MaxStep(s));
    }
}

/// A move cancelled at 40%: the anchor stops there; the camera (ahead of it) eases back without a
/// jump and ends exactly on it.
void TestCancel() {
    VrDashParams p;
    vr_dash_params_default(&p);
    p.max_lead = 2.0f;
    Roll roll;
    roll.from = V(0, 1.65f, 0);
    roll.dir = V(0, 0, -1);
    roll.stop_at = 0.4f;
    const VrDashHint hint = Hint(roll.dir, roll.distance);
    VrDash d;
    vr_dash_reset(&d);
    const double cancel = 0.4 * roll.length;
    auto anchor = [&](double t) { return roll.At(Tick(t, 30.0)); };
    auto move = [&](double t) { return t < cancel ? 7 : 0; };
    const auto s = Run(d, p, 0.0, 1.5, anchor, move, &hint);
    float jump = 0.0f, after = 0.0f, gap = -1.0f;
    double settled = -1.0;
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i].t >= cancel - 1e-9 && s[i].t < cancel + 2.0 / kHz) {
            // Across the cancel: no larger than the dash's own frame steps.
            jump = std::max(jump, Dist(s[i].out, s[i - 1].out));
            if (gap < 0.0f) {
                gap = Dist(s[i].out, s[i].anchor); // what the settle has to take back
            }
        }
        if (settled < 0.0 && s[i].mode == VR_DASH_IDLE && s[i].t > cancel) {
            settled = s[i].t;
        }
        if (settled >= 0.0) {
            after = std::max(after, Dist(s[i].out, s[i].anchor));
        }
    }
    const float frame_step = MaxStep(s);
    const double settle_time = std::max(p.settle, gap / p.settle_speed); // bigger takes longer
    CHECK(jump <= frame_step + 1e-6f);
    CHECK(jump < 0.5f);
    CHECK(settled > 0.0 && settled <= cancel + settle_time + 2.0 / kHz);
    CHECK(after == 0.0f);
    // Never further than max_lead ahead of the anchor.
    float lead = 0.0f;
    for (const Sample& x : s) {
        lead = std::max(lead, Along(x.out, roll.from, roll.dir) - Along(x.anchor, roll.from, roll.dir));
    }
    CHECK(lead <= p.max_lead + 0.35f); // the guard follows the anchor's progress, one 30 fps step behind
    std::printf("cancel at 40%%: step across the cancel %.3f m (max frame step %.3f), %.2f m to take back, settled "
                "after %.3f s, max lead %.2f m\n",
                jump, frame_step, gap, settled - cancel, lead);
}

/// Three moves back to back (forward, right, back), each starting before the previous one ended
/// or settled: continuous, every slide starts where the camera is, ends on the anchor.
void TestChain() {
    VrDashParams p;
    vr_dash_params_default(&p);
    p.max_lead = 0.0f;
    Roll rolls[3];
    const VrDashV3 dirs[3] = {V(0, 0, -1), V(1, 0, 0), V(0, 0, 1)};
    const double starts[3] = {0.5, 0.92, 1.34}; // 60% into the previous 0.7 s move
    VrDashV3 at = V(0, 1.65f, 0);
    for (int i = 0; i < 3; ++i) {
        rolls[i].dir = dirs[i];
        rolls[i].from = at;
        if (i < 2) {
            at = rolls[i].At(float(starts[i + 1] - starts[i]));
            at.y = 1.65f; // the next move takes over from where this one was (eye height reset)
        }
    }
    auto which = [&](double t) { return t < starts[0] ? -1 : t < starts[1] ? 0 : t < starts[2] ? 1 : 2; };
    auto anchor = [&](double t) {
        const double g = Tick(t, 30.0);
        const int i = which(g);
        return i < 0 ? rolls[0].from : rolls[i].At(float(g - starts[i]));
    };
    auto move = [&](double t) {
        const int i = which(t);
        return i < 0 || t - starts[i] >= rolls[i].length ? 0 : i + 1;
    };
    VrDashHint hints[3];
    for (int i = 0; i < 3; ++i) {
        hints[i] = Hint(dirs[i], rolls[i].distance);
    }
    VrDash d;
    vr_dash_reset(&d);
    std::vector<Sample> s;
    for (double t = 0.0; t < 3.0; t += 1.0 / kHz) {
        const int i = std::max(which(t), 0);
        const VrDashV3 a = anchor(t);
        const VrDashV3 o = vr_dash_update(&d, &p, a, move(t), &hints[i], t);
        s.push_back({t, a, o, d.mode, d.vignette});
    }
    CHECK(MaxStep(s) < 0.75f); // the fastest slide frame, no jump between slides
    float worst = 0.0f;
    for (size_t k = 1; k < s.size(); ++k) {
        for (double start : starts) {
            if (s[k].t >= start && s[k - 1].t < start) {
                worst = std::max(worst, Dist(s[k].out, s[k - 1].out));
            }
        }
    }
    CHECK(worst < 0.75f);
    CHECK(Dist(s.back().out, s.back().anchor) == 0.0f);
    CHECK(s.back().mode == VR_DASH_IDLE);
    std::printf("chain of 3: max step %.3f m, step at the starts %.3f m, end on the anchor\n", MaxStep(s), worst);
}

/// After dashes the camera is the anchor: walking around after 50 random moves (some with wrong
/// distances) never drifts.
void TestNoDrift() {
    VrDashParams p;
    vr_dash_params_default(&p);
    VrDash d;
    vr_dash_reset(&d);
    unsigned seed = 12345;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) / float(1u << 24);
    };
    double t = 0.0;
    VrDashV3 pos = V(0, 1.65f, 0);
    float worst_idle = 0.0f;
    int idle_frames = 0;
    for (int m = 0; m < 50; ++m) {
        Roll r;
        r.from = pos;
        const float a = rnd() * 2.0f * kPi;
        r.dir = V(std::sin(a), 0, std::cos(a));
        r.distance = 1.0f + 3.0f * rnd();
        r.length = 0.4f + 0.5f * rnd();
        r.stop_at = rnd() < 0.2f ? 0.3f + 0.6f * rnd() : 1.0f;
        VrDashHint h = Hint(r.dir, r.distance * (0.7f + 0.6f * rnd()), rnd() < 0.3f ? VR_DASH_JUMP : VR_DASH_DODGE);
        if (rnd() < 0.3f) {
            h.direction = V(0, 0, 0); // estimate the direction
        }
        const double t0 = t;
        const double fps = rnd() < 0.5f ? 30.0 : 60.0;
        for (; t < t0 + r.length + 0.6; t += 1.0 / kHz) {
            const double tau = Tick(t - t0, fps);
            const VrDashV3 an = r.At(float(tau));
            const VrDashV3 o = vr_dash_update(&d, &p, an, tau < r.length * r.stop_at ? m + 1 : 0, &h, t);
            if (d.mode == VR_DASH_IDLE) {
                worst_idle = std::max(worst_idle, Dist(o, an));
                ++idle_frames;
            }
        }
        pos = r.At(r.length);
        // Walk a bit between moves.
        for (int k = 0; k < 40; ++k, t += 1.0 / kHz) {
            pos.x += 0.03f * std::sin(0.1f * float(k + m));
            pos.z += 0.03f;
            const VrDashV3 o = vr_dash_update(&d, &p, pos, 0, nullptr, t);
            worst_idle = std::max(worst_idle, Dist(o, pos));
            ++idle_frames;
        }
    }
    CHECK(worst_idle == 0.0f);
    CHECK(idle_frames > 2000);
    std::printf("no drift: %d idle frames after 50 moves, max camera-anchor distance %.6f m\n", idle_frames,
                worst_idle);
}

/// A warp (lamp, respawn, cut) snaps in the same update, idle or mid-dash, and leaves no motion.
void TestTeleport() {
    VrDashParams p;
    vr_dash_params_default(&p);
    for (int mid_dash = 0; mid_dash < 2; ++mid_dash) {
        VrDash d;
        vr_dash_reset(&d);
        Roll roll;
        const VrDashHint hint = Hint(roll.dir, roll.distance);
        const VrDashV3 far = V(40.0f, 5.0f, -12.0f);
        const double warp = mid_dash ? 0.3 : 1.5;
        auto anchor = [&](double t) { return t < warp ? roll.At(float(t)) : far; };
        auto move = [&](double t) { return t < roll.length ? 1 : 0; };
        const auto s = Run(d, p, 0.0, 2.5, anchor, move, &hint);
        bool snapped = false;
        float after = 0.0f;
        for (const Sample& x : s) {
            if (x.t >= warp) {
                if (!snapped) {
                    snapped = Dist(x.out, far) == 0.0f;
                    CHECK(snapped); // the very first update after the warp
                }
                after = std::max(after, Dist(x.out, far));
            }
        }
        CHECK(after == 0.0f);
        CHECK(s.back().mode == VR_DASH_IDLE);
    }
    std::printf("teleport: snaps in the same update (idle and mid-dash), no motion after\n");
}

/// A jump: straight, one smooth bump of the configured height instead of the anchor's arc; flat
/// with height 0.
void TestJump() {
    for (float height : {0.3f, 0.0f}) {
        VrDashParams p;
        vr_dash_params_default(&p);
        p.jump_height = height;
        p.jump_duration = 0.6f;
        Roll jump;
        jump.from = V(0, 1.65f, 0);
        jump.dir = V(0, 0, -1);
        jump.distance = 2.5f;
        jump.length = 0.8f;
        jump.dip = 0.3f; // crouch before take-off
        jump.lift = 0.7f;
        const VrDashHint hint = Hint(jump.dir, jump.distance, VR_DASH_JUMP);
        VrDash d;
        vr_dash_reset(&d);
        auto anchor = [&](double t) { return jump.At(Tick(t, 30.0)); };
        auto move = [&](double t) { return t < jump.length ? 1 : 0; };
        const auto s = Run(d, p, 0.0, 1.6, anchor, move, &hint);
        float peak = 0.0f, low = 0.0f, across = 0.0f, worst_step_y = 0.0f;
        for (size_t i = 0; i < s.size(); ++i) {
            const float dy = s[i].out.y - jump.from.y;
            if (s[i].mode == VR_DASH_MOVE) {
                peak = std::max(peak, dy);
                low = std::min(low, dy);
                across = std::max(across, std::fabs(Across(s[i].out, jump.from, jump.dir)));
                if (i > 0) {
                    worst_step_y = std::max(worst_step_y, std::fabs(s[i].out.y - s[i - 1].out.y));
                }
            }
        }
        CHECK(std::fabs(peak - height) < 0.01f);
        CHECK(low > -1e-4f); // never below the start (the anchor crouches 0.3 m)
        CHECK(across < 1e-3f);
        CHECK(worst_step_y < 0.03f); // gentle: < 2.2 m/s vertical at 72 Hz
        CHECK(Dist(s.back().out, s.back().anchor) == 0.0f);
        std::printf("jump (height %.2f): camera peak %.3f m, lowest %.4f m, largest vertical step %.4f m\n", height,
                    peak, low, worst_step_y);
    }
}

/// Blocked: the hint says 3 m, the character stops after 1 m (a wall). The camera never runs
/// more than max_lead ahead of it and ends on it.
void TestBlocked() {
    VrDashParams p;
    vr_dash_params_default(&p);
    p.max_lead = 0.5f;
    Roll roll;
    roll.from = V(0, 1.65f, 0);
    roll.dir = V(0, 0, -1);
    roll.stop_at = 0.25f; // ~1.0 m of the 3 m
    const VrDashHint hint = Hint(roll.dir, roll.distance);
    VrDash d;
    vr_dash_reset(&d);
    auto anchor = [&](double t) { return roll.At(Tick(t, 30.0)); };
    auto move = [&](double t) { return t < roll.length ? 1 : 0; };
    const auto s = Run(d, p, 0.0, 2.0, anchor, move, &hint);
    float lead = 0.0f, furthest = 0.0f;
    for (const Sample& x : s) {
        lead = std::max(lead, Along(x.out, roll.from, roll.dir) - Along(x.anchor, roll.from, roll.dir));
        furthest = std::max(furthest, Along(x.out, roll.from, roll.dir));
    }
    const float stop = Along(roll.End(), roll.from, roll.dir);
    CHECK(furthest <= stop + p.max_lead + 0.1f); // + the guard's estimate overshooting a sudden stop
    CHECK(Dist(s.back().out, s.back().anchor) == 0.0f);
    std::printf("blocked after %.2f m of 3: camera furthest %.2f m (max_lead %.2f), lead over the anchor at most "
                "%.2f m\n",
                stop, furthest, p.max_lead, lead);
}

/// A roll off a ledge: the anchor drops 3 m; the dash stops holding the height and the camera
/// follows the fall.
void TestFall() {
    VrDashParams p;
    vr_dash_params_default(&p);
    Roll roll;
    roll.from = V(0, 1.65f, 0);
    const VrDashHint hint = Hint(roll.dir, roll.distance);
    VrDash d;
    vr_dash_reset(&d);
    auto anchor = [&](double t) {
        VrDashV3 a = roll.At(Tick(t, 30.0));
        if (t > 0.35) {
            a.y -= 9.0f * float((t - 0.35) * (t - 0.35)); // falling
        }
        return a;
    };
    auto move = [&](double t) { return t < roll.length ? 1 : 0; };
    const auto s = Run(d, p, 0.0, 1.5, anchor, move, &hint);
    // The anchor passes 2 m below the start (a fall: more than an eye can dip) near t = 0.82 s;
    // the camera then needs the settle (0.2 s) to join it.
    float gap = 0.0f;
    for (const Sample& x : s) {
        if (x.t > 0.85 + p.settle + 0.05) {
            gap = std::max(gap, std::fabs(x.out.y - x.anchor.y));
        }
    }
    CHECK(gap < 1e-4f);
    CHECK(MaxStep(s) < 0.6f);
    std::printf("fall: camera follows the drop (gap after 1.1 s %.5f m, max step %.3f m)\n", gap, MaxStep(s));
    // Bloodborne's roll takes the eye to the floor (1.65 -> 0.07 m): that is not a fall.
    Roll deep = roll;
    deep.dip = 1.58f;
    VrDash e;
    vr_dash_reset(&e);
    const auto r = Run(e, p, 0.0, 1.5, [&](double t) { return deep.At(Tick(t, 30.0)); },
                       [&](double t) { return t < deep.length ? 1 : 0; }, &hint);
    float held = 0.0f;
    for (const Sample& x : r) {
        if (x.mode == VR_DASH_MOVE) {
            held = std::max(held, std::fabs(x.out.y - deep.from.y));
        }
    }
    CHECK(held < 1e-4f);
    std::printf("roll to the floor (eye dips %.2f m): camera height kept within %.5f m\n", deep.dip, held);
}

/// The direction comes from the anchor when the hint has none: still straight along the real
/// motion, still no overshoot of the anchor's end.
void TestEstimated() {
    VrDashParams p;
    vr_dash_params_default(&p);
    Roll roll;
    roll.from = V(3, 1.65f, 3);
    roll.dir = V(-0.8f, 0, 0.6f);
    VrDashHint hint = Hint(V(0, 0, 0), 0.0f);
    VrDash d;
    vr_dash_reset(&d);
    auto anchor = [&](double t) { return roll.At(Tick(t, 30.0)); };
    auto move = [&](double t) { return t < roll.length ? 1 : 0; };
    const auto s = Run(d, p, 0.0, 1.5, anchor, move, &hint);
    const float end = Along(roll.End(), roll.from, roll.dir);
    float furthest = 0.0f, across = 0.0f;
    for (const Sample& x : s) {
        furthest = std::max(furthest, Along(x.out, roll.from, roll.dir));
        across = std::max(across, std::fabs(Across(x.out, roll.from, roll.dir)));
    }
    CHECK(furthest <= end + 0.02f);
    CHECK(across < 0.1f); // the anchor sways 8 cm sideways; the direction is refined as it travels
    CHECK(Dist(s.back().out, s.back().anchor) == 0.0f);
    std::printf("estimated direction and distance: furthest %.3f of %.3f m, off the line %.3f m\n", furthest, end,
                across);
}

/// Moving the world (a turn's pivot shift) mid-dash moves the camera by exactly that much.
void TestTranslateRotate() {
    VrDashParams p;
    vr_dash_params_default(&p);
    p.max_lead = 0.0f;
    Roll roll;
    roll.from = V(0, 1.65f, 0);
    const VrDashHint hint = Hint(roll.dir, roll.distance);
    VrDash a, b;
    vr_dash_reset(&a);
    vr_dash_reset(&b);
    const VrDashV3 shift = V(5.0f, 0.0f, -2.0f);
    float worst = 0.0f;
    for (double t = 0.0; t < 1.5; t += 1.0 / kHz) {
        const int m = t < roll.length ? 1 : 0;
        const VrDashV3 an = roll.At(Tick(t, 30.0));
        const VrDashV3 oa = vr_dash_update(&a, &p, an, m, &hint, t);
        if (std::fabs(t - 0.2) < 0.5 / kHz) {
            vr_dash_translate(&b, shift);
        }
        const bool shifted = t >= 0.2 - 0.5 / kHz;
        const VrDashV3 anb = shifted ? vr_dash_add(an, shift) : an;
        VrDashHint hb = hint;
        const VrDashV3 ob = vr_dash_update(&b, &p, anb, m, &hb, t);
        worst = std::max(worst, Dist(shifted ? vr_dash_sub(ob, shift) : ob, oa));
    }
    CHECK(worst < 1e-4f);
    // Rotation: a quarter turn about a pivot maps the outputs the same way.
    VrDash r;
    vr_dash_reset(&r);
    VrDash q;
    vr_dash_reset(&q);
    const VrDashV3 pivot = V(1.0f, 0.0f, 1.0f);
    float worst_rot = 0.0f;
    for (double t = 0.0; t < 1.5; t += 1.0 / kHz) {
        const int m = t < roll.length ? 1 : 0;
        const VrDashV3 an = roll.At(Tick(t, 30.0));
        const VrDashV3 oq = vr_dash_update(&q, &p, an, m, &hint, t);
        const bool turned = t >= 0.2 - 0.5 / kHz;
        if (std::fabs(t - 0.2) < 0.5 / kHz) {
            vr_dash_rotate(&r, pivot, 0.0f, 1.0f);
        }
        const VrDashV3 anr = turned ? vr_dash_rotate_y(an, pivot, 0.0f, 1.0f, 1) : an;
        VrDashHint hr = hint;
        hr.direction = vr_dash_rotate_y(hint.direction, pivot, 0.0f, 1.0f, 0);
        const VrDashV3 orr = vr_dash_update(&r, &p, anr, m, turned ? &hr : &hint, t);
        const VrDashV3 expect = turned ? vr_dash_rotate_y(oq, pivot, 0.0f, 1.0f, 1) : oq;
        worst_rot = std::max(worst_rot, Dist(orr, expect));
    }
    CHECK(worst_rot < 1e-3f);
    std::printf("translate/rotate mid-dash: off by %.6f / %.6f m\n", worst, worst_rot);
}

/// The game's settings (a 0.4 s quad slide, max_lead 2 m) on Bloodborne's roll as measured
/// (a000_007200: 4 m, the root 20% along after 0.1 s, 39% after 0.2 s, 59% after 0.3 s, 83%
/// after 0.5 s): the lead guard must not touch a roll that is not blocked.
void TestGuardLeavesRollsAlone() {
    VrDashParams game;
    vr_dash_params_default(&game);
    game.duration = 0.4f;
    game.power = 2.0f;
    game.max_lead = 2.0f;
    VrDashParams free = game;
    free.max_lead = 0.0f;
    const float share_t[] = {0.0f, 0.1f, 0.2f, 0.3f, 0.5f, 0.7f, 0.93f};
    const float share[] = {0.0f, 0.20f, 0.39f, 0.59f, 0.83f, 0.95f, 1.0f};
    auto root = [&](double t) { // the measured profile, linear between the points, 30 fps
        const float g = float(Tick(t, 30.0));
        for (int i = 1; i < 7; ++i) {
            if (g <= share_t[i]) {
                const float k = (g - share_t[i - 1]) / (share_t[i] - share_t[i - 1]);
                return share[i - 1] + (share[i] - share[i - 1]) * k;
            }
        }
        return 1.0f;
    };
    auto anchor = [&](double t) { return V(0.0f, 1.6f, -4.0f * root(t)); };
    auto move = [&](double t) { return t < 1.67 ? 1 : 0; };
    const VrDashHint hint = Hint(V(0, 0, -1), 4.0f);
    VrDash a, b;
    vr_dash_reset(&a);
    vr_dash_reset(&b);
    const auto s = Run(a, game, 0.0, 2.2, anchor, move, &hint);
    const auto f = Run(b, free, 0.0, 2.2, anchor, move, &hint);
    float diff = 0.0f, lead = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        diff = std::max(diff, Dist(s[i].out, f[i].out));
        lead = std::max(lead, s[i].anchor.z - s[i].out.z);
    }
    CHECK(diff < 0.01f);
    std::printf("game settings on the measured roll: the guard changes the path by %.3f m (lead up to %.2f m of %.1f)\n",
                diff, lead, game.max_lead);
}

/// The hint's direction was 30 degrees off (the character still turning into the roll); two
/// frames later the game knows better and re-aims: the slide bends once, continuously, and still
/// ends exactly where the move does.
void TestRetarget() {
    VrDashParams p;
    vr_dash_params_default(&p);
    p.max_lead = 0.0f;
    Roll roll;
    roll.from = V(0, 1.65f, 0);
    roll.dir = V(0, 0, -1);
    const VrDashHint wrong = Hint(V(std::sin(0.52f), 0, -std::cos(0.52f)), roll.distance);
    VrDash d;
    vr_dash_reset(&d);
    std::vector<Sample> s;
    int frame = 0;
    for (double t = 0.0; t < 1.6; t += 1.0 / kHz, ++frame) {
        const VrDashV3 a = roll.At(Tick(t, 30.0));
        const VrDashV3 o = vr_dash_update(&d, &p, a, t < roll.length ? 1 : 0, &wrong, t);
        if (frame == 2) {
            vr_dash_retarget(&d, roll.dir, roll.distance, t);
        }
        s.push_back({t, a, o, d.mode, d.vignette});
    }
    float across_end = 0.0f;
    for (const Sample& x : s) {
        if (x.t > 0.5 && x.t < roll.length) {
            across_end = std::max(across_end, std::fabs(Across(x.out, roll.from, roll.dir)));
        }
    }
    CHECK(MaxStep(s) < 0.45f);
    CHECK(across_end < 0.05f); // on the right line once re-aimed (it bent at frame 2)
    CHECK(Along(s.back().out, roll.from, roll.dir) <= roll.distance + 1e-3f);
    CHECK(Dist(s.back().out, s.back().anchor) == 0.0f);
    std::printf("retarget after 2 frames from 30 deg off: max step %.3f m, off the line later %.3f m\n", MaxStep(s),
                across_end);
}

/// FOLLOW mode (the game): a hint-less move whose character is blocked by a wall after 1 m of a
/// 4 m table, then a slope. The camera is never ahead of the anchor, stops with it, and its
/// height is the anchor's (plus nothing: a flat dash).
void TestFollow() {
    VrDashParams p;
    vr_dash_params_default(&p);
    p.follow = 1;
    CHECK(p.max_lead == 0.0f);
    {   // wall: the feet run 1 m in 0.25 s, then stand still until 1.5 s
        VrDash d;
        vr_dash_reset(&d);
        auto anchor = [&](double t) {
            const float x = std::min(float(Tick(t, 30.0)) / 0.25f, 1.0f);
            return V(0, 1.65f, -(1.0f - (1.0f - x) * (1.0f - x)));
        };
        auto move = [&](double t) { return t < 1.0 ? 1 : 0; };
        const VrDashHint hint = Hint(V(0, 0, -1), 4.0f);
        const auto s = Run(d, p, 0.0, 2.0, anchor, move, &hint);
        float ahead = -1e9f, past_wall = 0.0f, ydev = 0.0f;
        for (const Sample& x : s) {
            ahead = std::max(ahead, Along(x.out, V(0, 1.65f, 0), V(0, 0, -1)) - Along(x.anchor, V(0, 1.65f, 0), V(0, 0, -1)));
            past_wall = std::max(past_wall, Along(x.out, V(0, 1.65f, 0), V(0, 0, -1)) - 1.0f);
            ydev = std::max(ydev, std::fabs(x.out.y - x.anchor.y));
        }
        CHECK(ahead <= 1e-4f);
        CHECK(past_wall <= 1e-3f);
        CHECK(ydev < 1e-5f);
        CHECK(Dist(s.back().out, s.back().anchor) < 1e-3f);
        CHECK(MaxStep(s) < 0.45f);
        std::printf("follow, wall at 1 m of 4: camera ahead of the anchor at most %.4f m, past the wall %.4f m\n",
                    ahead, past_wall);
    }
    {   // slope: the feet climb 2 m while running 4 m; the camera height is theirs, never below
        VrDash d;
        vr_dash_reset(&d);
        auto anchor = [&](double t) {
            const float x = std::min(float(Tick(t, 30.0)) / 0.7f, 1.0f);
            return V(0, 1.65f + 0.5f * 4.0f * x, -4.0f * x);
        };
        auto move = [&](double t) { return t < 0.7 ? 1 : 0; };
        const auto s = Run(d, p, 0.0, 1.6, anchor, move, nullptr);
        float below = 0.0f, ahead = -1e9f;
        for (const Sample& x : s) {
            below = std::max(below, x.anchor.y - x.out.y);
            ahead = std::max(ahead, Along(x.out, V(0, 1.65f, 0), V(0, 0, -1)) - Along(x.anchor, V(0, 1.65f, 0), V(0, 0, -1)));
        }
        CHECK(below < 1e-4f);
        CHECK(ahead <= 1e-4f);
        CHECK(Dist(s.back().out, s.back().anchor) < 1e-3f);
        std::printf("follow, slope: camera below the anchor at most %.5f m, ahead %.4f m\n", below, ahead);
    }
    {   // a jump's bump stays above the feet
        VrDash d;
        vr_dash_reset(&d);
        VrDashHint hint = Hint(V(0, 0, -1), 3.0f);
        hint.kind = VR_DASH_JUMP;
        hint.height = -1.0f;
        auto anchor = [&](double t) { return V(0, 1.65f, -3.0f * std::min(float(Tick(t, 30.0)) / 0.9f, 1.0f)); };
        auto move = [&](double t) { return t < 0.9 ? 1 : 0; };
        const auto s = Run(d, p, 0.0, 2.0, anchor, move, &hint);
        float low = 1e9f, peak = -1e9f;
        for (const Sample& x : s) {
            low = std::min(low, x.out.y - x.anchor.y);
            peak = std::max(peak, x.out.y - x.anchor.y);
        }
        CHECK(low > -1e-4f);
        CHECK(peak > 0.1f);
        std::printf("follow, jump: bump above the feet %.2f m, never below (%.5f)\n", peak, low);
    }
}

/// The vignette rises with the dash and is gone shortly after it.
void TestVignette() {
    VrDashParams p;
    vr_dash_params_default(&p);
    Roll roll;
    const VrDashHint hint = Hint(roll.dir, roll.distance);
    VrDash d;
    vr_dash_reset(&d);
    auto anchor = [&](double t) { return t < 0.5 ? roll.from : roll.At(Tick(t - 0.5, 30.0)); };
    auto move = [&](double t) { return t >= 0.5 && t < 0.5 + roll.length ? 1 : 0; };
    const auto s = Run(d, p, 0.0, 2.5, anchor, move, &hint);
    float before = 0.0f, during = 0.0f, late = 0.0f;
    for (const Sample& x : s) {
        if (x.t < 0.5) before = std::max(before, x.vignette);
        else if (x.t < 0.5 + p.duration * 0.5) during = std::max(during, x.vignette);
        else if (x.t > 0.5 + roll.length + p.settle + 1.0) late = std::max(late, x.vignette);
    }
    CHECK(before == 0.0f);
    CHECK(during > 0.99f);
    CHECK(late < 0.01f);
    std::printf("vignette: before %.2f, during %.2f, 1 s after %.3f\n", before, during, late);
}

} // namespace

int main() {
    TestEase();
    TestReach();
    TestSurgePastEnd();
    TestNoJitter();
    TestCancel();
    TestChain();
    TestNoDrift();
    TestTeleport();
    TestJump();
    TestBlocked();
    TestFall();
    TestEstimated();
    TestTranslateRotate();
    TestGuardLeavesRollsAlone();
    TestRetarget();
    TestVignette();
    TestFollow();
    std::printf("dash tests: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
