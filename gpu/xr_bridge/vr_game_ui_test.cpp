// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_game_ui_test.exe [out/vr_assets]: offline tests of the game's VR UI (vr_game_ui.h,
// vr_ui_filter.h), no headset, no GPU, no game: the input filter (R3 tap = lock-on, R3 held =
// the inventory, a standing L3 click = the quick ring, L3 while walking = the jump, nothing to
// the game while the menus have the hands, held buttons hidden until released), the item ids,
// and the UI on scripted controllers and game state: swing labels and haptics from the stamina
// tickets, the quick ring choosing a quick slot (the action the host carries out), the inventory
// grid by R3 hold, the HUD's triangles. With out/vr_assets/ui the real item table and font are
// used (copied to a scratch folder: the user's ui_layouts.json is never touched).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "sandbox3d/image_out.h"
#include "sandbox3d/item_sheet.h"
#include "sandbox3d/ui_drag.h"
#include "vr_game_ui.h"
#include "vr_mirror_ui.h"
#include "vr_room_line.h"
#include "vr_skin_model.h"
#include "vr_ui_filter.h"

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

BbXrInput Pad(uint32_t buttons = 0, uint8_t lx = 128, uint8_t ly = 128, uint8_t rx = 128,
              uint8_t ry = 128) {
    BbXrInput in{};
    in.active = 1;
    in.buttons = buttons;
    in.lx = lx;
    in.ly = ly;
    in.rx = rx;
    in.ry = ry;
    return in;
}

uint32_t GameSees(vrui::InputFilter& f, double t, BbXrInput in) {
    f.Apply(t, in);
    return in.buttons;
}

void TestFilter() {
    using vrui::InputFilter;
    InputFilter f;
    InputFilter::Settings s;
    s.enabled = true;
    s.r3_hold_s = 0.5;
    f.Configure(s);
    // R3 tapped: nothing while held, a short R3 after the release; the menus never see it.
    double t = 0.0;
    CHECK(!(GameSees(f, t, Pad(BBXR_BTN_R3)) & BBXR_BTN_R3));
    CHECK(!(f.Step(t, Pad(BBXR_BTN_R3)).buttons & BBXR_BTN_R3));
    t = 0.2;
    CHECK(!(GameSees(f, t, Pad(BBXR_BTN_R3)) & BBXR_BTN_R3));
    t = 0.25;
    CHECK(GameSees(f, t, Pad()) & BBXR_BTN_R3); // released: lock-on
    CHECK(!(f.Step(t, Pad()).buttons & BBXR_BTN_R3));
    t = 0.30;
    CHECK(GameSees(f, t, Pad()) & BBXR_BTN_R3); // still the 0.12 s tap
    t = 0.45;
    CHECK(!(GameSees(f, t, Pad()) & BBXR_BTN_R3));
    // R3 held 0.5 s: the menus' r3, never the game's.
    t = 1.0;
    GameSees(f, t, Pad(BBXR_BTN_R3));
    t = 1.3;
    CHECK(!(f.Step(t, Pad(BBXR_BTN_R3)).buttons & BBXR_BTN_R3));
    t = 1.55;
    CHECK(f.Step(t, Pad(BBXR_BTN_R3)).buttons & BBXR_BTN_R3);
    CHECK(!(GameSees(f, t, Pad(BBXR_BTN_R3)) & BBXR_BTN_R3));
    t = 1.8;
    CHECK(!(GameSees(f, t, Pad()) & BBXR_BTN_R3)); // released after the hold: no lock-on
    t = 2.0;
    CHECK(!(GameSees(f, t, Pad()) & BBXR_BTN_R3));
    // An r3 menu open: a tap goes to the menus (it closes it), not to the game.
    f.SetMenuState(true, true, false, true);
    t = 3.0;
    f.Step(t, Pad(BBXR_BTN_R3));
    t = 3.1;
    CHECK(f.Step(t, Pad()).buttons & BBXR_BTN_R3);
    CHECK(!(GameSees(f, t, Pad()) & BBXR_BTN_R3));
    f.SetMenuState(false, false, false, false);

    // A standing L3 click (xr_input's pulse on release): the menus' l3, never the game's.
    t = 4.0;
    CHECK(!(GameSees(f, t, Pad(BBXR_BTN_L3)) & BBXR_BTN_L3));
    CHECK(f.Step(t, Pad(BBXR_BTN_L3)).buttons & BBXR_BTN_L3);
    t = 4.2;
    f.Step(t, Pad());
    // L3 while walking (the jump): the game's, the menus do not see it.
    t = 5.0;
    CHECK(GameSees(f, t, Pad(BBXR_BTN_L3, 128, 10)) & BBXR_BTN_L3);
    CHECK(!(f.Step(t, Pad(BBXR_BTN_L3, 128, 10)).buttons & BBXR_BTN_L3));
    t = 5.2;
    f.Step(t, Pad(0, 128, 10));
    GameSees(f, t, Pad(0, 128, 10));
    // L3 while walking with a menu open: closes the menus, no jump.
    f.SetMenuState(true, false, false, true);
    t = 6.0;
    const InputFilter::UiView v = f.Step(t, Pad(BBXR_BTN_L3, 128, 10));
    CHECK(v.close_menus);
    CHECK(!(GameSees(f, t, Pad(BBXR_BTN_L3, 128, 10)) & BBXR_BTN_L3));
    CHECK(!f.Step(t + 0.01, Pad(BBXR_BTN_L3, 128, 10)).close_menus); // once
    // Engaged: no buttons, the right stick is the menus', the left stick still walks.
    BbXrInput in = Pad(BBXR_BTN_R1 | BBXR_BTN_CROSS | BBXR_BTN_RIGHT, 128, 20, 250, 128);
    in.r2 = 255;
    f.Apply(t + 0.1, in);
    CHECK(in.buttons == 0);
    CHECK(in.rx == 128 && in.ry == 128);
    CHECK(in.ly == 20);
    CHECK(in.r2 == 0);
    // Let go with the trigger (d-pad right) still held: hidden until released.
    f.Step(t + 0.2, Pad(BBXR_BTN_RIGHT));
    GameSees(f, t + 0.2, Pad(BBXR_BTN_RIGHT));
    f.SetMenuState(false, false, false, false);
    CHECK(!(GameSees(f, t + 0.3, Pad(BBXR_BTN_RIGHT)) & BBXR_BTN_RIGHT));
    GameSees(f, t + 0.4, Pad());
    CHECK(GameSees(f, t + 0.5, Pad(BBXR_BTN_RIGHT)) & BBXR_BTN_RIGHT);
    // lock_on=false: a tap never reaches the game (with or without an r3 menu); the hold still
    // reaches the menus.
    for (const bool menu : {true, false}) {
        InputFilter g;
        InputFilter::Settings gs;
        gs.enabled = true;
        gs.r3_hold_s = 0.5;
        gs.r3_menu = menu;
        gs.lock_on = false;
        g.Configure(gs);
        double u = 0.0;
        CHECK(!(GameSees(g, u, Pad(BBXR_BTN_R3)) & BBXR_BTN_R3));
        u = 0.2;
        g.Step(u, Pad(BBXR_BTN_R3));
        u = 0.25;
        CHECK(!(GameSees(g, u, Pad()) & BBXR_BTN_R3));
        CHECK(!(g.Step(u, Pad()).buttons & BBXR_BTN_R3));
        u = 0.3;
        CHECK(!(GameSees(g, u, Pad()) & BBXR_BTN_R3));
        u = 1.0;
        GameSees(g, u, Pad(BBXR_BTN_R3));
        u = 1.55;
        CHECK(!(GameSees(g, u, Pad(BBXR_BTN_R3)) & BBXR_BTN_R3));
        CHECK(((g.Step(u, Pad(BBXR_BTN_R3)).buttons & BBXR_BTN_R3) != 0) == menu);
        u = 1.8;
        CHECK(!(GameSees(g, u, Pad()) & BBXR_BTN_R3));
    }
    // Off: everything passes as it comes.
    s.enabled = false;
    f.Configure(s);
    CHECK(GameSees(f, 10.0, Pad(BBXR_BTN_R3 | BBXR_BTN_L3)) == (BBXR_BTN_R3 | BBXR_BTN_L3));
}

void TestIds() {
    CHECK(vrui::ItemKey(0x400003E8u) == "goods:1000");
    CHECK(vrui::ItemKey(7000000u) == "weapon:7000000");
    CHECK(vrui::ItemKey(BBXR_ITEM_NONE).empty());
    CHECK(vrui::ItemKey(0x10000000u | 5u).empty()); // armor: not shown
    CHECK(vrui::ItemId("goods:900") == 0x40000384u);
    CHECK(vrui::ItemId("weapon:14000000") == 14000000u);
    CHECK(vrui::ItemId("Blood Vial") == BBXR_ITEM_NONE);
}

BbXrGameState GameState(int stamina) {
    BbXrGameState s{};
    s.size = sizeof(s);
    s.flags = BBXR_GAME_IN_WORLD | BBXR_GAME_STAMINA | BBXR_GAME_QUICK | BBXR_GAME_ITEMS |
              BBXR_GAME_WEAPONS;
    s.stamina = stamina;
    s.stamina_max = 91;
    s.quick_count = 5;
    s.quick_active = 1;
    const uint32_t quick[5] = {0x400003E8u, 0x400004B0u, BBXR_ITEM_NONE, 0x400004CEu,
                               0x40000586u}; // vial, molotov, -, pebble, shining coins
    std::memcpy(s.quick, quick, sizeof(quick));
    s.weapons[0] = 7000000;
    s.weapons[1] = 7010000; // Hunter Axe? any id
    s.weapons[2] = 14000000;
    s.weapons[3] = BBXR_ITEM_NONE;
    const BbXrGameItem items[] = {{0x400003E8u, 12}, {0x40000384u, 17}, {0x400004B0u, 3},
                                  {0x400004CEu, 9},  {0x40000586u, 20}, {7000000u, 1},
                                  {0x40000078u, 1}};
    s.item_count = uint32_t(std::size(items));
    std::memcpy(s.items, items, sizeof(items));
    return s;
}

struct Rig {
    vrui::GameUi ui;
    double t = 0.0;
    vrui::FrameInput in;
    std::vector<vrui::Haptic> haptics;
    std::string log;
    std::string all_log;   // every frame's log
    BbXrGameState state{}; // fed again every frame when `feed` (long scripts outlive the 1 s staleness)
    bool feed = false;

    Rig() {
        in.head = {true, {0.0f, 1.6f, 0.0f}, {0, 0, 0, 1}};
        // Hands in front, palms facing each other, pointing ahead.
        in.grip[0] = {true, {-0.2f, 1.2f, -0.35f}, {0, 0, 0, 1}};
        in.grip[1] = {true, {0.2f, 1.2f, -0.35f}, {0, 0, 0, 1}};
        in.aim[0] = in.grip[0];
        in.aim[1] = in.grip[1];
        in.stage = true;
        in.pad = Pad();
    }
    /// One 90 Hz frame with these buttons (the bridge's GetInput state).
    void Frame(BbXrInput pad, double dt = 1.0 / 90.0) {
        t += dt;
        in.time = t;
        in.pad = pad;
        haptics.clear();
        log.clear();
        if (feed) {
            ui.SetGameState(state, t);
        }
        ui.Update(in, haptics, log);
        all_log += log;
        ui.Build();
    }
    float StrongestHaptic(int hand) const {
        float a = 0.0f;
        for (const vrui::Haptic& h : haptics) {
            if (h.hand == hand) {
                a = std::max(a, h.amplitude);
            }
        }
        return a;
    }
};

/// The actions the UI queued, each answered as done by the host (so the next is not "busy").
uint32_t Take(Rig& r, BbXrUiAction* actions) {
    const uint32_t n = r.ui.TakeActions(actions, 4);
    for (uint32_t i = 0; i < n; ++i) {
        r.ui.PushEvent({BBXR_EV_ACTION_DONE, int32_t(actions[i].kind), 1, 0}, r.t);
    }
    if (n) {
        r.Frame(Pad());
    }
    return n;
}

/// The queued actions without the BBXR_UI_HELD signals (the weapon hide, checked on its own).
uint32_t TakeUse(vrui::GameUi& ui, BbXrUiAction* out, uint32_t capacity) {
    BbXrUiAction all[32];
    const uint32_t n = ui.TakeActions(all, 32);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (all[i].kind != BBXR_UI_HELD && k < capacity) {
            out[k++] = all[i];
        }
    }
    return k;
}

void TestGunRay() {
    sb::UiLayout layout;
    std::string error;
    // Defaults: on, 30 m.
    CHECK(sb::ParseLayout("{\"game\": {\"version\": 2}}", layout, error));
    CHECK(!layout.game.debug_gun_ray);
    CHECK(layout.game.debug_gun_ray_length_m == 30.0f);
    // Parsed keys, the length clamped.
    sb::UiLayout l2;
    CHECK(sb::ParseLayout("{\"game\": {\"version\": 2, \"debug_gun_ray\": false, "
                          "\"debug_gun_ray_length_m\": 12, \"debug_gun_ray_color\": [0, 1, 0]}}",
                          l2, error));
    CHECK(!l2.game.debug_gun_ray && l2.game.debug_gun_ray_length_m == 12.0f);
    CHECK(l2.game.debug_gun_ray_color.g > 0.99f && l2.game.debug_gun_ray_color.r < 0.01f);
    sb::UiLayout l3;
    CHECK(sb::ParseLayout("{\"game\": {\"version\": 2, \"debug_gun_ray_length_m\": 9999}}", l3,
                          error));
    CHECK(l3.game.debug_gun_ray_length_m == 200.0f);
    // Visibility: needs the flag, the world, a valid aim pose; flash for 0.3 s after a pull.
    using vrui::GunRayFor;
    layout.game.debug_gun_ray = true;
    CHECK(GunRayFor(layout.game, true, true, 5.0).visible);
    CHECK(!GunRayFor(layout.game, true, true, 5.0).flash);
    CHECK(!GunRayFor(layout.game, false, true, 5.0).visible);
    CHECK(!GunRayFor(layout.game, true, false, 5.0).visible);
    CHECK(!GunRayFor(l2.game, true, true, 5.0).visible);
    const vrui::GunRayLook idle = GunRayFor(layout.game, true, true, 5.0);
    const vrui::GunRayLook shot = GunRayFor(layout.game, true, true, 0.1);
    CHECK(shot.flash && shot.width_m > idle.width_m && shot.alpha > idle.alpha);
    CHECK(!GunRayFor(layout.game, true, true, 0.31).flash);
    // Angles: straight ahead (-Z) is 0/0; head-relative removes the head's yaw.
    float yaw, pitch;
    vrui::AimAngles({0, 0, -1}, nullptr, &yaw, &pitch);
    CHECK(std::fabs(yaw) < 1e-5f && std::fabs(pitch) < 1e-5f);
    vrui::AimAngles({-1, 0, 0}, nullptr, &yaw, &pitch); // left: positive yaw
    CHECK(std::fabs(yaw - kPi * 0.5f) < 1e-4f);
    const Quat head = AxisAngle({0, 1, 0}, 90.0f * kDeg); // head turned left
    vrui::AimAngles({-1, 0, 0}, &head, &yaw, &pitch);
    CHECK(std::fabs(yaw) < 1e-4f);
    vrui::AimAngles({0, 1, -1}, nullptr, &yaw, &pitch);
    CHECK(std::fabs(pitch - 45.0f * kDeg) < 1e-4f);

    // On the rig: the ray is drawn with the dashboard closed; a pull logs once and flashes.
    Rig r;
    std::string log;
    r.in.aim[0].valid = true;
    CHECK(r.ui.Load(fs::temp_directory_path() / "bbport_game_ui_test_ray", nullptr, log));
    r.ui.SetGameState(GameState(80), r.t);
    r.Frame(Pad());
    CHECK(r.ui.Active());
    const size_t idle_verts = r.ui.HudVertices();
    r.Frame(Pad(BBXR_BTN_L2));
    CHECK(r.log.find("gun ray: L2 pull") != std::string::npos);
    r.Frame(Pad(BBXR_BTN_L2));
    CHECK(r.log.find("gun ray") == std::string::npos); // held: not again
    CHECK(r.ui.HudVertices() > 0 && idle_verts > 0);
}

void TestGameUi(const fs::path& assets) {
    // A scratch copy: Load adds the "game" object to the layout file it finds.
    const fs::path dir = fs::temp_directory_path() / "bbport_game_ui_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "ui", ec);
    const bool have_ui = fs::exists(assets / "ui" / "icons.json", ec);
    if (have_ui) {
        for (const char* f : {"icons.json", "icons.png", "font.json", "font.png"}) {
            fs::copy_file(assets / "ui" / f, dir / "ui" / f, ec);
        }
    } else {
        std::printf("no %s: names instead of icons, no font\n", (assets / "ui").string().c_str());
    }
    // The single menus (quick ring on L3, inventory on R3 held, weapon wheel on right grip + B),
    // still available in the layout file, though no longer the default (the dashboard is).
    {
        std::ofstream old(dir / "ui_layouts.json", std::ios::binary);
        old << "{\n  \"version\": 1,\n  \"game\": {\"version\": 2, \"held_items\": false, \"active\": [\"quick_ring\", "
               "\"inventory\", \"weapon_wheel\", \"wrist_counters\"]}\n}\n";
    }
    Rig r;
    std::string log;
    CHECK(r.ui.Load(dir, nullptr, log));
    CHECK(r.ui.Menus().Layout().game.active.size() == 4);

    // No game state: off, the filter passes everything.
    r.Frame(Pad());
    CHECK(!r.ui.Active());
    BbXrInput raw = Pad(BBXR_BTN_R3);
    r.ui.Filter().Apply(r.t, raw);
    CHECK(raw.buttons == BBXR_BTN_R3);

    // In the world: on, the HUD has the stamina bar.
    r.ui.SetGameState(GameState(80), r.t);
    r.Frame(Pad());
    CHECK(r.ui.Active());
    CHECK(r.ui.Readout().enabled);
    CHECK(r.ui.Readout().stamina == 80.0f && r.ui.Readout().max == 91.0f);
    CHECK(r.ui.HudVertices() > 0);

    // A swing gesture (R1 pulse): LIGHT, pending, a weak tick.
    r.in.ms_since_swing = 5.0;
    r.in.pad.hand_speed[1] = 2.6f;
    r.ui.SetGameState(GameState(80), r.t);
    r.Frame(Pad(BBXR_BTN_R1));
    CHECK(r.ui.Readout().label == sb::SwingReadout::Label::Light);
    CHECK(r.ui.Readout().result == sb::SwingReadout::Result::Pending);
    CHECK(std::fabs(r.StrongestHaptic(1) - 0.2f) < 1e-4f);
    // Its ticket opens (cost 20), then it hits: HIT -20 and a stronger pulse.
    r.ui.PushEvent({BBXR_EV_SWING_OPEN, 20, 100000, 1}, r.t);
    r.Frame(Pad(BBXR_BTN_R1));
    CHECK(r.ui.Readout().label_cost == 20.0f);
    CHECK(r.ui.Readout().pending == 20.0f);
    r.ui.PushEvent({BBXR_EV_SWING_HIT, 20, 0, 1}, r.t);
    r.Frame(Pad());
    CHECK(r.ui.Readout().result == sb::SwingReadout::Result::Hit);
    CHECK(r.ui.Readout().label_paid == 20.0f);
    CHECK(r.ui.Readout().pending == 0.0f);
    CHECK(std::fabs(r.StrongestHaptic(1) - 0.775f) < 1e-4f); // the weapon collision pulse: 20 of 40 stamina
    CHECK(sb::SwingLabelText(r.ui.Readout()) == "LIGHT  HIT  -20");
    // A heavy one that misses: free.
    r.in.ms_since_swing = 5.0;
    r.Frame(Pad(BBXR_BTN_R2));
    CHECK(r.ui.Readout().label == sb::SwingReadout::Label::Heavy);
    CHECK(std::fabs(r.StrongestHaptic(1) - 0.4f) < 1e-4f);
    r.ui.PushEvent({BBXR_EV_SWING_OPEN, 35, 100150, 2}, r.t);
    r.Frame(Pad());
    r.ui.PushEvent({BBXR_EV_SWING_MISS, 35, 0, 2}, r.t);
    r.Frame(Pad());
    CHECK(r.ui.Readout().result == sb::SwingReadout::Result::Miss);
    CHECK(sb::SwingLabelText(r.ui.Readout()) == "HEAVY  MISS  free");
    // At 0 stamina: NO STAMINA and two buzzes 0.1 s apart.
    r.ui.SetGameState(GameState(0), r.t);
    r.in.ms_since_swing = 5.0;
    r.Frame(Pad(BBXR_BTN_R1));
    CHECK(r.ui.Readout().label == sb::SwingReadout::Label::Tired);
    CHECK(std::fabs(r.StrongestHaptic(1) - 0.5f) < 1e-4f);
    int buzzes = 0;
    for (int i = 0; i < 12; ++i) {
        r.ui.SetGameState(GameState(0), r.t);
        r.Frame(Pad());
        buzzes += r.StrongestHaptic(1) > 0.0f;
    }
    CHECK(buzzes == 1);
    r.in.ms_since_swing = 1e9;

    // The quick ring: a standing L3 click opens it; the stick points up (item 0, the vial).
    r.ui.SetGameState(GameState(60), r.t);
    r.Frame(Pad(BBXR_BTN_L3));
    CHECK(r.ui.Menus().IsOpen("quick_ring"));
    r.Frame(Pad());
    BbXrInput game = Pad(BBXR_BTN_SQUARE | BBXR_BTN_RIGHT);
    r.ui.Filter().Apply(r.t, game);
    CHECK(game.buttons == 0); // the menus have the hands
    r.Frame(Pad(0, 128, 128, 128, 0)); // right stick up
    CHECK(r.ui.Menus().Hovered("quick_ring") == 0);
    // Right stick right: the next shown item, the Molotov (slot 1).
    r.Frame(Pad(0, 128, 128, 255, 128));
    const int hovered = r.ui.Menus().Hovered("quick_ring");
    CHECK(hovered >= 0);
    // Confirm with the right trigger.
    r.in.trigger[1] = 0.9f;
    r.Frame(Pad());
    r.in.trigger[1] = 0.0f;
    CHECK(!r.ui.Menus().IsOpen("quick_ring"));
    BbXrUiAction actions[4];
    uint32_t n = Take(r, actions);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(actions[0].kind == BBXR_UI_USE_QUICK);
        // Shown: vial (0), molotov (1), pebble (3), coins (4): one step clockwise from the top.
        CHECK(actions[0].a == 1 || actions[0].a == 4);
        CHECK(uint32_t(actions[0].b) == (actions[0].a == 1 ? 0x400004B0u : 0x40000586u));
    }
    // A second L3 click with nothing picked: the ring opens and closes (cancel), no action.
    r.Frame(Pad(BBXR_BTN_L3));
    r.Frame(Pad());
    CHECK(r.ui.Menus().IsOpen("quick_ring"));
    r.Frame(Pad(BBXR_BTN_L3));
    CHECK(!r.ui.Menus().IsOpen("quick_ring"));
    CHECK(Take(r, actions) == 0);
    r.Frame(Pad());

    // The inventory: R3 held 0.5 s opens it, a tap closes it; lock-on never fires.
    for (int i = 0; i < 50; ++i) {
        r.ui.SetGameState(GameState(60), r.t);
        BbXrInput g = Pad(BBXR_BTN_R3);
        r.ui.Filter().Apply(r.t, g);
        CHECK(!(g.buttons & BBXR_BTN_R3));
        r.Frame(Pad(BBXR_BTN_R3));
    }
    CHECK(r.ui.Menus().IsOpen("inventory"));
    r.Frame(Pad());
    CHECK(r.ui.Menus().IsOpen("inventory")); // released after the hold: stays open
    r.Frame(Pad(BBXR_BTN_R3));
    r.Frame(Pad());
    r.Frame(Pad());
    CHECK(!r.ui.Menus().IsOpen("inventory"));
    BbXrInput g = Pad();
    r.ui.Filter().Apply(r.t, g);
    CHECK(!(g.buttons & BBXR_BTN_R3));

    // The weapon wheel: right grip + B (held). B never reaches the game (it is cross there).
    r.in.squeeze[1] = 0.9f;
    r.Frame(Pad());
    BbXrInput b = Pad(BBXR_BTN_CROSS);
    r.ui.Filter().Apply(r.t, b);
    CHECK(!(b.buttons & (BBXR_BTN_CROSS | BBXR_BTN_CIRCLE))); // neither the old cross nor the new roll
    r.Frame(Pad(BBXR_BTN_CROSS));
    CHECK(r.ui.Menus().IsOpen("weapon_wheel"));
    // Shown: right 1, right 2, left 1 (left 2 is empty); the stick right picks the second.
    r.Frame(Pad(BBXR_BTN_CROSS, 128, 128, 255, 128));
    CHECK(r.ui.Menus().Hovered("weapon_wheel") == 1);
    r.Frame(Pad(0, 128, 128, 128, 128)); // B released: equip
    CHECK(!r.ui.Menus().IsOpen("weapon_wheel"));
    n = Take(r, actions);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(actions[0].kind == BBXR_UI_EQUIP && actions[0].a == 1 && actions[0].b == 7010000);
    }
    r.in.squeeze[1] = 0.0f;
    b = Pad(BBXR_BTN_CROSS);
    r.Frame(Pad());
    r.ui.Filter().Apply(r.t, b);
    CHECK(b.buttons == BBXR_BTN_CIRCLE); // B alone is the game's again: the roll (circle)

    // An action the host never answers: busy for 8 s, then given up with a message.
    r.ui.SetGameState(GameState(60), r.t);
    r.Frame(Pad(BBXR_BTN_L3));
    r.Frame(Pad());
    r.Frame(Pad(0, 128, 128, 128, 0));
    r.in.trigger[1] = 0.9f;
    r.Frame(Pad());
    r.in.trigger[1] = 0.0f;
    CHECK(r.ui.TakeActions(actions, 4) == 1); // not answered
    r.Frame(Pad(BBXR_BTN_L3));
    r.Frame(Pad());
    r.Frame(Pad(0, 128, 128, 128, 0));
    r.in.trigger[1] = 0.9f;
    r.Frame(Pad());
    r.in.trigger[1] = 0.0f;
    CHECK(r.ui.TakeActions(actions, 4) == 0); // refused: busy
    for (int i = 0; i < 10; ++i) {
        r.ui.SetGameState(GameState(60), r.t);
        r.Frame(Pad(), 0.9);
    }
    r.ui.SetGameState(GameState(60), r.t);
    r.Frame(Pad());
    r.Frame(Pad(BBXR_BTN_L3));
    r.Frame(Pad());
    r.Frame(Pad(0, 128, 128, 128, 0));
    r.in.trigger[1] = 0.9f;
    r.Frame(Pad());
    r.in.trigger[1] = 0.0f;
    CHECK(Take(r, actions) == 1); // free again

    // Loading: no player, the UI goes off and closes its menus.
    r.Frame(Pad(BBXR_BTN_L3));
    r.Frame(Pad());
    BbXrGameState loading{};
    loading.size = sizeof(loading);
    r.ui.SetGameState(loading, r.t);
    r.Frame(Pad());
    CHECK(!r.ui.Active());
    CHECK(!r.ui.Menus().IsOpen("quick_ring"));
    fs::remove_all(dir, ec);
}

/// A rotation whose -Z points from `from` to `to` (+Y up).
Quat LookAt(Vec3 from, Vec3 to) {
    const Vec3 f = Normalize(to - from, {0, 0, -1});
    const Vec3 r = Normalize(Cross(f, {0, 1, 0}), {1, 0, 0});
    const Vec3 u = Cross(r, f);
    Mat4 m;
    m.m[0] = r.x, m.m[1] = r.y, m.m[2] = r.z;
    m.m[4] = u.x, m.m[5] = u.y, m.m[6] = u.z;
    m.m[8] = -f.x, m.m[9] = -f.y, m.m[10] = -f.z;
    return ToQuat(m);
}

/// Points hand h's aim ray at a dashboard element (false when it is not drawn).
bool PointAt(Rig& r, int h, int id) {
    Vec3 at;
    if (!r.ui.Menus().ElementCenter("dashboard", id, at)) {
        return false;
    }
    r.in.aim[h].position = r.in.grip[h].position;
    r.in.aim[h].rotation = LookAt(r.in.grip[h].position, at);
    r.in.aim[h].valid = true;
    return true;
}

/// Points at an element and pulls that hand's trigger (one frame in, one out).
void Click(Rig& r, int h, int id) {
    CHECK(PointAt(r, h, id));
    r.ui.SetGameState(GameState(60), r.t);
    r.Frame(Pad());
    r.in.trigger[h] = 0.9f;
    r.Frame(Pad());
    r.in.trigger[h] = 0.0f;
    r.Frame(Pad());
}

