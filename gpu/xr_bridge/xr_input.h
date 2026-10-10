// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Controllers for bbport_xr.dll: OpenXR actions mapped to the PS4 pad, motion gestures that press
// the game's own buttons (so its animations, stamina and poise stay in charge), haptics.

#pragma once

#include <openxr/openxr.h>

#include "bbport_xr_bridge.h"

namespace XrInput {

using LogFn = void (*)(int level, const char* format, ...);

/// After xrCreateSession: actions, suggested bindings, action spaces.
bool Create(XrInstance instance, XrSession session, LogFn log);
/// Frame loop thread, each frame while the session runs: sync, locate, gestures.
void Update(XrTime time, bool focused);
/// Any thread.
int Get(BbXrInput* out);
void Haptic(int hand, float amplitude, float seconds);
/// The latest state without consuming gesture bits (hand markers).
BbXrInput Peek();
/// Milliseconds since a gesture last fired on the hand (large when never).
double MsSinceGesture(int hand);
/// Frame loop thread, after Update: true once per left stick click held 1 s alone (screen
/// recenter; that click never reaches the game as L3).
bool TakeRecenter();
/// Any thread: recenter at the next frame (the game UI's dashboard button).
void RequestRecenter();
/// The runtime's own menu button (not the L3 + B chord that also gives Options): the bridge's
/// secondary recenter (menu held 1 s). Pimax keeps that button for its system menu.
bool MenuButtonDown();
/// Frame loop thread: the controllers' grip and aim poses at `time` in the space `base`
/// (AcquireStereo hands them out with the views of the same display time); valid 0 when untracked.
void LocateHands(XrTime time, XrSpace base, BbXrPose grip[2], BbXrPose aim[2]);
/// Frame loop thread: the latest analog trigger and grip (squeeze) values per hand, 0..1.
void Analog(float trigger[2], float grip[2]);
void Destroy();

} // namespace XrInput
