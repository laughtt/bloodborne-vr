// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <sstream>

#include "bbport_xr_bridge.h"

namespace sb {

using namespace m3;

namespace {

constexpr float kPress = 0.6f, kRelease = 0.4f; // analog grip/trigger hysteresis
constexpr float kTouchDepth = 0.03f;            // fingertip this close above a panel: touching
constexpr float kRadialPoke = 0.03f;            // same-hand radial: pushed this far through the ring: a poke
constexpr float kPokeAt = 0.006f;               // ... and crossing this height: a poke
constexpr float kNear = 0.35f;                  // body slot markers brighten within this reach

std::string Lower(std::string s) {
    for (char& c : s) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

uint32_t ButtonBit(UiInputId id) {
    switch (id) {
    case UiInputId::A: return BBXR_BTN_CIRCLE;
    case UiInputId::B: return BBXR_BTN_CROSS;
    case UiInputId::X: return BBXR_BTN_SQUARE;
    case UiInputId::Y: return BBXR_BTN_TRIANGLE;
    case UiInputId::L3: return BBXR_BTN_L3;
    case UiInputId::R3: return BBXR_BTN_R3;
    case UiInputId::DpadUp: return BBXR_BTN_UP;
    case UiInputId::DpadDown: return BBXR_BTN_DOWN;
    case UiInputId::DpadLeft: return BBXR_BTN_LEFT;
    case UiInputId::DpadRight: return BBXR_BTN_RIGHT;
    case UiInputId::Options: return BBXR_BTN_OPTIONS;
    case UiInputId::Touchpad: return BBXR_BTN_TOUCHPAD;
    default: return 0;
    }
}

bool Analog(UiInputId id) {
    return id == UiInputId::LeftGrip || id == UiInputId::RightGrip ||
           id == UiInputId::LeftTrigger || id == UiInputId::RightTrigger;
}

float AnalogValue(UiInputId id, const UiInput& in) {
    switch (id) {
    case UiInputId::LeftGrip: return in.squeeze[0];
    case UiInputId::RightGrip: return in.squeeze[1];
    case UiInputId::LeftTrigger: return in.trigger[0];
    case UiInputId::RightTrigger: return in.trigger[1];
    default: return 0.0f;
    }
}

const char* HandName(int hand) {
    return hand == 0 ? "left" : "right";
}

Vec3 AxisVec(const std::string& s) {
    const float sign = !s.empty() && s[0] == '-' ? -1.0f : 1.0f;
    const char a = s.empty() ? 'x' : char(std::tolower(static_cast<unsigned char>(s.back())));
    return a == 'y' ? Vec3{0, sign, 0} : a == 'z' ? Vec3{0, 0, sign} : Vec3{sign, 0, 0};
}

/// Radial items run clockwise from the top: item k is centered at 90 deg - k * 360 / n.
float SegmentAngle(int k, int n) {
    return 0.5f * kPi - float(k) * 2.0f * kPi / float(std::max(n, 1));
}

int SegmentAt(float angle, int n) {
    if (n <= 0) {
        return -1;
    }
    const float step = 2.0f * kPi / float(n);
    const int k = int(std::lround((0.5f * kPi - angle) / step));
    return ((k % n) + n) % n;
}

float Ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

bool Has(const std::vector<std::string>& list, const char* what) {
    return std::find(list.begin(), list.end(), what) != list.end();
}

Vec3 Forward(const Quat& q) {
    return Rotate(q, {0, 0, -1});
}

Vec3 Flat(Vec3 v, Vec3 fallback) {
    v.y = 0.0f;
    return Normalize(v, fallback);
}

/// A JSON object on one line (the interaction log).
class JsonLine {
public:
    JsonLine& Add(const char* key, const std::string& value) {
        Key(key);
        body += '"';
        for (char c : value) {
            if (c == '"' || c == '\\') {
                body += '\\';
                body += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                body += esc;
            } else {
                body += c;
            }
        }
        body += '"';
        return *this;
    }
    JsonLine& Add(const char* key, const char* value) { return Add(key, std::string(value)); }
    JsonLine& Add(const char* key, double value) {
        Key(key);
        char text[32];
        std::snprintf(text, sizeof(text), "%.4g", value);
        body += text;
        return *this;
    }
    JsonLine& Add(const char* key, int value) {
        Key(key);
        body += std::to_string(value);
        return *this;
    }
    JsonLine& Add(const char* key, bool value) {
        Key(key);
        body += value ? "true" : "false";
        return *this;
    }
    std::string Str() const { return "{" + body + "}"; }

private:
    void Key(const char* key) {
        if (!body.empty()) {
            body += ", ";
        }
        body += '"';
        body += key;
        body += "\": ";
    }
    std::string body;
};

} // namespace

namespace {

/// A grid's layout, everything scaled with its card size (0.08 m cards: the base sizes).
struct GridMetrics {
    float unit, pad, gap, title, tabs, footer, width, height;
};

GridMetrics GridOf(const UiMenuSpec& s) {
    GridMetrics g;
    g.unit = s.cell / 0.08f;
    g.pad = 0.014f * g.unit;
    g.gap = 0.008f * g.unit;
    g.title = 0.05f * g.unit;
    g.tabs = s.tabs.empty() ? 0.0f : 0.042f * g.unit;
    g.footer = 0.07f * g.unit;
    g.width = float(s.columns) * s.cell + float(s.columns - 1) * g.gap + 2.0f * g.pad;
    g.height = g.title + g.tabs + float(s.rows) * s.cell + float(s.rows - 1) * g.gap + g.footer +
               2.0f * g.pad;
    return g;
}

std::string TabLabel(const std::string& key) {
    if (key == "tool") {
        return "Tools";
    }
    std::string label = key;
    if (!label.empty()) {
        label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
    }
    return label;
}

} // namespace

UiPanel UiGridPanel(const UiMenuSpec& s, const UiPose& head) {
    const Vec3 f = Flat(Forward(head.rotation), {0, 0, -1});
    const float t = s.tilt * kDeg;
    UiPanel p;
    p.origin = head.position + f * s.distance + Vec3{0.0f, -s.below, 0.0f};
    p.z = Normalize(-f * std::cos(t) + Vec3{0, 1, 0} * std::sin(t), -f);
    p.x = Normalize(Cross({0, 1, 0}, p.z), {1, 0, 0});
    p.y = Cross(p.z, p.x);
    return p;
}

void UiGridSize(const UiMenuSpec& spec, float& width, float& height) {
    const GridMetrics g = GridOf(spec);
    width = g.width;
    height = g.height;
}

void UiGridCard(const UiMenuSpec& spec, int k, float& u, float& v) {
    const GridMetrics g = GridOf(spec);
    const int r = k / std::max(spec.columns, 1), c = k % std::max(spec.columns, 1);
    u = -g.width * 0.5f + g.pad + spec.cell * 0.5f + float(c) * (spec.cell + g.gap);
    v = g.height * 0.5f - g.pad - g.title - g.tabs - spec.cell * 0.5f -
        float(r) * (spec.cell + g.gap);
}

bool UiGridTab(const UiMenuSpec& spec, int i, float& u, float& v) {
    const GridMetrics g = GridOf(spec);
    const int n = int(spec.tabs.size());
    if (i < 0 || i >= n) {
        return false;
    }
    const float tw = (g.width - 2.0f * g.pad - float(n - 1) * g.gap) / float(n);
    u = -g.width * 0.5f + g.pad + tw * 0.5f + float(i) * (tw + g.gap);
    v = g.height * 0.5f - g.pad - g.title - g.tabs * 0.5f;
    return true;
}

void UiSticks(uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, float stick[2][2]) {
    const auto axis = [](uint8_t v) { return std::clamp((float(v) - 128.0f) / 127.0f, -1.0f, 1.0f); };
    stick[0][0] = axis(lx);
    stick[0][1] = -axis(ly);
    stick[1][0] = axis(rx);
    stick[1][1] = -axis(ry);
}

// ---- State -----------------------------------------------------------------------------------------

struct UiSystem::Impl {
    struct Element {
        int id; // >= 0 item index; kPrev, kNext, kClose
        float x0, y0, x1, y1;
    };
    static constexpr int kPrev = -2, kNext = -3, kClose = -4, kTab = -10; // tabs: kTab - i

    struct Menu {
        const UiMenuSpec* spec = nullptr;
        bool open = false;   // radial/grid: opened by its binding; wrist: shown; slots: always
        float anim = 0.0f;   // 0 closed .. 1 open
        double opened_at = 0.0, hover_since = 0.0;
        UiPanel base;        // placement (animation scale applied when drawn)
        Vec3 open_hand;
        int hover = -1, hover_hand = -1, hover_changes = 0;
        std::string hover_method;
        int page = 0, tab = 0;
        bool flick_armed = true, flick_armed_y = true, following = false;
        bool stick_armed = true; // radial: a stick already deflected at open is ignored until it recenters
        std::vector<Element> elements;
        float touch_height[2] = {1.0f, 1.0f};
        float half_w = 0.1f, half_h = 0.1f; // panel extents (grid, wrist), from the last draw
        // body slots
        std::vector<Vec3> slot_world;
        int slot_hover[2] = {-1, -1};
        double slot_since[2] = {};
        bool seated = false;
        int flash_slot = -1;
        double flash_at = -10.0;
        // dashboard
        struct DashInfo {
            bool enabled = true;
            std::string title, detail, reason;
        };
        std::map<int, DashInfo> dash; // per element id, from the last draw
        bool use_mode = true;         // a quick slot click: use it (else only make it current)
        bool remove_mode = false;     // one-shot: the next quick item pointed at leaves the quick slots
    };
    // Dashboard element ids (Element::id).
    static constexpr int kDashWeapon = 1000, kDashQuick = 1100, kDashTab = 1200,
                         kDashButton = 1300, kDashPrev = 1400, kDashNext = 1401,
                         kDashMode = 1402, kDashClose = 1403, kDashRemove = 1404, kDashItem = 2000;
    struct Popup {
        std::string text;
        Vec3 position;
        double start = 0.0;
    };
    struct Stats {
        int opens = 0, selects = 0, cancels = 0, grabs = 0, empties = 0;
        double select_ms = 0.0;
    };
    struct Pointer {
        bool show = false, hit = false;
        Vec3 from, to;
        float distance = 1e9f;
    };

    std::filesystem::path assets;
    UiAtlas atlas;
    UiFont font;
    UiRenderer gpu;
    bool gpu_ready = false;
    UiCanvas canvas{&atlas, &font};
    UiLayout layout;
    std::unique_ptr<UiLayoutFile> file;
    int preset = 0;
    std::vector<Menu> menus;
    std::map<std::string, int> counts; // lowercase item name -> count
    // The game mode (LoadGame): the layout's "game" menus, the live lists and counts.
    bool game = false;
    UiLiveData live;
    bool drew = false; // the last Build drew anything
    // Every input that is a modifier of some binding in use: a binding fires only when the held
    // ones among them are exactly its own (so "r3" does not fire with a grip held, and "l3" does
    // not fire inside an L3 chord).
    bool modifier_vocabulary[size_t(UiInputId::Count)] = {};

    bool held[size_t(UiInputId::Count)] = {}, prev[size_t(UiInputId::Count)] = {};
    uint32_t claimed = 0, last_raw = 0;
    double now = 0.0, dt = 0.0;
    bool started = false;
    UiInput in;
    Vec3 tip[2];
    bool tip_valid[2] = {};
    float body_yaw = 0.0f, eye_height = 1.65f;
    std::string equipped[2];
    Pointer pointer[2];
    bool weapons_hidden = false, weapons_shown_before = true;

    std::string toast_title, toast_body;
    double toast_at = -100.0, toast_seconds = 6.0;
    bool help = false;
    std::vector<Popup> popups;

    std::string log_prefix = "ui";
    std::filesystem::path log_path;
    std::ofstream log_file;
    bool log_failed = false;
    std::map<std::string, Stats> stats;

    // -- helpers --
    const UiPreset* Preset() const {
        return !game && preset >= 0 && size_t(preset) < layout.presets.size()
                   ? &layout.presets[size_t(preset)]
                   : nullptr;
    }
    std::string PresetName() const {
        return game ? std::string("game") : Preset() ? Preset()->name : std::string();
    }

    bool Held(UiInputId id) const { return held[size_t(id)]; }
    bool PressedKey(UiInputId id) const { return held[size_t(id)] && !prev[size_t(id)]; }
    bool Mods(const UiBinding& b) const {
        for (UiInputId id : b.modifiers) {
            if (!Held(id)) {
                return false;
            }
        }
        return true;
    }
    bool Pressed(const UiBinding& b) const { return !b.Empty() && PressedKey(b.key) && Mods(b); }
    /// Pressed, and no other modifier of the bindings in use is held (opening and cycling).
    bool PressedExact(const UiBinding& b) const {
        if (!Pressed(b)) {
            return false;
        }
        for (size_t i = 1; i < size_t(UiInputId::Count); ++i) {
            const UiInputId id = UiInputId(i);
            if (!modifier_vocabulary[i] || !Held(id) || id == b.key) {
                continue;
            }
            // A plain key ("r3", "l3") is not a chord: a squeezed grip or trigger (the hand is usually
            // gripping the controller or a weapon) does not stop it; only a held button does.
            if (b.modifiers.empty() && Analog(id)) {
                continue;
            }
            if (std::find(b.modifiers.begin(), b.modifiers.end(), id) == b.modifiers.end()) {
                return false;
            }
        }
        return true;
    }
    void BuildVocabulary() {
        std::fill(std::begin(modifier_vocabulary), std::end(modifier_vocabulary), false);
        const auto add = [&](const UiBinding& b) {
            for (UiInputId id : b.modifiers) {
                modifier_vocabulary[size_t(id)] = true;
            }
        };
        for (const Menu& m : menus) {
            add(m.spec->open);
        }
        if (!game) {
            add(layout.cycle_next);
            add(layout.cycle_prev);
            add(layout.help);
            add(layout.reset);
        }
    }
    bool HeldAll(const UiBinding& b) const { return !b.Empty() && Held(b.key) && Mods(b); }

