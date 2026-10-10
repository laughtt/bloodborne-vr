// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_sandbox3d_ui_test.exe [out/vr_assets]: offline tests of the VR menu prototypes (no
// headset, no GPU): the PNG reader, the layout file and bindings, and the menus driven by
// scripted input (radial by stick and by hand movement, weapon wheel, grid by pointing, wrist
// glance, body slots standing and seated, button filtering, preset cycling, hot reload, the
// interaction log). With out/vr_assets/ui (scripts/vr_assets/ui_extract.py) the real item table
// and font are used too. Exit code 0 when every check passes.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "bbport_xr_bridge.h"
#include "image_out.h"
#include "png_read.h"
#include "ui.h"
#include "ui_layout.h"

using namespace m3;
namespace fs = std::filesystem;

namespace {

int failures = 0, checks = 0;

void Check(bool ok, const char* what, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

// Test images written by Pillow (adaptive row filters; fixed and dynamic Huffman blocks).
// kPngRgba: 23x9 RGBA, pixel (x, y) = (16x, 32y, 8(x+y), 255-4x) mod 256.
// kPngGray: 23x9 gray, (11x + 7y) mod 256. kPngDynamic: 64x32 gray, ((xy) mod 7) * 30 + (x^y)&3.
const uint8_t kPngRgba[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x17,
    0x00,0x00,0x00,0x09,0x08,0x06,0x00,0x00,0x00,0xd9,0xf6,0xb7,0x4b,0x00,0x00,0x00,0x23,0x49,0x44,0x41,
    0x54,0x78,0xda,0x63,0x64,0x60,0x60,0xf8,0x2f,0xc0,0xc0,0xf1,0x87,0x16,0x98,0x85,0x41,0x81,0x83,0x81,
    0x81,0x81,0x36,0x78,0xd4,0xf0,0x51,0xc3,0x07,0x89,0xe1,0x00,0x65,0xbb,0x1f,0x99,0xa3,0x5d,0x35,0x0a,
    0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82,
};
const uint8_t kPngGray[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x17,
    0x00,0x00,0x00,0x09,0x08,0x00,0x00,0x00,0x00,0xfc,0x9d,0xe8,0x97,0x00,0x00,0x00,0x13,0x49,0x44,0x41,
    0x54,0x78,0xda,0x63,0x64,0xe0,0xc6,0x0a,0x98,0xd8,0xb1,0x83,0xa1,0x22,0x0e,0x00,0xab,0xed,0x06,0x0c,
    0x93,0x39,0x4d,0xcc,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82,
};
const uint8_t kPngDynamic[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x40,
    0x00,0x00,0x00,0x20,0x08,0x00,0x00,0x00,0x00,0x87,0xf6,0x21,0x58,0x00,0x00,0x01,0x80,0x49,0x44,0x41,
    0x54,0x78,0xda,0xed,0xd5,0xb1,0x6a,0xf3,0x30,0x10,0xc0,0x71,0x9d,0xac,0xb7,0x50,0x2f,0x57,0x3d,0x86,
    0x11,0x87,0xe7,0x8f,0x10,0xbe,0xb1,0x84,0xd0,0xb9,0x18,0xd3,0xb1,0x94,0xd2,0x39,0x84,0xd2,0xb1,0x04,
    0xd3,0x39,0x18,0x93,0xc7,0x28,0x21,0xf4,0x31,0x8c,0x10,0x7e,0x0c,0xa3,0x6f,0xf0,0xb7,0x68,0xaa,0x84,
    0xd7,0x0e,0x37,0x1c,0x3f,0x38,0x4d,0x7f,0x04,0x02,0x00,0xa6,0x05,0x03,0x80,0x06,0x11,0xcd,0x1a,0xd1,
    0x20,0xe2,0x5f,0x44,0x34,0x88,0x6b,0x83,0x88,0x06,0xd7,0x09,0xa6,0xc0,0x94,0xdc,0xb0,0x08,0xc4,0x65,
    0xc3,0x3c,0x21,0xf3,0x03,0x73,0xa1,0x2d,0x37,0x25,0xa7,0x98,0x04,0xe4,0xc3,0xcb,0xc5,0x07,0xb2,0x6f,
    0xcf,0x57,0x97,0xbf,0xc9,0x09,0x9b,0xfb,0xd3,0xd1,0x17,0xba,0xde,0x9d,0x3f,0x1c,0x98,0x87,0x6d,0xff,
    0x39,0x04,0x7a,0xbc,0xeb,0xda,0x31,0xc5,0x14,0x20,0x1f,0x8e,0xf3,0xdd,0xe3,0xc5,0x01,0xf2,0xe1,0xf3,
    0xff,0x36,0xbf,0xf2,0x93,0x29,0x30,0xe2,0xa6,0xf7,0x43,0x20,0xb9,0xea,0xfc,0x38,0x21,0xdf,0x9e,0xbc,
    0x2f,0xb4,0x5d,0x9d,0x07,0x9f,0x62,0xa2,0x90,0xb0,0x68,0x40,0x10,0x91,0x26,0xda,0x68,0x22,0xd2,0xb4,
    0x21,0x4d,0x44,0x7a,0x43,0xa4,0x89,0xe8,0x4f,0x82,0x29,0x40,0xb6,0xb5,0x95,0x81,0x6c,0x55,0x5b,0x01,
    0x68,0xed,0xa3,0xb5,0x81,0xac,0xad,0x2b,0x91,0x62,0x12,0x4c,0x79,0x78,0xf9,0x1e,0x02,0x55,0xfb,0xe7,
    0xeb,0x38,0x21,0xbf,0x3f,0x5d,0x7c,0xa1,0xed,0xdb,0xeb,0x97,0x4b,0x31,0x09,0xd8,0x6c,0xfb,0xa3,0x0f,
    0x54,0xef,0xba,0xd6,0xe5,0x6f,0x6a,0x42,0x7e,0xff,0x98,0xef,0xb6,0xf3,0xdd,0xf6,0x3a,0x04,0xaa,0xf6,
    0xed,0xfc,0xe6,0x4f,0xa6,0x00,0x79,0xd5,0xbb,0x31,0x90,0xb8,0xed,0x9c,0x9b,0x37,0x1f,0x48,0xdc,0x74,
    0xa3,0x4b,0x31,0x21,0x0b,0x01,0x4b,0x06,0x8a,0xec,0xfe,0x62,0x93,0x13,0xf2,0xfd,0xe9,0xe2,0x0b,0x6d,
    0x77,0xe7,0x2f,0x07,0xa6,0xdc,0xf6,0xdf,0x43,0xa0,0xea,0xae,0xbb,0x8e,0x29,0xb6,0xbc,0xc6,0xfc,0xfe,
    0x62,0x53,0xf9,0xfd,0xc5,0xa6,0xf2,0xfb,0x8b,0x4d,0xc0,0xd2,0x1e,0x65,0x76,0x7f,0xb1,0xa9,0xfc,0xfe,
    0x62,0x93,0xf9,0xfd,0xc5,0xb6,0xbc,0xc6,0xfc,0xfe,0x62,0x53,0xf9,0xfd,0xc5,0x06,0xbf,0x7f,0x63,0xf8,
    0x07,0xa3,0x51,0xcd,0x1e,0xfb,0xae,0xd0,0x25,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,
    0x82,
};

void TestPng() {
    uint32_t w = 0, h = 0;
    int channels = 0;
    std::vector<uint8_t> px;
    std::string error;
    CHECK(sb::DecodePng(std::vector<uint8_t>(std::begin(kPngRgba), std::end(kPngRgba)), w, h,
                        channels, px, error));
    CHECK(w == 23 && h == 9 && channels == 4);
    bool same = px.size() == size_t(w) * h * 4;
    for (uint32_t y = 0; same && y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = &px[(y * w + x) * 4];
            same = same && p[0] == uint8_t(x * 16) && p[1] == uint8_t(y * 32) &&
                   p[2] == uint8_t((x + y) * 8) && p[3] == uint8_t(255 - x * 4);
        }
    }
    CHECK(same);
    CHECK(sb::DecodePng(std::vector<uint8_t>(std::begin(kPngGray), std::end(kPngGray)), w, h,
                        channels, px, error));
    CHECK(w == 23 && h == 9 && channels == 1);
    same = px.size() == size_t(w) * h;
    for (uint32_t i = 0; same && i < w * h; ++i) {
        same = px[i] == uint8_t((i % w) * 11 + (i / w) * 7);
    }
    CHECK(same);
    CHECK(sb::DecodePng(std::vector<uint8_t>(std::begin(kPngDynamic), std::end(kPngDynamic)), w,
                        h, channels, px, error));
    CHECK(w == 64 && h == 32 && channels == 1);
    same = px.size() == size_t(w) * h;
    for (uint32_t i = 0; same && i < w * h; ++i) {
        const uint32_t x = i % w, y = i / w;
        same = px[i] == uint8_t(((x * y) % 7) * 30 + ((x ^ y) & 3));
    }
    CHECK(same);
    // Round trip through the sandbox's own (stored) PNG writer.
    std::vector<uint8_t> rgba(70 * 3 * 4);
    for (size_t i = 0; i < rgba.size(); ++i) {
        rgba[i] = uint8_t(i * 7 + 3);
    }
    const fs::path tmp = fs::temp_directory_path() / "bbport_ui_test_roundtrip.png";
    CHECK(sb::WritePng(tmp.string(), 70, 3, rgba));
    CHECK(sb::ReadPng(tmp.string(), w, h, channels, px, error) && px == rgba && channels == 4);
    std::error_code ec;
    fs::remove(tmp, ec);
    std::vector<uint8_t> broken(std::begin(kPngRgba), std::end(kPngRgba));
    broken.resize(broken.size() - 30);
    CHECK(!sb::DecodePng(broken, w, h, channels, px, error));
}

void TestLayout() {
    sb::UiBinding b = sb::ParseBinding("right_grip+b");
    CHECK(b.key == sb::UiInputId::B && b.modifiers.size() == 1 &&
          b.modifiers[0] == sb::UiInputId::RightGrip);
    CHECK(sb::ParseBinding(" Left_Grip + X ").key == sb::UiInputId::X);
    CHECK(sb::ParseBinding("dpad_right").modifiers.empty());
    CHECK(sb::ParseBinding("none").Empty() && sb::ParseBinding("").Empty());
    std::string error;
    CHECK(sb::ParseBinding("right_grip+banana", &error).Empty() && !error.empty());

    sb::UiLayout layout;
    std::string warnings;
    CHECK(sb::ParseLayout("{}", layout, error, &warnings));
    CHECK(warnings.empty());
    CHECK(layout.presets.size() == 5);
    CHECK(layout.active_preset == "body_slots");
    for (const sb::UiPreset& p : layout.presets) {
        for (const std::string& m : p.menus) {
            CHECK(layout.FindMenu(m) != nullptr);
        }
    }
    const sb::UiMenuSpec* radial = layout.FindMenu("quick_radial");
    CHECK(radial && radial->type == "radial" && radial->items.size() == 8 && radial->hand == 1);
    const sb::UiMenuSpec* slots = layout.FindMenu("body_slots");
    CHECK(slots && slots->slots.size() == 6);
    CHECK(slots && std::fabs(slots->slots[0].seated.z - slots->slots[0].standing.z) > 0.1f);
    // Overrides: a menu list replaces the default one; menu keys left out keep type defaults.
    CHECK(sb::ParseLayout(R"({"active_preset": "mine", "presets": [{"name": "mine",
        "menus": ["r"]}], "menus": [{"name": "r", "type": "radial", "items": ["Pebble"],
        "open": "left_grip+y"}]})", layout, error, &warnings));
    CHECK(layout.presets.size() == 1 && layout.menus.size() == 1);
    CHECK(layout.menus[0].open.key == sb::UiInputId::Y && layout.menus[0].on_top);
    CHECK(layout.style.open_seconds > 0.0f); // the built-in style stays
    // The dashboard's stick keys default on and can be turned off.
    CHECK(sb::ParseLayout(R"({"menus": [{"name": "d", "type": "dashboard"},
        {"name": "e", "type": "dashboard", "stick_nav": false, "block_move": false}]})",
                          layout, error, &warnings));
    CHECK(layout.menus.size() == 2 && layout.menus[0].stick_nav && layout.menus[0].block_move);
    CHECK(!layout.menus[1].stick_nav && !layout.menus[1].block_move);
    CHECK(sb::ParseLayout(R"({"presets": [{"name": "p", "menus": ["missing"]}]})", layout, error,
                          &warnings));
    CHECK(warnings.find("missing") != std::string::npos);
    CHECK(!sb::ParseLayout("{\"presets\": [", layout, error));
    // sRGB in the file, linear inside.
    const sb::UiColor c = sb::SrgbColor(0.5f, 1.0f, 0.0f, 0.25f);
    CHECK(std::fabs(c.r - 0.214f) < 0.002f && c.g == 1.0f && c.b == 0.0f && c.a == 0.25f);
}

