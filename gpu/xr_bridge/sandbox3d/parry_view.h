// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The parry trainer's drawing (parry.h): the dummy's hurtboxes tinted by phase, the weapon it raises
// and swings, the closing telegraph ring over its head, the score digits beside it, the bullets'
// tracers and the muzzle flash. Lines only (depth-tested for the body, overlay for the cues).

#pragma once

#include "math3d.h"
#include "parry.h"
#include "renderer.h"

namespace sb {

void DrawParry(const ParryTrainer& trainer, const ParryConfig& config, double now, m3::Vec3 head,
               FrameData& out);

} // namespace sb