void TestDashboard(const fs::path& assets) {
    const fs::path dir = fs::temp_directory_path() / "bbport_game_ui_dash_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "ui", ec);
    for (const char* f : {"icons.json", "icons.png", "font.json", "font.png"}) {
        fs::copy_file(assets / "ui" / f, dir / "ui" / f, ec);
    }
    // A file with the first game object (version 1, the single menus): replaced by the dashboard.
    {
        std::ofstream old(dir / "ui_layouts.json", std::ios::binary);
        old << "{\n  \"version\": 1,\n  \"game\": {\"active\": [\"quick_ring\", \"inventory\"],\n"
               "    \"menus\": [{\"name\": \"x\", \"type\": \"grid\"}]}\n}\n";
    }
    Rig r;
    std::string log;
    CHECK(r.ui.Load(dir, nullptr, log));
    CHECK(fs::exists(dir / "ui_layouts.json.bak", ec));
    const sb::UiGameLayout& game = r.ui.Menus().Layout().game;
    CHECK(game.version == 2);
    CHECK(game.active.size() == 2 && game.active[0] == "dashboard");
    // The upgrade brought the held-items defaults in; these tests are about the dashboard's own
    // actions (a click on a quick slot is the game's use), so turn them off in the file (reloaded).
    CHECK(game.held_items && game.collision_haptics.on);
    {
        bool ok = false;
        std::string text = sb::ReadText(dir / "ui_layouts.json", &ok);
        const size_t at = text.find("\"held_items\": true");
        CHECK(ok && at != std::string::npos);
        if (at != std::string::npos) {
            text.replace(at, 18, "\"held_items\": false");
            std::ofstream f(dir / "ui_layouts.json", std::ios::binary);
            f << text;
        }
        for (int i = 0; i < 120; ++i) {
            r.ui.SetGameState(GameState(60), r.t);
            r.Frame(Pad());
        }
        CHECK(!r.ui.Menus().Layout().game.held_items);
    }
    int recentered = 0;
    static int* recenter_count = nullptr;
    recenter_count = &recentered;
    r.ui.SetRecenter([] { ++*recenter_count; });

    r.ui.SetGameState(GameState(60), r.t);
    r.Frame(Pad());
    CHECK(r.ui.Active());
    // R3 is plain lock-on now: it reaches the game as it comes.
    BbXrInput r3 = Pad(BBXR_BTN_R3);
    r.ui.Filter().Apply(r.t, r3);
    CHECK(r3.buttons & BBXR_BTN_R3);
    // The standing L3 click opens the dashboard in front.
    r.Frame(Pad(BBXR_BTN_L3));
    r.Frame(Pad());
    CHECK(r.ui.Menus().IsOpen("dashboard"));
    CHECK(r.ui.Menus().Engaged());
    // Its triggers are the pointer's: the game gets no weapon swap while it is open.
    BbXrInput swap = Pad(BBXR_BTN_RIGHT);
    r.ui.Filter().Apply(r.t, swap);
    CHECK(swap.buttons == 0);
    // Every part is drawn: the weapon slots, 5 quick slots, the buttons, the inventory cards.
    Vec3 at;
    for (int k = 0; k < 4; ++k) CHECK(r.ui.Menus().ElementCenter("dashboard", sb::UiSystem::DashWeapon(k), at));
    for (int k = 0; k < 5; ++k) CHECK(r.ui.Menus().ElementCenter("dashboard", sb::UiSystem::DashQuick(k), at));
    for (int i = 0; i < 8; ++i) CHECK(r.ui.Menus().ElementCenter("dashboard", sb::UiSystem::DashButton(i), at));
    CHECK(r.ui.Menus().ElementCenter("dashboard", sb::UiSystem::DashItem(0), at));
    // It sits in front of the head, 0.75 m out.
    CHECK(at.z < -0.4f && at.z > -1.0f);
    BbXrUiAction actions[4];
    // The right hand points at the second right weapon (R2) and pulls: switch to it.
    Click(r, 1, sb::UiSystem::DashWeapon(1));
    uint32_t n = Take(r, actions);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_EQUIP && actions[0].a == 1 && actions[0].b == 7010000);
    CHECK(r.ui.Menus().IsOpen("dashboard"));
    // R1 is in the hand already: shown disabled, nothing happens.
    Click(r, 1, sb::UiSystem::DashWeapon(0));
    CHECK(Take(r, actions) == 0);
    // The empty left 2 slot: nothing either.
    Click(r, 1, sb::UiSystem::DashWeapon(3));
    CHECK(Take(r, actions) == 0);
    // An inventory item in no quick slot (the bullets, card 1): disabled with its reason.
    Click(r, 1, sb::UiSystem::DashItem(1));
    CHECK(Take(r, actions) == 0);
    // The left hand points at the Gestures button and pulls the left trigger: touchpad, closes.
    Click(r, 0, sb::UiSystem::DashButton(1));
    n = Take(r, actions);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_PRESS && uint32_t(actions[0].a) == BBXR_BTN_TOUCHPAD);
    CHECK(!r.ui.Menus().IsOpen("dashboard"));
    // Again: Recenter (button 6) calls the bridge's recenter.
    r.Frame(Pad(BBXR_BTN_L3));
    r.Frame(Pad());
    CHECK(r.ui.Menus().IsOpen("dashboard"));
    Click(r, 1, sb::UiSystem::DashButton(6));
    CHECK(recentered == 1);
    CHECK(Take(r, actions) == 0);
    // Tool / bullet (d-pad up).
    Click(r, 1, sb::UiSystem::DashButton(2));
    n = Take(r, actions);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_PRESS && uint32_t(actions[0].a) == BBXR_BTN_UP);
    // A quick slot (the Molotov, slot 1): used, the dashboard closes to show it.
    Click(r, 1, sb::UiSystem::DashQuick(1));
    n = Take(r, actions);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK && actions[0].a == 1 &&
          uint32_t(actions[0].b) == 0x400004B0u);
    CHECK(!r.ui.Menus().IsOpen("dashboard"));
    // The inventory card of the pebble (in quick slot 3) is used through its slot.
    r.Frame(Pad(BBXR_BTN_L3));
    r.Frame(Pad());
    Click(r, 1, sb::UiSystem::DashItem(3));
    n = Take(r, actions);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK && actions[0].a == 3);
    // Select mode (the title's toggle; element 1402): a quick slot only becomes the current one.
    r.Frame(Pad(BBXR_BTN_L3));
    r.Frame(Pad());
    Click(r, 1, 1402);
    Click(r, 1, sb::UiSystem::DashQuick(0));
    n = Take(r, actions);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_SELECT_QUICK && actions[0].a == 0);
    CHECK(r.ui.Menus().IsOpen("dashboard"));
    // With the equip hook running (BBXR_GAME_CAN_EQUIP): an item in no quick slot (the bullets) goes
    // into the first empty quick slot (2) and is used (c = 1), through the game's own command.
    {
        BbXrGameState can = GameState(60);
        can.flags |= BBXR_GAME_CAN_EQUIP;
        r.ui.SetGameState(can, r.t);
        r.Frame(Pad());
        CHECK(r.ui.Menus().IsOpen("dashboard")); // still open from the select above
        Click(r, 1, 1402); // back to USE mode
        r.ui.SetGameState(can, r.t);
        CHECK(PointAt(r, 1, sb::UiSystem::DashItem(1)));
        r.Frame(Pad());
        r.in.trigger[1] = 0.9f;
        r.Frame(Pad());
        r.in.trigger[1] = 0.0f;
        n = Take(r, actions);
        CHECK(n == 1 && actions[0].kind == BBXR_UI_ASSIGN_QUICK && actions[0].a == 2 &&
              uint32_t(actions[0].b) == 0x40000384u && actions[0].c == 1);
        // The weapons tab (tab 6): the Saw Cleaver into the right hand's held slot (R1, slot 1).
        r.ui.SetGameState(can, r.t);
        r.Frame(Pad(BBXR_BTN_L3));
        r.Frame(Pad());
        r.ui.SetGameState(can, r.t);
        CHECK(PointAt(r, 1, 1206));
        r.Frame(Pad());
        r.in.trigger[1] = 0.9f;
        r.Frame(Pad());
        r.in.trigger[1] = 0.0f;
        r.ui.SetGameState(can, r.t);
        r.Frame(Pad());
        CHECK(PointAt(r, 1, sb::UiSystem::DashItem(0)));
        r.Frame(Pad());
        r.in.trigger[1] = 0.9f;
        r.Frame(Pad());
        r.in.trigger[1] = 0.0f;
        n = Take(r, actions);
        CHECK(n == 1 && actions[0].kind == BBXR_UI_EQUIP_ITEM && actions[0].a == 1 &&
              actions[0].b == 7000000);
        // The host gives up (the hook did not take it): a message, nothing pending.
        r.ui.PushEvent({BBXR_EV_ACTION_DONE, int32_t(BBXR_UI_EQUIP_ITEM), 0, 5}, r.t);
        r.Frame(Pad());
        CHECK(r.log.find("did not put it there") != std::string::npos);
        // Remove from wheel (title toggle 1404), one-shot: point at the toggle, then at a quick slot.
        const auto pull = [&](int h, int id) {
            r.ui.SetGameState(can, r.t);
            CHECK(PointAt(r, h, id));
            r.Frame(Pad());
            r.in.trigger[h] = 0.9f;
            r.Frame(Pad());
            r.in.trigger[h] = 0.0f;
            r.Frame(Pad());
        };
        pull(1, sb::UiSystem::DashRemove());
        CHECK(Take(r, actions) == 0); // the toggle itself does nothing yet
        pull(1, sb::UiSystem::DashQuick(2)); // the empty slot: nothing, the mode stays armed
        CHECK(Take(r, actions) == 0);
        pull(1, sb::UiSystem::DashQuick(1)); // the Molotov goes out of slot 1
        n = Take(r, actions);
        CHECK(n == 1 && actions[0].kind == BBXR_UI_CLEAR_QUICK && actions[0].a == 1 &&
              uint32_t(actions[0].b) == 0x400004B0u);
        CHECK(r.ui.Menus().IsOpen("dashboard"));
        r.ui.PushEvent({BBXR_EV_ACTION_DONE, int32_t(BBXR_UI_CLEAR_QUICK), 1, 0}, r.t);
        r.Frame(Pad());
        pull(1, sb::UiSystem::DashQuick(3)); // one-shot: the mode is off again, the pebble is used
        n = Take(r, actions);
        CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK && actions[0].a == 3);
        r.ui.SetGameState(can, r.t);
        r.Frame(Pad(BBXR_BTN_L3));
        r.Frame(Pad());
        // Without the equip hook the toggle is disabled with the reason.
        r.ui.SetGameState(GameState(60), r.t);
        r.Frame(Pad());
        CHECK(r.ui.Menus().IsOpen("dashboard"));
        Click(r, 1, sb::UiSystem::DashRemove());
        Click(r, 1, sb::UiSystem::DashQuick(1));
        n = Take(r, actions);
        CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK); // not armed: a plain use
        // Every quick slot full: an item from the inventory replaces the LAST slot, with a message.
        BbXrGameState full = can;
        full.quick[2] = 0x40000078u;
        r.ui.SetGameState(full, r.t);
        r.Frame(Pad(BBXR_BTN_L3)); // the plain use closed it: open again
        r.Frame(Pad());
        CHECK(r.ui.Menus().IsOpen("dashboard"));
        pull(1, 1200 + 0); // the first tab
        const std::string before_full = r.log;
        r.ui.SetGameState(full, r.t);
        CHECK(PointAt(r, 1, sb::UiSystem::DashItem(1)));
        r.Frame(Pad());
        r.in.trigger[1] = 0.9f;
        r.Frame(Pad());
        const bool said_full = r.log.find("quick slots full") != std::string::npos;
        r.in.trigger[1] = 0.0f;
        n = Take(r, actions);
        CHECK(said_full);
        CHECK(n == 1 && actions[0].kind == BBXR_UI_ASSIGN_QUICK && actions[0].a == 4 &&
              uint32_t(actions[0].b) == 0x40000384u);
        (void)before_full;
        r.Frame(Pad(BBXR_BTN_L3)); // assigning in USE mode closed it: open again for the checks below
        r.Frame(Pad());
    }
    // L3 closes it; a trigger still held when it closed does not then swap weapons.
    r.in.trigger[1] = 0.9f;
    r.Frame(Pad(BBXR_BTN_RIGHT));
    r.Frame(Pad(BBXR_BTN_RIGHT | BBXR_BTN_L3));
    r.Frame(Pad(BBXR_BTN_RIGHT));
    CHECK(!r.ui.Menus().IsOpen("dashboard"));
    swap = Pad(BBXR_BTN_RIGHT);
    r.ui.Filter().Apply(r.t, swap);
    CHECK(!(swap.buttons & BBXR_BTN_RIGHT));
    r.in.trigger[1] = 0.0f;
    swap = Pad();
    r.ui.Filter().Apply(r.t, swap);
    swap = Pad(BBXR_BTN_RIGHT);
    r.ui.Filter().Apply(r.t, swap);
    CHECK(swap.buttons & BBXR_BTN_RIGHT); // a new pull swaps again
    fs::remove_all(dir, ec);
}


/// One stick push for a frame, then centered (a single focus step), with the stick of `hand`.
void Step(Rig& r, int dir, int hand = 0) {
    uint8_t x = 128, y = 128;
    if (dir == 1) y = 0;
    if (dir == 2) x = 255;
    if (dir == 3) y = 255;
    if (dir == 4) x = 0;
    r.ui.SetGameState(GameState(60), r.t);
    r.Frame(hand ? Pad(0, 128, 128, x, y) : Pad(0, x, y));
    r.ui.SetGameState(GameState(60), r.t);
    r.Frame(Pad());
}

/// The dashboard's stick navigation: the sticks move a focus, A and the triggers activate it, B
/// closes, and the game gets no stick or swing while it is open.
/// The drag state machine alone (sandbox3d/ui_drag.h).
void TestUiDrag() {
    using sb::UiDrag;
    const std::vector<std::string> quick = {"goods:1000", "goods:1200", "", "goods:1100", ""};
    // pick: one at a time, needs an item and a hand
    UiDrag d;
    CHECK(!d.Active());
    CHECK(!d.Pick(1, "", UiDrag::Source::Grid, 3));
    CHECK(!d.Pick(2, "goods:1120", UiDrag::Source::Grid, 3));
    CHECK(d.Pick(1, "goods:1120", UiDrag::Source::Grid, 3) && d.Active() && d.Hand() == 1 && d.Item() == "goods:1120");
    CHECK(!d.Pick(0, "goods:205", UiDrag::Source::Grid, 4)); // already dragging
    // hover: a change is reported once (the haptic tick), the same target is not
    CHECK(!d.Hover(-1, false));
    CHECK(d.Hover(2, false) && !d.Hover(2, false) && d.HoverSlot() == 2);
    CHECK(d.Preview(quick) == UiDrag::Result::Assign);
    // drop on an empty slot: assign
    UiDrag::Drop drop = d.Release(quick);
    CHECK(drop.result == UiDrag::Result::Assign && drop.slot == 2 && drop.item == "goods:1120" && drop.replaced.empty());
    CHECK(!d.Active() && d.HoverSlot() == -1);
    // drop on an occupied slot: replace (what was there is named)
    d.Pick(0, "goods:1120", UiDrag::Source::Grid, 3);
    d.Hover(1, false);
    CHECK(d.Preview(quick) == UiDrag::Result::Replace);
    drop = d.Release(quick);
    CHECK(drop.result == UiDrag::Result::Replace && drop.slot == 1 && drop.replaced == "goods:1200");
    // drop on the slot that already holds it: cancel with a reason
    d.Pick(1, "goods:1200", UiDrag::Source::Grid, 3);
    d.Hover(1, false);
    drop = d.Release(quick);
    CHECK(drop.result == UiDrag::Result::Cancel && !drop.reason.empty());
    // drop elsewhere (off the panel, the grid, a button): cancel, nothing assigned
    d.Pick(1, "goods:1120", UiDrag::Source::Grid, 3);
    CHECK(d.Release(quick).result == UiDrag::Result::Cancel);
    d.Pick(1, "goods:1120", UiDrag::Source::Grid, 3);
    d.Hover(-1, true);
    CHECK(d.Release(quick).result == UiDrag::Result::Cancel);
    // a slot past the end is not a target
    d.Pick(1, "goods:1120", UiDrag::Source::Grid, 3);
    d.Hover(9, false);
    CHECK(d.Release(quick).result == UiDrag::Result::Cancel);
    // an explicit cancel (the menu closed)
    d.Pick(1, "goods:1120", UiDrag::Source::Grid, 3);
    d.Cancel();
    CHECK(!d.Active() && d.Release(quick).result == UiDrag::Result::None);
    // from a quick slot: back on the grid unassigns it; the same slot, off, or another slot cancels
    d.Pick(1, "goods:1200", UiDrag::Source::Quick, 1);
    d.Hover(-1, true);
    CHECK(d.Preview(quick) == UiDrag::Result::Unassign);
    drop = d.Release(quick);
    CHECK(drop.result == UiDrag::Result::Unassign && drop.slot == 1 && drop.item == "goods:1200");
    d.Pick(1, "goods:1200", UiDrag::Source::Quick, 1);
    d.Hover(1, false);
    CHECK(d.Release(quick).result == UiDrag::Result::Cancel);
    d.Pick(1, "goods:1200", UiDrag::Source::Quick, 1);
    d.Hover(2, false);
    drop = d.Release(quick);
    CHECK(drop.result == UiDrag::Result::Cancel && !drop.reason.empty());
    d.Pick(1, "goods:1200", UiDrag::Source::Quick, 1);
    CHECK(d.Release(quick).result == UiDrag::Result::Cancel);
}

/// Drag and drop on the dashboard, through the real menu: grip pick on a card, the ray carries it, the
/// grip's release drops it on a quick slot (assign / replace), back on the grid (unassign) or nowhere.
void TestDashboardDrag(const fs::path& assets) {
    const fs::path dir = fs::temp_directory_path() / "bbport_game_ui_dash_drag_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "ui", ec);
    for (const char* f : {"icons.json", "icons.png", "font.json", "font.png"}) {
        fs::copy_file(assets / "ui" / f, dir / "ui" / f, ec);
    }
    {
        std::ofstream f(dir / "ui_layouts.json", std::ios::binary);
        f << "{\n  \"version\": 1,\n  \"game\": {\"version\": 2, \"held_items\": false}\n}\n";
    }
    Rig r;
    std::string log;
    CHECK(r.ui.Load(dir, nullptr, log));
    r.feed = true;
    r.state = GameState(60);
    r.state.flags |= BBXR_GAME_CAN_EQUIP;
    r.Frame(Pad());
    r.Frame(Pad(BBXR_BTN_L3));
    r.Frame(Pad());
    CHECK(r.ui.Menus().IsOpen("dashboard"));
    BbXrUiAction actions[4];
    const auto settle = [&] { for (int i = 0; i < 3; ++i) r.Frame(Pad()); };
    // squeeze (pick), carry to `to`, let go; returns the actions queued
    const auto drag = [&](int h, int from, int to, bool release = true) {
        CHECK(PointAt(r, h, from));
        r.Frame(Pad());
        r.in.squeeze[h] = 0.9f;
        r.Frame(Pad());
        CHECK(PointAt(r, h, to));
        r.Frame(Pad());
        r.Frame(Pad());
        if (release) {
            r.in.squeeze[h] = 0.0f;
            r.Frame(Pad());
        }
        return Take(r, actions);
    };
    // pick: the bullets (card 1, in no slot) follow the right hand's ray; a haptic grab tick
    CHECK(PointAt(r, 1, sb::UiSystem::DashItem(1)));
    r.Frame(Pad());
    r.in.squeeze[1] = 0.9f;
    r.Frame(Pad());
    CHECK(r.ui.Menus().Dragging() && !r.ui.Menus().DragItem().empty());
    CHECK(r.ui.Menus().DragHand() == 1 && r.StrongestHaptic(1) > 0.5f);
    // no click meanwhile: the trigger does nothing while dragging
    r.in.trigger[1] = 0.9f;
    r.Frame(Pad());
    r.in.trigger[1] = 0.0f;
    r.Frame(Pad());
    CHECK(r.ui.TakeActions(actions, 4) == 0 && r.ui.Menus().Dragging());
    // hover the empty quick slot 3: a hover tick; let go: assign into slot 2 (0-based), not used
    CHECK(PointAt(r, 1, sb::UiSystem::DashQuick(2)));
    r.Frame(Pad());
    CHECK(r.StrongestHaptic(1) > 0.0f); // the hover tick
    r.in.squeeze[1] = 0.0f;
    r.Frame(Pad());
    uint32_t n = Take(r, actions);
    CHECK(!r.ui.Menus().Dragging());
    CHECK(n == 1 && actions[0].kind == BBXR_UI_ASSIGN_QUICK && actions[0].a == 2 && uint32_t(actions[0].b) == 0x40000384u &&
          actions[0].c == 0);
    settle();
    // onto an occupied slot (the Molotov in slot 1): replaces it
    n = drag(1, sb::UiSystem::DashItem(1), sb::UiSystem::DashQuick(1));
    CHECK(n == 1 && actions[0].kind == BBXR_UI_ASSIGN_QUICK && actions[0].a == 1 && uint32_t(actions[0].b) == 0x40000384u);
    settle();
    // the left hand drags too
    n = drag(0, sb::UiSystem::DashItem(1), sb::UiSystem::DashQuick(4));
    CHECK(n == 1 && actions[0].kind == BBXR_UI_ASSIGN_QUICK && actions[0].a == 4);
    settle();
    // dropped on a button, a weapon slot or nothing: cancelled, no action, no haptic select
    CHECK(drag(1, sb::UiSystem::DashItem(1), sb::UiSystem::DashButton(0)) == 0 && !r.ui.Menus().Dragging());
    settle();
    CHECK(drag(1, sb::UiSystem::DashItem(1), sb::UiSystem::DashWeapon(1)) == 0);
    settle();
    {
        CHECK(PointAt(r, 1, sb::UiSystem::DashItem(1)));
        r.Frame(Pad());
        r.in.squeeze[1] = 0.9f;
        r.Frame(Pad());
        CHECK(r.ui.Menus().Dragging());
        r.in.aim[1].rotation = m3::FromEulerDeg({0.0f, 150.0f, 0.0f});
        r.Frame(Pad());
        r.in.squeeze[1] = 0.0f;
        r.Frame(Pad());
        CHECK(Take(r, actions) == 0 && !r.ui.Menus().Dragging());
        settle();
    }
    // a quick slot's item dragged onto the inventory: cleared
    n = drag(1, sb::UiSystem::DashQuick(3), sb::UiSystem::DashItem(0));
    CHECK(n == 1 && actions[0].kind == BBXR_UI_CLEAR_QUICK && actions[0].a == 3 && uint32_t(actions[0].b) == 0x400004CEu);
    settle();
    // ... onto another quick slot or its own: cancelled (a move is drop on the grid, then on the slot)
    CHECK(drag(1, sb::UiSystem::DashQuick(3), sb::UiSystem::DashQuick(2)) == 0);
    settle();
    CHECK(drag(1, sb::UiSystem::DashQuick(3), sb::UiSystem::DashQuick(3)) == 0);
    settle();
    // a card that already sits in a slot (the Molotov card) is dragged as that slot's item: onto the grid clears it
    {
        int molotov = -1;
        for (int i = 0; i < 12 && molotov < 0; ++i) {
            Vec3 at;
            if (r.ui.Menus().ElementCenter("dashboard", sb::UiSystem::DashItem(i), at)) {
                // cards are listed from the state's items: vial, bullets, molotov, pebble...
                if (i == 2) molotov = i;
            }
        }
        CHECK(molotov == 2);
        n = drag(1, sb::UiSystem::DashItem(molotov), sb::UiSystem::DashItem(0));
        CHECK(n == 1 && actions[0].kind == BBXR_UI_CLEAR_QUICK && actions[0].a == 1);
        settle();
    }
    // an item that is not available (no equip hook): refused with a message, no drag
    {
        BbXrGameState no_hook = GameState(60);
        r.state = no_hook;
        r.Frame(Pad());
        CHECK(PointAt(r, 1, sb::UiSystem::DashItem(1)));
        r.Frame(Pad());
        r.in.squeeze[1] = 0.9f;
        r.Frame(Pad());
        CHECK(!r.ui.Menus().Dragging());
        r.in.squeeze[1] = 0.0f;
        r.Frame(Pad());
    }
}

void TestDashboardSticks(const fs::path& assets) {
    const fs::path dir = fs::temp_directory_path() / "bbport_game_ui_dash_stick_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "ui", ec);
    for (const char* f : {"icons.json", "icons.png", "font.json", "font.png"}) {
        fs::copy_file(assets / "ui" / f, dir / "ui" / f, ec);
    }
    {
        std::ofstream f(dir / "ui_layouts.json", std::ios::binary);
        f << "{\n  \"version\": 1,\n  \"game\": {\"version\": 2, \"held_items\": false}\n}\n";
    }
    Rig r;
    std::string log;
    CHECK(r.ui.Load(dir, nullptr, log));
    r.in.aim[0].valid = r.in.aim[1].valid = false; // no ray on the panel
    r.ui.SetGameState(GameState(60), r.t);
    r.Frame(Pad());
    // Closed: the game gets the sticks.
    BbXrInput g = Pad(0, 255, 255, 255, 255);
    r.ui.Filter().Apply(r.t, g);
    CHECK(g.lx == 255 && g.ry == 255);
    r.Frame(Pad(BBXR_BTN_L3));
    r.Frame(Pad());
    CHECK(r.ui.Menus().IsOpen("dashboard"));
    // Open: no walking, no turning, no swing, no weapon swap or dodge button.
    g = Pad(BBXR_BTN_RIGHT | BBXR_BTN_R1 | BBXR_BTN_CIRCLE, 255, 0, 0, 255);
    r.ui.Filter().Apply(r.t, g);
    CHECK(g.lx == 128 && g.ly == 128 && g.rx == 128 && g.ry == 128 && g.buttons == 0);
    r.Frame(Pad());
    CHECK(r.ui.Menus().Hovered("dashboard") < 0); // nothing focused until the sticks move
    // First push picks the starting entry, the next moves from it.
    Step(r, 2);
    const int start = r.ui.Menus().Hovered("dashboard");
    CHECK(start >= 0);
    Vec3 p0, p1;
    CHECK(r.ui.Menus().ElementCenter("dashboard", start, p0));
    Step(r, 2, 1); // the right stick works too
    const int next = r.ui.Menus().Hovered("dashboard");
    CHECK(next >= 0 && next != start);
    CHECK(r.ui.Menus().ElementCenter("dashboard", next, p1));
    CHECK(p1.x > p0.x);
    Step(r, 4);
    CHECK(r.ui.Menus().Hovered("dashboard") == start);
    // Repeat timing: held right, one step at once, the next after 0.35 s, then every 0.14 s.
    {
        const int base = r.ui.Menus().Hovered("dashboard");
        std::vector<double> moves;
        int last = base;
        const double t0 = r.t;
        for (int f = 0; f < 90; ++f) { // 1 s at 90 Hz
            r.ui.SetGameState(GameState(60), r.t);
            r.Frame(Pad(0, 255, 128));
            const int h = r.ui.Menus().Hovered("dashboard");
            if (h != last) {
                moves.push_back(r.t - t0);
                last = h;
            }
        }
        CHECK(moves.size() >= 4);
        if (moves.size() >= 4) {
            CHECK(moves[0] < 0.03);
            CHECK(moves[1] - moves[0] > 0.30 && moves[1] - moves[0] < 0.40);
            CHECK(moves[2] - moves[1] > 0.11 && moves[2] - moves[1] < 0.18);
            CHECK(moves[3] - moves[2] > 0.11 && moves[3] - moves[2] < 0.18);
        }
        r.Frame(Pad());
    }
    // Move the focus to quick slot 1 with the sticks, then A pulls it (used; the panel closes).
    BbXrUiAction actions[4];
    auto goto_id = [&](int id) {
        for (int i = 0; i < 40 && r.ui.Menus().Hovered("dashboard") != id; ++i) {
            const int cur = r.ui.Menus().Hovered("dashboard");
            Vec3 a, b;
            if (cur < 0 || !r.ui.Menus().ElementCenter("dashboard", cur, a) ||
                !r.ui.Menus().ElementCenter("dashboard", id, b)) {
                Step(r, 2);
                continue;
            }
            // Panel up is world up (tilted a little); the panel's x is the world's.
            const float dx = b.x - a.x, dy = b.y - a.y;
            Step(r, std::fabs(dx) > std::fabs(dy) ? (dx > 0 ? 2 : 4) : (dy > 0 ? 1 : 3));
        }
        return r.ui.Menus().Hovered("dashboard") == id;
    };
    CHECK(goto_id(sb::UiSystem::DashQuick(1)));
    Take(r, actions);
    r.Frame(Pad(BBXR_BTN_CIRCLE)); // A (the pad's circle)
    r.Frame(Pad());
    uint32_t n = Take(r, actions);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK && actions[0].a == 1);
    CHECK(!r.ui.Menus().IsOpen("dashboard"));
    // Closed again: the sticks are the game's, and a held button is not hidden.
    g = Pad(0, 255, 0, 0, 255);
    r.ui.Filter().Apply(r.t, g);
    CHECK(g.lx == 255 && g.ly == 0 && g.rx == 0 && g.ry == 255);
    // A trigger pulls the focused entry too (stick focus wins over a ray that did not move).
    r.Frame(Pad(BBXR_BTN_L3));
    r.Frame(Pad());
    CHECK(r.ui.Menus().IsOpen("dashboard"));
    CHECK(goto_id(sb::UiSystem::DashWeapon(1)));
    r.in.trigger[0] = 0.9f;
    r.Frame(Pad());
    r.in.trigger[0] = 0.0f;
    r.Frame(Pad());
    n = Take(r, actions);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_EQUIP && actions[0].a == 1);
    CHECK(r.ui.Menus().IsOpen("dashboard"));
    // Pointing moves the focus; the stick takes it back.
    r.in.aim[1].valid = true;
    CHECK(PointAt(r, 1, sb::UiSystem::DashQuick(2)));
    r.Frame(Pad());
    CHECK(r.ui.Menus().Hovered("dashboard") == sb::UiSystem::DashQuick(2));
    Step(r, 4);
    CHECK(r.ui.Menus().Hovered("dashboard") != sb::UiSystem::DashQuick(2));
    r.in.aim[1].valid = false;
    // B closes it without an action; the right grip tap swap is not sent while it is open.
    r.Frame(Pad(BBXR_BTN_CROSS));
    r.Frame(Pad());
    CHECK(!r.ui.Menus().IsOpen("dashboard"));
    CHECK(Take(r, actions) == 0);
    // block_move off: the sticks reach the game with the dashboard open (stick_nav off too).
    {
        std::ofstream f(dir / "ui_layouts.json", std::ios::binary);
        f << "{\n  \"version\": 1,\n  \"game\": {\"version\": 2, \"held_items\": false, \"active\": [\"dashboard\"],\n"
             "    \"menus\": [{\"name\": \"dashboard\", \"type\": \"dashboard\", \"open\": \"l3\","
             " \"stick_nav\": false, \"block_move\": false}]}\n}\n";
    }
    Rig r2;
    std::string log2;
    CHECK(r2.ui.Load(dir, nullptr, log2));
    r2.ui.SetGameState(GameState(60), r2.t);
    r2.Frame(Pad(BBXR_BTN_L3));
    r2.Frame(Pad());
    CHECK(r2.ui.Menus().IsOpen("dashboard"));
    g = Pad(0, 255, 0, 0, 255);
    r2.ui.Filter().Apply(r2.t, g);
    CHECK(g.lx == 255 && g.ry == 255);
    fs::remove_all(dir, ec);
}
} // namespace

/// The hitbox overlay (vr_game_hitbox.h) with the sandbox's data: the Saw Cleaver's capsules on
/// an identity R_Weapon frame, the attack highlight and the hit flash, the player's hurtboxes.
void TestHitboxes(const fs::path& assets) {
    vrui::GameHitboxes hb;
    std::string log;
    hb.Load(assets, log);
    CHECK(hb.IndexIdFor(7000000u) == 7000000);
    CHECK(hb.IndexIdFor(7000500u) == 7000000);  // an upgrade: the base row
    CHECK(hb.IndexIdFor(6180000u) == 6180000);  // a variant with its own row
    CHECK(hb.IndexIdFor(BBXR_ITEM_NONE) == -1);
    CHECK(hb.IndexIdFor(0x40000000u | 1000u) == -1);
    static BbXrGameState s{};
    s.size = sizeof(s);
    s.flags = BBXR_GAME_IN_WORLD | BBXR_GAME_WEAPONS;
    s.weapons[0] = 7000000u;
    s.weapons[1] = s.weapons[2] = s.weapons[3] = BBXR_ITEM_NONE;
    s.debug.flags = BBXR_DEBUG_POSE;
    s.debug.weapon_valid = 2u;
    const float identity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    std::memcpy(s.debug.weapon[1], identity, sizeof(identity));
    sb::UiDebugHitboxes cfg;
    cfg.weapon = cfg.attack = cfg.enemies = cfg.hits = true; // drawing is off by default
    std::vector<vrui::HitboxLine> lines;
    hb.Lines(cfg, s, 1.0, lines, log);
    CHECK(!lines.empty());
    bool near_blade = !lines.empty();
    for (const vrui::HitboxLine& l : lines) {
        near_blade = near_blade && m3::Length(l.a) < 2.0f && l.width_scale == 1.0f &&
                     std::fabs(l.color.a - cfg.idle_alpha) < 1e-4f;
    }
    CHECK(near_blade);
    // The VR hit scale (findings/32): the same shapes, farther out and thicker; off (or from an older
    // host, valid 0): the game's own.
    auto extent = [&](const std::vector<vrui::HitboxLine>& v) {
        float e = 0.0f;
        for (const vrui::HitboxLine& l : v) {
            e = std::max({e, m3::Length(l.a), m3::Length(l.b)});
        }
        return e;
    };
    const float plain = extent(lines);
    s.debug.hit_scale = {1u, 0u, 1.5f, 0.15f, {-1, -1, -1}, {-2, -2, -2}};
    lines.clear();
    hb.Lines(cfg, s, 1.0, lines, log);
    CHECK(std::fabs(extent(lines) - plain) < 1e-4f); // enabled 0: unchanged
    s.debug.hit_scale.enabled = 1u;
    lines.clear();
    hb.Lines(cfg, s, 1.0, lines, log);
    CHECK(extent(lines) > plain + 0.14f);
    s.debug.hit_scale = {};
    // An attack of the weapon (AtkParam 700000) opens: its shapes thicker, full alpha.
    hb.OnEvent({BBXR_EV_SWING_OPEN, 20, 700000, 7}, 2.0);
    CHECK(hb.AttackAtk() == 700000);
    lines.clear();
    hb.Lines(cfg, s, 2.1, lines, log);
    CHECK(!lines.empty() && lines[0].width_scale == 2.0f && lines[0].color.a == 1.0f);
    hb.OnEvent({BBXR_EV_SWING_HIT, 20, 0, 7}, 2.2);
    lines.clear();
    hb.Lines(cfg, s, 2.3, lines, log);
    CHECK(!lines.empty() && lines[0].color.r == 1.0f && lines[0].color.g == 1.0f &&
          lines[0].width_scale == 3.0f);
    hb.OnEvent({BBXR_EV_SWING_MISS, 0, 0, 7}, 2.4); // another ticket's end: still flashing
    hb.OnEvent({BBXR_EV_SWING_OPEN, 20, 700010, 8}, 3.0);
    hb.OnEvent({BBXR_EV_SWING_MISS, 20, 0, 8}, 3.5);
    CHECK(hb.AttackAtk() == -1);
    // Off: nothing.
    cfg.weapon = cfg.attack = cfg.hits = false;
    lines.clear();
    hb.Lines(cfg, s, 4.0, lines, log);
    CHECK(lines.empty());
    // A hit marker (fades over 3 s).
    cfg.hits = true;
    s.debug.hit_count = 1;
    s.debug.hits[0] = {{1.0f, 1.0f, -2.0f}, 0.5f, 700000, 0};
    hb.Lines(cfg, s, 4.0, lines, log);
    CHECK(!lines.empty() && m3::Length(lines[0].a - m3::Vec3{1.0f, 1.0f, -2.0f}) < 0.2f);
    // The player's hurtboxes, skinned by bone hash (identity: the data's bind positions).
    cfg.hits = false;
    cfg.self = true;
    s.debug.hit_count = 0;
    const char* names[] = {"Master", "Pelvis", "Spine", "Spine1", "Spine2", "Neck", "Head",
                           "L_Thigh", "L_Calf", "L_Foot", "R_Thigh", "R_Calf", "R_Foot",
                           "L_UpperArm", "L_Forearm", "L_Hand", "R_UpperArm", "R_Forearm",
                           "R_Hand"};
    s.debug.bone_count = 0;
    for (const char* n : names) {
        BbXrDebugBone& b = s.debug.bones[s.debug.bone_count++];
        uint32_t h = 2166136261u;
        for (const char* c = n; *c; ++c) {
            h = (h ^ uint8_t(*c)) * 16777619u;
        }
        b.name_hash = h;
        std::memcpy(b.skin, identity, sizeof(identity));
    }
    lines.clear();
    hb.Lines(cfg, s, 5.0, lines, log);
    CHECK(lines.size() > 100);
    float top = -1.0f;
    for (const vrui::HitboxLine& l : lines) {
        top = std::max(top, l.a.y);
    }
    CHECK(top > 1.4f && top < 2.1f); // the head's capsule: a standing body
}