    /// "@list/k": entry k of a live list (body slots); anything else stays.
    std::string Resolve(const std::string& name) const {
        if (name.size() < 2 || name[0] != '@') {
            return name;
        }
        const size_t slash = name.find('/');
        if (slash == std::string::npos) {
            return name;
        }
        const auto it = live.lists.find(name.substr(1, slash - 1));
        const int k = std::atoi(name.c_str() + slash + 1);
        if (it == live.lists.end() || k < 0 || size_t(k) >= it->second.items.size()) {
            return std::string();
        }
        return it->second.items[size_t(k)];
    }
    const UiItem* Item(const std::string& name) const {
        const std::string key = Resolve(name);
        if (key.empty()) {
            return nullptr;
        }
        if (const UiItem* item = atlas.Find(key)) {
            return item;
        }
        // An upgraded weapon (+N: id + 100 N) has the base weapon's row in the atlas.
        if (key.rfind("weapon:", 0) == 0) {
            const long id = std::atol(key.c_str() + 7);
            if (id > 0) {
                return atlas.Find("weapon:" + std::to_string(id / 10000 * 10000));
            }
        }
        return nullptr;
    }
    std::string Display(const std::string& name) const {
        const UiItem* item = Item(name);
        return item ? item->name : Resolve(name);
    }
    int CountOf(const std::string& name) const {
        const auto it = counts.find(Lower(Display(name)));
        if (it != counts.end()) {
            return it->second;
        }
        // The game mode: a weapon or tool the player has is not counted; an item it lacks is 0.
        const UiItem* item = Item(name);
        return game && item && item->Consumable() ? 0 : -1;
    }
    int IconOf(const std::string& name) const {
        const UiItem* item = Item(name);
        return item ? item->icon : -1;
    }
    /// The live list a menu shows ("quick" for items ["@quick"]), or "".
    static std::string SourceOf(const Menu& m) {
        const std::vector<std::string>& items = m.spec->items;
        if (items.size() == 1 && items[0].size() > 1 && items[0][0] == '@' &&
            items[0].find('/') == std::string::npos) {
            return items[0].substr(1);
        }
        return std::string();
    }
    /// The items a menu shows now; `index` gets each one's index in its source list (the quick
    /// slot), or in the menu's own list.
    /// `sources` (when given) gets each item's source list name ("quick", "weapons"), "" for a
    /// plain item. A radial may chain several lists (["@quick", "@weapons"]: the quick wheel).
    static bool IsChain(const Menu& m) {
        const std::vector<std::string>& items = m.spec->items;
        if (items.size() < 2 || m.spec->type != "radial") {
            return false;
        }
        for (const std::string& it : items) {
            if (it.size() < 2 || it[0] != '@' || it.find('/') != std::string::npos) {
                return false;
            }
        }
        return true;
    }
    std::vector<std::string> Items(const Menu& m, std::vector<int>* index = nullptr,
                                   std::vector<std::string>* sources = nullptr) const {
        std::vector<std::string> out;
        const auto put = [&](const std::string& item, int k) {
            out.push_back(item);
            if (index) {
                index->push_back(k);
            }
            if (sources) {
                sources->push_back(SourceOf(m));
            }
        };
        if (IsChain(m)) {
            for (const std::string& ref : m.spec->items) {
                const auto it = live.lists.find(ref.substr(1));
                if (it == live.lists.end()) {
                    continue;
                }
                for (size_t k = 0; k < it->second.items.size(); ++k) {
                    if (it->second.items[k].empty()) {
                        continue;
                    }
                    out.push_back(it->second.items[k]);
                    if (index) {
                        index->push_back(int(k));
                    }
                    if (sources) {
                        sources->push_back(ref.substr(1));
                    }
                }
            }
            return out;
        }
        const std::string source = SourceOf(m);
        const bool inventory = source == "inventory" ||
                               (m.spec->items.empty() &&
                                (m.spec->type == "grid" || m.spec->type == "dashboard"));
        if (!inventory) {
            if (!source.empty()) {
                const auto it = live.lists.find(source);
                if (it != live.lists.end()) {
                    for (size_t k = 0; k < it->second.items.size(); ++k) {
                        if (!it->second.items[k].empty()) { // empty slots are not shown
                            put(it->second.items[k], int(k));
                        }
                    }
                }
                return out;
            }
            for (size_t k = 0; k < m.spec->items.size(); ++k) {
                put(m.spec->items[k], int(k));
            }
            return out;
        }
        const std::string tab = m.spec->tabs.empty() ? "all"
                                : m.spec->tabs[size_t(m.tab) % m.spec->tabs.size()];
        if (tab == "weapons") { // the weapons the player owns (the game mode's "owned_weapons")
            const auto it = live.lists.find("owned_weapons");
            if (it != live.lists.end()) {
                for (size_t k = 0; k < it->second.items.size(); ++k) {
                    put(it->second.items[k], int(k));
                }
            }
            return out;
        }
        const std::vector<UiInventoryEntry>& list = game ? live.inventory : layout.inventory;
        for (size_t k = 0; k < list.size(); ++k) {
            const UiItem* item = Item(list[k].item);
            if (tab == "all" || (item && item->use == tab)) {
                put(list[k].item, int(k));
            }
        }
        return out;
    }
    int PointerHand(const Menu& m) const {
        return m.spec->pointer >= 0 ? m.spec->pointer : 1 - m.spec->hand;
    }
    Stats& StatsOf(const Menu* m) { return stats[PresetName() + "/" + (m ? m->spec->name : "")]; }

    void Haptic(UiEvents& events, int hand, const UiPulse& pulse) {
        if (hand >= 0 && hand < 2 && pulse.amplitude > 0.0f && pulse.seconds > 0.0f) {
            events.haptics.push_back({hand, pulse.amplitude, pulse.seconds});
        }
    }

    void Write(JsonLine line) {
        if (!layout.log || log_failed) {
            return;
        }
        if (!log_file.is_open()) {
            std::error_code ec;
            const std::filesystem::path dir = assets / "ui_log";
            std::filesystem::create_directories(dir, ec);
            char stamp[32];
            const std::time_t t = std::time(nullptr);
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
            log_path = dir / (log_prefix + "_" + stamp + ".jsonl");
            log_file.open(log_path, std::ios::binary | std::ios::app);
            if (!log_file) {
                log_failed = true;
                return;
            }
        }
        log_file << line.Str() << '\n';
        log_file.flush();
    }
    JsonLine Event(const char* event, const Menu* m) const {
        JsonLine j;
        j.Add("t", now).Add("event", event).Add("preset", PresetName());
        if (m) {
            j.Add("menu", m->spec->name).Add("type", m->spec->type);
        }
        return j;
    }

    void Toast(const std::string& title, const std::string& body, double seconds = 6.0) {
        toast_title = title;
        toast_body = body;
        toast_at = now;
        toast_seconds = seconds;
    }
    void AddPopup(const std::string& text, Vec3 at) {
        // Beside what it is about (to the viewer's right, a little up), not over it.
        const Vec3 right = in.head.valid ? Rotate(in.head.rotation, {1, 0, 0}) : Vec3{1, 0, 0};
        popups.push_back({text, at + right * 0.06f + Vec3{0.0f, 0.02f, 0.0f}, now});
        if (popups.size() > 6) {
            popups.erase(popups.begin());
        }
    }

    void ResetCounts() {
        counts.clear();
        for (const UiInventoryEntry& e : game ? live.inventory : layout.inventory) {
            counts[Lower(Display(e.item))] += e.count;
        }
    }

    void Summary(std::string& log) {
        for (const auto& [key, s] : stats) {
            if (!s.opens && !s.selects && !s.grabs && !s.cancels) {
                continue;
            }
            char line[256];
            std::snprintf(line, sizeof(line),
                          "ui summary %s: %d opens, %d selects, %d cancels, %d grabs, %d empty, "
                          "mean %.0f ms to select\n",
                          key.c_str(), s.opens, s.selects, s.cancels, s.grabs, s.empties,
                          s.selects ? s.select_ms / s.selects : 0.0);
            log += line;
            Write(JsonLine()
                      .Add("t", now)
                      .Add("event", "summary")
                      .Add("key", key)
                      .Add("opens", s.opens)
                      .Add("selects", s.selects)
                      .Add("cancels", s.cancels)
                      .Add("grabs", s.grabs)
                      .Add("empty", s.empties)
                      .Add("mean_select_ms", s.selects ? s.select_ms / s.selects : 0.0));
        }
    }

    void Activate(int index, UiEvents* events, bool announce) {
        if (game) {
            // The game mode has one fixed set: the layout's game.active menus.
            preset = 0;
            menus.clear();
            popups.clear();
            for (const std::string& name : layout.game.active) {
                if (const UiMenuSpec* spec = layout.FindGameMenu(name)) {
                    Menu m;
                    m.spec = spec;
                    menus.push_back(m);
                }
            }
            BuildVocabulary();
            return;
        }
        if (layout.presets.empty()) {
            preset = -1;
            menus.clear();
            return;
        }
        const int count = int(layout.presets.size());
        index = ((index % count) + count) % count;
        const std::string previous = PresetName();
        preset = index;
        menus.clear();
        popups.clear();
        toast_at = -100.0;
        for (const std::string& name : layout.presets[size_t(index)].menus) {
            if (const UiMenuSpec* spec = layout.FindMenu(name)) {
                Menu m;
                m.spec = spec;
                menus.push_back(m);
            }
        }
        BuildVocabulary();
        if (announce && events) {
            Summary(events->log);
            const UiPreset& p = layout.presets[size_t(index)];
            Toast("Preset " + p.label, p.notes);
            events->log += "ui preset: " + p.label + " (" + p.name + "): " + p.notes + "\n";
            Haptic(*events, 0, layout.style.haptic_preset);
            Haptic(*events, 1, layout.style.haptic_preset);
            Write(Event("preset", nullptr).Add("previous", previous));
        }
    }

    // -- input --
    void Inputs(const UiInput& input) {
        std::copy(std::begin(held), std::end(held), std::begin(prev));
        for (size_t i = 1; i < size_t(UiInputId::Count); ++i) {
            const UiInputId id = UiInputId(i);
            if (Analog(id)) {
                const float v = AnalogValue(id, input);
                held[i] = held[i] ? v > kRelease : v > kPress;
            } else {
                held[i] = (input.buttons & ButtonBit(id)) != 0;
            }
        }
    }

    bool Ray(int hand, Vec3& from, Vec3& dir) const {
        if (hand < 0 || hand > 1 || !in.aim[hand].valid) {
            return false;
        }
        from = in.aim[hand].position;
        dir = Forward(in.aim[hand].rotation);
        return true;
    }
    void ShowPointer(int hand, Vec3 from, Vec3 to, bool hit, float distance) {
        Pointer& p = pointer[hand];
        if (!p.show || (hit && (!p.hit || distance < p.distance))) {
            p = {true, hit, from, to, hit ? distance : 1e9f};
        }
    }

    // -- actions --
    void SetHover(Menu& m, int index, const std::string& method, int hand, UiEvents& events,
                  const std::string& item) {
        if (index == m.hover) {
            if (index >= 0) {
                m.hover_method = method;
            }
            return;
        }
        m.hover = index;
        m.hover_method = method;
        m.hover_hand = hand;
        m.hover_since = now;
        if (index < 0) {
            return;
        }
        ++m.hover_changes;
        Haptic(events, hand, layout.style.haptic_hover);
        Write(Event("hover", &m)
                  .Add("item", item)
                  .Add("method", method)
                  .Add("hand", HandName(hand))
                  .Add("since_open_ms", (now - m.opened_at) * 1000.0));
    }

