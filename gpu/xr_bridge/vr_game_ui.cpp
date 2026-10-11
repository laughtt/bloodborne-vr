// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "vr_game_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "sandbox3d/json.h"
#include "sandbox3d/model.h"

namespace vrui {

using namespace m3;
using Label = sb::SwingReadout::Label;
using Result = sb::SwingReadout::Result;

namespace {

constexpr double kStaleS = 1.0;        // game state older than this: the UI is off
constexpr double kPendingS = 1.5;      // a swing label without its ticket fades after this
constexpr double kTicketMatchS = 1.0;  // a ticket opening this soon after a swing belongs to it
constexpr double kActivityS = 3.0;     // bar "action": shown this long after a swing or a cost
constexpr float kBodyDeadzone = 30.0f * kDeg;
constexpr uint32_t kBareFists = 1000000;   // the game's empty hand slot (EquipParamWeapon row)
constexpr uint32_t kFirstWeapon = 1000000; // lower weapon-category ids are not weapons one holds
constexpr double kHeldSignalS = 2.0;       // BBXR_UI_HELD is re-sent this often while an item is gripped
constexpr double kPendingActionS = 8.0;    // an action the host never finished: given up

std::string Fmt(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

uint32_t PadBit(sb::UiInputId id) {
    using I = sb::UiInputId;
    switch (id) {
    case I::A: return BBXR_BTN_CIRCLE;
    case I::B: return BBXR_BTN_CROSS;
    case I::X: return BBXR_BTN_SQUARE;
    case I::Y: return BBXR_BTN_TRIANGLE;
    case I::DpadUp: return BBXR_BTN_UP;
    case I::DpadDown: return BBXR_BTN_DOWN;
    case I::DpadLeft: return BBXR_BTN_LEFT;
    case I::DpadRight: return BBXR_BTN_RIGHT;
    case I::Options: return BBXR_BTN_OPTIONS;
    case I::Touchpad: return BBXR_BTN_TOUCHPAD;
    default: return 0; // grips and triggers are analog; L3/R3 belong to the filter's own rules
    }
}

bool ModifierHeld(sb::UiInputId id, const FrameInput& in) {
    using I = sb::UiInputId;
    switch (id) {
    case I::LeftGrip: return in.squeeze[0] > 0.6f;
    case I::RightGrip: return in.squeeze[1] > 0.6f;
    case I::LeftTrigger: return in.trigger[0] > 0.6f;
    case I::RightTrigger: return in.trigger[1] > 0.6f;
    default: return PadBit(id) && (in.pad.buttons & PadBit(id));
    }
}

bool HasMenuOn(const sb::UiLayout& layout, sb::UiInputId key) {
    for (const std::string& name : layout.game.active) {
        if (const sb::UiMenuSpec* m = layout.FindGameMenu(name)) {
            if (m->open.key == key) {
                return true;
            }
        }
    }
    return false;
}

/// The "game" object of the default layout, as text (for a file written before it existed).
std::string DefaultGameObject() {
    const std::string text = sb::DefaultLayoutText();
    const size_t at = text.find("\"game\": {");
    if (at == std::string::npos) {
        return std::string();
    }
    int depth = 0;
    for (size_t i = text.find('{', at); i < text.size(); ++i) {
        depth += text[i] == '{' ? 1 : text[i] == '}' ? -1 : 0;
        if (depth == 0) {
            return text.substr(at, i + 1 - at);
        }
    }
    return std::string();
}

/// The "game" object's version in a layout text (0: none or no version).
int GameVersion(const std::string& text, size_t* begin, size_t* end) {
    const size_t at = text.find("\"game\"");
    if (at == std::string::npos) {
        return -1;
    }
    const size_t open = text.find('{', at);
    int depth = 0;
    size_t close = std::string::npos;
    for (size_t i = open; open != std::string::npos && i < text.size(); ++i) {
        depth += text[i] == '{' ? 1 : text[i] == '}' ? -1 : 0;
        if (depth == 0) {
            close = i;
            break;
        }
    }
    if (close == std::string::npos) {
        return -1;
    }
    *begin = at;
    *end = close + 1;
    const std::string object = text.substr(open, close + 1 - open);
    std::string error;
    const json::Value v = json::Parse(object, &error);
    return error.empty() ? v["version"].Int(0) : 0;
}

constexpr int kGameVersion = 2; // the dashboard (one button, one action)

/// Adds the default "game" object to a layout file that has none (written by an older sandbox),
/// and replaces one older than kGameVersion (the user's tuning of it is kept in a .bak copy).
void AddGameObject(const std::filesystem::path& path, std::string& log) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return; // UiLayoutFile writes the full defaults
    }
    bool ok = false;
    std::string text = sb::ReadText(path, &ok);
    if (!ok) {
        return;
    }
    size_t begin = 0, stop = 0;
    const int version = GameVersion(text, &begin, &stop);
    if (version >= kGameVersion) {
        return;
    }
    if (version >= 0) {
        const std::string game = DefaultGameObject();
        if (game.empty()) {
            return;
        }
        std::filesystem::copy_file(path, path.string() + ".bak",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        text = text.substr(0, begin) + game + text.substr(stop);
        std::ofstream file(path, std::ios::binary);
        file << text;
        log += "replaced the game's VR UI section (\"game\", version " + std::to_string(version) +
               " -> " + std::to_string(kGameVersion) + ": the dashboard) in " + path.string() +
               " (the old file: .bak)\n";
        return;
    }
    const size_t end = text.find_last_of('}');
    const std::string game = DefaultGameObject();
    if (end == std::string::npos || game.empty()) {
        return;
    }
    size_t last = end;
    while (last > 0 && std::isspace(static_cast<unsigned char>(text[last - 1]))) {
        --last;
    }
    text = text.substr(0, last) + ",\n  " + game + "\n" + text.substr(end);
    std::ofstream file(path, std::ios::binary);
    file << text;
    log += "added the game's VR UI section (\"game\") to " + path.string() + "\n";
}

} // namespace

std::string ItemKey(uint32_t id) {
    if (id == BBXR_ITEM_NONE) {
        return std::string();
    }
    const uint32_t category = id & 0xF0000000u;
    const uint32_t row = id & 0x0FFFFFFFu;
    if (category == BBXR_ITEM_GOODS) {
        return "goods:" + std::to_string(row);
    }
    if (category == 0) {
        return "weapon:" + std::to_string(row);
    }
    return std::string();
}

uint32_t ItemId(const std::string& key) {
    if (key.rfind("goods:", 0) == 0) {
        return BBXR_ITEM_GOODS | (uint32_t(std::strtoul(key.c_str() + 6, nullptr, 10)) & 0x0FFFFFFFu);
    }
    if (key.rfind("weapon:", 0) == 0) {
        return uint32_t(std::strtoul(key.c_str() + 7, nullptr, 10)) & 0x0FFFFFFFu;
    }
    return BBXR_ITEM_NONE;
}

GunRayLook GunRayFor(const sb::UiGameLayout& game, bool in_world, bool aim_valid,
                     double since_pull_s) {
    GunRayLook look;
    look.visible = game.debug_gun_ray && in_world && aim_valid;
    look.flash = look.visible && since_pull_s >= 0.0 && since_pull_s < kGunRayFlashS;
    look.width_m = look.flash ? 0.012f : 0.004f;
    look.alpha = look.visible ? (look.flash ? 1.0f : 0.55f) : 0.0f;
    return look;
}

void AimAngles(Vec3 dir, const Quat* head, float* yaw, float* pitch) {
    auto angles = [](Vec3 d, float* y, float* p) {
        *y = std::atan2(-d.x, -d.z);
        *p = std::asin(std::clamp(d.y / std::max(Length(d), 1e-6f), -1.0f, 1.0f));
    };
    float y, p;
    angles(dir, &y, &p);
    if (head) {
        float hy, hp;
        angles(Rotate(*head, {0, 0, -1}), &hy, &hp);
        y -= hy;
        p -= hp;
        while (y > kPi) y -= 2.0f * kPi;
        while (y < -kPi) y += 2.0f * kPi;
    }
    *yaw = y;
    *pitch = p;
}

GameUi::GameUi() = default;

GameUi::~GameUi() {
    Destroy();
}

bool GameUi::Load(const std::filesystem::path& assets_dir, const sb::Renderer::Device* device,
                  std::string& log) {
    assets = assets_dir;
    if (const char* e = std::getenv("BB_XR_CUSTOM_HANDS"); e && *e) {
        custom_hands_env = std::atoi(e) != 0 ? 1 : 0;
    }
    AddGameObject(assets / "ui_layouts.json", log);
    const bool gpu_ok = menus.LoadGame(assets, device, log);
    hitboxes.Load(assets, log);
    if (!hud_font.Load(assets / "ui", log)) {
        log += "game UI: no font in " + (assets / "ui").string() +
               " (scripts/vr_assets/ui_extract.py): the stamina bar shows, no labels\n";
    }
    if (device && gpu_ok) {
        std::string error;
        if (hud_gpu.Create(*device, error)) {
            hud_gpu.SetAccumulateAlpha(true);
            hud_gpu_ready = true;
            if (hud_font.Ready() &&
                !hud_gpu.UploadFont(hud_font.width, hud_font.height, hud_font.pixels)) {
                log += "game UI: HUD font upload failed: " + hud_gpu.LastError() + "\n";
            }
        } else {
            log += "game UI: HUD: " + error + "\n";
        }
    }
    hud_font.pixels = {};
    loaded = !device || (gpu_ok && hud_gpu_ready);
    return loaded;
}

void GameUi::Destroy() {
    if (hud_gpu_ready) {
        hud_gpu.Destroy();
        hud_gpu_ready = false;
    }
    std::string log;
    if (loaded) {
        menus.Destroy(nullptr);
        loaded = false;
    }
}

void GameUi::SetGameState(const BbXrGameState& s, double t) {
    std::scoped_lock lock{mutex};
    const size_t n = std::min<size_t>(s.size ? s.size : sizeof(s), sizeof(state));
    state = {};
    std::memcpy(&state, &s, n);
    state.size = uint32_t(sizeof(state));
    state.item_count = std::min<uint32_t>(state.item_count, BBXR_GAME_MAX_ITEMS);
    state.quick_count = std::min<uint32_t>(state.quick_count, BBXR_GAME_MAX_QUICK);
    ++state_seq;
    state_at = t;
}

void GameUi::PushEvent(const BbXrGameEvent& e, double) {
    std::scoped_lock lock{mutex};
    if (events.size() < 256) {
        events.push_back(e);
    }
}

uint32_t GameUi::TakeActions(BbXrUiAction* out, uint32_t capacity) {
    std::scoped_lock lock{mutex};
    const uint32_t n = std::min<uint32_t>(capacity, uint32_t(actions.size()));
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = actions[i];
    }
    actions.erase(actions.begin(), actions.begin() + n);
    return n;
}

