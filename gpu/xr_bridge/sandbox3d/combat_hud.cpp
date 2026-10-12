// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "combat_hud.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace sb {

using namespace m3;
using Label = SwingReadout::Label;
using Result = SwingReadout::Result;

namespace {

std::string Fmt(const char* format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

std::string Upper(std::string s) {
    for (char& c : s) {
        c = char(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

constexpr UiColor kLight{0.40f, 0.82f, 1.00f, 1.0f};
constexpr UiColor kHeavy{1.00f, 0.62f, 0.20f, 1.0f};
constexpr UiColor kCharged{1.00f, 0.32f, 0.55f, 1.0f};
constexpr UiColor kWindup{0.90f, 0.84f, 0.50f, 1.0f};
constexpr UiColor kDim{0.62f, 0.62f, 0.64f, 1.0f};
constexpr UiColor kTired{1.00f, 0.28f, 0.22f, 1.0f};
constexpr UiColor kDash{0.62f, 0.78f, 0.95f, 1.0f};
constexpr UiColor kHit{0.55f, 1.00f, 0.55f, 1.0f};
constexpr UiColor kClang{0.95f, 0.90f, 0.55f, 1.0f};
constexpr UiColor kWhite{0.95f, 0.95f, 0.95f, 1.0f};

/// The label's words: the attack or what happened, its result, a detail line.
struct LabelText {
    std::string main, right, detail;
    UiColor color = kWhite, right_color = kWhite;
};

LabelText TextOf(const SwingReadout& r) {
    LabelText t;
    switch (r.label) {
    case Label::Light:
    case Label::Heavy:
    case Label::Charged: {
        t.main = r.label == Label::Light ? "LIGHT" : r.label == Label::Heavy ? "HEAVY" : "CHARGED";
        t.color = r.label == Label::Light ? kLight : r.label == Label::Heavy ? kHeavy : kCharged;
        switch (r.result) {
        case Result::Pending:
            t.right = r.label_cost > 0.0f ? Fmt("-%g if it hits", r.label_cost)
                                          : std::string("paid only if it hits");
            t.right_color = kDim;
            break;
        case Result::Hit:
            t.right = Fmt("HIT  -%g", r.label_paid);
            t.right_color = kHit;
            break;
        case Result::Clang:
            t.right = Fmt("CLANG  -%g", r.label_paid);
            t.right_color = kClang;
            break;
        case Result::Miss:
            t.right = "MISS  free";
            t.right_color = kDim;
            break;
        default:
            break;
        }
        t.detail = r.latency_ms >= 0.0f
                       ? Fmt("%s   peak %.1f m/s   %.0f ms", r.label == Label::Light ? "R1" : "R2",
                             r.label_speed, r.latency_ms)
                       : Fmt("%s   peak %.1f m/s", r.label == Label::Light ? "R1" : "R2",
                             r.label_speed);
        break;
    }
    case Label::Charging: {
        t.main = "CHARGING";
        t.color = kCharged;
        const double held = r.now - r.charge_since;
        t.right = r.charging ? (held >= r.charge_full_s ? std::string("FULL")
                                                        : Fmt("%.1f / %.1f s", held, r.charge_full_s))
                             : std::string();
        t.right_color = held >= r.charge_full_s ? kCharged : kDim;
        t.detail = "R2 held: swing out of it to strike";
        break;
    }
    case Label::Windup:
        t.main = "WINDUP";
        t.color = kWindup;
        t.right = "not an attack";
        t.right_color = kDim;
        t.detail = Fmt("rising stroke %.1f m/s: hold it still overhead to charge", r.label_speed);
        break;
    case Label::TooSlow:
        t.main = "TOO SLOW";
        t.color = kDim;
        t.right = Fmt("%.1f m/s", r.label_speed);
        t.right_color = kDim;
        t.detail = Fmt("light from %.1f, heavy from %.1f m/s", r.light, r.heavy);
        break;
    case Label::Tired:
        t.main = "NO STAMINA";
        t.color = kTired;
        t.right = "swing ignored";
        t.right_color = kDim;
        t.detail = Fmt("SP %.0f: nothing starts at 0 or below", r.stamina);
        break;
    case Label::Blocked:
        t.main = "BLOCKED";
        t.color = kDim;
        t.right = "menu open";
        t.right_color = kDim;
        break;
    case Label::Dash:
        t.main = Upper(r.move);
        t.color = kDash;
        t.right = Fmt("-%g", r.label_cost);
        t.right_color = kDash;
        break;
    case Label::DashTired:
        t.main = Upper(r.move);
        t.color = kTired;
        t.right = "NO STAMINA";
        t.right_color = kTired;
        t.detail = Fmt("SP %.0f: nothing starts at 0 or below", r.stamina);
        break;
    default:
        break;
    }
    if (!r.detail.empty()) {
        t.detail = r.detail;
    }
    return t;
}

/// How visible the label is now (fades out label_s after it was set or got its result).
float LabelAlpha(const SwingReadout& r, double label_s) {
    if (r.label == Label::None) {
        return 0.0f;
    }
    if (r.result == Result::Pending || (r.label == Label::Charging && r.charging)) {
        return 1.0f;
    }
    const double age = r.now - r.label_t;
    return float(std::clamp((label_s - age) / 0.4, 0.0, 1.0));
}

/// The stamina bar: the game's whole range, floor (-50) to max, centered at (cx, cy).
void Bar(UiCanvas& cv, const UiPanel& p, float cx, float cy, float w, float h,
         const SwingReadout& r) {
    const double now = r.now;
    const float lo = std::min(r.floor, 0.0f), hi = std::max(r.max, 1.0f);
    const auto u = [&](float v) { return cx - 0.5f * w + w * (std::clamp(v, lo, hi) - lo) / (hi - lo); };
    const auto seg = [&](float a, float b, float hh, UiColor c) {
        const float u0 = u(std::min(a, b)), u1 = u(std::max(a, b));
        if (u1 - u0 > 1e-5f) {
            cv.Box(p, 0.5f * (u0 + u1), cy, u1 - u0, hh, c);
        }
    };
    const double refused = now - r.refused_at;
    const bool blink = refused >= 0.0 && refused < 0.6 && std::fmod(refused, 0.15) < 0.075;
    const float edge = 0.12f * h;
    cv.Box(p, cx, cy, w + 2.0f * edge, h + 2.0f * edge,
           blink ? UiColor{1.0f, 0.15f, 0.1f, 0.95f} : UiColor{0.0f, 0.0f, 0.0f, 0.75f}, edge);
    seg(lo, 0.0f, h, {0.28f, 0.05f, 0.05f, 0.92f});
    seg(0.0f, hi, h, {0.09f, 0.09f, 0.10f, 0.92f});
    if (r.stamina >= 0.0f) {
        seg(0.0f, r.stamina, h, r.regen_paused ? bars::Paused(r.stamina_color) : r.stamina_color);
    } else {
        seg(r.stamina, 0.0f, h, {0.95f, 0.22f, 0.16f, 1.0f});
    }
    if (r.pending > 0.0f) { // what the open swings take if they connect
        seg(r.stamina - r.pending, r.stamina, 0.5f * h, {1.0f, 1.0f, 1.0f, 0.6f});
    }
    const double since = now - r.charged_at;
    if (since >= 0.0 && since < 0.5) { // where the last cost was taken
        seg(r.charged_to, r.charged_from, h, {1.0f, 0.55f, 0.1f, float(1.0 - since / 0.5)});
    }
    cv.Box(p, u(0.0f), cy, std::max(0.006f * w, 0.0006f), 1.7f * h, {1.0f, 1.0f, 1.0f, 0.9f});
}

/// The HP bar: 0 to max, centered at (cx, cy), the same frame and size as the stamina bar.
void HpBar(UiCanvas& cv, const UiPanel& p, float cx, float cy, float w, float h,
           const SwingReadout& r) {
    const float edge = 0.12f * h;
    cv.Box(p, cx, cy, w + 2.0f * edge, h + 2.0f * edge, {0.0f, 0.0f, 0.0f, 0.75f}, edge);
    cv.Box(p, cx, cy, w, h, {0.09f, 0.09f, 0.10f, 0.92f});
    const float f = bars::BarFraction(r.hp, r.hp_max);
    if (f > 0.0f) {
        cv.Box(p, cx - 0.5f * w + 0.5f * w * f, cy, w * f, h, r.hp_color);
    }
}

/// The live probe speed on a scale with the classifier's thresholds.
void Speed(UiCanvas& cv, const UiPanel& p, float cx, float cy, float w, float h,
           const SwingReadout& r, float speed_max, float text) {
    const auto u = [&](float v) { return cx - 0.5f * w + w * std::clamp(v / speed_max, 0.0f, 1.0f); };
    const auto seg = [&](float a, float b, UiColor c) {
        const float u0 = u(a), u1 = u(b);
        if (u1 - u0 > 1e-5f) {
            cv.Box(p, 0.5f * (u0 + u1), cy, u1 - u0, h, c);
        }
    };
    seg(0.0f, r.onset, {0.10f, 0.10f, 0.11f, 0.8f});
    seg(r.onset, r.light, {0.22f, 0.26f, 0.30f, 0.8f});
    seg(r.light, r.heavy, {0.16f, 0.42f, 0.56f, 0.85f});
    seg(r.heavy, speed_max, {0.58f, 0.34f, 0.12f, 0.85f});
    if (r.stroke_peak > 0.0f) {
        cv.Box(p, u(r.stroke_peak), cy, std::max(0.006f * w, 0.0006f), 2.4f * h,
               {1.0f, 0.88f, 0.3f, 0.95f});
    }
    cv.Box(p, u(r.speed), cy, std::max(0.009f * w, 0.0008f), 2.8f * h, {1.0f, 1.0f, 1.0f, 0.95f});
    const char* phase = r.phase == swing::SwingClassifier::Phase::Swing      ? "swing"
                        : r.phase == swing::SwingClassifier::Phase::Rearm    ? "rearm"
                        : r.phase == swing::SwingClassifier::Phase::Charging ? "charging"
                                                                             : "ready";
    cv.Text(p, cx + 0.5f * w + 0.6f * text, cy - 0.35f * text, text,
            Fmt("%.1f m/s  %s", r.speed, phase), kDim);
    cv.Text(p, u(r.onset), cy - 1.6f * h - text, 0.8f * text, Fmt("%.1f", r.onset), kDim,
            UiAlign::Center);
    cv.Text(p, u(r.light), cy - 1.6f * h - text, 0.8f * text, Fmt("%.1f", r.light), kLight,
            UiAlign::Center);
    cv.Text(p, u(r.heavy), cy - 1.6f * h - text, 0.8f * text, Fmt("%.1f", r.heavy), kHeavy,
            UiAlign::Center);
}

/// The label line: the main word in its color, the result after it, centered on x.
void LabelLine(UiCanvas& cv, const UiPanel& p, float x, float y, float size, const LabelText& t) {
    const float gap = 0.5f * size;
    const float right_size = 0.8f * size;
    const float wm = cv.TextWidth(t.main, size);
    const float wr = t.right.empty() ? 0.0f : cv.TextWidth(t.right, right_size);
    const float left = x - 0.5f * (wm + (wr > 0.0f ? gap + wr : 0.0f));
    cv.Text(p, left, y, size, t.main, t.color);
    if (wr > 0.0f) {
        cv.Text(p, left + wm + gap, y, right_size, t.right, t.right_color);
    }
}

} // namespace

void DrawSwingHud(UiCanvas& cv, const SwingReadout& r, const SwingHudConfig& c,
                  const UiPose& head, const UiPose& left_grip) {
    if (!r.enabled || !head.valid) {
        return;
    }
    cv.on_top = true;
    cv.outline = 0.12f;
    const LabelText text = TextOf(r);
    const float label_alpha = LabelAlpha(r, c.label_s);
    const Vec3 up = Rotate(head.rotation, {0, 1, 0});
    if (c.head) {
        // Head-locked below the view: the label, the bar, the speed scale. Sizes keep their angle
        // whatever the distance.
        const float d = c.distance;
        const float k = c.scale * d / 0.9f;
        const float down = c.down_deg * kDeg;
        const Vec3 origin =
            head.position + Rotate(head.rotation, {0.0f, -std::sin(down) * d, -std::cos(down) * d});
        const UiPanel p = UiPanel::Facing(origin, head.position, up);
        cv.alpha = 1.0f;
        Bar(cv, p, 0.0f, 0.0f, 0.26f * k, 0.017f * k, r);
        cv.Text(p, 0.142f * k, -0.0055f * k, 0.016f * k, Fmt("SP %.0f / %.0f", r.stamina, r.max),
                r.can_act ? kWhite : kTired);
        if (r.two_forms) { // the trick weapon's form; large for a moment after a transform
            const double age = r.now - r.form_t;
            const bool fresh = age >= 0.0 && age < c.label_s;
            cv.Text(p, -0.142f * k, -0.0055f * k, (fresh ? 0.024f : 0.016f) * k,
                    r.form == "B" ? "FORM B" : "FORM A", r.form == "B" ? kHeavy : kLight,
                    UiAlign::Right);
        }
        if (c.speed) {
            Speed(cv, p, 0.0f, -0.032f * k, 0.26f * k, 0.005f * k, r, c.speed_max, 0.0115f * k);
        }
        if (label_alpha > 0.0f) {
            cv.alpha = label_alpha;
            LabelLine(cv, p, 0.0f, 0.026f * k, 0.03f * k, text);
            cv.Text(p, 0.0f, 0.0115f * k, 0.012f * k, text.detail, kDim, UiAlign::Center);
            if (r.label == Label::Charging && r.charging) { // the charge filling up
                const float f = float(std::clamp((r.now - r.charge_since) / std::max(r.charge_full_s, 0.01), 0.0, 1.0));
                cv.Box(p, 0.0f, 0.058f * k, 0.14f * k, 0.005f * k, {0.1f, 0.1f, 0.12f, 0.8f});
                cv.Box(p, -0.07f * k + 0.07f * k * f, 0.058f * k, 0.14f * k * f, 0.005f * k, kCharged);
            }
            cv.alpha = 1.0f;
        }
    }
    if (c.wrist && left_grip.valid) {
        // Above the left wrist, facing the eyes.
        const float s = c.wrist_scale;
        const Vec3 origin = left_grip.position + c.wrist_offset;
        const UiPanel p = UiPanel::Facing(origin, head.position, {0, 1, 0});
        cv.alpha = 1.0f;
        cv.Text(p, 0.0f, 0.012f * s, 0.016f * s, Fmt("SP %.0f / %.0f", r.stamina, r.max),
                r.can_act ? kWhite : kTired, UiAlign::Center);
        Bar(cv, p, 0.0f, 0.0f, 0.14f * s, 0.013f * s, r);
        if (r.hp_max > 0.0f) { // HP over the stamina bar, the same width
            cv.Text(p, 0.0f, 0.056f * s, 0.016f * s, Fmt("HP %.0f / %.0f", r.hp, r.hp_max), kWhite,
                    UiAlign::Center);
            HpBar(cv, p, 0.0f, 0.044f * s, 0.14f * s, 0.013f * s, r);
        }
        if (label_alpha > 0.0f) {
            cv.alpha = label_alpha;
            LabelLine(cv, p, 0.0f, -0.026f * s, 0.013f * s, text);
            cv.alpha = 1.0f;
        }
    }
    if (c.popups) {
        // The costs, floating up from where the swings connected.
        for (const SwingReadout::Popup& pop : r.popups) {
            const float age = float(r.now - pop.t);
            if (age < 0.0f || age > 1.2f) {
                continue;
            }
            const Vec3 at = pop.point + Vec3{0.0f, 0.06f + 0.15f * age / 1.2f, 0.0f};
            const float distance = std::max(Length(at - head.position), 0.3f);
            const UiPanel p = UiPanel::Facing(at, head.position, {0, 1, 0});
            cv.alpha = std::clamp(1.0f - (age - 0.6f) / 0.6f, 0.0f, 1.0f);
            cv.Text(p, 0.0f, 0.0f, 0.035f * distance, Fmt("-%g", pop.amount),
                    pop.world ? kClang : UiColor{1.0f, 0.45f, 0.25f, 1.0f}, UiAlign::Center);
        }
        cv.alpha = 1.0f;
    }
    cv.on_top = false;
}

std::string SwingLabelText(const SwingReadout& readout) {
    const LabelText t = TextOf(readout);
    return t.right.empty() ? t.main : t.main + "  " + t.right;
}

} // namespace sb
