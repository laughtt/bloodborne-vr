// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The game's VR UI between the controllers and the game (docs/vr/findings/26-game-ui.md): which
// presses the menus take and which still reach the game. Pure logic (no OpenXR), unit tested in
// vr_game_ui_test.cpp. xr_input.cpp maps the controllers to the pad as before; this filter sits
// after it, in the bridge's GetInput (what the game reads) and in the UI thread (what the menus
// read), both on the same mapped state.
//
// Rules (BB_VR_UI=1):
//   - R3 stays lock-on: a press shorter than hold_s reaches the game as a short R3 tap when it is
//     released (so lock-on comes on release, at most hold_s late); held hold_s it never reaches
//     the game and the menus see it as an "r3" press (the inventory). While an r3 menu is open a
//     tap goes to the menus too (it closes it).
//   - L3: xr_input's L3 layer sends a standing click as a short L3 pulse on release (a 1 s hold is
//     the recenter and sends nothing, so the hold never opens a menu). A pulse that starts with
//     the left stick centered is the menus' "l3" (the quick ring; with another menu open it closes
//     the menus); the game never gets it (L3 standing does nothing useful in Bloodborne). L3 that
//     starts while the left stick moves is the game's (the jump), unless a menu is open: then it
//     closes the menus and the game does not jump.
//   - While the menus are engaged (a ring or grid open, a hand on a body slot, a pointer or a
//     fingertip on a panel) the game gets no buttons and no stick the menus use (swings, the
//     triggers' weapon swap and dodges included); a button still held when they let go stays
//     hidden until it is released (so the trigger that picked an item does not then swap weapons);
//   - a combo's key (B of "right_grip+b") pressed while its modifiers are held never reaches the
//     game, even in the moment before the menu opens (SetClaim).

#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>

#include "bbport_xr_bridge.h"

namespace vrui {

/// The left stick's deflection, 0..1 (pad values: 128 centered).
inline float StickMagnitude(uint8_t x, uint8_t y) {
    const float fx = (float(x) - 128.0f) / 127.0f, fy = (float(y) - 128.0f) / 127.0f;
    return std::sqrt(fx * fx + fy * fy);
}

class InputFilter {
public:
    static constexpr float kMoving = 0.3f;     // the left stick moves the character (xr_input)
    static constexpr double kPulseS = 0.12;    // an injected R3 tap

    struct Settings {
        bool enabled = false;
        double r3_hold_s = 0.5;
        bool r3_menu = true; // some menu opens on "r3"
        bool lock_on = true; // false: R3 never reaches the game (no lock-on); the hold still opens menus
        bool l3_menu = true; // some menu opens on "l3"
        uint32_t menu_face = 0; // pad bits of face buttons (A/B/X/Y) some menu opens on: the game never gets them
    };

    /// The menus' side of a frame (UI thread): what the menus may see as pressed.
    struct UiView {
        uint32_t buttons = 0;  // the mapped buttons; L3 only for a standing click, R3 only when
                               // held hold_s (or a tap while an r3 menu is open)
        bool close_menus = false; // an L3 while walking with a menu open: close them
    };

    void Configure(const Settings& next) {
        std::scoped_lock lock{mutex};
        settings = next;
    }

    /// The menus' state, from the UI thread after each update.
    void SetMenuState(bool engaged, bool r3_menu_open, bool left_stick, bool right_stick) {
        std::scoped_lock lock{mutex};
        if (menus_engaged && !engaged) {
            sticky = last_raw; // what is held now stays hidden until released
        }
        menus_engaged = engaged;
        r3_open = r3_menu_open;
        stick_used[0] = left_stick;
        stick_used[1] = right_stick;
    }

    /// The game's own menu is open (the host's menu page): Apply turns the sticks into d-pad
    /// navigation and maps the face buttons for menus (any thread).
    void SetGameMenu(bool open) {
        std::scoped_lock lock{mutex};
        game_menu = open;
        if (!open) {
            nav_dir = 0;
        }
    }

    /// Keys whose binding's modifiers are held now (e.g. B while the right grip is squeezed, for
    /// "right_grip+b"): the game never sees them press, even before the menu opens (UI thread).
    void SetClaim(uint32_t mask) {
        std::scoped_lock lock{mutex};
        claim = mask;
    }