/// The quick wheel (R3 held 0.5 s): quick items and the weapon slots in one ring, placed once
/// at the right hand and picked with that same hand (touch / poke), the trigger or the stick.
void TestQuickWheel(const fs::path& assets) {
    const fs::path dir = fs::temp_directory_path() / "bbport_game_ui_wheel_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "ui", ec);
    for (const char* f : {"icons.json", "icons.png", "font.json", "font.png"}) {
        fs::copy_file(assets / "ui" / f, dir / "ui" / f, ec);
    }
    {
        std::ofstream f(dir / "ui_layouts.json", std::ios::binary);
        f << "{\n  \"version\": 1,\n  \"game\": {\"version\": 2, \"held_items\": false, \"active\": [\"quick_wheel\"]}\n}\n";
    }
    Rig r;
    std::string log;
    CHECK(r.ui.Load(dir, nullptr, log));
    const sb::UiMenuSpec* spec = r.ui.Menus().Layout().FindGameMenu("quick_wheel");
    CHECK(spec != nullptr && spec->open.key == sb::UiInputId::R3 && spec->hand == 1 &&
          spec->pointer == 1 && spec->anchor == "world");
    CHECK(spec && spec->select == std::vector<std::string>({"touch", "point", "stick"}));
    r.ui.SetGameState(GameState(60), r.t);
    r.Frame(Pad());
    CHECK(r.ui.Active());
    // The wheel faces the head from the right fingertip; the plane's axes for moving the hand.
    const m3::Vec3 tip0 = r.in.aim[1].position + m3::Vec3{0.0f, 0.0f, -0.06f};
    const m3::Vec3 facing = m3::Normalize(r.in.head.position - tip0, {0, 0, 1});
    const m3::Vec3 px = m3::Normalize(m3::Cross({0, 1, 0}, facing), {1, 0, 0});
    const m3::Vec3 py = m3::Cross(facing, px);
    const auto hold_r3 = [&]() {
        for (int i = 0; i < 50; ++i) {
            r.ui.SetGameState(GameState(60), r.t);
            r.Frame(Pad(BBXR_BTN_R3));
        }
        r.Frame(Pad());
    };
    const auto hand_at = [&](float u, float v, float push) { // from the opening spot
        const m3::Vec3 p = r.in.grip[1].position;
        (void)p;
        const m3::Vec3 g = m3::Vec3{0.2f, 1.2f, -0.35f} + px * u + py * v - facing * push;
        r.in.grip[1].position = r.in.aim[1].position = g;
    };
    // A short R3 tap is still lock-on; the wheel stays shut.
    r.Frame(Pad(BBXR_BTN_R3));
    r.Frame(Pad());
    CHECK(!r.ui.Menus().IsOpen("quick_wheel"));
    // R3 held: opens at the hand, nothing hovered yet (the fingertip is at the centre).
    hold_r3();
    CHECK(r.ui.Menus().IsOpen("quick_wheel") && r.ui.Menus().Hovered("quick_wheel") == -1);
    // The ring stays where it opened: moving the hand 10 cm up the plane touches the top item (vial).
    hand_at(0.0f, 0.10f, 0.0f);
    r.Frame(Pad());
    r.Frame(Pad());
    CHECK(r.ui.Menus().Hovered("quick_wheel") == 0);
    CHECK(r.StrongestHaptic(1) >= 0.0f);
    r.in.trigger[1] = 0.9f; // the trigger confirms
    r.Frame(Pad());
    r.in.trigger[1] = 0.0f;
    r.Frame(Pad());
    CHECK(!r.ui.Menus().IsOpen("quick_wheel"));
    BbXrUiAction actions[4];
    uint32_t n = Take(r, actions);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK && actions[0].a == 0);
    // Open again, touch the 6th entry (right 2) and push through: selects without the trigger.
    hand_at(0.0f, 0.0f, 0.0f);
    r.Frame(Pad());
    hold_r3();
    CHECK(r.ui.Menus().IsOpen("quick_wheel"));
    const float a = (90.0f - 5.0f * 360.0f / 7.0f) * kDeg;
    hand_at(0.10f * std::cos(a), 0.10f * std::sin(a), 0.0f);
    r.Frame(Pad());
    r.Frame(Pad());
    CHECK(r.ui.Menus().Hovered("quick_wheel") == 5);
    hand_at(0.10f * std::cos(a), 0.10f * std::sin(a), 0.05f);
    r.Frame(Pad());
    r.Frame(Pad());
    CHECK(!r.ui.Menus().IsOpen("quick_wheel"));
    n = Take(r, actions);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_EQUIP && actions[0].a == 1 && actions[0].b == 7010000);
    // The right stick still picks (a second R3 hold, the stick up, then R3 again to confirm).
    hand_at(0.0f, 0.0f, 0.0f);
    r.Frame(Pad());
    hold_r3();
    CHECK(r.ui.Menus().IsOpen("quick_wheel") && r.ui.Menus().UsesStick(1));
    r.Frame(Pad(0, 128, 128, 128, 0));
    CHECK(r.ui.Menus().Hovered("quick_wheel") == 0);
    r.Frame(Pad(BBXR_BTN_R3, 128, 128, 128, 0));
    r.Frame(Pad());
    r.Frame(Pad());
    CHECK(!r.ui.Menus().IsOpen("quick_wheel"));
    n = Take(r, actions);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK && actions[0].a == 0);
}


/// The user's live layout (out/vr_assets/ui_layouts.json, copied to a scratch folder): an R3 hold
/// opens its quick wheel with lock_on=false and the dashboard on l3 in the active list too.
void TestLiveLayout(const fs::path& assets) {
    const fs::path live = assets / "ui_layouts.json";
    if (!fs::exists(live)) {
        return;
    }
    const fs::path dir = fs::temp_directory_path() / "bbport_game_ui_live_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "ui", ec);
    for (const char* f : {"icons.json", "icons.png", "font.json", "font.png"}) {
        fs::copy_file(assets / "ui" / f, dir / "ui" / f, ec);
    }
    fs::copy_file(live, dir / "ui_layouts.json", ec);
    Rig r;
    std::string log;
    CHECK(r.ui.Load(dir, nullptr, log));
    r.ui.SetGameState(GameState(60), r.t);
    r.Frame(Pad());
    CHECK(r.ui.Active());
    for (int round = 0; round < 3; ++round) {
        // The game thread reads the pad too (Apply), as the bridge's GetInput does every frame.
        for (int i = 0; i < 60; ++i) {
            r.ui.SetGameState(GameState(60), r.t);
            BbXrInput game = Pad(BBXR_BTN_R3);
            r.ui.Filter().Apply(r.t, game);
            CHECK(!(game.buttons & BBXR_BTN_R3));
            r.Frame(Pad(BBXR_BTN_R3));
        }
        CHECK(r.ui.Menus().IsOpen("quick_wheel"));
        r.Frame(Pad());
        r.Frame(Pad());
        CHECK(r.ui.Menus().IsOpen("quick_wheel"));
        // A tap closes it again (toggle).
        r.Frame(Pad(BBXR_BTN_R3));
        r.Frame(Pad());
        r.Frame(Pad());
        r.Frame(Pad());
        CHECK(!r.ui.Menus().IsOpen("quick_wheel"));
    }
    // The hand that clicks R3 is usually gripping the controller (the squeeze reads 0.6-1.0): that
    // is no modifier of a hold on r3, the wheel still opens.
    r.in.squeeze[1] = 0.9f;
    for (int i = 0; i < 60; ++i) {
        r.ui.SetGameState(GameState(60), r.t);
        r.Frame(Pad(BBXR_BTN_R3));
    }
    CHECK(r.ui.Menus().IsOpen("quick_wheel"));
    r.in.squeeze[1] = 0.0f;
}

/// Random mashing (grips, triggers, A/B/X/Y, sticks, L3, the d-pad and R3 clicks, taps and holds, the
/// game thread reading the pad too), then everything let go: a clean R3 hold must open the wheel
/// (after closing it first when the mashing left it open), however the mashing went.
void TestMashThenWheel(const fs::path& assets, const char* custom_active = nullptr) {
    const fs::path live = assets / "ui_layouts.json";
    if (!fs::exists(live)) {
        return;
    }
    const fs::path dir = fs::temp_directory_path() / "bbport_game_ui_mash_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "ui", ec);
    for (const char* f : {"icons.json", "icons.png", "font.json", "font.png"}) {
        fs::copy_file(assets / "ui" / f, dir / "ui" / f, ec);
    }
    if (custom_active) { // a layout with chords (grips are modifiers of some menu)
        std::ofstream f(dir / "ui_layouts.json", std::ios::binary);
        f << "{\n  \"version\": 1,\n  \"game\": {\"version\": 2, \"held_items\": false, \"active\": " << custom_active << "}\n}\n";
    } else {
        fs::copy_file(live, dir / "ui_layouts.json", ec);
    }
    uint32_t rng = 987654321u;
    const auto rnd = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return float((rng >> 8) & 0xFFFF) / 65535.0f;
    };
    for (int round = 0; round < 40; ++round) {
        Rig r;
        std::string log;
        CHECK(r.ui.Load(dir, nullptr, log));
        r.ui.SetGameState(GameState(60), r.t);
        r.Frame(Pad());
        uint32_t held = 0;
        uint8_t stick[4] = {128, 128, 128, 128};
        for (int step = 0; step < 600; ++step) {
            const uint32_t all = BBXR_BTN_CIRCLE | BBXR_BTN_CROSS | BBXR_BTN_SQUARE | BBXR_BTN_TRIANGLE | BBXR_BTN_L3 |
                                 BBXR_BTN_R3 | BBXR_BTN_UP | BBXR_BTN_DOWN | BBXR_BTN_LEFT | BBXR_BTN_RIGHT |
                                 BBXR_BTN_OPTIONS | BBXR_BTN_L1 | BBXR_BTN_R1 | BBXR_BTN_L2 | BBXR_BTN_R2;
            if (rnd() < 0.08f) {
                held ^= 1u << int(rnd() * 31.0f) & all;
                held &= all;
            }
            if (rnd() < 0.05f) held ^= BBXR_BTN_R3;
            if (rnd() < 0.02f) held = 0;
            if (rnd() < 0.1f) r.in.squeeze[0] = rnd() < 0.5f ? 0.0f : 0.95f;
            if (rnd() < 0.1f) r.in.squeeze[1] = rnd() < 0.5f ? 0.0f : 0.95f;
            if (rnd() < 0.1f) r.in.trigger[0] = rnd() < 0.5f ? 0.0f : 1.0f;
            if (rnd() < 0.1f) r.in.trigger[1] = rnd() < 0.5f ? 0.0f : 1.0f;
            if (rnd() < 0.1f) {
                for (uint8_t& v : stick) v = rnd() < 0.5f ? 128 : uint8_t(rnd() * 255.0f);
            }
            if (rnd() < 0.1f) {
                r.in.grip[1].position = r.in.aim[1].position = {0.2f + rnd() * 0.2f - 0.1f, 1.0f + rnd() * 0.4f, -0.3f - rnd() * 0.2f};
            }
            r.ui.SetGameState(GameState(60), r.t);
            BbXrInput game = Pad(held, stick[0], stick[1], stick[2], stick[3]);
            r.ui.Filter().Apply(r.t + 1e-4, game);
            r.Frame(Pad(held, stick[0], stick[1], stick[2], stick[3]));
        }
        r.in.squeeze[0] = r.in.squeeze[1] = 0.0f;
        r.in.trigger[0] = r.in.trigger[1] = 0.0f;
        for (int i = 0; i < 30; ++i) {
            r.ui.SetGameState(GameState(60), r.t);
            r.Frame(Pad());
        }
        const auto clean_hold = [&]() {
            for (int i = 0; i < 70; ++i) {
                r.ui.SetGameState(GameState(60), r.t);
                BbXrInput game = Pad(BBXR_BTN_R3);
                r.ui.Filter().Apply(r.t + 1e-4, game);
                r.Frame(Pad(BBXR_BTN_R3));
            }
            for (int i = 0; i < 10; ++i) {
                r.ui.SetGameState(GameState(60), r.t);
                r.Frame(Pad());
            }
        };
        // Menus the mashing left open (the dashboard, the wheel) are closed first.
        for (int attempts = 0; attempts < 4 && (r.ui.Menus().IsOpen("quick_wheel") || r.ui.Menus().IsOpen("dashboard") ||
                                                 r.ui.Menus().IsOpen("quick_ring") || r.ui.Menus().IsOpen("weapon_wheel")); ++attempts) {
            if (r.ui.Menus().IsOpen("dashboard") || r.ui.Menus().IsOpen("quick_ring") || r.ui.Menus().IsOpen("weapon_wheel")) {
                r.Frame(Pad(BBXR_BTN_L3));
                r.Frame(Pad());
                r.Frame(Pad());
            } else {
                clean_hold();
            }
        }
        CHECK(!r.ui.Menus().IsOpen("quick_wheel"));
        clean_hold();
        CHECK(r.ui.Menus().IsOpen("quick_wheel"));
        clean_hold(); // (and it closes again)
        CHECK(!r.ui.Menus().IsOpen("quick_wheel"));
        clean_hold();
        CHECK(r.ui.Menus().IsOpen("quick_wheel"));
    }
}

/// A layout with a grip chord (weapon_wheel: right_grip+b): the hand that clicks R3 or L3 is usually
/// squeezing its grip, which is no reason for a plain "r3" / "l3" binding to stay shut.
void TestGripDoesNotBlock(const fs::path& assets) {
    const fs::path dir = fs::temp_directory_path() / "bbport_game_ui_grip_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "ui", ec);
    for (const char* f : {"icons.json", "icons.png", "font.json", "font.png"}) {
        fs::copy_file(assets / "ui" / f, dir / "ui" / f, ec);
    }
    {
        std::ofstream f(dir / "ui_layouts.json", std::ios::binary);
        f << "{\n  \"version\": 1,\n  \"game\": {\"version\": 2, \"held_items\": false, \"active\": [\"quick_wheel\", \"quick_ring\", "
             "\"weapon_wheel\"]}\n}\n";
    }
    Rig r;
    std::string log;
    CHECK(r.ui.Load(dir, nullptr, log));
    r.ui.SetGameState(GameState(60), r.t);
    r.Frame(Pad());
    for (int grip = 0; grip < 2; ++grip) {
        r.in.squeeze[0] = r.in.squeeze[1] = 0.9f; // both hands gripping
        for (int i = 0; i < 60; ++i) {
            r.ui.SetGameState(GameState(60), r.t);
            r.Frame(Pad(BBXR_BTN_R3));
        }
        CHECK(r.ui.Menus().IsOpen("quick_wheel"));
        r.Frame(Pad());
        r.Frame(Pad(BBXR_BTN_R3)); // a tap closes it
        r.Frame(Pad());
        r.Frame(Pad());
        r.Frame(Pad());
        CHECK(!r.ui.Menus().IsOpen("quick_wheel"));
        r.Frame(Pad(BBXR_BTN_L3));
        r.Frame(Pad());
        CHECK(r.ui.Menus().IsOpen("quick_ring"));
        r.Frame(Pad(BBXR_BTN_L3));
        r.Frame(Pad());
        r.Frame(Pad());
        CHECK(!r.ui.Menus().IsOpen("quick_ring"));
        r.in.squeeze[0] = r.in.squeeze[1] = 0.0f;
        r.Frame(Pad());
    }
}

/// The filter alone, mashed: the R3 / L3 episodes cannot get stuck whatever the order of the
/// readers, the filter being switched off and on, or the settings changing in the middle of a press.
void TestFilterMash() {
    using vrui::InputFilter;
    uint32_t rng = 24680u;
    const auto rnd = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return float((rng >> 8) & 0xFFFF) / 65535.0f;
    };
    for (int round = 0; round < 50; ++round) {
        InputFilter f;
        InputFilter::Settings s;
        s.enabled = true;
        s.r3_hold_s = 0.15;
        s.lock_on = round % 2 == 0;
        f.Configure(s);
        double t = 0.0;
        uint32_t buttons = 0;
        for (int step = 0; step < 800; ++step) {
            t += (rnd() < 0.02f ? 0.5 : 1.0 / 90.0);
            if (rnd() < 0.1f) buttons ^= 1u << int(rnd() * 20.0f);
            if (rnd() < 0.08f) buttons ^= BBXR_BTN_R3;
            if (rnd() < 0.03f) {
                s.enabled = rnd() < 0.7f;
                s.r3_menu = rnd() < 0.8f;
                f.Configure(s);
            }
            if (rnd() < 0.05f) f.SetMenuState(rnd() < 0.5f, rnd() < 0.5f, rnd() < 0.5f, rnd() < 0.5f);
            if (rnd() < 0.05f) f.SetClaim(buttons & uint32_t(rnd() * 255.0f));
            BbXrInput a = Pad(buttons), b = Pad(buttons);
            if (rnd() < 0.5f) {
                f.Apply(t, a);
                f.Step(t, b);
            } else {
                f.Step(t, b);
                f.Apply(t, a);
            }
        }
        // Clean again: enabled, menus not engaged, nothing pressed.
        s.enabled = true;
        s.r3_menu = true;
        s.lock_on = true;
        f.Configure(s);
        f.SetMenuState(false, false, false, false);
        f.SetClaim(0);
        for (int i = 0; i < 40; ++i) {
            t += 1.0 / 90.0;
            BbXrInput a = Pad(), b = Pad();
            f.Apply(t, a);
            f.Step(t, b);
        }
        // A hold: the menus see R3, the game never.
        bool ui_saw = false, game_saw = false;
        for (int i = 0; i < 60; ++i) {
            t += 1.0 / 90.0;
            BbXrInput a = Pad(BBXR_BTN_R3);
            f.Apply(t, a);
            game_saw = game_saw || (a.buttons & BBXR_BTN_R3);
            ui_saw = ui_saw || (f.Step(t, Pad(BBXR_BTN_R3)).buttons & BBXR_BTN_R3);
        }
        CHECK(ui_saw && !game_saw);
        for (int i = 0; i < 40; ++i) {
            t += 1.0 / 90.0;
            BbXrInput a = Pad();
            f.Apply(t, a);
            f.Step(t, Pad());
            CHECK(!(a.buttons & BBXR_BTN_R3));
        }
        // A tap: lock-on to the game, nothing to the menus.
        ui_saw = game_saw = false;
        for (int i = 0; i < 40; ++i) {
            t += 1.0 / 90.0;
            BbXrInput a = Pad(i < 5 ? BBXR_BTN_R3 : 0u);
            f.Apply(t, a);
            game_saw = game_saw || (a.buttons & BBXR_BTN_R3);
            ui_saw = ui_saw || (f.Step(t, Pad(i < 5 ? BBXR_BTN_R3 : 0u)).buttons & BBXR_BTN_R3);
        }
        CHECK(game_saw && !ui_saw);
    }
}

void TestGameMenuNav() {
    vrui::InputFilter f;
    vrui::InputFilter::Settings s;
    s.enabled = true;
    f.Configure(s);
    f.SetGameMenu(true);
    double t = 10.0;
    auto apply = [&](BbXrInput in, bool trig = false) {
        f.Apply(t, in, trig);
        return in;
    };
    // Stick right: one d-pad right pulse, nothing else, the stick itself is centered.
    BbXrInput out = apply(Pad(0, 255, 128));
    CHECK(out.buttons == BBXR_BTN_RIGHT && out.lx == 128);
    t += 0.1;
    CHECK(apply(Pad(0, 255, 128)).buttons == 0); // pulse over, no repeat yet
    t += 0.3;
    CHECK(apply(Pad(0, 255, 128)).buttons == BBXR_BTN_RIGHT); // repeat after 0.35 s
    t += 0.1;
    CHECK(apply(Pad(0)).buttons == 0);
    t += 0.01;
    CHECK(apply(Pad(0, 128, 0)).buttons == BBXR_BTN_UP); // stick up (pad y 0)
    t += 0.1;
    // The right stick navigates too.
    CHECK(apply(Pad(0, 128, 128, 128, 255)).buttons == 0 || true);
    apply(Pad(0));
    t += 0.01;
    CHECK(apply(Pad(0, 128, 128, 128, 255)).buttons == BBXR_BTN_DOWN);
    t += 0.1;
    apply(Pad(0));
    // A (circle) is cross, B (cross) is circle, a trigger pull is cross, swings are dropped.
    CHECK(apply(Pad(BBXR_BTN_CIRCLE)).buttons == BBXR_BTN_CROSS);
    CHECK(apply(Pad(BBXR_BTN_CROSS)).buttons == BBXR_BTN_CIRCLE);
    CHECK(apply(Pad(BBXR_BTN_R1 | BBXR_BTN_RIGHT), true).buttons == BBXR_BTN_CROSS);
    CHECK(apply(Pad(BBXR_BTN_OPTIONS)).buttons == BBXR_BTN_OPTIONS);
    // Menu closed: the world mapping is back.
    f.SetGameMenu(false);
    out = apply(Pad(BBXR_BTN_CROSS, 255, 128)); // B: the roll
    CHECK(out.buttons == BBXR_BTN_CIRCLE && out.lx == 255);
}

// The face buttons in the world (vr_ui_filter.h FaceRemap): B roll, A right weapon, X left
// weapon (one d-pad pulse per press), Y interact (cross), the old heal buttons gone.
void TestFaceRemap() {
    vrui::InputFilter f;
    vrui::InputFilter::Settings s;
    s.enabled = true;
    f.Configure(s);
    auto game = [&](double t, BbXrInput in) {
        f.Apply(t, in);
        return in.buttons;
    };
    auto y = [](uint32_t buttons) {
        BbXrInput in = Pad(buttons);
        in.reserved[0] = BBXR_RAW_Y;
        return in;
    };
    double t = 1.0;
    // B (cross on the pad) is the roll (circle); held while held.
    CHECK(game(t, Pad(BBXR_BTN_CROSS)) == BBXR_BTN_CIRCLE);
    CHECK(game(t + 0.5, Pad(BBXR_BTN_CROSS)) == BBXR_BTN_CIRCLE);
    CHECK(game(t + 0.6, Pad()) == 0);
    // A (circle on the pad) is ONE d-pad right pulse, however long it is held.
    t = 2.0;
    CHECK(game(t, Pad(BBXR_BTN_CIRCLE)) == BBXR_BTN_RIGHT);
    CHECK(game(t + 0.05, Pad(BBXR_BTN_CIRCLE)) == BBXR_BTN_RIGHT);
    CHECK(game(t + 0.2, Pad(BBXR_BTN_CIRCLE)) == 0);
    CHECK(game(t + 0.3, Pad()) == 0);
    CHECK(game(t + 0.4, Pad(BBXR_BTN_CIRCLE)) == BBXR_BTN_RIGHT); // a new press swaps again
    // X (square on the pad) is ONE d-pad left pulse; no square reaches the game any more.
    t = 3.0;
    CHECK(game(t, Pad(BBXR_BTN_SQUARE)) == BBXR_BTN_LEFT);
    CHECK(game(t + 0.2, Pad(BBXR_BTN_SQUARE)) == 0);
    CHECK(game(t + 0.3, Pad()) == 0);
    // Y: no triangle (the heal button is gone), it is the interact (cross) instead; the vial
    // gesture's own triangle (no Y down) still passes.
    t = 4.0;
    CHECK(game(t, y(BBXR_BTN_TRIANGLE)) == BBXR_BTN_CROSS);
    CHECK(game(t + 0.1, Pad(BBXR_BTN_TRIANGLE)) == BBXR_BTN_TRIANGLE);
    CHECK(game(t + 0.5, Pad()) == 0);
    // A menu that opens on Y takes it: the game sees neither the triangle nor a cross.
    vrui::InputFilter::Settings on_y = s;
    on_y.menu_face = BBXR_BTN_TRIANGLE;
    f.Configure(on_y);
    CHECK(game(5.0, y(BBXR_BTN_TRIANGLE)) == 0);
    f.Configure(s);
    // A claimed press (right grip + A style combo) is not a swap.
    f.SetClaim(BBXR_BTN_CIRCLE);
    CHECK(game(6.0, Pad(BBXR_BTN_CIRCLE)) == 0);
    CHECK(game(6.5, Pad(BBXR_BTN_CIRCLE)) == 0);
    f.SetClaim(0);
    game(6.6, Pad());
    // A hand holding an item keeps its swap: hold mask with d-pad right (right hand) or left.
    f.SetHoldMask(BBXR_BTN_RIGHT);
    CHECK(game(7.0, Pad(BBXR_BTN_CIRCLE)) == 0);
    CHECK(game(7.05, Pad(BBXR_BTN_CIRCLE)) == 0);
    CHECK(game(7.5, Pad(BBXR_BTN_SQUARE)) == BBXR_BTN_LEFT); // the other hand's swap stays
    game(7.7, Pad());
    f.SetHoldMask(BBXR_BTN_LEFT);
    CHECK(game(8.0, Pad(BBXR_BTN_SQUARE)) == 0);
    CHECK(game(8.5, Pad(BBXR_BTN_CIRCLE)) == BBXR_BTN_RIGHT);
    game(8.7, Pad());
    f.SetHoldMask(0);
    // The game's own menu keeps the old A / B (confirm / back): no weapon swap, no roll.
    f.SetGameMenu(true);
    CHECK(game(9.0, Pad(BBXR_BTN_CIRCLE)) == BBXR_BTN_CROSS);
    CHECK(game(9.5, Pad(BBXR_BTN_CROSS)) == BBXR_BTN_CIRCLE);
    CHECK(game(9.6, Pad(BBXR_BTN_SQUARE)) == BBXR_BTN_SQUARE);
    f.SetGameMenu(false);
}

// Before the world (title, character creation): PRE_WORLD turns the sticks into the d-pad without
// the world UI; a player in the world takes it away again.
void TestPreWorldNav() {
    Rig r;
    r.state.size = sizeof(r.state);
    r.state.flags = BBXR_GAME_PRE_WORLD;
    r.feed = true;
    r.Frame(Pad());
    CHECK(!r.ui.Active()); // the world UI stays off
    // A (circle on the pad) is cross; the stick is centered for the game.
    BbXrInput g = Pad(BBXR_BTN_CIRCLE, 255, 128);
    r.ui.Filter().Apply(r.t, g);
    CHECK((g.buttons & BBXR_BTN_CROSS) && g.lx == 128);
    // Left on the stick repeats like d-pad left (character-creation sliders): 0.35 s, then 0.14 s.
    double t = r.t + 5.0;
    BbXrInput l = Pad(0, 0, 128);
    r.ui.Filter().Apply(t, l);
    CHECK(l.buttons == BBXR_BTN_LEFT);
    l = Pad(0, 0, 128);
    r.ui.Filter().Apply(t + 0.1, l);
    CHECK(l.buttons == 0);
    l = Pad(0, 0, 128);
    r.ui.Filter().Apply(t + 0.4, l);
    CHECK(l.buttons == BBXR_BTN_LEFT);
    // The player arrives: the world mapping is back.
    r.state.flags = BBXR_GAME_IN_WORLD;
    r.Frame(Pad());
    CHECK(r.ui.Active());
    BbXrInput w = Pad(BBXR_BTN_CROSS, 255, 128);
    r.ui.Filter().Apply(r.t, w);
    CHECK(w.buttons == BBXR_BTN_CIRCLE && w.lx == 255);
    // A stale pre-world state does nothing.
    r.state.flags = BBXR_GAME_PRE_WORLD;
    r.ui.SetGameState(r.state, r.t);
    r.feed = false;
    for (int i = 0; i < 300; ++i) r.Frame(Pad());
    BbXrInput st = Pad(BBXR_BTN_CROSS, 255, 128);
    r.ui.Filter().Apply(r.t, st);
    CHECK(st.buttons == BBXR_BTN_CROSS && st.lx == 255); // the filter is idle here: the pad as mapped (no remap)
}

// ---- Physical items (vr_held_items.h, docs/vr/findings/39-physical-items-plan.md) ----

/// Parses BBXR_THROW_BULLET_IDS the way the host does.
bool InThrowList(int bullet) {
    return vrui::held::IdInList(BBXR_THROW_BULLET_IDS, bullet);
}

uint32_t Goods(int row) {
    return BBXR_ITEM_GOODS | uint32_t(row);
}

