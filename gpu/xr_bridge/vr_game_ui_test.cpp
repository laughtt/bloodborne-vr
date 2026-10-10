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
#include <string>
#include <vector>

#include "vr_game_ui.h"
#include "vr_mirror_ui.h"
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
    CHECK(!(b.buttons & BBXR_BTN_CROSS));
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
    CHECK(b.buttons & BBXR_BTN_CROSS); // B alone is the game's again

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
    out = apply(Pad(BBXR_BTN_CIRCLE, 255, 128));
    CHECK(out.buttons == BBXR_BTN_CIRCLE && out.lx == 255);
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
    BbXrInput w = Pad(BBXR_BTN_CIRCLE, 255, 128);
    r.ui.Filter().Apply(r.t, w);
    CHECK(w.buttons == BBXR_BTN_CIRCLE && w.lx == 255);
    // A stale pre-world state does nothing.
    r.state.flags = BBXR_GAME_PRE_WORLD;
    r.ui.SetGameState(r.state, r.t);
    r.feed = false;
    for (int i = 0; i < 300; ++i) r.Frame(Pad());
    BbXrInput st = Pad(BBXR_BTN_CIRCLE, 255, 128);
    r.ui.Filter().Apply(r.t, st);
    CHECK(st.buttons == BBXR_BTN_CIRCLE && st.lx == 255);
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
    CHECK(std::strcmp(BBXR_THROW_BULLET_IDS, "100,105,120,121,150,170,180,410,460,470") == 0);
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
    // Throwables: right hand; only the flying ones are aimed from the hand.
    for (const int g : {1200, 1201, 1210, 1220, 1230, 1240, 1270, 2070, 2120, 2130}) {
        const ItemInfo i = Classify(Goods(g));
        CHECK(i.kind == Kind::Throw && i.aimed && HandFor(i) == 1 && InThrowList(i.bullet));
    }
    // Not aimed: the rope molotovs (a back throw at the player), Pungent Blood, coins, gloves.
    for (const int g : {1250, 1251, 1260, 1410, 2080}) {
        const ItemInfo i = Classify(Goods(g));
        CHECK(i.kind == Kind::Throw && !i.aimed);
    }
    // The self-targeted cures' bullets are never in the list.
    for (const int b : {200, 230, 320, 110, 114}) {
        CHECK(!InThrowList(b));
    }
    // Everything else is used at once, as before; weapons are not goods.
    for (const int g : {200, 205, 225, 900, 2040}) {
        CHECK(Classify(Goods(g)).kind == Kind::None);
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
    const sb::UiHeldItems c; // the defaults
    const m3::Vec3 hold = c.hold;
    // -- geometry
    CHECK(std::fabs(DistToSegment({0, 1, 0}, {-1, 0, 0}, {1, 0, 0}) - 1.0f) < 1e-5f);
    CHECK(std::fabs(DistToSegment({3, 0, 0}, {-1, 0, 0}, {1, 0, 0}) - 2.0f) < 1e-5f);
    CHECK(std::fabs(DistToSegment({1, 1, 1}, {0, 0, 0}, {0, 0, 0}) - std::sqrt(3.0f)) < 1e-5f);
    // The mouth: 12 cm around head + (0, -0.10, -0.07), at least 3 cm below the head.
    const m3::Vec3 head{0, 1.6f, 0};
    const m3::Quat id{0, 0, 0, 1};
    float d = 0.0f;
    CHECK(NearMouth({0, 1.5f, -0.07f}, head, id, c, &d) && d < 1e-4f);
    CHECK(NearMouth({0.08f, 1.5f, -0.07f}, head, id, c));
    CHECK(!NearMouth({0.14f, 1.5f, -0.07f}, head, id, c));  // 14 cm to the side
    CHECK(!NearMouth({0, 1.35f, -0.25f}, head, id, c));      // at the chest
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
    CHECK(!TouchesBlade({0.15f, 1.2f, -0.6f}, base, tip, c));
    CHECK(!TouchesBlade({0.0f, 1.2f, 0.0f}, base, tip, c));    // the handle (first 25 %) is not the blade
    CHECK(!TouchesBlade({0.0f, 1.2f, -0.1f}, base, tip, c));
    CHECK(TouchesBlade({0.0f, 1.2f, -0.3f}, base, tip, c));
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
        CHECK(!h.Pick(Goods(200), 0, 0.0));         // a bell: not physical
        CHECK(!h.Active());
        CHECK(h.Pick(Goods(1000), 0, 0.0) && h.GetState() == HeldItem::State::Pending && h.Hand() == 1);
        CHECK(!h.Pick(Goods(1100), 1, 0.0));         // one item at a time
        Frame f = HeldFrame(0.1);
        CHECK(h.Step(f, c).kind == Result::None);    // grip not closed yet
        f.squeeze[1] = 0.9f;
        f.time = 0.2;
        Result r = h.Step(f, c);
        CHECK(h.GetState() == HeldItem::State::Held && r.pulses.size() == 1 && r.pulses[0].hand == 1);
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
    // -- Cancel: the grip opened without a gesture; a menu opened; tracking lost; no grip in time
    {
        HeldItem h;
        h.Pick(Goods(1000), 0, 0.0);
        Frame f = HeldFrame(0.0);
        f.squeeze[1] = 0.9f;
        h.Step(f, c);
        f.squeeze[1] = 0.1f;
        f.time = 0.1;
        Result r = h.Step(f, c);
        CHECK(r.kind == Result::Cancel && r.why == "released" && !h.Active());
        CHECK(r.pulses.size() == 1);
        h.Pick(Goods(1000), 0, 1.0);
        f.squeeze[1] = 0.9f;
        f.time = 1.0;
        h.Step(f, c);
        f.blocked = true;
        f.time = 1.1;
        CHECK(h.Step(f, c).kind == Result::Cancel && !h.Active());
        h.Pick(Goods(1000), 0, 2.0);
        f = HeldFrame(2.1);
        f.hand_valid[1] = false;
        CHECK(h.Step(f, c).kind == Result::Cancel);
        h.Pick(Goods(1000), 0, 3.0);
        f = HeldFrame(3.5);
        CHECK(h.Step(f, c).kind == Result::None && h.Active());         // still within the grace
        f.time = 8.2;
        r = h.Step(f, c);
        CHECK(r.kind == Result::Cancel && !h.Active() && r.pulses.empty()); // 5 s without a grip
        h.Pick(Goods(1000), 0, 9.0);
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
    // -- Throw: letting go fast throws along the release; slow puts it back
    {
        HeldItem h;
        CHECK(h.Pick(Goods(1200), 1, 0.0) && h.Info().aimed);
        Frame f = HeldFrame(0.0);
        f.squeeze[1] = 0.9f;
        h.Step(f, c);
        for (int i = 1; i <= 8; ++i) { // 3.6 m/s along +X
            f.time = i / 90.0;
            f.grip_pos[1] = m3::Vec3{0.2f + 0.04f * float(i), 1.2f, -0.35f};
            CHECK(h.Step(f, c).kind == Result::None);
        }
        f.time = 9 / 90.0;
        f.grip_pos[1] = {0.2f + 0.04f * 9.0f, 1.2f, -0.35f};
        f.squeeze[1] = 0.0f;
        const Result r = h.Step(f, c);
        CHECK(r.kind == Result::Use && r.thrown && r.why == "thrown");
        CHECK(r.throw_speed > 3.0f && r.throw_dir.x > 0.99f);
        CHECK(std::fabs(m3::Length(r.throw_dir) - 1.0f) < 1e-4f);
        CHECK(std::fabs(r.throw_origin.x - (0.2f + 0.36f)) < 1e-4f && std::fabs(r.throw_origin.z - (-0.35f + hold.z)) < 1e-4f);
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
        CHECK(r.kind == Result::Cancel && r.why.find("slowly") != std::string::npos);
    }
    // A throwable that is not aimed (rope molotov) still throws: the game's own direction
    {
        HeldItem h;
        h.Pick(Goods(1250), 0, 0.0);
        CHECK(!h.Info().aimed && h.Info().kind == Kind::Throw);
    }
    // -- the buttons hidden from the game while an item is in a hand
    {
        HeldItem h;
        CHECK(h.BlockMask(0.0, c) == 0);
        h.Pick(Goods(1000), 0, 0.0);
        const uint32_t m = h.BlockMask(0.0, c);
        CHECK((m & BBXR_BTN_R1) && (m & BBXR_BTN_R2) && (m & BBXR_BTN_RIGHT) && !(m & BBXR_BTN_L1));
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
    CHECK(r.ui.TakeActions(actions, 8) == 0);
    CHECK(r.ui.HeldItemState().Active() && r.ui.HeldItemState().Info().goods == 1000 &&
          r.ui.HeldItemState().Hand() == 1);
    // Swings and the swap tap are hidden from the game while it is in the hand
    BbXrInput raw = Pad(BBXR_BTN_R1 | BBXR_BTN_RIGHT | BBXR_BTN_CROSS);
    raw.r2 = 255;
    r.ui.Filter().Apply(r.t, raw);
    CHECK(!(raw.buttons & (BBXR_BTN_R1 | BBXR_BTN_RIGHT)) && (raw.buttons & BBXR_BTN_CROSS));
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
    CHECK(r.StrongestHaptic(1) >= 0.29f);                    // the pick pulse
    r.in.grip[1].position = r.in.aim[1].position = m3::Vec3{0.0f, 1.5f, -0.07f} - m3::Vec3{0.0f, 0.0f, -0.05f};
    for (int i = 0; i < 12; ++i) {
        r.Frame(Pad());
    }
    CHECK(r.ui.HeldItemState().Active());
    for (int i = 0; i < 30 && r.ui.HeldItemState().Active(); ++i) {
        r.Frame(Pad());
    }
    CHECK(!r.ui.HeldItemState().Active());
    uint32_t n = r.ui.TakeActions(actions, 8);
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
    CHECK(r.ui.TakeActions(actions, 8) == 0);
    CHECK(r.ui.HeldItemState().Info().kind == vrui::held::Kind::Throw && r.ui.HeldItemState().Info().bullet == 100);
    r.in.squeeze[1] = 0.9f;
    r.Frame(Pad());
    CHECK(r.ui.HeldItemState().Gripped());
    for (int i = 1; i <= 8; ++i) { // 3.6 m/s along +X, then forward
        r.in.grip[1].position = r.in.aim[1].position = m3::Vec3{0.2f + 0.04f * float(i), 1.2f, -0.35f};
        r.Frame(Pad());
    }
    r.in.squeeze[1] = 0.0f;
    r.in.grip[1].position = r.in.aim[1].position = m3::Vec3{0.2f + 0.04f * 9.0f, 1.2f, -0.35f};
    r.Frame(Pad());
    CHECK(!r.ui.HeldItemState().Active());
    n = r.ui.TakeActions(actions, 8);
    CHECK(n == 3 && actions[0].kind == BBXR_UI_THROW_ORIGIN && actions[1].kind == BBXR_UI_THROW_DIR &&
          actions[2].kind == BBXR_UI_USE_QUICK);
    if (n == 3) {
        // the head is level at (0, 1.6, 0) facing -Z: the head frame is the world minus the head
        CHECK(std::abs(actions[0].a - 560) <= 2 && std::abs(actions[0].b - int(1000 * (1.2f - 1.6f))) <= 2 &&
              std::abs(actions[0].c - int(1000 * (-0.35f - 0.05f))) <= 2);
        CHECK(actions[1].a > 9900 && std::abs(actions[1].b) < 200 && std::abs(actions[1].c) < 200);
        CHECK(actions[2].a == 1 && actions[2].b == int32_t(Goods(1200)) && actions[2].c > 300);
    }
    CHECK(r.all_log.find("goods 1200 thrown (bullet 100, aimed)") != std::string::npos);
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
        r.ui.TakeActions(actions, 8);
    }

    // -- A slow release puts it back: no action
    PickOnWheel(r, 3); // pebble
    r.in.squeeze[1] = 0.9f;
    r.Frame(Pad());
    r.Frame(Pad());
    r.in.squeeze[1] = 0.0f;
    r.Frame(Pad());
    CHECK(!r.ui.HeldItemState().Active() && r.ui.TakeActions(actions, 8) == 0);
    CHECK(r.all_log.find("put back: released too slowly") != std::string::npos);

    // -- Opening the wheel again cancels an item in the hand
    PickOnWheel(r, 0);
    CHECK(r.ui.HeldItemState().Active());
    r.in.squeeze[1] = 0.9f;
    for (int i = 0; i < 60; ++i) { // R3 held 0.5 s opens the wheel
        r.ui.SetGameState(r.state, r.t);
        r.Frame(Pad(BBXR_BTN_R3));
    }
    CHECK(!r.ui.HeldItemState().Active());
    CHECK(r.ui.TakeActions(actions, 8) == 0);
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
    n = r.ui.TakeActions(actions, 8);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK && actions[0].a == 2 && actions[0].b == int32_t(Goods(1300)));
    r.in.squeeze[0] = 0.0f;
    r.in.grip[0].position = r.in.aim[0].position = {-0.2f, 1.2f, -0.35f};

    // -- Items that are not physical (a bell) are used at once, as before
    r.ui.PushEvent({BBXR_EV_ACTION_DONE, int32_t(BBXR_UI_USE_QUICK), 1, 0}, r.t);
    for (int i = 0; i < 100; ++i) {
        r.Frame(Pad());
    }
    r.state.quick[4] = Goods(200);
    r.state.debug = {};
    r.ui.SetGameState(r.state, r.t);
    r.Frame(Pad());
    PickOnWheel(r, 4);
    CHECK(!r.ui.HeldItemState().Active());
    n = r.ui.TakeActions(actions, 8);
    CHECK(n == 1 && actions[0].kind == BBXR_UI_USE_QUICK && actions[0].a == 4 && actions[0].b == int32_t(Goods(200)));
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
    CHECK(std::fabs(l.game.held.mouth_radius_m - 0.12f) < 1e-6f && std::fabs(l.game.held.throw_min_speed - 1.5f) < 1e-6f);
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

int main(int argc, char** argv) {
    const fs::path assets = argc > 1 ? fs::path(argv[1]) : fs::path("out/vr_assets");
    TestFilter();
    TestMirrorProjection();
    TestFilterMash();
    TestGameMenuNav();
    TestPreWorldNav();
    TestIds();
    TestGunRay();
    TestGameUi(assets);
    TestHeldClassify();
    TestHeldItemLogic();
    if (fs::exists(assets / "hitbox" / "index.json")) {
        TestHitboxes(assets);
    }
    if (fs::exists(assets / "ui" / "icons.json")) {
        TestDashboard(assets);
        TestDashboardSticks(assets);
        TestQuickWheel(assets);
        TestLiveLayout(assets);
        TestMashThenWheel(assets);
        TestMashThenWheel(assets, "[\"quick_wheel\", \"quick_ring\", \"weapon_wheel\"]");
        TestGripDoesNotBlock(assets);
        TestHeldItemsInGame(assets);
        TestHeldItemsOff(assets);
        TestCollisionHaptics(assets);
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
