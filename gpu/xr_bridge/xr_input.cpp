// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Default mapping: one button, one action (docs/vr/findings/26-game-ui.md; attacks are swings):
//   left stick         move; click = L3: the jump while the stick moves the character; standing,
//                      a click (sent on release) is the game UI's DASHBOARD toggle with BB_VR_UI=1
//                      (vr_ui_filter.h; plain L3 without it); held 1 s alone = recenter
//   right stick        camera / turn; click = R3 (lock-on)
//   A circle (dodge, sprint), B cross (interact), X square (use the quick item, weak haptic tick),
//   Y triangle (blood vial)
//   triggers           with gestures (BB_XR_TRIGGERS=gun, the default): the left trigger is L2
//                      (shoot, held, with the analog L2); each right pull is ONE d-pad right pulse
//                      (120 ms, logged: swap the right weapon); pull above 0.6, release below 0.3
//                      to pull again; a haptic tick. BB_XR_TRIGGERS=switch: the left trigger is a
//                      d-pad left pulse too (swap the left weapon) and does not shoot. While the dashboard is open they are its
//                      pointer's click (the game does not see them). BB_XR_TRIGGERS=attack or
//                      gestures off: right trigger R1, left trigger L2 (held, with the analog L2).
//   left grip          L1 (transform): ONE 100 ms pulse when the grip is squeezed (a held L1 locked the
//                      character's locomotion in the game, and a controller held in the hand reads as squeezed)
//   right grip         gestures off = R2. Gestures on = heavy modifier: a swing with the grip held
//                      is a heavy attack (R2); keeping the grip held after the swing keeps R2 down
//                      until release (charged attack); a grip held without a swing does nothing.
//   left menu button   options (pause) on release; held 1 s = recenter (bbport_xr_bridge.cpp).
//                      Pimax keeps that button for its system menu: the dashboard has Options and
//                      Recenter buttons.
//   The d-pad, the touchpad (gestures menu) and the tool/bullet choice are dashboard buttons.
// BB_XR_CHORDS=1 (debugging) brings back the old chords: while L3 is held a right stick flick is
// the d-pad (the camera stops), B is options and A the touchpad; right grip + right stick flick is
// the d-pad. B or A pressed before L3 stay cross or circle until released.
// Diagnostics: once the controllers are known, the paths bound to each action are logged, and
// each action's first use per hand; "menu button not reported by the runtime" when the menu
// action has no bound path. BB_XR_INPUT_DIAG=1 (default on in the sandboxes) adds a second action
// set bound to every input path of the profiles and logs each path the first time it reports
// anything (thumbrest and touch sensors, system buttons), to see what a runtime really sends.
// Gestures (a haptic pulse on the hand, or with BB_VR_UI=1 the game UI's weak tick; each fire is
// logged, one line per kind per 2 s):
//   right hand swing faster than 2 m/s, moving away from the head (not the return stroke), not
//     while L3 is held -> R1 (R2 with the right grip held or above 3.5 m/s),
//   left hand thrust along its pointing direction faster than 1.8 m/s, measured relative to the
//     head and ignored for 300 ms after a snap turn or recenter -> L2,
//   left hand within 10 cm of the mouth with the left grip held for 0.3 s -> triangle (vial).
// Gesture buttons are pressed for 100 ms.

#include <algorithm>
#include "xr_input.h"
#include "xr_input_logic.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace XrInput {
namespace {

using Clock = std::chrono::steady_clock;

constexpr float HeavySpeed = 3.5f;
constexpr float SwingRearm = 1.0f; // shoot rearm only
constexpr float ShootSpeed = 1.8f;
constexpr double TriggerPulseMs = 120.0;
constexpr double PulseMs = 100.0;
constexpr const char* OptionsChord = "the dashboard's Options button (BB_VR_UI=1; left stick click + B with BB_XR_CHORDS=1)";

struct Vec3 {
    float x, y, z;
};

Vec3 Rotate(const XrQuaternionf& q, Vec3 v) {
    // v + 2w(u x v) + 2u x (u x v)
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t{2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z),
                 2.0f * (u.x * v.y - u.y * v.x)};
    return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

float Length(Vec3 v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

uint8_t Axis(float v) {
    const int x = int(std::lround(128.0f + 127.0f * v));
    return uint8_t(x < 0 ? 0 : x > 255 ? 255 : x);
}

uint8_t Trigger(float v) {
    const int x = int(std::lround(255.0f * v));
    return uint8_t(x < 0 ? 0 : x > 255 ? 255 : x);
}

struct State {
    LogFn log = nullptr;
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrActionSet set = XR_NULL_HANDLE;
    XrPath hands[2]{};
    XrAction move{}, look{}, trigger{}, grip{}, stick_click{}, a{}, b{}, x{}, y{}, menu{};
    // The same sticks as separate axes, for runtimes whose vector2 thumbstick stays at zero.
    XrAction move_x{}, move_y{}, look_x{}, look_y{};
    bool logged_stick[2]{};
    float stick_peak[2][2]{}; // largest push per stick and axis, for sticks that stop short of 1
    XrAction grip_pose{}, aim_pose{}, haptic{};
    XrSpace grip_space[2]{}, aim_space[2]{}, local{}, view{};
    bool gestures = true;
    bool trigger_switch = true; // triggers swap weapons (d-pad left/right) instead of R1/L2
    bool left_gun = true;       // with trigger_switch: the left trigger still shoots (L2)
    bool profile_logged = false;

    std::mutex mutex;
    BbXrInput latest{};
    uint32_t pending_gestures = 0;

    // Gesture state (frame loop thread only).
    bool shoot_armed = true;
    Clock::time_point pulse_until[32]{};
    xrpad::SwingGate swing;
    xrpad::HeavyCharge charge;
    xrpad::MouthDwell mouth;
    xrpad::TurnGuard turn;
    xrpad::TriggerEdge trigger_edge[2];
    xrpad::RateLimit gesture_log;
    xrpad::RateLimit trigger_log;
    bool x_prev = false;
    bool l1_down = false; // the left grip is squeezed (above 0.6 until it drops below 0.35)
    bool chords = false;  // BB_XR_CHORDS=1: the old L3 layer chords and the right-grip flick
    bool ui = false;      // BB_VR_UI=1: the bridge's game UI gives the swing feedback itself
    xrpad::MenuTap menu_tap;
    Clock::time_point last_gesture[2]{};

    // The L3 layer (frame loop thread only).
    xrpad::L3Layer layer;
    Clock::time_point start = Clock::now();
    std::atomic<bool> recenter{false}; // for TakeRecenter (RequestRecenter: any thread)
    bool menu_raw = false; // for MenuButtonDown
    float analog_trigger[2] = {}, analog_grip[2] = {}; // for Analog (frame loop thread)

    // Diagnostics: per gameplay action and hand, used yet; the bound paths, once known.
    struct Watched {
        XrAction State::*action;
        const char* name;
        XrActionType type;
        bool per_hand;
        bool seen[2];
    };
    std::vector<Watched> watched;
    bool bindings_logged = false;
    // BB_XR_INPUT_DIAG=1: a second action set bound to every input path of the profiles.
    struct DiagPath {
        std::string path;
        bool is_float;
        XrAction action;
        bool seen;
    };
    XrActionSet diag_set = XR_NULL_HANDLE;
    std::vector<DiagPath> diag;
};

State* s = nullptr;

bool Ok(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    s->log(2, "%s failed (%d)", what, int(result));
    return false;
}

XrPath Path(const char* text) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(s->instance, text, &path);
    return path;
}

XrAction MakeAction(const char* name, XrActionType type, bool per_hand) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.actionType = type;
    if (per_hand) {
        info.countSubactionPaths = 2;
        info.subactionPaths = s->hands;
    }
    XrAction action = XR_NULL_HANDLE;
    Ok(xrCreateAction(s->set, &info, &action), name);
    return action;
}

