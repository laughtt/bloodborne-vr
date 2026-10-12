// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR: the game's 2D UI as a head-locked quad in the middle of the view (the HUD layer,
// docs/vr/findings/25-hud-layer.md) is hidden in plain gameplay: HP and stamina live on the left
// wrist (docs/vr/findings/26-game-ui.md section "HP and stamina on the wrist"). It comes back
// while something needs it. Pure logic, no Vulkan, tested in tests/test_xr_hud_center.cpp.
//
//   BB_XR_HUD_CENTER=menus|subtitles|auto|on|off   (control file key hud_center, live)
//     menus (default)   shown only while a game menu is open
//     subtitles         menus, plus messages but only their band (bottom centre of the UI image,
//                       the rest of the quad cleared): subtitles, pickup lines, prompts
//     auto              shown while a game menu is open (the host's BBXR_GAME_MENU_OPEN) or the UI
//                       image has "message" content outside the places the always-on HUD uses:
//                       subtitles, pickup messages, prompts, YOU DIED, boss bar. A short hold keeps
//                       it up after the last sign so a line of text does not flicker.
//     on                always shown (the old behaviour, with the game's own bars)
//     off               never shown (menus too: you would not see them)
//   Only the quad is hidden: the BB_XR_FPS counter is still drawn on an otherwise empty HUD layer.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace XrHudCenter {

enum class Mode { Auto, Menus, On, Off, Subtitles };

/// The default: the quad only while a game menu is open (the auto detector kept popping it up).
constexpr Mode kDefaultMode = Mode::Menus;

inline const char* ModeName(Mode m) {
    switch (m) {
    case Mode::Menus: return "menus";
    case Mode::On: return "on";
    case Mode::Off: return "off";
    case Mode::Subtitles: return "subtitles";
    default: return "auto";
    }
}

/// "auto", "menus", "on"/"1"/"true", "off"/"0"/"false" (case and surrounding blanks ignored);
/// anything else keeps `fallback`.
inline Mode ParseMode(const std::string& text, Mode fallback) {
    std::string w;
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            w += char(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (w == "auto") return Mode::Auto;
    if (w == "subtitles" || w == "subs") return Mode::Subtitles;
    if (w == "menus" || w == "menu") return Mode::Menus;
    if (w == "on" || w == "1" || w == "true" || w == "always") return Mode::On;
    if (w == "off" || w == "0" || w == "false" || w == "never") return Mode::Off;
    return fallback;
}

// ---- message detection ---------------------------------------------------------------------

/// The HUD image is probed on a grid of kProbeW x kProbeH single bilinear samples (one per
/// 15x15 pixels of the 1920x1080 image: thin text is hit by about one probe in five).
constexpr uint32_t kProbeW = 128, kProbeH = 72;
/// A probe counts as lit at this alpha (0-255).
constexpr uint8_t kLitAlpha = 40;
/// Lit probes ABOVE THE SESSION BASELINE that make "something is being said". The game's always-on
/// HUD is not all inside the zones above (the headset log had ~50 lit probes in plain play), so
/// the Gate tracks the lowest recent count as the baseline and compares against that. A lock-on
/// reticle, a damage number or an enemy bar add a handful; a subtitle line, a pickup message, a
/// prompt box or a fade add dozens.
constexpr uint32_t kActivityMin = 14;
/// The baseline creeps up this many probes per second when the count stays above it (a long
/// boss bar becomes the new normal and the quad hides again); it drops to any lower count at once.
constexpr double kBaselineLeakPerS = 1.0;
/// Frames this long after "the UI covers the screen" (loading, full-screen menus) are ignored: the
/// UI image still holds the loading screen for a moment.
constexpr double kIgnoreAfterCoverS = 1.5;
/// How long the quad stays after the last sign of a message / menu.
constexpr double kHoldMessageS = 2.5;
constexpr double kHoldMenuS = 0.4;

struct Zone {
    float x0, y0, x1, y1; // fractions of the image
};
/// Where the always-on HUD lives (the bars top left, the item cross bottom left, the souls and
/// bullets bottom right): probes there never count.
inline constexpr Zone kAlwaysOnZones[] = {
    {0.00f, 0.00f, 0.36f, 0.24f},
    {0.00f, 0.70f, 0.26f, 1.00f},
    {0.74f, 0.82f, 1.00f, 1.00f},
};

inline bool InAlwaysOnZone(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const float fx = (float(x) + 0.5f) / float(w), fy = (float(y) + 0.5f) / float(h);
    for (const Zone& z : kAlwaysOnZones) {
        if (fx >= z.x0 && fx < z.x1 && fy >= z.y0 && fy < z.y1) {
            return true;
        }
    }
    return false;
}

/// The lit probes outside the always-on zones: `rgba` is w*h RGBA8 texels.
inline uint32_t Activity(const uint8_t* rgba, uint32_t w, uint32_t h) {
    uint32_t n = 0;
    if (!rgba || !w || !h) {
        return 0;
    }
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            if (rgba[(size_t(y) * w + x) * 4 + 3] >= kLitAlpha && !InAlwaysOnZone(x, y, w, h)) {
                ++n;
            }
        }
    }
    return n;
}