void GameUi::ApplyState(const BbXrGameState& s, std::string&) {
    sb::UiLiveData live;
    if (s.flags & BBXR_GAME_ITEMS) {
        sb::UiLiveList& owned = live.lists["owned_weapons"];
        for (uint32_t i = 0; i < s.item_count; ++i) {
            const std::string key = ItemKey(s.items[i].id);
            if (key.empty()) {
                continue;
            }
            // Goods have counts and are used; weapons are equipped (the dashboard's weapons tab).
            if ((s.items[i].id & 0xF0000000u) == BBXR_ITEM_GOODS) {
                live.inventory.push_back({key, s.items[i].count});
            } else if (s.items[i].id >= kFirstWeapon && s.items[i].id != kBareFists &&
                       std::find(owned.items.begin(), owned.items.end(), key) == owned.items.end()) {
                owned.items.push_back(key);
            }
        }
    }
    if (s.flags & BBXR_GAME_QUICK) {
        sb::UiLiveList& quick = live.lists["quick"];
        for (uint32_t i = 0; i < s.quick_count; ++i) {
            quick.items.push_back(ItemKey(s.quick[i]));
        }
        if (s.quick_active < s.quick_count) {
            live.selected["quick"] = int(s.quick_active);
        }
    }
    // Equipping and quick-slot assignment go through the game's own command, run by the "VR UI
    // Equip (HUD input)" hook (src/runtime_vr_ui.c): without it they are shown disabled.
    if (!(s.flags & BBXR_GAME_CAN_EQUIP)) {
        const std::string hook =
            "the game's own equip command runs in the in-game step hook: select the patch \"VR FP "
            "Camera Heading (in-game step)\" in BB_HOOKS; or use the game's menu (Options)";
        live.disabled["assign_quick"] = "putting items into quick slots needs the equip hook: " + hook;
        live.disabled["equip_inventory"] = "equipping from the inventory needs the equip hook: " + hook;
        live.disabled["clear_quick"] = "emptying a quick slot needs the equip hook: " + hook;
    }
    world = sb::UiWorld{};
    if (s.flags & BBXR_GAME_WEAPONS) {
        // Bare Fists (1000000) is the game's placeholder for an empty hand slot.
        const auto weapon_key = [](uint32_t id) {
            return id == kBareFists ? std::string() : ItemKey(id);
        };
        sb::UiLiveList& weapons = live.lists["weapons"];
        for (uint32_t i = 0; i < 4; ++i) {
            weapons.items.push_back(weapon_key(s.weapons[i]));
        }
        world.weapon[1] = weapon_key(s.weapons[s.weapon_active[0] & 1]);
        world.weapon[0] = weapon_key(s.weapons[2 + (s.weapon_active[1] & 1)]);
        live.selected["weapons_right"] = int(s.weapon_active[0] & 1);
        live.selected["weapons_left"] = 2 + int(s.weapon_active[1] & 1);
    }
    live.room = room_line.Poll(now);
    menus.SetLiveData(live);
}

void GameUi::AddTired(std::vector<Haptic>& haptics) {
    const sb::UiLayout& layout = menus.Layout();
    haptics.push_back({tired_hand, layout.game.tired.amplitude, layout.game.tired.seconds});
    tired_second = now + 0.1;
}

void GameUi::Swings(const FrameInput& in, const BbXrGameState& s, std::vector<Haptic>& haptics) {
    const sb::UiGameLayout& g = menus.Layout().game;
    // The right hand's recent speeds: a swing's peak.
    speed_window[speed_next] = in.pad.hand_speed[1];
    speed_times[speed_next] = in.time;
    speed_next = (speed_next + 1) % 24;
    const uint32_t pressed = in.pad.buttons & ~prev_buttons;
    const bool gesture = in.ms_since_swing < 200.0;
    const bool r1 = (pressed & BBXR_BTN_R1) != 0, r2 = (pressed & BBXR_BTN_R2) != 0;
    const bool item_in_hand = held_item.Active() || in.time < held_item.BlockUntil();
    if ((r1 || r2) && gesture && !item_in_hand) {
        float peak = 0.0f;
        for (int i = 0; i < 24; ++i) {
            if (in.time - speed_times[i] < 0.4) {
                peak = std::max(peak, float(speed_window[i]));
            }
        }
        readout.swing_no++;
        readout.label_t = in.time;
        readout.label_speed = peak;
        readout.label_cost = 0.0f;
        readout.label_paid = 0.0f;
        readout.latency_ms = -1.0f;
        readout.detail.clear();
        last_activity = in.time;
        if (filter.Engaged()) {
            readout.label = Label::Blocked;
            readout.result = Result::None;
        } else if ((s.flags & BBXR_GAME_STAMINA) && s.stamina <= 0) {
            // The game refuses an attack at 0 or below (doc 20 3.2): say so.
            readout.label = Label::Tired;
            readout.result = Result::None;
            readout.refused_at = in.time;
            AddTired(haptics);
        } else {
            readout.label = r2 ? Label::Heavy : Label::Light;
            readout.result = Result::Pending;
            const sb::UiPulse& tick = r2 ? g.tick_heavy : g.tick_light;
            haptics.push_back({1, tick.amplitude, tick.seconds});
        }
    }
    if (tired_second >= 0.0 && in.time >= tired_second) {
        haptics.push_back({tired_hand, g.tired.amplitude, g.tired.seconds});
        tired_second = -1.0;
    }
    // A swing whose ticket never came (the stamina hooks are off, or the attack had no cost).
    if (readout.result == Result::Pending && readout.label_cost <= 0.0f &&
        in.time - readout.label_t > kPendingS) {
        readout.result = Result::None;
        readout.detail = "no stamina ticket: BB_VR_STAMINA=defer and the stamina hooks";
    }
}