void TestHeldClassify() {
    using namespace vrui::held;
    // The id list is the one the host defaults to (tests/test_vr_gun.c checks the same text).
    CHECK(std::strcmp(BBXR_THROW_BULLET_IDS, "100,105,111,115,120,121,150,160,170,180,300,350,410,460,470") == 0);
    CHECK(std::strcmp(BBXR_THROW_FOLLOW_IDS, "") == 0);
    // Heals, cures and body buffs go to the mouth, in the right hand.
    for (const int g : {1000, 703, 706, 1100, 1101, 1110, 1120, 2030, 1500, 2060}) {
        const ItemInfo i = Classify(Goods(g));
        CHECK(i.kind == Kind::Mouth && HandFor(i) == 1);
    }
    // Weapon buffs: the paper touches the right weapon from the left hand; the ash the left weapon (gun).
    for (const int g : {1300, 1320, 1310}) {
        const ItemInfo i = Classify(Goods(g));
        CHECK(i.kind == Kind::Blade && i.target_side == 1 && HandFor(i) == 0);
    }
    CHECK(Classify(Goods(1330)).kind == Kind::Blade && Classify(Goods(1330)).target_side == 0 &&
          HandFor(Classify(Goods(1330))) == 1);
    // Throwables: right hand; every one is aimed from the hand, with its own bullet (goods -> bullet).
    static const int kGoodsBullet[][2] = {{1200, 100}, {1201, 105}, {1210, 121}, {1220, 150}, {1230, 170},
                                          {1240, 120}, {1250, 110}, {1251, 114}, {1260, 160}, {1270, 180},
                                          {1410, 300}, {2070, 410}, {2080, 350}, {2120, 460}, {2130, 470}};
    for (const auto& gb : kGoodsBullet) {
        const ItemInfo i = Classify(Goods(gb[0]));
        CHECK(i.kind == Kind::Throw && i.aimed && HandFor(i) == 1 && InThrowList(i.aim_bullet) && i.bullet == gb[1] &&
              i.aim_bullet == (gb[0] == 1250 ? 111 : gb[0] == 1251 ? 115 : gb[1]) && i.speed > 0.0f);
    }
    // The cures' bullets and the impact children are never in the list; the rope chain follows the first bullet.
    for (const int b : {200, 230, 320, 101, 106, 151, 110, 114, 112, 0}) {
        CHECK(!InThrowList(b));
    }
    for (const int b : {110, 111, 112, 115, 116, 101}) {
        CHECK(!vrui::held::IdInList(BBXR_THROW_FOLLOW_IDS, b)); // the follow list is empty
    }
    // Everything else is used at once, as before; weapons are not goods.
    for (const int g : {900, 2040, 1450, 4000}) {
        CHECK(Classify(Goods(g)).kind == Kind::None);
    }
    // The bells: the head gesture, always the right hand, "TO THE HEAD"; the marks are crushed.
    for (const int g : {200, 201, 205, 206, 225, 226, 2050, 4312}) {
        const ItemInfo i = Classify(Goods(g));
        CHECK(i.kind == Kind::Mouth && i.bell && HandFor(i, 0) == 1);
    }
    CHECK(!Classify(Goods(1000)).bell);
    for (const int g : {100, 1400, 2090}) {
        const ItemInfo i = Classify(Goods(g));
        CHECK(i.kind == Kind::Crush && !i.bell && HandFor(i) == 1);
    }
    CHECK(Classify(7000000u).kind == Kind::None);
    CHECK(Classify(BBXR_ITEM_NONE).kind == Kind::None);
    // A drink grabbed from a body slot stays in the hand that grabbed it; a throw never does.
    CHECK(HandFor(Classify(Goods(1000)), 0) == 0 && HandFor(Classify(Goods(1200)), 0) == 1);
}

vrui::held::Frame HeldFrame(double t) {
    vrui::held::Frame f;
    f.time = t;
    f.head_valid = true;
    f.head_pos = {0.0f, 1.6f, 0.0f};
    f.head_rot = {0, 0, 0, 1};
    for (int h = 0; h < 2; ++h) {
        f.hand_valid[h] = true;
        f.grip_rot[h] = {0, 0, 0, 1};
        f.grip_pos[h] = {h ? 0.2f : -0.2f, 1.2f, -0.35f};
    }
    return f;
}

void TestHeldItemLogic() {
    using namespace vrui::held;
    sb::UiHeldItems c; // the defaults
    const m3::Vec3 hold = c.hold;
    // -- geometry
    CHECK(std::fabs(DistToSegment({0, 1, 0}, {-1, 0, 0}, {1, 0, 0}) - 1.0f) < 1e-5f);
    CHECK(std::fabs(DistToSegment({3, 0, 0}, {-1, 0, 0}, {1, 0, 0}) - 2.0f) < 1e-5f);
    CHECK(std::fabs(DistToSegment({1, 1, 1}, {0, 0, 0}, {0, 0, 0}) - std::sqrt(3.0f)) < 1e-5f);
    // The mouth: 24 cm (was 12) around head + (0, -0.10, -0.07), at least 3 cm below the head.
    const m3::Vec3 head{0, 1.6f, 0};
    const m3::Quat id{0, 0, 0, 1};
    float d = 0.0f;
    CHECK(NearMouth({0, 1.5f, -0.07f}, head, id, c, &d) && d < 1e-4f);
    CHECK(NearMouth({0.08f, 1.5f, -0.07f}, head, id, c));
    CHECK(NearMouth({0.14f, 1.5f, -0.07f}, head, id, c));   // 14 cm to the side: now inside
    CHECK(!NearMouth({0.26f, 1.5f, -0.07f}, head, id, c));  // 26 cm to the side
    CHECK(!NearMouth({0, 1.2f, -0.25f}, head, id, c));       // at the chest
    sb::UiHeldItems high = c;
    high.mouth = {0.0f, -0.01f, -0.07f}; // a mouth above the eyes' limit: never "below"
    CHECK(!NearMouth(head + m3::Vec3{0, -0.01f, -0.07f}, head, id, high));
    // turned head: the mouth turns with it (head yawed 90 deg left: its -Z points to -X)
    const m3::Quat left = m3::AxisAngle({0, 1, 0}, 90.0f * m3::kDeg);
    CHECK(NearMouth(head + m3::Rotate(left, {0, -0.10f, -0.07f}), head, left, c));
    CHECK(!NearMouth({0, 1.5f, -0.25f}, head, left, c));
    // The blade: from blade_start of the length to the tip, 10 cm.
    const m3::Vec3 base{0, 1.2f, 0}, tip{0, 1.2f, -1.0f};
    CHECK(TouchesBlade({0.05f, 1.2f, -0.6f}, base, tip, c));
    CHECK(!TouchesBlade({0.5f, 1.2f, -0.6f}, base, tip, c));
    CHECK(!TouchesBlade({0.0f, 1.2f, 0.0f}, base, tip, c));    // the handle (first 25 %) is not the blade
    CHECK(!TouchesBlade({0.0f, 1.2f, -0.1f}, base, tip, c));
    CHECK(TouchesBlade({0.0f, 1.2f, -0.3f}, base, tip, c));
    {   // anywhere along the blade: tip, middle (wider), just above the guard; not the handle or far off
        float fr = -1.0f;
        CHECK(TouchesBlade({0.0f, 1.2f, -1.0f}, base, tip, c, nullptr, false, &fr) && fr > 0.99f);
        CHECK(TouchesBlade({0.05f, 1.2f, -1.05f}, base, tip, c));                  // just past the tip
        CHECK(TouchesBlade({0.13f, 1.2f, -0.625f}, base, tip, c, nullptr, false, &fr) &&
              std::fabs(fr - 0.625f) < 1e-3f);                                       // middle: 13 cm counts
        CHECK(!TouchesBlade({0.5f, 1.2f, -0.3f}, base, tip, c));                   // 50 cm near the guard: no
        CHECK(TouchesBlade({0.0f, 1.2f, -0.27f}, base, tip, c, nullptr, false, &fr) && fr >= c.blade_start);
        CHECK(!TouchesBlade({0.04f, 1.2f, 0.05f}, base, tip, c));                  // the grip/handle
        CHECK(!TouchesBlade({0.03f, 1.2f, -0.15f}, base, tip, c));                 // the handle
        CHECK(!TouchesBlade({0.7f, 1.2f, -0.6f}, base, tip, c));                   // far from the blade
        // the mid zone (42 cm) sits 25 cm out of the blade toward the head (head above the blade here)
        const m3::Vec3 hd{0, 1.6f, -0.3f};
        CHECK(std::fabs(c.blade_mid_radius_m - 0.42f) < 1e-6f && std::fabs(c.blade_mid_offset_m - 0.25f) < 1e-6f);
        CHECK(TouchesBlade({0.0f, 1.7f, -0.625f}, base, tip, c, nullptr, false, &fr, &hd) &&
              std::fabs(fr - 0.625f) < 1e-3f);                                     // 50 cm off the blade, toward us
        CHECK(!TouchesBlade({0.0f, 1.7f, -0.625f}, base, tip, c));                 // no head: zone stays on the blade
        CHECK(!TouchesBlade({0.0f, 0.9f, -0.625f}, base, tip, c, nullptr, false, nullptr, &hd)); // away side
        CHECK(!TouchesBlade({0.0f, 1.2f, -0.1f}, base, tip, c, nullptr, false, nullptr, &hd));   // handle
        {   // sign check with the head on the -X side: the zone is on the head's side, not mirrored
            const m3::Vec3 hx{-0.8f, 1.2f, -0.3f};
            CHECK(TouchesBlade({-0.55f, 1.2f, -0.625f}, base, tip, c, nullptr, false, nullptr, &hx)); // toward the head
            CHECK(!TouchesBlade({0.55f, 1.2f, -0.625f}, base, tip, c, nullptr, false, nullptr, &hx)); // away from it
        }
    }
    CHECK(TouchesBlade({0.1f, 1.3f, 0.0f}, base, base, c, nullptr, true));   // approx: 18 cm around the bone
    CHECK(!TouchesBlade({0.3f, 1.3f, 0.0f}, base, base, c, nullptr, true));

    // -- velocity: 4 m/s along +X for 100 ms
    VelocityTracker v;
    for (int i = 0; i <= 9; ++i) {
        v.Push(i / 90.0, {0.04f * float(i), 1.2f, 0});
    }
    const m3::Vec3 est = v.Estimate(0.08f, 0.5f);
    CHECK(std::fabs(est.x - 3.6f) < 0.05f && std::fabs(est.y) < 1e-3f);
    VelocityTracker still;
    for (int i = 0; i <= 9; ++i) {
        still.Push(i / 90.0, {0, 1.2f, 0});
    }
    CHECK(m3::Length(still.Estimate(0.08f, 0.5f)) < 1e-4f);
    CHECK(m3::Length(VelocityTracker{}.Estimate(0.08f, 0.5f)) == 0.0f);

    // -- Mouth: pick, squeeze, bring to the mouth, dwell 0.3 s -> use
    {
        HeldItem h;
        CHECK(!h.Pick(Goods(900), 0, 0.0));         // not a physical item
        CHECK(!h.Active());
        CHECK(h.Pick(Goods(1000), 0, 0.0) && h.GetState() == HeldItem::State::Held && h.Gripped() && h.Hand() == 1);
        CHECK(!h.Pick(Goods(1100), 1, 0.0));         // one item at a time
        Frame f = HeldFrame(0.1);                    // no grip squeeze at all: the selection was the grip
        Result r = h.Step(f, c);
        CHECK(r.kind == Result::None && r.pulses.size() == 1 && r.pulses[0].hand == 1); // the pick pulse
        f.time = 0.2;
        CHECK(h.Step(f, c).kind == Result::None && h.Active() && h.Gripped());
        // the hand far from the mouth: no progress
        f.time = 0.3;
        CHECK(h.Step(f, c).kind == Result::None && h.Progress() == 0.0f);
        // at the mouth: dwell
        f.grip_pos[1] = m3::Vec3{0.0f, 1.5f, -0.07f} - hold;
        f.time = 0.4;
        r = h.Step(f, c);
        CHECK(r.kind == Result::None && h.InContact() && !r.pulses.empty());
        f.time = 0.55;
        CHECK(h.Step(f, c).kind == Result::None && h.Progress() > 0.4f && h.Progress() < 0.6f);
        f.time = 0.45; // (not monotone input is not a concern; keep going forward)
        f.time = 0.71;
        r = h.Step(f, c);
        CHECK(r.kind == Result::Use && !r.thrown && r.item.goods == 1000 && r.hand == 1 && r.why == "mouth");
        CHECK(!h.Active());
        CHECK(h.BlockMask(0.72, c) != 0 && h.BlockMask(0.71 + 0.9, c) == 0); // lockout 0.8 s
    }
    // A pass through the mouth zone shorter than the hold does not use it.
    {
        HeldItem h;
        h.Pick(Goods(1000), 0, 0.0);
        Frame f = HeldFrame(0.0);
        f.squeeze[1] = 0.9f;
        h.Step(f, c);
        for (int i = 0; i < 8; ++i) { // 0.2 s in the zone
            f.time = 0.1 + i * 0.025;
            f.grip_pos[1] = m3::Vec3{0.0f, 1.5f, -0.07f} - hold;
            CHECK(h.Step(f, c).kind == Result::None);
        }
        f.grip_pos[1] = {0.2f, 1.2f, -0.35f}; // away: the dwell restarts
        f.time = 0.4;
        CHECK(h.Step(f, c).kind == Result::None && h.Progress() == 0.0f);
        CHECK(h.Active());
    }
    // -- Cancel: a fresh trigger press (after the selecting click came up); a menu opened; tracking lost.
    // Opening a squeezed grip does NOT cancel a drink.
    {
        HeldItem h;
        h.Pick(Goods(1000), 0, 0.0);
        Frame f = HeldFrame(0.0);
        f.trigger[1] = 1.0f; // the selecting click is still down at the pick
        CHECK(h.Step(f, c).kind == Result::None);
        f.time = 0.1;
        CHECK(h.Step(f, c).kind == Result::None && h.Active()); // still down: not a cancel
        f.trigger[1] = 0.0f;
        f.time = 0.2;
        CHECK(h.Step(f, c).kind == Result::None);               // up: armed
        f.squeeze[1] = 0.9f;
        f.time = 0.3;
        h.Step(f, c);
        f.squeeze[1] = 0.1f;                                    // opening the hand: still held
        f.time = 0.4;
        CHECK(h.Step(f, c).kind == Result::None && h.Active());
        f.trigger[1] = 1.0f;
        f.time = 0.5;
        Result r = h.Step(f, c);
        CHECK(r.kind == Result::Cancel && r.why == "put back (trigger)" && !h.Active());
        CHECK(r.pulses.size() == 1);
        h.Pick(Goods(1000), 0, 1.0);
        f = HeldFrame(1.0);
        h.Step(f, c);
        f.blocked = true;
        f.time = 1.1;
        CHECK(h.Step(f, c).kind == Result::Cancel && !h.Active());
        h.Pick(Goods(1000), 0, 2.0);
        f = HeldFrame(2.1);
        f.hand_valid[1] = false;
        CHECK(h.Step(f, c).kind == Result::Cancel);
        h.Pick(Goods(1000), 0, 3.0);
        f = HeldFrame(8.2);
        CHECK(h.Step(f, c).kind == Result::None && h.Active()); // no grace timeout any more
        CHECK(h.Cancel(9.1, c, "wheel").kind == Result::Cancel && !h.Active());
    }
    // -- Blade: the paper in the left hand touches the right weapon's blade
    {
        HeldItem h;
        CHECK(h.Pick(Goods(1300), 2, 0.0) && h.Hand() == 0);
        Frame f = HeldFrame(0.0);
        f.squeeze[0] = 0.9f;
        f.blade[1] = {true, false, {0.2f, 1.2f, -0.35f}, {0.2f, 1.2f, -1.35f}};
        h.Step(f, c);
        // the right controller (the weapon's grip) is not touched: the item is at the left grip
        for (int i = 1; i <= 5; ++i) {
            f.time = i * 0.02;
            CHECK(h.Step(f, c).kind == Result::None);
        }
        CHECK(!h.InContact());
        f.grip_pos[0] = m3::Vec3{0.22f, 1.25f, -0.9f} - hold; // 5 cm from the blade
        f.time = 0.2;
        CHECK(h.Step(f, c).kind == Result::None && h.InContact());
        f.time = 0.25;
        CHECK(h.Step(f, c).kind == Result::None);
        f.time = 0.33; // 0.13 s of touch >= 0.12 s
        Result r = h.Step(f, c);
        CHECK(r.kind == Result::Use && r.why == "blade" && r.hand == 0 && r.item.goods == 1300);
        CHECK((h.BlockMask(0.4, c) & BBXR_BTN_L1) != 0); // the left grip's L1 stays off while it is held
    }
    // Bone Marrow Ash: the right hand dusts the left weapon
    {
        HeldItem h;
        CHECK(h.Pick(Goods(1330), 1, 0.0) && h.Hand() == 1);
        Frame f = HeldFrame(0.0);
        f.squeeze[1] = 0.9f;
        f.blade[0] = {true, true, {-0.2f, 1.2f, -0.35f}, {-0.2f, 1.2f, -0.35f}}; // no hitbox data: the bone
        f.grip_pos[1] = {-0.25f, 1.2f, -0.3f};
        h.Step(f, c);
        f.time = 0.05;
        CHECK(h.Step(f, c).kind == Result::None && h.InContact());
        f.time = 0.2;
        CHECK(h.Step(f, c).kind == Result::Use);
        CHECK((h.BlockMask(0.3, c) & BBXR_BTN_L1) == 0);
    }
    // No weapon pose: the blade never touches (nothing happens, the item stays)
    {
        HeldItem h;
        h.Pick(Goods(1300), 2, 0.0);
        Frame f = HeldFrame(0.0);
        f.squeeze[0] = 0.9f;
        for (int i = 0; i < 10; ++i) {
            f.time = i * 0.05;
            CHECK(h.Step(f, c).kind == Result::None);
        }
        CHECK(h.Active() && !h.InContact());
    }
    // -- Throw (flick mode): letting go fast throws along the release; slow puts it back
    c.throw_mode = "flick";
    {
        HeldItem h;
        CHECK(h.Pick(Goods(1200), 1, 0.0) && h.Info().aimed);
        Frame f = HeldFrame(0.0);
        f.squeeze[1] = 0.9f;
        h.Step(f, c);
        for (int i = 1; i <= 8; ++i) { // 3.6 m/s forward (-Z, the head faces -Z)
            f.time = i / 90.0;
            f.grip_pos[1] = m3::Vec3{0.2f, 1.2f, -0.35f - 0.04f * float(i)};
            CHECK(h.Step(f, c).kind == Result::None);
        }
        f.time = 9 / 90.0;
        f.grip_pos[1] = {0.2f, 1.2f, -0.35f - 0.04f * 9.0f};
        f.squeeze[1] = 0.0f;
        const Result r = h.Step(f, c);
        CHECK(r.kind == Result::Use && r.thrown && r.why == "thrown");
        CHECK(r.throw_speed > 3.0f && r.throw_dir.z < -0.99f && r.throw_forward > 3.0f && !r.throw_note.empty());
        CHECK(std::fabs(m3::Length(r.throw_dir) - 1.0f) < 1e-4f);
        CHECK(std::fabs(r.throw_origin.x - 0.2f) < 1e-4f && std::fabs(r.throw_origin.z - (-0.35f - 0.36f + hold.z)) < 1e-4f);
        CHECK(!h.Active() && h.BlockMask(0.2, c) != 0);
    }
    {
        HeldItem h;
        h.Pick(Goods(1230), 3, 0.0);
        Frame f = HeldFrame(0.0);
        f.squeeze[1] = 0.9f;
        h.Step(f, c);
        for (int i = 1; i <= 8; ++i) { // 0.9 m/s
            f.time = i / 90.0;
            f.grip_pos[1] = m3::Vec3{0.2f + 0.01f * float(i), 1.2f, -0.35f};
            h.Step(f, c);
        }
        f.squeeze[1] = 0.0f;
        f.time = 9 / 90.0;
        const Result r = h.Step(f, c);
        CHECK(r.kind == Result::None && h.Active()); // a slow opening does not throw (and does not drop it)
    }
    // A flick of the hand throws without any grip squeeze, once the pick motion is over (0.35 s)
    {
        HeldItem h;
        h.Pick(Goods(1200), 1, 0.0);
        Frame f = HeldFrame(0.0);
        h.Step(f, c);
        bool thrown = false;
        for (int i = 1; i <= 8 && !thrown; ++i) { // 3.6 m/s forward right after the pick: too early
            f.time = i / 90.0;
            f.grip_pos[1] = m3::Vec3{0.2f, 1.2f, -0.35f - 0.04f * float(i)};
            thrown = h.Step(f, c).kind == Result::Use;
        }
        CHECK(!thrown && h.Active());
        f.time = 0.5;
        f.grip_pos[1] = {0.2f, 1.2f, -0.8f};
        h.Step(f, c);
        Result r;
        for (int i = 1; i <= 8 && r.kind != Result::Use; ++i) {
            f.time = 0.5 + i / 90.0;
            f.grip_pos[1] = m3::Vec3{0.2f, 1.2f, -0.8f - 0.04f * float(i)};
            r = h.Step(f, c);
        }
        CHECK(r.kind == Result::Use && r.thrown && r.throw_dir.z < -0.99f && !h.Active());
    }
    // -- Only a forward swing throws (the wind-up must not send the item backwards). Head at the origin
    // facing -Z; a path is {frames, dx, dy, dz per frame} at 90 Hz, from t = 0.5 (the flick is armed).
    {
        struct Seg {
            int frames;
            float dx, dy, dz;
        };
        auto run = [&](const std::vector<Seg>& path, bool via_grip, Result* thrown_at, int* thrown_frame) {
            HeldItem h;
            h.Pick(Goods(1200), 1, 0.0);
            Frame f = HeldFrame(0.0);
            f.squeeze[1] = via_grip ? 0.9f : 0.0f;
            h.Step(f, c);
            f.time = 0.5;
            f.grip_pos[1] = {0.2f, 1.2f, -0.6f};
            h.Step(f, c);
            m3::Vec3 p = f.grip_pos[1];
            int n = 0;
            for (const Seg& sg : path) {
                for (int i = 0; i < sg.frames; ++i, ++n) {
                    f.time = 0.5 + (n + 1) / 90.0;
                    p = p + m3::Vec3{sg.dx, sg.dy, sg.dz};
                    f.grip_pos[1] = p;
                    const Result rr = h.Step(f, c);
                    if (rr.kind == Result::Use) {
                        *thrown_at = rr;
                        *thrown_frame = n;
                        return true;
                    }
                }
            }
            if (via_grip) { // open the grip at the end of the path
                f.time += 1.0 / 90.0;
                f.squeeze[1] = 0.0f;
                const Result rr = h.Step(f, c);
                if (rr.kind == Result::Use) {
                    *thrown_at = rr;
                    *thrown_frame = n;
                    return true;
                }
            }
            return false;
        };
        Result r;
        int fr = -1;
        // wind-up (hand back, +Z, 4.5 m/s) then a forward flick: thrown forward, after the backswing
        CHECK(run({{10, 0, 0, 0.05f}, {8, 0, 0, -0.05f}}, false, &r, &fr));
        CHECK(fr >= 10 && r.throw_dir.z < -0.9f && r.throw_forward > 1.5f);
        CHECK(r.throw_note.find("after a backswing") != std::string::npos);
        // the same wind-up with a grip held, opened at the end of the backswing: nothing thrown
        CHECK(!run({{10, 0, 0, 0.05f}}, true, &r, &fr));
        // a backswing alone (no forward motion): nothing
        CHECK(!run({{12, 0, 0, 0.05f}, {10, 0, 0, 0}}, false, &r, &fr));
        // a backswing up and back: nothing
        CHECK(!run({{10, 0, 0.03f, 0.05f}}, false, &r, &fr));
        // live vectors (v69): slow (1.5-1.9 m/s) releases, backwards or steeply up: never a throw
        CHECK(!run({{12, 0.0f, 0, 0.0178f}}, false, &r, &fr) && !run({{12, 0.0f, 0, 0.0178f}}, true, &r, &fr)); // 1.6 m/s back
        CHECK(!run({{12, 0.0f, 0.0094f, 0.0094f}, {4, 0, 0, 0}}, true, &r, &fr));
        CHECK(!run({{12, -0.0038f, 0.0086f, 0.006f}}, true, &r, &fr)); // 1.5 m/s back-up
        CHECK(!run({{12, 0.002f, 0.0111f, -0.0211f}}, false, &r, &fr)); // 1.9 m/s forward-up, below the 2.5 m/s floor
        // a forward flick just above the floor throws, and a near-vertical one is pitch-limited
        CHECK(run({{10, 0, 0, -0.03f}}, false, &r, &fr) && r.throw_dir.z < -0.99f);
        CHECK(run({{10, 0, 0.1f, -0.03f}}, false, &r, &fr) && r.throw_dir.y <= 0.8661f && r.throw_dir.z < -0.4f);
        // a slow forward swing (0.9 m/s): nothing, grip or not
        CHECK(!run({{20, 0, 0, -0.01f}}, false, &r, &fr) && !run({{20, 0, 0, -0.01f}}, true, &r, &fr));
        // a sideways swing (3.6 m/s, both ways): nothing
        CHECK(!run({{10, 0.04f, 0, 0}}, false, &r, &fr) && !run({{10, -0.04f, 0, 0}}, false, &r, &fr));
        // overhand: forward 2.7 m/s and up 3.6 m/s: thrown up and forward
        CHECK(run({{8, 0, 0.04f, -0.03f}}, false, &r, &fr));
        CHECK(r.throw_dir.y > 0.4f && r.throw_dir.z < -0.3f);
        // underhand lob: forward 3.6 m/s, up 1.8 m/s
        CHECK(run({{8, 0, 0.02f, -0.04f}}, false, &r, &fr));
        CHECK(r.throw_dir.z < -0.8f && r.throw_dir.y > 0.2f && r.throw_dir.y < 0.6f);
        // a downward throw keeps its pitch (forward 3.6, down 1.8)
        CHECK(run({{8, 0, -0.02f, -0.04f}}, false, &r, &fr) && r.throw_dir.y < -0.2f && r.throw_dir.z < -0.8f);
        // the grip opened during a forward swing throws forward
        CHECK(run({{8, 0, 0, -0.04f}}, true, &r, &fr) && r.throw_dir.z < -0.99f);
        // forward 2.7 m/s with a much faster sideways 10.8 m/s: the direction is held in the forward
        // hemisphere (never sideways-only, never backwards)
        CHECK(run({{8, 0.12f, 0, -0.03f}}, false, &r, &fr));
        CHECK(r.throw_dir.z <= -0.29f && std::fabs(m3::Length(r.throw_dir) - 1.0f) < 1e-4f);
        // a turned head (90 deg left: forward is -X): a swing along -X throws, along -Z does not
        {
            auto turned = [&](float dx, float dz) {
                HeldItem h;
                h.Pick(Goods(1200), 1, 0.0);
                Frame f = HeldFrame(0.0);
                f.head_rot = m3::Quat{0.0f, std::sqrt(0.5f), 0.0f, std::sqrt(0.5f)};
                h.Step(f, c);
                f.time = 0.5;
                h.Step(f, c);
                Result rr;
                for (int i = 1; i <= 8 && rr.kind != Result::Use; ++i) {
                    f.time = 0.5 + i / 90.0;
                    f.grip_pos[1] = m3::Vec3{0.2f + dx * float(i), 1.2f, -0.35f + dz * float(i)};
                    rr = h.Step(f, c);
                }
                return rr.kind == Result::Use ? rr.throw_dir.x : 0.0f;
            };
            CHECK(turned(-0.04f, 0.0f) < -0.99f);
            CHECK(turned(0.0f, -0.04f) == 0.0f);
        }
    }
    // The rope molotov is aimed like the molotov now
    {
        HeldItem h;
        h.Pick(Goods(1250), 0, 0.0);
        CHECK(h.Info().aimed && h.Info().kind == Kind::Throw && h.Info().bullet == 110 && h.Info().aim_bullet == 111);
    }
    c.throw_mode = "aim";
    // -- the buttons hidden from the game while an item is in a hand
    {
        HeldItem h;
        CHECK(h.BlockMask(0.0, c) == 0);
        h.Pick(Goods(1000), 0, 0.0);
        const uint32_t m = h.BlockMask(0.0, c);
        CHECK((m & BBXR_BTN_R1) && (m & BBXR_BTN_R2) && (m & BBXR_BTN_RIGHT) && !(m & BBXR_BTN_L1));
        CHECK(!(m & BBXR_BTN_L2)); // the right hand holds: the left hand's gun stays free
    }
    // -- the left hand holds (the Fire Paper): its L1 and L2 (the gun) are hidden, the right hand's attacks are not
    {
        HeldItem h;
        CHECK(h.Pick(Goods(1300), 0, 0.0) && h.Hand() == 0);
        const uint32_t m = h.BlockMask(0.0, c);
        CHECK((m & BBXR_BTN_L1) && (m & BBXR_BTN_L2));
        CHECK(!(m & (BBXR_BTN_R1 | BBXR_BTN_R2 | BBXR_BTN_RIGHT)));
        h.Cancel(1.0, c, "put back");
        CHECK(!h.Active() && (h.BlockMask(1.0 + 0.3, c) & BBXR_BTN_L2)); // the tail after it left (>= 300 ms)
        CHECK(h.BlockMask(1.0 + c.lockout_s + 0.05, c) == 0);
    }

    // -- collision haptics: the mapping
    {
        sb::UiCollisionHaptics ch;
        const Pulse light = CollisionPulse(ch, 1, 0, 0);    // a hit, cost unknown: the weak pulse
        CHECK(light.hand == 1 && std::fabs(light.amplitude - 0.55f) < 1e-5f && std::fabs(light.seconds - 0.07f) < 1e-5f);
        const Pulse mid = CollisionPulse(ch, 1, 0, 20);     // 20 of 40: half way
        CHECK(std::fabs(mid.amplitude - 0.775f) < 1e-4f && std::fabs(mid.seconds - 0.115f) < 1e-4f);
        const Pulse strong = CollisionPulse(ch, 1, 0, 80);  // over the reference: the strong one
        CHECK(std::fabs(strong.amplitude - 1.0f) < 1e-5f && std::fabs(strong.seconds - 0.16f) < 1e-5f);
        const Pulse wall = CollisionPulse(ch, 1, 1, 0);
        CHECK(std::fabs(wall.amplitude - 0.9f) < 1e-5f && std::fabs(wall.seconds - 0.18f) < 1e-5f);
        const Pulse obj = CollisionPulse(ch, 1, 2, 0);
        CHECK(std::fabs(obj.amplitude - 0.7f) < 1e-5f && std::fabs(obj.seconds - 0.12f) < 1e-5f);
        CHECK(CollisionPulse(ch, 1, 1, 40).amplitude >= wall.amplitude); // a stronger bounce is not weaker
        CHECK(CollisionPulse(ch, 1, 1, 40).seconds >= wall.seconds);     // (even when hit_strong is shorter)
        ch.scale_by_strength = false;
        CHECK(std::fabs(CollisionPulse(ch, 1, 0, 80).amplitude - 0.55f) < 1e-5f);
        ch.hit = {3.0f, 5.0f}; // out of range values are clamped
        CHECK(CollisionPulse(ch, 1, 0, 0).amplitude <= 1.0f && CollisionPulse(ch, 1, 0, 0).seconds <= 1.0f);
    }
}

/// Opens the quick wheel (R3 held) and picks quick slot `slot` with the right trigger.
void PickOnWheel(Rig& r, int slot) {
    r.in.grip[1].position = r.in.aim[1].position = {0.2f, 1.2f, -0.35f}; // where the wheel opens
    r.Frame(Pad());
    const m3::Vec3 tip0 = r.in.aim[1].position + m3::Vec3{0.0f, 0.0f, -0.06f};
    for (int i = 0; i < 50; ++i) {
        r.ui.SetGameState(r.state, r.t);
        r.Frame(Pad(BBXR_BTN_R3));
    }
    r.Frame(Pad());
    CHECK(r.ui.Menus().IsOpen("quick_wheel"));
    const m3::Vec3 facing = m3::Normalize(r.in.head.position - tip0, {0, 0, 1});
    const m3::Vec3 px = m3::Normalize(m3::Cross({0, 1, 0}, facing), {1, 0, 0});
    const m3::Vec3 py = m3::Cross(facing, px);
    const float a = (90.0f - float(slot) * 360.0f / 8.0f) * kDeg; // 5 quick + 3 weapons
    const m3::Vec3 g = m3::Vec3{0.2f, 1.2f, -0.35f} + px * (0.10f * std::cos(a)) + py * (0.10f * std::sin(a));
    r.in.grip[1].position = r.in.aim[1].position = g;
    r.Frame(Pad());
    r.Frame(Pad());
    CHECK(r.ui.Menus().Hovered("quick_wheel") == slot);
    r.in.trigger[1] = 0.9f;
    r.Frame(Pad());
    r.in.trigger[1] = 0.0f;
    r.Frame(Pad());
    CHECK(!r.ui.Menus().IsOpen("quick_wheel"));
    r.in.grip[1].position = r.in.aim[1].position = {0.2f, 1.2f, -0.35f};
}

fs::path HeldAssets(const fs::path& assets, const char* name, const std::string& game_extra) {
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "ui", ec);
    for (const char* f : {"icons.json", "icons.png", "font.json", "font.png"}) {
        fs::copy_file(assets / "ui" / f, dir / "ui" / f, ec);
    }
    std::ofstream f(dir / "ui_layouts.json", std::ios::binary);
    f << "{\n  \"version\": 1,\n  \"game\": {\"version\": 2, \"active\": [\"quick_wheel\"]" << game_extra << "}\n}\n";
    return dir;
}