    /// Uses or equips an item; returns the line for the console. The choice also goes out as a
    /// UiAction (`source`, `index`: where it came from). In the game mode nothing is changed here:
    /// the game does it, and its live data shows the result.
    std::string Act(const std::string& name, const std::string& action, int hand, UiWorld& world,
                    UiEvents& events, Vec3 where, Menu* m, const UiPulse& pulse,
                    const std::string& source = std::string(), int index = -1) {
        const UiItem* item = Item(name);
        const std::string display = item ? item->name : Resolve(name);
        const bool equip = action == "equip" || source == "weapons" ||
                           (item && item->kind == "weapon");
        UiAction act;
        act.menu = m ? m->spec->name : std::string();
        act.item = Resolve(name);
        act.action = equip ? "equip" : "use";
        act.source = source;
        act.index = index;
        act.hand = hand;
        if (equip) {
            act.side = item && item->hand == "left" ? 0 : item && item->hand == "right" ? 1 : hand;
        }
        events.actions.push_back(act);
        if (game) {
            const int count = CountOf(name);
            if (!equip && count == 0) {
                Haptic(events, hand, layout.style.haptic_empty);
                AddPopup(display + ": none left", where);
                ++StatsOf(m).empties;
                Write(Event("empty", m).Add("item", display));
                return (equip ? "equip " : "use ") + display + ": none left";
            }
            Haptic(events, hand, pulse);
            AddPopup(display, where);
            return (equip ? "equip " : "use ") + display + " (the game does it)";
        }
        if (equip) {
            const int side = act.side;
            std::string& slot = world.weapon[side ? 1 : 0];
            std::string want = item && !item->model.empty() ? item->model : display;
            if (slot == want) {
                // The same text would not make the scene pick it again (B may have cycled
                // since): the other case of the same name does (the scene ignores case).
                std::string upper = want;
                for (char& c : upper) {
                    c = char(std::toupper(static_cast<unsigned char>(c)));
                }
                want = upper == want ? Lower(want) : upper;
            }
            slot = want;
            equipped[side] = display;
            Haptic(events, hand, pulse);
            AddPopup(display, where);
            return "equip " + display + " (" + HandName(side) + " hand)";
        }
        const std::string key = Lower(display);
        const auto it = counts.find(key);
        // Tools and key items (Notebook, Monocular, bells) are not used up.
        if (it == counts.end() || layout.infinite || (item && !item->Consumable())) {
            Haptic(events, hand, pulse);
            AddPopup(display, where);
            return "use " + display;
        }
        if (it->second <= 0) {
            Haptic(events, hand, layout.style.haptic_empty);
            AddPopup(display + ": none left", where);
            ++StatsOf(m).empties;
            Write(Event("empty", m).Add("item", display));
            return "use " + display + ": none left";
        }
        --it->second;
        Haptic(events, hand, pulse);
        AddPopup(display + "  " + std::to_string(it->second), where);
        return "use " + display + ", " + std::to_string(it->second) + " left";
    }

    void Select(Menu& m, int index, const std::string& method, int hand, UiWorld& world,
                UiEvents& events, Vec3 where) {
        std::vector<int> slots;
        std::vector<std::string> sources;
        const std::vector<std::string> items = Items(m, &slots, &sources);
        if (index < 0 || size_t(index) >= items.size()) {
            return;
        }
        const std::string& name = items[size_t(index)];
        const double ms = (now - m.opened_at) * 1000.0;
        const std::string what = Act(name, m.spec->action, hand, world, events, where, &m,
                                     layout.style.haptic_select, sources[size_t(index)],
                                     slots[size_t(index)]);
        Stats& s = StatsOf(&m);
        ++s.selects;
        s.select_ms += ms;
        Write(Event("select", &m)
                  .Add("item", Display(name))
                  .Add("method", method)
                  .Add("hand", HandName(hand))
                  .Add("action", m.spec->action)
                  .Add("since_open_ms", ms)
                  .Add("hover_changes", m.hover_changes)
                  .Add("count_left", CountOf(name)));
        char line[320];
        std::snprintf(line, sizeof(line), "ui [%s] %s: %s by %s, %.0f ms after opening, %d hovers\n",
                      PresetName().c_str(), m.spec->name.c_str(), what.c_str(), method.c_str(), ms,
                      m.hover_changes);
        events.log += line;
    }

    void Open(Menu& m, UiEvents& events) {
        m.open = true;
        m.opened_at = now;
        m.hover = -1;
        m.hover_changes = 0;
        m.hover_method.clear();
        m.page = 0;
        ++StatsOf(&m).opens;
        Haptic(events, m.spec->hand, layout.style.haptic_open);
        Write(Event("open", &m).Add("hand", HandName(m.spec->hand)).Add("binding", m.spec->open.text));
    }

    void Close(Menu& m, const char* reason) {
        if (!m.open) {
            return;
        }
        m.open = false;
        m.remove_mode = false;
        if (std::string(reason) == "cancel") {
            ++StatsOf(&m).cancels;
        }
        Write(Event(reason, &m)
                  .Add("since_open_ms", (now - m.opened_at) * 1000.0)
                  .Add("hover_changes", m.hover_changes));
    }

    // -- element hit tests (grid, wrist) --
    const Element* ElementAt(const Menu& m, float u, float v) const {
        for (const Element& e : m.elements) {
            if (u >= e.x0 && u <= e.x1 && v >= e.y0 && v <= e.y1) {
                return &e;
            }
        }
        return nullptr;
    }

    /// Pointing and touching on a panel menu (its elements are from the last Build). Returns the
    /// element id hovered (-1 none) and sets `clicked` when it was selected this frame.
    int PanelInteract(Menu& m, const UiPanel& panel, bool& clicked, std::string& method,
                      int& by_hand) {
        clicked = false;
        int hovered = -1;
        if (Has(m.spec->select, "point")) {
            const int ph = PointerHand(m);
            Vec3 from, dir;
            if (Ray(ph, from, dir)) {
                float u, v, dist;
                if (panel.Hit(from, dir, u, v, dist) && dist < 4.0f) {
                    const Element* e = ElementAt(m, u, v);
                    const bool on_panel = std::fabs(u) < m.half_w && std::fabs(v) < m.half_h;
                    if (e || on_panel) {
                        ShowPointer(ph, from, from + dir * dist, true, dist);
                    }
                    if (e) {
                        hovered = e->id;
                        method = "point";
                        by_hand = ph;
                        if (Pressed(m.spec->click)) {
                            clicked = true;
                        }
                    }
                } else {
                    ShowPointer(ph, from, from + dir * 1.2f, false, 1e9f);
                }
            }
        }
        if (Has(m.spec->select, "touch")) {
            for (int h = 0; h < 2; ++h) {
                if (!tip_valid[h] || (m.spec->type == "wrist" && h == m.spec->hand)) {
                    continue;
                }
                float u, v, height;
                panel.Local(tip[h], u, v, height);
                const Element* e = ElementAt(m, u, v);
                const float before = m.touch_height[h];
                m.touch_height[h] = height;
                if (e && height < kTouchDepth && height > -0.05f) {
                    hovered = e->id;
                    method = "touch";
                    by_hand = h;
                    if (before > kPokeAt && height <= kPokeAt) {
                        clicked = true;
                    }
                }
            }
        }
        return hovered;
    }

    // -- menus --
    /// Where a radial stands: on the hand's fingertip when that hand touches it (so the first
    /// move sideways picks), else at the grip; `forward` away from the head and `up` on top.
    /// Placed once at open ("anchor": "world", the default); "hand" re-places it every frame.
    void PlaceRadial(Menu& m) {
        const UiMenuSpec& s = *m.spec;
        const int hand = s.hand;
        const bool tip_here = Has(s.select, "touch") && PointerHand(m) == hand && tip_valid[hand];
        const Vec3 at = tip_here ? tip[hand] : in.grip[hand].position;
        const Vec3 away = Normalize(at - in.head.position, Forward(in.head.rotation));
        m.base = UiPanel::Facing(at + away * s.forward + Vec3{0.0f, s.up, 0.0f}, in.head.position,
                                 {0, 1, 0});
    }

    /// The pointer hand's index fingertip in a radial's button volume: hovers the segment under it
    /// (a few cm in front of the ring, or behind it); `poked` when it is pushed through.
    void TouchRadial(Menu& m, int n, int& candidate, std::string& method, int& by, bool& poked) {
        const UiMenuSpec& s = *m.spec;
        const int th = PointerHand(m);
        if (!tip_valid[th]) {
            return;
        }
        float u, v, height;
        m.base.Local(tip[th], u, v, height);
        const float r = std::hypot(u, v);
        const float before = m.touch_height[th];
        m.touch_height[th] = height;
        const bool same_hand = th == s.hand;
        const float back = same_hand ? -0.07f : -kTouchDepth; // same hand: room to push through
        if (height < kTouchDepth && height > back && r > s.inner_radius && r < s.radius * 1.2f) {
            candidate = SegmentAt(std::atan2(v, u), n);
            method = "touch";
            by = th;
            poked = same_hand && before > -kRadialPoke && height <= -kRadialPoke;
        }
    }

    void UpdateRadial(Menu& m, UiWorld& world, UiEvents& events) {
        const UiMenuSpec& s = *m.spec;
        const int hand = s.hand;
        const std::vector<std::string> items = Items(m);
        const int n = int(items.size());
        if (!m.open) {
            if (Pressed(s.open) && !(PressedExact(s.open) && n > 0 && in.grip[hand].valid && in.head.valid)) {
                char why[200];
                std::snprintf(why, sizeof(why), "ui [%s] %s: %s pressed, not opened (items %d, grip %d, head %d, other modifier held %d)\n",
                              PresetName().c_str(), s.name.c_str(), s.open.text.c_str(), n, int(in.grip[hand].valid),
                              int(in.head.valid), int(!PressedExact(s.open)));
                events.log += why;
            }
            if (PressedExact(s.open) && n > 0 && in.grip[hand].valid && in.head.valid) {
                PlaceRadial(m);
                m.open_hand = in.grip[hand].position;
                Open(m, events);
                // The wheel on the left hand shares its stick with walking: a stick still pushed from
                // walking must not pick (and pin) a segment before it has been released once.
                m.stick_armed = std::hypot(in.stick[hand][0], in.stick[hand][1]) <= s.deadzone;
            }
            return;
        }
        if (s.anchor == "hand" && in.grip[hand].valid && in.head.valid) {
            PlaceRadial(m);
        }
        // Candidates, in priority order: stick, pointing, touching, moving the hand. A touch listed
        // before point wins over it (the same hand both touches and points).
        const auto Index = [&](const char* k) {
            for (size_t i = 0; i < s.select.size(); ++i) {
                if (s.select[i] == k) {
                    return int(i);
                }
            }
            return 1 << 20;
        };
        const bool touch_first = Index("touch") < Index("point");
        bool poked = false;
        int candidate = -1, by = hand;
        std::string method;
        if (Has(s.select, "stick")) {
            const float x = in.stick[hand][0], y = in.stick[hand][1];
            if (std::hypot(x, y) <= s.deadzone) {
                m.stick_armed = true;
            } else if (m.stick_armed) {
                candidate = SegmentAt(std::atan2(y, x), n);
                method = "stick";
            }
        }
        if (candidate < 0 && Has(s.select, "touch") && touch_first) {
            TouchRadial(m, n, candidate, method, by, poked);
        }
        if (candidate < 0 && Has(s.select, "point")) {
            const int ph = PointerHand(m);
            Vec3 from, dir;
            if (Ray(ph, from, dir)) {
                float u, v, dist;
                if (m.base.Hit(from, dir, u, v, dist) && dist < 3.0f &&
                    std::hypot(u, v) < s.radius * 1.4f) {
                    ShowPointer(ph, from, from + dir * dist, true, dist);
                    if (std::hypot(u, v) > s.inner_radius * 0.6f) {
                        candidate = SegmentAt(std::atan2(v, u), n);
                        method = "point";
                        by = ph;
                    } else if (m.hover >= 0 && m.hover_method == "point") {
                        SetHover(m, -1, "point", ph, events, ""); // the center cancels
                    }
                } else {
                    ShowPointer(ph, from, from + dir * 1.0f, false, 1e9f);
                }
            }
        }
        if (candidate < 0 && Has(s.select, "touch") && !touch_first) {
            TouchRadial(m, n, candidate, method, by, poked);
        }
        if (candidate < 0 && Has(s.select, "move") && in.grip[hand].valid) {
            const Vec3 d = in.grip[hand].position - m.open_hand;
            const float u = Dot(d, m.base.x), v = Dot(d, m.base.y);
            if (std::hypot(u, v) > s.move_threshold) {
                candidate = SegmentAt(std::atan2(v, u), n);
                method = "move";
            }
        }
        if (candidate >= 0) { // sticky: the last item stays picked when the stick recenters
            SetHover(m, candidate, method, by, events, Display(items[size_t(candidate)]));
        }
        const Vec3 where = m.base.At(0.0f, -s.radius - 0.05f);
        if (poked && m.hover >= 0) { // pushed through the wheel at the hovered item
            Select(m, m.hover, "touch", PointerHand(m), world, events, where);
            Close(m, "close");
            return;
        }
        if (m.hover >= 0 && m.hover_method == "point" && Pressed(s.click)) {
            Select(m, m.hover, "point", PointerHand(m), world, events, where);
            Close(m, "close");
            return;
        }
        if (m.hover >= 0 && Pressed(s.confirm)) { // the confirm press: whatever picked it
            Select(m, m.hover, m.hover_method,
                   m.hover_method == "point" || m.hover_method == "touch" ? PointerHand(m) : hand,
                   world, events, where);
            Close(m, "close");
            return;
        }
        const bool toggle = s.mode == "toggle";
        const bool done = toggle ? Pressed(s.open) : !Held(s.open.key);
        if (done) {
            if (m.hover >= 0 && s.use_on_release) {
                Select(m, m.hover, m.hover_method, m.hover_method == "point" ? PointerHand(m) : hand,
                       world, events, where);
                Close(m, "close");
            } else {
                Close(m, "cancel");
            }
        }
    }

