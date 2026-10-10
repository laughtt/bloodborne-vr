// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Physical items in the game's VR UI (docs/vr/findings/39-physical-items-plan.md, phase 1): a
// consumable picked on the quick wheel is not used at once; it is held in a hand, and a gesture
// then fires the game's own use path (quick slot + square, the host's executor):
//   - Mouth  (heals, cures, body buffs)  the item within mouth_radius_m of the mouth, below the eyes,
//                                        for mouth_hold_s (the sandbox's consumables head gesture);
//   - Blade  (weapon buffs)              the item touches the weapon's blade (a segment from the
//                                        weapon bones), held in the hand that holds no weapon of
//                                        the buffed side;
//   - Throw  (throwables)                the grip is let go faster than throw_min_speed: the release
//                                        velocity gives the direction (the host aims the game's
//                                        bullet along it, from the hand); slower puts the item back.
// The grip holds the item: it must close within pickup_grace_s of the pick and opening it without a
// gesture puts the item back (no use). Pure CPU, no I/O: unit tested in vr_game_ui_test.cpp.
// While an item is in a hand (and lockout_s after) BlockMask() says which pad buttons the filter
// hides from the game (swings, the swap tap, L1), so the throw is not also an attack.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "bbport_xr_bridge.h"
#include "sandbox3d/math3d.h"
#include "sandbox3d/ui_layout.h"

