// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Offline previews of the comfort dash (sandbox3d --offline): the moves played the way the game
// would (the animation's eye path sampled at the game's frame rate) and the camera at 72 Hz, raw
// against the dash, plotted to <out>/dash_*.png with the live "locomotion" settings of
// sandbox3d.json. No GPU needed.

#pragma once

#include <filesystem>
#include <string>

namespace sb {

/// Writes dash_roll.png, dash_backstep.png, dash_jump.png, dash_cases.png, dash_curves.png into
/// `out_dir`; `config_path` is sandbox3d.json (its "locomotion" object), `assets` the folder with
/// dash/dodges.json. Returns false when a file cannot be written; `log` gets one line per image.
bool WriteDashPreviews(const std::filesystem::path& out_dir, const std::filesystem::path& assets,
                       const std::filesystem::path& config_path, std::string& log);

} // namespace sb