    /// The menus' view of the latest mapped input (UI thread, every headset frame).
    UiView Step(double now, const BbXrInput& in) {
        std::scoped_lock lock{mutex};
        Observe(now, in);
        UiView out;
        out.buttons = in.buttons & ~(BBXR_BTN_L3 | BBXR_BTN_R3);
        if (!settings.enabled) {
            out.buttons = in.buttons;
            return out;
        }
        if (settings.menu_face & BBXR_BTN_TRIANGLE) { // Y: the button, not the vial gesture
            out.buttons &= ~BBXR_BTN_TRIANGLE;
            if (in.reserved[0] & BBXR_RAW_Y) {
                out.buttons |= BBXR_BTN_TRIANGLE;
            }
        }
        if (l3_class == L3Class::Tap && (in.buttons & BBXR_BTN_L3)) {
            out.buttons |= BBXR_BTN_L3;
        }
        if (l3_class == L3Class::Close && !close_reported) {
            close_reported = true;
            out.close_menus = true;
        }
        if (r3_ui_until > now || (r3_down && r3_held_fired)) {
            out.buttons |= BBXR_BTN_R3;
        }
        return out;
    }

    /// What the game gets (the bridge's GetInput, any thread): `in` is changed in place.
    void Apply(double now, BbXrInput& in, bool trigger_confirm = false) {
        std::scoped_lock lock{mutex};
        Observe(now, in);
        if (!settings.enabled) {
            return;
        }
        uint32_t b = in.buttons;
        // A face button a menu opens on never reaches the game (the vial gesture's triangle does).
        b &= ~(settings.menu_face & ~BBXR_BTN_TRIANGLE);
        if ((settings.menu_face & BBXR_BTN_TRIANGLE) && (in.reserved[0] & BBXR_RAW_Y)) {
            b &= ~BBXR_BTN_TRIANGLE;
        }
        if (settings.r3_menu || !settings.lock_on) {
            b &= ~BBXR_BTN_R3;
            if (settings.lock_on && r3_game_until > now) {
                b |= BBXR_BTN_R3;
            }
        }
        if ((b & BBXR_BTN_L3) && l3_class != L3Class::Game) {
            b &= ~BBXR_BTN_L3;
        }
        if (game_menu && !menus_engaged) {
            b = MenuNav(now, in, b, trigger_confirm);
        } else if (menus_engaged) {
            b = 0; // swings, weapon swaps, dodges, items: the menus have the hands
            if (stick_used[0]) {
                in.lx = in.ly = 128;
            }
            if (stick_used[1]) {
                in.rx = in.ry = 128;
            }
            in.l2 = in.r2 = 0;
        }
        // A combo's key pressed while its modifiers are held stays hidden until released.
        sticky |= in.buttons & claim & ~last_applied;
        last_applied = in.buttons;
        sticky &= in.buttons; // released buttons are the game's again
        b &= ~sticky;
        if (!(b & BBXR_BTN_L2)) {
            in.l2 = 0;
        }
        if (!(b & BBXR_BTN_R2)) {
            in.r2 = 0;
        }
        in.buttons = b;
    }

    bool Engaged() const {
        std::scoped_lock lock{mutex};
        return menus_engaged;
    }

    /// The last finished R3 press (UI thread, for the log): how long it was held and whether it
    /// reached the hold (the menus' r3). False when none finished since the last call.
    bool TakeR3Episode(double& held_s, bool& hold) {
        std::scoped_lock lock{mutex};
        if (!episode_ready) {
            return false;
        }
        episode_ready = false;
        held_s = episode_s;
        hold = episode_hold;
        return true;
    }

    /// The game's own menu: the stick (either) is the d-pad with repeat, A (lower) = cross, B =
    /// circle, a trigger pull = cross. Swings, gun, swap pulses and gesture buttons are dropped.
    /// Kept: Options, L1/L2-less shoulder tabs (L1), X, the Y button, the touchpad.
    static constexpr double kNavFirstS = 0.35, kNavRepeatS = 0.14, kNavPulseS = 0.07;
    static constexpr float kNavDeflect = 0.55f;