namespace vrui {
namespace held {

enum class Kind { None, Mouth, Blade, Throw };

struct ItemInfo {
    Kind kind = Kind::None;
    int goods = 0;
    int target_side = 1; // Blade: the weapon that is buffed (1 right, 0 left: the gun)
    int bullet = 0;      // Throw: the BulletParam row the game launches
    bool aimed = false;  // Throw: its bullet is in the host's redirect list (BBXR_THROW_BULLET_IDS)
};

/// True when `id` is in a list like "100,105,120-121" (the host's vg_id_in_list for the header's
/// default; "all" matches everything). An unparsable list matches nothing.
inline bool IdInList(const char* list, int id) {
    if (!list) {
        return false;
    }
    if (!std::strcmp(list, "all")) {
        return true;
    }
    const char* s = list;
    while (*s) {
        char* end;
        const long lo = std::strtol(s, &end, 10);
        if (end == s) {
            return false;
        }
        long hi = lo;
        s = end;
        if (*s == '-') {
            hi = std::strtol(s + 1, &end, 10);
            if (end == s + 1) {
                return false;
            }
            s = end;
        }
        if (id >= lo && id <= hi) {
            return true;
        }
        while (*s == ',' || *s == ' ') {
            ++s;
        }
    }
    return false;
}

/// What a game item id (goods 0x40000000 | row) is for. Anything not listed is used at once, as
/// before (bells, marks, the bone blade, keys, bullets).
inline ItemInfo Classify(uint32_t game_id) {
    ItemInfo info;
    if ((game_id & 0xF0000000u) != BBXR_ITEM_GOODS) {
        return info;
    }
    const int goods = int(game_id & 0x0FFFFFFFu);
    info.goods = goods;
    switch (goods) {
    case 1000: case 703: case 706:                      // Blood Vial, Iosefka's, Adeline's
    case 1100: case 1101:                               // Antidote, Sedative
    case 1110: case 1120: case 2030: case 1500: case 2060: // Beast Blood Pellet, Blue / Lead Elixir, Madman's, Old Hunter Bone
        info.kind = Kind::Mouth;
        return info;
    case 1300: case 1320: case 1310:                    // Fire Paper, Bolt Paper, Empty Phantasm Shell
        info.kind = Kind::Blade;
        info.target_side = 1;
        return info;
    case 1330:                                          // Bone Marrow Ash: the left weapon (the gun)
        info.kind = Kind::Blade;
        info.target_side = 0;
        return info;
    default:
        break;
    }
    // The throwables and the BulletParam row each launches (throwables.json: goods -> bullet).
    static const struct {
        int goods, bullet;
    } kThrow[] = {{1200, 100}, {1201, 105}, {1210, 121}, {1220, 150}, {1230, 170}, {1240, 120},
                  {1250, 110}, {1251, 114}, {1260, 160}, {1270, 180}, {1410, 300}, {2070, 410},
                  {2080, 350}, {2120, 460}, {2130, 470}};
    for (const auto& t : kThrow) {
        if (t.goods == goods) {
            info.kind = Kind::Throw;
            info.bullet = t.bullet;
            info.aimed = IdInList(BBXR_THROW_BULLET_IDS, t.bullet);
            return info;
        }
    }
    return info;
}

/// The hand that holds the item: the right hand takes drinks and throws; a weapon buff goes in the
/// hand that is not the buffed weapon's (the paper touches the blade of the other hand).
inline int HandFor(const ItemInfo& info, int hint = -1) {
    if (info.kind == Kind::Blade) {
        return 1 - info.target_side;
    }
    return info.kind == Kind::Mouth && (hint == 0 || hint == 1) ? hint : 1;
}

/// Distance from a point to the segment a-b.
inline float DistToSegment(m3::Vec3 p, m3::Vec3 a, m3::Vec3 b) {
    const m3::Vec3 ab = b - a;
    const float len2 = m3::Dot(ab, ab);
    if (len2 < 1e-9f) {
        return m3::Length(p - a);
    }
    const float t = std::clamp(m3::Dot(p - a, ab) / len2, 0.0f, 1.0f);
    return m3::Length(p - (a + ab * t));
}

/// The mouth test of the sandbox's head gesture: within the radius of the head-local mouth point
/// and at least `below_eyes` under the head.
inline bool NearMouth(m3::Vec3 item, m3::Vec3 head_pos, m3::Quat head_rot, const sb::UiHeldItems& c,
                      float* distance = nullptr) {
    const m3::Vec3 mouth = head_pos + m3::Rotate(head_rot, c.mouth);
    const float d = m3::Length(item - mouth);
    if (distance) {
        *distance = d;
    }
    return d <= c.mouth_radius_m && item.y <= head_pos.y - c.below_eyes_m;
}

/// The touch test: the item within blade_radius of the blade (the weapon's far part: from
/// blade_start of the length to the tip). `approx` (no hitbox data: the segment is the weapon bone
/// alone): within blade_fallback_m of the bone.
inline bool TouchesBlade(m3::Vec3 item, m3::Vec3 base, m3::Vec3 tip, const sb::UiHeldItems& c,
                         float* distance = nullptr, bool approx = false) {
    const m3::Vec3 from = approx ? base : base + (tip - base) * c.blade_start;
    const float d = DistToSegment(item, from, tip);
    if (distance) {
        *distance = d;
    }
    return d <= (approx ? c.blade_fallback_m : c.blade_radius_m);
}

/// A hand-velocity estimate from timed positions: the blend of the mean velocity over the last
/// `window_s` and the fastest step in it (the sandbox's HandVelocity, throwables.cpp).
class VelocityTracker {
public:
    void Reset() { samples.clear(); }
    void Push(double t, m3::Vec3 p) {
        if (!samples.empty() && t <= samples.back().t) {
            return;
        }
        samples.push_back({t, p});
        while (samples.size() > 2 && t - samples.front().t > 0.5) {
            samples.pop_front();
        }
    }
    m3::Vec3 Estimate(float window_s, float peak_weight) const {
        if (samples.size() < 2) {
            return {};
        }
        const double t_end = samples.back().t, t_start = t_end - double(window_s);
        std::vector<const Sample*> in;
        for (const Sample& s : samples) {
            if (s.t >= t_start - 1e-9) {
                in.push_back(&s);
            }
        }
        if (in.size() < 2) { // sparse frames: the two newest
            in.clear();
            for (auto it = samples.rbegin(); it != samples.rend() && in.size() < 2; ++it) {
                in.insert(in.begin(), &*it);
            }
        }
        const double span = in.back()->t - in.front()->t;
        if (in.size() < 2 || span < 1e-4) {
            return {};
        }
        const m3::Vec3 mean = (in.back()->p - in.front()->p) * float(1.0 / span);
        m3::Vec3 peak = mean;
        float best = -1.0f;
        for (size_t i = 0; i + 1 < in.size(); ++i) {
            const double dt = in[i + 1]->t - in[i]->t;
            if (dt < 1e-4) {
                continue;
            }
            const m3::Vec3 v = (in[i + 1]->p - in[i]->p) * float(1.0 / dt);
            if (m3::Length(v) > best) {
                best = m3::Length(v);
                peak = v;
            }
        }
        return mean * (1.0f - peak_weight) + peak * peak_weight;
    }

private:
    struct Sample {
        double t;
        m3::Vec3 p;
    };
    std::deque<Sample> samples;
};

/// A blade segment in the headset's space (from the weapon bone and the weapon's hitboxes).
struct Blade {
    bool valid = false;
    bool approx = false; // no hitbox data for the weapon: base == tip == the weapon bone
    m3::Vec3 base, tip;
};

struct Frame {
    double time = 0.0;
    bool head_valid = false;
    m3::Vec3 head_pos;
    m3::Quat head_rot;
    bool hand_valid[2] = {};
    m3::Vec3 grip_pos[2];
    m3::Quat grip_rot[2];
    float squeeze[2] = {};
    Blade blade[2];        // per weapon side (0 left, 1 right)
    bool blocked = false;  // a menu is engaged or the game's own menu is open
};

struct Pulse {
    int hand;
    float amplitude, seconds;
};

/// What a step decided.
struct Result {
    enum Kind { None, Use, Cancel } kind = None;
    ItemInfo item;
    int hand = 1;
    // Use of a Throw item: where and along what the hand let go (the headset's space).
    bool thrown = false;
    m3::Vec3 throw_origin, throw_dir;
    float throw_speed = 0.0f;
    std::string why;
    std::vector<Pulse> pulses;
};

class HeldItem {
public:
    enum class State { Idle, Pending, Held };

