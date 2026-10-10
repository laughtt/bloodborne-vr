// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// --offline collision previews: scripted swings at 72 Hz through the scene (physics_weapon.h)
// rendered to <out>/collision_*.png (left: first person, right: third person), with the contact
// log in <out>/collision_contacts.jsonl.

#pragma once

#include <cstdint>
#include <filesystem>

namespace sb {

class Scene;
class Renderer;
struct Config;

/// Returns 0 when every preview was written.
int RenderCollisionPreviews(Scene& scene, Renderer& renderer, const Config& config,
                            const std::filesystem::path& out_dir, uint32_t size);

} // namespace sb
