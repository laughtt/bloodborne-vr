// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The wrist HP and stamina bars (docs/vr/findings/26-game-ui.md, "HP and stamina on the wrist"):
// the pure parts, shared by combat_hud.cpp (the drawing) and the game UI (vr_game_ui.cpp: where
// the colours come from). Header only, no canvas, no GPU.
//   - BarFraction: how much of a bar is filled;
//   - ParseBarColor: "green", "red", "#33cc55", "0.3,0.9,0.4" or "80,220,100" -> a colour;
//   - Colors / ApplyControlText: the two bar colours and the "hp_color=red stamina_color=green"
//     keys of the control file (BB_XR_IQ_CONTROL, polled live by the game UI).
// Both bars default to green (the player asked for it); the game's own HP bar is red.

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

#include "ui_layout.h"

namespace sb::bars {

/// value / max clamped to 0..1; 0 when max is not positive or a value is not finite.
inline float BarFraction(float value, float max) {
    if (!(max > 0.0f) || !std::isfinite(value) || !std::isfinite(max)) {
        return 0.0f;
    }
    return std::clamp(value / max, 0.0f, 1.0f);
}

/// The default bar green: the stamina bar's colour since findings/20 (linear RGB).
inline constexpr UiColor kGreen{0.30f, 0.88f, 0.38f, 1.0f};
/// The bar red (the game's own HP colour), blue, ... as they look on the bar (linear RGB).
inline constexpr UiColor kRed{0.95f, 0.22f, 0.16f, 1.0f};

inline float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline std::string Lower(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            out += char(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

/// A colour from a word, "#rrggbb" (sRGB bytes), or "r,g,b" (all <= 1: sRGB fractions; else
/// bytes). False and `out` untouched when the text is not one of them.
inline bool ParseBarColor(const std::string& text, UiColor& out) {
    const std::string t = Lower(text);
    if (t.empty()) {
        return false;
    }
    struct Named {
        const char* name;
        UiColor color;
    };
    static const Named kNamed[] = {
        {"green", kGreen},
        {"red", kRed},
        {"blue", {0.22f, 0.50f, 1.00f, 1.0f}},
        {"cyan", {0.20f, 0.85f, 0.95f, 1.0f}},
        {"yellow", {1.00f, 0.82f, 0.15f, 1.0f}},
        {"orange", {1.00f, 0.50f, 0.12f, 1.0f}},
        {"purple", {0.65f, 0.35f, 1.00f, 1.0f}},
        {"white", {0.95f, 0.95f, 0.95f, 1.0f}},
    };
    for (const Named& n : kNamed) {
        if (t == n.name) {
            out = n.color;
            return true;
        }
    }
    if (t[0] == '#') {
        if (t.size() != 7) {
            return false;
        }
        char* end = nullptr;
        const unsigned long v = std::strtoul(t.c_str() + 1, &end, 16);
        if (end != t.c_str() + 7) {
            return false;
        }
        out = {SrgbToLinear(float((v >> 16) & 255) / 255.0f), SrgbToLinear(float((v >> 8) & 255) / 255.0f),
               SrgbToLinear(float(v & 255) / 255.0f), 1.0f};
        return true;
    }
    float c[3];
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        char* end = nullptr;
        c[i] = std::strtof(t.c_str() + pos, &end);
        if (end == t.c_str() + pos || !std::isfinite(c[i]) || c[i] < 0.0f) {
            return false;
        }
        pos = size_t(end - t.c_str());
        if (i < 2) {
            if (pos >= t.size() || t[pos] != ',') {
                return false;
            }
            ++pos;
        }
    }
    if (pos != t.size()) {
        return false;
    }
    const float scale = (c[0] <= 1.0f && c[1] <= 1.0f && c[2] <= 1.0f) ? 1.0f : 1.0f / 255.0f;
    out = {SrgbToLinear(std::min(c[0] * scale, 1.0f)), SrgbToLinear(std::min(c[1] * scale, 1.0f)),
           SrgbToLinear(std::min(c[2] * scale, 1.0f)), 1.0f};
    return true;
}

/// The two bar colours.
struct Colors {
    UiColor hp = kGreen, stamina = kGreen;
    bool operator==(const Colors& o) const {
        return hp.r == o.hp.r && hp.g == o.hp.g && hp.b == o.hp.b && stamina.r == o.stamina.r &&
               stamina.g == o.stamina.g && stamina.b == o.stamina.b;
    }
};

/// Applies the `hp_color=...` and `stamina_color=...` tokens of a control file's text (other
/// keys are for other readers and skipped); a value that does not parse is skipped. True when a
/// colour was set.
inline bool ApplyControlText(const std::string& text, Colors& colors) {
    bool any = false;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        size_t j = i;
        while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j]))) {
            ++j;
        }
        const std::string token = text.substr(i, j - i);
        i = j;
        const size_t eq = token.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Lower(token.substr(0, eq)), value = token.substr(eq + 1);
        if (key == "hp_color") {
            any |= ParseBarColor(value, colors.hp);
        } else if (key == "stamina_color") {
            any |= ParseBarColor(value, colors.stamina);
        }
    }
    return any;
}

/// The paused-regeneration tint of a stamina colour (the colour pulled toward grey).
inline UiColor Paused(UiColor c) {
    const float k = 0.45f, g = 0.55f;
    return {c.r + (g - c.r) * k, c.g + (g - c.g) * k, c.b + (g - c.b) * k, c.a};
}

} // namespace sb::bars
