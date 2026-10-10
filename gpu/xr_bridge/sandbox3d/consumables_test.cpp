// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_xr_sandbox3d_consume_test.exe [out/vr_assets]: offline tests of the consumables
// (consumables.h, docs/vr/findings/36-vr-consumables.md), no headset. Scripted hands (90 Hz,
// controller pose + squeeze) and the R3 button drive the wheel together with the real Throwables:
//   - the data: groups, gestures, stand-in shapes, the items.json overrides, the sample inventory;
//   - the wheel: R3 hold opens a layout of 3D objects below eye level, empty items are not
//     listed, the linger, the counts from the config, the old "cards" switch;
//   - take with the grip, put back; the Blood Vial brought to the mouth (used after 0.3 s, a fly-by
//     is not, HP and count change); cures, buffs with durations, insight;
//   - the paper rub: along the blade within 1.5 s uses it (fire, bolt: element, 60 s, count -1,
//     haptics), a short or slow rub does not, the wrong hand cannot take it, Bone Marrow Ash
//     buffs the left (gun) weapon, a buff wears off;
//   - a throwable from the wheel is thrown by the hand velocity and used up when thrown;
//   - crush (Hunter's Mark), quicksilver bullet costs, the jsonl log.
// Exit code 0 when every check passes. BB_CONSUME_TEST_VERBOSE=1 prints the logs.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bbport_xr_bridge.h"
#include "consumables.h"
#include "model.h"
#include "physics_weapon.h"
#include "throwables.h"

using namespace m3;