    void UpdateGrid(Menu& m, UiWorld& world, UiEvents& events) {
        const UiMenuSpec& s = *m.spec;
        if (m.open ? Pressed(s.open) : PressedExact(s.open)) {
            if (m.open) {
                Close(m, "close");
                return;
            }
            if (!in.head.valid) {
                return;
            }
            m.base = UiGridPanel(s, in.head);
            Open(m, events);
            m.flick_armed = m.flick_armed_y = false;
            m.following = false;
            return;
        }
        if (!m.open) {
            return;
        }
        // Head-anchored grids follow the head; lazy ones catch up once you turn away.
        if (s.anchor == "head" && in.head.valid) {
            m.base = UiGridPanel(s, in.head);
        } else if (s.anchor == "lazy" && in.head.valid) {
            const UiPanel want = UiGridPanel(s, in.head);
            const Vec3 have_dir = Flat(m.base.origin - in.head.position, {0, 0, -1});
            const Vec3 want_dir = Flat(want.origin - in.head.position, {0, 0, -1});
            if (Dot(have_dir, want_dir) < std::cos(25.0f * kDeg) ||
                Length(m.base.origin - want.origin) > 0.3f * s.distance) {
                m.following = true;
            }
            if (m.following) {
                const float k = 1.0f - float(std::exp(-dt / 0.18));
                m.base.origin = Lerp(m.base.origin, want.origin, k);
                m.base.z = Normalize(Lerp(m.base.z, want.z, k), want.z);
                m.base.x = Normalize(Cross({0, 1, 0}, m.base.z), want.x);
                m.base.y = Cross(m.base.z, m.base.x);
                m.following = Length(m.base.origin - want.origin) > 0.01f;
            }
        }
        const std::vector<std::string> items = Items(m);
        const int per_page = s.columns * s.rows;
        const int pages = std::max(1, (int(items.size()) + per_page - 1) / per_page);
        // Stick flicks turn pages.
        if (s.page_stick >= 0) {
            const float x = in.stick[s.page_stick][0], y = in.stick[s.page_stick][1];
            if (m.flick_armed && std::fabs(x) > 0.7f) {
                m.flick_armed = false;
                const int next = std::clamp(m.page + (x > 0 ? 1 : -1), 0, pages - 1);
                if (next != m.page) {
                    m.page = next;
                    Haptic(events, s.page_stick, layout.style.haptic_hover);
                    Write(Event("page", &m).Add("page", m.page + 1).Add("method", "stick"));
                }
            } else if (std::fabs(x) < 0.3f) {
                m.flick_armed = true;
            }
            if (!s.tabs.empty() && m.flick_armed_y && std::fabs(y) > 0.7f) {
                m.flick_armed_y = false;
                const int n = int(s.tabs.size());
                m.tab = ((m.tab + (y < 0 ? 1 : -1)) % n + n) % n;
                m.page = 0;
                Haptic(events, s.page_stick, layout.style.haptic_hover);
                Write(Event("tab", &m).Add("tab", s.tabs[size_t(m.tab)]).Add("method", "stick"));
            } else if (std::fabs(y) < 0.3f) {
                m.flick_armed_y = true;
            }
        }
        bool clicked = false;
        std::string method;
        int by = PointerHand(m);
        const int id = PanelInteract(m, m.base, clicked, method, by);
        std::string item = id >= 0 && size_t(id) < items.size() ? Display(items[size_t(id)]) : "";
        SetHover(m, id, method, by, events, id >= 0 ? item : "");
        if (!clicked) {
            return;
        }
        if (id == kClose) {
            Haptic(events, by, layout.style.haptic_select);
            Close(m, "close");
        } else if (id <= kTab && size_t(kTab - id) < s.tabs.size()) {
            m.tab = kTab - id;
            m.page = 0;
            Haptic(events, by, layout.style.haptic_select);
            Write(Event("tab", &m).Add("tab", s.tabs[size_t(m.tab)]).Add("method", method));
        } else if (id == kPrev || id == kNext) {
            const int next = std::clamp(m.page + (id == kNext ? 1 : -1), 0, pages - 1);
            if (next != m.page) {
                m.page = next;
                Haptic(events, by, layout.style.haptic_select);
                Write(Event("page", &m).Add("page", m.page + 1).Add("method", method));
            }
        } else if (id >= 0) {
            float u = 0.0f, v = 0.0f;
            for (const Element& e : m.elements) {
                if (e.id == id) {
                    u = 0.5f * (e.x0 + e.x1);
                    v = 0.5f * (e.y0 + e.y1);
                }
            }
            Select(m, id, method, by, world, events, m.base.At(u, v) + m.base.z * 0.03f);
        }
    }

    void UpdateWrist(Menu& m, UiWorld& world, UiEvents& events) {
        const UiMenuSpec& s = *m.spec;
        const int hand = s.hand;
        bool show = false;
        if (in.grip[hand].valid && in.head.valid) {
            const Quat q = in.grip[hand].rotation;
            const Vec3 center = in.grip[hand].position + Rotate(q, s.offset);
            const Vec3 normal = Rotate(q, AxisVec(s.normal));
            const Vec3 to_head = Normalize(in.head.position - center, normal);
            const float slack = m.open ? 8.0f * kDeg : 0.0f; // hysteresis
            const bool glance =
                Dot(normal, to_head) > std::cos(s.glance_deg * kDeg + slack) &&
                Dot(Forward(in.head.rotation), -to_head) > std::cos(s.view_deg * kDeg + slack);
            const bool hold = s.open.Empty() || HeldAll(s.open);
            show = s.show == "always" || (s.show == "glance" && glance) ||
                   (s.show == "hold" && !s.open.Empty() && hold) ||
                   (s.show == "glance+hold" && glance && hold);
            // Readable upright: the panel's up is the head's up, flattened onto the panel.
            UiPanel p;
            p.origin = center;
            p.z = normal;
            const Vec3 head_up = Rotate(in.head.rotation, {0, 1, 0});
            Vec3 up = head_up - normal * Dot(head_up, normal);
            if (Length(up) < 0.2f) {
                const Vec3 alt = Forward(in.head.rotation);
                up = alt - normal * Dot(alt, normal);
            }
            p.y = Normalize(up, AnyPerp(normal));
            p.x = Cross(p.y, p.z);
            m.base = p;
        }
        if (show && !m.open) {
            m.open = true;
            m.opened_at = now;
            m.hover = -1;
            m.hover_changes = 0;
            ++StatsOf(&m).opens;
            Write(Event("glance", &m).Add("hand", HandName(hand)));
        } else if (!show && m.open) {
            m.open = false;
            Write(Event("glance_end", &m).Add("shown_ms", (now - m.opened_at) * 1000.0));
        }
        if (!m.open || s.select.empty()) {
            return;
        }
        bool clicked = false;
        std::string method;
        int by = PointerHand(m);
        const std::vector<std::string> items = Items(m);
        const int id = PanelInteract(m, m.base, clicked, method, by);
        SetHover(m, id, method, by, events,
                 id >= 0 && size_t(id) < items.size() ? Display(items[size_t(id)]) : "");
        if (clicked && id >= 0) {
            Select(m, id, method, by, world, events, m.base.At(0.0f, 0.05f));
        }
    }

    void UpdateSlots(Menu& m, UiWorld& world, UiEvents& events) {
        const UiMenuSpec& s = *m.spec;
        m.open = true;
        if (!in.head.valid) {
            return;
        }
        const float head_height = in.head.position.y - in.floor_y;
        if (s.posture == "seated") {
            m.seated = true;
        } else if (s.posture == "standing") {
            m.seated = false;
        } else {
            const float drop = eye_height - head_height;
            m.seated = m.seated ? drop > s.seated_drop - 0.04f : drop > s.seated_drop + 0.04f;
        }
        const Vec3 neck = in.head.position + Rotate(in.head.rotation, s.neck);
        const Quat yaw = AxisAngle({0, 1, 0}, body_yaw);
        m.base.origin = neck;
        m.slot_world.resize(s.slots.size());
        for (size_t i = 0; i < s.slots.size(); ++i) {
            m.slot_world[i] = neck + Rotate(yaw, m.seated ? s.slots[i].seated : s.slots[i].standing);
        }
        for (int h = 0; h < 2; ++h) {
            int best = -1;
            float best_d = 1e9f;
            if (in.grip[h].valid) {
                for (size_t i = 0; i < s.slots.size(); ++i) {
                    const UiBodySlot& slot = s.slots[i];
                    if ((slot.hands == "left" && h != 0) || (slot.hands == "right" && h != 1)) {
                        continue;
                    }
                    const float r = slot.radius > 0.0f ? slot.radius : s.grab_radius;
                    const float d = Length(in.grip[h].position - m.slot_world[i]);
                    if (d < r && d < best_d) {
                        best = int(i);
                        best_d = d;
                    }
                }
            }
            const int before = m.slot_hover[h];
            if (best != m.slot_hover[h]) {
                m.slot_hover[h] = best;
                m.slot_since[h] = now;
                if (best >= 0) {
                    Haptic(events, h, layout.style.haptic_hover);
                    Write(Event("hover", &m)
                              .Add("slot", s.slots[size_t(best)].name)
                              .Add("item", Display(s.slots[size_t(best)].item))
                              .Add("hand", HandName(h))
                              .Add("method", "reach"));
                }
            }
            const UiInputId grip = h ? UiInputId::RightGrip : UiInputId::LeftGrip;
            // A grip squeezed before the hand got there takes nothing: the press must come while
            // the hand is on the slot (a hand closed around a weapon sweeping past is no grab).
            if (best >= 0 && best == before && PressedKey(grip)) {
                const UiBodySlot& slot = s.slots[size_t(best)];
                const double ms = (now - m.slot_since[h]) * 1000.0;
                const std::string what = Act(slot.item, slot.action, h, world, events,
                                             m.slot_world[size_t(best)], &m,
                                             layout.style.haptic_grab, std::string(), best);
                Stats& st = StatsOf(&m);
                ++st.grabs;
                ++st.selects;
                st.select_ms += ms;
                m.flash_slot = best;
                m.flash_at = now;
                Write(Event("grab", &m)
                          .Add("slot", slot.name)
                          .Add("item", Display(slot.item))
                          .Add("hand", HandName(h))
                          .Add("action", slot.action)
                          .Add("posture", m.seated ? "seated" : "standing")
                          .Add("since_hover_ms", ms)
                          .Add("count_left", CountOf(slot.item)));
                char line[320];
                std::snprintf(line, sizeof(line), "ui [%s] %s %s: %s with the %s hand, %.0f ms after reaching it (%s)\n",
                              PresetName().c_str(), s.name.c_str(), slot.name.c_str(), what.c_str(),
                              HandName(h), ms, m.seated ? "seated" : "standing");
                events.log += line;
            }
        }
    }

    // -- the dashboard (the game's one panel) --
    /// The quick slot holding an item (-1 none).
    int QuickSlotOf(const std::string& key) const {
        const auto it = live.lists.find("quick");
        if (it == live.lists.end() || key.empty()) {
            return -1;
        }
        for (size_t k = 0; k < it->second.items.size(); ++k) {
            if (it->second.items[k] == key) {
                return int(k);
            }
        }
        return -1;
    }
    int Selected(const char* key) const {
        const auto it = live.selected.find(key);
        return it == live.selected.end() ? -1 : it->second;
    }
    std::string Disabled(const std::string& key) const {
        const auto it = live.disabled.find(key);
        return it == live.disabled.end() ? std::string() : it->second;
    }
    void Emit(UiEvents& events, Menu& m, const std::string& action, const std::string& item,
              const std::string& source, int index, int hand, int side = -1) {
        UiAction a;
        a.side = side;
        a.menu = m.spec->name;
        a.item = item;
        a.action = action;
        a.source = source;
        a.index = index;
        a.hand = hand;
        events.actions.push_back(a);
        ++StatsOf(&m).selects;
        Write(Event("select", &m)
                  .Add("item", item.empty() ? action : Display(item))
                  .Add("action", action)
                  .Add("hand", HandName(hand))
                  .Add("since_open_ms", (now - m.opened_at) * 1000.0));
        events.log += "ui [game] " + m.spec->name + ": " + action + " " +
                      (item.empty() ? std::string() : Display(item)) + "\n";
    }