struct Binding {
    XrAction State::*action;
    const char* path;
};

/// Every input path of a profile for the BB_XR_INPUT_DIAG action set: "both/..." stands for the
/// left and the right hand. Only paths the OpenXR spec lists for the profile (a runtime rejects
/// the whole suggestion for one unknown path; Suggest then retries without these).
constexpr const char* kTouchPaths[] = {
    "left/input/x/click",          "left/input/x/touch",          "left/input/y/click",
    "left/input/y/touch",          "left/input/menu/click",       "right/input/a/click",
    "right/input/a/touch",         "right/input/b/click",         "right/input/b/touch",
    "both/input/squeeze/value",    "both/input/trigger/value",    "both/input/trigger/touch",
    "both/input/thumbstick/x",     "both/input/thumbstick/y",     "both/input/thumbstick/click",
    "both/input/thumbstick/touch", "both/input/thumbrest/touch",
};
constexpr const char* kIndexPaths[] = {
    "both/input/system/click",     "both/input/system/touch",     "both/input/a/click",
    "both/input/a/touch",          "both/input/b/click",          "both/input/b/touch",
    "both/input/squeeze/value",    "both/input/squeeze/force",    "both/input/trigger/click",
    "both/input/trigger/value",    "both/input/trigger/touch",    "both/input/thumbstick/x",
    "both/input/thumbstick/y",     "both/input/thumbstick/click", "both/input/thumbstick/touch",
    "both/input/trackpad/x",       "both/input/trackpad/y",       "both/input/trackpad/force",
    "both/input/trackpad/touch",
};
constexpr const char* kSimplePaths[] = {"both/input/select/click", "both/input/menu/click"};

/// The full paths ("/user/hand/left/input/x/click") of a profile's diagnostic list.
std::vector<std::string> ExpandPaths(const char* const* list, size_t count) {
    std::vector<std::string> out;
    for (size_t i = 0; i < count; ++i) {
        const std::string entry = list[i];
        if (entry.rfind("both/", 0) == 0) {
            out.push_back("/user/hand/left/" + entry.substr(5));
            out.push_back("/user/hand/right/" + entry.substr(5));
        } else {
            out.push_back("/user/hand/" + entry);
        }
    }
    return out;
}

/// The diagnostic action of a path, created on first use (BB_XR_INPUT_DIAG only).
XrAction DiagAction(const std::string& path) {
    for (const auto& d : s->diag) {
        if (path == d.path) {
            return d.action;
        }
    }
    const auto ends_with = [&](const char* tail) {
        const size_t n = std::strlen(tail);
        return path.size() >= n && path.compare(path.size() - n, n, tail) == 0;
    };
    const bool is_float =
        ends_with("/value") || ends_with("/force") || ends_with("/x") || ends_with("/y");
    char name[32];
    std::snprintf(name, sizeof(name), "diag_%02u", unsigned(s->diag.size()));
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, path.c_str(), XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.actionType = is_float ? XR_ACTION_TYPE_FLOAT_INPUT : XR_ACTION_TYPE_BOOLEAN_INPUT;
    XrAction action = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateAction(s->diag_set, &info, &action))) {
        return XR_NULL_HANDLE;
    }
    s->diag.push_back({path, is_float, action, false});
    return action;
}