    uint32_t MenuNav(double now, BbXrInput& in, uint32_t b, bool trigger_confirm) {
        uint32_t out = b & (BBXR_BTN_OPTIONS | BBXR_BTN_L1 | BBXR_BTN_SQUARE | BBXR_BTN_TOUCHPAD);
        if (in.reserved[0] & BBXR_RAW_Y) {
            out |= BBXR_BTN_TRIANGLE;
        }
        // xr_input maps A to circle and B to cross for the world; the menus want the reverse.
        if (b & BBXR_BTN_CIRCLE) out |= BBXR_BTN_CROSS;
        if (b & BBXR_BTN_CROSS) out |= BBXR_BTN_CIRCLE;
        if (trigger_confirm) out |= BBXR_BTN_CROSS;
        // The stick with the larger deflection chooses the direction.
        const float lx = (float(in.lx) - 128.0f) / 127.0f, ly = (float(in.ly) - 128.0f) / 127.0f;
        const float rx = (float(in.rx) - 128.0f) / 127.0f, ry = (float(in.ry) - 128.0f) / 127.0f;
        const bool use_l = lx * lx + ly * ly >= rx * rx + ry * ry;
        const float x = use_l ? lx : rx, y = use_l ? ly : ry; // pad y grows downwards
        int dir = 0;
        if (std::sqrt(x * x + y * y) >= kNavDeflect) {
            dir = std::fabs(x) > std::fabs(y) ? (x > 0 ? 2 : 4) : (y > 0 ? 3 : 1); // up right down left
        }
        if (dir != nav_dir) {
            nav_dir = dir;
            if (dir) {
                nav_until = now + kNavPulseS;
                nav_next = now + kNavFirstS;
            }
        } else if (dir && now >= nav_next) {
            nav_until = now + kNavPulseS;
            nav_next = now + kNavRepeatS;
        }
        if (nav_dir && now < nav_until) {
            static constexpr uint32_t pad[5] = {0, BBXR_BTN_UP, BBXR_BTN_RIGHT, BBXR_BTN_DOWN, BBXR_BTN_LEFT};
            out |= pad[nav_dir];
        }
        in.lx = in.ly = in.rx = in.ry = 128; // neither the character nor the camera moves under the menu
        in.l2 = in.r2 = 0;
        return out;
    }

private:
    enum class L3Class { None, Tap, Game, Close };

    /// Tracks the R3 and L3 episodes from whichever reader sees the input first.
    void Observe(double now, const BbXrInput& in) {
        last_raw = in.buttons;
        if (!settings.enabled) {
            ResetEpisodes(in.buttons); // (a press that ends while the filter is off must not linger)
            return;
        }
        const bool r3 = (in.buttons & BBXR_BTN_R3) != 0;
        if (!settings.r3_menu) {
            r3_down = r3_held_fired = false;
        } else if (r3_down && now < r3_since) {
            r3_since = now; // (a clock that went back: the hold starts over)
        }
        if (settings.r3_menu) {
            if (r3 && !r3_down) {
                r3_down = true;
                r3_since = now;
                r3_held_fired = false;
            } else if (r3 && r3_down && !r3_held_fired && now - r3_since >= settings.r3_hold_s) {
                r3_held_fired = true; // the menus' r3, from now until it is released
            } else if (!r3 && r3_down) {
                r3_down = false;
                episode_ready = true;
                episode_s = now - r3_since;
                episode_hold = r3_held_fired;
                if (!r3_held_fired) {
                    // A tap: the open r3 menu's (it closes it), else lock-on.
                    if (r3_open) {
                        r3_ui_until = now + kPulseS;
                    } else if (settings.lock_on) {
                        r3_game_until = now + kPulseS;
                    }
                }
                r3_held_fired = false;
            }
        }
        const bool l3 = (in.buttons & BBXR_BTN_L3) != 0;
        if (l3 && !l3_down) {
            close_reported = false;
            const bool moving = StickMagnitude(in.lx, in.ly) > kMoving;
            if (moving) {
                l3_class = menus_engaged ? L3Class::Close : L3Class::Game;
            } else {
                l3_class = settings.l3_menu || menus_engaged ? L3Class::Tap : L3Class::Game;
            }
        } else if (!l3) {
            l3_class = l3_class == L3Class::Close && !close_reported ? l3_class : L3Class::None;
        }
        l3_down = l3;
    }

    /// Forgets the R3 / L3 episodes in progress (the filter is off): the next press starts clean.
    void ResetEpisodes(uint32_t buttons) {
        r3_down = r3_held_fired = false;
        r3_game_until = r3_ui_until = -1.0;
        episode_ready = false;
        l3_down = (buttons & BBXR_BTN_L3) != 0;
        l3_class = L3Class::None;
        close_reported = true;
        sticky = 0;
        last_applied = buttons;
    }

    mutable std::mutex mutex;
    Settings settings;
    bool menus_engaged = false, r3_open = false, game_menu = false;
    int nav_dir = 0;
    double nav_until = -1.0, nav_next = 0.0;
    bool stick_used[2] = {};
    uint32_t last_raw = 0, sticky = 0, claim = 0, last_applied = 0;
    bool r3_down = false, r3_held_fired = false;
    bool episode_ready = false, episode_hold = false;
    double episode_s = 0.0;
    double r3_since = 0.0, r3_game_until = -1.0, r3_ui_until = -1.0;
    bool l3_down = false, close_reported = false;
    L3Class l3_class = L3Class::None;
};

} // namespace vrui