    void UpdateDashboard(Menu& m, UiWorld&, UiEvents& events) {
        const UiMenuSpec& s = *m.spec;
        if (m.open ? Pressed(s.open) : PressedExact(s.open)) {
            if (m.open) {
                Close(m, "close");
                return;
            }
            if (!in.head.valid) {
                return;
            }
            m.base = UiGridPanel(s, in.head); // world-locked where it opened
            Open(m, events);
            m.tab = 0;
            return;
        }
        if (!m.open) {
            return;
        }
        // Both hands point; the hand whose ray is on the panel clicks with its trigger.
        int hovered = -1, by = 1;
        bool clicked = false;
        for (int h = 1; h >= 0; --h) {
            Vec3 from, dir;
            if (!Ray(h, from, dir)) {
                continue;
            }
            float u, v, dist;
            if (m.base.Hit(from, dir, u, v, dist) && dist < 4.0f && std::fabs(u) < m.half_w &&
                std::fabs(v) < m.half_h) {
                ShowPointer(h, from, from + dir * dist, true, dist);
                const Element* e = ElementAt(m, u, v);
                const bool pulled =
                    PressedKey(h ? UiInputId::RightTrigger : UiInputId::LeftTrigger);
                // The hand that pulls wins; else the right hand's hover shows.
                if (e && (hovered < 0 || (pulled && !clicked))) {
                    hovered = e->id;
                    by = h;
                    clicked = pulled;
                }
            } else {
                ShowPointer(h, from, from + dir * 1.2f, false, 1e9f);
            }
        }
        if (hovered != m.hover) {
            m.hover = hovered;
            m.hover_hand = by;
            if (hovered >= 0) {
                ++m.hover_changes;
                Haptic(events, by, layout.style.haptic_hover);
            }
        }
        if (!clicked || hovered < 0) {
            return;
        }
        const auto info = m.dash.find(hovered);
        if (info != m.dash.end() && !info->second.enabled) {
            Haptic(events, by, layout.style.haptic_empty);
            Toast(info->second.title + ": not available", info->second.reason, 4.0);
            Write(Event("disabled", &m).Add("what", info->second.title).Add("reason", info->second.reason));
            return;
        }
        Haptic(events, by, layout.style.haptic_select);
        const auto weapons = live.lists.find("weapons");
        const auto quick = live.lists.find("quick");
        if (hovered >= kDashItem) {
            std::vector<int> index;
            const std::vector<std::string> items = Items(m, &index);
            const size_t k = size_t(hovered - kDashItem);
            if (k < items.size()) {
                const std::string key = Resolve(items[k]);
                const UiItem* item = Item(key);
                const int slot = QuickSlotOf(key);
                if (item && item->kind == "weapon") {
                    // Into the hand it belongs to, the slot held now (the game's equip command).
                    const int side = item->hand == "left" ? 0 : 1;
                    Emit(events, m, "equip_item", key, "inventory", int(k), by, side);
                } else if (slot >= 0 && m.remove_mode) {
                    m.remove_mode = false; // one at a time
                    Emit(events, m, "clear_quick", key, "quick", slot, by);
                } else if (slot >= 0) {
                    Emit(events, m, m.use_mode ? "use" : "select", key, "quick", slot, by);
                    if (m.use_mode) {
                        Close(m, "close"); // see it happen
                    }
                } else {
                    // Into a quick slot first (the game's command), then used in USE mode.
                    Emit(events, m, m.use_mode ? "assign_use" : "assign_quick", key, "inventory",
                         int(k), by);
                    if (m.use_mode) {
                        Close(m, "close");
                    }
                }
            }
        } else if (hovered >= kDashWeapon && hovered < kDashWeapon + 4 && weapons != live.lists.end()) {
            const int k = hovered - kDashWeapon;
            if (size_t(k) < weapons->second.items.size() && !weapons->second.items[size_t(k)].empty()) {
                Emit(events, m, "equip", weapons->second.items[size_t(k)], "weapons", k, by);
            }
        } else if (hovered >= kDashQuick && hovered < kDashQuick + 16 && quick != live.lists.end()) {
            const int k = hovered - kDashQuick;
            if (size_t(k) < quick->second.items.size() && !quick->second.items[size_t(k)].empty() &&
                m.remove_mode) {
                m.remove_mode = false; // one at a time
                Emit(events, m, "clear_quick", quick->second.items[size_t(k)], "quick", k, by);
            } else if (size_t(k) < quick->second.items.size() && !quick->second.items[size_t(k)].empty()) {
                Emit(events, m, m.use_mode ? "use" : "select", quick->second.items[size_t(k)], "quick", k, by);
                if (m.use_mode) {
                    Close(m, "close");
                }
            }
        } else if (hovered >= kDashTab && hovered < kDashTab + 32) {
            m.tab = hovered - kDashTab;
            m.page = 0;
        } else if (hovered == kDashPrev || hovered == kDashNext) {
            m.page = std::max(0, m.page + (hovered == kDashNext ? 1 : -1)); // clamped when drawn
        } else if (hovered == kDashMode) {
            m.use_mode = !m.use_mode;
        } else if (hovered == kDashRemove) {
            m.remove_mode = !m.remove_mode;
        } else if (hovered == kDashClose) {
            Close(m, "close");
        } else if (hovered >= kDashButton && hovered < kDashButton + 32 &&
                   size_t(hovered - kDashButton) < s.buttons.size()) {
            const std::string& action = s.buttons[size_t(hovered - kDashButton)].action;
            if (action == "close") {
                Close(m, "close");
            } else if (action == "mode") {
                m.use_mode = !m.use_mode;
            } else {
                Emit(events, m, action, std::string(), std::string(), hovered - kDashButton, by);
                // The game's own menus (pause, gestures) take over: the dashboard gets out of the way.
                if (action == "press:options" || action == "press:touchpad") {
                    Close(m, "close");
                }
            }
        }
    }

    // -- drawing --
    std::vector<std::string> Wrap(const std::string& text, float size, float width) const {
        std::vector<std::string> lines;
        std::istringstream words(text);
        std::string word, line;
        while (words >> word) {
            const std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && canvas.TextWidth(next, size) > width) {
                lines.push_back(line);
                line = word;
            } else {
                line = next;
            }
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
        return lines;
    }

    float Baseline(float cy, float size) const { return cy - 0.30f * size; }

    void ItemCard(UiCanvas& c, const UiPanel& p, float cx, float cy, float size,
                  const std::string& name, bool hot, bool frame) {
        const UiStyle& st = layout.style;
        if (frame) {
            c.Frame(p, cx, cy, size, size, hot ? st.highlight : st.slot,
                    hot ? st.highlight_border : st.slot_border, hot ? 0.0018f : 0.0012f,
                    size * 0.08f);
        }
        const float icon = size * (hot ? 0.9f : 0.84f);
        if (!c.Icon(p, cx, cy + size * 0.02f, icon, IconOf(name), {1, 1, 1, 1})) {
            c.Text(p, cx, Baseline(cy, size * 0.18f), size * 0.18f, Display(name), st.text,
                   UiAlign::Center, size * 0.9f);
        }
        const int count = CountOf(name);
        if (count >= 0) {
            const float ts = size * 0.26f;
            c.Text(p, cx + size * 0.44f, cy - size * 0.44f + ts * 0.1f, ts, std::to_string(count),
                   count > 0 ? st.text : st.accent, UiAlign::Right);
        }
    }

    void DrawRadial(Menu& m, UiCanvas& c) {
        const UiMenuSpec& s = *m.spec;
        const UiStyle& st = layout.style;
        const std::vector<std::string> items = Items(m);
        const int n = int(items.size());
        if (!n) {
            return;
        }
        UiPanel p = m.base;
        p.scale = 0.55f + 0.45f * Ease(m.anim);
        c.alpha = Ease(m.anim);
        c.on_top = s.on_top;
        const float half = kPi / float(n);
        c.Disc(p, 0, 0, s.radius + 0.014f, st.panel.WithAlpha(0.6f));
        for (int k = 0; k < n; ++k) {
            const float angle = SegmentAngle(k, n);
            const bool hot = k == m.hover;
            if (hot) {
                c.Sector(p, 0, 0, angle, half, s.inner_radius - 0.003f, s.radius + 0.008f,
                         st.highlight_border, std::max(s.gap - 0.004f, 0.0f));
            }
            c.Sector(p, 0, 0, angle, half, s.inner_radius, s.radius + (hot ? 0.005f : 0.0f),
                     hot ? st.highlight : st.slot, s.gap);
            const float r = 0.5f * (s.inner_radius + s.radius);
            const float x = r * std::cos(angle), y = r * std::sin(angle);
            ItemCard(c, p, x, y, s.item_size * (hot ? 1.18f : 1.0f), items[size_t(k)], hot, false);
        }
        // Center: which way is picked.
        c.Disc(p, 0, 0, s.inner_radius - s.gap, st.panel);
        if (m.hover >= 0) {
            const float a = SegmentAngle(m.hover, n);
            c.Sector(p, 0, 0, a, half * 0.6f, s.inner_radius * 0.55f, s.inner_radius - s.gap * 1.5f,
                     st.highlight_border);
        }
        // Name plate under the ring.
        const std::string title = m.hover >= 0 ? Display(items[size_t(m.hover)]) : s.label;
        const float size = 0.02f;
        const float plate_y = -s.radius - 0.032f;
        const float w = std::min(std::max(c.TextWidth(title, size) + 0.03f, 0.09f), 2.2f * s.radius);
        c.Frame(p, 0, plate_y, w, 0.034f, st.panel, st.panel_border, 0.0015f, 0.008f);
        c.Text(p, 0, Baseline(plate_y, size), size, title, st.text, UiAlign::Center, w - 0.02f);
        if (m.hover >= 0) {
            const int count = CountOf(items[size_t(m.hover)]);
            if (count >= 0) {
                c.Text(p, 0, Baseline(plate_y - 0.03f, 0.014f), 0.014f,
                       std::to_string(count) + " left", st.text_dim, UiAlign::Center);
            }
        }
        if (s.action == "equip") {
            // The weapons held now, over the ring.
            const float iy = s.radius + 0.035f;
            for (int h = 0; h < 2; ++h) {
                if (equipped[h].empty()) {
                    continue;
                }
                const float x = h ? 0.035f : -0.035f;
                c.Frame(p, x, iy, 0.05f, 0.05f, st.slot, st.slot_border, 0.001f, 0.005f);
                c.Icon(p, x, iy, 0.046f, IconOf(equipped[h]), {1, 1, 1, 1});
                c.Text(p, x, iy + 0.032f, 0.011f, h ? "right" : "left", st.text_dim,
                       UiAlign::Center);
            }
        }
    }

    void DrawGrid(Menu& m, UiCanvas& c) {
        const UiMenuSpec& s = *m.spec;
        const UiStyle& st = layout.style;
        const std::vector<std::string> items = Items(m);
        const GridMetrics g = GridOf(s);
        const float u = g.unit, pad = g.pad, footer = g.footer, title = g.title;
        const float cell = s.cell;
        const float W = g.width, H = g.height;
        UiPanel p = m.base;
        p.scale = 0.8f + 0.2f * Ease(m.anim);
        c.alpha = Ease(m.anim);
        c.on_top = s.on_top;
        m.elements.clear();
        m.half_w = W * 0.5f;
        m.half_h = H * 0.5f;
        c.Frame(p, 0, 0, W, H, st.panel, st.panel_border, 0.0025f * u, 0.012f * u);
        const int per_page = s.columns * s.rows;
        const int pages = std::max(1, (int(items.size()) + per_page - 1) / per_page);
        m.page = std::clamp(m.page, 0, pages - 1);
        // Title bar: label, page arrows, close.
        const float ty = H * 0.5f - pad - title * 0.5f;
        c.Text(p, -W * 0.5f + pad + 0.004f * u, Baseline(ty, 0.028f * u), 0.028f * u, s.label,
               st.text);
        const float bx = W * 0.5f - pad - 0.02f * u;
        const auto button = [&](int id, float x, float y, float w, const std::string& text,
                                bool selected) {
            const bool hot = m.hover == id;
            const float h = 0.032f * u;
            c.Frame(p, x, y, w, h, hot || selected ? st.highlight : st.slot,
                    hot ? st.highlight_border : selected ? st.panel_border : st.slot_border,
                    0.0012f * u, 0.006f * u);
            c.Text(p, x, Baseline(y, 0.02f * u), 0.02f * u, text, selected ? st.text : st.text_dim,
                   UiAlign::Center, w - 0.006f * u);
            m.elements.push_back({id, x - w * 0.5f, y - h * 0.5f, x + w * 0.5f, y + h * 0.5f});
        };
        button(kClose, bx, ty, 0.036f * u, "x", false);
        button(kNext, bx - 0.052f * u, ty, 0.036f * u, ">", false);
        c.Text(p, bx - 0.1f * u, Baseline(ty, 0.018f * u), 0.018f * u,
               std::to_string(m.page + 1) + " / " + std::to_string(pages), st.text_dim,
               UiAlign::Center, 0.032f * u);
        button(kPrev, bx - 0.148f * u, ty, 0.036f * u, "<", false);
        // Tabs: filters by use.
        const int tabs = int(s.tabs.size());
        for (int i = 0; i < tabs; ++i) {
            const float tw = (W - 2.0f * pad - float(tabs - 1) * g.gap) / float(tabs);
            float x, y;
            UiGridTab(s, i, x, y);
            button(kTab - i, x, y, tw, TabLabel(s.tabs[size_t(i)]), i == m.tab);
        }
        // Cards.
        for (int r = 0; r < s.rows; ++r) {
            for (int k = 0; k < s.columns; ++k) {
                const int index = m.page * per_page + r * s.columns + k;
                float x, y;
                UiGridCard(s, r * s.columns + k, x, y);
                if (size_t(index) >= items.size()) {
                    c.Frame(p, x, y, cell, cell, st.slot.WithAlpha(0.35f),
                            st.slot_border.WithAlpha(0.35f), 0.001f * u, cell * 0.08f);
                    continue;
                }
                ItemCard(c, p, x, y, cell, items[size_t(index)], m.hover == index, true);
                m.elements.push_back({index, x - cell * 0.5f, y - cell * 0.5f, x + cell * 0.5f,
                                      y + cell * 0.5f});
            }
        }
        // Footer: the hovered item's name, effect (doc 17's catalog) and description.
        const float fy = -H * 0.5f + pad + footer * 0.5f;
        const float left = -W * 0.5f + pad + 0.004f * u, line_w = W - 2.0f * pad;
        if (m.hover >= 0 && size_t(m.hover) < items.size()) {
            const UiItem* item = Item(items[size_t(m.hover)]);
            const float name_w =
                c.Text(p, left, Baseline(fy + 0.016f * u, 0.024f * u), 0.024f * u,
                       Display(items[size_t(m.hover)]), st.text, UiAlign::Left, line_w * 0.6f);
            if (item && !item->effect.empty()) {
                c.Text(p, W * 0.5f - pad - 0.004f * u, Baseline(fy + 0.016f * u, 0.016f * u),
                       0.016f * u, item->effect, st.highlight_border, UiAlign::Right,
                       line_w - name_w - 0.02f * u);
            }
            if (item && !item->info.empty()) {
                const std::vector<std::string> lines = Wrap(item->info, 0.014f * u, line_w);
                for (size_t i = 0; i < std::min<size_t>(lines.size(), 2); ++i) {
                    c.Text(p, left, Baseline(fy - (0.012f + float(i) * 0.017f) * u, 0.014f * u),
                           0.014f * u, lines[i], st.text_dim);
                }
            }
        } else {
            c.Text(p, left, Baseline(fy, 0.016f * u), 0.016f * u,
                   Has(s.select, "touch") ? "Point and pull the trigger, or touch a card"
                                          : "Point and pull the trigger",
                   st.text_dim);
        }
    }