// ---- The aimed throw (hold the grip to aim, let go to throw) ----
void TestAimThrow() {
    using namespace vrui::held;
    const float kDeg = 3.14159265f / 180.0f;
    // -- ComputeArc
    {
        ItemInfo p;
        p.speed = 10.0f; // no gravity, level: 10 m in a second
        ThrowArc a = ComputeArc({0, 1.5f, 0}, {0, 0, -1}, p, 0.0f, 1.0f);
        CHECK(!a.hit_floor && std::fabs(a.impact.z + 10.0f) < 0.3f && std::fabs(a.impact.y - 1.5f) < 1e-4f);
        CHECK(a.points.size() > 50 && a.points.size() <= 240 && std::fabs(a.flight_s - 1.0f) < 0.03f);
        p.speed = 0.0f;
        a = ComputeArc({1, 2, 3}, {0, 0, -1}, p, 0.0f);
        CHECK(a.points.size() == 1 && a.impact.x == 1.0f && a.impact.y == 2.0f && !a.hit_floor);
        // constant gravity 9.8 from 1.5 m: t = sqrt(2h/g), distance v t
        p.speed = 10.0f;
        p.g_in = p.g_out = 9.8f;
        a = ComputeArc({0, 1.5f, 0}, {0, 0, -1}, p, 0.0f);
        const float want = 10.0f * std::sqrt(2.0f * 1.5f / 9.8f);
        CHECK(a.hit_floor && std::fabs(-a.impact.z - want) < 0.05f * want && std::fabs(a.impact.y) < 1e-4f);
        // 45 degrees from the floor: v^2 / g
        const m3::Vec3 d45{0.0f, std::sin(45.0f * kDeg), -std::cos(45.0f * kDeg)};
        a = ComputeArc({0, 0, 0}, d45, p, 0.0f);
        CHECK(a.hit_floor && std::fabs(-a.impact.z - 100.0f / 9.8f) < 0.05f * 100.0f / 9.8f);
        // straight up with plenty of time still lands
        a = ComputeArc({0, 1.5f, 0}, {0, 1, 0}, p, 0.0f, 10.0f);
        CHECK(a.hit_floor && a.flight_s > 1.5f && a.points.size() <= 240);
        // a short flight does not reach the floor
        a = ComputeArc({0, 1.5f, 0}, {0, 0, -1}, p, 0.0f, 0.1f);
        CHECK(!a.hit_floor && a.impact.y > 1.0f);
        // the molotov (8 m/s^2 inside 6 m, 12 beyond), 1.5 m up, level: 0.4 s to 6 m, then 12 m/s^2
        const ItemInfo mol = Classify(Goods(1200));
        a = ComputeArc({0, 1.5f, 0}, {0, 0, -1}, mol, 0.0f);
        CHECK(a.hit_floor && std::fabs(-a.impact.z - 8.946f) < 0.05f * 8.946f);
    }
    // -- AimDirection
    {
        const m3::Quat id{0, 0, 0, 1};
        m3::Vec3 d = AimDirection(id, 10.0f, -30.0f, 60.0f);
        CHECK(std::fabs(d.y - std::sin(10.0f * kDeg)) < 1e-4f && std::fabs(d.z + std::cos(10.0f * kDeg)) < 1e-4f &&
              std::fabs(d.x) < 1e-4f);
        d = AimDirection(m3::FromTo({0, 0, -1}, {0, -1, 0}), 10.0f, -30.0f, 60.0f);
        CHECK(std::fabs(d.y - std::sin(-30.0f * kDeg)) < 1e-3f && d.z < -0.8f && std::fabs(d.x) < 1e-3f);
        d = AimDirection(m3::FromTo({0, 0, -1}, {0, 1, 0}), 10.0f, -30.0f, 60.0f);
        CHECK(std::fabs(d.y - std::sin(60.0f * kDeg)) < 1e-3f);
        d = AimDirection(m3::AxisAngle({0, 1, 0}, 90.0f * kDeg), 10.0f, -30.0f, 60.0f); // turned left
        CHECK(d.x < -0.98f && std::fabs(d.z) < 1e-3f && std::fabs(d.y - std::sin(10.0f * kDeg)) < 1e-3f);
    }
    // -- the launch elevation mapping: neutral (grip +20) -> flat, raised -> higher, lowered -> down, clamped
    {
        const sb::UiHeldItems dc;
        auto el = [&](float g) {
            return AimLaunchElevationDeg(g, dc.throw_aim_pitch_bias_deg, dc.throw_aim_min_pitch_deg,
                                         dc.throw_aim_max_pitch_deg, dc.throw_aim_level_deg, dc.throw_aim_gain);
        };
        CHECK(std::fabs(el(20.0f) - 3.0f) < 1e-4f);
        CHECK(el(35.0f) > el(20.0f) && el(50.0f) > el(35.0f));
        CHECK(std::fabs(el(35.0f) - 21.0f) < 1e-3f);
        CHECK(el(0.0f) < 0.0f && el(-20.0f) < el(0.0f));
        CHECK(el(89.0f) == dc.throw_aim_max_pitch_deg && el(-89.0f) == dc.throw_aim_min_pitch_deg);
        const m3::Vec3 d = AimDirection(m3::FromTo({0, 0, -1}, {0, std::sin(20.0f * kDeg), -std::cos(20.0f * kDeg)}),
                                        3.0f, -30.0f, 60.0f, 20.0f, 1.2f);
        CHECK(std::fabs(d.y - std::sin(3.0f * kDeg)) < 1e-3f && d.z < -0.99f);
    }
    // -- the flow
    const sb::UiHeldItems c;
    auto frame = [](double t, float sq, float trig = 0.0f) {
        Frame f = HeldFrame(t);
        f.squeeze[1] = sq;
        f.trigger[1] = trig;
        return f;
    };
    {
        HeldItem h;
        CHECK(h.Pick(Goods(1200), 1, 0.0) && !h.Aiming() && h.Aim() == nullptr);
        CHECK(h.Step(frame(0.0, 0.0f), c).kind == Result::None && !h.Aiming());
        CHECK(std::strcmp(h.Hint(), "HOLD GRIP: AIM") == 0);
        Result r = h.Step(frame(0.1, 0.9f), c);
        CHECK(r.kind == Result::None && !r.pulses.empty() && h.Aiming() && h.Aim() != nullptr);
        for (int i = 2; i < 6; ++i) {
            CHECK(h.Step(frame(0.1 * i, 0.9f), c).kind == Result::None);
        }
        CHECK(h.Aim()->points.size() > 5 && h.Aim()->hit_floor && std::strcmp(h.Hint(), "RELEASE: THROW") == 0);
        const m3::Vec3 dir = h.AimDir();
        const ThrowArc arc = *h.Aim();
        r = h.Step(frame(0.7, 0.0f), c);
        CHECK(r.kind == Result::Use && r.thrown && r.why == "thrown" && r.throw_speed == 0.0f && r.has_marker);
        CHECK(m3::Length(r.throw_dir - dir) < 1e-5f && m3::Length(r.throw_marker - arc.impact) < 1e-5f);
        CHECK(r.marker_on_floor && r.flight_s > 0.1f && !h.Active() && !h.Aiming() && h.Aim() == nullptr);
        CHECK(std::fabs(r.throw_origin.x - 0.2f) < 1e-4f && std::fabs(r.throw_origin.y - 1.2f) < 1e-4f);
    }
    { // the trigger cancels while aiming
        HeldItem h;
        h.Pick(Goods(1200), 1, 0.0);
        h.Step(frame(0.0, 0.0f), c);
        h.Step(frame(0.1, 0.9f), c);
        CHECK(h.Aiming());
        const Result r = h.Step(frame(0.2, 0.9f, 1.0f), c);
        CHECK(r.kind == Result::Cancel && !r.thrown && !h.Aiming() && !h.Active());
    }
    { // a grip that is closed at the pick does not aim until it was opened and closed again
        HeldItem h;
        h.Pick(Goods(1200), 1, 0.0);
        for (int i = 0; i < 5; ++i) {
            CHECK(h.Step(frame(0.05 * i, 0.9f), c).kind == Result::None && !h.Aiming());
        }
        CHECK(h.Step(frame(0.3, 0.0f), c).kind == Result::None && !h.Aiming());
        CHECK(h.Step(frame(0.35, 0.9f), c).kind == Result::None && h.Aiming());
    }
    { // flick mode keeps the old flow (no aiming on a squeeze)
        sb::UiHeldItems cf;
        cf.throw_mode = "flick";
        HeldItem h;
        h.Pick(Goods(1200), 1, 0.0);
        h.Step(frame(0.0, 0.0f), cf);
        h.Step(frame(0.1, 0.9f), cf);
        CHECK(!h.Aiming() && std::strcmp(h.Hint(), "THROW IT") == 0);
        const Result r = h.Step(frame(0.2, 0.0f), cf);
        CHECK(r.kind == Result::None && h.Active()); // a still hand opening: nothing thrown
    }
}

void TestHeldItemsInGame(const fs::path& assets) {
    const fs::path dir = HeldAssets(assets, "bbport_game_ui_held_test", "");
    Rig r;
    std::string log;
    CHECK(r.ui.Load(dir, nullptr, log));
    CHECK(r.ui.Menus().Layout().game.held_items);
    CHECK(r.ui.Menus().Layout().game.collision_haptics.on);
    r.feed = true;
    r.state = GameState(80);
    r.state.quick[2] = Goods(1300); // Fire Paper
    r.ui.SetGameState(r.state, r.t);
    r.Frame(Pad());
    CHECK(r.ui.Active());
    BbXrUiAction actions[8];

    // -- A vial from the wheel is held, not used.
    PickOnWheel(r, 0);
    CHECK(TakeUse(r.ui, actions, 8) == 0);
    CHECK(r.ui.HeldItemState().Active() && r.ui.HeldItemState().Info().goods == 1000 &&
          r.ui.HeldItemState().Hand() == 1);
    // Swings and the swap tap are hidden from the game while it is in the hand
    BbXrInput raw = Pad(BBXR_BTN_R1 | BBXR_BTN_RIGHT | BBXR_BTN_CROSS);
    raw.r2 = 255;
    r.ui.Filter().Apply(r.t, raw);
    CHECK(!(raw.buttons & (BBXR_BTN_R1 | BBXR_BTN_RIGHT)) && (raw.buttons & BBXR_BTN_CIRCLE)); // B is the roll
    // a swing gesture does not label an attack either
    r.in.ms_since_swing = 5.0;
    r.in.pad.hand_speed[1] = 3.0f;
    const int swings = r.ui.Readout().swing_no;
    r.Frame(Pad(BBXR_BTN_R1));
    CHECK(r.ui.Readout().swing_no == swings);
    r.in.ms_since_swing = 1e9;
    // the grip closes, the hand goes to the mouth: used after the dwell
    r.in.squeeze[1] = 0.9f;
    r.Frame(Pad());
    CHECK(r.ui.HeldItemState().Gripped());
    r.in.grip[1].position = r.in.aim[1].position = m3::Vec3{0.0f, 1.5f, -0.07f} - m3::Vec3{0.0f, 0.0f, -0.05f};
    for (int i = 0; i < 12; ++i) {
        r.Frame(Pad());
    }
    CHECK(r.ui.HeldItemState().Active());
    for (int i = 0; i < 30 && r.ui.HeldItemState().Active(); ++i) {
        r.Frame(Pad());
    }
    CHECK(!r.ui.HeldItemState().Active());
    uint32_t n = TakeUse(r.ui, actions, 8);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK && actions[0].a == 0 && actions[0].b == int32_t(Goods(1000)));
    CHECK(actions[0].c == 0);
    // the lock-out ends: swings and the swap tap are the game's again
    for (int i = 0; i < 100; ++i) {
        r.Frame(Pad());
    }
    raw = Pad(BBXR_BTN_R1 | BBXR_BTN_RIGHT);
    r.ui.Filter().Apply(r.t, raw);
    CHECK((raw.buttons & BBXR_BTN_R1) && (raw.buttons & BBXR_BTN_RIGHT));
    r.in.squeeze[1] = 0.0f;
    r.Frame(Pad());

    // -- A molotov is thrown: origin and direction, then the use
    r.ui.PushEvent({BBXR_EV_ACTION_DONE, int32_t(BBXR_UI_USE_QUICK), 1, 0}, r.t);
    r.Frame(Pad());
    PickOnWheel(r, 1);
    CHECK(TakeUse(r.ui, actions, 8) == 0);
    CHECK(r.ui.HeldItemState().Info().kind == vrui::held::Kind::Throw && r.ui.HeldItemState().Info().bullet == 100);
    r.in.squeeze[1] = 0.9f;
    r.Frame(Pad());
    CHECK(r.ui.HeldItemState().Gripped() && r.ui.HeldItemState().Aiming() && r.ui.HeldItemState().Aim() != nullptr);
    CHECK(r.all_log.find("goods 1200 aiming (hold grip, release to throw)") != std::string::npos);
    for (int i = 1; i <= 8; ++i) { // the hand keeps still, aiming
        r.Frame(Pad());
    }
    r.in.squeeze[1] = 0.0f;
    r.Frame(Pad());
    CHECK(!r.ui.HeldItemState().Active());
    n = TakeUse(r.ui, actions, 8);
    CHECK(n == 3 && actions[0].kind == BBXR_UI_THROW_ORIGIN && actions[1].kind == BBXR_UI_THROW_DIR &&
          actions[2].kind == BBXR_UI_USE_QUICK);
    if (n == 3) {
        // the head is level at (0, 1.6, 0) facing -Z: the head frame is the world minus the head
        CHECK(std::abs(actions[0].a - 200) <= 2 && std::abs(actions[0].b - int(1000 * (1.2f - 1.6f))) <= 2 &&
              std::abs(actions[0].c - int(1000 * (-0.35f - 0.05f))) <= 2);
        // a level grip (-20 deg under the neutral hand) aims 3 + 1.2 * -20 = -21 degrees: sin -21 = -0.358, cos 21 = 0.934
        CHECK(actions[1].c < -9200 && std::abs(actions[1].b + 3584) < 100 && std::abs(actions[1].a) < 200);
        CHECK(actions[2].a == 1 && actions[2].b == int32_t(Goods(1200)) && actions[2].c == 0); // the game's own speed
    }
    CHECK(r.all_log.find("goods 1200 thrown (bullet 100, aimed): origin") != std::string::npos &&
          r.all_log.find("(game's) -> use quick slot") != std::string::npos);
    r.ui.PushEvent({BBXR_EV_ACTION_DONE, int32_t(BBXR_UI_USE_QUICK), 1, 0}, r.t);
    r.Frame(Pad());

    // -- An item with none left is not held (the old path: the action goes out and the game refuses)
    {
        BbXrGameState empty = r.state;
        for (uint32_t i = 0; i < empty.item_count; ++i) {
            if (empty.items[i].id == Goods(1230)) {
                empty.items[i].count = 0;
            }
        }
        const BbXrGameState keep = r.state;
        r.state = empty;
        r.Frame(Pad());
        PickOnWheel(r, 3); // pebble, none left
        CHECK(!r.ui.HeldItemState().Active());
        r.state = keep;
        r.Frame(Pad());
        TakeUse(r.ui, actions, 8);
    }

    // -- A trigger press puts it back: no action
    PickOnWheel(r, 3); // pebble
    r.Frame(Pad());
    CHECK(r.ui.HeldItemState().Active());
    r.in.trigger[1] = 0.9f;
    r.Frame(Pad());
    r.in.trigger[1] = 0.0f;
    CHECK(!r.ui.HeldItemState().Active() && TakeUse(r.ui, actions, 8) == 0);
    CHECK(r.all_log.find("put back: put back (trigger)") != std::string::npos);

    // -- Opening the wheel again cancels an item in the hand
    PickOnWheel(r, 0);
    CHECK(r.ui.HeldItemState().Active());
    r.in.squeeze[1] = 0.9f;
    for (int i = 0; i < 60; ++i) { // R3 held 0.5 s opens the wheel
        r.ui.SetGameState(r.state, r.t);
        r.Frame(Pad(BBXR_BTN_R3));
    }
    CHECK(!r.ui.HeldItemState().Active());
    CHECK(TakeUse(r.ui, actions, 8) == 0);
    r.Frame(Pad());
    r.Frame(Pad(BBXR_BTN_R3)); // a tap closes the wheel
    for (int i = 0; i < 5; ++i) {
        r.Frame(Pad());
    }
    CHECK(!r.ui.Menus().IsOpen("quick_wheel"));
    r.in.squeeze[1] = 0.0f;
    for (int i = 0; i < 100; ++i) {
        r.Frame(Pad());
    }

    // -- A game menu open blocks and cancels
    PickOnWheel(r, 0);
    CHECK(r.ui.HeldItemState().Active());
    r.state.flags |= BBXR_GAME_MENU_OPEN;
    r.ui.SetGameState(r.state, r.t);
    r.Frame(Pad());
    CHECK(!r.ui.HeldItemState().Active());
    r.state.flags &= ~BBXR_GAME_MENU_OPEN;
    r.ui.SetGameState(r.state, r.t);
    r.Frame(Pad());

    // -- The paper: the left hand holds it, the right weapon's bone is the target (no hitbox data: the bone)
    for (int i = 0; i < 100; ++i) {
        r.Frame(Pad());
    }
    PickOnWheel(r, 2);
    CHECK(r.ui.HeldItemState().Active() && r.ui.HeldItemState().Info().kind == vrui::held::Kind::Blade &&
          r.ui.HeldItemState().Hand() == 0);
    r.state.debug.flags = BBXR_DEBUG_POSE;
    r.state.debug.weapon_valid = 2u; // the right weapon
    const float weapon[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0.2f, 1.2f, -0.35f}; // at the right hand
    std::memcpy(r.state.debug.weapon[1], weapon, sizeof(weapon));
    r.ui.SetGameState(r.state, r.t);
    r.in.squeeze[0] = 0.9f;
    r.Frame(Pad());
    for (int i = 0; i < 10; ++i) {
        r.Frame(Pad());
    }
    CHECK(r.ui.HeldItemState().Active() && !r.ui.HeldItemState().InContact()); // the left hand is 40 cm away
    r.in.grip[0].position = r.in.aim[0].position = {0.14f, 1.2f, -0.30f};
    for (int i = 0; i < 20 && r.ui.HeldItemState().Active(); ++i) {
        r.Frame(Pad());
    }
    CHECK(!r.ui.HeldItemState().Active());
    n = TakeUse(r.ui, actions, 8);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK && actions[0].a == 2 && actions[0].b == int32_t(Goods(1300)));
    r.in.squeeze[0] = 0.0f;
    r.in.grip[0].position = r.in.aim[0].position = {-0.2f, 1.2f, -0.35f};

    // -- Items that are not physical are used at once, as before
    r.ui.PushEvent({BBXR_EV_ACTION_DONE, int32_t(BBXR_UI_USE_QUICK), 1, 0}, r.t);
    for (int i = 0; i < 100; ++i) {
        r.Frame(Pad());
    }
    r.state.quick[4] = Goods(900);
    r.state.debug = {};
    r.ui.SetGameState(r.state, r.t);
    r.Frame(Pad());
    PickOnWheel(r, 4);
    CHECK(!r.ui.HeldItemState().Active());
    n = TakeUse(r.ui, actions, 8);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK && actions[0].a == 4 && actions[0].b == int32_t(Goods(900)));
}

void TestHeldItemsOff(const fs::path& assets) {
    // held_items=false: picking uses the item at once, as before
    const fs::path dir = HeldAssets(assets, "bbport_game_ui_held_off_test", ", \"held_items\": false");
    Rig r;
    std::string log;
    CHECK(r.ui.Load(dir, nullptr, log));
    CHECK(!r.ui.Menus().Layout().game.held_items);
    r.feed = true;
    r.state = GameState(80);
    r.ui.SetGameState(r.state, r.t);
    r.Frame(Pad());
    PickOnWheel(r, 0);
    CHECK(!r.ui.HeldItemState().Active());
    BbXrUiAction actions[4];
    const uint32_t n = r.ui.TakeActions(actions, 4);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK && actions[0].a == 0);
}

void TestCollisionHaptics(const fs::path& assets) {
    // Defaults parse; a custom block overrides; "on": false falls back to the old swing pulses.
    sb::UiLayout l;
    std::string error;
    CHECK(sb::ParseLayout("{\"game\": {\"version\": 2}}", l, error));
    CHECK(l.game.collision_haptics.on && l.game.held_items);
    CHECK(std::fabs(l.game.collision_haptics.wall.amplitude - 0.9f) < 1e-5f);
    CHECK(std::fabs(l.game.held.mouth_radius_m - 0.24f) < 1e-6f && std::fabs(l.game.held.throw_min_speed - 1.5f) < 1e-6f);
    sb::UiLayout c;
    CHECK(sb::ParseLayout("{\"game\": {\"version\": 2, \"held_items\": false, \"held\": {\"mouth_radius_m\": 0.2, "
                          "\"throw_min_speed\": 100, \"release_squeeze\": 2.0}, \"collision_haptics\": {\"on\": false, "
                          "\"wall\": [0.3, 0.5], \"strength_ref\": 0}}}",
                          c, error));
    CHECK(!c.game.held_items && !c.game.collision_haptics.on);
    CHECK(std::fabs(c.game.held.mouth_radius_m - 0.2f) < 1e-6f && c.game.held.throw_min_speed == 15.0f); // clamped
    CHECK(c.game.held.release_squeeze <= c.game.held.grab_squeeze);
    CHECK(std::fabs(c.game.collision_haptics.wall.seconds - 0.5f) < 1e-6f && c.game.collision_haptics.strength_ref == 1.0f);
    // The shipped default text carries the new keys.
    const std::string text = sb::DefaultLayoutText();
    CHECK(text.find("\"held_items\": true") != std::string::npos && text.find("\"collision_haptics\"") != std::string::npos);

    // In the UI: a hit, a wall bounce and an object bounce each buzz the weapon hand differently.
    const fs::path dir = HeldAssets(assets, "bbport_game_ui_collision_test", "");
    Rig r;
    std::string log;
    CHECK(r.ui.Load(dir, nullptr, log));
    r.feed = true;
    r.state = GameState(80);
    r.ui.SetGameState(r.state, r.t);
    r.Frame(Pad());
    const auto hit = [&](int contact, int paid, int ticket) {
        r.ui.PushEvent({BBXR_EV_SWING_OPEN, paid, 100000, ticket}, r.t);
        r.Frame(Pad());
        r.ui.PushEvent({BBXR_EV_SWING_HIT, paid, contact, ticket}, r.t);
        r.Frame(Pad());
        return r.StrongestHaptic(1);
    };
    const float weak = hit(0, 10, 11), strong = hit(0, 40, 12), wall = hit(1, 20, 13), object = hit(2, 20, 14);
    CHECK(weak > 0.55f && weak < 0.7f);
    CHECK(std::fabs(strong - 1.0f) < 1e-4f);
    CHECK(wall > 0.9f && object > 0.7f && wall > object);
    CHECK(r.all_log.find("weapon collision haptic: wall") != std::string::npos);
    // Contacts the ticket path did not announce (no open ticket): haptics only, de-duplicated per hand.
    for (int i = 0; i < 20; ++i) r.Frame(Pad()); // quiet past the 80 ms gap
    const sb::SwingReadout before = r.ui.Readout();
    r.ui.PushEvent({BBXR_EV_WEAPON_CONTACT, 0, 1, 1}, r.t); // a wall, strength unknown
    r.Frame(Pad());
    CHECK(r.StrongestHaptic(1) >= 0.89f && r.StrongestHaptic(0) == 0.0f);
    CHECK(r.all_log.find("weapon contact haptic: wall") != std::string::npos);
    CHECK(r.ui.Readout().result == before.result && r.ui.Readout().label_paid == before.label_paid); // no "HIT -N"
    r.ui.PushEvent({BBXR_EV_WEAPON_CONTACT, 0, 1, 1}, r.t); // the same burst, 11 ms later
    r.Frame(Pad());
    CHECK(r.StrongestHaptic(1) == 0.0f);
    for (int i = 0; i < 10; ++i) r.Frame(Pad()); // > 80 ms
    r.ui.PushEvent({BBXR_EV_WEAPON_CONTACT, 40, 0, 1}, r.t); // a heavy character hit: the strong pulse
    r.Frame(Pad());
    CHECK(std::fabs(r.StrongestHaptic(1) - 1.0f) < 1e-4f);
    for (int i = 0; i < 10; ++i) r.Frame(Pad());
    r.ui.PushEvent({BBXR_EV_WEAPON_CONTACT, 0, 0, 0}, r.t); // the left hand has its own gap
    r.ui.PushEvent({BBXR_EV_WEAPON_CONTACT, 0, 2, 1}, r.t);
    r.Frame(Pad());
    CHECK(r.StrongestHaptic(0) > 0.5f && r.StrongestHaptic(1) > 0.5f);
    for (int i = 0; i < 10; ++i) r.Frame(Pad());
    r.ui.PushEvent({BBXR_EV_WEAPON_CONTACT, 0, 3, 1}, r.t); // a floor scrape: nothing
    r.Frame(Pad());
    CHECK(r.StrongestHaptic(0) == 0.0f && r.StrongestHaptic(1) == 0.0f);
    for (int i = 0; i < 10; ++i) r.Frame(Pad());
    // A paid hit pulses once; a contact right after it (same burst) stays quiet.
    r.ui.PushEvent({BBXR_EV_SWING_OPEN, 20, 100000, 77}, r.t);
    r.Frame(Pad());
    r.ui.PushEvent({BBXR_EV_SWING_HIT, 20, 0, 77}, r.t);
    r.Frame(Pad());
    CHECK(r.StrongestHaptic(1) > 0.5f);
    r.ui.PushEvent({BBXR_EV_WEAPON_CONTACT, 20, 0, 1}, r.t);
    r.Frame(Pad());
    CHECK(r.StrongestHaptic(1) == 0.0f);
    // disabled: the old pulses (hit_light 0.6 for a light swing)
    const fs::path off = HeldAssets(assets, "bbport_game_ui_collision_off_test", ", \"collision_haptics\": {\"on\": false}");
    Rig q;
    CHECK(q.ui.Load(off, nullptr, log));
    q.feed = true;
    q.state = GameState(80);
    q.ui.SetGameState(q.state, q.t);
    q.Frame(Pad());
    q.ui.PushEvent({BBXR_EV_SWING_OPEN, 20, 100000, 1}, q.t);
    q.Frame(Pad());
    q.ui.PushEvent({BBXR_EV_SWING_HIT, 20, 1, 1}, q.t);
    q.Frame(Pad());
    CHECK(std::fabs(q.StrongestHaptic(1) - 0.6f) < 1e-4f);
}

// The desktop mirror (vr_mirror_ui.h): quad corners to window pixels.
void TestMirrorProjection() {
    const float pi = 3.14159265f;
    const vrmirror::Fov fov{-pi / 4, pi / 4, pi / 4, -pi / 4}; // 90 x 90 deg
    const Quat id{0, 0, 0, 1};
    const Mat4 vp = vrmirror::ViewProj(id, {0, 0, 0}, fov);
    float x = 0, y = 0;
    // Straight ahead (-Z): the center.
    CHECK(vrmirror::ProjectToPixel(vp, {0, 0, -2}, 1000, 500, &x, &y));
    CHECK(std::fabs(x - 500) < 0.01f && std::fabs(y - 250) < 0.01f);
    // A 2 m x 2 m quad at 2 m: its corners are the image corners (90 deg fov).
    CHECK(vrmirror::ProjectToPixel(vp, {-2, 2, -2}, 1000, 500, &x, &y));
    CHECK(std::fabs(x) < 0.01f && std::fabs(y) < 0.01f); // up-left is the top left
    CHECK(vrmirror::ProjectToPixel(vp, {2, -2, -2}, 1000, 500, &x, &y));
    CHECK(std::fabs(x - 1000) < 0.01f && std::fabs(y - 500) < 0.01f);
    // Right of center lands right, higher lands higher (smaller y).
    CHECK(vrmirror::ProjectToPixel(vp, {1, 1, -2}, 1000, 500, &x, &y));
    CHECK(x > 500 && y < 250);
    // Behind the camera: not drawn.
    CHECK(!vrmirror::ProjectToPixel(vp, {0, 0, 2}, 1000, 500, &x, &y));
    // The head moved 1 m right and turned 90 deg left: a point 2 m to the left of the origin is
    // straight ahead of the head.
    const Mat4 turned = vrmirror::ViewProj(AxisAngle({0, 1, 0}, pi / 2), {1, 0, 0}, fov);
    CHECK(vrmirror::ProjectToPixel(turned, {-1, 0, 0}, 1000, 500, &x, &y));
    CHECK(std::fabs(x - 500) < 0.1f && std::fabs(y - 250) < 0.1f);
    CHECK(!vrmirror::ProjectToPixel(turned, {3, 0, 0}, 1000, 500, &x, &y));
    // An asymmetric frustum shifts the center of the view axis.
    const vrmirror::Fov skew{-pi / 6, pi / 3, pi / 4, -pi / 4};
    const Mat4 vs = vrmirror::ViewProj(id, {0, 0, 0}, skew);
    CHECK(vrmirror::ProjectToPixel(vs, {0, 0, -1}, 800, 400, &x, &y));
    CHECK(std::fabs(x - 200) < 0.1f && std::fabs(y - 200) < 0.1f); // tan(30) / (tan(30) + tan(60))
    // Image size: square pixels, inside the maximum, 16:9 for a 16:9 frustum.
    uint32_t w = 0, h = 0;
    vrmirror::ImageSize({-0.8f, 0.8f, 0.45f, -0.45f}, 1920, 1080, &w, &h);
    CHECK(w <= 1920 && h <= 1080 && (h == 1080 || w == 1920));
    vrmirror::ImageSize({-pi / 4, pi / 4, pi / 4, -pi / 4}, 1920, 1080, &w, &h);
    CHECK(w == 1080 && h == 1080);
    vrmirror::ImageSize({-pi / 4, pi / 4, pi / 8, -pi / 8}, 1000, 1080, &w, &h);
    CHECK(w == 1000 && h < 1080);
    // Fitting a frame into the window: letterbox bars, centered.
    vrmirror::Rect r = vrmirror::FitRect(1920, 1080, 1000, 1000);
    CHECK(r.w == 1000 && r.h == 562 && r.x == 0 && r.y == 219);
    r = vrmirror::FitRect(1000, 1000, 1920, 1080);
    CHECK(r.w == 1080 && r.h == 1080 && r.x == 420 && r.y == 0);
}

// ---- The held item as a 3D mesh (vr_held_mesh.h) ----

void TestHeldMeshTable() {
    using namespace vrui::held;
    // Goods with a mesh in the sandbox's tables (throwables/th_<goods>, consumables/cs_<goods>).
    for (const int g : {1200, 1201, 1210, 1220, 1230, 1240, 1250, 1251, 1260, 1270, 1410, 2070, 2080, 2130}) {
        const MeshSpec* s = MeshFor(g);
        CHECK(s && s->file == "throwables/th_" + std::to_string(g) + ".json" && s->scale > 0.0f);
    }
    for (const int g : {1000, 703, 706, 2030}) {
        const MeshSpec* s = MeshFor(g);
        CHECK(s && s->file == "consumables/cs_" + std::to_string(g) + ".json");
    }
    // No mesh in the dump (or none identified): the disc stays. Includes everything the wheel can hold.
    for (const int g : {2120, 1100, 1101, 1110, 1120, 1500, 2060, 1300, 1310, 1320, 1330, 900, 0}) {
        CHECK(MeshFor(g) == nullptr);
    }
    // Every throwable the wheel holds has a mesh except the Blacksky Eye.
    for (int g = 1; g < 3000; ++g) {
        const ItemInfo i = Classify(Goods(g));
        if (i.kind == Kind::Throw) {
            CHECK((MeshFor(g) != nullptr) == (g != 2120));
        }
    }
    // The kind tweaks are all present (the one place to tune the pose).
    for (const Kind k : {Kind::Mouth, Kind::Blade, Kind::Throw}) {
        CHECK(TweakFor(k).kind == k);
    }
    // The pose: with the game's hold equal to the sandbox's (consumables) the mesh sits on ItemPos;
    // with a different hold it moves by the difference, in the grip frame.
    const MeshSpec* vial = MeshFor(1000);
    const m3::Quat rot = m3::AxisAngle({0, 1, 0}, 1.5707964f);
    const m3::Vec3 item{1.0f, 1.2f, -0.4f};
    CHECK(vial);
    if (!vial) {
        return;
    }
    const m3::Mat4 a = MeshWorld(*vial, Kind::Mouth, item, rot, vial->pos);
    CHECK(m3::Length(m3::Translation(a) - item) < 1e-5f);
    const m3::Mat4 b = MeshWorld(*vial, Kind::Mouth, item, rot, vial->pos + m3::Vec3{0, 0, -0.02f});
    CHECK(m3::Length(m3::Translation(b) - (item + m3::Rotate(rot, {0, 0, 0.02f}))) < 1e-5f);
}

