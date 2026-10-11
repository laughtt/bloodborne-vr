// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "parry_view.h"

#include <algorithm>
#include <cmath>

#include "consumables.h"
#include "wire_shapes.h"

namespace sb {

using namespace m3;

namespace {

struct Rgb {
    uint8_t r, g, b;
};

void Line(std::vector<LineVertex>& out, Vec3 a, Vec3 b, Rgb c) {
    out.push_back({{a.x, a.y, a.z}, {c.r, c.g, c.b, 255}});
    out.push_back({{b.x, b.y, b.z}, {c.r, c.g, c.b, 255}});
}

Rgb PhaseColor(ParryTrainer::Phase p) {
    switch (p) {
    case ParryTrainer::kWindUp: return {255, 170, 40};
    case ParryTrainer::kWindow: return {80, 255, 120};
    case ParryTrainer::kSwing: return {255, 50, 40};
    case ParryTrainer::kRecover: return {170, 80, 70};
    case ParryTrainer::kStagger: return {70, 150, 255};
    case ParryTrainer::kDown: return {60, 80, 130};
    default: return {150, 150, 160};
    }
}

void Ring(std::vector<LineVertex>& out, Vec3 centre, Vec3 right, Vec3 up, float r, Rgb c) {
    const int n = 32;
    for (int i = 0; i < n; ++i) {
        const float a0 = 2.0f * kPi * float(i) / n, a1 = 2.0f * kPi * float(i + 1) / n;
        Line(out, centre + (right * std::cos(a0) + up * std::sin(a0)) * r,
             centre + (right * std::cos(a1) + up * std::sin(a1)) * r, c);
    }
}

} // namespace

void DrawParry(const ParryTrainer& t, const ParryConfig& config, double now, Vec3 head,
               FrameData& out) {
    if (!config.enabled) {
        return;
    }
    // Bullets (a tracer behind each) and the muzzle flash.
    for (const ParryTrainer::Bullet& b : t.Bullets()) {
        if (b.alive) {
            Line(out.overlay, b.pos - b.dir * 0.7f, b.pos, {255, 235, 120});
            Line(out.overlay, b.pos - b.dir * 0.25f, b.pos, {255, 255, 255});
        }
    }
    const float since = float(now - t.LastShotTime());
    if (since >= 0.0f && since < 0.09f) {
        const Vec3 m = t.LastMuzzle(), d = t.LastDir();
        const Vec3 u = AnyPerp(d), v = Cross(d, u);
        const float k = 1.0f - since / 0.09f;
        for (int i = 0; i < 8; ++i) {
            const float a = float(i) * kPi / 4.0f;
            const Vec3 side = u * std::cos(a) + v * std::sin(a);
            Line(out.overlay, m, m + d * (0.10f * k) + side * (0.06f * k), {255, 200, 60});
        }
        Line(out.overlay, m, m + d * (0.22f * k), {255, 255, 220});
    }
    if (!t.HaveDummy() || !config.dummy) {
        return;
    }
    const ParryTrainer::Phase phase = t.GetPhase();
    const Rgb color = PhaseColor(phase);
    // Hurtboxes tinted by phase (depth tested, so it reads as the body).
    for (const col::Capsule& c : t.Parts()) {
        WireCapsule([&](Vec3 a, Vec3 b) { Line(out.lines, a, b, color); }, c.a, c.b, c.r);
    }
    const Vec3 base = t.Base(), f = t.Facing();
    const Vec3 right = Normalize(Cross(f, {0, 1, 0}), {1, 0, 0});
    const Vec3 up{0, 1, 0};
    const float h = t.Height();
    // The weapon: pivot at the right shoulder, angle a from hanging down-forward through up.
    float a = 160.0f;
    const double len = std::max(t.PhaseEnd() - t.PhaseStart(), 1e-6);
    const float p = float(std::clamp((now - t.PhaseStart()) / len, 0.0, 1.0));
    switch (phase) {
    case ParryTrainer::kWindUp: a = 160.0f + 180.0f * (1.0f - (1.0f - p) * (1.0f - p)); break;
    case ParryTrainer::kWindow: a = 340.0f; break;
    case ParryTrainer::kSwing: a = 340.0f + 180.0f * p * p; break;
    case ParryTrainer::kRecover: a = 160.0f; break;
    case ParryTrainer::kStagger:
    case ParryTrainer::kDown: a = 200.0f; break;
    default: break;
    }
    const Vec3 pivot = base + up * (0.72f * h) + right * 0.28f + f * 0.1f;
    const float ar = a * kDeg;
    const Vec3 dir = up * std::cos(ar) + f * std::sin(ar);
    const Vec3 tip = pivot + dir * (0.45f * h);
    for (const Vec3 off : {Vec3{0, 0, 0}, right * 0.015f, right * -0.015f, up * 0.015f}) {
        Line(out.overlay, pivot + off, tip + off, color);
    }
    // The telegraph ring over the head: closes onto the window; green and solid in it.
    const Vec3 crown = base + up * (h + 0.35f);
    const Vec3 hv = Normalize(Vec3{head.x - crown.x, 0, head.z - crown.z}, f);
    const Vec3 rr = Normalize(Cross(up, hv), right);
    if (phase == ParryTrainer::kWindUp) {
        const float k = t.WindUpFraction(now);
        Ring(out.overlay, crown, rr, up, 0.5f * (1.0f - k) + 0.12f * k, color);
        Ring(out.overlay, crown, rr, up, 0.12f, {120, 120, 120});
    } else if (phase == ParryTrainer::kWindow) {
        for (float r = 0.04f; r <= 0.16f; r += 0.04f) {
            Ring(out.overlay, crown, rr, up, r, color);
        }
        Line(out.overlay, crown - rr * 0.2f, crown + rr * 0.2f, color);
        Line(out.overlay, crown - up * 0.2f, crown + up * 0.2f, color);
    } else if (phase == ParryTrainer::kSwing) {
        Line(out.overlay, crown - rr * 0.15f - up * 0.15f, crown + rr * 0.15f + up * 0.15f, color);
        Line(out.overlay, crown - rr * 0.15f + up * 0.15f, crown + rr * 0.15f - up * 0.15f, color);
    } else if (phase == ParryTrainer::kStagger) {
        const float left = float(std::clamp((t.PhaseEnd() - now) / len, 0.0, 1.0));
        Line(out.overlay, crown - rr * 0.25f, crown - rr * 0.25f + rr * (0.5f * left), color);
    }
    // The score: coloured tick + digits stacked beside the dummy, facing the head.
    const ParryScore& s = t.Score();
    const struct {
        int value;
        Rgb c;
    } rows[] = {{s.parries, {80, 255, 120}}, {s.early, {255, 220, 60}}, {s.late, {255, 130, 40}},
                {s.misses, {150, 150, 160}}, {s.visceral, {70, 150, 255}}};
    const Vec3 origin = base + up * (h + 0.1f) + rr * 0.55f;
    for (int i = 0; i < 5; ++i) {
        const Vec3 o = origin - up * (0.11f * float(i));
        Line(out.overlay, o - rr * 0.07f + up * 0.04f, o - rr * 0.02f + up * 0.04f, rows[i].c);
        Line(out.overlay, o - rr * 0.07f + up * 0.05f, o - rr * 0.02f + up * 0.05f, rows[i].c);
        AppendGlyphs(std::to_string(rows[i].value), o, rr, up, 0.08f,
                     [&](Vec3 p0, Vec3 p1) { Line(out.overlay, p0, p1, rows[i].c); });
    }
}

} // namespace sb