    void DrawWrist(Menu& m, UiCanvas& c) {
        const UiMenuSpec& s = *m.spec;
        const UiStyle& st = layout.style;
        const std::vector<std::string> items = Items(m);
        const int n = int(items.size());
        UiPanel p = m.base;
        p.scale = 0.85f + 0.15f * Ease(m.anim);
        c.alpha = Ease(m.anim);
        c.on_top = s.on_top;
        m.elements.clear();
        m.half_w = s.width * 0.5f;
        m.half_h = s.height * 0.5f;
        c.Frame(p, 0, 0, s.width, s.height, st.panel, st.panel_border, 0.0012f, 0.006f);
        if (!s.label.empty()) {
            c.Text(p, -s.width * 0.5f + 0.004f, s.height * 0.5f + 0.004f, 0.011f, s.label,
                   st.text_dim);
        }
        if (!n) {
            return;
        }
        const float pad = 0.004f, gap = 0.004f;
        const float sw = (s.width - 2.0f * pad - float(n - 1) * gap) / float(n);
        const float sh = s.height - 2.0f * pad;
        for (int k = 0; k < n; ++k) {
            const float x = -s.width * 0.5f + pad + sw * 0.5f + float(k) * (sw + gap);
            const bool hot = m.hover == k;
            c.Frame(p, x, 0, sw, sh, hot ? st.highlight : st.slot,
                    hot ? st.highlight_border : st.slot_border, hot ? 0.0012f : 0.0008f, 0.004f);
            const float icon = std::min(sh, sw * 0.55f);
            c.Icon(p, x - sw * 0.5f + icon * 0.55f, 0, icon * 0.95f, IconOf(items[size_t(k)]),
                   {1, 1, 1, 1});
            const int count = CountOf(items[size_t(k)]);
            const float ts = sh * 0.5f;
            c.Text(p, x + sw * 0.5f - 0.003f, Baseline(0, ts), ts,
                   count >= 0 ? std::to_string(count) : std::string("-"),
                   count != 0 ? st.text : st.accent, UiAlign::Right, sw - icon);
            m.elements.push_back({k, x - sw * 0.5f, -sh * 0.5f, x + sw * 0.5f, sh * 0.5f});
        }
    }

    void DrawSlots(Menu& m, UiCanvas& c) {
        const UiMenuSpec& s = *m.spec;
        const UiStyle& st = layout.style;
        c.on_top = s.on_top;
        for (size_t i = 0; i < s.slots.size() && i < m.slot_world.size(); ++i) {
            const Vec3 at = m.slot_world[i];
            const bool hot = m.slot_hover[0] == int(i) || m.slot_hover[1] == int(i);
            float reach = 1e9f;
            for (int h = 0; h < 2; ++h) {
                if (in.grip[h].valid) {
                    reach = std::min(reach, Length(in.grip[h].position - at));
                }
            }
            if (!hot && (s.markers == "never" || (s.markers == "near" && reach > kNear))) {
                continue;
            }
            // Screen-aligned (the head's up): readable when looking steeply down at the hips.
            const UiPanel p = UiPanel::Facing(at, in.head.position,
                                              Rotate(in.head.rotation, {0, 1, 0}));
            c.alpha = hot ? 1.0f : reach < kNear ? 0.85f : 0.5f;
            const float r = s.marker_size * (hot ? 0.62f : 0.5f);
            c.Disc(p, 0, 0, r + 0.003f, hot ? st.highlight_border : st.slot_border);
            c.Disc(p, 0, 0, r, hot ? st.highlight : st.panel);
            c.Icon(p, 0, 0, r * 1.75f, IconOf(s.slots[i].item), {1, 1, 1, 1});
            const int count = CountOf(s.slots[i].item);
            if (count >= 0) {
                c.Text(p, r * 0.95f, -r * 0.95f, r * 0.62f, std::to_string(count),
                       count ? st.text : st.accent, UiAlign::Right);
            }
            const bool popup = int(i) == m.flash_slot && now - m.flash_at < 1.4;
            if ((hot || reach < 0.22f) && !popup) {
                c.Text(p, 0, r + 0.012f, 0.014f, Display(s.slots[i].item), st.text,
                       UiAlign::Center);
            }
            const double since = now - m.flash_at;
            if (int(i) == m.flash_slot && since < 0.35) {
                const float t = float(since / 0.35);
                c.alpha = 1.0f - t;
                c.Sector(p, 0, 0, 0, 3.2f, r * (1.0f + t), r * (1.0f + t) + 0.004f,
                         st.highlight_border);
            }
        }
        c.alpha = 1.0f;
    }

    void DrawDashboard(Menu& m, UiCanvas& c) {
        const UiMenuSpec& s = *m.spec;
        const UiStyle& st = layout.style;
        const float u = s.cell / 0.055f;
        const float cell = s.cell, gap = 0.006f * u, pad = 0.014f * u;
        const int cols = std::max(s.columns, 6);
        const float W = float(cols) * cell + float(cols - 1) * gap + 2.0f * pad;
        const float title_h = 0.04f * u, hands_h = 0.07f * u, label_h = 0.02f * u;
        const float tabs_h = 0.032f * u, button_h = 0.034f * u, footer_h = 0.055f * u;
        const float grid_h = float(s.rows) * cell + float(s.rows - 1) * gap;
        const float H = 2.0f * pad + title_h + hands_h + label_h + cell + tabs_h + grid_h +
                        button_h + footer_h + 6.0f * gap;
        UiPanel p = m.base;
        p.scale = 0.85f + 0.15f * Ease(m.anim);
        c.alpha = Ease(m.anim);
        c.on_top = s.on_top;
        m.elements.clear();
        m.dash.clear();
        m.half_w = W * 0.5f;
        m.half_h = H * 0.5f;
        c.Frame(p, 0, 0, W, H, st.panel, st.panel_border, 0.0025f * u, 0.012f * u);
        const float left = -W * 0.5f + pad, right = W * 0.5f - pad;
        float y = H * 0.5f - pad; // the top of the next row
        const auto add = [&](int id, float cx, float cy, float w, float h, bool enabled,
                             const std::string& title, const std::string& detail,
                             const std::string& reason) {
            m.elements.push_back({id, cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f});
            Menu::DashInfo info;
            info.enabled = enabled;
            info.title = title;
            info.detail = detail;
            info.reason = reason;
            m.dash[id] = info;
        };
        const auto button = [&](int id, float cx, float cy, float w, float h, const std::string& text,
                                bool on, bool enabled, const std::string& detail,
                                const std::string& reason) {
            const bool hot = m.hover == id;
            UiColor fill = hot ? st.highlight : on ? st.slot.WithAlpha(1.0f) : st.slot;
            c.Frame(p, cx, cy, w, h, enabled ? fill : st.slot.WithAlpha(0.35f),
                    hot ? st.highlight_border : on ? st.panel_border : st.slot_border, 0.0012f * u,
                    0.006f * u);
            c.Text(p, cx, Baseline(cy, 0.017f * u), 0.017f * u, text,
                   enabled ? (on || hot ? st.text : st.text_dim) : st.text_dim.WithAlpha(0.5f),
                   UiAlign::Center, w - 0.006f * u);
            add(id, cx, cy, w, h, enabled, text, detail, reason);
        };
        // Title: the label, the click mode, close.
        {
            const float cy = y - title_h * 0.5f;
            c.Text(p, left + 0.004f * u, Baseline(cy, 0.026f * u), 0.026f * u, s.label, st.text);
            button(kDashClose, right - 0.018f * u, cy, 0.036f * u, 0.03f * u, "x", false, true,
                   "Close the dashboard", "");
            button(kDashRemove, right - 0.325f * u, cy, 0.2f * u, 0.03f * u,
                   m.remove_mode ? "Point at one: REMOVE" : "Remove from wheel", m.remove_mode,
                   Disabled("clear_quick").empty(),
                   m.remove_mode ? "Point at a quick slot (or its card below) to empty it; pull again here to cancel"
                                 : "Then point at a quick item: it leaves the quick slots (the wheel)",
                   Disabled("clear_quick"));
            button(kDashMode, right - 0.13f * u, cy, 0.17f * u, 0.03f * u,
                   m.use_mode ? "Quick click: USE" : "Quick click: SELECT", true, true,
                   m.use_mode ? "A quick item you point at is used at once (d-pad to it, square)"
                              : "A quick item you point at only becomes the current one (X uses it)",
                   "");
            y -= title_h + gap;
        }
        // What each hand holds now, large.
        {
            const float cy = y - hands_h * 0.5f;
            const float half = (W - 2.0f * pad - gap) * 0.5f;
            for (int h = 1; h >= 0; --h) {
                const float cx = h ? right - half * 0.5f : left + half * 0.5f;
                c.Frame(p, cx, cy, half, hands_h, st.slot, st.slot_border, 0.0012f * u, 0.008f * u);
                const float icon = hands_h * 0.9f;
                const float ix = cx - half * 0.5f + icon * 0.6f;
                const bool held = !equipped[h].empty();
                if (held) {
                    c.Icon(p, ix, cy, icon, IconOf(equipped[h]), {1, 1, 1, 1});
                }
                c.Text(p, ix + icon * 0.6f, Baseline(cy + 0.012f * u, 0.014f * u), 0.014f * u,
                       h ? "RIGHT HAND" : "LEFT HAND", st.text_dim);
                c.Text(p, ix + icon * 0.6f, Baseline(cy - 0.012f * u, 0.02f * u), 0.02f * u,
                       held ? Display(equipped[h]) : std::string("nothing"),
                       held ? st.text : st.accent, UiAlign::Left, half - icon * 1.3f);
            }
            y -= hands_h + gap;
        }
        // Weapon slots (4) and quick slots, labelled.
        const auto weapons = live.lists.find("weapons");
        const auto quick = live.lists.find("quick");
        const int nq = quick == live.lists.end() ? 0 : int(quick->second.items.size());
        {
            const float ly = y - label_h * 0.5f;
            c.Text(p, left + 0.002f * u, Baseline(ly, 0.014f * u), 0.014f * u,
                   "WEAPON SLOTS", st.text_dim);
            const float qx = left + 4.0f * (cell + gap) + 0.5f * gap;
            c.Text(p, qx, Baseline(ly, 0.014f * u), 0.014f * u, "QUICK ITEMS", st.text_dim);
            y -= label_h;
            const float cy = y - cell * 0.5f;
            static const char* const kSlot[4] = {"R1", "R2", "L1", "L2"};
            const int held_r = Selected("weapons_right"), held_l = Selected("weapons_left");
            for (int k = 0; k < 4; ++k) {
                const float cx = left + cell * 0.5f + float(k) * (cell + gap);
                const std::string key = weapons != live.lists.end() &&
                                                size_t(k) < weapons->second.items.size()
                                            ? weapons->second.items[size_t(k)]
                                            : std::string();
                const bool active = k == held_r || k == held_l;
                const bool hot = m.hover == kDashWeapon + k;
                c.Frame(p, cx, cy, cell, cell, hot ? st.highlight : st.slot,
                        hot || active ? st.highlight_border : st.slot_border,
                        active ? 0.0022f * u : 0.0012f * u, cell * 0.08f);
                if (!key.empty()) {
                    c.Icon(p, cx, cy + cell * 0.04f, cell * 0.8f, IconOf(key), {1, 1, 1, 1});
                }
                c.Text(p, cx - cell * 0.42f, Baseline(cy + cell * 0.36f, 0.012f * u), 0.012f * u,
                       kSlot[k], active ? st.highlight_border : st.text_dim);
                if (active) {
                    c.Text(p, cx, Baseline(cy - cell * 0.4f, 0.011f * u), 0.011f * u, "held",
                           st.highlight_border, UiAlign::Center);
                }
                add(kDashWeapon + k, cx, cy, cell, cell, !key.empty() && !active,
                    key.empty() ? std::string(kSlot[k]) + ": empty" : Display(key),
                    active ? "In your hand now" : "Switch to it (the game's d-pad swap)",
                    key.empty() ? "Empty slot: put a weapon there in the game's equipment menu"
                                : "Already in your hand");
            }
            const int current = Selected("quick");
            for (int k = 0; k < nq && 4 + k < cols; ++k) {
                const float cx = left + cell * 0.5f + float(4 + k) * (cell + gap) + gap;
                const std::string& key = quick->second.items[size_t(k)];
                const bool hot = m.hover == kDashQuick + k, now_current = k == current;
                c.Frame(p, cx, cy, cell, cell, hot ? st.highlight : st.slot,
                        hot || now_current ? st.highlight_border : st.slot_border,
                        now_current ? 0.0022f * u : 0.0012f * u, cell * 0.08f);
                if (!key.empty()) {
                    c.Icon(p, cx, cy + cell * 0.04f, cell * 0.8f, IconOf(key), {1, 1, 1, 1});
                    const int count = CountOf(key);
                    if (count >= 0) {
                        c.Text(p, cx + cell * 0.44f, cy - cell * 0.44f, cell * 0.26f,
                               std::to_string(count), count ? st.text : st.accent, UiAlign::Right);
                    }
                }
                if (now_current) {
                    c.Text(p, cx - cell * 0.42f, Baseline(cy + cell * 0.36f, 0.011f * u), 0.011f * u,
                           "X", st.highlight_border);
                }
                add(kDashQuick + k, cx, cy, cell, cell, !key.empty(),
                    key.empty() ? "Quick slot " + std::to_string(k + 1) + ": empty" : Display(key),
                    m.remove_mode ? "Take it out of the quick slots"
                                  : m.use_mode ? "Use it now" : "Make it the current quick item",
                    "Empty slot: " + (Disabled("assign_quick").empty()
                                          ? std::string("point at an item in the inventory below to put it here")
                                          : Disabled("assign_quick")));
            }
            y -= cell + gap;
        }
        // Inventory tabs and pages.
        std::vector<int> index;
        const std::vector<std::string> items = Items(m, &index);
        const int per_page = cols * std::max(s.rows, 1);
        const int pages = std::max(1, (int(items.size()) + per_page - 1) / per_page);
        m.page = std::clamp(m.page, 0, pages - 1);
        {
            const float cy = y - tabs_h * 0.5f;
            const int n = int(s.tabs.size());
            const float arrows = 0.16f * u;
            const float tw = n ? (W - 2.0f * pad - arrows - float(n) * gap) / float(n) : 0.0f;
            for (int i = 0; i < n; ++i) {
                const float cx = left + tw * 0.5f + float(i) * (tw + gap);
                button(kDashTab + i, cx, cy, tw, tabs_h, TabLabel(s.tabs[size_t(i)]), i == m.tab, true,
                       "Show " + TabLabel(s.tabs[size_t(i)]), "");
            }
            button(kDashPrev, right - arrows + 0.02f * u, cy, 0.036f * u, tabs_h, "<", false,
                   m.page > 0, "Previous page", "First page");
            c.Text(p, right - arrows * 0.5f + 0.008f * u, Baseline(cy, 0.014f * u), 0.014f * u,
                   std::to_string(m.page + 1) + "/" + std::to_string(pages), st.text_dim,
                   UiAlign::Center);
            button(kDashNext, right - 0.018f * u, cy, 0.036f * u, tabs_h, ">", false,
                   m.page + 1 < pages, "Next page", "Last page");
            y -= tabs_h + gap;
        }
        // The inventory grid.
        {
            const bool weapons_tab = !s.tabs.empty() && s.tabs[size_t(m.tab) % s.tabs.size()] == "weapons";
            for (int r = 0; r < s.rows; ++r) {
                for (int k = 0; k < cols; ++k) {
                    const int i = m.page * per_page + r * cols + k;
                    const float cx = left + cell * 0.5f + float(k) * (cell + gap);
                    const float cy = y - cell * 0.5f - float(r) * (cell + gap);
                    if (size_t(i) >= items.size()) {
                        c.Frame(p, cx, cy, cell, cell, st.slot.WithAlpha(0.3f),
                                st.slot_border.WithAlpha(0.3f), 0.001f * u, cell * 0.08f);
                        continue;
                    }
                    const std::string key = Resolve(items[size_t(i)]);
                    const int slot = QuickSlotOf(key);
                    const UiItem* card = Item(key);
                    const bool weapon = weapons_tab || (card && card->kind == "weapon");
                    std::string reason;
                    if (weapon) {
                        reason = Disabled("equip_inventory");
                    } else if (slot < 0) {
                        reason = Disabled("assign_quick");
                    }
                    const bool enabled = reason.empty();
                    const std::string held = card && card->hand == "left" ? "left" : "right";
                    ItemCard(c, p, cx, cy, cell, items[size_t(i)], m.hover == kDashItem + i, true);
                    if (!enabled) {
                        c.Box(p, cx, cy, cell, cell, {0.0f, 0.0f, 0.0f, 0.45f}, cell * 0.08f);
                    } else if (slot >= 0) {
                        c.Text(p, cx - cell * 0.42f, Baseline(cy + cell * 0.36f, 0.011f * u),
                               0.011f * u, "Q" + std::to_string(slot + 1), st.highlight_border);
                    }
                    std::string detail;
                    if (weapon) {
                        detail = "Equip it in the " + held + " hand (into the slot held now)";
                    } else if (slot >= 0 && m.remove_mode) {
                        detail = "In quick slot " + std::to_string(slot + 1) + ": take it out";
                    } else if (slot >= 0) {
                        detail = m.use_mode ? "In quick slot " + std::to_string(slot + 1) + ": use it now"
                                            : "In quick slot " + std::to_string(slot + 1) + ": make it current";
                    } else {
                        detail = m.use_mode ? "Put it in a quick slot and use it"
                                            : "Put it in a quick slot";
                    }
                    add(kDashItem + i, cx, cy, cell, cell, enabled, Display(key), detail, reason);
                }
            }
            y -= grid_h + gap;
        }
        // Buttons: what has no button of its own.
        {
            const int n = int(s.buttons.size());
            const float cy = y - button_h * 0.5f;
            const float bw = n ? (W - 2.0f * pad - float(n - 1) * gap) / float(n) : 0.0f;
            for (int i = 0; i < n; ++i) {
                const UiDashButton& b = s.buttons[size_t(i)];
                const std::string reason = Disabled(b.action);
                const float cx = left + bw * 0.5f + float(i) * (bw + gap);
                button(kDashButton + i, cx, cy, bw, button_h, b.label, false, reason.empty(),
                       b.action, reason);
            }
            y -= button_h + gap;
        }
        // Footer: what the pointer is on, or why it is not available.
        {
            const float cy = y - footer_h * 0.5f;
            const auto it = m.dash.find(m.hover);
            if (it != m.dash.end()) {
                c.Text(p, left + 0.004f * u, Baseline(cy + 0.013f * u, 0.02f * u), 0.02f * u,
                       it->second.title, st.text, UiAlign::Left, W - 2.0f * pad);
                const std::string line = it->second.enabled ? it->second.detail
                                                            : "Not available: " + it->second.reason;
                c.Text(p, left + 0.004f * u, Baseline(cy - 0.012f * u, 0.015f * u), 0.015f * u, line,
                       it->second.enabled ? st.text_dim : st.accent, UiAlign::Left, W - 2.0f * pad);
            } else {
                c.Text(p, left + 0.004f * u, Baseline(cy, 0.015f * u), 0.015f * u,
                       "Point with either controller and pull its trigger. Left stick click closes.",
                       st.text_dim);
            }
        }
    }

