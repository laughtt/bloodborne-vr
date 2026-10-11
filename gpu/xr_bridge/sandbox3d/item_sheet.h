// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// The item texture sheet (scripts/vr_assets/item_tex.py: <assets>/items/tex_sheet.png + .json): the
// albedo textures of the held-item meshes. UiAtlas::Load appends the sheet's rows below the icon atlas
// (<assets>/ui/icons.png), so the held meshes are drawn by the UI pipeline's icon mode with no new
// descriptor; this header is the one place that knows where the sheet lands, so the atlas and the held
// mesh cache (vr_held_mesh.h) agree. Header only, no GPU.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <filesystem>
#include <map>
#include <string>

#include "json.h"
#include "model.h"

namespace sb {

struct ItemSheet {
    uint32_t icons_w = 0, icons_h = 0; // the icon atlas (icons.png) the sheet is appended to
    uint32_t sheet_w = 0, sheet_h = 0;
    std::map<std::string, std::array<int, 4>> rects; // key -> x, y, w, h in the sheet's pixels
    // The menu icons (ui/icons.json) for cards: "goods:<id>" keys resolve to the item's icon cell.
    int cell = 128;
    std::map<int, std::pair<int, int>> icon_xy;  // icon id -> x, y in the icon atlas
    std::map<int, int> goods_icon;               // goods id -> icon id

    /// The sizes of a PNG from its IHDR; false when it is missing.
    static bool PngSize(const std::filesystem::path& file, uint32_t& w, uint32_t& h) {
        std::FILE* f = std::fopen(file.string().c_str(), "rb");
        if (!f) {
            return false;
        }
        unsigned char b[24] = {};
        const size_t n = std::fread(b, 1, sizeof b, f);
        std::fclose(f);
        if (n < 24 || b[1] != 'P') {
            return false;
        }
        w = (uint32_t(b[16]) << 24) | (uint32_t(b[17]) << 16) | (uint32_t(b[18]) << 8) | b[19];
        h = (uint32_t(b[20]) << 24) | (uint32_t(b[21]) << 16) | (uint32_t(b[22]) << 8) | b[23];
        return true;
    }

    /// <assets>/items/tex_sheet.json and ui/icons.png sizes; false (and empty) when there is no sheet.
    bool Load(const std::filesystem::path& assets) {
        rects.clear();
        sheet_w = sheet_h = 0;
        LoadIcons(assets);
        bool ok = false;
        const std::string text = ReadText(assets / "items" / "tex_sheet.json", &ok);
        if (!ok || !PngSize(assets / "ui" / "icons.png", icons_w, icons_h)) {
            return false;
        }
        std::string error;
        const json::Value doc = json::Parse(text, &error);
        if (!error.empty() || !doc["textures"].IsObject()) {
            return false;
        }
        sheet_w = uint32_t(doc["width"].Int());
        sheet_h = uint32_t(doc["height"].Int());
        for (const auto& [key, r] : doc["textures"].object) {
            rects[key] = {r[0].Int(), r[1].Int(), r[2].Int(), r[3].Int()};
        }
        return sheet_w > 0 && sheet_h > 0 && !rects.empty();
    }

    /// ui/icons.json: the icon cells and the goods -> icon table (the paper cards). Independent of
    /// the sheet (cards work without extracted textures); false when there is no icon atlas.
    bool LoadIcons(const std::filesystem::path& assets) {
        icon_xy.clear();
        goods_icon.clear();
        if (!PngSize(assets / "ui" / "icons.png", icons_w, icons_h)) {
            return false;
        }
        bool ok = false;
        const std::string text = ReadText(assets / "ui" / "icons.json", &ok);
        if (!ok) {
            return false;
        }
        std::string error;
        const json::Value doc = json::Parse(text, &error);
        if (!error.empty()) {
            return false;
        }
        cell = doc["cell"].Int(128);
        if (doc["icons"].IsObject()) {
            for (const auto& [key, xy] : doc["icons"].object) {
                icon_xy[std::atoi(key.c_str())] = {xy[0].Int(), xy[1].Int()};
            }
        }
        for (const json::Value& v : doc["items"].array) {
            if (v["kind"].Str() == "goods" && v["icon"].Int(-1) >= 0) {
                goods_icon[v["id"].Int()] = v["icon"].Int();
            }
        }
        return !icon_xy.empty();
    }

    uint32_t Width() const { return std::max(icons_w, sheet_w); }
    uint32_t Height() const { return icons_h + sheet_h; }

    /// The atlas uv of the sheet position (fu, fv) in 0..1 of texture `key`; false for an unknown key.
    bool Uv(const std::string& key, float fu, float fv, float out[2]) const {
        if (key.rfind("goods:", 0) == 0) { // a menu icon cell, as a card face
            const auto g = goods_icon.find(std::atoi(key.c_str() + 6));
            const auto i = g == goods_icon.end() ? icon_xy.end() : icon_xy.find(g->second);
            if (i == icon_xy.end() || !Height()) {
                return false;
            }
            out[0] = (float(i->second.first) + std::clamp(fu * float(cell), 0.5f, float(cell) - 0.5f)) / float(Width());
            out[1] = (float(i->second.second) + std::clamp(fv * float(cell), 0.5f, float(cell) - 0.5f)) / float(Height());
            return true;
        }
        const auto it = rects.find(key);
        if (it == rects.end()) {
            return false;
        }
        const auto& r = it->second;
        // Half a texel in, so bilinear filtering stays inside the picture.
        const float px = std::clamp(fu * float(r[2]), 0.5f, float(r[2]) - 0.5f);
        const float py = std::clamp(fv * float(r[3]), 0.5f, float(r[3]) - 0.5f);
        out[0] = (float(r[0]) + px) / float(Width());
        out[1] = (float(icons_h) + float(r[1]) + py) / float(Height());
        return true;
    }
};

} // namespace sb