bool GameUi::PulseWeapon(int hand, int contact, int paid, double now, std::vector<Haptic>& haptics,
                         std::string& log) {
    if (now - last_contact_pulse[hand] < 0.08) {
        return false; // one pulse per contact burst
    }
    last_contact_pulse[hand] = now;
    const held::Pulse p = held::CollisionPulse(menus.Layout().game.collision_haptics, hand, contact, paid);
    haptics.push_back({p.hand, p.amplitude, p.seconds});
    log += Fmt("game UI: weapon contact haptic: %s, hand %d -> amplitude %.2f for %.0f ms\n",
               contact == 1 ? "wall" : contact == 2 ? "object" : "character", hand, double(p.amplitude),
               double(p.seconds) * 1000.0);
    return true;
}

void GameUi::Events(std::vector<Haptic>& haptics, std::string& log) {
    std::deque<BbXrGameEvent> list;
    {
        std::scoped_lock lock{mutex};
        list.swap(events);
    }
    const sb::UiGameLayout& g = menus.Layout().game;
    for (const BbXrGameEvent& e : list) {
        hitboxes.OnEvent(e, now);
        switch (e.kind) {
        case BBXR_EV_SWING_OPEN: {
            tickets.push_back({e.c, e.a, now});
            if (tickets.size() > 8) {
                tickets.pop_front();
            }
            const bool ours = readout.result == Result::Pending && readout.label_cost <= 0.0f &&
                              now - readout.label_t < kTicketMatchS;
            if (!ours) {
                // An attack the gestures did not announce (a trigger R1, a combo follow-up).
                readout.label = e.a >= 30 ? Label::Heavy : Label::Light;
                readout.label_t = now;
                readout.label_speed = 0.0f;
                readout.latency_ms = -1.0f;
                readout.detail = "attack";
                readout.swing_no++;
            }
            readout.result = Result::Pending;
            readout.label_cost = float(e.a);
            last_activity = now;
            break;
        }
        case BBXR_EV_SWING_HIT:
        case BBXR_EV_SWING_MISS: {
            int32_t cost = e.kind == BBXR_EV_SWING_MISS ? e.a : 0;
            for (auto it = tickets.begin(); it != tickets.end(); ++it) {
                if (it->id == e.c) {
                    cost = it->cost;
                    tickets.erase(it);
                    break;
                }
            }
            const bool latest = readout.label_cost > 0.0f && readout.result == Result::Pending;
            if (e.kind == BBXR_EV_SWING_HIT) {
                readout.charged_at = now;
                readout.charged_from = readout.stamina;
                readout.charged_to = readout.stamina - float(e.a);
                if (latest) {
                    readout.result = e.b == 1 ? Result::Clang : Result::Hit;
                    readout.label_paid = float(e.a);
                    readout.label_t = now;
                }
                if (g.collision_haptics.on) {
                    // The weapon hand buzzes when the game's weapon collides, harder for a costlier
                    // attack; a wall or object bounce has its own pulse.
                    const held::Pulse p = held::CollisionPulse(g.collision_haptics, 1, e.b, e.a);
                    haptics.push_back({p.hand, p.amplitude, p.seconds});
                    last_contact_pulse[1] = now; // the unpaid-contact path stays quiet for 80 ms
                    log += Fmt("game UI: weapon collision haptic: %s, paid %d -> amplitude %.2f for %.0f ms\n",
                               e.b == 1 ? "wall" : e.b == 2 ? "object" : "character", int(e.a),
                               double(p.amplitude), double(p.seconds) * 1000.0);
                } else {
                    const bool heavy = readout.label == Label::Heavy || readout.label == Label::Charged;
                    const sb::UiPulse& pulse = heavy ? g.hit_heavy : g.hit_light;
                    haptics.push_back({1, pulse.amplitude, pulse.seconds});
                }
                log += Fmt("game UI: swing paid %d on %s (ticket %d)\n", int(e.a),
                           e.b == 1 ? "a wall" : e.b == 2 ? "an object" : "a character",
                           int(e.c));
            } else if (latest) {
                readout.result = Result::Miss;
                readout.label_t = now;
            }
            (void)cost;
            last_activity = now;
            break;
        }
        case BBXR_EV_WEAPON_CONTACT: {
            // A contact the ticket path did not announce: only the weapon hand's pulse, never the readout.
            if (g.collision_haptics.on && e.b >= 0 && e.b <= 2) {
                PulseWeapon(e.c == 0 ? 0 : 1, e.b, e.a, now, haptics, log);
            }
            break;
        }
        case BBXR_EV_ACTION_DONE: {
            const std::string what = pending.empty() ? std::string("the action") : pending;
            pending.clear();
            if (e.b == 0) {
                static const char* const kWhy[] = {
                    "", "the item is no longer there",
                    "the game did not answer the button (a menu or an animation in the way)",
                    "not possible now (no player, or no button does it)",
                    "no game-thread hook ran: select the patch \"VR FP Camera Heading (in-game "
                    "step)\" in BB_HOOKS",
                    "the game's equip command did not put it there",
                    "a game menu is open: close it first"};
                Refuse("Not done: " + what, kWhy[e.c >= 1 && e.c <= 6 ? e.c : 3], log);
                haptics.push_back({1, menus.Layout().style.haptic_empty.amplitude,
                                   menus.Layout().style.haptic_empty.seconds});
            } else {
                log += "game UI: done: " + what + "\n";
            }
            break;
        }
        default:
            break;
        }
    }
    if (!pending.empty() && now - pending_at > kPendingActionS) {
        Refuse("Not done: " + pending, "no answer from the game side in 8 s", log);
        pending.clear();
    }
    float held = 0.0f;
    for (auto it = tickets.begin(); it != tickets.end();) {
        if (now - it->t > 3.0) { // the host never closed it (a lost event)
            it = tickets.erase(it);
            continue;
        }
        held += float(it->cost);
        ++it;
    }
    readout.pending = held;
}

void GameUi::Refuse(const std::string& what, const std::string& why, std::string& log) {
    menus.ShowMessage(what, why, 5.0);
    log += "game UI: " + what + ": " + why + "\n";
}

uint32_t PressBit(const std::string& name) {
    static const struct {
        const char* name;
        uint32_t bit;
    } kButtons[] = {{"options", BBXR_BTN_OPTIONS},   {"touchpad", BBXR_BTN_TOUCHPAD},
                    {"dpad_up", BBXR_BTN_UP},        {"dpad_down", BBXR_BTN_DOWN},
                    {"dpad_left", BBXR_BTN_LEFT},    {"dpad_right", BBXR_BTN_RIGHT},
                    {"square", BBXR_BTN_SQUARE},     {"triangle", BBXR_BTN_TRIANGLE},
                    {"circle", BBXR_BTN_CIRCLE},     {"cross", BBXR_BTN_CROSS},
                    {"l3", BBXR_BTN_L3},             {"r3", BBXR_BTN_R3}};
    for (const auto& b : kButtons) {
        if (name == b.name) {
            return b.bit;
        }
    }
    return 0;
}