/// The profile's gameplay bindings, plus (BB_XR_INPUT_DIAG) one diagnostic action per input
/// path. A later suggestion for a profile replaces an earlier one, so both go in one call; if the
/// runtime rejects it, the gameplay bindings are suggested alone.
void Suggest(const char* profile, std::initializer_list<Binding> list,
             const char* const* diag_paths = nullptr, size_t diag_count = 0) {
    std::vector<XrActionSuggestedBinding> bindings;
    for (const auto& binding : list) {
        bindings.push_back({s->*binding.action, Path(binding.path)});
    }
    const size_t gameplay = bindings.size();
    if (s->diag_set && diag_paths) {
        for (const std::string& path : ExpandPaths(diag_paths, diag_count)) {
            if (const XrAction action = DiagAction(path)) {
                bindings.push_back({action, Path(path.c_str())});
            }
        }
    }
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = Path(profile);
    suggested.countSuggestedBindings = uint32_t(bindings.size());
    suggested.suggestedBindings = bindings.data();
    XrResult result = xrSuggestInteractionProfileBindings(s->instance, &suggested);
    if (XR_FAILED(result) && bindings.size() > gameplay) {
        s->log(1, "bindings for %s with the input diagnostics not accepted (%d), retrying without",
               profile, int(result));
        suggested.countSuggestedBindings = uint32_t(gameplay);
        result = xrSuggestInteractionProfileBindings(s->instance, &suggested);
    }
    if (XR_FAILED(result)) {
        s->log(1, "bindings for %s not accepted (%d)", profile, int(result));
    }
}

void SuggestAll() {
    using S = State;
    Suggest("/interaction_profiles/oculus/touch_controller",
            {{&S::move, "/user/hand/left/input/thumbstick"},
             {&S::look, "/user/hand/right/input/thumbstick"},
             {&S::move_x, "/user/hand/left/input/thumbstick/x"},
             {&S::move_y, "/user/hand/left/input/thumbstick/y"},
             {&S::look_x, "/user/hand/right/input/thumbstick/x"},
             {&S::look_y, "/user/hand/right/input/thumbstick/y"},
             {&S::trigger, "/user/hand/left/input/trigger/value"},
             {&S::trigger, "/user/hand/right/input/trigger/value"},
             {&S::grip, "/user/hand/left/input/squeeze/value"},
             {&S::grip, "/user/hand/right/input/squeeze/value"},
             {&S::stick_click, "/user/hand/left/input/thumbstick/click"},
             {&S::stick_click, "/user/hand/right/input/thumbstick/click"},
             {&S::a, "/user/hand/right/input/a/click"},
             {&S::b, "/user/hand/right/input/b/click"},
             {&S::x, "/user/hand/left/input/x/click"},
             {&S::y, "/user/hand/left/input/y/click"},
             {&S::menu, "/user/hand/left/input/menu/click"},
             {&S::grip_pose, "/user/hand/left/input/grip/pose"},
             {&S::grip_pose, "/user/hand/right/input/grip/pose"},
             {&S::aim_pose, "/user/hand/left/input/aim/pose"},
             {&S::aim_pose, "/user/hand/right/input/aim/pose"},
             {&S::haptic, "/user/hand/left/output/haptic"},
             {&S::haptic, "/user/hand/right/output/haptic"}},
            kTouchPaths, std::size(kTouchPaths));
    Suggest("/interaction_profiles/valve/index_controller",
            {{&S::move, "/user/hand/left/input/thumbstick"},
             {&S::look, "/user/hand/right/input/thumbstick"},
             {&S::move_x, "/user/hand/left/input/thumbstick/x"},
             {&S::move_y, "/user/hand/left/input/thumbstick/y"},
             {&S::look_x, "/user/hand/right/input/thumbstick/x"},
             {&S::look_y, "/user/hand/right/input/thumbstick/y"},
             {&S::trigger, "/user/hand/left/input/trigger/value"},
             {&S::trigger, "/user/hand/right/input/trigger/value"},
             {&S::grip, "/user/hand/left/input/squeeze/value"},
             {&S::grip, "/user/hand/right/input/squeeze/value"},
             {&S::stick_click, "/user/hand/left/input/thumbstick/click"},
             {&S::stick_click, "/user/hand/right/input/thumbstick/click"},
             {&S::a, "/user/hand/right/input/a/click"},
             {&S::b, "/user/hand/right/input/b/click"},
             {&S::x, "/user/hand/left/input/a/click"},
             {&S::y, "/user/hand/left/input/b/click"},
             {&S::menu, "/user/hand/left/input/system/click"},
             {&S::grip_pose, "/user/hand/left/input/grip/pose"},
             {&S::grip_pose, "/user/hand/right/input/grip/pose"},
             {&S::aim_pose, "/user/hand/left/input/aim/pose"},
             {&S::aim_pose, "/user/hand/right/input/aim/pose"},
             {&S::haptic, "/user/hand/left/output/haptic"},
             {&S::haptic, "/user/hand/right/output/haptic"}},
            kIndexPaths, std::size(kIndexPaths));
    Suggest("/interaction_profiles/khr/simple_controller",
            {{&S::trigger, "/user/hand/left/input/select/click"},
             {&S::trigger, "/user/hand/right/input/select/click"},
             {&S::menu, "/user/hand/left/input/menu/click"},
             {&S::grip_pose, "/user/hand/left/input/grip/pose"},
             {&S::grip_pose, "/user/hand/right/input/grip/pose"},
             {&S::aim_pose, "/user/hand/left/input/aim/pose"},
             {&S::aim_pose, "/user/hand/right/input/aim/pose"},
             {&S::haptic, "/user/hand/left/output/haptic"},
             {&S::haptic, "/user/hand/right/output/haptic"}},
            kSimplePaths, std::size(kSimplePaths));
}