    /// An item was picked on the wheel (quick slot `slot`). False (nothing held) when it is not one
    /// of the physical items or another item is still in a hand.
    /// `hand_hint`: the hand that grabbed it from a body slot (honoured for drinks only).
    bool Pick(uint32_t game_id, int slot, double now, int hand_hint = -1) {
        if (state != State::Idle) {
            return false;
        }
        const ItemInfo i = Classify(game_id);
        if (i.kind == Kind::None) {
            return false;
        }
        info = i;
        id = game_id;
        quick_slot = slot;
        hand = HandFor(i, hand_hint);
        state = State::Pending;
        picked_at = now;
        dwell_since = -1.0;
        progress = 0.0f;
        contact = false;
        velocity.Reset();
        return true;
    }

    /// Puts the item back (no use).
    Result Cancel(double now, const sb::UiHeldItems& c, const char* why) {
        Result r;
        r.kind = Result::Cancel;
        r.item = info;
        r.hand = hand;
        r.why = why;
        if (state == State::Held) {
            r.pulses.push_back({hand, c.cancel.amplitude, c.cancel.seconds});
        }
        Finish(now, c);
        return r;
    }

    Result Step(const Frame& f, const sb::UiHeldItems& c) {
        Result r;
        if (state == State::Idle) {
            return r;
        }
        if (f.blocked) {
            return Cancel(f.time, c, "a menu opened");
        }
        if (!f.hand_valid[hand]) {
            return Cancel(f.time, c, "tracking lost");
        }
        item_pos = f.grip_pos[hand] + m3::Rotate(f.grip_rot[hand], c.hold);
        if (state == State::Pending) {
            if (f.squeeze[hand] >= c.grab_squeeze) {
                state = State::Held;
                held_at = f.time;
                velocity.Reset();
                r.pulses.push_back({hand, c.pick.amplitude, c.pick.seconds});
            } else if (f.time - picked_at > double(c.pickup_grace_s)) {
                return Cancel(f.time, c, "the grip did not close");
            } else {
                return r;
            }
        }
        velocity.Push(f.time, item_pos);
        if (f.squeeze[hand] < c.release_squeeze) { // let go
            if (info.kind == Kind::Throw) {
                const m3::Vec3 v = velocity.Estimate(c.throw_window_s, c.throw_peak_weight);
                const float speed = m3::Length(v);
                if (speed >= c.throw_min_speed) {
                    r.kind = Result::Use;
                    r.item = info;
                    r.hand = hand;
                    r.thrown = true;
                    r.throw_origin = item_pos;
                    r.throw_dir = v * (1.0f / speed);
                    r.throw_speed = speed;
                    r.why = "thrown";
                    r.pulses.push_back({hand, c.use.amplitude, c.use.seconds});
                    Finish(f.time, c);
                    return r;
                }
                return Cancel(f.time, c, "released too slowly to throw");
            }
            return Cancel(f.time, c, "released");
        }
        switch (info.kind) {
        case Kind::Mouth: {
            const bool at_mouth = f.head_valid && NearMouth(item_pos, f.head_pos, f.head_rot, c);
            contact = at_mouth;
            if (!at_mouth) {
                dwell_since = -1.0;
                progress = 0.0f;
                break;
            }
            if (dwell_since < 0.0) {
                dwell_since = f.time;
                r.pulses.push_back({hand, c.touch.amplitude, c.touch.seconds});
            }
            progress = c.mouth_hold_s > 0.0f
                           ? std::clamp(float((f.time - dwell_since) / double(c.mouth_hold_s)), 0.0f, 1.0f)
                           : 1.0f;
            if (f.time - dwell_since >= double(c.mouth_hold_s)) {
                return Fire(f, c, "mouth", r);
            }
            break;
        }
        case Kind::Blade: {
            const Blade& b = f.blade[info.target_side];
            bool touch = false;
            if (b.valid) {
                touch = TouchesBlade(item_pos, b.base, b.tip, c, nullptr, b.approx);
            }
            contact = touch;
            if (!touch) {
                dwell_since = -1.0;
                progress = 0.0f;
                break;
            }
            if (dwell_since < 0.0) {
                dwell_since = f.time;
                r.pulses.push_back({hand, c.touch.amplitude, c.touch.seconds});
            }
            progress = c.blade_hold_s > 0.0f
                           ? std::clamp(float((f.time - dwell_since) / double(c.blade_hold_s)), 0.0f, 1.0f)
                           : 1.0f;
            if (f.time - dwell_since >= double(c.blade_hold_s)) {
                return Fire(f, c, "blade", r);
            }
            break;
        }
        case Kind::Throw:
        case Kind::None:
            break;
        }
        return r;
    }