void GameUi::Act(const sb::UiAction& a, std::string& log) {
    BbXrUiAction out{};
    int start_held_slot = -1; // assign_use of a gesture item: held after the assignment
    const uint32_t id = ItemId(a.item);
    if (a.action == "recenter") {
        if (recenter) {
            recenter();
        }
        log += "game UI: recenter (dashboard)\n";
        return;
    }
    if (a.action.rfind("press:", 0) == 0) {
        const uint32_t bit = PressBit(a.action.substr(6));
        if (!bit) {
            log += "game UI: unknown button in \"" + a.action + "\"\n";
            return;
        }
        out = {BBXR_UI_PRESS, int32_t(bit), 0, 0};
    } else if (a.action == "select") {
        if (a.index < 0) {
            return;
        }
        out = {BBXR_UI_SELECT_QUICK, a.index, int32_t(id), 0};
    } else if (a.action == "equip_item") {
        // Into the hand's slot held now: 0 left 1, 1 right 1, 2 left 2, 3 right 2.
        const int side = a.side == 0 ? 0 : 1;
        const uint32_t held = side ? current.weapon_active[0] & 1 : current.weapon_active[1] & 1;
        const int slot = side ? (held ? 3 : 1) : (held ? 2 : 0);
        if (id == BBXR_ITEM_NONE || !(current.flags & BBXR_GAME_WEAPONS)) {
            Refuse("Cannot equip " + a.item, "no weapon slots read from the game", log);
            return;
        }
        out = {BBXR_UI_EQUIP_ITEM, slot, int32_t(id), 0};
    } else if (a.action == "assign_quick" || a.action == "assign_use") {
        int slot = -1;
        for (uint32_t i = 0; i < current.quick_count && slot < 0; ++i) {
            if (current.quick[i] == BBXR_ITEM_NONE) {
                slot = int(i);
            }
        }
        if (slot < 0 && current.quick_count > 0) {
            // Every slot is full: the last one is replaced (the one you reach last with the d-pad,
            // so the others keep their places) and the message says which. To choose, take one out
            // first (Remove from wheel).
            slot = int(current.quick_count) - 1;
            menus.ShowMessage("Quick slots full",
                              "Replaced slot " + std::to_string(slot + 1) +
                                  "; use Remove from wheel first to choose which one goes",
                              5.0);
            log += Fmt("game UI: quick slots full: replacing slot %d\n", slot + 1);
        }
        if (slot < 0) {
            Refuse("Cannot add " + a.item, "no quick slots read from the game", log);
            return;
        }
        out = {BBXR_UI_ASSIGN_QUICK, slot, int32_t(id), a.action == "assign_use" ? 1 : 0};
        if (a.action == "assign_use" && menus.Layout().game.held_items && held::Classify(id).kind != held::Kind::None &&
            !(current.flags & BBXR_GAME_MENU_OPEN)) {
            // A gesture item (paper, vial, throwable, bell...): into the slot, then into the hand; the gesture
            // uses it. Not used at once.
            if (held_item.Active()) {
                log += "game UI: held item: already holding goods " + std::to_string(held_item.Info().goods) + "; " +
                       a.item + " ignored\n";
                return;
            }
            out.b = 0;
            start_held_slot = slot;
        }
    } else if (a.action == "assign_slot") {
        // Drag and drop: into the quick slot the card was dropped on (an occupied slot is replaced; the old item
        // stays in the inventory, it only leaves the slot).
        const int slot = a.index;
        if (slot < 0 || uint32_t(slot) >= current.quick_count || id == BBXR_ITEM_NONE) {
            Refuse("Cannot add " + a.item, "that quick slot is not available", log);
            return;
        }
        if (current.quick[slot] == id) {
            return;
        }
        if (current.quick[slot] != BBXR_ITEM_NONE) {
            log += Fmt("game UI: drag and drop: quick slot %d: %s replaces item 0x%x\n", slot + 1, a.item.c_str(),
                       unsigned(current.quick[slot]));
        }
        out = {BBXR_UI_ASSIGN_QUICK, slot, int32_t(id), 0};
    } else if (a.action == "clear_quick") {
        const int slot = a.source == "quick" ? a.index : -1;
        if (slot < 0 || uint32_t(slot) >= current.quick_count ||
            current.quick[slot] == BBXR_ITEM_NONE) {
            Refuse("Cannot remove " + a.item, "it is in no quick slot", log);
            return;
        }
        out = {BBXR_UI_CLEAR_QUICK, slot, int32_t(current.quick[slot]), 0};
    } else if (a.action == "equip") {
        int slot = a.source == "weapons" ? a.index : -1;
        for (int i = 0; i < 4 && slot < 0; ++i) {
            if (current.weapons[i] == id) {
                slot = i;
            }
        }
        if (slot < 0 || !(current.flags & BBXR_GAME_WEAPONS)) {
            Refuse("Cannot equip " + a.item,
                   "it is in no hand slot, and putting a weapon into a slot needs the game's "
                   "equipment menu (Options)",
                   log);
            return;
        }
        out = {BBXR_UI_EQUIP, slot, int32_t(id), 0};
    } else if (a.action == "use") {
        int slot = a.source == "quick" ? a.index : -1;
        for (uint32_t i = 0; i < current.quick_count && slot < 0; ++i) {
            if (current.quick[i] == id) {
                slot = int(i);
            }
        }
        out = slot >= 0 ? BbXrUiAction{BBXR_UI_USE_QUICK, slot, int32_t(id), 0}
                        : BbXrUiAction{BBXR_UI_USE_ITEM, int32_t(id), 0, 0};
        if (slot >= 0 && menus.Layout().game.held_items && StartHeld(a, slot, id, log)) {
            return; // in the hand: a gesture uses it
        }
    } else {
        log += "game UI: " + a.action + ": nothing to do\n";
        return;
    }
    log += Fmt("game UI: %s %s -> action %u (%d, %d)\n", a.action.c_str(), a.item.c_str(),
               out.kind, int(out.a), int(out.b));
    if (out.kind == BBXR_UI_USE_ITEM) {
        Refuse("Cannot use it from here",
               "only items in a quick slot can be used from the dashboard (the game's "
               "use-from-menu path is not hooked yet)",
               log);
        return;
    }
    if (!pending.empty() && now - pending_at < kPendingActionS) {
        Refuse("Busy", "still doing: " + pending, log);
        return;
    }
    pending = a.action + (a.item.empty() ? std::string() : " " + a.item);
    pending_at = now;
    {
        std::scoped_lock lock{mutex};
        if (actions.size() < 32) {
            actions.push_back(out);
        }
    }
    if (start_held_slot >= 0 && !StartHeld(a, start_held_slot, id, log)) {
        // Could not be held after all (none left): the old behaviour, used at once.
        std::scoped_lock lock{mutex};
        if (!actions.empty() && actions.back().kind == BBXR_UI_ASSIGN_QUICK) {
            actions.back().b = 1;
        }
    }
}

bool GameUi::StartHeld(const sb::UiAction& a, int slot, uint32_t id, std::string& log) {
    if (held_item.Active()) {
        log += "game UI: held item: already holding goods " + std::to_string(held_item.Info().goods) +
               "; " + a.item + " ignored\n";
        return true; // swallowed: a second grab must not use the item at once
    }
    if (current.flags & BBXR_GAME_MENU_OPEN) {
        return false;
    }
    if (current.flags & BBXR_GAME_ITEMS) { // none left: nothing to hold (the old path says so)
        for (uint32_t i = 0; i < current.item_count; ++i) {
            if (current.items[i].id == id && current.items[i].count <= 0) {
                return false;
            }
        }
    }
    // From a body slot the grabbing hand holds a drink; from the wheel the right hand does.
    const bool from_body = a.menu.find("body") != std::string::npos || a.source.empty();
    if (!held_item.Pick(id, slot, now, from_body ? a.hand : -1)) {
        return false; // bells, marks, the bone blade...: used at once as before
    }
    const held::ItemInfo& info = held_item.Info();
    const char* gesture = info.kind == held::Kind::Mouth   ? (info.bell ? "bring it to the head" : "bring it to the mouth")
                          : info.kind == held::Kind::Blade ? "touch it to the blade"
                          : info.kind == held::Kind::Crush ? "squeeze the grip hard to crush it"
                          : held::HeldItem::ThrowMode(menus.Layout().game.held) == "aim"
                              ? "hold the grip to aim (arc and ring), let go to throw"
                              : "throw it";
    log += Fmt("game UI: held item: %s (goods %d, quick slot %d) in the %s hand (the selection is the grip): %s%s; a trigger press or opening a menu puts it back\n",
               a.item.c_str(), info.goods, slot, held_item.Hand() ? "right" : "left", gesture,
               info.kind == held::Kind::Throw
                   ? (info.aimed ? " (the game's bullet is aimed from the hand)"
                                 : " (this item's bullet is not aimed: the game's own direction)")
                   : "");
    menus.CloseAll(); // the wheel/dashboard that took the pick
    held_menu_ignore_until = now + 0.5;
    filter.SetHoldMask(held_item.BlockMask(now, menus.Layout().game.held));
    return true;
}