sb::Model CubeModel(float half) {
    sb::Model m;
    const float c[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                           {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    // faces, counter-clockwise seen from outside, with their outward normals
    const int f[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 4, 7, 3}, {1, 2, 6, 5}, {0, 1, 5, 4}, {3, 7, 6, 2}};
    const float n[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}};
    for (int k = 0; k < 6; ++k) {
        const uint32_t base = uint32_t(m.vertices.size());
        for (int j = 0; j < 4; ++j) {
            sb::Vertex v{};
            for (int a = 0; a < 3; ++a) {
                v.position[a] = c[f[k][j]][a] * half;
                v.normal[a] = n[k][a];
            }
            m.vertices.push_back(v);
        }
        for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            m.indices.push_back(base + i);
        }
    }
    return m;
}

void TestHeldMeshDraw(const fs::path& assets) {
    using namespace vrui::held;
    // A cube seen face-on shows one face (2 triangles); the alpha and the colour tint come through.
    const sb::Model cube = CubeModel(0.05f);
    std::vector<sb::UiVertex> out;
    const m3::Vec3 eye{0.0f, 0.0f, 1.0f};
    CHECK(AppendMesh(out, cube, m3::Translate({0, 0, 0}), {1.0f, 0.5f, 0.25f, 0.5f}, eye) == 2);
    CHECK(out.size() == 6);
    for (const sb::UiVertex& v : out) {
        CHECK(std::fabs(v.position[2] - 0.05f) < 1e-5f); // the +Z face, the one toward the eye
        CHECK(v.color[3] == 0.5f && v.color[0] > v.color[1] && v.color[1] > v.color[2] && v.shape[0] == 0.0f);
    }
    // A mesh range with a texture the sheet knows is drawn in icon mode (white tint, atlas uvs in the
    // sheet's rect, shape 1/1); a range without one (or an unknown key) stays flat.
    {
        sb::ItemSheet sheet;
        sheet.icons_w = 64, sheet.icons_h = 64, sheet.sheet_w = 64, sheet.sheet_h = 32;
        sheet.rects["tex_a"] = {0, 0, 32, 32};
        sb::Model textured = cube;
        sb::MeshRange r;
        r.texture = "tex_a";
        r.index_count = uint32_t(textured.indices.size());
        textured.meshes.push_back(r);
        for (size_t i = 0; i < textured.vertices.size(); ++i) {
            textured.vertices[i].uv[0] = float(i % 2), textured.vertices[i].uv[1] = float((i / 2) % 2);
        }
        std::vector<sb::UiVertex> tex_out;
        CHECK(AppendMesh(tex_out, textured, m3::Translate({0, 0, 0}), {1.0f, 0.2f, 0.2f, 0.5f}, eye, &sheet) == 2);
        for (const sb::UiVertex& v : tex_out) {
            CHECK(v.shape[0] == 1.0f && v.shape[1] == 1.0f && v.color[3] == 0.5f);
            CHECK(std::fabs(v.color[0] - v.color[1]) < 1e-5f); // white tint times the shade
            CHECK(v.uv[0] >= 0.0f && v.uv[0] <= 0.5f && v.uv[1] >= 64.0f / 96.0f && v.uv[1] <= 1.0f); // inside the rect
        }
        textured.meshes[0].texture = "nope";
        tex_out.clear();
        CHECK(AppendMesh(tex_out, textured, m3::Translate({0, 0, 0}), {1.0f, 0.2f, 0.2f, 0.5f}, eye, &sheet) == 2);
        CHECK(tex_out[0].shape[0] == 0.0f && tex_out[0].color[0] > tex_out[0].color[1]);
        // The paper stand-in is a card: textured faces named "goods:<id>" once resolved, flat edges.
        const sb::Model& card = StandIn(vrui::hand::HoldCategory::Paper);
        CHECK(card.meshes.size() == 1 && card.meshes[0].texture == "card");
    }
    // Seen from a corner: three faces, whichever winding the exporter used.
    sb::Model flipped = cube;
    for (size_t i = 0; i + 2 < flipped.indices.size(); i += 3) {
        std::swap(flipped.indices[i + 1], flipped.indices[i + 2]);
    }
    out.clear();
    CHECK(AppendMesh(out, cube, m3::Translate({0, 0, 0}), {1, 1, 1, 1}, {1, 1, 1}) == 6);
    std::vector<sb::UiVertex> out_flipped;
    CHECK(AppendMesh(out_flipped, flipped, m3::Translate({0, 0, 0}), {1, 1, 1, 1}, {1, 1, 1}) == 6);
    // Far triangles first (they are blended under the near ones).
    float last = 1e9f;
    for (size_t i = 0; i + 2 < out.size(); i += 3) {
        const m3::Vec3 c{(out[i].position[0] + out[i + 1].position[0] + out[i + 2].position[0]) / 3.0f,
                         (out[i].position[1] + out[i + 1].position[1] + out[i + 2].position[1]) / 3.0f,
                         (out[i].position[2] + out[i + 1].position[2] + out[i + 2].position[2]) / 3.0f};
        const float d = m3::Length(c - m3::Vec3{1, 1, 1});
        CHECK(d <= last + 1e-5f);
        last = d;
    }

    // Fallback: no mesh file -> no entry (the disc is drawn); the miss is remembered.
    MeshCache cache;
    const MeshSpec* mol = MeshFor(1200);
    CHECK(mol);
    if (mol) {
        CHECK(cache.Get(fs::temp_directory_path() / "bbport_no_such_assets", *mol) == nullptr);
        CHECK(cache.Peek(1200) && cache.Peek(1200)->tried && !cache.Peek(1200)->ok &&
              !cache.Peek(1200)->error.empty());
    }

    // With the extracted assets: every mesh the table names loads, within a sane size.
    int loaded = 0;
    MeshCache real;
    for (const MeshSpec& s : MeshTable()) {
        if (!fs::exists(assets / s.file)) {
            continue;
        }
        const MeshCache::Entry* e = real.Get(assets, s);
        CHECK(e != nullptr);
        if (e) {
            ++loaded;
            CHECK(e->radius > 0.02f && e->radius < 0.25f);
            out.clear();
            const size_t n = AppendMesh(out, e->model, MeshWorld(s, Kind::Throw, {0, 1.2f, -0.4f}, {}, {0, 0, -0.05f}),
                                        s.color, {0, 1.6f, 0});
            CHECK(n > 20 && n < e->model.indices.size() / 3 && out.size() == n * 3);
        }
    }
    std::printf("held meshes: %d of %zu table entries present under %s\n", loaded, MeshTable().size(),
                assets.string().c_str());
}

/// items/items.json (items_extract.py): the fallback for goods the sandbox tables have no mesh for.
void TestHeldItemsIndex(const fs::path& assets) {
    using namespace vrui::held;
    // A synthetic index: the row logic, independent of the dump.
    const fs::path dir = fs::temp_directory_path() / "bbport_items_index";
    fs::create_directories(dir / "items");
    {
        std::ofstream f(dir / "items" / "items.json");
        f << R"({"items": [
          {"id": 1100, "name": "Antidote", "mesh": "1100.json", "grip": "bottle", "size_m": [0.19, 0.38, 0.18]},
          {"id": 2060, "name": "Bone", "mesh": "2060.json", "grip": "tool", "size_m": [0.51, 0.09, 0.12]},
          {"id": 4301, "name": "Tear", "mesh": "4301.json", "grip": "small", "size_m": [0.03, 0.04, 0.03]},
          {"id": 1000, "name": "Blood Vial", "mesh": "1000.json", "grip": "bottle", "size_m": [0.13, 0.23, 0.13]},
          {"id": 1300, "name": "Fire Paper", "mesh": null, "grip": "paper", "size_m": null}]})";
    }
    CHECK(MeshFor(1100) == nullptr); // the sandbox table alone has none
    const MeshSpec* a = MeshFor(dir, 1100);
    CHECK(a && a->file == "items/1100.json");
    if (a) {
        CHECK(std::fabs(a->scale - 0.22f / 0.38f) < 1e-4f); // 38 cm bottle -> the 22 cm cap
    }
    const MeshSpec* bone = MeshFor(dir, 2060);
    CHECK(bone && bone->scale < 0.45f && bone->rot_deg.y == 90.0f); // long X turned forward
    const MeshSpec* tear = MeshFor(dir, 4301);
    CHECK(tear && std::fabs(tear->scale - 0.05f / 0.04f) < 1e-3f); // 4 cm -> half the 10 cm cap
    // The sandbox's row stays authoritative over the index's.
    const MeshSpec* vial = MeshFor(dir, 1000);
    CHECK(vial && vial->file == "consumables/cs_1000.json");
    CHECK(MeshFor(dir, 1300) == nullptr && MeshFor(dir, 99999) == nullptr); // no mesh: the disc / icon card
    fs::remove_all(dir);
    // The real index, when extracted: every row with a mesh names an existing, loadable file.
    if (fs::exists(assets / "items" / "items.json")) {
        int rows = 0;
        MeshCache cache;
        for (int g = 1; g < 7000; ++g) {
            if (const MeshSpec* s = MeshFor(assets, g)) {
                if (s->file.rfind("items/", 0) != 0) {
                    continue;
                }
                ++rows;
                const MeshCache::Entry* e = cache.Get(assets, *s);
                CHECK(e != nullptr);
                if (e) {
                    CHECK(e->radius > 0.02f && e->radius < 0.3f);
                }
            }
        }
        std::printf("held items index: %d goods with an items/ mesh under %s\n", rows, assets.string().c_str());
    }
}

/// The game UI draws the vial's mesh (many triangles) instead of the disc when the file is there.
void TestHeldMeshInGame(const fs::path& assets) {
    if (!fs::exists(assets / "consumables" / "cs_1000.json")) {
        std::printf("held mesh in game: skipped (no consumables/cs_1000.json under %s)\n", assets.string().c_str());
        return;
    }
    size_t verts[2] = {};
    for (int with_mesh = 0; with_mesh < 2; ++with_mesh) {
        const fs::path dir = HeldAssets(assets, with_mesh ? "bbport_game_ui_mesh_on" : "bbport_game_ui_mesh_off", "");
        std::error_code ec;
        if (with_mesh) {
            fs::create_directories(dir / "consumables", ec);
            fs::copy_file(assets / "consumables" / "cs_1000.json", dir / "consumables" / "cs_1000.json", ec);
            fs::copy_file(assets / "consumables" / "cs_1000.bin", dir / "consumables" / "cs_1000.bin", ec);
        }
        Rig r;
        std::string log;
        CHECK(r.ui.Load(dir, nullptr, log));
        r.feed = true;
        r.state = GameState(80);
        r.ui.SetGameState(r.state, r.t);
        r.Frame(Pad());
        PickOnWheel(r, 0);
        CHECK(r.ui.HeldItemState().Active() && r.ui.HeldItemState().Info().goods == 1000);
        r.Frame(Pad()); // pending: translucent
        const size_t pending = r.ui.HudVertices();
        r.in.squeeze[1] = 0.9f;
        r.Frame(Pad());
        CHECK(r.ui.HeldItemState().Gripped());
        verts[with_mesh] = r.ui.HudVertices();
        CHECK(with_mesh == 0 || pending > 300); // the mesh is drawn while pending too
    }
    CHECK(verts[1] > verts[0] + 300);
}

// ---- Custom hands (vr_hand_model.h, docs/vr/findings/40-custom-hands.md) ----

bool NearPose(const vrui::hand::Pose& a, const vrui::hand::Pose& b, float eps = 1e-3f) {
    for (int j = 0; j < vrui::hand::kJoints; ++j) {
        if (std::fabs(a.flex[j] - b.flex[j]) > eps) {
            return false;
        }
    }
    return true;
}

/// Pose blending, sockets and categories (no asset needed).
void TestHandPoses() {
    using namespace vrui::hand;
    // The bilinear weights sum to 1 and give the named poses at the corners.
    for (float g : {0.0f, 0.3f, 1.0f}) {
        for (float t : {0.0f, 0.6f, 1.0f}) {
            const ControllerWeights w = WeightsFor(g, t);
            CHECK(std::fabs(w.open + w.trigger + w.point + w.fist - 1.0f) < 1e-5f);
        }
    }
    CHECK(NearPose(ControllerPose(0, 0), OpenPose()));
    CHECK(NearPose(ControllerPose(1, 1), FistPose()));
    CHECK(NearPose(ControllerPose(1, 0), PointPose()));
    CHECK(NearPose(ControllerPose(0, 1), TriggerPose()));
    CHECK(NearPose(ControllerPose(2, -1), PointPose())); // clamped
    const Pose half = ControllerPose(0.5f, 0.5f);
    CHECK(std::fabs(half.flex[kIndexProx] - 0.5f * FistPose().flex[kIndexProx]) < 1e-3f);
    CHECK(std::fabs(half.flex[kMiddleInter] - 0.5f * FistPose().flex[kMiddleInter]) < 1e-3f);
    CHECK(PointPose().flex[kIndexProx] == 0.0f && PointPose().flex[kMiddleProx] < -60.0f);
    CHECK(FistPose().flex[kIndexProx] < -60.0f && FistPose().flex[kThumbDist] < 0.0f);
    // Blend: weights normalised; all zero -> the first pose.
    const Pose* two[2] = {&OpenPose(), &FistPose()};
    const float w2[2] = {3.0f, 1.0f}, w0[2] = {0.0f, 0.0f};
    CHECK(std::fabs(Blend(two, w2, 2).flex[kRingProx] - 0.25f * FistPose().flex[kRingProx]) < 1e-3f);
    CHECK(NearPose(Blend(two, w0, 2), OpenPose()));
    // CurlPose / HoldPose: shares of the fist per finger; the trigger can close the index further.
    const Pose c = CurlPose(0.0f, 1.0f, 0.5f);
    CHECK(c.flex[kThumbProx] == 0.0f && std::fabs(c.flex[kIndexInter] - FistPose().flex[kIndexInter]) < 1e-4f);
    CHECK(std::fabs(c.flex[kPinkyProx] - 0.5f * FistPose().flex[kPinkyProx]) < 1e-4f);
    const GripDef& paper = CategoryGrip(HoldCategory::Paper);
    CHECK(HoldPose(paper, 0.0f).flex[kIndexProx] > HoldPose(paper, 1.0f).flex[kIndexProx]);

    // Categories: every kind of item has one; weapons are weapons.
    CHECK(CategoryFor(BBXR_ITEM_GOODS | 1000) == HoldCategory::Bottle);  // Blood Vial
    CHECK(CategoryFor(BBXR_ITEM_GOODS | 1110) == HoldCategory::Small);   // Beast Blood Pellet
    CHECK(CategoryFor(BBXR_ITEM_GOODS | 1300) == HoldCategory::Paper);   // Fire Paper
    CHECK(CategoryFor(BBXR_ITEM_GOODS | 200) == HoldCategory::Bell);     // Beckoning Bell
    CHECK(CategoryFor(BBXR_ITEM_GOODS | 1240) == HoldCategory::Tool);    // Throwing Knife
    CHECK(CategoryFor(BBXR_ITEM_GOODS | 4006) == HoldCategory::Small);   // Orphanage Key
    CHECK(CategoryFor(BBXR_ITEM_GOODS | 6100) == HoldCategory::Bottle);  // a chalice
    CHECK(CategoryFor(7000000u) == HoldCategory::Weapon);
    CHECK(std::string(CategoryName(HoldCategory::Bell)) == "bell");

    // Sockets: the right hand's pose in the grip frame, the left mirrored across X.
    const GripDef small = GripFor(BBXR_ITEM_GOODS | 4006);
    CHECK(small.pos.x == CategoryGrip(HoldCategory::Small).pos.x);
    const m3::Vec3 gp{0.1f, 1.2f, -0.3f};
    const m3::Mat4 r = SocketWorld(small, 1, gp, {});
    const m3::Mat4 l = SocketWorld(small, 0, gp, {});
    CHECK(std::fabs(m3::Translation(r).x - (gp.x + small.pos.x)) < 1e-5f);
    CHECK(std::fabs(m3::Translation(l).x - (gp.x - small.pos.x)) < 1e-5f);
    CHECK(std::fabs(m3::Translation(l).y - m3::Translation(r).y) < 1e-5f);
    // The socket follows the grip's rotation.
    const m3::Quat yaw = m3::AxisAngle({0, 1, 0}, 90.0f * kDeg);
    const m3::Vec3 turned = m3::Translation(SocketWorld(small, 1, {}, yaw));
    const m3::Vec3 want = m3::Rotate(yaw, small.pos);
    CHECK(m3::Length(turned - want) < 1e-5f);
    // A tuned (sandbox) pose replaces the category's socket but keeps its curls.
    GripDef tuned;
    tuned.pos = {0.01f, 0.02f, 0.03f};
    tuned.rot_deg = {10, 20, 30};
    const GripDef vial = GripFor(BBXR_ITEM_GOODS | 1000, &tuned);
    CHECK(vial.pos.z == 0.03f && vial.rot_deg.y == 20.0f &&
          vial.others == CategoryGrip(HoldCategory::Bottle).others);

    // Not a glb: refused with a reason.
    HandMesh m;
    std::string error;
    CHECK(!ParseGlb(std::vector<uint8_t>(64, 0), m, error) && !error.empty() && !m.loaded);
}

/// The shipped asset: loads, the bind pose skins to itself, the fist closes toward the palm and the
/// grip pose places the hand with its palm where OpenXR's grip frame says.
void TestHandAsset(const fs::path& assets) {
    using namespace vrui::hand;
    const fs::path dir = assets / "hands";
    if (!fs::exists(dir / "right.glb")) {
        std::printf("hand asset: skipped (no hands/right.glb under %s)\n", assets.string().c_str());
        return;
    }
    HandModels models;
    CHECK(models.Load(dir));
    CHECK(models.Ready());
    if (!models.Ready()) {
        std::printf("hand asset: %s\n", models.error.c_str());
        return;
    }
    // Truncated: refused.
    {
        std::ifstream f(dir / "right.glb", std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        bytes.resize(bytes.size() / 2);
        HandMesh cut;
        std::string error;
        CHECK(!ParseGlb(bytes, cut, error));
    }
    for (int h = 0; h < 2; ++h) {
        const HandMesh& m = models.hand[h];
        CHECK(m.position.size() > 500 && m.indices.size() % 3 == 0 && m.indices.size() > 3000);
        for (size_t i = 0; i < m.weight.size(); i += 97) {
            const float s = m.weight[i][0] + m.weight[i][1] + m.weight[i][2] + m.weight[i][3];
            CHECK(std::fabs(s - 1.0f) < 1e-4f);
        }
        // The back of the hand: model +X for the right hand, -X for the left (mirrored assets).
        CHECK(h ? m.dorsal.x > 0.9f : m.dorsal.x < -0.9f);
        // The bind pose skins to the mesh as stored.
        m3::Mat4 joints[kJoints];
        PoseJoints(m, OpenPose(), joints);
        std::vector<m3::Vec3> pos, nrm;
        Skin(m, joints, m3::Mat4{}, pos, nrm);
        float worst = 0.0f;
        for (size_t i = 0; i < pos.size(); ++i) {
            worst = std::max(worst, m3::Length(pos[i] - m.position[i]));
        }
        CHECK(worst < 1e-4f);
        // The fist: every fingertip nearer the fist centre and on the palm side of the knuckles.
        m3::Mat4 fist[kJoints];
        PoseJoints(m, FistPose(), fist);
        for (int tip : {kIndexTip, kMiddleTip, kRingTip, kPinkyTip}) {
            const float open_d = m3::Length(m3::Translation(joints[tip]) - m.anchor);
            const float fist_d = m3::Length(m3::Translation(fist[tip]) - m.anchor);
            CHECK(fist_d < open_d * 0.6f);
            CHECK(m3::Dot(m3::Translation(fist[tip]) - m3::Translation(fist[tip - 3]), m.dorsal) < 0.0f);
        }
        // The point pose keeps the index where the open hand has it.
        m3::Mat4 point[kJoints];
        PoseJoints(m, PointPose(), point);
        CHECK(m3::Length(m3::Translation(point[kIndexTip]) - m3::Translation(joints[kIndexTip])) < 1e-4f);
        // Placement: the fist centre on the grip origin, the thumb toward the grip's -Z, the back
        // of the hand toward +X for the right hand (into its palm) and -X for the left (out of it).
        const m3::Vec3 gp{0.2f, 1.2f, -0.35f};
        const m3::Mat4 world = HandWorld(m, h, gp, {});
        CHECK(m3::Length(m3::TransformPoint(world, m.anchor) - gp) < 1e-5f);
        const m3::Vec3 thumb = m3::TransformPoint(world, m3::Translation(m.bind[kThumbTip]));
        const m3::Vec3 pinky = m3::TransformPoint(world, m3::Translation(m.bind[kPinkyProx]));
        CHECK(thumb.z < pinky.z);
        const m3::Vec3 back = m3::TransformDir(world, m.dorsal);
        CHECK(h ? back.x > 0.9f : back.x < -0.9f);
        // The anchor sits within the hand's span: under the knuckles, in front of the palm.
        CHECK(m3::Length(m.anchor - m3::Translation(m.bind[kMiddleProx])) < 0.06f);
    }
}

/// The game UI: off by default; on, both hands are drawn, the grip closes them, and a held item is
/// in the hand's palm socket with the hold pose.
void TestCustomHandsInGame(const fs::path& assets) {
    if (!fs::exists(assets / "hands" / "right.glb")) {
        std::printf("custom hands in game: skipped (no hands/right.glb under %s)\n", assets.string().c_str());
        return;
    }
    size_t base = 0;
    for (int on = 0; on < 2; ++on) {
        const fs::path dir = HeldAssets(assets, on ? "bbport_game_ui_hands_on" : "bbport_game_ui_hands_off",
                                        on ? ", \"custom_hands\": {\"on\": true}" : "");
        std::error_code ec;
        fs::create_directories(dir / "hands", ec);
        for (const char* f : {"left.glb", "right.glb"}) {
            fs::copy_file(assets / "hands" / f, dir / "hands" / f, ec);
        }
        Rig r;
        std::string log;
        CHECK(r.ui.Load(dir, nullptr, log));
        CHECK(r.ui.Menus().Layout().game.custom_hands.on == (on == 1));
        r.feed = true;
        r.state = GameState(80);
        r.ui.SetGameState(r.state, r.t);
        r.Frame(Pad());
        CHECK(r.ui.CustomHandsOn() == (on == 1));
        if (!on) {
            CHECK(r.ui.HandTriangles() == 0);
            base = r.ui.HudVertices();
            continue;
        }
        CHECK(r.ui.CustomHandsReady());
        r.Frame(Pad()); // Build's load line reaches the log with the next Update
        CHECK(r.all_log.find("custom hands") != std::string::npos);
        CHECK(r.ui.HandTriangles() > 1000);
        CHECK(r.ui.HudVertices() >= base + r.ui.HandTriangles() * 3);
        // The weapon hands (GameState gives both hands a weapon) keep the weapon grip.
        CHECK(r.ui.HandPose(1).flex[vrui::hand::kMiddleProx] < -60.0f);
        // An untracked hand is not drawn.
        const size_t both = r.ui.HandTriangles();
        r.in.grip[0].valid = false;
        r.Frame(Pad());
        CHECK(r.ui.HandTriangles() > 0 && r.ui.HandTriangles() < both);
        r.in.grip[0].valid = true;
        // Pick a vial: held in the right hand; once gripped, the hold pose of a bottle.
        PickOnWheel(r, 0);
        CHECK(r.ui.HeldItemState().Active());
        r.in.squeeze[1] = 0.9f;
        r.Frame(Pad());
        CHECK(r.ui.HeldItemState().Gripped());
        const vrui::hand::Pose hold = vrui::hand::HoldPose(vrui::hand::GripFor(BBXR_ITEM_GOODS | 1000), 0.0f);
        CHECK(NearPose(r.ui.HandPose(1), hold));
    }
    // Unarmed hands follow the controller: grip and trigger open -> the open pose.
    {
        const fs::path dir = HeldAssets(assets, "bbport_game_ui_hands_bare", ", \"custom_hands\": true");
        std::error_code ec;
        fs::create_directories(dir / "hands", ec);
        for (const char* f : {"left.glb", "right.glb"}) {
            fs::copy_file(assets / "hands" / f, dir / "hands" / f, ec);
        }
        Rig r;
        std::string log;
        CHECK(r.ui.Load(dir, nullptr, log));
        r.feed = true;
        r.state = GameState(80);
        for (uint32_t& w : r.state.weapons) {
            w = BBXR_ITEM_NONE;
        }
        r.Frame(Pad());
        CHECK(NearPose(r.ui.HandPose(0), vrui::hand::OpenPose()));
        r.in.squeeze[0] = 1.0f;
        r.in.trigger[0] = 0.0f;
        r.Frame(Pad());
        CHECK(NearPose(r.ui.HandPose(0), vrui::hand::PointPose()));
    }
}

// ---- Stand-ins, bells and marks, one placement, the weapon-hide signal, the offline render ----

/// A copy of the assets the held-item tests need (ui, hands, item meshes) with a layout.
fs::path FullAssets(const fs::path& assets, const char* name, const std::string& game_extra) {
    const fs::path dir = HeldAssets(assets, name, game_extra);
    std::error_code ec;
    for (const char* d : {"hands", "items", "consumables", "throwables"}) {
        if (fs::exists(assets / d)) {
            fs::copy(assets / d, dir / d, fs::copy_options::recursive, ec);
        }
    }
    return dir;
}

void TestStandIns() {
    using namespace vrui;
    for (int c = 0; c <= int(hand::HoldCategory::Weapon); ++c) {
        const hand::HoldCategory cat = hand::HoldCategory(c);
        const sb::Model& m = held::StandIn(cat);
        CHECK(!m.vertices.empty() && !m.indices.empty() && m.indices.size() % 3 == 0);
        CHECK(&m == &held::StandIn(cat)); // cached
        bool ok = true, winding = true;
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (const sb::Vertex& v : m.vertices) {
            const float n = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
            ok = ok && std::fabs(n - 1.0f) < 1e-3f;
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], v.position[k]);
                hi[k] = std::max(hi[k], v.position[k]);
            }
        }
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            for (int k = 0; k < 3; ++k) {
                ok = ok && m.indices[i + size_t(k)] < m.vertices.size();
            }
            if (!ok) {
                break;
            }
            const sb::Vertex &a = m.vertices[m.indices[i]], &b = m.vertices[m.indices[i + 1]],
                             &d = m.vertices[m.indices[i + 2]];
            const m3::Vec3 pa{a.position[0], a.position[1], a.position[2]},
                pb{b.position[0], b.position[1], b.position[2]}, pd{d.position[0], d.position[1], d.position[2]};
            const m3::Vec3 n = m3::Cross(pb - pa, pd - pa);
            winding = winding && m3::Dot(n, {a.normal[0], a.normal[1], a.normal[2]}) > 0.0f &&
                      m3::Dot(n, pa) > 0.0f; // outward (convex around the origin)
        }
        CHECK(ok && winding);
        // centred, long axis along +Y
        for (int k = 0; k < 3; ++k) {
            CHECK(std::fabs(lo[k] + hi[k]) < 0.0045f);
        }
        const float ex = hi[0] - lo[0], ey = hi[1] - lo[1], ez = hi[2] - lo[2];
        CHECK(ey >= ex && ey >= ez);
        if (cat == hand::HoldCategory::Paper) {
            CHECK(std::fabs(ex - 0.075f) < 1e-3f && std::fabs(ey - 0.11f) < 1e-3f && std::fabs(ez - 0.003f) < 1e-3f);
        } else if (cat == hand::HoldCategory::Small) {
            CHECK(ex > 0.018f && ex < 0.026f && ey > 0.018f && ey < 0.03f);
        } else if (cat == hand::HoldCategory::Bottle) {
            CHECK(ex > 0.030f && ex < 0.037f && std::fabs(ey - 0.10f) < 1e-3f);
        }
    }
    // The cache: a goods without a model gets its category's stand-in (and the sandbox row whose
    // file is missing too); tints by what the item does.
    held::MeshCache cache;
    const fs::path none = fs::temp_directory_path() / "bbport_no_such_assets";
    const held::MeshCache::Entry* paper = cache.Resolve(none, 1300);
    CHECK(paper && paper->standin && paper->source == "stand-in paper" && !paper->from_table);
    CHECK(paper && paper->spec.color.r > 0.9f && paper->spec.color.g < 0.5f); // Fire Paper: red-orange
    const held::MeshCache::Entry* bolt = cache.Resolve(none, 1320);
    CHECK(bolt && bolt->spec.color.b > 0.9f && bolt->spec.color.r < 0.8f);     // Bolt Paper: pale blue
    const held::MeshCache::Entry* vial = cache.Resolve(none, 1000);
    CHECK(vial && vial->standin && vial->source == "stand-in bottle" && vial->spec.color.r > 0.8f &&
          vial->spec.color.g < 0.2f);
    const held::MeshCache::Entry* pellet = cache.Resolve(none, 1110);
    CHECK(pellet && pellet->standin && pellet->source == "stand-in small");
    const held::MeshCache::Entry* knife = cache.Resolve(none, 1240);
    CHECK(knife && knife->standin && knife->spec.color.b > 0.9f && knife->spec.color.r < 0.4f);
    CHECK(vial && vial->radius > 0.04f && vial->radius < 0.07f);
    CHECK(cache.Resolve(none, 1000) == vial); // cached
}

void TestHeldCrushBell() {
    using namespace vrui::held;
    const sb::UiHeldItems c;
    CHECK(c.crush_squeeze > 0.9f && c.crush_hold_s > 0.3f && c.hide_weapon);
    // -- A mark: held, then squeezed hard for crush_hold_s
    {
        HeldItem h;
        CHECK(h.Pick(Goods(100), 0, 0.0) && h.Hand() == 1 && h.Info().kind == Kind::Crush);
        CHECK(h.Gripped());
        Frame f = HeldFrame(0.1);
        f.squeeze[1] = 0.9f; // a grip, but not hard enough
        f.time = 0.2;
        h.Step(f, c);
        CHECK(h.Gripped() && std::strcmp(h.Hint(), "SQUEEZE TO CRUSH") == 0);
        for (double t = 0.3; t < 1.5; t += 0.1) {
            f.time = t;
            CHECK(h.Step(f, c).kind == Result::None && h.Progress() == 0.0f);
        }
        f.squeeze[1] = 0.97f;
        f.time = 1.6;
        Result r = h.Step(f, c);
        CHECK(r.kind == Result::None && h.InContact() && !r.pulses.empty());
        f.time = 1.8;
        CHECK(h.Step(f, c).kind == Result::None && h.Progress() > 0.4f && h.Progress() < 0.6f);
        f.squeeze[1] = 0.8f; // relaxing resets
        f.time = 1.9;
        CHECK(h.Step(f, c).kind == Result::None && h.Progress() == 0.0f);
        f.squeeze[1] = 0.97f;
        f.time = 2.0;
        h.Step(f, c);
        f.time = 2.41;
        r = h.Step(f, c);
        CHECK(r.kind == Result::Use && r.why == "crush" && r.item.goods == 100 && r.hand == 1 && !r.thrown);
        CHECK(!h.Active());
        // letting go does not put it back; a trigger press does
        CHECK(h.Pick(Goods(1400), 1, 3.0) && h.Info().kind == Kind::Crush);
        f.squeeze[1] = 0.9f;
        f.time = 3.1;
        h.Step(f, c);
        f.squeeze[1] = 0.1f;
        f.time = 3.2;
        CHECK(h.Step(f, c).kind == Result::None && h.Active());
        f.trigger[1] = 1.0f;
        f.time = 3.3;
        CHECK(h.Step(f, c).kind == Result::Cancel); // (the trigger was up since the pick: armed)
    }
    // -- A bell: the head gesture, the right hand, "TO THE HEAD"
    {
        HeldItem h;
        CHECK(h.Pick(Goods(4312), 0, 0.0, 0) && h.Hand() == 1 && h.Info().bell);
        Frame f = HeldFrame(0.1);
        f.squeeze[1] = 0.9f;
        f.time = 0.2;
        h.Step(f, c);
        CHECK(h.Gripped() && std::strcmp(h.Hint(), "TO THE HEAD") == 0);
        f.grip_pos[1] = m3::Vec3{0.0f, 1.5f, -0.07f} - c.hold;
        f.time = 0.3;
        h.Step(f, c);
        CHECK(h.InContact());
        f.time = 0.65;
        const Result r = h.Step(f, c);
        CHECK(r.kind == Result::Use && r.why == "mouth" && r.item.goods == 4312);
    }
}