// ---- Menus, driven like the headset ---------------------------------------------------------------

struct Rig {
    sb::UiSystem ui;
    sb::Config config;
    double time = 10.0;
    sb::UiInput in;
    sb::UiEvents events;
    uint32_t scene_buttons = 0;

    void Reset() {
        in = {};
        in.stage = true;
        in.head = {true, {0.0f, 1.65f, 0.0f}, Quat{}};
        in.grip[0] = in.aim[0] = {true, {-0.24f, 1.05f, -0.30f}, Quat{}};
        in.grip[1] = in.aim[1] = {true, {0.24f, 1.05f, -0.30f}, Quat{}};
    }
    sb::UiBody Body() const {
        sb::UiBody body;
        for (int h = 0; h < 2; ++h) {
            body.tip[h] = in.aim[h].position + Rotate(in.aim[h].rotation, {0, 0, -0.06f});
            body.tip_valid[h] = true;
        }
        return body;
    }
    void Frame(int count = 1) {
        for (int k = 0; k < count; ++k) {
            in.time = (time += 1.0 / 72.0);
            scene_buttons = ui.FilterButtons(in);
            events = {};
            ui.Update(in, Body(), config, events);
            sb::FrameData data;
            ui.Build(data);
        }
    }
};

Quat LookAlong(Vec3 dir) {
    const Vec3 f = Normalize(dir, {0, 0, -1});
    const Vec3 r = Normalize(Cross(f, {0, 1, 0}), {1, 0, 0});
    const Vec3 u = Cross(r, f);
    Mat4 m;
    m.m[0] = r.x, m.m[1] = r.y, m.m[2] = r.z;
    m.m[4] = u.x, m.m[5] = u.y, m.m[6] = u.z;
    m.m[8] = -f.x, m.m[9] = -f.y, m.m[10] = -f.z;
    return ToQuat(m);
}