void GameUi::StepHeld(const FrameInput& in, std::vector<Haptic>& haptics, std::string& log) {
    const sb::UiGameLayout& g = menus.Layout().game;
    if (!held_item.Active()) {
        filter.SetHoldMask(held_item.BlockMask(now, g.held));
        SyncHeldSignal();
        return;
    }
    held::Frame f;
    f.time = in.time;
    f.head_valid = in.head.valid;
    f.head_pos = in.head.position;
    f.head_rot = in.head.rotation;
    for (int h = 0; h < 2; ++h) {
        f.hand_valid[h] = in.grip[h].valid;
        f.grip_pos[h] = in.grip[h].position;
        f.grip_rot[h] = in.grip[h].rotation;
        f.squeeze[h] = in.squeeze[h];
        f.trigger[h] = in.trigger[h];
    }
    if (in.grip[held_item.Hand()].valid) {
        held_grip_rot = in.grip[held_item.Hand()].rotation; // the mesh follows the grip's orientation
    }
    if (held_item.Info().kind == held::Kind::Blade) {
        const int side = held_item.Info().target_side;
        held::Blade& b = f.blade[side];
        b.valid = hitboxes.BladeSegment(current, side, &b.base, &b.tip, &b.approx, hitbox_log);
    }
    // A menu that is OPEN (the wheel, the grid, the dashboard) cancels the item; hovering a body
    // slot or the wrist does not (the hand goes by the chest on its way to the mouth).
    bool open = false;
    for (const std::string& name : g.active) {
        const sb::UiMenuSpec* m = menus.Layout().FindGameMenu(name);
        if (m && (m->type == "radial" || m->type == "grid" || m->type == "dashboard") && menus.IsOpen(name)) {
            open = true;
        }
    }
    f.blocked = (open && in.time >= held_menu_ignore_until) || (current.flags & BBXR_GAME_MENU_OPEN) != 0;
    const bool was_aiming = held_item.Aiming();
    const held::Result r = held_item.Step(f, g.held);
    if (!was_aiming && held_item.Aiming()) {
        log += Fmt("game UI: held item: goods %d aiming (hold grip, release to throw)\n", held_item.Info().goods);
    }
    for (const held::Pulse& p : r.pulses) {
        haptics.push_back({p.hand, p.amplitude, p.seconds});
    }
    if (r.kind == held::Result::Cancel) {
        log += Fmt("game UI: held item (goods %d) put back: %s\n", r.item.goods, r.why.c_str());
    } else if (r.kind == held::Result::Use) {
        FireHeld(r, in, log);
    }
    filter.SetHoldMask(held_item.BlockMask(now, g.held));
    SyncHeldSignal();
}

void GameUi::SyncHeldSignal() {
    held_marker_hand.store(held_item.Active() ? held_item.Hand() : -1, std::memory_order_relaxed);
    const bool want = held_item.Gripped() && menus.Layout().game.held.hide_weapon;
    if (!want && held_sig_id == 0) {
        return; // nothing to say (the common case: no lock)
    }
    if (want && held_sig_id == int32_t(held_item.Id()) && now - held_sig_at < kHeldSignalS) {
        return;
    }
    std::scoped_lock lock{mutex};
    SyncHeldSignalLocked();
}

void GameUi::SyncHeldSignalLocked() {
    const bool want = held_item.Gripped() && menus.Layout().game.held.hide_weapon;
    if (want) {
        if (held_sig_id != int32_t(held_item.Id()) || now - held_sig_at >= kHeldSignalS) {
            held_sig_id = int32_t(held_item.Id());
            held_sig_hand = held_item.Hand();
            held_sig_at = now;
            if (actions.size() < 32) {
                actions.push_back({BBXR_UI_HELD, held_sig_hand, held_sig_id, 0});
            }
        }
    } else if (held_sig_id != 0) {
        if (actions.size() < 32) {
            actions.push_back({BBXR_UI_HELD, -1, held_sig_id, 0});
        }
        held_sig_id = 0;
    }
}

void GameUi::FireHeld(const held::Result& r, const FrameInput& in, std::string& log) {
    const uint32_t id = BBXR_ITEM_GOODS | uint32_t(r.item.goods);
    int slot = held_item.QuickSlot();
    if (slot < 0 || uint32_t(slot) >= current.quick_count || current.quick[slot] != id) {
        slot = -1;
        for (uint32_t i = 0; i < current.quick_count && slot < 0; ++i) {
            if (current.quick[i] == id) {
                slot = int(i);
            }
        }
    }
    if (slot < 0) {
        Refuse("Not used", "the item is no longer in a quick slot", log);
        return;
    }
    held_fired_at = in.time;
    std::scoped_lock lock{mutex};
    SyncHeldSignalLocked(); // the item left the hand: the weapon is back before the use goes out
    const bool aimed_throw = r.thrown && r.has_marker;
    if (r.thrown && r.item.aimed && in.head.valid) {
        // Where and along what the hand let go, in the head's frame (the host turns the game's bullet).
        const Quat inv = Conjugate(in.head.rotation);
        const Vec3 o = Rotate(inv, r.throw_origin - in.head.position) * 1000.0f;
        const Vec3 d = Rotate(inv, r.throw_dir) * 10000.0f;
        actions.push_back({BBXR_UI_THROW_ORIGIN, int32_t(std::lround(o.x)), int32_t(std::lround(o.y)),
                           int32_t(std::lround(o.z))});
        actions.push_back({BBXR_UI_THROW_DIR, int32_t(std::lround(d.x)), int32_t(std::lround(d.y)),
                           int32_t(std::lround(d.z))});
    }
    const int32_t speed_cms = r.thrown && !aimed_throw ? int32_t(std::lround(r.throw_speed * 100.0f)) : 0;
    if (actions.size() < 32) {
        actions.push_back({BBXR_UI_USE_QUICK, slot, int32_t(id), speed_cms});
    }
    pending = std::string("use goods:") + std::to_string(r.item.goods) + " (" + r.why + ")";
    pending_at = in.time;
    if (aimed_throw) {
        const float yaw = std::atan2(-r.throw_dir.x, -r.throw_dir.z) * 57.29578f;
        const float pitch = std::asin(std::clamp(r.throw_dir.y, -1.0f, 1.0f)) * 57.29578f;
        log += Fmt("game UI: held item: goods %d thrown (bullet %d, aimed%s): origin (%.2f %.2f %.2f), direction (%.2f %.2f %.2f) yaw %.0f pitch %.0f deg (headset space), marker (%.2f %.2f %.2f) %s, flight %.2f s, speed %.1f m/s (game's) -> use quick slot %d\n",
                   r.item.goods, r.item.aim_bullet, r.item.aimed ? "" : ", not redirected by the host", double(r.throw_origin.x),
                   double(r.throw_origin.y), double(r.throw_origin.z), double(r.throw_dir.x), double(r.throw_dir.y),
                   double(r.throw_dir.z), double(yaw), double(pitch), double(r.throw_marker.x),
                   double(r.throw_marker.y), double(r.throw_marker.z), r.marker_on_floor ? "on the floor" : "in the air",
                   double(r.flight_s), double(r.item.speed), slot);
    } else if (r.thrown) {
        log += Fmt("game UI: held item: goods %d thrown (bullet %d%s) at %.1f m/s, direction (%.2f %.2f %.2f) [raw velocity (%.2f %.2f %.2f), forward %.2f m/s: %s] -> use quick slot %d\n",
                   r.item.goods, r.item.bullet, r.item.aimed ? ", aimed" : ", game direction",
                   double(r.throw_speed), double(r.throw_dir.x), double(r.throw_dir.y),
                   double(r.throw_dir.z), double(r.throw_raw_velocity.x), double(r.throw_raw_velocity.y),
                   double(r.throw_raw_velocity.z), double(r.throw_forward), r.throw_note.c_str(), slot);
    } else {
        log += Fmt("game UI: held item: goods %d used by gesture (%s%s) -> use quick slot %d\n", r.item.goods,
                   r.why.c_str(),
                   r.blade_frac >= 0.0f ? Fmt(", touched %.2f along the blade", double(r.blade_frac)).c_str() : "",
                   slot);
    }
}

