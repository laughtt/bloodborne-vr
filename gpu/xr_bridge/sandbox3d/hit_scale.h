// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Tunable weapon hit shapes (hit radius x scale, extra reach along the blade) for the sandbox's
// physics weapon and its wireframe. The math is the game's: src/vr_hit_scale.h (the "VR Hitbox
// Scale" hook, docs/vr/findings/32-vr-hitbox-scale.md); this is only the glue to HitShape.

#pragma once

#include <algorithm>
#include <vector>

#include "../../../src/vr_hit_scale.h"
#include "model.h"

namespace sb {

inline VrVec3 ToVr(const m3::Vec3& v) { return VrVec3{v.x, v.y, v.z}; }
inline m3::Vec3 FromVr(const VrVec3& v) { return m3::Vec3{v.x, v.y, v.z}; }

/// How far the weapon's own (non-body) shapes reach from the grip (the weapon model's origin), over
/// every attack: the weapon's class and the "tip" shapes follow from it.
inline float HitWeaponReach(const Hitboxes& hb) {
    float reach = 0.0f;
    for (const Attack& a : hb.attacks) {
        for (const HitShape& h : a.hits) {
            if (h.p[0].on_body || h.p[1].on_body) {
                continue;
            }
            reach = std::max(reach, vhs_far_end(ToVr(h.p[0].pos), ToVr(h.p[1].pos), VrVec3{0, 0, 0}));
        }
    }
    return reach;
}

/// One shape scaled (weapon model space, grip at the origin). Body-sourced shapes (kicks) only
/// get the radius.
inline HitShape ScaleHitShape(const VhsParams& params, const HitShape& in, float weapon_reach) {
    HitShape out = in;
    const bool on_body = in.p[0].on_body || in.p[1].on_body;
    const VhsShape s = vhs_apply(&params, ToVr(in.p[0].pos), ToVr(in.p[1].pos), VrVec3{0, 0, 0},
                                 in.radius, weapon_reach, on_body ? 0 : 1);
    out.radius = s.r;
    if (!on_body) {
        out.p[0].pos = FromVr(s.a);
        out.p[1].pos = FromVr(s.b);
        // A sphere that gained reach is a capsule now.
        if (!in.capsule && (Length(FromVr(s.b) - FromVr(s.a)) > 1e-5f)) {
            out.capsule = true;
        }
    }
    return out;
}

inline std::vector<HitShape> ScaleHitShapes(const VhsParams& params, const std::vector<HitShape>& in,
                                            float weapon_reach) {
    std::vector<HitShape> out;
    out.reserve(in.size());
    for (const HitShape& h : in) {
        out.push_back(ScaleHitShape(params, h, weapon_reach));
    }
    return out;
}

} // namespace sb