    /// The pad buttons the filter hides from the game while an item is in a hand (and lockout_s
    /// after): swings, the right grip tap's weapon swap (d-pad right), L1 when the left hand holds.
    uint32_t BlockMask(double now, const sb::UiHeldItems& c) const {
        const bool on = state != State::Idle || now < block_until;
        if (!on) {
            return 0;
        }
        uint32_t m = BBXR_BTN_R1 | BBXR_BTN_R2 | BBXR_BTN_RIGHT;
        if (hand == 0) {
            m |= BBXR_BTN_L1;
        }
        (void)c;
        return m;
    }

    State GetState() const { return state; }
    bool Active() const { return state != State::Idle; }
    int Hand() const { return hand; }
    const ItemInfo& Info() const { return info; }
    uint32_t Id() const { return id; }
    int QuickSlot() const { return quick_slot; }
    m3::Vec3 ItemPos() const { return item_pos; }
    float Progress() const { return progress; }
    bool InContact() const { return contact; }
    bool Gripped() const { return state == State::Held; }
    double BlockUntil() const { return block_until; }
    /// What to do with the item, for the HUD.
    const char* Hint() const {
        if (state == State::Pending) {
            return "SQUEEZE GRIP";
        }
        switch (info.kind) {
        case Kind::Mouth: return "TO THE MOUTH";
        case Kind::Blade: return "TOUCH THE BLADE";
        case Kind::Throw: return "THROW";
        default: return "";
        }
    }

private:
    Result& Fire(const Frame& f, const sb::UiHeldItems& c, const char* how, Result& r) {
        r.kind = Result::Use;
        r.item = info;
        r.hand = hand;
        r.why = how;
        r.pulses.push_back({hand, c.use.amplitude, c.use.seconds});
        Finish(f.time, c);
        return r;
    }

    void Finish(double now, const sb::UiHeldItems& c) {
        state = State::Idle;
        block_until = now + double(c.lockout_s);
        dwell_since = -1.0;
        progress = 0.0f;
        contact = false;
    }

    State state = State::Idle;
    ItemInfo info;
    uint32_t id = BBXR_ITEM_NONE;
    int quick_slot = -1, hand = 1;
    double picked_at = 0.0, held_at = 0.0, dwell_since = -1.0, block_until = -1e9;
    float progress = 0.0f;
    bool contact = false;
    m3::Vec3 item_pos;
    VelocityTracker velocity;
};

/// The weapon hand's pulse for a collision of the game's weapon (BBXR_EV_SWING_HIT): `contact` is
/// the event's b (0 character, 1 wall, 2 object), `paid` its a (the stamina the attack cost, 0
/// when unknown), `strength_ref` the cost that counts as a full-strength hit.
inline Pulse CollisionPulse(const sb::UiCollisionHaptics& c, int hand, int contact, int paid) {
    const sb::UiPulse base = contact == 1 ? c.wall : contact == 2 ? c.object : c.hit;
    float t = 0.0f;
    if (c.scale_by_strength && paid > 0) {
        t = std::clamp(float(paid) / c.strength_ref, 0.0f, 1.0f);
    }
    // A wall or object bounce already starts strong; the blend only lifts it towards hit_strong.
    Pulse p{hand, base.amplitude, base.seconds};
    if (c.scale_by_strength) {
        p.amplitude = base.amplitude + (std::max(c.hit_strong.amplitude, base.amplitude) - base.amplitude) * t;
        p.seconds = base.seconds + (std::max(c.hit_strong.seconds, base.seconds) - base.seconds) * t;
    }
    p.amplitude = std::clamp(p.amplitude, 0.0f, 1.0f);
    p.seconds = std::clamp(p.seconds, 0.0f, 1.0f);
    return p;
}

} // namespace held
} // namespace vrui