/// The band kept in Subtitles mode (fractions of the image): subtitles and prompts sit bottom centre.
inline constexpr Zone kMessageBand = {0.15f, 0.66f, 0.85f, 0.97f};

// ---- the decision --------------------------------------------------------------------------

enum class Reason { None, Forced, Menu, Message };

inline const char* ReasonName(Reason r) {
    switch (r) {
    case Reason::Forced: return "hud_center=on";
    case Reason::Menu: return "a game menu is open";
    case Reason::Message: return "message content in the UI";
    default: return "plain gameplay";
    }
}

/// Whether the HUD quad is shown this frame. Call once per presented frame with a steady clock.
struct Gate {
    double menu_until = -1e9, message_until = -1e9;
    double baseline = -1.0; // < 0: no sample yet
    double last_now = 0.0;
    bool shown = false;
    bool band_only = false; // Subtitles mode: shown for a message, so only the message band
    Reason reason = Reason::None;

    /// `ignore`: the UI image is not trustworthy this frame (a full-screen UI is up or just was):
    /// no sample is taken. The menu flag still counts.
    bool Update(double now, Mode mode, bool menu_open, uint32_t activity, bool ignore = false) {
        if (menu_open) {
            menu_until = now + kHoldMenuS;
        }
        if (!ignore) {
            const double a = double(activity);
            const double dt = std::clamp(now - last_now, 0.0, 1.0);
            if (baseline < 0.0 || a < baseline) {
                baseline = a;
            } else if (a < baseline + double(kActivityMin)) {
                baseline = std::min(a, baseline + kBaselineLeakPerS * dt);
            } else {
                baseline += kBaselineLeakPerS * dt; // a message, or the new normal if it stays
            }
            if (a >= baseline + double(kActivityMin)) {
                message_until = now + kHoldMessageS;
            }
        }
        last_now = now;
        band_only = false;
        switch (mode) {
        case Mode::On:
            reason = Reason::Forced;
            shown = true;
            break;
        case Mode::Off:
            reason = Reason::None;
            shown = false;
            break;
        case Mode::Menus:
            shown = now <= menu_until;
            reason = shown ? Reason::Menu : Reason::None;
            break;
        case Mode::Subtitles:
        default:
            if (now <= menu_until) {
                shown = true;
                reason = Reason::Menu;
            } else if (now <= message_until) {
                shown = true;
                reason = Reason::Message;
                band_only = mode == Mode::Subtitles;
            } else {
                shown = false;
                reason = Reason::None;
            }
            break;
        }
        return shown;
    }
};

} // namespace XrHudCenter
