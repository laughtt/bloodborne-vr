// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The "hands" object of sandbox3d.json (custom_hands.h): {"model": "custom" | "current", "scale",
// "position", "rotation_deg", "blend_rate"}. Header only, no GPU types (config.h includes it).

#pragma once

#include <algorithm>
#include <cstdio>
#include <string>

#include "json.h"
#include "math3d.h"

namespace sb {

struct HandsConfig {
    std::string model = "custom"; // "custom": the generic hand; "current": the hunter's own hands
    float scale = 1.0f;
    m3::Vec3 position, rotation_deg; // extra offset in the grip frame (the left hand mirrors it)
    float blend_rate = 18.0f;        // finger pose smoothing, 1/s
    bool Custom() const { return model != "current"; }
    inline void Load(const json::Value& d);
    inline std::string Summary() const;
};

inline void HandsConfig::Load(const json::Value& d) {
    if (!d.IsObject()) {
        return;
    }
    if (d["model"].IsString()) {
        model = d["model"].Str();
    }
    if (d["scale"].IsNumber()) {
        scale = std::clamp(d["scale"].Float(), 0.5f, 2.0f);
    }
    const auto vec = [](const json::Value& v, m3::Vec3& out) {
        if (v.IsArray() && v.Size() >= 3) {
            out = {v[0].Float(), v[1].Float(), v[2].Float()};
        }
    };
    vec(d["position"], position);
    vec(d["rotation_deg"], rotation_deg);
    if (d["blend_rate"].IsNumber()) {
        blend_rate = std::clamp(d["blend_rate"].Float(), 1.0f, 200.0f);
    }
}

inline std::string HandsConfig::Summary() const {
    char text[320];
    std::snprintf(text, sizeof(text),
                  "{\"model\": \"%s\", \"scale\": %g, \"position\": [%g, %g, %g], \"rotation_deg\": "
                  "[%g, %g, %g], \"blend_rate\": %g}",
                  model.c_str(), scale, position.x, position.y, position.z, rotation_deg.x,
                  rotation_deg.y, rotation_deg.z, blend_rate);
    return text;
}


} // namespace sb