    void DrawOverlay(UiCanvas& c) {
        const UiStyle& st = layout.style;
        c.on_top = true;
        c.alpha = 1.0f;
        if (!in.head.valid) {
            return;
        }
        // Popups: rise and fade.
        for (const Popup& pop : popups) {
            const float t = float((now - pop.start) / 1.4);
            if (t >= 1.0f) {
                continue;
            }
            const Vec3 head_up = Rotate(in.head.rotation, {0, 1, 0});
            const Vec3 at = pop.position + head_up * (0.06f * t);
            const UiPanel p = UiPanel::Facing(at, in.head.position, head_up);
            c.alpha = 1.0f - t * t;
            // The same angular size near (a chest slot) and far (a panel at arm's length).
            const float size = 0.016f * std::clamp(Length(at - in.head.position) / 0.6f, 0.4f, 1.6f);
            c.Text(p, 0, 0, size, pop.text, st.text, UiAlign::Center);
        }
        c.alpha = 1.0f;
        // Toast or help: head-locked, a little low.
        const double age = now - toast_at;
        std::string title = toast_title, body = toast_body;
        float alpha = age < toast_seconds ? float(std::min(1.0, (toast_seconds - age) / 0.5)) : 0.0f;
        if (help) {
            const UiPreset* p = Preset();
            title = p ? "Preset " + p->label : "No preset";
            body = (p ? p->notes : std::string()) + "  |  Presets: " + layout.cycle_next.text +
                   " / " + layout.cycle_prev.text + " (hold the left stick click, flick the right "
                   "stick). Refill: " + layout.reset.text + ". This help: " + layout.help.text + ".";
            alpha = 1.0f;
        }
        if (alpha <= 0.0f) {
            return;
        }
        const Vec3 origin = in.head.position + Rotate(in.head.rotation, {0.0f, -0.17f, -0.85f});
        const UiPanel p = UiPanel::Facing(origin, in.head.position,
                                          Rotate(in.head.rotation, {0, 1, 0}));
        const float width = 0.5f;
        const std::vector<std::string> lines = Wrap(body, 0.018f, width - 0.03f);
        const float h = 0.05f + 0.024f * float(lines.size());
        c.alpha = alpha;
        c.Frame(p, 0, 0, width, h, st.panel, st.panel_border, 0.002f, 0.01f);
        float y = h * 0.5f - 0.03f;
        c.Text(p, -width * 0.5f + 0.015f, y, 0.024f, title, st.text, UiAlign::Left, width - 0.03f);
        for (const std::string& line : lines) {
            y -= 0.024f;
            c.Text(p, -width * 0.5f + 0.015f, y, 0.018f, line, st.text_dim);
        }
        c.alpha = 1.0f;
    }
};

// ---- UiSystem --------------------------------------------------------------------------------------

UiSystem::UiSystem() : impl(std::make_unique<Impl>()) {}
UiSystem::~UiSystem() = default;

bool UiSystem::Load(const std::filesystem::path& assets, Renderer* renderer, std::string& log,
                    const std::string& log_prefix) {
    Impl& s = *impl;
    s.assets = assets;
    s.log_prefix = log_prefix;
    s.atlas.Load(assets / "ui", log);
    s.font.Load(assets / "ui", log);
    if (renderer) {
        std::string error;
        if (!s.gpu.Create(renderer->GetDevice(), error)) {
            log += error + "\n";
            return false;
        }
        s.gpu_ready = true;
        if (s.atlas.Ready() && !s.gpu.UploadIcons(s.atlas.width, s.atlas.height, s.atlas.pixels)) {
            log += "icon upload failed: " + s.gpu.LastError() + "\n";
        }
        if (s.font.Ready() && !s.gpu.UploadFont(s.font.width, s.font.height, s.font.pixels)) {
            log += "font upload failed: " + s.gpu.LastError() + "\n";
        }
        renderer->SetDrawHook(&s.gpu);
    }
    s.atlas.pixels = {};
    s.font.pixels = {};
    s.file = std::make_unique<UiLayoutFile>(assets / "ui_layouts.json");
    s.file->Poll(0.0, s.layout, log);
    s.ResetCounts();
    s.Activate(std::max(0, s.layout.FindPreset(s.layout.active_preset)), nullptr, false);
    return true;
}

void UiSystem::Destroy(Renderer* renderer) {
    Impl& s = *impl;
    std::string log;
    s.Summary(log);
    if (!log.empty()) {
        std::printf("%s", log.c_str());
    }
    if (renderer) {
        renderer->SetDrawHook(nullptr);
    }
    if (s.gpu_ready) {
        s.gpu.Destroy();
        s.gpu_ready = false;
    }
}

uint32_t UiSystem::FilterButtons(const UiInput& input) {
    Impl& s = *impl;
    const uint32_t raw = input.buttons;
    const uint32_t pressed = raw & ~s.last_raw;
    s.last_raw = raw;
    s.claimed &= raw; // released keys are the scene's again
    const auto down = [&](UiInputId id) {
        return Analog(id) ? AnalogValue(id, input) > kPress : (raw & ButtonBit(id)) != 0;
    };
    const auto claim = [&](const UiBinding& b, bool modifiers_needed) {
        const uint32_t bit = ButtonBit(b.key);
        if (!bit || !(pressed & bit)) {
            return;
        }
        if (modifiers_needed) {
            for (UiInputId id : b.modifiers) {
                if (!down(id)) {
                    return;
                }
            }
        }
        s.claimed |= bit;
    };
    for (const Impl::Menu& m : s.menus) {
        claim(m.spec->open, !m.open);
        if (m.open) {
            claim(m.spec->click, false);
        }
    }
    claim(s.layout.cycle_next, true);
    claim(s.layout.cycle_prev, true);
    claim(s.layout.help, true);
    claim(s.layout.reset, true);
    return raw & ~s.claimed;
}

void UiSystem::Update(const UiInput& input, const UiBody& body, Config& config, UiEvents& events) {
    UiWorld world;
    world.eye_height_m = config.eye_height_m;
    world.weapon[0] = config.left_weapon;
    world.weapon[1] = config.weapon;
    world.show_weapon = config.show_weapon;
    Update(input, body, world, events);
    config.left_weapon = world.weapon[0];
    config.weapon = world.weapon[1];
    config.show_weapon = world.show_weapon;
}

void UiSystem::Update(const UiInput& input, const UiBody& body, UiWorld& world, UiEvents& events) {
    Impl& s = *impl;
    s.dt = s.started ? std::clamp(input.time - s.now, 0.0, 0.1) : 0.0;
    s.started = true;
    s.now = input.time;
    s.in = input;
    s.eye_height = world.eye_height_m;
    std::string log;
    const std::string active = s.PresetName();
    if (s.file && s.file->Poll(input.time, s.layout, log)) {
        s.ResetCounts();
        int index = s.layout.FindPreset(active);
        if (index < 0 || s.layout.active_preset != active) {
            index = s.layout.FindPreset(s.layout.active_preset);
        }
        s.Activate(std::max(index, 0), &events, s.PresetName() != active);
        s.Write(s.Event("layout", nullptr).Add("file", s.file->Path().string()));
    }
    events.log += log;
    for (int h = 0; h < 2; ++h) {
        s.equipped[h] = s.Display(world.weapon[h]);
    }
    s.Inputs(input);
    s.body_yaw = body.yaw;
    for (int h = 0; h < 2; ++h) {
        s.tip[h] = body.tip[h];
        s.tip_valid[h] = body.tip_valid[h];
    }
    if (s.game) {
        // No presets, help or refill in the game: the counts are the game's.
    } else if (s.PressedExact(s.layout.cycle_next)) {
        s.Activate(s.preset + 1, &events, true);
    } else if (s.PressedExact(s.layout.cycle_prev)) {
        s.Activate(s.preset - 1, &events, true);
    }
    if (!s.game && s.PressedExact(s.layout.help)) {
        s.help = !s.help;
        s.Write(s.Event("help", nullptr).Add("shown", s.help));
    }
    if (!s.game && s.PressedExact(s.layout.reset)) {
        s.ResetCounts();
        s.Toast("Inventory refilled", "Every item is back to its count in ui_layouts.json.", 2.5);
        s.Write(s.Event("refill", nullptr));
        s.Haptic(events, 0, s.layout.style.haptic_preset);
    }
    for (Impl::Pointer& p : s.pointer) {
        p = {};
    }
    for (Impl::Menu& m : s.menus) {
        const std::string& type = m.spec->type;
        if (type == "radial") {
            s.UpdateRadial(m, world, events);
        } else if (type == "grid") {
            s.UpdateGrid(m, world, events);
        } else if (type == "wrist") {
            s.UpdateWrist(m, world, events);
        } else if (type == "body_slots") {
            s.UpdateSlots(m, world, events);
        } else if (type == "dashboard") {
            s.UpdateDashboard(m, world, events);
        }
        const float rate = float(s.dt) / (m.open ? s.layout.style.open_seconds
                                                 : -s.layout.style.close_seconds);
        m.anim = std::clamp(m.anim + rate, 0.0f, 1.0f);
        if (type == "body_slots") {
            m.anim = 1.0f;
        }
    }
    // Held weapons cover panels: menus with hide_weapons hide them while open.
    bool hide = false;
    for (const Impl::Menu& m : s.menus) {
        hide = hide || (m.spec->hide_weapons && (m.open || m.anim > 0.0f));
    }
    if (hide && !s.weapons_hidden) {
        s.weapons_shown_before = world.show_weapon;
        s.weapons_hidden = true;
    }
    if (s.weapons_hidden) {
        world.show_weapon = hide ? false : s.weapons_shown_before;
        s.weapons_hidden = hide;
    }
    s.popups.erase(std::remove_if(s.popups.begin(), s.popups.end(),
                                  [&](const Impl::Popup& p) { return s.now - p.start > 1.4; }),
                   s.popups.end());
}

void UiSystem::Build(FrameData& data) {
    BuildInto(&data);
}

void UiSystem::Build() {
    BuildInto(nullptr);
}

void UiSystem::BuildInto(FrameData* data) {
    Impl& s = *impl;
    s.canvas.Clear();
    s.drew = false;
    if (!s.in.head.valid) {
        if (s.gpu_ready) {
            s.gpu.SetGeometry(s.canvas);
        }
        return;
    }
    // Far menus first: there are no depth writes, so the nearer one is drawn over.
    struct Item {
        float distance;
        Impl::Menu* menu;
    };
    std::vector<Item> order;
    for (Impl::Menu& m : s.menus) {
        if (m.anim <= 0.0f && m.spec->type != "body_slots") {
            continue;
        }
        order.push_back({Length(m.base.origin - s.in.head.position), &m});
    }
    std::sort(order.begin(), order.end(),
              [](const Item& a, const Item& b) { return a.distance > b.distance; });
    for (const Item& it : order) {
        Impl::Menu& m = *it.menu;
        const std::string& type = m.spec->type;
        s.canvas.alpha = 1.0f;
        s.canvas.outline = s.layout.style.text_outline;
        if (type == "radial") {
            s.DrawRadial(m, s.canvas);
        } else if (type == "grid") {
            s.DrawGrid(m, s.canvas);
        } else if (type == "wrist") {
            s.DrawWrist(m, s.canvas);
        } else if (type == "body_slots") {
            s.DrawSlots(m, s.canvas);
        } else if (type == "dashboard") {
            s.DrawDashboard(m, s.canvas);
        }
    }
    // Pointers: the beam as lines, the hit as a dot.
    const UiColor laser = s.layout.style.laser;
    for (const Impl::Pointer& p : s.pointer) {
        if (!p.show) {
            continue;
        }
        // The line pipeline writes its color as linear: give it linear values.
        if (data) {
            const uint8_t r = uint8_t(std::clamp(laser.r, 0.0f, 1.0f) * 255.0f);
            const uint8_t g = uint8_t(std::clamp(laser.g, 0.0f, 1.0f) * 255.0f);
            const uint8_t b = uint8_t(std::clamp(laser.b, 0.0f, 1.0f) * 255.0f);
            data->overlay.push_back({{p.from.x, p.from.y, p.from.z}, {r, g, b, 255}});
            data->overlay.push_back({{p.to.x, p.to.y, p.to.z}, {r, g, b, 255}});
        } else {
            // No line pipeline: a 2 mm strip along the beam, turned toward the eyes.
            const Vec3 d = p.to - p.from;
            const float length = Length(d);
            if (length > 1e-3f) {
                UiPanel beam;
                beam.origin = p.from + d * 0.5f;
                beam.x = d * (1.0f / length);
                beam.z = Normalize(Perp(s.in.head.position - beam.origin, beam.x),
                                   AnyPerp(beam.x));
                beam.y = Cross(beam.z, beam.x);
                s.canvas.on_top = true;
                s.canvas.alpha = 1.0f;
                s.canvas.Box(beam, 0.0f, 0.0f, length, 0.002f, laser.WithAlpha(0.85f));
            }
        }
        if (p.hit) {
            const UiPanel dot = UiPanel::Facing(p.to, s.in.head.position, {0, 1, 0});
            s.canvas.on_top = true;
            s.canvas.alpha = 1.0f;
            s.canvas.Disc(dot, 0, 0, 0.005f, {1, 1, 1, 0.95f});
            s.canvas.Disc(dot, 0, 0, 0.0035f, laser);
        }
    }
    s.canvas.outline = s.layout.style.text_outline;
    s.DrawOverlay(s.canvas);
    s.drew = !s.canvas.world.empty() || !s.canvas.top.empty();
    if (s.gpu_ready && !s.gpu.SetGeometry(s.canvas)) {
        std::printf("ui: %s\n", s.gpu.LastError().c_str());
    }
}

const UiCanvas& UiSystem::Canvas() const {
    return impl->canvas;
}

bool UiSystem::Visible() const {
    return impl->drew;
}

bool UiSystem::LoadGame(const std::filesystem::path& assets, const Renderer::Device* device,
                        std::string& log) {
    Impl& s = *impl;
    s.game = true;
    s.assets = assets;
    s.log_prefix = "game_ui";
    s.atlas.Load(assets / "ui", log);
    s.font.Load(assets / "ui", log);
    bool ok = true;
    if (device) {
        std::string error;
        if (!s.gpu.Create(*device, error)) {
            log += error + "\n";
            ok = false;
        } else {
            s.gpu.SetAccumulateAlpha(true);
            s.gpu_ready = true;
            if (s.atlas.Ready() &&
                !s.gpu.UploadIcons(s.atlas.width, s.atlas.height, s.atlas.pixels)) {
                log += "icon upload failed: " + s.gpu.LastError() + "\n";
            }
            if (s.font.Ready() && !s.gpu.UploadFont(s.font.width, s.font.height, s.font.pixels)) {
                log += "font upload failed: " + s.gpu.LastError() + "\n";
            }
        }
    }
    s.atlas.pixels = {};
    s.font.pixels = {};
    s.file = std::make_unique<UiLayoutFile>(assets / "ui_layouts.json");
    s.file->Poll(0.0, s.layout, log);
    s.ResetCounts();
    s.Activate(0, nullptr, false);
    return ok;
}

DrawHook* UiSystem::Gpu() {
    return impl->gpu_ready ? &impl->gpu : nullptr;
}

void UiSystem::SetLiveData(const UiLiveData& data) {
    Impl& s = *impl;
    s.live = data;
    s.ResetCounts();
}

bool UiSystem::Engaged() const {
    const Impl& s = *impl;
    for (const Impl::Menu& m : s.menus) {
        const std::string& type = m.spec->type;
        if ((type == "radial" || type == "grid" || type == "dashboard") && m.open) {
            return true;
        }
        if (type == "body_slots" && (m.slot_hover[0] >= 0 || m.slot_hover[1] >= 0)) {
            return true;
        }
        if (type == "wrist" && m.open && m.hover >= 0) {
            return true;
        }
    }
    for (const Impl::Pointer& p : s.pointer) {
        if (p.show && p.hit) {
            return true;
        }
    }
    return false;
}

bool UiSystem::UsesStick(int hand) const {
    for (const Impl::Menu& m : impl->menus) {
        if (!m.open) {
            continue;
        }
        if (m.spec->type == "radial" && m.spec->hand == hand &&
            std::find(m.spec->select.begin(), m.spec->select.end(), "stick") !=
                m.spec->select.end()) {
            return true;
        }
        if (m.spec->type == "grid" && m.spec->page_stick == hand) {
            return true;
        }
    }
    return false;
}

bool UiSystem::ElementCenter(const std::string& menu, int id, Vec3& world) const {
    for (const Impl::Menu& m : impl->menus) {
        if (m.spec->name != menu) {
            continue;
        }
        for (const Impl::Element& e : m.elements) {
            if (e.id == id) {
                world = m.base.At(0.5f * (e.x0 + e.x1), 0.5f * (e.y0 + e.y1));
                return true;
            }
        }
    }
    return false;
}

void UiSystem::ShowMessage(const std::string& title, const std::string& body, double seconds) {
    impl->Toast(title, body, seconds);
}

void UiSystem::CloseAll() {
    Impl& s = *impl;
    for (Impl::Menu& m : s.menus) {
        if (m.spec->type == "radial" || m.spec->type == "grid" || m.spec->type == "dashboard") {
            s.Close(m, "cancel");
        }
    }
}

std::string UiSystem::Controls() const {
    const Impl& s = *impl;
    std::string out = "VR menus (" + (s.file ? s.file->Path().string() : std::string("ui_layouts.json")) +
                      "): next/previous preset " + s.layout.cycle_next.text + " / " +
                      s.layout.cycle_prev.text +
                      " (hold the left stick click and flick the right stick), help " +
                      s.layout.help.text + ", refill " + s.layout.reset.text + "\n";
    for (const UiPreset& p : s.layout.presets) {
        out += "  " + p.label + (p.name == s.PresetName() ? " (active)" : "") + ":";
        for (const std::string& name : p.menus) {
            if (const UiMenuSpec* m = s.layout.FindMenu(name)) {
                out += " " + m->name + (m->open.Empty() ? "" : " [" + m->open.text + "]");
            }
        }
        out += "\n";
    }
    return out;
}

bool UiSystem::SetPreset(const std::string& name) {
    const int index = impl->layout.FindPreset(name);
    if (index < 0) {
        return false;
    }
    impl->Activate(index, nullptr, false);
    return true;
}

std::string UiSystem::ActivePreset() const {
    return impl->PresetName();
}

const UiLayout& UiSystem::Layout() const {
    return impl->layout;
}

bool UiSystem::UsesSticks() const {
    for (const Impl::Menu& m : impl->menus) {
        if (m.open && (m.spec->type == "radial" || m.spec->type == "grid")) {
            return true;
        }
    }
    return false;
}

int UiSystem::Count(const std::string& item) const {
    return impl->CountOf(item);
}

int UiSystem::Hovered(const std::string& menu) const {
    for (const Impl::Menu& m : impl->menus) {
        if (m.spec->name == menu) {
            return m.spec->type == "body_slots" ? std::max(m.slot_hover[0], m.slot_hover[1])
                                                : m.hover;
        }
    }
    return -1;
}

bool UiSystem::IsOpen(const std::string& menu) const {
    for (const Impl::Menu& m : impl->menus) {
        if (m.spec->name == menu) {
            return m.open;
        }
    }
    return false;
}

} // namespace sb