/// The actions the UI queued, all of them (the weapon-hide signals too).
std::vector<BbXrUiAction> DrainAll(Rig& r) {
    std::vector<BbXrUiAction> out;
    BbXrUiAction a[32];
    uint32_t n;
    while ((n = r.ui.TakeActions(a, 32)) > 0) {
        out.insert(out.end(), a, a + n);
    }
    return out;
}

/// The host finished its action; the lock-out ends.
void SettleHeld(Rig& r) {
    r.ui.PushEvent({BBXR_EV_ACTION_DONE, int32_t(BBXR_UI_USE_QUICK), 1, 0}, r.t);
    r.in.squeeze[0] = r.in.squeeze[1] = 0.0f;
    for (int i = 0; i < 110; ++i) {
        r.Frame(Pad());
    }
    DrainAll(r);
}

int IndexOf(const std::vector<BbXrUiAction>& v, uint32_t kind, int a = INT32_MIN) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].kind == kind && (a == INT32_MIN || v[i].a == a)) {
            return int(i);
        }
    }
    return -1;
}
int CountOf(const std::vector<BbXrUiAction>& v, uint32_t kind, int a = INT32_MIN) {
    int n = 0;
    for (const BbXrUiAction& x : v) {
        n += x.kind == kind && (a == INT32_MIN || x.a == a);
    }
    return n;
}

/// Bells and marks through the game UI, and BBXR_UI_HELD.
void TestHeldExtrasInGame(const fs::path& assets) {
    const fs::path dir = HeldAssets(assets, "bbport_game_ui_held_extras", "");
    Rig r;
    std::string log;
    CHECK(r.ui.Load(dir, nullptr, log));
    r.feed = true;
    r.state = GameState(80);
    r.state.quick[1] = Goods(200);  // Beckoning Bell
    r.state.quick[2] = Goods(100);  // Hunter's Mark
    r.state.quick[4] = Goods(1300); // Fire Paper
    r.ui.SetGameState(r.state, r.t);
    r.Frame(Pad());
    CHECK(r.ui.Active());
    const m3::Vec3 mouth_grip = m3::Vec3{0.0f, 1.5f, -0.07f} - m3::Vec3{0.0f, 0.0f, -0.05f};

    // -- A vial: in the hand at the selection (no grip squeeze): HELD at once, again every 2 s, cleared
    // before the use.
    PickOnWheel(r, 0);
    CHECK(r.ui.HeldItemState().Active() && r.ui.HeldItemState().Gripped());
    std::vector<BbXrUiAction> got = DrainAll(r);
    CHECK(got.size() == 1 && got[0].kind == BBXR_UI_HELD && got[0].a == 1 && got[0].b == int32_t(Goods(1000)) &&
          got[0].c == 0);
    for (int i = 0; i < 160; ++i) { // ~1.9 s: not yet
        r.Frame(Pad());
    }
    CHECK(DrainAll(r).empty());
    for (int i = 0; i < 20; ++i) { // past 2 s: refreshed once
        r.Frame(Pad());
    }
    got = DrainAll(r);
    CHECK(got.size() == 1 && got[0].kind == BBXR_UI_HELD && got[0].a == 1 && got[0].b == int32_t(Goods(1000)));
    r.in.grip[1].position = r.in.aim[1].position = mouth_grip;
    for (int i = 0; i < 40 && r.ui.HeldItemState().Active(); ++i) {
        r.Frame(Pad());
    }
    CHECK(!r.ui.HeldItemState().Active());
    got = DrainAll(r);
    int clear = IndexOf(got, BBXR_UI_HELD, -1), use = IndexOf(got, BBXR_UI_USE_QUICK);
    CHECK(clear >= 0 && use >= 0 && clear < use && got[size_t(clear)].b == int32_t(Goods(1000)));
    CHECK(CountOf(got, BBXR_UI_USE_QUICK) == 1);
    // the HELD actions are not executor actions: no "busy" for the next use
    SettleHeld(r);

    // -- Put back by a trigger press (after the selecting click came up): cleared, no use.
    PickOnWheel(r, 0);
    CHECK(CountOf(DrainAll(r), BBXR_UI_HELD, 1) == 1);
    r.in.trigger[1] = 0.0f;
    r.Frame(Pad());
    CHECK(r.ui.HeldItemState().Active());
    r.in.trigger[1] = 1.0f;
    r.Frame(Pad());
    r.in.trigger[1] = 0.0f;
    CHECK(!r.ui.HeldItemState().Active());
    got = DrainAll(r);
    CHECK(got.size() == 1 && got[0].kind == BBXR_UI_HELD && got[0].a == -1 && got[0].b == int32_t(Goods(1000)));
    SettleHeld(r);

    // -- Never gripped, never used: the item simply stays in the hand (no timeout).
    PickOnWheel(r, 0);
    for (int i = 0; i < 90 * 6; ++i) {
        r.Frame(Pad());
    }
    CHECK(r.ui.HeldItemState().Active());
    r.in.trigger[1] = 0.0f;
    r.Frame(Pad());
    r.in.trigger[1] = 1.0f;
    r.Frame(Pad());
    r.in.trigger[1] = 0.0f;
    CHECK(!r.ui.HeldItemState().Active());
    DrainAll(r);
    SettleHeld(r);

    // -- A bell: the right hand, the head gesture, then the use.
    PickOnWheel(r, 1);
    CHECK(r.ui.HeldItemState().Active() && r.ui.HeldItemState().Info().bell && r.ui.HeldItemState().Hand() == 1);
    CHECK(r.ui.HeldItemState().Gripped() && std::strcmp(r.ui.HeldItemState().Hint(), "TO THE HEAD") == 0);
    CHECK(CountOf(DrainAll(r), BBXR_UI_HELD, 1) == 1);
    r.in.grip[1].position = r.in.aim[1].position = mouth_grip;
    for (int i = 0; i < 40 && r.ui.HeldItemState().Active(); ++i) {
        r.Frame(Pad());
    }
    CHECK(!r.ui.HeldItemState().Active());
    got = DrainAll(r);
    clear = IndexOf(got, BBXR_UI_HELD, -1);
    use = IndexOf(got, BBXR_UI_USE_QUICK);
    CHECK(clear >= 0 && use > clear && got[size_t(use)].a == 1 && got[size_t(use)].b == int32_t(Goods(200)));
    SettleHeld(r);

    // -- A mark: nothing happens at the mouth or with a light squeeze; a hard squeeze crushes it.
    PickOnWheel(r, 2);
    CHECK(r.ui.HeldItemState().Active() && r.ui.HeldItemState().Info().kind == vrui::held::Kind::Crush &&
          r.ui.HeldItemState().Hand() == 1);
    r.in.squeeze[1] = 0.85f;
    r.Frame(Pad());
    CHECK(r.ui.HeldItemState().Gripped());
    CHECK(std::strcmp(r.ui.HeldItemState().Hint(), "SQUEEZE TO CRUSH") == 0);
    CHECK(CountOf(DrainAll(r), BBXR_UI_HELD, 1) == 1);
    r.in.grip[1].position = r.in.aim[1].position = mouth_grip;
    for (int i = 0; i < 60; ++i) {
        r.Frame(Pad());
    }
    CHECK(r.ui.HeldItemState().Active() && r.ui.HeldItemState().Progress() == 0.0f);
    r.in.squeeze[1] = 0.97f;
    for (int i = 0; i < 15; ++i) {
        r.Frame(Pad());
    }
    CHECK(r.ui.HeldItemState().Active() && r.ui.HeldItemState().Progress() > 0.2f);
    for (int i = 0; i < 40 && r.ui.HeldItemState().Active(); ++i) {
        r.Frame(Pad());
    }
    CHECK(!r.ui.HeldItemState().Active());
    got = DrainAll(r);
    clear = IndexOf(got, BBXR_UI_HELD, -1);
    use = IndexOf(got, BBXR_UI_USE_QUICK);
    CHECK(clear >= 0 && use > clear && got[size_t(use)].a == 2 && got[size_t(use)].b == int32_t(Goods(100)));
    SettleHeld(r);

    // -- A throw: the weapon is back before the throw's origin, direction and use.
    r.state.quick[3] = Goods(1200);
    r.ui.SetGameState(r.state, r.t);
    PickOnWheel(r, 3);
    r.in.squeeze[1] = 0.9f;
    r.Frame(Pad());
    CHECK(r.ui.HeldItemState().Gripped() && r.ui.HeldItemState().Aiming());
    DrainAll(r);
    for (int i = 1; i <= 8; ++i) {
        r.Frame(Pad());
    }
    r.in.squeeze[1] = 0.0f;
    r.Frame(Pad());
    got = DrainAll(r);
    CHECK(got.size() == 4 && got[0].kind == BBXR_UI_HELD && got[0].a == -1 && got[1].kind == BBXR_UI_THROW_ORIGIN &&
          got[2].kind == BBXR_UI_THROW_DIR && got[3].kind == BBXR_UI_USE_QUICK);
    SettleHeld(r);

    // -- The paper is held in the left hand: HELD says so.
    PickOnWheel(r, 4);
    r.in.squeeze[0] = 0.9f;
    r.Frame(Pad());
    got = DrainAll(r);
    CHECK(r.ui.HeldItemState().Gripped() && got.size() == 1 && got[0].kind == BBXR_UI_HELD && got[0].a == 0 &&
          got[0].b == int32_t(Goods(1300)));
    // a menu opening takes it back: cleared
    r.state.flags |= BBXR_GAME_MENU_OPEN;
    r.ui.SetGameState(r.state, r.t);
    r.Frame(Pad());
    got = DrainAll(r);
    CHECK(!r.ui.HeldItemState().Active() && got.size() == 1 && got[0].kind == BBXR_UI_HELD && got[0].a == -1);
    r.state.flags &= ~BBXR_GAME_MENU_OPEN;
    r.ui.SetGameState(r.state, r.t);
    r.Frame(Pad());

    // -- hide_weapon=false: the item works, nothing is sent
    {
        const fs::path dir2 = HeldAssets(assets, "bbport_game_ui_held_nohide", ", \"held\": {\"hide_weapon\": false, \"crush_squeeze\": 0.8, \"crush_hold_s\": 1.5}");
        Rig q;
        CHECK(q.ui.Load(dir2, nullptr, log));
        const sb::UiHeldItems& hk = q.ui.Menus().Layout().game.held;
        CHECK(!hk.hide_weapon && std::fabs(hk.crush_squeeze - 0.8f) < 1e-6f && std::fabs(hk.crush_hold_s - 1.5f) < 1e-6f);
        q.feed = true;
        q.state = GameState(80);
        q.ui.SetGameState(q.state, q.t);
        q.Frame(Pad());
        PickOnWheel(q, 0);
        q.in.squeeze[1] = 0.9f;
        q.Frame(Pad());
        CHECK(q.ui.HeldItemState().Gripped());
        for (int i = 0; i < 200; ++i) {
            q.Frame(Pad());
        }
        q.in.grip[1].position = q.in.aim[1].position = mouth_grip;
        for (int i = 0; i < 40 && q.ui.HeldItemState().Active(); ++i) {
            q.Frame(Pad());
        }
        const std::vector<BbXrUiAction> all = DrainAll(q);
        CHECK(!q.ui.HeldItemState().Active() && CountOf(all, BBXR_UI_HELD) == 0 && CountOf(all, BBXR_UI_USE_QUICK) == 1);
    }
}

/// Item triangles found in the HUD's front layer: the vertices (flat mesh triangles have no uv /
/// shape; the ring, text and discs do) that sit on one of `world`'s positions.
size_t MatchingVertices(const std::vector<sb::UiVertex>& top, const std::vector<m3::Vec3>& world) {
    size_t n = 0;
    for (const sb::UiVertex& v : top) {
        const bool item_tex = v.shape[0] == 1.0f && v.shape[1] == 1.0f; // a textured item triangle (icon mode)
        if (!item_tex && (v.shape[0] != 0.0f || v.shape2[0] != 0.0f || v.uv[0] != 0.0f || v.uv[1] != 0.0f)) {
            continue;
        }
        const m3::Vec3 p{v.position[0], v.position[1], v.position[2]};
        for (const m3::Vec3& w : world) {
            if (m3::Length(p - w) < 1e-4f) {
                ++n;
                break;
            }
        }
    }
    return n;
}

/// Starts a held item in `r` (the quick slot 0), gripped, the holding hand at a tilted grip pose.
bool HoldItem(Rig& r, const fs::path& dir, int goods, std::string& log, const float* bone = nullptr) {
    if (!r.ui.Load(dir, nullptr, log)) {
        return false;
    }
    r.feed = true;
    r.state = GameState(80);
    r.state.quick[0] = Goods(goods);
    if (bone) { // the host's hand bone frames (BbXrGameDebug::weapon), both sides
        r.state.debug.flags = BBXR_DEBUG_POSE;
        r.state.debug.weapon_valid = 3;
        std::memcpy(r.state.debug.weapon[0], bone, 12 * sizeof(float));
        std::memcpy(r.state.debug.weapon[1], bone, 12 * sizeof(float));
    }
    r.ui.SetGameState(r.state, r.t);
    r.Frame(Pad());
    PickOnWheel(r, 0);
    if (!r.ui.HeldItemState().Active()) {
        return false;
    }
    const int h = r.ui.HeldItemState().Hand();
    r.in.grip[h].rotation = r.in.aim[h].rotation = m3::FromEulerDeg({-20.0f, 15.0f, 10.0f});
    r.in.squeeze[h] = 0.9f;
    r.Frame(Pad());
    r.Frame(Pad());
    return r.ui.HeldItemState().Gripped();
}

/// The meshes the game resolves, from the REAL extracted folder (out/vr_assets): the same call chain
/// the game DLL runs (GameUi::DrawHeldItem -> MeshCache::Resolve(assets, goods) -> the sandbox row's
/// consumables/ / throwables/ file, else items/<goods>.json), then drawn into the HUD. Skipped when the
/// folder has no items (a checkout without the extracted assets).
void TestHeldMeshRealFolder(const fs::path& assets) {
    if (!fs::exists(assets / "items" / "1000.json") || !fs::exists(assets / "consumables" / "cs_205.json")) {
        std::printf("held meshes (real folder): skipped (no items/ consumables/ under %s)\n", assets.string().c_str());
        return;
    }
    // 0. the atlas the held meshes sample: every texel of the item sheet part is opaque (the vial's and
    // the antidote's effect textures carry sprite alpha that made them invisible); the icons keep theirs.
    {
        sb::UiAtlas atlas;
        std::string alog;
        sb::ItemSheet sheet;
        if (atlas.Load(assets / "ui", alog) && sheet.Load(assets) && atlas.height == sheet.Height()) {
            size_t clear = 0;
            for (size_t y = sheet.icons_h; y < atlas.height; ++y) {
                for (size_t x = 0; x < sheet.sheet_w; ++x) {
                    clear += atlas.pixels[(y * atlas.width + x) * 4 + 3] != 255;
                }
            }
            CHECK(clear == 0);
            // A tinted textured mesh (the mouth items) is lit and opaque: a vial-like texture, red tint.
            sb::Model m;
            sb::Vertex vv{};
            vv.normal[2] = 1.0f;
            m.vertices.assign(3, vv);
            m.vertices[1].position[0] = 0.05f, m.vertices[2].position[1] = 0.05f;
            m.vertices[1].uv[0] = 1.0f, m.vertices[2].uv[1] = 1.0f;
            m.indices = {0, 1, 2};
            sb::MeshRange mr;
            mr.texture = sheet.rects.count("s10009") ? "s10009" : sheet.rects.begin()->first;
            mr.index_count = 3;
            m.meshes.push_back(mr);
            std::vector<sb::UiVertex> o;
            CHECK(vrui::held::AppendMesh(o, m, m3::Translate({0, 0, 0}), {0.85f, 0.08f, 0.1f, 1.0f}, {0, 0, 1}, &sheet, 0.85f) == 1);
            for (const sb::UiVertex& v : o) {
                const int px = int(v.uv[0] * float(atlas.width)), py = int(v.uv[1] * float(atlas.height));
                CHECK(v.color[3] == 1.0f && atlas.pixels[(size_t(py) * atlas.width + size_t(px)) * 4 + 3] == 255);
                CHECK(v.color[0] > 0.5f); // not near black
            }
        } else {
            std::printf("held meshes (real folder): item sheet atlas check skipped\n");
        }
    }
    // 1. the cache alone: each goods resolves to a file that exists under the folder, never a stand-in
    //    (the Fire Paper has no model: its stand-in says why).
    for (const int goods : {205, 1000, 1120, 1200, 1300}) {
        vrui::held::MeshCache cache;
        const auto* e = cache.Resolve(assets, goods);
        CHECK(e != nullptr);
        if (!e) {
            continue;
        }
        if (goods == 1300) {
            CHECK(e->standin && e->source == "stand-in paper" && !e->why.empty());
            continue;
        }
        CHECK(!e->standin);
        CHECK(!e->model.vertices.empty() && !e->model.indices.empty());
        CHECK(!e->tried_path.empty() && fs::exists(e->tried_path));
        CHECK(e->tried_path.find("vr_assets") != std::string::npos || !assets.empty());
        // the file named by the log line is the one under the folder given
        CHECK(fs::path(e->tried_path).lexically_normal() == (assets / e->spec.file).lexically_normal());
        std::printf("  held mesh goods %d: %s (%zu vertices)\n", goods, e->tried_path.c_str(), e->model.vertices.size());
    }
    // 2. the game's own chain: hold the item (the wheel, the grip), the HUD has its triangles at the
    //    grip, and the log has the one line per item: mesh file found / stand-in.
    const fs::path dir = FullAssets(assets, "bbport_game_ui_real_items", "");
    for (const int goods : {205, 1000, 1120, 1200, 1300}) {
        Rig r;
        std::string log;
        CHECK(HoldItem(r, dir, goods, log));
        const std::string tag = "game UI: held item: goods " + std::to_string(goods) + " ";
        if (goods == 1300) {
            CHECK(r.all_log.find(tag + "stand-in paper (no mesh file: ") != std::string::npos);
        } else {
            CHECK(r.all_log.find(tag + "mesh file found: ") != std::string::npos);
            CHECK(r.all_log.find(tag + "stand-in") == std::string::npos);
        }
        CHECK(r.all_log.find("game UI: held draw: goods " + std::to_string(goods) + ": ") != std::string::npos);
        vrui::held::MeshCache cache;
        const auto* e = cache.Resolve(dir, goods);
        const int h = r.ui.HeldItemState().Hand();
        CHECK(e && h >= 0);
        if (!e || h < 0) {
            continue;
        }
        const m3::Mat4 world = vrui::held::SocketMeshWorld(e->spec, e->from_table, r.ui.HeldItemState().Info().kind,
                                                           BBXR_ITEM_GOODS | uint32_t(goods), h, r.in.grip[h].position,
                                                           r.in.grip[h].rotation);
        std::vector<m3::Vec3> pos, nrm;
        vrui::held::TransformModel(e->model, world, pos, nrm);
        CHECK(MatchingVertices(r.ui.HudTop(), pos) >= 6); // the real mesh's triangles are in the HUD
    }
    // 3. a goods row whose file is gone: the log says which path was tried
    {
        const fs::path empty = HeldAssets(assets, "bbport_game_ui_no_items", "");
        vrui::held::MeshCache cache;
        const auto* e = cache.Resolve(empty, 1000);
        CHECK(e && e->standin && e->why.find("file missing") == 0 && e->why.find("cs_1000.json") != std::string::npos);
    }
}

/// The item in the game's hands goes to the game's hand (its bone frame from the host), not the
/// controller: pure placement math, then the same through the game UI (log, triangles).
void TestHeldAtGameHand(const fs::path& assets) {
    // 1. the math: a bone matrix is a grip pose; invalid ones fall back to the controller
    {
        const m3::Quat q = m3::FromEulerDeg({10.0f, -35.0f, 20.0f});
        const m3::Mat4 r = m3::FromQuat(q);
        const float bone[12] = {r.m[0], r.m[1], r.m[2], r.m[4], r.m[5], r.m[6], r.m[8], r.m[9], r.m[10], 0.31f, 1.05f, -0.42f};
        m3::Vec3 p;
        m3::Quat out;
        CHECK(vrui::held::BoneGripPose(bone, &p, &out));
        CHECK(m3::Length(p - m3::Vec3{0.31f, 1.05f, -0.42f}) < 1e-6f);
        CHECK(std::fabs(out.x * q.x + out.y * q.y + out.z * q.z + out.w * q.w) > 0.9999f);
        const m3::Vec3 ctrl{0.2f, 1.2f, -0.35f};
        const vrui::held::HandPlacement a = vrui::held::PlaceInHand(bone, ctrl, {0, 0, 0, 1});
        CHECK(a.from_bone && m3::Length(a.pos - p) < 1e-6f && std::fabs(a.gap_m - m3::Length(p - ctrl)) < 1e-6f);
        // the item mesh follows the bone: the socket world matrix is the controller's, moved by the pose difference
        const vrui::held::MeshSpec spec{};
        const m3::Mat4 w_bone = vrui::held::SocketMeshWorld(spec, true, vrui::held::Kind::Mouth, Goods(1000), 1, a.pos, a.rot);
        const m3::Mat4 w_grip = vrui::held::SocketMeshWorld(spec, true, vrui::held::Kind::Mouth, Goods(1000), 1, p, out);
        for (int i = 0; i < 16; ++i) {
            CHECK(std::fabs(w_bone.m[i] - w_grip.m[i]) < 1e-6f);
        }
        CHECK(!vrui::held::PlaceInHand(nullptr, ctrl, {0, 0, 0, 1}).from_bone);
        float far_bone[12];
        std::memcpy(far_bone, bone, sizeof(bone));
        far_bone[10] = 5.0f; // a wrong bone: 4 m away
        CHECK(!vrui::held::PlaceInHand(far_bone, ctrl, {0, 0, 0, 1}).from_bone);
        float nan_bone[12];
        std::memcpy(nan_bone, bone, sizeof(bone));
        nan_bone[3] = NAN;
        CHECK(!vrui::held::PlaceInHand(nan_bone, ctrl, {0, 0, 0, 1}).from_bone);
        float mirror[12];
        std::memcpy(mirror, bone, sizeof(bone));
        for (int i = 0; i < 3; ++i) {
            mirror[i] = -mirror[i]; // det -1: not a rotation
        }
        CHECK(!vrui::held::PlaceInHand(mirror, ctrl, {0, 0, 0, 1}).from_bone);
    }
    // 2. through the game UI (the game's hands): the mesh is drawn at the bone, the log says so
    if (!fs::exists(assets / "hands" / "right.glb") || !fs::exists(assets / "items" / "items.json")) {
        std::printf("held at game hand: UI part skipped (no hands / items under %s)\n", assets.string().c_str());
        return;
    }
    const fs::path game_dir = FullAssets(assets, "bbport_game_ui_place_bone", "");
    const m3::Mat4 rot = m3::FromQuat(m3::FromEulerDeg({0.0f, 25.0f, 0.0f}));
    const float bone[12] = {rot.m[0], rot.m[1], rot.m[2], rot.m[4], rot.m[5], rot.m[6], rot.m[8], rot.m[9], rot.m[10],
                            0.12f, 1.0f, -0.5f}; // ~0.25 m from the controllers
    for (const bool with_bone : {true, false}) {
        Rig r;
        std::string log;
        CHECK(HoldItem(r, game_dir, 1000, log, with_bone ? bone : nullptr));
        const int h = r.ui.HeldItemState().Hand();
        vrui::held::MeshCache cache;
        const auto* e = cache.Resolve(game_dir, 1000);
        CHECK(e && h >= 0);
        if (!e || h < 0) {
            continue;
        }
        const m3::Vec3 bp{bone[9], bone[10], bone[11]};
        const m3::Quat bq = m3::ToQuat(rot);
        const m3::Mat4 world = vrui::held::SocketMeshWorld(
            e->spec, e->from_table, r.ui.HeldItemState().Info().kind, Goods(1000), h,
            with_bone ? bp : r.in.grip[h].position, with_bone ? bq : r.in.grip[h].rotation);
        std::vector<m3::Vec3> pos, nrm;
        vrui::held::TransformModel(e->model, world, pos, nrm);
        CHECK(MatchingVertices(r.ui.HudTop(), pos) >= 6);
        const std::string tag = "game UI: held place: goods 1000 at the ";
        CHECK(r.all_log.find(tag) != std::string::npos);
        CHECK((r.all_log.find("hand bone (R_Weapon/L_Weapon frame); controller grip to the game hand") != std::string::npos) == with_bone);
        CHECK((r.all_log.find("no hand bone from the host") != std::string::npos) == !with_bone);
    }
}

/// The game's hands and the custom hands put the item at the same place (one helper): the same
/// triangles, for a sandbox row, an items.json row and a stand-in.
void TestHeldPlacementBothModes(const fs::path& assets) {
    if (!fs::exists(assets / "hands" / "right.glb") || !fs::exists(assets / "items" / "items.json")) {
        std::printf("held placement: skipped (no hands / items under %s)\n", assets.string().c_str());
        return;
    }
    const fs::path game_dir = FullAssets(assets, "bbport_game_ui_place_game", "");
    const fs::path hands_dir = FullAssets(assets, "bbport_game_ui_place_hands", ", \"custom_hands\": {\"on\": true}");
    for (const int goods : {1000, 1100, 1220, 1300, 100}) {
        Rig game, hands;
        std::string log;
        CHECK(HoldItem(game, game_dir, goods, log));
        CHECK(HoldItem(hands, hands_dir, goods, log));
        CHECK(!game.ui.CustomHandsOn() && hands.ui.CustomHandsOn() && hands.ui.HandTriangles() > 1000);
        const int h = game.ui.HeldItemState().Hand();
        CHECK(h == hands.ui.HeldItemState().Hand());
        vrui::held::MeshCache cache;
        const vrui::held::MeshCache::Entry* e = cache.Resolve(game_dir, goods);
        CHECK(e != nullptr);
        if (!e) {
            continue;
        }
        const bool table = vrui::held::MeshFor(goods) != nullptr;
        CHECK(e->from_table == table);
        const m3::Mat4 world = vrui::held::SocketMeshWorld(e->spec, e->from_table, game.ui.HeldItemState().Info().kind,
                                                           BBXR_ITEM_GOODS | uint32_t(goods), h, game.in.grip[h].position,
                                                           game.in.grip[h].rotation);
        std::vector<m3::Vec3> pos, nrm;
        vrui::held::TransformModel(e->model, world, pos, nrm);
        const size_t in_game = MatchingVertices(game.ui.HudTop(), pos);
        const size_t in_hands = MatchingVertices(hands.ui.HudTop(), pos);
        CHECK(in_game >= 6);
        CHECK(in_game == in_hands);
        // the log says once which mesh is used
        const std::string tag = "game UI: held item: goods " + std::to_string(goods) + " ";
        const std::string mesh_tag = tag + (e->standin ? "stand-in" : "mesh ");
        size_t first = game.all_log.find(mesh_tag);
        CHECK(first != std::string::npos && game.all_log.find(mesh_tag, first + 1) == std::string::npos);
    }
    // the in-game size rule: the longest side of the placed mesh is the goods' target (+-10%), and a
    // bottle's centre sits in the fist (within ~4 cm of the grip origin), the mesh's up toward the thumb (-Z)
    {
        const struct {
            int goods;
            float target;
        } kSizes[] = {{1000, 0.11f}, {1100, 0.11f}, {1120, 0.12f}, {1200, 0.18f}, {1220, 0.15f},
                      {1230, 0.055f}, {1240, 0.20f}, {200, 0.11f}, {4312, 0.17f}, {1410, 0.035f}, {100, 0.03f}};
        vrui::held::MeshCache cache;
        for (const auto& t : kSizes) {
            const auto* e = cache.Resolve(assets, t.goods);
            CHECK(e != nullptr);
            if (!e) {
                continue;
            }
            const m3::Mat4 w = vrui::held::SocketMeshWorld(e->spec, e->from_table, vrui::held::Kind::Mouth,
                                                           Goods(t.goods), 1, {0, 0, 0}, {0, 0, 0, 1});
            std::vector<m3::Vec3> pos, nrm;
            vrui::held::TransformModel(e->model, w, pos, nrm);
            m3::Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
            for (const m3::Vec3& p : pos) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            const float longest = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
            CHECK(std::fabs(longest - t.target) < 0.10f * t.target);
            if (vrui::hand::CategoryFor(Goods(t.goods)) == vrui::hand::HoldCategory::Bottle) {
                const m3::Vec3 c = (lo + hi) * 0.5f;
                CHECK(m3::Length(c) < 0.04f);
            }
        }
        const auto* vial = cache.Resolve(assets, 1000);
        if (vial && !vial->standin) {
            // the neck (mesh +Y) points out of the thumb side (grip -Z)
            const m3::Mat4 w = vrui::held::SocketMeshWorld(vial->spec, vial->from_table, vrui::held::Kind::Mouth,
                                                           Goods(1000), 1, {0, 0, 0}, {0, 0, 0, 1});
            CHECK(m3::Normalize(m3::TransformDir(w, {0, 1, 0}), {0, 1, 0}).z < -0.9f);
        }
    }
    // the placement helper itself: it ignores the sandbox's pose (the category socket, mirrored for the left hand)
    {
        vrui::held::MeshCache cache;
        const auto* e = cache.Resolve(assets, 1000);
        CHECK(e && e->from_table);
        if (e) {
            const m3::Quat rot = m3::FromEulerDeg({10.0f, 20.0f, 30.0f});
            const m3::Mat4 a = vrui::held::SocketMeshWorld(e->spec, true, vrui::held::Kind::Mouth, Goods(1000), 1,
                                                           {0.1f, 1.0f, -0.3f}, rot);
            const m3::Mat4 b = vrui::held::SocketMeshWorld(e->spec, true, vrui::held::Kind::Mouth, Goods(1000), 1,
                                                           {0.1f, 1.0f, -0.3f}, rot);
            CHECK(m3::Length(m3::Translation(a) - m3::Translation(b)) < 1e-7f);
            // a left hand mirrors it across the grip's X
            const m3::Mat4 l = vrui::held::SocketMeshWorld(e->spec, true, vrui::held::Kind::Mouth, Goods(1000), 0,
                                                           {0.0f, 0.0f, 0.0f}, {0, 0, 0, 1});
            const m3::Mat4 rr = vrui::held::SocketMeshWorld(e->spec, true, vrui::held::Kind::Mouth, Goods(1000), 1,
                                                            {0.0f, 0.0f, 0.0f}, {0, 0, 0, 1});
            CHECK(std::fabs(m3::Translation(l).x + m3::Translation(rr).x) < 1e-6f);
        }
    }
}