namespace {

int failures = 0, checks = 0;

void Check(bool ok, const char* what, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL consumables_test.cpp line %d: %s\n", line, what);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

bool Near(float a, float b, float tol) {
    return std::fabs(a - b) <= tol;
}
#define CHECK_NEAR(a, b, tol)                                                              \
    do {                                                                                   \
        const float va = float(a), vb = float(b);                                          \
        Check(Near(va, vb, float(tol)), #a " ~ " #b, __LINE__);                            \
        if (!Near(va, vb, float(tol))) {                                                   \
            std::printf("    %s = %g, %s = %g (tol %g)\n", #a, double(va), #b, double(vb), \
                        double(tol));                                                      \
        }                                                                                  \
    } while (0)

bool Verbose() {
    return std::getenv("BB_CONSUME_TEST_VERBOSE") != nullptr;
}

constexpr double kDt = 1.0 / 90.0;
const Vec3 kHead{0.0f, 1.65f, 0.0f};

/// The wheel, the throwables, the scripted hands, a right weapon blade and a left gun.
struct Sim {
    sb::Consumables c;
    sb::Throwables t;
    sb::ConsumeConfig cfg;
    sb::ThrowConfig tc;
    sb::WeaponPhysics phys;
    sb::PhysicsConfig pc;
    sb::ConsumeInput in;
    double time = 0.0;
    std::string log;
    sb::ConsumeEvents last;
    std::string toast;
    float peak[2] = {0, 0};

    explicit Sim(const char* inventory = nullptr, const std::filesystem::path& assets = "no-such-assets") {
        cfg.log = false;
        cfg.log_file = "";
        cfg.wheel_mode = "hold"; // (the toggle wheel has its own test)
        cfg.throwables_on_wheel = true; // (the old wheel; the defaults have their own test)
        cfg.buff_rub = true;
        if (inventory) {
            std::string error;
            cfg.inventory = json::Parse(inventory, &error);
        }
        tc.log = false;
        tc.log_file = "";
        tc.rack_grab = false;
        pc.props = false;
        pc.enemy = "";
        pc.contact_log = "";
        pc.log_contacts = false;
        std::string l;
        t.Load(assets, tc, l);
        t.Place(kHead, Quat{}, 0.0f, tc);
        c.Load(assets, cfg, t.Defs(), l);
        in.head_valid = true;
        in.head_position = kHead;
        in.head_rotation = Quat{};
        for (int s = 0; s < 2; ++s) {
            in.hand_valid[s] = true;
            in.grip_rotation[s] = Quat{};
            in.grip_position[s] = {s ? 0.3f : -0.3f, 0.9f, -0.2f};
        }
        // The right weapon: a blade of 0.8 m pointing forward and up from the right grip; the left
        // weapon a 0.3 m gun barrel.
        SetWeapons();
        phys.PlaceProps(kHead, Quat{});
        sb::WeaponPhysics::StepInput si;
        si.head_valid = true;
        si.head_position = kHead;
        sb::PhysicsEvents pe;
        phys.Step(si, pc, pe);
    }
    static Vec3 BladeDir() { return Normalize(Vec3{0.0f, 0.5f, -1.0f}); }
    void SetWeapons() {
        in.weapon_valid[1] = true;
        in.weapon_base[1] = {0.35f, 1.0f, -0.35f};
        in.weapon_tip[1] = in.weapon_base[1] + BladeDir() * 0.8f;
        in.weapon_valid[0] = true;
        in.weapon_base[0] = {-0.35f, 1.0f, -0.35f};
        in.weapon_tip[0] = in.weapon_base[0] + Vec3{0, 0, -0.3f};
    }
    sb::ThrowInput ThrowIn() const {
        sb::ThrowInput ti;
        ti.time = in.time;
        ti.head_valid = in.head_valid;
        ti.head_position = in.head_position;
        ti.head_rotation = in.head_rotation;
        for (int s = 0; s < 2; ++s) {
            ti.hand_valid[s] = in.hand_valid[s];
            ti.grip_position[s] = in.grip_position[s];
            ti.grip_rotation[s] = in.grip_rotation[s];
            ti.squeeze[s] = in.squeeze[s];
            ti.trigger[s] = in.trigger[s];
        }
        ti.buttons = in.buttons;
        return ti;
    }
    void Tick() {
        time += kDt;
        in.time = time;
        sb::ThrowInput ti = ThrowIn();
        sb::ThrowEvents te;
        t.Step(ti, tc, phys, te);
        last = {};
        c.Step(in, cfg, t, tc, ti, te, last);
        log += te.log + last.log;
        for (int s = 0; s < 2; ++s) {
            peak[s] = std::max(peak[s], std::max(last.haptic_amplitude[s], te.haptic_amplitude[s]));
        }
        if (!last.toast_title.empty()) {
            toast = last.toast_title + ": " + last.toast_body;
        }
        if (Verbose() && !(te.log + last.log).empty()) {
            std::printf("  %7.3f %s", time, (te.log + last.log).c_str());
        }
    }
    void Run(double seconds) {
        for (double end = time + seconds; time < end - 1e-9;) {
            Tick();
        }
    }
    void OpenWheel(double seconds = 0.2) {
        in.buttons |= BBXR_BTN_R3;
        Run(seconds);
    }
    void CloseWheel() { in.buttons &= ~BBXR_BTN_R3; }

    int Slot(int goods) const {
        for (size_t i = 0; i < c.Slots().size(); ++i) {
            if (c.DefOf(c.Slots()[i].entry).goods == goods && c.SlotVisible(int(i))) {
                return int(i);
            }
        }
        return -1;
    }
    /// The grip position that puts the grab point on an object of the wheel.
    Vec3 GripFor(int slot) const {
        return c.SlotPos(slot, 0.0f) - cfg.grab_point;
    }
    /// Opens the wheel (if needed), takes `goods` with `hand`: hand to the object, then close it.
    bool Take(int goods, int hand) {
        if (!c.WheelOpen()) {
            OpenWheel();
        }
        const int slot = Slot(goods);
        if (slot < 0) {
            return false;
        }
        in.grip_position[hand] = GripFor(slot);
        in.squeeze[hand] = 0.0f;
        Run(0.1);
        in.squeeze[hand] = 0.9f;
        Tick();
        return c.HeldBy(hand).entry >= 0;
    }
    /// Carries the held item (hold offset) to `item_pos` over `seconds`.
    void CarryTo(int hand, Vec3 item_pos, double seconds) {
        const Vec3 hold = c.HeldBy(hand).entry >= 0 ? c.DefOf(c.HeldBy(hand).entry).hold : Vec3{0, 0, -0.05f};
        const Vec3 from = in.grip_position[hand], to = item_pos - hold;
        const int n = std::max(1, int(std::lround(seconds / kDt)));
        for (int i = 1; i <= n; ++i) {
            in.grip_position[hand] = Lerp(from, to, float(i) / float(n));
            Tick();
        }
    }
    Vec3 Mouth() const { return kHead + cfg.mouth; }
    /// A point along the right blade at share t of its length (+ a little off the edge).
    Vec3 BladePoint(float t, int side = 1) const {
        return in.weapon_base[side] + (in.weapon_tip[side] - in.weapon_base[side]) * t + Vec3{0.0f, 0.02f, 0.0f};
    }
    bool Logged(const std::string& needle) const {
        for (const std::string& l : c.EventLog()) {
            if (l.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

// ---- Data ----------------------------------------------------------------------------------------

void TestData(const std::filesystem::path& assets) {
    const std::vector<sb::ConsumableDef> defs = sb::DefaultConsumables();
    CHECK(defs.size() >= 20);
    int seen[6] = {};
    for (const sb::ConsumableDef& d : defs) {
        seen[int(d.group)]++;
        // Every shape has a stand-in mesh.
        const sb::Model m = sb::MakeThrowModel(d.shape, 1.0f);
        CHECK(!m.vertices.empty() && !m.indices.empty());
        if (d.group == sb::CGroup::WeaponBuff) {
            CHECK(d.gesture == sb::CGesture::Rub && !d.element.empty() && d.duration_s > 0.0f);
        }
        if (d.group == sb::CGroup::Heal) {
            CHECK(d.gesture == sb::CGesture::Head && d.heal_pct > 0.0f);
        }
        if (d.gesture == sb::CGesture::Crush) {
            CHECK(d.group == sb::CGroup::Tool);
        }
    }
    CHECK(seen[int(sb::CGroup::WeaponBuff)] >= 3 && seen[int(sb::CGroup::Heal)] >= 1 &&
          seen[int(sb::CGroup::Cure)] >= 2 && seen[int(sb::CGroup::BodyBuff)] >= 4 &&
          seen[int(sb::CGroup::Tool)] >= 5);
    // Fire / Bolt papers go to the right weapon, Bone Marrow Ash to the left (gun) one.
    for (const sb::ConsumableDef& d : defs) {
        if (d.goods == 1300 || d.goods == 1320 || d.goods == 1310) {
            CHECK(d.target_side == 1);
        }
        if (d.goods == 1330) {
            CHECK(d.target_side == 0 && d.element == "gun");
        }
    }
    // The inventory covers the throwables and the sample counts.
    Sim s;
    CHECK(s.c.Count(1000) == 20 && s.c.Count(1300) == 10 && s.c.Count(1230) == 12);
    CHECK(s.c.FindEntry(1200) >= 0 && s.c.DefOf(s.c.FindEntry(1200)).gesture == sb::CGesture::Throw);
    CHECK(s.c.Count(900) == 30);
    // The dump's table, when extracted (out/vr_assets/items/items.json).
    bool ok = false;
    const json::Value items = json::Parse(sb::ReadText(assets / "items" / "items.json", &ok));
    if (ok) {
        std::vector<sb::ConsumableDef> d2 = sb::DefaultConsumables();
        const int matched = sb::ApplyItemsTable(d2, items);
        CHECK(matched >= 15);
        for (const sb::ConsumableDef& d : d2) {
            if (d.goods == 1300) {
                CHECK_NEAR(d.duration_s, 60.0f, 0.01f);
            }
            if (d.goods == 1120) {
                CHECK_NEAR(d.duration_s, 30.0f, 0.01f);   // Blue Elixir: SpEffect 2014, 30 s
            }
            if (d.goods == 2060) {
                CHECK_NEAR(d.duration_s, 16.0f, 0.01f);   // Old Hunter Bone: 16 s
                CHECK(d.bullet_cost == 4 && !d.consumed);
            }
            if (d.goods == 1000) {
                CHECK_NEAR(d.heal_pct, 40.0f, 0.01f);
            }
        }
        Sim loaded("", assets);
        CHECK(loaded.c.DefOf(loaded.c.FindEntry(1110)).duration_s > 0.0f);
    } else {
        std::printf("note: %s has no items/items.json; skipped\n", assets.string().c_str());
    }
    // Extracted meshes of the consumables, when present.
    int meshes = 0;
    Sim withm("", assets);
    for (size_t i = 0; i < withm.c.Entries().size(); ++i) {
        if (withm.c.HasMesh(int(i))) {
            sb::Model m;
            std::string error;
            CHECK(sb::LoadModel(withm.c.ModelPath(int(i)), m, error));
            CHECK(!m.vertices.empty());
            ++meshes;
        }
    }
    std::printf("consumables meshes found: %d\n", meshes);
}

// ---- The wheel -------------------------------------------------------------------------------------

void TestWheel() {
    Sim s;
    CHECK(!s.c.WheelOpen());
    s.OpenWheel(0.2);
    CHECK(s.c.WheelOpen());
    CHECK(int(s.c.Slots().size()) == s.c.ShownCount());
    CHECK(s.c.ShownCount() >= 35);
    // All objects are in front of the face, below eye level, at about the configured distance.
    float lowest = 10.0f, highest = -10.0f, dmin = 10.0f;
    for (const sb::Consumables::Slot& slot : s.c.Slots()) {
        const Vec3 d = slot.pos - kHead;
        CHECK(slot.pos.y < kHead.y - 0.05f);
        CHECK(d.z < -0.1f);
        const float dist = Length(d);
        CHECK(dist >= s.cfg.distance_m * 0.95f && dist <= s.cfg.distance_m + 4.0f * s.cfg.row_spacing_m);
        lowest = std::min(lowest, slot.pos.y);
        highest = std::max(highest, slot.pos.y);
    }
    CHECK(highest <= kHead.y - s.cfg.distance_m * std::sin(s.cfg.down_deg * 0.0174533f) + 0.001f);
    CHECK(lowest > 0.6f); // (the rows still fit in reach)
    // No two objects overlap.
    for (size_t i = 0; i < s.c.Slots().size(); ++i) {
        for (size_t j = i + 1; j < s.c.Slots().size(); ++j) {
            dmin = std::min(dmin, Length(s.c.Slots()[i].pos - s.c.Slots()[j].pos));
        }
    }
    CHECK(dmin > s.cfg.spacing_m * 0.7f);
    // The order: throwables first, tools last.
    CHECK(s.c.DefOf(s.c.Slots().front().entry).group == sb::CGroup::Throwable);
    CHECK(s.c.DefOf(s.c.Slots().back().entry).group == sb::CGroup::Tool);
    // Counts: the label text.
    CHECK(s.c.Label(s.c.FindEntry(1000)) == "x20");
    CHECK(s.c.Label(s.c.FindEntry(200)) == "~");
    // Closing: a linger after the release, then the wheel is gone.
    s.CloseWheel();
    s.Run(s.cfg.linger_s * 0.5);
    CHECK(s.c.WheelOpen());
    s.Run(s.cfg.linger_s);
    CHECK(!s.c.WheelOpen() && s.c.Slots().empty());
    // The shelf layout is flat in front of the head.
    s.cfg.layout = "shelf";
    s.OpenWheel(0.1);
    float zmin = 10.0f, zmax = -10.0f;
    for (const sb::Consumables::Slot& slot : s.c.Slots()) {
        zmin = std::min(zmin, slot.pos.z);
        zmax = std::max(zmax, slot.pos.z);
    }
    CHECK(zmax - zmin < 0.01f);
    s.CloseWheel();
    s.Run(2.0);
    // The facing: the wheel follows the head's yaw when it opens.
    s.cfg.layout = "arc";
    s.in.head_rotation = AxisAngle({0, 1, 0}, 0.5f * 3.14159265f); // looking at -X
    s.OpenWheel(0.1);
    CHECK(s.c.WheelOpen());
    Vec3 centre{0, 0, 0};
    for (const sb::Consumables::Slot& slot : s.c.Slots()) {
        centre = centre + slot.pos;
    }
    centre = centre * (1.0f / float(s.c.Slots().size()));
    CHECK(centre.x < -0.2f && std::fabs(centre.z) < 0.15f);

    // Empty items are not listed; an inventory from the config.
    Sim e("{\"1000\": 2, \"1300\": 1, \"1230\": 0, \"900\": 10}");
    e.OpenWheel(0.1);
    CHECK(e.c.ShownCount() == 2 && e.c.Slots().size() == 2);
    CHECK(e.Slot(1000) >= 0 && e.Slot(1300) >= 0 && e.Slot(1230) < 0);
    // The old card wheel: the objects wheel stays out of the way.
    Sim old;
    old.cfg.wheel = "cards";
    old.in.buttons |= BBXR_BTN_R3;
    old.Run(0.3);
    CHECK(!old.c.WheelOpen() && old.c.Slots().empty() && !old.cfg.Objects());
    // The config reads and writes its keys.
    sb::ConsumeConfig lc;
    lc.Load(json::Parse("{\"wheel\": \"cards\", \"down_deg\": 20, \"per_row\": 6, \"coverage\": 0.5, "
                        "\"inventory\": {\"1000\": 3}}"));
    CHECK(lc.wheel == "cards" && lc.per_row == 6 && Near(lc.down_deg, 20, 1e-3f) &&
          Near(lc.coverage, 0.5f, 1e-3f) && lc.inventory["1000"].Int() == 3);
    sb::ConsumeConfig rc;
    rc.Load(json::Parse(rc.Summary()));
    CHECK(rc.wheel == "objects" && rc.per_row == 10 && Near(rc.coverage, 0.5f, 1e-3f) &&
          rc.inventory.IsObject() && rc.inventory["1000"].Int() == 20);
}

// ---- Eating and drinking ---------------------------------------------------------------------------

void TestHead() {
    Sim s;
    CHECK(s.c.HpPct() == 55.0f);
    CHECK(s.Take(1000, 0));
    CHECK(s.c.HeldBy(0).entry == s.c.FindEntry(1000));
    CHECK(s.peak[0] > 0.3f); // the pick tick
    // The count is not changed by picking, and the object is still there for a second vial.
    CHECK(s.c.Count(1000) == 20 && s.Slot(1000) >= 0);
    // The item rides the hand.
    s.in.grip_position[0] = {-0.2f, 1.2f, -0.3f};
    s.in.grip_rotation[0] = AxisAngle({0, 1, 0}, 0.5f * 3.14159265f);
    s.Tick();
    const Vec3 want = s.in.grip_position[0] + Rotate(s.in.grip_rotation[0], s.c.DefOf(s.c.HeldBy(0).entry).hold);
    CHECK_NEAR(Length(s.c.HeldBy(0).pos - want), 0.0f, 1e-4f);
    s.in.grip_rotation[0] = Quat{};
    // Pass by the head fast (under 0.3 s): not used.
    s.CloseWheel();
    s.CarryTo(0, s.Mouth() + Vec3{-0.35f, 0.0f, 0.0f}, 0.2);
    s.CarryTo(0, s.Mouth(), 0.05);
    s.CarryTo(0, s.Mouth() + Vec3{-0.35f, 0.0f, 0.0f}, 0.05);
    CHECK(s.c.Count(1000) == 20 && s.c.HeldBy(0).entry >= 0);
    // Hover too high (at the eyes): not used.
    s.CarryTo(0, kHead + Vec3{0.0f, 0.0f, -0.08f}, 0.3);
    s.Run(0.6);
    CHECK(s.c.Count(1000) == 20 && s.c.HeldBy(0).entry >= 0);
    // Hover far from the face: not used.
    s.CarryTo(0, s.Mouth() + Vec3{-0.25f, -0.1f, 0.0f}, 0.3);
    s.Run(0.6);
    CHECK(s.c.Count(1000) == 20);
    // To the mouth and hold: not yet after 0.2 s, used after 0.3 s.
    s.CarryTo(0, s.Mouth(), 0.1);
    s.Run(0.15);
    CHECK(s.c.HeldBy(0).entry >= 0);
    s.Run(0.3);
    CHECK(s.c.HeldBy(0).entry < 0);
    CHECK(s.c.Count(1000) == 19);
    CHECK_NEAR(s.c.HpPct(), 95.0f, 0.01f); // 55 + 40 %
    CHECK_NEAR(s.c.LastHeal(), 40.0f, 0.01f);
    CHECK(s.Logged("used Blood Vial"));
    CHECK(s.toast.find("Blood Vial") != std::string::npos);
    CHECK(s.peak[0] >= 0.8f); // the use haptic
    // HP is capped at 100.
    CHECK(s.Take(1000, 1));
    s.CarryTo(1, s.Mouth(), 0.5);
    s.Run(0.5);
    CHECK_NEAR(s.c.HpPct(), 100.0f, 0.01f);
    CHECK(s.c.Count(1000) == 18);
    s.in.squeeze[1] = 0.0f;
    s.Tick();
    // Putting an item back: let go away from the head.
    CHECK(s.Take(1100, 0)); // Antidote
    s.in.squeeze[0] = 0.1f;
    s.Tick();
    CHECK(s.c.HeldBy(0).entry < 0 && s.c.Count(1100) == 4 && s.Logged("put back Antidote"));
    // The Antidote cures poison.
    s.c.AddAilment("poison");
    CHECK(s.c.Ailments().size() == 1);
    s.in.squeeze[0] = 0.0f;
    CHECK(s.Take(1100, 0));
    s.CarryTo(0, s.Mouth(), 0.4);
    s.Run(0.6);
    CHECK(s.c.Ailments().empty() && s.c.Count(1100) == 3 && s.Logged("poison cured"));
    // A timed buff: Beast Blood Pellet (60 s) and Blue Elixir (30 s).
    s.in.squeeze[0] = 0.0f;
    CHECK(s.Take(1110, 0));
    s.CarryTo(0, s.Mouth(), 0.4);
    s.Run(0.6);
    CHECK(s.c.Statuses().size() == 1 && s.c.Count(1110) == 2);
    CHECK_NEAR(s.c.Statuses()[0].duration, 60.0f, 0.01f);
    s.in.squeeze[0] = 0.0f;
    CHECK(s.Take(1120, 0));
    s.CarryTo(0, s.Mouth(), 0.4);
    s.Run(0.6);
    CHECK(s.c.Statuses().size() == 2 && s.c.Count(1120) == 1);
    s.Run(31.0);
    CHECK(s.c.Statuses().size() == 1); // the Blue Elixir (30 s) ran out, the pellet has not
    s.Run(31.0);
    CHECK(s.c.Statuses().empty() && s.Logged("Blue Elixir wore off"));
    // Insight (Madman's Knowledge), a bell (reusable, near the ear), the bullets-priced bone.
    s.in.squeeze[0] = 0.0f;
    CHECK(s.Take(1500, 0));
    s.CarryTo(0, s.Mouth(), 0.4);
    s.Run(0.6);
    CHECK(s.c.Insight() == 1 && s.c.Count(1500) == 1);
    s.in.squeeze[0] = 0.0f;
    CHECK(s.Take(200, 0));
    s.CarryTo(0, s.Mouth(), 0.4);
    s.Run(0.6);
    CHECK(s.c.Count(200) == 1 && s.Logged("used Beckoning Bell")); // not used up
    s.in.squeeze[0] = 0.0f;
    CHECK(s.Take(2060, 0)); // Old Hunter Bone: 4 quicksilver bullets
    s.CarryTo(0, s.Mouth(), 0.4);
    s.Run(0.6);
    CHECK(s.c.Count(2060) == 1 && s.c.Count(900) == 26);
    CHECK(s.c.Statuses().size() == 1 && Near(s.c.Statuses()[0].duration, 16.0f, 0.01f));
    // Too few bullets: refused, the item stays in the hand.
    s.c.SetCount(900, 3);
    s.in.squeeze[0] = 0.0f;
    s.Run(0.1);
    const bool took = s.Take(2060, 0);
    CHECK(!took && s.c.HeldBy(0).entry < 0);
    CHECK(s.toast.find("quicksilver") != std::string::npos);
}

// ---- Weapon buffs ----------------------------------------------------------------------------------

/// Rubs the item held in `hand` along the weapon of `side` from share a to b over `seconds`.
void Rub(Sim& s, int hand, int side, float a, float b, double seconds) {
    const Vec3 hold = s.c.DefOf(s.c.HeldBy(hand).entry).hold;
    (void)hold;
    const int n = std::max(1, int(std::lround(seconds / kDt)));
    for (int i = 0; i <= n; ++i) {
        const float t = a + (b - a) * float(i) / float(n);
        s.in.grip_position[hand] = s.BladePoint(t, side) - s.c.DefOf(s.c.HeldBy(hand).entry >= 0 ? s.c.HeldBy(hand).entry : 0).hold;
        s.Tick();
        if (s.c.HeldBy(hand).entry < 0) {
            break;
        }
    }
}

void TestRub() {
    // The right hand takes a paper too and rubs it onto the other hand's (left) weapon.
    {
        Sim s;
        CHECK(s.Take(1300, 1) && s.c.HeldBy(1).entry >= 0 && s.c.HeldBy(1).target == 0);
        CHECK(s.Logged("rub target: left weapon present"));
        CHECK(s.toast.find("left weapon") != std::string::npos);
        s.CloseWheel();
        s.CarryTo(1, s.BladePoint(0.3f, 0), 0.4);
        Rub(s, 1, 0, 0.3f, 1.0f, 0.6);
        CHECK(s.c.Buff(0).active && s.c.Buff(0).element == "fire" && !s.c.Buff(1).active);
        CHECK(s.c.Count(1300) == 9);
    }
    // Diagnostics: far from the blade, then touching it; no weapon in the target hand.
    {
        Sim s;
        CHECK(s.Take(1300, 0));
        s.CloseWheel();
        s.CarryTo(0, s.BladePoint(0.6f) + Vec3{0.3f, 0.0f, 0.0f}, 0.3);
        s.Run(0.7);
        CHECK(s.Logged("too far from the blade"));
        s.CarryTo(0, s.BladePoint(0.6f), 0.3);
        s.Run(0.6);
        CHECK(s.Logged("in contact, move along the blade"));
        CHECK(s.Logged("rub Fire Paper"));
        s.in.weapon_valid[1] = false;
        s.Run(0.7);
        CHECK(s.Logged("no weapon in the right hand"));
    }
    // A forgiving rub: 9 cm off the blade, 55 % of it, over 2 s.
    {
        Sim s;
        CHECK(s.Take(1300, 0));
        s.CloseWheel();
        s.CarryTo(0, s.BladePoint(0.4f) + Vec3{0.0f, 0.09f, 0.0f}, 0.3);
        for (int i = 0; i <= 180; ++i) {
            s.in.grip_position[0] = s.BladePoint(0.4f + 0.5f * float(i) / 180.0f) + Vec3{0.0f, 0.09f, 0.0f} - Vec3{0, 0, -0.05f};
            s.Tick();
        }
        CHECK(s.c.Buff(1).active);
    }
    // A full rub, base to tip in 0.8 s: fire, 60 s.
    {
        Sim s;
        CHECK(s.Take(1300, 0));
        CHECK(s.c.DefOf(s.c.HeldBy(0).entry).gesture == sb::CGesture::Rub);
        s.CloseWheel();
        // Approach the blade, then stroke along it.
        s.CarryTo(0, s.BladePoint(0.3f), 0.4);
        s.Run(0.1);
        CHECK(!s.c.Buff(1).active);
        Rub(s, 0, 1, 0.3f, 1.0f, 0.8);
        CHECK(s.c.Buff(1).active && s.c.Buff(1).element == "fire");
        CHECK_NEAR(s.c.Buff(1).duration, 60.0f, 0.01f);
        CHECK(s.c.Buff(1).color[0] > 0.9f && s.c.Buff(1).color[2] < 0.2f); // orange
        CHECK(s.c.Count(1300) == 9 && s.c.HeldBy(0).entry < 0);
        CHECK(!s.c.Buff(0).active);
        CHECK(s.Logged("buff applied") && s.toast.find("Fire Paper") != std::string::npos);
        CHECK(s.peak[0] >= 0.5f && s.peak[1] >= 0.5f); // rub + done haptics on both hands
        // The buff wears off after its 60 s.
        s.Run(59.0);
        CHECK(s.c.Buff(1).active);
        s.Run(1.5);
        CHECK(!s.c.Buff(1).active && s.Logged("wore off"));
        // The next paper replaces a running buff (Bolt: blue).
        s.in.squeeze[0] = 0.0f;
        s.in.buttons |= BBXR_BTN_R3;
        CHECK(s.Take(1320, 0));
        s.CloseWheel();
        s.CarryTo(0, s.BladePoint(0.3f), 0.4);
        Rub(s, 0, 1, 0.3f, 1.0f, 1.0);
        CHECK(s.c.Buff(1).active && s.c.Buff(1).element == "bolt" && s.c.Buff(1).color[2] > 0.9f);
        CHECK(s.c.Count(1320) == 5);
    }
    // Back-and-forth strokes count (the covered range, not the travel).
    {
        Sim s;
        CHECK(s.Take(1300, 0));
        s.CloseWheel();
        s.CarryTo(0, s.BladePoint(0.4f), 0.3);
        Rub(s, 0, 1, 0.4f, 0.7f, 0.3);
        CHECK(!s.c.Buff(1).active);
        Rub(s, 0, 1, 0.7f, 0.95f, 0.3);
        CHECK(s.c.Buff(1).active);
    }
    // An insufficient rub: a short stroke (about 30 % of the blade), many times.
    {
        Sim s;
        CHECK(s.Take(1300, 0));
        s.CloseWheel();
        s.CarryTo(0, s.BladePoint(0.4f), 0.3);
        for (int k = 0; k < 6; ++k) {
            Rub(s, 0, 1, 0.4f, 0.65f, 0.15);
            Rub(s, 0, 1, 0.65f, 0.4f, 0.15);
        }
        CHECK(!s.c.Buff(1).active && s.c.Count(1300) == 10 && s.c.HeldBy(0).entry >= 0);
        CHECK(s.c.RubProgress(0) < 0.6f);
    }
    // A slow rub: the whole blade, but over 6 s (never 60 % inside 1.5 s).
    {
        Sim s;
        CHECK(s.Take(1300, 0));
        s.CloseWheel();
        s.CarryTo(0, s.BladePoint(0.3f), 0.3);
        Rub(s, 0, 1, 0.3f, 1.0f, 6.0);
        CHECK(!s.c.Buff(1).active && s.c.Count(1300) == 10);
    }
    // Away from the blade (a stroke 20 cm beside it): no contact, nothing.
    {
        Sim s;
        CHECK(s.Take(1300, 0));
        s.CloseWheel();
        s.CarryTo(0, s.BladePoint(0.3f) + Vec3{0.2f, 0.0f, 0.0f}, 0.3);
        for (int i = 0; i <= 72; ++i) {
            const float t = 0.3f + 0.7f * float(i) / 72.0f;
            s.in.grip_position[0] = s.BladePoint(t) + Vec3{0.2f, 0.0f, 0.0f} - s.c.DefOf(s.c.HeldBy(0).entry).hold;
            s.Tick();
        }
        CHECK(!s.c.Buff(1).active && s.c.Count(1300) == 10);
        // Across the blade (perpendicular, same spot): no length covered.
        for (int i = 0; i <= 72; ++i) {
            const float x = -0.1f + 0.2f * float(i) / 72.0f;
            s.in.grip_position[0] = s.BladePoint(0.6f) + Vec3{x, 0.0f, 0.0f} - s.c.DefOf(s.c.HeldBy(0).entry).hold;
            s.Tick();
        }
        CHECK(!s.c.Buff(1).active);
    }
    // Bone Marrow Ash: the right hand takes it and dusts the left (gun) weapon.
    {
        Sim s;
        CHECK(s.Take(1330, 1));
        s.CloseWheel();
        s.CarryTo(1, s.BladePoint(0.3f, 0), 0.4);
        Rub(s, 1, 0, 0.3f, 1.0f, 0.4);
        CHECK(s.c.Buff(0).active && s.c.Buff(0).element == "gun" && !s.c.Buff(1).active);
        CHECK(s.c.Count(1330) == 3);
    }
    // Without a weapon in the target hand there is nothing to rub.
    {
        Sim s;
        s.in.weapon_valid[1] = false;
        CHECK(s.Take(1300, 0));
        s.CloseWheel();
        s.CarryTo(0, s.BladePoint(0.3f), 0.4);
        Rub(s, 0, 1, 0.3f, 1.0f, 0.8);
        CHECK(!s.c.Buff(1).active);
    }
    // The phantasm shell (arcane, 3 bullets, reusable).
    {
        Sim s;
        CHECK(s.Take(1310, 0));
        s.CloseWheel();
        s.CarryTo(0, s.BladePoint(0.3f), 0.4);
        Rub(s, 0, 1, 0.3f, 1.0f, 0.8);
        CHECK(s.c.Buff(1).active && s.c.Buff(1).element == "arcane");
        CHECK(s.c.Count(1310) == 1 && s.c.Count(900) == 27);
    }
}

// ---- Throwables from the wheel -----------------------------------------------------------------

void TestThrow() {
    Sim s;
    CHECK(s.t.ThrownGoods().empty());
    CHECK(s.Take(1230, 1)); // a Pebble
    CHECK(s.c.HeldBy(1).via_throwables && s.t.HeldItem(1) >= 0);
    CHECK(s.c.HandBusy(1));
    s.CloseWheel();
    // Carry it up and throw forward.
    const Vec3 start{0.2f, 1.3f, -0.1f};
    const Vec3 from = s.in.grip_position[1];
    for (int i = 1; i <= 54; ++i) {
        s.in.grip_position[1] = Lerp(from, start - Vec3{0, 0, -0.05f}, float(i) / 54.0f);
        s.Tick();
    }
    s.Run(0.1);
    CHECK(s.c.Count(1230) == 12); // holding is free
    for (int i = 0; i < 23; ++i) {
        s.in.grip_position[1] = s.in.grip_position[1] + Vec3{0, 0, -1}* (8.0f * float(kDt));
        s.Tick();
    }
    s.in.squeeze[1] = 0.0f;
    s.Tick();
    s.Run(0.1);
    CHECK(!s.t.Bullets().empty() || !s.t.Throws().empty());
    CHECK(s.t.ThrownGoods().size() == 1 && s.t.ThrownGoods()[0] == 1230);
    CHECK(s.c.Count(1230) == 11);
    CHECK(s.c.HeldBy(1).entry < 0 && !s.c.HandBusy(1));
    CHECK(s.Logged("thrown Pebble"));
    s.Run(3.0);
    // A drop (a weak let-go) does not use the item up; it falls and can be picked off the floor
    // by Throwables, a throw of it then uses it up.
    s.in.squeeze[1] = 0.0f;
    s.in.buttons |= BBXR_BTN_R3;
    CHECK(s.Take(1240, 1));
    s.CloseWheel();
    s.in.squeeze[1] = 0.0f;
    s.Tick();
    s.Run(1.0);
    CHECK(s.c.Count(1240) == 10 && s.t.ThrownGoods().size() == 1);
    // Another throwable in the other hand at the same time.
    s.in.squeeze[0] = 0.0f;
    s.in.squeeze[1] = 0.0f;
    s.in.buttons |= BBXR_BTN_R3;
    s.Run(0.1);
    CHECK(s.Take(1200, 0));
    CHECK(s.Take(1210, 1));
    CHECK(s.t.HeldItem(0) >= 0 && s.t.HeldItem(1) >= 0);
    // The quicksilver tools cost bullets instead of the item: Tiny Tonitrus (6).
    s.in.squeeze[0] = 0.0f;
    s.in.squeeze[1] = 0.0f;
    s.Tick();
    s.Tick();
    CHECK(s.c.DefOf(s.c.FindEntry(2070)).bullet_cost == 6 && !s.c.DefOf(s.c.FindEntry(2070)).consumed);
}

// ---- Crush -------------------------------------------------------------------------------------------

void TestCrush() {
    Sim s;
    CHECK(s.Take(1400, 0)); // Bold Hunter's Mark
    CHECK(s.c.DefOf(s.c.HeldBy(0).entry).gesture == sb::CGesture::Crush);
    s.CloseWheel();
    // A normal hard grip does not crush.
    s.in.squeeze[0] = 0.9f;
    s.Run(2.0);
    CHECK(s.c.Count(1400) == 3 && s.c.HeldBy(0).entry >= 0);
    // A short full squeeze (0.3 s) does not either.
    s.in.squeeze[0] = 1.0f;
    s.Run(0.3);
    s.in.squeeze[0] = 0.9f;
    s.Run(0.2);
    CHECK(s.c.Count(1400) == 3 && s.c.HeldBy(0).entry >= 0);
    // 0.5 s fully squeezed crushes it; the squeeze ramps up a haptic.
    s.peak[0] = 0.0f;
    s.in.squeeze[0] = 1.0f;
    s.Run(0.6);
    CHECK(s.c.HeldBy(0).entry < 0 && s.c.Count(1400) == 2);
    CHECK(s.Logged("used Bold Hunter's Mark (crush)"));
    CHECK(s.peak[0] >= 0.9f);
    // The reusable Hunter's Mark is not used up; the head gesture does nothing to a crush item.
    s.in.squeeze[0] = 0.0f;
    s.in.buttons |= BBXR_BTN_R3;
    CHECK(s.Take(100, 0));
    s.CloseWheel();
    s.CarryTo(0, s.Mouth(), 0.4);
    s.Run(1.0);
    CHECK(s.c.HeldBy(0).entry >= 0);
    s.in.squeeze[0] = 1.0f;
    s.Run(0.7);
    CHECK(s.c.HeldBy(0).entry < 0 && s.c.Count(100) == 1);
    // The Shaman Bone Blade is crushed, and used up.
    s.in.squeeze[0] = 0.0f;
    s.in.buttons |= BBXR_BTN_R3;
    CHECK(s.Take(2090, 0));
    s.in.squeeze[0] = 1.0f;
    s.Run(0.7);
    CHECK(s.c.Count(2090) == 1);
}

// ---- Last items, the log ------------------------------------------------------------------------------

void TestInventoryAndLog() {
    // Using the last one removes the item from the next wheel.
    Sim s("{\"1000\": 1, \"1100\": 1, \"900\": 5}");
    s.OpenWheel(0.1);
    CHECK(s.c.Slots().size() == 2);
    CHECK(s.Take(1000, 0));
    // While held, the only vial's object is gone from the wheel.
    CHECK(s.Slot(1000) < 0 && s.Slot(1100) >= 0);
    s.CloseWheel();
    s.CarryTo(0, s.Mouth(), 0.4);
    s.Run(0.5);
    CHECK(s.c.Count(1000) == 0);
    s.Run(2.0);
    CHECK(!s.c.WheelOpen());
    s.OpenWheel(0.1);
    CHECK(s.Slot(1000) < 0 && s.c.Slots().size() == 1 && s.Slot(1100) >= 0);
    // The jsonl log.
    const std::string path = "consumables_test_log.jsonl";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    {
        Sim l;
        l.cfg.log = true;
        l.cfg.log_file = path;
        CHECK(l.Take(1000, 0));
        l.CloseWheel();
        l.CarryTo(0, l.Mouth(), 0.4);
        l.Run(0.5);
        CHECK(l.Take(1300, 0));
        l.CloseWheel();
        l.CarryTo(0, l.BladePoint(0.3f), 0.3);
        Rub(l, 0, 1, 0.3f, 1.0f, 0.7);
    }
    std::ifstream in(path);
    std::string line, all;
    int lines = 0;
    while (std::getline(in, line)) {
        all += line + "\n";
        ++lines;
    }
    in.close();
    CHECK(lines >= 6);
    CHECK(all.find("\"event\": \"wheel_open\"") != std::string::npos);
    CHECK(all.find("\"event\": \"pick\"") != std::string::npos);
    CHECK(all.find("\"event\": \"use\"") != std::string::npos && all.find("\"hp_after\": 95.000") != std::string::npos);
    CHECK(all.find("\"event\": \"buff\"") != std::string::npos && all.find("\"element\": \"fire\"") != std::string::npos);
    std::filesystem::remove(path, ec);
    // Glyphs for the count labels.
    int n = 0;
    sb::AppendGlyphs("x12", {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, 0.02f, [&](Vec3, Vec3) { ++n; });
    CHECK(n == 2 + 2 + 5);
    n = 0;
    sb::AppendGlyphs("~", {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, 0.02f, [&](Vec3, Vec3) { ++n; });
    CHECK(n == 8);
}

// ---- The toggle wheel -----------------------------------------------------------------------------

void TestToggleWheel() {
    Sim s;
    s.cfg.wheel_mode = "toggle";
    // A tap opens it and it stays open after the release.
    s.in.buttons |= BBXR_BTN_R3;
    s.Run(0.1);
    CHECK(s.c.WheelOpen());
    s.CloseWheel();
    s.Run(5.0);
    CHECK(s.c.WheelOpen() && !s.c.Slots().empty());
    // Pressing again closes it (a long hold too: the press edge counts).
    s.in.buttons |= BBXR_BTN_R3;
    s.Run(0.1);
    CHECK(!s.c.WheelOpen() && s.Logged("wheel closed (button)"));
    s.CloseWheel();
    s.Run(0.3);
    CHECK(!s.c.WheelOpen());
    // Open, take an item: the wheel closes, the item stays in the hand.
    s.in.buttons |= BBXR_BTN_R3;
    s.Run(0.1);
    s.CloseWheel();
    s.Run(1.0);
    CHECK(s.c.WheelOpen());
    const int slot = s.Slot(1000);
    s.in.grip_position[0] = s.GripFor(slot);
    s.Run(0.1);
    s.in.squeeze[0] = 0.9f;
    s.Tick();
    CHECK(s.c.HeldBy(0).entry >= 0 && !s.c.WheelOpen() && s.Logged("wheel closed (item taken)"));
    // With the item used, it opens again by a press.
    s.CarryTo(0, s.Mouth(), 0.5);
    s.Run(0.5);
    CHECK(s.c.HeldBy(0).entry < 0 && s.c.Count(1000) == 19);
    s.in.squeeze[0] = 0.0f;
    s.in.buttons |= BBXR_BTN_R3;
    s.Run(0.1);
    CHECK(s.c.WheelOpen());
    // The mouth diagnostics (log) while an item is held near the head.
    s.CloseWheel();
    s.Run(0.1);
    s.in.buttons |= BBXR_BTN_R3; // close
    s.Run(0.1);
    s.CloseWheel();
    s.Run(0.1);
    CHECK(s.Take(1000, 1));
    s.CarryTo(1, s.Mouth() + Vec3{0.0f, 0.05f, 0.0f}, 0.3);
    s.cfg.mouth_radius_m = 0.02f; // unreachable: only the diagnostics
    s.Run(1.2);
    CHECK(s.Logged("cm from the mouth"));
    // The config default is the toggle wheel.
    sb::ConsumeConfig dc;
    CHECK(dc.wheel_mode == "toggle" && dc.close_on_grab);
    CHECK_NEAR(dc.contact_radius_m, 0.10f, 1e-4f);
    CHECK_NEAR(dc.coverage, 0.50f, 1e-4f);
    CHECK_NEAR(dc.window_s, 2.5f, 1e-4f);
}

// ---- The wheel can always be reopened; the weapons are put away while it is open -------------------

void TestReopenAndStow() {
    Sim s;
    CHECK(!s.c.WeaponStowed(1, s.cfg) && !s.c.WeaponStowed(0, s.cfg));
    s.OpenWheel(0.2);
    CHECK(s.c.WeaponStowed(1, s.cfg) && s.c.WeaponStowed(0, s.cfg)); // both hands free to grab
    // A paper in the left hand: the right weapon stays out (it is rubbed), the left is stowed.
    CHECK(s.Take(1300, 0));
    CHECK(!s.c.WeaponStowed(1, s.cfg) && s.c.WeaponStowed(0, s.cfg));
    s.in.squeeze[0] = 0.1f; // put it back
    s.Tick();
    s.CloseWheel();
    s.Run(2.0);
    CHECK(!s.c.WeaponStowed(1, s.cfg) && !s.c.WeaponStowed(0, s.cfg)); // closed, nothing held
    // The config switch.
    s.cfg.stow_weapons = false;
    s.OpenWheel(0.1);
    CHECK(!s.c.WeaponStowed(1, s.cfg));
    s.cfg.stow_weapons = true;
    s.CloseWheel();
    s.Run(2.0);
    // After a use: reopen, again and again.
    for (int round = 0; round < 3; ++round) {
        s.in.squeeze[0] = 0.0f;
        s.Run(0.1);
        CHECK(s.Take(1000, 0));
        s.CloseWheel();
        s.CarryTo(0, s.Mouth(), 0.4);
        s.Run(0.5);
        CHECK(s.c.HeldBy(0).entry < 0);
        s.in.squeeze[0] = 0.0f;
        s.Run(2.0);
        CHECK(!s.c.WheelOpen() && !s.c.WeaponStowed(1, s.cfg));
        s.OpenWheel(0.2);
        CHECK(s.c.WheelOpen() && !s.c.Slots().empty());
        s.CloseWheel();
        s.Run(2.0);
    }
    // After a drop (let go away from the head), a throw, and a close with an item still held.
    s.in.squeeze[0] = 0.0f;
    CHECK(s.Take(1100, 0));
    s.CloseWheel();
    s.in.squeeze[0] = 0.0f;
    s.Run(1.0);
    s.OpenWheel(0.2);
    CHECK(s.c.WheelOpen() && s.c.HeldBy(0).entry < 0 && s.c.Count(1100) == 4);
    CHECK(s.Take(1230, 1));
    s.CloseWheel();
    for (int i = 0; i < 23; ++i) {
        s.in.grip_position[1] = s.in.grip_position[1] + Vec3{0, 0, -1} * (8.0f * float(kDt));
        s.Tick();
    }
    s.in.squeeze[1] = 0.0f;
    s.Run(2.0);
    s.OpenWheel(0.2);
    CHECK(s.c.WheelOpen() && s.c.Slots().size() > 10 && !s.c.WeaponStowed(0, s.cfg) == false);
    CHECK(s.Take(1000, 0)); // hold an item and let the wheel close: the item stays, the hand's weapon is away
    s.CloseWheel();
    s.Run(3.0);
    CHECK(!s.c.WheelOpen() && s.c.HeldBy(0).entry >= 0 && s.c.WeaponStowed(0, s.cfg) && !s.c.WeaponStowed(1, s.cfg));
    s.in.buttons |= BBXR_BTN_R3; // reopen with the item still in the hand
    s.Run(0.2);
    CHECK(s.c.WheelOpen());
    s.in.squeeze[0] = 0.1f;
    s.CloseWheel();
    s.Run(3.0);
    CHECK(!s.c.WheelOpen() && !s.c.WeaponStowed(0, s.cfg) && !s.c.WeaponStowed(1, s.cfg));
    s.OpenWheel(0.2);
    CHECK(s.c.WheelOpen());
}


// ---- Random mashing: the wheel always opens on the next clean R3 click -----------------------------

void TestMashThenClick() {
    uint32_t rng = 12345u;
    const auto rnd = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return float((rng >> 8) & 0xFFFF) / 65535.0f;
    };
    for (int round = 0; round < 12; ++round) {
        Sim s;
        s.cfg.wheel_mode = "toggle";
        for (int step = 0; step < 400; ++step) {
            // Grips, triggers, the other buttons and R3 clicks at random, hands moving about.
            if (rnd() < 0.15f) s.in.squeeze[0] = rnd() < 0.5f ? 0.0f : 0.95f;
            if (rnd() < 0.15f) s.in.squeeze[1] = rnd() < 0.5f ? 0.0f : 0.95f;
            if (rnd() < 0.15f) s.in.trigger[0] = rnd() < 0.5f ? 0.0f : 1.0f;
            if (rnd() < 0.15f) s.in.trigger[1] = rnd() < 0.5f ? 0.0f : 1.0f;
            if (rnd() < 0.2f) {
                const uint32_t all = BBXR_BTN_CIRCLE | BBXR_BTN_CROSS | BBXR_BTN_SQUARE | BBXR_BTN_TRIANGLE | BBXR_BTN_L3 |
                                     BBXR_BTN_UP | BBXR_BTN_DOWN | BBXR_BTN_LEFT | BBXR_BTN_RIGHT | BBXR_BTN_OPTIONS;
                s.in.buttons = (s.in.buttons & BBXR_BTN_R3) | (uint32_t(rnd() * 1023.0f) & all);
            }
            if (rnd() < 0.1f) s.in.buttons ^= BBXR_BTN_R3;
            if (s.c.WheelOpen() && rnd() < 0.3f && !s.c.Slots().empty()) {
                const int slot = int(rnd() * float(s.c.Slots().size())) % int(s.c.Slots().size());
                s.in.grip_position[int(rnd() * 2.0f) & 1] = s.GripFor(slot);
            } else if (rnd() < 0.2f) {
                s.in.grip_position[int(rnd() * 2.0f) & 1] = Vec3{rnd() - 0.5f, 0.6f + rnd(), -0.6f * rnd()};
            }
            s.Tick();
        }
        // Everything let go; a clean R3 click must toggle the wheel, twice in a row.
        s.in.buttons = 0;
        s.in.squeeze[0] = s.in.squeeze[1] = 0.0f;
        s.in.trigger[0] = s.in.trigger[1] = 0.0f;
        s.Run(0.5);
        for (int click = 0; click < 4; ++click) {
            const bool was = s.c.WheelOpen();
            s.in.buttons |= BBXR_BTN_R3;
            s.Run(0.05);
            s.in.buttons &= ~BBXR_BTN_R3;
            s.Run(0.2);
            CHECK(s.c.WheelOpen() != was);
        }
        CHECK(!s.c.WheelOpen() || !s.c.Slots().empty());
    }
}

// ---- The simple defaults: no throwables on the wheel, weapon buffs at once ----------------------------

void TestSimpleDefaults() {
    sb::ConsumeConfig dc;
    CHECK(!dc.throwables_on_wheel && !dc.buff_rub && !dc.hide_rack);
    Sim s;
    s.cfg.throwables_on_wheel = dc.throwables_on_wheel;
    s.cfg.buff_rub = dc.buff_rub;
    s.cfg.wheel_mode = "toggle";
    s.OpenWheel(0.1);
    CHECK(s.c.WheelOpen() && !s.c.Slots().empty());
    for (const sb::Consumables::Slot& slot : s.c.Slots()) {
        CHECK(s.c.DefOf(slot.entry).group != sb::CGroup::Throwable);
    }
    CHECK(s.Slot(1000) >= 0 && s.Slot(1300) >= 0 && s.Slot(1200) < 0 && s.Slot(1230) < 0);
    // Fire Paper taken with the left hand: the right weapon glows at once, the paper is used up.
    const int before = s.c.Count(1300);
    s.CloseWheel();
    s.in.grip_position[0] = s.GripFor(s.Slot(1300));
    s.Run(0.1);
    s.in.squeeze[0] = 0.9f;
    s.Tick();
    CHECK(s.c.Buff(1).active && s.c.Buff(1).goods == 1300 && !s.c.Buff(0).active);
    CHECK(s.c.Count(1300) == before - 1 && s.c.HeldBy(0).entry < 0 && !s.c.WheelOpen());
    CHECK(s.last.haptic_amplitude[0] > 0.0f && s.last.haptic_amplitude[1] > 0.0f);
    CHECK(s.last.toast_title == "Fire Paper" || s.toast.find("Fire Paper") != std::string::npos);
    s.in.squeeze[0] = 0.0f;
    s.Run(0.5);
    // Picked with the right hand: the other hand's weapon (the left gun) gets it.
    s.in.buttons |= BBXR_BTN_R3;
    s.Run(0.1);
    s.CloseWheel();
    s.Run(0.1);
    CHECK(s.c.WheelOpen());
    s.in.grip_position[1] = s.GripFor(s.Slot(1310));
    s.Run(0.1);
    s.in.squeeze[1] = 0.9f;
    s.Tick();
    CHECK(s.c.Buff(0).active && s.c.Buff(0).goods == 1310);
    s.in.squeeze[1] = 0.0f;
    s.Run(0.5);
    // No weapon in the other hand: the right weapon is the default.
    s.in.weapon_valid[0] = false;
    s.in.buttons |= BBXR_BTN_R3;
    s.Run(0.1);
    s.CloseWheel();
    s.Run(0.1);
    s.in.grip_position[1] = s.GripFor(s.Slot(1320));
    s.Run(0.1);
    s.in.squeeze[1] = 0.9f;
    s.Tick();
    CHECK(s.c.Buff(1).active && s.c.Buff(1).goods == 1320);
    // The rub switch keeps the old gesture.
    Sim r;
    r.cfg.wheel_mode = "toggle";
    r.cfg.buff_rub = true;
    CHECK(r.Take(1300, 0) && r.c.HeldBy(0).entry >= 0 && !r.c.Buff(1).active);
}

// ---- Heals and body buffs: the toggle wheel, the mouth ---------------------------------------------

void TestHealFromWheel() {
    Sim s;
    s.cfg.throwables_on_wheel = false;
    s.cfg.wheel_mode = "toggle";
    s.c.SetHp(40.0f);
    const int vials = s.c.Count(1000);
    CHECK(s.Take(1000, 0));
    CHECK(!s.c.WheelOpen());
    s.CarryTo(0, s.Mouth(), 0.5);
    s.Run(0.6);
    CHECK(s.c.HeldBy(0).entry < 0 && s.c.Count(1000) == vials - 1 && s.c.HpPct() > 40.0f);
    // A body buff (Blue Elixir 1400? any with a duration): bring it to the mouth the same way.
    int buff_goods = 0;
    for (const sb::Consumables::Entry& e : s.c.Entries()) {
        if (e.count > 0 && e.def.group == sb::CGroup::BodyBuff && e.def.gesture == sb::CGesture::Head && e.def.duration_s > 0.0f) {
            buff_goods = e.def.goods;
            break;
        }
    }
    CHECK(buff_goods != 0);
    s.in.squeeze[1] = 0.0f;
    s.in.buttons |= BBXR_BTN_R3;
    s.Run(0.1);
    s.CloseWheel();
    s.Run(0.1);
    CHECK(s.Take(buff_goods, 1));
    s.CarryTo(1, s.Mouth(), 0.5);
    s.Run(0.6);
    CHECK(s.c.HeldBy(1).entry < 0 && !s.c.Statuses().empty());
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path assets = argc > 1 ? argv[1] : "out/vr_assets";
    TestData(assets);
    TestWheel();
    TestHead();
    TestRub();
    TestThrow();
    TestCrush();
    TestReopenAndStow();
    TestToggleWheel();
    TestMashThenClick();
    TestSimpleDefaults();
    TestHealFromWheel();
    TestInventoryAndLog();
    std::printf("consumables tests: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