void TestMenus(const fs::path& assets) {
    const fs::path dir = fs::temp_directory_path() / "bbport_ui_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "ui", ec);
    const bool have_ui = fs::exists(assets / "ui" / "icons.json");
    if (have_ui) {
        for (const char* f : {"icons.json", "icons.png", "font.json", "font.png"}) {
            fs::copy_file(assets / "ui" / f, dir / "ui" / f, fs::copy_options::overwrite_existing,
                          ec);
        }
    } else {
        std::printf("(no %s: item names stand in for icons)\n", (assets / "ui").string().c_str());
    }
    Rig r;
    std::string log;
    CHECK(r.ui.Load(dir, nullptr, log, "test"));
    CHECK(fs::exists(dir / "ui_layouts.json")); // written with the defaults
    CHECK(r.ui.ActivePreset() == "body_slots");
    CHECK(r.ui.Count("Blood Vial") == 20 && r.ui.Count("Saw Cleaver") == -1);
    r.Reset();
    r.Frame(3);

    // Preset cycling: d-pad right (the bridge's L3 layer) goes to the next preset.
    r.in.buttons = BBXR_BTN_RIGHT;
    r.Frame();
    CHECK(r.ui.ActivePreset() == "wrist_strip");
    CHECK(r.events.haptics.size() == 2);
    r.in.buttons = 0;
    r.Frame();

    // Radial: hold right grip + B. B reaches the scene only without the grip.
    CHECK(r.ui.SetPreset("radial"));
    r.in.grip[1] = r.in.aim[1] = {true, {0.17f, 1.30f, -0.36f}, Quat{}};
    r.in.buttons = BBXR_BTN_CROSS;
    r.Frame();
    CHECK(r.scene_buttons == BBXR_BTN_CROSS && !r.ui.IsOpen("quick_radial"));
    r.in.buttons = 0;
    r.Frame();
    r.in.squeeze[1] = 1.0f;
    r.Frame();
    r.in.buttons = BBXR_BTN_CROSS;
    r.Frame();
    CHECK(r.scene_buttons == 0 && r.ui.IsOpen("quick_radial"));
    CHECK(!r.events.haptics.empty() && r.events.haptics[0].hand == 1);
    r.in.stick[1][0] = r.in.stick[1][1] = 0.72f; // up-right: item 1
    r.Frame(3);
    CHECK(r.ui.Hovered("quick_radial") == 1);
    r.in.stick[1][0] = r.in.stick[1][1] = 0.0f; // recentered: the pick stays
    r.Frame(2);
    CHECK(r.ui.Hovered("quick_radial") == 1);
    r.in.buttons = 0; // release: use it
    r.Frame();
    CHECK(!r.ui.IsOpen("quick_radial"));
    CHECK(r.ui.Count("Quicksilver Bullets") == 19);
    CHECK(r.scene_buttons == 0);
    // By moving the hand: down-left is item 5 of 8.
    r.in.buttons = BBXR_BTN_CROSS;
    r.Frame();
    CHECK(r.ui.IsOpen("quick_radial"));
    r.in.grip[1].position = r.in.grip[1].position + Vec3{-0.04f, -0.04f, 0.0f};
    r.Frame(2);
    CHECK(r.ui.Hovered("quick_radial") == 5);
    r.in.buttons = 0;
    r.Frame();
    CHECK(r.ui.Count("Fire Paper") == 9);
    // Released without a pick: nothing used.
    r.in.grip[1].position = {0.17f, 1.30f, -0.36f};
    r.in.buttons = BBXR_BTN_CROSS;
    r.Frame(2);
    r.in.buttons = 0;
    r.Frame();
    CHECK(r.ui.Count("Fire Paper") == 9 && r.ui.Count("Quicksilver Bullets") == 19);
    r.in.squeeze[1] = 0.0f;

    // Weapon wheel: hold left grip + X, stick left (item 6: Hunter Blunderbuss), release.
    r.in.grip[0] = r.in.aim[0] = {true, {-0.19f, 1.28f, -0.36f}, Quat{}};
    r.in.squeeze[0] = 1.0f;
    r.Frame();
    r.in.buttons = BBXR_BTN_SQUARE;
    r.Frame();
    CHECK(r.ui.IsOpen("weapon_wheel") && r.scene_buttons == 0);
    r.in.stick[0][0] = -1.0f;
    r.Frame(2);
    CHECK(r.ui.Hovered("weapon_wheel") == 6);
    r.in.buttons = 0;
    r.Frame();
    r.in.stick[0][0] = 0.0f;
    CHECK(r.config.left_weapon == "wp_a_2600" || r.config.left_weapon == "Hunter Blunderbuss");
    // The same weapon again still changes the text the scene compares (forces a re-pick).
    const std::string before = r.config.left_weapon;
    r.in.buttons = BBXR_BTN_SQUARE;
    r.Frame();
    r.in.stick[0][0] = -1.0f;
    r.Frame(2);
    r.in.buttons = 0;
    r.Frame();
    CHECK(r.config.left_weapon != before);
    r.in.stick[0][0] = 0.0f;
    r.in.squeeze[0] = 0.0f;
    r.Frame();

    // Regression (wheel "locks"): a wheel on the stick's own hand opened while that stick is still
    // pushed from walking must not pin a segment; it picks only after the stick recentered once.
    r.in.squeeze[0] = 1.0f;
    r.in.stick[0][0] = -1.0f;
    r.in.buttons = BBXR_BTN_SQUARE;
    r.Frame(3);
    CHECK(r.ui.IsOpen("weapon_wheel") && r.ui.Hovered("weapon_wheel") == -1);
    r.in.stick[0][0] = 0.0f;
    r.Frame(2);
    r.in.stick[0][0] = -1.0f;
    r.Frame(2);
    CHECK(r.ui.Hovered("weapon_wheel") == 6);
    r.in.buttons = 0;
    r.in.stick[0][0] = 0.0f;
    r.Frame(2);
    CHECK(!r.ui.IsOpen("weapon_wheel"));
    r.in.squeeze[0] = 0.0f;
    r.Frame();

    // Grid: right grip + A toggles it in front of the head; point at a card and pull the trigger.
    CHECK(r.ui.SetPreset("grid"));
    r.Reset();
    r.in.squeeze[1] = 1.0f;
    r.Frame();
    r.in.buttons = BBXR_BTN_CIRCLE;
    r.Frame();
    CHECK(r.ui.IsOpen("inventory_grid") && r.scene_buttons == 0);
    r.in.buttons = 0;
    r.in.squeeze[1] = 0.0f;
    r.Frame(12);
    const sb::UiMenuSpec* grid = r.ui.Layout().FindMenu("inventory_grid");
    CHECK(grid != nullptr);
    if (grid) {
        // The panel: 0.6 m ahead, 0.15 m down, tilted 20 deg; aim at the first card.
        const sb::UiPanel panel = sb::UiGridPanel(*grid, r.in.head);
        CHECK(std::fabs(panel.origin.z + grid->distance) < 1e-4f &&
              std::fabs(panel.origin.y - (1.65f - grid->below)) < 1e-4f);
        const Vec3 z = panel.z;
        float u, v, u1, v1;
        sb::UiGridCard(*grid, 0, u, v);
        sb::UiGridCard(*grid, 1, u1, v1);
        const Vec3 target = panel.At(u, v);
        const Vec3 from{0.22f, 1.22f, -0.18f};
        r.in.aim[1] = {true, from, LookAlong(target - from)};
        r.Frame(2);
        CHECK(r.ui.Hovered("inventory_grid") == 0);
        r.in.trigger[1] = 1.0f;
        r.Frame();
        CHECK(r.ui.Count("Blood Vial") == 19);
        r.in.trigger[1] = 0.0f;
        r.Frame();
        // Touch: the left index fingertip pokes the second card (crosses the panel's surface).
        const int bullets = r.ui.Count("Quicksilver Bullets");
        r.in.aim[1].rotation = AxisAngle({1, 0, 0}, -80.0f * kDeg); // the right ray points down
        const Vec3 card = panel.At(u1, v1);
        const auto tip_at = [&](float height) {
            r.in.aim[0] = r.in.grip[0] = {true, card + z * height + Vec3{0, 0, 0.06f}, Quat{}};
        };
        tip_at(0.02f);
        r.Frame(2);
        CHECK(r.ui.Hovered("inventory_grid") == 1);
        CHECK(r.ui.Count("Quicksilver Bullets") == bullets);
        tip_at(-0.004f);
        r.Frame();
        CHECK(r.ui.Count("Quicksilver Bullets") == bullets - 1);
        tip_at(0.03f); // back out: no second poke
        r.Frame(2);
        CHECK(r.ui.Count("Quicksilver Bullets") == bullets - 1);
        r.in.aim[1] = {true, from, LookAlong(target - from)};
        r.Frame(2);
        // Tabs (with doc 17's item catalog): the "throw" tab shows the thrown items only.
        float tu, tv;
        if (have_ui && sb::UiGridTab(*grid, 2, tu, tv) && grid->tabs[2] == "throw") {
            const Vec3 tab = panel.At(tu, tv);
            r.in.aim[1] = {true, from, LookAlong(tab - from)};
            r.Frame(2);
            r.in.trigger[1] = 1.0f;
            r.Frame();
            r.in.trigger[1] = 0.0f;
            r.in.aim[1] = {true, from, LookAlong(target - from)};
            r.Frame(2);
            CHECK(r.ui.Hovered("inventory_grid") == 0);
            const int molotovs = r.ui.Count("Molotov Cocktail");
            r.in.trigger[1] = 1.0f;
            r.Frame();
            r.in.trigger[1] = 0.0f;
            CHECK(r.ui.Count("Molotov Cocktail") == molotovs - 1); // the first thrown item
            r.in.stick[1][1] = 1.0f; // flick up: back to the previous tab ("heal"), then "all"
            r.Frame();
            r.in.stick[1][1] = 0.0f;
            r.Frame();
            r.in.stick[1][1] = 1.0f;
            r.Frame();
            r.in.stick[1][1] = 0.0f;
            r.Frame(2);
        }
        // Stick flick right: page 2 (the first card is now item 15).
        r.in.stick[1][0] = 1.0f;
        r.Frame();
        r.in.stick[1][0] = 0.0f;
        r.Frame(2);
        CHECK(r.ui.Hovered("inventory_grid") == 15);
    }
    r.in.squeeze[1] = 1.0f;
    r.in.buttons = BBXR_BTN_CIRCLE;
    r.Frame();
    CHECK(!r.ui.IsOpen("inventory_grid"));
    r.in.buttons = 0;
    r.in.squeeze[1] = 0.0f;
    r.Frame();

    // Wrist: shown while the left palm (+X of the left grip) faces the eyes.
    CHECK(r.ui.SetPreset("wrist_strip"));
    r.Reset();
    const Vec3 wrist{-0.05f, 1.32f, -0.30f};
    const Vec3 to_head = Normalize(Vec3{0.0f, 1.65f, 0.0f} - wrist);
    const Vec3 zz = Normalize(Cross(to_head, {0, 1, 0}));
    Mat4 m;
    const Vec3 yy = Cross(zz, to_head);
    m.m[0] = to_head.x, m.m[1] = to_head.y, m.m[2] = to_head.z;
    m.m[4] = yy.x, m.m[5] = yy.y, m.m[6] = yy.z;
    m.m[8] = zz.x, m.m[9] = zz.y, m.m[10] = zz.z;
    r.in.head.rotation = LookAlong(wrist - r.in.head.position);
    r.in.grip[0] = r.in.aim[0] = {true, wrist, ToQuat(m)};
    r.Frame(3);
    CHECK(r.ui.IsOpen("wrist_strip"));
    r.in.grip[0].rotation = AxisAngle({0, 1, 0}, kPi) * ToQuat(m); // palm away
    r.Frame(3);
    CHECK(!r.ui.IsOpen("wrist_strip"));

    // Body slots: reach the chest pouch and squeeze; seated uses the seated offsets.
    CHECK(r.ui.SetPreset("body_slots"));
    r.Reset();
    const sb::UiMenuSpec* body = r.ui.Layout().FindMenu("body_slots");
    CHECK(body != nullptr);
    if (body) {
        const auto slot = [&](const char* name, bool seated) {
            const Vec3 neck = r.in.head.position + Rotate(r.in.head.rotation, body->neck);
            for (const sb::UiBodySlot& s : body->slots) {
                if (s.name == name) {
                    return neck + (seated ? s.seated : s.standing);
                }
            }
            return neck;
        };
        const int vials = r.ui.Count("Blood Vial");
        r.in.grip[0] = r.in.aim[0] = {true, slot("chest", false) + Vec3{0.01f, 0, 0}, Quat{}};
        r.Frame(2);
        CHECK(r.ui.Hovered("body_slots") == 2);
        r.in.squeeze[0] = 1.0f;
        r.Frame();
        CHECK(r.ui.Count("Blood Vial") == vials - 1);
        CHECK(!r.events.haptics.empty() && r.events.haptics.back().amplitude >= 0.79f);
        r.in.squeeze[0] = 0.0f;
        r.Frame();
        // The right hip slot is for the right hand only.
        r.in.grip[0].position = slot("right_hip", false);
        r.Frame(2);
        CHECK(r.ui.Hovered("body_slots") == -1);
        r.in.grip[1] = r.in.aim[1] = {true, slot("right_hip", false), Quat{}};
        r.Frame(2);
        CHECK(r.ui.Hovered("body_slots") == 0);
        // Without the item table the equip text is the display name; if it already equals
        // the current weapon the UI flips its case so the scene re-picks it. Start from a
        // different weapon so the result does not depend on the default config.
        r.config.weapon = "Hunter Axe";
        r.in.squeeze[1] = 1.0f;
        r.Frame();
        CHECK(r.config.weapon == "wp_a_0800" || r.config.weapon == "Saw Cleaver" ||
              r.config.weapon == "WP_A_0800");
        r.in.squeeze[1] = 0.0f;
        // Sit down (head 0.5 m lower than the calibrated eye height): the seated hip.
        r.in.head.position.y -= 0.5f;
        r.in.grip[1].position = slot("right_hip", true);
        r.in.grip[0].position = {-0.3f, 0.6f, 0.2f};
        r.Frame(3);
        CHECK(r.ui.Hovered("body_slots") == 0);
        r.in.grip[1].position = slot("right_hip", false); // the standing spot is now empty
        r.Frame(2);
        CHECK(r.ui.Hovered("body_slots") == -1);
    }

    // Hot reload: the file names another active preset.
    r.Reset();
    {
        std::string text = sb::DefaultLayoutText();
        const std::string key = "\"active_preset\": \"body_slots\"";
        const size_t at = text.find(key);
        CHECK(at != std::string::npos);
        text.replace(at, key.size(), "\"active_preset\": \"grid\"");
        std::ofstream(dir / "ui_layouts.json", std::ios::binary) << text;
        fs::last_write_time(dir / "ui_layouts.json",
                            fs::last_write_time(dir / "ui_layouts.json", ec) +
                                std::chrono::seconds(2),
                            ec);
        r.Frame(60); // polled every 0.5 s
        CHECK(r.ui.ActivePreset() == "grid");
        CHECK(r.ui.Count("Blood Vial") == 20); // counts refill on reload
    }
    r.ui.Destroy(nullptr);

    // The interaction log.
    std::string all;
    for (const auto& entry : fs::directory_iterator(dir / "ui_log", ec)) {
        std::ifstream f(entry.path(), std::ios::binary);
        all += std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    CHECK(all.find("\"event\": \"open\"") != std::string::npos);
    CHECK(all.find("\"event\": \"select\"") != std::string::npos);
    CHECK(all.find("\"method\": \"stick\"") != std::string::npos);
    CHECK(all.find("\"method\": \"move\"") != std::string::npos);
    CHECK(all.find("\"method\": \"point\"") != std::string::npos);
    CHECK(all.find("\"method\": \"touch\"") != std::string::npos);
    CHECK(all.find("\"event\": \"grab\"") != std::string::npos);
    CHECK(all.find("\"event\": \"cancel\"") != std::string::npos);
    CHECK(all.find("\"event\": \"glance\"") != std::string::npos);
    CHECK(all.find("\"event\": \"summary\"") != std::string::npos);
    CHECK(!have_ui || all.find("\"event\": \"tab\"") != std::string::npos);
    fs::remove_all(dir, ec);
}

void TestAssets(const fs::path& assets) {
    sb::UiAtlas atlas;
    sb::UiFont font;
    std::string log;
    if (!atlas.Load(assets / "ui", log) || !font.Load(assets / "ui", log)) {
        std::printf("(no %s: atlas/font tests skipped)\n", (assets / "ui").string().c_str());
        return;
    }
    const sb::UiItem* vial = atlas.Find("blood vial");
    CHECK(vial && vial->kind == "goods" && vial->id == 1000 && vial->max == 20);
    float uv[4];
    CHECK(vial && atlas.IconUv(vial->icon, uv) && uv[2] > uv[0] && uv[3] > uv[1]);
    const sb::UiItem* cleaver = atlas.Find("wp_a_0800");
    CHECK(cleaver && cleaver->name == "Saw Cleaver" && cleaver->hand == "right");
    const sb::UiItem* torch = atlas.Find("wp_a_1800");
    CHECK(torch && torch->hand == "left");
    CHECK(atlas.Find("weapon:14000000") && atlas.Find("weapon:14000000")->name == "Hunter Pistol");
    CHECK(font.Ready() && font.Find('A') && font.Find('g'));
    const float w1 = font.Measure("Blood Vial"), w2 = font.Measure("Blood Vial Blood Vial");
    CHECK(w1 > 2.0f && w1 < 8.0f && std::fabs(w2 - (2.0f * w1 + font.Measure(" "))) < 0.05f);
    CHECK(sb::Utf8Decode("A\xc3\xa9\xe2\x80\x99").size() == 3);
}

} // namespace

int main(int argc, char** argv) {
    const fs::path assets = argc > 1 ? fs::path(argv[1]) : fs::path("out/vr_assets");
    TestPng();
    TestLayout();
    TestAssets(assets);
    TestMenus(assets);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
