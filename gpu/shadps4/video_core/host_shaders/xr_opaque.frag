// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

// bbport VR: drawn with only alpha writes enabled, so the headset screen is opaque (the game
// leaves alpha at 0 and some OpenXR runtimes blend quad layers by it regardless of layer flags).
layout(location = 0) out vec4 color;

void main() {
    color = vec4(0.0, 0.0, 0.0, 1.0);
}