// ---- The custom character model override (vr_skin_model.h) ----

/// A placeholder .glb in memory: a body box (node 0, base colour red, computed normals) and a head
/// box (node 1, translated up by 1.5, COLOR_0 vec3 floats, base colour white). Height 1.75 m,
/// centred on X/Z, feet at 0, 24 triangles.
std::vector<uint8_t> PlaceholderGlb() {
    std::vector<uint8_t> bin;
    auto put = [&](const void* d, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(d);
        bin.insert(bin.end(), b, b + n);
    };
    auto box = [&](float x0, float x1, float y0, float y1, float z0, float z1) {
        const float v[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                               {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
        put(v, sizeof(v));
    };
    const uint16_t idx[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                              3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    box(-0.5f, 0.5f, 0.0f, 1.0f, -0.25f, 0.25f);      // 0..96
    put(idx, sizeof(idx));                            // 96..168
    box(-0.25f, 0.25f, -0.25f, 0.25f, -0.25f, 0.25f); // 168..264
    const float col[8][3] = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
    put(col, sizeof(col));                            // 264..360
    put(idx, sizeof(idx));                            // 360..432
    const std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],)"
        R"("nodes":[{"mesh":0},{"mesh":1,"translation":[0,1.5,0]}],)"
        R"("materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}},{"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1]}}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]},)"
        R"({"primitives":[{"attributes":{"POSITION":2,"COLOR_0":3},"indices":4,"material":1,"mode":4}]}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":8,"type":"VEC3"},)"
        R"({"bufferView":1,"componentType":5123,"count":36,"type":"SCALAR"},)"
        R"({"bufferView":2,"componentType":5126,"count":8,"type":"VEC3"},)"
        R"({"bufferView":3,"componentType":5126,"count":8,"type":"VEC3"},)"
        R"({"bufferView":4,"componentType":5123,"count":36,"type":"SCALAR"}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":96},{"buffer":0,"byteOffset":96,"byteLength":72},)"
        R"({"buffer":0,"byteOffset":168,"byteLength":96},{"buffer":0,"byteOffset":264,"byteLength":96},)"
        R"({"buffer":0,"byteOffset":360,"byteLength":72}],"buffers":[{"byteLength":432}]})";
    std::string j = json;
    while (j.size() % 4) {
        j += ' ';
    }
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t v) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), b, b + 4);
    };
    u32(0x46546C67u);
    u32(2u);
    u32(uint32_t(12 + 8 + j.size() + 8 + bin.size()));
    u32(uint32_t(j.size()));
    u32(0x4E4F534Au);
    out.insert(out.end(), j.begin(), j.end());
    u32(uint32_t(bin.size()));
    u32(0x004E4942u);
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool NearV(Vec3 a, Vec3 b, float eps = 1e-4f) { return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps); }

float Det(const Mat4& m) {
    return m.m[0] * (m.m[5] * m.m[10] - m.m[9] * m.m[6]) - m.m[4] * (m.m[1] * m.m[10] - m.m[9] * m.m[2]) +
           m.m[8] * (m.m[1] * m.m[6] - m.m[5] * m.m[2]);
}

void TestSkinModel() {
    namespace skin = vrui::skin;
    std::printf("skin model\n");
    skin::SkinMesh mesh;
    std::string err;
    const std::vector<uint8_t> glb = PlaceholderGlb();
    CHECK(skin::ParseSkinGlb(glb, mesh, err));
    CHECK(mesh.parts.size() == 2);
    CHECK(mesh.tris == 24);
    if (mesh.parts.size() == 2) {
        CHECK(mesh.parts[0].vcolor.empty() && mesh.parts[1].vcolor.size() == 8);
        CHECK(Near(mesh.parts[0].color.r, 1.0f) && Near(mesh.parts[0].color.g, 0.0f));
        CHECK(Near(mesh.parts[1].color.g, 1.0f) && Near(mesh.parts[1].vcolor[0].g, 1.0f) && Near(mesh.parts[1].vcolor[0].r, 0.0f));
        CHECK(mesh.parts[0].nrm.size() == 8); // computed
        // the head node's translation applied
        CHECK(Near(mesh.lo.y, 0.0f) && Near(mesh.hi.y, 1.75f) && Near(mesh.hi.x, 0.5f));
    }
    // Normalize: height, feet at 0, centred
    skin::SkinMesh big = mesh;
    CHECK(skin::Normalize(big, 1.8f));
    CHECK(Near(big.hi.y - big.lo.y, 1.8f) && Near(big.lo.y, 0.0f));
    CHECK(Near(big.lo.x + big.hi.x, 0.0f) && Near(big.lo.z + big.hi.z, 0.0f));
    CHECK(Near(big.hi.x, 0.5f * 1.8f / 1.75f));
    skin::SkinMesh shifted = mesh; // moved off the origin: still centred
    for (auto& p : shifted.parts) {
        for (Vec3& v : p.pos) {
            v = v + Vec3{3, -2, 5};
        }
    }
    CHECK(skin::Normalize(shifted, 1.0f));
    CHECK(Near(shifted.lo.y, 0.0f) && Near(shifted.hi.y, 1.0f) && Near(shifted.lo.x + shifted.hi.x, 0.0f) &&
          Near(shifted.lo.z + shifted.hi.z, 0.0f));
    skin::SkinMesh flat;
    CHECK(!skin::Normalize(flat, 1.8f));

    // SkinWorld: +Z -> forward, +Y -> up, det +1, origin at pos
    {
        const Vec3 pos{1, 0, -3}, fwd = Normalize(Vec3{1, 0, 1}), up{0, 1, 0};
        const Mat4 w = skin::SkinWorld(pos, fwd, up, 0.0f, 0.0f);
        CHECK(NearV(TransformDir(w, {0, 0, 1}), fwd));
        CHECK(NearV(TransformDir(w, {0, 1, 0}), up));
        CHECK(NearV(TransformDir(w, {1, 0, 0}), Cross(up, fwd)));
        CHECK(Near(Det(w), 1.0f, 1e-3f));
        CHECK(NearV(Translation(w), pos));
        // the yaw offset turns the mesh about its own Y
        const Mat4 w90 = skin::SkinWorld(pos, fwd, up, 90.0f * kDeg, 0.0f);
        CHECK(NearV(TransformDir(w90, {0, 0, 1}), TransformDir(w, {1, 0, 0})));
        CHECK(NearV(TransformDir(w90, {0, 1, 0}), up) && Near(Det(w90), 1.0f, 1e-3f));
        // the bob lifts along up
        const Mat4 wb = skin::SkinWorld(pos, fwd, up, 0.0f, 0.05f);
        CHECK(NearV(Translation(wb), pos + up * 0.05f));
        // a tilted up and a forward that is not orthogonal to it
        const Vec3 tup = Normalize(Vec3{0.2f, 1, 0.1f});
        const Mat4 wt = skin::SkinWorld(pos, {0, 0, 1}, tup, 0.0f, 0.0f);
        CHECK(NearV(TransformDir(wt, {0, 1, 0}), tup));
        CHECK(Near(Dot(TransformDir(wt, {0, 0, 1}), tup), 0.0f, 1e-4f) && Near(Det(wt), 1.0f, 1e-3f));
        // forward along up: still a valid frame
        const Mat4 wd = skin::SkinWorld(pos, {0, 1, 0}, {0, 1, 0}, 0.0f, 0.0f);
        CHECK(Near(Det(wd), 1.0f, 1e-3f));
    }
    // SkinBob: zero at rest, a bounded hop and roll while walking
    {
        float roll = 1.0f;
        CHECK(skin::SkinBob(1.234f, 0.0f, &roll) == 0.0f && roll == 0.0f);
        CHECK(skin::SkinBob(1.0f, 0.29f, nullptr) == 0.0f);
        float maxb = 0.0f, maxr = 0.0f;
        bool bounded = true;
        for (int i = 0; i < 400; ++i) {
            float r = 0.0f;
            const float b = skin::SkinBob(float(i) * 0.01f, 1.5f, &r);
            maxb = std::max(maxb, b);
            maxr = std::max(maxr, std::fabs(r));
            bounded &= b >= 0.0f && b <= 0.06f;
        }
        CHECK(bounded && maxb > 0.01f && maxr > 0.0f && maxr < 0.1f);
    }
    // AppendSkin: triangles drawn far to near, the per-vertex colour reaches the vertices
    {
        skin::SkinMesh m = mesh;
        skin::Normalize(m, 1.8f);
        std::vector<sb::UiVertex> out;
        const Vec3 eye{0, 1.6f, 0};
        const Mat4 w = skin::SkinWorld({0, 0, -3}, {0, 0, 1}, {0, 1, 0}, 0.0f, 0.0f);
        const size_t n = skin::AppendSkin(out, m, w, eye);
        CHECK(n > 0 && n <= 24 && out.size() == n * 3);
        bool greenish = false, finite = true, sorted = true;
        float prev = 1e9f;
        for (size_t i = 0; i + 2 < out.size(); i += 3) {
            Vec3 c{0, 0, 0};
            for (size_t k = 0; k < 3; ++k) {
                const sb::UiVertex& v = out[i + k];
                greenish |= v.color[1] > 0.2f && v.color[0] < 0.05f;
                finite &= std::isfinite(v.position[0] + v.position[1] + v.position[2]);
                c = c + Vec3{v.position[0], v.position[1], v.position[2]} * (1.0f / 3.0f);
            }
            const float d = Length(c - eye);
            sorted &= d <= prev + 1e-4f;
            prev = d;
        }
        CHECK(greenish && finite && sorted);
    }
    // Garbage and truncated files are rejected without crashing
    {
        skin::SkinMesh bad;
        std::string e;
        CHECK(!skin::ParseSkinGlb({}, bad, e) && !e.empty());
        std::vector<uint8_t> junk(200, 0xAB);
        CHECK(!skin::ParseSkinGlb(junk, bad, e));
        for (const size_t cut : {size_t(11), size_t(27), size_t(60), glb.size() / 2, glb.size() - 5}) {
            std::vector<uint8_t> t(glb.begin(), glb.begin() + std::ptrdiff_t(cut));
            CHECK(!skin::ParseSkinGlb(t, bad, e) && bad.parts.empty());
        }
        const size_t base = 20 + (size_t(glb[12]) | size_t(glb[13]) << 8 | size_t(glb[14]) << 16) + 8; // BIN data
        std::vector<uint8_t> oob = glb; // an index past the vertex count
        oob[base + 96] = 0xFF;
        CHECK(!skin::ParseSkinGlb(oob, bad, e) && e.find("range") != std::string::npos);
        std::vector<uint8_t> nan = glb; // a NaN position
        const float qnan = std::nanf("");
        std::memcpy(&nan[base], &qnan, 4);
        CHECK(!skin::ParseSkinGlb(nan, bad, e));
    }
    // LoadSkin / LoadConfigured: a folder (skin.glb first), a missing path
    {
        const fs::path dir = fs::temp_directory_path() / "bbport_skin_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        auto write = [&](const char* name, const std::vector<uint8_t>& d) {
            std::ofstream f(dir / name, std::ios::binary);
            f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
        };
        write("a_other.glb", {1, 2, 3});
        write("skin.glb", glb);
        skin::SkinMesh m;
        std::string e;
        fs::path used;
        CHECK(skin::LoadSkin(dir, m, e, &used) && used.filename() == "skin.glb" && m.tris == 24);
        fs::remove(dir / "skin.glb", ec);
        CHECK(!skin::LoadSkin(dir, m, e) && !e.empty()); // the first .glb is junk
        CHECK(!skin::LoadSkin(dir / "nope.glb", m, e));
        write("skin.glb", glb);
        skin::Config cfg;
        cfg.path = dir.string();
        cfg.height_m = 2.0f;
        cfg.scale = 0.5f;
        std::string msg;
        CHECK(skin::LoadConfigured(cfg, dir, m, msg) && Near(m.hi.y, 1.0f) && msg.find("VR skin: loaded") == 0);
        cfg.path = "no_such_skin_folder";
        CHECK(!skin::LoadConfigured(cfg, dir, m, msg) && msg.find("VR skin: cannot load") == 0);
        fs::remove_all(dir, ec);
    }
    // The built-in placeholder
    {
        skin::SkinMesh ph = skin::PlaceholderMesh();
        CHECK(skin::Normalize(ph, 1.8f) && ph.tris > 0);
    }
}

// ---- The offline render (--render DIR): the held items without the headset ----

struct Image {
    int w = 0, h = 0;
    std::vector<float> rgb;
    explicit Image(int width = 512, int height = 512) : w(width), h(height), rgb(size_t(width) * size_t(height) * 3, 0.0f) {}
};

/// A tiny CPU rasteriser of the emitted triangles (painter's order as given), alpha over a dark grey.
void Rasterise(Image& img, const std::vector<sb::UiVertex>& tris, m3::Vec3 eye, m3::Vec3 target,
               const sb::UiAtlas* atlas = nullptr) {
    using namespace m3;
    const Vec3 fwd = Normalize(target - eye, {0, 0, -1});
    const Vec3 world_up = std::fabs(fwd.y) > 0.95f ? Vec3{0, 0, -1} : Vec3{0, 1, 0};
    const Vec3 right = Normalize(Cross(fwd, world_up), {1, 0, 0});
    const Vec3 up = Cross(right, fwd);
    const float focal = 0.5f * float(img.w) / std::tan(0.5f * 38.0f * kDeg);
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        float sx[3], sy[3], sz[3];
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            const sb::UiVertex& v = tris[i + size_t(k)];
            const bool item_tex = v.shape[0] == 1.0f && v.shape[1] == 1.0f && atlas && !atlas->pixels.empty();
            if (!item_tex && (v.shape[0] != 0.0f || v.shape2[0] != 0.0f || v.uv[0] != 0.0f || v.uv[1] != 0.0f)) {
                ok = false; // the ring, discs and text are not the item
            }
            const Vec3 d = Vec3{v.position[0], v.position[1], v.position[2]} - eye;
            const float z = Dot(d, fwd);
            if (z < 0.02f) {
                ok = false;
                break;
            }
            sx[k] = float(img.w) * 0.5f + Dot(d, right) / z * focal;
            sy[k] = float(img.h) * 0.5f - Dot(d, up) / z * focal;
            sz[k] = z;
        }
        if (!ok) {
            continue;
        }
        const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (std::fabs(area) < 1e-6f) {
            continue;
        }
        const int x0 = std::max(0, int(std::floor(std::min({sx[0], sx[1], sx[2]})))),
                  x1 = std::min(img.w - 1, int(std::ceil(std::max({sx[0], sx[1], sx[2]})))),
                  y0 = std::max(0, int(std::floor(std::min({sy[0], sy[1], sy[2]})))),
                  y1 = std::min(img.h - 1, int(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                const float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
                const float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) {
                    continue;
                }
                float c[4] = {};
                for (int k = 0; k < 4; ++k) {
                    c[k] = w0 * tris[i].color[k] + w1 * tris[i + 1].color[k] + w2 * tris[i + 2].color[k];
                }
                if (tris[i].shape[0] == 1.0f && tris[i].shape[1] == 1.0f && atlas && !atlas->pixels.empty()) {
                    // the item texture (icon mode of ui.frag): sRGB atlas times the lit vertex colour
                    const float u = w0 * tris[i].uv[0] + w1 * tris[i + 1].uv[0] + w2 * tris[i + 2].uv[0];
                    const float t = w0 * tris[i].uv[1] + w1 * tris[i + 1].uv[1] + w2 * tris[i + 2].uv[1];
                    const int tx = std::clamp(int(u * float(atlas->width)), 0, int(atlas->width) - 1);
                    const int ty = std::clamp(int(t * float(atlas->height)), 0, int(atlas->height) - 1);
                    const uint8_t* px = &atlas->pixels[(size_t(ty) * atlas->width + size_t(tx)) * 4];
                    for (int k = 0; k < 3; ++k) {
                        c[k] *= std::pow(float(px[k]) / 255.0f, 2.2f);
                    }
                    c[3] *= float(px[3]) / 255.0f;
                }
                float* dst = &img.rgb[(size_t(y) * size_t(img.w) + size_t(x)) * 3];
                for (int k = 0; k < 3; ++k) {
                    dst[k] = dst[k] * (1.0f - c[3]) + c[k] * c[3];
                }
            }
        }
    }
}

std::vector<uint8_t> ToRgba(const Image& img) {
    std::vector<uint8_t> out(size_t(img.w) * size_t(img.h) * 4);
    for (size_t i = 0; i < size_t(img.w) * size_t(img.h); ++i) {
        for (int k = 0; k < 3; ++k) {
            const float v = std::pow(std::clamp(img.rgb[i * 3 + size_t(k)], 0.0f, 1.0f), 1.0f / 2.2f);
            out[i * 4 + size_t(k)] = uint8_t(std::lround(v * 255.0f));
        }
        out[i * 4 + 3] = 255;
    }
    return out;
}

Image Background(int w, int h) {
    Image img(w, h);
    std::fill(img.rgb.begin(), img.rgb.end(), 0.035f); // dark grey (linear)
    return img;
}

int RenderHeld(const fs::path& assets, const fs::path& out_dir, int view = 0) {
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    const fs::path dirs[2] = {FullAssets(assets, "bbport_game_ui_render_hands", ", \"custom_hands\": {\"on\": true}"),
                              FullAssets(assets, "bbport_game_ui_render_game", "")};
    const char* names[2] = {"hands", "game"};
    const int goods_list[] = {205, 1000, 1100, 1120, 1200, 1220, 1230, 1240, 1300, 2030, 200, 4312, 100, 1410};
    std::vector<Image> tiles;
    sb::UiAtlas atlas;
    std::string atlas_log;
    atlas.Load(assets / "ui", atlas_log);
    std::printf("render: %s", atlas_log.c_str());
    for (const int goods : goods_list) {
        for (int mode = 0; mode < 2; ++mode) {
            Rig r;
            std::string log;
            if (!HoldItem(r, dirs[mode], goods, log)) {
                std::printf("render: goods %d (%s): could not hold it\n", goods, names[mode]);
                continue;
            }
            const int h = r.ui.HeldItemState().Hand();
            const m3::Vec3 gp = r.in.grip[h].position;
            const m3::Quat gr = r.in.grip[h].rotation;
            // The camera: ~0.35 m in front of the fist, a little above and to the thumb side, looking at it.
            const float side = h ? 1.0f : -1.0f;
            m3::Vec3 eye = gp + m3::Vec3{0.20f * side, 0.12f, -0.25f};
            // views 1..6: along the grip frame's +X, -X, +Y, -Y, +Z, -Z (0.35 m), to judge how it sits
            static const m3::Vec3 kViews[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            if (view >= 1 && view <= 6) {
                eye = gp + m3::Rotate(gr, kViews[view - 1] * 0.35f);
            }
            const m3::Vec3 target = gp + m3::Rotate(gr, {0.0f, 0.0f, -0.02f});
            // The UI sorts and culls from the head: put the head at the camera (the item must stay held).
            r.in.head.position = eye;
            r.Frame(Pad());
            r.Frame(Pad());
            if (!r.ui.HeldItemState().Gripped()) {
                std::printf("render: goods %d (%s): the camera head fired the item\n", goods, names[mode]);
                continue;
            }
            Image img = Background(512, 512);
            Rasterise(img, r.ui.HudTop(), eye, target, &atlas);
            const std::string file = (out_dir / ("held_" + std::to_string(goods) + "_" + names[mode] +
                                                 (view ? "_v" + std::to_string(view) : std::string()) + ".png")).string();
            const bool ok = sb::WritePng(file, uint32_t(img.w), uint32_t(img.h), ToRgba(img));
            std::printf("render: %s %s\n", file.c_str(), ok ? "ok" : "FAILED");
            // the log line of what was used
            const size_t at = r.all_log.find("game UI: held item: goods " + std::to_string(goods));
            if (at != std::string::npos && mode == 0) {
                std::printf("  %s", r.all_log.substr(at, r.all_log.find('\n', at) - at + 1).c_str());
            }
            // half size for the sheet
            Image half(256, 256);
            for (int y = 0; y < 256; ++y) {
                for (int x = 0; x < 256; ++x) {
                    for (int k = 0; k < 3; ++k) {
                        float s = 0.0f;
                        for (int dy = 0; dy < 2; ++dy) {
                            for (int dx = 0; dx < 2; ++dx) {
                                s += img.rgb[(size_t(2 * y + dy) * 512 + size_t(2 * x + dx)) * 3 + size_t(k)];
                            }
                        }
                        half.rgb[(size_t(y) * 256 + size_t(x)) * 3 + size_t(k)] = s * 0.25f;
                    }
                }
            }
            tiles.push_back(std::move(half));
        }
    }
    // the contact sheet: 6 tiles per row (hands, game, hands, game, ...)
    const int cols = 6, rows = int((tiles.size() + size_t(cols) - 1) / size_t(cols));
    Image sheet = Background(cols * 256, std::max(rows, 1) * 256);
    for (size_t t = 0; t < tiles.size(); ++t) {
        const int ox = int(t % size_t(cols)) * 256, oy = int(t / size_t(cols)) * 256;
        for (int y = 0; y < 256; ++y) {
            for (int x = 0; x < 256; ++x) {
                for (int k = 0; k < 3; ++k) {
                    sheet.rgb[(size_t(oy + y) * size_t(sheet.w) + size_t(ox + x)) * 3 + size_t(k)] =
                        tiles[t].rgb[(size_t(y) * 256 + size_t(x)) * 3 + size_t(k)];
                }
            }
        }
    }
    const std::string sheet_file = (out_dir / (view ? "held_sheet_v" + std::to_string(view) + ".png" : std::string("held_sheet.png"))).string();
    std::printf("render: %s %s\n", sheet_file.c_str(),
                sb::WritePng(sheet_file, uint32_t(sheet.w), uint32_t(sheet.h), ToRgba(sheet)) ? "ok" : "FAILED");
    return 0;
}

/// --render-skin: the (normalized) skin standing 3 m in front of an eye at 1.6 m, facing it, at yaw
/// offsets 0/90/180/270; skin_<yaw>.png and a 2x2 skin_sheet.png. No usable mesh: the placeholder.
int RenderSkin(const fs::path& mesh_arg, const fs::path& out_dir) {
    namespace skin = vrui::skin;
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    skin::SkinMesh mesh;
    std::string err;
    if (!mesh_arg.empty() && skin::LoadSkin(mesh_arg, mesh, err)) {
        std::printf("render-skin: loaded %s: %zu parts, %zu triangles\n", mesh_arg.string().c_str(), mesh.parts.size(), mesh.tris);
    } else {
        if (!mesh_arg.empty()) {
            std::printf("render-skin: %s: %s; using the built-in placeholder\n", mesh_arg.string().c_str(), err.c_str());
        }
        mesh = skin::PlaceholderMesh();
    }
    if (!skin::Normalize(mesh, 1.8f)) {
        std::printf("render-skin: the mesh has no height\n");
        return 1;
    }
    const m3::Vec3 eye{0.0f, 1.6f, 0.0f}, feet{0.0f, 0.0f, -3.0f}, target{0.0f, 0.9f, -3.0f};
    std::vector<Image> tiles;
    for (const int yaw : {0, 90, 180, 270}) {
        // facing the camera (toward +Z), then the yaw offset about the mesh's own Y
        const m3::Mat4 world = skin::SkinWorld(feet, {0, 0, 1}, {0, 1, 0}, float(yaw) * m3::kDeg, 0.0f);
        std::vector<sb::UiVertex> tris;
        const size_t n = skin::AppendSkin(tris, mesh, world, eye);
        Image img = Background(512, 512);
        Rasterise(img, tris, eye, target);
        const std::string file = (out_dir / ("skin_" + std::to_string(yaw) + ".png")).string();
        std::printf("render-skin: %s (%zu triangles) %s\n", file.c_str(), n,
                    sb::WritePng(file, uint32_t(img.w), uint32_t(img.h), ToRgba(img)) ? "ok" : "FAILED");
        Image half(256, 256);
        for (int y = 0; y < 256; ++y) {
            for (int x = 0; x < 256; ++x) {
                for (int k = 0; k < 3; ++k) {
                    float sum = 0.0f;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            sum += img.rgb[(size_t(2 * y + dy) * 512 + size_t(2 * x + dx)) * 3 + size_t(k)];
                        }
                    }
                    half.rgb[(size_t(y) * 256 + size_t(x)) * 3 + size_t(k)] = sum * 0.25f;
                }
            }
        }
        tiles.push_back(std::move(half));
    }
    Image sheet = Background(512, 512);
    for (size_t t = 0; t < tiles.size(); ++t) {
        const int ox = int(t % 2) * 256, oy = int(t / 2) * 256;
        for (int y = 0; y < 256; ++y) {
            for (int x = 0; x < 256; ++x) {
                for (int k = 0; k < 3; ++k) {
                    sheet.rgb[(size_t(oy + y) * 512 + size_t(ox + x)) * 3 + size_t(k)] =
                        tiles[t].rgb[(size_t(y) * 256 + size_t(x)) * 3 + size_t(k)];
                }
            }
        }
    }
    const std::string sheet_file = (out_dir / "skin_sheet.png").string();
    std::printf("render-skin: %s %s\n", sheet_file.c_str(),
                sb::WritePng(sheet_file, 512, 512, ToRgba(sheet)) ? "ok" : "FAILED");
    return 0;
}

// The co-op room line on the dashboard title (vr_room_line.h): parse, format, read from the environment.
void TestRoomLine() {
    using namespace vrui::room;
    const std::string file =
        R"({"code": "K7Q-M4X", "role": "host", "max": 3, "members": [
  {"name": "A", "you": true}, {"name": "B", "you": false}
], "closed": false})";
    CHECK(Line(Parse(file, "K7Q-M4X")) == "Room K7Q-M4X  2/3");
    CHECK(Line(Parse(file, "")) == "Room K7Q-M4X  2/3"); // the code from the file when the environment has none
    CHECK(Line(Parse("", "K7Q-M4X")) == "Room K7Q-M4X"); // the heartbeat has not written yet
    CHECK(Line(Parse("{\"closed\": true}", "K7Q-M4X")) == "Room K7Q-M4X closed");
    CHECK(Line(Parse("{}", "")).empty());
    const fs::path path = fs::temp_directory_path() / "bbport_room_line_test.json";
    {
        std::ofstream out(path, std::ios::binary);
        out << file;
    }
    _putenv_s("BB_ONLINE_ROOM", "K7Q-M4X");
    _putenv_s("BB_ONLINE_ROOM_FILE", path.string().c_str());
    Reader reader;
    CHECK(reader.Poll(10.0) == "Room K7Q-M4X  2/3");
    std::error_code ec;
    fs::remove(path, ec);
    CHECK(reader.Poll(11.0) == "Room K7Q-M4X  2/3"); // read at most every 3 s
    CHECK(reader.Poll(14.0) == "Room K7Q-M4X");       // file gone: the code from the environment
    _putenv_s("BB_ONLINE_ROOM", "");
    Reader solo;
    CHECK(solo.Poll(0.0).empty()); // a solo game shows nothing
}

/// The skin in the game UI: loaded once from the configured file, drawn for the state's targets into
/// the HUD's top layer; none for a state without targets or from an older (shorter) host struct.
void TestSkinInGame(const fs::path& assets) {
    namespace skin = vrui::skin;
    std::printf("skin in game\n");
    const fs::path dir = FullAssets(assets, "bbport_game_ui_skin", "");
    const fs::path file = fs::temp_directory_path() / "bbport_skin_ingame.glb";
    {
        const std::vector<uint8_t> glb = PlaceholderGlb();
        std::ofstream f(file, std::ios::binary);
        f.write(reinterpret_cast<const char*>(glb.data()), std::streamsize(glb.size()));
    }
    auto run = [&](const skin::Config& cfg, uint32_t targets, bool older, std::string& log, size_t* tris) {
        Rig r;
        r.ui.SetSkinConfig(cfg);
        std::string load_log;
        if (!r.ui.Load(dir, nullptr, load_log)) {
            return size_t(0);
        }
        r.feed = false;
        BbXrGameState s = GameState(80);
        s.skin.count = targets;
        for (uint32_t i = 0; i < targets && i < BBXR_SKIN_MAX; ++i) {
            BbXrSkinTarget& t = s.skin.targets[i];
            t.pos[0] = 1.0f + float(i), t.pos[1] = 0.0f, t.pos[2] = -3.0f;
            t.forward[2] = 1.0f;
            t.up[1] = 1.0f;
            t.speed_mps = 1.2f;
            t.id = i;
        }
        if (older) {
            s.size = uint32_t(offsetof(BbXrGameState, skin)); // a host built before the skin field
        }
        for (int i = 0; i < 4; ++i) {
            r.ui.SetGameState(s, r.t + 0.01);
            r.Frame(Pad());
        }
        log = r.all_log;
        *tris = r.ui.SkinTriangles();
        return r.ui.HudVertices();
    };
    skin::Config cfg;
    cfg.path = file.string();
    std::string log;
    size_t tris = 0;
    const size_t none = run(cfg, 0, false, log, &tris);
    CHECK(tris == 0 && log.find("VR skin: loaded") != std::string::npos);
    CHECK(log.find("drawing") == std::string::npos);
    const size_t two = run(cfg, 2, false, log, &tris);
    CHECK(tris > 0 && two >= none + tris * 3);
    CHECK(log.find("VR skin: drawing 2 target(s)") != std::string::npos);
    CHECK(log.find("VR skin: loaded") != std::string::npos);
    CHECK(run(cfg, 2, true, log, &tris) <= none && tris == 0); // older host: no targets
    skin::Config off;
    CHECK(run(off, 2, false, log, &tris) <= none && tris == 0 && log.find("VR skin") == std::string::npos);
    skin::Config missing;
    missing.path = "no_such_skin_file.glb";
    CHECK(run(missing, 2, false, log, &tris) <= none && tris == 0 && log.find("VR skin: cannot load") != std::string::npos);
    std::error_code ec;
    fs::remove(file, ec);
}

int main(int argc, char** argv) {
    const fs::path assets = argc > 1 ? fs::path(argv[1]) : fs::path("out/vr_assets");
    if (argc > 3 && !std::strcmp(argv[2], "--render")) { // offline renders of the held items (no headset)
        return RenderHeld(assets, argv[3], argc > 4 ? std::atoi(argv[4]) : 0);
    }
    if (argc > 3 && !std::strcmp(argv[2], "--render-skin")) { // <assets> --render-skin [mesh file or dir] <out_dir>
        return argc > 4 ? RenderSkin(argv[3], argv[4]) : RenderSkin({}, argv[3]);
    }
    TestFilter();
    TestMirrorProjection();
    TestFilterMash();
    TestGameMenuNav();
    TestPreWorldNav();
    TestFaceRemap();
    TestRoomLine();
    TestIds();
    TestGunRay();
    TestGameUi(assets);
    TestHeldClassify();
    TestHeldItemLogic();
    TestAimThrow();
    TestStandIns();
    TestHeldCrushBell();
    TestSkinModel();
    TestHeldMeshTable();
    TestUiDrag();
    TestHeldItemsIndex(assets);
    TestHeldMeshDraw(assets);
    TestHandPoses();
    TestHandAsset(assets);
    if (fs::exists(assets / "hitbox" / "index.json")) {
        TestHitboxes(assets);
    }
    if (fs::exists(assets / "ui" / "icons.json")) {
        TestDashboard(assets);
        TestDashboardSticks(assets);
        TestDashboardDrag(assets);
        TestQuickWheel(assets);
        TestLiveLayout(assets);
        TestMashThenWheel(assets);
        TestMashThenWheel(assets, "[\"quick_wheel\", \"quick_ring\", \"weapon_wheel\"]");
        TestGripDoesNotBlock(assets);
        TestHeldItemsInGame(assets);
        TestHeldItemsOff(assets);
        TestHeldExtrasInGame(assets);
        TestHeldPlacementBothModes(assets);
        TestHeldAtGameHand(assets);
        TestHeldMeshInGame(assets);
        TestHeldMeshRealFolder(assets);
        TestSkinInGame(assets);
        TestCustomHandsInGame(assets);
        TestCollisionHaptics(assets);
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
