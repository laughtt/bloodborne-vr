// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Draws what physics_weapon.h simulates: the props (training post with crossbar, wall), the
// enemy model at its bind pose, collider wireframes (weapon capsules green, orange where they
// touch; the enemy's hurtboxes, red while hit), a faint ghost of the real controller hand when a
// blocked weapon holds the hand back, and fading hit markers.

#pragma once

#include <string>

#include "physics_weapon.h"
#include "renderer.h"

namespace sb {

class PhysicsView {
public:
    /// Uploads the prop meshes and the enemy model.
    bool Upload(const WeaponPhysics& physics, Renderer& renderer, std::string& log);
    /// Uploads the enemy again after the physics loaded another one (config change).
    void Sync(const WeaponPhysics& physics, Renderer& renderer);
    void Draw(const WeaponPhysics& physics, const PhysicsConfig& config, FrameData& out) const;

private:
    int cylinder = -1, box = -1;
    uint32_t cylinder_indices = 0, box_indices = 0;
    int enemy_gpu = -1, enemy_generation = -1;
};

} // namespace sb