XrSpace ActionSpace(XrAction action, int hand) {
    XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    info.action = action;
    info.subactionPath = s->hands[hand];
    info.poseInActionSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE;
    Ok(xrCreateActionSpace(s->session, &info, &space), "xrCreateActionSpace");
    return space;
}

XrSpace ReferenceSpace(XrReferenceSpaceType type) {
    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.referenceSpaceType = type;
    info.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE;
    Ok(xrCreateReferenceSpace(s->session, &info, &space), "xrCreateReferenceSpace");
    return space;
}

float FloatState(XrAction action, int hand) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    info.subactionPath = hand >= 0 ? s->hands[hand] : XR_NULL_PATH;
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    xrGetActionStateFloat(s->session, &info, &state);
    return state.isActive ? state.currentState : 0.0f;
}

bool BoolState(XrAction action, int hand) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    info.subactionPath = hand >= 0 ? s->hands[hand] : XR_NULL_PATH;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    xrGetActionStateBoolean(s->session, &info, &state);
    return state.isActive && state.currentState;
}

XrVector2f StickState(XrAction action) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    xrGetActionStateVector2f(s->session, &info, &state);
    return state.isActive ? state.currentState : XrVector2f{0.0f, 0.0f};
}

/// The thumbstick: its vector2 binding, else its x/y float bindings; logs the first source seen.
XrVector2f StickOrAxes(XrAction vector, XrAction x, XrAction y, int hand) {
    XrVector2f v = StickState(vector);
    const char* source = "vector2";
    if (std::fabs(v.x) < 0.01f && std::fabs(v.y) < 0.01f) {
        v = {FloatState(x, -1), FloatState(y, -1)};
        source = "x/y axes";
    }
    if (!s->logged_stick[hand] && (std::fabs(v.x) > 0.3f || std::fabs(v.y) > 0.3f)) {
        s->logged_stick[hand] = true;
        s->log(0, "%s stick works (%s)", hand ? "right" : "left", source);
    }
    // Each axis is scaled by its own largest push: the Pimax Dream Air's sticks reach full range
    // sideways but only ~0.35 up/down, too little for the game's menus.
    float* const axes[2] = {&v.x, &v.y};
    for (int axis = 0; axis < 2; ++axis) {
        float& value = *axes[axis];
        float& peak = s->stick_peak[hand][axis];
        if (std::fabs(value) > peak + 0.05f) {
            peak = std::fabs(value);
            s->log(0, "%s stick %s reaches %.2f", hand ? "right" : "left", axis ? "y" : "x", peak);
        }
        if (peak > 0.2f && peak < 0.95f) {
            value = std::clamp(value / peak, -1.0f, 1.0f);
        }
    }
    return v;
}

bool Locate(XrSpace space, XrTime time, BbXrPose& pose, Vec3* velocity) {
    XrSpaceVelocity speed{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    location.next = velocity ? &speed : nullptr;
    pose.valid = 0;
    if (XR_FAILED(xrLocateSpace(space, s->local, time, &location))) {
        return false;
    }
    const auto needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((location.locationFlags & needed) != needed) {
        return false;
    }
    const auto& p = location.pose;
    pose = {{p.position.x, p.position.y, p.position.z},
            {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
            1};
    if (velocity) {
        *velocity = (speed.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
                        ? Vec3{speed.linearVelocity.x, speed.linearVelocity.y, speed.linearVelocity.z}
                        : Vec3{0.0f, 0.0f, 0.0f};
    }
    return true;
}

XrQuaternionf Quat(const BbXrPose& pose) {
    return {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
}

void Pulse(uint32_t button, Clock::time_point now, double ms = PulseMs) {
    for (int bit = 0; bit < 32; ++bit) {
        if (button & (1u << bit)) {
            s->pulse_until[bit] = now + std::chrono::microseconds(int64_t(ms * 1000.0));
        }
    }
}

void Fire(uint32_t gesture, uint32_t button, int hand, Clock::time_point now, float speed) {
    Pulse(button, now);
    {
        static const char* const names[] = {"swing", "heavy swing", "shoot thrust", "vial"};
        int kind = 0;
        while (kind < 3 && !((gesture >> kind) & 1u)) {
            ++kind;
        }
        const double ms = std::chrono::duration<double, std::milli>(now - s->start).count();
        if (s->gesture_log.Allow(kind, ms)) {
            s->log(0, "gesture %s fired (%s hand, %.1f m/s)", names[kind], hand ? "right" : "left",
                   speed);
        }
    }
    s->last_gesture[hand] = now;
    {
        std::scoped_lock lock{s->mutex};
        s->pending_gestures |= gesture;
    }
    // With the game UI (BB_VR_UI=1) its weak tick on the swing and its pulse on the hit are the
    // feedback (vr_game_ui.cpp); a strong pulse here would say "hit" for every swing.
    if (!s->ui) {
        Haptic(hand, gesture == BBXR_GESTURE_HEAVY ? 1.0f : 0.6f, 0.06f);
    }
}

std::string PathString(XrPath path) {
    char text[XR_MAX_PATH_LENGTH] = "";
    uint32_t length = 0;
    if (XR_FAILED(xrPathToString(s->instance, path, sizeof(text), &length, text))) {
        return "?";
    }
    return text;
}

/// The input paths the runtime bound to an action, space separated ("" when none); only those
/// under `hand_prefix` when given.
std::string BoundPaths(XrAction action, const char* hand_prefix = nullptr) {
    XrBoundSourcesForActionEnumerateInfo info{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
    info.action = action;
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateBoundSourcesForAction(s->session, &info, 0, &count, nullptr)) ||
        count == 0) {
        return "";
    }
    std::vector<XrPath> paths(count);
    if (XR_FAILED(xrEnumerateBoundSourcesForAction(s->session, &info, count, &count,
                                                   paths.data()))) {
        return "";
    }
    std::string out;
    for (uint32_t i = 0; i < count; ++i) {
        const std::string text = PathString(paths[i]);
        if (hand_prefix && text.rfind(hand_prefix, 0) != 0) {
            continue;
        }
        out += (out.empty() ? "" : " ") + text;
    }
    return out;
}

void LogProfile() {
    if (s->profile_logged) {
        return;
    }
    for (int hand = 0; hand < 2; ++hand) {
        XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(s->session, s->hands[hand], &profile)) &&
            profile.interactionProfile != XR_NULL_PATH) {
            s->log(0, "%s controller: %s", hand ? "right" : "left",
                   PathString(profile.interactionProfile).c_str());
            s->profile_logged = true;
        }
    }
    if (!s->profile_logged || s->bindings_logged) {
        return;
    }
    // What the runtime bound, once (bound sources are known after a sync with a profile).
    s->bindings_logged = true;
    for (const auto& w : s->watched) {
        const std::string bound = BoundPaths(s->*w.action);
        s->log(0, "input %s bound to: %s", w.name, bound.empty() ? "(nothing)" : bound.c_str());
    }
    if (BoundPaths(s->menu).empty()) {
        s->log(1, "menu button not reported by the runtime; Options = %s", OptionsChord);
    } else {
        s->log(0, "Options = the menu button, or %s where the runtime keeps that button for "
                  "itself (Pimax)",
               OptionsChord);
    }
    if (s->diag_set) {
        s->log(0, "input diagnostics on (BB_XR_INPUT_DIAG=1): %u input paths watched",
               unsigned(s->diag.size()));
    }
}

/// Whether an action reports anything for a hand (-1: no subaction path).
bool ActionUsed(XrAction action, XrActionType type, int hand, float* value) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    info.subactionPath = hand >= 0 ? s->hands[hand] : XR_NULL_PATH;
    switch (type) {
    case XR_ACTION_TYPE_BOOLEAN_INPUT: {
        XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
        xrGetActionStateBoolean(s->session, &info, &b);
        *value = b.currentState ? 1.0f : 0.0f;
        return b.isActive && b.currentState;
    }
    case XR_ACTION_TYPE_FLOAT_INPUT: {
        XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
        xrGetActionStateFloat(s->session, &info, &f);
        *value = f.currentState;
        return f.isActive && std::fabs(f.currentState) > 0.25f;
    }
    case XR_ACTION_TYPE_VECTOR2F_INPUT: {
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        xrGetActionStateVector2f(s->session, &info, &v);
        *value = std::max(std::fabs(v.currentState.x), std::fabs(v.currentState.y));
        return v.isActive && *value > 0.25f;
    }
    default:
        return false;
    }
}