void GameUi::Update(const FrameInput& in, std::vector<Haptic>& haptics, std::string& log) {
    now = in.time;
    head_pose = in.head;
    hands_in = in;
    left_grip = in.grip[0];
    left_aim = in.aim[0];
    if (!hitbox_log.empty()) {
        log += hitbox_log;
        hitbox_log.clear();
    }
    BbXrGameState s;
    uint64_t seq;
    double at;
    {
        std::scoped_lock lock{mutex};
        seq = state_seq;
        at = state_at;
        if (seq != applied_seq) {
            current = state;
        }
    }
    s = current;
    if (seq != applied_seq) {
        applied_seq = seq;
        ApplyState(s, log);
    }
    const bool was_active = active;
    active = (s.flags & BBXR_GAME_IN_WORLD) && now - at < kStaleS;
    const sb::UiLayout& layout = menus.Layout();
    InputFilter::Settings fs;
    // No player yet (title, character creation, loading): the world UI stays off, the sticks navigate.
    const bool pre_world = !active && (s.flags & BBXR_GAME_PRE_WORLD) && now - at < kStaleS;
    fs.enabled = active || pre_world;
    fs.r3_hold_s = layout.game.r3_hold_s;
    fs.lock_on = layout.game.lock_on;
    fs.r3_menu = HasMenuOn(layout, sb::UiInputId::R3);
    fs.l3_menu = HasMenuOn(layout, sb::UiInputId::L3);
    for (const std::string& name : layout.game.active) { // face buttons the menus open on
        const sb::UiMenuSpec* m = layout.FindGameMenu(name);
        if (m && m->open.modifiers.empty() &&
            (m->open.key == sb::UiInputId::A || m->open.key == sb::UiInputId::B ||
             m->open.key == sb::UiInputId::X || m->open.key == sb::UiInputId::Y)) {
            fs.menu_face |= PadBit(m->open.key);
        }
    }
    filter.Configure(fs);
    filter.SetGameMenu(pre_world || (active && (s.flags & BBXR_GAME_MENU_OPEN) != 0));
    const InputFilter::UiView view = filter.Step(now, in.pad);
    {
        double held_s;
        bool hold;
        if (active && filter.TakeR3Episode(held_s, hold)) { // what became of an R3 click (the wheel's hold)
            char line[160];
            std::snprintf(line, sizeof(line), "game UI: R3 released after %.2f s: %s (hold %.2f s, r3 menu %d, lock-on %d)\n", held_s,
                          hold ? "the hold reached the menus" : "too short for the hold", fs.r3_hold_s, int(fs.r3_menu),
                          int(fs.lock_on));
            log += line;
        }
    }
    if (!active) {
        if (held_item.Active()) {
            held_item.Cancel(now, menus.Layout().game.held, "no player in the world");
        }
        SyncHeldSignal();
        filter.SetHoldMask(0);
        if (was_active) {
            menus.CloseAll();
            log += "game UI: off (no player in the world)\n";
        }
        filter.SetMenuState(false, false, false, false);
        filter.SetClaim(0);
        prev_buttons = in.pad.buttons;
        readout.enabled = false;
        return;
    }
    if (!was_active) {
        log += "game UI: on (a player in the world)\n";
    }

    // The body's yaw: the head's, with a dead zone (the sandbox's rig does the same).
    if (in.head.valid) {
        const float head_yaw = YawOf(in.head.rotation);
        if (!body_yaw_valid) {
            body_yaw = head_yaw;
            body_yaw_valid = true;
        }
        float d = head_yaw - float(body_yaw);
        while (d > kPi) d -= 2.0f * kPi;
        while (d < -kPi) d += 2.0f * kPi;
        if (std::fabs(d) > kBodyDeadzone) {
            body_yaw += d > 0.0f ? d - kBodyDeadzone : d + kBodyDeadzone;
        }
    }

    sb::UiInput ui;
    ui.time = in.time;
    ui.stage = in.stage;
    ui.floor_y = in.stage ? 0.0f : -world.eye_height_m; // LOCAL's origin is at the eyes
    ui.head = in.head;
    for (int h = 0; h < 2; ++h) {
        ui.grip[h] = in.grip[h];
        ui.aim[h] = in.aim[h];
        ui.trigger[h] = in.trigger[h];
        ui.squeeze[h] = in.squeeze[h];
    }
    ui.buttons = view.buttons;
    // L3 with a menu other than an l3 one open closes the menus (and is used up).
    if (view.buttons & BBXR_BTN_L3) {
        if (!l3_consumed && menus.Engaged() && !menus.IsOpen("quick_ring")) {
            bool l3_open = false;
            for (const std::string& name : layout.game.active) {
                const sb::UiMenuSpec* m = layout.FindGameMenu(name);
                l3_open = l3_open || (m && m->open.key == sb::UiInputId::L3 && menus.IsOpen(name));
            }
            if (!l3_open) {
                menus.CloseAll();
                l3_consumed = true;
                log += "game UI: L3: menus closed\n";
            }
        }
        if (l3_consumed) {
            ui.buttons &= ~BBXR_BTN_L3;
        }
    } else {
        l3_consumed = false;
    }
    if (view.close_menus) {
        menus.CloseAll();
        log += "game UI: L3 while walking: menus closed (no jump)\n";
    }
    sb::UiSticks(in.pad.lx, in.pad.ly, in.pad.rx, in.pad.ry, ui.stick);
    sb::UiBody body;
    body.yaw = float(body_yaw);
    for (int h = 0; h < 2; ++h) {
        if (in.aim[h].valid) {
            body.tip[h] = in.aim[h].position + Rotate(in.aim[h].rotation, {0, 0, -0.06f});
            body.tip_valid[h] = true;
        }
    }
    sb::UiEvents ev;
    menus.Update(ui, body, world, ev);
    for (const sb::UiHaptic& h : ev.haptics) {
        haptics.push_back({h.hand, h.amplitude, h.seconds});
    }
    log += ev.log;
    for (const sb::UiAction& a : ev.actions) {
        Act(a, log);
    }
    if (!layout.game.held_items && held_item.Active()) {
        held_item.Cancel(now, layout.game.held, "held_items turned off");
    }
    StepHeld(in, haptics, log);
    bool r3_open = false;
    for (const std::string& name : layout.game.active) {
        const sb::UiMenuSpec* m = layout.FindGameMenu(name);
        r3_open = r3_open || (m && m->open.key == sb::UiInputId::R3 && menus.IsOpen(name));
    }
    filter.SetMenuState(menus.Engaged(), r3_open, menus.UsesStick(0), menus.UsesStick(1));
    // Combos whose modifiers are held now: their key is the menus', not the game's.
    uint32_t claim = 0;
    for (const std::string& name : layout.game.active) {
        const sb::UiMenuSpec* m = layout.FindGameMenu(name);
        if (!m || m->open.modifiers.empty()) {
            continue;
        }
        bool held = true;
        for (sb::UiInputId id : m->open.modifiers) {
            held = held && ModifierHeld(id, in);
        }
        if (held) {
            claim |= PadBit(m->open.key);
        }
    }
    filter.SetClaim(claim);

    // The debug gun ray: remember the pull, log the aim once per pull (rate limited).
    ray_in_world = true;
    if ((in.pad.buttons & BBXR_BTN_L2) && !(prev_buttons & BBXR_BTN_L2)) {
        last_pull = now;
        if (in.aim[0].valid && now - last_pull_log > 0.25) {
            last_pull_log = now;
            const Vec3 dir = Rotate(in.aim[0].rotation, {0, 0, -1});
            float wy, wp, hy, hp;
            AimAngles(dir, nullptr, &wy, &wp);
            AimAngles(dir, in.head.valid ? &in.head.rotation : nullptr, &hy, &hp);
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "gun ray: L2 pull origin (%.3f %.3f %.3f) dir (%.3f %.3f %.3f) %s; "
                          "world yaw %.1f pitch %.1f deg, head-relative yaw %.1f pitch %.1f deg\n",
                          in.aim[0].position.x, in.aim[0].position.y, in.aim[0].position.z,
                          dir.x, dir.y, dir.z, in.stage ? "STAGE" : "LOCAL", wy / kDeg,
                          wp / kDeg, hy / kDeg, hp / kDeg);
            log += buf;
        }
    }

    // The swing HUD.
    Swings(in, s, haptics);
    Events(haptics, log);
    prev_buttons = in.pad.buttons;
    readout.now = now;
    const bool stamina = (s.flags & BBXR_GAME_STAMINA) != 0;
    readout.stamina = stamina ? float(s.stamina) : 0.0f;
    readout.max = stamina ? float(std::max(s.stamina_max, 1)) : 1.0f;
    readout.can_act = !stamina || s.stamina > 0;
    const bool recent = now - last_activity < kActivityS ||
                        readout.result == Result::Pending;
    readout.enabled = layout.game.hud && stamina && (layout.game.bar != "action" || recent);
}

void GameUi::DrawGunRay(const sb::UiGameLayout& g) {
    const GunRayLook look = GunRayFor(g, ray_in_world, left_aim.valid, now - last_pull);
    if (!look.visible) {
        return;
    }
    const Vec3 dir = Rotate(left_aim.rotation, {0, 0, -1});
    const float length = g.debug_gun_ray_length_m;
    const Vec3 from = left_aim.position, to = from + dir * length;
    const Vec3 mid = from + dir * (length * 0.5f);
    sb::UiPanel beam;
    beam.origin = mid;
    beam.x = dir;
    beam.z = Normalize(Perp(head_pose.position - mid, dir), AnyPerp(dir));
    beam.y = Cross(beam.z, beam.x);
    hud_canvas.on_top = true;
    hud_canvas.alpha = look.alpha;
    hud_canvas.Box(beam, 0.0f, 0.0f, length, look.width_m, g.debug_gun_ray_color);
    const sb::UiPanel dot = sb::UiPanel::Facing(to, head_pose.position, {0, 1, 0});
    const float r = look.flash ? 0.12f : 0.06f; // grows with the distance it is seen from
    hud_canvas.alpha = 1.0f;
    hud_canvas.Disc(dot, 0, 0, r * 1.3f, {1, 1, 1, 0.9f});
    hud_canvas.Disc(dot, 0, 0, r, g.debug_gun_ray_color);
    hud_canvas.on_top = false;
}