/// Logs each gameplay action's first use per hand (with its bound path) and, with
/// BB_XR_INPUT_DIAG, each input path's first report.
void LogFirstUse() {
    for (auto& w : s->watched) {
        for (int hand = 0; hand < (w.per_hand ? 2 : 1); ++hand) {
            float value = 0.0f;
            if (w.seen[hand] ||
                !ActionUsed(s->*w.action, w.type, w.per_hand ? hand : -1, &value)) {
                continue;
            }
            w.seen[hand] = true;
            const char* prefix = w.per_hand ? (hand ? "/user/hand/right" : "/user/hand/left")
                                            : nullptr;
            const std::string bound = BoundPaths(s->*w.action, prefix);
            s->log(0, "input first used: %s%s %.2f (%s)", w.name,
                   w.per_hand ? (hand ? " right" : " left") : "", value,
                   bound.empty() ? "no bound path reported" : bound.c_str());
        }
    }
    for (auto& d : s->diag) {
        float value = 0.0f;
        if (d.seen || !ActionUsed(d.action,
                                  d.is_float ? XR_ACTION_TYPE_FLOAT_INPUT
                                             : XR_ACTION_TYPE_BOOLEAN_INPUT,
                                  -1, &value)) {
            continue;
        }
        d.seen = true;
        s->log(0, "input path first reports: %s (%.2f)", d.path.c_str(), value);
    }
}

} // namespace

bool Create(XrInstance instance, XrSession session, LogFn log) {
    s = new State;
    s->log = log;
    s->instance = instance;
    s->session = session;
    const char* gestures = std::getenv("BB_XR_GESTURES");
    s->gestures = !(gestures && gestures[0] == '0');
    // BB_XR_TRIGGERS=switch (default with gestures: the swing attacks, the triggers swap the
    // right/left weapon) or attack (right trigger R1, left trigger L2, as without gestures).
    const char* triggers = std::getenv("BB_XR_TRIGGERS");
    s->trigger_switch = triggers ? std::strcmp(triggers, "attack") != 0 : s->gestures;
    s->left_gun = !(triggers && std::strcmp(triggers, "switch") == 0);
    const char* chords = std::getenv("BB_XR_CHORDS");
    s->chords = chords && chords[0] == '1';
    s->layer.chords = s->chords;
    const char* ui = std::getenv("BB_VR_UI");
    s->ui = ui && ui[0] == '1';
    s->hands[0] = Path("/user/hand/left");
    s->hands[1] = Path("/user/hand/right");

    XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(set.actionSetName, "gameplay");
    std::strcpy(set.localizedActionSetName, "Gameplay");
    if (!Ok(xrCreateActionSet(instance, &set, &s->set), "xrCreateActionSet")) {
        return false;
    }
    const char* diag = std::getenv("BB_XR_INPUT_DIAG");
    if (diag && diag[0] == '1') {
        XrActionSetCreateInfo diag_info{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(diag_info.actionSetName, "diagnostics");
        std::strcpy(diag_info.localizedActionSetName, "Input diagnostics");
        if (!Ok(xrCreateActionSet(instance, &diag_info, &s->diag_set), "xrCreateActionSet")) {
            s->diag_set = XR_NULL_HANDLE;
        }
    }
    s->move = MakeAction("move", XR_ACTION_TYPE_VECTOR2F_INPUT, false);
    s->look = MakeAction("look", XR_ACTION_TYPE_VECTOR2F_INPUT, false);
    s->move_x = MakeAction("move_x", XR_ACTION_TYPE_FLOAT_INPUT, false);
    s->move_y = MakeAction("move_y", XR_ACTION_TYPE_FLOAT_INPUT, false);
    s->look_x = MakeAction("look_x", XR_ACTION_TYPE_FLOAT_INPUT, false);
    s->look_y = MakeAction("look_y", XR_ACTION_TYPE_FLOAT_INPUT, false);
    s->trigger = MakeAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
    s->grip = MakeAction("grip", XR_ACTION_TYPE_FLOAT_INPUT, true);
    s->stick_click = MakeAction("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    s->a = MakeAction("button_a", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    s->b = MakeAction("button_b", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    s->x = MakeAction("button_x", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    s->y = MakeAction("button_y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    s->menu = MakeAction("menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    s->grip_pose = MakeAction("grip_pose", XR_ACTION_TYPE_POSE_INPUT, true);
    s->aim_pose = MakeAction("aim_pose", XR_ACTION_TYPE_POSE_INPUT, true);
    s->haptic = MakeAction("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
    using S = State;
    constexpr XrActionType kBool = XR_ACTION_TYPE_BOOLEAN_INPUT;
    constexpr XrActionType kFloat = XR_ACTION_TYPE_FLOAT_INPUT;
    s->watched = {{&S::move, "move (left stick)", XR_ACTION_TYPE_VECTOR2F_INPUT, false, {}},
                  {&S::look, "look (right stick)", XR_ACTION_TYPE_VECTOR2F_INPUT, false, {}},
                  {&S::move_x, "move_x", kFloat, false, {}},
                  {&S::move_y, "move_y", kFloat, false, {}},
                  {&S::look_x, "look_x", kFloat, false, {}},
                  {&S::look_y, "look_y", kFloat, false, {}},
                  {&S::trigger, "trigger", kFloat, true, {}},
                  {&S::grip, "grip", kFloat, true, {}},
                  {&S::stick_click, "stick_click", kBool, true, {}},
                  {&S::a, "button_a", kBool, false, {}},
                  {&S::b, "button_b", kBool, false, {}},
                  {&S::x, "button_x", kBool, false, {}},
                  {&S::y, "button_y", kBool, false, {}},
                  {&S::menu, "menu", kBool, false, {}}};
    SuggestAll();

    // Both sets in the one attach call a session allows; without the diagnostics if refused.
    XrActionSet sets[2] = {s->set, s->diag_set};
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = s->diag_set ? 2 : 1;
    attach.actionSets = sets;
    XrResult attached = xrAttachSessionActionSets(session, &attach);
    if (XR_FAILED(attached) && s->diag_set) {
        s->log(1, "input diagnostics could not be attached (%d): off", int(attached));
        s->diag.clear();
        xrDestroyActionSet(s->diag_set);
        s->diag_set = XR_NULL_HANDLE;
        attach.countActionSets = 1;
        attached = xrAttachSessionActionSets(session, &attach);
    }
    if (!Ok(attached, "xrAttachSessionActionSets")) {
        return false;
    }
    for (int hand = 0; hand < 2; ++hand) {
        s->grip_space[hand] = ActionSpace(s->grip_pose, hand);
        s->aim_space[hand] = ActionSpace(s->aim_pose, hand);
    }
    s->local = ReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL);
    s->view = ReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW);
    s->log(0, "controllers ready (gestures %s, triggers %s, %s)", s->gestures ? "on" : "off",
           !s->trigger_switch ? "attack"
           : s->left_gun      ? "left shoots, right swaps the right weapon"
                              : "swap weapons",
           s->chords ? "L3 chords on (BB_XR_CHORDS=1)"
                     : "one button one action (the dashboard has the d-pad, options, gestures)");
    return true;
}

void Update(XrTime time, bool focused) {
    if (!s) {
        return;
    }
    BbXrInput in{};
    in.lx = in.ly = in.rx = in.ry = 128;
    if (!focused) {
        std::scoped_lock lock{s->mutex};
        s->latest = in;
        return;
    }
    const XrActiveActionSet active[2] = {{s->set, XR_NULL_PATH}, {s->diag_set, XR_NULL_PATH}};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = s->diag_set ? 2 : 1;
    sync.activeActionSets = active;
    if (XR_FAILED(xrSyncActions(s->session, &sync))) {
        return;
    }
    LogProfile();
    LogFirstUse();
    const auto now = Clock::now();

    Vec3 velocity[2]{};
    Vec3 head_velocity{};
    Locate(s->view, time, in.head, &head_velocity);
    for (int hand = 0; hand < 2; ++hand) {
        Locate(s->grip_space[hand], time, in.grip[hand], &velocity[hand]);
        Locate(s->aim_space[hand], time, in.aim[hand], nullptr);
        in.hand_speed[hand] = Length(velocity[hand]);
    }
    in.active = in.grip[0].valid || in.grip[1].valid;

    // Buttons.
    const XrVector2f move = StickOrAxes(s->move, s->move_x, s->move_y, 0);
    XrVector2f look = StickOrAxes(s->look, s->look_x, s->look_y, 1);
    const float trigger[2] = {FloatState(s->trigger, 0), FloatState(s->trigger, 1)};
    const float grip[2] = {FloatState(s->grip, 0), FloatState(s->grip, 1)};
    for (int hand = 0; hand < 2; ++hand) {
        s->analog_trigger[hand] = trigger[hand];
        s->analog_grip[hand] = grip[hand];
    }
    const bool l3 = BoolState(s->stick_click, 0);
    // B and A sit under the right thumb, so they chord with L3 (left thumb): see the L3 layer.
    const bool b_button = BoolState(s->b, -1);
    const bool a_button = BoolState(s->a, -1);
    s->menu_raw = BoolState(s->menu, -1);
    uint32_t buttons = 0;
    if (s->trigger_switch) {
        // Edge triggered: one d-pad pulse per pull, not a held d-pad.
        const uint32_t pad[2] = {BBXR_BTN_LEFT, BBXR_BTN_RIGHT};
        if (s->left_gun && trigger[0] > 0.5f) buttons |= BBXR_BTN_L2;
        for (int hand = s->left_gun ? 1 : 0; hand < 2; ++hand) {
            if (s->trigger_edge[hand].Update(trigger[hand])) {
                Pulse(pad[hand], now, TriggerPulseMs);
                Haptic(hand, 0.3f, 0.04f);
                const double ms = std::chrono::duration<double, std::milli>(now - s->start).count();
                if (s->trigger_log.Allow(hand, ms)) {
                    s->log(0, "%s trigger pulled: d-pad %s pulse %.0f ms (swap the %s weapon)",
                           hand ? "right" : "left", hand ? "right" : "left", TriggerPulseMs,
                           hand ? "right" : "left");
                }
            }
        }
    } else {
        if (trigger[1] > 0.5f) buttons |= BBXR_BTN_R1;
        if (trigger[0] > 0.5f) buttons |= BBXR_BTN_L2;
    }
    {
        const bool squeezed = s->l1_down ? grip[0] > 0.35f : grip[0] > 0.6f;
        if (squeezed && !s->l1_down) {
            Pulse(BBXR_BTN_L1, now);
        }
        s->l1_down = squeezed;
    }
    if (grip[1] > 0.6f && !s->gestures) buttons |= BBXR_BTN_R2;
    if (BoolState(s->y, -1)) buttons |= BBXR_BTN_TRIANGLE;
    const bool x_button = BoolState(s->x, -1);
    if (x_button) buttons |= BBXR_BTN_SQUARE;
    if (x_button && !s->x_prev) {
        Haptic(0, 0.15f, 0.03f);
    }
    s->x_prev = x_button;
    // The menu button: Options on release when pressed < 1 s; held 1 s it is the bridge's
    // recenter (MenuButtonDown) and Options is not sent.
    {
        const double ms = std::chrono::duration<double, std::milli>(now - s->start).count();
        if (s->menu_tap.Update(ms, s->menu_raw)) {
            Pulse(BBXR_BTN_OPTIONS, now);
        }
    }
    if (BoolState(s->stick_click, 1)) buttons |= BBXR_BTN_R3;

    // The L3 layer (see the mapping at the top and xr_input_logic.h): the d-pad, options and
    // touchpad chords, the recenter hold, L3 on release.
    const double now_ms = std::chrono::duration<double, std::milli>(now - s->start).count();
    const xrpad::L3Layer::Out layer =
        s->layer.Update(now_ms, l3, b_button, a_button, look.x, look.y,
                        std::sqrt(move.x * move.x + move.y * move.y) > 0.3f);
    buttons |= layer.buttons;
    if (layer.pulses) {
        Pulse(layer.pulses, now); // a quick tap still lasts long enough for the game
    }
    if (layer.zero_look) {
        look = {0.0f, 0.0f};
    }
    if (layer.recenter) {
        s->recenter = true;
        s->log(0, "left stick click held %.1f s alone: recenter (no L3 to the game)",
               layer.held_ms / 1000.0);
        Haptic(0, 0.5f, 0.05f);
    }

    // Gestures.
    bool swing_in_progress = false;
    if (s->gestures) {
        // Head yaw guard for the gun thrust (snap turn, recenter).
        bool turning = false;
        if (in.head.valid) {
            turning = s->turn.Blocked(
                now_ms, xrpad::TurnGuard::Yaw(in.head.orientation[0], in.head.orientation[1],
                                              in.head.orientation[2], in.head.orientation[3]),
                layer.recenter);
        }
        if (in.active && in.grip[1].valid) {
            const float speed = in.hand_speed[1];
            const Vec3 v = velocity[1];
            bool away = true;
            if (in.head.valid) {
                away = xrpad::SwingAwayFromHead(
                    {v.x, v.y, v.z},
                    {in.grip[1].position[0], in.grip[1].position[1], in.grip[1].position[2]},
                    {in.head.position[0], in.head.position[1], in.head.position[2]});
            }
            if (s->swing.Update(speed, away, l3)) {
                const bool heavy = grip[1] > 0.6f || speed > HeavySpeed;
                Fire(heavy ? BBXR_GESTURE_HEAVY : BBXR_GESTURE_SWING,
                     heavy ? BBXR_BTN_R2 : BBXR_BTN_R1, 1, now, speed);
                if (heavy) {
                    s->charge.OnHeavy(now_ms);
                }
            }
        }
        if (s->charge.Update(now_ms, grip[1])) {
            buttons |= BBXR_BTN_R2; // charged attack: R2 stays down while the grip is held
        }
        swing_in_progress = !s->swing.armed || s->charge.active || s->charge.pending;
        if (in.active && in.aim[0].valid && in.grip[0].valid) {
            const Vec3 forward = Rotate(Quat(in.aim[0]), {0.0f, 0.0f, -1.0f});
            // The hand's speed relative to the head: walking and turning are not a thrust.
            const Vec3 rel{velocity[0].x - head_velocity.x, velocity[0].y - head_velocity.y,
                           velocity[0].z - head_velocity.z};
            const float thrust = rel.x * forward.x + rel.y * forward.y + rel.z * forward.z;
            if (turning) {
                s->shoot_armed = false;
            } else if (s->shoot_armed && thrust > ShootSpeed) {
                Fire(BBXR_GESTURE_SHOOT, BBXR_BTN_L2, 0, now, thrust);
                s->shoot_armed = false;
            } else if (thrust < SwingRearm * 0.5f) {
                s->shoot_armed = true;
            }
        }
        if (in.active && in.head.valid && in.grip[0].valid) {
            const Vec3 offset = Rotate(Quat(in.head), {0.0f, -0.08f, -0.06f});
            const Vec3 mouth_pos{in.head.position[0] + offset.x, in.head.position[1] + offset.y,
                                 in.head.position[2] + offset.z};
            const float distance =
                Length({in.grip[0].position[0] - mouth_pos.x, in.grip[0].position[1] - mouth_pos.y,
                        in.grip[0].position[2] - mouth_pos.z});
            if (s->mouth.Update(now_ms, distance, grip[0])) {
                Fire(BBXR_GESTURE_VIAL, BBXR_BTN_TRIANGLE, 0, now, 0.0f);
            }
        }
        // Right grip + right stick flick = d-pad (quick items while walking): BB_XR_CHORDS=1 only.
        if (s->chords) {
            const xrpad::GripFlick::Out flick =
                xrpad::GripFlick::Update(grip[1], swing_in_progress, l3, look.x, look.y);
            buttons |= flick.buttons;
            if (flick.zero_look) {
                look = {0.0f, 0.0f};
            }
        }
    }
    for (int bit = 0; bit < 32; ++bit) {
        if (now < s->pulse_until[bit]) {
            buttons |= 1u << bit;
        }
    }

    in.buttons = buttons;
    in.reserved[0] = BoolState(s->y, -1) ? BBXR_RAW_Y : 0; // the button, not the vial gesture
    in.lx = Axis(move.x);
    in.ly = Axis(-move.y);
    in.rx = Axis(look.x);
    in.ry = Axis(-look.y);
    // The left trigger's analog value is L2 only when it attacks; when it swaps weapons a pulled
    // trigger must not also read as an L2 (the gun) through the analog axis.
    in.l2 = (buttons & BBXR_BTN_L2)                 ? 255
            : s->trigger_switch && !s->left_gun ? 0
                                                : Trigger(trigger[0]);
    in.r2 = (buttons & BBXR_BTN_R2) ? 255 : 0;
    std::scoped_lock lock{s->mutex};
    s->latest = in;
}

int Get(BbXrInput* out) {
    if (!s) {
        *out = {};
        return 0;
    }
    std::scoped_lock lock{s->mutex};
    *out = s->latest;
    out->gestures = s->pending_gestures;
    s->pending_gestures = 0;
    return int(out->active);
}

BbXrInput Peek() {
    if (!s) {
        return {};
    }
    std::scoped_lock lock{s->mutex};
    return s->latest;
}

bool TakeRecenter() {
    return s && s->recenter.exchange(false);
}

void RequestRecenter() {
    if (s) {
        s->recenter = true;
    }
}

void LocateHands(XrTime time, XrSpace base, BbXrPose grip[2], BbXrPose aim[2]) {
    constexpr XrSpaceLocationFlags needed =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    for (int hand = 0; hand < 2; ++hand) {
        grip[hand] = aim[hand] = BbXrPose{};
        if (!s) {
            continue;
        }
        const XrSpace spaces[2] = {s->grip_space[hand], s->aim_space[hand]};
        BbXrPose* const out[2] = {&grip[hand], &aim[hand]};
        for (int k = 0; k < 2; ++k) {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (spaces[k] && XR_SUCCEEDED(xrLocateSpace(spaces[k], base, time, &location)) &&
                (location.locationFlags & needed) == needed) {
                const auto& p = location.pose;
                *out[k] = {{p.position.x, p.position.y, p.position.z},
                           {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
                           1};
            }
        }
    }
}

void Analog(float trigger[2], float grip[2]) {
    for (int hand = 0; hand < 2; ++hand) {
        trigger[hand] = s ? s->analog_trigger[hand] : 0.0f;
        grip[hand] = s ? s->analog_grip[hand] : 0.0f;
    }
}

bool MenuButtonDown() {
    return s && s->menu_raw;
}

double MsSinceGesture(int hand) {
    if (!s || s->last_gesture[hand] == Clock::time_point{}) {
        return 1e9;
    }
    return std::chrono::duration<double, std::milli>(Clock::now() - s->last_gesture[hand]).count();
}

void Haptic(int hand, float amplitude, float seconds) {
    if (!s) {
        return;
    }
    for (int h = 0; h < 2; ++h) {
        if (hand != 2 && hand != h) {
            continue;
        }
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = s->haptic;
        info.subactionPath = s->hands[h];
        XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
        vibration.amplitude = amplitude;
        vibration.duration = XrDuration(seconds * 1e9f);
        vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
        xrApplyHapticFeedback(s->session, &info,
                              reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
    }
}

void Destroy() {
    if (!s) {
        return;
    }
    for (XrSpace space : {s->grip_space[0], s->grip_space[1], s->aim_space[0], s->aim_space[1],
                          s->local, s->view}) {
        if (space) {
            xrDestroySpace(space);
        }
    }
    if (s->set) {
        xrDestroyActionSet(s->set);
    }
    if (s->diag_set) {
        xrDestroyActionSet(s->diag_set);
    }
    delete s;
    s = nullptr;
}

} // namespace XrInput