void GameUi::DrawHeldItem(const sb::UiGameLayout& g, held::TriBatch* hands) {
    if (!held_item.Active()) {
        return;
    }
    const held::ItemInfo& info = held_item.Info();
    const bool grip = held_item.Gripped();
    // The item: its extracted 3D mesh at the grip (translucent and pulsing until the grip closes,
    // solid once held), else a coloured disc at the hand; plus a ring that fills while the gesture
    // is held and what to do with it.
    const sb::UiColor color = info.kind == held::Kind::Mouth   ? sb::UiColor{0.85f, 0.08f, 0.1f, 1.0f}
                              : info.kind == held::Kind::Blade ? sb::UiColor{1.0f, 0.55f, 0.1f, 1.0f}
                                                               : sb::UiColor{0.3f, 0.7f, 1.0f, 1.0f};
    const Vec3 at = held_item.ItemPos();
    const sb::UiPanel panel = sb::UiPanel::Facing(at, head_pose.position, {0, 1, 0});
    hud_canvas.on_top = true;
    hud_canvas.alpha = grip ? 1.0f : 0.55f;
    float ring_in = 0.029f; // the progress ring clears the mesh
    const held::MeshCache::Entry* mesh = held_meshes.Resolve(assets, info.goods);
    const int h = held_item.Hand();
    if (mesh && held_logged.insert(info.goods).second) {
        // One line per held item: the mesh file was found / a stand-in is drawn (and why, with the path tried).
        if (mesh->standin) {
            hitbox_log += Fmt("game UI: held item: goods %d %s (no mesh file: %s)\n", info.goods, mesh->source.c_str(),
                              mesh->why.c_str());
        } else {
            hitbox_log += Fmt("game UI: held item: goods %d mesh file found: %s\n", info.goods,
                              mesh->tried_path.c_str());
        }
    }
    if (mesh && hands_in.grip[h].valid) {
        // One placement for both hand modes (vr_held_mesh.h SocketMeshWorld: the sandbox's tuned pose
        // for its rows, else the item's category socket with the mesh's own upright turn). With the
        // custom hands the mesh joins their batch (sorted with the fingers, which wrap it); with the
        // game's hands it is alone in the top layer.
        sb::UiColor tint = mesh->spec.color;
        tint.a = grip ? 1.0f : 0.40f + 0.20f * std::sin(float(now) * 7.0f);
        // The game's hands: the visible hand is the arms IK's, not the controller (the IK eases / clamps
        // a far target, and the game's hand sits where its bone is), so the item goes to the hand bone's
        // frame (a grip pose in the headset's space) when the host sent it, else the controller.
        const BbXrGameDebug& dbg = current.debug;
        const bool bone_ok = !hands && (dbg.flags & BBXR_DEBUG_POSE) && (dbg.weapon_valid & (1u << h));
        const held::HandPlacement at_hand = held::PlaceInHand(bone_ok ? dbg.weapon[h] : nullptr,
                                                               hands_in.grip[h].position, hands_in.grip[h].rotation);
        const Mat4 world = held::SocketMeshWorld(mesh->spec, mesh->from_table, info.kind, held_item.Id(), h,
                                                  at_hand.pos, at_hand.rot);
        size_t drawn = 0;
        if (hands) {
            std::vector<Vec3> pos, nrm;
            held::TransformModel(mesh->model, world, pos, nrm);
            drawn = held::AddModel(*hands, mesh->model, pos, nrm, tint, held_meshes.Sheet(assets),
                                    info.kind == held::Kind::Mouth ? 0.85f : 0.0f);
        } else {
            drawn = held::AppendMesh(hud_canvas.top, mesh->model, world, tint, head_pose.position,
                                      held_meshes.Sheet(assets),
                                      info.kind == held::Kind::Mouth ? 0.85f : 0.0f);
        }
        if (grip && held_logged.insert(-info.goods - 1000000).second) { // once: it was really drawn
            const Vec3 c = {world.m[12], world.m[13], world.m[14]};
            hitbox_log += Fmt("game UI: held draw: goods %d: %zu of %zu triangles in the %s hand (%s), centre %.2f m from the head, scale %.3f\n",
                              info.goods, drawn, mesh->model.indices.size() / 3, h ? "right" : "left",
                              hands ? "custom hands" : "the game's hands", double(Length(c - head_pose.position)),
                              double(mesh->spec.scale));
            if (!hands) {
                hitbox_log += Fmt("game UI: held place: goods %d at the %s hand %s; controller grip to the game hand %.3f m\n",
                                  info.goods, h ? "right" : "left",
                                  at_hand.from_bone ? "bone (R_Weapon/L_Weapon frame)"
                                  : bone_ok         ? "controller (the bone is not a pose or too far)"
                                                    : "controller (no hand bone from the host)",
                                  bone_ok ? double(at_hand.gap_m) : -1.0);
            }
        }
        ring_in = std::max(ring_in, mesh->radius + 0.008f);
    } else {
        hud_canvas.Disc(panel, 0, 0, 0.026f, {0.0f, 0.0f, 0.0f, 0.8f});
        hud_canvas.Disc(panel, 0, 0, 0.021f, color);
        if (held_logged.insert(-info.goods - 1).second) {
            hitbox_log += Fmt("game UI: held draw: goods %d drawn as a disc (%s)\n", info.goods,
                              !mesh ? "no mesh or stand-in" : "the hand's grip pose is not valid");
        }
    }
    if (held_item.Progress() > 0.0f || held_item.InContact()) {
        hud_canvas.Sector(panel, 0, 0, 1.5708f, 3.1416f * std::max(held_item.Progress(), 0.05f), ring_in,
                          ring_in + 0.006f, {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (hud_font.Ready()) {
        hud_canvas.Text(panel, 0.0f, ring_in + 0.017f, 0.02f, held_item.Hint(), {1, 1, 1, 1},
                        sb::UiAlign::Center);
        hud_canvas.Text(panel, 0.0f, ring_in + 0.039f, 0.013f, held_item.CancelHint(), {0.8f, 0.8f, 0.8f, 1},
                        sb::UiAlign::Center);
    }
    if (grip && info.kind == held::Kind::Mouth && head_pose.valid) {
        // The mouth, as a faint ring.
        const Vec3 mouth = head_pose.position + Rotate(head_pose.rotation, g.held.mouth);
        const sb::UiPanel ring = sb::UiPanel::Facing(mouth, head_pose.position, {0, 1, 0});
        hud_canvas.alpha = held_item.InContact() ? 0.9f : 0.35f;
        hud_canvas.Sector(ring, 0, 0, 0.0f, 3.2f, g.held.mouth_radius_m - 0.004f, g.held.mouth_radius_m,
                          color);
    }
    hud_canvas.on_top = false;
    hud_canvas.alpha = 1.0f;
}

bool GameUi::CustomHandsOn() const {
    return custom_hands_env >= 0 ? custom_hands_env == 1 : menus.Layout().game.custom_hands.on;
}

void GameUi::DrawHands(const sb::UiGameLayout& g) {
    hand_tris = 0;
    held::TriBatch batch(head_pose.position);
    const sb::UiCustomHands& c = g.custom_hands;
    for (int h = 0; h < 2; ++h) {
        const sb::UiPose& grip = hands_in.grip[h];
        if (!grip.valid) {
            continue;
        }
        // The pose: the item's hold while one is gripped in this hand, the weapon grip while the
        // hand holds a game weapon (the trigger still closes the index), else the controller's.
        hand::Pose pose = hand::ControllerPose(hands_in.squeeze[h], hands_in.trigger[h]);
        if (held_item.Active() && held_item.Hand() == h && held_item.Gripped()) {
            pose = hand::HoldPose(hand::GripFor(held_item.Id()), hands_in.trigger[h]);
        } else if (c.weapon_grip) {
            // weapons[]: right 1, right 2, left 1, left 2; weapon_active[0] right, [1] left.
            const uint32_t w = h ? current.weapons[current.weapon_active[0] & 1]
                                 : current.weapons[2 + (current.weapon_active[1] & 1)];
            if (w != BBXR_ITEM_NONE && w != 0u && w != 110000u) { // 0: no state yet; 110000: unarmed (the fists row)
                pose = hand::HoldPose(hand::CategoryGrip(hand::HoldCategory::Weapon), hands_in.trigger[h]);
            }
        }
        hand_pose[h] = pose;
        const hand::HandMesh& m = hand_models.hand[h];
        Mat4 joints[hand::kJoints];
        hand::PoseJoints(m, pose, joints);
        std::vector<Vec3> pos, nrm;
        hand::Skin(m, joints, hand::HandWorld(m, h, grip.position, grip.rotation, c.scale), pos, nrm);
        hand_tris += batch.Add(pos, nrm, m.indices.data(), m.indices.size(), c.color);
    }
    DrawHeldItem(g, &batch); // the item's mesh joins the batch; its ring and hint are drawn there
    hud_canvas.on_top = true;
    hud_canvas.alpha = 1.0f;
    batch.Emit(hud_canvas.top);
    hud_canvas.on_top = false;
}

void GameUi::DrawSkins() {
    skin_tris = 0;
    if (!skin_cfg_read) {
        skin_cfg = skin::ReadConfig();
        skin_cfg_read = true;
    }
    if (!skin_cfg.Enabled()) {
        return;
    }
    if (!skin_tried) { // once: the mesh is cached, never loaded per frame
        skin_tried = true;
        std::string msg;
        skin_ok = skin::LoadConfigured(skin_cfg, assets, skin_mesh, msg);
        hitbox_log += msg + '\n';
    }
    const uint32_t count = std::min<uint32_t>(current.skin.count, BBXR_SKIN_MAX);
    if (!skin_ok || count == 0) {
        return;
    }
    if (!skin_announced) {
        skin_announced = true;
        hitbox_log += Fmt("VR skin: drawing %u target(s)\n", count);
    }
    hud_canvas.on_top = true;
    hud_canvas.alpha = 1.0f;
    const float height = skin_mesh.hi.y - skin_mesh.lo.y;
    for (uint32_t i = 0; i < count; ++i) {
        const BbXrSkinTarget& t = current.skin.targets[i];
        float roll = 0.0f;
        float bob = 0.0f;
        if (skin_cfg.bob) {
            bob = skin::SkinBob(float(std::fmod(now, 3600.0)) + float(t.id) * 0.37f, t.speed_mps, &roll, height);
        }
        const Vec3 pos{t.pos[0], t.pos[1], t.pos[2]};
        const Vec3 fwd{t.forward[0], t.forward[1], t.forward[2]};
        const Vec3 up{t.up[0], t.up[1], t.up[2]};
        if (!std::isfinite(pos.x + pos.y + pos.z + fwd.x + fwd.y + fwd.z + up.x + up.y + up.z)) {
            continue;
        }
        const Mat4 world = skin::SkinWorld(pos, fwd, up, skin_cfg.yaw_rad, bob, roll);
        skin_tris += skin::AppendSkin(hud_canvas.top, skin_mesh, world, head_pose.position);
    }
    hud_canvas.on_top = false;
}

// The aimed throw: the arc as a thin ribbon from the hand and a flat ring + dot at the impact.
static void DrawThrowAim(sb::UiCanvas& cv, const held::ThrowArc& arc, const Vec3& eye) {
    const bool floor_hit = arc.hit_floor;
    const sb::UiColor arc_c = floor_hit ? sb::UiColor{1.0f, 0.55f, 0.12f, 0.75f} : sb::UiColor{0.7f, 0.7f, 0.7f, 0.6f};
    const sb::UiColor mark_c = floor_hit ? sb::UiColor{1.0f, 0.6f, 0.15f, 0.85f} : sb::UiColor{0.6f, 0.6f, 0.6f, 0.7f};
    cv.on_top = true;
    cv.alpha = 1.0f;
    const size_t step = 2;
    for (size_t i = 0; i + 1 < arc.points.size(); i += step) {
        const Vec3 a = arc.points[i], b = arc.points[std::min(i + step, arc.points.size() - 1)];
        const Vec3 d = b - a;
        const float len = Length(d);
        if (len < 1e-4f) {
            continue;
        }
        const Vec3 dir = d * (1.0f / len);
        const Vec3 mid = (a + b) * 0.5f;
        sb::UiPanel seg;
        seg.origin = mid;
        seg.x = dir;
        seg.z = Normalize(Perp(eye - mid, dir), AnyPerp(dir));
        seg.y = Cross(seg.z, seg.x);
        cv.Box(seg, 0.0f, 0.0f, len * 1.05f, 0.01f, arc_c);
    }
    const Vec3 c = arc.impact + Vec3{0.0f, 0.01f, 0.0f};
    constexpr int kSeg = 24;
    constexpr float kR = 0.35f, kTwoPi = 6.2831853f;
    for (int i = 0; i < kSeg; ++i) {
        const float a0 = kTwoPi * float(i) / kSeg, a1 = kTwoPi * float(i + 1) / kSeg, am = 0.5f * (a0 + a1);
        const Vec3 p0 = c + Vec3{std::cos(a0), 0, std::sin(a0)} * kR, p1 = c + Vec3{std::cos(a1), 0, std::sin(a1)} * kR;
        sb::UiPanel seg;
        seg.origin = c + Vec3{std::cos(am), 0, std::sin(am)} * kR;
        seg.x = Normalize(p1 - p0);
        seg.z = {0, 1, 0};
        seg.y = Cross(seg.z, seg.x);
        cv.Box(seg, 0.0f, 0.0f, Length(p1 - p0) * 1.1f, 0.03f, mark_c);
    }
    sb::UiPanel flat;
    flat.origin = c;
    flat.x = {1, 0, 0};
    flat.z = {0, 1, 0};
    flat.y = Cross(flat.z, flat.x);
    cv.Disc(flat, 0, 0, 0.05f, mark_c);
    cv.on_top = false;
}

bool GameUi::Build() {
    if (!active) {
        hud_canvas.Clear();
        menus.Build(); // empties the menus' geometry too
        ray_in_world = false;
        hud_visible = false;
        if (hud_gpu_ready) {
            hud_gpu.SetGeometry(hud_canvas);
        }
        return false;
    }
    menus.Build();
    hud_canvas.Clear();
    const sb::UiGameLayout& g = menus.Layout().game;
    sb::SwingHudConfig c;
    c.head = g.hud_head;
    c.wrist = g.hud_wrist;
    c.distance = g.hud_distance;
    c.down_deg = g.hud_down_deg;
    c.scale = g.hud_scale;
    c.wrist_scale = g.wrist_scale;
    c.wrist_offset = g.wrist_offset;
    c.label_s = g.label_s;
    c.speed = false;  // the game's swings are xr_input's speed thresholds, no live classifier
    c.popups = false; // no hit point from the game yet
    hud_canvas.outline = 0.12f;
    sb::DrawSwingHud(hud_canvas, readout, c, head_pose, left_grip);
    DrawGunRay(g);
    hand_tris = 0;
    if (CustomHandsOn() && !hand_models.tried) {
        if (hand_models.Load(assets / "hands")) {
            hitbox_log += "game UI: custom hands: " + (assets / "hands").string() + " loaded\n";
        } else {
            hitbox_log += "game UI: custom hands: " + hand_models.error +
                          " (out/vr_assets/hands: WebXR Input Profiles generic-hand, docs/vr/findings/40)\n";
        }
    }
    if (CustomHandsOn() && hand_models.Ready()) {
        DrawHands(g);
    } else {
        DrawHeldItem(g);
    }
    hud_canvas.on_top = false;
    hud_canvas.alpha = 1.0f;
    if (const held::ThrowArc* arc = held_item.Aim()) {
        DrawThrowAim(hud_canvas, *arc, head_pose.position);
    }
    DrawSkins();
    hitboxes.Draw(hud_canvas, g.debug_hitboxes, current, now, head_pose.position, hitbox_log);
    hud_visible = !hud_canvas.world.empty() || !hud_canvas.top.empty();
    if (hud_gpu_ready) {
        hud_gpu.SetGeometry(hud_canvas);
    }
    return hud_visible || menus.Visible();
}

void GameUi::Draw(VkCommandBuffer cmd, const Mat4& view_proj, VkFormat color) {
    if (sb::DrawHook* gpu = menus.Gpu()) {
        gpu->Draw(cmd, view_proj, color, VK_FORMAT_UNDEFINED);
    }
    if (hud_gpu_ready) {
        hud_gpu.Draw(cmd, view_proj, color, VK_FORMAT_UNDEFINED);
    }
}

} // namespace vrui
